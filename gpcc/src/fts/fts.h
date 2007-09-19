#ifndef FTS_H_
#define FTS_H_

#include <gpcc.h>
#include <gpcc_event.h>
#include "config.h"

int fts_initialize(config_t *config, gpcc_evqueue_t *alertq);

/**
 *
 *   msgtype:
 *      setup - first try to connect HAs.  if failed, startup HAs and connect again.
 *      init  - initdb
 *      start - start gpdb
 *      stop  - stop gpdb
 */ 
int fts_do(const char *msg, gpcc_evqueue_t *outq);



int fts_terminate();




#endif
