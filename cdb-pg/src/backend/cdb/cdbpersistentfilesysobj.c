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
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbmirroredfilesysobj.h"

#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbdirectopen.h"

#include "storage/itemptr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"

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
GpPersistentRelationNodeRelationId,GpPersistentRelationNodeRelType,false);


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
GpPersistentDatabaseNodeRelationId,GpPersistentDatabaseNodeRelType,false);

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
GpPersistentTablespaceNodeRelationId,GpPersistentTablespaceNodeRelType,false);


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
GpPersistentFilespaceNodeRelationId,GpPersistentFilespaceNodeRelType,false);


void PersistentFileSysObj_InitShared(
	PersistentFileSysObjSharedData 	*fileSysObjSharedData)
{
	PersistentStore_InitShared(&fileSysObjSharedData->storeSharedData);
}

void PersistentFileSysObj_Init(
	PersistentFileSysObjData 			*fileSysObjData,
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
						GpGlobalSequence_PersistentRelation,
						PersistentFileSysObj_RelationOpenShared,
						PersistentFileSysObj_RelationClose,
						scanTupleCallback,
						Natts_gp_persistent_relation_node,
						Anum_gp_persistent_relation_node_persistent_serial_num,
						Anum_gp_persistent_relation_node_previous_free_tid);
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentStore_Init(
						storeData,
						GpGlobalSequence_PersistentDatabase,
						PersistentFileSysObj_DatabaseOpenShared,
						PersistentFileSysObj_DatabaseClose,
						scanTupleCallback,
						Natts_gp_persistent_database_node,
						Anum_gp_persistent_database_node_persistent_serial_num,
						Anum_gp_persistent_database_node_previous_free_tid);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentStore_Init(
						storeData,
						GpGlobalSequence_PersistentTablespace,
						PersistentFileSysObj_TablespaceOpenShared,
						PersistentFileSysObj_TablespaceClose,
						scanTupleCallback,
						Natts_gp_persistent_tablespace_node,
						Anum_gp_persistent_tablespace_node_persistent_serial_num,
						Anum_gp_persistent_tablespace_node_previous_free_tid);
		break;
	
	case PersistentFsObjType_FilespaceDir:
		PersistentStore_Init(
						storeData,
						GpGlobalSequence_PersistentFilespace,
						PersistentFileSysObj_FilespaceOpenShared,
						PersistentFileSysObj_FilespaceClose,
						scanTupleCallback,
						Natts_gp_persistent_filespace_node,
						Anum_gp_persistent_filespace_node_persistent_serial_num,
						Anum_gp_persistent_filespace_node_previous_free_tid);
		break;
	
	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjType);
	}
}

void PersistentFileSysObj_InitScanUnderLock(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData)
{
	PersistentStore_InitScanUnderLock(
					&fileSysObjData->storeData,
					&fileSysObjSharedData->storeSharedData,
					fileSysObjData->fsObjType);
}

void PersistentFileSysObj_InitScan(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData)
{
	PersistentStore_InitScan(
					&fileSysObjData->storeData,
					&fileSysObjSharedData->storeSharedData,
					fileSysObjData->fsObjType);
}

void PersistentFileSysObj_FlushXLog(void)
{
	PersistentStore_FlushXLog();
}

static void PersistentFileSysObj_SetDatumValues(
	PersistentFileSysObjData		*fileSysObjData,

	Datum					*values,

	PersistentFileSysObjName	*fsObjName,
	
	PersistentFileSysState	state,
	
	TransactionId			parentXid,
	
	int64					persistentSerialNum,

	ItemPointer				previousFreeTid)
{
	switch (fileSysObjData->fsObjType)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = &fsObjName->variant.relFileNode;

			GpPersistentRelationNode_SetDatumValues(
												values,
												relFileNode->spcNode,
												relFileNode->dbNode,
												relFileNode->relNode,
												state,
												parentXid,
												persistentSerialNum,
												previousFreeTid);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		{
			DbDirNode *dbDirNode = &fsObjName->variant.dbDirNode;

			GpPersistentDatabaseNode_SetDatumValues(
										values,
										dbDirNode->tablespace,
										dbDirNode->database,
										state,
										parentXid,
										persistentSerialNum,
										previousFreeTid);
		}
		break;

	case PersistentFsObjType_TablespaceDir:
		{
			Oid tablespace = fsObjName->variant.tablespace;

			GpPersistentTablespaceNode_SetDatumValues(
										values,
										tablespace,
										state,
										parentXid,
										persistentSerialNum,
										previousFreeTid);
		}
		break;
			
	case PersistentFsObjType_FilespaceDir:
		{
			Oid filespace = fsObjName->variant.filespace;
	
			GpPersistentFilespaceNode_SetDatumValues(
										values,
										filespace,
										state,
										parentXid,
										persistentSerialNum,
										previousFreeTid);
		}
		break;
		
	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fileSysObjData->fsObjType);
	}
		
}

