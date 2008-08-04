/*-------------------------------------------------------------------------
 *
 * nodeGroup.h
 *	  prototypes for nodeGroup.c
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeGroup.h,v 1.30 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGROUP_H
#define NODEGROUP_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsGroup(Group *node);
extern GroupState *ExecInitGroup(Group *node, EState *estate, int eflags);
extern TupleTableSlot *ExecGroup(GroupState *node);
extern void ExecEndGroup(GroupState *node);
extern void ExecReScanGroup(GroupState *node, ExprContext *exprCtxt);

enum {
	GPMON_GROUP_TOTAL = GPMON_QEXEC_M_NODE_START,
};

static inline gpmon_packet_t * GpmonPktFromGroupState(GroupState *node)
{
	return &node->ss.ps.gpmon_pkt;
}

#endif   /* NODEGROUP_H */
