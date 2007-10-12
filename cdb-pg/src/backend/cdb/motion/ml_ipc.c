/*-------------------------------------------------------------------------
 * ml_ipc.c
 *	   Motion Layer IPC Layer.
 *
 * Copyright (c) 2005-2007, Greenplum
 *
 * IDENTIFICATION:
 *	   $Id$
 *-------------------------------------------------------------------------
 */
#ifdef WIN32
/*
 * Need this to get WSAPoll (poll). And it
 * has to be set before any header from the Win32 API is loaded.
 */
#define _WIN32_WINNT 0x0600
#endif

#include "postgres.h"

#include <pthread.h>

#include "nodes/execnodes.h"            /* Slice, SliceTable */
#include "nodes/pg_list.h"
#include "nodes/print.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "miscadmin.h"
#include "libpq/libpq-be.h"
#include "libpq/ip.h"
#include "utils/builtins.h"
#include "utils/debugbreak.h"

#include "cdb/cdbselect.h"
#include "cdb/tupchunklist.h"
#include "cdb/ml_ipc.h"
#include "cdb/cdbvars.h"

#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netinet/in.h>

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif

#include "port.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND

/* If we have old platform sdk headers, WSAPoll() might not be there */
#ifndef POLLIN
/* Event flag definitions for WSAPoll(). */

#define POLLRDNORM  0x0100
#define POLLRDBAND  0x0200
#define POLLIN      (POLLRDNORM | POLLRDBAND)
#define POLLPRI     0x0400

#define POLLWRNORM  0x0010
#define POLLOUT     (POLLWRNORM)
#define POLLWRBAND  0x0020

#define POLLERR     0x0001
#define POLLHUP     0x0002
#define POLLNVAL    0x0004

typedef struct pollfd {

    SOCKET  fd;
    SHORT   events;
    SHORT   revents;

} WSAPOLLFD, *PWSAPOLLFD, FAR *LPWSAPOLLFD;
__control_entrypoint(DllExport)
WINSOCK_API_LINKAGE
int
WSAAPI
WSAPoll(
    IN OUT LPWSAPOLLFD fdArray,
    IN ULONG fds,
    IN INT timeout
    );
#endif 

#define poll WSAPoll

/*
 * Postgres normally uses it's own custom select implementation 
 * on Windows, but they haven't implemented execeptfds, which
 * we use here.  So, undef this to use the normal Winsock version
 * for now
 */
#undef select
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
#define MAX_BIND_RETRIES	12
#define CONNECT_RETRY_MS	4000
#define CONNECT_AGGRESSIVERETRY_MS	500


/*=========================================================================
 * GLOBAL STATE VARIABLES
 */

/* listener backlog is calculated at listener-creation time */
int			listenerBacklog = 128;

/* Socket file descriptor for the listener. */
int		listenerFd;
int		UDPlistenerFd;

/* Socket file descriptor for the sequence server. */
int		seqserverFd = -1;
char	*seqServerHost;
int		seqServerPort;

/*
 * Outgoing port assignment
 *
 * To reserve a port number for outgoing connections, we open a dummy
 * listening socket.  Nobody connects to this socket.
 */
int		portReservationFd = -1;
int		outgoingPort = 0;

/* our timeout value for select() and other socket operations. */
struct timeval tval;

#define ML_CHECK_FOR_INTERRUPTS(teardownActive) \
		do {if (!teardownActive && InterruptPending) CHECK_FOR_INTERRUPTS(); } while (0)

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static void setupListeningSocket(int backlog, int *listenerSocketFd, uint16 *listenerPort);
static void setupUDPListeningSocket(int backlog, int *listenerSocketFd, uint16 *listenerPort);

static ChunkTransportStateEntry *createChunkTransportState(ChunkTransportState *transportStates,
													  Slice *sendSlice,
													  Slice *recvSlice,
													  int numPrimaryConns,
													  int numMirrorConns);

static ChunkTransportStateEntry *removeChunkTransportState(ChunkTransportState *transportStates,
														   int16 motNodeID);

static void setupSeqServerConnection(char *hostname, int port);

static ChunkTransportStateEntry *startOutgoingConnections(ChunkTransportState *transportStates,
													 Slice *sendSlice, 
													 int *pOutgoingCount);
static void setupOutgoingConnection(ChunkTransportState *transportStates,
									ChunkTransportStateEntry *pEntry, MotionConn *conn);
static void updateOutgoingConnection(ChunkTransportState *transportStates,
									 ChunkTransportStateEntry *pEntry, MotionConn *conn, int errnoSave);

static ChunkTransportStateEntry *startOutgoingUDPConnections(ChunkTransportState *transportStates,
													 Slice *sendSlice,
													 int *pOutgoingCount);

static void setupOutgoingUDPConnection(ChunkTransportState *transportStates,
									   ChunkTransportStateEntry *pEntry, MotionConn *conn);

static void sendRegisterMessage(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn * conn);
static bool readRegisterMessage(ChunkTransportState *transportStates,
								MotionConn *conn);
static MotionConn *acceptIncomingConnection(void);

static bool sendChunk(MotionLayerState *mlStates, ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn * conn, TupleChunkListItem tcItem, int16 motionId);
static inline void readPacket(MotionConn * conn, bool inTeardown);
static inline int doBroadcast(MotionLayerState *mlStates, ChunkTransportState *transportStates,
							  ChunkTransportStateEntry *pEntry, TupleChunkListItem tcItem);

static bool flushBuffer(MotionLayerState *mlStates, ChunkTransportState *transportStates,
						ChunkTransportStateEntry *pEntry, MotionConn * conn, int16 motionId);

#ifdef AMS_VERBOSE_LOGGING
static void	dumpEntryConnections(int elevel, ChunkTransportStateEntry *pEntry);
static void	print_connection(ChunkTransportState *transportStates, int fd, const char *msg);
#endif

static void format_fd_set(StringInfo buf, int nfds, mpp_fd_set fds, char* pfx, char *sfx);
static char *format_sockaddr(struct sockaddr *sa, char* buf, int bufsize);

static void waitOnSocketDone(int);

static void flushInterconnectListenerBacklog(void);

static inline TupleChunkListItem RecvTupleChunk(MotionConn *conn, bool inTeardown);

static void TeardownUDPInterconnect(ChunkTransportState *transportStates, MotionLayerState *mlStates, bool forceEOS);

static bool flushUDPBuffer(MotionLayerState *mlStates, ChunkTransportState *transportStates, 
						   ChunkTransportStateEntry *pEntry, MotionConn * conn, int16 motionId);

static void *ic_rx_thread_func(void *arg);

/* Global hash table for connections. */

struct conn_htab_bin {
	MotionConn *conn;
	struct conn_htab_bin *next;
};

struct conn_htab_bin	**conn_htab = NULL;
#define DEFAULT_CONN_HTAB_SIZE (Max((GpIdentity.numsegments*Gp_interconnect_hash_multiplier), 16))
int						conn_htab_size = 0;
static pthread_mutex_t	conn_htab_mutex = PTHREAD_MUTEX_INITIALIZER;

/* synchronization */
#define RX_THREAD_POLL_TIMEOUT (250) /* 1/4 sec in msec */

#define MAIN_THREAD_COND_TIMEOUT (250000) /* 1/4 second in usec */
static pthread_t ic_rx_thread;
static pthread_mutex_t	rx_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool rx_thread_shutdown = false;
static bool rx_thread_shutdown_complete = false;

static pthread_cond_t rx_thread_cond = PTHREAD_COND_INITIALIZER;
static bool rx_thread_waiting = false;

static int	rx_buf_maxcount = 1;
static int	rx_buf_count = 0;
static char	*rx_buf_freelist = NULL;

static bool main_thread_waiting = false;
static int main_thread_waiting_node = -1;
static int main_thread_waiting_route = ANY_ROUTE;

/* retry fallback definitions */
#define UDP_CONNECT_RETRY_MS	(100)

#define MAX_TRY (11)
int
timeouts[] =
{
	0,
	1,
	2,
	4,
	8,
	16,
	32,
	64,
	128,
	256,
	512,
	512 /* MAX_TRY*/
};

#define TIMEOUT(try) (try < MAX_TRY ? (timeouts[try]) : (timeouts[MAX_TRY]))

static uint64	ic_stats_pkt_count=0;
static uint64	ic_stats_dropped_acks=0;
static uint64	ic_stats_retrans=0;
static uint64	ic_stats_mismatch=0;
static uint64	ic_stats_rx_overrun=0;

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

#define getChunkTransportState(transportState, motNodeID, ppEntry) \
	do { \
		Assert((transportState) != NULL);		\
		if ((motNodeID) > 0 &&					\
			(transportState) &&					 \
			(motNodeID) <= (transportState)->size &&					\
			(transportState)->states[(motNodeID)-1].motNodeId == (motNodeID) && \
			(transportState)->states[(motNodeID)-1].valid)				\
		{ \
			*(ppEntry) = &(transportState)->states[(motNodeID) - 1];	\
		} \
		else \
		{ \
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR), \
							errmsg("Interconnect Error: Unexpected Motion Node Id: %d.  This means" \
								   " a motion node that wasn't setup is requesting interconnect" \
								   " resources.", (motNodeID))));		\
			/* not reached */ \
		} \
	} while (0)

/* Function doBroadcast() is used to send a TupleChunk to all recipients.
 *
 * PARAMETERS
 *	 pEntry - ChunkTransportState context that contains everything we need to send.
 *	 tcItem - TupleChunk to send.
 */
static inline int
doBroadcast(MotionLayerState *mlStates, ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, TupleChunkListItem tcItem)
{
	MotionConn *conn;
	int			i, inactive = 0;

	/* add our tcItem to each of the outgoing buffers. */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;
		/* only send to still interested receivers. */
		if (conn->stillActive)
		{
			sendChunk(mlStates, transportStates, pEntry, conn, tcItem, pEntry->motNodeId);

			if (!conn->stillActive)
				inactive++;
		}
	}

	return inactive;
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
readPacket(MotionConn *conn, bool inTeardown)
{
	int			n,
		bytesRead = conn->recvBytes;
	bool		gotHeader = false,
		gotPacket = false;
	mpp_fd_set	rset;

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "readpacket: (fd %d) (max %d) outstanding bytes %d", conn->sockfd, Gp_max_packet_size, conn->recvBytes);
#endif

	/* do we have a complete message waiting to be processed ? */
	if (conn->recvBytes >= PACKET_HEADER_SIZE)
	{
		memcpy(&conn->msgSize, conn->msgPos, sizeof(uint32));
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

	while (!gotPacket && bytesRead < Gp_max_packet_size)
	{
		/* see if user canceled and stuff like that */
		ML_CHECK_FOR_INTERRUPTS(inTeardown);

		/*
		 * we read at the end of the buffer, we've eliminated any
		 * slack above
		 */
		if ((n = recv(conn->sockfd, conn->pBuff + bytesRead,
					  Gp_max_packet_size - bytesRead, 0)) < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK)
			{
				do
				{
					struct timeval timeout = tval;

					/* see if user canceled and stuff like that */
					ML_CHECK_FOR_INTERRUPTS(inTeardown);

					MPP_FD_ZERO(&rset);
					MPP_FD_SET(conn->sockfd, &rset);
					n = select(conn->sockfd + 1, (fd_set *)&rset, NULL, NULL, &timeout);
					if (n == 0 || (n < 0 && errno == EINTR))
						continue;
					else if (n < 0)
					{
						ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
										errmsg("Interconnect error reading an incoming packet."),
						                errdetail("%m%s from seg%d at %s",
						                          "select",
                                                  conn->remoteContentId,
                                                  conn->remoteHostAndPort)));
					}
				}
				while (n < 1);
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					            errmsg("Interconnect error reading an incoming packet."),
						        errdetail("%m%s from seg%d at %s",
						                  "read",
                                          conn->remoteContentId,
                                          conn->remoteHostAndPort)));
			}
		}
		else if (n == 0)
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG5, "readpacket(); breaking in while (fd %d) recvBytes %d msgSize %d", conn->sockfd, conn->recvBytes, conn->msgSize);
			print_connection(transportStates, conn->sockfd, "interconnect error on");
