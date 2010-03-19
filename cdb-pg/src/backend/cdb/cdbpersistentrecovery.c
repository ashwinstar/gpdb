/*-------------------------------------------------------------------------
 *
 * cdbpersistentrecovery.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "postgres.h"
#include "cdb/cdbdoublylinked.h"
#include "access/persistentfilesysobjname.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbpersistentrecovery.h"
#include "access/heapam.h"
#include "catalog/pg_tablespace.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "cdb/cdbdoublylinked.h"
#include "utils/hsearch.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "cdb/cdbdirectopen.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "cdb/cdbmirroredfilesysobj.h"

typedef struct XactEntryData
{
	TransactionId	xid;

	XactInfoKind	infoKind;

	DoublyLinkedHead	fsObjEntryList;
} XactEntryData;
typedef XactEntryData *XactEntry;

static HTAB *xactHashTable = NULL;

static void
PersistentRecovery_XactHashTableInit(void)
{
	HASHCTL			info;
	int				hash_flags;

	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(TransactionId);
	info.entrysize = sizeof(XactEntryData);
	info.hash = tag_hash;

	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	xactHashTable = hash_create("XactEntry", 10, &info, hash_flags);
}
static XactEntry PersistentRecovery_FindOrCreateXactEntry(
	TransactionId	xid,

	bool			*found)
{
	XactEntry xactEntry;

	if (xactHashTable == NULL)
		PersistentRecovery_XactHashTableInit();

	xactEntry = 
			(XactEntry) 
					hash_search(xactHashTable,
								(void *) &xid,
								HASH_ENTER,
								found);

	if (!*found)
	{
		DoublyLinkedHead_Init(&xactEntry->fsObjEntryList);
	}

	return xactEntry;
}

#if suppress
static XactEntry PersistentRecovery_FindXactEntry(
	TransactionId	xid)
{
	XactEntry xactEntry;
	bool found;

	Assert (xactHashTable != NULL);

	xactEntry = 
			(XactEntry) 
					hash_search(xactHashTable,
								(void *) &xid,
								HASH_FIND,
								&found);

	return xactEntry;
}
#endif

typedef struct FsObjEntryKey
{
	ItemPointerData				persistentTid;
} FsObjEntryKey;

typedef struct FsObjEntryData
{
	FsObjEntryKey				key;

	PersistentFileSysObjName	fsObjName;

	PersistentFileSysRelStorageMgr relStorageMgr;

	int64						persistentSerialNum;
	
	PersistentFileSysState		state;

	TransactionId				xid;

	bool						updateNeeded;

	DoubleLinks					xactLinks;

} FsObjEntryData;
typedef FsObjEntryData *FsObjEntry;

static char *
FsObjEntryToBuffer(char *buffer, FsObjEntry fsObjEntry)
{
	if (fsObjEntry->state != PersistentFileSysState_Free)	
		sprintf(buffer,
			    "%s: state '%s', transaction %u, persistent serial number " INT64_FORMAT ", TID %s, fsObjEntry address %p",
				PersistentFileSysObjName_TypeAndObjectName(&fsObjEntry->fsObjName),
				PersistentFileSysObjState_Name(fsObjEntry->state),
				fsObjEntry->xid,
				fsObjEntry->persistentSerialNum,
				ItemPointerToString(&fsObjEntry->key.persistentTid),
				fsObjEntry);
	else
		sprintf(buffer,
			    "Free entry as free number " INT64_FORMAT ", TID %s, fsObjEntry address %p",
				fsObjEntry->persistentSerialNum,
				ItemPointerToString(&fsObjEntry->key.persistentTid),
				fsObjEntry);

	return buffer;
}

static char fsObjEntryBuffer[200];

static char *
FsObjEntryToString(FsObjEntry fsObjEntry)
{
	return FsObjEntryToBuffer(fsObjEntryBuffer, fsObjEntry);
}

static HTAB **fsObjHashTable = NULL;

static void
PersistentRecovery_FsObjHashTableInit(void)
{
	HASHCTL			info;
	int				hash_flags;

	PersistentFsObjType fsObjType;

	char name[20];

	fsObjHashTable = (HTAB**)palloc(CountPersistentFsObjType * sizeof(HTAB*));

	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(FsObjEntryKey);
	info.entrysize = sizeof(FsObjEntryData);
	info.hash = tag_hash;

	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	for (fsObjType = PersistentFsObjType_First;
	     fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
 	{
 		sprintf(name, "FsObjEntry_%d", fsObjType);
		fsObjHashTable[fsObjType] = hash_create(name, 10, &info, hash_flags);
 	}

}

static FsObjEntry PersistentRecovery_FindOrCreateFsObjEntry(
	PersistentFsObjType fsObjType,
	
	ItemPointer			persistentTid,

	bool				*found)
{
	FsObjEntry fsObjEntry;

	FsObjEntryKey	key;

	if (fsObjHashTable == NULL)
		PersistentRecovery_FsObjHashTableInit();

	MemSet(&key, 0, sizeof(FsObjEntryKey));
	key.persistentTid = *persistentTid;

	fsObjEntry = 
				(FsObjEntry) 
						hash_search(fsObjHashTable[fsObjType],
									(void *) &key,
									HASH_ENTER,
									found);

	Assert(ItemPointerCompare(&fsObjEntry->key.persistentTid, persistentTid) == 0);

	if (!*found)
	{
		DoubleLinks_Init(&fsObjEntry->xactLinks);
		fsObjEntry->persistentSerialNum = 0;
		MemSet(&fsObjEntry->fsObjName, 0, sizeof(PersistentFileSysObjName));
		fsObjEntry->xid = InvalidTransactionId;
		fsObjEntry->state = -1;
		fsObjEntry->updateNeeded = false;
	}

	return fsObjEntry;
}

static void PersistentRecovery_AddEndXactFsObj(
	TransactionId 			xid,
	bool 					isCommit,
	PersistentEndXactFsObj *fsObj,
	PersistentFileSysState	state)
{
	XactInfoKind		infoKind;
	XactEntry 			xactEntry;

	FsObjEntry 	fsObjEntry;
	FsObjEntryData prevFsObjEntry;

	bool found;

	infoKind = 
		(isCommit ? XACT_INFOKIND_COMMIT : XACT_INFOKIND_ABORT);

	/*
	 * Find or Create Transaction Entry.
	 */
	xactEntry = 
		PersistentRecovery_FindOrCreateXactEntry(xid, &found);
	if (!found)
		xactEntry->infoKind = infoKind;
	else
		Assert(xactEntry->infoKind == infoKind);

	/*
	 * Create Persistent Change entry keyed on TID.
	 */
	fsObjEntry = 
		PersistentRecovery_FindOrCreateFsObjEntry(
									fsObj->fsObjName.type, 
									&fsObj->persistentTid, 
									&found);
	if (found)
	{
		memcpy(&prevFsObjEntry, fsObjEntry, sizeof(FsObjEntryData));
	}

	fsObjEntry->fsObjName = fsObj->fsObjName;
	fsObjEntry->relStorageMgr = fsObj->relStorageMgr;
	fsObjEntry->persistentSerialNum = fsObj->persistentSerialNum;
	fsObjEntry->xid = xid;
	fsObjEntry->state = state;

	if (found)
	{
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
			     "%s REDO: Overwritting %s with %s",
				 (isCommit ? "Commit" : "Abort"),
				 FsObjEntryToString(&prevFsObjEntry),
				 FsObjEntryToString(fsObjEntry));
	}
	else
	{
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
			     "%s REDO: Adding %s",
				 (isCommit ? "Commit" : "Abort"),
				 FsObjEntryToString(fsObjEntry));
	}
}

