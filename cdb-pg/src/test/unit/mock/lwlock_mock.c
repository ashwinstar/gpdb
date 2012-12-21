/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/lwlock.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/clog.h"
#include "access/multixact.h"
#include "access/distributedlog.h"
#include "access/subtrans.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/spin.h"

LWLockId 
LWLockAssign(void)
{



    return (LWLockId) mock();

}

void 
LWLockAcquire(LWLockId lockid, LWLockMode mode)
{

    check_expected(lockid);
    check_expected(mode);

    mock();

}

bool 
LWLockConditionalAcquire(LWLockId lockid, LWLockMode mode)
{

    check_expected(lockid);
    check_expected(mode);

    return (bool) mock();

}

void 
LWLockRelease(LWLockId lockid)
{

    check_expected(lockid);

    mock();

}

void 
LWLockReleaseAll(void)
{



    mock();

}

bool 
LWLockHeldByMe(LWLockId lockid)
{

    check_expected(lockid);

    return (bool) mock();

}

bool 
LWLockHeldExclusiveByMe(LWLockId lockid)
{

    check_expected(lockid);

    return (bool) mock();

}

uint32 
LWLocksHeld(void)
{



    return (uint32) mock();

}

LWLockId 
LWLockHeldLatestId(void)
{



    return (LWLockId) mock();

}

void           *
LWLockHeldLatestCaller(void)
{



    mock();

}

const char     *
LWLocksHeldStackTraces(void)
{



    return (char *) (unsigned) mock();

}

int 
NumLWLocks(void)
{



    return (int) mock();

}

Size 
LWLockShmemSize(void)
{



    return (Size) mock();

}

void 
CreateLWLocks(void)
{



    mock();

}

void 
RequestAddinLWLocks(int n)
{

    check_expected(n);

    mock();

}
