/*-------------------------------------------------------------------------
 * ml_ipc.c
 *	   Motion Layer IPC Layer.
 *
 * Copyright (c) 2005-2006, Green Plum
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
#include <limits.h>
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

/*
#define AMS_VERBOSE_LOGGING
*/

/*=========================================================================
 * STRUCTS
 */

 /*
  * backlog for listen() call: it is important that this be something like a
  * good match for the maximum number of QEs. Slow insert performance will
  * result if it is too low.
  */
#define LISTEN_BACKLOG 64
#define MAX_BIND_RETRIES 12
#define CONNECT_RETRY_MS    5000

#define getInt16(pNetData)	  ntohs(*((int16 *)(pNetData)))
#define getInt32(pNetData)	  ntohl(*((int32 *)(pNetData)))

typedef enum MotionConnState
{
    mcsNull,
    mcsAccepted,
    mcsSetupOutgoingConnection,
    mcsConnecting,
    mcsRecvRegMsg,
    mcsSendRegMsg,
    mcsStarted,
} MotionConnState;

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

    MotionConnState state;

    int         wakeup_ms;

    CdbProcess *cdbProc;
    int			remoteContentId;
    char        remoteHostAndPort[64];
    char        localHostAndPort[64];

}	MotionConn;

/* used to organize all of the information for a given motion node.
 *
 */
typedef struct MotionLayerState
{
	int         motNodeId;
	bool		valid;

	/* Connection array: first the primaries, then the mirrors (if needed) */
    MotionConn *conns;
	int			numConns;               /* all, including mirrors if present */
    int         numPrimaryConns;        /* does not include mirrors */

	/*
	 * used for receiving. to select() from a set of interesting MotionConns
	 * to see when data is ready to be read.  When the incoming connections
	 * are established, read interest is turned on.  It is turned off when an
	 * EOS (End of Stream) message is read.
	 */
	fd_set		readSet;

	/* highest file descriptor in the readSet. */
	int			highReadSock;
    int         scanStart;

    /* slice table entries */
    Slice      *sendSlice;
    Slice      *recvSlice;

    /* setup info */
    int         outgoingPortRetryCount;

}	MotionLayerState;


/*
 * Registration message
 *
 * Upon making a connection, the sender sends a registration message to
 * identify itself to the receiver.  A lot of the fields are just there
 * for validity checking.
 */
typedef struct RegisterMessage
{
	int32       msgBytes;
    int32       recvSliceIndex;
    int32       sendSliceIndex;
	int32       srcContentId;
	int32		srcListenerPort;
	int32		srcPid;
    int32       srcSessionId;
    int32       srcCommandCount;
}	RegisterMessage;


/*=========================================================================
 * GLOBAL STATE VARIABLES
 */

/* Socket file descriptor for the listener. */
int			listenerFd;

/* Socket file descriptor for the sequence server. */
int			seqserverFd = -1;
char		*seqServerHost;
int			seqServerPort;

/* Use an array to store our motion-layer-state table. */
int			mlsSize = 0;
MotionLayerState *mlsStates = NULL;

#define MLS_INITIAL_SIZE (10)

/*
 * Outgoing port assignment
 *
 * To reserve a port number for outgoing connections, we open a dummy
 * listening socket.  Nobody connects to this socket.
 */
int         portReservationFd = -1;
int         outgoingPort = 0;

/* our timeout value for select() and other socket operations. */
struct timeval tval;

List	   *incompleteConns = NIL;

/* keeps track of if we've "activated" connections via SetupInterconnect(). */
bool		activated;

/*slice table stuff. */
SliceTable *sliceTable = NULL;
int			sliceId;

bool		teardownActive = false;

#define ML_CHECK_FOR_INTERRUPTS() \
		do {if (!teardownActive && InterruptPending) CHECK_FOR_INTERRUPTS(); } while (0)

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static void setupListeningSocket(int backlog, int *listenerSocketFd, int *listenerPort);

static MotionLayerState *createMotionState(Slice *sendSlice, Slice *recvSlice,
                                           int numPrimaryConns, int numMirrorConns);
static inline MotionLayerState *getMotionState(int motNodeID);
static MotionLayerState *removeMotionState(int16 motNodeID);

static void setupSeqServerConnection(char *hostname, int port);

static MotionLayerState *startOutgoingConnections(Slice *sendSlice, int *pOutgoingCount);
static void setupOutgoingConnection(MotionLayerState *pEntry, MotionConn *conn);
static void updateOutgoingConnection(MotionLayerState *pEntry, MotionConn *conn, int errnoSave);
static int  getOutgoingPort(MotionLayerState *pEntry);
static void zapOutgoingPort(MotionLayerState *pEntry);

static void sendRegisterMessage(MotionLayerState * pEntry, MotionConn * conn);
static bool readRegisterMessage(MotionConn *conn);
static MotionConn *acceptIncomingConnection(void);

static bool sendChunk(MotionConn * conn, TupleChunkListItem tcItem, int16 motionId);
static inline void readPacket(MotionConn * conn);
static inline int doBroadcast(MotionLayerState * pEntry, TupleChunkListItem tcItem);

static bool flushBuffer(MotionConn * conn, int16 motionId);

static void	dumpEntryConnections(int elevel, MotionLayerState *pEntry);
static void	print_connection(int fd, const char *msg);
static void format_fd_set(StringInfo buf, int nfds, fd_set *fds, char* pfx, char *sfx);
static char *format_sockaddr(struct sockaddr *sa, char* buf, int bufsize);

static void waitOnSocketDone(int);
static void flushListenerBacklog(void);

static inline TupleChunkListItem RecvTupleChunk(MotionConn * conn);


/*=========================================================================
 * INLINE FUNCTIONS
 */

static inline int
timeval_delta_ms(struct timeval t1, struct timeval t0)
{
    int tdiff = (t1.tv_usec - t0.tv_usec + 500) / 1000;

    if (t1.tv_sec != t0.tv_sec)
        tdiff += 1000 * (t1.tv_sec - t0.tv_sec);
    return  tdiff;
}

static inline int
gettime_elapsed_ms(struct timeval t0)
{
    struct timeval  t1;

    gettimeofday(&t1, NULL);
    return timeval_delta_ms(t1, t0);
}


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
	seqserverFd = -1;

	/* Listen for connections from qExecs. */
	setupListeningSocket(LISTEN_BACKLOG, &listenerFd, &Mpp_listener_port);

    elog(LOG, "Interconnect listening on tcp port %d", Mpp_listener_port);
}


/* See ml_ipc.h */
void
CleanUpMotionLayerIPC(void)
{

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "CleanUpMotionLayerIPC()");
#endif

	/* close down the Interconnect listener socket. */
    if (listenerFd >= 0)
        close(listenerFd);

    if (portReservationFd >= 0)
        close(portReservationFd);

	/* be safe and reset global state variables. */
	activated = false;
	Mpp_listener_port = 0;
	listenerFd = -1;
    portReservationFd = -1;
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

	Insist(activated);

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "SendTupleChunkToAMS(motNodeID: %d, targetRoute: %d)", motNodeID, targetRoute);
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
		elog(DEBUG5, "SendTupleChunkToAMS: chunk length %d", currItem->chunk_length);
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
				sendChunk(conn, currItem, motNodeID);
				if (!conn->stillActive)
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

	Insist(activated || teardownActive);

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "SendEosToAMS() Entering");
#endif

	pEntry = getMotionState(motNodeID);

	/* we want to add our tcItem onto each of the outgoing buffers */
	doBroadcast(pEntry, tcItem);

	/* now flush all of the buffers. */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;
		if (conn->sockfd >= 0 &&
            conn->state == mcsStarted)
			flushBuffer(conn, motNodeID);
	}

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "SendEosToAMS() Leaving");
#endif
}

/* See ml_ipc.h */
TupleChunkListItem
RecvTupleChunkFromAny(int16 motNodeID, int16 *srcRoute)
{
	MotionLayerState *pEntry;
	MotionNodeEntry  *pMNEntry;
	MotionConn *conn;
	TupleChunkListItem tcItem;
	fd_set		rset;
	int			n,
				i,
				index;
	bool		skipSelect = false;

	Insist(activated);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "RecvTupleChunkFromAny(motNodeId=%d)", motNodeID);
#endif

	pEntry = getMotionState(motNodeID);
	pMNEntry = getMotionNodeEntry(motNodeID, "RecvTupleChunkFromAny");

	do
	{
		struct timeval timeout = tval;

		/* make sure we check for these. */
		ML_CHECK_FOR_INTERRUPTS();

		memcpy(&rset, &(pEntry->readSet), sizeof(fd_set));

		/*
		 * since we may have data in a local buffer, we may be able to
		 * short-circuit the select() call (and if we don't do this we may
		 * wait when we have data ready, since it has already been read)
		 */
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->sockfd >= 0 &&
                FD_ISSET(conn->sockfd, &rset) &&
                conn->recvBytes != 0)
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
		pMNEntry->sel_rd_wait += (tval.tv_sec - timeout.tv_sec) * 1000000 + (tval.tv_usec - timeout.tv_usec);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error receiving an incoming packet."),
			                errdetail("%m%s", "select")
                    ));
		}
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "RecvTupleChunkFromAny() select() returned %d ready sockets", n);
#endif
	} while (n < 1);

	/*
	 * We scan the file descriptors starting from where we left off in the
	 * last call (don't continually poll the first when others may be ready!).
	 */
    index = pEntry->scanStart;
	for (i = 0; i < pEntry->numConns; i++, index++)
	{
		/*
		 * avoid division ? index = ((scanStart + i) % pEntry->numConns);
		 */
		if (index >= pEntry->numConns)
			index = 0;

		conn = pEntry->conns + index;

#ifdef AMS_VERBOSE_LOGGING
		if (!conn->stillActive)
		{
			elog(LOG, "RecvTupleChunkFromAny: trying to read on inactive socket %d", conn->sockfd);
		}
#endif

		if (conn->sockfd >= 0 &&
            FD_ISSET(conn->sockfd, &rset))
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG5, "RecvTupleChunkFromAny() (fd %d) %d/%d", conn->sockfd, motNodeID, index);
#endif
			tcItem = RecvTupleChunk(conn);

			*srcRoute = index;

			/*
			 * advance start point (avoid doing division/modulus operation
			 * here)
			 */
			pEntry->scanStart = index + 1;

			return tcItem;
		}
	}

	/* we should never ever get here... */
	Insist(0);
}

