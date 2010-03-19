/*-------------------------------------------------------------------------
 *
 * persistentendxactrec.h
 *	  Make and read persistent information in commit, abort, and prepared records.
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef PERSISTENTENDXACTREC_H
#define PERSISTENTENDXACTREC_H

#include "postgres.h"
#include "storage/dbdirnode.h"
#include "storage/relfilenode.h"
#include "storage/itemptr.h"
#include "access/persistentfilesysobjname.h"

/*
 * The end of transaction possibilities.
 */
typedef enum EndXactRecKind
{
	EndXactRecKind_None = 0,
	EndXactRecKind_Commit = 1,
	EndXactRecKind_Abort = 2,
	EndXactRecKind_Prepare = 3,
	MaxEndXactRecKind /* must always be last */
} EndXactRecKind;

/*
 * The persistent transaction object possibilities.
 */
typedef enum PersistentXactObjKind
{
	PersistentXactObjKind_None = 0,
	PersistentXactObjKind_CreateFsObjs = 1,
	PersistentXactObjKind_DropFsObjs = 2,
	MaxPersistentXactObjKind /* must always be last */
} PersistentXactObjKind;


/*
 * Structure containing the end transaction information for a relation file.
 */
typedef struct PersistentEndXactFsObj
{
	PersistentFileSysObjName 	fsObjName;

	PersistentFileSysRelStorageMgr relStorageMgr;

	ItemPointerData			persistentTid;

	int64					persistentSerialNum;

} PersistentEndXactFsObj;

typedef struct PeristentEndXaxtUntyped
{
	void 		*data;

	int32		len;

	int32		count;
} PeristentEndXaxtUntyped;

/*
 * Structure containing the objects read.
 */
typedef union PersistentEndXactRecObjects
{
	PeristentEndXaxtUntyped		untyped[MaxPersistentXactObjKind];

	struct typed
	{
		PersistentEndXactFsObj 		*createFsObjs;
		int32 						createFsObjsLen;
		int32 						createFsObjsCount;
		
		PersistentEndXactFsObj 		*dropFsObjs;
		int32 						dropFsObjsLen;
		int32 						dropFsObjsCount;
		
		// UNDONE: UnmirroredBulkLoads
		// UNDONE: UnmirroredAoInserts
	}typed; 
	
} PersistentEndXactRecObjects;

extern void PersistentEndXactRec_Init(
	PersistentEndXactRecObjects	*objects);

extern void PersistentEndXactRec_AddCreateFsObjs(
	PersistentEndXactRecObjects	*objects,
	PersistentEndXactFsObj		*createFsObjs,
	int							count);

extern int32 PersistentEndXactRec_FetchObjectsFromSmgr(
	PersistentEndXactRecObjects	*objects,

	EndXactRecKind				endXactRecKind,

	int16						*objectCount);

extern void PersistentEndXactRec_Print(
		char							*procName,
	
		PersistentEndXactRecObjects 	*objects);

extern int16 PersistentEndXactRec_ObjectCount(
	PersistentEndXactRecObjects	*objects,
	
	EndXactRecKind				endXactRecKind);

extern int32 PersistentEndXactRec_SerializeLen(
	PersistentEndXactRecObjects	*objects,
	
	EndXactRecKind				endXactRecKind);

extern void PersistentEndXactRec_Serialize(
		PersistentEndXactRecObjects *objects,
	
		EndXactRecKind				endXactRecKind,
	
		int16						*numOfObjects,
	
		uint8						*buffer,
	
		int32						bufferLen);

extern int32 PersistentEndXactRec_DeserializeLen(
		uint8						*buffer,
	
		int16						numOfObjects);

extern void PersistentEndXactRec_Deserialize(
	uint8						*buffer,

	int16						numOfObjects,

	PersistentEndXactRecObjects	*objects,

	uint8						**nextData);

#endif   /* PERSISTENTENDXACTREC_H */

