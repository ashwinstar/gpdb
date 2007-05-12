/*-------------------------------------------------------------------------
 *
 * nodeSort.c
 *	  Routines to handle sorting of relations.
 *
 * Portions Copyright (c) 2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeSort.c,v 1.58 2006/10/04 00:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeSort.h"
#include "lib/stringinfo.h"             /* StringInfo */
#include "miscadmin.h"
#include "utils/tuplesort.h"
#include "cdb/cdbvars.h" /* CDB *//* gp_sort_flags */

static void ExecSortExplainEnd(PlanState *planstate, struct StringInfoData *buf);

/* ----------------------------------------------------------------
 *		ExecSort
 *
 *		Sorts tuples from the outer subtree of the node using tuplesort,
 *		which saves the results in a temporary file or memory. After the
 *		initial call, returns a tuple from the file with each call.
 *
 *		Conditions:
 *		  -- none.
 *
 *		Initial States:
 *		  -- the outer child is prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecSort(SortState *node)
{
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *tuplesortstate;
	TupleTableSlot *slot;

	/*
	 * get state info from node
	 */
	SO1_printf("ExecSort: %s\n",
			   "entering routine");

	estate = node->ss.ps.state;
	dir = estate->es_direction;
	tuplesortstate = (Tuplesortstate *) node->tuplesortstate;

	/*
	 * If first time through, read all tuples from outer plan and pass them to
	 * tuplesort.c. Subsequent calls just fetch tuples from tuplesort.
	 */

	if (!node->sort_Done)
	{
		Sort	   *plannode = (Sort *) node->ss.ps.plan;
		PlanState  *outerNode;
		TupleDesc	tupDesc;

		SO1_printf("ExecSort: %s\n",
				   "sorting subplan");

		/*
		 * Want to scan subplan in the forward direction while creating the
		 * sorted data.
		 */
		estate->es_direction = ForwardScanDirection;

		/*
		 * Initialize tuplesort module.
		 */
		SO1_printf("ExecSort: %s\n",
				   "calling tuplesort_begin");

		outerNode = outerPlanState(node);
		tupDesc = ExecGetResultType(outerNode);

		tuplesortstate = tuplesort_begin_heap(tupDesc,
											  plannode->numCols,
											  plannode->sortOperators,
											  plannode->sortColIdx,
											  work_mem,
											  node->randomAccess);
		node->tuplesortstate = (void *) tuplesortstate;

		/* CDB */
		{
			ExprContext *econtext = node->ss.ps.ps_ExprContext;
			bool 		isNull;
			int64 		limit = 0;
			int64 		offset = 0;
			int 		unique = 0;
			int 		sort_flags = gp_sort_flags; /* get the guc */
			int         maxdistinct = gp_sort_max_distinct; /* get the guc */

			if (node->limitCount)
			{
				limit =
						DatumGetInt64(
								ExecEvalExprSwitchContext(node->limitCount,
														  econtext,
														  &isNull,
														  NULL));
				/* Interpret NULL limit as no limit */
				if (isNull)
					limit = 0;
				else if (limit < 0)
					limit = 0;

			}
			if (node->limitOffset)
			{
				offset =
						DatumGetInt64(
								ExecEvalExprSwitchContext(node->limitOffset,
														  econtext,
														  &isNull,
														  NULL));
				/* Interpret NULL offset as no offset */
				if (isNull)
					offset = 0;
				else if (offset < 0)
					offset = 0;

			}

			if (node->noduplicates)
				unique = 1;
			
			cdb_tuplesort_init(tuplesortstate, offset, limit, unique, 
							   sort_flags, maxdistinct);
		}

        /* If EXPLAIN ANALYZE, share our Instrumentation object with sort. */
        if (node->ss.ps.instrument)
            tuplesort_set_instrument(tuplesortstate,
                                     node->ss.ps.instrument,
                                     node->ss.ps.cdbexplainbuf);

		/*
		 * Scan the subplan and feed all the tuples to tuplesort.
		 */

		for (;;)
		{
			slot = ExecProcNode(outerNode);

			if (TupIsNull(slot))
				break;

			tuplesort_puttupleslot(tuplesortstate, slot);
		}

		/*
		 * Complete the sort.
		 */
		tuplesort_performsort(tuplesortstate);

		/*
		 * restore to user specified direction
		 */
		estate->es_direction = dir;

		/*
		 * finally set the sorted flag to true
		 */
		node->sort_Done = true;
		SO1_printf("ExecSort: %s\n", "sorting done");
	}

	/* for share input, do not need to return any tuple */
	if(node->ss.ps.plan->shared_as_id >= 0)
		return NULL;

	SO1_printf("ExecSort: %s\n",
			   "retrieving tuple from tuplesort");

	/*
	 * Get the first or next tuple from tuplesort. Returns NULL if no more
	 * tuples.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	(void) tuplesort_gettupleslot(tuplesortstate,
								  ScanDirectionIsForward(dir),
								  slot);
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitSort
 *
 *		Creates the run-time state information for the sort node
 *		produced by the planner and initializes its outer subtree.
 * ----------------------------------------------------------------
 */
