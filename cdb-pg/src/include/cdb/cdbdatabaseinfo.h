/*-------------------------------------------------------------------------
 *
 * cdbdatabaseinfo.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBDATABASEINFO_H
#define CDBDATABASEINFO_H

#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/itemptr.h"
#include "storage/dbdirnode.h"

typedef struct MiscEntry
{
	Oid		tablespace;

	bool	isDir;

	char	*name;
} MiscEntry;

typedef struct DbInfoGpRelationNode
{
	ItemPointerData		gpRelationNodeTid;

	Oid			relfilenodeOid;

	int32	segmentFileNum;

	ItemPointerData		persistentTid;

	int64		persistentSerialNum;
} DbInfoGpRelationNode;

typedef struct DbInfoSegmentFile
{
	int32			segmentFileNum;

	int64			eof;
} DbInfoSegmentFile;

typedef struct DbInfoRel
{
	// Key.

	Oid			relfilenodeOid;

	// Catalog.
	
	bool		inPgClass;

	ItemPointerData pgClassTid;

	Oid			relationOid;

	char		*relname;

	Oid			reltablespace;

	char		relkind;
	char		relstorage;

	DbInfoGpRelationNode 	*gpRelationNodes;
	int32		gpRelationNodesCount;
	int32		gpRelationNodesMaxCount;

	// File-System.

	DbInfoSegmentFile 	*physicalSegmentFiles;
	int32		physicalSegmentFilesCount;
	int32		physicalSegmentFilesMaxCount;

} DbInfoRel;

typedef struct DbInfoExtraSegmentFile
{
	Oid				relfilenode;

	int32			segmentFileNum;

	Oid				tablespaceOid;

	int64			eof;
} DbInfoExtraSegmentFile;

typedef struct DatabaseInfo
{
	Oid		database;

	Oid		*tablespaces;
	int32	tablespacesCount;
	int32	tablespacesMaxCount;

	DbInfoRel 	*dbInfoRelArray;
	int			dbInfoRelArrayCount;

	MiscEntry 	*miscEntries;
	int32		miscEntriesCount;
	int32		miscEntriesMaxCount;

	DbInfoExtraSegmentFile 	*extraSegmentFiles;
	int32		extraSegmentFilesCount;
	int32		extraSegmentFilesMaxCount;

	DbInfoGpRelationNode	*parentlessGpRelationNodes;
	int32		parentlessGpRelationNodesCount;
	int32		parentlessGpRelationNodesMaxCount;


} DatabaseInfo;



/*
 * This module is for collects information of the existing files within a database.
 */

// -----------------------------------------------------------------------------
// Collect
// -----------------------------------------------------------------------------

extern DatabaseInfo *DatabaseInfo_Collect(
		Oid 		database,
		Oid 		defaultTablespace);

#ifdef suppress

extern void DatabaseInfo_Check(DatabaseInfo *info);

extern void DatabaseInfo_Trace(DatabaseInfo *info);

#endif

#endif   /* CDBDATABASEINFO_H */
