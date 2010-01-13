/*-------------------------------------------------------------------------
 *
 * cdbpersistentstore.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/palloc.h"

#include "cdb/cdbpersistentstore.h"

#include "storage/itemptr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "storage/ipc.h"
#include "cdb/cdbglobalsequence.h"

void PersistentStore_InitShared(
	PersistentStoreSharedData 	*storeSharedData)
{
	MemSet(storeSharedData, 0, sizeof(PersistentStoreSharedData));

	storeSharedData->needToScanIntoSharedMemory = true;	
}

void PersistentStore_Init(
	PersistentStoreData 		*storeData,

	PersistentFsObjType 		fsObjType,

	GpGlobalSequence			gpGlobalSequence,
	
	PersistentStoreOpenRel		openRel,

	PersistentStoreCloseRel 	closeRel,

	PersistentStoreScanTupleCallback	scanTupleCallback,

	int 						numAttributes,

	int 						attNumPersistentSerialNum,

	int 						attNumPreviousFreeTid)
{
	MemSet(storeData, 0, sizeof(PersistentStoreData));

	storeData->fsObjType = fsObjType;
	storeData->gpGlobalSequence = gpGlobalSequence;
	storeData->openRel = openRel;
	storeData->closeRel = closeRel;
	storeData->scanTupleCallback = scanTupleCallback;
	storeData->numAttributes = numAttributes;
	storeData->attNumPersistentSerialNum = attNumPersistentSerialNum;
	storeData->attNumPreviousFreeTid = attNumPreviousFreeTid;
}

static void PersistentStore_DeformTuple(
	PersistentStoreData 	*storeData,

	TupleDesc				tupleDesc,			
						/* tuple descriptor */

    HeapTuple 				tuple,

	Datum					*values)
{
	bool	*nulls;

	int i;

	nulls = (bool*)palloc(storeData->numAttributes * sizeof(bool));

	heap_deform_tuple(tuple, tupleDesc, values, nulls);

	for (i = 1; i <= storeData->numAttributes; i++)
		Assert(!nulls[i - 1]);

	pfree(nulls);
}

static void PersistentStore_ExtractOurTupleData(
	PersistentStoreData 	*storeData,

	Datum					*values,

	int64					*persistentSerialNum,

	ItemPointer				previousFreeTid)
{
	*persistentSerialNum = DatumGetInt64(values[storeData->attNumPersistentSerialNum - 1]);

	*previousFreeTid = *((ItemPointer) DatumGetPointer(values[storeData->attNumPreviousFreeTid - 1]));
}

static void PersistentStore_FormTupleSetOurs(
	PersistentStoreData 	*storeData,

	TupleDesc				tupleDesc,
				/* Tuple descriptor. */

	Datum					*values,

	int64					persistentSerialNum,

	ItemPointerData			*previousFreeTid,

	HeapTuple	*tuple)
				/* The formed tuple. */
{
	int			natts = 0;

	bool	*nulls;

	natts = tupleDesc->natts;
	Assert(natts == storeData->numAttributes);

	/*
	 * In order to keep the tuples the exact same size to enable direct reuse of
	 * free tuples, we do not use NULLs.
	 */
	nulls = (bool*)palloc0(storeData->numAttributes * sizeof(bool));

	values[storeData->attNumPersistentSerialNum - 1] = 
											Int64GetDatum(persistentSerialNum);

	values[storeData->attNumPreviousFreeTid - 1] =
											PointerGetDatum(previousFreeTid);
		
	/*
	 * Form the tuple.
	 */
	*tuple = heap_form_tuple(tupleDesc, values, nulls);
	if (!HeapTupleIsValid(*tuple))
		elog(ERROR, "Failed to build persistent relation node tuple");

}

