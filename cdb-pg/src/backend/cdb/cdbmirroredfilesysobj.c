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
	MirroredObjectExistenceState 	*mirrorExistenceState,
	
	StorageManagerMirrorMode 		*mirrorMode)
{
	DataState_e 	snapDataState = dataState;

	/* Kludge for initdb */
	if (gp_initdb_mirrored)
	{
		*mirrorExistenceState = MirroredObjectExistenceState_MirrorCreatePending;
		*mirrorMode = StorageManagerMirrorMode_Both;
		return;
	}
	
	switch (snapDataState)
	{
	case DataStateNotInitialized:
		*mirrorExistenceState = MirroredObjectExistenceState_NotMirrored;
		*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		break;
		
	case DataStateInSync:
	case DataStateInResync:
		*mirrorExistenceState = MirroredObjectExistenceState_MirrorCreatePending;
		*mirrorMode = StorageManagerMirrorMode_Both;
		break;
			
	case DataStateInChangeTracking:
		*mirrorExistenceState = MirroredObjectExistenceState_MirrorReCreateNeeded;
		*mirrorMode = StorageManagerMirrorMode_PrimaryOnly;
		break;
		
	default:
		elog(ERROR, "Unexpected file-rep state: %d",
			 snapDataState);
	}
}

static void MirroredFileSysObj_FinishMirroredCreate(
	PersistentFsObjType				fsObjType,

	ItemPointer 					persistentTid,
	
	int64							persistentSerialNum,

	MirroredObjectExistenceState 	mirrorExistenceState,
	
	int						 		mirrorError)
{
	MirroredObjectExistenceState 	newMirrorExistenceState;
	MirroredRelDataSynchronizationState newRelDataSynchronizationState;

	if (mirrorExistenceState == MirroredObjectExistenceState_NotMirrored ||
		mirrorExistenceState == MirroredObjectExistenceState_MirrorReCreateNeeded)
		return;

	Assert(mirrorExistenceState == MirroredObjectExistenceState_MirrorCreatePending);

	if (mirrorError == 0)
	{
		newMirrorExistenceState = MirroredObjectExistenceState_MirrorCreated;
		newRelDataSynchronizationState = MirroredRelDataSynchronizationState_Synchronized;
	}
	else
	{
		newMirrorExistenceState = MirroredObjectExistenceState_MirrorReCreateNeeded;
		newRelDataSynchronizationState = MirroredRelDataSynchronizationState_None;
	}

	// UNDONE: Don't fsync this change to the XLOG for now.  Determine
	// UNDONE: what needs to be fsync'd.
	PersistentFileSysObj_UpdateMirrorExistenceState(
											fsObjType,
											persistentTid,
											persistentSerialNum,
											newMirrorExistenceState,
											newRelDataSynchronizationState,
											/* flushToXLog */ false);
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
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	int mirrorError;

	Assert(primaryFilespaceLocation != NULL);

	LWLockAcquire(MirroredLock, LW_SHARED);

	MirroredFileSysObj_BeginMirroredCreate(
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
								&mirrorError);
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
									PersistentFsObjType_FilespaceDir,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorError);	
	
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

	int							*mirrorError)
{
	smgrcreatefilespacedir(
					filespaceOid, 
					primaryFilespaceLocation, 
					mirrorFilespaceLocation, 
					mirrorMode,
					ignoreAlreadyExists,
					primaryError,
					mirrorError);
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

	StorageManagerMirrorMode 	mirrorMode,

	bool 						ignoreNonExistence)
{
	Assert(primaryFilespaceLocation != NULL);

	smgrdormfilespacedir(
				filespaceOid, 
				primaryFilespaceLocation, 
				mirrorFilespaceLocation, 
				mirrorMode,
				ignoreNonExistence);

}

void MirroredFileSysObj_TransactionCreateTablespaceDir(
	TablespaceDirNode	*tablespaceDirNode,

	ItemPointer 		persistentTid,
				/* Output: The TID of the gp_persistent_tablespace_node tuple. */

	int64				*persistentSerialNum)
				/* Output: The serial number of the gp_persistent_tablespace_node tuple. */
{
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	int mirrorError;

	LWLockAcquire(MirroredLock, LW_SHARED);

	MirroredFileSysObj_BeginMirroredCreate(
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
								&mirrorError);
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
									PersistentFsObjType_TablespaceDir,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorError);	

	LWLockRelease(MirroredLock);
}

void MirroredFileSysObj_CreateTablespaceDir(
	Oid							tablespaceOid,

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	int							*mirrorError)
{
	smgrcreatetablespacedir(
					tablespaceOid, 
					mirrorMode,
					ignoreAlreadyExists,
					primaryError,
					mirrorError);
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

	StorageManagerMirrorMode 	mirrorMode,

	bool 						ignoreNonExistence)
{
	smgrdormtablespacedir(
				tablespaceOid, 
				mirrorMode,
				ignoreNonExistence);
}

void MirroredFileSysObj_TransactionCreateDbDir(
	DbDirNode			*dbDirNode,

	ItemPointer 		persistentTid,
				/* Output: The TID of the gp_persistent_relation_node tuple. */

	int64				*persistentSerialNum)
				/* Output: The serial number of the gp_persistent_relation_node tuple. */

