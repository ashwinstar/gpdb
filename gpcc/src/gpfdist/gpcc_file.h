#ifndef GPCC_FILE_H
#define GPCC_FILE_H

#include <gpcc.h>

typedef struct gpcc_file_t gpcc_file_t;
APR_DECLARE(gpcc_file_t*) gpcc_file_open(const char *fpath, int oflag, mode_t mode);
APR_DECLARE(int) gpcc_file_close(gpcc_file_t*);
APR_DECLARE(int) gpcc_file_read(gpcc_file_t*, void* ptr, int len);


#endif
