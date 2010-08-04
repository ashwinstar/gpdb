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

/* This value is the gp_max_tablespaces * gp_max_databases. */
int		MaxPersistentDatabaseDirectories = 0;

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

static void PersistentDatabase_InitScan(void)
{
	if (persistentDatabaseSharedData == NULL)
		elog(PANIC, "Persistent database information shared-memory not setup");

	PersistentFileSysObj_InitScan();
}

// -----------------------------------------------------------------------------
// Scan 
// -----------------------------------------------------------------------------

static bool PersistentDatabase_ScanTupleCallback(
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	DbDirNode		dbDirNode;
	
	PersistentFileSysState	state;

	int64			createMirrorDataLossTrackingSessionNum;

	MirroredObjectExistenceState		mirrorExistenceState;

	int32					reserved;
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	SharedOidSearchAddResult addResult;
	DatabaseDirEntry databaseDirEntry;

	GpPersistentDatabaseNode_GetValues(
									values,
									&dbDirNode.tablespace,
									&dbDirNode.database,
									&state,
									&createMirrorDataLossTrackingSessionNum,
									&mirrorExistenceState,
									&reserved,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Free)
	{
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
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
			 PersistentFileSysObjState_Name(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);
	
	databaseDirEntry->state = state;
	databaseDirEntry->persistentSerialNum = serialNum;
	databaseDirEntry->persistentTid = *persistentTid;

	databaseDirEntry->iteratorRefCount = 0;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
			 "PersistentDatabase_ScanTupleCallback: database %u, tablespace %u, state %s, TID %s, serial number " INT64_FORMAT,
			 dbDirNode.database,
			 dbDirNode.tablespace,
			 PersistentFileSysObjState_Name(state),
			 ItemPointerToString2(persistentTid),
			 persistentSerialNum);

	return true;	// Continue.
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

	// NOTE: Since we are not accessing data in the Buffer Pool, we don't need to
	// acquire the MirroredLock.

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

	PersistentFileSysState	*state,

	ItemPointer				persistentTid,

	int64					*persistentSerialNum)
{
	DatabaseDirEntry prevDatabaseDirEntry;

	Assert(persistentDatabaseSharedData != NULL);

	MemSet(dbDirNode, 0, sizeof(DbDirNode));

	// NOTE: Since we are not accessing data in the Buffer Pool, we don't need to
	// acquire the MirroredLock.

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

		*persistentTid = dirIterateDatabaseDirEntry->persistentTid;
		*persistentSerialNum = dirIterateDatabaseDirEntry->persistentSerialNum;

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


bool PersistentDatabase_DbDirExistsUnderLock(
	DbDirNode				*dbDirNode)
{
	DatabaseDirEntry databaseDirEntry;
	
	databaseDirEntry =
			(DatabaseDirEntry)
				    SharedOidSearch_Find(
				    		&persistentDatabaseSharedData->databaseDirSearchTable,
				    		dbDirNode->database,
				    		dbDirNode->tablespace);
	
	return (databaseDirEntry != NULL);
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

/*
 * We pass in changable columns like mirrorExistenceState, parentXid, etc instead
 * of keep them in our DatabaseDirEntry to avoid stale data.
 */
static void PersistentDatabase_AddTuple(
	DatabaseDirEntry databaseDirEntry,

	int64			createMirrorDataLossTrackingSessionNum,

	MirroredObjectExistenceState mirrorExistenceState,

	int32			reserved,

	TransactionId 	parentXid,

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
								createMirrorDataLossTrackingSessionNum,
								mirrorExistenceState,
								reserved,
								parentXid,
								/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
								&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							PersistentFsObjType_DatabaseDir,
							values,
							flushToXLog,
							&databaseDirEntry->persistentTid,
							&databaseDirEntry->persistentSerialNum);
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

	MirroredObjectExistenceState mirrorExistenceState,

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_database_node tuple for the rel file */
				
	int64			*persistentSerialNum,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	DatabaseDirEntry databaseDirEntry;
	SharedOidSearchAddResult addResult;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));
		/*
		 * The initdb process will load the persistent table once we 
		 * out of bootstrap mode.
		 */
		return;
	}

	PersistentDatabase_InitScan();

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

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
		     PersistentFileSysObjState_Name(databaseDirEntry->state));

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
		     PersistentFileSysObjState_Name(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);

	databaseDirEntry->state = PersistentFileSysState_CreatePending;

	databaseDirEntry->iteratorRefCount = 0;

	PersistentDatabase_AddTuple(
							databaseDirEntry,
							/* createMirrorDataLossTrackingSessionNum */ 0,
							mirrorExistenceState,
							/* reserved */ 0,
							/* parentXid */ GetTopTransactionId(),
							flushToXLog);

	*persistentTid = databaseDirEntry->persistentTid;
	*persistentSerialNum = databaseDirEntry->persistentSerialNum;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
}

