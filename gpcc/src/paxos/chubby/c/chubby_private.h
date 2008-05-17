/* chubby_private.h */

#ifndef CHUBBY_PRIVATE_H
#define CHUBBY_PRIVATE_H

#include <apr.h>
#include <apr_md5.h>
#include <apr_time.h>
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
#include "chubby.h"

#define NUMBER_STRING_LENGTH	20
#define CHUBBY_ID_SIZE		(_POSIX_HOST_NAME_MAX+3*NUMBER_STRING_LENGTH+3)

struct chubby_counted{
    int count;
    int closed;
    void(*delet)(struct chubby_counted*thiz);
};

// chubby_udp_buffer_t extends chubby_string_t.
typedef struct{
    int length;
    char a[65536];
}chubby_udp_buffer_t;

struct chubby{
    struct chubby_counted super;
    void*(*malloc)(size_t);
    void(*free)(void*);
    char*addresses;
    char*paxosKey;
    int timeout;
    int fd;
    struct chubby_proposal*proposal;
    apr_time_t getLeaderTimeout;
    struct sockaddr socketName;
    char id[CHUBBY_ID_SIZE];
    int64_t count;
    int leaderValid;
    struct sockaddr leader;
};

struct chubby_connection{
    struct chubby_counted super;
    chubby_t*chubby;
    chubby_connection_t*parent;
    char leaseFilename[PATH_MAX];
    int64_t leaseDn;
    char leaseId[CHUBBY_ID_SIZE];
    apr_time_t leaseExpireTime;
};

void chubby_counted_set(void*a,void*b);
void chubby_counted_close(void*a);

chubby_connection_t*chubby_connection_new(chubby_t*chubby);
void chubby_getNewId(chubby_t*thiz,char*a);

int chubby_asynchronous_propose(chubby_t*thiz,chubby_asynchronous_callback_t callback,void*data,chubby_string_t**answer,chubby_string_t*what);
int chubby_propose(chubby_t*thiz,chubby_string_t**answer,chubby_string_t*what);

int chubby_udp_buffer_isOverflowed(chubby_udp_buffer_t*thiz);
void chubby_udp_buffer_format(chubby_udp_buffer_t*thiz,char*format,...);
void chubby_udp_buffer_cat(chubby_udp_buffer_t*thiz,chubby_string_t*a);

chubby_string_t*chubby_string_format(chubby_t*chubby,const char*format,...);

#define SIGNSTRING		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx "
#define SIGNSTRING_LENGTH	33

int chubby_readPaxosKey(chubby_t*thiz);
void chubby_sign(chubby_t*thiz,char*a,const char*b,int len,const struct sockaddr*f,const struct sockaddr*t);
void chubby_fixFilename(chubby_udp_buffer_t*b,const char*name);
int chubby_connection_asynchronous_propose(chubby_connection_t*thiz,chubby_asynchronous_callback_t callback,void*data,chubby_string_t**answer,chubby_udp_buffer_t*b);
int chubby_connection_propose(chubby_connection_t*thiz,chubby_string_t**answer,chubby_udp_buffer_t*b);
void chubby_two_int_callback(int e,void*data);
int chubby_two_int_finish(chubby_t*thiz,int a[2],int e);

#endif