#endif
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error: connection closed prematurely."),
							errdetail("from Remote Connection: contentId=%d at %s",
									  conn->remoteContentId, conn->remoteHostAndPort)));
			break;
		}
		else
		{
			bytesRead += n;

			if (!gotHeader && bytesRead >= PACKET_HEADER_SIZE)
			{
				/* got the header */
				memcpy(&conn->msgSize, conn->msgPos, sizeof(uint32));
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

static inline TupleChunkListItem
RecvTupleChunk(MotionConn *conn, bool inTeardown)
{
	TupleChunkListItem tcItem;
	TupleChunkListItem firstTcItem = NULL;
	TupleChunkListItem lastTcItem = NULL;
	uint16		tcSize;
	int			bytesProcessed = 0;

	if (Gp_interconnect_type == INTERCONNECT_TYPE_TCP)
	{
		/* read the packet in from the network. */
		readPacket(conn, inTeardown);

		/* go through and form us some TupleChunks. */
		bytesProcessed = PACKET_HEADER_SIZE;
	}
	else
	{
		/* go through and form us some TupleChunks. */
		bytesProcessed = sizeof(struct icpkthdr);
	}

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "read packet recv bytes %d msgsize %d", conn->recvBytes, conn->msgSize);
#endif

#if 0
	elog(LOG, "recvtuple chunk conn->pBuff %p conn->msgPos: %p", conn->pBuff, conn->msgPos);
#endif

	while (bytesProcessed != conn->msgSize)
	{
		tcSize = TUPLE_CHUNK_HEADER_SIZE + (*(uint16 *) (conn->msgPos + bytesProcessed));

		/* we only check for interrupts here when we don't have a guaranteed full-message */
		if (Gp_interconnect_type == INTERCONNECT_TYPE_TCP)
		{
			if (tcSize >= conn->msgSize)
			{
				/* it is possible that our message got messed up by a
				 * cancellation ? */
				ML_CHECK_FOR_INTERRUPTS(inTeardown);

				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error parsing message"),
								errdetail("tcSize %d >= conn->msgSize %d", tcSize, conn->msgSize)));
			}
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

/* MUST BE CALLED WITH rx_thread_mutex HELD! */
static inline bool
wait_on_receiver(int timeout_us)
{
	struct timeval tv;
	struct timespec ts;
	int wait;

	gettimeofday(&tv, NULL);
	ts.tv_sec = tv.tv_sec;
	/*  leave in ms for this */
	ts.tv_nsec = (tv.tv_usec + timeout_us);
	if (ts.tv_nsec > 1000000)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000;
	}
	ts.tv_nsec *= 1000; /* convert usec to nsec */

#if 0
	elog(LOG, "waiting (timed) on route %d %s", main_thread_waiting_route, 
		 (main_thread_waiting_route == ANY_ROUTE ? "(any route)" : ""));
#endif

	/* we can't handle interrupts for the time we're waiting,
	 * which is why we're using timedwaits in the first place */
	HOLD_INTERRUPTS();

	wait = pthread_cond_timedwait(&rx_thread_cond, &rx_thread_mutex, &ts);

	RESUME_INTERRUPTS();

	if (wait == ETIMEDOUT)
	{
		/* condition not met */
		return false;
	}

	/* we didn't time out, condition met! */
	return true;
}

static void
conn_addhash(MotionConn *conn)
{
	int hashcode;
	struct conn_htab_bin *bin, *newbin;

	hashcode = ((conn->conn_info.srcPid ^ conn->conn_info.dstPid) + conn->conn_info.dstContentId) % conn_htab_size;

	pthread_mutex_lock(&conn_htab_mutex);

	/* check for collision ? */
	for (bin = conn_htab[hashcode]; bin != NULL; bin = bin->next)
	{
		if (bin->conn->conn_info.motNodeId == conn->conn_info.motNodeId &&
			bin->conn->conn_info.dstContentId == conn->conn_info.dstContentId &&
			bin->conn->conn_info.srcContentId == conn->conn_info.srcContentId &&
			bin->conn->conn_info.srcPid == conn->conn_info.srcPid &&
			bin->conn->conn_info.dstPid == conn->conn_info.dstPid)
		{
			elog(LOG, "conn_addhash(): duplicate ?! node %d route %d", conn->conn_info.motNodeId, conn->route);
			pthread_mutex_unlock(&conn_htab_mutex);
			return;
		}
	}

	newbin = (struct conn_htab_bin *)malloc(sizeof(struct conn_htab_bin));
	newbin->conn = conn;
	newbin->next = conn_htab[hashcode];
	conn_htab[hashcode] = newbin;

	pthread_mutex_unlock(&conn_htab_mutex);
}

static void
conn_delhash(MotionConn *conn)
{
	int hashcode;
	struct conn_htab_bin *c, *p, *trash;

	hashcode = ((conn->conn_info.srcPid ^ conn->conn_info.dstPid) + conn->conn_info.dstContentId) % conn_htab_size;

	pthread_mutex_lock(&conn_htab_mutex);

	c = conn_htab[hashcode];

	/* find entry */
	p = NULL;
	while (c != NULL)
	{
		if (c->conn->conn_info.motNodeId == conn->conn_info.motNodeId &&
			c->conn->conn_info.dstContentId == conn->conn_info.dstContentId &&
			c->conn->conn_info.srcContentId == conn->conn_info.srcContentId &&
			c->conn->conn_info.srcPid == conn->conn_info.srcPid &&
			c->conn->conn_info.dstPid == conn->conn_info.dstPid)

			break;

		p = c;
		c = c->next;
	}

	/* not found ? */
	if (c == NULL)
	{
		pthread_mutex_unlock(&conn_htab_mutex);
		return;
	}

	/* found the connection, remove from the chain. */
	trash = c;	

	if (p == NULL)
		conn_htab[hashcode] = c->next;
	else
		p->next = c->next;

	pthread_mutex_unlock(&conn_htab_mutex);

	free(trash);
	return;
}

static MotionConn *
findConnByHeader(struct icpkthdr *hdr, int16 *srcRoute)
{
	int hashcode;
	struct conn_htab_bin *bin;
	struct MotionConn *ret=NULL;

	hashcode = ((hdr->srcPid ^ hdr->dstPid) + hdr->dstContentId) % conn_htab_size;

	pthread_mutex_lock(&conn_htab_mutex);

	for (bin = conn_htab[hashcode]; bin != NULL; bin = bin->next)
	{
#if 0
		write_log("findConnByHeader: scanning code %d. node %d srccontent %d "
					 "srcpid %d dstpid %d dstcontent %d conn %p\n", hashcode,
					 bin->conn->conn_info.motNodeId, bin->conn->conn_info.srcContentId,
					 bin->conn->conn_info.srcPid, bin->conn->conn_info.dstPid,
					 bin->conn->conn_info.dstContentId, bin->conn);
#endif
		if (bin->conn->conn_info.motNodeId == hdr->motNodeId &&
			bin->conn->conn_info.dstContentId == hdr->dstContentId &&
			bin->conn->conn_info.srcContentId == hdr->srcContentId &&
			bin->conn->conn_info.srcPid == hdr->srcPid &&
			bin->conn->conn_info.dstPid == hdr->dstPid)
		{
			ret = bin->conn;

#if 0
			write_log("findConnByHeader: found. route %d state %d hashcode %d conn %p\n", ret->route, ret->state, hashcode, ret);
#endif

			if (srcRoute != NULL)
				*srcRoute = ret->route;

			if (hdr->sessionId != gp_session_id)
				write_log("findConnByHeader: session mismatch %d/%d\n", hdr->sessionId, gp_session_id);

			if (hdr->commandCount != gp_command_count)
				write_log("findConnByHeader: command mismatch %d/%d\n", hdr->commandCount, gp_command_count);

			break;
		}
	}

	pthread_mutex_unlock(&conn_htab_mutex);

#if 0
	if (ret == NULL)
		write_log("findConnByHeader: not found! "
				  "(hdr->srcPid %d hdr->srcContentId %d hdr->dstContentId %d hdr->dstPid %d sess(%d:%d) cmd(%d:%d)) hashcode %d\n",
					 hdr->srcPid, hdr->srcContentId, hdr->dstContentId, hdr->dstPid, hdr->sessionId,
					 gp_session_id, hdr->commandCount, gp_command_count, hashcode);
#endif

	return ret;
}

/*
 * This must be called with the rx_thread mutex held (maybe these locks need to be restructured a bit).
 */
static void
conn_rehash(void)
{
	int i, new_hashcode;
	int new_htab_size = DEFAULT_CONN_HTAB_SIZE;
	struct conn_htab_bin		**new_conn_htab = NULL;
	struct conn_htab_bin		*bin, *newbin;

	new_conn_htab = (struct conn_htab_bin **)malloc(new_htab_size * sizeof(struct conn_htab_bin *));

	if (new_conn_htab == NULL)
	{
		elog(ERROR, "Could not resize connection hash table (old size %d, new size %d)",
			 conn_htab_size, new_htab_size);
	}

	for (i=0; i < new_htab_size; i++)
		new_conn_htab[i] = NULL;

	pthread_mutex_lock(&conn_htab_mutex);

	/* For each bucket in the hash table */
	for (i=0; i < conn_htab_size; i++)
	{
		/* for each entry in this bucket */
		bin = conn_htab[i];

		while (bin)
		{
			MotionConn *conn=bin->conn;

			new_hashcode = ((conn->conn_info.srcPid ^ conn->conn_info.dstPid) +
							conn->conn_info.dstContentId) % new_htab_size;

			/* save a reference to this bin */
			newbin = bin;
			
			/* advance to the next bin in the old table */
			bin = bin->next;

			/* add this bin to the new table */
			newbin->next = new_conn_htab[new_hashcode];;
		}
	}

	free(conn_htab);
	conn_htab_size = new_htab_size;
	conn_htab = new_conn_htab;

	pthread_mutex_unlock(&conn_htab_mutex);
}

/*=========================================================================
 * VISIBLE FUNCTIONS
 */

/* See ml_ipc.h */
void
InitMotionLayerIPC(void)
{
	int i;
	uint16 tcp_listener;
	uint16 udp_listener = 0;

	tval.tv_sec = 0;
	tval.tv_usec = 500000;

	/*activated = false;*/
	seqserverFd = -1;

#ifdef pg_on_solaris
	listenerBacklog = Min(1024, Max(GpIdentity.numsegments * 4, listenerBacklog));
#endif

	/* Listen for connections from qExecs. */
	setupListeningSocket(listenerBacklog, &listenerFd, &tcp_listener);
	setupUDPListeningSocket(listenerBacklog, &UDPlistenerFd, &udp_listener);

	Gp_listener_port = (udp_listener<<16) | tcp_listener;

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP || log_min_messages >= DEBUG1)
		elog(LOG, "Interconnect listening on tcp port %d udp port %d (0x%x)", tcp_listener, udp_listener, Gp_listener_port);

	/* set up the connection hash table */
	conn_htab_size = DEFAULT_CONN_HTAB_SIZE;
	conn_htab = (struct conn_htab_bin **)malloc(conn_htab_size * sizeof(struct conn_htab_bin *));

	for (i=0; i < conn_htab_size; i++)
		conn_htab[i] = NULL;

	/* Set up our mutex */
	{
		pthread_mutexattr_t m_atts;

		pthread_mutexattr_init(&m_atts);
		//pthread_mutexattr_settype(&m_atts, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutexattr_settype(&m_atts, PTHREAD_MUTEX_ERRORCHECK);

		pthread_mutex_init(&rx_thread_mutex, &m_atts);
	}

	/* Start up our rx-thread */
	{
		int pthread_err;

		/* attributes of the thread we're creating */
		pthread_attr_t t_atts;

		/* save ourselves some memory: the defaults for thread stack
		 * size are large (1M+) */
		pthread_attr_init(&t_atts);

#ifdef pg_on_solaris
		/* Solaris doesn't have PTHREAD_STACK_MIN ? */
		pthread_attr_setstacksize(&t_atts, (64*1024));
#else
		pthread_attr_setstacksize(&t_atts, Max(PTHREAD_STACK_MIN, (64*1024)));
#endif
		pthread_err = pthread_create(&ic_rx_thread, &t_atts, ic_rx_thread_func, NULL);

		pthread_attr_destroy(&t_atts);
		if (pthread_err != 0)
		{
			pthread_join(ic_rx_thread, NULL);

			ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("InitMotionLayerIPC: failed to create thread"),
							errdetail("pthread_create() failed with err %d", pthread_err)));
		}
	}
}


/* See ml_ipc.h */
void
CleanUpMotionLayerIPC(void)
{
	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
    	elog(DEBUG3, "Cleaning Up Motion Layer IPC...");

	elog(DEBUG2, "udp-ic: telling receiver thread to shutdown.");

	rx_thread_shutdown = true;

	pthread_join(ic_rx_thread, NULL);

	elog(DEBUG2, "udp-ic: receiver thread shutdown.");

	/* close down the Interconnect listener socket. */
    if (listenerFd >= 0)
        closesocket(listenerFd);

	if (UDPlistenerFd >= 0)
		closesocket(UDPlistenerFd);

    if (portReservationFd >= 0)
        closesocket(portReservationFd);

	/* be safe and reset global state variables. */
	Gp_listener_port = 0;
	listenerFd = -1;
    portReservationFd = -1;

	{
		int i;

		for (i=0; i < conn_htab_size; i++)
		{
			struct conn_htab_bin *trash;

			while (conn_htab[i] != NULL)
			{
				trash = conn_htab[i];
				conn_htab[i] = trash->next;
				free(trash);
			}
		}
		free(conn_htab);
	}
}

/* See ml_ipc.h */
bool
SendTupleChunkToAMS(MotionLayerState *mlStates,
					ChunkTransportState *transportStates,
					int16 motNodeID,
					int16 targetRoute,
					TupleChunkListItem tcItem)
{
	int			i,
		recount = 0;
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn *conn;
	TupleChunkListItem currItem;

	Insist(transportStates && transportStates->activated);

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "sendtuplechunktoams: calling get_transport_state w/transportStates %p transportState->size %d motnodeid %d",
		 transportStates, transportStates->size, motNodeID);
#endif

	getChunkTransportState(transportStates, motNodeID, &pEntry);

	/*
	 * tcItem can actually be a chain of tcItems.  we need to send out all of
	 * them.
	 */
	currItem = tcItem;
			
	for (currItem = tcItem; currItem != NULL; currItem = currItem->p_next)
	{
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "SendTupleChunkToAMS: chunk length %d", currItem->chunk_length);
#endif

		if (targetRoute == BROADCAST_SEGIDX)
		{
			if (doBroadcast(mlStates, transportStates, pEntry, currItem) != 0)
				recount = 1;
		}
		else
		{
			/* handle pt-to-pt message. Primary */
			conn = pEntry->conns + targetRoute;
			/* only send to interested connections */
			if (conn->stillActive)
			{
				sendChunk(mlStates, transportStates, pEntry, conn, currItem, motNodeID);
				if (!conn->stillActive)
					recount = 1;
			}

			/* handle pt-to-pt message. Mirror */
			if (pEntry->numConns > pEntry->numPrimaryConns)
			{
				conn = pEntry->conns + targetRoute + pEntry->numPrimaryConns;
				/* only send to interested connections */
				if (conn->stillActive)
				{
					sendChunk(mlStates, transportStates, pEntry, conn, currItem, motNodeID);
					if (!conn->stillActive)
						recount = 1;
				}
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
SendEosToAMS(MotionLayerState *mlStates,
			 ChunkTransportState *transportStates,
			 int motNodeID,
			 TupleChunkListItem tcItem)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn *conn;
	int			i;

	Insist(transportStates && (transportStates->activated || transportStates->teardownActive));

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

	getChunkTransportState(transportStates, motNodeID, &pEntry);

	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG3, "Interconnect seg%d slice%d sending end-of-stream to slice%d",
			 Gp_segment, motNodeID, pEntry->recvSlice->sliceIndex);

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
		pEntry->sendingEos = true;

	/* we want to add our tcItem onto each of the outgoing buffers --
	 * this is guaranteed to leave things in a state where a flush is
	 * *required*. */
	doBroadcast(mlStates, transportStates, pEntry, tcItem);

	/* now flush all of the buffers. */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;

		if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP &&
			conn->state == mcsStarted)
		{
#if 0
			elog(LOG, "sent eos to route %d tuplecount %d seq %d", conn->route, conn->tupleCount, conn->conn_info.seq);
#endif

			flushUDPBuffer(mlStates, transportStates, pEntry, conn, motNodeID);
			conn->state = mcsEosSent;
			conn->stillActive = false;
		}
		else if (conn->sockfd >= 0 &&
				 conn->state == mcsStarted)
			flushBuffer(mlStates, transportStates, pEntry, conn, motNodeID);

#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "SendEosToAMS() Leaving");
#endif
	}
}

/* we use bit operations to test these, flags are powers of two only*/
#define FLAGS_ACK  (1)
#define FLAGS_STOP (2)
#define FLAGS_EOS  (4)

static void
sendAck(MotionConn *conn, bool stop)
{
	int n;
	struct icpkthdr msg;

	if (stop)
		conn->conn_info.flags |= FLAGS_STOP;

	MemSet(&msg, 0, sizeof(msg));
	memcpy(&msg, &conn->conn_info, sizeof(msg));
	msg.flags |= FLAGS_ACK;

	msg.len = 0;

	if (DEBUG5 >= log_min_messages)
		write_log("sendack: flags 0x%x node %d route %d seq %d\n",
				  msg.flags, msg.motNodeId, conn->route, conn->conn_info.seq);

	n = sendto(UDPlistenerFd, (const char *)&msg, sizeof(msg), 0,
			   (struct sockaddr *)&conn->peer, sizeof(conn->peer));

	if (n < sizeof(msg))
		write_log("sendack: got error %d errno %d route %d seq %d\n",
				  n, errno, conn->route, conn->conn_info.seq);
}

/* should be called with  rx_thread_mutex *locked* */
static void
put_rx_buffer(MotionConn *conn)
{
	struct icpkthdr *buf=NULL;

	buf = (struct icpkthdr *)conn->pBuff;

	Assert(buf != NULL);

	Assert((uint8 *)buf == conn->pkt_q[conn->pkt_q_head]);

	conn->pkt_q[conn->pkt_q_head] = NULL;
	conn->pkt_q_head++;
	if (conn->pkt_q_head == Gp_interconnect_queue_depth)
		conn->pkt_q_head = 0;
	conn->pkt_q_size--;

#if 0
	elog(DEBUG5, "put_rx_buffer: buff %p", buf);
#endif

	*(char **)buf = rx_buf_freelist;
	rx_buf_freelist = (char *)buf;

	if (conn->pkt_q_size == 0)
		conn->pBuff = NULL;
	else
	{
		/* update the protocol-specific info of the newly active rx-buffer */
		conn->pBuff = conn->pkt_q[conn->pkt_q_tail];
		buf = (struct icpkthdr *)conn->pBuff;

		conn->msgPos = conn->pBuff;

		conn->msgSize = buf->len;
		conn->recvBytes = conn->msgSize;
	}

	if (rx_thread_waiting)
	{
		pthread_cond_signal(&rx_thread_cond);
		elog(DEBUG2, "put_rx_buffer: waking rx thread");
	}
}

/*
 * The cdbmotion code has discarded our pointer to the motion-conn
 * structure, but has enough info to fully specify it.
 */
void
ml_put_rx_buffer(ChunkTransportState *transportStates, int motNodeID, int route)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn			*conn=NULL;

	getChunkTransportState(transportStates, motNodeID, &pEntry);

	conn = pEntry->conns + route;

	pthread_mutex_lock(&rx_thread_mutex);

	put_rx_buffer(conn);

	pthread_mutex_unlock(&rx_thread_mutex);
}

/* should be called with rx_thread_mutex *locked* */
static struct icpkthdr *
get_rx_buffer(void)
{
	struct icpkthdr *ret=NULL;

	do
	{
		if (rx_buf_freelist == NULL)
		{
			if (rx_buf_count > rx_buf_maxcount)
			{
				if (DEBUG3 >= log_min_messages)
					write_log("Interconnect ran out of rx-buffers");
				break;
			}
			ret = (struct icpkthdr *)malloc(Gp_max_packet_size);
			rx_buf_count++;

			break;
		}

		/* we have buffers available in our freelist */
		ret = (struct icpkthdr *)rx_buf_freelist;
		rx_buf_freelist = *(char **)(rx_buf_freelist);
	} while (0);

	return ret;
}

/* ALWAYS CALLED WITH THREAD MUTEX *LOCKED* BEFORE RETURNING, *MUST* UNLOCK. */
static TupleChunkListItem
receiveChunksUDP(ChunkTransportStateEntry *pEntry, int16 motNodeID, int16 *srcRoute, MotionConn *conn, bool inTeardown)
{
	bool		directed=false;
	MotionConn	*rxconn = NULL;
	TupleChunkListItem	tcItem=NULL;

	elog(DEBUG5, "receivechunksUDP: motnodeid %d", motNodeID);

	/* directed receive ? */
	if (conn != NULL)
	{
		directed = true;
		*srcRoute = conn->route;
		main_thread_waiting_route = conn->route;
	}
	else
	{
		/* non-directed receive */
		main_thread_waiting_route = ANY_ROUTE;
	}

	main_thread_waiting_node = motNodeID;
	main_thread_waiting = true;

	/* we didn't have any data, so we've got to read it from the network. */
	for (;;)
	{
		/* 1. Do we have data ready */
		if (directed && conn->pBuff)
		{
			rxconn = conn;
		}
		else if (!directed && main_thread_waiting_route != ANY_ROUTE)
		{
			rxconn = pEntry->conns + main_thread_waiting_route;
			elog(DEBUG3, "receiveChunksUDP: non-directed rx woke on route %d (%p)", main_thread_waiting_route, rxconn->pBuff);
		}

		if (rxconn != NULL)
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(LOG, "got data with length %d", rxconn->recvBytes);
#endif

			/* successfully read into this connection's buffer. */
			tcItem = RecvTupleChunk(rxconn, inTeardown);

			if (!directed)
				*srcRoute = rxconn->route;

			main_thread_waiting_route = ANY_ROUTE;
			main_thread_waiting_node = -1;
			main_thread_waiting = false;

			pthread_mutex_unlock(&rx_thread_mutex);

			return tcItem;
		}

		/* 2. Wait for data to become ready */
		if (wait_on_receiver(MAIN_THREAD_COND_TIMEOUT))
			continue; /* success ! */

		/* handle timeout, check for cancel */
#if 0
		elog(LOG, "timed out waiting for data");
#endif
		pthread_mutex_unlock(&rx_thread_mutex);

		ML_CHECK_FOR_INTERRUPTS(inTeardown);

		/* if we made it here we need the lock again. */
		pthread_mutex_lock(&rx_thread_mutex);

		continue;
	}

	/* We either got data, or get cancelled. We never make it out to
	 * here. */
	Insist(0);
}