void PersistentDatabase_AddCreated(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the create. */

	MirroredObjectExistenceState mirrorExistenceState,

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;
	SharedOidSearchAddResult addResult;

	int64 persistentSerialNum;

	Assert (Persistent_BeforePersistenceWork());

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

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
		     PersistentFileSysObjState_Name(databaseDirEntry->state));

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
		     PersistentFileSysObjState_Name(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);

	databaseDirEntry->state = PersistentFileSysState_Created;

	databaseDirEntry->iteratorRefCount = 0;

	PersistentDatabase_AddTuple(
							databaseDirEntry,
							/* createMirrorDataLossTrackingSessionNum */ 0,
							mirrorExistenceState,
							/* reserved */ 0,
							InvalidTransactionId,
							flushToXLog);

	*persistentTid = databaseDirEntry->persistentTid;
	persistentSerialNum = databaseDirEntry->persistentSerialNum;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: Add '%s' in state 'Created', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

void
xlog_create_database(DbDirNode *db)
{
	DatabaseDirEntry dbe;
	SharedOidSearchAddResult addResult;
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentDatabase_InitScan();

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	dbe = (DatabaseDirEntry) SharedOidSearch_Find(
				    	&persistentDatabaseSharedData->databaseDirSearchTable,
				    							  db->database,
				    							  db->tablespace);
	if (dbe != NULL)
		elog(ERROR, "persistent database entry '%s' already exists "
			 		"in state '%s'", 
			 GetDatabasePath(
				   db->database, 
				   db->tablespace),
		     PersistentFileSysObjState_Name(dbe->state));

	addResult = SharedOidSearch_Add(
		    		&persistentDatabaseSharedData->databaseDirSearchTable,
		    		db->database,
		    		db->tablespace,
		    		(SharedOidSearchObjHeader**)&dbe);

	if (addResult == SharedOidSearchAddResult_NoMemory)
		elog(ERROR, "out of shared-memory for persistent databases");
	else if (addResult == SharedOidSearchAddResult_Exists)
		elog(PANIC, "persistent database entry '%s' already exists in "
			 		"state '%s'", 
		     GetDatabasePath(
		     		db->database, 
		     		db->tablespace),
		     PersistentFileSysObjState_Name(dbe->state));
	else
		Insist(addResult == SharedOidSearchAddResult_Ok);

	dbe->state = PersistentFileSysState_Created;

	dbe->iteratorRefCount = 0;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
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

	int64			persistentSerialNum,
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

	bool			retryPossible)

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentDatabase_LookupExistingDbDir(
									dbDirNode,
									&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Create Pending' state (actual state '%s')", 
			 GetDatabasePath(
			 		dbDirNode->database, 
			 		dbDirNode->tablespace),
			 PersistentFileSysObjState_Name(databaseDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Created,
								retryPossible,
								/* flushToXlog */ false,
								NULL);

	databaseDirEntry->state = PersistentFileSysState_Created;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: '%s' changed state from 'Create Pending' to 'Created', serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

/*
 * Iterate over gp_persistent_database_node, removing all
 * references to dbid.
 */
void
PersistentDatabase_RemoveSegment(int16 dbid, bool ismirror)
{
	Relation rel;
	HeapScanDesc scandesc;
	HeapTuple tuple;
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	if (Persistent_BeforePersistenceWork())
		elog(ERROR, "persistent table changes forbidden");

	rel = heap_open(GpPersistentDatabaseNodeRelationId,
					AccessExclusiveLock);
	scandesc = heap_beginscan(rel, SnapshotNow, 0, NULL);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		Form_gp_persistent_database_node form =
			(Form_gp_persistent_database_node)GETSTRUCT(tuple);
		Oid tblspcoid = form->tablespace_oid;
		Oid dboid = form->database_oid;
		int64 serial = form->persistent_serial_num;

		PersistentFileSysObjName fsObjName;

		/* 
		 * We need to perform this table for all (database, tablespace) pairs
		 * that are defined in the persistent table.
		 */
		PersistentFileSysObjName_SetDatabaseDir(&fsObjName,
												tblspcoid,
												dboid);

	    PersistentFileSysObj_RemoveSegment(&fsObjName,
										   &tuple->t_self,
										   serial,
										   dbid,
										   ismirror,
										   /* flushToXlog */ false);
	}

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	heap_endscan(scandesc);
	heap_close(rel, NoLock);
}

