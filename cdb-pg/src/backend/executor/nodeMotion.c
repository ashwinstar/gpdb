/*-------------------------------------------------------------------------
 *
 * nodeMotion.c
 *	  Routines to handle moving tuples around in MPP.
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

/* #define MEASURE_MOTION_TIME */

#ifdef MEASURE_MOTION_TIME
#include <unistd.h>				/* gettimeofday */
#endif

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static TupleTableSlot *execMotionSender(MotionState * node);
static TupleTableSlot *execMotionUnsortedReceiver(MotionState * node);
static TupleTableSlot *execMotionSortedReceiver(MotionState * node);

static void execMotionSortedReceiverFirstTime(MotionState * node);

static uint32 evalHashKey(ExprContext *econtext, List *hashkeys, List *hashtypes, CdbHash * h);

static void doSendEndOfStream(Motion * motion, MotionState * node);
static void doSendTuple(Motion * motion, MotionState * node, TupleTableSlot *outerTupleSlot);


/*=========================================================================
 * VISIBLE FUNCTIONS
 */


/* ----------------------------------------------------------------
 *		ExecMotion
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMotion(MotionState * node)
{
	Motion	   *motion = (Motion *) node->ps.plan;
	TupleTableSlot *tuple;

	/*
	 * at the top here we basically decide: -- SENDER vs. RECEIVER and --
	 * SORTED vs. UNSORTED
	 */
	if (node->isReceiver)
	{
#ifdef MEASURE_MOTION_TIME
		struct timeval startTime;
		struct timeval stopTime;

		gettimeofday(&startTime, NULL);
#endif

		if (motion->sendSorted)
			tuple = execMotionSortedReceiver(node);
		else
			tuple = execMotionUnsortedReceiver(node);

#ifdef MEASURE_MOTION_TIME
		gettimeofday(&stopTime, NULL);

		node->motionTime.tv_sec += stopTime.tv_sec - startTime.tv_sec;
		node->motionTime.tv_usec += stopTime.tv_usec - startTime.tv_usec;

		while (node->motionTime.tv_usec < 0)
		{
			node->motionTime.tv_usec += 1000000;
			node->motionTime.tv_sec--;
		}

		while (node->motionTime.tv_usec >= 1000000)
		{
			node->motionTime.tv_usec -= 1000000;
			node->motionTime.tv_sec++;
		}
#endif

		return tuple;
	}
	else
		return execMotionSender(node);
}


static TupleTableSlot *
execMotionSender(MotionState * node)
{
	/* SENDER LOGIC */
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerNode;
	Motion	   *motion = (Motion *) node->ps.plan;
	bool		done = false;

#ifdef MEASURE_MOTION_TIME
	struct timeval time1;
	struct timeval time2;
#endif
	SO1_printf("execMotionSender: %s\n", "entering routine");

	AssertState(motion->motionType == MOTIONTYPE_HASH ||
	 (motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs <= 1));

#ifdef MEASURE_MOTION_TIME
	gettimeofday(&time1, NULL);
#endif

	while (!done)
	{
		/* grab TupleTableSlot from our child. */
		outerNode = outerPlanState(node);

		outerTupleSlot = ExecProcNode(outerNode);

#ifdef MEASURE_MOTION_TIME
		gettimeofday(&time2, NULL);

		node->otherTime.tv_sec += time2.tv_sec - time1.tv_sec;
		node->otherTime.tv_usec += time2.tv_usec - time1.tv_usec;

		while (node->otherTime.tv_usec < 0)
		{
			node->otherTime.tv_usec += 1000000;
			node->otherTime.tv_sec--;
		}

		while (node->otherTime.tv_usec >= 1000000)
		{
			node->otherTime.tv_usec -= 1000000;
			node->otherTime.tv_sec++;
		}
#endif
		if (TupIsNull(outerTupleSlot))
		{
			doSendEndOfStream(motion, node);
			done = true;
		}
		else
		{
			doSendTuple(motion, node, outerTupleSlot);
			/* doSendTuple() may have set node->stopRequested as a side-effect */
			if (node->stopRequested)
			{
				/* propagate stop notification to our children */
				ExecSquelchNode(outerNode);
				done = true;
			}
		}
#ifdef MEASURE_MOTION_TIME
		gettimeofday(&time1, NULL);

		node->motionTime.tv_sec += time1.tv_sec - time2.tv_sec;
		node->motionTime.tv_usec += time1.tv_usec - time2.tv_usec;

		while (node->motionTime.tv_usec < 0)
		{
			node->motionTime.tv_usec += 1000000;
			node->motionTime.tv_sec--;
		}

		while (node->motionTime.tv_usec >= 1000000)
		{
			node->motionTime.tv_usec -= 1000000;
			node->motionTime.tv_sec++;
		}
#endif
	}

	/* nothing else to send out, so we return NULL up the tree. */
	return NULL;
}


