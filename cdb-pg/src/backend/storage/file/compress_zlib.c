/* compress_zlib.c */

#include "storage/bfz.h"
#include <zlib.h>

struct bfz_zlib_freeable_stuff{
    struct bfz_freeable_stuff super;
    gzFile f;
};

/* This file implements bfz compression algorithm "zlib". */

static void
bfz_zlib_close_ex(bfz_t*thiz)
{
    struct bfz_zlib_freeable_stuff*fs=(void*)thiz->freeable_stuff;
    if (fs->f){
	thiz->fd = dup(thiz->fd);
	gzclose(fs->f);
    }
    pfree(fs);
    thiz->freeable_stuff = NULL;
}

static void
gzwrite_fully(gzFile f,const char*buffer,int size)
{
    while (size){
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
    while (size){
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
    struct bfz_zlib_freeable_stuff*fs=(void*)thiz->freeable_stuff;
    gzwrite_fully(fs->f,buffer,size);
}

static int
bfz_zlib_read_ex(bfz_t*thiz,char*buffer,int size)
{
    struct bfz_zlib_freeable_stuff*fs=(void*)thiz->freeable_stuff;
    return gzread_fully(fs->f,buffer,size);
}

void
bfz_zlib_init(bfz_t*thiz)
{
    struct bfz_zlib_freeable_stuff*fs=palloc(sizeof*fs);
    if (!fs)
	elog(ERROR,"bfz: bfz_zlib_init palloc failed");
    thiz->freeable_stuff = &fs->super;
    fs->super.read_ex = bfz_zlib_read_ex;
    fs->super.write_ex = bfz_zlib_write_ex;
    fs->super.close_ex = bfz_zlib_close_ex;
    if (thiz->mode == BFZ_MODE_APPEND)
	fs->f = gzdopen(thiz->fd,"wb1");
    else
	fs->f = gzdopen(thiz->fd,"rb");
    if (!fs->f)
	elog(ERROR,"bfz: gzdopen failed");
}

static void*
bfz_threaded_zlib_compress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    struct bfz_threaded_freeable_stuff*fs=(void*)thiz->freeable_stuff;
    gzFile f=gzdopen(thiz->fd,"wb1");
    int size;
    char buf[BFZ_BUFFER_SIZE];
    if (!f)
	elog(ERROR,"bfz: gzdopen failed");
    while ((size=pipe_read(&fs->pipe,buf,sizeof buf)) != 0)
	gzwrite_fully(f,buf,size);
    thiz->fd = dup(thiz->fd);
    gzclose(f);
    return NULL;
}

static void*
bfz_threaded_zlib_uncompress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    struct bfz_threaded_freeable_stuff*fs=(void*)thiz->freeable_stuff;
    gzFile f=gzdopen(thiz->fd,"rb");
    int size;
    char buf[BFZ_BUFFER_SIZE];
    if (!f)
	elog(ERROR,"bfz: gzdopen failed");
    while ((size=gzread_fully(f,buf,sizeof buf)) != 0)
	pipe_write(&fs->pipe,buf,size);
    thiz->fd = dup(thiz->fd);
    gzclose(f);
    pipe_done_writing(&fs->pipe);
    return NULL;
}

void
bfz_threaded_zlib_init(bfz_t*thiz)
{
    bfz_threaded_init(thiz,bfz_threaded_zlib_compress,bfz_threaded_zlib_uncompress);
}
