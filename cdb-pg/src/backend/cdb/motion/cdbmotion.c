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
#include "cdb/cdblink.h"   //used for the getCdbIdentity() call.
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "cdb/ml_ipc.h"
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
    
    /* Flag specifying whether sends should go to all seg-dbs for a particular
     * segment-index.  This is specified at config-time and automatically
     * applied during sends.
     */
    bool include_backup_segdb;

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

    /* Variable that records the total number of remote senders to this motion
     * node.  This will usually be (number of seg-dbs) - 1.
     */
    uint num_remote_senders;

    /* Variable that tracks number of senders that have reported end-of-stream
     * for this motion node.
     */
    uint num_stream_ends_recvd;

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
    
    /* Flag recording whether end-of-stream has been reported from the source
     * segment-database.
     */
    bool end_of_stream;

} ChunkSorterEntry;

/*=========================================================================
 * MOTION LAYER DATA STRUCTURE
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


/*=========================================================================
 * STATIC STATE VARS
 */


static MotionLayerState s_motion_layer_state;

static TupleChunkListItemData s_eos_chunk_data =
    { NULL, TUPLE_CHUNK_HEADER_SIZE, "                " };

static int mySegIndex;
static bool isPrimary;

/*=========================================================================
 * FUNCTION DEFINITIONS
 */


MotionNodeEntry *getMotionNodeEntry(uint16 motNodeID);
ChunkSorterEntry *getChunkSorterEntry(MotionNodeEntry *motNodeEntry,
    uint16 srcSegIdx);
bool addChunkToSorter(TupleChunkListItem tcItem);

void processIncomingChunks(void);
SendReturnCode sendStreamEndChunk(MotionNodeInfo *mn_info,
    TupleChunkType tcType);



/*
 * Function:  SendTuple - Sends a portion or whole tuple to the AMS layer.
 *
 */
