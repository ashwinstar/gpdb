/*-------------------------------------------------------------------------
 *
 * persistentendxactrec.c
 *	  Make and read persistent information in commit, abort, and prepared records.
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "access/persistentendxactrec.h"
#include "storage/smgr.h"
#include "utils/guc.h"

/*
 * Structure containing an object.
 */
typedef struct PersistentEndXactRecObjHdr
{
	int32 	len;
	int16   objKind;

	uint8	data[0];
} PersistentEndXactRecObjHdr;

void PersistentEndXactRec_Print(
	char 							*procName,

	PersistentEndXactRecObjects		*objects)
{
	int i;

	elog(Debug_persistent_print_level, 
		 "%s: create count %d, drop count %d",
		 procName,
		 objects->typed.createFsObjsCount,
		 objects->typed.dropFsObjsCount);

	for (i = 0; i < objects->typed.createFsObjsCount; i++)
	{
		PersistentEndXactFsObj *fsObj = 
					&objects->typed.createFsObjs[i];

		elog(Debug_persistent_print_level, 
			 "%s: [%d] %s, persistent serial num " INT64_FORMAT ", TID %s, Create on Commit",
			 procName,
			 i,
			 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
			 fsObj->persistentSerialNum,
			 ItemPointerToString(&fsObj->persistentTid));
		
	}

	for (i = 0; i < objects->typed.dropFsObjsCount; i++)
	{
		PersistentEndXactFsObj *fsObj = 
					&objects->typed.dropFsObjs[i];

		elog(Debug_persistent_print_level, 
			 "%s: [%d] %s, persistent serial num " INT64_FORMAT ", TID %s, Drop on Commit",
			 procName,
			 i,
			 PersistentFileSysObjName_TypeAndObjectName(&fsObj->fsObjName),
			 fsObj->persistentSerialNum,
			 ItemPointerToString(&fsObj->persistentTid));
	}
}

static int PersistentEndXactRec_CountForObject(
	PersistentXactObjKind objKind,

	int32 len)
{
	int count = 0;
	
	switch (objKind)
	{
	case PersistentXactObjKind_CreateFsObjs:
	case PersistentXactObjKind_DropFsObjs:
		count = len / sizeof(PersistentEndXactFsObj);
		break;
		
	default:
		elog(ERROR, "Unexpected persistent transaction object kind: %d",
			 objKind);
	}

	return count;
}

static void PersistentEndXactRec_SetObjectInfo(
	PersistentEndXactRecObjects	*objects,
	
	PersistentXactObjKind 		objKind,

	void *data,

	int count,

	int32 len)
{
	PeristentEndXaxtUntyped *untyped = &objects->untyped[objKind-1];

	untyped->data = data;
	untyped->count = count;
	untyped->len = len;
}

static void PersistentEndXactRec_GetObjectInfo(
	PersistentEndXactRecObjects	*objects,
	
	PersistentXactObjKind 		objKind,

	uint8 **data,

	int *count,

	int32 *len)
{
	PeristentEndXaxtUntyped *untyped = &objects->untyped[objKind-1];

	*data = untyped->data;
	*count = untyped->count;
	*len = untyped->len;
}

static bool PersistentEndXactRec_IncludeObjKind(
	PersistentXactObjKind	objKind,

	EndXactRecKind			endXactRecKind)
{
	if (endXactRecKind == EndXactRecKind_Abort &&
		objKind != PersistentXactObjKind_CreateFsObjs)
		return false;

	return true;
}

void PersistentEndXactRec_Init(
	PersistentEndXactRecObjects	*objects)
{
	MemSet(objects, 0, sizeof(PersistentEndXactRecObjects));
}

void PersistentEndXactRec_AddCreateFsObjs(
	PersistentEndXactRecObjects	*objects,
	PersistentEndXactFsObj		*createFsObjs,
	int							count)
{
	objects->typed.createFsObjs = createFsObjs;
	objects->typed.createFsObjsCount = count;
	objects->typed.createFsObjsLen = count * sizeof(PersistentEndXactFsObj);
}

int32 PersistentEndXactRec_FetchObjectsFromSmgr(
	PersistentEndXactRecObjects	*objects,

	EndXactRecKind				endXactRecKind,

	int16						*objectCount)
{
	int32 serializeLen;

	int objKind;
	void *data;
	int32 count;
	int32 len;

	MemSet(objects, 0, sizeof(PersistentEndXactRecObjects));
	*objectCount = 0;
	serializeLen = 0;

	for (objKind = 1; objKind < MaxPersistentXactObjKind; objKind++)
	{
		if (!PersistentEndXactRec_IncludeObjKind(objKind, endXactRecKind))
			continue;

		data = NULL;
		count = 0;
		len = 0;
		switch (objKind)
		{
		case PersistentXactObjKind_CreateFsObjs:
			count = smgrGetPendingDeletes(
									/* forCommit */ false,
									(PersistentEndXactFsObj**)&data);
			len = count * sizeof(PersistentEndXactFsObj);
			break;
			
		case PersistentXactObjKind_DropFsObjs:
			count = smgrGetPendingDeletes(
									/* forCommit */ true,
									(PersistentEndXactFsObj**)&data);
			len = count * sizeof(PersistentEndXactFsObj);
			break;
								
		default:
			elog(ERROR, "Unexpected persistent transaction object kind: %d",
				 objKind);
		}

		if (len > 0)
		{
			(*objectCount)++;

			serializeLen += offsetof(PersistentEndXactRecObjHdr, data) + len;

			PersistentEndXactRec_SetObjectInfo(
										objects, objKind, data, count, len);
		}
	}

	return serializeLen;
}

