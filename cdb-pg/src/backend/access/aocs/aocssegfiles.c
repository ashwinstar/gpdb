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
#include "catalog/gp_fastsequence.h"
#include "cdb/cdbvars.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#include "utils/numeric.h"
#include "cdb/cdbappendonlyblockdirectory.h"
#include "cdb/cdbappendonlystoragelayer.h"
#include "cdb/cdbappendonlystorageread.h"
#include "cdb/cdbappendonlystoragewrite.h"
#include "utils/datumstream.h"
#include "utils/fmgroids.h"
#include "access/aocssegfiles.h"
#include "access/aosegfiles.h"
#include "cdb/cdbaocsam.h"
#include "executor/executor.h"

#include "utils/debugbreak.h"


void InsertInitialAOCSFileSegInfo(Oid segrelid, int4 segno, int4 nvp)
{
    bool *nulls = palloc0(sizeof(bool) * Natts_pg_aocsseg);
    Datum *values = palloc0(sizeof(Datum) * Natts_pg_aocsseg);
    AOCSVPInfo *vpinfo = create_aocs_vpinfo(nvp);
    HeapTuple segtup;
	ItemPointerData tid;
	Relation segrel;

    segrel = heap_open(segrelid, RowExclusiveLock);

	InsertFastSequenceEntry(segrelid,
							(int64)segno,
							0,
							&tid);

    values[Anum_pg_aocs_segno-1] = Int32GetDatum(segno);
    values[Anum_pg_aocs_vpinfo-1] = PointerGetDatum(vpinfo);

    segtup = heaptuple_form_to(segrel->rd_att, values, nulls, NULL, NULL);

    frozen_heap_insert(segrel, segtup);
    CatalogUpdateIndexes(segrel, segtup);

    heap_freetuple(segtup);
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
static AOCSFileSegInfo *GetAOCSFileSegInfo(Relation prel,
										   AppendOnlyEntry *aoEntry,
										   int32 segno,
										   SegfileLockStrategy lkst)
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

    segrel = heap_open(aoEntry->segrelid, lkmode);
    segidx = index_open(aoEntry->segidxid, lkmode);

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

            InsertInitialAOCSFileSegInfo(aoEntry->segrelid, segno, nvp);
            LWLockRelease(AppendOnlyLock);

            return GetAOCSFileSegInfo(prel, aoEntry, segno, lkst);
        }
        else
            return NULL;
    }

	/* Close the index */
    segtup = heap_copytuple(segtup);
	
	index_endscan(scan);
	index_close(segidx, lkmode);

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

    Assert(!null[Anum_pg_aocs_segno - 1]);
    Assert(DatumGetInt32(d[Anum_pg_aocs_segno - 1] == segno));
    seginfo->segno = segno;

    Assert(!null[Anum_pg_aocs_tupcount - 1]);
    seginfo->tupcount = DatumGetInt64(d[Anum_pg_aocs_tupcount - 1]);

    Assert(!null[Anum_pg_aocs_varblockcount - 1]);
    seginfo->varblockcount = DatumGetInt64(d[Anum_pg_aocs_varblockcount - 1]);

	ItemPointerSetInvalid(&seginfo->sequence_tid);

    Assert(!null[Anum_pg_aocs_vpinfo - 1]);
    {
        struct varlena *v = (struct varlena *) DatumGetPointer(d[Anum_pg_aocs_vpinfo - 1]);
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

    heap_close(segrel, lkmode);

    return seginfo;
}

AOCSFileSegInfo *GetAOCSFileSegInfoForRead(Relation prel, AppendOnlyEntry *aoEntry, int segno)
{
    return GetAOCSFileSegInfo(prel, aoEntry, segno, SegfileNoLock);
}

AOCSFileSegInfo *GetAOCSFileSegInfoForWrite(Relation prel, AppendOnlyEntry *aoEntry, int segno)
{
    return GetAOCSFileSegInfo(prel, aoEntry, segno, SegfileForceLock);
}

