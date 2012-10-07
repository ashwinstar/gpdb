/*-------------------------------------------------------------------------
 *
 * parse_agg.c
 *	  handle aggregates in parser
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_agg.c,v 1.73 2006/07/27 19:52:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/gramparse.h" /* for SystemFuncName */
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


typedef struct
{
	ParseState *pstate;
	List	   *groupClauses;
	bool		have_non_var_grouping;
	int			sublevels_up;
} check_ungrouped_columns_context;

typedef struct
{
	int sublevels_up;
} checkHasWindFuncs_context;

typedef struct
{
	int sublevels_up;
} checkHasGroupExtFuncs_context;

static void check_ungrouped_columns(Node *node, ParseState *pstate,
						List *groupClauses, bool have_non_var_grouping);
static bool check_ungrouped_columns_walker(Node *node,
							   check_ungrouped_columns_context *context);
static List* check_aggregate_ingroup(Node *grpcl, List *targetList, List *groupClauses);
static List* get_groupclause_exprs(Node *grpcl, List *targetList);
static void transformSequenceSpec(ParseState *pstate, SequenceSpec *spec);
static void transformSequenceDefn(ParseState *pstate, SequenceDefn *defn);

/*
 * Worker for transformAggregateCall() and transformWindowCall(). We need to
 * determine which query level the aggregate or function belongs to and
 * set that in the node's levelsup field. We also test some important rules:
 * aggregates cannot take aggregates or window functions are arguments.
 * Window functions can take aggregates as arguments but not window
 * functions.
 */

static void
check_call(ParseState *pstate, Node *call)
{
	bool		is_agg = IsA(call, Aggref);

	/*
	 * The call's level is the same as the level of the lowest-level
	 * variable or aggregate in its arguments; or if it contains no variables
	 * at all, we presume it to be local.
	 */
	int min_varlevel = -1;
	int max_varlevel = -1;
	if (is_agg)
	{
		find_minimum_maximum_var_level((Node *) ((Aggref *)call)->args, &min_varlevel, &max_varlevel);

		/*
		 * Currently, we don't support aggregates that refer to upper level
		 * variables.  We could check if min_varlevel > 0, but it seems
		 * the optimizer is not able to handle the mixture of different
		 * level of variables.  This means, only min == max <= 0
		 * are supported.
		 */
		if (max_varlevel > 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					errmsg("aggregate functions referencing an outer query block are not supported")));
		}
	}
	else
		find_minimum_maximum_var_level((Node *)((WindowRef *)call)->args, &min_varlevel, &max_varlevel);

	/*
	 * An aggregate can't directly contain another aggregate call of the same
	 * level (though outer aggs are okay).	We can skip this check if we
	 * didn't find any local vars or aggs.
	 */
	if (min_varlevel == 0 && is_agg)
	{
		if (checkExprHasAggs((Node *)((Aggref *)call)->args))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("aggregate function calls may not be nested"),
							 errOmitLocation(true)));
	}

	/*
	 * In both cases, nested window functions are not allowed regardless of
	 * var reference level, including < 0, which is possible because we
	 * don't treat WindowRef as if Var, as opposed to Aggref.
	 */
	if (is_agg)
	{
		if (checkExprHasWindFuncs((Node *) ((Aggref *) call)->args))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("window functions may not be used as arguments to "
						"aggregates"),
								 errOmitLocation(true)));
	}
	else
	{
		if (checkExprHasWindFuncs((Node *) ((WindowRef *) call)->args))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("cannot use window function as an argument "
							"to another window function"),
									 errOmitLocation(true)));
	}

	if (min_varlevel < 0)
		min_varlevel = 0;

	if (is_agg)	
		((Aggref *)call)->agglevelsup = min_varlevel;
	else
		((WindowRef *)call)->winlevelsup = min_varlevel;
	
	/* Mark the correct pstate as having aggregates */
	while (min_varlevel-- > 0)
		pstate = pstate->parentParseState;

	if (is_agg)
		pstate->p_hasAggs = true;
	else 
		pstate->p_hasWindFuncs = true;
}

/*
 * transformAggregateCall -
 *		Finish initial transformation of an aggregate call
 */
