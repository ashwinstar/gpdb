/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/planner.c,v 1.209 2006/10/04 00:29:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/execHashagg.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
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
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/syscache.h"
#include "nodes/bitmapset.h"

#include "cdb/cdbllize.h"
#include "cdb/cdbmutate.h" 	/* apply_shareinput */
#include "cdb/cdbpath.h"        /* cdbpath_segments */
#include "cdb/cdbpathtoplan.h"  /* cdbpathtoplan_create_flow() */
#include "cdb/cdbgroup.h" /* grouping_planner extensions */
#include "cdb/cdbsetop.h" /* motion utilities */
#include "cdb/cdbsubselect.h"   /* cdbsubselect_flatten_sublinks() */

#include "utils/debugbreak.h"

ParamListInfo PlannerBoundParamList = NULL;		/* current boundParams */


/* Expression kind codes for preprocess_expression */
#define EXPRKIND_QUAL		0
#define EXPRKIND_TARGET		1
#define EXPRKIND_RTFUNC		2
#define EXPRKIND_VALUES		3
#define EXPRKIND_LIMIT		4
#define EXPRKIND_ININFO		5
#define EXPRKIND_APPINFO	6

static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static Plan *inheritance_planner(PlannerInfo *root);
static Plan *grouping_planner(PlannerInfo *root, double tuple_fraction);
static bool is_dummy_plan(Plan *plan);
static double preprocess_limit(PlannerInfo *root,
				 double tuple_fraction,
				 int64 *offset_est, int64 *count_est);
static bool choose_hashed_grouping(PlannerInfo *root, double tuple_fraction,
					   Path *cheapest_path, Path *sorted_path,
					   double dNumGroups, AggClauseCounts *agg_counts);
static bool hash_safe_grouping(PlannerInfo *root);
static List *make_subplanTargetList(PlannerInfo *root, List *tlist,
					   AttrNumber **groupColIdx, bool *need_tlist_eval);
static void locate_grouping_columns(PlannerInfo *root,
						List *tlist,
						List *sub_tlist,
						AttrNumber *groupColIdx);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);

static Bitmapset* canonicalize_colref_list(Node * node);
static List *canonicalize_gs_list(List *gsl, bool ordinary);
static List *rollup_gs_list(List *gsl);
static List *add_gs_combinations(List *list, int n, int i, Bitmapset **base, Bitmapset **work);
static List *cube_gs_list(List *gsl);
static CanonicalGroupingSets *make_canonical_groupingsets(List *groupClause);
static int gs_compare(const void *a, const void*b);
static void sort_canonical_gs_list(List *gs, int *p_nsets, Bitmapset ***p_sets);

/*****************************************************************************
 *
 *	   Query optimizer entry point
 *
 *****************************************************************************/
Plan *
planner(Query *parse, bool isCursor, int cursorOptions,
		ParamListInfo boundParams)
{
	double		tuple_fraction;
	Plan	   *result_plan;
	Index		save_PlannerQueryLevel;
	List	   *save_PlannerParamList;
	ParamListInfo save_PlannerBoundParamList;
	ApplyShareInputContext shareinput_context;
	set_plan_ref_context setref_context;

	/*
	 * The planner can be called recursively (an example is when
	 * eval_const_expressions tries to pre-evaluate an SQL function). So,
	 * these global state variables must be saved and restored.
	 *
	 * Query level and the param list cannot be moved into the per-query
	 * PlannerInfo structure since their whole purpose is communication across
	 * multiple sub-queries. Also, boundParams is explicitly info from outside
	 * the query, and so is likewise better handled as a global variable.
	 *
	 * Note we do NOT save and restore PlannerPlanId: it exists to assign
	 * unique IDs to SubPlan nodes, and we want those IDs to be unique for the
	 * life of a backend.  Also, PlannerInitPlan is saved/restored in
	 * subquery_planner, not here.
	 */
	save_PlannerQueryLevel = PlannerQueryLevel;
	save_PlannerParamList = PlannerParamList;
	save_PlannerBoundParamList = PlannerBoundParamList;

	/* Initialize state for handling outer-level references and params */
	PlannerQueryLevel = 0;		/* will be 1 in top-level subquery_planner */
	PlannerParamList = NIL;
	PlannerBoundParamList = boundParams;

	/* Determine what fraction of the plan is likely to be scanned */
	if (isCursor)
	{
		/*
		 * We have no real idea how many tuples the user will ultimately FETCH
		 * from a cursor, but it seems a good bet that he doesn't want 'em
		 * all.  Optimize for 10% retrieval (you gotta better number?  Should
		 * this be a SETtable parameter?)
		 */
		tuple_fraction = 0.10;
	}
	else
	{
		/* Default assumption is we need all the tuples */
		tuple_fraction = 0.0;
	}

	/* primary planning entry point (may recurse for subqueries) */
	result_plan = subquery_planner(parse, tuple_fraction, NULL);

	/* check we popped out the right number of levels */
	Assert(PlannerQueryLevel == 0);

	/*
	 * If creating a plan for a scrollable cursor, make sure it can run
	 * backwards on demand.  Add a Material node at the top at need.
	 */
	if (isCursor && (cursorOptions & CURSOR_OPT_SCROLL))
	{
		if (!ExecSupportsBackwardScan(result_plan))
			result_plan = materialize_finished_plan(result_plan);
	}

	/* Fix sharing id and shared id.  
	 *
	 * This must be called before set_plan_references and cdbparallelize.  The other mutator
	 * or tree walker assumes the input is a tree.  If there is plan sharing, we have a DAG. 
	 *
	 * apply_shareinput will fix shared_id, and change the DAG to a tree.
	 */
	shareinput_context.sharedNodes = NULL;
	shareinput_context.sliceMarks = NULL;
	shareinput_context.motStack = NULL;

	result_plan = apply_shareinput_dag_to_tree(result_plan, &shareinput_context); 

	setref_context.qry = parse;
	setref_context.rtable = parse->rtable;
	setref_context.sharedNodes = shareinput_context.sharedNodes;

	/* final cleanup of the plan */
	result_plan = set_plan_references_qry(result_plan, &setref_context);

	/* executor wants to know total number of Params used overall */
	result_plan->nParamExec = list_length(PlannerParamList);

	if ( Gp_role == GP_ROLE_DISPATCH )
	{
		bool uses_correlation = has_correlation_params(PlannerParamList);
		
		result_plan = cdbparallelize(result_plan, parse,
									isCursor, cursorOptions,
									boundParams, uses_correlation);

		/* cdbparallelize may create additional slices that may affect share input.
		 * need to mark material nodes that are split acrossed multi slices.
		 */
		result_plan = apply_shareinput_xslice(result_plan, &shareinput_context);
	}

	/* restore state for outer planner, if any */
	PlannerQueryLevel = save_PlannerQueryLevel;
	PlannerParamList = save_PlannerParamList;
	PlannerBoundParamList = save_PlannerBoundParamList;

	return result_plan;
}


/*--------------------
 * subquery_planner
 *	  Invokes the planner on a subquery.  We recurse to here for each
 *	  sub-SELECT found in the query tree.
 *
 * parse is the querytree produced by the parser & rewriter.
 * tuple_fraction is the fraction of tuples we expect will be retrieved.
 * tuple_fraction is interpreted as explained for grouping_planner, below.
 *
 * If subquery_pathkeys isn't NULL, it receives a list of pathkeys indicating
 * the output sort ordering of the completed plan.
 *
 * Basically, this routine does the stuff that should only be done once
 * per Query object.  It then calls grouping_planner.  At one time,
 * grouping_planner could be invoked recursively on the same Query object;
 * that's not currently true, but we keep the separation between the two
 * routines anyway, in case we need it again someday.
 *
 * subquery_planner will be called recursively to handle sub-Query nodes
 * found within the query's expressions and rangetable.
 *
 * Returns a query plan.
 *--------------------
 */
