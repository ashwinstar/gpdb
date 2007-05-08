/*-------------------------------------------------------------------------
 *
 * cdbgroup.c
 *	  Routines to aid in planning grouping queries for parallel
 *    execution.  This is, essentially, an extension of the file
 *    optimizer/prep/planner.c, although some functions are not
 *    externalized.
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/print.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "catalog/pg_aggregate.h"

#include "cdb/cdbllize.h"
#include "cdb/cdbpathtoplan.h"  /* cdbpathtoplan_create_flow() */
#include "cdb/cdbpath.h"
#include "cdb/cdbvars.h"

#include "cdb/cdbsetop.h"
#include "cdb/cdbgroup.h"


/*
 * MppGroupPrep represents a strategy by which to precondition the
 * argument to  a parallel aggregation plan.
 *
 * MPP_GRP_PREP_NONE
 *    Use the input plan as is.
 * MPP_GRP_PREP_HASH_GROUPS
 *    Redistribute the input to collocate groups.
 * MPP_GRP_PREP_HASH_DISTINCT
 *    Redistribute the input on the sole distinct expression used as
 *    an aggregate argument.
 * MPP_GRP_PREP_CULL_GROUPS
 *    Use hash on grouping attributes to convert replicated input into
 *    input partitioned to collocate groups.
 * MPP_GRP_PREP_CULL_DISTINCT
 *    Use hash on the sole distinct expression used as an aggregate
 *    argument to convert replicated input into input partitioned to
 *    collocate groups.
 * MPP_GRP_PREP_CULL_QE
 *    Focus a replicated input on the utility QE.
 * MPP_GRP_PREP_FOCUS_QE
 *    Focus a partitioned input on the utility QE.
 * MPP_GRP_PREP_FOCUS_QD
 *    Focus a partitioned input on the QD.
 * MPP_GRP_PREP_BROADCAST
 *    Broadcast to convert a partitioned input into a replicated one.
 */
typedef enum MppGroupPrep
{
	MPP_GRP_PREP_NONE = 0,
	MPP_GRP_PREP_HASH_GROUPS,
	MPP_GRP_PREP_HASH_DISTINCT,
	MPP_GRP_PREP_CULL_GROUPS,
	MPP_GRP_PREP_CULL_DISTINCT,
	MPP_GRP_PREP_CULL_QE,
	MPP_GRP_PREP_FOCUS_QE,
	MPP_GRP_PREP_FOCUS_QD,
	MPP_GRP_PREP_BROADCAST
} MppGroupPrep;

/*
 * MppGroupType represents a stategy by which to implement parallel
 * aggregation on a possibly preconditioned input plan.
 *
 * MPP_GRP_TYPE_NONE
 *    No parallel plan found.
 * MPP_GRP_TYPE_BASEPLAN
 *    Use the sequential plan as is.
 * MPP_GRP_TYPE_PLAIN_2STAGE,
 *    Ungrouped, 2-stage aggregation.
 * MPP_GRP_TYPE_GROUPED_2STAGE,
 *    Grouped, 2-stage aggregation.
 *
 * TODO: Add types for min-max optimization, when ready:
 *
 * MPP_GRP_TYPE_MINMAX
 *    Use the sequential min-max optimization plan as is.
 * MPP_GRP_TYPE_MINMAX_2STAGE
 *    Use a 2-stage variant of min-max aggregation.
 */
typedef enum MppGroupType
{
	MPP_GRP_TYPE_NONE = 0,
	MPP_GRP_TYPE_BASEPLAN,
	MPP_GRP_TYPE_PLAIN_2STAGE,
	MPP_GRP_TYPE_GROUPED_2STAGE,
} MppGroupType;

typedef struct MppGroupContext
{
	MppGroupPrep prep;
	MppGroupType type;
	
	List *tlist;
	Path *best_path;
	Path *cheapest_path;
	AggClauseCounts *agg_counts;
	double tuple_fraction;
	double *p_dNumGroups;
	CanonicalGroupingSets *canonical_grpsets;
	
	List *sub_tlist;
	int numGroupCols;
	AttrNumber *groupColIdx;
	bool use_hashed_grouping;
	CdbPathLocus input_locus;
	CdbPathLocus output_locus;
	List *current_pathkeys;
	
} MppGroupContext;



/* Context for functions called by generate_two_stage_tlists. */
typedef struct two_stage_agg_context
{
	/* Input */
	List *tlist; /* The targetlist of the original query */
	List *sub_tlist; /* The derived input plan targetlist */
	Node *havingQual;
	int numGroupCols;
	AttrNumber *groupColIdx;
	
	/* Work */
	Index final_varno;
	List *refs_tlist; /* Aggref attributes for  prelim_tlist */
	List *grps_tlist; /* Grouping attributes for prelim_tlist */
} two_stage_agg_context;


static Plan * make_one_stage_agg_plan(PlannerInfo *root, MppGroupContext *ctx);
static Plan * make_two_stage_agg_plan(PlannerInfo *root, MppGroupContext *ctx);
static List *make_subplan_tlist(List *tlist, Node *havingQual,
						List *grp_clauses, int *pnum, AttrNumber **pcols);
static Expr *deconstruct_expr(Expr *expr, two_stage_agg_context *ctx);
static Node* deconstruct_expr_mutator(Node *node, two_stage_agg_context *ctx);
static Aggref *split_aggref(Aggref *aggref, two_stage_agg_context *ctx);
static Oid lookup_agg_transtype(Aggref *aggref);

/*
 * Function: cdb_grouping_planner
 *
 * This is basically an extension of the function grouping_planner() from
 * planner.c.  It (conditionally) replaces the logic in the "normal case"
 * (the aggregation/grouping branch) of the main "if" statement.
 *
 * The result is a Plan for one- or two-stage grouping/aggregation or NULL.
 *
 * A NULL result means that the ordinary sequential planner will produce
 * a correct (and preferred) result, so the "normal case" code should  be
 * used.
 *
 * A non-NULL result is taken as a partially formed Plan to be processed
 * by the trailing sort/distinct/limit logic of grouping_planner().  In
 * otherwords, after any associated changes to the local environment (see
 * the calling code), the resulting plan should be treated as if from the
 *  "normal case" or the function optimize_minmax_aggregates().
 *
 * The pointer at location pcurrent_pathkeys is updated to refer to the
 * ordering pathkey or NIL, if there is none.
 */

