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

/* 
 * Helper functions 
 */ 
static void trySendCurrentTuple(MotionState *node);
static TupleTableSlot *execMotionUnsorted(MotionState *node);
static TupleTableSlot *execMotionSorted(MotionState *node);
static void execMotionSortedFirstTime(MotionState *node);
static uint32 evalHashKey(ExprContext *econtext, List *hashkeys, Oid *hashOids, CdbHash *h);
static bool setReadFromAMS(Motion *node);

/* ----------------------------------------------------------------
 *		ExecMotion
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMotion(MotionState *node)
{
#ifndef CDB_NOMOTION
	Motion *motion = (Motion *)node->ps.plan;
    
    if (motion->sendSorted && !node->noMoreAMSTuples)
        return execMotionSorted(node);
    else
        return execMotionUnsorted(node);
#else
	TupleTableSlot *slot;
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerNode;
	HeapTuple tuple;
	ExprContext *econtext = node->ps.ps_ExprContext;
	Motion *motion = (Motion *)node->ps.plan;

	SO1_printf("ExecMotion: %s\n",
			   "entering routine");

	/*
	 * grab TupleTableSlot from our child.
	 */
	outerNode = outerPlanState(node);
	
	outerTupleSlot = ExecProcNode(outerNode);
	if ( TupIsNull(outerTupleSlot) )
		return NULL;
		
	/*
	 * make a copy of the tuple
	 */
	tuple = heap_copytuple(outerTupleSlot->val);
	
	slot = node->ps.ps_ResultTupleSlot;
	
	/*
	 * store it in our result slot and return this.
	 */
	return ExecStoreTuple(tuple, slot, InvalidBuffer, true);

#endif
}


