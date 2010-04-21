/*-------------------------------------------------------------------------
 *
 * cdbpersistentrelation.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"

#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbsharedoidsearch.h"
#include "access/persistentfilesysobjname.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistentrelation.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"
#include "storage/shmem.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"


/*
 * This module is for generic relation file create and drop.
 *
 * For create, it makes the file-system create of an empty file fully transactional so
 * the relation file will be deleted even on system crash.  The relation file could be a heap,
 * index, or append-only (row- or column-store).
 */

typedef struct PersistentRelationSharedData
{

	PersistentFileSysObjSharedData		fileSysObjSharedData;

} PersistentRelationSharedData;

#define PersistentRelationData_StaticInit {PersistentFileSysObjData_StaticInit}

typedef struct PersistentRelationData
{

	PersistentFileSysObjData		fileSysObjData;

} PersistentRelationData;


/*
 * Global Variables
 */
PersistentRelationSharedData	*persistentRelationSharedData = NULL;

PersistentRelationData	persistentRelationData = PersistentRelationData_StaticInit;

static void PersistentRelation_InitScan(void)
{
	if (persistentRelationSharedData == NULL)
		elog(PANIC, "Persistent relation information shared-memory not setup");

	PersistentFileSysObj_InitScan();
}

// -----------------------------------------------------------------------------
// Helpers 
// -----------------------------------------------------------------------------

inline static void XLogRecPtr_Zero(XLogRecPtr *xlogLoc)
{
	MemSet(xlogLoc, 0, sizeof(XLogRecPtr));
}

void PersistentRelation_FlushXLog(void)
{
	PersistentFileSysObj_FlushXLog();
}

//------------------------------------------------------------------------------

int64 PersistentRelation_MyHighestSerialNum(void)
{
	return PersistentFileSysObj_MyHighestSerialNum(
							PersistentFsObjType_RelationFile);
}

int64 PersistentRelation_CurrentMaxSerialNum(void)
{
	bool lockHeldByMe;

	int64 value;

	lockHeldByMe = LWLockHeldByMe(PersistentObjLock);

	if (!lockHeldByMe)
		LWLockAcquire(PersistentObjLock, LW_SHARED);

	value = PersistentFileSysObj_CurrentMaxSerialNum(
							PersistentFsObjType_RelationFile);

	if (!lockHeldByMe)
		LWLockRelease(PersistentObjLock);

	return value;
}

static Oid persistentRelationCheckTablespace;
static int32 persistentRelationCheckTablespaceUseCount;
static RelFileNode persistentRelationCheckTablespaceRelFileNode;

static bool PersistentRelation_CheckTablespaceScanTupleCallback(
	int 					token,
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	RelFileNode		relFileNode;
	int32			segmentFileNum;

	PersistentFileSysRelStorageMgr relationStorageManager;

	PersistentFileSysState	state;

	MirroredObjectExistenceState		mirrorExistenceState;

	MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
	bool							mirrorBufpoolPhysicallyTruncated;
	XLogRecPtr						mirrorBufpoolResyncCkptLoc;
	BlockNumber 					mirrorBufpoolResyncCkptBlockNum;

	int64					mirrorAppendOnlyLossEof;
	int64					mirrorAppendOnlyNewEof;
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	Assert(token == PersistentFsObjType_RelationFile);

	GpPersistentRelationNode_GetValues(
									values,
									&relFileNode.spcNode,
									&relFileNode.dbNode,
									&relFileNode.relNode,
									&segmentFileNum,
									&relationStorageManager,
									&state,
									&mirrorExistenceState,
									&mirrorDataSynchronizationState,
									&mirrorBufpoolPhysicallyTruncated,
									&mirrorBufpoolResyncCkptLoc,
									&mirrorBufpoolResyncCkptBlockNum,
									&mirrorAppendOnlyLossEof,
									&mirrorAppendOnlyNewEof,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Created &&
		relFileNode.spcNode == persistentRelationCheckTablespace)
	{
		persistentRelationCheckTablespaceUseCount++;
		if (persistentRelationCheckTablespaceUseCount == 1)
		{
			memcpy(&persistentRelationCheckTablespaceRelFileNode, &relFileNode, sizeof(RelFileNode));
		}
	}

	return true;	// Continue.
}

void PersistentRelation_CheckTablespace(
	Oid				tablespace,

	int32			*useCount,

	RelFileNode		*exampleRelFileNode)
{
	persistentRelationCheckTablespace = tablespace;
	persistentRelationCheckTablespaceUseCount = 0;

	MemSet(&persistentRelationCheckTablespaceRelFileNode, 0, sizeof(RelFileNode));

	PersistentFileSysObj_Scan(
		PersistentFsObjType_RelationFile,
		PersistentRelation_CheckTablespaceScanTupleCallback);

	*useCount = persistentRelationCheckTablespaceUseCount;
	memcpy(exampleRelFileNode, &persistentRelationCheckTablespaceRelFileNode, sizeof(RelFileNode));
}

