/*-------------------------------------------------------------------------
 *
 * cdbpersistentdatabase.c
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
#include "cdb/cdbpersistenttablespace.h"
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

int		MaxPersistentDatabaseDirectories = 0;
						// This value is the gp_max_tablespaces * gp_max_databases.

typedef struct PersistentDatabaseSharedData
{
	
	PersistentFileSysObjSharedData		fileSysObjSharedData;
	
	SharedOidSearchTable	databaseDirSearchTable;
							/* Variable length -- MUST BE LAST */

} PersistentDatabaseSharedData;

#define PersistentDatabaseData_StaticInit {PersistentFileSysObjData_StaticInit}

typedef struct PersistentDatabaseData
{

	PersistentFileSysObjData		fileSysObjData;

} PersistentDatabaseData;

/*
 * Global Variables
 */
PersistentDatabaseSharedData	*persistentDatabaseSharedData = NULL;

PersistentDatabaseData	persistentDatabaseData = PersistentDatabaseData_StaticInit;

static void PersistentDatabase_InitScan(void);

// -----------------------------------------------------------------------------
// Scan 
// -----------------------------------------------------------------------------

static bool PersistentDatabase_ScanTupleCallback(
	int 					token,
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	DbDirNode		dbDirNode;
	
	PersistentFileSysState	state;
	MirrorFileSysState		mirrorState;	// UNDONE
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	SharedOidSearchAddResult addResult;
	DatabaseDirEntry databaseDirEntry;

	Assert(token == PersistentFsObjType_DatabaseDir);

	GpPersistentDatabaseNode_GetValues(
									values,
									&dbDirNode.tablespace,
									&dbDirNode.database,
									&state,
									&mirrorState,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Free)
	{
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "PersistentDatabase_ScanTupleCallback: TID %s, serial number " INT64_FORMAT " is free",
				 ItemPointerToString2(persistentTid),
				 persistentSerialNum);
		return true;	// Continue.
	}
	
	addResult =
			SharedOidSearch_Add(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					dbDirNode.database,
					dbDirNode.tablespace,
					(SharedOidSearchObjHeader**)&databaseDirEntry);
	if (addResult == SharedOidSearchAddResult_NoMemory)
		elog(ERROR, "Out of shared-memory for persistent relations");
	else if (addResult == SharedOidSearchAddResult_Exists)
		elog(PANIC, "Persistent database entry '%s' already exists in state '%s'", 
			 GetDatabasePath(dbDirNode.database, dbDirNode.tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);
	
	databaseDirEntry->state = state;
	databaseDirEntry->serialNum = serialNum;
	databaseDirEntry->parentXid = parentXid;
	databaseDirEntry->presistentTid = *persistentTid;

	databaseDirEntry->iteratorRefCount = 0;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentDatabase_ScanTupleCallback: database %u, tablespace %u, state %s, TID %s, serial number " INT64_FORMAT,
			 dbDirNode.database,
			 dbDirNode.tablespace,
			 PersistentFileSysObj_StateName(state),
			 ItemPointerToString2(persistentTid),
			 persistentSerialNum);

	return true;	// Continue.
}

void PersistentDatabase_InitScanUnderLock(void)
{
	if (persistentDatabaseSharedData == NULL)
		elog(PANIC, "Persistent database information shared-memory not setup");

	PersistentFileSysObj_InitScanUnderLock(
					&persistentDatabaseData.fileSysObjData,
					&persistentDatabaseSharedData->fileSysObjSharedData);
}

