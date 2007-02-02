/*-------------------------------------------------------------------------
*
* nodeExternalscan.h
*
*
*
* Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
*
* $PostgreSQL: pgsql/src/include/executor/nodeExternalscan.h,v 1.24 2006/03/05 15:58:56 momjian Exp $
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