static void PersistentStore_DoInitScan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData)
{
	Relation		persistentRel;
    HeapScanDesc 	scan;
    HeapTuple 		tuple;

	int64					persistentSerialNum;
	ItemPointerData			previousFreeTid;

	Datum					*values;

	ItemPointerData			zeroTid;

	int64			globalSequenceNum;

	MemSet(&zeroTid, 0, sizeof(ItemPointerData));

	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	persistentRel = (*storeData->openRel)();
	
    scan = heap_beginscan(persistentRel, SnapshotNow, 0, NULL);

	MemSet(&storeSharedData->maxTid, 0, sizeof(ItemPointerData));
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Assert(ItemPointerCompare(
							&storeSharedData->maxTid,
							&tuple->t_self) == -1);	// Less-Than.
							
		storeSharedData->maxTid = tuple->t_self;
		
		PersistentStore_DeformTuple(
								storeData,
								persistentRel->rd_att,
								tuple,
								values);
		PersistentStore_ExtractOurTupleData(
									storeData,
									values,
									&persistentSerialNum,
									&previousFreeTid);

//		elog(WARNING, 
//			 "PersistentStore_DoInitScan: TID %s, serial number " INT64_FORMAT ", previous free TID %s",
//			 ItemPointerToString(&tuple->t_self),
//			 persistentSerialNum, 
//			 ItemPointerToString2(&previousFreeTid));
		
		if (ItemPointerCompare(&previousFreeTid, &zeroTid) != 0)
		{
			if (storeSharedData->maxFreeOrderNum < persistentSerialNum)
			{
				storeSharedData->maxFreeOrderNum = persistentSerialNum;
				storeSharedData->freeTid = tuple->t_self;
			}
		}
		else if (storeSharedData->maxInUseSerialNum < persistentSerialNum)
		{
			storeSharedData->maxInUseSerialNum = persistentSerialNum;
		}

		if (storeData->scanTupleCallback != NULL)
			(*storeData->scanTupleCallback)(
										storeData->fsObjType,
										&tuple->t_self,
										persistentSerialNum,
										values);

	}
	
    heap_endscan(scan);

	(*storeData->closeRel)(persistentRel);

	pfree(values);

	globalSequenceNum = GlobalSequence_Current(storeData->gpGlobalSequence);
	if (globalSequenceNum < storeSharedData->maxInUseSerialNum)
		elog(ERROR, "Global sequence number " INT64_FORMAT " less than maximum value " INT64_FORMAT " found in scan",
		     globalSequenceNum,
		     storeSharedData->maxInUseSerialNum);

	storeSharedData->maxInUseSerialNum = globalSequenceNum;
}

void PersistentStore_InitScanUnderLock(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData)
{
	if (!storeSharedData->needToScanIntoSharedMemory)
	{
		return;	// Someone else got in first.
	}

	PersistentStore_DoInitScan(
						storeData,
						storeSharedData);

	storeSharedData->needToScanIntoSharedMemory = false;
}

void PersistentStore_InitScan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData)
{
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	PersistentStore_InitScanUnderLock(
							storeData,
							storeSharedData);

	LWLockRelease(PersistentObjLock);
}

void PersistentStore_Scan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	PersistentStoreScanTupleCallback	scanTupleCallback)
{
	Relation		persistentRel;
    HeapScanDesc 	scan;
    HeapTuple 		tuple;

	int64					persistentSerialNum;
	ItemPointerData			previousFreeTid;

	Datum					*values;

	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	persistentRel = (*storeData->openRel)();
	
	LWLockAcquire(PersistentObjLock, LW_EXCLUSIVE);

	scan = heap_beginscan(persistentRel, SnapshotNow, 0, NULL);

	MemSet(&storeSharedData->maxTid, 0, sizeof(ItemPointerData));
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool okToContinue;

		PersistentStore_DeformTuple(
								storeData,
								persistentRel->rd_att,
								tuple,
								values);
		PersistentStore_ExtractOurTupleData(
									storeData,
									values,
									&persistentSerialNum,
									&previousFreeTid);

		okToContinue = (*scanTupleCallback)(
								storeData->fsObjType,
								&tuple->t_self,
								persistentSerialNum,
								values);

		if (!okToContinue)
			break;

	}
	
	heap_endscan(scan);

	(*storeData->closeRel)(persistentRel);

	pfree(values);

	LWLockRelease(PersistentObjLock);
}


// -----------------------------------------------------------------------------
// Helpers 
// -----------------------------------------------------------------------------

static XLogRecPtr nowaitXLogEndLoc;

inline static void XLogRecPtr_Zero(XLogRecPtr *xlogLoc)
{
	MemSet(xlogLoc, 0, sizeof(XLogRecPtr));
}

void PersistentStore_FlushXLog(void)
{
	if (nowaitXLogEndLoc.xlogid != 0 ||
		nowaitXLogEndLoc.xrecoff != 0)
	{
		XLogFlush(nowaitXLogEndLoc);
		XLogRecPtr_Zero(&nowaitXLogEndLoc);
	}
}

static void PersistentStore_DoInsertTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	Relation				persistentRel,
				/* The persistent table relation. */

	Datum					*values,

	bool					flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 			persistentTid)
				/* TID of the stored tuple. */

