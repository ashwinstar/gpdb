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
#include "utils/syscache.h" /* for ReleaseSysCache */
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
static void validateMotionNodeParms(MotionType motionType,
			bool sendToSecondary, bool sendSorted, 
			int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
			int numInputSegs, int *inputSegIdx );
static List *getExprListFromCondList(List *clist, HashMotionInfoType type);
static List *getOpListFromCondList(List *clist);
static List *getExprListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx);
static List *getOpListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx);
static bool motion_expr_walker(Node *node, char *not_used);
static uint32 evalHashKey(ExprContext *econtext, List *hashkeys, Oid *hashOids, CdbHash *h);
static bool setReadFromAMS(Motion *node);
static List * makeMotionTargetList(Plan *arg);

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

/* -----------------------------------------------------------------------
 * Helper functions for extracting Lists of join expressions or operators
 * from nodes, where the logic varies by node type
 * -----------------------------------------------------------------------
 */
List *getHashMotionInfoList( Plan *node, HashMotionInfoType type )
{
	List *hashinfo = NIL;
	
	switch ( nodeTag(node) )
	{
		case T_HashJoin:
			/*
			 * Deconstruct the hash clauses into outer and inner argument values,
			 * so that we can evaluate those subexpressions separately.  
			 */
			{
				HashJoin *hjnode = (HashJoin *)node;
				List *hclist = hjnode->hashclauses;
				if ( hclist == NIL )
					elog( ERROR, "Empty hashclauses list encountered in getHashMotionInfoList for HashJoin node");
					
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromCondList(hclist, type);
				else	
					hashinfo = getOpListFromCondList(hclist);
			}
				
			break;
			
		case T_Hash:
			elog( ERROR, "Musn't use a Hash Node in getHashMotionInfoList.  Use the parent HashJoin node instead." );
			break;
			
		case T_MergeJoin:
			/*
			 * Deconstruct the merge clauses into outer and inner argument values,
			 * so that we can evaluate those subexpressions separately.
			 */
			{
				MergeJoin *mjnode = (MergeJoin *)node;
				List *mclist = mjnode->mergeclauses;
				if ( mclist == NIL )
					elog( ERROR, "Empty mergeclauses list encountered in getHashMotionInfoList for MergeJoin node");
				
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromCondList(mclist, type);
				else	
					hashinfo = getOpListFromCondList(mclist);
			}
				
			break;
			
		case T_NestLoop:
			{
				NestLoop *nlnode = (NestLoop *)node;
				List *jqlist = nlnode->join.joinqual;
				if ( jqlist == NIL )
					elog( ERROR, 
							"Not yet implemented: calling getHashMotionInfoList for a NestLoop node with no joinqual list.  "
							"This can happen with an IndexScan node as the inner child." );
				
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromCondList(jqlist, type);
				else	
					hashinfo = getOpListFromCondList(jqlist);
			}
				
			break;
			
		case T_Sort:
			{
				Sort		*snode = (Sort *)node;
				int			numCols = snode->numCols;
				AttrNumber *sortColIdx = snode->sortColIdx;
				
				if ( numCols == 0 || sortColIdx == NULL )
					elog( ERROR, 
							"Empty sortColIdx array encountered in getHashExprList for Sort node" );
							
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromTargetList(snode->plan.targetlist, numCols, sortColIdx);
				else
					hashinfo = getOpListFromTargetList(snode->plan.targetlist, numCols, sortColIdx);
					
			}
				
			break;
			
		case T_Agg:
			{
				Agg			*aggnode = (Agg *)node;
				if ( aggnode->aggstrategy == AGG_PLAIN )
					elog( ERROR, "there is no hash expression list for Plain Aggregate nodes");	
				
				int			numCols = aggnode->numCols;
				AttrNumber *grpColIdx = aggnode->grpColIdx;

				if ( numCols == 0 || grpColIdx == NULL )				
					elog( ERROR, 
						"Empty grpColIdx array encountered in getHashExprList for Agg node" );
						
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromTargetList(aggnode->plan.lefttree->targetlist, numCols, grpColIdx);
				else
					hashinfo = getOpListFromTargetList(aggnode->plan.lefttree->targetlist, numCols, grpColIdx);
			}
				
			break;
			
		default:
			elog( WARNING, "getHashMotionInfoList called with an unsupported node type %d", nodeTag(node) );
			break;
	}	
	
	return hashinfo;
} 

