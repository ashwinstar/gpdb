/* header.h */

#ifndef HEADER_H
#define HEADER_H

#include <apr.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <chubby.h>
#include <zookeeper.h>

#include "mangle.h"
#include "memory.h"
#include "zframe.h"
#include "version.h"

#define ZOOKEEPER_LOCKTIME	10

struct _zhandle{
    chubby_t*chubby;
    chubby_connection_t*c;
    watcher_fn fn;
    void*context;
    int recv_timeout;
};

#endif
