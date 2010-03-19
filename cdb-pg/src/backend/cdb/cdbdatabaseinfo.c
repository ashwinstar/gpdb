/*-------------------------------------------------------------------------
 *
 * cdbdatabaseinfo.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/palloc.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "storage/fd.h"
#include "storage/relfilenode.h"
#include "storage/dbdirnode.h"
#include "cdb/cdbdatabaseinfo.h"
#include "catalog/catalog.h"
#include "access/relscan.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/pg_tablespace.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbdirectopen.h"

// -----------------------------------------------------------------------------
// Open/Close pg_class Relation 
// -----------------------------------------------------------------------------

/*
 * This HUGE MAGIC DEFINE expands into module globals and two routines:
 *     DbCommand_PgClassOpen
 *     DbCommand_PgClassClose
 * It allows for opening the relation without going through pg_class, etc.
 */
static FormData_pg_class 
			DatabaseInfo_PgClassPgClass = 
						{Class_pg_class};

static FormData_pg_attribute 
			DatabaseInfo_PgClassAttrArray[Natts_pg_class] =
							{Schema_pg_class};

static Form_pg_attribute 
			DatabaseInfo_PgClassAttrPtrArray[Natts_pg_class];

DirectOpenDefine(DatabaseInfo_PgClass,\
DatabaseInfo_PgClassPgClass,Natts_pg_class,\
DatabaseInfo_PgClassAttrArray,DatabaseInfo_PgClassAttrPtrArray,\
RelationRelationId,PG_CLASS_RELTYPE_OID,true,true);

static FormData_pg_class 
			DatabaseInfo_GpRelationNodePgClass = 
						{Class_gp_relation_node};

static FormData_pg_attribute 
			DatabaseInfo_GpRelationNodeAttrArray[Natts_gp_relation_node] =
							{Schema_gp_relation_node};

static Form_pg_attribute 
			DatabaseInfo_GpRelationNodeAttrPtrArray[Natts_gp_relation_node];

DirectOpenDefine(DatabaseInfo_GpRelationNode,\
DatabaseInfo_GpRelationNodePgClass,Natts_gp_relation_node,\
DatabaseInfo_GpRelationNodeAttrArray,DatabaseInfo_GpRelationNodeAttrPtrArray,\
GpRelationNodeRelationId,GP_RELATION_NODE_OID,false,true);


static void DatabaseInfo_Grow(
	void 	**array,
	int32	arrayCount,
	int32	*arrayMaxCount,
	int32	elementLen)
{
	void *newArray;
	
	(*arrayMaxCount) *= 2;
	newArray = palloc((*arrayMaxCount)*elementLen);
	memcpy(
		newArray, 
		(*array), 
		arrayCount*elementLen);
	pfree(*array);
	*array = newArray;
}

#ifdef suppress

void DatabaseInfo_Check(DatabaseInfo *info)
{
	int sr;
	int rsf;

	/*
	 * Compare Stored Relations to Relation Segment Files.
	 */
	sr = 0;
	rsf = 0;
	while (true)
	{
		int cmp;

		cmp = TablespaceRelFile_Compare(
						&info->pgClassStoredRelations[sr].tablespaceRelFile,
						&info->relSegFiles[rsf].tablespaceRelFile);
		if (cmp != 0)
		{
			elog(WARNING, "In database %u, stored relation doesn't match relation file on disk "
						"(stored tablespace %u, disk file tablespace %u, "
						"stored relation %u, disk file relation %u)",
						info->database,
						info->pgClassStoredRelations[sr].tablespaceRelFile.tablespace,
						info->relSegFiles[rsf].tablespaceRelFile.tablespace,
						info->pgClassStoredRelations[sr].tablespaceRelFile.relation,
						info->relSegFiles[rsf].tablespaceRelFile.relation);
			return;
		}

		/*
		 * Skip multiple relation segment files.
		 */
		while (true)
		{
			rsf++;
			if (rsf >= info->relSegFilesCount)
				break;
			if (TablespaceRelFile_Compare(
						&info->pgClassStoredRelations[sr].tablespaceRelFile,
						&info->relSegFiles[rsf].tablespaceRelFile) != 0)
				break;
		}

		sr++;

		if (sr >= info->pgClassStoredRelationsCount ||
			rsf >= info->relSegFilesCount)
		{
			if (sr < info->pgClassStoredRelationsCount)
			{
				elog(WARNING, "In database %u, extra stored relation (tablespace %u, relation %u)",
				     info->database,
					 info->pgClassStoredRelations[sr].tablespaceRelFile.tablespace,
					 info->pgClassStoredRelations[sr].tablespaceRelFile.relation);
				return;
			}

			if (rsf < info->relSegFilesCount)
			{
				elog(WARNING, "In database %u, extra relation file on disk (tablespace %u, relation %u)",
				     info->database,
					 info->relSegFiles[rsf].tablespaceRelFile.tablespace,
					 info->relSegFiles[rsf].tablespaceRelFile.relation);
				return;
			}
			break;
		}
	}
}

