/*-------------------------------------------------------------------------
 *
 * nodeMotion.c
 *	  Routines to handle moving tuples around in Greenplum Database.
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "cdb/cdbheap.h"
#include "cdb/cdblink.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/nodeMotion.h"
#include "optimizer/clauses.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/memutils.h"
#include "utils/debugbreak.h"

/* #define MEASURE_MOTION_TIME */

#ifdef MEASURE_MOTION_TIME
#include <unistd.h>				/* gettimeofday */
#endif

/* #define CDB_MOTION_DEBUG */

#ifdef CDB_MOTION_DEBUG
#include "lib/stringinfo.h"     /* StringInfo */
#endif

/*
 * CdbTupleHeapInfo
 *
 * A priority queue element holding the next tuple of the
 * sorted tuple stream received from a particular sender.
 * Used by sorted receiver (Merge Receive).
 */
typedef struct CdbTupleHeapInfo
{
	/* Next tuple from this sender */
    HeapTuple	tuple;

    /* Which sender did this tuple come from? */
	int			sourceRouteId;

} CdbTupleHeapInfo;

/*
 * CdbMergeComparatorContext
 *
 * This contains the information necessary to compare
 * two tuples (other than the tuples themselves).
 * It includes :
 *		1) the number and array of indexes into the tuples columns
 *			that are the basis for the ordering
 *			(numSortCols, sortColIdx)
 *		2) the kind and FmgrInfo of the compare function
 *			for each column being ordered
 *			(sortFnKinds, sortFunctions)
 *		3) the tuple desc
 *			(tupDesc)
 * Used by sorted receiver (Merge Receive).  It is passed as the
 * context argument to the key comparator.
 */
typedef struct CdbMergeComparatorContext
{
	FmgrInfo           *sortFunctions;
	SortFunctionKind   *sortFnKinds;
	int			        numSortCols;
	AttrNumber         *sortColIdx;
	TupleDesc	        tupDesc;
} CdbMergeComparatorContext;

static CdbMergeComparatorContext *
CdbMergeComparator_CreateContext(TupleDesc      tupDesc,
                                 int            numSortCols,
                                 AttrNumber    *sortColIdx,
                                 Oid           *sortOperators);

static void
CdbMergeComparator_DestroyContext(CdbMergeComparatorContext *ctx);


/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static TupleTableSlot *execMotionSender(MotionState * node);
static TupleTableSlot *execMotionUnsortedReceiver(MotionState * node);
static TupleTableSlot *execMotionSortedReceiver(MotionState * node);

static void execMotionSortedReceiverFirstTime(MotionState * node);

static int
CdbMergeComparator(void *lhs, void *rhs, void *context);

static void doSendEndOfStream(Motion * motion, MotionState * node);
static void doSendTuple(Motion * motion, MotionState * node, TupleTableSlot *outerTupleSlot);


/*=========================================================================
 */

