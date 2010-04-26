/*-------------------------------------------------------------------------
 *
 * cdbmirroredfilesysobj.c
 *	  Create and drop mirrored files and directories.
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#ifndef WIN32
#include <sys/fcntl.h>
#else
#include <io.h>
#endif 
#include <sys/file.h>
#include <unistd.h>

#include <signal.h>

#include "cdb/cdbmirroredfilesysobj.h"
#include "storage/smgr.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbpersistentfilespace.h"
#include "storage/lwlock.h"
#include "miscadmin.h"
#include "postmaster/primary_mirror_mode.h"
#include "cdb/cdbfilerep.h"
#include "catalog/catalog.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "utils/guc.h"
#include "cdb/cdbmirroredappendonly.h"

static void MirroredFileSysObj_BeginMirroredCreate(
	PersistentFileSysObjName		*fsObjName,

	MirrorDataLossTrackingState 	mirrorDataLossTrackingState,

	MirroredObjectExistenceState 	*mirrorExistenceState,
	
	StorageManagerMirrorMode 		*mirrorMode)
{
	/* Kludge for initdb */
	if (gp_initdb_mirrored)
	{
		*mirrorExistenceState = MirroredObjectExistenceState_MirrorCreatePending;
		*mirrorMode = StorageManagerMirrorMode_Both;
		
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "MirroredFileSysObj_BeginMirroredCreate (%s): -- initdb mirrored",
				 PersistentFileSysObjName_TypeAndObjectName(fsObjName));

		return;
	}

	switch (mirrorDataLossTrackingState)
	{
	case MirrorDataLossTrackingState_MirrorNotConfigured:
		*mirrorExistenceState = MirroredObjectExistenceState_NotMirrored;
		*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		break;
		
	case MirrorDataLossTrackingState_MirrorCurrentlyUpInSync:
	case MirrorDataLossTrackingState_MirrorCurrentlyUpInResync:		// Since we are creating a new object.
		*mirrorExistenceState = MirroredObjectExistenceState_MirrorCreatePending;
		*mirrorMode = StorageManagerMirrorMode_Both;
		break;
			
	case MirrorDataLossTrackingState_MirrorDown:
		*mirrorExistenceState = MirroredObjectExistenceState_MirrorReCreateNeeded;
		*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		break;
		
	default:
		elog(ERROR, "Unexpected mirror data loss tracking state: %d",
			 mirrorDataLossTrackingState);
		*mirrorMode = StorageManagerMirrorMode_None;	// A happy optimizer is the sound of one hand clapping.
	}

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_BeginMirroredCreate (%s): mirror existence state '%s', mirror mode %s",
			 PersistentFileSysObjName_TypeAndObjectName(fsObjName),
			 MirroredObjectExistenceState_Name(*mirrorExistenceState),
			 StorageManagerMirrorMode_Name(*mirrorMode));
}

static void MirroredFileSysObj_FinishMirroredCreate(
	PersistentFileSysObjName		*fsObjName,

	ItemPointer 					persistentTid,
	
	int64							persistentSerialNum,

	MirroredObjectExistenceState 	mirrorExistenceState,
	
	bool 							mirrorDataLossOccurred)
{
	MirroredObjectExistenceState 	newMirrorExistenceState;
	MirroredRelDataSynchronizationState newRelDataSynchronizationState;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_FinishMirroredCreate (%s): mirror existence state '%s', mirror data loss occurred %s, "
		     "serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_TypeAndObjectName(fsObjName),
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 (mirrorDataLossOccurred ? "true" : "false"),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));

	if (mirrorExistenceState == MirroredObjectExistenceState_NotMirrored ||
		mirrorExistenceState == MirroredObjectExistenceState_MirrorReCreateNeeded)
		return;

	Assert(mirrorExistenceState == MirroredObjectExistenceState_MirrorCreatePending);

	if (!mirrorDataLossOccurred)
	{
		newMirrorExistenceState = MirroredObjectExistenceState_MirrorCreated;
		newRelDataSynchronizationState = MirroredRelDataSynchronizationState_Synchronized;
	}
	else
	{
		newMirrorExistenceState = MirroredObjectExistenceState_MirrorReCreateNeeded;
		newRelDataSynchronizationState = MirroredRelDataSynchronizationState_FullCopy;
	}

	// UNDONE: Don't fsync this change to the XLOG for now.  Determine
	// UNDONE: what needs to be fsync'd.
	PersistentFileSysObj_UpdateMirrorExistenceState(
											fsObjName->type,
											persistentTid,
											persistentSerialNum,
											newMirrorExistenceState,
											newRelDataSynchronizationState,
											/* flushToXLog */ false);
}