int16 PersistentEndXactRec_ObjectCount(
	PersistentEndXactRecObjects	*objects,
	
	EndXactRecKind				endXactRecKind)
{
	int16 objectCount;

	int objKind;
	uint8 *data;
	int count;
	int32 len;

	objectCount = 0;

	for (objKind = 1; objKind < MaxPersistentXactObjKind; objKind++)
	{
		if (!PersistentEndXactRec_IncludeObjKind(objKind, endXactRecKind))
			continue;

		PersistentEndXactRec_GetObjectInfo(
									objects, objKind, &data, &count, &len);
		if (len > 0)
		{
			objectCount++;
		}
	}

	return objectCount;
}


int32 PersistentEndXactRec_SerializeLen(
	PersistentEndXactRecObjects	*objects,
	
	EndXactRecKind				endXactRecKind)
{
	int32 fillLen;

	int objKind;
	uint8 *data;
	int count;
	int32 len;

	fillLen = 0;

	for (objKind = 1; objKind < MaxPersistentXactObjKind; objKind++)
	{
		if (!PersistentEndXactRec_IncludeObjKind(objKind, endXactRecKind))
			continue;

		PersistentEndXactRec_GetObjectInfo(
									objects, objKind, &data, &count, &len);
		if (len > 0)
		{
			fillLen += offsetof(PersistentEndXactRecObjHdr, data) + len;
		}
	}

	return fillLen;
}

static void PersistentEndXactRec_AddSection(
	uint8 					*buffer,

	int32					bufferLen,

	int32					*currentLen,

	PersistentXactObjKind	objKind,

	uint8					*data,

	int32					len)
{
	uint8 *next;
	PersistentEndXactRecObjHdr *header;
	const int headerLen = offsetof(PersistentEndXactRecObjHdr, data);

	Assert(len > 0);
	Assert(*currentLen + headerLen + len <= bufferLen);

	next = &buffer[*currentLen];
	
	header = (PersistentEndXactRecObjHdr*)next;
	MemSet(header, 0, headerLen);
	header->len = len;
	header->objKind = objKind;
	
	memcpy(header->data, data, len);

	(*currentLen) += headerLen + len;
}


void PersistentEndXactRec_Serialize(
	PersistentEndXactRecObjects	*objects,

	EndXactRecKind				endXactRecKind,

	int16						*numOfObjects,

	uint8 						*buffer,

	int32						bufferLen)

{
	int objKind;
	int32 currentLen;
	uint8 *data;
	int count;
	int32 len;

	*numOfObjects = 0;

	currentLen = 0;
	for (objKind = 1; objKind < MaxPersistentXactObjKind; objKind++)
	{
		if (!PersistentEndXactRec_IncludeObjKind(objKind, endXactRecKind))
			continue;

		PersistentEndXactRec_GetObjectInfo(
									objects, objKind, &data, &count, &len);
		if (len > 0)
		{
			PersistentEndXactRec_AddSection(
										buffer,
										bufferLen,
										&currentLen,
										objKind,
										data,
										len);
			(*numOfObjects)++;
		}
	}

	if (currentLen != bufferLen)
	{
		elog(ERROR, "Persistent end transaction serialize length mismatch (actual %d, supplied %d)",
		     currentLen, bufferLen);
	}
}

int32 PersistentEndXactRec_DeserializeLen(
	uint8						*buffer,

	int16						numOfObjects)
{
	int deserializeLen;

	int n;
	PersistentEndXactRecObjHdr *header;
	int32 len;

	deserializeLen = 0;
	header = (PersistentEndXactRecObjHdr*)buffer;
	for (n = 0; n < numOfObjects; n++)
	{
		len = header->len;

		deserializeLen += offsetof(PersistentEndXactRecObjHdr, data) + len;
		header = (PersistentEndXactRecObjHdr*)&buffer[deserializeLen];
	}

	return deserializeLen;
}

void PersistentEndXactRec_Deserialize(
	uint8						*buffer,

	int16						numOfObjects,

	PersistentEndXactRecObjects	*objects,

	uint8						**nextData)
{
	int32 currentLen;
	int n;
	int objKind;
	PersistentEndXactRecObjHdr *header;
	int32 len;
	uint8 *data;
	int count;

	MemSet(objects, 0, sizeof(PersistentEndXactRecObjects));

	currentLen = 0;
	header = (PersistentEndXactRecObjHdr*)buffer;
	for (n = 0; n < numOfObjects; n++)
	{

		len = header->len;
		objKind = header->objKind;
		data = header->data;

		count = PersistentEndXactRec_CountForObject(objKind, len);

		PersistentEndXactRec_SetObjectInfo(
									objects, objKind, data, count, len);

		currentLen += offsetof(PersistentEndXactRecObjHdr, data) + len;
		header = (PersistentEndXactRecObjHdr*)&buffer[currentLen];
	}

	*nextData = (uint8*)header;
}
