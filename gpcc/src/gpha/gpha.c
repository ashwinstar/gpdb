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

#include "gpha.h"

gpha_t gpha = {0};

/* Match a cmdidx to a cmdslot */
static cmdslot_t* cmdslot_get(int cmdidx)
{
    const int idx = cmdidx & 0xffff;
    if (0 <= idx && idx < CMDSLOT_MAX) {
	cmdslot_t* cmdslot = &gpha.cmdslot[idx];
	if (cmdslot->cmdidx == cmdidx)
	    return cmdslot;
    }
    return 0;
}



/* Find a free cmdslot */
APR_DECLARE(cmdslot_t*) cmdslot_alloc(const char* name, 
				      cmdstate_t init_state, conninfo_t* conninfo)
{
    int i;
    cmdslot_t* cmdslot = 0;
    int seq;
    gpcc_event_t* ev;

    for (i = 0; i < CMDSLOT_MAX && gpha.cmdslot[i].cmdidx; i++);
    if (i < CMDSLOT_MAX) {
	cmdslot = &gpha.cmdslot[i];
	memset(cmdslot, 0, sizeof(*cmdslot));
	seq = (((int)gpha.cmdidx_gen++) << 16) & 0x80000000;
	if (! seq)
	    seq = 1 << 16;	/* ensure that seq is never 0 */
	cmdslot->cmdidx = seq | i;
	cmdslot->cmdstate = init_state;
	cmdslot->conninfo = conninfo;
	apr_pool_create(&cmdslot->pool, 0);
	cmdslot->cmdname = (name ? apr_pstrdup(cmdslot->pool, name) : 0);

	ev = gpcc_event_alloc(INIT_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpha.evq, ev);
    }

    return cmdslot;
}


APR_DECLARE(void) cmdslot_free(cmdslot_t* cp)
{
    if (cp->pool) apr_pool_destroy(cp->pool);
    memset(cp, 0, sizeof(*cp));
}






#ifdef UNDEF
static apr_status_t get_proto_cancel(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* cmdslot = cmdslot_;
    gpcc_warning(FLINE, "GET_PROTO cancelled");
    gpcc_net_close(cmdslot->conninfo->conn);
    return 0;
}



static apr_status_t get_proto_received(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* cmdslot = cmdslot_;
    net_msg_event_t* res = (net_msg_event_t*) ev->edata;
    struct json_object* dict;
    const char* protocol = 0;
    const char* array_name = 0;
    const char* guid = 0;
    const char* peer_id = 0;
    const char* role = 0;
    apr_pool_t* pool = cmdslot->conninfo->pool;

    GPCC_DO(ev->etype == PROTO_MSG);
    dict = json_tokener_parse(res->mbody);
    if (is_error(dict)) {
	gpcc_warning(FLINE, "peer protocol packet not valid json");
	queue_failure_event(cmdslot);
	return APR_EINVAL;
    }

    protocol = json_object_object_get_string(dict, "protocol");
    if (!protocol || 0 != strcmp(protocol, GPCC_NET_PROTO))  {
	gpcc_warning(FLINE, "peer protocol error");
	goto fail;
    }

    guid = json_object_object_get_string(dict, "guid");
    if (!guid || 0 != strcmp(guid, gpha.guid)) {
	gpcc_warning(FLINE, "peer guid mismatch");
	goto fail;
    }
    
    array_name = json_object_object_get_string(dict, "array_name");
    if (!array_name || 0 != strcmp(array_name, config_get(gpha.cfg, "sys.array_name"))) {
	gpcc_warning(FLINE, "peer sys.array_name mismatch");
	goto fail;
    }

    peer_id = json_object_object_get_string(dict, "peer_id");
    if (!peer_id) {
	gpcc_warning(FLINE, "missing peer_id");
	goto fail;
    }
    cmdslot->conninfo->peer_id = apr_pstrdup(pool, peer_id);

    role = json_object_object_get_string(dict, "role");
    if (!role) {
	gpcc_warning(FLINE, "missing peer role");
	goto fail;
    }
    if (0 == strcmp("ma", role))
	cmdslot->conninfo->peer_role = MA_ROLE;
    else if (0 == strcmp("ha", role))
	cmdslot->conninfo->peer_role = HA_ROLE;
    else if (0 == strcmp("qd", role))
	cmdslot->conninfo->peer_role = QD_ROLE;
    else if (0 == strcmp("qe", role))
	cmdslot->conninfo->peer_role = QE_ROLE;
    else {
	gpcc_warning(FLINE, "unknown peer role");
	goto fail;
    }

    json_object_put(dict);

    return 0;

    fail:
    queue_failure_event(cmdslot);
    return APR_EINVAL;
}


