#include <fstream/gfile.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef WIN32
#include <io.h>
#else
#define O_BINARY 0
#endif

#define COMPRESSION_BUFFER_SIZE		4096

static void*
bz_alloc(void*a,int b,int c)
{
    return gfile_malloc(b*c);
}

static void
bz_free(void*a,void*b)
{
    gfile_free(b);
}

static ssize_t
read_and_retry(gfile_t*fd,void*ptr,size_t size)
{
    ssize_t i;
    do
	i = read(fd->fd,ptr,size);
    while (i<0 && errno==EINTR);
    if (i > 0)
	fd->compressed_position += i;
    return i;
}

static int
nothing_close(gfile_t*fd)
{
    return 0;
}

struct bzlib_stuff{
    bz_stream s;
    int in_size,out_size,eof;
    char in[COMPRESSION_BUFFER_SIZE];
    char out[COMPRESSION_BUFFER_SIZE];
};

static ssize_t
bz_file_read(gfile_t*fd,void*ptr,size_t len)
{
    struct bzlib_stuff*z=fd->u.bz;
    for (;;){
	int e,s=z->s.next_out-z->out-z->out_size;
	if (s>0 || z->eof){
	    if (s > len)
		s = len;
	    memcpy(ptr,z->out+z->out_size,s);
	    z->out_size += s;
	    return s;
	}
	z->out_size = 0;
	z->s.next_out = z->out;
	while (z->in_size < sizeof z->in){
	    s = read_and_retry(fd,z->in+z->in_size,sizeof z->in-z->in_size);
	    if (s == 0)
		break;
	    if (s < 0)
		return -1;
	    z->in_size += s;
	}
	z->s.avail_in = s = z->in+z->in_size-z->s.next_in;
	z->s.avail_out = sizeof z->out;
	e = BZ2_bzDecompress(&z->s);
	if (e == BZ_STREAM_END)
	    z->eof = 1;
	else if (e)
	    return -1;
	if (z->s.avail_out==sizeof z->out && z->s.avail_in==s)
	    return -1;
	if (z->s.next_in == z->in+z->in_size){
	    z->s.next_in = z->in;
	    z->in_size = 0;
	}
   }
}

static int
bz_file_close(gfile_t*fd)
{
    int e=BZ2_bzDecompressEnd(&fd->u.bz->s);
    gfile_free(fd->u.bz);
    return e;
}

static int
bz_file_open(gfile_t*fd)
{
    if (!(fd->u.bz=gfile_malloc(sizeof*fd->u.bz))){
	gfile_printf_then_putc_newline("Out of memory");
	return 1;
    }
    memset(fd->u.bz,0,sizeof*fd->u.bz);
    fd->u.bz->s.bzalloc = bz_alloc;
    fd->u.bz->s.bzfree = bz_free;
    if (BZ2_bzDecompressInit(&fd->u.bz->s,0,0)){
	gfile_printf_then_putc_newline("BZ2_bzDecompressInit failed");
	return 1;
    }
    fd->u.bz->s.next_out = fd->u.bz->out;
    fd->u.bz->s.next_in = fd->u.bz->in;
    fd->read = bz_file_read;
    fd->close = bz_file_close;
    return 0;
}

/* GZ */

struct zlib_stuff{
    z_stream s;
    int in_size,out_size,eof;
    Byte in[COMPRESSION_BUFFER_SIZE];
    Byte out[COMPRESSION_BUFFER_SIZE];
};

static ssize_t
gz_file_read(gfile_t*fd,void*ptr,size_t len)
{
    struct zlib_stuff*z=fd->u.z;
    for (;;){
	int e,f=0,s=z->s.next_out-z->out-z->out_size;
	if (s>0 || z->eof){
	    if (s > len)
		s = len;
	    memcpy(ptr,z->out+z->out_size,s);
	    z->out_size += s;
	    return s;
	}
	z->out_size = 0;
	z->s.next_out = z->out;
	while (z->in_size < sizeof z->in){
	    s = read_and_retry(fd,z->in+z->in_size,sizeof z->in-z->in_size);
	    if (s == 0){
		if (z->in+z->in_size == z->s.next_in)
		    f = Z_FINISH;
		break;
	    }
	    if (s < 0)
		return -1;
	    z->in_size += s;
	}
	z->s.avail_in = s = z->in+z->in_size-z->s.next_in;
	z->s.avail_out = sizeof z->out;
	e = inflate(&z->s,f);
	if (e == Z_STREAM_END)
	    z->eof = 1;
	else if (e)
	    return -1;
	if (z->s.next_in == z->in+z->in_size){
	    z->s.next_in = z->in;
	    z->in_size = 0;
	}
   }
}

