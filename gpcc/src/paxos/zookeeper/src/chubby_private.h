/* chubby_private.h */

#ifndef CHUBBY_PRIVATE_H
#define CHUBBY_PRIVATE_H

#include <apr.h>
#include <assert.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <zookeeper.h>

#include "sign.h"
#include "chubby_counted.h"
#include "chubby_string.h"
#include "memory.h"

#define CHUBBY_CHUNK_SIZE	4000

#define CHUBBY_E_NOENT		1
#define CHUBBY_E_ISDIR		2
#define CHUBBY_E_IFFAILED	3
#define CHUBBY_E_INVAL		4
#define CHUBBY_E_MARSHALLINGERROR	101

#define NUMBER_STRING_LENGTH	20
#define CHUBBY_ID_SIZE		(_POSIX_HOST_NAME_MAX+3*NUMBER_STRING_LENGTH+3)

struct chubby_stat{
    int64_t decreeNumber;
    int size;
};

struct _zhandle{
    struct chubby_counted super;
    char*addresses;
    char*paxosKey;
    char id[CHUBBY_ID_SIZE];
    int64_t count;
    int leaderValid;
    struct sockaddr_in leader;
    watcher_fn fn;
    void*context;
};

void chubby_getNewId(zhandle_t*thiz,char*a);
int chubby_propose(zhandle_t*thiz,chubby_string_t**answer,chubby_string_t*what);

int chubby_stat(zhandle_t*thiz,struct chubby_stat*stat,const char*name);

#endif
