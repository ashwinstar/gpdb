/*-------------------------------------------------------------------------
 *
 * cdbplan.h
 *	  definitions for cdbplan.c utilities
 *
 * Copyright (c) 2004, Metapa
 *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *	  See src/backend/utils/misc/guc.c for variable external specification.
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBPLAN_H
#define CDBPLAN_H

#include "nodes/nodes.h" /* need to see definition of Node */
#include "nodes/plannodes.h" /* need to see definition of Plan */

/* Generally useful tools, frameworks */
extern bool
plan_tree_walker(Node *node, bool (*walker) (), void *context);



/* Example exerise of plan_tree_walker framework. */

/* Caller needs to allocate this, so define it in header. */
typedef struct PlanInventory
{
	int scans;
	int vars;
} PlanInventory;

/* This is the example. */
extern bool plan_inventory(Plan *plan, PlanInventory *pinventory);



#endif	/* CDBPLAN_H */
