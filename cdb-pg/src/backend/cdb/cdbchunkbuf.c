/*-------------------------------------------------------------------------
 *
 * cdbchunkbuf.c
 *
 * A byte string container which can hold as much data as will fit in
 * memory, using equal-sized chunks allocated from a CdbChunkPool; or
 * can transfer data to and from a workfile.
 *
 * Copyright (c) 2007-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/memnodes.h"                 /* MemoryContextData */
#include "storage/buffile.h"                /* BufFile */

#include "cdb/cdbchunkbuf.h"                /* me */


/* CdbChunkBuf_AddChunkFunc typedef matches these prototypes */
static Size
CdbChunkBuf_AddChunkHelper(struct CdbChunkBuf  *cbuf,
                           const char          *src,
                           Size                 len);
static Size
CdbChunkBuf_FileBuf_AddChunkHelper(CdbChunkBuf *cbuf,
                                   const char  *src,
                                   Size         len);

/* CdbChunkBuf_IterInitFunc typedef matches these prototypes */
static void
CdbChunkBuf_IterInitHelper(struct CdbChunkBuf          *cbuf,
                           struct CdbChunkBuf_Iterator *it);
static void
CdbChunkBuf_FileBuf_IterInitHelper(struct CdbChunkBuf          *cbuf,
                                   struct CdbChunkBuf_Iterator *it);

/* CdbChunkBuf_Iterator_NextChunkFunc typedef matches these prototypes */
static bool
CdbChunkBuf_Iterator_NextChunkHelper(struct CdbChunkBuf_Iterator *it);
static bool
CdbChunkBuf_FileBuf_NextChunkHelper(struct CdbChunkBuf_Iterator *it);

/* CdbChunkBuf_Iterator_SetPosFunc typedef matches these prototypes */
static void
CdbChunkBuf_FileBuf_SetPosHelper(struct CdbChunkBuf_Iterator   *it,
                                 CdbChunkBuf_Length             newoffset);
static void
CdbChunkBuf_Iterator_SetPosHelper(struct CdbChunkBuf_Iterator  *it,
                                  CdbChunkBuf_Length            newoffset);


/*-------------------------------------------------------------------------
 *                               CdbChunkBuf
 *-------------------------------------------------------------------------
 * There are two kinds of chunkbuf:
 *
 *  - Ordinary (or resident):  Holds all of its data in memory.  Expands by
 *      allocating chunks as needed.  Created by CdbChunkBuf_Create().
 *
 *  - Filebuf:  Writes to a workfile, and reads the data back in, using a
 *      one-chunk buffer.  Created by CdbChunkBuf_Create() followed by
 *      CdbChunkBuf_MakeFileBuf().
 */


/*-------------------------------------------------------------------------
 *                  Initialize/Reset caller's CdbChunkBuf
 *-------------------------------------------------------------------------
 */

/*
 * CdbChunkBuf_Init
 *
 * Initialize caller's CdbChunkBuf struct.
 *
 * 'chunkbuf' is the area to be initialized as a CdbChunkBuf structure.
 * 'initial_cell_area' is an optional memory area, properly aligned, where
 *      the first few cells of the chunklist can be stored.  It must be
 *      nearby the CdbChunkBuf itself, typically in the same struct or
 *      local variable stack frame.  This allows a small number of cells
 *      to be managed with good locality and no palloc/pfree.  If NULL,
 *      chunklist memory is allocated according to 'num_cells_expand'.
 * 'num_initial_cells' is the exact number of cells in the initial_cell_area.
 * 'num_cells_expand' is the suggested number of chunklist cells to be
 *      allocated whenever additional memory is needed.  The actual number
 *      will be rounded to fully utilize a power-of-2 allocation.  However,
 *      no expansion is done if num_cells_expand is zero; instead, after the
 *      initial cells are exhausted an error is raised.
 * 'context' is the memory context from which to allocate space for
 *      additional chunklist cells when there is no more room in
 *      initial_cell_area.  If NULL, the CurrentMemoryContext is used.
 * 'pool' is a CdbChunkPool from which to allocate chunks of memory to hold
 *      the byte string data.
 */
void
CdbChunkBuf_Init(CdbChunkBuf   *chunkbuf,
                 CdbPtrBuf_Ptr *initial_cell_area,
                 int            num_initial_cells,
                 int            num_cells_expand,
                 MemoryContext  context,
                 CdbChunkPool  *pool)
{
    memset(chunkbuf, 0, sizeof(*chunkbuf));

    /* Byte string data space will be allocated from caller's CdbChunkPool. */
    chunkbuf->chunkpool = pool;
    chunkbuf->chunksize = pool->bytesperchunk;

    /* Helper function will be called each time a chunk is to be allocated. */
    chunkbuf->addchunk = CdbChunkBuf_AddChunkHelper;

    /* Helper function for iterator initialization. */
    chunkbuf->iterinit = CdbChunkBuf_IterInitHelper;

    /* Initialize chunklist. */
    CdbPtrBuf_Init(&chunkbuf->chunklist,
                   initial_cell_area,
                   num_initial_cells,
                   num_cells_expand,
                   context);
}                               /* CdbChunkBuf_Init */


/*
 * CdbChunkBuf_InitEasy
 *
 * Initialize caller's CdbChunkBuf struct (easy-to-call version).
 *
 * 'chunkbuf' is the area to be initialized as a CdbChunkBuf.
 * 'pool' is a CdbChunkPool from which to allocate chunks of memory to hold
 *      the byte string data.
 */
void
CdbChunkBuf_InitEasy(CdbChunkBuf   *chunkbuf,
                     CdbChunkPool  *pool)
{
    CdbChunkBuf_Init(chunkbuf,
                     NULL,      /* initial_cell_area */
                     0,         /* num_initial_cells */
                     50,        /* num_cells_expand */
                     NULL,      /* context */
                     pool);
}                               /* CdbChunkBuf_InitEasy */


/*
 * CdbChunkBuf_Reset
 *
 * Release data memory held by a chunkbuf.
 *
 * If the chunkbuf is a filebuf and 'rescannable' is true, then the subfile
 * and its contents are retained although the buffer memory is released.
 *
 * If the chunkbuf is a filebuf and 'rescannable' is false, then the
 * CdbWorkfile_Subfile object is freed, and the chunkbuf becomes
 * ordinary.  The workfile itself is not affected, but the subfile
 * can no longer be accessed because we discard its extent list.
 *
 * Iterators (CdbChunkBuf_Iterator) become invalid when their chunkbuf
 * is reset.  Afterward the caller should abandon or reinitialize them.
 */