bool
PersistentRecovery_HandleXLogRec(
	XLogRecPtr		beginLoc,

	XLogRecPtr 		lsn,
	
	XLogRecord 		*record)
{
	RmgrId rmid = record->xl_rmid;

	if (rmid == RM_HEAP_ID)
	{
		RelFileNode relFileNode;
		
		if (!heap_getrelfilenode(record, &relFileNode))
			elog(ERROR, "No relfilenode");

		if (relFileNode.spcNode != GLOBALTABLESPACE_OID)
			return false;

		if (relFileNode.relNode != GpPersistentRelationNodeRelationId &&
			relFileNode.relNode != GpPersistentDatabaseNodeRelationId)
			return false;

		/*
		 * The XLOG record applies to one of our persistent relations.
		 */
		if (Debug_persistent_recovery_print)
		{
			StringInfoData buf;
		
			initStringInfo(&buf);
			appendStringInfo(&buf, "Persistent Relation REDO @ %s; LSN %s: ",
							 XLogLocationToString(&beginLoc),
							 XLogLocationToString2(&lsn));
			XLog_OutRec(&buf, record);
			appendStringInfo(&buf, " - ");
			RmgrTable[record->xl_rmid].rm_desc(&buf,
											   record->xl_info,
											   XLogRecGetData(record));
			elog(Debug_persistent_recovery_print_level, "%s", buf.data);
			pfree(buf.data);
		}
	}
	else if (rmid == RM_XACT_ID)
	{
		TransactionId xid;
		PersistentEndXactRecObjects persistentObjects;
		TransactionId *subXids;
		int subXidCount;
		XactInfoKind infoKind;
		int i;

		/*
		 * The XLOG record is for transaction module...
		 */
		if (Debug_persistent_recovery_print)
		{
			StringInfoData buf;
		
			initStringInfo(&buf);
			appendStringInfo(&buf, "Transaction Module REDO @ %s; LSN %s: ",
							 XLogLocationToString(&beginLoc),
							 XLogLocationToString2(&lsn));
			XLog_OutRec(&buf, record);
			appendStringInfo(&buf, " - ");
			RmgrTable[record->xl_rmid].rm_desc(&buf,
											   record->xl_info,
											   XLogRecGetData(record));
			elog(Debug_persistent_recovery_print_level, "%s", buf.data);
			pfree(buf.data);
		}

		if (!xact_redo_get_info(
							record,
							&infoKind,
							&xid,
							&persistentObjects,
							&subXids,
							&subXidCount))
			return false;

		if (infoKind == XACT_INFOKIND_COMMIT)
		{
			for (i = 0; i < persistentObjects.typed.createFsObjsCount; i++)
			{
				PersistentEndXactFsObj *fsObj = 
							&persistentObjects.typed.createFsObjs[i];

				PersistentRecovery_AddEndXactFsObj(
												xid,
												/* isCommit */ true,
												fsObj,
												PersistentFileSysState_Created);
			}
			for (i = 0; i < persistentObjects.typed.dropFsObjsCount; i++)
			{
				PersistentEndXactFsObj *fsObj = 
							&persistentObjects.typed.dropFsObjs[i];

				PersistentRecovery_AddEndXactFsObj(
												xid,
												/* isCommit */ true,
												fsObj,
												PersistentFileSysState_DropPending);
			}
		}
		else if (infoKind == XACT_INFOKIND_ABORT)
		{
			for (i = 0; i < persistentObjects.typed.createFsObjsCount; i++)
			{
				PersistentEndXactFsObj *fsObj = 
							&persistentObjects.typed.createFsObjs[i];

				PersistentRecovery_AddEndXactFsObj(
												xid,
												/* isCommit */ false,
												fsObj,
												PersistentFileSysState_AbortingCreate);
			}
		}
		else
		{
			Assert(infoKind == XACT_INFOKIND_PREPARE);

			if (Debug_persistent_recovery_print)
				elog(Debug_persistent_recovery_print_level, 
				     "Prepare REDO: Transaction %u",
					 xid);
		}
	}
	else
		return false;

	return true;
}

