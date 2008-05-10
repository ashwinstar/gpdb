/* zookeeper_extra.h */

#ifndef ZOOKEEPER_EXTRA_H
#define ZOOKEEPER_EXTRA_H

#include <zookeeper.h>

ZOOAPI void zookeeperExtra_setMallocAndFree(void*(*m)(size_t),void(*f)(void*));

/* Leases share name space with Nodes.  If you name a lease "/lease", please do not name a Node "/lease" or "/lease-info". */
/* If you lose your lease, your session will expire. */

/* zookeeperExtra_lease returns a new zhandle_t, with which you call the other zookeeperExtra_lease_* functions. */

ZOOAPI int zookeeperExtra_lease(zhandle_t*zh,zhandle_t**result,const char*path,int timeout,int leaseTime);
ZOOAPI int zookeeperExtra_lease_renew(zhandle_t*zh,int leaseTime);
ZOOAPI int zookeeperExtra_lease_giveUp(zhandle_t*zh);
ZOOAPI int zookeeperExtra_lease_getExpireTime(zhandle_t*zh,time_t*expireTime);
/* zookeeperExtra_lease_delete deletes all record of the lease--you probably never want to do this. */
ZOOAPI int zookeeperExtra_lease_delete(zhandle_t*zh,const char*path);

#endif
