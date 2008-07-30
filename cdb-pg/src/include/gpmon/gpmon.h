#ifndef GPMON_H
#define GPMON_H

#ifdef POSTGRES_H
void gpmon_init(void);
typedef int64 apr_int64_t;
typedef int32 apr_int32_t;
typedef int16 apr_int16_t;
typedef uint64 apr_uint64_t;
#endif

extern apr_int64_t gpmon_tick;
typedef struct gpmon_packet_t gpmon_packet_t;
typedef struct gpmon_qlogkey_t gpmon_qlogkey_t;
typedef struct gpmon_qlog_t gpmon_qlog_t;
typedef struct gpmon_qexeckey_t gpmon_qexeckey_t;
typedef struct gpmon_qexec_t gpmon_qexec_t;
typedef struct gpmon_hello_t gpmon_hello_t;
typedef struct gpmon_metrics_t gpmon_metrics_t;


/*
 * this dir sits in $MASTER_DATA_DIRECTORY. always include the
 * suffix / 
 */
#define GPMON_DIR   "./gpmon.1/"
#define GPMON_DB    "gpmon_1"

/* ------------------------------------------------------------------
         INTERFACE
   ------------------------------------------------------------------ */
extern void gpmon_record(apr_int32_t tmid, apr_int32_t xid, apr_int32_t cid,
						 const char* type, /* "qtext", "qplan" */
						 const char* content /* the query text or plan */
						 );
extern void gpmon_send(gpmon_packet_t*);

extern void gpmon_getgid(apr_int32_t*, apr_int32_t*);

/* ------------------------------------------------------------------
         METRICS
   ------------------------------------------------------------------ */
struct gpmon_metrics_t {
    /* if you modify this, do not forget to edit gpcc/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    char hname[NAMEDATALEN];
    struct {
		apr_uint64_t total, used, actual_used, actual_free;
    } mem;
    struct {
		apr_uint64_t total, used, page_in, page_out;
    } swap;
    struct {
		float user_pct, sys_pct, idle_pct;
    } cpu;
    struct {
		float value[3];
    } load_avg;
    struct {
 apr_uint64_t rcount, wcount, rbytes, wbytes;
    } disk;
    struct {
		apr_uint64_t rcount, wcount, rbytes, wbytes;
    } net;
};
    

/* ------------------------------------------------------------------
         QLOG 
   ------------------------------------------------------------------ */

struct gpmon_qlogkey_t {
    /* if you modify this, do not forget to edit gpcc/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    apr_int32_t tmid;  /* transaction time */
    apr_int32_t xid; /* global transaction id */
    apr_int32_t cid;
};

#define GPMON_QLOG_STATUS_INVALID -1
#define GPMON_QLOG_STATUS_SILENT   0
#define GPMON_QLOG_STATUS_SUBMIT   1
#define GPMON_QLOG_STATUS_START    2
#define GPMON_QLOG_STATUS_DONE     3
#define GPMON_QLOG_STATUS_ERROR    4

struct gpmon_qlog_t {
    /* if you modify this, do not forget to edit gpcc/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    gpmon_qlogkey_t key;
    char        user[NAMEDATALEN]; 
    char        db[NAMEDATALEN]; 
    apr_int32_t cost;
    apr_int32_t tsubmit, tstart, tfin;
    apr_int32_t status;		/* GPMON_QLOG_STATUS_XXXXXX */
    apr_int64_t rows_out;
    apr_int32_t _segmax;
    apr_int32_t _segcpu[10];
};


/* ------------------------------------------------------------------
         QEXEC
   ------------------------------------------------------------------ */


/* XXX According to CK.
 * QE will NOT need to touch anything begin with _ 
 */
struct gpmon_qexeckey_t {
    /* if you modify this, do not forget to edit gpcc/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    apr_int32_t tmid;  /* transaction time */
    apr_int32_t xid; /* global transaction id */
    apr_int16_t cid;	/* command id */
    apr_int16_t segid;	/* segment id */
    apr_int32_t pid; 	/* process id */
    apr_int16_t nid;	/* plan node id */
    apr_int16_t zero;		/* unused */
};

#define GPMON_QEXEC_M_COUNT 16 

/* process metrics ... filled in by gpsmon */
typedef struct gpmon_proc_metrics_t gpmon_proc_metrics_t;
struct gpmon_proc_metrics_t {
    apr_int32_t fd_cnt;		/* # opened files / sockets etc */
    float       cpu_pct;	/* cpu usage % */
    struct {
		apr_int64_t size, resident, share, page_faults;
    } mem;
};

struct gpmon_qexec_t {
    /* if you modify this, do not forget to edit gpcc/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    gpmon_qexeckey_t key;
    apr_int32_t pnid;	/* plan parent node id */
    char        _hname[NAMEDATALEN]; 
    char        ntype[NAMEDATALEN]; 	/* node type (SeqScan, HashJoin, etc) */	
    char        nstatus[NAMEDATALEN];  /* node status */
    apr_int32_t tstart, tduration; /* start (wall clock) time and duration.  XXX Leave as 0 for now. */
    apr_int64_t p_mem, p_memmax;   /* XXX Aset instrumentation.  Leave as 0 for now. */ 
    apr_int64_t _cpu_elapsed; /* CPU elapsed for iter */
    apr_int64_t rows_in, rows_out;
    apr_int64_t bytes_in, bytes_out;
    gpmon_proc_metrics_t _p_metrics;
    char  phase[NAMEDATALEN];		/* Spill, ReSpill, etc */ 
    struct {
		char name[NAMEDATALEN];	/* measure name */
		char unit[15+1];	/* measure unit */
		apr_int64_t value; 
		apr_int64_t valuemax;
    } m[GPMON_QEXEC_M_COUNT]; 		/* Measures */
};


/* ------------------------------------------------------------------
         HELLO
   ------------------------------------------------------------------ */

struct gpmon_hello_t {
    apr_int64_t signature;
};



#define GPMON_MAGIC     0x78ab928d
#define GPMON_PACKET_VERSION   1

enum gpmon_pkttype_t {
    GPMON_PKTTYPE_NONE = 0,
    GPMON_PKTTYPE_HELLO = 1,
    GPMON_PKTTYPE_METRICS = 2,
    GPMON_PKTTYPE_QLOG = 3,
    GPMON_PKTTYPE_QEXEC = 4,

    GPMON_PKTTYPE_MAX
};



struct gpmon_packet_t {
    /* if you modify this, do not forget to edit gpcc/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    apr_int32_t magic;
    apr_int16_t version;
    apr_int16_t pkttype;
    union {
		gpmon_hello_t   hello;
		gpmon_metrics_t metrics;
		gpmon_qlog_t    qlog;
		gpmon_qexec_t   qexec;
    } u;
};
    

extern const char* gpmon_qlog_status_string(int gpmon_qlog_status);

#endif 
