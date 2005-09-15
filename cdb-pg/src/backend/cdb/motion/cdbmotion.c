/*-------------------------------------------------------------------------
 *
 * cdbmotion.c
 *		Access into the motion-layer in order to send and receive tuples
 *		within a motion node ID within a particular process group id.
 *
 * Copyright (c) 2003-2004, Greenplum
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
#include "access/heapam.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "cdb/htupfifo.h"
#include "cdb/ml_ipc.h"
#include "cdb/tupser.h"
#include "libpq/pqformat.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "nodes/execnodes.h" //SliceTable


/*=========================================================================
 * MOTION NODE INFO DATA STRUCTURES
 */
int Cdb_max_tuple_chunk_size;

/* Keys in the motion-node info HTAB are motion node IDs. */

/* This is the entry data-structure for a motion node. */
typedef struct MotionNodeEntry
{
    /* First value in entry has to be the key value.  The key is the motion
     * node ID.
     */
    int16 motion_node_id;
    
    /* Flag specifying whether the order of tuples from each source should be
     * maintained or preserved.
     */
    bool preserve_order;

    /* The description of tuples that this motion node will be exchanging. */
    TupleDesc tuple_desc;

	/* The cached information to perform tuple serialization and
	 * deserialization as quickly as possible.
	 */
	SerTupInfo ser_tup_info;
	
    /* If preserve_order is false, this is used to hold completed tuples that
	 * have not yet been consumed.  If preserve_order is true, this is NULL.
     */
    htup_fifo ready_tuples;

    /* Variable that records the total number of senders to this motion
     * node.  This is expected to always be (number of qExecs).
     */
    uint num_senders;

    /* Variable that tracks number of senders that have reported end-of-stream
     * for this motion node.  When the local node sends end-of-stream, that is
     * also recorded.
     */
    uint num_stream_ends_recvd;
    
    bool cleanedUp;
    
    bool moreNetWork;
  
    

    /*----------------------------*/
    /* PER-MOTION-NODE STATISTICS */
    /*----------------------------*/

    uint32 stat_total_chunks_sent;      /* Tuple-chunks sent. */
    uint32 stat_total_bytes_sent;       /* Bytes sent, including headers. */
    uint32 stat_tuple_bytes_sent;       /* Bytes of pure tuple-data sent. */

    uint32 stat_total_chunks_recvd;     /* Tuple-chunks received. */
    uint32 stat_total_bytes_recvd;      /* Bytes received, including headers. */
    uint32 stat_tuple_bytes_recvd;      /* Bytes of pure tuple-data received. */

    uint32 stat_total_sends;            /* Total calls to SendTuple. */

    uint32 stat_total_recvs;            /* Total calls to RecvTuple/etc. */

    uint32 stat_tuples_available;       /* Total tuples awaiting receive. */
    uint32 stat_tuples_available_hwm;   /* High-water-mark of this value. */

} MotionNodeEntry;


/*=========================================================================
 * CHUNK SORTER DATA STRUCTURES
 */


/* This is the key for the chunk-sorter.  Incoming chunks are grouped by
 * sourceRoute-index and destination motion-node.
 */
typedef struct ChunkSorterKey
{
    int16 srcRoute;
    int16 motion_node_id;
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
    
    /* A FIFO to hold the tuples that have been completed but not yet
     * retrieved.  This will not be initialized until it is actually needed.
     */
    htup_fifo ready_tuples;
    
    /* Flag recording whether end-of-stream has been reported from the source.
     */
    bool end_of_stream;

    /*---------------------------------------*/
    /* PER-(MOTION NODE & SENDER) STATISTICS */
    /*                                       */
    /* These are utilized primarily in order */
    /* preserving motion nodes.              */
    /*---------------------------------------*/

    uint32 stat_tuples_available;       /* Total tuples awaiting receive. */
    uint32 stat_tuples_available_hwm;   /* High-water-mark of this value. */

} ChunkSorterEntry;

/*=========================================================================
 * MOTION LAYER DATA STRUCTURE
 */

typedef struct MotionLayerState
{
    /*--------------*/
    /* GLOBAL STATE */
    /*--------------*/

    /* The host ID that this segment-database is on. */
    int host_id;

	/* Memory context for the whole motion layer.  This is a child context of
	 * the TopMemoryContext, so we are responsible for managing it.
	 */
	MemoryContext motion_layer_mctx;

    /*------------------------------------------------------------------*/
    /* MOTION NODE STATE - Initialized and used on per-statement basis. */
    /*------------------------------------------------------------------*/

    /* Map to store information about each motion node in a plan. */
    HTAB *motionnode_map;

    /* Map to store incomplete tuples (and completed ones, when
     * order_preserving == true), associated with the sender and intended
     * destination motion-node.
     */
    HTAB *chunksort_map;

    /*--------------------------------*/
    /* GLOBAL MOTION-LAYER STATISTICS */
    /*--------------------------------*/

    uint32 stat_total_chunks_sent;  /* Tuple-chunks sent. */
    uint32 stat_total_bytes_sent;   /* Bytes sent, including headers. */
    uint32 stat_tuple_bytes_sent;   /* Bytes of pure tuple-data sent. */

    uint32 stat_total_chunks_recvd; /* Tuple-chunks received. */
    uint32 stat_total_bytes_recvd;  /* Bytes received, including headers. */
    uint32 stat_tuple_bytes_recvd;  /* Bytes of pure tuple-data received. */

    uint32 stat_total_chunkproc_calls;  /* Calls to processIncomingChunks() */

} MotionLayerState;