void
transformAggregateCall(ParseState *pstate, Aggref *agg, List *agg_order)
{
    /* 
     * Transform the aggregate order by, if any.
     *
     * This involves transforming a sortlist, which in turn requires 
     * maintenace of a targetlist maintained for the purposes of the
     * sort.  This targetlist cannot be the main query targetlist 
     * because the rules of which columns are referencable are different
     * for the two targetlists.  This one can reference any column in
     * in the from list, the main targetlist is limitted to expressions
     * that show up in the group by clause.
     *
     * CDB: This is a little different from the postgres implementation.
     * In the postgres implementation the "args" of the aggregate is 
     * the targetlist.  In the GP implementation the args are a regular
     * parameter list and we build a separate targetlist for use in 
     * the order by.
     */
    if (agg_order)
    {
        AggOrder *aggorder = makeNode(AggOrder);
        List     *tlist    = NIL;
        int       save_next_resno;

        /* transformSortClause will move the parse state resno which can
         * cause problems since this isn't actually modifying the main
         * target list.  To handle this we simply save the current resno and
         * restore it when the transform is complete. */
        save_next_resno = pstate->p_next_resno;
        pstate->p_next_resno = 1;
        
        aggorder->sortImplicit = false;   /* TODO: implicit ordered aggregates */
        aggorder->sortClause = 
            transformSortClause(pstate, 
                                agg_order,
                                &tlist,
                                true /* fix unknowns */ ,
                                true /* use SQL99 rules */ );
        aggorder->sortTargets = tlist;
        
        pstate->p_next_resno = save_next_resno;
        
        agg->aggorder = aggorder;
    }

    /* shared code with window functions */
	check_call(pstate, (Node *)agg);
}

void
transformWindowFuncCall(ParseState *pstate, WindowRef *wind)
{
	check_call(pstate, (Node *)wind);
}

/*
 * parseCheckAggregates
 *	Check for aggregates where they shouldn't be and improper grouping.
 *
 *	Ideally this should be done earlier, but it's difficult to distinguish
 *	aggregates from plain functions at the grammar level.  So instead we
 *	check here.  This function should be called after the target list and
 *	qualifications are finalized.
 */
void
parseCheckAggregates(ParseState *pstate, Query *qry)
{
	List	   *groupClauses = NIL;
	bool		have_non_var_grouping;
	ListCell   *l;
	bool		hasJoinRTEs;
	PlannerInfo *root;
	Node	   *clause;

	/* This should only be called if we found aggregates or grouping */
	Assert(pstate->p_hasAggs || qry->groupClause || qry->havingQual);

	/*
	 * Aggregates and window functions must never appear in WHERE or 
	 * JOIN/ON clauses.  Window function must never appear in HAVING
	 * clauses.
	 *
	 * (Note this check should appear first to deliver an appropriate error
	 * message; otherwise we are likely to complain about some innocent
	 * variable in the target list, which is outright misleading if the
	 * problem is in WHERE.)
	 */
	if (checkExprHasAggs(qry->jointree->quals))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("aggregates not allowed in WHERE clause"),
				 errOmitLocation(true)));
	if (checkExprHasAggs((Node *) qry->jointree->fromlist))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("aggregates not allowed in JOIN conditions"),
						 errOmitLocation(true)));

	if (checkExprHasWindFuncs(qry->jointree->quals))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("window functions not allowed in WHERE clause"),
						 errOmitLocation(true)));
	if (checkExprHasWindFuncs((Node *) qry->jointree->fromlist))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("window functions not allowed in JOIN conditions"),
						 errOmitLocation(true)));
	if (checkExprHasWindFuncs((Node *) qry->havingQual))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("window functions not allowed in HAVING conditions"),
						 errOmitLocation(true)));
	/*
	 * No aggregates allowed in GROUP BY clauses, either.
	 *
	 * While we are at it, build a list of the acceptable GROUP BY expressions
	 * for use by check_ungrouped_columns().
	 */
	foreach(l, qry->groupClause)
	{
		groupClauses =
			check_aggregate_ingroup((Node*)lfirst(l), qry->targetList, groupClauses);
	}

	/*
	 * If there are join alias vars involved, we have to flatten them to the
	 * underlying vars, so that aliased and unaliased vars will be correctly
	 * taken as equal.	We can skip the expense of doing this if no rangetable
	 * entries are RTE_JOIN kind.
	 */
	hasJoinRTEs = false;
	foreach(l, pstate->p_rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_JOIN)
		{
			hasJoinRTEs = true;
			break;
		}
	}

	/*
	 * We use the planner's flatten_join_alias_vars routine to do the
	 * flattening; it wants a PlannerInfo root node, which fortunately can be
	 * mostly dummy.
	 */
	if (hasJoinRTEs)
	{
		root = makeNode(PlannerInfo);
		root->parse = qry;
		root->hasJoinRTEs = true;

		groupClauses = (List *) flatten_join_alias_vars(root,
													  (Node *) groupClauses);
	}
	else
		root = NULL;			/* keep compiler quiet */

	/*
	 * Detect whether any of the grouping expressions aren't simple Vars; if
	 * they're all Vars then we don't have to work so hard in the recursive
	 * scans.  (Note we have to flatten aliases before this.)
	 */
	have_non_var_grouping = false;
	foreach(l, groupClauses)
	{
		if (!IsA((Node *) lfirst(l), Var))
		{
			have_non_var_grouping = true;
			break;
		}
	}

	/*
	 * Check the targetlist and HAVING clause for ungrouped variables.
	 */
	clause = (Node *) qry->targetList;
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(root, clause);
	check_ungrouped_columns(clause, pstate,
							groupClauses, have_non_var_grouping);

	clause = (Node *) qry->havingQual;
	if (hasJoinRTEs)
		clause = flatten_join_alias_vars(root, clause);
	check_ungrouped_columns(clause, pstate,
							groupClauses, have_non_var_grouping);

	/*
	 * Unfortunately, percentile functions in CSQ return wrong result
	 * and looks like a big effort to fix.  The issue is that cdbllize tries
	 * to push down Param node to subquery, but it fails to do it in
	 * some circomstances.  The future planner may be able to
	 * handle this case correctly.
	 */
	if (contain_vars_of_level_or_above((Node *) qry, 1))
	{
		if (extract_nodes(NULL, (Node *) qry->targetList, T_PercentileExpr) ||
			extract_nodes(NULL, qry->havingQual, T_PercentileExpr))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("correlated subquery cannot contain percentile functions")));
	}
}


