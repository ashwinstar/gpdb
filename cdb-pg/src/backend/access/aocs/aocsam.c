/* 
 * Append only columnar access methods
 *
 *	Copyright (c), 2009, Greenplum Inc.
 */

#include "postgres.h"

#include "access/appendonlytid.h"
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
#include "catalog/gp_fastsequence.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbappendonlyblockdirectory.h"
#include "cdb/cdbappendonlystoragelayer.h"
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

/*
 * Open the segment file for a specified column
 * associated with the datum stream.
 */
static void open_datumstream_segfile(char *basepath,
									 AOCSFileSegInfo *segInfo,
									 DatumStream *ds,
									 int colNo)
{
	int segNo = segInfo->segno;
    char fn[MAXPGPATH];
	
	AOCSVPInfoEntry *e = getAOCSVPEntry(segInfo, colNo);

	FormatAOSegmentFileName(basepath, segNo, colNo, fn);
	Assert(strlen(fn) + 1 <= MAXPGPATH);

	Assert(ds);
	datumstream_open_file(ds, fn, e->eof, e->eof_uncompressed); 
}

/*
 * Open all segment files associted with the datum stream.
 *
 * Currently, there is one segment file for each column. This function
 * only opens files for those columns which are in the projection.
 */
static void open_all_datumstream_segfiles(Relation rel,
										  AOCSFileSegInfo *segInfo,
										  DatumStream **ds, 
										  bool *proj,
										  int nvp)
{
    char *basepath = relpath(rel->rd_node);

    int i;

    Assert(proj);

    for(i=0; i<nvp; ++i)
    {
        if (proj[i])
		{
			open_datumstream_segfile(basepath, segInfo, ds[i], i);
			datumstream_read_block(ds[i]);
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
				open_all_datumstream_segfiles(scan->aos_rel,
											  scan->seginfo[scan->cur_seg],
											  scan->ds,
											  scan->proj,
											  nvp);

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

    scan->seginfo = GetAllAOCSFileSegInfo(relation, aoentry, snapshot, &scan->total_seg);

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
    int ncol;
    Datum *d = slot_get_values(slot);
    bool *null = slot_get_isnull(slot);
	AOTupleId aoTupleId;

    int err = 0;
    int i; 

    Assert(ScanDirectionIsForward(direction));

    ncol = slot->tts_tupleDescriptor->natts;
    Assert(ncol <= scan->aos_rel->rd_att->natts);

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
			scan->cur_seg_row = 0;
        }

        Assert(scan->cur_seg >= 0);

        /* Read from cur_seg */
        for(i=0; i<ncol; ++i)
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

		AOTupleIdInit_Init(&aoTupleId);
		AOTupleIdInit_segmentFileNum(&aoTupleId,
									 scan->seginfo[scan->cur_seg]->segno);
		
		scan->cur_seg_row++;
        AOTupleIdInit_rowNum(&aoTupleId, scan->cur_seg_row);

        /* OK, I got the tuple.  Read it out */
        Assert(scan->cur_seg >= 0 && scan->cur_seg < scan->total_seg);
        for(i=0; i<ncol; ++i)
        {
            if(scan->proj[i])
                datumstream_get(scan->ds[i], &d[i], &null[i]);
        }

		scan->cdb_fake_ctid = *((ItemPointer)&aoTupleId);

        TupSetVirtualTupleNValid(slot, ncol);
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
    seginfo = GetAOCSFileSegInfoForWrite(desc->aoi_rel, desc->aoEntry, desc->cur_segno);
    Assert(seginfo);
	desc->fsInfo = seginfo;
	desc->rowCount = seginfo->tupcount;

    for(i=0; i<nvp; ++i)
    {
        AOCSVPInfoEntry *e = getAOCSVPEntry(seginfo, i);

		FormatAOSegmentFileName(basepath, seginfo->segno, i, fn);
		Assert(strlen(fn) + 1 <= MAXPGPATH);

        datumstream_open_file(desc->ds[i], fn, e->eof, e->eof_uncompressed);
    }

    pfree(basepath);
}

static inline void
SetBlockFirstRowNums(DatumStream **datumStreams,
					 int numDatumStreams,
					 int64 blockFirstRowNum)
{
	int i;
	
	Assert(datumStreams != NULL);
	
	for (i=0; i<numDatumStreams; i++)
	{
		Assert(datumStreams[i] != NULL);
		datumStreams[i]->blockFirstRowNum = blockFirstRowNum;
	}
}


AOCSInsertDesc aocs_insert_init(Relation rel, int segno)
{
    AOCSInsertDesc desc;
    AppendOnlyEntry *aoentry;
	TupleDesc tupleDesc;
	int64 firstSequence = 0;

    aoentry = GetAppendOnlyEntry(RelationGetRelid(rel));
    Assert(aoentry->majorversion == 1 && aoentry->minorversion == 0);

    desc = (AOCSInsertDesc) palloc0(sizeof(AOCSInsertDescData));
    desc->aoi_rel = rel;
	tupleDesc =RelationGetDescr(desc->aoi_rel);

    Assert(segno >= 0);
    desc->cur_segno = segno;
	desc->aoEntry = aoentry;
	
    desc->compLevel = aoentry->compresslevel;
    desc->compType = CompressorStrToType(aoentry->compresstype, aoentry->compresslevel);
    desc->blocksz = aoentry->blocksize;

    OpenAOCSDatumStreams(desc);

	/*
	 * Obtain the next list of fast sequences if there is an index for
	 * this relation.
	 */
	desc->numSequences = 0;
	if (desc->aoi_rel->rd_rel->relhasindex)
	{
		firstSequence =
			GetFastSequencesByTid(&desc->fsInfo->sequence_tid,
								  desc->rowCount + 1,
								  NUM_FAST_SEQUENCES);
		desc->numSequences = NUM_FAST_SEQUENCES;
	}
	/* Set last_sequence value */
	desc->lastSequence = desc->rowCount;
	
	if (desc->lastSequence + 1 < firstSequence)
		desc->lastSequence = firstSequence - 1;

	SetBlockFirstRowNums(desc->ds, tupleDesc->natts, desc->lastSequence + 1);

	/* Initialize the block directory. */
	tupleDesc = RelationGetDescr(rel);
	AppendOnlyBlockDirectory_Init_forInsert(
		&(desc->blockDirectory), aoentry, (FileSegInfo *)desc->fsInfo, desc->lastSequence,
		rel, segno, tupleDesc->natts, true);

    return desc;
}


Oid aocs_insert_values(AOCSInsertDesc idesc, Datum *d, bool * null, AOTupleId *aoTupleId)
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
			int itemCount = idesc->ds[i]->nth;
			
            datumstream_write_block(idesc->ds[i]);
            err = datumstream_put(idesc->ds[i], d[i], null[i]);
            if (err < 0)
            {
                Assert(!null[i]);
                err = datumstream_write_lob(idesc->ds[i], d[i]);
                Assert(err >= 0);
            }

			/* Insert an entry to the block directory */
			Assert(idesc->ds[i]->ao_write != NULL);
			AppendOnlyBlockDirectory_InsertEntry(
				&idesc->blockDirectory,
				i,
				idesc->ds[i]->blockFirstRowNum,
				AppendOnlyStorageWrite_LastWriteBeginPosition(idesc->ds[i]->ao_write),
				itemCount);

			/* Set blockFirstRowNum to the next row number.*/
			idesc->ds[i]->blockFirstRowNum = idesc->lastSequence + 1;
        }
    }

    idesc->insertCount++;
	idesc->lastSequence++;
	if (idesc->numSequences > 0)
		(idesc->numSequences)--;

	Assert(idesc->numSequences >= 0);

	AOTupleIdInit_Init(aoTupleId);
	AOTupleIdInit_segmentFileNum(aoTupleId, idesc->cur_segno);
	AOTupleIdInit_rowNum(aoTupleId, idesc->lastSequence);

	/*
	 * If this relation has indexes, and the allocated fast sequence numbers
	 * is used up, we request for a next list of fast sequence numbers.
	 */
	if (idesc->aoi_rel->rd_rel->relhasindex &&
		idesc->numSequences == 0)
	{
		int64 firstSequence;
		Assert(ItemPointerIsValid(&idesc->fsInfo->sequence_tid));
		
		firstSequence =
			GetFastSequencesByTid(&idesc->fsInfo->sequence_tid,
								  idesc->lastSequence + 1,
								  NUM_FAST_SEQUENCES);
		Assert(firstSequence == idesc->lastSequence + 1);
		idesc->numSequences = NUM_FAST_SEQUENCES;
	}
	
    return InvalidOid;
}

