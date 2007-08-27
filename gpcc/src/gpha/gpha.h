#ifndef GPHA_H
#define GPHA_H

#include "config.h"
#include "enum.h"
#include <apr_thread_proc.h>


#define DEFAULT_QD_MAX_CONNECT  25
#define DEFAULT_QE_MAX_CONNECT  25
#define DEFAULT_QD_MAX_BUFFER   "128mb"
#define DEFAULT_QE_MAX_BUFFER   "128mb"
#define DEFAULT_MA_PORT   	12000
#define DEFAULT_QD_PORT   	12100
#define DEFAULT_QE_BASEPORT	12200

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
	    
	} a;

	struct {
	    const char *segment;
	    int port;
	    char *data_dir;
	    int primary;
	    int valid;

	    struct {
		apr_pool_t *subpool;
		struct json_object *msg;
		int parentidx;
		char *action;
		char *mode;
		char *data_dir;
		int port;
		int exitcode;
		char *reason;
		apr_proc_t proc;

		union {
		    struct {
		    } initdb;
		    
		    struct {
			
		    } startdb;
		    
		    struct {
			
		    } stopdb;
		    
		    struct {
			
		    } statusdb;
		    
		} u;
	    } request;
	} seg;


	struct {
	    
	} ma;
    } u;
};


typedef struct eventinfo_t eventinfo_t;
struct eventinfo_t {
    conninfo_t* conninfo;
};


/* Control block for GPHA */
typedef struct gpha_t gpha_t;
struct gpha_t {
    apr_pool_t*  pool;
    const char*  guid;		/* GUID */
    const char*  maid;		/* MA ID */
    const char*  dir;		/* MA data & log directory */
    config_t*    cfg;		/* config  */
    apr_uint64_t csn;		/* change seq number of config file */
    int          boot_flag;	/* TRUE if booting */
    int          debug;		/* TRUE if debug mode */
    const char*  logfpath;	/* path to log file */
    gpcc_evqueue_t* evq;	/* the event queue */
    int          system_cmdidx; /* cmdidx of system FSM */
    apr_uint16_t cmdidx_gen;
    const char*  gphome;
    cmdslot_t    cmdslot[CMDSLOT_MAX];
};

extern gpha_t gpha;

APR_DECLARE(void) fsm_setup(void);
APR_DECLARE(cmdslot_t*) cmdslot_alloc(const char* name, 
				      cmdstate_t init_state, conninfo_t* conninfo);
APR_DECLARE(void) cmdslot_free(cmdslot_t* cp);



#endif /* GPHA_H */
