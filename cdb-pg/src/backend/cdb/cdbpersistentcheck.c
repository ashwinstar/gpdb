/*-------------------------------------------------------------------------
 *
 * cdbpersistentcheck.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/palloc.h"

#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentcheck.h"

#include "storage/itemptr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"
#include "utils/fmgroids.h"
#include "utils/faultinjector.h"
#include "storage/smgr.h"
#include "storage/ipc.h"
#include "cdb/cdbglobalsequence.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentrecovery.h"

#define PTCHECK_LOG_LEVEL (debug_print_persistent_checks ? LOG : DEBUG5)

#define PTCHECK_RETURN_IF_DISABLED() \
{ \
	if (persistent_integrity_checks == false) \
	{ \
		return; \
	} \
}

typedef struct PT_GpPersistentRelationNode
{
	Oid                                 tablespaceOid;
	Oid                                 databaseOid;
	Oid                                 relfilenodeOid;
	int32                               segmentFileNum;
	PersistentFileSysRelStorageMgr      relationStorageManager;
	PersistentFileSysState              persistentState;
	int64                               createMirrorDataLossTrackingSessionNum;
	MirroredObjectExistenceState        mirrorExistenceState;
	MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
	bool                                mirrorBufpoolMarkedForScanIncrementalResync;
	int64                               mirrorBufpoolResyncChangedPageCount;
	XLogRecPtr                          mirrorBufpoolResyncCkptLoc;
	BlockNumber                         mirrorBufpoolResyncCkptBlockNum;
	int64                               mirrorAppendOnlyLossEof;
	int64                               mirrorAppendOnlyNewEof;
	PersistentFileSysRelBufpoolKind     relBufpoolKind;
	TransactionId                       parentXid;
	int64                               persistentSerialNum;
	ItemPointerData                     previousFreeTid;
} PT_GpPersistentRelationNode;


static inline void GpPersistentRelationNodeGetValues(Datum* values, PT_GpPersistentRelationNode *relnode)
{
	GpPersistentRelationNode_GetValues(
									values,
									&relnode->tablespaceOid,
									&relnode->databaseOid,
									&relnode->relfilenodeOid,
									&relnode->segmentFileNum,
									&relnode->relationStorageManager,
									&relnode->persistentState,
									&relnode->createMirrorDataLossTrackingSessionNum,
									&relnode->mirrorExistenceState,
									&relnode->mirrorDataSynchronizationState,
									&relnode->mirrorBufpoolMarkedForScanIncrementalResync,
									&relnode->mirrorBufpoolResyncChangedPageCount,
									&relnode->mirrorBufpoolResyncCkptLoc,
									&relnode->mirrorBufpoolResyncCkptBlockNum,
									&relnode->mirrorAppendOnlyLossEof,
									&relnode->mirrorAppendOnlyNewEof,
									&relnode->relBufpoolKind,
									&relnode->parentXid,
									&relnode->persistentSerialNum,
									&relnode->previousFreeTid);
}


typedef struct PT_GpPersistentDatabaseNode
{
	Oid                          tablespaceOid;
	Oid                          databaseOid;
	PersistentFileSysState       persistentState;
	int64                        createMirrorDataLossTrackingSessionNum;
	MirroredObjectExistenceState mirrorExistenceState;
	int32                        reserved;
	TransactionId                parentXid;
	int64                        persistentSerialNum;
	ItemPointerData              previousFreeTid;
} PT_GpPersistentDatabaseNode;

static inline void GpPersistentDatabaseNodeGetValues(Datum* values, PT_GpPersistentDatabaseNode *dbnode)
{
	GpPersistentDatabaseNode_GetValues(
									values,
									&dbnode->tablespaceOid,
									&dbnode->databaseOid,
									&dbnode->persistentState,
									&dbnode->createMirrorDataLossTrackingSessionNum,
									&dbnode->mirrorExistenceState,
									&dbnode->reserved,
									&dbnode->parentXid,
									&dbnode->persistentSerialNum,
									&dbnode->previousFreeTid);
}


typedef struct PT_GpPersistentTablespaceNode
{
	Oid                          filespaceOid;
	Oid                          tablespaceOid;
	PersistentFileSysState       persistentState;
	int64                        createMirrorDataLossTrackingSessionNum;
	MirroredObjectExistenceState mirrorExistenceState;
	int32                        reserved;
	TransactionId                parentXid;
	int64                        persistentSerialNum;
	ItemPointerData              previousFreeTid;
} PT_GpPersistentTablespaceNode;


static inline void GpPersistentTablespaceNodeGetValues(Datum* values, PT_GpPersistentTablespaceNode *tablenode)
{
	GpPersistentTablespaceNode_GetValues(
									values,
									&tablenode->filespaceOid,
									&tablenode->tablespaceOid,
									&tablenode->persistentState,
									&tablenode->createMirrorDataLossTrackingSessionNum,
									&tablenode->mirrorExistenceState,
									&tablenode->reserved,
									&tablenode->parentXid,
									&tablenode->persistentSerialNum,
									&tablenode->previousFreeTid);
}


/*
 * Function to check existence of entry in table with specified values.
 * If entry exist throw the error else just return fine.
 * 
 * Intention is to block any duplicate entries from geting IN,
 * hence must be called from every place trying to add entry to PT relation table.
 */
