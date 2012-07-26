/*
 * Auto-generated Mocking Source
 * 
 * Source: access/xlog.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "access/clog.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/distributedlog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogmm.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "catalog/pg_type.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "funcapi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"
#include "postmaster/checkpoint.h"
#include "postmaster/walsendserver.h"
#include "storage/proc.h"
#include "storage/bufpage.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/flatfiles.h"
#include "utils/pg_locale.h"
#include "utils/nabstime.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/resscheduler.h"
#include "pg_trace.h"
#include "utils/catcache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/pg_crc.h"
#include "storage/backendid.h"
#include "storage/sinval.h"
#include "cdb/cdblink.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbmirroredflatfile.h"
#include "cdb/cdbpersistentrecovery.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbfilerep.h"
#include "postmaster/primary_mirror_mode.h"
#include "utils/elog.h"

int             sync_method;
TimeLineID      ThisTimeLineID;
bool            InRecovery;
bool            CanEmitXLogRecords;
XLogRecPtr      MyLastRecPtr;
bool            MyXactMadeXLogEntry;
bool            MyXactMadeTempRelUpdate;
XLogRecPtr      ProcLastRecEnd;
int             CheckPointSegments;
int             XLOGbuffers;
char           *XLogArchiveCommand;
int             XLogArchiveTimeout;
char           *XLOG_sync_method;
const char      XLOG_sync_method_default[];
bool            gp_keep_all_xlog;
bool 
XLog_CanBypassWal(void)
{



    return (bool) mock();

}

bool 
XLog_UnconvertedCanBypassWal(void)
{



    return (bool) mock();

}

char           *writeBufAligned;
XLogRecPtr 
XLogInsert(RmgrId rmid, uint8 info, XLogRecData * rdata)
{

    check_expected(rmid);
    check_expected(info);
    check_expected(rdata);
    optional_assignment(rdata);
    uint64_t        mock__tmp_r = (uint64_t) mock();
    XLogRecPtr      mock__tmp_r2;
    memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
    return mock__tmp_r2;


}

XLogRecPtr 
XLogInsert_OverrideXid(RmgrId rmid, uint8 info, XLogRecData * rdata, TransactionId overrideXid)
{

    check_expected(rmid);
    check_expected(info);
    check_expected(rdata);
    check_expected(overrideXid);
    optional_assignment(rdata);
    uint64_t        mock__tmp_r = (uint64_t) mock();
    XLogRecPtr      mock__tmp_r2;
    memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
    return mock__tmp_r2;


}

XLogRecPtr 
XLogLastInsertBeginLoc(void)
{



    uint64_t        mock__tmp_r = (uint64_t) mock();
    XLogRecPtr      mock__tmp_r2;
    memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
    return mock__tmp_r2;


}

XLogRecPtr 
XLogLastInsertEndLoc(void)
{



    uint64_t        mock__tmp_r = (uint64_t) mock();
    XLogRecPtr      mock__tmp_r2;
    memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
    return mock__tmp_r2;


}

XLogRecPtr 
XLogLastChangeTrackedLoc(void)
{



    uint64_t        mock__tmp_r = (uint64_t) mock();
    XLogRecPtr      mock__tmp_r2;
    memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
    return mock__tmp_r2;


}

uint32 
XLogLastInsertTotalLen(void)
{



    return (uint32) mock();

}

uint32 
XLogLastInsertDataLen(void)
{



    return (uint32) mock();

}

void 
XLogFlush(XLogRecPtr RecPtr)
{

    check_expected(RecPtr.xlogid);
    check_expected(RecPtr.xrecoff);

    mock();

}

void 
XLogFileRepFlushCache(XLogRecPtr * lastChangeTrackingEndLoc)
{

    check_expected(lastChangeTrackingEndLoc);
    optional_assignment(lastChangeTrackingEndLoc);
    mock();

}

void 
xlog_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord * record)
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
xlog_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord * record)
{

    check_expected(buf);
    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}

void 
UpdateControlFile(void)
{



    mock();

}

Size 
XLOGShmemSize(void)
{



    return (Size) mock();

}

void 
XLOGShmemInit(void)
{



    mock();

}

void 
XLogStartupInit(void)
{



    mock();

}

void 
BootStrapXLOG(void)
{



    mock();

}

void 
StartupXLOG(void)
{



    mock();

}

bool 
XLogStartupMultipleRecoveryPassesNeeded(void)
{



    return (bool) mock();

}

bool 
XLogStartupIntegrityCheckNeeded(void)
{



    return (bool) mock();

}

void 
StartupXLOG_Pass2(void)
{



    mock();

}

void 
StartupXLOG_Pass3(void)
{



    mock();

}

void 
StartupXLOG_Pass4(void)
{



    mock();

}

void 
ShutdownXLOG(int code, Datum arg)
{

    check_expected(code);
    check_expected(arg);

    mock();

}

void 
InitXLOGAccess(void)
{



    mock();

}

void 
CreateCheckPoint(bool shutdown, bool force)
{

    check_expected(shutdown);
    check_expected(force);

    mock();

}

void 
XLogPutNextOid(Oid nextOid)
{

    check_expected(nextOid);

    mock();

}

XLogRecPtr 
GetRedoRecPtr(void)
{



    uint64_t        mock__tmp_r = (uint64_t) mock();
    XLogRecPtr      mock__tmp_r2;
    memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
    return mock__tmp_r2;


}

void 
GetNextXidAndEpoch(TransactionId * xid, uint32 * epoch)
{

    check_expected(xid);
    check_expected(epoch);
    optional_assignment(xid);
    optional_assignment(epoch);
    mock();

}

void 
XLogPopulateMasterMirroring(XLogRecPtr * redoCheckPointLoc)
{

    check_expected(redoCheckPointLoc);
    optional_assignment(redoCheckPointLoc);
    mock();

}

void 
XLogFileClose(void)
{



    mock();

}

void 
XLogGetBuffer(int startidx, int npages, char **from, Size * nbytes)
{

    check_expected(startidx);
    check_expected(npages);
    check_expected(from);
    check_expected(nbytes);
    optional_assignment(from);
    optional_assignment(nbytes);
    mock();

}

void 
XLogGetRecoveryStart(char *callerStr, char *reasonStr, XLogRecPtr * redoCheckPointLoc, CheckPoint * redoCheckPoint)
{

    check_expected(callerStr);
    check_expected(reasonStr);
    check_expected(redoCheckPointLoc);
    check_expected(redoCheckPoint);
    optional_assignment(callerStr);
    optional_assignment(reasonStr);
    optional_assignment(redoCheckPointLoc);
    optional_assignment(redoCheckPoint);
    mock();

}

void 
XLogScanForStandbyEndLocation(XLogRecPtr * startLoc, XLogRecPtr * endLoc)
{

    check_expected(startLoc);
    check_expected(endLoc);
    optional_assignment(startLoc);
    optional_assignment(endLoc);
    mock();

}

void 
XLogStandbyRecoverRange(XLogRecPtr * redoCheckpointLoc, CheckPoint * redoCheckPoint, XLogRecPtr * lastRecordLoc)
{

    check_expected(redoCheckpointLoc);
    check_expected(redoCheckPoint);
    check_expected(lastRecordLoc);
    optional_assignment(redoCheckpointLoc);
    optional_assignment(redoCheckPoint);
    optional_assignment(lastRecordLoc);
    mock();

}

bool 
XLogQDMirrorPositionToEnd(void)
{



    return (bool) mock();

}

bool 
XLogQDMirrorCatchup(XLogRecPtr * flushedLocation)
{

    check_expected(flushedLocation);
    optional_assignment(flushedLocation);
    return (bool) mock();

}

bool 
XLogCatchupQDMirror(XLogRecPtr * standbyLocation, XLogRecPtr * flushedLocation, struct timeval * timeout, bool * shutdownGlobal)
{

    check_expected(standbyLocation);
    check_expected(flushedLocation);
    check_expected(timeout);
    check_expected(shutdownGlobal);
    optional_assignment(standbyLocation);
    optional_assignment(flushedLocation);
    optional_assignment(timeout);
    optional_assignment(shutdownGlobal);
    return (bool) mock();

}

void 
XLogRemoveStandbyLogs(uint32 log, uint32 seg, int *nsegsremoved)
{

    check_expected(log);
    check_expected(seg);
    check_expected(nsegsremoved);
    optional_assignment(nsegsremoved);
    mock();

}

void 
XLog_OutRec(StringInfo buf, XLogRecord * record)
{

    check_expected(buf);
    check_expected(record);
    optional_assignment(record);
    mock();

}

void 
XLogPrintLogNames(void)
{



    mock();

}

char           *
XLogLocationToString(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString2(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString3(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString4(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString5(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString_Long(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString2_Long(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString3_Long(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString4_Long(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

char           *
XLogLocationToString5_Long(XLogRecPtr * loc)
{

    check_expected(loc);
    optional_assignment(loc);
    return (char *) mock();

}

void 
StartupProcessMain(int passNum)
{

    check_expected(passNum);

    mock();

}

int 
XLogReconcileEofPrimary(void)
{



    return (int) mock();

}

int 
XLogReconcileEofMirror(XLogRecPtr primaryEof, XLogRecPtr * mirrorEof)
{

    check_expected(primaryEof.xlogid);
    check_expected(primaryEof.xrecoff);
    check_expected(mirrorEof);
    optional_assignment(mirrorEof);
    return (int) mock();

}

int 
XLogRecoverMirrorControlFile(void)
{



    return (int) mock();

}

int 
XLogAddRecordsToChangeTracking(XLogRecPtr * lastChangeTrackingEndLoc)
{

    check_expected(lastChangeTrackingEndLoc);
    optional_assignment(lastChangeTrackingEndLoc);
    return (int) mock();

}

void 
XLogInChangeTrackingTransition(void)
{



    mock();

}

void 
XLogInitMirroredAlignedBuffer(int32 bufferLen)
{

    check_expected(bufferLen);

    mock();

}

void 
xlog_print_redo_read_buffer_not_found(Relation reln, BlockNumber blkno, XLogRecPtr lsn, const char *funcName)
{

    check_expected(reln);
    check_expected(blkno);
    check_expected(lsn.xlogid);
    check_expected(lsn.xrecoff);
    check_expected(funcName);

    mock();

}

void 
xlog_print_redo_lsn_application(Relation reln, BlockNumber blkno, void *page, XLogRecPtr lsn, const char *funcName)
{

    check_expected(reln);
    check_expected(blkno);
    check_expected(page);
    check_expected(lsn.xlogid);
    check_expected(lsn.xrecoff);
    check_expected(funcName);
    optional_assignment(page);
    mock();

}

XLogRecord     *
XLogReadRecord(XLogRecPtr * RecPtr, int emode)
{

    check_expected(RecPtr);
    check_expected(emode);
    optional_assignment(RecPtr);
    return (XLogRecord *) mock();

}

void 
XLogCloseReadRecord(void)
{



    mock();

}

XLogRecord     *
ReadCheckpointRecord(XLogRecPtr RecPtr, int whichChkpt)
{

    check_expected(RecPtr.xlogid);
    check_expected(RecPtr.xrecoff);
    check_expected(whichChkpt);

    return (XLogRecord *) mock();

}

void 
XLogReadRecoveryCommandFile(int emode)
{

    check_expected(emode);

    mock();

}

List           *
XLogReadTimeLineHistory(TimeLineID targetTLI)
{

    check_expected(targetTLI);

    return (List *) mock();

}

int 
XLogRecoverMirror(void)
{



    return (int) mock();

}