/* See ml_ipc.h */
TupleChunkListItem
RecvTupleChunkFrom(int16 motNodeID, int16 srcRoute)
{
	MotionLayerState *pEntry;
	MotionConn *conn;

	Insist(activated);

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "RecvTupleChunkFrom(motNodID=%d, srcRoute=%d)", motNodeID, srcRoute);
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
	elog(DEBUG5, "read packet recv bytes %d msgsize %d", conn->recvBytes, conn->msgSize);
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

        tcItem->chunk_length = tcSize;
		tcItem->inplace = (char *) (conn->msgPos + bytesProcessed);

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
		elog(DEBUG5, "residual message %d bytes", conn->recvBytes);
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

	Insist(activated);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG4, "DeregisterReadInterest(%d, %d)", motNodeID, srcRoute);
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
	 */
	shutdown(conn->sockfd, SHUT_WR);

	FD_CLR(conn->sockfd, &(pEntry->readSet));
}

static void
flushIncomingData(int fd)
{
	static char trash[8192];
	int			bytes;

	/*
	 * If we're in TeardownInterconnect, we should only have to call recv() a
	 * couple of times to empty out our socket buffers
	 */
	do
	{
		bytes = recv(fd, trash, sizeof(trash), 0);
	} while (bytes > 0);
}

/* Returns the fd of the socket that connects to the seqserver.  This value
 * is -1 if it has not been setup.
 */
int
GetSeqServerFD(void)
{
	/*
	 * setup connection to seq server if needed. The interconnect is
	 * responsible for maintaining the connection although it actually doesn't
	 * use the socket directly.  sequence.c does to obtain sequence values
	 * from the seqserver.	TeardownInterconnect() is responsible for closing
	 * the socket.
	 *
	 */
	if (seqServerHost == NULL)
		elog(ERROR, "Invalid Sequence Access.  Slice table did not specify a seqserver.");

	if (seqserverFd == -1)
		setupSeqServerConnection(seqServerHost, seqServerPort);

	return seqserverFd;
}



void
SetupSequenceServer(SliceTable *mySliceTable)
{
	if( mySliceTable != NULL )
	{
		seqServerHost = mySliceTable->seqServerHost;
		seqServerPort = mySliceTable->seqServerPort;
	}
}

void
TeardownSequenceServer(void)
{
	/*
	 * If we setup a connection to the seqserver then we need to disconnect
	 */
	if (seqserverFd != -1)
	{
		elog(DEBUG4, "tearing down seqserver connection");
		shutdown(seqserverFd, SHUT_RDWR);
		close(seqserverFd);
		seqserverFd = -1;
	}
}



/* See ml_ipc.h */
void
SetupInterconnect(SliceTable * mySliceTable, int mySliceId)
{
	int			i,
				n;
	ListCell   *cell;
	Slice	   *mySlice;
	Slice	   *aSlice;
    MotionConn *conn;
	int			incoming_count = 0;
	int			outgoing_count = 0;
	int			expectedTotalIncoming = 0;
	int			expectedTotalOutgoing = 0;
    int         iteration = 0;
	struct timeval  startTime;
    StringInfoData  logbuf;

	/* we can have at most one of these. */
	MotionLayerState *sendingMotionState = NULL;

	Insist(!activated && !mlsStates && mySliceTable);

	mlsSize = MLS_INITIAL_SIZE;
	mlsStates = palloc0(MLS_INITIAL_SIZE * sizeof(MotionLayerState));

	sliceTable = mySliceTable;
	sliceId = mySliceId;

	mySlice = (Slice *) list_nth(sliceTable->slices, mySliceId);

    Assert(mySlice &&
           IsA(mySlice, Slice) &&
           mySlice->sliceIndex == mySliceId);

  	gettimeofday(&startTime, NULL);

    /* Initiate outgoing connections. */
    if (mySlice->parentIndex != -1)
		sendingMotionState = startOutgoingConnections(mySlice, &expectedTotalOutgoing);

	/* now we'll do some setup for each of our Receiving Motion Nodes. */
	foreach(cell, mySlice->children)
	{
		int			numProcs;
		int			childId = lfirst_int(cell);
	    MotionLayerState *pEntry;

#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "Setting up RECEIVING motion node %d", childId);
#endif

		aSlice = (Slice *) list_nth(sliceTable->slices, childId);
		numProcs = list_length(aSlice->primaryProcesses);

		pEntry = createMotionState(aSlice, mySlice, numProcs, 0);

		/* let cdbmotion now how many receivers to expect. */
		setExpectedReceivers(childId, numProcs);

		expectedTotalIncoming += pEntry->numConns;
	}

    ereport(DEBUG4, (errmsg("SetupInterconnect+0ms: Activating "
                            "%d incoming, %d outgoing routes.  "
                            "Listening on port=%d sockfd=%d",
                            expectedTotalIncoming, expectedTotalOutgoing,
                            Mpp_listener_port, listenerFd)
                     ));

	/*
     * Loop until all connections are completed or time limit is exceeded.
	 */
	while (outgoing_count < expectedTotalOutgoing ||
           incoming_count < expectedTotalIncoming)
	{                           /* select() loop */
		struct timeval  timeout;
        fd_set          rset, wset, eset;
	    int			    highsock = -1;
        int             elapsed_ms = 0;
        int             timeout_ms = 20*60*1000;
        int             outgoing_fail_count = 0;
        int             elevel;

        iteration++;

        FD_ZERO(&rset);
	    FD_ZERO(&wset);
	    FD_ZERO(&eset);

        /* Expecting any new inbound connections? */
        if (incoming_count < expectedTotalIncoming)
        {
            Insist(listenerFd >= 0);
            FD_SET(listenerFd, &rset);
            highsock = listenerFd;
        }

        /* Inbound connections awaiting registration message */
        foreach(cell, incompleteConns)
        {
			conn = (MotionConn *)lfirst(cell);
            Insist(conn->state == mcsRecvRegMsg && conn->sockfd >= 0);
            FD_SET(conn->sockfd, &rset);
            highsock = Max(highsock, conn->sockfd);
        }

        /* Outgoing connections */
        outgoing_count = 0;
        n = sendingMotionState ? sendingMotionState->numConns : 0;
        for (i = 0; i < n; i++)
		{                       /* loop to set up outgoing connections */
			conn = &sendingMotionState->conns[i];

            /* Time to cancel incomplete connect() and retry? */
            if (conn->state == mcsConnecting &&
                conn->wakeup_ms > 0 &&
                conn->wakeup_ms <= elapsed_ms + 20)
            {
                ereport(LOG, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                              errmsg("Interconnect timeout: Connection "
                                     "to seg%d %s from local port %s was not "
                                     "complete after %d ms.  Will retry.",
                                     conn->remoteContentId,
                                     conn->remoteHostAndPort,
                                     conn->localHostAndPort,
                                     CONNECT_RETRY_MS)
                        ));
                zapOutgoingPort(sendingMotionState);
                conn->state = mcsSetupOutgoingConnection;
            }

            /* Time to connect? */
            if (conn->state == mcsSetupOutgoingConnection &&
                conn->wakeup_ms <= elapsed_ms + 20)
            {
                setupOutgoingConnection(sendingMotionState, conn);
                switch (conn->state)
                {
                    case mcsSetupOutgoingConnection:
                        /* Retry failed connection after awhile. */
                        conn->wakeup_ms = (iteration - 1) * 1000 + elapsed_ms;
                        break;
                    case mcsConnecting:
                        /* Set time limit for connect() to complete. */
                        conn->wakeup_ms = CONNECT_RETRY_MS + elapsed_ms;
                        break;
                    default:
                        conn->wakeup_ms = 0;
                        break;
                }
            }

            /* What events are we watching for? */
            switch (conn->state)
            {
                case mcsNull:
                    break;
                case mcsSetupOutgoingConnection:
                    outgoing_fail_count++;
                    break;
                case mcsConnecting:
                    Insist(conn->sockfd >= 0);
                    FD_SET(conn->sockfd, &wset);
                    FD_SET(conn->sockfd, &eset);
                    highsock = Max(highsock, conn->sockfd);
                    break;
                case mcsSendRegMsg:
                    Insist(conn->sockfd >= 0);
                    FD_SET(conn->sockfd, &wset);
                    highsock = Max(highsock, conn->sockfd);
                    break;
                case mcsStarted:
                    outgoing_count++;
                    break;
                default:
                    Insist(0);
            }

            if (conn->wakeup_ms > 0)
                timeout_ms = Min(timeout_ms, conn->wakeup_ms - elapsed_ms);
        }                       /* loop to set up outgoing connections */

        /* Break out of select() loop if completed all connections. */
        if (outgoing_count == expectedTotalOutgoing &&
            incoming_count == expectedTotalIncoming)
            break;

        /*
         * Been here long?  Bail if mpp_interconnect_setup_timeout exceeded.
         */
        if (interconnect_setup_timeout > 0)
        {
            int to = interconnect_setup_timeout * 1000;

            if (to <= elapsed_ms + 20)
                ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                                errmsg("Interconnect timeout: Unable to "
                                       "complete setup of all connections "
                                       "within time limit."),
                                errdetail("Completed %d of %d incoming and "
                                          "%d of %d outgoing connections.  "
                                          "mpp_interconnect_setup_timeout = %d "
                                          "seconds.",
                                          incoming_count, expectedTotalIncoming,
                                          outgoing_count, expectedTotalOutgoing,
                                          interconnect_setup_timeout)
                        ));
            timeout_ms = Min(timeout_ms, to - elapsed_ms);
        }

        /*
         * If no socket events to wait for, loop to retry after a pause.
         */
        if (highsock < 0)
        {
            if (timeout_ms > 0 ||
                iteration > 2)
                ereport(DEBUG4, (errmsg("SetupInterconnect+%dms:   pause %dms   "
                                        "outgoing_fail=%d iteration=%d",
                                        elapsed_ms, timeout_ms,
                                        outgoing_fail_count, iteration)
                        ));

            /* Shouldn't be in this loop unless we have some work to do. */
            Insist(outgoing_fail_count > 0);

            /* Wait until earliest wakeup time or overall timeout. */
            if (timeout_ms > 0)
            {
                ML_CHECK_FOR_INTERRUPTS();
                pg_usleep(timeout_ms * 1000);
                ML_CHECK_FOR_INTERRUPTS();
            }

            /* Back to top of loop and look again. */
            elapsed_ms = gettime_elapsed_ms(startTime);
            continue;
        }

        /*
		 * Wait for socket events.
		 */
        if (timeout_ms > 0)
        {
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_usec = (timeout_ms - (timeout.tv_sec * 1000)) * 1000;
            Assert(timeout_ms == timeout.tv_sec*1000 + timeout.tv_usec/1000);
        }
        else
            timeout.tv_sec = timeout.tv_usec = 0;

