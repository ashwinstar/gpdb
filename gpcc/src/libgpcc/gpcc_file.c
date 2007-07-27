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

/* Compress *.Z */
apr_status_t z_file_open(gpcc_file_t *fd, const char *fpath, int oflag, mode_t mode) 
{
    int ret;

    if (-1 == (fd->ffd = open(fpath, oflag, mode))) {
	gpcc_warning(FLINE, "unable to open file %s", fpath);
	return -1;
    }

    fd->type = GPCC_FILE_GZ;
    fd->u.z.closed = 0;
    fd->u.z.top = fd->u.z.bot = 0;
    fd->u.z.strm.zalloc = Z_NULL;
    fd->u.z.strm.zfree = Z_NULL;
    fd->u.z.strm.opaque = Z_NULL;
    fd->u.z.strm.avail_in = 0;
    fd->u.z.strm.next_in = Z_NULL;
    ret = inflateInit(&fd->u.z.strm);
    if (ret != Z_OK) {
	printf("inflatInit failed");
	return ret;
    }
    return APR_SUCCESS;
}


APR_DECLARE(size_t) z_file_read_ex(gpcc_file_t *fd, void *in, size_t size)
{
    unsigned char out[CHUNK];
    int ret = 0;
    int have = 0;

    printf("z_file_read_ex %d bytes\n", size);
    fd->u.z.strm.avail_in = size;
    fd->u.z.strm.next_in = in;

    for (;;) {
	fd->u.z.strm.avail_out = sizeof(out);
	fd->u.z.strm.next_out = out;
	ret = inflate(&fd->u.z.strm, Z_NO_FLUSH);
	assert(ret != Z_STREAM_ERROR);
	switch (ret) {
	case Z_NEED_DICT:
	    ret = Z_DATA_ERROR;
	case Z_DATA_ERROR:
	case Z_MEM_ERROR:
	    printf("ZLIB ERROR\n");
	    (void) inflateEnd(&fd->u.z.strm);
	    return ret;
	}

	have = CHUNK - fd->u.z.strm.avail_out;
	if (have == 0) {
	    break;
	}

	memcpy(fd->u.z.buf + fd->u.z.top, out, have);
	fd->u.z.top += have;

	printf("one round");
    }

    return ret;
}


APR_DECLARE(size_t) z_file_read(gpcc_file_t *fd, void *ptr, size_t size)
{
    unsigned char in[CHUNK];
    int n;
    int ret;

    n = fd->u.z.top - fd->u.z.bot;
    if (n == 0) {
	if (fd->u.z.closed) {
	    return 0;
	}

	n = read(fd->ffd, in, sizeof(in));
	if (n <= 0) {
	    (void) inflateEnd(&fd->u.z.strm);
	    fd->u.z.closed = 1;
	    return n;
	}

	printf("read %d bytes\n", n);

	fd->u.z.top = fd->u.z.bot = 0;

	printf("start decompressing...\n");
	/* decompress the file */
	ret = z_file_read_ex(fd, in, n);
	if (ret) {
	    printf("ret = %d\n", ret);
	    if (ret == Z_STREAM_END) {
		fd->u.z.closed = 1;
		(void) inflateEnd(&fd->u.z.strm);
	    } else {
		return -1;
	    }
	}

	n = fd->u.z.top - fd->u.z.bot;
    } 

    n = (n >= size) ? size : n;
    memcpy(ptr, fd->u.z.buf + fd->u.z.bot, n);
    fd->u.z.bot += n;

    printf("decompress %d\n", n);
    return n;
}

APR_DECLARE(int) z_file_close(gpcc_file_t *fd)
{
    int e;

    if (fd->u.z.closed == 0) {
	(void) inflateEnd(&fd->u.z.strm);
    }
    e = close(fd->ffd);	
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
	    return z_file_open(fd, fpath, oflag, mode);	
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
	n = z_file_read(fd, ptr, size);
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
	return z_file_close(fd);
	break;
    default:
	return txt_file_close(fd);
	break;
    }
    return 0;
}
