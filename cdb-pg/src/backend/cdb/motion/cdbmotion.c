/*-------------------------------------------------------------------------
 *
 * cdbmotion.c
 * 		Access into the motion-layer in order to send and receive tuples
 *	  		within a motion node ID within a particular process group id. 	
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *
 * IDENTIFICATION
 *        $Id$
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "libpq/pqformat.h"


/*
 * Function:  SendTuple - Sends a portion or whole tuple to the AMS layer.
 *
 */
int 
SendTuple( MotionNodeInfo *mn_info, TupleSendContext *send_ctxt )
{
    bool chunk_sent = false;
    bool done_sending = false;
    bool tuple_sent = false;
    TupleChunkList tcList;
    TupleChunkListItem tuple_chunk;
    
    AssertArg(mn_info != NULL);
    
    AssertArg(send_ctxt != NULL);
    AssertArg(send_ctxt->total_tuple_bytes > 0);
    AssertArg(send_ctxt->total_bytes_sent < send_ctxt->total_tuple_bytes); /* fail means already sent */
    AssertArg(send_ctxt->last_bytes_written >= 0);
    AssertArg(send_ctxt->last_bytes_written <= send_ctxt->total_bytes_sent);
    
    /* On the first call, we need to turn the Heapgenerate a serialized version of the
     * HeapTuple.  This gets stored into the TupleSendContext, but freed on
     * the last call required to send the tuple.  (For tuples that require a
     * single send call, it is freed right away; for tuples that require
     * multiple send calls, it is freed when it is no longer needed.)
     */
    if (send_ctxt->total_bytes_sent == 0 &&
        send_ctxt->tuple_chunk_list == NULL)
    {
        /* Create and store the serialized form, and some stats about it. */
        
        tcList = CvtHeapTupToChunks(send_ctxt->heap_tuple,
            mn_info->tuple_desc, Cdb_max_tuple_chunk_size,
            mn_info->segment_id, send_ctxt->target_segment,
            mn_info->motion_node_id);
        
        send_ctxt->tuple_chunk_list = tcList;
        send_ctxt->next_tuple_chunk = tcList->p_first;
        
        send_ctxt->total_tuple_bytes = tcList->serialized_data_length;
        send_ctxt->total_bytes_sent = 0;
    }
    
    send_ctxt->last_bytes_written = 0;
    
    /* Loop on sending tuple chunks until either the whole tuple is sent, or
     * we can't send the current chunk.  (If we get stopped in the middle, the
     * state is stored in the TupleSendContext, so that this function can be
     * called again later.)
     */
    
    tuple_sent = false;
    done_sending = false;
    do
    {
        tuple_chunk = send_ctxt->next_tuple_chunk;
        Assert(tuple_chunk != NULL);
        
        /* Push the tuple chunk to the AMS using some IPC mechanism (we don't
         * care how the data gets there).
         */
        chunk_sent = SendTupleChunkToAMS(tuple_chunk);
        if (!chunk_sent)
        {
            /* Update the send-context byte-counts. */
            send_ctxt->last_bytes_written = 0;

            /* We can't do anymore, so return to the caller. */
            done_sending = true;
        }
        else /* chunk was sent */
        {
            /* Update the send-context info. */
            send_ctxt->total_bytes_sent += tuple_chunk->chunk_length;
            send_ctxt->last_bytes_written += tuple_chunk->chunk_length;

            Assert(send_ctxt->tuple_bytes_sent > 0);
            Assert(send_ctxt->tuple_bytes_sent <= send_ctxt->total_tuple_bytes);

            Assert(send_ctxt->total_bytes_sent > 0);

            Assert(send_ctxt->last_bytes_written > 0);
            
            /* Get out the next chunk to send.  If the whole tuple has been
             * sent, exit the loop.
             */
            send_ctxt->next_tuple_chunk = tuple_chunk->p_next;
            if (send_ctxt->next_tuple_chunk == NULL)
            {
                /* Sanity check:  Should have sent all the tuple data... */
                Assert(send_ctxt->tuple_bytes_sent ==
                       send_ctxt->total_tuple_bytes);

                tuple_sent = true;
                done_sending = true;
            }
        }
    } while (!done_sending);
    
    return (tuple_sent ? SEND_COMPLETE : SEND_INCOMPLETE);
}


/*
 * Function: SendEndOfStream - Signals other peer Motion Nodes that this motion
 *			       node has no more tuples to send out.	
 * 			
 */
int  
SendEndOfStream( MotionNodeInfo *mn_info ) 
{
	return 0; 
}
 
 
/*
 * Function:
 *
 */
int 
RecvTuple( MotionNodeInfo *mn_info, HeapTuple *tup_i )
{
	return 0;

}
 
 
/*
 * Function:  CanReadTuple - blocks until a portion or whole tuple is ready to
 *			     to be read.
 *
 */
void 
CanReadTuple( )
{
	//TODO: eventually do some clever trick to monitor however the motion
	//	layer communicates with the AMS layer.  We want to block here
	//	until there is a portion of a tuple to be received. this will
	//	allow motion nodes that have already sent out their tuples
	//	to block until something interesting happens instead of having
	//	to poll or spin.
	// 
	
	//for now... we will do nothing.  :)

}
