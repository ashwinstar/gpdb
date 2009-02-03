/*-------------------------------------------------------------------------
 *
 * cdbsubselect.c
 *	  Flattens subqueries, transforms them to joins.
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"            /* INT8OID */
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/subselect.h"        /* convert_testexpr() */
#include "optimizer/prep.h"              /* canonicalize_qual() */
#include "optimizer/var.h"              /* contain_vars_of_level_or_above() */
#include "parser/parse_oper.h"          /* make_op() */
#include "parser/parse_relation.h"      /* addRangeTableEntryForSubquery() */
#include "parser/parsetree.h"           /* rt_fetch() */
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"            /* get_op_btree_interpretation() */

#include "cdb/cdbsubselect.h"           /* me */

static Node *
pull_up_IN_clauses(PlannerInfo *root, List** rtrlist_inout, Node *node);

static Node *
convert_IN_to_antijoin(PlannerInfo * root, List** rtrlist_inout, SubLink * sublink);

/*
 * cdbsubselect_flatten_sublinks
 */
void
cdbsubselect_flatten_sublinks(struct PlannerInfo *root, struct Node *jtnode)
{
    List       *rtrlist = NIL;
    ListCell   *cell;

    if (jtnode == NULL)
        return;

    switch (jtnode->type)
    {
        case T_Query:
        {
            Query   *query = (Query *)jtnode;

            /* Flatten sublinks in JOIN...ON and WHERE search conditions. */
            cdbsubselect_flatten_sublinks(root, (Node *)query->jointree);
            break;
        }

        case T_FromExpr:
        {
            FromExpr   *fromexpr = (FromExpr *)jtnode;

            /* Flatten sublinks in JOIN...ON search conditions. */
            foreach(cell, fromexpr->fromlist)
                cdbsubselect_flatten_sublinks(root, (Node *)lfirst(cell));

            /* Flatten sublinks in WHERE search condition. */
            fromexpr->quals = pull_up_IN_clauses(root, &rtrlist, fromexpr->quals);

            /* Append any new RangeTblRef nodes to the FROM clause. */
            if (rtrlist)
                fromexpr->fromlist = list_concat(fromexpr->fromlist, rtrlist);
            break;
        }
 
        case T_JoinExpr:
        {
            JoinExpr   *joinexpr = (JoinExpr *)jtnode;

            /* Process left and right inputs of JOIN. */
            cdbsubselect_flatten_sublinks(root, joinexpr->larg);
            cdbsubselect_flatten_sublinks(root, joinexpr->rarg);

            /* CDB TODO: Handle full outer joins. */
            if (joinexpr->jointype == JOIN_FULL)
                break;

            /* Flatten sublinks in JOIN...ON search condition. */
            joinexpr->quals = pull_up_IN_clauses(root, &rtrlist, joinexpr->quals);

            /* Add any new RangeTblRef nodes to the join. */
            joinexpr->subqfromlist = rtrlist;
            break;
        }

        case T_RangeTblRef:
            break;

        default:
            Assert(0);
    }
}                               /* cdbsubselect_flatten_sublinks */


/*
 * cdbsubselect_drop_distinct
 */
static void
cdbsubselect_drop_distinct(Query *subselect)
{
    if (subselect->limitCount == NULL &&
        subselect->limitOffset == NULL)
    {
        /* Delete DISTINCT. */
        subselect->distinctClause = NIL;

        /* Delete GROUP BY if subquery has no aggregates and no HAVING. */
        if (!subselect->hasAggs &&
            subselect->havingQual == NULL)
            subselect->groupClause = NIL;
    }
}                               /* cdbsubselect_drop_distinct */


/*
 * cdbsubselect_drop_orderby
 */
static void
cdbsubselect_drop_orderby(Query *subselect)
{
    if (subselect->limitCount == NULL &&
        subselect->limitOffset == NULL)
    {
        /* Delete ORDER BY. */
        subselect->sortClause = NIL;
    }
}                               /* cdbsubselect_drop_orderby */


/*
 * cdbsubselect_add_rte_and_ininfo
 */
