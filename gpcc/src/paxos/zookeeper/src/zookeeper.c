/* zookeeper.c */

#include "header.h"

ZOOAPI int
is_unrecoverable(zhandle_t*thiz)
{
    return thiz->c?ZOK:ZINVALIDSTATE;
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

static int
zookeeper_init1(zhandle_t*thiz,const char*host,watcher_fn fn,int recv_timeout,void*context)
{
    thiz->fn = fn;
    thiz->recv_timeout = recv_timeout;
    thiz->context = context;
    thiz->chubby = chubby_new(host,zookeeper_malloc,zookeeper_free);
    if (!thiz->chubby)
        return 1;
    thiz->c = chubby_makeConnection(thiz->chubby);
    if (!thiz->c)
        return 1;
    return 0;
}

ZOOAPI zhandle_t*
zookeeper_init(const char*host,watcher_fn fn,int recv_timeout,const clientid_t*clientid,void*context,int flags)
{
    zhandle_t*thiz=zookeeper_malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	if (zookeeper_init1(thiz,host,fn,recv_timeout,context)){
	    zookeeper_close(thiz);
	    return NULL;
	}
    }
    return thiz;
}

ZOOAPI int
zookeeper_close(zhandle_t*thiz)
{
    if (thiz->chubby)
        chubby_close(thiz->chubby);
    if (thiz->c)
        chubby_connection_close(thiz->c);
    zookeeper_free(thiz);
    return ZOK;
}

ZOOAPI const clientid_t*
zoo_client_id(zhandle_t*thiz)
{
    static const clientid_t id;
    return &id;
}

ZOOAPI int
zoo_recv_timeout(zhandle_t*thiz)
{
    return thiz->recv_timeout;
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
    return thiz->c?CONNECTED_STATE:EXPIRED_SESSION_STATE;
}

ZOOAPI const char*
zerror(int c)
{
    switch (c){
	case ZOK:return "ok";
	case ZSYSTEMERROR:return "system error";
	case ZRUNTIMEINCONSISTENCY:return "run time inconsistency";
	case ZDATAINCONSISTENCY:return "data inconsistency";
	case ZCONNECTIONLOSS:return "connection loss";
	case ZMARSHALLINGERROR:return "marshalling error";
	case ZUNIMPLEMENTED:return "unimplemented";
	case ZOPERATIONTIMEOUT:return "operation timeout";
	case ZBADARGUMENTS:return "bad arguments";
	case ZINVALIDSTATE:return "invalid zhandle state";
	case ZAPIERROR:return "api error";
	case ZNONODE:return "no node";
	case ZNOAUTH:return "not authenticated";
	case ZBADVERSION:return "bad version";
	case ZNOCHILDRENFOREPHEMERALS:return "no children for ephemerals";
	case ZNODEEXISTS:return "node exists";
	case ZNOTEMPTY:return "not empty";
	case ZSESSIONEXPIRED:return "session expired";
	case ZINVALIDCALLBACK:return "invalid callback";
	case ZINVALIDACL:return "invalid acl";
	case ZAUTHFAILED:return "authentication failed";
	case ZCLOSING:return "ZCLOSING";
    }
    return "unknown error";
}
