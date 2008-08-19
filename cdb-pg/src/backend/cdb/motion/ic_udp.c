/*-------------------------------------------------------------------------
 * ic_udp.c
 *	   Interconnect code specific to UDP transport.
 *
 * Copyright (c) 2005-2008, Greenplum
 *
 * IDENTIFICATION:
 *	   $Id$
 *
 *
 * $File$
 * $Change$
 * $Author$
 * $DateTime$
 * Reviewers: jzhang, tkordas
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
#include "cdb/cdbdisp.h"

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

#ifdef USE_ASSERT_CHECKING
static bool udp_testmode = false;
#endif

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

/*
 * flags definitions for flag-field of UDP-messges
 *
 * we use bit operations to test these, flags are powers of two only
 */
#define UDPIC_FLAGS_ACK  (1)
#define UDPIC_FLAGS_STOP (2)
#define UDPIC_FLAGS_EOS  (4)

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


static uint64	ic_stats_pkt_count=0;
static uint64	ic_stats_dropped_acks=0;
static uint64	ic_stats_retrans=0;
static uint64	ic_stats_mismatch=0;
static uint64	ic_stats_rx_overrun=0;


/* Global hash table for connections. */

struct conn_htab_bin {
	MotionConn *conn;
	struct conn_htab_bin *next;
};

struct conn_htab_bin	**conn_htab = NULL;
#define DEFAULT_CONN_HTAB_SIZE (Max((GpIdentity.numsegments*Gp_interconnect_hash_multiplier), 16))
int						conn_htab_size = 0;

static ChunkTransportStateEntry *startOutgoingUDPConnections(ChunkTransportState *transportStates,
															 Slice *sendSlice,
															 int *pOutgoingCount);
static void setupOutgoingUDPConnection(ChunkTransportState *transportStates,
									   ChunkTransportStateEntry *pEntry, MotionConn *conn);

static char *format_sockaddr(struct sockaddr *sa, char* buf, int bufsize);

static TupleChunkListItem RecvTupleChunkFromAnyUDP(MotionLayerState *mlStates,
												   ChunkTransportState *transportStates,
												   int16 motNodeID,
												   int16 *srcRoute);

static TupleChunkListItem RecvTupleChunkFromUDP(ChunkTransportState *transportStates,
												int16		motNodeID,
												int16		srcRoute);

static void SendEosUDP(MotionLayerState *mlStates, ChunkTransportState *transportStates,
					   int motNodeID, TupleChunkListItem tcItem);
static bool SendChunkUDP(MotionLayerState *mlStates, ChunkTransportState *transportStates,
						 ChunkTransportStateEntry *pEntry, MotionConn * conn, TupleChunkListItem tcItem, int16 motionId);

static TupleChunkListItem receiveChunksUDP(ChunkTransportState *pTransportStates, ChunkTransportStateEntry *pEntry, 
										   int16 motNodeID, int16 *srcRoute, MotionConn *conn, bool inTeardown);

static void doSendStopMessageUDP(ChunkTransportState *transportStates, int16 motNodeID);

static bool dispatcherAYT(void);

static void *ic_rx_thread_func(void *arg);

static void
setupUDPListeningSocket(int *listenerSocketFd, uint16 *listenerPort)
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

	if (Gp_udp_bufsize_k != 0)
		rcvbuf = Gp_udp_bufsize_k * 1024;

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

/*
 * Initialize UDP specific comms, and create rx-thread.
 */
void
InitMotionUDP(int *listenerSocketFd, uint16 *listenerPort)
{
	int pthread_err;
	int i;
	pthread_mutexattr_t m_atts;

	/* attributes of the thread we're creating */
	pthread_attr_t t_atts;

	setupUDPListeningSocket(listenerSocketFd, listenerPort);

	/* set up the connection hash table */
	conn_htab_size = DEFAULT_CONN_HTAB_SIZE;
	conn_htab = (struct conn_htab_bin **)malloc(conn_htab_size * sizeof(struct conn_htab_bin *));

	if (conn_htab == NULL)
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect error: could not allocate connection hash, out of memory."),
						errdetail("%m%s", "malloc")));

	for (i=0; i < conn_htab_size; i++)
		conn_htab[i] = NULL;

	/* Set up our mutex */
	pthread_mutexattr_init(&m_atts);
	pthread_mutexattr_settype(&m_atts, PTHREAD_MUTEX_ERRORCHECK);

	pthread_mutex_init(&rx_thread_mutex, &m_atts);

	/* Start up our rx-thread */

	/* save ourselves some memory: the defaults for thread stack
	 * size are large (1M+) */
	pthread_attr_init(&t_atts);

#ifdef pg_on_solaris
	/* Solaris doesn't have PTHREAD_STACK_MIN ? */
	pthread_attr_setstacksize(&t_atts, (128*1024));
#else
	pthread_attr_setstacksize(&t_atts, Max(PTHREAD_STACK_MIN, (128*1024)));
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

	return;
}

void
CleanupMotionUDP(void)
{
	int i;

	elog(DEBUG2, "udp-ic: telling receiver thread to shutdown.");

	rx_thread_shutdown = true;

	pthread_mutex_unlock(&rx_thread_mutex);
	pthread_join(ic_rx_thread, NULL);

	elog(DEBUG2, "udp-ic: receiver thread shutdown.");

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

#ifdef USE_ASSERT_CHECKING
static inline bool
testmode_drop(int percent)
{
	if (udp_testmode &&
		(gp_udpic_dropseg == UNDEF_SEGMENT || gp_udpic_dropseg == Gp_segment))
	{
			if (random() % 100 < percent)
				return true;
	}

	return false;
}
#endif

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

   	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG5, "waiting (timed) on route %d %s", main_thread_waiting_route, 
			 (main_thread_waiting_route == ANY_ROUTE ? "(any route)" : ""));

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

#define CONN_HASH_VALUE(icpkt) ((uint32)((((icpkt)->srcPid ^ (icpkt)->dstPid)) + (icpkt)->dstContentId))

static bool
conn_addhash(MotionConn *conn)
{
	uint32 hashcode;
	struct conn_htab_bin *bin, *newbin;

	hashcode = CONN_HASH_VALUE(&conn->conn_info) % conn_htab_size;

	/* check for collision ? */
	for (bin = conn_htab[hashcode]; bin != NULL; bin = bin->next)
	{
		if (bin->conn->conn_info.motNodeId == conn->conn_info.motNodeId &&
			bin->conn->conn_info.dstContentId == conn->conn_info.dstContentId &&
			bin->conn->conn_info.srcContentId == conn->conn_info.srcContentId &&

			bin->conn->conn_info.recvSliceIndex == conn->conn_info.recvSliceIndex &&
			bin->conn->conn_info.sendSliceIndex == conn->conn_info.sendSliceIndex &&

			bin->conn->conn_info.srcPid == conn->conn_info.srcPid &&
			bin->conn->conn_info.dstPid == conn->conn_info.dstPid)
		{
			elog(DEBUG5, "conn_addhash(): duplicate ?! node %d route %d", conn->conn_info.motNodeId, conn->route);
			return true; /* false *only* indicates memory-alloc failure. */
		}
	}

	newbin = (struct conn_htab_bin *)malloc(sizeof(struct conn_htab_bin));
	if (newbin == NULL)
	{
		return false;
	}
	newbin->conn = conn;
	newbin->next = conn_htab[hashcode];
	conn_htab[hashcode] = newbin;

	return true;
}