static InClauseInfo *
cdbsubselect_add_rte_and_ininfo(PlannerInfo    *root, 
                                List          **rtrlist_inout, 
                                Query          *subselect,
                                const char     *aliasname)
{
	InClauseInfo   *ininfo;
	RangeTblEntry  *rte;
    RangeTblRef    *rtr;
	int			    rtindex;

    /* Build an InClauseInfo struct. */
	ininfo = makeNode(InClauseInfo);
    ininfo->sub_targetlist = NULL;

    /* Determine the index of the subquery RTE that we'll create below. */
	rtindex = list_length(root->parse->rtable) + 1;
	ininfo->righthand = bms_make_singleton(rtindex);

    /* Tell join planner to quell duplication of outer query result rows. */
	root->in_info_list = lappend(root->in_info_list, ininfo);

    /* Make a subquery RTE in the current query level. */
    rte = addRangeTableEntryForSubquery(NULL,
										subselect,
										makeAlias(aliasname, NIL),
										false);
	root->parse->rtable = lappend(root->parse->rtable, rte);

    /* Tell caller to augment the jointree with a reference to the new RTE. */
    rtr = makeNode(RangeTblRef);
	rtr->rtindex = rtindex;
    *rtrlist_inout = lappend(*rtrlist_inout, rtr);

    return ininfo;
}                               /* cdbsubselect_add_rte_and_ininfo */


/*
 * convert_EXISTS_to_join
 */
static Node *
convert_EXISTS_to_join(PlannerInfo *root, List** rtrlist_inout, SubLink *sublink)
{
	Query	       *subselect = (Query *)sublink->subselect;
    Node           *limitqual = NULL;
    Node           *lnode;
    Node           *rnode;
    Node           *node;

    Assert(IsA(subselect, Query));

    /*
     * 'LIMIT n' makes EXISTS false when n <= 0, and doesn't affect the outcome
     * when n > 0.  Delete subquery's LIMIT and build (0 < n) expr to be ANDed
     * into the parent qual.
     */
    if (subselect->limitCount)
    {
        rnode = copyObject(subselect->limitCount);
        IncrementVarSublevelsUp(rnode, -1, 1);
        lnode = (Node *)makeConst(INT8OID, sizeof(int64), Int64GetDatum(0),
                                  false, true);
        limitqual = (Node *)make_op(NULL, list_make1(makeString("<")),
                                    lnode, rnode, -1);
        subselect->limitCount = NULL;
    }

    /* CDB TODO: Set-returning function in tlist could return empty set. */
    if (expression_returns_set((Node *)subselect->targetList))
        ereport(ERROR, (errcode(ERRCODE_GP_FEATURE_NOT_YET),
                        errmsg("Set-returning function in EXISTS subquery: not yet implemented")
                        ));

    /*
     * Trivial EXISTS subquery can be eliminated altogether.  If subquery has
     * aggregates without GROUP BY or HAVING, its result is exactly one row
     * (assuming no errors), unless that row is discarded by LIMIT/OFFSET.
     */
    if (subselect->hasAggs &&
        subselect->groupClause == NIL &&
        subselect->havingQual == NULL)
    {
        /*
         * 'OFFSET m' falsifies EXISTS for m >= 1, and doesn't affect the
         * outcome for m < 1, given that the subquery yields at most one row.
         * Delete subquery's OFFSET and build (m < 1) expr to be anded with
         * the current query's WHERE clause.
         */
        if (subselect->limitOffset)
        {
            lnode = copyObject(subselect->limitOffset);
            IncrementVarSublevelsUp(lnode, -1, 1);
            rnode = (Node *)makeConst(INT8OID, sizeof(int64), Int64GetDatum(1),
                                      false, true);
            node = (Node *)make_op(NULL, list_make1(makeString("<")),
                                   lnode, rnode, -1);
            limitqual = make_and_qual(limitqual, node);
        }

        /* Replace trivial EXISTS(...) with TRUE if no LIMIT/OFFSET. */
        if (limitqual == NULL)
            limitqual = makeBoolConst(true, false);

        return limitqual;
    }

    /* Delete ORDER BY and DISTINCT. */
    subselect->sortClause = NIL;
    subselect->distinctClause = NIL;

    /* HAVING is the only place that could still contain aggregates.
	 * We can delete targetlist if there is no havingQual.
	 */
    if (subselect->havingQual == NULL)
	{
		subselect->targetList = NULL;
        subselect->hasAggs = false;
	}

    /* If HAVING has no aggregates, demote it to WHERE. */
    else if (!checkExprHasAggs(subselect->havingQual))
    {
        subselect->jointree->quals = make_and_qual(subselect->jointree->quals,
                                                   subselect->havingQual);
        subselect->havingQual = NULL;
        subselect->hasAggs = false;
    }

    /* Delete GROUP BY if no aggregates. */
    if (!subselect->hasAggs)
        subselect->groupClause = NIL;

    /*
     * If uncorrelated, the subquery will be executed only once.  Add LIMIT 1
     * and let the SubLink remain unflattened.  It will become an InitPlan.
     * (CDB TODO: Would it be better to go ahead and convert these to joins?)
     */
    if (!contain_vars_of_level_or_above(sublink->subselect, 1))
    {
        subselect->limitCount = (Node *)makeConst(INT8OID, sizeof(int64), Int64GetDatum(1),
                                                  false, true);
        return make_and_qual(limitqual, (Node *)sublink);
    }

    /*
     * Build subquery RTE, InClauseInfo, etc.
     */
    cdbsubselect_add_rte_and_ininfo(root, rtrlist_inout, subselect, "EXISTS_subquery");

#if 0
    /*
     * Update FROM clause, placing the subselect into the right-hand side
     * of a LEFT JOIN.
     */

    /* Replace targetlist with TRUE. (Any non-NULL value would do.) */
    node = makeBoolConst(true, false);
    tle = makeTargetEntry((Expr *)node, 1, NULL, false);
    /* $$$$ tle->resname must be filled in $$$$$$ */
    subselect->targetList = list_make1(tle);

    /* Make a Var node referencing the subplan's result. */
	lnode = (Node *)makeVar(rtindex, tle->resno,
							exprType((Node *)tle->expr),
							exprTypmod((Node *)tle->expr),
							0);
	ininfo->sub_targetlist = list_make1(copyObject(lnode));

    /* Build (t IS NOT NULL) predicate. */
    ntest = makeNode(NullTest);
	ntest->nulltesttype = IS_NOT_NULL;
	ntest->arg = (Expr *)lnode;

    return make_and_qual(limitqual, (Node *)ntest);
#endif

    return limitqual;
}                               /* convert_EXISTS_to_join */

