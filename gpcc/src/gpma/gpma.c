#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_net.h>
#include <gpcc_fsm.h>
#include <json/json.h>
#include <event.h>
#include <ctype.h>

#include "cmdslot.h"
#include "gpma.h"

gpma_t gpma = {0};


/* Print usage info */
static void usage(const char* program, const char* msg)
{
    fprintf(stderr, "\nUsage: %s [-?b] gpdb_id ma_id dir\n\n", program);
    fprintf(stderr, "    -?      : print this help screen\n");
    fprintf(stderr, "    -b      : boot the Master Agent\n");
    fprintf(stderr, "    -D      : debug mode and do not run as daemon\n");
    fprintf(stderr, "    gpdb_id : the GPDB ID\n");
    fprintf(stderr, "    ma_id   : the Master Agent ID\n");
    fprintf(stderr, "    dir     : agent local storage directory\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "e.g. %s gpdb.test ma.0 /dbfast-1/\n", program);
    fprintf(stderr, "\n");
    if (msg)
	fprintf(stderr, "Error: %s\n\n", msg);
    exit(msg ? 1 : 0);
}


/*  Parse command line for - options and arguments according to 
 *  Usage Info 
 */
static apr_status_t parse_cmdline(int argc, const char* argv[])
{
    apr_getopt_t* handle;
    char opt;
    const char* optarg;
    int i, e, ch;
    FILE* f;
    char msg[1024];

    GPCC_DO(0 == apr_getopt_init(&handle, gpma.pool, argc, argv));
    while (0 == (e = apr_getopt(handle, "?bD", &opt, &optarg))) {
	switch (opt) {
	case 'b': 
	    gpma.boot_flag = 1;
	    break;
	case 'D':
	    gpma.debug = 1;
	    break;
	case '?':
	    usage(argv[0], 0);
	    break;
	default:
	    sprintf(msg, "Unknown option %c", opt);
	    usage(argv[0], msg);
	}
    }
    if (e != APR_EOF) 
	usage(argv[0], "bad command line");
    if (argc - handle->ind != 3)
	usage(argv[0], "argument mismatch");

    gpma.gpdb_id = argv[handle->ind++];
    gpma.ma_id   = argv[handle->ind++];
    gpma.dir     = argv[handle->ind++];

    /* check gpdb of the pattern : gpdb.XXXX */
    sprintf(msg, "GPDB ID must look like 'gpdb.XXXX' where XXXX is an alpha-numeric string");
    if (0 != strncmp(gpma.gpdb_id, "gpdb.", 5)) 
	usage(argv[0], msg);
    if (strlen(gpma.gpdb_id) <= 5) 
	usage(argv[0], msg);
    for (i = strlen(gpma.gpdb_id) - 1; i >= 0; i--) {
	ch = gpma.gpdb_id[i];
	if (! isalnum(ch)) 
	    usage(argv[0], msg);
    }

    /* check ma of the pattern: ma.N */
    sprintf(msg, "MA ID must look like 'ma.N' where N is a number between 0 and 2");
    if (0 != strcmp(gpma.ma_id, "ma.0") 
	&& 0 != strcmp(gpma.ma_id, "ma.1")
	&& 0 != strcmp(gpma.ma_id, "ma.2")) {
	usage(argv[0], msg);
    }

    /* check dir */
    if (gpma.dir[0] != '/') 
	usage(argv[0], "dir must be an absolute path");

    if (strlen(gpma.dir) > 255) 
	usage(argv[0], "dir name is too long (max 255 chars)");
    
    if (0 != apr_dir_make_recursive(gpma.dir, 0x700, gpma.pool))  {
	sprintf(msg, "unable to create directory: %s", gpma.dir);
	usage(argv[0], msg);
    }

    gpma.logfpath = apr_psprintf(gpma.pool, "%s/%s-%s.log", gpma.dir, gpma.gpdb_id, gpma.ma_id);
    f = fopen(gpma.logfpath, "a");
    if (!f) {
	sprintf(msg, "unable to create log file %s", gpma.logfpath);
	usage(argv[0], msg);
    }
    fclose(f);
    
    return 0;
}

static void net_opened(gpcc_net_conn_t conn, 
		       const char* peer_addr,
		       gpcc_net_header_t* to_peer, 
		       gpcc_net_header_t* from_peer)
{
}


static void net_accepted(gpcc_net_conn_t conn,
			 const char* peer_addr,
			 gpcc_net_header_t* from_peer)
{
}


static int net_received(gpcc_net_conn_t conn, 
			const char* buf, int buflen)
{
    /* should not invoke, yet */
    gpcc_warning(FLINE, "net_received should not get called");
    return -1;
}


static void net_received_packet(gpcc_net_conn_t conn, 
				gpcc_net_packet_t* pkt)
{
}


static void net_closed(gpcc_net_conn_t conn)
{
}


static void net_tick(apr_time_t now)
{
}





/* Main program for the daemon */
static void d_main(void)
{
    char date[APR_RFC822_DATE_LEN];
    gpcc_evqueue_t* evq = 0;
    gpcc_event_t* ev = 0;

    event_init();

    /* create the event queue */
    evq = gpma.evq = gpcc_evqueue_create(1000, gpma.pool);

    /* redirect cout and cerr to log */
    freopen(gpma.logfpath, "a", stderr);
    setlinebuf(stderr);
    freopen(gpma.logfpath, "a", stdout);

    /* greetings */
    apr_rfc822_date(date, apr_time_now());
    gpcc_log_info("[%s] started gpma %s %s %s", date, gpma.gpdb_id, gpma.ma_id, gpma.dir);

    /* network up */
    GPCC_DO(0 == gpcc_net_init(config_get_int(gpma.cfg, "sys.ma.port"), 
			       net_opened, net_accepted, 
			       net_received, net_received_packet,
			       net_closed, net_tick));

    gpcc_log_info("gpma listening to port %d", config_get_int(gpma.cfg, "sys.ma.port"));

    gpma.cmdslot = cmdslot_alloc("GPMA", GPMA_CMDSTATE);

    for (;;) {
	int nxtstate;
	gpcc_fsm_func_t** fn;
	cmdslot_t* slot;
	
	if (ev)
	    gpcc_event_free(ev);

	while ((ev = gpcc_evqueue_tryget(evq)) == 0) {
	    if (gpcc_net_once(1))
		gpcc_fatal(FLINE, "error reading network");
	}

	if (ev->cmdidx == 0) 
	    ev->cmdidx = gpma.cmdslot->cmdidx;

	/* find the right slot for the event */
	if (! (slot = cmdslot_find(ev->cmdidx))) {
	    gpcc_log_info("Event %s contains unknown cmdidx %x", 
			  event_str(ev->etype), ev->cmdidx);
	    continue;
	}

	cutin:

	/* retrieve the fsm record */
	if (0 != gpcc_fsm_get(slot->cmdstate, ev->etype, &nxtstate, &fn)) {
	    gpcc_log_info("FSM[%x]: %s + %s not found", slot->cmdidx,
			  cmdstate_str(slot->cmdstate), event_str(ev->etype));
	    continue;
	}

	/* dispatch the event */
	for ( ; *fn && 0 == (*fn)(ev, slot); fn++);

	/* error out ? */
	if (! (*fn)) {
	    gpcc_warning(FLINE, "FSM[%x]: FAILED - cancelling %s + %s = %s", 
			 slot->cmdidx, cmdstate_str(slot->cmdstate), 
			 event_str(ev->etype), cmdstate_str(nxtstate));

	    ev->etype = FAILURE_EVENT;
	    goto cutin;
	}

	/* effect the state transition */
	if (gpma.debug) {
	    gpcc_log_info("FSM[%x]: %s + %s = %s", slot->cmdidx,
			  cmdstate_str(slot->cmdstate), 
			  event_str(ev->etype), cmdstate_str(nxtstate));
	}
	slot->cmdstate = nxtstate;

	if (nxtstate == NULL_CMDSTATE)
	    cmdslot_free(slot->cmdidx);
    }
    /* not reached */
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
    if (gpma.csn != config_get_int64(gpma.cfg, "sys.csn")) {
	gpcc_fatal(FLINE, "config file error: csn mismatch (expect %lld, receive %lld)",
		   gpma.csn, config_get_int64(gpma.cfg, "sys.csn"));
    }

    /* guid must match */
    if (! config_eq(gpma.cfg, "sys.guid", gpma.guid)) {
	gpcc_fatal(FLINE, "config file error: guid mismatch (expect %s, receive %s)",
		       gpma.guid, config_getx(gpma.cfg, "sys.guid", "[null]"));
    }

    /* name must exist */
    if (! config_get(gpma.cfg, "sys.array_name")) {
	gpcc_fatal(FLINE, "config file error: sys.array_name missing");
    }

    /* default ma port if not set */
    i = config_get_or_set_int(gpma.cfg, "sys.ma.port", DEFAULT_MA_PORT);
    if (! (0 < i && i < 65000)) {
	gpcc_fatal(FLINE, "config file error: sys.qd.port invalid (value %d)", i);
    }

    /* default qd port if not set */
    i = config_get_or_set_int(gpma.cfg, "sys.qd.port", DEFAULT_QD_PORT);
    if (! (0 < i && i < 65000)) {
	gpcc_fatal(FLINE, "config file error: sys.qd.port invalid (value %d)", i);
    }

    /* default qe baseport if not set */
    i = config_get_or_set_int(gpma.cfg, "sys.qe.baseport", DEFAULT_QE_BASEPORT);
    if (! (0 < i && i < 65000)) {
	gpcc_fatal(FLINE, "config file error: sys.qe.baseport invalid (value %d)", i);
    }

    /* default min mirror if not set */
    i = config_get_or_set_int(gpma.cfg, "sys.min_mirror", 0);
    if (! (0 <= i && i < 2)) {
	gpcc_fatal(FLINE, "config file error: sys.qe.min_mirror invalid (value %d)", i);
    }


    /* all 3 ma must be set */
    for (i = 0; i < 3; i++) {
	sprintf(buf, "ma.%d", i);
	if (! (v = config_get(gpma.cfg, buf)))
	    gpcc_fatal(FLINE, "config file error: %s not set", buf);
    }

    /* qd must exist */
    v = config_get(gpma.cfg, "seg.m.0");
    if (!v) {
	gpcc_fatal(FLINE, "config file error: seg.m.0 not set");
    }

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "seg.%d.0", i);
	v = config_get(gpma.cfg, buf);
	if (!v) {
	    if (i < 2) 
		gpcc_fatal(FLINE, "config file error: %s not set", buf);
	    break;
	}
    }
    if (! (2 <= i && i < 512)) {
	gpcc_fatal(FLINE, "config file error: too many segments (max 512)");
    }

    /* set the number of QEs */
    config_set_int(gpma.cfg, "seg.max", i);
    
    return 0;
}


