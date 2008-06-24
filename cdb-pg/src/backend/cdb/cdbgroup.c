/*-------------------------------------------------------------------------
 *
 * cdbgroup.c
 *	  Routines to aid in planning grouping queries for parallel
 *    execution.  This is, essentially, an extension of the file
 *    optimizer/prep/planner.c, although some functions are not
 *    externalized.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
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
#include "optimizer/planshare.h"
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
#include "cdb/cdbhash.h"        /* isGreenplumDbHashable() */

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
 * MPP_GRP_TYPE_PLAIN_DQA_2STAGE,
 *    Ungrouped, 2-stage aggregation.
 * MPP_GRP_TYPE_GROUPED_DQA_2STAGE,
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
	MPP_GRP_TYPE_PLAIN_DQA_2STAGE,
	MPP_GRP_TYPE_GROUPED_DQA_2STAGE,
} MppGroupType;


/* DQA coplan information */
typedef struct DqaInfo
{
	Node *distinctExpr; /* By reference from agg_counts for convenience. */
	AttrNumber base_index; /* Index of attribute in base plan targetlist */
	bool can_hash;
	double num_rows; /* Estimated cardinality of grouping key, dqa arg */
	Plan *coplan; /* Coplan for this (later this and all prior) coplan */
	Query *parse; /* Plausible root->parse for the coplan. */
} DqaInfo;

typedef struct MppGroupContext
{
	MppGroupPrep prep;
	MppGroupType type;
	
	List *tlist; /* The preprocessed targetlist of the original query. */
	Node *havingQual; /* The proprocessed having qual of the original query. */
	Path *best_path;
	Path *cheapest_path;
	AggClauseCounts *agg_counts;
	double tuple_fraction;
	double *p_dNumGroups; /* Group count estimate shared up the call tree. */
	CanonicalGroupingSets *canonical_grpsets;

	List *sub_tlist; /* Derived (in cdb_grouping_planner) input targetlist. */
	int numGroupCols;
	AttrNumber *groupColIdx;
	int numDistinctCols;
	AttrNumber *distinctColIdx;
	DqaInfo *dqaArgs;
	bool use_hashed_grouping;
	CdbPathLocus input_locus;
	CdbPathLocus output_locus;
	List *current_pathkeys;

	/* Indicate if root->parse has been changed during planning */
	bool querynode_changed;
	
	/* Work space for aggregate/tlist deconstruction and reconstruction */
	Index final_varno; /* input */
	bool use_irefs_tlist; /* input */
	bool use_dqa_pruning; /* input */
	List *prefs_tlist; /* Aggref attributes for prelim_tlist */
	List *irefs_tlist; /* Aggref attributes for optional inter_tlist */
	List *frefs_tlist; /* Aggref attributes for optional join tlists */
	List *dqa_tlist; /* DQA argument attributes for prelim_tlist */
	List **dref_tlists; /* Array of DQA Aggref tlists (dqa_tlist order) */
	List *grps_tlist; /* Grouping attributes for prelim_tlist */
	List *fin_tlist; /* Final tlist cache. */
	List *fin_hqual; /* Final having qual cache. */
	Index outer_varno; /* work */
	Index inner_varno; /* work */
	int *dqa_offsets; /* work */
	List *top_tlist; /* work - the target list to finalize */
	
} MppGroupContext;

/* Constants for DQA pruning: 
 */
static const Index grp_varno = 1; /* var refers to grps_tlist */
static const Index ref_varno = 2; /* var refers to prefs_tlist or relatives */
static const Index dqa_base_varno = 3; /* refers to one of the dref_tlists */

/* Forward declarations */

static Plan * make_one_stage_agg_plan(PlannerInfo *root, MppGroupContext *ctx);
static Plan * make_two_stage_agg_plan(PlannerInfo *root, MppGroupContext *ctx);
static Plan * make_two_stage_agg_plan_with_dqas(PlannerInfo *root, MppGroupContext *ctx);
static Plan * make_plan_for_one_dqa(PlannerInfo *root, MppGroupContext *ctx, 
									int dqa_index, Plan* result_plan,
									bool final_sort, Query** coquery_p);
static Plan * join_dqa_coplan(PlannerInfo *root, MppGroupContext *ctx, Plan *plan, int dqa_index);
static int compareDqas(const void *larg, const void *rarg);
static void planDqaJoinOrder(PlannerInfo *root, MppGroupContext *ctx, 
						   double input_rows);
static List *make_subplan_tlist(List *tlist, Node *havingQual, 
								List *grp_clauses, int *pnum_gkeys, AttrNumber **pcols_gkeys,
								List *dqa_args, int *pnum_dqas, AttrNumber **pcols_dqas);
static List *augment_subplan_tlist(List *tlist, List *exprs, int *pnum, AttrNumber **pcols);
static void generate_multi_stage_tlists(MppGroupContext* ctx,
						List **p_prelim_tlist,
						List **p_inter_tlist,
						List **p_final_tlist,
						List **p_final_qual);
static void prepare_dqa_pruning_tlists(MppGroupContext *ctx);
static void generate_dqa_pruning_tlists(MppGroupContext *ctx,
						int dqa_index,
						List **p_prelim_tlist,
						List **p_inter_tlist,
						List **p_final_tlist,
						List **p_final_qual);
static void deconstruct_agg_info(MppGroupContext *ctx);
static void reconstruct_agg_info(MppGroupContext *ctx, 						
								List **p_prelim_tlist,
								List **p_inter_tlist,
								List **p_final_tlist,
								List **p_final_qual);
static void reconstruct_coplan_info(MppGroupContext *ctx, 
									int dqa_index,
									List **p_prelim_tlist,
									List **p_inter_tlist,
									List **p_final_tlist);
static Expr *deconstruct_expr(Expr *expr, MppGroupContext *ctx);
static Node* deconstruct_expr_mutator(Node *node, MppGroupContext *ctx);
static Node *split_aggref(Aggref *aggref, MppGroupContext *ctx);
static List *make_vars_tlist(List *tlist, Index varno, AttrNumber offset);
static Plan* add_subqueryscan(PlannerInfo* root, List **p_pathkeys,
					   Index varno, Query *subquery, Plan *subplan);
static List *seq_tlist_concat(List *tlist1, List *tlist2);
static Node *finalize_split_expr(Node *expr, MppGroupContext *ctx);
static Node* finalize_split_expr_mutator(Node *node, MppGroupContext *ctx);
static Oid lookup_agg_transtype(Aggref *aggref);
static bool hash_safe_type(Oid type);
static bool sorting_prefixes_grouping(PlannerInfo *root);
static bool gp_hash_safe_grouping(PlannerInfo *root);