/* --------------------------------------------------------------------
 * make_motion -- creates a Motion node.
 * Caller must have built the pHashDefn, pFixedDefn,
 * and pSortDefn structs already.
 * --------------------------------------------------------------------
 */
Motion *make_motion(Plan *lefttree, MotionType motionType,
			bool sendToSecondary, bool sendSorted, 
			int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
			int numInputSegs, int *inputSegIdx )
{
	validateMotionNodeParms(motionType,	sendToSecondary, sendSorted, 
			numHashSegs, hashSegIdx, hashExpr, hashOp,
			numOutputSegs, outputSegIdx,
			numSortCols, sortColIdx, sortOperators, 
			numInputSegs, inputSegIdx);

	Motion	*node = makeNode(Motion);
	Plan	*plan = &node->plan;
			
	plan->startup_cost = lefttree->startup_cost;
	plan->total_cost = lefttree->total_cost;
	plan->plan_rows = lefttree->plan_rows;
	plan->plan_width = lefttree->plan_width;
	plan->targetlist = copyObject(lefttree->targetlist);
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->motionType = motionType;
	node->sendToSecondary = sendToSecondary;
	node->sendSorted = sendSorted;

	node->hashToSelfOnly = false;
	node->fixedToSelfOnly = false;

	if ( motionType == MOTIONTYPE_HASH )
	{
		node->numHashSegs = numHashSegs;
		node->hashSegIdx = hashSegIdx;
	}

	node->hashExpr = hashExpr;
	node->hashOp = hashOp;
	node->numOutputSegs = numOutputSegs;
	node->outputSegIdx = outputSegIdx;

	node->numSortCols = numSortCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;

	if ( sendSorted )
	{
		node->numInputSegs = numInputSegs; 
		node->inputSegIdx = inputSegIdx;
	}

	return node;
}

Motion *make_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex )
{
	int *outputSegIdx = palloc( sizeof(int) );
	outputSegIdx[0] = destSegIndex;

	return 	make_motion(lefttree, MOTIONTYPE_FIXED, 
						sendToSecondary, false, 
						0, NULL, NIL, NIL,
						1, outputSegIdx,
						0, NULL, NULL, 0, NULL );
}

Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex, 
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
								int numInputSegs, int *inputSegIdx )
{
	int *outputSegIdx = palloc( sizeof(int) );
    int mySegIdx;
    
    /* TODO:  Pete needs to work with Brian on this part.  Donnie is clueless. */
    mySegIdx = CdbIdentity.segindex;
	outputSegIdx[0] = mySegIdx;

	return 	make_motion(lefttree, MOTIONTYPE_FIXED, 
						sendToSecondary, true, 
						0, NULL, NIL, NIL,
						1, outputSegIdx, 
						numSortCols, sortColIdx, sortOperators, numInputSegs, inputSegIdx);
}

Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary, 
							int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp )
{
	return make_motion(lefttree, MOTIONTYPE_HASH,
						sendToSecondary, false, 
						numHashSegs, hashSegIdx, hashExpr, hashOp,
						0, NULL, 0, NULL, NULL, 0, NULL);
}

Motion *make_sorted_hashed_motion(Plan *lefttree, bool sendToSecondary, 
								int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
								int numInputSegs, int *inputSegIdx )
{
	return make_motion(lefttree, MOTIONTYPE_HASH,
						sendToSecondary, true, 
						numHashSegs, hashSegIdx, hashExpr, hashOp, 
						0, NULL, 
						numSortCols, sortColIdx, sortOperators, numInputSegs, inputSegIdx );
}

Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary)
{
	return make_motion(lefttree, MOTIONTYPE_FIXED,
						sendToSecondary, false, 
						0, NULL, NIL, NIL,
						0, NULL, 
						0, NULL, NULL, 0, NULL );
}