static void
PersistentRecovery_PrintXactAndFsObjs(void)
{
	HASH_SEQ_STATUS iterateStatus;
	XactEntry xactEntry;
	FsObjEntry fsObjEntry;

	if (xactHashTable == NULL)
		return;
	
	hash_seq_init(&iterateStatus, xactHashTable);

	while (true)
	{
		xactEntry = 
				(XactEntry)
						hash_seq_search(&iterateStatus);
		if (xactEntry == NULL)
			break;


		elog(Debug_persistent_recovery_print_level, 
		     "Summary REDO: Transaction %u (%s)",
			 xactEntry->xid,
			 (xactEntry->infoKind == XACT_INFOKIND_ABORT ? "Abort" :
			     xactEntry->infoKind == XACT_INFOKIND_COMMIT ? "Commit" :
				 	xactEntry->infoKind == XACT_INFOKIND_PREPARE ? "Prepare" :
						"None"));

		fsObjEntry = 
			(FsObjEntry) DoublyLinkedHead_First(
									offsetof(FsObjEntryData, xactLinks),
									&xactEntry->fsObjEntryList);
		while (fsObjEntry != NULL)
		{
			elog(Debug_persistent_recovery_print_level, 
			     "Summary REDO: %s",
				 FsObjEntryToString(fsObjEntry));

			fsObjEntry = 
				(FsObjEntry) DoublyLinkedHead_Next(
										offsetof(FsObjEntryData, xactLinks),
										&xactEntry->fsObjEntryList,
										fsObjEntry);
		}
	}
}

