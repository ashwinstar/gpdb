/*-------------------------------------------------------------------------
 * ml_ipc.c
 *     Motion Layer IPC Layer.
 *
 * Copyright (c) 2005, Green Plum
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "miscadmin.h"

#include "cdb/tupchunklist.h"
#include "cdb/ml_ipc.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"

#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "port.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#endif

/*=========================================================================
 * STRUCTS
 */

 /* backlog for listen() call */
#define LISTEN_BACK_LOG 5

#define getInt16(pNetData)    ntohs(*((int16 *)(pNetData)))
#define getInt32(pNetData)    ntohl(*((int32 *)(pNetData)))

/* Structure used for keeping track of a pt-to-pt connection between two
 * Cdb Entities (either QE or QD).
 */
typedef struct MotionConn
{
	/* socket file descriptor. */
 	int sockfd;

 	/* pointer to the data buffer. */
 	uint8 *pBuff;

 	/* size of the message in the buffer, if any. */
 	int32 msgSize;

	/* recv bytes: we can have more than one message/message fragment in recv queue at once */
 	int32 recvBytes;

 	int tupleCount;

 	bool stillActive;

 	char *remoteHost;
 	int  remotePort;
 	int  remoteContentId;
 	
 	bool hasMirrorConn;
 	struct MotionConn *mirrorConn;
 	
} MotionConn;

/* used to organize all of the information for a given motion node.
 *
 */
typedef struct MotionLayerState
{
	int16 		motNodeId;

	int 		numConns;

	MotionConn 	*conns;
	
	int 		numMirrorConns;
	
	MotionConn  *mirrorConns;   /* only used by the sender.  the receiver has no
								 * concept of whether its a mirror or not. so it
								 * always stores its connections in conns.
								 */

	int			myRouteId;

	/* used for receiving. to select() from a set of interesting
	 * MotionConns to see when data is ready to be read.  When the
	 * incoming connections are established, read interest is turned
	 * on.  It is turned off when an EOS (End of Stream) message is
	 * read.
	 */
	fd_set readSet;

	/* highest file descriptor in the readSet. */
	int highReadSock;
} MotionLayerState;



/*DOH: include a routeId and move away from using contentId and
 *	   isPrimary for validation.  use ip address and port instead.
 */
typedef struct RegisterMessage
{
	int16 	motNodeId;
	int16   srcRouteId;
	int16 	srcContentId;
	int32   srcListenerPort;
} RegisterMessage;


/* message used to indicate that the receiver is no longer interested
 * in receiving tuples from this sender.
 */
typedef struct StopSendingMessage
{
	int16   motNodeId;
	int16   srcRouteId;
} StopSendingMessage;

/*=========================================================================
 * GLOBAL STATE VARIABLES
 */

/* Socket file descriptor for the listener. */
int listenerFd;

/* TODO: not sure we need this. could just push it as a local variable
 *       into ListenerSetup()
 */
struct sockaddr_in serv_addr;

/* Hash table mapping motion node Id --> MotionLayerState */
HTAB *hashtab;	

/* TCP port the Interconnect listens on for incoming connections from other
 * remote Interconnect entities.  The listenerPort is assigned at process startup
 * and is set by ListenerSetup().
 */
int listenerPort;	

/* our timeout value for select() and other socket operations. */
struct timeval 		tval;

List *incompleteConns=NIL;

/* keeps track of if we've "activated" connections via SetupInterconnect(). */
bool activated;

/*slice table stuff. */
SliceTable *sliceTable=NULL;
int	sliceId;

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static int listenerSetup( void );
 
static MotionLayerState *createMotionState(int16 motNodeID, int numConns);
static MotionLayerState *getMotionState(int16 motNodeID);
static MotionLayerState *removeMotionState(int16 motNodeID);

static MotionLayerState  *startConnections(Slice *mySlice, int *highsock, fd_set *wset, int *connsStarted);
static void setupOutgoingConnection( MotionConn *conn, CdbProcess *cdbProc,int *highsock, fd_set *wset );
static void sendRegisterMessage(MotionLayerState *pEntry, MotionConn *conn);
static void readRegisterMessage(MotionConn *in_conn, int *incoming_count);
static void acceptIncomingConnection(fd_set *rset, int *highsock);

static bool sendChunk(MotionConn *conn, TupleChunkListItem tcItem);
static void readPacket(MotionConn *conn);
static void doBroadcast(MotionLayerState *pEntry, TupleChunkListItem tcItem);
static int findMyRouteId(Slice *slice);
static CdbProcess *findCdbProcess(RegisterMessage *msg);

bool flushBuffer(MotionConn *conn);
void dumpEntryConnections( MotionLayerState *pEntry );

void waitOnEosAck(MotionLayerState *pEntry);

/*=========================================================================
 * VISIBLE FUNCTIONS
 */

/* See ml_ipc.h */
void InitMotionLayerIPC(void)
{
	HASHCTL hctl;
	
	/* Initialize our hashtable */
	hctl.keysize = sizeof(int16);
	hctl.entrysize = sizeof(MotionLayerState);
	hctl.hash = tag_hash;
	hashtab = hash_create("MotionLayerState", 10, &hctl, HASH_ELEM | HASH_FUNCTION);
	if (hashtab == NULL)
	{
		ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("ml_ipc Could not allocate hash table for MotionState info.")));
	}

	incompleteConns = NIL;

	tval.tv_sec = 0;
	tval.tv_usec = 500000;
	
	activated = false;
	
	elog( DEBUG1, "Setting Up Interconnect Listener Port...");
	Cdb_listener_port = listenerSetup();
}


/* See ml_ipc.h */
void CleanUpMotionLayerIPC(void)
{

#ifdef AMS_VERBOSE_LOGGING	
	elog(DEBUG3, "CleanUpMotionLayerIPC()");
#endif

	hash_destroy(hashtab);
	
	/* close down the Interconnect listener socket. */
	shutdown(listenerFd, SHUT_RDWR);
	close(listenerFd);
	
	/* be safe and reset global state variables. */
	activated 	 = false;
	listenerPort = -1;
	listenerFd   = -1;
	sliceTable   = NULL;
	sliceId 	 = -1;
	
	list_free( incompleteConns );
}


/* See ml_ipc.h */
bool SendTupleChunkToAMS(int16 motNodeID, int16 targetRoute, TupleChunkListItem tcItem)
{
	MotionLayerState	*pEntry;
	MotionConn		    *conn;
	TupleChunkListItem	currItem;

	Assert(activated);
	
	/* check em' */
	CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING	
	elog(DEBUG3, "SendTupleChunkToAMS( motNodeID: %d, targetRoute: %d)", motNodeID, targetRoute);
#endif


	/* tcItem can actually be a chain of tcItems.  we need to send out
	 * all of them. */
	pEntry = getMotionState(motNodeID);	
	
	currItem = tcItem;

#ifdef AMS_VERBOSE_LOGGING
	{
		struct timeval snapTime;

		gettimeofday(&snapTime, NULL);
		elog(DEBUG3, "starting chunklist @%s.%d time is %ld.%d",
			 __FILE__, __LINE__, snapTime.tv_sec, snapTime.tv_usec);
	}
#endif
	
	while (currItem != NULL)
	{
		
		if (targetRoute == BROADCAST_SEGIDX)
		{		
			doBroadcast(pEntry, tcItem);
		}
		else
		{
			/* handle pt-to-pt message. */
			conn = pEntry->conns + targetRoute;
			/* only send to interested connections */
			if (conn->stillActive) {
				sendChunk(conn, tcItem );
			
				if (conn->hasMirrorConn)
					sendChunk(conn->mirrorConn, tcItem);
			}
				
		}
	
		currItem = tcItem->p_next;
	}

#ifdef AMS_VERBOSE_LOGGING
	{
		struct timeval snapTime;

		gettimeofday(&snapTime, NULL);
		elog(DEBUG3, "done with chunklist @%s.%d time is %ld.%d",
			 __FILE__, __LINE__, snapTime.tv_sec, snapTime.tv_usec);
	}
#endif
	
#ifdef AMS_VERBOSE_LOGGING	
	elog(DEBUG3, "SendTupleChunkToAMS() Leaving");
#endif

	return true;
}


