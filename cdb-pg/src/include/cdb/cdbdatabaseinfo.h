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

typedef struct TablespaceRelFile
{
	Oid		tablespace;

	Oid		relation;
} TablespaceRelFile;

typedef struct PgClassStoredRelation
{
	TablespaceRelFile		tablespaceRelFile;

	bool					isBufferPoolRelation;

	ItemPointerData			pgClassTid;

} PgClassStoredRelation;

typedef struct GpRelationNode
{
	TablespaceRelFile		tablespaceRelFile;

	ItemPointerData 		persistentTid;

	int64					persistentSerialNum;

	ItemPointerData			gpRelationNodeTid;
	
} GpRelationNode;

typedef struct RelationSegmentFile
{
	TablespaceRelFile		tablespaceRelFile;

	int32	segmentFileNum;

} RelationSegmentFile;

typedef struct MiscEntry
{
	Oid		tablespace;

	bool	isDir;

	char	*name;
} MiscEntry;


typedef struct DatabaseInfo
{
	Oid		database;

	Oid		*tablespaces;
	int32	tablespacesCount;
	int32	tablespacesMaxCount;

	PgClassStoredRelation		*pgClassStoredRelations;
	int32						pgClassStoredRelationsCount;
	int32						pgClassStoredRelationsMaxCount;

	GpRelationNode		*gpRelationNodes;
	int32				gpRelationNodesCount;
	int32				gpRelationNodesMaxCount;

	RelationSegmentFile		*relSegFiles;
	int32					relSegFilesCount;
	int32					relSegFilesMaxCount;

	MiscEntry 	*miscEntries;
	int32		miscEntriesCount;
	int32		miscEntriesMaxCount;

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

extern void DatabaseInfo_Check(DatabaseInfo *info);

extern void DatabaseInfo_Trace(DatabaseInfo *info);

extern int TablespaceRelFile_Compare(const void *entry1, const void *entry2);

#endif   /* CDBDATABASEINFO_H */
