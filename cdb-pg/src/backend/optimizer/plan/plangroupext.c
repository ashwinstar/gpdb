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

#include "cdb/cdbsetop.h"					/* make_motion... routines */
#include "cdb/cdbvars.h"
#include "cdb/cdbpathlocus.h"
#include "cdb/cdbgroup.h" 					/* generate_three_tlists */
#include "cdb/cdbllize.h"                   /* pull_up_Flow */

typedef struct CanonicalRollup
{
	/* Define the number of columns in a rollup. The maximum number of
	 * grouping sets in a rollup is (numcols+1).
	 */
	int numcols;

	/* The column index for this rollup */
	AttrNumber *colIdx;

	/* Represent the number of times that each grouping set appears
	 * in a rollup. This should be the last one in this structure.
	 */
	int grpset_counts[1];
} CanonicalRollup;

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

	List *canonical_rollups; /* list of CanonicalRollups */

	/* the rollup this is currently being processed */
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
static List *generate_append_tlist(List *tlist);
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
static List *generate_list_subplans(PlannerInfo *root, GroupExtContext *context);
static List *add_grouping_groupid(List *tlist);
static void append_colIdx(AttrNumber *colIdx, int numcols, int colno,
						  Bitmapset *bms, AttrNumber *grpColIdx,
						  Index *sortrefs_new_to_orig);