static TupleChunkListItem
RecvTupleChunkFromAnyUDP(MotionLayerState *mlStates,
						 ChunkTransportState *transportStates,
						 int16 motNodeID,
						 int16 *srcRoute)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn			*conn=NULL;
	int					i, index, activeCount=0;
	TupleChunkListItem	tcItem=NULL;

	getChunkTransportState(transportStates, motNodeID, &pEntry);

    index = pEntry->scanStart;

	pthread_mutex_lock(&rx_thread_mutex);
	for (i = 0; i < pEntry->numConns; i++, index++)
	{
		if (index >= pEntry->numConns)
			index = 0;

		conn = pEntry->conns + index;

		if (conn->stillActive)
			activeCount++;

		if (conn->pBuff != NULL)
		{
			tcItem = RecvTupleChunk(conn, transportStates->teardownActive);
			*srcRoute = conn->route;

			pthread_mutex_unlock(&rx_thread_mutex);

			pEntry->scanStart = index + 1;

			return tcItem;
		}
	}

	/* no data pending in our queue */

#ifdef AMS_VERBOSE_LOGGING
	elog(LOG, "recvtuplechunkfromanyudp(): activeCount is %d", activeCount);
#endif
	if (activeCount == 0)
		return NULL;

	/* receiveChunksUDP() releases rx_thread_mutex as a side-effect */
	tcItem = receiveChunksUDP(pEntry, motNodeID, srcRoute, NULL, transportStates->teardownActive);

	pEntry->scanStart = *srcRoute + 1;

	return tcItem;
}

static TupleChunkListItem
RecvTupleChunkFromUDP(ChunkTransportState *transportStates,
					  int16		motNodeID,
					  int16		srcRoute)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn			*conn;
	int16				route;

#ifdef AMS_VERBOSE_LOGGING
	elog(LOG, "recvtuplechunkfromudp().");
#endif

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "RecvTupleChunkFromUDP(motNodID=%d, srcRoute=%d)", motNodeID, srcRoute);
#endif

	getChunkTransportState(transportStates, motNodeID, &pEntry);
	conn = pEntry->conns + srcRoute;

#ifdef AMS_VERBOSE_LOGGING
	if (!conn->stillActive)
	{
		elog(LOG, "recvtuplechunkfromudp(): connection inactive ?!");
	}
#endif

	if (!conn->stillActive)
		return NULL;

	pthread_mutex_lock(&rx_thread_mutex);
	if (conn->pBuff != NULL)
	{
			TupleChunkListItem	tcItem=NULL;

			tcItem = RecvTupleChunk(conn, transportStates->teardownActive);

			pthread_mutex_unlock(&rx_thread_mutex);
			return tcItem;
}

	/* no existing data, we've got to read a packet */
	/* receiveChunksUDP() releases rx_thread_mutex as a side-effect */
	return receiveChunksUDP(pEntry, motNodeID, &route, conn, transportStates->teardownActive);
}

/* See ml_ipc.h */
TupleChunkListItem
RecvTupleChunkFromAny(MotionLayerState *mlStates,
					  ChunkTransportState *transportStates,
					  int16 motNodeID,
					  int16 *srcRoute)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionNodeEntry  *pMNEntry;
	MotionConn *conn;
	TupleChunkListItem tcItem;
	mpp_fd_set	rset;
	int			n,
		i,
		index;
	bool		skipSelect = false;

	Insist(transportStates && transportStates->activated);

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
		return RecvTupleChunkFromAnyUDP(mlStates, transportStates, motNodeID, srcRoute);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "RecvTupleChunkFromAny(motNodeId=%d)", motNodeID);
#endif

	getChunkTransportState(transportStates, motNodeID, &pEntry);
	pMNEntry = getMotionNodeEntry(mlStates, motNodeID, "RecvTupleChunkFromAny");
	do
	{
		struct timeval timeout = tval;

		/* make sure we check for these. */
		ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

		memcpy(&rset, &pEntry->readSet, sizeof(mpp_fd_set));

		/*
		 * since we may have data in a local buffer, we may be able to
		 * short-circuit the select() call (and if we don't do this we may
		 * wait when we have data ready, since it has already been read)
		 */
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->sockfd >= 0 &&
                MPP_FD_ISSET(conn->sockfd, &rset) &&
                conn->recvBytes != 0)
			{
				/* we have data on this socket, let's short-circuit our select */
				MPP_FD_ZERO(&rset);
				MPP_FD_SET(conn->sockfd, &rset);

				skipSelect = true;
			}
		}
		if (skipSelect)
			break;

		n = select(pEntry->highReadSock + 1, (fd_set *)&rset, NULL, NULL, &timeout);
		pMNEntry->sel_rd_wait += (tval.tv_sec - timeout.tv_sec) * 1000000 + (tval.tv_usec - timeout.tv_usec);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error receiving an incoming packet."),
			                errdetail("%m%s", "select")));
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
            MPP_FD_ISSET(conn->sockfd, &rset))
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG5, "RecvTupleChunkFromAny() (fd %d) %d/%d", conn->sockfd, motNodeID, index);
#endif
			tcItem = RecvTupleChunk(conn, transportStates->teardownActive);

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
RecvTupleChunkFrom(ChunkTransportState *transportStates,
				   int16 motNodeID,
				   int16 srcRoute)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn *conn;

	Insist(transportStates && transportStates->activated);

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
		return RecvTupleChunkFromUDP(transportStates, motNodeID, srcRoute);

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "RecvTupleChunkFrom(motNodID=%d, srcpIncIdx %d srcRoute=%d)", motNodeID, srcpInc, srcRoute);
#endif

	getChunkTransportState(transportStates, motNodeID, &pEntry);
	conn = pEntry->conns + srcRoute;

	return RecvTupleChunk(conn, transportStates->teardownActive);
}

/* DeregisterReadInterest is called on receiving nodes when they
 * believe that they're done with the receiver */
void
DeregisterReadInterest(ChunkTransportState *transportStates,
                       int		motNodeID,
                       int		srcRoute,
                       const char		*reason)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn *conn;

	Insist(transportStates && transportStates->activated);

	getChunkTransportState(transportStates, motNodeID, &pEntry);
	conn = pEntry->conns + srcRoute;

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
    {
        elog(DEBUG3, "Interconnect finished receiving "
             "from seg%d slice%d %s pid=%d sockfd=%d; %s",
             conn->remoteContentId,
             pEntry->sendSlice->sliceIndex,
             conn->remoteHostAndPort,
             conn->cdbProc->pid,
             conn->sockfd,
             reason);
    }

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
	{
#ifdef AMS_VERBOSE_LOGGING
		elog(LOG, "deregisterReadInterest set stillactive = false for node %d route %d (%s)", motNodeID, srcRoute, reason);
#endif
		pthread_mutex_lock(&rx_thread_mutex);
		conn->stillActive = false;
		pthread_mutex_unlock(&rx_thread_mutex);
	}
	else
	{

		/*
		 * we also mark the connection as "done." The way synchronization works is
		 * strange. On QDs the "teardown" doesn't get called until all segments
		 * are finished, which means that we need some way for the QEs to know
		 * that Teardown should complete, otherwise we deadlock the entire query
		 * (QEs wait in their Teardown calls, while the QD waits for them to
		 * finish)
		 */
		shutdown(conn->sockfd, SHUT_WR);

		MPP_FD_CLR(conn->sockfd, &pEntry->readSet);
	}
	return;
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
SetupSequenceServer(struct SliceTable *mySliceTable)
{
	if (mySliceTable != NULL)
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
	    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		    elog(DEBUG3, "tearing down seqserver connection");
		shutdown(seqserverFd, SHUT_RDWR);
		closesocket(seqserverFd);
		seqserverFd = -1;
	}
}

void
SetupUDPInterconnect(ChunkTransportState **ppTransportStates,
					 struct SliceTable	*mySliceTable,
					 int mySliceId,
					 MotionLayerState *mlStates)
{
	int			i, n;
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
	int             elapsed_ms = 0;
	int             elapsed_timeout = 0;

	ChunkTransportStateEntry *sendingChunkTransportState = NULL;

	Insist(!*ppTransportStates && mySliceTable);

	*ppTransportStates = palloc0(sizeof(ChunkTransportState));
	
	/* initialize state variables */
	Assert((*ppTransportStates)->size == 0);
	(*ppTransportStates)->size = CTS_INITIAL_SIZE;
	(*ppTransportStates)->states = palloc0(CTS_INITIAL_SIZE * sizeof(ChunkTransportStateEntry));

	(*ppTransportStates)->teardownActive = false;
	(*ppTransportStates)->activated = false;
	(*ppTransportStates)->incompleteConns = NIL;
	(*ppTransportStates)->sliceTable = NULL;
	(*ppTransportStates)->sliceId = -1;

	(*ppTransportStates)->sliceTable = mySliceTable;
	(*ppTransportStates)->sliceId = mySliceId;

	mySlice = (Slice *) list_nth((*ppTransportStates)->sliceTable->slices, mySliceId);

    Assert(mySlice &&
           IsA(mySlice, Slice) &&
           mySlice->sliceIndex == mySliceId);

	pthread_mutex_lock(&rx_thread_mutex);

	/* rearrange the connection hash table, if required */
	if (conn_htab_size != DEFAULT_CONN_HTAB_SIZE)
	{
		conn_rehash();
	}

	/* now we'll do some setup for each of our Receiving Motion Nodes. */
	foreach(cell, mySlice->children)
	{
		int			numProcs;
		int			childId = lfirst_int(cell);
		ChunkTransportStateEntry *pEntry=NULL;

#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "Setting up RECEIVING motion node %d", childId);
#endif

		aSlice = (Slice *) list_nth((*ppTransportStates)->sliceTable->slices, childId);
		numProcs = list_length(aSlice->primaryProcesses);

#ifdef AMS_VERBOSE_LOGGING
		elog(LOG, "Setup recving connections: my slice %d, childId %d",
			 mySlice->sliceIndex, childId);
#endif

		pEntry = createChunkTransportState(*ppTransportStates, aSlice, mySlice, numProcs, 0);

		Assert(pEntry);
		Assert(pEntry->valid);

		/* let cdbmotion now how many receivers to expect. */
		setExpectedReceivers(mlStates, childId, numProcs);

		for (i=0; i < pEntry->numConns; i++)
		{
			MotionConn *conn;

			conn = &pEntry->conns[i];
			conn->cdbProc = list_nth(aSlice->primaryProcesses, i);

			/* rx_buffer_queue */
			conn->pkt_q_size = 0;
			conn->pkt_q_head = 0;
			conn->pkt_q_tail = 0;
			conn->pkt_q = (uint8 **)malloc(Gp_interconnect_queue_depth * sizeof(char *));

			/* connection header info (defining characteristics of this connection) */
			MemSet(&conn->conn_info, 0, sizeof(conn->conn_info));
			conn->route = i;
			conn->conn_info.motNodeId = pEntry->motNodeId;
			conn->conn_info.recvSliceIndex = mySlice->sliceIndex;
			conn->conn_info.sendSliceIndex = aSlice->sliceIndex;

			conn->conn_info.srcContentId = conn->cdbProc->contentid;
			conn->conn_info.dstContentId = Gp_segment;

			conn->conn_info.srcListenerPort = conn->cdbProc->listenerPort;
			conn->conn_info.srcPid = conn->cdbProc->pid;
			conn->conn_info.dstPid = MyProcPid;
			conn->conn_info.dstListenerPort = (Gp_listener_port>>16) & 0x0ffff;
			conn->conn_info.sessionId = gp_session_id;
			conn->conn_info.commandCount = gp_command_count;

			(*ppTransportStates)->incompleteConns = lappend((*ppTransportStates)->incompleteConns, conn);

			conn_addhash(conn);
		}

		expectedTotalIncoming += pEntry->numConns;

		/* update the length of our rx freelist */
		rx_buf_maxcount += pEntry->numConns;
	}

    /* Initiate outgoing connections. */
    if (mySlice->parentIndex != -1)
		sendingChunkTransportState = startOutgoingUDPConnections(*ppTransportStates, mySlice, &expectedTotalOutgoing);

	pthread_mutex_unlock(&rx_thread_mutex);

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        ereport(DEBUG1, (errmsg("SetupUDPInterconnect will activate "
                                "%d incoming, %d outgoing routes.  "
                                "Listening on ports=%d/%d sockfd=%d.",
                                expectedTotalIncoming, expectedTotalOutgoing,
                                Gp_listener_port&0x0ffff, (Gp_listener_port>>16)&0x0ffff, listenerFd)));

  	gettimeofday(&startTime, NULL);

	if (interconnect_setup_timeout > 0)
		elapsed_timeout = interconnect_setup_timeout * 1000;

	/*
	 * Loop until all connections are completed or time limit is exceeded.
	 *
	 * The UDP-IC works slightly differently, above we set up the
	 * descriptors for the inbound connections, so we don't have to do
	 * any *work* how for the inbounds -- we just have to count them
	 * (so the loop below is broken in two, one for outbound and one
	 * for inbound).
	 */
	while (outgoing_count < expectedTotalOutgoing)
	{
		int		timeout;
        int		timeout_ms = 20*60*1000;
		struct pollfd nfd;

		/* check for cancellation */
		ML_CHECK_FOR_INTERRUPTS(false);

        iteration++;

		/* send message out, if we haven't already */
		/* loop over outbound connections sending registration messages as needed */
		n = sendingChunkTransportState ? sendingChunkTransportState->numConns : 0;

		for (i = 0; i < n; i++)
		{                       /* loop to set up outgoing connections */
			conn = &sendingChunkTransportState->conns[i];

			if (conn->state == mcsStarted)
				continue;

			/* Time to (re)connect? */
			if (conn->wakeup_ms <= elapsed_ms + 1)
			{
				setupOutgoingUDPConnection(*ppTransportStates, sendingChunkTransportState, conn);
				switch (conn->state)
				{
					case mcsSetupOutgoingConnection:
					case mcsConnecting:
						/* Set time limit for connect() to complete. */
						conn->wakeup_ms = UDP_CONNECT_RETRY_MS + elapsed_ms;
						break;
					default:
						conn->wakeup_ms = 0;
						break;
				}
			}

			if (conn->wakeup_ms > 0)
				timeout_ms = Min(timeout_ms, conn->wakeup_ms - elapsed_ms);
		}     /* loop to set up outgoing connections */

		/* if we're not going to wait for incoming; we want to wait on acks. */
		timeout = Min(timeout_ms, ((interconnect_setup_timeout*1000) - elapsed_ms));
		timeout = Min(100, timeout);
		if (timeout < 0)
			timeout = 0;

		nfd.fd = sendingChunkTransportState->txfd;
		nfd.events = POLLIN;

		n = poll(&nfd, 1, timeout);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;

			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error: %m%s", "poll")));
		}
		if (n == 1 && (nfd.events & POLLIN))
		{
			for (;;)
			{
				/* read ack */
				struct sockaddr_in peer;
				socklen_t peerlen;

				struct icpkthdr pkt;
				int read_count;

				peerlen = sizeof(peer);
				read_count = recvfrom(sendingChunkTransportState->txfd,
									  (char *)&pkt, sizeof(pkt), 0, (struct sockaddr *)&peer, &peerlen);

				if (read_count < 0)
				{
					if (errno == EWOULDBLOCK)
						break;

					if (errno == EINTR)
						continue;

					ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
									errmsg("Interconnect error: %m%s", "recvfrom")));
				}

				if (read_count < sizeof(pkt))
					ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
									errmsg("Interconnect error: %m%s short ack receive!", "recvfrom")));

				conn = findConnByHeader(&pkt, NULL);
				if (conn != NULL)
				{
					if (conn->state == mcsSetupOutgoingConnection)
					{
						conn->state = mcsStarted;
						conn->msgPos = NULL;
						conn->msgSize = sizeof(conn->conn_info);
						conn->stillActive = true;
						conn->conn_info.seq++;

						outgoing_count++;
					}
					else
						elog(LOG, "state not waiting for setup node %d route %d state %d conn %p", pkt.motNodeId, conn->route, conn->state, conn);
				}
				else
					elog(LOG, "Got ack for unknown connection node %d command %d dstpid %d", pkt.motNodeId, pkt.commandCount, pkt.dstPid);
				/* if we have more available, don't wait at all --
				 * we'll wait once we run out of acks */
			} /* for (;;) */
		}
		/* done with outbound (for this iteration) */ 

		/* The UDP interconnect doesn't *do* anything but count
		 * completed connections for the inbound side. So we can just
		 * continue, unless we've timed out. */
		if (elapsed_timeout && outgoing_count < expectedTotalOutgoing)
		{
			if (elapsed_timeout <= elapsed_ms + 20)
			{
				/* timed out */
				break;
			}
		}
		elapsed_ms = gettime_elapsed_ms(startTime);
	}

	/* we've either timed out, or we're done. Time to count inbound. */
	do {

		/* check for cancellation */
		ML_CHECK_FOR_INTERRUPTS(false);

		/* now do inbound */
		if (incoming_count < expectedTotalIncoming &&
			list_length((*ppTransportStates)->incompleteConns) != 0)
		{
			/* We can't use foreach() because we're going to delete elements as we go */
			cell = list_head((*ppTransportStates)->incompleteConns);
			while (cell != NULL)
			{
				conn = (MotionConn *) lfirst(cell);

				cell = lnext(cell);

				if (conn->state == mcsStarted)
				{
					incoming_count++;
					(*ppTransportStates)->incompleteConns = list_delete_ptr((*ppTransportStates)->incompleteConns, conn);
				}
			}
		}

		/* Break out of select() loop if completed all connections. */
		if (outgoing_count == expectedTotalOutgoing &&
			incoming_count == expectedTotalIncoming)
			break;

		/*
		 * Been here long?  Bail if gp_interconnect_setup_timeout exceeded.
		 */
		if (interconnect_setup_timeout > 0)
		{
			int to = interconnect_setup_timeout * 1000;

			if (to <= elapsed_ms + 20)
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect timeout: Unable to "
									   "complete setup of all connections "
									   "within time limit."),
								errdetail("Completed %d of %d incoming and "
										  "%d of %d outgoing connections.  "
										  "gp_interconnect_setup_timeout = %d "
										  "seconds.",
										  incoming_count, expectedTotalIncoming,
										  outgoing_count, expectedTotalOutgoing,
										  interconnect_setup_timeout)));
		}

		pg_usleep(100);

		/* Back to top of loop and look again. */
		elapsed_ms = gettime_elapsed_ms(startTime);
	} while (1);

	(*ppTransportStates)->activated = true;

    if (gp_log_interconnect >= GPVARS_VERBOSITY_TERSE)
    {
        elapsed_ms = gettime_elapsed_ms(startTime);
        if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE ||
            elapsed_ms >= 0.1 * 1000 * interconnect_setup_timeout)
            elog(LOG, "SetupUDPInterconnect+%dms: Activated %d incoming, "
                 "%d outgoing routes.",
                 elapsed_ms, incoming_count, outgoing_count);
    }
}

