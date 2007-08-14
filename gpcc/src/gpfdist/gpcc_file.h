#ifndef GPCC_FILE_H
#define GPCC_FILE_H
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gpcc.h>

typedef struct gpcc_file_t gpcc_file_t;
APR_DECLARE(gpcc_file_t*) gpcc_file_open(const char *fpath, int oflag);
APR_DECLARE(int) gpcc_file_close(gpcc_file_t*);
APR_DECLARE(size_t) gpcc_file_read(gpcc_file_t*, void* ptr, int len);


#endif
