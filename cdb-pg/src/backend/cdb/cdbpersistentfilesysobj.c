/*-------------------------------------------------------------------------
 *
 * cdbpersistentfilesysobj.c
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

#include "access/persistentfilesysobjname.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbmirroredfilesysobj.h"

#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "cdb/cdbmirroredbufferpool.h"
#include "cdb/cdbmirroredappendonly.h"

#include "storage/itemptr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"

typedef struct PersistentFileSysObjPrivateSharedMemory
{
	bool	needInitScan;
} PersistentFileSysObjPrivateSharedMemory;

static PersistentFileSysObjPrivateSharedMemory *persistentFileSysObjPrivateSharedData = NULL;

/*
 * This HUGE MAGIC DEFINE expands into module globals and two routines:
 *     PersistentFileSysObj_RelationOpenShared
 *     PersistentFileSysObj_RelationClose
 * It allows for opening the relation without going through pg_class, etc.
 */
static FormData_pg_class 
			PersistentFileSysObj_RelationPgClass = 
							{Class_gp_persistent_relation_node};

static FormData_pg_attribute 
			PersistentFileSysObj_RelationAttrArray[Natts_gp_persistent_relation_node] =
							{Schema_gp_persistent_relation_node};

static Form_pg_attribute 
			PersistentFileSysObj_RelationAttrPtrArray[Natts_gp_persistent_relation_node];

DirectOpenDefine(PersistentFileSysObj_Relation,\
PersistentFileSysObj_RelationPgClass,Natts_gp_persistent_relation_node,\
PersistentFileSysObj_RelationAttrArray,PersistentFileSysObj_RelationAttrPtrArray,\
GpPersistentRelationNodeRelationId,GP_PERSISTENT_RELATION_NODE_OID,false,false);


/*
 * This HUGE MAGIC DEFINE expands into module globals and two routines:
 *     PersistentFileSysObj_DatabaseOpenShared
 *     PersistentFileSysObj_DatabaseClose
 * It allows for opening the relation without going through pg_class, etc.
 */
static FormData_pg_class 
			PersistentFileSysObj_DatabasePgClass = 
							{Class_gp_persistent_database_node};

static FormData_pg_attribute 
			PersistentFileSysObj_DatabaseAttrArray[Natts_gp_persistent_database_node] =
							{Schema_gp_persistent_database_node};

static Form_pg_attribute 
			PersistentFileSysObj_DatabaseAttrPtrArray[Natts_gp_persistent_database_node];

DirectOpenDefine(PersistentFileSysObj_Database,\
PersistentFileSysObj_DatabasePgClass,Natts_gp_persistent_database_node,\
PersistentFileSysObj_DatabaseAttrArray,PersistentFileSysObj_DatabaseAttrPtrArray,\
GpPersistentDatabaseNodeRelationId,GP_PERSISTENT_DATABASE_NODE_OID,false,false);

/*
 * This HUGE MAGIC DEFINE expands into module globals and two routines:
 *     PersistentFileSysObj_TablespaceOpenShared
 *     PersistentFileSysObj_TablespaceClose
 * It allows for opening the relation without going through pg_class, etc.
 */
static FormData_pg_class 
			PersistentFileSysObj_TablespacePgClass = 
							{Class_gp_persistent_tablespace_node};

static FormData_pg_attribute 
			PersistentFileSysObj_TablespaceAttrArray[Natts_gp_persistent_tablespace_node] =
							{Schema_gp_persistent_tablespace_node};

static Form_pg_attribute 
			PersistentFileSysObj_TablespaceAttrPtrArray[Natts_gp_persistent_tablespace_node];

DirectOpenDefine(PersistentFileSysObj_Tablespace,\
PersistentFileSysObj_TablespacePgClass,Natts_gp_persistent_tablespace_node,\
PersistentFileSysObj_TablespaceAttrArray,PersistentFileSysObj_TablespaceAttrPtrArray,\
GpPersistentTablespaceNodeRelationId,GP_PERSISTENT_TABLESPACE_NODE_OID,false,false);


/*
 * This HUGE MAGIC DEFINE expands into module globals and two routines:
 *     PersistentFileSysObj_FilespaceOpenShared
 *     PersistentFileSysObj_FilespaceClose
 * It allows for opening the relation without going through pg_class, etc.
 */
static FormData_pg_class 
			PersistentFileSysObj_FilespacePgClass = 
							{Class_gp_persistent_filespace_node};

static FormData_pg_attribute 
			PersistentFileSysObj_FilespaceAttrArray[Natts_gp_persistent_filespace_node] =
							{Schema_gp_persistent_filespace_node};

static Form_pg_attribute 
			PersistentFileSysObj_FilespaceAttrPtrArray[Natts_gp_persistent_filespace_node];

DirectOpenDefine(PersistentFileSysObj_Filespace,\
PersistentFileSysObj_FilespacePgClass,Natts_gp_persistent_filespace_node,\
PersistentFileSysObj_FilespaceAttrArray,PersistentFileSysObj_FilespaceAttrPtrArray,\
GpPersistentFilespaceNodeRelationId,GP_PERSISTENT_FILESPACE_NODE_OID,false,false);

void PersistentFileSysObj_InitShared(
	PersistentFileSysObjSharedData 	*fileSysObjSharedData)
{
	PersistentStore_InitShared(&fileSysObjSharedData->storeSharedData);
}

static PersistentFileSysObjData* fileSysObjDataArray[CountPersistentFsObjType];

static PersistentFileSysObjSharedData* fileSysObjSharedDataArray[CountPersistentFsObjType];

static void PersistentFileSysObj_InitOurs(
	PersistentFileSysObjData 	*fileSysObjData,
	
	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	PersistentFsObjType			fsObjType,

	int 						attNumPersistentState,

	int 						attNumMirrorExistenceState,

	int 						attNumParentXid)
{
	if (fsObjType < PersistentFsObjType_First ||
		fsObjType > PersistentFsObjType_Last)
		elog(ERROR, "Persistent file-system object type %d out-of-range",
		     fsObjType);
	
	fileSysObjDataArray[fsObjType] = fileSysObjData;
	fileSysObjSharedDataArray[fsObjType] = fileSysObjSharedData;

	fileSysObjData->fsObjType = fsObjType;
	fileSysObjData->attNumPersistentState = attNumPersistentState;
	fileSysObjData->attNumMirrorExistenceState = attNumMirrorExistenceState;
	fileSysObjData->attNumParentXid = attNumParentXid;
}

void PersistentFileSysObj_Init(
	PersistentFileSysObjData 			*fileSysObjData,
	
	PersistentFileSysObjSharedData 		*fileSysObjSharedData,

	PersistentFsObjType					fsObjType,

	PersistentStoreScanTupleCallback	scanTupleCallback)
{
	PersistentStoreData *storeData = &fileSysObjData->storeData;

	fileSysObjData->fsObjType = fsObjType;

	switch (fsObjType)
	{
	case PersistentFsObjType_RelationFile:
		PersistentStore_Init(
						storeData,
						fsObjType,
						GpGlobalSequence_PersistentRelation,
						PersistentFileSysObj_RelationOpenShared,
						PersistentFileSysObj_RelationClose,
						scanTupleCallback,
						Natts_gp_persistent_relation_node,
						Anum_gp_persistent_relation_node_persistent_serial_num,
						Anum_gp_persistent_relation_node_previous_free_tid);
		PersistentFileSysObj_InitOurs(
						fileSysObjData,
						fileSysObjSharedData,
						fsObjType,
						Anum_gp_persistent_relation_node_persistent_state,
						Anum_gp_persistent_relation_node_mirror_existence_state,
						Anum_gp_persistent_relation_node_parent_xid);
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentStore_Init(
						storeData,
						fsObjType,
						GpGlobalSequence_PersistentDatabase,
						PersistentFileSysObj_DatabaseOpenShared,
						PersistentFileSysObj_DatabaseClose,
						scanTupleCallback,
						Natts_gp_persistent_database_node,
						Anum_gp_persistent_database_node_persistent_serial_num,
						Anum_gp_persistent_database_node_previous_free_tid);
		PersistentFileSysObj_InitOurs(
						fileSysObjData,
						fileSysObjSharedData,
						fsObjType,
						Anum_gp_persistent_database_node_persistent_state,
						Anum_gp_persistent_database_node_mirror_existence_state,
						Anum_gp_persistent_database_node_parent_xid);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentStore_Init(
						storeData,
						fsObjType,
						GpGlobalSequence_PersistentTablespace,
						PersistentFileSysObj_TablespaceOpenShared,
						PersistentFileSysObj_TablespaceClose,
						scanTupleCallback,
						Natts_gp_persistent_tablespace_node,
						Anum_gp_persistent_tablespace_node_persistent_serial_num,
						Anum_gp_persistent_tablespace_node_previous_free_tid);
		PersistentFileSysObj_InitOurs(
						fileSysObjData,
						fileSysObjSharedData,
						fsObjType,
						Anum_gp_persistent_tablespace_node_persistent_state,
						Anum_gp_persistent_tablespace_node_mirror_existence_state,
						Anum_gp_persistent_tablespace_node_parent_xid);
		break;
	
	case PersistentFsObjType_FilespaceDir:
		PersistentStore_Init(
						storeData,
						fsObjType,
						GpGlobalSequence_PersistentFilespace,
						PersistentFileSysObj_FilespaceOpenShared,
						PersistentFileSysObj_FilespaceClose,
						scanTupleCallback,
						Natts_gp_persistent_filespace_node,
						Anum_gp_persistent_filespace_node_persistent_serial_num,
						Anum_gp_persistent_filespace_node_previous_free_tid);
		PersistentFileSysObj_InitOurs(
						fileSysObjData,
						fileSysObjSharedData,
						fsObjType,
						Anum_gp_persistent_filespace_node_persistent_state,
						Anum_gp_persistent_filespace_node_mirror_existence_state,
						Anum_gp_persistent_filespace_node_parent_xid);
		break;
	
	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjType);
	}
}

void PersistentFileSysObj_GetDataPtrs(
	PersistentFsObjType				fsObjType,
	
	PersistentFileSysObjData		**fileSysObjData,
	
	PersistentFileSysObjSharedData	**fileSysObjSharedData)
{
	if (fsObjType < PersistentFsObjType_First ||
		fsObjType > PersistentFsObjType_Last)
		elog(PANIC, "Persistent file-system object type %d out-of-range",
		     fsObjType);

	*fileSysObjData = fileSysObjDataArray[fsObjType];
	if (*fileSysObjData == NULL)
		elog(PANIC, "Persistent file-system object type %d memory not initialized",
		     fsObjType);
	if ((*fileSysObjData)->fsObjType != fsObjType)
		elog(PANIC, "Persistent file-system data's type %d doesn't match expected %d",
		     (*fileSysObjData)->fsObjType, fsObjType);
		
	*fileSysObjSharedData = fileSysObjSharedDataArray[fsObjType];
	if (*fileSysObjSharedData == NULL)
		elog(PANIC, "Persistent file-system object type %d shared-memory not initialized",
		     fsObjType);
}
	
void PersistentFileSysObj_InitScan(void)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFsObjType				fsObjType;

	PersistentFileSysObjData		*fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;
	
	if (persistentFileSysObjPrivateSharedData == NULL)
		elog(PANIC, "Persistent File-System Object shared-memory not initialized");

	if (!persistentFileSysObjPrivateSharedData->needInitScan)
		return;
	
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	// Re-test.
	if (!persistentFileSysObjPrivateSharedData->needInitScan)
	{
		MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
		return;
	}

	for (fsObjType = PersistentFsObjType_First; 
		 fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
	{
		 PersistentFileSysObj_GetDataPtrs(
									 fsObjType,
									 &fileSysObjData,
									 &fileSysObjSharedData);
		 PersistentStore_InitScanUnderLock(
						 &fileSysObjData->storeData,
						 &fileSysObjSharedData->storeSharedData);
	}

	 persistentFileSysObjPrivateSharedData->needInitScan = false;

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
}

void PersistentFileSysObj_FlushXLog(void)
{
	PersistentStore_FlushXLog();
}

void PersistentFileSysObj_Scan(
	PersistentFsObjType			fsObjType,

	PersistentStoreScanTupleCallback	scanTupleCallback)
{
	Assert(fsObjType >= PersistentFsObjType_First);
	Assert(fsObjType <= PersistentFsObjType_Last);

	PersistentStore_Scan(
					&(fileSysObjDataArray[fsObjType]->storeData),
					&(fileSysObjSharedDataArray[fsObjType]->storeSharedData),
					scanTupleCallback);
					
}

int64 PersistentFileSysObj_MyHighestSerialNum(
	PersistentFsObjType 	fsObjType)
{
	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	PersistentFileSysObj_GetDataPtrs(
								fsObjType,
								&fileSysObjData,
								&fileSysObjSharedData);

	return PersistentStore_MyHighestSerialNum(
								&fileSysObjData->storeData);
}

int64 PersistentFileSysObj_CurrentMaxSerialNum(
	PersistentFsObjType 	fsObjType)
{
	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	PersistentFileSysObj_GetDataPtrs(
								fsObjType,
								&fileSysObjData,
								&fileSysObjSharedData);

	return PersistentStore_CurrentMaxSerialNum(
								&fileSysObjSharedData->storeSharedData);
}

void PersistentFileSysObj_UpdateTuple(
	PersistentFsObjType		fsObjType,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	Datum 					*values,

	bool					flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Assert(fsObjType >= PersistentFsObjType_First);
	Assert(fsObjType <= PersistentFsObjType_Last);

	PersistentStore_UpdateTuple(
							&(fileSysObjDataArray[fsObjType]->storeData),
							&(fileSysObjSharedDataArray[fsObjType]->storeSharedData),
							persistentTid,
							values,
							flushToXLog);
}

