/*-------------------------------------------------------------------------
 *
 * cdbchunkbuf.h
 *
 * A byte string container which can hold as much data as will fit in
 * memory, using equal-sized chunks allocated from a CdbChunkPool; or
 * can transfer data to and from a workfile.
 *
 * Copyright (c) 2007-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBCHUNKBUF_H
#define CDBCHUNKBUF_H

#include "cdb/cdbchunkpool.h"           /* CdbChunkPool */
#include "cdb/cdbptrbuf.h"              /* CdbPtrBuf */
#include "cdb/cdbworkfile.h"            /* CdbWorkfile */


/* defined below... */
struct CdbChunkBuf;
struct CdbChunkBuf_Iterator;


/*
 * CdbChunkBuf_Length
 *
 * Use 64 bits for the total chunkbuf data length and for lengths of
 * substrings that could be longer than a chunk.  A chunkbuf that is
 * associated with a workfile can hold more data than a 32-bit address
 * space, so use 64 bits even on 32-bit platforms.
 */
typedef uint64  CdbChunkBuf_Length;

/*
 * Use 'Size' for lengths and offsets that are bounded by chunk size.
 *
 * A single chunk or any substring of a chunk is certain to fit in memory.
 * Such lengths can be represented by the 'Size' type (same as size_t),
 * which can be 32 or 64 bits depending on the platform.
 */


/*-------------------- Typedefs for function pointers ---------------------
 */

typedef Size                    /* returns #bytes consumed from src */
(*CdbChunkBuf_AddChunkFunc)(struct CdbChunkBuf *cbuf,
                            const char         *src,
                            Size                len);
typedef void
(*CdbChunkBuf_IterInitFunc)(struct CdbChunkBuf             *cbuf,
                            struct CdbChunkBuf_Iterator    *it);
typedef bool                            /* returns true if ok, false if end */
(*CdbChunkBuf_Iterator_NextChunkFunc)(struct CdbChunkBuf_Iterator *it);
typedef void
(*CdbChunkBuf_Iterator_SetPosFunc)(struct CdbChunkBuf_Iterator *it,
                                   CdbChunkBuf_Length           newoffset);


/*-------------------------------------------------------------------------
 *                               CdbChunkBuf
 *-------------------------------------------------------------------------
 * There are two kinds of chunkbuf:
 *
 *  - Ordinary (or resident):  Holds all of its data in memory.  Expands by
 *      allocating chunks as needed.  Created by CdbChunkBuf_Create().
 *
 *  - Filebuf:  Writes to a workfile using a one-chunk buffer.  Created by
 *      CdbChunkBuf_Create() followed by CdbChunkBuf_MakeFileBuf().
 */
typedef struct CdbChunkBuf
{
    CdbChunkBuf_Length  len;            /* total data length (bytes) */
    char               *freespace_beg;  /* next available byte in lastchunk */
    char               *freespace_end;  /* end of free space in lastchunk */
    char               *lastchunk;      /* chunk containing last byte of data...
                                         *  same as CdbPtrBuf_Last(&chunklist)
                                         */
    Size                chunksize;      /* bytes per chunk */
    CdbPtrBuf           chunklist;      /* collection of chunk ptrs */
    CdbChunkPool       *chunkpool;      /* chunk memory allocator */

    /* Helper functions */
    CdbChunkBuf_AddChunkFunc    addchunk;   /* make room for more data */
    CdbChunkBuf_IterInitFunc    iterinit;   /* iterator initialization */

    /* Filebuf fields... */
    CdbWorkfile_Subfile    *subfile;    /* for filebuf's workfile i/o */
    CdbChunkBuf_Length  bufendoffset;   /* current block's ending offset or 0 */
} CdbChunkBuf;


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
                 CdbChunkPool  *pool);

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
                     CdbChunkPool  *pool);

/*
 * CdbChunkBuf_Reset
 *
 * If 'rescannable' is true, and the chunkbuf has an association with a
 * CdbWorkfile_Subfile object, then that association is retained although
 * the chunkbuf's buffers are released.
 *
 * If the chunkbuf is a filebuf and 'keepsubfile' is false, then the
 * CdbWorkfile_Subfile object is freed, and the chunkbuf becomes
 * ordinary.  Note that the workfile itself is not affected.
 *
 * Iterators (CdbChunkBuf_Iterator) become invalid when their chunkbuf
 * is reset; afterward the caller should abandon or reinitialize them.
 */
