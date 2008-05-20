/* move.c */

#include "chubby_private.h"

struct data{
    chubby_connection_t*c;
    void(*callback)(void*data,int e);
    void*data;
    char fromname[PATH_MAX];
    char toname[PATH_MAX];
    int64_t fromdn;
};

static void
freeData(struct data*data)
{
    chubby_t*chubby=NULL;
    chubby_counted_set(&chubby,data->c->chubby);
    chubby_counted_set(&data->c,NULL);
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
statDone3(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if (e==0 && data->fromdn==stat->decreeNumber)
	e = CHUBBY_E_LEASEEXPIRED;
    else if (e == CHUBBY_E_NOENT)
	e = 0;
    allDone(data,e);
}

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    data->c->chubby->free(answer);
    if (e == CHUBBY_E_IFFAILED){
	e = chubby_asynchronous_stat(data->c->chubby,statDone3,data,data->fromname);
	if (!e)
	    return;
    }
    allDone(data,e);
}

static void
statDone2(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    chubby_udp_buffer_t b;
    int64_t todn=0;
    if (e == 0)
	todn = stat->decreeNumber;
    else if (e != CHUBBY_E_NOENT){
	allDone(data,e);
	return;
    }
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    chubby_fixFilename(&b,data->fromname);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" If ",data->fromdn);
    chubby_fixFilename(&b,data->toname);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Move ",todn);
    chubby_fixFilename(&b,data->fromname);
    chubby_udp_buffer_format(&b," ");
    chubby_fixFilename(&b,data->toname);
    e = chubby_connection_asynchronous_propose(data->c,proposeDone,data,&b);
    if (e)
	allDone(data,e);
}

static void
statDone1(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if (e){
	allDone(data,e);
	return;
    }
    data->fromdn = stat->decreeNumber;
    e = chubby_connection_asynchronous_stat(data->c,statDone2,data,data->toname);
    if (e)
	allDone(data,e);
}

int
chubby_connection_asynchronous_move(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*fromname,const char*toname)
{
    int e;
    struct data*data;
    if (strlen(fromname)>=PATH_MAX || strlen(toname)>=PATH_MAX)
	return CHUBBY_E_INVAL;
    data = thiz->chubby->malloc(sizeof*data);
    if (!data)
	return CHUBBY_E_OOM;
    memset(data,0,sizeof*data);
    chubby_counted_set(&data->c,thiz);
    data->callback = callback;
    data->data = dat;
    strcpy(data->fromname,fromname);
    strcpy(data->toname,toname);
    e = chubby_connection_asynchronous_stat(thiz,statDone1,data,fromname);
    if (e)
	freeData(data);
    return e;
}

int
chubby_connection_move(chubby_connection_t*thiz,const char*fromname,const char*toname)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_move(thiz,chubby_two_int_callback,&data,fromname,toname));
}
