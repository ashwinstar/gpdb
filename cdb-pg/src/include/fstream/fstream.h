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
    int64_t line_number; /* Line number of first line in buffer.  Zero means fstream doesn't know the line number. */
    int64_t foff;
};

// If read_whole_lines, then size must be at least FSTREAM_BLOCKSIZE. */
int fstream_read(fstream_t*fs,void*buffer,int size,struct fstream_filename_and_offset*fo,int read_whole_lines);
int fstream_eof(fstream_t*fs);
// fstream_get_size returns -1 when size is not available.
// fstream_get_size returns size including header lines.  So it might be a bit bigger than the number of bytes you can actually read.
int64_t fstream_get_size(fstream_t*fs);
int fstream_rewind(fstream_t*fs);
const char*fstream_get_error(fstream_t*fs);
fstream_t*fstream_open(const char*path,const struct fstream_options*options,int*response_code,const char**response_string);
void fstream_close(fstream_t*fs);
void*fstream_malloc(size_t size);
void fstream_free(void*a);

#endif
