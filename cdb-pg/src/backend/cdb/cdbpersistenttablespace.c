/*-------------------------------------------------------------------------
 *
 * cdbpersistenttablespace.c
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
 * GUC variables
 */
int		MaxPersistentTablespaces = 1024;		/* Max # of persistent tablespaces */
						// UNDONE: Create GUC have default value there....

typedef struct PersistentTablespaceSharedData
{
	
	PersistentFileSysObjSharedData		fileSysObjSharedData;

} PersistentTablespaceSharedData;

#define PersistentTablespaceData_StaticInit {PersistentFileSysObjData_StaticInit}

typedef struct PersistentTablespaceData
{

	PersistentFileSysObjData		fileSysObjData;

} PersistentTablespaceData;

typedef struct TablespaceDirEntryData
{
	Oid	tablespace;

	PersistentFileSysState	state;
	int64					serialNum;
	TransactionId			parentXid;
	ItemPointerData 		presistentTid;
	
} TablespaceDirEntryData;
typedef TablespaceDirEntryData *TablespaceDirEntry;


/*
 * Global Variables
 */
PersistentTablespaceSharedData	*persistentTablespaceSharedData = NULL;
static HTAB *persistentTablespaceSharedHashTable = NULL;

PersistentTablespaceData	persistentTablespaceData = PersistentTablespaceData_StaticInit;

static void PersistentTablespace_InitScan(void);

/*
 * Return the hash entry for a tablespace.
 */
static TablespaceDirEntry
PersistentTablespace_FindDirUnderLock(
	Oid 		tablespaceOid)
{
	bool			found;

	TablespaceDirEntry	tablespaceDirEntry;

	tablespaceDirEntry = 
			(TablespaceDirEntry) 
					hash_search(persistentTablespaceSharedHashTable,
								(void *) &tablespaceOid,
								HASH_FIND,
								&found);

	return tablespaceDirEntry;
}

static TablespaceDirEntry
PersistentTablespace_CreateDirUnderLock(
	Oid 		tablespaceOid)
{
	bool			found;

	TablespaceDirEntry	tablespaceDirEntry;

	tablespaceDirEntry = 
			(TablespaceDirEntry) 
					hash_search(persistentTablespaceSharedHashTable,
								(void *) &tablespaceOid,
								HASH_ENTER_NULL,
								&found);

	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Out of shared-memory for persistent tablespaces");
	
	tablespaceDirEntry->state = 0;
	tablespaceDirEntry->serialNum = 0;
	tablespaceDirEntry->parentXid = InvalidTransactionId;
	MemSet(&tablespaceDirEntry->presistentTid, 0, sizeof(ItemPointerData));
	
	return tablespaceDirEntry;
}

static void
PersistentTablespace_RemoveDirUnderLock(
	TablespaceDirEntry	tablespaceDirEntry)
{
	TablespaceDirEntry	removeTablespaceDirEntry;

	removeTablespaceDirEntry = 
				(TablespaceDirEntry) 
						hash_search(persistentTablespaceSharedHashTable,
									(void *) &tablespaceDirEntry->tablespace,
									HASH_REMOVE,
									NULL);

	if (removeTablespaceDirEntry == NULL)
		elog(ERROR, "Trying to delete entry that does not exist");
}


// -----------------------------------------------------------------------------
// Scan 
// -----------------------------------------------------------------------------

static bool PersistentTablespace_ScanTupleCallback(
	int 					token,
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	Oid		tablespace;
	
	PersistentFileSysState	state;
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	TablespaceDirEntry tablespaceDirEntry;

	Assert(token == PersistentFsObjType_TablespaceDir);

	GpPersistentTablespaceNode_GetValues(
									values,
									&tablespace,
									&state,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Free)
		return true;	// Continue.
	
	tablespaceDirEntry = 
			PersistentTablespace_CreateDirUnderLock(tablespace);
	
	tablespaceDirEntry->state = state;
	tablespaceDirEntry->serialNum = serialNum;
	tablespaceDirEntry->parentXid = parentXid;
	tablespaceDirEntry->presistentTid = *persistentTid;

	return true;	// Continue.
}

static void PersistentTablespace_InitScanUnderLock(void)
{
	if (persistentTablespaceSharedData == NULL)
		elog(PANIC, "Persistent tablespace information shared-memory not setup");

	PersistentFileSysObj_InitScanUnderLock(
					&persistentTablespaceData.fileSysObjData,
					&persistentTablespaceSharedData->fileSysObjSharedData);
}

static void PersistentTablespace_InitScan(void)
{
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentTablespace_InitScanUnderLock();

	LWLockRelease(PersistentObjLock);
}

//------------------------------------------------------------------------------

static Oid persistentTablespaceCheck;
static bool persistentTablespaceCheckFound;