void PersistentFileSysObj_ReplaceTuple(
	PersistentFsObjType		fsObjType,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	HeapTuple				tuple,

	Datum 					*newValues,

	bool					*replaces,

	bool					flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Assert(fsObjType >= PersistentFsObjType_First);
	Assert(fsObjType <= PersistentFsObjType_Last);

	PersistentStore_ReplaceTuple(
							&(fileSysObjDataArray[fsObjType]->storeData),
							&(fileSysObjSharedDataArray[fsObjType]->storeSharedData),
							persistentTid,
							tuple,
							newValues,
							replaces,
							flushToXLog);
}


void PersistentFileSysObj_ReadTuple(
	PersistentFsObjType			fsObjType,

	ItemPointer					readTid,

	Datum						*values,

	HeapTuple					*tupleCopy)
{
	Assert(fsObjType >= PersistentFsObjType_First);
	Assert(fsObjType <= PersistentFsObjType_Last);

	if (PersistentStore_IsZeroTid(readTid))
		elog(ERROR, "TID for fetch persistent '%s' tuple is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(fsObjType));

	PersistentStore_ReadTuple(
						&(fileSysObjDataArray[fsObjType]->storeData),
						&(fileSysObjSharedDataArray[fsObjType]->storeSharedData),
						readTid,
						values,
						tupleCopy);
}

void PersistentFileSysObj_AddTuple(
	PersistentFsObjType			fsObjType,

	Datum						*values,

	bool						flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	int64					*persistentSerialNum)
{
	Assert(fsObjType >= PersistentFsObjType_First);
	Assert(fsObjType <= PersistentFsObjType_Last);

	PersistentStore_AddTuple(
						&(fileSysObjDataArray[fsObjType]->storeData),
						&(fileSysObjSharedDataArray[fsObjType]->storeSharedData),
						values,
						flushToXLog,
						persistentTid,
						persistentSerialNum);
}

void PersistentFileSysObj_FreeTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	PersistentFsObjType			fsObjType,

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	bool						flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	Datum *freeValues;

	ItemPointerData previousFreeTid;

	Assert(fsObjType >= PersistentFsObjType_First);
	Assert(fsObjType <= PersistentFsObjType_Last);

	freeValues = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));

	MemSet(&previousFreeTid, 0, sizeof(ItemPointerData));

	switch (fsObjType)
	{
	case PersistentFsObjType_RelationFile:
		{
			XLogRecPtr mirrorBufpoolResyncCkptLoc;

			MemSet(&mirrorBufpoolResyncCkptLoc, 0, sizeof(XLogRecPtr));
			GpPersistentRelationNode_SetDatumValues(
												freeValues,
												/* tablespaceOid */ 0,
												/* databaseOid */ 0,
												/* relFileNodeOid */ 0,
												/* segmentFileNum */ 0,
												PersistentFileSysRelStorageMgr_None,
												PersistentFileSysState_Free,
												MirroredObjectExistenceState_None,
												MirroredRelDataSynchronizationState_None,
												/* mirrorBufpoolPhysicallyTruncated */ false,
												&mirrorBufpoolResyncCkptLoc,
												/* mirrorBufpoolResyncCkptBlockNum */ 0,
												/* mirrorAppendOnlyLossEof */ 0,
												/* mirrorAppendOnlyNewEof */ 0,
												InvalidTransactionId,
												/* persistentSerialNum */ 0,
												&previousFreeTid);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		GpPersistentDatabaseNode_SetDatumValues(
											freeValues,
											/* tablespaceOid */ 0,
											/* databaseOid */ 0,
											PersistentFileSysState_Free,
											MirroredObjectExistenceState_None,
											InvalidTransactionId,
											/* persistentSerialNum */ 0,
											&previousFreeTid);
		break;

	case PersistentFsObjType_TablespaceDir:
		GpPersistentTablespaceNode_SetDatumValues(
											freeValues,
											/* filespaceOid */ 0,
											/* tablespaceOid */ 0,
											PersistentFileSysState_Free,
											MirroredObjectExistenceState_None,
											InvalidTransactionId,
											/* persistentSerialNum */ 0,
											&previousFreeTid);
		break;

	case PersistentFsObjType_FilespaceDir:
		{
			char	locationBlankFilled[FilespaceLocationBlankPaddedWithNullTermLen];
			
			MemSet(locationBlankFilled, ' ', FilespaceLocationBlankPaddedWithNullTermLen - 1);
			locationBlankFilled[FilespaceLocationBlankPaddedWithNullTermLen - 1] = '\0';

			GpPersistentFilespaceNode_SetDatumValues(
												freeValues,
												/* filespaceOid */ 0,
												/* dbId1 */ 0,
												locationBlankFilled,
												/* dbId2 */ 0,
												locationBlankFilled,
												PersistentFileSysState_Free,
												MirroredObjectExistenceState_None,
												InvalidTransactionId,
												/* persistentSerialNum */ 0,
												&previousFreeTid);
		}
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjType);
	}

	PersistentStore_FreeTuple(
						&fileSysObjData->storeData,
						&fileSysObjSharedData->storeSharedData,
						persistentTid,
						freeValues,
						flushToXLog);
}

/*
 * errcontext_persistent_relation_state_change
 *
 * Add an errcontext() line showing the expected relation path, serial number, 
 * expected state(s), next state, and persistent TID.
 */
static int
errcontext_persistent_relation_state_change(
	PersistentFileSysObjName	*fsObjName,

	ItemPointer 			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64					serialNum,

	PersistentFileSysState	expectedState1,

	PersistentFileSysState	expectedState2,

	PersistentFileSysState	expectedState3,

	PersistentFileSysState	nextState)
	
{
	char states[100];

	if ((int)expectedState2 == -1)
		sprintf(states, "state '%s'", 
		        PersistentFileSysObjState_Name(expectedState1));
	else if ((int)expectedState3 == -1)
		sprintf(states, "states '%s' or '%s'",
			    PersistentFileSysObjState_Name(expectedState1),
			    PersistentFileSysObjState_Name(expectedState2));
	else
		sprintf(states, "states '%s', '%s', or '%s'",
			    PersistentFileSysObjState_Name(expectedState1),
			    PersistentFileSysObjState_Name(expectedState2),
			    PersistentFileSysObjState_Name(expectedState3));

	errcontext( 
		 "State change is for persistent '%s' to go from %s to state '%s' with serial number " INT64_FORMAT " at TID %s",
		 PersistentFileSysObjName_TypeAndObjectName(fsObjName),
		 states,
		 PersistentFileSysObjState_Name(nextState),
		 serialNum,
		 ItemPointerToString(persistentTid));

	return 0;
}

static void PersistentFileSysObj_VerifyExpected(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	PersistentFileSysObjName		*fsObjName,

	ItemPointer 					persistentTid,
								/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64							serialNum,

	Datum 							*values,

	PersistentFileSysState			expectedState1,

	PersistentFileSysState			expectedState2,

	PersistentFileSysState			expectedState3,

	PersistentFileSysState			nextState,

	PersistentFileSysState			*oldState)
{
	PersistentFsObjType				fsObjType;

	PersistentFileSysObjName 		actualFsObjName;
	PersistentFileSysState			actualState;
	MirroredObjectExistenceState 	actualMirrorExistenceState;	// UNDONE

	TransactionId				actualParentXid;
	int64						actualSerialNum;

	bool expectedStateMatch;

	fsObjType = fsObjName->type;

	GpPersistent_GetCommonValues(
							fsObjType,
							values,
							&actualFsObjName,
							&actualState,
							&actualMirrorExistenceState,
							&actualParentXid,
							&actualSerialNum);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "PersistentFileSysObj_VerifyExpected ('%s): common values -- state '%s' mirror existence state '%s', parent xid %u, serial number " INT64_FORMAT " at TID %s",
			 PersistentFileSysObjName_TypeAndObjectName(&actualFsObjName),
 			 PersistentFileSysObjState_Name(actualState),
			 MirroredObjectExistenceState_Name(actualMirrorExistenceState),
			 actualParentXid,
			 actualSerialNum,
			 ItemPointerToString(persistentTid));

	expectedStateMatch = (actualState == expectedState1);
	if (!expectedStateMatch && ((int)expectedState2 != -1))
		expectedStateMatch = (actualState == expectedState2);
	if (!expectedStateMatch && ((int)expectedState3 != -1))
		expectedStateMatch = (actualState == expectedState3);

	if (actualState == PersistentFileSysState_Free)
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Did not expect to find a 'Free' entry"),
				 errcontext_persistent_relation_state_change(
								 						fsObjName,
								 						persistentTid,
								 						serialNum,
								 						expectedState1,
								 						expectedState2,
								 						expectedState3,
								 						nextState)));
	}
	
	if (actualSerialNum != serialNum)
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Found different serial number " INT64_FORMAT " than expected (persistent file-system object found is '%s', state '%s')",
				        actualSerialNum,
						PersistentFileSysObjName_TypeAndObjectName(&actualFsObjName),
				        PersistentFileSysObjState_Name(actualState)),
				 errcontext_persistent_relation_state_change(
								 						fsObjName,
								 						persistentTid,
								 						serialNum,
								 						expectedState1,
								 						expectedState2,
														expectedState3,
								 						nextState)));
	}

	if (!expectedStateMatch)
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Found persistent file-system object in unexpected state '%s'",
						PersistentFileSysObjState_Name(actualState)),
				 errcontext_persistent_relation_state_change(
														fsObjName,
														persistentTid,
														serialNum,
														expectedState1,
														expectedState2,
														expectedState3,
														nextState)));
	}

	if (oldState != NULL)
		*oldState = actualState;
}

void PersistentFileSysObj_StateChange(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer					persistentTid,

	int64						persistentSerialNum,
	
	PersistentFileSysState		nextState,

	bool						flushToXLog,

	PersistentFileSysState		*oldState)
{
	PersistentFsObjType				fsObjType;

	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	Datum *values;

	HeapTuple tupleCopy;

	PersistentFileSysState	expectedState1;
	PersistentFileSysState	expectedState2 = -1;	// -1 means no second possibility.
	PersistentFileSysState	expectedState3 = -1;	// -1 means no third possibility.

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' state-change tuple is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(fsObjName->type));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for state-change is invalid (0)",
			 PersistentFileSysObjName_TypeName(fsObjName->type));

	if (oldState != NULL)
		*oldState = (PersistentFileSysState)-1;

	fsObjType = fsObjName->type;

	PersistentFileSysObj_GetDataPtrs(
								fsObjType,
								&fileSysObjData,
								&fileSysObjSharedData);
	
	values = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));

	PersistentFileSysObj_ReadTuple(
							fsObjType,
							persistentTid,
							values,
							&tupleCopy);

	switch (nextState)
	{
	case PersistentFileSysState_Created:
		expectedState1 = PersistentFileSysState_CreatePending;
		if (fsObjType == PersistentFsObjType_DatabaseDir)
			expectedState2 = PersistentFileSysState_JustInTimeCreatePending;
		break;

	case PersistentFileSysState_DropPending:
		expectedState1 = PersistentFileSysState_CreatePending;
		expectedState2 = PersistentFileSysState_Created;
		break;

	case PersistentFileSysState_AbortingCreate:
		expectedState1 = PersistentFileSysState_CreatePending;
		break;
		
	case PersistentFileSysState_Free:
		expectedState1 = PersistentFileSysState_DropPending;
		expectedState2 = PersistentFileSysState_AbortingCreate;
		if (fsObjType == PersistentFsObjType_DatabaseDir)
			expectedState3 = PersistentFileSysState_JustInTimeCreatePending;
		break;
			
	default:
		elog(ERROR, "Unexpected persistent object next state: %d",
			 nextState);
        expectedState1 = MaxPersistentFileSysState; /* to appease optimized compilation */
	}

	PersistentFileSysObj_VerifyExpected(
									fileSysObjData,
									fileSysObjSharedData,
									fsObjName,
									persistentTid,
									persistentSerialNum,
									values,
									expectedState1,
									expectedState2,
									expectedState3,
									nextState,
									oldState);

	if (nextState != PersistentFileSysState_Free)
	{
		Datum *newValues;
		bool *replaces;

		newValues = (Datum*)palloc0(fileSysObjData->storeData.numAttributes * sizeof(Datum));
		replaces = (bool*)palloc0(fileSysObjData->storeData.numAttributes * sizeof(bool));

		replaces[fileSysObjData->attNumPersistentState - 1] = true;
		newValues[fileSysObjData->attNumPersistentState - 1] = Int16GetDatum(nextState);

		replaces[fileSysObjData->attNumParentXid - 1] = true;
		newValues[fileSysObjData->attNumParentXid - 1] = Int32GetDatum(InvalidTransactionId);

		PersistentFileSysObj_ReplaceTuple(
									fsObjType,
									persistentTid,
									tupleCopy,
									newValues,
									replaces,
									flushToXLog);

		pfree(newValues);
		pfree(replaces);
	}
	else
	{
		PersistentFileSysObj_FreeTuple(
									fileSysObjData,
									fileSysObjSharedData,
									fsObjType,
									persistentTid,
									flushToXLog);
	}

	pfree(values);
}