#ifdef CDB_MOTION_DEBUG
static void
formatTuple(StringInfo buf, HeapTuple tup, TupleDesc tupdesc, Oid *outputFunArray)
{
    int         i;

    for (i = 0; i < tupdesc->natts; i++)
    {
        bool    isnull;
        Datum   d = heap_getattr(tup, i+1, tupdesc, &isnull);

        if (d && !isnull)
        {
            Datum   ds = OidFunctionCall1(outputFunArray[i], d);
            char   *s = DatumGetCString(ds);
            char   *name = NameStr(tupdesc->attrs[i]->attname);

            if (name && *name)
                appendStringInfo(buf, "  %s=\"%.30s\"", name, s);
            else
                appendStringInfo(buf, "  \"%.30s\"", s);
            pfree(s);
        }
    }
    appendStringInfoChar(buf, '\n');
}
#endif


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

	/*
	 * at the top here we basically decide: -- SENDER vs. RECEIVER and --
	 * SORTED vs. UNSORTED
	 */
	if (node->mstype == MOTIONSTATE_RECV)
	{
		TupleTableSlot *tuple;
#ifdef MEASURE_MOTION_TIME
		struct timeval startTime;
		struct timeval stopTime;

		gettimeofday(&startTime, NULL);
#endif

		if (node->ps.state->active_recv_id >= 0)
		{
			if (node->ps.state->active_recv_id != motion->motionID)
			{
				elog(LOG, "DEADLOCK HAZARD: Updating active_motion_id from %d to %d",
					 node->ps.state->active_recv_id, motion->motionID);
				node->ps.state->active_recv_id = motion->motionID;
			}
		} else
			node->ps.state->active_recv_id = motion->motionID;

		if (motion->sendSorted)
			tuple = execMotionSortedReceiver(node);
		else
			tuple = execMotionUnsortedReceiver(node);

		if (tuple == NULL)
			node->ps.state->active_recv_id = -1;

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
	else if(node->mstype == MOTIONSTATE_SEND)
	{
		return execMotionSender(node);
	}

	Assert(!"Non-active motion is executed");
	return NULL;
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

	gettimeofday(&time1, NULL);
#endif

	Assert(node->ps.state->interconnect_context);

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
		if (done || TupIsNull(outerTupleSlot))
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

	Assert(node->stopRequested || node->numTuplesFromChild == node->numTuplesToAMS);

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

	Assert(node->ps.state->motionlayer_context);
	Assert(node->ps.state->interconnect_context);

	if (node->stopRequested) 
	{
		SendStopMessage(node->ps.state->motionlayer_context,
						node->ps.state->interconnect_context,
						motion->motionID);
		return NULL;
	}

	recvRC = RecvTupleFrom(node->ps.state->motionlayer_context,
						   node->ps.state->interconnect_context,
						   motion->motionID, &tuple, ANY_ROUTE);

	if (recvRC == END_OF_STREAM)
	{
#ifdef CDB_MOTION_DEBUG
        if (gp_log_interconnect >= GPVARS_VERBOSITY_DEBUG)
		    elog(DEBUG4, "motionID=%d saw end of stream", motion->motionID);
#endif
		Assert(node->numTuplesFromAMS == node->numTuplesToParent);
        Assert(node->numTuplesFromChild == 0);
        Assert(node->numTuplesToAMS == 0);
		return NULL;
	}

    node->numTuplesFromAMS++;
    node->numTuplesToParent++;

	/* store it in our result slot and return this. */
	slot = node->ps.ps_ResultTupleSlot;
    slot = ExecStoreTuple(tuple, slot, InvalidBuffer, true /* shouldFree */);

#ifdef CDB_MOTION_DEBUG
    if (node->numTuplesToParent <= 20)
    {
        StringInfoData  buf;

        initStringInfo(&buf);
        appendStringInfo(&buf, "   motion%-3d rcv      %5d.",
                         motion->motionID,
                         node->numTuplesToParent);
        formatTuple(&buf, tuple, ExecGetResultType(&node->ps),
                    node->outputFunArray);
        elog(DEBUG3, buf.data);
        pfree(buf.data);
    }
#endif

	return slot;
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
 * We keep track of which one was selected, this will be slot we will need
 * to fill during the next call.
 *
 * Subsuquent calls to this function (after the 1st time) will start by
 * trying to receive a tuple for the slot that was emptied the previous call.
 * Then we again select the lowest value and return that tuple.
 *
 */
static TupleTableSlot *
execMotionSortedReceiver(MotionState * node)
{
	TupleTableSlot *slot;
    CdbHeap        *hp = node->tupleheap;
	HeapTuple	tuple,
				inputTuple;
	Motion	   *motion = (Motion *) node->ps.plan;
	ReceiveReturnCode recvRC;
	CdbTupleHeapInfo *tupHeapInfo;

	/* Notify senders and return EOS if caller doesn't want any more data. */
    if (node->stopRequested)
	{
		
		SendStopMessage(node->ps.state->motionlayer_context,
						node->ps.state->interconnect_context,
						motion->motionID);
		return NULL;
	}

	/* On first call, fill the priority queue with each sender's first tuple. */
	if (!node->tupleheapReady)
	{
		execMotionSortedReceiverFirstTime(node);
	}

    /*
     * Delete from the priority queue the element that we fetched last
     * time.  Receive and insert the next tuple from that same sender.
     */
    else
	{
        /* Old element is still at the head of the pq. */
        AssertState(NULL != (tupHeapInfo = CdbHeap_Min(CdbTupleHeapInfo, hp)) &&
                    tupHeapInfo->tuple == NULL &&
                    tupHeapInfo->sourceRouteId == node->routeIdNext);

        /* Receive the successor of the tuple that we returned last time. */
        recvRC = RecvTupleFrom(node->ps.state->motionlayer_context,
							   node->ps.state->interconnect_context,
							   motion->motionID, 
							   &inputTuple,
							   node->routeIdNext);

        /* Substitute it in the pq for its predecessor. */
		if (recvRC == GOT_TUPLE)
		{
            CdbTupleHeapInfo info;

            info.tuple = inputTuple;
            info.sourceRouteId = node->routeIdNext;

            CdbHeap_DeleteMinAndInsert(hp, &info);

            node->numTuplesFromAMS++;

#ifdef CDB_MOTION_DEBUG
            if (node->numTuplesFromAMS <= 20)
            {
                StringInfoData  buf;

                initStringInfo(&buf);
                appendStringInfo(&buf, "   motion%-3d rcv<-%-3d %5d.",
                                 motion->motionID,
                                 node->routeIdNext,
                                 node->numTuplesFromAMS);
                formatTuple(&buf, inputTuple, ExecGetResultType(&node->ps),
                            node->outputFunArray);
                elog(DEBUG3, buf.data);
                pfree(buf.data);
            }
#endif
		}

        /* At EOS, drop this sender from the priority queue. */
        else if (!CdbHeap_IsEmpty(hp))
            CdbHeap_DeleteMin(hp);
	}

    /* Finished if all senders have returned EOS. */
    if (CdbHeap_IsEmpty(hp))
    {
        Assert(node->numTuplesFromAMS == node->numTuplesToParent);
		Assert(node->numTuplesFromChild == 0);
        Assert(node->numTuplesToAMS == 0);
        return NULL;
    }

    /*
     * Our next result tuple, with lowest key among all senders, is now
     * at the head of the priority queue.  Get it from there.
     *
     * We transfer ownership of the tuple from the pq element to
     * our caller, but the pq element itself will remain in place
     * until the next time we are called, to avoid an unnecessary
     * rearrangement of the priority queue.
     */
	tupHeapInfo = CdbHeap_Min(CdbTupleHeapInfo, hp);	
    tuple = tupHeapInfo->tuple;
	node->routeIdNext = tupHeapInfo->sourceRouteId;

    /* Zap dangling tuple ptr for safety. PQ element doesn't own it anymore. */
    tupHeapInfo->tuple = NULL;

    /* Update counters. */
    node->numTuplesToParent++;

    /* Store tuple in our result slot. */
	slot = node->ps.ps_ResultTupleSlot;
	slot = ExecStoreTuple(tuple, slot, InvalidBuffer, true /* shouldFree */);

#ifdef CDB_MOTION_DEBUG
    if (node->numTuplesToParent <= 20)
    {
        StringInfoData  buf;

        initStringInfo(&buf);
        appendStringInfo(&buf, "   motion%-3d mrg<-%-3d %5d.",
                         motion->motionID,
                         node->routeIdNext,
                         node->numTuplesToParent);
        formatTuple(&buf, tuple, ExecGetResultType(&node->ps),
                    node->outputFunArray);
        elog(DEBUG3, buf.data);
        pfree(buf.data);
    }
#endif

    /* Return result slot. */
    return slot;
}                               /* execMotionSortedReceiver */


void
execMotionSortedReceiverFirstTime(MotionState * node)
{
	HeapTuple	inputTuple;
    CdbHeap    *hp = node->tupleheap;
	Motion	   *motion = (Motion *) node->ps.plan;
	int			mySegIdx;
	int			i;
    int         n = 0;

	ReceiveReturnCode recvRC;

	mySegIdx = Gp_segment;

	/*
	 * We need to get a tuple from every sender, and stick it into the heap.
	 */
	for (i = 0; i < motion->numInputSegs; i++)
	{
        int iSegIdx = motion->inputSegIdx ? motion->inputSegIdx[i] : i;

		/*
		 * another place where we are mapping segid space to routeid space. so
		 * route[x] = inputSegIdx[x] now.
		 */
		recvRC = RecvTupleFrom(node->ps.state->motionlayer_context,
							   node->ps.state->interconnect_context,
							   motion->motionID, &inputTuple, iSegIdx);

		if (recvRC == GOT_TUPLE)
		{
            CdbTupleHeapInfo   *infoArray = (CdbTupleHeapInfo *)hp->slotArray;
            CdbTupleHeapInfo   *info = &infoArray[n++];

            info->tuple = inputTuple;
            info->sourceRouteId = iSegIdx;

            node->numTuplesFromAMS++;

#ifdef CDB_MOTION_DEBUG
            if (node->numTuplesFromAMS <= 20)
            {
                StringInfoData  buf;

                initStringInfo(&buf);
                appendStringInfo(&buf, "   motion%-3d rcv<-%-3d %5d.",
                                 motion->motionID,
                                 iSegIdx,
                                 node->numTuplesFromAMS);
                formatTuple(&buf, inputTuple, ExecGetResultType(&node->ps),
                            node->outputFunArray);
                elog(DEBUG3, buf.data);
                pfree(buf.data);
            }
#endif
		}
	}

    /*
     * Rearrange the infoArray to satisfy the heap property.
     * This is quicker than inserting the initial elements one by one.
     */
    CdbHeap_Heapify(hp, n);

	node->tupleheapReady = true;
}                               /* execMotionSortedReceiverFirstTime */


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
ExecInitMotion(Motion * node, EState *estate, int eflags)
{
	MotionState *motionstate = NULL;
	TupleDesc	tupDesc;
	Slice	   *sendSlice = NULL;
    Slice      *recvSlice = NULL;
    Slice      *thisSlice = NULL;
    Slice      *thatSlice = NULL;
    SliceTable *sliceTable = estate->es_sliceTable;
	int			prior_es_cur_slice_idx = estate->es_cur_slice_idx;	/* save */
	int			prior_es_cur_root_idx = estate->es_cur_root_idx;	/* save */

	ListCell 	*sliceCell;
#ifdef CDB_MOTION_DEBUG
	int			i;
#endif

	Assert(node->motionID > 0);
	Assert(node->motionID <= sliceTable->nMotions);
    Assert(node->numInputSegs > 0);

    /* Look up the sending gang's slice table entry. */
    sendSlice = (Slice *)list_nth(sliceTable->slices, node->motionID);

    Assert(IsA(sendSlice, Slice));
	Assert(sendSlice->sliceIndex == node->motionID);

	/*
	 * First check if this is multiplexed.  For multiplexed motion, just create motion
	 * once
	 */
	if (node->plan.shared_as_id >= 0)
	{
		ShareNodeEntry *snEntry = ExecGetShareNodeEntry(estate, node->plan.shared_as_id, true);
		motionstate = (MotionState *) snEntry->shareState; 

		if (motionstate)
		{
#ifdef DEBUG
			if (Gp_role == GP_ROLE_DISPATCH)
			{
				/* get recv slice */ 
				recvSlice = (Slice *) list_nth(sliceTable->slices, estate->es_cur_slice_idx);

				Assert(IsA(recvSlice, Slice));
				Assert(recvSlice->sliceIndex == estate->es_cur_slice_idx);
				Assert(recvSlice->sliceIndex != sendSlice->sliceIndex);
				Assert(recvSlice->rootIndex == 0 ||
						(recvSlice->rootIndex > sliceTable->nMotions &&
						 recvSlice->rootIndex < list_length(sliceTable->slices)));

				Assert(list_member_int(recvSlice->children, sendSlice->sliceIndex));
			}
#endif
			return motionstate;
		}
	}

	/*
	 * create state structure
	 */
	motionstate = makeNode(MotionState);
	motionstate->ps.plan = (Plan *) node;
	motionstate->ps.state = estate;
	motionstate->mstype = MOTIONSTATE_NONE;
	motionstate->stopRequested = false;
	motionstate->sliceEntries = NULL;

	foreach(sliceCell, node->sliceEntries)
	{	
		MotionStateSliceEntry *stateSlice;
		MotionSliceEntry *sliceEntry = (MotionSliceEntry *) lfirst(sliceCell);
		Assert(IsA(sliceEntry, MotionSliceEntry));

		stateSlice = makeNode(MotionStateSliceEntry);
		stateSlice->hashExpr = NULL;
		stateSlice->cdbhash = NULL;

		motionstate->sliceEntries = lappend(motionstate->sliceEntries, stateSlice);
	}

	/* QD must fill in the global slice table. */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		MemoryContext   oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
        Flow           *sendFlow;

        /* Top node of subplan should have a Flow node. */
        Insist(node->plan.lefttree && node->plan.lefttree->flow);
        sendFlow = node->plan.lefttree->flow;

        /* Sending slice table entry hasn't been filled in yet. */
        Assert(sendSlice->rootIndex == -1);
		Assert(sendSlice->gangSize == 0);

		/* Look up the receiving (parent) gang's slice table entry. */
		recvSlice = (Slice *)list_nth(sliceTable->slices, estate->es_cur_slice_idx);

		Assert(IsA(recvSlice, Slice));
		Assert(recvSlice->sliceIndex == estate->es_cur_slice_idx);
        Assert(recvSlice->rootIndex == 0 ||
               (recvSlice->rootIndex > sliceTable->nMotions &&
                recvSlice->rootIndex < list_length(sliceTable->slices)));

		/* Sending slice become a children of recv slice */
		recvSlice->children = lappend_int(recvSlice->children, sendSlice->sliceIndex);
		sendSlice->parentIndex = estate->es_cur_slice_idx;
        sendSlice->rootIndex = recvSlice->rootIndex;

		/* The gang beneath a Motion will be a reader. */
		sendSlice->gangType = GANGTYPE_PRIMARY_READER;
        sendSlice->gangSize = node->numInputSegs;

        /* Does sending slice need 1-gang with read-only access to entry db? */
        if (sendFlow->flotype == FLOW_SINGLETON &&
            sendFlow->segindex == -1)
            sendSlice->gangType = GANGTYPE_ENTRYDB_READER;

		if (list_length(node->sliceEntries) == 1)
		{

			MotionSliceEntry *sliceEntry = (MotionSliceEntry *) linitial(node->sliceEntries);

			if (sliceEntry->motionType == MOTIONTYPE_FIXED)
			{
				if (sliceEntry->numOutputSegs == 1)
				{
					/* Sending to a single receiving process on the entry db? */
					/* Is receiving slice a root slice that runs here in the qDisp? */
					if (recvSlice->sliceIndex == recvSlice->rootIndex)
					{
						motionstate->mstype = MOTIONSTATE_RECV; 
						thisSlice = recvSlice;
						thatSlice = sendSlice;
						Assert(recvSlice->gangType == GANGTYPE_UNALLOCATED);
						Assert(sliceEntry->outputSegIdx[0] == -1);
					}
					else
					{
						Assert(recvSlice->gangSize == 1);
						Assert(sliceEntry->outputSegIdx[0] >= 0
								? recvSlice->gangType == GANGTYPE_PRIMARY_READER
								: recvSlice->gangType == GANGTYPE_ENTRYDB_READER);
					}
				}
			}
		}

		estate->es_cur_slice_idx = sendSlice->sliceIndex;
		MemoryContextSwitchTo(oldcxt);
	}

    /* QE must fill in map from motionID to MotionState node. */
	else
	{
        Insist(Gp_role == GP_ROLE_EXECUTE);
		Assert(estate->es_slice_map != NULL);
		Assert(estate->es_slice_map[0] == NULL);

		estate->es_slice_map[node->motionID] = motionstate;
		recvSlice = (Slice *)list_nth(sliceTable->slices, sendSlice->parentIndex);

		if(recvSlice->sliceIndex == sliceTable->localSlice)
		{
			/* this is recv */
			motionstate->mstype = MOTIONSTATE_RECV;
			thisSlice = recvSlice;
			thatSlice = sendSlice;
		}
		else if (sendSlice->sliceIndex == sliceTable->localSlice)
		{
			/* this is send */
			motionstate->mstype = MOTIONSTATE_SEND;
            thisSlice = sendSlice;
            thatSlice = recvSlice;
        }
		/* TODO: If neither sending nor receiving, don't bother to initialize. */
	}

    motionstate->tupleheapReady = false;
	motionstate->sentEndOfStream = false;

	motionstate->otherTime.tv_sec = 0;
	motionstate->otherTime.tv_usec = 0;
	motionstate->motionTime.tv_sec = 0;
	motionstate->motionTime.tv_usec = 0;

	motionstate->numTuplesFromChild = 0;
	motionstate->numTuplesToAMS = 0;
	motionstate->numTuplesFromAMS = 0;
	motionstate->numTuplesToParent = 0;

	motionstate->stopRequested = false;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &motionstate->ps);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &motionstate->ps);

	outerPlanState(motionstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * restore state that might have changed in recursion for our caller
	 * (and for ourself).
	 */
	estate->es_cur_slice_idx = prior_es_cur_slice_idx;
	estate->es_cur_root_idx = prior_es_cur_root_idx;

	/*
	 * initialize tuple type.  no need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecAssignResultTypeFromTL(&motionstate->ps);
	motionstate->ps.ps_ProjInfo = NULL;
    tupDesc = ExecGetResultType(&motionstate->ps);

	/* Set up motion send data structures */
	if (motionstate->mstype == MOTIONSTATE_SEND) 
	{
		int			nkeys;
		ListCell 	*sliceCell, *stateSliceCell;

		sliceCell = list_head(node->sliceEntries);
		stateSliceCell = list_head(motionstate->sliceEntries);

		while(sliceCell != NULL)
		{
			MotionSliceEntry *sliceEntry = (MotionSliceEntry *) lfirst(sliceCell);
			MotionStateSliceEntry *stateSliceEntry = (MotionStateSliceEntry *) lfirst(stateSliceCell);
			if (sliceEntry->motionType == MOTIONTYPE_HASH)
			{
				/*
				 * Get info about the hash functions to be used for each hash key.
				 */
				/* Assert(node->hashDataTypes != NULL); */
				Assert(sliceEntry->numHashSegs > 0);

				nkeys = list_length(sliceEntry->hashDataTypes);
		
				if (nkeys > 0)
					stateSliceEntry->hashExpr = (List *) ExecInitExpr((Expr *) sliceEntry->hashExpr,
							(PlanState *) motionstate);

				/*
				 * Create hash API reference
				 */
				stateSliceEntry->cdbhash = makeCdbHash(sliceEntry->numHashSegs, HASH_FNV_1);
			}

			sliceCell = lnext(sliceCell);
			stateSliceCell = lnext(stateSliceCell);
		}

#ifdef MEASURE_MOTION_TIME
		/*
		 * Create buckets to hold counts of tuples hashing to each
		 * destination segindex (for debugging purposes)
		 */
		motionstate->numTuplesByHashSegIdx = (int *) palloc0(node->numHashSegs * sizeof(int));
#endif
    }

	/* Merge Receive: Set up the key comparator and priority queue. */
    if (node->sendSorted && motionstate->mstype == MOTIONSTATE_RECV) 
	{
        CdbMergeComparatorContext  *mcContext;

        /* Allocate context object for the key comparator. */
        mcContext = CdbMergeComparator_CreateContext(tupDesc,
                                                     node->numSortCols,
                                                     node->sortColIdx,
                                                     node->sortOperators);

        /* Create the priority queue structure. */
        motionstate->tupleheap = CdbHeap_Create(CdbMergeComparator,
                                                mcContext,
                                                node->numInputSegs,
                                                sizeof(CdbTupleHeapInfo),
                                                NULL);
	}

	/*
	 * Perform per-node initialization in the motion layer.
	 */
	UpdateMotionLayerNode(motionstate->ps.state->motionlayer_context, node->motionID, node->sendSorted, tupDesc);

#ifdef CDB_MOTION_DEBUG
    motionstate->outputFunArray = (Oid *)palloc(tupDesc->natts * sizeof(Oid));
    for (i = 0; i < tupDesc->natts; i++)
    {
        bool    typisvarlena;

        getTypeOutputInfo(tupDesc->attrs[i]->atttypid,
                          &motionstate->outputFunArray[i],
                          &typisvarlena);
    }
#endif

	/* For multiplexed node, register with estate */
	if (node->plan.shared_as_id >= 0) 
	{
		ShareNodeEntry *snEntry = ExecGetShareNodeEntry(estate, node->plan.shared_as_id, true);
		snEntry->sharePlan = (Node *) node;
		snEntry->shareState = (Node *) motionstate;
	}

	return motionstate;
}

