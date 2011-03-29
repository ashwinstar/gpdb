/*-------------------------------------------------------------------------
 *
 * cdbllize.h
 *	  definitions for cdbplan.c utilities
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBLLIZE_H
#define CDBLLIZE_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/params.h"
#include "cdb/cdbplan.h"

extern Plan *cdbparallelize(struct PlannerInfo *root, Plan *plan, Query *query,
							int cursorOptions, 
							ParamListInfo boundParams, bool uses_correlation);

extern bool is_plan_node(Node *node);

extern Flow *makeFlow(FlowType flotype);

extern Flow *pull_up_Flow(Plan *plan, Plan *subplan, bool withSort);

bool focusPlan(Plan *plan, bool stable, bool rescannable);
bool repartitionPlan(Plan *plan, bool stable, bool rescannable, List *hashExpr);


#endif   /* CDBLLIZE_H */
