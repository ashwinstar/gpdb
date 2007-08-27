#ifndef GFILE_H
#define GFILE_H
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <apr_general.h>
#include <apr.h>

#define FLINE __FILE__ ":" APR_STRINGIFY(__LINE__)


APR_DECLARE(int) gfatal(const char* fline, const char* fmt, ...);
APR_DECLARE(int) gwarning(const char* fline, const char* fmt, ...);

typedef struct gfile_t gfile_t;
APR_DECLARE(gfile_t*) gfile_open(const char *fpath, int oflag);
APR_DECLARE(int) gfile_close(gfile_t*);
APR_DECLARE(size_t) gfile_read(gfile_t*, void* ptr, int len);


#endif
