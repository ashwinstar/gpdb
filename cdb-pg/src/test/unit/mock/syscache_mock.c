/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/syscache.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_window.h"
#include "utils/syscache.h"

void 
InitCatalogCache(void)
{



    mock();

}

void 
InitCatalogCachePhase2(void)
{



    mock();

}

HeapTuple 
SearchSysCache(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{

    check_expected(cacheId);
    check_expected(key1);
    check_expected(key2);
    check_expected(key3);
    check_expected(key4);

    return (HeapTuple) mock();

}

void 
ReleaseSysCache(HeapTuple tuple)
{

    check_expected(tuple);

    mock();

}

HeapTuple 
SearchSysCacheKeyArray(int cacheId, int numkeys, Datum * keys)
{

    check_expected(cacheId);
    check_expected(numkeys);
    check_expected(keys);
    optional_assignment(keys);
    return (HeapTuple) mock();

}

HeapTuple 
SearchSysCacheCopy(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{

    check_expected(cacheId);
    check_expected(key1);
    check_expected(key2);
    check_expected(key3);
    check_expected(key4);

    return (HeapTuple) mock();

}

bool 
SearchSysCacheExists(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{

    check_expected(cacheId);
    check_expected(key1);
    check_expected(key2);
    check_expected(key3);
    check_expected(key4);

    return (bool) mock();

}

Oid 
GetSysCacheOid(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{

    check_expected(cacheId);
    check_expected(key1);
    check_expected(key2);
    check_expected(key3);
    check_expected(key4);

    return (Oid) mock();

}

HeapTuple 
SearchSysCacheAttName(Oid relid, const char *attname)
{

    check_expected(relid);
    check_expected(attname);

    return (HeapTuple) mock();

}

HeapTuple 
SearchSysCacheCopyAttName(Oid relid, const char *attname)
{

    check_expected(relid);
    check_expected(attname);

    return (HeapTuple) mock();

}

bool 
SearchSysCacheExistsAttName(Oid relid, const char *attname)
{

    check_expected(relid);
    check_expected(attname);

    return (bool) mock();

}

Datum 
SysCacheGetAttr(int cacheId, HeapTuple tup, AttrNumber attributeNumber, bool * isNull)
{

    check_expected(cacheId);
    check_expected(tup);
    check_expected(attributeNumber);
    check_expected(isNull);
    optional_assignment(isNull);
    return (Datum) mock();

}

struct catclist *
SearchSysCacheList(int cacheId, int nkeys, Datum key1, Datum key2, Datum key3, Datum key4)
{

    check_expected(cacheId);
    check_expected(nkeys);
    check_expected(key1);
    check_expected(key2);
    check_expected(key3);
    check_expected(key4);

    mock();

}
