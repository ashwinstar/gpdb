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
#include "parser/parse_oper.h"     /* ordering_oper_opid */
#include "optimizer/planmain.h"
#include "optimizer/cost.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/paths.h"
#include "optimizer/planshare.h"
#include "parser/parse_relation.h" /* addRangeTableEntryForSubquery */
#include "parser/parsetree.h"  /* get_tle_by_resno */

#include "cdb/cdbsetop.h"					/* make_motion... routines */
#include "cdb/cdbvars.h"
#include "cdb/cdbpathlocus.h"
#include "cdb/cdbgroup.h" 					/* generate_three_tlists */
#include "cdb/cdbllize.h"                   /* pull_up_Flow */

/*
 * Define a canonical form of a ROLLUP.
 *
 * This can be used to represent a complete or partial ROLLUP, from
 * the user requested ROLLUP clause, or part of user requested CUBE
 * or GROUPING SETS clauases.
 */
typedef struct CanonicalRollup
{
	/* Define the number of columns in a rollup. The maximum number of
	 * grouping sets in a rollup is (numcols+1).
	 */
	int numcols;

	/* The column index for this rollup */
	AttrNumber *colIdx;

	/* This rollup may be part of a GROUPING SETS or a CUBE. This value
	 * is used to record the relative position of columns in the original
	 * grouping extension clause.
	 *
	 * For example, the original grouping extension is
	 *
	 *    CUBE (a,b,c)
	 *
	 * which can be converted into the following rollups:
	 *    (a,b,c) ==> (a,b) ==> (a) ==> ()
	 *    (c,a) ==> (c)
	 *    (b,a) ==> (b)
	 *
	 * Assume that this canonical rollup wants to represent the second
	 * rollup "(c,a) ==> (c)". We want to record the position of "c" and
	 * "a" in the original CUBE clause. This is used to calculate
	 * the GROUPING value.
	 *
	 * Note that the value for positions starts with 0.
	 */
	int *grping_pos;

	/* Since this rollup can be a partial rollup, we record the number
	 * of grouping sets that are really appeared in this rollup.
	 */
	int ngrpsets;

	/* Represent the number of times that each grouping set appears
	 * in a rollup. There should be 'numcols+1' elements in this array.
	 *
	 * Note that this should be the last one in this structure.
	 */
	int grpset_counts[1];
} CanonicalRollup;

typedef struct GroupExtContext
{
	Path *path; /* the path before grouping planning */
	List *tlist;
	List *sub_tlist;
	List *qual;

	AggStrategy aggstrategy;
	int numGroupCols;
	AttrNumber *grpColIdx;
	AggClauseCounts *agg_counts;
	double *p_dNumGroups;
	List *current_pathkeys;
	
	bool is_agg; /* indicate whether to create Agg/Group nodes. */
	bool twostage; /* indicate if this appears in a two-stage aggregation. */
	bool querynode_changed; /* indicate if root->parse is changed. */

	List *canonical_rollups; /* list of CanonicalRollups */

	/* the rollup which is currently being processed */
	CanonicalRollup *current_rollup;

	/* The following are intended for input sharing */
	Plan *subplan;
	CdbPathLocus input_locus;
	/* also the above p_current_pathkeys */
	/* requested sort order for each Agg node using this shared input */
	List *pathkeys;
} GroupExtContext;

static void destroyGroupExtContext(GroupExtContext *context);
static List *convert_gs_to_rollups(AttrNumber *grpColIdx,
								   int numGroupCols,
								   CanonicalGroupingSets *canonical_grpsets,
								   List *canonical_rollups,
								   List *sub_tlist,
								   List *tlist);
static Plan *make_aggs_for_rollup(PlannerInfo *root,
								  GroupExtContext *context,
								  Plan *lefttree);
static Plan *make_list_aggs_for_rollup(PlannerInfo *root,
									   GroupExtContext *context,
									   Plan *lefttree);
static Plan*make_append_aggs_for_rollup(PlannerInfo *root,
										GroupExtContext *context,
										Plan *lefttree);
static List *generate_append_tlist(List *tlist, int varno);
static List *plan_list_aggs(PlannerInfo *root,
							GroupExtContext *context,
							Plan *lefttree);
static Plan *add_first_agg(PlannerInfo *root,
						   GroupExtContext *context,
						   int num_nullcols,
						   uint64 input_grouping,
						   uint64 grouping,
						   int rollup_gs_times,
						   int numGroupCols,
						   AttrNumber *groupColIdx,
						   List *current_tlist,
						   List *current_qual,
						   Plan *current_lefttree);
static List *generate_list_subplans(PlannerInfo *root,
									int num_subplans,
									GroupExtContext *context);
static List *add_grouping_groupid(List *tlist);
static void append_colIdx(AttrNumber *colIdx, int numcols, int colno,
						  int *grping_pos, Bitmapset *bms, AttrNumber *grpColIdx,
						  Index *sortrefs_new_to_orig);
static Plan *plan_list_rollup_plans(PlannerInfo *root,
									GroupExtContext *context,
									Plan *lefttree);
static List *make_pathkeys_for_rollup(List *groupclause,
									  List *tlist,
									  AttrNumber *grpColIdx,
									  CanonicalRollup *rollup);

