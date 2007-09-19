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


void* net_opened(gpcc_net_conn_t conn, void* conn_arg, 
		 int self_origin,
		 apr_pool_t* pool)
{
    gpcc_warning(FLINE, "local net opened");
    return 0;
}

/** callback when a connection is closed */
void net_closed(gpcc_net_conn_t conn, void* conn_arg)
{
    gpcc_warning(FLINE, "local net closed");
}

/** callback when bytes arrives */
void net_recv(gpcc_net_conn_t conn, void* conn_arg)
{
    gpcc_warning(FLINE, "local net recv");
}

/** callback every second */
void net_tick(apr_time_t now)
{
    ;   
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
    
    if (0 != (e = gpcc_net_start(8080, net_opened, net_closed, net_recv, net_tick, 0))) {
	gpcc_warning(FLINE, "gpcc_net_start failed");
	return e;
    }
    
    alertq = gpcc_evqueue_create(100, pool);

    outq = gpcc_evqueue_create(100, pool);

    fts_initialize(config, alertq);

    for (;;) {
	if (gpcc_net_once())
	    break;
	
	if (!started) {
	    fts_do("start", outq);
	    started = 1;
	}
    }


    gpcc_net_stop();

    fts_terminate();

    apr_terminate();

    return 0;
}
