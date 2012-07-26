/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbmirroredappendonly.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include "access/xlogmm.h"
#include "utils/palloc.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbmirroredappendonly.h"
#include "storage/fd.h"
#include "catalog/catalog.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbfilerepprimary.h"
#include "storage/smgr.h"
#include "storage/lwlock.h"
#include "utils/guc.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentrecovery.h"

void 
MirroredAppendOnly_Create(RelFileNode * relFileNode, int32 segmentFileNum, char *relationName, MirrorDataLossTrackingState mirrorDataLossTrackingState, int64 mirrorDataLossTrackingSessionNum, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relationName);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
MirroredAppendOnly_MirrorReCreate(RelFileNode * relFileNode, int32 segmentFileNum, MirrorDataLossTrackingState mirrorDataLossTrackingState, int64 mirrorDataLossTrackingSessionNum, bool * mirrorDataLossOccurred)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relFileNode);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
MirroredAppendOnly_OpenReadWrite(MirroredAppendOnlyOpen * open, RelFileNode * relFileNode, int32 segmentFileNum, char *relationName, int64 logicalEof, bool traceOpenFlags, ItemPointer persistentTid, int64 persistentSerialNum, int *primaryError)
{

    check_expected(open);
    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relationName);
    check_expected(logicalEof);
    check_expected(traceOpenFlags);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(primaryError);
    optional_assignment(open);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(primaryError);
    mock();

}

void 
MirroredAppendOnly_AddMirrorResyncEofs(RelFileNode * relFileNode, int32 segmentFileNum, char *relationName, ItemPointer persistentTid, int64 persistentSerialNum, MirroredLockLocalVars * mirroredLockByRefVars, bool originalMirrorCatchupRequired, MirrorDataLossTrackingState originalMirrorDataLossTrackingState, int64 originalMirrorDataLossTrackingSessionNum, int64 mirrorNewEof)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(mirroredLockByRefVars);
    check_expected(originalMirrorCatchupRequired);
    check_expected(originalMirrorDataLossTrackingState);
    check_expected(originalMirrorDataLossTrackingSessionNum);
    check_expected(mirrorNewEof);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(mirroredLockByRefVars);
    mock();

}

void 
MirroredAppendOnly_EndXactCatchup(int entryIndex, RelFileNode * relFileNode, int32 segmentFileNum, int nestLevel, char *relationName, ItemPointer persistentTid, int64 persistentSerialNum, MirroredLockLocalVars * mirroredLockVarsByRef, bool lastMirrorCatchupRequired, MirrorDataLossTrackingState lastMirrorDataLossTrackingState, int64 lastMirrorDataLossTrackingSessionNum, int64 mirrorNewEof)
{

    check_expected(entryIndex);
    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(nestLevel);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(mirroredLockVarsByRef);
    check_expected(lastMirrorCatchupRequired);
    check_expected(lastMirrorDataLossTrackingState);
    check_expected(lastMirrorDataLossTrackingSessionNum);
    check_expected(mirrorNewEof);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(mirroredLockVarsByRef);
    mock();

}

void 
MirroredAppendOnly_OpenResynchonize(MirroredAppendOnlyOpen * open, RelFileNode * relFileNode, int32 segmentFileNum, int64 logicalEof, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(open);
    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(logicalEof);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(open);
    optional_assignment(relFileNode);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

bool 
MirroredAppendOnly_IsActive(MirroredAppendOnlyOpen * open)
{

    check_expected(open);
    optional_assignment(open);
    return (bool) mock();

}

void 
MirroredAppendOnly_FlushAndClose(MirroredAppendOnlyOpen * open, int *primaryError, bool * mirrorDataLossOccurred, bool * mirrorCatchupRequired, MirrorDataLossTrackingState * originalMirrorDataLossTrackingState, int64 * originalMirrorDataLossTrackingSessionNum)
{

    check_expected(open);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    check_expected(mirrorCatchupRequired);
    check_expected(originalMirrorDataLossTrackingState);
    check_expected(originalMirrorDataLossTrackingSessionNum);
    optional_assignment(open);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    optional_assignment(mirrorCatchupRequired);
    optional_assignment(originalMirrorDataLossTrackingState);
    optional_assignment(originalMirrorDataLossTrackingSessionNum);
    mock();

}

void 
MirroredAppendOnly_Flush(MirroredAppendOnlyOpen * open, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(open);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(open);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
MirroredAppendOnly_Close(MirroredAppendOnlyOpen * open, bool * mirrorDataLossOccurred)
{

    check_expected(open);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(open);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
MirroredAppendOnly_Drop(RelFileNode * relFileNode, int32 segmentFileNum, char *relationName, bool primaryOnly, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relationName);
    check_expected(primaryOnly);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
MirroredAppendOnly_MirrorReDrop(RelFileNode * relFileNode, int32 segmentFileNum, MirrorDataLossTrackingState mirrorDataLossTrackingState, int64 mirrorDataLossTrackingSessionNum, bool * mirrorDataLossOccurred)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relFileNode);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
MirroredAppendOnly_Append(MirroredAppendOnlyOpen * open, void *appendData, int32 appendDataLen, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(open);
    check_expected(appendData);
    check_expected(appendDataLen);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(open);
    optional_assignment(appendData);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
MirroredAppendOnly_Truncate(MirroredAppendOnlyOpen * open, int64 position, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(open);
    check_expected(position);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(open);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

int 
MirroredAppendOnly_Read(MirroredAppendOnlyOpen * open, void *buffer, int32 bufferLen)
{

    check_expected(open);
    check_expected(buffer);
    check_expected(bufferLen);
    optional_assignment(open);
    optional_assignment(buffer);
    return (int) mock();

}