Plan *
subquery_planner(Query *parse, double tuple_fraction,
				 List **subquery_pathkeys)
{
	List	   *saved_initplan = PlannerInitPlan;
	int			saved_planid = PlannerPlanId;
	PlannerInfo *root;
	Plan	   *plan;
	List	   *newHaving;
	ListCell   *l;

	/* Set up for a new level of subquery */
	PlannerQueryLevel++;
	PlannerInitPlan = NIL;

	/* Create a PlannerInfo data structure for this subquery */
	root = makeNode(PlannerInfo);
	root->parse = parse;
	root->in_info_list = NIL;
	root->append_rel_list = NIL;

    /* CDB: Initialize globals */
    mpp_trying_fallback_plan = false;
    if (Gp_role != GP_ROLE_DISPATCH)
        cdbpath_segments = 1;
    else
    {
        int segmentCount = getgpsegmentCount();

        if (gp_segments_for_planner > 0)
            cdbpath_segments = gp_segments_for_planner;
        else
            cdbpath_segments = segmentCount;

        /* Choose a segdb to which our singleton gangs should be dispatched. */
        if (segmentCount > 0)
            gp_singleton_segindex = gp_session_id % segmentCount;
    }

    /* CDB: Stash current query level's relids before pulling up subqueries. */
    root->currlevel_relids = get_relids_in_jointree((Node *)parse->jointree);

    /*
	 * Look for IN clauses at the top level of WHERE, and transform them into
	 * joins.  Note that this step only handles IN clauses originally at top
	 * level of WHERE; if we pull up any subqueries in the next step, their
	 * INs are processed just before pulling them up.
	 */
	if (parse->hasSubLinks)
        cdbsubselect_flatten_sublinks(root, (Node *)parse);

	/*
	 * Check to see if any subqueries in the rangetable can be merged into
	 * this query.
	 */
	parse->jointree = (FromExpr *)
		pull_up_subqueries(root, (Node *) parse->jointree, false, false);

	/*
	 * Detect whether any rangetable entries are RTE_JOIN kind; if not, we can
	 * avoid the expense of doing flatten_join_alias_vars().  Also check for
	 * outer joins --- if none, we can skip reduce_outer_joins() and some
	 * other processing.  This must be done after we have done
	 * pull_up_subqueries, of course.
	 *
	 * Note: if reduce_outer_joins manages to eliminate all outer joins,
	 * root->hasOuterJoins is not reset currently.	This is OK since its
	 * purpose is merely to suppress unnecessary processing in simple cases.
	 */
	root->hasJoinRTEs = false;
	root->hasOuterJoins = false;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_JOIN)
		{
			root->hasJoinRTEs = true;
			if (IS_OUTER_JOIN(rte->jointype))
			{
				root->hasOuterJoins = true;
				/* Can quit scanning once we find an outer join */
				break;
			}
		}
	}

	/*
	 * Expand any rangetable entries that are inheritance sets into "append
	 * relations".  This can add entries to the rangetable, but they must be
	 * plain base relations not joins, so it's OK (and marginally more
	 * efficient) to do it after checking for join RTEs.  We must do it after
	 * pulling up subqueries, else we'd fail to handle inherited tables in
	 * subqueries.
	 */
	expand_inherited_tables(root);

    /* CDB: If parent RTE belongs to current query level, children do too. */
    foreach (l, root->append_rel_list)
    {
        AppendRelInfo  *appinfo = (AppendRelInfo *)lfirst(l);

        if (bms_is_member(appinfo->parent_relid, root->currlevel_relids))
            root->currlevel_relids = bms_add_member(root->currlevel_relids, 
                                                    appinfo->child_relid);
    }

	/*
	 * Set hasHavingQual to remember if HAVING clause is present.  Needed
	 * because preprocess_expression will reduce a constant-true condition to
	 * an empty qual list ... but "HAVING TRUE" is not a semantic no-op.
	 */
	root->hasHavingQual = (parse->havingQual != NULL);

	/* Clear this flag; might get set in distribute_qual_to_rels */
	root->hasPseudoConstantQuals = false;

	/*
	 * Do expression preprocessing on targetlist and quals.
	 */
	parse->targetList = (List *)
		preprocess_expression(root, (Node *) parse->targetList,
							  EXPRKIND_TARGET);

	parse->returningList = (List *)
		preprocess_expression(root, (Node *) parse->returningList,
							  EXPRKIND_TARGET);

	preprocess_qual_conditions(root, (Node *) parse->jointree);

	parse->havingQual = preprocess_expression(root, parse->havingQual,
											  EXPRKIND_QUAL);

	parse->limitOffset = preprocess_expression(root, parse->limitOffset,
											   EXPRKIND_LIMIT);
	parse->limitCount = preprocess_expression(root, parse->limitCount,
											  EXPRKIND_LIMIT);

	root->in_info_list = (List *)
		preprocess_expression(root, (Node *) root->in_info_list,
							  EXPRKIND_ININFO);
	root->append_rel_list = (List *)
		preprocess_expression(root, (Node *) root->append_rel_list,
							  EXPRKIND_APPINFO);

	/* Also need to preprocess expressions for function and values RTEs */
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_FUNCTION)
			rte->funcexpr = preprocess_expression(root, rte->funcexpr,
												  EXPRKIND_RTFUNC);
		else if (rte->rtekind == RTE_VALUES)
			rte->values_lists = (List *)
				preprocess_expression(root, (Node *) rte->values_lists,
									  EXPRKIND_VALUES);
	}

	/*
	 * In some cases we may want to transfer a HAVING clause into WHERE. We
	 * cannot do so if the HAVING clause contains aggregates (obviously) or
	 * volatile functions (since a HAVING clause is supposed to be executed
	 * only once per group).  Also, it may be that the clause is so expensive
	 * to execute that we're better off doing it only once per group, despite
	 * the loss of selectivity.  This is hard to estimate short of doing the
	 * entire planning process twice, so we use a heuristic: clauses
	 * containing subplans are left in HAVING.	Otherwise, we move or copy the
	 * HAVING clause into WHERE, in hopes of eliminating tuples before
	 * aggregation instead of after.
	 *
	 * If the query has explicit grouping then we can simply move such a
	 * clause into WHERE; any group that fails the clause will not be in the
	 * output because none of its tuples will reach the grouping or
	 * aggregation stage.  Otherwise we must have a degenerate (variable-free)
	 * HAVING clause, which we put in WHERE so that query_planner() can use it
	 * in a gating Result node, but also keep in HAVING to ensure that we
	 * don't emit a bogus aggregated row. (This could be done better, but it
	 * seems not worth optimizing.)
	 *
	 * Note that both havingQual and parse->jointree->quals are in
	 * implicitly-ANDed-list form at this point, even though they are declared
	 * as Node *.
	 */
	newHaving = NIL;
	foreach(l, (List *) parse->havingQual)
	{
		Node	   *havingclause = (Node *) lfirst(l);

		if (contain_agg_clause(havingclause) ||
			contain_volatile_functions(havingclause) ||
			contain_subplans(havingclause))
		{
			/* keep it in HAVING */
			newHaving = lappend(newHaving, havingclause);
		}
		else if (parse->groupClause && 
				 !contain_extended_grouping(parse->groupClause))
		{
			/* move it to WHERE */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals, havingclause);
		}
		else
		{
			/* put a copy in WHERE, keep it in HAVING */
			parse->jointree->quals = (Node *)
				lappend((List *) parse->jointree->quals,
						copyObject(havingclause));
			newHaving = lappend(newHaving, havingclause);
		}
	}
	parse->havingQual = (Node *) newHaving;

	/*
	 * If we have any outer joins, try to reduce them to plain inner joins.
	 * This step is most easily done after we've done expression
	 * preprocessing.
	 */
	if (root->hasOuterJoins)
		reduce_outer_joins(root);

	/*
	 * Do the main planning.  If we have an inherited target relation, that
	 * needs special processing, else go straight to grouping_planner.
	 */
	if (parse->resultRelation &&
		rt_fetch(parse->resultRelation, parse->rtable)->inh)
		plan = inheritance_planner(root);
	else
		plan = grouping_planner(root, tuple_fraction);

	/*
	 * If any subplans were generated, or if we're inside a subplan, build
	 * initPlan list and extParam/allParam sets for plan nodes, and attach the
	 * initPlans to the top plan node.
	 */
	if (PlannerPlanId != saved_planid || PlannerQueryLevel > 1)
		SS_finalize_plan(plan, parse->rtable);

	/* Return sort ordering info if caller wants it */
	if (subquery_pathkeys)
		*subquery_pathkeys = root->query_pathkeys;

	/* Return to outer subquery context */
	PlannerQueryLevel--;
	PlannerInitPlan = saved_initplan;
	/* we do NOT restore PlannerPlanId; that's not an oversight! */

	return plan;
}

/*
 * preprocess_expression
 *		Do subquery_planner's preprocessing work for an expression,
 *		which can be a targetlist, a WHERE clause (including JOIN/ON
 *		conditions), or a HAVING clause.
 */
static Node *
preprocess_expression(PlannerInfo *root, Node *expr, int kind)
{
	/*
	 * Fall out quickly if expression is empty.  This occurs often enough to
	 * be worth checking.  Note that null->null is the correct conversion for
	 * implicit-AND result format, too.
	 */
	if (expr == NULL)
		return NULL;

	/*
	 * If the query has any join RTEs, replace join alias variables with
	 * base-relation variables. We must do this before sublink processing,
	 * else sublinks expanded out from join aliases wouldn't get processed. We
	 * can skip it in VALUES lists, however, since they can't contain any Vars
	 * at all.
	 */
	if (root->hasJoinRTEs && kind != EXPRKIND_VALUES)
		expr = flatten_join_alias_vars(root, expr);

	/*
	 * Simplify constant expressions.
	 *
	 * Note: this also flattens nested AND and OR expressions into N-argument
	 * form.  All processing of a qual expression after this point must be
	 * careful to maintain AND/OR flatness --- that is, do not generate a tree
	 * with AND directly under AND, nor OR directly under OR.
	 *
	 * Because this is a relatively expensive process, we skip it when the
	 * query is trivial, such as "SELECT 2+2;" or "INSERT ... VALUES()". The
	 * expression will only be evaluated once anyway, so no point in
	 * pre-simplifying; we can't execute it any faster than the executor can,
	 * and we will waste cycles copying the tree.  Notice however that we
	 * still must do it for quals (to get AND/OR flatness); and if we are in a
	 * subquery we should not assume it will be done only once.
	 *
	 * For VALUES lists we never do this at all, again on the grounds that we
	 * should optimize for one-time evaluation.
	 */
	if (kind != EXPRKIND_VALUES &&
		(root->parse->jointree->fromlist != NIL ||
		 kind == EXPRKIND_QUAL ||
		 PlannerQueryLevel > 1))
		expr = eval_const_expressions(expr);

	/*
	 * If it's a qual or havingQual, canonicalize it.
	 */
	if (kind == EXPRKIND_QUAL)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/* Expand SubLinks to SubPlans */
	if (root->parse->hasSubLinks)
		expr = SS_process_sublinks(expr, (kind == EXPRKIND_QUAL));

	/*
	 * XXX do not insert anything here unless you have grokked the comments in
	 * SS_replace_correlation_vars ...
	 */

	/* Replace uplevel vars with Param nodes (this IS possible in VALUES) */
	if (PlannerQueryLevel > 1)
		expr = SS_replace_correlation_vars(expr);

	/*
	 * If it's a qual or havingQual, convert it to implicit-AND format. (We
	 * don't want to do this before eval_const_expressions, since the latter
	 * would be unable to simplify a top-level AND correctly. Also,
	 * SS_process_sublinks expects explicit-AND format.)
	 */
	if (kind == EXPRKIND_QUAL)
		expr = (Node *) make_ands_implicit((Expr *) expr);

	return expr;
}

/*
 * preprocess_qual_conditions
 *		Recursively scan the query's jointree and do subquery_planner's
 *		preprocessing work on each qual condition found therein.
 */
