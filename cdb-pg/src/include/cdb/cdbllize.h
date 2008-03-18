/*-------------------------------------------------------------------------
 *
 * cdbllize.h
 *	  definitions for cdbplan.c utilities
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBLLIZE_H
#define CDBLLIZE_H

#include "nodes/nodes.h"                    /* Node, FlowType */

struct Flow;                                /* #include "nodes/primnodes.h" */
struct List;                                /* #include "nodes/pg_list.h" */
struct Plan;                                /* #include "nodes/plannodes.h" */
struct PlannedStmt;                         /* #include "nodes/plannodes.h" */
struct PlannerGlobal;                       /* #include "nodes/relation.h" */
struct Query;                               /* #include "nodes/parsenodes.h" */

struct Plan *
cdbparallelize(struct PlannerGlobal    *glob,
               struct PlannedStmt      *plannedStmt,
               struct Query            *query);

extern bool is_plan_node(Node *node);

extern struct Flow *makeFlow(FlowType flotype);

struct Flow *
pull_up_Flow(struct Plan *plan, struct Plan *subplan, bool withSort);

bool focusPlan(struct Plan *plan, bool live, bool stable, bool rescannable);
bool broadcastPlan(struct Plan *plan, bool live, bool stable, bool rescannable);

bool 
repartitionPlan(struct Plan    *plan, 
                bool            live, 
                bool            stable, 
                bool            rescannable, 
                struct List    *hashExpr);

#endif   /* CDBLLIZE_H */
