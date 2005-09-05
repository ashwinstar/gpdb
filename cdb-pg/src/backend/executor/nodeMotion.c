/*-------------------------------------------------------------------------
 *
 * nodeMotion.c
 *	  Routines to handle moving tuples around in cdb.
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "cdb/cdbheap.h"
#include "cdb/cdblink.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "executor/execdebug.h"
#include "executor/nodeMotion.h"
#include "optimizer/clauses.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static TupleTableSlot *execMotionSender(MotionState *node); 
static TupleTableSlot *execMotionUnsortedReceiver(MotionState *node);
static TupleTableSlot *execMotionSortedReceiver(MotionState *node);

static void execMotionSortedReceiverFirstTime(MotionState *node );

static uint32 evalHashKey(ExprContext *econtext, List *hashkeys, List* hashtypes, CdbHash *h);
static bool setReadFromAMS(Motion *node);

static void doSendEndOfStream( Motion *motion, MotionState *node );
static void doSendTuple( Motion *motion, MotionState *node, TupleTableSlot 	*outerTupleSlot );


/*=========================================================================
 * VISIBLE FUNCTIONS
 */


/* ----------------------------------------------------------------
 *		ExecMotion
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMotion(MotionState *node)
{
	bool isReceiver;
	
	Motion *motion = (Motion *)node->ps.plan;

    /* TODO: it would be nice to store this isReceiver somewhere maybe in
     * 		 MotionState.  
     */
	if( node->ps.state->es_cur_slice_idx == motion->motionID )
    	isReceiver = false;
    else
    	isReceiver = true;


	 /* at the top here we basically decide:
     * -- SENDER vs. RECEIVER and
     * -- SORTED vs. UNSORTED
     */
	if( isReceiver )
	{
		if( motion->sendSorted )
			return execMotionSortedReceiver(node);
		else
			return execMotionUnsortedReceiver(node);
	}
	else
		return execMotionSender(node);
}


static TupleTableSlot *execMotionSender(MotionState *node)
{
	/* SENDER LOGIC */
	TupleTableSlot 	*outerTupleSlot;
	PlanState  		*outerNode;
	Motion 			*motion = (Motion *)node->ps.plan;
	bool 			done = false;
	
	SO1_printf("execMotionSender: %s\n","entering routine");
	
	AssertState(motion->motionType == MOTIONTYPE_HASH ||
        (motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs <= 1));
	
	while( !done )
	{
		/* grab TupleTableSlot from our child. */
		outerNode = outerPlanState(node);		

		outerTupleSlot = ExecProcNode(outerNode);
		
		if ( TupIsNull(outerTupleSlot) )
		{
			/* Child plan says "No more tuples!" */
			node->noMoreChildTuples = true;
			
			doSendEndOfStream( motion, node );	
			done = true;
		}
		else
		{
			doSendTuple(motion, node, outerTupleSlot);
			/* doSendTuple() may have set node->stopRequested as a side-effect */
		}	
	}
	
	/* nothing else to send out, so we return NULL up the tree. */
	return NULL;
}


static TupleTableSlot *execMotionUnsortedReceiver(MotionState *node)
{
	/* RECEIVER LOGIC */
	TupleTableSlot 		*slot;
	HeapTuple			tuple;
	Motion 				*motion = (Motion *)node->ps.plan;
	ReceiveReturnCode 	recvRC;


    SO1_printf("ExecMotion: %s\n","entering routine");
	
	AssertState(motion->motionType == MOTIONTYPE_HASH ||
        (motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs <= 1));

	if (node->stopRequested)
	{
		SendStopMessage(motion->motionID);
		return NULL;
	}

	recvRC = RecvTuple(motion->motionID, &tuple);
	
	if (recvRC == END_OF_STREAM)
	{
		elog(DEBUG4, "motionID=%d saw END_OF_STREAM", motion->motionID );
		node->noMoreAMSTuples = true;				
		
		return NULL;
	}
	else if (recvRC == GOT_TUPLE)
	{
		node->numTuplesFromAMS++;
		
		/* store it in our result slot and return this. */
		slot = node->ps.ps_ResultTupleSlot;			
		node->numTuplesToParent++;
		
		return ExecStoreTuple(tuple, slot, InvalidBuffer, true /*shouldFree*/);
	}	

	return NULL;
}



