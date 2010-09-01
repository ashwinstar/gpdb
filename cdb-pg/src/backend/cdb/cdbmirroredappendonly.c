/*-------------------------------------------------------------------------
 *
 * cdbmirroredappendonly.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/xlogmm.h"
#include "utils/palloc.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbmirroredappendonly.h"
#include "storage/fd.h"
#include "catalog/catalog.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbfilerepprimary.h"
#include "storage/smgr.h"
#include "storage/lwlock.h"
#include "utils/guc.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentstore.h"

static void MirroredAppendOnly_SetUpMirrorAccess(
	RelFileNode					*relFileNode,
			/* The tablespace, database, and relation OIDs for the open. */
	 
	int32						segmentFileNum,
	
	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,

	bool						create,
	
	bool						drop,
	
	bool						primaryOnly,
	
	bool						mirrorOnly,
	
	bool						copyToMirror,
	
	StorageManagerMirrorMode	*mirrorMode,

	bool						*mirrorDataLossOccurred)
{
	*mirrorMode = StorageManagerMirrorMode_None;
	*mirrorDataLossOccurred = false;	// Assume.

	if (gp_initdb_mirrored)
	{
		/* Kludge for initdb */
		*mirrorMode = StorageManagerMirrorMode_Both;
		return;
	}
	
	switch (mirrorDataLossTrackingState)
	{
	case MirrorDataLossTrackingState_MirrorNotConfigured:
		if (mirrorOnly)
			elog(ERROR, "No mirror configured for mirror only");
		
		*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		break;
		
	case MirrorDataLossTrackingState_MirrorCurrentlyUpInSync:
		if (primaryOnly)
			*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		else if (!mirrorOnly)
			*mirrorMode = StorageManagerMirrorMode_Both;
		else
			*mirrorMode = StorageManagerMirrorMode_MirrorOnly;
		break;
			
	case MirrorDataLossTrackingState_MirrorCurrentlyUpInResync:
		if (primaryOnly)
			*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		else if (!mirrorOnly)
			*mirrorMode = StorageManagerMirrorMode_Both;
		else
			*mirrorMode = StorageManagerMirrorMode_MirrorOnly;
		break;

	case MirrorDataLossTrackingState_MirrorDown:
		if (!mirrorOnly)
			*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		else
			*mirrorMode = StorageManagerMirrorMode_MirrorOnly;	// Mirror only operations fails from the outset.

		*mirrorDataLossOccurred = true; 	// Mirror communication is down.
		break;
		
	default:
		elog(ERROR, "Unexpected mirror data loss tracking state: %d",
			 mirrorDataLossTrackingState);
		*mirrorMode = StorageManagerMirrorMode_None; 	// A happy optimizer is the sound of one hand clapping.
	}
	
	if (Debug_persistent_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(Persistent_DebugPrintLevel(), 
			 "MirroredAppendOnly_SetUpMirrorAccess %u/%u/%u, segment file #%d, relation name '%s': create %s, drop %s, primaryOnly %s, mirrorOnly %s, copyToMirror %s, "
			 "mirror data loss tracking (state '%s', session num " INT64_FORMAT "), mirror mode %s, mirror data loss occurred %s",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 (relationName == NULL ? "<null>" : relationName),
			 (create ? "true" : "false"),
			 (drop ? "true" : "false"),
			 (primaryOnly ? "true" : "false"),
			 (mirrorOnly ? "true" : "false"),
			 (copyToMirror ? "true" : "false"),
			 MirrorDataLossTrackingState_Name(mirrorDataLossTrackingState),
			 mirrorDataLossTrackingSessionNum,
			 StorageManagerMirrorMode_Name(*mirrorMode),
			 (*mirrorDataLossOccurred ? "true" : "false"));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

static void MirroredAppendOnly_CheckMirrorOperationSession(
	RelFileNode					*relFileNode,
			/* The tablespace, database, and relation OIDs for the open. */
	 
	int32						segmentFileNum,
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	bool						*mirrorDataLossOccurred)
{
	MirrorDataLossTrackingState currentMirrorDataLossTrackingState;
	int64 currentMirrorDataLossTrackingSessionNum;

	bool trace;

	Assert(!*mirrorDataLossOccurred);

	currentMirrorDataLossTrackingState =
		FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
										&currentMirrorDataLossTrackingSessionNum);

	trace = false;
	if (mirrorDataLossTrackingSessionNum != currentMirrorDataLossTrackingSessionNum)
	{
		Assert(currentMirrorDataLossTrackingSessionNum > mirrorDataLossTrackingSessionNum);

		*mirrorDataLossOccurred = true;
		trace = true;
	}
	else if (mirrorDataLossTrackingState != currentMirrorDataLossTrackingState)
	{
		/*
		 * Same mirror data loss tracking number, but different state.
		 */

		if (mirrorDataLossTrackingState == MirrorDataLossTrackingState_MirrorDown)
		{
			/*
			 * We started with the mirror down and ended with the mirror up in either
			 * Resynchronized or Synchronized state (or not configured).
			 *
			 * So, there was mirror data loss.
			 */
			*mirrorDataLossOccurred = true;
		}
		trace = true;
	}
	
	if (trace && Debug_persistent_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;
	
		SUPPRESS_ERRCONTEXT_PUSH();
	
		elog(Persistent_DebugPrintLevel(), 
			 "MirroredAppendOnly_CheckMirrorOperationSession %u/%u/%u, segment file #%d --> mirror data loss occurred %s "
			 "(original mirror data loss tracking session num " INT64_FORMAT ", current " INT64_FORMAT ", original state '%s', current '%s')",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 (*mirrorDataLossOccurred ? "true" : "false"),
			 mirrorDataLossTrackingSessionNum,
			 currentMirrorDataLossTrackingSessionNum,
			 MirrorDataLossTrackingState_Name(mirrorDataLossTrackingState),
			 MirrorDataLossTrackingState_Name(currentMirrorDataLossTrackingState));
	
		SUPPRESS_ERRCONTEXT_POP();
	}
}

static void
MirroredAppendOnly_SetMirrorDataLossOccurred(
	MirroredAppendOnlyOpen		*open,
			/* The resulting open struct. */

	char						*caller)
{
	if (open->mirrorDataLossOccurred)
		elog(ERROR, "Mirror data loss occurred should not already be set");

	open->mirrorDataLossOccurred = true;

	if (Debug_persistent_print)
	{
		MirrorDataLossTrackingState currentMirrorDataLossTrackingState;
		int64 currentMirrorDataLossTrackingSessionNum;
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		currentMirrorDataLossTrackingState =
			FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
											&currentMirrorDataLossTrackingSessionNum);
		elog(Persistent_DebugPrintLevel(), 
			 "MirroredAppendOnly_MirrorDataLossOccurred %u/%u/%u, segment file #%d --> setting mirror data loss occurred "
			 "(original mirror data loss tracking session num " INT64_FORMAT ", current " INT64_FORMAT ", original state '%s', current '%s', caller '%s')",
			 open->relFileNode.spcNode,
			 open->relFileNode.dbNode,
			 open->relFileNode.relNode,
			 open->segmentFileNum,
			 open->mirrorDataLossTrackingSessionNum,
			 currentMirrorDataLossTrackingSessionNum,
			 MirrorDataLossTrackingState_Name(open->mirrorDataLossTrackingState),
			 MirrorDataLossTrackingState_Name(currentMirrorDataLossTrackingState),
			 caller);

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/*
 * Open a relation for mirrored write.
 */
static void MirroredAppendOnly_DoOpen(
	MirroredAppendOnlyOpen 		*open,
			/* The resulting open struct. */
							 
	RelFileNode					*relFileNode,
			/* The tablespace, database, and relation OIDs for the open. */
	 
	int32						segmentFileNum,
	
	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	int64						logicalEof,
				/* The file name path. */

	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,

	bool						create,

	bool						primaryOnlyToLetResynchronizeWork,

	bool						mirrorOnly,
	
	bool						copyToMirror,

	bool						guardOtherCallsWithMirroredLock,
	
	int 						*primaryError,
	
	bool						*mirrorDataLossOccurred)
{
	int		fileFlags = O_RDWR | PG_BINARY;
	int		fileMode = 0600;
						/*
						 * File mode is S_IRUSR 00400 user has read permission
						 *               + S_IWUSR 00200 user has write permission
						 */

	char *primaryFilespaceLocation;
	char *mirrorFilespaceLocation;

	Assert(open != NULL);
	
	*primaryError = 0;
	*mirrorDataLossOccurred = false;

	if (create)
		fileFlags |= O_CREAT;

	MemSet(open, 0, sizeof(MirroredAppendOnlyOpen));

	open->relFileNode = *relFileNode;
	
	open->segmentFileNum = segmentFileNum;

	open->mirrorDataLossTrackingState = mirrorDataLossTrackingState;
	open->mirrorDataLossTrackingSessionNum = mirrorDataLossTrackingSessionNum;

	open->create = create;
	open->primaryOnlyToLetResynchronizeWork = primaryOnlyToLetResynchronizeWork;
	open->mirrorOnly = mirrorOnly;
	open->copyToMirror = copyToMirror;
	open->guardOtherCallsWithMirroredLock = guardOtherCallsWithMirroredLock;
	
	MirroredAppendOnly_SetUpMirrorAccess(
									relFileNode,
									segmentFileNum,
									relationName,
									mirrorDataLossTrackingState,
									mirrorDataLossTrackingSessionNum,
									create,
									/* drop */ false,
									primaryOnlyToLetResynchronizeWork,
									mirrorOnly,
									copyToMirror,
									&open->mirrorMode,
									&open->mirrorDataLossOccurred);

	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
										relFileNode->spcNode,
										&primaryFilespaceLocation,
										&mirrorFilespaceLocation);
	
	if (mirrorFilespaceLocation == NULL)
		sprintf(open->mirrorFilespaceLocation, "%s", "");
	else
		sprintf(open->mirrorFilespaceLocation, "%s", mirrorFilespaceLocation);

	if (StorageManagerMirrorMode_DoPrimaryWork(open->mirrorMode))
	{
		char *dbPath; 
		char *path;

		dbPath = (char*)palloc(MAXPGPATH + 1);
		path = (char*)palloc(MAXPGPATH + 1);

		/*
		 * Do the primary work first so we don't leave files on the mirror or have an
		 * open to clean up.
		 */
		FormDatabasePath(
					dbPath,
					primaryFilespaceLocation,
					relFileNode->spcNode,
					relFileNode->dbNode);
		
		if (segmentFileNum == 0)
			sprintf(path, "%s/%u", dbPath, relFileNode->relNode);
		else
			sprintf(path, "%s/%u.%u", dbPath, relFileNode->relNode, segmentFileNum);

		errno = 0;
		
		open->primaryFile = PathNameOpenFile(path, fileFlags, fileMode);
				
		if (open->primaryFile < 0)
		{
			*primaryError = errno;
		}

		pfree(dbPath);
		pfree(path);
	}

	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		*primaryError == 0 &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_MirrorOpen(
									  FileRep_GetRelationIdentifier(
																	open->mirrorFilespaceLocation,
																	open->relFileNode, 
																	open->segmentFileNum),
									  FileRepRelationTypeAppendOnly,
									  logicalEof,
									  fileFlags,
									  fileMode,
									  TRUE /* supressError */) != 0) 
		{		
			ereport(LOG,
					(errmsg("ERROR(filerep): could not sent file open request to mirror "),
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorOpen");
		}
	}

	if (*primaryError != 0)
	{
		open->isActive = false;
	}
	else if (StorageManagerMirrorMode_DoPrimaryWork(open->mirrorMode))
	{
		open->isActive = true;
	}
	else if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
			 !open->mirrorDataLossOccurred)
	{
		open->isActive = true;
	}

	*mirrorDataLossOccurred = open->mirrorDataLossOccurred;

	if (primaryFilespaceLocation != NULL)
		pfree(primaryFilespaceLocation);
	if (mirrorFilespaceLocation != NULL)
		pfree(mirrorFilespaceLocation);

}

