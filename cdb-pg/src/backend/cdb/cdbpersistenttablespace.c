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
#include "cdb/cdbpersistentfilespace.h"
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

typedef struct PersistentTablespaceSharedData
{
	
	PersistentFileSysObjSharedData		fileSysObjSharedData;

} PersistentTablespaceSharedData;

#define PersistentTablespaceData_StaticInit {PersistentFileSysObjData_StaticInit}

typedef struct PersistentTablespaceData
{

	PersistentFileSysObjData		fileSysObjData;

} PersistentTablespaceData;

typedef struct TablespaceDirEntryKey
{
	Oid	tablespaceOid;
} TablespaceDirEntryKey;

typedef struct TablespaceDirEntryData
{
	TablespaceDirEntryKey	key;

	Oid						filespaceOid;

	PersistentFileSysState	state;
	int64					persistentSerialNum;
	ItemPointerData 		persistentTid;
	
} TablespaceDirEntryData;
typedef TablespaceDirEntryData *TablespaceDirEntry;


/*
 * Global Variables
 */
PersistentTablespaceSharedData	*persistentTablespaceSharedData = NULL;
static HTAB *persistentTablespaceSharedHashTable = NULL;

PersistentTablespaceData	persistentTablespaceData = PersistentTablespaceData_StaticInit;

static void PersistentTablespace_InitScan(void)
{
	if (persistentTablespaceSharedData == NULL)
		elog(PANIC, "Persistent tablespace information shared-memory not setup");

	PersistentFileSysObj_InitScan();
}

/*
 * Return the hash entry for a tablespace.
 */
static TablespaceDirEntry
PersistentTablespace_FindEntryUnderLock(
	Oid			tablespaceOid)
{
	bool			found;

	TablespaceDirEntry	tablespaceDirEntry;

	TablespaceDirEntryKey key;

	if (persistentTablespaceSharedHashTable == NULL)
		elog(PANIC, "Persistent tablespace information shared-memory not setup");

	key.tablespaceOid = tablespaceOid;

	tablespaceDirEntry = 
			(TablespaceDirEntry) 
					hash_search(persistentTablespaceSharedHashTable,
								(void *) &key,
								HASH_FIND,
								&found);
	if (!found)
		return NULL;

	return tablespaceDirEntry;
}

static TablespaceDirEntry
PersistentTablespace_CreateEntryUnderLock(
	Oid			filespaceOid,
	
	Oid 		tablespaceOid)
{
	bool			found;

	TablespaceDirEntry	tablespaceDirEntry;

	TablespaceDirEntryKey key;

	if (persistentTablespaceSharedHashTable == NULL)
		elog(PANIC, "Persistent tablespace information shared-memory not setup");

	key.tablespaceOid = tablespaceOid;

	tablespaceDirEntry = 
			(TablespaceDirEntry) 
					hash_search(persistentTablespaceSharedHashTable,
								(void *) &key,
								HASH_ENTER_NULL,
								&found);

	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Out of shared-memory for persistent tablespaces");

	tablespaceDirEntry->filespaceOid = filespaceOid;
	
	tablespaceDirEntry->state = 0;
	tablespaceDirEntry->persistentSerialNum = 0;
	MemSet(&tablespaceDirEntry->persistentTid, 0, sizeof(ItemPointerData));
	
	return tablespaceDirEntry;
}

static void
PersistentTablespace_RemoveEntryUnderLock(
	TablespaceDirEntry	tablespaceDirEntry)
{
	TablespaceDirEntry	removeTablespaceDirEntry;

	if (persistentTablespaceSharedHashTable == NULL)
		elog(PANIC, "Persistent tablespace information shared-memory not setup");

	removeTablespaceDirEntry = 
				(TablespaceDirEntry) 
						hash_search(persistentTablespaceSharedHashTable,
									(void *) &tablespaceDirEntry->key,
									HASH_REMOVE,
									NULL);

	if (removeTablespaceDirEntry == NULL)
		elog(ERROR, "Trying to delete entry that does not exist");
}

