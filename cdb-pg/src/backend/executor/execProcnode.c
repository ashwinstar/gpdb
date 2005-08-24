/*-------------------------------------------------------------------------
 *
 * execProcnode.c
 *	 contains dispatch functions which call the appropriate "initialize",
 *	 "get a tuple", and "cleanup" routines for the given node type.
 *	 If the node has children, then it will presumably call ExecInitNode,
 *	 ExecProcNode, or ExecEndNode on its subnodes and do the appropriate
 *	 processing.
 *
 * Portions Copyright (c) 2005, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/execProcnode.c,v 1.46 2004/12/31 21:59:45 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecCountSlotsNode -	count tuple slots needed by plan tree
 *		ExecInitNode	-		initialize a plan node and its subplans
 *		ExecProcNode	-		get a tuple by executing the plan node
 *		ExecEndNode		-		shut down a plan node and its subplans
 *      ExecSquelchNode     -   notify subtree that no more tuples are needed
 *      ExecStateTreeWalker -   call given function for each node of plan state
 *
 *	 NOTES
 *		This used to be three files.  It is now all combined into
 *		one file so that it is easier to keep ExecInitNode, ExecProcNode,
 *		and ExecEndNode in sync when new nodes are added.
 *
 *	 EXAMPLE
 *		suppose we want the age of the manager of the shoe department and
 *		the number of employees in that department.  so we have the query:
 *
 *				retrieve (DEPT.no_emps, EMP.age)
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
 *		ExecStart() is called first.
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
#include "executor/nodeResult.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSetOp.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/nodeSubqueryscan.h"
#include "executor/nodeTidscan.h"
#include "executor/nodeUnique.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "cdb/cdbvars.h"

#ifdef CDB_TRACE_EXECUTOR
static void ExecCdbTraceNode(PlanState *node, bool entry, TupleTableSlot *result);
#endif /* CDB_TRACE_EXECUTOR */

/* ------------------------------------------------------------------------
 *		ExecInitNode
 *
 *		Recursively initializes all the nodes in the plan rooted
 *		at 'node'.
 *
 *		Initial States:
 *		  'node' is the plan produced by the query planner
 *		  'estate' is the shared execution state for the query tree
 *
 *		Returns a PlanState node corresponding to the given Plan node.
 * ------------------------------------------------------------------------
 */
PlanState *
ExecInitNode(Plan *node, EState *estate)
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
			result = (PlanState *) ExecInitResult((Result *) node, estate);
			break;

		case T_Append:
			result = (PlanState *) ExecInitAppend((Append *) node, estate);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			result = (PlanState *) ExecInitSeqScan((SeqScan *) node, estate);
			break;

		case T_IndexScan:
			result = (PlanState *) ExecInitIndexScan((IndexScan *) node, estate);
			break;

		case T_TidScan:
			result = (PlanState *) ExecInitTidScan((TidScan *) node, estate);
			break;

		case T_SubqueryScan:
			result = (PlanState *) ExecInitSubqueryScan((SubqueryScan *) node, estate);
			break;

		case T_FunctionScan:
			result = (PlanState *) ExecInitFunctionScan((FunctionScan *) node, estate);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoop:
			result = (PlanState *) ExecInitNestLoop((NestLoop *) node, estate);
			break;

		case T_MergeJoin:
			result = (PlanState *) ExecInitMergeJoin((MergeJoin *) node, estate);
			break;

		case T_HashJoin:
			result = (PlanState *) ExecInitHashJoin((HashJoin *) node, estate);
			break;

			/*
			 * materialization nodes
			 */
		case T_Material:
			result = (PlanState *) ExecInitMaterial((Material *) node, estate);
			break;

		case T_Sort:
			result = (PlanState *) ExecInitSort((Sort *) node, estate);
			break;

		case T_Group:
			result = (PlanState *) ExecInitGroup((Group *) node, estate);
			break;

		case T_Agg:
			result = (PlanState *) ExecInitAgg((Agg *) node, estate);
			break;

		case T_Unique:
			result = (PlanState *) ExecInitUnique((Unique *) node, estate);
			break;

		case T_Hash:
			result = (PlanState *) ExecInitHash((Hash *) node, estate);
			break;

		case T_SetOp:
			result = (PlanState *) ExecInitSetOp((SetOp *) node, estate);
			break;

		case T_Limit:
			result = (PlanState *) ExecInitLimit((Limit *) node, estate);
			break;

		case T_Motion:
			result = (PlanState *) ExecInitMotion((Motion *) node, estate);
			break;
			
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * Initialize any initPlans present in this node.  The planner put
	 * them in a separate list for us.
	 */
	subps = NIL;
	foreach(l, node->initPlan)
	{
		SubPlan    *subplan = (SubPlan *) lfirst(l);
		SubPlanState *sstate;
		int prior_es_cur_slice_idx;
		int prior_es_cur_root_idx;

		Assert(IsA(subplan, SubPlan));
		sstate = ExecInitExprInitPlan(subplan, result);
		
		/*
		 * CDB Slice Plan:  QD must initialize the global slice table.
		 */
		prior_es_cur_slice_idx = estate->es_cur_slice_idx; /* save */
		prior_es_cur_root_idx = estate->es_cur_root_idx; /* save */
		
		if ( Cdb_role == CDB_ROLE_DISPATCH )
		{
			MemoryContext oldcxt;
			int idx;
			Slice *thisSlice;
			
			oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
			idx = estate->es_sliceTable->nextInitPlan++;
			
			Assert(idx < list_length(estate->es_sliceTable->slices));
			
			estate->es_cur_slice_idx = idx;
			estate->es_cur_root_idx = idx;
			thisSlice =  (Slice*)list_nth(estate->es_sliceTable->slices, idx);
			
			Assert(thisSlice->sliceIndex == -1);
			
			thisSlice->sliceIndex = idx;
			thisSlice->rootIndex = idx;
			
			MemoryContextSwitchTo(oldcxt);
		}
		
		ExecInitSubPlan(sstate, estate);
		
		estate->es_cur_slice_idx = prior_es_cur_slice_idx; /* restore */
		estate->es_cur_root_idx = prior_es_cur_root_idx; /* restore */
		
		subps = lappend(subps, sstate);
	}
	result->initPlan = subps;

	/*
	 * Initialize any subPlans present in this node.  These were found by
	 * ExecInitExpr during initialization of the PlanState.  Note we must
	 * do this after initializing initPlans, in case their arguments
	 * contain subPlans (is that actually possible? perhaps not).
	 */
	foreach(l, result->subPlan)
	{
		SubPlanState *sstate = (SubPlanState *) lfirst(l);

		Assert(IsA(sstate, SubPlanState));
		ExecInitSubPlan(sstate, estate);
	}

	/* Set up instrumentation for this node if requested */
	if (estate->es_instrument)
		result->instrument = InstrAlloc();

	return result;
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
#endif /* CDB_TRACE_EXECUTOR */

	/*
	 * deal with NULL nodes..
	 */
	if (node == NULL)
		return NULL;

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

			/*
			 * scan nodes
			 */
		case T_SeqScanState:
			result = ExecSeqScan((SeqScanState *) node);
			break;

		case T_IndexScanState:
			result = ExecIndexScan((IndexScanState *) node);
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

			/*
			 * join nodes
			 */
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
		InstrStopNode(node->instrument, !TupIsNull(result));