/*
 * Create the plan for grouping extensions
 *
 * Most of the arguments are straightforward and similar to make_agg().
 * Exceptions are twostage, is_agg, p_tlist and sub_tlist.
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
						List *qual,
						int *p_numGroupCols, AttrNumber **p_grpColIdx,
						AggClauseCounts *agg_counts,
						CanonicalGroupingSets *canonical_grpsets,
						double *p_dNumGroups,
						bool *querynode_changed,
						List **p_current_pathkeys,
						Plan *lefttree)
{
	GroupExtContext context;
	Plan *result_plan;
	ListCell *lc;
	List *tlist = copyObject(*p_tlist);

	context.path = path;
	context.tlist = tlist;
	context.sub_tlist = copyObject(sub_tlist);
	context.qual = qual;
	context.numGroupCols = *p_numGroupCols;
	context.grpColIdx = palloc0(context.numGroupCols * sizeof(AttrNumber));
	memcpy(context.grpColIdx, *p_grpColIdx,
		   context.numGroupCols * sizeof(AttrNumber));
	context.agg_counts = agg_counts;
	context.p_dNumGroups = p_dNumGroups;
	context.current_pathkeys = *p_current_pathkeys;
	context.is_agg = is_agg;
	context.twostage = twostage;
	context.querynode_changed = false;
	context.canonical_rollups = NIL;

	/*
	 * Set ressortgroupref in all target entries in sub_tlist solely
	 * based on existing grpColidx.
	 */
	foreach (lc, context.sub_tlist)
	{
		int group_no;
		TargetEntry *tle = (TargetEntry *)lfirst(lc);
		for (group_no=0; group_no<context.numGroupCols; group_no++)
		{
			if (tle->resno == context.grpColIdx[group_no])
			{
				tle->ressortgroupref = group_no + 1;
			}
		}
	}

	/*
	 * Convert a list of grouping sets into multiple rollups.
	 */
	context.canonical_rollups =
		convert_gs_to_rollups(context.grpColIdx, context.numGroupCols,
							  canonical_grpsets, context.canonical_rollups,
							  context.sub_tlist, context.tlist);

	/* Construct a plan for a list of rollups. */
	result_plan = plan_list_rollup_plans(root, &context, lefttree);

	/* destroy the context */
	destroyGroupExtContext(&context);

	/* Set the proper resno for Grouping column in *p_grpColIdx.
	 */
	*p_grpColIdx =
		(AttrNumber *)repalloc(*p_grpColIdx,
							   (*p_numGroupCols + 1) * sizeof(AttrNumber));
	(*p_numGroupCols)++;

	*p_tlist = context.tlist;
	(*p_grpColIdx)[(*p_numGroupCols)-1] = list_length(*p_tlist) - 1;

	*p_current_pathkeys = context.current_pathkeys;

	*querynode_changed = context.querynode_changed;

	return result_plan;
}

/*
 * Create an agg plan for a complete or partial ROLLUP clause,
 * defined by context->current_rollup->grpset_counts.
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

	AttrNumber *orig_grpColIdx;
	AttrNumber *groupColIdx;
	List *current_tlist;
	List *current_qual;
	double current_numGroups;
	uint64 grouping = 0;
	double numGroups = *context->p_dNumGroups; /* make a copy of the original number */

	TargetEntry *tle;
	Grouping *grping;
	uint64    input_grouping = 0;
	bool need_finalagg = !context->twostage;

	generate_three_tlists(context->tlist, context->twostage,
						  context->sub_tlist, (Node *)context->qual,
						  context->numGroupCols,
						  context->grpColIdx,
						  &tlist1, &tlist2, &tlist3, &qual3);

	/* Even though we use context->grpColIdx to generate
	 * these tlists, but the order of grouping columns are defined in
	 * context->current_rollup->colIdx. We reflect this
	 * in constructing prelimGroupColIdx.
	 */
	prelimGroupColIdx =
		(AttrNumber*)palloc(context->numGroupCols * sizeof(AttrNumber));
	for (group_no = 0; group_no < context->numGroupCols; group_no++ )
	{
		int resno = context->current_rollup->colIdx[group_no];
		int pos;

		for (pos=0; pos<context->numGroupCols; pos++)
		{
			if (context->grpColIdx[pos] == resno)
				break;
		}
		prelimGroupColIdx[group_no] = pos+1;
	}

	orig_grpColIdx = context->grpColIdx;
	context->grpColIdx = context->current_rollup->colIdx;

	/*
	 * We always append Grouping and group_id to all three tlists.
	 * Grouping is also served as an additional grouping column. So
	 * we add it to both context->grpColIdx, and prelimGroupColIdx.
	 */
	tlist1 = add_grouping_groupid(tlist1);
	tlist2 = add_grouping_groupid(tlist2);
	tlist3 = add_grouping_groupid(tlist3);

	/* Also add GROUPING into sub_tlist. */
	grping = makeNode(Grouping);
	tle = makeTargetEntry((Expr *)grping, list_length(context->sub_tlist) + 1,
						  "grouping", true);
	context->sub_tlist = lappend(context->sub_tlist, tle);

	context->numGroupCols++;
	context->grpColIdx =
		(AttrNumber *)repalloc(context->grpColIdx,
							   context->numGroupCols * sizeof(AttrNumber));
	context->grpColIdx[context->numGroupCols - 1] = tle->resno;

	prelimGroupColIdx = (AttrNumber *)
		repalloc(prelimGroupColIdx, context->numGroupCols * sizeof(AttrNumber));
	prelimGroupColIdx[context->numGroupCols - 1] = list_length(tlist2) - 1;

