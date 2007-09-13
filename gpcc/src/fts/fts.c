#include <stdio.h>
#include <stdlib.h>
#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <apr_queue.h>
#include <gpcc_event.h>
#include <gpcc_ha.h>
#include <gpcc_net.h>
#include "fts.h"


typedef struct hainfo_t hainfo_t;
struct hainfo_t {
    int index;

    gpcc_ha_t *ha;
    int rcvidx;
    int sndidx;

    const char *hosts[4];
    int port;
    
    int status;

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

    hainfo_t *hainfo[1024];
    int nha;

    struct {
	apr_queue_t *cmd;
	apr_queue_t *msg;
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

    if (0 != (e = apr_queue_create(&fts.queue.msg, 1024, fts.pool))) {
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
    apr_queue_term(fts.queue.msg);
    
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


static void ha_opened(gpcc_ha_t *ha)
{

    return;
}

static void ha_closed(gpcc_ha_t *ha)
{
    /* generate error and add to alertq */

    return;
}

static void ha_recv(gpcc_ha_t *ha, gpcc_net_packet_t *packet)
{
    /* if alert, add to alertq else add to msgq */

    /* if IDENTITY_MSG, update rcvidx and sndidx */

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


/* try to connect to HAs.  If failed, startup HAs by ssh and connect again.  */
static void do_setup(fts_request_t *req)
{


}

/* do initdb */
static void do_init(fts_request_t *req)
{


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
