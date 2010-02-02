/*-------------------------------------------------------------------------
 *
 * cdbpersistentfilespace.c
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
#include "catalog/pg_filespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbsharedoidsearch.h"
#include "access/persistentfilesysobjname.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentfilespace.h"
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
#include "cdb/cdbvars.h"


/*
 * GUC variables
 */
int		MaxPersistentFilespaces = 128;		/* Max # of persistent filespaces */
						// UNDONE: Create GUC have default value there....

typedef struct PersistentFilespaceSharedData
{
	
	PersistentFileSysObjSharedData		fileSysObjSharedData;

} PersistentFilespaceSharedData;

#define PersistentFilespaceData_StaticInit {PersistentFileSysObjData_StaticInit}

typedef struct PersistentFilespaceData
{

	PersistentFileSysObjData		fileSysObjData;

} PersistentFilespaceData;

typedef struct FilespaceDirEntryKey
{
	Oid	filespaceOid;
} FilespaceDirEntryKey;

typedef struct FilespaceDirEntryData
{
	FilespaceDirEntryKey	key;

	int16	dbId1;
	char	locationBlankPadded1[FilespaceLocationBlankPaddedWithNullTermLen];

	int16	dbId2;
	char	locationBlankPadded2[FilespaceLocationBlankPaddedWithNullTermLen];

	PersistentFileSysState	state;
	int64					serialNum;
	TransactionId			parentXid;
	ItemPointerData 		presistentTid;
	
} FilespaceDirEntryData;
typedef FilespaceDirEntryData *FilespaceDirEntry;


/*
 * Global Variables
 */
PersistentFilespaceSharedData	*persistentFilespaceSharedData = NULL;
static HTAB *persistentFilespaceSharedHashTable = NULL;

PersistentFilespaceData	persistentFilespaceData = PersistentFilespaceData_StaticInit;

/*
 * Return the hash entry for a filespace.
 */
static FilespaceDirEntry
PersistentFilespace_FindDirUnderLock(
	Oid			filespaceOid)
{
	bool			found;

	FilespaceDirEntry	filespaceDirEntry;

	FilespaceDirEntryKey key;

	key.filespaceOid = filespaceOid;

	filespaceDirEntry = 
			(FilespaceDirEntry) 
					hash_search(persistentFilespaceSharedHashTable,
								(void *) &key,
								HASH_FIND,
								&found);

	return filespaceDirEntry;
}

static FilespaceDirEntry
PersistentFilespace_CreateDirUnderLock(
	Oid			filespaceOid)
{
	bool			found;

	FilespaceDirEntry	filespaceDirEntry;

	FilespaceDirEntryKey key;

	key.filespaceOid = filespaceOid;

	filespaceDirEntry = 
			(FilespaceDirEntry) 
					hash_search(persistentFilespaceSharedHashTable,
								(void *) &key,
								HASH_ENTER_NULL,
								&found);

	if (filespaceDirEntry == NULL)
		elog(ERROR, "Out of shared-memory for persistent filespaces");

	filespaceDirEntry->state = 0;
	filespaceDirEntry->serialNum = 0;
	filespaceDirEntry->parentXid = InvalidTransactionId;
	MemSet(&filespaceDirEntry->presistentTid, 0, sizeof(ItemPointerData));
	
	return filespaceDirEntry;
}

static void
PersistentFilespace_RemoveDirUnderLock(
	FilespaceDirEntry	filespaceDirEntry)
{
	FilespaceDirEntry	removeFilespaceDirEntry;

	removeFilespaceDirEntry = 
				(FilespaceDirEntry) 
						hash_search(persistentFilespaceSharedHashTable,
									(void *) &filespaceDirEntry->key,
									HASH_REMOVE,
									NULL);

	if (removeFilespaceDirEntry == NULL)
		elog(ERROR, "Trying to delete entry that does not exist");
}


// -----------------------------------------------------------------------------
// Scan 
// -----------------------------------------------------------------------------

