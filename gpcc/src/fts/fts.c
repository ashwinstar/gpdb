#include <stdio.h>
#include <stdlib.h>
#include <apr_time.h>
#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <apr_queue.h>
#include <gpcc_event.h>
#include <gpcc_ha.h>
#include <gpcc_net.h>
#include <gpcc_spin.h>
#include <gpcc_json.h>
#include "fts.h"

typedef enum msg_t msg_t;
enum msg_t {
    NULL_MSG,
    GET_IDENTITY_MSG,
    IDENTITY_MSG,
    KEEP_ALIVE_MSG,
    VOTE_ME_MSG = 10,
    VOTE_REPLY_MSG,
    CSN_GET_MSG = 20,
    CSN_REPLY_MSG,
    CONFIG_GET_MSG,
    CONFIG_REPLY_MSG,
    LEADMA_GET_MSG = 30,
    LEADMA_REPLY_MSG,
    SEGMENT_REQUEST_MSG = 100,
    SEGMENT_REPLY_MSG,
    HOST_REQUEST_MSG,
    ARRAY_REQUEST_MSG,
    STATUS_REQUEST_MSG,
    STATUS_REPLY_MSG,
    ALERT_MSG = 400,
    ERROR_MSG,

    MSG_MAX,
};

typedef enum ha_status_t ha_status_t;
enum ha_status_t {
    HA_NULL_STATUS,
    HA_CONNECTING_STATUS,
    HA_CONNECTED_STATUS,
    HA_CLOSED_STATUS,
    HA_MAX_STATUS,
};

typedef struct hainfo_t hainfo_t;
struct hainfo_t {
    int index;

    gpcc_ha_t *ha;
    const char *hosts[4];
    int port;
    
    ha_status_t status;

    struct {
	gpcc_net_packet_t *packet;
    } peer;
    
    struct {
	;
    } self;
};


typedef struct fts_request_t fts_request_t;
struct fts_request_t {
    apr_queue_t *outq;
    char msg[1];
};

typedef struct fts_t fts_t;
struct fts_t {
    apr_pool_t *pool;
    apr_threadattr_t *attr;
    apr_thread_t *thread;

    hainfo_t *hainfos[1024];
    int nhainfo;

    apr_uint32_t spin[1];

    struct {
	apr_queue_t *cmd;
	apr_queue_t *net;
	apr_queue_t *alert;
    } queue;

    fts_request_t *creq;

};



static fts_t fts = {0};


static void fts_run(apr_thread_t *thread, void *arg);

/* do setup */
static void do_setup(fts_request_t *req);

/* do initdb */
static void do_init(fts_request_t *req);

/* do start */
static void do_start(fts_request_t *req);

/* do stop */
static void do_stop(fts_request_t *req);
/* do status */
static void do_status(fts_request_t *req);

static void * APR_THREAD_FUNC thread_run(apr_thread_t *thread, void *arg)
{
    while (fts.thread == thread) {
	fts_run(thread, arg);
    }
    return 0;
}

int fts_initialize(apr_queue_t *alertq)
{
    int e;

    if (0 != (e = apr_pool_create(&fts.pool, 0))) {
	gpcc_warning(FLINE, "apr_pool_create failed");
	return e;
    }

    if (0 != (e = apr_queue_create(&fts.queue.cmd, 100, fts.pool))) {
	gpcc_warning(FLINE, "gpcc_evqueue_create failed");
	return e;
    }

    if (0 != (e = apr_queue_create(&fts.queue.net, 1024, fts.pool))) {
	gpcc_warning(FLINE, "gpcc_evqueue_create failed");
	return e;
    }	

    fts.queue.alert = alertq;

    if (0 != (e = apr_threadattr_create(&fts.attr, fts.pool))) {
	gpcc_warning(FLINE, "apr_threadattr_create failed");
	return e;
    }

    if (0 != (e = apr_thread_create(&fts.thread, fts.attr, thread_run, 0, fts.pool))) {
	gpcc_warning(FLINE, "apr_thread_create failed");
	return e;
    }

    return 0;
}

int fts_terminate()
{
    int e;
    int ret;
    apr_thread_t *th = fts.thread;
    fts.thread = 0;

    apr_queue_term(fts.queue.cmd);
    apr_queue_term(fts.queue.net);
    
    e = apr_thread_join(&ret, th);

    apr_pool_destroy(fts.pool);
    
    return 0;
}

fts_request_t * fts_request_alloc(const char *msg, apr_queue_t *outq)
{
    fts_request_t *req;

    int size = sizeof(*req) + strlen(msg);

    req = malloc(size);
    req->outq = outq;
    strcpy(req->msg, msg);
    return req;
}

void fts_request_free(fts_request_t *req)
{
    if (req) free(req);
}