/*
 * Call MirroredAppendOnly_Create with the MirroredLock already held.
 */
void MirroredAppendOnly_Create(
	RelFileNode 				*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	int32						segmentFileNum,
				/* Which segment file. */
	
	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	int 						*primaryError,
	
	bool						*mirrorDataLossOccurred)
{
	MirroredAppendOnlyOpen mirroredOpen;

	bool mirrorCatchupRequired;

	*primaryError = 0;
	*mirrorDataLossOccurred = false;

	MirroredAppendOnly_DoOpen(
					&mirroredOpen, 
					relFileNode,
					segmentFileNum,
					relationName,
					/* logicalEof */ 0,
					mirrorDataLossTrackingState,
					mirrorDataLossTrackingSessionNum,
					/* create */ true,
					/* primaryOnlyToLetResynchronizeWork */ false,
					/* mirrorOnly */ false,
					/* copyToMirror */ false,
					/* guardOtherCallsWithMirroredLock */ false,
					primaryError,
					mirrorDataLossOccurred);
	if (primaryError != 0)
		return;

	MirroredAppendOnly_FlushAndClose(
							&mirroredOpen,
							primaryError,
							mirrorDataLossOccurred,
							&mirrorCatchupRequired);
}

void MirroredAppendOnly_MirrorReCreate(
	RelFileNode 				*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	int32						segmentFileNum,
				/* Which segment file. */
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	bool						*mirrorDataLossOccurred)
{
	MirroredAppendOnlyOpen mirroredOpen;

	int primaryError;

	bool mirrorCatchupRequired;

	*mirrorDataLossOccurred = false;

	MirroredAppendOnly_DoOpen(
				&mirroredOpen, 
				relFileNode,
				segmentFileNum,
				NULL,
				/* logicalEof */ 0,
				mirrorDataLossTrackingState,
				mirrorDataLossTrackingSessionNum,
				/* create */ true,
				/* primaryOnlyToLetResynchronizeWork */ false,
				/* mirrorOnly */ true,
				/* copyToMirror */ false,
				/* guardOtherCallsWithMirroredLock */ false,
				&primaryError,
				mirrorDataLossOccurred);
	Assert(primaryError == 0);	// No primary work here.
	if (mirrorDataLossOccurred)
		return;

	MirroredAppendOnly_FlushAndClose(
							&mirroredOpen,
							&primaryError,
							mirrorDataLossOccurred,
							&mirrorCatchupRequired);
	Assert(primaryError == 0);	// No primary work here.
}

