/*-------------------------------------------------------------------------
 * ml_ipc.c
 *	   Motion Layer IPC Layer.
 *
 * Copyright (c) 2005, Green Plum
 *
 * IDENTIFICATION:
 *	   $Id$
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
#include <netinet/in.h>

#include "port.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#endif

/*=========================================================================
 * STRUCTS
 */

 /*
  * backlog for listen() call: it is important that this be something like a
  * good match for the maximum number of QEs. Slow insert performance will
  * result if it is too low.
  */
#define LISTEN_BACK_LOG 64

#define getInt16(pNetData)	  ntohs(*((int16 *)(pNetData)))
#define getInt32(pNetData)	  ntohl(*((int32 *)(pNetData)))

/* Structure used for keeping track of a pt-to-pt connection between two
 * Cdb Entities (either QE or QD).
 */
typedef struct MotionConn
{
	/* socket file descriptor. */
	int			sockfd;

	/* pointer to the data buffer. */
	uint8	   *pBuff;

	/* size of the message in the buffer, if any. */
	int32		msgSize;

	/* position of message inside of buffer, "cursor" pointer */
	uint8	   *msgPos;

	/*
	 * recv bytes: we can have more than one message/message fragment in recv
	 * queue at once
	 */
	int32		recvBytes;

	int			tupleCount;

	bool		stillActive;

	char	   *remoteHost;
	int			remotePort;
	int			remoteContentId;

	bool		hasMirrorConn;
	struct MotionConn *mirrorConn;

}	MotionConn;

/* used to organize all of the information for a given motion node.
 *
 */
typedef struct MotionLayerState
{
	int16		motNodeId;

	int			numConns;

	MotionConn *conns;

	int			numMirrorConns;

	MotionConn *mirrorConns;	/* only used by the sender.  the receiver has
								 * no concept of whether its a mirror or not.
								 * so it always stores its connections in
								 * conns. */

	int			myRouteId;

	/*
	 * used for receiving. to select() from a set of interesting MotionConns
	 * to see when data is ready to be read.  When the incoming connections
	 * are established, read interest is turned on.  It is turned off when an
	 * EOS (End of Stream) message is read.
	 */
	fd_set		readSet;

	/* highest file descriptor in the readSet. */
	int			highReadSock;

	bool		valid;
}	MotionLayerState;

/*DOH: include a routeId and move away from using contentId and
 *	   isPrimary for validation.  use ip address and port instead.
 */
typedef struct RegisterMessage
{
	int16		motNodeId;
	int16		srcRouteId;
	int16		srcContentId;
	int32		srcListenerPort;
#ifdef AMS_VERBOSE_LOGGING
	int32		srcPid;
#endif
}	RegisterMessage;

/*=========================================================================
 * GLOBAL STATE VARIABLES
 */

/* Socket file descriptor for the listener. */
int			listenerFd;

/* TODO: not sure we need this. could just push it as a local variable
 *		 into ListenerSetup()
 */
struct sockaddr_in serv_addr;

/* Use an array to store our motion-layer-state table. */
int			mlsSize = 0;
MotionLayerState *mlsStates = NULL;

#define MLS_INITIAL_SIZE (10)

/* TCP port the Interconnect listens on for incoming connections from other
 * remote Interconnect entities.  The listenerPort is assigned at process startup
 * and is set by ListenerSetup().
 */
int			listenerPort;

/* our timeout value for select() and other socket operations. */
struct timeval tval;

List	   *incompleteConns = NIL;

/* keeps track of if we've "activated" connections via SetupInterconnect(). */
bool		activated;

/*slice table stuff. */
SliceTable *sliceTable = NULL;
int			sliceId;

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static int	listenerSetup(void);

static MotionLayerState *createMotionState(int16 motNodeID, int numConns);
static inline MotionLayerState *getMotionState(int16 motNodeID);
static MotionLayerState *removeMotionState(int16 motNodeID);

static MotionLayerState *startConnections(Slice * mySlice, int *highsock, fd_set *wset, int *connsStarted);
static void setupOutgoingConnection(MotionConn * conn, CdbProcess * cdbProc, int *highsock, fd_set *wset);
static void sendRegisterMessage(MotionLayerState * pEntry, MotionConn * conn);
static void readRegisterMessage(MotionConn * in_conn, int *incoming_count);
static void acceptIncomingConnection(fd_set *rset, int *highsock);

static bool sendChunk(MotionConn * conn, TupleChunkListItem tcItem);
static inline void readPacket(MotionConn * conn);
static inline int doBroadcast(MotionLayerState * pEntry, TupleChunkListItem tcItem);
static int	findMyRouteId(Slice * slice);
static CdbProcess *findCdbProcess(RegisterMessage * msg);

static bool flushBuffer(MotionConn * conn);
void		dumpEntryConnections(MotionLayerState * pEntry);

void		print_connection(int fd, const char *msg);

static void waitOnSocketDone(int);
static void flushListenerBacklog(void);

static inline TupleChunkListItem RecvTupleChunk(MotionConn * conn);

/*=========================================================================
 * VISIBLE FUNCTIONS
 */

/* See ml_ipc.h */
void
InitMotionLayerIPC(void)
{
	incompleteConns = NIL;

	tval.tv_sec = 0;
	tval.tv_usec = 500000;

	activated = false;

	elog(DEBUG1, "Setting Up Interconnect Listener Port...");
	Mpp_listener_port = listenerSetup();
}


/* See ml_ipc.h */
void
CleanUpMotionLayerIPC(void)
{

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "CleanUpMotionLayerIPC()");
#endif

	/* close down the Interconnect listener socket. */
	shutdown(listenerFd, SHUT_RDWR);
	close(listenerFd);

	/* be safe and reset global state variables. */
	activated = false;
	listenerPort = -1;
	listenerFd = -1;
	sliceTable = NULL;
	sliceId = -1;

	list_free(incompleteConns);
}

/* See ml_ipc.h */
bool
SendTupleChunkToAMS(int16 motNodeID, int16 targetRoute, TupleChunkListItem tcItem)
{
	int			i,
				recount = 0;
	MotionLayerState *pEntry;
	MotionConn *conn;
	TupleChunkListItem currItem;

	Assert(activated);

	/* check em' */
	CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "SendTupleChunkToAMS(motNodeID: %d, targetRoute: %d)", motNodeID, targetRoute);
