/*-------------------------------------------------------------------------
 *
 * persistentfilesysobjname.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"

#include "catalog/catalog.h"
#include "catalog/gp_persistent.h"
#include "access/persistentfilesysobjname.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"
#include "storage/shmem.h"
#include "access/heapam.h"
#include "access/transam.h"


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

char *PersistentFileSysObjName_ObjectName(
	const PersistentFileSysObjName		*name)
{
	Oid oid = 0;
	char *path;
	
	switch (name->type)
	{
	case PersistentFsObjType_RelationFile:	
		return relpath(name->variant.rel.relFileNode);
	case PersistentFsObjType_DatabaseDir:	
		return GetDatabasePath(
						name->variant.dbDirNode.database,
						name->variant.dbDirNode.tablespace);

	case PersistentFsObjType_TablespaceDir:
		{
			int resultLen;
			char *result;

			resultLen = 12 + 25 + 12 + 1;
			result = (char*)palloc(resultLen);
			
			sprintf(result, "%u (maps to filespace %u)", 
					name->variant.tablespaceDirNode.tablespace, 
					name->variant.tablespaceDirNode.filespace);

			return result;
		}
	case PersistentFsObjType_FilespaceDir:
		oid = name->variant.filespaceOid;
		break;

	default:
		elog(ERROR, "Unexpected persistent file-system object type: %d",
			 name->type);
		return NULL;
	}
	path = (char *) palloc(15);
	snprintf(path, 15, "%u", oid);
	return path;
}

char *PersistentFileSysObjName_TypeName(
	const PersistentFsObjType		type)
{
	switch (type)
	{
	case PersistentFsObjType_RelationFile: 		return "Relation File";
	case PersistentFsObjType_DatabaseDir: 		return "Database Directory";
	case PersistentFsObjType_TablespaceDir: 	return "Tablespace Directory";
	case PersistentFsObjType_FilespaceDir: 		return "Filespace Directory";
	default:
		return "Unknown";
	}
}

char *PersistentFileSysObjName_TypeAndObjectName(
	const PersistentFileSysObjName		*name)
{
	char *typeName;
	char *objectName;

	char resultLen;
	char *result;

	typeName = PersistentFileSysObjName_TypeName(name->type);
	objectName = PersistentFileSysObjName_ObjectName(name);

	resultLen = strlen(typeName) + 4 + strlen(objectName) + 1;
	result = (char*)palloc(resultLen);

	snprintf(result, resultLen, "%s: '%s'", typeName, objectName);

	pfree(objectName);

	return result;
	
}


int PersistentFileSysObjName_Compare(
	const PersistentFileSysObjName		*name1,
	const PersistentFileSysObjName		*name2)
{
	int compareLen = 0;
	int cmp;

	if (name1->type == name2->type)
	{		
		switch (name1->type)
		{
		case PersistentFsObjType_RelationFile:	
			compareLen = sizeof(RelFileNode);
			break;

		case PersistentFsObjType_DatabaseDir:	
			compareLen = sizeof(DbDirNode);
			break;
		case PersistentFsObjType_TablespaceDir: 
			compareLen = sizeof(Oid);
			break;
		case PersistentFsObjType_FilespaceDir:	
			compareLen = sizeof(Oid);
			break;
		default:
			elog(ERROR, "Unexpected persistent file-system object type: %d",
				 name1->type);
			return 0;
		}

		cmp = memcmp(
					&name1->variant, 
					&name2->variant,
					compareLen);
		if (cmp == 0)
			return 0;
		else if (cmp > 0)
			return 1;
		else
			return -1;
	}
	else if (name1->type > name2->type)
		return 1;
	else
		return -1;
}

char *PersistentFileSysObj_StateName(
	PersistentFileSysState state)
{
	switch (state)
	{
	case PersistentFileSysState_Free: 			return "Free";
	case PersistentFileSysState_CreatePending: 	return "Create Pending";
	case PersistentFileSysState_Created: 		return "Created";
	case PersistentFileSysState_DropPending: 	return "Drop Pending";
	case PersistentFileSysState_AbortingCreate: return "Aborting Create";
	default:
		return "Unknown";
	}
}

