/* version.c */

#include "header.h"

int
getVersion(struct zframe*z,char*path,int*version)
{
    chubby_string_t*s;
    char old=mangle_setChar(path,'v');
    int e=chubby_connection_get(z->zh->c,NULL,&s,path,ZOOKEEPER_LOCKTIME);
    mangle_setChar(path,old);
    if (e == CHUBBY_E_NOENT){
        *version = 0;
	return 0;
    }
    if (e == 0){
        *version = atoi(s->a);
	zookeeper_free(s);
	return 0;
    }
    return e;
}

int
setVersion(struct zframe*z,char*path,int version)
{
    int e;
    char old;
    chubby_string_t*s=zookeeper_malloc(sizeof*s+10);
    if (!s)
        return CHUBBY_E_OOM;
    sprintf(s->a,"%d",version);
    s->length = strlen(s->a);
    old = mangle_setChar(path,'v');
    e = chubby_connection_put(z->l,path,s);
    mangle_setChar(path,old);
    zookeeper_free(s);
    return e;
}

int
deleteVersion(struct zframe*z,char*path)
{
    char old=mangle_setChar(path,'v');
    int e=chubby_connection_remove(z->l,path);
    mangle_setChar(path,old);
    return e==CHUBBY_E_NOENT?0:e;
}