SendReturnCode SendTuple(MotionNodeInfo *mn_info, TupleSendContext *send_ctxt)
{
    MotionNodeEntry *pMNEntry;
    uint16 targetSegIdx;
    
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

    /* The target segment-index should be an "active-only" segment-index.  We
     * add in the "include-backup-segdb" part ourselves.
     */
    AssertArg(IsActiveOnlySegIdx(send_ctxt->target_segment));

    /* Pull up the motion node entry with the node's details.  This includes
     * details that affect sending, such as whether the motion node needs to
     * include backup segment-dbs.
     */
    pMNEntry = getMotionNodeEntry(mn_info->motion_node_id);
    if (pMNEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("SendTuple called for unrecognized motion-node ID %d.",
            mn_info->motion_node_id)));
    }

    /* On the first call, we need to generate a serialized version of the
     * HeapTuple.  This gets stored into the TupleSendContext, but freed on
     * the last call required to send the tuple.  (For tuples that require a
     * single send call, it is freed right away; for tuples that require
     * multiple send calls, it is freed when it is no longer needed.)
     */
    if (send_ctxt->total_bytes_sent == 0 &&
        send_ctxt->tuple_chunk_list == NULL)
    {
        /* Set up the target segment index based on the motion node info. */

        targetSegIdx = send_ctxt->target_segment;
        if (pMNEntry->include_backup_segdb)
            targetSegIdx = SetAllSegdbsFlag(targetSegIdx);

        /* Create and store the serialized form, and some stats about it. */

        tcList = CvtHeapTupToChunks(send_ctxt->heap_tuple,
            pMNEntry->tuple_desc, Cdb_max_tuple_chunk_size, mn_info,
            targetSegIdx);

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


/* Signals other peer Motion Nodes that this motion node has no more tuples to
 * send out.  The send operation is nonblocking, and will return
 * SEND_INCOMPLETE if the send would block.  When this value is returned, the
 * caller must try again until the SEND_COMPLETE value is returned.
 */
SendReturnCode SendEndOfStream(MotionNodeInfo *mn_info)
{
    return sendStreamEndChunk(mn_info, TC_END_OF_STREAM);
}


/* Signals other peer Motion Nodes that this motion node has terminated its
 * data stream due to an error condition.  This can be done mid-send, so there
 * may be partial tuple data at the receivers.
 *
 * The send operation is nonblocking, and will return SEND_INCOMPLETE if the
 * send would block.  When this value is returned, the caller must try again
 * until the SEND_COMPLETE value is returned.
 */
SendReturnCode SendAbortStream(MotionNodeInfo *mn_info)
{
    return sendStreamEndChunk(mn_info, TC_ABORT_STREAM);
}


/* Helper function that actually sends the stream-end tuple-chunk. */
SendReturnCode sendStreamEndChunk(MotionNodeInfo *mn_info,
    TupleChunkType tcType)
{
    bool chunk_sent;
    uint8 *pData;

    AssertArg(tcType == TC_END_OF_STREAM || tcType == TC_ABORT_STREAM);
    
    /* Use the statically allocated chunk that is intended for sending end-of-
     * stream messages so that we don't incur allocation and deallocation
     * overheads.
     */

    Assert(s_eos_chunk_data.p_next == NULL);
    Assert(s_eos_chunk_data.chunk_length == TUPLE_CHUNK_HEADER_SIZE);

    pData = s_eos_chunk_data.chunk_data;

    SetTargetSegIdx(pData, BROADCAST_SEGIDX);
    SetQueryGroupID(pData, mn_info->process_group_id);
    SetSourceSegIdx(pData, mn_info->segment_id);
    SetMotionNodeID(pData, mn_info->motion_node_id);
    SetChunkDataSize(pData, 0);
    SetChunkType(pData, TC_ABORT_STREAM);

    chunk_sent = SendTupleChunkToAMS(&s_eos_chunk_data);

    return (chunk_sent ? SEND_COMPLETE : SEND_INCOMPLETE);
}


/*
 * Function:
 *
 */
ReceiveReturnCode RecvTuple(MotionNodeInfo *mn_info, HeapTuple *tup_i)
{
    MotionNodeEntry *pMNEntry;
    Tuplestorestate *tupStoreState;

    /* Process all chunks in our incoming queue, regardless of which motion
     * nodes they are coming from.
     */
    processIncomingChunks();

    /* Get the tuple-store that should contain heap-tuples that we want. */

    pMNEntry = getMotionNodeEntry(mn_info->motion_node_id);

    if (pMNEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("RecvTuple called for unrecognized motion-node ID %d.",
            mn_info->motion_node_id)));
    }

    if (pMNEntry->preserve_order)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("RecvTuple called for order-preserving"
                " motion-node ID %d.", mn_info->motion_node_id)));
    }

    tupStoreState = pMNEntry->ready_tuples;

    /* Get the next HeapTuple, if one is available! */
    *tup_i = getNextTupleFromStore(tupStoreState);
    
    if (*tup_i != NULL)
    {
        /* We got a tuple. */
        return GOT_TUPLE;
    }
    else if (pMNEntry->num_stream_ends_recvd == pMNEntry->num_remote_senders)
    {
        /* No tuple was available (tuple-store was at EOF), and end-of-stream
         * has been marked.  No more tuples are going to show up.
         */
        return END_OF_STREAM;
    }
    else
    {
        /* No tuple was available (tuple-store was at EOF), but we have not
         * been sent an end-of-stream.  Tell the caller that there are no
         * tuples right now.
         */
        return NO_TUPLE;
    }
}


ReceiveReturnCode RecvTupleFrom(MotionNodeInfo *mn_info, HeapTuple *tup_i,
    uint16 srcSegIdx)
{
    MotionNodeEntry *pMNEntry;
    ChunkSorterEntry *pCSEntry;
    Tuplestorestate *tupStoreState;

    /* Process all chunks in our incoming queue, regardless of which motion
     * nodes they are coming from.
     */
    processIncomingChunks();

    /* Get the tuple-store that should contain heap-tuples that we want. */

    pMNEntry = getMotionNodeEntry(mn_info->motion_node_id);

    if (pMNEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("RecvTuple called for unrecognized motion-node ID %d.",
            mn_info->motion_node_id)));
    }

    if (!pMNEntry->preserve_order)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("RecvTupleFrom called for non order-preserving"
                " motion-node ID %d.", mn_info->motion_node_id)));
    }

    /* Pull up the chunk-sorter entry for the specified sender, and get the
     * tuple-store we should use.
     */
    pCSEntry = getChunkSorterEntry(pMNEntry, srcSegIdx);
    tupStoreState = pCSEntry->ready_tuples;

    /* Get the next HeapTuple, if one is available! */
    *tup_i = getNextTupleFromStore(tupStoreState);

    if (*tup_i != NULL)
    {
        /* We got a tuple. */
        return GOT_TUPLE;
    }
    else if (pCSEntry->end_of_stream)
    {
        /* No tuple was available (tuple-store was at EOF), and end-of-stream
         * has been marked.  No more tuples are going to show up.
         */
        return END_OF_STREAM;
    }
    else
    {
        /* No tuple was available (tuple-store was at EOF), but we have not
         * been sent an end-of-stream.  Tell the caller that there are no
         * tuples right now.
         */
        return NO_TUPLE;
    }
}


