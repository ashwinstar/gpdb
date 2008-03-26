/* compress_threaded.c */

#include "storage/bfz.h"

static void
bfz_threaded_close_ex(bfz_t*thiz)
{
    if (thiz->mode == BFZ_MODE_APPEND)
	pipe_done_writing(&thiz->u.threaded.pipe);
    else if (thiz->mode == BFZ_MODE_SCAN)
	while (pipe_read(&thiz->u.threaded.pipe,thiz->buffer,sizeof thiz->buffer) != 0);
    pthread_join(thiz->u.threaded.thread,NULL);
    pipe_close(&thiz->u.threaded.pipe);
}

static void
bfz_threaded_write_ex(bfz_t*thiz,const char*buffer,int size)
{
    pipe_write(&thiz->u.threaded.pipe,buffer,size);
}

static int
bfz_threaded_read_ex(bfz_t*thiz,char*buffer,int size)
{
    return pipe_read(&thiz->u.threaded.pipe,buffer,size);
}

static void
bfz_threaded_scan_begin_ex(bfz_t*thiz)
{
    pipe_done_writing(&thiz->u.threaded.pipe);
    pthread_join(thiz->u.threaded.thread,NULL);
    pipe_close(&thiz->u.threaded.pipe);
    thiz->tot_compressed = lseek(thiz->fd,0,2);
    lseek(thiz->fd,0,SEEK_SET);
    pipe_init(&thiz->u.threaded.pipe);
    if (pthread_create(&thiz->u.threaded.thread,NULL,thiz->u.threaded.uncompress,thiz))
	elog(ERROR,"bfz: pthread_create failed");
}

void
bfz_threaded_init(bfz_t*thiz,void*(*compress)(void*),void*(*uncompress)(void*))
{
    thiz->u.threaded.uncompress = uncompress;
    thiz->read_ex = bfz_threaded_read_ex;
    thiz->write_ex = bfz_threaded_write_ex;
    thiz->close_ex = bfz_threaded_close_ex;
    thiz->scan_begin_ex = bfz_threaded_scan_begin_ex;
    pipe_init(&thiz->u.threaded.pipe);
    if (pthread_create(&thiz->u.threaded.thread,NULL,compress,thiz))
	elog(ERROR,"bfz: pthread_create failed");
}
