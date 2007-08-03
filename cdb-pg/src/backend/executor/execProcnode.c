/*-------------------------------------------------------------------------
 *
 * execProcnode.c
 *	 contains dispatch functions which call the appropriate "initialize",
 *	 "get a tuple", and "cleanup" routines for the given node type.
 *	 If the node has children, then it will presumably call ExecInitNode,
 *	 ExecProcNode, or ExecEndNode on its subnodes and do the appropriate
 *	 processing.
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/execProcnode.c,v 1.59 2006/10/04 00:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecCountSlotsNode -	count tuple slots needed by plan tree
 *		ExecInitNode	-		initialize a plan node and its subplans
 *		ExecProcNode	-		get a tuple by executing the plan node
 *		ExecEndNode		-		shut down a plan node and its subplans
 *		ExecSquelchNode		-	notify subtree that no more tuples are needed
 *		ExecStateTreeWalker -	call given function for each node of plan state
 *
 *	 NOTES
 *		This used to be three files.  It is now all combined into
 *		one file so that it is easier to keep ExecInitNode, ExecProcNode,
 *		and ExecEndNode in sync when new nodes are added.
 *
 *	 EXAMPLE
 *		Suppose we want the age of the manager of the shoe department and
 *		the number of employees in that department.  So we have the query:
 *
 *				select DEPT.no_emps, EMP.age
 *				where EMP.name = DEPT.mgr and
 *					  DEPT.name = "shoe"
 *
 *		Suppose the planner gives us the following plan:
 *
 *						Nest Loop (DEPT.mgr = EMP.name)
 *						/		\
 *					   /		 \
 *				   Seq Scan		Seq Scan
 *					DEPT		  EMP
 *				(name = "shoe")
 *
 *		ExecutorStart() is called first.
 *		It calls InitPlan() which calls ExecInitNode() on
 *		the root of the plan -- the nest loop node.
 *
 *	  * ExecInitNode() notices that it is looking at a nest loop and
 *		as the code below demonstrates, it calls ExecInitNestLoop().
 *		Eventually this calls ExecInitNode() on the right and left subplans
 *		and so forth until the entire plan is initialized.	The result
 *		of ExecInitNode() is a plan state tree built with the same structure
 *		as the underlying plan tree.
 *
 *	  * Then when ExecRun() is called, it calls ExecutePlan() which calls
 *		ExecProcNode() repeatedly on the top node of the plan state tree.
 *		Each time this happens, ExecProcNode() will end up calling
 *		ExecNestLoop(), which calls ExecProcNode() on its subplans.
 *		Each of these subplans is a sequential scan so ExecSeqScan() is
 *		called.  The slots returned by ExecSeqScan() may contain
 *		tuples which contain the attributes ExecNestLoop() uses to
 *		form the tuples it returns.
 *
 *	  * Eventually ExecSeqScan() stops returning tuples and the nest
 *		loop join ends.  Lastly, ExecEnd() calls ExecEndNode() which
 *		calls ExecEndNestLoop() which in turn calls ExecEndNode() on
 *		its subplans which result in ExecEndSeqScan().
 *
 *		This should show how the executor works by having
 *		ExecInitNode(), ExecProcNode() and ExecEndNode() dispatch
 *		their work to the appopriate node support routines which may
 *		in turn call these routines themselves on their subplans.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeBitmapAnd.h"
#include "executor/nodeBitmapHeapscan.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeBitmapOr.h"
#include "executor/nodeExternalscan.h"
#include "executor/nodeFunctionscan.h"
#include "executor/nodeGroup.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeLimit.h"
#include "executor/nodeMaterial.h"
#include "executor/nodeMergejoin.h"
#include "executor/nodeMotion.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeAdaptiveNestloop.h"
#include "executor/nodeResult.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSetOp.h"
#include "executor/nodeShareInputScan.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/nodeSubqueryscan.h"
#include "executor/nodeTidscan.h"
#include "executor/nodeUnique.h"
#include "executor/nodeValuesscan.h"
#include "executor/nodeWindow.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "cdb/cdbvars.h"

#include "cdb/ml_ipc.h" /* interconnect context */

#include "utils/debugbreak.h"

#ifdef CDB_TRACE_EXECUTOR
#include "nodes/print.h"
static void ExecCdbTraceNode(PlanState *node, bool entry, TupleTableSlot *result);
#endif   /* CDB_TRACE_EXECUTOR */