TupleTableSlot *
execMotionUnsorted(MotionState *node)
{
	TupleTableSlot *slot;
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerNode;
	HeapTuple tuple;
    bool shouldFree;
	ExprContext *econtext = node->ps.ps_ExprContext;
	Motion *motion = (Motion *)node->ps.plan;
    
    int mySegIdx;               /* TODO:  This too. */
    bool tupleInFlight;
    SendReturnCode sendRC;
    ReceiveReturnCode recvRC;
    
	SO1_printf("ExecMotion: %s\n",
			   "entering routine");

    AssertState(motion->motionType == MOTIONTYPE_HASH ||
        (motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs <= 1));
    
    mySegIdx = CdbIdentity.segindex;

    tuple = NULL;
    tupleInFlight = (node->tupSendCtxt.heap_tuple != NULL);
    do
    {
		CHECK_FOR_INTERRUPTS();

        /* If a tuple is in-flight, try to continue the send operation. */

        if (tupleInFlight)
        {
            trySendCurrentTuple(node);

            /* We could look at sendRC, but we want to use the same
             * condition throughout.
             */
            tupleInFlight = (node->tupSendCtxt.heap_tuple != NULL);
        }
        else if (node->noMoreChildTuples && !node->sentEndOfStream)
        {
            int tgtSegIdx;
            
            /* We have no more child tuples, but we have not successfully
             * sent an End-of-Stream token yet.
             *
             * NOTE:  If there is only one destination, we do not broadcast
             *        the end-of-stream; we only send it to the single dest.
             */
            if (motion->motionType == MOTIONTYPE_FIXED &&
                motion->numOutputSegs == 1)
            {
                tgtSegIdx = motion->outputSegIdx[0];
            }
            else
            {
                tgtSegIdx = BROADCAST_SEGIDX;
            }
            
            sendRC = SendEndOfStream(motion->motionID, tgtSegIdx);
            node->sentEndOfStream = (sendRC == SEND_COMPLETE);
			elog(DEBUG4, "motionID=%d node->sentEndOfStream=%d", motion->motionID, node->sentEndOfStream);
        }

        /*
         * Next, try to read a tuple from AMS.  Tuples we read from AMS are
         * ALWAYS for us.
         */

        if (!node->noMoreAMSTuples)
        {
            /* We do not need to preserve the order of incoming tuples. */
            
            recvRC = RecvTuple(motion->motionID, &tuple);
            if (recvRC == END_OF_STREAM)
            {
				elog(DEBUG4, "motionID=%d saw END_OF_STREAM", motion->motionID );
                node->noMoreAMSTuples = true;
            }
            else if (recvRC == GOT_TUPLE)
            {
                node->numTuplesFromAMS++;
                shouldFree = true;      /* TODO:  Pass up from RecvTuple. */
            }
        }

        /*
         * Finally, if we didn't receive a tuple, and there's no tuple
         * in-flight, we try to read from the child-plan.
         */
        if (!node->noMoreChildTuples && !tupleInFlight && tuple == NULL)
        {
            /*
             * grab TupleTableSlot from our child.
             */
            outerNode = outerPlanState(node);
            
            outerTupleSlot = ExecProcNode(outerNode);
            if ( TupIsNull(outerTupleSlot) )
            {
                /* Child plan says "No more tuples!" */
                node->noMoreChildTuples = true;
            }
            else
            {
                /* We got a tuple from the child-plan. */
                
                node->numTuplesFromChild++;
            
                /* TODO:  Decide what to do with the child's tuple. */
                
                if (motion->motionType == MOTIONTYPE_FIXED)
                {
                    if (motion->numOutputSegs == 0)         /* Broadcast */
                    {
                        /* We broadcast the tuple, AND we hand it up to our
                         * parent.
                         */
                        tuple = outerTupleSlot->val;
                        shouldFree = false;
                        node->tupSendCtxt.heap_tuple = tuple;
                        node->tupSendCtxt.target_segment = BROADCAST_SEGIDX;
                    }
                    else
                    {
                        AssertState(motion->outputSegIdx != NULL);
                        
                        /* TODO:  We can only support one output-segment right now! */
                        AssertState(motion->numOutputSegs == 1);

						/*
						* If hashSegIdxMask is non-NULL, check to see whether
						* hashing to the target_segindex has been turned off.
						*/
						if ( node->fixedSegIdxMask != NULL && !node->fixedSegIdxMask[0] )
						{
							// this tuple will be deep-sixed!
							node->numTuplesToDeepSix++;
						}
						else
						{
							if (motion->outputSegIdx[0] == mySegIdx)
							{
								/* We are the receiver, so pass-thru the tuple. */
								tuple = outerTupleSlot->val;
								shouldFree = false;
							}
							else
							{
								/* The target is someone else.  Set up the send. */
								node->tupSendCtxt.heap_tuple = outerTupleSlot->val;
								node->tupSendCtxt.target_segment =
									motion->outputSegIdx[0];

								Assert(tuple == NULL);
							}
						}
                    }
                }
                else    /* Hash motion node */
                {
					// Get hash value of hash expressions
					Assert( motion->numHashSegs > 0 && motion->hashSegIdx != NULL );
					uint32 hval = 0;
					uint16 target_segindex;
					
					econtext->ecxt_outertuple = outerTupleSlot;
					hval = evalHashKey(econtext, node->hashExprState, node->hashOids, node->cdbhash);
					target_segindex = motion->hashSegIdx[ hval  ];

					Assert( node->numTuplesByHashSegIdx != NULL );
					node->numTuplesByHashSegIdx[ hval ]++;

					/*
					 * If hashSegIdxMask is non-NULL, check to see whether
					 * hashing to the target_segindex has been turned off.
					 */
					if ( node->hashSegIdxMask != NULL && !node->hashSegIdxMask[hval] )
					{
						// this tuple will be deep-sixed!
						node->numTuplesToDeepSix++;
					}
					else
					{
						if ( target_segindex == mySegIdx )
						{
							/* We are the receiver, so pass-thru the tuple. */
							tuple = outerTupleSlot->val;
							shouldFree = false;
						}
						else
						{
							/* The target is someone else.  Set up the send. */
							node->tupSendCtxt.heap_tuple = outerTupleSlot->val;
							node->tupSendCtxt.target_segment = target_segindex;

							Assert(tuple == NULL);
						}
					}
                }
                
                /* If we have to send this tuple, kick off that process here. */
                
                if (node->tupSendCtxt.heap_tuple != NULL)
                {
                    trySendCurrentTuple(node);
                    
                    /* We could look at sendRC, but we want to use the same
                     * condition throughout.
                     */
                    tupleInFlight = (node->tupSendCtxt.heap_tuple != NULL);
                }
            }
        }
//		pg_usleep(1);
    }
    while (tuple == NULL &&
        !(node->noMoreAMSTuples && node->noMoreChildTuples && node->sentEndOfStream));

    if (tuple != NULL)
    {
        /*
         * store it in our result slot and return this.
         */
    
        slot = node->ps.ps_ResultTupleSlot;
        
        node->numTuplesToParent++;
    
        return ExecStoreTuple(tuple, slot, InvalidBuffer, shouldFree);
    }
    else
    {
        return NULL;
    }
}