PersistentFileSysState
PersistentTablespace_GetState(
	Oid		tablespaceOid)
{
	TablespaceDirEntry tablespaceDirEntry;

	PersistentFileSysState	state;

	/*
	 * pg_default and pg_global always exist, but do not have entries in
	 * gp_persistent_tablespace_node.
	 */
	if (tablespaceOid == DEFAULTTABLESPACE_OID ||
		tablespaceOid == GLOBALTABLESPACE_OID) 
	{ 
		return PersistentFileSysState_Created; 
	}

	PersistentTablespace_InitScan();

	// NOTE: Since we are not accessing data in the Buffer Pool, we don't need to
	// acquire the MirroredLock.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	tablespaceDirEntry = 
				PersistentTablespace_FindEntryUnderLock(
												tablespaceOid);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespaceOid);

	state = tablespaceDirEntry->state;

	LWLockRelease(PersistentObjLock);

	return state;
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
	Oid		filespaceOid;
	Oid		tablespaceOid;
	
	PersistentFileSysState	state;

	MirroredObjectExistenceState		mirrorExistenceState;	// UNDONE

	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	TablespaceDirEntry tablespaceDirEntry;

	Assert(token == PersistentFsObjType_TablespaceDir);

	GpPersistentTablespaceNode_GetValues(
									values,
									&filespaceOid,
									&tablespaceOid,
									&state,
									&mirrorExistenceState,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Free)
	{
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "PersistentTablespace_ScanTupleCallback: TID %s, serial number " INT64_FORMAT " is free",
				 ItemPointerToString2(persistentTid),
				 persistentSerialNum);
		return true;	// Continue.
	}
	
	tablespaceDirEntry = 
		PersistentTablespace_CreateEntryUnderLock(filespaceOid, tablespaceOid);
	
	tablespaceDirEntry->state = state;
	tablespaceDirEntry->persistentSerialNum = serialNum;
	tablespaceDirEntry->persistentTid = *persistentTid;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentTablespace_ScanTupleCallback: tablespace %u, filespace %u, state %s, TID %s, serial number " INT64_FORMAT,
			 tablespaceOid,
			 filespaceOid,
			 PersistentFileSysObjState_Name(state),
			 ItemPointerToString2(persistentTid),
			 persistentSerialNum);

	return true;	// Continue.
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
	Oid		filespaceOid;
	Oid		tablespaceOid;
	
	PersistentFileSysState	state;
	MirroredObjectExistenceState		mirrorExistenceState;	// UNDONE
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	Assert(token == PersistentFsObjType_TablespaceDir);

	GpPersistentTablespaceNode_GetValues(
									values,
									&filespaceOid,
									&tablespaceOid,
									&state,
									&mirrorExistenceState,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Created &&
		tablespaceOid == persistentTablespaceCheck)
	{
		persistentTablespaceCheckFound = true;
		return false;
	}

	return true;	// Continue.
}

bool PersistentTablespace_Check(
	Oid				tablespace)
{
	PersistentTablespace_InitScan();

	persistentTablespaceCheck = tablespace;
	persistentTablespaceCheckFound = false;

	PersistentFileSysObj_Scan(
		PersistentFsObjType_DatabaseDir,
		PersistentTablespace_CheckScanTupleCallback);

	return persistentTablespaceCheckFound;
}

extern void PersistentTablespace_LookupTidAndSerialNum(
	Oid 		tablespaceOid,
				/* The tablespace OID for the lookup. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_tablespace_node tuple for the rel file */

	int64			*persistentSerialNum)
{
	TablespaceDirEntry tablespaceDirEntry;

	PersistentTablespace_InitScan();

	// NOTE: Since we are not accessing data in the Buffer Pool, we don't need to
	// acquire the MirroredLock.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	tablespaceDirEntry = 
				PersistentTablespace_FindEntryUnderLock(
												tablespaceOid);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespaceOid);

	*persistentTid = tablespaceDirEntry->persistentTid;
	*persistentSerialNum = tablespaceDirEntry->persistentSerialNum;

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// Helpers 
// -----------------------------------------------------------------------------