/*=========================================================================
 * STATIC STATE VARS
 */


static MotionLayerState s_motion_layer_state;

static TupleChunkListItemData s_eos_chunk_data =
    { NULL, TUPLE_CHUNK_HEADER_SIZE, "                " };

/*=========================================================================
 * HELPER FUNCTION DECLARATIONS
 */


static void initMotionLayerStructs(void);

static MotionNodeEntry *getMotionNodeEntry(int16 motNodeID, char *errString);
static ChunkSorterEntry *getChunkSorterEntry(MotionNodeEntry *motNodeEntry,
                                             int16 srcRoute);
static bool addChunkToSorter(MotionNodeEntry *pMNEntry, TupleChunkListItem tcItem, 
		int16 motNodeID, int16 srcRoute);

static void processIncomingChunks( MotionNodeEntry *pMNEntry, int16 motNodeID, 
		int16 srcRoute );

static void reconstructTuple(MotionNodeEntry *pMNEntry, ChunkSorterEntry *pCSEntry);

/* Stats-function declarations. */
static void statSendTuple(MotionNodeEntry *pMNEntry, TupleChunkList tcList);
static void statSendEOS(MotionNodeEntry *pMNEntry);
static void statChunksProcessed(MotionNodeEntry *pMNEntry, int chunksProcessed, int chunkBytes, int tupleBytes);
static void statNewTupleArrived(MotionNodeEntry *pMNEntry, ChunkSorterEntry *pCSEntry);
static void statRecvTuple(MotionNodeEntry *pMNEntry,
                          ChunkSorterEntry *pCSEntry,
                          ReceiveReturnCode recvRC);


/*=========================================================================
 * FUNCTION DEFINITIONS
 */


/* This function performs one-time initialization of the Motion Layer.  This
 * should be called at "postgres" process initialization time.
 */
void InitMotionLayer(void)
{
	/* Create the memory-contexts that we will use within the Motion Layer.
	 *
	 * The Motion Layer memory-context is a child of the TopMemoryContext, so
	 * that we can manage when it is reset ourselves.
	 *
	 * The tuple-serial memory-context is a child of the Motion Layer
	 * memory-context 
	 */
	s_motion_layer_state.motion_layer_mctx = AllocSetContextCreate(
		TopMemoryContext, "MotionLayerMemCtxt",
		ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);


	Cdb_max_tuple_chunk_size = Mpp_max_packet_size - PACKET_HEADER_SIZE - TUPLE_CHUNK_HEADER_SIZE;
	
	/* Initialize the Motion Layer IPC subsystem. */
	InitMotionLayerIPC( );

	/* Initialize the dynamically-allocated data-structures that the Motion
	 * Layer uses.
	 */
	initMotionLayerStructs();
}



/* This function completely resets the Motion Layer at the end of a query
 * execution.
 */
void ResetMotionLayer(bool flushCommLayer)
{
	/* Emit statistics to log */
	elog(DEBUG1, "ResetMotionLayer(): dumping stats");
	elog(DEBUG1, "  Sent:");
	elog(DEBUG1, "   total chunks: %u", s_motion_layer_state.stat_total_chunks_sent);
	elog(DEBUG1, "   total bytes: %u", s_motion_layer_state.stat_total_bytes_sent);
	elog(DEBUG1, "   tuple bytes: %u", s_motion_layer_state.stat_tuple_bytes_sent);
	elog(DEBUG1, "  Received:");
	elog(DEBUG1, "   total chunks: %u", s_motion_layer_state.stat_total_chunks_recvd);
	elog(DEBUG1, "   total bytes: %u", s_motion_layer_state.stat_total_bytes_recvd);
	elog(DEBUG1, "   tuple bytes: %u", s_motion_layer_state.stat_tuple_bytes_recvd);
	elog(DEBUG1, "  Chunkproc call count: %u", s_motion_layer_state.stat_total_chunkproc_calls);

	hash_destroy(s_motion_layer_state.motionnode_map);
	hash_destroy(s_motion_layer_state.chunksort_map);

	/* Free all memory used by the Motion Layer in the processing of the
	 * previous query.
	 */
	MemoryContextReset(s_motion_layer_state.motion_layer_mctx);

	/* Reinitialize the dynamic data-structures that the Motion Layer uses. */
	initMotionLayerStructs();
}


/* CleanUpMotionLayer function used to shutdown the Motion Layer and associated
 * interconnect code.  This includes shutting down the Motion Listener that 
 * ml_ipc sets up.  
 *
 * Generally, this function should only be called on process exit or when 
 * switching out of a cdb mode.
 */
