#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_net.h>
#include "config.h"



#define DEFAULT_QD_MAX_CONNECT  25
#define DEFAULT_QE_MAX_CONNECT  25
#define DEFAULT_QD_MAX_BUFFER   "128mb"
#define DEFAULT_QE_MAX_BUFFER   "128mb"
#define DEFAULT_MA_PORT   	12000
#define DEFAULT_QD_PORT   	12100
#define DEFAULT_QE_BASEPORT	12200

struct {
    apr_pool_t*  pool;
    const char*  guid;		/* GUID */
    const char*  maid;		/* MA ID */
    const char*  dir;		/* MA data & log directory */
    config_t*    cfg;		/* config  */
    apr_uint64_t csn;		/* change seq number of config file */
    int          boot_flag;	/* TRUE if booting */
    int          debug;		/* TRUE if debug mode */
    const char*  logfpath;	/* path to log file */
    gpcc_evqueue_t* evq;	/* the event queue */
} cb = {0};



typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    int 	cmdidx;
    /*cmdstate_t  cmdstate;*/
    int         countdown;

    union {
	struct {

	} a;
    } u;
};



static void* d_net_opened(gpcc_net_conn_t conn, void* arg)
{
    return arg;
}

static void d_net_closed(gpcc_net_conn_t conn, void* arg)
{
}


static void d_net_recv(gpcc_net_conn_t conn, void* arg,
		       int pkttype, int reqid, 
		       const char* pkt, int pktlen)
{
}


static void d_net_tick(apr_time_t now)
{
}



/* Main program for the daemon */
static void d_main(void)
{
    char date[APR_RFC822_DATE_LEN];
    gpcc_event_t* ev = 0;
    gpcc_evqueue_t* evq;

    /* create the event queue */
    evq = cb.evq = gpcc_evqueue_create(1000, cb.pool);

    /* redirect cout and cerr to log */
    freopen(cb.logfpath, "a", stderr);
    setlinebuf(stderr);
    freopen(cb.logfpath, "a", stdout);

    /* greetings */
    apr_rfc822_date(date, apr_time_now());
    gpcc_log_info("[%s] started gpma %s %s %s", date, cb.guid, cb.maid, cb.dir);

    /* network up */
    GPCC_DO(0 == gpcc_net_start(config_get_int(cb.cfg, "sys.ma.port"), 
				d_net_opened,
				d_net_closed,
				d_net_recv,
				d_net_tick));
    gpcc_log_info("gpma listening to port %d", config_get_int(cb.cfg, "sys.ma.port"));

    for ( ; 0 != (ev = gpcc_evqueue_get(evq)); gpcc_event_free(ev)) {
	/* map the cmdidx */
	/* retrieve fsm record */
	/* dispatch actions */
    }

    fprintf(stderr, "exiting\n");
}



/* Print usage info */
static void usage(const char* program, int status)
{
    fprintf(stderr, "\nUsage: %s [-?b] guid maid dir\n\n", program);
    fprintf(stderr, "    -?   : print this help screen\n");
    fprintf(stderr, "    -b   : boot the Master Agent\n");
    fprintf(stderr, "    -D   : debug mode and do not run as daemon\n");
    fprintf(stderr, "    guid : the Global Unique ID of the GPDB\n");
    fprintf(stderr, "    maid : the Master Agent ID\n");
    fprintf(stderr, "    dir  : agent local storage directory\n");
    fprintf(stderr, "\n");
    exit(status);
}


/*  Parse command line for - options and arguments according to 
 *  Usage Info 
 */
