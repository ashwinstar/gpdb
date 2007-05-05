/*-------------------------------------------------------------------------
 *
 * plangroupexp.c
 *    Planning routines for grouping extensions.
 *
 * Copyright (c) 2007, Greenplum Inc
 *
 * IDENTIFICATION
 *    $PostgreSQL$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "parser/parse_expr.h"
#include "parser/parse_clause.h"   /* assignSortGroupRef */
#include "optimizer/planmain.h"
#include "optimizer/cost.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/paths.h"
#include "optimizer/planshare.h"

#include "cdb/cdbsetop.h"					/* make_motion... routines */
#include "cdb/cdbvars.h"
#include "cdb/cdbpathlocus.h"
#include "cdb/cdbgroup.h" 					/* generate_three_tlists */

typedef struct GroupExtContext
{
	Path *path; /* the path before grouping planning */
	List **p_tlist;
	List *sub_tlist;
	List *qual;

	AggStrategy aggstrategy;
	int numGroupCols;
	AttrNumber *grpColIdx;
	AggClauseCounts *agg_counts;
	double *p_dNumGroups;
	List **p_current_pathkeys;
	
	bool is_agg; /* indicate whether to create Agg/Group nodes. */
	bool twostage; /* indicate if this appears in a two-stage aggregation. */

	CanonicalGroupingSets *canonical_grpsets;

	/* The following are intended for input sharing */
	Plan *subplan;
	CdbPathLocus input_locus;
	/* also the above p_current_pathkeys */
	/* requested sort order for each Agg node using this shared input */
	List *pathkeys;
} GroupExtContext;

static void destroyGroupExtContext(GroupExtContext *context);
static Plan *make_aggs_for_rollup(PlannerInfo *root,
								  GroupExtContext *context,
								  Plan *lefttree);
static Plan *make_list_aggs_for_rollup(PlannerInfo *root,
									   GroupExtContext *context,
									   Plan *lefttree);
static Plan*make_append_aggs_for_rollup(PlannerInfo *root,
										GroupExtContext *context,
										Plan *lefttree);
static List *generate_append_tlist(List *tlist);
static List *plan_list_aggs(PlannerInfo *root,
							GroupExtContext *context,
							Plan *lefttree);
static Plan *add_first_agg(PlannerInfo *root,
						   GroupExtContext *context,
						   int rollup_level,
						   int grouping,
						   int rollup_gs_times,
						   AttrNumber *groupColIdx,
						   List *current_tlist,
						   List *current_qual,
						   Plan *current_lefttree);
static List *generate_list_subplans(PlannerInfo *root, GroupExtContext *context);

/*
 * Create the plan for grouping extensions
 *
 * Most of the arguments are straightforward and similar to make_agg().
 * Exceptions are is_agg, p_tlist, twostage and sub_tlist.
 *
 * 'twostage' is used to tell if this function is called within a
 * two-stage aggregation.  If this is the case, we want to make sure
 * all targets in the targetlists generated here are not junk.
 *
 * 'is_agg' indicates to construct an Agg or Group node.
 *
 * 'p_tlist' is a pointer to the final target list for this sequence of Agg nodes.
 * We need to pass the final target list out because the final stage of two-stage
 * aggregation needs this.
 *
 * 'sub_tlist' is the subplan targetlist generated from '*p_tlist'.
 */