#define MOTION_NSLOTS 1

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
	Motion	   *motion = (Motion *)node->ps.plan;
	uint16		motNodeID = motion->motionID;
#ifdef MEASURE_MOTION_TIME
	double		otherTimeSec;
	double		motionTimeSec;
#endif
	ListCell *sliceCell;

	if (outerPlanState(node) == NULL)
		return; 

	ExecFreeExprContext(&node->ps);

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

#ifdef MEASURE_MOTION_TIME
	motionTimeSec = (double) node->motionTime.tv_sec + (double) node->motionTime.tv_usec / 1000000.0;

	if (node->mstype == MOTIONSTATE_RECV) 
	{
		elog(DEBUG1,
			 "Motion Node %d (RECEIVER) Statistics:\n"
			 "Timing:  \n"
			 "\t Time receiving the tuple: %f sec\n"
			 "Counters: \n"
			 "\tnumTuplesFromChild: %d\n"
			 "\tnumTuplesFromAMS: %d\n"
			 "\tnumTuplesToAMS: %d\n"
			 "\tnumTuplesToParent: %d\n",
			 motNodeID,
			 motionTimeSec,
			 node->numTuplesFromChild,
			 node->numTuplesFromAMS,
			 node->numTuplesToAMS,
			 node->numTuplesToParent
			);
	}
	else if(node->mstype == MOTIONSTATE_SEND)
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
			 "\tnumTuplesToAMS: %d\n",
			 motNodeID,
			 otherTimeSec,
			 motionTimeSec,
			 (double) (motionTimeSec / (otherTimeSec + motionTimeSec)) * 100,
			 node->numTuplesFromChild,
			 node->numTuplesToAMS
			);
	}

	if (node->numTuplesByHashSegIdx != NULL)
	{
		int i;
        for (i = 0; i < motion->numHashSegs; i++)
		{
			elog(DEBUG1, "Motion Node %3d Hash Bucket %3d: %10d tuples",
                 motNodeID, i, node->numTuplesByHashSegIdx[i]);
		}
		pfree(node->numTuplesByHashSegIdx);
		node->numTuplesByHashSegIdx = NULL;
	}