void DatabaseInfo_Trace(DatabaseInfo *info)
{
	int t;
	int sr;
	int rsf;
	int grn;
	int m;

	for (t = 0; t < info->tablespacesCount; t++)
		elog(WARNING, "Database Info: Tablespace #%d is %u",
			 t, info->tablespaces[t]);

	for (sr = 0; sr < info->pgClassStoredRelationsCount; sr++)
		elog(WARNING, "Database Info: Stored relation (tablespace %u, relation %u, isBufferPoolRealtion %s, TID %s)",
			 info->pgClassStoredRelations[sr].tablespaceRelFile.tablespace, 
			 info->pgClassStoredRelations[sr].tablespaceRelFile.relation,
			 (info->pgClassStoredRelations[sr].isBufferPoolRelation ? "true" : "false"),
			 ItemPointerToString(&info->pgClassStoredRelations[sr].pgClassTid));

	for (rsf = 0; rsf < info->relSegFilesCount; rsf++)
		elog(WARNING, "Database Info: Relation segment file (tablespace %u, relation %u, segment file num %d)",
			 info->relSegFiles[rsf].tablespaceRelFile.tablespace, 
			 info->relSegFiles[rsf].tablespaceRelFile.relation, 
			 info->relSegFiles[rsf].segmentFileNum);

	for (grn = 0; grn < info->gpRelationNodesCount; grn++)
		elog(WARNING, "Database Info: Tablespace %u, relation %u node information (persistent TID %s, perstent serial number " INT64_FORMAT ")",
			 info->gpRelationNodes[grn].tablespaceRelFile.tablespace, 
			 info->gpRelationNodes[grn].tablespaceRelFile.relation, 
			 ItemPointerToString(&info->gpRelationNodes[grn].persistentTid),
			 info->gpRelationNodes[grn].persistentSerialNum);

	for (m = 0; m < info->miscEntriesCount; m++)
		elog(WARNING, "Database Info: Misc entry #%d (tablespace %u, directory = %s, name '%s')",
			 m, 
			 info->miscEntries[m].tablespace,
			 (info->miscEntries[m].isDir ? "true" : "false"),
			 info->miscEntries[m].name);
}

#endif

static void DatabaseInfo_AddTablespace(
	DatabaseInfo 		*info,
	Oid 				tablespace)
{
	int t;

	t = 0;
	while (true)
	{
		if (t >= info->tablespacesCount)
		{
			Assert(t == info->tablespacesCount);
			if (t >= info->tablespacesMaxCount)
			{
				DatabaseInfo_Grow(
								(void**)&info->tablespaces,
								info->tablespacesCount,
								&info->tablespacesMaxCount,
								sizeof(Oid));
			}
			info->tablespaces[info->tablespacesCount++] = tablespace;
			break;
		}
		
		if (info->tablespaces[t] == tablespace)
			break;
	
		t++;
	}
}

static void DatabaseInfo_AddExtraSegmentFile(
	DatabaseInfo 		*info,
	Oid 				tablespace,
	Oid					relfilenode,
	int32				segmentFileNum,
	int64				eof)
{
	DbInfoExtraSegmentFile	*dbInfoExtraSegmentFile;

	if (info->extraSegmentFilesCount>= info->extraSegmentFilesMaxCount)
	{
		DatabaseInfo_Grow(
						(void**)&info->extraSegmentFiles,
						info->extraSegmentFilesCount,
						&info->extraSegmentFilesMaxCount,
						sizeof(DbInfoExtraSegmentFile));
	}

	dbInfoExtraSegmentFile = 
					&info->extraSegmentFiles[info->extraSegmentFilesCount];
	info->extraSegmentFilesCount++;
	
	dbInfoExtraSegmentFile->relfilenode = relfilenode;
	dbInfoExtraSegmentFile->segmentFileNum = segmentFileNum;
	dbInfoExtraSegmentFile->tablespaceOid = tablespace;
	dbInfoExtraSegmentFile->eof = eof;
}