/*
 * General background on Sorted Motion:
 * -----------------------------------
 * NOTE: This function is only used for order-preserving motion.  There are
 * only 2 types of motion that order-preserving makes sense for: FIXED and
 * BROADCAST (HASH does not make sense). so we have:
 *
 * CASE 1:   broadcast order-preserving fixed motion.  This should only be
 *			 called for SENDERs.
 *
 * CASE 2:   single-destination order-preserving fixed motion.  The SENDER
 *			 side will act like Unsorted motion and won't call this. So only
 *			 the RECEIVER should be called for this case.
 *
 *
 * Sorted Receive Notes:
 * --------------------
 *
 * The 1st time we execute, we need to pull a tuple from each of our source
 * and store them in our tupleheap, this is what execMotionSortedFirstTime()
 * does.  Once that is done, we can pick the lowest (or whatever the
 * criterion is) value from amongst all the sources.  This works since each
 * stream is sorted itself.
 *
 * After pulling the tuple out of its slot, we null it out in its slot.
 * We keep track of which one was selected, this will be slot we will need
 * to fill during the next call.
 *
 * Subsuquent calls to this function (after the 1st time) will start by
 * trying to receive a tuple for the slot that was emptied the previous call.
 * Then we again select the lowest value and return that tuple.
 *
 * If an EndOfStream (EOS) is received during all of this, we essentially
 * just leave that slot empty and proceed.  The
 * GetLowestValueFromCdbTupleHeap() is smart enough to ignore them.
 *
 */
static TupleTableSlot *execMotionSortedReceiver(MotionState *node)
{
	TupleTableSlot 		*slot;
	HeapTuple 			tuple, inputTuple;
	Motion 				*motion = (Motion *)node->ps.plan;
    SendReturnCode 		sendRC;
    ReceiveReturnCode 	recvRC;
    CdbTupleHeapInfo 	*tupHeapInfo;


	SO1_printf("execMotionSorted: %s\n",
			   "entering routine");

    AssertState(motion->motionType == MOTIONTYPE_FIXED &&
                motion->numOutputSegs <= 1);
    AssertState(motion->sendSorted);
    AssertState(node->tupleheap != NULL);	

	if (node->stopRequested)
	{
		SendStopMessage(motion->motionID);
		return NULL;
	}

	/* RECEIVER LOGIC */
	if (!node->tupleheapReady)
	{
		execMotionSortedReceiverFirstTime(node);
	}
	else
	{
		// we don't add a 1 here since route 0 is segId 0 for some strange
		// reason.
		recvRC = RecvTupleFrom(motion->motionID, &inputTuple, node->segindexNext);
		
		if (recvRC == GOT_TUPLE)
		{
			node->numTuplesFromAMS++;
			node->numTuplesInHeap++;
		}			
			
		MergeNewValueIntoCdbTupleHeap(node->tupleheap, inputTuple, node->segindexNext);
					
	}
	
	/* THE HEAP SHOULD NOW HAVE A TUPLE FROM EVERY SOURCE.
	 *
	 * Pull out the next tuple and pass it back.
	 */
	
	tupHeapInfo = GetLowestValueFromCdbTupleHeap(node->tupleheap);
	tuple = tupHeapInfo->tuple;
	node->segindexNext = tupHeapInfo->sourceSegIndex;
	
	if (tuple != NULL)
	{
		/*
		 * store it in our result slot and return this.
		 */
		slot = node->ps.ps_ResultTupleSlot;
		
		node->numTuplesToParent++;
		node->numTuplesInHeap--;
		Assert( node->numTuplesInHeap >= 0 );
	
		return ExecStoreTuple(tuple, slot, InvalidBuffer, true /*shouldFree*/);
	}
	else    /* tuple is null */
	{
		AssertState(node->noMoreChildTuples);
		while (!node->sentEndOfStream)
		{
			/* Try to complete the End-Of-Stream send.  We know this
			 * is the broadcast scenario because we won't have a
			 * pending EOS send if we are the single receiver of a
			 * order-preserving motion.  (This is because a single
			 * receiver won't be sending to anybody.)
			 */
			AssertState(motion->numOutputSegs == 0);
			
			sendRC = SendEndOfStream(motion->motionID, BROADCAST_SEGIDX);
			node->sentEndOfStream = (sendRC == SEND_COMPLETE);
		}
		
		return NULL;
	}
	
}