static List *adjust_tlist(AttrNumber *orig_grpColIdx, AttrNumber *grpColIdx,
						  int numCols, List *tlist);

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
						List *qual,
						int *p_numGroupCols, AttrNumber **p_grpColIdx,
						AggClauseCounts *agg_counts,
						CanonicalGroupingSets *canonical_grpsets,
						double *p_dNumGroups,
						List **p_current_pathkeys,
						Plan *lefttree)
{
	GroupExtContext context;
	Plan *result_plan;
	ListCell *lc;
	List *tlist = copyObject(*p_tlist);

	context.path = path;
	context.p_tlist = &tlist;
	context.sub_tlist = copyObject(sub_tlist);
	context.qual = qual;
	context.numGroupCols = *p_numGroupCols;
	context.grpColIdx = palloc0(context.numGroupCols * sizeof(AttrNumber));
	memcpy(context.grpColIdx, *p_grpColIdx,
		   context.numGroupCols * sizeof(AttrNumber));
	context.agg_counts = agg_counts;
	context.p_dNumGroups = p_dNumGroups;
	context.p_current_pathkeys = p_current_pathkeys;
	context.is_agg = is_agg;
	context.twostage = twostage;
	context.canonical_rollups = NIL;

	/*
	 * Set ressortgroupref in all target entries in sub_tlist.
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
							  context.sub_tlist, *context.p_tlist);

	foreach (lc, context.canonical_rollups)
	{
		int group_no;
		ListCell *sub_lc;

		context.current_rollup = (CanonicalRollup *)lfirst(lc);

		/* Reset ressortgroupref in sub_tlist for each rollup. */
		foreach (sub_lc, context.sub_tlist)
		{
			TargetEntry *tle = (TargetEntry *)lfirst(sub_lc);
			for (group_no=0; group_no<context.numGroupCols; group_no++)
			{
				if (tle->resno == context.current_rollup->colIdx[group_no])
				{
					tle->ressortgroupref = group_no + 1;
				}
			}
		}


		/*
		 * XXX We need to plan for multiple rollups eventually. Currently,
		 * we know there is only one rollup.
		 */
		result_plan = make_aggs_for_rollup(root, &context, lefttree);
	}

	/* destroy the context */
	destroyGroupExtContext(&context);

	/* Set the proper resno for Grouping column in *p_grpColIdx.
	 */
	*p_grpColIdx =
		(AttrNumber *)repalloc(*p_grpColIdx,
							   (*p_numGroupCols + 1) * sizeof(AttrNumber));
	(*p_numGroupCols)++;

	*p_tlist = *(context.p_tlist);
	(*p_grpColIdx)[(*p_numGroupCols)-1] = list_length(*p_tlist) - 1;

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
	bool use_tlist2 = false; /* used to tell if we should free tlist2. */

	AttrNumber *groupColIdx;
	List *current_tlist;
	List *current_qual;
	bool need_finalagg = false;
	double current_numGroups;
	uint64 grouping = 0;
	double numGroups = *context->p_dNumGroups; /* make a copy of the original number */

	TargetEntry *tle;
	Grouping *grping;
	bool      only_oneagg = false;
	uint64    input_grouping;

	generate_three_tlists(*context->p_tlist, context->twostage,
						  context->sub_tlist, (Node *)context->qual,
						  context->numGroupCols,
						  context->grpColIdx,
						  &tlist1, &tlist2, &tlist3, &qual2, &qual3);

	/* Even though we use context->grpColIdx to generate
	 * these tlists, but the order of grouping columns are defined
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

	pfree(context->grpColIdx);
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


	/*
	 * Find the index position for the last rollup level. This is used
	 * to determine if we want to use tlist3 or tlist2 for the intermediate
	 * Agg node in a complete rollup.
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
			grouping += (1 << (group_no-1));

		if (context->current_rollup->grpset_counts[group_no] > 0)
		{
			AggStrategy aggstrategy;
			groupColIdx = context->grpColIdx;
			current_qual = context->qual;

			if (group_no == last_group_no)
			{
				*(context->p_tlist) = add_grouping_groupid(*(context->p_tlist));
				current_tlist = *context->p_tlist;
				prelimGroupColIdx[context->numGroupCols-1] = list_length(current_tlist) - 1;

				/* We know that we only have one rollup level for this query.
				 * If later in this function, we find out that we need another
				 * Agg node to bring back the aggregate results, we want to make
				 * sure that all Aggrefs in context->p_tlist are AGGSTAGE_PARTIAL.
				 */
				only_oneagg = true;
			}
			else
				current_tlist = tlist1;

			/* Add sort node if needed, and set AggStrategy */
			if (root->parse->groupClause)
			{
				if (!pathkeys_contained_in(root->group_pathkeys,
										   *(context->p_current_pathkeys)))
				{
					current_lefttree = (Plan *)
						make_sort_from_groupcols(root,
												 root->parse->groupClause,
												 groupColIdx,
												 false,
												 current_lefttree);
					*(context->p_current_pathkeys) = root->group_pathkeys;
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
				*(context->p_current_pathkeys) = NIL;
			}

			context->aggstrategy = aggstrategy;

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
			*context->p_tlist = current_tlist;

			input_grouping = grouping;
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

	else
		need_finalagg = true;

	/*
	 * We have to make sure all Aggrefs in context->p_tlist are AGGSTAGE_PARTIAL.
	 */
	if (only_oneagg && need_finalagg)
	{
		ListCell *lc;

		foreach (lc, *context->p_tlist)
		{
			TargetEntry *tle = (TargetEntry *)lfirst(lc);
			if (IsA(tle->expr, Aggref))
			{
				Aggref *aggref = (Aggref *)tle->expr;

				aggref->aggstage = AGGSTAGE_PARTIAL;
			}
		}
	}

	/* Construct the rest Agg nodes. */
	for (; group_no <= last_group_no; group_no++)
	{
		if (group_no > 0)
			grouping += (1 << (group_no-1));

		if (context->current_rollup->grpset_counts[group_no] <= 0)
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
										group_no, input_grouping, grouping,
										context->current_rollup->grpset_counts[group_no],
										current_numGroups,
										context->agg_counts->numAggs, context->agg_counts->transitionSpace,
										"rollup",
										context->p_current_pathkeys,
										agg_node);

		/* Set inputHasGrouping */
		if (context->is_agg)
			((Agg *)agg_node)->inputHasGrouping = true;
		else
			((Group *)agg_node)->inputHasGrouping = true;

		current_lefttree = agg_node;
		*context->p_tlist = current_tlist;
		input_grouping = grouping;
	}

	/*
	 * If we need to bring back aggregate results from different segments,
	 * we add a Gather motion node on top of the top Agg node to do the
	 * final aggregation.
	 */
	if (need_finalagg && !context->twostage)
	{
		long lNumGroups;
		Flow *flow;

		double motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
			gp_motion_cost_per_row :
			2.0 * cpu_tuple_cost;

		/* We need to add Grouping into agg_node->flow so that it will
		 * be in Gather merge key.
		 */
		flow = agg_node->flow;
		if (flow->numSortCols > 0)
		{
			flow->numSortCols++;
			if (flow->numSortCols != context->numGroupCols)
				ereport(ERROR, (
							errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Group keys not equal to sort keys") ));
			flow->sortColIdx = repalloc(flow->sortColIdx,
										flow->numSortCols * sizeof(AttrNumber));
			flow->sortColIdx[flow->numSortCols-1] = prelimGroupColIdx[flow->numSortCols-1];
			flow->sortOperators = repalloc(flow->sortOperators,
										   flow->numSortCols * sizeof(Oid));
			flow->sortOperators[flow->numSortCols-1] =
				ordering_oper_opid(exprType((Node *)grping));
		}

		agg_node = (Plan *)make_motion_gather_to_QE(agg_node, true);
		agg_node->total_cost += motion_cost_per_row * agg_node->plan_rows;

		/* Convert # groups to long int --- but 'ware overflow! */
		lNumGroups = (long) Min(*context->p_dNumGroups, (double) LONG_MAX);

		/* Add a Agg/Group node */
		agg_node = add_second_stage_agg(root, context->is_agg, *context->p_tlist, tlist3,
										qual3, context->aggstrategy,
										context->numGroupCols, prelimGroupColIdx,
										0,  /* num_nullcols */
										0, /* input_grouping */
										0, /* grouping */
										0, /* rollup_gs_times */
										lNumGroups, context->agg_counts->numAggs,
										context->agg_counts->transitionSpace,
										"rollup", context->p_current_pathkeys, agg_node);

		if (context->is_agg)
			((Agg *)agg_node)->inputHasGrouping = true;
		else
			((Group *)agg_node)->inputHasGrouping = true;

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

	/* Add an explicit sort if we couldn't make the path
	 * come out the way the Agg/Group node needs it.
	 */
	if (!pathkeys_contained_in(root->group_pathkeys,
							   *context->p_current_pathkeys))
	{
		lefttree = (Plan *)
			make_sort_from_groupcols(root,
									 root->parse->groupClause,
									 context->grpColIdx,
									 false,
									 lefttree);
		*context->p_current_pathkeys = root->group_pathkeys;
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
	for (group_no=0; group_no<context->current_rollup->numcols + 1; group_no++)
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

	/* Append Grouping and GroupId into context->p_tlist */
	*(context->p_tlist) = add_grouping_groupid(*(context->p_tlist));

	/* Generate an Agg/Group node for each rollup level */
	for (group_no=0; group_no<context->current_rollup->numcols + 1; group_no++)
	{
		Plan *subplan = (Plan *)list_nth(subplans, group_no);

		if (group_no > 0)
			grouping += (1 << (group_no-1));

		if (context->current_rollup->grpset_counts[group_no] > 0)
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
											 false,
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
									 group_no,
									 0,
									 grouping,
									 context->current_rollup->grpset_counts[group_no],
									 context->numGroupCols,
									 context->grpColIdx,
									 *context->p_tlist,
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
 *    current_tlist: the targetlist for the Agg node to be generated.
 *    current_qual: the qual for the Agg node.
 *    current_lefttree: the subplan for the Agg node.
 *    rollup_level: the rollup_level for the Agg node.
 *    grouping: the grouping value for the Agg node.
 *    rollup_gs_times: the times that each aggregate result is outputed.
 *    groupColIdx: the grouping column.
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
		agg_node = (Plan *)make_agg(root, current_tlist, current_qual, context->aggstrategy,
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

	ShareNode_ShareType stype = SHARENODE_UNKNOWN;

	if(gp_window_shareinput && context->numGroupCols > 0)
	{
		stype = add_sharenode(root,
				&context->subplan,
				&context->pathkeys,
				context->input_locus,
				(void *) context,
				context->numGroupCols + 1,
				NULL,
				NULL);
	}
 
	if(stype == SHARENODE_UNKNOWN)
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
	int size_rollup = sizeof(CanonicalRollup) +
		canonical_grpsets->num_distcols * sizeof(int);
	Index *sortrefs_new_to_orig = (Index *)
		palloc0(numGroupCols * sizeof(Index));
	ListCell *sub_lc;

	if (canonical_grpsets == NULL ||
		canonical_grpsets->ngrpsets == 0)
		return NIL;

	used = (bool *)palloc0(canonical_grpsets->ngrpsets * sizeof(bool));

	/* In canonical grouping sets, we use tleSortGroupRef to represent
	 * a grouping column. These tleSortGroupRefs are also affected by
	 * ORDER BY clauses. However, for sub_tlist, we may re-order
	 * grouping columns since we only consider GROUP BY clauses.
	 * So we construct a mapping from new ressortgrouprefs to
	 * original tleSortGroupRefs -- the array indexes representing
	 * new ressortgrouprefs, while the array elements representing
	 * orginal tleSortGroupRefs.
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
	 *
	 * Note that we also ignore some variation of possible single
	 * rollups for grouping sets. For example,
	 *     GROUPING SETS ((vn,cn), (cn), ())
	 * is a single rollup, but we ignore it here for now.
	 */
	while (more)
	{
		int grpsetno;
		int prev_grpsetno = -1;
		List *list_grpsetnos = NIL;
		ListCell *lc;
		int colno = 0;

		CanonicalRollup *rollup = (CanonicalRollup *)
			palloc0(size_rollup);
		rollup->numcols = canonical_grpsets->num_distcols;
		rollup->colIdx = (AttrNumber *)palloc0(rollup->numcols * sizeof(AttrNumber));

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
							  canonical_grpsets->grpsets[grpsetno],
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
							  bms_diff, grpColIdx, sortrefs_new_to_orig);
				colno += bms_num_members(bms_diff);
				prev_grpsetno = grpsetno;
							  
			}
		}

		canonical_rollups = lappend(canonical_rollups, rollup);
	}

	pfree(sortrefs_new_to_orig);

	/*
	 * XXX Only consider a single rollup now.
	 */
	if (list_length(canonical_rollups) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Grouping extensions \"CUBE\" or "
						"\"GROUPING SETS\" not supported.")));

	return canonical_rollups;
}