/* See ml_ipc.h */
void SendEosToAMS(int16 motNodeID, int16 targetRoute, TupleChunkListItem tcItem)
{
	MotionLayerState 	*pEntry;	
	MotionConn 			*conn;
	int 				i;

	Assert(activated);

	/* check em' */
	CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING	
	elog(DEBUG3, "SendEosToAMS() Entering");
#endif

	pEntry = getMotionState(motNodeID);

	/* we want to add our tcItem onto each of the outgoing buffers */
	doBroadcast(pEntry, tcItem);

	/* now flush all of the buffers.*/	
	for (i=0; i < pEntry->numConns; i++)
	{		
		conn = pEntry->conns + i;
		flushBuffer(conn);
		
		if ( conn->hasMirrorConn )
			flushBuffer( conn->mirrorConn );
	}

	waitOnEosAck(pEntry);

#ifdef AMS_VERBOSE_LOGGING	
	elog(DEBUG3, "SendEosToAMS() Leaving");
#endif
}

/* See ml_ipc.h */
TupleChunkListItem RecvTupleChunkFromAny(int16 motNodeID, int16 *srcRoute)
{
	static int			scanStart = 0;
	MotionLayerState	*pEntry;
	MotionConn			*conn;
	TupleChunkListItem	tcItem;
	fd_set				rset;
	int					n, i, index;
	bool				skipSelect = false;

	Assert(activated);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "RecvTupleChunkFromAny(motNodeId=%d)", motNodeID);
#endif

	pEntry = getMotionState(motNodeID);
	do
	{
        struct timeval timeout = tval;
		
		/* make sure we check for these. */
		CHECK_FOR_INTERRUPTS();

		memcpy(&rset, &(pEntry->readSet), sizeof(fd_set));

		/* since we may have data in a local buffer, we may be able to
		 * short-circuit the select() call (and if we don't do this we
		 * may wait when we have data ready, since it has already been
		 * read) */
		for (i=0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (FD_ISSET(conn->sockfd, &rset) && conn->recvBytes != 0) {
				/* we have data on this socket, let's short-circuit our select */
				FD_ZERO(&rset);
				FD_SET(conn->sockfd, &rset);

				skipSelect = true;
			}
		}
		if (skipSelect)
			break;

		n = select(pEntry->highReadSock + 1, &rset, NULL, NULL, &timeout);
        if (n < 0)
		{
            if (errno == EINTR)
                continue;
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Interconnect error receiving an incoming packet: %s", strerror(errno)),
							errdetail("error during select() call (error:%d).\n", errno)));
		}
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "RecvTupleChunkFromAny() select() returned %d ready sockets", n);
#endif
	} while (n < 1);

	/* We scan the file descriptors starting from where we left off in
	 * the last call (don't continually poll the first when others may
	 * be ready!).
	 * 
	 * The calculation of scanEnd looks weird. we're calculating
	 * (scanStart - 1)modN, but C doesn't like mixing the "mod"
	 * operator with negative numbers (-1 mod N != N -  )
	 */
	for (i = 0; i < pEntry->numConns; i++)
	{	
		index = ((scanStart + i) % pEntry->numConns);
		conn = pEntry->conns + index;

		if (!conn->stillActive)
		{
			elog(LOG, "RecvTupleChunkFromAny: trying to read on inactive socket %d", conn->sockfd);
		}
			
		if (FD_ISSET(conn->sockfd, &rset))
		{		
			elog(DEBUG5, "RecvTupleChunkFromAny() (fd %d) %d/%d", conn->sockfd, motNodeID, index);
			tcItem = RecvTupleChunkFrom(motNodeID, index);

			*srcRoute = index;

			/* advance start point */
			scanStart = (index + 1) % pEntry->numConns;

			return tcItem;
		}
	}

	/* we should never ever get here... */
	elog(ERROR, "for some reason RecvTupleFromAny didn't return anything.");
	return NULL;
}

/* See ml_ipc.h */
TupleChunkListItem RecvTupleChunkFrom(int16 motNodeID, int16 srcRoute)
{
	MotionLayerState	*pEntry;
	MotionConn			*conn;
	TupleChunkListItem	tcItem;
	TupleChunkListItem	firstTcItem = NULL;
	TupleChunkListItem	lastTcItem = NULL;
	uint16				tcSize;
	int					bytesProcessed = 0;

	Assert(activated);

	/* check em' */
	CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "RecvTupleChunkFrom(motNodID=%d, srcRoute=%d)", motNodeID, srcRoute);
#endif

	pEntry = getMotionState(motNodeID);
	conn = pEntry->conns + srcRoute;
	
	/* read the packet in from the network. */
	readPacket(conn);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "read packet recv bytes %d msgsize %d", conn->recvBytes, conn->msgSize);
#endif

	/* go through and form us some TupleChunks. */
	bytesProcessed = PACKET_HEADER_SIZE;
	
	while (bytesProcessed != conn->msgSize )
	{
		tcSize = TUPLE_CHUNK_HEADER_SIZE + GetChunkDataSize(conn->pBuff + bytesProcessed);
		
		if (tcSize >= conn->msgSize) {
			elog(ERROR, "tcSize %d >= conn->msgSize %d !", tcSize, conn->msgSize);
		}
		Assert(tcSize < conn->msgSize);
		
		tcItem = (TupleChunkListItem)palloc0(sizeof(TupleChunkListData) + tcSize);
		tcItem->chunk_length = tcSize;
		
		memcpy(tcItem->chunk_data, conn->pBuff + bytesProcessed, tcSize);
		
		bytesProcessed += tcSize;
		
		if (firstTcItem == NULL)
		{
			firstTcItem = tcItem;
			lastTcItem = tcItem;
		}
		else
		{
			lastTcItem->p_next = tcItem;
			lastTcItem = tcItem;
		}
	}

	conn->recvBytes -= conn->msgSize;
	if (conn->recvBytes != 0)
	{
		/* partial message waiting in recv buffer! Move to head of buffer */
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG3, "residual message %d bytes", conn->recvBytes);
#endif

		memcpy(conn->pBuff, conn->pBuff + conn->msgSize, conn->recvBytes);
	}
	
	conn->msgSize = 0;

	return firstTcItem;
}


/* See ml_ipc.h */
void DeregisterReadInterest(int16 motNodeID, int16 srcRoute)
{
	MotionLayerState 	*pEntry;
	MotionConn 		 	*conn;
		
	Assert(activated);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "DeregisterReadInterest()");
#endif

	pEntry = getMotionState(motNodeID);
	conn = pEntry->conns + srcRoute;
	
	FD_CLR(conn->sockfd, &(pEntry->readSet));
}