// -----------------------------------------------------------------------------


static void
PersistentRecovery_AddScanEntry(
	PersistentFsObjType 	fsObjType,

	ItemPointer				persistentTid,

	PersistentFileSysObjName	*fsObjName,
	
	PersistentFileSysState	state,
	
	TransactionId			parentXid,
	
	int64					persistentSerialNum)
{

	FsObjEntry fsObjEntry;
	bool found;

	if (state == PersistentFileSysState_Free)
	{
		fsObjEntry = 
			PersistentRecovery_FindOrCreateFsObjEntry(fsObjType, persistentTid, &found);

		if (found)
		{
			if (Debug_persistent_recovery_print)
				elog(Debug_persistent_recovery_print_level, 
				     "Scan REDO: Overwriting %s as free", 
					 FsObjEntryToString(fsObjEntry));

		}
		else
		{
			if (Debug_persistent_recovery_print)
				elog(Debug_persistent_recovery_print_level, 
				     "Scan REDO: Free entry with no end transaction work: %s",
					 FsObjEntryToString(fsObjEntry));
		}
		
		fsObjEntry->state = PersistentFileSysState_Free;
		
		fsObjEntry->updateNeeded = false;		// Already in terminal condition.

		return;
	}

	/*
	 * Create Persistent Change entry.
	 */
	fsObjEntry = 
		PersistentRecovery_FindOrCreateFsObjEntry(
										fsObjType,
										persistentTid,
										&found);
	if (!found)
	{
		fsObjEntry->fsObjName = *fsObjName;
		fsObjEntry->persistentSerialNum = persistentSerialNum;
		fsObjEntry->xid = parentXid;
		fsObjEntry->state = state;
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
			     "Scan REDO: Add %s", FsObjEntryToString(fsObjEntry));
		return;
	}

	if (fsObjEntry->persistentSerialNum == persistentSerialNum)
	{
		if (fsObjEntry->state < state)
		{
			if (Debug_persistent_recovery_print)
				elog(Debug_persistent_recovery_print_level, 
				     "Scan REDO: Newer %s than %s state, transaction %u",
					 FsObjEntryToString(fsObjEntry),
					 PersistentFileSysObjState_Name(state),
					 parentXid);

			fsObjEntry->state = state;
			fsObjEntry->xid = parentXid;
		}
		else if (fsObjEntry->state > state)
		{
			if (Debug_persistent_recovery_print)
				elog(Debug_persistent_recovery_print_level, 
				     "Scan REDO: Update %s to state '%s', transaction %u, serial number " INT64_FORMAT,
					 FsObjEntryToString(fsObjEntry),
					 PersistentFileSysObjState_Name(state),
					 parentXid,
					 persistentSerialNum);

			fsObjEntry->updateNeeded = true;
		}
	}
	else if (fsObjEntry->persistentSerialNum < persistentSerialNum)
	{
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
			     "Scan REDO: Overwriting obsolete %s with '%s', state '%s', transaction %u, serial number " INT64_FORMAT,
				 FsObjEntryToString(fsObjEntry),
				 PersistentFileSysObjName_TypeAndObjectName(fsObjName),
				 PersistentFileSysObjState_Name(state),
				 parentXid,
				 persistentSerialNum);

		fsObjEntry->fsObjName = *fsObjName;
		fsObjEntry->state = state;
		fsObjEntry->xid = parentXid;
		fsObjEntry->persistentSerialNum = persistentSerialNum;
	}
	else
	{
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
			     "Scan REDO: Wrong state %s", FsObjEntryToString(fsObjEntry));
	}
}