/*
 * Function: cdb_grouping_planner
 *
 * This is basically an extension of the function grouping_planner() from
 * planner.c.  It (conditionally) replaces the logic in the "normal case"
 * (the aggregation/grouping branch) of the main "if" statement.
 *
 * The result is a Plan for one-, two-, or three-phase grouping/aggregation
 * (possibly including a top-level join in case of DQA pruning) or NULL.
 *
 * A NULL result means that the ordinary sequential planner will produce
 * a correct (and preferred) result, so the "normal case" code should  be
 * used.
 *
 * A non-NULL result is taken as a partially formed Plan to be processed
 * by the trailing sort/distinct/limit logic of grouping_planner().  In
 * otherwords, after any associated changes to the local environment (see
 * the calling code), the resulting plan should be treated as if from the
 * "normal case" or the function optimize_minmax_aggregates().
 *
 * The pointer at location pcurrent_pathkeys is updated to refer to the
 * ordering pathkey or NIL, if none.  The parse tree at root->parse may
 * be modified in place to reflect changes in the context (e.g. current
 * range table).
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
					 List **pcurrent_pathkeys,
					 bool *querynode_changed)
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
	int numGroupCols = 0;
	AttrNumber *groupColIdx = NULL;
	int numDistinctCols = 0;
	AttrNumber *distinctColIdx = NULL;

	Assert( !has_groups || root->group_pathkeys != NULL );

	CdbPathLocus_MakeNull(&output_locus);
	memset(&ctx, 0, sizeof(ctx));

	*querynode_changed = false;

	/* We always use sequential plans for distinct-qualified rollup queries,
	 * so don't waste time working on alternatives.
	 */
	if (is_grouping_extension(canonical_grpsets) &&
		agg_counts->numDistinctAggs > 0)
		return NULL;

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
			if (gp_hash_safe_grouping(root))
			{
				group_prep = MPP_GRP_PREP_HASH_GROUPS;
				CdbPathLocus_MakeHashed(&output_locus, root->group_pathkeys);
			}
			else
			{
				group_prep = MPP_GRP_PREP_FOCUS_QE;
				CdbPathLocus_MakeSingleQE(&output_locus);
			}
		}
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
	 * in the target list (numGroupCols, groupColIdx).
	 *
	 * Also make sure there's a target entry with a non-zero sortgroupref
	 * for each DQA argument and note the location of the attributes
	 * (numDistinctCols, distinctColIdx).
	 */
	sub_tlist = make_subplan_tlist(tlist,
			root->parse->havingQual,
			root->parse->groupClause,
			&numGroupCols,
			&groupColIdx,
			agg_counts->dqaArgs, 
			&numDistinctCols,
			&distinctColIdx);

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
	 * Per MPP-2378, we don't insist on has_aggs for multi-phase plans
	 * anymore.
	 */
	if ( !agg_counts->missing_prelimfunc) /* Possible improvement some DQAs 2phase, some 1phase? */
	{
		MppGroupPrep group_prep_2s = MPP_GRP_PREP_NONE;
		MppGroupType group_type_2s = MPP_GRP_TYPE_NONE;
		CdbPathLocus input_locus_2s, output_locus_2s;

		input_locus_2s = input_locus;
		CdbPathLocus_MakeNull(&output_locus_2s);

		/* We can to do a two-stage plan.  But do we want to?  If not,
		 * fall out of the if and stick with the one-stage approach. */
		if ( ! enable_agg_restruct )
		{
			; /* 2-phase disallowed by configuration. */
					}
		else if ( ! CdbPathLocus_IsPartitioned(input_locus) )
		{
			; /* Already collocated, don't bother.  This code is purely
			   * defensive since we will have taken an early exit above.
			   */
		}
		else if ( ! gp_enable_dqa_pruning || list_length(agg_counts->dqaArgs)==0 )
		{
			if ( has_groups && list_length(agg_counts->dqaArgs)==1 && !gp_enable_agg_distinct )
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
			else if ( list_length(agg_counts->dqaArgs) > 1 )
			{
				; /* Don't use a 2-phase plan on an mDQA query when 
				   * gp_enable_dqa_pruning is off. */
			}
			else /* Pick a 2-stage plan to compare to the 1-stage plan in hand. */
			{
				if ( list_length(agg_counts->dqaArgs) ==  1 )
				{
					List *distinct_pathkeys;
					
					Insist( IsA(agg_counts->dqaArgs, List) &&
							list_length((List*)agg_counts->dqaArgs) == 1 );
					distinct_pathkeys = cdb_make_pathkey_for_expr(root,
							linitial(agg_counts->dqaArgs),
							list_make1(makeString("=")));
					distinct_pathkeys = list_make1(distinct_pathkeys);

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
		}
		else /* Pruning is enabled and there are some DQAs. */
		{
			group_prep_2s = MPP_GRP_PREP_NONE;
			if ( has_groups )
			{
				group_type_2s = MPP_GRP_TYPE_GROUPED_DQA_2STAGE;
				CdbPathLocus_MakeHashed(&output_locus_2s, root->group_pathkeys);
			}
			else
			{
				group_type_2s = MPP_GRP_TYPE_PLAIN_DQA_2STAGE;
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
			case MPP_GRP_TYPE_GROUPED_DQA_2STAGE:		
			case MPP_GRP_TYPE_PLAIN_DQA_2STAGE:
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
	ctx.havingQual = root->parse->havingQual;
	ctx.sub_tlist = sub_tlist;
	ctx.numGroupCols = numGroupCols;
	ctx.groupColIdx = groupColIdx;
	ctx.numDistinctCols = numDistinctCols;
	ctx.distinctColIdx = distinctColIdx;
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
	ctx.querynode_changed = false;

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
	else if (group_type == MPP_GRP_TYPE_PLAIN_DQA_2STAGE || 
			 group_type == MPP_GRP_TYPE_GROUPED_DQA_2STAGE)
		result_plan = make_two_stage_agg_plan_with_dqas(root, &ctx);
	else
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("no parallel plan for aggregation")));	

	*querynode_changed = ctx.querynode_changed;
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

    Assert(result_plan->flow); 
    
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
	case MPP_GRP_PREP_BROADCAST:
			ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("no parallel plan for aggregation")));	
			break; /* Never */
	}
	
    Assert(result_plan->flow); 
    
    /*
	 * Insert AGG or GROUP node if needed, plus an explicit sort step
	 * if necessary.
	 *
	 * HAVING clause, if any, becomes qual of the Agg or Group node.
	 */
	if (!is_grpext && use_hashed_grouping)
	{
		/* Hashed aggregate plan --- no sort needed */
		result_plan = (Plan *) make_agg(root,
										tlist,
										(List *) parse->havingQual,
										AGG_HASHED, false,
										numGroupCols,
										groupColIdx,
										numGroups,
										0, /* num_nullcols */
										0, /* input_grouping */
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
		if (!is_grpext)
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

			result_plan = (Plan *) make_agg(root,
											tlist,
											(List *) parse->havingQual,
											aggstrategy, false,
											numGroupCols,
											groupColIdx,
											numGroups,
											0, /* num_nullcols */
											0, /* input_grouping */
											0, /* grouping */
											0, /* rollup_gs_times */
											ctx->agg_counts->numAggs,
											ctx->agg_counts->transitionSpace,
											result_plan);
		}

		else
		{
			result_plan = plan_grouping_extension(root, path, ctx->tuple_fraction,
												  &tlist, sub_tlist,
												  true, false,
												  (List *) parse->havingQual,
												  &numGroupCols,
												  &groupColIdx,
												  ctx->agg_counts,
												  ctx->canonical_grpsets,
												  ctx->p_dNumGroups,
												  &(ctx->querynode_changed),
												  &current_pathkeys,
												  result_plan);
		}
	}
	else if (parse->groupClause)
	{
		if (!is_grpext)
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

			result_plan = (Plan *) make_group(root,
											  tlist,
											  (List *) parse->havingQual,
											  numGroupCols,
											  groupColIdx,
											  *(ctx->p_dNumGroups),
											  0, /* num_nullcols */
											  0, /* input_grouping */
											  0, /* grouping */
											  0, /* rollup_gs_times */
											  result_plan);
		}

		else
		{
			result_plan = (Plan *) plan_grouping_extension(root, path, ctx->tuple_fraction,
														   &tlist, sub_tlist,
														   false, false,
														   (List *) parse->havingQual,
														   &numGroupCols,
														   &groupColIdx,
														   ctx->agg_counts,
														   ctx->canonical_grpsets,
														   ctx->p_dNumGroups,
														   &(ctx->querynode_changed),
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
    if (!result_plan->flow)
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
	int			numGroupCols;
	AttrNumber *groupColIdx;
	AttrNumber *prelimGroupColIdx;
	Path       *path = ctx->best_path; /* no use for ctx->cheapest_path */
	long		numGroups = (*(ctx->p_dNumGroups) < 0) ? 0 :
	                        (*(ctx->p_dNumGroups) > LONG_MAX) ? LONG_MAX :
							(long)*(ctx->p_dNumGroups);
	bool       is_grpext;

	is_grpext = is_grouping_extension(ctx->canonical_grpsets);
	
	motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
				gp_motion_cost_per_row :
				2.0 * cpu_tuple_cost;
		
	/* Copy these from context rather than using them directly because we may 
	 * scribble on them in plan_grouping_extension().  It would be good to 
	 * clean this up, but not today.
	 */
	numGroupCols = ctx->numGroupCols;
	groupColIdx = ctx->groupColIdx;

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
	result_plan = plan_pushdown_tlist(result_plan, ctx->sub_tlist);
	
	/* Account for the cost of evaluation of the sub_tlist. */
	cost_qual_eval(&tlist_cost, ctx->sub_tlist);
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
		Assert(list_length( ctx->agg_counts->dqaArgs) == 1 );
		Assert( ctx->agg_counts->dqaArgs != NIL);
		distinctExpr = list_make1(linitial(ctx->agg_counts->dqaArgs));
		distinctExpr = copyObject(distinctExpr);
		result_plan = (Plan*)make_motion_hash(root, result_plan, distinctExpr);
		result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
		current_pathkeys = NIL; /* No longer sorted. */
		break;
			
	case MPP_GRP_PREP_FOCUS_QD:
	case MPP_GRP_PREP_FOCUS_QE:
	case MPP_GRP_PREP_HASH_GROUPS:
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
	generate_multi_stage_tlists(ctx,
					&prelim_tlist,
					NULL,
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
	
	/*
	 * Add the Preliminary Agg Node.
	 *
	 * When this aggregate is a ROLLUP, we add a sequence of preliminary Agg node.
	 */
	/* Determine the aggregation strategy to use. */
	if ( ctx->use_hashed_grouping )
	{
		aggstrategy = AGG_HASHED;
		current_pathkeys = NIL;
	}
	else
	{
		if (parse->groupClause)
		{
			if (!is_grpext && !pathkeys_contained_in(root->group_pathkeys,
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
	
	if (!is_grpext)
	{
		result_plan = (Plan *) make_agg(root,
										prelim_tlist,
										NIL, /* no havingQual */
										aggstrategy, gp_hashagg_streambottom,
										numGroupCols,
										groupColIdx,
										numGroups,
										0, /* num_nullcols */
										0, /* input_grouping */
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
		result_plan = plan_grouping_extension(root, path, ctx->tuple_fraction,
											  &prelim_tlist, ctx->sub_tlist,
											  true, true,
											  NIL, /* no havingQual */
											  &numGroupCols,
											  &groupColIdx,
											  ctx->agg_counts,
											  ctx->canonical_grpsets,
											  ctx->p_dNumGroups,
											  &(ctx->querynode_changed),
											  &current_pathkeys,
											  result_plan);
		/* Since we add Grouping as an additional grouping column,
		 * we need to add it into prelimGroupColIdx. */
		if (prelimGroupColIdx != NULL)
			prelimGroupColIdx = (AttrNumber *)
				repalloc(prelimGroupColIdx, 
						 numGroupCols * sizeof(AttrNumber));
		else
			prelimGroupColIdx = (AttrNumber *)
				palloc0(numGroupCols * sizeof(AttrNumber));
		
		Assert(numGroupCols >= 2);
		prelimGroupColIdx[numGroupCols-1] = groupColIdx[numGroupCols-1];
		prelimGroupColIdx[numGroupCols-2] = groupColIdx[numGroupCols-2];
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

			/* skip Grouping/GroupId columns */
			if (is_grpext && (i == numGroupCols-1 || i == numGroupCols-2))
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
	case MPP_GRP_TYPE_GROUPED_DQA_2STAGE:
	case MPP_GRP_TYPE_PLAIN_DQA_2STAGE:
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

	result_plan = add_second_stage_agg(root,
									   true,
									   prelim_tlist,
									   final_tlist,
									   final_qual,
									   aggstrategy,
									   numGroupCols,
									   prelimGroupColIdx,
									   0, /* num_nullcols */
									   0, /* input_grouping */
									   0, /* grouping */
									   0, /* rollup_gs_times */
									   *ctx->p_dNumGroups,
									   ctx->agg_counts->numAggs,
									   ctx->agg_counts->transitionSpace,
									   "partial_aggregation",
									   &current_pathkeys,
									   result_plan,
									   !is_grpext);
	
	if (is_grpext)
		((Agg *)result_plan)->inputHasGrouping = true;
	
	/* Marshal implicit results. Return explicit result. */
	ctx->current_pathkeys = current_pathkeys;
	ctx->querynode_changed = true;
	return result_plan;
}


/*
 * Function make_two_stage_agg_plan_with_dqas
 *
 * Construct a two-stage aggregation plan involving DQAs (DISTINCT-qualified
 * aggregate functions. 
 * 
 * Such a plan will always involve three aggregation phases: 
 *
 * - preliminary -- remove duplicate (grouping key, DQA argument) values 
 *   from an arbitrarily partitioned input; pre-aggregate plain aggregate 
 *   functions.
 *
 * - intermediate -- remove duplicate (grouping key, DQA argument) values 
 *   from an input partitioned on the grouping key; pre-aggregate the
 *   pre-aggregated results of preliminary plain aggregate functions.
 *
 * - final -- apply ordinary aggregation to DQA arguments (now distinct
 *   within their group) and final aggregation to the pre-aggregated results
 *   of the previous phase.
 *
 * In addition, if there is more than one DQA in the query, the plan will
 * join the results of the individual three-phase aggregations into the
 * final result.
 */
static Plan *
make_two_stage_agg_plan_with_dqas(PlannerInfo *root, MppGroupContext *ctx)
{
	List	   *current_pathkeys;
	Plan       *result_plan;
	QualCost	tlist_cost;
	double      motion_cost_per_row;
	Path       *path = ctx->best_path; /* no use for ctx->cheapest_path */
	bool		final_sort = false;
	
	motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
				gp_motion_cost_per_row :
				2.0 * cpu_tuple_cost;

	/* We assume that we are called only when
	 * - there are no grouping extensions (like ROLLUP),
	 * - the input is partitioned and needs no preparatory Motion,
	 * - the required transformation involves DQAs.
	 */
	Assert ( !is_grouping_extension(ctx->canonical_grpsets) );
	Assert ( ctx->prep == MPP_GRP_PREP_NONE );
	Assert ( ctx->type == MPP_GRP_TYPE_GROUPED_DQA_2STAGE
			|| ctx->type == MPP_GRP_TYPE_PLAIN_DQA_2STAGE );

	/* Create the base plan which will serve as the outer plan (argument)
	 * of the partial Agg node(s).
	 */
	result_plan = create_plan(root, path);
	current_pathkeys = path->pathkeys;

	/* Instead of the flat target list produced above, use the sub_tlist
	 * constructed in cdb_grouping_planner.  Add a Result node if the
	 * base plan can't project. (This may be unnecessary, but, if so,
	 * the Result node will be removed later.)
	 */
    result_plan = plan_pushdown_tlist(result_plan, ctx->sub_tlist);

    Assert(result_plan->flow); 
    
	/* Account for the cost of evaluation of the sub_tlist. */
	cost_qual_eval(&tlist_cost, ctx->sub_tlist);
	result_plan->startup_cost += tlist_cost.startup;
	result_plan->total_cost +=
		tlist_cost.startup +
		tlist_cost.per_tuple * result_plan->plan_rows;
	
	/* Collect row count estimates and other info for the partial results. 
	 * Order the output array of DqaInfo structures (in context) according
	 * to how they should be joined.
	 */
	planDqaJoinOrder(root, ctx, result_plan->plan_rows);
	
	/* Does the context (either a sort request on our result when there
	 * is a single DQA or a local requirement to merge join coplans)
	 * enforce output sorted by (a prefix of) the grouping key?
	 */
	final_sort = ctx->numGroupCols > 0
				&& (sorting_prefixes_grouping(root) 
					|| list_length(ctx->agg_counts->dqaArgs) > 1);
	
	prepare_dqa_pruning_tlists(ctx);
	
	if ( list_length(ctx->agg_counts->dqaArgs) == 1  )
	{
		/* Note single-DQA plans don't require a join and are handled 
		 * specially by make_plan_for_one_dqa so we can return the result 
		 * directly.
		 */
		Query *query;
		
		result_plan = make_plan_for_one_dqa(root, ctx, 0, 
											result_plan, final_sort, &query);
		memcpy(root->parse, query, sizeof(Query));
		
		pfree(query);
	}
	else
	{
		/* Here's the tricky part. */
		List *share_partners;
		int i;
		List *rtable = NIL;

		share_partners = share_plan(root, result_plan, ctx->numDistinctCols);
		
		/* Construct a coplan for each distinct DQA argument. */
		for ( i = 0; i < ctx->numDistinctCols; i++ )
		{
			char buffer[50]; 
			int j;
			ListCell *l;
			Alias *eref;
			Plan *coplan;
			Query *coquery;
			
			coplan = (Plan*)list_nth(share_partners,i);
			coplan = make_plan_for_one_dqa(root, ctx, i, 
										   coplan, final_sort, &coquery);
			
			eref = makeNode(Alias);
			sprintf(buffer, "dqa_coplan_%d", i+1);
			eref->aliasname = pstrdup(buffer);
			eref->colnames = NIL;
			j = 1;
			foreach (l, coplan->targetlist)
			{
				TargetEntry *tle = (TargetEntry*)lfirst(l);
				Value *colname = get_tle_name(tle, coquery->rtable, buffer);
				eref->colnames = lappend(eref->colnames, colname);
				j++;
			}
			
			rtable = lappend(rtable,
							 package_plan_as_rte(coquery, coplan, eref, NIL));
			ctx->dqaArgs[i].coplan = add_subqueryscan(root, NULL, i+1, coquery, coplan);
		}

		/* Begin with the first coplan, then join in each suceeding coplan. */
		result_plan = ctx->dqaArgs[0].coplan;
		for ( i = 1; i < ctx->numDistinctCols; i++ )
		{
			result_plan = join_dqa_coplan(root, ctx, result_plan, i);
		}
		
		/* Finalize the last join plan so it has the correct target list
		 * and having qual. 
		 */	
		ctx->top_tlist = result_plan->targetlist;
		
		result_plan->targetlist = (List*) finalize_split_expr((Node*) ctx->fin_tlist, ctx);
		result_plan->qual = (List*) finalize_split_expr((Node*) ctx->fin_hqual, ctx);
		
		/* Need to adjust root.  Is this enuf?  I think so. */
		root->parse->rtable = rtable;
		root->parse->targetList = copyObject(result_plan->targetlist);
	}
	
	return result_plan;
}


/* Helper for qsort in planDqaJoinOrder. */
int compareDqas(const void *larg, const void *rarg)
{
	double lft = ((DqaInfo*)larg)->num_rows;
	double rgt = ((DqaInfo*)rarg)->num_rows;
	return (lft < rgt)? -1 : (lft == rgt)? 0 : 1;
}

/* Collect per distinct DQA argument information for use in single- and
 * multiple-DQA planning and cache it in the context as a new array of
 * DqaInfo structures anchored at ctx->dqaArgs.  The order of elements
 * in the array determines join order for a multi-DQA plan.
 * 
 * Note: The original list of distinct DQA arguments was collected by 
 * the count_agg_clauses earlier in planning.  Later,  make_subplan_tlist 
 * used it to guarantee that the DQA arguments have target entries with a 
 * non-zero sortgroupref values and to generate  vector ctx->distinctColIdx 
 * to locate those entries  Here, however, we use that vector to locate
 * the DQA arguments and reorder the vector to agree with join order..
 */
void planDqaJoinOrder(PlannerInfo *root, MppGroupContext *ctx, 
						   double input_rows)
{
	int i;
	DqaInfo *args;
	Node *distinctExpr;
	
	Assert( ctx->numDistinctCols == list_length(ctx->agg_counts->dqaArgs) );
	
	/* Collect row count estimates for the partial results. */
	if (  ctx->numDistinctCols == 0 ) 
	{
		ctx->dqaArgs = NULL;
		return;
	}
	
	args = (DqaInfo*)palloc( ctx->numDistinctCols * sizeof(DqaInfo));

	for ( i = 0; i <  ctx->numDistinctCols; i++)
	{
		TargetEntry *dtle;
		List *x;
		int j;
		
		/* Like PG and the SQL standard, we assume that a DQA may take only 
		* a single argument -- no REGR_SXY(DISTINCT X,Y). This is what allows 
		* distinctExpr to be an expression rather than a list of expressions.
		*/
		dtle = get_tle_by_resno(ctx->sub_tlist, ctx->distinctColIdx[i]);
		distinctExpr = (Node*) dtle->expr;
		
		x = NIL;
		for ( j = 0; j < ctx->numGroupCols ; j++ )
		{
			TargetEntry *tle;
			
			tle = get_tle_by_resno(ctx->sub_tlist,ctx->groupColIdx[j]);
			x = lappend(x, tle->expr);
		}	
		x = lappend(x, distinctExpr);
		
		args[i].distinctExpr = distinctExpr; /* no copy */
		args[i].base_index = dtle->resno;
		args[i].num_rows = estimate_num_groups(root, x, input_rows);
		args[i].can_hash = hash_safe_type(exprType(distinctExpr));
		
		list_free(x);
	}
	qsort(args, ctx->numDistinctCols, sizeof(DqaInfo), compareDqas);
	
	/* Reorder ctx->distinctColIdx to agree with join order. */
	for ( i = 0; i < ctx->numDistinctCols; i++ )
	{
		ctx->distinctColIdx[i] = args[i].base_index;
	}	
	
	ctx->dqaArgs = args;
}


/* Function make_plan_for_one_dqa
 *
 * Subroutine for make_two_stage_agg_plan_with_dqas constructs a coplan for
 * the specified DQA index [0..numDistinctCols-1] which selects a DqaInfo
 * entry from the context.
 *
 * In multi-DQA plans, coplans have minimal targetlists (just grouping
 * keys, DQA arguments, and results of single aggregate functions).  In
 * case this is a single-DQA (join-less) plan, the coplan target list is 
 * "finalized" to produce the result requested by the user (which may 
 * include expressions over the minimal list in the targetlist and/or
 * having qual).
 *
 * A Query (including range table) which approximates a query for the 
 * returned plan is stored back into *coquery_p, if coquery_p is not NULL.
 */
static Plan *
make_plan_for_one_dqa(PlannerInfo *root, MppGroupContext *ctx, int dqa_index, 
					  Plan* result_plan, bool final_sort, Query **coquery_p)
{
	List	   *prelim_tlist = NIL;
	List	   *inter_tlist = NIL;
	List	   *final_tlist = NIL;
	List	   *final_qual = NIL;
	List	   *groupExprs = NIL;
	List	   *current_pathkeys;
	QualCost	tlist_cost; 
	double      motion_cost_per_row;
	AggStrategy aggstrategy;
	AttrNumber *prelimGroupColIdx;
	AttrNumber *inputGroupColIdx;
	List	   *extendedGroupClause;
	Query	   *original_parse;
	bool        can_use_hashed_grouping = ctx->agg_counts->canHashAgg;
	bool		groups_sorted = false;
	long		numGroups;
	int i, n;
	DqaInfo *dqaArg = &ctx->dqaArgs[dqa_index];
	
	motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
				gp_motion_cost_per_row :
				2.0 * cpu_tuple_cost;
	
	/* Planning will perturb root->parse, so we copy it's content aside
	 * so we can restore it later.  We flat copy instead of resetting
	 * because code in the stack may have a local variable set to the
	 * value of root->parse.
	 */
	original_parse = makeNode(Query);
	memcpy(original_parse, root->parse, sizeof(Query));

	/* It would be possible to generate a smaller targetlist than sub_tlist 
	 * to use for this single DQA based on requirements in the dqaArg, but
	 * it is probably not worth the complexity.  We'll use sub_tlist.  
	 * The DQA argument of interest is attribute dqaArg->baseIndex.
	 */
	result_plan = plan_pushdown_tlist(result_plan, ctx->sub_tlist);
	
	/* Account for the cost of evaluation of the sub_tlist. */
	cost_qual_eval(&tlist_cost, ctx->sub_tlist);
	result_plan->startup_cost += tlist_cost.startup;
	result_plan->total_cost +=
		tlist_cost.startup +
		tlist_cost.per_tuple * result_plan->plan_rows;	

	/*
	 * Get the target lists for the preliminary, intermediate and final 
	 * aggregations and the qual (HAVING clause) for the final aggregation 
	 * based on the target list of the base plan. Grouping attributes go on 
	 * front of preliminary and intermediate target lists.
	 */
	generate_dqa_pruning_tlists(ctx, 
					dqa_index,
					&prelim_tlist,
					&inter_tlist,
					&final_tlist,
					&final_qual);
	
	/*
	 * For the first aggregation phases the original grouping attributes 
	 * (maybe zero of them) must be extended to include the DQA argument
	 * attribute (exactly one of them) to be pruned.
	 *
	 * The grouping attributes and a single DQA argument are on the front and
	 * in order on the preliminary and intermediate targetlists so we need a 
	 * new vector of grouping attributes, prelimGroupColIdx = (1, 2, 3, ...),
	 * for use in these aggregations. The vector inputGroupColIdx plays a
	 * similar role for sub_tlist.
	 *
	 * The initial-phase group clause, extendedGroupClause, is the one in
	 * the query (assumed to have no grouping extensions) augmented by a
	 * GroupClause node for the DQA argument.  This is where the sort
	 * operator for the DQA argument is selected.
	 */
	 {
		GroupClause* gc;
		TargetEntry *tle;
		 
		prelimGroupColIdx = inputGroupColIdx = NULL;
		
		n = ctx->numGroupCols + 1; /* add the DQA argument as a grouping key */
		Assert( n > 0 );
		
		prelimGroupColIdx = (AttrNumber*)palloc(n * sizeof(AttrNumber));
		inputGroupColIdx = (AttrNumber*)palloc(n * sizeof(AttrNumber));
		
		for ( i = 0; i < n; i++ )
			prelimGroupColIdx[i] = i+1;
		for ( i = 0; i < ctx->numGroupCols; i++ )
			inputGroupColIdx[i] = ctx->groupColIdx[i];
		inputGroupColIdx[ctx->numGroupCols] = dqaArg->base_index;

		gc = makeNode(GroupClause);
		tle = get_tle_by_resno(ctx->sub_tlist,  dqaArg->base_index);
		gc->tleSortGroupRef = tle->ressortgroupref;
		gc->sortop = ordering_oper_opid(exprType((Node*)dqaArg->distinctExpr));
		
		extendedGroupClause = list_copy(root->parse->groupClause);
		extendedGroupClause = lappend(extendedGroupClause,gc); 
	}
	
	/* 
	 * Determine the first-phase aggregation strategy to use.  Prefer hashing
	 * to sorting because the benefit of the sort will be lost by the Motion
	 * to follow.
	 */
	if ( dqaArg->can_hash 
		 && (ctx->type == MPP_GRP_TYPE_PLAIN_DQA_2STAGE 
			 || can_use_hashed_grouping) )
	{
		aggstrategy = AGG_HASHED;
		current_pathkeys = NIL;
	}
	else
	{
		/* Here we need to sort! The input pathkeys won't contain the
		 * DQA argument, so just do it.
		 */			
		result_plan = (Plan *)
			make_sort_from_groupcols(root,
									 extendedGroupClause,
									 inputGroupColIdx,
									 false,
									 result_plan);
		current_pathkeys = root->group_pathkeys;
		mark_sort_locus(result_plan);
		aggstrategy = AGG_SORTED;
		/* The AGG node will not change the sort ordering of its
		 * groups, so current_pathkeys describes the result too.
		 */
	}
	
	/* 
	 * Preliminary Aggregation:  With the pre-existing distribution, group
	 * by the combined grouping key and DQA argument.  In the case of the
	 * first coplan, this phase also pre-aggregates any non-DQAs.  This 
	 * eliminates duplicate values of the DQA argument on each QE.
	 */
	numGroups = (dqaArg->num_rows < 0) ? 0 :
					(dqaArg->num_rows > LONG_MAX) ? LONG_MAX :
					(long)dqaArg->num_rows;

	result_plan = (Plan *) make_agg(root,
								prelim_tlist,
								NIL, /* no havingQual */
								aggstrategy, gp_hashagg_streambottom,
								ctx->numGroupCols + 1,
								inputGroupColIdx,
								numGroups,
								0, /* num_nullcols */
								0, /* input_grouping */
								0, /* grouping */
								0, /* rollup_gs_times */
								ctx->agg_counts->numAggs - ctx->agg_counts->numDistinctAggs + 1,
								ctx->agg_counts->transitionSpace, /* worst case */
								result_plan);
	/* May lose useful locus and sort. Unlikely, but could do better. */
	mark_plan_strewn(result_plan);
	current_pathkeys = NIL;
	
	/*
	 * Intermediate Motion: Gather or Hash on Groups to get colocation
	 * on the grouping key.  Note that this may bring duplicate values
	 * of the DQA argument together on the QEs.
	 */
	switch ( ctx->type )
	{
	case MPP_GRP_TYPE_GROUPED_DQA_2STAGE:
		groupExprs = NIL;
		Assert(ctx->numGroupCols > 0);
		for ( i = 0; i < ctx->numGroupCols; i++)
		{
			TargetEntry *tle;

			tle = get_tle_by_resno(prelim_tlist, prelimGroupColIdx[i]);
			groupExprs = lappend(groupExprs, copyObject(tle->expr));
		}
		result_plan = (Plan*)make_motion_hash(root, result_plan, groupExprs);
		result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
		break;
			
	case MPP_GRP_TYPE_PLAIN_DQA_2STAGE:
		result_plan = (Plan*)make_motion_gather_to_QE(result_plan, false);
		result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
		break;
	
	case MPP_GRP_TYPE_NONE:
	case MPP_GRP_TYPE_BASEPLAN:
	case MPP_GRP_TYPE_GROUPED_2STAGE:
	case MPP_GRP_TYPE_PLAIN_2STAGE:
		ereport(ERROR,
			(errcode(ERRCODE_CDB_INTERNAL_ERROR),
			errmsg("unexpected use of DQA pruned 2-phase aggregation")));	
		break; /* Never */
	}
	current_pathkeys = NIL;
	
	/* 
	 * The context may require a result sorted by the grouping key.  There 
	 * are several strategies that produce a correctly sorted result:
	 *
	 *			SortedAgg <- SortedAgg <- Sort <- R
	 *			SortedAgg <- Sort <- HashedAgg <- R
	 *			Sort <- HashedAgg <- HashedAgg <- R
	 *
	 * Here we just choose the first (probably pretty good) by shifting in 
	 * to the code path we'd be using if PG couldn't hash on the DQA argument 
	 * type.
	 *
	 * XXX The other strategies might be better in some cases.  However the 
	 *     number of rows in the redistributed result is hard to estimate
	 *     reliably.  (It is between D and N*D where N is the number of
	 *     segments and D is  number of distinct values of the combined 
	 *     grouping key and distinct-qualified argument.)   For now this
	 *     optimization is unexplored.
	 */
	if ( aggstrategy == AGG_HASHED && final_sort )
	{
		aggstrategy = AGG_SORTED;
	}

	/*
	 * Add Sort on Groups if needed for AGG_SORTED strategy
	 */
	if (aggstrategy == AGG_SORTED)
	{
		result_plan = (Plan *)
			make_sort_from_groupcols(root,
									 extendedGroupClause,
									 prelimGroupColIdx,
									 false,
									 result_plan);
		groups_sorted = true;
		current_pathkeys = root->group_pathkeys;
		mark_sort_locus(result_plan);
	}
	
	/* Intermediate Aggregation: Grouping key values are colocated so group 
	 * by the combined grouping key and DQA argument while intermediate-
	 * aggregating any non-DQAs.  This once again (and finally) eliminates
	 * duplicate values of the DQA argument on each QE.
	 */
	result_plan = add_second_stage_agg(root,
									   true,
									   prelim_tlist,
									   inter_tlist,
									   NULL,
									   aggstrategy,
									   ctx->numGroupCols + 1,
									   prelimGroupColIdx,
									   0, /* num_nullcols */
									   0, /* input_grouping */
									   0, /* grouping */
									   0, /* rollup_gs_times */
									   dqaArg->num_rows,
									   ctx->agg_counts->numAggs,
									   ctx->agg_counts->transitionSpace,
									   "partial_aggregation",
									   &current_pathkeys,
									   result_plan, 
									   true);
	 
	 
	
	/* Revisit strategy decision for final stage. */
	if ( ctx->type == MPP_GRP_TYPE_PLAIN_DQA_2STAGE )
	{
		aggstrategy = AGG_PLAIN;
		groups_sorted = false;
	}

	/* Final Aggregation: Group by the grouping key, aggregate the now
	 * distinct values of the DQA argument using non-distinct-qualified
	 * aggregation, final aggregate the intermediate values of any non-DQAs.
	 */
	result_plan = add_second_stage_agg(root,
									   true,
									   inter_tlist,
									   final_tlist,
									   final_qual,
									   aggstrategy,
									   ctx->numGroupCols,
									   prelimGroupColIdx,
									   0, /* num_nullcols */
									   0, /* input_grouping */
									   0, /* grouping */
									   0, /* rollup_gs_times */
									   *ctx->p_dNumGroups,
									   ctx->agg_counts->numAggs,
									   ctx->agg_counts->transitionSpace,
									   "partial_aggregation",
									   &current_pathkeys,
									   result_plan, true);
	
	/* Marshal implicit results. Return explicit result. */
	if ( groups_sorted )
	{
		/* The following settings work correctly though they seem wrong.
		 * Though we changed the query tree, we say that we did not so that
		 * planner.c will notice the useful sort order we have produced.
		 * We also reset the current pathkeys to the original group keys.
		 * (Though our target list may differ, its attribute-wise ordering
		 * is on the group keys.)
		 */
		ctx->current_pathkeys = root->group_pathkeys; /* current_pathkeys are wrong! */
		ctx->querynode_changed = false;
	}
	else
	{
		ctx->current_pathkeys = NIL;
		ctx->querynode_changed = true;
	}
	
	/* If requested, copy our modified Query (at root->parse) for caller. */
	if ( coquery_p != NULL )
	{
		*coquery_p = makeNode(Query);
		memcpy(*coquery_p, root->parse, sizeof(Query));
	}

	/* Restore the original referent of root->parse. */
	memcpy(root->parse, original_parse, sizeof(Query));
	pfree(original_parse);
	
	return result_plan;
}


static Plan * 
join_dqa_coplan(PlannerInfo *root, MppGroupContext *ctx, Plan *outer, int dqa_index)
{
	Plan *join_plan = NULL;
	Plan *inner = ctx->dqaArgs[dqa_index].coplan;
	List *join_tlist = NIL;
	List *tlist = NIL;
	Index outer_varno = 1;
	Index inner_varno = dqa_index + 1;
	Index varno = 1;
	int i, ng, nd;
	
	/* Make the target list for this join.  The outer and inner target lists
	 * will look like
	 *		(<g'> <D0'> ... <Dn-1'> <F'>) and (<g'> <Dn>)
	 * or
	 *		(<g'> <D0> <F>) and (<g'> <Dn>)
	 * The join target list should look like
	 *		(<g'> <D0'> ... <Dn'> <F'>)
	 */
	/* Use varno 1 for grouping key. */
	join_tlist = make_vars_tlist(ctx->grps_tlist, varno, 0);

	ng = list_length(join_tlist); /* (<g'>) */
	nd = ng + list_length(ctx->dref_tlists[0]);/* (<g'> <D0'>) */
	
	for ( i = 0; i <= dqa_index; i++ )
	{
		tlist = make_vars_tlist(ctx->dref_tlists[i], varno+i, ng);
		join_tlist = seq_tlist_concat(join_tlist, tlist); /* (... <Di'>) */
	}
	
	tlist = make_vars_tlist(ctx->frefs_tlist, varno, nd);
	join_tlist = seq_tlist_concat(join_tlist, tlist); /* (... <F'>) */
	
	/* Make the join which will be either a cartesian product (in case of
	 * scalar aggregation) or a merge join (in case of grouped aggregation.)
	 */
	if ( ctx->numGroupCols > 0 )  /* MergeJoin: 1x1 */
	{
		List *mergeclause = NIL;
		AttrNumber attrno;
		
		/* Make the merge join clause -- a conjunction of IS NOT DISTINCT FROM
		 * predicates on the attributes of the grouping key.
		 */
		for ( attrno = 1; attrno <= ctx->numGroupCols; attrno++ )
		{
			Expr *mc;
			Var *outer_var;
			Var *inner_var;
			TargetEntry *tle = get_tle_by_resno(outer->targetlist, attrno);
			
			Assert( tle && IsA(tle->expr, Var) );
			
			outer_var = (Var*)copyObject(tle->expr);
			outer_var->varno = outer_varno;
			
			inner_var = (Var*)copyObject(tle->expr);
			inner_var->varno = inner_varno;
			
			mc = make_op(NULL, list_make1(makeString("=")), 
						 (Node*) outer_var, 
						 (Node*) inner_var, -1);
						 
			mc->type = T_DistinctExpr;
			mc = make_notclause(mc);
			mergeclause = lappend(mergeclause, mc);
		}
		
		/* Make the merge join noting that the outer plan produces rows 
		 * distinct in the join key.  (So does the inner, for that matter,
		 * but the MJ algorithm is only sensitive to the outer.)
		 */
		join_plan = (Plan*)make_mergejoin(join_tlist,
										  NIL, NIL,
										  mergeclause,
										  outer, inner,
										  JOIN_INNER);
		((MergeJoin*)join_plan)->unique_outer = true;
	}
	else /* NestLoop: Cartesian product: 1x1 */
	{
		join_plan = (Plan*)make_nestloop(join_tlist,
										 NIL, NIL,
										 outer, inner,
										 JOIN_INNER);
		((NestLoop*)join_plan)->singleton_outer = true;
	}
	
	join_plan->startup_cost = outer->startup_cost + inner->startup_cost;
	join_plan->plan_rows = outer->plan_rows;
	join_plan->plan_width = outer->plan_width + inner->plan_width; /* too high for MJ */
	join_plan->total_cost = outer->total_cost + inner->total_cost;
	join_plan->total_cost += cpu_tuple_cost * join_plan->plan_rows;
	
	join_plan->flow = pull_up_Flow(join_plan, join_plan->lefttree, true);
	
	return join_plan;
}


/*
 * Function make_subplan_tlist (for multi-phase aggregation)
 *
 * The input to the "lower" Agg node will use a "flattened" tlist (having
 * just the (levelsup==0) vars mentioned in the targetlist and HAVING qual.
 * This is similar to the target list produced by make_subplanTargetList
 * and used for 1-stage aggregation in grouping_planner.
 *
 * The result target list contains entries for all the simple Var attributes
 * of the original SELECT and HAVING clauses, plus entries for any GROUP BY
 * expressions and DQA arguments that are not simple Vars.
 *
 * The implicit results are 
 *
 * - the number of grouping attributes and a vector of their positions 
 *   (which are equal to their resno's) in the target list delivered through 
 *   pointers pnum_gkeys and pcols_gkeys, and
 *
 * - the number of distinct arguments to DISTINCT-qualified aggregate
 *   function and a vector of their positions (which are equal to their 
 *   resno's) in the target list delivered through pointers pnum_dqas and 
 *   pcols_dqas.  These arguments are guaranteed (by the call to function
 *   augment_subplan_tlist) to appear as attributes of the subplan target
 *   list.
 *
 * There are no similar results for sort and distinct attributes since 
 * they don't necessarily appear in the subplan target list.
 */
List *make_subplan_tlist(List *tlist, Node *havingQual, 
						 List *grp_clauses, 
						 int *pnum_gkeys, AttrNumber **pcols_gkeys,
						 List *dqa_args,
						 int *pnum_dqas, AttrNumber **pcols_dqas)
{
	List	   *sub_tlist;
	List	   *extravars;

	int num_gkeys;
	AttrNumber *cols_gkeys;

	Assert( dqa_args != NIL? pnum_dqas != NULL && pcols_dqas != NULL: true );
	
	sub_tlist = flatten_tlist(tlist);
	extravars = pull_var_clause(havingQual, false);
	sub_tlist = add_to_flat_tlist(sub_tlist, extravars);
	list_free(extravars);
	
	num_gkeys = num_distcols_in_grouplist(grp_clauses);
	if (num_gkeys > 0)
	{
		int			keyno = 0;
		ListCell   *l;
		List       *tles;

		cols_gkeys = (AttrNumber*) palloc(sizeof(AttrNumber) * num_gkeys);

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
			cols_gkeys[keyno++] = sub_tle->resno;
		}
		*pnum_gkeys = num_gkeys;
		*pcols_gkeys = cols_gkeys;
	}
	else
	{
		*pnum_gkeys = 0;
		*pcols_gkeys = NULL;
	}
	
	if ( dqa_args != NIL )
		sub_tlist = augment_subplan_tlist(sub_tlist, dqa_args, pnum_dqas, pcols_dqas);
		
	return sub_tlist; /* Possibly modified by appending expression entries. */
}


/* Function augment_subplan_tlist
 *
 * Called from make_subplan_tlist, not directly.
 *
 * Make a target list like the input that includes sortgroupref'd entries
 * for the expressions in exprs.  Note that the entries in the input expression
 * list must be distinct.
 *
 * New entries corresponding to the expressions in the input exprs list 
 * (if any) are added to the argument list.  Existing entries are modified 
 * (if necessary) in place.
 *
 * Return the (modified) input targetlist.
 * 
 * Implicitly return an array of resno values for exprs in (pnum, *pcols).
 */
List *augment_subplan_tlist(List *tlist, List *exprs, int *pnum, AttrNumber **pcols)
{
	int num;
	AttrNumber *cols;
	
	num = list_length(exprs); /* Known to be distinct. */
	if (num > 0)
	{
		int			keyno = 0;
		ListCell   *lx, *lt;
		TargetEntry *tle, *matched_tle;
		Index max_sortgroupref = 0;
		
		foreach (lt, tlist)
		{
			tle = (TargetEntry*)lfirst(lt);
			if ( tle->ressortgroupref > max_sortgroupref )
				max_sortgroupref = tle->ressortgroupref;
		}

		cols = (AttrNumber*) palloc(sizeof(AttrNumber) * num);

		foreach (lx, exprs)
		{
			Node *expr = (Node*)lfirst(lx);
			matched_tle = NULL;
			
			foreach (lt, tlist)
			{
				tle = (TargetEntry*)lfirst(lt);
				
				if ( equal(expr, tle->expr) )
				{
					matched_tle = tle;
					break;
				}
			}

			if ( matched_tle == NULL )
			{
				matched_tle = makeTargetEntry((Expr*) expr,
											  list_length(tlist) + 1,
											  NULL,
											  false);
				tlist = lappend(tlist, matched_tle);
			}
			
			if ( matched_tle->ressortgroupref == 0 )
				matched_tle->ressortgroupref = ++max_sortgroupref;
	
			cols[keyno++] = matched_tle->resno;
		}
	
		*pnum = num;
		*pcols = cols;
	}
	else
	{
		*pnum = 0;
		*pcols = NULL;
	}
	
	/* Note that result is a copy, possibly modified by appending expression 
	 * targetlist entries and/or updating sortgroupref values. 
	 */
	return tlist; 
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
 *
 * This function also returns a map between the original targetlist
 * entry to new target list entry using resno values. The index
 * positions for resno_map represent the original resnos, while the
 * array elements represent the new resnos. The array is allocated
 * by the caller, which should have length of list_length(input_tlist).
 */
List *
generate_subquery_tlist(Index varno, List *input_tlist, int **p_resno_map)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *j;
	TargetEntry *tle;
	Node	   *expr;

	*p_resno_map = (int *)palloc0(list_length(input_tlist) * sizeof(int));

	foreach(j, input_tlist)
	{
		TargetEntry *inputtle = (TargetEntry *) lfirst(j);

		Assert(inputtle->resno == resno && inputtle->resno >= 1);

		/* Don't pull up constants, always use a Var to reference the input. */
		expr = (Node *) makeVar(varno,
								inputtle->resno,
								exprType((Node *) inputtle->expr),
								exprTypmod((Node *) inputtle->expr),
								0);

		(*p_resno_map)[inputtle->resno - 1] = resno;

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
        case FLOW_UNDEFINED:
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
 *
 * NB This function is called externally (from plangroupext.c) and not
 * used in this file!  Beware: the API is now legacy here!
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
						   List **p_final_qual)
{
	ListCell *lc;
	int resno = 1;
	
	MppGroupContext ctx; /* Just for API matching! */

	/* Similar to the final tlist entries in two-stage aggregation,
	 * we use consistent varno in the middle tlist entries.
	 */
	int middle_varno = 1;

	/* Generate the top and bottom tlists by calling the multi-phase
	 * aggregation code in cdbgroup.c. 
	 */
	ctx.tlist = tlist;
	ctx.sub_tlist = sub_tlist;
	ctx.havingQual = havingQual;
	ctx.numGroupCols = numGroupCols;
	ctx.groupColIdx = groupColIdx;
	ctx.numDistinctCols = 0;
	ctx.distinctColIdx = NULL;
	
	generate_multi_stage_tlists(&ctx,
								p_tlist1,
								 NULL,
								 p_tlist3,
								  p_final_qual);

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
			new_aggref->aggstage = AGGSTAGE_INTERMEDIATE;

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
		new_tle->ressortgroupref = tle->ressortgroupref;
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

		/* We also set aggstage to AGGSTAGE_INTERMEDIATE if this is in
		 * a two-stage aggregation, because the agg node in
		 * the second stage aggregation will do the finalize.
		 */
		if (twostage && IsA(tle->expr, Aggref))
		{
			Aggref *aggref = (Aggref *)tle->expr;
			aggref->aggstage = AGGSTAGE_INTERMEDIATE;
		}
	}
}


/*
 * Function: generate_multi_stage_tlists
 *
 * Generate target lists and having qual for multi-stage aggregation.
 *
 * Input is
 *
 * From the ctx argument:
 *	 tlist - the preprocessed target list of the original query
 *	 sub_tlist - the reduced target list to use as input to the aggregation
 *               (If use_dqa_pruning, the all DQA arguments must appear in
 *               this list and must have non-zero sortgrouprefs.)
 *	 havingQual - the preprocesses having qual of the originaly query 
 *                (in list-of-conjunct-Exprs form)
 *	 numGroupCols - number of grouping attributes (no grouping extensions)
 *	 groupColIdx - resnos (= attr numbers) of the grouping attributes
 *	 numDistinctCols - number of DISTINCT-qualified argument attributes
 *	 distinctColIdx - resnos (= attr numbers) of the DQA argument attributes
 *
 * From use_dqa_pruning:
 *   Do we want to construct the tlists to support pruning DQAs?
 *
 * Output is pointers to
 *
 *	prelim_tlist - the target list of the preliminary Agg node.
 *  inter_tlist - an optional intermediate target list for an Agg node
 *                used in multi-phase DQA pruning (p_inter_tlist non-null).
 *	final_tlist - the target list of the final Agg node.
 *	final_qual - the qual of the final Agg node.
 */
void generate_multi_stage_tlists(MppGroupContext *ctx,
						List **p_prelim_tlist,
						List **p_inter_tlist,
						List **p_final_tlist,
						List **p_final_qual)
{
	/* Use consistent varno in final and intermediate tlist entries.  It will 
	 * refer to the sole RTE (a Subquery RTE) of a SubqueryScan. */
	ctx->final_varno = 1;
	
	/* Do we need to build an intermediate tlist in irefs_tlist? */
	ctx->use_irefs_tlist = ( p_inter_tlist != NULL );
	
	/* Don't do DQA pruning.  Use prepare/generate_dqa_pruning_tlists! */
	ctx->use_dqa_pruning = false;
		
	deconstruct_agg_info(ctx);	
	reconstruct_agg_info(ctx, 
						 p_prelim_tlist, p_inter_tlist, 
						 p_final_tlist, p_final_qual);
}


/*
 * Function: prepare_dqa_pruning_tlists
 *
 * Performs the first phase of generate_multi_phase_tlist, but waits for
 * a subquent call to generate_dqa_pruning_tlists to actually produce the
 * target list.  (This allows for the fact that DQA pruning may involve
 * several "coplans" each with its own target list requirements.  This
 * function lays the groundwork for all such target lists.
 */
void prepare_dqa_pruning_tlists(MppGroupContext *ctx)
{
	/* Use consistent varno in final and intermediate tlist entries.  It will 
	 * refer to the sole RTE (a Subquery RTE) of a SubqueryScan. */
	ctx->final_varno = 1;
	
	/* Do we need to build an intermediate tlist in irefs_tlist? */
	ctx->use_irefs_tlist = true;
	
	/* Do we want to do DQA pruning (in case there are any DISTINCT-qualified
	 * aggregate functions)? */
	ctx->use_dqa_pruning = true;
	
	deconstruct_agg_info(ctx);	
}

/*
 * Function: generate_dqa_pruning_tlists
 *
 * Performs the last phase of generate_multi_phase_tlist in the context of 
 * DQA pruning.
 */
void generate_dqa_pruning_tlists(MppGroupContext *ctx,
						int dqa_index,
						List **p_prelim_tlist,
						List **p_inter_tlist,
						List **p_final_tlist,
						List **p_final_qual)
{
	Assert( p_inter_tlist != NULL ); /* optional elsewhere, required here. */
	Assert( ctx->use_dqa_pruning );
	
	if ( ctx->numDistinctCols == 1 )
	{
		/* Finalized results for single-DQA (join-less) plan. */
		reconstruct_agg_info(ctx, 
							 p_prelim_tlist, 
							 p_inter_tlist,
							 p_final_tlist, 
							 p_final_qual);
	}
	else
	{
		/* Minimal results for multi-DQA (join) plan. */
		reconstruct_coplan_info(ctx, 
								dqa_index,
								p_prelim_tlist,
								p_inter_tlist,
								p_final_tlist);
		*p_final_qual = NIL;
	}
}

/* Function: deconstruct_agg_info
 *
 * Top-level deconstruction of the target list and having qual of an
 * aggregate Query into intermediate structures that will later guide
 * reconstruction of the various target lists and expressions involved
 * in a multi-phase aggregation plan, possibly with DISTINCT-qualified
 * aggregate functions (DQAs).
 */
void deconstruct_agg_info(MppGroupContext *ctx)
{
	int i;
	ListCell *lc;
	
	/* Initialize temporaries to hold the parts of the preliminary target
	 * list under construction. */
	ctx->grps_tlist = NIL;
	ctx->dqa_tlist = NIL;
	ctx->prefs_tlist = NIL;
	ctx->irefs_tlist = NIL;
	ctx->frefs_tlist = NIL;
	ctx->dref_tlists = NULL;
	ctx->fin_tlist = NIL;
	ctx->fin_hqual = NIL;
	
	/*
	 * Begin constructing the target list for the preliminary Agg node
	 * by placing targets for the grouping attributes on the grps_tlist
	 * temporary. Make sure ressortgroupref matches the original. Copying
	 * the expression may be overkill, but it is safe.
	 */
	for ( i = 0; i < ctx->numGroupCols; i++ )
	{
		TargetEntry *sub_tle, *prelim_tle;
		
		sub_tle = get_tle_by_resno(ctx->sub_tlist, ctx->groupColIdx[i]);
		prelim_tle = makeTargetEntry(copyObject(sub_tle->expr),
									list_length(ctx->grps_tlist) + 1,
									(sub_tle->resname == NULL) ?
											NULL :
											pstrdup(sub_tle->resname),
									false);
		prelim_tle->ressortgroupref = sub_tle->ressortgroupref;
		prelim_tle->resjunk = false;
		ctx->grps_tlist = lappend(ctx->grps_tlist, prelim_tle);
	}

	/*
	 * Continue to construct the target list for the preliminary Agg node
	 * by placing targets for the argument attribute of each DQA on the 
	 * dqa_tlist temporary. Make sure ressortgroupref matches the original.
	 */
	for ( i = 0; i < ctx->numDistinctCols; i++ )
	{
		TargetEntry *sub_tle, *prelim_tle;
		
		sub_tle = get_tle_by_resno(ctx->sub_tlist, ctx->distinctColIdx[i]);
		prelim_tle = makeTargetEntry(copyObject(sub_tle->expr),
									list_length(ctx->dqa_tlist) + 1,
									(sub_tle->resname == NULL) ?
											NULL :
											pstrdup(sub_tle->resname),
									false);
		prelim_tle->ressortgroupref = sub_tle->ressortgroupref;
		prelim_tle->resjunk = false;
		ctx->dqa_tlist = lappend(ctx->dqa_tlist, prelim_tle);
	}
	
	/* Initialize the array of Aggref target lists corresponding to the 
	 * DQA argument target list just constructed.
	 */
	ctx->dref_tlists = (List **)palloc0(ctx->numDistinctCols * sizeof(List*));
	
	/*
	 * Derive the final target list with entries corresponding to the input
	 * target list, but referring to the attributes of the preliminary target
	 * list rather than to the input attributes.  Note that this involves
	 * augmenting the prefs_tlist temporary as we encounter new Aggref nodes.
	 */
	foreach (lc, ctx->tlist)
	{
		TargetEntry *tle, *final_tle;
		Expr *expr;
		
		tle = (TargetEntry*)lfirst(lc);
		expr = deconstruct_expr(tle->expr, ctx);
		final_tle = makeTargetEntry(expr,
									tle->resno,
									(tle->resname == NULL) ?
										NULL :
										pstrdup(tle->resname),
									tle->resjunk);
		final_tle->ressortgroupref = tle->ressortgroupref;
		ctx->fin_tlist = lappend(ctx->fin_tlist, final_tle);
	}
	
	/*
	 * Derive the final qual while augmenting the preliminary target list. */
	ctx->fin_hqual = (List*)deconstruct_expr((Expr*)ctx->havingQual, ctx);
	
	
	/* Now cache some values to avoid repeated recalculation by subroutines. */
	
	/* Use consistent varno in final, intermediate an join tlist entries.  
	 * final refers to the sole RTE (a Subquery RTE) of a SubqueryScan. 
	 * outer and inner to the respective inputs to a join.
	 */
	ctx->final_varno = 1;
	ctx->outer_varno = OUTER;
	ctx->inner_varno = INNER;
	
	/* Target lists used in multi-phase planning at or above the level
	 * of individual DQA coplans have one of the forms
	 *
	 *   [G][D0...Dn][R]
	 * where
	 *   G represents the grouping key attributes (if any)
	 *   Di represents the results of the DQAs that take the i-th
	 *      unique DQA argument (if any)
	 *   R represents the results of regular aggregate functions (if any)
	 *
	 * The offset at index position i is the number of attributes that
	 * precede the first for the i-th DQA (index origin 0).  The last
	 * is the offset of the first attribute following the DQA attributes.
	 */
	ctx->dqa_offsets = palloc(sizeof(int) * (1 + ctx->numDistinctCols));
	ctx->dqa_offsets[0] = ctx->numGroupCols;
	for ( i = 0; i < ctx->numDistinctCols; i++ )
	{
		ctx->dqa_offsets[i+1] = ctx->dqa_offsets[i] 
							  + list_length(ctx->dref_tlists[i]);
	}
}

/* Function: reconstruct_agg_info
 *
 * Construct the preliminay, optional intermediate, and final target lists
 * and the final having qual for the aggregation plan.  If we are doing
 * DQA pruning, this function is appropriate only for the cases of 0 or 1
 * DQA.
 *
 * During processing we set ctx->top_tlist to be the flat target list 
 * containing only the grouping key and the results of individual aggregate
 * functions.  This list is transient -- it drives the production of the
 * final target list and having qual through finalize_split_expression. 
 */
void reconstruct_agg_info(MppGroupContext *ctx, 						
						List **p_prelim_tlist,
						List **p_inter_tlist,
						List **p_final_tlist,
						List **p_final_qual)
{	
	List *prelim_tlist = NIL;
	List *inter_tlist = NIL;
	List *final_tlist = NIL;
	
	/* Grouping keys */
	
	prelim_tlist = ctx->grps_tlist;
	if ( p_inter_tlist != NULL )
		inter_tlist = make_vars_tlist(ctx->grps_tlist, ctx->final_varno, 0);
	final_tlist =  make_vars_tlist(ctx->grps_tlist, ctx->final_varno, 0);
	
	/* If applicable, single DQA argument, corresponding DQAs */
	
	if ( ctx->use_dqa_pruning )
	{
		if ( list_length(ctx->dqa_tlist) == 1 )
		{
			int n = list_length(prelim_tlist);
			TargetEntry *tle = (TargetEntry*)linitial(ctx->dqa_tlist);
			tle->resno = n+1;
			
			prelim_tlist = lappend(prelim_tlist, tle);
			if ( p_inter_tlist != NULL )
			{
				inter_tlist = list_concat(inter_tlist,
										  make_vars_tlist(ctx->dqa_tlist, 
														  ctx->final_varno, n));
			}
			final_tlist = seq_tlist_concat(final_tlist, ctx->dref_tlists[0]);
		}
		else if ( list_length(ctx->dqa_tlist) != 0 )
		{
			/* Shouldn't use this function for multi-DQA pruning. */
			elog(ERROR,"Unexpected use of DISTINCT-qualified aggregate pruning");
		}
	}

	/* Aggrefs */
	
	prelim_tlist = seq_tlist_concat(prelim_tlist, ctx->prefs_tlist);
	if ( p_inter_tlist != NULL )
	{
		inter_tlist = seq_tlist_concat(inter_tlist, ctx->irefs_tlist);
	}
	final_tlist = seq_tlist_concat(final_tlist, ctx->frefs_tlist);

	/* Set implicit results */
	
	*p_prelim_tlist = prelim_tlist;
	if ( p_inter_tlist != NULL ) 
		*p_inter_tlist = inter_tlist;

	ctx->top_tlist = final_tlist;

	*p_final_tlist = (List*) finalize_split_expr((Node*) ctx->fin_tlist, ctx);
	*p_final_qual = (List*) finalize_split_expr((Node*) ctx->fin_hqual, ctx);
}

/* Function: reconstruct_coplan_info
 *
 * Construct the preliminary, intermediate, and final target lists
 * for the DQA pruning aggregation coplan specified by dqa_index.
 *
 * Note: Similar to reconstruct_agg_info but stop short of finalization
 *       and is sensitive to dqa_index.  Ordinarily this function would
 *       be used only for multiple-DQA planning.
 */
void reconstruct_coplan_info(MppGroupContext *ctx, 
							 int dqa_index,
							 List **p_prelim_tlist,
							 List **p_inter_tlist,
							 List **p_final_tlist)
{	
	List *prelim_tlist = NIL;
	List *inter_tlist = NIL;
	List *final_tlist = NIL;

	int n;
	TargetEntry *tle;
	 
	/* Grouping keys */
	
	prelim_tlist = copyObject(ctx->grps_tlist);
	if ( p_inter_tlist != NULL )
		inter_tlist = make_vars_tlist(ctx->grps_tlist, ctx->final_varno, 0);
	final_tlist =  make_vars_tlist(ctx->grps_tlist, ctx->final_varno, 0);
	
	/* Single DQA argument, corresponding DQAs */
	
	Assert ( ctx->use_dqa_pruning );

	n = list_length(prelim_tlist);
	tle = (TargetEntry*)list_nth(ctx->dqa_tlist, dqa_index);
	tle->resno = n+1;
	
	prelim_tlist = lappend(prelim_tlist, tle);
	if ( p_inter_tlist != NULL )
	{
		List *x = list_make1(tle);
		inter_tlist = list_concat(inter_tlist,
								  make_vars_tlist(x, ctx->final_varno, n));
		list_free(x);
	}
	final_tlist = seq_tlist_concat(final_tlist, ctx->dref_tlists[dqa_index]);


	/* Plain Aggrefs go only on the first coplan! */
	if ( dqa_index == 0 )
	{
		prelim_tlist = seq_tlist_concat(prelim_tlist, ctx->prefs_tlist);
		if ( p_inter_tlist != NULL )
		{
			inter_tlist = seq_tlist_concat(inter_tlist, ctx->irefs_tlist);
		}
		final_tlist = seq_tlist_concat(final_tlist, ctx->frefs_tlist);
	}

	/* Set implicit results */
	
	*p_prelim_tlist = prelim_tlist;
	if ( p_inter_tlist != NULL ) 
	{
		*p_inter_tlist = inter_tlist;
	}
	*p_final_tlist = final_tlist;
}


/*
 * Function: deconstruct_expr
 *
 * Prepare an expression for execution within 2-stage aggregation.
 * This involves adding targets as needed to the target list of the
 * first (partial) aggregation and referring to this target list from
 * the modified expression for use in the second (final) aggregation.
 */
Expr *deconstruct_expr(Expr *expr, MppGroupContext *ctx)
{
	return (Expr*)deconstruct_expr_mutator((Node*)expr, ctx);
}

/*
 * Function: deconstruct_expr_mutator
 *
 * Work for deconstruct_expr.
 */
Node* deconstruct_expr_mutator(Node *node, MppGroupContext *ctx)
{
	TargetEntry *tle;
	
	if (node == NULL)
		return NULL;
		
	if (IsA(node, Aggref))
	{
		Aggref *aggref = (Aggref*)node;
		return split_aggref(aggref, ctx);
	}
	
	/* If the given expression is a grouping expression, replace it with
	 * a Var node referring to the (lower) preliminary aggregation's
	 * target list.
	 */
	tle = tlist_member(node, ctx->grps_tlist);
	if ( tle != NULL )
	{
		return  (Node*) makeVar(grp_varno, tle->resno,
								exprType((Node*)tle->expr),
								exprTypmod((Node*)tle->expr), 0);
	}

	return expression_tree_mutator(node, deconstruct_expr_mutator, (void*)ctx);
}


/*
 * Function: split_aggref
 *
 * Find or add a partial-stage Aggref expression for the argument in the
 * preliminary target list under construction.  Return the final-stage
 * Aggref with a single Var node argument referring to the partial-stage
 * Aggref.  In case of a DQA argument reduction, however, there is no 
 * partial-stage Aggref and the final-stage Aggref differs from the original
 * in that (1) it does not specify DISTINCT and (2) it refers to its arguments 
 * via Vars on the lower range.
 *
 * For the normal 2-phase case: 
 *
 * Note that he result type of the partial-stage Aggref will be the
 * transition type of the aggregate function.
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
Node *split_aggref(Aggref *aggref, MppGroupContext *ctx)
{
	ListCell *cell;
	Node *final_node;
	Oid transtype = InvalidOid;
	AttrNumber attrno = OUTER;
	TargetEntry *prelim_tle = NULL;

	Assert(aggref != NULL  && aggref->agglevelsup == 0);
	
	if ( aggref->aggdistinct && ctx->use_dqa_pruning )
	{
		Index arg_attno;
		Index dqa_attno;
		TargetEntry *dqa_tle = NULL;
		TargetEntry *arg_tle;
		List *dref_tlist = NIL;

		/* First find the DQA argument.  Since this is a DQA, its argument
		 * list must contain a single expression that matches one of the
		 * target expressions in ctx->dqa_tlist.
		 */
		arg_tle = NULL;
		if ( list_length(aggref->args) == 1 ) /* safer than Assert */
		{
			arg_tle = tlist_member(linitial(aggref->args), ctx->dqa_tlist);
		}
		if (arg_tle == NULL)
			elog(ERROR,"Unexpected use of DISTINCT-qualified aggregation");
		arg_attno = arg_tle->resno; /* [1..numDistinctCols] */

		/* We may have seen a DQA just like this one already.  Look for
		 * one in the distinct Aggref target list to date.
		 */
		dref_tlist = ctx->dref_tlists[arg_attno - 1];
		dqa_attno = 1;
		foreach( cell, dref_tlist )
		{
			TargetEntry *tle = (TargetEntry*)lfirst(cell);
			Aggref *ref = (Aggref*)tle->expr;
			
			/* Check similarity, avoid aggtype and aggstage 
			 * (which we control) and don't bother with agglevelsup 
			 * (which is always 0 here) or aggdistinct.
			 */
			if ( aggref->aggfnoid == ref->aggfnoid
				&& aggref->aggstar == ref->aggstar
				&& equal(aggref->args, ref->args) )
			{
				dqa_tle = tle;
				break;
			}
			dqa_attno++;
		}
		
		if ( dqa_tle == NULL )
		{
			/* Didn't find a target for the DQA Aggref so make a new one.  
			 */
			Var *arg_var;
			Aggref *dqa_aggref;
			
			arg_var = makeVar(ctx->final_varno, ctx->numGroupCols + 1,
							  exprType(linitial(aggref->args)),
							  exprTypmod(linitial(aggref->args)),
							  0);
			
			dqa_aggref = makeNode(Aggref);
			memcpy(dqa_aggref, aggref, sizeof(Aggref)); /* flat copy */
			dqa_aggref->args = list_make1(arg_var);
			dqa_aggref->aggdistinct = false;
			
			dqa_tle = makeTargetEntry((Expr*)dqa_aggref, dqa_attno, NULL, false);
			dref_tlist = lappend(dref_tlist, dqa_tle);
		}
		ctx->dref_tlists[arg_attno-1] = dref_tlist;

		/* Make the "final" target for the DQA case, a reference to the 
		 * DQA Aggref we just found or constructed.
		 */
		final_node = (Node*) makeVar(dqa_base_varno + arg_attno - 1, 
									 dqa_attno,
									 exprType((Node*)arg_tle->expr),
									 exprTypmod((Node*)arg_tle->expr),
									 0);
	}
	else /* Ordinary Aggref -or- DQA but ctx->use_dqa_pruning is off. */
	{
		Aggref *pref;
		Aggref *iref;
		Aggref *fref;
		
		/*
		 * We may have seen an Aggref just like this one already.  Look for
		 * the preliminary form of such in the preliminary Aggref target
		 * list to date.
		 */
		foreach( cell, ctx->prefs_tlist )
		{
			TargetEntry *tle = (TargetEntry*)lfirst(cell);
			Aggref *ref = (Aggref*)tle->expr;
			
			/* Check similarity, avoid aggtype and aggstage 
			 * (which we control) and don't bother with agglevelsup 
			 * (which is always 0 here).
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
			TargetEntry *final_tle;
			Var *args;
			
			/* Get type information for the Aggref */
			transtype = lookup_agg_transtype(aggref);
			
			/* Make a new preliminary Aggref wrapped as a new target entry. 
			 * Like the input Aggref, the preliminary refers to the lower
			 * range. */		
			pref = (Aggref*)copyObject(aggref);
			pref->aggtype = transtype;
			pref->aggstage = AGGSTAGE_PARTIAL;

			attrno = 1 + list_length(ctx->prefs_tlist);
			prelim_tle = makeTargetEntry((Expr*)pref, attrno, NULL, false);
			ctx->prefs_tlist = lappend(ctx->prefs_tlist, prelim_tle);
			
			args = makeVar(ctx->final_varno, 
						  ctx->numGroupCols 
						  + (ctx->use_dqa_pruning ? 1 : 0) 
						  + attrno, 
						  transtype, -1, 0);
			
			if ( ctx->use_irefs_tlist )
			{
				TargetEntry *inter_tle;
				
				iref = makeNode(Aggref);
				iref->aggfnoid = pref->aggfnoid;
				iref->aggtype = transtype;
				iref->args = list_make1((Expr*)copyObject(args));
				iref->agglevelsup = 0;
				iref->aggstar = false;
				iref->aggdistinct = false;
				iref->aggstage = AGGSTAGE_INTERMEDIATE;
				
				inter_tle =  makeTargetEntry((Expr*)iref, attrno, NULL, false);
				ctx->irefs_tlist = lappend(ctx->irefs_tlist, inter_tle);
			}

			/* Make a new final Aggref. */
			fref = makeNode(Aggref);
			
			fref->aggfnoid = aggref->aggfnoid;
			fref->aggtype = aggref->aggtype;
			fref->args = list_make1((Expr*)args);
			fref->agglevelsup = 0;
			fref->aggstar = false;
			fref->aggdistinct = false; /* handled in preliminary aggregation */
			fref->aggstage = AGGSTAGE_FINAL;
			final_tle = makeTargetEntry((Expr*)fref, attrno, NULL, false);
			ctx->frefs_tlist = lappend(ctx->frefs_tlist, final_tle);
		}

		final_node = (Node*)makeVar(ref_varno, attrno, aggref->aggtype, -1, 0);
	}
	
	return final_node;
}


/* Function: make_vars_tlist
 *
 * Make a targetlist similar to the given length n tlist but consisting of
 * simple Var nodes with the given varno and varattno in offset + [1..N].
 */
List *make_vars_tlist(List *tlist, Index varno, AttrNumber offset)
{
	List *new_tlist = NIL;
	AttrNumber attno = offset;
	ListCell *lc;
	
	foreach (lc, tlist)
	{
		Var *new_var;
		TargetEntry *new_tle;

		TargetEntry *tle = (TargetEntry*)lfirst(lc);

		attno++;
		
		new_var = makeVar(varno, attno,
						  exprType((Node*)tle->expr),
						  exprTypmod((Node*)tle->expr), 0);
		
		new_tle = makeTargetEntry((Expr*)new_var, 
				  				  attno,  /* resno always matches attnr */ 
				  				  (tle->resname == NULL) ? NULL : pstrdup(tle->resname),
				  				  false);
		new_tle->ressortgroupref = tle->ressortgroupref;
		
		new_tlist = lappend(new_tlist, new_tle);
	}
	return new_tlist;
}

/* Function: seq_tlist_concat
 *
 * Concatenates tlist2 to the end of tlist1 adjusting the resno values
 * of tlist2 so that the resulting entries have resno = position+1.
 * The resno values of tlist1 must be dense from 1 to the length of 
 * the list.  (They are sequential by position, though this is not
 * strictly required.
 * 
 * May modify tlist1 in place (to adjust last link and length).  Does not
 * modified tlist2, but the result shares structure below the TargetEntry
 * nodes.
 */
List *seq_tlist_concat(List *tlist1, List *tlist2)
{
	ListCell *lc;
	AttrNumber high_attno = list_length(tlist1);
	
	foreach (lc, tlist2)
	{
		TargetEntry *tle= (TargetEntry*)lfirst(lc);
		TargetEntry *new_tle = (TargetEntry*)makeNode(TargetEntry);
		memcpy(new_tle, tle, sizeof(TargetEntry));
		new_tle->resno = ++high_attno;
		tlist1 = lappend(tlist1, new_tle);
	}
	return tlist1;
}

/* Function finalize_split_expr 
 *
 * Note: Only called on the top of the "join" tree, so all D_i are
 *       included in attribute offset calculations.
 */
Node *finalize_split_expr(Node *expr, MppGroupContext *ctx) 
{
	return finalize_split_expr_mutator(expr, ctx);
}

/* Mutator subroutine for finalize_split_expr() replaces pseudo Var nodes
 * produced by split_aggref() with the similarly typed expression found in
 * the top-level targetlist, ctx->top_tlist,  being finalized. 
 *
 * For example, a pseudo Var node that represents the 3rd DQA for the 
 * 2nd DQA argument will be replaced by the targetlist expression that
 * corresponds to that DQA.  
 */
Node* finalize_split_expr_mutator(Node *node, MppGroupContext *ctx)
{
	if (node == NULL)
		return NULL;
		
	if (IsA(node, Var))
	{
		AttrNumber attrno=(AttrNumber)NULL;
		TargetEntry *tle;
		
		Var *pseudoVar = (Var*)node;
		
		if ( pseudoVar->varno == grp_varno )
		{			
			attrno = pseudoVar->varattno;
		}
		else if ( pseudoVar->varno == ref_varno )
		{
			if ( ctx->use_dqa_pruning )
			{
				attrno = ctx->dqa_offsets[ctx->numDistinctCols]
						 + pseudoVar->varattno;
			}
			else
			{
				attrno = ctx->numGroupCols + pseudoVar->varattno;
			}
		}
		else if ( pseudoVar->varno >= dqa_base_varno && ctx->use_dqa_pruning )
		{
			int i = pseudoVar->varno - dqa_base_varno;
			attrno = ctx->dqa_offsets[i] + pseudoVar->varattno;
		}
		else
		{
			elog(ERROR,"Unexpected failure of multi-phase aggregation planning");
		}
		
		tle = (TargetEntry*) list_nth(ctx->top_tlist, attrno - 1);
			
		return (Node*) tle->expr;
	}
	
	return expression_tree_mutator(node, 
								   finalize_split_expr_mutator,
								   (void*)ctx);
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
	flow = plan->flow;

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
 * Add an Agg/Group node on top of an Agg/Group node. We add a SubqueryScan
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
					 List *lower_tlist,
					 List *upper_tlist,
					 List *upper_qual,
					 AggStrategy aggstrategy,
					 int numGroupCols,
					 AttrNumber *prelimGroupColIdx,
					 int num_nullcols,
					 uint64 input_grouping,
					 uint64 grouping,
					 int rollup_gs_times,
					 double numGroups,
					 int numAggs,
					 int transSpace,
					 const char *alias,
					 List **p_current_pathkeys,
					 Plan *result_plan,
					 bool use_root)
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
	 * The result of the preliminary aggregation (represented by lower_tlist)
	 * may contain targets with no representatives in the range of its outer 
	 * relation.  We resolve this be treating the preliminary aggregation as 
	 * a subquery.
	 *
	 * However, this breaks the correspondence between the Plan tree and
	 * the Query tree that is assumed by the later call to set_plan_references
	 * as well as by the deparse processing used (e.g.) in EXPLAIN.
	 * 
	 * So we also push the Query node from the root structure down into a new
	 * subquery RTE and scribble over the original Query node to make it into
	 * a simple SELECT * FROM a Subquery RTE.
	 *
	 * Note that the Agg phase we add below will refer to the attributes of 
	 * the result of this new SubqueryScan plan node.  It is up to the caller 
	 * to set up upper_tlist and upper_qual accordingly.
	 */
	 		
	/* Flat-copy the root query into a newly allocated Query node and adjust
	 * its target list and having qual to match the lower (existing) Agg
	 * plan we're about to make into a SubqueryScan.
	 */
	subquery = makeNode(Query);
	memcpy(subquery, parse, sizeof(Query));
	
	subquery->targetList = copyObject(lower_tlist);
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
				if ( use_root && IsA(tle->expr, Var) )
				{
					Var *var = (Var*)tle->expr;
					RangeTblEntry *rte = rt_fetch(var->varno, root->parse->rtable);
					tle->resname = pstrdup(get_rte_attribute_name(rte, var->varattno));
				}
				else
				{
					const char *fmt = "unnamed_attr_%d";
					char buf[32]; /* big enuf for fmt */
					sprintf(buf,fmt,tle->resno);
					tle->resname = pstrdup(buf);
				}
			}
		}
	}
	
	/* Construct a range table entry referring to it. */
	newrte = addRangeTableEntryForSubquery(NULL,
										   subquery,
										   makeAlias(alias, NULL),
										   TRUE);
	newrtable = list_make1(newrte);

	/* Modify the root query in place to look like its range table is
	* a simple Subquery. */
	parse->querySource = QSRC_PLANNER; /* but remember it's really ours  */
	parse->rtable = newrtable;
	parse->jointree = makeNode(FromExpr);
	newrtref = makeNode(RangeTblRef);
	newrtref->rtindex = 1;
	parse->jointree->fromlist = list_make1(newrtref);
	parse->jointree->quals = NULL;
	parse->rowMarks = NIL;

	/* <EXECUTE s> uses parse->targetList to derive the portal's tupDesc,
	 * so when use_root is true, the caller owns the responsibility to make 
	 * sure it ends up in an appropriate form at the end of planning.
	 */
	if ( use_root )
	{
		parse->targetList = copyObject(upper_tlist); /* Match range. */
	}

	result_plan = add_subqueryscan(root, p_current_pathkeys, 
								   1, subquery, result_plan);

	/* Add an Agg/Group node */
	if (is_agg ||
		aggstrategy == AGG_HASHED)
	{
		/* convert current_numGroups to long int */
		long lNumGroups = (long) Min(numGroups, (double) LONG_MAX);

		agg_node = (Plan *)make_agg(root,
									upper_tlist,
									upper_qual,
									aggstrategy, false,
									numGroupCols,
									prelimGroupColIdx,
									lNumGroups,
									num_nullcols,
									input_grouping,
									grouping,
									rollup_gs_times,
									numAggs,
									transSpace,
									result_plan);
	}

	else
	{
		agg_node = (Plan *)make_group(root,
									  upper_tlist,
									  upper_qual,
									  numGroupCols,
									  prelimGroupColIdx,
									  numGroups,
									  num_nullcols,
									  input_grouping,
									  grouping,
									  rollup_gs_times,
									  result_plan);
	}

	/*
	 * Agg/Group will not change the sort order unless it is hashed.
	 */
	agg_node->flow = pull_up_Flow(agg_node, 
								  agg_node->lefttree, 
								  (*p_current_pathkeys != NIL)
								   && aggstrategy != AGG_HASHED );
	
	/* 
	 * Since the rtable has changed, we had better recreate a RelOptInfo entry for it.
	 */
	if (root->simple_rel_array)
		pfree(root->simple_rel_array);
	root->simple_rel_array_size = list_length(parse->rtable) + 1;
	root->simple_rel_array = (RelOptInfo **)
		palloc0(root->simple_rel_array_size * sizeof(RelOptInfo *));
	build_simple_rel(root, 1, RELOPT_BASEREL);

	return agg_node;
}


/* 
 * Add a SubqueryScan node to the input plan and maintain the given 
 * pathkeys by making adjustments to them and to the equivalence class
 * information in root.
 *
 * Note that submerging a plan into a subquery scan will require changes
 * to the range table and to any expressions above the new scan node.  
 * This is the caller's responsibility since the nature of the changes
 * depends on the context in which the subquery is used.
 */
Plan* add_subqueryscan(PlannerInfo* root, List **p_pathkeys,
					   Index varno, Query *subquery, Plan *subplan)
{
	List *subplan_tlist;
	int *resno_map;

	subplan_tlist = generate_subquery_tlist(varno, subquery->targetList,
											&resno_map); 

	/* Do we need to fill in more?  I don't think so. */
	
	subplan = (Plan*)make_subqueryscan(subplan_tlist,
										   NIL,
										   varno, /* scanrelid (= varno) */
										   subplan);

	mark_passthru_locus(subplan, true, true);

	/* Reconstruct equi_key_list since the rtable has changed.
	 * XXX we leak the old one.
	 */
	root->equi_key_list = construct_equivalencekey_list(root->equi_key_list,
														resno_map,
														subquery->targetList,
														subplan_tlist);
	if ( p_pathkeys != NULL && *p_pathkeys != NULL )
	{
		*p_pathkeys = reconstruct_pathkeys(root, *p_pathkeys, resno_map,
										   subquery->targetList, subplan_tlist);
	}

	pfree(resno_map);
	
	return subplan;
}


/*
 * hash_safe_type - is the given type hashable?
 *
 * Modelled on function hash_safe_grouping in planner.c, we assume
 * the type is hashable if its equality operator is marked hashjoinable.
 */
static bool
hash_safe_type(Oid type)
{
	Operator	optup;
	bool		oprcanhash;

	optup = equality_oper(type, true);
	if (!optup)
		return false;
	oprcanhash = ((Form_pg_operator) GETSTRUCT(optup))->oprcanhash;
	ReleaseSysCache(optup);
	if (!oprcanhash)
		return false;

	return true;
}

/*
 * sorting_prefixes_grouping - is the result ordered on a grouping key prefix?
 *
 * If so, then we might prefer a pre-ordered grouping result to one that would
 * need sorting after the fact. 
 */
static bool 
sorting_prefixes_grouping(PlannerInfo *root)
{
	return root->sort_pathkeys != NIL
			&& pathkeys_contained_in(root->sort_pathkeys, root->group_pathkeys);
}

/*
 * gp_hash_safe_grouping - are grouping operators GP hashable for
 * redistribution motion nodes?
 */
static bool
gp_hash_safe_grouping(PlannerInfo *root)
{
	List *grouptles;
	ListCell   *glc;

	grouptles = get_sortgroupclauses_tles(root->parse->groupClause,
										 root->parse->targetList);
	foreach(glc, grouptles)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(glc);
		bool canhash;
		canhash = isGreenplumDbHashable(exprType((Node *)tle->expr));
		if (!canhash)
			return false;
	}
	return true;
}

