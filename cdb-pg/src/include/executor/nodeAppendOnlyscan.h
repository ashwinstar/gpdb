/*-------------------------------------------------------------------------
 *
 * nodeAppendOnlyscan.h
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2008, Greenplum Inc.
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAPPENDONLYSCAN_H
#define NODEAPPENDONLYSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsAppendOnlyScan(AppendOnlyScan *node);
extern AppendOnlyScanState *ExecInitAppendOnlyScan(AppendOnlyScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecAppendOnlyScan(AppendOnlyScanState *node);
extern void ExecEndAppendOnlyScan(AppendOnlyScanState *node);
extern void ExecAppendOnlyReScan(AppendOnlyScanState *node, ExprContext *exprCtxt);

#endif   /* NODEAPPENDONLYSCAN_H */