void
CdbChunkBuf_Reset(CdbChunkBuf *cbuf, bool rescannable)
{
    CdbPtrBuf_Iterator  it;
    CdbPtrBuf_Ptr      *cell;

    /* Reset workfile. */
    if (cbuf->subfile)
    {
        /* If keeping subfile for rescan, make sure all data has been written */
        if (rescannable)
            CdbChunkBuf_FileBuf_Flush(cbuf);

        /* Release subfile.  Make chunkbuf ordinary (no longer a filebuf). */
        else
        {
            CdbWorkfile_Subfile_Unpin(cbuf->subfile);
            cbuf->subfile = NULL;
            cbuf->bufendoffset = 0;
            cbuf->addchunk = CdbChunkBuf_AddChunkHelper;
        }
    }

    /* Reset buffer ptrs. */
    cbuf->len = 0;
    cbuf->freespace_beg = NULL;
    cbuf->freespace_end = NULL;
    cbuf->lastchunk = NULL;

    /*
     * Release data memory to the chunk pool.
     *
     * The chunklist could contain null pointers if a destructive iterator
     * (CdbChunkBuf_Iterator_MakeDestructive) has freed some chunks early.
     */
    CdbPtrBuf_Iterator_Init(&it, &cbuf->chunklist);
    while (NULL != (cell = CdbPtrBuf_Iterator_NextCell(&it)))
    {
        if (*cell)
        {
            CdbChunkPool_Free(cbuf->chunkpool, *cell);
            *cell = NULL;
        }
    }

    /* Empty the chunklist. */
    CdbPtrBuf_Reset(&cbuf->chunklist);
}                               /* CdbChunkBuf_Reset */


/*-------------------------------------------------------------------------
 *            Create/Destroy dynamically allocated CdbChunkBuf
 *-------------------------------------------------------------------------
 */


/*
 * CdbChunkBuf_Create
 *
 * Returns a newly created resident chunkbuf which can expand to hold as
 * much data as will fit in memory.
 *
 * 'num_initial_cells' is the suggested number of chunklist cells to be
 *      created immediately following the struct.  This allows a few
 *      cells to be managed with good locality and no palloc/pfree.
 *      The actual number will be rounded up to minimize wasted space.
 *      If zero, cell storage is allocated according to 'num_cells_expand'.
 * 'num_cells_expand' is the suggested number of ptr cells to be allocated
 *      whenever additional memory is needed.  The actual number will be
 *      rounded to fully utilize a power-of-2 allocation.  However, no
 *      expansion is done if num_cells_expand is zero; instead, after the
 *      initial cells are exhausted an error is raised.
 * 'context' is the memory context from which the CdbChunkBuf struct and
 *      chunklist are to be allocated.  If NULL, CurrentMemoryContext is used.
 * 'pool' is a CdbChunkPool from which to allocate chunks of memory to hold
 *      the byte string data.
 */
CdbChunkBuf *
CdbChunkBuf_Create(int              num_initial_cells,
                   int              num_cells_expand,
                   MemoryContext    context,
                   CdbChunkPool    *pool)
{
    return (CdbChunkBuf *)CdbChunkBuf_CreateWrap(sizeof(CdbChunkBuf),
                                                 0,
                                                 num_initial_cells,
                                                 num_cells_expand,
                                                 context,
                                                 pool);
}                               /* CdbChunkBuf_Create */


/*
 * CdbChunkBuf_CreateWrap
 *
 * Returns zeroed, newly allocated memory for a structure of given size,
 * with a CdbChunkBuf struct initialized at a given offset within the outer
 * structure, and followed by space for the chunklist's initial cells.
 *
 * 'sizeof_outer_struct' is the size of the struct in which the CdbChunkBuf is
 *      embedded.
 * 'offsetof_chunkbuf_in_struct' is the offset of the CdbChunkBuf within that.
 * 'num_initial_cells' is the suggested number of chunklist cells to be
 *      created immediately following the outer struct.  This allows a
 *      few cells to be managed with good locality and no palloc/pfree.
 *      The actual number will be rounded up to minimize wasted space.
 *      If zero, cell storage is allocated according to 'num_cells_expand'.
 * 'num_cells_expand' is the suggested number of chunklist cells to be
 *      allocated whenever additional memory is needed.  The actual number
 *      will be rounded to fully utilize a power-of-2 allocation.  However,
 *      no expansion is done if num_cells_expand is zero; instead, after the
 *      initial cells are exhausted an error is raised.
 * 'context' is the memory context from which the CdbChunkBuf struct and
 *      chunklist are to be allocated.  If NULL, CurrentMemoryContext is used.
 * 'pool' is a CdbChunkPool from which to allocate chunks of memory to hold
 *      the byte string data.
 */
void *
CdbChunkBuf_CreateWrap(size_t           sizeof_outer_struct,
                       size_t           offsetof_chunkbuf_in_struct,
                       int              num_initial_cells,
                       int              num_cells_expand,
                       MemoryContext    context,
                       CdbChunkPool    *pool)
{
    void           *outer;
    CdbChunkBuf    *cbuf;

    Assert(offsetof_chunkbuf_in_struct + sizeof(CdbChunkBuf) <= sizeof_outer_struct);

    /*
     * Allocate outer struct, zero it, and initialize empty CdbPtrBuf for
     * our chunklist with initial cell space following the outer struct.
     */
    outer = CdbPtrBuf_CreateWrap(sizeof_outer_struct,
                                 offsetof_chunkbuf_in_struct + offsetof(CdbChunkBuf, chunklist),
                                 num_initial_cells,
                                 num_cells_expand,
                                 context);

    /*
     * Complete the CdbChunkBuf initialization.
     */
    cbuf = (CdbChunkBuf *)((char *)outer + offsetof_chunkbuf_in_struct);

    /* Byte string data space will be allocated from caller's CdbChunkPool. */
    cbuf->chunkpool = pool;
    cbuf->chunksize = pool->bytesperchunk;

    /* Helper function will be called each time a chunk is to be allocated. */
    cbuf->addchunk = CdbChunkBuf_AddChunkHelper;

    /* Helper function for iterator initialization. */
    cbuf->iterinit = CdbChunkBuf_IterInitHelper;

    /* Caller can now finish initializing the outer struct (already cleared). */
    return outer;
}                               /* CdbChunkBuf_CreateWrap */


/*
 * CdbChunkBuf_Destroy
 *
 * Delete a CdbChunkBuf allocated by CdbChunkBuf_Create().
 *
 * Do not use this to free a chunkbuf allocated by CdbChunkBuf_CreateWrap()
 * unless offsetof_chunkbuf_in_struct == 0.
 *
 * NB: CdbChunkBuf_Reset() can be used to free the memory owned by a chunkbuf
 * without freeing the CdbChunkBuf struct itself.  That is safe for all
 * chunkbufs.
 *
 * (NB: An acceptable alternative for cleaning up would be to destroy or
 * reset the associated MemoryContext and CdbChunkPool, and close the
 * workfile if applicable.)
 */