void PersistentFileSysObj_GetValues(
	PersistentFileSysObjData		*fileSysObjData,

	Datum					*values,

	PersistentFileSysObjName	*fsObjName,
	
	PersistentFileSysState	*state,
	
	TransactionId			*parentXid,
	
	int64					*persistentSerialNum)
{
	ItemPointerData previousFreeTid;

	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = fileSysObjData->fsObjType;

	switch (fileSysObjData->fsObjType)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = &fsObjName->variant.relFileNode;

			GpPersistentRelationNode_GetValues(
											values,
											&relFileNode->spcNode,
											&relFileNode->dbNode,
											&relFileNode->relNode,
											state,
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
									state,
									parentXid,
									persistentSerialNum,
									&previousFreeTid);
		}
		break;

	case PersistentFsObjType_TablespaceDir:
		{
			Oid tablespace;

			GpPersistentTablespaceNode_GetValues(
											values,
											&tablespace,
											state,
											parentXid,
											persistentSerialNum,
											&previousFreeTid);

			PersistentFileSysObjName_SetTablespaceDir(fsObjName, tablespace);
		}
	break;
	
	case PersistentFsObjType_FilespaceDir:
		{
			Oid filespace;

			GpPersistentFilespaceNode_GetValues(
											values,
											&filespace,
											state,
											parentXid,
											persistentSerialNum,
											&previousFreeTid);

			PersistentFileSysObjName_SetFilespaceDir(fsObjName, filespace);
		}
	break;

	default:
		Assert(false);
	}
		
}

void PersistentFileSysObj_Scan(
	PersistentFileSysObjData		*fileSysObjData,
	
	PersistentFileSysObjSharedData	*fileSysObjSharedData,

	PersistentStoreScanTupleCallback	scanTupleCallback)
{
	PersistentStore_Scan(
					&fileSysObjData->storeData,
					&fileSysObjSharedData->storeSharedData,
					/* token */ fileSysObjData->fsObjType,
					scanTupleCallback);
					
}

void PersistentFileSysObj_UpdateTuple(
		PersistentFileSysObjData 		*fileSysObjData,
	
		PersistentFileSysObjSharedData	*fileSysObjSharedData,
	
		ItemPointer 			persistentTid,
					/* TID of the stored tuple. */
	
		PersistentFileSysObjName	*fsObjName,
	
		PersistentFileSysState	state,
		
		TransactionId			parentXid,
		
		int64					persistentSerialNum,
					/* Serial number that distinquishes the uses of the tuple. */

		bool					flushToXLog)
					/* When true, the XLOG record for this change will be flushed to disk. */
{
	PersistentStoreData *storeData = &fileSysObjData->storeData;
	PersistentStoreSharedData *storeSharedData = &fileSysObjSharedData->storeSharedData;

	ItemPointerData zeroTid;

	Datum *values;
	
	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	Assert(fsObjName->type == fileSysObjData->fsObjType);

	MemSet(&zeroTid, 0, sizeof(ItemPointerData));

	PersistentFileSysObj_SetDatumValues(
									fileSysObjData,
									values,
									fsObjName,
									state,
									parentXid,
									persistentSerialNum,
									&zeroTid);
	PersistentStore_UpdateTuple(
							storeData,
							storeSharedData,
							persistentTid,
							values,
							flushToXLog);

	pfree(values);
}