void CleanUpMotionLayer(void)
{
	elog( DEBUG3, "Cleaning Up Motion Layer...");
	
	/* shutdown our listener socket */
	CleanUpMotionLayerIPC();
	
	MemoryContextDelete(s_motion_layer_state.motion_layer_mctx);
}


void initMotionLayerStructs(void)
{
    MemoryContext oldCtxt;
    HASHCTL hctl;
    HTAB *htab;

    /* Switch to the Motion Layer memory context, so that we can clean things
	 * up easily.
     */
    oldCtxt = MemoryContextSwitchTo(s_motion_layer_state.motion_layer_mctx);

    /* Initialize motion-node hashtable.  The initial number of bins is chosen
     * to be relatively small, since the max number of bins is just the number
     * of motion nodes in the plan.
     */

    hctl.keysize = sizeof(int16);
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

    /* Allocation is done.  Go back to caller memory-context. */
    MemoryContextSwitchTo(oldCtxt);
	
	/* Clear out statistics values.
	 *
	 * TODO:  Perhaps move these to a separate struct so we can just memset()
	 *        them all out?
	 */
    s_motion_layer_state.stat_total_chunks_sent = 0;
    s_motion_layer_state.stat_total_bytes_sent = 0;
    s_motion_layer_state.stat_tuple_bytes_sent = 0;
    s_motion_layer_state.stat_total_chunks_recvd = 0;
    s_motion_layer_state.stat_total_bytes_recvd = 0;
    s_motion_layer_state.stat_tuple_bytes_recvd = 0;
    s_motion_layer_state.stat_total_chunkproc_calls = 0;
}



/* Initialize a single motion node.  This is called by the executor when a
 * motion node in the plan tree is being initialized.
 *
 * This function is called from:  ExecInitMotion()
 */
void InitMotionLayerNode(int16 motNodeID, bool preserveOrder, TupleDesc tupDesc)
{
    MemoryContext oldCtxt;
    MotionNodeEntry *pEntry;
    bool found = false;

    AssertArg(tupDesc != NULL);

    /* Switch to the Motion Layer's memory-context, so that the motion node
	 * can be reset later.
     */
    oldCtxt = MemoryContextSwitchTo(s_motion_layer_state.motion_layer_mctx);

    elog(DEBUG2, "Initializing entry for motion node %d in Motion Layer.",
        motNodeID);

    /* Create a new MotionNodeEntry.  If one already exists, complain! */

	do
	{
			
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
			EndMotionLayerNode( motNodeID,false );
		}
	}
	while( found );



    /* Finish up initialization of the motion node entry. */

    pEntry->preserve_order = preserveOrder;
    pEntry->tuple_desc = CreateTupleDescCopy(tupDesc);
	InitSerTupInfo(pEntry->tuple_desc, &pEntry->ser_tup_info);
	
    if (!preserveOrder)
    {
        /* Create a tuple-store for the motion node's incoming tuples. */
        
        elog(DEBUG3, "Motion node %d is not order-preserving.  Creating"
            " node-level tuple-store.", motNodeID);

        pEntry->ready_tuples = htfifo_create(work_mem);
    }
    else
    {
        elog(DEBUG3, "Motion node %d is order-preserving.  No node-level"
            " tuple-store.", motNodeID);

        pEntry->ready_tuples = NULL;
    }


    /* we'll just set this to 0.  later, ml_ipc will call setExpectedReceivers() 
     * to set this if we are a "Receiving" motion node.
     */
    pEntry->num_senders = 0;

    pEntry->num_stream_ends_recvd = 0;

    /* Initialize statistics counters. */
    pEntry->stat_total_chunks_sent 		= 0;
    pEntry->stat_total_bytes_sent 		= 0;
    pEntry->stat_tuple_bytes_sent 		= 0;
    pEntry->stat_total_sends 			= 0;
    pEntry->stat_total_recvs 			= 0;
    pEntry->stat_tuples_available 		= 0;
    pEntry->stat_tuples_available_hwm 	= 0;
	pEntry->stat_total_chunks_recvd		= 0;
	pEntry->stat_tuple_bytes_recvd		= 0;

    pEntry->cleanedUp = false;
    pEntry->moreNetWork = true;
      
    /* All done!  Go back to caller memory-context. */
    MemoryContextSwitchTo(oldCtxt);
}


void setExpectedReceivers( int16 motNodeID, int expectedReceivers )
{
	MotionNodeEntry *pEntry = getMotionNodeEntry(motNodeID,"setExpectedReceivers");
			
	pEntry->num_senders = expectedReceivers;		
}

/*
 * Function:  SendTuple - Sends a portion or whole tuple to the AMS layer.
 *
 */