/*
 * MirroredAppendOnly_OpenReadWrite will acquire and release the MirroredLock.
 *
 * Use aaa after writing data to determine if mirror loss occurred and
 * mirror catchup must be performed.
 */
void MirroredAppendOnly_OpenReadWrite(
	MirroredAppendOnlyOpen		*open,
				/* The resulting open struct. */

	RelFileNode 				*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	int32						segmentFileNum,
				/* Which segment file. */
	
	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	int64						logicalEof,
				/* The logical EOF to begin appending the new data. */

	ItemPointer					persistentTid,

	int64						persistentSerialNum,
	
	int 						*primaryError)
{
	MIRRORED_LOCK_DECLARE;

	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;

	bool primaryOnlyToLetResynchronizeWork = false;
	
	bool	mirrorDataLossOccurred;
	
	*primaryError = 0;

	MIRRORED_LOCK;

	/*
	 * Make this call while under the MirroredLock (unless we are a resync worker).
	 */
	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);
	if (mirrorDataLossTrackingState == MirrorDataLossTrackingState_MirrorCurrentlyUpInResync &&
		logicalEof > 0)
	{
		/*
		 * While in resynchronize state, we don't know for this Append-Only segment
		 * file whether to write to the mirror or not.
		 *
		 * If the Append-Only segment file already has mirror loss, then NO.
		 *
		 * However, if the Append-Only segment file is synchronized because either
		 * there was no data loss from a previous committed transaction or resynchronize
		 * has completed recovery of that segment file.
		 */
		if (PersistentFileSysObj_CheckForAppendOnlyCatchup(
												relFileNode,
												segmentFileNum,
												persistentTid,
												persistentSerialNum))
		{
			/*
			 * Don't write to mirror while resynchronize may be working on the segment file.
			 */
			primaryOnlyToLetResynchronizeWork = true;
		}
	}
	
	MirroredAppendOnly_DoOpen(
							open, 
							relFileNode,
							segmentFileNum,
							relationName,
							logicalEof,
							mirrorDataLossTrackingState,
							mirrorDataLossTrackingSessionNum,
							/* create */ false,
							primaryOnlyToLetResynchronizeWork,
							/* mirrorOnly */ false,
							/* copyToMirror */ false,
							/* guardOtherCallsWithMirroredLock */ true,
							primaryError,
							&mirrorDataLossOccurred);

	MIRRORED_UNLOCK;
}

void MirroredAppendOnly_OpenResynchonize(
	MirroredAppendOnlyOpen		*open,
				/* The resulting open struct. */

	RelFileNode 				*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	int32						segmentFileNum,
				/* Which segment file. */
		
	int64						logicalEof,
				/* The logical EOF to begin appending the new data. */
									
	int 						*primaryError,
	
	bool						*mirrorDataLossOccurred)
{
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	int64 seekResult = 0;

	*primaryError = 0;
	*mirrorDataLossOccurred = false;

	// NOTE: No acquisition of MirroredLock

	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);
	
	MirroredAppendOnly_DoOpen(
							open, 
							relFileNode,
							segmentFileNum,
							NULL,
							logicalEof,
							mirrorDataLossTrackingState,
							mirrorDataLossTrackingSessionNum,
							/* create */ false,
							/* primaryOnly */ false,
							/* mirrorOnly */ false,
							/* copyToMirror */ true,
							/* guardOtherCallsWithMirroredLock */ false,
							primaryError,
							mirrorDataLossOccurred);
	/* 
	 * Resync Worker open/seek primary for read.
	 */
	errno = 0;
	seekResult = FileSeek(open->primaryFile, 
						  logicalEof, 
						  SEEK_SET);
	if (seekResult != logicalEof) 
	{
		*primaryError = errno;
	}
}


