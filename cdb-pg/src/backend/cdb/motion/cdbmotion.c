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

#include "miscadmin.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "cdb/tupser.h"
#include "libpq/pqformat.h"
#include "utils/hsearch.h"
#include "utils/tuplestore.h"


/*=========================================================================
 * MOTION NODE INFO DATA STRUCTURES
 */


/* Keys in the motion-node info HTAB are motion node IDs. */

/* This is the entry data-structure for a motion node. */
typedef struct MotionNodeEntry
{
    /* First value in entry has to be the key value.  The key is the motion
     * node ID.
     */
    uint16 motion_node_id;

    /* Flag specifying whether the order of tuples from each source should be
     * maintained or preserved.
     */
    bool preserve_order;

    /* The description of tuples that this motion node will be exchanging. */
    TupleDesc tuple_desc;
    
    /* If preserve_order is false, this tuple-store is used to hold completed
     * tuples that have not yet been consumed.  If preserve_order is true,
     * this is NULL.
     */
    Tuplestorestate *ready_tuples;

} MotionNodeEntry;


/*=========================================================================
 * CHUNK SORTER DATA STRUCTURES
 */


/* This is the key for the chunk-sorter.  Incoming chunks are grouped by
 * source segment-index and destination motion-node.
 */
typedef struct ChunkSorterKey
{
    uint16 source_seg_idx;
    uint16 motion_node_id;
} ChunkSorterKey;


/* This structure is used to keep track of partially completed tuples, and
 * tuples that have been completed but have not been consumed by the executor
 * yet.
 */
typedef struct ChunkSorterEntry
{
    /* The key has to be the first part of the value for the hash table. */
    ChunkSorterKey key;
    
    /* A tuple-chunk list containing the chunks for the currently incomplete
     * HeapTuple being received.
     */
    TupleChunkListData chunk_list;
    
    /* A tuple-store to hold the tuples that have been completed but not yet
     * retrieved.  This will not be initialized until it is actually needed.
     */
    Tuplestorestate *ready_tuples;
    
} ChunkSorterEntry;

/*=========================================================================
 * MOTION LAYER DATA STRUCTURE AND GLOBAL STATE VARIABLE
 */

typedef struct MotionLayerState
{
    /* Map to store information about each motion node in a plan. */
    HTAB *motionnode_map;

    /* Map to store incomplete tuples (and completed ones, when
     * order_preserving == true), associated with the sender and intended
     * destination motion-node.
     */
    HTAB *chunksort_map;
    
} MotionLayerState;


static MotionLayerState s_motion_layer_state;


/*=========================================================================
 * FUNCTION DEFINITIONS
 */


MotionNodeEntry *getMotionNodeEntry(uint16 motNodeID);
ChunkSorterEntry *getChunkSorterEntry(MotionNodeEntry *motNodeEntry,
    uint16 srcSegIdx);
bool addChunkToSorter(TupleChunkListItem tcItem);





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
            mn_info->tuple_desc, Cdb_max_tuple_chunk_size, mn_info,
            send_ctxt->target_segment);
        
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


void InitMotionLayer(void)
{
    HASHCTL hctl;
    HTAB *htab;

    /* Initialize motion-node hashtable.  The initial number of bins is chosen
     * to be relatively small, since the max number of bins is just the number
     * of motion nodes in the plan.
     */
    
    hctl.keysize = sizeof(uint16);
    hctl.entrysize = sizeof(MotionNodeEntry);
    hctl.hash = tag_hash;
    htab = hash_create("MotionNodeInfo", 10, &hctl, HASH_ELEM | HASH_FUNCTION);
    if (htab == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
            errmsg("Could not allocate hash table for motion node info.")));
    }
    
    s_motion_layer_state.motionnode_map = htab;
    
    /* Initialize tuple-data hashtable.  The initial number of bins is chosen
     * to be relatively small, since the max number of bins is just the number
     * of segment databases multiplied by the number of motion nodes in the
     * plan.
     */

    hctl.keysize = sizeof(ChunkSorterKey);
    hctl.entrysize = sizeof(ChunkSorterEntry);
    hctl.hash = tag_hash;
    htab = hash_create("ChunkSorter", 10, &hctl, HASH_ELEM | HASH_FUNCTION);
    if (htab == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
            errmsg("Could not allocate hash table for tuple chunk sorter.")));
    }

    s_motion_layer_state.chunksort_map = htab;
}


/* Initialize a single motion node.  This is called by the executor when a
 * motion node in the plan tree is being initialized.
 *
 */