static void
preprocess_qual_conditions(PlannerInfo *root, Node *jtnode)
{
	ListCell   *l;

	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;

		foreach(l, f->fromlist)
			preprocess_qual_conditions(root, lfirst(l));

		f->quals = preprocess_expression(root, f->quals, EXPRKIND_QUAL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_qual_conditions(root, j->larg);
		preprocess_qual_conditions(root, j->rarg);

        foreach(l, j->subqfromlist)
		    preprocess_qual_conditions(root, lfirst(l));

		j->quals = preprocess_expression(root, j->quals, EXPRKIND_QUAL);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * inheritance_planner
 *	  Generate a plan in the case where the result relation is an
 *	  inheritance set.
 *
 * We have to handle this case differently from cases where a source relation
 * is an inheritance set. Source inheritance is expanded at the bottom of the
 * plan tree (see allpaths.c), but target inheritance has to be expanded at
 * the top.  The reason is that for UPDATE, each target relation needs a
 * different targetlist matching its own column set.  Also, for both UPDATE
 * and DELETE, the executor needs the Append plan node at the top, else it
 * can't keep track of which table is the current target table.  Fortunately,
 * the UPDATE/DELETE target can never be the nullable side of an outer join,
 * so it's OK to generate the plan this way.
 *
 * Returns a query plan.
 */
static Plan *
inheritance_planner(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Index       parentRTindex = parse->resultRelation;
	List	   *subplans = NIL;
	List	   *resultRelations = NIL;
	List	   *returningLists = NIL;
	List	   *rtable = NIL;
	List	   *tlist = NIL;
	PlannerInfo subroot;
	ListCell   *l;

	/* MPP */
	Plan *plan;
	CdbLocusType append_locustype = CdbLocusType_Null;
	bool locus_ok = TRUE;

	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		Plan	   *subplan;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/*
		 * Generate modified query with this rel as target.  We have to be
		 * prepared to translate varnos in in_info_list as well as in the
		 * Query proper.
		 */
		memcpy(&subroot, root, sizeof(PlannerInfo));
		subroot.parse = (Query *)
			adjust_appendrel_attrs((Node *) parse,
								   appinfo);
		subroot.in_info_list = (List *)
			adjust_appendrel_attrs((Node *) root->in_info_list,
								   appinfo);
		/* There shouldn't be any OJ info to translate, as yet */
		Assert(subroot.oj_info_list == NIL);

		/* Generate plan */
		subplan = grouping_planner(&subroot, 0.0 /* retrieve all tuples */ );

		/*
		 * If this child rel was excluded by constraint exclusion, exclude it
		 * from the plan.
		 *
		 * MPP-1544: perform this check before testing for loci compatibility
		 * we might have inserted a dummy table with incorrect locus
		 */
		if (is_dummy_plan(subplan))
			continue;
		
		/* MPP needs target loci to match. */
		if ( Gp_role == GP_ROLE_DISPATCH )
		{
			CdbLocusType locustype = ( subplan->flow == NULL ) ?
				CdbLocusType_Null : subplan->flow->locustype;
				
			if ( append_locustype == CdbLocusType_Null && locus_ok )
			{
				append_locustype = locustype;
			}
			else
			{
				switch ( locustype )
				{
				case CdbLocusType_Entry:
				case CdbLocusType_Hashed:
				case CdbLocusType_Strewn:
					/* Loci of inheritance set must match. */
					locus_ok = locus_ok && (locustype == append_locustype ||
						(locustype == CdbLocusType_Hashed && append_locustype == CdbLocusType_Strewn) ||
						(locustype == CdbLocusType_Strewn && append_locustype == CdbLocusType_Hashed));
					if (locus_ok && locustype != append_locustype)
						ereport(NOTICE, (
							errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("This SQL statement involving inheritance has not been tested... Verify correct answers!") ));
					break;
				case CdbLocusType_Null:
				case CdbLocusType_SingleQE:
				case CdbLocusType_General:
				case CdbLocusType_HashedOJ:
				case CdbLocusType_Replicated:
				default:
					/* These loci not valid on base relations. */
					locus_ok = FALSE;
					break;
				}
			}
			if ( ! locus_ok )
			{
				ereport(ERROR, (
					errcode(ERRCODE_CDB_INTERNAL_ERROR),
					errmsg("incompatible loci in target inheritance set") ));
			}
		}

		/* Save rtable and tlist from first rel for use below */
		if (subplans == NIL)
		{
			rtable = subroot.parse->rtable;
			tlist = subplan->targetlist;
		}

		subplans = lappend(subplans, subplan);

		/* Build target-relations list for the executor */
		resultRelations = lappend_int(resultRelations, appinfo->child_relid);

		/* Build list of per-relation RETURNING targetlists */
		if (parse->returningList)
		{
			Assert(list_length(subroot.parse->returningLists) == 1);
			returningLists = list_concat(returningLists,
										 subroot.parse->returningLists);
		}
	}

	parse->resultRelations = resultRelations;
	parse->returningLists = returningLists;

	/* Mark result as unordered (probably unnecessary) */
	root->query_pathkeys = NIL;

	/*
	 * If we managed to exclude every child rel, return a dummy plan
	 */
	if (subplans == NIL)
		return (Plan *) make_result(tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);

	plan = (Plan *) make_append(subplans, true, tlist);
	
	/* MPP dispatch needs to know the kind of locus. */
	if ( Gp_role == GP_ROLE_DISPATCH )
	{
		switch ( append_locustype )
		{
		case CdbLocusType_Entry:
			mark_plan_entry(plan);
			break;
			
		case CdbLocusType_Hashed:
		case CdbLocusType_Strewn:
			/* Depend on caller to avoid incompatible hash keys. */
			/* For our purpose (UPD/DEL target), strewn is good enough. */
			mark_plan_strewn(plan);
			break;
			
		default:
			ereport(ERROR, (
				errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("unexpected locus assigned to target inheritance set") ));
		}
	}
	
	/*
	 * Planning might have modified the rangetable, due to changes of the
	 * Query structures inside subquery RTEs.  We have to ensure that this
	 * gets propagated back to the master copy.  But can't do this until we
	 * are done planning, because all the calls to grouping_planner need
	 * virgin sub-Queries to work from.  (We are effectively assuming that
	 * sub-Queries will get planned identically each time, or at least that
	 * the impacts on their rangetables will be the same each time.)
	 *
	 * XXX should clean this up someday
	 */
	parse->rtable = rtable;

	return plan;
}

/*--------------------
 * grouping_planner
 *	  Perform planning steps related to grouping, aggregation, etc.
 *	  This primarily means adding top-level processing to the basic
 *	  query plan produced by query_planner.
 *
 * tuple_fraction is the fraction of tuples we expect will be retrieved
 *
 * tuple_fraction is interpreted as follows:
 *	  0: expect all tuples to be retrieved (normal case)
 *	  0 < tuple_fraction < 1: expect the given fraction of tuples available
 *		from the plan to be retrieved
 *	  tuple_fraction >= 1: tuple_fraction is the absolute number of tuples
 *		expected to be retrieved (ie, a LIMIT specification)
 *
 * Returns a query plan.  Also, root->query_pathkeys is returned as the
 * actual output ordering of the plan (in pathkey format).
 *--------------------
 */
static Plan *
grouping_planner(PlannerInfo *root, double tuple_fraction)
{
	Query	   *parse = root->parse;
	List	   *tlist = parse->targetList;
	int64		offset_est = 0;
	int64		count_est = 0;
	Plan	   *result_plan;
	List	   *current_pathkeys = NIL;
	List	   *sort_pathkeys;
    Path       *best_path = NULL;
	double		dNumGroups = 0;
	double		motion_cost_per_row  =
						(gp_motion_cost_per_row > 0.0) ?
						gp_motion_cost_per_row :
						2.0 * cpu_tuple_cost;
	
	/* Tweak caller-supplied tuple_fraction if have LIMIT/OFFSET */
	if (parse->limitCount || parse->limitOffset)
		tuple_fraction = preprocess_limit(root, tuple_fraction,
										  &offset_est, &count_est);

	if (parse->setOperations)
	{
		List	   *set_sortclauses;

		/*
		 * If there's a top-level ORDER BY, assume we have to fetch all the
		 * tuples.	This might seem too simplistic given all the hackery below
		 * to possibly avoid the sort ... but a nonzero tuple_fraction is only
		 * of use to plan_set_operations() when the setop is UNION ALL, and
		 * the result of UNION ALL is always unsorted.
		 */
		if (parse->sortClause)
			tuple_fraction = 0.0;

		/*
		 * Construct the plan for set operations.  The result will not need
		 * any work except perhaps a top-level sort and/or LIMIT.
		 */
		result_plan = plan_set_operations(root, tuple_fraction,
										  &set_sortclauses);

		/*
		 * Calculate pathkeys representing the sort order (if any) of the set
		 * operation's result.  We have to do this before overwriting the sort
		 * key information...
		 */
		current_pathkeys = make_pathkeys_for_sortclauses(set_sortclauses,
													result_plan->targetlist);
		current_pathkeys = canonicalize_pathkeys(root, current_pathkeys);

		/*
		 * We should not need to call preprocess_targetlist, since we must be
		 * in a SELECT query node.	Instead, use the targetlist returned by
		 * plan_set_operations (since this tells whether it returned any
		 * resjunk columns!), and transfer any sort key information from the
		 * original tlist.
		 */
		Assert(parse->commandType == CMD_SELECT);

		tlist = postprocess_setop_tlist(result_plan->targetlist, tlist);

		/*
		 * Can't handle FOR UPDATE/SHARE here (parser should have checked
		 * already, but let's make sure).
		 */
		if (parse->rowMarks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));

		/*
		 * Calculate pathkeys that represent result ordering requirements
		 */
		sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
													  tlist);
		sort_pathkeys = canonicalize_pathkeys(root, sort_pathkeys);
	}
	else if ( parse->windowClause && parse->targetList )
	/* Should really test for no windowing target, but empty tlist suffices. */
	{
		/*
		 * Calculate pathkeys that represent ordering requirements. Stash
		 * them in PlannerInfo so that query_planner can canonicalize them.
		 */
		root->group_pathkeys = NIL;
		root->sort_pathkeys =
			make_pathkeys_for_sortclauses(parse->sortClause, tlist);

		
		result_plan = window_planner(root, tuple_fraction, &current_pathkeys);
		
		/* Recover sort pathkeys for use later.  These may or may not
		 * match the current_pathkeys resulting from the window plan.  
		 */
		sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
													  tlist);
		sort_pathkeys = canonicalize_pathkeys(root, sort_pathkeys);
	}
	else
	{
		/* No set operations, do regular planning */
		List	   *sub_tlist;
		List	   *group_pathkeys;
		AttrNumber *groupColIdx = NULL;
		bool		need_tlist_eval = true;
		QualCost	tlist_cost;
		Path	   *cheapest_path;
		Path	   *sorted_path;
		long		numGroups = 0;
		AggClauseCounts agg_counts;
		int			numGroupCols;
		bool		use_hashed_grouping = false;
		bool        grpext = false;
		CanonicalGroupingSets *canonical_grpsets;

		/* Preprocess targetlist */
		tlist = preprocess_targetlist(root, tlist);

		/* Obtain canonical grouping sets */
		canonical_grpsets = make_canonical_groupingsets(parse->groupClause);
		numGroupCols = canonical_grpsets->num_distcols;

		grpext = is_grouping_extension(canonical_grpsets);

		/*
		 * Generate appropriate target list for subplan; may be different from
		 * tlist if grouping or aggregation is needed.
		 */
		sub_tlist = make_subplanTargetList(root, tlist,
										   &groupColIdx, &need_tlist_eval);

		/*
		 * Calculate pathkeys that represent grouping/ordering requirements.
		 * Stash them in PlannerInfo so that query_planner can canonicalize
		 * them.
		 */
		root->group_pathkeys =
			make_pathkeys_for_groupclause(parse->groupClause, tlist);
		root->sort_pathkeys =
			make_pathkeys_for_sortclauses(parse->sortClause, tlist);

		MemSet(&agg_counts, 0, sizeof(AggClauseCounts));

		/*
		 * Will need actual number of aggregates for estimating costs.
		 *
		 * Note: we do not attempt to detect duplicate aggregates here; a
		 * somewhat-overestimated count is okay for our present purposes.
		 *
		 * Note: think not that we can turn off hasAggs if we find no aggs. It
		 * is possible for constant-expression simplification to remove all
		 * explicit references to aggs, but we still have to follow the
		 * aggregate semantics (eg, producing only one output row).
		 */
		if (parse->hasAggs)
		{
			count_agg_clauses((Node *) tlist, &agg_counts);
			count_agg_clauses(parse->havingQual, &agg_counts);
		}

		/*
		 * Figure out whether we need a sorted result from query_planner.
		 *
		 * If we have a GROUP BY clause, then we want a result sorted properly
		 * for grouping.  Otherwise, if there is an ORDER BY clause, we want
		 * to sort by the ORDER BY clause.	(Note: if we have both, and ORDER
		 * BY is a superset of GROUP BY, it would be tempting to request sort
		 * by ORDER BY --- but that might just leave us failing to exploit an
		 * available sort order at all. Needs more thought...)
		 */
		if (parse->groupClause)
			root->query_pathkeys = root->group_pathkeys;
		else if (parse->sortClause)
			root->query_pathkeys = root->sort_pathkeys;
		else
			root->query_pathkeys = NIL;

		/*
		 * Generate the best unsorted and presorted paths for this Query (but
		 * note there may not be any presorted path).  query_planner will also
		 * estimate the number of groups in the query, and canonicalize all
		 * the pathkeys.
		 */
		query_planner(root, sub_tlist, tuple_fraction,
					  &cheapest_path, &sorted_path, &dNumGroups);

		group_pathkeys = root->group_pathkeys;
		sort_pathkeys = root->sort_pathkeys;

		/*
		 * XXX: if this is a grouping extension, we will not choose the hash aggregation.
		 */
		if (grpext)
		{
			use_hashed_grouping = false;
		}

		else

		/*
		 * If grouping, decide whether we want to use hashed grouping.
		 */
	    if (parse->groupClause)
		{
			use_hashed_grouping =
				choose_hashed_grouping(root, tuple_fraction,
									   cheapest_path, sorted_path,
									   dNumGroups, &agg_counts);

			/* Also convert # groups to long int --- but 'ware overflow! */
			numGroups = (long) Min(dNumGroups, (double) LONG_MAX);
		}

		/*
		 * Select the best path.  If we are doing hashed grouping, we will
		 * always read all the input tuples, so use the cheapest-total path.
		 * Otherwise, trust query_planner's decision about which to use.
		 */
		if (use_hashed_grouping || !sorted_path)
		{
            best_path = cheapest_path;
             /* elog(DEBUG1, "Path chosen: unordered"); CDB*/
		}
		else
		{
            best_path = sorted_path;
            /* elog(DEBUG1, "Path chosen: ordered"); CDB*/
		}
		
		/* CDB:  For now, we either
		 * - construct a general parallel plan,
		 * - let the sequential planner handle the situation, or
		 * - construct a sequential plan using the mix-max index optimization.
		 *
		 * Eventually we should add a parallel version of the min-max
		 * optimization.  For now, it's either-or.
		 */
		if ( Gp_role == GP_ROLE_DISPATCH )
		{
			bool querynode_changed = false;

			result_plan = cdb_grouping_planner(root,
											   &agg_counts,
											   tlist,
											   best_path,
											   cheapest_path,
											   use_hashed_grouping,
											   tuple_fraction,
											   &dNumGroups,
											   canonical_grpsets,
											   &current_pathkeys,
											   &querynode_changed);
			if ( result_plan != NULL && result_plan->flow->numSortCols == 0 )
			{
				/*
				 * cdb_grouping_planner generated the full plan, with the
				 * the right tlist.  And it has no sort order
				 */
				current_pathkeys = NIL;
			}

 			if (result_plan != NULL && querynode_changed)
 			{
 				/* We want to re-write sort_pathkeys here since the 2-stage
				 * aggregation subplan or grouping extension subplan may change
				 * the previous root->parse Query node, which makes the current
				 * sort_pathkeys invalid.
 				 */
 				sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
															  result_plan->targetlist);
			}
		}
		else /* Not GP_ROLE_DISPATCH */
		{
			/*
			 * Check to see if it's possible to optimize MIN/MAX aggregates.
			 * If so, we will forget all the work we did so far to choose a
			 * "regular" path ... but we had to do it anyway to be able to
			 * tell which way is cheaper.
			 */
			result_plan = optimize_minmax_aggregates(root,
													 tlist,
													 best_path);
			if (result_plan != NULL)
			{
				/*
				 * optimize_minmax_aggregates generated the full plan, with the
				 * right tlist, and it has no sort order.
				 */
				current_pathkeys = NIL;
                mark_plan_entry(result_plan);
			}

		}
		
		if (result_plan == NULL)
		{
			/*
			 * Normal case --- create a plan according to query_planner's
			 * results.
			 */
			result_plan = create_plan(root, best_path);
			current_pathkeys = best_path->pathkeys;

			/*
			 * create_plan() returns a plan with just a "flat" tlist of
			 * required Vars.  Usually we need to insert the sub_tlist as the
			 * tlist of the top plan node.	However, we can skip that if we
			 * determined that whatever query_planner chose to return will be
			 * good enough.
			 */
			if (need_tlist_eval)
			{
				/*
				 * If the top-level plan node is one that cannot do expression
				 * evaluation, we must insert a Result node to project the
				 * desired tlist.
				 */
				result_plan = plan_pushdown_tlist(result_plan, sub_tlist);

				/*
				 * Also, account for the cost of evaluation of the sub_tlist.
				 *
				 * Up to now, we have only been dealing with "flat" tlists,
				 * containing just Vars.  So their evaluation cost is zero
				 * according to the model used by cost_qual_eval() (or if you
				 * prefer, the cost is factored into cpu_tuple_cost).  Thus we
				 * can avoid accounting for tlist cost throughout
				 * query_planner() and subroutines.  But now we've inserted a
				 * tlist that might contain actual operators, sub-selects, etc
				 * --- so we'd better account for its cost.
				 *
				 * Below this point, any tlist eval cost for added-on nodes
				 * should be accounted for as we create those nodes.
				 * Presently, of the node types we can add on, only Agg and
				 * Group project new tlists (the rest just copy their input
				 * tuples) --- so make_agg() and make_group() are responsible
				 * for computing the added cost.
				 */
				cost_qual_eval(&tlist_cost, sub_tlist);
				result_plan->startup_cost += tlist_cost.startup;
				result_plan->total_cost += tlist_cost.startup +
					tlist_cost.per_tuple * result_plan->plan_rows;
			}
			else
			{
				/*
				 * Since we're using query_planner's tlist and not the one
				 * make_subplanTargetList calculated, we have to refigure any
				 * grouping-column indexes make_subplanTargetList computed.
				 */
				locate_grouping_columns(root, tlist, result_plan->targetlist,
										groupColIdx);
			}

            Assert(result_plan->flow);

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
												0, /* num_nullcols */
												0, /* input_grouping */
												0, /* grouping_id */
												0, /* rollup_gs_times */
												agg_counts.numAggs,
												agg_counts.transitionSpace,
												result_plan);
				/* Hashed aggregation produces randomly-ordered results */
				current_pathkeys = NIL;
			}
			else if (parse->hasAggs)
			{
				if (!grpext)
				{
					/* Plain aggregate plan --- sort if needed */
					AggStrategy aggstrategy;

					if (parse->groupClause)
					{
						if (!pathkeys_contained_in(group_pathkeys,
												   current_pathkeys))
						{
							result_plan = (Plan *)
								make_sort_from_groupcols(root,
														 parse->groupClause,
														 groupColIdx,
														 false,
														 result_plan);
							current_pathkeys = group_pathkeys;

							/* Decorate the Sort node with a Flow node. */
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

					/*
					 * We make a single Agg node if this is not a grouping extension.
					 */
					result_plan = (Plan *) make_agg(root,
													tlist,
													(List *) parse->havingQual,
													aggstrategy,
													numGroupCols,
													groupColIdx,
													numGroups,
													0, /* num_nullcols */
													0, /* input_grouping */
													0, /* grouping_id */
													0, /* rollup_gs_times */
													agg_counts.numAggs,
													agg_counts.transitionSpace,
													result_plan);
				}

				/* Plan the grouping extension */
				else
				{
					ListCell *lc;
					bool querynode_changed = false;

					/*
					 * Make a copy of tlist. Really need to?
					 */
					List *new_tlist = copyObject(tlist);

					/* Make EXPLAIN output look nice */
					foreach(lc, result_plan->targetlist)
					{
						TargetEntry *tle = (TargetEntry*)lfirst(lc);

						if ( IsA(tle->expr, Var) && tle->resname == NULL )
						{
							TargetEntry *vartle = tlist_member((Node*)tle->expr, tlist);

							if ( vartle != NULL && vartle->resname != NULL )
								tle->resname = pstrdup(vartle->resname);
						}
					}

					result_plan = plan_grouping_extension(root, best_path,
														  &new_tlist, result_plan->targetlist,
														  true, false,
														  (List *) parse->havingQual,
														  &numGroupCols,
														  &groupColIdx,
														  &agg_counts,
														  canonical_grpsets,
														  &dNumGroups,
														  &querynode_changed,
														  &current_pathkeys,
														  result_plan);
					if (querynode_changed)
					{
						/* We want to re-write sort_pathkeys here since the 2-stage
						 * aggregation subplan or grouping extension subplan may change
						 * the previous root->parse Query node, which makes the current
						 * sort_pathkeys invalid.
						 */
						sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
																	  result_plan->targetlist);
					}
				}
			}
			else if (parse->groupClause)
			{
				if (!grpext)
				{
					/*
					 * GROUP BY without aggregation, so insert a group node (plus
					 * the appropriate sort node, if necessary).
					 *
					 * Add an explicit sort if we couldn't make the path come out
					 * the way the GROUP node needs it.
					 */
					if (!pathkeys_contained_in(group_pathkeys, current_pathkeys))
					{
						result_plan = (Plan *)
							make_sort_from_groupcols(root,
													 parse->groupClause,
													 groupColIdx,
													 false,
													 result_plan);
						current_pathkeys = group_pathkeys;
						mark_sort_locus(result_plan);

						/* CDB */ /* pass DISTINCT to sort */
						if (IsA(result_plan, Sort) && gp_enable_sort_distinct)
						{
							Sort* pSort = (Sort*)result_plan;
							pSort->noduplicates = true;
						}
					}

					result_plan = (Plan *) make_group(root,
													  tlist,
													  (List *) parse->havingQual,
													  numGroupCols,
													  groupColIdx,
													  dNumGroups,
													  0, /* num_nullcols */
													  0, /* input_grouping */
													  0, /* grouping_id */
													  0, /* rollup_gs_times */
													  result_plan);
				}
				else
				{
					ListCell *lc;
					bool querynode_changed = false;

					List *new_tlist = copyObject(tlist);

					/* Make EXPLAIN output look nice */
					foreach(lc, result_plan->targetlist)
					{
						TargetEntry *tle = (TargetEntry*)lfirst(lc);

						if ( IsA(tle->expr, Var) && tle->resname == NULL )
						{
							TargetEntry *vartle = tlist_member((Node*)tle->expr, tlist);

							if ( vartle != NULL && vartle->resname != NULL )
								tle->resname = pstrdup(vartle->resname);
						}
					}

					result_plan = (Plan *)
						plan_grouping_extension(root, best_path,
												&new_tlist, result_plan->targetlist,
												false, false,
												(List *) parse->havingQual,
												&numGroupCols,
												&groupColIdx,
												&agg_counts,
												canonical_grpsets,
												&dNumGroups,
												&querynode_changed,
												&current_pathkeys,
												result_plan);
					if (querynode_changed)
					{
						/* We want to re-write sort_pathkeys here since the 2-stage
						 * aggregation subplan or grouping extension subplan may change
						 * the previous root->parse Query node, which makes the current
						 * sort_pathkeys invalid.
						 */
						sort_pathkeys = make_pathkeys_for_sortclauses(parse->sortClause,
																	  result_plan->targetlist);
					}
				}
				/* The Group node won't change sort ordering */
			}
			else if (root->hasHavingQual)
			{
				/*
				 * No aggregates, and no GROUP BY, but we have a HAVING qual.
				 * This is a degenerate case in which we are supposed to emit
				 * either 0 or 1 row depending on whether HAVING succeeds.
				 * Furthermore, there cannot be any variables in either HAVING
				 * or the targetlist, so we actually do not need the FROM
				 * table at all!  We can just throw away the plan-so-far and
				 * generate a Result node.	This is a sufficiently unusual
				 * corner case that it's not worth contorting the structure of
				 * this routine to avoid having to generate the plan in the
				 * first place.
				 */
				result_plan = (Plan *) make_result(tlist,
												   parse->havingQual,
												   NULL);
				/* Result will be only one row anyway; no sort order */
				current_pathkeys = NIL;
				mark_plan_general(result_plan);
			}
		}						/* end of non-minmax-aggregate case */

		/* free canonical_grpsets */
		{
			int grpset_no;
			for (grpset_no=0; grpset_no<canonical_grpsets->ngrpsets; grpset_no++)
			{
				bms_free(canonical_grpsets->grpsets[grpset_no]);
			}

			if (canonical_grpsets)
			{
				if (canonical_grpsets->grpsets)
					pfree(canonical_grpsets->grpsets);
				if (canonical_grpsets->grpset_counts)
					pfree(canonical_grpsets->grpset_counts);
				pfree(canonical_grpsets);
			}
		}
	}							/* end of if (setOperations) */

    /*
     * Decorate the top node with a Flow node if it doesn't have one yet.
     * (In such cases we require the next-to-top node to have a Flow node
     * from which we can obtain the distribution info.)
     */
    if (!result_plan->flow)
        result_plan->flow = pull_up_Flow(result_plan,
                                         result_plan->lefttree,
                                         (current_pathkeys != NIL));

	/*
	 * MPP: If there's a DISTINCT clause and we're not collocated on the
	 * key, we need to redistribute.
	 */
	 if ( Gp_role == GP_ROLE_DISPATCH && parse->distinctClause != NULL)
	 {
		if ( result_plan->flow->flotype == FLOW_PARTITIONED )
		{
			List *distinctExprs = NIL;
			ListCell *cell;
		
			/* Regard input as strewn.  Redistribute on distinct keys. */
			
			if ( gp_enable_preunique ) /* Do we want to pre-unique? */
			{
				/* Reduce the number of rows to move by adding a [Sort and]
				 * Unique prior to the redistribute Motion. */
				if (parse->sortClause)
				{
					if (!pathkeys_contained_in(sort_pathkeys, current_pathkeys))
					{
						result_plan = (Plan *)
							make_sort_from_sortclauses(root,
													   parse->sortClause,
													   result_plan);
						current_pathkeys = sort_pathkeys;
						mark_sort_locus(result_plan);
					}
				}

				result_plan = (Plan *) make_unique(result_plan, parse->distinctClause);

                result_plan->flow = pull_up_Flow(result_plan,
                                                 result_plan->lefttree,
                                                 true);
				
				/* Is this right? */
				result_plan->plan_rows = dNumGroups;
			}
			
			foreach(cell, parse->distinctClause)
			{
				SortClause *sortcl = (SortClause *) lfirst(cell);
				TargetEntry *tle = get_sortgroupclause_tle(sortcl, result_plan->targetlist);
				distinctExprs = lappend(distinctExprs, tle->expr);
			}
			result_plan = (Plan*)make_motion_hash(root, result_plan, distinctExprs);
			result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
			
			current_pathkeys = NULL; /* Any pre-existing order now lost. */
		}
		else if ( result_plan->flow->flotype == FLOW_SINGLETON )
			; /* Already collocated. */
		else
		{
			ereport(ERROR, (
				errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("unexpected input locus to distinct") ));
		}
	 }

    /*
	 * If we were not able to make the plan come out in the right order, add
	 * an explicit sort step.  Note that, if we going to add a Unique node,
	 *  the sort_pathkeys will have the distinct keys as a prefix.
	 */
	if (parse->sortClause)
	{
		if (!pathkeys_contained_in(sort_pathkeys, current_pathkeys))
		{
			result_plan = (Plan *)
				make_sort_from_sortclauses(root,
										   parse->sortClause,
										   result_plan);
			current_pathkeys = sort_pathkeys;
			mark_sort_locus(result_plan);
		}
	}

	/*
	 * If there is a DISTINCT clause, add the UNIQUE node.
	 */
	if (parse->distinctClause)
	{
		result_plan = (Plan *) make_unique(result_plan, parse->distinctClause);
        result_plan->flow = pull_up_Flow(result_plan,
                                         result_plan->lefttree,
                                         true);

		/*
		 * If there was grouping or aggregation, leave plan_rows as-is (ie,
		 * assume the result was already mostly unique).  If not, use the
		 * number of distinct-groups calculated by query_planner.
		 */
		if (!parse->groupClause && !root->hasHavingQual && !parse->hasAggs)
			result_plan->plan_rows = dNumGroups;
	}

	/*
	 * Finally, if there is a LIMIT/OFFSET clause, add the LIMIT node.
	 */
	if (parse->limitCount || parse->limitOffset)
	{
		if ( Gp_role == GP_ROLE_DISPATCH && result_plan->flow->flotype == FLOW_PARTITIONED )
		{
			/* Note max result size.  May be unspecified. */
			Node *precount = copyObject(parse->limitCount);
			int64 precount_est = count_est;
			
			/* add offset to each QEs requested contribution. 
			 * ( MPP-1370: Do it even if no ORDER BY was specified) 
			 */	
			if (precount && parse->limitOffset )
			{
				precount = (Node*)make_op(NULL,
										  list_make1(makeString(pstrdup("+"))),
										  copyObject(parse->limitOffset),
										  precount,
                                          -1);
				precount_est += offset_est;
			}
			
			if ( precount != NULL )
			{
				/* Add a prelimary LIMIT on the partitioned results. This
				 * may reduce the amount of work done on the QEs.  */
				result_plan = (Plan *) make_limit(result_plan,
												  NULL,
												  precount,
												  0,
												  precount_est);
                result_plan->flow = pull_up_Flow(result_plan,
                                                 result_plan->lefttree,
                                                 true);
			}
			
			/* Focus on QE [merge to preserve order], prior to final LIMIT. */
			result_plan = (Plan*)make_motion_gather_to_QE(result_plan, current_pathkeys != NIL);
			result_plan->total_cost += motion_cost_per_row * result_plan->plan_rows;
		}
			
		if ( current_pathkeys == NIL )
		{
			/* This used to be a WARNING.  If reinstated, it should be a NOTICE
			 * and steps taken to avoid issuing it at inopportune times, e.g.,
			 * from the query generated by psql tab-completion.
			 */
			ereport(DEBUG1, (errmsg("LIMIT/OFFSET applied to unordered result.") ));
		}
			
		result_plan = (Plan *) make_limit(result_plan,
										  parse->limitOffset,
										  parse->limitCount,
										  offset_est,
										  count_est);
        result_plan->flow = pull_up_Flow(result_plan,
                                         result_plan->lefttree,
                                         true);
	}

    Insist(result_plan->flow);

	/*
	 * Deal with the RETURNING clause if any.  It's convenient to pass the
	 * returningList through setrefs.c now rather than at top level (if we
	 * waited, handling inherited UPDATE/DELETE would be much harder).
	 */
	if (parse->returningList)
	{
		List	   *rlist;

		rlist = set_returning_clause_references(parse->returningList,
												result_plan,
												parse->resultRelation);
		parse->returningLists = list_make1(rlist);
	}

	/*
	 * Return the actual output ordering in query_pathkeys for possible use by
	 * an outer query level.
	 */
	root->query_pathkeys = current_pathkeys;

	return result_plan;
}