static apr_status_t system_send_proto(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* cmdslot = cmdslot_;
    net_msg_event_t* req = (net_msg_event_t*) ev->edata;
    gpcc_net_node_id_t nodeid;
    char nodeidstr[sizeof(nodeid.opaque) * 2 + 1];
    gpcc_proto_msghdr_t replyhdr;
    int i;
    char buffer[1024];
    

    nodeid = gpcc_net_my_node_id();
    for (i = 0; i < sizeof(nodeid.opaque); i++)
	sprintf(nodeidstr + i*2, "%x", nodeid.opaque[i]);

    buffer[sizeof(buffer)-2] = 0;
    apr_snprintf(buffer, sizeof(buffer), "{ "
		 "protocol: '%s', "
		 "array_name: '%s', "
		 "guid: '%s', "
		 "peer_id: '%s', "
		 "role: '%s' }", 
		 GPCC_NET_PROTO,
		 config_get(gpha.cfg, "sys.array_name"),
		 config_get(gpha.cfg, "sys.guid"),
		 nodeidstr, 
		 "ma");
    GPCC_DO(buffer[sizeof(buffer)-2] == 0); /* make sure buffer not overflowed */

    GPCC_NET_MSGHDR_FILL(&replyhdr, PROTO_MSG, strlen(buffer),
			 req->mhdr.rcvidx, req->mhdr.sndidx);
    if (gpcc_net_send(cmdslot->conninfo->conn, &replyhdr, buffer)) {
	gpcc_net_close(cmdslot->conninfo->conn);
	return 0;
    }
    return 0;
}
#endif








/* callback from gpcc_net. this runs in the network thread */
static void* net_opened_cb(gpcc_net_conn_t conn, void* arg, 
			    int self_origin, apr_pool_t* pool)
{
    conninfo_t* conninfo;
    eventinfo_t* ei;
    gpcc_event_t* ev = gpcc_event_alloc(NET_OPENED_EVENT, 0, sizeof(*ei));

    GPCC_DO(arg == 0);
    conninfo = apr_pcalloc(pool, sizeof(*conninfo));
    conninfo->pool = pool;
    conninfo->conn = conn;
    conninfo->self_origin = self_origin;
    ei = (eventinfo_t*) ev->edata;
    ei->conninfo = conninfo;
    gpcc_evqueue_put(gpha.evq, ev);
    return conninfo;
}

/* callback from gpcc_net. this runs in the network thread */
static void net_closed_cb(gpcc_net_conn_t conn, void* arg)
{
    eventinfo_t* ei;
    gpcc_event_t* ev = gpcc_event_alloc(NET_CLOSED_EVENT, 0, sizeof(*ei));
    ei = (eventinfo_t*) ev->edata;
    ei->conninfo = arg;
    gpcc_evqueue_put(gpha.evq, ev);
}


/* callback from gpcc_net. this runs in the network thread */
static void net_recv_cb(gpcc_net_conn_t conn, void* arg)
{
    eventinfo_t* ei;
    gpcc_event_t* ev = gpcc_event_alloc(NET_RECEIVED_EVENT, 0, sizeof(*ei));
    ei = (eventinfo_t*) ev->edata;
    ei->conninfo = arg;
    gpcc_evqueue_put(gpha.evq, ev);
}

/* callback from gpcc_net. this runs in the network thread */
static void time_tick_cb(apr_time_t now)
{
    gpcc_event_t* ev = gpcc_event_alloc(TICK_EVENT, 0, 0);
    gpcc_evqueue_put(gpha.evq, ev);
}



