/*-------------------------------------------------------------------------
 *
 * cdbsubselect.c
 *	  Flattens subqueries, transforms them to joins.
 *
 * Copyright (c) 2005-2007, greenplum inc
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
#include "optimizer/var.h"              /* contain_vars_of_level_or_above() */
#include "parser/parse_oper.h"          /* make_op() */
#include "parser/parse_relation.h"      /* addRangeTableEntryForSubquery() */
#include "parser/parsetree.h"           /* rt_fetch() */
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"            /* get_op_btree_interpretation() */

#include "cdb/cdbsubselect.h"           /* me */

static Node *
pull_up_IN_clauses(PlannerInfo *root, List** rtrlist_inout, Node *node);


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
                                  false, false);
        limitqual = (Node *)make_op(NULL, list_make1(makeString("<")),
                                    lnode, rnode, -1);
        subselect->limitCount = NULL;
    }

    /* CDB TODO: Set-returning function in tlist could return empty set. */
    if (expression_returns_set((Node *)subselect->targetList))
        ereport(ERROR, (errcode(ERRCODE_MPP_FEATURE_NOT_YET),
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
                                      false, false);
            node = (Node *)make_op(NULL, list_make1(makeString("<")),
                                   lnode, rnode, -1);
            limitqual = make_and_qual(limitqual, node);
        }

        /* Replace trivial EXISTS(...) with TRUE if no LIMIT/OFFSET. */
        if (limitqual == NULL)
            limitqual = makeBoolConst(true, false);

        return limitqual;
    }

    /* Delete targetlist, ORDER BY and DISTINCT. */
    subselect->targetList = NULL;
    subselect->sortClause = NIL;
    subselect->distinctClause = NIL;

    /* HAVING is the only place that could still contain aggregates. */
    if (subselect->havingQual == NULL)
        subselect->hasAggs = false;

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
                                                  false, false);
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
            cdbsubselect_drop_orderby(subselect);
            cdbsubselect_drop_distinct(subselect);
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
pull_up_IN_clauses(PlannerInfo *root, List** rtrlist_inout, Node *node)
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
            Node       *newclause = pull_up_IN_clauses(root, rtrlist_inout, oldclause);

			if (newclause)
                newclauses = lappend(newclauses, newclause);
		}
		return (Node *)make_ands_explicit(newclauses);
	}
	/* Stop if not an AND */
	return node;
}
