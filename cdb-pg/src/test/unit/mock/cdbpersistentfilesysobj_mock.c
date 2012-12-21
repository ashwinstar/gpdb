/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbpersistentfilesysobj.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"
#include "access/persistentfilesysobjname.h"
#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "catalog/gp_segment_config.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "cdb/cdbmirroredbufferpool.h"
#include "cdb/cdbmirroredappendonly.h"
#include "cdb/cdbpersistentrecovery.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbglobalsequence.h"
#include "storage/itemptr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"

void 
PersistentFileSysObj_InitShared(PersistentFileSysObjSharedData * fileSysObjSharedData)
{

    check_expected(fileSysObjSharedData);
    optional_assignment(fileSysObjSharedData);
    mock();

}

void 
PersistentFileSysObj_Init(PersistentFileSysObjData * fileSysObjData, PersistentFileSysObjSharedData * fileSysObjSharedData, PersistentFsObjType fsObjType, PersistentStoreScanTupleCallback scanTupleCallback)
{

    check_expected(fileSysObjData);
    check_expected(fileSysObjSharedData);
    check_expected(fsObjType);
    check_expected(scanTupleCallback);
    optional_assignment(fileSysObjData);
    optional_assignment(fileSysObjSharedData);
    mock();

}

void 
PersistentFileSysObj_Reset(void)
{



    mock();

}

void 
PersistentFileSysObj_GetDataPtrs(PersistentFsObjType fsObjType, PersistentFileSysObjData ** fileSysObjData, PersistentFileSysObjSharedData ** fileSysObjSharedData)
{

    check_expected(fsObjType);
    check_expected(fileSysObjData);
    check_expected(fileSysObjSharedData);
    optional_assignment(fileSysObjData);
    optional_assignment(fileSysObjSharedData);
    mock();

}

void 
PersistentFileSysObj_BuildInitScan(void)
{



    mock();

}

void 
PersistentFileSysObj_StartupInitScan(void)
{



    mock();

}

void 
PersistentFileSysObj_VerifyInitScan(void)
{



    mock();

}

void 
PersistentFileSysObj_Scan(PersistentFsObjType fsObjType, PersistentStoreScanTupleCallback scanTupleCallback)
{

    check_expected(fsObjType);
    check_expected(scanTupleCallback);

    mock();

}

void 
PersistentFileSysObj_FlushXLog(void)
{



    mock();

}

int64 
PersistentFileSysObj_MyHighestSerialNum(PersistentFsObjType fsObjType)
{

    check_expected(fsObjType);

    return (int64) mock();

}

int64 
PersistentFileSysObj_CurrentMaxSerialNum(PersistentFsObjType fsObjType)
{

    check_expected(fsObjType);

    return (int64) mock();

}

PersistentTidIsKnownResult 
PersistentFileSysObj_TidIsKnown(PersistentFsObjType fsObjType, ItemPointer persistentTid, ItemPointer maxTid)
{

    check_expected(fsObjType);
    check_expected(persistentTid);
    check_expected(maxTid);

    return (PersistentTidIsKnownResult) mock();

}

void 
PersistentFileSysObj_UpdateTuple(PersistentFsObjType fsObjType, ItemPointer persistentTid, Datum * values, bool flushToXLog)
{

    check_expected(fsObjType);
    check_expected(persistentTid);
    check_expected(values);
    check_expected(flushToXLog);
    optional_assignment(values);
    mock();

}

void 
PersistentFileSysObj_ReplaceTuple(PersistentFsObjType fsObjType, ItemPointer persistentTid, HeapTuple tuple, Datum * newValues, bool * replaces, bool flushToXLog)
{

    check_expected(fsObjType);
    check_expected(persistentTid);
    check_expected(tuple);
    check_expected(newValues);
    check_expected(replaces);
    check_expected(flushToXLog);
    optional_assignment(newValues);
    optional_assignment(replaces);
    mock();

}

void 
PersistentFileSysObj_ReadTuple(PersistentFsObjType fsObjType, ItemPointer readTid, Datum * values, HeapTuple * tupleCopy)
{

    check_expected(fsObjType);
    check_expected(readTid);
    check_expected(values);
    check_expected(tupleCopy);
    optional_assignment(values);
    optional_assignment(tupleCopy);
    mock();

}