/* See ml_ipc.h */
void
SetupInterconnect(ChunkTransportState **ppTransportStates,
				  struct SliceTable * mySliceTable,
				  int mySliceId,
				  MotionLayerState *mlStates)
{
	int			i, index, n;
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
	int             elapsed_ms = 0;

	/* we can have at most one of these. */
	ChunkTransportStateEntry *sendingChunkTransportState = NULL;

	Insist(!*ppTransportStates && mySliceTable);

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
	{
		SetupUDPInterconnect(ppTransportStates, mySliceTable, mySliceId, mlStates);
		return;
	}

	*ppTransportStates = palloc0(sizeof(ChunkTransportState));
	
	/* initialize state variables */
	Assert((*ppTransportStates)->size == 0);
	(*ppTransportStates)->size = CTS_INITIAL_SIZE;
	(*ppTransportStates)->states = palloc0(CTS_INITIAL_SIZE * sizeof(ChunkTransportStateEntry));

	(*ppTransportStates)->teardownActive = false;
	(*ppTransportStates)->activated = false;
	(*ppTransportStates)->incompleteConns = NIL;
	(*ppTransportStates)->sliceTable = NULL;
	(*ppTransportStates)->sliceId = -1;


	(*ppTransportStates)->sliceTable = mySliceTable;
	(*ppTransportStates)->sliceId = mySliceId;

	if (Gp_interconnect_type == INTERCONNECT_TYPE_NIL)
	{
		return;
	}

	mySlice = (Slice *) list_nth((*ppTransportStates)->sliceTable->slices, mySliceId);

    Assert(mySlice &&
           IsA(mySlice, Slice) &&
           mySlice->sliceIndex == mySliceId);

  	gettimeofday(&startTime, NULL);

    /* Initiate outgoing connections. */
    if (mySlice->parentIndex != -1)
		sendingChunkTransportState = startOutgoingConnections(*ppTransportStates, mySlice, &expectedTotalOutgoing);

	/* now we'll do some setup for each of our Receiving Motion Nodes. */
	foreach(cell, mySlice->children)
	{
		int			numProcs;
		int			childId = lfirst_int(cell);
	    ChunkTransportStateEntry *pEntry;

#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "Setting up RECEIVING motion node %d", childId);
#endif

		aSlice = (Slice *) list_nth((*ppTransportStates)->sliceTable->slices, childId);
		numProcs = list_length(aSlice->primaryProcesses);

		pEntry = createChunkTransportState(*ppTransportStates, aSlice, mySlice, numProcs, 0);

		/* let cdbmotion now how many receivers to expect. */
		setExpectedReceivers(mlStates, childId, numProcs);

		expectedTotalIncoming += pEntry->numConns;
	}

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        ereport(DEBUG1, (errmsg("SetupInterconnect will activate "
                                "%d incoming, %d outgoing routes.  "
                                "Listening on port=%d sockfd=%d.",
                                expectedTotalIncoming, expectedTotalOutgoing,
                                Gp_listener_port, listenerFd)
					));

	/*
     * Loop until all connections are completed or time limit is exceeded.
	 */
	while (outgoing_count < expectedTotalOutgoing ||
           incoming_count < expectedTotalIncoming)
	{                           /* select() loop */
		struct timeval	timeout;
        mpp_fd_set		rset, wset, eset;
	    int			    highsock = -1;
        int             timeout_ms = 20*60*1000;
        int             outgoing_fail_count = 0;

        iteration++;

		MPP_FD_ZERO(&rset);
		MPP_FD_ZERO(&wset);
		MPP_FD_ZERO(&eset);

        /* Expecting any new inbound connections? */
        if (incoming_count < expectedTotalIncoming)
        {
            Insist(listenerFd >= 0);
            MPP_FD_SET(listenerFd, &rset);
            highsock = listenerFd;
        }

        /* Inbound connections awaiting registration message */
        foreach(cell, (*ppTransportStates)->incompleteConns)
        {
			conn = (MotionConn *)lfirst(cell);
            Insist(conn->state == mcsRecvRegMsg && conn->sockfd >= 0);
            MPP_FD_SET(conn->sockfd, &rset);
            highsock = Max(highsock, conn->sockfd);
        }

        /* Outgoing connections */
        outgoing_count = 0;
        n = sendingChunkTransportState ? sendingChunkTransportState->numConns : 0;

        for (i = 0; i < n; i++)
		{
			index = i;
			
			conn = &sendingChunkTransportState->conns[index];

            /* Time to cancel incomplete connect() and retry? */
            if (conn->state == mcsConnecting &&
                conn->wakeup_ms > 0 &&
                conn->wakeup_ms <= elapsed_ms + 20)
            {
                ereport(LOG, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                              errmsg("Interconnect timeout: Connection "
                                     "to seg%d %s from local port %s was not "
                                     "complete after %d ms %d elapsed.  Will retry.",
                                     conn->remoteContentId,
                                     conn->remoteHostAndPort,
                                     conn->localHostAndPort,
                                     conn->wakeup_ms, (elapsed_ms + 20))
							));
                conn->state = mcsSetupOutgoingConnection;
            }

            /* Time to connect? */
            if (conn->state == mcsSetupOutgoingConnection &&
                conn->wakeup_ms <= elapsed_ms + 20)
            {
                setupOutgoingConnection(*ppTransportStates, sendingChunkTransportState, conn);
                switch (conn->state)
                {
                    case mcsSetupOutgoingConnection:
                        /* Retry failed connection after awhile. */
                        conn->wakeup_ms = (iteration - 1) * 1000 + elapsed_ms;
                        break;
                    case mcsConnecting:
                        /* Set time limit for connect() to complete. */
						if ((*ppTransportStates)->aggressiveRetry)
							conn->wakeup_ms = CONNECT_AGGRESSIVERETRY_MS + elapsed_ms;
						else
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
                    MPP_FD_SET(conn->sockfd, &wset);
                    MPP_FD_SET(conn->sockfd, &eset);
                    highsock = Max(highsock, conn->sockfd);
                    break;
                case mcsSendRegMsg:
                    Insist(conn->sockfd >= 0);
                    MPP_FD_SET(conn->sockfd, &wset);
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
         * Been here long?  Bail if gp_interconnect_setup_timeout exceeded.
         */
        if (interconnect_setup_timeout > 0)
        {
            int to = interconnect_setup_timeout * 1000;

            if (to <= elapsed_ms + 20)
                ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                                errmsg("Interconnect timeout: Unable to "
                                       "complete setup of all connections "
                                       "within time limit."),
                                errdetail("Completed %d of %d incoming and "
                                          "%d of %d outgoing connections.  "
                                          "gp_interconnect_setup_timeout = %d "
                                          "seconds.",
                                          incoming_count, expectedTotalIncoming,
                                          outgoing_count, expectedTotalOutgoing,
                                          interconnect_setup_timeout)
							));
			/* don't wait for more than 500ms */
            timeout_ms = Min(500, Min(timeout_ms, to - elapsed_ms));
        }

        /*
         * If no socket events to wait for, loop to retry after a pause.
         */
        if (highsock < 0)
        {
            if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE &&
                (timeout_ms > 0 || iteration > 2))
                ereport(LOG, (errmsg("SetupInterconnect+%dms:   pause %dms   "
                                     "outgoing_fail=%d iteration=%d",
                                     elapsed_ms, timeout_ms,
                                     outgoing_fail_count, iteration)
							));

            /* Shouldn't be in this loop unless we have some work to do. */
            Insist(outgoing_fail_count > 0);

            /* Wait until earliest wakeup time or overall timeout. */
            if (timeout_ms > 0)
            {
                ML_CHECK_FOR_INTERRUPTS((*ppTransportStates)->teardownActive);
                pg_usleep(timeout_ms * 1000);
                ML_CHECK_FOR_INTERRUPTS((*ppTransportStates)->teardownActive);
            }

            /* Back to top of loop and look again. */
            elapsed_ms = gettime_elapsed_ms(startTime);
            continue;
        }

        /*
		 * Wait for socket events.
		 *
		 * In order to handle errors at intervals less than the full
		 * timeout length, we limit our select(2) wait to a maximum of
		 * 500ms.
		 */
        if (timeout_ms > 0)
        {
            timeout.tv_sec = timeout_ms / 1000; /* 0 */
            timeout.tv_usec = (timeout_ms - (timeout.tv_sec * 1000)) * 1000;
            Assert(timeout_ms == timeout.tv_sec*1000 + timeout.tv_usec/1000);
        }
        else
            timeout.tv_sec = timeout.tv_usec = 0;

        if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        {
            initStringInfo(&logbuf);
            format_fd_set(&logbuf, highsock+1, rset, "r={", "} ");
            format_fd_set(&logbuf, highsock+1, wset, "w={", "} ");
            format_fd_set(&logbuf, highsock+1, eset, "e={", "}");
            elapsed_ms = gettime_elapsed_ms(startTime);
            ereport(DEBUG1, (errmsg("SetupInterconnect+%dms:   select()  "
                                    "Interest: %s.  timeout=%dms "
                                    "outgoing_fail=%d iteration=%d",
                                    elapsed_ms, logbuf.data, timeout_ms,
                                    outgoing_fail_count, iteration)
						));
            pfree(logbuf.data);
            MemSet(&logbuf, 0, sizeof(logbuf));
        }

		ML_CHECK_FOR_INTERRUPTS((*ppTransportStates)->teardownActive);
        n = select(highsock + 1, (fd_set *)&rset, (fd_set *)&wset, (fd_set *)&eset, &timeout);
		ML_CHECK_FOR_INTERRUPTS((*ppTransportStates)->teardownActive);

        elapsed_ms = gettime_elapsed_ms(startTime);

        /*
         * Log the select() if requested.
         */
        if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE)
        {
            if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG ||
                n != expectedTotalIncoming + expectedTotalOutgoing)
            {
                int elevel = (n == expectedTotalIncoming + expectedTotalOutgoing)
					? DEBUG1 : LOG;
                int errnoSave = errno;

                initStringInfo(&logbuf);
                if (n > 0)
                {
                    appendStringInfo(&logbuf, "result=%d  Ready: ", n);
                    format_fd_set(&logbuf, highsock+1, rset, "r={", "} ");
                    format_fd_set(&logbuf, highsock+1, wset, "w={", "} ");
                    format_fd_set(&logbuf, highsock+1, eset, "e={", "}");
                }
                else
                    appendStringInfoString(&logbuf, n < 0 ? "error" : "timeout");
                ereport(elevel, (errmsg("SetupInterconnect+%dms:   select()  %s",
                                        elapsed_ms, logbuf.data) ));
                pfree(logbuf.data);
                MemSet(&logbuf, 0, sizeof(logbuf));
                errno = errnoSave;
            }
        }

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
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
		cell = list_head((*ppTransportStates)->incompleteConns);
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

			if (MPP_FD_ISSET(conn->sockfd, &rset))
			{
                n--;
                if (readRegisterMessage(*ppTransportStates, conn))
                {
                    (*ppTransportStates)->incompleteConns = list_delete_ptr((*ppTransportStates)->incompleteConns, conn);

					/* is the connection ready ? */
					if (conn->sockfd != -1)
						incoming_count++;

                    /* Free temporary MotionConn storage. */
                    pfree(conn);
                }
			}
		}

        /*
         * Someone tickling our listener port?  Accept pending connections.
         */
        if (MPP_FD_ISSET(listenerFd, &rset))
		{
			n--;
            while ((conn = acceptIncomingConnection()) != NULL)
            {
				/* get the connection read for a subsequent call to ReadRegisterMessage() */
				conn->state = mcsRecvRegMsg;
				conn->msgSize = sizeof(RegisterMessage);
				conn->msgPos = conn->pBuff;

				(*ppTransportStates)->incompleteConns = lappend((*ppTransportStates)->incompleteConns, conn);
            }
		}

		/*
         * Check our outgoing connections.
         */
        i = 0;
        while (n > 0 &&
               outgoing_count < expectedTotalOutgoing &&
		       i < sendingChunkTransportState->numConns)
		{                       /* loop to check outgoing connections */
			conn = &sendingChunkTransportState->conns[i++];
            switch (conn->state)
            {
                case mcsConnecting:
                    /* Has connect() succeeded or failed? */
                    if (MPP_FD_ISSET(conn->sockfd, &wset) ||
                        MPP_FD_ISSET(conn->sockfd, &eset))
                    {
                        n--;
                        updateOutgoingConnection(*ppTransportStates, sendingChunkTransportState, conn, -1);
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
                    if (MPP_FD_ISSET(conn->sockfd, &wset))
                    {
                        n--;
                        sendRegisterMessage(*ppTransportStates, sendingChunkTransportState, conn);
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
	if (list_length((*ppTransportStates)->incompleteConns) != 0)
	{
        if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
            elog(DEBUG2, "Incomplete connections after known connections done, cleaning %d",
			     list_length((*ppTransportStates)->incompleteConns));

		while ((cell = list_head((*ppTransportStates)->incompleteConns)) != NULL)
		{
			conn = (MotionConn *) lfirst(cell);

			if (conn->sockfd != -1)
			{
				flushIncomingData(conn->sockfd);
				shutdown(conn->sockfd, SHUT_WR);
				closesocket(conn->sockfd);
				conn->sockfd = -1;
			}

			(*ppTransportStates)->incompleteConns = list_delete_ptr((*ppTransportStates)->incompleteConns, conn);

			if (conn->pBuff)
                pfree(conn->pBuff);
			pfree(conn);
		}
	}

	(*ppTransportStates)->activated = true;

    if (gp_log_interconnect >= GPVARS_VERBOSITY_TERSE)
    {
        elapsed_ms = gettime_elapsed_ms(startTime);
        if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE ||
            elapsed_ms >= 0.1 * 1000 * interconnect_setup_timeout)
            elog(LOG, "SetupInterconnect+%dms: Activated %d incoming, "
                 "%d outgoing routes.",
                 elapsed_ms, incoming_count, outgoing_count);
    }
}                               /* SetupInterconnect */


/*
 * Move this out to separate stack frame, so that we don't have to mark
 * tons of stuff volatile in TeardownInterconnect().
 */
static void
forceEosToPeers(MotionLayerState       *mlStates,
			    ChunkTransportState    *transportStates,
			    int                     motNodeID)
{
	Insist(transportStates);
	
	transportStates->teardownActive = true;

	SendEosToAMS(mlStates, transportStates, motNodeID, get_eos_tuplechunklist());

	transportStates->teardownActive = false;
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
TeardownInterconnect(ChunkTransportState *transportStates,
					 MotionLayerState *mlStates,
					 bool forceEOS)
{
	ListCell   *cell;
	ChunkTransportStateEntry *pEntry = NULL;
	int			i;
	Slice	   *mySlice;
	MotionConn *conn;

	Insist(transportStates->sliceTable != NULL);

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
	{
		TeardownUDPInterconnect(transportStates, mlStates, forceEOS);
		return;
	}
	else if (Gp_interconnect_type == INTERCONNECT_TYPE_NIL)
	{
		return;
	}

	/* if we're already trying to clean up after an error -- don't
	 * allow signals to interrupt us */
	if (forceEOS)
		HOLD_INTERRUPTS();

	mySlice = (Slice *) list_nth(transportStates->sliceTable->slices, transportStates->sliceId);

    /* Log the start of TeardownInterconnect. */
    if (gp_log_interconnect >= GPVARS_VERBOSITY_TERSE)
    {
        int     elevel = 0;

        if (forceEOS || !transportStates->activated)
             elevel = LOG;
        else if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
             elevel = DEBUG4;

        if (elevel)
            ereport(LOG, (errmsg("Interconnect seg%d slice%d cleanup state: "
                                 "%s; setup was %s",
                                 Gp_segment, mySlice->sliceIndex,
                                 forceEOS ? "force" : "normal",
                                 transportStates->activated ? "completed" : "exited")));

	    /* if setup did not complete, log the slicetable */
        if (!transportStates->activated &&
            gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		    elog_node_display(DEBUG3, "local slice table", transportStates->sliceTable, true);
    }

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
	if (transportStates->incompleteConns &&
        gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
    	elog(DEBUG2, "Found incomplete conn. length %d", list_length(transportStates->incompleteConns));

	/* These are connected inbound peers that we haven't dealt with
	 * quite yet */
	while ((cell = list_head(transportStates->incompleteConns)) != NULL)
	{
		MotionConn *conn = (MotionConn *) lfirst(cell);

		/* they're incomplete, so just slam them shut. */
		if (conn->sockfd != -1)
		{
			flushIncomingData(conn->sockfd);
			shutdown(conn->sockfd, SHUT_WR);
			closesocket(conn->sockfd);
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
		transportStates->incompleteConns = list_delete(transportStates->incompleteConns, conn);
	}

	list_free(transportStates->incompleteConns);
	transportStates->incompleteConns = NIL;

	/*
	 * Now "normal" connections which made it through our
	 * peer-registration step. With these we have to worry about
	 * "in-flight" data.
	 */
	if (mySlice->parentIndex != -1)
	{
        /* cleanup a Sending motion node. */
        if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
            elog(DEBUG3, "Interconnect seg%d slice%d closing connections to slice%d",
                 Gp_segment, mySlice->sliceIndex, mySlice->parentIndex);

		getChunkTransportState(transportStates, mySlice->sliceIndex, &pEntry);

		if (forceEOS)
			forceEosToPeers(mlStates, transportStates, mySlice->sliceIndex);

		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;
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

		aSlice = (Slice *) list_nth(transportStates->sliceTable->slices, childId);

		pEntry = removeChunkTransportState(transportStates, aSlice->sliceIndex);

        if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
            elog(DEBUG3, "Interconnect closing connections from slice%d",
                 aSlice->sliceIndex);

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

				closesocket(conn->sockfd);
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
		pEntry = removeChunkTransportState(transportStates, mySlice->sliceIndex);

		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->sockfd >= 0)
			{
				if (!forceEOS)
					waitOnSocketDone(conn->sockfd);
				closesocket(conn->sockfd);
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
		flushInterconnectListenerBacklog();

	if (transportStates != NULL)
	{
		pfree(transportStates);
	}

	transportStates->activated = false;
	transportStates->sliceTable = NULL;

	if (forceEOS)
		RESUME_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "TeardownInterconnect successful");
#endif
}

static void
TeardownUDPInterconnect(ChunkTransportState *transportStates,
						MotionLayerState *mlStates,
						bool forceEOS)
{
	ListCell   *cell;
	ChunkTransportStateEntry *pEntry;
	int			i;
	Slice	   *mySlice;
	MotionConn *conn;

	Insist(transportStates->sliceTable != NULL);

	mySlice = (Slice *) list_nth(transportStates->sliceTable->slices, transportStates->sliceId);

	if (forceEOS)
		HOLD_INTERRUPTS();

    /* Log the start of TeardownInterconnect. */
    if (gp_log_interconnect >= GPVARS_VERBOSITY_TERSE)
    {
        int     elevel = 0;

        if (forceEOS || !transportStates->activated)
			elevel = LOG;
        else if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
			elevel = DEBUG4;

        if (elevel)
            ereport(LOG, (errmsg("Interconnect seg%d slice%d cleanup state: "
                                 "%s; setup was %s",
                                 Gp_segment, mySlice->sliceIndex,
                                 forceEOS ? "force" : "normal",
                                 transportStates->activated ? "completed" : "exited")));

	    /* if setup did not complete, log the slicetable */
        if (!transportStates->activated &&
            gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		    elog_node_display(DEBUG3, "local slice table", transportStates->sliceTable, true);
    }

	/* remove any "extra" incompleteConns ? (these are only used by
	 * one thread, no lock required) -- we're removing elements as we
	 * go, so don't use foreach() () */
	if (list_length(transportStates->incompleteConns) != 0)
	{
		cell = list_head(transportStates->incompleteConns);
		while (cell != NULL)
		{
			conn = (MotionConn *) lfirst(cell);

			cell = lnext(cell);

			transportStates->incompleteConns =
				list_delete_ptr(transportStates->incompleteConns, conn);
		}
	}

	/*
	 * Now "normal" connections which made it through our
	 * peer-registration step. With these we have to worry about
	 * "in-flight" data.
	 */
	if (mySlice->parentIndex != -1)
	{
		Slice	   *parentSlice;

		parentSlice = (Slice *) list_nth(transportStates->sliceTable->slices, mySlice->parentIndex);

		/* cleanup a Sending motion node. */
		if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
			elog(DEBUG3, "Interconnect seg%d slice%d closing connections to slice%d (%d peers)",
				 Gp_segment, mySlice->sliceIndex, mySlice->parentIndex,
				 list_length(parentSlice->primaryProcesses) + list_length(parentSlice->mirrorProcesses));

		/* Our dispatcher doesn't notice certain kinds of errors. If a
		 * QE-detected error occurs, the QD doesn't complete the query
		 * -- even when all of the QEs get correctly cancelled. 
		 * 
		 * The dispatcher doesn't set 'pleaseCancel' for some cases --
		 * the top "target" may not complete.  This problem also
		 * occurs with INSERT-SELECTs on clusters with mirrors.
		 */
		if (forceEOS)
		{
			if (parentSlice->parentIndex == -1)
			{
				forceEosToPeers(mlStates, transportStates, mySlice->sliceIndex);
			}
		}

		/* now it is safe to remove. */
		pEntry = removeChunkTransportState(transportStates, mySlice->sliceIndex);

		closesocket(pEntry->txfd);

		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			conn_delhash(conn);

			if (conn->pBuff)
			{
				pfree(conn->pBuff);
			}
		}
	}
	pthread_mutex_lock(&rx_thread_mutex);

	/*
	 * cleanup all of our Receiving Motion nodes, these get closed
	 * immediately (the receiver know for real if they want to shut
	 * down -- they aren't going to be processing any more data).
	 */
	foreach(cell, mySlice->children)
	{
		Slice	*aSlice;
		int		childId = lfirst_int(cell);

		aSlice = (Slice *) list_nth(transportStates->sliceTable->slices, childId);

		pEntry = removeChunkTransportState(transportStates, aSlice->sliceIndex);
		Assert(pEntry);

        if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
            elog(DEBUG3, "Interconnect closing connections from slice%d",
                 aSlice->sliceIndex);

		/*
		 * receivers know that they no longer care about data from
		 * below ... so we can safely discard data queued in both
		 * directions
		 */
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->stillActive)
				elog(LOG, "teardownUDP: shutting down with a receiver still active ? this will confuse our peer.");

			conn_delhash(conn);

			/* put_rx_buffer() dequeues messages and moves them to pBuff */
			while (conn->pBuff != NULL)
			{
				put_rx_buffer(conn); /* as a side-effect this may wake the rx_thread */
			}

			/* free up the packet queue */
			free(conn->pkt_q);
		}
		rx_buf_maxcount -= pEntry->numConns;
		pfree(pEntry->conns);
	}

	/* now that we've moved active rx-buffers to the freelist, we can prune the freelist itself */
	while (rx_buf_count > rx_buf_maxcount)
	{
		struct icpkthdr *buf=NULL;

		Assert(rx_buf_freelist != NULL);

		buf = (struct icpkthdr *)rx_buf_freelist;
		rx_buf_freelist = *(char **)(rx_buf_freelist);

		free(buf);
		rx_buf_count--;
	}

	if (rx_thread_waiting)
	{
		elog(LOG, "During teardownUDP, rx_thread is waiting");
	}

	elog(LOG, "TeardownUDPInterconnect: total pkt count " INT64_FORMAT
		 " dropped acks " INT64_FORMAT " retransmits " INT64_FORMAT
		 " conn mismatch " INT64_FORMAT " rx overrun " INT64_FORMAT,
		 ic_stats_pkt_count, ic_stats_dropped_acks, ic_stats_retrans,
		 ic_stats_mismatch, ic_stats_rx_overrun);

	ic_stats_pkt_count=0;
	ic_stats_dropped_acks=0;
	ic_stats_retrans=0;
	ic_stats_mismatch=0;
	ic_stats_rx_overrun=0;

	pthread_mutex_unlock(&rx_thread_mutex);

	transportStates->activated = false;
	transportStates->sliceTable = NULL;

	if (forceEOS)
		RESUME_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "TeardownUDPInterconnect successful");
#endif
}

/*=========================================================================
 * HELPER FUNCTIONS
 */

/*
 * setupListeningSocket
 */
static void
setupListeningSocket(int backlog, int *listenerSocketFd, uint16 *listenerPort)
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

	MemSet(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;

	/* We use INADDR_ANY if we don't have a valid address for
	 * ourselves (e.g. QD local connections tend to be AF_UNIX, or
	 * on 127.0.0.1 -- so bind everything) */
	if (Gp_role == GP_ROLE_DISPATCH ||
		MyProcPort->laddr.addr.ss_family != AF_INET)
		saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	else
		saddr.sin_addr = ((struct sockaddr_in *)&MyProcPort->laddr.addr)->sin_addr;

	saddr.sin_port = 0;	/* system picks port */

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        ereport(DEBUG4, (errmsg("binding listener %s", inet_ntoa(saddr.sin_addr))));

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
        closesocket(fd);
    errno = errnoSave;
    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					errmsg("Interconnect Error: Could not set up tcp listener socket."),
			        errdetail("%m%s", fun)));
}                               /* setupListeningSocket */

static void
setupUDPListeningSocket(int backlog, int *listenerSocketFd, uint16 *listenerPort)
{
	struct sockaddr_in  saddr;
	socklen_t           saddrlen;
    int                 errnoSave;
    int                 fd = -1;
    int                 rcvbuf;
	socklen_t           rcvbufsize=0;
    const char         *fun;

    fun = "socket";
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		goto error;

    fun = "fcntl(O_NONBLOCK)";
    if (!pg_set_noblock(fd))
        goto error;

	MemSet(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = 0;

    fun = "bind";
	if (bind(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
		goto error;

	MemSet(&saddr, 0, sizeof(saddr));
	saddrlen = sizeof(saddr);

    fun = "getsockname";
    if (getsockname(fd, (struct sockaddr *) &saddr, &saddrlen) < 0)
		goto error;

    *listenerSocketFd = fd;
    *listenerPort = ntohs(saddr.sin_port);

    fun = "setsockopt";

	/* the max-size supported by all of our platforms appears to be 128K ? */
	rcvbuf = 128 * 1024;
	rcvbufsize = sizeof(rcvbuf);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, rcvbufsize) < 0)
		goto error;

    fun = "getsockopt";
	rcvbufsize = sizeof(rcvbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, &rcvbufsize) < 0)
		goto error;

	elog(DEBUG2, "setupUDPListeningSocket: rcvbuf size %d", rcvbuf);

	return;

error:
    errnoSave = errno;
    if (fd >= 0)
        closesocket(fd);
    errno = errnoSave;
    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					errmsg("Interconnect Error: Could not set up udp listener socket."),
			        errdetail("%m%s", fun)));
	return;
}