/*
 * check_ungrouped_columns -
 *	  Scan the given expression tree for ungrouped variables (variables
 *	  that are not listed in the groupClauses list and are not within
 *	  the arguments of aggregate functions).  Emit a suitable error message
 *	  if any are found.
 *
 * NOTE: we assume that the given clause has been transformed suitably for
 * parser output.  This means we can use expression_tree_walker.
 *
 * NOTE: we recognize grouping expressions in the main query, but only
 * grouping Vars in subqueries.  For example, this will be rejected,
 * although it could be allowed:
 *		SELECT
 *			(SELECT x FROM bar where y = (foo.a + foo.b))
 *		FROM foo
 *		GROUP BY a + b;
 * The difficulty is the need to account for different sublevels_up.
 * This appears to require a whole custom version of equal(), which is
 * way more pain than the feature seems worth.
 */
static void
check_ungrouped_columns(Node *node, ParseState *pstate,
						List *groupClauses, bool have_non_var_grouping)
{
	check_ungrouped_columns_context context;

	context.pstate = pstate;
	context.groupClauses = groupClauses;
	context.have_non_var_grouping = have_non_var_grouping;
	context.sublevels_up = 0;
	check_ungrouped_columns_walker(node, &context);
}

static bool
check_ungrouped_columns_walker(Node *node,
							   check_ungrouped_columns_context *context)
{
	ListCell   *gl;

	if (node == NULL)
		return false;
	if (IsA(node, Const) ||
		IsA(node, Param))
		return false;			/* constants are always acceptable */

	/*
	 * If we find an aggregate call of the original level, do not recurse into
	 * its arguments; ungrouped vars in the arguments are not an error. We can
	 * also skip looking at the arguments of aggregates of higher levels,
	 * since they could not possibly contain Vars that are of concern to us
	 * (see transformAggregateCall).  We do need to look into the arguments of
	 * aggregates of lower levels, however.
	 */
	if (IsA(node, Aggref) &&
		(int) ((Aggref *) node)->agglevelsup >= context->sublevels_up)
		return false;

	/*
	 * PercentileExpr's levelsup is zero.
	 */
	if (IsA(node, PercentileExpr) &&
		0 >= context->sublevels_up)
		return false;

	/*
	 * If we have any GROUP BY items that are not simple Vars, check to see if
	 * subexpression as a whole matches any GROUP BY item. We need to do this
	 * at every recursion level so that we recognize GROUPed-BY expressions
	 * before reaching variables within them. But this only works at the outer
	 * query level, as noted above.
	 */
	if (context->have_non_var_grouping && context->sublevels_up == 0)
	{
		foreach(gl, context->groupClauses)
		{
			if (equal(node, lfirst(gl)))
				return false;	/* acceptable, do not descend more */
		}
	}

	/*
	 * If we have an ungrouped Var of the original query level, we have a
	 * failure.  Vars below the original query level are not a problem, and
	 * neither are Vars from above it.	(If such Vars are ungrouped as far as
	 * their own query level is concerned, that's someone else's problem...)
	 */
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;
		const char *attname;

		if (var->varlevelsup != context->sublevels_up)
			return false;		/* it's not local to my query, ignore */

		/*
		 * Check for a match, if we didn't do it above.
		 */
		if (!context->have_non_var_grouping || context->sublevels_up != 0)
		{
			foreach(gl, context->groupClauses)
			{
				Var		   *gvar = (Var *) lfirst(gl);

				if (IsA(gvar, Var) &&
					gvar->varno == var->varno &&
					gvar->varattno == var->varattno &&
					gvar->varlevelsup == 0)
					return false;		/* acceptable, we're okay */
			}
		}

		/* Found an ungrouped local variable; generate error message */
		Assert(var->varno > 0 &&
			   (int) var->varno <= list_length(context->pstate->p_rtable));
		rte = rt_fetch(var->varno, context->pstate->p_rtable);
		attname = get_rte_attribute_name(rte, var->varattno);
		if (context->sublevels_up == 0)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("column \"%s.%s\" must appear in the GROUP BY clause or be used in an aggregate function",
							rte->eref->aliasname, attname),
									 errOmitLocation(true)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("subquery uses ungrouped column \"%s.%s\" from outer query",
							rte->eref->aliasname, attname),
									 errOmitLocation(true)));

	}

	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   check_ungrouped_columns_walker,
								   (void *) context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, check_ungrouped_columns_walker,
								  (void *) context);
}

