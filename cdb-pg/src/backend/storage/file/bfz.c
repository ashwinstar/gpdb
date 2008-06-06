/* bfz.c */

#include <sys/stat.h>
#include <unistd.h>
#include <storage/bfz.h>
#include <storage/fd.h>
#include <miscadmin.h>
#include <access/xact.h>

static const struct{
    const char*name[6];
    void(*init)(bfz_t*thiz);
}compression_algorithms[]={
    {{"nothing","false","no","off","0",0},bfz_nothing_init},
    {{"zlib",0},bfz_zlib_init},
    {{"threaded_nothing",0},bfz_threaded_nothing_init},
    {{"threaded_zlib","true","yes","on","1",0},bfz_threaded_zlib_init},
#ifdef WITH_LZO
    {{"lzop",0},bfz_lzop_init},
    {{"threaded_lzop",0},bfz_threaded_lzop_init},
#endif
    {{0}}
};

const char*
bfz_compression_to_string(int compress)
{
    return compression_algorithms[compress].name[0];
}

int
bfz_string_to_compression(const char*string)
{
    int i;
    const char*const*a;
    for (i=0; compression_algorithms[i].name[0]; i++)
	for (a=compression_algorithms[i].name; *a; a++)
	    if (!pg_strcasecmp(*a,string))
		return i;
    return -1;
}

static void
bfz_close_callback(XactEvent event,void*arg)
{
    bfz_close(arg);
}

bfz_t*
bfz_create(bfz_t*thiz,const char*filePrefix,int compress)
{
    char name[MAXPGPATH];
    struct bfz_freeable_stuff*fs;
    memset(thiz,0,sizeof*thiz);
    sprintf(name,"%s/"PG_TEMP_FILES_DIR,DatabasePath);
    mkdir(name,0777);
    sprintf(name,"%s/"PG_TEMP_FILES_DIR"/%sXXXXXX",DatabasePath,filePrefix);
    thiz->fd = mkstemp(name);
    if (thiz->fd == -1)
	elog(ERROR,"bfz: mkstemp failed");
    RegisterXactCallbackOnce(bfz_close_callback,thiz);
    if (unlink(name))
	elog(ERROR,"bfz: unlink failed");
    thiz->mode = BFZ_MODE_APPEND;
    thiz->compression_index = compress;
    compression_algorithms[compress].init(thiz);
    fs = thiz->freeable_stuff;
    fs->tot_bytes = 0;
    fs->buffer_pointer = fs->buffer;
    fs->buffer_end = fs->buffer+sizeof fs->buffer;
    return thiz;
}

void
bfz_close(bfz_t*thiz)
{
    UnregisterXactCallbackOnce(bfz_close_callback,thiz);
    if (thiz->freeable_stuff)
	thiz->freeable_stuff->close_ex(thiz);
    if (thiz->fd != -1){
	close(thiz->fd);
	thiz->fd = -1;
    }
    thiz->mode = BFZ_MODE_CLOSED;
}

void
bfz_append_end(bfz_t*thiz)
{
    struct bfz_freeable_stuff*fs=thiz->freeable_stuff;
    off_t tot_compressed,tot_bytes;
    Assert(thiz->mode == BFZ_MODE_APPEND);
    fs->tot_bytes += fs->buffer_pointer-fs->buffer;
    fs->write_ex(thiz,fs->buffer,fs->buffer_pointer-fs->buffer);
    tot_bytes = fs->tot_bytes;
    fs->close_ex(thiz);
    if ((tot_compressed=lseek(thiz->fd,0,SEEK_END)) == -1)
	elog(ERROR,"bfz: lseek failed");
    elog(DEBUG5,"bfz file size uncompressed %lld, compressed %lld, savings %d%%",(long long)tot_bytes,(long long)tot_compressed,tot_bytes==0?0:(int)((tot_bytes-tot_compressed)*100/tot_bytes));
    thiz->mode = BFZ_MODE_FREED;
}

void
bfz_scan_begin(bfz_t*thiz)
{
    struct bfz_freeable_stuff*fs;
    Assert(thiz->mode == BFZ_MODE_FREED);
    if (lseek(thiz->fd,0,SEEK_SET) == -1)
	elog(ERROR,"bfz: lseek failed");
    thiz->mode = BFZ_MODE_SCAN;
    compression_algorithms[thiz->compression_index].init(thiz);
    fs = thiz->freeable_stuff;
    fs->buffer_pointer = fs->buffer_end = fs->buffer;
}

void
bfz_write_ex(bfz_t*thiz,const char*buffer,int size)
{
    struct bfz_freeable_stuff*fs=thiz->freeable_stuff;
    Assert(thiz->mode == BFZ_MODE_APPEND);
    while (size){
	int s=fs->buffer_end-fs->buffer_pointer;
	if (size < s)
	    s = size;
	memcpy(fs->buffer_pointer,buffer,s);
	fs->buffer_pointer += s;
	buffer += s;
	size -= s;
	if (fs->buffer_pointer == fs->buffer_end){
	    fs->tot_bytes += sizeof fs->buffer;
	    fs->write_ex(thiz,fs->buffer,sizeof fs->buffer);
	    fs->buffer_pointer = fs->buffer;
	}
    }
}

int
bfz_read_ex(bfz_t*thiz,char*buffer,int size)
{
    struct bfz_freeable_stuff*fs=thiz->freeable_stuff;
    int orig_size=size;
    Assert(thiz->mode == BFZ_MODE_SCAN);
    while (size){
	int s=fs->buffer_end-fs->buffer_pointer;
	if (size < s)
	    s = size;
	memcpy(buffer,fs->buffer_pointer,s);
	fs->buffer_pointer += s;
	buffer += s;
	size -= s;
	if (fs->buffer_pointer == fs->buffer_end){
	    s = fs->read_ex(thiz,fs->buffer,sizeof fs->buffer);
	    fs->buffer_pointer = fs->buffer;
	    fs->buffer_end = fs->buffer+s;
	    if (s == 0)
		break;
	}
    }
    return orig_size-size;
}