static void PersistentFileSysObj_DoMirrorReCreate(
	PersistentFileSysObjName 	*fsObjName,

	PersistentFileSysRelStorageMgr relStorageMgr,

	ItemPointer					persistentTid,

	int64						persistentSerialNum,
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	bool						*mirrorDataLossOccurred)
{
	StorageManagerMirrorMode mirrorMode = StorageManagerMirrorMode_MirrorOnly;
	bool 					 ignoreAlreadyExists = true;
	int						 primaryError;

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for mirror re-create is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(fsObjName->type));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for mirror re-create is invalid (0)",
			 PersistentFileSysObjName_TypeName(fsObjName->type));

	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = PersistentFileSysObjName_GetRelFileNodePtr(fsObjName);
			int32 segmentFileNum = PersistentFileSysObjName_GetSegmentFileNum(fsObjName);

			Assert(relStorageMgr != PersistentFileSysRelStorageMgr_None);

			if (relStorageMgr == PersistentFileSysRelStorageMgr_BufferPool)
			{
				Assert(segmentFileNum == 0);

				MirroredBufferPool_MirrorReCreate(
											relFileNode,
											/* segmentFileNum */ 0,
											mirrorDataLossTrackingState,
											mirrorDataLossTrackingSessionNum,
											mirrorDataLossOccurred);
			}
			else
			{
				Assert(relStorageMgr == PersistentFileSysRelStorageMgr_AppendOnly);

				MirroredAppendOnly_MirrorReCreate(
											relFileNode,
											segmentFileNum,
											mirrorDataLossTrackingState,
											mirrorDataLossTrackingSessionNum,
											mirrorDataLossOccurred);
			}
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		{
			DbDirNode *dbDirNode = &fsObjName->variant.dbDirNode;
			
			MirroredFileSysObj_CreateDbDir(
								dbDirNode, 
								mirrorMode,
								ignoreAlreadyExists,
								&primaryError,
								mirrorDataLossOccurred);
		}
		break;

	case PersistentFsObjType_TablespaceDir:			
		MirroredFileSysObj_CreateTablespaceDir(
							fsObjName->variant.tablespaceOid,
							mirrorMode,
							ignoreAlreadyExists,
							&primaryError,
							mirrorDataLossOccurred);
		break;

	case PersistentFsObjType_FilespaceDir:	
		{
			char *primaryFilespaceLocation;
			char *mirrorFilespaceLocation;

			PersistentFilespace_GetPrimaryAndMirror(
								fsObjName->variant.filespaceOid,
								&primaryFilespaceLocation,
								&mirrorFilespaceLocation);

			MirroredFileSysObj_CreateFilespaceDir(
								fsObjName->variant.filespaceOid,
								primaryFilespaceLocation,
								mirrorFilespaceLocation,
								mirrorMode,
								ignoreAlreadyExists,
								&primaryError,
								mirrorDataLossOccurred);

			if (primaryFilespaceLocation != NULL)
				pfree(primaryFilespaceLocation);
			if (mirrorFilespaceLocation != NULL)
				pfree(mirrorFilespaceLocation);
		}
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}
}

