/* 
 * AOCS Segment files.
 *
 * Copyright (c) 2009, Greenplum Inc.
 */

#include "postgres.h"
#include "postgres.h"

#include "cdb/cdbappendonlystorage.h"
#include "access/aomd.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_appendonly.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbappendonlystorageread.h"
#include "cdb/cdbappendonlystoragewrite.h"
#include "utils/datumstream.h"
#include "utils/fmgroids.h"
#include "access/aocssegfiles.h"
#include "access/aosegfiles.h"
#include "cdb/cdbaocsam.h"
#include "executor/executor.h"

#include "utils/debugbreak.h"


void InsertInitialAOCSFileSegInfo(Relation prel, int4 segno, int4 nvp)
{
    bool *nulls = palloc0(sizeof(bool) * Natts_pg_aocsseg);
    Datum *values = palloc0(sizeof(Datum) * Natts_pg_aocsseg);
    AOCSVPInfo *vpinfo = create_aocs_vpinfo(nvp);

    HeapTuple segtup;

    Relation segrel = heap_open(prel->rd_rel->relaosegrelid, RowExclusiveLock);

    // Relation segidx = index_open(segrel->rd_rel->relaosegidxid, RowExclusiveLock);

    values[Anum_pg_aocs_segno-1] = Int32GetDatum(segno);
    values[Anum_pg_aocs_vpinfo-1] = PointerGetDatum(vpinfo);

    segtup = heaptuple_form_to(segrel->rd_att, values, nulls, NULL, NULL);

    frozen_heap_insert(segrel, segtup);
    CatalogUpdateIndexes(segrel, segtup);

    heap_freetuple(segtup);
    // index_close(segidx, RowExclusiveLock);
    heap_close(segrel, RowExclusiveLock);

    pfree(vpinfo);
    pfree(nulls);
    pfree(values);
}

/* 
 * Getting segment file info.
 * 
 * I assume Alon work out the locking sequence right.  The locking order is quite
 * interesting here.  See GetFileSegInfo in aosegfiles.c
 */