#endif

	/*
	 * tcItem can actually be a chain of tcItems.  we need to send out all of
	 * them.
	 */
	pEntry = getMotionState(motNodeID);

	currItem = tcItem;

	for (currItem = tcItem; currItem != NULL; currItem = currItem->p_next)
	{
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG3, "SendTupleChunkToAMS: chunk length %d", currItem->chunk_length);
#endif

		if (targetRoute == BROADCAST_SEGIDX)
		{
			if (doBroadcast(pEntry, currItem) != 0)
				recount = 1;
		}
		else
		{
			/* handle pt-to-pt message. */
			conn = pEntry->conns + targetRoute;
			/* only send to interested connections */
			if (conn->stillActive)
			{
				sendChunk(conn, currItem);
				if (!conn->stillActive)
					recount = 1;
			}
			if (conn->hasMirrorConn && conn->mirrorConn->stillActive)
			{
				sendChunk(conn->mirrorConn, currItem);
				if (conn->mirrorConn->stillActive)
					recount = 1;
			}
		}
	}

	if (recount == 0)
		return true;

	/* if we don't have any connections active, return false */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;
		if (conn->stillActive)
			break;
		if (conn->hasMirrorConn && conn->mirrorConn->stillActive)
			break;
	}

	/* if we found an active connection we're not done */
	return (i < pEntry->numConns);
}


/* See ml_ipc.h */
void
SendEosToAMS(int16 motNodeID, int16 targetRoute, TupleChunkListItem tcItem)
{
	MotionLayerState *pEntry;
	MotionConn *conn;
	int			i;

	Assert(activated);

	/* check em' */
	CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "SendEosToAMS() Entering");
#endif

	pEntry = getMotionState(motNodeID);

	/* we want to add our tcItem onto each of the outgoing buffers */
	doBroadcast(pEntry, tcItem);

	/* now flush all of the buffers. */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;
		flushBuffer(conn);

		if (conn->hasMirrorConn)
			flushBuffer(conn->mirrorConn);
	}

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "SendEosToAMS() Leaving");
#endif
}

/* See ml_ipc.h */
TupleChunkListItem
RecvTupleChunkFromAny(int16 motNodeID, int16 *srcRoute)
{
	static int	scanStart = 0;
	MotionLayerState *pEntry;
	MotionConn *conn;
	TupleChunkListItem tcItem;
	fd_set		rset;
	int			n,
				i,
				index;
	bool		skipSelect = false;

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

		/*
		 * since we may have data in a local buffer, we may be able to
		 * short-circuit the select() call (and if we don't do this we may
		 * wait when we have data ready, since it has already been read)
		 */
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (FD_ISSET(conn->sockfd, &rset) && conn->recvBytes != 0)
			{
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

	/*
	 * We scan the file descriptors starting from where we left off in the
	 * last call (don't continually poll the first when others may be ready!).
	 */
	for (i = 0; i < pEntry->numConns; i++)
	{
		/*
		 * avoid division ? index = ((scanStart + i) % pEntry->numConns);
		 */
		index = scanStart + i;
		if (index >= pEntry->numConns)
			index -= pEntry->numConns;

		conn = pEntry->conns + index;

#ifdef AMS_VERBOSE_LOGGING
		if (!conn->stillActive)
		{
			elog(LOG, "RecvTupleChunkFromAny: trying to read on inactive socket %d", conn->sockfd);
		}
#endif

		if (FD_ISSET(conn->sockfd, &rset))
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "RecvTupleChunkFromAny() (fd %d) %d/%d", conn->sockfd, motNodeID, index);
#endif
			tcItem = RecvTupleChunk(conn);

			*srcRoute = index;

			/*
			 * advance start point (avoid doing division/modulus operation
			 * here)
			 */
			scanStart = index + 1;

			return tcItem;
		}
	}

	/* we should never ever get here... */
	elog(ERROR, "for some reason RecvTupleFromAny didn't return anything.");
	return NULL;
}

/* See ml_ipc.h */
TupleChunkListItem
RecvTupleChunkFrom(int16 motNodeID, int16 srcRoute)
{
	MotionLayerState *pEntry;
	MotionConn *conn;

	Assert(activated);

	/* check em' */
	CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "RecvTupleChunkFrom(motNodID=%d, srcRoute=%d)", motNodeID, srcRoute);
#endif

	pEntry = getMotionState(motNodeID);
	conn = pEntry->conns + srcRoute;

	return RecvTupleChunk(conn);
}

static inline TupleChunkListItem
RecvTupleChunk(MotionConn * conn)
{
	TupleChunkListItem tcItem;
	TupleChunkListItem firstTcItem = NULL;
	TupleChunkListItem lastTcItem = NULL;
	uint16		tcSize;
	int			bytesProcessed = 0;

	/* read the packet in from the network. */
	readPacket(conn);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "read packet recv bytes %d msgsize %d", conn->recvBytes, conn->msgSize);
#endif

	/* go through and form us some TupleChunks. */
	bytesProcessed = PACKET_HEADER_SIZE;

	while (bytesProcessed != conn->msgSize)
	{
		tcSize = TUPLE_CHUNK_HEADER_SIZE + (*(uint16 *) (conn->msgPos + bytesProcessed));

		if (tcSize >= conn->msgSize)
		{
			elog(ERROR, "tcSize %d >= conn->msgSize %d !", tcSize, conn->msgSize);
		}
		Assert(tcSize < conn->msgSize);

		/*
		 * We store the data inplace, and handle any necessary copying later
		 * on
		 */
		tcItem = (TupleChunkListItem) palloc0(sizeof(TupleChunkListItemData));
		if (tcItem == NULL)
		{
			ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
					errmsg("Could not allocate space on interconnect rx.")));
		}
		tcItem->chunk_length = tcSize;
		tcItem->inplace = (char *)(conn->msgPos + bytesProcessed);

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
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG3, "residual message %d bytes", conn->recvBytes);
#endif
		conn->msgPos += conn->msgSize;
	}

	conn->msgSize = 0;

	return firstTcItem;
}

/* DeregisterReadInterest is called on receiving nodes when they
 * believe that they're done with the receiver */
void
DeregisterReadInterest(int16 motNodeID, int16 srcRoute)
{
	MotionLayerState *pEntry;
	MotionConn *conn;

	Assert(activated);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "DeregisterReadInterest()");
#endif

	pEntry = getMotionState(motNodeID);
	conn = pEntry->conns + srcRoute;

	/*
	 * we also mark the connection as "done." The way synchronization works is
	 * strange. On QDs the "teardown" doesn't get called until all segments
	 * are finished, which means that we need some way for the QEs to know
	 * that Teardown should complete, otherwise we deadlock the entire query
	 * (QEs wait in their Teardown calls, while the QD waits for them to
	 * finish)
	 *
	 *
	 */
	shutdown(conn->sockfd, SHUT_WR);

	FD_CLR(conn->sockfd, &(pEntry->readSet));
}