/* Function createChunkTransportState() is used to create a ChunkTransportState struct and
 * place it in the hashtab hashtable based on the motNodeID.
 *
 * PARAMETERS
 *
 *	 motNodeID - motion node ID for this ChunkTransportState.
 *
 *	 numPrimaryConns  - number of primary connections for this motion node.
 *               All are incoming if this is a receiving motion node.
 *               All are outgoing if this is a sending motion node.
 *
 *   numMirrorConns - number of mirror connections (always 0 if receiving).
 *
 * RETURNS
 *	 An empty and initialized ChunkTransportState struct for the given motion node. If
 *	 a ChuckTransportState struct is already registered for the motNodeID an ERROR is
 *	 thrown.
 */
ChunkTransportStateEntry *
createChunkTransportState(ChunkTransportState *transportStates,
						  Slice *sendSlice,
						  Slice *recvSlice,
						  int numPrimaryConns,
						  int numMirrorConns)
{
	ChunkTransportStateEntry *pEntry;
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
    if (motNodeID > transportStates->size)
	{
		/* increase size of our table */
		ChunkTransportStateEntry *newTable;

		newTable = repalloc(transportStates->states, motNodeID * sizeof(ChunkTransportStateEntry));
		transportStates->states = newTable;
		/* zero-out the new piece at the end */
		MemSet(&transportStates->states[transportStates->size], 0, (motNodeID - transportStates->size) * sizeof(ChunkTransportStateEntry));
		transportStates->size = motNodeID;
	}
	
	pEntry = &transportStates->states[motNodeID - 1];

	if (pEntry->valid)
	{
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
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

	MPP_FD_ZERO(&pEntry->readSet);

	return pEntry;
}

static MotionConn *
getMotionConn(ChunkTransportStateEntry *pEntry, int iConn)
{
	Assert(pEntry);
	Assert(pEntry->conns);
	Assert(iConn < pEntry->numConns + pEntry->numPrimaryConns);

	return pEntry->conns + iConn;
}

/* Function removeChunkTransportState() is used to remove a ChunkTransportState struct from
 * the hashtab hashtable.
 *
 * This should only be called after createChunkTransportState().
 *
 * PARAMETERS
 *
 *	 motNodeID - motion node ID to lookup the ChunkTransportState.
 *   pIncIdx - parent slice idx in child slice.  If not multiplexed, should be 1.
 *
 * RETURNS
 *	 The ChunkTransportState that was removed from the hashtab hashtable.
 */
static ChunkTransportStateEntry *
removeChunkTransportState(ChunkTransportState *transportStates,
						  int16 motNodeID)
{
	ChunkTransportStateEntry *pEntry = NULL;

	if (motNodeID > transportStates->size || !transportStates->states[motNodeID - 1].valid)
	{
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: Unexpected Motion Node Id: %d. During remove.",
							   motNodeID)));
	}
	else
	{
		transportStates->states[motNodeID - 1].valid = false;
		pEntry = &transportStates->states[motNodeID - 1];
	}

	return pEntry;
}


static void
setupSeqServerConnection(char *seqServerHost, int seqServerPort)
{
	int  n;
	struct sockaddr_in addr;	
	
	/*
	 * open a connection to the sequence server
	 */
	seqserverFd = socket(AF_INET, SOCK_STREAM, 0);
	
	addr.sin_family = AF_INET;

	if ((n = inet_net_pton(AF_INET, seqServerHost, &addr.sin_addr, sizeof(addr.sin_addr))) < 1)
	{
		if (n == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_GP_INTERNAL_ERROR),
							errmsg("Interconnect Error: Could not parse remote seqserver"
								   "address: '%s'", seqServerHost),
							errdetail("inet_pton() unable to parse address: '%s'",
									  seqServerHost)));
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_GP_INTERNAL_ERROR),
							errmsg("Interconnect Error: System error occured during parsing"
								   " of remote seqserver address: '%s'", seqServerHost),
							errdetail("inet_pton()  (errno: %d) unable to parse address: '%s'",
									  errno, seqServerHost)));
		}
	}

	addr.sin_port = htons(seqServerPort);

	if ((n = connect(seqserverFd, (struct sockaddr *) & addr, sizeof(addr))) < 0)
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: Could not connect to seqserver."),
		                errdetail("%m%s", "connect")));
	
	/* make socket non-blocking BEFORE we connect. */
	if (!pg_set_noblock(seqserverFd))
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: Could not set seqserver socket"
						       "to non-blocking mode."),
			            errdetail("%m%s sockfd=%d", "fcntl", seqserverFd)));
}


/* Function startOutgoingConnections() is used to initially kick-off any outgoing
 * connections for mySlice.
 *
 * This should not be called for root slices (i.e. QD ones) since they don't
 * ever have outgoing connections.
 *
 * PARAMETERS
 *
 *  sendSlice - Slice that this process is member of.
 *  pIncIdx - index in the parent slice list of myslice.
 *
 * RETURNS
 *	 Initialized ChunkTransportState for the Sending Motion Node Id.
 */
ChunkTransportStateEntry *
startOutgoingConnections(ChunkTransportState *transportStates,
						 Slice	*sendSlice,
						 int	*pOutgoingCount)
{
	ChunkTransportStateEntry *pEntry;
    MotionConn *conn;
    ListCell   *cell;
	Slice	   *recvSlice;
    CdbProcess *cdbProc;

    *pOutgoingCount = 0;

	recvSlice = (Slice *) list_nth(transportStates->sliceTable->slices, sendSlice->parentIndex);

	if (gp_interconnect_aggressive_retry)
	{
		if ((list_length(recvSlice->children) * sendSlice->gangSize) > listenerBacklog)
			transportStates->aggressiveRetry = true;
	}
	else
		transportStates->aggressiveRetry = false;

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "Interconnect seg%d slice%d setting up sending motion node (aggressive retry is %s)",
             Gp_segment, sendSlice->sliceIndex,
			 (transportStates->aggressiveRetry ? "active" : "inactive"));

	pEntry = createChunkTransportState(transportStates,
									   sendSlice,
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
            conn->pBuff = palloc(Gp_max_packet_size);
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
            conn->pBuff = palloc(Gp_max_packet_size);
            conn->state = mcsSetupOutgoingConnection;
            (*pOutgoingCount)++;
		}
        conn++;
	}

    return pEntry;
}                               /* startOutgoingConnections */

