/* list.c */

#include "chubby_private.h"

struct data{
    struct super_data super;
    chubby_string_t*string;
};

static void
freeData(void*_data)
{
    struct data*data=_data;
    if (data->string)
	data->super.c->chubby->free(data->string);
}

static void
getDone(void*_data,int e,struct chubby_stat*stat,chubby_string_t*a)
{
    struct data*data=_data;
    char**answer;
    int count=0,i;
    if (e){
	super_data_done_charstarstar(&data->super,e,NULL);
	return;
    }
    data->string = a;
    if (a->length != 0){
	count++;
	for (i=0; i<a->length; i++)
	    if (a->a[i] == ' ')
		count++;
    }
    answer = data->super.c->chubby->malloc(sizeof*answer*(count+1));
    if (!answer){
	super_data_done_charstarstar(&data->super,CHUBBY_E_OOM,NULL);
	return;
    }
    *answer = NULL;
    if (count){
	char*s=data->super.c->chubby->malloc(a->length+1);
	if (!s){
	    data->super.c->chubby->free(answer);
	    super_data_done_charstarstar(&data->super,CHUBBY_E_OOM,NULL);
	    return;
	}
	count = 0;
	for (i=0; i<a->length; i++){
	    answer[count++] = s;
	    for (; i<a->length&&a->a[i]!=' '; i++)
		*s++ = a->a[i]==0?' ':a->a[i];
	    *s++ = 0;
	}
	answer[count] = NULL;
    }
    super_data_done_charstarstar(&data->super,0,answer);
}

int
chubby_connection_asynchronous_list(chubby_connection_t*thiz,void(*callback)(void*data,int e,char**answer),void*dat,const char*name)
{
    int e;
    struct data*data;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,freeData);
    if (!data)
	return CHUBBY_E_OOM;
    e = chubby_connection_asynchronous_get(thiz,getDone,data,name,-1);
    if (e)
	super_data_delete(&data->super);
    return e;
}

struct list_struct{
    struct chubby_two_int super;
    char***answer;
};

static void
list_callback(void*_data,int e,char**answer)
{
    struct list_struct*data=_data;
    data->super.done = 1;
    data->super.e = e;
    *data->answer = answer;
}

int
chubby_connection_list(chubby_connection_t*thiz,char***answer,const char*name)
{
    struct list_struct data;
    memset(&data,0,sizeof data);
    data.answer = answer;
    return chubby_two_int_finish(thiz->chubby,&data.super,chubby_connection_asynchronous_list(thiz,list_callback,&data,name));
}

void
chubby_connection_listFree(chubby_connection_t*thiz,char**answer)
{
    if (answer){
	if (*answer)
	    thiz->chubby->free(*answer);
	thiz->chubby->free(answer);
    }
}
