/* mkdir.c */

#include "chubby_private.h"

int
chubby_connection_asynchronous_mkdir(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name)
{
    return 0;
}

int
chubby_connection_mkdir(chubby_connection_t*thiz,const char*name)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_mkdir(thiz,chubby_two_int_callback,&data,name));
}
