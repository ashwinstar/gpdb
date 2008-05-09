/* zookeeper_extra.h */

#ifndef ZOOKEEPER_EXTRA_H
#define ZOOKEEPER_EXTRA_H

#include <zookeeper.h>

ZOOAPI void zookeeperExtra_setMallocAndFree(void*(*m)(size_t),void(*f)(void*));

#endif