void
CdbChunkBuf_Destroy(CdbChunkBuf *cbuf)
{
    CdbChunkBuf_Reset(cbuf, false);
    pfree(cbuf);
}                               /* CdbChunkBuf_Destroy */


/*-------------------------------------------------------------------------
 *                           CdbChunkBuf_FileBuf
 *-------------------------------------------------------------------------
 * Specialization of CdbChunkBuf for writing a byte stream to a workfile and
 * reading it back in.  Uses the CdbWorkfile_Subfile interface so more than
 * one stream can coexist in the same workfile if desired.
 *
 * The CdbChunkBuf_FileBuf_xxx functions use the CdbChunkBuf struct with
 * non-NULL 'subfile' field.  There is no CdbChunkBuf_FileBuf struct.
 *
 * Initially, a new filebuf has no buffer space.  One chunk of buffer space
 * is allocated when data is appended to the filebuf for the first time.
 * When the chunk fills up, it is emptied to the workfile.
 *
 * At most one filebuf can append to a given subfile.  While appending,
 * until CdbChunkBuf_FileBuf_Flush() is called, there should be no other
 * filebufs referencing the same subfile.  (To relax this restriction, we
 * would need some way to keep all of their 'len' fields updated as the
 * subfile grows.)
 *
 * Any number of filebufs can share a subfile for read-only access.  To read,
 * use a CdbChunkBuf_Iterator.  Each filebuf should be referenced by no more
 * than one CdbChunkBuf_Iterator at a time, since each iterator needs a buffer
 * of its own.
 *
 * After CdbChunkBuf_FileBuf_Flush() has been called, you should not append
 * more data to the subfile, unless its last chunk was completely filled.
 * The reason is as follows.  When an incompletely filled buffer is flushed,
 * we might write additional uninitialized data, rounding out the buffer to
 * facilitate direct I/O.  Any such roundoff needs to be skipped or excluded
 * when the subfile is read back in.  The CdbChunkBuf_Iterator conceals
 * roundoff at the end of a subfile, but for now, doesn't support reading
 * from subfiles which were flushed with roundoff and then had more data
 * appended.  (If ever needed, a CdbWorkfile_Subfile_Iterator can read those.)
 */

/*
 * CdbChunkBuf_MakeFileBuf
 *
 * Makes an ordinary chunkbuf into a filebuf.
 *
 * On entry, the chunkbuf must have no chunks.
 *
 * If caller associates the same subfile with more than one filebuf, it is the
 * caller's responsibility to call CdbWorkfile_Subfile_Pin() as appropriate.
 *
 * To unpin the subfile and change the filebuf back to an ordinary chunkbuf,
 * call CdbChunkBuf_Reset(cbuf, false).
 */
void
CdbChunkBuf_MakeFileBuf(CdbChunkBuf *cbuf, CdbWorkfile_Subfile *subfile)
{
    Assert(cbuf->len == 0 &&
           !cbuf->lastchunk &&
           !cbuf->subfile &&
           subfile &&
           subfile->workfile);

    cbuf->subfile = subfile;
    cbuf->bufendoffset = 0;

    cbuf->addchunk = CdbChunkBuf_FileBuf_AddChunkHelper;
    cbuf->iterinit = CdbChunkBuf_FileBuf_IterInitHelper;
}                               /* CdbChunkBuf_MakeFileBuf */


/*
 * CdbChunkBuf_FileBuf_Flush
 *
 * If buffer is nonempty, write the buffer contents to the associated
 * subfile.  The filebuf state is not changed.
 *
 * This function should be called when there is no more data to be appended
 * to the subfile, before beginning to read from the subfile.
 */
void
CdbChunkBuf_FileBuf_Flush(CdbChunkBuf *cbuf)
{
    CdbWorkfile_Subfile    *subfile = cbuf->subfile;

    Assert(subfile);

    /* If data has been added to buffer, append it to the subfile. */
    if (cbuf->len > subfile->len)
    {
        Size                bytesinbuf = cbuf->freespace_beg - cbuf->lastchunk;
        CdbChunkBuf_Length  bufbegoffset = cbuf->len - bytesinbuf;
        Size                alreadywritten = 0;
        Size                bytestowrite = bytesinbuf;
        Size                rounded;
        int                 boundary;

        Assert(subfile->workfile &&
               bytesinbuf <= cbuf->chunksize);

        /* Filebuf's end of data should lie within the buffer's current block */
        Assert(bufbegoffset + cbuf->chunksize == cbuf->bufendoffset);

        /* All earlier data has already been written to subfile. */
        Assert(subfile->len >= bufbegoffset);

        /* Determine an alignment which we hope is efficient for direct I/O. */
        boundary = Min(BLCKSZ, (int)cbuf->chunksize & -(int)cbuf->chunksize);

        /*
         * If buffer was already flushed and then more data was added,
         * truncate the subfile back to a suitably aligned point within
         * the block, and rewrite from there.
         */
        if (subfile->len > bufbegoffset)
        {
            int     chop = (int)subfile->len & (boundary - 1);

            Assert(subfile->len >= bufbegoffset + chop);
            CdbWorkfile_Subfile_Truncate(subfile, subfile->len - chop);
            alreadywritten = (Size)(subfile->len - bufbegoffset);
            bytestowrite -= alreadywritten;
        }

        /* Round the size up; otherwise filesystem might not use direct i/o. */
        rounded = (Size)TYPEALIGN(boundary, bytestowrite);

        /* Write buffer contents. */
        CdbWorkfile_Subfile_Append(subfile, cbuf->lastchunk + alreadywritten, rounded);

        Assert(subfile->len >= cbuf->len &&
               subfile->len <= cbuf->bufendoffset);

        /*
         * Bytes skipped by rounding are to be logically excluded from the
         * subfile data, and ignored when the data is read back in.
         * Adjust the subfile data length to chop off the excess.
         */
        if (rounded > bytestowrite)
            CdbWorkfile_Subfile_Truncate(subfile, cbuf->len);

        /*
         * We don't advance the buffer position or empty the buffer.  Let the
         * same block stay in the buffer until a different block is needed.
         */
    }

    Assert(cbuf->len == subfile->len);
}                               /* CdbChunkBuf_FileBuf_Flush */


/*-------------------------------------------------------------------------
 *                  CdbChunkBuf_FileBuf helper functions
 *-------------------------------------------------------------------------
 */

