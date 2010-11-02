#ifndef GPMON_H
#define GPMON_H

#ifdef POSTGRES_H
void gpmon_init(void);
typedef int64 apr_int64_t;
typedef int32 apr_int32_t;
typedef int16 apr_int16_t;
typedef uint8 apr_byte_t;
typedef uint16 apr_uint16_t;
typedef uint32 apr_uint32_t;
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
typedef struct gpmon_seginfo_t gpmon_seginfo_t;
typedef struct gpmon_filerepinfo_t gpmon_filerepinfo_t;

/*
 * this dir sits in $MASTER_DATA_DIRECTORY. always include the
 * suffix /
 */
#define GPMON_DIR   "./gpperfmon/data/"
#define GPMON_DIR_MAX_PATH 100
#define GPMON_DB    "gpperfmon"

/*
this is enough space for 2 names plus a . between the names plus a null char at the end of the string
for example SCHEMA.RELATION\0
*/
#define SCAN_REL_NAME_BUF_SIZE (NAMEDATALEN*2)


/* ------------------------------------------------------------------
         INTERFACE
   ------------------------------------------------------------------ */

extern void gpmon_qlog_packet_init(gpmon_packet_t *gpmonPacket);
extern void gpmon_qlog_query_submit(gpmon_packet_t *gpmonPacket);
extern void gpmon_qlog_query_text(const gpmon_packet_t *gpmonPacket, const char *queryText);
extern void gpmon_qlog_query_start(gpmon_packet_t *gpmonPacket);
extern void gpmon_qlog_query_end(gpmon_packet_t *gpmonPacket);
extern void gpmon_qlog_query_error(gpmon_packet_t *gpmonPacket);
extern void gpmon_send(gpmon_packet_t*);
extern void gpmon_gettmid(apr_int32_t*);

/* ------------------------------------------------------------------
         METRICS
   ------------------------------------------------------------------ */
struct gpmon_metrics_t 
{
	/* if you modify this, do not forget to edit gpperfmon/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */

	char hname[NAMEDATALEN];
	struct 
	{
		apr_uint64_t total, used, actual_used, actual_free;
	} mem;

	struct 
	{
		apr_uint64_t total, used, page_in, page_out;
	} swap;

	struct 
	{
		float user_pct, sys_pct, idle_pct;
	} cpu;

	struct 
	{
		float value[3];
	} load_avg;

	struct 
	{
		apr_uint64_t ro_rate, wo_rate, rb_rate, wb_rate;
	} disk;

	struct 
	{
		apr_uint64_t rp_rate, wp_rate, rb_rate, wb_rate;
	} net;
};


/* ------------------------------------------------------------------
         QLOG
   ------------------------------------------------------------------ */

struct gpmon_qlogkey_t {
    /* if you modify this, do not forget to edit gpperfmon/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    apr_int32_t tmid;  /* transaction time */
    apr_int32_t ssid; /* session id */
    apr_int32_t ccnt; /* command count */
};

#define GPMON_QLOG_STATUS_INVALID -1
#define GPMON_QLOG_STATUS_SILENT   0
#define GPMON_QLOG_STATUS_SUBMIT   1
#define GPMON_QLOG_STATUS_START    2
#define GPMON_QLOG_STATUS_DONE     3
#define GPMON_QLOG_STATUS_ERROR    4

#define GPMON_NUM_SEG_CPU 10

struct gpmon_qlog_t {
    /* if you modify this, do not forget to edit gpperfmon/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    gpmon_qlogkey_t key;
    char        user[NAMEDATALEN];
    char        db[NAMEDATALEN];
    apr_int32_t cost;
    apr_int32_t tsubmit, tstart, tfin;
    apr_int32_t status;		/* GPMON_QLOG_STATUS_XXXXXX */
    apr_int64_t rows_out;
    apr_int32_t _segmax;
    apr_int32_t _segcpu[GPMON_NUM_SEG_CPU];
};


/* ------------------------------------------------------------------
         QEXEC
   ------------------------------------------------------------------ */


/* XXX According to CK.
 * QE will NOT need to touch anything begin with _
 */
struct gpmon_qexeckey_t {
    /* if you modify this, do not forget to edit gpperfmon/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    apr_int32_t tmid;  /* transaction time */
    apr_int32_t ssid; /* session id */
    apr_int16_t ccnt;	/* command count */
    apr_int16_t segid;	/* segment id */
    apr_int32_t pid; 	/* process id */
    apr_int16_t nid;	/* plan node id */
    apr_int16_t zero;		/* unused */
};


/* process metrics ... filled in by gpsmon */
typedef struct gpmon_proc_metrics_t gpmon_proc_metrics_t;
struct gpmon_proc_metrics_t {
    apr_int32_t fd_cnt;		/* # opened files / sockets etc */
    float       cpu_pct;	/* cpu usage % */
    struct {
		apr_int64_t size, resident, share;
    } mem;
};

