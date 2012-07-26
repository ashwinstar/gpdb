/*
 * Auto-generated Mocking Source
 * 
 * Source: access/persistentfilesysobjname.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "miscadmin.h"
#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"
#include "catalog/catalog.h"
#include "catalog/gp_persistent.h"
#include "access/persistentfilesysobjname.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"
#include "storage/shmem.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/pg_tablespace.h"
#include "utils/guc.h"

char           *
PersistentFileSysObjName_ObjectName(const PersistentFileSysObjName * name)
{

    check_expected(name);

    return (char *) mock();

}

char           *
PersistentFileSysObjName_TypeName(PersistentFsObjType type)
{

    check_expected(type);

    return (char *) mock();

}

char           *
PersistentFileSysObjName_TypeAndObjectName(const PersistentFileSysObjName * name)
{

    check_expected(name);

    return (char *) mock();

}

int 
PersistentFileSysObjName_Compare(const PersistentFileSysObjName * name1, const PersistentFileSysObjName * name2)
{

    check_expected(name1);
    check_expected(name2);

    return (int) mock();

}

char           *
PersistentFileSysObjState_Name(PersistentFileSysState state)
{

    check_expected(state);

    return (char *) mock();

}

char           *
MirroredObjectExistenceState_Name(MirroredObjectExistenceState mirrorExistenceState)
{

    check_expected(mirrorExistenceState);

    return (char *) mock();

}

char           *
PersistentFileSysRelStorageMgr_Name(PersistentFileSysRelStorageMgr relStorageMgr)
{

    check_expected(relStorageMgr);

    return (char *) mock();

}

char           *
MirroredRelDataSynchronizationState_Name(MirroredRelDataSynchronizationState relDataSynchronizationState)
{

    check_expected(relDataSynchronizationState);

    return (char *) mock();

}

char           *
PersistentFileSysRelBufpoolKind_Name(PersistentFileSysRelBufpoolKind relBufpoolKind)
{

    check_expected(relBufpoolKind);

    return (char *) mock();

}