/*
 * CdbChunkBuf_FileBuf_AddChunkHelper
 *
 * Private internal helper function called by CdbChunkBuf_AppendBytes()
 * for workfile output when there is more data than will fit in the buffer.
 * On return, the filebuf's buffer exists and is empty.
 *
 * 'src' optionally points to some data to be appended.  Callers can specify
 * src and len to allow the helper to take care of appending some or all of
 * this data.  This lets us write to the workfile directly from the caller's
 * buffer without moving all the data through our own buffer.
 *
 * 'len' is the maximum number of bytes which the helper may append from 'src'.
 *
 * Returns the number of bytes (<= 'len') which this function has transferred
 * from 'src' to the workfile.  On return, the caller is responsible for
 * transferring whatever is left over, calling again when still more space
 * is needed.
 */
Size
CdbChunkBuf_FileBuf_AddChunkHelper(CdbChunkBuf *cbuf, const char *src, Size len)
{
    Size    chunksize = cbuf->chunksize;
    Size    bytesmoved = 0;

    Assert(cbuf->subfile);

    /* Buffer should be exactly full (or both ptrs NULL if not allocated yet) */
    Assert(cbuf->freespace_beg == cbuf->freespace_end);

    /* Current end of data coincides with end of the buffer's current block. */
    Assert(cbuf->len == cbuf->bufendoffset);

    /* All earlier data has already been written to subfile. */
    Assert(cbuf->subfile->len + chunksize >= cbuf->bufendoffset);

    /*
     * On first call, allocate one buffer.
     */
    if (cbuf->lastchunk == NULL)
        CdbChunkBuf_AddChunkHelper(cbuf, NULL, 0);

    /*
     * If we have a buffer, it's full.  Write it.
     */
    else
    {
        /* Flush() handles case where some of the buffer was written already. */
        if (cbuf->subfile->len + chunksize > cbuf->bufendoffset)
            CdbChunkBuf_FileBuf_Flush(cbuf);

        /* Write to end of workfile. */
        else
            CdbWorkfile_Subfile_Append(cbuf->subfile, cbuf->lastchunk, chunksize);

        /* Buffer is empty now. */
        cbuf->freespace_beg = cbuf->lastchunk;
    }

    /*
     * Write a multiple of chunksize directly from caller's area to workfile.
     */
    if (len >= chunksize)
    {
        bytesmoved = len - (len % chunksize);
        CdbWorkfile_Subfile_Append(cbuf->subfile, src, bytesmoved);
        cbuf->len += bytesmoved;
    }

    /*
     * Advance buffer position.  A new empty block now occupies the buffer.
     * It logically follows the end of the subfile.
     */
    cbuf->bufendoffset = cbuf->len + chunksize;

    Assert(cbuf->len == cbuf->subfile->len);
    Assert(cbuf->bufendoffset % chunksize == 0);

    return bytesmoved;
}                               /* CdbChunkBuf_FileBuf_AddChunkHelper */


/*
 * CdbChunkBuf_FileBuf_IterInitHelper
 *
 * Called by CdbChunkBuf_Iterator_Init() to finish initializing an iterator
 * over a filebuf.
 */
void
CdbChunkBuf_FileBuf_IterInitHelper(struct CdbChunkBuf          *cbuf,
                                   struct CdbChunkBuf_Iterator *it)
{
    it->nextchunk = CdbChunkBuf_FileBuf_NextChunkHelper;
    it->setpos = CdbChunkBuf_FileBuf_SetPosHelper;
    memset(&it->chunkit, 0, sizeof(it->chunkit));
    CdbWorkfile_Subfile_Iterator_Init(&it->subfileit, cbuf->subfile);
}                               /* CdbChunkBuf_FileBuf_IterInitHelper */


/*
 * CdbChunkBuf_FileBuf_NextChunkHelper
 *
 * Private internal helper called by CdbChunkBuf_Iterator_NextChunk()
 * when iterating over a filebuf.
 *
 * Advance the iterator to the next chunk.  Returns true if successful,
 * false if there is not another chunk.
 */
bool
CdbChunkBuf_FileBuf_NextChunkHelper(struct CdbChunkBuf_Iterator *it)
{
    CdbChunkBuf        *cbuf = it->chunkbuf;
    char               *chunk;
    CdbChunkBuf_Length  newchunkoffset = it->curchunkendoffset;
    CdbChunkBuf_Length  readendoffset;
    Size                nwant;
    Size                shortage;

    Assert(cbuf &&
           cbuf->subfile);

    /* Seen it all?  Then quit, and leave the iterator undisturbed. */
    if (newchunkoffset >= cbuf->len)
        return false;

    /* If the filebuf doesn't have a chunk yet, allocate one. */
    if (cbuf->lastchunk == NULL)
        CdbChunkBuf_AddChunkHelper(cbuf, NULL, 0);

    /* Skip reading if the desired block is already in the buffer. */
    else if (newchunkoffset + cbuf->chunksize == cbuf->bufendoffset)
    {
        it->curchunkoffset = newchunkoffset;
        it->curchunkendoffset = newchunkoffset + cbuf->chunksize;
        it->curpos = cbuf->lastchunk;
        it->curchunk = cbuf->lastchunk;
        return true;
    }

    /* Before reading, write any unwritten data remaining in buffer. */
    else if (cbuf->len > cbuf->subfile->len)
        CdbChunkBuf_FileBuf_Flush(cbuf);

    /* Position the subfile iterator. */
    CdbWorkfile_Subfile_Iterator_SetPos(&it->subfileit, newchunkoffset);

    /*
     * Read into the buffer.
     *
     * We request a full chunk even when reading the final block, because a
     * short request might defeat direct I/O and force the lower layers to
     * interpose extra buffering.  We hope the host file system will read
     * directly into our buffer.
     */
    chunk = cbuf->lastchunk;
    nwant = cbuf->chunksize;
    shortage = CdbWorkfile_Subfile_Iterator_Read(&it->subfileit, chunk, nwant);

    /* Note subfile's new position (the offset at which reading stopped). */
    readendoffset = newchunkoffset + nwant - shortage;
    Assert(readendoffset == CdbWorkfile_Subfile_Iterator_GetPos(&it->subfileit));

    /*
     * If we got less than our request, it's ok provided the truncation
     * did not lose real data (before cbuf->len).  Beyond cbuf->len may be
     * an unknown amount of uninitialized data which was written to fill out
     * the block; we don't care how much of that was read back in.
     */
    if (shortage > 0 &&
        readendoffset < cbuf->len)
        ereport(ERROR, (errmsg("Workfile read error: Unexpected eof."),
                        errdetail("Workfile offset=" UINT64_FORMAT " "
                                  "size=" UINT64_FORMAT ".  "
                                  "Subfile offset=" UINT64_FORMAT " "
                                  "size=" UINT64_FORMAT ".",
                                  it->subfileit.extent->seekbeg,
                                  it->subfileit.workfile->len,
                                  readendoffset,
                                  cbuf->len)
                        ));

    /* Update iterator position. */
    it->curpos = chunk;
    it->curchunk = chunk;
    it->curchunkoffset = newchunkoffset;
    it->curchunkendoffset = newchunkoffset + nwant;

    /* Update buffer ptrs in chunkbuf. */
    cbuf->bufendoffset = newchunkoffset + nwant;
    if (it->curchunkendoffset < cbuf->len)
        cbuf->freespace_beg = chunk + nwant;
    else
        cbuf->freespace_beg = cbuf->len - newchunkoffset + chunk;

    Assert(cbuf->freespace_end == chunk + nwant);
    return true;
}                               /* CdbChunkBuf_FileBuf_NextChunkHelper */