Plan *
cdb_grouping_planner(PlannerInfo* root,
		AggClauseCounts *agg_counts,
		List *tlist,
		Path *best_path,
		Path *cheapest_path,
		bool use_hashed_grouping,
		double tuple_fraction,
		double *p_dNumGroups,
		CanonicalGroupingSets *canonical_grpsets,
		List **pcurrent_pathkeys)
{
	MppGroupContext ctx;
	Plan * result_plan = NULL;
	List * sub_tlist = NIL;
	bool has_groups = root->parse->groupClause != NIL;
	bool has_aggs = agg_counts->numAggs > 0;
	CdbPathLocus input_locus = cheapest_path->locus;
	ListCell *lc;

	MppGroupPrep group_prep;
	MppGroupType group_type;
	CdbPathLocus output_locus;

	Assert( !has_groups || root->group_pathkeys != NULL );

	CdbPathLocus_MakeNull(&output_locus);

	/*
	 * First choose a one-stage plan.  Since there's always a way to do this,
	 * it serves as our default choice.
	 */
	group_type = MPP_GRP_TYPE_BASEPLAN;
	group_prep = MPP_GRP_PREP_NONE;

	if ( !CdbPathLocus_IsPartitioned(input_locus) )
	{
		/* Can use base plan with no motion yielding same locus. */
		group_prep = MPP_GRP_PREP_NONE;
		output_locus = input_locus;
	}
	else if ( has_groups ) /* and not single or replicated */
	{
		if (root->group_pathkeys != NULL &&
				cdbpathlocus_collocates(input_locus, root->group_pathkeys) )
		{
			group_prep = MPP_GRP_PREP_NONE;
			output_locus = input_locus; /* may be less discriminating that group locus */
		}
		else
		{
			group_prep = MPP_GRP_PREP_HASH_GROUPS;
			CdbPathLocus_MakeHashed(&output_locus, root->group_pathkeys);
		}

		/* Note that we could explore other approaches to using the base
		 * plan on a grouped query.  For example, cull a replicated input
		 * locus to obtain a group-wise hashed one, but it isn't clear that
		 * this is worthwhile.
		 */
	}
	else if ( has_aggs ) /* and not grouped and not single or replicated  */
	{
		group_prep = MPP_GRP_PREP_FOCUS_QE;
		CdbPathLocus_MakeSingleQE(&output_locus);
	}

	/*
	 * If the sequential planner can handle the situation with no Motion
	 * involved, let it do so.  Don't bother to investigate the 2-stage
	 * approach.
	 */
	if ( group_prep == MPP_GRP_PREP_NONE )
	{
		*pcurrent_pathkeys = NIL;
		return NULL;
	}

	/*
	 * At this point, we're committed to producing a one- or two-stage
	 * plan with motion.  Calculate the subplan target list to use in
	 * the first stage.  Also note the location of any grouping attributes
	 * in the target list.
	 */
	sub_tlist = make_subplan_tlist(tlist,
			root->parse->havingQual,
			root->parse->groupClause,
			&ctx.numGroupCols,
			&ctx.groupColIdx);

	/* Where we need to and we can, add column names to the sub_tlist 
	 * entries to make EXPLAIN output look nice.  Note that we could dig 
	 * further than this (if we come up empty handed) by probing the range 
	 * table (root->parse->rtable), but this covers the ordinary cases. 
	 */
	foreach(lc, sub_tlist)
	{
		TargetEntry *tle = (TargetEntry*)lfirst(lc);

		if ( IsA(tle->expr, Var) && tle->resname == NULL )
		{
			TargetEntry *vartle = tlist_member((Node*)tle->expr, tlist);

			if ( vartle != NULL && vartle->resname != NULL )
				tle->resname = pstrdup(vartle->resname);
		}
	}

	/*
	 * Now look into doing a two-stage plan. Sometimes this isn't possible.
	 */
	if (has_aggs &&
			agg_counts->numDistinctAggs<=1 && 
			!agg_counts->missing_prelimfunc)
	{
		MppGroupPrep group_prep_2s = MPP_GRP_PREP_NONE;
		MppGroupType group_type_2s = MPP_GRP_TYPE_NONE;
		CdbPathLocus input_locus_2s, output_locus_2s;

		input_locus_2s = input_locus;
		CdbPathLocus_MakeNull(&output_locus_2s);

		/* We can to do a two-stage plan.  But do we want to?  If not,
		 * fall out of the if and stick with the one-stage approach. */
		if ( ! enable_agg_restruct
				|| !CdbPathLocus_IsPartitioned(input_locus) )
		{
			; /* Already collocated, don't bother.  This code is purely
			   * defensive since we will have taken an early exit above.
			   */
		}
		else if ( has_groups && agg_counts->numDistinctAggs==1 && !gp_enable_agg_distinct )
		{
			; /* If we continue, we'll use an initial redistibute motion to
			   * allow use of 2-phase aggregation with the lone agg distinct.
			   * Here, that is not enabled, so we bail out and use the
			   * 1-phase plan.
			   *
			   * Since HashAgg can't handle distinct, the 2-phase plan will be
			   *    (Agg (Sort (Motion (Agg (Sort (Motion R))))))
			   * which we probably won't like.  However, the 2-phase approach
			   * may be enabled using the GUC gp_enable_agg_distinct.
			   */
		}
		else /* Pick a 2-stage plan to compare to the 1-stage plan in hand. */
		{
			if ( agg_counts->numDistinctAggs ==  1 )
			{
				List *distinct_pathkeys = cdb_make_pathkey_for_expr(root,
						(Node *) agg_counts->distinctExpr,
						list_make1(makeString("=")));
				distinct_pathkeys =list_make1(distinct_pathkeys);

				if ( cdbpathlocus_collocates(input_locus, distinct_pathkeys))
					group_prep_2s = MPP_GRP_PREP_NONE;
				else
				{
					group_prep_2s = MPP_GRP_PREP_HASH_DISTINCT;
					CdbPathLocus_MakeHashed(&input_locus_2s, distinct_pathkeys);
				}
			}

			if ( has_groups )
			{
				group_type_2s = MPP_GRP_TYPE_GROUPED_2STAGE;
				CdbPathLocus_MakeHashed(&output_locus_2s, root->group_pathkeys);
			}
			else
			{
				group_type_2s = MPP_GRP_TYPE_PLAIN_2STAGE;
				CdbPathLocus_MakeSingleQE(&output_locus_2s);
			}
		}

		/* If we found a two-stage approach, compare it to the one-stage
		 * approach in hand and decide which we prefer.
		 *
		 * The cost of the one stage approach will include
		 * - the base plan,
		 * - motion to precondition input (if specified), and
		 * - the aggregation
		 *
		 * The cost of the two-stage approach will include
		 * - the base plan,
		 * - motion to hash input on distinct expression (if specified),
		 * - preliminary aggregration,
		 * - intermediate focus or hash motion, and
		 * - final aggregation.
		 *
		 * We assume that the cost of the input plan and final Agg balance.
		 * So the one-stage incremental cost is the cost  to move all the
		 * tuples.  The two-stage incremental cost is the cost to move all
		 * the tuples (only if preconditioning for distinct aggregation), the
		 * cost of the stage-one aggregation, and the cost of relocate a tuple
		 * per group.  We just need to compare these incremental costs.
		 */
		switch ( group_type_2s )
		{
			case MPP_GRP_TYPE_GROUPED_2STAGE:		
			case MPP_GRP_TYPE_PLAIN_2STAGE:
				/* TODO: Use cost to decide whether to implement 2 stage.*/
				group_prep = group_prep_2s;
				group_type = group_type_2s;
				output_locus = output_locus_2s;
				break;

			case MPP_GRP_TYPE_NONE:
				break; /* No 2-stage plan, use the 1-stage plan. */

			case MPP_GRP_TYPE_BASEPLAN:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_CDB_INTERNAL_ERROR),
						 errmsg("unexpected outcome for 2-stage aggregation")));

				break;
		}
	}

	/*
	 * Package up byproducts for the actual planner.
	 */
	ctx.prep = group_prep;
	ctx.type = group_type;
	ctx.tlist = tlist;
	ctx.sub_tlist = sub_tlist;
	ctx.use_hashed_grouping = use_hashed_grouping;
	ctx.best_path = best_path;
	ctx.cheapest_path = cheapest_path;
	ctx.input_locus = input_locus;
	ctx.output_locus = output_locus;
	ctx.agg_counts = agg_counts;
	ctx.tuple_fraction = tuple_fraction;
	ctx.p_dNumGroups = p_dNumGroups;
	ctx.canonical_grpsets = canonical_grpsets;
	ctx.current_pathkeys = NIL; /* Initialize to be tidy. */

	/*
	 * XXX We always use sequential plans for distinct-qualified
	 * rollup queries.
	 */
	if (is_grouping_extension(ctx.canonical_grpsets) &&
		agg_counts->numDistinctAggs > 0)
		return NULL;

	/* Call appropriate planner. */
	if (group_type == MPP_GRP_TYPE_BASEPLAN)
	{
		if (group_prep != MPP_GRP_PREP_NONE)
			result_plan = make_one_stage_agg_plan(root, &ctx);
		else
			result_plan = NULL; /* allow sequential planner to do the work. */
	}
	else if (group_type == MPP_GRP_TYPE_PLAIN_2STAGE || 
			 group_type == MPP_GRP_TYPE_GROUPED_2STAGE)
		result_plan = make_two_stage_agg_plan(root, &ctx);
	else
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("no parallel plan for aggregation")));	
	
	*pcurrent_pathkeys = ctx.current_pathkeys;
	return result_plan;
}