static bool PersistentTablespace_CheckScanTupleCallback(
	int 					token,
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	Oid		tablespace;
	
	PersistentFileSysState	state;
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	Assert(token == PersistentFsObjType_TablespaceDir);

	GpPersistentTablespaceNode_GetValues(
									values,
									&tablespace,
									&state,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Created &&
		tablespace == persistentTablespaceCheck)
	{
		persistentTablespaceCheckFound = true;
		return false;
	}

	return true;	// Continue.
}

bool PersistentTablespace_Check(
	Oid				tablespace)
{
	persistentTablespaceCheck = tablespace;
	persistentTablespaceCheckFound = false;

	PersistentFileSysObj_Scan(
		&persistentTablespaceData.fileSysObjData,
		&persistentTablespaceSharedData->fileSysObjSharedData,
		PersistentTablespace_CheckScanTupleCallback);

	return persistentTablespaceCheckFound;
}

// -----------------------------------------------------------------------------
// Helpers 
// -----------------------------------------------------------------------------

static void PersistentTablespace_AddTuple(
	TablespaceDirEntry tablespaceDirEntry,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Oid tablespace = tablespaceDirEntry->tablespace;

	PersistentFileSysObjName fsObjName;

	PersistentFileSysObjName_SetTablespaceDir(&fsObjName,tablespace);

	PersistentFileSysObj_AddTuple(
							&persistentTablespaceData.fileSysObjData,
							&persistentTablespaceSharedData->fileSysObjSharedData,
							&fsObjName,
							tablespaceDirEntry->state,
							tablespaceDirEntry->parentXid,
							flushToXLog,
							&tablespaceDirEntry->presistentTid,
							&tablespaceDirEntry->serialNum);
}

static void PersistentTablespace_UpdateTuple(
	TablespaceDirEntry tablespaceDirEntry,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Oid tablespace = tablespaceDirEntry->tablespace;

	PersistentFileSysObjName fsObjName;
	
	PersistentFileSysObjName_SetTablespaceDir(&fsObjName,tablespace);

	PersistentFileSysObj_UpdateTuple(
							&persistentTablespaceData.fileSysObjData,
							&persistentTablespaceSharedData->fileSysObjSharedData,
							&tablespaceDirEntry->presistentTid,
							&fsObjName,
							tablespaceDirEntry->state,
							tablespaceDirEntry->parentXid,
							tablespaceDirEntry->serialNum,
							flushToXLog);
}

Datum
gp_persistent_tablespace_node_add(PG_FUNCTION_ARGS)
{
	Oid				tablespace = PG_GETARG_OID(0);
	int16			state = PG_GETARG_INT16(1);

	TablespaceDirEntryData tablespaceDirEntry;

	MemSet(&tablespaceDirEntry, 0, sizeof(TablespaceDirEntry));
	
	tablespaceDirEntry.tablespace = tablespace;

	tablespaceDirEntry.state = state;

	PersistentTablespace_AddTuple(
							&tablespaceDirEntry,
							/* flushToXLog */ false);

	PG_RETURN_INT32(tablespaceDirEntry.serialNum);
}

// -----------------------------------------------------------------------------
// State Change 
// -----------------------------------------------------------------------------

/*
 * Indicate we intend to create a tablespace file as part of the current transaction.
 *
 * An XLOG IntentToCreate record is generated that will guard the subsequent file-system
 * create in case the transaction aborts.
 *
 * After 1 or more calls to this routine to mark intention about tablespace files that are going
 * to be created, call ~_DoPendingCreates to do the actual file-system creates.  (See its
 * note on XLOG flushing).
 */
