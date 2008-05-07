/* zookeeper_extra.h */

#ifndef ZOOKEEPER_EXTRA_H
#define ZOOKEEPER_EXTRA_H

#include <zookeeper.h>

ZOOAPI int zoo_set_stat(zhandle_t *zh, const char *path, const char *buffer, int buflen,int version,struct Stat*stat);

ZOOAPI void zookeeperExtra_setMallocAndFree(void*(*m)(size_t),void(*f)(void*));

#endif