/*
 * Function make_one_stage_agg_plan
 *
 * Construct a one-stage aggregation plan by redistributing the result
 * of the input plan appropriately
 */
static Plan *
make_one_stage_agg_plan(PlannerInfo *root,
		MppGroupContext *ctx)
{
	Query	   *parse = root->parse;
	List	   *tlist = ctx->tlist;
	List	   *sub_tlist = ctx->sub_tlist;
	int			numGroupCols = ctx->numGroupCols;
	AttrNumber *groupColIdx = ctx->groupColIdx;
	Path       *best_path = ctx->best_path;
	Path       *cheapest_path = ctx->cheapest_path;
	Path       *path = NULL;
	bool        use_hashed_grouping = ctx->use_hashed_grouping;
	long		numGroups = (*(ctx->p_dNumGroups) < 0) ? 0 :
		(*(ctx->p_dNumGroups) > LONG_MAX) ? LONG_MAX :
		(long)*(ctx->p_dNumGroups);

	List       *groupExprs = NIL;
	List	   *current_pathkeys;
	QualCost	tlist_cost;
	double      motion_cost_per_row;
	int i;

	Plan       *result_plan;
	bool       is_grpext;

	is_grpext = is_grouping_extension(ctx->canonical_grpsets);
	

	/*
	 * The argument to the "lower" Agg node will use a "flattened" tlist
	 * (having just the (levelsup==0) vars mentioned in the SELECT targetlist
	 * and HAVING qual plus entries for any GROUP BY expressions that are not
	 * simple Vars.  This is the same sub_tlist as that used for 1-stage
	 * aggregation in grouping_planner.
	 */
	
	/* Create the base plan. */
	switch ( ctx->prep )
	{
	case MPP_GRP_PREP_NONE:
	case MPP_GRP_PREP_CULL_GROUPS:
	case MPP_GRP_PREP_CULL_DISTINCT:
	case MPP_GRP_PREP_CULL_QE:
	case MPP_GRP_PREP_FOCUS_QE:
	case MPP_GRP_PREP_FOCUS_QD:
		path = best_path; /* Can preserve sort order, so retain best. */
		break;
	case MPP_GRP_PREP_HASH_GROUPS:
	case MPP_GRP_PREP_HASH_DISTINCT:
	case MPP_GRP_PREP_BROADCAST:
		path = cheapest_path; /* Will lose sort order, so take the cheapest. */
		break;
	}
	result_plan = create_plan(root, path);
	current_pathkeys = path->pathkeys;
	
	/* Instead of the flat target list produced above, use the sub_tlist
	 * constructed in cdb_grouping_planner.  Add a Result node if the
	 * base plan can't project. (This may be unnecessary, but, if so,
	 * the Result node will be removed later.)
	 */
    result_plan = plan_pushdown_tlist(result_plan, sub_tlist);

    Assert(get_plan_flow(result_plan)); 
    
	/* Account for the cost of evaluation of the sub_tlist. */
	cost_qual_eval(&tlist_cost, sub_tlist);
	result_plan->startup_cost += tlist_cost.startup;
	result_plan->total_cost +=
		tlist_cost.startup +
		tlist_cost.per_tuple * result_plan->plan_rows;
	
	/* Precondition the input by adjusting its locus prior to adding
	 * the Agg or Group node to the base plan, if needed.
	 *
	 * In accounting for cost of motion, our rule is no increase in startup
	 * cost (doesn't sound right, but that's what to use for now) and a
	 * per row cost added to total cost (also doubtful).
	 */
	motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
				gp_motion_cost_per_row :
				2.0 * cpu_tuple_cost;

	switch ( ctx->prep )
	{
	case MPP_GRP_PREP_NONE:
			break;
			
	case MPP_GRP_PREP_HASH_GROUPS:
			Assert(numGroupCols > 0);
			for ( i = 0; i < numGroupCols; i++)
			{
				TargetEntry *tle = get_tle_by_resno(sub_tlist, groupColIdx[i]);
				groupExprs = lappend(groupExprs, copyObject(tle->expr));
			}

			result_plan = (Plan*)make_motion_hash(root, result_plan, groupExprs);
			result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
			current_pathkeys = NIL; /* No longer sorted. */
			break;
			
	case MPP_GRP_PREP_FOCUS_QE:
			result_plan = (Plan*)make_motion_gather_to_QE(result_plan, (current_pathkeys != NIL));
			result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
			break;
			
	case MPP_GRP_PREP_FOCUS_QD:
			result_plan = (Plan*)make_motion_gather_to_QD(result_plan, (current_pathkeys != NIL));	
			result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
			break;
		
	case MPP_GRP_PREP_HASH_DISTINCT:
	case MPP_GRP_PREP_CULL_GROUPS:
	case MPP_GRP_PREP_CULL_DISTINCT:
	case MPP_GRP_PREP_CULL_QE:
	case MPP_GRP_PREP_BROADCAST:
			ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("no parallel plan for aggregation")));	
			break; /* Never */
	}
	
    Assert(get_plan_flow(result_plan)); 
    
    /*
	 * Insert AGG or GROUP node if needed, plus an explicit sort step
	 * if necessary.
	 *
	 * HAVING clause, if any, becomes qual of the Agg or Group node.
	 */
	if (use_hashed_grouping)
	{
		/* Hashed aggregate plan --- no sort needed */
		result_plan = (Plan *) make_agg(root,
										tlist,
										(List *) parse->havingQual,
										AGG_HASHED,
										numGroupCols,
										groupColIdx,
										numGroups,
										0, /* rollup_level */
										0, /* grouping */
										0, /* rollup_gs_times */
										ctx->agg_counts->numAggs,
										ctx->agg_counts->transitionSpace,
										result_plan);
		/* Hashed aggregation produces randomly-ordered results */
		current_pathkeys = NIL;
	}
	else if (parse->hasAggs)
	{
		/* Plain aggregate plan --- sort if needed */
		AggStrategy aggstrategy;

		if (parse->groupClause)
		{
			if (!pathkeys_contained_in(root->group_pathkeys,
									   current_pathkeys))
			{
				result_plan = (Plan *)
					make_sort_from_groupcols(root,
											 parse->groupClause,
											 groupColIdx,
											 false,
											 result_plan);
				current_pathkeys = root->group_pathkeys;
				mark_sort_locus(result_plan);
			}
			aggstrategy = AGG_SORTED;

			/*
			 * The AGG node will not change the sort ordering of its
			 * groups, so current_pathkeys describes the result too.
			 */
		}
		else
		{
			aggstrategy = AGG_PLAIN;
			/* Result will be only one row anyway; no sort order */
			current_pathkeys = NIL;
		}

		if (!is_grpext)
			result_plan = (Plan *) make_agg(root,
											tlist,
											(List *) parse->havingQual,
											aggstrategy,
											numGroupCols,
											groupColIdx,
											numGroups,
											0, /* rollup_level */
											0, /* grouping */
											0, /* rollup_gs_times */
											ctx->agg_counts->numAggs,
											ctx->agg_counts->transitionSpace,
											result_plan);
		else
		{
			result_plan = plan_grouping_extension(root, path,
												  &tlist, sub_tlist,
												  true, false,
												  (List *) parse->havingQual,
												  aggstrategy,
												  &numGroupCols,
												  &groupColIdx,
												  ctx->agg_counts,
												  ctx->canonical_grpsets,
												  ctx->p_dNumGroups,
												  &current_pathkeys,
												  result_plan);
		}
	}
	else if (parse->groupClause)
	{
		/*
		 * GROUP BY without aggregation, so insert a group node (plus
		 * the appropriate sort node, if necessary).
		 *
		 * Add an explicit sort if we couldn't make the path come out
		 * the way the GROUP node needs it.
		 */
		if (!pathkeys_contained_in(root->group_pathkeys, current_pathkeys))
		{
			result_plan = (Plan *)
				make_sort_from_groupcols(root,
										 parse->groupClause,
										 groupColIdx,
										 false,
										 result_plan);
			current_pathkeys = root->group_pathkeys;
			mark_sort_locus(result_plan);
		}

		if (!is_grpext)
			result_plan = (Plan *) make_group(root,
											  tlist,
											  (List *) parse->havingQual,
											  numGroupCols,
											  groupColIdx,
											  *(ctx->p_dNumGroups),
											  0, /* rollup_level */
											  0, /* grouping */
											  0, /* rollup_gs_times */
											  result_plan);
		else
		{
			result_plan = (Plan *) plan_grouping_extension(root, path,
														   &tlist, sub_tlist,
														   false, false,
														   (List *) parse->havingQual,
														   AGG_SORTED,
														   &numGroupCols,
														   &groupColIdx,
														   ctx->agg_counts,
														   ctx->canonical_grpsets,
														   ctx->p_dNumGroups,
														   &current_pathkeys,
														   result_plan);
		}

		/* The Group node won't change sort ordering */
	}
	else if (root->hasHavingQual)
	{
		/* No aggregates, and no GROUP BY, but a HAVING qual is a
		 * degenerate case discussed in grouping_planner.  We can
		 * just throw away the plan-so-far and let the caller handle
		 * the whole enchilada.
		 */
		return NULL;
	}
	
    /* 
     * Decorate the top node with a Flow node if it doesn't have one yet.
     * (In such cases we require the next-to-top node to have a Flow node 
     * from which we can obtain the distribution info.)
     */
    if (!get_plan_flow(result_plan))
	{
		Assert(!IsA(result_plan, Motion)); 
        result_plan->flow = pull_up_Flow(result_plan, 
                                         result_plan->lefttree, 
                                         (current_pathkeys != NIL));
	}

	/* Marshal implicit results. Return explicit result. */
	ctx->current_pathkeys = current_pathkeys;
	return result_plan;
}