/*
 * CdbChunkBuf_FileBuf_SetPosHelper
 *
 * Private internal helper called by CdbChunkBuf_Iterator_SetPos() when
 * iterating over a filebuf and 'newoffset' is outside the iterator's
 * current chunk.
 *
 * Positions the iterator at offset 'newoffset' from the beginning of
 * the subfile.  0 <= newoffset <= it->chunkbuf->len
 *
 * On return, it->curpos points to a substring of length 0 or more, which the
 * caller may access starting at the new position.  The substring ends at the
 * end of all the data, or at the end of the current chunk, whichever is first.
 */
void
CdbChunkBuf_FileBuf_SetPosHelper(struct CdbChunkBuf_Iterator   *it,
                                 CdbChunkBuf_Length             newoffset)
{
    CdbChunkBuf        *cbuf = it->chunkbuf;
    Size                offsetinchunk = (Size)(newoffset % cbuf->chunksize);
    CdbChunkBuf_Length  newchunkoffset = newoffset - offsetinchunk;

    Assert(newoffset <= cbuf->len);

    /* If positioning to a block boundary and it's the end, use last block. */
    if (offsetinchunk == 0 &&
        newoffset == cbuf->len)
    {
        Assert(newchunkoffset >= cbuf->chunksize);
        newchunkoffset -= cbuf->chunksize;
        offsetinchunk = cbuf->chunksize;
    }

    /* Read the block into the buffer. */
    it->curchunkendoffset = newchunkoffset;
    CdbChunkBuf_FileBuf_NextChunkHelper(it);

    /* Set new position within chunk. */
    it->curpos += offsetinchunk;

    Assert(newoffset == CdbChunkBuf_Iterator_GetPos(it));
}                               /* CdbChunkBuf_FileBuf_SetPosHelper */


/*-------------------------------------------------------------------------
 *                      CdbChunkBuf helper functions
 *-------------------------------------------------------------------------
 */

/*
 * CdbChunkBuf_AddChunkHelper
 *
 * Default helper function called by CdbChunkBuf_Append() to allocate
 * another chunk for an ordinary resident chunkbuf.  On return, the
 * chunkbuf's last chunk exists and is empty.
 *
 * 'src' optionally points to some data to be appended.  Callers can specify
 * src and len to allow the helper to append some or all of this data.
 *
 * 'len' is the maximum number of bytes which the helper may append from 'src'.
 *
 * Returns the number of bytes (<= 'len') which the helper has transferred
 * from 'src'.  On return, the caller is responsible for transferring
 * whatever is left over, calling again when still more space is needed.
 *
 * This function just allocates a chunk and lets the caller move the data.
 * It doesn't use 'src' and 'len'.
 */
Size
CdbChunkBuf_AddChunkHelper(struct CdbChunkBuf  *cbuf,
                           const char          *src,
                           Size                 len)
{
    UnusedArg(src);
    UnusedArg(len);

    cbuf->lastchunk = CdbChunkPool_Alloc(cbuf->chunkpool);
    CdbPtrBuf_Append(&cbuf->chunklist, cbuf->lastchunk);
    cbuf->freespace_beg = cbuf->lastchunk;
    cbuf->freespace_end = cbuf->lastchunk + cbuf->chunksize;
    return 0;
}                               /* CdbChunkBuf_AddChunkHelper */


/*
 * CdbChunkBuf_AllocHelper
 *
 * Helper called by CdbChunkBuf_Alloc when the last chunk doesn't have room
 * to append 'len' bytes of data.
 */
void
CdbChunkBuf_AllocHelper(CdbChunkBuf *cbuf, Size len)
{
    Assert((Size)(cbuf->freespace_end - cbuf->freespace_beg) <= cbuf->chunksize);
    Assert(cbuf->lastchunk
            ? (cbuf->chunksize == (Size)(cbuf->freespace_end - cbuf->lastchunk))
            : (!cbuf->freespace_beg && !cbuf->freespace_end));

    if (len > cbuf->chunksize)
        ereport(ERROR, (errmsg_internal("Invalid length for CdbChunkBuf_Alloc. "
                                        "len=%ld exceeds limit of %ld bytes.",
                                        (long)len,
                                        (long)cbuf->chunksize)
                        ));

    /* Any space left in the current chunk is wasted. */
    cbuf->len += cbuf->freespace_end - cbuf->freespace_beg;
    cbuf->freespace_beg = cbuf->freespace_end;

    /* Allocate a new chunk. */
    cbuf->addchunk(cbuf, NULL, 0);
}                               /* CdbChunkBuf_AllocHelper */


/*
 * CdbChunkBuf_AppendHelper
 *
 * Helper called by CdbChunkBuf_Append when the last chunk doesn't have room
 * to append 'len' bytes of data.
 */
void
CdbChunkBuf_AppendHelper(CdbChunkBuf *cbuf, const void *src, Size len)
{
    Size        nbytes_free = cbuf->freespace_end - cbuf->freespace_beg;
    const char *pos = (const char *)src;

    Assert(pos);
    Assert(nbytes_free <= cbuf->chunksize);
    Assert(cbuf->lastchunk
            ? (cbuf->chunksize == (Size)(cbuf->freespace_end - cbuf->lastchunk))
            : (!cbuf->freespace_beg && !cbuf->freespace_end));

    /* Too much data for this chunk? */
    while (len > nbytes_free)
    {
        Size    bytesmoved;

        /* Finish filling this chunk. */
        if (nbytes_free > 0)
        {
            memcpy(cbuf->freespace_beg, pos, nbytes_free);
            cbuf->freespace_beg += nbytes_free;
            cbuf->len += nbytes_free;
            pos += nbytes_free;
            len -= nbytes_free;
        }

        /* Make room for more data.  Helper can optionally move some data. */
        bytesmoved = cbuf->addchunk(cbuf, pos, len);
        pos += bytesmoved;
        len -= bytesmoved;
        nbytes_free = cbuf->freespace_end - cbuf->freespace_beg;
    }

    Assert(cbuf->subfile
           ? (cbuf->len == cbuf->bufendoffset - nbytes_free)
           : (cbuf->len == cbuf->chunksize * CdbPtrBuf_Length(&cbuf->chunklist)
                            - nbytes_free));

    /* Copy the rest. */
    if (len > 0)
    {
        memcpy(cbuf->freespace_beg, pos, len);
        cbuf->freespace_beg += len;
        cbuf->len += len;
    }
}                               /* CdbChunkBuf_AppendHelper */