/*
 * reconstruct_pathkeys
 *
 * Reconstruct the given pathkeys based on the given mapping from the original
 * targetlist to a new targetlist.
 */
List *
reconstruct_pathkeys(PlannerInfo *root, List *pathkeys, int *resno_map,
					 List *orig_tlist, List *new_tlist)
{
	List *new_pathkeys;
	
	ListCell *lc;
	foreach (lc, pathkeys)
	{
		List *keys = (List *)lfirst(lc);
		ListCell *key_lc;
		TargetEntry *new_tle;
			
		Assert(IsA(keys, List));
		foreach(key_lc, keys)
		{
			PathKeyItem *item = (PathKeyItem *)lfirst(key_lc);
			int resno = 0;
			ListCell *tle_lc;
			Assert(IsA(item, PathKeyItem));
				
			foreach(tle_lc, orig_tlist)
			{
				TargetEntry *tle = (TargetEntry *)lfirst(tle_lc);
				Assert(IsA(tle, TargetEntry));
				if (equal(tle->expr, item->key))
				{
					resno = tle->resno;
					break;
				}
			}
			if (resno > 0)
			{
				new_tle = get_tle_by_resno(new_tlist, resno_map[resno-1]);
				Assert(new_tle != NULL);
				item->key = (Node *)new_tle->expr;
			}
		}
	}

	new_pathkeys = canonicalize_pathkeys(root, pathkeys);

	return new_pathkeys;
}