void
PersistentDatabase_ActivateStandby(int16 oldmaster, int16 newmaster)
{
	Relation rel;
	HeapScanDesc scandesc;
	HeapTuple tuple;
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	if (Persistent_BeforePersistenceWork())
		elog(ERROR, "persistent table changes forbidden");

	rel = heap_open(GpPersistentDatabaseNodeRelationId,
					AccessExclusiveLock);
	scandesc = heap_beginscan(rel, SnapshotNow, 0, NULL);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		Form_gp_persistent_database_node form =
			(Form_gp_persistent_database_node)GETSTRUCT(tuple);
		Oid tblspcoid = form->tablespace_oid;
		Oid dboid = form->database_oid;
		int64 serial = form->persistent_serial_num;

		PersistentFileSysObjName fsObjName;

		/* 
		 * We need to perform this table for all (database, tablespace) pairs
		 * that are defined in the persistent table.
		 */
		PersistentFileSysObjName_SetDatabaseDir(&fsObjName,
												tblspcoid,
												dboid);

	    PersistentFileSysObj_ActivateStandby(&fsObjName,
											 &tuple->t_self,
											 serial,
											 oldmaster,
											 newmaster,
											 /* flushToXlog */ false);
	}

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	heap_endscan(scandesc);
	heap_close(rel, NoLock);
}

void
PersistentDatabase_AddMirrorAll(
	int16			pridbid,
	int16			mirdbid)
{
	Relation rel;
	HeapScanDesc scandesc;
	HeapTuple tuple;
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "skipping persistent database mirror addition");

		return;
	}

	rel = heap_open(GpPersistentDatabaseNodeRelationId,
					AccessExclusiveLock);
	scandesc = heap_beginscan(rel, SnapshotNow, 0, NULL);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		Form_gp_persistent_database_node form =
			(Form_gp_persistent_database_node)GETSTRUCT(tuple);
		Oid tblspcoid = form->tablespace_oid;
		Oid dboid = form->database_oid;
		int64 serial = form->persistent_serial_num;

		PersistentFileSysObjName fsObjName;

		PersistentFileSysObjName_SetDatabaseDir(&fsObjName,
												tblspcoid,
												dboid);

	    PersistentFileSysObj_AddMirror(&fsObjName,
									   &tuple->t_self,
									   serial,
									   pridbid,
           	        	               mirdbid,
									   NULL,
									   true,
                   	        	       /* flushToXlog */ false);
	}

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	heap_endscan(scandesc);
	heap_close(rel, NoLock);
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
bool PersistentDatabase_MarkDropPending(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the drop. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum,
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

	bool			retryPossible)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;

	bool result;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return false;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentDatabase_LookupExistingDbDir(
									dbDirNode,
									&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_CreatePending &&
		databaseDirEntry->state != PersistentFileSysState_Created)
		elog(ERROR, "Persistent database entry %s expected to be in 'Create Pending' or 'Created' state (actual state '%s')", 
			 GetDatabasePath(
				   dbDirNode->database, 
				   dbDirNode->tablespace),
			 PersistentFileSysObjState_Name(databaseDirEntry->state));

	result =
		PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_DropPending,
								retryPossible,
								/* flushToXlog */ false,
								NULL);

	databaseDirEntry->state = PersistentFileSysState_DropPending;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: '%s' changed state from 'Create Pending' to 'Aborting Create', serial number " INT64_FORMAT " at TID %s (deleteUnnecessary %s)",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid),
			 (result ? "true" : "false"));

	return result;
}

