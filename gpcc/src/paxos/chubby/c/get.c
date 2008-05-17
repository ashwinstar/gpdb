/* get.c */

#include "chubby_private.h"

struct chubby_string_linked_list{
    struct chubby_string_linked_list*next;
    chubby_string_t*s;
};

static void
chubby_connection_freeChubbyStringLinkedList(chubby_connection_t*thiz,struct chubby_string_linked_list*ll)
{
    while (ll){
	struct chubby_string_linked_list*a=ll;
	ll = a->next;
	if (a->s)
	    thiz->chubby->free(a->s);
	thiz->chubby->free(a);
    }
}

int
chubby_connection_get(chubby_connection_t*thiz,struct chubby_stat*stat,chubby_string_t**answer,const char*name,int maxsize)
{
    int64_t dn=0;
    struct chubby_string_linked_list*ll=NULL,*m;
    int size=0;
    int e=0;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    for (;;){
	chubby_udp_buffer_t b;
	int64_t newdn=0;
	int i,n=0,s=0;
	b.length = 0;
	chubby_udp_buffer_format(&b,"Get ");
	chubby_fixFilename(&b,name);
	i = CHUBBY_CHUNK_SIZE;
	if (maxsize>=0 && maxsize-size<i)
	    i = maxsize-size;
	chubby_udp_buffer_format(&b," %d %d",size,i);
	m = thiz->chubby->malloc(sizeof*m);
	if (!m){
	    e = CHUBBY_E_OOM;
	    break;
	}
	m->next = ll;
	m->s = NULL;
	ll = m;
	e = chubby_connection_propose(thiz,&ll->s,&b);
	if (e)
	    break;
	sscanf(ll->s->a,"%"APR_INT64_T_FMT" %d%n",&newdn,&s,&n);
	assert(n);
	if (dn != newdn){
	    dn = newdn;
	    if (size){
		chubby_connection_freeChubbyStringLinkedList(thiz,ll);
		ll = NULL;
		size = 0;
	    }else
		size = ll->s->length-n-1;
	}else
	    size += ll->s->length-n-1;
	if (size==s || (maxsize>=0 && size>=maxsize)){
	    *answer = thiz->chubby->malloc(sizeof**answer+size);
	    if (!*answer){
		e = CHUBBY_E_OOM;
		break;
	    }
	    (*answer)->length = size;
	    (*answer)->a[size] = 0;
	    if (stat){
		stat->decreeNumber = dn;
		stat->size = size;
	    }
	    for (m=ll; m; m=m->next){
		sscanf(m->s->a,"%"APR_INT64_T_FMT" %d%n",&newdn,&s,&n);
		s = m->s->length-n-1;
		size -= s;
		assert(size >= 0);
		memcpy((*answer)->a+size,m->s->a+n+1,s);
	    }
	    assert(size == 0);
	    break;
	}
    }
    chubby_connection_freeChubbyStringLinkedList(thiz,ll);
    return e;
}