void MirroredFileSysObj_ValidateFilespaceDir(char *mirrorFilespaceLocation)
{
	char *parentdir;

	int status = FileRepPrimary_MirrorValidation(
		FileRep_GetDirFilespaceIdentifier(mirrorFilespaceLocation),
		FileRepRelationTypeDir
		);

	/* If everything was fine the error status will be zero */
	if (status == FileRepStatusSuccess)
		return;

	/* 
	 * Otherwise report the error, there are currently four known errors:
	 *  - File or directory exists
	 *  - Parent directory does not exist
	 *  - Parent directory not a directory
	 *  - No write permission on parent directory
	 */
	parentdir = pstrdup(mirrorFilespaceLocation);
	get_parent_directory(parentdir);
	switch (status)
	{
		/* Errors on the specified directory */
		case FileRepStatusDirectoryExist:
			ereport(ERROR, 
					(errcode_for_file_access(),
					 errmsg("%s : %s", 
							mirrorFilespaceLocation,
							FileRepStatusToString[status])));
			break;

		/* Errors on the parent directory */
		case FileRepStatusNoSuchFileOrDirectory:
		case FileRepStatusNotDirectory:
		case FileRepStatusNoWritePermissions:
			ereport(ERROR, 
					(errcode_for_file_access(),
					 errmsg("%s : %s", 
							parentdir,
							FileRepStatusToString[status])));
			break;

		/* Status that should never occur */
		default:
			elog(ERROR, "%s : %s", 
				 mirrorFilespaceLocation,
				 FileRepStatusToString[status]);
			break;
	}

	return;
}

void MirroredFileSysObj_TransactionCreateFilespaceDir(
	Oid					filespaceOid,

	int16				primaryDbId,
	
	char				*primaryFilespaceLocation,
						/* 
						 * The primary filespace directory path.  NOT Blank padded.
						 * Just a NULL terminated string.
						 */
	
	int16				mirrorDbId,
	
	char				*mirrorFilespaceLocation,

	ItemPointer 		persistentTid,
				/* Output: The TID of the gp_persistent_filespace_node tuple. */

	int64				*persistentSerialNum)
				/* Output: The serial number of the gp_persistent_filespace_node tuple. */
{
	PersistentFileSysObjName	fsObjName;
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	bool mirrorDataLossOccurred;

	Assert(primaryFilespaceLocation != NULL);

	PersistentFileSysObjName_SetFilespaceDir(
										&fsObjName,
										filespaceOid);

	LWLockAcquire(MirroredLock, LW_SHARED);

	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);

	MirroredFileSysObj_BeginMirroredCreate(
									&fsObjName,
									mirrorDataLossTrackingState,
									&mirrorExistenceState,
									&mirrorMode);
	/*
	 * We write our intention or 'Create Pending' persistent information before we do
	 * any create filespace work on either the primary or mirror.
	 */
	smgrcreatefilespacedirpending(
							filespaceOid, 
							primaryDbId, 
							primaryFilespaceLocation, 
							mirrorDbId, 
							mirrorFilespaceLocation,
							mirrorExistenceState,
							persistentTid,
							persistentSerialNum, 
							/* flushToXLog */ true);

	/*
	 * Synchronous primary and mirror create tablespace directory.
	 */
	MirroredFileSysObj_CreateFilespaceDir(
								filespaceOid,
								primaryFilespaceLocation,
								mirrorFilespaceLocation,
								mirrorMode,
								/* ignoreAlreadyExists */ false,
								&primaryError,
								&mirrorDataLossOccurred);
	if (primaryError != 0)
	{
		LWLockRelease(MirroredLock);
		// UNDONE: Need equiv. of GetDatabasePath here for filespace.
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create filespace directory %u: %s",
					filespaceOid,
					strerror(primaryError))));
	}
	
	MirroredFileSysObj_FinishMirroredCreate(
									&fsObjName,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorDataLossOccurred);	
	
	LWLockRelease(MirroredLock);
}

