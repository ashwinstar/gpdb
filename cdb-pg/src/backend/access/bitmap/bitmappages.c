/*-------------------------------------------------------------------------
 *
 * bitmappage.c
 *	  Bitmap index page management code for the bitmap index.
 *
 * Copyright (c) 2006, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/tupdesc.h"
#include "access/bitmap.h"
#include "parser/parse_oper.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* 
 * Helper functions for hashing and matching build data. At this stage, the
 * hash API doesn't know about complex keys like those use during index
 * creation (the key is an array of key attributes). c.f. execGrouping.c.
 */
typedef struct BMBuildHashData
{
	int			natts;
	FmgrInfo   *hash_funcs;
	FmgrInfo   *eq_funcs;
	MemoryContext tmpcxt;
	MemoryContext hash_cxt;
} BMBuildHashData;

static BMBuildHashData *cur_bmbuild = NULL;

static uint32 build_hash_key(const void *key, Size keysize);
static int build_match_key(const void *key1, const void *key2, Size keysize);

/*
 * _bitmap_getbuf() -- return the buffer for the given block number and
 * 					   the access method.
 */
Buffer
_bitmap_getbuf(Relation rel, BlockNumber blkno, int access)
{
	Buffer buf;

	if (blkno != P_NEW)
	{
		buf = ReadBuffer(rel, blkno);
		if (access != BM_NOLOCK)
			LockBuffer(buf, access);
	}

	else
	{
		bool		needLock;

		Assert(access == BM_WRITE);

		/*
		 * Extend the relation by one page.
		 *
		 * We have to use a lock to ensure no one else is extending the rel at
		 * the same time, else we will both try to initialize the same new
		 * page.  We can skip locking for new or temp relations, however,
		 * since no one else could be accessing them.
		 */
		needLock = !RELATION_IS_LOCAL(rel);

		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);

		buf = ReadBuffer(rel, P_NEW);

		/* Acquire buffer lock on new page */
		LockBuffer(buf, BM_WRITE);

		/*
		 * Release the file-extension lock; it's now OK for someone else to
		 * extend the relation some more.
		 */
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);
	}

	return buf;
}

/*
 * _bitmap_wrtbuf() -- write a buffer page to disk.
 *
 * Release the lock and the pin held on the buffer.
 */
