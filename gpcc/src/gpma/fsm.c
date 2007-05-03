#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpma.h"
#include "fsm.h"


static void u_queue_failure_event(cmdslot_t* cmdslot)
{
    gpcc_event_t* ev = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}

static void u_queue_timeup_event(cmdslot_t* cmdslot)
{
    gpcc_event_t* ev = gpcc_event_alloc(TIMEUP_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}

static apr_status_t u_send_gpcc_net_proto(conninfo_t* conninfo)
{
    if (conninfo->self.request.argc) 
	return APR_EINVAL;

    conninfo->self.request.argc = 1;
    conninfo->self.request.argv[0] = GPCC_NET_PROTO;
    return gpcc_net_send_request(conninfo->conn, &conninfo->self.request);
}

static apr_status_t u_send_get_identity(cmdslot_t* cmdslot)
{
    return gpcc_net_send_packet(cmdslot->conninfo->conn, GET_IDENTITY_MSG, 
				cmdslot->cmdidx, 0, 0, 0);
}

static apr_status_t u_send_identity(conninfo_t* conninfo, apr_uint32_t rcvidx, char* json)
{
    return gpcc_net_send_packet(conninfo->conn, IDENTITY_MSG,
				0, rcvidx, json, strlen(json));
}




/* 
 *  This gets called every second. go thru all cmdslot to find anyone that 
 *  needs a TIMEUP EVENT.
 */
static apr_status_t a_system_tick(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* p = &gpma.cmdslot[0];
    cmdslot_t* q = &gpma.cmdslot[CMDSLOT_MAX];

    for ( ; p < q; p++) {
	if (p->cmdidx && p->countdown > 0)
	    if (--p->countdown == 0) 
		u_queue_timeup_event(p);
    }
    return 0;
}


/** connection is opened
 */
static apr_status_t a_system_net_opened(gpcc_event_t* ev, void* cmdslot_)
{
    eventinfo_t* ei = (eventinfo_t*) ev->edata;
    conninfo_t* conninfo = ei->conninfo;

    /* if i opened it, send my proto */
    if (conninfo->self_origin) {
	if (0 != u_send_gpcc_net_proto(conninfo)) 
	    gpcc_net_close(conninfo->conn);
	return 0;
    }

    return 0;
}


static apr_status_t a_system_net_closed(gpcc_event_t* ev, void* cmdslot_)
{
    /*
    eventinfo_t* ei = (eventinfo_t*) ev->edata;
    conninfo_t* conninfo = ei->conninfo;
    */
    /* queue a HA failure, or MA failure, or client failure event */
    return 0;
}



static apr_status_t a_system_net_received(gpcc_event_t* ev, void* cmdslot_)
{
    eventinfo_t* ei = (eventinfo_t*) ev->edata;
    conninfo_t* conninfo = ei->conninfo;
    int e;

    /* peer has not send protocol ? */
    if (! conninfo->peer.request) {
	e = gpcc_net_read_request(conninfo->conn, &conninfo->peer.request, 
				  conninfo->pool);
	if (e) {
	    if (e == APR_EAGAIN)
		return 0;
	    gpcc_net_close(conninfo->conn);
	    return 0;
	}
	if (1 == conninfo->peer.request->argc &&
	    0 == strcmp(conninfo->peer.request->argv[0], GPCC_NET_PROTO)) {
	    if (0 != u_send_gpcc_net_proto(conninfo)) {
		gpcc_net_close(conninfo->conn);
		return 0;
	    }
	    if (! cmdslot_alloc("GET_IDENTITY", GET_IDENTITY_CMDSTATE, conninfo)) {
		gpcc_net_close(conninfo->conn);
		return 0;
	    }
	}
	else {
	    /* handle http */
	    gpcc_fatal(FLINE, "protocol not implemented");
	}
	return 0;
    }
}

static apr_status_t a_close_conn(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* cmdslot = cmdslot_;
    gpcc_close_conn(cmdslot->conn);
    return 0;
}


static apr_status_t a_get_identity_send_request(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* cmdslot = cmdslot_;
    eventinfo_t* ei = (eventinfo_t*) ev->edata;
    conninfo_t* conninfo = ei->conninfo;
    if (! u_send_get_identity(cmdslot)) { 
	return APR_EINVAL;
    }
}


APR_DECLARE(void) fsm_setup(void)
{
    gpcc_fsm_put(SYSTEM_CMDSTATE, TICK_EVENT, SYSTEM_CMDSTATE, 
		 a_system_tick, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_OPENED_EVENT, SYSTEM_CMDSTATE, 
		 a_system_net_opened, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_RECV_EVENT, SYSTEM_CMDSTATE,
		 a_system_net_received, 0);

    gpcc_fsm_put(GET_IDENTITY_CMDSTATE, INIT_EVENT, GET_IDENTITY_CMDSTATE, 
		 a_get_identity_send_request, 0);
    gpcc_fsm_put(GET_IDENTITY_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE, 
		 a_close_conn, 0);
}