static void
conn_delhash(MotionConn *conn)
{
	uint32 hashcode;
	struct conn_htab_bin *c, *p, *trash;

	hashcode = CONN_HASH_VALUE(&conn->conn_info) % conn_htab_size;

	c = conn_htab[hashcode];

	/* find entry */
	p = NULL;
	while (c != NULL)
	{
		if (c->conn->conn_info.motNodeId == conn->conn_info.motNodeId &&
			c->conn->conn_info.dstContentId == conn->conn_info.dstContentId &&
			c->conn->conn_info.srcContentId == conn->conn_info.srcContentId &&

			c->conn->conn_info.recvSliceIndex == conn->conn_info.recvSliceIndex &&
			c->conn->conn_info.sendSliceIndex == conn->conn_info.sendSliceIndex &&


			c->conn->conn_info.srcPid == conn->conn_info.srcPid &&
			c->conn->conn_info.dstPid == conn->conn_info.dstPid)

			break;

		p = c;
		c = c->next;
	}

	/* not found ? */
	if (c == NULL)
	{
		return;
	}

	/* found the connection, remove from the chain. */
	trash = c;

	if (p == NULL)
		conn_htab[hashcode] = c->next;
	else
		p->next = c->next;

	free(trash);
	return;
}

static MotionConn *
findConnByHeader(struct icpkthdr *hdr)
{
	uint32 hashcode;
	struct conn_htab_bin *bin;
	struct MotionConn *ret=NULL;

	hashcode = CONN_HASH_VALUE(hdr) % conn_htab_size;

	for (bin = conn_htab[hashcode]; bin != NULL; bin = bin->next)
	{
		if (bin->conn->conn_info.motNodeId == hdr->motNodeId &&
			bin->conn->conn_info.dstContentId == hdr->dstContentId &&
			bin->conn->conn_info.srcContentId == hdr->srcContentId &&

			bin->conn->conn_info.recvSliceIndex == hdr->recvSliceIndex &&
			bin->conn->conn_info.sendSliceIndex == hdr->sendSliceIndex &&

			bin->conn->conn_info.srcPid == hdr->srcPid &&
			bin->conn->conn_info.dstPid == hdr->dstPid &&

			bin->conn->conn_info.icId == hdr->icId)
		{
			ret = bin->conn;

			if (DEBUG5 >= log_min_messages)
				write_log("findConnByHeader: found. route %d state %d hashcode %d conn %p\n",
						  ret->route, ret->state, hashcode, ret);

			return ret;
		}
	}

	if (DEBUG5 >= log_min_messages)
		write_log("findConnByHeader: not found! (hdr->srcPid %d "
				  "hdr->srcContentId %d hdr->dstContentId %d hdr->dstPid %d sess(%d:%d) cmd(%d:%d)) hashcode %d\n",
				  hdr->srcPid, hdr->srcContentId, hdr->dstContentId, hdr->dstPid, hdr->sessionId,
				  gp_session_id, hdr->icId, gp_interconnect_id, hashcode);

	return NULL;
}

/*
 * This must be called with the rx_thread mutex held (maybe these locks need to be restructured a bit).
 */
static bool
conn_rehash(void)
{
	int i;
	uint32 new_hashcode;
	int new_htab_size = DEFAULT_CONN_HTAB_SIZE;
	struct conn_htab_bin		**new_conn_htab = NULL;
	struct conn_htab_bin		*bin, *newbin;

	new_conn_htab = (struct conn_htab_bin **)malloc(new_htab_size * sizeof(struct conn_htab_bin *));

	if (new_conn_htab == NULL)
	{
		elog(ERROR, "Could not resize connection hash table (old size %d, new size %d)",
			 conn_htab_size, new_htab_size);
		return false;
	}

	for (i=0; i < new_htab_size; i++)
		new_conn_htab[i] = NULL;

	/* For each bucket in the hash table */
	for (i=0; i < conn_htab_size; i++)
	{
		/* for each entry in this bucket */
		bin = conn_htab[i];

		while (bin)
		{
			MotionConn *conn=bin->conn;

			new_hashcode = CONN_HASH_VALUE(&conn->conn_info) % new_htab_size;

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

	return true;
}

static void
sendAck(MotionConn *conn, bool stop)
{
	int n;
	struct icpkthdr msg;

	if (stop)
		conn->conn_info.flags |= UDPIC_FLAGS_STOP;

	MemSet(&msg, 0, sizeof(msg));
	memcpy(&msg, &conn->conn_info, sizeof(msg));
	msg.flags |= UDPIC_FLAGS_ACK;

	msg.len = 0;

	if (DEBUG5 >= log_min_messages)
		write_log("sendack: flags 0x%x node %d route %d seq %d\n",
				  msg.flags, msg.motNodeId, conn->route, conn->conn_info.seq);

#ifdef USE_ASSERT_CHECKING
	if (testmode_drop(gp_udpic_dropacks_percent))
		return;
#endif

	n = sendto(UDP_listenerFd, (const char *)&msg, sizeof(msg), 0,
			   (struct sockaddr *)&conn->peer, sizeof(conn->peer));

	/* No need to handle EAGAIN here: no-space just means that we
	 * dropped the packet: our ordinary retransmit mechanism will
	 * handle that case*/

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

	/* advance queue head. */
	conn->pkt_q[conn->pkt_q_head] = NULL;
	conn->pkt_q_head++;
	if (conn->pkt_q_head == Gp_interconnect_queue_depth)
		conn->pkt_q_head = 0;
	conn->pkt_q_size--;

	*(char **)buf = rx_buf_freelist;
	rx_buf_freelist = (char *)buf;

	/* now we need to pick up the next element (@ the new head).*/
	if (conn->pkt_q_size == 0)
		conn->pBuff = NULL;
	else
	{
		/* update the protocol-specific info of the newly active rx-buffer */
		conn->pBuff = conn->pkt_q[conn->pkt_q_head];
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
					write_log("Interconnect ran out of rx-buffers count/max %d/%d", rx_buf_count, rx_buf_maxcount);
				break;
			}
			ret = (struct icpkthdr *)malloc(Gp_max_packet_size);
			/* Note: we return NULL if the malloc() fails -- and the
			 * main-thread will either free a buffer up, or notice
			 * that we ran out (throw an erorr) and start the
			 * teardown */
			if (ret != NULL)
				rx_buf_count++;

			break;
		}

		/* we have buffers available in our freelist */
		ret = (struct icpkthdr *)rx_buf_freelist;
		rx_buf_freelist = *(char **)(rx_buf_freelist);
	} while (0);

	return ret;
}

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
            conn->ping = palloc(Gp_max_packet_size);
            conn->pong = palloc(Gp_max_packet_size);
            conn->pBuff = conn->ping;
			conn->activeXmit = NULL;
            conn->state = mcsSetupOutgoingConnection;
			conn->route = i++;
            (*pOutgoingCount)++;
		}

        conn++;
	}

	/* NOTE: we don't reset conn here -- we're walking down the second
	 * half of the connection-list */
	i = 0;
	foreach(cell, recvSlice->mirrorProcesses)
	{
        cdbProc = (CdbProcess *)lfirst(cell);
        if (cdbProc)
		{
            conn->cdbProc = cdbProc;
            conn->ping = palloc(Gp_max_packet_size);
            conn->pong = palloc(Gp_max_packet_size);
            conn->pBuff = conn->ping;
			conn->activeXmit = NULL;
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

	{
		int                 rcvbuf;
		socklen_t           rcvbufsize=0;

		fun = "getsockopt";
		rcvbufsize = sizeof(rcvbuf);
		if (getsockopt(pEntry->txfd, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, &rcvbufsize) < 0)
			goto error;

		elog(DEBUG1, "UDP-IC: xmit default rx-size %d bytes", rcvbuf);

		fun = "setsockopt";
		rcvbuf = 128 * 1024;

		if(Gp_udp_bufsize_k != 0)
			rcvbuf = Gp_udp_bufsize_k * 1024;

		rcvbufsize = sizeof(rcvbuf);
		if (setsockopt(pEntry->txfd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, rcvbufsize) < 0)
			goto error;

		elog(DEBUG1, "UDP-IC: xmit will use rx size of %d bytes", rcvbuf);
	}

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
 *		state then transitions to mcsStarted (once connection request is
 * acked by remote peer).
 */
void
setupOutgoingUDPConnection(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn *conn)
{
    CdbProcess         *cdbProc = conn->cdbProc;
	struct sockaddr_in  saddr;
	int                 n;

    Assert(conn->state == mcsSetupOutgoingConnection);
    Assert(conn->cdbProc);

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

	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG3, "setupOutgoingUDPConnection: node %d route %d srccontent %d dstcontent %d", pEntry->motNodeId, conn->route, Gp_segment, conn->cdbProc->contentid);

	conn->conn_info.srcListenerPort = (Gp_listener_port>>16) & 0x0ffff;
	conn->conn_info.srcPid = MyProcPid;
	conn->conn_info.dstPid = conn->cdbProc->pid;
	conn->conn_info.dstListenerPort = conn->cdbProc->listenerPort;

	conn->conn_info.sessionId = gp_session_id;
	conn->conn_info.icId = gp_interconnect_id;

	if (!conn_addhash(conn))
	{
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect error: could not allocate UDP-IC hash, out of memory."),
						errdetail("%m%s", "malloc")));
		/* not reached */
	}

