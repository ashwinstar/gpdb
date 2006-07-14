/*-------------------------------------------------------------------------
 *
 * cdbmotion.h
 *
 *
 *
 * Copyright (c) 2004, Greenplum
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBMOTION_H
#define CDBMOTION_H

#include "access/htup.h"
#include "cdb/htupfifo.h"
#include "cdb/tupser.h"
#include "cdb/tupchunk.h"
#include "cdb/tupchunklist.h"
#include "nodes/primnodes.h"
#include "nodes/execnodes.h" //SliceTable



/* Define this if you want tons of logs! */
#undef AMS_VERBOSE_LOGGING


typedef enum SendReturnCode
{
	SEND_COMPLETE,
	STOP_SENDING
}	SendReturnCode;


typedef enum ReceiveReturnCode
{
	GOT_TUPLE,
	NO_TUPLE,
	END_OF_STREAM
}	ReceiveReturnCode;


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




/* API FUNCTION CALLS */

extern MotionNodeEntry *getMotionNodeEntry(int16 motNodeID, char *errString);

/* One-time initialization of motion layer. */
extern void InitMotionLayer(void);
extern void CleanUpMotionLayer(void);

/* Initialization of each motion node in execution plan. */
extern void InitMotionLayerNode(int16 motNodeID, bool preserveOrder,
					TupleDesc tupDesc);

/* Cleanup of each motion node in execution plan (normal termination). */
extern void EndMotionLayerNode(int16 motNodeID, bool flushCommLayer);

/* Reset the Motion Layer's state between query executions (normal termination
 * or error-cleanup). */
extern void ResetMotionLayer(bool flushCommLayer);


/* non-blocking operation that may perform only part (or none) of the
 * send before returning.  The TupleSendContext is used to help keep track
 * of the send operations status.  The caller of SendTuple() is responsible
 * for continuing to call SendTuple() until the entire tuple has been sent.
 *
 * Failing to do so and calling SendTuple() with a new Tuple before the old
 * one has been sent is a VERY BAD THING to do and will surely cause bad
 * failures.
 *
 * PARAMETERS:
 *	 - mn_info:  Motion node this tuple is being sent from.
 *
 *	 - tupCtxt: tuple data to send, and state of send operation.
 *
 * RETURN:	return codes to indicate result of send. Possible values are:
 *
 *		SEND_COMPLETE - The entire tuple was accepted by the AMS.
 *				Note that the tuple data may still be in the
 *				AMS send-buffers, but as far as the motion node
 *				is concerned the send is done.
 *
 *		STOP_SENDING - Receiver no longer wants to receive from us.
 */
extern SendReturnCode SendTuple(int16 motNodeID, HeapTuple tuple, int16 targetRoute);


/* Send or broadcast an END_OF_STREAM token to the corresponding motion-node
 * on other segments.
 *
 * The send operation is nonblocking, and will return SEND_INCOMPLETE if the
 * send would block.  When this value is returned, the caller must try again
 * until the SEND_COMPLETE value is returned.
 *
 * RETURN: Possible return values are:
 *
 *		SEND_COMPLETE - The token was accepted by the AMS.	Note that the
 *				token may still be in the AMS send-buffers, but as far as the
 *				motion node is concerned, the send is done.
 *
 *		SEND_INCOMPLETE - The END_OF_STREAM token could not be sent.
 *				  This generally means that the send-buffers
 *				  were full and the operation should be tried
 *				  later.  (Real errors are signalled via ereport().)
 */
extern SendReturnCode SendEndOfStream(int16 motNodeID, int16 targetRoute);


/* Receive a tuple from the corresponding motion-node on any query-executor
 * in the process-group.  This function returns immediately without blocking;
 *
 * To get an result for unordered receive (we used to provide a separate
 * RecvTuple() function, set the srcRoute to ANY_ROUTE
 *
 * RETURN: the return code is one of the following:
 *
 *		GOT_TUPLE - A tuple was received, and it data is stored in tup_in.
 *
 *		NO_TUPLE - No tuple was available, but more are still expected.
 *
 *		END_OF_STREAM - No tuple was received, and no more are expected.
 */
extern ReceiveReturnCode RecvTupleFrom(int16 motNodeID, HeapTuple *tup_i,
									   int16 srcRoute);

extern void SendStopMessage(int16 motNodeID);

/* used by ml_ipc to set the number of receivers that the motion node is expecting.
 * This is used by cdbmotion to keep track of when its seen enough EndOfStream
 * messages.
 */
extern void setExpectedReceivers(int16 motNodeID, int expectedReceivers);

/*
 * Used by ml_ipc to tell the intermediate layer that we were stopped
 * explicitly, so we should freak out about not being finished.
 */
extern void setNodeStopped(int16 motNodeID);

#endif   /* CDBMOTION_H */
