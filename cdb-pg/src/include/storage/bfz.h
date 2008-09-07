#ifndef BFZ_H
#define BFZ_H

#include <postgres.h>
#include "pipe.h"

#define BFZ_MODE_CLOSED   	0
#define BFZ_MODE_APPEND   	1
#define BFZ_MODE_FREED   	2
#define BFZ_MODE_SCAN     	3

#define BFZ_BUFFER_SIZE		(1<<14)

struct bfz;

struct bfz_freeable_stuff{
    off_t tot_bytes;
    char*buffer_pointer;
    char*buffer_end;
    int(*read_ex)(struct bfz*thiz,char*buffer,int size);
    void(*write_ex)(struct bfz*thiz,const char*buffer,int size);
    void(*close_ex)(struct bfz*thiz);
    char buffer[BFZ_BUFFER_SIZE];
};

struct bfz_threaded_freeable_stuff{
    struct bfz_freeable_stuff super;
    pthread_t thread;
    pipe_t pipe;
};

typedef struct bfz{
    struct bfz_freeable_stuff*freeable_stuff;
    int fd;
    unsigned char mode;
    unsigned char compression_index;
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

extern bfz_t*bfz_create(const char*filePrefix,int compress);
extern void bfz_append_end(bfz_t*thiz);
extern void bfz_scan_begin(bfz_t*thiz);
extern void bfz_close(bfz_t*thiz, bool unreg); 
extern size_t readAndRetry(int fd,void*buffer,size_t size);
extern size_t writeAndRetry(int fd,const void*buffer,size_t size);

static inline off_t
bfz_totalbytes(bfz_t *bfz)
{
    return bfz->freeable_stuff->tot_bytes;
}

static inline void
bfz_append(bfz_t*thiz,const char*buffer,int size)
{
    struct bfz_freeable_stuff*fs=thiz->freeable_stuff;
    Assert(thiz->mode == BFZ_MODE_APPEND);
    if (fs->buffer_pointer+size <= fs->buffer_end){
	memcpy(fs->buffer_pointer,buffer,size);
	fs->buffer_pointer += size;
    }else
	bfz_write_ex(thiz,buffer,size);
}

static inline void*
bfz_scan_peek(bfz_t*thiz,int size)
{
    struct bfz_freeable_stuff*fs=thiz->freeable_stuff;
    Assert(thiz->mode == BFZ_MODE_SCAN);
    if (fs->buffer_pointer+size > fs->buffer_end)
	return NULL;
    fs->buffer_pointer += size;
    return fs->buffer_pointer-size;
}

static inline int
bfz_scan_next(bfz_t*thiz,char*buffer,int size)
{
    struct bfz_freeable_stuff*fs=thiz->freeable_stuff;
    Assert(thiz->mode == BFZ_MODE_SCAN);
    if (fs->buffer_pointer+size <= fs->buffer_end){
	memcpy(buffer,fs->buffer_pointer,size);
	fs->buffer_pointer += size;
	return size;
    }
    return bfz_read_ex(thiz,buffer,size);
}

#endif /* BFZ_H */
