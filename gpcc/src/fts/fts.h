#ifndef FTS_H_
#define FTS_H_

#include <gpcc.h>
#include <apr_queue.h>

int fts_initialize(apr_queue_t *alertq);

/**
 *
 *   msgtype:
 *      setup - first try to connect HAs.  if failed, startup HAs and connect again.
 *      init  - initdb
 *      start - start gpdb
 *      stop  - stop gpdb
 */ 
int fts_do(const char *msg, apr_queue_t *outq);



int fts_terminate();




#endif
