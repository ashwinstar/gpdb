/*
 * Auto-generated Mocking Source
 * 
 * Source: access/xact.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <time.h>
#include <unistd.h>
#include "access/appendonlywriter.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "cdb/cdblocaldistribxact.h"
#include "cdb/cdbdistributedsnapshot.h"
#include "cdb/cdbtm.h"
#include "access/xlogutils.h"
#include "access/fileam.h"
#include "catalog/namespace.h"
#include "commands/async.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "libpq/be-fsstubs.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "storage/freespace.h"
#include "utils/combocid.h"
#include "utils/faultinjector.h"
#include "utils/flatfiles.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/resscheduler.h"
#include "access/distributedlog.h"
#include "access/clog.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbvars.h"	/* Gp_role, Gp_is_writer,
				 * interconnect_setup_timeout */
#include "cdb/cdbpersistentstore.h"

int             DefaultXactIsoLevel;
int             XactIsoLevel;
bool            DefaultXactReadOnly;
bool            XactReadOnly;
XidBuffer       subxbuf;
File            subxip_file;
char           *
XactInfoKind_Name(const XactInfoKind kind)
{

    check_expected(kind);

    return (char *) mock();

}

void 
SetSharedTransactionId(void)
{



    mock();

}

void 
SetSharedTransactionId_reader(TransactionId xid, CommandId cid)
{

    check_expected(xid);
    check_expected(cid);

    mock();

}

void 
SetXactSeqXlog(void)
{



    mock();

}

bool 
IsTransactionState(void)
{



    return (bool) mock();

}

bool 
IsAbortInProgress(void)
{



    return (bool) mock();

}

bool 
IsAbortedTransactionBlockState(void)
{



    return (bool) mock();

}

void 
GetAllTransactionXids(DistributedTransactionId * distribXid, TransactionId * localXid, TransactionId * subXid)
{

    check_expected(distribXid);
    check_expected(localXid);
    check_expected(subXid);
    optional_assignment(distribXid);
    optional_assignment(localXid);
    optional_assignment(subXid);
    mock();

}

TransactionId 
GetTopTransactionId(void)
{



    return (TransactionId) mock();

}

TransactionId 
GetCurrentTransactionId(void)
{



    return (TransactionId) mock();

}

TransactionId 
GetCurrentTransactionIdIfAny(void)
{



    return (TransactionId) mock();

}

SubTransactionId 
GetCurrentSubTransactionId(void)
{



    return (SubTransactionId) mock();

}

CommandId 
GetCurrentCommandId(void)
{



    return (CommandId) mock();

}

TimestampTz 
GetCurrentTransactionStartTimestamp(void)
{



    return (TimestampTz) mock();

}

TimestampTz 
GetCurrentStatementStartTimestamp(void)
{



    return (TimestampTz) mock();

}

TimestampTz 
GetCurrentTransactionStopTimestamp(void)
{



    return (TimestampTz) mock();

}

void 
SetCurrentStatementStartTimestamp(void)
{



    mock();

}

void 
SetCurrentStatementStartTimestampToMaster(TimestampTz masterTime)
{

    check_expected(masterTime);

    mock();

}

int 
GetCurrentTransactionNestLevel(void)
{



    return (int) mock();

}

bool 
TransactionIdIsCurrentTransactionId(TransactionId xid)
{

    check_expected(xid);

    return (bool) mock();

}

void 
CommandCounterIncrement(void)
{



    mock();

}

void 
StartTransactionCommand(void)
{



    mock();

}

void 
CommitTransactionCommand(void)
{



    mock();

}

void 
AbortCurrentTransaction(void)
{



    mock();

}

void 
BeginTransactionBlock(void)
{



    mock();

}

bool 
EndTransactionBlock(void)
{



    return (bool) mock();

}

bool 
PrepareTransactionBlock(char *gid)
{

    check_expected(gid);
    optional_assignment(gid);
    return (bool) mock();

}

void 
UserAbortTransactionBlock(void)
{



    mock();

}

void 
ReleaseSavepoint(List * options)
{

    check_expected(options);
    optional_assignment(options);
    mock();

}

void 
DefineSavepoint(char *name)
{

    check_expected(name);
    optional_assignment(name);
    mock();

}

void 
DefineDispatchSavepoint(char *name)
{

    check_expected(name);
    optional_assignment(name);
    mock();

}

void 
RollbackToSavepoint(List * options)
{

    check_expected(options);
    optional_assignment(options);
    mock();

}

void 
BeginInternalSubTransaction(char *name)
{

    check_expected(name);
    optional_assignment(name);
    mock();

}

void 
ReleaseCurrentSubTransaction(void)
{



    mock();

}

void 
RollbackAndReleaseCurrentSubTransaction(void)
{



    mock();

}

void 
TransactionInformationQEWriter(DistributedTransactionId * QEDistributedTransactionId, CommandId * QECommandId, bool * QEDirty)
{

    check_expected(QEDistributedTransactionId);
    check_expected(QECommandId);
    check_expected(QEDirty);
    optional_assignment(QEDistributedTransactionId);
    optional_assignment(QECommandId);
    optional_assignment(QEDirty);
    mock();

}

bool 
IsSubTransaction(void)
{



    return (bool) mock();

}

bool 
IsTransactionBlock(void)
{



    return (bool) mock();

}

bool 
IsTransactionOrTransactionBlock(void)
{



    return (bool) mock();

}

