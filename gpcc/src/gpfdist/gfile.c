#include "gfile.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <bzlib.h>
#include <zlib.h>
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
	BZFILE*bz;
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
	fprintf(stderr,"gfile open failed %s\n",fpath);
	return 1;
    }
    fd->read = txt_file_read;
    fd->close = txt_file_close;
    return 0;
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
bz_file_open(gfile_t*fd,const char*fpath)
{
    if (!(fd->u.bz=BZ2_bzopen(fpath,"rb"))){
	fprintf(stderr,"gfile gzopen failed %s\n",fpath);
	return 1;
    }
    fd->read = bz_file_read;
    fd->close = bz_file_close;
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
	fprintf(stderr,"gfile gzopen failed %s\n",fpath);
	return 1;
    }
    fd->read = gz_file_read;
    fd->close = gz_file_close;
    return 0;
}

gfile_t*
gfile_open(const char*fpath)
{
    const char*s=strrchr(fpath,'.');
    struct stat sta;
    gfile_t*fd;
    int e=1;

    if (stat(fpath,&sta)){
	fprintf(stderr,"gfile stat %s failure\n",fpath);
	return 0;
    }
    if (S_ISDIR(sta.st_mode)){
	fprintf(stderr,"gfile %s is a directory\n",fpath);
	return 0;
    }
    if (!(fd=malloc(sizeof*fd))){
	fprintf(stderr,"gfile out of memory\n");
	return 0;
    }
    memset(fd,0,sizeof*fd);
    if (s&&strcasecmp(s,".gz")==0)
	e = gz_file_open(fd,fpath);
    else if (s&&strcasecmp(s,".bz2")==0)
	e = bz_file_open(fd,fpath);
    else if (s&&strcasecmp(s,".z") == 0)
	fprintf(stderr,"gfile compression .z file is not supported\n");
    else if (s&&strcasecmp(s,".zip") == 0)
	fprintf(stderr,"gfile compression zip is not supported\n");
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
gfile_read(gfile_t*fd,void*ptr,size_t len)
{
    size_t olen=len;
    while (len){
	size_t i=fd->read(fd,ptr,len);
	if (i < 0)
	    return i;
	if (i == 0)
	    break;
	ptr += i;
	len -= i;
    }
    return olen-len;
}
