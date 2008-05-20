/* chubby_string.c */

#include "chubby_private.h"

void
chubby_udp_buffer_format(chubby_udp_buffer_t*thiz,char*format,...)
{
    va_list va;
    va_start(va,format);
    thiz->length += vsnprintf(thiz->a+thiz->length,sizeof thiz->a-thiz->length,format,va);
    if (thiz->length > sizeof thiz->a)
	thiz->length = sizeof thiz->a;
    va_end(va);
}

int
chubby_udp_buffer_isOverflowed(chubby_udp_buffer_t*thiz)
{
    return thiz->length>=sizeof thiz->a;
}

void
chubby_udp_buffer_cat(chubby_udp_buffer_t*thiz,chubby_string_t*a)
{
    int len=sizeof thiz->a-thiz->length;
    if (len > a->length)
	len = a->length;
    memcpy(thiz->a+thiz->length,a->a,len);
    thiz->length += len;
}

chubby_string_t*
chubby_string_format(chubby_t*chubby,const char*format,...)
{
    chubby_string_t*s;
    int len;
    va_list va;
    va_start(va,format);
    len = vsnprintf(0,0,format,va);
    va_end(va);
    s = chubby->malloc(sizeof*s+len);
    if (s){
	s->length = len;
	va_start(va,format);
	vsnprintf(s->a,len+1,format,va);
	va_end(va);
    }
    return s;
}

chubby_string_t*
chubby_string_copy(chubby_t*chubby,chubby_string_t*s)
{
    chubby_string_t*a=chubby->malloc(sizeof*a+s->length);
    if (a){
	a->length = s->length;
	memcpy(a->a,s->a,s->length);
    }
    return a;
}