/*
 * CdbChunkBuf_IterInitHelper
 *
 * Called by CdbChunkBuf_Iterator_Init() to finish setting up an iterator
 * over an ordinary resident tuplebuf.
 */
void
CdbChunkBuf_IterInitHelper(struct CdbChunkBuf          *cbuf,
                           struct CdbChunkBuf_Iterator *it)
{
    it->nextchunk = CdbChunkBuf_Iterator_NextChunkHelper;
    it->setpos = CdbChunkBuf_Iterator_SetPosHelper;
    CdbPtrBuf_Iterator_Init(&it->chunkit, &cbuf->chunklist);
    it->subfileit.workfile = NULL;
}                               /* CdbChunkBuf_IterInitHelper */


/*-------------------------------------------------------------------------
 *                      CdbChunkBuf data manipulation
 *-------------------------------------------------------------------------
 */

/*
 * CdbChunkBuf_AppendBuf
 *
 * Append the contents of the 'srcbuf' chunkbuf to the 'tgtbuf' chunkbuf,
 * for 'len' bytes (0 <= len <= srcbuf->len).  The srcbuf is not changed.
 *
 * When 'srcbuf' is a filebuf, this appends the file contents first,
 * followed by any unflushed data left in the srcbuf memory buffer.
 */
void
CdbChunkBuf_AppendBuf(CdbChunkBuf          *tgtbuf,
                      CdbChunkBuf          *srcbuf,
                      CdbChunkBuf_Length    len)
{
    const char *src;

    Assert(srcbuf && tgtbuf);
    Insist(len <= srcbuf->len);

    /* Append from a filebuf? */
    if (srcbuf->subfile)
    {
        CdbWorkfile_Subfile_Iterator    it;

        Assert(srcbuf->subfile->len <= srcbuf->len);

        /* Read 'len' bytes or whole subfile, whichever is shorter. */
        CdbWorkfile_Subfile_Iterator_Init(&it, srcbuf->subfile);
        len = CdbChunkBuf_AppendSubfile(tgtbuf, &it, len);

        /* Append unwritten data still in memory. */
        if (len > 0)
        {
            src = srcbuf->freespace_beg - (srcbuf->len - srcbuf->subfile->len);
            Assert(src >= srcbuf->lastchunk &&
                   len <= srcbuf->chunksize &&
                   src + len <= srcbuf->freespace_beg &&
                   srcbuf->len <= srcbuf->bufendoffset);
            CdbChunkBuf_Append(tgtbuf, src, len);
        }
    }

    /* srcbuf is an ordinary resident chunkbuf */
    else
    {
        CdbPtrBuf_Iterator  chunkit;

        CdbPtrBuf_Iterator_Init(&chunkit, &srcbuf->chunklist);

        /* Append full source chunks to target. */
        for (; len > srcbuf->chunksize; len -= srcbuf->chunksize)
        {
            src = CdbPtrBuf_Iterator_Next(&chunkit);
            CdbChunkBuf_Append(tgtbuf, src, srcbuf->chunksize);
        }

        /* Append the remainder of the requested data. */
        if (len > 0)
        {
            src = CdbPtrBuf_Iterator_Next(&chunkit);
            CdbChunkBuf_Append(tgtbuf, src, len);
        }
    }
}                               /* CdbChunkBuf_AppendBuf */


/*
 * CdbChunkBuf_AppendSubfile
 *
 * Reads from workfile and appends to chunkbuf.
 *
 * Using a CdbWorkfile_Subfile_Iterator provided by the caller, starts
 * reading from the iterator's current position, and continues for length
 * 'len' or until eof.
 *
 * Returns 0 if 'len' bytes were read and appended successfully.  If end
 * of subfile is encountered, returns the original value of 'len' minus
 * the number of bytes read.
 */
CdbChunkBuf_Length
CdbChunkBuf_AppendSubfile(CdbChunkBuf                  *cbuf,
                          CdbWorkfile_Subfile_Iterator *it,
                          CdbChunkBuf_Length            len)
{
    Size    nbytes_free = cbuf->freespace_end - cbuf->freespace_beg;
    Size    nread;
    Size    shortage;

    Assert(it &&
           it->workfile);
    Assert(nbytes_free <= cbuf->chunksize);
    Assert(cbuf->lastchunk
            ? (cbuf->chunksize == (Size)(cbuf->freespace_end - cbuf->lastchunk))
            : (!cbuf->freespace_beg && !cbuf->freespace_end));

    /* Want to fill this whole chunk and more? */
    while (len > nbytes_free)
    {
        /* Fill this chunk's free area with data from file. */
        if (nbytes_free > 0)
        {
            shortage = CdbWorkfile_Subfile_Iterator_Read(it,
                                                         cbuf->freespace_beg,
                                                         nbytes_free);
            nread = nbytes_free - shortage;
            len -= nread;
            cbuf->len += nread;
            cbuf->freespace_beg += nread;
            if (shortage > 0)
                return len;
        }

        /* Chunk is full.  Allocate another chunk. */
        cbuf->addchunk(cbuf, NULL, 0);
        nbytes_free = cbuf->freespace_end - cbuf->freespace_beg;
    }

    /* Read the remainder of requested data into this chunk's free area. */
    if (len > 0)
    {
        /*
         * If reading to end of subfile, let the subfile iterator use the
         * rest of the buffer, in case it can pad out the read to a multiple
         * of a host-filesystem-dependent size which will permit direct i/o.
         */
        if (len >= CdbWorkfile_Subfile_Iterator_Length(it) -
                   CdbWorkfile_Subfile_Iterator_GetPos(it))
        {
            shortage = CdbWorkfile_Subfile_Iterator_Read(it,
                                                         cbuf->freespace_beg,
                                                         nbytes_free);
            nread = nbytes_free - shortage;
            if (nread > len)
                nread = len;
        }

        /* Won't reach end of subfile.  Read only the requested amount. */
        else
        {
            shortage = CdbWorkfile_Subfile_Iterator_Read(it,
                                                         cbuf->freespace_beg,
                                                         (Size)len);
            Insist(shortage == 0);
            nread = (Size)len - shortage;
        }

        len -= nread;
        cbuf->len += nread;
        cbuf->freespace_beg += nread;
    }

    Assert(cbuf->subfile
           ? (cbuf->len == cbuf->bufendoffset
                            - (cbuf->freespace_end - cbuf->freespace_beg))
           : (cbuf->len == cbuf->chunksize * CdbPtrBuf_Length(&cbuf->chunklist)
                            - (cbuf->freespace_end - cbuf->freespace_beg)));
    return len;
}                               /* CdbChunkBuf_AppendSubfile */


