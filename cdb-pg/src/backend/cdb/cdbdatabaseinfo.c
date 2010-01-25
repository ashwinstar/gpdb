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
RelationRelationId,RelationRelType,true);

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
GpRelationNodeRelationId,GpRelationNodeRelType,false);


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

static void DatabaseInfo_AddTablespace(
	DatabaseInfo 	*info,
	Oid 					tablespace)
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

static void DatabaseInfo_AddPgClassStoredRelation(
	DatabaseInfo 		*info,
	Oid 				tablespace,
	Oid					relation,
	bool				isBufferPoolRelation,
	ItemPointer			pgClassTid)
{
	int32 count = info->pgClassStoredRelationsCount;

	if (count >= info->pgClassStoredRelationsMaxCount)
	{
		DatabaseInfo_Grow(
						(void**)&info->pgClassStoredRelations,
						count,
						&info->pgClassStoredRelationsMaxCount,
						sizeof(PgClassStoredRelation));
	}

	info->pgClassStoredRelations[count].tablespaceRelFile.tablespace = tablespace;
	info->pgClassStoredRelations[count].tablespaceRelFile.relation = relation;
	info->pgClassStoredRelations[count].isBufferPoolRelation = isBufferPoolRelation;
	info->pgClassStoredRelations[count].pgClassTid = *pgClassTid;

	info->pgClassStoredRelationsCount++;
}

static void DatabaseInfo_AddGpRelationNode(
	DatabaseInfo 		*info,
	Oid					tablespace,
	Oid					relationNode,
	int32				segmentFileNum,
	ItemPointer			persistentTid,
	int64				persistentSerialNum,
	ItemPointer			gpRelationNodeTid)
{
	int32 count = info->gpRelationNodesCount;

	if (count >= info->gpRelationNodesMaxCount)
	{
		DatabaseInfo_Grow(
						(void**)&info->gpRelationNodes,
						count,
						&info->gpRelationNodesMaxCount,
						sizeof(GpRelationNode));
	}

	info->gpRelationNodes[count].tablespaceRelFile.tablespace = tablespace;
	info->gpRelationNodes[count].tablespaceRelFile.relation = relationNode;
	// UNDONE: segmentFileNum
	info->gpRelationNodes[count].persistentTid = *persistentTid;
	info->gpRelationNodes[count].persistentSerialNum = persistentSerialNum;
	info->gpRelationNodes[count].gpRelationNodeTid = *gpRelationNodeTid;

	info->gpRelationNodesCount++;
}

static void DatabaseInfo_AddMiscEntry(
	DatabaseInfo 	*info,
	Oid 					tablespace,
	bool					isDir,
	char					*name)
{
	if (info->miscEntriesCount >= info->miscEntriesMaxCount)
	{
		DatabaseInfo_Grow(
						(void**)&info->miscEntries,
						info->miscEntriesCount,
						&info->miscEntriesMaxCount,
						sizeof(MiscEntry));
	}

	info->miscEntries[info->miscEntriesCount].tablespace = tablespace;
	info->miscEntries[info->miscEntriesCount].isDir = isDir;
	info->miscEntries[info->miscEntriesCount].name = pstrdup(name);

	info->miscEntriesCount++;
}


static void DatabaseInfo_AddRelSegFile(
	DatabaseInfo 	*info,
	Oid 					tablespace,
	Oid						relation,
	int32					segmentFileNum)
{
	int32 count = info->relSegFilesCount;

	if (count >= info->relSegFilesMaxCount)
	{
		DatabaseInfo_Grow(
						(void**)&info->relSegFiles,
						count,
						&info->relSegFilesMaxCount,
						sizeof(RelationSegmentFile));
	}

	info->relSegFiles[count].tablespaceRelFile.tablespace = tablespace;
	info->relSegFiles[count].tablespaceRelFile.relation = relation;
	info->relSegFiles[count].segmentFileNum = segmentFileNum;
	info->relSegFilesCount++;
}

static void DatabaseInfo_AddFile(
	DatabaseInfo 	*info,
	Oid 					tablespace,
	char					*name)
{
	int itemCount;
	Oid relation;
	uint32 segmentFileNum;

	itemCount = sscanf(name, "%u.%u", &relation, &segmentFileNum);

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

	DatabaseInfo_AddRelSegFile(info, tablespace, relation, segmentFileNum);
}

static void DatabaseInfo_Scan(
	DatabaseInfo 	*info,
	Oid 					tablespace,
	Oid 					database)
{
	char		*dbDirPath;
	DIR			*xldir;
	struct dirent *xlde;
	char		fromfile[MAXPGPATH];

	dbDirPath = GetDatabasePath(database, tablespace);
	
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
								tablespace,
								xlde->d_name);
		}
	}

	FreeDir(xldir);
}

/*
 * A compare function for 2 TablespaceRelFile.
 */
