/*-------------------------------------------------------------------------
 *
 * cdbpersistentbuild.c
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
#include "access/persistentfilesysobjname.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistentrelation.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"
#include "storage/shmem.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/heap.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "cdb/cdbdatabaseinfo.h"
#include "utils/syscache.h"

static void PersistentBuild_ScanGpPersistentRelationNodeForGlobal(
	Relation 	gp_relation_node,

	int64		*count)
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
		int64							createMirrorDataLossTrackingSessionNum;
		MirroredObjectExistenceState	mirrorExistenceState;
		MirroredRelDataSynchronizationState mirrorDataSynchronizationState;
		bool							mirrorBufpoolMarkedForScanIncrementalResync;
		int64							mirrorBufpoolResyncChangedPageCount;
		XLogRecPtr						mirrorBufpoolResyncCkptLoc;
		BlockNumber 					mirrorBufpoolResyncCkptBlockNum;
		int64							mirrorAppendOnlyLossEof;
		int64							mirrorAppendOnlyNewEof;
		PersistentFileSysRelBufpoolKind relBufpoolKind;
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
										&createMirrorDataLossTrackingSessionNum,
										&mirrorExistenceState,
										&mirrorDataSynchronizationState,
										&mirrorBufpoolMarkedForScanIncrementalResync,
										&mirrorBufpoolResyncChangedPageCount,
										&mirrorBufpoolResyncCkptLoc,
										&mirrorBufpoolResyncCkptBlockNum,
										&mirrorAppendOnlyLossEof,
										&mirrorAppendOnlyNewEof,
										&relBufpoolKind,
										&parentXid,
										&serialNum,
										&previousFreeTid);

		if (persistentState == PersistentFileSysState_Free)
			continue;

		PersistentFileSysObjName_SetRelationFile(
											&fsObjName,
											&relFileNode,
											segmentFileNum);

		if (relFileNode.spcNode != GLOBALTABLESPACE_OID)
			continue;

		if (relationStorageManager != PersistentFileSysRelStorageMgr_BufferPool)
			elog(ERROR, "Only expecting global tables to be Buffer Pool managed");

		InsertGpRelationNodeTuple(
						gp_relation_node,
						relFileNode.relNode, 	// pg_class OID
						/* relationName */ NULL,	// Optional.
						relFileNode.relNode,	// pg_class relfilenode
						/* segmentFileNum */ 0,
						&persistentTid,
						persistentSerialNum);

		(*count)++;
	}

	PersistentStore_EndScan(&storeScan);
}

