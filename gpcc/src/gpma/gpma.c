#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_net.h>
#include <gpcc_fsm.h>

#define DEFAULT_QD_MAX_CONNECT  25
#define DEFAULT_QE_MAX_CONNECT  25
#define DEFAULT_QD_MAX_BUFFER   "128mb"
#define DEFAULT_QE_MAX_BUFFER   "128mb"
#define DEFAULT_MA_PORT   	12000
#define DEFAULT_QD_PORT   	12100
#define DEFAULT_QE_BASEPORT	12200

#define CMDSLOT_MAX    512

#include "config.h"
#include "enum.h"


/* connection info, this is stored in gpcc_net via the conn_arg */
typedef struct conninfo_t conninfo_t;
struct conninfo_t {
    apr_pool_t* pool;
    gpcc_net_conn_t conn;
    int    self_origin;
    role_t role;
    char*  rolespec;		/* for QE, "0.1", "m.0", etc
				   for MA, "lead" or "",
				*/
    const char* peer_id;
};




typedef struct conn_opened_event_t conn_opened_event_t;
struct conn_opened_event_t {
    conninfo_t*     conninfo;
};

typedef struct conn_closed_event_t conn_closed_event_t;
struct conn_closed_event_t {
    conninfo_t*     conninfo;
};

typedef struct conn_msg_event_t conn_msg_event_t;
struct conn_msg_event_t {
    conninfo_t*       conninfo;
    gpcc_net_msghdr_t hdr;
    char              data[1];
};



typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    int 	cmdidx;
    const char* cmdname;
    cmdstate_t  cmdstate;
    int         countdown;	/* if > 0, will count down to 0 
				   and generate a TIMEUP_EVENT */
    conninfo_t* conninfo;
    union {
	struct {
	    
	} a;
    } u;
};


/* Control block for GPMA */
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
    int          system_cmdidx; /* cmdidx of system FSM */
    apr_uint16_t cmdidx_gen;
    cmdslot_t    cmdslot[CMDSLOT_MAX];
} cb = {0};


/* Match a cmdidx to a cmdslot */
static cmdslot_t* cmdslot_get(int cmdidx)
{
    const int idx = cmdidx & 0xffff;
    if (0 <= idx && idx < CMDSLOT_MAX) {
	cmdslot_t* cmdslot = &cb.cmdslot[idx];
	if (cmdslot->cmdidx == cmdidx)
	    return cmdslot;
    }
    return 0;
}


/* Find a free cmdslot */
static cmdslot_t* cmdslot_alloc(const char* name, 
				cmdstate_t init_state, conninfo_t* conninfo)
{
    int i;
    cmdslot_t* cmdslot = 0;
    int seq;

    for (i = 0; i < CMDSLOT_MAX && cb.cmdslot[i].cmdidx; i++);
    if (i < CMDSLOT_MAX) {
	cmdslot = &cb.cmdslot[i];
	memset(cmdslot, 0, sizeof(*cmdslot));
	seq = (((int)cb.cmdidx_gen++) << 16) & 0x80000000;
	if (! seq)
	    seq = 1 << 16;	/* ensure that seq is never 0 */
	cmdslot->cmdidx = seq | i;
	cmdslot->cmdstate = init_state;
	cmdslot->conninfo = conninfo;
    }

    return cmdslot;
}


static void cmdslot_free(cmdslot_t* cp)
{
    memset(cp, 0, sizeof(*cp));
}


/* 
 *  This gets called every second. go thru all cmdslot to find anyone that 
 *  needs a TIMEUP EVENT.
 */
static apr_status_t system_tick(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* p = &cb.cmdslot[0];
    cmdslot_t* q = &cb.cmdslot[CMDSLOT_MAX];
    gpcc_event_t* nev;

    for ( ; p < q; p++) {
	if (p->cmdidx && p->countdown > 0) {
	    if (--p->countdown == 0) {
		nev = gpcc_event_alloc(TIMEUP_EVENT, p->cmdidx, 0);
		gpcc_evqueue_put(cb.evq, nev);
	    }
	}
    }
    return 0;
}


