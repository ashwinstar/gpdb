/*-------------------------------------------------------------------------
 *
 * cdbpullup.c
 *    Provides routines supporting plan tree manipulation.
 *
 * Copyright (c)2006, greenplum inc
 *
 * IDENTIFICATION
 *    $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"            /* makeVar() */
#include "optimizer/clauses.h"          /* expression_tree_walker/mutator */
#include "optimizer/tlist.h"            /* tlist_member() */
#include "parser/parse_expr.h"          /* exprType() and exprTypmod() */
#include "parser/parsetree.h"           /* get_tle_by_resno() */

#include "cdb/cdbpullup.h"              /* me */


/*
 * cdbpullup_colIdx
 *
 * Given a [potentially] projecting Plan operator and a vector
 * of attribute numbers in the plan's single subplan, create a
 * vector of attribute numbers in the plan's targetlist.
 *
 * Parameters:
 *      plan -> the Plan node
 *      subplan -> the Plan node's subplan
 *      numCols = number of colIdx array elements
 *      subplanColIdx (IN) -> array [0..numCols-1] of AttrNumber, designating
 *          entries in the targetlist of the Plan operator's subplan.
 *     *pProjectedColIdx (OUT) -> array [0..n-1] of AttrNumber, palloc'd
 *          by this function, where n is the function's return value; or
 *          NULL when n == 0.
 *
 * Returns the number of leading subplanColIdx entries for which matching
 * Var nodes were found in the plan's targetlist.
 */
int
cdbpullup_colIdx(Plan          *plan,
                 Plan          *subplan,
                 int            numCols,
                 AttrNumber    *subplanColIdx,
                 AttrNumber   **pProjectedColIdx)
{
    TargetEntry    *tle;
    int             i;
    bool            useExecutorVarFormat;

    Assert(pProjectedColIdx);
    *pProjectedColIdx = NULL;

    /*
     * Look at an arbitrary Var node in the upper Plan node's targetlist
     * to see if it has been adjusted by set_plan_references().
     *
     * Before set_plan_references(), if Var nodes are equal(), then
     * they are equivalent in meaning, more or less.  Afterwards
     * that is no longer the case; each Var node is implicitly tied to
     * a particular evaluation context (i.e. a certain Plan node's
     * targetlist), which means expr trees for different contexts are
     * incomparable.
     */
    useExecutorVarFormat = cdbpullup_exprHasSubplanRef((Expr *)plan->targetlist);

    /* 
     * Translate the column index array. 
     */
    for (i = 0; i < numCols; i++)
    {
        Index   kidTargetIdx = subplanColIdx[i];

        /*
         * Search our targetlist for a Var that references the specified
         * item of the subplan's targetlist.  All Var nodes (at least, all
         * that we expect to encounter here) are converted to this form by
         * set_plan_references() at the end of optimization.
         */
        if (useExecutorVarFormat)
            tle = cdbpullup_findSubplanRefInTargetList(kidTargetIdx, plan->targetlist);

        /*
         * Iff set_plan_references() hasn't been called yet, a different
         * procedure is necessary: search our own targetlist for a copy of
         * the designated expr from the subplan targetlist.
         */
        else
        {
            /* Get subplan's targetlist entry. */
            tle = get_tle_by_resno(subplan->targetlist, kidTargetIdx);
            Assert(tle);

            /* Look for matching expr in the given plan's targetlist. */
            tle = tlist_member((Node *)tle->expr, plan->targetlist);
        }

        /* Quit if the plan's projection doesn't include this column. */
        if (!tle)
            break;

        if (i == 0)
            *pProjectedColIdx = palloc(numCols * sizeof((*pProjectedColIdx)[0]));

        (*pProjectedColIdx)[i] = tle->resno;
    }

    return i;
}                               /* cdbpullup_colIdx */


/*
 * cdbpullup_expr
 *
 * Suppose there is a Plan node 'P' whose projection is defined by
 * a targetlist 'TL', and which has subplan 'S'.  Given TL and an
 * expr 'X0' whose Var nodes reference the result columns of S, this
 * function returns a new expr 'X1' that is a copy of X0 with Var nodes
 * adjusted to reference the columns of S after their passage through P.
 *
 * Parameters:
 *      expr -> X0, the expr in terms of the subplan S's targetlist
 *      targetlist -> TL (a List of TargetEntry), the plan P's targetlist
 *              (or can be a List of Expr)
 *      newvarno = varno to be used in new Var nodes in executor format
 *              (should usually be OUTER; but specify 0 when the expr will
 *              be used in the targetlist of an Agg or Group node)
 *
 * Returns X1, the expr recast in terms of the given targetlist; or
 * NULL if X0 references a column of S that is not projected in TL.
 */
struct pullUpExpr_context
{
    List   *targetlist;
    Var    *notfound;
    Index   newvarno;
};