/* See ml_ipc.h */
void
SetupInterconnect(SliceTable * mySliceTable, int mySliceId)
{
	int			i,
				n,
				highsock = 0;
	fd_set		rset,
				rrset;
	fd_set		wset,
				wwset;
	ListCell   *cell;
	Slice	   *mySlice;
	Slice	   *aSlice;
	MotionLayerState *pEntry;

	struct timeval startTime;
	struct timeval snapTime;
	struct timeval stopTime;

	int			incoming_count = 0;
	int			outgoing_count = 0;
	int			expectedTotalIncoming = 0;
	int			expectedTotalOutgoing = 0;

	/* we can have at most one of these. */
	MotionLayerState *sendingMotionState = NULL;

	Assert(!activated);

	/*
	 * The assert only happens when we are compiled with --enable-cassert.
	 * So, let's report the error even if we are a production build.
	 */
	if (activated)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
			   errmsg("SetupInterconnect: Called when already activated.")));
	}

	if (mlsStates != NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("SetupInterconnect: MLS table not cleaned.")));
	}
	mlsSize = MLS_INITIAL_SIZE;
	mlsStates = palloc0(MLS_INITIAL_SIZE * sizeof(MotionLayerState));
	if (mlsStates == NULL)
	{
		ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("Could not allocate space for motion layer state.")));
	}

	gettimeofday(&snapTime, NULL);
	elog(DEBUG3, "in SetupInterconnect @%d time is %ld.%ld",
		 __LINE__, (long int) snapTime.tv_sec, (long int) snapTime.tv_usec);

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

	mySlice = (Slice *) list_nth(sliceTable->slices, mySliceId);

	if (mySlice->parentIndex != -1)
	{
		elog(DEBUG5, "--Setting up SENDING motion node: %d...starting Connections",
			 mySlice->sliceIndex);

		/* can only have at most one sender. */
		Assert(sendingMotionState == NULL);

		/* MotionEntry created inside of startConnections. */
		pEntry = startConnections(mySlice, &highsock, &wset, &expectedTotalOutgoing);

		pEntry->myRouteId = findMyRouteId(list_nth(sliceTable->slices, mySlice->sliceIndex));

		sendingMotionState = pEntry;
	}

	/* now we'll do some setup for each of our Receiving Motion Nodes. */
	foreach(cell, mySlice->children)
	{
		int			numProcs;
		int			childId = lfirst_int(cell);

		elog(DEBUG5, "--Setting up RECEIVING motion node: %d", childId);

		aSlice = (Slice *) list_nth(sliceTable->slices, childId);
		numProcs = list_length(aSlice->primaryProcesses);

		pEntry = createMotionState(childId, numProcs);

		/* let cdbmotion now how many receivers to expect. */
		setExpectedReceivers(childId, numProcs);

		pEntry->myRouteId = 0;           /* DOH:	do i need this for receivers? */

		expectedTotalIncoming += pEntry->numConns;
		}

		gettimeofday(&startTime, NULL);

		/*
		 * now we have kicked off all of the sends.  so we will just sit back
		 * and do some select()'s and process any outgoing connection or
		 * incoming connection as they complete.
		 */
		do
		{
			struct timeval timeout = tval;

			/*
			 * since we might be here for a bit, we should prolly check for
			 * any interrupts that come in.
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
			if (n == 0)			/* timeout */
				continue;
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("Interconnect error setting up: %s", strerror(errno)),
				errdetail("error during select() call (error:%d).", errno)));
			}

			elog(DEBUG5, "select() returned %d ready sockets", n);

			/* is it someone tickling our listener port? */
			if (FD_ISSET(listenerFd, &rrset))
			{
				elog(DEBUG4, "accepting an Incoming Connection ");
				acceptIncomingConnection(&rset, &highsock);
				n--;
			}
			if (!n)
				continue;

			/* check our outgoing connections. */
			if (outgoing_count < expectedTotalOutgoing)
			{
				for (i = 0; i < sendingMotionState->numConns; i++)
				{
					MotionConn *conn = sendingMotionState->conns + i;

					if (FD_ISSET(conn->sockfd, &wwset))
					{
						elog(DEBUG4, "Sending a register message to sockfd: %d", conn->sockfd);
						sendRegisterMessage(sendingMotionState, conn);

						/* no longer interested in this for now. */
						FD_CLR(conn->sockfd, &wset);

						outgoing_count++;
						if (--n == 0)
							break;		/* leave for loop */
					}

					if (conn->hasMirrorConn)
					{
						if (FD_ISSET(conn->mirrorConn->sockfd, &wwset))
						{
							elog(DEBUG4, "Sending a register message to sockfd: %d", conn->mirrorConn->sockfd);
							sendRegisterMessage(sendingMotionState, conn->mirrorConn);

							/* no longer interested in this for now. */
							FD_CLR(conn->mirrorConn->sockfd, &wset);

							outgoing_count++;
							if (--n == 0)
								break;	/* leave for loop */
						}
					}
				}
			}
			if (!n)
				continue;

			/*
			 * check our connections that are accepted'd but no register
			 * message. we don't know which motion node these apply to until
			 * we actually receive the REGISTER message.  this is why they are
			 * all in a single list.
			 *
			 * NOTE: we don't use foreach() here because we want to trim from the
			 * list as we go.
			 */
			cell = list_head(incompleteConns);
			while (cell != NULL)
			{
				MotionConn *conn = (MotionConn *) lfirst(cell);

				/*
				 * we'll get the next cell ready now in case we need to delete
				 * the cell that corresponds to our MotionConn
				 */
				cell = lnext(cell);

				if (FD_ISSET(conn->sockfd, &rrset))
				{
					elog(DEBUG4, "conn # %d has data!", conn->sockfd);

					readRegisterMessage(conn, &incoming_count);
					FD_CLR(conn->sockfd, &rset);
					incompleteConns = list_delete_ptr(incompleteConns, conn);
				}
			}

			elog(DEBUG3, "%d of %d outgoing conns and %d of %d incoming conns completed",
				 outgoing_count, expectedTotalOutgoing, incoming_count, expectedTotalIncoming);
		}
		while ((outgoing_count != expectedTotalOutgoing) || (incoming_count != expectedTotalIncoming));

		/*
		 * if everything really got setup properly then we shouldn't have any
		 * incomplete connections.
		 */
		Assert(list_length(incompleteConns) == 0);

		activated = true;
		elog(DEBUG3, "FINISHED ACTIVATING ROUTES. (out: %d , in: %d)", outgoing_count, incoming_count);

		gettimeofday(&snapTime, NULL);
		elog(DEBUG3, "exiting SetupInterconnect @%d time is %ld.%ld",
		  __LINE__, (long int) snapTime.tv_sec, (long int) snapTime.tv_usec);
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
		void		TeardownInterconnect(void)
		{
			ListCell   *cell;
			MotionLayerState *pEntry;
			int			i;
			Slice	   *mySlice;
			MotionConn *conn;

			elog(DEBUG3, "Interconnect cleanup.");
			Assert(sliceTable != NULL);

			mySlice = (Slice *) list_nth(sliceTable->slices, sliceId);

			/* multi-phase shutdown, mark all as shutdown */

			/*
			 * phase 1 mark all sockets (senders and receivers) with
			 * shutdown(2)
			 */
			if (mySlice->parentIndex != -1)
			{
				/* cleanup a Sending motion node. */
				elog(DEBUG3, "close down outgoing connections for motion id: %d",
					 mySlice->sliceIndex);
				pEntry = getMotionState(mySlice->sliceIndex);

				for (i = 0; i < pEntry->numConns; i++)
				{
					conn = pEntry->conns + i;

					if (conn == NULL)
						continue;

					if (conn->sockfd != -1)
					{
						shutdown(conn->sockfd, SHUT_WR);
					}

					if (conn->hasMirrorConn && conn->mirrorConn->sockfd != -1)
					{
						shutdown(conn->mirrorConn->sockfd, SHUT_WR);
					}
				}
			}

			/*
			 * cleanup all of our Receiving Motion nodes, these get closed
			 * immediately
			 */
			foreach(cell, mySlice->children)
			{
				Slice	   *aSlice;
				int			childId = lfirst_int(cell);

				aSlice = (Slice *) list_nth(sliceTable->slices, childId);

				pEntry = removeMotionState(aSlice->sliceIndex);
				elog(DEBUG3, "close down connections for motion Id: %d", pEntry->motNodeId);

				/*
				 * receivers know that they no longer care about data from
				 * below ... so we can safely discard data queued in both
				 * directions
				 */
				for (i = 0; i < pEntry->numConns; i++)
				{
					conn = pEntry->conns + i;

					if (conn == NULL)
						continue;

					if (conn->sockfd != -1)
					{
						shutdown(conn->sockfd, SHUT_WR);
						close(conn->sockfd);
						conn->sockfd = -1;
					}
				}
				pfree(pEntry->conns);
			}

			/*
			 * phase 2: wait on all sockets for completion, when complete call
			 * close and free (if required)
			 */
			if (mySlice->parentIndex != -1)
			{
				/* cleanup a Sending motion node. */
				elog(DEBUG3, "close down outgoing connections for motion id: %d",
					 mySlice->sliceIndex);
				pEntry = removeMotionState(mySlice->sliceIndex);

				for (i = 0; i < pEntry->numConns; i++)
				{
					conn = pEntry->conns + i;

					if (conn == NULL)
						continue;

					if (conn->sockfd != -1)
					{
						waitOnSocketDone(conn->sockfd);
						close(conn->sockfd);
						conn->sockfd = -1;
					}

					if (conn->hasMirrorConn && conn->mirrorConn->sockfd != -1)
					{
						waitOnSocketDone(conn->mirrorConn->sockfd);
						close(conn->mirrorConn->sockfd);
						conn->mirrorConn->sockfd = -1;
					}
				}
			}

			/*
			 * If there are clients waiting on our listener; we *must*
			 * disconnect them; otherwise we'll be out of sync with the client
			 * (we may accept them on a subsequent query!)
			 */
			if (listenerFd != -1)
				flushListenerBacklog();

			/*
			 * The incompleteConns list is only used as a staging area for
			 * MotionConns during by SetupInterconnect().  So we only expect
			 * to have entries here if SetupInterconnect() did not finish
			 * correctly.
			 *
			 * NOTE: we don't use foreach() here because we want to trim from the
			 * list as we go.
			 */
			while ((cell = list_head(incompleteConns)) != NULL)
			{
				MotionConn *conn = (MotionConn *) lfirst(cell);

#ifdef AMS_VERBOSE_LOGGING
				elog(LOG, "Found incomplete conn. length %d", list_length(incompleteConns));
#endif

				if (conn->sockfd != -1)
				{
					shutdown(conn->sockfd, SHUT_RDWR);
					close(conn->sockfd);
					conn->sockfd = -1;
				}

				/*
				 * The list operations are kind of confusing (see list.c), we
				 * could alternatively write the following line as:
				 *
				 * incompleteConns = list_delete_cell(incompleteConns, cell,
				 * NULL); or incompleteConns =
				 * list_delete_first(incompleteConns); or incompleteConns =
				 * list_delete_ptr(incompleteConns, conn)
				 */
				incompleteConns = list_delete(incompleteConns, conn);
			}

			incompleteConns = NIL;

			if (mlsStates != NULL)
			{
				pfree(mlsStates);
				mlsStates = NULL;
				mlsSize = 0;
			}

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
 *	  System picked TCP port that listener is using.
 */
		static int	listenerSetup(void)
		{
			struct sockaddr_in addr;
			socklen_t	addrlen;

			if ((listenerFd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								errmsg("Interconnect Error: Could not setup listener socket: %s", strerror(errno)),
				errdetail("error during socket() call (error:%d).", errno)));
			}

			memset(&addr, 0, sizeof(addr));
			serv_addr.sin_family = AF_INET;
			serv_addr.sin_addr.s_addr = INADDR_ANY;
			serv_addr.sin_port = htons(0);		/* system picks port */

			/*
			 * we let the system pick the TCP port here so we don't have to
			 * manage port resources ourselves.
			 */
			if (bind(listenerFd, (struct sockaddr *) & serv_addr, sizeof(serv_addr)) < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("Interconnect Error: Could not setup listener socket"),
				  errdetail("error during bind() call (error:%d).", errno)));

			}

			if (listen(listenerFd, LISTEN_BACK_LOG) < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								errmsg("Interconnect Error: Could not setup listener socket: %s", strerror(errno)),
				errdetail("error during listen() call (error:%d).", errno)));
			}


			addrlen = sizeof(struct sockaddr_in);

			if (getsockname(listenerFd, (struct sockaddr *) & addr, &addrlen) < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
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
 *	 motNodeID - motion node ID for this MotionState.
 *
 *	 numConns  - number of connections for this motion node. These could be all
 *				 incoming or outgoing connections depending on if this is a
 *				 Sending or Receiving motion node (motNodeID).
 *
 * RETURNS
 *	 An empty and initialized MotionState struct for the given motion node. If
 *	 a MotionState struct is already registered for the motNodeID an ERROR is
 *	 thrown.
 */
		static MotionLayerState *createMotionState(int16 motNodeID, int numConns)
		{
			MotionLayerState *pEntry;
			int			i;

			if (motNodeID > mlsSize)
			{
				/* increase size of our table */
				MotionLayerState *newTable;

				newTable = repalloc(mlsStates, motNodeID * sizeof(MotionLayerState));
				if (newTable == NULL)
				{
					ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
									errmsg("Could not re-allocate motion layer state table.")));
				}
				mlsStates = newTable;
				/* zero-out the new piece at the end */
				memset(&mlsStates[mlsSize], 0, (motNodeID - mlsSize) * sizeof(MotionLayerState));
				mlsSize = motNodeID;
			}

			pEntry = &mlsStates[motNodeID - 1];

			if (pEntry->valid)
			{
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								errmsg("Interconect Error: A HTAB entry for motion node %d already exists.", motNodeID),
								errdetail("conns %p numConns %d first sock %d highreadsock %d", pEntry->conns, pEntry->numConns, pEntry->conns[0].sockfd, pEntry->highReadSock)));
			}

			pEntry->valid = true;

			pEntry->motNodeId = motNodeID;
			pEntry->numConns = numConns;
			pEntry->highReadSock = 0;
			pEntry->conns = palloc0(pEntry->numConns * sizeof(MotionConn));
			if (pEntry->conns == NULL)
			{
				ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
								errmsg("Could not allocate space for interconnect connection list")));
			}

			for (i = 0; i < numConns; i++)
			{
				pEntry->conns[i].sockfd = -1;
			}

			FD_ZERO(&(pEntry->readSet));

			return pEntry;
		}