void 
PersistentFileSysObj_AddTuple(PersistentFsObjType fsObjType, Datum * values, bool flushToXLog, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(fsObjType);
    check_expected(values);
    check_expected(flushToXLog);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(values);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
PersistentFileSysObj_FreeTuple(PersistentFileSysObjData * fileSysObjData, PersistentFileSysObjSharedData * fileSysObjSharedData, PersistentFsObjType fsObjType, ItemPointer persistentTid, bool flushToXLog)
{

    check_expected(fileSysObjData);
    check_expected(fileSysObjSharedData);
    check_expected(fsObjType);
    check_expected(persistentTid);
    check_expected(flushToXLog);
    optional_assignment(fileSysObjData);
    optional_assignment(fileSysObjSharedData);
    mock();

}

char           *
PersistentFileSysObjStateChangeResult_Name(PersistentFileSysObjStateChangeResult result)
{

    check_expected(result);

    return (char *) mock();

}

PersistentFileSysObjStateChangeResult 
PersistentFileSysObj_StateChange(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, PersistentFileSysState nextState, bool retryPossible, bool flushToXLog, PersistentFileSysState * oldState, PersistentFileSysObjVerifiedActionCallback verifiedActionCallback)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(nextState);
    check_expected(retryPossible);
    check_expected(flushToXLog);
    check_expected(oldState);
    check_expected(verifiedActionCallback);
    optional_assignment(fsObjName);
    optional_assignment(oldState);
    return (PersistentFileSysObjStateChangeResult) mock();

}

void 
PersistentFileSysObj_RepairDelete(PersistentFsObjType fsObjType, ItemPointer persistentTid)
{

    check_expected(fsObjType);
    check_expected(persistentTid);

    mock();

}

void 
PersistentFileSysObj_Created(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(fsObjName);
    mock();

}

PersistentFileSysObjStateChangeResult 
PersistentFileSysObj_MarkAbortingCreate(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(fsObjName);
    return (PersistentFileSysObjStateChangeResult) mock();

}

PersistentFileSysObjStateChangeResult 
PersistentFileSysObj_MarkDropPending(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(fsObjName);
    return (PersistentFileSysObjStateChangeResult) mock();

}

void 
PersistentFileSysObj_DropObject(PersistentFileSysObjName * fsObjName, PersistentFileSysRelStorageMgr relStorageMgr, char *relationName, ItemPointer persistentTid, int64 persistentSerialNum, bool ignoreNonExistence, bool debugPrint, int debugPrintLevel)
{

    check_expected(fsObjName);
    check_expected(relStorageMgr);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(ignoreNonExistence);
    check_expected(debugPrint);
    check_expected(debugPrintLevel);
    optional_assignment(fsObjName);
    optional_assignment(relationName);
    mock();

}

void 
PersistentFileSysObj_EndXactDrop(PersistentFileSysObjName * fsObjName, PersistentFileSysRelStorageMgr relStorageMgr, char *relationName, ItemPointer persistentTid, int64 persistentSerialNum, bool ignoreNonExistence)
{

    check_expected(fsObjName);
    check_expected(relStorageMgr);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(ignoreNonExistence);
    optional_assignment(fsObjName);
    optional_assignment(relationName);
    mock();

}

void 
PersistentFileSysObj_PreparedEndXactAction(TransactionId preparedXid, const char *gid, PersistentEndXactRecObjects * persistentObjects, bool isCommit, int prepareAppendOnlyIntentCount)
{

    check_expected(preparedXid);
    check_expected(gid);
    check_expected(persistentObjects);
    check_expected(isCommit);
    check_expected(prepareAppendOnlyIntentCount);
    optional_assignment(persistentObjects);
    mock();

}

void 
PersistentFileSysObj_ChangeMirrorState(PersistentFsObjType fsObjType, ItemPointer persistentTid, int64 persistentSerialNum, MirroredObjectExistenceState mirrorExistenceState, MirroredRelDataSynchronizationState relDataSynchronizationState, bool flushToXLog)
{

    check_expected(fsObjType);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(mirrorExistenceState);
    check_expected(relDataSynchronizationState);
    check_expected(flushToXLog);

    mock();

}

void 
PersistentFileSysObj_MarkBufPoolRelationForScanIncrementalResync(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, bool flushToXLog)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(fsObjName);
    mock();

}

void 
PersistentFileSysObj_UpdateAppendOnlyMirrorResyncEofs(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, bool mirrorCatchupRequired, int64 mirrorNewEof, bool recovery, bool flushToXLog)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(mirrorCatchupRequired);
    check_expected(mirrorNewEof);
    check_expected(recovery);
    check_expected(flushToXLog);
    optional_assignment(relFileNode);
    mock();

}

void 
PersistentFileSysObj_UpdateRelationBufpoolKind(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, PersistentFileSysRelBufpoolKind relBufpoolKind)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(relBufpoolKind);
    optional_assignment(relFileNode);
    mock();

}

bool 
PersistentFileSysObj_CanAppendOnlyCatchupDuringResync(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, int64 * eof)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(eof);
    optional_assignment(relFileNode);
    optional_assignment(eof);
    return (bool) mock();

}