#ifdef USE_ASSERT_CHECKING
	if (testmode_drop(gp_udpic_dropxmit_percent))
		return;
#endif

	n = sendto(pEntry->txfd, (const char *)&conn->conn_info, sizeof(conn->conn_info), 0,
			   (struct sockaddr *)&saddr, (socklen_t)sizeof(saddr));

	/* we'll let this packet drop. */
	if (n < 0 && errno == EAGAIN)
	{
		if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
			elog(DEBUG4, "setupOutgoingUDPConnection: sendto() return EAGAIN, dropped packet.");
		return;
	}

	if (n != sizeof(conn->conn_info))
	{
	    ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			            errmsg("Interconnect error setting up outgoing "
                               "connection."),
			            errdetail("%m%s", "sendto")));
	}

	return;
}                               /* setupOutgoingUDPConnection */

void
SetupUDPInterconnect(EState *estate)
{
	int			i, n;
	ListCell   *cell;
	Slice	   *mySlice;
	Slice	   *aSlice;
    MotionConn *conn=NULL;
	int			incoming_count = 0;
	int			outgoing_count = 0;
	int			expectedTotalIncoming = 0;
	int			expectedTotalOutgoing = 0;
    int         iteration = 0;
	struct timeval  startTime;
	int             elapsed_ms = 0;
	int             elapsed_timeout = 0;

	ChunkTransportStateEntry *sendingChunkTransportState = NULL;

	Insist(!estate->interconnect_context && estate->es_sliceTable);

	estate->interconnect_context = palloc0(sizeof(ChunkTransportState));

	/* add back-pointer for dispatch check. */
	estate->interconnect_context->estate = estate;

	/* initialize state variables */
	Assert(estate->interconnect_context->size == 0);
	estate->interconnect_context->size = CTS_INITIAL_SIZE;
	estate->interconnect_context->states = palloc0(CTS_INITIAL_SIZE * sizeof(ChunkTransportStateEntry));

	estate->interconnect_context->teardownActive = false;
	estate->interconnect_context->activated = false;
	estate->interconnect_context->incompleteConns = NIL;
	estate->interconnect_context->sliceTable = NULL;
	estate->interconnect_context->sliceId = -1;

	estate->interconnect_context->sliceTable = estate->es_sliceTable;
	estate->interconnect_context->sliceId = estate->es_cur_slice_idx;

	estate->interconnect_context->RecvTupleChunkFrom = RecvTupleChunkFromUDP;
	estate->interconnect_context->RecvTupleChunkFromAny = RecvTupleChunkFromAnyUDP;
	estate->interconnect_context->SendEos = SendEosUDP;
	estate->interconnect_context->SendChunk = SendChunkUDP;
	estate->interconnect_context->doSendStopMessage = doSendStopMessageUDP;

	mySlice = (Slice *) list_nth(estate->interconnect_context->sliceTable->slices, estate->es_cur_slice_idx);

    Assert(mySlice &&
           IsA(mySlice, Slice) &&
           mySlice->sliceIndex == estate->es_cur_slice_idx);

#ifdef USE_ASSERT_CHECKING
	if (gp_udpic_dropseg != UNDEF_SEGMENT ||
		gp_udpic_dropacks_percent != 0 || gp_udpic_dropxmit_percent != 0)
		udp_testmode = true;
	else
		udp_testmode = false;
#endif

	gp_interconnect_id = estate->interconnect_context->sliceTable->ic_instance_id;

	pthread_mutex_lock(&rx_thread_mutex);

	/* rearrange the connection hash table, if required */
	if (conn_htab_size != DEFAULT_CONN_HTAB_SIZE)
	{
		if (!conn_rehash())
		{
			pthread_mutex_unlock(&rx_thread_mutex);

			elog(ERROR, "Could not resize connection hash table allocation failed");
			/* not reached */
		}
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

		aSlice = (Slice *) list_nth(estate->interconnect_context->sliceTable->slices, childId);
		numProcs = list_length(aSlice->primaryProcesses);

#ifdef AMS_VERBOSE_LOGGING
		elog(LOG, "Setup recving connections: my slice %d, childId %d",
			 mySlice->sliceIndex, childId);
#endif

		pEntry = createChunkTransportState(estate->interconnect_context, aSlice, mySlice, numProcs, 0);

		Assert(pEntry);
		Assert(pEntry->valid);

		/* let cdbmotion now how many receivers to expect. */
		setExpectedReceivers(estate->motionlayer_context, childId, numProcs);

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

			if (!conn->pkt_q)
			{
				pthread_mutex_unlock(&rx_thread_mutex);

				ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
					errmsg("Setup UDP interconnect failed: out of memory")));
				/* not reached */
			}

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
			conn->conn_info.icId = gp_interconnect_id;

			estate->interconnect_context->incompleteConns = lappend(estate->interconnect_context->incompleteConns, conn);

			if (!conn_addhash(conn))
			{
				pthread_mutex_unlock(&rx_thread_mutex);

				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error: could not allocate UDP-IC hash, out of memory."),
								errdetail("%m%s", "malloc")));
				/* not reached */
			}
		}

		expectedTotalIncoming += pEntry->numConns;

		/* update the length of our rx freelist */
		rx_buf_maxcount += (pEntry->numConns * Gp_interconnect_queue_depth);
	}

    /* Initiate outgoing connections. */
    if (mySlice->parentIndex != -1)
		sendingChunkTransportState = startOutgoingUDPConnections(estate->interconnect_context, mySlice, &expectedTotalOutgoing);

	pthread_mutex_unlock(&rx_thread_mutex);

    if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        ereport(DEBUG1, (errmsg("SetupUDPInterconnect will activate "
                                "%d incoming, %d outgoing routes.  "
                                "Listening on ports=%d/%d sockfd=%d.",
                                expectedTotalIncoming, expectedTotalOutgoing,
                                Gp_listener_port&0x0ffff, (Gp_listener_port>>16)&0x0ffff, UDP_listenerFd)));

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
			if (conn->state == mcsSetupOutgoingConnection &&
				conn->wakeup_ms <= elapsed_ms + 1)
			{
				setupOutgoingUDPConnection(estate->interconnect_context, sendingChunkTransportState, conn);
				switch (conn->state)
				{
					case mcsSetupOutgoingConnection:
					case mcsConnecting:
						/* Set time limit for connect() to complete. */
						conn->wakeup_ms = TIMEOUT(iteration) + elapsed_ms;
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

				if (pkt.flags & UDPIC_FLAGS_STOP)
				{
					elog(DEBUG1, "during setup received ack with STOP. node %d route %d state %d pktseq %d connseq %d", pkt.motNodeId, conn->route, conn->state, pkt.seq, conn->conn_info.seq);
				}

				conn = findConnByHeader(&pkt);
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
						elog(DEBUG1, "state not waiting for setup node %d route %d state %d conn %p pktflags %d", pkt.motNodeId, conn->route, conn->state, conn, pkt.flags);
				}
				else
					elog(DEBUG3, "Got ack for unknown connection node %d command %d dstpid %d", pkt.motNodeId, pkt.icId, pkt.dstPid);
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
			list_length(estate->interconnect_context->incompleteConns) != 0)
		{
			/* We can't use foreach() because we're going to delete elements as we go */
			cell = list_head(estate->interconnect_context->incompleteConns);
			while (cell != NULL)
			{
				conn = (MotionConn *) lfirst(cell);

				cell = lnext(cell);

				if (conn->state != mcsStarted)
				{
					pthread_mutex_lock(&rx_thread_mutex);

					main_thread_waiting_node = conn->conn_info.motNodeId;
					main_thread_waiting = true;
					main_thread_waiting_route = conn->route;

					if (conn->state == mcsStarted ||
						wait_on_receiver(MAIN_THREAD_COND_TIMEOUT))
					{
						/* Got what we wanted. */
					}
					else
					{
						/* timed out waiting. */
					}
					main_thread_waiting_route = ANY_ROUTE;
					main_thread_waiting_node = -1;
					main_thread_waiting = false;

					pthread_mutex_unlock(&rx_thread_mutex);
				}

				/* we check again, since it may have changed. */
				if (conn->state == mcsStarted)
				{
					incoming_count++;
					estate->interconnect_context->incompleteConns = list_delete_ptr(estate->interconnect_context->incompleteConns, conn);
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

		/* Back to top of loop and look again. */
		elapsed_ms = gettime_elapsed_ms(startTime);
	} while (1);

	estate->interconnect_context->activated = true;

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

void
TeardownUDPInterconnect(ChunkTransportState *transportStates,
						MotionLayerState *mlStates,
						bool forceEOS)
{
	ListCell   *cell;
	ChunkTransportStateEntry *pEntry=NULL;
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

		/* in the olden days, we required that the error case
		 * successfully transmit and end-of-stream message here. But
		 * the introduction of cdbdisp_check_estate_for_cancel()
		 * alleviates for the QD case, and the cross-connection of
		 * primary/mirror writer gangs in the dispatcher (propagation
		 * of cancel between them) fixes the I-S case.
		 *
		 * So the call to forceEosToPeers() is no longer required.
		 */

		/* now it is safe to remove. */
		pEntry = removeChunkTransportState(transportStates, mySlice->sliceIndex);

		closesocket(pEntry->txfd);

		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			conn_delhash(conn);

			conn->pBuff = NULL;
			conn->activeXmit = NULL;

			if (conn->ping)
			{
				pfree(conn->ping);
				conn->ping = NULL;
			}
			if (conn->pong)
			{
				pfree(conn->pong);
				conn->pBuff = NULL;
			}
		}
	}

	/* if we have issued *any* stop requests, we may have peers which
	 * we will mess up if we just start ignoring them (they depend on
	 * our ability to acknowledge their final messages) */
	if (!forceEOS)
	{
		List *pendingStop = NIL;

		foreach(cell, mySlice->children)
   		{
			Slice	*aSlice;
			int		childId = lfirst_int(cell);

			aSlice = (Slice *) list_nth(transportStates->sliceTable->slices, childId);

			getChunkTransportState(transportStates, aSlice->sliceIndex, &pEntry);
			Assert(pEntry);

			pthread_mutex_lock(&rx_thread_mutex);
			for (i = 0; i < pEntry->numConns; i++)
			{
				conn = pEntry->conns + i;

				/* We'll make a first pass at freeing buffer space
				 * here. (we can't wait (see below) until we've freed
				 * up some buffers -- if we do we may deadlock) */
				if (conn->pBuff != NULL)
				{
					put_rx_buffer(conn); /* as a side-effect this may wake the rx_thread */
				}

				if (conn->stillActive)
				{
					pendingStop = lappend(pendingStop, conn);
				}
			}
			pthread_mutex_unlock(&rx_thread_mutex);
		}

		elog(DEBUG3, "TeardownUDP: %d pending stops", list_length(pendingStop));

		pthread_mutex_lock(&rx_thread_mutex);
		while ((cell = list_head(pendingStop)) != NULL)
		{
			conn = (MotionConn *)lfirst(cell);

			if (!conn->stillActive)
			{
				/* remove from the list */
				pendingStop = list_delete_ptr(pendingStop, conn);

				elog(DEBUG3, "TeardownUDP: %d pending stops", list_length(pendingStop));
				continue;
			}

			main_thread_waiting = true;
			main_thread_waiting_node = conn->conn_info.motNodeId;
			main_thread_waiting_route = conn->route;

			if (wait_on_receiver(MAIN_THREAD_COND_TIMEOUT))
			{
				main_thread_waiting_route = ANY_ROUTE;
				main_thread_waiting_node = -1;
				main_thread_waiting = false;

				/* success, remove list (as above) */
				pendingStop = list_delete_ptr(pendingStop, conn);

				elog(DEBUG3, "TeardownUDP: %d pending stops", list_length(pendingStop));

				continue;
			}
			else
			{
				main_thread_waiting_route = ANY_ROUTE;
				main_thread_waiting_node = -1;
				main_thread_waiting = false;

				pthread_mutex_unlock(&rx_thread_mutex);

				/* if requested we want to bail-out to handle errors,
				 * so we use "false" */
				ML_CHECK_FOR_INTERRUPTS(false);

				/* if we made it here we need the lock again. */
				pthread_mutex_lock(&rx_thread_mutex);
			}
		}
		pthread_mutex_unlock(&rx_thread_mutex);
	}

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

		getChunkTransportState(transportStates, aSlice->sliceIndex, &pEntry);
		Assert(pEntry);

        if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
            elog(DEBUG3, "Interconnect closing connections from slice%d",
                 aSlice->sliceIndex);

		pthread_mutex_lock(&rx_thread_mutex);
		/*
		 * receivers know that they no longer care about data from
		 * below ... so we can safely discard data queued in both
		 * directions
		 */
		for (i = 0; i < pEntry->numConns; i++)
		{
			conn = pEntry->conns + i;

			if (conn->stillActive && !forceEOS)
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
		rx_buf_maxcount -= (pEntry->numConns * Gp_interconnect_queue_depth);
		pfree(pEntry->conns);

		pthread_mutex_unlock(&rx_thread_mutex);

		/* remove it */
		removeChunkTransportState(transportStates, aSlice->sliceIndex);
	}

	pthread_mutex_lock(&rx_thread_mutex);
	/* now that we've moved active rx-buffers to the freelist, we can prune the freelist itself */
	while (rx_buf_count > rx_buf_maxcount)
	{
		struct icpkthdr *buf=NULL;

		if (rx_buf_freelist == NULL)
			elog(LOG, "freelist NULL: count %d max %d buf %p", rx_buf_count, rx_buf_maxcount, rx_buf_freelist);
		Assert(rx_buf_freelist != NULL);

		buf = (struct icpkthdr *)rx_buf_freelist;
		rx_buf_freelist = *(char **)(rx_buf_freelist);

		free(buf);
		rx_buf_count--;
	}

	if (rx_thread_waiting)
	{
		elog(LOG, "During teardownUDP, rx_thread is waiting (waking)");
		pthread_cond_signal(&rx_thread_cond);
	}

	elog(DEBUG1, "TeardownUDPInterconnect: total pkt count " INT64_FORMAT
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

	if (transportStates != NULL)
	{
		if (transportStates->states != NULL)
			pfree(transportStates->states);
		pfree(transportStates);
	}

	if (forceEOS)
		RESUME_INTERRUPTS();

#ifdef AMS_VERBOSE_LOGGING
	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "TeardownUDPInterconnect successful");
#endif
}

