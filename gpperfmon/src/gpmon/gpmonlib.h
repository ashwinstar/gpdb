#ifndef GPMONLIB_H
#define GPMONLIB_H

#undef GP_VERSION
#include "postgres_fe.h"

#include "apr_general.h"
#include "apr_time.h"
#include "gpmon/gpmon.h"

#define BATCH 8

#define GPPERFMON_VERSIONX APR_STRINGIFY(GPPERFMON_VERSION)

#ifndef WIN32
typedef int SOCKET;
#endif

#define GPMON_DBUSER "gpmon"

#define GPMON_PACKET_ERR_LOG_TIME 60

#define FMT64 APR_INT64_T_FMT
#define FLINE __FILE__ ":" APR_STRINGIFY(__LINE__)
#define CHECKMEM(x)  if (x) ; else gpmon_fatal(FLINE, "out of memory")

extern int verbose;
/* TODO: REMOVE */
//extern int very_verbose;
#define TR0(x) gpmon_print x
#define TR1(x) if (verbose == 1) gpmon_print x
#define TR2(x) if (verbose == 2) gpmon_print x

/* Architecture specific limits for metrics */
#if defined(osx104_x86) || defined(osx105_x86) || defined(rhel4_x86_32) || defined(rhel5_x86_32)
	#define GPSMON_METRIC_MAX 0xffffffffUL
#elif defined(rhel4_x86_64) || defined(rhel5_x86_64) || defined(sol10_x86_64) || defined(suse10_x86_64)
	#define GPSMON_METRIC_MAX 0xffffffffffffffffULL
#else
	#define GPSMON_METRIC_MAX 0xffffffffUL
#endif


/* fatal & warning messages */
extern int gpmon_print(const char* fmt, ...);
extern int gpmon_fatal(const char* fline, const char* fmt, ...);
extern int gpmon_fatalx(const char* fline, int e, const char* fmt, ...);
extern int gpmon_warning(const char* fline, const char* fmt, ...);
extern int gpmon_warningx(const char* fline, int e, const char* fmt, ...);

/* convert packets to host order */
extern apr_status_t gpmon_ntohpkt(gpmon_packet_t* pkt);

/* strings */
extern char* gpmon_trim(char* s);

/* datetime, e.g. 2004-02-14  23:50:02 */
extern char* gpmon_datetime(apr_time_t t, char str[24]);

/* utility */
extern apr_int32_t get_query_status(apr_int32_t tmid, apr_int32_t ssid,
									apr_int32_t ccnt);

#endif /* GPMONLIB_H */