/*
 * Function make_two_stage_agg_plan
 *
 * Construct a two-stage aggregation plan.
 */
static Plan *
make_two_stage_agg_plan(PlannerInfo *root,
					   MppGroupContext *ctx)
{
	Query	   *parse = root->parse;
	List	   *tlist = ctx->tlist;
	List	   *sub_tlist = ctx->sub_tlist;
	List	   *prelim_tlist = NIL;
	List	   *final_tlist = NIL;
	List	   *final_qual = NIL;
	List	   *distinctExpr = NIL;
	List	   *groupExprs = NIL;
	List	   *current_pathkeys;
	Plan       *result_plan;
	QualCost	tlist_cost;
	double      motion_cost_per_row;
	AggStrategy aggstrategy;
	int			i;
	int			numGroupCols = ctx->numGroupCols;
	AttrNumber *groupColIdx = ctx->groupColIdx;
	AttrNumber *prelimGroupColIdx;
	Path       *path = ctx->best_path; /* no use for ctx->cheapest_path */
	bool        use_hashed_grouping = ctx->use_hashed_grouping;
	long		numGroups = (*(ctx->p_dNumGroups) < 0) ? 0 :
	                        (*(ctx->p_dNumGroups) > LONG_MAX) ? LONG_MAX :
							(long)*(ctx->p_dNumGroups);
	bool       is_grpext;

	is_grpext = is_grouping_extension(ctx->canonical_grpsets);
	
	motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
				gp_motion_cost_per_row :
				2.0 * cpu_tuple_cost;

	/* Create the base plan which will serve as the outer plan (argument)
	 * of the partial Agg node.
	 */
	result_plan = create_plan(root, path);
	current_pathkeys = path->pathkeys;

	/* Instead of the flat target list produced by create_plan above, use
	 * the sub_tlist constructed in cdb_grouping_planner.  This consists
	 * of just the (levelsup==0) vars mentioned in the SELECT and HAVING
	 * clauses plus entries for any GROUP BY expressions that are not
	 * simple Vars.  (This is the same sub_tlist as used in 1-stage
	 * aggregation and in normal aggregation in grouping_planner).
	 *
	 * If the base plan is of a type that can't project, add a Result
	 * node to carry the new target list, else install it directly.
	 * (Though the result node may not always be necessary, it is safe,
	 * and superfluous Result nodes are removed later.)
	 */
	result_plan = plan_pushdown_tlist(result_plan, sub_tlist);
	
	/* Account for the cost of evaluation of the sub_tlist. */
	cost_qual_eval(&tlist_cost, sub_tlist);
	result_plan->startup_cost += tlist_cost.startup;
	result_plan->total_cost +=
		tlist_cost.startup +
		tlist_cost.per_tuple * result_plan->plan_rows;
	
	/* At this point result_plan produces the input relation for two-stage
	 * aggregation.
	 *
	 * Begin by preconditioning the input, if necessary, to collocate on
	 * non-distinct values of a single DISTINCT argument.
	 */
	switch ( ctx->prep )
	{
	case MPP_GRP_PREP_NONE:
		break;
			
	case MPP_GRP_PREP_HASH_DISTINCT:
		Assert( ctx->agg_counts->numDistinctAggs == 1
				&& ctx->agg_counts->distinctExpr != NULL);
		distinctExpr = list_make1(copyObject(ctx->agg_counts->distinctExpr));
		result_plan = (Plan*)make_motion_hash(root, result_plan, distinctExpr);
		result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
		current_pathkeys = NIL; /* No longer sorted. */
		break;
			
	case MPP_GRP_PREP_FOCUS_QD:
	case MPP_GRP_PREP_FOCUS_QE:
	case MPP_GRP_PREP_HASH_GROUPS:
	case MPP_GRP_PREP_CULL_GROUPS:
	case MPP_GRP_PREP_CULL_DISTINCT:
	case MPP_GRP_PREP_CULL_QE:
	case MPP_GRP_PREP_BROADCAST:
			ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("unexpected call for two-stage aggregation")));	
			break; /* Never */
	}
	
	/*
	 * Get the target lists for the preliminary and final aggregations and
	 * the qual (HAVING clause) for the final aggregation based on the target
	 * list of the base plan. Grouping attributes go on front of preliminary
	 * target list.
	 */
	generate_two_stage_tlists(tlist,
					sub_tlist,
					parse->havingQual,
					numGroupCols,
					groupColIdx,
					&prelim_tlist,
					&final_tlist,
					&final_qual);
	
	/*
	 * Since the grouping attributes, if any, are on the front and in order
	 * on the preliminary targetlist, we need a different vector of grouping
	 * attribute numbers: (1, 2, 3, ...).  Later, we'll need
	 */
	prelimGroupColIdx = NULL;
	if ( numGroupCols > 0 )
	{
		prelimGroupColIdx = (AttrNumber*)palloc(numGroupCols * sizeof(AttrNumber));
		for ( i = 0; i < numGroupCols; i++ )
			prelimGroupColIdx[i] = i+1;
	}
	
	/* Determine the aggregation strategy to use. */
	if ( use_hashed_grouping )
	{
		aggstrategy = AGG_HASHED;
		current_pathkeys = NIL;
	}
	else
	{
		if (parse->groupClause)
		{
			if (!pathkeys_contained_in(root->group_pathkeys,
									   current_pathkeys))
			{
				/* TODO -- Investigate WHY we might sort here!
				 *
				 * Good reasons would be that one of the grouping
				 * expressions isn't "hashable" or that too may groups
				 * are anticipated.
				 *
				 * A bad reason would be that the final result will be in
				 * order of the grouping key.  (Redistribution will remove
				 * the ordering.)
				 */
				result_plan = (Plan *)
				make_sort_from_groupcols(root,
										 parse->groupClause,
										 groupColIdx,
										 false,
										 result_plan);
				current_pathkeys = root->group_pathkeys;
				mark_sort_locus(result_plan);
			}
			aggstrategy = AGG_SORTED;
			/* The AGG node will not change the sort ordering of its
			 * groups, so current_pathkeys describes the result too.
			 */
		}
		else
		{
			aggstrategy = AGG_PLAIN;
			current_pathkeys = NIL; /* One row, no sort order */
		}
	}
		
	/*
	 * Add the Preliminary Agg Node.
	 *
	 * When this aggregate is a ROLLUP, we add a sequence of preliminary Agg node.
	 */
	if (!is_grpext)
	{
		result_plan = (Plan *) make_agg(root,
										prelim_tlist,
										NIL, /* no havingQual */
										aggstrategy,
										numGroupCols,
										groupColIdx,
										numGroups,
										0, /* rollup_level */
										0, /* grouping */
										0, /* rollup_gs_times */
										ctx->agg_counts->numAggs,
										ctx->agg_counts->transitionSpace,
										result_plan);
		/* May lose useful locus and sort. Unlikely, but could do better. */
		mark_plan_strewn(result_plan);
		current_pathkeys = NIL;
	}

	else
	{
		result_plan = plan_grouping_extension(root, path,
											  &prelim_tlist, sub_tlist,
											  true, true,
											  NIL, /* no havingQual */
											  aggstrategy,
											  &numGroupCols,
											  &groupColIdx,
											  ctx->agg_counts,
											  ctx->canonical_grpsets,
											  ctx->p_dNumGroups,
											  &current_pathkeys,
											  result_plan);
		/* Since we add Grouping as an additional grouping column,
		 * we need to add it into prelimGroupColIdx. */
		prelimGroupColIdx = (AttrNumber *)
			repalloc(prelimGroupColIdx, 
					 numGroupCols * sizeof(AttrNumber));
		prelimGroupColIdx[numGroupCols-1] = groupColIdx[numGroupCols-1];
	}
	
	/*
	 * Add Intermediate Motion to Gather or Hash on Groups
	  */
	switch ( ctx->type )
	{
	case MPP_GRP_TYPE_GROUPED_2STAGE:
		groupExprs = NIL;
		Assert(numGroupCols > 0);
		for ( i = 0; i < numGroupCols; i++)
		{
			TargetEntry *tle;

			/* skip Grouping column */
			if (is_grpext && i == numGroupCols-1)
				continue;

			tle = get_tle_by_resno(prelim_tlist, prelimGroupColIdx[i]);
			groupExprs = lappend(groupExprs, copyObject(tle->expr));
		}
		result_plan = (Plan*)make_motion_hash(root, result_plan, groupExprs);
		result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
		break;
			
	case MPP_GRP_TYPE_PLAIN_2STAGE:
		result_plan = (Plan*)make_motion_gather_to_QE(result_plan, false);
		result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
		break;
	
	case MPP_GRP_TYPE_NONE:
	case MPP_GRP_TYPE_BASEPLAN:
		ereport(ERROR,
			(errcode(ERRCODE_CDB_INTERNAL_ERROR),
			errmsg("unexpected use of 2-stage aggregation")));	
		break; /* Never */
	}

	/*
	 * Add Sort on Groups if needed for AGG_SORTED strategy
	 */
	if (aggstrategy == AGG_SORTED)
	{
		result_plan = (Plan *)
			make_sort_from_groupcols(root,
									 parse->groupClause,
									 prelimGroupColIdx,
									 is_grpext,
									 result_plan);
		current_pathkeys = root->group_pathkeys;
		mark_sort_locus(result_plan);
	}

	{
		/*
		 * If this is the final Agg node for a rollup, we will set rollup_level to
		 * the special value "-1".
		 */
		int rollup_level = 0;
		if (is_grpext)
			rollup_level = -1;

		result_plan = add_second_stage_agg(root,
										   true,
										   prelim_tlist,
										   final_tlist,
										   final_qual,
										   aggstrategy,
										   numGroupCols,
										   prelimGroupColIdx,
										   rollup_level,
										   0,
										   0,
										   *ctx->p_dNumGroups,
										   ctx->agg_counts->numAggs,
										   ctx->agg_counts->transitionSpace,
										   "partial_aggregation",
										   &current_pathkeys,
										   result_plan);
	}
	
	/* Marshal implicit results. Return explicit result. */
	ctx->current_pathkeys = current_pathkeys;
	return result_plan;
}