void PersistentFileSysObj_ReadTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	ItemPointer					readTid,

	PersistentFileSysObjName	*fsObjName,
	
	PersistentFileSysState		*state,
	
	TransactionId				*parentXid,

	int64						*persistentSerialNum)
{
	PersistentStoreData *storeData = &fileSysObjData->storeData;
	PersistentStoreSharedData *storeSharedData = &fileSysObjSharedData->storeSharedData;

	Datum *values;

	HeapTuple tupleCopy;
	
	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	PersistentStore_ReadTuple(
							storeData,
							storeSharedData,
							readTid,
							values,
							&tupleCopy);
	
	PersistentFileSysObj_GetValues(
								fileSysObjData,
								values,
								fsObjName,
								state,
								parentXid,
								persistentSerialNum);

	heap_freetuple(tupleCopy);

	pfree(values);
}

void PersistentFileSysObj_AddTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	PersistentFileSysObjName	*fsObjName,
	
	PersistentFileSysState		state,
	
	TransactionId				parentXid,

	bool						flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	int64					*persistentSerialNum)
{
	PersistentStoreData *storeData = &fileSysObjData->storeData;
	PersistentStoreSharedData *storeSharedData = &fileSysObjSharedData->storeSharedData;

	ItemPointerData zeroTid;

	Datum *values;
	
	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	Assert(fsObjName->type == fileSysObjData->fsObjType);

	MemSet(&zeroTid, 0, sizeof(ItemPointerData));

	PersistentFileSysObj_SetDatumValues(
									fileSysObjData,
									values,
									fsObjName,
									state,
									parentXid,
									0,
									&zeroTid);
	PersistentStore_AddTuple(
							storeData,
							storeSharedData,
							values,
							flushToXLog,
							persistentTid,
							persistentSerialNum);

	pfree(values);
}

void PersistentFileSysObj_FreeTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	bool						flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
	PersistentStoreData *storeData = &fileSysObjData->storeData;
	PersistentStoreSharedData *storeSharedData = &fileSysObjSharedData->storeSharedData;

	Datum *freeValues;
	
	PersistentFileSysObjName	freeFsObjName;

	ItemPointerData zeroTid;

	MemSet(&freeFsObjName, 0, sizeof(PersistentFileSysObjName));
	freeFsObjName.type = fileSysObjData->fsObjType;

	freeValues = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	MemSet(&zeroTid, 0, sizeof(ItemPointerData));

	PersistentFileSysObj_SetDatumValues(
									fileSysObjData,
									freeValues,
									&freeFsObjName,
									PersistentFileSysState_Free,
									InvalidTransactionId,
									0,
									&zeroTid);
	PersistentStore_FreeTuple(
							storeData,
							storeSharedData,
							persistentTid,
							freeValues,
							flushToXLog);

	pfree(freeValues);
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

	PersistentFileSysState	nextState)
	
{
	char states[100];

	if ((int)expectedState2 == -1)
		sprintf(states, "state '%s'", 
		        PersistentFileSysObj_StateName(expectedState1));
	else
		sprintf(states, "states '%s' or '%s'",
			    PersistentFileSysObj_StateName(expectedState1),
			    PersistentFileSysObj_StateName(expectedState2));

	errcontext( 
		 "State change is for persistent '%s' to go from %s to state '%s' with serial number " INT64_FORMAT " at TID %s",
		 PersistentFileSysObjName_TypeAndObjectName(fsObjName),
		 states,
		 PersistentFileSysObj_StateName(nextState),
		 serialNum,
		 ItemPointerToString(persistentTid));

	return 0;
}

void PersistentFileSysObj_VerifyExpected(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	PersistentFileSysObjName	*fsObjName,

	ItemPointer 			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64					serialNum,

	PersistentFileSysState	expectedState1,

	PersistentFileSysState	expectedState2,

	PersistentFileSysState	nextState,

	PersistentFileSysState	*actualState)
	
{
	PersistentFileSysObjName actualFsObjName;

	TransactionId			actualParentXid;
	int64					actualSerialNum;

	bool expectedStateMatch;

	PersistentFileSysObj_ReadTuple(
							fileSysObjData,
							fileSysObjSharedData,
							persistentTid,
							&actualFsObjName,
							actualState,
							&actualParentXid,
							&actualSerialNum);

	expectedStateMatch = (*actualState == expectedState1);
	if (!expectedStateMatch && ((int)expectedState2 != -1))
		expectedStateMatch = (*actualState == expectedState2);

	if (*actualState == PersistentFileSysState_Free)
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
								 						nextState)));
	}
	
	if (actualSerialNum != serialNum)
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Found different serial number " INT64_FORMAT " than expected (persistent file-system object found is '%s', state '%s')",
				        actualSerialNum,
						PersistentFileSysObjName_TypeAndObjectName(&actualFsObjName),
				        PersistentFileSysObj_StateName(*actualState)),
				 errcontext_persistent_relation_state_change(
								 						fsObjName,
								 						persistentTid,
								 						serialNum,
								 						expectedState1,
								 						expectedState2,
								 						nextState)));
	}

	if (!expectedStateMatch)
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Found persistent file-system object in unexpected state '%s'",
						PersistentFileSysObj_StateName(*actualState)),
				 errcontext_persistent_relation_state_change(
														fsObjName,
														persistentTid,
														serialNum,
														expectedState1,
														expectedState2,
														nextState)));
	}
}

