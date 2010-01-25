/*-------------------------------------------------------------------------
 *
 * cdbpersistentfilesysobj.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPERSISTENTFILESYSOBJ_H
#define CDBPERSISTENTFILESYSOBJ_H

#include "utils/palloc.h"
#include "storage/fd.h"
#include "access/persistentfilesysobjname.h"
#include "access/persistentendxactrec.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbpersistentdatabase.h"

#define PersistentFileSysObjSharedData_StaticInit {PersistentStoreSharedData_StaticInit}

typedef struct PersistentFileSysObjSharedData
{

	PersistentStoreSharedData	storeSharedData;

} PersistentFileSysObjSharedData;

#define PersistentFileSysObjData_StaticInit {0,PersistentStoreData_StaticInit}

typedef struct PersistentFileSysObjData
{
	PersistentFsObjType		fsObjType;

	PersistentStoreData		storeData;

} PersistentFileSysObjData;

extern void PersistentFileSysObj_InitShared(
	PersistentFileSysObjSharedData 	*fileSysObjSharedData);

extern void PersistentFileSysObj_Init(
	PersistentFileSysObjData 			*fileSysObjData,
	PersistentFsObjType					fsObjType,
	PersistentStoreScanTupleCallback	scanTupleCallback);

extern void PersistentFileSysObj_InitScanUnderLock(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData);

extern void PersistentFileSysObj_InitScan(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData);

extern void PersistentFileSysObj_Scan(
	PersistentFileSysObjData		*fileSysObjData,
	
	PersistentFileSysObjSharedData	*fileSysObjSharedData,

	PersistentStoreScanTupleCallback	scanTupleCallback);

extern void PersistentFileSysObj_FlushXLog(void);

extern void PersistentFileSysObj_UpdateTuple(
		PersistentFileSysObjData 		*fileSysObjData,
	
		PersistentFileSysObjSharedData	*fileSysObjSharedData,
	
		ItemPointer 			persistentTid,
					/* TID of the stored tuple. */
	
		Datum 					*values,

		bool					flushToXLog);
					/* When true, the XLOG record for this change will be flushed to disk. */

extern void PersistentFileSysObj_ReadTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	ItemPointer					readTid,

	Datum						*values,
	
	HeapTuple					*tupleCopy);

extern void PersistentFileSysObj_AddTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	Datum						*values,

	bool						flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	int64					*persistentSerialNum);

extern void PersistentFileSysObj_FreeTuple(
	PersistentFileSysObjData 		*fileSysObjData,

	PersistentFileSysObjSharedData 	*fileSysObjSharedData,

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	Datum 						*freeValues,
	
	bool						flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */


extern void PersistentFileSysObj_VerifyExpected(
	PersistentFileSysObjData		*fileSysObjData,
	
	PersistentFileSysObjSharedData	*fileSysObjSharedData,
	
	PersistentFileSysObjName	*fsObjName,

	ItemPointer 			persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	int64					serialNum,

	PersistentFileSysState	expectedState1,

	PersistentFileSysState	expectedState2,

	PersistentFileSysState	nextState,

	PersistentFileSysState	*actualState);

extern void PersistentFileSysObj_Created(
	PersistentFileSysObjName	*fsObjName,

	ItemPointer 			persistentTid,

	int64					persistentSerialNum);

extern void PersistentFileSysObj_MarkAbortingCreate(
	PersistentFileSysObjName	*fsObjName,

	ItemPointer 			persistentTid,

	int64					persistentSerialNum);

extern void PersistentFileSysObj_MarkDropPending(
	PersistentFileSysObjName	*fsObjName,

	ItemPointer 			persistentTid,

	int64					persistentSerialNum);

extern void PersistentFileSysObj_Drop(
	PersistentFileSysObjName	*fsObjName,

	ItemPointer 			persistentTid,

	int64					persistentSerialNum,
	
	bool					isRedo);

extern void PersistentFileSysObj_EndXactAction(
	PersistentEndXactRecObjects 	*persistentObjects,
	
	bool							isCommit);

#endif   /* CDBPERSISTENTFILESYSOBJ_H */