void MirroredFileSysObj_CreateFilespaceDir(
	Oid							filespaceOid,

	char						*primaryFilespaceLocation,
								/* 
								 * The primary filespace directory path.  NOT Blank padded.
								 * Just a NULL terminated string.
								 */
	
	char						*mirrorFilespaceLocation,

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{
	smgrcreatefilespacedir(
					filespaceOid, 
					primaryFilespaceLocation, 
					mirrorFilespaceLocation, 
					mirrorMode,
					ignoreAlreadyExists,
					primaryError,
					mirrorDataLossOccurred);
}

void MirroredFileSysObj_ScheduleDropFilespaceDir(
	Oid			filespaceOid)
{
	ItemPointerData persistentTid;
	int64 persistentSerialNum;

	PersistentFilespace_LookupTidAndSerialNum(
										filespaceOid,
										&persistentTid,
										&persistentSerialNum);

	smgrschedulermfilespacedir(
						filespaceOid,
						&persistentTid,
						persistentSerialNum);
}

void MirroredFileSysObj_DropFilespaceDir(
	Oid							filespaceOid,

	char						*primaryFilespaceLocation,
								/* 
								 * The primary filespace directory path.  NOT Blank padded.
								 * Just a NULL terminated string.
								 */
	
	char						*mirrorFilespaceLocation,

	bool					 	mirrorOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
	Assert(primaryFilespaceLocation != NULL);

	smgrdormfilespacedir(
				filespaceOid, 
				primaryFilespaceLocation, 
				mirrorFilespaceLocation, 
				mirrorOnly,
				ignoreNonExistence,
				mirrorDataLossOccurred);

}

void MirroredFileSysObj_TransactionCreateTablespaceDir(
	TablespaceDirNode	*tablespaceDirNode,

	ItemPointer 		persistentTid,
				/* Output: The TID of the gp_persistent_tablespace_node tuple. */

	int64				*persistentSerialNum)
				/* Output: The serial number of the gp_persistent_tablespace_node tuple. */
{
	PersistentFileSysObjName	fsObjName;
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	bool mirrorDataLossOccurred;

	PersistentFileSysObjName_SetTablespaceDir(
										&fsObjName,
										tablespaceDirNode->tablespace);

	LWLockAcquire(MirroredLock, LW_SHARED);

	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);

	MirroredFileSysObj_BeginMirroredCreate(
									&fsObjName,
									mirrorDataLossTrackingState,
									&mirrorExistenceState,
									&mirrorMode);
	/*
	 * We write our intention or 'Create Pending' persistent information before we do
	 * any create tablespace directory work on either the primary or mirror.
	 */
	smgrcreatetablespacedirpending(
							tablespaceDirNode, 
							mirrorExistenceState,
							persistentTid, 
							persistentSerialNum, 
							/* flushToXLog */ true);

	/*
	 * Synchronous primary and mirror create tablespace directory.
	 */
	MirroredFileSysObj_CreateTablespaceDir(
								tablespaceDirNode->tablespace,
								mirrorMode,
								/* ignoreAlreadyExists */ false,
								&primaryError,
								&mirrorDataLossOccurred);
	if (primaryError != 0)
	{
		LWLockRelease(MirroredLock);
		// UNDONE: Need equiv. of GetDatabasePath here for tablespace.
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create tablespace directory %u: %s",
					tablespaceDirNode->tablespace,
					strerror(primaryError))));
	}

	MirroredFileSysObj_FinishMirroredCreate(
									&fsObjName,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorDataLossOccurred);	

	LWLockRelease(MirroredLock);
}