{

	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	int mirrorError;

	LWLockAcquire(MirroredLock, LW_SHARED);

	MirroredFileSysObj_BeginMirroredCreate(
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
								&mirrorError);
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
									PersistentFsObjType_DatabaseDir,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorError);	

	LWLockRelease(MirroredLock);
}

void MirroredFileSysObj_CreateDbDir(
	DbDirNode					*dbDirNode,

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	int							*mirrorError)
{
	Assert(dbDirNode != NULL);

	smgrcreatedbdir(
				dbDirNode, 
				mirrorMode,
				ignoreAlreadyExists,
				primaryError,
				mirrorError);
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

	StorageManagerMirrorMode 	mirrorMode,

	bool 						ignoreNonExistence)
{
	Assert(dbDirNode != NULL);

	smgrdormdbdir(
			dbDirNode, 
			mirrorMode, 
			ignoreNonExistence);
}

static void MirroredFileSysObj_JustInTimeDbDirCreate(
	DbDirNode			*justInTimeDbDirNode)
{
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

	if (!smgrcreatedbdirjustintime(justInTimeDbDirNode))
	{
		RESUME_INTERRUPTS();
		LWLockRelease(TablespaceCreateLock);
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create database directory %u/%u: %m",
					justInTimeDbDirNode->tablespace,
					justInTimeDbDirNode->database)));
	}
	
	RESUME_INTERRUPTS();

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
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	int mirrorError;

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

	LWLockAcquire(MirroredLock, LW_SHARED);

	MirroredFileSysObj_BeginMirroredCreate(
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
	MirroredFileSysObj_CreateBufferPoolFile(
									smgrOpen,
									isLocalBuf,
									relationName,
									mirrorMode,
									/* ignoreAlreadyExists */ false,
									&primaryError,
									&mirrorError);
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
									PersistentFsObjType_RelationFile,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorError);	

	LWLockRelease(MirroredLock);
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
	MirroredObjectExistenceState mirrorExistenceState = MirroredObjectExistenceState_None;
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_None;
	int primaryError;
	int mirrorError;

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

	LWLockAcquire(MirroredLock, LW_SHARED);

	MirroredFileSysObj_BeginMirroredCreate(
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
	MirroredFileSysObj_CreateAppendOnlyFile(
									relFileNode,
									segmentFileNum,
									relationName,
									mirrorMode,
									/* ignoreAlreadyExists */ false,
									&primaryError,
									&mirrorError);
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
									PersistentFsObjType_RelationFile,
									persistentTid, 
									*persistentSerialNum, 
									mirrorExistenceState,
									mirrorError);	

	LWLockRelease(MirroredLock);
}

void MirroredFileSysObj_CreateBufferPoolFile(
	SMgrRelation 				smgrOpen,

	bool 						isLocalBuf,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	int							*mirrorError)
{
	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_CreateBufferPoolFile: %u/%u/%u, relation name '%s', mirror mode '%s', ignore already exists %s",
			 smgrOpen->smgr_rnode.spcNode,
			 smgrOpen->smgr_rnode.dbNode,
			 smgrOpen->smgr_rnode.relNode,
			 (relationName == NULL ? "<null>" : relationName),
			 StorageManagerMirrorMode_Name(mirrorMode),
			 (ignoreAlreadyExists ? "true" : "false"));

	smgrcreate(
			smgrOpen,
			isLocalBuf,
			relationName,
			mirrorMode,
			ignoreAlreadyExists,
			primaryError,
			mirrorError);
}

void MirroredFileSysObj_CreateAppendOnlyFile(
	RelFileNode 				*relFileNode,

	int32						segmentFileNum,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	int							*mirrorError)
{
	MirroredAppendOnlyOpen		mirroredOpen;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "MirroredFileSysObj_CreateAppendOnlyFile: %u/%u/%u, relation name '%s', mirror mode '%s', ignore already exists %s",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 (relationName == NULL ? "<null>" : relationName),
			 StorageManagerMirrorMode_Name(mirrorMode),
			 (ignoreAlreadyExists ? "true" : "false"));

	*mirrorError = 0;	// UNDONE: Not plumbed yet.

	MirroredAppendOnly_Open(
				&mirroredOpen, 
				relFileNode,
				segmentFileNum,
				relationName,
				/* logicalEof */ 0,
				O_RDWR | PG_BINARY | O_CREAT,
				0600);
	if(mirroredOpen.primaryFile < 0)
	{
		*primaryError = errno;
		return;
	}
	
	MirroredAppendOnly_FlushAndClose(&mirroredOpen);

	*primaryError = 0;
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

	StorageManagerMirrorMode 	mirrorMode,

	bool 						ignoreNonExistence)
{
	switch (relStorageMgr)
	{
	case PersistentFileSysRelStorageMgr_None:	// UNDONE: Assume.
	case PersistentFileSysRelStorageMgr_BufferPool:
		smgrdounlink(
				relFileNode, 
				isLocalBuf,
				relationName,
				mirrorMode, 
				ignoreNonExistence);
		break;

	case PersistentFileSysRelStorageMgr_AppendOnly:
		MirroredAppendOnly_Drop(
					relFileNode,
					segmentFileNum,
					relationName,
					mirrorMode, 
					ignoreNonExistence);
		break;

	default:
		elog(ERROR, "Unexpected relation storage manager %d", relStorageMgr);
	}
}