static TupleTableSlot *
execMotionUnsortedReceiver(MotionState * node)
{
	/* RECEIVER LOGIC */
	TupleTableSlot *slot;
	HeapTuple	tuple;
	Motion	   *motion = (Motion *) node->ps.plan;
	ReceiveReturnCode recvRC;

	SO1_printf("ExecMotion: %s\n", "entering routine");

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
		elog(DEBUG4, "motionID=%d saw END_OF_STREAM", motion->motionID);

		return NULL;
	}
	else if (recvRC == GOT_TUPLE)
	{
		node->numTuplesFromAMS++;

		/* store it in our result slot and return this. */
		slot = node->ps.ps_ResultTupleSlot;
		node->numTuplesToParent++;

		return ExecStoreTuple(tuple, slot, InvalidBuffer, true /* shouldFree */ );
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
 * CASE 1:	 broadcast order-preserving fixed motion.  This should only be
 *			 called for SENDERs.
 *
 * CASE 2:	 single-destination order-preserving fixed motion.	The SENDER
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
 * just leave that slot empty and proceed.	The
 * GetLowestValueFromCdbTupleHeap() is smart enough to ignore them.
 *
 */
static TupleTableSlot *
execMotionSortedReceiver(MotionState * node)
{
	TupleTableSlot *slot;
	HeapTuple	tuple,
				inputTuple;
	Motion	   *motion = (Motion *) node->ps.plan;
	ReceiveReturnCode recvRC;
	CdbTupleHeapInfo *tupHeapInfo;

	SO1_printf("execMotionSorted: %s\n", "entering routine");

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
		recvRC = RecvTupleFrom(motion->motionID, &inputTuple, node->routeIdNext);

		if (recvRC == GOT_TUPLE)
		{
			node->numTuplesFromAMS++;
			node->numTuplesInHeap++;
		}

		MergeNewValueIntoCdbTupleHeap(node->tupleheap, inputTuple, node->routeIdNext);
	}

	/*
	 * THE HEAP SHOULD NOW HAVE A TUPLE FROM EVERY SOURCE.
	 *
	 * Pull out the next tuple and pass it back.
	 */

	tupHeapInfo = GetLowestValueFromCdbTupleHeap(node->tupleheap);
	tuple = tupHeapInfo->tuple;
	node->routeIdNext = tupHeapInfo->sourceRouteId;

	if (tuple != NULL)
	{
		/*
		 * store it in our result slot and return this.
		 */
		slot = node->ps.ps_ResultTupleSlot;

		node->numTuplesToParent++;
		node->numTuplesInHeap--;
		Assert(node->numTuplesInHeap >= 0);

		return ExecStoreTuple(tuple, slot, InvalidBuffer, true /* shouldFree */ );
	}
	else
		/* tuple is null */
	{
		return NULL;
	}
}