#if 0
	/*
	 * XXX The sortgrouprefs for current_lefttree->targetlist may not be accurate
	 * anymore since context->grpColIdx may be different with orig_grpColIdx.
	 * However, we don't really use this information during planner here.
	 * I comment this out for now.
	 */
	{
		ListCell *lc;

		/*
		 * Set ressortgroupref in all target entries in current_lefttree->targtlist
		 * solely based on existing grpColidx.
		 */
		foreach (lc, current_lefttree->targetlist)
		{
			int group_no;
			TargetEntry *grp_tle = (TargetEntry *)lfirst(lc);
			for (group_no=0; group_no<context->numGroupCols; group_no++)
			{
				if (grp_tle->resno == context->grpColIdx[group_no])
				{
					grp_tle->ressortgroupref = group_no + 1;
				}
			}
		}
	}
#endif
	
	/*
	 * Find the index position for the last rollup level.
	 */
	for (group_no=0; group_no < context->current_rollup->numcols + 1; group_no++)
	{
		if (context->current_rollup->grpset_counts[group_no] > 0)
			last_group_no = group_no;
	}

	/* Generate the bottom-level Agg node */
	for (group_no=0; group_no <= last_group_no; group_no++)
	{
		if (group_no > 0)
			grouping += ( ((uint64)1) << context->current_rollup->grping_pos[
							 context->current_rollup->numcols - group_no]);

		if (context->current_rollup->grpset_counts[group_no] > 0)
		{
			AggStrategy aggstrategy;
			groupColIdx = context->grpColIdx;

			/* Add sort node if needed, and set AggStrategy */
			if (root->parse->groupClause)
			{
				if (!pathkeys_contained_in(root->group_pathkeys,
										   context->current_pathkeys))
				{
					current_lefttree = (Plan *)
						make_sort_from_reordered_groupcols(root,
														   root->parse->groupClause,
														   orig_grpColIdx,
														   groupColIdx,
														   NULL,
														   current_lefttree);
					context->current_pathkeys = root->group_pathkeys;
					mark_sort_locus(current_lefttree);

					if (!context->is_agg)
					{
						/* CDB */ /* pass DISTINCT to sort */
						if (IsA(current_lefttree, Sort) && gp_enable_sort_distinct)
						{
							Sort* pSort = (Sort*)current_lefttree;
							pSort->noduplicates = true;
						}
					}
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
				context->current_pathkeys = NIL;
			}

			context->aggstrategy = aggstrategy;

			if (current_lefttree->flow != NULL &&
				current_lefttree->flow->flotype == FLOW_SINGLETON)
				need_finalagg = false;

			if (!need_finalagg && group_no == last_group_no)
			{
				current_qual = context->qual;
				context->tlist = add_grouping_groupid(context->tlist);
				current_tlist = context->tlist;
				prelimGroupColIdx[context->numGroupCols-1] = list_length(current_tlist) - 1;
			}
			else
			{
				current_qual = NIL;
				current_tlist = tlist1;
			}

			/* Add an Agg node */
			agg_node = add_first_agg(root, context,
									 group_no,
									 0,
									 grouping,
									 context->current_rollup->grpset_counts[group_no],
									 context->numGroupCols - 1, /* ignore GROUPING */
									 groupColIdx,
									 current_tlist,
									 current_qual,
									 current_lefttree);

			current_lefttree = agg_node;
			context->tlist = current_tlist;

			input_grouping = grouping;
			group_no++;

			break;
		}
	}

	/* Construct the rest Agg nodes. */
	for (; group_no <= last_group_no; group_no++)
	{
		AttrNumber *new_grpColIdx;
		AttrNumber last_grpCol;

		if (group_no > 0)
			grouping += ( ((uint64)1) << context->current_rollup->grping_pos[
							 context->current_rollup->numcols - group_no]);

		if (context->current_rollup->grpset_counts[group_no] <= 0)
			continue;

		groupColIdx = prelimGroupColIdx;
		current_tlist = tlist2;
		current_qual = qual2;

		if (!need_finalagg && group_no == last_group_no)
		{
			current_tlist = tlist3;
			current_qual = qual3;
		}

		current_numGroups =
			(((double)(context->numGroupCols-group_no))/((double)(context->numGroupCols))) *
			numGroups;
		if (current_numGroups == 0)
			current_numGroups = 1;

		*context->p_dNumGroups += current_numGroups;

		/*
		 * Re-construct the grouping columns index array. The last column
		 * GROUPING is switched to its rightful position.
		 */
		new_grpColIdx = palloc0(context->numGroupCols * sizeof(AttrNumber));
		memcpy(new_grpColIdx, groupColIdx, context->numGroupCols * sizeof(AttrNumber));
		last_grpCol = new_grpColIdx[context->numGroupCols - 1];
		new_grpColIdx[context->numGroupCols - 1] =
			new_grpColIdx[context->numGroupCols - group_no - 1];
		new_grpColIdx[context->numGroupCols - group_no - 1] = last_grpCol;
		
		agg_node = add_second_stage_agg(root, context->is_agg, context->tlist, current_tlist,
										current_qual, context->aggstrategy,
										context->numGroupCols, new_grpColIdx,
										group_no, input_grouping, grouping,
										context->current_rollup->grpset_counts[group_no],
										current_numGroups,
										context->agg_counts->numAggs,
										context->agg_counts->transitionSpace,
										"rollup",
										&context->current_pathkeys,
										agg_node);

		/* Set inputHasGrouping */
		if (context->is_agg)
			((Agg *)agg_node)->inputHasGrouping = true;
		else
			((Group *)agg_node)->inputHasGrouping = true;

		context->querynode_changed = true;

		current_lefttree = agg_node;
		context->tlist = current_tlist;
		input_grouping = grouping;
	}

	/*
	 * If we need to bring back aggregate results from different segments,
	 * we add a Gather motion node on top of the top Agg node to do the
	 * final aggregation.
	 */
	if (need_finalagg)
	{
		long lNumGroups;
		int key_no;
		List *groupExprs = NIL;

		double motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
			gp_motion_cost_per_row :
			2.0 * cpu_tuple_cost;

		/* Build the hash keys */
		for (key_no=0; key_no<context->numGroupCols; key_no++)
		{
			TargetEntry *tle = get_tle_by_resno(agg_node->targetlist,
												prelimGroupColIdx[key_no]);
			groupExprs = lappend(groupExprs, copyObject(tle->expr));
		}
		
		agg_node = (Plan *)make_motion_hash(root, agg_node, groupExprs);
		agg_node->total_cost += motion_cost_per_row * agg_node->plan_rows;

		/* Add a sort node */
		if (context->aggstrategy == AGG_SORTED)
		{
			AttrNumber *orig_prelimGroupColIdx =
				(AttrNumber *)palloc0(context->numGroupCols * sizeof(AttrNumber));
			int attno;
			TargetEntry *grouping_entry;

			/* Create a group column index array that maps columns in
			 * prelimGroupColIdx to the position in the original GROUP
			 * BY clause.
			 */
			for (attno=0; attno<context->numGroupCols -1; attno++)
				orig_prelimGroupColIdx[attno] = attno+1;

			grouping_entry =
				get_tle_by_resno(agg_node->targetlist,
								 prelimGroupColIdx[context->numGroupCols - 1]);
			agg_node = (Plan *)
				make_sort_from_reordered_groupcols(root,
												   root->parse->groupClause,
												   orig_prelimGroupColIdx,
												   prelimGroupColIdx,
												   grouping_entry,
												   agg_node);
			pfree(orig_prelimGroupColIdx);
			mark_sort_locus(agg_node);
		}
		
		/* Convert # groups to long int --- but 'ware overflow! */
		lNumGroups = (long) Min(*context->p_dNumGroups, (double) LONG_MAX);

		/* Add a Agg/Group node */
		agg_node = add_second_stage_agg(root, context->is_agg, context->tlist, tlist3,
										qual3, context->aggstrategy,
										context->numGroupCols, prelimGroupColIdx,
										0,  /* num_nullcols */
										0, /* input_grouping */
										0, /* grouping */
										0, /* rollup_gs_times */
										lNumGroups, context->agg_counts->numAggs,
										context->agg_counts->transitionSpace,
										"rollup", &context->current_pathkeys,
										agg_node);

		if (context->is_agg)
			((Agg *)agg_node)->inputHasGrouping = true;
		else
			((Group *)agg_node)->inputHasGrouping = true;

		context->querynode_changed = true;
		context->tlist = tlist3;
	}

	if (!context->twostage)
	{
		if (context->is_agg)
			((Agg *)agg_node)->lastAgg = true;
		else
			((Group*)agg_node)->lastGroup = true;
	}

	pfree(orig_grpColIdx);
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
	AttrNumber *orig_grpColIdx;

	orig_grpColIdx = context->grpColIdx;
	context->grpColIdx = context->current_rollup->colIdx;

	/* Add an explicit sort if we couldn't make the path
	 * come out the way the Agg/Group node needs it.
	 */
	if (!pathkeys_contained_in(root->group_pathkeys,
							   context->current_pathkeys))
	{
		lefttree = (Plan *)
			make_sort_from_reordered_groupcols(root,
											   root->parse->groupClause,
											   orig_grpColIdx,
											   context->grpColIdx,
											   NULL,
											   lefttree);
		context->current_pathkeys = root->group_pathkeys;
		mark_sort_locus(lefttree);

		/* If this is a Group node, pass DISTINCT to sort */
		if (!context->is_agg &&
			IsA(lefttree, Sort) &&
			gp_enable_sort_distinct)
		{
			Sort* pSort = (Sort*)lefttree;
			pSort->noduplicates = true;
		}
	}

	pfree(orig_grpColIdx);

	context->aggstrategy = AGG_SORTED; /* set the strategy to AGG_SORTED */

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
		(Plan *)make_motion_gather_to_QE(lefttree, (context->current_pathkeys != NIL));
	common_source_plan->total_cost +=
		motion_cost_per_row * common_source_plan->plan_rows;

	/* Plan a list of Agg/Group nodes with the common source plan */
	agg_plans = plan_list_aggs(root, context, common_source_plan);

	/* Add 'Append' if there are more than one subplan. */
	if (list_length(agg_plans) > 1)
	{
		/* Decide on approach, condition argument plans to suit. */
		if ( Gp_role == GP_ROLE_DISPATCH )
		{
			optype = choose_setop_type(agg_plans);
			adjust_setop_arguments(agg_plans, optype);
		}

		/* Generate the targetlist for Append node */
		targetlist = generate_append_tlist(context->tlist, 1);

		/* Append all agg_plans together */
		result_plan = (Plan *)make_append(agg_plans, false, targetlist);
		mark_append_locus(result_plan, optype); /* Mark the plan result locus. */

		/* set the final pathkey to NIL */
		context->current_pathkeys = NIL;
	}
	
	else
	{
		result_plan = (Plan *)linitial(agg_plans);
	}

	return result_plan;
}