/* See ml_ipc.h */
void SetupInterconnect(SliceTable *mySliceTable, int mySliceId)
{
	int					i, n, highsock = 0;
	fd_set 				rset,rrset;
	fd_set				wset,wwset;
	ListCell 			*cell;
	Slice				*mySlice;
	Slice				*aSlice;
	MotionLayerState 	*pEntry;

	struct timeval startTime;
	struct timeval snapTime;
	struct timeval stopTime;
	
	int incoming_count 		  = 0;
	int outgoing_count 		  = 0;
	int expectedTotalIncoming = 0;
	int expectedTotalOutgoing = 0;
	
	/* we can have at most one of these. */
	MotionLayerState *sendingMotionState = NULL;

	Assert(!activated);

	gettimeofday(&snapTime, NULL);
	elog(DEBUG3, "in SetupInterconnect @%d time is %ld.%ld",
		 __LINE__, (long int)snapTime.tv_sec, (long int)snapTime.tv_usec);
	
	sliceTable = mySliceTable;
	sliceId = mySliceId;
	
	FD_ZERO(&rset);
	FD_ZERO(&wset);
	FD_SET(listenerFd, &rset);
	highsock = listenerFd;
	
	elog(DEBUG3, "Interconnect Activating Routes");
	
	if (mySliceTable == NULL)
	{
		elog(ERROR, "SetupInterconnect() called with a NULL sliceTable.");
	}

	mySlice = (Slice *)list_nth(sliceTable->slices, mySliceId);

	if (mySlice->parentIndex != -1)
	{
		elog(DEBUG5, "--Setting up SENDING motion node: %d...starting Connections",
			  mySlice->sliceIndex);
		
		/* can only have at most one sender. */
		Assert(sendingMotionState == NULL);
		
		/* MotionEntry created inside of startConnections. */
		pEntry = startConnections(mySlice, &highsock, &wset, &expectedTotalOutgoing);

		pEntry->myRouteId = findMyRouteId(list_nth(sliceTable->slices, mySlice->sliceIndex));
			
		sendingMotionState    = pEntry;
	}

	/* now we'll do some setup for each of our Receiving Motion Nodes. */
	foreach(cell, mySlice->children)
	{
		int numProcs;
		int childId = lfirst_int(cell);
	
		elog(DEBUG5, "--Setting up RECEIVING motion node: %d", childId);
		
		aSlice = (Slice *)list_nth(sliceTable->slices, childId);
		numProcs = list_length(aSlice->primaryProcesses);
		
		pEntry = createMotionState(childId, numProcs);
		
		/* let cdbmotion now how many receivers to expect. */
		setExpectedReceivers( childId, numProcs );
		
		pEntry->myRouteId = 0; // DOH: do i need this for receivers?
		
		expectedTotalIncoming += pEntry->numConns;
	}

	gettimeofday(&startTime, NULL);

	/* now we have kicked off all of the sends.  so we will just sit
	 * back and do some select()'s and process any outgoing connection
	 * or incoming connection as they complete.
	 */
	do
	{
        struct timeval timeout = tval;

        /* since we might be here for a bit, we should prolly check
		 * for any interrupts that come in.
		 */
		CHECK_FOR_INTERRUPTS();

		/* Have we been here too long ? */
		gettimeofday(&stopTime, NULL);

		if (interconnect_setup_timeout != 0 &&
			(stopTime.tv_sec - startTime.tv_sec) > interconnect_setup_timeout)
			elog(ERROR, "Aborting activating_routes.... TIMEOUT (%lu)",
				 stopTime.tv_sec - startTime.tv_sec);
		
		memcpy(&rrset, &rset, sizeof(fd_set));
		memcpy(&wwset, &wset, sizeof(fd_set));

		n = select(highsock + 1, &rrset, &wwset, NULL, &timeout);
        if (n < 0)
		{
			if (errno == EINTR)
                continue;
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							 errmsg("Interconnect error setting up: %s", strerror(errno)),
							 errdetail("error during select() call (error:%d).",errno)));
		}

		elog(DEBUG3, "select() returned %d ready sockets", n);

		/* is it someone tickling our listener port? */
		if (FD_ISSET(listenerFd, &rrset))
		{
			elog(DEBUG5, "accepting an Incoming Connection ");
			acceptIncomingConnection(&rset, &highsock);
			n--;
		}
		if (!n)
			continue;

		/* check our outgoing connections. */
		if (outgoing_count < expectedTotalOutgoing)
		{
			for (i=0; i < sendingMotionState->numConns; i++)
			{
				MotionConn *conn = sendingMotionState->conns + i;

				if (FD_ISSET(conn->sockfd, &wwset))
				{			
					elog(DEBUG5, "Sending a register message to sockfd: %d", conn->sockfd);
					sendRegisterMessage(sendingMotionState, conn);
					
					/*no longer interested in this for now.*/
					FD_CLR(conn->sockfd, &wset);
					
					outgoing_count++;
					if (--n == 0)
						break; /* leave for loop */
				}
				
				if( conn->hasMirrorConn )
				{
					if(FD_ISSET(conn->mirrorConn->sockfd, &wwset))
					{
						elog(DEBUG5, "Sending a register message to sockfd: %d", conn->mirrorConn->sockfd);
						sendRegisterMessage(sendingMotionState, conn->mirrorConn);
						
						/* no longer interested in this for now. */
						FD_CLR(conn->mirrorConn->sockfd, &wset);
						
						outgoing_count++;
						if (--n == 0)
							break; /* leave for loop */
					}
				}
			}
		}
		if (!n)
			continue;

		/* check our connections that are accepted'd but no register message.
		 * we don't know which motion node these apply to until we actually
		 * receive the REGISTER message.  this is why they are all in a single
		 * list.
		 *
		 * NOTE: we don't use foreach() here because we want to trim from 
		 * the list as we go.
		 */
		cell = list_head(incompleteConns); 
		while( cell != NULL )
		{
			MotionConn *conn = (MotionConn*)lfirst(cell);

			/* we'll get the next cell ready now in case we need to delete the
			 * cell that corresponds to our MotionConn 
			 */
			cell = lnext(cell);			
			
			if (FD_ISSET(conn->sockfd, &rrset))
			{
				elog(DEBUG5, "conn # %d has data!", conn->sockfd);
				
				readRegisterMessage(conn, &incoming_count);				
				FD_CLR(conn->sockfd, &rset);	
				incompleteConns = list_delete_ptr(incompleteConns, conn);	
			}
		}
		
		elog(DEBUG3, "%d of %d outgoing conns and %d of %d incoming conns completed",
			  outgoing_count, expectedTotalOutgoing, incoming_count, expectedTotalIncoming);
	}
	while ((outgoing_count != expectedTotalOutgoing) || (incoming_count != expectedTotalIncoming));


	/* if everything really got setup properly then we shouldn't have any incomplete connections. */
	Assert( list_length(incompleteConns) == 0 );

	
	activated = true;
	elog(DEBUG3, "FINISHED ACTIVATING ROUTES. (out: %d , in: %d)", outgoing_count, incoming_count);

	gettimeofday(&snapTime, NULL);
	elog(DEBUG3, "exiting SetupInterconnect @%d time is %ld.%ld",
		 __LINE__, (long int)snapTime.tv_sec, (long int)snapTime.tv_usec);
}


/* TeardownInterconnect() function is used to cleanup interconnect resources that
 * were allocated during SetupInterconnect().  This function should ALWAYS be
 * called after SetupInterconnect to avoid leaking resources (like sockets)
 * even if SetupInterconnect did not complete correctly.  As a result, this 
 * function must complete successfully even if SetupInterconnect didn't.
 * 
 * SetupInterconnect() always gets called under the ExecutorState MemoryContext.
 * This context is destroyed at the end of the query and all memory that gets 
 * allocated under it is free'd.  We don't have have to worry about pfree() but
 * we definitely have to worry about socket resources.
 * 
 */