#ifdef AMS_VERBOSE_LOGGING
        elevel = DEBUG5;
        if (elevel >= log_min_messages)
        {
            initStringInfo(&logbuf);
            format_fd_set(&logbuf, highsock+1, &rset, "r={", "} ");
            format_fd_set(&logbuf, highsock+1, &wset, "w={", "} ");
            format_fd_set(&logbuf, highsock+1, &eset, "e={", "}");
            elapsed_ms = gettime_elapsed_ms(startTime);
            ereport(elevel, (errmsg("SetupInterconnect+%dms:   select()  "
                                    "Interest: %s.  timeout=%dms "
                                    "outgoing_fail=%d iteration=%d",
                                    elapsed_ms, logbuf.data, timeout_ms,
                                    outgoing_fail_count, iteration)
                    ));
            pfree(logbuf.data);
            memset(&logbuf, 0, sizeof(logbuf));
        }
#endif

		ML_CHECK_FOR_INTERRUPTS();
        n = select(highsock + 1, &rset, &wset, &eset, &timeout);
		ML_CHECK_FOR_INTERRUPTS();

        elapsed_ms = gettime_elapsed_ms(startTime);

        /*
         * Log the select() if requested.
         */
        elevel = (n == expectedTotalIncoming + expectedTotalOutgoing)
                 ? DEBUG5 : DEBUG4;
        if (elevel >= log_min_messages)
        {
            int errnoSave = errno;

            initStringInfo(&logbuf);
            if (n > 0)
            {
                appendStringInfo(&logbuf, "result=%d  Ready: ", n);
                format_fd_set(&logbuf, highsock+1, &rset, "r={", "} ");
                format_fd_set(&logbuf, highsock+1, &wset, "w={", "} ");
                format_fd_set(&logbuf, highsock+1, &eset, "e={", "}");
            }
            else
                appendStringInfoString(&logbuf, n < 0 ? "error" : "timeout");
            ereport(elevel, (errmsg("SetupInterconnect+%dms:   select()  %s",
                                    elapsed_ms, logbuf.data) ));
            pfree(logbuf.data);
            memset(&logbuf, 0, sizeof(logbuf));
            errno = errnoSave;
        }

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
				            errmsg("Interconnect error: %m%s", "select")
                    ));
		}

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
		while (n > 0 &&
               incoming_count < expectedTotalIncoming &&
               cell != NULL)
		{
			conn = (MotionConn *) lfirst(cell);

			/*
			 * we'll get the next cell ready now in case we need to delete
			 * the cell that corresponds to our MotionConn
			 */
			cell = lnext(cell);

			if (FD_ISSET(conn->sockfd, &rset))
			{
                n--;
                if (readRegisterMessage(conn))
                {
                    incompleteConns = list_delete_ptr(incompleteConns, conn);
                    incoming_count++;

                    /* Free temporary MotionConn storage. */
                    pfree(conn);
                }
			}
		}

        /*
         * Someone tickling our listener port?  Accept pending connections.
         */
        if (FD_ISSET(listenerFd, &rset))
		{
			n--;
            while ((conn = acceptIncomingConnection()) != NULL)
            {
                /* Try to read the Register message without waiting. */
                if (readRegisterMessage(conn))
                {
                    incoming_count++;
                    pfree(conn);
                }

                /* We'll try again when more data arrives on the socket. */
                else
                    incompleteConns = lappend(incompleteConns, conn);
            }
		}

		/*
         * Check our outgoing connections.
         */
        i = 0;
        while (n > 0 &&
               outgoing_count < expectedTotalOutgoing &&
		       i < sendingMotionState->numConns)
		{                       /* loop to check outgoing connections */
			conn = &sendingMotionState->conns[i++];
            switch (conn->state)
            {
                case mcsConnecting:
                    /* Has connect() succeeded or failed? */
                    if (FD_ISSET(conn->sockfd, &wset) ||
                        FD_ISSET(conn->sockfd, &eset))
                    {
                        n--;
                        updateOutgoingConnection(sendingMotionState, conn, -1);
                        switch (conn->state)
                        {
                            case mcsSetupOutgoingConnection:
                                /* Failed.  Wait awhile before retrying. */
                                conn->wakeup_ms = (iteration - 1) * 1000 + elapsed_ms;
                                break;
                            case mcsSendRegMsg:
                                /* Connected, but reg msg not fully sent. */
                                conn->wakeup_ms = 0;
                                break;
                            case mcsStarted:
                                /* Connected, sent reg msg, ready to rock. */
                                outgoing_count++;
                                break;
                            default:
                                Insist(0);
                        }
                    }
                    break;

                case mcsSendRegMsg:
                    /* Ready to continue sending? */
                    if (FD_ISSET(conn->sockfd, &wset))
                    {
                        n--;
                        sendRegisterMessage(sendingMotionState, conn);
                        if (conn->state == mcsStarted)
                            outgoing_count++;
                    }
                    break;

                default:
                    break;
            }

		}                       /* loop to check outgoing connections */

        /* By now we have dealt with all the events reported by select(). */
        Insist(n == 0);
	}                           /* select() loop */

	/*
	 * if everything really got setup properly then we shouldn't have
	 * any incomplete connections.
	 *
	 * XXX: In some cases (when the previous query got 'fast-track
	 * cancelled' because of an error during setup) we can wind up
	 * with connections here which ought to have been cleaned
	 * up. These connections should be closed out here. It would
	 * obviously be better if we could avoid these connections in the
	 * first place!
	 */
	if (list_length(incompleteConns) != 0)
	{
		elog(DEBUG2, "Incomplete connections after known connections done, cleaning %d",
			 list_length(incompleteConns));

		while ((cell = list_head(incompleteConns)) != NULL)
		{
			conn = (MotionConn *) lfirst(cell);

			if (conn->sockfd != -1)
			{
				flushIncomingData(conn->sockfd);
				shutdown(conn->sockfd, SHUT_WR);
				close(conn->sockfd);
				conn->sockfd = -1;
			}

			incompleteConns = list_delete_ptr(incompleteConns, conn);

			if (conn->pBuff)
                pfree(conn->pBuff);
			pfree(conn);
		}
	}

	activated = true;

    ereport(DEBUG3, (errmsg("SetupInterconnect+%dms: Ready.  "
                            "Activated %d incoming, "
                            "%d outgoing routes.",
                            gettime_elapsed_ms(startTime),
                            incoming_count, outgoing_count)
                     ));
}                               /* SetupInterconnect */


/*
 * Move this out to separate stack frame, so that we don't have to mark
 * tons of stuff volatile in TeardownInterconnect().
 */
static void
forceEosToPeer(int16 motNodeID, int16 targetRoute)
{

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "forceEosToPeer(%d, %d)", motNodeID, targetRoute);
#endif

	teardownActive = true;

	SendEndOfStream(motNodeID, targetRoute);

	teardownActive = false;
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
 * If forceEOS is set, we force end-of-stream notifications out any send-nodes,
 * and we wrap that send in a PG_TRY/CATCH block because the goal is to reduce
 * confusion (and if we're being shutdown abnormally anyhow, let's not start
 * adding errors!).
 */