/*
 * Indicate we are aborting the create of a relation file.
 *
 * This state will make sure the relation gets dropped after a system crash.
 */
bool PersistentDatabase_MarkAbortingCreate(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the aborting create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum,
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

	bool			retryPossible)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;

	bool result;
	
	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return false;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Create Pending' (actual state '%s')", 
			 GetDatabasePath(
				   dbDirNode->database, 
				   dbDirNode->tablespace),
			 PersistentFileSysObjState_Name(databaseDirEntry->state));


	result =
		PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_AbortingCreate,
								retryPossible,
								/* flushToXlog */ false,
								NULL);

	databaseDirEntry->state = PersistentFileSysState_AbortingCreate;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: '%s' changed state from 'Create Pending' to 'Aborting Create', serial number " INT64_FORMAT " at TID %s (deleteUnnecessary %s)",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid),
			 (result ? "true" : "false"));

	return result;
}

/*
 * Indicate we physically removed the relation file.
 */
void PersistentDatabase_Dropped(
	DbDirNode 		*dbDirNode,
				/* The tablespace and database OIDs for the dropped relation. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum)
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;

	PersistentFileSysState oldState;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_DropPending &&
		databaseDirEntry->state != PersistentFileSysState_AbortingCreate)
		elog(ERROR, "Persistent database entry %s expected to be in 'Drop Pending' or 'Aborting Create' (actual state '%s')", 
			 GetDatabasePath(
				  dbDirNode->database, 
				  dbDirNode->tablespace),
			 PersistentFileSysObjState_Name(databaseDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Free,
								/* retryPossible */ false,
								/* flushToXlog */ false,
								&oldState);

	databaseDirEntry->state = PersistentFileSysState_Free;

	if (databaseDirEntry->iteratorRefCount == 0)
		SharedOidSearch_Delete(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					&databaseDirEntry->header);

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: '%s' changed state from '%s' to (Free), serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 PersistentFileSysObjState_Name(oldState),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

bool 
PersistentDatabase_DirIsCreated(DbDirNode *dbDirNode)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	DatabaseDirEntry databaseDirEntry;
	bool result;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before "
				 "persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));
		/* 
		 * The initdb process will load the persistent table once we out of
		 * bootstrap mode.
		 */
		return true;
	}

	PersistentDatabase_InitScan();

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	databaseDirEntry =
			(DatabaseDirEntry)
				    SharedOidSearch_Find(
				    		&persistentDatabaseSharedData->databaseDirSearchTable,
				    		dbDirNode->database,
				    		dbDirNode->tablespace);
	result = (databaseDirEntry != NULL);
	if (result &&
		databaseDirEntry->state != PersistentFileSysState_Created &&
		databaseDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Create Pending' or 'Created' (actual state '%s')", 
			 GetDatabasePath(
				  dbDirNode->database, 
				  dbDirNode->tablespace),
			 PersistentFileSysObjState_Name(databaseDirEntry->state));

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	return result;
}
		
