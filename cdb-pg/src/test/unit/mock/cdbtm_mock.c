/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbtm.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "access/catquery.h"
#include "cdb/cdbtm.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbdtxcontextinfo.h"
#include "cdb/cdbvars.h"
#include "gp-libpq-fe.h"
#include "access/transam.h"
#include "access/xact.h"
#include "cdb/cdbfts.h"
#include "lib/stringinfo.h"
#include "access/twophase.h"
#include "access/distributedlog.h"
#include "postmaster/postmaster.h"
#include "cdb/cdbpersistentrecovery.h"
#include "cdb/cdbllize.h"
#include "utils/faultinjector.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"

DtxContext      DistributedTransactionContext;
volatile bool  *shmDtmStarted;
volatile int   *shmSegmentCount;
volatile int   *shmSegmentsStatesByteLen;
char           *
DtxStateToString(DtxState state)
{

    check_expected(state);

    return (char *) mock();

}

char           *
DtxProtocolCommandToString(DtxProtocolCommand command)
{

    check_expected(command);

    return (char *) mock();

}

char           *
DtxContextToString(DtxContext context)
{

    check_expected(context);

    return (char *) mock();

}

DistributedTransactionTimeStamp 
getDtxStartTime(void)
{



    return (DistributedTransactionTimeStamp) mock();

}

void 
dtxCrackOpenGid(const char *gid, DistributedTransactionTimeStamp * distribTimeStamp, DistributedTransactionId * distribXid)
{

    check_expected(gid);
    check_expected(distribTimeStamp);
    check_expected(distribXid);
    optional_assignment(distribTimeStamp);
    optional_assignment(distribXid);
    mock();

}

DistributedTransactionId 
getDistributedTransactionId(void)
{



    return (DistributedTransactionId) mock();

}

bool 
getDistributedTransactionIdentifier(char *id)
{

    check_expected(id);
    optional_assignment(id);
    return (bool) mock();

}

void 
createDtx(DistributedTransactionId * distribXid, TransactionId * localXid)
{

    check_expected(distribXid);
    check_expected(localXid);
    optional_assignment(distribXid);
    optional_assignment(localXid);
    mock();

}

bool 
createDtxSnapshot(DistributedSnapshotWithLocalMapping * distribSnapshotWithLocalMapping)
{

    check_expected(distribSnapshotWithLocalMapping);
    optional_assignment(distribSnapshotWithLocalMapping);
    return (bool) mock();

}

void 
prepareDtxTransaction(void)
{



    mock();

}

bool 
isPreparedDtxTransaction(void)
{



    return (bool) mock();

}

void 
getDtxLogInfo(TMGXACT_LOG * gxact_log)
{

    check_expected(gxact_log);
    optional_assignment(gxact_log);
    mock();

}

bool 
notifyCommittedDtxTransactionIsNeeded(void)
{



    return (bool) mock();

}

void 
notifyCommittedDtxTransaction(void)
{



    mock();

}

void 
rollbackDtxTransaction(void)
{



    mock();

}

void 
insertingDistributedCommitted(void)
{



    mock();

}

void 
insertedDistributedCommitted(void)
{



    mock();

}

void 
forcedDistributedCommitted(XLogRecPtr * recptr)
{

    check_expected(recptr);
    optional_assignment(recptr);
    mock();

}

void 
redoDtxCheckPoint(TMGXACT_CHECKPOINT * gxact_checkpoint)
{

    check_expected(gxact_checkpoint);
    optional_assignment(gxact_checkpoint);
    mock();

}

void 
redoDistributedCommitRecord(TMGXACT_LOG * gxact_log)
{

    check_expected(gxact_log);
    optional_assignment(gxact_log);
    mock();

}

void 
redoDistributedForgetCommitRecord(TMGXACT_LOG * gxact_log)
{

    check_expected(gxact_log);
    optional_assignment(gxact_log);
    mock();

}

void 
descDistributedCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log)
{

    check_expected(buf);
    check_expected(gxact_log);
    optional_assignment(gxact_log);
    mock();

}

void 
descDistributedForgetCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log)
{

    check_expected(buf);
    check_expected(gxact_log);
    optional_assignment(gxact_log);
    mock();

}

void 
dtmPreCommand(const char *debugCaller, const char *debugDetail, PlannedStmt * stmt, bool needsTwoPhaseCommit, bool dispatchToPrimaries, bool dispatchToMirrors)
{

    check_expected(debugCaller);
    check_expected(debugDetail);
    check_expected(stmt);
    check_expected(needsTwoPhaseCommit);
    check_expected(dispatchToPrimaries);
    check_expected(dispatchToMirrors);
    optional_assignment(stmt);
    mock();

}

