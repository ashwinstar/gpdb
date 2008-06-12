/* move.c */

#include "chubby_private.h"

struct data{
    struct super_data super;
    char fromname[PATH_MAX];
    char toname[PATH_MAX];
    int64_t fromdn;
};

static void
statDone2(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if (e==0 && data->fromdn==stat->decreeNumber)
	e = CHUBBY_E_LEASEEXPIRED;
    else if (e == CHUBBY_E_NOENT)
	e = 0;
    super_data_done_nothing(&data->super,e);
}

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    if (!e)
	data->super.c->chubby->free(answer);
    if (e == CHUBBY_E_IFFAILED){
	e = chubby_asynchronous_stat(data->super.c,statDone2,data,data->fromname);
	if (!e)
	    return;
    }
    super_data_done_nothing(&data->super,e);
}

static void
statDone1(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    chubby_udp_buffer_t b;
    if (e){
	super_data_done_nothing(&data->super,e);
	return;
    }
    data->fromdn = stat->decreeNumber;
    if (!data->fromdn){
	super_data_done_nothing(&data->super,CHUBBY_E_NOENT);
	return;
    }
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    chubby_fixFilename(&b,data->fromname);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Move ",data->fromdn);
    chubby_fixFilename(&b,data->fromname);
    chubby_udp_buffer_format(&b," ");
    chubby_fixFilename(&b,data->toname);
    e = chubby_connection_asynchronous_propose(data->super.c,proposeDone,data,&b);
    if (e)
	super_data_done_nothing(&data->super,e);
}

int
chubby_connection_asynchronous_move(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*fromname,const char*toname)
{
    int e;
    struct data*data;
    if (strlen(fromname)>=PATH_MAX || strlen(toname)>=PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,NULL);
    if (!data)
	return CHUBBY_E_OOM;
    strcpy(data->fromname,fromname);
    strcpy(data->toname,toname);
    e = chubby_connection_asynchronous_stat(thiz,statDone1,data,fromname);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_move(chubby_connection_t*thiz,const char*fromname,const char*toname)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_move(thiz,chubby_two_int_callback,&data,fromname,toname));
}
