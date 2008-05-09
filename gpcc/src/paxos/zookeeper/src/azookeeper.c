/* azookeeper.c */

#include "header.h"

ZOOAPI int
zoo_add_auth(zhandle_t*thiz,const char*scheme,const char*cert,int certLen,void_completion_t completion,const void*data)
{
    completion(ZOK,data);
    return ZOK;
}