// -----------------------------------------------------------------------------
// State Change 
// -----------------------------------------------------------------------------

/*
 * Indicate we intend to create a relation file as part of the current transaction.
 *
 * An XLOG IntentToCreate record is generated that will guard the subsequent file-system
 * create in case the transaction aborts.
 *
 * After 1 or more calls to this routine to mark intention about relation files that are going
 * to be created, call ~_DoPendingCreates to do the actual file-system creates.  (See its
 * note on XLOG flushing).
 */
void PersistentRelation_AddCreatePending(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the create. */

	int32				segmentFileNum,

	PersistentFileSysRelStorageMgr relStorageMgr,

	MirroredObjectExistenceState mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	char				*relationName,

	ItemPointer			persistentTid,
				/* Resulting TID of the gp_persistent_rel_files tuple for the relation. */

	int64				*serialNum,
				/* Resulting serial number for the relation.  Distinquishes the uses of the tuple. */

	bool 				flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	PersistentFileSysObjName fsObjName;

	XLogRecPtr mirrorBufpoolResyncCkptLoc;
	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));
	MemSet(&mirrorBufpoolResyncCkptLoc, 0, sizeof(XLogRecPtr));

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are before persistence work",
				 relpath(*relFileNode));

		MemSet(persistentTid, 0, sizeof(ItemPointerData));
		*serialNum = 0;

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName, 
										relFileNode,
										segmentFileNum);

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	GpPersistentRelationNode_SetDatumValues(
										values,
										relFileNode->spcNode,
										relFileNode->dbNode,
										relFileNode->relNode,
										segmentFileNum,
										relStorageMgr,
										PersistentFileSysState_CreatePending,
										mirrorExistenceState,
										relDataSynchronizationState,
										/* mirrorBufpoolPhysicallyTruncated */ false,
										&mirrorBufpoolResyncCkptLoc,
										/* mirrorBufpoolResyncCkptBlockNum */ 0,
										/* mirrorAppendOnlyLossEof */ 0,
										/* mirrorAppendOnlyNewEof */ 0,
										GetTopTransactionId(),
										/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
										&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							PersistentFsObjType_RelationFile,
							values,
							flushToXLog,
							persistentTid,
							serialNum);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: Add '%s', relation name '%s' in state 'Create Pending', relation storage manager '%s', mirror existence state '%s', relation data resynchronization state '%s', serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 relationName,
			 PersistentFileSysRelStorageMgr_Name(relStorageMgr),
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 MirroredRelDataSynchronizationState_Name(relDataSynchronizationState),
			 *serialNum,
			 ItemPointerToString(persistentTid));
}

void PersistentRelation_AddCreated(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the create. */

	int32				segmentFileNum,

	PersistentFileSysRelStorageMgr relStorageMgr,

	MirroredObjectExistenceState mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	int64				mirrorAppendOnlyLossEof,

	int64				mirrorAppendOnlyNewEof,

	char				*relationName,

	ItemPointer			persistentTid,
				/* Resulting TID of the gp_persistent_rel_files tuple for the relation. */

	int64				*persistentSerialNum,
				/* Resulting serial number for the relation.  Distinquishes the uses of the tuple. */

	bool 				flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	PersistentFileSysObjName fsObjName;

	XLogRecPtr mirrorBufpoolResyncCkptLoc;
	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));
	MemSet(&mirrorBufpoolResyncCkptLoc, 0, sizeof(XLogRecPtr));

	Assert (PersistentFileSysObj_BeforePersistenceWork());

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName, 
										relFileNode,
										segmentFileNum);

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	GpPersistentRelationNode_SetDatumValues(
										values,
										relFileNode->spcNode,
										relFileNode->dbNode,
										relFileNode->relNode,
										segmentFileNum,
										relStorageMgr,
										PersistentFileSysState_Created,
										mirrorExistenceState,
										relDataSynchronizationState,
										/* mirrorBufpoolPhysicallyTruncated */ false,
										&mirrorBufpoolResyncCkptLoc,
										/* mirrorBufpoolResyncCkptBlockNum */ 0,
										mirrorAppendOnlyLossEof,
										mirrorAppendOnlyNewEof,
										InvalidTransactionId,
										/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
										&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							PersistentFsObjType_RelationFile,
							values,
							flushToXLog,
							persistentTid,
							persistentSerialNum);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: Add '%s', relation name '%s', in state 'Created', relation storage manager '%s', mirror existence state '%s', relation data resynchronization state '%s', serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 relationName,
			 PersistentFileSysRelStorageMgr_Name(relStorageMgr),
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 MirroredRelDataSynchronizationState_Name(relDataSynchronizationState),
			 *persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

// -----------------------------------------------------------------------------
// Transaction End  
// -----------------------------------------------------------------------------

