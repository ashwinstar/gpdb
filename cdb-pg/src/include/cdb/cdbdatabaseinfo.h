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
#include "catalog/pg_appendonly.h"

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

	int64		logicalEof;
} DbInfoGpRelationNode;

typedef struct DbInfoSegmentFile
{
	int32			segmentFileNum;

	int64			eof;
} DbInfoSegmentFile;

typedef struct DbInfoAppendOnlyCatalogSegmentInfo
{
	int32			segmentFileNum;

	int64			logicalEof;
} DbInfoAppendOnlyCatalogSegmentInfo;


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

	int			relnatts;

	DbInfoGpRelationNode 	*gpRelationNodes;
	int32		gpRelationNodesCount;
	int32		gpRelationNodesMaxCount;

	DbInfoAppendOnlyCatalogSegmentInfo 	*appendOnlyCatalogSegmentInfo;
	int32		appendOnlyCatalogSegmentInfoCount;
	int32		appendOnlyCatalogSegmentInfoMaxCount;

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

typedef struct PgAppendOnlyHashEntry
{
	// Key.

	Oid				relationId;

	// Pointer.
	
	AppendOnlyEntry *aoEntry;
} PgAppendOnlyHashEntry;

typedef struct DatabaseInfo
{
	Oid		database;
	Oid		defaultTablespace;

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

	bool		collectAppendOnlyCatalogSegmentInfo;

} DatabaseInfo;



/*
 * This module is for collects information of the existing files within a database.
 */

// -----------------------------------------------------------------------------
// Collect
// -----------------------------------------------------------------------------

extern DatabaseInfo *DatabaseInfo_Collect(
	Oid 		database,
	Oid 		defaultTablespace,
	bool		collectAppendOnlyCatalogSegmentInfo);

extern void DatabaseInfo_AlignAppendOnly(
	DatabaseInfo 			*info,

	DbInfoRel 				*dbInfoRel);

#ifdef suppress

extern void DatabaseInfo_Check(DatabaseInfo *info);

extern void DatabaseInfo_Trace(DatabaseInfo *info);

#endif

#endif   /* CDBDATABASEINFO_H */
