/*
 * Auto-generated Mocking Source
 * 
 * Source: access/transam.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "utils/tqual.h"

VariableCache   ShmemVariableCache;
int             xid_stop_limit;
bool 
TransactionIdDidCommit(TransactionId transactionId)
{

    check_expected(transactionId);

    return (bool) mock();

}

bool 
TransactionIdDidAbort(TransactionId transactionId)
{

    check_expected(transactionId);

    return (bool) mock();

}

void 
TransactionIdCommit(TransactionId transactionId)
{

    check_expected(transactionId);

    mock();

}

void 
TransactionIdAbort(TransactionId transactionId)
{

    check_expected(transactionId);

    mock();

}

void 
TransactionIdSubCommit(TransactionId transactionId)
{

    check_expected(transactionId);

    mock();

}

void 
TransactionIdCommitTree(int nxids, TransactionId * xids)
{

    check_expected(nxids);
    check_expected(xids);
    optional_assignment(xids);
    mock();

}

void 
TransactionIdAbortTree(int nxids, TransactionId * xids)
{

    check_expected(nxids);
    check_expected(xids);
    optional_assignment(xids);
    mock();

}

bool 
TransactionIdPrecedes(TransactionId id1, TransactionId id2)
{

    check_expected(id1);
    check_expected(id2);

    return (bool) mock();

}

bool 
TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2)
{

    check_expected(id1);
    check_expected(id2);

    return (bool) mock();

}

bool 
TransactionIdFollows(TransactionId id1, TransactionId id2)
{

    check_expected(id1);
    check_expected(id2);

    return (bool) mock();

}

bool 
TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2)
{

    check_expected(id1);
    check_expected(id2);

    return (bool) mock();

}

TransactionId 
GetNewTransactionId(bool isSubXact, bool setProcXid)
{

    check_expected(isSubXact);
    check_expected(setProcXid);

    return (TransactionId) mock();

}

TransactionId 
ReadNewTransactionId(void)
{



    return (TransactionId) mock();

}

void 
SetTransactionIdLimit(TransactionId oldest_datfrozenxid, Name oldest_datname)
{

    check_expected(oldest_datfrozenxid);
    check_expected(oldest_datname);

    mock();

}

Oid 
GetNewObjectId(void)
{



    return (Oid) mock();

}