/*
 * Create expression trees for the transition and final functions
 * of an aggregate.  These are needed so that polymorphic functions
 * can be used within an aggregate --- without the expression trees,
 * such functions would not know the datatypes they are supposed to use.
 * (The trees will never actually be executed, however, so we can skimp
 * a bit on correctness.)
 *
 * agg_input_types, agg_state_type, agg_result_type identify the input,
 * transition, and result types of the aggregate.  These should all be
 * resolved to actual types (ie, none should ever be ANYARRAY or ANYELEMENT).
 *
 * transfn_oid and finalfn_oid identify the funcs to be called; the latter
 * may be InvalidOid.
 *
 * Pointers to the constructed trees are returned into *transfnexpr and
 * *finalfnexpr.  The latter is set to NULL if there's no finalfn.
 */
void
build_aggregate_fnexprs(Oid *agg_input_types,
						int agg_num_inputs,
						Oid agg_state_type,
						Oid agg_result_type,
						Oid transfn_oid,
						Oid finalfn_oid,
						Oid prelimfn_oid,
						Oid invtransfn_oid,
						Oid invprelimfn_oid,
						Expr **transfnexpr,
						Expr **finalfnexpr,
						Expr **prelimfnexpr,
						Expr **invtransfnexpr,
						Expr **invprelimfnexpr)
{
	Param	   *argp;
	List	   *args;
	int			i;

	/*
	 * Build arg list to use in the transfn FuncExpr node. We really only care
	 * that transfn can discover the actual argument types at runtime using
	 * get_fn_expr_argtype(), so it's okay to use Param nodes that don't
	 * correspond to any real Param.
	 */
	argp = makeNode(Param);
	argp->paramkind = PARAM_EXEC;
	argp->paramid = -1;
	argp->paramtype = agg_state_type;

	args = list_make1(argp);

	for (i = 0; i < agg_num_inputs; i++)
	{
		argp = makeNode(Param);
		argp->paramkind = PARAM_EXEC;
		argp->paramid = -1;
		argp->paramtype = agg_input_types[i];
		args = lappend(args, argp);
	}

	*transfnexpr = (Expr *) makeFuncExpr(transfn_oid,
										 agg_state_type,
										 args,
										 COERCE_DONTCARE);

	/* see if we have a final function */
	if (!OidIsValid(finalfn_oid))
		*finalfnexpr = NULL;
	else
	{
		/*
		 * Build expr tree for final function
		 */
		argp = makeNode(Param);
		argp->paramkind = PARAM_EXEC;
		argp->paramid = -1;
		argp->paramtype = agg_state_type;
		args = list_make1(argp);

		*finalfnexpr = (Expr *) makeFuncExpr(finalfn_oid,
											 agg_result_type,
											 args,
											 COERCE_DONTCARE);
	}

	/* prelim function */
	if (OidIsValid(prelimfn_oid))
	{
		/*
		 * Build expr tree for inverse transition function
		 */
		argp = makeNode(Param);
		argp->paramkind = PARAM_EXEC;
		argp->paramid = -1;
		argp->paramtype = agg_state_type;
		args = list_make1(argp);

		/* XXX: is agg_state_type correct here? */
		*prelimfnexpr = (Expr *) makeFuncExpr(prelimfn_oid, agg_state_type,
											  args, COERCE_DONTCARE);
	}
	
	/* inverse functions */
	if (OidIsValid(invtransfn_oid))
	{
		/*
		 * Build expr tree for inverse transition function
		 */
		argp = makeNode(Param);
		argp->paramkind = PARAM_EXEC;
		argp->paramid = -1;
		argp->paramtype = agg_state_type;
		args = list_make1(argp);

		*invtransfnexpr = (Expr *) makeFuncExpr(invtransfn_oid,
											 agg_state_type,
											 args,
											 COERCE_DONTCARE);
	}

	if (OidIsValid(invprelimfn_oid))
	{
		/*
		 * Build expr tree for inverse prelim function
		 */
		argp = makeNode(Param);
		argp->paramkind = PARAM_EXEC;
		argp->paramid = -1;
		argp->paramtype = agg_state_type;
		args = list_make1(argp);

		*invprelimfnexpr = (Expr *) makeFuncExpr(invprelimfn_oid,
											 agg_state_type,
											 args,
											 COERCE_DONTCARE);
	}
	
}

