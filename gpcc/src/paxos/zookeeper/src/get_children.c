/* get_children.c */

#include "header.h"

ZOOAPI int
zoo_get_children(zhandle_t*thiz,const char*path,int watch,struct String_vector*strings)
{
    int i;
    struct zframe z;
    char**a;
    char mangled[PATH_MAX];
    zframe_init(&z,thiz);
    if (!thiz->c)
        return zframe_returnRc(&z,ZINVALIDSTATE);
    if (watch)
        return zframe_returnRc(&z,ZUNIMPLEMENTED);
    if (mangle_path(mangled,sizeof mangled,path))
	return zframe_returnRc(&z,ZBADARGUMENTS);
    mangle_setChar(mangled,'d');
    if (zframe_lock(&z))
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    i = chubby_connection_list(z.l,&z.list,mangled,ZOOKEEPER_LOCKTIME);
    if (i)
	return zframe_returnE(&z,i);
    for (a=z.list; *a; a++);
    strings->count = 0;
    strings->data = zookeeper_malloc((a-z.list)*sizeof(char*));
    if (!strings->data)
	return zframe_returnRc(&z,ZMARSHALLINGERROR);
    for (a=z.list; *a; a++){
        char*b=*a;
        if (b[0]=='f' && b[1]=='-'){
	    strings->data[strings->count] = zookeeper_malloc(strlen(b+1));
	    if (!strings->data[strings->count]){
		deallocate_String_vector(strings);
		return zframe_returnRc(&z,ZMARSHALLINGERROR);
	    }
	    strcpy(strings->data[strings->count++],b+2);
	}
    }
    return zframe_returnRc(&z,ZOK);
}
