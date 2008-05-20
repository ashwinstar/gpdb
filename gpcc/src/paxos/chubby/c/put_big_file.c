/* put_big_file.c */

#include "chubby_private.h"

struct data{
    chubby_connection_t*c;
    void(*callback)(void*data,int e);
    void*data;
    chubby_string_t*contents;
    int offset;
    char name[PATH_MAX];
};

static void
freeData(struct data*data)
{
    chubby_t*chubby=NULL;
    chubby_counted_set(&chubby,data->c->chubby);
    chubby_counted_set(&data->c,NULL);
    if (data->contents)
	chubby->free(data->contents);
    chubby->free(data);
    chubby_counted_set(&chubby,NULL);
}

static void
allDone(struct data*data,int e)
{
    data->callback(data->data,e);
    freeData(data);
}

static int propose(struct data*data);

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    data->c->chubby->free(answer);
    if (!e && data->offset<data->contents->length){
	e = propose(data);
	if (!e)
	    return;
    }
    allDone(data,e);
}

static int
propose(struct data*data)
{
    int len;
    chubby_udp_buffer_t b;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Put ");
    chubby_fixFilename(&b,data->name);
    chubby_udp_buffer_format(&b," %d ",data->offset);
    len = data->contents->length-data->offset;
    if (len > CHUBBY_CHUNK_SIZE)
	len = CHUBBY_CHUNK_SIZE;
    if (len > sizeof b.a-b.length)
	len = sizeof b.a-b.length;
    memcpy(b.a+b.length,data->contents->a+data->offset,len);
    b.length += len;
    data->offset += len;
    return chubby_connection_asynchronous_propose(data->c,proposeDone,data,&b);
}

int
chubby_connection_asynchronous_putBigFile(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name,chubby_string_t*contents)
{
    int e;
    struct data*data;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = thiz->chubby->malloc(sizeof*data);
    if (!data)
	return CHUBBY_E_OOM;
    memset(data,0,sizeof*data);
    chubby_counted_set(&data->c,thiz);
    data->callback = callback;
    data->data = dat;
    data->contents = chubby_string_copy(data->c->chubby,contents);
    if (!data->contents){
	freeData(data);
	return CHUBBY_E_OOM;
    }
    strcpy(data->name,name);
    e = propose(data);
    if (e)
	freeData(data);
    return e;
}

int
chubby_connection_putBigFile(chubby_connection_t*thiz,const char*name,chubby_string_t*contents)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_putBigFile(thiz,chubby_two_int_callback,&data,name,contents));
}