static AOCSFileSegInfo *GetAOCSFileSegInfo(Relation prel, int32 segno, SegfileLockStrategy lkst)
{
    /* XXX Again, here we took the way too short cut to set nvp to natts. */
    int32 nvp = prel->rd_att->natts;

    Relation segrel;
    Relation segidx;
    ScanKeyData scankey;
    IndexScanDesc scan;
    HeapTuple segtup;
    HeapTuple needfree = NULL;
    HeapTupleData segtupdata;

    AOCSFileSegInfo *seginfo;
    Datum *d;
    bool *null;

    bool forupdate = (lkst != SegfileNoLock);
    LOCKMODE lkmode = (forupdate ? RowExclusiveLock : AccessShareLock);
    Snapshot usesnapshot = (forupdate ? SnapshotSelf : GetTransactionSnapshot());

    if(forupdate)
        LWLockAcquire(AppendOnlyLock, LW_EXCLUSIVE);

    segrel = heap_open(prel->rd_rel->relaosegrelid, lkmode);
    segidx = index_open(segrel->rd_rel->relaosegidxid, lkmode);

    ScanKeyInit(&scankey, 1, BTEqualStrategyNumber, F_OIDEQ, Int32GetDatum(segno));

    scan = index_beginscan(segrel, segidx, usesnapshot, 1, &scankey);

    segtup = index_getnext(scan, ForwardScanDirection);

    if(!HeapTupleIsValid(segtup))
    {
        /* this seg does not exist */
        index_endscan(scan);
        index_close(segidx, lkmode);
        heap_close(segrel, lkmode);

        if(forupdate)
        {
#if 0
            /* XXX Alon Is this the right check?  See aosegfile.c */
            if(Gp_role == GP_ROLE_EXECUTE && !Gp_is_writer)
                elog(ERROR, "AOCS Insert must be executed by writer gang");
#endif
            AssertImply(Gp_role == GP_ROLE_EXECUTE, Gp_is_writer);

            InsertInitialAOCSFileSegInfo(prel, segno, nvp);
            LWLockRelease(AppendOnlyLock);

            return GetAOCSFileSegInfo(prel, segno, lkst);
        }
        else
            return NULL;
    }

    if(forupdate)
    {
        HTSU_Result 	test;
        Buffer buffer;
        ItemPointerData update_ctid;
        TransactionId 	update_xmax;
        LockTupleWaitType waittype;

        LWLockRelease(AppendOnlyLock);
lmark:
        segtupdata = *segtup;

        if(lkst == SegfileForceLock)
            waittype = LockTupleWait;
        else
        {
            Assert(lkst == SegfileTryLock);
            waittype = LockTupleIfNotLocked;
        }

        Assert(ItemPointerIsValid(&segtupdata.t_self));
        test = heap_lock_tuple(segrel, &segtupdata, &buffer, 
                &update_ctid, &update_xmax,
                GetCurrentCommandId(),
                LockTupleExclusive,
                waittype);

        ReleaseBuffer(buffer);

        switch(test)
        {
            case HeapTupleSelfUpdated:
                /* treat it as deleted; do not process */
                Assert(false); /* can this happen? */ 
                break;

            case HeapTupleMayBeUpdated:
                break;

            case HeapTupleUpdated:
                if (IsXactIsoLevelSerializable)
                    ereport(ERROR,
                            (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                             errmsg("could not serialize access due to concurrent update")));

                if (!ItemPointerEquals(&update_ctid, &segtupdata.t_self))
                {
                    /* updated, so look at updated version */

                    HeapTuple updatedtup;
                    updatedtup = GetUpdatedTuple_Int(segrel,
                            &update_ctid, update_xmax,
                            GetCurrentCommandId());

                    if (updatedtup)
                    {
                        /* 
                           if(needfree)
                           pfree(needfree);
                         */
                        needfree = updatedtup;

                        segtup = updatedtup;
                        goto lmark;
                    }
                }
                else
                {
                    /* no aoseg tuple should ever get deleted */
                    Insist(false);
                }

            case HeapTupleBeingUpdated:
                /*
                 * we tried to get a lock but couldn't (can only happen in
                 * LockTupleIfNotLocked waittype) then just quit and return NULL
                 */
                Assert(waittype = LockTupleIfNotLocked);
                index_endscan(scan);
                index_close(segidx, lkmode);
                heap_close(segrel, lkmode);
                return NULL;

            default:
                elog(ERROR, "unrecognized heap_lock_tuple status: %u", test);
                return NULL;
        }

    }

    seginfo = (AOCSFileSegInfo *) palloc(aocsfileseginfo_size(nvp));

    d = (Datum *) palloc(sizeof(Datum) * Natts_pg_aocsseg);
    null = (bool *) palloc(sizeof(bool) * Natts_pg_aocsseg); 

    heap_deform_tuple(segtup, segrel->rd_att, d, null);

    Assert(!null[0]);
    Assert(DatumGetInt32(d[0] == segno));
    seginfo->segno = segno;

    Assert(!null[1]);
    seginfo->tupcount = DatumGetInt64(d[1]);

    Assert(!null[2]);
    seginfo->varblockcount = DatumGetInt64(d[2]);

    Assert(!null[3]);
    {
        struct varlena *v = (struct varlena *) DatumGetPointer(d[3]);
        struct varlena *dv = pg_detoast_datum(v);

        Assert(VARSIZE(dv) == aocs_vpinfo_size(nvp));
        memcpy(&seginfo->vpinfo, dv, aocs_vpinfo_size(nvp));
        if(dv!=v)
            pfree(dv);
    }

    pfree(d);
    pfree(null);
    /*
       if(needfree)
       pfree(needfree);
     */

    index_endscan(scan);
    index_close(segidx, lkmode);
    heap_close(segrel, lkmode);

    return seginfo;
}

AOCSFileSegInfo *GetAOCSFileSegInfoForRead(Relation prel, int segno)
{
    return GetAOCSFileSegInfo(prel, segno, SegfileNoLock);
}

AOCSFileSegInfo *GetAOCSFileSegInfoForWrite(Relation prel, int segno)
{
    return GetAOCSFileSegInfo(prel, segno, SegfileForceLock);
}

AOCSFileSegInfo *GetAOCSFileSegInfoForVacuum(Relation prel, int segno)
{
    return GetAOCSFileSegInfo(prel, segno, SegfileTryLock);
}