#endif /* MEASURE_MOTION_TIME */

	/* Merge Receive: Free the priority queue and associated structures. */
    if (node->tupleheap != NULL)
	{
        CdbMergeComparator_DestroyContext(node->tupleheap->comparatorContext);
        CdbHeap_Destroy(node->tupleheap);
        node->tupleheap = NULL;
	}

	/*
	 * Free up this motion node's resources in the Motion Layer.
	 *
	 * TODO: For now, we don't flush the comm-layer.  NO ERRORS DURING AMS!!!
	 */
	EndMotionLayerNode(node->ps.state->motionlayer_context, motNodeID, /* flush-comm-layer */ false);

#ifdef CDB_MOTION_DEBUG
    if (node->outputFunArray)
        pfree(node->outputFunArray);
#endif

	/* Free the slices and routes */
	foreach(sliceCell, node->sliceEntries)
	{
		MotionStateSliceEntry *sliceEntry = lfirst(sliceCell);
		if(sliceEntry->cdbhash != NULL)
		{
			pfree(sliceEntry->cdbhash);
			sliceEntry->cdbhash = NULL;
		}
		pfree(sliceEntry);
	}

	outerPlanState(node) = NULL;
}



/*=========================================================================
 * HELPER FUNCTIONS
 */

/*
 * CdbMergeComparator:
 * Used to compare tuples for a sorted motion node.
 */
