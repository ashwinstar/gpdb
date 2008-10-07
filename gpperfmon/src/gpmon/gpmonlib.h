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

#define FMT64 APR_INT64_T_FMT
#define FLINE __FILE__ ":" APR_STRINGIFY(__LINE__)
#define CHECKMEM(x)  if (x) ; else gpmon_fatal(FLINE, "out of memory")

extern int verbose;
extern int very_verbose;
#define TR0(x) gpmon_print x
#define TR1(x) if (!verbose) ; else gpmon_print x
#define TR2(x) if (!very_verbose) ; else gpmon_print x


/* fatal & warning messages */
extern int gpmon_print(const char* fmt, ...);
extern int gpmon_fatal(const char* fline, const char* fmt, ...);
extern int gpmon_fatalx(const char* fline, int e, const char* fmt, ...);
extern int gpmon_warning(const char* fline, const char* fmt, ...);
extern int gpmon_warningx(const char* fline, int e, const char* fmt, ...);

/* convert packets to host order */
extern apr_status_t gpmon_ntohpkt(gpmon_packet_t* pkt);

/* spin lock */
typedef volatile apr_uint32_t gpmon_spin_t;
extern gpmon_spin_t* gpmon_spin_lock(gpmon_spin_t* spin);
extern gpmon_spin_t* gpmon_spin_unlock(gpmon_spin_t* spin);

/* strings */
extern char* gpmon_trim(char* s);

/* datetime, e.g. 2004-02-14  23:50:02 */
extern char* gpmon_datetime(apr_time_t t, char str[24]);


#endif /* GPMONLIB_H */