void InitMotionNode(uint16 motNodeID, bool preserveOrder, TupleDesc tupDesc)
{
    MotionNodeEntry *pEntry;
    bool found;

    AssertArg(tupDesc != NULL);

    elog(DEBUG2, "Initializing entry for motion node %d in Motion Layer.",
        motNodeID);

    /* Create a new MotionNodeEntry.  If one already exists, complain! */

    pEntry = (MotionNodeEntry *) hash_search(
        s_motion_layer_state.motionnode_map, &motNodeID, HASH_ENTER, &found);

    if (pEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
            errmsg("Could not allocate HTAB entry for motion node %d.",
            motNodeID)));
    }

    if (found)
    {
        /* An entry for this motion node ID already existed.  Initializing
         * twice for a given motion node is not allowed.  Having two nodes
         * with the same ID is also not allowed.  So, this is pretty likely a
         * serious problem.
         */
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("A HTAB entry for motion node %d already exists.",
            motNodeID)));
    }

    /* Finish up initialization of the motion node entry. */

    Assert(pEntry->motion_node_id == motNodeID);
    
    pEntry->preserve_order = preserveOrder;
    pEntry->tuple_desc = tupDesc;
    
    if (!preserveOrder)
    {
        /* Create a tuple-store for the motion node's incoming tuples. */
        
        elog(DEBUG3, "Motion node %d is not order-preserving.  Creating"
            " node-level tuple-store.", motNodeID);

        pEntry->ready_tuples = tuplestore_begin_heap(/*randomAccess*/ false,
            /*interXact*/ false, work_mem);
    }
    else
    {
        elog(DEBUG3, "Motion node %d is order-preserving.  No node-level"
            " tuple-store.", motNodeID);

        pEntry->ready_tuples = NULL;
    }
}


/* Helper function to get the motion node entry for a given ID.  NULL is
 * returned if the ID is unrecognized.
 */
MotionNodeEntry *getMotionNodeEntry(uint16 motNodeID)
{
    return (MotionNodeEntry *) hash_search(
        s_motion_layer_state.motionnode_map, &motNodeID, HASH_FIND, NULL);
}


/* Retrieve the chunk-sorter entry for the specified motion-node/source pair.
 * If one doesn't exist, it is created and initialized.
 */
ChunkSorterEntry *getChunkSorterEntry(MotionNodeEntry *motNodeEntry,
    uint16 srcSegIdx)
{
    uint16 motNodeID;
    ChunkSorterKey csKey;
    ChunkSorterEntry *chunkSorterEntry;
    bool found;
    
    AssertArg(motNodeEntry != NULL);

    motNodeID = motNodeEntry->motion_node_id;
    
    bzero(&csKey, sizeof(csKey));
    csKey.source_seg_idx = srcSegIdx;
    csKey.motion_node_id = motNodeID;

    chunkSorterEntry = (ChunkSorterEntry *) hash_search(
        s_motion_layer_state.chunksort_map, &csKey, HASH_ENTER, &found);

    if (chunkSorterEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
            errmsg("Could not allocate htab entry for tuple chunk sorter.")));
    }

    if (!found)
    {
        /* Complete the initialization of the new entry (hash_search
         * initializes the key).
         */

        elog(DEBUG3, "Tuple chunk sorter allocated new HTAB entry for"
            " [src=%d,mn=%d].", srcSegIdx, motNodeID);

        chunkSorterEntry->chunk_list.p_first = NULL;
        chunkSorterEntry->chunk_list.p_last = NULL;

        /* If motion node is not order-preserving, then all chunk-sorter
         * entries share one global tuple-store.  If motion node is order-
         * preserving then there is a tuple-store per sender per motion node.
         */
        if (motNodeEntry->preserve_order)
        {
            elog(DEBUG3, "Motion node %d is order-preserving.  Creating"
                " tuple-store for entry [src=%d,mn=%d].", motNodeID,
                srcSegIdx, motNodeID);

            chunkSorterEntry->ready_tuples = tuplestore_begin_heap(
                /*randomAccess*/ false, /*interXact*/ false, work_mem);
            
            /* Sanity-check: */
            Assert(motNodeEntry->ready_tuples == NULL);
        }
        else
        {
            elog(DEBUG3, "Motion node %d is not order-preserving.  Using"
                " shared tuple-store for entry [src=%d,mn=%d].", motNodeID,
                srcSegIdx, motNodeID);

            chunkSorterEntry->ready_tuples = motNodeEntry->ready_tuples;
            
            /* Sanity-check: */
            Assert(motNodeEntry->ready_tuples != NULL);
        }
    }
    
    Assert(chunkSorterEntry != NULL);
    return chunkSorterEntry;
}


