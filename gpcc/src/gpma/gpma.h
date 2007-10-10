#ifndef GPMA_H
#define GPMA_H

#include "config.h"
#include "enum.h"
#include "cmdslot.h"
#include <gpcc_event.h>
#include <gpcc_net.h>

#define DEFAULT_QD_MAX_CONNECT  25
#define DEFAULT_QE_MAX_CONNECT  25
#define DEFAULT_QD_MAX_BUFFER   "128MB"
#define DEFAULT_QE_MAX_BUFFER   "128MB"
#define DEFAULT_MA_PORT   	12000
#define DEFAULT_QD_PORT   	12100
#define DEFAULT_QE_BASEPORT	12200
#define DEFAULT_MIRROR_QE_BASEPORT  22200
#define DEFAULT_HA_PORT  53333



/* Control block for GPMA */
typedef struct gpma_t gpma_t;
struct gpma_t {
    apr_pool_t*  pool;
    const char*  progname;    /* argv[0] of cmdline  */
    const char*  guid; 	      /* Global unique ID (generated from gpdb_id) */
    const char*  ma_unit;     /* MA ID - e.g. ma.0 */
    const char*  datadir;	      /* dir for config file, log file, etc */
    const char*  boot_fname;  /* NULL if not booting, otherwise the name of the cfgfile */
    int          debug;	      /* TRUE if debug run, don't daemonize */
    gpcc_evqueue_t* evq;      /* event queue */
    config_t*    cfg;	      /* config  */
    cmdslot_t*   gpma_cmdslot;

    int          node_max;
    cmdslot_t**  node_cmdtab;
};

extern gpma_t gpma;

APR_DECLARE(void) fsm_init(void);

#endif /* GPMA_H */
