/* put.c */

#include "chubby_private.h"

struct data{
    struct super_data super;
    chubby_string_t*contents;
    char name[PATH_MAX];
    char ifname[PATH_MAX];
    int64_t dn;
    int offset;
};

static void
freeData(void*_data)
{
    struct data*data=_data;
    if (data->contents)
	data->super.c->chubby->free(data->contents);
}

static int startPutLoop(struct data*data);

static void
statDone2(void*_data,int e,struct chubby_stat*stat)
{
    struct data*data=_data;
    if (!e){
	if (data->dn == stat->decreeNumber)
	    e = CHUBBY_E_LEASEEXPIRED;
	else if (strcmp(data->name,data->ifname))
	    e = startPutLoop(data);
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
    if (stat->decreeNumber == 0){
	chubby_dirname(data->ifname,data->name);
	data->dn = stat->parentDecreeNumber;
    }else{
	strcpy(data->ifname,data->name);
	data->dn = stat->decreeNumber;
    }
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    chubby_fixFilename(&b,data->ifname);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Put ",data->dn);
    chubby_fixFilename(&b,data->name);
    chubby_udp_buffer_format(&b," %d ",data->offset);
    chubby_udp_buffer_cat(&b,data->contents);
    e = chubby_connection_asynchronous_propose(data->super.c,proposeDone,data,&b);
    if (e)
	super_data_done_nothing(&data->super,e);
}

static int
startPutLoop(struct data*data)
{
    return chubby_connection_asynchronous_stat(data->super.c,statDone1,data,data->name);
}

static int
chubby_connection_asynchronous_putWithOffset(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name,int offset,chubby_string_t*contents)
{
    struct data*data;
    int e;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,freeData);
    if (!data)
	return CHUBBY_E_OOM;
    data->offset = offset;
    data->contents = chubby_string_copy(data->super.c->chubby,contents);
    if (!data->contents){
	super_data_delete(&data->super);
	return CHUBBY_E_OOM;
    }
    strcpy(data->name,name);
    e = startPutLoop(data);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_asynchronous_put(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name,chubby_string_t*contents)
{
    return chubby_connection_asynchronous_putWithOffset(thiz,callback,dat,name,0,contents);
}

int
chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_string_t*contents)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_put(thiz,chubby_two_int_callback,&data,name,contents));
}

struct big_file_data{
    struct super_data super;
    chubby_string_t*contents;
    int offset;
    char name[PATH_MAX];
};

static void
bigFileFreeData(void*_data)
{
    struct big_file_data*data=_data;
    if (data->contents)
	data->super.c->chubby->free(data->contents);
}

static int sendPut(struct big_file_data*data);

static void
putDone(void*_data,int e)
{
    struct big_file_data*data=_data;
    data->offset += CHUBBY_CHUNK_SIZE;
    if (!e && data->offset<data->contents->length){
	e = sendPut(data);
	if (!e)
	    return;
    }
    super_data_done_nothing(&data->super,e);
}

static int
sendPut(struct big_file_data*data)
{
    chubby_udp_buffer_t b;
    int len=data->contents->length-data->offset;
    if (len > CHUBBY_CHUNK_SIZE)
	len = CHUBBY_CHUNK_SIZE;
    memcpy(b.a,data->contents->a+data->offset,len);
    b.length = len;
    return chubby_connection_asynchronous_putWithOffset(data->super.c,putDone,data,data->name,data->offset,(chubby_string_t*)&b);
}

int
chubby_connection_asynchronous_nonAtomicPutBigFile(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*dat,const char*name,chubby_string_t*contents)
{
    int e;
    struct big_file_data*data;
    if (strlen(name) >= PATH_MAX)
	return CHUBBY_E_INVAL;
    data = super_data_new(sizeof*data,thiz,callback,dat,bigFileFreeData);
    data->contents = chubby_string_copy(data->super.c->chubby,contents);
    if (!data->contents){
	super_data_delete(&data->super);
	return CHUBBY_E_OOM;
    }
    strcpy(data->name,name);
    e = sendPut(data);
    if (e)
	super_data_delete(&data->super);
    return e;
}

int
chubby_connection_nonAtomicPutBigFile(chubby_connection_t*thiz,const char*name,chubby_string_t*contents)
{
    struct chubby_two_int data;
    memset(&data,0,sizeof data);
    return chubby_two_int_finish(thiz->chubby,&data,chubby_connection_asynchronous_nonAtomicPutBigFile(thiz,chubby_two_int_callback,&data,name,contents));
}
