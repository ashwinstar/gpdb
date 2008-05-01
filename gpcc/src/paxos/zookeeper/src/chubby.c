/* chubby.c */

#include "chubby_private.h"

void
chubby_getNewId(zhandle_t*thiz,char*a)
{
    sprintf(a,"%s-%"APR_INT64_T_FMT,thiz->id,++thiz->count);
}
