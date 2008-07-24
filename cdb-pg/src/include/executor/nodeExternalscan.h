/*-------------------------------------------------------------------------
*
* nodeExternalscan.h
*
* Copyright (c) 2007-2008, Greenplum inc
*
*-------------------------------------------------------------------------
*/
#ifndef NODEEXTERNALSCAN_H
#define NODEEXTERNALSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsExternalScan(ExternalScan *node);
extern ExternalScanState *ExecInitExternalScan(ExternalScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecExternalScan(ExternalScanState *node);
extern void ExecEndExternalScan(ExternalScanState *node);
extern void ExecExternalReScan(ExternalScanState *node, ExprContext *exprCtxt);

enum {
	GPMON_EXTSCAN_OUTTUPLE = 0,
	GPMON_EXTSCAN_TOTAL
};

static inline gpmon_packet_t *GpmonPktFromExtScanState(ExternalScanState *node)
{
	return &node->ss.ps.gpmon_pkt;
}


#endif   /* NODEEXTERNALSCAN_H */
