/* compress_lzop.c */

#include <storage/bfz.h>

#ifdef WITH_LZO

#include <unistd.h>
#include <lzo/lzo1x.h>

/* This file implements bfz compression algorithm "lzop". */

static void
bfz_lzop_close_ex(bfz_t*thiz)
{
    free(thiz->freeable_stuff);
    thiz->freeable_stuff = NULL;
}

static int
read_fully(int fd,char*buffer,int size)
{
    char*old=buffer;
    while (size){
	int i=read(fd,buffer,size);
	if (i <= 0)
	    break;
	buffer += i;
	size -= i;
    }
    return buffer-old;
}

static void
write_fully(int fd,char const*buffer,int size)
{
    while (size){
	int i=write(fd,buffer,size);
	if (i <= 0)
	    break;
	buffer += i;
	size -= i;
    }
}

static void
bfz_lzop_write_ex(bfz_t*thiz,const char*buffer,int size)
{
    char b[BFZ_BUFFER_SIZE*17/16+63+3+sizeof(lzo_uint)];
    char work[LZO1X_1_MEM_COMPRESS];
    lzo1x_1_compress((lzo_bytep)buffer,size,(lzo_bytep)b+sizeof(lzo_uint),(lzo_uintp)b,work);
    write_fully(thiz->fd,b,*(lzo_uintp)b+sizeof(lzo_uint));
}

static int
bfz_lzop_read_ex(bfz_t*thiz,char*buffer,int size)
{
    lzo_uint u;
    char b[BFZ_BUFFER_SIZE*17/16+63+3];
    if (read_fully(thiz->fd,(char*)&u,sizeof u) != sizeof u)
	return 0;
    u = read_fully(thiz->fd,b,u);
    lzo1x_decompress((lzo_bytep)b,u,(lzo_bytep)buffer,&u,0);
    return u;
}

void
bfz_lzop_init(bfz_t*thiz)
{
    struct bfz_freeable_stuff*fs=malloc(sizeof*fs);
    if (!fs)
	elog(ERROR,"bfz: bfz_lzop_init malloc failed");
    lzo_init();
    thiz->freeable_stuff = fs;
    fs->read_ex = bfz_lzop_read_ex;
    fs->write_ex = bfz_lzop_write_ex;
    fs->close_ex = bfz_lzop_close_ex;
}

static void*
bfz_threaded_lzop_compress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    struct bfz_threaded_freeable_stuff*fs=(void*)thiz->freeable_stuff;
    int size;
    char buf[BFZ_BUFFER_SIZE];
    lzo_init();
    while ((size=pipe_read(&fs->pipe,buf,sizeof buf)) != 0)
	bfz_lzop_write_ex(thiz,buf,size);
    return NULL;
}

static void*
bfz_threaded_lzop_uncompress(void*arg)
{
    bfz_t*thiz=(bfz_t*)arg;
    struct bfz_threaded_freeable_stuff*fs=(void*)thiz->freeable_stuff;
    int size;
    char buf[BFZ_BUFFER_SIZE];
    while ((size=bfz_lzop_read_ex(thiz,buf,sizeof buf)) != 0)
	pipe_write(&fs->pipe,buf,size);
    pipe_done_writing(&fs->pipe);
    return NULL;
}

void
bfz_threaded_lzop_init(bfz_t*thiz)
{
    bfz_threaded_init(thiz,bfz_threaded_lzop_compress,bfz_threaded_lzop_uncompress);
}

#endif
