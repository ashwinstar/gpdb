#include <apr_time.h>
#include <gpcc.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>


extern apr_int64_t gpcc_net_tick;

static apr_int64_t exptime = 0;
static apr_time_exp_t exp = {0};

#define TMLEN  sizeof("2007-10-14 22:25:43")

static char* mktime(char buf[TMLEN])
{
    if (exptime != gpcc_net_tick) {
	exptime = gpcc_net_tick;
	apr_time_exp_lt(&exp, exptime);
    }
    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
	    1900 + exp.tm_year, 1 + exp.tm_mon, exp.tm_mday, 
	    exp.tm_hour, exp.tm_min, exp.tm_sec);
    return buf;
}



APR_DECLARE(int) gpcc_fatal(const char* fline, const char* fmt, ...)
{
    char tmbuf[TMLEN];
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s FATAL] [%s] ", mktime(tmbuf), fline);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n          ... exiting\n"); 
    exit(1);
    return 0;
}


APR_DECLARE(int) gpcc_warning(const char* fline, const char* fmt, ...)
{
    char tmbuf[TMLEN];
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s WRN] [%s] ", mktime(tmbuf), fline); 
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

APR_DECLARE(int) gpcc_log_info(const char* fmt, ...)
{
    char tmbuf[TMLEN];
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s INF] ", mktime(tmbuf));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

APR_DECLARE(int) gpcc_log_error(const char* fmt, ...)
{
    char tmbuf[TMLEN];
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s ERR] ", mktime(tmbuf));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

