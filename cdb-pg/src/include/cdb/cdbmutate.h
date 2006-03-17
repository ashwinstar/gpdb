/*-------------------------------------------------------------------------
 *
 * cdbllize.h
 *	  definitions for cdbmutate.c utilities
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBMUTATE_H
#define CDBMUTATE_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/params.h"
#include "cdb/cdbplan.h"

/*
 * An ApplyMotionState holds state for the recursive apply_motion_mutator().
 * It is externalized here to make it shareable by helper code in other
 * modules, e.g., cdbaggmutate.c.
 */
typedef struct ApplyMotionState
{
	int			nextMotionID;
    int         sliceDepth;
    int         numInitPlanMotionNodes;
    bool		parent_is_subplan;

	/*
	 * TODO Finish the mutator which will eventually need some state: At a
	 * minimum, a range table, and perhaps more.
	 */
}	ApplyMotionState;

extern Plan *apply_motion(Plan *plan, Query *query);
extern void assignMotionID(Node *newnode, ApplyMotionState * context, Node *oldnode);

/* Declarations of functions defined in cdbaggmutate.c */
void cdbaggmutate_transformToTwoStage(Agg *agg, bool useExecutorVarFormat);

/* Declarations of functions defined in cdblimmutate.c */
extern Plan *apply_limit_motion(Plan *plan, ApplyMotionState * state);


List *
getExprListFromTargetList(List         *tlist,
                          int           numCols,
                          AttrNumber   *colIdx,
                          bool          useExecutorVarFormat);

#ifdef UNUSED
extern Node *getHashExprForExpression(Plan *varsource, Expr *expr);
#endif /*UNUSED*/

Motion *
make_motion(Plan       *lefttree,
            MotionType  motionType,
            bool        sendToSecondary,
            bool        sendSorted,
            int         numInputSegs,
            List       *hashExpr,
            int         numOutputSegs,
            int        *outputSegIdx,
            int         numSortCols,
            AttrNumber *sortColIdx,
            Oid        *sortOperators);

extern Motion *make_union_motion(Plan *lefttree, bool sendToSecondary,
                                 int numInputSegs, int destSegIndex);
extern Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary,
                                        int numInputSegs, int destSegIndex,
				int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators);
extern Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary,
				    int numInputSegs, List *hashExpr);

#if UNUSED
extern Motion *make_sorted_hashed_motion(Plan *lefttree, bool sendToSecondary,
						   int numInputSegs, List *hashExpr,
				int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators);
#endif /*UNUSED*/

extern Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary,
                                     int numInputSegs);

#if UNUSED
extern Motion *make_sorted_broadcast_motion(Plan *lefttree, bool sendToSecondary,
				 int numInputSegs,
                 int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators);
#endif /*UNUSED*/

extern int *makeDefaultSegIdxArray(int numSegs);

extern bool findHashAttrsInTargetList(List *targetlist, List *hashExprs, AttrNumber *attrs);
extern List *getExprListForSubqueryScan(List *targetlist, List *hashExprs);

#endif   /* CDBMUTATE_H */
