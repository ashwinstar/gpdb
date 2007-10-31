/*-------------------------------------------------------------------------
 *
 * nodeGroup.c
 *	  Routines to handle group nodes (used for queries with GROUP BY clause).
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * DESCRIPTION
 *	  The Group node is designed for handling queries with a GROUP BY clause.
 *	  Its outer plan must deliver tuples that are sorted in the order
 *	  specified by the grouping columns (ie. tuples from the same group are
 *	  consecutive).  That way, we just have to compare adjacent tuples to
 *	  locate group boundaries.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeGroup.c,v 1.65.2.1 2007/02/02 00:07:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeGroup.h"

#if 0
/* debugging.. */
#include "utils/debugutils.h"
#endif

/*
 *	 ExecGroup -
 *
 *		Return one tuple for each group of matching input tuples.
 */
TupleTableSlot *
ExecGroup(GroupState *node)
{
	ExprContext *econtext;
	int			numCols;
	AttrNumber *grpColIdx;
	TupleTableSlot *firsttupleslot;
	TupleTableSlot *outerslot;
	Group *group_node = (Group *)node->ss.ps.plan;

	uint64 input_grouping;
	bool   input_has_grouping;
	bool   is_final_rollup_group;
	bool   is_middle_rollup_group;

	HeapTuple copytuple;
	bool passthru_ready = false;

	/*
	 * get state info from node
	 */
	econtext = node->ss.ps.ps_ExprContext;
	numCols = group_node->numCols - group_node->numNullCols;
	grpColIdx = group_node->grpColIdx;
	input_grouping = group_node->inputGrouping;
	input_has_grouping = group_node->inputHasGrouping;
	is_final_rollup_group =
		(group_node->lastGroup ||
		 (input_has_grouping && group_node->numNullCols == 0));
	is_middle_rollup_group =
		(input_has_grouping && group_node->numNullCols > 0);

	/*
	 * If the previous group tuple needs to be outputed
	 * several times, we simple return the previous output tuple.
	 */
	if (node->num_repeats > 0)
	{
		/*
		 * XXX We do the projection here again. It may be cheaper to simply
		 * update the GROUP_ID column, but it seems that it is so much easier
		 * to just do the projection again.
		 */
		econtext->ecxt_scantuple = node->prev_groupslot;
		econtext->group_id = node->num_repeats;

		if (is_final_rollup_group)
			econtext->grouping =
				get_grouping_groupid(econtext->ecxt_scantuple,
									 group_node->grpColIdx[numCols-2]);
		else
			econtext->grouping = group_node->grouping;

		while (econtext->group_id > 0)
		{
			econtext->group_id --;
			node->num_repeats--;

			if (ExecQual(node->ss.ps.qual, econtext, false))
				return ExecProject(node->ss.ps.ps_ProjInfo, NULL);
		}
	}

	/* Do not need to release the space since this is just a pointer
	 * to a slot, which will be released somewhere else.
	 */
	node->prev_groupslot = NULL;

	if (node->grp_done)
		return NULL;

	/*
	 * The ScanTupleSlot holds the (copied) first tuple of each group.
	 */
	firsttupleslot = node->ss.ss_ScanTupleSlot;

	/*
	 * This loop reads from the input until a pass-thru tuple is found
	 * or a group tuple that satisfies the qual condition is found.
	 */
	while(!node->grp_done)
	{
		/*
		 * This loop reads input tuples until one of the following conditions
		 * meets:
		 * (1) An input tuple is a simple pass-thru tuple.
		 * (2) An input tuple is a pass-thru tuple and the first tuple in
		 *     a new group.
		 */
		for (;;)
		{
			int outer_grouping;

			if (node->grp_firstTuple != NULL)
			{
				ExecStoreTuple(node->grp_firstTuple, firsttupleslot,
							   InvalidBuffer, true);
				node->grp_firstTuple = NULL;

				outer_grouping =
					tuple_grouping(firsttupleslot, numCols, input_grouping, input_has_grouping,
							   group_node->grpColIdx[numCols-2]);
				
				if (outer_grouping < input_grouping)
					node->is_passthru = true;

				if (is_middle_rollup_group || outer_grouping < input_grouping)
				{
					/* grp_firstTuple is a pass-thru tuple.*/
					passthru_ready = true;
					outerslot = firsttupleslot;
					break;
				}
			}

			else if (node->is_passthru)
			{
				/* We want to clean up firsttupeslot since if
				 * it is just a pass-thru tuple.
				 */
				firsttupleslot = ExecClearTuple(firsttupleslot);
				node->is_passthru = false;
			}
			
			
			outerslot = ExecProcNode(outerPlanState(node));
			if (TupIsNull(outerslot))
			{
				node->grp_done = TRUE;
				if (TupIsNull(firsttupleslot))
				{
					/* empty input, so return nothing */
					return NULL;
				}
				/* This is the last tuple. */
				break;
			}

#if 0
			elog(LOG, "nodeGroup: numNullCols=%d, inputHasGrouping=%d, INPUT tuple: %s",
				 group_node->numNullCols, group_node->inputHasGrouping, tup2str(outerslot));
#endif

			outer_grouping =
				tuple_grouping(outerslot, numCols, input_grouping, input_has_grouping,
							   group_node->grpColIdx[numCols-2]);

			Assert (outer_grouping <= input_grouping);
		
			if (TupIsNull(firsttupleslot) && outer_grouping == input_grouping)
			{
				/* This is the first tuple from the input. Copy it to firsttupleslot. */
				firsttupleslot =
					ExecCopySlot(firsttupleslot, outerslot);
			}

			else
			{
				int match_numcols = numCols;
				
				if (outer_grouping < input_grouping)
					/* ignore grouping/groupid column */
					match_numcols -= 2;
					
				/* Check if we've acrossed the group boundary. */
				if (!TupIsNull(firsttupleslot) && 
					!execTuplesMatch(firsttupleslot, outerslot,
									 match_numcols, grpColIdx,
									 node->eqfunctions,
									 econtext->ecxt_per_tuple_memory))
				{
					/*
					 * Make a copy of the first tuple in a new group.
					 */
					node->grp_firstTuple = ExecCopySlotTuple(outerslot);
					break;
				}
			}

			if (is_middle_rollup_group || outer_grouping < input_grouping)
			{
				passthru_ready = true;
				break;
			}
		}
		
		if (passthru_ready)
			econtext->ecxt_scantuple = outerslot;
		else
			econtext->ecxt_scantuple = firsttupleslot;

		/*
		 * Simply return a pass-thru tuple when this is a middle
		 * rollup node.
		 */
		if (passthru_ready && !group_node->lastGroup)
		{
#if 0
			elog(LOG, "nodeGroup: numNullCols=%d, inputHasGrouping=%d, output tuple: %s",
				 group_node->numNullCols, group_node->inputHasGrouping, tup2str(outerslot));
#endif

			return outerslot;
		}

		/*
		 * We obtain GROUP_ID from the input tuples when this is
		 * the middle Group or final Group in a ROLLUP.
		 */
		if (is_final_rollup_group ||
			(passthru_ready && is_middle_rollup_group))
			econtext->group_id =
				get_grouping_groupid(econtext->ecxt_scantuple,
									 group_node->grpColIdx[numCols-1]);
		else
			econtext->group_id = group_node->rollupGSTimes;

		/* Set GROUPING value */
		if (is_final_rollup_group ||
			(passthru_ready && is_middle_rollup_group))
			econtext->grouping =
				get_grouping_groupid(econtext->ecxt_scantuple,
									 group_node->grpColIdx[numCols-2]);
		else
			econtext->grouping = group_node->grouping;
		
		/*
		 * When some grouping columns do not appear in this Agg node,
		 * We modify the input tuple before doing projection.
		 */
		if (!passthru_ready && group_node->numNullCols > 0)
		{
			/*
			 * Initialize the arrays to support modifying tuples when
			 * needed. (Only do it once.)
			 */
			if (node->num_attrs == 0 && group_node->numNullCols > 0)
			{
				int num_grpattrs = group_node->numCols;
				int attno;

				node->num_attrs =
					firsttupleslot->tts_tupleDescriptor->natts;
				node->replValues =
					palloc0(node->num_attrs * sizeof(Datum));
				node->replIsnull = palloc0(node->num_attrs * sizeof(bool));
				node->doReplace = palloc0(node->num_attrs * sizeof(bool));

				for (attno=num_grpattrs - group_node->numNullCols; attno<num_grpattrs; attno++)
				{
					node->replIsnull[group_node->grpColIdx[attno] - 1] = true;
					node->doReplace[group_node->grpColIdx[attno] - 1] = true;
				}
			}

			/* modify the tuple */
			if (group_node->numNullCols > 0)
			{
				HeapTuple newtuple;

				copytuple = ExecCopySlotTuple(econtext->ecxt_scantuple);
				/* modify this tuple */
				newtuple =
					heap_modify_tuple(copytuple,
									  firsttupleslot->tts_tupleDescriptor,
									  node->replValues,
									  node->replIsnull,
									  node->doReplace);
				/* free copytuple */
				heap_freetuple(copytuple);
				copytuple = newtuple;

				ExecStoreTuple(copytuple,
							   econtext->ecxt_scantuple,
							   InvalidBuffer,
							   true);
			}
		}
		
		/*
		 * Since one tuple may be outputted several times with different
		 * GROUP_ID values, and when the GROUP_ID() is appeared in
		 * the HAVING clause, we want to loop here to find the first
		 * tuple that satisfies the qual.
		 */
		do
		{
			if (group_node->lastGroup)
			{
				node->prev_groupslot = econtext->ecxt_scantuple;
				node->num_repeats = econtext->group_id - 1;
				if (econtext->group_id > 0)
					econtext->group_id--;
			}

			/*
			 * Check the qual (HAVING clause); if the group does not match, ignore
			 * it and loop back to try to process another group.
			 */
			if (ExecQual(node->ss.ps.qual, econtext, false))
			{
#if 1
				/*
				 * Form and return a projection tuple using the aggregate results
				 * and the representative input tuple.	Note we do not support
				 * aggregates returning sets ...
				 */
				return ExecProject(node->ss.ps.ps_ProjInfo, NULL);
#else
				TupleTableSlot *out = ExecProject(node->ss.ps.ps_ProjInfo, NULL);
				if (out != NULL)
					elog(LOG, "nodeGroup: numNullCols=%d, inputHasGrouping=%d, output tuple: %s",
						 group_node->numNullCols, group_node->inputHasGrouping, tup2str(out));
		return out;
#endif
			}
		} while (econtext->group_id > 0);
	}
	
	/* NOTREACHED */
	return NULL;
}

