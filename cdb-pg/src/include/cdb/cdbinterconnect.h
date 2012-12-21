/*-------------------------------------------------------------------------
* cdbinterconnect.h
*	defines state that is used by both the Motion Layer and IPC Layer.
*
* Copyright (c) 2006-2008, Greenplum inc
*-------------------------------------------------------------------------
*/

#ifndef CDBINTERCONNECT_H
#define CDBINTERCONNECT_H

#include "nodes/primnodes.h"
#include "cdb/tupchunklist.h"
#include "access/htup.h"
#include "cdb/htupfifo.h"

struct directTransportBuffer;

#include "cdb/tupser.h"
#include "cdb/tupchunk.h"
#include "cdb/tupchunklist.h"

struct CdbProcess;                          /* #include "nodes/execnodes.h" */
struct Slice;                               /* #include "nodes/execnodes.h" */
struct SliceTable;                          /* #include "nodes/execnodes.h" */

struct icpkthdr
{
	int			motNodeId;

	/*
	 * three pairs which seem useful for identifying packets.
	 *
	 * MPP-4194:
	 * It turns out that these can cause collisions; but the
	 * high bit (1<<31) of the dstListener port is now used
	 * for disambiguation with mirrors.
	 */
	int32		srcPid;
	int32		srcListenerPort;

	int32		dstPid;
	int32		dstListenerPort;

    int32       sessionId;
    uint32      icId;

	/*
	 * are these really useful ? YES THEY ARE! they're guaranteed to
	 * specify a connection.
	 *
	 * MPP-4194: the one piece that they don't specify is the
	 * difference between outbound connections to primaries
	 * vs. mirrors (see above).
	 */
    int32       recvSliceIndex;
    int32       sendSliceIndex;
	int32       srcContentId;
	int32		dstContentId;

	/* MPP-6042: add CRC field */
	uint32		crc;

	/* packet specific info */
	int32		flags;
	int32		len;

	uint32		seq;
};

typedef enum MotionConnState
{
    mcsNull,
    mcsAccepted,
    mcsSetupOutgoingConnection,
    mcsConnecting,
    mcsRecvRegMsg,
    mcsSendRegMsg,
    mcsStarted,
	mcsEosSent
} MotionConnState;

/*
 * Structure used for keeping track of a pt-to-pt connection between two
 * Cdb Entities (either QE or QD).
 */
typedef struct MotionConn
{
	/* socket file descriptor. */
	int			sockfd;

	/* pointer to the data buffer. */
	uint8	   *pBuff;

	/* transmit uses two data buffers, along with a timer */
	uint8	   *ping;
	uint8	   *pong;
	uint8	   *activeXmit;
	GpMonotonicTime activeXmitTime;

	uint16		route;

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
	bool		stopRequested;

    MotionConnState state;

    uint64      wakeup_ms;

	struct icpkthdr		conn_info;
	bool		isMirror;

    struct CdbProcess  *cdbProc;
    int			remoteContentId;
    char        remoteHostAndPort[128];	/* Numeric IP addresses should never be longer than about 50 chars, but play it safe */
    char        localHostAndPort[128];

	struct sockaddr_storage peer;		/* Allow for IPv4 or IPv6 */
	socklen_t peer_len;					/* And remember the actual length */

	/* a queue of maximum length Gp_interconnect_queue_depth */
	int			pkt_q_size;
	int			pkt_q_head;
	int			pkt_q_tail;
	uint8		**pkt_q;

	/* Statistics info for this connection */
	GpMonotonicTime ackWaitBeginTime;

	uint64 stat_total_ack_time;
	uint64 stat_count_acks;
	uint64 stat_max_ack_time;
	uint64 stat_min_ack_time;
	uint64 stat_count_resent;
	uint64 stat_max_resent;
	uint64 stat_count_dropped;

	/* Indicate whether an EOS is received and acked. */
	bool eosAcked;

}	MotionConn;

/*
 * Used to organize all of the information for a given motion node.
 */
typedef struct ChunkTransportStateEntry
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
	mpp_fd_set	readSet;

	/* highest file descriptor in the readSet. */
	int			highReadSock;
    int         scanStart;

    /* slice table entries */
    struct Slice   *sendSlice;
    struct Slice   *recvSlice;

    /* setup info */
    int         outgoingPortRetryCount;

	int			txfd;
	int			txfd_family;
	unsigned short txport;

	bool		sendingEos;

	/* Statistics info for this motion on the interconnect level */
	uint64 stat_total_ack_time;
	uint64 stat_count_acks;
	uint64 stat_max_ack_time;
	uint64 stat_min_ack_time;
	uint64 stat_count_resent;
	uint64 stat_max_resent;
	uint64 stat_count_dropped;

}	ChunkTransportStateEntry;

/* ChunkTransportState array initial size */
#define CTS_INITIAL_SIZE (10)

/*
 * This structure is used to keep track of partially completed tuples,
 * and tuples that have been completed but have not been consumed by
 * the executor yet.
 */
typedef struct ChunkSorterEntry
{
	bool		init;

	/*
	 * A tuple-chunk list containing the chunks for the currently incomplete
	 * HeapTuple being received.
	 */
	TupleChunkListData chunk_list;

	/*
	 * A FIFO to hold the tuples that have been completed but not yet
	 * retrieved.  This will not be initialized until it is actually needed.
	 */
	htup_fifo	ready_tuples;

	/*
	 * Flag recording whether end-of-stream has been reported from the source.
	 */
	bool		end_of_stream;

	/*
	 * PER-(MOTION NODE & SENDER) STATISTICS
	 *
	 * These are utilized primarily in order
	 * preserving motion nodes.
	 */
	/* Total tuples awaiting receive. */
	uint32		stat_tuples_available;

	/* High-water-mark of this value. */
	uint32		stat_tuples_available_hwm;
}	ChunkSorterEntry;

