/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.c
 *	  Support routines for scanning subqueries (subselects in rangetable).
 *
 * This is just enough different from sublinks (nodeSubplan.c) to mean that
 * we need two sets of code.  Ought to look at trying to unify the cases.
 *
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeSubqueryscan.c,v 1.39 2008/01/01 19:45:49 momjian Exp $
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

#include "executor/execdebug.h"
#include "executor/nodeSubqueryscan.h"
#include "optimizer/var.h"              /* CDB: contain_var_reference() */

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
	TupleTableSlot *slot;

	/*
	 * We need not support EvalPlanQual here, since we are not scanning a real
	 * relation.
	 */

	/*
	 * Get the next tuple from the sub-query.
	 */
	slot = ExecProcNode(node->subplan);

    /*
     * CDB: Label each row with a synthetic ctid if needed for subquery dedup.
     */
    if (node->cdb_want_ctid &&
        !TupIsNull(slot))
    {
        HeapTuple   tuple = ExecFetchSlotHeapTuple(slot); 

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
	List       *tlist;
#if 0
	// MERGEBUG
	EState     *sp_estate; /* PG8.3 merge. See below. */
#endif

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * SubqueryScan should not have any "normal" children.	Also, if planner
	 * left anything in subrtable, it's fishy.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);
	Assert(node->subrtable == NIL);

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
	 */
#if 0
	// MERGEBUG
	/* XXX PG8.3 Merge.
	 * PG8.3 does not create a sp_estate and use estate directly, for whatever
	 * reason, I don't know this will work for CDB.
	 */
	/*
	 * "Loan" the global slice table and map to the subplan EState.  The
	 * global state is already set up by the code that called us.
	 */
	sp_estate->es_cur_root_idx = estate->es_cur_root_idx;
	sp_estate->es_cur_slice_idx = estate->es_cur_slice_idx;
	sp_estate->es_sliceTable = estate->es_sliceTable;
	sp_estate->es_slice_map = estate->es_slice_map;
	
	/* 
	 * also load shared nodes list 
	 */
	sp_estate->es_sharenode = estate->es_sharenode;

	/*
	 * also loan the motion later state and interconnect state
	 */
	sp_estate->motionlayer_context = estate->motionlayer_context;
	sp_estate->interconnect_context = estate->interconnect_context;

	/* return borrowed share node list */
	estate->es_sharenode = sp_estate->es_sharenode;
#endif
	subquerystate->subplan = ExecInitNode(node->subplan, estate, eflags);

	/*
	 * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
	 */
	ExecAssignScanType(&subquerystate->ss,
					   ExecGetResultType(subquerystate->subplan));

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
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);
	return ExecCountSlotsNode(node->subplan) +
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
	ExecEndNode(node->subplan);
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

	estate = node->ss.ps.state;

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

	node->ss.ss_ScanTupleSlot = NULL;
	/*node->ss.ps.ps_TupFromTlist = false;*/
}
