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


/*=========================================================================
 * STRUCTS 
 */
 
 /* backlog for listen() call */
#define LISTEN_BACK_LOG 5
 
#define getInt16(pNetData)    ntohs(*((int16 *)(pNetData)))
#define getInt32(pNetData)    ntohl(*((int32 *)(pNetData)))

 /* 2 bytes to store the size of the entire packet.  a packet is composed of
  * of one or more serialized TupleChunks (each of which has a TupleChunk 
  * header. 
  */
 #define PACKET_HEADER_SIZE	  2

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

List *incompleteConns;

/* keeps track of if we've "activated" connections via SetupInterconnect(). */
bool activated;

/*slice table stuff. */
SliceTable *sliceTable;
int	sliceId;

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
MotionLayerState *createMotionState(int16 motNodeID, int numConns);
MotionLayerState *getMotionState(int16 motNodeID);
MotionLayerState *removeMotionState(int16 motNodeID);

MotionLayerState  *startConnections(Slice *mySlice, int *highsock, fd_set *wset, int *connsStarted);
void setupOutgoingConnection( MotionConn *conn, CdbProcess *cdbProc,int *highsock, fd_set *wset );
void sendRegisterMessage(MotionLayerState *pEntry, MotionConn *conn);
void readRegisterMessage(MotionConn *in_conn, int *incoming_count);
void acceptIncomingConnection(fd_set *rset, int *highsock);

bool sendChunk(MotionConn *conn, TupleChunkListItem tcItem);
void readPacket(MotionConn *conn);
void doBroadcast(MotionLayerState *pEntry, TupleChunkListItem tcItem);
int findMyRouteId(Slice *slice);
CdbProcess *findCdbProcess(RegisterMessage *msg);

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
}


/* See ml_ipc.h */
void CleanUpMotionLayerIPC(void)
{
	/* DOH: do we shutdown the listener socket?  need to figure out who actually
	 *      calls this and when
	 */
#ifdef AMS_VERBOSE_LOGGING	 
	elog(DEBUG3, "CleanUpMotionLayerIPC()");
#endif

	/*DOH: any memory to clean up?*/
	
	/*DOH: incompleteConns? */
	
	hash_destroy(hashtab);
	
	/* close down the Interconnect listener socket. */
	shutdown(listenerFd, SHUT_RDWR);
	close(listenerFd);
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
	elog(DEBUG3, "SendTupleChunkToAMS()");
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
			sendChunk(conn, tcItem );
			
			if( conn->hasMirrorConn )
				sendChunk(conn->mirrorConn, tcItem);
				
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

	Assert(activated);

	/* check em' */
	CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING	
	elog(DEBUG3, "SendEosToAMS() Entering");
#endif

	pEntry = getMotionState(motNodeID);

	/* we want to add our tcItem onto each of the outgoing buffers */
	doBroadcast(pEntry, tcItem);

#ifdef AMS_VERBOSE_LOGGING	
	elog(DEBUG3, "SendEosToAMS() Leaving");
#endif
}

/* See ml_ipc.h */
TupleChunkListItem RecvTupleChunkFromAny(int16 motNodeID, int16 *srcRoute)
{
	MotionLayerState 		*pEntry;
	MotionConn 				*conn;
	TupleChunkListItem 		tcItem;
	fd_set 					rset;
	int 					n,i;

	Assert(activated);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "RecvTupleChunkFromAny(motNodeId=%d)", motNodeID);
