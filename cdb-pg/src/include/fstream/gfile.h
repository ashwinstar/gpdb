#ifndef GFILE_H
#define GFILE_H

/*#include "c.h"*/
#include <sys/types.h>
#include <bzlib.h>
#include <zlib.h>
#ifdef WIN32
typedef long ssize_t;
#endif

/* The struct gfile_t is private.  Please do not use any of its fields. */
typedef struct gfile_t{
    ssize_t(*read)(struct gfile_t*,void*,size_t);
    int(*close)(struct gfile_t*);
    off_t compressed_size,compressed_position;
    int fd;
    union{
	struct zlib_stuff*z;
	struct bzlib_stuff*bz;
    }u;
}gfile_t;

int gfile_open(gfile_t*fd,const char*fpath,int*response_code,const char**response_string);
int gfile_close(gfile_t*fd);
off_t gfile_get_compressed_size(gfile_t*fd);
off_t gfile_get_compressed_position(gfile_t*fd);
ssize_t gfile_read(gfile_t*fd,void*ptr,size_t len); /* gfile_read reads as much as it can--short read indicates error. */
void gfile_printf_then_putc_newline(const char*format,...);
void*gfile_malloc(size_t size);
void gfile_free(void*a);

#endif
