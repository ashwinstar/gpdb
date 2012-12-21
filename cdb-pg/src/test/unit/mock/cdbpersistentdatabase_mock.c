/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbpersistentdatabase.h
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
#include "cdb/cdbsharedoidsearch.h"
#include "access/persistentfilesysobjname.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistenttablespace.h"
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

void 
PersistentDatabase_DirIterateInit(void)
{



    mock();

}

bool 
PersistentDatabase_DirIterateNext(DbDirNode * dbDirNode, PersistentFileSysState * state, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(dbDirNode);
    check_expected(state);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(dbDirNode);
    optional_assignment(state);
    optional_assignment(persistentSerialNum);
    return (bool) mock();

}

void 
PersistentDatabase_DirIterateClose(void)
{



    mock();

}

bool 
PersistentDatabase_DbDirExistsUnderLock(DbDirNode * dbDirNode)
{

    check_expected(dbDirNode);
    optional_assignment(dbDirNode);
    return (bool) mock();

}

void 
PersistentDatabase_Reset(void)
{



    mock();

}

void 
PersistentDatabase_MarkCreatePending(DbDirNode * dbDirNode, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(dbDirNode);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(dbDirNode);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
PersistentDatabase_AddCreated(DbDirNode * dbDirNode, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, bool flushToXLog)
{

    check_expected(dbDirNode);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(flushToXLog);
    optional_assignment(dbDirNode);
    mock();

}

void 
PersistentDatabase_Created(DbDirNode * dbDirNode, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(dbDirNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(dbDirNode);
    mock();

}

PersistentFileSysObjStateChangeResult 
PersistentDatabase_MarkDropPending(DbDirNode * dbDirNode, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(dbDirNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(dbDirNode);
    return (PersistentFileSysObjStateChangeResult) mock();

}

PersistentFileSysObjStateChangeResult 
PersistentDatabase_MarkAbortingCreate(DbDirNode * dbDirNode, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(dbDirNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);
    optional_assignment(dbDirNode);
    return (PersistentFileSysObjStateChangeResult) mock();

}

void 
PersistentDatabase_Dropped(DbDirNode * dbDirNode, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(dbDirNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(dbDirNode);
    mock();

}

bool 
PersistentDatabase_DirIsCreated(DbDirNode * dbDirNode)
{

    check_expected(dbDirNode);
    optional_assignment(dbDirNode);
    return (bool) mock();

}

void 
PersistentDatabase_MarkJustInTimeCreatePending(DbDirNode * dbDirNode, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(dbDirNode);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(dbDirNode);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
PersistentDatabase_JustInTimeCreated(DbDirNode * dbDirNode, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(dbDirNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(dbDirNode);
    mock();

}

void 
PersistentDatabase_AbandonJustInTimeCreatePending(DbDirNode * dbDirNode, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(dbDirNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(dbDirNode);
    mock();

}

Size 
PersistentDatabase_ShmemSize(void)
{



    return (Size) mock();

}

void 
PersistentDatabase_ShmemInit(void)
{



    mock();

}

void 
PersistentDatabase_AddMirrorAll(int16 pridbid, int16 mirdbid)
{

    check_expected(pridbid);
    check_expected(mirdbid);

    mock();

}

void 
PersistentDatabase_RemoveSegment(int16 dbid, bool ismirror)
{

    check_expected(dbid);
    check_expected(ismirror);

    mock();

}

void 
PersistentDatabase_ActivateStandby(int16 oldmaster, int16 newmaster)
{

    check_expected(oldmaster);
    check_expected(newmaster);

    mock();

}

void 
xlog_create_database(DbDirNode * db)
{

    check_expected(db);
    optional_assignment(db);
    mock();

}

void 
get_database_data(dbdir_agg_state ** das, char *caller)
{

    check_expected(das);
    check_expected(caller);
    optional_assignment(das);
    optional_assignment(caller);
    mock();

}
