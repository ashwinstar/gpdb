/*-------------------------------------------------------------------------
 *
 * nodeLimit.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeLimit.h,v 1.13 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELIMIT_H
#define NODELIMIT_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsLimit(Limit *node);
extern LimitState *ExecInitLimit(Limit *node, EState *estate, int eflags);
extern TupleTableSlot *ExecLimit(LimitState *node);
extern void ExecEndLimit(LimitState *node);
extern void ExecReScanLimit(LimitState *node, ExprContext *exprCtxt);

enum {
	GPMON_LIMIT_TOTAL = 0,
};

static inline gpmon_packet_t * GpmonPktFromLimitState(LimitState *node)
{
	return &node->ps.gpmon_pkt;
}

#endif   /* NODELIMIT_H */