static int
gz_file_close(gfile_t *fd)
{
    int e=inflateEnd(&fd->u.z->s);
    gfile_free(fd->u.z);
    return e;
}

static voidpf
z_alloc(voidpf a,uInt b,uInt c)
{
    return gfile_malloc(b*c);
}

static void
z_free(voidpf a,voidpf b)
{
    gfile_free(b);
}

static int
gz_file_open(gfile_t*fd)
{
    if (!(fd->u.z=gfile_malloc(sizeof*fd->u.z))){
	gfile_printf_then_putc_newline("Out of memory");
	return 1;
    }
    memset(fd->u.z,0,sizeof*fd->u.z);
    fd->u.z->s.zalloc = z_alloc;
    fd->u.z->s.zfree = z_free;
    if (inflateInit2(&fd->u.z->s,31)){
	gfile_printf_then_putc_newline("inflateInit2 failed");
	return 1;
    }
    fd->u.z->s.next_out = fd->u.z->out;
    fd->u.z->s.next_in = fd->u.z->in;
    fd->read = gz_file_read;
    fd->close = gz_file_close;
    return 0;
}

int
gfile_open(gfile_t*fd,const char*fpath,int*response_code,const char**response_string)
{
    const char*s=strrchr(fpath,'.');
    struct stat sta;
    *response_code = 500;
    *response_string = "Internal Server Error";
    if (stat(fpath,&sta)){
	gfile_printf_then_putc_newline("gfile stat %s failure",fpath);
	*response_code = 404;
	*response_string = "Not Found";
	return 1;
    }
    if (S_ISDIR(sta.st_mode)){
	gfile_printf_then_putc_newline("gfile %s is a directory",fpath);
	*response_code = 403;
	*response_string = "Reading a directory is forbidden.";
	return 1;
    }
    memset(fd,0,sizeof*fd);
    fd->compressed_size = sta.st_size;
    do
	fd->fd = open(fpath,O_RDONLY|O_BINARY);
    while (fd->fd<0 && errno==EINTR);
    if (-1 == fd->fd) {
	gfile_printf_then_putc_newline("gfile open failed %s",fpath);
	*response_code = 404;
	*response_string = "Not Found";
	return 1;
    }
    fd->read = read_and_retry;
    fd->close = nothing_close;
    if (s&&strcasecmp(s,".gz")==0)
	return gz_file_open(fd);
    else if (s&&strcasecmp(s,".bz2")==0)
	return bz_file_open(fd);
    else if (s&&strcasecmp(s,".z") == 0)
	gfile_printf_then_putc_newline("gfile compression .z file is not supported");
    else if (s&&strcasecmp(s,".zip") == 0)
	gfile_printf_then_putc_newline("gfile compression zip is not supported");
    else
	return 0;
    *response_code = 415;
    *response_string = "Unsupported File Type";
    return 1;
}

int
gfile_close(gfile_t*fd)
{
    int e=1,i;
    if (fd->close){
	e = fd->close(fd);
	do
	    i = close(fd->fd);
	while (i<0 && errno==EINTR);
	if (e == 0)
	    e = i;
	fd->read = 0;
	fd->close = 0;
    }
    return e;
}

ssize_t
gfile_read(gfile_t*fd,void*ptr,size_t len)
{
    size_t olen=len;
    while (len){
	ssize_t i=fd->read(fd,ptr,len);
	if (i < 0)
	    return i;
	if (i == 0)
	    break;
	ptr = (char*)ptr+i;
	len -= i;
    }
    return olen-len;
}

off_t
gfile_get_compressed_size(gfile_t*fd)
{
    return fd->compressed_size;
}

off_t
gfile_get_compressed_position(gfile_t*fd)
{
    return fd->compressed_position;
}
