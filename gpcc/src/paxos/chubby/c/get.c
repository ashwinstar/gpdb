/* get.c */

#include "chubby_private.h"

struct chubby_string_linked_list{
    struct chubby_string_linked_list*next;
    chubby_string_t*s;
};

struct data{
    chubby_connection_t*c;
    struct chubby_string_linked_list*ll;
    chubby_asynchronous_callback_t callback;
    void*data;
    char name[PATH_MAX];
    struct chubby_stat*stat;
    chubby_string_t**answer;
    chubby_string_t*string;
    int maxsize;
    int size;
    int64_t dn;
};

static void
freeLinkedList(struct data*data)
{
    while (data->ll){
	struct chubby_string_linked_list*a=data->ll;
	data->ll = a->next;
	if (a->s)
	    data->c->chubby->free(a->s);
	data->c->chubby->free(a);
    }
}

static void
allDone(int e,void*_data)
{
    struct data*data=_data;
    chubby_t*chubby=NULL;
    freeLinkedList(data);
    if (data->string)
	data->c->chubby->free(data->string);
    if (data->callback)
	data->callback(e,data->data);
    chubby_counted_set(&chubby,data->c->chubby);
    chubby_counted_set(&data->c,NULL);
    chubby->free(data);
    chubby_counted_set(&chubby,NULL);
}

static void sendProposal(struct data*data);

static void
proposeDone(int e,void*_data)
{
    struct data*data=_data;
    struct chubby_string_linked_list*m;
    int64_t newdn=0;
    int n=0,s=0;
    if (e){
	allDone(e,data);
	return;
    }
    sscanf(data->string->a,"%"APR_INT64_T_FMT" %d%n",&newdn,&s,&n);
    assert(n);
    m = data->c->chubby->malloc(sizeof*m);
    if (!m){
	allDone(CHUBBY_E_OOM,data);
	return;
    }
    m->next = data->ll;
    m->s = data->string;
    data->ll = m;
    data->string = NULL;
    if (data->dn != newdn){
	data->dn = newdn;
	if (data->size){
	    freeLinkedList(data);
	    data->size = 0;
	}else
	    data->size = data->ll->s->length-n-1;
    }else
	data->size += data->ll->s->length-n-1;
    if (data->size==s || (data->maxsize>=0 && data->size>=data->maxsize)){
	*data->answer = data->c->chubby->malloc(sizeof**data->answer+data->size);
	if (!*data->answer){
	    allDone(CHUBBY_E_OOM,data);
	    return;
	}
	(*data->answer)->length = data->size;
	(*data->answer)->a[data->size] = 0;
	if (data->stat){
	    data->stat->decreeNumber = data->dn;
	    data->stat->size = data->size;
	}
	for (m=data->ll; m; m=m->next){
	    sscanf(m->s->a,"%"APR_INT64_T_FMT" %d%n",&newdn,&s,&n);
	    s = m->s->length-n-1;
	    data->size -= s;
	    assert(data->size >= 0);
	    memcpy((*data->answer)->a+data->size,m->s->a+n+1,s);
	}
	assert(data->size == 0);
	allDone(0,data);
	return;
    }
    sendProposal(data);
}

static void
sendProposal(struct data*data)
{
    chubby_udp_buffer_t b;
    int i,e;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Get ");
    chubby_fixFilename(&b,data->name);
    i = CHUBBY_CHUNK_SIZE;
    if (data->maxsize>=0 && data->maxsize-data->size<i)
	i = data->maxsize-data->size;
    chubby_udp_buffer_format(&b," %d %d",data->size,i);
    e = chubby_connection_asynchronous_propose(data->c,proposeDone,data,&data->string,&b);
    if (e)
	allDone(e,data);
}

int
chubby_connection_asynchronous_get(chubby_connection_t*thiz,chubby_asynchronous_callback_t callback,void*dat,struct chubby_stat*stat,chubby_string_t**answer,const char*name,int maxsize)
{
    struct data*data;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = thiz->chubby->malloc(sizeof*data);
    if (!data)
	return CHUBBY_E_OOM;
    memset(data,0,sizeof*data);
    chubby_counted_set(&data->c,thiz);
    data->callback = callback;
    data->data = dat;
    strcpy(data->name,name);
    data->stat = stat;
    data->answer = answer;
    data->maxsize = maxsize;
    sendProposal(data);
    return 0;
}

int
chubby_connection_get(chubby_connection_t*thiz,struct chubby_stat*stat,chubby_string_t**answer,const char*name,int maxsize)
{
    int a[2]={0};
    return chubby_two_int_finish(thiz->chubby,a,chubby_connection_asynchronous_get(thiz,chubby_two_int_callback,a,stat,answer,name,maxsize));
}