SendReturnCode SendTuple(int16 motNodeID, HeapTuple tuple, int16 targetRoute)
{
    MotionNodeEntry 	*pMNEntry;
    TupleChunkList 		tcList;
    MemoryContext 		oldCtxt;
	SendReturnCode		rc;
	
    /* Pull up the motion node entry with the node's details.  This includes
     * details that affect sending, such as whether the motion node needs to
     * include backup segment-dbs.
     */
    pMNEntry = getMotionNodeEntry(motNodeID,"SendTuple");
    
    
#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "Serializing HeapTuple for sending.");
#endif

	/* Create and store the serialized form, and some stats about it. */
	oldCtxt = MemoryContextSwitchTo(s_motion_layer_state.motion_layer_mctx);
	tcList = CvtHeapTupToChunks(tuple,&pMNEntry->ser_tup_info, Cdb_max_tuple_chunk_size );
	MemoryContextSwitchTo(oldCtxt);

#ifdef AMS_VERBOSE_LOGGING
	elog(DEBUG3, "Serialized HeapTuple for sending:\n"
		"\ttarget-route %d \n"
		"\t%d bytes in serial form\n"
		"\tbroken into %d chunks",
		targetRoute,
		tcList->serialized_data_length,
		tcList->num_chunks);
#endif
    
    
    
    /* do the send. */
    if (!SendTupleChunkToAMS(motNodeID, targetRoute, tcList->p_first))
    {
  		rc = STOP_SENDING;
    }
    else
    {
    	/* update stats */
    	statSendTuple(pMNEntry, tcList ); 
    	
    	rc = SEND_COMPLETE;
    }
    
    /* cleanup */
    deleteTCList( &tcList );
	        
    return rc;
}


/* Sends a token to one or all peer Motion Nodes, indicating that this motion
 * node has no more tuples to send out.
 *
 * The send operation is nonblocking, and will return SEND_INCOMPLETE if the
 * send would block.  When this value is returned, the caller must try again
 * until the SEND_COMPLETE value is returned.
 */
SendReturnCode SendEndOfStream(int16 motNodeID, int16 targetRoute)
{
    MotionNodeEntry *pMNEntry;
    uint8 *pData;


    /* Pull up the motion node entry with the node's details.  This includes
     * details that affect sending, such as whether the motion node needs to
     * include backup segment-dbs.
     */
    pMNEntry = getMotionNodeEntry(motNodeID, "SendEndOfStream");    

    /* Use the statically allocated chunk that is intended for sending end-of-
     * stream messages so that we don't incur allocation and deallocation
     * overheads.
     */
    Assert(s_eos_chunk_data.p_next == NULL);
    Assert(s_eos_chunk_data.chunk_length == TUPLE_CHUNK_HEADER_SIZE);

    pData = s_eos_chunk_data.chunk_data;

    SetChunkDataSize(pData, 0);
    SetChunkType(pData, TC_END_OF_STREAM);	
		
    SendEosToAMS(motNodeID, targetRoute, &s_eos_chunk_data);

	/* We increment our own "stream-ends received" count when we send our
	 * own, as well as when we receive one.
	 */
	pMNEntry->num_stream_ends_recvd++;

    /* We record EOS as if a tuple were sent. */
    statSendEOS(pMNEntry);

    return SEND_COMPLETE;
}


/*
 * Function:
 *
 */
ReceiveReturnCode RecvTuple(int16 motNodeID, HeapTuple *tup_i)
{
    MotionNodeEntry *pMNEntry;
    ReceiveReturnCode recvRC;

    /* Process all chunks in our incoming queue, regardless of which motion
     * nodes they are coming from.
     */

    /* Get the tuple-store that should contain heap-tuples that we want. */

    pMNEntry = getMotionNodeEntry(motNodeID, "RecvTuple");

    if (pMNEntry->preserve_order)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("RecvTuple called for order-preserving"
                " motion-node ID %d.", motNodeID)));
    }

    /* Get the next HeapTuple, if one is available! */
    *tup_i = htfifo_gettuple(pMNEntry->ready_tuples);

	do
	{
		if (*tup_i == NULL && pMNEntry->moreNetWork)
		{
			processIncomingChunks( pMNEntry, motNodeID, ANY_ROUTE );
			*tup_i = htfifo_gettuple(pMNEntry->ready_tuples);
		}
			
	
		if (*tup_i != NULL)
		{
			/* We got a tuple. */
			recvRC = GOT_TUPLE;
		}
		else if (!pMNEntry->moreNetWork)
		{
			
			/* No tuple was available (tuple-store was at EOF), and end-of-stream
			 * has been marked.  No more tuples are going to show up.
			 */
			recvRC = END_OF_STREAM;
		}
		else
		{
			/* No tuple was available (tuple-store was at EOF), but we have not
			 * been sent an end-of-stream.  Tell the caller that there are no
			 * tuples right now.
			 */
			recvRC = NO_TUPLE;
		}

	    /* Stats */
    	statRecvTuple(pMNEntry, NULL, recvRC);

	}
	while( recvRC == NO_TUPLE );
	
    return recvRC;
}