TupleTableSlot *
execMotionSorted(MotionState *node)
{
	TupleTableSlot *slot;
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerNode;
	HeapTuple tuple, inputTuple;
    bool shouldFree;
	Motion *motion = (Motion *)node->ps.plan;
    
    int mySegIdx;               /* TODO:  This too. */
    SendReturnCode sendRC;
    ReceiveReturnCode recvRC;
    
    CdbTupleHeapInfo *tupHeapInfo;
    
	SO1_printf("execMotionSorted: %s\n",
			   "entering routine");

    AssertState(motion->motionType == MOTIONTYPE_FIXED &&
                motion->numOutputSegs <= 1);
    AssertState(motion->sendSorted);
    AssertState(node->tupleheap != NULL);
    
    mySegIdx = CdbIdentity.segindex;
    
    /* NOTE:  THIS IS TRICKY!!!
     *
     * In an order-preserving fixed motion node, segments that are receiving
     * tuples are basically passing up tuples from the tuple-heap, and not
     * directly from the AMS or child-plan.
     *
     * There are two cases - broadcast order-preserving fixed motion node, or
     * single-destination order-preserving fixed motion node.
     *
     * In the second case, all of the "send only" segments will act like
     * NORMAL motion nodes, which means this function won't be called.  (They
     * will do the proper end-of-stream processing, and all that stuff.)
     *
     * In the first case, we know that we will only have broadcast sends.
     */
    
    /* These are all kinda different ways of saying the same thing... */
    AssertState(!node->noMoreAMSTuples);
    AssertState(motion->numOutputSegs == 0 ||
                motion->outputSegIdx[0] == mySegIdx);
    
    if (!node->tupleheapReady)
    {
        execMotionSortedFirstTime(node);
    }
    else
    {
        /* There will be one entry in the heap that is missing a tuple.  We
         * need to fill in that tuple.  If the segment without a tuple is not
         * us, we unfortunately have to loop until we do get a tuple, since
         * we can't hand up a tuple until we have one from every sender.
         */
        
        if (node->segindexNext != mySegIdx)
        {
            /* The segment missing a tuple is remote, so try to receive a
             * tuple from them.  (Also, if a tuple is in the middle of being
             * sent, we continue the send.)
             */

            /* TODO:  This is an awfully tight loop - not very friendly to
             *        other processes!  MAKE IT A BLOCKING CALL.
             */
            do
            {
                CHECK_FOR_INTERRUPTS();
                
                if (node->tupSendCtxt.heap_tuple != NULL)
                {
                    trySendCurrentTuple(node);
                }
                else if (node->noMoreChildTuples && !node->sentEndOfStream)
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
                
                recvRC = RecvTupleFrom(motion->motionID, &inputTuple, node->segindexNext);
            }
            while (recvRC == NO_TUPLE);
            
            if (recvRC == GOT_TUPLE)
                node->numTuplesFromAMS++;
            
            MergeNewValueIntoCdbTupleHeap(node->tupleheap, inputTuple,
                node->segindexNext);
        }
        else
        {
            /* This segment is the one missing a tuple. */
            
            /* Unfortunately, if there is currently a tuple being sent out, it
             * must be finished before we can pull up the next tuple.
             *
             * Fortunately, this only happens when tuples are being broadcast.
             */
            if (motion->numOutputSegs == 0)
            {
                /* The helper function will update the send-context once the
                 * tuple has completed sending, so the loop will terminate.
                 */
                
                /* TODO:  This is an awfully tight loop - not very friendly to
                 *        other processes!  MAKE IT A BLOCKING CALL.
                 */
                while (node->tupSendCtxt.heap_tuple != NULL)
                {
                    CHECK_FOR_INTERRUPTS();
                    
                    trySendCurrentTuple(node);
                }
            }
            
            /*
             * OK, now we can pull a tuple from the child plan.
             */
            
            AssertState(!node->noMoreChildTuples);
            
            /* grab TupleTableSlot from our child. */
            outerNode = outerPlanState(node);
            
            outerTupleSlot = ExecProcNode(outerNode);
            if ( TupIsNull(outerTupleSlot) )
            {
                /* Child plan says "No more tuples!" */
                node->noMoreChildTuples = true;
                
                MergeNewValueIntoCdbTupleHeap(node->tupleheap, NULL, mySegIdx);
                
                /* Send an End-Of-Stream token, if it is broadcast. */
                if (motion->numOutputSegs == 0)
                {
                    sendRC = SendEndOfStream(motion->motionID, BROADCAST_SEGIDX);
                    node->sentEndOfStream = (sendRC == SEND_COMPLETE);
                }
            }
            else
            {
                /* We got a tuple from the child-plan. */
                
                node->numTuplesFromChild++;
                
                MergeNewValueIntoCdbTupleHeap(node->tupleheap,
                    outerTupleSlot->val, mySegIdx);
                
                /* We only send the tuple here if it is broadcast.  (Remember
                 * that we previously asserted that either it is broadcast, or
                 * this segment is the receiver of all tuples.)
                 */
                if (motion->numOutputSegs == 0)
                {
                    node->tupSendCtxt.heap_tuple = outerTupleSlot->val;
                    node->tupSendCtxt.target_segment = BROADCAST_SEGIDX;
                }
                
                /* Try the send here, since it might be a while before we try
                 * again.
                 */
                trySendCurrentTuple(node);
            }
        }
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
    
        return ExecStoreTuple(tuple, slot, InvalidBuffer, shouldFree);
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
        
        /* This is just a sanity check. */
        if (motion->numOutputSegs == 0)
        {
            /* If the tuple-heap hands back a NULL value, then that means that we
             * don't have a tuple in flight.  This is because we cannot get the
             * next tuple from the child plan until any in-progress send has
             * completed.  Thus, when the child plan says it has no more tuples,
             * we KNOW that all in-progress sends have completed.  Because the
             * child plan must return NULL before the tuple-heap returns NULL, we
             * know there should be no tuple-sends in flight at this point.
             */
            AssertState(node->tupSendCtxt.heap_tuple == NULL);
            
            /* We also must have sent our end-of-stream token already, since the
             * heap cannot report NULL as the next tuple until it has received
             * end-of-stream values from everyone.
             */
            AssertState(node->sentEndOfStream == true);
        }

        return NULL;
    }
}