Plan *
plan_grouping_extension(PlannerInfo *root,
						Path *path,
						List **p_tlist, List *sub_tlist,
						bool is_agg, bool twostage,
						List *qual, AggStrategy aggstrategy,
						int *p_numGroupCols, AttrNumber **p_grpColIdx,
						AggClauseCounts *agg_counts,
						CanonicalGroupingSets *canonical_grpsets,
						double *p_dNumGroups,
						List **p_current_pathkeys,
						Plan *lefttree)
{
	GroupExtContext context;
	Plan *result_plan;
	Grouping *grping;
	GroupId *grpid;
	AttrNumber grping_resno;
	TargetEntry *tle;
	List *new_subtlist = NIL;

	/*
	 * We always add GROUPING and GROUP_ID to *p_tlist.
	 */
	grping = makeNode(Grouping);
	tle = makeTargetEntry((Expr *)grping, list_length(*p_tlist) + 1,
						  "GROUPING", true);
	tle->ressortgroupref = assignSortGroupRef(tle, *p_tlist);
	*p_tlist = lappend(*p_tlist, tle);
	grping_resno = tle->resno;

	grpid = makeNode(GroupId);
	tle = makeTargetEntry((Expr *)grpid, list_length(*p_tlist) + 1,
						  "GROUP_ID", true);
	*p_tlist = lappend(*p_tlist, tle);

	/* Also add GROUPING into sub_tlist and grpColIdx. */
	tle = makeTargetEntry((Expr *)grping, list_length(sub_tlist) + 1,
						  "GROUPING", true);
	new_subtlist = (List *)copyObject(sub_tlist);
	new_subtlist = lappend(new_subtlist, tle);

	*p_grpColIdx =
		(AttrNumber *)repalloc(*p_grpColIdx,
							   (*p_numGroupCols + 1) * sizeof(AttrNumber));
	(*p_grpColIdx)[*p_numGroupCols] = tle->resno;
	(*p_numGroupCols)++;

	context.path = path;
	context.p_tlist = p_tlist;
	context.sub_tlist = new_subtlist;
	context.qual = qual;
	context.aggstrategy = aggstrategy;
	context.numGroupCols = *p_numGroupCols;
	context.grpColIdx = *p_grpColIdx;
	context.agg_counts = agg_counts;
	context.p_dNumGroups = p_dNumGroups;
	context.p_current_pathkeys = p_current_pathkeys;
	context.is_agg = is_agg;
	context.twostage = twostage;
	context.canonical_grpsets = canonical_grpsets;

	/*
	 * XXX Right now, we only consider single-ROLLUP queries.
	 * For generalized grouping extensions, we will convert them
	 * to multiple rollups, and add the planning here eventually.
	 */
	result_plan = make_aggs_for_rollup(root, &context, lefttree);

	/* destroy the context */
	destroyGroupExtContext(&context);

	/* reset resno for Grouping */
	if (twostage)
		(*p_grpColIdx)[(*p_numGroupCols)-1] = *p_numGroupCols;
	else
		(*p_grpColIdx)[(*p_numGroupCols)-1] = grping_resno;

	*p_tlist = *(context.p_tlist);

	return result_plan;
}

/*
 * Create an agg plan for a complete or partial ROLLUP clause,
 * defined by context->canonical_grpsets->grpset_counts.
 */
static Plan *
make_aggs_for_rollup(PlannerInfo *root,
					 GroupExtContext *context,
					 Plan *lefttree)
{
	Plan *result_plan;

	/*
	 * We always try to create a sequence of Agg/Group nodes one after
	 * the other whenever possible. Currently, this includes non-distinct
	 * qualified queries and there are defined prelimfuncs for requested
	 * aggregates. Otherwise, we fall back to use
	 * Append-Aggs.
	 */
	if (context->agg_counts->numDistinctAggs == 0 &&
		!context->agg_counts->missing_prelimfunc)
		result_plan = make_list_aggs_for_rollup(root, context, lefttree);
	else
		result_plan = make_append_aggs_for_rollup(root, context, lefttree);

	return result_plan;
}

/*
 * Create a sequence of Agg/Group nodes one after the other.
 *
 * The upper Agg/Group nodes aggregate over the results from the lower
 * Agg/Group nodes. The upper Agg/Group nodes not only return their own
 * aggregate results, but also pass through the aggregate results from
 * all lower nodes.
 *
 * Note that this is not suitable for DISTINCT-qualified rollup queries.
 */
