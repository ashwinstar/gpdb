/*-------------------------------------------------------------------------
 *
 * persistentfilesysobjname.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef PERSISTENTFILESYSOBJNAME_H
#define PERSISTENTFILESYSOBJNAME_H

#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"
#include "storage/dbdirnode.h"
#include "storage/tablespacedirnode.h"

typedef enum PersistentFsObjType
{
	PersistentFsObjType_First = 0,	// Must start at 0 for 0-based indexing.
	PersistentFsObjType_RelationFile = PersistentFsObjType_First,
	PersistentFsObjType_DatabaseDir = 1,
	PersistentFsObjType_TablespaceDir = 2,
	PersistentFsObjType_FilespaceDir = 3,
	PersistentFsObjType_Last = PersistentFsObjType_FilespaceDir,
	CountPersistentFsObjType = PersistentFsObjType_Last - PersistentFsObjType_First + 1
} PersistentFsObjType;

typedef union PersistentFileSysObjNameVariant
{
	struct rel
	{
		RelFileNode 	relFileNode;

		int32 segmentFileNum;
	} rel; 

	DbDirNode		dbDirNode;

	Oid 	tablespaceOid;
	
	Oid 	filespaceOid;

} PersistentFileSysObjNameVariant;

typedef struct PersistentFileSysObjName
{
	PersistentFsObjType		type;

	PersistentFileSysObjNameVariant variant;
} PersistentFileSysObjName;

inline static void PersistentFileSysObjName_SetRelationFile(
	PersistentFileSysObjName	*fsObjName,
	RelFileNode					*relFileNode)
{
	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = PersistentFsObjType_RelationFile;
	memcpy(&(fsObjName->variant.rel.relFileNode), relFileNode, sizeof(RelFileNode));
	fsObjName->variant.rel.segmentFileNum = 0;	// UNDONE
}

inline static void PersistentFileSysObjName_SetDatabaseDir(
	PersistentFileSysObjName	*fsObjName,
	Oid							tablespaceOid,
	Oid							databaseOid)
{
	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = PersistentFsObjType_DatabaseDir;
	fsObjName->variant.dbDirNode.tablespace = tablespaceOid;
	fsObjName->variant.dbDirNode.database = databaseOid;
}

inline static void PersistentFileSysObjName_SetTablespaceDir(
	PersistentFileSysObjName	*fsObjName,
	Oid							tablespaceOid)
{
	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = PersistentFsObjType_TablespaceDir;
	fsObjName->variant.tablespaceOid = tablespaceOid;
}

inline static void PersistentFileSysObjName_SetFilespaceDir(
	PersistentFileSysObjName	*fsObjName,
	Oid							filespaceOid)
{
	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = PersistentFsObjType_FilespaceDir;
	fsObjName->variant.filespaceOid = filespaceOid;
}

/*
 * The file kinds of a persistent file-system object.
 */
typedef enum PersistentFileSysRelStorageMgr
{
	PersistentFileSysRelStorageMgr_None = 0,
	PersistentFileSysRelStorageMgr_BufferPool = 1,
	PersistentFileSysRelStorageMgr_AppendOnly = 2,
	MaxPersistentFileSysRelStorageMgr /* must always be last */
} PersistentFileSysRelStorageMgr;

/*
 * The states of a persistent file-system object.
 */
typedef enum PersistentFileSysState
{
	PersistentFileSysState_Free = 0,
	PersistentFileSysState_CreatePending = 1,
	PersistentFileSysState_Created = 2,
	PersistentFileSysState_DropPending = 3,
	PersistentFileSysState_AbortingCreate = 4,
	MaxPersistentFileSysState /* must always be last */
} PersistentFileSysState;

/*
 * The states of a persistent file-system object.
 */
typedef enum MirrorFileSysState
{
	MirrorFileSysState_None = 0,
	MirrorFileSysState_Synchronized = 1,
	MirrorFileSysState_PageIncremental = 2,
	MirrorFileSysState_FileIncremental = 3,
	MirrorFileSysState_All = 4,
	MaxMirrorFileSysState /* must always be last */
} MirrorFileSysState;


/*
 * This module is for generic relation file create and drop.
 *
 * For create, it makes the file-system create of an empty file fully transactional so
 * the relation file will be deleted even on system crash.  The relation file could be a heap,
 * index, or append-only (row- or column-store).
 */

// -----------------------------------------------------------------------------
// Helper
// -----------------------------------------------------------------------------

extern char *PersistentFileSysObjName_ObjectName(
	const PersistentFileSysObjName		*name);

extern char *PersistentFileSysObjName_TypeName(
		PersistentFsObjType		type);

extern char *PersistentFileSysObjName_TypeAndObjectName(
		const PersistentFileSysObjName		*name);

extern int PersistentFileSysObjName_Compare(
	const PersistentFileSysObjName		*name1,
	const PersistentFileSysObjName		*name2);

extern char *PersistentFileSysObj_StateName(
		PersistentFileSysState state);
																																																																
#endif   /* PERSISTENTFILESYSOBJNAME_H */