AOCSFileSegInfo **GetAllAOCSFileSegInfo(Relation prel, Snapshot snapshot, int32 *totalseg)
{
    int32 nvp = prel->rd_att->natts;

    Relation segrel;
    HeapScanDesc scan;
    HeapTuple tup;

    AOCSFileSegInfo **allseg;
    AOCSFileSegInfo *seginfo;
    int cur_seg;
    Datum *d;
    bool *null;

    segrel = heap_open(prel->rd_rel->relaosegrelid, AccessShareLock);
    scan = heap_beginscan(segrel, snapshot, 0, NULL);

    *totalseg = 0;
    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
        ++(*totalseg);

    heap_endscan(scan);

    if(*totalseg == 0)
    {
        heap_close(segrel, AccessShareLock);
        return NULL;
    }

    allseg = (AOCSFileSegInfo **) palloc0(sizeof(AOCSFileSegInfo*) * (*totalseg));
    d = (Datum *) palloc(sizeof(Datum) * Natts_pg_aocsseg);
    null = (bool *) palloc(sizeof(bool) * Natts_pg_aocsseg);

    cur_seg = 0;

    scan = heap_beginscan(segrel, snapshot, 0, NULL);
    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        seginfo = (AOCSFileSegInfo *) palloc(aocsfileseginfo_size(nvp));

        Assert(cur_seg < *totalseg);
        allseg[cur_seg] = seginfo;

        heap_deform_tuple(tup, segrel->rd_att, d, null);

        Assert(!null[0]);
        seginfo->segno = DatumGetInt32(d[0]);

        Assert(!null[1]);
        seginfo->tupcount = DatumGetInt64(d[1]);

        Assert(!null[2]);
        seginfo->varblockcount = DatumGetInt64(d[2]);

        Assert(!null[3]);
        {
            struct varlena *v = (struct varlena *) DatumGetPointer(d[3]);
            struct varlena *dv = pg_detoast_datum(v);

            Assert(VARSIZE(dv) == aocs_vpinfo_size(nvp));
            memcpy(&seginfo->vpinfo, dv, aocs_vpinfo_size(nvp));
            if(dv!=v)
                pfree(dv);
        }
        ++cur_seg;
    }

    pfree(d);
    pfree(null);

    heap_endscan(scan);
    heap_close(segrel, AccessShareLock);

    return allseg;
}


void UpdateAOCSFileSegInfo(AOCSInsertDesc idesc)
{
    Relation prel = idesc->aoi_rel;
    Relation segrel;
    Relation segidx;

    ScanKeyData key;
    IndexScanDesc scan;

    HeapTuple oldtup;
    HeapTuple newtup;
    Datum d[Natts_pg_aocsseg];
    bool null[Natts_pg_aocsseg] = {0, };
    bool repl[Natts_pg_aocsseg] = {0, };

    TupleDesc tupdesc; 
    int nvp = prel->rd_att->natts;
    int i;
    AOCSVPInfo *vpinfo = create_aocs_vpinfo(nvp);

    Snapshot usesnapshot = SnapshotDirty;

    segrel = heap_open(prel->rd_rel->relaosegrelid, RowExclusiveLock);
    segidx = index_open(segrel->rd_rel->relaosegidxid, RowExclusiveLock);
    tupdesc = segrel->rd_att;

    ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(idesc->cur_segno)); 
    scan = index_beginscan(segrel, segidx, usesnapshot, 1, &key);

    oldtup = index_getnext(scan, ForwardScanDirection);

    if(!HeapTupleIsValid(oldtup))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("AOCS table \"%s\" file segment \"%d\" does not exist",
                        RelationGetRelationName(prel), idesc->cur_segno)
                    ));

#ifdef USE_ASSERT_CHECKING
    d[Anum_pg_aocs_segno-1] = fastgetattr(oldtup, Anum_pg_aocs_segno, tupdesc, &null[Anum_pg_aoseg_segno-1]);
    Assert(!null[Anum_pg_aocs_segno-1]);
    Assert(DatumGetInt32(d[Anum_pg_aocs_segno-1]) == idesc->cur_segno);