void execMotionSortedReceiverFirstTime(MotionState *node)
{
	HeapTuple inputTuple;
	Motion 	  *motion = (Motion *)node->ps.plan;
    int 	  mySegIdx;
    bool      *tupsReceived;
    int 	  i;

    ReceiveReturnCode recvRC;

    mySegIdx = Cdb_segment;

    /* INITIALIZATION -
     *
     * We need to get a tuple from every sender, and stick it into the heap.
     *
     * To keep all the initialize-logic in one place, we will pull a single
     * tuple from our child plan-node, IF noMoreChildTuples is false, AND if
     * this segment is in the output segindex array.  (The second condition is
     * true by default, if we are calling this function.  Thus, we assert it
     * too.)
     */


    /* These are all kinda different ways of saying the same thing... */
    AssertState(!node->noMoreAMSTuples);
    AssertState(motion->numOutputSegs == 0 ||
                motion->outputSegIdx[0] == mySegIdx);


	/* Now we iterate over the tuple-heap, and try to receive a tuple for
	 * each entry that doesn't yet have a tuple.
	 */	
	tupsReceived = (bool *) palloc0(sizeof(bool) * motion->numInputSegs);
		

	for (i = 0; i < motion->numInputSegs; i++)
	{
		if (motion->inputSegIdx[i] == mySegIdx || tupsReceived[i])
			continue;
		
		
		//don't add 1 since route0 = segid0
		recvRC = RecvTupleFrom(motion->motionID, &inputTuple,
			motion->inputSegIdx[i]);
		
		if (recvRC != NO_TUPLE)
		{
			SetCdbTupleHeapInitialValue(node->tupleheap, inputTuple,
				motion->inputSegIdx[i]);

			tupsReceived[i] = true;

			if (recvRC == GOT_TUPLE)
			{
				node->numTuplesFromAMS++;
				node->numTuplesInHeap++;
			}
		}
		else
		{
			/* TODO: this shouldn't ever happen. just leaving this as a check
			 * 		 to make sure that's really true. at some point this should
			 *		 be cleaned up a bit.
			 */
			elog( FATAL, "RecvTupleFrom returned recvRC of: %d which is invalid",
					recvRC );
		}
	}

	/* Be nice and clean up. */
	pfree(tupsReceived);
	
	node->tupleheapReady = true;

}


/* ----------------------------------------------------------------
 *		ExecInitMotion
 *
 * NOTE: have to be a bit careful, estate->es_cur_slice_idx is not the
 *		 ultimate correct value that it should be on the QE. this happens
 *		 after this call in cdbexec.c.  This is ok since we don't need it,
 *		 but just be aware before you try and use it here.
 * ----------------------------------------------------------------
 */