#endif

	pEntry = getMotionState(motNodeID);
	do
	{
		memcpy(&rset, &(pEntry->readSet), sizeof(fd_set));
		if ((n = select(pEntry->highReadSock + 1, &rset, NULL, NULL, &tval)) < 0)
		{
			/* TODO: would be nice to read the errno and try and
			 * 		 provide more useful info as to why this happened.
			 *       (select sets either EINTR or EBADF)
			 */
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Interconnect error receiving an incoming packet: %s", strerror(errno)),
							errdetail("error during select() call (error:%d).\n", errno)));
		}
		
		/* make sure we check for these. */
		CHECK_FOR_INTERRUPTS();
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "RecvTupleChunkFromAny() select() returned %d ready sockets", n);
#endif
	} while (n < 1);
	
	for (i=0; i < pEntry->numConns; i++)
	{	
		conn = pEntry->conns + i;
			
		/* TODO: should probably start from a random location in the
		 *		 list else things won't be that fair. In small tests,
		 *		 it looks like we usually only have one fd ready at a
		 *		 time; so scan order doesn't help much in the few-QE
		 *		 case.
		 */
		if (FD_ISSET(conn->sockfd, &rset))
		{		
			tcItem = RecvTupleChunkFrom(motNodeID, i);
			*srcRoute = i;
			
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

	MotionLayerState 	*pEntry;
	MotionConn 			*conn;
	TupleChunkListItem 	tcItem;
	TupleChunkListItem 	firstTcItem = NULL;
	TupleChunkListItem 	lastTcItem = NULL;
	uint16 				tcSize;
	int 				bytesProcessed = 0;

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
	
	conn->msgSize = 0;
	
	return firstTcItem;
}


/* See ml_ipc.h */
void DeregisterReadInterest(int16 motNodeID, int16 srcRoute)
{
	MotionLayerState 	*pEntry;
	MotionConn 		 	*conn;
		
	Assert(activated);
		
	pEntry = getMotionState(motNodeID);
	conn = pEntry->conns + srcRoute;
	
	FD_CLR(conn->sockfd, &(pEntry->readSet));
}



/* See ml_ipc.h */
int ListenerSetup(void)
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

	bzero(&addr, sizeof(addr));
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

	mySlice = (Slice*)list_nth(sliceTable->slices, mySliceId);

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
		int childId = (int)lfirst(cell);
	
		elog(DEBUG5, "--Setting up RECEIVING motion node: %d", childId);
		
		aSlice = (Slice*)list_nth(sliceTable->slices, childId);
		
		pEntry = createMotionState(childId, list_length(aSlice->primaryProcesses));
		
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

		if ((n = select(highsock + 1, &rrset, &wwset, NULL, &tval)) < 0)
		{
			if (errno != EINTR)
			{
				/* TODO: would be nice to read the errno and try and provide more
				 * 		 useful info as to why this happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								 errmsg("Interconnect error setting up: %s", strerror(errno)),
								 errdetail("error during select() call (error:%d).",errno)));
			}
		}

		elog(DEBUG3, "select() returned %d ready sockets", n);

		/* is it someone tickling our listener port? */
		if (FD_ISSET(listenerFd, &rrset))
		{
			elog(DEBUG5, "accepting an Incoming Connection ");
			acceptIncomingConnection(&rset, &highsock);
			n--;
		}

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
					n--;
				}
				
				if( conn->hasMirrorConn )
				{
					if( FD_ISSET( conn->mirrorConn->sockfd, &wwset))
					{
						elog(DEBUG5, "Sending a register message to sockfd: %d", conn->mirrorConn->sockfd);
						sendRegisterMessage(sendingMotionState, conn->mirrorConn); 
						
						/*no longer interested in this for now.*/
						FD_CLR(conn->mirrorConn->sockfd, &wset);
						
						outgoing_count++;
						n--;
					}
				}
			}

		}

		/* check our connections that are accepted'd but no register message.
		 * we don't know which motion node these apply to until we actually
		 * receive the REGISTER message.  this is why they are all in a single
		 * list.
		 */
		foreach(cell, incompleteConns)
		{
			MotionConn *conn = (MotionConn*)lfirst(cell);
		
			if (FD_ISSET(conn->sockfd, &rrset))
			{
				elog(DEBUG3, "conn # %d has data!", conn->sockfd);
				
				readRegisterMessage(conn, &incoming_count);				
				FD_CLR(conn->sockfd, &rset);
				list_delete(incompleteConns, cell);
			}
		}

		elog(DEBUG3, "%d of %d outgoing conns and %d of %d incoming conns completed",
			  outgoing_count, expectedTotalOutgoing, incoming_count, expectedTotalIncoming);
	}
	while ((outgoing_count != expectedTotalOutgoing) || (incoming_count != expectedTotalIncoming));

	activated = true;
	elog(DEBUG3, "FINISHED ACTIVATING ROUTES. (out: %d , in: %d)", outgoing_count, incoming_count);

	gettimeofday(&snapTime, NULL);
	elog(DEBUG3, "exiting SetupInterconnect @%d time is %ld.%ld", 
		 __LINE__, (long int)snapTime.tv_sec, (long int)snapTime.tv_usec);
}


