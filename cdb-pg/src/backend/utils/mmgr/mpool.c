/*-------------------------------------------------------------------------
 *
 * mpool.c
 *	  Fast memory pool. free is a NOP. 
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memutils.h"

/* Define this to detail debug alloc information */
/* #define HAVE_ALLOCINFO */

#ifdef CDB_MCXT_DEBUG
#define CDB_MCXT_WHERE(context) (context)->callerFile, (context)->callerLine
#else
#define CDB_MCXT_WHERE(context) __FILE__, __LINE__
#endif

#define PRINT(x) elog x
#undef PRINT
#define PRINT(x)  


typedef struct mpool_block_t mpool_block_t;

#define BLOCKSZ_STEP1	(32*1024)
#define BLOCKSZ_STEP2   (128*1024)  /* 128K */
#define BLOCKSZ_STEP3   (1024*1024) /* 1MB */


typedef struct mpool_context_t mpool_context_t;
struct mpool_context_t
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* Info about storage allocated in this context: */
	mpool_block_t* free_tail;	/* points to tail of free list */
	mpool_block_t* inuse_tail;	/* points to tail of inuse list */
    int blocks_allocated;
	uint64 max_bytes_allocated;
	uint64 bytes_allocated;
};

/* free_tail and inuse_tail are circular linked list where 
   free_tail->next and inuse_tail->next is the head of list. */

struct mpool_block_t
{
	mpool_context_t* owner;
	char* begin;				/* datablock is char[begin .. end) */
	char* end;		
	char* cur;					/* points inside datablock, marks next free byte */
	mpool_block_t* next;		/* next datablock in the list */
};

/*
 * mpool_chunkhdr_t
 *		The prefix of each piece of memory in an mpool_block
 *
 * NB: this MUST match StandardChunkHeader as defined by utils/memutils.h.
 */
typedef struct mpool_chunkhdr_t mpool_chunkhdr_t;
struct mpool_chunkhdr_t
{
	void* mpool;				/* points to the pool */
	Size  size;					/* size of usable space in chunk */
#ifdef MEMORY_CONTEXT_CHECKING
	/* when debugging memory usage, also store actual requested size */
	Size		requested_size;
	const char *alloc_tag;
	int 		alloc_n;
	void *alloc_prev;
	void *alloc_next;
#endif
};

/* NB: this must match STANDARDCHUNKHEADERSIZE in memutils.h */
#define MPOOL_CHUNKHDR_SIZE MAXALIGN(sizeof(mpool_chunkhdr_t))


#define mpool_get_chunk(ptr) \
	((mpool_chunkhdr_t*)(((char *)(ptr)) - MPOOL_CHUNKHDR_SIZE))
#define mpool_get_pointer(chk) \
	((void*)(((char *)(chk)) + MPOOL_CHUNKHDR_SIZE))


/*
 *  Allocate a block via malloc
 */
static mpool_block_t* mpool_block_alloc(mpool_context_t* mpool, int minsz)
{
	const int hdrsz = MAXALIGN(sizeof(mpool_block_t));
	mpool_block_t* bp;
	int tot = (mpool->blocks_allocated < 4 ? BLOCKSZ_STEP1
			   : (mpool->blocks_allocated < 8 ? BLOCKSZ_STEP2 : BLOCKSZ_STEP3));

	minsz = MAXALIGN(minsz);

	if (hdrsz + MPOOL_CHUNKHDR_SIZE + minsz > tot)
		tot = minsz + MPOOL_CHUNKHDR_SIZE + hdrsz;

	bp = malloc(tot);
	if (bp) 
	{
		bp->owner = mpool;
		bp->begin = bp->cur = ((char*) bp) + hdrsz;
		bp->end = ((char*) bp) + tot;
		bp->next = 0;
		MemoryContextNoteAlloc(&mpool->header, tot);
	}
	mpool->blocks_allocated++;
	return bp;
}


/*
 *  Allocate a chunk in a block 
 */
static inline mpool_chunkhdr_t* mpool_block_alloc_chunk(mpool_block_t* bp, int sz)
{
	mpool_chunkhdr_t* p = 0;
	const int tot = MPOOL_CHUNKHDR_SIZE + MAXALIGN(sz);
	Assert(bp->begin <= bp->cur && bp->cur <= bp->end);
	if (bp->cur + tot <= bp->end)
	{
		/* we have enough freespace in the block. alloc it. */
		p = (mpool_chunkhdr_t*) bp->cur;
		bp->cur += tot;
		p->mpool = bp->owner;
		p->size = MAXALIGN(sz);
#ifdef MEMORY_CONTEXT_CHECKING
		p->requested_size = sz;
#endif
	}
	return p;
}