#ifdef CDB_TRACE_EXECUTOR
	ExecCdbTraceNode(node, false, result);
#endif /* CDB_TRACE_EXECUTOR */

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

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			return ExecCountSlotsSeqScan((SeqScan *) node);

		case T_IndexScan:
			return ExecCountSlotsIndexScan((IndexScan *) node);

		case T_TidScan:
			return ExecCountSlotsTidScan((TidScan *) node);

		case T_SubqueryScan:
			return ExecCountSlotsSubqueryScan((SubqueryScan *) node);

		case T_FunctionScan:
			return ExecCountSlotsFunctionScan((FunctionScan *) node);

			/*
			 * join nodes
			 */
		case T_NestLoop:
			return ExecCountSlotsNestLoop((NestLoop *) node);

		case T_MergeJoin:
			return ExecCountSlotsMergeJoin((MergeJoin *) node);

		case T_HashJoin:
			return ExecCountSlotsHashJoin((HashJoin *) node);

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
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}

	return 0;
}


/* ----------------------------------------------------------------
 *		ExecSquelchNode
 *
 *      When a node decides that it will not consume any more
 *      input tuples from a subtree that has not yet returned
 *      end-of-data, it must call ExecSquelchNode() on the subtree.
 * ----------------------------------------------------------------
 */

static CdbVisitOpt
squelchNodeWalker(PlanState    *node,
                  void         *context)
{
    if (IsA(node, MotionState))
    {
        ExecStopMotion((MotionState *)node);
        return CdbVisit_Skip;   /* don't visit subtree */
    }
    return CdbVisit_Walk;
}                               /* squelchNodeWalker */


void
ExecSquelchNode(PlanState *node)
{
    ExecStateTreeWalker(node, squelchNodeWalker, NULL, NULL);
}                               /* ExecSquelchNode */


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

			/*
			 * scan nodes
			 */
		case T_SeqScanState:
			ExecEndSeqScan((SeqScanState *) node);
			break;

		case T_IndexScanState:
			ExecEndIndexScan((IndexScanState *) node);
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

			/*
			 * join nodes
			 */
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
	bool willReScan = FALSE;
	bool willReturnTuple = FALSE;
	Plan *plan = NULL;
	const char *nameTag = NULL;
	const char *extraTag = "";
	char extraTagBuffer[20];
	
	/*
	 * Don't trace NULL nodes..
	 */
	if (node == NULL)
		return;

	plan = node->plan;
	Assert( plan != NULL );
	Assert( result == NULL || !entry );
	willReScan = (entry && node->chgParam != NULL);
	willReturnTuple = ( !entry && !TupIsNull(result) );

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

		case T_TidScanState:
			nameTag = "TidScan";
			break;

		case T_SubqueryScanState:
			nameTag = "SubqueryScan";
			break;

		case T_FunctionScanState:
			nameTag = "FunctionScan";
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
				snprintf(extraTagBuffer, sizeof extraTagBuffer, " %d", ((Motion*)plan)->motionID);
				extraTag = &extraTagBuffer[0];
			}
			break;
			
		default:
			nameTag = "*unknown*";
			break;
	}
	
	if ( entry )
	{
		elog(DEBUG4,"CDB_TRACE_EXECUTOR: Exec %s%s%s", nameTag, extraTag, willReScan ? " (will ReScan)."  : ".");
	}
	else
	{
		elog(DEBUG4,"CDB_TRACE_EXECUTOR: Return from %s%s with %s tuple.", nameTag, extraTag, willReturnTuple? "a" : "no");
	}

	return;
}
#endif /* CDB_TRACE_EXECUTOR */


