/*-------------------------------------------------------------------------
 *
 * cdbpersistentstore.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPERSISTENTSTORE_H
#define CDBPERSISTENTSTORE_H

#include "utils/palloc.h"
#include "storage/fd.h"
#include "access/persistentfilesysobjname.h"
#include "catalog/gp_global_sequence.h"
#include "storage/itemptr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "utils/rel.h"

#define PersistentStoreSharedData_StaticInit {false,0,0,{{0,0},0},{{0,0},0}}

typedef struct PersistentStoreSharedData
{
	bool	needToScanIntoSharedMemory;

	int64					maxInUseSerialNum;

	int64 					maxFreeOrderNum;

	ItemPointerData			freeTid;

	ItemPointerData			maxTid;

} PersistentStoreSharedData;

typedef Relation (*PersistentStoreOpenRel) (void);
typedef void (*PersistentStoreCloseRel) (Relation relation);
typedef bool (*PersistentStoreScanTupleCallback) (
								int 					token,
								ItemPointer 			persistentTid,
								int64					persistentSerialNum,
								Datum					*values);

#define PersistentStoreData_StaticInit {0,0,NULL,NULL,NULL,0,0,0}

typedef struct PersistentStoreData
{
	int							token;

	GpGlobalSequence			gpGlobalSequence;

	PersistentStoreOpenRel		openRel;

	PersistentStoreCloseRel		closeRel;

	PersistentStoreScanTupleCallback	scanTupleCallback;

	int64			myHighestSerialNum;

	int				numAttributes;

	int				attNumPersistentSerialNum;

	int				attNumPreviousFreeTid;
} PersistentStoreData;

typedef struct PersistentStoreScan
{
	PersistentStoreData 		*storeData;

	PersistentStoreSharedData 	*storeSharedData;

	Relation					persistentRel;

	HeapScanDesc				scan;

	HeapTuple					tuple;
} PersistentStoreScan;

inline static bool PersistentStore_IsZeroTid(
	ItemPointer		testTid)
{
	static ItemPointerData zeroTid = {{0,0},0};
	
	return (ItemPointerCompare(testTid, &zeroTid) == 0);
}


extern void PersistentStore_InitShared(
	PersistentStoreSharedData 	*storeSharedData);

extern void PersistentStore_Init(
	PersistentStoreData 		*storeData,

	int							token,

	GpGlobalSequence			gpGlobalSequence,
	
	PersistentStoreOpenRel		openRel,

	PersistentStoreCloseRel 	closeRel,

	PersistentStoreScanTupleCallback	scanTupleCallback,

	int 						numAttributes,

	int 						attNumPersistentSerialNum,

	int 						attNumPreviousFreeTid);

extern void PersistentStore_InitScanUnderLock(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData);

extern void PersistentStore_Scan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData	*storeSharedData,

	PersistentStoreScanTupleCallback	scanTupleCallback);

extern void PersistentStore_BeginScan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData	*storeSharedData,

	PersistentStoreScan			*storeScan);

extern bool PersistentStore_GetNext(
	PersistentStoreScan			*storeScan,

	Datum						*values,

	ItemPointer					persistentTid,

	int64						*persistentSerialNum);

extern HeapTuple PersistentStore_GetScanTupleCopy(
	PersistentStoreScan			*storeScan);

extern void PersistentStore_EndScan(
	PersistentStoreScan			*storeScan);

extern void PersistentStore_FlushXLog(void);

extern int64 PersistentStore_MyHighestSerialNum(
	PersistentStoreData 	*storeData);

extern int64 PersistentStore_CurrentMaxSerialNum(
	PersistentStoreSharedData 	*storeSharedData);

extern void PersistentStore_UpdateTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData	*storeSharedData,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	Datum					*values,

	bool					flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */

extern void PersistentStore_ReplaceTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData	*storeSharedData,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	HeapTuple				tuple,

	Datum					*newValues,
	
	bool					*replaces,

	bool					flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */

extern void PersistentStore_ReadTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	ItemPointer					readTid,

	Datum						*values,

	HeapTuple					*tupleCopy);

extern void PersistentStore_AddTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	Datum					*values,

	bool					flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	int64					*persistentSerialNum);

extern void PersistentStore_FreeTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	Datum					*freeValues,

	bool					flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */

#endif   /* CDBPERSISTENTSTORE_H */

