#ifndef GPFDIST_GFILE_H
#define GPFDIST_GFILE_H

#include <apr_pools.h>

typedef struct gfile_t gfile_t;
gfile_t*gfile_open(const char*fpath,apr_pool_t*pool);
int gfile_close(gfile_t*);
size_t gfile_read(gfile_t*,void*ptr,int len); /* gfile_read reads as much as it can--short read indicates error. */

#endif
