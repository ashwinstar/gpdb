/*
 * Auto-generated Mocking Source
 * 
 * Source: access/twophase.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "access/heapam.h"
#include "access/xlogmm.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "access/distributedlog.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbpersistentrecovery.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbmirroredflatfile.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbpersistentfilesysobj.h"

int             max_prepared_xacts;
Size 
TwoPhaseShmemSize(void)
{



    return (Size) mock();

}

void 
TwoPhaseShmemInit(void)
{



    mock();

}

PGPROC         *
TwoPhaseGetDummyProc(TransactionId xid)
{

    check_expected(xid);

    return (PGPROC *) mock();

}

GlobalTransaction 
MarkAsPreparing(TransactionId xid, LocalDistribXactRef * distribToLocalXactRef, const char *gid, TimestampTz prepared_at, Oid owner, Oid databaseid, XLogRecPtr * xlogrecptr)
{

    check_expected(xid);
    check_expected(distribToLocalXactRef);
    check_expected(gid);
    check_expected(prepared_at);
    check_expected(owner);
    check_expected(databaseid);
    check_expected(xlogrecptr);
    optional_assignment(distribToLocalXactRef);
    optional_assignment(xlogrecptr);
    return (GlobalTransaction) mock();

}

void 
StartPrepare(GlobalTransaction gxact)
{

    check_expected(gxact);

    mock();

}

void 
EndPrepare(GlobalTransaction gxact)
{

    check_expected(gxact);

    mock();

}

TransactionId 
PrescanPreparedTransactions(void)
{



    return (TransactionId) mock();

}

void 
RecoverPreparedTransactions(void)
{



    mock();

}

void 
RecreateTwoPhaseFile(TransactionId xid, void *content, int len, XLogRecPtr * xlogrecptr)
{

    check_expected(xid);
    check_expected(content);
    check_expected(len);
    check_expected(xlogrecptr);
    optional_assignment(content);
    optional_assignment(xlogrecptr);
    mock();

}

void 
RemoveTwoPhaseFile(TransactionId xid, bool giveWarning)
{

    check_expected(xid);
    check_expected(giveWarning);

    mock();

}

void 
CheckPointTwoPhase(XLogRecPtr redo_horizon)
{

    check_expected(redo_horizon.xlogid);
    check_expected(redo_horizon.xrecoff);

    mock();

}

void 
PrepareIntentAppendOnlyCommitWork(char *gid)
{

    check_expected(gid);
    optional_assignment(gid);
    mock();

}

void 
PrepareDecrAppendOnlyCommitWork(char *gid)
{

    check_expected(gid);
    optional_assignment(gid);
    mock();

}

bool 
FinishPreparedTransaction(const char *gid, bool isCommit, bool raiseErrorIfNotFound)
{

    check_expected(gid);
    check_expected(isCommit);
    check_expected(raiseErrorIfNotFound);

    return (bool) mock();

}

int 
TwoPhaseRecoverMirror(void)
{



    return (int) mock();

}

void 
TwoPhaseAddPreparedTransactionInit(prepared_transaction_agg_state ** ptas, int *maxCount)
{

    check_expected(ptas);
    check_expected(maxCount);
    optional_assignment(ptas);
    optional_assignment(maxCount);
    mock();

}

XLogRecPtr     *
getTwoPhaseOldestPreparedTransactionXLogRecPtr(XLogRecData * rdata)
{

    check_expected(rdata);
    optional_assignment(rdata);
    return (XLogRecPtr *) mock();

}

void 
TwoPhaseAddPreparedTransaction(prepared_transaction_agg_state ** ptas, int *maxCount, TransactionId xid, XLogRecPtr * xlogPtr, char *caller)
{

    check_expected(ptas);
    check_expected(maxCount);
    check_expected(xid);
    check_expected(xlogPtr);
    check_expected(caller);
    optional_assignment(ptas);
    optional_assignment(maxCount);
    optional_assignment(xlogPtr);
    optional_assignment(caller);
    mock();

}

void 
getTwoPhasePreparedTransactionData(prepared_transaction_agg_state ** ptas, char *caller)
{

    check_expected(ptas);
    check_expected(caller);
    optional_assignment(ptas);
    optional_assignment(caller);
    mock();

}

void 
SetupCheckpointPreparedTransactionList(XLogRecord * record)
{

    check_expected(record);
    optional_assignment(record);
    mock();

}

bool 
TwoPhaseFindRecoverPostCheckpointPreparedTransactionsMapEntry(TransactionId xid, XLogRecPtr * m, char *caller)
{

    check_expected(xid);
    check_expected(m);
    check_expected(caller);
    optional_assignment(m);
    optional_assignment(caller);
    return (bool) mock();

}