/*
 * Generate the targetlist for an Append node based on the children targetlist.
 *
 * The entries in the Append's targetlist should always be simple Vars;
 * we just have to make sure they have the right datatypes and typmods.
 * The Vars are always generated with the given varno.
 */
static List *
generate_append_tlist(List *tlist, int varno)
{
	List *targetlist = NIL;
	ListCell *lc;
	int resno = 1;

	foreach (lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);
		Expr *new_expr;
		TargetEntry *new_tle;

		new_expr = (Expr *)makeVar(varno,
								   resno,
								   exprType((Node *) tle->expr),
								   exprTypmod((Node *) tle->expr),
								   0);
		new_tle = makeTargetEntry(new_expr,
								  (AttrNumber)resno,
								  tle->resname ? pstrdup(tle->resname) : NULL,
								  tle->resjunk);
		/* Keep the ressortgroupref */
		new_tle->ressortgroupref = tle->ressortgroupref;

		resno++;
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
	int num_subplans = 0;
	int subplan_no;

	/*
	 * Generate subplans for Agg nodes in each requested rollup level.
	 * Input sharing should be applied here by constructing appropriate
	 * subplans to be used.
	 */
	context->subplan = lefttree;
	context->input_locus = context->path->locus;
	context->pathkeys = NIL;
	for (group_no=0; group_no<context->current_rollup->numcols + 1; group_no++)
	{
		if (context->current_rollup->grpset_counts[group_no] > 0)
		{
			num_subplans++;
			
			/*
			 * XXX We use the same pathkeys for each Agg node here. This
			 * could be improved to use different pathkeys, because each
			 * Agg node does have different sorting requirements.
			 */
			context->pathkeys = lappend(context->pathkeys,
										context->current_pathkeys);
		}
	}

	subplans = generate_list_subplans(root, num_subplans, context);

	/* Append Grouping and GroupId into context->tlist */
	context->tlist = add_grouping_groupid(context->tlist);

	/* Generate an Agg/Group node for each rollup level */
	for (group_no=0, subplan_no=0; group_no<context->current_rollup->numcols + 1; group_no++)
	{
		if (group_no > 0)
			grouping += ( ((uint64)1) << context->current_rollup->grping_pos[
							 context->current_rollup->numcols - group_no]);

		if (context->current_rollup->grpset_counts[group_no] > 0)
		{
			Plan *agg_plan ;
			Plan *subplan = (Plan *)list_nth(subplans, subplan_no);
			subplan_no++;

			/* Add an explicit sort if we couldn't make the path
			 * come out the way the Agg/Group node needs it.
			 */
			if (!pathkeys_contained_in(root->group_pathkeys,
									   context->current_pathkeys))
			{
				subplan = (Plan *)
					make_sort_from_groupcols(root,
											 root->parse->groupClause,
											 context->grpColIdx,
											 false,
											 subplan);
				context->current_pathkeys = root->group_pathkeys;
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
									 group_no,
									 0,
									 grouping,
									 context->current_rollup->grpset_counts[group_no],
									 context->numGroupCols,
									 context->grpColIdx,
									 context->tlist,
									 context->qual,
									 subplan);

			agg_plans = lappend(agg_plans, agg_plan);
		}
	}

	return agg_plans;
}