void TeardownInterconnect(void)
{
	ListCell 		    *cell;
	MotionLayerState   	*pEntry;
	int					i;
	Slice 				*mySlice;
	MotionConn			*conn;	

	elog(DEBUG3, "Interconnect cleanup.");
	Assert( sliceTable != NULL );
	
	mySlice = (Slice*)list_nth(sliceTable->slices, sliceId);

	if (mySlice->parentIndex != -1)
	{
		/* cleanup a Sending motion node. */
		elog(DEBUG3, "close down incoming connections for motion id: %d",
				mySlice->sliceIndex);
		pEntry = removeMotionState(mySlice->sliceIndex);

		for (i=0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;
			
			if (conn == NULL)
				continue;
			
			if (conn->sockfd != -1)
			{
				shutdown(conn->sockfd, SHUT_RDWR);
				close(conn->sockfd);
				conn->sockfd = -1;
			}
			
			if (conn->hasMirrorConn)
			{
				if (conn->mirrorConn->sockfd != -1)
				{
					shutdown(conn->mirrorConn->sockfd, SHUT_RDWR);
					close(conn->mirrorConn->sockfd);
					conn->mirrorConn->sockfd = -1;
				}
			}
		}
	}

	/* cleanup all of our Receiving Motion nodes. */
	foreach(cell, mySlice->children)
	{
		Slice *aSlice;
		int childId = lfirst_int(cell);

		aSlice = (Slice *)list_nth(sliceTable->slices, childId);

		pEntry = removeMotionState(aSlice->sliceIndex);
		elog(DEBUG3, "close down connections for motion Id: %d", pEntry->motNodeId);

		/* receivers know that they no longer care about data from
		 * below ... so we can safely discard data queued in both
		 * directions */
		for (i=0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;
			
			if (conn == NULL)
				continue;
				
			if (conn->sockfd != -1)
			{
				shutdown(conn->sockfd, SHUT_RDWR);
				close(conn->sockfd);
				conn->sockfd = -1;
			}
		}
	}

	/* The incompleteConns list is only used as a staging area for MotionConns 
	 * during by SetupInterconnect().  So we only expect to have entries here
	 * if SetupInterconnect() did not finish correctly.
	 *
	 * NOTE: we don't use foreach() here because we want to trim from 
     * the list as we go.
     */
	while ((cell = list_head(incompleteConns)) != NULL)
	{
		MotionConn *conn = (MotionConn*)lfirst(cell);
	
#ifdef AMS_VERBOSE_LOGGING
		elog(LOG, "Found incomplete conn. length %d", list_length(incompleteConns));
#endif

		if (conn->sockfd != -1)
		{
			shutdown(conn->sockfd, SHUT_RDWR);
			close(conn->sockfd);
			conn->sockfd = -1;
		}
	
		incompleteConns = list_delete(incompleteConns, cell);
		cell = list_head(incompleteConns);
	}

	incompleteConns = NIL;

	activated = false;
	sliceTable = NULL;
}


/*=========================================================================
 * HELPER FUNCTIONS
 */

/* listenerSetup
 *
 * Returns a system assigned TCP port that will be used to "listen" for
 * incoming connections from other postgres ml_ipc layers LATER during
 * query initialization.  
 *
 * This port is used for the duration of this process and should never change.
 *
 * RETURNS
 *    System picked TCP port that listener is using.
 */
int listenerSetup(void)
{
	struct 		sockaddr_in addr;
	socklen_t 	addrlen;
	
	if ((listenerFd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		/* TODO: would be nice to read the errno and try and provide
		 * 		 more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not setup listener socket: %s", strerror(errno)),
						errdetail("error during socket() call (error:%d).", errno)));	
	}

	memset(&addr, 0, sizeof(addr));
	serv_addr.sin_family		= AF_INET;
	serv_addr.sin_addr.s_addr	= INADDR_ANY;
	serv_addr.sin_port			= htons(0);  /* system picks port */
	
	/* we let the system pick the TCP port here so we don't have to 
 	 * manage port resources ourselves.  
 	 */
	if (bind(listenerFd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
	{
		/* TODO: would be nice to read the errno and try and provide more
		 * 		 useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not setup listener socket"),
						errdetail("error during bind() call (error:%d).", errno)));
			
	}

	if (listen(listenerFd, LISTEN_BACK_LOG) < 0)
	{
		/* TODO: would be nice to read the errno and try and provide
		 * 		 more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not setup listener socket: %s", strerror(errno)),
						errdetail("error during listen() call (error:%d).", errno)));
	}

	
	addrlen = sizeof(struct sockaddr_in);

	if (getsockname(listenerFd, (struct sockaddr *)&addr,  &addrlen) < 0)
	{
		/* TODO: would be nice to read the errno and try and provide
		 * 		 more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not setup listener socket: %s", strerror(errno)),
						errdetail("error during getsockname() call (error:%d).", errno)));	
	}
		
	
	/* display which port was chosen by the system. */
	listenerPort = ntohs(addr.sin_port);
		
	elog(DEBUG1, "Interconnect listener on port: %d", listenerPort);
	
	return listenerPort;
}

/* Function createMotionState() is used to create a MotionLayerState struct and
 * place it in the hashtab hashtable based on the motNodeID.
 *
 * PARAMETERS
 *
 *   motNodeID - motion node ID for this MotionState.
 *
 *	 numConns  - number of connections for this motion node. These could be all
 *				 incoming or outgoing connections depending on if this is a
 *				 Sending or Receiving motion node (motNodeID).
 *
 * RETURNS
 *   An empty and initialized MotionState struct for the given motion node. If
 *   a MotionState struct is already registered for the motNodeID an ERROR is
 *	 thrown.
 */
MotionLayerState *createMotionState(int16 motNodeID, int numConns)
{
	MotionLayerState 	*pEntry;
	bool 				found;
	int					i;

	pEntry = (MotionLayerState *)hash_search(hashtab, &motNodeID, HASH_ENTER, &found);

	if (pEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
            errmsg("Interconnect Error:  Could not allocate HTAB entry for motion node %d.",
            motNodeID)));
    }

    if (found)
    {
        /* An entry for this motion node ID already existed.  Initializing
         * twice for a given motion node is not allowed.  Having two nodes
         * with the same ID is also not allowed.  So, this is pretty likely a
         * serious problem.
         */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconect Error: A HTAB entry for motion node %d already exists.", motNodeID),
						errdetail("conns %p numConns %d first sock %d highreadsock %d", pEntry->conns, pEntry->numConns, pEntry->conns[0].sockfd, pEntry->highReadSock)));
    }

    pEntry->motNodeId 	   = motNodeID;
    pEntry->numConns 	   = numConns;
	pEntry->highReadSock   = 0;
    pEntry->conns 		   = palloc0(pEntry->numConns * sizeof(MotionConn));
	
	for (i=0; i < numConns; i++)
	{
		pEntry->conns[i].sockfd = -1;
	}

	FD_ZERO(&(pEntry->readSet));

	return pEntry;
}


/* Function getMotionState() is used to retrieve a MotionLayerState from the
 * hashtab hashtable based on the given motNodeID.  This function should only
 * be called after createMotionState() for a given motion node has been called
 * first.
 *
 * PARAMETERS
 *
 *   motNodeID - motion node ID to lookup the MotionLayerState.
 *
 * RETURNS
 *   The MotionLayerState struct for the given motNodeID.	
 */
MotionLayerState *getMotionState(int16 motNodeID)
{
	MotionLayerState 	*pEntry;
	bool 				found;
	
	pEntry = (MotionLayerState *)hash_search(hashtab, &motNodeID, HASH_ENTER, &found);
	
	if (pEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
            errmsg("Interconect Error: Unexpected Motion Node Id: %d.  This means"
                   " a motion node that wasn't setup is requesting interconnect"
                   " resources.", motNodeID)));
    }

	if (!found)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
               errmsg("Interconect Error: Unexpected Motion Node Id: %d.  This means"
                   " a motion node that wasn't setup is requesting interconnect"
                   " resources.", motNodeID)));
	}

	return pEntry;
}


/* Function removeMotionState() is used to remove a MotionLayerState struct from
 * the hashtab hashtable.
 *
 * This should only be called after createMotionState().
 *
 * PARAMETERS
 *
 *   motNodeID - motion node ID to lookup the MotionLayerState.
 *
 * RETURNS
 *   The MotionLayerState that was removed from the hashtab hashtable.
 */
MotionLayerState *removeMotionState(int16 motNodeID)
{
	MotionLayerState 	*pEntry;
	bool 				found;
	
	pEntry = (MotionLayerState *)hash_search(hashtab, &motNodeID, HASH_REMOVE, &found);

	return pEntry;
}