/*
 * convert_IN_to_join: can we convert an IN SubLink to join style?
 *
 * This function was formerly in subselect.c.
 */
static Node *
convert_IN_to_join(PlannerInfo *root, List** rtrlist_inout, SubLink *sublink)
{
	Query	   *subselect = (Query *) sublink->subselect;
	int			rtindex;
	InClauseInfo *ininfo;
    bool        correlated;

    Assert(IsA(subselect, Query));

	/*
	 * The combining operators and left-hand expressions mustn't be volatile.
	 */
	if (contain_volatile_functions(sublink->testexpr))
		return (Node *)sublink;

    /*
     * If uncorrelated, and no Var nodes on lhs, the subquery will be executed
     * only once.  It should become an InitPlan, but make_subplan() doesn't
     * handle that case, so just flatten it for now.
     * CDB TODO: Let it become an InitPlan, so its QEs can be recycled.
     */
    correlated = contain_vars_of_level_or_above(sublink->subselect, 1);
#if 0
    if (!correlated &&
        !contain_vars_of_level_or_above((Node *)sublink->lefthand, 0))
		return (Node *)sublink;
#endif

#if 0
    /*
     * Transform
     *      c IN (SELECT e ... WHERE/HAVING q)
     * to
     *      EXISTS (SELECT ... WHERE/HAVING q AND c = e)
     */
#endif

    /*
	 * Okay, pull up the sub-select into top range table and jointree.
	 *
	 * We rely here on the assumption that the outer query has no references
	 * to the inner (necessarily true, other than the Vars that we build
	 * below). Therefore this is a lot easier than what pull_up_subqueries has
	 * to go through.
	 */
    ininfo = cdbsubselect_add_rte_and_ininfo(root, rtrlist_inout, subselect, "IN_subquery");

    /* Get the index of the subquery RTE that was just created. */
	rtindex = list_length(root->parse->rtable);
    Assert(rt_fetch(rtindex, root->parse->rtable)->subquery == subselect);

    /*
     * Uncorrelated "=ANY" subqueries can use JOIN_UNIQUE dedup technique.  We
	 * expect that the test expression will be either a single OpExpr, or an
	 * AND-clause containing OpExprs.  (If it's anything else then the parser
	 * must have determined that the operators have non-equality-like
	 * semantics.  In the OpExpr case we can't be sure what the operator's
	 * semantics are like, and must check for ourselves.)
     */
    ininfo->try_join_unique = false;
    if (!correlated &&
        sublink->testexpr)
    {
        if (IsA(sublink->testexpr, OpExpr))
	    {
		    List	   *opclasses;
		    List	   *opstrats;

		    get_op_btree_interpretation(((OpExpr *) sublink->testexpr)->opno,
									    &opclasses, &opstrats);
		    if (list_member_int(opstrats, ROWCOMPARE_EQ))
			    ininfo->try_join_unique = true;
	    }
	    else if (and_clause(sublink->testexpr))
		    ininfo->try_join_unique = true;
    }

	/*
	 * Build the result qual expression.  As a side effect,
	 * ininfo->sub_targetlist is filled with a list of Vars representing the
	 * subselect outputs.
	 */
	return convert_testexpr(sublink->testexpr,
							rtindex,
							&ininfo->sub_targetlist,
							subselect->targetList);
}                               /* convert_IN_to_join */


