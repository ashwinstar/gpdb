/*-------------------------------------------------------------------------
 *
 * cdbllize.h
 *	  definitions for cdbplan.c utilities
 *
 * Copyright (c) 2004, Metapa
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

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/params.h"
#include "cdb/cdbplan.h"

extern Plan *
cdbparallelize(Plan* plan, Query *query, bool isCursor, int cursorOptions, ParamListInfo boundParams);

extern bool 
is_plan_node(Node* node);

extern Flow *
makeFlow(FlowType flotype);

#endif /* CDBLLIZE_H */