void
CdbChunkBuf_Reset(CdbChunkBuf *cbuf, bool rescannable);


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
                   CdbChunkPool    *pool);

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
                       CdbChunkPool    *pool);

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
CdbChunkBuf_Destroy(CdbChunkBuf *cbuf);


/*-------------------------------------------------------------------------
 *                           CdbChunkBuf_FileBuf
 *-------------------------------------------------------------------------
 * Specialization of CdbChunkBuf for writing a byte stream to a workfile and
 * reading it back in.  Uses the CdbWorkfile_Subfile interface so more than
 * one stream can coexist in the same workfile if desired.
 *
 * The CdbChunkBuf_FileBuf_xxx functions use the CdbChunkBuf struct with
 * non-NULL 'subfile' field.  There is no CdbChunkBuf_FileBuf struct.
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
CdbChunkBuf_MakeFileBuf(CdbChunkBuf *cbuf, CdbWorkfile_Subfile *subfile);

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
CdbChunkBuf_FileBuf_Flush(CdbChunkBuf *cbuf);


/*-------------------------------------------------------------------------
 *                      CdbChunkBuf data manipulation
 *-------------------------------------------------------------------------
 */

/* Helpers for inline functions */
void
CdbChunkBuf_AllocHelper(CdbChunkBuf *cbuf, Size len);
void
CdbChunkBuf_AppendHelper(CdbChunkBuf *cbuf, const void *src, Size len);


/*
 * CdbChunkBuf_Alloc
 *
 * Allocates 'len' bytes of uninitialized space from chunkbuf 'cbuf'.
 * Returns a pointer to the area (never NULL).  0 < len < cbuf->chunksize
 *
 * Alignment is the same as the ending alignment of the data already in
 * the chunkbuf, or MAXALIGN(1) if the allocated space happens to be at
 * the beginning of a chunk.
 */
static inline void *
CdbChunkBuf_Alloc(CdbChunkBuf *cbuf, Size len)
{
    char   *area;

    Assert(cbuf &&
           len > 0 &&
           len < cbuf->chunksize &&
           cbuf->lastchunk <= cbuf->freespace_beg &&
           cbuf->freespace_beg <= cbuf->freespace_end);

    /* Allocate a new chunk if current chunk doesn't have enough space. */
    if (len > cbuf->freespace_end - cbuf->freespace_beg)
        CdbChunkBuf_AllocHelper(cbuf, len);

    /* Allocate 'len' bytes, appending to current chunk contents if any. */
    area = cbuf->freespace_beg;
    cbuf->freespace_beg += len;
    cbuf->len += len;
    return area;
}                               /* CdbChunkBuf_Alloc */


/*
 * CdbChunkBuf_Append
 */
static inline void
CdbChunkBuf_Append(CdbChunkBuf *cbuf, const void *src, Size len)
{
    Assert(src &&
           cbuf &&
           len > 0 &&
           cbuf->lastchunk <= cbuf->freespace_beg &&
           cbuf->freespace_beg <= cbuf->freespace_end);

    /* Fast path if chunk has room for all of the data. */
    if (len <= cbuf->freespace_end - cbuf->freespace_beg)
    {
        memcpy(cbuf->freespace_beg, src, len);
        cbuf->freespace_beg += len;
        cbuf->len += len;
    }

    /* First time: Allocate first chunk.  Subsequently: Fragment the data. */
    else
        CdbChunkBuf_AppendHelper(cbuf, src, len);
}                               /* CdbChunkBuf_Append */


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
                      CdbChunkBuf_Length    len);

/*
 * CdbChunkBuf_AppendSubfile
 *
 * Reads from workfile and appends to chunkbuf.
 *
 * Using a CdbWorkfile_Subfile_Iterator provided by the caller, starts
 * reading from the iterator's current position, and continues for length
 * 'len' or until eof.
 *
 * Returns 0 if 'len' bytes were read successfully.  If end of subfile is
 * encountered, returns the original value of 'len' minus the number of
 * bytes read.
 */