void
TeardownInterconnect(bool forceEOS)
{
	ListCell   *cell;
	MotionLayerState *pEntry;
	int			i;
	Slice	   *mySlice;
	MotionConn *conn;

	/* multi-phase shutdown, mark all as shutdown */
	ereport(DEBUG3, (errmsg("Interconnect cleanup state: %s; setup was %s",
		                    forceEOS ? "force" : "normal",
                            activated ? "completed" : "exited")
            ));

	Assert(sliceTable != NULL);

	mySlice = (Slice *) list_nth(sliceTable->slices, sliceId);

	/*
	 * phase 1 mark all sockets (senders and receivers) with
	 * shutdown(2), start with incomplete connections (if any).
	 */

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

		/* they're incomplete, so just slam them shut. */
		if (conn->sockfd != -1)
		{
			flushIncomingData(conn->sockfd);
			shutdown(conn->sockfd, SHUT_WR);
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

	/*
	 * Now "normal" connections which made it through our
	 * peer-registration step. With these we have to worry about
	 * "in-flight" data.
	 */
	if (mySlice->parentIndex != -1)
	{
#ifdef AMS_VERBOSE_LOGGING
        /* cleanup a Sending motion node. */
		elog(DEBUG5, "shutdown outgoing connections for motion id: %d",
			 mySlice->sliceIndex);
#endif

		pEntry = getMotionState(mySlice->sliceIndex);

		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (forceEOS)
			{
				forceEosToPeer(mySlice->sliceIndex, i);
			}

			if (conn->sockfd >= 0)
                shutdown(conn->sockfd, SHUT_WR);
		}
	}

	/*
	 * cleanup all of our Receiving Motion nodes, these get closed
	 * immediately (the receiver know for real if they want to shut
	 * down -- they aren't going to be processing any more data).
	 */
	foreach(cell, mySlice->children)
	{
		Slice	   *aSlice;
		int			childId = lfirst_int(cell);

		aSlice = (Slice *) list_nth(sliceTable->slices, childId);

		pEntry = removeMotionState(aSlice->sliceIndex);
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG4, "close down connections for motion Id: %d", pEntry->motNodeId);
#endif

		/*
		 * receivers know that they no longer care about data from
		 * below ... so we can safely discard data queued in both
		 * directions
		 */
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->sockfd >= 0)
			{
				flushIncomingData(conn->sockfd);
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
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG4, "close outgoing connections for motion id: %d",
			 mySlice->sliceIndex);
#endif

		pEntry = removeMotionState(mySlice->sliceIndex);

		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->sockfd >= 0)
			{
				if (!forceEOS)
					waitOnSocketDone(conn->sockfd);
				close(conn->sockfd);
				conn->sockfd = -1;
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

	if (mlsStates != NULL)
	{
		pfree(mlsStates);
		mlsStates = NULL;
		mlsSize = 0;
	}

	activated = false;
	sliceTable = NULL;

	ereport(DEBUG4, (errmsg("TeardownInterconnect successful") ));
}


/*=========================================================================
 * HELPER FUNCTIONS
 */

/*
 * setupListeningSocket
 */
void
setupListeningSocket(int backlog, int *listenerSocketFd, int *listenerPort)
{
	struct sockaddr_in  saddr;
	socklen_t           saddrlen;
    int                 errnoSave;
    int                 fd = -1;
    const char         *fun;

    *listenerSocketFd = -1;
    *listenerPort = 0;

    fun = "socket";
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        goto error;

    /* Make socket non-blocking. */
    fun = "fcntl(O_NONBLOCK)";
    if (!pg_set_noblock(fd))
        goto error;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	saddr.sin_port = htons(0);	/* system picks port */

	/*
	 * we let the system pick the TCP port here so we don't have to
	 * manage port resources ourselves.
	 */
    fun = "bind";
	if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
        goto error;

    fun = "listen";
    if (listen(fd, backlog) < 0)
        goto error;

    /* Get the listening socket's port number. */
    saddrlen = sizeof(struct sockaddr_in);
    fun = "getsockname";
    if (getsockname(fd, (struct sockaddr *) &saddr, &saddrlen) < 0)
        goto error;
	
    /* Give results to caller. */
    *listenerSocketFd = fd;
    *listenerPort = ntohs(saddr.sin_port);
	return;

error:
    errnoSave = errno;
    if (fd >= 0)
        close(fd);
    errno = errnoSave;
    ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
					errmsg("Interconnect Error: Could not set up listener socket."),
			        errdetail("%m%s", fun)
            ));
}                               /* setupListeningSocket */


/* Function createMotionState() is used to create a MotionLayerState struct and
 * place it in the hashtab hashtable based on the motNodeID.
 *
 * PARAMETERS
 *
 *	 motNodeID - motion node ID for this MotionState.
 *
 *	 numPrimaryConns  - number of primary connections for this motion node.
 *               All are incoming if this is a receiving motion node.
 *               All are outgoing if this is a sending motion node.
 *
 *   numMirrorConns - number of mirror connections (always 0 if receiving).
 *
 * RETURNS
 *	 An empty and initialized MotionState struct for the given motion node. If
 *	 a MotionState struct is already registered for the motNodeID an ERROR is
 *	 thrown.
 */
MotionLayerState *
createMotionState(Slice *sendSlice, Slice *recvSlice, int numPrimaryConns, int numMirrorConns)
{
	MotionLayerState *pEntry;
    int         motNodeID;
	int			i;

    Assert(numMirrorConns == 0 || numMirrorConns == numPrimaryConns);
    Assert(recvSlice &&
           IsA(recvSlice, Slice) &&
           recvSlice->sliceIndex >= 0);
    Assert(sendSlice &&
           IsA(sendSlice, Slice) &&
           sendSlice->sliceIndex > 0);

    motNodeID = sendSlice->sliceIndex;
    if (motNodeID > mlsSize)
	{
		/* increase size of our table */
		MotionLayerState *newTable;

		newTable = repalloc(mlsStates, motNodeID * sizeof(MotionLayerState));
		mlsStates = newTable;
		/* zero-out the new piece at the end */
		memset(&mlsStates[mlsSize], 0, (motNodeID - mlsSize) * sizeof(MotionLayerState));
		mlsSize = motNodeID;
	}

	pEntry = &mlsStates[motNodeID - 1];

	if (pEntry->valid)
	{
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: A HTAB entry for motion node %d already exists.", motNodeID),
						errdetail("conns %p numConns %d first sock %d highreadsock %d", pEntry->conns, pEntry->numConns, pEntry->conns[0].sockfd, pEntry->highReadSock)));
	}

	pEntry->valid = true;

	pEntry->motNodeId = motNodeID;
    pEntry->numConns = numPrimaryConns + numMirrorConns;
	pEntry->numPrimaryConns = numPrimaryConns;
	pEntry->highReadSock = 0;
    pEntry->scanStart = 0;
    pEntry->sendSlice = sendSlice;
    pEntry->recvSlice = recvSlice;
    pEntry->outgoingPortRetryCount = 0;

	pEntry->conns = palloc0(pEntry->numConns * sizeof(pEntry->conns[0]));

	for (i = 0; i < pEntry->numConns; i++)
    {
        MotionConn *conn = &pEntry->conns[i];

        /* Initialize MotionConn entry. */
        conn->state = mcsNull;
        conn->sockfd = -1;
        conn->msgSize = 0;
        conn->tupleCount = 0;
        conn->stillActive = false;
        conn->wakeup_ms = 0;
        conn->cdbProc = NULL;
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
static inline MotionLayerState *
getMotionState(int motNodeID)
{
	MotionLayerState *pEntry = NULL;

	if (motNodeID > 0 &&
        motNodeID <= mlsSize &&
        mlsStates &&
        mlsStates[motNodeID-1].motNodeId == motNodeID &&
        mlsStates[motNodeID-1].valid)
    {
        pEntry = &mlsStates[motNodeID - 1];
    }
    else
	{
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
		errmsg("Interconnect Error: Unexpected Motion Node Id: %d.  This means"
			   " a motion node that wasn't setup is requesting interconnect"
			   " resources.", motNodeID)));
        /* not reached */
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
 *	 motNodeID - motion node ID to lookup the MotionLayerState.
 *
 * RETURNS
 *	 The MotionLayerState that was removed from the hashtab hashtable.
 */
static MotionLayerState *
removeMotionState(int16 motNodeID)
{
	MotionLayerState *pEntry = NULL;

	if (motNodeID > mlsSize || !mlsStates[motNodeID - 1].valid)
	{
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: Unexpected Motion Node Id: %d. During remove.",
							   motNodeID)));
	}
	else
	{
		mlsStates[motNodeID - 1].valid = false;
		pEntry = &mlsStates[motNodeID - 1];
	}

	return pEntry;
}


static void
setupSeqServerConnection(char *seqServerHost, int seqServerPort )
{
	int  n;
	struct sockaddr_in addr;	
	
	
	/*
	 * open a connection to the sequence server
	 */
	seqserverFd = socket(AF_INET, SOCK_STREAM, 0);
	
	
	addr.sin_family = AF_INET;

	if ((n = inet_pton(AF_INET, seqServerHost, &addr.sin_addr)) < 1)
	{
		if (n == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
				 errmsg("Interconnect Error: Could not parse remote seqserver"
						"address: '%s'", seqServerHost),
					   errdetail("inet_pton() unable to parse address: '%s'",
								 seqServerHost)));
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
			 errmsg("Interconnect Error: System error occured during parsing"
				 " of remote seqserver address: '%s'", seqServerHost),
							errdetail("inet_pton()  (errno: %d) unable to parse address: '%s'",
									  errno, seqServerHost)));
		}
	}

	addr.sin_port = htons(seqServerPort);

	if ((n = connect(seqserverFd, (struct sockaddr *) & addr, sizeof(addr))) < 0)
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: Could not connect to seqserver."),
		                errdetail("%m%s", "connect")
                ));
	
	/* make socket non-blocking BEFORE we connect. */
	if (!pg_set_noblock(seqserverFd))
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: Could not set seqserver socket"
						       "to non-blocking mode."),
			            errdetail("%m%s sockfd=%d", "fcntl", seqserverFd)
                ));
	
}


/* Function startOutgoingConnections() is used to initially kick-off any outgoing
 * connections for mySlice.
 *
 * This should not be called for root slices (i.e. QD ones) since they don't
 * ever have outgoing connections.
 *
 * PARAMETERS
 *
 *	 mySlice  - Slice that this process is a member of.
 *
 * RETURNS
 *	 Initialized MotionLayerState for the Sending Motion Node Id.
 */
