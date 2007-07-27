#ifndef GPCC_FILE_H
#define GPCC_FILE_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <gpcc.h>
#include <zlib.h>
typedef enum gpcc_file_type_t gpcc_file_type_t;
enum gpcc_file_type_t {
    GPCC_FILE_TXT,
    GPCC_FILE_GZ,
    GPCC_FILE_Z,
    GPCC_FILE_ZIP,
    GPCC_FILE_MAX,
};

typedef struct gpcc_file_t gpcc_file_t;
struct gpcc_file_t {
    gpcc_file_type_t type;
    int ffd;

    union {
	struct {
	    unsigned char buf[256*1024];
	    int top;
	    int bot;
	    int closed;
	    z_stream strm;
	} z;
	struct {
	    gzFile fp;
	} gz;
    } u;
};

APR_DECLARE(apr_status_t) gpcc_file_open(gpcc_file_t *fd, const char *fpath, int oflag, mode_t mode);

APR_DECLARE(size_t) gpcc_file_read(gpcc_file_t *fd, void *ptr, size_t size);

APR_DECLARE(int) gpcc_file_close(gpcc_file_t *fd);


#endif
