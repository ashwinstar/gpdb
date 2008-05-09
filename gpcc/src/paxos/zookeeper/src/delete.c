/* delete.c */

#include "header.h"

ZOOAPI int
zoo_delete(zhandle_t*thiz,const char*path,int version)
{
    int i;
    struct zoostat*zs;
    struct zframe z;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    if (version != -1){
	i = chubby_connection_get(z.l,NULL,&z.string,mangled,sizeof(struct zoostat),0);
	if (i)
	    return zframe_returnE(&z,i);
	zs = (struct zoostat*)z.string->a;
	if (version != ntohl(zs->version))
	    return zframe_returnRc(&z,ZBADVERSION);
    }
    i = chubby_connection_remove(z.l,mangled);
    return zframe_returnE(&z,i);
}