/* ----------------------------------------------------------------
 *		ExecStateTreeWalker
 *
 *		Visits the nodes of a plan state tree in depth-first
 *      order, passing each node (including the starting node)
 *      to the given callback function(s).  
 *
 *      Returns the result from the last callback.  (If 'node' 
 *      is NULL, CdbVisit_Walk is returned.)
 *
 *      The 'visit_preorder' function is called when the walker
 *      reaches a node.  
 *        - If it returns CdbVisit_Walk, or if 'visit_preorder' is
 *          NULL, then the node's subtrees are visited and the 
 *          visit_postorder function is called.
 *        - If it returns CdbVisit_Skip, the walk continues without
 *          visiting the node's subtrees and without calling the
 *          visit_postorder function.
 *        - If it returns CdbVisit_Stop or another value, the 
 *          walk ends immediately with no further callbacks.
 *
 *      The 'visit_postorder' function is called after visiting a
 *      node's subtrees (if any).  If it returns CdbVisit_Walk
 *      or CdbVisit_Skip, or if 'visit_postorder' is NULL, the 
 *      walk continues.  If it returns CdbVisit_Stop or another 
 *      value, the walk ends immediately with no further callbacks.
 * ----------------------------------------------------------------
 */

typedef struct ExecStateTreeWalkerContext
{
    CdbVisitOpt   (*visit_preorder)(PlanState *node, void *context);
    CdbVisitOpt   (*visit_postorder)(PlanState *node, void *context);
    void           *context;
    CdbVisitOpt     result;
} ExecStateTreeWalkerContext;


static bool                     /* returns true to continue, false to stop */
stateTreeWalker(PlanState                  *node,
                ExecStateTreeWalkerContext *ctx)
{
    PlanState  *next;
    PlanState  *subtree;
    int         i;

    for (; node; node = next)
    {
        next = NULL;

        /* Visit this node ... preorder */
        if (ctx->visit_preorder)
        {
            ctx->result = ctx->visit_preorder(node, ctx->context);
            if (ctx->result != CdbVisit_Walk)
                return (ctx->result == CdbVisit_Skip);
        }

        /* Some node types have their own subtree ptr fields */
        switch (nodeTag(node))
        {
            case T_AppendState:
            {
                AppendState    *as = (AppendState *)node;
                for (i = 0; i < as->as_nplans; i++)
                {
                    subtree = as->appendplans[i];
                    if (subtree)
                    {
                        if (next && !stateTreeWalker(next, ctx))
                            return false;
                        next = subtree;
                    }
                }
                break;
            }
            case T_SubqueryScanState:
                next = ((SubqueryScanState *)node)->subplan;
                break;
            default:
                break;
        }

        /* Left subtree */
        subtree = node->lefttree;
        if (subtree)
        {
            if (next && !stateTreeWalker(next, ctx))
                return false;
            next = subtree;
        }

        /* Right subtree */
        subtree = node->righttree;
        if (subtree)
        {
            if (next && !stateTreeWalker(next, ctx))
                return false;
            next = subtree;
        }

        /* Visit this node ... postorder */
        if (ctx->visit_postorder)
        {
            if (next && !stateTreeWalker(next, ctx))
                return false;
            ctx->result = ctx->visit_postorder(node, ctx->context);
            if (ctx->result != CdbVisit_Walk)
                return (ctx->result == CdbVisit_Skip);
            break;
        }
    }
    return true;
}                               /* stateTreeWalker */


CdbVisitOpt
ExecStateTreeWalker(PlanState      *node,
                    CdbVisitOpt   (*visit_preorder)(PlanState *node, void *context),
                    CdbVisitOpt   (*visit_postorder)(PlanState *node, void *context),
                    void           *context)
{
	ExecStateTreeWalkerContext ctx;

    ctx.result = CdbVisit_Walk;
    ctx.visit_preorder = visit_preorder;
    ctx.visit_postorder = visit_postorder;
    ctx.context = context;

    stateTreeWalker(node, &ctx);
    
    return ctx.result;
}                               /* ExecStateTreeWalker */