static void DatabaseInfo_AddPgClassStoredRelation(
	DatabaseInfo 		*info,
	HTAB 				*dbInfoRelHashTable,
	Oid 				relfilenode,
	ItemPointer			pgClassTid,
	Oid					relationOid,
	char				*relname,
	Oid					reltablespace,
	char				relkind,
	char				relstorage)
{
	DbInfoRel *dbInfoRel;
	bool found;

	dbInfoRel = 
			(DbInfoRel*) 
					hash_search(dbInfoRelHashTable,
								(void *) &relfilenode,
								HASH_ENTER,
								&found);
	if (found)
		elog(ERROR, "More than one pg_class entry ('%s' %u and '%s' %u) references the same relfilenode %u",
			 dbInfoRel->relname,
			 dbInfoRel->relationOid,
			 relname,
			 relationOid,
			 relfilenode);

	dbInfoRel->inPgClass = true;
	dbInfoRel->pgClassTid = *pgClassTid;
	dbInfoRel->relationOid = relationOid;
	dbInfoRel->relname = pstrdup(relname);
	dbInfoRel->reltablespace = reltablespace;
	dbInfoRel->relkind = relkind;
	dbInfoRel->relstorage = relstorage;

	dbInfoRel->gpRelationNodesMaxCount = 1;
	dbInfoRel->gpRelationNodes = 
			palloc0(dbInfoRel->gpRelationNodesMaxCount * sizeof(DbInfoGpRelationNode));
	dbInfoRel->gpRelationNodesCount = 0;
	
	dbInfoRel->physicalSegmentFilesMaxCount = 1;
	dbInfoRel->physicalSegmentFiles = 
			palloc0(dbInfoRel->physicalSegmentFilesMaxCount * sizeof(DbInfoSegmentFile));
	dbInfoRel->physicalSegmentFilesCount = 0;
	
}

static bool DatabaseInfo_AddGpRelationNode(
	DatabaseInfo		*info,
	HTAB 				*dbInfoRelHashTable,
	Oid					relfilenode,
	int32				segmentFileNum,
	ItemPointer			persistentTid,
	int64				persistentSerialNum,
	ItemPointer			gpRelationNodeTid)
{
	DbInfoRel *dbInfoRel;
	bool found;

	DbInfoGpRelationNode *dbInfoGpRelationNode;

	dbInfoRel = 
			(DbInfoRel*) 
					hash_search(dbInfoRelHashTable,
								(void *) &relfilenode,
								HASH_FIND,
								&found);
	if (found)
	{
		if (dbInfoRel->gpRelationNodesCount >= dbInfoRel->gpRelationNodesMaxCount)
		{
			DatabaseInfo_Grow(
							(void**)&dbInfoRel->gpRelationNodes,
							dbInfoRel->gpRelationNodesCount,
							&dbInfoRel->gpRelationNodesMaxCount,
							sizeof(DbInfoGpRelationNode));
		}

		dbInfoGpRelationNode = 
						&dbInfoRel->gpRelationNodes[dbInfoRel->gpRelationNodesCount];
		dbInfoRel->gpRelationNodesCount++;
	}
	else
	{
		if (info->parentlessGpRelationNodesCount >= dbInfoRel->physicalSegmentFilesMaxCount)
		{
			DatabaseInfo_Grow(
							(void**)&info->parentlessGpRelationNodes,
							info->parentlessGpRelationNodesCount,
							&info->parentlessGpRelationNodesMaxCount,
							sizeof(DbInfoGpRelationNode));
		}

		dbInfoGpRelationNode = 
						&info->parentlessGpRelationNodes[info->parentlessGpRelationNodesCount];
		info->parentlessGpRelationNodesCount++;
	}
	
	dbInfoGpRelationNode->gpRelationNodeTid = *gpRelationNodeTid;
	dbInfoGpRelationNode->relfilenodeOid = relfilenode;
	dbInfoGpRelationNode->segmentFileNum = segmentFileNum;
	dbInfoGpRelationNode->persistentTid = *persistentTid;
	dbInfoGpRelationNode->persistentSerialNum = persistentSerialNum;

	return found;
}

