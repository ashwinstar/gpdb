/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/memutils.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "utils/memutils.h"
#include "miscadmin.h"

MemoryContext   TopMemoryContext;
MemoryContext   ErrorContext;
MemoryContext   PostmasterContext;
MemoryContext   CacheMemoryContext;
MemoryContext   MessageContext;
MemoryContext   TopTransactionContext;
MemoryContext   CurTransactionContext;
MemoryContext   QueryContext;
MemoryContext   PortalContext;
void 
MemoryContextInit(void)
{



    mock();

}

void 
MemoryContextReset(MemoryContext context)
{

    check_expected(context);

    mock();

}

void 
MemoryContextResetChildren(MemoryContext context)
{

    check_expected(context);

    mock();

}

void 
MemoryContextDeleteChildren(MemoryContext context)
{

    check_expected(context);

    mock();

}

void 
MemoryContextResetAndDeleteChildren(MemoryContext context)
{

    check_expected(context);

    mock();

}

Size 
GetMemoryChunkSpace(void *pointer)
{

    check_expected(pointer);
    optional_assignment(pointer);
    return (Size) mock();

}

MemoryContext 
GetMemoryChunkContext(void *pointer)
{

    check_expected(pointer);
    optional_assignment(pointer);
    return (MemoryContext) mock();

}

bool 
MemoryContextIsEmpty(MemoryContext context)
{

    check_expected(context);

    return (bool) mock();

}

Size 
MemoryContextGetCurrentSpace(MemoryContext context)
{

    check_expected(context);

    return (Size) mock();

}

Size 
MemoryContextGetPeakSpace(MemoryContext context)
{

    check_expected(context);

    return (Size) mock();

}

Size 
MemoryContextSetPeakSpace(MemoryContext context, Size nbytes)
{

    check_expected(context);
    check_expected(nbytes);

    return (Size) mock();

}

char           *
MemoryContextName(MemoryContext context, MemoryContext relativeTo, char *buf, int bufsize)
{

    check_expected(context);
    check_expected(relativeTo);
    check_expected(buf);
    check_expected(bufsize);
    optional_assignment(buf);
    return (char *) mock();

}

void 
MemoryContextStats(MemoryContext context)
{

    check_expected(context);

    mock();

}

void 
MemoryContextDeleteImpl(MemoryContext context, const char *sfile, const char *func, int sline)
{

    check_expected(context);
    check_expected(sfile);
    check_expected(func);
    check_expected(sline);

    mock();

}

void 
dump_memory_allocation(const char *fname)
{

    check_expected(fname);

    mock();

}

void 
dump_memory_allocation_ctxt(FILE * ofile, void *ctxt)
{

    check_expected(ofile);
    check_expected(ctxt);
    optional_assignment(ofile);
    optional_assignment(ctxt);
    mock();

}

void 
MemoryContextCheck(MemoryContext context)
{

    check_expected(context);

    mock();

}

bool 
MemoryContextContains(MemoryContext context, void *pointer)
{

    check_expected(context);
    check_expected(pointer);
    optional_assignment(pointer);
    return (bool) mock();

}

void 
MemoryContextNoteAlloc(MemoryContext context, Size nbytes)
{

    check_expected(context);
    check_expected(nbytes);

    mock();

}

void 
MemoryContextNoteFree(MemoryContext context, Size nbytes)
{

    check_expected(context);
    check_expected(nbytes);

    mock();

}

MemoryContext 
MemoryContextCreate(NodeTag tag, Size size, MemoryContextMethods * methods, MemoryContext parent, const char *name)
{

    check_expected(tag);
    check_expected(size);
    check_expected(methods);
    check_expected(parent);
    check_expected(name);
    optional_assignment(methods);
    return (MemoryContext) mock();

}

MemoryContext 
AllocSetContextCreate(MemoryContext parent, const char *name, Size minContextSize, Size initBlockSize, Size maxBlockSize)
{

    check_expected(parent);
    check_expected(name);
    check_expected(minContextSize);
    check_expected(initBlockSize);
    check_expected(maxBlockSize);

    return (MemoryContext) mock();

}

MPool          *
mpool_create(MemoryContext parent, const char *name)
{

    check_expected(parent);
    check_expected(name);

    return (MPool *) mock();

}

void           *
mpool_alloc(MPool * mpool, Size size)
{

    check_expected(mpool);
    check_expected(size);
    optional_assignment(mpool);
    return (void *) mock();

}

void 
mpool_reset(MPool * mpool)
{

    check_expected(mpool);
    optional_assignment(mpool);
    mock();

}

void 
mpool_delete(MPool * mpool)
{

    check_expected(mpool);
    optional_assignment(mpool);
    mock();

}

uint64 
mpool_total_bytes_allocated(MPool * mpool)
{

    check_expected(mpool);
    optional_assignment(mpool);
    return (uint64) mock();

}

uint64 
mpool_bytes_used(MPool * mpool)
{

    check_expected(mpool);
    optional_assignment(mpool);
    return (uint64) mock();

}

MemoryContext 
AsetDirectContextCreate(MemoryContext parent, const char *name)
{

    check_expected(parent);
    check_expected(name);

    return (MemoryContext) mock();

}

int 
floor_log2_Size(Size sz)
{

    check_expected(sz);

    return (int) mock();

}

int 
ceil_log2_Size(Size sz)
{

    check_expected(sz);

    return (int) mock();

}