/* Function startOutgoingConnections() is used to initially kick-off any outgoing
 * connections for mySlice.
 *
 * This should not be called for root slices (i.e. QD ones) since they don't
 * ever have outgoing connections.
 *
 * PARAMETERS
 *
 *	 sendSlice  - Slice that this process is a member of.
 *   pIncIdx - index in the parent slice list of mySlice.
 *
 * RETURNS
 *	 Initialized ChunkTransportState for the Sending Motion Node Id.
 */
ChunkTransportStateEntry *
startOutgoingUDPConnections(ChunkTransportState *transportStates,
							Slice		*sendSlice,
							int			*pOutgoingCount)
{
	ChunkTransportStateEntry *pEntry;
    MotionConn *conn;
    ListCell   *cell;
	Slice	   *recvSlice;
    CdbProcess *cdbProc;

	struct sockaddr_in  saddr;
	socklen_t           saddrlen;

    const char         *fun;
    int                 i, errnoSave;

    *pOutgoingCount = 0;

	recvSlice = (Slice *) list_nth(transportStates->sliceTable->slices, sendSlice->parentIndex);

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "Interconnect seg%d slice%d setting up sending motion node",
             Gp_segment, sendSlice->sliceIndex);

	pEntry = createChunkTransportState(transportStates,
									   sendSlice,
									   recvSlice,
									   list_length(recvSlice->primaryProcesses),
									   list_length(recvSlice->mirrorProcesses));
	
	Assert(pEntry && pEntry->valid);
	/*
	 * Setup a MotionConn entry for each of our outbound connections.
     * Request a connection to each receiving backend's listening port.
     * NB: Some mirrors could be down & have no CdbProcess entry.
	 */
    conn = pEntry->conns;

	i = 0;
    foreach(cell, recvSlice->primaryProcesses)
	{
        cdbProc = (CdbProcess *)lfirst(cell);
        if (cdbProc)
		{
            conn->cdbProc = cdbProc;
            conn->pBuff = palloc(Gp_max_packet_size);
            conn->state = mcsSetupOutgoingConnection;
			conn->route = i++;
            (*pOutgoingCount)++;
		}

        conn++;
	}

	i = 0;
	foreach(cell, recvSlice->mirrorProcesses)
	{
        cdbProc = (CdbProcess *)lfirst(cell);
        if (cdbProc)
		{
            conn->cdbProc = cdbProc;
            conn->pBuff = palloc(Gp_max_packet_size);
            conn->state = mcsSetupOutgoingConnection;
			conn->route = i++;
            (*pOutgoingCount)++;
		}
        conn++;
	}

	pEntry->txfd = socket(AF_INET, SOCK_DGRAM, 0);
	fun = "socket";
	if (pEntry->txfd < 0)
		goto error;

	if (!pg_set_noblock(pEntry->txfd))
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect Error: Could not set udp-tx socket"
						       "to non-blocking mode."),
			            errdetail("%m%s sockfd=%d", "fcntl", pEntry->txfd)));

	MemSet(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = 0;

	fun = "bind";
	if (bind(pEntry->txfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
		goto error;

	MemSet(&saddr, 0, sizeof(saddr));
	saddrlen = sizeof(saddr);

	fun = "getsockname";
    if (getsockname(pEntry->txfd, (struct sockaddr *) &saddr, &saddrlen) < 0)
		goto error;


	pEntry->txport = saddr.sin_port;
//	elog(LOG, "Connecting from UDP port %d", ntohs(saddr.sin_port));

#if 0
	{
		int                 rcvbuf;
		socklen_t           rcvbufsize=0;

		fun = "getsockopt";
		rcvbufsize = sizeof(rcvbuf);
		if (getsockopt(pEntry->txfd, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, &rcvbufsize) < 0)
			goto error;

		elog(LOG, "XMIT DEFAULT RX SIZE IS %d", rcvbuf);

		fun = "setsockopt";
		rcvbuf = 128 * 1024;
		rcvbufsize = sizeof(rcvbuf);
		if (setsockopt(pEntry->txfd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, rcvbufsize) < 0)
			goto error;

		elog(LOG, "XMIT NEW RX SIZE IS %d", rcvbuf);
	}
#endif

    return pEntry;

error:
    errnoSave = errno;
    if (pEntry->txfd >= 0)
        closesocket(pEntry->txfd);
    errno = errnoSave;
    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					errmsg("Interconnect Error: Could not set up listener socket."),
			        errdetail("%m%s", fun)));
	return NULL;
}

/*
 * setupOutgoingUDPConnection
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
setupOutgoingUDPConnection(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn *conn)
{
    CdbProcess         *cdbProc = conn->cdbProc;
	struct sockaddr_in  saddr;
	int                 n;

    Assert(conn->cdbProc);
    Assert(conn->state == mcsSetupOutgoingConnection);

    conn->wakeup_ms = 0;
    conn->remoteContentId = cdbProc->contentid;

    snprintf(conn->remoteHostAndPort, sizeof(conn->remoteHostAndPort),
             "%s:%d", cdbProc->listenerAddr, cdbProc->listenerPort);

    /*
     * Get ipaddr to connect to.
     */
    MemSet(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;

    if ((n = inet_net_pton(AF_INET, cdbProc->listenerAddr, &saddr.sin_addr, sizeof(saddr.sin_addr))) < 1)
    {
	    if (n == 0)
	    {
		    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect Error: Could not parse remote listener"
								   "address: '%s'", cdbProc->listenerAddr),
							errdetail("inet_pton() unable to parse address: '%s'",
									  cdbProc->listenerAddr)));
	    }
	    else
	    {
		    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect Error: System error occured during parsing"
								   " of remote listener address '%s'", cdbProc->listenerAddr),
						    errdetail("%m%s", "inet_pton")));
	    }
    }

    saddr.sin_port = htons(cdbProc->listenerPort);

    format_sockaddr((struct sockaddr *) &saddr, conn->remoteHostAndPort,
                    sizeof(conn->remoteHostAndPort));

	memcpy(&conn->peer, &saddr, sizeof(saddr));

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        ereport(DEBUG1, (errmsg("Interconnect connecting to seg%d slice%d %s "
                                "pid=%d sockfd=%d",
                                conn->remoteContentId,
                                pEntry->recvSlice->sliceIndex,
                                conn->remoteHostAndPort,
                                conn->cdbProc->pid,
                                conn->sockfd)));

	/* send connection request */
	MemSet(&conn->conn_info, 0, sizeof(conn->conn_info));
	conn->conn_info.len = 0;
	conn->conn_info.flags = 0;
	conn->conn_info.motNodeId = pEntry->motNodeId;

	conn->conn_info.recvSliceIndex = pEntry->recvSlice->sliceIndex;
	conn->conn_info.sendSliceIndex = pEntry->sendSlice->sliceIndex;
	conn->conn_info.srcContentId = Gp_segment;
	conn->conn_info.dstContentId = conn->cdbProc->contentid;

	elog(LOG, "setupOutgoingUDPConnection: node %d route %d srccontent %d dstcontent %d", pEntry->motNodeId, conn->route, Gp_segment, conn->cdbProc->contentid);

	conn->conn_info.srcListenerPort = (Gp_listener_port>>16) & 0x0ffff;
	conn->conn_info.srcPid = MyProcPid;
	conn->conn_info.dstPid = conn->cdbProc->pid;
	conn->conn_info.dstListenerPort = conn->cdbProc->listenerPort;

	conn->conn_info.sessionId = gp_session_id;
	conn->conn_info.commandCount = gp_command_count;

	conn_addhash(conn);

	n = sendto(pEntry->txfd, (const char *)&conn->conn_info, sizeof(conn->conn_info), 0,
			   (struct sockaddr *)&saddr, (socklen_t)sizeof(saddr));
	if (n != sizeof(conn->conn_info))
	{
	    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
			            errdetail("%m%s", "sendto")));
	}

	return;
}                               /* setupOutgoingUDPConnection */

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
setupOutgoingConnection(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn *conn)
{
    CdbProcess         *cdbProc = conn->cdbProc;
	struct sockaddr_in  saddr;
    int                 sockopt;
	int                 n;

    Assert(conn->cdbProc);
    Assert(conn->state == mcsSetupOutgoingConnection);

    conn->wakeup_ms = 0;
    conn->remoteContentId = cdbProc->contentid;

    snprintf(conn->remoteHostAndPort, sizeof(conn->remoteHostAndPort),
             "%s:%d", cdbProc->listenerAddr, cdbProc->listenerPort);

    /* Might be retrying due to connection failure etc.  Close old socket. */
    if (conn->sockfd >= 0)
    {
        closesocket(conn->sockfd);
        conn->sockfd = -1;
    }

    /*
     * Create a socket.
     */
    conn->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->sockfd < 0)
	    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
			            errdetail("%m%s", "socket")));

    /* make socket non-blocking BEFORE we connect. */
    if (!pg_set_noblock(conn->sockfd))
    {
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
		                errdetail("%m%s", "fcntl(O_NONBLOCK)")));
    }

    /* Allow bind() to succeed even if the port is in TIME_WAIT state. */
    sockopt = 1;
    if (setsockopt(conn->sockfd, SOL_SOCKET, SO_REUSEADDR,
                   (void *)&sockopt, sizeof(sockopt)) < 0)
	    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
		                errdetail("%m%s", "setsockopt(SO_REUSEADDR)")));

    MemSet(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;

	/* We use INADDR_ANY if we don't have a valid address for
	 * ourselves (e.g. QD local connections tend to be AF_UNIX */
	if (MyProcPort->laddr.addr.ss_family != AF_INET)
		saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	else
		saddr.sin_addr = ((struct sockaddr_in *)&MyProcPort->laddr.addr)->sin_addr;

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        ereport(DEBUG4, (errmsg("bind outbound  %s", inet_ntoa(saddr.sin_addr))));

    saddr.sin_port = 0; /* pick any available outbound port */

    if (bind(conn->sockfd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
        /* Should never get EADDRINUSE because we know it's our port. */
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
                        errdetail("Could not bind to local addr %s. %m",
                                  inet_ntoa(saddr.sin_addr))));
    }

    /*
     * Get ipaddr to connect to.
     */
    MemSet(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;

    if ((n = inet_net_pton(AF_INET, cdbProc->listenerAddr, &saddr.sin_addr, sizeof(saddr.sin_addr))) < 1)
    {
	    if (n == 0)
	    {
		    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect Error: Could not parse remote listener"
								   "address: '%s'", cdbProc->listenerAddr),
							errdetail("inet_pton() unable to parse address: '%s'",
									  cdbProc->listenerAddr)));
	    }
	    else
	    {
		    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect Error: System error occured during parsing"
								   " of remote listener address '%s'", cdbProc->listenerAddr),
						    errdetail("%m%s", "inet_pton")));
	    }
    }

    saddr.sin_port = htons(cdbProc->listenerPort);

    format_sockaddr((struct sockaddr *) &saddr, conn->remoteHostAndPort,
                    sizeof(conn->remoteHostAndPort));

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        ereport(DEBUG1, (errmsg("Interconnect connecting to seg%d slice%d %s "
                                "pid=%d sockfd=%d",
                                conn->remoteContentId,
                                pEntry->recvSlice->sliceIndex,
                                conn->remoteHostAndPort,
                                conn->cdbProc->pid,
                                conn->sockfd)));

    /*
     * Initiate the connection.
     */
    for (;;)
    {                           /* connect() EINTR retry loop */
        ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

        n = connect(conn->sockfd, (struct sockaddr *) &saddr, sizeof(saddr));
	
        /* Non-blocking socket never connects immediately, but check anyway. */
        if (n == 0)
        {
            sendRegisterMessage(transportStates, pEntry, conn);
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
        updateOutgoingConnection(transportStates, pEntry, conn, errno);
        return;
    }                           /* connect() EINTR retry loop */
}                               /* setupOutgoingConnection */


/*
 * updateOutgoingConnection
 *
 * Called when connect() succeeds or fails.
 */
void
updateOutgoingConnection(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn *conn, int errnoSave)
{
    socklen_t   sizeoferrno = sizeof(errnoSave);

    /* Get errno value indicating success or failure. */
    if (errnoSave == -1 &&
        getsockopt(conn->sockfd, SOL_SOCKET, SO_ERROR,
                   (void *)&errnoSave, &sizeoferrno))
    {
        /* getsockopt failed */
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect could not connect to seg%d %s",
                               conn->remoteContentId,
                               conn->remoteHostAndPort),
                        errdetail("%m%s sockfd=%d",
                                  "getsockopt(SO_ERROR)",
                                  conn->sockfd)));
    }

    switch (errnoSave)
    {
        /* Success!  Advance to next state. */
        case 0:
            sendRegisterMessage(transportStates, pEntry, conn);
            return;
        default:
            errno = errnoSave;
            ereport(LOG, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
		                  errmsg("Interconnect could not connect to seg%d %s "
                                 "pid=%d; "
                                 "will retry. %m%s",
                                 conn->remoteContentId,
                                 conn->remoteHostAndPort,
                                 conn->cdbProc->pid,
                                 "connect")));
            break;
    }

    /* Tell caller to close the socket and try again. */
    conn->state = mcsSetupOutgoingConnection;
}                               /* updateOutgoingConnection */

