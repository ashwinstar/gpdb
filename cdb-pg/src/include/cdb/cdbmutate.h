/*-------------------------------------------------------------------------
 *
 * cdbllize.h
 *	  definitions for cdbmutate.c utilities
 *
 * Copyright (c) 2005, greenplum inc
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
	int nextMotionID;
	
	/* TODO Finish the mutator which will eventually need some state:
	 *      At a minimum, a range table, and perhaps more.
	 */
} ApplyMotionState;

extern Plan *apply_motion(Plan* plan, Query* query);
extern void assignMotionID(Node* newnode, ApplyMotionState* context, Node* oldnode);
extern bool invertColIdx(List *targetlist, int numCols, AttrNumber *colIdx, AttrNumber **pinverse);
extern bool invertGrpColIdx(Agg *agg, AttrNumber **pinverse);

/* Declarations of functions defined in cdbaggmutate.c */
extern Plan *apply_agg_motion(Plan *plan, ApplyMotionState *state);

/* Declarations of functions defined in cdblimmutate.c */
extern Plan *apply_limit_motion(Plan *plan, ApplyMotionState *state);


typedef enum HashMotionInfoType
{
	INFOTYPE_LEFTEXPR,	
	INFOTYPE_RIGHTEXPR	
} HashMotionInfoType;

extern List *getHashMotionInfoList( Plan *node, HashMotionInfoType type );

extern List *getExprListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx);


extern Motion *make_motion(Plan *lefttree, MotionType motionType,
			bool sendToSecondary, bool sendSorted,
			List *hashExpr,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators );
			
extern Motion *make_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex );
extern Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators );
extern Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary,
							      List *hashExpr);
extern Motion *make_sorted_hashed_motion(Plan *lefttree, bool sendToSecondary,
								List *hashExpr,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators );
extern Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary);
extern Motion *make_sorted_broadcast_motion(Plan *lefttree, bool sendToSecondary,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators );
extern void setHashToSelfOnly( Motion *motion );
extern void setFixedToSelfOnly( Motion *motion );

extern int *makeDefaultSegIdxArray(int numSegs);

extern bool findHashAttrsInTargetList(List* targetlist, List* hashExprs, AttrNumber* attrs);
extern List* getExprListForSubqueryScan(List* targetlist, List* hashExprs);

#endif /* CDBMUTATE_H */
