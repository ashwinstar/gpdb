/* put.c */

#include "chubby_private.h"

struct data{
    struct super_data super;
    chubby_string_t*contents;
    char name[PATH_MAX];
    int64_t dn;
};

static void
freeData(void*_data)
{
    struct data*data=_data;
    if (data->contents)
	data->super.c->chubby->free(data->contents);
}

static void
statDone2(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if ((e==CHUBBY_E_NOENT&&!data->dn) || (e==0&&data->dn==stat->decreeNumber))
	e = CHUBBY_E_LEASEEXPIRED;
    super_data_done_nothing(&data->super,e);
}

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    data->super.c->chubby->free(answer);
    if (e == CHUBBY_E_IFFAILED){
	e = chubby_asynchronous_stat(data->super.c,statDone2,data,data->name);
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
    if (e == 0)
	data->dn = stat->decreeNumber;
    else if (e != CHUBBY_E_NOENT){
	super_data_done_nothing(&data->super,e);
	return;
    }
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    chubby_fixFilename(&b,data->name);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Put ",data->dn);
    chubby_fixFilename(&b,data->name);
    chubby_udp_buffer_format(&b," 0 ");
    chubby_udp_buffer_cat(&b,data->contents);
    e = chubby_connection_asynchronous_propose(data->super.c,proposeDone,data,&b);
    if (e)
	super_data_done_nothing(&data->super,e);
}

int
chubby_connection_asynchronous_put(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name,chubby_string_t*contents)
{
    int e;
    struct data*data;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,freeData);
    if (!data)
	return CHUBBY_E_OOM;
    data->contents = chubby_string_copy(data->super.c->chubby,contents);
    if (!data->contents){
	super_data_delete(&data->super);
	return CHUBBY_E_OOM;
    }
    strcpy(data->name,name);
    e = chubby_connection_asynchronous_stat(thiz,statDone1,data,name);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_string_t*contents)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_put(thiz,chubby_two_int_callback,&data,name,contents));
}