static Plan *
make_list_aggs_for_rollup(PlannerInfo *root,
						  GroupExtContext *context,
						  Plan *lefttree)
{
	int group_no;
	Plan *current_lefttree = lefttree;
	Plan *agg_node = NULL;
	AttrNumber *prelimGroupColIdx;

	List *tlist1 = NIL;
	List *tlist2 = NIL;
	List *tlist3 = NIL;
	List *qual2 = NIL;
	List *qual3 = NIL;
	int last_group_no = 0;
	bool use_tlist2 = false; /* used to tell if we should free tlist2. */

	AttrNumber *groupColIdx;
	List *current_tlist;
	List *current_qual;
	bool need_finalagg = false;
	double current_numGroups;
	uint64 grouping = 0;
	double numGroups = *context->p_dNumGroups; /* make a copy of the original number */

	prelimGroupColIdx =
		(AttrNumber*)palloc(context->numGroupCols * sizeof(AttrNumber));
	for ( group_no = 0; group_no < context->numGroupCols; group_no++ )
		prelimGroupColIdx[group_no] = group_no+1;
	
	generate_three_tlists(*context->p_tlist, context->twostage,
						  context->sub_tlist, (Node *)context->qual,
						  context->numGroupCols, context->grpColIdx,
						  &tlist1, &tlist2, &tlist3, &qual2, &qual3);

	/*
	 * Find the index position for the last rollup level. This is used
	 * to determine if we want to use tlist3 or tlist2 for the intermediate
	 * Agg node in a complete rollup.
	 */
	for (group_no=0; group_no < context->canonical_grpsets->ngrpsets; group_no++)
	{
		if (context->canonical_grpsets->grpset_counts[group_no] > 0)
			last_group_no = group_no;
	}

	/* Generate the bottom-level Agg node */
	for (group_no=0; group_no <= last_group_no; group_no++)
	{
		if (group_no > 0)
			grouping += (1 << (group_no-1));

		if (context->canonical_grpsets->grpset_counts[group_no] > 0)
		{
			groupColIdx = context->grpColIdx;
			current_tlist = tlist1;
			current_qual = context->qual;

			/* Add an Agg node */
			agg_node = add_first_agg(root, context,
									 group_no+1,
									 grouping,
									 context->canonical_grpsets->grpset_counts[group_no],
									 groupColIdx,
									 current_tlist,
									 current_qual,
									 current_lefttree);

			current_lefttree = agg_node;
			*context->p_tlist = current_tlist;

			group_no++;

			/* XXX Do we want to add Grouping into pathkey? */

			break;
		}
	}

	/*
	 * Check if the attributes appeared in the final rollup level match
	 * the distribution attributes. If not, we need to add on another
	 * aggregate node to aggregate results from different segments.
	 */
	if (agg_node->flow->hashExpr != NULL)
	{
		/*
		 * If agg_node->flow is FLOW_SINGLETON, we do not need the final
		 * aggregation.
		 */
		if (agg_node->flow->flotype == FLOW_SINGLETON)
			need_finalagg = false;
		else
		{

			ListCell *lc;
			TargetEntry *tle;

			foreach (lc, agg_node->flow->hashExpr)
				{
					Node *x = (Node *)lfirst(lc);

					tle = tlist_member(x, current_tlist);
					if (tle == NULL)
						ereport(ERROR, (
									errcode(ERRCODE_CDB_INTERNAL_ERROR),
									errmsg("target not found") ));
					if (tle->resno > context->numGroupCols-1-last_group_no)
					{
						need_finalagg = true;
						break;
					}
				}
		}
	}

	/* Construct the rest Agg nodes. */
	for (; group_no <= last_group_no; group_no++)
	{
		if (group_no > 0)
			grouping += (1 << (group_no-1));

		if (context->canonical_grpsets->grpset_counts[group_no] <= 0)
			continue;

		if ((group_no == context->numGroupCols ||
			 group_no == last_group_no) &&
			!need_finalagg &&
			!context->twostage)
		{
			groupColIdx = prelimGroupColIdx;
			current_tlist = tlist3;
			current_qual = qual3;
		}
		else
		{
			groupColIdx = prelimGroupColIdx;
			current_tlist = tlist2;
			current_qual = qual2;
			use_tlist2 = true;
		}

		current_numGroups =
			(((double)(context->numGroupCols-group_no))/((double)(context->numGroupCols))) *
			numGroups;
		if (current_numGroups == 0)
			current_numGroups = 1;

		*context->p_dNumGroups += current_numGroups;

		agg_node = add_second_stage_agg(root, context->is_agg, *context->p_tlist, current_tlist,
										current_qual, context->aggstrategy,
										context->numGroupCols, groupColIdx,
										group_no+1, grouping,
										context->canonical_grpsets->grpset_counts[group_no],
										current_numGroups,
										context->agg_counts->numAggs, context->agg_counts->transitionSpace,
										"rollup",
										context->p_current_pathkeys,
										agg_node);

		current_lefttree = agg_node;
		*context->p_tlist = current_tlist;
	}

	/*
	 * If we need to bring back aggregate results from different segments,
	 * we add a Gather motion node on top of the top Agg node to do the
	 * final aggregation.
	 */
	if (need_finalagg && !context->twostage)
	{
		long lNumGroups;

		/* This is the final Agg node for a rollup. */
		int rollup_level = -1;

		double motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
			gp_motion_cost_per_row :
			2.0 * cpu_tuple_cost;

		agg_node = (Plan *)make_motion_gather_to_QE(agg_node, true);
		agg_node->total_cost += motion_cost_per_row * agg_node->plan_rows;

		/* Convert # groups to long int --- but 'ware overflow! */
		lNumGroups = (long) Min(*context->p_dNumGroups, (double) LONG_MAX);

		/* Add a Agg/Group node */
		agg_node = add_second_stage_agg(root, context->is_agg, *context->p_tlist, tlist3,
										qual3, context->aggstrategy,
										context->numGroupCols, prelimGroupColIdx,
										rollup_level, /* rollup_level */
										0, /* grouping */
										0, /* rollup_gs_times */
										lNumGroups, context->agg_counts->numAggs,
										context->agg_counts->transitionSpace,
										"rollup", context->p_current_pathkeys, agg_node);

		*context->p_tlist = tlist3;
	}

	if (!context->is_agg)
	{
		((Group*)agg_node)->lastGroup = true;
	}

	if (!use_tlist2)
		list_free_deep(tlist2);

	return agg_node;
}