/* See ml_ipc.h */
void TeardownInterconnect(void)
{
	ListCell 		*cell;
	MotionLayerState 	*pEntry;
	int				i;
	Slice 			*mySlice;
	MotionConn		*conn;	

	if (!activated)
		return;

	elog(DEBUG3, "Interconnect cleanup.");

	mySlice = (Slice*)list_nth(sliceTable->slices, sliceId);

	if (mySlice->parentIndex != -1)
	{
		/* cleanup a Sending motion node. */
		elog(DEBUG3, "close down incoming connections for motion id: %d", 
				mySlice->sliceIndex);
		pEntry = removeMotionState(mySlice->sliceIndex);

	 	if (activated)
	 	{
	 		for (i=0; i < pEntry->numConns; i++)
			{
				conn = pEntry->conns + i;
				shutdown(conn->sockfd, SHUT_WR);
				close(conn->sockfd);
				
				if( conn->hasMirrorConn )
				{
					shutdown(conn->mirrorConn->sockfd, SHUT_WR);
					close(conn->mirrorConn->sockfd);
					pfree(conn->mirrorConn);
				}
				
				pfree(conn->pBuff);
			}
			pfree(pEntry->conns);
			
	 	}
	}

	/* cleanup all of our Receiving Motion nodes. */
	foreach(cell, mySlice->children)
	{
		Slice *aSlice;

		int childId = (int)lfirst(cell);

		aSlice = (Slice*)list_nth(sliceTable->slices, childId);

		pEntry = removeMotionState(aSlice->sliceIndex);
		elog(DEBUG3, "close down connections for motion Id: %d", pEntry->motNodeId);

    	if (activated)
	 	{
			for (i=0; i < pEntry->numConns; i++)
			{
				conn = pEntry->conns + i;
				shutdown(pEntry->conns[i].sockfd, SHUT_WR);	
				close(pEntry->conns[i].sockfd);
				
				pfree(conn->pBuff);
			}

			pfree(pEntry->conns);
		}		
	}

	list_free(incompleteConns);

	incompleteConns = NIL;

	activated = false;
}


/*=========================================================================
 * HELPER FUNCTIONS
 */


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
            errmsg("Interconect Error: A HTAB entry for motion node %d already exists.",
            motNodeID)));
    }

    pEntry->motNodeId 	   = motNodeID;
    pEntry->numConns 	   = numConns;
    pEntry->conns 		   = palloc(pEntry->numConns * sizeof(MotionConn));
	pEntry->highReadSock   = 0;

	bzero(pEntry->conns, pEntry->numConns * sizeof(MotionConn));
	
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
	
	if (pEntry == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not find a MotionLayerState entry"
							   "for motion node ID: %d", motNodeID)));
	}

	if (!found)
	{
		/* not sure why we would be cleaning up a motion node we never setup.
		 * so this seems like a problem. */
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Could not find a MotionLayerState entry"
							   "for motion node %d.", motNodeID)));
	}  

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
	int					i, n, motNodeId;
	CdbProcess			*cdbProc;
	

	/* for sending motion nodes... motionNodeId = sliceIndex. */
	motNodeId = mySlice->sliceIndex;

#ifdef AMS_VERBOSE_LOGGING		
	elog(DEBUG3, "Interconnect - Starting Outgoing Connections");
#endif	
	
	recvSlice = (Slice*)list_nth(sliceTable->slices, mySlice->parentIndex);
	
	pEntry = createMotionState(motNodeId, list_length(recvSlice->primaryProcesses));
	
	/* Setup an outgoing socket to the right place for each of our outbound 
	 * connections for the primary Connections.
	 */
	for (i=0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;
	
		cdbProc = (CdbProcess*) list_nth(recvSlice->primaryProcesses, i);
		setupOutgoingConnection( conn, cdbProc, highsock, wset );	
		(*connsStarted)++;
		
		/*setup mirror connection if not null*/
		if( recvSlice->mirrorProcesses != NIL )
		{
			cdbProc = (CdbProcess*) list_nth(recvSlice->mirrorProcesses, i);
			if( cdbProc != NULL )
			{
				conn->mirrorConn = palloc( sizeof(MotionConn) );
				conn->hasMirrorConn = true;
			
				setupOutgoingConnection( conn->mirrorConn, cdbProc, highsock, wset );
				(*connsStarted)++;
			}
		}
	}
	
	return pEntry;
}

