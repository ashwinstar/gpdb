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

	int64			*count)
{
	Relation gp_relation_node;
	int sr;

	gp_relation_node = heap_open(GpRelationNodeRelationId, RowExclusiveLock);

	for (sr = 0; sr < info->pgClassStoredRelationsCount; sr++)
	{
		RelFileNode relFileNode;
		ItemPointerData persistentTid;
		int64 persistentSerialNum;

		relFileNode.spcNode = info->pgClassStoredRelations[sr].tablespaceRelFile.tablespace;
		relFileNode.dbNode = info->database;
		relFileNode.relNode = info->pgClassStoredRelations[sr].tablespaceRelFile.relation;

		if (relFileNode.spcNode == GLOBALTABLESPACE_OID &&
			relFileNode.dbNode != TemplateDbOid)
			continue;
		
		PersistentRelation_AddCreated(
									&relFileNode,
									&persistentTid,
									&persistentSerialNum,
									/* flushToXLog */ false);

		/*
		 * Since we are using the direct open, we do not support indicies.
		 */
		// UNDONE: Distinquish pg_class OID from pg_class relfilenode...
		InsertGpRelationNodeTuple(
						gp_relation_node,
						relFileNode.relNode,	// pg_class OID
						relFileNode.spcNode,
						relFileNode.relNode,	// pg_class relfilenode
						&persistentTid,
						persistentSerialNum);

		(*count)++;
	}

	heap_close(gp_relation_node, RowExclusiveLock);
}

Datum
gp_persistent_build_db(PG_FUNCTION_ARGS)
{
	int64 count = 0;

	Relation pg_database;
	HeapTuple tuple;
	Form_pg_database form_pg_database;

	DatabaseInfo *info;

	Oid defaultTablespace;

	int t;
	
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
	
		dbDirNode.tablespace = tablespace;
		dbDirNode.database = MyDatabaseId;
		
		PersistentDatabase_AddCreated(
								&dbDirNode,
								&persistentTid,
								/* flushToXLog */ false);
	}								
	
	PersistentBuild_PopulateGpRelationNode(
										info,
										defaultTablespace,
										&count);
	heap_close(pg_database, AccessShareLock);

	PG_RETURN_INT64(count);
}