/*
 * convert_sublink_to_join: can we convert a SubLink to join style?
 *
 * The caller has found a SubLink at the top level of WHERE, but has not
 * checked the properties of the SubLink at all.  Decide whether it is
 * appropriate to process this SubLink in join style.  If so, build the
 * qual clause(s) to replace the SubLink, and return them.  Else return
 * the original sublink.  In either case the subquery is modified in
 * place to remove unnecessary clauses.
 *
 * Side effects of a successful conversion include adding the SubLink's
 * subselect to the query's rangetable and adding an InClauseInfo node to
 * its in_info_list.
 *
 * Upon creating an RTE for a flattened subquery, a corresponding RangeTblRef
 * node is appended to the caller's List referenced by *rtrlist_inout, so the
 * caller can add it to the jointree.
 */
static Node *
convert_sublink_to_join(PlannerInfo *root, List** rtrlist_inout, SubLink *sublink)
{
    Node   *result = (Node *)sublink;
	Query  *subselect = (Query *)sublink->subselect;

    Assert(IsA(sublink, SubLink) &&
           IsA(subselect, Query));

    switch (sublink->subLinkType)
    {
        case EXISTS_SUBLINK:
            result = convert_EXISTS_to_join(root, rtrlist_inout, sublink);
            break;

        case ALL_SUBLINK:
			result = convert_IN_to_antijoin(root, rtrlist_inout, sublink);
            break;

        case ANY_SUBLINK:
            cdbsubselect_drop_orderby(subselect);
            cdbsubselect_drop_distinct(subselect);
            result = convert_IN_to_join(root, rtrlist_inout, sublink);
            break;

        case ROWCOMPARE_SUBLINK:
        case EXPR_SUBLINK:
            cdbsubselect_drop_orderby(subselect);
            break;

        case ARRAY_SUBLINK:
            break;

        default:
            Insist(0);
    }
    return result;
}                               /* convert_sublink_to_join */

/* NOTIN subquery transformation -start */

/* check if NOT IN conversion to antijoin is possible */
static bool
safe_to_convert_NOTIN(SubLink * sublink)
{
	Query	   *subselect = (Query *) sublink->subselect;
	Relids		left_varnos;

	/* cases we don't currently handle are listed below. */

	/* ARRAY sublinks have empty test expressions */
	if (sublink->testexpr == NULL)
		return false;

	/* No volatile functions in the subquery */
	if (contain_volatile_functions(sublink->testexpr))
		return false;

	/* Left-hand expressions must contain some Vars of the current */
	left_varnos = pull_varnos(sublink->testexpr);
	if (bms_is_empty(left_varnos))
		return false;

	/* Correlation - subquery referencing Vars of parent not handled */
	if (contain_vars_of_level((Node *) subselect, 1))
		return false;

	/* No set operations in the subquery */
	if (subselect->setOperations)
		return false;

	/* No more than one outer relation reference in the test expression */
	if (bms_num_members(left_varnos) != 1)
		return false;

	return true;
}

