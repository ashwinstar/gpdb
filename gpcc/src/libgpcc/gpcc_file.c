#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>
#include <gpcc_file.h>
#include <apr_fnmatch.h>

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif


#define CHUNK 16384

/* TEXT */

apr_status_t txt_file_open(gpcc_file_t *fd, const char *fpath, int oflag, mode_t mode) 
{
    int ret;

    if (-1 == (fd->ffd = open(fpath, oflag, mode))) {
	gpcc_warning(FLINE, "unable to open file %s", fpath);
	return -1;
    }
    fd->type = GPCC_FILE_TXT;
    return APR_SUCCESS;
}

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

/* GZ */
apr_status_t gz_file_open(gpcc_file_t *fd, const char *fpath, int oflag, mode_t mode)
{
    if (-1 == (fd->ffd = open(fpath, oflag, mode))) {
	gpcc_warning(FLINE, "unable to open file %s", fpath);
	return -1;
    }
    
    fd->u.gz.fp = gzdopen(fd->ffd, "rb");
    if (fd->u.gz.fp == 0) {
	gpcc_warning(FLINE, "unable to open gzfp\n", fpath);
	return -1;
    }

    fd->type = GPCC_FILE_GZ;

    return APR_SUCCESS;

}

APR_DECLARE(size_t) gz_file_read(gpcc_file_t *fd, void *ptr, size_t len)
{
    int n = gzread(fd->u.gz.fp, ptr, len);
    return n;
}

APR_DECLARE(int) gz_file_close(gpcc_file_t *fd)
{
    int e =  gzclose(fd->u.gz.fp);
    fd->ffd = 0;
    return e;
}

APR_DECLARE(apr_status_t) gpcc_file_open(gpcc_file_t *fd, const char *fpath, int oflag, mode_t mode)
{
    char *s = strrchr(fpath, '.');
    if (s) {
	if (apr_strnatcasecmp(s, ".gz") == 0) {
	    return gz_file_open(fd, fpath, oflag, mode);
	} 
	else if (apr_strnatcasecmp(s, ".z") == 0) {
	    gpcc_warning(FLINE, "compression .z file is not supported");
	    return -1;
	}
	else if (apr_strnatcasecmp(s, ".zip") == 0) {
	    gpcc_warning(FLINE, "compression zip is not supported");
	    return -1;
	} 
	else {
	    return txt_file_open(fd, fpath, oflag, mode);
	}
    } else {
	return txt_file_open(fd, fpath, oflag, mode);
    }
    
    return APR_SUCCESS;

}


APR_DECLARE(size_t) gpcc_file_read(gpcc_file_t *fd, void *ptr, size_t size)
{
    int n = 0;

    switch (fd->type) {
    case GPCC_FILE_GZ:
	n = gz_file_read(fd, ptr, size);
	break;
    case GPCC_FILE_Z:
	break;
    case GPCC_FILE_ZIP:
	break;
    default:
	n = txt_file_read(fd, ptr, size);
	break;
    }

    return n;
}

APR_DECLARE(int) gpcc_file_close(gpcc_file_t *fd)
{
    switch(fd->type) {
    case GPCC_FILE_GZ:
	return gz_file_close(fd);
    case GPCC_FILE_Z:
	break;
    default:
	return txt_file_close(fd);
	break;
    }
    return 0;
}
