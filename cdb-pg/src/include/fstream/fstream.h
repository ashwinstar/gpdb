#ifndef FSTREAM_H
#define FSTREAM_H

#include <sys/types.h>

#define FSTREAM_BLOCKSIZE	(1024*32)

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
    int64_t foff;
};

int fstream_read(fstream_t*fs,void*buffer,int size,struct fstream_filename_and_offset*fos,int read_whole_lines);
const char*fstream_get_error(fstream_t*fs);
fstream_t*fstream_open(const char*path,const struct fstream_options*options);
void fstream_close(fstream_t*fs);

#endif
