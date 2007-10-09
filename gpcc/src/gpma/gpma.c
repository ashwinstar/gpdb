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
static void usage(const char* msg)
{
    fprintf(stderr, "\nUsage: %s [-D?] [-b cfgfile] ma_unit\n\n", gpma.progname);
    fprintf(stderr, "    -?         : print this help screen\n");
    fprintf(stderr, "    -D         : debug mode and do not run as daemon\n");
    fprintf(stderr, "    -b cfgfile : boot the Master Agent with cfgfile ('-' for stdin)\n");
    fprintf(stderr, "    ma_unit    : the Master Agent Unit (e.g. ma.1) \n");
    fprintf(stderr, "\n");
    fprintf(stderr, "e.g. %s mycluster ma.0\n", gpma.progname);
    fprintf(stderr, "\n");
    if (msg)
	fprintf(stderr, "Error: %s\n\n", msg);
    exit(msg ? 1 : 0);
}


/*  Parse command line for - options and arguments according to 
 *  Usage Info 
 */
apr_status_t parse_cmdline(int argc, const char* argv[])
{
    apr_getopt_t* handle;
    char opt;
    const char* optarg;
    int e;
    FILE* f;
    char msg[1024];

    GPCC_DO(0 == apr_getopt_init(&handle, gpma.pool, argc, argv));
    while (0 == (e = apr_getopt(handle, "?b:D", &opt, &optarg))) {
	switch (opt) {
	case 'b': 
	    gpma.boot_fname = optarg;
	    break;
	case 'D':
	    gpma.debug = 1;
	    break;
	case '?':
	    usage(0);
	    break;
	default:
	    sprintf(msg, "Unknown option %c", opt);
	    usage(msg);
	}
    }
    if (e != APR_EOF) 
	usage("bad command line");

    if (argc - handle->ind != 1)
	usage("Please specify ma_unit");

    gpma.ma_unit = argv[handle->ind++];

    /* check ma of the pattern: ma.N */
    sprintf(msg, "MA ID must look like 'ma.N' where N is a number between 0 and 2");
    if (0 != strcmp(gpma.ma_unit, "ma.0") 
	&& 0 != strcmp(gpma.ma_unit, "ma.1")
	&& 0 != strcmp(gpma.ma_unit, "ma.2")) {
	usage(msg);
    }

    /* check the boot_fname */
    if (gpma.boot_fname && 0 != strcmp(gpma.boot_fname, "-")) {
	f = fopen(gpma.boot_fname, "r");
	if (!f) {
	    sprintf(msg, "unable to open boot file %s", gpma.boot_fname);
	    usage(msg);
	}
	fclose(f);
    }
    
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
    if (from_peer->argc == 3) {
	/* check for HTTP GET/POST */
	if (0 == memcmp("HTTP/1.", from_peer->argv[2], 7)
	    && 0 == memcmp("GET", from_peer->argv[0], 4)
	    && 0 == memcmp("/gpcc?", from_peer->argv[1], 6)) {

	}
    }
    else if (from_peer->argc == 1) {
	/* check if it is GPCC_NET_PACKET_PROTO */
    }
    /* unknown protocol - reject the connection */
    gpcc_net_close(conn);
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
    gpcc_event_t* ev = gpcc_event_alloc(TICK_EVENT, gpma.cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}





/* Main program for the daemon */
void d_main(void)
{
    char date[APR_RFC822_DATE_LEN];
    gpcc_evqueue_t* evq = 0;
    gpcc_event_t* ev = 0;
    const char* logfpath = apr_psprintf(gpma.pool, "%s/%s-%s.log", 
					gpma.datadir, gpma.guid, gpma.ma_unit);
    /* redirect cerr to log */
    if (! freopen(logfpath, "a", stderr)) 
	gpcc_warning(FLINE, "unable to open log file '%s'", logfpath);
    else {
	printf("Logs will be written to %s\n", logfpath);
	setlinebuf(stderr);
	/* freopen(logfpath, "a", stdout); */
    }

    event_init();

    cmdslot_init(gpma.pool);
    gpma.cmdslot = cmdslot_alloc("GPMA", GPMA_CMDSTATE);

    fsm_init();

    /* create the event queue */
    evq = gpma.evq = gpcc_evqueue_create(1000, gpma.pool);

    /* greetings */
    apr_rfc822_date(date, apr_time_now());
    gpcc_log_info("[%s] started gpma %s %s %s", date, gpma.guid, gpma.ma_unit, gpma.datadir);

    /* network up */
    GPCC_DO(0 == gpcc_net_init(config_get_int(gpma.cfg, "ma.port"), 
			       net_opened, net_accepted, 
			       net_received, net_received_packet,
			       net_closed, net_tick));

    gpcc_log_info("gpma listening to port %d", config_get_int(gpma.cfg, "ma.port"));


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

	/*printf("event %s\n", event_str(ev->etype));*/

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

    /* guid must match */
    if (! config_eq(gpma.cfg, "gpdb.guid", gpma.guid)) {
	gpcc_fatal(FLINE, "config file error: gpdb.guid mismatch (expect %s, receive %s)",
		       gpma.guid, config_getx(gpma.cfg, "gpdb.guid", "[null]"));
    }

    /* name must exist */
    if (! config_get(gpma.cfg, "gpdb.name")) {
	gpcc_fatal(FLINE, "config file error: gpdb.name missing");
    }

    /* default ma port if not set */
    if (! config_get(gpma.cfg, "ma.port")) {
	gpcc_fatal(FLINE, "config file error: ma.port missing");
    }

    /* default master_port port if not set */
    if (! config_get(gpma.cfg, "gpdb.seg.master_port")) {
	gpcc_fatal(FLINE, "config file error: gpdb.seg.master_port missing");
    }
    i = config_get_int(gpma.cfg, "gpdb.seg.master_port");
    if (! (0 < i && i < (1 << 16))) {
	gpcc_fatal(FLINE, "config file error: gpdb.seg.master_port invalid (value %d)", i);
    }

    /* default qe baseport if not set */
    if (! config_get(gpma.cfg, "gpdb.seg.base_port")) {
	gpcc_fatal(FLINE, "config file error: gpdb.seg.base_port missing");
    }
    i = config_get_int(gpma.cfg, "gpdb.seg.base_port");
    if (! (0 < i && i < (1 << 16))) {
	gpcc_fatal(FLINE, "config file error: gpdb.seg.base_port invalid (value %d)", i);
    }

    /* default min mirror if not set */
    i = config_get_or_set_int(gpma.cfg, "gpdb.min_mirror", 0);
    if (! (0 <= i && i < 2)) {
	gpcc_fatal(FLINE, "config file error: gpdb.min_mirror invalid (value %d)", i);
    }


    /* at least 1 ma must be set */
    for (i = 0; i < 3; i++) {
	sprintf(buf, "ma.%d", i);
	if (! (v = config_get(gpma.cfg, buf)))
	    break;
    }
    if (i == 0)
	gpcc_fatal(FLINE, "config file error: ma.0 not set");

    /* ma.datadir */
    v = config_get(gpma.cfg, "ma.datadir");
    if (! v)
	gpcc_fatal(FLINE, "config file error: ma.datadir not set");
    if (v[0] != '/') 
	gpcc_fatal(FLINE, "config file error: ma.datadir must be an absolute path");
    if (strlen(v) > 255)
	gpcc_fatal(FLINE, "config file error: ma.datadir is too long (max 255 chars)");

    /* qd must exist */
    v = config_get(gpma.cfg, "gpdb.seg.m.0");
    if (!v) {
	gpcc_fatal(FLINE, "config file error: gpdb.seg.m.0 not set");
    }

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "gpdb.seg.%d.0", i);
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
    config_set_int(gpma.cfg, "gpdb.seg.max", i);
    
    return 0;
}




