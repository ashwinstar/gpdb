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

#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/palloc.h"
#include "storage/fd.h"
#include "access/persistentfilesysobjname.h"
#include "access/persistentendxactrec.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbdoublylinked.h"
#include "storage/smgr.h"

#define PersistentFileSysObjSharedData_StaticInit {PersistentStoreSharedData_StaticInit}

typedef struct PersistentFileSysObjSharedData
{

	PersistentStoreSharedData	storeSharedData;

} PersistentFileSysObjSharedData;

#define PersistentFileSysObjData_StaticInit {0,PersistentStoreData_StaticInit,0,0,0}

typedef struct PersistentFileSysObjData
{
	PersistentFsObjType		fsObjType;

	PersistentStoreData		storeData;

	int 					attNumPersistentState;

	int 					attNumMirrorExistenceState;

	int 					attNumParentXid;

} PersistentFileSysObjData;

inline static bool PersistentFileSysObj_BeforePersistenceWork(void)
{
	if (IsBootstrapProcessingMode())
		return true;

	if (gp_before_persistence_work && !IsUnderPostmaster)
		return true;

	return false;
}


extern void PersistentFileSysObj_InitShared(
	PersistentFileSysObjSharedData 	*fileSysObjSharedData);

extern void PersistentFileSysObj_Init(
	PersistentFileSysObjData 			*fileSysObjData,

	PersistentFileSysObjSharedData 		*fileSysObjSharedData,

	PersistentFsObjType					fsObjType,

	PersistentStoreScanTupleCallback	scanTupleCallback);

extern void PersistentFileSysObj_GetDataPtrs(
	PersistentFsObjType				fsObjType,
	
	PersistentFileSysObjData		**fileSysObjData,
	
	PersistentFileSysObjSharedData	**fileSysObjSharedData);

extern void PersistentFileSysObj_InitScan(void);

extern void PersistentFileSysObj_Scan(
	PersistentFsObjType			fsObjType,

	PersistentStoreScanTupleCallback	scanTupleCallback);

extern void PersistentFileSysObj_FlushXLog(void);

extern int64 PersistentFileSysObj_MyHighestSerialNum(
	PersistentFsObjType 	fsObjType);
extern int64 PersistentFileSysObj_CurrentMaxSerialNum(
	PersistentFsObjType 	fsObjType);

