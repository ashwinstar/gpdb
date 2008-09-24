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

/* TEXT */

static size_t
txt_file_read(gfile_t*fd,void*ptr,size_t size)
{
    size_t i;
    do
	i = read(fd->u.txt,ptr,size);
    while (i<0 && errno==EINTR);
    return i;
}

static int
txt_file_close(gfile_t*fd)
{
    int i;
    do
	i = close(fd->u.txt);
    while (i<0 && errno==EINTR);
    return i;
}

static off_t
txt_file_get_size(gfile_t*fd) 
{
    struct stat s;
    if (fstat(fd->u.txt,&s))
	return -1;
    return s.st_size;
}

static int
txt_file_open(gfile_t*fd,const char*fpath,int*response_code,const char**response_string)
{
    do
	fd->u.txt = open(fpath,O_RDONLY|O_BINARY);
    while (fd->u.txt<0 && errno==EINTR);
    if (-1 == fd->u.txt) {
	gfile_printf_then_putc_newline("gfile open failed %s",fpath);
	*response_code = 404;
	*response_string = "Not Found";
	return 1;
    }
    fd->read = txt_file_read;
    fd->close = txt_file_close;
    fd->get_size = txt_file_get_size;
    return 0;
}

static off_t
not_available_get_size(gfile_t*fd) 
{
    return -1;
}

static size_t
bz_file_read(gfile_t*fd,void*ptr,size_t len)
{
    return BZ2_bzread(fd->u.bz,ptr,len);
}

static int
bz_file_close(gfile_t *fd)
{
    BZ2_bzclose(fd->u.bz);
    return 0;
}

static int
bz_file_open(gfile_t*fd,const char*fpath,int*response_code,const char**response_string)
{
    if (!(fd->u.bz=BZ2_bzopen(fpath,"rb"))){
	gfile_printf_then_putc_newline("gfile gzopen failed %s",fpath);
	*response_code = 404;
	*response_string = "Not Found";
	return 1;
    }
    fd->read = bz_file_read;
    fd->close = bz_file_close;
    fd->get_size = not_available_get_size;
    return 0;
}

/* GZ */

static size_t
gz_file_read(gfile_t*fd,void*ptr,size_t len)
{
    return gzread(fd->u.gz,ptr,len);
}

static int
gz_file_close(gfile_t *fd)
{
    return gzclose(fd->u.gz);
}

static int
gz_file_open(gfile_t*fd,const char*fpath,int*response_code,const char**response_string)
{
    if (!(fd->u.gz=gzopen(fpath,"rb"))){
	gfile_printf_then_putc_newline("gfile gzopen failed %s",fpath);
	*response_code = 404;
	*response_string = "Not Found";
	return 1;
    }
    fd->read = gz_file_read;
    fd->close = gz_file_close;
    fd->get_size = not_available_get_size;
    return 0;
}

int
gfile_open(gfile_t*fd,const char*fpath,int*response_code,const char**response_string)
{
    const char*s=strrchr(fpath,'.');
    struct stat sta;
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
    if (s&&strcasecmp(s,".gz")==0)
	return gz_file_open(fd,fpath,response_code,response_string);
    else if (s&&strcasecmp(s,".bz2")==0)
	return bz_file_open(fd,fpath,response_code,response_string);
    else if (s&&strcasecmp(s,".z") == 0)
	gfile_printf_then_putc_newline("gfile compression .z file is not supported");
    else if (s&&strcasecmp(s,".zip") == 0)
	gfile_printf_then_putc_newline("gfile compression zip is not supported");
    else
	return txt_file_open(fd,fpath,response_code,response_string);
    *response_code = 415;
    *response_string = "Unsupported File Type";
    return 1;
}

int
gfile_close(gfile_t*fd)
{
    int e=1;
    if (fd->close){
	e = fd->close(fd);
	fd->read = 0;
	fd->close = 0;
    }
    return e;
}

size_t
gfile_read(gfile_t*fd,void*ptr,size_t len)
{
    size_t olen=len;
    while (len){
	size_t i=fd->read(fd,ptr,len);
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
gfile_get_size(gfile_t*fd)
{
    return fd->get_size(fd);
}
