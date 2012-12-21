#ifndef GPMONLIB_H
#define GPMONLIB_H

#undef GP_VERSION
#include "postgres_fe.h"

#include "apr_general.h"
#include "apr_time.h"
#include "event.h"
#include "gpmon/gpmon.h"

#ifdef USE_CONNECTEMC
#include "emcconnect/api.h"
#else
typedef enum EmcConnectSeverity
{
	EMC_CONNECT_SEVERITY_UNKNOWN = 0,
} EmcConnectSeverity;

typedef enum EmcConnectDetailedSymptom
{
	EMC_CONNECT_DETAILED_SYMPTOM_NULL = 0,
} EmcConnectDetailedSymptom;
typedef enum EmcConnectModeType_t
{
    EMCCONNECT_MODE_TYPE_OFF = 0,
} EmcConnectModeType_t;

#endif

#define BATCH 8
#define DEFAULT_GPMMON_LOGDIR "gpperfmon/logs"

#ifndef WIN32
typedef int SOCKET;
#endif

#define GPMON_DBUSER "gpmon"

#define GPMON_PACKET_ERR_LOG_TIME 60

#define FMT64 APR_INT64_T_FMT
#define FMTU64 APR_UINT64_T_FMT
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

#define GPMON_DATE_BUF_SIZE 24


/* fatal & warning messages */
extern int gpmon_print(const char* fmt, ...);
extern int gpmon_fatal(const char* fline, const char* fmt, ...);
extern int gpmon_fatalx(const char* fline, int e, const char* fmt, ...);
extern int gpmon_warning(const char* fline, const char* fmt, ...);
extern int gpmon_warningx(const char* fline, int e, const char* fmt, ...);

// fatal messages for smon -- go to stdout only
extern int gpsmon_fatal(const char* fline, const char* fmt, ...);
extern int gpsmon_fatalx(const char* fline, int e, const char* fmt, ...);

/* convert packets to host order */
extern apr_status_t gpmon_ntohpkt(gpmon_packet_t* pkt);

/* strings */
extern char* gpmon_trim(char* s);

/* file manip */
extern int gpmon_recursive_mkdir(char* work_dir);

/* datetime, e.g. 2004-02-14  23:50:02 */
extern char* gpmon_datetime(time_t t, char str[GPMON_DATE_BUF_SIZE]);

/* version that rounds to lowest 5 sec interval */
extern char* gpmon_datetime_rounded(time_t t, char str[GPMON_DATE_BUF_SIZE]);

/* utility */
extern apr_int32_t get_query_status(apr_int32_t tmid, apr_int32_t ssid,
									apr_int32_t ccnt);

#define PATH_TO_APPLIANCE_VERSION_FILE "/etc/gpdb-appliance-version"

typedef struct addressinfo_holder_t addressinfo_holder_t;
struct addressinfo_holder_t                                                                        
{
        char* address;  // an alternate host name to access this host                                                                           
	char* ipstr; // the ipstring assoicated with this address
        struct addressinfo_holder_t* next;
};

typedef struct multi_interface_holder_t multi_interface_holder_t;
struct multi_interface_holder_t
{
	addressinfo_holder_t* current; 
	unsigned int counter;
};


/* segment host */ 
typedef struct host_t host_t; 
struct host_t
{
	apr_thread_mutex_t *mutex; 
	int sock; /* socket connected to gpsmon on this host */ 
	int eflag; /* flag: socket has error */
	struct event* event; /* points to _event if set */
	struct event _event; 

	char* hostname; 

	addressinfo_holder_t* addressinfo_head; 
	addressinfo_holder_t* addressinfo_tail; 

	// there are 2 of these so we don't need to mutex
	multi_interface_holder_t connection_hostname;	
	multi_interface_holder_t snmp_hostname;	

	apr_uint32_t address_count; 
	char* data_dir;
	unsigned char is_master; /* 1 if host is the same host where the master runs */ 
	char ever_connected; /* set to non-zero after first connection attempt */
};

char* get_connection_hostname(host_t* host);
char* get_connection_ip(host_t* host);
char* get_snmp_hostname(host_t* host);
void advance_connection_hostname(host_t* host);
void advance_snmp_hostname(host_t* host);

typedef struct snmp_module_params_t snmp_module_params_t;
struct snmp_module_params_t
{
	char configfile[PATH_MAX];
	char outdir[PATH_MAX];
	char externaltable_dir[PATH_MAX];
	EmcConnectModeType_t emcconnect_mode;
	int health_harvest_interval;
	int warning_disk_space_percentage;
	int error_disk_space_percentage;
	int healthmon_running_separately;
};

double subtractTimeOfDay(struct timeval* begin, struct timeval* end);


extern const char* gpdb_getnodename(PerfmonNodeType type);
extern const char* gpdb_getnodestatus(PerfmonNodeStatus status);
extern apr_status_t gpdb_getnode_metricinfo(PerfmonNodeType type, apr_byte_t metricnum, const char** name, const char** unit);
extern apr_status_t gpdb_debug_string_lookup_table();
extern void gpdb_get_single_string_from_query(const char* QUERY, char** resultstring, apr_pool_t* pool);
extern int snmp_report(host_t* tab, int tabsz);
extern apr_status_t gpmmon_init_snmp(snmp_module_params_t* params);
extern bool is_healthmon_running_separately();
#endif /* GPMONLIB_H */