void aocs_insert_finish(AOCSInsertDesc idesc)
{
    Relation rel = idesc->aoi_rel;
    int i;

    for(i=0; i<rel->rd_att->natts; ++i)
    {
		int itemCount = idesc->ds[i]->nth;
		
        datumstream_write_block(idesc->ds[i]);

		AppendOnlyBlockDirectory_InsertEntry(
			&idesc->blockDirectory,
			i,
			idesc->ds[i]->blockFirstRowNum,
			AppendOnlyStorageWrite_LastWriteBeginPosition(idesc->ds[i]->ao_write),
			itemCount);

        datumstream_close_file(idesc->ds[i]);
    }

	AppendOnlyBlockDirectory_End_forInsert(&(idesc->blockDirectory));

    UpdateAOCSFileSegInfo(idesc);

	pfree(idesc->fsInfo);
    pfree(idesc->aoEntry);

	close_ds(idesc->ds, rel->rd_att->natts);
}

static void
positionFirstBlockOfRange(
	DatumStreamFetchDesc datumStreamFetchDesc)
{
	AppendOnlyBlockDirectoryEntry_GetBeginRange(
				&datumStreamFetchDesc->currentBlock.blockDirectoryEntry,
				&datumStreamFetchDesc->scanNextFileOffset,
				&datumStreamFetchDesc->scanNextRowNum);
}

