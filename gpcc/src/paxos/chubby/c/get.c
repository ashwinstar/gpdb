/* get.c */

#include "chubby_private.h"

struct chubby_string_linked_list{
    struct chubby_string_linked_list*next;
    chubby_string_t*s;
};

struct data{
    struct super_data super;
    struct chubby_string_linked_list*ll;
    char name[PATH_MAX];
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
	    data->super.c->chubby->free(a->s);
	data->super.c->chubby->free(a);
    }
}

static void
freeData(void*_data)
{
    struct data*data=_data;
    freeLinkedList(data);
    if (data->string)
	data->super.c->chubby->free(data->string);
}

static int sendProposal(struct data*data);

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    struct chubby_string_linked_list*m;
    int64_t newdn=0,parentDn;
    int n=0,s=0;
    data->string = answer;
    if (e){
	super_data_done_stat_string(&data->super,e,NULL,NULL);
	return;
    }
    sscanf(answer->a,"%"APR_INT64_T_FMT" %d %"APR_INT64_T_FMT"%n",&newdn,&s,&parentDn,&n);
    assert(n);
    m = data->super.c->chubby->malloc(sizeof*m);
    if (!m){
	super_data_done_stat_string(&data->super,CHUBBY_E_OOM,NULL,NULL);
	return;
    }
    m->next = data->ll;
    m->s = answer;
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
	struct chubby_stat stat;
	answer = data->super.c->chubby->malloc(sizeof*answer+data->size);
	if (!answer){
	    super_data_done_stat_string(&data->super,CHUBBY_E_OOM,NULL,NULL);
	    return;
	}
	answer->length = data->size;
	answer->a[data->size] = 0;
	stat.decreeNumber = data->dn;
	stat.size = data->size;
	stat.parentDecreeNumber = parentDn;
	for (m=data->ll; m; m=m->next){
	    sscanf(m->s->a,"%"APR_INT64_T_FMT" %d %"APR_INT64_T_FMT"%n",&newdn,&s,&parentDn,&n);
	    s = m->s->length-n-1;
	    data->size -= s;
	    assert(data->size >= 0);
	    memcpy(answer->a+data->size,m->s->a+n+1,s);
	}
	assert(data->size == 0);
	super_data_done_stat_string(&data->super,0,&stat,answer);
	return;
    }
    e = sendProposal(data);
    if (e)
	super_data_done_stat_string(&data->super,e,NULL,NULL);
}

static int
sendProposal(struct data*data)
{
    chubby_udp_buffer_t b;
    int i;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Get ");
    chubby_fixFilename(&b,data->name);
    i = CHUBBY_CHUNK_SIZE;
    if (data->maxsize>=0 && data->maxsize-data->size<i)
	i = data->maxsize-data->size;
    chubby_udp_buffer_format(&b," %d %d",data->size,i);
    return chubby_connection_asynchronous_propose(data->super.c,proposeDone,data,&b);
}

int
chubby_connection_asynchronous_get(chubby_connection_t*thiz,void(*callback)(void*data,int e,struct chubby_stat*stat,chubby_string_t*answer),void*dat,const char*name,int maxsize)
{
    struct data*data;
    int e;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,freeData);
    if (!data)
	return CHUBBY_E_OOM;
    strcpy(data->name,name);
    data->maxsize = maxsize;
    e = sendProposal(data);
    if (e)
	super_data_delete(&data->super);
    return e;
}

struct get_struct{
    struct chubby_two_int super;
    chubby_connection_t*c;
    struct chubby_stat*stat;
    chubby_string_t**answer;
};

static void
get_callback(void*_data,int e,struct chubby_stat*stat,chubby_string_t*answer)
{
    struct get_struct*data=_data;
    data->super.done = 1;
    data->super.e = e;
    if (stat && data->stat)
	*data->stat = *stat;
    if (data->answer)
	*data->answer = answer;
    else if (answer)
	data->c->chubby->free(answer);
}

int
chubby_connection_get(chubby_connection_t*thiz,struct chubby_stat*stat,chubby_string_t**answer,const char*name,int maxsize)
{
    struct get_struct data;
    memset(&data,0,sizeof data);
    data.c = thiz;
    data.stat = stat;
    data.answer = answer;
    return chubby_two_int_finish(thiz->chubby,&data.super,chubby_connection_asynchronous_get(thiz,get_callback,&data,name,maxsize));
}