bool 
IsTransactionDirty(void)
{



    return (bool) mock();

}

void 
ExecutorMarkTransactionUsesSequences(void)
{



    mock();

}

void 
ExecutorMarkTransactionDoesWrites(void)
{



    mock();

}

bool 
ExecutorSaysTransactionDoesWrites(void)
{



    return (bool) mock();

}

char 
TransactionBlockStatusCode(void)
{



    return (char) mock();

}

void 
AbortOutOfAnyTransaction(void)
{



    mock();

}

void 
PreventTransactionChain(void *stmtNode, const char *stmtType)
{

    check_expected(stmtNode);
    check_expected(stmtType);
    optional_assignment(stmtNode);
    mock();

}

void 
RequireTransactionChain(void *stmtNode, const char *stmtType)
{

    check_expected(stmtNode);
    check_expected(stmtType);
    optional_assignment(stmtNode);
    mock();

}

bool 
IsInTransactionChain(void *stmtNode)
{

    check_expected(stmtNode);
    optional_assignment(stmtNode);
    return (bool) mock();

}

void 
RegisterXactCallback(XactCallback callback, void *arg)
{

    check_expected(callback);
    check_expected(arg);
    optional_assignment(arg);
    mock();

}

void 
UnregisterXactCallback(XactCallback callback, void *arg)
{

    check_expected(callback);
    check_expected(arg);
    optional_assignment(arg);
    mock();

}

void 
RegisterXactCallbackOnce(XactCallback callback, void *arg)
{

    check_expected(callback);
    check_expected(arg);
    optional_assignment(arg);
    mock();

}

void 
UnregisterXactCallbackOnce(XactCallback callback, void *arg)
{

    check_expected(callback);
    check_expected(arg);
    optional_assignment(arg);
    mock();

}

void 
RegisterSubXactCallback(SubXactCallback callback, void *arg)
{

    check_expected(callback);
    check_expected(arg);
    optional_assignment(arg);
    mock();

}

void 
UnregisterSubXactCallback(SubXactCallback callback, void *arg)
{

    check_expected(callback);
    check_expected(arg);
    optional_assignment(arg);
    mock();

}

void 
RecordTransactionCommit(void)
{



    mock();

}

void 
RecordDistributedForgetCommitted(struct TMGXACT_LOG * gxact_log)
{

    check_expected(gxact_log);
    optional_assignment(gxact_log);
    mock();

}

bool 
RecordCrashTransactionAbortRecord(TransactionId xid, PersistentEndXactRecObjects * persistentAbortObjects)
{

    check_expected(xid);
    check_expected(persistentAbortObjects);
    optional_assignment(persistentAbortObjects);
    return (bool) mock();

}

void 
EarlyVacuumRecordTransactionCommit(void)
{



    mock();

}

int 
xactGetCommittedChildren(TransactionId ** ptr)
{

    check_expected(ptr);
    optional_assignment(ptr);
    return (int) mock();

}

void 
xact_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord * record)
{

    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(lsn.xlogid);
    check_expected(lsn.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}

bool 
xact_redo_get_info(XLogRecord * record, XactInfoKind * infoKind, TransactionId * xid, PersistentEndXactRecObjects * persistentObjects, TransactionId ** subXids, int *subXidCount)
{

    check_expected(record);
    check_expected(infoKind);
    check_expected(xid);
    check_expected(persistentObjects);
    check_expected(subXids);
    check_expected(subXidCount);
    optional_assignment(record);
    optional_assignment(infoKind);
    optional_assignment(xid);
    optional_assignment(persistentObjects);
    optional_assignment(subXids);
    optional_assignment(subXidCount);
    return (bool) mock();

}

void 
xact_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord * record)
{

    check_expected(buf);
    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}

const char     *
IsoLevelAsUpperString(int IsoLevel)
{

    check_expected(IsoLevel);

    return (char *) mock();

}

char           *
WatchCurrentTransactionString(void)
{



    return (char *) mock();

}

bool 
FindXidInXidBuffer(XidBuffer * xidbuf, TransactionId xid, uint32 * cnt, int32 * index)
{

    check_expected(xidbuf);
    check_expected(xid);
    check_expected(cnt);
    check_expected(index);
    optional_assignment(xidbuf);
    optional_assignment(cnt);
    optional_assignment(index);
    return (bool) mock();

}

void 
AddSortedToXidBuffer(XidBuffer * xidbuf, TransactionId * xids, uint32 cnt)
{

    check_expected(xidbuf);
    check_expected(xids);
    check_expected(cnt);
    optional_assignment(xidbuf);
    optional_assignment(xids);
    mock();

}

void 
ResetXidBuffer(XidBuffer * xidbuf)
{

    check_expected(xidbuf);
    optional_assignment(xidbuf);
    mock();

}

TransactionId 
GetLastTransactionIdFromSnapshot(DistributedTransactionId dxid)
{

    check_expected(dxid);

    return (TransactionId) mock();

}

void 
ShowSubtransactionsForSharedSnapshot(void)
{



    mock();

}

void 
GetSubXidsInXidBuffer(void)
{



    mock();

}

void 
UpdateSubtransactionsInSharedSnapshot(DistributedTransactionId dxid)
{

    check_expected(dxid);

    mock();

}

void 
ClearTransactionFromPgProc_UnderLock(void)
{



    mock();

}