/*
 * Function make_subplan_tlist
 *
 * The input to the "lower" Agg node will use a "flattened" tlist (having
 * just the (levelsup==0) vars mentioned in the targetlist and HAVING qual.
 * This is similar to the target list produced by make_subplanTargetList
 * and used for 1-stage aggregation in grouping_planner.
 *
 * The result target list contains entries for all the simple Var attributes
 * of the original SELECT and HAVING clauses, plus entries for any GROUP BY
 * expressions that are not simple Vars.
 *
 * The implicit results, delivered through pointers pnum and pcols, are the
 * number of grouping attributes and a vector of their positions (which are
 * equal to their resno's) in the target list.
 *
 * Note that there are no similar results for sort or distinct attributes
 * since they don't necessarily appear in the subplan list.
 */
List *make_subplan_tlist(List *tlist, Node *havingQual, List *grp_clauses,
						   int *pnum, AttrNumber **pcols)
{
	List	   *sub_tlist;
	List	   *extravars;

	int num;
	AttrNumber *cols;

	sub_tlist = flatten_tlist(tlist);
	extravars = pull_var_clause(havingQual, false);
	sub_tlist = add_to_flat_tlist(sub_tlist, extravars);
	list_free(extravars);
	
	num = num_distcols_in_grouplist(grp_clauses);
	if (num > 0)
	{
		int			keyno = 0;
		ListCell   *l;
		List       *tles;

		cols = (AttrNumber*) palloc(sizeof(AttrNumber) * num);

		tles = get_sortgroupclauses_tles(grp_clauses, tlist);

		foreach (l, tles)
		{
			Node	   *expr;
			TargetEntry *tle, *sub_tle = NULL;
			ListCell   *sl;

			tle = (TargetEntry*) lfirst(l);

			expr = (Node*)tle->expr;

			/* Find or make a matching sub_tlist entry. */
			foreach(sl, sub_tlist)
			{
				sub_tle = (TargetEntry *) lfirst(sl);
				if (equal(expr, sub_tle->expr))
					break;
			}
			if (!sl)
			{
				sub_tle = makeTargetEntry((Expr*) expr,
										  list_length(sub_tlist) + 1,
										  NULL,
										  false);
				sub_tlist = lappend(sub_tlist, sub_tle);
			}

			/* Set its group reference and save its resno */
			sub_tle->ressortgroupref = tle->ressortgroupref;
			cols[keyno++] = sub_tle->resno;
		}
		*pnum = num;
		*pcols = cols;
	}
	else
	{
		*pnum = 0;
		*pcols = NULL;
	}
	return sub_tlist; /* Possibly modified by appending expression entries. */
}


/*
 * Generate targetlist for a SubqueryScan node to wrap the stage-one
 * Agg node (partial aggregation) of a 2-Stage aggregation sequence.
 *
 * varno: varno to use in generated Vars
 * input_tlist: targetlist of this node's input node
 *
 * Result is a "flat" (all simple Var node) targetlist in which
 * varattno and resno match and are sequential.
 */