void
execMotionSortedReceiverFirstTime(MotionState * node)
{
	HeapTuple	inputTuple;
	Motion	   *motion = (Motion *) node->ps.plan;
	int			mySegIdx;
	bool	   *tupsReceived;
	int			i;

	ReceiveReturnCode recvRC;

	mySegIdx = Mpp_segment;

	/*
	 * INITIALIZATION -
	 *
	 * We need to get a tuple from every sender, and stick it into the heap.
	 *
	 * To keep all the initialize-logic in one place, we will pull a single tuple
	 * from our child plan-node,
	 */


	/* These are all kinda different ways of saying the same thing... */
	AssertState(motion->numOutputSegs == 0 ||
				motion->outputSegIdx[0] == mySegIdx);

	/*
	 * Now we iterate over the tuple-heap, and try to receive a tuple for each
	 * entry that doesn't yet have a tuple.
	 */
	tupsReceived = (bool *) palloc0(sizeof(bool) * motion->numInputSegs);

	for (i = 0; i < motion->numInputSegs; i++)
	{
		if (motion->inputSegIdx[i] == mySegIdx || tupsReceived[i])
			continue;


		/*
		 * another place where we are mapping segid space to routeid space. so
		 * route[x] = inputSegIdx[x] now.
		 */
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
			/*
			 * TODO: this shouldn't ever happen. just leaving this as a check
			 * to make sure that's really true. at some point this should be
			 * cleaned up a bit.
			 */
			elog(FATAL, "RecvTupleFrom returned recvRC of: %d which is invalid",
				 recvRC);
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
 *		 after this call in mppexec.c.	This is ok since we don't need it,
 *		 but just be aware before you try and use it here.
 * ----------------------------------------------------------------
 */
MotionState *
ExecInitMotion(Motion * node, EState *estate)
{
	MotionState *motionstate;
	int			i;
	TupleDesc	tupDesc;
	Slice	   *parentSlice;
	Slice	   *thisSlice;
	int			prior_es_cur_slice_idx;
	int			prior_es_cur_root_idx;

	SO_printf("ExecInitMotion: initializing motion node\n");

	AssertState(Mpp_role != MPP_ROLE_UTILITY);

	prior_es_cur_slice_idx = estate->es_cur_slice_idx;	/* save */
	prior_es_cur_root_idx = estate->es_cur_root_idx;	/* save */

	/*
	 * create state structure
	 */
	motionstate = makeNode(MotionState);
	motionstate->ps.plan = (Plan *) node;
	motionstate->ps.state = estate;

	/*
	 *	MPP  Slice Plan:
	 *	 QD must initialize the global slice table.
	 *	 QE must fill in map from motionID to MotionState node.
	 */
	if (Mpp_role == MPP_ROLE_DISPATCH)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

		parentSlice = (Slice *) list_nth(estate->es_sliceTable->slices, estate->es_cur_slice_idx);

		Assert(IsA(parentSlice, Slice));
		Assert(parentSlice->sliceIndex == estate->es_cur_slice_idx);
		Assert(0 < node->motionID && node->motionID <= estate->es_sliceTable->nMotions);

		thisSlice = (Slice *) list_nth(estate->es_sliceTable->slices, node->motionID);

		Assert(IsA(thisSlice, Slice));
		Assert(thisSlice->sliceIndex == -1);

		thisSlice->sliceIndex = node->motionID;
		thisSlice->rootIndex = parentSlice->rootIndex;

		/*
		 * The gang beneath a Motion will be a reader, but its size must be
		 * determined.
		 */
		thisSlice->gangType = GANGTYPE_READER;
		Assert(node->plan.lefttree->flow != NULL);
		switch (node->plan.lefttree->flow->base_flotype)
		{
			case FLOW_SINGLETON:
				thisSlice->gangSize = 1;
				break;
			case FLOW_REPLICATED:
			case FLOW_PARTITIONED:
				thisSlice->gangSize = getMPPSegmentCount();
				break;
			default:
				/* Assert FALSE -- Above are only values we should see. */
				Assert(node->plan.lefttree->flow->flotype == FLOW_SINGLETON);
		}

		thisSlice->parentIndex = estate->es_cur_slice_idx;
		parentSlice->children = lappend_int(parentSlice->children, thisSlice->sliceIndex);

		estate->es_cur_slice_idx = thisSlice->sliceIndex;

		MemoryContextSwitchTo(oldcxt);
	}
	else if (Mpp_role == MPP_ROLE_EXECUTE)
	{
		Assert(estate->es_slice_map != NULL && estate->es_slice_map[0] == NULL);
		estate->es_slice_map[node->motionID] = motionstate;
	}

	motionstate->sentEndOfStream = false;

	motionstate->otherTime.tv_sec = 0;
	motionstate->otherTime.tv_usec = 0;
	motionstate->motionTime.tv_sec = 0;
	motionstate->motionTime.tv_usec = 0;

	motionstate->numTuplesFromChild = 0;
	motionstate->numTuplesToAMS = 0;
	motionstate->numTuplesFromAMS = 0;
	motionstate->numTuplesToParent = 0;
	motionstate->numTuplesToDeepSix = 0;
	motionstate->numTuplesInHeap = 0;

	motionstate->stopRequested = false;

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
	estate->es_cur_slice_idx = prior_es_cur_slice_idx;
	estate->es_cur_root_idx = prior_es_cur_root_idx;

	if (motionstate->ps.state->es_cur_slice_idx == node->motionID)
		motionstate->isReceiver = false;
	else
		motionstate->isReceiver = true;

	elog(DEBUG2, "Motion-node %d type %s is a %s",
		 node->motionID,
		 (node->motionType == MOTIONTYPE_HASH ? "hash" : "fixed"),
		 motionstate->isReceiver ? "RECEIVER" : "SENDER");

	/*
	 * initialize tuple type.  no need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecAssignResultTypeFromOuterPlan(&motionstate->ps);
	motionstate->ps.ps_ProjInfo = NULL;
	motionstate->hashExprState = NULL;
	motionstate->cdbhash = NULL;

	if (node->motionType == MOTIONTYPE_HASH)
	{
		int			nkeys;

		/*
		 * Get info about the hash functions to be used for each hash key.
		 */
		Assert(node->hashDataTypes != NULL);
		Assert(node->numHashSegs > 0);

		nkeys = list_length(node->hashDataTypes);
		motionstate->hashExprState = (List *) ExecInitExpr((Expr *) node->hashExpr,
												  (PlanState *) motionstate);

		/*
		 * Create hash API reference
		 */
		motionstate->cdbhash = makeCdbHash(node->numHashSegs, HASH_FNV_1);

		/*
		 * Create bit vector of masks if this is a "deep-six" hash motion node
		 */
		if (node->hashToSelfOnly)
		{
			Assert(!node->sendSorted);

			motionstate->hashSegIdxMask = palloc0(node->numHashSegs * sizeof(bool));
			for (i = 0; i < node->numHashSegs; i++)
			{
				if (node->hashSegIdx[i] == Mpp_segment)
					motionstate->hashSegIdxMask[i] = true;
				else
					motionstate->hashSegIdxMask[i] = false;
			}
		}

		/*
		 * Create buckets to hold counts of tuples hashing to each
		 * destination segindex (for debugging purposes)
		 */
		motionstate->numTuplesByHashSegIdx = (int *) palloc0(node->numHashSegs * sizeof(int));
	}
	else if (node->motionType == MOTIONTYPE_FIXED)
	{
		/*
		 * Create bit vector of masks if this is a "deep-six" fixed motion node
		 */
		if (node->fixedToSelfOnly)
		{
			Assert(node->numOutputSegs > 0);
			Assert(!node->sendSorted);

			motionstate->fixedSegIdxMask = palloc0(node->numOutputSegs * sizeof(bool));
			for (i = 0; i < node->numOutputSegs; i++)
			{
				if (node->outputSegIdx[i] == Mpp_segment)
					motionstate->fixedSegIdxMask[i] = true;
				else
					motionstate->fixedSegIdxMask[i] = false;
			}
		}
	}

	if (node->sendSorted)
	{
		Assert(node->numSortCols > 0);
		Assert(node->numInputSegs > 0);
		Assert(node->inputSegIdx != NULL);

		motionstate->sortFunctions = (FmgrInfo *) palloc0(node->numSortCols * sizeof(FmgrInfo));
		motionstate->sortFnKinds = (SortFunctionKind *) palloc0(node->numSortCols * sizeof(SortFunctionKind));

		for (i = 0; i < node->numSortCols; i++)
		{
			RegProcedure sortFunction;

			AssertArg(node->sortOperators[i] != 0);
			AssertArg(node->sortColIdx[i] != 0);

			/* select a function that implements the sort operator */
			SelectSortFunction(node->sortOperators[i], &sortFunction, &motionstate->sortFnKinds[i]);

			fmgr_info(sortFunction, &motionstate->sortFunctions[i]);
		}

		motionstate->tupleheap = CreateCdbTupleHeap(node->numInputSegs,
													motionstate,
													node);
		motionstate->tupleheapReady = false;

		/*
		 * Here is a leap of faith where we are implicitly mapping from a
		 * segment id space to the routeId space of the interconnect.
		 */
		motionstate->routeIdNext = node->inputSegIdx[0];
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
ExecCountSlotsMotion(Motion * node)
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
ExecEndMotion(MotionState * node)
{
	Motion	   *motion;
	uint16		motNodeID;
	int			i;

#ifdef MEASURE_MOTION_TIME
	double		otherTimeSec;
	double		motionTimeSec;
#endif

	SO1_printf("ExecEndMotion: %s\n",
			   "shutting down motion node");

	motNodeID = ((Motion *) node->ps.plan)->motionID;

	ExecFreeExprContext(&node->ps);

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

	motion = (Motion *) node->ps.plan;
	Assert(motion != NULL);

#ifdef MEASURE_MOTION_TIME
	motionTimeSec = (double) node->motionTime.tv_sec + (double) node->motionTime.tv_usec / 1000000.0;

	if (node->isReceiver)
	{
		elog(DEBUG1,
			 "Motion Node %d (RECEIVER) Statistics:\n"
			 "Timing:  \n"
			 "\t Time receiving the tuple: %f sec\n"
			 "Counters: \n"
			 "\tnumTuplesFromChild: %d\n"
			 "\tnumTuplesFromAMS: %d\n"
			 "\tnumTuplesToAMS: %d\n"
			 "\tnumTuplesToParent: %d\n"
			 "\tnumTuplesToDeepSix: %d\n"
			 "\tnumTuplesInHeap: %d\n",
			 motNodeID,
			 motionTimeSec,
			 node->numTuplesFromChild,
			 node->numTuplesFromAMS,
			 node->numTuplesToAMS,
			 node->numTuplesToParent,
			 node->numTuplesToDeepSix,
			 node->numTuplesInHeap
			);
	}
	else
	{
		otherTimeSec = (double) node->otherTime.tv_sec + (double) node->otherTime.tv_usec / 1000000.0;
		elog(DEBUG1,
			 "Motion Node %d (SENDER) Statistics:\n"
			 "Timing:  \n"
			 "\t Time getting next tuple to send: %f sec \n"
			 "\t Time sending the tuple:          %f  sec\n"
			 "\t Percentage of time sending:      %2.2f%% \n"
			 "Counters: \n"
			 "\tnumTuplesFromChild: %d\n"
			 "\tnumTuplesFromAMS: %d\n"
			 "\tnumTuplesToAMS: %d\n"
			 "\tnumTuplesToParent: %d\n"
			 "\tnumTuplesToDeepSix: %d\n"
			 "\tnumTuplesInHeap: %d\n",
			 motNodeID,
			 otherTimeSec,
			 motionTimeSec,
			 (double) (motionTimeSec / (otherTimeSec + motionTimeSec)) * 100,
			 node->numTuplesFromChild,
			 node->numTuplesFromAMS,
			 node->numTuplesToAMS,
			 node->numTuplesToParent,
			 node->numTuplesToDeepSix,
			 node->numTuplesInHeap
			);
	}
#endif

	/*
	 * I notice that it is wrong, at least, in the case of a LIMIT which calls
	 * ExecStopMotion which (correctly) sets node->stopRequested to true. So I
	 * made it conditional as a stopgap.  I doubt this is the correct
	 * permanent fix.  Brian 8/2005
	 *
	 * I extended Brian's temporary stopRequested fix while testing
	 * early-end-of-retrieval support (stop messages are now actually getting
	 * sent). We seem to have two choices, do it like this or fixup the tuple
	 * counts.	Tim 9/20/2005
	 */
	if (motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs == 0)
	{
		Assert(node->stopRequested || node->numTuplesFromChild == node->numTuplesToAMS);
		Assert(node->stopRequested || node->numTuplesFromAMS == node->numTuplesToParent);
	}
	else
	{
		Assert(node->stopRequested || node->numTuplesFromChild == node->numTuplesToAMS + node->numTuplesToDeepSix + node->numTuplesInHeap);
		/* Hmmm... It this " - node->numTuplesInHeap" correct? */
		Assert(node->numTuplesFromAMS - node->numTuplesInHeap == node->numTuplesToParent);
	}

	if (motion->motionType == MOTIONTYPE_HASH)
	{
		if (node->numTuplesByHashSegIdx != NULL)
		{
			elog(DEBUG1, "Motion Node %d Hash Bucket Distribution of child tuples: ", motNodeID);
			for (i = 0; i < motion->numHashSegs; i++)
			{
				elog(DEBUG1, "Bucket %d : %d", i, node->numTuplesByHashSegIdx[i]);
			}

			pfree(node->numTuplesByHashSegIdx);
			node->numTuplesByHashSegIdx = NULL;
		}

		if (node->hashSegIdxMask != NULL)
			pfree(node->hashSegIdxMask);
	}
	else if (motion->motionType == MOTIONTYPE_FIXED)
	{
		if (node->fixedSegIdxMask != NULL)
			pfree(node->fixedSegIdxMask);
	}

	if (node->tupleheap != NULL)
	{
		DestroyCdbHeap(node->tupleheap);
		node->tupleheap = NULL;
	}

	if (node->cdbhash != NULL)
	{
		pfree(node->cdbhash);
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
uint32
evalHashKey(ExprContext *econtext, List *hashkeys, List *hashtypes, CdbHash * h)
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


void
doSendEndOfStream(Motion * motion, MotionState * node)
{
	SendReturnCode sendRC;
	int			targetRoute;

	/*
	 * We have no more child tuples, but we have not successfully sent an
	 * End-of-Stream token yet.
	 *
	 * NOTE:  If there is only one destination, we do not broadcast the
	 * end-of-stream; we only send it to the single dest.
	 */
	if (motion->motionType == MOTIONTYPE_FIXED && motion->numOutputSegs == 1)
		targetRoute = 0;
	else
		targetRoute = BROADCAST_SEGIDX;

	sendRC = SendEndOfStream(motion->motionID, targetRoute);

	Assert(sendRC == SEND_COMPLETE);

	node->sentEndOfStream = true;
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
 *	FIXED - sending to a single process.  the value in node->fixedSegIdxMask[0]
 *			is the contentId of who to send to.  But we can actually ignore that
 *			since now with slice tables, we should only have a single CdbProcess
 *			that we could send to for this motion node.
 *
 *
 *	BROADCAST - actually a subcase of FIXED, but handling is simple. send to all
 *				of our routes.
 *
 *	HASH -	maps hash values to segid.	this mapping is 1->1 such that a hash
 *			value of 2 maps to contentid of 2 (for now).  Since we can't ever
 *			use Hash to send to the QD, the QD's contentid of -1 is not an issue.
 *			Also, the contentid maps directly to the routeid.
 *
 */
void
doSendTuple(Motion * motion, MotionState * node, TupleTableSlot *outerTupleSlot)
{
	int16		targetRoute;

	SendReturnCode sendRC;
	ExprContext *econtext = node->ps.ps_ExprContext;

	/* We got a tuple from the child-plan. */
	node->numTuplesFromChild++;

	if (motion->motionType == MOTIONTYPE_FIXED)
	{
		if (motion->numOutputSegs == 0) /* Broadcast */
		{
			targetRoute = BROADCAST_SEGIDX;
		}
		else
			/* Fixed Motion. */
		{
			AssertState(motion->outputSegIdx != NULL);

			/* TODO:  We can only support one output-segment right now! */
			AssertState(motion->numOutputSegs == 1);

			if (node->fixedSegIdxMask != NULL && !node->fixedSegIdxMask[0])
			{
				/* this tuple will be deep-sixed. */
				node->numTuplesToDeepSix++;
				return;
			}
			else
			{
				/*
				 * Actually, since we can only send to a single output segment
				 * here, we are guarantted that we only have a single
				 * targetRoute setup that we could possibly send to.  So we
				 * can cheat and just fix the targetRoute to 0 (the 1st
				 * route).
				 */
				targetRoute = 0;
			}
		}
	}
	else
		/* Hash motion node */
	{
		/*
		 * hashSegIdx structure doesn not include support for the QD. so 0
		 * maps to segID with contentId = 0
		 */
		uint32		hval = 0;

		Assert(motion->numHashSegs > 0 && motion->hashSegIdx != NULL);

		econtext->ecxt_outertuple = outerTupleSlot;
		hval = evalHashKey(econtext, node->hashExprState,
						   motion->hashDataTypes, node->cdbhash);
		targetRoute = motion->hashSegIdx[hval];

		Assert(node->numTuplesByHashSegIdx != NULL);
		node->numTuplesByHashSegIdx[hval]++;

		if (node->hashSegIdxMask != NULL && !node->hashSegIdxMask[hval])
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
 *
 * Note: once this is called, it is possible that the motion node will
 * never be called again, so we *must* send the stop message now.
 */
void
ExecStopMotion(MotionState * node)
{
	Motion	   *motion;

	AssertArg(node != NULL);

	motion = (Motion *) node->ps.plan;

	node->stopRequested = true;
	SendStopMessage(motion->motionID);
}
