#ifndef GPFDIST_HEADER_H
#define GPFDIST_HEADER_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <apr_general.h>
#include <apr.h>
#include <apr_strings.h>
#include "gfile.h"

#define FLINE		__FILE__":"APR_STRINGIFY(__LINE__)
#define BLOCKSIZE  (1024 * 32)

#ifndef WIN32
#define SOCKET int
#define closesocket(x)   close(x)
#endif

APR_DECLARE(int) gprint(const char* fmt, ...);
APR_DECLARE(int) gfatal(const char* fline, const char* fmt, ...);
APR_DECLARE(int) gwarning(const char* fline, const char* fmt, ...);


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
    blockhdr_t  hdr;
    int         bot, top;
    block_t*    __next;		/* chaining for freelist */
    char        byte[BLOCKSIZE - sizeof(blockhdr_t) - sizeof(int)*2 - sizeof(block_t*)];
};

/* A file stream - data may come from several files */
typedef struct fstream_t fstream_t;
struct fstream_t {
    const char** fpath;		/* array of fpath */
    gfile_t*fd;
    int          fn;		/* #elements in ffd[] and fpath[] */
    int		 fidx;		/* current index in ffd[] */
    apr_uint64_t foff;		/* current offset in ffd[fidx] */
    int          opth;		/* reflect opt.h (skip first line) */
    int          fskip;		/* reflect opt.h (skip first line) */
    block_t*     fblock;	/* residue block */
    char* 	 ferror;	/* error flag */
    int 	 is_csv;	/* is the file csv? */
    char 	 quote;		/* quote char */
    char 	 escape;	/* escape char */
};

void block_put(block_t* b);
block_t*fstream_get_block(fstream_t* fs, apr_pool_t* pool);
fstream_t*fstream_alloc(const char* path, apr_pool_t* pool,int opth);
void fstream_free(fstream_t* fs);

#endif
