/* zookeeper.c */

#include "header.h"

ZOOAPI int
is_unrecoverable(zhandle_t *zh)
{
    return ZOK;
}

ZOOAPI void
zoo_set_debug_level(ZooLogLevel logLevel)
{
}

ZOOAPI void
zoo_set_log_stream(FILE*logStream)
{
}

ZOOAPI void
zoo_deterministic_conn_order(int yesOrNo)
{
}

ZOOAPI zhandle_t*
zookeeper_init(const char*host,watcher_fn fn,int recv_timeout,const clientid_t*clientid,void*context,int flags)
{
    zhandle_t*thiz=zookeeper_malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->fn = fn;
	thiz->context = context;
    }
    return thiz;
}

ZOOAPI int
zookeeper_close(zhandle_t*thiz)
{
    zookeeper_free(thiz);
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
//...
    return CONNECTED_STATE;
}

ZOOAPI int
zoo_create(zhandle_t*thiz,const char*path,const char*value,int valuelen,const struct ACL_vector*acl,int flags,char*realpath,int max_realpath_len)
{
//...
    return ZOK;
}

#define ZERRORCASE(a)	case a:return #a;
ZOOAPI const char*
zerror(int c)
{
    switch (c){
	ZERRORCASE(ZOK)
	ZERRORCASE(ZSYSTEMERROR)
	ZERRORCASE(ZRUNTIMEINCONSISTENCY)
	ZERRORCASE(ZDATAINCONSISTENCY)
	ZERRORCASE(ZCONNECTIONLOSS)
	ZERRORCASE(ZMARSHALLINGERROR)
	ZERRORCASE(ZUNIMPLEMENTED)
	ZERRORCASE(ZOPERATIONTIMEOUT)
	ZERRORCASE(ZBADARGUMENTS)
	ZERRORCASE(ZINVALIDSTATE)
	ZERRORCASE(ZAPIERROR)
	ZERRORCASE(ZNONODE)
	ZERRORCASE(ZNOAUTH)
	ZERRORCASE(ZBADVERSION)
	ZERRORCASE(ZNOCHILDRENFOREPHEMERALS)
	ZERRORCASE(ZNODEEXISTS)
	ZERRORCASE(ZNOTEMPTY)
	ZERRORCASE(ZSESSIONEXPIRED)
	ZERRORCASE(ZINVALIDCALLBACK)
	ZERRORCASE(ZINVALIDACL)
	ZERRORCASE(ZAUTHFAILED)
	ZERRORCASE(ZCLOSING)
    }
    return "Zerror does not know what that return code is.";
}
