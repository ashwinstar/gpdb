#ifndef FSTREAM_H
#define FSTREAM_H

// Eventually we need to get rid of APR.
#include <apr_pools.h>

#define FSTREAM_BLOCKSIZE	(1024*32)

/*  A data block */
typedef struct blockhdr_t blockhdr_t;
struct blockhdr_t {
    char        fname[256];
    int         fname_len;
    apr_int64_t foff;
    char	hbyte[280];
    int         hbot, htop;
};

typedef struct block_t block_t;
struct block_t {
    int i;
    blockhdr_t  hdr;
    int         bot, top;
    char        byte[FSTREAM_BLOCKSIZE - sizeof(blockhdr_t) - sizeof(int)*2 - sizeof(block_t*)];
};

/* A file stream - data may come from several files */
typedef struct fstream_t fstream_t;

struct fstream_options{
    int header;
    int is_csv;
    int verbose;
    char quote;		/* quote char */
    char escape;	/* escape char */
};

struct fstream_filename_and_offset{
    char fname[256];
    apr_int64_t foff;
};

int fstream_read(fstream_t*fs,apr_pool_t*pool,block_t*block);
const char*fstream_get_error(fstream_t*fs);
fstream_t*fstream_open(const char*path,apr_pool_t*pool,const struct fstream_options*options);
void fstream_close(fstream_t*fs);

#endif
