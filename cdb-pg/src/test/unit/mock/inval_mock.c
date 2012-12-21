/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/inval.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/gp_policy.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/simex.h"
#include "utils/syscache.h"

void 
AcceptInvalidationMessages(void)
{



    mock();

}

void 
AtStart_Inval(void)
{



    mock();

}

void 
AtSubStart_Inval(void)
{



    mock();

}

void 
AtEOXact_Inval(bool isCommit)
{

    check_expected(isCommit);

    mock();

}

void 
AtEOSubXact_Inval(bool isCommit)
{

    check_expected(isCommit);

    mock();

}

void 
AtPrepare_Inval(void)
{



    mock();

}

void 
PostPrepare_Inval(void)
{



    mock();

}

void 
CommandEndInvalidationMessages(void)
{



    mock();

}

void 
CacheInvalidateHeapTuple(Relation relation, HeapTuple tuple)
{

    check_expected(relation);
    check_expected(tuple);

    mock();

}

void 
CacheInvalidateRelcache(Relation relation)
{

    check_expected(relation);

    mock();

}

void 
CacheInvalidateRelcacheByTuple(HeapTuple classTuple)
{

    check_expected(classTuple);

    mock();

}

void 
CacheInvalidateRelcacheByRelid(Oid relid)
{

    check_expected(relid);

    mock();

}

void 
CacheRegisterSyscacheCallback(int cacheid, CacheCallbackFunction func, Datum arg)
{

    check_expected(cacheid);
    check_expected(func);
    check_expected(arg);

    mock();

}

void 
CacheRegisterRelcacheCallback(CacheCallbackFunction func, Datum arg)
{

    check_expected(func);
    check_expected(arg);

    mock();

}

void 
inval_twophase_postcommit(TransactionId xid, uint16 info, void *recdata, uint32 len)
{

    check_expected(xid);
    check_expected(info);
    check_expected(recdata);
    check_expected(len);
    optional_assignment(recdata);
    mock();

}

int             gp_test_system_cache_flush_force;