/*
 * CdbChunkBuf_Peek
 *
 * Copy data from specified offset within a chunkbuf in memory.  The source
 * area should not overlap with the target.
 */
void
CdbChunkBuf_Peek(CdbChunkBuf       *cbuf,
                 void              *copyto,
                 CdbChunkBuf_Length offset,
                 Size               nbytes)
{
    CdbChunkBuf_Length      endoff = offset + nbytes;
    CdbChunkBuf_Iterator    it;

    Insist(offset <= endoff &&
           endoff <= cbuf->len);

    CdbChunkBuf_Iterator_Init(&it, cbuf);
    CdbChunkBuf_Iterator_SetPos(&it, offset);
    while (offset < endoff)
    {
        CdbChunkBuf_Length  boff;
        CdbChunkBuf_Length  eoff;
        Size    nb;

        /* At end of chunk, advance to next. */
        if (offset == it.curchunkendoffset)
            CdbChunkBuf_Iterator_NextChunk(&it);

        Assert(it.curchunk &&
               offset >= it.curchunkoffset &&
               offset < it.curchunkendoffset);

        boff = Max(it.curchunkoffset, offset);
        eoff = Min(it.curchunkendoffset, endoff);
        nb = (Size)(eoff - boff);

        memcpy(copyto, boff - it.curchunkoffset + it.curchunk, nb);

        offset += nb;
        copyto = (char *)copyto + nb;
    }
}                               /* CdbChunkBuf_Peek */


/*
 * CdbChunkBuf_Poke
 *
 * Overwrite data at specified offset within a chunkbuf in memory.  The
 * source area should not overlap with the target.
 */
void
CdbChunkBuf_Poke(CdbChunkBuf       *cbuf,
                 CdbChunkBuf_Length offset,
                 const void        *copyfrom,
                 Size               nbytes)
{
    CdbChunkBuf_Length      endoff = offset + nbytes;
    CdbChunkBuf_Iterator    it;

    Insist(offset <= endoff &&
           endoff <= cbuf->len);

    CdbChunkBuf_Iterator_Init(&it, cbuf);
    CdbChunkBuf_Iterator_SetPos(&it, offset);
    while (offset < endoff)
    {
        CdbChunkBuf_Length  boff;
        CdbChunkBuf_Length  eoff;
        Size    nb;

        /* At end of chunk, advance to next. */
        if (offset == it.curchunkendoffset)
            CdbChunkBuf_Iterator_NextChunk(&it);

        Assert(it.curchunk &&
               offset >= it.curchunkoffset &&
               offset < it.curchunkendoffset);

        boff = Max(it.curchunkoffset, offset);
        eoff = Min(it.curchunkendoffset, endoff);
        nb = (Size)(eoff - boff);

        memcpy(boff - it.curchunkoffset + it.curchunk, copyfrom, nb);

        offset += nb;
        copyfrom = (char *)copyfrom + nb;
    }
}                               /* CdbChunkBuf_Poke */


/*-------------------------------------------------------------------------
 *                          CdbChunkBuf_Iterator
 *-------------------------------------------------------------------------
 */

/*
 * CdbChunkBuf_Iterator_Init
 */
void
CdbChunkBuf_Iterator_Init(CdbChunkBuf_Iterator *it, CdbChunkBuf *cbuf)
{
    Assert(cbuf &&
           cbuf->chunklist.cellbuf.cellbytes == sizeof(it->curchunk));

    it->curpos = NULL;
    it->curchunk = NULL;
    it->curchunkoffset = 0;
    it->curchunkendoffset = 0;

    it->chunkbuf = cbuf;

    /* Call helper for initialization specific to the type of chunkbuf. */
    cbuf->iterinit(cbuf, it);
}                               /* CdbChunkBuf_Iterator_Init */


/*
 * CdbChunkBuf_Iterator_NextChunkHelper
 *
 * Advance the iterator to the next chunk.  Returns true if successful,
 * false if there is not another chunk.
 */
bool
CdbChunkBuf_Iterator_NextChunkHelper(struct CdbChunkBuf_Iterator *it)
{
    CdbChunkBuf        *cbuf = it->chunkbuf;
    char               *chunk;
    CdbChunkBuf_Length  newchunkoffset = it->curchunkendoffset;

    Assert((it->curchunk == NULL)
            ? (it->curchunkoffset == 0 &&
               it->curchunkendoffset == 0 &&
               CdbPtrBuf_Iterator_AtBegin(&it->chunkit))
            : (it->curchunkendoffset == it->curchunkoffset + cbuf->chunksize &&
               !CdbPtrBuf_Iterator_AtBegin(&it->chunkit)));

    /* Advance the chunklist iterator and get next chunk ptr. */
    chunk = CdbPtrBuf_Iterator_Next(&it->chunkit);

    /* Seen it all?  Then quit, and leave the iterator undisturbed. */
    if (!chunk)
        return false;

    it->curpos = chunk;
    it->curchunk = chunk;
    it->curchunkoffset = newchunkoffset;
    it->curchunkendoffset = newchunkoffset + cbuf->chunksize;
    return true;
}                               /* CdbChunkBuf_Iterator_NextChunkHelper */


/*
 * CdbChunkBuf_Iterator_SetPosHelper
 *
 * Called by inline function CdbChunkBuf_Iterator_SetPos() when 'newoffset'
 * is outside the iterator's current chunk.
 *
 * Positions the iterator at offset 'newoffset' from the beginning of the
 * chunkbuf.    0 <= newoffset <= it->chunkbuf->len
 *
 * On return, it->curpos points to a substring of length 0 or more, which the
 * caller may access starting at the new position.  The substring ends at the
 * end of all the data, or at the end of the current chunk, whichever is first.
 */