/** handle open request from remote node
 */
static apr_status_t system_conn_opened(gpcc_event_t* ev, void* cmdslot_)
{
    conn_opened_event_t* cev = (conn_opened_event_t*) ev->edata;
    conninfo_t* c = cev->conninfo;
    cmdslot_t* cmdslot;
    gpcc_net_msghdr_t msghdr;
    
    if (c->self_origin) {
	cmdslot = cmdslot_alloc("GET_PROTO", GET_PROTO_CMDSTATE, c);
	if (! cmdslot) {
	    gpcc_net_close(c->conn);
	    gpcc_warning(FLINE, "cmdslot overflowed");
	    return 0;
	}

	cmdslot->countdown = 5;

	GPCC_NET_MSGHDR_FILL(&msghdr, GET_PROTO_MSG, 0, cmdslot->cmdidx, 0);
	if (gpcc_net_send(c->conn, &msghdr, 0)) {
	    gpcc_net_close(c->conn);
	    return 0;
	}
    }

    return 0;
}


static apr_status_t cancel_get_proto(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* cmdslot = cmdslot_;
    gpcc_warning(FLINE, "GET_PROTO timeup");
    gpcc_net_close(cmdslot->conninfo->conn);
    return 0;
}


static apr_status_t reply_get_proto(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* cmdslot = cmdslot_;
    gpcc_net_node_id_t nodeid;
    char nodeidstr[sizeof(nodeid.opaque) * 2 + 1];
    gpcc_net_msghdr_t msghdr;
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
		 config_get(cb.cfg, "sys.name"),
		 config_get(cb.cfg, "sys.guid"),
		 nodeidstr, 
		 "ma");
    GPCC_DO(buffer[sizeof(buffer)-2] == 0); /* make sure buffer not overflowed */

    GPCC_NET_MSGHDR_FILL(&msghdr, PROTO_MSG, strlen(buffer), 0, 0);
    if (gpcc_net_send(cmdslot->conninfo->conn, &msghdr, buffer)) {
	gpcc_net_close(cmdslot->conninfo->conn);
	return 0;
    }
    return 0;
}



static void d_setup_fsm(void)
{
    gpcc_fsm_put(SYSTEM_CMDSTATE, TICK_EVENT, SYSTEM_CMDSTATE, 
		 system_tick, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, CONN_OPENED_EVENT, SYSTEM_CMDSTATE, 
		 system_conn_opened, 0);
    gpcc_fsm_put(GET_PROTO_CMDSTATE, GET_PROTO_MSG_EVENT, NULL_CMDSTATE,
		 reply_get_proto, 0);
    gpcc_fsm_put(GET_PROTO_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 cancel_get_proto, 0);
}




/* callback from gpcc_net. this runs in the network thread */
static void* conn_opened_cb(gpcc_net_conn_t conn, void* arg, 
			    int self_origin, apr_pool_t* pool)
{
    conn_opened_event_t* cev;
    conninfo_t* conninfo;
    gpcc_event_t* ev = gpcc_event_alloc(CONN_OPENED_EVENT, 0, sizeof(*cev));

    GPCC_DO(arg == 0);
    conninfo = apr_pcalloc(pool, sizeof(*conninfo));
    conninfo->pool = pool;
    conninfo->conn = conn;
    conninfo->self_origin = self_origin;

    cev = (conn_opened_event_t*) ev->edata;
    cev->conninfo = conninfo;
    gpcc_evqueue_put(cb.evq, ev);
    return conninfo;
}

/* callback from gpcc_net. this runs in the network thread */
static void conn_closed_cb(gpcc_net_conn_t conn, void* arg)
{
    conn_closed_event_t* cev;
    gpcc_event_t* ev = gpcc_event_alloc(CONN_CLOSED_EVENT, 0, sizeof(*cev));
    cev = (conn_closed_event_t*) ev->edata;
    cev->conninfo = arg;
    gpcc_evqueue_put(cb.evq, ev);
}