/* -----------------
 * ExecInitGroup
 *
 *	Creates the run-time information for the group node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
GroupState *
ExecInitGroup(Group *node, EState *estate, int eflags)
{
	GroupState *grpstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	grpstate = makeNode(GroupState);
	grpstate->ss.ps.plan = (Plan *) node;
	grpstate->ss.ps.state = estate;
	grpstate->grp_done = FALSE;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &grpstate->ss.ps);

#define GROUP_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &grpstate->ss);
	ExecInitResultTupleSlot(estate, &grpstate->ss.ps);

	/*
	 * initialize child expressions
	 */
	grpstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) grpstate);
	grpstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) grpstate);

	/*
	 * initialize child nodes
	 */
	outerPlanState(grpstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * initialize tuple type.
	 */
	ExecAssignScanTypeFromOuterPlan(&grpstate->ss);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&grpstate->ss.ps);
	ExecAssignProjectionInfo(&grpstate->ss.ps, NULL);

	/*
	 * Precompute fmgr lookup data for inner loop
	 */
	grpstate->eqfunctions =
		execTuplesMatchPrepare(ExecGetScanType(&grpstate->ss),
							   node->numCols,
							   node->grpColIdx);

	/* for rollup */
	grpstate->num_attrs = 0;
	grpstate->replValues = NULL;
	grpstate->replIsnull = NULL;
	grpstate->doReplace = NULL;
	grpstate->grp_firstTuple = NULL;
	grpstate->is_passthru = false;

	return grpstate;
}

int
ExecCountSlotsGroup(Group *node)
{
	return ExecCountSlotsNode(outerPlan(node)) + GROUP_NSLOTS;
}

/* ------------------------
 *		ExecEndGroup(node)
 *
 * -----------------------
 */
void
ExecEndGroup(GroupState *node)
{
	PlanState  *outerPlan;

	ExecFreeExprContext(&node->ss.ps);

	/* clean up tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	node->is_passthru = false;
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}
	if (node->num_attrs > 0)
	{
		pfree(node->replValues);
		pfree(node->replIsnull);
		pfree(node->doReplace);
	}

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

void
ExecReScanGroup(GroupState *node, ExprContext *exprCtxt)
{
	node->grp_done = FALSE;
	/* must clear first tuple */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	node->is_passthru = false;
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}

	if (((PlanState *) node)->lefttree &&
		((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}