void MirroredFileSysObj_CreateTablespaceDir(
	Oid							tablespaceOid,

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{
	smgrcreatetablespacedir(
					tablespaceOid, 
					mirrorMode,
					ignoreAlreadyExists,
					primaryError,
					mirrorDataLossOccurred);
}

void MirroredFileSysObj_ScheduleDropTablespaceDir(
	Oid			tablespaceOid)
{
	ItemPointerData persistentTid;
	int64 persistentSerialNum;

	PersistentTablespace_LookupTidAndSerialNum(
										tablespaceOid,
										&persistentTid,
										&persistentSerialNum);

	smgrschedulermtablespacedir(
						tablespaceOid,
						&persistentTid,
						persistentSerialNum);
}

void MirroredFileSysObj_DropTablespaceDir(
	Oid							tablespaceOid,

	bool					 	mirrorOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
	smgrdormtablespacedir(
				tablespaceOid, 
				mirrorOnly,
				ignoreNonExistence,
				mirrorDataLossOccurred);
}

void MirroredFileSysObj_TransactionCreateDbDir(
	DbDirNode			*dbDirNode,

	ItemPointer 		persistentTid,
				/* Output: The TID of the gp_persistent_relation_node tuple. */

	int64				*persistentSerialNum)
				/* Output: The serial number of the gp_persistent_relation_node tuple. */

{
	PersistentFileSysObjName	fsObjName;
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	bool mirrorDataLossOccurred;

	PersistentFileSysObjName_SetDatabaseDir(
										&fsObjName,
										dbDirNode->tablespace,
										dbDirNode->database);

	LWLockAcquire(MirroredLock, LW_SHARED);

	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);

	MirroredFileSysObj_BeginMirroredCreate(
									&fsObjName,
									mirrorDataLossTrackingState,
									&mirrorExistenceState,
									&mirrorMode);
	/*
	 * We write our intention or 'Create Pending' persistent information before we do
	 * any create database directory work on either the primary or mirror.
	 */
	smgrcreatedbdirpending(
				dbDirNode,
				mirrorExistenceState,
				persistentTid, 
				persistentSerialNum, 
				/* flushToXLog */ true);

	/*
	 * Synchronous primary and mirror create database directory.
	 */
	MirroredFileSysObj_CreateDbDir(
								dbDirNode,
								mirrorMode,
								/* ignoreAlreadyExists */ false,
								&primaryError,
								&mirrorDataLossOccurred);
	if (primaryError != 0)
	{
		LWLockRelease(MirroredLock);
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create database directory %s: %s",
					GetDatabasePath(dbDirNode->database, dbDirNode->tablespace),
					strerror(primaryError))));
	}

	MirroredFileSysObj_FinishMirroredCreate(
									&fsObjName,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorDataLossOccurred);	

	LWLockRelease(MirroredLock);
}