/*
 * append_colIdx - append the elements in a Bitmapset into
 *   a given array, starting from colno.
 */
static void
append_colIdx(AttrNumber *colIdx, int numcols, int colno,
			  Bitmapset *bms, AttrNumber *grpColIdx,
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

		colIdx[colno++] = grpColIdx[pos];
		x = bms_first_from(bms, x + 1);
	}
}

static List *
adjust_tlist(AttrNumber *orig_grpColIdx, AttrNumber *grpColIdx,
			 int numCols, List *tlist)
{
	List *new_tlist = NIL;
	int entry_no;

	for (entry_no=0; entry_no<list_length(tlist); entry_no++)
	{
		TargetEntry *new_tle;

		/* the resno in the orig_grpColIdx */
		int resno = orig_grpColIdx[entry_no];

		/* Find the index position of resno in grpColIdx */
		int pos;
		for (pos=0; pos<numCols; pos++)
		{
			if (grpColIdx[pos] == resno)
				break;
		}
		
		if (pos < numCols)
		{
			TargetEntry *tle = (TargetEntry *)list_nth(tlist, pos);
			new_tle = makeTargetEntry(copyObject(tle->expr),
									  list_length(new_tlist) + 1,
									  tle->resname, tle->resjunk);
		}

		else
		{
			TargetEntry *tle = (TargetEntry *)list_nth(tlist, entry_no);
			Assert(tle->resno == entry_no+1);
			new_tle = copyObject(tle);
		}
		new_tlist = lappend(new_tlist, new_tle);
	}

	return new_tlist;
}
