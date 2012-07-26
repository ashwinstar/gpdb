/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbpersistentfilespace.h
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
#include "access/genam.h"
#include "access/heapam.h"
#include "access/persistentfilesysobjname.h"
#include "access/transam.h"
#include "access/xlogmm.h"
#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "catalog/gp_segment_config.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "postmaster/postmaster.h"
#include "postmaster/primary_mirror_mode.h"
#include "storage/ipc.h"
#include "storage/itemptr.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbsharedoidsearch.h"
#include "cdb/cdbdirectopen.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentfilespace.h"

void 
PersistentFilespace_Reset(void)
{



    mock();

}

bool 
PersistentFilespace_Check(Oid filespace)
{

    check_expected(filespace);

    return (bool) mock();

}

void 
PersistentFilespace_FileRepVerify(void)
{



    mock();

}

void 
PersistentFilespace_ConvertBlankPaddedLocation(char **filespaceLocation, char *locationBlankPadded, bool isPrimary)
{

    check_expected(filespaceLocation);
    check_expected(locationBlankPadded);
    check_expected(isPrimary);
    optional_assignment(filespaceLocation);
    optional_assignment(locationBlankPadded);
    mock();

}

void 
PersistentFilespace_LookupTidAndSerialNum(Oid filespaceOid, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(filespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(persistentSerialNum);
    mock();

}

bool 
PersistentFilespace_TryGetPrimaryAndMirrorUnderLock(Oid filespaceOid, char **primaryFilespaceLocation, char **mirrorFilespaceLocation)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    return (bool) mock();

}

void 
PersistentFilespace_GetPrimaryAndMirrorUnderLock(Oid filespaceOid, char **primaryFilespaceLocation, char **mirrorFilespaceLocation)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    mock();

}

bool 
PersistentFilespace_TryGetPrimaryAndMirror(Oid filespaceOid, char **primaryFilespaceLocation, char **mirrorFilespaceLocation)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    return (bool) mock();

}

void 
PersistentFilespace_GetPrimaryAndMirror(Oid filespaceOid, char **primaryFilespaceLocation, char **mirrorFilespaceLocation)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    mock();

}

void 
PersistentFilespace_MarkCreatePending(Oid filespaceOid, int16 primaryDbId, char *primaryFilespaceLocation, int16 mirrorDbId, char *mirrorFilespaceLocation, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(filespaceOid);
    check_expected(primaryDbId);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorDbId);
    check_expected(mirrorFilespaceLocation);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
PersistentFilespace_Created(Oid filespaceOid, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(filespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);

    mock();

}

PersistentFileSysObjStateChangeResult 
PersistentFilespace_MarkDropPending(Oid filespaceOid, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(filespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);

    return (PersistentFileSysObjStateChangeResult) mock();

}

PersistentFileSysObjStateChangeResult 
PersistentFilespace_MarkAbortingCreate(Oid filespaceOid, ItemPointer persistentTid, int64 persistentSerialNum, bool retryPossible)
{

    check_expected(filespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(retryPossible);

    return (PersistentFileSysObjStateChangeResult) mock();

}

void 
PersistentFilespace_Dropped(Oid filespaceOid, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(filespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);

    mock();

}

int16 
PersistentFilespace_LookupMirrorDbid(int16 primaryDbid)
{

    check_expected(primaryDbid);

    return (int16) mock();

}

Size 
PersistentFilespace_ShmemSize(void)
{



    return (Size) mock();

}

void 
PersistentFilespace_ShmemInit(void)
{



    mock();

}

void 
PersistentFilespace_AddMirror(Oid filespace, char *mirpath, int16 pridbid, int16 mirdbid, bool set_mirror_existence)
{

    check_expected(filespace);
    check_expected(mirpath);
    check_expected(pridbid);
    check_expected(mirdbid);
    check_expected(set_mirror_existence);
    optional_assignment(mirpath);
    mock();

}

void 
PersistentFilespace_RemoveSegment(int16 dbid, bool ismirror)
{

    check_expected(dbid);
    check_expected(ismirror);

    mock();

}

void 
PersistentFilespace_ActivateStandby(int16 oldmaster, int16 newmaster)
{

    check_expected(oldmaster);
    check_expected(newmaster);

    mock();

}

void 
get_filespace_data(fspc_agg_state ** fas, char *caller)
{

    check_expected(fas);
    check_expected(caller);
    optional_assignment(fas);
    optional_assignment(caller);
    mock();

}