void PersistentFileSysObj_Created(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer				persistentTid,

	int64					persistentSerialNum)
{
	if (!PersistentFileSysObj_BeforePersistenceWork())
	{
		if (PersistentStore_IsZeroTid(persistentTid))
			elog(ERROR, "TID for persistent '%s' tuple for created is invalid (0,0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));

		if (persistentSerialNum == 0)
			elog(ERROR, "Persistent '%s' serial number for created is invalid (0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));
	}
	
	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = PersistentFileSysObjName_GetRelFileNodePtr(fsObjName);
			int32 segmentFileNum = PersistentFileSysObjName_GetSegmentFileNum(fsObjName);

			PersistentRelation_Created(
								relFileNode,
								segmentFileNum,
								persistentTid,
								persistentSerialNum);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentDatabase_Created(
							&fsObjName->variant.dbDirNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentTablespace_Created(
							fsObjName->variant.tablespaceOid,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_FilespaceDir:
		PersistentFilespace_Created(
							fsObjName->variant.filespaceOid,
							persistentTid,
							persistentSerialNum);
		break;


	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}
}

void PersistentFileSysObj_MarkAbortingCreate(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer				persistentTid,

	int64					persistentSerialNum)
{
	if (!PersistentFileSysObj_BeforePersistenceWork())
	{
		if (PersistentStore_IsZeroTid(persistentTid))
			elog(ERROR, "TID for persistent '%s' tuple for mark aborting CREATE is invalid (0,0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));

		if (persistentSerialNum == 0)
			elog(ERROR, "Persistent '%s' serial number for mark aborting CREATE is invalid (0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));
	}
	
	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = PersistentFileSysObjName_GetRelFileNodePtr(fsObjName);
			int32 segmentFileNum = PersistentFileSysObjName_GetSegmentFileNum(fsObjName);

			PersistentRelation_MarkAbortingCreate(
								relFileNode,
								segmentFileNum,
								persistentTid,
								persistentSerialNum);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentDatabase_MarkAbortingCreate(
							&fsObjName->variant.dbDirNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentTablespace_MarkAbortingCreate(
							fsObjName->variant.tablespaceOid,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_FilespaceDir:
		PersistentFilespace_MarkAbortingCreate(
							fsObjName->variant.filespaceOid,
							persistentTid,
							persistentSerialNum);
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}
}

void PersistentFileSysObj_MarkDropPending(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer				persistentTid,

	int64					persistentSerialNum)
{
	if (!PersistentFileSysObj_BeforePersistenceWork())
	{
		if (PersistentStore_IsZeroTid(persistentTid))
			elog(ERROR, "TID for persistent '%s' tuple for mark DROP pending is invalid (0,0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));

		if (persistentSerialNum == 0)
			elog(ERROR, "Persistent '%s' serial number for mark DROP pending is invalid (0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));
	}

	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = PersistentFileSysObjName_GetRelFileNodePtr(fsObjName);
			int32 segmentFileNum = PersistentFileSysObjName_GetSegmentFileNum(fsObjName);

			PersistentRelation_MarkDropPending(
								relFileNode,
								segmentFileNum,
								persistentTid,
								persistentSerialNum);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentDatabase_MarkDropPending(
							&fsObjName->variant.dbDirNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentTablespace_MarkDropPending(
							fsObjName->variant.tablespaceOid,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_FilespaceDir:
		PersistentFilespace_MarkDropPending(
							fsObjName->variant.tablespaceOid,
							persistentTid,
							persistentSerialNum);
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}
}

/*
 * Error context callback for errors occurring during PersistentFileSysObj_DropObject().
 */
static void
PersistentFileSysObj_DropObjectErrorCallback(void *arg)
{
	PersistentFileSysObjName *fsObjName = (PersistentFileSysObjName *) arg;

	errcontext("Dropping file-system object -- %s",
		       PersistentFileSysObjName_TypeAndObjectName(fsObjName));
}


void PersistentFileSysObj_DropObject(
	PersistentFileSysObjName 	*fsObjName,

	PersistentFileSysRelStorageMgr relStorageMgr,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	ItemPointer				persistentTid,

	int64					persistentSerialNum,

	bool 					ignoreNonExistence)
{
	ErrorContextCallback errcontext;

	bool mirrorDataLossOccurred;

	/* Setup error traceback support for ereport() */
	errcontext.callback = PersistentFileSysObj_DropObjectErrorCallback;
	errcontext.arg = (void *) fsObjName;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;
	
	if (!PersistentFileSysObj_BeforePersistenceWork())
	{
		if (PersistentStore_IsZeroTid(persistentTid))
			elog(ERROR, "TID for persistent '%s' tuple for DROP is invalid (0,0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));

		if (persistentSerialNum == 0)
			elog(ERROR, "Persistent '%s' serial number for DROP is invalid (0)",
				 PersistentFileSysObjName_TypeName(fsObjName->type));
	}

	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = PersistentFileSysObjName_GetRelFileNodePtr(fsObjName);
			int32 segmentFileNum = PersistentFileSysObjName_GetSegmentFileNum(fsObjName);
			
			MirroredFileSysObj_DropRelFile(
								relFileNode,
								segmentFileNum,
								relStorageMgr,
								relationName,
								/* isLocalBuf */ false, 
								ignoreNonExistence,
								&mirrorDataLossOccurred);

			if (!mirrorDataLossOccurred)
				PersistentRelation_Dropped(
									relFileNode,
									segmentFileNum,
									persistentTid,
									persistentSerialNum);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		{
			DbDirNode *dbDirNode = &fsObjName->variant.dbDirNode;
			
			MirroredFileSysObj_DropDbDir(
								dbDirNode,
								/* mirrorOnly */ false,
								ignoreNonExistence,
								&mirrorDataLossOccurred);
			
			if (!mirrorDataLossOccurred)
				PersistentDatabase_Dropped(
									dbDirNode,
									persistentTid,
									persistentSerialNum);
		}
		break;

	case PersistentFsObjType_TablespaceDir:			
		MirroredFileSysObj_DropTablespaceDir(
							fsObjName->variant.tablespaceOid,
							/* mirrorOnly */ false,
							ignoreNonExistence,
							&mirrorDataLossOccurred);

		if (!mirrorDataLossOccurred)
			PersistentTablespace_Dropped(
								fsObjName->variant.tablespaceOid,
								persistentTid,
								persistentSerialNum);
		break;

	case PersistentFsObjType_FilespaceDir:	
		{
			char *primaryFilespaceLocation;
			char *mirrorFilespaceLocation;

			PersistentFilespace_GetPrimaryAndMirror(
								fsObjName->variant.filespaceOid,
								&primaryFilespaceLocation,
								&mirrorFilespaceLocation);

			MirroredFileSysObj_DropFilespaceDir(
								fsObjName->variant.filespaceOid,
								primaryFilespaceLocation,
								mirrorFilespaceLocation,
								/* mirrorOnly */ false,
								ignoreNonExistence,
								&mirrorDataLossOccurred);

			if (!mirrorDataLossOccurred)
				PersistentFilespace_Dropped(
									fsObjName->variant.filespaceOid,
									persistentTid,
									persistentSerialNum);

			if (primaryFilespaceLocation != NULL)
				pfree(primaryFilespaceLocation);
			if (mirrorFilespaceLocation != NULL)
				pfree(mirrorFilespaceLocation);
		}
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}

	if (mirrorDataLossOccurred)
	{
		/*
		 * Leave this persistent object in 'Drop Pending' state with an indication that
		 * Re-Drop will be needed during Resynchronize.
		 */
		PersistentFileSysObj_ChangeMirrorExistenceState(
												fsObjName->type,
												persistentTid,
												persistentSerialNum,
												MirroredObjectExistenceState_MirrorReDropNeeded,
												MirroredRelDataSynchronizationState_None,
												/* flushToXLog */ true);
	}
	
	/* Pop the error context stack */
	error_context_stack = errcontext.previous;
}

static void PersistentFileSysObj_DoMirrorReDrop(
	PersistentFileSysObjName 	*fsObjName,

	PersistentFileSysRelStorageMgr relStorageMgr,

	ItemPointer					persistentTid,

	int64						persistentSerialNum,
	
	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	bool						*mirrorDataLossOccurred)
{
	*mirrorDataLossOccurred = false;

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for mirror re-drop is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(fsObjName->type));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for mirror re-drop is invalid (0)",
			 PersistentFileSysObjName_TypeName(fsObjName->type));

	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = PersistentFileSysObjName_GetRelFileNodePtr(fsObjName);
			int32 segmentFileNum = PersistentFileSysObjName_GetSegmentFileNum(fsObjName);

			Assert(relStorageMgr != PersistentFileSysRelStorageMgr_None);

			if (relStorageMgr == PersistentFileSysRelStorageMgr_BufferPool)
			{
				Assert(segmentFileNum == 0);

				MirroredBufferPool_MirrorReDrop(
											relFileNode,
											segmentFileNum,
											mirrorDataLossTrackingState,
											mirrorDataLossTrackingSessionNum,
											mirrorDataLossOccurred);
			}
			else
			{
				Assert(relStorageMgr == PersistentFileSysRelStorageMgr_AppendOnly);

				MirroredAppendOnly_MirrorReDrop(
											relFileNode,
											segmentFileNum,
											mirrorDataLossTrackingState,
											mirrorDataLossTrackingSessionNum,
											mirrorDataLossOccurred);
			}

			if (*mirrorDataLossOccurred)
				return;

			PersistentRelation_Dropped(
								relFileNode,
								segmentFileNum,
								persistentTid,
								persistentSerialNum);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		{
			DbDirNode *dbDirNode = &fsObjName->variant.dbDirNode;
			
			MirroredFileSysObj_DropDbDir(
								dbDirNode, 
								/* mirrorOnly */ true,
								/* ignoreNonExistence */ true,
								mirrorDataLossOccurred);
			
			if (*mirrorDataLossOccurred)
				return;

			PersistentDatabase_Dropped(
								dbDirNode,
								persistentTid,
								persistentSerialNum);
		}
		break;

	case PersistentFsObjType_TablespaceDir:			
		MirroredFileSysObj_DropTablespaceDir(
							fsObjName->variant.tablespaceOid,
							/* mirrorOnly */ true,
							/* ignoreNonExistence */ true,
							mirrorDataLossOccurred);
		
		if (*mirrorDataLossOccurred)
			return;
		
		PersistentTablespace_Dropped(
							fsObjName->variant.tablespaceOid,
							persistentTid,
							persistentSerialNum);
		break;
		break;

	case PersistentFsObjType_FilespaceDir:	
		{
			char *primaryFilespaceLocation;
			char *mirrorFilespaceLocation;

			PersistentFilespace_GetPrimaryAndMirror(
								fsObjName->variant.filespaceOid,
								&primaryFilespaceLocation,
								&mirrorFilespaceLocation);

			MirroredFileSysObj_DropFilespaceDir(
								fsObjName->variant.filespaceOid,
								primaryFilespaceLocation,
								mirrorFilespaceLocation,
								/* mirrorOnly */ true,
								/* ignoreNonExistence */ true,
								mirrorDataLossOccurred);
			
			if (*mirrorDataLossOccurred)
				return;
			
			PersistentFilespace_Dropped(
								fsObjName->variant.filespaceOid,
								persistentTid,
								persistentSerialNum);

			if (primaryFilespaceLocation != NULL)
				pfree(primaryFilespaceLocation);
			if (mirrorFilespaceLocation != NULL)
				pfree(mirrorFilespaceLocation);
		}
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}
}

void PersistentFileSysObj_EndXactDrop(
	PersistentFileSysObjName 	*fsObjName,

	PersistentFileSysRelStorageMgr relStorageMgr,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	ItemPointer				persistentTid,

	int64					persistentSerialNum,

	bool					abortingCreate)
{
	if (fsObjName->type == PersistentFsObjType_RelationFile)
	{
		LockRelationForResynchronize(
						PersistentFileSysObjName_GetRelFileNodePtr(fsObjName), 
						AccessExclusiveLock);

		/*
		 * More likely that the exclusive resynchronize lock would block for a long
		 * time than the MirroredLock.
		 */

		LWLockAcquire(MirroredLock, LW_SHARED);
	}

	PersistentFileSysObj_DropObject(
							fsObjName,
							relStorageMgr,
							relationName,
							persistentTid,
							persistentSerialNum,
							/* ignoreNonExistence */ abortingCreate);

	if (fsObjName->type == PersistentFsObjType_RelationFile)
	{
		UnlockRelationForResynchronize(
						PersistentFileSysObjName_GetRelFileNodePtr(fsObjName), 
						AccessExclusiveLock);

	
		LWLockRelease(MirroredLock);
	}
}

static void PersistentFileSysObj_UpdateTupleAppendOnlyMirrorResyncEofs(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	HeapTuple					tuple,
	
	ItemPointer 				persistentTid,

	int64						persistentSerialNum,

	int64						mirrorLossEof,
	
	int64						mirrorNewEof,

	bool						flushToXLog)
{
	Datum newValues[Natts_gp_persistent_relation_node];
	bool replaces[Natts_gp_persistent_relation_node];

	MemSet(newValues, 0, Natts_gp_persistent_relation_node * sizeof(Datum));
	MemSet(replaces, false, Natts_gp_persistent_relation_node * sizeof(bool));
	
	replaces[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1] = true;
	newValues[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1] = 
													Int64GetDatum(mirrorLossEof);
	
	replaces[Anum_gp_persistent_relation_node_mirror_append_only_new_eof - 1] = true;
	newValues[Anum_gp_persistent_relation_node_mirror_append_only_new_eof - 1] = 
													Int64GetDatum(mirrorNewEof);
	
	PersistentFileSysObj_ReplaceTuple(
								PersistentFsObjType_RelationFile,
								persistentTid,
								tuple,
								newValues,
								replaces,
								flushToXLog);
}

void PersistentFileSysObj_UpdateAppendOnlyMirrorResyncEofs(
	RelFileNode					*relFileNode,

	int32						segmentFileNum,

	ItemPointer 				persistentTid,

	int64						persistentSerialNum,

	int64						mirrorLossEof,

	int64						mirrorNewEof,

	bool						recovery,
	
	bool						flushToXLog)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjData		*fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;
	
	Datum values[Natts_gp_persistent_relation_node];

	HeapTuple tupleCopy;

	RelFileNode 					actualRelFileNode;
	int32							actualSegmentFileNum;
	
	PersistentFileSysRelStorageMgr	relationStorageManager;
	PersistentFileSysState			persistentState;
	MirroredObjectExistenceState	mirrorExistenceState;
	MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
	bool							mirrorBufpoolPhysicallyTruncated;
	XLogRecPtr						mirrorBufpoolResyncCkptLoc;
	BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
	int64							mirrorAppendOnlyLossEof;
	int64							mirrorAppendOnlyNewEof;
	TransactionId					parentXid;
	int64							actualPersistentSerialNum;
	ItemPointerData 				previousFreeTid;

	int64							updateMirrorAppendOnlyLossEof;
	int64							updateMirrorAppendOnlyNewEof;

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);
	
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent %u/%u/%u, segment file #%d, tuple for update Append-Only mirror resync EOFs is invalid (0,0)",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum);

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent %u/%u/%u, segment file #%d, serial number for update Append-Only mirror resync EOFs is invalid (0)",
		relFileNode->spcNode,
		relFileNode->dbNode,
		relFileNode->relNode,
		segmentFileNum);

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentFileSysObj_ReadTuple(
							PersistentFsObjType_RelationFile,
							persistentTid,
							values,
							&tupleCopy);

	GpPersistentRelationNode_GetValues(
									values,
									&actualRelFileNode.spcNode,
									&actualRelFileNode.dbNode,
									&actualRelFileNode.relNode,
									&actualSegmentFileNum,
									&relationStorageManager,
									&persistentState,
									&mirrorExistenceState,
									&mirrorDataSynchronizationState,
									&mirrorBufpoolPhysicallyTruncated,
									&mirrorBufpoolResyncCkptLoc,
									&mirrorBufpoolResyncCkptBlockNum,
									&mirrorAppendOnlyLossEof,
									&mirrorAppendOnlyNewEof,
									&parentXid,
									&actualPersistentSerialNum,
									&previousFreeTid);	

	if (persistentState == PersistentFileSysState_Free)
		elog(ERROR, "Persistent %u/%u/%u, segment file #%d, entry is 'Free' for update Append-Only mirror resync EOFs at TID %s",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 ItemPointerToString(persistentTid));

	if (persistentSerialNum != actualPersistentSerialNum)
		elog(ERROR, "Persistent %u/%u/%u, segment file #%d, serial number mismatch for update Append-Only mirror resync EOFs (expected " INT64_FORMAT ", found " INT64_FORMAT "), at TID %s",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 persistentSerialNum,
			 actualPersistentSerialNum,
			 ItemPointerToString(persistentTid));

	if (relationStorageManager != PersistentFileSysRelStorageMgr_AppendOnly)
		elog(ERROR, "Persistent %u/%u/%u, segment file #%d, relation storage manager mismatch for update Append-Only mirror resync EOFs (expected '%s', found '%s'), at TID %s",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 PersistentFileSysRelStorageMgr_Name(PersistentFileSysRelStorageMgr_AppendOnly),
			 PersistentFileSysRelStorageMgr_Name(relationStorageManager),
			 ItemPointerToString(persistentTid));

	if (!recovery)
	{
		if (mirrorAppendOnlyNewEof >= mirrorNewEof)
			elog(ERROR, "Persistent %u/%u/%u, segment file #%d, current new EOF is greater than or equal to update new EOF for Append-Only mirror resync EOFs (current new EOF " INT64_FORMAT ", update new EOF " INT64_FORMAT "), persistent serial num " INT64_FORMAT " at TID %s",
				 relFileNode->spcNode,
				 relFileNode->dbNode,
				 relFileNode->relNode,
				 segmentFileNum,
				 mirrorAppendOnlyNewEof,
				 mirrorNewEof,
				 persistentSerialNum,
				 ItemPointerToString(persistentTid));
	}
	else
	{
		if (mirrorAppendOnlyNewEof > mirrorNewEof)
			elog(ERROR, "Persistent %u/%u/%u, segment file #%d, current new EOF is greater than update new EOF for Append-Only mirror resync EOFs recovery (current new EOF " INT64_FORMAT ", update new EOF " INT64_FORMAT "), persistent serial num " INT64_FORMAT " at TID %s",
				 relFileNode->spcNode,
				 relFileNode->dbNode,
				 relFileNode->relNode,
				 segmentFileNum,
				 mirrorAppendOnlyNewEof,
				 mirrorNewEof,
				 persistentSerialNum,
				 ItemPointerToString(persistentTid));

		if (mirrorAppendOnlyNewEof == mirrorNewEof)
		{
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "Persistent %u/%u/%u, segment file #%d, current new EOF equal to update new EOF for Append-Only mirror resync EOFs recovery (EOF " INT64_FORMAT "), persistent serial num " INT64_FORMAT " at TID %s",
					 relFileNode->spcNode,
					 relFileNode->dbNode,
					 relFileNode->relNode,
					 segmentFileNum,
					 mirrorAppendOnlyNewEof,
					 persistentSerialNum,
					 ItemPointerToString(persistentTid));

			MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

			return;		// Old news.
		}
	}

	updateMirrorAppendOnlyLossEof = mirrorLossEof;		// Assume.
	updateMirrorAppendOnlyNewEof = mirrorNewEof;		// Assume.
	
	if (mirrorAppendOnlyLossEof < mirrorAppendOnlyNewEof)
	{
		/*
		 * Keep original loss EOF.
		 */
		updateMirrorAppendOnlyLossEof = mirrorAppendOnlyLossEof;
	}

	PersistentFileSysObj_UpdateTupleAppendOnlyMirrorResyncEofs(
												fileSysObjData,
												fileSysObjSharedData,
												tupleCopy,
												persistentTid,
												persistentSerialNum,
												updateMirrorAppendOnlyLossEof,
												updateMirrorAppendOnlyNewEof,
												flushToXLog);

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentFileSysObj_UpdateAppendOnlyMirrorResyncEofs: %u/%u/%u, segment file #%d, Append-Only Mirror Resync EOFs (recovery %s) -- persistent serial num " INT64_FORMAT ", TID %s" 
			 ", original mirror loss EOF " INT64_FORMAT ", input mirror loss EOF " INT64_FORMAT ", update mirror loss EOF " INT64_FORMAT 
			 ", original mirror new EOF " INT64_FORMAT ", input mirror new EOF " INT64_FORMAT ", update mirror new EOF " INT64_FORMAT,
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 (recovery ? "true" : "false"),
			 persistentSerialNum,
			 ItemPointerToString(persistentTid),
			 mirrorAppendOnlyLossEof,
			 mirrorLossEof,
			 updateMirrorAppendOnlyLossEof,
			 mirrorAppendOnlyNewEof,
			 mirrorNewEof,
			 updateMirrorAppendOnlyNewEof);
}

void PersistentFileSysObj_EndXactAction(
	PersistentEndXactRecObjects 	*persistentObjects,
	
	bool							isCommit)
{
	int i;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentFileSysObj_EndXactAction: commit %s, create count %d, drop count %d, , Append-Only mirror resync EOFs count %d",
			 (isCommit ? "true" : "false"),
			 persistentObjects->typed.createFsObjsCount,
			 persistentObjects->typed.dropFsObjsCount,
			 persistentObjects->typed.appendOnlyMirrorResyncEofsCount);

	if (isCommit)
	{
		/*
		 * 'Create Pending' --> 'Created'.
		 */
		for (i = 0; i < persistentObjects->typed.createFsObjsCount; i++)
		{
			PersistentEndXactFsObj *fsObj = 
						&persistentObjects->typed.createFsObjs[i];

			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_EndXactAction: [%d] %s, persistent serial num " INT64_FORMAT ", TID %s, 'Created'",
					 i,
					 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
					 fsObj->persistentSerialNum,
					 ItemPointerToString(&fsObj->persistentTid));
			
			PersistentFileSysObj_Created(
									&fsObj->fsObjName,
									&fsObj->persistentTid,
									fsObj->persistentSerialNum);
		}

		/*
		 * 'Create Pending' OR 'Created' --> 'Drop Pending'.
		 */
		for (i = 0; i < persistentObjects->typed.dropFsObjsCount; i++)
		{
			PersistentEndXactFsObj *fsObj = 
						&persistentObjects->typed.dropFsObjs[i];

			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_EndXactAction: [%d] %s, persistent serial num " INT64_FORMAT ", TID %s, 'Drop Pending'",
					 i,
					 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
					 fsObj->persistentSerialNum,
					 ItemPointerToString(&fsObj->persistentTid));

			PersistentFileSysObj_MarkDropPending(
									&fsObj->fsObjName,
									&fsObj->persistentTid,
									fsObj->persistentSerialNum);
		}

		PersistentRelation_FlushXLog();
		
		/*
		 * 1. Physically drop the file-system object.
		 *
		 * 2. 'Drop Pending' -> (Free)
		 */
		for (i = 0; i < persistentObjects->typed.dropFsObjsCount; i++)
		{
			PersistentEndXactFsObj *fsObj = 
						&persistentObjects->typed.dropFsObjs[i];

			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_EndXactAction: [%d] %s, relation storage manager '%s', persistent serial num " INT64_FORMAT ", TID %s, Drop",
					 i,
					 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
					 PersistentFileSysRelStorageMgr_Name(fsObj->relStorageMgr),
					 fsObj->persistentSerialNum,
					 ItemPointerToString(&fsObj->persistentTid));

			PersistentFileSysObj_EndXactDrop(
									&fsObj->fsObjName,
									fsObj->relStorageMgr,
									/* relationName */ NULL,		// Ok to be NULL -- we don't know the name here.
									&fsObj->persistentTid,
									fsObj->persistentSerialNum,
									/* abortingCreate */ false);
		}
		
		/*
		 * Append-Only mirror resync EOFs.
		 */
		for (i = 0; i < persistentObjects->typed.appendOnlyMirrorResyncEofsCount; i++)
		{
			PersistentEndXactAppendOnlyMirrorResyncEofs *eofs = 
						&persistentObjects->typed.appendOnlyMirrorResyncEofs[i];

			PersistentFileSysObj_UpdateAppendOnlyMirrorResyncEofs(
															&eofs->relFileNode,
															eofs->segmentFileNum,
															&eofs->persistentTid,
															eofs->persistentSerialNum,
															eofs->mirrorLossEof,
															eofs->mirrorNewEof,
															/* recovery */ false,
															/* flushToXLog */ false);
		}
	}
	else
	{
		/*
		 * 'Create Pending' --> 'Aborting Create'.
		 */
		for (i = 0; i < persistentObjects->typed.createFsObjsCount; i++)
		{
			PersistentEndXactFsObj *fsObj = 
						&persistentObjects->typed.createFsObjs[i];

			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_EndXactAction: [%d] %s, persistent serial num " INT64_FORMAT ", TID %s, 'Aborting Create'",
					 i,
					 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
					 fsObj->persistentSerialNum,
					 ItemPointerToString(&fsObj->persistentTid));

			PersistentFileSysObj_MarkAbortingCreate(
									&fsObj->fsObjName,
									&fsObj->persistentTid,
									fsObj->persistentSerialNum);
		}

		PersistentRelation_FlushXLog();
		
		/*
		 * 1. Physically drop the file-system object.
		 *
		 * 2. 'Drop Pending' -> (Free)
		 */
		for (i = 0; i < persistentObjects->typed.createFsObjsCount; i++)
		{
			PersistentEndXactFsObj *fsObj = 
						&persistentObjects->typed.createFsObjs[i];

			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_EndXactAction: [%d] %s, relation storage manager '%s', persistent serial num " INT64_FORMAT ", TID %s, Drop",
					 i,
					 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
					 PersistentFileSysRelStorageMgr_Name(fsObj->relStorageMgr),
					 fsObj->persistentSerialNum,
					 ItemPointerToString(&fsObj->persistentTid));

			/*
			 * Allow for possibility the file-system create did not succeed --
			 * treat it like crash recovery.
			 */
			PersistentFileSysObj_EndXactDrop(
									&fsObj->fsObjName,
									fsObj->relStorageMgr,
									/* relationName */ NULL,		// Ok to be NULL -- we don't know the name here.
									&fsObj->persistentTid,
									fsObj->persistentSerialNum,
									/* abortingCreate */ true);
		}
	}

	/*
	 * For both COMMIT and ABORT, release our intent to add Append-Only mirror resync EOFs.
	 */
	if (persistentObjects->typed.appendOnlyMirrorResyncEofsCount > 0)
		FileRepPrimary_FinishedAppendOnlyCommitWork(persistentObjects->typed.appendOnlyMirrorResyncEofsCount);
}

