/* create.c */

#include "header.h"

ZOOAPI int
zoo_create(zhandle_t*thiz,const char*path,const char*value,int valuelen,const struct ACL_vector*acl,int flags,char*realpath,int max_realpath_len)
{
    int i;
    struct zframe z;
    struct chubby_stat stat;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->lease)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (flags&EPHEMERAL)
        return zframe_returnRc(&z,ZUNIMPLEMENTED);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    if (flags&SEQUENCE){
	if (chubby_connection_stat(thiz->counted->c,&stat,"/paxos/decreeNumber"))
	    return zframe_returnRc(&z,ZMARSHALLINGERROR);
	i = strlen(mangled);
	if (snprintf(mangled+i,sizeof mangled-i,"%"APR_INT64_T_FMT,stat.decreeNumber) >= sizeof mangled-i)
	    return zframe_returnRc(&z,ZBADARGUMENTS);
    }
    z.string = zookeeper_malloc(sizeof*z.string+valuelen+sizeof(struct zoostat));
    if (!z.string)
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    z.string->length = valuelen+sizeof(struct zoostat);
    memset(z.string->a,0,sizeof(struct zoostat));
    memcpy(z.string->a+sizeof(struct zoostat),value,valuelen);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_stat(z.l,&stat,mangled);
    if (i)
	return zframe_returnE(&z,i);
    if (stat.decreeNumber)
	return zframe_returnRc(&z,ZNODEEXISTS);
    i = zoochubby_set1(z.l,mangled,z.string);
    if (i)
	return zframe_returnE(&z,i);
    i = unmangle_path(mangled);
    assert(!i);
    snprintf(realpath,max_realpath_len,"%s",mangled);
    return zframe_returnRc(&z,ZOK);
}