/*
 * Detect whether a plan node is a "dummy" plan created when a relation
 * is deemed not to need scanning due to constraint exclusion.
 *
 * Currently, such dummy plans are Result nodes with constant FALSE
 * filter quals.
 */
static bool
is_dummy_plan(Plan *plan)
{
	if (IsA(plan, Result))
	{
		List	   *rcqual = (List *) ((Result *) plan)->resconstantqual;

		if (list_length(rcqual) == 1)
		{
			Const	   *constqual = (Const *) linitial(rcqual);

			if (constqual && IsA(constqual, Const))
			{
				if (!constqual->constisnull &&
					!DatumGetBool(constqual->constvalue))
					return true;
			}
		}
	}
	return false;
}

/*
 * preprocess_limit - do pre-estimation for LIMIT and/or OFFSET clauses
 *
 * We try to estimate the values of the LIMIT/OFFSET clauses, and pass the
 * results back in *count_est and *offset_est.	These variables are set to
 * 0 if the corresponding clause is not present, and -1 if it's present
 * but we couldn't estimate the value for it.  (The "0" convention is OK
 * for OFFSET but a little bit bogus for LIMIT: effectively we estimate
 * LIMIT 0 as though it were LIMIT 1.  But this is in line with the planner's
 * usual practice of never estimating less than one row.)  These values will
 * be passed to make_limit, which see if you change this code.
 *
 * The return value is the suitably adjusted tuple_fraction to use for
 * planning the query.	This adjustment is not overridable, since it reflects
 * plan actions that grouping_planner() will certainly take, not assumptions
 * about context.
 */