static void
positionLimitToEndOfRange(
	DatumStreamFetchDesc datumStreamFetchDesc)
{
	AppendOnlyBlockDirectoryEntry_GetEndRange(
				&datumStreamFetchDesc->currentBlock.blockDirectoryEntry,
				&datumStreamFetchDesc->scanAfterFileOffset,
				&datumStreamFetchDesc->scanLastRowNum);
}


static void
positionSkipCurrentBlock(
	DatumStreamFetchDesc datumStreamFetchDesc)
{
	datumStreamFetchDesc->scanNextFileOffset = 
		datumStreamFetchDesc->currentBlock.fileOffset + 
		datumStreamFetchDesc->currentBlock.overallBlockLen;
	
	datumStreamFetchDesc->scanNextRowNum =
		datumStreamFetchDesc->currentBlock.lastRowNum + 1;
}

static void
fetchFromCurrentBlock(AOCSFetchDesc aocsFetchDesc,
					  int64 rowNum,
					  TupleTableSlot *slot,
					  int colno)
{
	DatumStreamFetchDesc datumStreamFetchDesc =
		aocsFetchDesc->datumStreamFetchDesc[colno];
	DatumStream *datumStream = datumStreamFetchDesc->datumStream;
	int status;
	Datum value;
	bool null;

	int rowNumInBlock = rowNum - datumStreamFetchDesc->currentBlock.firstRowNum;

	Assert(rowNumInBlock >= 0);

	datumstream_find(datumStream, rowNumInBlock);

	if (slot != NULL)
	{
		Datum *values = slot_get_values(slot);
		bool *nulls = slot_get_isnull(slot);

		status = datumstream_get(datumStream, &(values[colno]), &(nulls[colno]));
	}
	else
	{
		status = datumstream_get(datumStream, &value, &null);
	}
		  
	Assert(status == 0);
}