void setupOutgoingConnection( MotionConn *conn, CdbProcess *cdbProc, int *highsock, fd_set *wset )
{
	struct sockaddr_in	addr;
	int n;
	
	conn->sockfd			= socket(AF_INET, SOCK_STREAM, 0);
	conn->pBuff				= palloc(MAX_PACKET_SIZE);
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
	if (fcntl(conn->sockfd, F_SETFL, O_NONBLOCK) < 0)
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
	netval = htons(CdbIdentity.segindex);
	memcpy(msg + 4, &netval, 2);

	/* srcListenerPort */
	netval2 = htonl(listenerPort);
	memcpy(msg + 6, &netval2, 4);

	/* Since our message is very small and we should have at least some 
	 * dedicated TCP buffer space.  so we should be able to use a blocking 
	 * send() call to write it out.
	 */	
	if ((n = write(conn->sockfd, msg, msglen)) < 0)
	{
		/* TODO: would be nice to read the errno and try and provide more
		 * 		 useful info as to why this happened.
		 */
		elog(DEBUG3, "n = %d", n);
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
		if ((n = read(conn->sockfd, buff + bytesRead, msgSize - bytesRead)) < 0)
		{
			/* TODO: would be nice to read the errno and try and provide more
			 * 		 useful info as to why this happened.
			 */
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
	if ((pEntry->conns + msg.srcRouteId)->sockfd != 0)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("Interconnect Error: Already a conn for (mnId=%d,routeId=%d)", 
							   msg.motNodeId, msg.srcRouteId),
						errdetail("Should not have a non-NULL connection in that slot.")));	
	}
	
	pEntry->conns[msg.srcRouteId] = (MotionConn)*conn;
	
	/* find and validate our RegisterMessage based on the CdbProcess. */
	proc = findCdbProcess(&msg);
		
	conn->remoteHost 	  = proc->listenerAddr;
	conn->remotePort 	  = msg.srcListenerPort;
	conn->remoteContentId = msg.srcContentId;
	
	elog(DEBUG3, "readRegisterMessage: conn->sockfd: %d", conn->sockfd);
	
	FD_SET(conn->sockfd, &(pEntry->readSet));
	
	if (conn->sockfd > pEntry->highReadSock)
		pEntry->highReadSock = conn->sockfd;

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
	
	bzero(&addr, sizeof(addr));
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
	if (fcntl(newsockfd, F_SETFL, O_NONBLOCK) < 0)
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
	conn->pBuff      = palloc(MAX_PACKET_SIZE);
	conn->msgSize    = 0;
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
 * destination.
 *
 * PARAMETERS
 *   conn - MotionConn that the tcItem is to be sent to.
 *   tcItem - message to be sent.
 */