ReceiveReturnCode RecvTupleFrom(int16 motNodeID, HeapTuple *tup_i, int16 srcRoute)
{
    MotionNodeEntry 	*pMNEntry;
    ChunkSorterEntry 	*pCSEntry;
    ReceiveReturnCode 	recvRC;
  
  
#ifdef AMS_VERBOSE_LOGGING
	elog( DEBUG3, "RecvTupleFrom( motNodeID = %d, srcRoute = %d )", motNodeID, srcRoute );
#endif	


    pMNEntry = getMotionNodeEntry(motNodeID, "RecvTupleFrom");


    if (!pMNEntry->preserve_order)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("RecvTupleFrom called for non order-preserving"
                " motion-node ID %d.", motNodeID)));
    }
    

    /* Pull up the chunk-sorter entry for the specified sender, and get the
     * tuple-store we should use.
     */
    pCSEntry = getChunkSorterEntry(pMNEntry, srcRoute);

    /* Get the next HeapTuple, if one is available! */
    *tup_i = htfifo_gettuple(pCSEntry->ready_tuples);

	if( *tup_i == NULL )
	{
		
		if( pCSEntry->end_of_stream )
		{
			recvRC = END_OF_STREAM;
			statRecvTuple(pMNEntry, pCSEntry, recvRC);
			return recvRC;
		}
		
		processIncomingChunks( pMNEntry, motNodeID, srcRoute );	
    	*tup_i = htfifo_gettuple(pCSEntry->ready_tuples);
	}


    if (*tup_i != NULL)
    {
        /* We got a tuple. */
        recvRC = GOT_TUPLE;
    }
    else if (pCSEntry->end_of_stream)
    {
        /* No tuple was available (tuple-store was at EOF), and end-of-stream
         * has been marked.  No more tuples are going to show up.
         */
        recvRC = END_OF_STREAM;
    }
    else
    {
        /* No tuple was available (tuple-store was at EOF), but we have not
         * been sent an end-of-stream.  Tell the caller that there are no
         * tuples right now.
         */
        recvRC = NO_TUPLE;
    }

    /* Stats */
    statRecvTuple(pMNEntry, pCSEntry, recvRC);

    return recvRC;
}




/* This helper function is the receive-tuple workhorse.  It pulls tuple chunks
 * from the AMS, and pushes them to the chunk-sorter where they can be sorted
 * based on sender, and reconstituted into whole HeapTuples.  Functions like
 * RecvTuple() and RecvTupleFrom() should definitely call this before trying
 * to get a HeapTuple to return.  It can also be called during other
 * operations if that seems like a good idea.  For example, it can be called
 * sometime during send-tuple operations as well.
 */
void processIncomingChunks(  MotionNodeEntry *pMNEntry, int16 motNodeID, int16 srcRoute )
{
    TupleChunkListItem tcItem, tcNext;
    MemoryContext oldCtxt;
	
	/* Keep track of processed chunk stats. */
    int numChunks, chunkBytes, tupleBytes;

    oldCtxt = MemoryContextSwitchTo(s_motion_layer_state.motion_layer_mctx);

    /* Get all of the currently available tuple-chunks, and push each one into
     * the chunk-sorter.
     */     
	if( srcRoute == ANY_ROUTE )
		tcItem = RecvTupleChunkFromAny( motNodeID, &srcRoute);
	else
		tcItem = RecvTupleChunkFrom( motNodeID, srcRoute );

    numChunks = 0;
    chunkBytes = 0;
    tupleBytes = 0;

    while (tcItem != NULL)
    {
        numChunks++;

        /* Detach the current chunk off of the front of the list. */
        tcNext = tcItem->p_next;
        tcItem->p_next = NULL;


		/* Track stats. */
		chunkBytes += tcItem->chunk_length;
		if (tcItem->chunk_length >= TUPLE_CHUNK_HEADER_SIZE)
		{
			tupleBytes += tcItem->chunk_length - TUPLE_CHUNK_HEADER_SIZE;
		}
		else
		{
			elog(ERROR, "Received tuple-chunk of size %u; smaller than"
				" chunk header size %d!", tcItem->chunk_length,
				TUPLE_CHUNK_HEADER_SIZE);
		}

        /* Stick the chunk into the sorter. */
        addChunkToSorter(pMNEntry, tcItem, motNodeID, srcRoute);

        tcItem = tcNext;
    }

    /* Stats */
    statChunksProcessed(pMNEntry, numChunks, chunkBytes, tupleBytes);

    MemoryContextSwitchTo(oldCtxt);
}