bool MirroredAppendOnly_IsActive(
	MirroredAppendOnlyOpen *open)
					/* The open struct. */
{
	return open->isActive;
}

/*
 * Flush and close a bulk relation file.
 *
 * If the flush is unable to complete on the mirror, then this relation will be marked in the
 * commit, distributed commit, distributed prepared and commit prepared records as having
 * un-mirrored bulk initial data.
 */
void MirroredAppendOnly_FlushAndClose(
	MirroredAppendOnlyOpen 	*open,
				/* The open struct. */		

	int						*primaryError,

	bool					*mirrorDataLossOccurred,

	bool					*mirrorCatchupRequired)
{
	MIRRORED_LOCK_DECLARE;

	Assert(open != NULL);
	Assert(open->isActive);

	*primaryError = 0;
	*mirrorDataLossOccurred = false;
	*mirrorCatchupRequired = false;
	
	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_LOCK;
	}

	/*
	 * See if things have changed and we shouldn't write to mirror anymore.
	 */
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		MirroredAppendOnly_CheckMirrorOperationSession(
												&open->relFileNode,
												open->segmentFileNum,
												open->mirrorDataLossTrackingState,
												open->mirrorDataLossTrackingSessionNum,
												&open->mirrorDataLossOccurred);
	}
	
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_MirrorFlushAndClose(
										   FileRep_GetRelationIdentifier(
																		 open->mirrorFilespaceLocation,
																		 open->relFileNode, 
																		 open->segmentFileNum),
										   FileRepRelationTypeAppendOnly) != 0)
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not sent file fsync and close request to mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));		
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorFlushAndClose (send)");
		}
	}
	
	if (StorageManagerMirrorMode_DoPrimaryWork(open->mirrorMode))
	{
		if (!open->copyToMirror)
		{
			errno = 0;

			if (FileSync(open->primaryFile) != 0)
				*primaryError = errno;
		}

		FileClose(open->primaryFile);
	}
	
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_IsOperationCompleted(
											FileRep_GetRelationIdentifier(
																		  open->mirrorFilespaceLocation,
																		  open->relFileNode, 
																		  open->segmentFileNum),									
											FileRepRelationTypeAppendOnly) == FALSE)	
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not fsync file on mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));	
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorFlushAndClose (complete)");
		}
	}

	/*
	 * Finally since we've flushed and closed, see if things have changed and we had
	 * a mirror issue...
	 */
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		MirroredAppendOnly_CheckMirrorOperationSession(
												&open->relFileNode,
												open->segmentFileNum,
												open->mirrorDataLossTrackingState,
												open->mirrorDataLossTrackingSessionNum,
												&open->mirrorDataLossOccurred);
	}

	*mirrorDataLossOccurred = open->mirrorDataLossOccurred;	// Keep reporting -- it may have occurred anytime during the open session.

	/*
	 * Evaluate whether we need to catchup the mirror.
	 */
	*mirrorCatchupRequired = (open->primaryOnlyToLetResynchronizeWork || open->mirrorDataLossOccurred);

	open->isActive = false;
	open->primaryFile = 0;

	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_UNLOCK;
	}

}

/*
 * Flush and close a bulk relation file.
 *
 * If the flush is unable to complete on the mirror, then this relation will be marked in the
 * commit, distributed commit, distributed prepared and commit prepared records as having
 * un-mirrored bulk initial data.
 */
void MirroredAppendOnly_Flush(
	MirroredAppendOnlyOpen 	*open,
				/* The open struct. */				

	int						*primaryError,

	bool					*mirrorDataLossOccurred)
{
	MIRRORED_LOCK_DECLARE;

	Assert(open != NULL);
	Assert(open->isActive);
	
	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_LOCK;
	}

	/*
	 * See if things have changed and we shouldn't write to mirror anymore.
	 */
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		MirroredAppendOnly_CheckMirrorOperationSession(
												&open->relFileNode,
												open->segmentFileNum,
												open->mirrorDataLossTrackingState,
												open->mirrorDataLossTrackingSessionNum,
												&open->mirrorDataLossOccurred);
	}
	
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_MirrorFlush(
								   FileRep_GetRelationIdentifier(
																 open->mirrorFilespaceLocation,
																 open->relFileNode, 
																 open->segmentFileNum),
									FileRepRelationTypeAppendOnly) != 0) 
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not sent file fsync and close request to mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));				
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorFlush (send)");
		}
	}
	
	if (StorageManagerMirrorMode_DoPrimaryWork(open->mirrorMode) &&
		!open->copyToMirror)
	{
		errno = 0;
		
		if (FileSync(open->primaryFile) != 0)
		{
			*primaryError = errno;
		}
	}
	
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_IsOperationCompleted(
												FileRep_GetRelationIdentifier(
																			  open->mirrorFilespaceLocation,
																			  open->relFileNode, 
																			  open->segmentFileNum),									
												FileRepRelationTypeAppendOnly) == FALSE)
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not fsync file on mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));		
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorFlush (complete)");
		}
	}
	
	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_UNLOCK;
	}

	*mirrorDataLossOccurred = open->mirrorDataLossOccurred;	// Keep reporting -- it may have occurred anytime during the open session.

}

/*
 * Close a bulk relation file.
 *
 */
