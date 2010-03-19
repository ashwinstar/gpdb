/*-------------------------------------------------------------------------
 *
 * persistentrecovery.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef PERSISTENTRECOVERY_H
#define PERSISTENTRECOVERY_H

#include "access/xlogdefs.h"
#include "access/xlog.h"

extern bool
PersistentRecovery_HandleXLogRec(
	XLogRecPtr		beginLoc,

	XLogRecPtr 		lsn,
	
	XLogRecord 		*record);

extern void
PersistentRecovery_CompletePass(void);

extern void
PersistentRecovery_Scan(void);

extern void
PersistentRecovery_CrashAbort(void);

extern void
PersistentRecovery_Update(void);

extern void
PersistentRecovery_Drop(void);

extern void
PersistentRecovery_SerializeRedoRelationFile(
	int redoRelationFile);

extern void
PersistentRecovery_DeserializeRedoRelationFile(
	int redoRelationFile);

#endif   /* PERSISTENTRECOVERY_H */