/*
 * Create an agg plan by using an Append node to glue together a list
 * of Agg/Group nodes, each of which represents an Agg/Group node in a
 * rollup level.
 */
static Plan*
make_append_aggs_for_rollup(PlannerInfo *root,
							GroupExtContext *context,
							Plan *lefttree)
{
	Plan *result_plan;
	List *agg_plans = NIL;
	List *targetlist = NIL;
	GpSetOpType optype = PSETOP_NONE;
	Plan *common_source_plan;
	double      motion_cost_per_row;

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

	/* The common source plan is a Gather merge node on top of the given
	 * subplan
	 */
	common_source_plan =
		(Plan *)make_motion_gather_to_QE(lefttree, (*context->p_current_pathkeys != NIL));
	common_source_plan->total_cost +=
		motion_cost_per_row * common_source_plan->plan_rows;

	/* Plan a list of Agg/Group nodes with the common source plan */
	agg_plans = plan_list_aggs(root, context, common_source_plan);

	/* CDB: Decide on approach, condition argument plans to suit. */
	if ( Gp_role == GP_ROLE_DISPATCH )
	{
		optype = choose_setop_type(agg_plans);
		adjust_setop_arguments(agg_plans, optype);
	}

	/* Generate the targetlist for Append node */
	targetlist = generate_append_tlist(*context->p_tlist);

	/* Append all agg_plans together */
	result_plan = (Plan *)make_append(agg_plans, false, targetlist);
	mark_append_locus(result_plan, optype); /* CDB: Mark the plan result locus. */

	return result_plan;
}