/* Add another tuple-chunk to the chunk sorter.  If the new chunk completes
 * another HeapTuple, that tuple will be deserialized and stored into a
 * tuple-store.   If not, the chunk is added to the appropriate list of
 * chunks.
 *
 * Return Values:
 *   true  - if another HeapTuple is completed by this chunk.
 *   false - if the chunk does not complete a HeapTuple.
 */
bool addChunkToSorter(TupleChunkListItem tcItem)
{
    uint16 srcSegIdx, motNodeID;
    ChunkSorterEntry *chunkSorterEntry;
    MotionNodeEntry *motNodeEntry;

    bool tupleCompleted = false;
    TupleChunkType tcType;
    HeapTuple htup;

    AssertArg(tcItem != NULL);

    srcSegIdx = GetSourceSegIdx(tcItem->chunk_data);
    motNodeID = GetMotionNodeID(tcItem->chunk_data);
    
    /* Pull up the motion node entry for this item.  If one doesn't exist,
     * something is SERIOUSLY WRONG.
     */
    motNodeEntry = getMotionNodeEntry(motNodeID);
    if (motNodeEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("Received tuple-chunk for unrecognized"
            " motion-node ID %d.", motNodeID)));
    }

    chunkSorterEntry = getChunkSorterEntry(motNodeEntry, srcSegIdx);

    /* Look at the chunk's type, to figure out what to do with it. */

    tcType = GetChunkType(tcItem->chunk_data);
    
    switch (tcType)
    {
    case TC_WHOLE:
        
        /* There shouldn't be any partial tuple data in the list! */
        if (chunkSorterEntry->chunk_list.num_chunks != 0)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received TC_WHOLE chunk from [src=%d,mn=%d] after"
                    " partial tuple data.", srcSegIdx, motNodeID)));
        }
        
        /* Put this chunk into the list. */
        appendChunkToTCList(&chunkSorterEntry->chunk_list, tcItem);
        
        /* Process the list into a new tuple, then store the tuple. */

        htup = CvtChunksToHeapTup(&chunkSorterEntry->chunk_list,
            motNodeEntry->tuple_desc);

        tuplestore_puttuple(chunkSorterEntry->ready_tuples, htup);

        tupleCompleted = true;

        /* Clean up the chunk-list to receive a new tuple. */
        clearTCList(&chunkSorterEntry->chunk_list);
        
        break;
        
    case TC_PARTIAL_START:
        
        /* There shouldn't be any partial tuple data in the list! */
        if (chunkSorterEntry->chunk_list.num_chunks != 0)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received TC_PARTIAL_START chunk from [src=%d,mn=%d]"
                    " after partial tuple data.", srcSegIdx, motNodeID)));
        }

        /* Put this chunk into the list. */
        appendChunkToTCList(&chunkSorterEntry->chunk_list, tcItem);

        break;

    case TC_PARTIAL_MID:

        /* There should be partial tuple data in the list. */
        if (chunkSorterEntry->chunk_list.num_chunks <= 0)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received TC_PARTIAL_MID chunk from [src=%d,mn=%d]"
                    " without any leading tuple data.", srcSegIdx, motNodeID)));
        }

        /* Append this chunk to the list. */
        appendChunkToTCList(&chunkSorterEntry->chunk_list, tcItem);
        break;

    case TC_PARTIAL_END:

        /* There should be partial tuple data in the list. */
        if (chunkSorterEntry->chunk_list.num_chunks <= 0)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received TC_PARTIAL_END chunk from [src=%d,mn=%d]"
                    " without any leading tuple data.", srcSegIdx, motNodeID)));
        }

        /* Append this chunk to the list. */
        appendChunkToTCList(&chunkSorterEntry->chunk_list, tcItem);

        /* Process the list into a new tuple, then store the tuple. */

        htup = CvtChunksToHeapTup(&chunkSorterEntry->chunk_list,
            motNodeEntry->tuple_desc);

        tuplestore_puttuple(chunkSorterEntry->ready_tuples, htup);

        tupleCompleted = true;

        /* Clean up the chunk-list to receive a new tuple. */
        clearTCList(&chunkSorterEntry->chunk_list);

        break;

    case TC_END_OF_STREAM:

        /* There shouldn't be any partial tuple data in the list! */
        if (chunkSorterEntry->chunk_list.num_chunks != 0)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received TC_END_OF_STREAM chunk from [src=%d,mn=%d]"
                    " after partial tuple data.", srcSegIdx, motNodeID)));
        }
        
        /* TODO:  Mark the state as "end of stream." */
        
        break;
        
    default:
        ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("Received tuple chunk of unrecognized type %d"
                " from [src=%d,mn=%d].", tcType, srcSegIdx, motNodeID)));
    }
    
    return tupleCompleted;
}

