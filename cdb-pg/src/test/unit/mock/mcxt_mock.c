/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/palloc.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "miscadmin.h"		/* MyProcPid */
#include "utils/memutils.h"
#include "utils/debugbreak.h"
#include "cdb/cdbvars.h"	/* gp_process_memory_cutoff_bytes */
#include <stdint.h>		/* SIZE_MAX (C99) */

MemoryContext   CurrentMemoryContext;
void           *
MemoryContextAllocImpl(MemoryContext context, Size size, const char *file, const char *func, int line)
{

    void           *p = malloc(size);
    return p;

}

void           *
MemoryContextAllocZeroImpl(MemoryContext context, Size size, const char *file, const char *func, int line)
{

    void           *p = malloc(size);
    memset(p, 0, size);
    return p;

}

void           *
MemoryContextAllocZeroAlignedImpl(MemoryContext context, Size size, const char *file, const char *func, int line)
{

    void           *p = malloc(size);
    return p;

}

void           *
MemoryContextReallocImpl(void *pointer, Size size, const char *file, const char *func, int line)
{

    return realloc(pointer, size);

}

void 
MemoryContextFreeImpl(void *pointer, const char *file, const char *func, int sline)
{

    free(pointer);

}

char           *
MemoryContextStrdup(MemoryContext context, const char *string)
{

    return strdup(string);

}

char           *
pnstrdup(const char *in, Size len)
{

    check_expected(in);
    check_expected(len);

    return (char *) mock();

}

void 
GPMemoryProtectInit(void)
{



    mock();

}

void 
GPMemoryProtectReset(void)
{



    mock();

}

int64 
getMOPHighWaterMark(void)
{



    return (int64) mock();

}

int 
MemProtSemas(void)
{



    return (int) mock();

}

void           *
gp_malloc(int64 sz)
{

    check_expected(sz);

    return (void *) mock();

}

void           *
gp_calloc(int64 nmemb, int64 sz)
{

    check_expected(nmemb);
    check_expected(sz);

    return (void *) mock();

}

void           *
gp_realloc(void *ptr, int64 sz, int64 newsz)
{

    check_expected(ptr);
    check_expected(sz);
    check_expected(newsz);
    optional_assignment(ptr);
    return (void *) mock();

}

void 
gp_free2(void *ptr, int64 sz)
{

    check_expected(ptr);
    check_expected(sz);
    optional_assignment(ptr);
    mock();

}

uint64 
gp_vmem_used(void)
{



    return (uint64) mock();

}

uint64 
gp_vmem_max(void)
{



    return (uint64) mock();

}