MotionState *
ExecInitMotion(Motion *node, EState *estate)
{
	MotionState* motionstate;
	bool readFromChild, readFromAMS;
	int i;
	TupleDesc tupDesc;
	Slice *parentSlice;
	Slice *thisSlice;
	int prior_es_cur_slice_idx;
	int prior_es_cur_root_idx;

	SO_printf("ExecInitMotion: initializing motion node\n");
	
    AssertState(Cdb_role != CDB_ROLE_UTILITY);
	
	prior_es_cur_slice_idx = estate->es_cur_slice_idx; /* save */
	prior_es_cur_root_idx = estate->es_cur_root_idx; /* save */

	/*
	 * create state structure
	 */
	motionstate = makeNode(MotionState);
	motionstate->ps.plan = (Plan *) node;
	motionstate->ps.state = estate;
	
	/*
	 * CDB Slice Plan:
	 *   QD must initialize the global slice table.
	 *   QE must fill in map from motionID to MotionState node.
	 */
	if ( Cdb_role == CDB_ROLE_DISPATCH )
	{
		MemoryContext oldcxt;
		oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		
		parentSlice = (Slice*)list_nth(estate->es_sliceTable->slices, estate->es_cur_slice_idx);
		
		Assert(IsA(parentSlice, Slice));
		Assert(parentSlice->sliceIndex == estate->es_cur_slice_idx);
		Assert(0 < node->motionID && node->motionID <= estate->es_sliceTable->nMotions);
		
		thisSlice = (Slice*)list_nth(estate->es_sliceTable->slices, node->motionID);
		
		Assert(IsA(thisSlice, Slice));
		Assert(thisSlice->sliceIndex == -1);

		thisSlice->sliceIndex = node->motionID;
		thisSlice->rootIndex = parentSlice->rootIndex;
		
		/* The gang beneath a Motion will be a reader, but its size must
		 * be determined. */
		thisSlice->gangType = GANGTYPE_READER;
		Assert ( node->plan.lefttree->flow != NULL );
		switch ( node->plan.lefttree->flow->base_flotype )
		{
			case FLOW_SINGLETON:
				thisSlice->gangSize = 1;
				break;
				
			case FLOW_REPLICATED:
			case FLOW_PARTITIONED:
				thisSlice->gangSize = getCdbSegmentCount();
				break;
				
			default:
				/* Assert FALSE -- Above are only values we should see. */
				Assert( node->plan.lefttree->flow->flotype == FLOW_SINGLETON );
		}
		
		thisSlice->parentIndex = estate->es_cur_slice_idx;		
		parentSlice->children = lappend_int(parentSlice->children, thisSlice->sliceIndex);
		
		estate->es_cur_slice_idx = thisSlice->sliceIndex;
		
		MemoryContextSwitchTo(oldcxt);
	}
	else if ( Cdb_role == CDB_ROLE_EXECUTE )
	{
		Assert( estate->es_slice_map != NULL && estate->es_slice_map[0] == NULL );
		estate->es_slice_map[node->motionID] = motionstate;
	}

    readFromChild = (Cdb_role != CDB_ROLE_DISPATCH);
    readFromAMS = setReadFromAMS(node);

    /* Short-circuit these values right away, if we shouldn't read from these
     * sources of tuples.
     */
	motionstate->noMoreChildTuples = !readFromChild;
	motionstate->noMoreAMSTuples = !readFromAMS;

    /* Query Dispatcher top motion-node doesn't send anything to qExecs. */
    motionstate->sentEndOfStream = (Cdb_role == CDB_ROLE_DISPATCH);
    Assert(motionstate->noMoreChildTuples || !motionstate->sentEndOfStream);

	motionstate->numTuplesFromChild = 0;
	motionstate->numTuplesToAMS     = 0;
	motionstate->numTuplesFromAMS   = 0;
	motionstate->numTuplesToParent  = 0;
	motionstate->numTuplesToDeepSix = 0;
	motionstate->numTuplesInHeap    = 0;

	motionstate->stopRequested = false;
	
    elog(DEBUG2, "Motion-node %d type %s:  read from child = %s, from AMS = %s",
        node->motionID,
        (node->motionType == MOTIONTYPE_HASH ? "hash" : "fixed"),
        (readFromChild ? "true" : "false"),
        (readFromAMS ? "true" : "false")
        );

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &motionstate->ps);

