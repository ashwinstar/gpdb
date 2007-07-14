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

#include "nodes/plannodes.h"
#include "nodes/params.h"

extern Plan *apply_motion(Plan *plan, Query *query);

extern Motion *make_union_motion(Plan *lefttree, bool sendToSecondary,
                                 int numInputSegs, int destSegIndex);
extern Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary,
                                        int numInputSegs, int destSegIndex,
				int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators);
extern Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary,
				    int numInputSegs, List *hashExpr);

extern Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary,
                                     int numInputSegs);

extern void add_slice_to_motion(Motion *m,
		MotionType motionType, List *hashExpr, 
		int numOutputSegs, int *outputSegIdx 
		);

/*
 * Context for apply shareinput
 */
typedef struct ApplyShareInputContext
{
	List *sharedNodes;
	List *sliceMarks;
	List *motStack;
} ApplyShareInputContext;

extern Plan *apply_shareinput_dag_to_tree(Plan *plan, ApplyShareInputContext *ctxt); 
extern Plan *apply_shareinput_xslice(Plan *plan, ApplyShareInputContext *ctxt);
#endif   /* CDBMUTATE_H */
