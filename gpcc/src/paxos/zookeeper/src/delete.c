/* delete.c */

#include "header.h"

ZOOAPI int
zoo_delete(zhandle_t*thiz,const char*path,int version)
{
    char realpath[PATH_MAX];
    int i;
    struct zframe z;
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (mangle_path(realpath,sizeof realpath,path,0))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    if (version != -1){
        int v;
	if (getVersion(&z,realpath,&v))
	    return zframe_returnRc(&z,ZMARSHALLINGERROR);
	if (version != v)
	    return zframe_returnRc(&z,ZBADVERSION);
    }
    i = chubby_connection_remove(z.l,realpath);
    return zframe_returnE(&z,i);
}