#define MOTION_NSLOTS 1

	/*
	 * tuple table initialization
	 *
	 */
	ExecInitResultTupleSlot(estate, &motionstate->ps);

	/*
	 * initializes child nodes
	 */
	outerPlanState(motionstate) = ExecInitNode(outerPlan(node), estate);
	
	/*
	 * restore state that might have changed in recursion for our caller
	 * (and for ourself).
	 */
	 estate->es_cur_slice_idx =	prior_es_cur_slice_idx;
	 estate->es_cur_root_idx =	prior_es_cur_root_idx;
	
	/*
	 * initialize tuple type.  no need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecAssignResultTypeFromOuterPlan(&motionstate->ps);
	motionstate->ps.ps_ProjInfo = NULL;
	motionstate->hashExprState = NULL;
	motionstate->cdbhash = NULL;
		
	if ( node->motionType == MOTIONTYPE_HASH )
	{
		int			nkeys;
		
		/*
		 * Get info about the hash functions to be used for each hash key.
		 */
		Assert( node->hashDataTypes != NULL );
		Assert( node->numHashSegs > 0 );
		
		nkeys = list_length(node->hashDataTypes);
		motionstate->hashExprState = (List *)ExecInitExpr( (Expr *)node->hashExpr,
														 (PlanState *) motionstate);

		/*
		 * Create hash API reference
		 */
		motionstate->cdbhash = makeCdbHash(node->numHashSegs, HASH_FNV_1);

		/*
		 * Create bit vector of masks if this is a "deep-six" hash motion node
		 */
		if ( node->hashToSelfOnly )
		{
			Assert( !node->sendSorted );

			motionstate->hashSegIdxMask = palloc0( node->numHashSegs * sizeof(bool) );
			for ( i=0; i<node->numHashSegs; i++ )
			{
				if ( node->hashSegIdx[i] == Cdb_segment )
					motionstate->hashSegIdxMask[i] = true;
				else
					motionstate->hashSegIdxMask[i] = false;
			}
		}

		/*
		 * Create buckets to hold counts of tuples hashing to each destination segindex (for debugging purposes)
		 */
		motionstate->numTuplesByHashSegIdx = (int *)palloc0( node->numHashSegs * sizeof(int) );
	}
	else if ( node->motionType == MOTIONTYPE_FIXED )
	{
		/*
		 * Create bit vector of masks if this is a "deep-six" fixed motion node
		 */
		if ( node->fixedToSelfOnly )
		{
			Assert( node->numOutputSegs > 0 );
			Assert( !node->sendSorted );

			motionstate->fixedSegIdxMask = palloc0( node->numOutputSegs * sizeof(bool) );
			for ( i=0; i<node->numOutputSegs; i++ )
			{
				if ( node->outputSegIdx[i] == Cdb_segment )
					motionstate->fixedSegIdxMask[i] = true;
				else
					motionstate->fixedSegIdxMask[i] = false;
			}
		}
	}

	if ( node->sendSorted )
	{
		Assert( node->numSortCols > 0 );
		Assert( node->numInputSegs > 0 );
		Assert( node->inputSegIdx != NULL );

		motionstate->sortFunctions = (FmgrInfo *) palloc0(node->numSortCols * sizeof(FmgrInfo));
		motionstate->sortFnKinds = (SortFunctionKind *) palloc0(node->numSortCols * sizeof(SortFunctionKind));

		for ( i=0; i<node->numSortCols; i++ )
		{
			RegProcedure sortFunction;

			AssertArg(node->sortOperators[i] != 0);
			AssertArg(node->sortColIdx[i] != 0);

			/* select a function that implements the sort operator */
			SelectSortFunction(node->sortOperators[i], &sortFunction, &motionstate->sortFnKinds[i]);

			fmgr_info(sortFunction, &motionstate->sortFunctions[i]);
		}
		
		
		motionstate->tupleheap = CreateCdbTupleHeap( node->numInputSegs,
													motionstate,
													node);
		motionstate->tupleheapReady = false;
		motionstate->segindexNext = node->inputSegIdx[0];
	}
	
    /*
     * Perform per-node initialization in the motion layer.
     */
	tupDesc = ExecGetResultType(&motionstate->ps);

    InitMotionLayerNode(node->motionID, node->sendSorted,
    		ExecGetResultType(&motionstate->ps));

	SO1_printf("ExecInitMotion: %s\n",
			   "motion node initialized");

	return motionstate;
}

/* ----------------------------------------------------------------
 *		ExecCountSlotsMotion
 * ----------------------------------------------------------------
 */
int
ExecCountSlotsMotion(Motion *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) +
		MOTION_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndMotion(node)
 * ----------------------------------------------------------------
 */