static int	persistentRecoveryCount = 0;

static void PersistentRecovery_ScanTuple(
	PersistentFsObjType 	fsObjType,
	ItemPointer 			persistentTid,
	int64					persistentSerialNum,
	Datum					*values)
{
	PersistentFileSysObjName	fsObjName;
	
	PersistentFileSysState	state;

	MirroredObjectExistenceState		mirrorExistenceState;	// UNDONE

	TransactionId			parentXid;
	int64					serialNum;

	GpPersistent_GetCommonValues(
							fsObjType,
							values,
							&fsObjName,
							&state,
							&mirrorExistenceState,
							&parentXid,
							&serialNum);
	Assert(serialNum == persistentSerialNum);

	PersistentRecovery_AddScanEntry(
							fsObjType,
							persistentTid,
							&fsObjName,
							state,
							parentXid,
							persistentSerialNum);

	persistentRecoveryCount++;
}

static void
PersistentRecovery_ScanType(
	PersistentFsObjType fsObjType)
{
	PersistentFileSysObjData *fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;

	PersistentStoreScan storeScan;

	Datum *values;
	 
	ItemPointerData			 persistentTid;
	int64					 persistentSerialNum;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_ScanType %s",
		     PersistentFileSysObjName_TypeName(fsObjType));

	persistentRecoveryCount = 0;

	PersistentFileSysObj_GetDataPtrs(
								fsObjType,
								&fileSysObjData,
								&fileSysObjSharedData);
		 
	values = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));
		 
	PersistentStore_BeginScan(
						&fileSysObjData->storeData,
						&fileSysObjSharedData->storeSharedData,
						&storeScan);

	while (PersistentStore_GetNext(
							&storeScan,
							values,
							&persistentTid,
							&persistentSerialNum))
	{
		PersistentRecovery_ScanTuple(
								fsObjType,
								&persistentTid,
								persistentSerialNum,
								values);
	}

	pfree(values);
	
	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Exiting PersistentRecovery_ScanType %s, count %d",
		     PersistentFileSysObjName_TypeName(fsObjType),
		     persistentRecoveryCount);
}