/* Function getMotionState() is used to retrieve a MotionLayerState from the
 * hashtab hashtable based on the given motNodeID.	This function should only
 * be called after createMotionState() for a given motion node has been called
 * first.
 *
 * PARAMETERS
 *
 *	 motNodeID - motion node ID to lookup the MotionLayerState.
 *
 * RETURNS
 *	 The MotionLayerState struct for the given motNodeID.
 */
		static inline MotionLayerState *getMotionState(int16 motNodeID)
		{
			MotionLayerState *pEntry = NULL;

			if (motNodeID > mlsSize || !mlsStates[motNodeID - 1].valid)
			{
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								errmsg("Interconect Error: Unexpected Motion Node Id: %d.  This means"
				" a motion node that wasn't setup is requesting interconnect"
									   " resources.", motNodeID)));
			}
			else
				pEntry = &mlsStates[motNodeID - 1];

			return pEntry;
		}


/* Function removeMotionState() is used to remove a MotionLayerState struct from
 * the hashtab hashtable.
 *
 * This should only be called after createMotionState().
 *
 * PARAMETERS
 *
 *	 motNodeID - motion node ID to lookup the MotionLayerState.
 *
 * RETURNS
 *	 The MotionLayerState that was removed from the hashtab hashtable.
 */
		static MotionLayerState *removeMotionState(int16 motNodeID)
		{
			MotionLayerState *pEntry = NULL;

			if (motNodeID > mlsSize || !mlsStates[motNodeID - 1].valid)
			{
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
								errmsg("Interconect Error: Unexpected Motion Node Id: %d. During remove.",
									   motNodeID)));
			}
			else
			{
				mlsStates[motNodeID - 1].valid = false;
				pEntry = &mlsStates[motNodeID - 1];
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
 *	 mySlice  - Slice that this process is a member of.
 *
 *	 highsock - Current highest valued socket filedescriptor (used for select())
 *
 *	 wset	  - fd_set for writes (used for select())
 *
 *	 connsStarted - tell caller how many connections are expected
 *
 * RETURNS
 *	 Initialized MotionLayerState for the Sending Motion Node Id.
 */
		static MotionLayerState *startConnections(Slice * mySlice, int *highsock, fd_set *wset, int *connsStarted)
		{
			MotionLayerState *pEntry;
			MotionConn *conn;
			Slice	   *recvSlice;
			int			i,
						motNodeId;
			CdbProcess *cdbProc;


			/* for sending motion nodes... motionNodeId = sliceIndex. */
			motNodeId = mySlice->sliceIndex;

#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "Interconnect - Starting Outgoing Connections (I'm slice %d parent is %d)",
				 mySlice->sliceIndex, mySlice->parentIndex);
#endif

			recvSlice = (Slice *) list_nth(sliceTable->slices, mySlice->parentIndex);

			Assert(recvSlice->mirrorProcesses == NIL || (list_length(recvSlice->primaryProcesses) == list_length(recvSlice->mirrorProcesses)));

			pEntry = createMotionState(motNodeId, list_length(recvSlice->primaryProcesses));

			/*
			 * Setup an outgoing socket to the right place for each of our
			 * outbound connections for the primary Connections.
			 */
			for (i = 0; i < pEntry->numConns; i++)
			{
				conn = pEntry->conns + i;
				conn->sockfd = -1;

				cdbProc = (CdbProcess *) list_nth(recvSlice->primaryProcesses, i);

				setupOutgoingConnection(conn, cdbProc, highsock, wset);
				(*connsStarted)++;

				/* setup mirror connection if not null */
				if (recvSlice->mirrorProcesses != NIL)
				{
					cdbProc = (CdbProcess *) list_nth(recvSlice->mirrorProcesses, i);
					if (cdbProc != NULL)
					{
						conn->mirrorConn = palloc(sizeof(MotionConn));
						if (conn->mirrorConn == NULL)
						{
							ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
											errmsg("Could not allocate space for mirror connection detail.")));
						}
						conn->mirrorConn->sockfd = -1;
						conn->hasMirrorConn = true;

						setupOutgoingConnection(conn->mirrorConn, cdbProc, highsock, wset);
						(*connsStarted)++;
					}
				}
			}

			return pEntry;
		}

		static void setupOutgoingConnection(MotionConn * conn, CdbProcess * cdbProc, int *highsock, fd_set *wset)
		{
			struct sockaddr_in addr;
			int			n;

			conn->sockfd = socket(AF_INET, SOCK_STREAM, 0);
			conn->pBuff = palloc(Mpp_max_packet_size);
			if (conn->pBuff == NULL)
			{
				ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
								errmsg("Could not allocate space for interconnect tx-buffer")));
			}
			conn->msgSize = PACKET_HEADER_SIZE;
			conn->tupleCount = 0;
			conn->stillActive = true;
			conn->remoteHost = cdbProc->listenerAddr;
			conn->remotePort = cdbProc->listenerPort;
			conn->remoteContentId = cdbProc->contentid;
			conn->hasMirrorConn = false;

			if (conn->sockfd < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
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
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
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

			/*
			 * Since our socket is set to non-blocking, then connect can
			 * return immediately with an error of EINPROGRESS but the TCP
			 * connection is still continuing to be made.  we will later use
			 * select() to find out when its actually done.
			 */
			if ((n = connect(conn->sockfd, (struct sockaddr *) & addr, sizeof(addr))) < 0)
			{
				if (errno != EINPROGRESS)
				{
					/*
					 * TODO: would be nice to read the errno and try and
					 * provide more useful info as to why this happened.
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

			elog(DEBUG4, "Interconnect started outgoing connection to CdbProcess with contentId=%d "
				 "to: '%s':%d with sockfd: %d", cdbProc->contentid, cdbProc->listenerAddr,
				 cdbProc->listenerPort, conn->sockfd);
		}


/* Function sendRegisterMessage() used to send a Register message to the
 * remote destination on the other end of the provided conn.
 *
 * PARAMETERS
 *
 *	 pEntry - MotionLayerState.
 *	 conn	- MotionConn to send message out on.
 */
		static void sendRegisterMessage(MotionLayerState * pEntry, MotionConn * conn)
		{
			int			n;
			int16		netval;
			int32		netval2;
			RegisterMessage regMsg;
			int			msglen = sizeof(RegisterMessage);
			uint8	   *msg = (uint8 *) &regMsg;

#ifdef AMS_VERBOSE_LOGGING
			print_connection(conn->sockfd, "send register message");
#endif
			/* motNodeId */
			netval = htons(pEntry->motNodeId);
			memcpy(msg + 0, &netval, 2);

			/* srcRouteId */
			netval = htons(pEntry->myRouteId);
			memcpy(msg + 2, &netval, 2);

			/* srcContentId */
			netval = htons(Mpp_segment);
			memcpy(msg + 4, &netval, 2);

			/* srcListenerPort */
			netval2 = htonl(listenerPort);
			memcpy(msg + 6, &netval2, 4);

#ifdef AMS_VERBOSE_LOGGING
			/* srcPid */
			netval2 = htonl(getpid());
			memcpy(msg + 10, &netval2, 4);
#endif

			/*
			 * Since our message is very small and we should have at least
			 * some dedicated TCP buffer space.  so we should be able to use a
			 * blocking send() call to write it out.
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
 *	 conn - MotionConn to read the register messagefrom.
 *	 incoming_count - ptr to number of incoming connections that have been set
 *					  up so far.
 *
 */
		static void readRegisterMessage(MotionConn * conn, int *incoming_count)
		{
			MotionLayerState *pEntry;
			MotionConn *newConn;
			RegisterMessage msg;
			int			n;
			int			bytesRead = 0;
			CdbProcess *proc;

			int			msgSize = sizeof(RegisterMessage);
			RegisterMessage regMsg;
			uint8	   *buff = (uint8 *) &regMsg;

#ifdef AMS_VERBOSE_LOGGING
			print_connection(conn->sockfd, "register message on");
#endif

			/* loop till we read in the entire RegisterMessage. */
			while (bytesRead < msgSize)
			{
				/* see if user canceled and stuff like that */
				CHECK_FOR_INTERRUPTS();

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
				Assert(bytesRead <= (int) sizeof(RegisterMessage));
			}

			msg.motNodeId = getInt16(buff + 0);
			msg.srcRouteId = getInt16(buff + 2);
			msg.srcContentId = getInt16(buff + 4);
			msg.srcListenerPort = getInt32(buff + 6);

#ifdef AMS_VERBOSE_LOGGING
			msg.srcPid = getInt32(buff + 10);

			elog(DEBUG3, "Read a Register Message: (motNodeID: %d, srcRouteId: %d, "
				 "srcContentId: %d, srcListenerPort: %d srcPid: %d",
				 msg.motNodeId, msg.srcRouteId, msg.srcContentId, msg.srcListenerPort, msg.srcPid);
#endif

			pEntry = getMotionState(msg.motNodeId);

			/*
			 * store the MotionConn in the right slot, make sure no one is
			 * there.
			 */
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

			newConn->sockfd = conn->sockfd;
			newConn->pBuff = palloc(Mpp_max_packet_size);
			if (conn->pBuff == NULL)
			{
				ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
								errmsg("Could not allocate space for interconnect rx-buffer")));
			}
			newConn->msgSize = 0;
			newConn->recvBytes = 0;
			newConn->msgPos = 0;
			newConn->tupleCount = 0;
			newConn->stillActive = true;
			newConn->remoteHost = proc->listenerAddr;
			newConn->remotePort = msg.srcListenerPort;
			newConn->remoteContentId = msg.srcContentId;
			newConn->hasMirrorConn = false;

			FD_SET(newConn->sockfd, &(pEntry->readSet));

			if (newConn->sockfd > pEntry->highReadSock)
				pEntry->highReadSock = newConn->sockfd;

#ifdef AMS_VERBOSE_LOGGING
			dumpEntryConnections(pEntry);
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
 *	rset - readset so we can register readinterest in the new socket once we
 *		   accept() it.
 *
 *	highsock - ptr to highest socket filedescriptor. (needed for select())
 *
 */
		static void acceptIncomingConnection(fd_set *rset, int *highsock)
		{
			int			newsockfd;
			struct sockaddr_in addr;
			socklen_t	addrlen;
			MotionConn *conn;

			memset(&addr, 0, sizeof(addr));
			addrlen = sizeof(addr);

			if ((newsockfd = accept(listenerFd, (struct sockaddr *) & addr, &addrlen)) < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
					 errmsg("Interconnect error accepting a new connection"),
				 errdetail("error during accept() call (error:%d)", errno)));
			}


			/* make socket non-blocking BEFORE we connect. */
			if (!pg_set_noblock(newsockfd))
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
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
			if (conn == NULL)
			{
				ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
								errmsg("Could not allocate space for incoming connection detail.")));
			}
			conn->sockfd = newsockfd;
			conn->pBuff = palloc(Mpp_max_packet_size);
			if (conn->pBuff == NULL)
			{
				ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("Could not allocate space for incoming rx-buffer")));
			}
			conn->msgSize = 0;
			conn->recvBytes = 0;
			conn->msgPos = 0;
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
 *	 conn - MotionConn that the tcItem is to be sent to.
 *	 tcItem - message to be sent.
 */
		static bool sendChunk(MotionConn * conn, TupleChunkListItem tcItem)
		{
			Assert(conn->msgSize > 0);

#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "sendChunk: msgSize %d this chunk length %d", conn->msgSize, tcItem->chunk_length);
#endif

			if (conn->msgSize + tcItem->chunk_length > Mpp_max_packet_size)
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
 * out to the network.	This function blocks until the entire contents have
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
 *	 conn - MotionConn whose buffer needs to be flushed.
 *
 */
		static bool flushBuffer(MotionConn * conn)
		{
			char	   *sendptr;

			int			n,
						sent = 0;
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
			*(uint32 *) conn->pBuff = htonl(conn->msgSize);

			/* now send message */
			sendptr = (char *) conn->pBuff;
			sent = 0;
			do
			{
				struct timeval timeout;

				/* check for stop message before sending anything  */
				timeout.tv_sec = 0;
				timeout.tv_usec = 0;
				FD_ZERO(&rset);
				FD_SET(conn->sockfd, &rset);
				n = select(conn->sockfd + 1, &rset, NULL, NULL, &timeout);
				/* handle errors at the write call, below */
				if (n > 0 && FD_ISSET(conn->sockfd, &rset))
				{
#ifdef AMS_VERBOSE_LOGGING
					print_connection(conn->sockfd, "stop from");
#endif
					/* got a stop message */
					conn->stillActive = false;
					return false;
				}

				if ((n = write(conn->sockfd, sendptr + sent, conn->msgSize - sent)) < 0)
				{
					CHECK_FOR_INTERRUPTS();
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

							/*
							 * as a sender... if there is something to read...
							 * it must mean its a StopSendingMessage.  we
							 * don't even bother to read it.
							 */
							if (FD_ISSET(conn->sockfd, &rset))
							{
#ifdef AMS_VERBOSE_LOGGING
								print_connection(conn->sockfd, "stop from");
#endif
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
 *	 conn - MotionConn to read the packet from.
 *
 */
		static inline void readPacket(MotionConn * conn)
		{
			int			n,
						bytesRead = conn->recvBytes;
			bool		gotHeader = false,
						gotPacket = false;
			fd_set		rset;

#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "readpacket: (fd %d) (max %d) outstanding bytes %d", conn->sockfd, Mpp_max_packet_size, conn->recvBytes);
#endif

			/* do we have a complete message waiting to be processed ? */
			if (conn->recvBytes >= PACKET_HEADER_SIZE)
			{
				conn->msgSize = getInt32(conn->msgPos);
				gotHeader = true;
				if (conn->recvBytes >= conn->msgSize)
				{
#ifdef AMS_VERBOSE_LOGGING
					elog(DEBUG3, "readpacket: returning previously read data (%d)", conn->recvBytes);
#endif
					return;
				}
			}

			/*
			 * partial message waiting in recv buffer! Move to head of buffer:
			 * eliminate the slack (which will always be at the beginning) in
			 * the buffer
			 */
			if (conn->recvBytes != 0)
				memcpy(conn->pBuff, conn->msgPos, conn->recvBytes);

			conn->msgPos = conn->pBuff;

#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "readpacket: %s on previous call msgSize %d", gotHeader ? "got header" : "no header", conn->msgSize);
#endif

			while (!gotPacket && bytesRead < Mpp_max_packet_size)
			{
				/*
				 * we read at the end of the buffer, we've eliminated any
				 * slack above
				 */
				if ((n = read(conn->sockfd, conn->pBuff + bytesRead,
							  Mpp_max_packet_size - bytesRead)) < 0)
				{
					CHECK_FOR_INTERRUPTS();

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
								CHECK_FOR_INTERRUPTS();

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
						CHECK_FOR_INTERRUPTS();
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
#ifdef AMS_VERBOSE_LOGGING
					elog(DEBUG3, "readpacket(); breaking in while (fd %d) recvBytes %d msgSize %d", conn->sockfd, conn->recvBytes, conn->msgSize);
					print_connection(conn->sockfd, "interconnect error on");
#endif
					CHECK_FOR_INTERRUPTS();
					ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
									errmsg("Interconnect error: connection closed prematurely."),
						   errdetail("error during read() call (error:%d).\n"
						   "From Remote Connection: contentId=%d at '%s':%d",
							  errno, conn->remoteContentId, conn->remoteHost,
									 conn->remotePort)));
					break;
				}
				else
				{
					bytesRead += n;

					if (!gotHeader && bytesRead >= PACKET_HEADER_SIZE)
					{
						/* got the header */
						conn->msgSize = getInt32(conn->msgPos);
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
 *	 pEntry - MotionLayerState context that contains everything we need to send.
 *	 tcItem - TupleChunk to send.
 */
		static int	doBroadcast(MotionLayerState * pEntry, TupleChunkListItem tcItem)
		{
			MotionConn *conn;
			int			i,
						inactive = 0;

			/* add our tcItem to each of the outgoing buffers. */
			for (i = 0; i < pEntry->numConns; i++)
			{
				conn = pEntry->conns + i;
				/* only send to still interested receivers. */
				if (conn->stillActive)
				{
					sendChunk(conn, tcItem);
					if (!conn->stillActive)
						inactive++;
				}

				if (conn->hasMirrorConn && conn->mirrorConn->stillActive)
				{
					sendChunk(conn->mirrorConn, tcItem);
					if (!conn->mirrorConn->stillActive)
						inactive++;
				}
			}

			return inactive;
		}


/* Function findMyRouteId() is used to search through the given slice and find
 * the position in the slice->primaryProcesses list where "we" are located at.
 * This is done by matching up the contentid and listenerPort.
 *
 * PARAMETERS
 *
 *	 slice - Slice to search through.
 *
 * RETURN
 *	 An int which is "our" position in the slice->primaryProcesses list.
 */
		static int	findMyRouteId(Slice * slice)
		{
			ListCell   *cell;
			int			id = 0;

			foreach(cell, slice->primaryProcesses)
			{
				CdbProcess *proc = (CdbProcess *) lfirst(cell);

#ifdef AMS_VERBOSE_LOGGING
				elog(DEBUG3, "Checking if CdbProc is our own: segid: (ours is %d ==  CdbProc's is %d) "
					 " listenerPort:  (ours is %d == CdbProc's is %d)",
					 Mpp_segment, proc->contentid,
					 listenerPort, proc->listenerPort);
#endif

				if (proc->contentid == Mpp_segment &&
					proc->listenerPort == listenerPort)
					return id;

				id++;
			}

			/* DOH: dump more info here */
			elog(ERROR, "Could not find my CdbProcess in slice #: %d", slice->sliceIndex);

			return -1;
		}



/* Function findCdbProcess() takes the info in the RegisterMessage and looks up
 * the CdbProcess that corresponds to it.  Part of the reason for doing this is
 * to verify that the RegisterMessage is a valid one.
 *
 * PARAMETERS
 *
 *	 msg - RegisterMessage that contains the info of which CdbProcess we are
 *			looking for.
 *
 * RETURNS
 *	 The CdbProcess that matches with the info in the RegisterMessage.
 */
		static CdbProcess *findCdbProcess(RegisterMessage * msg)
		{
			CdbProcess *proc = NULL;
			Slice	   *slice;
			ListCell   *cell;
			int			count = 0;
			bool		found = false;

			if (list_length(sliceTable->slices) < msg->motNodeId)
			{
				ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("Interconnect Error: Received a Register Message with "
					   "unknown motion Node ID: %d", msg->motNodeId),
				  errdetail("Could not find a Slice in our slice table with "
							"motion node ID: %d", msg->motNodeId)));
			}


			slice = (Slice *) list_nth(sliceTable->slices, msg->motNodeId);


			foreach(cell, slice->primaryProcesses)
			{
				proc = (CdbProcess *) lfirst(cell);

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
						  msg->motNodeId, msg->srcRouteId, msg->srcContentId,
										  msg->srcListenerPort)));
			}

			return proc;
		}

		void		dumpEntryConnections(MotionLayerState * pEntry)
		{
			int			i;

			elog(DEBUG3, "----------------pEntry motNodeID: %d conn dump------------------", pEntry->motNodeId);
			for (i = 0; i < pEntry->numConns; i++)
			{
				MotionConn *conn = pEntry->conns + i;

				elog(DEBUG3, "conn#%d (sockfd=%d, remote host= '%s' port=%d, contentId: %d)",
					 i, conn->sockfd, conn->remoteHost, conn->remotePort, conn->remoteContentId);

				if (conn->hasMirrorConn)
				{
					conn = conn->mirrorConn;
					elog(DEBUG3, "conn#%d MIRROR (sockfd=%d, remote host= '%s' port=%d, contentId: %d)",
						 i, conn->sockfd, conn->remoteHost, conn->remotePort, conn->remoteContentId);

				}

			}
			elog(DEBUG3, "----------------------------------------------------------------");
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
 *	 motNodeID - motion node identifier
 */
		void		doSendStopMessage(int16 motNodeID)
		{
			MotionLayerState *pEntry;
			MotionConn *conn;
			int			i;
			char		m = 'S';
			ssize_t		written;

			pEntry = getMotionState(motNodeID);
			Assert(pEntry != NULL);

			/*
			 * Note: we're only concerned with receivers here, so mirrors
			 * don't matter
			 */
			for (i = 0; i < pEntry->numConns; i++)
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
						/*
						 * how can this happen ? the kernel buffer should be
						 * empty in the send direction
						 */
						elog(LOG, "SendStopMessage: failed on write %d", errno);
					}
				}
				/* CRITICAL TO AVOID DEADLOCK */
				DeregisterReadInterest(motNodeID, i);
			}
		}

		void		print_connection(int fd, const char *msg)
		{
			struct sockaddr_in local,
						remote;
			socklen_t	len;

			len = sizeof(remote);
			if (getpeername(fd, (struct sockaddr *) & remote, &len) < 0)
			{
				elog(ERROR, "print_connection(): can't get peername for sock %d err: %s",
					 fd, strerror(errno));
			}

			len = sizeof(local);
			if (getsockname(fd, (struct sockaddr *) & local, &len) < 0)
			{
				elog(ERROR, "print_connection(): can't get localname for sock %d err: %s",
					 fd, strerror(errno));
			}

			elog(DEBUG2, "%s: w/ports (%d/%d)",
				 msg, ntohs(local.sin_port), ntohs(remote.sin_port));
		}

		static void flushListenerBacklog(void)
		{
			int			pendingConn,
						newfd;
			fd_set		rset;
			struct timeval timeout;
			struct sockaddr_in peer;
			socklen_t	peerlen;

			do
			{
				FD_ZERO(&rset);
				FD_SET(listenerFd, &rset);
				timeout.tv_sec = 0;
				timeout.tv_usec = 0;

				pendingConn = select(listenerFd + 1, &rset, NULL, NULL, &timeout);
				if (pendingConn > 0)
				{
#ifdef AMS_VERBOSE_LOGGING
					elog(DEBUG3, "flushListenerBacklog(): accepting and closing client conn");
#endif
					peerlen = sizeof(peer);
					newfd = accept(listenerFd, (struct sockaddr *) & peer, &peerlen);
					if (newfd < 0)
					{
						elog(DEBUG1, "flushListenerBacklog: %s during accept", strerror(errno));
						continue;
					}
					/* shutdown this socket */
					shutdown(newfd, SHUT_RDWR);
					close(newfd);
				}
				else if (pendingConn < 0 && errno != EINTR)
				{
					elog(ERROR, "%s during listener cleanup", strerror(errno));
				}

				/*
				 * now we either loop through for another check (on EINTR or
				 * if we cleaned one client) or we're done
				 */
			}
			while (pendingConn != 0);
		}