/*
 * Load the config into gpma. 
 */
static apr_status_t load_config(void)
{
    if (gpma.boot_flag) {
	/* read config from stdin */
	gpma.cfg = config_read_stream(stdin, gpma.pool);
	gpma.csn = 0;
	config_set_int64(gpma.cfg, "sys.csn", gpma.csn);
	GPCC_DO(0 == verify_config());

	++gpma.csn;
	config_set_int64(gpma.cfg, "sys.csn", gpma.csn);
	GPCC_DO(0 == config_write(gpma.dir, gpma.guid, gpma.csn, gpma.cfg));
    }
    else {
	gpma.csn = config_determine_csn(gpma.dir, gpma.guid);
	gpma.cfg = config_read(gpma.dir, gpma.guid, gpma.csn, gpma.pool);
	if (gpma.cfg == 0) {
	    fprintf(stderr, "ERROR: config file not found in dir %s\n", gpma.dir);
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

    GPCC_DO(0 == apr_pool_create(&gpma.pool, 0));

    GPCC_DO(0 == parse_cmdline(argc, argv));

    GPCC_DO(0 == load_config());

    if (! gpma.debug) {
	GPCC_DO(0 == apr_proc_detach(1));
	/* At this point, we are now a daemon */
    }

    d_main();
    return 0;
}