void
PersistentRecovery_Scan(void)
{
	PersistentFsObjType fsObjType;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_Scan");

	for (fsObjType = PersistentFsObjType_First; 
		 fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
	 	PersistentRecovery_ScanType(fsObjType);

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Exiting PersistentRecovery_Scan");
}

static void
PersistentRecovery_AttachFsObjTypeToXact(
	PersistentFsObjType fsObjType)
{
	HASH_SEQ_STATUS iterateStatus;
	FsObjEntry fsObjEntry;
	XactEntry xactEntry;
	bool found;

	Assert (fsObjHashTable != NULL);
	
	hash_seq_init(&iterateStatus, fsObjHashTable[fsObjType]);

	while (true)
	{
		fsObjEntry = 
				(FsObjEntry)
						hash_seq_search(&iterateStatus);
		if (fsObjEntry == NULL)
			break;

		if (fsObjEntry->xid == InvalidTransactionId)
			continue;

		xactEntry = 
			PersistentRecovery_FindOrCreateXactEntry(
											fsObjEntry->xid, 
											&found);
		if (!found)
			xactEntry->infoKind = XACT_INFOKIND_NONE;

		DoublyLinkedHead_AddFirst(
						offsetof(FsObjEntryData, xactLinks),
						&xactEntry->fsObjEntryList,
						fsObjEntry);
	}
}


void
PersistentRecovery_CrashAbort(void)
{
	PersistentFsObjType fsObjType;

	HASH_SEQ_STATUS iterateStatus;
	FsObjEntry fsObjEntry;
	XactEntry xactEntry;
	int xactCount;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_CrashAbort");

	if (fsObjHashTable == NULL)
	{
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
			     "Exiting PersistentRecovery_CrashAbort (no relation hash table)");
		return;
	}

	for (fsObjType = PersistentFsObjType_First; 
		 fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
		PersistentRecovery_AttachFsObjTypeToXact(fsObjType);

	if (Debug_persistent_recovery_print)
		PersistentRecovery_PrintXactAndFsObjs();

	hash_seq_init(&iterateStatus, xactHashTable);

	xactCount = 0;
	while (true)
	{
		int count;
		bool needsAbort;
		int i;
		int e;
		PersistentEndXactFsObj *endXactFsObjs;
		PersistentEndXactRecObjects persistentObjects;

		xactEntry = 
				(XactEntry)
						hash_seq_search(&iterateStatus);
		if (xactEntry == NULL)
			break;

		xactCount++;

		if (xactEntry->infoKind != XACT_INFOKIND_NONE)
		{
			/*
			 * If it is Prepared, we don't know yet what to do with the transaction.
			 *
			 * If it is known Committed or Aborted because we found an XLOG Commit or
			 * or Abort, we may have updates that will be handled later.
			 */
			continue;
		}

		/*
		 * We don't know the status of the transaction until we examine the
		 * persistent node states.
		 */

		count = DoublyLinkedHead_Count(&xactEntry->fsObjEntryList);

		needsAbort = false;

		fsObjEntry = 
			(FsObjEntry) DoublyLinkedHead_First(
									offsetof(FsObjEntryData, xactLinks),
									&xactEntry->fsObjEntryList);
									
		e = 0;
		while (fsObjEntry != NULL)
		{
			if (fsObjEntry->state == PersistentFileSysState_CreatePending)
			{
				needsAbort = true;
				
				if (Debug_persistent_recovery_print)
					elog(Debug_persistent_recovery_print_level, 
 						 "Needs-Abort REDO: (New state 'Aborting Create') %s",
 						 FsObjEntryToString(fsObjEntry));

				e++;
			}
			else if (fsObjEntry->state != PersistentFileSysState_Free)
			{
				Assert(!needsAbort);
				break;
			}

			fsObjEntry = 
				(FsObjEntry) DoublyLinkedHead_Next(
										offsetof(FsObjEntryData, xactLinks),
										&xactEntry->fsObjEntryList,
										fsObjEntry);
		}

		if (!needsAbort)
			continue;

		if (e == 0)
			continue;

		endXactFsObjs = 
			(PersistentEndXactFsObj*)
				palloc(count * sizeof(PersistentEndXactFsObj));
		
		fsObjEntry = 
			(FsObjEntry) DoublyLinkedHead_First(
									offsetof(FsObjEntryData, xactLinks),
									&xactEntry->fsObjEntryList);
		e = 0;
		while (fsObjEntry != NULL)
		{
			if (fsObjEntry->state == PersistentFileSysState_CreatePending)
			{
				endXactFsObjs[e].fsObjName = fsObjEntry->fsObjName;
				endXactFsObjs[e].persistentTid = fsObjEntry->key.persistentTid;
				endXactFsObjs[e].persistentSerialNum = fsObjEntry->persistentSerialNum;
				e++;
			}

			fsObjEntry = 
				(FsObjEntry) DoublyLinkedHead_Next(
										offsetof(FsObjEntryData, xactLinks),
										&xactEntry->fsObjEntryList,
										fsObjEntry);
		}

		PersistentEndXactRec_Init(&persistentObjects);
		PersistentEndXactRec_AddCreateFsObjs(
										&persistentObjects,
										endXactFsObjs, 
										count);

		RecordCrashTransactionAbortRecord(
									xactEntry->xid, 
									&persistentObjects);
		
		fsObjEntry = 
			(FsObjEntry) DoublyLinkedHead_First(
									offsetof(FsObjEntryData, xactLinks),
									&xactEntry->fsObjEntryList);

		for (i = 0; i < count; i++)
		{
			if (fsObjEntry->state == PersistentFileSysState_CreatePending)
			{
				if (Debug_persistent_recovery_print)
					elog(Debug_persistent_recovery_print_level, 
					     "MarkAbortingCreate REDO: %s", FsObjEntryToString(fsObjEntry));

				PersistentFileSysObj_MarkAbortingCreate(
								&fsObjEntry->fsObjName,
								&fsObjEntry->key.persistentTid,
								fsObjEntry->persistentSerialNum);
			}
			else if (fsObjEntry->state != PersistentFileSysState_Free)
				elog(ERROR, "Unexpected persistent file-system state: %d",
					 fsObjEntry->state);

			fsObjEntry->state = PersistentFileSysState_AbortingCreate;
			
			fsObjEntry = 
				(FsObjEntry) DoublyLinkedHead_Next(
										offsetof(FsObjEntryData, xactLinks),
										&xactEntry->fsObjEntryList,
										fsObjEntry);
		}

	}

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
			 "Exiting PersistentRecovery_CrashAbort (transaction count %d)", xactCount);
}

