/*-------------------------------------------------------------------------
*
* nodeExternalscan.h
*
* Copyright (c) 2007, greenplum inc
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

#endif   /* NODEEXTERNALSCAN_H */