CdbChunkBuf_Length
CdbChunkBuf_AppendSubfile(CdbChunkBuf                  *cbuf,
                          CdbWorkfile_Subfile_Iterator *it,
                          CdbChunkBuf_Length            len);

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
                 Size               nbytes);

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
                 Size               nbytes);


/*-------------------------------------------------------------------------
 *                          CdbChunkBuf_Iterator
 *-------------------------------------------------------------------------
 */
typedef struct CdbChunkBuf_Iterator
{
    char               *curpos;         /* -> current position within or at
                                         *  end of curchunk; initially NULL.
                                         *  Callers may adjust curpos within
                                         *  the bounds of the current chunk.
                                         */

    /* Callers may examine these fields. */
    char               *curchunk;       /* -> start of current chunk (or NULL) */
    CdbChunkBuf_Length  curchunkoffset; /* #bytes in chunks preceding curchunk */
    CdbChunkBuf_Length  curchunkendoffset;  /* up to and including curchunk */

    CdbChunkBuf        *chunkbuf;       /* -> chunkbuf */

    /* Helpers for inline functions */
    CdbChunkBuf_Iterator_NextChunkFunc  nextchunk;  /* advance to next chunk */
    CdbChunkBuf_Iterator_SetPosFunc     setpos;     /* jump to given offset */

    /* Private fields */
    CdbPtrBuf_Iterator              chunkit;    /* for getting next chunk ptr */
    CdbWorkfile_Subfile_Iterator    subfileit;  /* for reading from filebuf */
    CdbChunkBuf                    *cbuf2;      /* 2nd chunkbuf if grotesque */
} CdbChunkBuf_Iterator;


/*
 * CdbChunkBuf_Iterator_Init
 */
void
CdbChunkBuf_Iterator_Init(CdbChunkBuf_Iterator *it, CdbChunkBuf *cbuf);

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
CdbChunkBuf_Iterator_MakeDestructive(CdbChunkBuf_Iterator *it);

/*
 * CdbChunkBuf_Iterator_MakeGrotesque
 *
 * Sets the iterator so that, upon reaching the end of its current chunk, it
 * will reinitialize and reposition itself to its same offset in 'cbuf2'.
 */
void
CdbChunkBuf_Iterator_MakeGrotesque(CdbChunkBuf_Iterator *it, CdbChunkBuf *cbuf2);

/*
 * CdbChunkBuf_Iterator_IsGrotesque
 *
 * Returns true if the iterator is grotesque.
 */
bool
CdbChunkBuf_Iterator_IsGrotesque(const CdbChunkBuf_Iterator *it);


/*
 * CdbChunkBuf_Iterator_NextChunk
 *
 * Advance the iterator to the next chunk.  Returns true if successful,
 * false if there is not another chunk.
 *
 * If CdbChunkBuf_Iterator_MakeGrotesque(it, cbuf2) has been called, the
 * iterator jumps into chunkbuf 'cbuf2'.  Therefore, callers shouldn't keep
 * local copies of the iterator's chunkbuf ptr, chunksize, etc, unless such
 * are refreshed after calling any function which might invoke NextChunk().
 */
static inline bool
CdbChunkBuf_Iterator_NextChunk(CdbChunkBuf_Iterator *it)
{
    Assert(it->chunkbuf &&
           it->chunkbuf->chunklist.cellbuf.cellbytes == sizeof(it->curchunk));
    return it->nextchunk(it);
}                               /* CdbChunkBuf_Iterator_NextChunk */


/*
 * CdbChunkBuf_Iterator_AdvanceTo
 *
 * Positions the iterator at offset 'newoffset' from the beginning of the
 * byte string.
 *
 * 'newoffset' must be not less than it->curchunkoffset, the beginning of
 * the iterator's current chunk.
 *
 * If 'newoffset' is beyond the last byte of data in the chunkbuf, the
 * iterator is positioned at the end of the data, and 0 is returned.
 * The iterator remains valid and can continue advancing after more
 * data is appended.
 *
 * On return, it->curpos points to a substring of length 0 or more, which the
 * caller may access starting at the new position.  The substring ends at the
 * end of all the data, or at the end of the current chunk, whichever is first.
 * The length of this substring is returned: it is > 0 if newoffset is less
 * than it->chunkbuf->len.
 *
 * This function is optimized for advancing sequentially through a byte
 * string.  For random repositioning or long jumps across many chunks,
 * use CdbChunkBuf_Iterator_SetPos().
 */