/* Function startConnections() is used to initially kick-off any outgoing
 * connections for mySlice.
 *
 * This should not be called for root slices (i.e. QD ones) since they don't
 * ever have outgoing connections.
 *
 * PARAMETERS
 *	
 *   mySlice  - Slice that this process is a member of.
 *
 *   highsock - Current highest valued socket filedescriptor (used for select())
 *
 *   wset     - fd_set for writes (used for select())
 *
 * RETURNS
 *   Initialized MotionLayerState for the Sending Motion Node Id.	
 */
MotionLayerState  *startConnections(Slice *mySlice, int *highsock, fd_set *wset, int *connsStarted)
{
	MotionLayerState	*pEntry;
	MotionConn			*conn;
	Slice				*recvSlice;
	int					i, motNodeId;
	CdbProcess			*cdbProc;
	

	/* for sending motion nodes... motionNodeId = sliceIndex. */
	motNodeId = mySlice->sliceIndex;

#ifdef AMS_VERBOSE_LOGGING		
	elog(DEBUG3, "Interconnect - Starting Outgoing Connections");
#endif	
	
	recvSlice = (Slice *)list_nth(sliceTable->slices, mySlice->parentIndex);
	
	pEntry = createMotionState(motNodeId, list_length(recvSlice->primaryProcesses));
	
	/* Setup an outgoing socket to the right place for each of our outbound
	 * connections for the primary Connections.
	 */
	for (i=0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;
		conn->sockfd = -1;
	
		cdbProc = (CdbProcess *)list_nth(recvSlice->primaryProcesses, i);
		setupOutgoingConnection( conn, cdbProc, highsock, wset );	
		(*connsStarted)++;
		
		/* setup mirror connection if not null */
		if( recvSlice->mirrorProcesses != NIL )
		{
			cdbProc = (CdbProcess *)list_nth(recvSlice->mirrorProcesses, i);
			if( cdbProc != NULL )
			{
				conn->mirrorConn = palloc(sizeof(MotionConn));
				conn->mirrorConn->sockfd = -1;
				conn->hasMirrorConn = true;
			
				setupOutgoingConnection(conn->mirrorConn, cdbProc, highsock, wset);
				(*connsStarted)++;
			}
		}
	}
	
	return pEntry;
}

void setupOutgoingConnection( MotionConn *conn, CdbProcess *cdbProc, int *highsock, fd_set *wset )
{
	struct sockaddr_in	addr;
	int					n;
	
	conn->sockfd			= socket(AF_INET, SOCK_STREAM, 0);
	conn->pBuff				= palloc(Cdb_max_packet_size);
	conn->msgSize			= PACKET_HEADER_SIZE;
	conn->tupleCount		= 0;
	conn->stillActive		= true;
	conn->remoteHost		= cdbProc->listenerAddr;
	conn->remotePort		= cdbProc->listenerPort;
	conn->remoteContentId	= cdbProc->contentid;
	conn->hasMirrorConn		= false;

	if (conn->sockfd < 0)
	{
		/* TODO: would be nice to read the errno and try and provide more
		 * 		 useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not create outbound socket: %s", strerror(errno)),
						errdetail("error during socket() call (error:%d) to outgoing"
								  "CdbProcess with contentId=%d at: '%s':%d", errno,
								  cdbProc->contentid, cdbProc->listenerAddr,
								  cdbProc->listenerPort)));
	}

	/* make socket non-blocking BEFORE we connect. */
	if (!pg_set_noblock(conn->sockfd))
	{
		/* TODO: would be nice to read the errno and try and
		 * 		 provide more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not set outbound socket to non-blocking mode"),
						errdetail("error during fcntl() call (error:%d) for sockfd: %d",
								  errno, conn->sockfd)));
	}
	
	addr.sin_family = AF_INET;
	
	if ((n = inet_pton(AF_INET, cdbProc->listenerAddr, &addr.sin_addr)) < 1)
	{
		if (n == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Interconnect Error: Could not parse remote listener"
								   "address: '%s'", cdbProc->listenerAddr),
							errdetail("inet_pton() unable to parse address: '%s'",
									  cdbProc->listenerAddr)));
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Interconnect Error: System error occured during parsing"
								   " of remote listener address: '%s'", cdbProc->listenerAddr),
							errdetail("inet_pton()  (errno: %d) unable to parse address: '%s'",
									  errno, cdbProc->listenerAddr)));
		}
	}

	addr.sin_port = htons(cdbProc->listenerPort);

	/* Since our socket is set to non-blocking, then connect can return
	 * immediately with an error of EINPROGRESS but the TCP connection is
	 * still continuing to be made.  we will later use select() to find out
	 * when its actually done.
	 */
	if ((n = connect(conn->sockfd, (struct sockaddr *)&addr, sizeof(addr))) < 0)
	{
		if (errno != EINPROGRESS)
		{
			/* TODO: would be nice to read the errno and try and provide more
			 * 		 useful info as to why this happened.
			 */
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Interconnect Error: Could not connect() to remote destination: %s", strerror(errno)),
							errdetail("error during connect() call (error:%d) to outgoing"
									  "CdbProcess with contentId=%d at: '%s':%d", errno,
									  cdbProc->contentid, cdbProc->listenerAddr,
									  cdbProc->listenerPort)));	
		}
	}

	FD_SET(conn->sockfd, wset);

	if (conn->sockfd > *highsock)
		*highsock = conn->sockfd;

	elog(DEBUG5, "Interconnect started outgoing connection to CdbProcess with contentId=%d "
		 "to: '%s':%d with sockfd: %d", cdbProc->contentid, cdbProc->listenerAddr,
		 cdbProc->listenerPort, conn->sockfd);
}


/* Function sendRegisterMessage() used to send a Register message to the
 * remote destination on the other end of the provided conn.
 *
 * PARAMETERS
 *
 *   pEntry - MotionLayerState.
 *   conn   - MotionConn to send message out on.
 */
void sendRegisterMessage(MotionLayerState *pEntry, MotionConn *conn)
{
	int   	n;
	int16 	netval;	
	int32   netval2;
	RegisterMessage regMsg;
	int 	msglen = sizeof(RegisterMessage);
	uint8 	*msg = (uint8 *)&regMsg;

	/* motNodeId */
	netval = htons(pEntry->motNodeId);
	memcpy(msg + 0, &netval, 2);

	/* srcRouteId */
	netval = htons(pEntry->myRouteId);
	memcpy(msg + 2, &netval, 2);

	/* srcContentId */
	netval = htons(Cdb_segment);
	memcpy(msg + 4, &netval, 2);

	/* srcListenerPort */
	netval2 = htonl(listenerPort);
	memcpy(msg + 6, &netval2, 4);

	/* Since our message is very small and we should have at least some
	 * dedicated TCP buffer space.  so we should be able to use a blocking
	 * send() call to write it out.
	 */	
	while ((n = write(conn->sockfd, msg, msglen)) < 0)
        if (errno != EINTR)
            break;

    if (n < 0)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not write() Register message to remote destination: %s", strerror(errno)),
						errdetail("error during write() call (error:%d) to remote destination"
								  "'%s':%d with contentid = %d", errno, conn->remoteHost,
								  conn->remotePort, conn->remoteContentId)));
	}

	if (n != msglen)
	{
		elog(DEBUG3, "n = %d", n);
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not write() complete Register"
							   "message to remote destination"),
						errdetail("sent: %d of %d bytes during write() call to remote destination"
								  "'%s':%d with contentid = %d", n, msglen, conn->remoteHost,
								  conn->remotePort, conn->remoteContentId)));
	}
}


	
/* Function readRegisterMessage() reads a "Register" message off of the conn
 * and places it in the right MotionLayerEntry conn slot based on the contents
 * of the register message.
 *
 * PARAMETERS
 *
 *   conn - MotionConn to read the register messagefrom.
 *	 incoming_count - ptr to number of incoming connections that have been set
 *					  up so far.
 *
 */
