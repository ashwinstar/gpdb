/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbpersistentrelation.h
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
#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "catalog/gp_fastsequence.h"
#include "cdb/cdbsharedoidsearch.h"
#include "access/persistentfilesysobjname.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistentrelation.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"
#include "storage/shmem.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

int64 
PersistentRelation_MyHighestSerialNum(void)
{



    return (int64) mock();

}

int64 
PersistentRelation_CurrentMaxSerialNum(void)
{



    return (int64) mock();

}

void 
PersistentRelation_CheckTablespace(Oid tablespace, int32 * useCount, RelFileNode * exampleRelFileNode)
{

    check_expected(tablespace);
    check_expected(useCount);
    check_expected(exampleRelFileNode);
    optional_assignment(useCount);
    optional_assignment(exampleRelFileNode);
    mock();

}

void 
PersistentRelation_FlushXLog(void)
{



    mock();

}

void 
PersistentRelation_Reset(void)
{



    mock();

}

void 
PersistentRelation_AddCreatePending(RelFileNode * relFileNode, int32 segmentFileNum, PersistentFileSysRelStorageMgr relStorageMgr, PersistentFileSysRelBufpoolKind relBufpoolKind, bool bufferPoolBulkLoad, MirroredObjectExistenceState mirrorExistenceState, MirroredRelDataSynchronizationState relDataSynchronizationState, char *relationName, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relStorageMgr);
    check_expected(relBufpoolKind);
    check_expected(bufferPoolBulkLoad);
    check_expected(mirrorExistenceState);
    check_expected(relDataSynchronizationState);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
PersistentRelation_AddCreated(RelFileNode * relFileNode, int32 segmentFileNum, PersistentFileSysRelStorageMgr relStorageMgr, PersistentFileSysRelBufpoolKind relBufpoolKind, MirroredObjectExistenceState mirrorExistenceState, MirroredRelDataSynchronizationState relDataSynchronizationState, int64 mirrorAppendOnlyLossEof, int64 mirrorAppendOnlyNewEof, char *relationName, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relStorageMgr);
    check_expected(relBufpoolKind);
    check_expected(mirrorExistenceState);
    check_expected(relDataSynchronizationState);
    check_expected(mirrorAppendOnlyLossEof);
    check_expected(mirrorAppendOnlyNewEof);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
PersistentRelation_FinishBufferPoolBulkLoad(RelFileNode * relFileNode, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(relFileNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(relFileNode);
    mock();

}

void 
PersistentRelation_Created(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(relFileNode);
    mock();

}

PersistentFileSysObjStateChangeResult 
PersistentRelation_MarkDropPending(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(relFileNode);
    return (PersistentFileSysObjStateChangeResult) mock();

}

PersistentFileSysObjStateChangeResult 
PersistentRelation_MarkAbortingCreate(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(relFileNode);
    return (PersistentFileSysObjStateChangeResult) mock();

}

void 
PersistentRelation_Dropped(RelFileNode * relFileNode, int32 segmentFileNum, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(relFileNode);
    mock();

}

void 
PersistentRelation_MarkBufPoolRelationForScanIncrementalResync(RelFileNode * relFileNode, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(relFileNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(relFileNode);
    mock();

}

Size 
PersistentRelation_ShmemSize(void)
{



    return (Size) mock();

}

void 
PersistentRelation_ShmemInit(void)
{



    mock();

}

void 
PersistentRelation_AddMirrorAll(int16 pridbid, int16 mirdbid)
{

    check_expected(pridbid);
    check_expected(mirdbid);

    mock();

}

void 
PersistentRelation_RemoveSegment(int16 dbid, bool ismirror)
{

    check_expected(dbid);
    check_expected(ismirror);

    mock();

}

void 
PersistentRelation_ActivateStandby(int16 oldmaster, int16 newmaster)
{

    check_expected(oldmaster);
    check_expected(newmaster);

    mock();

}