List *
generate_subquery_tlist(Index varno, List *input_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *j;
	TargetEntry *tle;
	Node	   *expr;

	foreach(j, input_tlist)
	{
		TargetEntry *inputtle = (TargetEntry *) lfirst(j);

		Assert(inputtle->resno == resno);

		/* Don't pull up constants, always use a Var to reference the input. */
		expr = (Node *) makeVar(varno,
								inputtle->resno,
								exprType((Node *) inputtle->expr),
								exprTypmod((Node *) inputtle->expr),
								0);
		/* We don't have to mark resjunk here, the second stage handles it. */
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  (inputtle->resname == NULL) ?
									NULL :
									pstrdup(inputtle->resname),
							  false);
		tle->ressortgroupref = inputtle->ressortgroupref;
		tlist = lappend(tlist, tle);
	}

	return tlist;
}


/*
 * Function: cdbpathlocus_collocates
 *
 * Is a relation with the given locus guaranteed to collocate tuples with
 * non-distinct values of the key.  The key is a list of pathkeys (each of
 * which is a list of PathKeyItem*).
 *
 * Collocation is guaranteed if the locus specifies a single process or
 * if the result is partitioned on a subset of the keys that must be
 * collocated.
 *
 * We ignore onther sorts of collocation, e.g., replication or partitioning
 * on a range since these cannot occur at the moment (MPP 2.3).
 */
bool cdbpathlocus_collocates(CdbPathLocus locus, List *pathkeys)
{
	ListCell *lc;
	List *exprs = NIL;
	
	if ( CdbPathLocus_IsBottleneck(locus) )
		return true;
	
	if ( !CdbPathLocus_IsHashed(locus) )
		return false;  /* Or would HashedOJ ok, too? */
	
	/* Extract a list of expressions from the pathkeys.  Since the locus
	 * presumably knows all about attribute equivalence classes, we use
	 * only the first item in each input path key. 
	 */
	foreach( lc, pathkeys )
	{
		PathKeyItem *item;
		List *lst = (List*)lfirst(lc);
		Assert( list_length(lst) > 0 );
		item = (PathKeyItem*)linitial(lst);
		exprs = lappend(exprs, item->key);
	}
	
	/* Check for containment of locus in exprs. */
	return cdbpathlocus_is_hashed_on_exprs(locus, exprs);
}


/*
 * Function: cdbpathlocus_from_flow
 *
 * Generate a locus from a flow.  Since the information needed to produce
 * canonical path keys is unavailable, this function will never return a
 * hashed locus.
 */
CdbPathLocus cdbpathlocus_from_flow(Flow *flow)
{
    CdbPathLocus    locus;

    CdbPathLocus_MakeNull(&locus);

    if (!flow)
        return locus;

    switch (flow->flotype)
    {
        case FLOW_SINGLETON:
            if (flow->segindex == -1)
                CdbPathLocus_MakeEntry(&locus);
            else
                CdbPathLocus_MakeSingleQE(&locus);
            break;
        case FLOW_REPLICATED:
            CdbPathLocus_MakeReplicated(&locus);
            break;
        case FLOW_PARTITIONED:
			CdbPathLocus_MakeStrewn(&locus);
            break;
        default:
            Insist(0);
    }
    return locus;
}

/*
 * Generate 3 target lists for a sequence of consecutive Agg nodes.
 *
 * This is intended for a sequence of consecutive Agg nodes used in 
 * a ROLLUP. '*p_tlist3' is for the upper Agg node, and '*p_tlist2' is
 * for any Agg node in the middle, and '*p_tlist1' is for the
 * bottom Agg node.
 *
 * '*p_tlist1' and '*p_tlist2' have similar target lists. '*p_tlist3'
 * is constructed based on tlist and outputs from *p_tlist2 or
 * '*p_tlist1' if twostage is true.
 */
void generate_three_tlists(List *tlist,
						   bool twostage,
						   List *sub_tlist,
						   Node *havingQual,
						   int numGroupCols,
						   AttrNumber *groupColIdx,
						   List **p_tlist1,
						   List **p_tlist2,
						   List **p_tlist3,
						   List **p_qual2,
						   List **p_qual3)
{
	ListCell *lc;
	int resno = 1;
	GroupId *gid;

	/* Similar to the final tlist entries in two-stage aggregation,
	 * we use consistent varno in the middle tlist entries.
	 */
	int middle_varno = 1;

	/* Generate the top and bottom tlists. */
	generate_two_stage_tlists(tlist,
							  sub_tlist,
							  havingQual,
							  numGroupCols,
							  groupColIdx,
							  p_tlist1,
							  p_tlist3,
							  p_qual3);

	*p_qual2 = *p_qual3;

	/*
	 * Read target entries in '*p_tlist1' one by one, and construct
	 * the entries for '*p_tlist2'. 
	 */
	foreach (lc, *p_tlist1)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);
		Expr *new_expr;
		TargetEntry *new_tle;

		if (IsA(tle->expr, Aggref))
		{
			Aggref *aggref = (Aggref *)tle->expr;
			Aggref *new_aggref = makeNode(Aggref);

			new_aggref->aggfnoid = aggref->aggfnoid;
			new_aggref->aggtype = aggref->aggtype;
			new_aggref->args =
				list_make1((Expr*)makeVar(middle_varno, tle->resno, aggref->aggtype, -1, 0));
			new_aggref->agglevelsup = 0;
			new_aggref->aggstar = false;
			new_aggref->aggdistinct = false; /* handled in preliminary aggregation */
			new_aggref->aggstage = AGGSTAGE_PARTIAL;

			new_expr = (Expr *)new_aggref;
		}

		else
		{
			/* Just make a new Var. */
			new_expr = (Expr *)makeVar(middle_varno,
									   tle->resno,
									   exprType((Node *)tle->expr),
									   exprTypmod((Node *)tle->expr),
									   0);
			
		}

		new_tle = makeTargetEntry(new_expr, resno,
								  (tle->resname == NULL) ?
								  NULL :
								  pstrdup(tle->resname),
								  false);
		*p_tlist2 = lappend(*p_tlist2, new_tle);

		resno++;
	}

	/*
	 * This may be called inside a two-stage aggregation. In this case,
	 * We want to make sure all entries in the '*p_tlist3' are visible.
	 */
	foreach (lc, *p_tlist3)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);

		if (twostage)
			tle->resjunk = false;

		/* We also set aggstage to AGGSTAGE_PARTIAL if this is in
		 * a two-stage aggregation, because the agg node in
		 * the second stage aggregation will do the finalize.
		 */
		if (twostage && IsA(tle->expr, Aggref))
		{
			Aggref *aggref = (Aggref *)tle->expr;
			aggref->aggstage = AGGSTAGE_PARTIAL;
		}
	}
}

/*
 * Function: generate_two_stage_tlists
 *
 * Generate target lists and having qual for 2-stage aggregation.
 *
 * Input is
 *
 *	tlist - the original target list of the query
 *	sub_tlist - the reduced target supplied as input to the aggregation
 *	havingQual - the original having qual of the query (list of Expr)
 *	numGroupCols - number of grouping attributes
 *	groupColIdx - resnos (= attr numbers) of the grouping attributes
 *
 * Output is pointers to
 *
 *	prelim_tlist - the target list of the preliminary Agg node.
 *	final_tlist - the target list of the final Agg node.
 *	final_qual - the qual of the final Agg node.
 */
