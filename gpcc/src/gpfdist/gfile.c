#include <gfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>
#include <apr_fnmatch.h>
#include <apr_strings.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef WIN32
#include <io.h>
#endif

typedef enum gfile_type_t gfile_type_t;
enum gfile_type_t {
    GFILE_TXT,
    GFILE_GZ,
    GFILE_Z,
    GFILE_ZIP,
    GFILE_MAX,
};


struct gfile_t {
    gfile_type_t type;
    int ffd;
    size_t (*read)(gfile_t*, void*, size_t);
    int (*close)(gfile_t*);

    union {
	struct {
	    gzFile fp;
	} gz;
    } u;
};



#define CHUNK (1024 * 16)

/* TEXT */


size_t txt_file_read(gfile_t *fd, void *ptr, size_t size) 
{
    return read(fd->ffd, ptr, size);
}

int txt_file_close(gfile_t *fd)
{
    int e = close(fd->ffd);
    fd->ffd = 0;
    return e;
}

static gfile_t* txt_file_open(const char *fpath, int oflag) 
{
    gfile_t* fd = malloc(sizeof(gfile_t));
    if (! fd) 
	gfatal(FLINE, "out of memory");
    
    memset(fd, 0, sizeof(*fd));
    if (-1 == (fd->ffd = open(fpath, oflag, 0600))) {
	gwarning(FLINE, "unable to open file %s", fpath);
	free(fd);
	return 0;
    }
    fd->type = GFILE_TXT;
    fd->read = txt_file_read;
    fd->close = txt_file_close;
    return fd;
}


/* GZ */

size_t gz_file_read(gfile_t *fd, void *ptr, size_t len)
{
    int n = gzread(fd->u.gz.fp, ptr, len);
    return n;
}

int gz_file_close(gfile_t *fd)
{
    int e =  gzclose(fd->u.gz.fp);
    fd->ffd = 0;
    free(fd);
    return e;
}

static gfile_t* gz_file_open(const char *fpath, int oflag)
{
    gfile_t* fd = malloc(sizeof(gfile_t));
    if (! fd) 
	gfatal(FLINE, "out of memory");
    
    memset(fd, 0, sizeof(*fd));
    if (-1 == (fd->ffd = open(fpath, oflag, 0600))) {
	gwarning(FLINE, "unable to open file %s", fpath);
	free(fd);
	return 0;
    }
    
    fd->u.gz.fp = gzdopen(fd->ffd, "rb");
    if (fd->u.gz.fp == 0) {
	gwarning(FLINE, "unable to open gzfp\n", fpath);
	free(fd);
	return 0;
    }

    fd->type = GFILE_GZ;
    fd->read = gz_file_read;
    fd->close = gz_file_close;
    return fd;
}

static apr_status_t gfile_is_directory(const char *fpath, int *is_dir)
{
  struct stat buf;
  int e;

  if (0 != (e = stat(fpath, &buf))) {
      return e;
  }

  *is_dir = S_ISDIR(buf.st_mode);
  return 0;
}

APR_DECLARE(gfile_t*) gfile_open(const char *fpath, int oflag)
{
    char *s;
    int is_dir = 0;

    if (gfile_is_directory(fpath, &is_dir) != 0) {
	gwarning(FLINE, "stat %s failure", fpath);
	return 0;
    }
    
    if (is_dir) {
	gwarning(FLINE, "%s is a directory", fpath);
	return 0;
    }

    if (0 != (s = strrchr(fpath, '.'))) {
	if (apr_strnatcasecmp(s, ".gz") == 0) {
	    return gz_file_open(fpath, oflag);
	} 
	if (apr_strnatcasecmp(s, ".z") == 0) {
	    gwarning(FLINE, "compression .z file is not supported");
	    return 0;
	}
	if (apr_strnatcasecmp(s, ".zip") == 0) {
	    gwarning(FLINE, "compression zip is not supported");
	    return 0;
	}
    }
    return txt_file_open(fpath, oflag);
}


APR_DECLARE(int) gfile_close(gfile_t* fd)
{
    return fd->close(fd);
}

APR_DECLARE(size_t) gfile_read(gfile_t* fd, void* ptr, int len)
{
    return fd->read(fd, ptr, len);
}
