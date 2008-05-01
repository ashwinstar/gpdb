/* chubby_string.c */

#include "chubby_private.h"

void
chubby_string_free(chubby_string_t*thiz)
{
    chubby_free(thiz->a);
    chubby_free(thiz);
}

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

chubby_string_t*
chubby_string_format(const char*format,...)
{
    chubby_string_t*s=chubby_malloc(sizeof*s);
    int len;
    va_list va;
    if (!s)
        return NULL;
    va_start(va,format);
    len = vsnprintf(0,0,format,va);
    va_end(va);
    s->a = chubby_malloc(len+1);
    if (!s->a){
	chubby_free(s);
	return NULL;
    }
    s->length = len;
    va_start(va,format);
    vsnprintf(s->a,len+1,format,va);
    va_end(va);
    return s;
}