void 
PersistentFileSysObj_GetAppendOnlyCatchupMirrorStartEof(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, int64 * startEof)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(startEof);
    optional_assignment(relFileNode);
    optional_assignment(startEof);
    mock();

}

void 
PersistentFileSysObj_RequestResynchronizeTransition(void)
{



    mock();

}

void 
PersistentFileSysObj_MarkWholeMirrorFullCopy(void)
{



    mock();

}

void 
PersistentFileSysObj_MarkWholeMirrorScanIncremental(void)
{



    mock();

}

void 
PersistentFileSysObj_MarkAppendOnlyCatchup(void)
{



    mock();

}

void 
PersistentFileSysObj_MarkSpecialScanIncremental(void)
{



    mock();

}

void 
PersistentFileSysObj_MarkPageIncrementalFromChangeLog(void)
{



    mock();

}

void 
PersistentFileSysObj_MirrorReCreate(void)
{



    mock();

}

void 
PersistentFileSysObj_MarkMirrorReCreated(void)
{



    mock();

}

void 
PersistentFileSysObj_MirrorReDrop(void)
{



    mock();

}

void 
PersistentFileSysObj_MirrorAdd(void)
{



    mock();

}

bool 
PersistentFileSysObj_ResynchronizeScan(ResynchronizeScanToken * resynchronizeScanToken, RelFileNode * relFileNode, int32 * segmentFileNum, PersistentFileSysRelStorageMgr * relStorageMgr, MirroredRelDataSynchronizationState * mirrorDataSynchronizationState, int64 * mirrorBufpoolResyncChangedPageCount, XLogRecPtr * mirrorBufpoolResyncCkptLoc, BlockNumber * mirrorBufpoolResyncCkptBlockNum, int64 * mirrorAppendOnlyLossEof, int64 * mirrorAppendOnlyNewEof, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(resynchronizeScanToken);
    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relStorageMgr);
    check_expected(mirrorDataSynchronizationState);
    check_expected(mirrorBufpoolResyncChangedPageCount);
    check_expected(mirrorBufpoolResyncCkptLoc);
    check_expected(mirrorBufpoolResyncCkptBlockNum);
    check_expected(mirrorAppendOnlyLossEof);
    check_expected(mirrorAppendOnlyNewEof);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(resynchronizeScanToken);
    optional_assignment(relFileNode);
    optional_assignment(segmentFileNum);
    optional_assignment(relStorageMgr);
    optional_assignment(mirrorDataSynchronizationState);
    optional_assignment(mirrorBufpoolResyncChangedPageCount);
    optional_assignment(mirrorBufpoolResyncCkptLoc);
    optional_assignment(mirrorBufpoolResyncCkptBlockNum);
    optional_assignment(mirrorAppendOnlyLossEof);
    optional_assignment(mirrorAppendOnlyNewEof);
    optional_assignment(persistentSerialNum);
    return (bool) mock();

}

void 
PersistentFileSysObj_ResynchronizeAbandonScan(ResynchronizeScanToken * resynchronizeScanToken)
{

    check_expected(resynchronizeScanToken);
    optional_assignment(resynchronizeScanToken);
    mock();

}

bool 
PersistentFileSysObj_ResynchronizeRefetch(RelFileNode * relFileNode, int32 * segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, PersistentFileSysRelStorageMgr * relStorageMgr, MirroredRelDataSynchronizationState * mirrorDataSynchronizationState, XLogRecPtr * mirrorBufpoolResyncCkptLoc, BlockNumber * mirrorBufpoolResyncCkptBlockNum, int64 * mirrorAppendOnlyLossEof, int64 * mirrorAppendOnlyNewEof)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(relStorageMgr);
    check_expected(mirrorDataSynchronizationState);
    check_expected(mirrorBufpoolResyncCkptLoc);
    check_expected(mirrorBufpoolResyncCkptBlockNum);
    check_expected(mirrorAppendOnlyLossEof);
    check_expected(mirrorAppendOnlyNewEof);
    optional_assignment(relFileNode);
    optional_assignment(segmentFileNum);
    optional_assignment(relStorageMgr);
    optional_assignment(mirrorDataSynchronizationState);
    optional_assignment(mirrorBufpoolResyncCkptLoc);
    optional_assignment(mirrorBufpoolResyncCkptBlockNum);
    optional_assignment(mirrorAppendOnlyLossEof);
    optional_assignment(mirrorAppendOnlyNewEof);
    return (bool) mock();

}

void 
PersistentFileSysObj_ResynchronizeBufferPoolCkpt(ItemPointer persistentTid, int64 persistentSerialNum, XLogRecPtr mirrorBufpoolResyncCkptLoc, BlockNumber mirrorBufpoolResyncCkptBlockNum, bool flushToXLog)
{

    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(mirrorBufpoolResyncCkptLoc.xlogid);
    check_expected(mirrorBufpoolResyncCkptLoc.xrecoff);
    check_expected(mirrorBufpoolResyncCkptBlockNum);
    check_expected(flushToXLog);

    mock();

}