/* ALWAYS CALLED WITH THREAD MUTEX *LOCKED* BEFORE RETURNING, *MUST* UNLOCK. */
TupleChunkListItem
receiveChunksUDP(ChunkTransportState *pTransportStates, ChunkTransportStateEntry *pEntry, 
				 int16 motNodeID, int16 *srcRoute, MotionConn *conn, bool inTeardown)
{
	int			retries=0;
	bool		directed=false;
	MotionConn	*rxconn = NULL;
	TupleChunkListItem	tcItem=NULL;

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "receivechunksUDP: motnodeid %d", motNodeID);
#endif

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
		retries++;

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

		/* every 15 or 16 wait-intervals, check to see if the dispatcher*/
		if (Gp_role == GP_ROLE_DISPATCH &&
			(retries & 0x0f) == 0)
		{
			Assert(pTransportStates);
			Assert(pTransportStates->estate);

			if (cdbdisp_check_estate_for_cancel(pTransportStates->estate))
			{
				pthread_mutex_unlock(&rx_thread_mutex);

				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error QD lost contact with QE")));

				/* not reached */
				return NULL;
			}
		}

		/* 2. Wait for data to become ready */
		if (wait_on_receiver(MAIN_THREAD_COND_TIMEOUT))
			continue; /* success ! */

		/* handle timeout, check for cancel */
		pthread_mutex_unlock(&rx_thread_mutex);

		ML_CHECK_FOR_INTERRUPTS(inTeardown);

		if ((retries & 0x3f) == 0)
		{
			if (!dispatcherAYT())
			{
				if (Gp_role == GP_ROLE_EXECUTE)
					ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
									errmsg("Interconnect error QE lost contact with QD (recv)")));
				else
					ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
									errmsg("Interconnect error QD lost contact with client (recv)")));
			}
		}

		if (rx_thread_waiting)
		{
			/* deadlock, out of memory! */
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error: UDP-IC out of memory.")));
		}

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

	Insist(transportStates && transportStates->activated);

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
	tcItem = receiveChunksUDP(transportStates, pEntry, motNodeID, srcRoute, NULL, transportStates->teardownActive);

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

	Insist(transportStates && transportStates->activated);

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
	return receiveChunksUDP(transportStates, pEntry, motNodeID, &route, conn, transportStates->teardownActive);
}