static void
PersistentRecovery_UpdateType(
	PersistentFsObjType fsObjType)
{
	HASH_SEQ_STATUS iterateStatus;
	FsObjEntry fsObjEntry;
	PersistentFileSysState	state;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_UpdateType %s",
		     PersistentFileSysObjName_TypeName(fsObjType));

	hash_seq_init(&iterateStatus, fsObjHashTable[fsObjType]);

	persistentRecoveryCount = 0;

	while (true)
	{
		fsObjEntry = 
				(FsObjEntry)
						hash_seq_search(&iterateStatus);
		if (fsObjEntry == NULL)
			break;

		if (fsObjEntry->updateNeeded)
		{
			persistentRecoveryCount++;

			if (Debug_persistent_recovery_print)
				elog(Debug_persistent_recovery_print_level, 
				     "End-Xact-Update-Needed REDO: %s", FsObjEntryToString(fsObjEntry));

			state = fsObjEntry->state;

			switch (state)
			{
				case PersistentFileSysState_Created:
					PersistentFileSysObj_Created(
									&fsObjEntry->fsObjName,
									&fsObjEntry->key.persistentTid,
									fsObjEntry->persistentSerialNum);
					break;
				
				case PersistentFileSysState_AbortingCreate:
					PersistentFileSysObj_MarkAbortingCreate(
									&fsObjEntry->fsObjName,
									&fsObjEntry->key.persistentTid,
									fsObjEntry->persistentSerialNum);
					break;

				case PersistentFileSysState_DropPending:
					PersistentFileSysObj_MarkDropPending(
									&fsObjEntry->fsObjName,
									&fsObjEntry->key.persistentTid,
									fsObjEntry->persistentSerialNum);
					break;
				default:
					elog(ERROR, "Unexpected persistent file-system state: %d",
						 state);
			}
		}
	}
	
	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Exiting PersistentRecovery_UpdateType %s, count %d",
		     PersistentFileSysObjName_TypeName(fsObjType),
		     persistentRecoveryCount);
}

void
PersistentRecovery_Update(void)
{
	PersistentFsObjType fsObjType;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_Update");

	if (fsObjHashTable == NULL)
	{
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
				 "Exiting PersistentRecovery_Update (no relation hash table)");
		return;
	}

	for (fsObjType = PersistentFsObjType_First; 
		 fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
	 	PersistentRecovery_UpdateType(fsObjType);
	
	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
			 "Exiting PersistentRecovery_Update");
}

