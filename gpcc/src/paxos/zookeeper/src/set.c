/* set.c */

#include "header.h"

ZOOAPI int
zoo_set(zhandle_t*thiz,const char*path,const char*buffer,int buflen,int version)
{
    int i,v;
    struct zframe z;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    z.string = zookeeper_malloc(sizeof*z.string+buflen);
    if (!z.string)
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    z.string->length = buflen;
    memcpy(z.string->a,buffer,buflen);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    if (getVersion(&z,mangled,&v))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    if (version!=-1 && version!=v)
	    return zframe_returnRc(&z,ZBADVERSION);
    i = setVersion(&z,mangled,v+1);
    if (i)
	return zframe_returnE(&z,i);
    i = chubby_connection_putBigFile(z.l,mangled,z.string,ZOOKEEPER_LOCKTIME);
    return zframe_returnE(&z,i);
}