/*
 * get_groupclause_exprs -
 *     Return a list of expressions appeared in a given GroupClause or
 *     GroupingClause.
 */
List*
get_groupclause_exprs(Node *grpcl, List *targetList)
{
	List *result = NIL;
	
	if ( !grpcl )
		return result;

	Assert(IsA(grpcl, GroupClause) ||
		   IsA(grpcl, GroupingClause) ||
		   IsA(grpcl, List));

	if (IsA(grpcl, GroupClause))
	{
		Node *node = get_sortgroupclause_expr((GroupClause*)grpcl, targetList);
		result = lappend(result, node);
	}

	else if (IsA(grpcl, GroupingClause))
	{
		ListCell* l;
		GroupingClause *gc = (GroupingClause*)grpcl;

		foreach(l, gc->groupsets)
		{
			result = list_concat(result,
								 get_groupclause_exprs((Node*)lfirst(l), targetList));
		}
	}

	else
	{
		List *exprs = (List *)grpcl;
		ListCell *lc;

		foreach (lc, exprs)
		{
			result = list_concat(result, get_groupclause_exprs((Node *)lfirst(lc),
															   targetList));
		}
	}

	return result;
}

/*
 * check_aggregate_ingroup -
 *     Check if any aggregates are appeared in a given GroupClause or
 *     GroupingClause. Report an error if this is the case.
 *
 * All relevant expressions defined in the given GroupClause or
 * GroupingClause are returned as a list.
 */
List*
check_aggregate_ingroup(Node *grpcl, List *targetList, List *groupClauses)
{
	List *exprs;
	ListCell *l;
	List *result = groupClauses;

	if (grpcl == NULL)
		return result;

	Assert(IsA(grpcl, GroupClause) || IsA(grpcl, GroupingClause));

	exprs = get_groupclause_exprs(grpcl, targetList);

	foreach(l, exprs)
	{
		Node *expr = (Node*)lfirst(l);

		if (expr == NULL)
			continue;          /* probably cannot happen */
		if (checkExprHasAggs(expr))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("aggregates not allowed in GROUP BY clause"),
							 errOmitLocation(true)));

		if (checkExprHasGroupExtFuncs(expr))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("grouping() or group_id() not allowed in GROUP BY clause"),
							 errOmitLocation(true)));

		if (checkExprHasWindFuncs(expr))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("window functions not allowed in GROUP BY clause"),
							 errOmitLocation(true)));
		result = lcons(expr, result);
	}

	return result;
}

