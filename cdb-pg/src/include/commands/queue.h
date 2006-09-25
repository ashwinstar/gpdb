/*-------------------------------------------------------------------------
 *
 * queue.h
 *	  Commands for manipulating resource queues.
 *
 * Copyright (c) 2006, Greenplum, Inc.
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUEUE_H
#define QUEUE_H

#include "nodes/parsenodes.h"


extern void CreateQueue(CreateQueueStmt *stmt);
extern void AlterQueue(AlterQueueStmt *stmt);
extern void DropQueue(DropQueueStmt *stmt);

#endif   /* QUEUE_H */
