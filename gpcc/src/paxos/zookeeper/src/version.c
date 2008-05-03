/* version.c */

#include "header.h"

int
getVersion(struct zframe*z,char*path,int*version)
{
    chubby_string_t*s;
    char old=mangle_setChar(path,'v');
    int i=chubby_connection_get(z->l,NULL,&s,path,ZOOKEEPER_LOCKTIME);
    mangle_setChar(path,old);
    if (i == CHUBBY_E_NOENT){
        *version = 0;
	return 0;
    }
    if (i == 0){
        *version = atoi(s->a);
	zookeeper_free(s);
	return 0;
    }
    return 1;
}

int
setVersion(struct zframe*z,char*path,int version)
{
    int i;
    char old;
    chubby_string_t*s=zookeeper_malloc(sizeof*s+10);
    if (!s)
        return 1;
    sprintf(s->a,"%d",version);
    s->length = strlen(s->a);
    old = mangle_setChar(path,'v');
    i = chubby_connection_put(z->l,path,s);
    mangle_setChar(path,old);
    zookeeper_free(s);
    return i;
}
