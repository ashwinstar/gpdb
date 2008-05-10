/* zookeeper.c */

#include "header.h"

ZOOAPI int
is_unrecoverable(zhandle_t*thiz)
{
    int rc;
    pthread_mutex_lock(&thiz->counted->mutex);
    rc = thiz->lease?ZOK:ZINVALIDSTATE;
    pthread_mutex_unlock(&thiz->counted->mutex);
    return rc;
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
    chubby_t*chubby;
    struct zoo_counted*counted=zookeeper_malloc(sizeof*counted);
    if (!counted)
        return 1;
    memset(counted,0,sizeof*counted);
    pthread_mutex_init(&counted->mutex,NULL);
    counted->count = 1;
    thiz->counted = counted;
    thiz->fn = fn;
    thiz->recv_timeout = recv_timeout;
    thiz->context = context;
    chubby = chubby_new(host,zookeeper_malloc,zookeeper_free);
    if (!chubby)
        return 1;
    thiz->counted->c = chubby_makeConnection(chubby);
    chubby_close(chubby);
    if (!thiz->counted->c)
        return 1;
    return chubby_connection_clone(thiz->counted->c,&thiz->lease);
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

zhandle_t*
zookeeper_clone(zhandle_t*thiz)
{
    zhandle_t*zh=zookeeper_malloc(sizeof*zh);
    if (zh){
	memset(zh,0,sizeof*zh);
	zh->counted = thiz->counted;
	zh->counted->count++;
	zh->fn = thiz->fn;
	zh->context = thiz->context;
	zh->recv_timeout = thiz->recv_timeout;
    }
    return zh;
}

ZOOAPI int
zookeeper_close(zhandle_t*thiz)
{
    if (thiz->counted){
        int i;
	pthread_mutex_lock(&thiz->counted->mutex);
	if (thiz->l)
	    chubby_connection_close(thiz->l);
	if (thiz->lease)
	    chubby_connection_close(thiz->lease);
	i = --thiz->counted->count;
	pthread_mutex_unlock(&thiz->counted->mutex);
	if (!i){
	    if (thiz->counted->c)
		chubby_connection_close(thiz->counted->c);
	    pthread_mutex_destroy(&thiz->counted->mutex);
	    zookeeper_free(thiz->counted);
	}
    }
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
    int i;
    pthread_mutex_lock(&thiz->counted->mutex);
    i = thiz->recv_timeout;
    pthread_mutex_unlock(&thiz->counted->mutex);
    return i;
}

ZOOAPI const void*
zoo_get_context(zhandle_t*thiz)
{
    void*c;
    pthread_mutex_lock(&thiz->counted->mutex);
    c = thiz->context;
    pthread_mutex_unlock(&thiz->counted->mutex);
    return c;
}

ZOOAPI void
zoo_set_context(zhandle_t*thiz,void*context)
{
    pthread_mutex_lock(&thiz->counted->mutex);
    thiz->context = context;
    pthread_mutex_unlock(&thiz->counted->mutex);
}

ZOOAPI watcher_fn
zoo_set_watcher(zhandle_t*thiz,watcher_fn newFn)
{
    watcher_fn old;
    pthread_mutex_lock(&thiz->counted->mutex);
    old = thiz->fn;
    thiz->fn = newFn;
    pthread_mutex_unlock(&thiz->counted->mutex);
    return old;
}

ZOOAPI int
zoo_state(zhandle_t*thiz)
{
    int rc;
    pthread_mutex_lock(&thiz->counted->mutex);
    rc = thiz->lease?CONNECTED_STATE:EXPIRED_SESSION_STATE;
    pthread_mutex_unlock(&thiz->counted->mutex);
    return rc;
}

ZOOAPI int
zoo_get_acl(zhandle_t*thiz,const char*path,struct ACL_vector*acl,struct Stat*stat)
{
    if (acl)
	memset(acl,0,sizeof*acl);
    if (stat)
	memset(stat,0,sizeof*stat);
    return ZOK;
}

ZOOAPI int
zoo_set_acl(zhandle_t*thiz,const char*path,int version,const struct ACL_vector*acl)
{
    return ZOK;
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
