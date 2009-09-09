/* 
 * Append only column store access 
 *
 *	Copyright (c), 2009, Greenplum Inc.
 */

#include "postgres.h"

#include "cdb/cdbappendonlystorage.h"
#include "access/aomd.h"
#include "access/heapam.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_appendonly.h"
#include "catalog/namespace.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbappendonlystorageread.h"
#include "cdb/cdbappendonlystoragewrite.h"
#include "utils/datumstream.h"
#include "access/aocssegfiles.h"
#include "cdb/cdbaocsam.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "storage/freespace.h"
#include "storage/smgr.h"
#include "storage/quicklz1.h"
#include "storage/quicklz3.h"
#include "utils/guc.h"

#include "utils/debugbreak.h"

static void open_rds_seg(AOCSScanDesc scan, int segno, DatumStream **ds, 
        int nvp, bool *proj)
{
    char *basepath = relpath(scan->aos_rel->rd_node);
    char fn[MAXPGPATH];

    int i;

    Assert(proj);

    for(i=0; i<nvp; ++i)
    {
        if (proj[i])
        {
            AOCSVPInfoEntry *e = getAOCSVPEntry(scan->seginfo[scan->cur_seg], i);

            snprintf(fn, MAXPGPATH, AOCS_FILE_PATH_FORMAT, basepath, i, segno);

            Assert(scan->ds[i]);
            datumstream_open_file(ds[i], fn, e->eof, e->eof_uncompressed); 
            datumstream_read_block(scan->ds[i]);
        }
    }

    pfree(basepath);
}

static void open_ds(Relation rel, CompressorType compType, int32 compLv, int32 blocksz, 
        DatumStream **ds, int nvp, bool *proj)
{
    char *basepath = relpath(rel->rd_node);
    int i;
    TupleDesc tupdesc = RelationGetDescr(rel);

    /* open datum streams.  It will open segment file underneath */
    for(i=0; i<nvp; ++i)
    {
        Form_pg_attribute attr = tupdesc->attrs[i];

         if(proj)
        {
            if(proj[i])
                ds[i] = create_datumstream(compType, compLv, blocksz, attr, basepath, true);
            else
                ds[i] = NULL;
        }
        else 
            ds[i] = create_datumstream(compType, compLv, blocksz, attr, basepath, false);
    }

    pfree(basepath);
}

static void close_ds(DatumStream **ds, int nvp)
{
    int i;
    for(i=0; i<nvp; ++i)
    {
        if(ds[i])
        {
            destroy_datumstream(ds[i]);
            ds[i] = NULL;
        }
    }
}

static void aocs_initscan(AOCSScanDesc scan, ScanKey key)
{
    if(key)
        memcpy(scan->aos_key, key, scan->aos_nkeys * sizeof(ScanKeyData));

    scan->cur_seg = -1;

    ItemPointerSet(&scan->cdb_fake_ctid, 0, 0);
    scan->cur_seg_row = 0;

    open_ds(scan->aos_rel, scan->compType, scan->compLevel, scan->blocksz,
            scan->ds, scan->aos_rel->rd_att->natts, scan->proj);

    pgstat_count_heap_scan(scan->aos_rel);
}

static int open_next_scan_seg(AOCSScanDesc scan)
{
    int nvp = scan->aos_rel->rd_att->natts;

    while(++scan->cur_seg < scan->total_seg)
    {
        if(scan->seginfo[scan->cur_seg]->tupcount > 0)
        {
            int i = 0;
            bool emptySeg = false;
            for(; i<nvp; ++i)
            {
                if (scan->proj[i])
                {
                    AOCSVPInfoEntry *e = getAOCSVPEntry(scan->seginfo[scan->cur_seg], i);
                    if (e->eof == 0)
                        emptySeg = true;

                    break;
                }
            }

            if (!emptySeg)
            {
            open_rds_seg(scan, scan->seginfo[scan->cur_seg]->segno, scan->ds, nvp, scan->proj);
            return scan->cur_seg;
        }
    }
    }

    return -1;
}

