/*-------------------------------------------------------------------------
 *
 * nodeNestloop.c
 *	  routines to support nest-loop joins
 *
 * Portions Copyright (c) 2006-2007, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeNestloop.c,v 1.39.2.1 2005/11/22 18:23:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecNestLoop	 - process a nestloop join of two plans
 *		ExecInitNestLoop - initialize the join
 *		ExecEndNestLoop  - shut down the join
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeAdaptiveNestloop.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"

#include "utils/memutils.h"

TupleTableSlot *
ExecAdaptiveNestLoop(AdaptiveNestLoopState *node)
{
	if (node->nl_strategy == NL_STRATEGY_UNDECIDED)
	{
		int i = 0;
		TupleTableSlot *slot;

		if (node->outer_q != NULL)
		{
			for (i=0; i < node->nl_strategy_threshold; i++)
			{
				slot = ExecProcNode(outerPlanState(node->main));
				if (TupIsNull(slot))
					break;
				enqueueTQ(node->outer_q, slot);
			}

			elog(DEBUG2, "read %d tuple from outer relation threshold %d", i, node->nl_strategy_threshold);
		}

		/* did we get more tuples than are allowed for loop ? Or
		 * are we working on a non-hashable join ? Or do we have insufficient room
		 * for our queue */
		if (i < node->nl_strategy_threshold ||
			node->alternate == NULL ||
			node->outer_q == NULL)
		{
			node->nl_strategy = NL_STRATEGY_LOOP;

			node->main->outer_q = node->outer_q;

			/* shut down alternate's inner. */
			ExecSquelchNode(innerPlanState(node->alternate));
		}
		else
		{
			node->nl_strategy = NL_STRATEGY_HASH;

			node->alternate->outer_q = node->outer_q;

			/* shut down nested join's inner. */
			ExecSquelchNode(innerPlanState(node->main));
		}

		elog(DEBUG2, "nested join picked strategy: %s read %d thresh %d",
			 (node->nl_strategy == NL_STRATEGY_LOOP ? "nested loop" : "hash join"),
			 i, node->nl_strategy_threshold);
	}

	if (node->nl_strategy == NL_STRATEGY_LOOP)
		return ExecProcNode((PlanState *)node->main);
	else if (node->nl_strategy == NL_STRATEGY_HASH)
		return ExecProcNode((PlanState *)node->alternate);
	else
	{
		elog(ERROR, "ExecAdaptiveNestLoop(): unknown join strategy");
		return NULL;
	}
}

/* ----------------------------------------------------------------
 *		ExecInitNestLoop
 * ----------------------------------------------------------------
 */
AdaptiveNestLoopState *
ExecInitAdaptiveNestLoop(AdaptiveNestLoop *node, EState *estate, int eflags)
{
	AdaptiveNestLoopState *anlstate;

	/*
	 * create state structure
	 */
	anlstate = makeNode(AdaptiveNestLoopState);
	anlstate->ps.plan = (Plan *)node;
	anlstate->ps.state = estate;

	/* initialize adaptive state */
	anlstate->nl_strategy = NL_STRATEGY_UNDECIDED;
	anlstate->nl_strategy_threshold = node->nl_strategy_threshold;

	anlstate->outer_q = makeTQ(anlstate->nl_strategy_threshold, anlstate->ps.instrument);

	/* if our threshold is too large to queue up tuples within work_mem we're in trouble. */
	if (anlstate->outer_q == NULL)
	{
		elog(DEBUG1, "AdaptiveNL: insufficient memory for %d tuples", node->nl_strategy_threshold);
	}

	/* Initialize our child plans */

	/* Plan-A: note tell ExecInitNestLoop() that the outer is shared (let plan-B initialize it) */
	((NestLoop *)node->plan.lefttree)->shared_outer = true;
	anlstate->main = (NestLoopState *)ExecInitNode(node->plan.lefttree, estate, eflags);
	anlstate->ps.lefttree = (PlanState *)anlstate->main;
	
	/* Plan-B */
	node->plan.righttree->lefttree = node->plan.lefttree->lefttree;
	anlstate->alternate = (HashJoinState *)ExecInitNode(node->plan.righttree, estate, eflags);
	anlstate->ps.righttree = (PlanState *)anlstate->alternate;

	/* Now we need a link the outer from Plan-B to Plan-A */
	outerPlanState(anlstate->main) = outerPlanState(anlstate->alternate);
	/* children are ready to go. */

	/*
	 * tuple table initialization (not used, is it really necessary?)
	 */
#define ADAPTIVENESTLOOP_NSLOTS 1
	ExecInitResultTupleSlot(estate, &anlstate->ps);
	ExecAssignResultTypeFromTL(&anlstate->ps);

	/* no projection is done */
	anlstate->ps.ps_ProjInfo = NULL;

    return anlstate;
}

int
ExecCountSlotsAdaptiveNestLoop(AdaptiveNestLoop *node)
{
	int count=ADAPTIVENESTLOOP_NSLOTS;

	count += ExecCountSlotsNode(node->plan.lefttree);
	count += ExecCountSlotsNode(node->plan.righttree);

	return count;
}

/* ----------------------------------------------------------------
 *		ExecEndNestLoop
 *
 *		closes down scans and frees allocated storage
 * ----------------------------------------------------------------
 */
void
ExecEndAdaptiveNestLoop(AdaptiveNestLoopState *node)
{
	ExecEndNode((PlanState *)node->main);
    node->main->shared_outer = false;

	ExecEndNode((PlanState *)node->alternate);
}

/* ----------------------------------------------------------------
 *		ExecReScanNestLoop
 * ----------------------------------------------------------------
 */
void
ExecReScanAdaptiveNestLoop(AdaptiveNestLoopState *node, ExprContext *exprCtxt)
{
	if (node->outer_q != NULL)
	{
		releaseTQ(node->outer_q);
		pfree(node->outer_q);
		node->outer_q = NULL;
	}

	if (node->nl_strategy == NL_STRATEGY_LOOP)
	{
		node->main->outer_q = NULL;
		ExecReScanNestLoop(node->main, exprCtxt);
	}
	else /* forward rescan request to hash alternate */
	{
		node->alternate->outer_q = NULL;
		ExecReScanHashJoin(node->alternate, exprCtxt);
	}
}