/*
 * Load the config into gpma. 
 */
apr_status_t load_config(void)
{
    if (gpma.boot_fname) {
	apr_uint64_t guid;
	apr_uint64_t rand;
	char guidstr[16 + 4];
	apr_time_t now = apr_time_now();
	FILE* f = (0 == strcmp(gpma.boot_fname, "-") ? stdin : fopen(gpma.boot_fname, "r"));
	if (!f) 
	    gpcc_fatal(FLINE, "unable to open boot config file: '%s'", gpma.boot_fname);
	gpma.cfg = config_read_stream(f, gpma.pool);
	fclose(f);

	/* generate a guid */
	guid = now;
	rand = random();
	rand <<= 32;
	rand |= random();
	guid ^= rand;
	sprintf(guidstr, "%04x-%04x-%04x-%04x", 
		0xffffU & (int) (guid >> 48), 0xffffU & (int) (guid >> 32),
		0xffffU & (int) (guid >> 16), 0xffffU & (int) (guid));
	gpma.guid = apr_pstrdup(gpma.pool, guidstr);
	config_set(gpma.cfg, "gpdb.guid", gpma.guid);

	config_set_int64(gpma.cfg, "ma.csn", 1);
	GPCC_DO(0 == verify_config());
	
	gpma.datadir = config_get(gpma.cfg, "ma.datadir");
    }
    else {
	gpma.cfg = config_read(gpma.datadir, gpma.pool);
	if (gpma.cfg == 0) {
	    fprintf(stderr, "ERROR: config file not found in dir %s\n", gpma.datadir);
	    exit(1);
	}
	gpma.guid = config_get(gpma.cfg, "gpdb.guid");
	if (! gpma.guid) {
	    fprintf(stderr, "ERROR: config file does not specify gpdb.guid\n");
	    exit(1);
	}
	gpma.guid = apr_pstrdup(gpma.pool, gpma.guid);
	GPCC_DO(0 == verify_config());
    }

    if (0 != apr_dir_make_recursive(gpma.datadir, 0x700, gpma.pool)) 
	gpcc_fatal(FLINE, "unable to create ma.datadir '%s'", gpma.datadir);
    GPCC_DO(0 == config_write(gpma.datadir, gpma.guid, gpma.cfg));
    return 0;
}

int main(int argc, const char* argv[])
{
    GPCC_DO(0 == apr_app_initialize(&argc, (char const* const **) &argv, NULL));
    atexit(apr_terminate);

    gpma.progname = argv[0];

    srandom((unsigned long) apr_time_now());

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
