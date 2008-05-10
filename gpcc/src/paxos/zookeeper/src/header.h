/* header.h */

#ifndef HEADER_H
#define HEADER_H

#include <apr.h>
#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <chubby.h>
#include <zookeeper_extra.h>

#include "mangle.h"
#include "memory.h"
#include "zframe.h"

#define ZOOKEEPER_LOCKTIME	10

struct _zhandle{
    pthread_mutex_t mutex;
    chubby_connection_t*c;
    chubby_connection_t*lease;
    chubby_connection_t*l;
    watcher_fn fn;
    void*context;
    int recv_timeout;
};

struct zoostat{
    int64_t create_dn;
    int32_t version;
};

int zoochubby_set1(chubby_connection_t*l,const char*name,chubby_string_t*string);

static inline int64_t
zoo_htonll(int64_t a)
{
    union{
        uint32_t a[2];
	int64_t b;
    }u;
    u.a[0] = htonl((uint32_t)(a>>32));
    u.a[1] = htonl((uint32_t)a);
    return u.b;
}

static inline int64_t
zoo_ntohll(int64_t a)
{
    union{
        uint32_t a[2];
	int64_t b;
    }u;
    u.b = a;
    return (int64_t)ntohl(u.a[0])<<32|ntohl(u.a[1]);
}

#endif