{
	bool 		*nulls;
	HeapTuple	persistentTuple = NULL;
	XLogRecPtr	xlogInsertEndLoc;

	/*
	 * In order to keep the tuples the exact same size to enable direct reuse of
	 * free tuples, we do not use NULLs.
	 */
	nulls = (bool*)palloc0(storeData->numAttributes * sizeof(bool));
		
	/*
	 * Form the tuple.
	 */
	persistentTuple = heap_form_tuple(persistentRel->rd_att, values, nulls);
	if (!HeapTupleIsValid(persistentTuple))
		elog(ERROR, "Failed to build persistent relation node tuple");

	/*
	 * (We have an exclusive lock (higher up) here so we can direct the insert to the last page.)
	 */
	{
		// Do not assert valid ItemPointer -- it is ok if it is (0,0)...
		BlockNumber blockNumber = 
						BlockIdGetBlockNumber(
								&storeSharedData->maxTid.ip_blkid);
		
		frozen_heap_insert_directed(
							persistentRel, 
							persistentTuple,
							blockNumber);
	}

	if (ItemPointerCompare(
						&storeSharedData->maxTid,
						&persistentTuple->t_self) == -1)		
	{
		// Current max is Less-Than.
		storeSharedData->maxTid = persistentTuple->t_self;
	}
	
	/*
	 * Return the TID of the INSERT tuple.
	 * Return the XLOG location of the INSERT tuple's XLOG record.
	 */
	*persistentTid = persistentTuple->t_self;
		
	xlogInsertEndLoc = XLogLastInsertEndLoc();

	heap_freetuple(persistentTuple);

	if (flushToXLog)
	{
		XLogFlush(xlogInsertEndLoc);
		XLogRecPtr_Zero(&nowaitXLogEndLoc);
	}
	else
		nowaitXLogEndLoc = xlogInsertEndLoc;

	pfree(nulls);

}

static void PersistentStore_InsertTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	Datum					*values,

	bool					flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 			persistentTid)
				/* TID of the stored tuple. */

{
	Relation	persistentRel;

	persistentRel = (*storeData->openRel)();

	PersistentStore_DoInsertTuple(
								storeData,
								storeSharedData,
								persistentRel,
								values,
								flushToXLog,
								persistentTid);

	(*storeData->closeRel)(persistentRel);
}

void PersistentStore_UpdateTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	Datum					*values,

	bool					flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	Relation	persistentRel;
	bool 		*nulls;
	HeapTuple	persistentTuple = NULL;
	XLogRecPtr 	xlogUpdateEndLoc;
	
	persistentRel = (*storeData->openRel)();

	/*
	 * In order to keep the tuples the exact same size to enable direct reuse of
	 * free tuples, we do not use NULLs.
	 */
	nulls = (bool*)palloc0(storeData->numAttributes * sizeof(bool));
		
	/*
	 * Form the tuple.
	 */
	persistentTuple = heap_form_tuple(persistentRel->rd_att, values, nulls);
	if (!HeapTupleIsValid(persistentTuple))
		elog(ERROR, "Failed to build persistent relation node tuple");

	persistentTuple->t_self = *persistentTid;
		
	heap_inplace_update(persistentRel, persistentTuple);

	/*
	 * Return the XLOG location of the UPDATE tuple's XLOG record.
	 */
	xlogUpdateEndLoc = XLogLastInsertEndLoc();

	heap_freetuple(persistentTuple);

	(*storeData->closeRel)(persistentRel);

	if (flushToXLog)
	{
		XLogFlush(xlogUpdateEndLoc);
		XLogRecPtr_Zero(&nowaitXLogEndLoc);
	}
	else
		nowaitXLogEndLoc = xlogUpdateEndLoc;
}

void PersistentStore_ReadTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	ItemPointer					readTid,

	Datum						*values,

	HeapTuple					*tupleCopy)
{
	Relation	persistentRel;

	HeapTupleData 	tuple;
	Buffer			buffer;

	bool *nulls;

	persistentRel = (*storeData->openRel)();

	tuple.t_self = *readTid;
	
	if (!heap_fetch(persistentRel, SnapshotAny,
					&tuple, &buffer, false, NULL))
		elog(ERROR, "Failed to fetch persistent tuple at %s",
			 ItemPointerToString(&tuple.t_self));

	
	*tupleCopy = heaptuple_copy_to(&tuple, NULL, NULL);

	ReleaseBuffer(buffer);
	
	/*
	 * In order to keep the tuples the exact same size to enable direct reuse of
	 * free tuples, we do not use NULLs.
	 */
	nulls = (bool*)palloc(storeData->numAttributes * sizeof(bool));

	heap_deform_tuple(*tupleCopy, persistentRel->rd_att, values, nulls);

	(*storeData->closeRel)(persistentRel);

	pfree(nulls);
}


