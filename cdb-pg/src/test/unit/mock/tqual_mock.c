/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/tqual.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/tqual.h"
#include "access/twophase.h"	/* max_prepared_xacts */
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "storage/procarray.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "access/clog.h"
#include "storage/buffile.h"
#include "access/distributedlog.h"

Snapshot        SnapshotDirty;
Snapshot        SerializableSnapshot;
Snapshot        LatestSnapshot;
Snapshot        ActiveSnapshot;
TransactionId   TransactionXmin;
TransactionId   RecentXmin;
TransactionId   RecentGlobalXmin;
volatile SharedSnapshotSlot *SharedLocalSnapshotSlot;
Size 
SharedSnapshotShmemSize(void)
{



    return (Size) mock();

}

void 
CreateSharedSnapshotArray(void)
{



    mock();

}

char           *
SharedSnapshotDump(void)
{



    return (char *) mock();

}

void 
SharedSnapshotRemove(volatile SharedSnapshotSlot * slot, char *creatorDescription)
{

    check_expected(slot);
    check_expected(creatorDescription);
    optional_assignment(slot);
    optional_assignment(creatorDescription);
    mock();

}

void 
addSharedSnapshot(char *creatorDescription, int id)
{

    check_expected(creatorDescription);
    check_expected(id);
    optional_assignment(creatorDescription);
    mock();

}

void 
lookupSharedSnapshot(char *lookerDescription, char *creatorDescription, int id)
{

    check_expected(lookerDescription);
    check_expected(creatorDescription);
    check_expected(id);
    optional_assignment(lookerDescription);
    optional_assignment(creatorDescription);
    mock();

}

void 
dumpSharedLocalSnapshot_forCursor(void)
{



    mock();

}

void 
readSharedLocalSnapshot_forCursor(Snapshot snapshot)
{

    check_expected(snapshot);

    mock();

}

bool 
HeapTupleSatisfiesItself(Relation relation, ItemPointer tid, HeapTupleHeader tuple, Buffer buffer, char *caller)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(tuple);
    check_expected(buffer);
    check_expected(caller);
    optional_assignment(caller);
    return (bool) mock();

}

bool 
HeapTupleSatisfiesNow(Relation relation, ItemPointer tid, HeapTupleHeader tuple, Buffer buffer, char *caller)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(tuple);
    check_expected(buffer);
    check_expected(caller);
    optional_assignment(caller);
    return (bool) mock();

}

bool 
HeapTupleSatisfiesDirty(Relation relation, ItemPointer tid, HeapTupleHeader tuple, Buffer buffer, char *caller)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(tuple);
    check_expected(buffer);
    check_expected(caller);
    optional_assignment(caller);
    return (bool) mock();

}

bool 
HeapTupleSatisfiesToast(Relation relation, ItemPointer tid, HeapTupleHeader tuple, Buffer buffer, char *caller)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(tuple);
    check_expected(buffer);
    check_expected(caller);
    optional_assignment(caller);
    return (bool) mock();

}

bool 
HeapTupleSatisfiesSnapshot(Relation relation, ItemPointer tid, HeapTupleHeader tuple, Snapshot snapshot, Buffer buffer, char *caller)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(tuple);
    check_expected(snapshot);
    check_expected(buffer);
    check_expected(caller);
    optional_assignment(caller);
    return (bool) mock();

}

HTSU_Result 
HeapTupleSatisfiesUpdate(Relation relation, ItemPointer tid, HeapTupleHeader tuple, CommandId curcid, Buffer buffer, char *caller)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(tuple);
    check_expected(curcid);
    check_expected(buffer);
    check_expected(caller);
    optional_assignment(caller);
    return (HTSU_Result) mock();

}

char           *
HeapTupleSatisfiesUpdate_String(HTSU_Result result)
{

    check_expected(result);

    return (char *) mock();

}

HTSV_Result 
HeapTupleSatisfiesVacuum(Relation relation, ItemPointer tid, HeapTupleHeader tuple, TransactionId OldestXmin, Buffer buffer, bool vacuumFull, char *caller)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(tuple);
    check_expected(OldestXmin);
    check_expected(buffer);
    check_expected(vacuumFull);
    check_expected(caller);
    optional_assignment(caller);
    return (HTSV_Result) mock();

}

char           *
HeapTupleSatisfiesVacuum_String(HTSV_Result result)
{

    check_expected(result);

    return (char *) mock();

}

Snapshot 
GetTransactionSnapshot(void)
{



    return (Snapshot) mock();

}

Snapshot 
GetLatestSnapshot(void)
{



    return (Snapshot) mock();

}

Snapshot 
CopySnapshot(Snapshot snapshot)
{

    check_expected(snapshot);

    return (Snapshot) mock();

}

void 
FreeSnapshot(Snapshot snapshot)
{

    check_expected(snapshot);

    mock();

}

void 
FreeXactSnapshot(void)
{



    mock();

}

void 
LogDistributedSnapshotInfo(Snapshot snapshot, const char *prefix)
{

    check_expected(snapshot);
    check_expected(prefix);

    mock();

}

void 
GetTupleVisibilitySummary(HeapTuple tuple, TupleVisibilitySummary * tupleVisibilitySummary)
{

    check_expected(tuple);
    check_expected(tupleVisibilitySummary);
    optional_assignment(tupleVisibilitySummary);
    mock();

}

void 
GetTupleVisibilitySummaryDatums(Datum * values, bool * nulls, TupleVisibilitySummary * tupleVisibilitySummary)
{

    check_expected(values);
    check_expected(nulls);
    check_expected(tupleVisibilitySummary);
    optional_assignment(values);
    optional_assignment(nulls);
    optional_assignment(tupleVisibilitySummary);
    mock();

}

char           *
GetTupleVisibilitySummaryString(TupleVisibilitySummary * tupleVisibilitySummary)
{

    check_expected(tupleVisibilitySummary);
    optional_assignment(tupleVisibilitySummary);
    return (char *) mock();

}
