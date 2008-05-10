/* get.c */

#include "header.h"

ZOOAPI int
zoo_get(zhandle_t*thiz,const char*path,int watch,char*buffer,int*buffer_len,struct Stat*stat)
{
    int i;
    struct zframe z;
    struct chubby_stat s;
    struct zoostat*zs;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->lease)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (watch)
        return zframe_returnRc(&z,ZUNIMPLEMENTED);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_get(z.l,&s,&z.string,mangled,*buffer_len+sizeof(struct zoostat),ZOOKEEPER_LOCKTIME);
    if (i)
	return zframe_returnE(&z,i);
    assert(z.string->length>=sizeof(struct zoostat));
    zs = (struct zoostat*)z.string->a;
    memcpy(buffer,z.string->a+sizeof(struct zoostat),z.string->length-sizeof(struct zoostat)<*buffer_len?z.string->length-sizeof(struct zoostat):*buffer_len);
    *buffer_len = s.size-sizeof(struct zoostat);
    if (stat){
	memset(stat,0,sizeof*stat);
	stat->czxid = stat->mzxid = s.decreeNumber;
	if (zs->create_dn)
	    stat->czxid = zoo_ntohll(zs->create_dn);
	stat->version = ntohl(zs->version);
    }
    return zframe_returnRc(&z,ZOK);
}
