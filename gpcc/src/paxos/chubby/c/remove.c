/* remove.c */

#include "chubby_private.h"

struct data{
    struct super_data super;
    char name[PATH_MAX];
    int64_t dn;
};

static void
statDone2(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if (e==0 && data->dn==stat->decreeNumber)
	e = CHUBBY_E_LEASEEXPIRED;
    super_data_done_nothing(&data->super,e);
}

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    data->super.c->chubby->free(answer);
    if (e == CHUBBY_E_NOENT)
	e = 0;
    else if (e == CHUBBY_E_IFFAILED){
	e = chubby_asynchronous_stat(data->super.c,statDone2,data,data->name);
	if (!e)
	    return;
    }
    super_data_done_nothing(&data->super,e);
}

static void
statDone(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    chubby_udp_buffer_t b;
    if (e){
	super_data_done_nothing(&data->super,e);
	return;
    }
    data->dn = stat->decreeNumber;
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    chubby_fixFilename(&b,data->name);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Remove ",data->dn);
    chubby_fixFilename(&b,data->name);
    e = chubby_connection_asynchronous_propose(data->super.c,proposeDone,data,&b);
    if (e)
	super_data_done_nothing(&data->super,e);
}

int
chubby_connection_asynchronous_remove(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name)
{
    int e;
    struct data*data;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,NULL);
    if (!data)
	return CHUBBY_E_OOM;
    strcpy(data->name,name);
    e = chubby_connection_asynchronous_stat(thiz,statDone,data,name);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_remove(chubby_connection_t*thiz,const char*name)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_remove(thiz,chubby_two_int_callback,&data,name));
}
