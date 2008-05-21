/* lease.c */

#include "chubby_private.h"

static void
checkLeaseStatDone(void*_data,int e,struct chubby_stat*stat)
{
    struct super_data*data=_data;
    if (!e && stat->decreeNumber==data->c->leaseDn)
	e = 0;
    else
	e = CHUBBY_E_LEASEEXPIRED;
    super_data_done_nothing(data,e);
}

int
chubby_connection_asynchronous_checkLease(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat)
{
    int e;
    struct super_data*data;
    if (!thiz->leaseDn)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,NULL);
    if (!data)
	return CHUBBY_E_OOM;
    e = chubby_connection_asynchronous_stat(thiz,checkLeaseStatDone,data,thiz->leaseFilename);
    if (e)
	super_data_delete(data);
    return e;
}

int
chubby_connection_checkLease(chubby_connection_t*thiz)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_checkLease(thiz,chubby_two_int_callback,&data));
}

/*
int
chubby_connection_getLease(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int timeout,int leaseTime)
*/

struct renew_data{
    struct super_data super;
    apr_time_t startTime;
    int leaseTime;
    chubby_string_t*string;
};

static void
renewDataFree(void*_data)
{
    struct renew_data*data=_data;
    if (data->string)
	data->super.c->chubby->free(data->string);
}

static void
renewGetDone(void*_data,int e,struct chubby_stat*stat,chubby_string_t*answer)
{
    struct renew_data*data=_data;
    if (!e){
	if (answer->length==data->string->length && !memcmp(answer->a,data->string->a,answer->length))
	    data->super.c->leaseExpireTime = data->startTime+apr_time_from_sec(data->leaseTime);
	else
	    e = CHUBBY_E_LEASEEXPIRED;
	data->super.c->chubby->free(answer);
    }
    super_data_done_nothing(&data->super,e);
}

static void
renewPutDone(void*_data,int e)
{
    struct renew_data*data=_data;
    char info[PATH_MAX];
    if (!e){
	sprintf(info,"%s-info",data->super.c->leaseFilename);
	e = chubby_connection_asynchronous_get(data->super.c,renewGetDone,data,info,-1);
	if (!e)
	    return;
    }
    super_data_done_nothing(&data->super,e);
}

int
chubby_connection_asynchronous_renew(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,int leaseTime)
{
    int e;
    struct renew_data*data;
    char info[PATH_MAX];
    if (!*thiz->leaseId)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,renewDataFree);
    if (!data)
	return CHUBBY_E_OOM;
    data->startTime = apr_time_now();
    data->leaseTime = leaseTime;
    data->string = chubby_string_format(thiz->chubby,"%d %s",leaseTime,thiz->leaseId);
    if (!data->string){
	super_data_delete(&data->super);
	return CHUBBY_E_OOM;
    }
    sprintf(info,"%s-info",thiz->leaseFilename);
    e = chubby_connection_asynchronous_put(thiz,renewPutDone,data,info,data->string);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_renew(chubby_connection_t*thiz,int leaseTime)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_renew(thiz,chubby_two_int_callback,&data,leaseTime));
}

void
chubby_connection_asynchronous_giveUpLease(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data)
{
    chubby_connection_asynchronous_renew(thiz,callback,data,0);
}

void
chubby_connection_giveUpLease(chubby_connection_t*thiz)
{
    chubby_connection_renew(thiz,0);
}

int
chubby_connection_getLeaseExpireTime(chubby_connection_t*thiz,time_t*t)
{
    if (!thiz->leaseDn)
	return CHUBBY_E_INVAL;
    *t = apr_time_sec(thiz->leaseExpireTime);
    return 0;
}

int
chubby_connection_getLeaseFilenameAndDecreeNumber(chubby_connection_t*thiz,char**filename,int64_t*decreeNumber)
{
    if (!thiz->leaseDn)
	return CHUBBY_E_INVAL;
    if (filename){
	*filename = thiz->chubby->malloc(1+strlen(thiz->leaseFilename));
	if (!*filename)
	    return CHUBBY_E_OOM;
	strcpy(*filename,thiz->leaseFilename);
    }
    if (decreeNumber)
	*decreeNumber = thiz->leaseDn;
    return 0;
}

int
chubby_connection_getLeaseFilenameDecreeNumberConnection(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int64_t decreeNumber)
{
    chubby_connection_t*c;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    c = chubby_connection_new(thiz->chubby);
    if (!c)
	return CHUBBY_E_OOM;
    chubby_counted_set(&c->parent,thiz);
    c->leaseDn = decreeNumber;
    strcpy(c->leaseFilename,name);
    *answer = c;
    return 0;
}
