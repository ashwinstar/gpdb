#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_net.h>
#include <gpcc_fsm.h>
#include <gpcc_spin.h>
#include <gpcc_json.h>
#include <json/json.h>
#include <event.h>

#include "gpfts.h"
#include "fts.h"

gpfts_t gpfts = {0};

/* Match a cmdidx to a cmdslot */
static cmdslot_t* cmdslot_get(int cmdidx)
{
    const int idx = cmdidx & 0xffff;
    if (0 <= idx && idx < CMDSLOT_MAX) {
	cmdslot_t* cmdslot = &gpfts.cmdslot[idx];
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

    for (i = 0; i < CMDSLOT_MAX && gpfts.cmdslot[i].cmdidx; i++);
    if (i < CMDSLOT_MAX) {
	cmdslot = &gpfts.cmdslot[i];
	memset(cmdslot, 0, sizeof(*cmdslot));
	seq = (((int)gpfts.cmdidx_gen++) << 16) & 0x80000000;
	if (! seq)
	    seq = 1 << 16;	/* ensure that seq is never 0 */
	cmdslot->cmdidx = seq | i;
	cmdslot->cmdstate = init_state;
	cmdslot->conninfo = conninfo;
	apr_pool_create(&cmdslot->pool, 0);
	cmdslot->cmdname = (name ? apr_pstrdup(cmdslot->pool, name) : 0);

	ev = gpcc_event_alloc(INIT_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, ev);
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
		 "peer_id: '%s', "
		 "role: '%s' }", 
		 GPCC_NET_PROTO,
		 nodeidstr, 
		 "ha");
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
void time_tick_cb(apr_time_t now)
{
    gpcc_event_t *ev = gpcc_event_alloc(TICK_EVENT, gpfts.timer_cmdidx, 0);
    gpcc_evqueue_put(gpfts.evq, ev);
}


/* Main program for the daemon */
static void * APR_THREAD_FUNC fts_thread_run(apr_thread_t *thread, void *arg)
{
    gpcc_event_t* ev = 0;
    cmdslot_t* cp;
    apr_uint32_t *spin;

    spin = gpcc_spin_secure(gpfts.spin);
    while (gpfts.thread == thread) {
	int nxtstate;
	gpcc_fsm_func_t** action;

	if (ev) 
	    gpcc_event_free(ev);

	spin = gpcc_spin_release(gpfts.spin);

	while ((ev = gpcc_evqueue_get(gpfts.evq)) == 0) {
	    break;
	}
	
	spin = gpcc_spin_secure(gpfts.spin);

	if (ev == 0) {
	    continue;
	}

	/* map the cmdidx */
	if (ev->cmdidx == 0) 	/* artificially match 0 to system_cmdidx */
	    ev->cmdidx = gpfts.system_cmdidx;

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
	    if (gpfts.debug) {
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

	    gpcc_evqueue_put(gpfts.evq, 
			     gpcc_event_alloc(FAILURE_EVENT, cp->cmdidx, 0));
	}

	/* free up the cmdslot if necessary */
	if (cp->cmdstate == NULL_CMDSTATE) {
	    cmdslot_free(cp);
	}
    }

    spin = gpcc_spin_release(gpfts.spin);

    if (ev) {
	gpcc_event_free(ev);
    }

    gpcc_log_info("d_main exiting");

    return 0;
}


static void do_timeout(int fd, short event, void *arg)
{
    struct timeval tm;
    apr_time_t now = apr_time_now();

    time_tick_cb(now);
    /* 1 sec timeout */
    tm.tv_sec = 0;
    tm.tv_usec = 1000 * 1000;
    evtimer_add(&gpfts.timer_event, &tm);

}


int fts_initialize(config_t *config, gpcc_evqueue_t *alertq)
{
    int e;
    struct timeval tm;
    gpcc_evqueue_t* evq;
    cmdslot_t* cp;

    if (0 != (e = apr_pool_create(&gpfts.pool, 0))) {
	return e;
    }

    /* create the event queue */
    evq = gpfts.evq = gpcc_evqueue_create(1000, gpfts.pool);

    /* set up the fsm */
    fsm_setup();

    /* timer slot */
    cp = cmdslot_alloc("TIMER", TIMER_CMDSTATE, 0);
    gpfts.timer_cmdidx = cp->cmdidx;

    /* system slot */
    cp = cmdslot_alloc("SYSTEM", SYSTEM_CMDSTATE, 0);
    gpfts.system_cmdidx = cp->cmdidx;

    /* setup timer */
    evtimer_set(&gpfts.timer_event, do_timeout, 0);
    tm.tv_sec = 0;
    tm.tv_usec = 1000 * 1000;
    evtimer_add(&gpfts.timer_event, &tm);

    /* alert queue */
    gpfts.alertq = alertq;
    gpfts.config = config;

    if (0 != (e = apr_threadattr_create(&gpfts.attr, gpfts.pool))) {
	gpcc_warning(FLINE, "apr_threadattr_create failed");
	return e;
    }

    if (0 != (e = apr_thread_create(&gpfts.thread, gpfts.attr, fts_thread_run, 0, gpfts.pool))) {
	gpcc_warning(FLINE, "apr_thread_create failed");
	return e;
    }

    return 0;
}

int fts_terminate()
{
    int ret;
    apr_thread_t *thread = gpfts.thread;

    gpfts.thread = 0;
    gpcc_evqueue_put(gpfts.evq, 0);

    apr_thread_join(&ret, thread);

    apr_pool_destroy(gpfts.pool);
    
    return 0;
}


int fts_do(const char *cmd, gpcc_evqueue_t *outq)
{
    struct json_object *obj;
    char *action;
    gpcc_event_t *ev = 0;
    cmdslot_t *p;
    apr_uint32_t *spin;

    obj = json_tokener_parse((char *)cmd);
    action = gpcc_json_object_get_string(obj, "action", 0);

    spin = gpcc_spin_secure(gpfts.spin);

    if (gpfts.system_cmdidx == 0) {
	p = cmdslot_alloc("SYSTEM", SYSTEM_CMDSTATE, 0);
	gpfts.system_cmdidx = p->cmdidx;
    } else {
	p = cmdslot_get(gpfts.system_cmdidx);
    }
    if (p->u.sys.request.outq) {
	gpcc_warning(FLINE, "fts is running on something else.");
	spin = gpcc_spin_release(gpfts.spin);
	json_object_put(obj);
	return APR_EINVAL;
    }
    p->u.sys.request.outq = outq;
    p->u.sys.request.reqobj = obj;

    spin = gpcc_spin_release(gpfts.spin);

    if (strcmp(action, "setup") == 0) {
	ev = gpcc_event_alloc(ARRAY_SETUP_EVENT, gpfts.system_cmdidx, strlen(cmd)+1);
	memcpy(ev->edata, cmd, strlen(cmd) + 1);
    } else if (strcmp(action, "init") == 0) {
	ev = gpcc_event_alloc(ARRAY_INIT_EVENT, gpfts.system_cmdidx, strlen(cmd)+1);
	memcpy(ev->edata, cmd, strlen(cmd) + 1);
    } else if (strcmp(action, "start") == 0) {
	ev = gpcc_event_alloc(ARRAY_START_EVENT, gpfts.system_cmdidx, strlen(cmd)+1);
	memcpy(ev->edata, cmd, strlen(cmd) + 1);
    } else if (strcmp(action, "stop") == 0) {
	ev = gpcc_event_alloc(ARRAY_STOP_EVENT, gpfts.system_cmdidx, strlen(cmd)+1);
	memcpy(ev->edata, cmd, strlen(cmd) + 1);
    } else if (strcmp(action, "status") == 0) {
	ev = gpcc_event_alloc(ARRAY_STATUS_EVENT, gpfts.system_cmdidx, strlen(cmd)+1);
	memcpy(ev->edata, cmd, strlen(cmd) + 1);
    } else {
	gpcc_warning(FLINE, "invalid command %s", cmd);
	json_object_put(obj);
	return APR_EINVAL;
    }
    
    gpcc_evqueue_put(gpfts.evq, ev);

    return 0;
}