static bool
checkExprHasWindFuncs_walker(Node *node, checkHasWindFuncs_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowRef))
	{
		if (((WindowRef *) node)->winlevelsup == context->sublevels_up)
			return true;		/* abort the tree traversal and return true */
		/* else fall through to examine argument */
	}
	else if (IsA(node, SortBy))
	{
		SortBy *s = (SortBy *)node;
		return checkExprHasWindFuncs_walker(s->node, context);
	}
	else if (IsA(node, WindowFrame))
	{
		WindowFrame *f = (WindowFrame *)node;
		if (checkExprHasWindFuncs_walker((Node *)f->trail, context))
			return true;
		if (checkExprHasWindFuncs_walker((Node *)f->lead, context))
			return true;
	}
	else if (IsA(node, WindowFrameEdge))
	{
		WindowFrameEdge *e = (WindowFrameEdge *)node;

		return checkExprHasWindFuncs_walker(e->val, context);
	}
	else if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   checkExprHasWindFuncs_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	else if(IsA(node, A_Expr))
	{
		/* could be seen inside an untransformed window clause */
		return false;
	}
	else if(IsA(node, ColumnRef))
	{
		/* could be seen inside an untransformed window clause */
		return false;
	}

	else if (IsA(node, A_Const))
	{
		/* could be seen inside an untransformed window clause */
		return false;
	}

	else if (IsA(node, TypeCast))
	{
		/* could be seen inside an untransformed window clause */
		return false;
	}
	
	return expression_tree_walker(node, checkExprHasWindFuncs_walker,
								  (void *) context);
}

/*
 * checkExprHasWindFuncs -
 *	Check if an expression contains a window function call.
 *
 * The objective of this routine is to detect whether there are window functions
 * belonging to the initial query level. Window functions belonging to 
 * subqueries or outer queries do NOT cause a true result.  We must recurse into
 * subqueries to detect outer-reference window functions that logically belong 
 * to the initial query level.
 *
 * Compare this function to checkExprHasAggs().
 */
bool
checkExprHasWindFuncs(Node *node)
{
	checkHasWindFuncs_context context;
	context.sublevels_up = 0;
	
	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   checkExprHasWindFuncs_walker,
										   (void *) &context, 0);
}

static bool
checkExprHasGroupExtFuncs_walker(Node *node, checkHasGroupExtFuncs_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, GroupingFunc) || IsA(node, GroupId))
	{
		/* XXX do GroupingFunc or GroupId need 'levelsup'? */
		return true;		/* abort the tree traversal and return true */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   checkExprHasGroupExtFuncs_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, checkExprHasGroupExtFuncs_walker,
								  (void *) context);
}

/*
 * checkExprHasGroupExtFuncs -
 *  Check if an expression contains a grouping() or group_id() call.
 *
 * 
 * The objective of this routine is to detect whether there are window functions
 * belonging to the initial query level. Window functions belonging to 
 * subqueries or outer queries do NOT cause a true result.  We must recurse into
 * subqueries to detect outer-reference window functions that logically belong 
 * to the initial query level.
 *
 * Compare this function to checkExprHasAggs().
 */
bool
checkExprHasGroupExtFuncs(Node *node)
{
	checkHasGroupExtFuncs_context context;
	context.sublevels_up = 0;
	
	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   checkExprHasGroupExtFuncs_walker,
										   (void *) &context, 0);	
}

/*
 * transformWindowSpec
 *
 * Transform the expressions inside a "WindowSpec" structure.
 *
 * This is used both for window specifications in a WINDOW clause
 * and for window or sequence references in an OVER clause.
 */
void
transformWindowSpec(ParseState *pstate, WindowSpec *spec)
{
	ListCell *lc2;
	List *new = NIL;

	foreach(lc2, spec->partition)
		{
			Node *n = (Node *)lfirst(lc2);
			SortBy *sb;

			Assert(IsA(n, SortBy));

			sb = (SortBy *)n;

			sb->node = (Node *)transformExpr(pstate, sb->node);
			new = lappend(new, (void *)sb);
		}
	spec->partition = new;
		
	new = NIL;
	foreach(lc2, spec->order)
		{
			Node *n = (Node *)lfirst(lc2);
			SortBy *sb;

			Assert(IsA(n, SortBy));

			sb = (SortBy *)n;

			sb->node = (Node *)transformExpr(pstate, sb->node);
			new = lappend(new, (void *)sb);
		}
	spec->order = new;

	if (spec->frame)
	{
		WindowFrame *frame = spec->frame;

		if (frame->trail)
			frame->trail->val = transformExpr(pstate, frame->trail->val);
		if (frame->lead)
			frame->lead->val = transformExpr(pstate, frame->lead->val);

	}
}

/* transformSequenceDefn
 *
 * Transform the expressions inside a SequenceDefn structure.
 */
void 
transformSequenceDefn(ParseState *pstate, SequenceDefn *defn)
{
	defn->span = (Node*)transformExpr(pstate, defn->span);
	defn->seqlow = (Node*)transformExpr(pstate, defn->seqlow);
	defn->seqlim = (Node*)transformExpr(pstate, defn->seqlim);
}