MotionLayerState *
startOutgoingConnections(Slice *sendSlice, int *pOutgoingCount)
{
	MotionLayerState *pEntry;
    MotionConn *conn;
    ListCell   *cell;
	Slice	   *recvSlice;
    CdbProcess *cdbProc;

    *pOutgoingCount = 0;

	recvSlice = (Slice *) list_nth(sliceTable->slices, sendSlice->parentIndex);

	ereport(DEBUG5, (errmsg("Setting up SENDING motion node %d",
		                    sendSlice->sliceIndex)
            ));

	pEntry = createMotionState(sendSlice,
                               recvSlice,
                               list_length(recvSlice->primaryProcesses),
                               list_length(recvSlice->mirrorProcesses));

	/*
	 * Setup a MotionConn entry for each of our outbound connections.
     * Request a connection to each receiving backend's listening port.
     * NB: Some mirrors could be down & have no CdbProcess entry.
	 */
    conn = pEntry->conns;

    foreach(cell, recvSlice->primaryProcesses)
	{
        cdbProc = (CdbProcess *)lfirst(cell);
        if (cdbProc)
		{
            conn->cdbProc = cdbProc;
            conn->pBuff = palloc(Mpp_max_packet_size);
            conn->state = mcsSetupOutgoingConnection;
            (*pOutgoingCount)++;
		}
        conn++;
	}

	foreach(cell, recvSlice->mirrorProcesses)
	{
        cdbProc = (CdbProcess *)lfirst(cell);
        if (cdbProc)
		{
            conn->cdbProc = cdbProc;
            conn->pBuff = palloc(Mpp_max_packet_size);
            conn->state = mcsSetupOutgoingConnection;
            (*pOutgoingCount)++;
		}
        conn++;
	}

    return pEntry;
}                               /* startOutgoingConnections */


/*
 * setupOutgoingConnection
 *
 * Called by SetupInterconnect when conn->state == mcsSetupOutgoingConnection.
 *
 * On return, state is:
 *      mcsSetupOutgoingConnection if failed and caller should retry.
 *      mcsConnecting if non-blocking connect() is pending.  Caller should
 *          send registration message when socket becomes write-ready.
 *      mcsSendRegMsg or mcsStarted if connect() completed successfully.
 */
void
setupOutgoingConnection(MotionLayerState *pEntry, MotionConn *conn)
{
    CdbProcess         *cdbProc = conn->cdbProc;
	struct sockaddr_in  saddr;
    int                 sockopt;
	int                 n;
#if 0   /* port number conservation not working yet */
    int                 outgoingPort;
#endif

    Assert(conn->cdbProc);
    Assert(conn->state == mcsSetupOutgoingConnection);

    conn->wakeup_ms = 0;
    conn->remoteContentId = cdbProc->contentid;

    snprintf(conn->remoteHostAndPort, sizeof(conn->remoteHostAndPort),
             "%s:%d", cdbProc->listenerAddr, cdbProc->listenerPort);

    /* Might be retrying due to connection failure etc.  Close old socket. */
    if (conn->sockfd >= 0)
    {
        close(conn->sockfd);
        conn->sockfd = -1;
    }

    /*
     * Create a socket.
     */
    conn->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->sockfd < 0)
	    ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
			            errdetail("%m%s", "socket")
                ));

    /* make socket non-blocking BEFORE we connect. */
    if (!pg_set_noblock(conn->sockfd))
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
		                errdetail("%m%s", "fcntl(O_NONBLOCK)")
                ));
    }

    /* Allow bind() to succeed even if the port is in TIME_WAIT state. */
    sockopt = 1;
    if (setsockopt(conn->sockfd, SOL_SOCKET, SO_REUSEADDR,
                   &sockopt, sizeof(sockopt)) < 0)
	    ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
		                errdetail("%m%s", "setsockopt(SO_REUSEADDR)")
                ));

#if 0   /* port number conservation not working yet */
    /*
     * we can run out of ports if we aren't careful, so we try to reuse
     * them as much as possible
     */
    outgoingPort = getOutgoingPort(pEntry);
    snprintf(conn->localHostAndPort, sizeof(conn->localHostAndPort),
             "tcp:%d", outgoingPort);

    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(outgoingPort);

    if (bind(conn->sockfd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
        /* Should never get EADDRINUSE because we know it's our port. */
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
                        errdetail("Could not bind to local port %s. %m%s",
                                  conn->localHostAndPort,
                                  "bind")
                ));
    }
#endif

    /*
     * Get ipaddr to connect to.
     */
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;

    if ((n = inet_pton(AF_INET, cdbProc->listenerAddr, &saddr.sin_addr)) < 1)
    {
	    if (n == 0)
	    {
		    ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
			     errmsg("Interconnect Error: Could not parse remote listener"
					    "address: '%s'", cdbProc->listenerAddr),
				       errdetail("inet_pton() unable to parse address: '%s'",
							     cdbProc->listenerAddr)));
	    }
	    else
	    {
		    ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
		     errmsg("Interconnect Error: System error occured during parsing"
			     " of remote listener address '%s'", cdbProc->listenerAddr),
						    errdetail("%m%s", "inet_pton")
                    ));
	    }
    }

    saddr.sin_port = htons(cdbProc->listenerPort);

    format_sockaddr((struct sockaddr *) &saddr, conn->remoteHostAndPort,
                    sizeof(conn->remoteHostAndPort));

    ereport(DEBUG5, (errmsg("Connecting to seg%d %s pid=%d sockfd=%d",
                            conn->remoteContentId,
                            conn->remoteHostAndPort,
                            conn->cdbProc->pid,
                            conn->sockfd)
                     ));

    /*
     * Initiate the connection.
     */
    for (;;)
    {                           /* connect() EINTR retry loop */
        ML_CHECK_FOR_INTERRUPTS();

        n = connect(conn->sockfd, (struct sockaddr *) &saddr, sizeof(saddr));
	
        /* Non-blocking socket never connects immediately, but check anyway. */
        if (n == 0)
        {
            sendRegisterMessage(pEntry, conn);
            return;
        }
	
        /* Retry if a signal was received. */
        if (errno == EINTR)
            continue;

        /* Normal case: select() will tell us when connection is made. */
        if (errno == EINPROGRESS ||
            errno == EWOULDBLOCK)
        {
            conn->state = mcsConnecting;
            return;
        }

        /* connect() failed.  Log the error.  Caller should retry. */
        updateOutgoingConnection(pEntry, conn, errno);
        return;
    }                           /* connect() EINTR retry loop */
}                               /* setupOutgoingConnection */


/*
 * updateOutgoingConnection
 *
 * Called when connect() succeeds or fails.
 */
void
updateOutgoingConnection(MotionLayerState *pEntry, MotionConn *conn, int errnoSave)
{
    socklen_t   sizeoferrno = sizeof(errnoSave);

    /* Get errno value indicating success or failure. */
    if (errnoSave == -1 &&
        getsockopt(conn->sockfd, SOL_SOCKET, SO_ERROR,
                   &errnoSave, &sizeoferrno))
    {
        /* getsockopt failed */
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect could not connect to seg%d %s",
                               conn->remoteContentId,
                               conn->remoteHostAndPort),
                        errdetail("%m%s sockfd=%d",
                                  "getsockopt(SO_ERROR)",
                                  conn->sockfd)
                ));
    }

    switch (errnoSave)
    {
        /* Success!  Advance to next state. */
        case 0:
            sendRegisterMessage(pEntry, conn);
            return;

#if 0   /* port number conservation not working yet */
        /* An old connection between same endpoints might be in TIME_WAIT. */
        case EADDRINUSE:
        case EINVAL:
        case ECONNREFUSED:
            errno = errnoSave;
            ereport(LOG, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
		                  errmsg("Interconnect could not connect to seg%d %s from "
                                 "local port %s; will try another port. %m%s",
                                 conn->remoteContentId,
                                 conn->remoteHostAndPort,
                                 conn->localHostAndPort,
                                 "connect")
                    ));

            /* Try a different port number for the next connection attempt. */
            zapOutgoingPort(pEntry);
            break;
#endif

        default:
            errno = errnoSave;
            ereport(LOG, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
		                  errmsg("Interconnect could not connect to seg%d %s "
                                 "pid=%d; "
                                 "will retry. %m%s",
                                 conn->remoteContentId,
                                 conn->remoteHostAndPort,
                                 conn->cdbProc->pid,
                                 "connect")
                    ));
            break;
    }

    /* Tell caller to close the socket and try again. */
    conn->state = mcsSetupOutgoingConnection;
}                               /* updateOutgoingConnection */


/*
 * getOutgoingPort
 *
 * Returns port number for an outgoing connection.
 */
static int
getOutgoingPort(MotionLayerState *pEntry)
{
    int oldPortReservationFd = portReservationFd;

    /* Already got a port number?  Keep using it. */
    if (outgoingPort)
        return outgoingPort;

    /* Reserve new port number.  There will be no connections to this socket. */
    setupListeningSocket(0, &portReservationFd, &outgoingPort);

    /* Close dummy listening socket that reserved our old outgoing port#. */
    if (oldPortReservationFd >= 0)
    {
        close(oldPortReservationFd);
        pEntry->outgoingPortRetryCount++;
    }

    ereport(DEBUG3, (errmsg("Outgoing connections will use tcp port %d",
                            outgoingPort) ));
    return outgoingPort;
}                               /* getOutgoingPort */


/*
 * zapOutgoingPort
 *
 * Abandon the current outgoing port reservation.
 */
static void
zapOutgoingPort(MotionLayerState *pEntry)
{
    /* Tell getOutgoingPort() to pick a new port number. */
    outgoingPort = 0;

    /* Bail if too many retries. */
    if (pEntry->outgoingPortRetryCount >= MAX_BIND_RETRIES)
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect Error: Connection setup abandoned "
                               "after %d retries.",
                               pEntry->outgoingPortRetryCount)
                ));
    }
}                               /* zapOutgoingPort */


