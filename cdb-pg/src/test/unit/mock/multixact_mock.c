/*
 * Auto-generated Mocking Source
 * 
 * Source: access/multixact.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/multixact.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "storage/procarray.h"
#include "cdb/cdbpersistentstore.h"

MultiXactId 
MultiXactIdCreate(TransactionId xid1, TransactionId xid2)
{

    check_expected(xid1);
    check_expected(xid2);

    return (MultiXactId) mock();

}

MultiXactId 
MultiXactIdExpand(MultiXactId multi, TransactionId xid)
{

    check_expected(multi);
    check_expected(xid);

    return (MultiXactId) mock();

}

bool 
MultiXactIdIsRunning(MultiXactId multi)
{

    check_expected(multi);

    return (bool) mock();

}

bool 
MultiXactIdIsCurrent(MultiXactId multi)
{

    check_expected(multi);

    return (bool) mock();

}

void 
MultiXactIdWait(MultiXactId multi)
{

    check_expected(multi);

    mock();

}

bool 
ConditionalMultiXactIdWait(MultiXactId multi)
{

    check_expected(multi);

    return (bool) mock();

}

void 
MultiXactIdSetOldestMember(void)
{



    mock();

}

int 
GetMultiXactIdMembers(MultiXactId multi, TransactionId ** xids)
{

    check_expected(multi);
    check_expected(xids);
    optional_assignment(xids);
    return (int) mock();

}

void 
AtEOXact_MultiXact(void)
{



    mock();

}

Size 
MultiXactShmemSize(void)
{



    return (Size) mock();

}

void 
MultiXactShmemInit(void)
{



    mock();

}

void 
BootStrapMultiXact(void)
{



    mock();

}

void 
StartupMultiXact(void)
{



    mock();

}

void 
ShutdownMultiXact(void)
{



    mock();

}

void 
MultiXactGetCheckptMulti(bool is_shutdown, MultiXactId * nextMulti, MultiXactOffset * nextMultiOffset)
{

    check_expected(is_shutdown);
    check_expected(nextMulti);
    check_expected(nextMultiOffset);
    optional_assignment(nextMulti);
    optional_assignment(nextMultiOffset);
    mock();

}

void 
CheckPointMultiXact(void)
{



    mock();

}

void 
MultiXactSetNextMXact(MultiXactId nextMulti, MultiXactOffset nextMultiOffset)
{

    check_expected(nextMulti);
    check_expected(nextMultiOffset);

    mock();

}

void 
MultiXactAdvanceNextMXact(MultiXactId minMulti, MultiXactOffset minMultiOffset)
{

    check_expected(minMulti);
    check_expected(minMultiOffset);

    mock();

}

void 
multixact_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord * record)
{

    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(lsn.xlogid);
    check_expected(lsn.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}

void 
multixact_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord * record)
{

    check_expected(buf);
    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}
