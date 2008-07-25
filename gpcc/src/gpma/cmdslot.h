#ifndef CMDSLOT_H
#define CMDSLOT_H

#include <gpcc.h>
#include <apr_hash.h>
#include <apr_md5.h>
#include <apr_time.h>
#include "enum.h"
#include "config.h"
#include <json/json.h>
#include <apr_thread_proc.h>
#include <gpcc_net.h>
#include <chubby.h>

#define CMDSLOT_TABLE_MAX  512


typedef struct seginfo_t seginfo_t;
struct seginfo_t {
    char *name;
    char *host;
    char *hostname;
    int dbid;
    int content;
    int port;
    char *dir;
    int haidx;
    char *ha;
    char valid;
    char isprimary;
    char definedprimary;
};

typedef struct user_t user_t;
struct user_t {
    apr_size_t datasz;
    apr_time_t login_time;
    apr_time_t expire_time;
    char key[1];
};
    
typedef struct chubby_result_t chubby_result_t;
struct chubby_result_t {
    int retval;
    union {
	struct {
	    struct chubby_stat stat;
	    chubby_string_t content;
	} get;

	struct {
	    struct chubby_stat stat;
	} stat;

	struct {
	    chubby_connection_t *conn;
	} lease;

    } u;
};

typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    apr_uint32_t cmdidx;
    char         cmdname[32];
    cmdstate_t   cmdstate;
    int          countdown;	/* if > 0, will count down to 0 
	                           and generate a TIMEUP_EVENT */

    apr_pool_t *pool;
    
    union {

	struct {
	    apr_hash_t *ht;
	    seginfo_t* seginfo;
	    int nseginfo;
	    int nopened;
	    int nnode;
	    apr_uint32_t active_cmdidx;
	} fts;

	struct {
	    char *config;
	    /* use this to send a command to all nodes */
	    apr_uint32_t parent_cmdidx;
	    int idx;
	    int ntotal;
	    int nsuccess, nfailed; /* finished -> nsuccess + nfailed == ntotal */
	    struct {
		char* node;	/* which node to send to (e.g. node.2) */
		char* unit;	/* which segment (e.g. seg.2.0)  */
		char* mode;
		char* host;
		int port;	/* which port for this segment */
		char* datadir;	/* which dir for this segment */
		int dbid;
		int content;
		/* other stuff here */
		char* reply;	/* reply from the start command */
		int status;     /* 0 - not started, 1 - running, 2 - finished */
	    } *req;		/* one for each unit, total of ntotal units */
	    struct {
		char *unit;
		int requesting;
	    } *node;
	} initdb;


	struct {
	    /* use this to send a command to all nodes */
	    apr_uint32_t parent_cmdidx;
	    int ntotal;
	    int nsuccess, nfailed; /* finished -> nsuccess + nfailed == ntotal */
	    struct {
		char* node;	/* which node to send to (e.g. node.2) */
		char* unit;	/* which segment (e.g. seg.2.0)  */
		char* mode;
		char* host;
		int port;	/* which port for this segment */
		char* datadir;	/* which dir for this segment */
		char  isprimary;
		char  valid;
		int   rotate_log;  /* rotate log or not */
		/* other stuff here */
		char* reply;	/* reply from the start command */
	    } *req;		/* one for each unit, total of ntotal units */
	} start;


	struct {
	    /* use this to send a command to all nodes */
	    apr_uint32_t parent_cmdidx;
	    int ntotal;
	    int nsuccess, nfailed; /* finished -> nsuccess + nfailed == ntotal */
	    struct {
		char* node;	/* which node to send to (e.g. node.2) */
		char* unit;	/* which segment (e.g. seg.2.0)  */
		char* mode;
		char* host;
		int port;	/* which port for this segment */
		char* datadir;	/* which dir for this segment */
		char  isprimary;
		char  valid;
		/* other stuff here */
		char* reply;	/* reply from the start command */
	    } *req;		/* one for each unit, total of ntotal units */
	} stop;

	struct {
	    /* use this to send a command to all nodes */
	    apr_uint32_t parent_cmdidx;
	    int ntotal;
	    int nsuccess, nfailed; /* finished -> nsuccess + nfailed == ntotal */
	    struct {
		char* node;	/* which node to send to (e.g. node.2) */
		char* unit;	/* which segment (e.g. seg.2.0)  */
		char* mode;
		char* host;
		int port;	/* which port for this segment */
		char* datadir;	/* which dir for this segment */
		/* other stuff here */
		char* reply;	/* reply from the start command */
	    } *req;		/* one for each unit, total of ntotal units */
	} delete;

	struct {
	    /* use this to send a command to all nodes */
	    apr_uint32_t parent_cmdidx;
	    int ntotal;
	    int nsuccess, nfailed; /* finished -> nsuccess + nfailed == ntotal */
	    struct {
		char* node;	/* which node to send to (e.g. node.2) */
		char* unit;	/* which segment (e.g. seg.2.0)  */
		char* mode;
		char* host;
		int port;	/* which port for this segment */
		char* datadir;	/* which dir for this segment */
		/* other stuff here */
		char* reply;	/* reply from the start command */
	    } *req;		/* one for each unit, total of ntotal units */
	} status;

	struct {
	    apr_uint32_t parent_cmdidx;
	    apr_proc_t proc;
	} array_init;

	struct {
	    apr_uint32_t parent_cmdidx;
	} segment_init;


	struct {
	    apr_uint32_t parent_cmdidx;
	    struct {
		int content;
		char *definedprimary;
		int dbid;
		char *isprimary;
		char *valid;
		char *hostname;
		int port;
		char *datadir;
	    } *gpconf;
	    int nvalid;
	    char *mode;
	    int nostandbyqd;
	    int rotate_qd_log;
	    int rotate_qes_log;
	} array_start;

	struct {
	    apr_uint32_t parent_cmdidx;
	    struct {
		int content;
		char *definedprimary;
		int dbid;
		char *isprimary;
		char *valid;
		char *hostname;
		int port;
		char *datadir;
	    } *gpconf;
	    int nconf;
	    int nvalid;
	    char **unit;
	    int nunit;
	    char *mode;
	} segment_start;

	struct {
	    apr_uint32_t parent_cmdidx;
	    char *mode;
	} array_stop;

	struct {
	    apr_uint32_t parent_cmdidx;
	    char *mode;
	    char **unit;
	    int nunit;
	} segment_stop;

	struct {
	    apr_uint32_t parent_cmdidx;
	    char *mode;
	} array_delete;

	
	struct {
	    gpcc_net_conn_t conn;
	    apr_proc_t proc;
	    /* host IPs belonging to this node */
	    int hosttab_max;	/* size of hosttab */
	    int    hostidx;
	    char** hosttab;	/* array of IP */
	    int opened;
	    int valid;  /* valid if at least one segment is valid */
	} node;

	struct {
	    gpcc_net_conn_t peer_conn;
	    gpcc_net_conn_t self_conn;
	    gpcc_net_conn_t conn;
	    int port;
	    const char* ma_unit;
	    const char* node_unit;
	} ma;

	struct {
	    char *leader;
	    struct {
		chubby_connection_t *conn;
		chubby_connection_t *lease;
	    } chubby;
	} lma;

	struct {
	    int total;
	    int nopened;
	    struct {
		int opened;
	    } *matab;
	} maset;

	struct {
	    int total;
	    int nopened;
	    int ninvalid;
	    struct {
		int opened;
	    } *nodetab;
	} haset;
	

	struct {
	    int content;
	    int definedprimary;
	    int dbid;
	    int isprimary;
	    int valid;
	    char *host;
	    int port;
	    char *datadir;
	    int nodeid;
	    char *node;
	    int parentidx;
	} unit;

	struct {
	    apr_hash_t *usertb;
	} user;

	struct {
	    char *sid;
	    int expire;
	    char *username;
	    char *password;
	    char *host;
	    char *passwdfile;
	    gpcc_net_conn_t conn;

	    struct {
		chubby_connection_t *conn;
	    } chubby;
	} login;

	struct {
	    char *username;
	    char *password;
	    char *new_password;
	    gpcc_net_conn_t conn;

	    struct {
		chubby_connection_t *conn;
		chubby_connection_t *lease;
	    } chubby;
	} admin;


	struct {
	    apr_uint32_t parent_cmdidx;
	    int timeout;
	    char **hosts;
	    char **hostname;
	    int nhost;
	    int nthread;
	    int nsuccess;
	    int nfailure;
	    int start;
	    int offset;
	    apr_proc_t *proc;
	} gethostname;

	struct {
	    apr_uint32_t parent_cmdidx;
	    int timeout;
	    char **hosts;
	    char **address;
	    int nhost;
	    int nthread;
	    int nsuccess;
	    int nfailure;
	    int start;
	    int offset;
	    apr_proc_t *proc;
	} getaddress;
    } u;
};


APR_DECLARE(void) cmdslot_init(apr_pool_t* pool);
APR_DECLARE(cmdslot_t*) cmdslot_alloc(const char* name, cmdstate_t state);
APR_DECLARE(cmdslot_t*) cmdslot_find(apr_uint32_t cmdidx);
APR_DECLARE(void) cmdslot_free(apr_uint32_t cmdidx);
APR_DECLARE(void) cmdslot_tick(void);
APR_DECLARE(void) cmdslot_broadcast_fts_net_closed(const char *node);


#endif /* CMDSLOT_H */
