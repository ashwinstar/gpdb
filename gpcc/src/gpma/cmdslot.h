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
	    seginfo_t* seginfo;
	    int nseginfo;
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
	    } *req;		/* one for each unit, total of ntotal units */
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
	} status;

	struct {
	    gpcc_net_conn_t conn; /* if we need to send reply over http */
	    apr_uint32_t parent_cmdidx;
	} array_init;

	struct {
	    gpcc_net_conn_t conn; /* if we need to send reply over http */
	    apr_uint32_t parent_cmdidx;
	} segment_init;


	struct {
	    gpcc_net_conn_t conn; /* if we need to send reply over http */
	    apr_uint32_t parent_cmdidx;
	} array_start;

	struct {
	    gpcc_net_conn_t conn; /* if we need to send reply over http */
	    apr_uint32_t parent_cmdidx;
	} segment_start;

	struct {
	    gpcc_net_conn_t conn; /* if we need to send reply over http */
	    apr_uint32_t parent_cmdidx;
	} array_stop;

	struct {
	    gpcc_net_conn_t conn; /* if we need to send reply over http */
	    apr_uint32_t parent_cmdidx;
	} segment_stop;

	
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
APR_DECLARE(void) cmdslot_broadcast_fts_net_closed(const char *node);


#endif /* CMDSLOT_H */
