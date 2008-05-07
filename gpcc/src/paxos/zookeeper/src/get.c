/* get.c */

#include "header.h"

ZOOAPI int
zoo_get(zhandle_t*thiz,const char*path,int watch,char*buffer,int*buffer_len,struct Stat*stat)
{
    int i,v;
    struct zframe z;
    struct chubby_stat s;
    chubby_string_t*answer;
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
    i = chubby_connection_get(z.l,&s,&answer,mangled,ZOOKEEPER_LOCKTIME);
    if (i)
	return zframe_returnE(&z,i);
    memcpy(buffer,answer->a,answer->length<*buffer_len?answer->length:*buffer_len);
    *buffer_len = answer->length;
    zookeeper_free(answer);
    if (stat){
	if (getVersion(&z,mangled,&v))
	    return zframe_returnRc(&z,ZMARSHALLINGERROR);
	memset(stat,0,sizeof*stat);
	stat->czxid = stat->mzxid = s.decreeNumber;
	stat->version = v;
    }
    return zframe_returnRc(&z,ZOK);
}