void
_bitmap_wrtbuf(Buffer buf)
{
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

/*
 * _bitmap_wrtnorelbuf() -- write a buffer page to disk without still holding
 *		the pin on this page.
 */
void
_bitmap_wrtnorelbuf(Buffer buf)
{
	MarkBufferDirty(buf);
}

/*
 * _bitmap_relbuf() -- release the buffer without writing.
 */
void
_bitmap_relbuf(Buffer buf)
{
	UnlockReleaseBuffer(buf);
}

/*
 * _bitmap_init_lovpage -- initialize a new LOV page.
 */
void
_bitmap_init_lovpage(Relation rel, Buffer buf)
{
	Page			page;

	page = (Page) BufferGetPage(buf);

	if(PageIsNew(page))
		PageInit(page, BufferGetPageSize(buf), 0);
}

/*
 * _bitmap_init_bitmappage() -- initialize a new page to store the bitmap.
 */
void
_bitmap_init_bitmappage(Relation rel, Buffer buf)
{
	Page			page;
	BMBitmapOpaque	opaque;

	page = (Page) BufferGetPage(buf);

	if(PageIsNew(page))
		PageInit(page, BufferGetPageSize(buf), sizeof(BMBitmapOpaqueData));

	/* even though page may not be new, reset all values */
	opaque = (BMBitmapOpaque) PageGetSpecialPointer(page);
	opaque->bm_hrl_words_used = 0;
	opaque->bm_bitmap_next = InvalidBlockNumber;
	opaque->bm_last_tid_location = 0;
}

/*
 * _bitmap_init_buildstate() -- initialize the build state before building
 *	a bitmap index.
 */
void
_bitmap_init_buildstate(Relation index, BMBuildState *bmstate)
{
	Page		page;
	BMMetaPage	mp;
	HASHCTL		hash_ctl;
	int			hash_flags;
	int			i;
	Buffer		metabuf;


	/* initialize the build state */
	bmstate->bm_tupDesc = RelationGetDescr(index);
	bmstate->bm_tidLocsBuffer = (BMTidBuildBuf *)
		palloc(sizeof(BMTidBuildBuf));
	bmstate->bm_tidLocsBuffer->byte_size = 0;
	bmstate->bm_tidLocsBuffer->lov_blocks = NIL;
	bmstate->bm_tidLocsBuffer->max_lov_block = InvalidBlockNumber;

	metabuf = _bitmap_getbuf(index, BM_METAPAGE, BM_READ);
	page = BufferGetPage(metabuf);
	mp = (BMMetaPage) PageGetContents(page);
	_bitmap_open_lov_heapandindex(index, mp, &(bmstate->bm_lov_heap),
								  &(bmstate->bm_lov_index), 
								  RowExclusiveLock);

	_bitmap_relbuf(metabuf);

	cur_bmbuild = (BMBuildHashData *)palloc(sizeof(BMBuildHashData));
	cur_bmbuild->hash_funcs = (FmgrInfo *)
						palloc(sizeof(FmgrInfo) * bmstate->bm_tupDesc->natts);
	cur_bmbuild->eq_funcs = (FmgrInfo *)
                        palloc(sizeof(FmgrInfo) * bmstate->bm_tupDesc->natts);

	for (i = 0; i < bmstate->bm_tupDesc->natts; i++)
	{
		Oid			typid = bmstate->bm_tupDesc->attrs[i]->atttypid;
		Operator	optup;
		Oid			eq_opr;
		Oid			eq_function;
 		Oid			hash_function;

		optup = equality_oper(typid, false);
		eq_opr = oprid(optup);
		eq_function = oprfuncid(optup);
		ReleaseSysCache(optup);
 		hash_function = get_op_hash_function(eq_opr);
 		if (!OidIsValid(hash_function))
		{
			pfree(cur_bmbuild);
			cur_bmbuild = NULL;
			break;
		}

		fmgr_info(eq_function, &cur_bmbuild->eq_funcs[i]);
		fmgr_info(hash_function, &cur_bmbuild->hash_funcs[i]);
	}

	if (cur_bmbuild)
	{
		cur_bmbuild->natts = bmstate->bm_tupDesc->natts;
		cur_bmbuild->tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
        	                      "Bitmap build temp space",
            	                  ALLOCSET_DEFAULT_MINSIZE,
                	              ALLOCSET_DEFAULT_INITSIZE,
                    	          ALLOCSET_DEFAULT_MAXSIZE);
		cur_bmbuild = cur_bmbuild;

		/* setup the hash table */
	    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    	hash_ctl.keysize = sizeof(Datum) * cur_bmbuild->natts;
		hash_ctl.entrysize = hash_ctl.keysize + sizeof(BMBuildLovData) + 200; 
    	hash_ctl.hash = build_hash_key;
	    hash_ctl.match = build_match_key;
    	hash_ctl.hcxt = AllocSetContextCreate(CurrentMemoryContext,
        	                      "Bitmap build hash table",
            	                  ALLOCSET_DEFAULT_MINSIZE,
                	              ALLOCSET_DEFAULT_INITSIZE,
                    	          ALLOCSET_DEFAULT_MAXSIZE);
		cur_bmbuild->hash_cxt = hash_ctl.hcxt;

		hash_flags = HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT;

		bmstate->lovitem_hash = hash_create("Bitmap index build lov item hash",
											100, &hash_ctl, hash_flags);
	}
	else
	{
		int attno;
		bmstate->lovitem_hash = NULL;
		bmstate->bm_lov_scanKeys =
			(ScanKey)palloc0(bmstate->bm_tupDesc->natts * sizeof(ScanKeyData));

		for (attno = 0; attno < bmstate->bm_tupDesc->natts; attno++)
		{
			RegProcedure	opfuncid;
			Oid				atttypid;

			atttypid = bmstate->bm_tupDesc->attrs[attno]->atttypid;
			opfuncid = equality_oper_funcid(atttypid);

			ScanKeyEntryInitialize(&(bmstate->bm_lov_scanKeys[attno]), SK_ISNULL, 
							   attno + 1, BTEqualStrategyNumber, InvalidOid, 
							   opfuncid, 0);
		}

		bmstate->bm_lov_scanDesc = index_beginscan(bmstate->bm_lov_heap,
							 bmstate->bm_lov_index, SnapshotAny, 
							 bmstate->bm_tupDesc->natts,
							 bmstate->bm_lov_scanKeys);
	}

	/*
	 * We need to log index creation in WAL iff WAL archiving is enabled
	 * AND it's not a temp index. Currently, since building an index
	 * writes page to the shared buffer, we can't disable WAL archiving.
	 * We will add this shortly.
	 */	
	bmstate->use_wal = XLogArchivingActive() && !index->rd_istemp;
}

/*
 * _bitmap_cleanup_buildstate() -- clean up the build state after
 *	inserting all rows in the heap into the bitmap index.
 */
