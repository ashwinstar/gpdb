/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.c
 *	  Support routines for scanning subqueries (subselects in rangetable).
 *
 * This is just enough different from sublinks (nodeSubplan.c) to mean that
 * we need two sets of code.  Ought to look at trying to unify the cases.
 *
 *
 * Portions Copyright (c) 2006, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeSubqueryscan.c,v 1.27 2005/10/15 02:49:17 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSubqueryScan			scans a subquery.
 *		ExecSubqueryNext			retrieve next tuple in sequential order.
 *		ExecInitSubqueryScan		creates and initializes a subqueryscan node.
 *		ExecEndSubqueryScan			releases any storage allocated.
 *		ExecSubqueryReScan			rescans the relation
 *
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/execdefs.h"
#include "executor/execdesc.h"
#include "executor/nodeSubqueryscan.h"
#include "optimizer/var.h"              /* CDB: contain_var_reference() */
#include "parser/parsetree.h"
#include "tcop/pquery.h"

static TupleTableSlot *SubqueryNext(SubqueryScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		SubqueryNext
 *
 *		This is a workhorse for ExecSubqueryScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SubqueryNext(SubqueryScanState *node)
{
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;
	MemoryContext oldcontext;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;

	/*
	 * We need not support EvalPlanQual here, since we are not scanning a real
	 * relation.
	 */

	/*
	 * Get the next tuple from the sub-query.  We have to be careful to run it
	 * in its appropriate memory context.
	 */
	node->sss_SubEState->es_direction = direction;

	oldcontext = MemoryContextSwitchTo(node->sss_SubEState->es_query_cxt);

	slot = ExecProcNode(node->subplan);

	MemoryContextSwitchTo(oldcontext);

    /*
     * CDB: Label each row with a synthetic ctid if needed for subquery dedup.
     */
    if (node->cdb_want_ctid &&
        !TupIsNull(slot))
    {
        HeapTuple   tuple = ExecMaterializeSlot(slot);

        /* Increment 48-bit row count */
        node->cdb_fake_ctid.ip_posid++;
        if (node->cdb_fake_ctid.ip_posid == 0)
            ItemPointerSetBlockNumber(&node->cdb_fake_ctid,
                                      1 + ItemPointerGetBlockNumber(&node->cdb_fake_ctid));

        tuple->t_self = node->cdb_fake_ctid;
        tuple->t_tableOid = node->cdb_fake_tableoid;
    }

	/*
	 * We just overwrite our ScanTupleSlot with the subplan's result slot,
	 * rather than expending the cycles for ExecCopySlot().
	 */
	node->ss.ss_ScanTupleSlot = slot;

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecSubqueryScan(node)
 *
 *		Scans the subquery sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieve tuples sequentially.
 *
 */

TupleTableSlot *
ExecSubqueryScan(SubqueryScanState *node)
{
	/*
	 * use SubqueryNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) SubqueryNext);
}

/* ----------------------------------------------------------------
 *		ExecInitSubqueryScan
 * ----------------------------------------------------------------
 */
SubqueryScanState *
ExecInitSubqueryScan(SubqueryScan *node, EState *estate, int eflags)
{
	SubqueryScanState *subquerystate;
	RangeTblEntry *rte;
	EState	   *sp_estate;
    List       *tlist;
	MemoryContext oldcontext;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * SubqueryScan should not have any "normal" children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	subquerystate = makeNode(SubqueryScanState);
	subquerystate->ss.ps.plan = (Plan *) node;
	subquerystate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &subquerystate->ss.ps);

	/*
	 * initialize child expressions
	 */
	subquerystate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) subquerystate);
	subquerystate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) subquerystate);

    /* CDB: Does targetlist contain a Var node referencing the ctid column? */
    tlist = node->scan.plan.targetlist;
    subquerystate->cdb_want_ctid = contain_var_reference((Node *)tlist,
                                                         node->scan.scanrelid,
                                                         SelfItemPointerAttributeNumber,
                                                         0);
    ItemPointerSet(&subquerystate->cdb_fake_ctid, 0, 0);
    subquerystate->cdb_fake_tableoid = (Oid)node->scan.scanrelid;

