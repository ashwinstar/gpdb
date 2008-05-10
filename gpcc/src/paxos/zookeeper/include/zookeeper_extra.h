/* zookeeper_extra.h */

#ifndef ZOOKEEPER_EXTRA_H
#define ZOOKEEPER_EXTRA_H

#include <zookeeper.h>

ZOOAPI void zookeeperExtra_setMallocAndFree(void*(*m)(size_t),void(*f)(void*));

/* Leases share name space with Nodes.  If you name a lease "/lease", please do not name a Node "/lease" or "/lease-info". */
/* If you lose your lease, your session will expire. */

ZOOAPI int zookeeperExtra_lease(zhandle_t*zh,const char*path,int timeout,int leaseTime);
ZOOAPI int zookeeperExtra_lease_renew(zhandle_t*zh,int leaseTime);
ZOOAPI int zookeeperExtra_lease_giveUp(zhandle_t*zh);
ZOOAPI int zookeeperExtra_lease_getExpireTime(zhandle_t*zh,time_t*expireTime);
ZOOAPI int zookeeperExtra_lease_delete(zhandle_t*zh,const char*path);

#endif
