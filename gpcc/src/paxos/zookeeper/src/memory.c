/* memory.c */

#include "chubby_private.h"

void*(*chubby_malloc)(size_t)=malloc;
void(*chubby_free)(void*)=free;

void
zookeeperExtra_setMallocAndFree(void*(*m)(size_t),void(*f)(void*))
{
    chubby_malloc = m;
    chubby_free = f;
}