void generate_two_stage_tlists(List *tlist,
						List *sub_tlist,
						Node *havingQual,
						int numGroupCols,
						AttrNumber *groupColIdx,
						List **p_prelim_tlist,
						List **p_final_tlist,
						List **p_final_qual)
{
	two_stage_agg_context ctx_data;
	List *prelim_tlist = NIL;
	List *final_tlist = NIL;
	List *final_qual = NIL;
	int i;
	ListCell *lc;
	
	/* Copy (read-only) arguments to context for subroutines. */
	ctx_data.tlist = tlist;
	ctx_data.sub_tlist = sub_tlist;
	ctx_data.havingQual = havingQual;
	ctx_data.numGroupCols = numGroupCols;
	ctx_data.groupColIdx = groupColIdx;
	
	/* Use consistent varno in final tlist entries.  It will refer to the
	 * sole RTE (a Subquery RTE) of a SubqueryScan. */
	ctx_data.final_varno = 1;
	
	/* Initialize temporaries to hold the two parts of the preliminary
	 * target list under construction. */
	ctx_data.grps_tlist = NIL;
	ctx_data.refs_tlist = NIL;

	/*
	 * Begin constructing the target list for the preliminary Agg node
	 * by placing targets for the grouping attributes on the grps_tlist
	 * temporary. Make sure ressortgroupref matches the original. Copying
	 * the expression may be overkill, but it is safe.
	 */
	for ( i = 0; i < numGroupCols; i++ )
	{
		TargetEntry *sub_tle, *prelim_tle;
		
		sub_tle = get_tle_by_resno(ctx_data.sub_tlist, groupColIdx[i]);
		prelim_tle = makeTargetEntry(copyObject(sub_tle->expr),
									list_length(ctx_data.grps_tlist) + 1,
									(sub_tle->resname == NULL) ?
											NULL :
											pstrdup(sub_tle->resname),
									false);
		prelim_tle->ressortgroupref = sub_tle->ressortgroupref;
		prelim_tle->resjunk = false;
		ctx_data.grps_tlist = lappend(ctx_data.grps_tlist, prelim_tle);
	}
	
	/*
	 * Derive the final target list with entries corresponding to the input
	 * target list, but referring to the attributes of the prelimnary target
	 * list rather than to the input attributes.  Note that this involves
	 * augmenting the refs_tlist temporary as we encounter new Aggref nodes.
	 */
	foreach (lc, ctx_data.tlist)
	{
		TargetEntry *tle, *final_tle;
		Expr *expr;
		
		tle = (TargetEntry*)lfirst(lc);
		expr = deconstruct_expr(tle->expr, &ctx_data);
		final_tle = makeTargetEntry(expr,
									tle->resno,
									(tle->resname == NULL) ?
										NULL :
										pstrdup(tle->resname),
									tle->resjunk);
		final_tle->ressortgroupref = tle->ressortgroupref;
		final_tlist = lappend(final_tlist, final_tle);
	}
	
	/*
	 * Derive the final qual while augmenting the preliminary target list. */
	final_qual = (List*)deconstruct_expr((Expr*)ctx_data.havingQual, &ctx_data);
	
	/* Glue the grps and refs parts together. (We kept resno current.) */
	prelim_tlist = list_concat(ctx_data.grps_tlist, ctx_data.refs_tlist);

	/* Set implicit results */
	*p_prelim_tlist = prelim_tlist;
	*p_final_tlist = final_tlist;
	*p_final_qual = final_qual;
}


/*
 * Function: deconstruct_expr
 *
 * Prepare an expression for execution within 2-stage aggregation.
 * This involves adding targets as needed to the target list of the
 * first (partial) aggregation and referring to this target list from
 * the modified expression for use in the second (final) aggregation.
 */
Expr *deconstruct_expr(Expr *expr, two_stage_agg_context *ctx)
{
	return (Expr*)deconstruct_expr_mutator((Node*)expr, ctx);
}

/*
 * Function: deconstruct_expr_mutator
 *
 * Work for deconstruct_expr.
 */
Node* deconstruct_expr_mutator(Node *node, two_stage_agg_context *ctx)
{
	TargetEntry *tle;
	
	if (node == NULL)
		return NULL;
		
	if (IsA(node, Aggref))
	{
		Aggref *aggref = (Aggref*)node;
		return (Node*)split_aggref(aggref, ctx);
	}
	
	/* If the given expression is a grouping expression, replace it with
	 * a Var node referring to the (lower) preliminary aggregation's
	 * target list.
	 */
	tle = tlist_member(node, ctx->grps_tlist);
	if ( tle != NULL )
	{
		return  (Node*) makeVar(ctx->final_varno, tle->resno,
								exprType((Node*)tle->expr),
								exprTypmod((Node*)tle->expr), 0);
	}

	return expression_tree_mutator(node, deconstruct_expr_mutator, (void*)ctx);
}


/*
 * Function: split_aggref
 *
 * Find or add a partial-stage Aggref expression for the argument in the
 * preliminary target list under construction.  Return the final-starge
 * Aggref with a single Var node argument referring to the partial-stage
 * Aggref.
 *
 * Note that he result type of the partial-stage Aggref will be the transition
 * type of the aggregate function.
 *
 * At execution, the function used to compute the transition type in the
 * lower aggregation will be the normal accumulator function for the
 * aggregate function.  The final function, however, will not be used.
 * The  result will be the ending transition value.
 *
 * At execution, the function used to compute the transition type in the
 * upper aggregation will be the prelim function (known to exist, else
 * we would have rejected 2-stage aggregation as a strategy) on input
 * values of the transition type.  The normal accumulator function for the
 * aggregate function will not  be used.  The normal final function will
 * be used to convert the ending transition value to the result type.
 * aggregation
 */
Aggref *split_aggref(Aggref *aggref, two_stage_agg_context *ctx)
{
	ListCell *cell;
	Aggref *final_aggref;
	Oid transtype = InvalidOid;
	AttrNumber attrno = OUTER;
	TargetEntry *prelim_tle = NULL;

	Assert(aggref != NULL  && aggref->agglevelsup == 0);
	
	/*
	 * We may have seen an Aggref just like this one already.  Look for
	 * the preliminary form of such in the preliminary Aggref target
	 * list to date.
	 */
	foreach( cell, ctx->refs_tlist )
	{
		TargetEntry *tle = (TargetEntry*)lfirst(cell);
		Aggref *ref = (Aggref*)tle->expr;
		
		/* Check similarity, avoid aggtype and aggstage (which we control)
		 * and don't bother with agglevelsup (which is always 0 here).
		 */
		if ( aggref->aggfnoid == ref->aggfnoid
			&& aggref->aggstar == ref->aggstar
			&& aggref->aggdistinct == ref->aggdistinct
			&& equal(aggref->args, ref->args) )
		{
			prelim_tle = tle;
			transtype = ref->aggtype;
			attrno = prelim_tle->resno;
			break;
		}
	}

	/*
	 * If no existing preliminary Aggref target matched, add one that does.
	 */
	if ( prelim_tle == NULL )
	{
		Aggref *ref;
		
		/* Get type information for the Aggref */
		transtype = lookup_agg_transtype(aggref);
		
		/* Make a new preliminary Aggref wrapped as a new target entry. */		
		ref = (Aggref*)copyObject(aggref);
		ref->aggtype = transtype;
		ref->aggstage = AGGSTAGE_PARTIAL;

		attrno = 1+list_length(ctx->grps_tlist) + list_length(ctx->refs_tlist);
		prelim_tle = makeTargetEntry((Expr*)ref, attrno, NULL, false);
		ctx->refs_tlist = lappend(ctx->refs_tlist, prelim_tle);
	}
	
	/*
	 * Make and return the final Aggref node corresponding to the preliminary
	 * one we found or made above.
	 */
	final_aggref = makeNode(Aggref);
	final_aggref->aggfnoid = aggref->aggfnoid;
	final_aggref->aggtype = aggref->aggtype;
	final_aggref->args = list_make1((Expr*)makeVar(ctx->final_varno, attrno, transtype, -1, 0));
	final_aggref->agglevelsup = 0;
	final_aggref->aggstar = false;
	final_aggref->aggdistinct = false; /* handled in preliminary aggregation */
	final_aggref->aggstage = AGGSTAGE_FINAL;
	
	return final_aggref;
}