int fts_do(const char *msg, apr_queue_t *outq)
{
    fts_request_t *req;

    req = fts_request_alloc(msg, outq);
    apr_queue_push(fts.queue.cmd, req);

    return 0;
}


static void fts_run(apr_thread_t *thread, void *arg) {
    fts_request_t *req;
    int e;

    while (fts.thread == thread && 
	   (0 == (e = apr_queue_pop(fts.queue.cmd, (void **) &req)))) {

	/* do "setup", "start", "init", "stop" */
	printf("got command %s\n", req->msg);

	fts.creq = req;

	if (strcmp(req->msg, "setup") == 0) {
	    do_setup(req);
	} else if (strcmp(req->msg, "start") == 0) {
	    do_start(req);
	} else if (strcmp(req->msg, "init") == 0) {
	    do_init(req);
	} else if (strcmp(req->msg, "stop") == 0) {
	    do_stop(req);
	} else if (strcmp(req->msg, "status") == 0) {
	    do_status(req);
	} else {
	    
	}

	apr_queue_push(req->outq, strdup(req->msg));

	fts_request_free(req);
	fts.creq = 0;
    }

}



static void *ha_opened(gpcc_ha_t *ha, void *arg)
{
    hainfo_t *info = (hainfo_t *) arg;

    info->status = HA_CONNECTED_STATUS;
    return arg;
}

static void ha_closed(gpcc_ha_t *ha, void *arg)
{
    int e;
    apr_uint32_t *spin;
    hainfo_t *info = (hainfo_t *) arg;

    /* generate error and add to alertq */
    
    info->status = HA_CLOSED_STATUS;
    spin = gpcc_spin_secure(fts.spin);
    fts.hainfos[info->index] = 0;
    spin = gpcc_spin_release(spin);

    if (0 != (e = apr_queue_interrupt_all(fts.queue.net))) {
	gpcc_warning(FLINE, "apr_queue_interrup_all failed");
    }

    free(info);
}

static void ha_recv(gpcc_ha_t *ha, gpcc_net_packet_t *packet, void *arg)
{
    hainfo_t *info = (hainfo_t *) arg;
    int e;
    apr_queue_t *queue = 0;
    gpcc_net_packet_t *pkt = malloc(sizeof(*packet) + packet->mh.len - 1);

    memcpy(pkt, packet, sizeof(*packet) + packet->mh.len - 1);
    pkt->mh.rcvidx = info->index;

    /* if alert, add to alertq else add to netq */
    queue = (pkt->mh.type >= 400 && pkt->mh.type < 500) ? fts.queue.alert : fts.queue.net;
    if (0 != (e = apr_queue_push(queue, pkt))) {
	gpcc_warning(FLINE, "apr_queue_push failed");
    }
}

static apr_status_t ha_open(hainfo_t **hainfo, const char *addr) 
{
    hainfo_t *info;

    info = malloc(sizeof(*info));
    if (!hainfo) {
	return APR_ENOMEM;
    }
    
    memset(info, 0, sizeof(*info));
    
    info->ha = gpcc_ha_open(addr, ha_opened, ha_closed, ha_recv, info);
    if (!info->ha) {
	gpcc_warning(FLINE, "gpcc_ha_open %s error", addr);
	free(info);
	return APR_ECONNREFUSED;
    }

    info->status = HA_CONNECTING_STATUS;
    *hainfo = info;
    
    return APR_SUCCESS;
}

static apr_status_t ha_close(hainfo_t *hainfo)
{
    if (hainfo) {
	if (hainfo->ha) {
	    gpcc_ha_close(hainfo->ha);
	}
    }
    return 0;
}

static apr_status_t hainfo_all_connected(int *connected)
{
    apr_uint32_t *spin;
    int e;
    int nconnected;
    int i;

    e = 0;
    nconnected = 0;
    spin = gpcc_spin_secure(fts.spin);
    for (i = 0 ; i < fts.nhainfo ; i++) {
	if (! fts.hainfos[i]) {
	    gpcc_warning(FLINE, "network reset by peer [index = %d]", i);
	    spin = gpcc_spin_release(fts.spin);
	    return APR_ECONNRESET;
	}
	
	if (fts.hainfos[i]->status == HA_CONNECTED_STATUS) {
	    nconnected++;
	}
    }
    spin = gpcc_spin_release(fts.spin);
    
    *connected = (nconnected == fts.nhainfo) ? 1 : 0;
    return APR_SUCCESS;
}

