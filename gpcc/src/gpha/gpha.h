#ifndef GPHA_H
#define GPHA_H

#include "enum.h"
#include <apr_thread_proc.h>

#define CMDSLOT_MAX    512

#define CONFIG_DBNAME_KEY                        "gpdb.name"
#define CONFIG_USER_KEY                          "gpdb.user"
#define CONFIG_PASSWORD_KEY                      "gpdb.password"
#define CONFIG_MAX_CONNECT_KEY                   "gpdb.max_connect"
#define CONFIG_ENCODING_KEY                      "gpdb.encoding"
#define CONFIG_LOCALE_KEY                        "gpdb.locale"
#define CONFIG_SHARED_BUFFERS_KEY                "gpdb.shared_buffers"
#define CONFIG_CHECKPOINT_SEGMENT_KEY            "gpdb.checkpoint_segments"
#define CONFIG_MASTER_PORT_KEY                   "gpdb.seg.master_port"
#define CONFIG_BASE_PORT_KEY                     "gpdb.seg.base_port"
#define CONFIG_MIRROR_BASE_PORT_KEY              "gpdb.seg.mirror_base_port"


#define CONFIG_DBNAME_VALUE                        "mygpdb"
#define CONFIG_USER_VALUE                          "user"
#define CONFIG_PASSWORD_VALUE                      "mpparray"
#define CONFIG_MAX_CONNECT_VALUE                   25
#define CONFIG_ENCODING_VALUE                      "SQL_ASCII"
#ifdef SOLARIS2
#define CONFIG_LOCALE_VALUE                        "en_US.UTF-8"
#else
#define CONFIG_LOCALE_VALUE                        "en_US.utf-8"
#endif
#define CONFIG_SHARED_BUFFERS_VALUE                "128MB"
#define CONFIG_CHECKPOINT_SEGMENT_VALUE            31
#define CONFIG_MASTER_PORT_VALUE                   12000
#define CONFIG_BASE_PORT_VALUE                     12100
#define CONFIG_MIRROR_BASE_PORT_VALUE              22200


typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    int 	cmdidx;
    const char* cmdname;
    cmdstate_t  cmdstate;
    int         countdown;	/* if > 0, will count down to 0 
				   and generate a TIMEUP_EVENT */

    gpcc_net_conn_t conn;

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
	    int isqd;
	    int idx;

	    struct {
		apr_pool_t *subpool;
		struct json_object *msg;
		int parentidx;
		char *action;
		char *mode;
		char *data_dir;
		int port;
		int dbid;
		int content;
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
	    int rcvidx;
	} ma;
    } u;
};


/* Control block for GPHA */
typedef struct gpha_t gpha_t;
struct gpha_t {
    apr_pool_t*  pool;
    const char*  dir;		/* HA data & log directory */
    int          port;          /* HA port number */
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
				      cmdstate_t init_state, gpcc_net_conn_t conn);
APR_DECLARE(void) cmdslot_free(cmdslot_t* cp);



#endif /* GPHA_H */