void
markUDPConnInactive(MotionConn *conn)
{
	pthread_mutex_lock(&rx_thread_mutex);
	conn->stillActive = false;
	pthread_mutex_unlock(&rx_thread_mutex);

	return;
}

/*
 * handle acks incoming from our upstream peers.
 *
 * if we receive a stop message, return true (caller will clean up).
 */
static bool
handleAcks(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry)
{
	bool ret=false;
	MotionConn *ackConn;
	int n;

	struct sockaddr_in peer;
	socklen_t peerlen;

	struct icpkthdr pkt;

	for (;;)
	{
		/* ready to read on our socket ? */
		peerlen = sizeof(peer);
		n = recvfrom(pEntry->txfd, (char *)&pkt, sizeof(pkt), 0,
					 (struct sockaddr *)&peer, &peerlen);

		if (n < 0)
		{
			if (errno == EWOULDBLOCK) /* had nothing to read. */
				return ret;

			ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
			if (errno == EINTR)
				continue;

			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error waiting for peer ack"),
							errdetail("During recvfrom() call.\n")));
			/* not reached */
		}
		else if (n < sizeof(pkt))
		{
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Interconnect error waiting for peer ack"),
							errdetail("short read during recvfrom() call. (%d)\n", n)));
			/* not reached */
		}

		/* read packet, is this the ack we want ? */
		if (pkt.srcContentId == Gp_segment &&
			pkt.srcPid == MyProcPid &&
			pkt.srcListenerPort == ((Gp_listener_port>>16) & 0x0ffff) &&
			pkt.sessionId == gp_session_id &&
			pkt.icId == gp_interconnect_id)
		{
			/* packet is for me */

			ackConn = findConnByHeader(&pkt);

			if (ackConn == NULL)
			{
				elog(LOG, "Received ack for unknown connection (flags 0x%x)", pkt.flags);
				continue;
			}

			if (pkt.seq != ackConn->conn_info.seq)
			{
				if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
					elog(DEBUG2, "ack with bad seq?! expected %d got %d flags 0x%x", ackConn->conn_info.seq, pkt.seq, pkt.flags);

				continue;
			}

			/* Has this connection already received a stop request ?
			 * once we processed the first stop request, we are
			 * waiting for the ack of our ack (which will have EOS
			 * set). Ignore anything else. */
			if (ackConn->stopRequested)
			{
				if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
					elog(DEBUG2, "got ack-stop ack; srcpid %d dstpid %d cmd %d flags 0x%x pktseq %d connseq %d", pkt.srcPid, pkt.dstPid, pkt.icId, pkt.flags, pkt.seq, ackConn->conn_info.seq);

				if (pkt.flags & UDPIC_FLAGS_EOS)
				{
					ackConn->conn_info.seq++;
					if (ackConn->activeXmit)
						ackConn->activeXmit = NULL;
				}
				continue;
			}

			/* haven't gotten a stop request, maybe this is one ? */
			if (pkt.flags & UDPIC_FLAGS_STOP)
			{
				if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
					elog(DEBUG2, "got ack with stop; srcpid %d dstpid %d cmd %d flags 0x%x pktseq %d connseq %d", pkt.srcPid, pkt.dstPid, pkt.icId, pkt.flags, pkt.seq, ackConn->conn_info.seq);

				ackConn->stopRequested = true;
				ackConn->conn_info.flags |= UDPIC_FLAGS_STOP;
				ret = true;

				ackConn->conn_info.seq++;
				if (ackConn->activeXmit)
					ackConn->activeXmit = NULL;

				continue;
			}

			/* this isn't a stop request. it must be a regular ack. */
			if (pkt.flags & UDPIC_FLAGS_ACK)
			{
				ackConn->conn_info.seq++;

				if (ackConn->activeXmit)
					ackConn->activeXmit = NULL;

				if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
					elog(DEBUG5, "got good ack with seq %d node %d route %d peer %d", pkt.seq, pkt.motNodeId, ackConn->route, pkt.dstPid);
				continue;
			}
		}
		else
			elog(DEBUG4, "flushUDPbuffer: not the ack we're looking for (flags 0x%x)...mot(%d) content(%d:%d) srcpid(%d:%d) dstpid(%d) srcport(%d:%d) dstport(%d) sess(%d:%d) cmd(%d:%d)",
				 pkt.flags, pkt.motNodeId,
				 pkt.srcContentId, Gp_segment,
				 pkt.srcPid, MyProcPid, 
				 pkt.dstPid,
				 pkt.srcListenerPort, ((Gp_listener_port>>16) & 0x0ffff),
				 pkt.dstListenerPort,
				 pkt.sessionId, gp_session_id,
				 pkt.icId, gp_interconnect_id);
	}

	return ret;
}