void
ExecEndMotion(MotionState *node)
{
	Motion *motion;
    uint16 motNodeID;
	int i;

	SO1_printf("ExecEndMotion: %s\n",
			   "shutting down motion node");

    motNodeID = ((Motion *) node->ps.plan)->motionID;

	ExecFreeExprContext(&node->ps);
	
	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

    motion = (Motion *)node->ps.plan;
	Assert( motion != NULL );

	elog( DEBUG1,
		"Motion Node %d counts: \n"
		"numTuplesFromChild: %d\n"
		"numTuplesFromAMS: %d\n"
		"numTuplesToAMS: %d\n"
		"numTuplesToParent: %d\n"
		"numTuplesToDeepSix: %d\n"
		"numTuplesInHeap: %d\n",
		motNodeID,
		node->numTuplesFromChild,
		node->numTuplesFromAMS,
		node->numTuplesToAMS,
		node->numTuplesToParent,
		node->numTuplesToDeepSix,
		node->numTuplesInHeap
		);

	if ( motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs == 0 )
	{
		Assert(node->numTuplesFromChild == node->numTuplesToAMS);
		Assert(node->numTuplesFromAMS == node->numTuplesToParent);
	}
	else
	{
		
		/* I notice that it is wrong, at least, in the case of a LIMIT which calls 
		 * ExecStopMotion which (correctly) sets node->stopRequested to true.
		 * So I made it conditional as a stopgap.  I doubt this is the correct
		 * permanent fix.  Brian 8/2005 
		 */
		Assert(node->stopRequested || node->numTuplesFromChild == node->numTuplesToAMS + node->numTuplesToDeepSix + node->numTuplesInHeap);
		/* Hmmm... It this " - node->numTuplesInHeap" correct? */
		Assert(node->numTuplesFromAMS - node->numTuplesInHeap == node->numTuplesToParent);
	}

	if ( motion->motionType == MOTIONTYPE_HASH )
	{
		if ( node->numTuplesByHashSegIdx != NULL )
		{
			elog( DEBUG1, "Motion Node %d Hash Bucket Distribution of child tuples: ", motNodeID );
			for ( i=0; i<motion->numHashSegs; i++ )
			{
				elog( DEBUG1, "Bucket %d : %d", i, node->numTuplesByHashSegIdx[i] );
			}

			pfree( node->numTuplesByHashSegIdx );
			node->numTuplesByHashSegIdx = NULL;
		}

		if ( node->hashSegIdxMask != NULL )
			pfree( node->hashSegIdxMask );
	}
	else if ( motion->motionType == MOTIONTYPE_FIXED )
	{
		if ( node->fixedSegIdxMask != NULL )
			pfree( node->fixedSegIdxMask );
	}

	if ( node->tupleheap != NULL )
	{
		DestroyCdbHeap(node->tupleheap);
		node->tupleheap = NULL;
	}

	if ( node->cdbhash != NULL )
	{
		pfree( node->cdbhash );
		node->cdbhash = NULL;
	}

    /*
     * Free up this motion node's resources in the Motion Layer.
     *
     * TODO: For now, we don't flush the comm-layer.  NO ERRORS DURING AMS!!!
     */
    EndMotionLayerNode(motNodeID, /* flush-comm-layer */ false);

	SO1_printf("ExecEndMotion: %s\n",
			   "motion node shutdown");
}



/*=========================================================================
 * HELPER FUNCTIONS
 */



/*
 * Experimental code that will be replaced later with new hashing mechanism
 */
uint32 evalHashKey(ExprContext *econtext, List *hashkeys, List* hashtypes, CdbHash *h)
{
	ListCell   *hk;
    ListCell   *ht;
	MemoryContext oldContext;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	cdbhashinit(h);

	forboth(hk, hashkeys, ht, hashtypes)
	{
		ExprState  *keyexpr = (ExprState *) lfirst(hk);
		Datum		keyval;
		bool		isNull;

		/*
		 * Get the attribute value of the tuple
		 */
		keyval = ExecEvalExpr(keyexpr, econtext, &isNull, NULL);

		/*
		 * Compute the hash function
		 */
		if (!isNull)			/* treat nulls as having hash key 0 */
			cdbhash(h, keyval, lfirst_oid(ht));
		else
			cdbhashnull(h);
	}
	
	MemoryContextSwitchTo(oldContext);
	
	return cdbhashreduce(h);
}


/* Helper function to determine whether the motion node needs to read from the
 * Motion Layer (aka AMS).
 */
bool setReadFromAMS(Motion *node)
{
    int i, mySegIdx;
    bool readFromAMS = true;

    AssertArg(node != NULL);

    /* TODO:  Figure out whether we need is-primary here. */
    mySegIdx = Cdb_segment;

    switch (node->motionType)
    {
        case MOTIONTYPE_HASH:
            AssertState(node->numHashSegs > 0);
            AssertState(node->hashSegIdx != NULL);

            readFromAMS = false;
            for (i = 0; i < node->numHashSegs; i++)
            {
                if (node->hashSegIdx[i] == mySegIdx)
                {
                    readFromAMS = true;
                    break;
                }
            }

            break;

        case MOTIONTYPE_FIXED:
            AssertState(node->numOutputSegs >= 0);
            AssertState(node->numOutputSegs == 0 || node->outputSegIdx != NULL);

            if (node->numOutputSegs == 0)
            {
                readFromAMS = true;
            }
            else
            {
                readFromAMS = false;
                for (i = 0; i < node->numOutputSegs; i++)
                {
                    if (node->outputSegIdx[i] == mySegIdx)
                    {
                        readFromAMS = true;
                        break;
                    }
                }
            }

            break;

        default:
            AssertState(false);
    }

    return readFromAMS;
}