void
_bitmap_cleanup_buildstate(Relation index, BMBuildState *bmstate)
{
	/* write out remaining tids in bmstate->bm_tidLicsBuffer */
	BMTidBuildBuf	*tidLocsBuffer = bmstate->bm_tidLocsBuffer;
	_bitmap_write_alltids(index, tidLocsBuffer, bmstate->use_wal);

	pfree(bmstate->bm_tidLocsBuffer);

	if (cur_bmbuild)
	{
		MemoryContextDelete(cur_bmbuild->tmpcxt);
		MemoryContextDelete(cur_bmbuild->hash_cxt);
		pfree(cur_bmbuild->hash_funcs);
		pfree(cur_bmbuild->eq_funcs);
		pfree(cur_bmbuild);
		cur_bmbuild = NULL;
	}
	else
	{
		/* 
		 * We might have build an index on a non-hashable data type, in
		 * which case we will have searched the btree manually. Free associated
		 * memory.
		 */
		index_endscan(bmstate->bm_lov_scanDesc);
		pfree(bmstate->bm_lov_scanKeys);
	}

	_bitmap_close_lov_heapandindex(bmstate->bm_lov_heap,bmstate->bm_lov_index,
						 		   RowExclusiveLock);
}

/*
 * _bitmap_init() -- initialize the bitmap index.
 *
 * Create the meta page, a new heap which stores the distinct values for
 * the attributes to be indexed, a btree index on this new heap for searching
 * those distinct values, and the first LOV page.
 */
void
_bitmap_init(Relation rel, Oid comptypeOid,
			 Oid heapOid, Oid indexOid, bool use_wal)
{
	BMMetaPage		metapage;
	Buffer			metabuf;
	Page			page;
	Buffer			buf;
	BMLOVItem 		lovItem;
	OffsetNumber	newOffset;
	Page			currLovPage;
	OffsetNumber	o;
  
	/* sanity check */
	if (RelationGetNumberOfBlocks(rel) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				errmsg("cannot initialize non-empty bitmap index \"%s\"",
				RelationGetRelationName(rel))));

	/* create the metapage */
	metabuf = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
	page = BufferGetPage(metabuf);
	Assert(PageIsNew(page));

	/* initialize the LOV metadata */
	_bitmap_create_lov_heapandindex(rel, comptypeOid,
									&(heapOid),
									&(indexOid));

	START_CRIT_SECTION();

	MarkBufferDirty(metabuf);

	/* initialize the metapage */
	PageInit(page, BufferGetPageSize(metabuf), 0);
	metapage = (BMMetaPage) PageGetContents(page);

	metapage->bm_lov_heapId = heapOid;
	metapage->bm_lov_indexId = indexOid;

	if (use_wal)
		_bitmap_log_metapage(rel, page);

	/* allocate the first LOV page. */
	buf = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
	_bitmap_init_lovpage(rel, buf);

	MarkBufferDirty(buf);

	currLovPage = BufferGetPage(buf);

	/* set the first item to support NULL value */
	lovItem = _bitmap_formitem(0);
	newOffset = OffsetNumberNext(PageGetMaxOffsetNumber(currLovPage));

	/*
	 * XXX: perhaps this could be a special page, with more efficient storage
	 * after all, we have fixed size data
	 */
	o = PageAddItem(currLovPage, (Item)lovItem, sizeof(BMLOVItemData),
                    newOffset, LP_USED);

	if (o == InvalidOffsetNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to add LOV item to \"%s\"",
				 RelationGetRelationName(rel))));

	metapage->bm_lov_lastpage = BufferGetBlockNumber(buf);

	if(use_wal)
		_bitmap_log_lovitem(rel, buf, newOffset, lovItem, metabuf, true);

	END_CRIT_SECTION();

	_bitmap_wrtbuf(buf);
	_bitmap_wrtbuf(metabuf);

	pfree(lovItem);
}

/*
 * Build a hash of the key we're indexing.
 */

static uint32
build_hash_key(const void *key, Size keysize)
{
	Datum *k = (Datum *)key;
	int i;
	uint32 hashkey = 0;

	for(i = 0; i < cur_bmbuild->natts; i++)
	{
		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		hashkey ^= DatumGetUInt32(FunctionCall1(&cur_bmbuild->hash_funcs[i],
												k[i]));
	}
	return hashkey;
}

/*
 * Test whether key1 matches key2. Since the equality functions may leak,
 * reset the temporary context at each call and do all equality calculation
 * in that context.
 */
static int
build_match_key(const void *key1, const void *key2, Size keysize)
{
	int i;
	MemoryContext old;
	int result = 0;

	MemoryContextReset(cur_bmbuild->tmpcxt);
	old = MemoryContextSwitchTo(cur_bmbuild->tmpcxt);

	for(i = 0; i < cur_bmbuild->natts; i++)
	{
		Datum attr1 = ((Datum *)key1)[i];
		Datum attr2 = ((Datum *)key2)[i];
        if (!DatumGetBool(FunctionCall2(&cur_bmbuild->eq_funcs[i],
                                        attr1, attr2)))
        {
            result = 1;     /* they aren't equal */
            break;
        }
	}
	MemoryContextSwitchTo(old);
	return result;
}