static void
sendOnce(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn * conn)
{
	int			n;

	Assert(conn->activeXmit);

#ifdef USE_ASSERT_CHECKING
	if (testmode_drop(gp_udpic_dropxmit_percent))
	{
		return;
	}
#endif

xmit_retry:
	n = sendto(pEntry->txfd, conn->activeXmit, conn->conn_info.len, 0,
			   (struct sockaddr *)&conn->peer, (socklen_t)sizeof(conn->peer));
	if (n < 0)
	{
		ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);
		if (errno == EINTR)
			goto xmit_retry;

		if (errno == EAGAIN) /* no space ? not an error. */
			return;
			
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect error writing an outgoing packet"),
						errdetail("error during sendto() call (error:%d).\n"
								  "For Remote Connection: contentId=%d at %s",
								  errno, conn->remoteContentId,
								  conn->remoteHostAndPort)));
		/* not reached */
	}

	if (n != conn->conn_info.len)
	{
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("Interconnect error writing an outgoing packet"),
						errdetail("short transmit during sendto() call.\n"
								  "For Remote Connection: contentId=%d at %s",
								  conn->remoteContentId,
								  conn->remoteHostAndPort)));
		/* not reached */
	}

	return;
}

static bool
waitOnXmit(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn *conn)
{
	int		retry = 0;
	int		timeout;
	int		elapsed_ms = 0;
	bool	got_stops=false;
	int		n;
	struct pollfd nfd;

	/* send this packet */
	for (;;)
	{
		/* MPP-3088: don't get elapsed time on first call */
		if (retry)
			elapsed_ms = gettime_elapsed_ms(conn->activeXmitTime);

		timeout = TIMEOUT(retry);

		if (timeout <= elapsed_ms)
		{
			sendOnce(transportStates, pEntry, conn);

			if (timeout)
			{
				conn->activeXmitTime.tv_usec += timeout * 1000;
				if (conn->activeXmitTime.tv_usec > 1000000)
				{
					conn->activeXmitTime.tv_usec -= 1000000;
					conn->activeXmitTime.tv_sec += 1;
				}
			}

			if (retry)
				ic_stats_retrans++;

			retry++;
		}

		if (Gp_role == GP_ROLE_EXECUTE &&
			(retry & 0x3f) == 0)
		{
			if (!dispatcherAYT())
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Interconnect error QE lost contact with QD (xmit)")));
		}

		timeout = Max(1, timeout);

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
							errdetail("During poll() call.\n")));
			/* not reached */
		}

		if (n == 0) /* timeout */
			continue;

		/* got an ack to handle (possibly a stop message) */
		if (n == 1 && (nfd.events & POLLIN))
		{
			if (handleAcks(transportStates, pEntry))
				got_stops = true;

			if (!conn->activeXmit)
				break;
		}
	}

	return got_stops;
}

/*
 * This is a helper function for flushUDPBuffer(). We're essentially
 * acknowledging the stop message, and agreeing that the connection is
 * done. Note that as we do this we may be getting more stop messages;
 * which we have to handle too (so we re-scan our list as we get those
 * kind of responses).
 *
 * Called from flushUDPBuffer(), after noticing the return from
 * flushUDPBuffer().
 */
static void
handleStopMsgs(ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, int16 motionId)
{
	int i=0;
	bool morestops=false;

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "handleStopMsgs: node %d", motionId);
#endif

	while (i < pEntry->numConns)
	{
		MotionConn *conn=NULL;

		conn = pEntry->conns + i;

#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG3, "handleStopMsgs: node %d route %d %s %s", motionId, conn->route,
			(conn->stillActive ? "active" : "NOT active"), (conn->stopRequested ? "stop requested" : ""));
		elog(DEBUG3, "handleStopMsgs: node %d route %d msgSize %d", motionId, conn->route, conn->msgSize);
