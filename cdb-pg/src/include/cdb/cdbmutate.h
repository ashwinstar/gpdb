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
    List       *initPlans;
}	ApplyMotionState;

extern Plan *apply_motion(Plan *plan, Query *query);
extern void assignMotionID(Node *newnode, ApplyMotionState * context, Node *oldnode);

List *
getExprListFromTargetList(List         *tlist,
                          int           numCols,
                          AttrNumber   *colIdx,
                          bool          useExecutorVarFormat);

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

extern Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary,
                                     int numInputSegs);

extern int *makeDefaultSegIdxArray(int numSegs);

extern bool findHashAttrsInTargetList(List *targetlist, List *hashExprs, AttrNumber *attrs);
extern List *getExprListForSubqueryScan(List *targetlist, List *hashExprs);

#endif   /* CDBMUTATE_H */