static bool PersistentFilespace_ScanTupleCallback(
	int 					token,
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	Oid		filespaceOid;

	int16	dbId1;
	char	locationBlankPadded1[FilespaceLocationBlankPaddedWithNullTermLen];

	int16	dbId2;
	char	locationBlankPadded2[FilespaceLocationBlankPaddedWithNullTermLen];
	
	PersistentFileSysState	state;
	MirrorFileSysState		mirrorState;	// UNDONE
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	FilespaceDirEntry filespaceDirEntry;

	Assert(token == PersistentFsObjType_FilespaceDir);

	GpPersistentFilespaceNode_GetValues(
									values,
									&filespaceOid,
									&dbId1,
									locationBlankPadded1,
									&dbId2,
									locationBlankPadded2,
									&state,
									&mirrorState,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Free)
	{
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "PersistentFilespace_ScanTupleCallback: TID %s, serial number " INT64_FORMAT " is free",
				 ItemPointerToString2(persistentTid),
				 persistentSerialNum);
		return true;	// Continue.
	}
	
	filespaceDirEntry = 
			PersistentFilespace_CreateDirUnderLock(
											filespaceOid);

	filespaceDirEntry->dbId1 = dbId1;
	memcpy(filespaceDirEntry->locationBlankPadded1, locationBlankPadded1, FilespaceLocationBlankPaddedWithNullTermLen);

	filespaceDirEntry->dbId2 = dbId2;
	memcpy(filespaceDirEntry->locationBlankPadded2, locationBlankPadded2, FilespaceLocationBlankPaddedWithNullTermLen);

	filespaceDirEntry->state = state;
	filespaceDirEntry->serialNum = serialNum;
	filespaceDirEntry->parentXid = parentXid;
	filespaceDirEntry->presistentTid = *persistentTid;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentFilespace_ScanTupleCallback: filespace %u, dbId1 %d, dbId2 %d, state %s, TID %s, serial number " INT64_FORMAT,
			 filespaceOid,
			 dbId1,
			 dbId2,
			 PersistentFileSysObj_StateName(state),
			 ItemPointerToString2(persistentTid),
			 persistentSerialNum);

	return true;	// Continue.
}

static void PersistentFilespace_InitScanUnderLock(void)
{
	if (persistentFilespaceSharedData == NULL)
		elog(PANIC, "Persistent filespace information shared-memory not setup");

	PersistentFileSysObj_InitScanUnderLock(
					&persistentFilespaceData.fileSysObjData,
					&persistentFilespaceSharedData->fileSysObjSharedData);
}

void PersistentFilespace_InitScan(void)
{
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFilespace_InitScanUnderLock();

	LWLockRelease(PersistentObjLock);
}

//------------------------------------------------------------------------------

static Oid persistentFilespaceCheck;
static bool persistentFilespaceCheckFound;