AOCSFileSegInfo *GetAOCSFileSegInfoForVacuum(Relation prel, AppendOnlyEntry *aoEntry, int segno)
{
    return GetAOCSFileSegInfo(prel, aoEntry, segno, SegfileTryLock);
}


AOCSFileSegInfo **GetAllAOCSFileSegInfo(Relation prel,
										AppendOnlyEntry *aoEntry,
										Snapshot snapshot,
										int32 *totalseg)
{
    Relation 			pg_aocsseg_rel;

	AOCSFileSegInfo 	**results;
	
	pg_aocsseg_rel = relation_open(aoEntry->segrelid, AccessShareLock);

	results = GetAllAOCSFileSegInfo_pg_aocsseg_rel(
											prel->rd_att->natts,
											NameStr(prel->rd_rel->relname),
											aoEntry,
											pg_aocsseg_rel,
											snapshot,
											totalseg);

    heap_close(pg_aocsseg_rel, AccessShareLock);

	return results;
}

AOCSFileSegInfo **GetAllAOCSFileSegInfo_pg_aocsseg_rel(
										int numOfColumns,
										char *relationName,
										AppendOnlyEntry *aoEntry,
									    Relation pg_aocsseg_rel,
										Snapshot snapshot,
										int32 *totalseg)
{

    int32 nvp = numOfColumns;

    HeapScanDesc scan;
    HeapTuple tup;

    AOCSFileSegInfo **allseg;
    AOCSFileSegInfo *seginfo;
    int cur_seg;
    Datum *d;
    bool *null;

	Assert(aoEntry != NULL);

    scan = heap_beginscan(pg_aocsseg_rel, snapshot, 0, NULL);

    *totalseg = 0;
    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
        ++(*totalseg);

    heap_endscan(scan);

    if(*totalseg == 0)
    {
        return NULL;
    }

    allseg = (AOCSFileSegInfo **) palloc0(sizeof(AOCSFileSegInfo*) * (*totalseg));
    d = (Datum *) palloc(sizeof(Datum) * Natts_pg_aocsseg);
    null = (bool *) palloc(sizeof(bool) * Natts_pg_aocsseg);

    cur_seg = 0;

    scan = heap_beginscan(pg_aocsseg_rel, snapshot, 0, NULL);
    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        seginfo = (AOCSFileSegInfo *) palloc0(aocsfileseginfo_size(nvp));

        Assert(cur_seg < *totalseg);
        allseg[cur_seg] = seginfo;

        heap_deform_tuple(tup, pg_aocsseg_rel->rd_att, d, null);

        Assert(!null[Anum_pg_aocs_segno - 1]);
        seginfo->segno = DatumGetInt32(d[Anum_pg_aocs_segno - 1]);

        Assert(!null[Anum_pg_aocs_tupcount - 1]);
        seginfo->tupcount = DatumGetInt64(d[Anum_pg_aocs_tupcount - 1]);

        Assert(!null[Anum_pg_aocs_varblockcount - 1]);
        seginfo->varblockcount = DatumGetInt64(d[Anum_pg_aocs_varblockcount - 1]);

		ItemPointerSetInvalid(&seginfo->sequence_tid);

       Assert(!null[Anum_pg_aocs_vpinfo - 1]);
        {
            struct varlena *v = (struct varlena *) DatumGetPointer(d[Anum_pg_aocs_vpinfo - 1]);
            struct varlena *dv = pg_detoast_datum(v);

            /* 
             * VARSIZE(dv) may be less than aocs_vpinfo_size, in case of
             * add column, we try to do a ctas from old table to new table.
             */
            Assert(VARSIZE(dv) <= aocs_vpinfo_size(nvp));

            memcpy(&seginfo->vpinfo, dv, VARSIZE(dv));
            if(dv!=v)
                pfree(dv);
        }
        ++cur_seg;
    }

    pfree(d);
    pfree(null);

    heap_endscan(scan);

    return allseg;
}

/*
 * GetAOCSTotalBytes
 *
 * Get the total bytes for a specific AOCS table from the pg_aocsseg table on this local segdb.
 */