static void PersistentFileSysObj_UpdateTupleMirrorExistenceState(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	PersistentFsObjType				fsObjType,

	HeapTuple					tuple,
	
	ItemPointer 				persistentTid,

	int64						persistentSerialNum,

	MirroredObjectExistenceState mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	bool						flushToXLog)
{
	Datum *newValues;
	bool *replaces;

	newValues = (Datum*)palloc0(fileSysObjData->storeData.numAttributes * sizeof(Datum));
	replaces = (bool*)palloc0(fileSysObjData->storeData.numAttributes * sizeof(bool));
	
	replaces[fileSysObjData->attNumMirrorExistenceState - 1] = true;
	newValues[fileSysObjData->attNumMirrorExistenceState - 1] = Int16GetDatum(mirrorExistenceState);

	if (fsObjType == PersistentFsObjType_RelationFile)
	{
		replaces[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1] = true;
		newValues[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1] = Int16GetDatum(relDataSynchronizationState);
	}
	
	PersistentFileSysObj_ReplaceTuple(
								fsObjType,
								persistentTid,
								tuple,
								newValues,
								replaces,
								flushToXLog);
	
	pfree(newValues);
	pfree(replaces);
}

static void PersistentFileSysObj_UpdateTupleMirrorReCreated(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	PersistentFsObjType				fsObjType,

	HeapTuple						tuple,
	
	ItemPointer 					persistentTid,

	int64							persistentSerialNum,

	PersistentFileSysRelStorageMgr 	relStorageMgr,
	
	bool							flushToXLog)
{
	Datum *newValues;
	bool *replaces;

	newValues = (Datum*)palloc0(fileSysObjData->storeData.numAttributes * sizeof(Datum));
	replaces = (bool*)palloc0(fileSysObjData->storeData.numAttributes * sizeof(bool));
	
	replaces[fileSysObjData->attNumMirrorExistenceState - 1] = true;
	newValues[fileSysObjData->attNumMirrorExistenceState - 1] = 
												Int16GetDatum(MirroredObjectExistenceState_MirrorCreated);

	if (fsObjType == PersistentFsObjType_RelationFile)
	{
		replaces[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1] = true;
		newValues[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1] = 
												Int16GetDatum(MirroredRelDataSynchronizationState_FullCopy);

		if (relStorageMgr == PersistentFileSysRelStorageMgr_AppendOnly)
		{
			replaces[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1] = true;
			newValues[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1] = 
												Int64GetDatum(0);
		}
	}
	
	PersistentFileSysObj_ReplaceTuple(
								fsObjType,
								persistentTid,
								tuple,
								newValues,
								replaces,
								flushToXLog);
	
	pfree(newValues);
	pfree(replaces);
}

static void PersistentFileSysObj_UpdateTupleMirrorBufPoolPhysicallyTruncated(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	HeapTuple					tuple,
	
	ItemPointer 				persistentTid,

	int64						persistentSerialNum,

	bool						newMirrorBufPoolPhysicallyTruncated,

	bool						flushToXLog)
{
	Datum newValues[Natts_gp_persistent_relation_node];
	bool replaces[Natts_gp_persistent_relation_node];

	MemSet(newValues, 0, Natts_gp_persistent_relation_node * sizeof(Datum));
	MemSet(replaces, false, Natts_gp_persistent_relation_node * sizeof(bool));
	
	replaces[Anum_gp_persistent_relation_node_mirror_bufpool_physically_truncated - 1] = true;
	newValues[Anum_gp_persistent_relation_node_mirror_bufpool_physically_truncated - 1] = 
													BoolGetDatum(newMirrorBufPoolPhysicallyTruncated);
	
	PersistentFileSysObj_ReplaceTuple(
								PersistentFsObjType_RelationFile,
								persistentTid,
								tuple,
								newValues,
								replaces,
								flushToXLog);
}

void PersistentFileSysObj_ChangeMirrorExistenceState(
	PersistentFsObjType				fsObjType,

	ItemPointer 					persistentTid,

	int64							persistentSerialNum,

	MirroredObjectExistenceState 	mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	bool							flushToXLog)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	Datum *values;

	HeapTuple tupleCopy;

	PersistentFileSysObj_GetDataPtrs(
								fsObjType,
								&fileSysObjData,
								&fileSysObjSharedData);

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for update mirror existence state is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(fsObjType));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for update mirror existence state is invalid (0)",
			 PersistentFileSysObjName_TypeName(fsObjType));

	values = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));
		 
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;
		 
	PersistentFileSysObj_ReadTuple(
							fsObjType,
							persistentTid,
							values,
							&tupleCopy);

	PersistentFileSysObj_UpdateTupleMirrorExistenceState(
												fileSysObjData,
												fileSysObjSharedData,
												fsObjType,
												tupleCopy,
												persistentTid,
												persistentSerialNum,
												mirrorExistenceState,
												relDataSynchronizationState,
												flushToXLog);
	
	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
	
	pfree(values);
}

void PersistentFileSysObj_MarkBufPoolRelationPhysicallyTruncated(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer					persistentTid,

	int64						persistentSerialNum,
	
	bool						flushToXLog)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;
	
	Datum *values;

	HeapTuple tupleCopy;

	RelFileNode 					actualRelFileNode;
	int32							actualSegmentFileNum;
	
	PersistentFileSysRelStorageMgr	relationStorageManager;
	PersistentFileSysState			persistentState;
	MirroredObjectExistenceState	mirrorExistenceState;
	MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
	bool							mirrorBufpoolPhysicallyTruncated;
	XLogRecPtr						mirrorBufpoolResyncCkptLoc;
	BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
	int64							mirrorAppendOnlyLossEof;
	int64							mirrorAppendOnlyNewEof;
	TransactionId					parentXid;
	int64							actualPersistentSerialNum;
	ItemPointerData 				previousFreeTid;

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);
	
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for update relation physically truncated is invalid (0,0)",
			 PersistentFileSysObjName_TypeAndObjectName(fsObjName));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for update relation physically truncated is invalid (0)",
			 PersistentFileSysObjName_TypeAndObjectName(fsObjName));

	values = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));
		 
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;
		 
	PersistentFileSysObj_ReadTuple(
							PersistentFsObjType_RelationFile,
							persistentTid,
							values,
							&tupleCopy);

	GpPersistentRelationNode_GetValues(
									values,
									&actualRelFileNode.spcNode,
									&actualRelFileNode.dbNode,
									&actualRelFileNode.relNode,
									&actualSegmentFileNum,
									&relationStorageManager,
									&persistentState,
									&mirrorExistenceState,
									&mirrorDataSynchronizationState,
									&mirrorBufpoolPhysicallyTruncated,
									&mirrorBufpoolResyncCkptLoc,
									&mirrorBufpoolResyncCkptBlockNum,
									&mirrorAppendOnlyLossEof,
									&mirrorAppendOnlyNewEof,
									&parentXid,
									&actualPersistentSerialNum,
									&previousFreeTid);
	

	if (persistentSerialNum != actualPersistentSerialNum)
		elog(ERROR, "Persistent '%s' serial number mismatch for update physically truncated (expected " INT64_FORMAT ", found " INT64_FORMAT ")",
			 PersistentFileSysObjName_TypeAndObjectName(fsObjName),
			 persistentSerialNum,
			 actualPersistentSerialNum);

	if (relationStorageManager != PersistentFileSysRelStorageMgr_BufferPool)
		elog(ERROR, "Persistent '%s' relation storage manager mismatch for update physically truncated (expected '%s', found '%s')",
			 PersistentFileSysObjName_TypeAndObjectName(fsObjName),
			 PersistentFileSysRelStorageMgr_Name(PersistentFileSysRelStorageMgr_BufferPool),
			 PersistentFileSysRelStorageMgr_Name(relationStorageManager));

	PersistentFileSysObj_UpdateTupleMirrorBufPoolPhysicallyTruncated(
											fileSysObjData,
											fileSysObjSharedData,
											tupleCopy,
											persistentTid,
											persistentSerialNum,
											/* newmirrorBufpoolPhysicallyTruncated */ true,
											flushToXLog);
	
	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
	
	pfree(values);
}

bool PersistentFileSysObj_CheckForAppendOnlyCatchup(
	RelFileNode					*relFileNode,

	int32						segmentFileNum,

	ItemPointer 				persistentTid,

	int64						persistentSerialNum)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;
	
	PersistentFileSysObjName 		fsObjName;

	Datum *values;

	HeapTuple tupleCopy;

	RelFileNode 					actualRelFileNode;
	int32							actualSegmentFileNum;
	
	PersistentFileSysRelStorageMgr	relationStorageManager;
	PersistentFileSysState			persistentState;
	MirroredObjectExistenceState	mirrorExistenceState;
	MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
	bool							mirrorBufpoolPhysicallyTruncated;
	XLogRecPtr						mirrorBufpoolResyncCkptLoc;
	BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
	int64							mirrorAppendOnlyLossEof;
	int64							mirrorAppendOnlyNewEof;
	TransactionId					parentXid;
	int64							actualPersistentSerialNum;
	ItemPointerData 				previousFreeTid;

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName,
										relFileNode,
										segmentFileNum);

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);
	
	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for update relation physically truncated is invalid (0,0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for update relation physically truncated is invalid (0)",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName));

	values = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;
	
	PersistentFileSysObj_ReadTuple(
							PersistentFsObjType_RelationFile,
							persistentTid,
							values,
							&tupleCopy);

	GpPersistentRelationNode_GetValues(
									values,
									&actualRelFileNode.spcNode,
									&actualRelFileNode.dbNode,
									&actualRelFileNode.relNode,
									&actualSegmentFileNum,
									&relationStorageManager,
									&persistentState,
									&mirrorExistenceState,
									&mirrorDataSynchronizationState,
									&mirrorBufpoolPhysicallyTruncated,
									&mirrorBufpoolResyncCkptLoc,
									&mirrorBufpoolResyncCkptBlockNum,
									&mirrorAppendOnlyLossEof,
									&mirrorAppendOnlyNewEof,
									&parentXid,
									&actualPersistentSerialNum,
									&previousFreeTid);

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	pfree(values);
	
	if (persistentSerialNum != actualPersistentSerialNum)
		elog(ERROR, "Persistent '%s' serial number mismatch for check for Append-Only catch-up (expected " INT64_FORMAT ", found " INT64_FORMAT ")",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName),
			 persistentSerialNum,
			 actualPersistentSerialNum);

	if (relationStorageManager != PersistentFileSysRelStorageMgr_AppendOnly)
		elog(ERROR, "Persistent '%s' relation storage manager mismatch for check for Append-Only catch-up (expected '%s', found '%s')",
			 PersistentFileSysObjName_TypeAndObjectName(&fsObjName),
			 PersistentFileSysRelStorageMgr_Name(PersistentFileSysRelStorageMgr_AppendOnly),
			 PersistentFileSysRelStorageMgr_Name(relationStorageManager));

	return (mirrorAppendOnlyLossEof < mirrorAppendOnlyNewEof);

}

