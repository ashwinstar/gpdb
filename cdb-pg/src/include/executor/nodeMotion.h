/*-------------------------------------------------------------------------
 *
 * nodeMotion.h
 *
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMOTION_H
#define NODEMOTION_H

#include "nodes/execnodes.h"

typedef enum HashMotionInfoType
{
	INFOTYPE_LEFTEXPR,	
	INFOTYPE_RIGHTEXPR,	
	INFOTYPE_OP	
} HashMotionInfoType;

extern int	ExecCountSlotsMotion(Motion *node);
extern MotionState *ExecInitMotion(Motion *node, EState *estate);
extern TupleTableSlot *ExecMotion(MotionState *node);
extern void ExecEndMotion(MotionState *node);

extern List *getHashMotionInfoList( Plan *node, HashMotionInfoType type );

extern Motion *make_motion(Plan *lefttree, MotionType motionType,
			bool sendToSecondary, bool sendSorted, 
			int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
			int numInputSegs, int *inputSegIdx );
			
extern Motion *make_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex );
extern Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex, 
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
								int numInputSegs, int *inputSegIdx );
extern Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary, 
							int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp );
extern Motion *make_sorted_hashed_motion(Plan *lefttree, bool sendToSecondary, 
								int numHashSegs, int *hashSegIdx, List *hashExpr, List *hashOp,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
								int numInputSegs, int *inputSegIdx );
extern Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary);
extern Motion *make_sorted_broadcast_motion(Plan *lefttree, bool sendToSecondary, 
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, 
								int numInputSegs, int *inputSegIdx );
extern int *makeDefaultSegIdxArray(int numSegs);

#endif   /* NODEMOTION_H */
