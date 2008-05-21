/* put_big_file.c */

#include "chubby_private.h"

struct data{
    struct super_data super;
    chubby_string_t*contents;
    int offset;
    char name[PATH_MAX];
};

static void
freeData(void*_data)
{
    struct data*data=_data;
    if (data->contents)
	data->super.c->chubby->free(data->contents);
}

static int propose(struct data*data);

static void
proposeDone(void*_data,int e,chubby_string_t*answer)
{
    struct data*data=_data;
    data->super.c->chubby->free(answer);
    if (!e && data->offset<data->contents->length){
	e = propose(data);
	if (!e)
	    return;
    }
    super_data_done_nothing(&data->super,e);
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
    return chubby_connection_asynchronous_propose(data->super.c,proposeDone,data,&b);
}

int
chubby_connection_asynchronous_putBigFile(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name,chubby_string_t*contents)
{
    int e;
    struct data*data;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,freeData);
    data->contents = chubby_string_copy(data->super.c->chubby,contents);
    if (!data->contents){
	super_data_delete(&data->super);
	return CHUBBY_E_OOM;
    }
    strcpy(data->name,name);
    e = propose(data);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_putBigFile(chubby_connection_t*thiz,const char*name,chubby_string_t*contents)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_putBigFile(thiz,chubby_two_int_callback,&data,name,contents));
}
