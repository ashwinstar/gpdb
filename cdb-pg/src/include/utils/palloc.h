/*-------------------------------------------------------------------------
 *
 * palloc.h
 *	  POSTGRES memory allocator definitions.
 *
 * This file contains the basic memory allocation interface that is
 * needed by almost every backend module.  It is included directly by
 * postgres.h, so the definitions here are automatically available
 * everywhere.	Keep it lean!
 *
 * Memory allocation occurs within "contexts".	Every chunk obtained from
 * palloc()/MemoryContextAlloc() is allocated within a specific context.
 * The entire contents of a context can be freed easily and quickly by
 * resetting or deleting the context --- this is both faster and less
 * prone to memory-leakage bugs than releasing chunks individually.
 * We organize contexts into context trees to allow fine-grain control
 * over chunk lifetime while preserving the certainty that we will free
 * everything that should be freed.  See utils/mmgr/README for more info.
 *
 *
 * Portions Copyright (c) 2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/palloc.h,v 1.35 2006/03/05 15:59:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PALLOC_H
#define PALLOC_H

/*
 * Optional #defines for debugging...
 *
 * If CDB_PALLOC_CALLER_ID is defined, MemoryContext error and warning
 * messages (such as "out of memory" and "invalid memory alloc request
 * size") will include the caller's source file name and line number.
 * This can be useful in optimized builds where the error handler's
 * stack trace doesn't accurately identify the call site.  Overhead
 * is minimal: two extra parameters to memory allocation functions,
 * and 8 to 16 bytes per context.
 *
 * If CDB_PALLOC_TAGS is defined, every allocation from a standard
 * memory context (aset.c) is tagged with an extra 16 to 32 bytes of
 * debugging info preceding the first byte of the area.  The added
 * header fields identify the allocation call site (source file name
 * and line number).  Also each context keeps a linked list of all
 * of its allocated areas.  The dump_memory_allocation() and
 * dump_memory_allocation_ctxt() functions in aset.c may be called 
 * from a debugger to write the area headers to a file.
 */

/*
#define CDB_PALLOC_CALLER_ID
*/

#define CDB_PALLOC_TAGS

/* CDB_PALLOC_TAGS implies CDB_PALLOC_CALLER_ID */
#if defined(CDB_PALLOC_TAGS) && !defined(CDB_PALLOC_CALLER_ID)
#define CDB_PALLOC_CALLER_ID
#endif

/*
 * Type MemoryContextData is declared in nodes/memnodes.h.	Most users
 * of memory allocation should just treat it as an abstract type, so we
 * do not provide the struct contents here.
 */
typedef struct MemoryContextData *MemoryContext;

/*
 * CurrentMemoryContext is the default allocation context for palloc().
 * We declare it here so that palloc() can be a macro.	Avoid accessing it
 * directly!  Instead, use MemoryContextSwitchTo() to change the setting.
 */
extern DLLIMPORT MemoryContext CurrentMemoryContext;

/*
 * Fundamental memory-allocation operations (more are in utils/memutils.h)
 */
extern void *MemoryContextAlloc(MemoryContext context, Size size);
extern void *MemoryContextAllocZero(MemoryContext context, Size size);
extern void *MemoryContextAllocZeroAligned(MemoryContext context, Size size);
extern void *repalloc(void *pointer, Size size);
extern void pfree(void *pointer);

#ifdef CDB_PALLOC_CALLER_ID
#define MemoryContextAlloc(context, size)               (MemoryContextAllocDbg(context, size, __FILE__, __LINE__))
#define MemoryContextAllocZero(context, size)           (MemoryContextAllocZeroDbg(context, size, __FILE__, __LINE__))
#define MemoryContextAllocZeroAligned(context, size)    (MemoryContextAllocZeroAlignedDbg(context, size, __FILE__, __LINE__))
#define repalloc(pointer, size)                         (repallocDbg(pointer, size, __FILE__, __LINE__))
#define pfree(pointer)                                  (pfreeDbg(pointer, __FILE__, __LINE__))
extern void *MemoryContextAllocDbg(MemoryContext context, Size size, const char* sfile, int sline);
extern void *MemoryContextAllocZeroDbg(MemoryContext context, Size size, const char* sfile, int sline);
extern void *MemoryContextAllocZeroAlignedDbg(MemoryContext context, Size size, const char* sfile, int sline);
extern void *repallocDbg(void *pointer, Size size, const char* sfile, int sline);
extern void pfreeDbg(void *pointer, const char* sfile, int sline);
#endif

#define palloc(sz)	MemoryContextAlloc(CurrentMemoryContext, (sz))

#define palloc0(sz) MemoryContextAllocZero(CurrentMemoryContext, (sz))

#define ctxt_alloc(ctxt, sz) MemoryContextAlloc((ctxt), (sz))
/*
 * The result of palloc() is always word-aligned, so we can skip testing
 * alignment of the pointer when deciding which MemSet variant to use.
 * Note that this variant does not offer any advantage, and should not be
 * used, unless its "sz" argument is a compile-time constant; therefore, the
 * issue that it evaluates the argument multiple times isn't a problem in
 * practice.
 */
#define palloc0fast(sz) \
	( MemSetTest(0, sz) ? \
		MemoryContextAllocZeroAligned(CurrentMemoryContext, sz) : \
		MemoryContextAllocZero(CurrentMemoryContext, sz) )

/*
 * MemoryContextSwitchTo
 *		Returns the current context; installs the given context.
 */
static inline MemoryContext
MemoryContextSwitchTo(MemoryContext context)
{
	MemoryContext old = CurrentMemoryContext;

	CurrentMemoryContext = context;
	return old;
}

/*
 * These are like standard strdup() except the copied string is
 * allocated in a context, not with malloc().
 */
extern char *MemoryContextStrdup(MemoryContext context, const char *string);

#define pstrdup(str)  MemoryContextStrdup(CurrentMemoryContext, (str))

#if defined(WIN32) || defined(__CYGWIN__)
extern void *pgport_palloc(Size sz);
extern char *pgport_pstrdup(const char *str);
extern void pgport_pfree(void *pointer);
#endif

#endif   /* PALLOC_H */
