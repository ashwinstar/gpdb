/* exists.c */

#include "header.h"

ZOOAPI int
zoo_exists(zhandle_t*thiz,const char*path,int watch,struct Stat*stat)
{
    int i,v;
    struct zframe z;
    struct chubby_stat s;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (watch)
        return zframe_returnRc(&z,ZUNIMPLEMENTED);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_stat(z.l,&s,mangled);
    if (i)
	return zframe_returnE(&z,i);
    if (!s.decreeNumber)
	return zframe_returnRc(&z,ZNONODE);
    if (getVersion(&z,mangled,&v))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    memset(stat,0,sizeof*stat);
    stat->czxid = stat->mzxid = s.decreeNumber;
    stat->version = v;
    return zframe_returnRc(&z,ZOK);
}
