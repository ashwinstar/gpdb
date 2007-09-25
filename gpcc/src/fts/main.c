#include <stdio.h>
#include <stdlib.h>
#include <gpcc_strings.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>
#include <gpcc_event.h>
#include <gpcc_net.h>
#include <json/json.h>
#include <event.h>
#include "fts.h"


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


int main(int argc, const char *argv[]) 
{
    apr_pool_t *pool;
    gpcc_evqueue_t *alertq = 0;
    gpcc_evqueue_t *outq = 0;
    int e;
    int started = 0;
    config_t *config = 0;

    apr_app_initialize(&argc, (char const *const **) &argv, 0);

    apr_pool_create(&pool, 0);

    event_init();

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

    printf("fts_initialize...\n");
    alertq = gpcc_evqueue_create(100, pool);

    outq = gpcc_evqueue_create(100, pool);

    fts_initialize(config, alertq);


    printf("enter loop\n");
    for (;;) {
	for (;;) {
	    if ((e = gpcc_net_once()))
		break;
	}
	apr_sleep(10);
	//printf("gpcc_net_once exit=%d\n", e);
	/*
	if (!started) {
	    fts_do("start", outq);
	    started = 1;
	}
	*/
    }
    printf("exit loop\n");

    gpcc_net_stop();

    fts_terminate();

    apr_terminate();

    return 0;
}