void MirroredAppendOnly_Close(
	MirroredAppendOnlyOpen 	*open,
				/* The open struct. */				

	bool					*mirrorDataLossOccurred)
{
	MIRRORED_LOCK_DECLARE;

	Assert(open != NULL);
	Assert(open->isActive);
	
	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_LOCK;
	}

	/*
	 * See if things have changed and we shouldn't write to mirror anymore.
	 */
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		MirroredAppendOnly_CheckMirrorOperationSession(
												&open->relFileNode,
												open->segmentFileNum,
												open->mirrorDataLossTrackingState,
												open->mirrorDataLossTrackingSessionNum,
												&open->mirrorDataLossOccurred);
	}
	
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_MirrorClose(
									   FileRep_GetRelationIdentifier(
																	 open->mirrorFilespaceLocation,
																	 open->relFileNode, 
																	 open->segmentFileNum),
									   FileRepRelationTypeAppendOnly) != 0)
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not sent file close request to mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));						
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorClose");
		}

	}
	
	if (StorageManagerMirrorMode_DoPrimaryWork(open->mirrorMode))
	{
		// No primary error to report.
		errno = 0;
		
		FileClose(open->primaryFile);
	}
	
	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_UNLOCK;
	}

	*mirrorDataLossOccurred = open->mirrorDataLossOccurred;	// Keep reporting -- it may have occurred anytime during the open session.

	open->isActive = false;
	open->primaryFile = 0;
}

static void MirroredAppendOnly_DoCopyDataToMirror(
	MirroredAppendOnlyOpen 		*mirroredOpen,

	int64						mirrorLossEof,

	int64						mirrorNewEof,
	
	bool						*mirrorDataLossOccurred)
{
	char	*buffer;
	int64	endOffset = mirrorNewEof;
	int64	readOffset = mirrorLossEof;
	int32	bufferLen;
	int 	retval;

	int		primaryError;

	bufferLen = (Size) Min(2*BLCKSZ, endOffset - readOffset);
	buffer = (char*) palloc(bufferLen);

	while (readOffset < endOffset)
	{
		retval = MirroredAppendOnly_Read(
								mirroredOpen,
								buffer,
								bufferLen);
		
		if (retval != bufferLen) 
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from position: " INT64_FORMAT " in file %d/%d/%d.%d : %m",
							readOffset, 
							mirroredOpen->relFileNode.dbNode,
							mirroredOpen->relFileNode.spcNode,
							mirroredOpen->relFileNode.relNode,
							mirroredOpen->segmentFileNum)));
			
			break;
		}						
		
		MirroredAppendOnly_Append(
							  mirroredOpen,
							  buffer,
							  bufferLen,
							  &primaryError,
							  mirrorDataLossOccurred);
		Assert(primaryError == 0);	// No primary writes.

		if (*mirrorDataLossOccurred)
			break;
		
		readOffset += bufferLen;
		
		bufferLen = (Size) Min(2*BLCKSZ, endOffset - readOffset);						
	}

	pfree(buffer);
}

