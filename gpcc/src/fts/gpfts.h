#ifndef GPFTS_H
#define GPFTS_H

#include "enum.h"
#include <apr_thread_proc.h>
#include <gpcc_net.h>
#include <gpcc_event.h>
#include <gpcc_ha.h>
#include <event.h>
#include "config.h"

#define DEFAULT_QD_MAX_CONNECT  25
#define DEFAULT_QE_MAX_CONNECT  25
#define DEFAULT_QD_MAX_BUFFER   "128MB"
#define DEFAULT_QE_MAX_BUFFER   "128MB"
#define DEFAULT_MA_PORT   	12000
#define DEFAULT_QD_PORT   	12100
#define DEFAULT_QE_BASEPORT	12200
#define DEFAULT_MIRROR_QE_BASEPORT  22200
#define CMDSLOT_MAX    512



/* connection info, this is stored in gpcc_net via the conn_arg */
typedef struct conninfo_t conninfo_t;
struct conninfo_t {
    apr_pool_t* pool;
    gpcc_net_conn_t conn;
    int    self_origin;
    struct {
	role_t role;
	const char* rolespec;
	gpcc_net_node_id_t* node_id;
	gpcc_net_request_t* request;
    } peer;
    struct {
	role_t role;
	const char*  rolespec;		/* for QE, "0.1", "m.0", etc
					   for MA, "lead" or "",
					*/
	gpcc_net_request_t request;
    } self;
};


typedef struct hainfo_t hainfo_t;
struct hainfo_t {
    int index;
    gpcc_ha_t *ha;
    char host[1];
};

typedef struct seginfo_t seginfo_t;
struct seginfo_t {
    char *name;
    char *host;
    int dbid;
    int content;
    int port;
    char *dir;
    int haidx;
};

typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    int 	cmdidx;
    const char* cmdname;
    cmdstate_t  cmdstate;
    int         countdown;	/* if > 0, will count down to 0 
				   and generate a TIMEUP_EVENT */
    conninfo_t* conninfo;

    apr_pool_t *pool;

    union {
	struct {
	    hainfo_t **hainfo;
	    int nhainfo;
	    seginfo_t *seginfo;
	    int nseginfo;
	    struct json_object *config_json;
	    int network_failure;

	    struct {
		apr_pool_t *pool;
		gpcc_evqueue_t *outq;
		struct json_object *reqobj;

		union {
		    struct {
			int nreply;
			int nconnected;
			int ndisconnected;
			char **hosts;
			int nhost;
			apr_proc_t *proc;
		    } setup;

		    struct {
			char *mode;
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
	    } request;

	} sys;

	struct {
	    
	} a;

    } u;
};


typedef struct eventinfo_t eventinfo_t;
struct eventinfo_t {
    conninfo_t* conninfo;
};


/* Control block for GPFTS */
typedef struct gpfts_t gpfts_t;
struct gpfts_t {
    apr_pool_t*  pool;
    const char*  dir;		/* HA data & log directory */
    int          port;          /* HA port number */
    int          boot_flag;	/* TRUE if booting */
    int          debug;		/* TRUE if debug mode */
    const char*  logfpath;	/* path to log file */
    gpcc_evqueue_t* evq;	/* the event queue */
    gpcc_evqueue_t* alertq;     /* the alert queue */
    int          system_cmdidx; /* cmdidx of system FSM */
    apr_uint16_t cmdidx_gen;
    const char*  gphome;
    cmdslot_t    cmdslot[CMDSLOT_MAX];
    apr_thread_t *thread;
    apr_threadattr_t *attr;
    struct event timer_event;
    config_t     *config;
    apr_uint32_t spin[1];
    apr_uint32_t net_spin[1];
};

extern gpfts_t gpfts;

APR_DECLARE(void) fsm_setup(void);
APR_DECLARE(cmdslot_t*) cmdslot_alloc(const char* name, 
				      cmdstate_t init_state, conninfo_t* conninfo);
APR_DECLARE(void) cmdslot_free(cmdslot_t* cp);



#endif /* GPFTS_H */
