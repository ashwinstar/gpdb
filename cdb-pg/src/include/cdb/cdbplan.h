/*-------------------------------------------------------------------------
 *
 * cdbplan.h
 *	  definitions for cdbplan.c utilities
 *
 * Copyright (c) 2004-2008, Greenplum inc
 *
 *
 * NOTES
 *	  See src/backend/utils/misc/guc.c for variable external specification.
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBPLAN_H
#define CDBPLAN_H

#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"

/* The plan associated with a SubPlan is found in a list.  During planning this is in 
 * the global structure found through the root PlannerInfo.  After planning this is in
 * the PlannedStmt.
 *
 * Structure plan_tree_base_prefix carries the appropriate pointer for GPDB's general plan
 * tree walker/mutator framework.  All users of the framework must prefix their context
 * structure with a plan_tree_base_prefix and initialize it appropriately.  
 */
typedef struct plan_tree_base_prefix
{
	Node *node; /* PlannerInfo* or PlannedStmt* */
} plan_tree_base_prefix;

/* Initialize a plan_tree_base_prefix during planning. */
static inline void planner_init_plan_tree_base(plan_tree_base_prefix *base, PlannerInfo *root) 
{
	base->node = (Node*)root;
}

/* Initialize a plan_tree_base_prefix from a PlannerGlobal. */
static inline void global_init_plan_tree_base(plan_tree_base_prefix *base, PlannerGlobal *glob) 
{
	base->node = (Node*)glob;
}

/* Initialize a plan_tree_base_prefix after planning. */
static inline void exec_init_plan_tree_base(plan_tree_base_prefix *base, PlannedStmt *stmt) 
{
	base->node = (Node*)stmt;
}

/* Initialize a plan_tree_base_prefix after planning. */
static inline void null_init_plan_tree_base(plan_tree_base_prefix *base) 
{
	base->node = NULL;
}

/* Return the plan associated with a SubPlan node in a walker.  (This is used by
 * framework, not by users of the framework.)
 */
static inline Plan *plan_tree_base_subplan_get_plan(plan_tree_base_prefix *base, SubPlan *subplan) 
{
	if (!base)
		return NULL;
	else if (IsA(base->node, PlannedStmt))
		return exec_subplan_get_plan((PlannedStmt*)base->node, subplan);
	else if (IsA(base->node, PlannerInfo))
		return planner_subplan_get_plan((PlannerInfo*)base->node, subplan);
	else if (IsA(base->node, PlannerGlobal))
	{
		PlannerInfo rootdata;
		rootdata.glob = (PlannerGlobal*)base->node;
		return planner_subplan_get_plan(&rootdata, subplan);
	}
	Insist(false);
	return NULL;
}

/* Rewrite the plan associated with a SubPlan node in a mutator.  (This is used by
 * framework, not by users of the framework.)
 */
static inline void plan_tree_base_subplan_put_plan(plan_tree_base_prefix *base, SubPlan *subplan, Plan *plan) 
{
	Assert(base);
	if (IsA(base->node, PlannedStmt))
	{
		exec_subplan_put_plan((PlannedStmt*)base->node, subplan, plan);
		return;
	}
	else if (IsA(base->node, PlannerInfo))
	{
		planner_subplan_put_plan((PlannerInfo*)base->node, subplan, plan);
		return;
	}
	Assert(false && "Must provide relevant base info.");
}


typedef struct XsliceInAppendContext
{
	plan_tree_base_prefix base;
	
	/* The slice number for the node that is being processed */
	int currentSliceNo;

	/* A set of slice numbers that contain cross-slice shared nodes */
	Bitmapset *slices;
} XsliceInAppendContext;

/* Generally useful tools, frameworks */
extern bool plan_tree_walker(Node *node, bool (*walker) (), void *context);

extern Node * plan_tree_mutator(Node *node, Node *(*mutator) (), void *context);

extern bool set_hasxslice_in_append_walker(Node *node, void *ctx);

#ifdef USE_ASSERT_CHECKING
extern List *extract_nodes(PlannerGlobal *glob, Node *node, int nodeTag);
#endif

#endif   /* CDBPLAN_H */
