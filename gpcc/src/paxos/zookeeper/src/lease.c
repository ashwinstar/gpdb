/* lease.c */

#include "header.h"

ZOOAPI int
zookeeperExtra_lease(zhandle_t*thiz,zhandle_t**result,const char*path,int timeout,int leaseTime)
{
    int i;
    zhandle_t*zh;
    struct zframe z;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    zh = zookeeper_clone(thiz);
    if (!zh)
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_lock(thiz->lease,&zh->lease,mangled,timeout,leaseTime);
    if (i){
        zookeeper_close(zh);
	return zframe_returnE(&z,i);
    }
    *result = zh;
    return zframe_returnE(&z,ZOK);
}

ZOOAPI int
zookeeperExtra_lease_renew(zhandle_t*thiz,int leaseTime)
{
    int i;
    struct zframe z;
    zframe_init(&z,thiz);
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
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_remove(z.l,mangled);
    if (i)
	return zframe_returnE(&z,i);
    strcat(mangled,"-info");
    i = chubby_connection_remove(z.l,mangled);
    return zframe_returnE(&z,i);
}