static bool PersistentStore_GetFreeTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	ItemPointer				freeTid)
{
	ItemPointerData			zeroTid;

	Datum			*values;
	HeapTuple		tupleCopy;

	int64					persistentSerialNum;
	ItemPointerData			previousFreeTid;

	MemSet(&zeroTid, 0, sizeof(ItemPointerData));

	MemSet(freeTid, 0, sizeof(ItemPointerData));

	if (storeSharedData->maxFreeOrderNum == 0)
	{
		return false;	// No free tuples.
	}

	Assert(storeSharedData->freeTid.ip_posid != 0);

	/*
	 * Read the current last free tuple.
	 */
	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));
	
	PersistentStore_ReadTuple(
						storeData,
						storeSharedData,
						&storeSharedData->freeTid,
						values,
						&tupleCopy);

	PersistentStore_ExtractOurTupleData(
								storeData,
								values,
								&persistentSerialNum,
								&previousFreeTid);

	if (ItemPointerCompare(&previousFreeTid, &zeroTid) == 0)
		elog(ERROR, "Expected persistent store tuple at %s to be in free", 
			 ItemPointerToString(&storeSharedData->freeTid));
	if (storeSharedData->maxFreeOrderNum == 1)
		Assert(ItemPointerCompare(&previousFreeTid, &storeSharedData->freeTid) == 0);

	if (persistentSerialNum != storeSharedData->maxFreeOrderNum)
		elog(ERROR, "Expected persistent store tuple at %s to have order number " INT64_FORMAT " (found " INT64_FORMAT ")", 
			 ItemPointerToString(&storeSharedData->freeTid),
			 storeSharedData->maxFreeOrderNum,
			 persistentSerialNum);

	*freeTid = storeSharedData->freeTid;
	storeSharedData->maxFreeOrderNum--;
	storeSharedData->freeTid = previousFreeTid;

	pfree(values);

	heap_freetuple(tupleCopy);

	return true;
}

void PersistentStore_AddTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	Datum					*values,

	bool					flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	int64					*persistentSerialNum)

{
	ItemPointerData zeroTid;

	MemSet(&zeroTid, 0, sizeof(ItemPointerData));

	*persistentSerialNum = ++storeSharedData->maxInUseSerialNum;
	GlobalSequence_Set(
				storeData->gpGlobalSequence,
				*persistentSerialNum);
	
	// UNDONE: Move somewhere else...
	values[storeData->attNumPersistentSerialNum - 1] = 
										Int64GetDatum(*persistentSerialNum);

	values[storeData->attNumPreviousFreeTid - 1] = 
										PointerGetDatum(&zeroTid);

	if (!Debug_filerep_skip_persistent_free_list &&
		PersistentStore_GetFreeTuple(
								storeData,
								storeSharedData,
								persistentTid))
	{
		Assert(persistentTid->ip_posid != 0);
		PersistentStore_UpdateTuple(
									storeData,
									storeSharedData,
									persistentTid,
									values,
									flushToXLog);
	}
	else
	{
		/*
		 * Add new tuple.
		 */

		PersistentStore_InsertTuple(
								storeData,
								storeSharedData,
								values,
								flushToXLog,
								persistentTid);
		Assert(persistentTid->ip_posid != 0);
	}

}

void PersistentStore_FreeTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	Datum					*freeValues,

	bool					flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	Relation	persistentRel;
	HeapTuple	persistentTuple = NULL;
	ItemPointerData prevFreeTid;
	XLogRecPtr xlogEndLoc;
				/* The end location of the UPDATE XLOG record. */

	Assert(persistentTid->ip_posid != 0);

	persistentRel = (*storeData->openRel)();

	storeSharedData->maxFreeOrderNum++;
	if (storeSharedData->maxFreeOrderNum == 1)
		prevFreeTid = *persistentTid;		// So non-zero PreviousFreeTid indicates free.
	else
		prevFreeTid = storeSharedData->freeTid;
	storeSharedData->freeTid = *persistentTid;

	PersistentStore_FormTupleSetOurs(
							storeData,
							persistentRel->rd_att,
							freeValues,
							storeSharedData->maxFreeOrderNum,
							&prevFreeTid,
							&persistentTuple);

	persistentTuple->t_self = *persistentTid;
		
	heap_inplace_update(persistentRel, persistentTuple);

	/*
	 * XLOG location of the UPDATE tuple's XLOG record.
	 */
	xlogEndLoc = XLogLastInsertEndLoc();

	heap_freetuple(persistentTuple);

	(*storeData->closeRel)(persistentRel);

	if (flushToXLog)
	{
		XLogFlush(xlogEndLoc);
		XLogRecPtr_Zero(&nowaitXLogEndLoc);
	}
	else
		nowaitXLogEndLoc = xlogEndLoc;
}
