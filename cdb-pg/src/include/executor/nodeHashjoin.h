/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h
 *	  prototypes for nodeHashjoin.c
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeHashjoin.h,v 1.33 2006/06/27 21:31:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASHJOIN_H
#define NODEHASHJOIN_H

#include "nodes/execnodes.h"
#include "storage/buffile.h"

struct HashJoinBatchSide;               /* #include "executor/hashjoin.h" */
struct Instrumentation;                 /* #include "executor/instrument.h" */

extern int	ExecCountSlotsHashJoin(HashJoin *node);
extern HashJoinState *ExecInitHashJoin(HashJoin *node, EState *estate, int eflags);
extern TupleTableSlot *ExecHashJoin(HashJoinState *node);
extern void ExecEndHashJoin(HashJoinState *node);
extern void ExecReScanHashJoin(HashJoinState *node, ExprContext *exprCtxt);

extern void ExecHashJoinSaveTuple(MinimalTuple tuple, uint32 hashvalue,
					              struct HashJoinBatchSide *batchside);

extern executorTupleQueue *makeTQ(int size, struct Instrumentation *instr);
extern void enqueueTQ(executorTupleQueue *tq, TupleTableSlot *slot);
extern TupleTableSlot *dequeueTQ(executorTupleQueue *tq);
extern void releaseTQ(executorTupleQueue *tq);

#endif   /* NODEHASHJOIN_H */