extern void PersistentFileSysObj_UpdateTuple(
	PersistentFsObjType		fsObjType,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	Datum 					*values,

	bool					flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */

extern void PersistentFileSysObj_ReplaceTuple(
	PersistentFsObjType 	fsObjType,

	ItemPointer 			persistentTid,
				/* TID of the stored tuple. */

	HeapTuple				tuple,

	Datum					*newValues,

	bool					*replaces,

	bool					flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */

extern void PersistentFileSysObj_ReadTuple(
	PersistentFsObjType 		fsObjType,
					
	ItemPointer					readTid,

	Datum						*values,
	
	HeapTuple					*tupleCopy);

extern void PersistentFileSysObj_AddTuple(
	PersistentFsObjType			fsObjType,

	Datum						*values,

	bool						flushToXLog,
				/* When true, the XLOG record for this change will be flushed to disk. */

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	int64						*persistentSerialNum);

extern void PersistentFileSysObj_FreeTuple(
	PersistentFileSysObjData		*fileSysObjData,

	PersistentFileSysObjSharedData	*fileSysObjSharedData,

	PersistentFsObjType 		fsObjType,

	ItemPointer 				persistentTid,
				/* TID of the stored tuple. */

	bool						flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */

extern void PersistentFileSysObj_StateChange(
	PersistentFileSysObjName	*fsObjName,

	ItemPointer 				persistentTid,

	int64						persistentSerialNum,
	
	PersistentFileSysState		nextState,

	bool						flushToXLog,

	PersistentFileSysState		*oldState);

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

extern void PersistentFileSysObj_DropObject(
	PersistentFileSysObjName	*fsObjName,

	PersistentFileSysRelStorageMgr relStorageMgr,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	ItemPointer 				persistentTid,

	int64						persistentSerialNum,

	bool 						ignoreNonExistence);

extern void PersistentFileSysObj_EndXactDrop(
	PersistentFileSysObjName	*fsObjName,

	PersistentFileSysRelStorageMgr relStorageMgr,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	ItemPointer 				persistentTid,

	int64						persistentSerialNum,

	bool						abortingCreate);

extern void PersistentFileSysObj_EndXactAction(
	PersistentEndXactRecObjects 	*persistentObjects,
	
	bool							isCommit);

extern void PersistentFileSysObj_ChangeMirrorExistenceState(
	PersistentFsObjType 			fsObjType,

	ItemPointer 					persistentTid,

	int64							persistentSerialNum,

	MirroredObjectExistenceState	mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	bool							flushToXLog);

extern void PersistentFileSysObj_MarkBufPoolRelationPhysicallyTruncated(
	PersistentFileSysObjName 	*fsObjName,

	ItemPointer					persistentTid,

	int64						persistentSerialNum,
	
	bool						flushToXLog);

extern void PersistentFileSysObj_UpdateAppendOnlyMirrorResyncEofs(
	RelFileNode					*relFileNode,

	int32						segmentFileNum,

	ItemPointer 				persistentTid,

	int64						persistentSerialNum,

	int64						mirrorLossEof,

	int64						mirrorNewEof,

	bool						recovery,
	
	bool						flushToXLog);

bool PersistentFileSysObj_CheckForAppendOnlyCatchup(
	RelFileNode					*relFileNode,

	int32						segmentFileNum,

	ItemPointer 				persistentTid,

	int64						persistentSerialNum);

extern void PersistentFileSysObj_RequestResynchronizeTransition(void);

extern void PersistentFileSysObj_MarkWholeMirrorFullCopy(void);

extern void PersistentFileSysObj_MarkWholeMirrorScanIncremental(void);

extern void PersistentFileSysObj_MarkAppendOnlyCatchup(void);

extern void PersistentFileSysObj_MarkSpecialScanIncremental(void);

extern void PersistentFileSysObj_MarkPageIncrementalFromChangeLog(void);

extern void PersistentFileSysObj_MirrorReCreate(void);

extern void PersistentFileSysObj_MarkMirrorReCreated(void);

extern void PersistentFileSysObj_MirrorReDrop(void);

typedef struct ResynchronizeScanToken
{
	bool				beginScan;

	PersistentStoreScan storeScan;

	bool				done;
} ResynchronizeScanToken;

inline static void ResynchronizeScanToken_Init(
	ResynchronizeScanToken		*token)
{
	MemSet(token, 0, sizeof(ResynchronizeScanToken));
	token->beginScan = false;
	token->done = false;
}

extern bool PersistentFileSysObj_ResynchronizeScan(
	ResynchronizeScanToken			*resynchronizeScanToken,

	RelFileNode						*relFileNode,

	int32							*segmentFileNum,

	PersistentFileSysRelStorageMgr 	*relStorageMgr,

	MirroredRelDataSynchronizationState *mirrorDataSynchronizationState,

	XLogRecPtr						*mirrorBufpoolResyncCkptLoc,

	BlockNumber 					*mirrorBufpoolResyncCkptBlockNum,

	int64							*mirrorAppendOnlyLossEof,

	int64							*mirrorAppendOnlyNewEof,

	ItemPointer						persistentTid,

	int64							*persistentSerialNum);

extern void PersistentFileSysObj_ResynchronizeAbandonScan(
	ResynchronizeScanToken			*resynchronizeScanToken);

/*
 * Refetch the resynchronize relation information while under RELATION_RESYNCHRONIZE lock
 * based on its persistent TID and serial number.
 */
extern bool PersistentFileSysObj_ResynchronizeRefetch(
	RelFileNode						*relFileNode,

	int32							*segmentFileNum,

	ItemPointer						persistentTid,

	int64							persistentSerialNum,

	PersistentFileSysRelStorageMgr 	*relStorageMgr,

	MirroredRelDataSynchronizationState *mirrorDataSynchronizationState,

	XLogRecPtr						*mirrorBufpoolResyncCkptLoc,

	BlockNumber 					*mirrorBufpoolResyncCkptBlockNum,

	int64							*mirrorAppendOnlyLossEof,

	int64							*mirrorAppendOnlyNewEof);

extern void PersistentFileSysObj_ResynchronizeBufferPoolCkpt(
	ItemPointer 					persistentTid,

	int64							persistentSerialNum,

	XLogRecPtr						mirrorBufpoolResyncCkptLoc,

	BlockNumber 					mirrorBufpoolResyncCkptBlockNum,

	bool							flushToXLog);

extern void PersistentFileSysObj_ResynchronizeRelationComplete(
	ItemPointer 					persistentTid,

	int64							persistentSerialNum,

	int64						mirrorLossEof,

	bool							flushToXLog);

extern bool PersistentFileSysObj_ScanForRelation(
	RelFileNode 		*relFileNode,
				/* The tablespace, database, and relation OIDs for the create. */

	int32				segmentFileNum,

	ItemPointer 		persistentTid,
				/* Resulting TID of the gp_persistent_rel_files tuple for the relation. */

	int64				*persistentSerialNum);
				/* Resulting serial number for the relation.  Distinquishes the uses of the tuple. */

extern Size PersistentFileSysObj_ShmemSize(void);

extern void PersistentFileSysObj_ShmemInit(void);

extern void PersistentFileSysObj_AddMirror(PersistentFileSysObjName *fsObjName,
										   ItemPointer persistentTid,
										   int64 persistentSerialNum,
										   int16 pridbid,
										   int16 mirdbid,
										   void *arg,
										   bool flushToXLog);

extern void PersistentFileSysObj_RemoveSegment(
								   PersistentFileSysObjName *fsObjName,
								   ItemPointer persistentTid,
								   int64 persistentSerialNum,
								   int16 dbid,
								   bool ismirror,
								   bool flushToXLog);

extern void PersistentFileSysObj_ActivateStandby(
								   PersistentFileSysObjName *fsObjName,
								   ItemPointer persistentTid,
								   int64 persistentSerialNum,
								   int16 oldmaster,
								   int16 newmaster,
								   bool flushToXLog);
#endif   /* CDBPERSISTENTFILESYSOBJ_H */