SortState *
ExecInitSort(Sort *node, EState *estate, int eflags)
{
	SortState  *sortstate;

	SO1_printf("ExecInitSort: %s\n",
			   "initializing sort node");

	/*
	 * create state structure
	 */
	sortstate = makeNode(SortState);
	sortstate->ss.ps.plan = (Plan *) node;
	sortstate->ss.ps.state = estate;

	/*
	 * We must have random access to the sort output to do backward scan or
	 * mark/restore.  We also prefer to materialize the sort output if we
	 * might be called on to rewind and replay it many times.
	 */
	sortstate->randomAccess = (eflags & (EXEC_FLAG_REWIND |
										 EXEC_FLAG_BACKWARD |
										 EXEC_FLAG_MARK)) != 0;

	/* If the sort is shared, we need random access */
	if(node->plan.shared_as_id >= 0)
		sortstate->randomAccess = true;

	sortstate->sort_Done = false;
	sortstate->tuplesortstate = NULL;

	/* CDB */

	/* BUT:
	 * The LIMIT optimizations requires exprcontext in which to
	 * evaluate the limit/offset parameters.
	 */
	ExecAssignExprContext(estate, &sortstate->ss.ps);

	/* CDB */ /* evaluate a limit as part of the sort */
	{
		/* pass node state to sort state */
		sortstate->limitOffset = ExecInitExpr((Expr *) node->limitOffset,
											  (PlanState *) sortstate);
		sortstate->limitCount = ExecInitExpr((Expr *) node->limitCount,
											 (PlanState *) sortstate);
		sortstate->noduplicates = node->noduplicates;
	}

	/*
	 * Miscellaneous initialization
	 *
	 * Sort nodes don't initialize their ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

#define SORT_NSLOTS 2

	/*
	 * tuple table initialization
	 *
	 * sort nodes only return scan tuples from their sorted relation.
	 */
	ExecInitResultTupleSlot(estate, &sortstate->ss.ps);
	ExecInitScanTupleSlot(estate, &sortstate->ss);

    /* 
     * CDB: Offer extra info for EXPLAIN ANALYZE.
     */
    if (estate->es_instrument)
    {
	    /* Allocate string buffer. */
        sortstate->ss.ps.cdbexplainbuf = makeStringInfo();

        /* Request a callback at end of query. */
        sortstate->ss.ps.cdbexplainfun = ExecSortExplainEnd;
    }

	/*
	 * initialize child nodes
	 *
	 * We shield the child node from the need to support REWIND, BACKWARD, or
	 * MARK/RESTORE.
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlanState(sortstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * initialize tuple type.  no need to initialize projection info because
	 * this node doesn't do projections.
	 */
	ExecAssignResultTypeFromTL(&sortstate->ss.ps);
	ExecAssignScanTypeFromOuterPlan(&sortstate->ss);
	sortstate->ss.ps.ps_ProjInfo = NULL;

	if(node->plan.shared_as_id >= 0) 
	{
		ShareNodeEntry *snEntry = ExecGetShareNodeEntry(estate, node->plan.shared_as_id, true);
		snEntry->sharePlan = (Node *)node;
		snEntry->shareState = (Node *)sortstate;
	}

	SO1_printf("ExecInitSort: %s\n",
			   "sort node initialized");

	return sortstate;
}

int
ExecCountSlotsSort(Sort *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) +
		SORT_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndSort(node)
 * ----------------------------------------------------------------
 */
static void
ExecEndSort_internal(SortState *node)
{
	SO1_printf("ExecEndSort: %s\n",
			   "shutting down sort node");

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * Release tuplesort resources
     *
     * CDB: For EXPLAIN ANALYZE, tuplesort_end() may update statistics in our
     * Instrumentation object via a ptr saved by tuplesort_set_instrument().
	 */
	if (node->tuplesortstate != NULL)
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
	node->tuplesortstate = NULL;
}

void
ExecEndSort(SortState *node)
{
    ExecEndSort_internal(node);

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

	SO1_printf("ExecEndSort: %s\n",
			   "sort node shutdown");
}

/* ----------------------------------------------------------------
 *		ExecSortMarkPos
 *
 *		Calls tuplesort to save the current position in the sorted file.
 * ----------------------------------------------------------------
 */
void
ExecSortMarkPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return
	 */
	if (!node->sort_Done)
		return;

	tuplesort_markpos((Tuplesortstate *) node->tuplesortstate);
}

/* ----------------------------------------------------------------
 *		ExecSortRestrPos
 *
 *		Calls tuplesort to restore the last saved sort file position.
 * ----------------------------------------------------------------
 */
void
ExecSortRestrPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return.
	 */
	if (!node->sort_Done)
		return;

	/*
	 * restore the scan to the previously marked position
	 */
	tuplesort_restorepos((Tuplesortstate *) node->tuplesortstate);
}

void
ExecReScanSort(SortState *node, ExprContext *exprCtxt)
{
	/*
	 * If we haven't sorted yet, just return. If outerplan' chgParam is not
	 * NULL then it will be re-scanned by ExecProcNode, else - no reason to
	 * re-scan it at all.
	 */
	if (!node->sort_Done)
		return;

	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * If subnode is to be rescanned then we forget previous sort results; we
	 * have to re-read the subplan and re-sort.
	 *
	 * Otherwise we can just rewind and rescan the sorted output.
	 */
	if (((PlanState *) node)->lefttree->chgParam != NULL ||
		!node->randomAccess)
	{
		node->sort_Done = false;
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
		node->tuplesortstate = NULL;

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (((PlanState *) node)->lefttree->chgParam == NULL)
			ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
	}
	else
		tuplesort_rescan((Tuplesortstate *) node->tuplesortstate);
}


/*
 * ExecSortExplainEnd
 *      Called before ExecutorEnd to finish EXPLAIN ANALYZE reporting.
 */
void
ExecSortExplainEnd(PlanState *planstate, struct StringInfoData *buf)
{
    /*
     * Some of the cleanup that ordinarily would occur during ExecEndSort()
     * needs to be done earlier (i.e. now) in order to report statistics to 
     * EXPLAIN ANALYZE.  Note that ExecEndSort_internal() may be called twice:
     * first here, and again when ExecEndSort is called during ExecutorEnd().
     */
    ExecEndSort_internal((SortState *)planstate);
}                               /* ExecSortExplainEnd */
