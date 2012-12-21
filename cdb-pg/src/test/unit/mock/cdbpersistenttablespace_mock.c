/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbpersistenttablespace.h
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
#include "access/xlogmm.h"
#include "catalog/catalog.h"
#include "catalog/gp_persistent.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_filespace.h"
#include "cdb/cdbsharedoidsearch.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbpersistenttablespace.h"
#include "postmaster/postmaster.h"
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
PersistentTablespace_Reset(void)
{



    mock();

}

bool 
PersistentTablespace_Check(Oid tablespace)
{

    check_expected(tablespace);

    return (bool) mock();

}

PersistentFileSysState 
PersistentTablespace_GetState(Oid tablespaceOid)
{

    check_expected(tablespaceOid);

    return (PersistentFileSysState) mock();

}

void 
PersistentTablespace_LookupTidAndSerialNum(Oid tablespaceOid, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(tablespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(persistentSerialNum);
    mock();

}

PersistentTablespaceGetFilespaces 
PersistentTablespace_TryGetPrimaryAndMirrorFilespaces(Oid tablespaceOid, char **primaryFilespaceLocation, char **mirrorFilespaceLocation, Oid * filespaceOid)
{

    check_expected(tablespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    check_expected(filespaceOid);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    optional_assignment(filespaceOid);
    return (PersistentTablespaceGetFilespaces) mock();

}

void 
PersistentTablespace_GetPrimaryAndMirrorFilespaces(Oid tablespaceOid, char **primaryFilespaceLocation, char **mirrorFilespaceLocation)
{

    check_expected(tablespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    mock();

}

void 
PersistentTablespace_MarkCreatePending(Oid filespaceOid, Oid tablespaceOid, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(filespaceOid);
    check_expected(tablespaceOid);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
PersistentTablespace_Created(Oid tablespaceOid, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(tablespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);

    mock();

}

PersistentFileSysObjStateChangeResult 
PersistentTablespace_MarkDropPending(Oid tablespaceOid, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(tablespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);

    return (PersistentFileSysObjStateChangeResult) mock();

}

PersistentFileSysObjStateChangeResult 
PersistentTablespace_MarkAbortingCreate(Oid tablespaceOid, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(tablespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);

    return (PersistentFileSysObjStateChangeResult) mock();

}

void 
PersistentTablespace_Dropped(Oid tablespaceOid, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(tablespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);

    mock();

}

void 
PersistentTablespace_AddCreated(Oid filespaceOid, Oid tablespaceOid, MirroredObjectExistenceState mirrorExistenceState, bool flushToXLog)
{

    check_expected(filespaceOid);
    check_expected(tablespaceOid);
    check_expected(mirrorExistenceState);
    check_expected(flushToXLog);

    mock();

}

Size 
PersistentTablespace_ShmemSize(void)
{



    return (Size) mock();

}

void 
PersistentTablespace_ShmemInit(void)
{



    mock();

}

void 
PersistentTablespace_AddMirrorAll(int16 pridbid, int16 mirdbid)
{

    check_expected(pridbid);
    check_expected(mirdbid);

    mock();

}

void 
PersistentTablespace_RemoveSegment(int16 dbid, bool ismirror)
{

    check_expected(dbid);
    check_expected(ismirror);

    mock();

}

void 
PersistentTablespace_ActivateStandby(int16 oldmaster, int16 newmaster)
{

    check_expected(oldmaster);
    check_expected(newmaster);

    mock();

}

void 
xlog_persistent_tablespace_create(Oid filespaceoid, Oid tablespaceoid)
{

    check_expected(filespaceoid);
    check_expected(tablespaceoid);

    mock();

}

Oid 
PersistentTablespace_GetFileSpaceOid(Oid tablespaceOid)
{

    check_expected(tablespaceOid);

    return (Oid) mock();

}

void 
get_tablespace_data(tspc_agg_state ** tas, char *caller)
{

    check_expected(tas);
    check_expected(caller);
    optional_assignment(tas);
    optional_assignment(caller);
    mock();

}
