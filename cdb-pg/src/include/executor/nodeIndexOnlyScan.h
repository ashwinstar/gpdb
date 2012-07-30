/*
 * nodeIndexOnlyScan.h
 *
 * Copyright (c) 2012 - present, EMC/Greenplum
 */
#ifndef NODEINDEXONLYSCAN_H
#define NODEINDEXONLYSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsIndexOnlyScan(IndexOnlyScan *node);
extern IndexOnlyScanState *ExecInitIndexOnlyScan(IndexOnlyScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecIndexOnlyScan(IndexOnlyScanState *node);
extern void ExecEndIndexOnlyScan(IndexOnlyScanState *node);
extern void ExecIndexOnlyMarkPos(IndexOnlyScanState *node);
extern void ExecIndexOnlyRestrPos(IndexOnlyScanState *node);
extern void ExecIndexOnlyReScan(IndexOnlyScanState *node, ExprContext *exprCtxt);
extern void ExecEagerFreeIndexOnlyScan(IndexOnlyScanState *node);

enum {
	GPMON_INDEXONLYSCAN_RESTOREPOS = GPMON_QEXEC_M_NODE_START,
	GPMON_INDEXONLYSCAN_RESCAN,
	GPMON_INDEXONLYSCAN_TOTAL,
};

static inline gpmon_packet_t * GpmonPktFromIndexOnlyScanState(IndexOnlyScanState *node)
{
	return &node->ss.ps.gpmon_pkt;
}

#endif