int
CdbMergeComparator(void *lhs, void *rhs, void *context)
{
    CdbMergeComparatorContext  *ctx = (CdbMergeComparatorContext *)context;
    CdbTupleHeapInfo   *linfo = (CdbTupleHeapInfo *) lhs;
    CdbTupleHeapInfo   *rinfo = (CdbTupleHeapInfo *) rhs;
    HeapTuple           ltup = linfo->tuple;
    HeapTuple           rtup = rinfo->tuple;
    FmgrInfo           *sortFunctions;
    SortFunctionKind   *sortFnKinds;
    int                 numSortCols;
    AttrNumber         *sortColIdx;
    TupleDesc           tupDesc;
    int                 nkey;

    Assert(ltup && rtup);

    sortFunctions   = ctx->sortFunctions;
    sortFnKinds     = ctx->sortFnKinds;
    numSortCols     = ctx->numSortCols;
    sortColIdx      = ctx->sortColIdx;
    tupDesc         = ctx->tupDesc;

    for (nkey = 0; nkey < numSortCols; nkey++)
    {
        AttrNumber  attno = sortColIdx[nkey];
        Datum       datum1,
                    datum2;
        bool        isnull1,
                    isnull2;
        int32       compare;

        datum1 = heap_getattr(ltup, attno, tupDesc, &isnull1);
        datum2 = heap_getattr(rtup, attno, tupDesc, &isnull2);

        compare = ApplySortFunction(&sortFunctions[nkey],
                                    sortFnKinds[nkey],
                                    datum1, isnull1,
                                    datum2, isnull2);
        if (compare != 0)
            return compare;
    }

    return 0;
}
                               /* CdbMergeComparator */