void PersistentDatabase_MarkJustInTimeCreatePending(
	DbDirNode		*dbDirNode,

	MirroredObjectExistenceState 	mirrorExistenceState,

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_database_node tuple for the rel file */
				
	int64			*persistentSerialNum)
				
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;

	SharedOidSearchAddResult addResult;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

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
		     PersistentFileSysObjState_Name(databaseDirEntry->state));
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
			 PersistentFileSysObjState_Name(databaseDirEntry->state));
	else
		Assert(addResult == SharedOidSearchAddResult_Ok);

	databaseDirEntry->state = PersistentFileSysState_JustInTimeCreatePending;

	PersistentDatabase_AddTuple(
							databaseDirEntry,
							/* createMirrorDataLossTrackingSessionNum */ 0,
							mirrorExistenceState,
							/* reserved */ 0,
							/* parentXid */ InvalidTransactionId,
							/* flushToXLog */ true);


	*persistentTid = databaseDirEntry->persistentTid;
	*persistentSerialNum = databaseDirEntry->persistentSerialNum;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: Add '%s' in state 'Just-In-Time Create Pending', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 *persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

/*
 * Indicate the non-transaction just-in-time database create was successful.
 */
void PersistentDatabase_JustInTimeCreated(
	DbDirNode 		*dbDirNode,

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum)
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_JustInTimeCreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Just-In-Time Create Pending' state (actual state '%s')", 
			 GetDatabasePath(
			 		dbDirNode->database, 
			 		dbDirNode->tablespace),
			 PersistentFileSysObjState_Name(databaseDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Created,
								/* retryPossible */ false,
								/* flushToXlog */ false,
								NULL);

	databaseDirEntry->state = PersistentFileSysState_Created;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: '%s' changed state from 'Just-In-Time Create Pending' to 'Created', serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}

/*
 * Indicate the non-transaction just-in-time database create was NOT successful.
 */
void PersistentDatabase_AbandonJustInTimeCreatePending(
	DbDirNode 		*dbDirNode,

	ItemPointer 	persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum)
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	DatabaseDirEntry databaseDirEntry;

	if (Persistent_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "Skipping persistent database '%s' because we are before persistence work",
				 GetDatabasePath(
					  dbDirNode->database, 
					  dbDirNode->tablespace));

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentDatabase_InitScan();

	PersistentFileSysObjName_SetDatabaseDir(&fsObjName,dbDirNode->tablespace,dbDirNode->database);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentDatabase_LookupExistingDbDir(
								dbDirNode,
								&databaseDirEntry);

	if (databaseDirEntry->state != PersistentFileSysState_JustInTimeCreatePending)
		elog(ERROR, "Persistent database entry %s expected to be in 'Just-In-Time Create Pending' state (actual state '%s')", 
			 GetDatabasePath(
			 		dbDirNode->database, 
			 		dbDirNode->tablespace),
			 PersistentFileSysObjState_Name(databaseDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Free,
								/* retryPossible */ false,
								/* flushToXlog */ false,
								NULL);

	databaseDirEntry->state = PersistentFileSysState_Free;

	if (databaseDirEntry->iteratorRefCount == 0)
		SharedOidSearch_Delete(
					&persistentDatabaseSharedData->databaseDirSearchTable,
					&databaseDirEntry->header);

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "Persistent database directory: Abandon '%s' in state 'Just-In-Time Create Pending', serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_ObjectName(&fsObjName),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid));
}


// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

static Size PersistentDatabase_SharedDataSize(void)
{
	return MAXALIGN(
				offsetof(PersistentDatabaseSharedData,databaseDirSearchTable)) +
		   SharedOidSearch_TableLen(
					/* hashSize */ 127,
					/* freeObjectCount */ MaxPersistentDatabaseDirectories,
					/* objectLen */ sizeof(DatabaseDirEntryData));
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
						&persistentDatabaseSharedData->fileSysObjSharedData,
						PersistentFsObjType_DatabaseDir,
						PersistentDatabase_ScanTupleCallback);


	Assert(persistentDatabaseSharedData != NULL);
}