Motion *make_sorted_broadcast_motion(Plan *lefttree, bool sendToSecondary, 
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
								int numInputSegs, int *inputSegIdx )
{
	return make_motion(lefttree, MOTIONTYPE_FIXED,
						sendToSecondary, true, 
						0, NULL, NIL, NIL,
						0, NULL, 
						numSortCols, sortColIdx, sortOperators, numInputSegs, inputSegIdx );
}

/*
 * setHashToSelfOnly
 * This enables "deep-sixing" of hash motion, whereby
 * a tuple is only passed along if it hashes to its own segindex.
 * This implements "replicated to partitioned" motion.
 *
 * The motion node must already have been created, 
 * it must of course be a hash motion node,
 * and it must not be sorted (Currently sorted hash motion ins not permitted anyway).
 */
void setHashToSelfOnly( Motion *motion )
{
	Assert( motion != NULL );
	Assert( motion->motionType == MOTIONTYPE_HASH );
	Assert( motion->numHashSegs > 0 );
	Assert( motion->hashSegIdx != NULL );
	Assert( !motion->sendSorted );

	motion->hashToSelfOnly = true;
}

/*
 * setFixedToSelfOnly
 * This enables "deep-sixing" of fixed motion, whereby
 * a tuple is only passed along if its destination is its own segindex.
 * This implements "replicated to singleton" motion.
 *
 * The motion node must already have been created, 
 * it must of course be a fixed motion node,
 * and it must not be sorted,
 * and it must not be a broadcast fixed motion node.
 */
void setFixedToSelfOnly( Motion *motion )
{
	Assert( motion != NULL );
	Assert( motion->motionType == MOTIONTYPE_FIXED );
	Assert( motion->numOutputSegs > 0 );
	Assert( motion->outputSegIdx != NULL );
	Assert( !motion->sendSorted );

	motion->fixedToSelfOnly = true;
}

/* --------------------------------------------------------------------
 * makeDefaultSegIdxArray -- creates an int array with numSegs elements,
 * with values between 0 and numSegs-1
 * --------------------------------------------------------------------
 */
int *makeDefaultSegIdxArray(int numSegs)
{
	int *segIdx = palloc(numSegs * sizeof(int));

	/*
	 * The following assumes that the default is to have segindexes
	 * numbered sequentially starting at 0.
	 */
	int i;
	for ( i=0; i<numSegs; i++ )
		segIdx[i] = i;
		
	return segIdx;	
}

/* --------------------------------------------------------------------
 * 
 *  Static Helper routines
 * --------------------------------------------------------------------
 */
 
/* --------------------------------------------------------------------
 * validateMotionNodeParms -- validates the parameters passed to
 * make_motion
 * --------------------------------------------------------------------
 */
