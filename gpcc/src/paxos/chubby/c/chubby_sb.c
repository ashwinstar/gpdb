/* chubby_sb.c */

#include "chubby_private.h"

void
chubby_sb_init(struct chubby_sb*thiz)
{
    thiz->s = 0;
    thiz->length = 0;
    thiz->size = 0;
    thiz->invalid = 0;
}

void
chubby_sb_free(struct chubby_sb*thiz)
{
    if (thiz->s)
	free(thiz->s);
    thiz->s = 0;
    thiz->length = 0;
    thiz->size = 0;
    thiz->invalid = 1;
}

int
chubby_sb_ensureSize(struct chubby_sb*thiz,int size)
{
    if (!thiz->invalid && thiz->size<size){
	char*a;
	thiz->size = thiz->size*3/2+10;
	if (thiz->size < size)
	    thiz->size = size;
	a = malloc(thiz->size);
	if (!a)
	    thiz->invalid = 1;
	else
	    memcpy(a,thiz->s,thiz->length);
	if (thiz->s)
	    free(thiz->s);
	thiz->s = a;
    }
    return thiz->invalid;
}

void
chubby_sb_append_sb(struct chubby_sb*thiz,struct chubby_sb*a)
{
    if (a->invalid)
	chubby_sb_free(thiz);
    if (!chubby_sb_ensureSize(thiz,thiz->length+a->length)){
	    memcpy(thiz->s+thiz->length,a->s,a->length);
	    thiz->length += a->length;
    }
}

void
chubby_sb_append_format(struct chubby_sb*thiz,const char*fmt,...)
{
    int len;
    va_list va;
    va_start(va,fmt);
    len = vsnprintf(0,0,fmt,va);
    va_end(va);
    va_start(va,fmt);
    if (!chubby_sb_ensureSize(thiz,thiz->length+len+1)){
	vsnprintf(thiz->s+thiz->length,len+1,fmt,va);
	thiz->length += len;
    }
    va_end(va);
}

void
chubby_sb_append_c(struct chubby_sb*thiz,char c)
{
    if (!chubby_sb_ensureSize(thiz,thiz->length+1))
	thiz->s[thiz->length++] = c;
}
