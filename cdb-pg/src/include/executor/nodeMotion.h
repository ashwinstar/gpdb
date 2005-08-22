/*-------------------------------------------------------------------------
 *
 * nodeMotion.h
 *
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMOTION_H
#define NODEMOTION_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsMotion(Motion *node);
extern MotionState *ExecInitMotion(Motion *node, EState *estate);
extern TupleTableSlot *ExecMotion(MotionState *node);
extern void ExecEndMotion(MotionState *node);

extern void ExecStopMotion(MotionState *node);

#endif   /* NODEMOTION_H */
