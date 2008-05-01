/* zookeeper.c */

#include "chubby_private.h"

static void
chubby_delete(struct chubby_counted*_thiz)
{
    zhandle_t*thiz=(zhandle_t*)_thiz;
    if (thiz->addresses)
	chubby_free(thiz->addresses);
    if (thiz->paxosKey)
	chubby_free(thiz->paxosKey);
    chubby_free(thiz);
}

static int
chubby_init(zhandle_t*thiz,const char*addresses,watcher_fn fn,void*context)
{
    thiz->addresses = chubby_malloc(strlen(addresses)+1);
    if (!thiz->addresses)
	return 1;
    strcpy(thiz->addresses,addresses);
    thiz->fn = fn;
    thiz->context = context;
    if (gethostname(thiz->id,_POSIX_HOST_NAME_MAX))
	return 1;
    sprintf(thiz->id+strlen(thiz->id),"-%d-%d-%p",(int)getpid(),(int)time(NULL),thiz);
    if (chubby_readPaxosKey(thiz))
        return 1;
    return 0;
}

ZOOAPI zhandle_t*
zookeeper_init(const char*host,watcher_fn fn,int recv_timeout,const clientid_t*clientid,void*context,int flags)
{
    zhandle_t*thiz=chubby_malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_delete;
	if (chubby_init(thiz,host,fn,context)){
	    chubby_counted_close(thiz);
	    return NULL;
	}
    }
    return thiz;
}

ZOOAPI int
zookeeper_close(zhandle_t*thiz)
{
    chubby_counted_close(thiz);
    return ZOK;
}

ZOOAPI const clientid_t*
zoo_client_id(zhandle_t*thiz)
{
    return NULL;
}

ZOOAPI int
zoo_recv_timeout(zhandle_t*thiz)
{
    return 0;
}

ZOOAPI const void*
zoo_get_context(zhandle_t*thiz)
{
    return thiz->context;
}

ZOOAPI void
zoo_set_context(zhandle_t*thiz,void*context)
{
    thiz->context = context;
}

ZOOAPI watcher_fn
zoo_set_watcher(zhandle_t*thiz,watcher_fn newFn)
{
    watcher_fn old=thiz->fn;
    thiz->fn = newFn;
    return old;
}

ZOOAPI int
zoo_state(zhandle_t*thiz)
{
    return CONNECTED_STATE;
}

ZOOAPI int
zoo_acreate(zhandle_t*thiz,const char*path1,const char*value,int valuelen,const struct ACL_vector*acl,int flags,string_completion_t completion,const void *data)
{
    struct chubby_stat stat;
    char path[PATH_MAX];
    strcpy(path,path1);
    if (flags&EPHEMERAL)
	return ZBADARGUMENTS; // We don't support EPHEMERAL.
    if (flags&SEQUENCE){
	if (chubby_stat(thiz,&stat,"/paxos/decreeNumber"))
	    return ZMARSHALLINGERROR;
	sprintf(path+strlen(path),"%"APR_INT64_T_FMT,stat.decreeNumber);
    }
    if (chubby_stat(thiz,&stat,path))
	return ZMARSHALLINGERROR;
    if (stat.decreeNumber){
	completion(ZNODEEXISTS,NULL,data);
	return ZOK;
    }
//...
    return ZOK;
}
