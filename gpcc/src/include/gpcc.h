#ifndef GPCC_H
#define GPCC_H

#include <gpcc.h>
#include <apr_general.h>
#include <apr.h>

#define FLINE __FILE__ ":" APR_STRINGIFY(__LINE__)

APR_DECLARE(int) gpcc_fatal(const char* fline);
APR_DECLARE(int) gpcc_warning(const char* fline, const char* fmt, ...);
APR_DECLARE(int) gpcc_log_info(const char* fmt, ...);
APR_DECLARE(int) gpcc_log_error(const char* fmt, ...);


#define GPCC_DO(x)  (0 != (x) ? 0 : gpcc_fatal(FLINE))

#define gpcc_lengthof(x)   (sizeof(x) / sizeof((x)[0]))


#endif /* GPCC_H */
