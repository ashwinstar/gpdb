/*-------------------------------------------------------------------------
 *
 * nodeUnique.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeUnique.h,v 1.22 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEUNIQUE_H
#define NODEUNIQUE_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsUnique(Unique *node);
extern UniqueState *ExecInitUnique(Unique *node, EState *estate, int eflags);
extern TupleTableSlot *ExecUnique(UniqueState *node);
extern void ExecEndUnique(UniqueState *node);
extern void ExecReScanUnique(UniqueState *node, ExprContext *exprCtxt);

enum {
	GPMON_UNIQUE_INTUPLE = 0,
	GPMON_UNIQUE_OUTTUPLE,
	GPMON_UNIQUE_TOTAL,
};

static inline gpmon_packet_t * GpmonPktFromUniqueState(UniqueState *node) 
{
	return &node->ps.gpmon_pkt;
}
#endif   /* NODEUNIQUE_H */