static void close_cur_scan_seg(AOCSScanDesc scan)
{
    int nvp = scan->aos_rel->rd_att->natts;
    int i;

    for(i=0; i<nvp; ++i)
    {
        if(scan->ds[i])
        {
            datumstream_close_file(scan->ds[i]);
            scan->ds[i] = NULL;
        }
    }
}

AOCSScanDesc aocs_beginscan(Relation relation, Snapshot snapshot, int nkeys, ScanKey key, bool *proj)
{
    AOCSScanDesc scan;
    AppendOnlyEntry *aoentry;
    int nvp = relation->rd_att->natts;

    RelationIncrementReferenceCount(relation);
    scan = (AOCSScanDesc) palloc0(sizeof(AOCSScanDescData));
    scan->aos_rel = relation;

    aoentry = GetAppendOnlyEntry(RelationGetRelid(relation));
    Assert(aoentry->majorversion == 1 && aoentry->minorversion == 0);

    scan->compLevel = aoentry->compresslevel;
    scan->compType = CompressorStrToType(aoentry->compresstype, aoentry->compresslevel);
    scan->blocksz = aoentry->blocksize;

    scan->seginfo = GetAllAOCSFileSegInfo(relation, snapshot, &scan->total_seg);

    scan->aos_nkeys = nkeys;
    if(nkeys > 0)
        scan->aos_key = palloc(sizeof(ScanKeyData) * nkeys);
    else
        scan->aos_key = NULL;

    Assert(proj);
    scan->proj = proj;

    scan->ds = (DatumStream **) palloc0(sizeof(DatumStream *) * nvp);

    aocs_initscan(scan, key);

    return scan;
}

void aocs_rescan(AOCSScanDesc scan, ScanKey key)
{
    close_cur_scan_seg(scan);
    close_ds(scan->ds, scan->aos_rel->rd_att->natts);
    aocs_initscan(scan, key);
}

void aocs_endscan(AOCSScanDesc scan)
{
    int i;

    RelationDecrementReferenceCount(scan->aos_rel);

    if(scan->aos_key)
        pfree(scan->aos_key);

    close_cur_scan_seg(scan);
    close_ds(scan->ds, scan->aos_rel->rd_att->natts);

    pfree(scan->ds);

    for(i=0; i<scan->total_seg; ++i)
    {
        if(scan->seginfo[i])
        {
            pfree(scan->seginfo[i]);
            scan->seginfo[i] = NULL;
        }
    }
    if (scan->seginfo)
        pfree(scan->seginfo);

    pfree(scan);
}

void aocs_getnext(AOCSScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
    int nvp; 
    Datum *d = slot_get_values(slot);
    bool *null = slot_get_isnull(slot);

    int err = 0;
    int i; 

    Assert(ScanDirectionIsForward(direction));

    nvp = scan->aos_rel->rd_att->natts;

    while(1)
    {
ReadNext:
        /* If necessary, open next seg */
        if(scan->cur_seg < 0 || err < 0)
        {
            err = open_next_scan_seg(scan);
            if(err < 0)
            {
                /* No more seg, we are at the end */
                ExecClearTuple(slot);
                return;
            }
        }

        Assert(scan->cur_seg >= 0);

        /* Read from cur_seg */
        for(i=0; i<nvp; ++i)
        {
            if(scan->proj[i])
            {
                err = datumstream_advance(scan->ds[i]);
                Assert(err >= 0);
                if(err == 0)
                {
                    err = datumstream_read_block(scan->ds[i]);
                    if(err < 0)
                    {
                        /* Ha, cannot read next block, 
                         * we need to go to next seg
                         */
                        goto ReadNext;
                    }
                    err = datumstream_advance(scan->ds[i]);
                    Assert(err > 0);
                }
            }
        }

        AOTupleIdInit_Init((AOTupleId *) &scan->cdb_fake_ctid);
        AOTupleIdInit_segmentFileNum((AOTupleId *) &scan->cdb_fake_ctid, scan->cur_seg+1);
        AOTupleIdInit_rowNum((AOTupleId *) &scan->cdb_fake_ctid, ++scan->cur_seg_row);

        /* OK, I got the tuple.  Read it out */
        Assert(scan->cur_seg >= 0 && scan->cur_seg < scan->total_seg);
        for(i=0; i<nvp; ++i)
        {
            if(scan->proj[i])
                datumstream_get(scan->ds[i], &d[i], &null[i]);
        }

        TupSetVirtualTupleNValid(slot, nvp);
        slot_set_ctid(slot, &(scan->cdb_fake_ctid));
        return;
    }

    Assert(!"Never here");
    return;
}