/* This helper function is the receive-tuple workhorse.  It pulls tuple chunks
 * from the AMS, and pushes them to the chunk-sorter where they can be sorted
 * based on sender, and reconstituted into whole HeapTuples.  Functions like
 * RecvTuple() and RecvTupleFrom() should definitely call this before trying
 * to get a HeapTuple to return.  It can also be called during other
 * operations if that seems like a good idea.  For example, it can be called
 * sometime during send-tuple operations as well.
 */
void processIncomingChunks(void)
{
    TupleChunkListItem tcItem, tcNext;
    uint16 motNodeID, srcSegIdx;

    /* Get all of the currently available tuple-chunks, and push each one into
     * the chunk-sorter.
     */

    tcItem = RecvTupleChunkFromAMS();
    while (tcItem != NULL)
    {
        /* Detach the current chunk off of the front of the list. */
        tcNext = tcItem->p_next;
        tcItem->p_next = NULL;

        /* Pull out the motion node ID and the sender segment-database ID. */
        motNodeID = GetMotionNodeID(tcItem->chunk_data);
        srcSegIdx = GetSourceSegIdx(tcItem->chunk_data);

        elog(DEBUG3, "Processing tuple-chunk received from source segment %d"
            " for motion-node %d.", srcSegIdx, motNodeID);

        /* Stick the chunk into the sorter. */
        addChunkToSorter(tcItem);
        
        tcItem = tcNext;
    }
}


/* This helper function pulls the next HeapTuple from a tuple-store, working
 * around the fact that when the tuple-store read-position hits EOF, it stays
 * at EOF when new tuples are written to the store.  We don't want that
 * behavior, so we have to use the mark-position/restore-position
 * functionality to work around it.
 *
 * If no tuple is available, NULL is returned.
 */
HeapTuple getNextTupleFromStore(Tuplestorestate *tupStoreState)
{
    bool shouldFree;
    HeapTuple htup;
    
    AssertArg(tupStoreState != NULL);
    
    /* If we are already at EOF, set the read-position to where EOF was
     * previously encountered.  This is necessary since the read-position
     * tracks the write-position once EOF occurs in the tuple-store.
     */
    if (tuplestore_ateof(tupStoreState))
        tuplestore_restorepos(tupStoreState);

    /* Get the next HeapTuple, if one is available! */
    htup = tuplestore_getheaptuple(tupStoreState, true, &shouldFree);

    /* I don't know what we should do about the value of should-free. */
    if (htup != NULL)
    {
        elog(DEBUG3, "In getNextTupleFromStore(), shouldFree is %s.",
            (shouldFree ? "true" : "false"));
    }
    
    /* If the read-position ends up at EOF, mark the position so that we can
     * restore it next time we read.  This is because the read-position tracks
     * with the write-position when EOF occurs in the tuple-store.
     */
    if (tuplestore_ateof(tupStoreState))
        tuplestore_markpos(tupStoreState);
    
    return htup;
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


/* This function performs one-time initialization of the Motion Layer.  This
 * should be called at "postgres" process initialization time.
 */
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
        ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
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
        ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
            errmsg("Could not allocate hash table for tuple chunk sorter.")));
    }

    s_motion_layer_state.chunksort_map = htab;
	
	
	/* Initialize the Motion Layer IPC subsystem. */
	getCdbIdentity( &mySegIndex, &isPrimary );	
	InitMotionLayerIPC( mySegIndex );
	
}


