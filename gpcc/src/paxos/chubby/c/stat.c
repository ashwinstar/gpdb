/* stat.c */

#include "chubby_private.h"

struct data{
    chubby_t*c;
    void(*callback)(void*data,int e,struct chubby_stat*stat);
    void*data;
};

static void
freeData(struct data*data)
{
    chubby_t*chubby=NULL;
    chubby_counted_set(&chubby,data->c);
    chubby_counted_set(&data->c,NULL);
    chubby->free(data);
    chubby_counted_set(&chubby,NULL);
}

static void
allDone(struct data*data,int e,struct chubby_stat*stat)
{
    data->callback(data->data,e,stat);
    freeData(data);
}

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    if (!e){
	struct chubby_stat stat;
	sscanf(answer->a,"%"APR_INT64_T_FMT" %d",&stat.decreeNumber,&stat.size);
	data->c->free(answer);
	allDone(data,0,&stat);
    }else
	allDone(data,e,NULL);
}

int
chubby_asynchronous_stat(chubby_t*thiz,void(*callback)(void*data,int e,struct chubby_stat*stat),void*dat,const char*name)
{
    int e;
    struct data*data;
    chubby_udp_buffer_t b;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = thiz->malloc(sizeof*data);
    if (!data)
	return CHUBBY_E_OOM;
    memset(data,0,sizeof*data);
    chubby_counted_set(&data->c,thiz);
    data->callback = callback;
    data->data = dat;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Stat ");
    chubby_fixFilename(&b,name);
    e = chubby_asynchronous_propose(thiz,proposeDone,data,(chubby_string_t*)&b);
    if (e)
	freeData(data);
    return e;
}

int
chubby_connection_asynchronous_stat(chubby_connection_t*thiz,void(*callback)(void*data,int e,struct chubby_stat*stat),void*dat,const char*name)
{
    int e;
    struct data*data;
    chubby_udp_buffer_t b;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = thiz->chubby->malloc(sizeof*data);
    if (!data)
	return CHUBBY_E_OOM;
    memset(data,0,sizeof*data);
    chubby_counted_set(&data->c,thiz->chubby);
    data->callback = callback;
    data->data = dat;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Stat ");
    chubby_fixFilename(&b,name);
    e = chubby_connection_asynchronous_propose(thiz,proposeDone,data,&b);
    if (e)
	freeData(data);
    return e;
}

struct stat_struct{
    struct chubby_two_int super;
    struct chubby_stat*stat;
};

static void
stat_callback(void*_data,int e,struct chubby_stat*stat)
{
    struct stat_struct*data=_data;
    data->super.done = 1;
    data->super.e = e;
    if (stat && data->stat)
	*data->stat = *stat;
}

int
chubby_connection_stat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name)
{
    struct stat_struct data;
    memset(&data,0,sizeof data);
    data.stat = stat;
    return chubby_two_int_finish(thiz->chubby,&data.super,chubby_connection_asynchronous_stat(thiz,stat_callback,&data,name));
}