/*
 * Generate the targetlist for an Append node based on the children targetlist.
 *
 * The entries in the Append's targetlist should always be simple Vars;
 * we just have to make sure they have the right datatypes and typmods.
 * The Vars are always generated with varno OUTER (CDB/MPP change for
 * EXPLAIN; varno 0 was used in PostgreSQL), right?
 */
static List *
generate_append_tlist(List *tlist)
{
	List *targetlist = NIL;
	ListCell *lc;
	int resno = 1;

	foreach (lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);
		Expr *new_expr;
		TargetEntry *new_tle;

		new_expr = (Expr *)makeVar(OUTER,
								   resno,
								   exprType((Node *) tle->expr),
								   exprTypmod((Node *) tle->expr),
								   0);
		new_tle = makeTargetEntry(new_expr,
								  (AttrNumber)resno,
								  tle->resname ? pstrdup(tle->resname) : NULL,
								  tle->resjunk);
		resno++;

		/* Keep the resortgroupref */
		new_tle->ressortgroupref = tle->ressortgroupref;

		targetlist = lappend(targetlist, new_tle);
	}

	return targetlist;
}

/*
 * Generates a list of Agg/Group nodes, which share the same
 * common source plan.
 */
static List *
plan_list_aggs(PlannerInfo *root,
			   GroupExtContext *context,
			   Plan *lefttree)
{
	List *agg_plans = NIL;
	int group_no;
	uint64 grouping = 0;
	List *subplans = NIL;

	/*
	 * Generate subplans for Agg nodes in each requested rollup level.
	 * Input sharing should be applied here by constructing appropriate
	 * subplans to be used.
	 */
	context->subplan = lefttree;
	context->input_locus = context->path->locus;
	context->pathkeys = NIL;
	for (group_no=0; group_no<context->canonical_grpsets->ngrpsets; group_no++)
	{
		/*
		 * XXX We use the same pathkeys for each Agg node here. This
		 * could be improved to use different pathkeys, because each
		 * Agg node does have different sorting requirements.
		 */
		context->pathkeys = lappend(context->pathkeys,
									*context->p_current_pathkeys);
	}

	subplans = generate_list_subplans(root, context);

	/* Generate an Agg/Group node for each rollup level */
	for (group_no=0; group_no<context->canonical_grpsets->ngrpsets; group_no++)
	{
		Plan *subplan = (Plan *)list_nth(subplans, group_no);

		if (group_no > 0)
			grouping += (1 << (group_no-1));

		if (context->canonical_grpsets->grpset_counts[group_no] > 0)
		{
			Plan *agg_plan ;

			/* Add an explicit sort if we couldn't make the path
			 * come out the way the Agg/Group node needs it.
			 */
			if (!pathkeys_contained_in(root->group_pathkeys,
									   *context->p_current_pathkeys))
			{
				subplan = (Plan *)
					make_sort_from_groupcols(root,
											 root->parse->groupClause,
											 context->grpColIdx,
											 true,
											 subplan);
				*context->p_current_pathkeys = root->group_pathkeys;
				mark_sort_locus(subplan);

				/* If this is a Group node, pass DISTINCT to sort */
				if (!context->is_agg &&
					IsA(subplan, Sort) &&
					gp_enable_sort_distinct)
				{
					Sort* pSort = (Sort*)subplan;
					pSort->noduplicates = true;
				}
			}

			/* Add an Agg node */
			agg_plan = add_first_agg(root, context,
									 0,
									 grouping,
									 context->canonical_grpsets->grpset_counts[group_no],
									 context->grpColIdx,
									 *context->p_tlist,
									 context->qual,
									 subplan);

			/* Set numNullColls explicitly */
			if (context->is_agg)
				((Agg *) agg_plan)->numNullCols = group_no;
			else
				((Group *) agg_plan)->numNullCols = group_no;

			agg_plans = lappend(agg_plans, agg_plan);
		}
	}

	return agg_plans;
}