#define SUBQUERYSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &subquerystate->ss.ps);
	ExecInitScanTupleSlot(estate, &subquerystate->ss);

	/*
	 * initialize subquery
	 *
	 * This should agree with ExecInitSubPlan
	 */
	rte = rt_fetch(node->scan.scanrelid, estate->es_range_table);
	Assert(rte->rtekind == RTE_SUBQUERY);

	/*
	 * Do access checking on the rangetable entries in the subquery.
	 */
	ExecCheckRTPerms(rte->subquery->rtable);

	/*
	 * The subquery needs its own EState because it has its own rangetable. It
	 * shares our Param ID space, however.	XXX if rangetable access were done
	 * differently, the subquery could share our EState, which would eliminate
	 * some thrashing about in this module...
	 */
	sp_estate = CreateExecutorState();
	subquerystate->sss_SubEState = sp_estate;

	oldcontext = MemoryContextSwitchTo(sp_estate->es_query_cxt);

	sp_estate->es_range_table = rte->subquery->rtable;
	sp_estate->es_param_list_info = estate->es_param_list_info;
	sp_estate->es_param_exec_vals = estate->es_param_exec_vals;
	sp_estate->es_tupleTable =
		ExecCreateTupleTable(ExecCountSlotsNode(node->subplan) + 10);
	sp_estate->es_snapshot = estate->es_snapshot;
	sp_estate->es_crosscheck_snapshot = estate->es_crosscheck_snapshot;
	sp_estate->es_instrument = estate->es_instrument;

	/*
	 * "Loan" the global slice table and map to the subplan EState.  The
	 * global state is already set up by the code that called us.
	 */
	sp_estate->es_cur_root_idx = estate->es_cur_root_idx;
	sp_estate->es_cur_slice_idx = estate->es_cur_slice_idx;
	sp_estate->es_sliceTable = estate->es_sliceTable;
	sp_estate->es_slice_map = estate->es_slice_map;
	
	/*
	 * also loan the motion later state.
	 */
	sp_estate->motionlayer_context = estate->motionlayer_context;

	/*
	 * Start up the subplan (this is a very cut-down form of InitPlan())
	 */
	subquerystate->subplan = ExecInitNode(node->subplan, sp_estate, eflags);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
	 */
	ExecAssignScanType(&subquerystate->ss,
					   ExecGetResultType(subquerystate->subplan),
					   false);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&subquerystate->ss.ps);
	ExecAssignScanProjectionInfo(&subquerystate->ss);

	return subquerystate;
}

int
ExecCountSlotsSubqueryScan(SubqueryScan *node)
{
	/*
	 * The subplan has its own tuple table and must not be counted here!
	 */
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		SUBQUERYSCAN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndSubqueryScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSubqueryScan(SubqueryScanState *node)
{
	MemoryContext oldcontext;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clean out the upper tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	node->ss.ss_ScanTupleSlot = NULL;	/* not ours to clear */

	/*
	 * close down subquery
	 */
	oldcontext = MemoryContextSwitchTo(node->sss_SubEState->es_query_cxt);

	ExecEndPlan(node->subplan, node->sss_SubEState);

	MemoryContextSwitchTo(oldcontext);

	FreeExecutorState(node->sss_SubEState);
}

/* ----------------------------------------------------------------
 *		ExecSubqueryReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecSubqueryReScan(SubqueryScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	MemoryContext oldcontext;

	estate = node->ss.ps.state;

	oldcontext = MemoryContextSwitchTo(node->sss_SubEState->es_query_cxt);

	/*
	 * ExecReScan doesn't know about my subplan, so I have to do
	 * changed-parameter signaling myself.	This is just as well, because the
	 * subplan has its own memory context in which its chgParam state lives.
	 */
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(node->subplan, node->ss.ps.chgParam);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->subplan->chgParam == NULL)
		ExecReScan(node->subplan, NULL);

	MemoryContextSwitchTo(oldcontext);

	node->ss.ss_ScanTupleSlot = NULL;
}