/*
 * Given fromlist of a jointree, locate and return a jointree node with the
 * given rtindex. We assume this has got to be either a RangeTblRef or a
 * JoinExpr. In the JoinExpr case, we may have to recurse to find the reference.
 */
static Node *
get_fromlist_item(Node *jtnode, int rtindx)
{
	Node	   *match = NULL;

	if (IsA(jtnode, FromExpr))
	{
		List	   *fromlist = ((FromExpr *) jtnode)->fromlist;
		ListCell   *curr;

		foreach(curr, fromlist)
		{
			Node	   *flnode = (Node *) lfirst(curr);

			match = get_fromlist_item(flnode, rtindx);
			if (match)
				break;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *jexpr = (JoinExpr *) jtnode;

		if (jexpr->rtindex == rtindx)
			match = (Node *) jexpr;
		else
		{
			Node	   *l_match = get_fromlist_item(jexpr->larg, rtindx);
			Node	   *r_match = get_fromlist_item(jexpr->rarg, rtindx);

			if (l_match || r_match)
				match = jtnode;
		}
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) jtnode;

		if (rtr->rtindex == rtindx)
			match = (Node *) rtr;
	}
	return match;
}


/*
 * Find if the supplied targetlist has any resjunk
 * entries. We only have to check the tail since
 * resjunks (if any) can only appear in the end.
 */
static bool inline
has_resjunk(List *tlist)
{
	bool		resjunk = false;
	Node	   *tlnode = (Node *) (lfirst(tlist->tail));

	if (IsA(tlnode, TargetEntry))
	{
		TargetEntry *te = (TargetEntry *) tlnode;

		if (te->resjunk)
			resjunk = true;
	}
	return resjunk;
}

/* add a dummy constant var to the end of the supplied list */
static List *
add_dummy_const(List *tlist)
{
	TargetEntry *dummy;
	Const	   *zconst;
	int			resno;

	zconst = makeConst(INT4OID, sizeof(int32), (Datum) 0,
					   false, true);	/* isnull, byval */
	resno = list_length(tlist) + 1;
	dummy = makeTargetEntry((Expr *) zconst,
							resno,
							"zero",
							false /* resjunk */ );

	if (tlist == NIL)
		tlist = list_make1(dummy);
	else
		tlist = lappend(tlist, dummy);

	return tlist;
}

/* Add a dummy variable to the supplied target list. The
 * variable is added to end of targetlist but before all
 * resjunk vars (if any). The caller should make use of
 * the returned targetlist since this code might modify
 * the list in-place.
 */
static List *
mutate_targetlist(List *tlist)
{
	List	   *new_list = NIL;

	if (has_resjunk(tlist))
	{
		ListCell   *prev = NULL;
		ListCell   *curr = NULL;
		bool		junk = false;

		foreach(curr, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(curr);

			if (tle->resjunk)
			{
				tle->resno = tle->resno + 1;
				if (!junk)
				{
					junk = true;
					new_list = add_dummy_const(new_list);
				}
			}
			new_list = lappend(new_list, tle);
			prev = curr;
		}
	}
	else
	{
		new_list = tlist;
		new_list = add_dummy_const(new_list);
	}
	return new_list;
}

/* Pulls up the subquery into the top-level range table.
 * Before that add a dummy column zero to the target list
 * of the subquery.
 */
static int
add_notin_subquery_rte(Query * parse, Query * subselect)
{
	RangeTblEntry *subq_rte;
	int			subq_indx;

	subselect->targetList = mutate_targetlist(subselect->targetList);
	subq_rte = addRangeTableEntryForSubquery(NULL,		/* pstate */
											 subselect,
											 makeAlias("NotIn_SUBQUERY", NIL),
											 false /* inFromClause */ );
	parse->rtable = lappend(parse->rtable, subq_rte);

	/* assume new rte is at end */
	subq_indx = list_length(parse->rtable);
	Assert(subq_rte = rt_fetch(subq_indx, parse->rtable));

	return subq_indx;
}