enum {
	GPMON_QEXEC_M_ROWSIN = 0,
#if 0
	GPMON_QEXEC_M_BYTESIN,
	GPMON_QEXEC_M_BYTESOUT,
#endif
	GPMON_QEXEC_M_NODE_START,
	GPMON_QEXEC_M_COUNT = 10,
	GPMON_QEXEC_M_DBCOUNT = 16
};

/* there is an array of 10 measurements in the array inside a gpmon packet
   however the gpperfmon DB will contain 16 measurements. 
   long term we need a more extensible solution. short term we limit size of packets
   to what is actually used to conserve resources
*/

struct gpmon_qexec_t {
	/* if you modify this, do not forget to edit gpperfmon/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
	gpmon_qexeckey_t 		key;
	apr_int32_t  			pnid;	/* plan parent node id */
	char        			_hname[NAMEDATALEN];
	apr_uint16_t			nodeType; 					/* using enum PerfmonNodeType */
	apr_byte_t 				status;    /* node status using PerfmonNodeStatus */
	apr_int32_t 			tstart, tduration; /* start (wall clock) time and duration.  XXX Leave as 0 for now. */
	apr_int64_t 			p_mem, p_memmax;   /* XXX Aset instrumentation.  Leave as 0 for now. */
	apr_int64_t				_cpu_elapsed; /* CPU elapsed for iter */
	gpmon_proc_metrics_t 	_p_metrics;
	apr_int64_t 			rowsout, rowsout_est;
	apr_int64_t 			measures[GPMON_QEXEC_M_COUNT];
	char 					relation_name[SCAN_REL_NAME_BUF_SIZE];
};

/*
 * Segment-related statistics
 */
struct gpmon_seginfo_t {
	apr_int32_t dbid; 							// dbid as in gp_segment_configuration
	char hostname[NAMEDATALEN];					// hostname without NIC extension
	apr_uint64_t dynamic_memory_used;			// allocated memory in bytes
	apr_uint64_t dynamic_memory_available;		// available memory in bytes,
};

/* 
 * Filerep related statistics
 */
struct gpmon_filerepinfo_t 
{
	// sequence number incremented by 1 for each gpperfmon message sent by a sender
	apr_uint32_t sequencenum;

	// KEY
	char hostname[NAMEDATALEN];					// hostname without NIC extension .. filled in by perfmon not GPDB
    apr_uint16_t primary_port;
    apr_uint16_t mirror_port;

	// EVENT: write systemcall on primary
	apr_uint32_t primary_write_syscall_size_avg;
	apr_uint32_t primary_write_syscall_size_max;
	apr_uint32_t primary_write_syscall_time_avg; // microseconds;
	apr_uint32_t primary_write_syscall_time_max; // microseconds;
	apr_uint32_t primary_write_syscall_count; 

	// EVENT: write systemcall on mirror
	apr_uint32_t mirror_write_syscall_size_avg;
	apr_uint32_t mirror_write_syscall_size_max;
	apr_uint32_t mirror_write_syscall_time_avg; // microseconds;
	apr_uint32_t mirror_write_syscall_time_max; // microseconds;
	apr_uint32_t mirror_write_syscall_count; 

	// EVENT: fsync systemcall on primary
	apr_uint32_t primary_fsync_syscall_time_avg; // microseconds;
	apr_uint32_t primary_fsync_syscall_time_max; // microseconds;
	apr_uint32_t primary_fsync_syscall_count; 

	// EVENT: fsync systemcall on mirror
	apr_uint32_t mirror_fsync_syscall_time_avg; // microseconds;
	apr_uint32_t mirror_fsync_syscall_time_max; // microseconds;
	apr_uint32_t mirror_fsync_syscall_count; 

	// EVENT: putting write message into shared memory
	apr_uint32_t primary_write_shmem_size_avg;
	apr_uint32_t primary_write_shmem_size_max;
	apr_uint32_t primary_write_shmem_time_avg; // microseconds;
	apr_uint32_t primary_write_shmem_time_max; // microseconds;
	apr_uint32_t primary_write_shmem_count; 

	// EVENT: putting fsync message into shared memory
	apr_uint32_t primary_fsync_shmem_size_avg;
	apr_uint32_t primary_fsync_shmem_size_max;
	apr_uint32_t primary_fsync_shmem_time_avg; // microseconds;
	apr_uint32_t primary_fsync_shmem_time_max; // microseconds;
	apr_uint32_t primary_fsync_shmem_count; 

	// EVENT: Roundtrip from sending fsync message to mirror to get ack back on primary
	apr_uint32_t roundtrip_fsync_msg_time_avg; // microseconds;
	apr_uint32_t roundtrip_fsync_msg_time_max; // microseconds;
	apr_uint32_t roundtrip_fsync_msg_count;