/* Create context object for use by CdbMergeComparator */
CdbMergeComparatorContext *
CdbMergeComparator_CreateContext(TupleDesc      tupDesc,
                                 int            numSortCols,
                                 AttrNumber    *sortColIdx,
                                 Oid           *sortOperators)
{
    CdbMergeComparatorContext  *ctx;
    int     i;

    Assert(tupDesc &&
           numSortCols > 0 &&
           sortColIdx &&
           sortOperators);

    /* Allocate and initialize the context object. */
    ctx = (CdbMergeComparatorContext *)palloc0(sizeof(*ctx));

    ctx->numSortCols = numSortCols;
    ctx->sortColIdx = sortColIdx;
    ctx->tupDesc = tupDesc;

    /* Allocate the sort function arrays. */
    ctx->sortFunctions = (FmgrInfo *)palloc0(numSortCols * sizeof(FmgrInfo));
    ctx->sortFnKinds = (SortFunctionKind *)palloc0(numSortCols * sizeof(SortFunctionKind));

    /* Load the sort functions. */
    for (i = 0; i < numSortCols; i++)
    {
        RegProcedure    sortFunction;

        Assert(sortOperators[i] && sortColIdx[i]);

        /* select a function that implements the sort operator */
        SelectSortFunction(sortOperators[i],
                           &sortFunction,
                           &ctx->sortFnKinds[i]);

        fmgr_info(sortFunction, &ctx->sortFunctions[i]);
    }

    return ctx;
}                               /* CdbMergeComparator_CreateContext */