/* Function sendRegisterMessage() used to send a Register message to the
 * remote destination on the other end of the provided conn.
 *
 * PARAMETERS
 *
 *	 pEntry - MotionLayerState.
 *	 conn	- MotionConn to send message out on.
 *
 * Called by SetupInterconnect when conn->state == mcsSetupOutgoingConnection.
 *
 * On return, state is:
 *      mcsSendRegMsg if registration message has not been completely sent.
 *          Caller should retry when socket becomes write-ready.
 *      mcsStarted if registration message has been sent.  Caller can start
 *          sending data.
 */
static void
sendRegisterMessage(MotionLayerState * pEntry, MotionConn * conn)
{
    int     bytesToSend;
    int     bytesSent;

    if (conn->state != mcsSendRegMsg)
    {
	    RegisterMessage        *regMsg = (RegisterMessage *)conn->pBuff;
        struct sockaddr_storage localAddr;
        socklen_t               addrsize;

        Assert(conn->cdbProc &&
               conn->pBuff &&
               sizeof(*regMsg) <= Mpp_max_packet_size);
        Assert(pEntry->recvSlice &&
               pEntry->sendSlice &&
               IsA(pEntry->recvSlice, Slice) &&
               IsA(pEntry->sendSlice, Slice));

        /* Save local host and port for log messages. */
        addrsize = sizeof(localAddr);
	    if (getsockname(conn->sockfd, (struct sockaddr *)&localAddr, &addrsize))
	    {
            ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
		                    errmsg("Interconnect error after making connection."),
			                errdetail("%m%s sockfd=%d remote=%s",
					                  "getsockname",
                                      conn->sockfd,
                                      conn->remoteHostAndPort)
                    ));
	    }
        format_sockaddr((struct sockaddr *)&localAddr, conn->localHostAndPort,
                        sizeof(conn->localHostAndPort));

        ereport(DEBUG3, (errmsg("Interconnect sending registration message to "
                                "seg%d pid=%d remote=%s "
                                "from local=%s sockfd=%d: "
                                "recvSlice=%d sendSlice=%d "
                                "srcContentId=%d",
                                conn->remoteContentId,
                                conn->cdbProc->pid,
                                conn->remoteHostAndPort,
                                conn->localHostAndPort,
                                conn->sockfd,
                                pEntry->recvSlice->sliceIndex,
                                pEntry->sendSlice->sliceIndex,
                                Mpp_segment)
                ));

        regMsg->msgBytes = htonl(sizeof(*regMsg));
        regMsg->recvSliceIndex = htonl(pEntry->recvSlice->sliceIndex);
        regMsg->sendSliceIndex = htonl(pEntry->sendSlice->sliceIndex);
        regMsg->srcContentId = htonl(Mpp_segment);
        regMsg->srcListenerPort = htonl(Mpp_listener_port);
        regMsg->srcPid = htonl(MyProcPid);
        regMsg->srcSessionId = htonl(mpp_session_id);
        regMsg->srcCommandCount = htonl(mpp_command_count);

        conn->state = mcsSendRegMsg;
        conn->msgPos = conn->pBuff;
        conn->msgSize = sizeof(*regMsg);
    }

    /* Send as much as we can. */
    for (;;)
    {
	    bytesToSend = conn->pBuff + conn->msgSize - conn->msgPos;
        bytesSent = write(conn->sockfd, conn->msgPos, bytesToSend);
        if (bytesSent == bytesToSend)
            break;
        else if (bytesSent >= 0)
            conn->msgPos += bytesSent;
        else if (errno == EWOULDBLOCK)
            return;                     /* call me again to send the rest */
        else if (errno == EINTR)
            ML_CHECK_FOR_INTERRUPTS();
        else
        {
            ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                            errmsg("Interconnect error writing registration "
                                   "message to seg%d at %s",
                                   conn->remoteContentId,
                                   conn->remoteHostAndPort),
                            errdetail("%m%s pid=%d sockfd=%d local=%s",
                                      "write",
                                      conn->cdbProc->pid,
                                      conn->sockfd,
                                      conn->localHostAndPort)
                    ));
        }
    }

    /* Sent it all. */
    conn->state = mcsStarted;
    conn->msgPos = NULL;
	conn->msgSize = PACKET_HEADER_SIZE;
    conn->stillActive = true;
}                               /* sendRegisterMessage */


/* Function readRegisterMessage() reads a "Register" message off of the conn
 * and places it in the right MotionLayerEntry conn slot based on the contents
 * of the register message.
 *
 * PARAMETERS
 *
 *	 conn - MotionConn to read the register messagefrom.
 *
 * Returns true if message has been received; or false if caller must retry
 * when socket becomes read-ready.
 */
static bool
readRegisterMessage(MotionConn *conn)
{
    int     bytesToReceive;
    int     bytesReceived;
    int     iconn;
    RegisterMessage    *regMsg;
	RegisterMessage     msg;
	MotionConn         *newConn;
    MotionLayerState   *pEntry;
	CdbProcess         *cdbproc;

    /* Get ready to receive the Register message. */
    if (conn->state != mcsRecvRegMsg)
    {
        conn->state = mcsRecvRegMsg;
        conn->msgSize = sizeof(*regMsg);
        conn->msgPos = conn->pBuff;

        Assert(conn->pBuff &&
               sizeof(*regMsg) <= Mpp_max_packet_size);
    }

    /* Receive all that is available, up to the expected message size. */
    for (;;)
    {
	    bytesToReceive = conn->pBuff + conn->msgSize - conn->msgPos;
        bytesReceived = read(conn->sockfd, conn->msgPos, bytesToReceive);
        if (bytesReceived == bytesToReceive)
            break;
        else if (bytesReceived > 0)
            conn->msgPos += bytesReceived;
 		else if (bytesReceived == 0)
		{
            ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                            errmsg("Interconnect error reading register message "
                                   "from %s: connection closed",
                                   conn->remoteHostAndPort),
                            errdetail("read() sockfd=%d local=%s",
                                      conn->sockfd, conn->localHostAndPort)
                            ));
		}
        else if (errno == EWOULDBLOCK)
            return false;       /* call me again to receive the rest */
        else if (errno == EINTR)
            ML_CHECK_FOR_INTERRUPTS();
        else
        {
            ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                            errmsg("Interconnect error reading register message "
                                   "from %s", conn->remoteHostAndPort),
                            errdetail("%m%s sockfd=%d local=%s",
                                      "read",
                                      conn->sockfd,
                                      conn->localHostAndPort)
                    ));
        }
    }

    /*
     * Got the whole message.  Convert fields to native byte order.
     */
    regMsg = (RegisterMessage *)conn->pBuff;
    msg.msgBytes = ntohl(regMsg->msgBytes);
    msg.recvSliceIndex = ntohl(regMsg->recvSliceIndex);
    msg.sendSliceIndex = ntohl(regMsg->sendSliceIndex);
    msg.srcContentId = ntohl(regMsg->srcContentId);
    msg.srcListenerPort = ntohl(regMsg->srcListenerPort);
    msg.srcPid = ntohl(regMsg->srcPid);
    msg.srcSessionId = ntohl(regMsg->srcSessionId);
    msg.srcCommandCount = ntohl(regMsg->srcCommandCount);

    /* Check for valid message format. */
    if (msg.msgBytes != sizeof(*regMsg))
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error reading register message "
                               "from %s: format not recognized",
                               conn->remoteHostAndPort),
                        errdetail("msgBytes=%d expected=%d sockfd=%d local=%s",
                                  msg.msgBytes, sizeof(*regMsg),
                                  conn->sockfd, conn->localHostAndPort)
                        ));
    }

    /*
     * Find state info for the specified Motion node.  The sender's slice
     * number equals the motion node id.  (Don't use this ptr until after
     * the bounds are checked, just below.)
     */
    pEntry = &mlsStates[msg.sendSliceIndex-1];

    /* Verify that the message pertains to one of our receiving Motion nodes. */
    if (msg.sendSliceIndex > 0 &&
        msg.sendSliceIndex <= mlsSize &&
        msg.sendSliceIndex == pEntry->motNodeId &&
        pEntry->valid &&
        msg.recvSliceIndex == sliceId &&
        msg.srcContentId >= 0 &&
        msg.srcSessionId == mpp_session_id &&
        msg.srcCommandCount == mpp_command_count)
    {
        ereport(DEBUG3, (errmsg("Interconnect got registration message "
                                "from %s sockfd=%d: recvSlice=%d sendSlice=%d "
                                "srcContentId=%d srcPid=%d",
                                conn->remoteHostAndPort, conn->sockfd,
                                msg.recvSliceIndex, msg.sendSliceIndex,
                                msg.srcContentId, msg.srcPid)
                ));
    }
    else
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error: Invalid registration "
                               "message received from %s.",
                               conn->remoteHostAndPort),
                        errdetail("sendSlice=%d recvSlice=%d srcContentId=%d "
                                  "srcPid=%d srcListenerPort=%d "
                                  "srcSessionId=%d srcCommandCount=%d",
                                  msg.sendSliceIndex, msg.recvSliceIndex,
                                  msg.srcContentId, msg.srcPid,
                                  msg.srcListenerPort, msg.srcSessionId,
                                  msg.srcCommandCount)
                ));
    }

    /*
     * Find and verify the CdbProcess node for the sending process.
     */
    if (list_length(pEntry->sendSlice->primaryProcesses) == 1)
        iconn = 0;
    else
        iconn = msg.srcContentId;

    cdbproc = (CdbProcess *)list_nth(pEntry->sendSlice->primaryProcesses, iconn);

    if (msg.srcContentId != cdbproc->contentid ||
        msg.srcListenerPort != cdbproc->listenerPort ||
        msg.srcPid != cdbproc->pid)
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error: Invalid registration "
                               "message received from %s.",
                               conn->remoteHostAndPort),
                        errdetail("sendSlice=%d srcContentId=%d "
                                  "srcPid=%d srcListenerPort=%d",
                                  msg.sendSliceIndex, msg.srcContentId,
                                  msg.srcPid, msg.srcListenerPort)
                ));
    }

    /*
	 * Allocate MotionConn slot corresponding to sender's position in the
     * sending slice's CdbProc list.
	 */
	newConn = &pEntry->conns[iconn];
	
    if (newConn->sockfd != -1 ||
        newConn->state != mcsNull)
	{
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error: Duplicate registration "
                               "message received from %s.",
                               conn->remoteHostAndPort),
                        errdetail("Already accepted registration from %s for "
                                  "sendSlice=%d srcContentId=%d "
                                  "srcPid=%d srcListenerPort=%d",
                                  newConn->remoteHostAndPort,
                                  msg.sendSliceIndex, msg.srcContentId,
                                  msg.srcPid, msg.srcListenerPort)
                ));
	}

    /* Copy caller's temporary MotionConn to its assigned slot. */
    *newConn = *conn;

    newConn->cdbProc = cdbproc;
    newConn->remoteContentId = msg.srcContentId;

    /*
     * The caller's MotionConn object is no longer valid.
     */
    memset(conn, 0, sizeof(*conn));
    conn->state = mcsNull;

    /*
     * Prepare to begin reading tuples.
     */
    newConn->state = mcsStarted;
    newConn->msgPos = NULL;
    newConn->msgSize = 0;
    newConn->stillActive = true;

    FD_SET(newConn->sockfd, &(pEntry->readSet));

	if (newConn->sockfd > pEntry->highReadSock)
		pEntry->highReadSock = newConn->sockfd;

