#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_fsm.h>
#include <gpcc_json.h>
#include <json/json.h>
#include <event.h>
#include <ctype.h>

#include "cmdslot.h"
#include "gpma.h"

gpma_t gpma = {0};


/* Print usage info */
static void usage(const char* msg)
{
    fprintf(stderr, "\nUsage: %s [-D?] [-b cfgfile] ma_unit ma_dir\n\n", gpma.progname);
    fprintf(stderr, "    -?         : print this help screen\n");
    fprintf(stderr, "    -D         : debug mode and do not run as daemon\n");
    fprintf(stderr, "    -b cfgfile : boot the Master Agent with cfgfile ('-' for stdin)\n");
    fprintf(stderr, "    ma_unit    : the Master Agent Unit (e.g. ma.1) \n");
    fprintf(stderr, "    ma_dir     : the data directory for storing Master Agent data files\n");
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
void parse_cmdline(int argc, const char* argv[])
{
    apr_getopt_t* handle;
    char opt;
    const char* optarg;
    int e;
    FILE* f;
    char msg[1024];
    char* p;

    switch (argv[0][0]) {
    case '/': case '.': 
	break;
    default:
	sprintf(msg, "please specify %s with an explicit full or relative path", argv[0]);
	usage(msg);
    }

    /* get full path to gpma executable */
    apr_filepath_merge(&p, NULL, argv[0],
		       APR_FILEPATH_TRUENAME | APR_FILEPATH_NOTRELATIVE, 
		       gpma.pool);

    /* get full path to parent directory of gpma executable */
    gpma.gphome = p;
    setenv("GPHOME", p, 1);

    p += strlen(p) + 1;
    while (*p != '/') *p-- = 0;
    if (p == gpma.gphome) {
	sprintf(msg, "unable to set get GPHOME from path %s", gpma.gphome);
	usage(msg);
    }
    *p-- = 0;
    while (*p != '/') *p-- = 0;

    /* parse command line options */
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

    if (argc - handle->ind != 2)
	usage("Please specify ma_unit ma_dir");

    gpma.ma_unit = argv[handle->ind++];
    gpma.datadir = argv[handle->ind++];

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
}

static void net_opened(gpcc_net_conn_t conn, 
		       const char* peer_addr,
		       gpcc_net_header_t* to_peer, 
		       gpcc_net_header_t* from_peer,
		       apr_uint64_t arg)
{
    apr_uint32_t cmdidx = (apr_uint32_t) arg;
    gpcc_event_t *ev;

    if (from_peer->argc != 1 || strcmp(from_peer->argv[0], GPCC_NET_PACKET_PROTO) != 0) {
	gpcc_net_close(conn);
	return;
    }

    gpcc_log_info("net_opened...");
    
    ev = gpcc_event_alloc(NET_OPENED_EVENT, cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
    return;
}


static gpcc_event_t *mkfts_event(event_t event, gpcc_net_conn_t conn, apr_uint32_t cmdidx, apr_uint32_t sndidx, apr_uint32_t rcvidx, char *msg, int len)
{
    gpcc_event_t *ev;

    ev = gpcc_event_alloc(event, cmdidx, sizeof(gpcc_net_conn_t) + len);
    memcpy(ev->pdata, &conn, sizeof(conn));
    memcpy(ev->pdata+sizeof(conn), msg, len);
    return ev;

}
static void net_accepted(gpcc_net_conn_t conn,
			 const char* peer_addr,
			 gpcc_net_header_t* from_peer,
			 apr_uint64_t *arg)
{
    gpcc_event_t* ev;

    if (from_peer->argc == 3
	&& 0 == memcmp("HTTP/1.", from_peer->argv[2], 7)
	&& 0 == memcmp("GET", from_peer->argv[0], 4)) {
	
	if (0 == memcmp("/gpcc/start?", from_peer->argv[1], 12)) {
	    /* send startcmd_event to FTS cmdslot ? */
	    ev = mkfts_event(ARRAY_START_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
			     gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
	    gpcc_evqueue_put(gpma.evq, ev);
	}
	else if (0 == memcmp("/gpcc/stop?", from_peer->argv[1], 11)) {
	    ev = mkfts_event(ARRAY_STOP_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
			     gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
	    gpcc_evqueue_put(gpma.evq, ev);
	}
	else if (0 == memcmp("/gpcc/init?", from_peer->argv[1], 11)) {
	    ev = mkfts_event(ARRAY_INIT_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
			     gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
	    gpcc_evqueue_put(gpma.evq, ev);
	}
	else if (0 == memcmp("/gpcc/status", from_peer->argv[1], 12)) {
	    ev = mkfts_event(ARRAY_STATUS_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
			     gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
	    gpcc_evqueue_put(gpma.evq, ev);
	} else {
	    gpcc_warning(FLINE, "invalid URL");
	    gpcc_net_close(conn);
	}
	return;
    }
    else if (from_peer->argc == 1) {
	/* check if it is GPCC_NET_PACKET_PROTO */
    }
    /* unknown protocol - reject the connection */
    gpcc_net_close(conn);
}


static int net_received(gpcc_net_conn_t conn, 
			const char* buf, int buflen,
			apr_uint64_t arg)
{
    /* should not invoke, yet */
    gpcc_warning(FLINE, "net_received should not get called");
    return -1;
}


static void net_received_packet(gpcc_net_conn_t conn, 
				gpcc_net_packet_t* pkt,
				apr_uint64_t arg)
{
    gpcc_event_t *ev;
    apr_uint32_t cmdidx = pkt->mh.rcvidx;

    if (cmdidx == 0) {
	/* GPMA */
	cmdidx = gpma.gpma_cmdslot->cmdidx;
    } 

    ev = gpcc_event_alloc(pkt->mh.type, cmdidx, sizeof(*pkt) + pkt->mh.len);
    memcpy(ev->pdata, pkt, sizeof(*pkt)+pkt->mh.len);
    gpcc_evqueue_put(gpma.evq, ev);
}


static void net_closed(gpcc_net_conn_t conn, apr_uint64_t arg)
{
    apr_uint32_t cmdidx = (apr_uint32_t) arg;
    gpcc_event_t *ev;

    if (! cmdidx ) {
	/* HTTP request */
	gpcc_warning(FLINE, "HTTP connection closed by peer");
	return;
    }
    ev = gpcc_event_alloc(NET_CLOSED_EVENT, cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}


static void net_tick(apr_time_t now)
{
    gpcc_event_t* ev = gpcc_event_alloc(TICK_EVENT, gpma.gpma_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}


static void node_init(void)
{
    char node_name[100];
    const char* node;
    int cnt, i;
    cmdslot_t* cp;

    for (cnt = 0; ; cnt++) {
	sprintf(node_name, "node.%d", cnt);
	node = config_get(gpma.cfg, node_name);
	if (!node) 
	    break;
    }

    if (cnt == 0) {
	gpcc_fatal(FLINE, "No node exists in configuration");
    }
    gpma.node_max = cnt;
    gpma.node_cmdtab = apr_palloc(gpma.pool, (cnt + 1) * sizeof(gpma.node_cmdtab[0]));
    memset(gpma.node_cmdtab, 0, (cnt + 1) * sizeof(gpma.node_cmdtab[0]));
    for (i = 0; i < cnt; i++) {
	sprintf(node_name, "node.%d", i);
	node = config_get(gpma.cfg, node_name);
	cp = gpma.node_cmdtab[i] = cmdslot_alloc(node_name, NODE_OFF_CMDSTATE);
	if (apr_tokenize_to_argv(node, &cp->u.node.hosttab, cp->pool)) {
	    gpcc_fatal(FLINE, "Illegal format for entry '%s' in configuration", node_name);
	}
	for (cp->u.node.hosttab_max = 0 ; 
	     cp->u.node.hosttab[cp->u.node.hosttab_max] ; 
	     cp->u.node.hosttab_max++) ;
	cp->u.node.hostidx = 0;
	    
    }
}




/* Main program for the daemon */
void d_main(void)
{
    char date[APR_RFC822_DATE_LEN];
    gpcc_evqueue_t* evq = 0;
    gpcc_event_t* ev = 0;
    int i;

    gpma.logfpath = apr_psprintf(gpma.pool, "%s/%s-%s.log", 
				 gpma.datadir, gpma.guid, gpma.ma_unit);

    /* redirect cerr to log */
    if (! freopen(gpma.logfpath, "a", stderr)) {
	gpcc_warning(FLINE, "unable to open log file '%s'", gpma.logfpath);
	gpma.logfpath = NULL;
    }
    else {
	printf("Logs will be written to %s\n", gpma.logfpath);
	setlinebuf(stderr);
	freopen(gpma.logfpath, "a", stdout);
	setlinebuf(stdout);
    }

    event_init();

    cmdslot_init(gpma.pool);
    gpma.gpma_cmdslot = cmdslot_alloc("GPMA", GPMA_CMDSTATE);
    gpma.fts_cmdslot = cmdslot_alloc("FTS", FTS_CMDSTATE);

    fsm_init();

    node_init();

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

    /* kick off all the nodes */
    for (i = 0; i < gpma.node_max; i++) {
	ev = gpcc_event_alloc(INIT_EVENT, gpma.node_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, ev);
    }
    ev = 0;
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
	    ev->cmdidx = gpma.gpma_cmdslot->cmdidx;

	/*
	if (ev->etype != TICK_EVENT)
	    printf("event %s, cmdidx %x\n", event_str(ev->etype), ev->cmdidx);
	*/

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
	if (*fn) {
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
    int errcnt = 0;
    char* errmsg[100];

    /* at least 1 ma must be set */
    for (i = 0; i < 3; i++) {
	sprintf(buf, "ma.%d", i);
	if (! (v = config_get(gpma.cfg, buf)))
	    break;
    }
    if (i == 0) 
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- ma.0 not set");

    /* default ma port if not set */
    if (! config_get(gpma.cfg, "ma.port")) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- ma.port missing");
    }

    /* ma.datadir */
    v = config_get(gpma.cfg, "ma.datadir");
    if (! v)
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- ma.datadir not set");
    if (v[0] != '/') 
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- ma.datadir must be an absolute path");
    if (strlen(v) > 255)
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- ma.datadir is too long (max 255 chars)");

    /* guid must match */
    if (! config_eq(gpma.cfg, "gpdb.guid", gpma.guid)) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.guid mismatch (expect %s, receive %s)",
					gpma.guid, config_getx(gpma.cfg, "gpdb.guid", "[null]"));
    }

    /* name must exist */
    if (! config_get(gpma.cfg, "gpdb.name")) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.name missing");
    }

    /* default master_port port if not set */
    if (! config_get(gpma.cfg, "gpdb.seg.master_port")) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.seg.master_port missing");
    }
    i = config_get_int(gpma.cfg, "gpdb.seg.master_port");
    if (! (0 < i && i < (1 << 16))) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.seg.master_port invalid (value %d)", i);
    }

    /* default qe baseport if not set */
    if (! config_get(gpma.cfg, "gpdb.seg.base_port")) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.seg.base_port missing");
    }
    i = config_get_int(gpma.cfg, "gpdb.seg.base_port");
    if (! (0 < i && i < (1 << 16))) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.seg.base_port invalid (value %d)", i);
    }

    /* default min mirror if not set */
    i = config_get_or_set_int(gpma.cfg, "gpdb.min_mirror", 0);
    if (! (0 <= i && i < 2)) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.min_mirror invalid (value %d)", i);
    }

    /* qd must exist */
    v = config_get(gpma.cfg, "gpdb.seg.m.0");
    if (!v) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- gpdb.seg.m.0 not set");
    }

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "gpdb.seg.%d.0", i);
	v = config_get(gpma.cfg, buf);
	if (!v) {
	    if (i < 2) 
		errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- %s not set", buf);
	    break;
	}
    }
    if (! (2 <= i && i < 512)) {
	errmsg[errcnt++] = apr_psprintf(gpma.pool, "\t- too many segments (max 512)");
    }

    /* fail out */
    if (errcnt) {
	gpcc_log_error("config file error:");
	for (i = 0; i < errcnt; i++)
	    gpcc_log_error("%s\n", errmsg[i]);

	gpcc_fatal(FLINE, "config file error - see log file '%s'", gpma.logfpath ? gpma.logfpath : "-");
    }

    /* set the number of QEs */
    config_set_int(gpma.cfg, "gpdb.seg.max", i);
    
    return 0;
}




/*
 * Load the config into gpma. 
 */
void load_config(void)
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
}



int main(int argc, const char* argv[])
{
    GPCC_DO(0 == apr_app_initialize(&argc, (char const* const **) &argv, NULL));
    atexit(apr_terminate);

    gpma.progname = argv[0];

    srandom((unsigned long) apr_time_now());

    GPCC_DO(0 == apr_pool_create(&gpma.pool, 0));

    parse_cmdline(argc, argv);

    GPCC_DO(0 == setenv("GPHOME", gpma.gphome, 1));
    printf("GPHOME=%s\n", gpma.gphome);
    
    load_config();

    if (! gpma.debug) {
	GPCC_DO(0 == apr_proc_detach(1));
	/* At this point, we are now a daemon */
    }

    d_main();
    return 0;
}
