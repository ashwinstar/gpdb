/* get_lease.c */

#include "chubby_private.h"

struct getLease_data{
    struct super_data super;
    apr_time_t startTime;
    apr_time_t timeout;
    apr_time_t expire;
    int64_t dn;
    int leaseTime;
    chubby_connection_t*c;
    char name[PATH_MAX];
    char id[CHUBBY_ID_SIZE];
};

static void
getLeaseFreeData(void*_data)
{
    struct getLease_data*data=_data;
    if (data->c)
	chubby_connection_close(data->c);
}

static void
getLeaseRenewDone(void*_data,int e)
{
    struct getLease_data*data=_data;
    if (!e){
	chubby_connection_t*c=data->c;
	data->c = NULL;
	super_data_done_connection(&data->super,0,c);
    }else
	super_data_done_connection(&data->super,CHUBBY_E_LEASEEXPIRED,NULL);
}

static void getLeaseGetDone1(void*_data,int e,struct chubby_stat*stat,chubby_string_t*a);

static void
getLeaseSleepDone(void*_data,int e)
{
    struct getLease_data*data=_data;
    if (!e){
	if (data->timeout>=0 && apr_time_now()>=data->timeout)
	    e = CHUBBY_E_TIMEOUT;
	else{
	    char info[PATH_MAX];
	    sprintf(info,"%s-info",data->name);
	    e = chubby_connection_asynchronous_get(data->super.c,getLeaseGetDone1,data,info,-1);
	}
    }
    if (e)
	super_data_done_connection(&data->super,e,NULL);
}

static void
getLeaseGetDone2(void*_data,int e,struct chubby_stat*stat,chubby_string_t*a)
{
    struct getLease_data*data=_data;
    if (!e){
	if (a->length==strlen(data->id) && !memcmp(a->a,data->id,a->length)){
	    data->super.c->chubby->free(a);
	    data->c = chubby_connection_new(data->super.c->chubby);
	    if (!data->c)
		e = CHUBBY_E_OOM;
	    else{
		chubby_counted_set(&data->c->parent,data->super.c);
		data->c->leaseDn = stat->decreeNumber;
		strcpy(data->c->leaseFilename,data->name);
		strcpy(data->c->leaseId,data->id);
		e = chubby_connection_asynchronous_renew(data->c,getLeaseRenewDone,data,data->leaseTime);
	    }
	}else{
	    data->super.c->chubby->free(a);
	    e = chubby_connection_asynchronous_sleep(data->super.c,getLeaseSleepDone,data,1);
	}
    }
    if (e)
	super_data_done_connection(&data->super,e,NULL);
}

static void
getLeasePutDone(void*_data,int e)
{
    struct getLease_data*data=_data;
    if (!e)
	e = chubby_connection_asynchronous_get(data->super.c,getLeaseGetDone2,data,data->name,-1);
    if (e)
	super_data_done_connection(&data->super,e,NULL);
}

static void
getLeaseGetDone1(void*_data,int e,struct chubby_stat*stat,chubby_string_t*a)
{
    struct getLease_data*data=_data;
    int waitTime;
    int64_t decreeNumber;
    apr_time_t now;
    if (!e){
	decreeNumber = stat->decreeNumber;
	sscanf(a->a,"%d",&waitTime);
	data->super.c->chubby->free(a);
    }else if (e == CHUBBY_E_NOENT)
	decreeNumber = 0;
    else{
	super_data_done_connection(&data->super,e,NULL);
	return;
    }
    now = apr_time_now();
    if (decreeNumber > data->dn){
	data->expire = waitTime==0?0:now+apr_time_from_sec(waitTime)+1;
	data->dn = stat->decreeNumber;
    }
    if (decreeNumber==0 || data->expire<now){
	a = chubby_string_format(data->super.c->chubby,"%s",data->id);
	if (!a){
	    super_data_done_connection(&data->super,CHUBBY_E_OOM,NULL);
	    return;
	}
	e = chubby_connection_asynchronous_put(data->super.c,getLeasePutDone,data,data->name,a);
	data->super.c->chubby->free(a);
    }else
	e = chubby_connection_asynchronous_sleep(data->super.c,getLeaseSleepDone,data,1);
    if (e)
	super_data_done_connection(&data->super,e,NULL);
}

int
chubby_connection_asynchronous_getLease(chubby_connection_t*thiz,void(*callback)(void*data,int e,chubby_connection_t*c),void*dat,const char*name,int timeout,int leaseTime)
{
    int e;
    struct getLease_data*data;
    char info[PATH_MAX];
    if (strlen(name) >= PATH_MAX-5)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,getLeaseFreeData);
    if (!data)
	return CHUBBY_E_OOM;
    data->leaseTime = leaseTime;
    strcpy(data->name,name);
    chubby_getNewId(thiz->chubby,data->id);
    sprintf(info,"%s-info",name);
    data->startTime = apr_time_now();
    data->timeout = timeout>=0?data->startTime+apr_time_from_sec(timeout):-1;
    e = chubby_connection_asynchronous_get(thiz,getLeaseGetDone1,data,info,-1);
    if (e)
	super_data_delete(&data->super);
    return e;
}

struct getLease_struct{
    struct chubby_two_int super;
    chubby_connection_t**answer;
};

static void
getLease_callback(void*_data,int e,chubby_connection_t*c)
{
    struct getLease_struct*data=_data;
    data->super.done = 1;
    data->super.e = e;
    *data->answer = c;
}

int
chubby_connection_getLease(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int timeout,int leaseTime)
{
    struct getLease_struct data;
    memset(&data,0,sizeof data);
    data.answer = answer;
    return chubby_two_int_finish(thiz->chubby,&data.super,chubby_connection_asynchronous_getLease(thiz,getLease_callback,&data,name,timeout,leaseTime));
}