typedef enum StateAction
{
	StateAction_None = 0,
	StateAction_MarkWholeMirrorFullCopy = 1,
	StateAction_MirrorReCreate = 2,
	StateAction_MarkMirrorReCreated = 3,
	StateAction_MirrorReDrop = 4,
	MaxStateAction /* must always be last */
} StateAction;

static void PersistentFileSysObj_ScanStateAction(
	StateAction				stateAction,

	PersistentFsObjType		fsObjType)
{
	PersistentFileSysObjData *fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;

	PersistentStoreScan storeScan;
	 
	Datum *values;
	 
	ItemPointerData persistentTid;
	int64 persistentSerialNum;
	 
	PersistentFileSysState  		state;
	PersistentFileSysObjName		fsObjName;
	MirroredObjectExistenceState	mirrorExistenceState;
	TransactionId				 	parentXid;
	int64						 	serialNum;

	PersistentFileSysObj_GetDataPtrs(
								fsObjType,
								&fileSysObjData,
								&fileSysObjSharedData);
	 
	values = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		 
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
		GpPersistent_GetCommonValues(
								fsObjType,
								values,
								&fsObjName,
								&state,
								&mirrorExistenceState,
								&parentXid,
								&serialNum);

		switch (stateAction)
		{
		case StateAction_MarkWholeMirrorFullCopy:
			{
				MirroredObjectExistenceState newMirrorExistenceState;

				switch (state)
				{
				case PersistentFileSysState_Free:
					continue;

				case PersistentFileSysState_CreatePending:
				case PersistentFileSysState_Created:
					newMirrorExistenceState = MirroredObjectExistenceState_MirrorReCreateNeeded;
					break;

				case PersistentFileSysState_DropPending:
				case PersistentFileSysState_AbortingCreate:
					newMirrorExistenceState = MirroredObjectExistenceState_NotMirrored;
					break;

				default:
					elog(ERROR, "Unexpected persistent file-system object state: %d",
						 state);
                    newMirrorExistenceState = MaxMirroredObjectExistenceState;
				}

				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "StateAction_MarkWholeMirrorFullCopy: Mark '%s' full copy, persistent state '%s', old mirror existence state '%s', new mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 PersistentFileSysObjState_Name(state),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 MirroredObjectExistenceState_Name(newMirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));

				PersistentFileSysObj_UpdateTupleMirrorExistenceState(
														fileSysObjData,
														fileSysObjSharedData,
														fsObjType,
														PersistentStore_GetScanTupleCopy(&storeScan),
														&persistentTid,
														persistentSerialNum,
														newMirrorExistenceState,
														MirroredRelDataSynchronizationState_FullCopy,
														/* flushToXLog */ false);
			}
			break;
			
		case StateAction_MirrorReCreate:
			if ((state == PersistentFileSysState_CreatePending ||
				 state == PersistentFileSysState_Created ||
				 state == PersistentFileSysState_JustInTimeCreatePending) 
				&&
				mirrorExistenceState == MirroredObjectExistenceState_MirrorReCreateNeeded)
			{
				MirrorDataLossTrackingState mirrorDataLossTrackingState;
				int64						mirrorDataLossTrackingSessionNum;
				
				bool mirrorDataLossOccurred;

				PersistentFileSysRelStorageMgr relStorageMgr = PersistentFileSysRelStorageMgr_None;
				
				if (fsObjType == PersistentFsObjType_RelationFile)
					relStorageMgr = 
						(PersistentFileSysRelStorageMgr)
										DatumGetInt16(
												values[Anum_gp_persistent_relation_node_relation_storage_manager - 1]);

				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "StateAction_MirrorReCreate: Attempt '%s' mirror re-create, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 PersistentFileSysObjState_Name(state),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));
				
				LWLockRelease(PersistentObjLock);	// Don't hold lock during physical file-system operation.
				
				// UNDONE: Pass this in from the resync worker?
				mirrorDataLossTrackingState = 
							FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
															&mirrorDataLossTrackingSessionNum);
				FileRepResync_SetReCreateRequest();
				
				PersistentFileSysObj_DoMirrorReCreate(
										&fsObjName,
										relStorageMgr,
										&persistentTid,
										persistentSerialNum,
										mirrorDataLossTrackingState,
										mirrorDataLossTrackingSessionNum,
										&mirrorDataLossOccurred);
				
				FileRepResync_ResetReCreateRequest();

				// UNDONE: Act on mirrorDataLossOccurred
				
				LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
				
			}
			else
			{
				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "StateAction_MirrorReCreate: Skip '%s' mirror re-create, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 PersistentFileSysObjState_Name(state),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));
			}
			break;
			
		case StateAction_MarkMirrorReCreated:
			if ((state == PersistentFileSysState_CreatePending ||
				 state == PersistentFileSysState_Created ||
				 state == PersistentFileSysState_JustInTimeCreatePending) 
				&&
				mirrorExistenceState == MirroredObjectExistenceState_MirrorReCreateNeeded)
			{
				PersistentFileSysRelStorageMgr relStorageMgr = PersistentFileSysRelStorageMgr_None;
				
				if (fsObjType == PersistentFsObjType_RelationFile)
					relStorageMgr = 
						(PersistentFileSysRelStorageMgr)
										DatumGetInt16(
												values[Anum_gp_persistent_relation_node_relation_storage_manager - 1]);

				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "StateAction_MarkMirrorReCreated: Mark '%s' mirror re-created, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 PersistentFileSysObjState_Name(state),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));
				
				PersistentFileSysObj_UpdateTupleMirrorReCreated(
														fileSysObjData,
														fileSysObjSharedData,
														fsObjType,
														PersistentStore_GetScanTupleCopy(&storeScan),
														&persistentTid,
														persistentSerialNum,
														relStorageMgr,
														/* flushToXLog */ false);
			}
			else
			{
				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "StateAction_MarkMirrorReCreated: Skip '%s' mirror re-create, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 PersistentFileSysObjState_Name(state),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));
			}
			break;
			
		case StateAction_MirrorReDrop:
			if ((state == PersistentFileSysState_DropPending ||
				 state == PersistentFileSysState_AbortingCreate) 
				&&
				mirrorExistenceState == MirroredObjectExistenceState_MirrorReDropNeeded)
			{
				MirrorDataLossTrackingState mirrorDataLossTrackingState;
				int64						mirrorDataLossTrackingSessionNum;
				
				PersistentFileSysRelStorageMgr relStorageMgr = PersistentFileSysRelStorageMgr_None;

				bool mirrorDataLossOccurred;
					
				if (fsObjType == PersistentFsObjType_RelationFile)
					relStorageMgr = 
						(PersistentFileSysRelStorageMgr)
										DatumGetInt16(
												values[Anum_gp_persistent_relation_node_relation_storage_manager - 1]);

				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "StateAction_MirrorReDrop: Attempt '%s' mirror re-drop, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 PersistentFileSysObjState_Name(state),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));

				
				LWLockRelease(PersistentObjLock);	// Don't hold lock during physical file-system operation.
				
				// UNDONE: Pass this in from the resync worker?
				mirrorDataLossTrackingState = 
							FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
															&mirrorDataLossTrackingSessionNum);
				PersistentFileSysObj_DoMirrorReDrop(
										&fsObjName,
										relStorageMgr,
										&persistentTid,
										persistentSerialNum,
										mirrorDataLossTrackingState,
										mirrorDataLossTrackingSessionNum,
										&mirrorDataLossOccurred);

				// UNDONE: Act on mirrorDataLossOccurred
				
				LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
				
			}
			else
			{
				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "StateAction_MirrorReDrop: Skip '%s' mirror re-drop, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 PersistentFileSysObjState_Name(state),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));
			}
			break;

		default:
			elog(ERROR, "Unexpected state-action: %d",
				 stateAction);
		}

		LWLockRelease(PersistentObjLock);
		
		LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		
	}

	LWLockRelease(PersistentObjLock);

	PersistentStore_EndScan(&storeScan);

	pfree(values);
}

void PersistentFileSysObj_MarkWholeMirrorFullCopy(void)
{
	PersistentFsObjType		fsObjType;
	
	for (fsObjType = PersistentFsObjType_First; 
		 fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
 	{
 		PersistentFileSysObj_ScanStateAction(
										StateAction_MarkWholeMirrorFullCopy,
										fsObjType);
 	}
}

void PersistentFileSysObj_MirrorReCreate(void)
{
	PersistentFsObjType		fsObjType;

	/*
	 * For CREATE, we go top-down.
	 */
	fsObjType = PersistentFsObjType_Last;
	while (true)
	{
		/*
		 * Since PersistentFsObjType is an enumeration type and treated by the
		 * compiler as unsigned, we must be careful to not decrement below 0...
		 * Or, it will wrap-around to 4 billion or negative 2 billion.  Didn't PASCAL
		 * have cool iterators?  Perhaps not.
		 */
		PersistentFileSysObj_ScanStateAction(
										StateAction_MirrorReCreate,
										fsObjType);
		if (fsObjType == PersistentFsObjType_First)
			break;
		fsObjType--;
	}
}

void PersistentFileSysObj_MarkMirrorReCreated(void)
{
	PersistentFsObjType		fsObjType;

	/*
	 * For CREATE, we go top-down.
	 */
	fsObjType = PersistentFsObjType_Last;
	while (true)
	{
		/*
		 * Since PersistentFsObjType is an enumeration type and treated by the
		 * compiler as unsigned, we must be careful to not decrement below 0...
		 * Or, it will wrap-around to 4 billion or negative 2 billion.  Didn't PASCAL
		 * have cool iterators?  Perhaps not.
		 */
		PersistentFileSysObj_ScanStateAction(
										StateAction_MarkMirrorReCreated,
										fsObjType);
		if (fsObjType == PersistentFsObjType_First)
			break;
		fsObjType--;
	}
}


void PersistentFileSysObj_MirrorReDrop(void)
{
	PersistentFsObjType 	fsObjType;
	
	/*
	 * For DROP, we go bottom-up.
	 */
	for (fsObjType = PersistentFsObjType_First; 
		 fsObjType <= PersistentFsObjType_Last;
		 fsObjType++)
	{
		PersistentFileSysObj_ScanStateAction(
										StateAction_MirrorReDrop,
										fsObjType);
	}
}

void PersistentFileSysObj_MarkWholeMirrorScanIncremental(void)
{
	PersistentFileSysObjData *fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;

	PersistentStoreScan storeScan;
	 
	Datum values[Natts_gp_persistent_relation_node];
	 
	ItemPointerData persistentTid;
	int64 persistentSerialNum;

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		 
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
		RelFileNode 					relFileNode;
		int32 							segmentFileNum;
		
		PersistentFileSysRelStorageMgr	relationStorageManager;
		PersistentFileSysState			persistentState;
		MirroredObjectExistenceState	mirrorExistenceState;
		MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
		bool 							mirrorBufpoolPhysicallyTruncated;
		XLogRecPtr						mirrorBufpoolResyncCkptLoc;
		BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
		int64							mirrorAppendOnlyLossEof;
		int64							mirrorAppendOnlyNewEof;
		TransactionId					parentXid;
		int64							serialNum;
		ItemPointerData					previousFreeTid;
		
		PersistentFileSysObjName		fsObjName;

		GpPersistentRelationNode_GetValues(
										values,
										&relFileNode.spcNode,
										&relFileNode.dbNode,
										&relFileNode.relNode,
										&segmentFileNum,
										&relationStorageManager,
										&persistentState,
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

		PersistentFileSysObjName_SetRelationFile(
											&fsObjName,
											&relFileNode,
											segmentFileNum);

		if ((persistentState == PersistentFileSysState_CreatePending ||
			 persistentState == PersistentFileSysState_Created) 
			&&
			(mirrorExistenceState == MirroredObjectExistenceState_MirrorCreatePending ||
			 mirrorExistenceState == MirroredObjectExistenceState_MirrorCreated) 
			&&
			relationStorageManager == PersistentFileSysRelStorageMgr_BufferPool)
		{
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_MarkWholeMirrorScanIncremental: Mark '%s' needing scan incremental, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
					 PersistentFileSysObjName_ObjectName(&fsObjName),
					 PersistentFileSysObjState_Name(persistentState),
					 MirroredObjectExistenceState_Name(mirrorExistenceState),
					 serialNum,
					 ItemPointerToString(&persistentTid));

			PersistentFileSysObj_UpdateTupleMirrorExistenceState(
													fileSysObjData,
													fileSysObjSharedData,
													PersistentFsObjType_RelationFile,
													PersistentStore_GetScanTupleCopy(&storeScan),
													&persistentTid,
													persistentSerialNum,
													mirrorExistenceState,
													MirroredRelDataSynchronizationState_BufferPoolScanIncremental,
													/* flushToXLog */ false);
		}
		else
		{
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_MarkWholeMirrorScanIncremental: Skipping '%s' for scan incremental, relation storage manager '%s', persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
					 PersistentFileSysObjName_ObjectName(&fsObjName),
					 PersistentFileSysRelStorageMgr_Name(relationStorageManager),
					 PersistentFileSysObjState_Name(persistentState),
					 MirroredObjectExistenceState_Name(mirrorExistenceState),
					 serialNum,
					 ItemPointerToString(&persistentTid));
		}

		LWLockRelease(PersistentObjLock);
		
		LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		
	}

	LWLockRelease(PersistentObjLock);

	PersistentStore_EndScan(&storeScan);
}

/*
 * Mark the special persistent tables as needing 'Scan Incremental' recovery.
 */
