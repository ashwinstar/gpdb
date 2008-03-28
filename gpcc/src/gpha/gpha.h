#ifndef GPHA_H
#define GPHA_H

#include "enum.h"
#include <apr_thread_proc.h>

#define CMDSLOT_MAX    512

#define CONFIG_DBNAME_KEY                        "array_name"
#define CONFIG_USER_KEY                          "user"
#define CONFIG_PASSWORD_KEY                      "password"
#define CONFIG_MAX_CONNECT_KEY                   "max_connect"
#define CONFIG_ENCODING_KEY                      "encoding"
#define CONFIG_LOCALE_KEY                        "locale"
#define CONFIG_SHARED_BUFFERS_KEY                "shared_buffers"
#define CONFIG_CHECKPOINT_SEGMENT_KEY            "checkpoint_segments"


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


typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    int 	cmdidx;
    char        cmdname[32];
    cmdstate_t  cmdstate;
    int         countdown;	/* if > 0, will count down to 0 
				   and generate a TIMEUP_EVENT */

    gpcc_net_conn_t conn;

    apr_pool_t *pool;

    union {
	struct {
	    
	} a;

	struct {
	    char *segment;
	    int port;
	    int dbid;
	    int content;
	    int exitcode;
	    int primary;
	    int valid;
	    int isqd;
	    int idx;
	    struct json_object *msg;
	    int parentidx;
	    int maidx;
	    char *action;
	    char *mode;
	    char *data_dir;
	    char *reason;
	    apr_proc_t proc;
	} request;
	
	struct {
	} ma;
    } u;
};


/* Control block for GPHA */
typedef struct gpha_t gpha_t;
struct gpha_t {
    apr_pool_t*  pool;
    const char*  guid;          /* GUID */
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
    cmdslot_t*   ma_cmdslot;    /* MA cmdslot */
};

extern gpha_t gpha;

APR_DECLARE(void) fsm_setup(void);
APR_DECLARE(cmdslot_t*) cmdslot_alloc(const char* name, 
				      cmdstate_t init_state, gpcc_net_conn_t conn);
APR_DECLARE(void) cmdslot_free(cmdslot_t* cp);



#endif /* GPHA_H */