/* try to connect to HAs.  If failed, startup HAs by ssh and connect again.  */
static void do_setup(fts_request_t *req)
{
    int e;
    int connected = 0;

    /* get node.X and connect */

    /* wait for opened callback */
    while (!connected) {
	if (0 != (e = hainfo_all_connected(&connected))) {
	    gpcc_warning(FLINE, "check ha connection error");
	    goto bail;
	}
	apr_sleep(1000);
    }
    
    if (!connected) {

	goto bail;
    }

    /* queue success result to req->outq */


    return;

    bail:
    ;
    /* queue failure result to req->outq */
	    
}

static int ha_network_healthy() 
{
    apr_uint32_t *spin;
    int i;

    /* check network connection before waiting for packet */
    spin = gpcc_spin_secure(fts.spin);
    for (i = 0 ; i < fts.nhainfo ; i++) {
	if (! fts.hainfos[i]) {
	    spin = gpcc_spin_release(fts.spin);
	    return 0;
	}
    }
    spin = gpcc_spin_release(fts.spin);
    
    return 1;
}

static apr_status_t hainfo_segment_reply(const char *segname, int *success)
{
    int nsuccess = 0;
    int nfail = 0;
    int e;
    gpcc_net_packet_t *packet;

    /* check network connection before waiting for packet */
    if (! ha_network_healthy()) {
	gpcc_warning(FLINE, "network connection reset by peer");
	return APR_ECONNRESET;
    }

    if (0 != (e = apr_queue_pop(fts.queue.net, (void **) &packet))) {
	if (e == APR_EINTR) {
	    gpcc_warning(FLINE, "apr_queue_pop interrupted.  network connection reset by peer");
	    return APR_EINTR;
	}
    }
    
    if (packet->mh.type == SEGMENT_REPLY_MSG) {
	struct json_object *obj;
	char *seg, *status, *reason;
	
	obj = json_tokener_parse(packet->mb);
	if (!obj) {
	    gpcc_warning(FLINE, "json_tokener_parse error");
	    return APR_EINVAL;
	}
	
	seg = gpcc_json_object_get_string(obj, "seg", "defaultseg");
	status = gpcc_json_object_get_string(obj, "status", "failed");
	reason = gpcc_json_object_get_string(obj, "reason", "");

	if (strcmp(seg, segname) != 0) {
	    gpcc_warning(FLINE, "reply from different HA (%s should be %s)", seg, segname);
	    return APR_EMISMATCH;
	}

	if (strcmp(status, "ok") == 0) {
	    nsuccess++;
	} else {
	    nfail++;
	    gpcc_warning(FLINE, "%s failed. reason = %s", seg, reason);
	}
	
	json_object_put(obj);
    } else {
	gpcc_warning(FLINE, "[Error] invalid message from segment (msg = %s)", packet->mb);
    }
    
    *success = (nsuccess == fts.nhainfo) ? 1: 0;
    return 0;
}

static apr_status_t hainfo_all_segment_reply(int *success) 
{
    int nsuccess = 0;
    int nfail = 0;
    int i = 0;
    int e;
    gpcc_net_packet_t *packet;

    for (i = 0 ; i < fts.nhainfo ; i++) {

	/* check network connection before waiting for packet */
	if (! ha_network_healthy()) {
	    gpcc_warning(FLINE, "network connection reset by peer");
	    return APR_ECONNRESET;
	}
	
	if (0 != (e = apr_queue_pop(fts.queue.net, (void **) &packet))) {
	    if (e == APR_EINTR) {
		gpcc_warning(FLINE, "apr_queue_pop interrupted.  network connection reset by peer");
		return APR_EINTR;
	    }
	}
									  
	if (packet->mh.type == SEGMENT_REPLY_MSG) {
	    struct json_object *obj;
	    char *seg, *status, *reason;

	    obj = json_tokener_parse(packet->mb);
	    if (!obj) {
		gpcc_warning(FLINE, "json_tokener_parse error");
		return APR_EINVAL;
	    }

	    seg = gpcc_json_object_get_string(obj, "seg", "defaultseg");
	    status = gpcc_json_object_get_string(obj, "status", "failed");
	    reason = gpcc_json_object_get_string(obj, "reason", "");

	    if (strcmp(status, "ok") == 0) {
		nsuccess++;
	    } else {
		nfail++;
		gpcc_warning(FLINE, "%s failed. reason = %s", seg, reason);
	    }

	    json_object_put(obj);
	} else {
	    gpcc_warning(FLINE, "[Error] invalid message from segment (msg = %s)", packet->mb);
	}
	
    }
    
    *success = (nsuccess == fts.nhainfo) ? 1: 0;
    return 0;
}

/* do initdb */
static void do_init(fts_request_t *req)
{

    /* get seg.* and init */
}

/* do start */
static void do_start(fts_request_t *req)
{
    

}

/* do stop */
static void do_stop(fts_request_t *req)
{
    
}

/* do status */
static void do_status(fts_request_t *req)
{

}