/*
 * These functions implement the MemoryContext API for AllocSet contexts.
 */
static void *mpool_alloc(MemoryContext context, Size size);
static void *mpool_realloc(MemoryContext context, void *pointer, Size size);
static void mpool_init(MemoryContext context);
static void mpool_reset(MemoryContext context);
static void mpool_delete(MemoryContext context);
static Size mpool_get_chunk_space(MemoryContext context, void *pointer);
static bool mpool_is_empty(MemoryContext context);
static void mpool_stats(MemoryContext context, const char* contextName);

#ifdef MEMORY_CONTEXT_CHECKING
static void mpool_check(MemoryContext context);
#endif

/*
 * This is the virtual function table for AllocSet contexts.
 */
static MemoryContextMethods mpool_methods = {
	mpool_alloc,
	0, /*mpool_free,*/
	mpool_realloc,
	mpool_init,
	mpool_reset,
	mpool_delete,
	mpool_get_chunk_space,
	mpool_is_empty,
	mpool_stats
#ifdef MEMORY_CONTEXT_CHECKING
	,mpool_check
#endif
};



/*
 * Public routines
 */


/*
 * mpool_create
 *		Create a new mpool context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (for debugging --- string will be copied)
 * minContextSize: minimum context size
 * initBlockSize: initial allocation block size
 * maxBlockSize: maximum allocation block size
 */
MemoryContext
mpool_create(MemoryContext parent,
			 const char *name,
			 Size minContextSize,  /* useless */
			 Size initBlockSize, /* useless */
			 Size maxBlockSize /* useless */ )
{
	mpool_context_t*	context;

	/* Do the type-independent part of context creation */
	context = (mpool_context_t*) MemoryContextCreate(T_MPoolContext,
													 sizeof(mpool_context_t),
													 &mpool_methods,
													 parent,
													 name);
	

	return (MemoryContext) context;
}

/*
 * mpool_init
 *		Context-type-specific initialization routine.
 *
 * This is called by MemoryContextCreate() after setting up the
 * generic MemoryContext fields and before linking the new context
 * into the context tree.  We must do whatever is needed to make the
 * new context minimally valid for deletion.  We must *not* risk
 * failure --- thus, for example, allocating more memory is not cool.
 * (AllocSetContextCreate can allocate memory when it gets control
 * back, however.)
 *
 * note: mpool_create() calls MemoryContextCreate, which calls mpool_init().
 */
static void
mpool_init(MemoryContext context)
{
	/*
	 * Since MemoryContextCreate already zeroed the context node, we don't
	 * have to do anything here: it's already OK.
	 */
}

/*
 * mpool_reset
 *		Frees all memory which is allocated in the given set.
 */
static void
mpool_reset(MemoryContext context)
{
	mpool_context_t* mpool = (mpool_context_t*) context;
	mpool_block_t* bp;

	AssertArg(NULL != mpool);

	/* if something is inuse, then slip it into free list */
	if (mpool->inuse_tail) 
	{
		if (mpool->free_tail)
		{
			/* slip at head of free list */
			bp = mpool->free_tail->next; /* head of free list */
			mpool->free_tail->next = mpool->inuse_tail->next; /* free head now head of inuse */
			mpool->inuse_tail->next = bp;	 /* tail of inuse now new head of free list*/
		}
		else
		{
			mpool->free_tail = mpool->inuse_tail;
		}
		PRINT((LOG, "mpool_reset %x: slip inuse into free list", context));

		/* all of inuse is gone */
		mpool->inuse_tail = 0;
		mpool->bytes_allocated = 0;
	}
}

/*
 * mpool_delete
 *		Frees all memory which is allocated in the given set,
 *		in preparation for deletion of the set.
 *
 * Unlike mpool_reset, this *must* free all resources of the set.
 * But note we are not responsible for deleting the context node itself.
 */
static void
mpool_delete(MemoryContext context)
{
	mpool_context_t* mpool = (mpool_context_t*) context;
	mpool_block_t* head;

	PRINT((LOG, "mpool_delete %x", context));

	mpool_reset(context);		/* reuse some code ... first put everything in free list */

	if (mpool->free_tail) 
	{
		head = mpool->free_tail->next;
		mpool->free_tail->next = 0; /* NUL terminate the list */
		mpool->free_tail = 0;		/* no more free list */
		/* free head until list is empty */
		while (head) 
		{
			mpool_block_t* p = head->next;
			MemoryContextNoteFree(&mpool->header, head->end - (char*) head);
			free(head);
			PRINT((LOG, "mpool_delete %x: free block %x", context, head));
			head = p;
			mpool->blocks_allocated--;
		}
	}

	Assert(mpool->blocks_allocated == 0);
}