void 
PersistentFileSysObj_ResynchronizeRelationComplete(ItemPointer persistentTid, int64 persistentSerialNum, int64 mirrorLossEof, bool flushToXLog)
{

    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(mirrorLossEof);
    check_expected(flushToXLog);

    mock();

}

bool 
PersistentFileSysObj_OnlineVerifyScan(OnlineVerifyScanToken * onlineVerifyScanToken, RelFileNode * relFileNode, int32 * segmentFileNum, PersistentFileSysRelStorageMgr * relStorageMgr, MirroredRelDataSynchronizationState * mirrorDataSynchronizationState, PersistentFileSysRelBufpoolKind * relBufpoolKind, int64 * mirrorAppendOnlyLossEof, int64 * mirrorAppendOnlyNewEof, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(onlineVerifyScanToken);
    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relStorageMgr);
    check_expected(mirrorDataSynchronizationState);
    check_expected(relBufpoolKind);
    check_expected(mirrorAppendOnlyLossEof);
    check_expected(mirrorAppendOnlyNewEof);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(onlineVerifyScanToken);
    optional_assignment(relFileNode);
    optional_assignment(segmentFileNum);
    optional_assignment(relStorageMgr);
    optional_assignment(mirrorDataSynchronizationState);
    optional_assignment(relBufpoolKind);
    optional_assignment(mirrorAppendOnlyLossEof);
    optional_assignment(mirrorAppendOnlyNewEof);
    optional_assignment(persistentSerialNum);
    return (bool) mock();

}

void 
PersistentFileSysObj_OnlineVerifyAbandonScan(OnlineVerifyScanToken * onlineVerifyScanToken)
{

    check_expected(onlineVerifyScanToken);
    optional_assignment(onlineVerifyScanToken);
    mock();

}

bool 
PersistentFileSysObj_FilespaceScan(FilespaceScanToken * filespaceScanToken, Oid * filespaceOid, int16 * dbId1, char locationBlankPadded1[1024], int16 * dbId2, char locationBlankPadded2[1024], PersistentFileSysState * persistentState, MirroredObjectExistenceState * mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(filespaceScanToken);
    check_expected(filespaceOid);
    check_expected(dbId1);
    check_expected(locationBlankPadded1);
    check_expected(dbId2);
    check_expected(locationBlankPadded2);
    check_expected(persistentState);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(filespaceScanToken);
    optional_assignment(filespaceOid);
    optional_assignment(dbId1);
    optional_assignment(dbId2);
    optional_assignment(persistentState);
    optional_assignment(mirrorExistenceState);
    optional_assignment(persistentSerialNum);
    return (bool) mock();

}

void 
PersistentFileSysObj_FilespaceAbandonScan(FilespaceScanToken * filespaceScanToken)
{

    check_expected(filespaceScanToken);
    optional_assignment(filespaceScanToken);
    mock();

}

bool 
PersistentFileSysObj_ScanForRelation(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(relFileNode);
    optional_assignment(persistentSerialNum);
    return (bool) mock();

}

void 
PersistentFileSysObj_StartupIntegrityCheck(void)
{



    mock();

}

Size 
PersistentFileSysObj_ShmemSize(void)
{



    return (Size) mock();

}

void 
PersistentFileSysObj_ShmemInit(void)
{



    mock();

}

void 
PersistentFileSysObj_AddMirror(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, int16 pridbid, int16 mirdbid, void *arg, bool set_mirror_existence, bool flushToXLog)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(pridbid);
    check_expected(mirdbid);
    check_expected(arg);
    check_expected(set_mirror_existence);
    check_expected(flushToXLog);
    optional_assignment(fsObjName);
    optional_assignment(arg);
    mock();

}

void 
PersistentFileSysObj_RemoveSegment(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, int16 dbid, bool ismirror, bool flushToXLog)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(dbid);
    check_expected(ismirror);
    check_expected(flushToXLog);
    optional_assignment(fsObjName);
    mock();

}

void 
PersistentFileSysObj_ActivateStandby(PersistentFileSysObjName * fsObjName, ItemPointer persistentTid, int64 persistentSerialNum, int16 oldmaster, int16 newmaster, bool flushToXLog)
{

    check_expected(fsObjName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(oldmaster);
    check_expected(newmaster);
    check_expected(flushToXLog);
    optional_assignment(fsObjName);
    mock();

}

void 
PersistentFileSysObj_DoGlobalSequenceScan(void)
{



    mock();

}