void doSendEndOfStream( Motion *motion, MotionState *node )
{
	int targetRoute;
	SendReturnCode 	sendRC;
	
	/* We have no more child tuples, but we have not successfully
	 * sent an End-of-Stream token yet.
	 *
	 * NOTE:  If there is only one destination, we do not broadcast
	 *        the end-of-stream; we only send it to the single dest.
	 */
	if (motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs == 1)
		targetRoute = 0;
	else
		targetRoute = BROADCAST_SEGIDX;
	
	sendRC = SendEndOfStream(motion->motionID, targetRoute);
	node->sentEndOfStream = (sendRC == SEND_COMPLETE);
	elog(DEBUG4, "motionID=%d node->sentEndOfStream=%d", motion->motionID, node->sentEndOfStream);
}


/*
 * A crufty confusing part of the current code is how contentId is used within
 * the motion structures and then how that gets translated to targetRoutes by
 * this motion nodes.
 *
 * WARNING: There are ALOT of assumptions in here about how the motion node
 *			instructions are encoded into motion and stuff.
 *
 * There are 3 types of sending that can happen here:
 *
 *  FIXED - sending to a single process.  the value in node->fixedSegIdxMask[0]
 *		    is the contentId of who to send to.  But we can actually ignore that
 *			since now with slice tables, we should only have a single CdbProcess
 *			that we could send to for this motion node.
 *
 *
 *  BROADCAST - actually a subcase of FIXED, but handling is simple. send to all
 *			    of our routes.
 *
 *  HASH -  maps hash values to segid.  this mapping is 1->1 such that a hash
 *			value of 2 maps to contentid of 2 (for now).  Since we can't ever
 *			use Hash to send to the QD, the QD's contentid of -1 is not an issue.
 *			Also, the contentid maps directly to the routeid.
 *
 */
void doSendTuple(Motion *motion, MotionState *node, TupleTableSlot *outerTupleSlot)
{
	int16 targetRoute;
	
	SendReturnCode sendRC;
	ExprContext *econtext = node->ps.ps_ExprContext;
	
	/* We got a tuple from the child-plan. */
	node->numTuplesFromChild++;

	if (motion->motionType == MOTIONTYPE_FIXED)
	{
		if (motion->numOutputSegs == 0)         /* Broadcast */
		{						
			targetRoute = BROADCAST_SEGIDX;			
		}
		else									/* Fixed Motion. */
		{
			AssertState(motion->outputSegIdx != NULL);
			
			/* TODO:  We can only support one output-segment right now! */
			AssertState(motion->numOutputSegs == 1);

			if( node->fixedSegIdxMask != NULL && !node->fixedSegIdxMask[0] )
			{
				/* this tuple will be deep-sixed. */
				node->numTuplesToDeepSix++;
				return;
			}
			else
			{
				/* Actually, since we can only send to a single output segment
				 * here, we are guarantted that we only have a single targetRoute
				 * setup that we could possibly send to.  So we can cheat and
				 * just fix the targetRoute to 0 (the 1st route).
				 */
				targetRoute = 0;				
			}
		}
	}
	else    /* Hash motion node */
	{
		/* hashSegIdx structure doesn not include support for the QD.
		 * so 0 maps to segID with contentId = 0
		 */
		uint32 hval = 0;
		
		Assert( motion->numHashSegs > 0 && motion->hashSegIdx != NULL );

		econtext->ecxt_outertuple = outerTupleSlot;
		hval = evalHashKey(econtext, node->hashExprState,
						   motion->hashDataTypes, node->cdbhash);
		targetRoute = motion->hashSegIdx[ hval ];

		Assert( node->numTuplesByHashSegIdx != NULL );
		node->numTuplesByHashSegIdx[ hval ]++;

		if(node->hashSegIdxMask != NULL && !node->hashSegIdxMask[hval])
		{
			/* this tuple will be deep-sixed! */
			node->numTuplesToDeepSix++;
			return;
		}
	}
	
	/* send the tuple out. */
	sendRC = SendTuple(motion->motionID, outerTupleSlot->val, targetRoute);
	
	Assert(sendRC == SEND_COMPLETE || sendRC == STOP_SENDING);

	if (sendRC == SEND_COMPLETE)
		node->numTuplesToAMS++;
	else if (sendRC == STOP_SENDING)
		node->stopRequested = true;

	return;
}

/* 
 * Mark this node as "stopped." When ExecProcNode() is called on a
 * stopped motion node it should behave as if there are no tuples
 * available.
 *
 * ExecProcNode() on a stopped motion node should also notify the
 * "other end" of the motion node of the stoppage.
 */
void ExecStopMotion(MotionState *node)
{
    AssertArg(node != NULL);

	node->stopRequested = true;
}