/*
 * Create the first Agg node whose subplan is not an Agg node.
 *
 * Params:
 *    num_nullcols: the number of grouping colummns should not be considered
 *                  as a grouping column in this Agg node. For an input tuple
 *                  in this Agg node, the values for these grouping columns
 *                  will be set to NULL.
 *    input_grouping: the grouping value for input tuples.
 *    grouping: the grouping value for the Agg node.
 *    rollup_gs_times: the times that each aggregate result is outputed.
 *    groupColIdx: the grouping column.
 *    current_tlist: the targetlist for the Agg node to be generated.
 *    current_qual: the qual for the Agg node.
 *    current_lefttree: the subplan for the Agg node.
 *
 * Note that we always assume that groupColIdx includes GROUPING column
 * at the end, and context->numGroupCols specifies the number of grouping
 * columns. However, the result Agg node does not consider GROUPING column
 * because its subplan has no idea about this column.
 */
static Plan *
add_first_agg(PlannerInfo *root,
			  GroupExtContext *context,
			  int num_nullcols,
			  uint64 input_grouping,
			  uint64 grouping,
			  int rollup_gs_times,
			  int numGroupCols,
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
		(((double)(numGroupCols-num_nullcols))/((double)(numGroupCols))) *
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
		 * The parameter numGroupCols should have excluded the GROUPING column.
		 */
		agg_node = (Plan *)make_agg(root, current_tlist, current_qual, context->aggstrategy, false,
									numGroupCols, groupColIdx,
									lNumGroups, num_nullcols, input_grouping, grouping,
									rollup_gs_times,
									context->agg_counts->numAggs, context->agg_counts->transitionSpace,
									current_lefttree);
	}

	else
	{
		agg_node = (Plan *)make_group(root, current_tlist, current_qual,
									  numGroupCols,
									  groupColIdx, current_numGroups,
									  num_nullcols, input_grouping, grouping,
									  rollup_gs_times,
									  current_lefttree);
	}

	/* Pull up the Flow from the subplan */
	agg_node->flow =
		pull_up_Flow(agg_node, agg_node->lefttree,
					 context->aggstrategy == AGG_SORTED);

	/* Simply pulling up the Flow from the subplan is not enough.
	 * We set hash->hashExpr to NULL since we can not be sure that
	 * the order of all entries in hash->hashExpr will match with
	 * the agg_node->targetlist, which is required by the upper-level
	 * Agg nodes, see also mark_passthru_locus(). This can not
	 * be done by simplying re-order the entries in flow->hashExpr since
	 * some grouping columns may not be in flow->hashExpr at all.
	 */
	list_free_deep(agg_node->flow->hashExpr);
	agg_node->flow->hashExpr = NULL;

	return agg_node;
}