void MirroredFileSysObj_CreateDbDir(
	DbDirNode					*dbDirNode,

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{
	Assert(dbDirNode != NULL);

	smgrcreatedbdir(
				dbDirNode, 
				mirrorMode,
				ignoreAlreadyExists,
				primaryError,
				mirrorDataLossOccurred);
}

void MirroredFileSysObj_ScheduleDropDbDir(
	DbDirNode			*dbDirNode,
	
	ItemPointer		 	persistentTid,

	int64 				persistentSerialNum)
{
	Assert(dbDirNode != NULL);

	smgrschedulermdbdir(
					dbDirNode,
					persistentTid,
					persistentSerialNum);

}

void MirroredFileSysObj_DropDbDir(
	DbDirNode					*dbDirNode,

	bool 						mirrorOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
	Assert(dbDirNode != NULL);

	smgrdormdbdir(
			dbDirNode, 
			mirrorOnly, 
			ignoreNonExistence,
			mirrorDataLossOccurred);
}

static void MirroredFileSysObj_JustInTimeDbDirCreate(
	DbDirNode			*justInTimeDbDirNode)
{
	PersistentFileSysObjName	fsObjName;
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;

	ItemPointerData 		persistentTid;
	int64					persistentSerialNum;

	int primaryError;
	bool mirrorDataLossOccurred;

	Assert(justInTimeDbDirNode != NULL);

	if (justInTimeDbDirNode->tablespace == GLOBALTABLESPACE_OID ||
		(justInTimeDbDirNode->tablespace == DEFAULTTABLESPACE_OID &&
		 justInTimeDbDirNode->database == TemplateDbOid))
	{
		/*
		 * Optimize out the common cases.
		 */
		 return;
	}

	PersistentFileSysObjName_SetDatabaseDir(
										&fsObjName,
										justInTimeDbDirNode->tablespace,
										justInTimeDbDirNode->database);

	/*
	 * Acquire TablespaceCreateLock to ensure that no DROP TABLESPACE
	 * or MirroredFileSysObj_JustInTimeDbDirCreate is running concurrently.
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

	/* Prevent cancel/die interrupt while doing this multi-step */
	HOLD_INTERRUPTS();

	if (PersistentDatabase_DirIsCreated(justInTimeDbDirNode))
	{
		RESUME_INTERRUPTS();
		LWLockRelease(TablespaceCreateLock);
		return;
	}

	LWLockAcquire(MirroredLock, LW_SHARED);

	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);

	MirroredFileSysObj_BeginMirroredCreate(
									&fsObjName,
									mirrorDataLossTrackingState,
									&mirrorExistenceState,
									&mirrorMode);


	if (PersistentTablespace_GetState(justInTimeDbDirNode->tablespace) == PersistentFileSysState_CreatePending)
	{
		/*
		 * This tablespace was created in our explicit (i.e. BEGIN ... END) transaction.
		 *
		 * Otherwise, if would not be visible to us.
		 *
		 * Do a regular transaction database directory create.
		 */
		 
		/*
		 * We write our intention or 'Create Pending' persistent information before we do
		 * any create database directory work on either the primary or mirror.
		 */
		smgrcreatedbdirpending(
						justInTimeDbDirNode,
						mirrorExistenceState,
						&persistentTid, 
						&persistentSerialNum, 
						/* flushToXLog */ true);
		
		/*
		 * Synchronous primary and mirror create database directory.
		 */
		MirroredFileSysObj_CreateDbDir(
									justInTimeDbDirNode,
									mirrorMode,
									/* ignoreAlreadyExists */ false,
									&primaryError,
									&mirrorDataLossOccurred);
		if (primaryError != 0)
		{
			RESUME_INTERRUPTS();
			LWLockRelease(MirroredLock);
			LWLockRelease(TablespaceCreateLock);
			ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create database directory %u/%u: %s",
						justInTimeDbDirNode->tablespace,
						justInTimeDbDirNode->database,
						strerror(primaryError))));
		}
	}
	else
	{
		/*
		 * Do Just-In-Time non-transaction database directory create.
		 */
		smgrcreatedbdirjustintime(
							justInTimeDbDirNode,
							mirrorExistenceState,
							mirrorMode,
							&persistentTid, 
							&persistentSerialNum, 
							&primaryError,
							&mirrorDataLossOccurred);
		if (primaryError != 0)
		{
			RESUME_INTERRUPTS();
			LWLockRelease(MirroredLock);
			LWLockRelease(TablespaceCreateLock);
			ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create just-in-time database directory %u/%u: %s",
						justInTimeDbDirNode->tablespace,
						justInTimeDbDirNode->database,
						strerror(primaryError))));
		}
	}

	MirroredFileSysObj_FinishMirroredCreate(
									&fsObjName,
									&persistentTid, 
									persistentSerialNum, 
									mirrorExistenceState,
									mirrorDataLossOccurred);	


	RESUME_INTERRUPTS();

	LWLockRelease(MirroredLock);
	LWLockRelease(TablespaceCreateLock);
}