#ifdef AMS_VERBOSE_LOGGING
	dumpEntryConnections(DEBUG4, pEntry);
#endif

    return true;
}                               /* readRegisterMessage */


/*
 * acceptIncomingConnection
 *
 * accept() a connection request that is pending on the listening socket.
 * Returns a newly palloc'ed MotionConn object; or NULL if the listening
 * socket does not have any pending connection requests.
 */
MotionConn *
acceptIncomingConnection(void)
{
	int			newsockfd;
    socklen_t   addrsize;
	MotionConn *conn;
	struct sockaddr_storage remoteAddr;
	struct sockaddr_storage localAddr;

    /*
     * Accept a connection.
     */
    for (;;)
    {                           /* loop until success or EWOULDBLOCK */
	    memset(&remoteAddr, 0, sizeof(remoteAddr));
	    addrsize = sizeof(remoteAddr);
	    newsockfd = accept(listenerFd, (struct sockaddr *)&remoteAddr, &addrsize);	
	    if (newsockfd >= 0)
            break;

        switch (errno)
        {
        	case EINTR:
                /* A signal arrived.  Loop to retry the accept(). */
                break;

        	case EWOULDBLOCK:
                /* Connection request queue is empty.  Normal return. */
        		return NULL;

            case EBADF:
            case EFAULT:
            case EINVAL:
            case ENOTSOCK:
            case EOPNOTSUPP:
                /* Shouldn't get these errors unless there is a bug. */
                ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                                errmsg("Interconnect error on listener port %d",
                                       Mpp_listener_port),
			                    errdetail("%m%s sockfd=%d",
                                          "accept",
                                          listenerFd)
                        ));
                break;          /* not reached */
            case ENOMEM:
            case ENFILE:
            case EMFILE:
            case ENOBUFS:
                /* Out of resources. */
                ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                                errmsg("Interconnect error on listener port %d",
                                       Mpp_listener_port),
			                    errdetail("%m%s sockfd=%d",
                                          "accept",
                                          listenerFd)
                        ));
                break;          /* not reached */
        	default:
                /* Network problem, connection aborted, etc.  Continue. */
                ereport(LOG, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                              errmsg("Interconnect connection request not "
                                     "completed on listener port %d",
                                      Mpp_listener_port),
			                  errdetail("%m%s sockfd=%d",
                                        "accept",
                                        listenerFd)
                        ));
        }                       /* switch (errno) */
    }                           /* loop until success or EWOULDBLOCK */

    /*
     * Create a MotionConn object to hold the connection state.
     */
    conn = palloc0(sizeof(MotionConn));
    conn->sockfd = newsockfd;
    conn->pBuff = palloc(Mpp_max_packet_size);
    conn->msgSize = 0;
    conn->recvBytes = 0;
    conn->msgPos = 0;
    conn->tupleCount = 0;
    conn->stillActive = false;
    conn->state = mcsAccepted;
    conn->remoteContentId = -2;

    /* Save remote and local host:port strings for error messages. */
    format_sockaddr((struct sockaddr *)&remoteAddr, conn->remoteHostAndPort,
                    sizeof(conn->remoteHostAndPort));
    addrsize = sizeof(localAddr);
    if (getsockname(newsockfd, (struct sockaddr *)&localAddr, &addrsize))
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error after accepting connection."),
	                    errdetail("%m%s sockfd=%d remote=%s",
			                      "getsockname",
                                  newsockfd,
                                  conn->remoteHostAndPort)
                ));
    }
    format_sockaddr((struct sockaddr *)&localAddr, conn->localHostAndPort,
                    sizeof(conn->localHostAndPort));

    /* make socket non-blocking */
    if (!pg_set_noblock(newsockfd))
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error after accepting connection."),
	                    errdetail("%m%s sockfd=%d remote=%s local=%s",
                                  "fcntl(O_NONBLOCK)",
                                  newsockfd,
                                  conn->remoteHostAndPort,
                                  conn->localHostAndPort)
                ));
    }

    ereport(DEBUG4, (errmsg("Interconnect got incoming connection "
                            "to local=%s from remote=%s.  sockfd=%d",
                            conn->localHostAndPort,
                            conn->remoteHostAndPort,
                            newsockfd)
                     ));
    return conn;
}                               /* acceptIncomingConnection */


/* The Function sendChunk() is used to send a tcItem to a single
 * destination. Tuples often are *very small* we aggregate in our
 * local buffer before sending into the kernel.
 *
 * PARAMETERS
 *	 conn - MotionConn that the tcItem is to be sent to.
 *	 tcItem - message to be sent.
 *	 motionId - Node Motion Id.
 */
static bool
sendChunk(MotionConn * conn, TupleChunkListItem tcItem, int16 motionId)
{
	Assert(conn->msgSize > 0);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "sendChunk: msgSize %d this chunk length %d", conn->msgSize, tcItem->chunk_length);
#endif

	if (conn->msgSize + tcItem->chunk_length > Mpp_max_packet_size)
	{
		if (!flushBuffer(conn, motionId))
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
static bool
flushBuffer(MotionConn * conn, int16 motionId)
{
	char	   *sendptr;
	MotionNodeEntry *pMNEntry;
	int			n,
				sent = 0;
	fd_set		wset;
	fd_set		rset;

#ifdef AMS_VERBOSE_LOGGING
	{
		struct timeval snapTime;

		gettimeofday(&snapTime, NULL);
		elog(DEBUG5, "----sending chunk @%s.%d time is %ld.%ld",
			 __FILE__, __LINE__, snapTime.tv_sec, snapTime.tv_usec);
	}
#endif

	pMNEntry = getMotionNodeEntry(motionId, "flushBuffer");

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

		/*
		 * since timeout = 0, select returns imediately and no time is wasted
		 * waiting trying to send data on the network
		 */
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
			ML_CHECK_FOR_INTERRUPTS();
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK)
			{
				do
				{
					timeout = tval;

					ML_CHECK_FOR_INTERRUPTS();

					FD_ZERO(&rset);
					FD_ZERO(&wset);
					FD_SET(conn->sockfd, &wset);
					FD_SET(conn->sockfd, &rset);
					n = select(conn->sockfd + 1, &rset, &wset, NULL, &timeout);
					pMNEntry->sel_wr_wait += (tval.tv_sec - timeout.tv_sec)*1000000 +(tval.tv_usec - timeout.tv_usec);
					if (n < 0)
					{
						if (errno == EINTR)
							continue;

						/*
						 * if we got an error in teardown, ignore it: treat it
						 * as a stop message
						 */
						if (teardownActive)
						{
#ifdef AMS_VERBOSE_LOGGING
							print_connection(conn->sockfd, "stop from");
#endif
							conn->stillActive = false;
							return false;
						}
						ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
										errmsg("Interconnect error writing an outgoing packet"),
						 errdetail("error during select() call (error:%d).\n"
							"For Remote Connection: contentId=%d at %s",
							  errno, conn->remoteContentId,
                              conn->remoteHostAndPort)
                              ));
					}

					/*
					 * as a sender... if there is something to read...
					 * it must mean its a StopSendingMessage.  we
					 * don't even bother to read it.
					 */
					if (FD_ISSET(conn->sockfd, &rset) || teardownActive)
					{
#ifdef AMS_VERBOSE_LOGGING
						print_connection(conn->sockfd, "stop from");
#endif
						conn->stillActive = false;
						return false;
					}
				} while (n < 1);
			}
			else
			{
				/*
				 * if we got an error in teardown, ignore it: treat it as a
				 * stop message
				 */
				if (teardownActive)
				{
#ifdef AMS_VERBOSE_LOGGING
					print_connection(conn->sockfd, "stop from");
#endif
					conn->stillActive = false;
					return false;
				}
				ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error writing an outgoing packet"),
								errdetail("error during write() call (error:%d).\n"
										  "For Remote Connection: contentId=%d at %s",
										  errno, conn->remoteContentId,
                                          conn->remoteHostAndPort)
				                ));
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
static inline void
readPacket(MotionConn * conn)
{
	int			n,
				bytesRead = conn->recvBytes;
	bool		gotHeader = false,
				gotPacket = false;
	fd_set		rset;

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "readpacket: (fd %d) (max %d) outstanding bytes %d", conn->sockfd, Mpp_max_packet_size, conn->recvBytes);
#endif

	/* do we have a complete message waiting to be processed ? */
	if (conn->recvBytes >= PACKET_HEADER_SIZE)
	{
		conn->msgSize = getInt32(conn->msgPos);
		gotHeader = true;
		if (conn->recvBytes >= conn->msgSize)
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG5, "readpacket: returning previously read data (%d)", conn->recvBytes);
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
	elog(DEBUG5, "readpacket: %s on previous call msgSize %d", gotHeader ? "got header" : "no header", conn->msgSize);