static void PersistentBuild_PopulateGpRelationNode(
	DatabaseInfo 	*info,

	Oid				defaultTablespace,

	MirroredObjectExistenceState mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	int64			*count)
{
	Relation gp_relation_node;
	int r;

	gp_relation_node = heap_open(GpRelationNodeRelationId, RowExclusiveLock);

	for (r = 0; r < info->dbInfoRelArrayCount; r++)
	{
		DbInfoRel *dbInfoRel = &info->dbInfoRelArray[r];
		
		RelFileNode relFileNode;

		PersistentFileSysRelStorageMgr relStorageMgr;

		ItemPointerData persistentTid;
		int64 persistentSerialNum;

		if (dbInfoRel->reltablespace == GLOBALTABLESPACE_OID &&
			info->database != TemplateDbOid)
			continue;

		relFileNode.spcNode = dbInfoRel->reltablespace;
		relFileNode.dbNode = 
				(dbInfoRel->reltablespace == GLOBALTABLESPACE_OID ?
														0 : info->database);
		relFileNode.relNode = dbInfoRel->relfilenodeOid;


		relStorageMgr = (
				 (dbInfoRel->relstorage == RELSTORAGE_AOROWS ||
				  dbInfoRel->relstorage == RELSTORAGE_AOCOLS	) ?
								PersistentFileSysRelStorageMgr_AppendOnly :
								PersistentFileSysRelStorageMgr_BufferPool);

		/*
		 * The gp_relation_node mapping table is empty, so use the physical files as
		 * the guide.
		 */
		if (relStorageMgr == PersistentFileSysRelStorageMgr_BufferPool)
		{
			PersistentFileSysRelStorageMgr localRelStorageMgr;
			PersistentFileSysRelBufpoolKind relBufpoolKind;
			
			GpPersistentRelationNode_GetRelationInfo(
												dbInfoRel->relkind,
												dbInfoRel->relstorage,
												dbInfoRel->relam,
												&localRelStorageMgr,
												&relBufpoolKind);
			Assert(localRelStorageMgr == PersistentFileSysRelStorageMgr_BufferPool);

			/*
			 * Heap tables only ever add a single segment_file_num=0 entry to
			 * gp_persistent_relation regardless of how many segment files there
			 * really are.
			 */
			PersistentRelation_AddCreated(
										&relFileNode,
										/* segmentFileNum */ 0,
										relStorageMgr,
										relBufpoolKind,
										mirrorExistenceState,
										relDataSynchronizationState,
										/* mirrorAppendOnlyLossEof */ 0,
										/* mirrorAppendOnlyNewEof */ 0,
										dbInfoRel->relname,
										&persistentTid,
										&persistentSerialNum,
										/* flushToXLog */ false);
		
			InsertGpRelationNodeTuple(
							gp_relation_node,
							dbInfoRel->relationOid,	// pg_class OID
							dbInfoRel->relname,
							relFileNode.relNode,	// pg_class relfilenode
							/* segmentFileNum */ 0,
							&persistentTid,
							persistentSerialNum);
			
		}
		else
		{
			int a;
			int p;

			/*
			 * Append-Only.
			 */
			if (dbInfoRel->physicalSegmentFilesCount == 0 ||
				dbInfoRel->physicalSegmentFiles[0].segmentFileNum != 0)
			{
				elog(ERROR, "Physical segment file #0 missing for relation '%s'",
				     dbInfoRel->relname);
			}

			/*
			 * Merge physical file existence and ao[cs]seg catalog logical EOFs .
			 */
			a = 0;
			for (p = 0; p < dbInfoRel->physicalSegmentFilesCount; p++)
			{
				int physicalSegmentFileNum = dbInfoRel->physicalSegmentFiles[p].segmentFileNum;

				bool	haveCatalogInfo;
				int64	logicalEof;

				/* 
				 * There is mostly a 1:1 matching of physical files and logical
				 * files and we just have to match them up correctly.  However
				 * there are several cases where this can diverge that we have
				 * to be able to handle.
				 *
				 * 1) Segment file 0 always exists as a physical file, but is
				 * only cataloged when it actually contains data - this only
				 * occurs for ao when data is inserted in utility mode.
				 *
				 * 2) Files created in aborted transactions where an initial
				 * frozen tuple never made it to disk may have a physical file
				 * with no logical file.  
				 *     XXX - These are leaked files that should probably be 
				 *     cleaned up at some point.
				 *
				 * 3) It is possible to have files that logically exist with a
				 * logical EOF of 0 but not exist in the filesystem.  
				 *     XXX - how does this happen, is it really safe?
				 */

				logicalEof		= 0;
				haveCatalogInfo = false;

				/* If we exhaust the loop then we are in case 2 */
				while (a < dbInfoRel->appendOnlyCatalogSegmentInfoCount)
				{
					DbInfoAppendOnlyCatalogSegmentInfo *logicalSegInfo = \
						&dbInfoRel->appendOnlyCatalogSegmentInfo[a];

					/* Normal Case: both exist */
					if (logicalSegInfo->segmentFileNum == physicalSegmentFileNum)
					{
						logicalEof		= logicalSegInfo->logicalEof;
						haveCatalogInfo = true;
						a++;
						break;  /* found */
					}
					
					/* case 0 or case 2 */
					else if (logicalSegInfo->segmentFileNum > physicalSegmentFileNum)
					{
						logicalEof		= 0;
						haveCatalogInfo = false;
						break;  /* not found */
					}

					/* case 3 - skip over logical segments w/o physical files */
					else if (logicalSegInfo->logicalEof == 0)
					{
						a++;
						continue;  /* keep looking */
					}

					/* otherwise it is an error */
					else
					{
						elog(ERROR, "logical EOF greater than zero (" INT64_FORMAT ") for segment file #%d in relation '%s' but physical file is missing",
							 logicalSegInfo->logicalEof,
							 logicalSegInfo->segmentFileNum,
							 dbInfoRel->relname);
					}

					/* unreachable */
					Assert(false);
				}

				/* 
				 * case 2) Ignore segment file left over from pre-Release 4.0 aborted
				 * transaction whose initial frozen ao[cs]seg tuple never made it to
				 * disk.  This will be a file that can result in an upgrade complaint...
				 */
				if (physicalSegmentFileNum > 0 && !haveCatalogInfo)
					continue;
				
				PersistentRelation_AddCreated(
											&relFileNode,
											physicalSegmentFileNum,
											relStorageMgr,
											PersistentFileSysRelBufpoolKind_None,
											mirrorExistenceState,
											relDataSynchronizationState,
											/* mirrorAppendOnlyLossEof */ logicalEof,
											/* mirrorAppendOnlyNewEof */ logicalEof,
											dbInfoRel->relname,
											&persistentTid,
											&persistentSerialNum,
											/* flushToXLog */ false);
				
				InsertGpRelationNodeTuple(
								gp_relation_node,
								dbInfoRel->relationOid, // pg_class OID
								dbInfoRel->relname,
								relFileNode.relNode,	// pg_class relfilenode
								physicalSegmentFileNum,
								&persistentTid,
								persistentSerialNum);
			}
		}
		(*count)++;
	}

	if (info->database != TemplateDbOid)
	{
		PersistentBuild_ScanGpPersistentRelationNodeForGlobal(
														gp_relation_node,
														count);
	}

	heap_close(gp_relation_node, RowExclusiveLock);
}