void validateMotionNodeParms(MotionType motionType,
			bool sendToSecondary, bool sendSorted, 
			int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
			int numInputSegs, int *inputSegIdx )
{
	/*
	 *  Perform validation based on motionType.
	 */
	switch( motionType )
	{
		case MOTIONTYPE_HASH: 
	
		/*
		 *  A Hash motion node must have hash expr info specified.
		 */
			if ( hashExpr == NIL )
				elog(ERROR, "motion type 'hash' node must have non-NULL hashExpr");
				
		/*
		 *  A Hash motion node must have hash operator info specified.
		 */
			if ( hashOp == NIL )
				elog(ERROR, "motion type 'hash' node must have non-NULL hashOp");
				
		/*
		 *  A Hash motion node must have hash segment info specified.
		 */
			if ( numHashSegs <= 0 || hashSegIdx == NULL )
				elog(ERROR, "motion type 'hash' node must have numHashSegs > 0 and non-NULL hashSegIdx");

		/*
		 *  A Hash motion node shouldn't have FixedMotionDefn specified.
		 */
			Assert(numOutputSegs == 0 && outputSegIdx == NULL);
			break;	
	
		case MOTIONTYPE_FIXED: 

		/*
		 *  An Fixed motion node must have output Segs info specified.
		 */
			if ( numOutputSegs < 0 || 
				(numOutputSegs == 0 && outputSegIdx != NULL ) || 
				(numOutputSegs > 0 && outputSegIdx == NULL ) )
				elog(ERROR, "motion type 'fixed' node must have numOutputSegs > 0 and non-NULL outputSegIdx"
							" or numOutputSegs == 0 and NULL outputSegIdx");

		/*
		 *  A Fixed motion node shouldn't have HashMotionDefn specified.
		 */
			Assert( numHashSegs == 0 && hashSegIdx == NULL && hashExpr == NIL && hashOp == NIL);
				
			break;	
			
		default:
		/*
		 *  Invalid motion node type.
		 */
			Assert( true );
			break;
	}
	
	/*
	 *  Perform validation based on sendSorted flag
	 */
	if ( sendSorted )
	{
		/*
		 *  A sorted motion node must have sort Cols specified
		 */
		if (numSortCols == 0 || sortColIdx == NULL || sortOperators == NULL)
			elog(ERROR, "sorted motion node must have numSortCols > 0 and non-NULL sortColIdx and sortOperators");

		/*
		 *  A sorted motion node must have input Segs info specified
		 */
		if (numInputSegs == 0 || inputSegIdx == NULL)
			elog(ERROR, "sorted motion node must have numInputSegs > 0 and non-NULL inputSegIdx");
	}
	else
	{
		/*
		 *  A non-sorted motion node should not have SortMotionDefn specified
		 */
		Assert(numSortCols == 0 && sortColIdx == NULL && sortOperators == NULL && 
				numInputSegs == 0 && inputSegIdx == NULL);
	}
}			

/* --------------------------------------------------------------------
 * motion_expr_walker -- replaces occurrences of INNER with OUTER
 * for Var's varno in hashexpr for motion nodes.
 * This came about because the join nodes args for the right subtree were
 * changed by setrefs to be INNER.  We grabbed them after that change.
 * But the motion node has no inner subtree, just an outer.
 * --------------------------------------------------------------------
 */
bool motion_expr_walker(Node *node, char *not_used)
{
	if (node == NULL)
		return false;
	
	// check for nodes that special work is required for, eg:
	if (IsA(node, Var))
	{
		Var* var = (Var *)node;
		switch( var->varno )
		{
			case INNER:
				var->varno = OUTER;
				break;

			default:
				var->varno = OUTER;
				break;
		}
		
		return false;
	}
	else
	{
		return expression_tree_walker(node, motion_expr_walker, (void *) not_used);
	}
	
}

/* ----------------------------------------------------------------
 * getExprListFromCondList
 *
 * Extracts either the left or right arguments or operators from 
 * the given join condition list.
 * 
 * The entries returned are copies of the expressions from the
 * condition list in which any Var references to INNER have been
 * replaced by references to OUTER.
 * ----------------------------------------------------------------
 */
List *getExprListFromCondList(List *clist, HashMotionInfoType type)
{
	List *elist = NIL;
	ListCell *l;
	Node *expr;
	Node *node;
		
	foreach(l, clist)
	{
		OpExpr	*opexpr = (OpExpr *) lfirst(l);

		Assert(IsA(opexpr, OpExpr));
		Assert(list_length(opexpr->args) == 2);
		
		if (type == INFOTYPE_LEFTEXPR)
			node = linitial(opexpr->args);
		else if (type == INFOTYPE_RIGHTEXPR)	
			node = lsecond(opexpr->args);
		else
			Assert(0);	
			
		/* Assert(IsA(node, Expr)); -- Expr is abstract, IsA is not smart enought to deal with this. */
		expr = copyObject(node);
		
		motion_expr_walker(expr, NULL);
		elist = lappend(elist, expr);
	}
	
	return elist;
}	