/*
 * Generate a list of subplans based on context->subplan.
 */
static List *
generate_list_subplans(PlannerInfo *root, int num_subplans,
					   GroupExtContext *context)
{
	int group_no;
	List *subplans = NIL;
	Cost share_cost;
	Cost copy_cost;

	/*
	 * We consider both input sharing plan and a list of common
	 * subplan copies, and choose the one which has the cheaper
	 * cost.
	 */
	copy_cost = num_subplans * context->subplan->total_cost;
	share_cost = cost_share_plan(context->subplan, num_subplans);

	if (copy_cost > share_cost)
		subplans = share_plan(root, context->subplan, num_subplans);
	else
	{
		for (group_no = 0; group_no < num_subplans; group_no++)
		{
			Plan *copy_plan = copyObject(context->subplan);
			subplans = lappend(subplans, copy_plan);
		}
	}

	list_free(context->pathkeys);
	context->pathkeys = NIL;

	for(group_no = 0; group_no < num_subplans; ++group_no)
	{
		List *pathkeys = copyObject(context->current_pathkeys);
		context->pathkeys = lappend(context->pathkeys, pathkeys);
	}

	return subplans;
}

static void
destroyGroupExtContext(GroupExtContext *context)
{
	if (context->sub_tlist)
		list_free(context->sub_tlist);
	if (context->canonical_rollups)
		list_free_deep(context->canonical_rollups);
}

/*
 * Append Grouping and GroupId into the given target list.
 */
static List *
add_grouping_groupid(List *tlist)
{
	TargetEntry *tle;
	Grouping *grping;
	GroupId *grpid;

	grping = makeNode(Grouping);
	tle = makeTargetEntry((Expr *)grping, list_length(tlist) + 1,
						  "grouping", true);
	tle->ressortgroupref = assignSortGroupRef(tle, tlist);
	tlist = lappend(tlist, tle);

	grpid = makeNode(GroupId);
	tle = makeTargetEntry((Expr *)grpid, list_length(tlist) + 1,
						  "group_id", true);
	tlist = lappend(tlist, tle);

	return tlist;
}

/*
 * Convert a given list of canonical grouping sets into multiple
 * canonical rollups.
 *
 * The input list of canonical grouping sets is sorted.
 */