int64 GetAOCSTotalBytes(Relation parentrel)
{
	AOCSFileSegInfo **allseg;
	int totalseg;
	int64 result;
	int s;
	AOCSVPInfo *vpinfo;
	AppendOnlyEntry *aoEntry = NULL;

	aoEntry = GetAppendOnlyEntry(RelationGetRelid(parentrel));
	Assert(aoEntry != NULL);

	result = 0;
	allseg = GetAllAOCSFileSegInfo(parentrel, aoEntry, SnapshotNow, &totalseg);
	for (s = 0; s < totalseg; s++)
	{
		int32 nEntry;
		int e;
		
		vpinfo = &((allseg[s])->vpinfo);
		nEntry = vpinfo->nEntry;

		for (e = 0; e < nEntry; e++)
			result += vpinfo->entry[e].eof;
	}

	pfree(aoEntry);
	
	// UNDONE: How do you free allseg?

	return result;
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
	AppendOnlyEntry *aoEntry = idesc->aoEntry;

    Snapshot usesnapshot = SnapshotDirty;
	Assert(aoEntry != NULL);

    segrel = heap_open(aoEntry->segrelid, RowExclusiveLock);
    segidx = index_open(aoEntry->segidxid, RowExclusiveLock);
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
    d[Anum_pg_aocs_segno-1] = fastgetattr(oldtup, Anum_pg_aocs_segno, tupdesc, &null[Anum_pg_aocs_segno-1]);
    Assert(!null[Anum_pg_aocs_segno-1]);
    Assert(DatumGetInt32(d[Anum_pg_aocs_segno-1]) == idesc->cur_segno);
#endif

    d[Anum_pg_aocs_tupcount-1] = fastgetattr(oldtup, Anum_pg_aocs_tupcount, tupdesc, &null[Anum_pg_aocs_tupcount-1]);
    Assert(!null[Anum_pg_aocs_tupcount-1]);

    d[Anum_pg_aocs_tupcount-1] += idesc->insertCount;
    repl[Anum_pg_aocs_tupcount-1] = true;


    d[Anum_pg_aocs_varblockcount-1] = fastgetattr(oldtup, Anum_pg_aocs_varblockcount, tupdesc, &null[Anum_pg_aocs_varblockcount-1]);
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

void AOCSFileSegInfoAddCount(Relation prel, AppendOnlyEntry *aoEntry, int32 segno, int64 tupadded, int64 varblockadded)
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

	Assert(aoEntry != NULL);

    segrel = heap_open(aoEntry->segrelid, RowExclusiveLock);
    segidx = index_open(aoEntry->segidxid, RowExclusiveLock);

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
    d[Anum_pg_aocs_segno-1] = fastgetattr(oldtup, Anum_pg_aocs_segno, tupdesc, &null[Anum_pg_aocs_segno-1]);
    Assert(!null[Anum_pg_aocs_segno-1]);
    Assert(DatumGetInt32(d[Anum_pg_aocs_segno-1]) == segno);
#endif

    d[Anum_pg_aocs_tupcount-1] = fastgetattr(oldtup, Anum_pg_aocs_tupcount, tupdesc, &null[Anum_pg_aocs_tupcount-1]);
    Assert(!null[Anum_pg_aocs_tupcount-1]);

    d[Anum_pg_aocs_tupcount-1] += tupadded;
    repl[Anum_pg_aocs_tupcount-1] = true;

    d[Anum_pg_aocs_varblockcount-1] = fastgetattr(oldtup, Anum_pg_aocs_varblockcount, tupdesc, &null[Anum_pg_aocs_varblockcount-1]);
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

Datum
gp_update_aocol_master_stats_internal(Relation parentrel)
{
	StringInfoData 	sqlstmt;
	Relation		aosegrel;
	bool 			connected = false;
	char			aoseg_relname[32];
	int 			proc;
	int 			ret;
	float8			total_count = 0;
	MemoryContext 	oldcontext = CurrentMemoryContext;
	AppendOnlyEntry *aoEntry = GetAppendOnlyEntry(RelationGetRelid(parentrel));

	Assert(aoEntry != NULL);
	
	/*
	 * get the name of the aoseg relation
	 */
	aosegrel = heap_open(aoEntry->segrelid, AccessShareLock);
	sprintf(aoseg_relname, "%s", RelationGetRelationName(aosegrel));
	heap_close(aosegrel, AccessShareLock);

	/*
	 * assemble our query string
	 */
	initStringInfo(&sqlstmt);
	appendStringInfo(&sqlstmt, "select segno,sum(tupcount) "
					"from gp_dist_random('pg_aoseg.%s') "
					"group by (segno)", aoseg_relname);


	PG_TRY();
	{

		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Unable to obtain AO relation information from segment databases."),
							errdetail("SPI_connect failed in gp_update_ao_master_stats")));
		}
		connected = true;

		/* Do the query. */
		ret = SPI_execute(sqlstmt.data, false, 0);
		proc = SPI_processed;


		if (ret > 0 && SPI_tuptable != NULL)
		{
			TupleDesc tupdesc = SPI_tuptable->tupdesc;
			SPITupleTable *tuptable = SPI_tuptable;
			int i;

			/*
			 * Iterate through each result tuple
			 */
			for (i = 0; i < proc; i++)
			{
				HeapTuple 	tuple = tuptable->vals[i];
				AOCSFileSegInfo *aocsfsinfo = NULL;
				int			qe_segno;
				int64 		qe_tupcount;
				char 		*val_segno;
				char 		*val_tupcount;
				MemoryContext 	cxt_save;

				/*
				 * Get totals from QE's for a specific segment
				 */
				val_segno = SPI_getvalue(tuple, tupdesc, 1);
				val_tupcount = SPI_getvalue(tuple, tupdesc, 2);

				/* use our own context so that SPI won't free our stuff later */
				cxt_save = MemoryContextSwitchTo(oldcontext);

				/*
				 * Convert to desired data type
				 */
				qe_segno = pg_atoi(val_segno, sizeof(int32), 0);
				qe_tupcount = DatumGetInt64(DirectFunctionCall1(int8in,
																CStringGetDatum(val_tupcount)));

				total_count += qe_tupcount;

				/*
				 * Get the numbers on the QD for this segment
				 */
				aocsfsinfo = GetAOCSFileSegInfoForWrite(parentrel, aoEntry, qe_segno);

				/* since we passed 'true' a segfile record must exist now */
				Assert(aocsfsinfo);

				/*
				 * check if numbers match.
				 * NOTE: proper way is to use int8eq() but since we
				 * don't expect any NAN's in here better do it directly
				 */
				if(aocsfsinfo->tupcount != qe_tupcount)
				{
					int64 	diff = qe_tupcount - aocsfsinfo->tupcount;

					elog(DEBUG3, "gp_update_ao_master_stats: updating "
						"segno %d with tupcount %d", qe_segno,
						(int)qe_tupcount);

					/*
					 * QD tup count !=  QE tup count. update QD count by
					 * passing in the diff (may be negative sometimes).
					 */
					AOCSFileSegInfoAddCount(parentrel, aoEntry, qe_segno, diff, 0);
				}
				else
					elog(DEBUG3, "gp_update_ao_master_stats: no need to "
						"update segno %d. it is synced", qe_segno);

				pfree(aocsfsinfo);

				MemoryContextSwitchTo(cxt_save);

				/*
				 * TODO: if an entry exists for this rel in the AO hash table
				 * need to also update that entry in shared memory. Need to
				 * figure out how to do this safely when concurrent operations
				 * are in progress. note that if no entry exists we are ok.
				 *
				 * At this point this doesn't seem too urgent as we generally
				 * only expect this function to update segno 0 only and the QD
				 * never cares about segment 0 anyway.
				 */
			}
		}

		connected = false;

		SPI_finish();
	}

	/* Clean up in case of error. */
	PG_CATCH();
	{
		if (connected)
			SPI_finish();

		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(aoEntry);
		
	pfree(sqlstmt.data);

	PG_RETURN_FLOAT8(total_count);
}

