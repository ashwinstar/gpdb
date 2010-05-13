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

	int							token,

	GpGlobalSequence			gpGlobalSequence,
	
	PersistentStoreOpenRel		openRel,

	PersistentStoreCloseRel		closeRel,

	PersistentStoreScanTupleCallback	scanTupleCallback,

	int 						numAttributes,

	int 						attNumPersistentSerialNum,

	int 						attNumPreviousFreeTid)
{
	MemSet(storeData, 0, sizeof(PersistentStoreData));

	storeData->token = token;
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
	PersistentStoreScan storeScan;

	ItemPointerData			persistentTid;
	int64					persistentSerialNum;

	ItemPointerData			previousFreeTid;

	Datum					*values;

	int64			globalSequenceNum;

	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	MemSet(&storeSharedData->maxTid, 0, sizeof(ItemPointerData));

	PersistentStore_BeginScan(
						storeData,
						storeSharedData,
						&storeScan);

	while (PersistentStore_GetNext(
							&storeScan,
							values,
							&persistentTid,
							&persistentSerialNum))
	{
		Assert(ItemPointerCompare(
							&storeSharedData->maxTid,
							&persistentTid) == -1);	// Less-Than.
							
		storeSharedData->maxTid = persistentTid;

		PersistentStore_ExtractOurTupleData(
									storeData,
									values,
									&persistentSerialNum,
									&previousFreeTid);
		
		if (Debug_persistent_recovery_print)
			elog(Debug_persistent_recovery_print_level, 
				 "PersistentStore_DoInitScan (token %d): TID %s, serial number " INT64_FORMAT ", previous free TID %s",
				 storeData->token,
				 ItemPointerToString(&persistentTid),
				 persistentSerialNum, 
				 ItemPointerToString2(&previousFreeTid));
		
		if (!PersistentStore_IsZeroTid(&previousFreeTid))
		{
			if (storeSharedData->maxFreeOrderNum < persistentSerialNum)
			{
				storeSharedData->maxFreeOrderNum = persistentSerialNum;
				storeSharedData->freeTid = persistentTid;
			}
		}
		else if (storeSharedData->maxInUseSerialNum < persistentSerialNum)
		{
			storeSharedData->maxInUseSerialNum = persistentSerialNum;
			storeData->myHighestSerialNum = storeSharedData->maxInUseSerialNum;
		}

		if (storeData->scanTupleCallback != NULL)
			(*storeData->scanTupleCallback)(
										storeData->token,
										&persistentTid,
										persistentSerialNum,
										values);

	}

	PersistentStore_EndScan(&storeScan);

	pfree(values);

	globalSequenceNum = GlobalSequence_Current(storeData->gpGlobalSequence);

	/* Only test this outside of recovery scenarios */
	if (!InRecovery && globalSequenceNum < storeSharedData->maxInUseSerialNum)
		elog(ERROR, "Global sequence number " INT64_FORMAT " less than maximum value " INT64_FORMAT " found in scan (token %d)",
		     globalSequenceNum,
		     storeSharedData->maxInUseSerialNum,
		     storeData->token);

	storeSharedData->maxInUseSerialNum = globalSequenceNum;
	
	if (Debug_persistent_recovery_print)
		elog(Debug_persistent_recovery_print_level, 
			 "PersistentStore_DoInitScan (token %d): maximum in-use serial number " INT64_FORMAT ", maximum free order number " INT64_FORMAT ", free TID %s",
			 storeData->token,
			 storeSharedData->maxInUseSerialNum, 
			 storeSharedData->maxFreeOrderNum, 
			 ItemPointerToString2(&storeSharedData->freeTid));
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

void PersistentStore_Scan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	PersistentStoreScanTupleCallback	scanTupleCallback)
{
	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK_DECLARE;

	PersistentStoreScan storeScan;

	ItemPointerData			persistentTid;
	int64					persistentSerialNum = 0;

	Datum					*values;

	values = (Datum*)palloc(storeData->numAttributes * sizeof(Datum));

	MIRRORED_PERSISTENTOBJ_ORDERED_LOCK;

	PersistentStore_BeginScan(
						storeData,
						storeSharedData,
						&storeScan);

	while (PersistentStore_GetNext(
							&storeScan,
							values,
							&persistentTid,
							&persistentSerialNum))
	{
		bool okToContinue;

		okToContinue = (*scanTupleCallback)(
								storeData->token,
								&persistentTid,
								persistentSerialNum,
								values);

		if (!okToContinue)
			break;

	}
	
	PersistentStore_EndScan(&storeScan);

	MIRRORED_PERSISTENTOBJ_ORDERED_UNLOCK;

	pfree(values);
}

void PersistentStore_BeginScan(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData	*storeSharedData,

	PersistentStoreScan			*storeScan)
{
	MemSet(storeScan, 0, sizeof(PersistentStoreScan));

	storeScan->storeData = storeData;
	storeScan->storeSharedData = storeSharedData;

	storeScan->persistentRel = (*storeData->openRel)();

	storeScan->scan = heap_beginscan(
							storeScan->persistentRel, 
							SnapshotNow, 
							0, 
							NULL);

	storeScan->tuple = NULL;
}

