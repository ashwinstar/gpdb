#ifndef GFILE_H
#define GFILE_H

#include <sys/types.h>
#include <bzlib.h>
#include <zlib.h>

/* The struct gfile_t is private.  Please do not use any of its fields. */
typedef struct gfile_t{
    size_t(*read)(struct gfile_t*,void*,size_t);
    int(*close)(struct gfile_t*);
    off_t(*get_size)(struct gfile_t*);
    union{
	gzFile gz;
	BZFILE*bz;
	int txt;
    }u;
}gfile_t;

int gfile_open(gfile_t*fd,const char*fpath,int*response_code,const char**response_string);
int gfile_close(gfile_t*fd);
off_t gfile_get_size(gfile_t*fd); // -1 means size is not available.
size_t gfile_read(gfile_t*fd,void*ptr,size_t len); /* gfile_read reads as much as it can--short read indicates error. */
void gfile_printf_then_putc_newline(const char*format,...);

#endif
