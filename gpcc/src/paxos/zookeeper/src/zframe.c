/* zframe.c */

#include "header.h"

#define ZOOKEEPER_LOCK		"zoo/lock"
#define ZOOKEEPER_LOCK_TIMEOUT	600

void
zframe_init(struct zframe*thiz,zhandle_t*zh)
{
    memset(thiz,0,sizeof*thiz);
    thiz->zh = zh;
    pthread_mutex_lock(&zh->mutex);
}

int
zframe_lock(struct zframe*thiz)
{
    assert(!thiz->l);
    if (thiz->zh->l){
	if (!chubby_connection_renew(thiz->zh->l,ZOOKEEPER_LOCKTIME)){
	    thiz->l = thiz->zh->l;
	    return 0;
	}
	chubby_connection_close(thiz->zh->l);
	thiz->zh->l = NULL;
    }
    if (chubby_connection_lock(thiz->zh->c,&thiz->zh->l,ZOOKEEPER_LOCK,ZOOKEEPER_LOCK_TIMEOUT,ZOOKEEPER_LOCKTIME))
        return 1;
    thiz->l = thiz->zh->l;
    return 0;
}

void
zframe_renew(struct zframe*thiz)
{
    time_t t;
    chubby_connection_getLockExpireTime(thiz->l,&t);
    if (t <= time(NULL)+ZOOKEEPER_LOCKTIME*3/4)
	chubby_connection_renew(thiz->l,ZOOKEEPER_LOCKTIME);
}

void
zframe_unlock(struct zframe*thiz)
{
    if (thiz->l){
	chubby_connection_giveUpLock(thiz->l);
	thiz->l = NULL;
    }
}

int
zframe_returnRc(struct zframe*thiz,int rc)
{
    zframe_unlock(thiz);
    if (thiz->string)
        zookeeper_free(thiz->string);
    if (thiz->list)
        chubby_connection_listFree(thiz->zh->c,thiz->list);
    if (rc == ZCONNECTIONLOSS){
	chubby_connection_close(thiz->zh->l);
	thiz->zh->l = NULL;
	chubby_connection_close(thiz->zh->c);
	thiz->zh->c = NULL;
    }
    pthread_mutex_unlock(&thiz->zh->mutex);
    return rc;
}

int
zframe_returnE(struct zframe*thiz,int e)
{
    int rc;
    switch (e){
	case 0:
	    rc = ZOK;
	    break;
	case CHUBBY_E_OOM:
	case CHUBBY_E_IO:
	    rc = ZMARSHALLINGERROR;
	    break;
	case CHUBBY_E_NOENT:
	    rc = ZNONODE;
	    break;
	case CHUBBY_E_LOCKEXPIRED:
	    rc = ZCONNECTIONLOSS;
	    break;
	default:
	    assert(0);
	    rc = ZMARSHALLINGERROR;
	    break;
    }
    return zframe_returnRc(thiz,rc);
}