bool PersistentStore_GetNext(
	PersistentStoreScan			*storeScan,

	Datum						*values,

	ItemPointer					persistentTid,

	int64						*persistentSerialNum)
{
	ItemPointerData			previousFreeTid;

	storeScan->tuple = heap_getnext(storeScan->scan, ForwardScanDirection);
	if (storeScan->tuple == NULL)
		return false;

	PersistentStore_DeformTuple(
							storeScan->storeData,
							storeScan->persistentRel->rd_att,
							storeScan->tuple,
							values);
	PersistentStore_ExtractOurTupleData(
								storeScan->storeData,
								values,
								persistentSerialNum,
								&previousFreeTid);

	*persistentTid = storeScan->tuple->t_self;

	return true;
}

HeapTuple PersistentStore_GetScanTupleCopy(
	PersistentStoreScan			*storeScan)
{
	return heaptuple_copy_to(storeScan->tuple, NULL, NULL);
}

void PersistentStore_EndScan(
	PersistentStoreScan			*storeScan)
{
	heap_endscan(storeScan->scan);

	(*storeScan->storeData->closeRel)(storeScan->persistentRel);
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

int64 PersistentStore_MyHighestSerialNum(
	PersistentStoreData 	*storeData)
{
	return storeData->myHighestSerialNum;
}

int64 PersistentStore_CurrentMaxSerialNum(
	PersistentStoreSharedData 	*storeSharedData)
{
	return storeSharedData->maxInUseSerialNum;
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
		
	frozen_heap_inplace_update(persistentRel, persistentTuple);

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

void PersistentStore_ReplaceTuple(
	PersistentStoreData 		*storeData,

	PersistentStoreSharedData 	*storeSharedData,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	HeapTuple				tuple,

	Datum					*newValues,
	
	bool					*replaces,

	bool					flushToXLog)
				/* When true, the XLOG record for this change will be flushed to disk. */

{
	Relation	persistentRel;
	bool 		*nulls;
	HeapTuple	replacementTuple = NULL;
	XLogRecPtr 	xlogUpdateEndLoc;
	
	persistentRel = (*storeData->openRel)();

	/*
	 * In order to keep the tuples the exact same size to enable direct reuse of
	 * free tuples, we do not use NULLs.
	 */
	nulls = (bool*)palloc0(storeData->numAttributes * sizeof(bool));
		
	/*
	 * Modify the tuple.
	 */
	replacementTuple = heap_modify_tuple(tuple, persistentRel->rd_att, 
										 newValues, nulls, replaces);

	replacementTuple->t_self = *persistentTid;
		
	frozen_heap_inplace_update(persistentRel, replacementTuple);

	/*
	 * Return the XLOG location of the UPDATE tuple's XLOG record.
	 */
	xlogUpdateEndLoc = XLogLastInsertEndLoc();

	heap_freetuple(replacementTuple);

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

	if (PersistentStore_IsZeroTid(readTid))
		elog(ERROR, "TID for fetch persistent tuple is invalid (0,0) (token %d)",
			 storeData->token);

	persistentRel = (*storeData->openRel)();

	tuple.t_self = *readTid;
	
	if (!heap_fetch(persistentRel, SnapshotAny,
					&tuple, &buffer, false, NULL))
	{
		elog(ERROR, "Failed to fetch persistent tuple at %s (token %d)",
			 ItemPointerToString(&tuple.t_self),
			 storeData->token);
	}

	
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
	Datum			*values;
	HeapTuple		tupleCopy;

	int64					persistentSerialNum;
	ItemPointerData			previousFreeTid;

	MemSet(freeTid, 0, sizeof(ItemPointerData));

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentStore_GetFreeTuple: Enter: maximum free order number " INT64_FORMAT ", free TID %s",
			 storeSharedData->maxFreeOrderNum, 
			 ItemPointerToString2(&storeSharedData->freeTid));

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

	if (PersistentStore_IsZeroTid(&previousFreeTid))
		elog(ERROR, "Expected persistent store tuple at %s to be free (token %d)", 
			 ItemPointerToString(&storeSharedData->freeTid),
			 storeData->token);

	if (storeSharedData->maxFreeOrderNum == 1)
		Assert(ItemPointerCompare(&previousFreeTid, &storeSharedData->freeTid) == 0);

	if (persistentSerialNum != storeSharedData->maxFreeOrderNum)
		elog(ERROR, "Expected persistent store tuple at %s to have order number " INT64_FORMAT " (found " INT64_FORMAT ", token %d)", 
			 ItemPointerToString(&storeSharedData->freeTid),
			 storeSharedData->maxFreeOrderNum,
			 persistentSerialNum,
			 storeData->token);

	*freeTid = storeSharedData->freeTid;
	storeSharedData->maxFreeOrderNum--;
	storeSharedData->freeTid = previousFreeTid;

	pfree(values);

	heap_freetuple(tupleCopy);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "PersistentStore_GetFreeTuple: Exit: maximum free order number " INT64_FORMAT ", free TID %s (token %d)",
			 storeSharedData->maxFreeOrderNum, 
			 ItemPointerToString2(&storeSharedData->freeTid),
			 storeData->token);

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
	*persistentSerialNum = ++storeSharedData->maxInUseSerialNum;
	storeData->myHighestSerialNum = storeSharedData->maxInUseSerialNum;

	GlobalSequence_Set(
				storeData->gpGlobalSequence,
				*persistentSerialNum);
	
	// Overwrite with the new serial number value.
	values[storeData->attNumPersistentSerialNum - 1] = 
										Int64GetDatum(*persistentSerialNum);

	if (PersistentStore_GetFreeTuple(
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
		
	frozen_heap_inplace_update(persistentRel, persistentTuple);

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
