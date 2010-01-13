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
#include "catalog/gp_persistent.h"

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
	RelFileNode		relFileNode;

	DbDirNode		dbDirNode;

	Oid				tablespace;

	Oid				filespace;
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
	memcpy(&(fsObjName->variant.relFileNode), relFileNode, sizeof(RelFileNode));
}

inline static void PersistentFileSysObjName_SetDatabaseDir(
	PersistentFileSysObjName	*fsObjName,
	Oid							tablespace,
	Oid							database)
{
	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = PersistentFsObjType_DatabaseDir;
	fsObjName->variant.dbDirNode.tablespace = tablespace;
	fsObjName->variant.dbDirNode.database = database;
}

inline static void PersistentFileSysObjName_SetTablespaceDir(
	PersistentFileSysObjName	*fsObjName,
	Oid							tablespace)
{
	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = PersistentFsObjType_TablespaceDir;
	fsObjName->variant.tablespace = tablespace;
}

inline static void PersistentFileSysObjName_SetFilespaceDir(
	PersistentFileSysObjName	*fsObjName,
	Oid							filespace)
{
	MemSet(fsObjName, 0, sizeof(PersistentFileSysObjName));
	fsObjName->type = PersistentFsObjType_FilespaceDir;
	fsObjName->variant.filespace = filespace;
}

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

