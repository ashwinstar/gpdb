/*-------------------------------------------------------------------------
 *
 * cdbllize.h
 *	  definitions for cdbmutate.c utilities
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBMUTATE_H
#define CDBMUTATE_H

#include "nodes/plannodes.h"
#include "nodes/params.h"
#include "nodes/relation.h"

extern Plan *apply_motion(struct PlannerInfo *root, Plan *plan, Query *query);

extern Motion *make_union_motion(Plan *lefttree, bool sendToSecondary,
		                                int destSegIndex);
extern Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary,
                                        int destSegIndex,
				int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators);
extern Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary,
				    List *hashExpr);

extern Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary );

extern Motion *make_explicit_motion(Plan *lefttree, bool sendToSecondary, AttrNumber segidColIdx);

void 
cdbmutate_warn_ctid_without_segid(struct PlannerInfo *root, struct RelOptInfo *rel);

extern void add_slice_to_motion(Motion *m,
		MotionType motionType, List *hashExpr, 
		int numOutputSegs, int *outputSegIdx 
		);

extern Plan *zap_trivial_result(PlannerInfo *root, Plan *plan); 
extern Plan *apply_shareinput_dag_to_tree(Plan *plan, ApplyShareInputContext *ctxt); 
extern Plan *apply_shareinput_xslice(Plan *plan, ApplyShareInputContext *ctxt);
extern void assign_plannode_id(PlannedStmt *stmt);

#endif   /* CDBMUTATE_H */
