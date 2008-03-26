/* compress_zlib.c */

#include "storage/bfz.h"
#include <zlib.h>

/* This file implements bfz compression algorithm "zlib". */

static void
bfz_zlib_close_ex(bfz_t*thiz)
{
    if (thiz->u.zlib.f)
	gzclose(thiz->u.zlib.f);
    thiz->fd = -1;
}

static void
gzwrite_fully(gzFile f,const char*buffer,int size)
{
    while (size)
    {
	int i=gzwrite(f,(void*)buffer,size);
	if (i <= 0)
	    elog(ERROR,"gzwrite write failed");
	buffer += i;
	size -= i;
    }
}

static int
gzread_fully(gzFile f,char*buffer,int size)
{
    int orig_size=size;
    while (size)
    {
	int i=gzread(f,buffer,size);
	if (i < 0)
	    elog(ERROR,"gzread read failed");
	if (i == 0)
	    break;
	buffer += i;
	size -= i;
    }
    return orig_size-size;
}

static void
bfz_zlib_write_ex(bfz_t*thiz,const char*buffer,int size)
{
    gzwrite_fully(thiz->u.zlib.f,buffer,size);
}

static int
bfz_zlib_read_ex(bfz_t*thiz,char*buffer,int size)
{
    return gzread_fully(thiz->u.zlib.f,buffer,size);
}

static void
bfz_zlib_scan_begin_ex(bfz_t*thiz)
{
    thiz->fd = dup(thiz->fd);
    gzclose(thiz->u.zlib.f);
    thiz->tot_compressed = lseek(thiz->fd,0,2);
    lseek(thiz->fd,0,0);
    thiz->u.zlib.f = gzdopen(thiz->fd,"rb");
    if (!thiz->u.zlib.f)
	elog(ERROR,"bfz: gzdopen failed");
}

void
bfz_zlib_init(bfz_t*thiz)
{
    thiz->read_ex = bfz_zlib_read_ex;
    thiz->write_ex = bfz_zlib_write_ex;
    thiz->close_ex = bfz_zlib_close_ex;
    thiz->scan_begin_ex = bfz_zlib_scan_begin_ex;
    thiz->u.zlib.f = gzdopen(thiz->fd,"wb1");
    if (!thiz->u.zlib.f)
	elog(ERROR,"bfz: gzdopen failed");
}

static void*
bfz_threaded_zlib_compress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    int size;
    gzFile f=gzdopen(thiz->fd,"wb1");
    char buf[BFZ_BUFFER_SIZE];
    if (!f)
	elog(ERROR,"bfz: gzdopen failed");
    while ((size=pipe_read(&thiz->u.threaded.pipe,buf,sizeof buf)) != 0)
	gzwrite_fully(f,buf,size);
    thiz->fd = dup(thiz->fd);
    gzclose(f);
    return NULL;
}

static void*
bfz_threaded_zlib_uncompress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    int size;
    gzFile f=gzdopen(thiz->fd,"rb");
    char buf[BFZ_BUFFER_SIZE];
    if (!f)
	elog(ERROR,"bfz: gzdopen failed");
    while ((size=gzread_fully(f,buf,sizeof buf)) != 0)
	pipe_write(&thiz->u.threaded.pipe,buf,size);
    gzclose(f);
    thiz->fd = -1;
    pipe_done_writing(&thiz->u.threaded.pipe);
    return NULL;
}

void
bfz_threaded_zlib_init(bfz_t*thiz)
{
    bfz_threaded_init(thiz,bfz_threaded_zlib_compress,bfz_threaded_zlib_uncompress);
}
