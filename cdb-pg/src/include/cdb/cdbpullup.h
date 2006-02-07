/*-------------------------------------------------------------------------
 *
 * cdbpullup.h
 *    definitions for cdbpullup.c utilities
 *
 * Copyright (c)2006, greenplum inc
 *
 * IDENTIFICATION
 *    $Id$
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBPULLUP_H
#define CDBPULLUP_H

#include "nodes/nodes.h"        /* need to see definition of Node */
#include "nodes/plannodes.h"    /* need to see definition of Plan */


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
                 AttrNumber   **pProjectedColIdx);


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
Expr *
cdbpullup_expr(Expr *expr, List *targetlist, Index newvarno);


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
bool
cdbpullup_exprHasSubplanRef(Expr *expr);


/*
 * cdbpullup_findSubplanRefInTargetList
 *
 * Given a targetlist, returns ptr to first TargetEntry whose expr is a
 * Var node having the specified varattno, and having its varno in executor
 * format (varno is either 0 or OUTER) as set by set_plan_references().
 * Returns NULL if no such TargetEntry is found.
 *
 * (NB: Could also check for varno INNER, but we don't bother because that is
 * used only in joins and inner indexscans, which we don't expect to see here.)
 */
TargetEntry *
cdbpullup_findSubplanRefInTargetList(AttrNumber varattno, List *targetlist);


/*
 * cdbpullup_missing_var_walker
 *
 * Returns true if some Var in expr is not in targetlist.
 * 'targetlist' is a List of TargetEntry.
 */
bool
cdbpullup_missing_var_walker(Node *node, void *targetlist);


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
                      bool          useExecutorVarFormat);

/*
 * cdbpullup_targetlist
 *
 * Return a targetlist that can be attached to a parent plan yielding
 * the same projection as the given subplan.
 */
List *
cdbpullup_targetlist(Plan *subplan, bool useExecutorVarFormat);


#endif   /* CDBPULLUP_H */