static bool
scanToFetchValue(AOCSFetchDesc aocsFetchDesc,
				 int64 rowNum,
				 TupleTableSlot *slot,
				 int colno)
{
	DatumStreamFetchDesc datumStreamFetchDesc =
		aocsFetchDesc->datumStreamFetchDesc[colno];
	DatumStream *datumStream = datumStreamFetchDesc->datumStream;
	bool found;
	
	found = datumstream_find_block(datumStream,
								   datumStreamFetchDesc,
								   rowNum);
	if (found)
		fetchFromCurrentBlock(aocsFetchDesc, rowNum, slot, colno);

	return found;
}

static void
closeFetchSegmentFile(DatumStreamFetchDesc datumStreamFetchDesc)
{
	Assert(datumStreamFetchDesc->currentSegmentFile.isOpen);

	datumstream_close_file(datumStreamFetchDesc->datumStream);
	datumStreamFetchDesc->currentSegmentFile.isOpen = false;
}

static bool
openFetchSegmentFile(AOCSFetchDesc aocsFetchDesc,
					 int openSegmentFileNum,
					 int colNo)
{
	int		i;
	
	AOCSFileSegInfo	*fsInfo;
	int			segmentFileNum;
	int64		logicalEof;
	DatumStreamFetchDesc datumStreamFetchDesc =
		aocsFetchDesc->datumStreamFetchDesc[colNo];
	
	Assert(!datumStreamFetchDesc->currentSegmentFile.isOpen);
	
	i = 0;
	while (true)
	{
		if (i >= aocsFetchDesc->totalSegfiles)
			return false;	// Segment file not visible in catalog information.
			
		fsInfo = aocsFetchDesc->segmentFileInfo[i];
		segmentFileNum = fsInfo->segno;
		if (openSegmentFileNum == segmentFileNum)
		{
			AOCSVPInfoEntry *entry = getAOCSVPEntry(fsInfo, colNo);
			
			logicalEof = entry->eof;
			break;
		}
		i++;
	}

	open_datumstream_segfile(aocsFetchDesc->basepath,
							 fsInfo,
							 datumStreamFetchDesc->datumStream,
							 colNo);

	datumStreamFetchDesc->currentSegmentFile.num = openSegmentFileNum;
	datumStreamFetchDesc->currentSegmentFile.logicalEof = logicalEof;

	datumStreamFetchDesc->currentSegmentFile.isOpen = true;

	return true;
}

static void
resetCurrentBlockInfo(CurrentBlock *currentBlock)
{
	currentBlock->have = false;
	currentBlock->firstRowNum = 0;
	currentBlock->lastRowNum = 0;
}

/*
 * Initialize the fetch descriptor.
 */