/*
 * Indicate the transaction commited and the relation is officially created.
 */
void PersistentRelation_Created(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the created relation. */

	int32				segmentFileNum,

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				persistentSerialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are before persistence work",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName, 
										relFileNode,
										segmentFileNum);

	// Do this check after skipping out if in bootstrap mode.
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for 'Created' is invalid (0,0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for 'Created' is invalid (0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Created,
								/* flushToXlog */ false,
								NULL);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from 'Create Pending' to 'Created', serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

/*
 * Indicate we intend to drop a relation file as part of the current transaction.
 *
 * This relation file to drop will be listed inside a commit, distributed commit, a distributed 
 * prepared, and distributed commit prepared XOG records.
 *
 * For any of the commit type records, once that XLOG record is flushed then the actual
 * file-system delete will occur.  The flush guarantees the action will be retried after system
 * crash.
 */
void PersistentRelation_MarkDropPending(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the drop. */

	int32				segmentFileNum,

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				persistentSerialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	PersistentFileSysState oldState;

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are before persistence work",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName, 
										relFileNode,
										segmentFileNum);

	// Do this check after skipping out if in bootstrap mode.
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for mark DROP pending is invalid (0,0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for mark DROP pending is invalid (0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
	
	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_DropPending,
								/* flushToXlog */ false,
								&oldState);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from '%s' to 'Drop Pending', serial number " INT64_FORMAT " TID %s",
			 relpath(*relFileNode),
			 PersistentFileSysObjState_Name(oldState),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

/*
 * Indicate we are aborting the create of a relation file.
 *
 * This state will make sure the relation gets dropped after a system crash.
 */
void PersistentRelation_MarkAbortingCreate(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the aborting create. */

	int32				segmentFileNum,

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				persistentSerialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent relation '%s' because we are before persistence work",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName, 
										relFileNode,
										segmentFileNum);

	// Do this check after skipping out if in bootstrap mode.
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for mark DROP pending is invalid (0,0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for mark DROP pending is invalid (0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
	
	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_AbortingCreate,
								/* flushToXlog */ false,
								NULL);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from 'Create Pending' to 'Aborting Create', serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

/*
 * Indicate we physically removed the relation file.
 */
void PersistentRelation_Dropped(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the dropped relation. */

	int32				segmentFileNum,

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				persistentSerialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	PersistentFileSysState oldState;

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are before persistence work",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName, 
										relFileNode,
										segmentFileNum);

	// Do this check after skipping out if in bootstrap mode.
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for mark DROP pending is invalid (0,0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for mark DROP pending is invalid (0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Free,
								/* flushToXlog */ false,
								&oldState);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from '%s' to (Free), serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 PersistentFileSysObjState_Name(oldState),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

void PersistentRelation_MarkBufPoolRelationPhysicallyTruncated(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the created relation. */

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				persistentSerialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	if (GpPersistent_SkipXLogInfo(relFileNode->relNode))
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent relation '%s' because it is special",
				 relpath(*relFileNode));
	
		return; // Resynchronize will always handle these relations as 'Scan Incremental'..
	}

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are in bootstrap mode",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent relation '%s' because we are before persistence work",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName, 
										relFileNode,
										/* segmentFileNum */ 0);

	// Do this check after skipping out if in bootstrap mode.
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for mark physically truncated is invalid (0,0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for mark physcially truncated is invalid (0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
	
	PersistentFileSysObj_MarkBufPoolRelationPhysicallyTruncated(
										&fsObjName,
										persistentTid,
										persistentSerialNum,
										/* flushToXlog */ true);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' marked physically truncated, serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

static Size PersistentRelation_SharedDataSize(void)
{
	return MAXALIGN(sizeof(PersistentRelationSharedData));
}

/*
 * Return the required shared-memory size for this module.
 */
Size PersistentRelation_ShmemSize(void)
{
	Size size = 0;

	/* The shared-memory structure. */
	size = add_size(size, PersistentRelation_SharedDataSize());

	return size;
}

/*
 * Initialize the shared-memory for this module.
 */
void PersistentRelation_ShmemInit(void)
{
	bool found;

	/* Create the shared-memory structure. */
	persistentRelationSharedData = 
		(PersistentRelationSharedData *)
						ShmemInitStruct("Mirrored Rel File Data",
										PersistentRelation_SharedDataSize(),
										&found);

	if (!found)
	{
		PersistentFileSysObj_InitShared(
						&persistentRelationSharedData->fileSysObjSharedData);
	}

	PersistentFileSysObj_Init(
						&persistentRelationData.fileSysObjData,
						&persistentRelationSharedData->fileSysObjSharedData,
						PersistentFsObjType_RelationFile,
						/* scanTupleCallback */ NULL);

	Assert(persistentRelationSharedData != NULL);
}