/* Function lookup_agg_transtype
 *
 * Return the transition type Oid of the given aggregate fuction or throw
 * an error, if none.
 */
Oid lookup_agg_transtype(Aggref *aggref)
{
	HeapTuple aggTuple;
	Form_pg_aggregate aggform;
	Oid aggtranstype;

	aggTuple = SearchSysCache(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid), 0, 0, 0);
	if (!HeapTupleIsValid(aggTuple))
	{
		elog(ERROR, "cache lookup failed for aggregate %u", aggref->aggfnoid);
	}
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggtranstype = aggform->aggtranstype;
	ReleaseSysCache(aggTuple);

	return aggtranstype;
}

/* Function:  adapt_flow_to_targetlist
 *
 * Sometimes we replace the targetlist (especially subplan of an Agg).
 * In this case, we need to assure that any hash expressions in the
 * node's flow remain in the targetlist OR null out the hash leaving,
 * in effect, a strewn flow.
 */
void adapt_flow_to_targetlist(Plan *plan)
{
	ListCell   *c;
	Flow *flow;

	Assert(plan != NULL);
	flow = get_plan_flow(plan);

	Assert(flow); 

	foreach(c, flow->hashExpr)
	{
		Node *expr = (Node*)lfirst(c);
		if (!tlist_member(expr, plan->targetlist))
        {
		    flow->hashExpr = NIL;
			break;
        }
	}
}

/*
 * Function: add_second_stage_agg
 *
 * Add an Agg/Group node on top of an Agg/Group node. We add a SubquerySan
 * node on top of the existing Agg/Group node before adding the new Agg/Group
 * node.
 *
 * Params:
 *  is_agg -- indicate to add an Agg or a Group node.
 *  prelim_tlist -- the targetlist for the existing Agg/Group node.
 *  final_tlist -- the targetlist for the new Agg/Group node.
 */
Plan *
add_second_stage_agg(PlannerInfo *root,
					 bool is_agg,
					 List *prelim_tlist,
					 List *final_tlist,
					 List *final_qual,
					 AggStrategy aggstrategy,
					 int numGroupCols,
					 AttrNumber *prelimGroupColIdx,
					 int rollup_level,
					 uint64 grouping,
					 int rollup_gs_times,
					 double numGroups,
					 int numAggs,
					 int transSpace,
					 const char *alias,
					 List **p_current_pathkeys,
					 Plan *result_plan)
{
	Query	   *parse = root->parse;
	Query	   *subquery;
	List	   *newrtable;
	RangeTblEntry *newrte;
	RangeTblRef *newrtref;
	Plan         *agg_node;

	/*
	 * Add a SubqueryScan node to renumber the range of the query.
	 *
	 * The result of the preliminary aggregation my contain targets that
	 * have no representatives in the range of its outer relation.  We
	 * resolve this be treating the preliminary aggregation as a subquery.
	 *
	 * However, this breaks the correspondence between the Plan tree and
	 * the Query tree that is assumed by the later call to set_plan_references.
	 * So we also push the Query node from the root structure down into a new
	 * subquery RTE and scribble over the original Query node to make it into
	 * Simple SELECT * FROM a Subquery RTE.
	 *
	 * Note that the final Agg should refer to the attributes of the result
	 * SubqueryScan plan node.
	 */
	 		
	/* Move the root query aside into a newly allocated Query node. */
	subquery = makeNode(Query);
	memcpy(subquery, parse, sizeof(Query));
	subquery->targetList = copyObject(prelim_tlist);
	subquery->havingQual = NULL;
	
	/* Subquery attributes shouldn't be marked as junk, else they'll be
	 * skipped by addRangeTableEntryForSubquery. */
	{
		ListCell *cell;
		
		foreach ( cell, subquery->targetList )
		{
			TargetEntry *tle = (TargetEntry *)lfirst(cell);	
			tle->resjunk = false;
			if ( tle->resname == NULL )
			{
				const char *fmt = "unnamed_attr_%d";
				char buf[32]; /* big enuf for fmt */
				sprintf(buf,fmt,tle->resno);
				tle->resname = pstrdup(buf);
			}
		}
	}
	
	/* Construct a range table entry referring to it. */
	newrte = addRangeTableEntryForSubquery(NULL,
										   subquery,
										   makeAlias(alias, NULL),
										   TRUE);
	newrtable = list_make1(newrte);

	/* Modify the root query in place to look like a simple Subquery. */
	parse->querySource = QSRC_PLANNER; /* but remember it's really ours  */
	parse->rtable = newrtable;
	parse->jointree = makeNode(FromExpr);
	newrtref = makeNode(RangeTblRef);
	newrtref->rtindex = 1;
	parse->jointree->fromlist = list_make1(newrtref);
	parse->jointree->quals = NULL;
	parse->rowMarks = NIL;

	parse->targetList = generate_subquery_tlist(1, prelim_tlist); /* Query varno is 1 */
	/* Do we need to fill in more?  I don't think so. */
	
	result_plan = (Plan*)make_subqueryscan(parse->targetList,
										   NIL,
										   1, /* scanrelid (= varno) */
										   result_plan);
	mark_passthru_locus(result_plan, true, true);
	
	if ( *p_current_pathkeys != NULL )
	{
		/* Generate current_pathkeys with new var numbering.  The added
		 * SubqueryScan scrambled them.
		 */
		List *new_keys = NIL;
		ListCell *c;
		int i = 1;

		/*
		 * There may be duplicate keys in the GroupClause, but
		 * the pathkeys contain non-duplicate keys. So,
		 * numGroupCols is not necessarily equal to the number
		 * of keys in *p_current_pathkeys. But we want to make
		 * sure that numGroupCols is not less than this number.
		 */
		Assert( numGroupCols >= list_length(*p_current_pathkeys) );

		foreach ( c, *p_current_pathkeys )
		{
			List *k;
			PathKeyItem *pki;
			TargetEntry *tle;
			Oid sortop;
			
			k = (List *)lfirst(c);
			
			/* Need the pathkey sortop, use the first on the list. */
			Assert( IsA(k,List) && list_length(k) > 0 );
			pki = (PathKeyItem*)linitial(k);
			sortop = pki->sortop;
			
			tle = get_tle_by_resno(parse->targetList, i);
			pki = makeNode(PathKeyItem);
			pki->key = copyObject((Node*)tle->expr);
			pki->sortop = sortop;
			
			new_keys = lappend(new_keys, list_make1(pki));
			i++;
		}
		
		*p_current_pathkeys = new_keys;
	}

	/* Add an Agg/Group node */
	if (is_agg)
	{
		/* convert current_numGroups to long int */
		long lNumGroups = (long) Min(numGroups, (double) LONG_MAX);

		agg_node = (Plan *)make_agg(root,
									final_tlist,
									final_qual,
									aggstrategy,
									numGroupCols,
									prelimGroupColIdx,
									lNumGroups,
									rollup_level,
									grouping,
									rollup_gs_times,
									numAggs,
									transSpace,
									result_plan);
	}

	else
	{
		agg_node = (Plan *)make_group(root,
									  final_tlist,
									  final_qual,
									  numGroupCols,
									  prelimGroupColIdx,
									  numGroups,
									  rollup_level,
									  grouping,
									  rollup_gs_times,
									  result_plan);
	}

	/*
	 * Agg/Group will not change the sort order, so we pull up
	 * the Flow here
	 */
	agg_node->flow = pull_up_Flow(agg_node, 
								  agg_node->lefttree, 
								  (*p_current_pathkeys != NIL));

	return agg_node;
}