void PTCheck_BeforeAddingEntry( PersistentStoreData *storeData, Datum *values)
{
	PTCHECK_RETURN_IF_DISABLED();

	elog(PTCHECK_LOG_LEVEL, "PTCheck: Checking before adding entry to PT");

	int nKey = 0;
	bool allowDuplicates = false;
	bool status = true;
	ItemPointerData iptr;

	ScanKey key = (*storeData->scanKeyInitCallback)(values, &nKey);
	if (key == NULL)
	{
		return;
	}

	Datum *existing_values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));
	if (existing_values == NULL)
	{
		elog(LOG, "PTCheck: Failed to allocate memory for existing_values datastructure");
		return;
	}

	/*
	 * Lets scan the table and fetch the entry with the key
	 */
	Relation persistentRel = (*storeData->openRel)();

	HeapScanDesc scan = heap_beginscan(persistentRel, SnapshotNow, nKey, key);

	HeapTuple tuple = heap_getnext(scan, ForwardScanDirection);
	while (tuple != NULL)
	{
		PersistentStore_DeformTuple(storeData, persistentRel->rd_att, tuple, existing_values);

		allowDuplicates = (*storeData->allowDuplicateCallback)(existing_values, values);
		if (allowDuplicates == false)
		{
			/*
			 * Not expecting duplicate, MUST error-out then
			 */
			 status = false;
			 break;
		}

		(*storeData->printTupleCallback)(LOG, "PTCheck insert", &iptr, values); 
		(*storeData->printTupleCallback)(LOG, "PTCheck allowing with duplicate", &iptr, existing_values);
		
		/*
		 * Callback returned its exception case and allow duplicate for this,
		 * hence proceed forward to check
		 */ 
		tuple = heap_getnext(scan, ForwardScanDirection);
	}

	heap_endscan(scan);

	(*storeData->closeRel)(persistentRel);

	if (status == false)
	{
		(*storeData->printTupleCallback)(LOG, "PTCheck insert", &iptr, values); 
		(*storeData->printTupleCallback)(LOG, "PTCheck conflicts with duplicate", &iptr, existing_values);
		ereport(ERROR,
			(ERRCODE_INTERNAL_ERROR,
			 errmsg("PTCheck: Failed object entry already exist.")));
	}
	
	pfree(existing_values);
	pfree(key);
}

ScanKey Persistent_RelationScanKeyInit(Datum *values, int *nKeys)
{
	PT_GpPersistentRelationNode relnode;

	GpPersistentRelationNodeGetValues(values, &relnode);

	/* tablespace_oid, database_oid, relfilenode_oid and segment_file_num */
	*nKeys = 4;
	ScanKey key = palloc0(*nKeys * sizeof(ScanKeyData));
	if (key == NULL)
	{
		return NULL;
	}

	/*
	 * We needn't fill in sk_strategy or sk_subtype since these scankeys
	 * will never be passed to an index.
	 */
	ScanKeyInit(&key[0], Anum_gp_persistent_relation_node_tablespace_oid, InvalidStrategy, F_OIDEQ, (Datum) relnode.tablespaceOid);
	ScanKeyInit(&key[1], Anum_gp_persistent_relation_node_database_oid, InvalidStrategy, F_OIDEQ, (Datum) relnode.databaseOid);
	ScanKeyInit(&key[2], Anum_gp_persistent_relation_node_relfilenode_oid, InvalidStrategy, F_OIDEQ, (Datum) relnode.relfilenodeOid);
	ScanKeyInit(&key[3], Anum_gp_persistent_relation_node_segment_file_num, InvalidStrategy, F_INT4EQ, (Datum) relnode.segmentFileNum);

	return key;
}


/*
 * Any exceptions intended to allow duplicate entries need to be checked here
 * and for those specific cases returned TRUE and logged.
 * For all rest of the cases default is to return false to avoid duplicate entry.
 * Currently, this code is shared between realtion, database, tablespace and filespace.
 * If in future exceptions vary for the same can write different routine for the same.
 */
