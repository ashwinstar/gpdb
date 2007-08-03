/*-------------------------------------------------------------------------
 *
 * nodeShareInputScan.h
 *
 * Portions Copyright (c) 2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESHAREINPUTSCAN_H
#define NODESHAREINPUTSCAN_H

#include "nodes/execnodes.h"
extern int ExecCountSlotsShareInputScan(ShareInputScan* node);
extern ShareInputScanState *ExecInitShareInputScan(ShareInputScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecShareInputScan(ShareInputScanState *node);
extern void ExecEndShareInputScan(ShareInputScanState *node);
extern void ExecShareInputScanMarkPos(ShareInputScanState *node);
extern void ExecShareInputScanRestrPos(ShareInputScanState *node);
extern void ExecShareInputScanReScan(ShareInputScanState *node, ExprContext *exprCtxt);

extern void ExecSliceDependencyShareInputScan(ShareInputScanState *node);

#endif   /* NODESHAREINPUTSCAN_H */
