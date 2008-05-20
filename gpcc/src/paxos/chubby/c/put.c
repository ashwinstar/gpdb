/* put.c */

#include "chubby_private.h"

struct data{
    chubby_connection_t*c;
    void(*callback)(void*data,int e);
    void*data;
    chubby_string_t*contents;
    char name[PATH_MAX];
    int64_t dn;
};

static void
freeData(struct data*data)
{
    chubby_t*chubby=NULL;
    chubby_counted_set(&chubby,data->c->chubby);
    chubby_counted_set(&data->c,NULL);
    if (data->contents)
	chubby->free(data->contents);
    chubby->free(data);
    chubby_counted_set(&chubby,NULL);
}

static void
allDone(struct data*data,int e)
{
    data->callback(data->data,e);
    freeData(data);
}

static void
statDone2(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if ((e==CHUBBY_E_NOENT&&!data->dn) || (e==0&&data->dn==stat->decreeNumber))
	e = CHUBBY_E_LEASEEXPIRED;
    allDone(data,e);
}

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    data->c->chubby->free(answer);
    if (e == CHUBBY_E_IFFAILED){
	e = chubby_asynchronous_stat(data->c->chubby,statDone2,data,data->name);
	if (!e)
	    return;
    }
    allDone(data,e);
}

static void
statDone1(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    chubby_udp_buffer_t b;
    if (e == 0)
	data->dn = stat->decreeNumber;
    else if (e != CHUBBY_E_NOENT){
	allDone(data,e);
	return;
    }
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    chubby_fixFilename(&b,data->name);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Put ",data->dn);
    chubby_fixFilename(&b,data->name);
    chubby_udp_buffer_format(&b," 0 ");
    chubby_udp_buffer_cat(&b,data->contents);
    e = chubby_connection_asynchronous_propose(data->c,proposeDone,data,&b);
    if (e)
	allDone(data,e);
}

int
chubby_connection_asynchronous_put(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name,chubby_string_t*contents)
{
    int e;
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
    data->contents = chubby_string_copy(data->c->chubby,contents);
    if (!data->contents){
	freeData(data);
	return CHUBBY_E_OOM;
    }
    strcpy(data->name,name);
    e = chubby_connection_asynchronous_stat(thiz,statDone1,data,name);
    if (e)
	freeData(data);
    return e;
}

int
chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_string_t*contents)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_put(thiz,chubby_two_int_callback,&data,name,contents));
}