/*
 * Add a JOIN_LEFT join RTE entry to the top level range table
 * for NOT IN subquery flattening.
 */
static int
add_join_rte(Query * parse, int l_rtindex, int r_rtindex, int join_type)
{
	int			join_indx;
	List	   *l_colnames,
			   *r_colnames,
			   *j_colnames,
			   *l_colvars,
			   *r_colvars,
			   *j_colvars;
	RangeTblEntry *l_rte,
			   *r_rte,
			   *join_rte;

	l_rte = rt_fetch(l_rtindex, parse->rtable);
	r_rte = rt_fetch(r_rtindex, parse->rtable);

	expandRTE(l_rte, l_rtindex, 0, false,
			  &l_colnames, &l_colvars);
	expandRTE(r_rte, r_rtindex, 0, false,
			  &r_colnames, &r_colvars);

	/* make list of column names and vars to be output by the join */
	j_colnames = list_concat(l_colnames, r_colnames);
	j_colvars = list_concat(l_colvars, r_colvars);

	join_rte = addRangeTableEntryForJoin(NULL,	/* pstate */
										 j_colnames,
										 join_type,
										 j_colvars,
										 NULL,	/* alias */
										 false /* inFromClause */ );

	parse->rtable = lappend(parse->rtable, join_rte);

	/* assume new rte is at end */
	join_indx = list_length(parse->rtable);
	Assert(join_rte = rt_fetch(join_indx, parse->rtable));

	return join_indx;
}

/* Create a join expression linking the supplied larg node
 * with the pulled up NOT IN subquery located at r_rtindex
 * in the range table. The appropriate JOIN_RTE has already
 * been created by the caller and can be located at j_rtindex
 */
static JoinExpr *
make_join_expr(Node *larg, int r_rtindex, int j_rtindex, int join_type)
{
	JoinExpr   *jexpr;
	RangeTblRef *rhs;

	rhs = makeNode(RangeTblRef);
	rhs->rtindex = r_rtindex;

	jexpr = makeNode(JoinExpr);
	jexpr->jointype = join_type;
	jexpr->isNatural = false;
	jexpr->larg = larg;
	jexpr->rarg = (Node *) rhs;
	jexpr->rtindex = j_rtindex;

	return jexpr;
}

/* Convert subquery's test expr to a suitable predicate.*/
static Node *
make_join_on_quals(SubLink * sublink, int subquery_indx)
{
	Expr	   *join_pred;
	Query	   *subselect = (Query *) sublink->subselect;
	List	   *subtlist = NIL;

	join_pred = (Expr *) convert_testexpr(sublink->testexpr,
										  subquery_indx,
										  &subtlist,
										  subselect->targetList);

	join_pred = canonicalize_qual(make_notclause(join_pred));

	Assert(join_pred != NULL);
	return (Node *) join_pred;
}

/* add IS NOT FALSE clause on top of the clause */
static Node *
add_null_match_clause(Node *clause)
{
	BooleanTest *btest;

	Assert(clause != NULL);
	btest = makeNode(BooleanTest);
	btest->arg = (Expr *) clause;
	btest->booltesttype = IS_NOT_FALSE;
	return (Node *) btest;
}

/* 
 * Go through a jointree's fromlist entries and delete an entry
 * that matches the supplied delnode. This can only be a
 * RangeTblRef or a JoinExpr. See also resolvenew_in_jointree()
 */
static List *
remove_fromlist_item(List *fromlist, Node *delnode)
{
	ListCell   *prev = NULL;
	ListCell   *curr = NULL;

	foreach(curr, fromlist)
	{
		Node	   *jtnode = (Node *) lfirst(curr);

		if (equal(jtnode, delnode))
		{
			fromlist = list_delete_cell(fromlist, curr, prev);
			break;
		}
		prev = curr;
	}
	return fromlist;
}

