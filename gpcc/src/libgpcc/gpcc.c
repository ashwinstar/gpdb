#include <gpcc.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>


APR_DECLARE(int) gpcc_fatal(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[INTERNAL ERROR] [%s] ", fline);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n          ... exiting\n"); 
    exit(1);
    return 0;
}


APR_DECLARE(int) gpcc_warning(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[WRN] [%s] ", fline); 
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

APR_DECLARE(int) gpcc_log_info(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[INF] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

APR_DECLARE(int) gpcc_log_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[ERR] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