/* callback from gpcc_net. this runs in the network thread */
static void conn_recv_cb(gpcc_net_conn_t conn, void* arg,
			 const gpcc_net_msghdr_t* msghdr, 
			 const char* msgbody)
{
    conn_msg_event_t* cev;
    gpcc_event_t* ev = gpcc_event_alloc(NULL_EVENT, msghdr->rcvidx,
					sizeof(*cev) + msghdr->mlen - 1);
    cev = (conn_msg_event_t*) ev->edata;
    cev->conninfo = arg;
    cev->hdr = *msghdr;
    memcpy(cev->data, msgbody, msghdr->mlen);

    switch (msghdr->mtype) {
    case GET_PROTO_MSG: ev->etype = GET_PROTO_MSG_EVENT; break;
    case PROTO_MSG:     ev->etype = PROTO_MSG_EVENT; break;
    default:
	gpcc_event_free(ev);
	gpcc_warning(FLINE, "received unknown event: %d\n", msghdr->mtype);
	return;
    }

    gpcc_evqueue_put(cb.evq, ev);
}

/* callback from gpcc_net. this runs in the network thread */
static void time_tick_cb(apr_time_t now)
{
    gpcc_event_t* ev = gpcc_event_alloc(TICK_EVENT, 0, 0);
    gpcc_evqueue_put(cb.evq, ev);
}



/* Main program for the daemon */
static void d_main(void)
{
    char date[APR_RFC822_DATE_LEN];
    gpcc_event_t* ev = 0;
    gpcc_evqueue_t* evq;
    cmdslot_t* cp;

    /* create the event queue */
    evq = cb.evq = gpcc_evqueue_create(1000, cb.pool);

    /* redirect cout and cerr to log */
    freopen(cb.logfpath, "a", stderr);
    setlinebuf(stderr);
    freopen(cb.logfpath, "a", stdout);

    /* set up the fsm */
    d_setup_fsm();

    /* greetings */
    apr_rfc822_date(date, apr_time_now());
    gpcc_log_info("[%s] started gpma %s %s %s", date, cb.guid, cb.maid, cb.dir);

    /* network up */
    GPCC_DO(0 == gpcc_net_start(config_get_int(cb.cfg, "sys.ma.port"), 
				conn_opened_cb,
				conn_closed_cb,
				conn_recv_cb,
				time_tick_cb));

    gpcc_log_info("gpma listening to port %d", config_get_int(cb.cfg, "sys.ma.port"));

    cp = cmdslot_alloc("SYSTEM", SYSTEM_CMDSTATE, 0);
    cb.system_cmdidx = cp->cmdidx;

    for ( ; 0 != (ev = gpcc_evqueue_get(evq)); gpcc_event_free(ev)) {
	int nxtstate;
	gpcc_fsm_func_t** action;
	/* map the cmdidx */
	if (ev->cmdidx == 0) 	/* artificially match 0 to system */
	    ev->cmdidx = cb.system_cmdidx;
	if (! (cp = cmdslot_get(ev->cmdidx)))
	    continue;
	/* retrieve fsm record */
	if (0 != gpcc_fsm_get(cp->cmdstate, ev->etype, &nxtstate, &action)) {
	    continue;
	}
	/* dispatch actions */
	for ( ; *action && 0 == (*action)(ev, cp); action++);

	if (*action == 0) {
	    if (cb.debug) {
		gpcc_log_info("FSM[%d]: %s + %s = %s", cp->cmdidx,
			      cmdstate_str(cp->cmdstate), 
			      event_str(ev->etype), 
			      cmdstate_str(nxtstate));
	    }
	    cp->cmdstate = nxtstate;
	}
	else {
	}

	if (cp->cmdstate == NULL_CMDSTATE)
	    cmdslot_free(cp);
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