/* 
 * verify whether the output columns produced by the given query is 
 * all non-nullable 
 */
static bool
is_inner_nonnullable (Query *subq)
{
	ListCell   *l;
	Node *quals = subq->jointree->quals;

	/* 
	 * a quick first check is to see if there are any non-nullable rels
	 * at all present in the query expression 
	 */
		
	Node *inner_qual = (Node *)make_ands_implicit ((Expr *)quals); 
	Relids nonnullable = find_nonnullable_rels (inner_qual);

	if (nonnullable == NULL || 
		bms_num_members(nonnullable) != list_length(subq->rtable))
		return false;

	/* 
	 * we now know some nonnullable rels exist, but still need to ensure
	 * if the attributes involved in the subq are nonnullable 
	 */
	foreach(l, subq->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node *node = (Node *) tle->expr;

		if (tle->resjunk)	/* ignore resjunk columns */
			continue;

		if (IsA (node, Var)) {	
			Var *var = (Var *) node;
			if (var->varlevelsup == 0 && 
				contain_var_reference (quals, var->varno, var->varattno, var->varlevelsup)) 
				return true;
		}
	}
	return false;
}

/* 
 * verify whether the attributes of the query that are referenced 
 * in the testexpr is all non-nullable
 */
static bool 
is_outer_nonnullable (Query *outer, Node *testexpr) {

	Node *quals = outer->jointree->quals;
	Node *outer_qual = (Node *)make_ands_implicit ((Expr *)quals);
	Relids		outer_rels = find_nonnullable_rels(outer_qual);
	Relids		left_varnos = pull_varnos(testexpr);

	if (outer_rels == NULL || 
		(!bms_is_subset(left_varnos, outer_rels))) 
		return false;
	else 
	{
		List *vars;
		ListCell *vl; 
		vars = pull_var_clause (testexpr, false); 
							
		foreach (vl, vars)  
		{
			Var *var = (Var *)lfirst (vl);
			if (var->varlevelsup == 0 && 
				contain_var_reference (quals, var->varno, var->varattno, var->varlevelsup)) 
				return true;
		}
		return false;
	}
	return false;
}

/* Create a null test. */
static Expr *
make_notin_quals(List *rtable, int j_rtindex, JoinType join_type)
{
	NullTest   *null_test = NULL;

	if (join_type == JOIN_LEFT)
	{
		RangeTblEntry *j_rte;
		Var		   *dummy;
		List	   *j_colnames,
				   *j_colvars;

		j_rte = rt_fetch(j_rtindex, rtable);
		expandRTE(j_rte, j_rtindex, 0, false,
				  &j_colnames, &j_colvars);

		null_test = makeNode(NullTest);
		null_test->nulltesttype = IS_NULL;
		dummy = (Var *) list_nth(j_colvars, list_length(j_colvars) - 1);
		null_test->arg = (Expr *) dummy;
	}
	else if (join_type == JOIN_LASJ)
		null_test = NULL;

	return (Expr *) null_test;
}

/*
 * convert_IN_to_antijoin: can we convert an ALL SubLink to join style?
 * If not appropriate to process this SubLink, return it as it is.
 * Side effects of a successful conversion include adding the SubLink's
 * subselect to the top-level rangetable, adding a JOIN RTE linking the outer
 * query with the subselect and setting up the qualifiers correctly.
 *
 * The transformation is to rewrite a query of the form:
 *		select c1 from t1 where c1 NOT IN (select c2 from t2);
 *						(to)
 *		select c1 from t1 left join (select 0 as zero, c2 from t2) foo
 *						  ON (c1 = c2) IS NOT FALSE where zero is NULL;
 *
 * The pseudoconstant column zero is needed to correctly pipe in the NULLs
 * from the subselect upstream.
 */