/*
 * Wait for our peer to close the socket (at which point our select(2)
 * will tell us that the socket is ready to read, and the socket-read
 * will only return 0.
 *
 * This works without the select, but burns tons of CPU doing nothing
 * useful.
 */
		static void waitOnSocketDone(int fd)
		{
			int			count,
						n;
			char		buf;
			struct timeval timeout;
			fd_set		rset;

			for (;;)
			{
				CHECK_FOR_INTERRUPTS();

				FD_ZERO(&rset);
				FD_SET(fd, &rset);
				timeout = tval;
				n = select(fd + 1, &rset, NULL, NULL, &timeout);
				if (n == 0 || (n < 0 && errno == EINTR))
				{
					continue;
				}
				else if (n < 0)
				{
					/*
					 * Something unexpected, but probably not horrible warn
					 * and return
					 */
					elog(WARNING, "TeardownInterconnect: waitOnSocketDone select (%d)", errno);
					break;
				}

				/* ready to read. */
				count = recv(fd, &buf, sizeof(buf), 0);
				if (count == 0) /* done ! */
					break;
				if (count > 0 || (count < 0 && errno == EAGAIN))
					continue;
				/* Some other kind of error happened */
				if (errno == EINTR)
					continue;

				/*
				 * Something unexpected, but probably not horrible warn and
				 * return
				 */
				elog(WARNING, "TeardownInterconnect: waitOnSocketDone (%d)", errno);
				break;
			}
			return;
		}