void PersistentTablespace_MarkCreatePending(
	Oid 		tablespace,
				/* The tablespace OID for the create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	TablespaceDirEntry tablespaceDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are in bootstrap mode",
				 tablespace);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	tablespaceDirEntry = PersistentTablespace_CreateDirUnderLock(tablespace);
	Assert(tablespaceDirEntry != NULL);

	tablespaceDirEntry->state = PersistentFileSysState_CreatePending;
	tablespaceDirEntry->parentXid = GetTopTransactionId();

	PersistentTablespace_AddTuple(
							tablespaceDirEntry,
							flushToXLog);

	*persistentTid = tablespaceDirEntry->presistentTid;

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// Transaction End  
// -----------------------------------------------------------------------------

/*
 * Indicate the transaction commited and the tablespace is officially created.
 */
void PersistentTablespace_Created(
	Oid 		tablespace,
				/* The tablespace OID for the created tablespace. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	TablespaceDirEntry tablespaceDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are in bootstrap mode",
				 tablespace);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	tablespaceDirEntry = PersistentTablespace_FindDirUnderLock(tablespace);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespace);

	if (tablespaceDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Create Pending' state (actual state '%s')", 
			 tablespace,
			 PersistentFileSysObj_StateName(tablespaceDirEntry->state));

	tablespaceDirEntry->state = PersistentFileSysState_Created;

	PersistentTablespace_UpdateTuple(
								tablespaceDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we intend to drop a tablespace file as part of the current transaction.
 *
 * This tablespace file to drop will be listed inside a commit, distributed commit, a distributed 
 * prepared, and distributed commit prepared XOG records.
 *
 * For any of the commit type records, once that XLOG record is flushed then the actual
 * file-system delete will occur.  The flush guarantees the action will be retried after system
 * crash.
 */
void PersistentTablespace_MarkDropPending(
	Oid 		tablespace,
				/* The tablespace OID for the drop. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	TablespaceDirEntry tablespaceDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are in bootstrap mode",
				 tablespace);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	tablespaceDirEntry = PersistentTablespace_FindDirUnderLock(tablespace);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespace);

	if (tablespaceDirEntry->state != PersistentFileSysState_CreatePending &&
		tablespaceDirEntry->state != PersistentFileSysState_Created)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Create Pending' or 'Created' state (actual state '%s')", 
			 tablespace,
			 PersistentFileSysObj_StateName(tablespaceDirEntry->state));

	tablespaceDirEntry->state = PersistentFileSysState_DropPending;

	PersistentTablespace_UpdateTuple(
								tablespaceDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we are aborting the create of a tablespace file.
 *
 * This state will make sure the tablespace gets dropped after a system crash.
 */
void PersistentTablespace_MarkAbortingCreate(
	Oid 		tablespace,
				/* The tablespace OID for the aborting create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	TablespaceDirEntry tablespaceDirEntry;
	
	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are in bootstrap mode",
				 tablespace);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	tablespaceDirEntry = PersistentTablespace_FindDirUnderLock(tablespace);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespace);

	if (tablespaceDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Create Pending' (actual state '%s')", 
			 tablespace,
			 PersistentFileSysObj_StateName(tablespaceDirEntry->state));

	tablespaceDirEntry->state = PersistentFileSysState_AbortingCreate;

	PersistentTablespace_UpdateTuple(
								tablespaceDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we physically removed the tablespace file.
 */
void PersistentTablespace_Dropped(
	Oid 		tablespace,
				/* The tablespace OID for the dropped tablespace. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	TablespaceDirEntry tablespaceDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are in bootstrap mode",
				 tablespace);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	tablespaceDirEntry = PersistentTablespace_FindDirUnderLock(tablespace);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespace);

	if (tablespaceDirEntry->state != PersistentFileSysState_DropPending &&
		tablespaceDirEntry->state != PersistentFileSysState_AbortingCreate)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Drop Pending' or 'Aborting Create' (actual state '%s')", 
			 tablespace,
			 PersistentFileSysObj_StateName(tablespaceDirEntry->state));

	tablespaceDirEntry->state = PersistentFileSysState_Free;

	PersistentFileSysObj_FreeTuple(
							&persistentTablespaceData.fileSysObjData,
							&persistentTablespaceSharedData->fileSysObjSharedData,
							&tablespaceDirEntry->presistentTid,
							/* flushToXLog */ false);

	PersistentTablespace_RemoveDirUnderLock(tablespaceDirEntry);

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

/*
 * Return the required shared-memory size for this module.
 */
Size PersistentTablespace_ShmemSize(void)
{
	Size		size;

	/* The hash table of persistent tablespaces */
	size = hash_estimate_size((Size)MaxPersistentTablespaces,
							  sizeof(TablespaceDirEntryData));

	/* The shared-memory structure. */
	size = add_size(size, sizeof(TablespaceDirEntryData));

	/* Add a safety margin. */
	size = add_size(size, size / 10);

	return size;
}

/*
 * PersistentTablespace_HashTableInit
 *
 * Create or find shared-memory hash table.
 */
static bool
PersistentTablespace_HashTableInit(void)
{
	HASHCTL			info;
	int				hash_flags;

	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	info.entrysize = sizeof(TablespaceDirEntryData);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	persistentTablespaceSharedHashTable = 
						ShmemInitHash("Persistent Tablespace Hash",
								   MaxPersistentTablespaces,
								   MaxPersistentTablespaces,
								   &info,
								   hash_flags);

	if (persistentTablespaceSharedHashTable == NULL)
		return false;

	return true;
}
						
/*
 * Initialize the shared-memory for this module.
 */
void PersistentTablespace_ShmemInit(void)
{
	bool found;
	bool ok;

	/* Create the shared-memory structure. */
	persistentTablespaceSharedData = 
		(PersistentTablespaceSharedData *)
						ShmemInitStruct("Persistent Tablespace Data",
										sizeof(TablespaceDirEntryData),
										&found);

	if (!found)
	{
		PersistentFileSysObj_InitShared(
						&persistentTablespaceSharedData->fileSysObjSharedData);
	}

	/* Create or find our shared-memory hash table. */
	ok = PersistentTablespace_HashTableInit();
	if (!ok)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("Not enough shared memory for persistent tablespace hash table")));

	PersistentFileSysObj_Init(
						&persistentTablespaceData.fileSysObjData,
						PersistentFsObjType_TablespaceDir,
						PersistentTablespace_ScanTupleCallback);


	Assert(persistentTablespaceSharedData != NULL);
	Assert(persistentTablespaceSharedHashTable != NULL);
}

