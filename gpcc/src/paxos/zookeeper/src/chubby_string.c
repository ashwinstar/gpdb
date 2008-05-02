/* chubby_string.c */

#include "chubby_private.h"

chubby_udp_buffer_t*
chubby_udp_buffer_init(chubby_udp_buffer_t*thiz)
{
    thiz->length = 0;
    thiz->a = thiz->aa;
    return thiz;
}

void
chubby_udp_buffer_format(chubby_udp_buffer_t*thiz,char*format,...)
{
    va_list va;
    va_start(va,format);
    thiz->length += vsnprintf(thiz->aa+thiz->length,sizeof thiz->aa-thiz->length,format,va);
    if (thiz->length > sizeof thiz->aa)
	thiz->length = sizeof thiz->aa;
    va_end(va);
}

int
chubby_udp_buffer_isOverflowed(chubby_udp_buffer_t*thiz)
{
    return thiz->length>=sizeof thiz->aa;
}

void
chubby_udp_buffer_cat(chubby_udp_buffer_t*thiz,chubby_string_t*a)
{
    int len=sizeof thiz->aa-thiz->length;
    if (len > a->length)
	len = a->length;
    memcpy(thiz->aa+thiz->length,a->a,len);
    thiz->length += len;
}

void
chubby_string_free(chubby_string_t*thiz)
{
    if (thiz->a)
	chubby_free(thiz->a);
    thiz->a = NULL;
    thiz->length = 0;
}

chubby_string_t*
chubby_string_format(chubby_string_t*thiz,const char*format,...)
{
    int len;
    va_list va;
    va_start(va,format);
    len = vsnprintf(0,0,format,va);
    va_end(va);
    thiz->a = chubby_malloc(len+1);
    if (!thiz->a){
	thiz->length = 0;
	return NULL;
    }
    thiz->length = len;
    va_start(va,format);
    vsnprintf(thiz->a,len+1,format,va);
    va_end(va);
    return thiz;
}