static double
preprocess_limit(PlannerInfo *root, double tuple_fraction,
				 int64 *offset_est, int64 *count_est)
{
	Query	   *parse = root->parse;
	Node	   *est;
	double		limit_fraction;

	/* Should not be called unless LIMIT or OFFSET */
	Assert(parse->limitCount || parse->limitOffset);

	/*
	 * Try to obtain the clause values.  We use estimate_expression_value
	 * primarily because it can sometimes do something useful with Params.
	 */
	if (parse->limitCount)
	{
		est = estimate_expression_value(parse->limitCount);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* NULL indicates LIMIT ALL, ie, no limit */
				*count_est = 0; /* treat as not present */
			}
			else
			{
				if (((Const *)est)->consttype == INT4OID)
					*count_est = DatumGetInt32(((Const *) est)->constvalue);
				else
					*count_est = DatumGetInt64(((Const *) est)->constvalue);
				if (*count_est <= 0)
					*count_est = 1;		/* force to at least 1 */
			}
		}
		else
			*count_est = -1;	/* can't estimate */
	}
	else
		*count_est = 0;			/* not present */

	if (parse->limitOffset)
	{
		est = estimate_expression_value(parse->limitOffset);
		if (est && IsA(est, Const))
		{
			if (((Const *) est)->constisnull)
			{
				/* Treat NULL as no offset; the executor will too */
				*offset_est = 0;	/* treat as not present */
			}
			else
			{
				if (((Const *)est)->consttype == INT4OID)
					*offset_est = DatumGetInt32(((Const *) est)->constvalue);
				else
				*offset_est = DatumGetInt64(((Const *) est)->constvalue);

				if (*offset_est < 0)
					*offset_est = 0;	/* less than 0 is same as 0 */
			}
		}
		else
			*offset_est = -1;	/* can't estimate */
	}
	else
		*offset_est = 0;		/* not present */

	if (*count_est != 0)
	{
		/*
		 * A LIMIT clause limits the absolute number of tuples returned.
		 * However, if it's not a constant LIMIT then we have to guess; for
		 * lack of a better idea, assume 10% of the plan's result is wanted.
		 */
		if (*count_est < 0 || *offset_est < 0)
		{
			/* LIMIT or OFFSET is an expression ... punt ... */
			limit_fraction = 0.10;
		}
		else
		{
			/* LIMIT (plus OFFSET, if any) is max number of tuples needed */
			limit_fraction = (double) *count_est + (double) *offset_est;
		}

		/*
		 * If we have absolute limits from both caller and LIMIT, use the
		 * smaller value; likewise if they are both fractional.  If one is
		 * fractional and the other absolute, we can't easily determine which
		 * is smaller, but we use the heuristic that the absolute will usually
		 * be smaller.
		 */
		if (tuple_fraction >= 1.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* both absolute */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
			else
			{
				/* caller absolute, limit fractional; use caller's value */
			}
		}
		else if (tuple_fraction > 0.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* caller fractional, limit absolute; use limit */
				tuple_fraction = limit_fraction;
			}
			else
			{
				/* both fractional */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
		}
		else
		{
			/* no info from caller, just use limit */
			tuple_fraction = limit_fraction;
		}
	}
	else if (*offset_est != 0 && tuple_fraction > 0.0)
	{
		/*
		 * We have an OFFSET but no LIMIT.	This acts entirely differently
		 * from the LIMIT case: here, we need to increase rather than decrease
		 * the caller's tuple_fraction, because the OFFSET acts to cause more
		 * tuples to be fetched instead of fewer.  This only matters if we got
		 * a tuple_fraction > 0, however.
		 *
		 * As above, use 10% if OFFSET is present but unestimatable.
		 */
		if (*offset_est < 0)
			limit_fraction = 0.10;
		else
			limit_fraction = (double) *offset_est;

		/*
		 * If we have absolute counts from both caller and OFFSET, add them
		 * together; likewise if they are both fractional.	If one is
		 * fractional and the other absolute, we want to take the larger, and
		 * we heuristically assume that's the fractional one.
		 */
		if (tuple_fraction >= 1.0)
		{
			if (limit_fraction >= 1.0)
			{
				/* both absolute, so add them together */
				tuple_fraction += limit_fraction;
			}
			else
			{
				/* caller absolute, limit fractional; use limit */
				tuple_fraction = limit_fraction;
			}
		}
		else
		{
			if (limit_fraction >= 1.0)
			{
				/* caller fractional, limit absolute; use caller's value */
			}
			else
			{
				/* both fractional, so add them together */
				tuple_fraction += limit_fraction;
				if (tuple_fraction >= 1.0)
					tuple_fraction = 0.0;		/* assume fetch all */
			}
		}
	}

	return tuple_fraction;
}

