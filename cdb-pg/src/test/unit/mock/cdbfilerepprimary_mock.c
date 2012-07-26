/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbfilerepprimary.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include "miscadmin.h"
#include "access/xlog_internal.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerepconnclient.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbvars.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/ipc.h"
#include "postmaster/primary_mirror_mode.h"
#include "utils/faultinjector.h"
#include "utils/memutils.h"

bool 
FileRepPrimary_IsResyncWorker(void)
{



    return (bool) mock();

}

bool 
FileRepPrimary_IsResyncManagerOrWorker(void)
{



    return (bool) mock();

}

MirrorDataLossTrackingState 
FileRepPrimary_GetMirrorDataLossTrackingSessionNum(int64 * sessionNum)
{

    check_expected(sessionNum);
    optional_assignment(sessionNum);
    return (MirrorDataLossTrackingState) mock();

}

bool 
FileRepPrimary_IsMirrorDataLossOccurred(void)
{



    return (bool) mock();

}

int 
FileRepPrimary_IntentAppendOnlyCommitWork(void)
{



    return (int) mock();

}

int 
FileRepPrimary_FinishedAppendOnlyCommitWork(int count)
{

    check_expected(count);

    return (int) mock();

}

int 
FileRepPrimary_GetAppendOnlyCommitWorkCount(void)
{



    return (int) mock();

}

int 
FileRepPrimary_MirrorOpen(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType, int64 logicalEof, int fileFlags, int fileMode, bool suppressError)
{

    check_expected(fileRepRelationType);
    check_expected(logicalEof);
    check_expected(fileFlags);
    check_expected(fileMode);
    check_expected(suppressError);

    return (int) mock();

}

int 
FileRepPrimary_MirrorFlush(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (int) mock();

}

int 
FileRepPrimary_MirrorClose(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (int) mock();

}

int 
FileRepPrimary_MirrorFlushAndClose(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (int) mock();

}

int 
FileRepPrimary_MirrorTruncate(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType, int64 position)
{

    check_expected(fileRepRelationType);
    check_expected(position);

    return (int) mock();

}

int 
FileRepPrimary_ReconcileXLogEof(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType, XLogRecPtr primaryXLogEof)
{

    check_expected(fileRepRelationType);
    check_expected(primaryXLogEof.xlogid);
    check_expected(primaryXLogEof.xrecoff);

    return (int) mock();

}

int 
FileRepPrimary_MirrorRename(FileRepIdentifier_u oldFileRepIdentifier, FileRepIdentifier_u newFileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (int) mock();

}

int 
FileRepPrimary_MirrorWrite(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType, int32 offset, char *data, uint32 dataLength, XLogRecPtr lsn)
{

    check_expected(fileRepRelationType);
    check_expected(offset);
    check_expected(data);
    check_expected(dataLength);
    check_expected(lsn.xlogid);
    check_expected(lsn.xrecoff);
    optional_assignment(data);
    return (int) mock();

}

int 
FileRepPrimary_MirrorCreate(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType, bool reportStatus)
{

    check_expected(fileRepRelationType);
    check_expected(reportStatus);

    return (int) mock();

}

int 
FileRepPrimary_MirrorDrop(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (int) mock();

}

int 
FileRepPrimary_MirrorDropFilesFromDir(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (int) mock();

}

int 
FileRepPrimary_MirrorDropTemporaryFiles(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (int) mock();

}

int 
FileRepPrimary_MirrorValidation(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType, FileRepValidationType_e fileRepValidationType)
{

    check_expected(fileRepRelationType);
    check_expected(fileRepValidationType);

    return (int) mock();

}

bool 
FileRepPrimary_IsOperationCompleted(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType)
{

    check_expected(fileRepRelationType);

    return (bool) mock();

}

XLogRecPtr 
FileRepPrimary_GetMirrorXLogEof(void)
{



    uint64_t        mock__tmp_r = (uint64_t) mock();
    XLogRecPtr      mock__tmp_r2;
    memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
    return mock__tmp_r2;


}

int 
FileRepPrimary_GetMirrorStatus(void)
{



    return (int) mock();

}

void 
FileRepPrimary_MirrorShutdown(void)
{



    mock();

}

void 
FileRepPrimary_MirrorInSyncTransition(void)
{



    mock();

}

void 
FileRepPrimary_MirrorHeartBeat(FileRepConsumerProcIndex_e index)
{

    check_expected(index);

    mock();

}

int 
FileRepPrimary_MirrorVerify(FileRepIdentifier_u fileRepIdentifier, FileRepRelationType_e fileRepRelationType, FileRepOperationDescription_u fileRepOperationDescription, char *data, uint32 dataLength, void **responseData, uint32 * responseDataLength, FileRepOperationDescription_u * responseDesc)
{

    check_expected(fileRepRelationType);
    check_expected(data);
    check_expected(dataLength);
    check_expected(responseData);
    check_expected(responseDataLength);
    check_expected(responseDesc);
    optional_assignment(data);
    optional_assignment(responseData);
    optional_assignment(responseDataLength);
    optional_assignment(responseDesc);
    return (int) mock();

}

void 
FileRepPrimary_StartSender(void)
{



    mock();

}
