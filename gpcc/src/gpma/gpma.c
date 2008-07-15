#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_fsm.h>
#include <gpcc_json.h>
#include <gpcc_proc.h>
#include <gpcc_file.h>
#include <gpcc_pg.h>
#include <json/json.h>
#include <event.h>
#include <ctype.h>

#include "cmdslot.h"
#include "gpma.h"

gpma_t gpma = {0};

static const char* verify_hostaddr(const char* addr);

/* Print usage info */
static void usage(const char* msg)
{
    fprintf(stderr, "\nUsage: %s [-vd?f] [-a array_name] [-s] [-c server_cert_path] [-k server_key_path] [-l logdir] [-p ma_port] -P px1 [-P px2 -P px3]\n\n", gpma.progname);
    fprintf(stderr, "    -?                 : print this help screen\n");
    fprintf(stderr, "    -v                 : verbose\n");
    fprintf(stderr, "    -d                 : run as daemon\n");
    fprintf(stderr, "    -a                 : array name\n");
    fprintf(stderr, "    -l logdir          : the log directory\n");
    fprintf(stderr, "    -p ma_port         : the MA port number\n");
    fprintf(stderr, "    -P pxN             : paxos instance\n");
    fprintf(stderr, "    -s                 : SSL support\n");
    fprintf(stderr, "    -c server_cert_path: server certificate file path\n");
    fprintf(stderr, "    -k server_key_path : server private key file path\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "e.g. %s -a gpdb -l /db/ma -P localhost:11000\n", gpma.progname);
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
    int i, e, len;
    char msg[1024];
    char* p;
    const char* v;

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
    while (0 == (e = apr_getopt(handle, "?vda:p:l:P:sc:k:", &opt, &optarg))) {
	switch (opt) {
	case 'v':
	    gpma.verbose = 1;
	    break;
	case 'd':
	    gpma.daemonize = 1;
	    break;
	case 'a':
	    gpma.array_name = optarg;
	    break;
	case 'p':
	    gpma.ma_port = strtol(optarg, 0, 0);
	    break;
	case 'l':
	    gpma.datadir = optarg;
	    break;
	case 'P':
	    if (gpma.px_max >= 5)
		usage("too many paxos instance");
	    gpma.px_addr[gpma.px_max] = optarg;
	    v = verify_hostaddr(gpma.px_addr[gpma.px_max]);
	    if (v) {
		usage("paxos instance format error");
	    }
	    gpma.px_max++;
	    break;
	case 's':
	    gpma.ssl_enabled = 1;
	    break;
	case 'c':
	    gpma.server_cert_path = optarg;
	    break;
	case 'k':
	    gpma.server_key_path = optarg;
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

    if (argc - handle->ind != 0)
	usage("bad command line");

    if (! gpma.array_name) {
	usage("Please specify array name");
    }

    if (! (0 < gpma.ma_port && gpma.ma_port < (1 << 16)))
	gpcc_fatal(FLINE, "Illegal ma port specification: %s", gpma.ma_port);
    
    /* set MA data directory to /tmp */
    if (! gpma.datadir) {
	gpma.datadir = apr_pstrdup(gpma.pool, "/tmp");
    }
    
    /* set ha data directory to /tmp */
    gpma.ha_datadir = apr_pstrdup(gpma.pool, "/tmp");

    /* set SSL */
    if (gpma.ssl_enabled) {
	if (!gpma.server_cert_path)
	    gpcc_fatal(FLINE, "please specify server certificate file path");
	if (!gpma.server_key_path)
	    gpcc_fatal(FLINE, "please specify server private key file path");
    }

    /* set px_address for chubby */
    if (gpma.px_max == 0)
	gpcc_fatal(FLINE, "at least one paxos instance");

    len = 0;
    for (i= 0; i < gpma.px_max ; i++) {
	len += strlen(gpma.px_addr[i]) + 1;
    }
    gpma.px_address = apr_palloc(gpma.pool, len);
    (* (char *)gpma.px_address) = 0;
    for (i=0; i< gpma.px_max ; i++) {
	strcat((char *) gpma.px_address, gpma.px_addr[i]);
	if (i < gpma.px_max - 1)
	    strcat((char *) gpma.px_address, ",");
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
	gpcc_net_close(conn, 0);
	return;
    }

    gpcc_log_info("net_opened...");
    
    ev = gpcc_event_alloc(NET_OPENED_EVENT, cmdidx, sizeof(gpcc_net_conn_t));
    memcpy(ev->pdata, &conn, sizeof(gpcc_net_conn_t));
    gpcc_evqueue_put(gpma.evq, ev);
    return;
}


static gpcc_event_t *mkhttp_event(event_t event, gpcc_net_conn_t conn, 
				  apr_uint32_t cmdidx, apr_uint32_t sndidx, apr_uint32_t rcvidx,
				  const char *msg, int len)
{
    gpcc_event_t *ev;
    ev = gpcc_event_alloc(event, cmdidx, sizeof(gpcc_net_conn_t) + len);
    memcpy(ev->pdata, &conn, sizeof(conn));
    memcpy(ev->pdata+sizeof(conn), msg, len);
    return ev;
}

static gpcc_event_t *mkhttp_event_more(event_t event, gpcc_net_conn_t conn, 
				       apr_uint32_t cmdidx, apr_uint32_t sndidx, apr_uint32_t rcvidx,
				       const char *datastr, const char *msg, int len)
{
    gpcc_event_t *ev;
    char *p;
    int slen = strlen(datastr)+1;
    ev = gpcc_event_alloc(event, cmdidx, sizeof(gpcc_net_conn_t) + sizeof(int) * 2 + slen + len);
    p = ev->pdata;
    memcpy(p, &conn, sizeof(conn));
    p += sizeof(conn);
    memcpy(p, &len, sizeof(int));
    p += sizeof(int);
    memcpy(p, msg, len);
    p += len;
    memcpy(p, &slen, sizeof(int));
    p += sizeof(int);
    memcpy(p, datastr, slen);
    

    return ev;
}

static void net_accepted(gpcc_net_conn_t conn,
			 const char* peer_addr,
			 gpcc_net_header_t* from_peer)
{
    gpcc_event_t* ev = 0;

    gpcc_log_info("net_accepted");

    if (from_peer->argc == 3
	&& 0 == memcmp("HTTP/1.", from_peer->argv[2], 7)
	&& (0 == strcmp("GET", from_peer->argv[0]) ||
	    0 == strcmp("POST", from_peer->argv[0]))) {

	gpcc_net_uri_t* uri = gpcc_net_uri_alloc(from_peer->argv[1]);
	char* a = (uri ? gpcc_net_uri_param(uri, "a") : 0);
	const char* session_id = (uri ? gpcc_net_uri_param(uri, "sid") : 0);
	int is_bad_uri = 0;
	int is_bad_array_name = 0;
	int is_bad_session_id = 0;
	int is_lead_ma = 0;
	int is_no_lead_ma = 0;

	/* is Lead MA */
	is_no_lead_ma = (gpma.lma_cmdslot->u.lma.chubby.lease == NULL && gpma.lma_cmdslot->u.lma.leader == NULL);
	is_lead_ma = (gpma.lma_cmdslot->u.lma.chubby.lease != NULL);

	is_bad_array_name = ! (gpma.array_name && a && 0 == strcmp(a, gpma.array_name));
	if (is_lead_ma && ! is_bad_array_name) {
		if (!session_id) {
		    is_bad_session_id = 1;
		    
		} else {
		    user_t *user;
		    /* check the sessionid is valid */
		    user = apr_hash_get(gpma.user_cmdslot->u.user.usertb, session_id, APR_HASH_KEY_STRING);
		    if (!user) {
			gpcc_log_error("invalid session_id = %s", session_id);
			is_bad_session_id = 1;
		    } else {
			/* extend the expire time */
			user->expire_time = apr_time_now() + 2 * 3600 * APR_USEC_PER_SEC;
		    }
		}


	    if (0 == strcmp("/gpcc/login", uri->path)) {
		ev = mkhttp_event_more(USER_LOGIN_EVENT, conn, gpma.user_cmdslot->cmdidx, 0, 
				       gpma.user_cmdslot->cmdidx, peer_addr, from_peer->post, from_peer->postlen);
		is_bad_session_id = 0;
	    } else if (0 == strcmp("/gpcc/logout", uri->path)) {
		if (session_id) {
		    ev = mkhttp_event_more(USER_LOGOUT_EVENT, conn, gpma.user_cmdslot->cmdidx, 0, 
					   gpma.user_cmdslot->cmdidx, session_id, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		    is_bad_session_id = 0;
		}
	    } else if (! is_bad_session_id) {
		/* execute the command */

		if (0 == strcmp("/gpcc/lscfg", uri->path)) {
		    ev = mkhttp_event(ARRAY_CONFIG_EVENT, conn, gpma.cfg_cmdslot->cmdidx, 0,
				      gpma.cfg_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		}
		else if (0 == strcmp("/gpcc/status", uri->path)) {
		    char *seg = (uri ? gpcc_net_uri_param(uri, "s") : 0);
		    
		    if (! seg) {
			ev = mkhttp_event(ARRAY_STATUS_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
					  gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		    } else {
			int i;
			if (gpma.unit_cmdtab) {
			    for (i = 0 ; i < gpma.unit_max && strcmp(seg, gpma.unit_cmdtab[i]->cmdname) != 0 ; i++) {;}
			    if (i < gpma.unit_max) {
				ev = mkhttp_event(UNIT_STATUS_EVENT, conn, gpma.unit_cmdtab[i]->cmdidx, 0,
						  gpma.unit_cmdtab[i]->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
			    } else {
				gpcc_net_http_close(conn, 200, "ok", "{\"status\":\"error\", 'errmsg': 'no segment found' }");
			    }
			} else {
			    gpcc_net_http_close(conn, 200, "ok", "{\"status\":\"error\", 'errmsg' : 'no segment found' }");
			}
		    }
		}
		else if (0 == strcmp("/gpcc/init", uri->path)) {
		    ev = mkhttp_event(ARRAY_INIT_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
				      gpma.fts_cmdslot->cmdidx, from_peer->post, from_peer->postlen+1);
		}
		else if (0 == strcmp("/gpcc/start", uri->path)) {
		    char *seg = (uri ? gpcc_net_uri_param(uri, "s") : 0);
		    
		    if (! seg) {
			/* send startcmd_event to FTS cmdslot ? */
			ev = mkhttp_event(ARRAY_START_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
					  gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		    } else {
			ev = mkhttp_event(SEGMENT_START_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0,
					  gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		    }
		}
		else if (0 == strcmp("/gpcc/stop", uri->path)) {
		    char *seg = (uri ? gpcc_net_uri_param(uri, "s") : 0);
		    if (! seg) {
			ev = mkhttp_event(ARRAY_STOP_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
					  gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		    } else {
			ev = mkhttp_event(SEGMENT_STOP_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0,
					  gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		    }
		}
		else if (0 == strcmp("/gpcc/delete", uri->path)) {
		    ev = mkhttp_event(ARRAY_DELETE_EVENT, conn, gpma.fts_cmdslot->cmdidx, 0, 
				      gpma.fts_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		} else if (0 == strcmp("/gpcc/admin/add", uri->path)) {
		    ev = mkhttp_event(ADMIN_ADD_EVENT, conn, gpma.admin_cmdslot->cmdidx, 0, 
				      gpma.admin_cmdslot->cmdidx, from_peer->post, from_peer->postlen+1);
		} else if (0 == strcmp("/gpcc/admin/delete", uri->path)) {
		    ev = mkhttp_event(ADMIN_DELETE_EVENT, conn, gpma.admin_cmdslot->cmdidx, 0, 
				      gpma.admin_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		} else if (0 == strcmp("/gpcc/admin/list", uri->path)) {
		    ev = mkhttp_event(ADMIN_LIST_EVENT, conn, gpma.admin_cmdslot->cmdidx, 0, 
				      gpma.admin_cmdslot->cmdidx, from_peer->argv[1], strlen(from_peer->argv[1])+1);
		} else if (0 == strcmp("/gpcc/admin/password", uri->path)) {
		    ev = mkhttp_event(ADMIN_PASSWORD_EVENT, conn, gpma.admin_cmdslot->cmdidx, 0, 
				      gpma.admin_cmdslot->cmdidx, from_peer->post, from_peer->postlen+1);
		}
		else {
		    is_bad_uri = 1; 
		}
	    }
	}
	
	if (ev) {
	    gpcc_evqueue_put(gpma.evq, ev);
	}
	else if (! is_lead_ma) {
	    if (is_no_lead_ma) {
		gpcc_warning(FLINE, "invalid URI: %s. Lead MA is not determined", from_peer->argv[1]);
		gpcc_net_http_close(conn, 200, "OK", 
				    "{ \"status\": \"error\", \"errmsg\": \"Lead MA is not determined\" }");
	    } else {
		char location[128];
		char json[1024];

		gpcc_warning(FLINE, "invalid URI: %s. Redirect request to %s", from_peer->argv[1], gpma.lma_cmdslot->u.lma.leader);
		sprintf(location, "http://%s%s", gpma.lma_cmdslot->u.lma.leader, from_peer->argv[1]);
		sprintf(json, "{ \"a\": \"%s\", \"status\": \"error\", \"location\" : \"%s\", \"errmsg\": \"this is not a lead MA\"}", 
			gpma.array_name,
			gpma.lma_cmdslot->u.lma.leader);
		gpcc_net_http_redirect_close(conn, location, json);
	    }
	}
	else if (is_bad_uri) {
	    gpcc_warning(FLINE, "invalid URI: %s", from_peer->argv[1]);
	    gpcc_net_http_close(conn, 200, "OK", 
				"{ \"status\": \"error\", \"errmsg\": \"invalid URI\" }");
	} 
	else if (is_bad_array_name) {
	    gpcc_warning(FLINE, "invalid URI: %s", from_peer->argv[1]);
	    gpcc_net_http_close(conn, 200, "OK",
				"{ \"status\": \"error\", \"errmsg\": \"bad array name\" }");
	}
	else if (is_bad_session_id) {
	    gpcc_warning(FLINE, "invalid URI: %s", from_peer->argv[1]);
	    gpcc_net_http_close(conn, 200, "OK",
				"{ \"status\": \"error\", \"errmsg\": \"please do 'gp login' to log on\" }");
	}

	if (uri) 
	    gpcc_net_uri_free(uri);
	return;
    }
    
    if (from_peer->argc == 1 && strcmp(from_peer->argv[0], GPCC_NET_PACKET_PROTO) == 0) {
	int i;
	const char *role = 0;
	for (i = 0 ; i < from_peer->hc ; i++) {
	    if (apr_strnatcasecmp(from_peer->hname[i], "X-GPCC-ROLE") == 0) {
		role = from_peer->hvalue[i];
	    }
	}

    }


    /* unknown protocol - reject the connection */
    gpcc_net_http_close(conn, 403, "Forbidden", "Bad protocol");
}


static int net_received(gpcc_net_conn_t conn, 
			const char* buf, int buflen,
			apr_uint64_t arg)
{
    /* should not invoke, yet */
    ((char *)buf)[buflen]=0;
    gpcc_warning(FLINE, "net_received should not get called. len=%d, c= %s", buflen, buf);
    
    return -1;
}


static void net_received_packet(gpcc_net_conn_t conn, 
				gpcc_net_packet_t* pkt,
				apr_uint64_t arg)
{
    gpcc_event_t *ev;
    apr_uint32_t cmdidx = pkt->mh.rcvidx;

    ev = gpcc_event_alloc(pkt->mh.type, cmdidx, sizeof(*pkt) + pkt->mh.len);
    memcpy(ev->pdata, pkt, sizeof(*pkt)+pkt->mh.len);
    gpcc_evqueue_put(gpma.evq, ev);
}


static void net_closed(gpcc_net_conn_t conn, apr_uint64_t arg)
{
    apr_uint32_t cmdidx = (apr_uint32_t) arg;
    gpcc_event_t *ev;

    if (cmdidx) {
	ev = gpcc_event_alloc(NET_CLOSED_EVENT, cmdidx, sizeof(gpcc_net_conn_t));
	memcpy(ev->pdata, &conn, sizeof(gpcc_net_conn_t));
	gpcc_evqueue_put(gpma.evq, ev);
    }
}


static void net_tick(apr_time_t now)
{
    gpcc_event_t* ev = gpcc_event_alloc(TICK_EVENT, gpma.gpma_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}

/* chubby */
static void do_chubby(int fd, short event, void* arg)
{
    int new_fd;
    struct timeval tv;
    chubby_asynchronous_think(gpma.chubby, &new_fd, &tv);
    gpcc_net_chubby_event(new_fd, do_chubby, &tv, 0);
}


/* Main program for the daemon */
void d_main(void)
{
    char date[APR_RFC822_DATE_LEN];
    gpcc_evqueue_t* evq = 0;
    gpcc_event_t* ev = 0;

    gpma.logfpath = apr_psprintf(gpma.pool, "%s/gpma.log", 
				 gpma.datadir);

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

    /* dump the config */
    /*    process_lscfg(0); */

    event_init();

    cmdslot_init(gpma.pool);

    /* configuration slot */
    gpma.cfg_cmdslot = cmdslot_alloc("CFG", CFG_CMDSTATE);

    /* GPMA slot */
    gpma.gpma_cmdslot = cmdslot_alloc("GPMA", GPMA_CMDSTATE);

    gpma.lma_cmdslot = cmdslot_alloc("LMA", LMA_OFF_CMDSTATE);

    /* FTS slot */
    gpma.fts_cmdslot = cmdslot_alloc("FTS", FTS_CMDSTATE);

    /* user slot */
    gpma.user_cmdslot = cmdslot_alloc("USER", USER_CMDSTATE);
    gpma.user_cmdslot->u.user.usertb = apr_hash_make(gpma.user_cmdslot->pool);
    gpma.user_cmdslot->countdown = 30;

    /* user slot */
    gpma.admin_cmdslot = cmdslot_alloc("ADMIN", ADMIN_CMDSTATE);

    
    fsm_init();

    /* create the event queue */
    evq = gpma.evq = gpcc_evqueue_create(1000, gpma.pool);

    /* greetings */
    apr_rfc822_date(date, apr_time_now());
    gpcc_log_info("[%s] started gpma %s %s", date, gpma.guid,gpma.datadir);

    /* network up */
    GPCC_DO(0 == gpcc_net_init(gpma.ma_port,
			       gpma.ssl_enabled,
			       gpma.server_cert_path,
			       gpma.server_key_path,
			       NULL,
			       net_opened, net_accepted, 
			       net_received, net_received_packet,
			       net_closed, net_tick));

    gpcc_log_info("gpma listening to port %d %s", gpma.ma_port, (gpma.ssl_enabled? "(SSL)" : ""));

    
    /* chubby */
    {
	int fd;
	struct timeval tv;
	
	gpma.chubby = chubby_new(gpma.px_address, 45, malloc, free);
	if (! gpma.chubby) {
	    gpcc_fatal(FLINE, "error in initializing chubby");
	}
	chubby_asynchronous_think(gpma.chubby, &fd, &tv);
	gpcc_net_chubby_event(fd, do_chubby, &tv, 0);
    }

    /* kick of FTS slot */
    /*
    ev = gpcc_event_alloc(INIT_EVENT, gpma.fts_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
    */

    gpma.lma_cmdslot->countdown = 1;

    /* MAIN LOOP - 
     *     - get an event from queue
     *     - find the target slot of the event
     *     - get the FSM of the slot 
     *     - dispatch the event to the FSM
     */
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
	if (gpma.verbose) {
	    if (ev->etype != TICK_EVENT && slot != gpma.gpma_cmdslot)
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

static const char* verify_hostaddr(const char* ma)
{
    char buf[1024];
    char* p;
    strncpy(buf, ma, sizeof(buf));
    buf[sizeof(buf)-1] = 0;
    if (strlen(buf) == sizeof(buf) - 1) 
        return "specification too long";

    p = strchr(buf, ':');
    if (!p || strchr(p+1, ':'))
        return "illegal format - expects node:port";

    return NULL;
}



/*
 * Load the config into gpma. 
 */
void load_config(void)
{
    apr_uint64_t guid;
    apr_uint64_t rand;
    char guidstr[16 + 4];
    apr_time_t now = apr_time_now();
    
    gpma.cfg = config_create(gpma.pool);
	
    /* set HA datadir */
    config_set(gpma.cfg, "ha.datadir", gpma.ha_datadir);

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
    config_set(gpma.cfg, "sys.guid", gpma.guid);

    /* prep the datadir */
    if (0 != apr_dir_make_recursive(gpma.datadir, 0x700, gpma.pool)) 
	gpcc_fatal(FLINE, "unable to create ma.datadir '%s'", gpma.datadir);
    
}


void check_gphome(void)
{
    char ls[300];
    apr_snprintf(ls, sizeof(ls), "ls %s/bin/gpha > /dev/null 2>&1", gpma.gphome);
    if (0 != system(ls))
	gpcc_fatal(FLINE, "gpha executable not found in GPHOME %s", gpma.gphome);
    apr_snprintf(ls, sizeof(ls), "ls %s/bin/postgres > /dev/null 2>&1", gpma.gphome);
    if (0 != system(ls))
	gpcc_fatal(FLINE, "postgres executable not found in GPHOME %s", gpma.gphome);
}


void check_user(void)
{
    char *user, *logname;
    
    user = getenv("USER");
    logname = getenv("LOGNAME");
    

    if (user && *user) {
	gpma.user = apr_pstrdup(gpma.pool, user);
	return;
    }
    
    if (logname && *logname) {
	gpma.user = apr_pstrdup(gpma.pool, logname);
	return;
    }

    gpcc_fatal(FLINE, "environment variable USER or LOGNAME not exist");
}

void check_hostname(void)
{
    char local[256];

    if (gethostname(local, sizeof(local)) != 0)
	gpcc_fatal(FLINE, "get local hostname failed");

    gpma.hostname = apr_pstrdup(gpma.pool, local);

}

void chubby_path_setup(void)
{
    gpma.chubby_etc_dir = apr_pstrdup(gpma.pool, "/gpcc/etc/");
    gpma.chubby_passwd_file = apr_pstrdup(gpma.pool, "/gpcc/etc/passwd");
    gpma.chubby_passwd_lease = apr_pstrdup(gpma.pool, "/gpcc/etc/passwd.lease");
    gpma.chubby_array_dir = apr_psprintf(gpma.pool, "/gpcc/%s/gpma", gpma.array_name);
    gpma.chubby_leader_file = apr_psprintf(gpma.pool, "%s/leader", gpma.chubby_array_dir);
    gpma.chubby_leader_lease = apr_psprintf(gpma.pool, "%s/lease", gpma.chubby_array_dir);
    gpma.chubby_master_file = apr_psprintf(gpma.pool, "%s/master", gpma.chubby_array_dir);
}

int main(int argc, const char* argv[])
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    GPCC_DO(0 == apr_app_initialize(&argc, (char const* const **) &argv, NULL));
    atexit(apr_terminate);

    gpma.progname = argv[0];

    srandom((unsigned long) apr_time_now());

    GPCC_DO(0 == apr_pool_create(&gpma.pool, 0));

    parse_cmdline(argc, argv);

    GPCC_DO(0 == setenv("GPHOME", gpma.gphome, 1));
    printf("GPHOME=%s\n", gpma.gphome);

    check_user();
    check_hostname();
    check_gphome();
    load_config();
    chubby_path_setup();

    if (gpma.daemonize) {
	GPCC_DO(0 == apr_proc_detach(1));
	/* At this point, we are now a daemon */
    }

    d_main();
    return 0;
}
