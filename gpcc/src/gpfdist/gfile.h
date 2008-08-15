#ifndef GFILE_H
#define GFILE_H

#include <sys/types.h>

typedef struct gfile_t gfile_t;
gfile_t*gfile_open(const char*fpath);
int gfile_close(gfile_t*);
size_t gfile_read(gfile_t*,void*ptr,size_t len); /* gfile_read reads as much as it can--short read indicates error. */

#endif