static Node *
convert_IN_to_antijoin(PlannerInfo * root, List **rtrlist_inout, SubLink * sublink)
{
	Expr	   *quals = (Expr *) sublink;
	Query	   *parse = root->parse;
	Query	   *subselect = (Query *) sublink->subselect;
	int			join_type = JOIN_LASJ;

	if (safe_to_convert_NOTIN(sublink))
	{
		Relids		left_varnos = pull_varnos(sublink->testexpr);
		int			larg_indx = bms_singleton_member(left_varnos);
		Node	   *larg = get_fromlist_item((Node *) parse->jointree, larg_indx);

		Assert(larg != NULL);

		if (larg != NULL)
		{
			int			subq_indx = add_notin_subquery_rte(parse, subselect);
			int			join_indx = add_join_rte(parse, larg_indx, subq_indx, join_type);
			bool		nonnullable = is_outer_nonnullable (parse, sublink->testexpr) && 
										is_inner_nonnullable(subselect);

			JoinExpr   *join_expr = make_join_expr(larg, subq_indx, join_indx, join_type);

			join_expr->quals = make_join_on_quals(sublink, subq_indx);

			if (nonnullable == false)
					join_expr->quals = add_null_match_clause(join_expr->quals);

			parse->jointree->fromlist = lappend(parse->jointree->fromlist, join_expr);
			parse->jointree->fromlist = remove_fromlist_item(parse->jointree->fromlist, larg);

			quals = make_notin_quals(parse->rtable, join_indx, join_type);
		}
	}

	return (Node *) quals;
}

/*
 * pull_up_IN_clauses
 *		Attempt to pull up top-level IN clauses to be treated like joins.
 *
 * A clause "foo IN (sub-SELECT)" appearing at the top level of WHERE can
 * be processed by pulling the sub-SELECT up to become a rangetable entry
 * and handling the implied equality comparisons as join operators (with
 * special join rules).
 * This optimization *only* works at the top level of WHERE, because
 * it cannot distinguish whether the IN ought to return FALSE or NULL in
 * cases involving NULL inputs.  This routine searches for such clauses
 * and does the necessary parsetree transformations if any are found.
 *
 * This routine has to run before preprocess_expression(), so the WHERE
 * clause is not yet reduced to implicit-AND format.  That means we need
 * to recursively search through explicit AND clauses, which are
 * probably only binary ANDs.  We stop as soon as we hit a non-AND item.
 *
 * Returns the possibly-modified version of the given qual-tree node.
 *
 * This function was formerly in prepjointree.c.
 */
Node *
pull_up_IN_clauses(PlannerInfo * root, List **rtrlist_inout, Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Node	   *subst;

		/* Is it a convertible IN clause?  If not, return it as-is */
		subst = convert_sublink_to_join(root, rtrlist_inout, sublink);
		return subst;
	}
	if (and_clause(node))
	{
		List	   *newclauses = NIL;
		ListCell   *l;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *oldclause = (Node *) lfirst(l);
			Node	   *newclause = pull_up_IN_clauses(root, rtrlist_inout, oldclause);

			if (newclause)
				newclauses = lappend(newclauses, newclause);
		}
		return (Node *) make_ands_explicit(newclauses);
	}
	if (not_clause(node))
	{
		Node	   *arg = (Node *) get_notclausearg((Expr *) node);

		/*
		 *	 We normalize NOT subqueries using the following axioms:
		 *
		 *		 val NOT IN (subq)		 =>  val <> ALL (subq)
		 *		 NOT val op ANY (subq)	 =>  val op' ALL (subq)
		 *		 NOT val op ALL (subq)	 =>  val op' ANY (subq)
		 */

		if (IsA(arg, SubLink))
		{
			SubLink    *sublink = (SubLink *) arg;

			if (sublink->subLinkType == ANY_SUBLINK)
				sublink->subLinkType = ALL_SUBLINK;
			else if (sublink->subLinkType == ALL_SUBLINK)
				sublink->subLinkType = ANY_SUBLINK;
			else
				return node;	/* do nothing for other sublinks */

			sublink->testexpr = (Node *) canonicalize_qual(
								 make_notclause((Expr *) sublink->testexpr));

			return (Node *) pull_up_IN_clauses(root, rtrlist_inout, (Node *) sublink);

		}
		else if (not_clause(arg))
		{
			/* NOT NOT (expr) => (expr)  */
			return (Node *) pull_up_IN_clauses(root, rtrlist_inout,
									(Node *) get_notclausearg((Expr *) arg));
		}
	}
	/* Stop if not an AND */
	return node;
}