/*
 * mpool_alloc
 *		Returns pointer to allocated memory of given size; memory is added
 *		to the set.
 */
static void *
mpool_alloc(MemoryContext context, Size size)
{
	mpool_context_t* mpool = (mpool_context_t*) context;
	mpool_block_t* bp = 0;		/* which block to allocate chunk */
	mpool_chunkhdr_t* chunk = 0; /* which chunk to allocate size bytes */

	AssertArg(NULL != mpool);
	Assert(MPOOL_CHUNKHDR_SIZE == STANDARDCHUNKHEADERSIZE);

	/* first try to get from inuse tail */
	if (0 != (bp = mpool->inuse_tail)) 
	{
		/* fastcase this. return immediately if we get a chunk */
		if (0 != (chunk = mpool_block_alloc_chunk(bp, size)))
			goto do_alloc;
	}


	/* need to get a block from free list ? */
	if (!chunk && 0 != (bp = mpool->free_tail))
	{
		bp = bp->next;			/* pop the head of free list */
		if (bp == mpool->free_tail)
			mpool->free_tail = 0;
		else
			mpool->free_tail->next = bp->next;

		/* init the new block */
		bp->next = 0;
		bp->cur = bp->begin;

		/* try alloc from new block */
		if ( ! (chunk = mpool_block_alloc_chunk(bp, size)))
		{
			/* an empty block cannot fit size ... just free it */
			MemoryContextNoteFree(&mpool->header, bp->end - (char*) bp);
			free(bp);
			bp = 0;
		}
		else 
		{
			PRINT((LOG, "mpool_alloc %x: from free list %d bytes\n", context, size));
		}
	}

	/* need to allocate a new block ? */
	if (!chunk)
	{
		if (! (bp = mpool_block_alloc(mpool, size)))
		{
			MemoryContextStats(TopMemoryContext);
		    ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %lu for context '%s'.",
								(unsigned long) size, 
								mpool->header.name)));
			
		}
		chunk = mpool_block_alloc_chunk(bp, size);
		{
			PRINT((LOG, "mpool_alloc %x: from malloc %d bytes at %x\n", context, size, bp));
		}
	}

	/* we must have a chunk now */
	Assert(chunk && chunk->size >= size && bp);

	/* add bp to inuse list */
	if (mpool->inuse_tail)
	{
		bp->next = mpool->inuse_tail->next;
		mpool->inuse_tail->next = bp;
	}
	else 
		bp->next = bp;
	mpool->inuse_tail = bp;

	do_alloc:
	mpool->bytes_allocated += size;
	if (mpool->bytes_allocated > mpool->max_bytes_allocated)
		mpool->max_bytes_allocated = mpool->bytes_allocated;
	return mpool_get_pointer(chunk);
}


/*
 * mpool_realloc
 *		Returns new pointer to allocated memory of given size; this memory
 *		is added to the set.  Memory associated with given pointer is copied
 *		into the new memory, and the old memory is freed.
 */
static void *
mpool_realloc(MemoryContext context, void *pointer, Size size)
{
	mpool_chunkhdr_t* chunk = mpool_get_chunk(pointer);
	Size oldsz = chunk->size;
	char* p;

	/* this pool is alloc only */
	p = mpool_alloc(context, size);
	if (oldsz > size) 
		oldsz = size;
	memcpy(p, pointer, oldsz);
	return p;
}

/*
 * mpool_get_chunk_space
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
static Size
mpool_get_chunk_space(MemoryContext context, void *pointer)
{
	mpool_chunkhdr_t* chunk = mpool_get_chunk(pointer);
	return chunk->size + MPOOL_CHUNKHDR_SIZE;
}

/*
 * mpool_is_empty
 *		Is an allocset empty of any allocated space?
 */
static bool
mpool_is_empty(MemoryContext context)
{
	mpool_context_t* mpool = (mpool_context_t*) context;
	return 0 == mpool->inuse_tail;
}

/*
 * AllocSetStats
 *		Displays stats about memory consumption of an allocset.
 */
static void
mpool_stats(MemoryContext context, const char* contextName)
{
}


#ifdef MEMORY_CONTEXT_CHECKING

/*
 * AllocSetCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
static void
mpool_check(MemoryContext context)
{
}

#endif   /* MEMORY_CONTEXT_CHECKING */