void PersistentFileSysObj_MarkSpecialScanIncremental(void)
{
	PersistentFileSysObjData *fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;

	PersistentStoreScan storeScan;
	 
	Datum values[Natts_gp_persistent_relation_node];
	 
	ItemPointerData persistentTid;
	int64 persistentSerialNum;

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		 
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
		RelFileNode 					relFileNode;
		int32 							segmentFileNum;
		
		PersistentFileSysRelStorageMgr	relationStorageManager;
		PersistentFileSysState			persistentState;
		MirroredObjectExistenceState	mirrorExistenceState;
		MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
		bool							mirrorBufpoolPhysicallyTruncated;
		XLogRecPtr						mirrorBufpoolResyncCkptLoc;
		BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
		int64							mirrorAppendOnlyLossEof;
		int64							mirrorAppendOnlyNewEof;
		TransactionId					parentXid;
		int64							serialNum;
		ItemPointerData					previousFreeTid;
		
		PersistentFileSysObjName		fsObjName;

		GpPersistentRelationNode_GetValues(
										values,
										&relFileNode.spcNode,
										&relFileNode.dbNode,
										&relFileNode.relNode,
										&segmentFileNum,
										&relationStorageManager,
										&persistentState,
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

		PersistentFileSysObjName_SetRelationFile(
											&fsObjName,
											&relFileNode,
											segmentFileNum);

		/*
		 * We need 'Scan Incremental' for the special relations that we don't cover with the
		 * change tracking log.
		 */
		if (GpPersistent_SkipXLogInfo(relFileNode.relNode))
		{
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_MarkSpecialScanIncremental: Mark '%s' needing special scan incremental, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
					 PersistentFileSysObjName_ObjectName(&fsObjName),
					 PersistentFileSysObjState_Name(persistentState),
					 MirroredObjectExistenceState_Name(mirrorExistenceState),
					 serialNum,
					 ItemPointerToString(&persistentTid));

			PersistentFileSysObj_UpdateTupleMirrorExistenceState(
													fileSysObjData,
													fileSysObjSharedData,
													PersistentFsObjType_RelationFile,
													PersistentStore_GetScanTupleCopy(&storeScan),
													&persistentTid,
													persistentSerialNum,
													mirrorExistenceState,
													MirroredRelDataSynchronizationState_BufferPoolScanIncremental,
													/* flushToXLog */ false);
		}
		else if (mirrorBufpoolPhysicallyTruncated)
		{
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_MarkSpecialScanIncremental: Mark '%s' vacuumed relation as scan incremental, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
					 PersistentFileSysObjName_ObjectName(&fsObjName),
					 PersistentFileSysObjState_Name(persistentState),
					 MirroredObjectExistenceState_Name(mirrorExistenceState),
					 serialNum,
					 ItemPointerToString(&persistentTid));

			PersistentFileSysObj_UpdateTupleMirrorExistenceState(
													fileSysObjData,
													fileSysObjSharedData,
													PersistentFsObjType_RelationFile,
													PersistentStore_GetScanTupleCopy(&storeScan),
													&persistentTid,
													persistentSerialNum,
													mirrorExistenceState,
													MirroredRelDataSynchronizationState_BufferPoolScanIncremental,
													/* flushToXLog */ false);
			PersistentFileSysObj_UpdateTupleMirrorBufPoolPhysicallyTruncated(
													fileSysObjData,
													fileSysObjSharedData,
													PersistentStore_GetScanTupleCopy(&storeScan),
													&persistentTid,
													persistentSerialNum,
													/* newMirrorBufPoolPhysicallyTruncated */ false,
													/* flushToXLog */ false);
		}

		LWLockRelease(PersistentObjLock);
		
		LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		
	}

	LWLockRelease(PersistentObjLock);

	PersistentStore_EndScan(&storeScan);
}

/*
 * Mark any Append-Only tables that have mirror data loss as needing 'Append-Only Catch-Up' recovery.
 */
void PersistentFileSysObj_MarkAppendOnlyCatchup(void)
{
	PersistentFileSysObjData *fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;

	PersistentStoreScan storeScan;
	 
	Datum values[Natts_gp_persistent_relation_node];
	 
	ItemPointerData persistentTid;
	int64 persistentSerialNum;

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		 
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
		RelFileNode 					relFileNode;
		int32 							segmentFileNum;
		
		PersistentFileSysRelStorageMgr	relationStorageManager;
		PersistentFileSysState			persistentState;
		MirroredObjectExistenceState	mirrorExistenceState;
		MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
		bool							mirrorBufpoolPhysicallyTruncated;
		XLogRecPtr						mirrorBufpoolResyncCkptLoc;
		BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
		int64							mirrorAppendOnlyLossEof;
		int64							mirrorAppendOnlyNewEof;
		TransactionId					parentXid;
		int64							serialNum;
		ItemPointerData					previousFreeTid;
		
		PersistentFileSysObjName		fsObjName;

		GpPersistentRelationNode_GetValues(
										values,
										&relFileNode.spcNode,
										&relFileNode.dbNode,
										&relFileNode.relNode,
										&segmentFileNum,
										&relationStorageManager,
										&persistentState,
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

		if (persistentState == PersistentFileSysState_Free)
			continue;

		PersistentFileSysObjName_SetRelationFile(
											&fsObjName,
											&relFileNode,
											segmentFileNum);

		if (persistentState == PersistentFileSysState_Created 
			&&
			relationStorageManager == PersistentFileSysRelStorageMgr_AppendOnly)
		{
			if (mirrorAppendOnlyLossEof < mirrorAppendOnlyNewEof)
			{
				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "PersistentFileSysObj_MarkAppendOnlyCatchup: Mark '%s' Append-Only relation has needing catch-up (mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT "), persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 mirrorAppendOnlyLossEof,
						 mirrorAppendOnlyNewEof,
						 PersistentFileSysObjState_Name(persistentState),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));

				PersistentFileSysObj_UpdateTupleMirrorExistenceState(
														fileSysObjData,
														fileSysObjSharedData,
														PersistentFsObjType_RelationFile,
														PersistentStore_GetScanTupleCopy(&storeScan),
														&persistentTid,
														persistentSerialNum,
														mirrorExistenceState,
														MirroredRelDataSynchronizationState_AppendOnlyCatchup,
														/* flushToXLog */ false);
			}
			else
			{
				if (Debug_persistent_print)
					elog(Debug_persistent_print_level, 
						 "PersistentFileSysObj_MarkAppendOnlyCatchup: Found '%s' Append-Only relation not needing catch-up (mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT "), persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
						 PersistentFileSysObjName_ObjectName(&fsObjName),
						 mirrorAppendOnlyLossEof,
						 mirrorAppendOnlyNewEof,
						 PersistentFileSysObjState_Name(persistentState),
						 MirroredObjectExistenceState_Name(mirrorExistenceState),
						 serialNum,
						 ItemPointerToString(&persistentTid));
			}

		}
		else
		{
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "PersistentFileSysObj_MarkAppendOnlyCatchup: Skipping '%s' for Append-Only, relation storage manager '%s', persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
					 PersistentFileSysObjName_ObjectName(&fsObjName),
					 PersistentFileSysRelStorageMgr_Name(relationStorageManager),
					 PersistentFileSysObjState_Name(persistentState),
					 MirroredObjectExistenceState_Name(mirrorExistenceState),
					 serialNum,
					 ItemPointerToString(&persistentTid));
		}

		LWLockRelease(PersistentObjLock);
		
		LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		
	}

	LWLockRelease(PersistentObjLock);

	PersistentStore_EndScan(&storeScan);
}


static void PersistentFileSysObj_MarkPageIncremental(
	ItemPointer			persistentTid,

	int64				persistentSerialNum)
{
	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	Datum values[Natts_gp_persistent_relation_node];

	HeapTuple tupleCopy;

	RelFileNode 					relFileNode;
	int32							segmentFileNum;
	
	PersistentFileSysRelStorageMgr	relationStorageManager;
	PersistentFileSysState			persistentState;
	MirroredObjectExistenceState	mirrorExistenceState;
	MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
	bool							mirrorBufpoolPhysicallyTruncated;
	XLogRecPtr						mirrorBufpoolResyncCkptLoc;
	BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
	int64							mirrorAppendOnlyLossEof;
	int64							mirrorAppendOnlyNewEof;
	TransactionId					parentXid;
	int64							serialNum;
	ItemPointerData 				previousFreeTid;

	PersistentFileSysObjName		fsObjName;

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for marking page incremental is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for update mirror existence state is invalid (0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFileSysObj_ReadTuple(
							PersistentFsObjType_RelationFile,
							persistentTid,
							values,
							&tupleCopy);


	GpPersistentRelationNode_GetValues(
									values,
									&relFileNode.spcNode,
									&relFileNode.dbNode,
									&relFileNode.relNode,
									&segmentFileNum,
									&relationStorageManager,
									&persistentState,
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

	if (persistentState == PersistentFileSysState_Free)
	{
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "PersistentFileSysObj_MarkPageIncremental: Skipping 'Free' entry (input serial number " INT64_FORMAT ", actual serial number " INT64_FORMAT ") at TID %s",
				 persistentSerialNum,
				 serialNum,
				 ItemPointerToString(persistentTid));
		
		heap_freetuple(tupleCopy);
		
		LWLockRelease(PersistentObjLock);
		
		return;
	}

	if (serialNum != persistentSerialNum)
	{
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "PersistentFileSysObj_MarkPageIncremental: Skipping obsolete serial number (input serial number " INT64_FORMAT ", actual serial number " INT64_FORMAT ") at TID %s",
				 persistentSerialNum,
				 serialNum,
				 ItemPointerToString(persistentTid));

		heap_freetuple(tupleCopy);
		
		LWLockRelease(PersistentObjLock);
		
		return;
	}

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName,
										&relFileNode,
										segmentFileNum);

	if (persistentState == PersistentFileSysState_Created 
		&&
		relationStorageManager == PersistentFileSysRelStorageMgr_BufferPool)
	{
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "PersistentFileSysObj_MarkPageIncremental: Mark '%s' needing page incremental, persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
				 PersistentFileSysObjName_ObjectName(&fsObjName),
				 PersistentFileSysObjState_Name(persistentState),
				 MirroredObjectExistenceState_Name(mirrorExistenceState),
				 serialNum,
				 ItemPointerToString(persistentTid));
		
		PersistentFileSysObj_UpdateTupleMirrorExistenceState(
												fileSysObjData,
												fileSysObjSharedData,
												PersistentFsObjType_RelationFile,
												tupleCopy,
												persistentTid,
												persistentSerialNum,
												mirrorExistenceState,
												MirroredRelDataSynchronizationState_BufferPoolPageIncremental,
												/* flushToXLog */ false);
	}
	else
	{
		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "PersistentFileSysObj_MarkPageIncremental: Skipping '%s' for page incremental, relation storage manager '%s', persistent state '%s', mirror existence state '%s', serial number " INT64_FORMAT " at TID %s",
				 PersistentFileSysObjName_ObjectName(&fsObjName),
				 PersistentFileSysRelStorageMgr_Name(relationStorageManager),
				 PersistentFileSysObjState_Name(persistentState),
				 MirroredObjectExistenceState_Name(mirrorExistenceState),
				 serialNum,
				 ItemPointerToString(persistentTid));

		heap_freetuple(tupleCopy);
	}

	LWLockRelease(PersistentObjLock);

}

void PersistentFileSysObj_MarkPageIncrementalFromChangeLog(void)
{
	IncrementalChangeList *incrementalChangeList;
	int count = 0;
	int i;
	
	incrementalChangeList = ChangeTracking_GetIncrementalChangeList();
	
	if (incrementalChangeList != NULL)
	{
		count = incrementalChangeList->count;
	}

	elog(LOG, "ChangeTracking_GetIncrementalChangeList returned %d entries", count);

	for (i = 0; i < count; i++)
	{
		if (PersistentStore_IsZeroTid(&incrementalChangeList->entries[i].persistentTid))
		{
			elog(WARNING, "TID for persistent '%s' tuple from change log is invalid (0,0)",
				 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));
			continue;
		}
		
		if (incrementalChangeList->entries[i].persistentSerialNum == 0)
		{
			elog(WARNING, "Persistent '%s' serial number from change log is invalid (0)",
				 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));
			continue;
		}
		
		elog(LOG, "marking page incremental for SN " INT64_FORMAT, incrementalChangeList->entries[i].persistentSerialNum);
		PersistentFileSysObj_MarkPageIncremental(
											&incrementalChangeList->entries[i].persistentTid,
											incrementalChangeList->entries[i].persistentSerialNum);
	}

	if (incrementalChangeList != NULL)
	{
		ChangeTracking_FreeIncrementalChangeList(incrementalChangeList);
	}
}

