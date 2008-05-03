/* zframe.h */

#ifndef ZFRAME_H
#define ZFRAME_H

struct zframe{
    zhandle_t*zh;
    chubby_connection_t*l;
    void*a;
};

void zframe_init(struct zframe*thiz,zhandle_t*zh);

int zframe_lock(struct zframe*thiz);
void zframe_renew(struct zframe*thiz);
void zframe_unlock(struct zframe*thiz);

int zframe_returnRc(struct zframe*thiz,int rc);
int zframe_returnE(struct zframe*thiz,int e);

#endif
