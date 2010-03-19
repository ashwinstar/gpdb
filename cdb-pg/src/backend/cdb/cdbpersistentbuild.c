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

static void PersistentBuild_PopulateGpRelationNode(
	DatabaseInfo 	*info,

	Oid				defaultTablespace,

	MirroredObjectExistenceState mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	int64			*count)
{
	Relation gp_relation_node;
	int r;

	// UNDONE: Somehow UPGRADE needs to get mapping of Global tablespace objects
	// UNDONE: to persistent information...

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
			
			// UNDONE: Assume we have a synchronized mirrored segment pair
			// UNDONE: If we have no, mirror we should use ~_MirrorNotConfigured
			PersistentRelation_AddCreated(
										&relFileNode,
										/* segmentFileNum */ 0,
										relStorageMgr,
										mirrorExistenceState,
										relDataSynchronizationState,
										dbInfoRel->relname,
										&persistentTid,
										&persistentSerialNum,
										/* flushToXLog */ false);
		
			/*
			 * Since we are using the direct open, we do not support indicies.
			 */
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
			int p;

			for (p = 0; p < dbInfoRel->physicalSegmentFilesCount; p++)
			{
				int32 segmentFileNum;

				segmentFileNum = dbInfoRel->physicalSegmentFiles[p].segmentFileNum;
				
				// UNDONE: Assume we have a synchronized mirrored segment pair
				// UNDONE: If we have no, mirror we should use ~_MirrorNotConfigured
				PersistentRelation_AddCreated(
											&relFileNode,
											segmentFileNum,
											relStorageMgr,
											mirrorExistenceState,
											relDataSynchronizationState,
											dbInfoRel->relname,
											&persistentTid,
											&persistentSerialNum,
											/* flushToXLog */ false);
				
				/*
				 * Since we are using the direct open, we do not support indicies.
				 */
				InsertGpRelationNodeTuple(
								gp_relation_node,
								dbInfoRel->relationOid, // pg_class OID
								dbInfoRel->relname,
								relFileNode.relNode,	// pg_class relfilenode
								segmentFileNum,
								&persistentTid,
								persistentSerialNum);
			}
		}

		(*count)++;
	}

	heap_close(gp_relation_node, RowExclusiveLock);
}

Datum
gp_persistent_build_db(PG_FUNCTION_ARGS)
{
	bool mirrored = PG_GETARG_BOOL(0);

	MirroredObjectExistenceState mirrorExistenceState;
	MirroredRelDataSynchronizationState relDataSynchronizationState;

	int64 count = 0;

	Relation pg_database;
	HeapTuple tuple;
	Form_pg_database form_pg_database;

	DatabaseInfo *info;

	Oid defaultTablespace;

	int t;

	if (mirrored)
	{
		mirrorExistenceState = MirroredObjectExistenceState_MirrorCreated;
		relDataSynchronizationState = MirroredRelDataSynchronizationState_Synchronized;
	}
	else
	{
		mirrorExistenceState = MirroredObjectExistenceState_NotMirrored;
		relDataSynchronizationState = MirroredRelDataSynchronizationState_None;
	}

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			"gp_persistent_build_db: Using mirror existence state '%s', data synchronization state '%s'",
			MirroredObjectExistenceState_Name(mirrorExistenceState),
			MirroredRelDataSynchronizationState_Name(relDataSynchronizationState));

	pg_database = heap_open(DatabaseRelationId, AccessShareLock);

	/* Fetch a copy of the tuple to scribble on */
	tuple = SearchSysCacheCopy(DATABASEOID,
							   ObjectIdGetDatum(MyDatabaseId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", MyDatabaseId);
	form_pg_database = (Form_pg_database) GETSTRUCT(tuple);
	
	defaultTablespace = form_pg_database->dattablespace;

	info = DatabaseInfo_Collect(MyDatabaseId, defaultTablespace);

	for (t = 0; t < info->tablespacesCount; t++)
	{
		Oid tablespace = info->tablespaces[t];
		DbDirNode dbDirNode;
	
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

	PG_RETURN_INT64(count);
}