/* Function sendRegisterMessage() used to send a Register message to the
 * remote destination on the other end of the provided conn.
 *
 * PARAMETERS
 *
 *	 pEntry - ChunkTransportState.
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
sendRegisterMessage(ChunkTransportState *transportStates, ChunkTransportStateEntry * pEntry, MotionConn * conn)
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
               sizeof(*regMsg) <= Gp_max_packet_size);
        Assert(pEntry->recvSlice &&
               pEntry->sendSlice &&
               IsA(pEntry->recvSlice, Slice) &&
               IsA(pEntry->sendSlice, Slice));

        /* Save local host and port for log messages. */
        addrsize = sizeof(localAddr);
	    if (getsockname(conn->sockfd, (struct sockaddr *)&localAddr, &addrsize))
	    {
            ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
		                    errmsg("Interconnect error after making connection."),
			                errdetail("%m%s sockfd=%d remote=%s",
					                  "getsockname",
                                      conn->sockfd,
                                      conn->remoteHostAndPort)));
	    }
        format_sockaddr((struct sockaddr *)&localAddr, conn->localHostAndPort,
                        sizeof(conn->localHostAndPort));

    	if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE)
            ereport(LOG, (errmsg("Interconnect sending registration message "
                                 "to seg%d slice%d %s pid=%d "
                                 "from seg%d slice%d %s sockfd=%d",
                                 conn->remoteContentId,
                                 pEntry->recvSlice->sliceIndex,
                                 conn->remoteHostAndPort,
                                 conn->cdbProc->pid,
                                 Gp_segment,
                                 pEntry->sendSlice->sliceIndex,
                                 conn->localHostAndPort,
                                 conn->sockfd)));

        regMsg->msgBytes = sizeof(*regMsg);
        regMsg->recvSliceIndex = pEntry->recvSlice->sliceIndex;
        regMsg->sendSliceIndex = pEntry->sendSlice->sliceIndex;

        regMsg->srcContentId = Gp_segment;
        regMsg->srcListenerPort = Gp_listener_port&0x0ffff;
        regMsg->srcPid = MyProcPid;
        regMsg->srcSessionId = gp_session_id;
        regMsg->srcCommandCount = gp_command_count;


        conn->state = mcsSendRegMsg;
        conn->msgPos = conn->pBuff;
        conn->msgSize = sizeof(*regMsg);
    }

    /* Send as much as we can. */
    for (;;)
    {
	    bytesToSend = conn->pBuff + conn->msgSize - conn->msgPos;
        bytesSent = send(conn->sockfd, conn->msgPos, bytesToSend, 0);
        if (bytesSent == bytesToSend)
            break;
        else if (bytesSent >= 0)
            conn->msgPos += bytesSent;
        else if (errno == EWOULDBLOCK)
            return;                     /* call me again to send the rest */
        else if (errno == EINTR)
            ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
        else
        {
            ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                            errmsg("Interconnect error writing registration "
                                   "message to seg%d at %s",
                                   conn->remoteContentId,
                                   conn->remoteHostAndPort),
                            errdetail("%m%s pid=%d sockfd=%d local=%s",
                                      "write",
                                      conn->cdbProc->pid,
                                      conn->sockfd,
                                      conn->localHostAndPort)));
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
readRegisterMessage(ChunkTransportState *transportStates,
					MotionConn *conn)
{
   	int     bytesToReceive;
    int     bytesReceived;
    int     iconn;
    RegisterMessage    *regMsg;
	RegisterMessage     msg;
	MotionConn         *newConn;
    ChunkTransportStateEntry   *pEntry = NULL;
	CdbProcess         *cdbproc;

    /* Get ready to receive the Register message. */
    if (conn->state != mcsRecvRegMsg)
    {
        conn->state = mcsRecvRegMsg;
        conn->msgSize = sizeof(*regMsg);
        conn->msgPos = conn->pBuff;

        Assert(conn->pBuff &&
               sizeof(*regMsg) <= Gp_max_packet_size);
    }

    /* Receive all that is available, up to the expected message size. */
    for (;;)
    {
	    bytesToReceive = conn->pBuff + conn->msgSize - conn->msgPos;
        bytesReceived = recv(conn->sockfd, conn->msgPos, bytesToReceive, 0);
        if (bytesReceived == bytesToReceive)
            break;
        else if (bytesReceived > 0)
            conn->msgPos += bytesReceived;
 		else if (bytesReceived == 0)
		{
            ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                            errmsg("Interconnect error reading register message "
                                   "from %s: connection closed",
                                   conn->remoteHostAndPort),
                            errdetail("recv() sockfd=%d local=%s",
                                      conn->sockfd, conn->localHostAndPort)));
		}
        else if (errno == EWOULDBLOCK)
            return false;       /* call me again to receive the rest */
        else if (errno == EINTR)
            ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
        else
        {
            ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                            errmsg("Interconnect error reading register message "
                                   "from %s", conn->remoteHostAndPort),
                            errdetail("%m%s sockfd=%d local=%s",
                                      "read",
                                      conn->sockfd,
                                      conn->localHostAndPort)));
        }
    }

    /*
     * Got the whole message.  Convert fields to native byte order.
     */
    regMsg = (RegisterMessage *)conn->pBuff;
    msg.msgBytes = regMsg->msgBytes;
    msg.recvSliceIndex = regMsg->recvSliceIndex;
    msg.sendSliceIndex = regMsg->sendSliceIndex;

    msg.srcContentId = regMsg->srcContentId;
    msg.srcListenerPort = regMsg->srcListenerPort;
    msg.srcPid = regMsg->srcPid;
    msg.srcSessionId = regMsg->srcSessionId;
    msg.srcCommandCount = regMsg->srcCommandCount;

    /* Check for valid message format. */
    if (msg.msgBytes != sizeof(*regMsg))
    {
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error reading register message "
                               "from %s: format not recognized",
                               conn->remoteHostAndPort),
                        errdetail("msgBytes=%d expected=%d sockfd=%d local=%s",
                                  msg.msgBytes, (int)sizeof(*regMsg),
                                  conn->sockfd, conn->localHostAndPort)));
    }

	/* get rid of old connections first */
	if (msg.srcSessionId != gp_session_id ||
		msg.srcCommandCount < gp_command_count)
	{
		/* This is an old connection, which can be safely
		 * ignored. We get this kind of stuff for cases in which
		 * one gang participating in the interconnect exited a
		 * query before calling SetupInterconnect(). Later queries
		 * wind up receiving their registration messages. */
		elog(LOG, "Received invalid, old registration message: "
			 "will ignore ('expected:received' session %d:%d command-id %d:%d)",
			 gp_session_id, msg.srcSessionId,
			 gp_command_count, msg.srcCommandCount);

		shutdown(conn->sockfd, SHUT_RDWR);
		closesocket(conn->sockfd);
		conn->sockfd = -1;

		pfree(conn->pBuff);

		return true;
	}

    /* Verify that the message pertains to one of our receiving Motion nodes. */
    if (msg.sendSliceIndex > 0 &&
        msg.sendSliceIndex <= transportStates->size &&
        msg.recvSliceIndex == transportStates->sliceId &&
        msg.srcContentId >= -1)
    {
		/* this is a good connection */
    }
    else
	{
		/* something is wrong */
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error: Invalid registration "
                               "message received from %s.",
                               conn->remoteHostAndPort),
                        errdetail("sendSlice=%d recvSlice=%d srcContentId=%d "
                                  "srcPid=%d srcListenerPort=%d "
                                  "srcSessionId=%d srcCommandCount=%d motnode=%d",
                                  msg.sendSliceIndex, msg.recvSliceIndex,
                                  msg.srcContentId, msg.srcPid,
                                  msg.srcListenerPort, msg.srcSessionId,
                                  msg.srcCommandCount, msg.sendSliceIndex)));
	}

    /*
     * Find state info for the specified Motion node.  The sender's slice
     * number equals the motion node id.
     */
	getChunkTransportState(transportStates, msg.sendSliceIndex, &pEntry);
	Assert(pEntry);

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
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error: Invalid registration "
                               "message received from %s.",
                               conn->remoteHostAndPort),
                        errdetail("sendSlice=%d srcContentId=%d "
                                  "srcPid=%d srcListenerPort=%d",
                                  msg.sendSliceIndex, msg.srcContentId,
                                  msg.srcPid, msg.srcListenerPort)));
    }

    /*
	 * Allocate MotionConn slot corresponding to sender's position in the
     * sending slice's CdbProc list.
	 */
	newConn = getMotionConn(pEntry, iconn);
	
    if (newConn->sockfd != -1 ||
        newConn->state != mcsNull)
	{
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error: Duplicate registration "
                               "message received from %s.",
                               conn->remoteHostAndPort),
                        errdetail("Already accepted registration from %s for "
                                  "sendSlice=%d srcContentId=%d "
                                  "srcPid=%d srcListenerPort=%d",
                                  newConn->remoteHostAndPort,
                                  msg.sendSliceIndex, msg.srcContentId,
                                  msg.srcPid, msg.srcListenerPort)));
	}

	/* message looks good */
	if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE)
	{
		ereport(LOG, (errmsg("Interconnect seg%d slice%d sockfd=%d accepted "
                             "registration message from seg%d slice%d %s pid=%d",
                             Gp_segment,
							 msg.recvSliceIndex,
                             conn->sockfd,
							 msg.srcContentId,
                             msg.sendSliceIndex,
                             conn->remoteHostAndPort,
							 msg.srcPid)));
	}

    /* Copy caller's temporary MotionConn to its assigned slot. */
    *newConn = *conn;

    newConn->cdbProc = cdbproc;
    newConn->remoteContentId = msg.srcContentId;

    /*
     * The caller's MotionConn object is no longer valid.
     */
    MemSet(conn, 0, sizeof(*conn));
    conn->state = mcsNull;

    /*
     * Prepare to begin reading tuples.
     */
    newConn->state = mcsStarted;
    newConn->msgPos = NULL;
    newConn->msgSize = 0;
    newConn->stillActive = true;

    MPP_FD_SET(newConn->sockfd, &pEntry->readSet);

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
	    MemSet(&remoteAddr, 0, sizeof(remoteAddr));
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
#ifndef _WIN32
            case ENOTSOCK:
#endif
            case EOPNOTSUPP:
                /* Shouldn't get these errors unless there is a bug. */
                ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                                errmsg("Interconnect error on listener port %d",
                                       Gp_listener_port),
			                    errdetail("%m%s sockfd=%d",
                                          "accept",
                                          listenerFd)));
                break;          /* not reached */
            case ENOMEM:
            case ENFILE:
            case EMFILE:
            case ENOBUFS:
                /* Out of resources. */
                ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                                errmsg("Interconnect error on listener port %d",
                                       Gp_listener_port),
			                    errdetail("%m%s sockfd=%d",
                                          "accept",
                                          listenerFd)));
                break;          /* not reached */
        	default:
                /* Network problem, connection aborted, etc.  Continue. */
                ereport(LOG, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                              errmsg("Interconnect connection request not "
                                     "completed on listener port %d",
									 Gp_listener_port),
			                  errdetail("%m%s sockfd=%d",
                                        "accept",
                                        listenerFd)));
        }                       /* switch (errno) */
    }                           /* loop until success or EWOULDBLOCK */

    /*
     * Create a MotionConn object to hold the connection state.
     */
    conn = palloc0(sizeof(MotionConn));
    conn->sockfd = newsockfd;
    conn->pBuff = palloc(Gp_max_packet_size);
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
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error after accepting connection."),
	                    errdetail("%m%s sockfd=%d remote=%s",
			                      "getsockname",
                                  newsockfd,
                                  conn->remoteHostAndPort)));
    }
    format_sockaddr((struct sockaddr *)&localAddr, conn->localHostAndPort,
                    sizeof(conn->localHostAndPort));

    /* make socket non-blocking */
    if (!pg_set_noblock(newsockfd))
    {
        ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                        errmsg("Interconnect error after accepting connection."),
	                    errdetail("%m%s sockfd=%d remote=%s local=%s",
                                  "fcntl(O_NONBLOCK)",
                                  newsockfd,
                                  conn->remoteHostAndPort,
                                  conn->localHostAndPort)));
    }

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "Interconnect got incoming connection "
             "from remote=%s to local=%s sockfd=%d",
             conn->remoteHostAndPort, conn->localHostAndPort, newsockfd);

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
sendChunk(MotionLayerState *mlStates, ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn * conn, TupleChunkListItem tcItem, int16 motionId)
{
	Assert(conn->msgSize > 0);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "sendChunk: msgSize %d this chunk length %d", conn->msgSize, tcItem->chunk_length);
#endif

	if (conn->msgSize + tcItem->chunk_length > Gp_max_packet_size)
	{
		if (!flushBuffer(mlStates, transportStates, pEntry, conn, motionId))
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
flushUDPBuffer(MotionLayerState *mlStates, ChunkTransportState *transportStates, 
			   ChunkTransportStateEntry *pEntry, MotionConn * conn, int16 motionId)
{
	char	   *sendptr;
	int			n;

	struct pollfd nfd;

	struct timeval  startTime;
	int			timeout;
	int			elapsed_ms;
	int			retry=0;

	bool		result = true; /* assume that the connection is going to stay "live" */

#ifdef AMS_VERBOSE_LOGGING
	elog(LOG, "flushUDPBUffer: msgSize %d tuplecount %d", conn->msgSize, conn->tupleCount);
#endif

	/* first set up our header */
	conn->conn_info.len = conn->msgSize;
	if (pEntry->sendingEos)
		conn->conn_info.flags |= FLAGS_EOS;

	memcpy(conn->pBuff, &conn->conn_info, sizeof(conn->conn_info));

	/* now send message */
	sendptr = (char *) conn->pBuff;

  	gettimeofday(&startTime, NULL);

	/* send this packet */
	for (;;)
	{
		elapsed_ms = gettime_elapsed_ms(startTime);

		timeout = TIMEOUT(retry);

		if (timeout <= elapsed_ms)
		{
#if 0
			elog(LOG, "xmit retry @ %d", elapsed_ms);
#endif

			n = sendto(pEntry->txfd, conn->pBuff, conn->msgSize, 0,
					   (struct sockaddr *)&conn->peer, (socklen_t)sizeof(conn->peer));
			if (n < 0)
			{
				ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
				if (errno == EINTR)
					continue;
			
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error writing an outgoing packet"),
								errdetail("error during sendto() call (error:%d).\n"
										  "For Remote Connection: contentId=%d at %s",
										  errno, conn->remoteContentId,
										  conn->remoteHostAndPort)));
			}

			if (n != conn->msgSize)
			{
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error writing an outgoing packet"),
								errdetail("short transmit during sendto() call.\n"
										  "For Remote Connection: contentId=%d at %s",
										  conn->remoteContentId,
										  conn->remoteHostAndPort)));
			}
		}

		if (retry)
			ic_stats_retrans++;

		retry++;

		if (timeout < 1)
			timeout = 1;


		/* now we're waiting for our ack */
		nfd.fd = pEntry->txfd;
		nfd.events = POLLIN;

		n = poll(&nfd, 1, timeout);
		if (n < 0)
		{
			ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
			if (errno == EINTR)
				continue;

			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error waiting for peer ack"),
							errdetail("During poll() call.\n"
									  "For Remote Connection: contentId=%d at %s",
									  conn->remoteContentId,
									  conn->remoteHostAndPort)));
		}

		if (n == 0)
		{
			/* timeout, retry */
			ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
			elog(DEBUG4, "waiting for ack, need to retry at elapsed %d", elapsed_ms);
			continue;
		}

		/* if we've got something to read, handle it. */
		if (n == 1 && (nfd.events & POLLIN))
		{
			/* ready to read on our socket */
			struct sockaddr_in peer;
			socklen_t peerlen;

			struct icpkthdr pkt;

			peerlen = sizeof(peer);
			n = recvfrom(pEntry->txfd, (char *)&pkt, sizeof(pkt), 0,
						 (struct sockaddr *)&peer, &peerlen);

			if (n < 0)
			{
				ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
				if (errno == EINTR)
					continue;

				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error waiting for peer ack"),
								errdetail("During recvfrom() call.\n"
										  "For Remote Connection: contentId=%d at %s",
										  conn->remoteContentId,
										  conn->remoteHostAndPort)));
				/* not reached */
			}
			else if (n < sizeof(pkt))
			{
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error waiting for peer ack"),
								errdetail("short read during recvfrom() call. (%d)\n"
										  "For Remote Connection: contentId=%d at %s",
										  n, conn->remoteContentId,
										  conn->remoteHostAndPort)));
				/* not reached */
			}

			/* read packet, is this the ack we want ? */
			if (pkt.motNodeId == conn->conn_info.motNodeId &&
				pkt.srcContentId == conn->conn_info.srcContentId &&
				pkt.srcPid == conn->conn_info.srcPid &&
				pkt.dstPid == conn->conn_info.dstPid &&
				pkt.srcListenerPort == ((Gp_listener_port>>16) & 0x0ffff) &&
				pkt.dstListenerPort == conn->conn_info.dstListenerPort &&
				pkt.sessionId == gp_session_id &&
				pkt.commandCount == gp_command_count)
			{
				/* this was for me */
				if (pkt.flags & FLAGS_STOP)
				{
#if 0
					elog(LOG, "got ack with stop; not advancing seq, setting stillactive <- false srcpid %d dstpid %d cmd %d", pkt.srcPid, pkt.dstPid, pkt.commandCount);
#endif
					result = false;
					conn->stillActive = false;
					conn->conn_info.seq++;
					break;
				}
				if (pkt.flags & FLAGS_ACK)
				{
					if (pkt.seq != conn->conn_info.seq)
					{
#if 0
						elog(LOG, "ack with bad seq?! expected %d got %d", conn->conn_info.seq, pkt.seq);
#endif
						continue;
					}
					conn->conn_info.seq++;
#if 0
					elog(LOG, "got good ack with seq %d node %d route %d peer %d", pkt.seq, pkt.motNodeId, conn->route, pkt.dstPid);
#endif
					break;
				}
			}
			else
				elog(DEBUG4, "flushUDPbuffer: not the ack we're looking for (flags 0x%x)...mot(%d:%d) content(%d:%d) srcpid(%d:%d) dstpid(%d:%d) srcport(%d:%d) dstport(%d:%d) sess(%d:%d) cmd(%d:%d)",
					 pkt.flags, pkt.motNodeId, conn->conn_info.motNodeId,
					 pkt.srcContentId, conn->conn_info.srcContentId,
					 pkt.srcPid,conn->conn_info.srcPid,
					 pkt.dstPid, conn->conn_info.dstPid,
					 pkt.srcListenerPort, ((Gp_listener_port>>16) & 0x0ffff),
					 pkt.dstListenerPort, conn->conn_info.dstListenerPort,
					 pkt.sessionId, gp_session_id,
					 pkt.commandCount, gp_command_count);
		}
	}

	/* now, reset so that we're ready to send more. */
	conn->tupleCount = 0;
	conn->msgSize = sizeof(conn->conn_info);

	return result;
}