/* Open next file segment for write.  See SetCurrentFileSegForWrite */
/* XXX Right now, we put each column to different files */
static void OpenAOCSDatumStreams(AOCSInsertDesc desc)
{
    char *basepath = relpath(desc->aoi_rel->rd_node);
    char fn[MAXPGPATH];

    AOCSFileSegInfo *seginfo;

    int i;
    TupleDesc tupdesc = RelationGetDescr(desc->aoi_rel);
    int nvp = tupdesc->natts;

    desc->ds = (DatumStream **) palloc0(sizeof(DatumStream *) * nvp);
    open_ds(desc->aoi_rel, desc->compType, desc->compLevel, desc->blocksz, 
            desc->ds, nvp, NULL);

    /* Now open seg info file and get eof mark. */
    seginfo = GetAOCSFileSegInfoForWrite(desc->aoi_rel, desc->cur_segno);
    Assert(seginfo);

    for(i=0; i<nvp; ++i)
    {
        AOCSVPInfoEntry *e = getAOCSVPEntry(seginfo, i);

        snprintf(fn, MAXPGPATH, AOCS_FILE_PATH_FORMAT, basepath, i, seginfo->segno);
        datumstream_open_file(desc->ds[i], fn, e->eof, e->eof_uncompressed);
    }

    pfree(basepath);
    pfree(seginfo);
}

AOCSInsertDesc aocs_insert_init(Relation rel, int segno)
{
    AOCSInsertDesc desc;
    AppendOnlyEntry *aoentry;

    aoentry = GetAppendOnlyEntry(RelationGetRelid(rel));
    Assert(aoentry->majorversion == 1 && aoentry->minorversion == 0);

    desc = (AOCSInsertDesc) palloc0(sizeof(AOCSInsertDescData));
    desc->aoi_rel = rel;

    Assert(segno >= 0);
    desc->cur_segno = segno;

    desc->compLevel = aoentry->compresslevel;
    desc->compType = CompressorStrToType(aoentry->compresstype, aoentry->compresslevel);
    desc->blocksz = aoentry->blocksize;

    OpenAOCSDatumStreams(desc);

    pfree(aoentry);
    return desc;
}


Oid aocs_insert_values(AOCSInsertDesc idesc, Datum *d, bool * null)
{
    Relation rel = idesc->aoi_rel;
    int i;

    Assert(!rel->rd_rel->relhasoids);

    /* As usual, at this momnet, we assume one col per vp */
    for(i=0; i<rel->rd_att->natts; ++i)
    {
        int err = datumstream_put(idesc->ds[i], d[i], null[i]);

        if(err < 0)
        {
            datumstream_write_block(idesc->ds[i]);
            err = datumstream_put(idesc->ds[i], d[i], null[i]);
            if (err < 0)
            {
                Assert(!null[i]);
                err = datumstream_write_lob(idesc->ds[i], d[i]);
                Assert(err >= 0);
            }
        }
    }

    ++idesc->insertCount;
    return InvalidOid;
}

void aocs_insert_finish(AOCSInsertDesc idesc)
{
    Relation rel = idesc->aoi_rel;
    int i;

    for(i=0; i<rel->rd_att->natts; ++i)
    {
        datumstream_write_block(idesc->ds[i]);
        datumstream_close_file(idesc->ds[i]);
    }

    UpdateAOCSFileSegInfo(idesc);
}

