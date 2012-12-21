/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/lmgr.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/gp_policy.h"	/* CDB: POLICYTYPE_PARTiITIONED */
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"	/* CDB: get_rel_name() */
#include "cdb/cdbvars.h"

void 
RelationInitLockInfo(Relation relation)
{

    check_expected(relation);

    mock();

}

void 
LockRelationOid(Oid relid, LOCKMODE lockmode)
{

    check_expected(relid);
    check_expected(lockmode);

    mock();

}

bool 
ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode)
{

    check_expected(relid);
    check_expected(lockmode);

    return (bool) mock();

}

void 
UnlockRelationId(LockRelId * relid, LOCKMODE lockmode)
{

    check_expected(relid);
    check_expected(lockmode);
    optional_assignment(relid);
    mock();

}

void 
UnlockRelationOid(Oid relid, LOCKMODE lockmode)
{

    check_expected(relid);
    check_expected(lockmode);

    mock();

}

void 
LockRelation(Relation relation, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(lockmode);

    mock();

}

bool 
ConditionalLockRelation(Relation relation, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(lockmode);

    return (bool) mock();

}

void 
UnlockRelation(Relation relation, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(lockmode);

    mock();

}

void 
LockRelationIdForSession(LockRelId * relid, LOCKMODE lockmode)
{

    check_expected(relid);
    check_expected(lockmode);
    optional_assignment(relid);
    mock();

}

void 
UnlockRelationIdForSession(LockRelId * relid, LOCKMODE lockmode)
{

    check_expected(relid);
    check_expected(lockmode);
    optional_assignment(relid);
    mock();

}

void 
LockRelationForExtension(Relation relation, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(lockmode);

    mock();

}

void 
UnlockRelationForExtension(Relation relation, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(lockmode);

    mock();

}

void 
LockRelationForResyncExtension(RelFileNode * relFileNode, LOCKMODE lockmode)
{

    check_expected(relFileNode);
    check_expected(lockmode);
    optional_assignment(relFileNode);
    mock();

}

void 
UnlockRelationForResyncExtension(RelFileNode * relFileNode, LOCKMODE lockmode)
{

    check_expected(relFileNode);
    check_expected(lockmode);
    optional_assignment(relFileNode);
    mock();

}

void 
LockRelationForResynchronize(RelFileNode * relFileNode, LOCKMODE lockmode)
{

    check_expected(relFileNode);
    check_expected(lockmode);
    optional_assignment(relFileNode);
    mock();

}

void 
UnlockRelationForResynchronize(RelFileNode * relFileNode, LOCKMODE lockmode)
{

    check_expected(relFileNode);
    check_expected(lockmode);
    optional_assignment(relFileNode);
    mock();

}

LockAcquireResult 
LockRelationAppendOnlySegmentFile(RelFileNode * relFileNode, int32 segno, LOCKMODE lockmode, bool dontWait)
{

    check_expected(relFileNode);
    check_expected(segno);
    check_expected(lockmode);
    check_expected(dontWait);
    optional_assignment(relFileNode);
    return (LockAcquireResult) mock();

}

void 
UnlockRelationAppendOnlySegmentFile(RelFileNode * relFileNode, int32 segno, LOCKMODE lockmode)
{

    check_expected(relFileNode);
    check_expected(segno);
    check_expected(lockmode);
    optional_assignment(relFileNode);
    mock();

}

void 
LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(blkno);
    check_expected(lockmode);

    mock();

}

bool 
ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(blkno);
    check_expected(lockmode);

    return (bool) mock();

}

void 
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(blkno);
    check_expected(lockmode);

    mock();

}

void 
LockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(lockmode);

    mock();

}

bool 
ConditionalLockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(lockmode);

    return (bool) mock();

}

void 
UnlockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{

    check_expected(relation);
    check_expected(tid);
    check_expected(lockmode);

    mock();

}

void 
XactLockTableInsert(TransactionId xid)
{

    check_expected(xid);

    mock();

}

void 
XactLockTableDelete(TransactionId xid)
{

    check_expected(xid);

    mock();

}

void 
XactLockTableWait(TransactionId xid)
{

    check_expected(xid);

    mock();

}

bool 
ConditionalXactLockTableWait(TransactionId xid)
{

    check_expected(xid);

    return (bool) mock();

}

void 
LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid, LOCKMODE lockmode)
{

    check_expected(classid);
    check_expected(objid);
    check_expected(objsubid);
    check_expected(lockmode);

    mock();

}

void 
UnlockDatabaseObject(Oid classid, Oid objid, uint16 objsubid, LOCKMODE lockmode)
{

    check_expected(classid);
    check_expected(objid);
    check_expected(objsubid);
    check_expected(lockmode);

    mock();

}

void 
LockSharedObject(Oid classid, Oid objid, uint16 objsubid, LOCKMODE lockmode)
{

    check_expected(classid);
    check_expected(objid);
    check_expected(objsubid);
    check_expected(lockmode);

    mock();

}

void 
UnlockSharedObject(Oid classid, Oid objid, uint16 objsubid, LOCKMODE lockmode)
{

    check_expected(classid);
    check_expected(objid);
    check_expected(objsubid);
    check_expected(lockmode);

    mock();

}

bool 
LockTagIsTemp(const LOCKTAG * tag)
{

    check_expected(tag);

    return (bool) mock();

}

void 
LockDatabaseMaintenance(Oid dbid, LOCKMODE lockmode)
{

    check_expected(dbid);
    check_expected(lockmode);

    mock();

}

void 
UnlockDatabaseMaintenance(Oid dbid, LOCKMODE lockmode)
{

    check_expected(dbid);
    check_expected(lockmode);

    mock();

}
