/* bfz.c */

#include <sys/stat.h>
#include "storage/bfz.h"
#include "miscadmin.h"

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

bfz_t*
bfz_create(bfz_t*thiz,const char*filePrefix,int compress)
{
    char name[MAXPGPATH];
    sprintf(name,"%s/"PG_TEMP_FILES_DIR,DatabasePath);
    mkdir(name,0777);
    sprintf(name,"%s/"PG_TEMP_FILES_DIR"/%sXXXXXX",DatabasePath,filePrefix);
    thiz->fd = mkstemp(name);
    if (thiz->fd == -1)
	elog(ERROR,"bfz: mkstemp failed");
    if (unlink(name))
	elog(ERROR,"bfz: unlink failed");
    thiz->mode = BFZ_MODE_APPEND;
    thiz->tot_bytes = 0;
    thiz->buffer_pointer = thiz->buffer;
    thiz->buffer_end = thiz->buffer+sizeof thiz->buffer;
    compression_algorithms[compress].init(thiz);
    return thiz;
}

void
bfz_close(bfz_t*thiz)
{
    if (thiz->mode == BFZ_MODE_CLOSED)
	return;
    thiz->close_ex(thiz);
    close(thiz->fd);
    thiz->mode = BFZ_MODE_CLOSED;
}

void
bfz_scan_begin(bfz_t*thiz)
{
    Assert(thiz->mode == BFZ_MODE_APPEND);
    thiz->tot_bytes += thiz->buffer_pointer-thiz->buffer;
    thiz->write_ex(thiz,thiz->buffer,thiz->buffer_pointer-thiz->buffer);
    thiz->buffer_pointer = thiz->buffer;
    thiz->buffer_end = thiz->buffer;
    thiz->mode = BFZ_MODE_SCAN;
    thiz->scan_begin_ex(thiz);
    elog(DEBUG5, "bfz file size uncompressed %lld, compressed %lld, savings %d%%", (long long)thiz->tot_bytes, (long long)thiz->tot_compressed, 
		 thiz->tot_bytes == 0 ? 0 : 
		 (int) ((thiz->tot_bytes - thiz->tot_compressed) * 100 / thiz->tot_bytes));
}

void
bfz_write_ex(bfz_t*thiz,const char*buffer,int size)
{
    while (size)
    {
	int s=thiz->buffer_end-thiz->buffer_pointer;
	if (size < s)
	    s = size;
	memcpy(thiz->buffer_pointer,buffer,s);
	thiz->buffer_pointer += s;
	buffer += s;
	size -= s;
	if (thiz->buffer_pointer == thiz->buffer_end)
	{
	    thiz->tot_bytes += sizeof thiz->buffer;
	    thiz->write_ex(thiz,thiz->buffer,sizeof thiz->buffer);
	    thiz->buffer_pointer = thiz->buffer;
	}
    }
}

int
bfz_read_ex(bfz_t*thiz,char*buffer,int size)
{
    int orig_size=size;
    while (size)
    {
	int s=thiz->buffer_end-thiz->buffer_pointer;
	if (size < s)
	    s = size;
	memcpy(buffer,thiz->buffer_pointer,s);
	thiz->buffer_pointer += s;
	buffer += s;
	size -= s;
	if (thiz->buffer_pointer == thiz->buffer_end)
	{
	    s = thiz->read_ex(thiz,thiz->buffer,sizeof thiz->buffer);
	    thiz->buffer_pointer = thiz->buffer;
	    thiz->buffer_end = thiz->buffer+s;
	    if (s == 0)
		break;
	}
    }
    return orig_size-size;
}
