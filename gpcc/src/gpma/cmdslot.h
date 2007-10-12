#ifndef CMDSLOT_H
#define CMDSLOT_H

#include <gpcc.h>
#include "enum.h"
#include <json/json.h>
#include <apr_thread_proc.h>
#include <gpcc_net.h>

#define CMDSLOT_TABLE_MAX  512


typedef struct seginfo_t seginfo_t;
struct seginfo_t {
    char *name;
    char *host;
    int dbid;
    int content;
    int port;
    char *dir;
    int haidx;
    char *ha;
};

typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    apr_uint32_t cmdidx;
    const char*  cmdname;
    cmdstate_t   cmdstate;
    int          countdown;	/* if > 0, will count down to 0 
	                           and generate a TIMEUP_EVENT */

    apr_pool_t *pool;
    
    union {
	struct {
	    gpcc_net_conn_t conn;
	    seginfo_t *seginfo;
	    int nseginfo;
	    struct json_object *config_json;
	    int network_failure;

	    struct json_object *reqobj;
	    int sndidx;

	    union {
		struct {
		    char *mode;
		    int segidx;
		    int nreply;
		    int nsuccess;
		    int nfailed;
		    int *replied;
		} initdb;
		
		struct {
		    int nrequest;
		    int nreply;
		    int nsuccess;
		    int nfailed;
		    int *replied;
		} start;
		
		struct {
		    int nreply;
		    int nsuccess;
		    int nfailed;
		    int *replied;
		} stop;
		
		struct {
		    int nreply;
		    int nsuccess;
		    int nfailed;
		    int *replied;
		} status;
		
	    } u;
	} fts;

	struct {
	    gpcc_net_conn_t conn;
	    apr_proc_t proc;
	    /* host IPs belonging to this node */
	    int hosttab_max;	/* size of hosttab */
	    int    hostidx;
	    char** hosttab;	/* array of IP */
	} node;
    } u;
};


APR_DECLARE(void) cmdslot_init(apr_pool_t* pool);
APR_DECLARE(cmdslot_t*) cmdslot_alloc(const char* name, cmdstate_t state);
APR_DECLARE(cmdslot_t*) cmdslot_find(apr_uint32_t cmdidx);
APR_DECLARE(void) cmdslot_free(apr_uint32_t cmdidx);
APR_DECLARE(void) cmdslot_tick(void);



#endif /* CMDSLOT_H */