#endif

		/* MPP-2427: we're guaranteed to have recently flushed, but
		 * this might not be empty (if we got a stop on a buffer that
		 * wasn't the one we were sending) ... empty it first so the
		 * outbound buffer is empty when we get here. */
		if (conn->stillActive && conn->stopRequested)
		{
			/* mark buffer empty */
			conn->tupleCount = 0;
			conn->msgSize = sizeof(conn->conn_info);

			/* now send our stop-ack EOS */
			conn->conn_info.flags |= UDPIC_FLAGS_EOS;
			conn->pBuff[conn->msgSize] = 'S';
			conn->msgSize += 1;

			/* prepare this for transmit */
			conn->conn_info.len = conn->msgSize;

			memcpy(conn->pBuff, &conn->conn_info, sizeof(conn->conn_info));

			gettimeofday(&conn->activeXmitTime, NULL);

			/* active xmit, advance xmit buffer */
			conn->activeXmit = conn->pBuff;

			/* now ready to actually send */

			elog(DEBUG3, "handleStopMsgs: node %d route %d", motionId, i);
			morestops = waitOnXmit(transportStates, pEntry, conn);

			Assert(!conn->activeXmit);

			conn->stillActive = false;
			conn->stopRequested = false;

			if (morestops)
			{
				i=0;
				continue;
			}
		}

		i++;
	}
}

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
SendChunkUDP(MotionLayerState *mlStates, ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn *conn, TupleChunkListItem tcItem, int16 motionId)
{
	bool	gotStops=false;
	int		length=TYPEALIGN(TUPLE_CHUNK_ALIGN, tcItem->chunk_length);

	Assert(conn->msgSize > 0);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG5, "sendChunk: msgSize %d this chunk length %d", conn->msgSize, tcItem->chunk_length);
#endif

	if (conn->msgSize + length <= Gp_max_packet_size)
	{
		memcpy(conn->pBuff + conn->msgSize, tcItem->chunk_data, tcItem->chunk_length);
		conn->msgSize += length;

		conn->tupleCount++;

		return true;
	}
	else if (conn->activeXmit)
	{
		if (waitOnXmit(transportStates, pEntry, conn))
			gotStops = true;
	}

	Assert(!conn->activeXmit);

	/* prepare this for transmit */
	conn->conn_info.len = conn->msgSize;
	if (pEntry->sendingEos)
		conn->conn_info.flags |= UDPIC_FLAGS_EOS;

	memcpy(conn->pBuff, &conn->conn_info, sizeof(conn->conn_info));

	gettimeofday(&conn->activeXmitTime, NULL);

	/* active xmit, advance xmit buffer */
	conn->activeXmit = conn->pBuff;
	conn->pBuff = (conn->pBuff == conn->ping ? conn->pong : conn->ping);

	conn->tupleCount = 0;
	conn->msgSize = sizeof(conn->conn_info);

	/* Send the packet */
	sendOnce(transportStates, pEntry, conn);

	/* handle any acks, but don't wait */
	if (handleAcks(transportStates, pEntry))
		gotStops = true;

	/* if we got a stop */
	if (gotStops)
	{
		/* handle stop messages here */
		handleStopMsgs(transportStates, pEntry, motionId);
	}

	/* now we can copy the input to the new buffer */
	memcpy(conn->pBuff + conn->msgSize, tcItem->chunk_data, tcItem->chunk_length);
	conn->msgSize += length;

	conn->tupleCount++;

	return true;
}

/* See ml_ipc.h */
static void
SendEosUDP(MotionLayerState *mlStates,
		   ChunkTransportState *transportStates,
		   int motNodeID,
		   TupleChunkListItem tcItem)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn *conn;
	int			i;
	bool		stopRace=false;

	Insist(transportStates && (transportStates->activated || transportStates->teardownActive));

	/* check em' */
	ML_CHECK_FOR_INTERRUPTS(transportStates->teardownActive);

	getChunkTransportState(transportStates, motNodeID, &pEntry);

	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG3, "Interconnect seg%d slice%d sending end-of-stream to slice%d",
			 Gp_segment, motNodeID, pEntry->recvSlice->sliceIndex);

	pEntry->sendingEos = true;

	/* we want to add our tcItem onto each of the outgoing buffers --
	 * this is guaranteed to leave things in a state where a flush is
	 * *required*. 
	 */
	doBroadcast(mlStates, transportStates, pEntry, tcItem);

	/* now flush all of the buffers. */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;

#ifdef AMS_VERBOSE_LOGGING
		elog(LOG, "SendEosUDP: %d, flags %d %s %s %d",
			 i, conn->conn_info.flags, conn->state == mcsStarted ? "started" : "not started", conn->stillActive ? "active" : "not active", conn->stopRequested);
#endif

		if (conn->state == mcsStarted && conn->stillActive)
		{
			elog(DEBUG2, "sent eos to route %d tuplecount %d seq %d flags 0x%x stillActive %s",
				 conn->route, conn->tupleCount, conn->conn_info.seq, conn->conn_info.flags, (conn->stillActive ? "true" : "false"));

			if (conn->activeXmit)
			{
				if (waitOnXmit(transportStates, pEntry, conn))
				{
					handleStopMsgs(transportStates, pEntry, motNodeID);
					if (!conn->stillActive)
						continue;
				}
			}
			Assert(!conn->activeXmit);

			/* prepare this for transmit */
			conn->conn_info.len = conn->msgSize;
			if (pEntry->sendingEos)
				conn->conn_info.flags |= UDPIC_FLAGS_EOS;

			memcpy(conn->pBuff, &conn->conn_info, sizeof(conn->conn_info));

			gettimeofday(&conn->activeXmitTime, NULL);

			/* active xmit, advance xmit buffer */
			conn->activeXmit = conn->pBuff;
			conn->pBuff = (conn->pBuff == conn->ping ? conn->pong : conn->ping);

			conn->tupleCount = 0;
			conn->msgSize = sizeof(conn->conn_info);

			if (waitOnXmit(transportStates, pEntry, conn))
			{
				stopRace = true;
			}
			else
			{
				conn->state = mcsEosSent;
				conn->stillActive = false;
			}
			Assert(!conn->activeXmit);
		}
#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG5, "SendEosUDP() Leaving");
#endif
	}

	if (stopRace)
		handleStopMsgs(transportStates, pEntry, motNodeID);
}

