#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_net.h>
#include <gpcc_json.h>
#include <json/json.h>
#include <event.h>
#include "fts.h"

static apr_pool_t *pool = 0;

static void* net_opened(gpcc_net_conn_t conn, void* conn_arg, 
		 int self_origin,
		 apr_pool_t* pool)
{
    gpcc_warning(FLINE, "local net opened");
    return 0;
}

/** callback when a connection is closed */
static void net_closed(gpcc_net_conn_t conn, void* conn_arg)
{
    gpcc_warning(FLINE, "local net closed");
}

/** callback when bytes arrives */
static void net_recv(gpcc_net_conn_t conn, void* conn_arg)
{
    gpcc_warning(FLINE, "local net recv");
}

/** callback every second */
static void net_tick(apr_time_t now)
{
    //    printf("net_tick\n");   
}

void *APR_THREAD_FUNC fts_cmd(apr_thread_t *thread, void *arg) 
{
    struct json_object *obj;
    gpcc_event_t *ev;
    gpcc_evqueue_t *outq = 0;

    outq = gpcc_evqueue_create(100, pool);

    apr_sleep(2000);
    
    gpcc_log_info("setup..........");
    obj = gpcc_json_array_request("setup", "");
    fts_do(json_object_to_json_string(obj), outq);
    json_object_put(obj);

    ev = gpcc_evqueue_get(outq);
    if (!ev)
	return 0;
	
    gpcc_log_info("result = %s", ev->edata);

    free(ev);
	
    if (strstr(ev->edata, "fail"))
	return 0;


    gpcc_log_info("initdb...");
    
    obj = gpcc_json_array_request("init", "");
    fts_do(json_object_to_json_string(obj), outq);
    json_object_put(obj);

    ev = gpcc_evqueue_get(outq);
    if (!ev)
	return 0;
	
    gpcc_log_info("result = %s", ev->edata);

    
    gpcc_evqueue_destroy(outq);
    return 0;

}

int main(int argc, const char *argv[]) 
{
    int e;
    int started = 0;
    config_t *config = 0;
    struct json_object *obj = 0;
    gpcc_event_t *ev = 0;
    apr_thread_t *thread;
    apr_threadattr_t *attr;
    gpcc_evqueue_t *alertq = 0;

    apr_app_initialize(&argc, (char const *const **) &argv, 0);

    apr_pool_create(&pool, 0);

    config = config_read("/Users/elam/w/head/cdb2/gpcc/src/fts/qa", "guid", 0, pool);
    if (!config) {
	gpcc_warning(FLINE, "config_read failed");
	return APR_EINVAL;
    }
    
    printf("gpdb.name=%s\n", config_get(config, "gpdb.name"));
    printf("node.0=%s\n", config_get(config, "node.0"));
    printf("gpdb.guid=%s\n", config_get(config, "gpdb.guid"));
    printf("gpdb.encoding=%s\n", config_get(config, "gpdb.encoding"));
    printf("gpdb.seg.m.0=%s\n", config_get(config, "gpdb.seg.m.0"));
    printf("gpdb.seg.0.0=%s\n", config_get(config, "gpdb.seg.0.0"));
    printf("gpdb.seg.0.1=%s\n", config_get(config, "gpdb.seg.0.1"));
    printf("gpdb.seg.master_port=%s\n", config_get(config, "gpdb.seg.master_port"));

    printf("gpcc_net_start...\n");

    if (0 != (e = gpcc_net_start(8081, net_opened, net_closed, net_recv, net_tick, 0))) {
	gpcc_warning(FLINE, "gpcc_net_start failed");
	return e;
    }


    if (0 != (e = apr_threadattr_create(&attr, pool))) {
	gpcc_warning(FLINE, "apr_threadattr_create failed");
	return e;
    }

    if (0 != (e = apr_thread_create(&thread, attr, fts_cmd, 0, pool))) {
	gpcc_warning(FLINE, "apr_thread_create failed");
	return e;
    }


    alertq = gpcc_evqueue_create(100, pool);


    printf("fts_initialize...\n");

    fts_initialize(config, alertq);

    //    event_dispatch();
    
    printf("enter loop\n");
    for (;;) {
	if ((e = gpcc_net_once()) < 0) {
	    printf("gpcc_net_once error\n");
	    break;
	}
    }
    printf("exit loop\n");
    

    gpcc_net_stop();

    fts_terminate();

    apr_terminate();

    return 0;
}
