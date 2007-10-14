#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_net.h>
#include <gpcc_spin.h>
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
				      cmdstate_t init_state, gpcc_net_conn_t conn)
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
	cmdslot->conn = conn;
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


/* callback from gpcc_net. this runs in the network thread */
static void net_opened_cb(gpcc_net_conn_t conn,
			  const char *peer_addr,
			  gpcc_net_header_t *to_peer,
			  gpcc_net_header_t *from_peer,
			  apr_uint64_t arg)
{
    /* NOT IMPLEMENTED */
    gpcc_log_info("net_opened_cb");
    return;
}


/* callback from gpcc_net. this runs in the network thread */
static void net_accepted_cb(gpcc_net_conn_t conn,
			    const char *peer_addr,
			    gpcc_net_header_t *from_peer)
{
    int i, e;
    gpcc_net_header_t hdr;
    cmdslot_t *cmdslot;

    gpcc_log_info("net_accepted_cb");
    if (from_peer->argc != 1 || strcmp(from_peer->argv[0], GPCC_NET_PACKET_PROTO) != 0){
	gpcc_warning(FLINE, "peer header not match!");
	gpcc_net_close(conn);
	return;
    }

    for (i = 0 ; i < from_peer->hc ; i++) {
	if (apr_strnatcasecmp(from_peer->hname[i], "X-GPCC-ROLE") == 0) {
	    if (strcmp(from_peer->hvalue[i], "GPMA") == 0) {
		/* create cmdslot */

		cmdslot = cmdslot_alloc("MA->HA", GET_IDENTITY_CMDSTATE, conn);
		gpcc_net_set_arg(conn, cmdslot->cmdidx);
		gpcc_log_info("create MA socket...%d", cmdslot->cmdidx);

		hdr.argc = 1;
		hdr.argv[0] = GPCC_NET_PACKET_PROTO;
		hdr.hc = 0;
		if (0 != (e = gpcc_net_send_header(conn, &hdr))) {
		    gpcc_warning(FLINE, "gpcc_net_send_header failed");
		}
		return;
	    }
	}
    }
}

/* callback from gpcc_net. this runs in the network thread */
static int net_recv_cb(gpcc_net_conn_t conn,
		       const char *buf, int buflen, apr_uint64_t arg)
{
    /* NOT IMPLEMENTED */
    gpcc_log_info("net_recv_cb");
    return 0;
}
			  
/* callback from gpcc_net. this runs in the network thread */
static void net_recv_packet_cb(gpcc_net_conn_t conn,
			       gpcc_net_packet_t *pkt,
			       apr_uint64_t arg)
{
    gpcc_event_t *ev;
    apr_uint32_t cmdidx = (apr_uint32_t) arg;

    if (cmdidx) {
	ev = gpcc_event_alloc(pkt->mh.type, cmdidx, sizeof(*pkt) + pkt->mh.len);
	memcpy(ev->pdata, pkt, sizeof(*pkt) + pkt->mh.len);
	gpcc_evqueue_put(gpha.evq, ev);
    }
}

/* callback from gpcc_net. this runs in the network thread */
static void net_closed_cb(gpcc_net_conn_t conn, apr_uint64_t arg)
{
    gpcc_event_t *ev;
    apr_uint32_t cmdidx = (apr_uint32_t) arg;

    if (cmdidx) {
	ev = gpcc_event_alloc(NET_CLOSED_EVENT, cmdidx, 0);
	gpcc_evqueue_put(gpha.evq, ev);
    }

}

/* callback from gpcc_net. this runs in the network thread */
static void net_tick_cb(apr_time_t now)
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
    setlinebuf(stdout);

    /* set up the fsm */
    fsm_setup();

    /* greetings */
    apr_rfc822_date(date, apr_time_now());
    gpcc_log_info("[%s] started gpha %s", date, gpha.dir);

    /* network up */
    GPCC_DO(0 == gpcc_net_init(gpha.port,
			       net_opened_cb,
			       net_accepted_cb,
			       net_recv_cb,
			       net_recv_packet_cb,
			       net_closed_cb,
			       net_tick_cb));

    gpcc_log_info("gpha listening to port %d", gpha.port);

    cp = cmdslot_alloc("SYSTEM", SYSTEM_CMDSTATE, 0);
    gpha.system_cmdidx = cp->cmdidx;


    for (;;) {
	int nxtstate;
	gpcc_fsm_func_t** action;

	if (ev) 
	    gpcc_event_free(ev);

	while ((ev = gpcc_evqueue_tryget(evq)) == 0) {
	    if (gpcc_net_once(1))
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
    fprintf(stderr, "\nUsage: %s [-?bD] [-p port] dir\n\n", program);
    fprintf(stderr, "    -?   : print this help screen\n");
    fprintf(stderr, "    -b   : boot the Host Agent\n");
    fprintf(stderr, "    -D   : debug mode and do not run as daemon\n");
    fprintf(stderr, "    -p   : port number\n");
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
    int e;
    FILE* f;
    
    GPCC_DO(0 == apr_getopt_init(&handle, gpha.pool, argc, argv));
    while (0 == (e = apr_getopt(handle, "?bDp:", &opt, &optarg))) {
	switch (opt) {
	case 'b': 
	    gpha.boot_flag = 1;
	    break;
	case 'D':
	    gpha.debug = 1;
	    break;
	case 'p':
	    gpha.port = strtol(optarg, 0, 0);
	    break;
	case '?':
	    usage(argv[0], 0);
	    break;
	default:
	    usage(argv[0], 1);
	    break;
	}
    }

    if (e != APR_EOF) 
	usage(argv[0], 1);
    if (argc - handle->ind != 1)
	usage(argv[0], 1);
    gpha.dir = argv[handle->ind++];

    if (gpha.port == 0) {
	gpha.port = 53333;
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

    gpha.logfpath = apr_psprintf(gpha.pool, "%s/gpha.log", gpha.dir);
    f = fopen(gpha.logfpath, "a");
    if (!f) {
	fprintf(stderr, "unable to create log file %s\n", gpha.logfpath);
	exit(1);
    }
    fclose(f);
    
    return 0;
}


int main(int argc, const char* argv[])
{
    GPCC_DO(0 == apr_app_initialize(&argc, (char const* const **) &argv, NULL));
    atexit(apr_terminate);

    GPCC_DO(0 == apr_pool_create(&gpha.pool, 0));

    GPCC_DO(0 == parse_cmdline(argc, argv));

    GPCC_DO(0 != (gpha.gphome = getenv("GPHOME")));

    if (! gpha.debug) {
	GPCC_DO(0 == apr_proc_detach(1));
	/* At this point, we are now a daemon */
    }

    gpha.debug = 0;

    d_main();
    return 0;
}
