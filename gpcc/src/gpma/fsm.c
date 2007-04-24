#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpma.h"
#include "fsm.h"


static void queue_failure_event(cmdslot_t* cmdslot)
{
    gpcc_event_t* ev = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}

static void queue_timeup_event(cmdslot_t* cmdslot)
{
    gpcc_event_t* ev = gpcc_event_alloc(TIMEUP_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);
}

static apr_status_t send_gpcc_net_proto(conninfo_t* conninfo)
{
    if (conninfo->self.request.argc) 
	return APR_EINVAL;

    conninfo->self.request.argc = 1;
    conninfo->self.request.argv[0] = GPCC_NET_PROTO;
    return gpcc_net_send_request(conninfo->conn, &conninfo->self.request);
}




/* 
 *  This gets called every second. go thru all cmdslot to find anyone that 
 *  needs a TIMEUP EVENT.
 */
static apr_status_t system_tick(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* p = &gpma.cmdslot[0];
    cmdslot_t* q = &gpma.cmdslot[CMDSLOT_MAX];

    for ( ; p < q; p++) {
	if (p->cmdidx && p->countdown > 0)
	    if (--p->countdown == 0) 
		queue_timeup_event(p);
    }
    return 0;
}


/** connection is opened
 */
static apr_status_t system_net_opened(gpcc_event_t* ev, void* cmdslot_)
{
    eventinfo_t* ei = (eventinfo_t*) ev->edata;
    conninfo_t* conninfo = ei->conninfo;

    /* if i opened it, send my proto */
    if (conninfo->self_origin) {
	if (0 != send_gpcc_net_proto(conninfo)) 
	    gpcc_net_close(conninfo->conn);
	return 0;
    }

    return 0;
}


static apr_status_t system_net_closed(gpcc_event_t* ev, void* cmdslot_)
{
    eventinfo_t* ei = (eventinfo_t*) ev->edata;
    conninfo_t* conninfo = ei->conninfo;
    /* queue a HA failure, or MA failure, or client failure event */
}



static apr_status_t system_net_received(gpcc_event_t* ev, void* cmdslot_)
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
	    if (0 != send_gpcc_net_proto(conninfo)) {
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


APR_DECLARE(void) fsm_setup(void)
{
    gpcc_fsm_put(SYSTEM_CMDSTATE, TICK_EVENT, SYSTEM_CMDSTATE, 
		 system_tick, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_OPENED_EVENT, SYSTEM_CMDSTATE, 
		 system_net_opened, 0);
}