static void PersistentDatabase_InitScan(void)
{
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentDatabase_InitScanUnderLock();

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// Iterate	
// -----------------------------------------------------------------------------

static bool iterateOnShmemExitArmed = false;

static DatabaseDirEntry dirIterateDatabaseDirEntry = NULL;

static void PersistentDatabase_DirIterateMoveAway(
	DatabaseDirEntry prevDatabaseDirEntry)
{
	if (prevDatabaseDirEntry == NULL)
		return;

	Assert(prevDatabaseDirEntry->iteratorRefCount > 0);
	prevDatabaseDirEntry->iteratorRefCount--;
	if (prevDatabaseDirEntry->iteratorRefCount == 0 &&
		prevDatabaseDirEntry->state == PersistentFileSysState_Free)
	{
		/*
		 * Our job is to actually free the entry.
		 */
		SharedOidSearch_Delete(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					&prevDatabaseDirEntry->header);
	}
}

static void PersistentDatabase_ReleaseDirIterator(void)
{

	if (dirIterateDatabaseDirEntry != NULL)
	{
		SharedOidSearch_ReleaseIterator(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					(SharedOidSearchObjHeader**)&dirIterateDatabaseDirEntry);
		Assert(dirIterateDatabaseDirEntry == NULL);
	}
	
	PersistentDatabase_DirIterateMoveAway(dirIterateDatabaseDirEntry);
}

static void AtProcExit_PersistentDatabase(int code, Datum arg)
{
	PersistentDatabase_ReleaseDirIterator();
}

void PersistentDatabase_DirIterateInit(void)
{
	Assert(persistentDatabaseSharedData != NULL);
	
	PersistentDatabase_InitScan();

	PersistentDatabase_ReleaseDirIterator();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	if (!iterateOnShmemExitArmed)
	{
		on_shmem_exit(AtProcExit_PersistentDatabase, 0);
		iterateOnShmemExitArmed = true;
	}

	dirIterateDatabaseDirEntry = NULL;
	
	LWLockRelease(PersistentObjLock);
}


bool PersistentDatabase_DirIterateNext(
	DbDirNode				*dbDirNode,
	PersistentFileSysState	*state)
{
	DatabaseDirEntry prevDatabaseDirEntry;

	Assert(persistentDatabaseSharedData != NULL);

	MemSet(dbDirNode, 0, sizeof(DbDirNode));

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	while (true)
	{
		prevDatabaseDirEntry = dirIterateDatabaseDirEntry;
		SharedOidSearch_Iterate(
						&persistentDatabaseSharedData->databaseDirSearchTable,
						(SharedOidSearchObjHeader**)&dirIterateDatabaseDirEntry);

		PersistentDatabase_DirIterateMoveAway(prevDatabaseDirEntry);

		if (dirIterateDatabaseDirEntry == NULL)
		{
			LWLockRelease(PersistentObjLock);
			return false;
		}

		*state = dirIterateDatabaseDirEntry->state;
		if (*state == PersistentFileSysState_Free)
		{
			// UNDONE: Or, PinCount > 1
			Assert(dirIterateDatabaseDirEntry->iteratorRefCount > 0);
			continue;
		}
		
		dbDirNode->tablespace = dirIterateDatabaseDirEntry->header.oid2;
		dbDirNode->database = dirIterateDatabaseDirEntry->header.oid1;

		dirIterateDatabaseDirEntry->iteratorRefCount++;
		break;
	}

	LWLockRelease(PersistentObjLock);

	return true;
}

void PersistentDatabase_DirIterateClose(void)
{
	Assert(persistentDatabaseSharedData != NULL);
	PersistentDatabase_ReleaseDirIterator();
}



// -----------------------------------------------------------------------------
// Helpers 
// -----------------------------------------------------------------------------

static void PersistentDatabase_LookupExistingDbDir(
	DbDirNode				*dbDirNode,

	DatabaseDirEntry 	    *databaseDirEntry)
{
	*databaseDirEntry =
			(DatabaseDirEntry)
				    SharedOidSearch_Find(
				    		&persistentDatabaseSharedData->databaseDirSearchTable,
				    		dbDirNode->database,
				    		dbDirNode->tablespace);
	if (*databaseDirEntry == NULL)
		elog(ERROR, "Persistent database entry '%s' expected to exist", 
			 GetDatabasePath(dbDirNode->database, dbDirNode->tablespace));
}

static void PersistentDatabase_AddTuple(
	DatabaseDirEntry databaseDirEntry,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Oid tablespaceOid = databaseDirEntry->header.oid2;
	Oid databaseOid = databaseDirEntry->header.oid1;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_database_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	GpPersistentDatabaseNode_SetDatumValues(
								values,
								tablespaceOid,
								databaseOid,
								databaseDirEntry->state,
								MirrorFileSysState_None,		// UNDONE
								databaseDirEntry->parentXid,
								/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
								&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							&persistentDatabaseData.fileSysObjData,
							&persistentDatabaseSharedData->fileSysObjSharedData,
							values,
							flushToXLog,
							&databaseDirEntry->presistentTid,
							&databaseDirEntry->serialNum);
}

static void PersistentDatabase_UpdateTuple(
	DatabaseDirEntry databaseDirEntry,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Oid tablespaceOid = databaseDirEntry->header.oid2;
	Oid databaseOid = databaseDirEntry->header.oid1;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_database_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	GpPersistentDatabaseNode_SetDatumValues(
										values,
										tablespaceOid,
										databaseOid,
										databaseDirEntry->state,
										MirrorFileSysState_None,		// UNDONE
										databaseDirEntry->parentXid,
										databaseDirEntry->serialNum,
										&previousFreeTid);

	PersistentFileSysObj_UpdateTuple(
							&persistentDatabaseData.fileSysObjData,
							&persistentDatabaseSharedData->fileSysObjSharedData,
							&databaseDirEntry->presistentTid,
							values,
							flushToXLog);
}

Datum
gp_persistent_database_node_add(PG_FUNCTION_ARGS)
{
	Oid				tablespace = PG_GETARG_OID(0);
	Oid				database = PG_GETARG_OID(1);
	int16			state = PG_GETARG_INT16(2);

	DatabaseDirEntryData databaseDirEntry;

	PersistentDatabase_InitScan();

	MemSet(&databaseDirEntry, 0, sizeof(DatabaseDirEntry));
	
	databaseDirEntry.header.oid1 = database;
	databaseDirEntry.header.oid2 = tablespace;

	databaseDirEntry.state = state;

	PersistentDatabase_AddTuple(
							&databaseDirEntry,
							/* flushToXLog */ false);

	PG_RETURN_INT32(databaseDirEntry.serialNum);
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
void PersistentDatabase_MarkCreatePending(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	DatabaseDirEntry databaseDirEntry;
	SharedOidSearchAddResult addResult;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	databaseDirEntry =
			(DatabaseDirEntry)
				    SharedOidSearch_Find(
				    		&persistentDatabaseSharedData->databaseDirSearchTable,
				    		dbDirNode->database,
				    		dbDirNode->tablespace);
	if (databaseDirEntry != NULL)
		elog(ERROR, "Persistent database entry '%s' already exists in state '%s'", 
			 GetDatabasePath(
				   dbDirNode->database, 
				   dbDirNode->tablespace),
		     PersistentFileSysObj_StateName(databaseDirEntry->state));

	addResult =
		    SharedOidSearch_Add(
		    		&persistentDatabaseSharedData->databaseDirSearchTable,
		    		dbDirNode->database,
		    		dbDirNode->tablespace,
		    		(SharedOidSearchObjHeader**)&databaseDirEntry);
	if (addResult == SharedOidSearchAddResult_NoMemory)
		elog(ERROR, "Out of shared-memory for persistent databaseS");
	else if (addResult == SharedOidSearchAddResult_Exists)
		elog(PANIC, "Persistent database entry '%s' already exists in state '%s'", 
		     GetDatabasePath(
		     		dbDirNode->database, 
		     		dbDirNode->tablespace),
		     PersistentFileSysObj_StateName(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);

	databaseDirEntry->state = PersistentFileSysState_CreatePending;
	databaseDirEntry->parentXid = GetTopTransactionId();

	databaseDirEntry->iteratorRefCount = 0;

	PersistentDatabase_AddTuple(
							databaseDirEntry,
							flushToXLog);

	*persistentTid = databaseDirEntry->presistentTid;

	LWLockRelease(PersistentObjLock);
}

void PersistentDatabase_AddCreated(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	DatabaseDirEntry databaseDirEntry;
	SharedOidSearchAddResult addResult;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	databaseDirEntry =
			(DatabaseDirEntry)
				    SharedOidSearch_Find(
				    		&persistentDatabaseSharedData->databaseDirSearchTable,
				    		dbDirNode->database,
				    		dbDirNode->tablespace);
	if (databaseDirEntry != NULL)
		elog(ERROR, "Persistent database entry '%s' already exists in state '%s'", 
			 GetDatabasePath(
				   dbDirNode->database, 
				   dbDirNode->tablespace),
		     PersistentFileSysObj_StateName(databaseDirEntry->state));

	addResult =
		    SharedOidSearch_Add(
		    		&persistentDatabaseSharedData->databaseDirSearchTable,
		    		dbDirNode->database,
		    		dbDirNode->tablespace,
		    		(SharedOidSearchObjHeader**)&databaseDirEntry);
	if (addResult == SharedOidSearchAddResult_NoMemory)
		elog(ERROR, "Out of shared-memory for persistent databaseS");
	else if (addResult == SharedOidSearchAddResult_Exists)
		elog(PANIC, "Persistent database entry '%s' already exists in state '%s'", 
		     GetDatabasePath(
		     		dbDirNode->database, 
		     		dbDirNode->tablespace),
		     PersistentFileSysObj_StateName(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);

	databaseDirEntry->state = PersistentFileSysState_Created;
	databaseDirEntry->parentXid = GetTopTransactionId();

	databaseDirEntry->iteratorRefCount = 0;

	PersistentDatabase_AddTuple(
							databaseDirEntry,
							flushToXLog);

	*persistentTid = databaseDirEntry->presistentTid;

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// Transaction End  
// -----------------------------------------------------------------------------

/*
 * Indicate the transaction commited and the relation is officially created.
 */
void PersistentDatabase_Created(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the created relation. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

{
	DatabaseDirEntry databaseDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentDatabase_LookupExistingDbDir(
									dbDirNode,
									&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Create Pending' state (actual state '%s')", 
			 GetDatabasePath(
			 		dbDirNode->database, 
			 		dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));

	databaseDirEntry->state = PersistentFileSysState_Created;

	PersistentDatabase_UpdateTuple(
								databaseDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
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
void PersistentDatabase_MarkDropPending(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the drop. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

{
	DatabaseDirEntry databaseDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentDatabase_LookupExistingDbDir(
									dbDirNode,
									&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_CreatePending &&
		databaseDirEntry->state != PersistentFileSysState_Created)
		elog(ERROR, "Persistent database entry %s expected to be in 'Create Pending' or 'Created' state (actual state '%s')", 
			 GetDatabasePath(
				   dbDirNode->database, 
				   dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));

	databaseDirEntry->state = PersistentFileSysState_DropPending;

	PersistentDatabase_UpdateTuple(
								databaseDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we are aborting the create of a relation file.
 *
 * This state will make sure the relation gets dropped after a system crash.
 */
void PersistentDatabase_MarkAbortingCreate(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the aborting create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

{
	DatabaseDirEntry databaseDirEntry;
	
	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Create Pending' (actual state '%s')", 
			 GetDatabasePath(
				   dbDirNode->database, 
				   dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));

	databaseDirEntry->state = PersistentFileSysState_AbortingCreate;

	PersistentDatabase_UpdateTuple(
								databaseDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we physically removed the relation file.
 */
void PersistentDatabase_Dropped(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the dropped relation. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

{
	DatabaseDirEntry databaseDirEntry;

	ItemPointerData previousFreeTid;

	Datum freeValues[Natts_gp_persistent_database_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_DropPending &&
		databaseDirEntry->state != PersistentFileSysState_AbortingCreate)
		elog(ERROR, "Persistent database entry %s expected to be in 'Drop Pending' or 'Aborting Create' (actual state '%s')", 
			 GetDatabasePath(
				  dbDirNode->database, 
				  dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));

	databaseDirEntry->state = PersistentFileSysState_Free;

	GpPersistentDatabaseNode_SetDatumValues(
								freeValues,
								/* tablespaceOid */ 0,
								/* databaseOid */ 0,
								PersistentFileSysState_Free,
								MirrorFileSysState_None,
								InvalidTransactionId,
								/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
								&previousFreeTid);


	PersistentFileSysObj_FreeTuple(
							&persistentDatabaseData.fileSysObjData,
							&persistentDatabaseSharedData->fileSysObjSharedData,
							&databaseDirEntry->presistentTid,
							freeValues,
							/* flushToXLog */ false);

	if (databaseDirEntry->iteratorRefCount == 0)
		SharedOidSearch_Delete(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					&databaseDirEntry->header);

	LWLockRelease(PersistentObjLock);
}

bool PersistentDatabase_DirIsCreated(
		DbDirNode		*dbDirNode)
{
	DatabaseDirEntry databaseDirEntry;
	bool result;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return true;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	databaseDirEntry =
			(DatabaseDirEntry)
				    SharedOidSearch_Find(
				    		&persistentDatabaseSharedData->databaseDirSearchTable,
				    		dbDirNode->database,
				    		dbDirNode->tablespace);
	result = (databaseDirEntry != NULL);
	if (result &&
		databaseDirEntry->state != PersistentFileSysState_Created)
		elog(ERROR, "Persistent database entry %s expected to be in 'Created' (actual state '%s')", 
			 GetDatabasePath(
				  dbDirNode->database, 
				  dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));

	LWLockRelease(PersistentObjLock);

	return result;
}
		
void PersistentDatabase_MarkJustInTimeCreatePending(
	DbDirNode		*dbDirNode)

{
	DatabaseDirEntry databaseDirEntry;

	SharedOidSearchAddResult addResult;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	databaseDirEntry =
			(DatabaseDirEntry)
				    SharedOidSearch_Find(
				    		&persistentDatabaseSharedData->databaseDirSearchTable,
				    		dbDirNode->database,
				    		dbDirNode->tablespace);
	if (databaseDirEntry != NULL)
	{
		/*
		 * An existence check should have been done before calling this routine.
		 */
		elog(ERROR, "Persistent database entry '%s' already exists in state '%s'", 
			 GetDatabasePath(
				   dbDirNode->database, 
				   dbDirNode->tablespace),
		     PersistentFileSysObj_StateName(databaseDirEntry->state));
	}

	addResult =
			SharedOidSearch_Add(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					dbDirNode->database,
					dbDirNode->tablespace,
					(SharedOidSearchObjHeader**)&databaseDirEntry);
	if (addResult == SharedOidSearchAddResult_NoMemory)
		elog(ERROR, "Out of shared-memory for persistent databases");
	else if (addResult == SharedOidSearchAddResult_Exists)
		elog(PANIC, "Persistent database entry '%s' already exists in state '%s'", 
			 GetDatabasePath(
					dbDirNode->database, 
					dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);

	databaseDirEntry->state = PersistentFileSysState_JustInTimeCreatePending;
	databaseDirEntry->parentXid = InvalidTransactionId;

	PersistentDatabase_AddTuple(
							databaseDirEntry,
							/* flushToXLog */ true);

	LWLockRelease(PersistentObjLock);

}

/*
 * Indicate the non-transaction just-in-time database create was successful.
 */
void PersistentDatabase_JustInTimeCreated(
	DbDirNode 		*dbDirNode)

{
	DatabaseDirEntry databaseDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_JustInTimeCreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Just-In-Time Create Pending' state (actual state '%s')", 
			 GetDatabasePath(
			 		dbDirNode->database, 
			 		dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));

	databaseDirEntry->state = PersistentFileSysState_Created;

	PersistentDatabase_UpdateTuple(
								databaseDirEntry,
								/* flushToXLog */ true);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate the non-transaction just-in-time database create was NOT successful.
 */
void PersistentDatabase_AbandonJustInTimeCreatePending(
	DbDirNode 		*dbDirNode)

{
	DatabaseDirEntry databaseDirEntry;

	ItemPointerData previousFreeTid;

	Datum freeValues[Natts_gp_persistent_database_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent database '%s' because we are in bootstrap mode",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_JustInTimeCreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Just-In-Time Create Pending' state (actual state '%s')", 
			 GetDatabasePath(
			 		dbDirNode->database, 
			 		dbDirNode->tablespace),
			 PersistentFileSysObj_StateName(databaseDirEntry->state));

	databaseDirEntry->state = PersistentFileSysState_Free;

	GpPersistentDatabaseNode_SetDatumValues(
								freeValues,
								/* tablespaceOid */ 0,
								/* databaseOid */ 0,
								PersistentFileSysState_Free,
								MirrorFileSysState_None,
								InvalidTransactionId,
								/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
								&previousFreeTid);

	PersistentFileSysObj_FreeTuple(
							&persistentDatabaseData.fileSysObjData,
							&persistentDatabaseSharedData->fileSysObjSharedData,
							&databaseDirEntry->presistentTid,
							freeValues,
							/* flushToXLog */ false);

	if (databaseDirEntry->iteratorRefCount == 0)
		SharedOidSearch_Delete(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					&databaseDirEntry->header);

	LWLockRelease(PersistentObjLock);
}


// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

static Size PersistentDatabase_SharedDataSize(void)
{
	return MAXALIGN(
				offsetof(PersistentDatabaseSharedData,databaseDirSearchTable) +
			    SharedOidSearch_TableLen(
					/* hashSize */ 127,
					/* freeObjectCount */ MaxPersistentDatabaseDirectories,
					/* objectLen */ sizeof(DatabaseDirEntryData)));
}

/*
 * Return the required shared-memory size for this module.
 */
Size PersistentDatabase_ShmemSize(void)
{
	if (MaxPersistentDatabaseDirectories == 0)
		MaxPersistentDatabaseDirectories = gp_max_databases * gp_max_tablespaces;

	/* The shared-memory structure. */
	return PersistentDatabase_SharedDataSize();
}

/*
 * Initialize the shared-memory for this module.
 */
void PersistentDatabase_ShmemInit(void)
{
	bool found;

	/* Create the shared-memory structure. */
	persistentDatabaseSharedData = 
		(PersistentDatabaseSharedData *)
						ShmemInitStruct("Persistent Database Data",
										PersistentDatabase_SharedDataSize(),
										&found);

	if (!found)
	{
		PersistentFileSysObj_InitShared(
						&persistentDatabaseSharedData->fileSysObjSharedData);

		SharedOidSearch_InitTable(
						&persistentDatabaseSharedData->databaseDirSearchTable,
						127,
						MaxPersistentDatabaseDirectories,
						sizeof(DatabaseDirEntryData));
	}

	PersistentFileSysObj_Init(
						&persistentDatabaseData.fileSysObjData,
						PersistentFsObjType_DatabaseDir,
						PersistentDatabase_ScanTupleCallback);


	Assert(persistentDatabaseSharedData != NULL);
}
