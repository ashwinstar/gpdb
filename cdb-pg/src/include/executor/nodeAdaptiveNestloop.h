/*-------------------------------------------------------------------------
 *
 * nodeNestloop.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeNestloop.h,v 1.23 2004/12/31 22:03:29 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEADAPTIVENESTLOOP_H
#define NODEADAPTIVENESTLOOP_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsAdaptiveNestLoop(AdaptiveNestLoop *node);
extern AdaptiveNestLoopState *ExecInitAdaptiveNestLoop(AdaptiveNestLoop *node, EState *estate, int eflags);
extern TupleTableSlot *ExecAdaptiveNestLoop(AdaptiveNestLoopState *node);
extern void ExecEndAdaptiveNestLoop(AdaptiveNestLoopState *node);
extern void ExecReScanAdaptiveNestLoop(AdaptiveNestLoopState *node, ExprContext *exprCtxt);

enum {
	NL_STRATEGY_UNDECIDED,
	NL_STRATEGY_LOOP,
	NL_STRATEGY_HASH
};
 
enum {
	GPMON_ADAPTIVENL_TOTAL = GPMON_QEXEC_M_NODE_START,
};

static inline gpmon_packet_t * GpmonPktFromANLState(AdaptiveNestLoopState *node)
{
	return &node->ps.gpmon_pkt;
}

#endif   /* NODEADAPTIVENESTLOOP_H */