/*
 * choose_hashed_grouping - should we use hashed grouping?
 */
static bool
choose_hashed_grouping(PlannerInfo *root, double tuple_fraction,
					   Path *cheapest_path, Path *sorted_path,
					   double dNumGroups, AggClauseCounts *agg_counts)
{
	int			numGroupCols = num_distcols_in_grouplist(root->parse->groupClause);
	double		cheapest_path_rows;
	int			cheapest_path_width;
	Size		hashentrysize;
	List	   *current_pathkeys;
	Path		hashed_p;
	Path		sorted_p;

	HashAggTableSizes   hash_info;

	/*
	 * Check can't-do-it conditions, including whether the grouping operators
	 * are hashjoinable.
	 *
	 * Executor doesn't support hashed aggregation with DISTINCT aggregates.
	 * (Doing so would imply storing *all* the input values in the hash table,
	 * which seems like a certain loser.)
	 */
	if (!enable_hashagg)
		return false;
	if (agg_counts->numDistinctAggs != 0)
		return false;
	if (!hash_safe_grouping(root))
		return false;

	/*
	 * Don't do it if it doesn't look like the hashtable will fit into
	 * work_mem.
	 *
	 * Beware here of the possibility that cheapest_path->parent is NULL. This
	 * could happen if user does something silly like SELECT 'foo' GROUP BY 1;
	 */
	if (cheapest_path->parent)
	{
		cheapest_path_rows = cheapest_path->parent->rows;
		cheapest_path_width = cheapest_path->parent->width;
	}
	else
	{
		cheapest_path_rows = 1; /* assume non-set result */
		cheapest_path_width = 100;		/* arbitrary */
	}

	/* Estimate per-hash-entry space at tuple width... */
	/* (Should improve this estimate since not all
	 *  attributes are saved in a hash table entry's
	 *  grouping key tuple.)
	 */
	hashentrysize = MAXALIGN(cheapest_path_width) + MAXALIGN(sizeof(MinimalTupleData));
	/* plus space for pass-by-ref transition values... */
	hashentrysize += agg_counts->transitionSpace;
	/* plus the per-hash-entry overhead */
	hashentrysize += hash_agg_entry_size(agg_counts->numAggs);

	if (!calcHashAggTableSizes(1024.0 * (double)work_mem,
							   dNumGroups,
							   agg_counts->numAggs,
							   /* The following estimate is very rough but good enough for planning. */
							   sizeof(HeapTupleData) + sizeof(HeapTupleHeaderData) + cheapest_path_width,
							   agg_counts->transitionSpace,
							   false,
							   &hash_info))
		return false;

	/*
	 * See if the estimated cost is no more than doing it the other way. While
	 * avoiding the need for sorted input is usually a win, the fact that the
	 * output won't be sorted may be a loss; so we need to do an actual cost
	 * comparison.
	 *
	 * We need to consider cheapest_path + hashagg [+ final sort] versus
	 * either cheapest_path [+ sort] + group or agg [+ final sort] or
	 * presorted_path + group or agg [+ final sort] where brackets indicate a
	 * step that may not be needed. We assume query_planner() will have
	 * returned a presorted path only if it's a winner compared to
	 * cheapest_path for this purpose.
	 *
	 * These path variables are dummies that just hold cost fields; we don't
	 * make actual Paths for these steps.
	 */
	cost_agg(&hashed_p, root, AGG_HASHED, agg_counts->numAggs,
			 numGroupCols, dNumGroups,
			 cheapest_path->startup_cost, cheapest_path->total_cost,
			 cheapest_path_rows, hash_info.workmem_per_entry,
			 hash_info.spill ? hash_info.nbatches : 1.0);
	/* Result of hashed agg is always unsorted */
	if (root->sort_pathkeys)
		cost_sort(&hashed_p, root, root->sort_pathkeys, hashed_p.total_cost,
				  dNumGroups, cheapest_path_width);

	if (sorted_path)
	{
		sorted_p.startup_cost = sorted_path->startup_cost;
		sorted_p.total_cost = sorted_path->total_cost;
		current_pathkeys = sorted_path->pathkeys;
	}
	else
	{
		sorted_p.startup_cost = cheapest_path->startup_cost;
		sorted_p.total_cost = cheapest_path->total_cost;
		current_pathkeys = cheapest_path->pathkeys;
	}
	if (!pathkeys_contained_in(root->group_pathkeys, current_pathkeys))
	{
		cost_sort(&sorted_p, root, root->group_pathkeys, sorted_p.total_cost,
				  cheapest_path_rows, cheapest_path_width);
		current_pathkeys = root->group_pathkeys;
	}

	if (root->parse->hasAggs)
		cost_agg(&sorted_p, root, AGG_SORTED, agg_counts->numAggs,
				 numGroupCols, dNumGroups,
				 sorted_p.startup_cost, sorted_p.total_cost,
				 cheapest_path_rows, 0.0, 0.0);
	else
		cost_group(&sorted_p, root, numGroupCols, dNumGroups,
				   sorted_p.startup_cost, sorted_p.total_cost,
				   cheapest_path_rows);
	/* The Agg or Group node will preserve ordering */
	if (root->sort_pathkeys &&
		!pathkeys_contained_in(root->sort_pathkeys, current_pathkeys))
		cost_sort(&sorted_p, root, root->sort_pathkeys, sorted_p.total_cost,
				  dNumGroups, cheapest_path_width);

	/*
	 * Now make the decision using the top-level tuple fraction.  First we
	 * have to convert an absolute count (LIMIT) into fractional form.
	 */
	if (tuple_fraction >= 1.0)
		tuple_fraction /= dNumGroups;

	if (!enable_groupagg)
		return true;

	if (compare_fractional_path_costs(&hashed_p, &sorted_p,
									  tuple_fraction) < 0)
	{
		/* Hashed is cheaper, so use it */
		return true;
	}
	return false;
}

