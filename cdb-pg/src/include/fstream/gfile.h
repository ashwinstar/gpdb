#ifndef GFILE_H
#define GFILE_H

#include <sys/types.h>

typedef struct gfile_t gfile_t;
gfile_t*gfile_open(const char*fpath);
int gfile_close(gfile_t*);
size_t gfile_read(gfile_t*,void*ptr,size_t len); /* gfile_read reads as much as it can--short read indicates error. */
void gfile_printf_then_putc_newline(const char*format,...);
void*gfile_malloc(size_t size);
void gfile_free(void*a);

#endif