bool 
isCurrentDtxTwoPhase(void)
{



    return (bool) mock();

}

bool 
isCurrentDtxActive(void)
{



    return (bool) mock();

}

void 
sendDtxExplicitBegin(void)
{



    mock();

}

int 
dtxCurrentPhase1Count(void)
{



    return (int) mock();

}

bool 
dispatchDtxCommand(const char *cmd, bool withSnapshot, bool raiseError)
{

    check_expected(cmd);
    check_expected(withSnapshot);
    check_expected(raiseError);

    return (bool) mock();

}

void 
tmShmemInit(void)
{



    mock();

}

int 
tmShmemSize(void)
{



    return (int) mock();

}

void 
initTM(void)
{



    mock();

}

void 
restoreGxact(TMGXACT_LOG * gxact, DtxState state)
{

    check_expected(gxact);
    check_expected(state);
    optional_assignment(gxact);
    mock();

}

void 
getDtxCheckPointInfoAndLock(char **result, int *result_size)
{

    check_expected(result);
    check_expected(result_size);
    optional_assignment(result);
    optional_assignment(result_size);
    mock();

}

void 
freeDtxCheckPointInfoAndUnlock(char *info, int info_size, XLogRecPtr * recptr)
{

    check_expected(info);
    check_expected(info_size);
    check_expected(recptr);
    optional_assignment(info);
    optional_assignment(recptr);
    mock();

}

bool 
isTMInRecovery(void)
{



    return (bool) mock();

}

void 
setRecoverTMOn(void)
{



    mock();

}

void 
verify_shared_snapshot_ready(void)
{



    mock();

}

void 
getTmLock(void)
{



    mock();

}

void 
releaseTmLock(void)
{



    mock();

}

int 
mppTxnOptions(bool needTwoPhase)
{

    check_expected(needTwoPhase);

    return (int) mock();

}

int 
mppTxOptions_IsoLevel(int txnOptions)
{

    check_expected(txnOptions);

    return (int) mock();

}

bool 
isMppTxOptions_ReadOnly(int txnOptions)
{

    check_expected(txnOptions);

    return (bool) mock();

}

void 
unpackMppTxnOptions(int txnOptions, int *isoLevel, bool * readOnly)
{

    check_expected(txnOptions);
    check_expected(isoLevel);
    check_expected(readOnly);
    optional_assignment(isoLevel);
    optional_assignment(readOnly);
    mock();

}

bool 
isMppTxOptions_NeedTwoPhase(int txnOptions)
{

    check_expected(txnOptions);

    return (bool) mock();

}

bool 
isMppTxOptions_ExplicitBegin(int txnOptions)
{

    check_expected(txnOptions);

    return (bool) mock();

}

void 
getAllDistributedXactStatus(TMGALLXACTSTATUS ** allDistributedXactStatus)
{

    check_expected(allDistributedXactStatus);
    optional_assignment(allDistributedXactStatus);
    mock();

}

bool 
getNextDistributedXactStatus(TMGALLXACTSTATUS * allDistributedXactStatus, TMGXACTSTATUS ** distributedXactStatus)
{

    check_expected(allDistributedXactStatus);
    check_expected(distributedXactStatus);
    optional_assignment(allDistributedXactStatus);
    optional_assignment(distributedXactStatus);
    return (bool) mock();

}

void 
setupRegularDtxContext(void)
{



    mock();

}

void 
setupQEDtxContext(DtxContextInfo * dtxContextInfo)
{

    check_expected(dtxContextInfo);
    optional_assignment(dtxContextInfo);
    mock();

}

void 
finishDistributedTransactionContext(char *debugCaller, bool aborted)
{

    check_expected(debugCaller);
    check_expected(aborted);
    optional_assignment(debugCaller);
    mock();

}

void 
performDtxProtocolCommand(DtxProtocolCommand dtxProtocolCommand, int flags, const char *loggingStr, const char *gid, DistributedTransactionId gxid, DtxContextInfo * contextInfo)
{

    check_expected(dtxProtocolCommand);
    check_expected(flags);
    check_expected(loggingStr);
    check_expected(gid);
    check_expected(gxid);
    check_expected(contextInfo);
    optional_assignment(contextInfo);
    mock();

}

void 
doDtxPhase2Retry(void)
{



    mock();

}

void 
UtilityModeFindOrCreateDtmRedoFile(void)
{



    mock();

}

void 
UtilityModeCloseDtmRedoFile(void)
{



    mock();

}

void 
PleaseDebugMe(char *caller)
{

    check_expected(caller);
    optional_assignment(caller);
    mock();

}

void 
cdbtm_performDeferredRecovery(void)
{



    mock();

}
