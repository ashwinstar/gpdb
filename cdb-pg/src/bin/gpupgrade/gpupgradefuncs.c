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
#include "readfuncs_3_0.h"
#include "outfuncs_3_0.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/htup.h"
#include "catalog/pg_control.h"
#include "nodes/nodes.h"
#include "storage/backendid.h" /* MyBackendId */
#include "storage/fd.h" /* BasicOpenFile() */
#include "storage/lwlock.h"
#include "storage/sinvaladt.h" /* for cache invalidation */
#include "storage/buf.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "access/xlog_internal.h"

PG_MODULE_MAGIC;

Datum		gp_oid_update(PG_FUNCTION_ARGS);
Datum		gp_str30_2node2str(PG_FUNCTION_ARGS);
Datum		gp_str31_2node2str(PG_FUNCTION_ARGS);
Datum		gp_invalidate_syscache(PG_FUNCTION_ARGS);
Datum		gp_forget_relation(PG_FUNCTION_ARGS);
Datum		gp_downgrade_controlfile(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gp_oid_update);
PG_FUNCTION_INFO_V1(gp_str30_2node2str);
PG_FUNCTION_INFO_V1(gp_str31_2node2str);
PG_FUNCTION_INFO_V1(gp_invalidate_syscache);
PG_FUNCTION_INFO_V1(gp_forget_relation);
PG_FUNCTION_INFO_V1(gp_downgrade_controlfile);

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
    HeapTupleData   tuple;
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
	
	ItemPointerSet(&(tuple.t_self), ItemPointerGetBlockNumber(tid),
				   ItemPointerGetOffsetNumber(tid));

	if (!heap_fetch(rel, SnapshotAny, &tuple, &buf, false, NULL))
	{
		elog(ERROR, "could not locate tuple with ctid (%u, %i) in table"
			 " \"%s\"", ItemPointerGetBlockNumber(tid),
			 ItemPointerGetOffsetNumber(tid),
			 RelationGetRelationName(rel));
	}

	if (HeapTupleGetOid(&tuple) != oldoid)
		elog(ERROR, "tuple with ctid (%u, %i) in table \"%s\" has wrong "
			 "OID: expected %u, got %u", ItemPointerGetBlockNumber(tid),
			 ItemPointerGetOffsetNumber(tid),
			 RelationGetRelationName(rel),
			 oldoid,
			 HeapTupleGetOid(&tuple));

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	HeapTupleSetOid(&tuple, newoid);
	MarkBufferDirty(buf);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	elog(NOTICE, "changed tuple (%u, %i), table %s, OID %u -> OID %u",
		 ItemPointerGetBlockNumber(tid),
		 ItemPointerGetOffsetNumber(tid),
		 RelationGetRelationName(rel),
		 oldoid, newoid);

	relation_close(rel, AccessShareLock);
	/* XXX: let everyone else see the change? */
	/* CommandCounterIncrement(); */
	PG_RETURN_BOOL(true);
}

/* convert a string to a node and back to a string.
 * 'instr' is the string, if true, is30 means we're going from 3.0 -> 3.1
 * otherwise 3.1 -> 3.0.
 */
static text *
str2node2str(text *instr, bool is30)
{
	char *str;
	Node *node;
	text *outstr;
	
	str = palloc(VARSIZE(instr));
	memcpy(str, VARDATA(instr), VARSIZE(instr) - VARHDRSZ);
	str[VARSIZE(instr) - VARHDRSZ] = '\0';

	if (is30)
		node = (Node *)string30toNode(str);
	else
		node = (Node *)stringToNode(str);

	pfree(str);

	if (is30)
		str = nodeToString((void *)node);
	else
		str = nodeToString30((void *)node);

	outstr = (text *) palloc(strlen(str) + VARHDRSZ);

    memcpy(VARDATA(outstr), str, strlen(str));
    SET_VARSIZE(outstr, strlen(str) + VARHDRSZ);

	return outstr;
}

Datum
gp_str31_2node2str(PG_FUNCTION_ARGS)
{
	text *instr = PG_GETARG_TEXT_P(0);
	text *outstr;
	
	outstr = str2node2str(instr, false);
   	
	PG_RETURN_TEXT_P(outstr);
}

Datum
gp_str30_2node2str(PG_FUNCTION_ARGS)
{
	text *instr = PG_GETARG_TEXT_P(0);
	text *outstr;
	
	outstr = str2node2str(instr, true);
   	
	PG_RETURN_TEXT_P(outstr);
}

Datum
gp_invalidate_syscache(PG_FUNCTION_ARGS)
{
	SISeg *segP = shmInvalBuffer;
	ProcState  *stateP = &segP->procState[MyBackendId - 1];

	RelationCacheInvalidate();

	ResetCatalogCaches();

	LWLockAcquire(SInvalLock, LW_SHARED);
	stateP->resetState = true;
	LWLockRelease(SInvalLock);


	PG_RETURN_BOOL(true);
}

Datum
gp_forget_relation(PG_FUNCTION_ARGS)
{
	Oid relid = PG_GETARG_OID(0);
	RelationForgetRelation(relid);

	PG_RETURN_BOOL(true);
}

#define CATVERSION_3_0 200703112

Datum
gp_downgrade_controlfile(PG_FUNCTION_ARGS)
{
	int fd;
	bool found;
	/* this should be safe because we are in a running backend.. after all */
	ControlFileData *ControlFile = (ControlFileData *)
	     ShmemInitStruct("Control File", sizeof(ControlFileData), &found);

	if (!found)
		elog(ERROR, "Control File is uninitialised");

    fd = BasicOpenFile(XLOG_CONTROL_FILE,
                       O_RDWR | PG_BINARY,
                       S_IRUSR | S_IWUSR);
    if (fd < 0)
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open control file \"%s\": %m",
                        XLOG_CONTROL_FILE)));

	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
    if (read(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData
))
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not read from control file: %m")));

	ControlFile->catalog_version_no = CATVERSION_3_0;

	INIT_CRC32(ControlFile->crc);
	COMP_CRC32(ControlFile->crc,
			   (char *) ControlFile,
			   offsetof(ControlFileData, crc));
	FIN_CRC32(ControlFile->crc);

	lseek(fd, 0, SEEK_SET);
	
	errno = 0;
	if (write(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to control file: %m")));
	}

	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync control file: %m")));

	if (close(fd))
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close control file: %m")));
	LWLockRelease(ControlFileLock);

	PG_RETURN_BOOL(true);
}
