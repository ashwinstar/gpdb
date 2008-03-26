/* compress_nothing.c */

#include "storage/bfz.h"

/* This file implements bfz compression algorithm "nothing".
We don't compress at all. */

static void
bfz_nothing_close_ex(bfz_t*thiz)
{
}

static void
bfz_nothing_scan_begin_ex(bfz_t*thiz)
{
    thiz->tot_compressed = lseek(thiz->fd,0,2);
    lseek(thiz->fd,0,SEEK_SET);
}

static int
bfz_nothing_read_ex(bfz_t*thiz,char*buffer,int size)
{
    int orig_size=size;
    while (size)
    {
	int i=read(thiz->fd,buffer,size);
	if (i < 0)
	    elog(ERROR,"bfz_nothing_read_ex read failed");
	if (i == 0)
	    break;
	buffer += i;
	size -= i;
    }
    return orig_size-size;
}

static void
bfz_nothing_write_ex(bfz_t*bfz,const char*buffer,int size)
{
    while (size)
    {
	int i=write(bfz->fd,buffer,size);
	if (i <= 0)
	    elog(ERROR,"bfz_nothing_write_ex write failed");
	buffer += i;
	size -= i;
    }
}

void
bfz_nothing_init(bfz_t*thiz)
{
    thiz->read_ex = bfz_nothing_read_ex;
    thiz->write_ex = bfz_nothing_write_ex;
    thiz->close_ex = bfz_nothing_close_ex;
    thiz->scan_begin_ex = bfz_nothing_scan_begin_ex;
}

static void*
bfz_threaded_nothing_compress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    for (;;){
	char buffer[BFZ_BUFFER_SIZE];
	int i=pipe_read(&thiz->u.threaded.pipe,buffer,sizeof buffer);
	if (i == 0)
	    break;
	bfz_nothing_write_ex(thiz,buffer,i);
    }
    return NULL;
}

static void*
bfz_threaded_nothing_uncompress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    for (;;){
	char buffer[BFZ_BUFFER_SIZE];
	int i=read(thiz->fd,buffer,sizeof buffer);
	if (i <= 0)
	    break;
	pipe_write(&thiz->u.threaded.pipe,buffer,i);
    }
    pipe_done_writing(&thiz->u.threaded.pipe);
    return NULL;
}

void
bfz_threaded_nothing_init(bfz_t*thiz)
{
    bfz_threaded_init(thiz,bfz_threaded_nothing_compress,bfz_threaded_nothing_uncompress);
}