#endif

	while (!gotPacket && bytesRead < Mpp_max_packet_size)
	{
		/* see if user canceled and stuff like that */
		ML_CHECK_FOR_INTERRUPTS();

		/*
		 * we read at the end of the buffer, we've eliminated any
		 * slack above
		 */
		if ((n = read(conn->sockfd, conn->pBuff + bytesRead,
					  Mpp_max_packet_size - bytesRead)) < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK)
			{
				do
				{
					struct timeval timeout = tval;

					/* see if user canceled and stuff like that */
					ML_CHECK_FOR_INTERRUPTS();

					FD_ZERO(&rset);
					FD_SET(conn->sockfd, &rset);
					n = select(conn->sockfd + 1, &rset, NULL, NULL, &timeout);
					if (n == 0 || (n < 0 && errno == EINTR))
						continue;
					else if (n < 0)
					{
						ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
										errmsg("Interconnect error reading an incoming packet."),
						                errdetail("%m%s from seg%d at %s",
						                          "select",
                                                  conn->remoteContentId,
                                                  conn->remoteHostAndPort)
						        ));
					}
				}
				while (n < 1);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
					            errmsg("Interconnect error reading an incoming packet."),
						        errdetail("%m%s from seg%d at %s",
						                  "read",
                                          conn->remoteContentId,
                                          conn->remoteHostAndPort)
						));
			}
		}
		else if (n == 0)
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG5, "readpacket(); breaking in while (fd %d) recvBytes %d msgSize %d", conn->sockfd, conn->recvBytes, conn->msgSize);
			print_connection(conn->sockfd, "interconnect error on");
#endif
			ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
				errmsg("Interconnect error: connection closed prematurely."),
				 errdetail("from Remote Connection: contentId=%d at %s",
						   conn->remoteContentId, conn->remoteHostAndPort)
					));
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
	elog(DEBUG5, "readpacket: got %d bytes", conn->recvBytes);
#endif
}

/* Function doBroadcast() is used to send a TupleChunk to all recipients.
 *
 * PARAMETERS
 *	 pEntry - MotionLayerState context that contains everything we need to send.
 *	 tcItem - TupleChunk to send.
 */
static int
doBroadcast(MotionLayerState * pEntry, TupleChunkListItem tcItem)
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
			sendChunk(conn, tcItem, pEntry->motNodeId);
			if (!conn->stillActive)
				inactive++;
		}
	}

	return inactive;
}


void
dumpEntryConnections(int elevel, MotionLayerState *pEntry)
{
	int			i;
    MotionConn *conn;

    if (elevel < log_min_messages)
        return;

    for (i = 0; i < pEntry->numConns; i++)
	{
		conn = &pEntry->conns[i];
        if (conn->sockfd == -1 &&
            conn->state == mcsNull)
            elog(elevel, "... motNodeId=%d conns[%d]:         not connected",
                         pEntry->motNodeId, i);
        else
            elog(elevel, "... motNodeId=%d conns[%d]:  "
                         "%s%d pid=%d sockfd=%d remote=%s local=%s",
		                 pEntry->motNodeId, i,
                         (i < pEntry->numPrimaryConns) ? "seg" : "mir",
                         conn->remoteContentId,
                         conn->cdbProc ? conn->cdbProc->pid : 0,
                         conn->sockfd,
                         conn->remoteHostAndPort,
                         conn->localHostAndPort);
	}
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
void
doSendStopMessage(int16 motNodeID)
{
	MotionLayerState *pEntry;
	MotionConn *conn;
	int			i;
	char		m = 'S';
	ssize_t		written;

	pEntry = getMotionState(motNodeID);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "sending stop message from slice %d to node %d", sliceId, motNodeID);
#endif

	/*
	 * Note: we're only concerned with receivers here, so mirrors
	 * don't matter
	 */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;

		if (conn->sockfd >= 0 &&
            FD_ISSET(conn->sockfd, &pEntry->readSet))
		{
			/* someone is trying to send stuff to us, let's stop 'em */
			while ((written = write(conn->sockfd, &m, sizeof(m))) < 0)
			{
				if (errno == EINTR)
				{
					ML_CHECK_FOR_INTERRUPTS();
					continue;
				}
				else
					break;
			}

			if (written != sizeof(m))
			{
				/*
				 * how can this happen ? the kernel buffer should be
				 * empty in the send direction
				 */
				elog(LOG, "SendStopMessage: failed on write.  %m");
			}
		}
		/* CRITICAL TO AVOID DEADLOCK */
		DeregisterReadInterest(motNodeID, i);
	}
}

void
print_connection(int fd, const char *msg)
{
	struct sockaddr_in local,
				remote;
	socklen_t	len;
	int			errlevel = teardownActive ? LOG : ERROR;

	len = sizeof(remote);
	if (getpeername(fd, (struct sockaddr *) & remote, &len) < 0)
	{
		elog(errlevel, "print_connection(%d, %s): can't get peername err: %m",
			 fd, msg);
	}

	len = sizeof(local);
	if (getsockname(fd, (struct sockaddr *) & local, &len) < 0)
	{
		elog(errlevel, "print_connection(%d, %s): can't get localname err: %m",
			 fd, msg);
	}

	elog(DEBUG2, "%s: w/ports (%d/%d)",
		 msg, ntohs(local.sin_port), ntohs(remote.sin_port));
}


void
format_fd_set(StringInfo buf, int nfds, fd_set *fds, char* pfx, char *sfx)
{
    int     i;

    appendStringInfoString(buf, pfx);
    for (i = 1; i < nfds; i++)
    {
        if (FD_ISSET(i, fds))
            appendStringInfo(buf, "%d,", i);
    }
    if (buf->len > 0 &&
        buf->data[buf->len-1] == ',')
        truncateStringInfo(buf, buf->len - 1);
    appendStringInfoString(buf, sfx);
}


char *
format_sockaddr(struct sockaddr *sa, char* buf, int bufsize)
{
    /* Save remote host:port string for error messages. */
    if (sa->sa_family == AF_INET)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)sa;
        unsigned            s_addr = ntohl(sin->sin_addr.s_addr);

        snprintf(buf, bufsize, "%d.%d.%d.%d:%d",
                 (s_addr >> 24)&0xff,
                 (s_addr >> 16)&0xff,
                 (s_addr >> 8)&0xff,
                 s_addr&0xff,
                 ntohs(sin->sin_port));
    }
#ifdef HAVE_IPV6
    else if (sa->sa_family == AF_INET6)
    {
        struct sockaddr_in6    *sin6 = (struct sockaddr_in6 *)sa;

        if (bufsize > 8)
        {
            inet_ntop(AF_INET6, &sin6->sin6_addr, buf, bufsize - 8);
            buf += strlen(buf);
        }
        snprintf(buf, 8, ":%d", ntohs(sin6->sin6_port));
    }
#endif
    else
        snprintf(buf, bufsize, "?host?:?port?");

    return buf;
}                               /* format_sockaddr */


static void
flushListenerBacklog(void)
{
	int			pendingConn,
				newfd,
				i;
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
			for (i = 0; i < pendingConn; i++)
			{
#ifdef AMS_VERBOSE_LOGGING
				elog(DEBUG3, "flushListenerBacklog(): accepting and closing client conn");
#endif
				peerlen = sizeof(peer);
				newfd = accept(listenerFd, (struct sockaddr *) & peer, &peerlen);
				if (newfd < 0)
				{
					elog(DEBUG2, "flushListenerBacklog: %m%s", "accept");
					continue;
				}
				/* shutdown this socket */
				flushIncomingData(newfd);
				shutdown(newfd, SHUT_WR);
				close(newfd);
			}
		}
		else if (pendingConn < 0 && errno != EINTR)
		{
			ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                            errmsg("%m during listener cleanup") ));
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
 *
 * ----
 * The way it used to work, is we used CHECK_FOR_INTERRUPTS(), and
 * wrapped it in PG_TRY: We *must* return locally; otherwise
 * TeardownInterconnect() can't exit cleanly. So we wrap our
 * cancel-detection checks for interrupts with a PG_TRY block.
 *
 * By swallowing the non-local return on cancel, we lose the "cancel"
 * state (CHECK_FOR_INTERRUPTS() clears QueryCancelPending()). So we
 * should just check QueryCancelPending here ... and avoid calling
 * CHECK_FOR_INTERRUPTS().
 *
 * ----
 *
 * Now we just check explicitly for interrupts (which is, as far as I
 * can tell, the only interrupt-driven state change we care
 * about). This should give us notification of ProcDiePending and
 * QueryCancelPending
 */
static void
waitOnSocketDone(int fd)
{
	int			count,
				n,
				saved_err;
	char		buf;
	struct timeval timeout;
	fd_set		rset;

	for (;;)
	{
		if (InterruptPending)
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "waitOnSocketDone(): interrupt pending fast-track");
#endif
			return;
		}

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
			saved_err = errno;

			if (InterruptPending)
				return;

			/*
			 * Something unexpected, but probably not horrible warn
			 * and return
			 */
			elog(LOG, "TeardownInterconnect: waitOnSocketDone select errno=%d", saved_err);
			break;
		}

		/* ready to read. */
		count = recv(fd, &buf, sizeof(buf), 0);
		if (count == 0)			/* done ! */
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
		elog(LOG, "TeardownInterconnect: waitOnSocketDone %m%s", "recv");
		break;
	}

	return;
}