void
CdbMergeComparator_DestroyContext(CdbMergeComparatorContext *ctx)
{
    if (!ctx)
        return;
    if (ctx->sortFnKinds)
        pfree(ctx->sortFnKinds);
    if (ctx->sortFunctions)
        pfree(ctx->sortFunctions);
}                               /* CdbMergeComparator_DestroyContext */


/*
 * Experimental code that will be replaced later with new hashing mechanism
 */
uint32
evalHashKey(void *vp_econtext, List *hashkeys, List *hashtypes, CdbHash * h)
{
	ListCell   *hk;
	ListCell   *ht;
	MemoryContext oldContext;
	ExprContext *econtext = (ExprContext *) vp_econtext;

	Assert(IsA(econtext, ExprContext));

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	cdbhashinit(h);

	/*
	 * If we have 1 or more distribution keys for this relation, hash
	 * them. However, If this happens to be a relation with an empty
	 * policy (partitioning policy with a NULL distribution key list)
	 * then we have no hash key value to feed in, so use cdbhashnokey()
	 * to assign a hash value for us.
	 */
	if (list_length(hashkeys) > 0)
	{	
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
	}
	else
	{
		cdbhashnokey(h);
	}

	MemoryContextSwitchTo(oldContext);

	return cdbhashreduce(h);
}


void
doSendEndOfStream(Motion * motion, MotionState * node)
{
	/*
	 * We have no more child tuples, but we have not successfully sent an
	 * End-of-Stream token yet.
	 */
	SendEndOfStream(node->ps.state->motionlayer_context,
					node->ps.state->interconnect_context,
					motion->motionID);
	node->sentEndOfStream = true;
}


