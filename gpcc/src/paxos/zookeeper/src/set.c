/* set.c */

#include "header.h"

#define ZOOKEEPER_TMP_FILE	"zoo/temp"

int
zoochubby_set1(chubby_connection_t*l,const char*name,chubby_string_t*string)
{
    int i;
    if (string->length <= CHUBBY_CHUNK_SIZE)
	return chubby_connection_put(l,name,string);
    i = chubby_connection_putBigFile(l,ZOOKEEPER_TMP_FILE,string,ZOOKEEPER_LOCKTIME);
    if (i)
	return i;
    return chubby_connection_move(l,ZOOKEEPER_TMP_FILE,name);
}

ZOOAPI int
zoo_set(zhandle_t*thiz,const char*path,const char*buffer,int buflen,int version)
{
    int i;
    struct zframe z;
    chubby_string_t*s;
    struct zoostat*zs;
    struct chubby_stat stat;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    z.string = zookeeper_malloc(sizeof*z.string+buflen+sizeof(struct zoostat));
    if (!z.string)
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    z.string->length = buflen+sizeof(struct zoostat);
    memcpy(z.string->a+sizeof(struct zoostat),buffer,buflen);
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_get(z.l,&stat,&s,mangled,sizeof(struct zoostat),0);
    if (i)
	return zframe_returnE(&z,i);
    assert(s->length>=sizeof(struct zoostat));
    memcpy(z.string->a,s->a,sizeof(struct zoostat));
    zookeeper_free(s);
    zs = (struct zoostat*)z.string->a;
    if (version!=-1 && version!=ntohl(zs->version))
	return zframe_returnRc(&z,ZBADVERSION);
    if (zs->create_dn == 0)
	zs->create_dn = zoo_htonll(stat.decreeNumber);
    zs->version = htonl(ntohl(zs->version)+1);
    i = zoochubby_set1(z.l,mangled,z.string);
    return zframe_returnE(&z,i);
}
