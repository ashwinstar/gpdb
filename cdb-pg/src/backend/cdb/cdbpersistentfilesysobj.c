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
	
		Datum 					*values,

		bool					flushToXLog)
					/* When true, the XLOG record for this change will be flushed to disk. */
{
	PersistentStore_UpdateTuple(
							&fileSysObjData->storeData,
							&fileSysObjSharedData->storeSharedData,
							persistentTid,
							values,
							flushToXLog);
}

void PersistentFileSysObj_ReadTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	ItemPointer					readTid,

	Datum						*values,

	HeapTuple					*tupleCopy)
{
	PersistentStore_ReadTuple(
							&fileSysObjData->storeData,
							&fileSysObjSharedData->storeSharedData,
							readTid,
							values,
							tupleCopy);
}

void PersistentFileSysObj_AddTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	Datum						*values,

	bool						flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	int64					*persistentSerialNum)
{
	PersistentStore_AddTuple(
							&fileSysObjData->storeData,
							&fileSysObjSharedData->storeSharedData,
							values,
							flushToXLog,
							persistentTid,
							persistentSerialNum);
}

void PersistentFileSysObj_FreeTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	Datum 						*freeValues,
	
	bool						flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */
{
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
	PersistentFileSysObjName 	actualFsObjName;
	MirrorFileSysState			actualMirrorState;	// UNDONE
	TransactionId				actualParentXid;
	int64						actualSerialNum;

	bool expectedStateMatch;

	Datum *values;

	HeapTuple tupleCopy;
	
	values = (Datum*)palloc(fileSysObjData->storeData.numAttributes * sizeof(Datum));

	PersistentFileSysObj_ReadTuple(
							fileSysObjData,
							fileSysObjSharedData,
							persistentTid,
							values,
							&tupleCopy);

	GpPersistent_GetCommonValues(
							fileSysObjData->fsObjType,
							values,
							&actualFsObjName,
							actualState,
							&actualMirrorState,
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
	
	heap_freetuple(tupleCopy);

	pfree(values);
}

void PersistentFileSysObj_Created(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer				persistentTid,

	int64					persistentSerialNum)
{
	switch (fsObjName->type)
	{
	case PersistentFsObjType_RelationFile:
		// UNDONE: segmentFileNum
		PersistentRelation_Created(
							&fsObjName->variant.rel.relFileNode,
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
							&fsObjName->variant.tablespaceDirNode,
							persistentTid,
							persistentSerialNum);
		break;

	// UNDONE: Filespace

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
		// UNDONE: segmentFileNum
		PersistentRelation_MarkAbortingCreate(
							&fsObjName->variant.rel.relFileNode,
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
							&fsObjName->variant.tablespaceDirNode,
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
		// UNDONE: segmentFileNum
		PersistentRelation_MarkDropPending(
							&fsObjName->variant.rel.relFileNode,
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
							&fsObjName->variant.tablespaceDirNode,
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
			RelFileNode *relFileNode = &fsObjName->variant.rel.relFileNode;
			// UNDONE: segmentFileNum
			
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
			TablespaceDirNode *tablespaceDirNode = &fsObjName->variant.tablespaceDirNode;
			
			MirroredFileSysObj_DropTablespaceDir(
								tablespaceDirNode,
								isRedo);
			
			PersistentTablespace_Dropped(
								tablespaceDirNode,
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