void readRegisterMessage(MotionConn *conn, int *incoming_count)
{
	MotionLayerState 	*pEntry;	
	MotionConn			*newConn;
	RegisterMessage 	msg;
	int 				n;
	int 				bytesRead = 0;	
	CdbProcess			*proc;

	int msgSize = sizeof(RegisterMessage);
	RegisterMessage		regMsg;
	uint8 *buff = (uint8 *)&regMsg;

	/* loop till we read in the entire RegisterMessage. */
	while (bytesRead < msgSize)
	{
		while ((n = read(conn->sockfd, buff + bytesRead, msgSize - bytesRead)) < 0)
            if (errno != EINTR)
                break;

        if (n < 0)
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Interconnect Error: Could not read() Register message "
								   "from remote destination"),
							errdetail("error during read() call (error:%d) from remote"
									  "sockfd = %d", errno, conn->sockfd)));
		}
	
		bytesRead += n;		
		Assert(bytesRead <= (int)sizeof(RegisterMessage));
	}
	
	msg.motNodeId 		= getInt16(buff + 0);
	msg.srcRouteId 		= getInt16(buff + 2);
	msg.srcContentId 	= getInt16(buff + 4);
	msg.srcListenerPort = getInt32(buff + 6);

	elog(DEBUG5, "Read a Register Message: (motNodeID: %d, srcRouteId: %d, "
	      "srcContentId: %d, srcListenerPort: %d\n",
	      msg.motNodeId,msg.srcRouteId,msg.srcContentId,msg.srcListenerPort);

	pEntry = getMotionState(msg.motNodeId);
	
	/* store the MotionConn in the right slot, make sure no one is there. */
	if ((pEntry->conns + msg.srcRouteId)->sockfd != -1)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Already a conn for (mnId=%d,routeId=%d,%d)",
							   msg.motNodeId, msg.srcRouteId, (pEntry->conns + msg.srcRouteId)->sockfd),
						errdetail("Should not have a non-NULL connection in that slot.")));
	}
	
	
	/* find and validate our RegisterMessage based on the CdbProcess. */
	proc = findCdbProcess(&msg);
	
	newConn = pEntry->conns + msg.srcRouteId;	
	
	newConn->sockfd		   	 = conn->sockfd;
	newConn->pBuff	     	 = palloc(Cdb_max_packet_size);
	newConn->msgSize 		 = 0;
	newConn->recvBytes 		 = 0;
	newConn->tupleCount 	 = 0;
	newConn->stillActive 	 = true;
	newConn->remoteHost 	 = proc->listenerAddr;
	newConn->remotePort 	 = msg.srcListenerPort;
	newConn->remoteContentId = msg.srcContentId;
	newConn->hasMirrorConn   = false;
	
	FD_SET(newConn->sockfd, &(pEntry->readSet));
	
	if (newConn->sockfd > pEntry->highReadSock)
		pEntry->highReadSock = newConn->sockfd;
	
#ifdef AMS_VERBOSE_LOGGING	
	dumpEntryConnections( pEntry );
#endif

	(*incoming_count)++;
}

/* Function acceptIncomingConnection() is used to accept() and setup an
 * incoming connection on the Interconnect Listener socket.
 *
 * This function should not be called unless we know we have a connection
 * waiting (by using select() or something).  We add the MotionConn created
 * for the socket to the incompleteConns list since we don't know anything about
 * the connection until we receive the "Register" message.
 *
 * PARAMETERS:
 *
 *  rset - readset so we can register readinterest in the new socket once we
 *		   accept() it.
 *
 *  highsock - ptr to highest socket filedescriptor. (needed for select())
 *
 */
void acceptIncomingConnection(fd_set *rset, int *highsock)
{
	int 		newsockfd;
	struct 		sockaddr_in addr;
	socklen_t 	addrlen;
	MotionConn 	*conn;
	
	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	if ((newsockfd = accept(listenerFd, (struct sockaddr *)&addr, &addrlen)) < 0)
	{
		/* TODO: would be nice to read the errno and try and provide more
		 * 		 useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						 errmsg("Interconnect error accepting a new connection"),
						 errdetail("error during accept() call (error:%d)", errno)));
	}


	//make socket non-blocking BEFORE we connect.
	if (!pg_set_noblock(newsockfd))
	{
		/* TODO: would be nice to read the errno and try and provide more
		 * 		 useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not set outbound socket to non-blocking mode"),
						errdetail("error during fcntl() call (error:%d) for sockfd: %d",
								  errno, newsockfd)));
	}
		

	FD_SET(newsockfd, rset);
	
	if (newsockfd > *highsock)
		*highsock = newsockfd;
	
	conn = palloc(sizeof(MotionConn));	
	conn->sockfd     = newsockfd;
	conn->pBuff      = palloc(Cdb_max_packet_size);
	conn->msgSize    = 0;
	conn->recvBytes  = 0;
	conn->tupleCount = 0;
	conn->stillActive = true;
	conn->remoteHost = "unknown";
	conn->remotePort = -1;
	conn->remoteContentId = -2;
	
	if (incompleteConns == NIL)	
		incompleteConns = list_make1(conn);
	else
		incompleteConns = lappend(incompleteConns, conn);		
}

/* The Function sendChunk() is used to send a tcItem to a single
 * destination. Tuples often are *very small* we aggregate in our
 * local buffer before sending into the kernel.
 *
 * PARAMETERS
 *   conn - MotionConn that the tcItem is to be sent to.
 *   tcItem - message to be sent.
 */
bool sendChunk(MotionConn *conn, TupleChunkListItem tcItem)
{
	Assert(conn->msgSize > 0);		
	
	if (conn->msgSize + tcItem->chunk_length > Cdb_max_packet_size)
	{
		if (!flushBuffer(conn))
			return false;
	}
	
	memcpy(conn->pBuff + conn->msgSize, tcItem->chunk_data, tcItem->chunk_length);
	conn->msgSize += tcItem->chunk_length;
	
	conn->tupleCount++;	
	return true;
}

/* Function flushBuffer() is used to write the contents of a MotionConn buffer
 * out to the network.  This function blocks until the entire contents have
 * been written to the socket.
 *
 * We also have to check for reads here.  Having data to read on a "sending"
 * socket means the receiver is sending us a stopSending Message.  Below, we
 * only check during select() and don't have to worry about write().  The reason
 * is that if the Receiver sends us a stopSending message it will also stop
 * reading data from the socket.  This guarantees that eventually our write()
 * call will get an EWOULDBLOCK and our select() call will pick it up.
 *
 *
 * PARAMETERS
 *
 *   conn - MotionConn whose buffer needs to be flushed.
 *
 */
bool flushBuffer(MotionConn *conn)
{
	char		*sendptr;
	int16		header;
	int			n, sent = 0;
	fd_set		wset;
	fd_set		rset;

#ifdef AMS_VERBOSE_LOGGING
	{
		struct timeval snapTime;

		gettimeofday(&snapTime, NULL);
		elog(DEBUG3, "----sending chunk @%s.%d time is %ld.%d",
			 __FILE__, __LINE__, snapTime.tv_sec, snapTime.tv_usec);
	}
#endif

	/* first set header length */
	header = htons(conn->msgSize);
	memcpy(conn->pBuff, &header, sizeof(header));

	/* now send message */
	sendptr = (char *)conn->pBuff;
	sent = 0;
	do {
		struct timeval timeout;

		/* check for stop message before sending anything  */
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		FD_ZERO(&rset);
		FD_SET(conn->sockfd, &rset);
		n = select(conn->sockfd + 1, &rset, NULL, NULL, &timeout);
		/* handle errors at the write call, below */
		if (n > 0  && FD_ISSET(conn->sockfd, &rset))
		{
			/* got a stop message */
			conn->stillActive = false;
			return false;
		}

		if ((n = write(conn->sockfd, sendptr + sent, conn->msgSize - sent)) < 0)
		{
            if (errno == EINTR)
                continue;
			if (errno == EWOULDBLOCK)
			{
				do
				{
                    timeout = tval;

                    CHECK_FOR_INTERRUPTS();
                    FD_ZERO(&rset);
                    FD_ZERO(&wset);
                    FD_SET(conn->sockfd, &wset);
					FD_SET(conn->sockfd, &rset);
					n = select(conn->sockfd + 1, &rset, &wset, NULL, &timeout);
                    if (n < 0)
					{
						if (errno == EINTR)
                            continue;
                        ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
										 errmsg("Interconnect error writing an outgoing packet"),
										 errdetail("error during select() call (error:%d).\n"
												   "For Remote Connection: contentId=%d at '%s':%d",
												   errno, conn->remoteContentId, conn->remoteHost,
												   conn->remotePort)));
					}

					/* as a sender... if there is something to read... it must
					 * mean its a StopSendingMessage.  we don't even bother to
					 * read it.
					 */
					if (FD_ISSET(conn->sockfd, &rset))
					{
						conn->stillActive = false;
						return false;
					}
				} while (n < 1);
			}
		}
		else
		{
			sent += n;
		}
	} while (sent < conn->msgSize);
	
	conn->tupleCount = 0;
	conn->msgSize = PACKET_HEADER_SIZE;
	
	return true;
}

