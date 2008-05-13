#ifndef GPMA_H
#define GPMA_H

#include "config.h"
#include "enum.h"
#include "cmdslot.h"
#include <gpcc_event.h>
#include <gpcc_net.h>

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

#define QD_NULL_STATUS                             0
#define QD_STANDBY_STATUS                          1
#define QD_MASTER_STATUS                           2

/* Control block for GPMA */
typedef struct gpma_t gpma_t;
struct gpma_t {
    apr_pool_t*  pool;
    const char*  hostname;    /* hostname */
    const char*  user;        /* login user name */
    const char*  progname;    /* argv[0] of cmdline  */
    const char*  guid; 	      /* Global unique ID (generated from gpdb_id) */
    const char*  array_name;  /* array name */
    const char*  ma_unit;     /* MA ID - e.g. ma.0 */
    int          ma_port;     /* MA Port */
    const char*  ma_addr[5];  /* MA Address */
    const char*  px_addr[5];  /* Paxos Address */
    const char*  px_address;  /* Zookeeper Addresss based on px_addr */
    const char*  datadir;     /* dir for config file, log file, etc */
    const char*  master_datadir; /* master_data_dir */
    const char*  ha_datadir;  /* dir for HA */
    int          verbose;     /* TRUE if more verbose */
    int          daemonize;   /* TRUE if daemonize */
    gpcc_evqueue_t* evq;      /* event queue */
    const char*  gphome;
    const char*  logfpath;    /* log file path */
    config_t*    cfg;	      /* config  */
    cmdslot_t*   gpma_cmdslot;
    cmdslot_t*   fts_cmdslot;
    cmdslot_t*   haset_cmdslot;

    int          node_max;
    cmdslot_t**  node_cmdtab;

    int          px_max;
    cmdslot_t**  px_cmdtab;

    int          unit_max;
    cmdslot_t**  unit_cmdtab;

    cmdslot_t *  cfg_cmdslot;   /* configuration cmdslot */

    cmdslot_t * lma_cmdslot;    /* Lead MA cmdslot */
};

extern gpma_t gpma;

APR_DECLARE(void) fsm_init(void);

#endif /* GPMA_H */