static Plan *
add_first_agg(PlannerInfo *root,
			  GroupExtContext *context,
			  int rollup_level,
			  int grouping,
			  int rollup_gs_times,
			  AttrNumber *groupColIdx,
			  List *current_tlist,
			  List *current_qual,
			  Plan *current_lefttree)
{
	double current_numGroups;
	Plan *agg_node;

	/*
	 * XXX We estimate number of groups for each rollup level using
	 * a "straight-line interpolation" approach. Consider there
	 * are N groups for (a,b,c). We consider that there are (2/3)N
	 * groups for (a,b), (1/3)N groups for (a), and 1 group for ().
	 * We also consider the cost for a pass-through tuple as 0.
	 */
	current_numGroups =
		(((double)(context->numGroupCols-rollup_level+1))/((double)(context->numGroupCols))) *
		*context->p_dNumGroups;
	if (current_numGroups == 0)
		current_numGroups = 1;

	*context->p_dNumGroups += current_numGroups;

	if (context->is_agg)
	{
		/* convert current_numGroups to long int */
		long lNumGroups = (long) Min(current_numGroups, (double) LONG_MAX);

		/* For the first Agg node, we don't really want to consider
		 * GROUPING in groupColIdx, which is the last entry in groupColIdx.
		 */
		agg_node = (Plan *)make_agg(root, current_tlist, current_qual, context->aggstrategy,
									context->numGroupCols-1, groupColIdx,
									lNumGroups, rollup_level, grouping,
									rollup_gs_times,
									context->agg_counts->numAggs, context->agg_counts->transitionSpace,
									current_lefttree);
	}

	else
	{
		agg_node = (Plan *)make_group(root, current_tlist, current_qual,
									  context->numGroupCols-1,
									  groupColIdx, current_numGroups,
									  rollup_level, grouping,
									  rollup_gs_times,
									  current_lefttree);
	}
			
	/*
	 * Sort-based Agg/Group will not change the sort order.
	 */
	if (context->aggstrategy == AGG_SORTED)
		mark_sort_locus(agg_node);
	else
		mark_plan_strewn(agg_node);

	return agg_node;
}

/*
 * Generate a list of subplans based on context->subplan.
 *
 * Input sharing should be applied here by constructing appropriate
 * subplans to be used instead of repeated uses of the common subplan
 * from context->subplan.
 */
static List *
generate_list_subplans(PlannerInfo *root, GroupExtContext *context)
{
	List *subplans = NIL;
	int group_no;

	ShareNode_ShareType stype = add_sharenode(root,
											  &context->subplan,
											  &context->pathkeys,
											  context->input_locus,
											  (void *) context,
											  context->numGroupCols + 1,
											  NULL,
											  NULL);
 
	if(stype == SHARENODE_UNKNOWN || !gp_window_shareinput)
		for (group_no = 0; group_no < context->numGroupCols + 1; group_no++)
			subplans = lappend(subplans, copyObject(context->subplan));

	else
	{
		Assert(stype == SHARENODE_MATERIAL);
		for(group_no = 0; group_no < context->numGroupCols + 1; group_no++)
		{
			Plan *newplan = (*(shareplan_ms.newplan_for_subplan)) (NULL, root, context->subplan);
			subplans = lappend(subplans, newplan);
		}
	}

	return subplans;
}

static void
destroyGroupExtContext(GroupExtContext *context)
{
	if (context->sub_tlist)
		list_free(context->sub_tlist);
}
