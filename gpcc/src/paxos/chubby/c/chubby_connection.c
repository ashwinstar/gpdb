/* chubby_connection.c */

#include "chubby_private.h"

void
chubby_fixFilename(chubby_udp_buffer_t*b,const char*name)
{
    for (; *name; name++)
	if (b->length < sizeof b->a)
	    b->a[b->length++] = *name==' '?0:*name;
}

static void
chubby_connection_delete(struct chubby_counted*_thiz)
{
    chubby_connection_t*thiz=(chubby_connection_t*)_thiz;
    void(*free)(void*)=thiz->chubby->free;
    chubby_counted_set(&thiz->chubby,NULL);
    chubby_counted_set(&thiz->parent,NULL);
    free(thiz);
}

chubby_connection_t*
chubby_connection_new(chubby_t*chubby)
{
    chubby_connection_t*thiz=chubby->malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_connection_delete;
	chubby_counted_set(&thiz->chubby,chubby);
    }
    return thiz;
}

void
chubby_connection_close(chubby_connection_t*thiz)
{
    chubby_counted_close(thiz);
}

static void
reverse(char*a,int length)
{
    char*b=a+length-1;
    while (a < b){
	char tmp=*a;
	*a++ = *b;
	*b-- = tmp;
    }
}

int
chubby_connection_asynchronous_propose(chubby_connection_t*thiz,void(*callback)(void*data,int e,chubby_string_t*answer),void*data,chubby_udp_buffer_t*b)
{
    int length=b->length;
    if (!thiz->parent)
	return chubby_asynchronous_propose(thiz->chubby,callback,data,(chubby_string_t*)b);
    chubby_udp_buffer_format(b,"If ");
    chubby_fixFilename(b,thiz->leaseFilename);
    chubby_udp_buffer_format(b," %"APR_INT64_T_FMT" ",thiz->leaseDn);
    reverse(b->a,length);
    reverse(b->a+length,b->length-length);
    reverse(b->a,b->length);
    return chubby_connection_asynchronous_propose(thiz->parent,callback,data,b);
}

int
chubby_connection_clone(chubby_connection_t*thiz,chubby_connection_t**answer)
{
    chubby_connection_t*c=chubby_connection_new(thiz->chubby);
    if (!c)
	return CHUBBY_E_OOM;
    chubby_counted_set(&c->parent,thiz->parent);
    c->leaseDn = thiz->leaseDn;
    strcpy(c->leaseFilename,thiz->leaseFilename);
    strcpy(c->leaseId,thiz->leaseId);
    c->leaseExpireTime = thiz->leaseExpireTime;
    *answer = c;
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
chubby_connection_checkLease(chubby_connection_t*thiz)
{
    struct chubby_stat stat;
    int e;
    if (!thiz->leaseDn)
	return CHUBBY_E_INVAL;
    e = chubby_connection_stat(thiz,&stat,thiz->leaseFilename);
    return e==0&&stat.decreeNumber==thiz->leaseDn?0:CHUBBY_E_LEASEEXPIRED;
}

int
chubby_connection_getLease(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int timeout,int leaseTime)
{
    int e;
    apr_time_t timedout=apr_time_now()+apr_time_from_sec(timeout);
    apr_time_t expire=0,now;
    int waitTime;
    int64_t dn=0;
    struct chubby_stat stat;
    chubby_string_t*a;
    char id[CHUBBY_ID_SIZE];
    char info[PATH_MAX];
    if (strlen(name) >= PATH_MAX-5)
	return CHUBBY_E_INVAL;
    sprintf(info,"%s-info",name);
    chubby_getNewId(thiz->chubby,id);
    do{
	e = chubby_connection_get(thiz,&stat,&a,info,-1);
	if (!e){
	    sscanf(a->a,"%d",&waitTime);
	    thiz->chubby->free(a);
	}else if (e == CHUBBY_E_NOENT)
	    stat.decreeNumber = 0;
	else
	    return e;
	now = apr_time_now();
	if (stat.decreeNumber > dn){
	    expire = waitTime==0?0:now+apr_time_from_sec(waitTime)+1;
	    dn = stat.decreeNumber;
	}
	if (stat.decreeNumber==0 || expire<now){
	    a = chubby_string_format(thiz->chubby,"%s",id);
	    if (!a)
		return CHUBBY_E_OOM;
	    e = chubby_connection_put(thiz,name,a);
	    thiz->chubby->free(a);
	    if (e)
		return e;
	    e = chubby_connection_get(thiz,&stat,&a,name,-1);
	    if (e)
		return e;
	    if (a->length==strlen(id) && !memcmp(a->a,id,a->length)){
		chubby_connection_t*c=chubby_connection_new(thiz->chubby);
		thiz->chubby->free(a);
		if (!c)
		    return CHUBBY_E_OOM;
		chubby_counted_set(&c->parent,thiz);
		c->leaseDn = stat.decreeNumber;
		strcpy(c->leaseFilename,name);
		strcpy(c->leaseId,id);
		e = chubby_connection_renew(c,leaseTime);
		if (e){
		    chubby_connection_close(c);
		    return CHUBBY_E_LEASEEXPIRED;
		}
		*answer = c;
		return 0;
	    }
	    thiz->chubby->free(a);
	}
	sleep(1);
    }while (timeout>=0 && apr_time_now()<timedout);
    return CHUBBY_E_TIMEOUT;
}

int
chubby_connection_renew(chubby_connection_t*thiz,int leaseTime)
{
    chubby_string_t*a,*b;
    char info[PATH_MAX];
    apr_time_t now=apr_time_now();
    int e;
    if (!*thiz->leaseId)
	return CHUBBY_E_INVAL;
    a = chubby_string_format(thiz->chubby,"%d %s",leaseTime,thiz->leaseId);
    if (!a)
	return CHUBBY_E_OOM;
    sprintf(info,"%s-info",thiz->leaseFilename);
    e = chubby_connection_put(thiz,info,a);
    if (!e){
	e = chubby_connection_get(thiz,NULL,&b,info,-1);
	if (!e){
	    if (a->length==b->length && !memcmp(a->a,b->a,a->length))
		thiz->leaseExpireTime = now+apr_time_from_sec(leaseTime);
	    else
		e = CHUBBY_E_LEASEEXPIRED;
	    thiz->chubby->free(b);
	}
    }
    thiz->chubby->free(a);
    return e;
}

int
chubby_connection_getLeaseExpireTime(chubby_connection_t*thiz,time_t*t)
{
    if (!thiz->leaseDn)
	return CHUBBY_E_INVAL;
    *t = apr_time_sec(thiz->leaseExpireTime);
    return 0;
}

void
chubby_connection_giveUpLease(chubby_connection_t*thiz)
{
    chubby_connection_renew(thiz,0);
}
