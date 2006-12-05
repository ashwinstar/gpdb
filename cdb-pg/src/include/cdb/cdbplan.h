/*-------------------------------------------------------------------------
 *
 * cdbplan.h
 *	  definitions for cdbplan.c utilities
 *
 * Copyright (c) 2004-2006, Greenplum
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

#include "nodes/nodes.h"		/* need to see definition of Node */
#include "nodes/plannodes.h"	/* need to see definition of Plan */

/* Generally useful tools, frameworks */
extern bool
			plan_tree_walker(Node *node, bool (*walker) (), void *context);

extern Node *
			plan_tree_mutator(Node *node, Node *(*mutator) (), void *context);

#endif   /* CDBPLAN_H */