void EndMotionLayerNode(int16 motNodeID, bool flushCommLayer)
{
    HASH_SEQ_STATUS seqStatus;
    MotionNodeEntry *pMNEntry;
    ChunkSorterEntry *pCSEntry;

    pMNEntry = getMotionNodeEntry(motNodeID, "EndMotionLayerNode");

    elog(DEBUG2, "Cleaning up Motion Layer details for motion node %d.",
        motNodeID);

    /* Iterate through all entries in the motion layer's chunk-sort map, to
     * see if we have gotten end-of-stream from all senders.
     */

    hash_seq_init(&seqStatus, s_motion_layer_state.chunksort_map);

    while ((pCSEntry = (ChunkSorterEntry *) hash_seq_search(&seqStatus)) != NULL)
    {
        /* Skip entries for motion nodes we don't care about. */
        if (pCSEntry->key.motion_node_id != motNodeID)
            continue;

        if (pMNEntry->preserve_order)
        {
            /* Print chunk-sorter entry statistics. */
            
            elog(DEBUG1, "Chunk-sorter entry [src=%d,mn=%d] statistics:\n"
                "\tAvailable Tuples High-Watermark:  %u\n",
                pCSEntry->key.srcRoute, pCSEntry->key.motion_node_id,
                pMNEntry->stat_tuples_available_hwm);
        }

        elog(DEBUG3, "Cleaning up chunk-sorter entry [src=%d,mn=%d].",
            pCSEntry->key.srcRoute, pCSEntry->key.motion_node_id);

        /* If we haven't gotten the end-of-stream yet, handle it according to
         * the caller's request.
         */

        if (!(pCSEntry->end_of_stream))
        {
            if (flushCommLayer)
            {
                elog(DEBUG2, "Motion layer node %d cleanup - did not receive"
                    " end-of-stream from sender %d.  Waiting.", motNodeID,
                    pCSEntry->key.srcRoute);

                /*** TODO - get chunks until end-of-stream comes in. ***/
                Assert(false);
            }
            else
            {
                elog(WARNING, "Motion layer node %d cleanup - did not receive"
                    " end-of-stream from sender %d.", motNodeID,
                    pCSEntry->key.srcRoute);
            }
        }
        else
        {
            /* End-of-stream is marked for this entry. */
            
            /*** TODO - do more than just complain! ***/
            
            if (pCSEntry->chunk_list.num_chunks > 0)
            {
                elog(WARNING, "Motion layer node %d cleanup - there are still"
                    " %d chunks enqueued from sender %d.", motNodeID,
                    pCSEntry->chunk_list.num_chunks,
                    pCSEntry->key.srcRoute);
            }
            
            /***
             TODO - Make sure there are no outstanding tuples in the
                    tuple-store.
             ***/
            
        }
        

        /* Clean up the chunk-sorter entry, then remove it from the hash
         * table.
         */

        clearTCList(&pCSEntry->chunk_list);
        if (pMNEntry->preserve_order)   /* Clean up the tuple-store. */
            htfifo_destroy(pCSEntry->ready_tuples);
        
        hash_search(s_motion_layer_state.chunksort_map, &pCSEntry->key,
            HASH_REMOVE, NULL);
            
        pMNEntry->cleanedUp = true;
    }

    /* Clean up the motion-node entry, then remove it from the hash table. */
	elog(DEBUG1, "Motion Node %d Statistics:\n"
	    	"-- Sending:   Attempts: %u Chunks Sent: %u  Tuple Bytes: %u  Total Bytes: %u \n"
	    	"-- Receiving: Attempts: %u Chunks Received: %u Tuple Bytes: %u Total Bytes: %u\n",
	    	motNodeID, 
	    	pMNEntry->stat_total_sends,  
	    	pMNEntry->stat_total_chunks_sent,
        	pMNEntry->stat_tuple_bytes_sent,
        	pMNEntry->stat_total_bytes_sent,
        	pMNEntry->stat_total_recvs,
        	pMNEntry->stat_total_chunks_recvd,
        	pMNEntry->stat_tuple_bytes_recvd,
        	pMNEntry->stat_total_bytes_recvd
        	);

    elog(DEBUG3, "Cleaning up motion-node entry %d.", motNodeID);

	CleanupSerTupInfo(&pMNEntry->ser_tup_info);
    FreeTupleDesc(pMNEntry->tuple_desc);
    if (!pMNEntry->preserve_order)
        htfifo_destroy(pMNEntry->ready_tuples);

    hash_search(s_motion_layer_state.motionnode_map,
        &pMNEntry->motion_node_id, HASH_REMOVE, NULL);
        
}


/* Helper function to get the motion node entry for a given ID.  NULL is
 * returned if the ID is unrecognized.
 */
MotionNodeEntry *getMotionNodeEntry(int16 motNodeID, char *errString)
{
	MotionNodeEntry *pMNEntry;
	
    pMNEntry = (MotionNodeEntry *) hash_search(
        s_motion_layer_state.motionnode_map, &motNodeID, HASH_FIND, NULL);

	if (pMNEntry == NULL)
    {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("%s called for unrecognized motion-node ID %d.",
            errString, motNodeID)));
    }
    
    return pMNEntry;
}


/* Retrieve the chunk-sorter entry for the specified motion-node/source pair.
 * If one doesn't exist, it is created and initialized.
 *
 * It might not seem obvious why the MotionNodeEntry is required as an
 * argument.  It's in there to ensure that only valid motion nodes are
 * represented in the chunk-sorter.  The motion-node's entry is typically
 * retrieved using getMotionNodeEntry() before this function is called.
 */
