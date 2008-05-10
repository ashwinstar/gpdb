/* lease.c */

#include "header.h"

ZOOAPI int
zookeeperExtra_lease(zhandle_t*thiz,const char*path,int timeout,int leaseTime)
{
    int i;
    struct zframe z;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (thiz->lease)
	return zframe_returnRc(&z,ZBADARGUMENTS);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    i = chubby_connection_lock(thiz->c,&thiz->lease,mangled,timeout,leaseTime);
    if (thiz->lease){
	chubby_connection_close(thiz->l);
	thiz->l = NULL;
    }
    return zframe_returnE(&z,i);
}

ZOOAPI int
zookeeperExtra_lease_renew(zhandle_t*thiz,int leaseTime)
{
    int i;
    struct zframe z;
    zframe_init(&z,thiz);
    if (!thiz->lease)
	return zframe_returnRc(&z,ZBADARGUMENTS);
    i = chubby_connection_renew(thiz->lease,leaseTime);
    return zframe_returnE(&z,i);
}

ZOOAPI int
zookeeperExtra_lease_giveUp(zhandle_t*thiz)
{
    return zookeeperExtra_lease_renew(thiz,0);
}

ZOOAPI int
zookeeperExtra_lease_getExpireTime(zhandle_t*thiz,time_t*expireTime)
{
    int i;
    struct zframe z;
    zframe_init(&z,thiz);
    if (!thiz->lease)
	return zframe_returnRc(&z,ZBADARGUMENTS);
    i = chubby_connection_getLockExpireTime(thiz->lease,expireTime);
    return zframe_returnE(&z,i);
}

ZOOAPI int
zookeeperExtra_lease_delete(zhandle_t*thiz,const char*path)
{
    int i;
    struct zframe z;
    char mangled[PATH_MAX+5];
    zframe_init(&z,thiz);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    i = chubby_connection_remove(thiz->c,mangled);
    if (i)
	return zframe_returnE(&z,i);
    strcat(mangled,"-info");
    i = chubby_connection_remove(thiz->c,mangled);
    return zframe_returnE(&z,i);
}