/* Helper function to try to send out the current tuple.
 *
 * NOTE:  node->tupSendCtxt.heap_tuple MUST NOT BE NULL!
 */
void trySendCurrentTuple(MotionState *node)
{
    Motion *motion = (Motion *)node->ps.plan;
    SendReturnCode sendRC;

    AssertArg(node->tupSendCtxt.heap_tuple != NULL);
    
    sendRC = SendTuple(motion->motionID, &node->tupSendCtxt);
    if (sendRC == SEND_COMPLETE)
    {
        node->numTuplesToAMS++;
        
        /* Clean up the send-context. */
        /* TODO:  Donnie should create a helper for this. */
        
        clearTCList(node->tupSendCtxt.tuple_chunk_list);
        bzero(&node->tupSendCtxt, sizeof(node->tupSendCtxt));
    }
}


void execMotionSortedFirstTime(MotionState *node)
{
	TupleTableSlot *outerTupleSlot;
	PlanState *outerNode;
	HeapTuple inputTuple;
	Motion *motion = (Motion *)node->ps.plan;
    int mySegIdx;
    bool *tupsReceived;
    bool allReady;
    int i;
    ReceiveReturnCode recvRC;
    
    mySegIdx = CdbIdentity.segindex;
    
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
    
    if (!node->noMoreChildTuples)
    {
        /*
         * grab TupleTableSlot from our child.
         */
        outerNode = outerPlanState(node);
        
        outerTupleSlot = ExecProcNode(outerNode);
        if ( TupIsNull(outerTupleSlot) )
        {
            /* Child plan says "No more tuples!" */
            node->noMoreChildTuples = true;
            
            SetCdbTupleHeapInitialValue(node->tupleheap, NULL, mySegIdx);
        }
        else
        {
            /* We got a tuple from the child-plan. */
            
            node->numTuplesFromChild++;
            
            SetCdbTupleHeapInitialValue(node->tupleheap, outerTupleSlot->val,
                mySegIdx);
            
            /* We only send the tuple here if it is broadcast.  (Remember that
             * we previously asserted that either it is broadcast, or this
             * segment is the receiver of all tuples.)
             */
            if (motion->numOutputSegs == 0)
            {
                node->tupSendCtxt.heap_tuple = outerTupleSlot->val;
                node->tupSendCtxt.target_segment = BROADCAST_SEGIDX;
            }
            
            /* Don't really need to kick off the send here, since we will do
             * it in the loop below.
             */
        }
    }
    
    /* Now we iterate over the tuple-heap, and try to receive a tuple for
     * each entry that doesn't yet have a tuple.
     */
    
    tupsReceived = (bool *) palloc0(sizeof(bool) * motion->numInputSegs);
    do
    {
		CHECK_FOR_INTERRUPTS();
        
        /* Unfortunately, if we have a send in progress, we MUST try to
         * complete it, or else non-top order-preserving broadcast motion nodes
         * will deadlock.
         */
        
        if (node->tupSendCtxt.heap_tuple != NULL)
            trySendCurrentTuple(node);
        
        /* This part of the loop is just trying to get a tuple for everybody. */
        
        allReady = true;
        
        for (i = 0; i < motion->numInputSegs; i++)
        {
            if (motion->inputSegIdx[i] == mySegIdx || tupsReceived[i])
                continue;
            
            recvRC = RecvTupleFrom(motion->motionID, &inputTuple,
                motion->inputSegIdx[i]);
            
            if (recvRC != NO_TUPLE)
            {
                SetCdbTupleHeapInitialValue(node->tupleheap, inputTuple,
                    motion->inputSegIdx[i]);

                tupsReceived[i] = true;
                
                if (recvRC == GOT_TUPLE)
                    node->numTuplesFromAMS++;
            }
            else
            {
                allReady = false;
            }
        }
    }
    while (!allReady);
    
    /* Be nice and clean up. */
    pfree(tupsReceived);
    
    node->tupleheapReady = true;
}