static void DatabaseInfo_AddMiscEntry(
	DatabaseInfo		 	*info,
	Oid 					tablespace,
	bool					isDir,
	char					*name)
{
	MiscEntry 	*miscEntry;

	if (info->miscEntriesCount >= info->miscEntriesMaxCount)
	{
		DatabaseInfo_Grow(
						(void**)&info->miscEntries,
						info->miscEntriesCount,
						&info->miscEntriesMaxCount,
						sizeof(MiscEntry));
	}

	miscEntry = &info->miscEntries[info->miscEntriesCount];
	info->miscEntriesCount++;
	
	miscEntry->tablespace = tablespace;
	miscEntry->isDir = isDir;
	miscEntry->name = pstrdup(name);

}

static void DatabaseInfo_AddPhysicalSegmentFile(
	DbInfoRel 				*dbInfoRel,
	int32 					segmentFileNum,
	int64					eof)
{
	DbInfoSegmentFile 	*dbInfoSegmentFile;

	if (dbInfoRel->physicalSegmentFilesCount>= dbInfoRel->physicalSegmentFilesMaxCount)
	{
		DatabaseInfo_Grow(
						(void**)&dbInfoRel->physicalSegmentFiles,
						dbInfoRel->physicalSegmentFilesCount,
						&dbInfoRel->physicalSegmentFilesMaxCount,
						sizeof(DbInfoSegmentFile));
	}

	dbInfoSegmentFile = &dbInfoRel->physicalSegmentFiles[dbInfoRel->physicalSegmentFilesCount];
	dbInfoRel->physicalSegmentFilesCount++;
	
	dbInfoSegmentFile->segmentFileNum = segmentFileNum;
	dbInfoSegmentFile->eof = eof;

}

static void DatabaseInfo_AddRelSegFile(
	DatabaseInfo 			*info,
	HTAB 					*dbInfoRelHashTable,
	Oid 					tablespace,
	Oid						relfilenode,
	int32					segmentFileNum,
	int64					eof)
{
	DbInfoRel *dbInfoRel;
	bool found;

	PersistentFileSysRelStorageMgr relStorageMgr;

	dbInfoRel = 
			(DbInfoRel*) 
					hash_search(dbInfoRelHashTable,
								(void *) &relfilenode,
								HASH_FIND,
								&found);
	if (!found || dbInfoRel->reltablespace != tablespace)
	{
		DatabaseInfo_AddExtraSegmentFile(
									info,
									tablespace,
									relfilenode,
									segmentFileNum,
									eof);
		return;
	}

	relStorageMgr = (
			 (dbInfoRel->relstorage == RELSTORAGE_AOROWS ||
			  dbInfoRel->relstorage == RELSTORAGE_AOCOLS	) ?
							PersistentFileSysRelStorageMgr_AppendOnly :
							PersistentFileSysRelStorageMgr_BufferPool);

	if (relStorageMgr == PersistentFileSysRelStorageMgr_AppendOnly)
	{
		int nextIndex = dbInfoRel->physicalSegmentFilesCount;

		if (nextIndex >= dbInfoRel->gpRelationNodesCount ||
			dbInfoRel->gpRelationNodes[nextIndex].segmentFileNum != segmentFileNum)
		{
			DatabaseInfo_AddExtraSegmentFile(
										info,
										tablespace,
										relfilenode,
										segmentFileNum,
										eof);
			return;
		}
		
	}

	DatabaseInfo_AddPhysicalSegmentFile(
										dbInfoRel,
										segmentFileNum,
										eof);
}

static void DatabaseInfo_AddFile(
	DatabaseInfo		 	*info,
	HTAB 					*dbInfoRelHashTable,
	Oid 					tablespace,
	char					*dbDirPath,
	char					*name)
{
	int64 eof;
	int itemCount;
	Oid relfilenode;
	uint32 segmentFileNum;
	char path[MAXPGPATH];
	int	fileFlags = O_RDONLY | PG_BINARY;
	int	fileMode = 0400;
						/* File mode is S_IRUSR 00400 user has read permission */
	File file;

	sprintf(path, "%s/%s", dbDirPath, name);
	
	/*
	 * Open the file for read.
	 */	
	file = PathNameOpenFile(path, fileFlags, fileMode);
	if(file < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Could not open segment file '%s'", 
						path)));
	}
	eof = FileSeek(file, 0L, SEEK_END);
	if (eof < 0) {
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Could not seek to end of file \"%s\" : %m",
						path)));
	}
	FileClose(file);

	itemCount = sscanf(name, "%u.%u", &relfilenode, &segmentFileNum);

	// UNDONE: sscanf is a rather poor scanner.
	// UNDONE: For right now, just assume properly named files....
	if (itemCount == 0)
	{
		DatabaseInfo_AddMiscEntry(info, tablespace, false, name);
		return;
	}
	else if (itemCount == 1)
		segmentFileNum = 0;
	else
		Assert(itemCount == 2);

	DatabaseInfo_AddRelSegFile(info, dbInfoRelHashTable, tablespace, relfilenode, segmentFileNum, eof);
}