void MirroredAppendOnly_AddMirrorResyncEofs(
	RelFileNode						*relFileNode,

	int32							segmentFileNum,

	char							*relationName,

	ItemPointer						persistentTid,

	int64							persistentSerialNum,

	int64							*mirrorLossEof,

	int64							mirrorNewEof)
{
	MIRRORED_LOCK_DECLARE;

	MirroredAppendOnlyOpen 		mirroredOpen;

	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	int64 seekResult = 0;

	int 	primaryError;
	bool	mirrorDataLossOccurred;
	
	bool mirrorCatchupRequired;

	while (true)
	{
		/*
		 * Do our FileRep mirror data loss tracking state evaluation under the MirroredLock.
		 */
		MIRRORED_LOCK;

		if (*mirrorLossEof == mirrorNewEof)
		{
			/*
			 * No data loss.  Report new EOFs now.
			 */
			if (Debug_persistent_print)
			{
				SUPPRESS_ERRCONTEXT_DECLARE;

				SUPPRESS_ERRCONTEXT_POP();

				elog(Persistent_DebugPrintLevel(), 
					 "MirroredAppendOnly_AddMirrorResyncEofs %u/%u/%u, segment file #%d, relation name '%s': no mirror data loss occurred --> report new EOF (EOF " INT64_FORMAT ")",
					 relFileNode->spcNode,
					 relFileNode->dbNode,
					 relFileNode->relNode,
					 segmentFileNum,
					 (relationName == NULL ? "<null>" : relationName),
					 mirrorNewEof);

				SUPPRESS_ERRCONTEXT_PUSH();
			}

			smgrappendonlymirrorresynceofs(
									relFileNode,
									segmentFileNum,
									relationName,
									persistentTid,
									persistentSerialNum,
									*mirrorLossEof,
									mirrorNewEof);
			MIRRORED_UNLOCK;
			break;
		}

		mirrorDataLossTrackingState = 
					FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
													&mirrorDataLossTrackingSessionNum);

		switch (mirrorDataLossTrackingState)
		{
		case MirrorDataLossTrackingState_MirrorNotConfigured:
			/*
			 * Apparently, we've dropped the mirror.
			 */
			if (Debug_persistent_print)
			{
				SUPPRESS_ERRCONTEXT_DECLARE;
			
				SUPPRESS_ERRCONTEXT_POP();
			
				elog(Persistent_DebugPrintLevel(), 
					 "MirroredAppendOnly_AddMirrorResyncEofs %u/%u/%u, segment file #%d, relation name '%s': mirror no longer configured (mirror data loss tracking serial num " INT64_FORMAT ")-- reporting no data loss occurred --> report new EOF (EOF " INT64_FORMAT ")",
					 relFileNode->spcNode,
					 relFileNode->dbNode,
					 relFileNode->relNode,
					 segmentFileNum,
					 (relationName == NULL ? "<null>" : relationName),
					 mirrorDataLossTrackingSessionNum,
					 mirrorNewEof);
			
				SUPPRESS_ERRCONTEXT_PUSH();
			}
			
			smgrappendonlymirrorresynceofs(
									relFileNode,
									segmentFileNum,
									relationName,
									persistentTid,
									persistentSerialNum,
									mirrorNewEof,	// No data loss for no mirror.
									mirrorNewEof);
			MIRRORED_UNLOCK;
			return;

		case MirrorDataLossTrackingState_MirrorDown:
			/*
			 * We are not in synchronized state -- report data loss.
			 */
			if (Debug_persistent_print)
			{
				SUPPRESS_ERRCONTEXT_DECLARE;

				SUPPRESS_ERRCONTEXT_PUSH();

				elog(Persistent_DebugPrintLevel(), 
					 "MirroredAppendOnly_AddMirrorResyncEofs %u/%u/%u, segment file #%d, relation name '%s': mirror down (mirror data loss tracking serial num " INT64_FORMAT ") --> "
					 "report loss (mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT ")",
					 relFileNode->spcNode,
					 relFileNode->dbNode,
					 relFileNode->relNode,
					 segmentFileNum,
					 (relationName == NULL ? "<null>" : relationName),
					 mirrorDataLossTrackingSessionNum,
					 *mirrorLossEof,
					 mirrorNewEof);

				SUPPRESS_ERRCONTEXT_POP();
			}

			smgrappendonlymirrorresynceofs(
									relFileNode,
									segmentFileNum,
									relationName,
									persistentTid,
									persistentSerialNum,
									*mirrorLossEof,
									mirrorNewEof);
			MIRRORED_UNLOCK;
			return;

		case MirrorDataLossTrackingState_MirrorCurrentlyUpInSync:
		case MirrorDataLossTrackingState_MirrorCurrentlyUpInResync:
			/*
			 * Fall through and catch-up the mirror.
			 */
			break;

		default:
			elog(ERROR, "Unexpected mirror data loss tracking state: %d",
				 mirrorDataLossTrackingState);
		}

		/*
		 * Resynchronize didn't know about us writing more data, so it is our responsibility
		 * to catch-up the mirror.
		 */
		if (Debug_persistent_print)
		{
			SUPPRESS_ERRCONTEXT_DECLARE;

			SUPPRESS_ERRCONTEXT_PUSH();

			elog(Persistent_DebugPrintLevel(), 
				 "MirroredAppendOnly_AddMirrorResyncEofs %u/%u/%u, segment file #%d, relation name '%s': we are in '%s' state (mirror data loss tracking serial num " INT64_FORMAT "), so it is our responsibility to catch-up mirror (mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT ")",
				 relFileNode->spcNode,
				 relFileNode->dbNode,
				 relFileNode->relNode,
				 segmentFileNum,
				 (relationName == NULL ? "<null>" : relationName),
				 MirrorDataLossTrackingState_Name(mirrorDataLossTrackingState),
				 mirrorDataLossTrackingSessionNum,
				 *mirrorLossEof,
				 mirrorNewEof);

			SUPPRESS_ERRCONTEXT_POP();
		}

		MirroredAppendOnly_DoOpen(
						&mirroredOpen, 
						relFileNode,
						segmentFileNum,
						relationName,
						/* logicalEof */ *mirrorLossEof,
						mirrorDataLossTrackingState,
						mirrorDataLossTrackingSessionNum,
						/* create */ false,
						/* primaryOnly */ false,
						/* mirrorOnly */ false,
						/* copyToMirror */ true,
						/* guardOtherCallsWithMirroredLock */ true,
						&primaryError,
						&mirrorDataLossOccurred);

		errno = 0;
		seekResult = FileSeek(mirroredOpen.primaryFile, 
							  *mirrorLossEof, 
							  SEEK_SET);
		if (seekResult != *mirrorLossEof) 
		{
			primaryError = errno;
		}		
		
		MIRRORED_UNLOCK;

		if (primaryError != 0)
		{
			ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open relation file '%s', relation name '%s': %s",
						relpath(*relFileNode),
						relationName,
						strerror(primaryError))));
		}

		if (!mirrorDataLossOccurred)
			MirroredAppendOnly_DoCopyDataToMirror(
							&mirroredOpen,
							*mirrorLossEof,
							mirrorNewEof,
							&mirrorDataLossOccurred);

		MirroredAppendOnly_FlushAndClose(
						&mirroredOpen,
						&primaryError,
						&mirrorDataLossOccurred,
						&mirrorCatchupRequired);
		Assert(primaryError == 0);	// No primary work.

		/*
		 * We we successful in copying the data to the mirror?
		 */
		if (mirrorCatchupRequired)
		{
			if (Debug_persistent_print)
			{
				SUPPRESS_ERRCONTEXT_DECLARE;

				SUPPRESS_ERRCONTEXT_PUSH();

				elog(Persistent_DebugPrintLevel(), 
					 "MirroredAppendOnly_AddMirrorResyncEofs %u/%u/%u, segment file #%d, relation name '%s': mirror catch-up mirror required again... (mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT ")",
					 relFileNode->spcNode,
					 relFileNode->dbNode,
					 relFileNode->relNode,
					 segmentFileNum,
					 (relationName == NULL ? "<null>" : relationName),
					 *mirrorLossEof,
					 mirrorNewEof);

				SUPPRESS_ERRCONTEXT_POP();
			}
		}
		else
		{
			if (Debug_persistent_print)
			{
				SUPPRESS_ERRCONTEXT_DECLARE;

				SUPPRESS_ERRCONTEXT_PUSH();

				elog(Persistent_DebugPrintLevel(), 
					 "MirroredAppendOnly_AddMirrorResyncEofs %u/%u/%u, segment file #%d, relation name '%s': no mirror data loss occurred --> mirror catch-up copied and flushed (mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT ")",
					 relFileNode->spcNode,
					 relFileNode->dbNode,
					 relFileNode->relNode,
					 segmentFileNum,
					 (relationName == NULL ? "<null>" : relationName),
					 *mirrorLossEof,
					 mirrorNewEof);

				SUPPRESS_ERRCONTEXT_POP();
			}
			
			*mirrorLossEof = mirrorNewEof;
		}
	}

}

