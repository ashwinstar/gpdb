#include <gpcc_file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>
#include <apr_fnmatch.h>
#include <apr_strings.h>
#include <fcntl.h>



typedef enum gpcc_file_type_t gpcc_file_type_t;
enum gpcc_file_type_t {
    GPCC_FILE_TXT,
    GPCC_FILE_GZ,
    GPCC_FILE_Z,
    GPCC_FILE_ZIP,
    GPCC_FILE_MAX,
};


struct gpcc_file_t {
    gpcc_file_type_t type;
    int ffd;
    size_t (*read)(gpcc_file_t*, void*, size_t);
    int (*close)(gpcc_file_t*);

    union {
	struct {
	    gzFile fp;
	} gz;
    } u;
};



#define CHUNK (1024 * 16)

/* TEXT */


APR_DECLARE(size_t) txt_file_read(gpcc_file_t *fd, void *ptr, size_t size) 
{
    return read(fd->ffd, ptr, size);
}

APR_DECLARE(int) txt_file_close(gpcc_file_t *fd)
{
    int e = close(fd->ffd);
    fd->ffd = 0;
    return e;
}

static gpcc_file_t* txt_file_open(const char *fpath, int oflag, mode_t mode) 
{
    gpcc_file_t* fd = malloc(sizeof(gpcc_file_t));
    if (! fd) 
	gpcc_fatal(FLINE, "out of memory");
    
    memset(fd, 0, sizeof(*fd));
    if (-1 == (fd->ffd = open(fpath, oflag, mode))) {
	gpcc_warning(FLINE, "unable to open file %s", fpath);
	free(fd);
	return 0;
    }
    fd->type = GPCC_FILE_TXT;
    fd->read = txt_file_read;
    fd->close = txt_file_close;
    return fd;
}


/* GZ */

APR_DECLARE(size_t) gz_file_read(gpcc_file_t *fd, void *ptr, size_t len)
{
    int n = gzread(fd->u.gz.fp, ptr, len);
    return n;
}

APR_DECLARE(int) gz_file_close(gpcc_file_t *fd)
{
    int e =  gzclose(fd->u.gz.fp);
    fd->ffd = 0;
    free(fd);
    return e;
}

static gpcc_file_t* gz_file_open(const char *fpath, int oflag, mode_t mode)
{
    gpcc_file_t* fd = malloc(sizeof(gpcc_file_t));
    if (! fd) 
	gpcc_fatal(FLINE, "out of memory");
    
    memset(fd, 0, sizeof(*fd));
    if (-1 == (fd->ffd = open(fpath, oflag, mode))) {
	gpcc_warning(FLINE, "unable to open file %s", fpath);
	free(fd);
	return 0;
    }
    
    fd->u.gz.fp = gzdopen(fd->ffd, "rb");
    if (fd->u.gz.fp == 0) {
	gpcc_warning(FLINE, "unable to open gzfp\n", fpath);
	free(fd);
	return 0;
    }

    fd->type = GPCC_FILE_GZ;
    fd->read = gz_file_read;
    fd->close = gz_file_close;
    return fd;
}

APR_DECLARE(gpcc_file_t*) gpcc_file_open(const char *fpath, int oflag, mode_t mode)
{
    char *s;
    if (0 != (s = strrchr(fpath, '.'))) {
	if (apr_strnatcasecmp(s, ".gz") == 0) {
	    return gz_file_open(fpath, oflag, mode);
	} 
	if (apr_strnatcasecmp(s, ".z") == 0) {
	    gpcc_warning(FLINE, "compression .z file is not supported");
	    return 0;
	}
	if (apr_strnatcasecmp(s, ".zip") == 0) {
	    gpcc_warning(FLINE, "compression zip is not supported");
	    return 0;
	}
    }
    return txt_file_open(fpath, oflag, mode);
}


APR_DECLARE(int) gpcc_file_close(gpcc_file_t* fd)
{
    return fd->close(fd);
}

APR_DECLARE(int) gpcc_file_read(gpcc_file_t* fd, void* ptr, int len)
{
    return fd->read(fd, ptr, len);
}