/* pullUpExpr_mutator pulls up Var nodes in executor format */
static Node*
pullUpExpr_mutator(Node *node, void *context)
{
    struct pullUpExpr_context  *ctx = (struct pullUpExpr_context *)context;
    Var            *var;
    TargetEntry    *tle;

    if (!node ||
        ctx->notfound)
        return NULL;

    /* Pull up Var. */
    if (IsA(node, Var))
    {
        /* Look for a matching Var in the targetlist. */
        var = (Var *)node;
        tle = cdbpullup_findSubplanRefInTargetList(var->varattno, ctx->targetlist);

        /* Fail if P's result does not include this column. */
        if (!tle)
        {
            ctx->notfound = var;
            return NULL;
        }

        /* Substitute a Var node referencing the targetlist entry. */
        var = (Var *)copyObject(tle->expr);
        var->varno = ctx->newvarno;
        var->varattno = tle->resno;
        return (Node *)var;
    }

    /* The whole expr might be in the targetlist. */
    else if (!IsA(node, List) &&
             NULL != (tle = tlist_member(node, ctx->targetlist)))
    {
        /* Replace expr with a Var node referencing the targetlist entry. */
        var = makeVar(ctx->newvarno,
                      tle->resno,
                      exprType((Node *) tle->expr),
                      exprTypmod((Node *) tle->expr),
                      0);
        return (Node *)var;
    }

    return expression_tree_mutator(node, pullUpExpr_mutator, context);
}                               /* pullUpExpr_mutator */

Expr *
cdbpullup_expr(Expr *expr, List *targetlist, Index newvarno)
{
    ListCell   *cell;
    Expr       *newexpr = NULL;
    bool        useExecutorVarFormat;

    /*
     * Above the Scan strata in a plan, we can tell by looking at any Var's 
     * varno whether or not set_plan_references() has converted the expr's 
     * Var nodes from parser format to executor format.  If so, we should
     * conform to the executor format.
     */
    useExecutorVarFormat = cdbpullup_exprHasSubplanRef((Expr *)targetlist);

    /*
     * Executor format: in each Var node, the varno is either 0 or OUTER (or
     * INNER, but we don't expect that here).  The varattno is the resno of 
     * an entry in the subplan's targetlist.
     */
    if (useExecutorVarFormat)
    {
        struct pullUpExpr_context   ctx;

        ctx.targetlist = targetlist;
        ctx.notfound = NULL;
        ctx.newvarno = newvarno;

        newexpr = (Expr *)pullUpExpr_mutator((Node *)expr, &ctx);

        if (ctx.notfound)
            newexpr = NULL;
    }

    /*
     * Parser format: in each Var node, the varno is an RTE index, >= 1.
     * The meaning of an expr is the same anywhere in the Query, so all
     * we have to do is copy it.  However, if it uses a Var that is not
     * in the targetlist, return NULL instead.
     */
    else
    {
        /* List of Expr?  Verify that all items are projected. */
        if (IsA(expr, List))
        {
            foreach(cell, (List *)expr)
            {
                Expr   *item = (Expr *)lfirst(cell);

                /* The whole expr or all of its Vars must be in targetlist. */
                if (cdbpullup_missing_var_walker((Node *)item, targetlist) &&
                    !tlist_member((Node *)item, targetlist))
                    return NULL;
            }
        }

        /* The whole expr or all of its Vars must be in targetlist. */
        else if (cdbpullup_missing_var_walker((Node *)expr, targetlist) &&
                 !tlist_member((Node *)expr, targetlist))
            return NULL;

        /* ok, copy it */
        newexpr = copyObject(expr);
    }

    return newexpr;
}                               /* cdbpullup_expr */


/*
 * cdbpullup_exprHasSubplanRef
 *
 * Returns true if the expr's first Var is a reference to an item in the
 * targetlist of the associated Plan node's subplan.  If so, it can be
 * assumed that the Plan node and associated exprs have been processed
 * by set_plan_references(), and its Var nodes are in executor format.
 *
 * Returns false otherwise, which implies no conclusion about whether or 
 * not set_plan_references() has been done.  
 *
 * Note that a Var node that belongs to a Scan operator and refers to the 
 * Scan's source relation or index, doesn't have its varno changed by 
 * set_plan_references() to OUTER/INNER/0.  Think twice about using this
 * unless you know that the expr comes from an upper Plan node.
 */

/* Find any Var in an expr */
static bool
findAnyVar_walker(Node *node, void *ppVar)
{
    if (!node)
        return false;
    if (IsA(node, Var))
    {
        *(Var **)ppVar = (Var *)node;
        return true;
    }
    return expression_tree_walker(node, findAnyVar_walker, ppVar);
}

bool
cdbpullup_exprHasSubplanRef(Expr *expr)
{
    Var    *var;

    if (!findAnyVar_walker((Node *)expr, &var))
        return false;

    return var->varno == OUTER ||
           var->varno == INNER ||
           var->varno == 0;
}                               /* cdbpullup_exprHasSubplanRef */