static apr_status_t parse_cmdline(int argc, const char* argv[])
{
    apr_getopt_t* handle;
    char opt;
    const char* optarg;
    int i, e;
    FILE* f;

    GPCC_DO(0 == apr_getopt_init(&handle, cb.pool, argc, argv));
    while (0 == (e = apr_getopt(handle, "?bD", &opt, &optarg))) {
	switch (opt) {
	case 'b': 
	    cb.boot_flag = 1;
	    break;
	case 'D':
	    cb.debug = 1;
	    break;
	case '?':
	    usage(argv[0], 0);
	    break;
	default:
	    usage(argv[0], 1);
	}
    }
    if (e != APR_EOF) 
	usage(argv[0], 1);
    if (argc - handle->ind != 3)
	usage(argv[0], 1);
    cb.guid = argv[handle->ind++];
    cb.maid = argv[handle->ind++];
    cb.dir = argv[handle->ind++];

    if (strlen(cb.guid) != 17 || cb.guid[8] != '-') {
	gpcc_log_error("invalid guid in command");
	exit(1);
    }
    for (i = 0; i < 17; i++) {
	int c = cb.guid[i];
	if (i != 8 && ! ('0' <= c && c <= '9')) {
	    gpcc_log_error("invalid guid in command");
	    exit(1);
	}
    }

    cb.maid = apr_pstrcat(cb.pool, "ma.", cb.maid, NULL);
    if (0 != strcmp(cb.maid, "ma.0") 
	&& 0 != strcmp(cb.maid, "ma.1")
	&& 0 != strcmp(cb.maid, "ma.2")) {
	gpcc_log_error("invalid master agent id in command");
	exit(1);
    }

    if (strlen(cb.dir) == 0) {
	fprintf(stderr, "invalid dir parameter\n");
	exit(1);
    }

    if (cb.dir[0] != '/') {
	fprintf(stderr, "dir must be an absolute path\n");
	exit(1);
    }
    
    if (0 != apr_dir_make_recursive(cb.dir, 0x700, cb.pool))  {
	fprintf(stderr, "unable to create dir %s\n", cb.dir);
	exit(1);
    }

    cb.logfpath = apr_psprintf(cb.pool, "%s/gpma_%s.log", cb.dir, cb.guid);
    f = fopen(cb.logfpath, "a");
    if (!f) {
	fprintf(stderr, "unable to create log file %s\n", cb.logfpath);
	exit(1);
    }
    fclose(f);
    
    return 0;
}


/*
 *  Sanity check on config info.
 */