/*
 * hash_safe_grouping - are grouping operators hashable?
 *
 * We assume hashed aggregation will work if the datatype's equality operator
 * is marked hashjoinable.
 */
static bool
hash_safe_grouping(PlannerInfo *root)
{
	List *grouptles;
	ListCell   *glc;

	grouptles = get_sortgroupclauses_tles(root->parse->groupClause,
										 root->parse->targetList);
	foreach(glc, grouptles)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(glc);
		Operator	optup;
		bool		oprcanhash;

		optup = equality_oper(exprType((Node *) tle->expr), true);
		if (!optup)
			return false;
		oprcanhash = ((Form_pg_operator) GETSTRUCT(optup))->oprcanhash;
		ReleaseSysCache(optup);
		if (!oprcanhash)
			return false;
	}
	return true;
}

/*---------------
 * make_subplanTargetList
 *	  Generate appropriate target list when grouping is required.
 *
 * When grouping_planner inserts Aggregate, Group, or Result plan nodes
 * above the result of query_planner, we typically want to pass a different
 * target list to query_planner than the outer plan nodes should have.
 * This routine generates the correct target list for the subplan.
 *
 * The initial target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, we flatten all expressions
 * except GROUP BY items into their component variables; the other expressions
 * will be computed by the inserted nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a,b,c,d,a+b
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.	(In the
 * above example we could theoretically suppress the a and b targets and
 * pass down only c,d,a+b, but it's not really worth the trouble to
 * eliminate simple var references from the subplan.  We will avoid doing
 * the extra computation to recompute a+b at the outer level; see
 * replace_vars_with_subplan_refs() in setrefs.c.)
 *
 * If we are grouping or aggregating, *and* there are no non-Var grouping
 * expressions, then the returned tlist is effectively dummy; we do not
 * need to force it to be evaluated, because all the Vars it contains
 * should be present in the output of query_planner anyway.
 *
 * 'tlist' is the query's target list.
 * 'groupColIdx' receives an array of column numbers for the GROUP BY
 *			expressions (if there are any) in the subplan's target list.
 * 'need_tlist_eval' is set true if we really need to evaluate the
 *			result tlist.
 *
 * The result is the targetlist to be passed to the subplan.
 *---------------
 */
static List *
make_subplanTargetList(PlannerInfo *root,
					   List *tlist,
					   AttrNumber **groupColIdx,
					   bool *need_tlist_eval)
{
	Query	   *parse = root->parse;
	List	   *sub_tlist;
	List	   *extravars;
	int			numCols;

	*groupColIdx = NULL;

	/*
	 * If we're not grouping or aggregating, there's nothing to do here;
	 * query_planner should receive the unmodified target list.
	 */
	if (!parse->hasAggs && !parse->groupClause && !root->hasHavingQual)
	{
		*need_tlist_eval = true;
		return tlist;
	}

	/*
	 * Otherwise, start with a "flattened" tlist (having just the vars
	 * mentioned in the targetlist and HAVING qual --- but not upper- level
	 * Vars; they will be replaced by Params later on).
	 */
	sub_tlist = flatten_tlist(tlist);
	extravars = pull_var_clause(parse->havingQual, false);
	sub_tlist = add_to_flat_tlist(sub_tlist, extravars);
	list_free(extravars);
	*need_tlist_eval = false;	/* only eval if not flat tlist */

	/*
	 * If grouping, create sub_tlist entries for all GROUP BY expressions
	 * (GROUP BY items that are simple Vars should be in the list already),
	 * and make an array showing where the group columns are in the sub_tlist.
	 */
	numCols = num_distcols_in_grouplist(parse->groupClause);

	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *grpColIdx;
		List       *grouptles;
		ListCell   *l;

		grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		*groupColIdx = grpColIdx;

		grouptles = get_sortgroupclauses_tles(parse->groupClause, tlist);

		foreach (l, grouptles)
		{
			Node *groupexpr;
			TargetEntry	   *tle;
			TargetEntry *sub_tle = NULL;
			ListCell   *sl = NULL;

			tle = (TargetEntry *)lfirst(l);
			groupexpr = (Node*) tle->expr;

			/* Find or make a matching sub_tlist entry */
			foreach(sl, sub_tlist)
			{
				sub_tle = (TargetEntry *) lfirst(sl);
				if (equal(groupexpr, sub_tle->expr))
					break;
			}
			if (!sl)
			{
				sub_tle = makeTargetEntry((Expr *) groupexpr,
									 list_length(sub_tlist) + 1,
									 NULL,
									 false);
				sub_tlist = lappend(sub_tlist, sub_tle);
				*need_tlist_eval = true;		/* it's not flat anymore */
			}

			/* Set its group reference and save its resno */
			sub_tle->ressortgroupref = tle->ressortgroupref;
			grpColIdx[keyno++] = sub_tle->resno;
		}
	}

	return sub_tlist;
}

/*
 * locate_grouping_columns
 *		Locate grouping columns in the tlist chosen by query_planner.
 *
 * This is only needed if we don't use the sub_tlist chosen by
 * make_subplanTargetList.	We have to forget the column indexes found
 * by that routine and re-locate the grouping vars in the real sub_tlist.
 */
static void
locate_grouping_columns(PlannerInfo *root,
						List *tlist,
						List *sub_tlist,
						AttrNumber *groupColIdx)
{
	int			keyno = 0;
	List *grouptles;
	ListCell *ge;

	/*
	 * No work unless grouping.
	 */
	if (!root->parse->groupClause)
	{
		Assert(groupColIdx == NULL);
		return;
	}
	Assert(groupColIdx != NULL);

	grouptles = get_sortgroupclauses_tles(root->parse->groupClause, tlist);

	foreach (ge, grouptles)
	{
		TargetEntry *groupte = (TargetEntry *)lfirst(ge);
		Node	*groupexpr;

		TargetEntry *te = NULL;
		ListCell   *sl;

		groupexpr = (Node *) groupte->expr;

		foreach(sl, sub_tlist)
		{
			te = (TargetEntry *) lfirst(sl);
			if (equal(groupexpr, te->expr))
				break;
		}
		if (!sl)
			elog(ERROR, "failed to locate grouping columns");

		groupColIdx[keyno++] = te->resno;
	}
}

/*
 * postprocess_setop_tlist
 *	  Fix up targetlist returned by plan_set_operations().
 *
 * We need to transpose sort key info from the orig_tlist into new_tlist.
 * NOTE: this would not be good enough if we supported resjunk sort keys
 * for results of set operations --- then, we'd need to project a whole
 * new tlist to evaluate the resjunk columns.  For now, just ereport if we
 * find any resjunk columns in orig_tlist.
 */
static List *
postprocess_setop_tlist(List *new_tlist, List *orig_tlist)
{
	ListCell   *l;
	ListCell   *orig_tlist_item = list_head(orig_tlist);

	foreach(l, new_tlist)
	{
		TargetEntry *new_tle = (TargetEntry *) lfirst(l);
		TargetEntry *orig_tle;

		/* ignore resjunk columns in setop result */
		if (new_tle->resjunk)
			continue;

		Assert(orig_tlist_item != NULL);
		orig_tle = (TargetEntry *) lfirst(orig_tlist_item);
		orig_tlist_item = lnext(orig_tlist_item);
		if (orig_tle->resjunk)	/* should not happen */
			elog(ERROR, "resjunk output columns are not implemented");
		Assert(new_tle->resno == orig_tle->resno);
		new_tle->ressortgroupref = orig_tle->ressortgroupref;
	}
	if (orig_tlist_item != NULL)
		elog(ERROR, "resjunk output columns are not implemented");
	return new_tlist;
}

/*
 * Produce the canonical form of a GROUP BY clause given the parse
 * tree form.
 *
 * The result is a CanonicalGroupingSets, which contains a list of
 * Bitmapsets.  Each Bitmapset contains the sort-group reference
 * values of the attributes in one of the grouping sets specified in
 * the GROUP BY clause.  The number of list elements is the number of
 * grouping sets specified.
 */