bool PersistentFileSysObj_ResynchronizeScan(
	ResynchronizeScanToken			*resynchronizeScanToken,

	RelFileNode						*relFileNode,

	int32							*segmentFileNum,

	PersistentFileSysRelStorageMgr 	*relStorageMgr,

	MirroredRelDataSynchronizationState *mirrorDataSynchronizationState,

	XLogRecPtr						*mirrorBufpoolResyncCkptLoc,

	BlockNumber 					*mirrorBufpoolResyncCkptBlockNum,

	int64							*mirrorAppendOnlyLossEof,

	int64							*mirrorAppendOnlyNewEof,

	ItemPointer						persistentTid,

	int64							*persistentSerialNum)
{
	PersistentFileSysObjData *fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;

	Datum values[Natts_gp_persistent_relation_node];
	 
	PersistentFileSysState			state;
	MirroredObjectExistenceState	mirrorExistenceState;
	bool							mirrorBufpoolPhysicallyTruncated;
	TransactionId					parentXid;
	int64							serialNum;
	ItemPointerData 				previousFreeTid;

	if (resynchronizeScanToken->done)
		return false;

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	if (!resynchronizeScanToken->beginScan)
	{
		PersistentStore_BeginScan(
							&fileSysObjData->storeData,
							&fileSysObjSharedData->storeSharedData,
							&resynchronizeScanToken->storeScan);
		
		resynchronizeScanToken->beginScan = true;
	}

	while (true)
	{
		if (!PersistentStore_GetNext(
								&resynchronizeScanToken->storeScan,
								values,
								persistentTid,
								persistentSerialNum))
		{
			PersistentStore_EndScan(&resynchronizeScanToken->storeScan);
			resynchronizeScanToken->beginScan = false;
			resynchronizeScanToken->done = true;
			
			LWLockRelease(PersistentObjLock);
			
			return false;
		}

		GpPersistentRelationNode_GetValues(
										values,
										&relFileNode->spcNode,
										&relFileNode->dbNode,
										&relFileNode->relNode,
										segmentFileNum,
										relStorageMgr,
										&state,
										&mirrorExistenceState,
										mirrorDataSynchronizationState,
										&mirrorBufpoolPhysicallyTruncated,
										mirrorBufpoolResyncCkptLoc,
										mirrorBufpoolResyncCkptBlockNum,
										mirrorAppendOnlyLossEof,
										mirrorAppendOnlyNewEof,
										&parentXid,
										&serialNum,
										&previousFreeTid);
		if (state == PersistentFileSysState_Created)
		{
			// UNDONE: What about pending?

			// UNDONE: Verify mirrorExistenceState is ~_MirrorCreated
			break;
		}

		LWLockRelease(PersistentObjLock);
		
		LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);
		
	}
	
	LWLockRelease(PersistentObjLock);
	
	return true;
}

void PersistentFileSysObj_ResynchronizeAbandonScan(
	ResynchronizeScanToken			*resynchronizeScanToken)
{
	if (resynchronizeScanToken->done)
		return;

	if (resynchronizeScanToken->beginScan)
	{
		PersistentStore_EndScan(&resynchronizeScanToken->storeScan);
		resynchronizeScanToken->beginScan = false;
	}

	resynchronizeScanToken->done = true;
}

/*
 * Refetch the resynchronize relation information while under RELATION_RESYNCHRONIZE lock
 * based on its persistent TID and serial number.
 */
bool PersistentFileSysObj_ResynchronizeRefetch(
	RelFileNode						*relFileNode,

	int32							*segmentFileNum,

	ItemPointer						persistentTid,

	int64							persistentSerialNum,

	PersistentFileSysRelStorageMgr 	*relStorageMgr,

	MirroredRelDataSynchronizationState *mirrorDataSynchronizationState,

	XLogRecPtr						*mirrorBufpoolResyncCkptLoc,

	BlockNumber 					*mirrorBufpoolResyncCkptBlockNum,

	int64							*mirrorAppendOnlyLossEof,

	int64							*mirrorAppendOnlyNewEof)
{
	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	Datum values[Natts_gp_persistent_relation_node];

	HeapTuple tupleCopy;

	RelFileNode 					actualRelFileNode;
	int32							actualSegmentFileNum;
	
	PersistentFileSysState			persistentState;
	MirroredObjectExistenceState	mirrorExistenceState;
	bool							mirrorBufpoolPhysicallyTruncated;
	TransactionId					parentXid;
	int64							serialNum;
	ItemPointerData 				previousFreeTid;

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for marking page incremental is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for update mirror existence state is invalid (0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFileSysObj_ReadTuple(
							PersistentFsObjType_RelationFile,
							persistentTid,
							values,
							&tupleCopy);

	GpPersistentRelationNode_GetValues(
									values,
									&actualRelFileNode.spcNode,
									&actualRelFileNode.dbNode,
									&actualRelFileNode.relNode,
									&actualSegmentFileNum,
									relStorageMgr,
									&persistentState,
									&mirrorExistenceState,
									mirrorDataSynchronizationState,
									&mirrorBufpoolPhysicallyTruncated,
									mirrorBufpoolResyncCkptLoc,
									mirrorBufpoolResyncCkptBlockNum,
									mirrorAppendOnlyLossEof,
									mirrorAppendOnlyNewEof,
									&parentXid,
									&serialNum,
									&previousFreeTid);

	heap_freetuple(tupleCopy);

	LWLockRelease(PersistentObjLock);

	if (persistentState == PersistentFileSysState_Free)
		return false;

	if (persistentSerialNum != serialNum)
		return false;

	return true;
}

void PersistentFileSysObj_ResynchronizeBufferPoolCkpt(
	ItemPointer 					persistentTid,

	int64							persistentSerialNum,

	XLogRecPtr						mirrorBufpoolResyncCkptLoc,

	BlockNumber 					mirrorBufpoolResyncCkptBlockNum,

	bool							flushToXLog)
{
	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	Datum values[Natts_gp_persistent_relation_node];

	HeapTuple tupleCopy;

	Datum newValues[Natts_gp_persistent_relation_node];
	bool replaces[Natts_gp_persistent_relation_node];

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for resynchronize Buffer Pool checkpoint is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for resynchronize Buffer Pool checkpoint is invalid (0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFileSysObj_ReadTuple(
							PersistentFsObjType_RelationFile,
							persistentTid,
							values,
							&tupleCopy);

	MemSet(newValues, 0, Natts_gp_persistent_relation_node * sizeof(Datum));
	MemSet(replaces, 0, Natts_gp_persistent_relation_node * sizeof(bool));
	
	replaces[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_loc - 1] = true;
	newValues[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_loc - 1] =
									PointerGetDatum(&mirrorBufpoolResyncCkptLoc);

	replaces[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_block_num - 1] = true;
	newValues[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_block_num - 1] = 
									Int32GetDatum(mirrorBufpoolResyncCkptBlockNum);

	PersistentFileSysObj_ReplaceTuple(
								PersistentFsObjType_RelationFile,
								persistentTid,
								tupleCopy,
								newValues,
								replaces,
								flushToXLog);

	LWLockRelease(PersistentObjLock);

}

void PersistentFileSysObj_ResynchronizeRelationComplete(
	ItemPointer 					persistentTid,

	int64							persistentSerialNum,
														
	int64							mirrorLossEof,
														
	bool							flushToXLog)
{
	PersistentFileSysObjData 		*fileSysObjData;
	PersistentFileSysObjSharedData 	*fileSysObjSharedData;

	Datum values[Natts_gp_persistent_relation_node];

	HeapTuple tupleCopy;

	Datum newValues[Natts_gp_persistent_relation_node];
	bool replaces[Natts_gp_persistent_relation_node];

	XLogRecPtr mirrorBufpoolResyncCkptLoc;

	PersistentFileSysRelStorageMgr relStorageMgr;

	if (PersistentStore_IsZeroTid(persistentTid))
		elog(ERROR, "TID for persistent '%s' tuple for resynchronize relation complete is invalid (0,0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	if (persistentSerialNum == 0)
		elog(ERROR, "Persistent '%s' serial number for resynchronize relation complete is invalid (0)",
			 PersistentFileSysObjName_TypeName(PersistentFsObjType_RelationFile));

	MemSet(&mirrorBufpoolResyncCkptLoc, 0, sizeof(mirrorBufpoolResyncCkptLoc));

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	// NOTE: No need to acquire the MirroredLock for Resynchronize routines.

	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentFileSysObj_ReadTuple(
							PersistentFsObjType_RelationFile,
							persistentTid,
							values,
							&tupleCopy);

	MemSet(newValues, 0, Natts_gp_persistent_relation_node * sizeof(Datum));
	MemSet(replaces, 0, Natts_gp_persistent_relation_node * sizeof(bool));
	
	replaces[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1] = true;
	newValues[Anum_gp_persistent_relation_node_mirror_data_synchronization_state - 1] = 
									Int16GetDatum(MirroredRelDataSynchronizationState_Synchronized);

	relStorageMgr = 
		(PersistentFileSysRelStorageMgr)
						DatumGetInt16(
								values[Anum_gp_persistent_relation_node_relation_storage_manager - 1]);

	if (relStorageMgr == PersistentFileSysRelStorageMgr_BufferPool)
	{
		replaces[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_loc - 1] = true;
		newValues[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_loc - 1] =
										PointerGetDatum(&mirrorBufpoolResyncCkptLoc);
		
		replaces[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_block_num - 1] = true;
		newValues[Anum_gp_persistent_relation_node_mirror_bufpool_resync_ckpt_block_num - 1] = 
										Int32GetDatum(0);
	}
	else if (relStorageMgr == PersistentFileSysRelStorageMgr_AppendOnly)
	{
		int64 mirrorAppendOnlyNewEof;

		mirrorAppendOnlyNewEof = DatumGetInt64(values[Anum_gp_persistent_relation_node_mirror_append_only_new_eof - 1]);
		
		Assert(mirrorLossEof <= mirrorAppendOnlyNewEof);
		
		replaces[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1] = true;
		newValues[Anum_gp_persistent_relation_node_mirror_append_only_loss_eof - 1] = 
										Int64GetDatum(mirrorLossEof);
	}

	PersistentFileSysObj_ReplaceTuple(
								PersistentFsObjType_RelationFile,
								persistentTid,
								tupleCopy,
								newValues,
								replaces,
								flushToXLog);

	LWLockRelease(PersistentObjLock);

}

bool PersistentFileSysObj_ScanForRelation(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the create. */

	int32				segmentFileNum,

	ItemPointer			persistentTid,
				/* Resulting TID of the gp_persistent_rel_files tuple for the relation. */

	int64				*persistentSerialNum)
				/* Resulting serial number for the relation.  Distinquishes the uses of the tuple. */
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentFileSysObjData *fileSysObjData;
	PersistentFileSysObjSharedData	*fileSysObjSharedData;

	PersistentStoreScan storeScan;
	 
	Datum values[Natts_gp_persistent_relation_node];

	bool found;

	if(RelFileNode_IsEmpty(relFileNode))
		elog(ERROR, "Invalid RelFileNode (0,0,0)");

	if (PersistentFileSysObj_BeforePersistenceWork())
	{	
		elog(ERROR, 
		     "Cannot scan for persistent relation %u/%u/%u, segment file #%d because we are before persistence work",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum);
	}

	PersistentFileSysObj_InitScan();

	PersistentFileSysObj_GetDataPtrs(
								PersistentFsObjType_RelationFile,
								&fileSysObjData,
								&fileSysObjSharedData);

	found = false;
	MemSet(persistentTid, 0, sizeof(ItemPointerData));
	*persistentSerialNum = 0;
		 
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;
		 
	PersistentStore_BeginScan(
						&fileSysObjData->storeData,
						&fileSysObjSharedData->storeSharedData,
						&storeScan);

	while (PersistentStore_GetNext(
							&storeScan,
							values,
							persistentTid,
							persistentSerialNum))
	{
		RelFileNode 					candidateRelFileNode;
		int32 							candidateSegmentFileNum;
		
		PersistentFileSysRelStorageMgr	relationStorageManager;
		PersistentFileSysState			persistentState;
		MirroredObjectExistenceState	mirrorExistenceState;
		MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
		bool							mirrorBufpoolPhysicallyTruncated;
		XLogRecPtr						mirrorBufpoolResyncCkptLoc;
		BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
		int64							mirrorAppendOnlyLossEof;
		int64							mirrorAppendOnlyNewEof;
		TransactionId					parentXid;
		int64							serialNum;
		ItemPointerData					previousFreeTid;
		
		GpPersistentRelationNode_GetValues(
										values,
										&candidateRelFileNode.spcNode,
										&candidateRelFileNode.dbNode,
										&candidateRelFileNode.relNode,
										&candidateSegmentFileNum,
										&relationStorageManager,
										&persistentState,
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

		if (persistentState != PersistentFileSysState_CreatePending &&
			persistentState != PersistentFileSysState_Created)
			continue;

		if (RelFileNodeEquals(candidateRelFileNode, *relFileNode) &&
			candidateSegmentFileNum == segmentFileNum)
		{
			found = true;
			break;
		}

		MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;
		
		MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;
		
	}

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	PersistentStore_EndScan(&storeScan);

	if (found && Debug_persistent_print)
		elog(Debug_persistent_print_level, 
		     "Scan found persistent relation %u/%u/%u, segment file #%d with serial number " INT64_FORMAT " at TID %s",
			 relFileNode->spcNode,
			 relFileNode->dbNode,
			 relFileNode->relNode,
			 segmentFileNum,
			 *persistentSerialNum,
			 ItemPointerToString(persistentTid));

	return found;
}

static Size PersistentFileSysObj_SharedDataSize(void)
{
	return MAXALIGN(sizeof(PersistentFileSysObjPrivateSharedMemory));
}

/*
 * Return the required shared-memory size for this module.
 */
Size PersistentFileSysObj_ShmemSize(void)
{
	Size size = 0;

	/* The shared-memory structure. */
	size = add_size(size, PersistentFileSysObj_SharedDataSize());

	return size;
}

/*
 * Initialize the shared-memory for this module.
 */
void PersistentFileSysObj_ShmemInit(void)
{
	bool found;

	/* Create the shared-memory structure. */
	persistentFileSysObjPrivateSharedData = 
		(PersistentFileSysObjPrivateSharedMemory *)
						ShmemInitStruct("Persist File-Sys Data",
										PersistentFileSysObj_SharedDataSize(),
										&found);

	if (!found)
	{
		persistentFileSysObjPrivateSharedData->needInitScan = true;
	}
}

