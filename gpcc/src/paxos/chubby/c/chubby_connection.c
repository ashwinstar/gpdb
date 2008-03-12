/* chubby_connection.c */

#include "chubby_private.h"

struct chubby_connection{
    struct chubby_counted super;
    chubby_t*chubby;
    chubby_connection_t*parent;
    char lockFilename[PATH_MAX];
    char lockString[PATH_MAX+30];
    int64_t lockDn;
    char lockId[PATH_MAX];
    time_t lockExpireTime;
};

static void
chubby_connection_delete(struct chubby_counted*_thiz)
{
    chubby_connection_t*thiz=(chubby_connection_t*)_thiz;
    chubby_counted_set(&thiz->chubby,NULL);
    chubby_counted_set(&thiz->parent,NULL);
    free(thiz);
}

chubby_connection_t*
chubby_connection_new(chubby_t*chubby)
{
    chubby_connection_t*thiz=malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_connection_delete;
	chubby_counted_set(&thiz->chubby,chubby);
    }
    return thiz;
}

void
chubby_connection_close(chubby_connection_t*thiz)
{
    chubby_counted_close(thiz);
}

int
chubby_connection_get(chubby_connection_t*thiz,chubby_sb_t*result,const char*name)
{
    return -1;
}

int
chubby_connection_stat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name)
{
    return -1;
}

int
chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_sb_t*contents)
{
    return -1;
}

int
chubby_connection_putBigFile(chubby_connection_t*thiz,const char*name,chubby_sb_t*contents)
{
    return -1;
}

int
chubby_connection_remove(chubby_connection_t*thiz,const char*name)
{
    return -1;
}

int
chubby_connection_list(chubby_connection_t*thiz,chubby_sb_t*result,const char*name)
{
    return -1;
}

chubby_connection_t*
chubby_connection_getLockFilenameDecreeNumberConnection(chubby_connection_t*thiz,const char*name,int64_t decreeNumber)
{
    return NULL;
}

chubby_connection_t*
chubby_connection_getLockStringConnection(chubby_connection_t*thiz,const char*lockString)
{
    return NULL;
}

const char*
chubby_connection_getLockString(chubby_connection_t*thiz)
{
     return NULL;
}

int
chubby_connection_isLockValid(chubby_connection_t*thiz)
{
    return 0;
}

int
chubby_connection_lock(chubby_connection_t*thiz,const char*name,int timeout,int lockTime)
{
    return -1;
}

int
chubby_connection_renew(chubby_connection_t*thiz,int lockTime)
{
    return -1;
}

time_t
chubby_connection_getLockExpireTime(chubby_connection_t*thiz)
{
    return 0;
}

void
chubby_connection_giveUpLock(chubby_connection_t*thiz)
{
}
