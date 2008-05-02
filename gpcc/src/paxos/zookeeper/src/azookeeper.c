/* azookeeper.c */

#include "header.h"

static int
complete_void(void_completion_t completion,int rc,const void*data)
{
    switch (rc){
	case ZBADARGUMENTS:
	case ZINVALIDSTATE:
	case ZMARSHALLINGERROR:
	    return rc;
    }
    completion(rc,data);
    return ZOK;
}

static int
complete_stat(stat_completion_t completion,int rc,const struct Stat*stat,const void*data)
{
    switch (rc){
	case ZBADARGUMENTS:
	case ZINVALIDSTATE:
	case ZMARSHALLINGERROR:
	    return rc;
    }
    completion(rc,stat,data);
    return ZOK;
}

static int
complete_string(string_completion_t completion,int rc,const char*value,const void*data)
{
    switch (rc){
	case ZBADARGUMENTS:
	case ZINVALIDSTATE:
	case ZMARSHALLINGERROR:
	    return rc;
    }
    completion(rc,value,data);
    return ZOK;
}

static int
complete_strings(strings_completion_t completion,int rc,const struct String_vector*strings,const void*data)
{
    switch (rc){
	case ZBADARGUMENTS:
	case ZINVALIDSTATE:
	case ZMARSHALLINGERROR:
	    return rc;
    }
    completion(rc,strings,data);
    return ZOK;
}

static int
complete_data(data_completion_t completion,int rc,const char*value,int value_len,const struct Stat*stat,const void*data)
{
    switch (rc){
	case ZBADARGUMENTS:
	case ZINVALIDSTATE:
	case ZMARSHALLINGERROR:
	    return rc;
    }
    completion(rc,value,value_len,stat,data);
    return ZOK;
}

static int
complete_acl(acl_completion_t completion,int rc,struct ACL_vector*acl,struct Stat*stat,const void*data)
{
    switch (rc){
	case ZBADARGUMENTS:
	case ZINVALIDSTATE:
	case ZMARSHALLINGERROR:
	    return rc;
    }
    completion(rc,acl,stat,data);
    return ZOK;
}

ZOOAPI int
zoo_acreate(zhandle_t*thiz,const char*path,const char*value,int valuelen,const struct ACL_vector*acl,int flags,string_completion_t completion,const void*data)
{
    char realpath[PATH_MAX];
    int rc=zoo_create(thiz,path,value,valuelen,acl,flags,realpath,sizeof realpath);
    return complete_string(completion,rc,realpath,data);
}

ZOOAPI int
zoo_adelete(zhandle_t*thiz,const char*path,int version,void_completion_t completion,const void*data)
{
    int rc=zoo_delete(thiz,path,version);
    return complete_void(completion,rc,data);
}

ZOOAPI int
zoo_aexists(zhandle_t*thiz,const char*path,int watch,stat_completion_t completion,const void*data)
{
    struct Stat stat;
    int rc=zoo_exists(thiz,path,watch,&stat);
    return complete_stat(completion,rc,&stat,data);
}

ZOOAPI int
zoo_aget(zhandle_t*thiz,const char*path,int watch,data_completion_t completion,const void*data)
{
    struct Stat stat;
    char*buffer=NULL;
    int buffer_len=1,rc,size;
    do{
	size = buffer_len;
	if (buffer)
	    zookeeper_free(buffer);
	buffer = zookeeper_malloc(size);
	if (!buffer)
	    return ZMARSHALLINGERROR;
	rc = zoo_get(thiz,path,watch,buffer,&buffer_len,&stat);
    }while (rc==ZOK && buffer_len>size);
    rc = complete_data(completion,rc,buffer,buffer_len,&stat,data);
    zookeeper_free(buffer);
    return rc;
}

ZOOAPI int
zoo_aset(zhandle_t*thiz,const char*path,const char*buffer,int buflen,int version,stat_completion_t completion,const void *data)
{
    int rc=zoo_set(thiz,path,buffer,buflen,version);
    return complete_stat(completion,rc,NULL,data);
}

ZOOAPI int
zoo_aget_children(zhandle_t*thiz,const char*path,int watch,strings_completion_t completion,const void*data)
{
    struct String_vector strings;
    int rc;
    memset(&strings,0,sizeof strings);
    rc = zoo_get_children(thiz,path,watch,&strings);
    rc = complete_strings(completion,rc,&strings,data);
    deallocate_String_vector(&strings);
    return rc;
}

ZOOAPI int
zoo_async(zhandle_t*thiz,const char*path,string_completion_t completion,const void*data)
{
    return complete_string(completion,ZOK,path,data);
}

ZOOAPI int
zoo_aget_acl(zhandle_t*thiz,const char*path,acl_completion_t completion,const void*data)
{
    struct ACL_vector acl;
    struct Stat stat;
    int rc;
    memset(&acl,0,sizeof acl);
    rc = zoo_get_acl(thiz,path,&acl,&stat);
    rc = complete_acl(completion,rc,&acl,&stat,data);
    deallocate_ACL_vector(&acl);
    return rc;
}

ZOOAPI int
zoo_aset_acl(zhandle_t*thiz,const char*path,int version,struct ACL_vector*acl,void_completion_t completion,const void*data)
{
    int rc=zoo_set_acl(thiz,path,version,acl);
    return complete_void(completion,rc,data);
}

ZOOAPI int
zoo_add_auth(zhandle_t*thiz,const char*scheme,const char*cert,int certLen,void_completion_t completion,const void*data)
{
    return complete_void(completion,ZOK,data);
}