/* This is the entry data-structure for a motion node. */
typedef struct MotionNodeEntry
{
	/*
	 * First value in entry has to be the key value.  The key is the motion
	 * node ID.
	 */
	int16           motion_node_id;

	/*
	 * Flag specifying whether the order of tuples from each source should be
	 * maintained or preserved.
	 */
	bool            preserve_order;

	/*
	 * Our route-based array of htup_fifos, for the case where we are a merge receive.
	 */
	ChunkSorterEntry *ready_tuple_lists;

	/* The description of tuples that this motion node will be exchanging. */
	TupleDesc       tuple_desc;

	/*
	 * The cached information to perform tuple serialization and
	 * deserialization as quickly as possible.
	 */
	SerTupInfo      ser_tup_info;

	/*
	 * If preserve_order is false, this is used to hold completed tuples that
	 * have not yet been consumed.  If preserve_order is true, this is NULL.
	 */
	htup_fifo       ready_tuples;

	/*
	 * Variable that records the total number of senders to this motion node.
	 * This is expected to always be (number of qExecs).
	 */
	uint            num_senders;

	/*
	 * Variable that tracks number of senders that have reported end-of-stream
	 * for this motion node.  When the local node sends end-of-stream, that is
	 * also recorded.
	 */
	uint            num_stream_ends_recvd;

	bool            cleanedUp;
	bool            valid;
	bool            moreNetWork;
	bool            stopped;

	/*
	 * PER-MOTION-NODE STATISTICS
	 */

	uint64          stat_total_chunks_sent; /* Tuple-chunks sent. */
	uint64          stat_total_bytes_sent;  /* Bytes sent, including headers. */
	uint64          stat_tuple_bytes_sent;  /* Bytes of pure tuple-data sent. */

	uint64          stat_total_chunks_recvd;                /* Tuple-chunks received. */
	uint64          stat_total_bytes_recvd; /* Bytes received, including headers. */
	uint64          stat_tuple_bytes_recvd; /* Bytes of pure tuple-data received. */

	uint64          stat_total_sends;               /* Total calls to SendTuple. */

	uint64          stat_total_recvs;               /* Total calls to RecvTuple/etc. */

	uint64          stat_tuples_available;  /* Total tuples awaiting receive. */
	uint64          stat_tuples_available_hwm;              /* High-water-mark of this
		* value. */
	uint64          sel_rd_wait;            /* Total time (usec) spent in select wait trying to read */
	uint64          sel_wr_wait;            /* Total time spent (usec) in select wait trying to write */

	uint64			memKB;	/* How much memory should this motion node use? */
}       MotionNodeEntry;


/*=========================================================================
* MOTION LAYER DATA STRUCTURE
*/

typedef struct MotionLayerState
{
	/* The host ID that this segment-database is on. */
	int			host_id;

	/*
	 * Memory context for the whole motion layer.  This is a child context of
	 * the Executor State Context, so the if it fails to get cleaned up the
	 * estate context should free our resources at the end of the query.
	 */
	MemoryContext motion_layer_mctx;

	/*
	 * MOTION NODE STATE - Initialized and used on per-statement basis.
	 */

#define MNE_INITIAL_COUNT (10)
	int			mneCount;
	MotionNodeEntry *mnEntries;

	/*
	 * GLOBAL MOTION-LAYER STATISTICS
	 */

	uint32		stat_total_chunks_sent; /* Tuple-chunks sent. */
	uint32		stat_total_bytes_sent;	/* Bytes sent, including headers. */
	uint32		stat_tuple_bytes_sent;	/* Bytes of pure tuple-data sent. */

	uint32		stat_total_chunks_recvd;/* Tuple-chunks received. */
	uint32		stat_total_bytes_recvd; /* Bytes received, including headers. */
	uint32		stat_tuple_bytes_recvd; /* Bytes of pure tuple-data received. */

	uint32		stat_total_chunkproc_calls;		/* Calls to processIncomingChunks() */

}	MotionLayerState;

typedef struct ChunkTransportState
{
	/* array of per-motion-node chunk transport state */
	int size;
	ChunkTransportStateEntry *states;

	/* keeps track of if we've "activated" connections via SetupInterconnect(). */
	bool		activated;

	bool		aggressiveRetry;

	bool		teardownActive;
	List	   *incompleteConns;

	/* slice table stuff. */
	struct SliceTable  *sliceTable;
	int			sliceId;

	/* Estate pointer for this statement (UDP-IC specific) */
	EState		*estate;

	/* Function pointers to our send/receive functions */
	bool (*SendChunk)(MotionLayerState *mlStates, struct ChunkTransportState *transportStates, ChunkTransportStateEntry *pEntry, MotionConn *conn, TupleChunkListItem tcItem, int16 motionId);
	TupleChunkListItem (*RecvTupleChunkFrom)(struct ChunkTransportState *transportStates, int16 motNodeID, int16 srcRoute);
	TupleChunkListItem (*RecvTupleChunkFromAny)(MotionLayerState *mlStates, struct ChunkTransportState *transportStates, int16 motNodeID, int16 *srcRoute);
	void (*doSendStopMessage)(struct ChunkTransportState *transportStates, int16 motNodeID);
	void (*SendEos)(MotionLayerState *mlStates, struct ChunkTransportState *transportStates, int motNodeID, TupleChunkListItem tcItem);
} ChunkTransportState;

#endif   /* CDBINTERCONNECT_H */

