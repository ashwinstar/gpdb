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

#define PersistentStoreData_StaticInit {0,NULL,NULL,NULL,0,0,0}

typedef struct PersistentStoreData
{
	GpGlobalSequence			gpGlobalSequence;

	PersistentStoreOpenRel		openRel;

	PersistentStoreCloseRel		closeRel;

	PersistentStoreScanTupleCallback	scanTupleCallback;

	int				numAttributes;

	int				attNumPersistentSerialNum;

	int				attNumPreviousFreeTid;
} PersistentStoreData;

extern void PersistentStore_InitShared(
	PersistentStoreSharedData 	*storeSharedData);

extern void PersistentStore_Init(
	PersistentStoreData 		*storeData,

	GpGlobalSequence			gpGlobalSequence,
	
	PersistentStoreOpenRel		openRel,

	PersistentStoreCloseRel 	closeRel,

	PersistentStoreScanTupleCallback	scanTupleCallback,

	int 						numAttributes,

	int 						attNumPersistentSerialNum,

	int 						attNumPreviousFreeTid);

extern void PersistentStore_InitScanUnderLock(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	int							token);

extern void PersistentStore_InitScan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	int							token);

extern void PersistentStore_Scan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData	*storeSharedData,

	int							token,

	PersistentStoreScanTupleCallback	scanTupleCallback);

extern void PersistentStore_FlushXLog(void);

extern void PersistentStore_UpdateTuple(
		PersistentStoreData 		*storeData,
	
		PersistentStoreSharedData	*storeSharedData,
	
		ItemPointer 			persistentTid,
					/* TID of the stored tuple. */
	
		Datum					*values,
	
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

