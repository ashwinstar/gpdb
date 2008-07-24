/*-------------------------------------------------------------------------
 *
 * nodeBitmapHeapscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeBitmapHeapscan.h,v 1.3 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEBITMAPHEAPSCAN_H
#define NODEBITMAPHEAPSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsBitmapHeapScan(BitmapHeapScan *node);
extern BitmapHeapScanState *ExecInitBitmapHeapScan(BitmapHeapScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecBitmapHeapScan(BitmapHeapScanState *node);
extern void ExecEndBitmapHeapScan(BitmapHeapScanState *node);
extern void ExecBitmapHeapReScan(BitmapHeapScanState *node, ExprContext *exprCtxt);

enum
{
	GPMON_BITMAPHEAPSCAN_OUTTUPLE = 0,
	GPMON_BITMAPHEAPSCAN_PAGE, 
	GPMON_BITMAPHEAPSCAN_TOTAL
};

static inline gpmon_packet_t * GpmonPktFromBitmapHeapScanState(BitmapHeapScanState *node)
{
	return &node->ss.ps.gpmon_pkt;
}

#endif   /* NODEBITMAPHEAPSCAN_H */