/* Function readPacket() is used to read in the next packet from the given
 * MotionConn.
 *
 * This call blocks until the packet is read in, and is part of a
 * global scheme where senders block until the entire message is sent, and
 * receivers block until the entire message is read.  Both use non-blocking
 * socket calls so that we can handle any PG interrupts.
 *
 * Note, that for speed we want to read a message all in one go,
 * header and all. A consequence is that we may read in part of the
 * next message, which we've got to keep track of ... recvBytes holds
 * the byte-count of the unprocessed messages.
 *
 * PARAMETERS
 *   conn - MotionConn to read the packet from.
 *
 */
void readPacket(MotionConn *conn)
{
	int			n, bytesRead = conn->recvBytes;
	bool		gotHeader = false, gotPacket=false;
	fd_set		rset;

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "readpacket: (fd %d) (max %d) outstanding bytes %d", conn->sockfd, Cdb_max_packet_size, conn->recvBytes);
#endif

	/* do we have a complete message waiting to be processed ? */
	if (conn->recvBytes >= PACKET_HEADER_SIZE)
	{
		conn->msgSize = getInt16(conn->pBuff);
		gotHeader = true;
		if (conn->recvBytes >= conn->msgSize) {
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "readpacket: returning previously read data (%d)", conn->recvBytes);
#endif
			return;
		}
	}

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "readpacket: %s on previous call msgSize %d", gotHeader ? "got header" : "no header", conn->msgSize);
#endif

	while (!gotPacket && bytesRead < Cdb_max_packet_size)
	{
		   
		if ((n = read(conn->sockfd, conn->pBuff + bytesRead,
					  Cdb_max_packet_size - bytesRead)) < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK)
			{
				do
				{
                    struct timeval timeout = tval;

					/* see if user canceled and stuff like that */
					CHECK_FOR_INTERRUPTS();

                    FD_ZERO(&rset);
					FD_SET(conn->sockfd, &rset);
					n = select(conn->sockfd + 1, &rset, NULL, NULL, &timeout);
                    if (n < 0)
					{
                        if (errno == EINTR)
                            continue;
                        ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
										errmsg("Interconnect error reading an incoming packet"),
										errdetail("error during select() call (error:%d).\n"
												  "From Remote Connection: contentId=%d at '%s':%d",
												  errno, conn->remoteContentId, conn->remoteHost,
												  conn->remotePort)));	
					}
				}
				while (n < 1);
			}
			else
			{

				/* TODO: would be nice to read the errno and try and
				 * 		 provide more useful info as to why this
				 * 		 happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								errmsg("Interconnect error reading an incoming packet"),
								errdetail("error during read() call (error:%d).\n"
										  "From Remote Connection: contentId=%d at '%s':%d",
										  errno, conn->remoteContentId, conn->remoteHost,
										  conn->remotePort)));								
			}
		} else if (n == 0) 
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "readpacket(); breaking in while (fd %d) recvBytes %d msgSize %d", conn->sockfd, conn->recvBytes, conn->msgSize);
#endif
			/* Either we're done, or there is something seriously
			 * wrong */
			if (conn->recvBytes == 0) {
				elog(LOG, "INTERCONNECT: Connection ended prematurely????");
				conn->recvBytes = conn->msgSize = PACKET_HEADER_SIZE;
#if 0
				conn->stillActive = false;
#endif
				if (Cdb_role == CDB_ROLE_DISPATCH)
				{
					elog(LOG, "ending prematurely in dispatcher");
				} 
				else if (Cdb_role == CDB_ROLE_EXECUTE)
				{
					elog(LOG, "ending prematurely in executor");
				}
				elog(LOG, "my listener port %d remote host %s remote port %d remote contentId %d",
					 listenerPort, conn->remoteHost, conn->remotePort, conn->remoteContentId);
				break;
			}
			else if (conn->recvBytes == conn->msgSize)
			{
				/* got a completed message before the connection
				 * closed; probably this is an EOS message */
				elog(LOG, "INTERCONNECT: breaking with data %d", conn->msgSize);
				break;
			}
			else
			{
				/* TODO: would be nice to read the errno and try and provide more
				 * 		 useful info as to why this happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								errmsg("Interconnect error reading an incoming packet"),
								errdetail("Socket closed from remote connection).\n"
										  "contentId=%d at '%s':%d",conn->remoteContentId,
										  conn->remoteHost,conn->remotePort)));
			}
		}
		else
		{
			bytesRead += n;

			if (!gotHeader && bytesRead >= PACKET_HEADER_SIZE)
			{
				/* got the header */
				conn->msgSize = getInt16(conn->pBuff);
				gotHeader = true;
			}
			conn->recvBytes = bytesRead;

			if (gotHeader && bytesRead >= conn->msgSize)
				gotPacket = true;
		}
	}
#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "readpacket: got %d bytes", conn->recvBytes);
#endif
}

/* Function doBroadcast() is used to send a TupleChunk to all recipients.
 *
 * PARAMETERS
 *   pEntry - MotionLayerState context that contains everything we need to send.
 *   tcItem - TupleChunk to send.
 */
void doBroadcast(MotionLayerState *pEntry, TupleChunkListItem tcItem)
{	
	MotionConn 	*conn;
	int 		i;
	
	/* add our tcItem to each of the outgoing buffers. */
	for (i= 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;	
		
		if (conn->stillActive) /* only send to still interested receivers. */
			sendChunk(conn, tcItem);
		
		if (conn->hasMirrorConn && conn->mirrorConn->stillActive)
			sendChunk(conn->mirrorConn, tcItem);		
		
	}
	
}


/* Function findMyRouteId() is used to search through the given slice and find
 * the position in the slice->primaryProcesses list where "we" are located at.
 * This is done by matching up the contentid and listenerPort.
 *
 * PARAMETERS
 *	
 *   slice - Slice to search through.
 *
 * RETURN
 *   An int which is "our" position in the slice->primaryProcesses list.
 */
int findMyRouteId(Slice *slice)
{
	ListCell 	*cell;
	int 		id = 0;
	
	foreach(cell, slice->primaryProcesses)
	{
		CdbProcess *proc = (CdbProcess *)lfirst(cell);

#ifdef AMS_VERBOSE_LOGGING		
		elog(DEBUG3, "Checking if CdbProc is our own: segid: (ours is %d ==  CdbProc's is %d) "
		      " listenerPort:  (ours is %d == CdbProc's is %d)",
		      Cdb_segment, proc->contentid,
		      listenerPort , proc->listenerPort);
#endif
		
		if (proc->contentid == Cdb_segment &&
		    proc->listenerPort == listenerPort)
			return id;
			
		id++;
	}
	
	//DOH: dump more info here
	elog(ERROR, "Could not find my CdbProcess in slice #: %d", slice->sliceIndex);
		
	return -1;
}