int
TablespaceRelFile_Compare(const void *entry1, const void *entry2)
{
	const TablespaceRelFile *tablespaceRelFile1 = (TablespaceRelFile*)entry1;
	const TablespaceRelFile *tablespaceRelFile2 = (TablespaceRelFile*)entry2;
	const Oid *oidArray1;
	const Oid *oidArray2;
	int i;

	oidArray1 = &tablespaceRelFile1->tablespace;
	oidArray2 = &tablespaceRelFile2->tablespace;
	for (i = 0; i < 2; i++)
	{
		if (oidArray1[i] != oidArray2[i])
		{
			if (oidArray1[i] > oidArray2[i])
				return 1;
			else
				return -1;
		}
	}
	return 0;
}

/*
 * A compare function for 2 PgClassStoredRelation.
 */
static int
PgClassStoredRelation_Compare(const void *entry1, const void *entry2)
{
	const PgClassStoredRelation *storedRelation1 = (PgClassStoredRelation*)entry1;
	const PgClassStoredRelation *storedRelation2 = (PgClassStoredRelation*)entry2;

	return TablespaceRelFile_Compare(
					&storedRelation1->tablespaceRelFile,
					&storedRelation2->tablespaceRelFile);
}

/*
 * A compare function for 2 GpRelationNode.
 */
static int
GpRelationNode_Compare(const void *entry1, const void *entry2)
{
	const GpRelationNode *gpRelationNode1 = (GpRelationNode*)entry1;
	const GpRelationNode *gpRelationNode2 = (GpRelationNode*)entry2;

	return TablespaceRelFile_Compare(
					&gpRelationNode1->tablespaceRelFile,
					&gpRelationNode2->tablespaceRelFile);
}


/*
 * A compare function for 2 RelationSegmentFile.
 */
static int
RelationSegmentFile_Compare(const void *entry1, const void *entry2)
{
	const RelationSegmentFile *relSegFile1 = (RelationSegmentFile*)entry1;
	const RelationSegmentFile *relSegFile2 = (RelationSegmentFile*)entry2;

	int cmp;
	
	cmp = TablespaceRelFile_Compare(
					&relSegFile1->tablespaceRelFile,
					&relSegFile2->tablespaceRelFile);
	if (cmp != 0)
		return cmp;

	if (relSegFile1->segmentFileNum == relSegFile2->segmentFileNum)
		return 0;
	else if (relSegFile1->segmentFileNum > relSegFile2->segmentFileNum)
		return 1;
	else
		return -1;
}