/* ----------------------------------------------------------------
 * getOpListFromCondList - -
 * this extracts the list of hash operator OIDs
 * appropriate to the datatype of condition in the clist
 * ----------------------------------------------------------------
 */
List *getOpListFromCondList(List *clist)
{
	List		*oplist = NIL;
	ListCell	*l;
	
	foreach(l, clist)
	{
		OpExpr	*opexpr = (OpExpr *) lfirst(l);
		Assert(IsA(opexpr, OpExpr));
		oplist = lappend_oid(oplist, opexpr->opno);
	}
	
	return oplist;
}

/* ----------------------------------------------------------------
 * getExprListFromTargetList
 *
 * Creates a VAR that references the indicated entries from the target list, 
 * sets the restype and restypmod fields from the target list info,
 * and puts them into a list.
 *
 * The AttrNumber indexes actually refer to the 1 based index into the
 * target list.
 * 
 * The entries have the varno field replaced by references in OUTER.
 * ----------------------------------------------------------------
 */
List *getExprListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx)
{
	int		i;
	List	*elist = NIL;
	
	for (i=0; i<numCols; i++ )
	{
		Var *new_var;

		/* Find expr in TargetList */
		AttrNumber n = colIdx[i];

		TargetEntry* target = get_tle_by_resno( tlist, n );
		if ( target == NULL )
			elog( ERROR, "no tlist entry for key %d", n );

		/* Make a Var that references the target list entry at this offset, using OUTER as the varno */
		new_var = makeVar(OUTER, n, target->resdom->restype, target->resdom->restypmod, 0);
			
		elist = lappend(elist, new_var);
	}
	
	return elist;
}

/* ----------------------------------------------------------------
 * getOpListFromTargetList
 * 
 * Returns a list of oids or the equality operators for indicated
 * entries of the target list.
 *
 * The AttrNumber indexes actually refer to the resno value in the 
 * Resdom entries in the target list.  These are often just the
 * ordinal positions (origin 1) of the entries, but for non-query
 * DML it may not be.
 * 
 * ----------------------------------------------------------------
 */
List *getOpListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx)
{
	int			i;
	List		*oplist = NIL;
	Operator	eq_operator;
	Oid			typeoid;
	
	for (i=0; i<numCols; i++ )
	{
		// Find expr in TargetList
		AttrNumber n = colIdx[i];

		TargetEntry* target = get_tle_by_resno( tlist, n );
		if ( target == NULL )
			elog( ERROR, "no tlist entry for key %d", n );
			
		typeoid = target->resdom->restype;
		eq_operator = compatible_oper(list_make1(makeString("=")), typeoid, typeoid, true);
		oplist = lappend_oid(oplist, oprid(eq_operator));
		ReleaseSysCache(eq_operator);
	}
	
	return oplist;
}

/* ----------------------------------------------------------------
 * makeMotionTargetList
 * 
 * Returns a target list for a new motion node based on the
 * argument plan's target list.
 * 
 * ----------------------------------------------------------------
 */
 List * makeMotionTargetList(Plan *arg)
 {
	TargetEntry *tle;
	Resdom *new_resdom, *resdom;
	Var *new_var;
	ListCell *cell;
	
	int attno = 0;
	List *new_tlist = NIL;
	
	foreach ( cell, arg->targetlist )
	{
		tle = (TargetEntry*)lfirst(cell);
		resdom = tle->resdom;
		attno++;
		
		Assert( attno == tle->resdom->resno );
		
		new_resdom = makeResdom(
			tle->resdom->resno, 
			tle->resdom->restype, 
			tle->resdom->restypmod, 
			tle->resdom->resname? pstrdup(tle->resdom->resname): (char *) NULL,
			tle->resdom->resjunk
		);
			
		new_var = makeVar(
			0, attno, 
			new_resdom->restype, new_resdom->restypmod, 0
		);		
		
		new_tlist = lappend(new_tlist, makeTargetEntry(resdom, (Expr*)new_var));
	}
	return new_tlist;
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