static void PersistentTablespace_AddTuple(
	TablespaceDirEntry tablespaceDirEntry,

	MirroredObjectExistenceState mirrorObjectState,

	TransactionId parentXid,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Oid filespaceOid = tablespaceDirEntry->filespaceOid;
	Oid tablespaceOid = tablespaceDirEntry->key.tablespaceOid;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_tablespace_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	GpPersistentTablespaceNode_SetDatumValues(
								values,
								filespaceOid,
								tablespaceOid,
								tablespaceDirEntry->state,
								mirrorObjectState,		// UNDONE
								parentXid,
								/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
								&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							PersistentFsObjType_TablespaceDir,
							values,
							flushToXLog,
							&tablespaceDirEntry->persistentTid,
							&tablespaceDirEntry->persistentSerialNum);
}

void PersistentTablespace_GetPrimaryAndMirrorFilespaces(
	Oid 		tablespaceOid,
				/* The tablespace OID for the create. */

	char **primaryFilespaceLocation,
				/* The primary filespace directory path.  Return NULL for global and base. */
	
	char **mirrorFilespaceLocation)
				/* The primary filespace directory path.  Return NULL for global and base. 
				 * Or, returns NULL when mirror not configured. */
{
	bool lockHeldByMe;

	TablespaceDirEntry tablespaceDirEntry;

	*primaryFilespaceLocation = NULL;
	*mirrorFilespaceLocation = NULL;

	if (tablespaceOid == GLOBALTABLESPACE_OID ||
		tablespaceOid == DEFAULTTABLESPACE_OID)
	{
		/*
		 * Optimize out the common cases.
		 */
		 return;
	}

	PersistentTablespace_InitScan();

	// NOTE: Since we are not accessing data in the Buffer Pool, we don't need to
	// acquire the MirroredLock.

	lockHeldByMe = LWLockHeldByMe(PersistentObjLock);

	/*
	 * Try to prevent a performance issue from many processes gathering tablespace/filespace
	 * information.
	 */
	if (!lockHeldByMe)
		LWLockAcquire(PersistentObjLock, LW_SHARED);

	tablespaceDirEntry =
		PersistentTablespace_FindEntryUnderLock(
										tablespaceOid);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespaceOid);

	PersistentFilespace_GetPrimaryAndMirrorUnderLock(
												tablespaceDirEntry->filespaceOid,
												primaryFilespaceLocation,
												mirrorFilespaceLocation);

	if (!lockHeldByMe)
		LWLockRelease(PersistentObjLock);
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
	Oid 		filespaceOid,
				/* The filespace where the tablespace lives. */

	Oid 		tablespaceOid,
				/* The tablespace OID for the create. */

	MirroredObjectExistenceState mirrorExistenceState,

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			*persistentSerialNum,


	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	TablespaceDirEntry tablespaceDirEntry;

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are before persistence work",
				 tablespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	tablespaceDirEntry = 
				PersistentTablespace_CreateEntryUnderLock(
												filespaceOid,
												tablespaceOid);
	Assert(tablespaceDirEntry != NULL);

	tablespaceDirEntry->state = PersistentFileSysState_CreatePending;

	PersistentTablespace_AddTuple(
							tablespaceDirEntry,
							mirrorExistenceState,
							/* parentXid */ GetTopTransactionId(),
							flushToXLog);

	*persistentTid = tablespaceDirEntry->persistentTid;
	*persistentSerialNum = tablespaceDirEntry->persistentSerialNum;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
}

// -----------------------------------------------------------------------------
// Transaction End  
// -----------------------------------------------------------------------------

/*
 * Indicate the transaction commited and the tablespace is officially created.
 */
