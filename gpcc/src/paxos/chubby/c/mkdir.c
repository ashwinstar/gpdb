/* mkdir.c */

#include "chubby_private.h"

struct data{
    struct super_data super;
    char name[PATH_MAX];
    char ifname[PATH_MAX];
    int64_t dn;
};

static int startMkdirLoop(struct data*data);

static void
statDone2(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if (!e){
	if (data->dn == stat->decreeNumber)
	    e = CHUBBY_E_LEASEEXPIRED;
	else
	    e = startMkdirLoop(data);
    }
    if (e)
	super_data_done_nothing(&data->super,e);
}

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    data->super.c->chubby->free(answer);
    if (e == CHUBBY_E_IFFAILED){
	e = chubby_asynchronous_stat(data->super.c,statDone2,data,data->ifname);
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
    if (stat->decreeNumber){
	super_data_done_nothing(&data->super,CHUBBY_E_EXIST);
	return;
    }
    if (!stat->parentDecreeNumber){
	super_data_done_nothing(&data->super,CHUBBY_E_NOENT);
	return;
    }
    chubby_dirname(data->ifname,data->name);
    data->dn = stat->parentDecreeNumber;
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    chubby_fixFilename(&b,data->ifname);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Mkdir ",data->dn);
    chubby_fixFilename(&b,data->name);
    e = chubby_connection_asynchronous_propose(data->super.c,proposeDone,data,&b);
    if (e)
	super_data_done_nothing(&data->super,e);
}

static int
startMkdirLoop(struct data*data)
{
    return chubby_connection_asynchronous_stat(data->super.c,statDone1,data,data->name);
}

int
chubby_connection_asynchronous_mkdir(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name)
{
    struct data*data;
    int e;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,NULL);
    if (!data)
	return CHUBBY_E_OOM;
    strcpy(data->name,name);
    e = startMkdirLoop(data);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_mkdir(chubby_connection_t*thiz,const char*name)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_mkdir(thiz,chubby_two_int_callback,&data,name));
}

struct makeManyDirsData{
    struct super_data super;
    char name[PATH_MAX];
    int i;
};

static int startMakeManyDirsLoop(struct makeManyDirsData*data);

static void
startMakeManyDirsMkdirDone(void*_data,int e)
{
    struct makeManyDirsData*data=_data;
    if ((e==0 || e==CHUBBY_E_EXIST) && data->i!=-1){
	e = startMakeManyDirsLoop(data);
	if (!e)
	    return;
    }
    super_data_done_nothing(&data->super,e);
}

static int
startMakeManyDirsLoop(struct makeManyDirsData*data)
{
    char name[PATH_MAX],*a;
    strcpy(name,data->name);
    a = strchr(name+data->i+1,'/');
    if (a)
	name[data->i=a-name] = 0;
    else
	data->i = -1;
    return chubby_connection_asynchronous_mkdir(data->super.c,startMakeManyDirsMkdirDone,data,name);
}

int
chubby_connection_asynchronous_nonAtomicMakeManyDirs(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name)
{
    struct makeManyDirsData*data;
    int e;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,NULL);
    if (!data)
	return CHUBBY_E_OOM;
    strcpy(data->name,name);
    e = startMakeManyDirsLoop(data);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_nonAtomicMakeManyDirs(chubby_connection_t*thiz,const char*name)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_nonAtomicMakeManyDirs(thiz,chubby_two_int_callback,&data,name));
}