AOCSFetchDesc
aocs_fetch_init(Relation relation,
				Snapshot snapshot,
				bool *proj)
{
	AOCSFetchDesc aocsFetchDesc;
	int colno;
	AppendOnlyEntry *aoentry;
	AppendOnlyStorageAttributes *attr;
	char *basePath = relpath(relation->rd_node);
	TupleDesc tupleDesc = RelationGetDescr(relation);
	
	/*
	 * increment relation ref count while scanning relation
	 *
	 * This is just to make really sure the relcache entry won't go away while
	 * the scan has a pointer to it.  Caller should be holding the rel open
	 * anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	aocsFetchDesc = (AOCSFetchDesc) palloc0(sizeof(AOCSFetchDescData));
	aocsFetchDesc->relation = relation;
	
	aocsFetchDesc->initContext = CurrentMemoryContext;

	aocsFetchDesc->segmentFileNameMaxLen = AOSegmentFilePathNameLen(relation) + 1;
	aocsFetchDesc->segmentFileName = 
						(char*)palloc(aocsFetchDesc->segmentFileNameMaxLen);
	aocsFetchDesc->segmentFileName[0] = '\0';
	aocsFetchDesc->basepath = basePath;

	Assert(proj);
	aocsFetchDesc->proj = proj;

	aoentry = GetAppendOnlyEntry(RelationGetRelid(relation));
    Assert(aoentry->majorversion == 1 && aoentry->minorversion == 0);

	attr = &aocsFetchDesc->storageAttributes;
	attr->compress = (aoentry->compresslevel > 0) ? true : false;
	attr->compressType =
		CompressorStrToType(aoentry->compresstype,
							aoentry->compresslevel);
	attr->compressLevel = aoentry->compresslevel;
	attr->checksum = aoentry->checksum;
	attr->use32BitAlignment = true; /* Old-style 32-bit alignment of storage blocks */
	attr->safeFSWriteSize = aoentry->safefswritesize;

	aocsFetchDesc->blockSize = aoentry->blocksize;

	aocsFetchDesc->segmentFileInfo =
		GetAllAOCSFileSegInfo(relation, aoentry, snapshot, &aocsFetchDesc->totalSegfiles);

	AppendOnlyBlockDirectory_Init_forSearch(
		&aocsFetchDesc->blockDirectory,
		aoentry,
		(FileSegInfo **)aocsFetchDesc->segmentFileInfo,
		aocsFetchDesc->totalSegfiles,
		aocsFetchDesc->relation,
		relation->rd_att->natts,
		true);
	
	Assert(relation->rd_att != NULL);
	
	aocsFetchDesc->datumStreamFetchDesc = (DatumStreamFetchDesc*)
		palloc0(relation->rd_att->natts * sizeof(DatumStreamFetchDesc));
	
	for (colno = 0; colno < relation->rd_att->natts; colno++)
	{
		aocsFetchDesc->datumStreamFetchDesc[colno] = NULL;
		if (proj[colno])
		{
			aocsFetchDesc->datumStreamFetchDesc[colno] = (DatumStreamFetchDesc)
				palloc0(sizeof(DatumStreamFetchDescData));

			aocsFetchDesc->datumStreamFetchDesc[colno]->datumStream =
				create_datumstream(attr->compressType,
								   attr->compressLevel,
								   aocsFetchDesc->blockSize,
								   tupleDesc->attrs[colno],
								   basePath,
								   true);
		}
	}

	return aocsFetchDesc;
}

/*
 * Fetch the tuple based on the given tuple id.
 *
 * If the 'slot' is not NULL, the tuple will be assigned to the slot.
 *
 * Return true if the tuple is found. Otherwise, return false.
 */
