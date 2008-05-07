/* create.c */

#include "header.h"

ZOOAPI int
zoo_create(zhandle_t*thiz,const char*path,const char*value,int valuelen,const struct ACL_vector*acl,int flags,char*realpath,int max_realpath_len)
{
    int i;
    struct zframe z;
    struct chubby_stat stat;
    chubby_string_t*s;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (flags&EPHEMERAL)
        return zframe_returnRc(&z,ZUNIMPLEMENTED);
    if (flags&SEQUENCE){
	if (chubby_connection_stat(thiz->c,&stat,"/paxos/decreeNumber"))
	    return zframe_returnRc(&z,ZMARSHALLINGERROR);
	if (mangle_path(mangled,sizeof mangled,path))
	    return zframe_returnRc(&z,ZBADARGUMENTS);
	i = strlen(mangled);
	if (snprintf(mangled+i,sizeof mangled-i,"%"APR_INT64_T_FMT,stat.decreeNumber) >= sizeof mangled-i)
	    return zframe_returnRc(&z,ZBADARGUMENTS);
    }else if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    z.a = s = zookeeper_malloc(sizeof*s+valuelen);
    if (!s)
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    s->length = valuelen;
    memcpy(s->a,value,valuelen);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_stat(z.l,&stat,mangled);
    if (i)
	return zframe_returnE(&z,i);
    if (stat.decreeNumber)
	return zframe_returnRc(&z,ZNODEEXISTS);
    if (deleteVersion(&z,mangled))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_putBigFile(z.l,mangled,s,ZOOKEEPER_LOCKTIME);
    if (i)
	return zframe_returnE(&z,i);
    zframe_unlock(&z);
    i = unmangle_path(mangled);
    assert(!i);
    snprintf(realpath,max_realpath_len,"%s",mangled);
    return zframe_returnRc(&z,ZOK);
}
