#ifndef GPFDIST_GFILE_H
#define GPFDIST_GFILE_H

#if 0

This fstream interface isn't ready yet.

#include <apr.h>

#define BLOCKSIZE	(1024*32)

struct fstream_options{
    int verbose;
    int header;
    int is_csv;
    char quote;
    char escape;
};

struct FilenameAndOffset{
    char fname[256];
    apr_int64_t foff;
};

typedef struct fstream_t fstream_t;

fstream_t*fstream_open(const char*name,apr_pool_t*pool,const struct fstream_optins*options);
int fstream_read(fstream_t*fs,char*buffer,int size,struct FilenameAndOffset*fao); /* size is at least BLOCKSIZE.  fstream_read must return an integral number of rows. */
const char*fstream_ferror(fstream_t*fs);
void fstream_close(fstream_t*fs);
#endif

#endif