bool sendChunk(MotionConn *conn, TupleChunkListItem tcItem) 
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

	/* first send header */
	header = htons(sizeof(header) + tcItem->chunk_length);
	sendptr = (char *)&header;
	sent = 0;
	do {
		if ((n = write(conn->sockfd, sendptr + sent, sizeof(header) - sent)) < 0)
		{
			if (errno == EWOULDBLOCK)
			{
				FD_ZERO(&rset);
				FD_ZERO(&wset);
				do
				{
					FD_SET(conn->sockfd, &wset);
					/* TODO: uncomment this when ready to support "STOP MESSAGES" */
					//FD_SET(conn->sockfd, &rset);
					if ((n = select(conn->sockfd + 1, &rset, &wset, NULL, &tval)) < 0)
					{
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

					/* make sure we check for these. */
					CHECK_FOR_INTERRUPTS();
				} while (n < 1);
			}
		}
		else
		{
			sent += n;
		}
	} while (sent < (int)sizeof(header));

	/* now send body */
	sendptr = (char *)tcItem->chunk_data;
	sent = 0;
	do {
		if ((n = write(conn->sockfd, sendptr + sent, tcItem->chunk_length - sent)) < 0)
		{
			if (errno == EWOULDBLOCK)
			{
				FD_ZERO(&rset);
				FD_ZERO(&wset);
				do
				{
					FD_SET(conn->sockfd, &wset);
					/* TODO: uncomment this when ready to support "STOP MESSAGES" */
					//FD_SET(conn->sockfd, &rset);
					if ((n = select(conn->sockfd + 1, &rset, &wset, NULL, &tval)) < 0)
					{
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

					/* make sure we check for these. */
					CHECK_FOR_INTERRUPTS();
				} while (n < 1);
			}
		}
		else
		{
			sent += n;
		}
	} while (sent < (int)(tcItem->chunk_length));
	
	conn->tupleCount++;	
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
 * PARAMETERS
 *   conn - MotionConn to read the packet from.
 *
 */
void readPacket(MotionConn *conn)
{
	int 	n, bytesRead = 0;
	fd_set 	rset;	

	/* read in the header. */
	while (bytesRead < PACKET_HEADER_SIZE)
	{
		if ((n = read(conn->sockfd, conn->pBuff + bytesRead, 
			       PACKET_HEADER_SIZE - bytesRead)) < 0)
		{
			if (errno == EWOULDBLOCK)
			{
				FD_ZERO(&rset);
				do
				{
					FD_SET(conn->sockfd, &rset);
					if ((n = select(conn->sockfd + 1, &rset, NULL, NULL, &tval)) < 0)
					{
						/* TODO: would be nice to read the errno and try and provide more
						 * 		 useful info as to why this happened.
						 */
						ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
										 errmsg("Interconnect error reading an incoming packet"),
										 errdetail("error during select() call (error:%d).\n"
												   "From Remote Connection: contentId=%d at '%s':%d",
												   errno, conn->remoteContentId, conn->remoteHost,
												   conn->remotePort)));	
					}

					/* see if user canceled and stuff like that */
					CHECK_FOR_INTERRUPTS();
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
		}
		else if (n == 0)
		{
			/* TODO: would be nice to read the errno and try and
			 * 		 provide more useful info as to why this happened.
			 */
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
					 errmsg("Interconnect error reading an incoming packet"),
					 errdetail("Socket closed from remote connection).\n"
						   "contentId=%d at '%s':%d",conn->remoteContentId, 
						   conn->remoteHost,conn->remotePort)));
		}
		else
			bytesRead += n;
			
	}
	
	/* now we've read in the header, go ahead and figure out how big the whole
	 * packet is.
	 */	
	conn->msgSize = getInt16(conn->pBuff);
	Assert(conn->msgSize > 0);

	/* read in the rest of the message. */
	while (bytesRead != conn->msgSize)
	{
		if ((n = read(conn->sockfd, conn->pBuff + bytesRead, 
			       conn->msgSize - bytesRead)) < 0)
		{
			if (errno == EWOULDBLOCK)
			{
				FD_ZERO(&rset);
				do
				{
					FD_SET(conn->sockfd, &rset);
					if ((n = select(conn->sockfd + 1, &rset, NULL, NULL, &tval)) < 0)
					{
						/* TODO: would be nice to read the errno and try and provide more
						 * 		 useful info as to why this happened.
						 */
						ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
										 errmsg("Interconnect error reading an incoming packet"),
										 errdetail("error during select() call (error:%d).\n"
												   "From Remote Connection: contentId=%d at '%s':%d",
												   errno, conn->remoteContentId, conn->remoteHost,
												   conn->remotePort)));				
					}					

					/* make sure we check for these. */
					CHECK_FOR_INTERRUPTS();
				}
				while (n < 1); 
			}
			else
			{
				/* TODO: would be nice to read the errno and try and provide more
				 * 		 useful info as to why this happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						 errmsg("Interconnect error reading an incoming packet"),
						 errdetail("error during read() call (error:%d).\n"
							   "From Remote Connection: contentId=%d at '%s':%d",
							   errno, conn->remoteContentId, conn->remoteHost,
							   conn->remotePort)));
			}
		}
		else if (n == 0)
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
		else
			bytesRead += n;
				
		Assert(bytesRead <= conn->msgSize);
	}
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
		
		if( conn->hasMirrorConn && conn->mirrorConn->stillActive )
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
		CdbProcess *proc = (CdbProcess*) lfirst(cell);

#ifdef AMS_VERBOSE_LOGGING		
		elog(DEBUG3, "Checking if CdbProc is our own: segid: (ours is %d ==  CdbProc's is %d) "
		      " listenerPort:  (ours is %d == CdbProc's is %d)", 
		      CdbIdentity.segindex, proc->contentid, 
		      listenerPort , proc->listenerPort);
#endif
		
		if (proc->contentid == CdbIdentity.segindex && 
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
	
	
	slice = (Slice*)list_nth(sliceTable->slices,  msg->motNodeId);		
	
	
	foreach(cell, slice->primaryProcesses)
	{
		proc = (CdbProcess*) lfirst(cell);
		
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

