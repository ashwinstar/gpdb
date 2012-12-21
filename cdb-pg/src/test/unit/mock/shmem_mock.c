/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/shmem.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include <unistd.h>

SHMEM_OFFSET    ShmemBase;
void 
InitShmemAccess(void *seghdr)
{

    check_expected(seghdr);
    optional_assignment(seghdr);
    mock();

}

void 
InitShmemAllocation(void)
{



    mock();

}

void           *
ShmemAlloc(Size size)
{

    check_expected(size);

    return (void *) mock();

}

bool 
ShmemIsValid(unsigned long addr)
{

    check_expected(addr);

    return (bool) mock();

}

void 
InitShmemIndex(void)
{



    mock();

}

HTAB           *
ShmemInitHash(const char *name, long init_size, long max_size, HASHCTL * infoP, int hash_flags)
{

    check_expected(name);
    check_expected(init_size);
    check_expected(max_size);
    check_expected(infoP);
    check_expected(hash_flags);
    optional_assignment(infoP);
    return (HTAB *) mock();

}

void           *
ShmemInitStruct(const char *name, Size size, bool * foundPtr)
{

    check_expected(name);
    check_expected(size);
    check_expected(foundPtr);
    optional_assignment(foundPtr);
    return (void *) mock();

}

Size 
add_size(Size s1, Size s2)
{

    check_expected(s1);
    check_expected(s2);

    return (Size) mock();

}

Size 
mul_size(Size s1, Size s2)
{

    check_expected(s1);
    check_expected(s2);

    return (Size) mock();

}

void 
RequestAddinShmemSpace(Size size)
{

    check_expected(size);

    mock();

}

void 
SHMQueueInit(SHM_QUEUE * queue)
{

    check_expected(queue);
    optional_assignment(queue);
    mock();

}

bool 
SHMQueueIsDetached(SHM_QUEUE * queue)
{

    check_expected(queue);
    optional_assignment(queue);
    return (bool) mock();

}

void 
SHMQueueElemInit(SHM_QUEUE * queue)
{

    check_expected(queue);
    optional_assignment(queue);
    mock();

}

void 
SHMQueueDelete(SHM_QUEUE * queue)
{

    check_expected(queue);
    optional_assignment(queue);
    mock();

}

void 
SHMQueueInsertBefore(SHM_QUEUE * queue, SHM_QUEUE * elem)
{

    check_expected(queue);
    check_expected(elem);
    optional_assignment(queue);
    optional_assignment(elem);
    mock();

}

Pointer 
SHMQueueNext(SHM_QUEUE * queue, SHM_QUEUE * curElem, Size linkOffset)
{

    check_expected(queue);
    check_expected(curElem);
    check_expected(linkOffset);
    optional_assignment(queue);
    optional_assignment(curElem);
    return (Pointer) mock();

}

bool 
SHMQueueEmpty(SHM_QUEUE * queue)
{

    check_expected(queue);
    optional_assignment(queue);
    return (bool) mock();

}