static apr_status_t verify_config(void)
{
    int i;
    const char* v;
    char buf[1024];

    /* csn must match */
    if (cb.csn != config_get_int64(cb.cfg, "sys.csn")) {
	gpcc_log_error("config file error: csn mismatch (expect %lld, receive %lld)",
		       cb.csn, config_get_int64(cb.cfg, "sys.csn"));
	gpcc_fatal(FLINE);
    }

    /* guid must match */
    if (! config_eq(cb.cfg, "sys.guid", cb.guid)) {
	gpcc_log_error("config file error: guid mismatch (expect %s, receive %s)",
		       cb.guid, config_getx(cb.cfg, "sys.guid", "[null]"));
	gpcc_fatal(FLINE);
    }

    /* name must exist */
    if (! config_get(cb.cfg, "sys.name")) {
	gpcc_log_error("config file error: sys.name missing");
	gpcc_fatal(FLINE);
    }

    /* default ma port if not set */
    i = config_get_or_set_int(cb.cfg, "sys.ma.port", DEFAULT_MA_PORT);
    if (! (0 < i && i < 65000)) {
	gpcc_log_error("config file error: sys.qd.port invalid (value %d)", i);
	gpcc_fatal(FLINE);
    }

    /* default qd port if not set */
    i = config_get_or_set_int(cb.cfg, "sys.qd.port", DEFAULT_QD_PORT);
    if (! (0 < i && i < 65000)) {
	gpcc_log_error("config file error: sys.qd.port invalid (value %d)", i);
	gpcc_fatal(FLINE);
    }

    /* default qe baseport if not set */
    i = config_get_or_set_int(cb.cfg, "sys.qe.baseport", DEFAULT_QE_BASEPORT);
    if (! (0 < i && i < 65000)) {
	gpcc_log_error("config file error: sys.qe.baseport invalid (value %d)", i);
	gpcc_fatal(FLINE);
    }

    /* default qd max connect if not set */
    i = config_get_or_set_int(cb.cfg, "sys.qd.max_connect", DEFAULT_QD_MAX_CONNECT);
    if (! (0 < i && i <= 512)) {
	gpcc_log_error("config file error: sys.qd.max_connect invalid (value %d)", i);
	gpcc_fatal(FLINE);
    }

    /* default qe max connect if not set */
    i = config_get_or_set_int(cb.cfg, "sys.qe.max_connect", DEFAULT_QE_MAX_CONNECT);
    if (! (0 < i && i <= 512)) {
	gpcc_log_error("config file error: sys.qe.max_connect invalid (value %d)", i);
	gpcc_fatal(FLINE);
    }

    /* default min mirror if not set */
    i = config_get_or_set_int(cb.cfg, "sys.min_mirror", 0);
    if (! (0 <= i && i < 2)) {
	gpcc_log_error("config file error: sys.qe.min_mirror invalid (value %d)", i);
	gpcc_fatal(FLINE);
    }

    /* default qd max buffer if not set */
    v = config_get_or_set(cb.cfg, "sys.qd.max_buffer", DEFAULT_QD_MAX_BUFFER);
    if (gpcc_string_parse_memory(v) == 0) {
	gpcc_log_error("config file error: sys.qd.max_buffer invalid (value %s)", v);
	gpcc_fatal(FLINE);
    }
	
    /* default qe max buffer if not set */
    v = config_get_or_set(cb.cfg, "sys.qe.max_buffer", DEFAULT_QE_MAX_BUFFER);
    if (gpcc_string_parse_memory(v) == 0) {
	gpcc_log_error("config file error: sys.qe.max_buffer invalid (value %s)", v);
	gpcc_fatal(FLINE);
    }

    /* all 3 ma must be set */
    for (i = 0; i < 3; i++) {
	sprintf(buf, "ma.%d", i);
	v = config_get(cb.cfg, buf);
	if (!v) {
	    gpcc_log_error("config file error: %s not set", buf);
	    gpcc_fatal(FLINE);
	}
    }

    /* qd must exist */
    v = config_get(cb.cfg, "seg.m.0");
    if (!v) {
	gpcc_log_error("config file error: seg.m.0 not set");
	gpcc_fatal(FLINE);
    }

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "seg.%d.0", i);
	v = config_get(cb.cfg, buf);
	if (!v) {
	    if (i < 2) {
		gpcc_log_error("config file error: %s not set", buf);
		gpcc_fatal(FLINE);
	    }
	    break;
	}
    }
    if (! (2 <= i && i < 512)) {
	gpcc_log_error("config file error: too many segments (max 512)");
	gpcc_fatal(FLINE);
    }

    /* set the number of QEs */
    config_set_int(cb.cfg, "seg.max", i);
    
    return 0;
}


/*
 * Load the config into cb. 
 */
static apr_status_t load_config(void)
{
    if (cb.boot_flag) {
	/* read config from stdin */
	cb.cfg = config_read_stream(stdin, cb.pool);
	cb.csn = 0;
	config_set_int64(cb.cfg, "sys.csn", cb.csn);
	GPCC_DO(0 == verify_config());

	++cb.csn;
	config_set_int64(cb.cfg, "sys.csn", cb.csn);
	GPCC_DO(0 == config_write(cb.dir, cb.guid, cb.csn, cb.cfg));
    }
    else {
	cb.csn = config_determine_csn(cb.dir, cb.guid);
	cb.cfg = config_read(cb.dir, cb.guid, cb.csn, cb.pool);
	if (cb.cfg == 0) {
	    fprintf(stderr, "ERROR: config file not found in dir %s\n", cb.dir);
	    exit(1);
	}
	GPCC_DO(0 == verify_config());
    }

    return 0;
}


int main(int argc, const char* argv[])
{
    GPCC_DO(0 == apr_app_initialize(&argc, (char const* const **) &argv, NULL));
    atexit(apr_terminate);

    GPCC_DO(0 == apr_pool_create(&cb.pool, 0));

    GPCC_DO(0 == parse_cmdline(argc, argv));

    GPCC_DO(0 == load_config());

    if (! cb.debug) {
	GPCC_DO(0 == apr_proc_detach(1));
	/* At this point, we are now a daemon */
    }

    d_main();
    return 0;
}