/*
 * A crufty confusing part of the current code is how contentId is used within
 * the motion structures and then how that gets translated to targetRoutes by
 * this motion nodes.
 *
 * WARNING: There are ALOT of assumptions in here about how the motion node
 *			instructions are encoded into motion and stuff.
 *
 * We first find out what is the intended receiving slice of the send.  If
 * the motion is multiplexed, the the first column should be a tinyint (byte)
 * to say which slice is this for, otherwise, it should the the first (only)
 * recv slice.
 *
 * Once we determined the recv slice, there are 3 types of sending that can happen 
 * here:
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
	int16		    targetRoute;
    HeapTuple       tuple;
    SendReturnCode  sendRC;
	ListCell       *sliceCell; 
	ListCell 	   *stateSliceCell;
	ExprContext    *econtext = node->ps.ps_ExprContext;
	List 		   *seenTarget = NULL;
	
	/* We got a tuple from the child-plan. */
	node->numTuplesFromChild++;

	tuple = ExecFetchSlotTuple(outerTupleSlot);

	/* initialize to the first slice */
	sliceCell = list_head(motion->sliceEntries);
	stateSliceCell = list_head(node->sliceEntries);
	
	while(sliceCell != NULL)
	{
		MotionSliceEntry *sliceEntry = lfirst(sliceCell);
		MotionStateSliceEntry *stateSliceEntry = lfirst(stateSliceCell);

		Assert(IsA(sliceEntry, MotionSliceEntry));
		Assert(IsA(stateSliceEntry, MotionStateSliceEntry));

		if (sliceEntry->motionType == MOTIONTYPE_FIXED)
		{
			if (sliceEntry->numOutputSegs == 0) /* Broadcast */
			{
				targetRoute = BROADCAST_SEGIDX;
			}
			else /* Fixed Motion. */
			{
				Assert(sliceEntry->numOutputSegs == 1);
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
		else /* Hash motion node */
		{
			uint32		hval = 0;

			/*
			 * hashSegIdx structure doesn not include support for the QD. so 0
			 * maps to segID with contentId = 0
			 */

			Assert(sliceEntry->numHashSegs > 0);
			Assert(sliceEntry->hashSegIdx != NULL);

			econtext->ecxt_outertuple = outerTupleSlot;
			hval = evalHashKey(econtext, stateSliceEntry->hashExpr,
						sliceEntry->hashDataTypes, stateSliceEntry->cdbhash);
			targetRoute = sliceEntry->hashSegIdx[hval];
		}

		if(seenTarget==NULL || list_find_int(seenTarget, targetRoute) < 0)
		{
			/* send the tuple out. */
			sendRC = SendTuple(node->ps.state->motionlayer_context,
					node->ps.state->interconnect_context,
					motion->motionID,
					tuple,
					targetRoute);

			Assert(sendRC == SEND_COMPLETE || sendRC == STOP_SENDING);
			if (sendRC == SEND_COMPLETE)
			{
				if(!seenTarget)
					node->numTuplesToAMS++;

#ifdef CDB_MOTION_DEBUG
				if (node->numTuplesToAMS <= 20)
				{
					StringInfoData  buf;

					initStringInfo(&buf);
					appendStringInfo(&buf, "   motion%-3d snd->%-3d, %5d.",
							motion->motionID,
							targetRoute,
							node->numTuplesToAMS);
					formatTuple(&buf, tuple, ExecGetResultType(&node->ps),
							node->outputFunArray);
					elog(DEBUG3, buf.data);
					pfree(buf.data);
				}
#endif
			}
			else
				node->stopRequested = true;

			/* Add to the seen target list */
			seenTarget = lappend_int(seenTarget, targetRoute);
		}

		sliceCell = lnext(sliceCell);
		stateSliceCell = lnext(stateSliceCell);
	}  /* end of loop for slice entries */
}

/*
 * ExecReScanMotion
 *
 * Motion nodes do not allow rescan after a tuple has been fetched.
 *
 * When the planner knows that a NestLoop cannot have more than one outer
 * tuple, it can omit the usual Materialize operator atop the inner subplan,
 * which can lead to invocation of ExecReScanMotion before the motion node's
 * first tuple is fetched.  Rescan can be implemented as a no-op in this case.
 * (After ExecNestLoop fetches an outer tuple, it invokes rescan on the inner
 * subplan before fetching the first inner tuple.  That doesn't bother us,
 * provided there is only one outer tuple.)
 */
void
ExecReScanMotion(MotionState *node, ExprContext *exprCtxt)
{
	if (node->mstype != MOTIONSTATE_RECV || 
		node->numTuplesToParent != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Illegal rescan of motion node: invalid plan."),
				 errhint("likely caused by bad NL-join, try setting enable_nestloop off")));
	}
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

	/* pass down */
	SendStopMessage(node->ps.state->motionlayer_context,
					node->ps.state->interconnect_context,
					motion->motionID);
}
