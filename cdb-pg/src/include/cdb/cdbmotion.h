/*-------------------------------------------------------------------------
 *
 * cdbmotion.h
 *
 *
 *
 * Copyright (c) 2004, Metapa
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBMOTION_H
#define CDBMOTION_H

#include "access/htup.h"
#include "cdb/tupchunk.h"
#include "cdb/tupchunklist.h"

typedef enum SendReturnCode
{
	SEND_COMPLETE,
	SEND_INCOMPLETE
} SendReturnCode;


typedef enum ReceiveReturnCode
{
	GOT_TUPLE,
	NO_TUPLE,
	END_OF_STREAM
} ReceiveReturnCode;


/* Used to keep track of the motion node.
 *
 * FIELDS:
 *
 *	query_group_id:  When a user connects to the CDB, a postgres process
 *			   will be started for every segment-database in the CDB.
 *			   All of these postgres processes are part of the same 
 *			   group of processes serving that user's connection and
 *			   so all will have the same globally unique 
 *			   "process_group_id" that identifies this group from
 *			   other groups within the CDB.  
 *
 *			   TODO:  This id is assigned by the AMS?
 *
 *	segment_id: The segment id for the local segment database.
 *
 *	host_id: The host id for the local segment database.
 *
 */
typedef struct MotionNodeInfo
{
	int query_group_id;
	int segment_id;
	int host_id;
    
} MotionNodeInfo;


/* Used to keep track of a Tuple that is attempting to be sent.
 *
 * FIELDS:
 *
 *	heap_tuple:  Heap tuple to be sent.
 *
 *	target_segment:  The target destination segment; -1 indicates broadcast.
 *
 *	flags:	indicates if the tuple should be delivered to the active
 *		segment database or to both the active and backup.
 *
 *	total_tuple_bytes: The total number of bytes that the serialized tuple
 *			   will occupy.
 *
 *	total_bytes_sent:  The total number of bytes that have been written
 *			   into the local AMS buffers.  This is NOT the number
 *			   of bytes that have been copied to the network. 
 *
 *	last_bytes_written:   The number of bytes that were written the last
 *			      time the SentTuple() function was called.
 *
 */
typedef struct TupleSendContext
{
	HeapTuple heap_tuple;
	uint16 target_segment;
	unsigned char flags;

	long total_tuple_bytes;
    long tuple_bytes_sent;      /* Bytes of tuple data sent. */
	long total_bytes_sent;      /* Bytes sent, including chunk header info. */
	long last_bytes_written;    /* Bytes sent in the last operation. */
    
    /*
     * The following information is initialized and managed by the Motion
     * Layer.  DON'T MESS WITH IT, or you will make the Motion Layer ANGRY.
     *
     * Data structures referenced below are allocated with palloc().
     */
    
    TupleChunkList tuple_chunk_list;
    TupleChunkListItem next_tuple_chunk;
    
} TupleSendContext;



/* API FUNCTION CALLS */


/* One-time initialization of motion layer. */
extern void InitMotionLayer(void);

/* Initialization of each motion node in execution plan. */
extern void InitMotionLayerNode(uint16 motNodeID, bool includeBackupSegDB,
    bool preserveOrder, TupleDesc tupDesc);

/* Cleanup of each motion node in execution plan (normal termination). */
extern void EndMotionLayerNode(uint16 motNodeID, bool flushCommLayer);

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
 *   - mn_info:  Motion node this tuple is being sent from.
 *
 *   - tupCtxt: tuple data to send, and state of send operation.
 *
 * RETURN:  return codes to indicate result of send. Possible values are:
 *
 *		SEND_COMPLETE - The entire tuple was accepted by the AMS.
 *				Note that the tuple data may still be in the 
 *				AMS send-buffers, but as far as the motion node
 *				is concerned the send is done.
 *
 *		SEND_INCOMPLETE - Not all of the tuple could be sent in this 
 *				  call.  Zero or more bytes may have been sent;
 *				  the TupleSendContext structure will be updated
 *				  with the change in status.
 */
extern SendReturnCode SendTuple(uint16 motNodeID, TupleSendContext *tupCtxt);



/* Send or broadcast an END_OF_STREAM token to the corresponding motion-node
 * on other segments.
 *
 * The send operation is nonblocking, and will return SEND_INCOMPLETE if the
 * send would block.  When this value is returned, the caller must try again
 * until the SEND_COMPLETE value is returned.
 *
 * RETURN: Possible return values are:
 *
 *		SEND_COMPLETE - The token was accepted by the AMS.  Note that the
 *				token may still be in the AMS send-buffers, but as far as the
 *				motion node is concerned, the send is done.
 *
 *		SEND_INCOMPLETE - The END_OF_STREAM token could not be sent.
 *				  This generally means that the send-buffers 
 *				  were full and the operation should be tried
 *				  later.  (Real errors are signalled via ereport().)
 */
extern SendReturnCode SendEndOfStream(uint16 motNodeID, uint16 target_segment);


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
extern ReceiveReturnCode RecvTuple(uint16 motNodeID, HeapTuple *tup_i);
extern ReceiveReturnCode RecvTupleFrom(uint16 motNodeID, HeapTuple *tup_i,
                                       uint16 srcSegIdx);


/* Proposed API call.  Would block until there was something to be received
 * with RecvTuple().  This function could ONLY EVER be called 
 * after its own local tuples had all been sent out. 
 *
 * It would prevent the local motion node from having to spin or poll.
 * 
 * we can probably skip implementing this initially... but i think its a good
 * thing that we should ultimately.
 */
extern void CanReadTuple(void);		    


#endif /*CDBMOTION_H */