/* 
 * Create a parse-form expression tree representing a call to the window
 * function "sequence_key" on the argument sequence key over the sequence
 * window.  The idea is to depend on the overloads of "sequence_key" to
 * resolve to the correct implementation (e.g. "sequence_key_timestamp"),
 * when analysis transforms the expression tree.
 */
static Node*
make_sequence_key_call(const char *sequence_name, Node* sequence_key)
{     
    WindowSpec *spec = makeNode(WindowSpec);
    FuncCall *call = makeNode(FuncCall);
    
    spec->parent = pstrdup(sequence_name);
    spec->location = -1;
    
    call->funcname = SystemFuncName("sequence_key");
    call->args = list_make1(copyObject(sequence_key));
	call->agg_star = false;
	call->agg_distinct = false;
	call->func_variadic = false;
	call->over = (Node*)spec;
	call->location = -1;
	
	return (Node*)call;
}

/* 
 * Create a parse-form expression tree representing a call to the window
 * function "sequence_index".
 */
static Node*
make_sequence_index_call(const char *sequence_name)
{     
    WindowSpec *spec = makeNode(WindowSpec);
    FuncCall *call = makeNode(FuncCall);
    
    spec->parent = pstrdup(sequence_name);
    spec->location = -1;
    
    call->funcname = SystemFuncName("sequence_index");
    call->args = NIL;
	call->agg_star = false;
	call->agg_distinct = false;
	call->func_variadic = false;
	call->over = (Node*)spec;
	call->location = -1;
	
	return (Node*)call;
}

/* transformSequenceSpec
 *
 * Transform the expressions inside a SequenceSpec structure and cache 
 * various items derived from the SequenceSpec in the ParseState.  
 * This is okay, since there is only one SequenceSpec in a query block.
 * 
 * This is used only for sequence specification in a SEQUENCE clause.
 */
void
transformSequenceSpec(ParseState *pstate, SequenceSpec *spec)
{
	ListCell *lc2;
	List *new = NIL;
	Node *parse_ordering_key = NULL;
	Node *seqkey_sub = NULL;
	Node *seqind_sub = NULL;
	int n;
	
	foreach(lc2, spec->partition)
	{
		Node *n = (Node *)lfirst(lc2);
		SortBy *sb;
		
		Assert(IsA(n, SortBy));
		
		sb = (SortBy *)n;
		
		sb->node = (Node *)transformExpr(pstate, sb->node);
		new = lappend(new, (void *)sb);
	}
	spec->partition = new;
	
	new = NIL;
	n = list_length(spec->order);
	
	if ( n == 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("no ORDER BY specified for SEQUENCE"),
				 errOmitLocation(false)));
	}
	else if ( n != 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("more than one ORDER BY column specified for SEQUENCE"),
				 errOmitLocation(false)));
	}
	else 
	{
		Node *n = (Node *)linitial(spec->order);
		SortBy *sb;
		
		Assert(IsA(n, SortBy));
		
		sb = (SortBy *)n;
		
		if ( sb->sortby_kind != SORTBY_ASC )
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("non-ascending ORDER BY specified for SEQUENCE"),
					 errOmitLocation(false)));
			
		}
		
		parse_ordering_key = sb->node;
		sb->node = (Node *)transformExpr(pstate, parse_ordering_key);
		new = lappend(new, (void *)sb);
		
	}
	spec->order = new;
	
	transformSequenceDefn(pstate, spec->seqdef);
	
	Assert(pstate->p_seq_name == NULL);
	Assert(pstate->p_seqkey_name == NULL);
	Assert(pstate->p_seqkey_transformed == NULL);
	
	pstate->p_seq_name = pstrdup(spec->seqname);
	pstate->p_seqkey_name = pstrdup(spec->seqdef->keyname);
	seqkey_sub = make_sequence_key_call(spec->seqname, parse_ordering_key);
	seqind_sub = make_sequence_index_call(spec->seqname);
	pstate->p_seqkey_transformed = transformExpr(pstate, seqkey_sub);
	pstate->p_seqind_name = pstrdup("sequence_index");
	pstate->p_seqind_transformed = transformExpr(pstate, seqind_sub);
}
 

/*
 * transformWindowSpecExprs
 *
 * Member p_win_clauses may be an empty list, a list of WindowSpec, or
 * a list of one SequenceSpec.  The grammar should assure us there are
 * no other possibilities, but be defensive anyway.
 *
 * Do a quick pre-process of WindowSpecs or SequenceSpec to transform 
 * expressions into  something the rest of the parser will recognise.
 */
