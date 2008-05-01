/* getput.c */

#include "chubby_private.h"

static void
fixFilename(chubby_udp_buffer_t*b,const char*name)
{
    for (; *name; name++)
	if (b->length < sizeof b->aa)
	    b->aa[b->length++] = *name==' '?0:*name;
}

int
chubby_stat(zhandle_t*thiz,struct chubby_stat*stat,const char*name)
{
    chubby_udp_buffer_t b;
    chubby_string_t*answer;
    int e;
    chubby_udp_buffer_init(&b);
    chubby_udp_buffer_format(&b,"Stat ");
    fixFilename(&b,name);
    e = chubby_propose(thiz,&answer,(chubby_string_t*)&b);
    if (e == CHUBBY_E_NOENT){
	stat->decreeNumber = 0;
	stat->size = 0;
	e = 0;
    }else if (!e){
	sscanf(answer->a,"%"APR_INT64_T_FMT" %d",&stat->decreeNumber,&stat->size);
	chubby_string_free(answer);
    }
    return e;
}

int
chubby_putBigFile(zhandle_t*thiz,const char*name,chubby_string_t*contents)
{
    int offset=0,len,e;
    chubby_udp_buffer_t b;
    do{
	chubby_udp_buffer_init(&b);
	chubby_udp_buffer_format(&b,"Put ");
	fixFilename(&b,name);
	chubby_udp_buffer_format(&b," %d ",offset);
	len = contents->length-offset;
	if (len > CHUBBY_CHUNK_SIZE)
	    len = CHUBBY_CHUNK_SIZE;
	for (; len&&b.length<sizeof b.a; --len)
	    b.a[b.length++] = contents->a[offset++];
	e = chubby_propose(thiz,NULL,(chubby_string_t*)&b);
    }while (!e && offset<contents->length);
    return e;
}

int
chubby_put(zhandle_t*thiz,const char*name,chubby_string_t*contents)
{
    chubby_udp_buffer_t b;
    struct chubby_stat stat;
    int e=chubby_stat(thiz,&stat,name);
    if (e)
	return e;
    chubby_udp_buffer_init(&b);
    chubby_udp_buffer_format(&b,"If ");
    fixFilename(&b,name);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Put ",stat.decreeNumber);
    fixFilename(&b,name);
    chubby_udp_buffer_format(&b," 0 ");
    chubby_udp_buffer_cat(&b,contents);
    e = chubby_propose(thiz,NULL,(chubby_string_t*)&b);
    if (e == CHUBBY_E_IFFAILED){
	int64_t dn=stat.decreeNumber;
	e = chubby_stat(thiz,&stat,name);
	if (e==0 && dn==stat.decreeNumber)
	    return CHUBBY_E_IFFAILED;
    }
    return e;
}
