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


static void PersistentRelation_InitScanUnderLock(void)
{
	PersistentDatabase_InitScanUnderLock();

	if (persistentRelationSharedData == NULL)
		elog(PANIC, "Persistent relation information shared-memory not setup");

	PersistentFileSysObj_InitScanUnderLock(
						&persistentRelationData.fileSysObjData,
						&persistentRelationSharedData->fileSysObjSharedData);
}

static void PersistentRelation_InitScan(void)
{
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentRelation_InitScanUnderLock();

	LWLockRelease(PersistentObjLock);
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

Datum
gp_persistent_relation_node_add(PG_FUNCTION_ARGS)
{
	Oid				tablespace = PG_GETARG_OID(0);
	Oid				database = PG_GETARG_OID(1);
	Oid				relationNode = PG_GETARG_OID(2);
	int16			state = PG_GETARG_INT16(3);

	ItemPointerData persistentTid;
	int64 persistentSerialNum;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	// UNDONE: Remove whole routine
	GpPersistentRelationNode_SetDatumValues(
										values,
										tablespace,
										database,
										relationNode,
										/* segmentFileNum */ 0,
										PersistentFileSysRelStorageMgr_None,	// UNDONE
										(PersistentFileSysState)state,
										MirrorFileSysState_None,	// UNDONE
										/* mirrorAppendOnlyEof */ 0,
										InvalidTransactionId,
										0,
										&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							&persistentRelationData.fileSysObjData,
							&persistentRelationSharedData->fileSysObjSharedData,
							values,
							/* flushToXLog */ false,
							&persistentTid,
							&persistentSerialNum);

//	if (Debug_persistent_print)
//		elog(Debug_persistent_print_level, 
//		     "Persistent relation: gp_persistent_relation_node_add '%s' in state '%s', serial number " INT64_FORMAT " at TID %s",
//			 relpath(relFileNode),
//			 PersistentFileSysObj_StateName(state),
//			 persistentSerialNum,
//			 ItemPointerToString(&persistentTid));

	PG_RETURN_INT64(persistentSerialNum);
}

//------------------------------------------------------------------------------

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
	MirrorFileSysState		mirrorState;
	int64					mirrorAppendOnlyEof;
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
									&mirrorState,
									&mirrorAppendOnlyEof,
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
		&persistentRelationData.fileSysObjData,
		&persistentRelationSharedData->fileSysObjSharedData,
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

	ItemPointer			persistentTid,
				/* Resulting TID of the gp_persistent_rel_files tuple for the relation. */

	int64				*serialNum,
				/* Resulting serial number for the relation.  Distinquishes the uses of the tuple. */

	bool 				flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are in bootstrap mode",
				 relpath(*relFileNode));

		MemSet(persistentTid, 0, sizeof(ItemPointerData));
		*serialNum = 0;

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	GpPersistentRelationNode_SetDatumValues(
										values,
										relFileNode->spcNode,
										relFileNode->dbNode,
										relFileNode->relNode,
										/* segmentFileNum */ 0,					// UNDONE
										PersistentFileSysRelStorageMgr_None,	// UNDONE
										PersistentFileSysState_CreatePending,
										MirrorFileSysState_None,				// UNDONE
										/* mirrorAppendOnlyEof */ 0,				// UNDONE
										GetTopTransactionId(),
										/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
										&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							&persistentRelationData.fileSysObjData,
							&persistentRelationSharedData->fileSysObjSharedData,
							values,
							flushToXLog,
							persistentTid,
							serialNum);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: Add '%s' in state 'Create Pending', serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 *serialNum,
			 ItemPointerToString(persistentTid));
}