bool
aocs_fetch(AOCSFetchDesc aocsFetchDesc,
		   AOTupleId *aoTupleId,
		   TupleTableSlot *slot)
{
	int segmentFileNum = AOTupleIdGet_segmentFileNum(aoTupleId);
	int64 rowNum = AOTupleIdGet_rowNum(aoTupleId);
	int numCols = aocsFetchDesc->relation->rd_att->natts;
	int colno;
	bool found = true;
	
	Assert(numCols > 0);
	
	/*
	 * Go through columns one by one. Check if the current block
	 * has the requested tuple. If so, fetch it. Otherwise, read
	 * the block that contains the requested tuple.
	 */
	for(colno=0; colno<numCols; colno++)
	{
		DatumStreamFetchDesc datumStreamFetchDesc = aocsFetchDesc->datumStreamFetchDesc[colno];

		/* If this column does not need to be fetched, skip it. */
		if (datumStreamFetchDesc == NULL)
			continue;
		
		/*
		 * If the current block has the requested tuple, read it.
		 */
		if (datumStreamFetchDesc->currentSegmentFile.isOpen &&
			datumStreamFetchDesc->currentSegmentFile.num == segmentFileNum &&
			datumStreamFetchDesc->currentBlock.have)
		{
			if (rowNum >= datumStreamFetchDesc->currentBlock.firstRowNum &&
				rowNum <= datumStreamFetchDesc->currentBlock.lastRowNum)
			{
				fetchFromCurrentBlock(aocsFetchDesc, rowNum, slot, colno);
				continue;
			}
			
			/*
			 * Otherwise, fetch the right block.
			 */
			if (AppendOnlyBlockDirectoryEntry_RangeHasRow(
					&(datumStreamFetchDesc->currentBlock.blockDirectoryEntry),
					rowNum))
			{
				/*
				 * The tuple is covered by the current Block Directory entry,
				 * but is it before or after our current block?
				 */
				if (rowNum < datumStreamFetchDesc->currentBlock.firstRowNum)
				{
					/*
					 * Set scan range to prior block
					 */
					positionFirstBlockOfRange(datumStreamFetchDesc);
					
					datumStreamFetchDesc->scanAfterFileOffset =
						datumStreamFetchDesc->currentBlock.fileOffset;
					datumStreamFetchDesc->scanLastRowNum =
						datumStreamFetchDesc->currentBlock.firstRowNum - 1;
				}
				else
				{
					/*
					 * Set scan range to following blocks.
					 */
					positionSkipCurrentBlock(datumStreamFetchDesc);
					positionLimitToEndOfRange(datumStreamFetchDesc);
				}

				if (!scanToFetchValue(aocsFetchDesc, rowNum, slot, colno))
				{
					found = false;
					break;
				}
				
				continue;
			}
		}

		/*
		 * Open or switch open, if necessary.
		 */
		if (datumStreamFetchDesc->currentSegmentFile.isOpen &&
			segmentFileNum != datumStreamFetchDesc->currentSegmentFile.num)
		{
			closeFetchSegmentFile(datumStreamFetchDesc);

			Assert(!datumStreamFetchDesc->currentSegmentFile.isOpen);
		}

		if (!datumStreamFetchDesc->currentSegmentFile.isOpen)
		{
			if (!openFetchSegmentFile(
					aocsFetchDesc,
					segmentFileNum,
					colno))
			{
				found = false;	// Segment file not in aoseg table..
						   		// Must be aborted or deleted and reclaimed.
				break;
			}

			/* Reset currentBlock info */
			resetCurrentBlockInfo(&(datumStreamFetchDesc->currentBlock));
		}

		/*
		 * Need to get the Block Directory entry that covers the TID.
		 */
		if (!AppendOnlyBlockDirectory_GetEntry(
				&aocsFetchDesc->blockDirectory,
				aoTupleId,
				colno,
				&datumStreamFetchDesc->currentBlock.blockDirectoryEntry))
		{
			found = false;	// Row not represented in Block Directory.
				   			// Must be aborted or deleted and reclaimed.
			break;
		}
		
		/*
		 * Set scan range covered by new Block Directory entry.
		 */
		positionFirstBlockOfRange(datumStreamFetchDesc);
		
		positionLimitToEndOfRange(datumStreamFetchDesc);
		
		if (!scanToFetchValue(aocsFetchDesc, rowNum, slot, colno))
		{
			found = false;
			break;
		}
	}
	
	if (found)
	{
		if (slot != NULL)
		{
			TupSetVirtualTupleNValid(slot, colno);
			slot_set_ctid(slot, (ItemPointer)aoTupleId);
		}
	}
	else
	{
		if (slot != NULL)
			slot = ExecClearTuple(slot);
	}

	return found;
}

void
aocs_fetch_finish(AOCSFetchDesc aocsFetchDesc)
{
	int colno;
	Relation relation = aocsFetchDesc->relation;

	Assert(relation != NULL && relation->rd_att != NULL);
	
	for (colno = 0; colno < relation->rd_att->natts; colno++)
	{
		DatumStreamFetchDesc datumStreamFetchDesc =
			aocsFetchDesc->datumStreamFetchDesc[colno];
		if (datumStreamFetchDesc != NULL)
		{
			Assert(datumStreamFetchDesc->datumStream != NULL);
			datumstream_close_file(datumStreamFetchDesc->datumStream);
			destroy_datumstream(datumStreamFetchDesc->datumStream);
			pfree(datumStreamFetchDesc);
		}
	}

	AppendOnlyBlockDirectory_End_forSearch(&aocsFetchDesc->blockDirectory);

	RelationDecrementReferenceCount(aocsFetchDesc->relation);

	pfree(aocsFetchDesc->segmentFileName);
	pfree(aocsFetchDesc->basepath);
	
	// UNDONE: More?

	pfree(aocsFetchDesc);
}
