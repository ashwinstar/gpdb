#include "gfile.h"
#include "header.h"
#include <zlib.h>
#include <fcntl.h>
#include <apr_file_io.h>
#ifdef WIN32
#include <io.h>
#else
#define O_BINARY 0
#endif

struct gfile_t{
    size_t(*read)(gfile_t*,void*,size_t);
    int(*close)(gfile_t*);
    union{
	gzFile gz;
	int txt;
    }u;
};

/* TEXT */

static size_t
txt_file_read(gfile_t*fd,void*ptr,size_t size) 
{
    return read(fd->u.txt,ptr,size);
}

static int
txt_file_close(gfile_t*fd)
{
    return close(fd->u.txt);
}

static int
txt_file_open(gfile_t*fd,const char*fpath) 
{
    if (-1 == (fd->u.txt=open(fpath,O_RDONLY|O_BINARY))) {
	gwarning(FLINE,"unable to open file %s",fpath);
	return 1;
    }
    fd->read = txt_file_read;
    fd->close = txt_file_close;
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
gz_file_open(gfile_t*fd,const char*fpath)
{
    if (!(fd->u.gz=gzopen(fpath,"rb"))){
	gwarning(FLINE, "unable to open gzfp\n", fpath);
	return 1;
    }
    fd->read = gz_file_read;
    fd->close = gz_file_close;
    return 0;
}

gfile_t*
gfile_open(const char*fpath,apr_pool_t*pool)
{
    const char*s=strrchr(fpath,'.');
    apr_finfo_t finfo;
    gfile_t*fd;
    int e=1;
    if (apr_stat(&finfo,fpath,APR_FINFO_TYPE,pool)){
	gwarning(FLINE,"stat %s failure",fpath);
	return 0;
    }
    if (finfo.filetype == APR_DIR){
	gwarning(FLINE,"%s is a directory",fpath);
	return 0;
    }
    if (!(fd=malloc(sizeof*fd)))
	gfatal(FLINE,"out of memory");
    memset(fd,0,sizeof*fd);
    if (s&&apr_strnatcasecmp(s,".gz")==0)
	e = gz_file_open(fd,fpath);
    else if (s&&apr_strnatcasecmp(s,".z") == 0)
	gwarning(FLINE,"compression .z file is not supported");
    else if (s&&apr_strnatcasecmp(s,".zip") == 0)
	gwarning(FLINE,"compression zip is not supported");
    else
	e = txt_file_open(fd,fpath);
    if (e){
	free(fd);
	return 0;
    }
    return fd;
}

int
gfile_close(gfile_t*fd)
{
    int e=fd->close(fd);
    free(fd);
    return e;
}

size_t
gfile_read(gfile_t*fd,void*ptr,int len)
{
    int olen=len;
    while (len){
	int i=fd->read(fd,ptr,len);
	if (i < 0)
	    return i;
	if (i == 0)
	    break;
	ptr += i;
	len -= i;
    }
    return olen-len;
}