Datum
gp_persistent_build_db(PG_FUNCTION_ARGS)
{
	MirroredObjectExistenceState		 mirrorExistenceState;
	MirroredRelDataSynchronizationState	 relDataSynchronizationState;

	bool				 mirrored = PG_GETARG_BOOL(0);
	int64				 count = 0;
	Relation			 gp_global_sequence;
	Relation			 pg_database;
	HeapTuple			 tuple;
	HeapScanDesc		 scandesc;
	Form_pg_database	 form_pg_database;
	DatabaseInfo		*info;
	Oid					 defaultTablespace;
	int					 t;

	/*
	 * Turn this on so we don't try to fetch persistence information from
	 * gp_releation_node for gp_relation_node and its index until we've done the
	 * assignment with PersistentRelation_AddCreated.
	 */
	gp_before_persistence_work = true;

	if (mirrored)
	{
		mirrorExistenceState = MirroredObjectExistenceState_MirrorCreated;
		relDataSynchronizationState = 
			MirroredRelDataSynchronizationState_DataSynchronized;
	}
	else
	{
		mirrorExistenceState = MirroredObjectExistenceState_NotMirrored;
		relDataSynchronizationState = MirroredRelDataSynchronizationState_None;
	}

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
			 "gp_persistent_build_db: Using mirror existence state '%s', "
			 "data synchronization state '%s'",
			 MirroredObjectExistenceState_Name(mirrorExistenceState),
			 MirroredRelDataSynchronizationState_Name(
				 relDataSynchronizationState));

	/* 
	 * If the gp_global_sequence table hasn't been populated yet then we need 
	 * to populate it before we can procede with building the rest of the 
	 * persistent tables. 
	 */
	gp_global_sequence = heap_open(GpGlobalSequenceRelationId, RowExclusiveLock);
	scandesc = heap_beginscan(gp_global_sequence, SnapshotAny, 0, NULL);
	tuple = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tuple))
	{
		TupleDesc		tupDesc;
		Datum			values[Natts_gp_global_sequence];
		bool			nulls[Natts_gp_global_sequence];

		/* Insert N frozen tuples of value 0 */
		tupDesc = RelationGetDescr(gp_global_sequence);
		MemSet(nulls, false, sizeof(nulls));
		values[Anum_gp_global_sequence_sequence_num-1] = Int64GetDatum(0);
		tuple = heap_form_tuple(tupDesc, values, nulls);

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "failed to build global sequence tuple");
		
		for (t = 0; t < GpGlobalSequence_MaxSequenceTid; t++)
			frozen_heap_insert(gp_global_sequence, tuple);
	}
	heap_endscan(scandesc);
	heap_close(gp_global_sequence, RowExclusiveLock);

	/* Lookup the information for the current database */
	pg_database = heap_open(DatabaseRelationId, AccessShareLock);

	/* Fetch a copy of the tuple to scribble on */
	tuple = SearchSysCacheCopy(DATABASEOID,
							   ObjectIdGetDatum(MyDatabaseId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", MyDatabaseId);
	form_pg_database = (Form_pg_database) GETSTRUCT(tuple);
	
	defaultTablespace = form_pg_database->dattablespace;

	/*
	 * Special call here to scan the persistent meta-data structures so we are open for 
	 * business and then we can add information.
	 */
	PersistentFileSysObj_BuildInitScan();

	info = DatabaseInfo_Collect(
						MyDatabaseId,
						defaultTablespace,
						/* collectGpRelationNodeInfo */ false,
						/* collectAppendOnlyCatalogSegmentInfo */ true);

	for (t = 0; t < info->tablespacesCount; t++)
	{
		Oid				tablespace = info->tablespaces[t];
		DbDirNode		dbDirNode;
		ItemPointerData persistentTid;

		if (tablespace == GLOBALTABLESPACE_OID)
			continue;
	
		dbDirNode.tablespace = tablespace;
		dbDirNode.database = MyDatabaseId;

		PersistentDatabase_AddCreated(
								&dbDirNode,
								mirrorExistenceState,
								&persistentTid,
								/* flushToXLog */ false);
	}								
	
	PersistentBuild_PopulateGpRelationNode(
										info,
										defaultTablespace,
										mirrorExistenceState,
										relDataSynchronizationState,
										&count);

	heap_close(pg_database, AccessShareLock);

	gp_before_persistence_work = false;

	/* 
	 * Since we have written XLOG records with <persistentTid,
	 * persistentSerialNum> of zeroes because of the gp_before_persistence_work
	 * GUC, lets do a checkpoint to force out all buffer pool pages so we never
	 * try to redo those XLOG records in Crash Recovery.
	 */
	CreateCheckPoint(false, true);

	PG_RETURN_INT64(count);
}
