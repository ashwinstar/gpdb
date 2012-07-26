/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/procarray.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <signal.h>
#include "access/distributedlog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "utils/tqual.h"
#include "access/twophase.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/combocid.h"
#include "cdb/cdbtm.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "access/xact.h"	/* setting the shared xid */
#include "cdb/cdbvars.h"

Size 
ProcArrayShmemSize(void)
{



    return (Size) mock();

}

void 
CreateSharedProcArray(void)
{



    mock();

}

void 
ProcArrayAdd(PGPROC * proc)
{

    check_expected(proc);
    optional_assignment(proc);
    mock();

}

void 
ProcArrayRemove(PGPROC * proc, bool forPrepare, bool isCommit)
{

    check_expected(proc);
    check_expected(forPrepare);
    check_expected(isCommit);
    optional_assignment(proc);
    mock();

}

bool 
TransactionIdIsInProgress(TransactionId xid)
{

    check_expected(xid);

    return (bool) mock();

}

bool 
TransactionIdIsActive(TransactionId xid)
{

    check_expected(xid);

    return (bool) mock();

}

TransactionId 
GetOldestXmin(bool allDbs)
{

    check_expected(allDbs);

    return (TransactionId) mock();

}

PGPROC         *
BackendPidGetProc(int pid)
{

    check_expected(pid);

    return (PGPROC *) mock();

}

int 
BackendXidGetPid(TransactionId xid)
{

    check_expected(xid);

    return (int) mock();

}

bool 
IsBackendPid(int pid)
{

    check_expected(pid);

    return (bool) mock();

}

bool 
DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself)
{

    check_expected(databaseId);
    check_expected(ignoreMyself);

    return (bool) mock();

}

int 
CountActiveBackends(void)
{



    return (int) mock();

}

int 
CountDBBackends(Oid databaseid)
{

    check_expected(databaseid);

    return (int) mock();

}

int 
CountUserBackends(Oid roleid)
{

    check_expected(roleid);

    return (int) mock();

}

void 
XidCacheRemoveRunningXids(TransactionId xid, int nxids, TransactionId * xids)
{

    check_expected(xid);
    check_expected(nxids);
    check_expected(xids);
    optional_assignment(xids);
    mock();

}

PGPROC         *
FindProcByGpSessionId(long gp_session_id)
{

    check_expected(gp_session_id);

    return (PGPROC *) mock();

}

void 
UpdateSerializableCommandId(void)
{



    mock();

}

struct SnapshotData *
GetSnapshotData(struct SnapshotData * snapshot, bool serializable)
{

    check_expected(snapshot);
    check_expected(serializable);
    optional_assignment(snapshot);
    mock();

}

void 
updateSharedLocalSnapshot(struct DtxContextInfo * dtxContextInfo, struct SnapshotData * snapshot, char *debugCaller)
{

    check_expected(dtxContextInfo);
    check_expected(snapshot);
    check_expected(debugCaller);
    optional_assignment(dtxContextInfo);
    optional_assignment(snapshot);
    optional_assignment(debugCaller);
    mock();

}

void 
GetSlotTableDebugInfo(void **snapshotArray, int *maxSlots)
{

    check_expected(snapshotArray);
    check_expected(maxSlots);
    optional_assignment(snapshotArray);
    optional_assignment(maxSlots);
    mock();

}

bool 
FindAndSignalProcess(int sessionId, int commandId)
{

    check_expected(sessionId);
    check_expected(commandId);

    return (bool) mock();

}