DatabaseInfo *DatabaseInfo_Collect(
	Oid			database,
	Oid 		defaultTablespace)
{
	DatabaseInfo *info;

	HeapScanDesc scan;
	Relation	pg_class_rel;
	Relation	gp_relation_node_rel;
	HeapTuple	tuple;

	int 		t;

	info = (DatabaseInfo*)palloc0(sizeof(DatabaseInfo));

	info->database = database;

	info->tablespacesMaxCount = 10;
	info->tablespaces = 
			palloc(info->tablespacesMaxCount*sizeof(Oid));

	info->pgClassStoredRelationsMaxCount= 500;
	info->pgClassStoredRelations = 
			palloc(info->pgClassStoredRelationsMaxCount*sizeof(PgClassStoredRelation));

	info->gpRelationNodesMaxCount = 500;
	info->gpRelationNodes = 
			palloc(info->gpRelationNodesMaxCount*sizeof(GpRelationNode));

	info->relSegFilesMaxCount = 500;
	info->relSegFiles = 
			palloc(info->relSegFilesMaxCount*sizeof(RelationSegmentFile));

	info->miscEntriesMaxCount = 50;
	info->miscEntries = 
			palloc(info->miscEntriesMaxCount*sizeof(MiscEntry));

	/*
 	 * Iterate through all the relations of the database and determine which
 	 * database directories are active.  I.e. Fill up the tablespaces array.
	 */
	pg_class_rel = DatabaseInfo_PgClassOpen(defaultTablespace, database);
	scan = heap_beginscan(pg_class_rel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class			form_pg_class;

		Oid				tablespace;

		char			relkind;
		char			relstorage;

		bool			isBufferPoolRelation;
		
		form_pg_class = (Form_pg_class) GETSTRUCT(tuple);

		tablespace = form_pg_class->reltablespace;
	
		/* No need to examine global tablespace */
		if (tablespace == GLOBALTABLESPACE_OID)
			continue;

		if (tablespace == 0)
			tablespace = defaultTablespace;

		/*
		 *  Skip non-storage relations.
		 */
		relkind = form_pg_class->relkind;

		if (relkind == RELKIND_VIEW ||
			relkind == RELKIND_COMPOSITE_TYPE)
			continue;

		if (form_pg_class->relstorage == RELSTORAGE_EXTERNAL)
			continue;

		DatabaseInfo_AddTablespace(
								info, 
								tablespace);

		relstorage = form_pg_class->relstorage;

		isBufferPoolRelation = (relstorage != RELSTORAGE_AOROWS &&
								relstorage != RELSTORAGE_AOCOLS);

		DatabaseInfo_AddPgClassStoredRelation(
											info,
											tablespace,
											form_pg_class->relfilenode,
											isBufferPoolRelation,
											&tuple->t_self);
	}
	heap_endscan(scan);
	DatabaseInfo_PgClassClose(pg_class_rel);

	gp_relation_node_rel = DatabaseInfo_GpRelationNodeOpen(defaultTablespace, database);
	scan = heap_beginscan(gp_relation_node_rel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool			nulls[Natts_gp_relation_node];
		Datum			values[Natts_gp_relation_node];
		
		Oid					tablespace;
		Oid					relationNode;
		int32				segmentFileNum;
		ItemPointerData		persistentTid;
		int64				persistentSerialNum;
		
		heap_deform_tuple(tuple, RelationGetDescr(gp_relation_node_rel), values, nulls);
		
		Assert(!nulls[Anum_gp_relation_node_tablespace_oid - 1]);
		tablespace = DatumGetObjectId(values[Anum_gp_relation_node_tablespace_oid - 1]);
		if (tablespace == GLOBALTABLESPACE_OID)
			continue;
		
		Assert(!nulls[Anum_gp_relation_node_relfilenode_oid - 1]);
		relationNode = DatumGetObjectId(values[Anum_gp_relation_node_relfilenode_oid - 1]);
		
		Assert(!nulls[Anum_gp_relation_node_segment_file_num - 1]);
		segmentFileNum = DatumGetInt32(values[Anum_gp_relation_node_segment_file_num - 1]);
		
		Assert(!nulls[Anum_gp_relation_node_persistent_tid - 1]);
		persistentTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_relation_node_persistent_tid - 1]));
			
		Assert(!nulls[Anum_gp_relation_node_persistent_serial_num - 1]);
		persistentSerialNum = DatumGetInt64(values[Anum_gp_relation_node_persistent_serial_num - 1]);
		
		DatabaseInfo_AddGpRelationNode(
									info,
									tablespace,
									relationNode,
									segmentFileNum,
									&persistentTid,
									persistentSerialNum,
									&tuple->t_self);
	}
	heap_endscan(scan);
	DatabaseInfo_GpRelationNodeClose(gp_relation_node_rel);

	// UNDONE: Verify used tablespaces are a subset of the persistent database directories.

	/*
	 * Scan each used directory for its relation segment files and misc files/dirs.
	 */
	for (t = 0; t < info->tablespacesCount; t++)
	{
		DatabaseInfo_Scan(info, info->tablespaces[t], database);
	}

	/*
	 * Sort the relation segment files.
	 */
	qsort(
		info->pgClassStoredRelations,
		info->pgClassStoredRelationsCount, 
		sizeof(PgClassStoredRelation),
		PgClassStoredRelation_Compare);

	/*
	 * Sort the gp_relation_node entries.
	 */
	qsort(
		info->gpRelationNodes,
		info->gpRelationNodesCount, 
		sizeof(GpRelationNode),
		GpRelationNode_Compare);

	/*
	 * Sort the relation segment files.
	 */
	qsort(
		info->relSegFiles,
		info->relSegFilesCount, 
		sizeof(RelationSegmentFile),
		RelationSegmentFile_Compare);

	return info;

}

void DatabaseInfo_Check(DatabaseInfo *info)
{
	int sr;
	int rsf;
	int t;

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
			elog(ERROR, "In database %u, stored relation doesn't match relation file on disk "
						"(stored tablespace %u, disk file tablespace %u, "
						"stored relation %u, disk file relation %u)",
						info->database,
						info->pgClassStoredRelations[sr].tablespaceRelFile.tablespace,
						info->relSegFiles[rsf].tablespaceRelFile.tablespace,
						info->pgClassStoredRelations[sr].tablespaceRelFile.relation,
						info->relSegFiles[rsf].tablespaceRelFile.relation);

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
				elog(ERROR, "In database %u, extra stored relation (tablespace %u, relation %u)",
				     info->database,
					 info->pgClassStoredRelations[sr].tablespaceRelFile.tablespace,
					 info->pgClassStoredRelations[sr].tablespaceRelFile.relation);

			if (rsf < info->relSegFilesCount)
				elog(ERROR, "In database %u, extra relation file on disk (tablespace %u, relation %u)",
				     info->database,
					 info->relSegFiles[rsf].tablespaceRelFile.tablespace,
					 info->relSegFiles[rsf].tablespaceRelFile.relation);
			break;
		}
	}

	
	/*
	 * Verify all tablespaces are in persistent database directory lookup.
	 *
	 * NOTE: There may be more persistent database directories (i.e. a superset),
	 * bit they are currently empty of relations.
	 */
	for (t = 0; t < info->tablespacesCount; t++)
	{
		DbDirNode dbDirNode;

		DatabaseEntry		checkDatabaseEntry;
		DatabaseDirEntry	checkDatabaseDirEntry;
		
		dbDirNode.tablespace = info->tablespaces[t];
		dbDirNode.database = info->database;
	
		if (!PersistentDatabase_FindDir(
									&dbDirNode,
									&checkDatabaseEntry,
									&checkDatabaseDirEntry))
			elog(ERROR, "Did not find relation's database directory (tablespace %u, database %u) in persistent database lookup",
				 dbDirNode.tablespace,
				 dbDirNode.database);
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