static void
PersistentRecovery_DropType(
	PersistentFsObjType fsObjType)
{
	HASH_SEQ_STATUS iterateStatus;
	FsObjEntry fsObjEntry;
	PersistentFileSysState	state;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_DropType %s",
		     PersistentFileSysObjName_TypeName(fsObjType));

	hash_seq_init(&iterateStatus, fsObjHashTable[fsObjType]);

	persistentRecoveryCount = 0;

	while (true)
	{
		fsObjEntry = 
				(FsObjEntry)
						hash_seq_search(&iterateStatus);
		if (fsObjEntry == NULL)
			break;

		state = fsObjEntry->state;
		
		if (state == PersistentFileSysState_AbortingCreate ||
			state == PersistentFileSysState_DropPending)
		{
			persistentRecoveryCount++;

			if (Debug_persistent_recovery_print)
				elog(Debug_persistent_recovery_print_level, 
				     "Drop REDO: %s", FsObjEntryToString(fsObjEntry));

			// UNDONE: Determine the StorageManagerMirrorMode to pass in here.
			PersistentFileSysObj_DropObject(
							&fsObjEntry->fsObjName,
							fsObjEntry->relStorageMgr,
							/* relationName */ NULL,		// Ok to be NULL -- we don't know the name here.
							&fsObjEntry->key.persistentTid,
							fsObjEntry->persistentSerialNum,
							StorageManagerMirrorMode_Both,
							/* ignoreNonExistence */ true);
		}
	}

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Exiting PersistentRecovery_DropType %s, count %d",
		     PersistentFileSysObjName_TypeName(fsObjType),
		     persistentRecoveryCount);

}

void
PersistentRecovery_Drop(void)
{
	PersistentFsObjType fsObjType;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_Scan");

	for (fsObjType = PersistentFsObjType_First; 
		 fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
	 	PersistentRecovery_DropType(fsObjType);

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Exiting PersistentRecovery_Scan");
}

typedef struct RedoRelationEntry
{
	RelFileNode		relFileNode;

	// UNDONE: mirrorExistence
} RedoRelationEntry;

void
PersistentRecovery_SerializeRedoRelationFile(
	int redoRelationFile)
{
	HASH_SEQ_STATUS iterateStatus;
	FsObjEntry fsObjEntry;

	RedoRelationEntry redoRelationEntry;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_SerializeRedoRelationFile");

	hash_seq_init(
			&iterateStatus, 
			fsObjHashTable[PersistentFsObjType_RelationFile]);

	persistentRecoveryCount = 0;

	while (true)
	{
		int writeLen;

		fsObjEntry = 
				(FsObjEntry)
						hash_seq_search(&iterateStatus);
		if (fsObjEntry == NULL)
			break;

		redoRelationEntry.relFileNode = fsObjEntry->fsObjName.variant.rel.relFileNode;

		writeLen = write(redoRelationFile, &redoRelationEntry, sizeof(RedoRelationEntry));
		if (writeLen != sizeof(RedoRelationEntry))
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write redo relation file : %m")));
		}
		persistentRecoveryCount++;
		
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
				 "PersistentRecovery_SerializeRedoRelationFile: %u/%u/%u",
				 redoRelationEntry.relFileNode.spcNode,
				 redoRelationEntry.relFileNode.dbNode,
				 redoRelationEntry.relFileNode.relNode);
	}
	
	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Exiting PersistentRecovery_SerializeRedoRelationFile count %d",
		     persistentRecoveryCount);
}

void
PersistentRecovery_DeserializeRedoRelationFile(
	int redoRelationFile)
{
	RedoRelationEntry redoRelationEntry;

	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Entering PersistentRecovery_DeserializeRedoRelationFile");

	while(true)
	{
		int readLen;

		errno = 0;
		readLen = read(redoRelationFile, &redoRelationEntry, sizeof(RedoRelationEntry));

		if (readLen == 0)
			break;
		else if (readLen != sizeof(RedoRelationEntry) && errno == 0)
			elog(ERROR, "Bad redo relation entry length (expected %d and found %d)",
			     (int)sizeof(RedoRelationEntry), readLen);
		else if (errno != 0)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("error reading redo relation file: %m")));
		}
		
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
				"PersistentRecovery_DeserializeRedoRelationFile: %u/%u/%u",
				redoRelationEntry.relFileNode.spcNode,
				redoRelationEntry.relFileNode.dbNode,
				redoRelationEntry.relFileNode.relNode);
	}
	
	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
		     "Exiting PersistentRecovery_DeserializeRedoRelationFile count %d",
		     persistentRecoveryCount);
}