ChunkSorterEntry *getChunkSorterEntry( MotionNodeEntry *motNodeEntry,
    								   int16 srcRoute)
{
    int16 motNodeID;
    MemoryContext oldCtxt;
    ChunkSorterKey csKey;
    ChunkSorterEntry *chunkSorterEntry;
    bool found;

    AssertArg(motNodeEntry != NULL);

    motNodeID = motNodeEntry->motion_node_id;

    memset(&csKey, 0, sizeof(csKey));
    csKey.srcRoute = srcRoute;
    csKey.motion_node_id = motNodeID;

    oldCtxt = MemoryContextSwitchTo(s_motion_layer_state.motion_layer_mctx);

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
			 " [src=%d,mn=%d].", srcRoute, motNodeID);

		chunkSorterEntry->chunk_list.serialized_data_length = 0;
        chunkSorterEntry->chunk_list.num_chunks = 0;
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
				 srcRoute, motNodeID);

            chunkSorterEntry->ready_tuples = htfifo_create(work_mem);

            /* Sanity-check: */
            Assert(motNodeEntry->ready_tuples == NULL);
        }
        else
        {
            elog(DEBUG3, "Motion node %d is not order-preserving.  Using"
				 " shared tuple-store for entry [src=%d,mn=%d].", motNodeID,
				 srcRoute, motNodeID);

            chunkSorterEntry->ready_tuples = motNodeEntry->ready_tuples;

            /* Sanity-check: */
            Assert(motNodeEntry->ready_tuples != NULL);
        }
    }

    MemoryContextSwitchTo(oldCtxt);

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
bool addChunkToSorter(MotionNodeEntry *pMNEntry, TupleChunkListItem tcItem, 
		int16 motNodeID, int16 srcRoute)
{
    MemoryContext 		oldCtxt;
    ChunkSorterEntry 	*chunkSorterEntry;
    bool 				tupleCompleted = false;
    TupleChunkType 		tcType;


    AssertArg(tcItem != NULL);


    oldCtxt = MemoryContextSwitchTo(s_motion_layer_state.motion_layer_mctx);

   
    chunkSorterEntry = getChunkSorterEntry(pMNEntry, srcRoute);

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
                    " partial tuple data.", srcRoute, motNodeID)));
        }
        
        /* Put this chunk into the list, then turn it into a HeapTuple! */

		appendChunkToTCList(&chunkSorterEntry->chunk_list, tcItem);
		reconstructTuple(pMNEntry, chunkSorterEntry);
		tupleCompleted = true;

        break;

    case TC_PARTIAL_START:
        
        /* There shouldn't be any partial tuple data in the list! */
        if (chunkSorterEntry->chunk_list.num_chunks != 0)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received TC_PARTIAL_START chunk from [src=%d,mn=%d]"
                    " after partial tuple data.", srcRoute, motNodeID)));
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
                    " without any leading tuple data.", srcRoute, motNodeID)));
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
                    " without any leading tuple data.", srcRoute, motNodeID)));
        }

        /* Put this chunk into the list, then turn it into a HeapTuple! */
		appendChunkToTCList(&chunkSorterEntry->chunk_list, tcItem);
		reconstructTuple(pMNEntry, chunkSorterEntry);
		tupleCompleted = true;

        break;

    case TC_END_OF_STREAM:

	
        
        /* There shouldn't be any partial tuple data in the list! */
        if (chunkSorterEntry->chunk_list.num_chunks != 0)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received TC_END_OF_STREAM chunk from [src=%d,mn=%d]"
                    " after partial tuple data.", srcRoute, motNodeID)));
        }
        
        /* Make sure that we haven't already received end-of-stream! */
        
        if (chunkSorterEntry->end_of_stream)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received end-of-stream chunk from"
                    " [src=%d,mn=%d] when already marked as at end-of-stream.",
                    srcRoute, motNodeID)));
        }
        
        /* Mark the state as "end of stream." */
        chunkSorterEntry->end_of_stream = true;
        pMNEntry->num_stream_ends_recvd++;
        
        if( pMNEntry->num_stream_ends_recvd == pMNEntry->num_senders )
        	pMNEntry->moreNetWork = false;

		/* Since we received an end-of-stream.  Then we no longer need read
		 * interest in the interconnect. 
		 */
		DeregisterReadInterest(motNodeID, srcRoute);
		SendEosAck(motNodeID, srcRoute);
        
        break;
        
    case TC_ABORT_STREAM:
    
        /* Since this indicates an error condition, there maybe partial tuple
         * data in the list.
         */
        
        elog(DEBUG2, "Received TC_ABORT_STREAM from [src=%d,mn=%d].",
            srcRoute, motNodeID);

        /* Make sure that we haven't already received end-of-stream! */
        
        if (chunkSorterEntry->end_of_stream)
        {
            ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg("Received end-of-stream chunk from [src=%d,mn=%d]"
                    " when already marked as at end-of-stream.",
                    srcRoute, motNodeID)));
        }
        
        /* Mark the state as "end of stream." */
        chunkSorterEntry->end_of_stream = true;
        pMNEntry->num_stream_ends_recvd++;

		DeregisterReadInterest(motNodeID, srcRoute);
		SendEosAck(motNodeID, srcRoute);
        
        break;
        
    default:
        ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("Received tuple chunk of unrecognized type %d"
                " from [src=%d,mn=%d].", tcType, srcRoute, motNodeID)));
    }

    MemoryContextSwitchTo(oldCtxt);

    return tupleCompleted;
}


/* Helper function to perform the operations necessary to reconstruct a
 * HeapTuple from a list of tuple-chunks, and then update the Motion Layer
 * state appropriately.  This includes storing the tuple, cleaning out the
 * tuple-chunk list, and recording statistics about the newly formed tuple.
 */
