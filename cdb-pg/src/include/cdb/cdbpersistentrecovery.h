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
PersistentRecovery_ShouldHandlePass1XLogRec(
	XLogRecPtr		*beginLoc,

	XLogRecPtr 		*lsn,
	
	XLogRecord 		*record);

extern void
PersistentRecovery_HandlePass2XLogRec(
	XLogRecPtr		*beginLoc,

	XLogRecPtr 		*lsn,
	
	XLogRecord 		*record);

extern bool
PersistentRecovery_ShouldHandlePass3XLogRec(
	XLogRecPtr		*beginLoc,

	XLogRecPtr 		*lsn,
	
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

extern bool
PersistentRecovery_RedoRelationExists(
	RelFileNode 	*relFileNode,
	
	ItemPointer 	persistentTid,
	
	int64			*persistentSerialNum);

#endif   /* PERSISTENTRECOVERY_H */
