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

static uint64 get_grouping(TupleTableSlot *slot,
						   int grping_idx);

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
	uint64 input_grouping;
	bool   input_has_grouping;
	bool   is_final_rollup_agg;
	bool   is_middle_rollup_agg;
	TupleTableSlot *firsttupleslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *copyslot;
	HeapTuple copytuple;
	bool      first_time = true;
	Group *group_node = (Group *)node->ss.ps.plan;

	/*
	 * get state info from node
	 */
	econtext = node->ss.ps.ps_ExprContext;
	numCols = group_node->numCols;
	grpColIdx = group_node->grpColIdx;
	input_grouping = group_node->inputGrouping;
	input_has_grouping = group_node->inputHasGrouping;
	is_final_rollup_agg =
		(input_has_grouping && group_node->numNullCols == 0);
	is_middle_rollup_agg =
		(input_has_grouping && group_node->numNullCols > 0);


	/*
	 * If the previous aggregated result needs to be outputed
	 * several times, we simple return the previous output tuple.
	 */
	if (node->num_repeats > 0)
	{
		/*
		 * XXX We do the projection here again. It may be cheaper to simply
		 * update the GROUP_ID column, but it seems that it is so much easier
		 * to just do the projection again.
		 */
		econtext->ecxt_scantuple = node->prev_aggslot;

		econtext->group_id = node->num_repeats - 1;
		econtext->grouping =
			get_grouping(econtext->ecxt_scantuple,
						 group_node->grpColIdx[group_node->numCols-1]);

		node->num_repeats--;

		return ExecProject(node->ss.ps.ps_ProjInfo, NULL);
	}

	else
	{
		/* Do not need to release the space */
		node->prev_aggslot = NULL;

		if (node->grp_done)
			return NULL;
	}

	/*
	 * The ScanTupleSlot holds the (copied) first tuple of each group.
	 */
	firsttupleslot = node->ss.ss_ScanTupleSlot;

	/*
	 * This loop reads a new input tuple, and handles several cases:
	 * (1) If this is a pass-through tuple, simple return it.
	 * (2) If this is not a rollup Group node, return the first tuple in
	 *     a new group or loop through tuples in this group.
	 * (3) If this is a rollup Group node, return the input tuple.
	 */
	for (;;)
	{
		int outer_grouping;

		/*
		 * If we have read the first tuple in the next group, we want
		 * to pass through this tuple if this is a rollup query. We also
		 * store this tuple, modified when this is a rollup query,
		 * into firsttupleslot.
		 */
		if (node->grp_firstTuple != NULL)
		{
			HeapTuple tuple = node->grp_firstTuple;

			/*
			 * The previous input tuple is stored in grp_firstTuple in the form
			 * when it was received. For a rollup query, we want to modify it
			 * before storing it to firstupleslot. 
			 */
			if (is_middle_rollup_agg)
			{
				tuple = heap_copytuple(node->grp_firstTuple);
				tuple = heap_modify_tuple(tuple,
										  firsttupleslot->tts_tupleDescriptor,
										  node->replValues,
										  node->replIsnull,
										  node->doReplace);
				ExecStoreTuple(node->grp_firstTuple,
							   node->copyslot,
							   InvalidBuffer,
							   true);
			}

			ExecStoreTuple(tuple,
						   firsttupleslot,
						   InvalidBuffer,
						   true);

			node->grp_firstTuple = NULL;

			if (is_middle_rollup_agg &&
				node->out_state == PASS_THRU)
			{
				node->out_state = PROCESSING;

				/* We need to reproject the pass-through tuple */
				if (group_node->lastGroup)
				{
					econtext->ecxt_scantuple = node->copyslot;
					econtext->group_id = get_group_id(econtext->ecxt_scantuple) - 1;
					econtext->grouping =
						get_grouping(econtext->ecxt_scantuple,
									 group_node->grpColIdx[group_node->numCols-1]);
					return ExecProject(node->ss.ps.ps_ProjInfo, NULL);
				}
				else
					return node->copyslot;
			}

			node->out_state = PROCESSING;
		}

		first_time = TupIsNull(firsttupleslot);

		outerslot = ExecProcNode(outerPlanState(node));
		copyslot = outerslot;

		if (TupIsNull(outerslot))
		{
			/* no more input */
			node->grp_done = TRUE;

			/* empty input, so return nothing */
			if (first_time)
				return NULL;

			node->out_state = AGG_READY;
		}

		else
		{
			outer_grouping =
				tuple_grouping(outerslot, numCols, input_grouping, input_has_grouping,
							   group_node->grpColIdx[group_node->numCols-1]);
			/*
			 * For a rollup query, we need to modify the input tuple
			 * to match with the current rollup level. We initialize those
			 * arrays that are needed for this process.
			 */
			if (node->num_attrs == 0 && is_middle_rollup_agg)
			{
				/* Ignore Grouping column */
				int num_grpattrs = numCols - 1;
				int attno;

				node->num_attrs =
					outerslot->tts_tupleDescriptor->natts;
				node->replValues =
					palloc0(node->num_attrs * sizeof(Datum));
				node->replIsnull = palloc0(node->num_attrs * sizeof(bool));
				node->doReplace = palloc0(node->num_attrs * sizeof(bool));

				for (attno=0; attno<group_node->numNullCols; attno++)
				{
					node->replIsnull[num_grpattrs-attno-1] = true;
					node->doReplace[num_grpattrs-attno-1] = true;
				}
			}

			if (outer_grouping < input_grouping)
			{
				/* simple pass-through */
				node->out_state = PASS_THRU;
				return outerslot;
			}

			Assert(outer_grouping == input_grouping);

			/*
			 * For a rollup query, we modify the input tuple to
			 * match the rollup level of this node. The modified tuple
			 * is stored in node->copyslot.
			 */
			if (is_middle_rollup_agg)
			{
				copytuple = ExecCopySlotTuple(outerslot);
				/* modify this tuple */
				copytuple =
					heap_modify_tuple(copytuple,
									  outerslot->tts_tupleDescriptor,
									  node->replValues,
									  node->replIsnull,
									  node->doReplace);

				ExecStoreTuple(copytuple,
							   node->copyslot,
							   InvalidBuffer,
							   true);
				copyslot = node->copyslot;
			}

			if (first_time ||
				!execTuplesMatch(firsttupleslot, copyslot,
								 numCols, grpColIdx,
								 node->eqfunctions,
								 econtext->ecxt_per_tuple_memory))
			{
			
				/*
				 * Copy the first tuple for the new group in its input form, because
				 * we have to pass through this tuple if this is a rollup query.
				 */
				node->grp_firstTuple = ExecCopySlotTuple(outerslot);

				node->out_state = AGG_READY;
			}
		}

		if (node->out_state != AGG_READY)
		{
			node->out_state = PROCESSING;

			/* return a pass-through tuple if this is a rollup query. */
			if (is_middle_rollup_agg)
			{
				/* We need to reproject the pass-through tuple */
				if (group_node->lastGroup)
				{
					econtext->ecxt_scantuple = outerslot;
					econtext->group_id = get_group_id(econtext->ecxt_scantuple) - 1;
					econtext->grouping =
						get_grouping(econtext->ecxt_scantuple,
									 group_node->grpColIdx[group_node->numCols-1]);
					return ExecProject(node->ss.ps.ps_ProjInfo, NULL);
				}
				else
					return outerslot;
			}

			/* Otherwise, read the next tuple */
			continue;
		}

		Assert(node->out_state == AGG_READY);

		/*
		 * If this is the first time call, then the grouped result is not
		 * ready. We simply output the pass-through tuple.
		 */
		if (first_time && is_middle_rollup_agg)
		{
			node->out_state = PROCESSING;

			/* We need to reproject the pass-through tuple */
			if (group_node->lastGroup)
			{
				econtext->ecxt_scantuple = outerslot;
				econtext->group_id = get_group_id(econtext->ecxt_scantuple) - 1;
				econtext->grouping =
					get_grouping(econtext->ecxt_scantuple,
								 group_node->grpColIdx[group_node->numCols-1]);
				return ExecProject(node->ss.ps.ps_ProjInfo, NULL);
			}
			else
				return outerslot;
		}

		/* We just read the first tuple, so continue to read the next one. */
		if (first_time)
			continue;

		/*
		 * We need not call ResetExprContext here because execTuplesMatch will
		 * reset the per-tuple memory context once per input tuple.
		 */
		econtext->ecxt_scantuple = firsttupleslot;

		if (is_final_rollup_agg)
			econtext->grouping =
				get_grouping(econtext->ecxt_scantuple,
							 group_node->grpColIdx[group_node->numCols-1]);
		else
			econtext->grouping = ((Group *) node->ss.ps.plan)->grouping;

		if (is_middle_rollup_agg)
		{
			node->out_state = PASS_THRU;

			/* We will output the aggregate value below. The group_id
			 * for this aggregate value is group_node->rollupGSTimes.
			 */
			econtext->group_id = group_node->rollupGSTimes;
		}
		else
		{
			node->out_state = PROCESSING;

			if (is_final_rollup_agg)
			{
				uint32 gid = get_group_id(econtext->ecxt_scantuple);
				econtext->group_id = gid;
			}
			else
			{
				econtext->group_id = group_node->rollupGSTimes;
			}
		}

		if (group_node->lastGroup)
		{
			node->prev_aggslot = econtext->ecxt_scantuple;
			node->num_repeats = econtext->group_id - 1;
			econtext->group_id--;
		}

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and loop back to scan the rest of the group.
		 */
		if (ExecQual(node->ss.ps.qual, econtext, false))
		{
			/*
			 * Form and return a projection tuple using the first input tuple.
			 */
			return ExecProject(node->ss.ps.ps_ProjInfo, NULL);
		}
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
	grpstate->out_state = PROCESSING;
	grpstate->grp_firstTuple = NULL;
	grpstate->num_attrs = 0;
	grpstate->replValues = NULL;
	grpstate->replIsnull = NULL;
	grpstate->doReplace = NULL;

	grpstate->copyslot = 
		MakeSingleTupleTableSlot(grpstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
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

	if (node->num_attrs > 0)
	{
		pfree(node->replValues);
		pfree(node->replIsnull);
		pfree(node->doReplace);
	}
	ExecClearTuple(node->copyslot);
	/* Release first tuple of group, if we have made a copy */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
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

	node->out_state = PROCESSING;
	ExecClearTuple(node->copyslot);

	if (((PlanState *) node)->lefttree &&
		((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}

static uint64
get_grouping(TupleTableSlot *slot, int grping_attno)
{
	bool isnull;
	uint64 grouping;
	
	/* Obtain grouping from input */
	Datum grping_datum = slot_getattr(slot,
									  grping_attno,
									  &isnull);
	Assert(!isnull);
	grouping = DatumGetInt64(grping_datum);

	return grouping;
}