void reconstructTuple(MotionNodeEntry *pMNEntry, ChunkSorterEntry *pCSEntry)
{
	HeapTuple htup;
	
	/* Convert the list of chunks into a tuple, then stow it away. */

	htup = CvtChunksToHeapTup(&pCSEntry->chunk_list, &pMNEntry->ser_tup_info);

	 
	htfifo_addtuple(pCSEntry->ready_tuples, htup);

	/* Clean up the chunk-list to receive a new tuple. */
	clearTCList(&pCSEntry->chunk_list);

	/* Stats */
	statNewTupleArrived(pMNEntry, pCSEntry);
}


/*=========================================================================
 * STATISTICS HELPER-FUNCTIONS
 */

void statSendTuple(MotionNodeEntry *pMNEntry, TupleChunkList tcList)
{
 	int headerOverhead;
 	
    AssertArg(pMNEntry != NULL);
 
 	headerOverhead = TUPLE_CHUNK_HEADER_SIZE * tcList->num_chunks;
 	
 	/* per motion-node stats. */
    pMNEntry->stat_total_chunks_sent += tcList->num_chunks;
    pMNEntry->stat_total_bytes_sent += tcList->serialized_data_length + headerOverhead;
    pMNEntry->stat_tuple_bytes_sent += tcList->serialized_data_length;
    
    /* Update global motion-layer statistics. */        
	s_motion_layer_state.stat_total_chunks_sent += tcList->num_chunks;

	s_motion_layer_state.stat_total_bytes_sent += 
			tcList->serialized_data_length + headerOverhead;
	
	s_motion_layer_state.stat_tuple_bytes_sent += tcList->serialized_data_length;
	 	 
}


void statSendEOS(MotionNodeEntry *pMNEntry)
{
    AssertArg(pMNEntry != NULL);
    
	/* Update motion node statistics. */
    pMNEntry->stat_total_sends++;
    pMNEntry->stat_total_chunks_sent++;
    pMNEntry->stat_total_bytes_sent += TUPLE_CHUNK_HEADER_SIZE;

    /* Update global motion-layer statistics. */        
    s_motion_layer_state.stat_total_chunks_sent++;
    s_motion_layer_state.stat_total_bytes_sent += TUPLE_CHUNK_HEADER_SIZE;

}


void statChunksProcessed(MotionNodeEntry *pMNEntry, int chunksProcessed, int chunkBytes, int tupleBytes)
{
	AssertArg(chunksProcessed >= 0);
	AssertArg(chunkBytes >= 0);
	AssertArg(tupleBytes >= 0);
	
	/* Update Global Motion Layer Stats. */
    s_motion_layer_state.stat_total_chunks_recvd += chunksProcessed;	
    s_motion_layer_state.stat_total_chunkproc_calls++;
	s_motion_layer_state.stat_total_bytes_recvd += chunkBytes;
	s_motion_layer_state.stat_tuple_bytes_recvd += tupleBytes;
	
	/* Update Motion-node stats. */
	pMNEntry->stat_total_chunks_recvd += chunksProcessed;
	pMNEntry->stat_total_bytes_recvd += chunkBytes;
	pMNEntry->stat_tuple_bytes_recvd += tupleBytes;
}


void statNewTupleArrived(MotionNodeEntry *pMNEntry, ChunkSorterEntry *pCSEntry)
{
    uint32 tupsAvail;

    AssertArg(pMNEntry != NULL);
    AssertArg(pCSEntry != NULL);

    /*
     * High-watermarks:  We track the number of tuples available to receive,
     * but that haven't yet been received.  The high-watermark is recorded.
     *
     * Also, if the motion node is order-preserving, we track a per-sender
     * high-watermark as well.
     */

    tupsAvail = ++(pMNEntry->stat_tuples_available);
    if (pMNEntry->stat_tuples_available_hwm < tupsAvail)
    {
        /* New high-watermark! */
        pMNEntry->stat_tuples_available_hwm = tupsAvail;
    }

    if (pMNEntry->preserve_order)
    {
        /* Track per-sender high watermark. */
        tupsAvail = ++(pCSEntry->stat_tuples_available);
        if (pCSEntry->stat_tuples_available_hwm < tupsAvail)
        {
            /* New high-watermark! */
            pCSEntry->stat_tuples_available_hwm = tupsAvail;
        }
    }
}


void statRecvTuple(MotionNodeEntry *pMNEntry, ChunkSorterEntry *pCSEntry,
                   ReceiveReturnCode recvRC)
{
    AssertArg(pMNEntry != NULL);
    AssertArg(pCSEntry != NULL || !pMNEntry->preserve_order);

    /* Keep track of calls to RecvTuple() and RecvTupleFrom(). */

    pMNEntry->stat_total_recvs++;

    /* Update "tuples available" counts for high watermark stats. */

    if (recvRC == GOT_TUPLE)
    {
        pMNEntry->stat_tuples_available--;

        if (pMNEntry->preserve_order)
            pCSEntry->stat_tuples_available--;
    }
}