void PersistentTablespace_Created(
	Oid 		tablespaceOid,
				/* The tablespace OID for the create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	TablespaceDirEntry tablespaceDirEntry;

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are before persistence work",
				 tablespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	PersistentFileSysObjName_SetTablespaceDir(&fsObjName,tablespaceOid);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	tablespaceDirEntry = 
				PersistentTablespace_FindEntryUnderLock(
												tablespaceOid);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespaceOid);

	if (tablespaceDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Create Pending' state (actual state '%s')", 
			 tablespaceOid,
			 PersistentFileSysObjState_Name(tablespaceDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Created,
								/* flushToXlog */ false,
								NULL);

	tablespaceDirEntry->state = PersistentFileSysState_Created;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
}

void
PersistentTablespace_RemoveSegment(int16 dbid, bool ismirror)
{
	TablespaceDirEntry tablespaceDirEntry;
	HASH_SEQ_STATUS hstat;

	hash_seq_init(&hstat, persistentTablespaceSharedHashTable);

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "skipping persistent tablespace mirror addition");

		return;
	}

	PersistentTablespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	while ((tablespaceDirEntry = hash_seq_search(&hstat)) != NULL)
	{
		PersistentFileSysObjName fsObjName;
		Oid tblspc = tablespaceDirEntry->key.tablespaceOid;

		tablespaceDirEntry = PersistentTablespace_FindEntryUnderLock(tblspc);

		if (tablespaceDirEntry == NULL)
			elog(ERROR, "Did not find persistent tablespace entry %u", 
				 tblspc);

        PersistentFileSysObjName_SetTablespaceDir(&fsObjName, tblspc);

	    PersistentFileSysObj_RemoveSegment(&fsObjName,
										   &tablespaceDirEntry->persistentTid,
										   tablespaceDirEntry->persistentSerialNum,
										   dbid,
										   ismirror,
										   /* flushToXlog */ false);
	}

	LWLockRelease(PersistentObjLock);
}

void
PersistentTablespace_AddMirrorAll(
	int16			pridbid,
	int16			mirdbid)
{
	TablespaceDirEntry tablespaceDirEntry;
	HASH_SEQ_STATUS hstat;

	hash_seq_init(&hstat, persistentTablespaceSharedHashTable);

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "skipping persistent tablespace mirror addition");

		return;
	}

	PersistentTablespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	while ((tablespaceDirEntry = hash_seq_search(&hstat)) != NULL)
	{
		PersistentFileSysObjName fsObjName;
		Oid tblspc = tablespaceDirEntry->key.tablespaceOid;

		tablespaceDirEntry = PersistentTablespace_FindEntryUnderLock(tblspc);

		if (tablespaceDirEntry == NULL)
			elog(ERROR, "Did not find persistent tablespace entry %u", 
				 tblspc);

        PersistentFileSysObjName_SetTablespaceDir(&fsObjName, tblspc);

	    PersistentFileSysObj_AddMirror(&fsObjName,
    	                               &tablespaceDirEntry->persistentTid,
        	                           tablespaceDirEntry->persistentSerialNum,
									   pridbid,
            	                       mirdbid,
									   NULL,
                    	               /* flushToXlog */ false);
	}

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
	Oid 		tablespaceOid,
				/* The tablespace OID for the drop. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	TablespaceDirEntry tablespaceDirEntry;

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are before persistence work",
				 tablespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	PersistentFileSysObjName_SetTablespaceDir(&fsObjName,tablespaceOid);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	tablespaceDirEntry = 
				PersistentTablespace_FindEntryUnderLock(
												tablespaceOid);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespaceOid);

	if (tablespaceDirEntry->state != PersistentFileSysState_CreatePending &&
		tablespaceDirEntry->state != PersistentFileSysState_Created)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Create Pending' or 'Created' state (actual state '%s')", 
			 tablespaceOid,
			 PersistentFileSysObjState_Name(tablespaceDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_DropPending,
								/* flushToXlog */ false,
								NULL);

	tablespaceDirEntry->state = PersistentFileSysState_DropPending;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
}