void MirroredFileSysObj_TransactionCreateBufferPoolFile(
	SMgrRelation 			smgrOpen,

	bool 					isLocalBuf,

	char					*relationName,

	bool					doJustInTimeDirCreate,

	ItemPointer 			persistentTid,

	int64					*persistentSerialNum)
{
	PersistentFileSysObjName	fsObjName;
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;

	int primaryError;
	bool mirrorDataLossOccurred;

	Assert(relationName != NULL);
	Assert(persistentTid != NULL);
	Assert(persistentSerialNum != NULL);

	if (doJustInTimeDirCreate)
	{
		DbDirNode justInTimeDbDirNode;
		
		/*
		 * "Fault-in" the database directory in a tablespace if it doesn't exist yet.
		 */
		justInTimeDbDirNode.tablespace = smgrOpen->smgr_rnode.spcNode;
		justInTimeDbDirNode.database = smgrOpen->smgr_rnode.dbNode;
		MirroredFileSysObj_JustInTimeDbDirCreate(&justInTimeDbDirNode);
	}

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName,
										&smgrOpen->smgr_rnode,
										/* segmentFileNum */ 0);

	LWLockAcquire(MirroredLock, LW_SHARED);

	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);

	MirroredFileSysObj_BeginMirroredCreate(
									&fsObjName,
									mirrorDataLossTrackingState,
									&mirrorExistenceState,
									&mirrorMode);

	/*
	 * We write our intention or 'Create Pending' persistent information before we do
	 * any create relation work on either the primary or mirror.
	 */
	smgrcreatepending(
				&smgrOpen->smgr_rnode,
				/* segmentFileNum */ 0,
				PersistentFileSysRelStorageMgr_BufferPool,
				mirrorExistenceState,
				MirroredRelDataSynchronizationState_None,	// ~_FinishMirroredCreate will update this to ~_Synchronized.
				relationName,
				persistentTid, 
				persistentSerialNum, 
				isLocalBuf, 
				/* flushToXLog */ true);

	/*
	 * Synchronous primary and mirror create relation.
	 */
	smgrcreate(
			smgrOpen,
			isLocalBuf,
			relationName,
			mirrorDataLossTrackingState,
			mirrorDataLossTrackingSessionNum,
			/* ignoreAlreadyExists */ false,
			&primaryError,
			&mirrorDataLossOccurred);
	if (primaryError != 0)
	{
		LWLockRelease(MirroredLock);
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create relation file '%s', relation name '%s': %s",
					relpath(smgrOpen->smgr_rnode),
					relationName,
					strerror(primaryError))));
	}

	MirroredFileSysObj_FinishMirroredCreate(
									&fsObjName,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorDataLossOccurred);	

	LWLockRelease(MirroredLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_TransactionCreateBufferPoolFile: %u/%u/%u, relation name '%s', mirror existence state '%s', mirror data loss occurred %s"
		     ", persistent serial number " INT64_FORMAT " at TID %s",
			 smgrOpen->smgr_rnode.spcNode,
			 smgrOpen->smgr_rnode.dbNode,
			 smgrOpen->smgr_rnode.relNode,
			 (relationName == NULL ? "<null>" : relationName),
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 (mirrorDataLossOccurred ? "true" : "false"),
			 *persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

void MirroredFileSysObj_TransactionCreateAppendOnlyFile(
	RelFileNode 			*relFileNode,

	int32					segmentFileNum,

	char					*relationName,

	bool					doJustInTimeDirCreate,

	ItemPointer 			persistentTid,
				/* Output: The TID of the gp_persistent_relation_node tuple. */

	int64					*persistentSerialNum)
				/* Output: The serial number of the gp_persistent_relation_node tuple. */
{
	PersistentFileSysObjName	fsObjName;
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState;
	int64 mirrorDataLossTrackingSessionNum;
	
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;

	int primaryError;
	bool mirrorDataLossOccurred;

	Assert(relationName != NULL);
	Assert(persistentTid != NULL);
	Assert(persistentSerialNum != NULL);

	if (doJustInTimeDirCreate)
	{
		DbDirNode justInTimeDbDirNode;
		
		/*
		 * "Fault-in" the database directory in a tablespace if it doesn't exist yet.
		 */
		justInTimeDbDirNode.tablespace = relFileNode->spcNode;
		justInTimeDbDirNode.database = relFileNode->dbNode;
		MirroredFileSysObj_JustInTimeDbDirCreate(&justInTimeDbDirNode);
	}

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName,
										relFileNode,
										segmentFileNum);

	LWLockAcquire(MirroredLock, LW_SHARED);

	mirrorDataLossTrackingState = 
				FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
												&mirrorDataLossTrackingSessionNum);

	MirroredFileSysObj_BeginMirroredCreate(
									&fsObjName,
									mirrorDataLossTrackingState,
									&mirrorExistenceState,
									&mirrorMode);

	/*
	 * We write our intention or 'Create Pending' persistent information before we do
	 * any create relation work on either the primary or mirror.
	 */
	smgrcreatepending(
				relFileNode,
				segmentFileNum,
				PersistentFileSysRelStorageMgr_AppendOnly,
				mirrorExistenceState,
				MirroredRelDataSynchronizationState_None,	// ~_FinishMirroredCreate will update this to ~_Synchronized.
				relationName,
				persistentTid, 
				persistentSerialNum, 
				/* isLocalBuf */ false, 
				/* flushToXLog */ true);

	/*
	 * Synchronous primary and mirror create relation.
	 */
	MirroredAppendOnly_Create(
							relFileNode,
							segmentFileNum,
							relationName,
							mirrorDataLossTrackingState,
							mirrorDataLossTrackingSessionNum,
							&primaryError,
							&mirrorDataLossOccurred);
	if (primaryError != 0)
	{
		LWLockRelease(MirroredLock);
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create relation file '%s', relation name '%s': %s",
					relpath(*relFileNode),
					relationName,
					strerror(primaryError))));
	}

	MirroredFileSysObj_FinishMirroredCreate(
									&fsObjName,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorDataLossOccurred);	

	LWLockRelease(MirroredLock);
	
	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_TransactionCreateAppendOnlyFile: %u/%u/%u, relation name '%s', mirror existence state '%s', mirror data loss occurred %s"
			 ", persistent serial number " INT64_FORMAT " at TID %s",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 (relationName == NULL ? "<null>" : relationName),
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 (mirrorDataLossOccurred ? "true" : "false"),
			 *persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

