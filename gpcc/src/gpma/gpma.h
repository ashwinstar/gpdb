#ifndef GPMA_H
#define GPMA_H

#include "config.h"
#include "enum.h"
#include "cmdslot.h"
#include <gpcc_event.h>
#include <gpcc_net.h>

#define DEFAULT_HA_PORT  53333

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



/* Control block for GPMA */
typedef struct gpma_t gpma_t;
struct gpma_t {
    apr_pool_t*  pool;
    const char*  progname;    /* argv[0] of cmdline  */
    const char*  guid; 	      /* Global unique ID (generated from gpdb_id) */
    const char*  ma_unit;     /* MA ID - e.g. ma.0 */
    int          ma_port;     /* MA Port */
    const char*  datadir;     /* dir for config file, log file, etc */
    int          boot_force;  /* force the creation of boot_fname cfg file */
    const char*  boot_fname;  /* NULL if not booting, otherwise the name of the cfgfile */
    int          debug;	      /* TRUE if debug run, don't daemonize */
    gpcc_evqueue_t* evq;      /* event queue */
    const char*  gphome;
    const char*  logfpath;    /* log file path */
    config_t*    cfg;	      /* config  */
    cmdslot_t*   gpma_cmdslot;
    cmdslot_t*   fts_cmdslot;

    int          node_max;
    cmdslot_t**  node_cmdtab;

    int          ma_max;
    cmdslot_t**  ma_cmdtab;
};

extern gpma_t gpma;

APR_DECLARE(void) fsm_init(void);

#endif /* GPMA_H */