static void DatabaseInfo_Scan(
	DatabaseInfo 			*info,
	HTAB 					*dbInfoRelHashTable,
	Oid 					tablespace,
	Oid 					database)
{
	char		*dbDirPath;
	DIR			*xldir;
	struct dirent *xlde;
	char		fromfile[MAXPGPATH];

	dbDirPath = GetDatabasePath(
						(tablespace == GLOBALTABLESPACE_OID ? 0 : database), 
						tablespace);
	
	xldir = AllocateDir(dbDirPath);
	if (xldir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Could not open database directory \"%s\": %m", dbDirPath)));

	while ((xlde = ReadDir(xldir, dbDirPath)) != NULL)
	{
		struct stat fst;

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fromfile, MAXPGPATH, "%s/%s", dbDirPath, xlde->d_name);

		if (lstat(fromfile, &fst) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", fromfile)));

		if (S_ISDIR(fst.st_mode))
		{
			DatabaseInfo_AddMiscEntry(
									info, 
									tablespace,
									/* isDir */ true, 
									xlde->d_name);
		}
		else if (S_ISREG(fst.st_mode))
		{
			DatabaseInfo_AddFile(
								info,
								dbInfoRelHashTable,
								tablespace,
								dbDirPath,
								xlde->d_name);
		}
	}

	FreeDir(xldir);
}

static void
DatabaseInfo_HashTableInit(
	HTAB 	**dbInfoRelHashTable)
{
	HASHCTL			info;
	int				hash_flags;

	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	info.entrysize = sizeof(DbInfoRel);
	info.hash = tag_hash;

	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	*dbInfoRelHashTable = hash_create("DbInfoRel", 100, &info, hash_flags);
}

/*
 * A compare function for 2 RelationSegmentFile.
 */
static int
DbInfoRelPtrArray_Compare(const void *entry1, const void *entry2)
{
	const DbInfoRel *dbInfoRel1 = *((DbInfoRel**)entry1);
	const DbInfoRel *dbInfoRel2 = *((DbInfoRel**)entry2);

	if (dbInfoRel1->relfilenodeOid == dbInfoRel2->relfilenodeOid)
		return 0;
	else if (dbInfoRel1->relfilenodeOid > dbInfoRel2->relfilenodeOid)
		return 1;
	else
		return -1;
}