/* Function findCdbProcess() takes the info in the RegisterMessage and looks up
 * the CdbProcess that corresponds to it.  Part of the reason for doing this is
 * to verify that the RegisterMessage is a valid one.
 *
 * PARAMETERS
 *
 *   msg - RegisterMessage that contains the info of which CdbProcess we are
 *		   	looking for.
 *
 * RETURNS
 *   The CdbProcess that matches with the info in the RegisterMessage.
 */
CdbProcess *findCdbProcess(RegisterMessage *msg)
{
	CdbProcess 	*proc = NULL;
	Slice 		*slice;
	ListCell 	*cell;
	int 		count = 0;
	bool 		found = false;
	
	if (list_length(sliceTable->slices) < msg->motNodeId)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Received a Register Message with "
							   "unknown motion Node ID: %d", msg->motNodeId), 						
						errdetail("Could not find a Slice in our slice table with "
								  "motion node ID: %d", msg->motNodeId)));
	}
	
	
	slice = (Slice *)list_nth(sliceTable->slices,  msg->motNodeId);
	
	
	foreach(cell, slice->primaryProcesses)
	{
		proc = (CdbProcess *)lfirst(cell);
		
		if (count == msg->srcRouteId && proc->contentid == msg->srcContentId &&
		    proc->listenerPort == msg->srcListenerPort)
		{
			found = true;
		}
		
		count++;
	}
	
	
	if (!found)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Received a Register Message from "
							   "unknown CdbProcess"), 						
						errdetail("Received register message from: (mnId=%d, routeId=%d,"
								  "contentId=%d, listenerPort=%d) but was unable to find"
								  " a matching CdbProcess in our sliceTable.",
								  msg->motNodeId,msg->srcRouteId,msg->srcContentId,
								  msg->srcListenerPort)));	
	}

	return proc;
}

void dumpEntryConnections( MotionLayerState *pEntry )
{
	int i;
	
	elog( DEBUG3, "----------------pEntry motNodeID: %d conn dump------------------", pEntry->motNodeId);
	for( i=0; i < pEntry->numConns; i++ )
	{
		MotionConn *conn = pEntry->conns + i;
		
		elog( DEBUG3, "conn#%d (sockfd=%d, remote host= '%s' port=%d, contentId: %d )",
				i, conn->sockfd, conn->remoteHost,	conn->remotePort, conn->remoteContentId );
		
		if( conn->hasMirrorConn )
		{
			conn = conn->mirrorConn;
			elog( DEBUG3, "conn#%d MIRROR (sockfd=%d, remote host= '%s' port=%d, contentId: %d )",
				i, conn->sockfd, conn->remoteHost,	conn->remotePort, conn->remoteContentId );
		
		}	
		
	}
	elog( DEBUG3, "----------------------------------------------------------------");
}

/* declared in cdbmotion.h */
/* Function SendStopMessage() sends a short "cancel" message to all motion-nodes
 * which are sending us tuples. This is the communication that is passed outbound
 * on the receive socket.
 *
 * We re-implement doBroadcast here, but instead of sending a tuple we send a
 * stop message.
 *
 * PARAMETERS
 *
 *   motNodeID - motion node identifier
 */
void SendStopMessage(int16 motNodeID)
{
	MotionLayerState	*pEntry;
	MotionConn			*conn;
	int					i;
	char				m = 'S';
	ssize_t				written;

	pEntry = getMotionState(motNodeID);
	Assert(pEntry != NULL);

	/* Note: we're only concerned with receivers here, so mirrors don't matter */
	for (i = 0; i < pEntry->highReadSock; i++)
	{
		conn = pEntry->conns + i;

		if (FD_ISSET(conn->sockfd, &pEntry->readSet))
		{
			/* someone is trying to send stuff to us, let's stop 'em */
			while ((written = write(conn->sockfd, &m, sizeof(m))) < 0)
                if (errno != EINTR)
                    break;

			if (written != sizeof(m))
			{
				/* how can this happen ? the kernel buffer should be
				 * empty in the send direction */
				elog(LOG, "SendStopMessage: failed on write %d", errno);
			}

			/* this connection will be shutdown() and close()d in
			 * CleanUpMotionLayerIPC() */
		}
	}
}

/* Function SendStopMessage() sends a short "ack" message, to one node
 * tell it that we've received its EOS notifier. This is pretty much
 * exactly the same code as SendStopMessage(), but without the
 * broadcast loop.
 *
 * PARAMETERS
 *
 *   motNodeID - motion node identifier
 *   route - route identifier
 */
void SendEosAck(int16 motNodeID, int16 route)
{
	MotionLayerState	*pEntry;
	MotionConn			*conn;
	ssize_t				written;
	char				m = 'A';

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "Sending EOS ack to route %d", route);
#endif

	pEntry = getMotionState(motNodeID);
	Assert(pEntry != NULL);

	conn = pEntry->conns + route;
	Assert(conn != NULL);

	while ((written = write(conn->sockfd, &m, sizeof(m))) < 0)
		if (errno != EINTR)
			break;

	if (written != sizeof(m))
	{
		/* how can this happen ? the kernel buffer should be
		 * empty in the send direction */
		elog(ERROR, "SendEosAck: failed on write %d", errno);
	}

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "Sent EOS ack (fd %d)", conn->sockfd);
#endif
}

void waitOnEosAck(MotionLayerState *pEntry)
{
	MotionConn	*conn;
	int			i, n, limit, readCount, waitCount;
	char		response;
	fd_set		waitset;

	do {
		struct timeval timeout = tval;

		waitCount = 0;

		FD_ZERO(&waitset);
		for (i= 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;	
		
			if (conn->stillActive)
			{
				FD_SET(conn->sockfd, &waitset);
				limit = MAX(limit, conn->sockfd);
				waitCount++;
			}
		
			if (conn->hasMirrorConn && conn->mirrorConn->stillActive)
			{
				FD_SET(conn->mirrorConn->sockfd, &waitset);
				limit = MAX(limit, conn->sockfd);
				waitCount++;
			}
		}
		if (!waitCount)
			break;

		n = select(limit+1, &waitset, NULL, NULL, &timeout);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Interconnect error waiting for EOS Ack"),
							errdetail("Select errno %d).\n"
									  "contentId=%d at '%s':%d",
									  errno, conn->remoteContentId,
									  conn->remoteHost,conn->remotePort)));
		}
		else if (n > 0) {
			for (i= 0; i < pEntry->numConns; i++)
			{
				conn = pEntry->conns + i;

				if (conn->stillActive &&
					FD_ISSET(conn->sockfd, &waitset)) {
					readCount = read(conn->sockfd, &response, sizeof(response));
					if (readCount != sizeof(response))
					{
						ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
										errmsg("Interconnect error reading EOS Ack"),
										errdetail("Select errno %d).\n"
												  "contentId=%d at '%s':%d",
												  errno, conn->remoteContentId,
												  conn->remoteHost,conn->remotePort)));
					}
					if (response == 'A' || response == 'S')
					{
						/* successfully acknowledged */
						conn->stillActive = false;
#ifdef AMS_VERBOSE_LOGGING
						elog(DEBUG3, "Received EOS ack from route %d", i);
#endif
					}
					else
					{
						ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
										errmsg("Interconnect error reading EOS Ack"),
										errdetail("invalid Ack message (%c).\n"
												  "contentId=%d at '%s':%d",
												  response, conn->remoteContentId,
												  conn->remoteHost,conn->remotePort)));
					}
				}
			}
		}
#ifdef AMS_VERBOSE_LOGGING
		if (n == 0) {
			elog(DEBUG3, "waitOnEosAck(): timeout");
		}
#endif
	} while (waitCount != 0);

}