/*
 * cdbpullup_findSubplanRefInTargetList
 *
 * Given a targetlist, returns ptr to first TargetEntry whose expr is a
 * Var node having the specified varattno, and having its varno in executor
 * format (varno is OUTER, INNER, or 0) as set by set_plan_references().
 * Returns NULL if no such TargetEntry is found.
 */
TargetEntry *
cdbpullup_findSubplanRefInTargetList(AttrNumber varattno, List *targetlist)
{
    ListCell       *cell;
    TargetEntry    *tle;
    Var            *var;

    foreach(cell, targetlist)
    {
        tle = (TargetEntry *)lfirst(cell);
        if (IsA(tle->expr, Var))
        {
            var = (Var *)tle->expr;
            if (var->varattno == varattno)
            {
                if (var->varno == OUTER ||
                    var->varno == INNER ||
                    var->varno == 0)
                    return tle;
            }
        }
    }
    return NULL;
}                               /* cdbpullup_findSubplanRefInTargetList */


/*
 * cdbpullup_missing_var_walker
 *
 * Returns true if some Var in expr is not in targetlist.
 * 'targetlist' is a List of TargetEntry.
 */
bool
cdbpullup_missing_var_walker(Node *node, void *targetlist)
{
    if (!node)
        return false;

    if (IsA(node, Var))
    {
        if (tlist_member(node, (List *)targetlist))
            return false;       /* this Var ok - go on to check rest of expr */
        return true;            /* Var is not in targetlist - quit the walk */
    }

    return expression_tree_walker(node, cdbpullup_missing_var_walker, targetlist);
}                               /* cdbpullup_missing_var_walker */


/*
 * cdbpullup_targetentry
 *
 * Given a TargetEntry from a subplan's targetlist, this function returns a
 * new TargetEntry that can be used to pull the result value up into the
 * parent plan's projection.
 *
 * Parameters:
 *      subplan_targetentry is an item in the targetlist of the subplan S.
 *      newresno is the attribute number of the new TargetEntry (its
 *          position in P's targetlist).
 *      newvarno should be OUTER; except it should be 0 if P is an Agg or
 *          Group node.  It is ignored if useExecutorVarFormat is false.
 *      useExecutorVarFormat must be true if called after optimization, 
 *          when set_plan_references() has been called and Var nodes have 
 *          been adjusted to refer to items in the subplan's targetlist.  
 *          It must be false before the end of optimization, when Var 
 *          nodes are still in parser format, referring to RTE items.
 */
TargetEntry *
cdbpullup_targetentry(TargetEntry  *subplan_targetentry,
                      AttrNumber    newresno,
                      Index         newvarno,
                      bool          useExecutorVarFormat)
{
    TargetEntry    *kidtle = subplan_targetentry;
    TargetEntry    *newtle;

    /* After optimization, a Var's referent depends on the owning Plan node. */
    if (useExecutorVarFormat)
    {
        Expr   *kidexpr = kidtle->expr;
        Var    *var;

        /* If subplan target expr is Var, copy to preserve its EXPLAIN info. */
        if (IsA(kidexpr, Var))
        {
            newtle = copyObject(kidtle);
            var = (Var *)newtle->expr;
            var->varno = newvarno;
            var->varattno = kidtle->resno;
        }

        /* General expr */        
        else
        {
            /* Copy the subplan's TargetEntry, without its expr. */
            kidtle->expr = NULL;
            newtle = copyObject(kidtle);
            kidtle->expr = kidexpr;

            /* Make a Var node referencing the subplan's targetlist entry. */
            newtle->expr = (Expr *)makeVar(newvarno,
                                           kidtle->resno,
                                           exprType((Node *)kidexpr),
                                           exprTypmod((Node *)kidexpr),
                                           0);
        }       
    }

    /* Until then, a Var's referent is the same anywhere in a Query. */
    else
        newtle = copyObject(kidtle);

    newtle->resno = newresno;
    return newtle;
}                               /* cdbpullup_targetentry */


/*
 * cdbpullup_targetlist
 *
 * Return a targetlist that can be attached to a parent plan yielding
 * the same projection as the given subplan.
 */
List *
cdbpullup_targetlist(Plan *subplan, bool useExecutorVarFormat)
{
    ListCell   *cell;
    List       *targetlist = NIL;
    AttrNumber  resno = 1;

    foreach(cell, subplan->targetlist)
    {
        TargetEntry    *kidtle = (TargetEntry *)lfirst(cell);
        TargetEntry    *newtle = cdbpullup_targetentry(kidtle, 
                                                       resno++, 
                                                       OUTER,
                                                       useExecutorVarFormat);

        targetlist = lappend(targetlist, newtle);
    }

    return targetlist;
}                               /* cdbpullup_targetlist */