void PersistentRelation_AddCreated(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the create. */

	ItemPointer			persistentTid,
				/* Resulting TID of the gp_persistent_rel_files tuple for the relation. */

	int64				*serialNum,
				/* Resulting serial number for the relation.  Distinquishes the uses of the tuple. */

	bool 				flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are in bootstrap mode",
				 relpath(*relFileNode));

		MemSet(persistentTid, 0, sizeof(ItemPointerData));
		*serialNum = 0;

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	GpPersistentRelationNode_SetDatumValues(
										values,
										relFileNode->spcNode,
										relFileNode->dbNode,
										relFileNode->relNode,
										/* segmentFileNum */ 0, 					// UNDONE
										PersistentFileSysRelStorageMgr_None,	// UNDONE
										PersistentFileSysState_Created,
										MirrorFileSysState_None,				// UNDONE
										/* mirrorAppendOnlyEof */ 0,				// UNDONE
										InvalidTransactionId,
										/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
										&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							&persistentRelationData.fileSysObjData,
							&persistentRelationSharedData->fileSysObjSharedData,
							values,
							flushToXLog,
							persistentTid,
							serialNum);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: Add '%s' in state 'Created', serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 *serialNum,
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

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				serialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	PersistentFileSysState	actualState;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are in bootstrap mode",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(&fsObjName,relFileNode);
	
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
	
	PersistentFileSysObj_VerifyExpected(
				&persistentRelationData.fileSysObjData,
				&persistentRelationSharedData->fileSysObjSharedData,
				&fsObjName,
				persistentTid,
				serialNum,
				/* expectedState1 */ PersistentFileSysState_CreatePending,
				/* expectedState2 */ (PersistentFileSysState)-1,
				/* nextState */ PersistentFileSysState_Created,
				&actualState);

	GpPersistentRelationNode_SetDatumValues(
										values,
										relFileNode->spcNode,
										relFileNode->dbNode,
										relFileNode->relNode,
										/* segmentFileNum */ 0, 					// UNDONE
										PersistentFileSysRelStorageMgr_None,	// UNDONE
										PersistentFileSysState_Created,
										MirrorFileSysState_None,				// UNDONE
										/* mirrorAppendOnlyEof */ 0,				// UNDONE
										InvalidTransactionId,
										serialNum,
										&previousFreeTid);

	PersistentFileSysObj_UpdateTuple(
							&persistentRelationData.fileSysObjData,
							&persistentRelationSharedData->fileSysObjSharedData,
							persistentTid,
							values,
							/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from 'Create Pending' to 'Created', serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 serialNum,
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

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				serialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	PersistentFileSysState	actualState;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are in bootstrap mode",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(&fsObjName,relFileNode);
	
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
	
	PersistentFileSysObj_VerifyExpected(
				&persistentRelationData.fileSysObjData,
				&persistentRelationSharedData->fileSysObjSharedData,
				&fsObjName,
				persistentTid,
				serialNum,
				/* expectedState1 */ PersistentFileSysState_CreatePending,
				/* expectedState2 */ PersistentFileSysState_Created,
				/* nextState */ PersistentFileSysState_DropPending,
				&actualState);

	GpPersistentRelationNode_SetDatumValues(
										values,
										relFileNode->spcNode,
										relFileNode->dbNode,
										relFileNode->relNode,
										/* segmentFileNum */ 0, 					// UNDONE
										PersistentFileSysRelStorageMgr_None,	// UNDONE
										PersistentFileSysState_DropPending,
										MirrorFileSysState_None,				// UNDONE
										/* mirrorAppendOnlyEof */ 0,				// UNDONE
										InvalidTransactionId,
										serialNum,
										&previousFreeTid);
		
	PersistentFileSysObj_UpdateTuple(
						&persistentRelationData.fileSysObjData,
						&persistentRelationSharedData->fileSysObjSharedData,
						persistentTid,
						values,
						/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from '%s' to 'Drop Pending', serial number " INT64_FORMAT " TID %s",
			 relpath(*relFileNode),
			 PersistentFileSysObj_StateName(actualState),
			 serialNum,
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

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				serialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	PersistentFileSysState	actualState;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_relation_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent relation '%s' because we are in bootstrap mode",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(&fsObjName,relFileNode);
	
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
	
	PersistentFileSysObj_VerifyExpected(
				&persistentRelationData.fileSysObjData,
				&persistentRelationSharedData->fileSysObjSharedData,
				&fsObjName,
				persistentTid,
				serialNum,
				/* expectedState1 */ PersistentFileSysState_CreatePending,
				/* expectedState2 */ (PersistentFileSysState)-1,
				/* nextState */ PersistentFileSysState_AbortingCreate,
				&actualState);

	GpPersistentRelationNode_SetDatumValues(
										values,
										relFileNode->spcNode,
										relFileNode->dbNode,
										relFileNode->relNode,
										/* segmentFileNum */ 0, 					// UNDONE
										PersistentFileSysRelStorageMgr_None,	// UNDONE
										PersistentFileSysState_AbortingCreate,
										InvalidTransactionId,
										MirrorFileSysState_None,				// UNDONE
										/* mirrorAppendOnlyEof */ 0,				// UNDONE
										serialNum,
										&previousFreeTid);
		
	PersistentFileSysObj_UpdateTuple(
						&persistentRelationData.fileSysObjData,
						&persistentRelationSharedData->fileSysObjSharedData,
						persistentTid,
						values,
						/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from 'Create Pending' to 'Aborting Create', serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 serialNum,
			 ItemPointerToString(persistentTid));
}

/*
 * Indicate we physically removed the relation file.
 */
void PersistentRelation_Dropped(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the dropped relation. */

	ItemPointer			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the relation. */

	int64				serialNum)
				/* Serial number for the relation.  Distinquishes the uses of the tuple. */

{
	PersistentFileSysObjName fsObjName;

	PersistentFileSysState actualState;

	ItemPointerData previousFreeTid;

	Datum freeValues[Natts_gp_persistent_relation_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
			     "Skipping persistent relation '%s' because we are in bootstrap mode",
				 relpath(*relFileNode));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentRelation_InitScan();

	PersistentFileSysObjName_SetRelationFile(&fsObjName,relFileNode);
	
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFileSysObj_VerifyExpected(
				&persistentRelationData.fileSysObjData,
				&persistentRelationSharedData->fileSysObjSharedData,
				&fsObjName,
				persistentTid,
				serialNum,
				/* expectedState1 */ PersistentFileSysState_DropPending,
				/* expectedState2 */ PersistentFileSysState_AbortingCreate,
				/* nextState */ PersistentFileSysState_Free,
				&actualState);

	GpPersistentRelationNode_SetDatumValues(
										freeValues,
										/* tablespaceOid */ 0,
										/* databaseOid */ 0,
										/* relFileNodeOid */ 0,
										/* segmentFileNum */ 0,
										PersistentFileSysRelStorageMgr_None,
										PersistentFileSysState_Free,
										MirrorFileSysState_None,
										/* mirrorAppendOnlyEof */ 0,
										InvalidTransactionId,
										/* persistentSerialNum */ 0,
										&previousFreeTid);
	PersistentFileSysObj_FreeTuple(
							&persistentRelationData.fileSysObjData,
							&persistentRelationSharedData->fileSysObjSharedData,
							persistentTid,
							freeValues,
							/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Persistent relation: '%s' changed state from '%s' to (Free), serial number " INT64_FORMAT " at TID %s",
			 relpath(*relFileNode),
			 PersistentFileSysObj_StateName(actualState),
			 serialNum,
			 ItemPointerToString(persistentTid));
}

// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

static Size PersistentRelation_SharedDataSize(void)
{
	return sizeof(PersistentRelationSharedData);
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
						PersistentFsObjType_RelationFile,
						/* scanTupleCallback */ NULL);

	Assert(persistentRelationSharedData != NULL);
}