/* Main program for the daemon */
static void d_main(void)
{
    char date[APR_RFC822_DATE_LEN];
    gpcc_event_t* ev = 0;
    gpcc_evqueue_t* evq;
    cmdslot_t* cp;

    event_init();

    /* create the event queue */
    evq = gpha.evq = gpcc_evqueue_create(1000, gpha.pool);

    /* redirect cout and cerr to log */
    freopen(gpha.logfpath, "a", stderr);
    setlinebuf(stderr);
    freopen(gpha.logfpath, "a", stdout);

    /* set up the fsm */
    fsm_setup();

    /* greetings */
    apr_rfc822_date(date, apr_time_now());
    gpcc_log_info("[%s] started gpha %s %s %s", date, gpha.guid, gpha.maid, gpha.dir);

    /* network up */
    GPCC_DO(0 == gpcc_net_start(config_get_int(gpha.cfg, "sys.ha.port"), 
				net_opened_cb,
				net_closed_cb,
				net_recv_cb,
				time_tick_cb, 0));

    gpcc_log_info("gpha listening to port %d", config_get_int(gpha.cfg, "sys.ha.port"));

    cp = cmdslot_alloc("SYSTEM", SYSTEM_CMDSTATE, 0);
    gpha.system_cmdidx = cp->cmdidx;

    for (;;) {
	int nxtstate;
	gpcc_fsm_func_t** action;

	if (ev) 
	    gpcc_event_free(ev);

	while ((ev = gpcc_evqueue_tryget(evq)) == 0) {
	    if (gpcc_net_once())
		break;
	}

	/* map the cmdidx */
	if (ev->cmdidx == 0) 	/* artificially match 0 to system_cmdidx */
	    ev->cmdidx = gpha.system_cmdidx;

	if (! (cp = cmdslot_get(ev->cmdidx))) {
	    gpcc_log_info("Event %s contains unknown cmdidx %x", 
			  event_str(ev->etype), ev->cmdidx);
	    continue;
	}

	/* retrieve fsm record */
	if (0 != gpcc_fsm_get(cp->cmdstate, ev->etype, &nxtstate, &action)) {
	    gpcc_log_info("FSM[%d]: %s + %s not found", cp->cmdidx,
			  cmdstate_str(cp->cmdstate), 
			  event_str(ev->etype));
	    continue;
	}

	/* dispatch actions */
	for ( ; *action && 0 == (*action)(ev, cp); action++);

	/* effect the state transition */
	if (*action == 0) {
	    if (gpha.debug) {
		gpcc_log_info("FSM[%d]: %s + %s = %s", cp->cmdidx,
			      cmdstate_str(cp->cmdstate), 
			      event_str(ev->etype), 
			      cmdstate_str(nxtstate));
	    }
	    cp->cmdstate = nxtstate;
	}
	else {
	    gpcc_warning(FLINE, "FSM[%d] FAILED: cancelled %s + %s = %s ", cp->cmdidx,
			 cmdstate_str(cp->cmdstate), 
			 event_str(ev->etype), 
			 cmdstate_str(nxtstate));

	    gpcc_evqueue_put(gpha.evq, 
			     gpcc_event_alloc(FAILURE_EVENT, cp->cmdidx, 0));
	}

	/* free up the cmdslot if necessary */
	if (cp->cmdstate == NULL_CMDSTATE)
	    cmdslot_free(cp);
    }

    if (ev) {
	gpcc_event_free(ev);
    }

    gpcc_log_info("d_main exiting");
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

    GPCC_DO(0 == apr_getopt_init(&handle, gpha.pool, argc, argv));
    while (0 == (e = apr_getopt(handle, "?bD", &opt, &optarg))) {
	switch (opt) {
	case 'b': 
	    gpha.boot_flag = 1;
	    break;
	case 'D':
	    gpha.debug = 1;
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
    gpha.guid = argv[handle->ind++];
    gpha.maid = argv[handle->ind++];
    gpha.dir = argv[handle->ind++];

    if (strlen(gpha.guid) != 17 || gpha.guid[8] != '-') {
	gpcc_log_error("invalid guid in command");
	exit(1);
    }
    for (i = 0; i < 17; i++) {
	int c = gpha.guid[i];
	if (i != 8 && ! ('0' <= c && c <= '9')) {
	    gpcc_log_error("invalid guid in command");
	    exit(1);
	}
    }

    gpha.maid = apr_pstrcat(gpha.pool, "ma.", gpha.maid, NULL);
    if (0 != strcmp(gpha.maid, "ma.0") 
	&& 0 != strcmp(gpha.maid, "ma.1")
	&& 0 != strcmp(gpha.maid, "ma.2")) {
	gpcc_log_error("invalid master agent id in command");
	exit(1);
    }

    if (strlen(gpha.dir) == 0) {
	fprintf(stderr, "invalid dir parameter\n");
	exit(1);
    }

    if (gpha.dir[0] != '/') {
	fprintf(stderr, "dir must be an absolute path\n");
	exit(1);
    }
    
    if (0 != apr_dir_make_recursive(gpha.dir, 0x700, gpha.pool))  {
	fprintf(stderr, "unable to create dir %s\n", gpha.dir);
	exit(1);
    }

    gpha.logfpath = apr_psprintf(gpha.pool, "%s/gpha_%s.log", gpha.dir, gpha.guid);
    f = fopen(gpha.logfpath, "a");
    if (!f) {
	fprintf(stderr, "unable to create log file %s\n", gpha.logfpath);
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
    if (gpha.csn != config_get_int64(gpha.cfg, "sys.csn")) {
	gpcc_fatal(FLINE, "config file error: csn mismatch (expect %lld, receive %lld)",
		   gpha.csn, config_get_int64(gpha.cfg, "sys.csn"));
    }

    /* guid must match */
    if (! config_eq(gpha.cfg, "sys.guid", gpha.guid)) {
	gpcc_fatal(FLINE, "config file error: guid mismatch (expect %s, receive %s)",
		       gpha.guid, config_getx(gpha.cfg, "sys.guid", "[null]"));
    }

    /* name must exist */
    if (! config_get(gpha.cfg, "sys.array_name")) {
	gpcc_fatal(FLINE, "config file error: sys.array_name missing");
    }

    /* default ma port if not set */
    i = config_get_or_set_int(gpha.cfg, "sys.ma.port", DEFAULT_MA_PORT);
    if (! (0 < i && i < 65000)) {
	gpcc_fatal(FLINE, "config file error: sys.qd.port invalid (value %d)", i);
    }

    /* default qd port if not set */
    i = config_get_or_set_int(gpha.cfg, "sys.qd.port", DEFAULT_QD_PORT);
    if (! (0 < i && i < 65000)) {
	gpcc_fatal(FLINE, "config file error: sys.qd.port invalid (value %d)", i);
    }

    /* default qe baseport if not set */
    i = config_get_or_set_int(gpha.cfg, "sys.qe.baseport", DEFAULT_QE_BASEPORT);
    if (! (0 < i && i < 65000)) {
	gpcc_fatal(FLINE, "config file error: sys.qe.baseport invalid (value %d)", i);
    }

    /* default qd max connect if not set */
    i = config_get_or_set_int(gpha.cfg, "sys.qd.max_connect", DEFAULT_QD_MAX_CONNECT);
    if (! (0 < i && i <= 512)) {
	gpcc_fatal(FLINE, "config file error: sys.qd.max_connect invalid (value %d)", i);
    }

    /* default qe max connect if not set */
    i = config_get_or_set_int(gpha.cfg, "sys.qe.max_connect", DEFAULT_QE_MAX_CONNECT);
    if (! (0 < i && i <= 512)) {
	gpcc_fatal(FLINE, "config file error: sys.qe.max_connect invalid (value %d)", i);
    }

    /* default min mirror if not set */
    i = config_get_or_set_int(gpha.cfg, "sys.min_mirror", 0);
    if (! (0 <= i && i < 2)) {
	gpcc_fatal(FLINE, "config file error: sys.qe.min_mirror invalid (value %d)", i);
    }

    /* default qd max buffer if not set */
    v = config_get_or_set(gpha.cfg, "sys.qd.max_buffer", DEFAULT_QD_MAX_BUFFER);
    if (gpcc_string_parse_memory(v) == 0) {
	gpcc_fatal(FLINE, "config file error: sys.qd.max_buffer invalid (value %s)", v);
    }
	
    /* default qe max buffer if not set */
    v = config_get_or_set(gpha.cfg, "sys.qe.max_buffer", DEFAULT_QE_MAX_BUFFER);
    if (gpcc_string_parse_memory(v) == 0) {
	gpcc_fatal(FLINE, "config file error: sys.qe.max_buffer invalid (value %s)", v);
    }

    /* all 3 ma must be set */
    for (i = 0; i < 3; i++) {
	sprintf(buf, "ma.%d", i);
	if (! (v = config_get(gpha.cfg, buf)))
	    gpcc_fatal(FLINE, "config file error: %s not set", buf);
    }

    /* qd must exist */
    v = config_get(gpha.cfg, "seg.m.0");
    if (!v) {
	gpcc_fatal(FLINE, "config file error: seg.m.0 not set");
    }

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "seg.%d.0", i);
	v = config_get(gpha.cfg, buf);
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
    config_set_int(gpha.cfg, "seg.max", i);
    
    return 0;
}


/*
 * Load the config into gpha. 
 */
static apr_status_t load_config(void)
{
    if (gpha.boot_flag) {
	/* read config from stdin */
	gpha.cfg = config_read_stream(stdin, gpha.pool);
	gpha.csn = 0;
	config_set_int64(gpha.cfg, "sys.csn", gpha.csn);
	GPCC_DO(0 == verify_config());

	++gpha.csn;
	config_set_int64(gpha.cfg, "sys.csn", gpha.csn);
	GPCC_DO(0 == config_write(gpha.dir, gpha.guid, gpha.csn, gpha.cfg));
    }
    else {
	gpha.csn = config_determine_csn(gpha.dir, gpha.guid);
	gpha.cfg = config_read(gpha.dir, gpha.guid, gpha.csn, gpha.pool);
	if (gpha.cfg == 0) {
	    fprintf(stderr, "ERROR: config file not found in dir %s\n", gpha.dir);
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

    GPCC_DO(0 == apr_pool_create(&gpha.pool, 0));

    GPCC_DO(0 == parse_cmdline(argc, argv));

    GPCC_DO(0 == load_config());

    GPCC_DO(0 == (gpha.gphome = getenv("GPHOME")));

    if (! gpha.debug) {
	GPCC_DO(0 == apr_proc_detach(1));
	/* At this point, we are now a daemon */
    }

    d_main();
    return 0;
}
