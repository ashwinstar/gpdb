/*-------------------------------------------------------------------------
* cdbinterconnect.h
*	defines state that is used by both the Motion Layer and IPC Layer.
*
* Copyright (c) 2006, Greenplum
*-------------------------------------------------------------------------
*/

#ifndef CDBINTERCONNECT_H
#define CDBINTERCONNECT_H

#include "nodes/primnodes.h"
#include "cdb/tupchunklist.h"
#include "access/htup.h"
#include "cdb/htupfifo.h"
#include "cdb/tupser.h"
#include "cdb/tupchunk.h"
#include "cdb/tupchunklist.h"

struct CdbProcess;                          /* #include "nodes/execnodes.h" */
struct Slice;                               /* #include "nodes/execnodes.h" */
struct SliceTable;                          /* #include "nodes/execnodes.h" */

struct icpkthdr
{
	int			motNodeId;

	/* three pairs which seem useful for identifying packets */
	int32		srcPid;
	int32		srcListenerPort;

	int32		dstPid;
	int32		dstListenerPort;
	
    int32       sessionId;
    int32       commandCount;

	/* are these really useful ? YES THEY ARE! they're guaranteed to specify a connection */
    int32       recvSliceIndex;
    int32       sendSliceIndex;
	int32       srcContentId;
	int32		dstContentId;

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

/* Structure used for keeping track of a pt-to-pt connection between two
* Cdb Entities (either QE or QD).
*/
typedef struct MotionConn
{
	/* socket file descriptor. */
	int			sockfd;
	
	/* pointer to the data buffer. */
	uint8	   *pBuff;

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
	
    int         wakeup_ms;

	struct icpkthdr		conn_info;
	
    struct CdbProcess  *cdbProc;
    int			remoteContentId;
    char        remoteHostAndPort[64];
    char        localHostAndPort[64];

	struct sockaddr_in peer;

	/* a queue of maximum length Gp_interconnect_queue_depth */
	int			pkt_q_size;
	int			pkt_q_head;
	int			pkt_q_tail;
	uint8		**pkt_q;
}	MotionConn;

/* used to organize all of the information for a given motion node.
*
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
	unsigned short txport;

	bool		sendingEos;
	
}	ChunkTransportStateEntry;

/* ChunkTransportState array initial size */
#define CTS_INITIAL_SIZE (10)

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
	
	/*slice table stuff. */
	struct SliceTable  *sliceTable;
	int			sliceId;	
} ChunkTransportState;

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
	
	/*----------------------------*/
	/* PER-MOTION-NODE STATISTICS */
	/*----------------------------*/
	
	uint32          stat_total_chunks_sent; /* Tuple-chunks sent. */
	uint32          stat_total_bytes_sent;  /* Bytes sent, including headers. */
	uint32          stat_tuple_bytes_sent;  /* Bytes of pure tuple-data sent. */
	
	uint32          stat_total_chunks_recvd;                /* Tuple-chunks received. */
	uint32          stat_total_bytes_recvd; /* Bytes received, including headers. */
	uint32          stat_tuple_bytes_recvd; /* Bytes of pure tuple-data received. */
	
	uint32          stat_total_sends;               /* Total calls to SendTuple. */
	
	uint32          stat_total_recvs;               /* Total calls to RecvTuple/etc. */
	
	uint32          stat_tuples_available;  /* Total tuples awaiting receive. */
	uint32          stat_tuples_available_hwm;              /* High-water-mark of this
		* value. */
	uint32          sel_rd_wait;            /* Total time (usec) spent in select wait trying to read */
	uint32          sel_wr_wait;            /* Total time spent (usec) in select wait trying to write */
	
}       MotionNodeEntry;


/*=========================================================================
* MOTION LAYER DATA STRUCTURE
*/

typedef struct MotionLayerState
{
	/*--------------*/
	/* GLOBAL STATE */
	/*--------------*/
	
	/* The host ID that this segment-database is on. */
	int			host_id;
	
	/*
	 * Memory context for the whole motion layer.  This is a child context of
	 * the Executor State Context, so the if it fails to get cleaned up the 
	 * estate context should free our resources at the end of the query.
	 */
	MemoryContext motion_layer_mctx;
	
	/*------------------------------------------------------------------*/
	/* MOTION NODE STATE - Initialized and used on per-statement basis. */
	/*------------------------------------------------------------------*/
	
#define MNE_INITIAL_COUNT (10)
	int			mneCount;
	MotionNodeEntry *mnEntries;
	
	/*
	 * Map to store incomplete tuples (and completed ones, when
									   * order_preserving == true), associated with the sender and intended
	 * destination motion-node.
	 */
	HTAB	   *chunksort_map;
	
	/*--------------------------------*/
	/* GLOBAL MOTION-LAYER STATISTICS */
	/*--------------------------------*/
	
	uint32		stat_total_chunks_sent; /* Tuple-chunks sent. */
	uint32		stat_total_bytes_sent;	/* Bytes sent, including headers. */
	uint32		stat_tuple_bytes_sent;	/* Bytes of pure tuple-data sent. */
	
	uint32		stat_total_chunks_recvd;		/* Tuple-chunks received. */
	uint32		stat_total_bytes_recvd; /* Bytes received, including headers. */
	uint32		stat_tuple_bytes_recvd; /* Bytes of pure tuple-data received. */
	
	uint32		stat_total_chunkproc_calls;		/* Calls to
		* processIncomingChunks() */
	
}	MotionLayerState;

#endif   /* CDBINTERCONNECT_H */