void
transformWindowSpecExprs(ParseState *pstate)
{
	ListCell *lc;
	SequenceSpec *s = getSequenceSpec(pstate->p_win_clauses);
	
	if ( s != NULL )
	{		
	    transformSequenceSpec(pstate, s);
		return;
	}

	foreach(lc, pstate->p_win_clauses)
	{
		WindowSpec *s = (WindowSpec *)lfirst(lc);
		transformWindowSpec(pstate, s);
	}
}

/*
 * transformTargetListSequenced -
 *	  transform the target list of a SEQUENCE query so that it contains 
 *    tles for both the sequence key and the sequence index (as junk, 
 *    if necessary).  Assure that the entries have non-zero sort group
 *    reference values.   Mark p_hasWindFuncs true.
 *    
 * This is NOP on non-SEQUENCE queries (since we check for the sequence 
 * key substitution expression in the parse context.
 *
 * Since items may be added to the targetList (as a resjunk columns), the
 * targetList must be passed by reference.
 */
List *
transformTargetListSequenced(ParseState *pstate, List **targetList)
{
	ListCell   *lc_tle;
	TargetEntry *seq_key_tle;
	TargetEntry *seq_ind_tle;
	
	if ( !pstate->p_seqkey_transformed )
		return *targetList;
	Assert( pstate->p_seqkey_name != NULL );
	Assert( pstate->p_seqind_name != NULL );
	Assert( pstate->p_seqind_transformed != NULL );
	
	seq_key_tle = tlist_member(pstate->p_seqkey_transformed, *targetList);
	seq_ind_tle = tlist_member(pstate->p_seqind_transformed, *targetList);
	
	/* Assure existent of tles */
	if ( seq_key_tle == NULL || seq_ind_tle == NULL )
	{
		AttrNumber	maxresno = list_length(*targetList);
		
		foreach (lc_tle, *targetList)
		{
			TargetEntry *tle = (TargetEntry*)lfirst(lc_tle);
			if ( tle->resno > maxresno )
			{
				maxresno = tle->resno;
			}
		}
		
		if ( seq_key_tle == NULL )
		{
			seq_key_tle = makeTargetEntry((Expr*)copyObject(pstate->p_seqkey_transformed),
										  ++maxresno,
										  pstate->p_seqkey_name,
										  true);
			*targetList = lappend(*targetList, seq_key_tle);
		}
		
		if (seq_ind_tle == NULL )
		{
			seq_ind_tle = makeTargetEntry((Expr*)copyObject(pstate->p_seqind_transformed),
										  ++maxresno,
										  pstate->p_seqind_name,
										  true);
			*targetList = lappend(*targetList, seq_ind_tle);
		}
	}
	
	/* Assure usable sort group reference values. */
	if ( seq_key_tle->ressortgroupref == 0 || seq_ind_tle->ressortgroupref == 0 )
	{
		Index maxsgr = maxSortGroupRef(*targetList, true);
		
		if ( seq_key_tle->ressortgroupref == 0 )
		{
			seq_key_tle->ressortgroupref = ++maxsgr;
		}
		
		if ( seq_ind_tle->ressortgroupref == 0 )
		{
			seq_ind_tle->ressortgroupref = ++maxsgr;
		}
	}
	
    pstate->p_hasWindFuncs = true;
	return *targetList;
}


/*
 * parseProcessWindFuncs
 *
 * Do some final checks on window functions and do post processing
 * on queries with grouping and window functions.
 */
void
parseProcessWindFuncs(ParseState *pstate, Query *qry)
{
	/* This should only be called if we found window functions */
	Assert(pstate->p_hasWindFuncs);

	/*
	 * Window functions must never appear in WHERE or 
	 * JOIN/ON clauses.
	 *
	 * (Note this check should appear first to deliver an appropriate error
	 * message; otherwise we are likely to complain about some innocent
	 * variable in the target list, which is outright misleading if the
	 * problem is in WHERE.)
	 */

	if (checkExprHasWindFuncs(qry->jointree->quals))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("window functions not allowed in WHERE clause"),
						 errOmitLocation(true)));
/*	if (checkExprHasAggs((Node *) qry->jointree->fromlist)) */
	if (checkExprHasWindFuncs((Node *) qry->jointree->fromlist))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("window functions not allowed in JOIN conditions"),
						 errOmitLocation(true)));
}