static bool PersistentFilespace_CheckScanTupleCallback(
	int 					token,
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	Oid		filespaceOid;

	int16	dbId1;
	char	locationBlankPadded1[FilespaceLocationBlankPaddedWithNullTermLen];

	int16	dbId2;
	char	locationBlankPadded2[FilespaceLocationBlankPaddedWithNullTermLen];
	
	PersistentFileSysState	state;
	MirrorFileSysState		mirrorState;	// UNDONE
	TransactionId			parentXid;
	int64					serialNum;
	ItemPointerData			previousFreeTid;
	
	Assert(token == PersistentFsObjType_FilespaceDir);

	GpPersistentFilespaceNode_GetValues(
									values,
									&filespaceOid,
									&dbId1,
									locationBlankPadded1,
									&dbId2,
									locationBlankPadded2,
									&state,
									&mirrorState,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	if (state == PersistentFileSysState_Created &&
		filespaceOid == persistentFilespaceCheck)
	{
		persistentFilespaceCheckFound = true;
		return false;
	}

	return true;	// Continue.
}

bool PersistentFilespace_Check(
	Oid				filespace)
{
	persistentFilespaceCheck = filespace;
	persistentFilespaceCheckFound = false;

	PersistentFileSysObj_Scan(
		&persistentFilespaceData.fileSysObjData,
		&persistentFilespaceSharedData->fileSysObjSharedData,
		PersistentFilespace_CheckScanTupleCallback);

	return persistentFilespaceCheckFound;
}

// -----------------------------------------------------------------------------
// Helpers 
// -----------------------------------------------------------------------------

static void PersistentFilespace_AddTuple(
	FilespaceDirEntry filespaceDirEntry,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Oid filespaceOid = filespaceDirEntry->key.filespaceOid;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_filespace_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	GpPersistentFilespaceNode_SetDatumValues(
										values,
										filespaceOid,
										filespaceDirEntry->dbId1,
										filespaceDirEntry->locationBlankPadded1,
										filespaceDirEntry->dbId2,
										filespaceDirEntry->locationBlankPadded2,
										filespaceDirEntry->state,
										MirrorFileSysState_None,		// UNDONE
										filespaceDirEntry->parentXid,
										/* persistentSerialNum */ 0,	// This will be set by PersistentFileSysObj_AddTuple.
										&previousFreeTid);

	PersistentFileSysObj_AddTuple(
							&persistentFilespaceData.fileSysObjData,
							&persistentFilespaceSharedData->fileSysObjSharedData,
							values,
							flushToXLog,
							&filespaceDirEntry->presistentTid,
							&filespaceDirEntry->serialNum);
}

static void PersistentFilespace_UpdateTuple(
	FilespaceDirEntry filespaceDirEntry,

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Oid filespaceOid = filespaceDirEntry->key.filespaceOid;

	ItemPointerData previousFreeTid;

	Datum values[Natts_gp_persistent_filespace_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	GpPersistentFilespaceNode_SetDatumValues(
										values,
										filespaceOid,
										filespaceDirEntry->dbId1,
										filespaceDirEntry->locationBlankPadded1,
										filespaceDirEntry->dbId2,
										filespaceDirEntry->locationBlankPadded2,
										filespaceDirEntry->state,
										MirrorFileSysState_None,		// UNDONE
										filespaceDirEntry->parentXid,
										filespaceDirEntry->serialNum,
										&previousFreeTid);

	PersistentFileSysObj_UpdateTuple(
							&persistentFilespaceData.fileSysObjData,
							&persistentFilespaceSharedData->fileSysObjSharedData,
							&filespaceDirEntry->presistentTid,
							values,
							flushToXLog);
}

static void PersistentFilespace_BlankPadCopyLocation(
	char locationBlankPadded[FilespaceLocationBlankPaddedWithNullTermLen],
	char *location)
{
	int len;
	int blankPadLen;

	if (location != NULL)
	{
		len = strlen(location);
		if (len > FilespaceLocationBlankPaddedWithNullTermLen - 1)
			elog(ERROR, "Location '%s' is too long (found %d characaters -- expected no more than %d characters)",
				 location,
			     len,
			     FilespaceLocationBlankPaddedWithNullTermLen - 1);
	}
	else
		len = 0;

	if (len > 0)
		memcpy(locationBlankPadded, location, len);
	
	blankPadLen = FilespaceLocationBlankPaddedWithNullTermLen - 1 - len;
	if (blankPadLen > 0)
		MemSet(&locationBlankPadded[len], ' ', blankPadLen);

	locationBlankPadded[FilespaceLocationBlankPaddedWithNullTermLen - 1] = '\0';
}

static void PersistentFilespace_ConvertBlankPaddedLocation(
	char 		**filespaceLocation,

	char 		*locationBlankPadded,

	bool		isPrimary)
{
	char *firstBlankChar;
	int len;

	firstBlankChar = strchr(locationBlankPadded, ' ');
	if (firstBlankChar != NULL)
	{
		len = firstBlankChar - locationBlankPadded;
		if (len == 0)
		{
			if (isPrimary)
				elog(ERROR, "Expecting non-empty primary location");

			*filespaceLocation = NULL;
			return;
		}

		*filespaceLocation = (char*)palloc(len + 1);
		memcpy(*filespaceLocation, locationBlankPadded, len);
		(*filespaceLocation)[len] = '\0';
	}
	else
	{
		/*
		 * Whole location is non-blank characters...
		 */
		len = strlen(locationBlankPadded) + 1;
		if (len != FilespaceLocationBlankPaddedWithNullTermLen)
			elog(ERROR, "Incorrect format for blank-padded filespace location");

		*filespaceLocation = pstrdup(locationBlankPadded);
	}
}

void PersistentFilespace_GetPrimaryAndMirrorUnderLock(
	Oid 		filespaceOid,
				/* The filespace OID for the create. */

	char **primaryFilespaceLocation,
				/* The primary filespace directory path.  Return NULL for global and base. */
	
	char **mirrorFilespaceLocation)
				/* The primary filespace directory path.  Return NULL for global and base. 
				 * Or, returns NULL when mirror not configured. */
{
	FilespaceDirEntry filespaceDirEntry;

	int16 primaryDbId;

	char *primaryBlankPadded;
	char *mirrorBlankPadded;

	*primaryFilespaceLocation = NULL;
	*mirrorFilespaceLocation = NULL;
	
	filespaceDirEntry = 
				PersistentFilespace_FindDirUnderLock(
												filespaceOid);
	if (filespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent filespace entry %u", 
			 filespaceOid);

	primaryDbId = GpIdentity.dbid;
	if (filespaceDirEntry->dbId1 == primaryDbId)
	{
		primaryBlankPadded = filespaceDirEntry->locationBlankPadded1;
		mirrorBlankPadded = filespaceDirEntry->locationBlankPadded2;
	}
	else
	{
		primaryBlankPadded = filespaceDirEntry->locationBlankPadded2;
		mirrorBlankPadded = filespaceDirEntry->locationBlankPadded1;
	}

	PersistentFilespace_ConvertBlankPaddedLocation(
											primaryFilespaceLocation,
											primaryBlankPadded,
											/* isPrimary */ true);

	PersistentFilespace_ConvertBlankPaddedLocation(
											mirrorFilespaceLocation,
											mirrorBlankPadded,
											/* isPrimary */ false);
}

void PersistentFilespace_GetPrimaryAndMirror(
	Oid 		filespaceOid,
				/* The filespace OID for the create. */

	char **primaryFilespaceLocation,
				/* The primary filespace directory path.  Return NULL for global and base. */
	
	char **mirrorFilespaceLocation)
				/* The primary filespace directory path.  Return NULL for global and base. 
				 * Or, returns NULL when mirror not configured. */
{
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFilespace_GetPrimaryAndMirrorUnderLock(
											filespaceOid,
											primaryFilespaceLocation,
											mirrorFilespaceLocation);

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// State Change 
// -----------------------------------------------------------------------------

/*
 * Indicate we intend to create a filespace file as part of the current transaction.
 *
 * An XLOG IntentToCreate record is generated that will guard the subsequent file-system
 * create in case the transaction aborts.
 *
 * After 1 or more calls to this routine to mark intention about filespace files that are going
 * to be created, call ~_DoPendingCreates to do the actual file-system creates.  (See its
 * note on XLOG flushing).
 */
void PersistentFilespace_MarkCreatePending(
	Oid 		filespaceOid,
				/* The filespace where the filespace lives. */

	int16		primaryDbId,
	
	char 		*primaryFilespaceLocation,
				/* 
				 * The primary filespace directory path.  NOT Blank padded.
				 * Just a NULL terminated string.
				 */

	int16		mirrorDbId,
	
	char 		*mirrorFilespaceLocation,

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	bool			flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	FilespaceDirEntry filespaceDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent filespace %u because we are in bootstrap mode",
				 filespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentFilespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	filespaceDirEntry = 
				PersistentFilespace_CreateDirUnderLock(
												filespaceOid);
	Assert(filespaceDirEntry != NULL);

	filespaceDirEntry->dbId1 = primaryDbId;
	PersistentFilespace_BlankPadCopyLocation(
										filespaceDirEntry->locationBlankPadded1,
										primaryFilespaceLocation);

	filespaceDirEntry->dbId2 = mirrorDbId;
	PersistentFilespace_BlankPadCopyLocation(
										filespaceDirEntry->locationBlankPadded2,
										mirrorFilespaceLocation);

	filespaceDirEntry->state = PersistentFileSysState_CreatePending;
	filespaceDirEntry->parentXid = GetTopTransactionId();

	PersistentFilespace_AddTuple(
							filespaceDirEntry,
							flushToXLog);

	*persistentTid = filespaceDirEntry->presistentTid;

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// Transaction End  
// -----------------------------------------------------------------------------

/*
 * Indicate the transaction commited and the filespace is officially created.
 */
void PersistentFilespace_Created(
	Oid 		filespaceOid,
				/* The filespace OID for the create. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the filespace.	Distinquishes the uses of the tuple. */

{
	FilespaceDirEntry filespaceDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent filespace %u because we are in bootstrap mode",
				 filespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentFilespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	filespaceDirEntry = 
				PersistentFilespace_FindDirUnderLock(
												filespaceOid);
	if (filespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent filespace entry %u", 
			 filespaceOid);

	if (filespaceDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent filespace entry %u expected to be in 'Create Pending' state (actual state '%s')", 
			 filespaceOid,
			 PersistentFileSysObj_StateName(filespaceDirEntry->state));

	filespaceDirEntry->state = PersistentFileSysState_Created;

	PersistentFilespace_UpdateTuple(
								filespaceDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we intend to drop a filespace file as part of the current transaction.
 *
 * This filespace file to drop will be listed inside a commit, distributed commit, a distributed 
 * prepared, and distributed commit prepared XOG records.
 *
 * For any of the commit type records, once that XLOG record is flushed then the actual
 * file-system delete will occur.  The flush guarantees the action will be retried after system
 * crash.
 */
void PersistentFilespace_MarkDropPending(
	Oid 		filespaceOid,
				/* The filespace OID for the drop. */

	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the filespace.	Distinquishes the uses of the tuple. */

{
	FilespaceDirEntry filespaceDirEntry;

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent filespace %u because we are in bootstrap mode",
				 filespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentFilespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	filespaceDirEntry = 
				PersistentFilespace_FindDirUnderLock(
												filespaceOid);
	if (filespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent filespace entry %u", 
			 filespaceOid);

	if (filespaceDirEntry->state != PersistentFileSysState_CreatePending &&
		filespaceDirEntry->state != PersistentFileSysState_Created)
		elog(ERROR, "Persistent filespace entry %u expected to be in 'Create Pending' or 'Created' state (actual state '%s')", 
			 filespaceOid,
			 PersistentFileSysObj_StateName(filespaceDirEntry->state));

	filespaceDirEntry->state = PersistentFileSysState_DropPending;

	PersistentFilespace_UpdateTuple(
								filespaceDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we are aborting the create of a filespace file.
 *
 * This state will make sure the filespace gets dropped after a system crash.
 */
void PersistentFilespace_MarkAbortingCreate(
	Oid 		filespaceOid,
				/* The filespace OID for the aborting create. */
							
	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the filespace.	Distinquishes the uses of the tuple. */

{
	FilespaceDirEntry filespaceDirEntry;
	
	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent filespace %u because we are in bootstrap mode",
				 filespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentFilespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	filespaceDirEntry = 
				PersistentFilespace_FindDirUnderLock(
												filespaceOid);
	if (filespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent filespace entry %u", 
			 filespaceOid);

	if (filespaceDirEntry->state != PersistentFileSysState_CreatePending)
		elog(ERROR, "Persistent filespace entry %u expected to be in 'Create Pending' (actual state '%s')", 
			 filespaceOid,
			 PersistentFileSysObj_StateName(filespaceDirEntry->state));

	filespaceDirEntry->state = PersistentFileSysState_AbortingCreate;

	PersistentFilespace_UpdateTuple(
								filespaceDirEntry,
								/* flushToXLog */ false);

	LWLockRelease(PersistentObjLock);
}

/*
 * Indicate we physically removed the filespace file.
 */
void PersistentFilespace_Dropped(
	Oid 		filespaceOid,
				/* The filespace OID for the dropped filespace. */
										
	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64			serialNum)
				/* Serial number for the filespace.	Distinquishes the uses of the tuple. */

{
	FilespaceDirEntry filespaceDirEntry;

	ItemPointerData previousFreeTid;

	char	locationBlankFilled[FilespaceLocationBlankPaddedWithNullTermLen];

	Datum freeValues[Natts_gp_persistent_filespace_node];

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	MemSet(locationBlankFilled, ' ', FilespaceLocationBlankPaddedWithNullTermLen - 1);
	locationBlankFilled[FilespaceLocationBlankPaddedWithNullTermLen - 1] = '\0';

	if (IsBootstrapProcessingMode())
	{	
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Skipping persistent filespace %u because we are in bootstrap mode",
				 filespaceOid);

		return;	// The initdb process will load the persistent table once we out of bootstrap mode.
	}

	PersistentFilespace_InitScan();

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	filespaceDirEntry = 
				PersistentFilespace_FindDirUnderLock(
												filespaceOid);
	if (filespaceDirEntry == NULL)
		elog(ERROR, "Did not find persistent filespace entry %u", 
			 filespaceOid);

	if (filespaceDirEntry->state != PersistentFileSysState_DropPending &&
		filespaceDirEntry->state != PersistentFileSysState_AbortingCreate)
		elog(ERROR, "Persistent filespace entry %u expected to be in 'Drop Pending' or 'Aborting Create' (actual state '%s')", 
			 filespaceOid,
			 PersistentFileSysObj_StateName(filespaceDirEntry->state));

	GpPersistentFilespaceNode_SetDatumValues(
										freeValues,
										/* filespaceOid */ 0,
										/* dbId1 */ 0,
										locationBlankFilled,
										/* dbId2 */ 0,
										locationBlankFilled,
										PersistentFileSysState_Free,
										MirrorFileSysState_None,
										InvalidTransactionId,
										/* persistentSerialNum */ 0,
										&previousFreeTid);

	PersistentFileSysObj_FreeTuple(
							&persistentFilespaceData.fileSysObjData,
							&persistentFilespaceSharedData->fileSysObjSharedData,
							&filespaceDirEntry->presistentTid,
							freeValues,
							/* flushToXLog */ false);

	PersistentFilespace_RemoveDirUnderLock(filespaceDirEntry);

	LWLockRelease(PersistentObjLock);
}

// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

/*
 * Return the required shared-memory size for this module.
 */
Size PersistentFilespace_ShmemSize(void)
{
	Size		size;

	/* The hash table of persistent filespaces */
	size = hash_estimate_size((Size)MaxPersistentFilespaces,
							  sizeof(FilespaceDirEntryData));

	/* The shared-memory structure. */
	size = add_size(size, sizeof(FilespaceDirEntryData));

	/* Add a safety margin. */
	size = add_size(size, size / 10);

	return size;
}

/*
 * PersistentFilespace_HashTableInit
 *
 * Create or find shared-memory hash table.
 */
static bool
PersistentFilespace_HashTableInit(void)
{
	HASHCTL			info;
	int				hash_flags;

	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(FilespaceDirEntryKey);
	info.entrysize = sizeof(FilespaceDirEntryData);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	persistentFilespaceSharedHashTable = 
						ShmemInitHash("Persistent Filespace Hash",
								   MaxPersistentFilespaces,
								   MaxPersistentFilespaces,
								   &info,
								   hash_flags);

	if (persistentFilespaceSharedHashTable == NULL)
		return false;

	return true;
}
						
/*
 * Initialize the shared-memory for this module.
 */
void PersistentFilespace_ShmemInit(void)
{
	bool found;
	bool ok;

	/* Create the shared-memory structure. */
	persistentFilespaceSharedData = 
		(PersistentFilespaceSharedData *)
						ShmemInitStruct("Persistent Filespace Data",
										sizeof(FilespaceDirEntryData),
										&found);

	if (!found)
	{
		PersistentFileSysObj_InitShared(
						&persistentFilespaceSharedData->fileSysObjSharedData);
	}

	/* Create or find our shared-memory hash table. */
	ok = PersistentFilespace_HashTableInit();
	if (!ok)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("Not enough shared memory for persistent filespace hash table")));

	PersistentFileSysObj_Init(
						&persistentFilespaceData.fileSysObjData,
						PersistentFsObjType_FilespaceDir,
						PersistentFilespace_ScanTupleCallback);


	Assert(persistentFilespaceSharedData != NULL);
	Assert(persistentFilespaceSharedHashTable != NULL);
}