	// EVENT: Roundtrip from sending test message to mirror to getting back on primary
	apr_uint32_t roundtrip_test_msg_time_avg; // microseconds;
	apr_uint32_t roundtrip_test_msg_time_max; // microseconds;
	apr_uint32_t roundtrip_test_msg_count;
};

/* ------------------------------------------------------------------
         HELLO
   ------------------------------------------------------------------ */

struct gpmon_hello_t {
    apr_int64_t signature;
};



#define GPMON_MAGIC     0x78ab928d

/*
 *  When updating the format of the packets update these variables
 *  in order to make sure that the communication between the GPDB
 *  and perfmon is compatible 
 */
#define GPMON_PACKET_VERSION   5
#define GPMMON_PACKET_VERSION_STRING "gpmmon packet version 5\n"

enum gpmon_pkttype_t {
    GPMON_PKTTYPE_NONE = 0,
    GPMON_PKTTYPE_HELLO = 1,
    GPMON_PKTTYPE_METRICS = 2,
    GPMON_PKTTYPE_QLOG = 3,
    GPMON_PKTTYPE_QEXEC = 4,
    GPMON_PKTTYPE_SEGINFO = 5,
    GPMON_PKTTYPE_FILEREP = 6,

    GPMON_PKTTYPE_MAX
};



struct gpmon_packet_t {
    /* if you modify this, do not forget to edit gpperfmon/src/gpmon/gpmonlib.c:gpmon_ntohpkt() */
    apr_int32_t magic;
    apr_int16_t version;
    apr_int16_t pkttype;
    union {
		gpmon_hello_t   hello;
		gpmon_metrics_t metrics;
		gpmon_qlog_t    qlog;
		gpmon_qexec_t   qexec;
		gpmon_seginfo_t seginfo;
		gpmon_filerepinfo_t filerepinfo;
    } u;
};


extern const char* gpmon_qlog_status_string(int gpmon_qlog_status);

/* when adding a node type for perfmon display be sure to also update the corresponding structures in
   in gpperfmon/src/gpmon/gpmonlib.c */

typedef enum PerfmonNodeType
{
    PMNT_Invalid = 0,
    PMNT_Append,
    PMNT_AppendOnlyScan,
    PMNT_Aggregate,
    PMNT_GroupAggregate,
    PMNT_HashAggregate,
    PMNT_AppendOnlyColumnarScan,
    PMNT_BitmapAnd,
    PMNT_BitmapOr,
    PMNT_BitmapAppendOnlyScan,
    PMNT_BitmapHeapScan,
    PMNT_BitmapIndexScan,
    PMNT_ExternalScan,
    PMNT_FunctionScan,
    PMNT_Group,
    PMNT_Hash,
    PMNT_HashJoin,
    PMNT_HashLeftJoin,
    PMNT_HashLeftAntiSemiJoin,
    PMNT_HashFullJoin,
    PMNT_HashRightJoin,
    PMNT_HashExistsJoin,
    PMNT_HashReverseInJoin,
    PMNT_HashUniqueOuterJoin,
    PMNT_HashUniqueInnerJoin,
    PMNT_IndexScan,
    PMNT_Limit,
    PMNT_Materialize,
    PMNT_MergeJoin,
    PMNT_MergeLeftJoin,
    PMNT_MergeLeftAntiSemiJoin,
    PMNT_MergeFullJoin,
    PMNT_MergeRightJoin,
    PMNT_MergeExistsJoin,
    PMNT_MergeReverseInJoin,
    PMNT_MergeUniqueOuterJoin,
    PMNT_MergeUniqueInnerJoin,
    PMNT_RedistributeMotion,
    PMNT_BroadcastMotion,
    PMNT_GatherMotion,
    PMNT_NestedLoop,
    PMNT_NestedLoopLeftJoin,
    PMNT_NestedLoopLeftAntiSemiJoin,
    PMNT_NestedLoopFullJoin,
    PMNT_NestedLoopRightJoin,
    PMNT_NestedLoopExistsJoin,
    PMNT_NestedLoopReverseInJoin,
    PMNT_NestedLoopUniqueOuterJoin,
    PMNT_NestedLoopUniqueInnerJoin,
    PMNT_Result,
    PMNT_Repeat,
    PMNT_SeqScan,
    PMNT_SetOp,
    PMNT_SetOpIntersect,
    PMNT_SetOpIntersectAll,
    PMNT_SetOpExcept,
    PMNT_SetOpExceptAll,
    PMNT_SharedScan,
    PMNT_Sort,
    PMNT_SubqueryScan,
    PMNT_TidScan,
    PMNT_Unique,
    PMNT_ValuesScan,
    PMNT_Window,
	PMNT_MAXIMUM_ENUM

} PerfmonNodeType;

typedef enum PerfmonNodeStatus
{
	PMNS_Initialize = 0,
	PMNS_Executing,
	PMNS_Finished

} PerfmonNodeStatus;

#endif
