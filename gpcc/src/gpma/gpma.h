#ifndef GPMA_H
#define GPMA_H

#include "config.h"
#include "enum.h"


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
    union {
	struct {
	    
	} a;
    } u;
};


typedef struct eventinfo_t eventinfo_t;
struct eventinfo_t {
    conninfo_t* conninfo;
};


/* Control block for GPMA */
typedef struct gpma_t gpma_t;
struct gpma_t {
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
    cmdslot_t    cmdslot[CMDSLOT_MAX];
};

extern gpma_t gpma;



#endif /* GPMA_H */
