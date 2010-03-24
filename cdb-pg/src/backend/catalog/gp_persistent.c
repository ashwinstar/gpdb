/*-------------------------------------------------------------------------
 *
 * gp_persistent.c
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

#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbsharedoidsearch.h"
#include "storage/itemptr.h"
#include "storage/shmem.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"

void GpPersistentRelationNode_GetValues(
	Datum							*values,

	Oid 							*tablespaceOid,
	Oid 							*databaseOid,
	Oid 							*relfilenodeOid,
	int32							*segmentFileNum,
	PersistentFileSysRelStorageMgr	*relationStorageManager,
	PersistentFileSysState			*persistentState,
	MirroredObjectExistenceState	*mirrorExistenceState,
	MirroredRelDataSynchronizationState *mirrorDataSynchronizationState,
	bool							*mirrorBufpoolVacuumed,
	XLogRecPtr						*mirrorBufpoolResyncCkptLoc,
	BlockNumber						*mirrorBufpoolResyncCkptBlockNum,
	int64							*mirrorAppendOnlyLossEof,
	int64							*mirrorAppendOnlyNewEof,
	TransactionId					*parentXid,
	int64							*serialNum,
	ItemPointerData 				*previousFreeTid)
{
	*tablespaceOid = DatumGetObjectId(values[Anum_gp_persistent_relation_node_tablespace_oid - 1]);

	*databaseOid = DatumGetObjectId(values[Anum_gp_persistent_relation_node_database_oid - 1]);

	*relfilenodeOid = DatumGetObjectId(values[Anum_gp_persistent_relation_node_relfilenode_oid - 1]);

	*segmentFileNum = DatumGetInt32(values[Anum_gp_persistent_relation_node_segment_file_num - 1]);

	*relationStorageManager = (PersistentFileSysRelStorageMgr)DatumGetInt16(values[Anum_gp_persistent_relation_node_relation_storage_manager - 1]);

	*persistentState = (PersistentFileSysState)DatumGetInt16(values[Anum_gp_persistent_relation_node_persistent_state - 1]);

	*mirrorExistenceState = (MirroredObjectExistenceState)DatumGetInt16(values[Anum_gp_persistent_relation_node_mirror_existence_state - 1]);

	*mirrorDataSynchronizationState = (MirroredRelDataSynchronizationState)DatumGetInt16(values[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1]);

	*mirrorBufpoolVacuumed = DatumGetBool(values[Anum_gp_persistent_relation_node_mirror_bufpool_vacuumed - 1]);

	*mirrorBufpoolResyncCkptLoc = *((XLogRecPtr*) DatumGetPointer(values[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_loc - 1]));

	*mirrorBufpoolResyncCkptBlockNum = (BlockNumber)DatumGetInt32(values[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_block_num - 1]);

	*mirrorAppendOnlyLossEof = DatumGetInt64(values[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1]);

	*mirrorAppendOnlyNewEof = DatumGetInt64(values[Anum_gp_persistent_relation_node_mirror_append_only_new_eof - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_relation_node_parent_xid - 1]);

	*serialNum = DatumGetInt64(values[Anum_gp_persistent_relation_node_persistent_serial_num - 1]);

	*previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_relation_node_previous_free_tid - 1]));

}

void GpPersistentRelationNode_SetDatumValues(
	Datum							*values,

	Oid 							tablespaceOid,
	Oid 							databaseOid,
	Oid 							relfilenodeOid,
	int32							segmentFileNum,
	PersistentFileSysRelStorageMgr	relationStorageManager,
	PersistentFileSysState			persistentState,
	MirroredObjectExistenceState	mirrorExistenceState,
	MirroredRelDataSynchronizationState mirrorDataSynchronizationState,
	bool							mirrorBufpoolVacuumed,
	XLogRecPtr						*mirrorBufpoolResyncCkptLoc,
	BlockNumber						mirrorBufpoolResyncCkptBlockNum,
	int64							mirrorAppendOnlyLossEof,
	int64							mirrorAppendOnlyNewEof,
	TransactionId					parentXid,
	int64							serialNum,
	ItemPointerData 				*previousFreeTid)
{
	values[Anum_gp_persistent_relation_node_tablespace_oid - 1] = 
									ObjectIdGetDatum(tablespaceOid);
	values[Anum_gp_persistent_relation_node_database_oid - 1] = 
									ObjectIdGetDatum(databaseOid);
	values[Anum_gp_persistent_relation_node_relfilenode_oid - 1] = 
									ObjectIdGetDatum(relfilenodeOid);

	values[Anum_gp_persistent_relation_node_segment_file_num - 1] = 
									Int32GetDatum(segmentFileNum);

	values[Anum_gp_persistent_relation_node_relation_storage_manager - 1] = 
									Int16GetDatum(relationStorageManager);

	values[Anum_gp_persistent_relation_node_persistent_state - 1] = 
									Int16GetDatum(persistentState);

	values[Anum_gp_persistent_relation_node_mirror_existence_state - 1] = 
									Int16GetDatum(mirrorExistenceState);

	values[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1] = 
									Int16GetDatum(mirrorDataSynchronizationState);

	values[Anum_gp_persistent_relation_node_mirror_bufpool_vacuumed - 1] = 
									BoolGetDatum(mirrorBufpoolVacuumed);

	values[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_loc - 1] =
									PointerGetDatum(mirrorBufpoolResyncCkptLoc);

	values[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_block_num - 1] = 
									Int32GetDatum(mirrorBufpoolResyncCkptBlockNum);

	values[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1] = 
									Int64GetDatum(mirrorAppendOnlyLossEof);

	values[Anum_gp_persistent_relation_node_mirror_append_only_new_eof - 1] = 
									Int64GetDatum(mirrorAppendOnlyNewEof);

	values[Anum_gp_persistent_relation_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_relation_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_relation_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}

void GpPersistentDatabaseNode_GetValues(
	Datum							*values,

	Oid 							*tablespaceOid,
	Oid 							*databaseOid,
	PersistentFileSysState			*persistentState,
	MirroredObjectExistenceState	*mirrorExistenceState,
	TransactionId					*parentXid,
	int64							*serialNum,
	ItemPointerData 				*previousFreeTid)
{
    *tablespaceOid = DatumGetObjectId(values[Anum_gp_persistent_database_node_tablespace_oid - 1]);

    *databaseOid = DatumGetObjectId(values[Anum_gp_persistent_database_node_database_oid - 1]);

    *persistentState = DatumGetInt16(values[Anum_gp_persistent_database_node_persistent_state - 1]);

    *mirrorExistenceState = DatumGetInt16(values[Anum_gp_persistent_database_node_mirror_existence_state - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_database_node_parent_xid - 1]);

    *serialNum = DatumGetInt64(values[Anum_gp_persistent_database_node_persistent_serial_num - 1]);

    *previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_database_node_previous_free_tid - 1]));

}

void GpPersistentDatabaseNode_SetDatumValues(
	Datum							*values,

	Oid 							tablespaceOid,
	Oid 							databaseOid,
	PersistentFileSysState			persistentState,
	MirroredObjectExistenceState	mirrorExistenceState,
	TransactionId					parentXid,
	int64							serialNum,
	ItemPointerData 				*previousFreeTid)
{
	values[Anum_gp_persistent_database_node_tablespace_oid - 1] = 
									ObjectIdGetDatum(tablespaceOid);
	values[Anum_gp_persistent_database_node_database_oid - 1] = 
									ObjectIdGetDatum(databaseOid);

	values[Anum_gp_persistent_database_node_persistent_state - 1] = 
									Int16GetDatum(persistentState);

	values[Anum_gp_persistent_database_node_mirror_existence_state - 1] = 
									Int16GetDatum(mirrorExistenceState);

	values[Anum_gp_persistent_database_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_database_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_database_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}

void GpPersistentTablespaceNode_GetValues(
	Datum							*values,

	Oid 							*filespaceOid,
	Oid 							*tablespaceOid,
	PersistentFileSysState			*persistentState,
	MirroredObjectExistenceState	*mirrorExistenceState,
	TransactionId					*parentXid,
	int64							*serialNum,
	ItemPointerData 				*previousFreeTid)
{
    *filespaceOid = DatumGetObjectId(values[Anum_gp_persistent_tablespace_node_filespace_oid - 1]);

    *tablespaceOid = DatumGetObjectId(values[Anum_gp_persistent_tablespace_node_tablespace_oid - 1]);

    *persistentState = DatumGetInt16(values[Anum_gp_persistent_tablespace_node_persistent_state - 1]);

    *mirrorExistenceState = DatumGetInt16(values[Anum_gp_persistent_tablespace_node_mirror_existence_state - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_tablespace_node_parent_xid - 1]);

    *serialNum = DatumGetInt64(values[Anum_gp_persistent_tablespace_node_persistent_serial_num - 1]);

    *previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_tablespace_node_previous_free_tid - 1]));

}

void GpPersistentTablespaceNode_SetDatumValues(
	Datum							*values,

	Oid 							filespaceOid,
	Oid 							tablespaceOid,
	PersistentFileSysState			persistentState,
	MirroredObjectExistenceState	mirrorExistenceState,
	TransactionId					parentXid,
	int64							serialNum,
	ItemPointerData 				*previousFreeTid)
{
	values[Anum_gp_persistent_tablespace_node_filespace_oid - 1] = 
									ObjectIdGetDatum(filespaceOid);

	values[Anum_gp_persistent_tablespace_node_tablespace_oid - 1] = 
									ObjectIdGetDatum(tablespaceOid);

	values[Anum_gp_persistent_tablespace_node_persistent_state - 1] = 
									Int16GetDatum(persistentState);

	values[Anum_gp_persistent_tablespace_node_mirror_existence_state - 1] = 
									Int16GetDatum(mirrorExistenceState);

	values[Anum_gp_persistent_tablespace_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_tablespace_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_tablespace_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}

void GpPersistentFilespaceNode_GetValues(
	Datum							*values,

	Oid 							*filespaceOid,
	int16							*dbId1,
	char							locationBlankPadded1[FilespaceLocationBlankPaddedWithNullTermLen],
	int16							*dbId2,
	char							locationBlankPadded2[FilespaceLocationBlankPaddedWithNullTermLen],
	PersistentFileSysState			*persistentState,
	MirroredObjectExistenceState	*mirrorExistenceState,
	TransactionId					*parentXid,
	int64							*serialNum,
	ItemPointerData 				*previousFreeTid)
{
	char *locationPtr;
	int locationLen;

    *filespaceOid = DatumGetObjectId(values[Anum_gp_persistent_filespace_node_filespace_oid - 1]);

    *dbId1 = DatumGetInt16(values[Anum_gp_persistent_filespace_node_db_id_1 - 1]);

	locationPtr = TextDatumGetCString(values[Anum_gp_persistent_filespace_node_location_1 - 1]);;
	locationLen = strlen(locationPtr);
	if (locationLen != FilespaceLocationBlankPaddedWithNullTermLen - 1)
		elog(ERROR, "Expected filespace location 1 to be %d characters and found %d",
			 FilespaceLocationBlankPaddedWithNullTermLen - 1,
			 locationLen);
			 
	memcpy(locationBlankPadded1, locationPtr, FilespaceLocationBlankPaddedWithNullTermLen);

    *dbId2 = DatumGetInt16(values[Anum_gp_persistent_filespace_node_db_id_2 - 1]);

	locationPtr = TextDatumGetCString(values[Anum_gp_persistent_filespace_node_location_2 - 1]);
	locationLen = strlen(locationPtr);
	if (locationLen != FilespaceLocationBlankPaddedWithNullTermLen - 1)
		elog(ERROR, "Expected filespace location 2 to be %d characters and found %d",
			 FilespaceLocationBlankPaddedWithNullTermLen - 1,
			 locationLen);
			 
	memcpy(locationBlankPadded2, locationPtr, FilespaceLocationBlankPaddedWithNullTermLen);

    *persistentState = DatumGetInt16(values[Anum_gp_persistent_filespace_node_persistent_state - 1]);

    *mirrorExistenceState = DatumGetInt16(values[Anum_gp_persistent_filespace_node_mirror_existence_state - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_filespace_node_parent_xid - 1]);

    *serialNum = DatumGetInt64(values[Anum_gp_persistent_filespace_node_persistent_serial_num - 1]);

    *previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_filespace_node_previous_free_tid - 1]));

}

void GpPersistentFilespaceNode_SetDatumValues(
	Datum							*values,

	Oid 							filespaceOid,
	int16							dbId1,
	char							locationBlankPadded1[FilespaceLocationBlankPaddedWithNullTermLen],
	int16							dbId2,
	char							locationBlankPadded2[FilespaceLocationBlankPaddedWithNullTermLen],
	PersistentFileSysState			persistentState,
	MirroredObjectExistenceState	mirrorExistenceState,
	TransactionId					parentXid,
	int64							serialNum,
	ItemPointerData 				*previousFreeTid)
{
	int locationLen;

	locationLen = strlen(locationBlankPadded1);
	if (locationLen != FilespaceLocationBlankPaddedWithNullTermLen - 1)
		elog(ERROR, "Expected filespace location 1 to be %d characters and found %d",
			 FilespaceLocationBlankPaddedWithNullTermLen - 1,
			 locationLen);

	locationLen = strlen(locationBlankPadded2);
	if (locationLen != FilespaceLocationBlankPaddedWithNullTermLen - 1)
		elog(ERROR, "Expected filespace location 2 to be %d characters and found %d",
			 FilespaceLocationBlankPaddedWithNullTermLen - 1,
			 locationLen);

	values[Anum_gp_persistent_filespace_node_filespace_oid - 1] = 
									ObjectIdGetDatum(filespaceOid);

	values[Anum_gp_persistent_filespace_node_db_id_1 - 1] = 
									Int16GetDatum(dbId1);

	values[Anum_gp_persistent_filespace_node_location_1 - 1] =
									CStringGetTextDatum(locationBlankPadded1);

	values[Anum_gp_persistent_filespace_node_db_id_2 - 1] = 
									Int16GetDatum(dbId2);

	values[Anum_gp_persistent_filespace_node_location_2 - 1] =
									CStringGetTextDatum(locationBlankPadded2);

	values[Anum_gp_persistent_filespace_node_persistent_state - 1] = 
									Int16GetDatum(persistentState);

	values[Anum_gp_persistent_filespace_node_mirror_existence_state - 1] = 
									Int16GetDatum(mirrorExistenceState);

	values[Anum_gp_persistent_filespace_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_filespace_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_filespace_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}

void GpPersistent_GetCommonValues(
	PersistentFsObjType 			fsObjType,
	Datum							*values,

	PersistentFileSysObjName		*fsObjName,
	PersistentFileSysState			*persistentState,
	MirroredObjectExistenceState	*mirrorExistenceState,
	TransactionId					*parentXid,
	int64							*persistentSerialNum)
{
	ItemPointerData previousFreeTid;

	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = fsObjType;

	switch (fsObjType)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = &fsObjName->variant.rel.relFileNode;
			int32 segmentFileNum;	// UNDONE
			PersistentFileSysRelStorageMgr	relationStorageManager;
			MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
			bool							mirrorBufpoolVacuumed;
			XLogRecPtr						mirrorBufpoolResyncCkptLoc;
			BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
			int64							mirrorAppendOnlyLossEof;
			int64							mirrorAppendOnlyNewEof;

			GpPersistentRelationNode_GetValues(
											values,
											&relFileNode->spcNode,
											&relFileNode->dbNode,
											&relFileNode->relNode,
											&segmentFileNum,
											&relationStorageManager,
											persistentState,
											mirrorExistenceState,
											&mirrorDataSynchronizationState,
											&mirrorBufpoolVacuumed,
											&mirrorBufpoolResyncCkptLoc,
											&mirrorBufpoolResyncCkptBlockNum,
											&mirrorAppendOnlyLossEof,
											&mirrorAppendOnlyNewEof,
											parentXid,
											persistentSerialNum,
											&previousFreeTid);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		{
			DbDirNode *dbDirNode = &fsObjName->variant.dbDirNode;

			GpPersistentDatabaseNode_GetValues(
									values,
									&dbDirNode->tablespace,
									&dbDirNode->database,
									persistentState,
									mirrorExistenceState,
									parentXid,
									persistentSerialNum,
									&previousFreeTid);
		}
		break;

	case PersistentFsObjType_TablespaceDir:
		{
			Oid tablespaceOid;
			Oid filespaceOid;

			GpPersistentTablespaceNode_GetValues(
											values,
											&filespaceOid,
											&tablespaceOid,
											persistentState,
											mirrorExistenceState,
											parentXid,
											persistentSerialNum,
											&previousFreeTid);

			PersistentFileSysObjName_SetTablespaceDir(
												fsObjName, 
												tablespaceOid);
		}
	break;
	
	case PersistentFsObjType_FilespaceDir:
		{
			Oid filespaceOid;
			int16 dbId1;
			char locationBlankPadded1[FilespaceLocationBlankPaddedWithNullTermLen];
			int16 dbId2;
			char locationBlankPadded2[FilespaceLocationBlankPaddedWithNullTermLen];

			GpPersistentFilespaceNode_GetValues(
											values,
											&filespaceOid,
											&dbId1,
											locationBlankPadded1,
											&dbId2,
											locationBlankPadded2,
											persistentState,
											mirrorExistenceState,
											parentXid,
											persistentSerialNum,
											&previousFreeTid);

			PersistentFileSysObjName_SetFilespaceDir(
												fsObjName, 
												filespaceOid);
		}
	break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjType);
	}
		
}

