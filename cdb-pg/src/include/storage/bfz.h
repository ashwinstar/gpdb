#ifndef BFZ_H
#define BFZ_H

#include <postgres.h>
#include <zlib.h>
#include "fd.h"
#include "pipe.h"

#define BFZ_MODE_CLOSED   	0
#define BFZ_MODE_APPEND   	1
#define BFZ_MODE_SCAN     	2

#define BFZ_BUFFER_SIZE		(1<<14)

struct bfz_zlib_state
{
    gzFile f;
};

struct bfz_threaded_state
{
    pthread_t thread;
    pipe_t pipe;
    void*(*uncompress)(void*);
};

typedef struct bfz
{
    int fd;
    int mode;
    uint64 tot_bytes;
    uint64 tot_compressed;
    char*buffer_pointer;
    char*buffer_end;
    int(*read_ex)(struct bfz*thiz,char*buffer,int size);
    void(*write_ex)(struct bfz*thiz,const char*buffer,int size);
    void(*close_ex)(struct bfz*thiz);
    void(*scan_begin_ex)(struct bfz*thiz);
    union
    {
	struct bfz_zlib_state zlib;
	struct bfz_threaded_state threaded;
    }u;
    char buffer[BFZ_BUFFER_SIZE];
}bfz_t;

/* These functions are internal to bfz. */
extern void bfz_nothing_init(bfz_t*thiz);
extern void bfz_threaded_nothing_init(bfz_t*thiz);
extern void bfz_zlib_init(bfz_t*thiz);
extern void bfz_threaded_zlib_init(bfz_t*thiz);
extern void bfz_lzop_init(bfz_t*thiz);
extern void bfz_threaded_lzop_init(bfz_t*thiz);
extern void bfz_threaded_init(bfz_t*thiz,void*(*compress)(void*),void*(*uncompress)(void*));
extern void bfz_write_ex(bfz_t*thiz,const char*buffer,int size);
extern int bfz_read_ex(bfz_t*thiz,char*buffer,int size);

/* These functions are interface to bfz. */
extern const char*bfz_compression_to_string(int compress);
extern int bfz_string_to_compression(const char*string);

extern bfz_t*bfz_create(bfz_t*thiz,const char*filePrefix,int compress);
extern void bfz_close(bfz_t*thiz);
extern void bfz_scan_begin(bfz_t*thiz);

static inline uint64 bfz_totalbytes(bfz_t *bfz)
{
	return bfz->tot_bytes;
}

static inline void
bfz_append(bfz_t*thiz,const char*buffer,int size)
{
    Assert(thiz->mode == BFZ_MODE_APPEND);
    if (thiz->buffer_pointer+size <= thiz->buffer_end)
    {
	memcpy(thiz->buffer_pointer,buffer,size);
	thiz->buffer_pointer += size;
    }
    else
	bfz_write_ex(thiz,buffer,size);
}

static inline void*
bfz_scan_peek(bfz_t*thiz,int size)
{
    Assert(thiz->mode == BFZ_MODE_SCAN);
    if (thiz->buffer_pointer+size <= thiz->buffer_end)
    {
	thiz->buffer_pointer += size;
	return thiz->buffer_pointer-size;
    }
    return NULL;
}

static inline int
bfz_scan_next(bfz_t*thiz,char*buffer,int size)
{
    Assert(thiz->mode == BFZ_MODE_SCAN);
    if (thiz->buffer_pointer+size <= thiz->buffer_end)
    {
	memcpy(buffer,thiz->buffer_pointer,size);
	thiz->buffer_pointer += size;
	return size;
    }
    return bfz_read_ex(thiz,buffer,size);
}

#endif /* BFZ_H */