/* Initialize a single motion node.  This is called by the executor when a
 * motion node in the plan tree is being initialized.
 *
 * This function is called from:  ExecInitMotion()
 */
void InitMotionLayerNode(uint16 motNodeID, bool includeBackupSegDB,
    bool preserveOrder, TupleDesc tupDesc)
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
    
    pEntry->include_backup_segdb = includeBackupSegDB;
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


/* This function completely resets the Motion Layer at the end of a query
 * execution.  It iterates through each entry in the motion-node and
 * chunk-sorter tables, and performs necessary cleanup of the data-structures.
 */
void ResetMotionLayer(bool flushCommLayer)
{
    
    /*hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp)*/
}


void EndMotionLayerNode(uint16 motNodeID, bool flushCommLayer)
{
    HASH_SEQ_STATUS seqStatus;
    ChunkSorterEntry *pCSEntry;

    /* Iterate through all entries in the motion layer's chunk-sort map, to
     * see if we have gotten end-of-stream from all senders.
     */

    hash_seq_init(&seqStatus, s_motion_layer_state.chunksort_map);

    while ((pCSEntry = (ChunkSorterEntry *) hash_seq_search(&seqStatus)) != NULL)
    {
        /* Skip entries for motion nodes we don't care about. */
        if (pCSEntry->key.motion_node_id != motNodeID)
            continue;
        
        /* If we haven't gotten the end-of-stream yet, handle it according to
         * the caller's request.
         */
        
        if (!(pCSEntry->end_of_stream))
        {
            if (flushCommLayer)
            {
                elog(DEBUG2, "Motion layer node %d cleanup - did not receive"
                    " end-of-stream from sender %d.  Waiting.", motNodeID,
                    pCSEntry->key.source_seg_idx);
                
                /*** TODO - get chunks until end-of-stream comes in. ***/
            }
            else
            {
                elog(WARNING, "Motion layer node %d cleanup - did not receive"
                    " end-of-stream from sender %d.", motNodeID,
                    pCSEntry->key.source_seg_idx);
            }
        }
        else
        {
            /* End-of-stream is marked for this entry. */
            
            /*** TODO - make sure there is no chunk data or outstanding tuple
                 in the tuple-store.
             ***/
        }
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
 *
 * It might not seem obvious why the MotionNodeEntry is required as an
 * argument.  It's in there to ensure that only valid motion nodes are
 * represented in the chunk-sorter.  The motion-node's entry is typically
 * retrieved using getMotionNodeEntry() before this function is called.
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
        chunkSorterEntry->end_of_stream = false;

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

    if (IsBroadcastSegIdx(srcSegIdx))
    {
        ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("Source segment index may not be broadcast"
            " segment index.")));
    }
    if (!IsActiveOnlySegIdx(srcSegIdx))
    {
        ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("Source segment index may not set"
            " the \"all seg-dbs\" flag.")));
    }

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
        
        /* Make sure that we haven't already received end-of-stream! */
        
        if (chunkSorterEntry->end_of_stream)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received end-of-stream chunk from [src=%d,mn=%d]"
                    " when already marked as at end-of-stream.",
                    srcSegIdx, motNodeID)));
        }
        
        /* Mark the state as "end of stream." */
        chunkSorterEntry->end_of_stream = true;
        motNodeEntry->num_stream_ends_recvd++;
        
        break;
        
    case TC_ABORT_STREAM:
    
        /* Since this indicates an error condition, there maybe partial tuple
         * data in the list.
         */
        
        elog(DEBUG2, "Received TC_ABORT_STREAM from [src=%d,mn=%d].",
            srcSegIdx, motNodeID);
        
        /* Make sure that we haven't already received end-of-stream! */
        
        if (chunkSorterEntry->end_of_stream)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received end-of-stream chunk from [src=%d,mn=%d]"
                    " when already marked as at end-of-stream.",
                    srcSegIdx, motNodeID)));
        }
        
        /* Mark the state as "end of stream." */
        chunkSorterEntry->end_of_stream = true;
        motNodeEntry->num_stream_ends_recvd++;
        
        break;
        
    default:
        ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("Received tuple chunk of unrecognized type %d"
                " from [src=%d,mn=%d].", tcType, srcSegIdx, motNodeID)));
    }
    
    return tupleCompleted;
}