void PersistentFileSysObj_Created(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer				persistentTid,

	int64					persistentSerialNum)
{
	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		PersistentRelation_Created(
							&fsObjName->variant.relFileNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentDatabase_Created(
							&fsObjName->variant.dbDirNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentTablespace_Created(
							fsObjName->variant.tablespace,
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
	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		PersistentRelation_MarkAbortingCreate(
							&fsObjName->variant.relFileNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentDatabase_MarkAbortingCreate(
							&fsObjName->variant.dbDirNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentTablespace_MarkAbortingCreate(
							fsObjName->variant.tablespace,
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
	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		PersistentRelation_MarkDropPending(
							&fsObjName->variant.relFileNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_DatabaseDir:
		PersistentDatabase_MarkDropPending(
							&fsObjName->variant.dbDirNode,
							persistentTid,
							persistentSerialNum);
		break;

	case PersistentFsObjType_TablespaceDir:
		PersistentTablespace_MarkDropPending(
							fsObjName->variant.tablespace,
							persistentTid,
							persistentSerialNum);
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}
}

void PersistentFileSysObj_Drop(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer				persistentTid,

	int64					persistentSerialNum,

	bool					isRedo)
{
	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		{
			RelFileNode *relFileNode = &fsObjName->variant.relFileNode;
			
			MirroredFileSysObj_DropRelFile(
								smgropen(*relFileNode), 
								/* isLocalBuf */ false, 
								isRedo);

			PersistentRelation_Dropped(
								relFileNode,
								persistentTid,
								persistentSerialNum);
		}
		break;

	case PersistentFsObjType_DatabaseDir:
		{
			DbDirNode *dbDirNode = &fsObjName->variant.dbDirNode;
			
			MirroredFileSysObj_DropDbDir(
								dbDirNode, 
								isRedo);
			
			PersistentDatabase_Dropped(
								dbDirNode,
								persistentTid,
								persistentSerialNum);
		}
		break;

	case PersistentFsObjType_TablespaceDir:
		{
			Oid tablespace = fsObjName->variant.tablespace;
			
			MirroredFileSysObj_DropTablespaceDir(
								tablespace, 
								isRedo);
			
			PersistentTablespace_Dropped(
								tablespace,
								persistentTid,
								persistentSerialNum);
		}
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 fsObjName->type);
	}
}

void PersistentFileSysObj_EndXactAction(
	PersistentEndXactRecObjects 	*persistentObjects,
	
	bool							isCommit)
{
	int i;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentFileSysObj_EndXactAction: commit %s, create count %d, drop count %d",
			 (isCommit ? "true" : "false"),
			 persistentObjects->typed.createFsObjsCount,
			 persistentObjects->typed.dropFsObjsCount);

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
					 "PersistentFileSysObj_EndXactAction: [%d] %s, persistent serial num " INT64_FORMAT ", TID %s, Drop",
					 i,
					 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
					 fsObj->persistentSerialNum,
					 ItemPointerToString(&fsObj->persistentTid));

			PersistentFileSysObj_Drop(
									&fsObj->fsObjName,
									&fsObj->persistentTid,
									fsObj->persistentSerialNum,
									/* isRedo */ false);
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
					 "PersistentFileSysObj_EndXactAction: [%d] %s, persistent serial num " INT64_FORMAT ", TID %s, Drop",
					 i,
					 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
					 fsObj->persistentSerialNum,
					 ItemPointerToString(&fsObj->persistentTid));

			PersistentFileSysObj_Drop(
									&fsObj->fsObjName,
									&fsObj->persistentTid,
									fsObj->persistentSerialNum,
									/* isRedo */ false);
		}
	}
}
