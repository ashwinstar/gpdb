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



/* API FUNCTION CALLS */


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
 *
 * RETURN: the return code is one of the following:
 *
 *		GOT_TUPLE - A tuple was received, and it data is stored in tup_in.
 *
 *		NO_TUPLE - No tuple was available, but more are still expected.
 *
 *		END_OF_STREAM - No tuple was received, and no more are expected.
 */
extern ReceiveReturnCode RecvTuple(int16 motNodeID, HeapTuple *tup_i);
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
