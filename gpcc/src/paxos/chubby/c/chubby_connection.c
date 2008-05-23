/* chubby_connection.c */

#include "chubby_private.h"

static void
chubby_normalizeFilename(char*name)
{
    char*n=name;
    for (;*name=='/';name++);
    for (; *name; name++){
	if (*name=='/'){
	    for (;*name=='/';name++);
	    if (!*name)
		break;
	    --name;
	}
	*n++ = *name;
    }
    *n = 0;
}

void
chubby_fixFilename(chubby_udp_buffer_t*b,const char*name)
{
    char n[PATH_MAX];
    chubby_normalizeFilename(strcpy(n,name));
    for (name=n; *name; name++)
	if (b->length < sizeof b->a)
	    b->a[b->length++] = *name==' '?0:*name;
}

void
chubby_dirname(char*dirname,const char*name)
{
    char*a;
    chubby_normalizeFilename(strcpy(dirname,name));
    a = strrchr(dirname,'/');
    if (!a)
	a = dirname;
    *a = 0;
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

void
super_data_delete(struct super_data*data)
{
     chubby_t*chubby=NULL;
     if (data->free)
 	data->free(data);
     chubby_counted_set(&chubby,data->c->chubby);
     chubby_counted_set(&data->c,NULL);
     chubby->free(data);
     chubby_counted_set(&chubby,NULL);
}

void
super_data_done_nothing(struct super_data*data,int e)
{
    ((void(*)(void*,int))data->callback)(data->data,e);
    super_data_delete(data);
}

void
super_data_done_stat_charstarstar(struct super_data*data,int e,struct chubby_stat*stat,char**charstarstar)
{
    ((void(*)(void*,int,struct chubby_stat*,char**))data->callback)(data->data,e,stat,charstarstar);
    super_data_delete(data);
}

void
super_data_done_stat_string(struct super_data*data,int e,struct chubby_stat*stat,chubby_string_t*string)
{
    ((void(*)(void*,int,struct chubby_stat*,chubby_string_t*))data->callback)(data->data,e,stat,string);
    super_data_delete(data);
}

void
super_data_done_stat(struct super_data*data,int e,struct chubby_stat*stat)
{
    ((void(*)(void*,int,struct chubby_stat*))data->callback)(data->data,e,stat);
    super_data_delete(data);
}

void
super_data_done_connection(struct super_data*data,int e,chubby_connection_t*c)
{
    ((void(*)(void*,int,chubby_connection_t*))data->callback)(data->data,e,c);
    super_data_delete(data);
}

void*
super_data_new(int size,chubby_connection_t*c,void*callback,void*dat,void(*free)(void*data))
{
    struct super_data*data=c->chubby->malloc(size);
    if (data){
	memset(data,0,size);
 	chubby_counted_set(&data->c,c);
	data->callback = callback;
	data->data = dat;
	data->free = free;
    }
    return data;
}

int
chubby_connection_asynchronous_sleep(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,time_t time)
{
    return chubby_asynchronous_sleep(thiz->chubby,callback,data,apr_time_from_sec(time));
}