static CanonicalGroupingSets *
make_canonical_groupingsets(List *groupClause)
{
	CanonicalGroupingSets *canonical_grpsets = 
		(CanonicalGroupingSets *) palloc0(sizeof(CanonicalGroupingSets));
	ListCell *lc;
	List *ord_grping = NIL; /* the ordinary grouping */
	List *rollups = NIL;    /* the grouping sets from ROLLUP */
	List *grpingsets = NIL; /* the grouping sets from GROUPING SETS */
	List *cubes = NIL;      /* the grouping sets from CUBE */
	Bitmapset *bms = NULL;
	List *final_grpingsets = NIL;
	List *list_grpingsets = NIL;
	int setno;
	int prev_setno = 0;

	if (groupClause == NIL)
		return canonical_grpsets;

	foreach (lc, groupClause)
	{
		GroupingClause *gc;

		Node *node = lfirst(lc);

		/* Note that the top-level empty sets have been removed
		 * in the parser.
		 */
		Assert(IsA(node, GroupClause) ||
			   IsA(node, GroupingClause) ||
			   IsA(node, List));

		if (IsA(node, GroupClause) ||
			IsA(node, List))
		{
			ord_grping = lappend(ord_grping,
								 canonicalize_colref_list(node));
			continue;
		}

		gc = (GroupingClause *)node;
		switch (gc->groupType)
		{
			case GROUPINGTYPE_ROLLUP:
				rollups = lappend(rollups,
								  rollup_gs_list(canonicalize_gs_list(gc->groupsets, true)));
				break;
			case GROUPINGTYPE_CUBE:
				cubes = lappend(cubes,
								cube_gs_list(canonicalize_gs_list(gc->groupsets, true)));
				break;
			case GROUPINGTYPE_GROUPING_SETS:
				grpingsets = lappend(grpingsets,
									 canonicalize_gs_list(gc->groupsets, false));
				break;
			default:
				elog(ERROR, "invalid grouping set");
		}
	}

	/* Obtain the cartesian product of grouping sets generated for ordinary
	 * grouping sets, rollups, cubes, and grouping sets.
	 *
	 * We apply a small optimization here. We always append grouping sets
	 * generated for rollups, cubes and grouping sets to grouping sets for
	 * ordinary sets. This makes it easier to tell if there is a partial
	 * rollup. Consider the example of GROUP BY rollup(i,j),k. There are
	 * three grouping sets for rollup(i,j): (i,j), (i), (). If we append
	 * k after each grouping set for rollups, we get three sets:
	 * (i,j,k), (i,k) and (k). We can not easily tell that this is a partial
	 * rollup. However, if we append each grouping set after k, we get
	 * these three sets: (k,i,j), (k,i), (k), which is obviously a partial
	 * rollup.
	 */

	/* First, we bring all columns in ordinary grouping sets together into
	 * one list.
	 */
	foreach (lc, ord_grping)
	{
	    Bitmapset *sub_bms = (Bitmapset *)lfirst(lc);
		bms = bms_add_members(bms, sub_bms);
	}

	final_grpingsets = lappend(final_grpingsets, bms);

	/* Make the list of grouping sets */
	if (rollups)
		list_grpingsets = list_concat(list_grpingsets, rollups);
	if (cubes)
		list_grpingsets = list_concat(list_grpingsets, cubes);
	if (grpingsets)
		list_grpingsets = list_concat(list_grpingsets, grpingsets);

	/* Obtain the cartesian product of grouping sets generated from ordinary
	 * grouping sets, rollups, cubes, and grouping sets.
	 */
	foreach (lc, list_grpingsets)
	{
		List *bms_list = (List *)lfirst(lc);
		ListCell *tmp_lc;
		List *tmp_list;

		tmp_list = final_grpingsets;
		final_grpingsets = NIL;

		foreach (tmp_lc, tmp_list)
		{
			Bitmapset *tmp_bms = (Bitmapset *)lfirst(tmp_lc);
			ListCell *bms_lc;

			foreach (bms_lc, bms_list)
			{
				bms = bms_copy(tmp_bms);
				bms = bms_add_members(bms, (Bitmapset *)lfirst(bms_lc));
				final_grpingsets = lappend(final_grpingsets, bms);
			}
		}
	}

	/* Sort final_grpingsets */
	sort_canonical_gs_list(final_grpingsets,
						   &(canonical_grpsets->ngrpsets),
						   &(canonical_grpsets->grpsets));

	/* Combine duplicate grouping sets and set the counts for
	 * each grouping set.
	 */
	canonical_grpsets->grpset_counts =
		(int *)palloc0(canonical_grpsets->ngrpsets * sizeof(int));
	prev_setno = 0;
	canonical_grpsets->grpset_counts[0] = 1;
	for (setno = 1; setno<canonical_grpsets->ngrpsets; setno++)
	{
		if (bms_equal(canonical_grpsets->grpsets[setno],
					  canonical_grpsets->grpsets[prev_setno]))
		{
			canonical_grpsets->grpset_counts[prev_setno]++;
			if (canonical_grpsets->grpsets[setno])
				pfree(canonical_grpsets->grpsets[setno]);
		}

		else
		{
			prev_setno++;
			canonical_grpsets->grpsets[prev_setno] =
				canonical_grpsets->grpsets[setno];
			canonical_grpsets->grpset_counts[prev_setno]++;
		}
	}
	/* Reset ngrpsets to eliminate duplicate groupint sets */
	canonical_grpsets->ngrpsets = prev_setno + 1;

	/* Obtain the number of distinct columns appeared in these
	 * grouping sets.
	 */
	{
		Bitmapset *distcols = NULL;
		for (setno =0; setno < canonical_grpsets->ngrpsets; setno++)
			distcols =
				bms_add_members(distcols, canonical_grpsets->grpsets[setno]);
		
		canonical_grpsets->num_distcols = bms_num_members(distcols);
		bms_free(distcols);
	}
	

	/* Release spaces */
	list_free_deep(ord_grping);
	list_free_deep(list_grpingsets);
	list_free(final_grpingsets);
	
	return canonical_grpsets;
}

/* Produce the canonical representation of a column reference list.
 *
 * A column reference list (in SQL) is a comma-delimited list of
 * column references which are represented by the parser as a
 * List of GroupClauses.  No nesting is allowed in column reference 
 * lists.
 *
 * As a convenience, this function also recognizes a bare column
 * reference.
 *
 * The result is a Bitmapset of the sort-group-ref values in the list.
 */
static Bitmapset* canonicalize_colref_list(Node * node)
{
	ListCell *lc;
	GroupClause *gc;
	Bitmapset* gs = NULL;
	
	if ( node == NULL )
		elog(ERROR,"invalid column reference list");
	
	if ( IsA(node, GroupClause) )
	{
		gc = (GroupClause*)node;
		return bms_make_singleton(gc->tleSortGroupRef);
	}
	
	if ( !IsA(node, List) )
		elog(ERROR,"invalid column reference list");
	
	foreach (lc, (List*)node)
	{
		Node *cr = lfirst(lc);
		
		if ( cr == NULL )
			continue;
			
		if ( !IsA(cr, GroupClause) )
			elog(ERROR,"invalid column reference list");

		gc = (GroupClause*)cr;
		gs = bms_add_member(gs, gc->tleSortGroupRef);	
	}
	return gs;
}

/* Produce the list of canonical grouping sets corresponding to a
 * grouping set list or an ordinary grouping set list.
 * 
 * An ordinary grouping set list (in SQL) is a comma-delimited list 
 * of ordinary grouping sets.  
 * 
 * Each ordinary grouping set is either a grouping column reference 
 * or a parenthesized list of grouping column references.  No nesting 
 * is allowed.  
 *
 * A grouping set list (in SQL) is a comma-delimited list of grouping 
 * sets.  
 *
 * Each grouping set is either an ordinary grouping set, a rollup list, 
 * a cube list, the empty grouping set, or (recursively) a grouping set 
 * list.
 *
 * The parse tree form of an ordinary grouping set is a  list containing
 * GroupClauses and lists of GroupClauses (without nesting).  In the case
 * of a (general) grouping set, the parse tree list may also include
 * NULLs and GroupingClauses.
 *
 * The result is a list of bit map sets.
 */
static List *canonicalize_gs_list(List *gsl, bool ordinary)
{
	ListCell *lc;
	List *list = NIL;

	foreach (lc, gsl)
	{
		Node *node = lfirst(lc);

		if ( node == NULL )
		{
			if ( ordinary )
				elog(ERROR,"invalid ordinary grouping set");
			
			list = lappend(list, NIL); /* empty grouping set */
		}
		else if ( IsA(node, GroupClause) || IsA(node, List) )
		{
			/* ordinary grouping set */
			list = lappend(list, canonicalize_colref_list(node));
		}
		else if ( IsA(node, GroupingClause) )
		{	
			List *gs;
			GroupingClause *gc = (GroupingClause*)node;
			
			if ( ordinary )
				elog(ERROR,"invalid ordinary grouping set");
				
			switch ( gc->groupType )
			{
				case GROUPINGTYPE_ROLLUP:
					gs = rollup_gs_list(canonicalize_gs_list(gc->groupsets, true));
					break;
				case GROUPINGTYPE_CUBE:
					gs = cube_gs_list(canonicalize_gs_list(gc->groupsets, true));
					break;
				case GROUPINGTYPE_GROUPING_SETS:
					gs = canonicalize_gs_list(gc->groupsets, false);
					break;
				default:
					elog(ERROR,"invalid grouping set");
			}
			list = list_concat(list,gs);
		}
		else
		{
			elog(ERROR,"invalid grouping set list");
		}
	}
	return list;
}

/* Produce the list of N+1 canonical grouping sets corresponding
 * to the rollup of the given list of N canonical grouping sets.
 * These N+1 grouping sets are listed in the descending order
 * based on the number of columns.
 *
 * Argument and result are both a list of bit map sets.
 */
static List *rollup_gs_list(List *gsl)
{
	ListCell *lc;
	Bitmapset **bms;
	int i, n = list_length(gsl);
	
	if ( n == 0 )
		elog(ERROR,"invalid grouping ordinary grouping set list");
	
	if ( n > 1 )
	{
		/* Reverse the elements in gsl */
		List *new_gsl = NIL;
		foreach (lc, gsl)
		{
			new_gsl = lcons(lfirst(lc), new_gsl);
		}
		list_free(gsl);
		gsl = new_gsl;

		bms = (Bitmapset**)palloc(n*sizeof(Bitmapset*));
		i = 0;
		foreach (lc, gsl)
		{
			bms[i++] = (Bitmapset*)lfirst(lc);
		}
		for ( i = n-2; i >= 0; i-- )
		{
			bms[i] = bms_add_members(bms[i], bms[i+1]);
		}
		pfree(bms);
	}

	return lappend(gsl, NULL);
}

/* Subroutine for cube_gs_list. */
static List *add_gs_combinations(List *list, int n, int i,
								 Bitmapset **base, Bitmapset **work)
{
	if ( i < n )
	{
		work[i] = base[i];
		list = add_gs_combinations(list, n, i+1, base, work);
		work[i] = NULL;
		list = add_gs_combinations(list, n, i+1, base, work);	
	}
	else
	{
		Bitmapset *gs = NULL;
		int j;
		for ( j = 0; j < n; j++ )
		{
			gs = bms_add_members(gs, work[j]);
		}
		list = lappend(list,gs);
	}
	return list;
}

/* Produce the list of 2^N canonical grouping sets corresponding
 * to the cube of the given list of N canonical grouping sets.
 *
 * We could do this more efficiently, but the number of grouping
 * sets should be small, so don't bother.
 *
 * Argument and result are both a list of bit map sets.
 */
static List *cube_gs_list(List *gsl)
{
	ListCell *lc;
	Bitmapset **bms_base;
	Bitmapset **bms_work;
	int i, n = list_length(gsl);
	
	if ( n == 0 )
		elog(ERROR,"invalid grouping ordinary grouping set list");
	
	bms_base = (Bitmapset**)palloc(n*sizeof(Bitmapset*));
	bms_work = (Bitmapset**)palloc(n*sizeof(Bitmapset*));
	i = 0;
	foreach (lc, gsl)
	{
		bms_work[i] = NULL;
		bms_base[i++] = (Bitmapset*)lfirst(lc);
	}

	return add_gs_combinations(NIL, n, 0, bms_base, bms_work);
}

/* Subroutine for sort_canonical_gs_list. */
static int gs_compare(const void *a, const void*b)
{
	/* Put the larger grouping sets before smaller ones. */
	return (0-bms_compare(*(Bitmapset**)a, *(Bitmapset**)b));
}

/* Produce a sorted array of Bitmapsets from the given list of Bitmapsets in
 * descending order.
 */
static void sort_canonical_gs_list(List *gs, int *p_nsets, Bitmapset ***p_sets)
{
	ListCell *lc;
	int nsets = list_length(gs);
	Bitmapset **sets = palloc(nsets*sizeof(Bitmapset*));
	int i = 0;
	
	foreach (lc, gs)
	{
		sets[i++] =  (Bitmapset*)lfirst(lc);
	}
	
	qsort(sets, nsets, sizeof(Bitmapset*), gs_compare);
	
	Assert( p_nsets != NULL && p_sets != NULL );
	
	*p_nsets = nsets;
	*p_sets = sets;
}