static List *
convert_gs_to_rollups(AttrNumber *grpColIdx,
					  int numGroupCols,
					  CanonicalGroupingSets *canonical_grpsets,
					  List *canonical_rollups,
					  List *sub_tlist,
					  List *tlist)
{
	bool *used;
	bool more = true;
	Index *sortrefs_new_to_orig = (Index *)
		palloc0(numGroupCols * sizeof(Index));
	ListCell *sub_lc;

	if (canonical_grpsets == NULL ||
		canonical_grpsets->ngrpsets == 0)
		return NIL;

	used = (bool *)palloc0(canonical_grpsets->ngrpsets * sizeof(bool));

	/* In canonical grouping sets, we use tleSortGroupRef to represent
	 * a grouping column. These tleSortGroupRefs are also computed
	 * based on both GROUP BY and ORDER BY clauses. However, for
	 * sub_tlist, only GROUP BY clause is considered, so that
	 * tleSortGroupRefs may be different. Here, we construct a mapping
	 * from new ressortgrouprefs to original tleSortGroupRefs -- the array
	 * indexes representing new ressortgrouprefs, while the array
	 * elements representing orginal tleSortGroupRefs.
	 */
	foreach (sub_lc, sub_tlist)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(sub_lc);
		TargetEntry *orig_tle = NULL;
		ListCell *lc = NULL;

		if (tle->ressortgroupref == 0)
			continue;

		/* find tle in tlist */
		foreach (lc, tlist)
		{
			orig_tle = (TargetEntry *)lfirst(lc);
			if (equal(orig_tle->expr, tle->expr))
				break;
		}
		if (lc == NULL)
			ereport(ERROR, (
						errcode(ERRCODE_GP_INTERNAL_ERROR),
						errmsg("can find the entry in tlist")));

		sortrefs_new_to_orig[tle->ressortgroupref-1] =
			orig_tle->ressortgroupref;
	}

	/*
	 * XXX We use a simple greedy algorithm to make up the list
	 * of rollups. We scan over all grouping sets several times
	 * util there are no more grouping sets left. In each round,
	 * we pick up the maximum length of grouping sets for a
	 * single rollup. The grouping sets have been picked in the
	 * previous rounds are ignored in this round.
	 */
	while (more)
	{
		int grpsetno;
		int prev_grpsetno = -1;
		List *list_grpsetnos = NIL;
		ListCell *lc;
		int colno = 0;
		int size_rollup = sizeof(CanonicalRollup) +
			canonical_grpsets->num_distcols * sizeof(int);

		CanonicalRollup *rollup = (CanonicalRollup *)
			palloc0(size_rollup);
		rollup->numcols = canonical_grpsets->num_distcols;
		rollup->colIdx = (AttrNumber *)palloc0(rollup->numcols * sizeof(AttrNumber));
		rollup->grping_pos = (int *)palloc0(rollup->numcols * sizeof(int));
		rollup->ngrpsets = 0;

		more = false;

		for (grpsetno=0; grpsetno<canonical_grpsets->ngrpsets; grpsetno++)
		{
			if (used[grpsetno])
				continue;

			if (prev_grpsetno == -1 ||
				bms_is_subset(canonical_grpsets->grpsets[grpsetno],
							  canonical_grpsets->grpsets[prev_grpsetno]))
			{
				int curr_grpset;
				curr_grpset = canonical_grpsets->num_distcols -
					bms_num_members(canonical_grpsets->grpsets[grpsetno]);
				Assert(curr_grpset >= 0);

				rollup->grpset_counts[curr_grpset] =
					canonical_grpsets->grpset_counts[grpsetno];
				rollup->ngrpsets++;
				used[grpsetno] = true;

				list_grpsetnos = lcons_int(grpsetno, list_grpsetnos);

				prev_grpsetno = grpsetno;
			}

			else
				more = true;
		}

		/* Build colIdx based on the grouping sets in the current ROLLUP. */
		prev_grpsetno = -1;
		foreach(lc, list_grpsetnos)
		{
			grpsetno = lfirst_int(lc);

			if (prev_grpsetno == -1)
			{
				append_colIdx(rollup->colIdx, rollup->numcols, colno,
							  rollup->grping_pos, canonical_grpsets->grpsets[grpsetno],
							  grpColIdx, sortrefs_new_to_orig);
				prev_grpsetno = grpsetno;
				colno += bms_num_members(canonical_grpsets->grpsets[grpsetno]);
			}

			else
			{
				Bitmapset *bms_diff =
					bms_difference(canonical_grpsets->grpsets[grpsetno],
								   canonical_grpsets->grpsets[prev_grpsetno]);
				append_colIdx(rollup->colIdx, rollup->numcols, colno,
							  rollup->grping_pos, bms_diff, grpColIdx, sortrefs_new_to_orig);
				colno += bms_num_members(bms_diff);
				prev_grpsetno = grpsetno;
							  
			}
		}

		/* This rollup may not use all grouping columns in the user-defined
		 * grouping sets, we fill the blanks in colIdx with not-used columns.
		 */
		if (colno < rollup->numcols)
		{
			int grpcol_no;
			
			for (grpcol_no=0; grpcol_no<rollup->numcols && colno < rollup->numcols; grpcol_no++)
			{
				int rollup_colno;
				for (rollup_colno=0; rollup_colno<colno; rollup_colno++)
				{
					if (grpColIdx[grpcol_no] == rollup->colIdx[rollup_colno])
					{
						rollup->grping_pos[rollup_colno] = rollup->numcols - 1 - grpcol_no;
						break;
					}
				}
				
				if (rollup_colno == colno)
				{
					rollup->colIdx[colno] = grpColIdx[grpcol_no];
					rollup->grping_pos[colno] = rollup->numcols - 1 - grpcol_no;
					colno++;
				}
			}
		}

		canonical_rollups = lappend(canonical_rollups, rollup);
	}

	pfree(sortrefs_new_to_orig);

	return canonical_rollups;
}

/*
 * append_colIdx - append the elements in a Bitmapset into
 *   a given array, starting from colno.
 */
static void
append_colIdx(AttrNumber *colIdx, int numcols, int colno,
			  int *grping_pos, Bitmapset *bms, AttrNumber *grpColIdx,
			  Index *sortrefs_new_to_orig)
{
	int x;
	int pos;
	Assert(colno + bms_num_members(bms) <= numcols);

	/* x is the original tleSortGroupRef */
	x = bms_first_from(bms, 0);
	while (x >= 0)
	{
		Assert(x > 0);

		/* Find x in sortrefs_new_to_orig */
		for (pos=0; pos<numcols; pos++)
		{
			if (sortrefs_new_to_orig[pos] == x)
				break;
		}

		Assert(pos < numcols);

		colIdx[colno] = grpColIdx[pos];
		grping_pos[colno] = numcols - 1 - pos;
		colno++;
		x = bms_first_from(bms, x + 1);
	}
}