static inline bool 
Persistent_AllowDuplicateEntry( PersistentFileSysState old_persistentState,
								MirroredObjectExistenceState old_mirrorExistenceState,
								PersistentFileSysState new_persistentState,
								MirroredObjectExistenceState new_mirrorExistenceState )
{
	/*
	 * Currently, only one exception hence coidng with ifs, as not many exceptions are expected.
	 */
	if ((old_persistentState == PersistentFileSysState_AbortingCreate) &&
		(old_mirrorExistenceState == MirroredObjectExistenceState_OnlyMirrorDropRemains))
	{
		if ((new_persistentState == PersistentFileSysState_CreatePending) &&
			(new_mirrorExistenceState == MirroredObjectExistenceState_MirrorDownBeforeCreate))
		{
			return true;
		}
	}

	return false;
}

bool Persistent_RelationAllowDuplicateEntry(Datum *exist_values, Datum *new_values)
{
	PT_GpPersistentRelationNode old_relnode;
	PT_GpPersistentRelationNode new_relnode;

	GpPersistentRelationNodeGetValues(exist_values, &old_relnode);
	GpPersistentRelationNodeGetValues(new_values, &new_relnode);

	return Persistent_AllowDuplicateEntry(  old_relnode.persistentState,
	 										old_relnode.mirrorExistenceState,
											new_relnode.persistentState,
											new_relnode.mirrorExistenceState );
}

ScanKey Persistent_DatabaseScanKeyInit(Datum *values, int *nKeys)
{
	PT_GpPersistentDatabaseNode dbnode;

	GpPersistentDatabaseNodeGetValues(values, &dbnode);

	/* tablespace_oid and database_oid */
	*nKeys = 2;
	ScanKey key = palloc0(*nKeys * sizeof(ScanKeyData));
	if (key == NULL)
	{
		return NULL;
	}

	/*
	 * We needn't fill in sk_strategy or sk_subtype since these scankeys
	 * will never be passed to an index.
	 */
	ScanKeyInit(&key[0], Anum_gp_persistent_database_node_tablespace_oid, InvalidStrategy, F_OIDEQ, (Datum) dbnode.tablespaceOid);
	ScanKeyInit(&key[1], Anum_gp_persistent_database_node_database_oid, InvalidStrategy, F_OIDEQ, (Datum) dbnode.databaseOid);

	return key;
}

bool Persistent_DatabaseAllowDuplicateEntry(Datum *exist_values, Datum *new_values)
{
	/*
	 * Any exceptions intended to allow duplicate entries need to be checked here
	 * and for those specific cases returned TRUE and logged.
	 * For all rest of the cases default is to return false to avoid duplicate entry.
	 */

	/* For Database we expect no duplicates */
	return false;
}

ScanKey Persistent_TablespaceScanKeyInit(Datum *values, int *nKeys)
{
	PT_GpPersistentTablespaceNode tablespacenode;

	GpPersistentTablespaceNodeGetValues(values, &tablespacenode);

	/* filespace_oid and tablespace_oid */
	*nKeys = 2;
	ScanKey key = palloc0(*nKeys * sizeof(ScanKeyData));
	if (key == NULL)
	{
		return NULL;
	}

	/*
	 * We needn't fill in sk_strategy or sk_subtype since these scankeys
	 * will never be passed to an index.
	 */
	ScanKeyInit(&key[0], Anum_gp_persistent_tablespace_node_filespace_oid, InvalidStrategy, F_OIDEQ, (Datum) tablespacenode.filespaceOid );
	ScanKeyInit(&key[1], Anum_gp_persistent_tablespace_node_tablespace_oid, InvalidStrategy, F_OIDEQ, (Datum) tablespacenode.tablespaceOid );

	return key;
}

bool Persistent_TablespaceAllowDuplicateEntry(Datum *exist_values, Datum *new_values)
{
	/*
	 * Any exceptions intended to allow duplicate entries need to be checked here
	 * and for those specific cases returned TRUE and logged.
	 * For all rest of the cases default is to return false to avoid duplicate entry.
	 */


	/* For TableSpace we expect no duplicates */
	return false;
}

ScanKey Persistent_FilespaceScanKeyInit(Datum *values, int *nKeys)
{
	return NULL;
}

bool Persistent_FilespaceAllowDuplicateEntry(Datum *exist_values, Datum *new_values)
{
	/*
	 * Any exceptions intended to allow duplicate entries need to be checked here
	 * and for those specific cases returned TRUE and logged.
	 * For all rest of the cases default is to return false to avoid duplicate entry.
	 */

	 return false;
}