static inline Size
CdbChunkBuf_Iterator_AdvanceTo(CdbChunkBuf_Iterator    *it,
                               CdbChunkBuf_Length       newoffset)
{
    Size    fraglen;

    /* Advance until we're on the right chunk. At end, remain on final chunk. */
    while (newoffset >= it->curchunkendoffset)
    {
        if (!CdbChunkBuf_Iterator_NextChunk(it))
        {
            /* If past the end, position to exactly the end. */
            if (newoffset > it->chunkbuf->len)
                newoffset = it->chunkbuf->len;
            break;
        }
    }

    Assert(newoffset >= it->curchunkoffset);

    /* Chunks before the last one are always full. */
    if (it->curchunkendoffset < it->chunkbuf->len)
    {
        fraglen = (Size)(it->curchunkendoffset - newoffset);

        Assert(fraglen > 0 &&
               fraglen <= it->chunkbuf->chunksize);
    }

    /* The last chunk can have free space at the end. */
    else
    {
        fraglen = (Size)(it->chunkbuf->len - newoffset);

        Assert(fraglen <= (Size)(it->chunkbuf->len - it->curchunkoffset));
        Assert((Size)(it->chunkbuf->freespace_beg - it->curchunk) ==
                    (Size)(it->chunkbuf->len - it->curchunkoffset));
    }

    it->curpos = newoffset - it->curchunkoffset + it->curchunk;
    return fraglen;
}                               /* CdbChunkBuf_Iterator_AdvanceTo */


/*
 * CdbChunkBuf_Iterator_GetPos
 *
 * Returns the iterator's current position as an offset in bytes from the
 * beginning of the chunkbuf's contents.   0 <= offset <= it->chunkbuf->len
 */
static inline CdbChunkBuf_Length
CdbChunkBuf_Iterator_GetPos(CdbChunkBuf_Iterator *it)
{
    Assert(it->curpos >= it->curchunk &&
           it->curpos - it->curchunk <= it->chunkbuf->chunksize);

    return it->curpos - it->curchunk + it->curchunkoffset;
}                               /* CdbChunkBuf_Iterator_GetPos */


/*
 * CdbChunkBuf_Iterator_SetPos
 *
 * Positions the iterator at offset 'newoffset' from the beginning of the
 * chunkbuf.    0 <= newoffset <= it->chunkbuf->len
 *
 * On return, it->curpos points to a substring of length 0 or more, which the
 * caller may access starting at the new position.  The substring ends at the
 * end of all the data, or at the end of the current chunk, whichever is first.
 *
 * Returns the length of this substring: it is > 0 unless the new position is
 * at the end.
 */
static inline Size
CdbChunkBuf_Iterator_SetPos(CdbChunkBuf_Iterator   *it,
                            CdbChunkBuf_Length      newoffset)
{
    CdbChunkBuf    *cbuf = it->chunkbuf;
    Size            fraglen;

    Assert(cbuf &&
           cbuf->chunklist.cellbuf.cellbytes == sizeof(it->curchunk));
    Insist(newoffset <= cbuf->len);

    /* Is newoffset within current chunk?  (includes all-pointers-null case) */
    if (newoffset >= it->curchunkoffset &&
        newoffset <= it->curchunkendoffset)
        it->curpos = newoffset - it->curchunkoffset + it->curchunk;

    /* Jump to another chunk. */
    else
        it->setpos(it, newoffset);

    /* Return number of bytes from here to end of data in chunk. */
    if (it->curchunkendoffset < cbuf->len)
        fraglen = (Size)(it->curchunkendoffset - newoffset);
    else
        fraglen = (Size)(cbuf->len - newoffset);

    return fraglen;
}                               /* CdbChunkBuf_Iterator_SetPos */

#endif   /* CDBCHUNKBUF_H */
