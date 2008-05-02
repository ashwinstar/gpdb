/* header.h */

#ifndef HEADER_H
#define HEADER_H

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <chubby.h>
#include <zookeeper.h>

#include "memory.h"

struct _zhandle{
    chubby_t*chubby;
    chubby_connection_t*c;
    watcher_fn fn;
    void*context;
};

#endif