void
CdbChunkBuf_Iterator_SetPosHelper(struct CdbChunkBuf_Iterator  *it,
                                  CdbChunkBuf_Length            newoffset)
{
    CdbChunkBuf    *cbuf = it->chunkbuf;
    Size            ichunk = (Size)(newoffset / cbuf->chunksize);
    Size            offsetinchunk = (Size)(newoffset % cbuf->chunksize);
    Size            nchunk = CdbPtrBuf_Iterator_Length(&it->chunkit);
    char           *chunk;

    Assert(newoffset <= cbuf->len &&
           ichunk <= nchunk);

    /* On chunk boundary at end of chunkbuf?  Point to end of last chunk. */
    if (offsetinchunk == 0 &&
        ichunk == nchunk)
    {
        Assert(ichunk > 0);
        ichunk--;
        offsetinchunk = cbuf->chunksize;
    }

    /* Set the chunklist iterator to the new position. */
    CdbPtrBuf_Iterator_SetPos(&it->chunkit, (long)ichunk);

    /* Get requested chunk ptr. */
    chunk = CdbPtrBuf_Iterator_Next(&it->chunkit);
    Assert(chunk);

    /* Set new position within chunk. */
    it->curpos = chunk + offsetinchunk;
    it->curchunk = chunk;
    it->curchunkoffset = newoffset - offsetinchunk;
    it->curchunkendoffset = it->curchunkoffset + cbuf->chunksize;
}                               /* CdbChunkBuf_Iterator_SetPosHelper */


/*-------------------------------------------------------------------------
 *                   CdbChunkBuf_Iterator (Destructive)
 *-------------------------------------------------------------------------
 */

/* Helpers for inline functions */
static bool
CdbChunkBuf_DestructiveIterator_NextChunkHelper(struct CdbChunkBuf_Iterator *it)
{
    CdbPtrBuf_Ptr   oldchunk = it->curchunk;
    bool            advanced;

    advanced = CdbChunkBuf_Iterator_NextChunkHelper(it);

    /* At end, free any remaining chunks and reset to a normal empty state. */
    if (!advanced)
    {
        CdbChunkBuf_Reset(it->chunkbuf, false);
        CdbChunkBuf_Iterator_Init(it, it->chunkbuf);
    }

    /* If we had a chunk before advancing, recycle it and zap its saved ptr. */
    else if (oldchunk)
    {
        CdbPtrBuf_Ptr  *oldcell = CdbPtrBuf_Iterator_CellAt(&it->chunkit, -2);

        Assert(oldcell &&
               *oldcell == oldchunk);

        CdbChunkPool_Free(it->chunkbuf->chunkpool, oldchunk);

        *oldcell = NULL;
    }

    return advanced;
}                               /* CdbChunkBuf_DestructiveIterator_NextChunkHelper */

static void
CdbChunkBuf_DestructiveIterator_SetPosHelper(struct CdbChunkBuf_Iterator *it,
                                             CdbChunkBuf_Length           newoffset)
{
    UnusedArg(it);
    UnusedArg(newoffset);
    Insist(0);                  /* unsupported */
}                               /* CdbChunkBuf_DestructiveIterator_SetPosHelper */


/*
 * CdbChunkBuf_Iterator_MakeDestructive
 *
 * Sets an ordinary chunkbuf iterator into destructive mode.  Each time the
 * iterator advances from one chunk to the next, the old chunk is freed to
 * the chunkpool.  When the iterator reaches the end, the chunkbuf is reset
 * and the iterator is reinitialized.
 *
 * If the caller does not proceed all the way to the end of a destructive
 * iteration, then the automatic reset does not take place; it will then
 * be the caller's responsibility to reset or destroy the chunkbuf.
 *
 * Chunks which have been traversed by a destructive iterator should be
 * considered invalid; no operations are supported involving that subrange
 * of the chunkbuf, other than CdbChunkBuf_Reset() and CdbChunkBuf_Destroy().
 * After the chunkbuf is reset it can again be used normally.
 *
 * This can be useful when moving all of the tuples out of a chunkbuf into
 * other chunkbufs, so that the vacated chunks can be recycled immediately
 * for the target chunkbufs to use.
 */
void
CdbChunkBuf_Iterator_MakeDestructive(CdbChunkBuf_Iterator *it)
{
    /* Iterator must be a normal one over an ordinary resident chunkbuf. */
    Assert(it->nextchunk == CdbChunkBuf_Iterator_NextChunkHelper);

    it->nextchunk = CdbChunkBuf_DestructiveIterator_NextChunkHelper;
    it->setpos = CdbChunkBuf_DestructiveIterator_SetPosHelper;
}                               /* CdbChunkBuf_Iterator_MakeDestructive */


/*-------------------------------------------------------------------------
 *                    CdbChunkBuf_Iterator (Grotesque)
 *-------------------------------------------------------------------------
 */

/* Helpers for inline functions */
static bool
CdbChunkBuf_GrotesqueIterator_NextChunkHelper(struct CdbChunkBuf_Iterator *it)
{
    CdbChunkBuf_Length  newchunkoffset = it->curchunkendoffset;

    Insist(it->cbuf2 &&
           newchunkoffset <= it->cbuf2->len);

    /* Reinitialize iterator, make it non-grotesque and jump to 2nd chunkbuf. */
    CdbChunkBuf_Iterator_Init(it, it->cbuf2);
    it->cbuf2 = NULL;
    CdbChunkBuf_Iterator_SetPos(it, newchunkoffset);

    return newchunkoffset < it->chunkbuf->len;
}                               /* CdbChunkBuf_GrotesqueIterator_NextChunkHelper */

static void
CdbChunkBuf_GrotesqueIterator_SetPosHelper(struct CdbChunkBuf_Iterator *it,
                                           CdbChunkBuf_Length           newoffset)
{
    UnusedArg(it);
    UnusedArg(newoffset);
    Insist(0);                  /* unsupported */
}                               /* CdbChunkBuf_GrotesqueIterator_SetPosHelper */


/*
 * CdbChunkBuf_Iterator_MakeGrotesque
 *
 * Sets a CdbChunkBuf_Iterator so that, upon reaching the end of its current
 * chunk, it will reinitialize and reposition itself to its same offset in
 * 'cbuf2'.
 */
void
CdbChunkBuf_Iterator_MakeGrotesque(CdbChunkBuf_Iterator *it, CdbChunkBuf *cbuf2)
{
    Assert(cbuf2 &&
           cbuf2->chunklist.cellbuf.cellbytes == sizeof(it->curchunk));

    it->cbuf2 = cbuf2;
    it->nextchunk = CdbChunkBuf_GrotesqueIterator_NextChunkHelper;
    it->setpos = CdbChunkBuf_GrotesqueIterator_SetPosHelper;
}                               /* CdbChunkBuf_Iterator_MakeGrotesque */

/*
 * CdbChunkBuf_Iterator_IsGrotesque
 *
 * Returns true if the iterator is grotesque.
 */
bool
CdbChunkBuf_Iterator_IsGrotesque(const CdbChunkBuf_Iterator *it)
{
    return it->nextchunk == CdbChunkBuf_GrotesqueIterator_NextChunkHelper;
}                               /* CdbChunkBuf_Iterator_IsGrotesque */