void MirroredAppendOnly_CopyToMirror(
	RelFileNode						*relFileNode,

	int32							segmentFileNum,

	char							*relationName,

	ItemPointer						persistentTid,

	int64							persistentSerialNum,

	int64							*mirrorLossEof,

	int64							mirrorNewEof,
	
	bool							*mirrorDataLossOccurred)
{
	MIRRORED_LOCK_DECLARE;

	MirroredAppendOnlyOpen 		mirroredOpen;

	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	int64 seekResult = 0;

	int 	primaryError;

	bool mirrorCatchupRequired;

	MIRRORED_LOCK;
	
	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);
	
	MirroredAppendOnly_DoOpen(
					&mirroredOpen, 
					relFileNode,
					segmentFileNum,
					relationName,
					/* logicalEof */ *mirrorLossEof,
					mirrorDataLossTrackingState,
					mirrorDataLossTrackingSessionNum,
					/* create */ false,
					/* primaryOnly */ false,
					/* mirrorOnly */ false,
					/* copyToMirror */ true,
					/* guardOtherCallsWithMirroredLock */ true,
					&primaryError,
					mirrorDataLossOccurred);
	
	errno = 0;
	seekResult = FileSeek(mirroredOpen.primaryFile, 
						  *mirrorLossEof, 
						  SEEK_SET);
	if (seekResult != *mirrorLossEof) 
	{
		primaryError = errno;
	}			
	
	MIRRORED_UNLOCK;
	
	if (primaryError != 0)
	{
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not open relation file '%s', relation name '%s': %s",
					relpath(*relFileNode),
					relationName,
					strerror(primaryError))));
	}
	
	if (!mirrorDataLossOccurred)
		MirroredAppendOnly_DoCopyDataToMirror(
						&mirroredOpen,
						*mirrorLossEof,
						mirrorNewEof,
						mirrorDataLossOccurred);
	
	if (!mirrorDataLossOccurred)
	{
		MirroredAppendOnly_FlushAndClose(
						&mirroredOpen,
						&primaryError,
						mirrorDataLossOccurred,
						&mirrorCatchupRequired);
		Assert(primaryError == 0);	// No primary work.
	}
}

static void MirroredAppendOnly_DoDrop(
	RelFileNode					*relFileNode,
	 
	int32						segmentFileNum,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	bool  						primaryOnly,

	bool						mirrorOnly,
	
	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{
	StorageManagerMirrorMode	mirrorMode;

	char *primaryFilespaceLocation;
	char *mirrorFilespaceLocation;

#ifdef MASTER_MIRROR_SYNC
	mmxlog_log_remove_relfilenode(relFileNode->spcNode, relFileNode->dbNode, relFileNode->relNode,
								  segmentFileNum);
#endif


	*primaryError = 0;
	*mirrorDataLossOccurred = false;

	MirroredAppendOnly_SetUpMirrorAccess(
									relFileNode,
									segmentFileNum,
									relationName,
									mirrorDataLossTrackingState,
									mirrorDataLossTrackingSessionNum,
									/* create */ false,
									/* drop */ true,
									primaryOnly,
									mirrorOnly,
									/* copyToMirror */ false,
									&mirrorMode,
									mirrorDataLossOccurred);

	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
													   relFileNode->spcNode,
													   &primaryFilespaceLocation,
													   &mirrorFilespaceLocation);	
	
	if (StorageManagerMirrorMode_SendToMirror(mirrorMode) &&
		!*mirrorDataLossOccurred)
	{
		if (FileRepPrimary_MirrorDrop(
									  FileRep_GetRelationIdentifier(
																	mirrorFilespaceLocation,
																	*relFileNode, 
																	segmentFileNum),				  
									  FileRepRelationTypeAppendOnly) != 0) 
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not sent file drop request to mirror "), 
					 FileRep_ReportRelationPath(
												mirrorFilespaceLocation,
												*relFileNode,
												segmentFileNum)));
		}
		
		*mirrorDataLossOccurred = FileRepPrimary_IsMirrorDataLossOccurred();
	}
	
	if (StorageManagerMirrorMode_DoPrimaryWork(mirrorMode))
	{
		char *dbPath; 
		char *path;

		dbPath = (char*)palloc(MAXPGPATH + 1);
		path = (char*)palloc(MAXPGPATH + 1);

		FormDatabasePath(
						 dbPath,
						 primaryFilespaceLocation,
						 relFileNode->spcNode,
						 relFileNode->dbNode);
		
		if (segmentFileNum == 0)
			sprintf(path, "%s/%u", dbPath, relFileNode->relNode);
		else
			sprintf(path, "%s/%u.%u", dbPath, relFileNode->relNode, segmentFileNum);
		
		errno = 0;
		
		if (unlink(path) < 0) 
		{
			*primaryError = errno;
		}

		pfree(dbPath);
		pfree(path);
	}
	
	if (StorageManagerMirrorMode_SendToMirror(mirrorMode) &&
		!*mirrorDataLossOccurred)
	{
		if (FileRepPrimary_IsOperationCompleted(
												FileRep_GetRelationIdentifier(
																			  mirrorFilespaceLocation,
																			  *relFileNode, 
																			  segmentFileNum),									
												FileRepRelationTypeAppendOnly) == FALSE)	
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not drop file on mirror "), 
					 FileRep_ReportRelationPath(
												mirrorFilespaceLocation,
												*relFileNode,
												segmentFileNum)));	
		}
		
		*mirrorDataLossOccurred = FileRepPrimary_IsMirrorDataLossOccurred();

	}

	if (primaryFilespaceLocation != NULL)
		pfree(primaryFilespaceLocation);
	
	if (mirrorFilespaceLocation != NULL)
		pfree(mirrorFilespaceLocation);	
}