#endif

    d[Anum_pg_aocs_tupcount-1] = fastgetattr(oldtup, Anum_pg_aocs_tupcount, tupdesc, &null[Anum_pg_aoseg_tupcount-1]);
    Assert(!null[Anum_pg_aocs_tupcount-1]);

    d[Anum_pg_aocs_tupcount-1] += idesc->insertCount;
    repl[Anum_pg_aocs_tupcount-1] = true;


    d[Anum_pg_aocs_varblockcount-1] = fastgetattr(oldtup, Anum_pg_aocs_varblockcount, tupdesc, &null[Anum_pg_aoseg_varblockcount-1]);
    Assert(!null[Anum_pg_aocs_varblockcount-1]);
    d[Anum_pg_aocs_varblockcount-1] += idesc->varblockCount;
    repl[Anum_pg_aocs_varblockcount-1] = true;

    for(i=0; i<nvp; ++i)
    {
        vpinfo->entry[i].eof = idesc->ds[i]->eof;
        vpinfo->entry[i].eof_uncompressed = idesc->ds[i]->eofUncompress;
    }
    d[Anum_pg_aocs_vpinfo-1] = PointerGetDatum(vpinfo);
    null[Anum_pg_aocs_vpinfo-1] = false;
    repl[Anum_pg_aocs_vpinfo-1] = true;

    newtup = heap_modify_tuple(oldtup, tupdesc, d, null, repl);

    simple_heap_update(segrel, &oldtup->t_self, newtup);
    CatalogUpdateIndexes(segrel, newtup);

    pfree(newtup);
    pfree(vpinfo);

    index_endscan(scan);
    index_close(segidx, RowExclusiveLock);
    heap_close(segrel, RowExclusiveLock);
}

void AOCSFileSegInfoAddCount(Relation prel, int32 segno, int64 tupadded, int64 varblockadded)
{
    Relation segrel;
    Relation segidx;

    ScanKeyData key;
    IndexScanDesc scan;

    HeapTuple oldtup;
    HeapTuple newtup;
    Datum d[Natts_pg_aocsseg];
    bool null[Natts_pg_aocsseg] = {0, };
    bool repl[Natts_pg_aocsseg] = {0, };

    TupleDesc tupdesc; 

    Snapshot usesnapshot = SnapshotDirty;

    segrel = heap_open(prel->rd_rel->relaosegrelid, RowExclusiveLock);
    segidx = index_open(segrel->rd_rel->relaosegidxid, RowExclusiveLock);

    tupdesc = segrel->rd_att;

    ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(segno));
    scan = index_beginscan(segrel, segidx, usesnapshot, 1, &key);

    oldtup = index_getnext(scan, ForwardScanDirection);

    if(!HeapTupleIsValid(oldtup))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("AOCS table \"%s\" file segment \"%d\" does not exist",
                        RelationGetRelationName(prel), segno)
                    ));

#ifdef USE_ASSERT_CHECKING
    d[Anum_pg_aocs_segno-1] = fastgetattr(oldtup, Anum_pg_aocs_segno, tupdesc, &null[Anum_pg_aoseg_segno-1]);
    Assert(!null[Anum_pg_aocs_segno-1]);
    Assert(DatumGetInt32(d[Anum_pg_aocs_segno-1]) == segno);
#endif

    d[Anum_pg_aocs_tupcount-1] = fastgetattr(oldtup, Anum_pg_aocs_tupcount, tupdesc, &null[Anum_pg_aoseg_tupcount-1]);
    Assert(!null[Anum_pg_aocs_tupcount-1]);

    d[Anum_pg_aocs_tupcount-1] += tupadded;
    repl[Anum_pg_aocs_tupcount-1] = true;

    d[Anum_pg_aocs_varblockcount-1] = fastgetattr(oldtup, Anum_pg_aocs_varblockcount, tupdesc, &null[Anum_pg_aoseg_varblockcount-1]);
    Assert(!null[Anum_pg_aocs_varblockcount-1]);
    d[Anum_pg_aocs_varblockcount-1] += varblockadded;
    repl[Anum_pg_aocs_tupcount-1] = true;

    newtup = heap_modify_tuple(oldtup, tupdesc, d, null, repl);

    simple_heap_update(segrel, &oldtup->t_self, newtup);
    CatalogUpdateIndexes(segrel, newtup);

    pfree(newtup);

    index_endscan(scan);
    index_close(segidx, RowExclusiveLock);
    heap_close(segrel, RowExclusiveLock);
}

extern Datum aocsvpinfo_decode(PG_FUNCTION_ARGS);
Datum aocsvpinfo_decode(PG_FUNCTION_ARGS)
{
    AOCSVPInfo *vpinfo = (AOCSVPInfo *) PG_GETARG_BYTEA_P(0);
    int i = PG_GETARG_INT32(1);
    int j = PG_GETARG_INT32(2);
    int64 result;

    if (i<0 || i>=vpinfo->nEntry)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid entry for decoding aocsvpinfo")
                    ));

    if (j<0 || j>1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid entry for decoding aocsvpinfo")
                    ));

    if (j==0)
        result = vpinfo->entry[i].eof;
    else
        result = vpinfo->entry[i].eof_uncompressed;

    PG_RETURN_INT64(result);
}