/* ----------------------------------------------------------------
 *		ExecInitMotion
 * ----------------------------------------------------------------
 */
MotionState *
ExecInitMotion(Motion *node, EState *estate)
{
	MotionState* motionstate;
	bool readFromChild, readFromAMS;
	int i;
	TupleDesc tupDesc;
    
	SO_printf("ExecInitMotion: initializing motion node\n");
	
    AssertState(Cdb_role != CDB_ROLE_UTILITY);
    
	/*
	 * create state structure
	 */
	motionstate = makeNode(MotionState);
	motionstate->ps.plan = (Plan *) node;
	motionstate->ps.state = estate;
    
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
	motionstate->numTuplesToAMS = 0;
	motionstate->numTuplesFromAMS = 0;
	motionstate->numTuplesToParent = 0;
	motionstate->numTuplesToDeepSix = 0;
	
    bzero(&motionstate->tupSendCtxt, sizeof(motionstate->tupSendCtxt));
    
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
	 * initialize tuple type.  no need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecAssignResultTypeFromOuterPlan(&motionstate->ps);
	motionstate->ps.ps_ProjInfo = NULL;
	motionstate->hashExprState = NULL;
	motionstate->hashOids = NULL;
	motionstate->cdbhash = NULL;
		
	if ( node->motionType == MOTIONTYPE_HASH )
	{
		int			nkeys;
		ListCell	*ho;
		
		/*
		 * Get info about the hash functions to be used for each hash key.
		 */
		Assert( node->hashOp != NULL );
		Assert( node->numHashSegs > 0 );
		 
		nkeys = list_length(node->hashOp);
		motionstate->hashOids = (Oid *) palloc(nkeys * sizeof(Oid));
		i = 0;
		foreach(ho, node->hashOp)
		{
			Oid lefttype;
			Oid righttype;

			op_input_types( lfirst_oid(ho), &lefttype, &righttype );
			Assert( lefttype == righttype );
			motionstate->hashOids[i] = lefttype;
			i++;
		}
		
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
				if ( node->hashSegIdx[i] == CdbIdentity.segindex )
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
				if ( node->outputSegIdx[i] == CdbIdentity.segindex )
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
	/* TODO  Take this out when issue CDB-47 is resolved  */
	if ( tupDesc->natts == 0 )
	{
        ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
            errmsg("Motion node ID %d has an empty tuple attribute list.  This is not supported yet.  (See CDB-47)",
            node->motionID)));
	}

    InitMotionLayerNode(node->motionID, node->sendToSecondary,
        node->sendSorted, ExecGetResultType(&motionstate->ps));

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

    Motion *motion = (Motion *)node->ps.plan;
	Assert( motion != NULL );

	elog( DEBUG1, "Motion Node %d counts: "
					"%d numTuplesFromChild, %d numTuplesFromAMS, %d numTuplesToAMS, %d numTuplesToParent, %d numTuplesToDeepSix",
					motNodeID,
					node->numTuplesFromChild, 
					node->numTuplesFromAMS, 
					node->numTuplesToAMS, 
					node->numTuplesToParent,
					node->numTuplesToDeepSix
		);

	if ( motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs == 0 )
	{
		Assert(node->numTuplesFromChild == node->numTuplesToAMS);
		Assert(node->numTuplesFromChild + node->numTuplesFromAMS == node->numTuplesToParent);
	}
	else
	{
		Assert(node->numTuplesFromChild + node->numTuplesFromAMS == node->numTuplesToAMS + node->numTuplesToParent + node->numTuplesToDeepSix);
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


/*
 * Experimental code that will be replaced later with new hashing mechanism
 */
uint32 evalHashKey(ExprContext *econtext, List *hashkeys, Oid *hashOids, CdbHash *h)
{
	ListCell   *hk;
	int			i = 0;
	MemoryContext oldContext;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	cdbhashinit(h);

	foreach(hk, hashkeys)
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
			cdbhash(h, keyval, hashOids[i]); 
		else
			cdbhashnull(h);

		i++;
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
    mySegIdx = CdbIdentity.segindex;
    
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


