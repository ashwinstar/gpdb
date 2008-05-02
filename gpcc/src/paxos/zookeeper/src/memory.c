/* memory.c */

#include "header.h"

void*(*zookeeper_malloc)(size_t)=malloc;
void(*zookeeper_free)(void*)=free;

void
zookeeperExtra_setMallocAndFree(void*(*m)(size_t),void(*f)(void*))
{
    zookeeper_malloc = m;
    zookeeper_free = f;
}