void MirroredAppendOnly_Drop(
	RelFileNode					*relFileNode,
	 
	int32						segmentFileNum,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	bool  						primaryOnly,

	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	
	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);
	
	MirroredAppendOnly_DoDrop(
						relFileNode,
						segmentFileNum,
						relationName,
						mirrorDataLossTrackingState,
						mirrorDataLossTrackingSessionNum,
						primaryOnly,
						/* mirrorOnly */ false,
						primaryError,
						mirrorDataLossOccurred);
}


void MirroredAppendOnly_MirrorReDrop(
	RelFileNode					*relFileNode,
	 
	int32						segmentFileNum,
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	bool						*mirrorDataLossOccurred)
{
	int primaryError;
	
	MirroredAppendOnly_DoDrop(
						relFileNode,
						segmentFileNum,
						/* relationName */ NULL,
						mirrorDataLossTrackingState,
						mirrorDataLossTrackingSessionNum,
						/* primaryOnly */ false,
						/* mirrorOnly */ true,
						&primaryError,
						mirrorDataLossOccurred);
	if (primaryError != 0)
	{
		// Only doing mirror work here?
		elog(LOG, "%u/%u/%u, segment file #%d: Not expecting a primary error %d ('%s') here!",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 primaryError,
		     strerror(primaryError));
	}
}

// -----------------------------------------------------------------------------
// Append 
// -----------------------------------------------------------------------------
				
/*
 * Write bulk mirrored.
 */
void MirroredAppendOnly_Append(
	MirroredAppendOnlyOpen *open,
				/* The open struct. */

	void					*buffer,
				/* Pointer to the buffer. */

	int32					bufferLen,
				/* Byte length of buffer. */

	int 					*primaryError,

	bool					*mirrorDataLossOccurred)
{
	MIRRORED_LOCK_DECLARE;

	Assert(open != NULL);
	Assert(open->isActive);
	
	*primaryError = 0;
	*mirrorDataLossOccurred = false;

	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_LOCK;
	}

	/*
	 * See if things have changed and we shouldn't write to mirror anymore.
	 */
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		MirroredAppendOnly_CheckMirrorOperationSession(
												&open->relFileNode,
												open->segmentFileNum,
												open->mirrorDataLossTrackingState,
												open->mirrorDataLossTrackingSessionNum,
												&open->mirrorDataLossOccurred);
	}
	
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_MirrorWrite(
									   FileRep_GetRelationIdentifier(
																	 open->mirrorFilespaceLocation,
																	 open->relFileNode, 
																	 open->segmentFileNum),
									   FileRepRelationTypeAppendOnly,
									   FILEREP_OFFSET_UNDEFINED,
									   buffer, 
									   bufferLen, 
									   FileRep_GetXLogRecPtrUndefined())	!= 0)
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not sent write request to mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorWrite");
		}
	}
	
	if (StorageManagerMirrorMode_DoPrimaryWork(open->mirrorMode) &&
		!open->copyToMirror)
	{
		errno = 0;	

		if ((int) FileWrite(open->primaryFile, buffer, bufferLen) != bufferLen)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			*primaryError = errno;
		}
	}
	
	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_UNLOCK;
	}

	*mirrorDataLossOccurred = open->mirrorDataLossOccurred;	// Keep reporting -- it may have occurred anytime during the open session.

}

// -----------------------------------------------------------------------------
// Truncate
// ----------------------------------------------------------------------------
void MirroredAppendOnly_Truncate(
	MirroredAppendOnlyOpen *open,
				/* The open struct. */
	
	int64		position,
				/* The position to cutoff the data. */

	int 		*primaryError,
	
	bool		*mirrorDataLossOccurred)
{
	MIRRORED_LOCK_DECLARE;

	*primaryError = 0;
	*mirrorDataLossOccurred = false;

	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_LOCK;
	}

	/*
	 * See if things have changed and we shouldn't write to mirror anymore.
	 */
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		MirroredAppendOnly_CheckMirrorOperationSession(
												&open->relFileNode,
												open->segmentFileNum,
												open->mirrorDataLossTrackingState,
												open->mirrorDataLossTrackingSessionNum,
												&open->mirrorDataLossOccurred);
	}
	
	if (StorageManagerMirrorMode_SendToMirror(open->mirrorMode) &&
		!open->mirrorDataLossOccurred)
	{
		if (FileRepPrimary_MirrorTruncate(
									  FileRep_GetRelationIdentifier(
																	open->mirrorFilespaceLocation,
																	open->relFileNode, 
																	open->segmentFileNum),
									  FileRepRelationTypeAppendOnly,
									  position) != 0)
		{
			ereport(LOG,
					(errmsg("ERROR(filerep): could not sent file truncate request to mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));	
		}
		
		if (FileRepPrimary_IsMirrorDataLossOccurred())
		{
			MirroredAppendOnly_SetMirrorDataLossOccurred(open, "MirrorTruncate");
		}

	}
				
	if (StorageManagerMirrorMode_DoPrimaryWork(open->mirrorMode) &&
		!open->copyToMirror)
	{
		errno = 0;
		
		if (FileTruncate(open->primaryFile, position) < 0)
			*primaryError = errno;
	}

	if (open->guardOtherCallsWithMirroredLock)
	{
		MIRRORED_UNLOCK;
	}

	*mirrorDataLossOccurred = open->mirrorDataLossOccurred; // Keep reporting -- it may have occurred anytime during the open session.

}

/*
 * Read an append only file in sequential way.
 * The routine is used for Resync.
 */
int MirroredAppendOnly_Read(
	MirroredAppendOnlyOpen *open,
		/* The open struct. */
	
	void		*buffer,
		/* Pointer to the buffer. */
	
	int32		bufferLen)
		/* Byte length of buffer. */

{	
	Assert(open != NULL);
	Assert(buffer != NULL);
	Assert(open->isActive);
	
	errno = 0;
	return FileRead(open->primaryFile, buffer, bufferLen);
}