static bool
flushBuffer(MotionLayerState *mlStates, ChunkTransportState *transportStates,
			ChunkTransportStateEntry *pEntry, MotionConn *conn, int16 motionId)
{
	char	   *sendptr;
	MotionNodeEntry *pMNEntry;
	int			n,
		sent = 0;
	mpp_fd_set	wset;
	mpp_fd_set 	rset;

#ifdef AMS_VERBOSE_LOGGING
	{
		struct timeval snapTime;

		gettimeofday(&snapTime, NULL);
		elog(DEBUG5, "----sending chunk @%s.%d time is %d.%d",
			 __FILE__, __LINE__, (int)snapTime.tv_sec, (int)snapTime.tv_usec);
	}
#endif

	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
		return flushUDPBuffer(mlStates, transportStates, pEntry, conn, motionId);

	pMNEntry = getMotionNodeEntry(mlStates, motionId, "flushBuffer");

	/* first set header length */
	*(uint32 *) conn->pBuff = conn->msgSize;

	/* now send message */
	sendptr = (char *) conn->pBuff;
	sent = 0;
	do
	{
		struct timeval timeout;

		/* check for stop message before sending anything  */
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		MPP_FD_ZERO(&rset);
		MPP_FD_SET(conn->sockfd, &rset);

		/*
		 * since timeout = 0, select returns imediately and no time is wasted
		 * waiting trying to send data on the network
		 */
		n = select(conn->sockfd + 1, (fd_set *)&rset, NULL, NULL, &timeout);
		/* handle errors at the write call, below */
		if (n > 0 && MPP_FD_ISSET(conn->sockfd, &rset))
		{
#ifdef AMS_VERBOSE_LOGGING
			print_connection(transportStates, conn->sockfd, "stop from");
#endif
			/* got a stop message */
			conn->stillActive = false;
			return false;
		}

		if ((n = send(conn->sockfd, sendptr + sent, conn->msgSize - sent, 0)) < 0)
		{
			ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK)
			{
				do
				{
					timeout = tval;

					ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

					MPP_FD_ZERO(&rset);
					MPP_FD_ZERO(&wset);
					MPP_FD_SET(conn->sockfd, &wset);
					MPP_FD_SET(conn->sockfd, &rset);
					n = select(conn->sockfd + 1, (fd_set *)&rset, (fd_set *)&wset, NULL, &timeout);
					pMNEntry->sel_wr_wait += (tval.tv_sec - timeout.tv_sec)*1000000 +(tval.tv_usec - timeout.tv_usec);
					if (n < 0)
					{
						if (errno == EINTR)
							continue;

						/*
						 * if we got an error in teardown, ignore it: treat it
						 * as a stop message
						 */
						if (transportStates->teardownActive)
						{
#ifdef AMS_VERBOSE_LOGGING
							print_connection(transportStates, conn->sockfd, "stop from");
#endif
							conn->stillActive = false;
							return false;
						}
						ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
										errmsg("Interconnect error writing an outgoing packet"),
										errdetail("error during select() call (error:%d).\n"
												  "For Remote Connection: contentId=%d at %s",
												  errno, conn->remoteContentId,
												  conn->remoteHostAndPort)));
					}

					/*
					 * as a sender... if there is something to read...
					 * it must mean its a StopSendingMessage.  we
					 * don't even bother to read it.
					 */
					if (MPP_FD_ISSET(conn->sockfd, &rset) || transportStates->teardownActive)
					{
#ifdef AMS_VERBOSE_LOGGING
						print_connection(transportStates, conn->sockfd, "stop from");
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
				if (transportStates->teardownActive)
				{
#ifdef AMS_VERBOSE_LOGGING
					print_connection(transportStates, conn->sockfd, "stop from");
#endif
					conn->stillActive = false;
					return false;
				}
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error writing an outgoing packet"),
								errdetail("error during send() call (error:%d).\n"
										  "For Remote Connection: contentId=%d at %s",
										  errno, conn->remoteContentId,
                                          conn->remoteHostAndPort)));
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

#ifdef AMS_VERBOSE_LOGGING
void
dumpEntryConnections(int elevel, ChunkTransportStateEntry *pEntry)
{
	int			i;
    MotionConn *conn;

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
#endif

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
doSendStopMessage(ChunkTransportState *transportStates, int16 motNodeID)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn *conn;
	int			i;
	char		m = 'S';
	ssize_t		written;

	getChunkTransportState(transportStates, motNodeID, &pEntry);
	Assert(pEntry);

	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG3, "Interconnect needs no more input from slice%d; notifying senders to stop.",
			 motNodeID);

	elog(LOG, "Interconnect needs no more input from slice%d; notifying senders to stop.",
		 motNodeID);

	/*
	 * Note: we're only concerned with receivers here, so mirrors
	 * don't matter
	 */
	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
	{
		pthread_mutex_lock(&rx_thread_mutex);
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->stillActive)
			{
//			sendAck(conn, true);
				conn->conn_info.flags |= FLAGS_STOP;
				elog(LOG, "sent stop message; should shut down read interest");

				/* CRITICAL TO AVOID DEADLOCK */
				conn->stillActive = false;
			}
		}
		pthread_mutex_unlock(&rx_thread_mutex);
	}
	else /* TCP interconnect */
	{
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->sockfd >= 0 &&
				MPP_FD_ISSET(conn->sockfd, &pEntry->readSet))
			{
				/* someone is trying to send stuff to us, let's stop 'em */
				while ((written = send(conn->sockfd, &m, sizeof(m), 0)) < 0)
				{
					if (errno == EINTR)
					{
						ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
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
			DeregisterReadInterest(transportStates, motNodeID, i,
								   "no more input needed");
		}
	}
}

#ifdef AMS_VERBOSE_LOGGING
void
print_connection(ChunkTransportState *transportStates, int fd, const char *msg)
{
	struct sockaddr_in local,
		remote;

	socklen_t	len;
	int			errlevel = transportStates->teardownActive ? LOG : ERROR;

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
#endif

void
format_fd_set(StringInfo buf, int nfds, mpp_fd_set fds, char* pfx, char *sfx)
{
    int     i;

    appendStringInfoString(buf, pfx);
    for (i = 1; i < nfds; i++)
    {
        if (MPP_FD_ISSET(i, &fds))
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
        struct sockaddr_in *	sin = (struct sockaddr_in *)sa;
        uint32					saddr = ntohl(sin->sin_addr.s_addr);

        snprintf(buf, bufsize, "%d.%d.%d.%d:%d",
                 (saddr >> 24)&0xff,
                 (saddr >> 16)&0xff,
                 (saddr >> 8)&0xff,
                 saddr&0xff,
                 ntohs(sin->sin_port));
    }
#ifdef HAVE_IPV6
    else if (sa->sa_family == AF_INET6)
    {
        struct sockaddr_in6    *sin6 = (struct sockaddr_in6 *)sa;
		char remote_port[20];

        if (bufsize > 8)
        {
			/*
			 * inet_ntop isn't portable.
             * //inet_ntop(AF_INET6, &sin6->sin6_addr, buf, bufsize - 8);
			 * 
			 * postgres has a standard routine for converting addresses to printable format,
			 * which works for IPv6, IPv4, and Unix domain sockets.  I've changed this 
			 * routine to use that, but I think the entire format_sockaddr routine could
			 * be replaced with it.
			 */
			pg_getnameinfo_all((const struct sockaddr_storage *)&sin6->sin6_addr, sizeof(sin6->sin6_addr),
							   buf, bufsize-8,
							   remote_port, sizeof(remote_port),
							   NI_NUMERICHOST | NI_NUMERICSERV);
            buf += strlen(buf);
        }
        snprintf(buf, 8, ":%s", remote_port);
    }
#endif
    else
        snprintf(buf, bufsize, "?host?:?port?");

    return buf;
}                               /* format_sockaddr */


static void
flushInterconnectListenerBacklog(void)
{
	int			pendingConn,
		newfd,
		i;
	mpp_fd_set	rset;
	struct timeval timeout;

	do
	{
		MPP_FD_ZERO(&rset);
		MPP_FD_SET(listenerFd, &rset);
		timeout.tv_sec = 0;
		timeout.tv_usec = 10;

		pendingConn = select(listenerFd + 1, (fd_set *)&rset, NULL, NULL, &timeout);
		if (pendingConn > 0)
		{
			for (i = 0; i < pendingConn; i++)
			{
                struct sockaddr_storage remoteAddr;
                struct sockaddr_storage localAddr;
                char        remoteHostAndPort[64];
                char        localHostAndPort[64];
                socklen_t   addrsize;

	            addrsize = sizeof(remoteAddr);
				newfd = accept(listenerFd, (struct sockaddr *)&remoteAddr, &addrsize);
				if (newfd < 0)
				{
                    ereport(DEBUG3, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                                     errmsg("Interconnect error while clearing incoming connections."),
                                     errdetail("%m%s sockfd=%d", "accept", newfd)));
					continue;
				}

				if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE)
				{
					/* Get remote and local host:port strings for message. */
					format_sockaddr((struct sockaddr *)&remoteAddr, remoteHostAndPort,
									sizeof(remoteHostAndPort));
					addrsize = sizeof(localAddr);
					if (getsockname(newfd, (struct sockaddr *)&localAddr, &addrsize))
					{
						ereport(LOG, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
									  errmsg("Interconnect error while clearing incoming connections."),
									  errdetail("%m%s sockfd=%d remote=%s",
												"getsockname",
												newfd,
												remoteHostAndPort)));
					}
					else
					{
						format_sockaddr((struct sockaddr *)&localAddr, localHostAndPort,
										sizeof(localHostAndPort));
						ereport(DEBUG2, (errmsg("Interconnect clearing incoming connection "
												"from remote=%s to local=%s.  sockfd=%d.",
												remoteHostAndPort, localHostAndPort,
												newfd)));
					}
				}

				/* shutdown this socket */
				flushIncomingData(newfd);
				shutdown(newfd, SHUT_WR);
				closesocket(newfd);
			}
		}
		else if (pendingConn < 0 && errno != EINTR)
		{
            ereport(LOG, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
                          errmsg("Interconnect error during listener cleanup."),
                          errdetail("%m%s sockfd=%d", "select", listenerFd)));
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
	mpp_fd_set		rset;

	for (;;)
	{
		if (InterruptPending)
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG3, "waitOnSocketDone(): interrupt pending fast-track");
#endif
			return;
		}

		MPP_FD_ZERO(&rset);
		MPP_FD_SET(fd, &rset);
		timeout = tval;

		n = select(fd + 1, (fd_set *)&rset, NULL, NULL, &timeout);
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

static void *
ic_rx_thread_func(void *arg)
{
	struct icpkthdr *pkt=NULL;
	bool	skip_poll=false;

	for (;;)
	{
		struct pollfd nfd;
		int		timeout;
		int		n;

		if (rx_thread_shutdown)
		{
			if (DEBUG2 >= log_min_messages)
				write_log("udp-ic: rx-thread shutting down\n");
			break;
		}

		if (pkt == NULL)
		{
			pthread_mutex_lock(&rx_thread_mutex);

			pkt = get_rx_buffer();
			if (pkt == NULL)
			{
				if (DEBUG3 >= log_min_messages)
					write_log("no rx_buffer available\n");

				/* SLEEP HERE ? WAIT ON COND_VAR */
				rx_thread_waiting = true;
				pthread_cond_wait(&rx_thread_cond, &rx_thread_mutex);
				rx_thread_waiting = false;

				pthread_mutex_unlock(&rx_thread_mutex);
				continue;
			}

			if (DEBUG5 >= log_min_messages)
				write_log("rx_thread: got buffer %p\n", pkt);

			pthread_mutex_unlock(&rx_thread_mutex);
		}

		if (!skip_poll)
		{
			/* do we have inbound traffic to handle ?*/
			nfd.fd = UDPlistenerFd;
			nfd.events = POLLIN;

			timeout = RX_THREAD_POLL_TIMEOUT;

			n = poll(&nfd, 1, timeout);

			if (n < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				write_log("Interconnect error: poll %d\n", errno);
				break;
			}

			if (n == 0)
			{
				continue;
			}
		}

		if (skip_poll || (n == 1 && (nfd.events & POLLIN)))
		{
			/* we've got something interesting to read */
			/* handle incoming */
			/* ready to read on our socket */
			MotionConn *conn;
			int read_count;

			struct sockaddr_in peer;
			socklen_t peerlen;

			peerlen = sizeof(peer);
			read_count = recvfrom(UDPlistenerFd, (char *)pkt, Gp_max_packet_size, 0,
								  (struct sockaddr *)&peer, &peerlen);

			ic_stats_pkt_count++;

			if (DEBUG5 >= log_min_messages)
				write_log("received inbound len %d\n", read_count);

			if (read_count < 0)
			{
				if (errno == EWOULDBLOCK)
				{
					skip_poll = false;
					continue;
				}
				write_log("Interconnect error: recvfrom\n");
				break;
			}

			/* when we get a "good" recvfrom() result, we can skip poll() until we get a bad one. */
			skip_poll = true;

			if (read_count < sizeof(pkt))
			{
				write_log("Interconnect error: short conn receive\n");
				break;
			}

			/* length must be >= 0 */
			if (pkt->len < 0)
			{
				if (DEBUG3 >= log_min_messages)
					write_log("received inbound with negative length\n");
				continue;
			}
			if (DEBUG5 >= log_min_messages)
				write_log("received inbound mot-id %d listener %d\n",
						  pkt->motNodeId, pkt->srcListenerPort);

			pthread_mutex_lock(&rx_thread_mutex);

			conn = findConnByHeader(pkt, NULL);
			if (conn != NULL)
			{
				/* Is this a connection request ? */
				if (pkt->seq == 0)
				{
					if (DEBUG5 >= log_min_messages)
						write_log("connection request from node %d route %d, seq 0\n",
								  pkt->motNodeId, conn->route);

					if (pkt->len > 0) /* invalid ? */
					{
						if (LOG >= log_min_messages)
							write_log("invalid packet, seq 0 pkt.len %d\n", pkt->len);
						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					/* fill in the peer */
					conn->peer = peer;

					if (conn->state == mcsNull)
					{
						sendAck(conn, false);

						conn->state = mcsStarted;
						conn->stillActive = true;

						conn->conn_info.seq++;
						if (DEBUG4 >= log_min_messages)
							write_log("successful conn request.\n");
					}
					else 
					{
						uint32	seq;

						/* this happens when our ack has been dropped;
						 * or we didn't respond quickly enough. we
						 * send another ack (with the matching
						 * seq-number) */

						ic_stats_dropped_acks++;

						if (DEBUG3 >= log_min_messages)
							write_log("duplicate conn request ?!\n");

						seq = conn->conn_info.seq;
						conn->conn_info.seq = pkt->seq;
						sendAck(conn, false);
						conn->conn_info.seq = seq;

						if (conn->state != mcsStarted)
							write_log("corrupt connection state ?\n");
						if (!conn->stillActive)
							write_log("corrupt stillActive flag?\n");
					}

					/* we'll reuse the current rx-buffer */
					pthread_mutex_unlock(&rx_thread_mutex);
					continue;
				}
				else /* pkt->seq > 0 */
				{
					if (pkt->len == 0) /* stop message ? */
					{
						if (DEBUG1 >= log_min_messages)
							write_log("data packet with length of ZERO, seq %d (ignoring, stop ?)\n",
									  pkt->seq);
						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					/* data packet */

					if (pkt->flags & FLAGS_EOS)
						if (DEBUG3 >= log_min_messages)
							write_log("received packet with EOS motid %d route %d seq %d\n",
									  pkt->motNodeId, conn->route, pkt->seq);

					/* dropped ack ? */
					if (pkt->seq < conn->conn_info.seq)
					{
						uint32	seq;

						ic_stats_dropped_acks++;

						if (DEBUG3 >= log_min_messages)
							write_log("dropped ack ? ignored data packet w/ cmd %d node %d route %d seq %d expected %d flags 0x%x\n",
										 pkt->commandCount, pkt->motNodeId,
										 conn->route, pkt->seq, conn->conn_info.seq, pkt->flags);

						seq = conn->conn_info.seq;
						conn->conn_info.seq = pkt->seq;
						sendAck(conn, false);
						conn->conn_info.seq = seq;

						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					/* sequence number is correct */

					if (!conn->stillActive)
					{
						/* peer may have dropped ack */
						if (DEBUG1 >= log_min_messages)
							write_log("received on inactive connection node %d route %d (seq %d pkt->seq %d)\n",
									  pkt->motNodeId, conn->route, conn->conn_info.seq, pkt->seq);

						conn->conn_info.seq = pkt->seq;
						sendAck(conn, true);
						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					/* valid, do we have space ? */
					if (conn->pkt_q_size == Gp_interconnect_queue_depth)
					{
						/* NO RX SPACE, drop */
						/* SEND NAK HERE */

						/* we could wait for the other thread to clear
						 * its input queue; but for now, we drop
						 * this packet */
						if (DEBUG2 >= log_min_messages)
							write_log("no rx-space for pkt with motid %d route %d seq %d dropped\n",
									  pkt->motNodeId, conn->route, pkt->seq);

						ic_stats_rx_overrun++;

						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					sendAck(conn, false);

					/* valid packet, and we have space. */
					if (DEBUG5 >= log_min_messages)
						write_log("delivered data packet for node %d route %d seq %d\n",
								  pkt->motNodeId, conn->route, pkt->seq);

					/* point the pBuff at the head of the queue */
					if (conn->pkt_q_size == 0)
					{
						conn->pBuff = (uint8 *)pkt;
						conn->msgPos = conn->pBuff;
						conn->msgSize = pkt->len;
						conn->recvBytes = conn->msgSize;
					}

					/* put the packet at the head (advance tail) */
					conn->pkt_q[conn->pkt_q_tail] = (uint8 *)pkt;
					conn->pkt_q_tail++;
					if (conn->pkt_q_tail == Gp_interconnect_queue_depth)
						conn->pkt_q_tail = 0;

					/* advance size */
					conn->pkt_q_size++;

					conn->conn_info.seq++;

					/* Was the main thread waiting for something ? */
					if (main_thread_waiting && main_thread_waiting_node == pkt->motNodeId)
					{
						if (main_thread_waiting_route == ANY_ROUTE)
							main_thread_waiting_route = conn->route;

						if (main_thread_waiting_route == conn->route)
						{
							if (DEBUG2 >= log_min_messages)
								write_log("rx thread: main_waiting waking it route %d (%p)\n", main_thread_waiting_route, conn->pBuff);
							/* WAKE MAIN THREAD HERE */
							pthread_cond_signal(&rx_thread_cond);
						}
					}

					pkt = NULL;

					pthread_mutex_unlock(&rx_thread_mutex);
					continue;
				}
			}
			else
			{
				/* else, ignore and drop */
				/* perhaps we want to check to see if this *is* a
				 * packet from our session (previous command-id ?)
				 * which still needs some acking ?*/

				ic_stats_mismatch++;

				if (DEBUG4 >= log_min_messages)
					write_log("dropping packet from command-id %d seq %d\n", pkt->commandCount, pkt->seq);
				/* we want to ack old packets; but *must* avoid acking connection requests */
				if (pkt->seq > 0 && pkt->sessionId == gp_session_id && gp_command_count == 0)
				{
					MotionConn dummyconn;

					memcpy(&dummyconn.conn_info, pkt, sizeof(struct icpkthdr));
					dummyconn.peer = peer;

					if (DEBUG4 >= log_min_messages)
						write_log("idle, acking old packet ?\n");

					/* we're idle ? send an Ack to a fake conn */
					sendAck(&dummyconn, true);
				}
			}
			pthread_mutex_unlock(&rx_thread_mutex);
		}

		/* pthread_yield(); */
	}

	/* clean up any thread-state here */
	rx_thread_shutdown_complete = true;

	/* nothing to return */
	return NULL;
}