static Plan *
plan_list_rollup_plans(PlannerInfo *root,
					   GroupExtContext *context,
					   Plan *lefttree)
{
	Plan *result_plan;
	GpSetOpType optype = PSETOP_NONE;
	List *targetlist = NIL;
	List *rollup_plans = NIL;
	int rollup_no;
	List *subplans = NIL;
	List *orig_tlist = context->tlist;
	AttrNumber *orig_grpColIdx = context->grpColIdx;
	int orig_numGroupCols = context->numGroupCols;
	Query *orig_query = root->parse;
	Query *final_query = NULL;
	List *pathkeys = NIL;

	/* Generate the shared input subplans for all rollups. */
	context->subplan = lefttree;
	context->input_locus = context->path->locus;
	context->pathkeys = NIL;
	for (rollup_no=0; rollup_no<list_length(context->canonical_rollups); rollup_no++)
	{
		List *pathkeys = NIL;
		/* Make pathkeys for each rollup */
		pathkeys = make_pathkeys_for_rollup(root->parse->groupClause,
											context->tlist,
											context->grpColIdx,
											(CanonicalRollup *)list_nth(
												context->canonical_rollups, rollup_no));
		
		context->pathkeys =	lappend(context->pathkeys, pathkeys);
	}

	pathkeys = context->pathkeys;
	context->pathkeys = NIL;

	if (list_length(context->canonical_rollups) > 1)
	{
		subplans = generate_list_subplans(root,
										  list_length(context->canonical_rollups),
										  context);
	}
	else
		subplans = lappend(subplans, lefttree);

	context->tlist = NIL;

	for (rollup_no=0; rollup_no<list_length(context->canonical_rollups); rollup_no++)
	{
		Plan *subplan = (Plan *)list_nth(subplans, rollup_no);
		Plan *rollup_plan;

		context->current_rollup = (CanonicalRollup *)
			list_nth(context->canonical_rollups, rollup_no);

		context->current_pathkeys = list_nth(pathkeys, rollup_no);

		context->tlist = copyObject(orig_tlist);
		context->numGroupCols = orig_numGroupCols;
		context->grpColIdx =
			(AttrNumber *)palloc0(context->numGroupCols * sizeof(AttrNumber));
		memcpy(context->grpColIdx, orig_grpColIdx,
			   context->numGroupCols * sizeof(AttrNumber));
		root->parse = copyObject(orig_query);

		rollup_plan = make_aggs_for_rollup(root, context, subplan);

		if (rollup_no > 0 && list_length(context->canonical_rollups) > 1)
		{
			RangeTblEntry *newrte;
			List *subquery_tlist = NIL;
			ListCell *lc;

			/* addRangeTableEntryForSubquery requires the resnames in 
			 * the targetlist to be not NULL.
			 */
			foreach (lc, root->parse->targetList)
			{
				TargetEntry *tle = (TargetEntry *)lfirst(lc);
				if (tle->resname == NULL)
				{
					const char *fmt = "unnamed_attr_%d";
					char buf[32]; /* big enuf for fmt */
					sprintf(buf,fmt,tle->resno);
					tle->resname = pstrdup(buf);
				}
			}

			/* Since the range table for each rollup plan may be different,
			 * we build a new range table to include all range table entries.
			 */
			newrte = addRangeTableEntryForSubquery(NULL, root->parse,
												   makeAlias("rollup", NULL),
												   TRUE);
			final_query->rtable = lappend(final_query->rtable, newrte);
			subquery_tlist = generate_subquery_tlist(list_length(final_query->rtable),
													 rollup_plan->targetlist);
			rollup_plan = (Plan *)make_subqueryscan(subquery_tlist,
													NIL,
													list_length(final_query->rtable),
													rollup_plan);
			mark_passthru_locus(rollup_plan, true, true);
		}

		else
		{
			final_query = root->parse;
		}

		rollup_plans = lappend(rollup_plans, rollup_plan);
	}

	if (list_length(rollup_plans) > 1)
	{
		/* Decide on approach, condition argument plans to suit. */
		if ( Gp_role == GP_ROLE_DISPATCH )
		{
			optype = choose_setop_type(rollup_plans);
			adjust_setop_arguments(rollup_plans, optype);
		}

		/* Generate the targetlist for Append node */
		targetlist = generate_append_tlist(context->tlist, rollup_no + 1);

		/* Append all rollup_plans together */
		result_plan = (Plan *)make_append(rollup_plans, false, targetlist);
		mark_append_locus(result_plan, optype); /* Mark the plan result locus. */

		/* Set the final targetlist to Append's targetlist */
		context->tlist = targetlist;

		/* set the final pathkey to NIL */
		context->current_pathkeys = NIL;
	}
	
	else
	{
		Assert (list_length(rollup_plans) == 1);
		result_plan = (Plan *)linitial(rollup_plans);
	}

	/* Update the original Query node */
	root->parse = orig_query;
	memcpy(root->parse, final_query, sizeof(Query));

	return result_plan;
}

/*
 * Construct a new pathkeys for a given rollup.
 */
static List *
make_pathkeys_for_rollup(List *groupclause,
						 List *tlist,
						 AttrNumber *grpColIdx,
						 CanonicalRollup *rollup)
{
	int pos;
	int colno;
	List *pathkeys = NIL;
	List *current_pathkeys;
	current_pathkeys = make_pathkeys_for_groupclause(groupclause, tlist);
	
	for (colno=0; colno<rollup->numcols; colno++)
	{
		/* Find the position of rollup->colIdx[colno] in grpColIdx */
	    for (pos=0; pos<rollup->numcols; pos++)
		{
			if (rollup->colIdx[colno] == grpColIdx[pos])
				break;
		}

		Assert (pos < rollup->numcols);
		pathkeys = lappend(pathkeys, list_nth(current_pathkeys, pos));
	}

	return pathkeys;
}