/*
 * Indicate we are aborting the create of a tablespace file.
 *
 * This state will make sure the tablespace gets dropped after a system crash.
 */
void PersistentTablespace_MarkAbortingCreate(
	Oid 		tablespaceOid,
				/* The tablespace OID for the aborting create. */
							
	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	TablespaceDirEntry tablespaceDirEntry;
	
	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are before persistence work",
				 tablespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	PersistentFileSysObjName_SetTablespaceDir(&fsObjName,tablespaceOid);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	tablespaceDirEntry = 
				PersistentTablespace_FindEntryUnderLock(
												tablespaceOid);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespaceOid);

	if (tablespaceDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Create Pending' (actual state '%s')", 
			 tablespaceOid,
			 PersistentFileSysObjState_Name(tablespaceDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_AbortingCreate,
								/* flushToXlog */ false,
								NULL);

	tablespaceDirEntry->state = PersistentFileSysState_AbortingCreate;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
}

/*
 * Indicate we physically removed the tablespace file.
 */
void PersistentTablespace_Dropped(
	Oid 		tablespaceOid,
				/* The tablespace OID for the dropped tablespace. */
										
	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			persistentSerialNum)
				/* Serial number for the tablespace.	Distinquishes the uses of the tuple. */

{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjName fsObjName;

	TablespaceDirEntry tablespaceDirEntry;

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent tablespace %u because we are before persistence work",
				 tablespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentTablespace_InitScan();

	PersistentFileSysObjName_SetTablespaceDir(&fsObjName,tablespaceOid);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	tablespaceDirEntry = 
				PersistentTablespace_FindEntryUnderLock(
												tablespaceOid);
	if (tablespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent tablespace entry %u", 
			 tablespaceOid);

	if (tablespaceDirEntry->state != PersistentFileSysState_DropPending &&
		tablespaceDirEntry->state != PersistentFileSysState_AbortingCreate)
		elog(ERROR, "Persistent tablespace entry %u expected to be in 'Drop Pending' or 'Aborting Create' (actual state '%s')", 
			 tablespaceOid,
			 PersistentFileSysObjState_Name(tablespaceDirEntry->state));

	PersistentFileSysObj_StateChange(
								&fsObjName,
								persistentTid,
								persistentSerialNum,
								PersistentFileSysState_Free,
								/* flushToXlog */ false,
								NULL);

	tablespaceDirEntry->state = PersistentFileSysState_Free;

	PersistentTablespace_RemoveEntryUnderLock(tablespaceDirEntry);

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
}

// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

static Size PersistentTablespace_SharedDataSize(void)
{
	return MAXALIGN(sizeof(PersistentTablespaceSharedData));
}


/*
 * Return the required shared-memory size for this module.
 */
Size PersistentTablespace_ShmemSize(void)
{
	Size		size;

	/* The hash table of persistent tablespaces */
	size = hash_estimate_size((Size)gp_max_tablespaces,
							  sizeof(TablespaceDirEntryData));

	/* The shared-memory structure. */
	size = add_size(size, PersistentTablespace_SharedDataSize());

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
	info.keysize = sizeof(TablespaceDirEntryKey);
	info.entrysize = sizeof(TablespaceDirEntryData);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	persistentTablespaceSharedHashTable = 
						ShmemInitHash("Persistent Tablespace Hash",
								   gp_max_tablespaces,
								   gp_max_tablespaces,
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
										PersistentTablespace_SharedDataSize(),
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
						&persistentTablespaceSharedData->fileSysObjSharedData,
						PersistentFsObjType_TablespaceDir,
						PersistentTablespace_ScanTupleCallback);


	Assert(persistentTablespaceSharedData != NULL);
	Assert(persistentTablespaceSharedHashTable != NULL);
}