Datum
aocol_compression_ratio_internal(Relation parentrel)
{
	StringInfoData 	sqlstmt;
	Relation		aosegrel;
	bool 			connected = false;
	char			aocsseg_relname[32];
	int 			proc;
	int 			ret;
	int64			eof = 0;
	int64			eof_uncompressed = 0;
	float8			compress_ratio = -1; /* the default, meaning "not available" */

	MemoryContext 	oldcontext = CurrentMemoryContext;
	Oid segrelid = InvalidOid;

	GetAppendOnlyEntryAuxOids(RelationGetRelid(parentrel),
							  &segrelid, NULL, NULL, NULL);
	Assert(OidIsValid(segrelid));

	/*
	 * get the name of the aoseg relation
	 */
	aosegrel = heap_open(segrelid, AccessShareLock);
	sprintf(aocsseg_relname, "%s", RelationGetRelationName(aosegrel));
	heap_close(aosegrel, AccessShareLock);

	/*
	 * assemble our query string.
	 *
	 * NOTE: The aocsseg (per table) system catalog lives in the gp_aoseg namespace, too.
	 */
	initStringInfo(&sqlstmt);
	if (Gp_role == GP_ROLE_DISPATCH)
		appendStringInfo(&sqlstmt, "select vpinfo "
								"from gp_dist_random('pg_aoseg.%s')",
									aocsseg_relname);
	else
		appendStringInfo(&sqlstmt, "select vpinfo "
								"from pg_aoseg.%s",
									aocsseg_relname);

	PG_TRY();
	{

		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Unable to obtain AO relation information from segment databases."),
							errdetail("SPI_connect failed in get_ao_compression_ratio")));
		}
		connected = true;

		/* Do the query. */
		ret = SPI_execute(sqlstmt.data, false, 0);
		proc = SPI_processed;


		if (ret > 0 && SPI_tuptable != NULL)
		{
			TupleDesc 		tupdesc = SPI_tuptable->tupdesc;
			SPITupleTable*	tuptable = SPI_tuptable;
			int				i;
			HeapTuple 		tuple;
			bool			isnull;
			Datum			vpinfoDatum;
			AOCSVPInfo 		*vpinfo;
			int				j;
			MemoryContext 	cxt_save;

			for (i = 0; i < proc; i++)
			{
				/*
				 * Each row is a binary struct vpinfo with a variable number of entries
				 * on the end.
				 */
				tuple = tuptable->vals[i];
				
				vpinfoDatum = heap_getattr(tuple, 1, tupdesc, &isnull);
				if (isnull)
					break;

				vpinfo = (AOCSVPInfo*)DatumGetByteaP(vpinfoDatum);
				
				// CONSIDER: Better verification of vpinfo.
				Assert(vpinfo->version == 0);
				for (j = 0; j < vpinfo->nEntry; j++)
				{
					eof += vpinfo->entry[j].eof;
					eof_uncompressed += vpinfo->entry[j].eof_uncompressed;
				}
			}

			/* use our own context so that SPI won't free our stuff later */
			cxt_save = MemoryContextSwitchTo(oldcontext);

			/* guard against division by zero */
			if (eof > 0)
			{
				char  buf[8];

				/* calculate the compression ratio */
				float8 compress_ratio_raw = 
						((float8)eof_uncompressed) /
												((float8)eof);

				/* format to 2 digits past the decimal point */
				sprintf(buf, "%.2f", compress_ratio_raw);

				/* format to 2 digit decimal precision */
				compress_ratio = DatumGetFloat8(DirectFunctionCall1(float8in,
												CStringGetDatum(buf)));
			}

			MemoryContextSwitchTo(cxt_save);

		}

		connected = false;
		SPI_finish();
	}

	/* Clean up in case of error. */
	PG_CATCH();
	{
		if (connected)
			SPI_finish();

		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();


	pfree(sqlstmt.data);

	PG_RETURN_FLOAT8(compress_ratio);
}