/* ------------------------------------------------------------------------
 *		ExecInitNode
 *
 *		Recursively initializes all the nodes in the plan tree rooted
 *		at 'node'.
 *
 *		Inputs:
 *		  'node' is the current node of the plan produced by the query planner
 *		  'estate' is the shared execution state for the plan tree
 *		  'eflags' is a bitwise OR of flag bits described in executor.h
 *
 *		Returns a PlanState node corresponding to the given Plan node.
 * ------------------------------------------------------------------------
 */
PlanState *
ExecInitNode(Plan *node, EState *estate, int eflags)
{
	PlanState  *result;
	List	   *subps;
	ListCell   *l;

	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return NULL;

	switch (nodeTag(node))
	{
			/*
			 * control nodes
			 */
		case T_Result:
			result = (PlanState *) ExecInitResult((Result *) node,
												  estate, eflags);
			break;

		case T_Append:
			result = (PlanState *) ExecInitAppend((Append *) node,
												  estate, eflags);
			break;

		case T_BitmapAnd:
			result = (PlanState *) ExecInitBitmapAnd((BitmapAnd *) node,
													 estate, eflags);
			break;

		case T_BitmapOr:
			result = (PlanState *) ExecInitBitmapOr((BitmapOr *) node,
													estate, eflags);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			result = (PlanState *) ExecInitSeqScan((SeqScan *) node,
												   estate, eflags);
			break;
			
		case T_ExternalScan:
			result = (PlanState *) ExecInitExternalScan((ExternalScan *) node,
														estate, eflags);
			break;			

		case T_IndexScan:
			result = (PlanState *) ExecInitIndexScan((IndexScan *) node,
													 estate, eflags);
			break;

		case T_BitmapIndexScan:
			result = (PlanState *) ExecInitBitmapIndexScan((BitmapIndexScan *) node,
														   estate, eflags);
			break;

		case T_BitmapHeapScan:
			result = (PlanState *) ExecInitBitmapHeapScan((BitmapHeapScan *) node,
														  estate, eflags);
			break;

		case T_TidScan:
			result = (PlanState *) ExecInitTidScan((TidScan *) node,
												   estate, eflags);
			break;

		case T_SubqueryScan:
			result = (PlanState *) ExecInitSubqueryScan((SubqueryScan *) node,
														estate, eflags);
			break;

		case T_FunctionScan:
			result = (PlanState *) ExecInitFunctionScan((FunctionScan *) node,
														estate, eflags);
			break;

		case T_ValuesScan:
			result = (PlanState *) ExecInitValuesScan((ValuesScan *) node,
													  estate, eflags);
			break;

			/*
			 * join nodes
			 */
		case T_AdaptiveNestLoop:
			result = (PlanState *) ExecInitAdaptiveNestLoop((AdaptiveNestLoop *) node,
															estate, eflags);
			break;
		case T_NestLoop:
			result = (PlanState *) ExecInitNestLoop((NestLoop *) node,
													estate, eflags);
			break;

		case T_MergeJoin:
			result = (PlanState *) ExecInitMergeJoin((MergeJoin *) node,
													 estate, eflags);
			break;

		case T_HashJoin:
			result = (PlanState *) ExecInitHashJoin((HashJoin *) node,
													estate, eflags);
			break;

			/*
			 * share input nodes
			 */
		case T_ShareInputScan:
			result = (PlanState *) ExecInitShareInputScan((ShareInputScan *) node, estate, eflags);
			break;

			/*
			 * materialization nodes
			 */
		case T_Material:
			result = (PlanState *) ExecInitMaterial((Material *) node,
													estate, eflags);
			break;

		case T_Sort:
			result = (PlanState *) ExecInitSort((Sort *) node,
												estate, eflags);
			break;

		case T_Group:
			result = (PlanState *) ExecInitGroup((Group *) node,
												 estate, eflags);
			break;

		case T_Agg:
			result = (PlanState *) ExecInitAgg((Agg *) node,
											   estate, eflags);
			break;

		case T_Window:
			result = (PlanState *) ExecInitWindow((Window *) node,
											   estate, eflags);
			break;

		case T_Unique:
			result = (PlanState *) ExecInitUnique((Unique *) node,
												  estate, eflags);
			break;

		case T_Hash:
			result = (PlanState *) ExecInitHash((Hash *) node,
												estate, eflags);
			break;

		case T_SetOp:
			result = (PlanState *) ExecInitSetOp((SetOp *) node,
												 estate, eflags);
			break;

		case T_Limit:
			result = (PlanState *) ExecInitLimit((Limit *) node,
												 estate, eflags);
			break;

		case T_Motion:
			result = (PlanState *) ExecInitMotion((Motion *) node,
												  estate, eflags);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * Initialize any initPlans present in this node.  The planner put them in
	 * a separate list for us.
	 */
	subps = NIL;
	foreach(l, node->initPlan)
	{
		SubPlan    *subplan = (SubPlan *) lfirst(l);
		SubPlanState *sstate;

		Assert(IsA(subplan, SubPlan));
		sstate = ExecInitExprInitPlan(subplan, result);
		ExecInitSubPlan(sstate, estate, eflags);
		subps = lappend(subps, sstate);
	}
	result->initPlan = subps;

	/*
	 * Initialize any subPlans present in this node.  These were found by
	 * ExecInitExpr during initialization of the PlanState.  Note we must do
	 * this after initializing initPlans, in case their arguments contain
	 * subPlans (is that actually possible? perhaps not).
	 */
	foreach(l, result->subPlan)
	{
		SubPlanState *sstate = (SubPlanState *) lfirst(l);

		Assert(IsA(sstate, SubPlanState));
		ExecInitSubPlan(sstate, estate, eflags);
	}

	/* Set up instrumentation for this node if requested */
	if (estate->es_instrument)
		result->instrument = InstrAlloc(1);

	return result;
}

/* ----------------------------------------------------------------
 *		ExecSliceDependencyNode
 *
 *	 	Exec dependency, block till slice dependency are met	
 * ----------------------------------------------------------------
 */
void 
ExecSliceDependencyNode(PlanState *node)
{
	CHECK_FOR_INTERRUPTS();

	if(node == NULL)
		return;

	if(nodeTag(node) == T_ShareInputScanState)
		ExecSliceDependencyShareInputScan((ShareInputScanState *) node);
	else if(nodeTag(node) == T_SubqueryScanState)
	{
		SubqueryScanState *subq = (SubqueryScanState *) node;
		ExecSliceDependencyNode(subq->subplan);
	}
	else if(nodeTag(node) == T_AppendState)
	{
		int i=0;
		AppendState *app = (AppendState *) node;

		for(; i<app->as_nplans; ++i)
			ExecSliceDependencyNode(app->appendplans[i]);
	}

	ExecSliceDependencyNode(outerPlanState(node));
	ExecSliceDependencyNode(innerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecProcNode
 *
 *		Execute the given node to return a(nother) tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProcNode(PlanState *node)
{
	TupleTableSlot *result;

	CHECK_FOR_INTERRUPTS();

#ifdef CDB_TRACE_EXECUTOR
	ExecCdbTraceNode(node, true, NULL);
#endif   /* CDB_TRACE_EXECUTOR */

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node, NULL); /* let ReScan handle this */

	if (node->instrument)
		InstrStartNode(node->instrument);

	switch (nodeTag(node))
	{
			/*
			 * control nodes
			 */
		case T_ResultState:
			result = ExecResult((ResultState *) node);
			break;

		case T_AppendState:
			result = ExecAppend((AppendState *) node);
			break;

			/* BitmapAndState does not yield tuples */

			/* BitmapOrState does not yield tuples */

			/*
			 * scan nodes
			 */
		case T_SeqScanState:
			result = ExecSeqScan((SeqScanState *) node);
			break;

		case T_IndexScanState:
			result = ExecIndexScan((IndexScanState *) node);
			break;

		case T_ExternalScanState:
			result = ExecExternalScan((ExternalScanState *) node);
			break;
			
			/* BitmapIndexScanState does not yield tuples */

		case T_BitmapHeapScanState:
			result = ExecBitmapHeapScan((BitmapHeapScanState *) node);
			break;

		case T_TidScanState:
			result = ExecTidScan((TidScanState *) node);
			break;

		case T_SubqueryScanState:
			result = ExecSubqueryScan((SubqueryScanState *) node);
			break;

		case T_FunctionScanState:
			result = ExecFunctionScan((FunctionScanState *) node);
			break;

		case T_ValuesScanState:
			result = ExecValuesScan((ValuesScanState *) node);
			break;

			/*
			 * join nodes
			 */
		case T_AdaptiveNestLoopState:
			result = ExecAdaptiveNestLoop((AdaptiveNestLoopState *) node);
			break;
		case T_NestLoopState:
			result = ExecNestLoop((NestLoopState *) node);
			break;

		case T_MergeJoinState:
			result = ExecMergeJoin((MergeJoinState *) node);
			break;

		case T_HashJoinState:
			result = ExecHashJoin((HashJoinState *) node);
			break;

			/*
			 * shareinput nodes
			 */
		case T_ShareInputScanState:
			result = ExecShareInputScan((ShareInputScanState *) node);
			break;

			/*
			 * materialization nodes
			 */
		case T_MaterialState:
			result = ExecMaterial((MaterialState *) node);
			break;

		case T_SortState:
			result = ExecSort((SortState *) node);
			break;

		case T_GroupState:
			result = ExecGroup((GroupState *) node);
			break;

		case T_AggState:
			result = ExecAgg((AggState *) node);
			break;

		case T_WindowState:
			result = ExecWindow((WindowState *) node);
			break;

		case T_UniqueState:
			result = ExecUnique((UniqueState *) node);
			break;

		case T_HashState:
			result = ExecHash((HashState *) node);
			break;

		case T_SetOpState:
			result = ExecSetOp((SetOpState *) node);
			break;

		case T_LimitState:
			result = ExecLimit((LimitState *) node);
			break;

		case T_MotionState:
			result = ExecMotion((MotionState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;
			break;
	}

	if (node->instrument)
		InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

#ifdef CDB_TRACE_EXECUTOR
	ExecCdbTraceNode(node, false, result);
#endif   /* CDB_TRACE_EXECUTOR */

	return result;
}


/* ----------------------------------------------------------------
 *		MultiExecProcNode
 *
 *		Execute a node that doesn't return individual tuples
 *		(it might return a hashtable, bitmap, etc).  Caller should
 *		check it got back the expected kind of Node.
 *
 * This has essentially the same responsibilities as ExecProcNode,
 * but it does not do InstrStartNode/InstrStopNode (mainly because
 * it can't tell how many returned tuples to count).  Each per-node
 * function must provide its own instrumentation support.
 * ----------------------------------------------------------------
 */
Node *
MultiExecProcNode(PlanState *node)
{
	Node	   *result;

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node, NULL); /* let ReScan handle this */

	switch (nodeTag(node))
	{
			/*
			 * Only node types that actually support multiexec will be listed
			 */

		case T_HashState:
			result = MultiExecHash((HashState *) node);
			break;

		case T_BitmapIndexScanState:
			result = MultiExecBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapAndState:
			result = MultiExecBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			result = MultiExecBitmapOr((BitmapOrState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;
			break;
	}

	return result;
}


/*
 * ExecCountSlotsNode - count up the number of tuple table slots needed
 *
 * Note that this scans a Plan tree, not a PlanState tree, because we
 * haven't built the PlanState tree yet ...
 */
int
ExecCountSlotsNode(Plan *node)
{
	if (node == NULL)
		return 0;

	switch (nodeTag(node))
	{
			/*
			 * control nodes
			 */
		case T_Result:
			return ExecCountSlotsResult((Result *) node);

		case T_Append:
			return ExecCountSlotsAppend((Append *) node);

		case T_BitmapAnd:
			return ExecCountSlotsBitmapAnd((BitmapAnd *) node);

		case T_BitmapOr:
			return ExecCountSlotsBitmapOr((BitmapOr *) node);

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			return ExecCountSlotsSeqScan((SeqScan *) node);

		case T_ExternalScan:
			return ExecCountSlotsExternalScan((ExternalScan *) node);

		case T_IndexScan:
			return ExecCountSlotsIndexScan((IndexScan *) node);

		case T_BitmapIndexScan:
			return ExecCountSlotsBitmapIndexScan((BitmapIndexScan *) node);

		case T_BitmapHeapScan:
			return ExecCountSlotsBitmapHeapScan((BitmapHeapScan *) node);

		case T_TidScan:
			return ExecCountSlotsTidScan((TidScan *) node);

		case T_SubqueryScan:
			return ExecCountSlotsSubqueryScan((SubqueryScan *) node);

		case T_FunctionScan:
			return ExecCountSlotsFunctionScan((FunctionScan *) node);

		case T_ValuesScan:
			return ExecCountSlotsValuesScan((ValuesScan *) node);

			/*
			 * join nodes
			 */
		case T_AdaptiveNestLoop:
			return ExecCountSlotsAdaptiveNestLoop((AdaptiveNestLoop *) node);
		case T_NestLoop:
			return ExecCountSlotsNestLoop((NestLoop *) node);

		case T_MergeJoin:
			return ExecCountSlotsMergeJoin((MergeJoin *) node);

		case T_HashJoin:
			return ExecCountSlotsHashJoin((HashJoin *) node);

		/*
		 * share input nodes
		 */
		case T_ShareInputScan:
			return ExecCountSlotsShareInputScan((ShareInputScan *) node);

			/*
			 * materialization nodes
			 */
		case T_Material:
			return ExecCountSlotsMaterial((Material *) node);

		case T_Sort:
			return ExecCountSlotsSort((Sort *) node);

		case T_Group:
			return ExecCountSlotsGroup((Group *) node);

		case T_Agg:
			return ExecCountSlotsAgg((Agg *) node);

		case T_Window:
			return ExecCountSlotsWindow((Window *) node);

		case T_Unique:
			return ExecCountSlotsUnique((Unique *) node);

		case T_Hash:
			return ExecCountSlotsHash((Hash *) node);

		case T_SetOp:
			return ExecCountSlotsSetOp((SetOp *) node);

		case T_Limit:
			return ExecCountSlotsLimit((Limit *) node);

		case T_Motion:
			return ExecCountSlotsMotion((Motion *) node);

		default:
			debug_break();
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}

	return 0;
}

/* ----------------------------------------------------------------
 *		ExecSquelchNode
 *
 *		When a node decides that it will not consume any more
 *		input tuples from a subtree that has not yet returned
 *		end-of-data, it must call ExecSquelchNode() on the subtree.
 * ----------------------------------------------------------------
 */

static CdbVisitOpt
squelchNodeWalker(PlanState *node,
				  void *context)
{
	if (IsA(node, MotionState))
	{
		ExecStopMotion((MotionState *) node);
		return CdbVisit_Skip;	/* don't visit subtree */
	}
	return CdbVisit_Walk;
}	/* squelchNodeWalker */


void
ExecSquelchNode(PlanState *node)
{
    planstate_walk_node(node, squelchNodeWalker, NULL);
}	                            /* ExecSquelchNode */

static CdbVisitOpt
transportUpdateNodeWalker(PlanState *node, void *context)
{

	/* For motion nodes, we just transfer the context information established during SetupInterconnect */
	if (IsA(node, MotionState))
	{
		((MotionState *)node)->ps.state->interconnect_context = (ChunkTransportState *)context;
		/* visit subtree */
	}

	return CdbVisit_Walk;
}	/* transportUpdateNodeWalker */

void
ExecUpdateTransportState(PlanState *node, struct ChunkTransportState *state)
{
	planstate_walk_node(node, transportUpdateNodeWalker, state);
}	                            /* ExecUpdateTransportState */


/* ----------------------------------------------------------------
 *		ExecEndNode
 *
 *		Recursively cleans up all the nodes in the plan rooted
 *		at 'node'.
 *
 *		After this operation, the query plan will not be able to
 *		processed any further.	This should be called only after
 *		the query plan has been fully executed.
 * ----------------------------------------------------------------
 */
void
ExecEndNode(PlanState *node)
{
	ListCell   *subp;

	/*
	 * do nothing when we get to the end of a leaf on tree.
	 */
	if (node == NULL)
		return;

	/* Clean up initPlans and subPlans */
	foreach(subp, node->initPlan)
		ExecEndSubPlan((SubPlanState *) lfirst(subp));
	foreach(subp, node->subPlan)
		ExecEndSubPlan((SubPlanState *) lfirst(subp));

	if (node->chgParam != NULL)
	{
		bms_free(node->chgParam);
		node->chgParam = NULL;
	}

    /* Free EXPLAIN ANALYZE buffer */
    if (node->cdbexplainbuf)
    {
        if (node->cdbexplainbuf->data)
            pfree(node->cdbexplainbuf->data);
        pfree(node->cdbexplainbuf);
        node->cdbexplainbuf = NULL;
    }

    switch (nodeTag(node))
	{
			/*
			 * control nodes
			 */
		case T_ResultState:
			ExecEndResult((ResultState *) node);
			break;

		case T_AppendState:
			ExecEndAppend((AppendState *) node);
			break;

		case T_BitmapAndState:
			ExecEndBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			ExecEndBitmapOr((BitmapOrState *) node);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScanState:
			ExecEndSeqScan((SeqScanState *) node);
			break;

		case T_IndexScanState:
			ExecEndIndexScan((IndexScanState *) node);
			break;
			
		case T_ExternalScanState:
			ExecEndExternalScan((ExternalScanState *) node);
			break;			

		case T_BitmapIndexScanState:
			ExecEndBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapHeapScanState:
			ExecEndBitmapHeapScan((BitmapHeapScanState *) node);
			break;

		case T_TidScanState:
			ExecEndTidScan((TidScanState *) node);
			break;

		case T_SubqueryScanState:
			ExecEndSubqueryScan((SubqueryScanState *) node);
			break;

		case T_FunctionScanState:
			ExecEndFunctionScan((FunctionScanState *) node);
			break;

		case T_ValuesScanState:
			ExecEndValuesScan((ValuesScanState *) node);
			break;

			/*
			 * join nodes
			 */
		case T_AdaptiveNestLoopState:
			ExecEndAdaptiveNestLoop((AdaptiveNestLoopState *) node);
			break;
		case T_NestLoopState:
			ExecEndNestLoop((NestLoopState *) node);
			break;

		case T_MergeJoinState:
			ExecEndMergeJoin((MergeJoinState *) node);
			break;

		case T_HashJoinState:
			ExecEndHashJoin((HashJoinState *) node);
			break;

			/*
			 * ShareInput nodes
			 */
		case T_ShareInputScanState:
			ExecEndShareInputScan((ShareInputScanState *) node);
			break;

			/*
			 * materialization nodes
			 */
		case T_MaterialState:
			ExecEndMaterial((MaterialState *) node);
			break;

		case T_SortState:
			ExecEndSort((SortState *) node);
			break;

		case T_GroupState:
			ExecEndGroup((GroupState *) node);
			break;

		case T_AggState:
			ExecEndAgg((AggState *) node);
			break;

		case T_WindowState:
			ExecEndWindow((WindowState *) node);
			break;

		case T_UniqueState:
			ExecEndUnique((UniqueState *) node);
			break;

		case T_HashState:
			ExecEndHash((HashState *) node);
			break;

		case T_SetOpState:
			ExecEndSetOp((SetOpState *) node);
			break;

		case T_LimitState:
			ExecEndLimit((LimitState *) node);
			break;

		case T_MotionState:
			ExecEndMotion((MotionState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}


#ifdef CDB_TRACE_EXECUTOR
/* ----------------------------------------------------------------
 *	ExecCdbTraceNode
 *
 *	Trace entry and exit from ExecProcNode on an executor node.
 * ----------------------------------------------------------------
 */
void
ExecCdbTraceNode(PlanState *node, bool entry, TupleTableSlot *result)
{
	bool		willReScan = FALSE;
	bool		willReturnTuple = FALSE;
	Plan	   *plan = NULL;
	const char *nameTag = NULL;
	const char *extraTag = "";
	char		extraTagBuffer[20];

	/*
	 * Don't trace NULL nodes..
	 */
	if (node == NULL)
		return;

	plan = node->plan;
	Assert(plan != NULL);
	Assert(result == NULL || !entry);
	willReScan = (entry && node->chgParam != NULL);
	willReturnTuple = (!entry && !TupIsNull(result));

	switch (nodeTag(node))
	{
			/*
			 * control nodes
			 */
		case T_ResultState:
			nameTag = "Result";
			break;

		case T_AppendState:
			nameTag = "Append";
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScanState:
			nameTag = "SeqScan";
			break;

		case T_IndexScanState:
			nameTag = "IndexScan";
			break;
			
		case T_BitmapIndexScanState:
			nameTag = "BitmapIndexScan";
			break;
			
		case T_BitmapHeapScanState:
			nameTag = "BitmapHeapScan";
			break;

		case T_TidScanState:
			nameTag = "TidScan";
			break;

		case T_SubqueryScanState:
			nameTag = "SubqueryScan";
			break;

		case T_FunctionScanState:
			nameTag = "FunctionScan";
			break;

		case T_ValuesScanState:
			nameTag = "ValuesScan";
			break;

			/*
			 * join nodes
			 */
		case T_NestLoopState:
			nameTag = "NestLoop";
			break;

		case T_MergeJoinState:
			nameTag = "MergeJoin";
			break;

		case T_HashJoinState:
			nameTag = "HashJoin";
			break;

		case T_AdaptiveNestLoopState:
			nameTag = "AdaptiveNestLoop";
			break;

			/*
			 * share inpt nodess
			 */
		case T_ShareInputScanState:
			nameTag = "ShareInputScan";
			break;

			/*
			 * materialization nodes
			 */
		case T_MaterialState:
			nameTag = "Material";
			break;

		case T_SortState:
			nameTag = "Sort";
			break;

		case T_GroupState:
			nameTag = "Group";
			break;

		case T_AggState:
			nameTag = "Agg";
			break;

		case T_WindowState:
			nameTag = "Window";
			break;

		case T_UniqueState:
			nameTag = "Unique";
			break;

		case T_HashState:
			nameTag = "Hash";
			break;

		case T_SetOpState:
			nameTag = "SetOp";
			break;

		case T_LimitState:
			nameTag = "Limit";
			break;

		case T_MotionState:
			nameTag = "Motion";
			{
				snprintf(extraTagBuffer, sizeof extraTagBuffer, " %d", ((Motion *) plan)->motionID);
				extraTag = &extraTagBuffer[0];
			}
			break;

		default:
			nameTag = "*unknown*";
			break;
	}

	if (entry)
	{
		elog(DEBUG4, "CDB_TRACE_EXECUTOR: Exec %s%s%s", nameTag, extraTag, willReScan ? " (will ReScan)." : ".");
	}
	else
	{
		elog(DEBUG4, "CDB_TRACE_EXECUTOR: Return from %s%s with %s tuple.", nameTag, extraTag, willReturnTuple ? "a" : "no");
		if ( willReturnTuple )
			print_slot(result);
	}

	return;
}
#endif   /* CDB_TRACE_EXECUTOR */


/* -----------------------------------------------------------------------
 *                      PlanState Tree Walking Functions
 * -----------------------------------------------------------------------
 *
 * planstate_walk_node
 *    Calls a 'walker' function for the given PlanState node; or returns
 *    CdbVisit_Walk if 'planstate' is NULL.
 *
 *    If 'walker' returns CdbVisit_Walk, then this function calls
 *    planstate_walk_kids() to visit the node's children, and returns
 *    the result.
 *
 *    If 'walker' returns CdbVisit_Skip, then this function immediately
 *    returns CdbVisit_Walk and does not visit the node's children.
 *
 *    If 'walker' returns CdbVisit_Stop or another value, then this function
 *    immediately returns that value and does not visit the node's children.
 *
 * planstate_walk_array
 *    Calls planstate_walk_node() for each non-NULL PlanState ptr in
 *    the given array of pointers to PlanState objects.
 *
 *    Quits if the result of planstate_walk_node() is CdbVisit_Stop or another
 *    value other than CdbVisit_Walk, and returns that result without visiting
 *    any more nodes.
 *
 *    Returns CdbVisit_Walk if 'planstates' is NULL, or if all of the 
 *    subtrees return CdbVisit_Walk.
 *
 *    Note that this function never returns CdbVisit_Skip to its caller.
 *    Only the caller's 'walker' function can return CdbVisit_Skip.
 *
 * planstate_walk_list
 *    Calls planstate_walk_node() for each PlanState node in the given List.
 *
 *    Quits if the result of planstate_walk_node() is CdbVisit_Stop or another
 *    value other than CdbVisit_Walk, and returns that result without visiting
 *    any more nodes.
 *
 *    Returns CdbVisit_Walk if all of the subtrees return CdbVisit_Walk, or
 *    if the list is empty.
 *
 *    Note that this function never returns CdbVisit_Skip to its caller.
 *    Only the caller's 'walker' function can return CdbVisit_Skip.
 *
 * planstate_walk_kids
 *    Calls planstate_walk_node() for each child of the given PlanState node.
 *
 *    Quits if the result of planstate_walk_node() is CdbVisit_Stop or another
 *    value other than CdbVisit_Walk, and returns that result without visiting
 *    any more nodes.
 *
 *    Returns CdbVisit_Walk if the given planstate node ptr is NULL, or if 
 *    all of the children return CdbVisit_Walk, or if there are no children.
 *
 *    Note that this function never returns CdbVisit_Skip to its caller.
 *    Only the 'walker' can return CdbVisit_Skip.
 *
 * NB: All CdbVisitOpt values other than CdbVisit_Walk or CdbVisit_Skip are
 * treated as equivalent to CdbVisit_Stop.  Thus the walker can break out
 * of a traversal and at the same time return a smidgen of information to the
 * caller, perhaps to indicate the reason for termination.  For convenience,
 * a couple of alternative stopping codes are predefined for walkers to use at
 * their discretion: CdbVisit_Failure and CdbVisit_Success.
 *
 * NB: We do not visit the left subtree of a NestLoopState node (NJ) whose
 * 'shared_outer' flag is set.  This occurs when the NJ is the left child of
 * an AdaptiveNestLoopState (AJ); the AJ's right child is a HashJoinState (HJ);
 * and both the NJ and HJ point to the same left subtree.  This way we avoid
 * visiting the common subtree twice when descending through the AJ node.
 * The caller's walker function can handle the NJ as a special case to
 * override this behavior if there is a need to always visit both subtrees.
 * -----------------------------------------------------------------------
 */
CdbVisitOpt
planstate_walk_node(PlanState      *planstate,
			        CdbVisitOpt   (*walker)(PlanState *planstate, void *context),
			        void           *context)
{
    CdbVisitOpt     whatnext;

    if (planstate == NULL)
        whatnext = CdbVisit_Walk;
    else
    {
        whatnext = walker(planstate, context);
        if (whatnext == CdbVisit_Walk)
            whatnext = planstate_walk_kids(planstate, walker, context);
        else if (whatnext == CdbVisit_Skip)
            whatnext = CdbVisit_Walk;
    }
    Assert(whatnext != CdbVisit_Skip);
    return whatnext;
}	                            /* planstate_walk_node */

CdbVisitOpt
planstate_walk_array(PlanState    **planstates,
                     int            nplanstate,
			         CdbVisitOpt  (*walker)(PlanState *planstate, void *context),
			         void          *context)
{
    CdbVisitOpt     whatnext = CdbVisit_Walk;
    int             i;

    if (planstates == NULL)
        return CdbVisit_Walk;

    for (i = 0; i < nplanstate && whatnext == CdbVisit_Walk; i++)
        whatnext = planstate_walk_node(planstates[i], walker, context);

    return whatnext;
}	                            /* planstate_walk_array */

CdbVisitOpt
planstate_walk_list(List           *planstatelist,
			        CdbVisitOpt   (*walker)(PlanState *planstate, void *context),
			        void           *context)
{
    ListCell       *cell;
    PlanState      *planstate;
    CdbVisitOpt     v = CdbVisit_Walk;

	foreach(cell, planstatelist)
	{
        planstate = (PlanState *)lfirst(cell);
        v = planstate_walk_node(planstate, walker, context);
        if (v != CdbVisit_Walk) /* stop */
            break;
	}
	return v;
}	                            /* planstate_walk_list */

CdbVisitOpt
planstate_walk_kids(PlanState      *planstate,
			        CdbVisitOpt   (*walker)(PlanState *planstate, void *context),
			        void           *context)
{
    CdbVisitOpt v;

    if (planstate == NULL)
        return CdbVisit_Walk;

	switch (nodeTag(planstate))
	{
        case T_NestLoopState:
        {
            NestLoopState  *nls = (NestLoopState *)planstate;

            /* Don't visit left subtree of NJ if it is shared with brother HJ */
            if (nls->shared_outer)
                v = CdbVisit_Walk;
            else
                v = planstate_walk_node(planstate->lefttree, walker, context);

            /* Right subtree */
            if (v == CdbVisit_Walk)
                v = planstate_walk_node(planstate->righttree, walker, context);
            break;
        }

        case T_AppendState:
		{
			AppendState *as = (AppendState *)planstate;

            v = planstate_walk_array(as->appendplans, as->as_nplans, walker, context);
            Assert(!planstate->lefttree && !planstate->righttree);
			break;
		}
        case T_BitmapAndState:
        {
            BitmapAndState *bas = (BitmapAndState *)planstate;

            v = planstate_walk_array(bas->bitmapplans, bas->nplans, walker, context);
            Assert(!planstate->lefttree && !planstate->righttree);
			break;
        }
        case T_BitmapOrState:
        {
            BitmapOrState  *bos = (BitmapOrState *)planstate;

            v = planstate_walk_array(bos->bitmapplans, bos->nplans, walker, context);
            Assert(!planstate->lefttree && !planstate->righttree);
			break;
        }

        case T_SubqueryScanState:
            v = planstate_walk_node(((SubqueryScanState *)planstate)->subplan, walker, context);
            Assert(!planstate->lefttree && !planstate->righttree);
			break;

        default:
            /* Left subtree */
            v = planstate_walk_node(planstate->lefttree, walker, context);

	        /* Right subtree */
            if (v == CdbVisit_Walk)
                v = planstate_walk_node(planstate->righttree, walker, context);
	}
    return v;
}	                            /* planstate_walk_kids */
