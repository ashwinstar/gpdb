/* exists.c */

#include "header.h"

ZOOAPI int
zoo_exists(zhandle_t*thiz,const char*path,int watch,struct Stat*stat)
{
    int len=0;
    return zoo_get(thiz,path,watch,NULL,&len,stat);
}
