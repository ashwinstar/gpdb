/* delete.c */

#include "header.h"

ZOOAPI int
zoo_delete(zhandle_t*thiz,const char*path,int version)
{
    char mangled[PATH_MAX];
    int e;
    struct zframe z;
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    if (version != -1){
        int v;
	if (getVersion(&z,mangled,&v))
	    return zframe_returnRc(&z,ZMARSHALLINGERROR);
	if (version != v)
	    return zframe_returnRc(&z,ZBADVERSION);
    }
    e = chubby_connection_remove(z.l,mangled);
    if (e)
	return zframe_returnE(&z,e);
    deleteVersion(&z,mangled);
    return zframe_returnRc(&z,ZOK);
}
