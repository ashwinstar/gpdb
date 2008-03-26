#ifndef GPCC_FILE_H
#define GPCC_FILE_H

#include <gpcc.h>

/**
 * file read fully
 * @param fpath file path to read
 * @param buf output buffer
 * @param ap memory pool
 * @return buffer size
 */
APR_DECLARE(size_t) gpcc_file_read_fully(const char *fpath, char **buf, apr_pool_t *ap);

/**
 * file exists
 * @param fpath file to check existence
 * @return 1 if file exists
 */
APR_DECLARE(int) gpcc_file_exists(const char *fpath);

/**
 * directory exists
 * @param dirname directory name to check
 * @return 1 if directory exists
 */
APR_DECLARE(int) gpcc_file_directory_exists(const char *dirname);

/**
 * directory writable
 * @param dirname directory name to check
 * return 1 is directory is writable
 */
APR_DECLARE(int) gpcc_file_directory_writable(const char *dirname);

#endif