void MirroredFileSysObj_ScheduleDropBufferPoolFile(
	SMgrRelation 				smgrOpen,

	bool 						isLocalBuf,

	char						*relationName,

	ItemPointer 				persistentTid,

	int64						persistentSerialNum)
{
	Assert(persistentTid != NULL);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_ScheduleDropBufferPoolFile: %u/%u/%u, relation name '%s'",
			 smgrOpen->smgr_rnode.spcNode,
			 smgrOpen->smgr_rnode.dbNode,
			 smgrOpen->smgr_rnode.relNode,
			 relationName);

	smgrscheduleunlink(
					&smgrOpen->smgr_rnode,
					/* segmentFileNum */ 0,
					PersistentFileSysRelStorageMgr_BufferPool,
					isLocalBuf,
					relationName,
					persistentTid, 
					persistentSerialNum);

	/* Now close the file and throw away the hashtable entry */
	smgrclose(smgrOpen);
}

void MirroredFileSysObj_ScheduleDropAppendOnlyFile(
	RelFileNode 				*relFileNode,

	int32						segmentFileNum,

	char						*relationName,

	ItemPointer 				persistentTid,

	int64						persistentSerialNum)
{
	Assert(persistentTid != NULL);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_ScheduleDropAppendOnlyFile: %u/%u/%u, relation name '%s'",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 relationName);

	smgrscheduleunlink(
					relFileNode,
					segmentFileNum,
					PersistentFileSysRelStorageMgr_AppendOnly,
					/* isLocalBuf */ false,
					relationName,
					persistentTid, 
					persistentSerialNum);
}

void MirroredFileSysObj_DropRelFile(
	RelFileNode 				*relFileNode,

	int32						segmentFileNum,

	PersistentFileSysRelStorageMgr relStorageMgr,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	bool 						isLocalBuf,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
	int primaryError;

	switch (relStorageMgr)
	{
	case PersistentFileSysRelStorageMgr_None:	// UNDONE: Assume.
	case PersistentFileSysRelStorageMgr_BufferPool:
		smgrdounlink(
				relFileNode, 
				isLocalBuf,
				relationName,
				ignoreNonExistence,
				mirrorDataLossOccurred);
		break;

	case PersistentFileSysRelStorageMgr_AppendOnly:
		MirroredAppendOnly_Drop(
					relFileNode,
					segmentFileNum,
					relationName,
					&primaryError,
					mirrorDataLossOccurred);
		if (ignoreNonExistence && primaryError == ENOENT)
			primaryError = 0;

		// UNDONE: This needs to be an error.
		if (primaryError != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove relation %u/%u/%u (segment file #%d): %s",
							relFileNode->spcNode,
							relFileNode->dbNode,
							relFileNode->relNode,
							segmentFileNum,
							strerror(primaryError))));
		break;

	default:
		elog(ERROR, "Unexpected relation storage manager %d", relStorageMgr);
	}
}

 
