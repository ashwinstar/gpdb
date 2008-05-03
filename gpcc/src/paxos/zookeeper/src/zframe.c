/* zframe.c */

#include "header.h"

#define ZOOKEEPER_LOCK		"zoo/lock"
#define ZOOKEEPER_LOCK_TIMEOUT	600

void
zframe_init(struct zframe*thiz,zhandle_t*zh)
{
    memset(thiz,0,sizeof*thiz);
    thiz->zh = zh;
}

int
zframe_lock(struct zframe*thiz)
{
    assert(!thiz->l);
    if (chubby_connection_lock(thiz->zh->c,&thiz->l,ZOOKEEPER_LOCK,ZOOKEEPER_LOCK_TIMEOUT,ZOOKEEPER_LOCKTIME))
        return 1;
    return 0;
}

void
zframe_unlock(struct zframe*thiz)
{
    if (thiz->l){
	chubby_connection_giveUpLock(thiz->l);
	chubby_connection_close(thiz->l);
	thiz->l = NULL;
    }
}

int
zframe_returnRc(struct zframe*thiz,int rc)
{
    if (thiz->a)
        zookeeper_free(thiz->a);
    zframe_unlock(thiz);
    if (rc == ZCONNECTIONLOSS){
	chubby_connection_close(thiz->zh->c);
	thiz->zh->c = NULL;
    }
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
