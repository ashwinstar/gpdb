/*-------------------------------------------------------------------------
 *
 * gpupgradefuncs.c
 *
 *
 * Copyright (c) 2007 Greenplum
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h" /* for PG_MODULE_MAGIC */

#include "access/heapam.h"
#include "access/htup.h"
#include "storage/buf.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

Datum		gp_oid_update(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gp_oid_update);

/* we must define these because they're private to tid.c :-( */
#define DatumGetItemPointer(X)	 ((ItemPointer) DatumGetPointer(X))
#define ItemPointerGetDatum(X)	 PointerGetDatum(X)
#define PG_GETARG_ITEMPOINTER(n) DatumGetItemPointer(PG_GETARG_DATUM(n))
#define PG_RETURN_ITEMPOINTER(x) return ItemPointerGetDatum(x)


/*
 * gp_oid_update(oldoid, newoid, tid, tableoid)
 * 
 * change oldoid to newoid in table. We locate the tuple by tid to avoid
 * the OID swap problem -- that is, A wants OID1 -> OID2; B wants
 * OID2 -> OID1. We'll have a situation where there are two OID2s for
 * a while.
 */
Datum
gp_oid_update(PG_FUNCTION_ARGS)
{
	Relation rel;
    HeapTuple   tuple;
	Oid oldoid = PG_GETARG_OID(0);
	Oid newoid = PG_GETARG_OID(1);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(2);
	Oid relid = PG_GETARG_OID(3);
	Buffer buf;
    
    /* 
	 * Open relation using relid, get tupdesc. AccessShareLock seems fine as
	 * we're in upgrade mode.
	 */
    rel = relation_open(relid, AccessShareLock);

	if (rel->rd_rel->relhasoids == false)
		elog(ERROR, "relation \"%s\" does not have OIDs",
			 RelationGetRelationName(rel));
	tuple = palloc0(sizeof(HeapTuple));

	ItemPointerSet(&(tuple->t_self), ItemPointerGetBlockNumber(tid),
				   ItemPointerGetOffsetNumber(tid));

	if (!heap_fetch(rel, SnapshotNow, tuple, &buf, false, NULL))
	{
		elog(ERROR, "could not locate tuple with ctid (%u, %i) in table"
			 " \"%s\"", ItemPointerGetBlockNumber(tid),
			 ItemPointerGetOffsetNumber(tid),
			 RelationGetRelationName(rel));
	}

	Assert(HeapTupleIsValid(tuple));
	
	if (HeapTupleGetOid(tuple) != oldoid)
		elog(ERROR, "tuple with ctid (%u, %i) in table \"%s\" has wrong "
			 "OID: expected %u, got %u", ItemPointerGetBlockNumber(tid),
			 ItemPointerGetOffsetNumber(tid),
			 RelationGetRelationName(rel),
			 oldoid,
			 HeapTupleGetOid(tuple));

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	HeapTupleSetOid(tuple, newoid);
	MarkBufferDirty(buf);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	relation_close(rel, AccessShareLock);
	PG_RETURN_BOOL(true);
}