DatabaseInfo *DatabaseInfo_Collect(
	Oid			database,
	Oid 		defaultTablespace)
{
	DatabaseInfo *info;
	
	HTAB 	*dbInfoRelHashTable;

	HeapScanDesc scan;
	Relation	pg_class_rel;
	Relation	gp_relation_node_rel;
	HeapTuple	tuple;
	int			count;

	int 		t;

	DbInfoRel	**dbInfoRelPtrArray;

	HASH_SEQ_STATUS iterateStatus;

	int d;

	info = (DatabaseInfo*)palloc0(sizeof(DatabaseInfo));

	info->database = database;

	info->tablespacesMaxCount = 10;
	info->tablespaces = 
			palloc0(info->tablespacesMaxCount*sizeof(Oid));

	info->miscEntriesMaxCount = 50;
	info->miscEntries = 
			palloc0(info->miscEntriesMaxCount*sizeof(MiscEntry));

	info->extraSegmentFilesMaxCount = 10;
	info->extraSegmentFiles = 
			palloc0(info->extraSegmentFilesMaxCount*sizeof(DbInfoExtraSegmentFile));

	info->parentlessGpRelationNodesMaxCount = 10;
	info->parentlessGpRelationNodes = 
			palloc0(info->parentlessGpRelationNodesMaxCount*sizeof(DbInfoGpRelationNode));

	DatabaseInfo_HashTableInit(&dbInfoRelHashTable);

	/*
 	 * Iterate through all the relations of the database and determine which
 	 * database directories are active.  I.e. Fill up the tablespaces array.
	 */
	count = 0;
	pg_class_rel = DatabaseInfo_PgClassOpen(defaultTablespace, database);
	scan = heap_beginscan(pg_class_rel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid				relationOid;

		Form_pg_class	form_pg_class;

		Oid				reltablespace;

		char			relkind;
		char			relstorage;

		relationOid = HeapTupleGetOid(tuple);

		form_pg_class = (Form_pg_class) GETSTRUCT(tuple);

		reltablespace = form_pg_class->reltablespace;
	
		if (reltablespace == 0)
			reltablespace = defaultTablespace;

		/*
		 *  Skip non-storage relations.
		 */
		relkind = form_pg_class->relkind;

		if (relkind == RELKIND_VIEW ||
			relkind == RELKIND_COMPOSITE_TYPE)
			continue;

		relstorage = form_pg_class->relstorage;

		if (relstorage == RELSTORAGE_EXTERNAL)
			continue;

		DatabaseInfo_AddTablespace(
									info, 
									reltablespace);

		DatabaseInfo_AddPgClassStoredRelation(
											info,
											dbInfoRelHashTable,
											form_pg_class->relfilenode,
											&tuple->t_self,
											relationOid,
											form_pg_class->relname.data,
											reltablespace,
											relkind,
											relstorage);

		count++;
	}
	heap_endscan(scan);
	DatabaseInfo_PgClassClose(pg_class_rel);

	gp_relation_node_rel = DatabaseInfo_GpRelationNodeOpen(defaultTablespace, database);
	scan = heap_beginscan(gp_relation_node_rel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool			nulls[Natts_gp_relation_node];
		Datum			values[Natts_gp_relation_node];
		
		Oid					relfilenode;
		int32				segmentFileNum;
		ItemPointerData		persistentTid;
		int64				persistentSerialNum;
		
		heap_deform_tuple(tuple, RelationGetDescr(gp_relation_node_rel), values, nulls);
				
		Assert(!nulls[Anum_gp_relation_node_relfilenode_oid - 1]);
		relfilenode = DatumGetObjectId(values[Anum_gp_relation_node_relfilenode_oid - 1]);
		
		Assert(!nulls[Anum_gp_relation_node_segment_file_num - 1]);
		segmentFileNum = DatumGetInt32(values[Anum_gp_relation_node_segment_file_num - 1]);
		
		Assert(!nulls[Anum_gp_relation_node_persistent_tid - 1]);
		persistentTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_relation_node_persistent_tid - 1]));
			
		Assert(!nulls[Anum_gp_relation_node_persistent_serial_num - 1]);
		persistentSerialNum = DatumGetInt64(values[Anum_gp_relation_node_persistent_serial_num - 1]);
		
		if (!DatabaseInfo_AddGpRelationNode(
									info,
									dbInfoRelHashTable,
									relfilenode,
									segmentFileNum,
									&persistentTid,
									persistentSerialNum,
									&tuple->t_self))
		{
			elog(WARNING, "Did not find matching pg_class entry for gp_relation_node entry relfilenode %u (parentless!!!)",
				 relfilenode);
		}
	}
	heap_endscan(scan);
	DatabaseInfo_GpRelationNodeClose(gp_relation_node_rel);

	/*
	 * Scan each used directory for its relation segment files and misc files/dirs.
	 */
	for (t = 0; t < info->tablespacesCount; t++)
	{
		DatabaseInfo_Scan(info, dbInfoRelHashTable, info->tablespaces[t], database);
	}

	dbInfoRelPtrArray = (DbInfoRel**)palloc0(sizeof(DbInfoRel**) * count);

	hash_seq_init(&iterateStatus, dbInfoRelHashTable);

	d = 0;
	while (true)
	{
		DbInfoRel *dbInfoRel;

		dbInfoRel = 
				(DbInfoRel*)
						hash_seq_search(&iterateStatus);
		if (dbInfoRel == NULL)
			break;

		Assert(d < count);

		dbInfoRelPtrArray[d] = dbInfoRel;
		d++;
	}

	Assert(d == count);

	/*
	 * Sort the pointers to DbRelInfo structs in the hash table.
	 */
	qsort(
		dbInfoRelPtrArray,
		count, 
		sizeof(DbInfoRel**),
		DbInfoRelPtrArray_Compare);

	info->dbInfoRelArray = (DbInfoRel*)palloc0(sizeof(DbInfoRel) * count);

	for (d = 0; d < count; d++)
		info->dbInfoRelArray[d] = *(dbInfoRelPtrArray[d]);

	info->dbInfoRelArrayCount = count;
	
	return info;

}