static void
doSendStopMessageUDP(ChunkTransportState *transportStates, int16 motNodeID)
{
	ChunkTransportStateEntry *pEntry = NULL;
	MotionConn *conn;
	int			i;

	getChunkTransportState(transportStates, motNodeID, &pEntry);
	Assert(pEntry);

	if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG3, "Interconnect needs no more input from slice%d; notifying senders to stop.",
			 motNodeID);

	/*
	 * Note: we're only concerned with receivers here, so mirrors
	 * don't matter
	 */
	for (i = 0; i < pEntry->numConns; i++)
	{
		conn = pEntry->conns + i;

		pthread_mutex_lock(&rx_thread_mutex);
		if (conn->stillActive)
		{
			if (conn->conn_info.flags & UDPIC_FLAGS_EOS)
			{
				/* we have a queued packet that has EOS in it. We've acked it, so we're done */
				elog(DEBUG4, "do sendstop: already have queued EOS packet, we're done. node %d route %d", motNodeID, i);
				Assert(conn->pBuff != NULL);
				conn->stillActive = false;

				while (conn->pBuff != NULL)
				{
					put_rx_buffer(conn); /* as a side-effect this may wake the rx_thread */
				}
			}
			else
			{
				conn->stopRequested = true;
				conn->conn_info.flags |= UDPIC_FLAGS_STOP;
				sendAck(conn, true);
				elog(DEBUG4, "sent stop message. node %d route %d seq %d", motNodeID, i, conn->conn_info.seq);
			}
		}
		pthread_mutex_unlock(&rx_thread_mutex);
	}
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

/*
 * Check the connection from the dispatcher to verify that it is still
 * there.
 *
 * The connection is a struct Port, stored in the global MyProcPort.
 *
 * Return true if the dispatcher connection is still alive.
 */
static bool
dispatcherAYT(void)
{
	ssize_t		ret;
	char		buf;

	if (MyProcPort->sock < 0)
		return false;

	ret = recv(MyProcPort->sock, &buf, 1, MSG_PEEK|MSG_DONTWAIT);

	if (ret == 0) /* socket has been closed. EOF */
		return false;

	if (ret > 0) /* data waiting on socket, it must be OK. */
		return true;

	if (ret == -1) /* error, or would be block. */
	{
		if (errno == EAGAIN || errno == EINPROGRESS)
			return true; /* connection intact, no data available */
		else
			return false;
	}
	/* not reached */

	return true;
}

static void *
ic_rx_thread_func(void *arg)
{
	struct icpkthdr *pkt=NULL;
	bool	skip_poll=false;

	gp_set_thread_sigmasks();

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
			nfd.fd = UDP_listenerFd;
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
			read_count = recvfrom(UDP_listenerFd, (char *)pkt, Gp_max_packet_size, 0,
								  (struct sockaddr *)&peer, &peerlen);

			ic_stats_pkt_count++;

			if (DEBUG5 >= log_min_messages)
				write_log("received inbound len %d\n", read_count);

			if (read_count < 0)
			{
				if (errno == EWOULDBLOCK || errno == EINTR)
				{
					skip_poll = false;
					continue;
				}
				write_log("Interconnect error: recvfrom (%d)\n", errno);
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

			conn = findConnByHeader(pkt);
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

						/* MPP-3088: if the main thread is waiting on
						 * inbound connections, wake it if appropriate.
						 *
						 * All such waiters will have a specific
						 * conn->route set (that is, ANY_ROUTE isn't
						 * allowed).
						 */
						if (main_thread_waiting && main_thread_waiting_node == pkt->motNodeId)
						{
							if (main_thread_waiting_route == conn->route)
							{
								/* WAKE MAIN THREAD HERE */
								pthread_cond_signal(&rx_thread_cond);
							}
						}
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
						{
							write_log("duplicate connection request from node %d route %d, seq 0\n",
									  pkt->motNodeId, conn->route);
							if (conn->stopRequested)
							{
								write_log("duplicate conn request, stopRequested!\n");
							}
						}

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
					if (pkt->len != read_count)
					{
						write_log("received inbound packet, short: read %d bytes, pkt->len %d\n", read_count, pkt->len);
						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					if (pkt->len == 0) /* no data ? */
					{
						if (DEBUG1 >= log_min_messages)
							write_log("data packet with length of ZERO, seq %d (ignoring, stop ?)\n",
									  pkt->seq);
						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					/* data packet */
					if (pkt->flags & UDPIC_FLAGS_EOS)
						if (DEBUG3 >= log_min_messages)
							write_log("received packet with EOS motid %d route %d seq %d\n",
									  pkt->motNodeId, conn->route, pkt->seq);

					if (conn->stopRequested && conn->stillActive)
					{
						if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG && DEBUG5 >= log_min_messages)
							write_log("rx_thread got packet on active connection marked stopRequested. "
									  "(flags 0x%x) node %d route %d pkt seq %d conn seq %d",
									  pkt->flags, pkt->motNodeId, conn->route, pkt->seq, conn->conn_info.seq);

						/* can we update stillActive ? */
						if (DEBUG2 >= log_min_messages)
							if (!(pkt->flags & UDPIC_FLAGS_STOP) &&
								!(pkt->flags & UDPIC_FLAGS_EOS))
								write_log("stop requested but no stop flag on return packet ?!");

						if (pkt->flags & UDPIC_FLAGS_EOS)
							conn->conn_info.flags |= UDPIC_FLAGS_EOS;

						conn->conn_info.seq = pkt->seq;
						sendAck(conn, true);

						/* we only update stillActive if eos has been sent by peer. */
						if (pkt->flags & UDPIC_FLAGS_EOS)
						{
							if (DEBUG2 >= log_min_messages)
								write_log("stop requested and acknowledged by sending peer");
							conn->stillActive = false;

							/* we may need to wake a waiter */
							if (main_thread_waiting && main_thread_waiting_node == pkt->motNodeId)
							{
								if (main_thread_waiting_route == conn->route)
								{
									/* WAKE MAIN THREAD HERE */
									pthread_cond_signal(&rx_thread_cond);
								}
							}
						}

						pthread_mutex_unlock(&rx_thread_mutex);
						continue;
					}

					/* dropped ack ? */
					if (pkt->seq < conn->conn_info.seq)
					{
						uint32	seq;

						ic_stats_dropped_acks++;

						if (DEBUG3 >= log_min_messages)
							write_log("dropped ack ? ignored data packet w/ cmd %d conn->cmd %d node %d route %d seq %d expected %d flags 0x%x\n",
									  pkt->icId, conn->conn_info.icId, pkt->motNodeId,
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
						if (gp_log_interconnect >= GPVARS_VERBOSITY_VERBOSE &&
							DEBUG1 >= log_min_messages)
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

					/* we need to know about packets which we may have
					 * "in queue" which have EOS, so we don't bother
					 * with things like stop messages */
					if (pkt->flags & UDPIC_FLAGS_EOS)
						conn->conn_info.flags |= UDPIC_FLAGS_EOS;

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
					write_log("dropping packet from command-id %d seq %d (my cmd %d)\n", pkt->icId, pkt->seq, gp_interconnect_id);

				/* we want to ack old packets; but *must* avoid acking connection requests */
				if (pkt->seq > 0 && pkt->sessionId == gp_session_id && 
					(gp_interconnect_id == 0 || gp_interconnect_id >= pkt->icId))
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
