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
    apr_time_t now;
    apr_time_t nextTime;
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

int chubby_asynchronous_propose(chubby_t*thiz,void(*callback)(void*data,int e,chubby_string_t*answer),void*data,chubby_string_t*what);

int chubby_udp_buffer_isOverflowed(chubby_udp_buffer_t*thiz);
void chubby_udp_buffer_format(chubby_udp_buffer_t*thiz,char*format,...);
void chubby_udp_buffer_cat(chubby_udp_buffer_t*thiz,chubby_string_t*a);

chubby_string_t*chubby_string_format(chubby_t*chubby,const char*format,...);
chubby_string_t*chubby_string_copy(chubby_t*chubby,chubby_string_t*s);

#define SIGNSTRING		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx "
#define SIGNSTRING_LENGTH	33

int chubby_readPaxosKey(chubby_t*thiz);
void chubby_sign(chubby_t*thiz,char*a,const char*b,int len,const struct sockaddr*f,const struct sockaddr*t);
void chubby_fixFilename(chubby_udp_buffer_t*b,const char*name);
int chubby_connection_asynchronous_propose(chubby_connection_t*thiz,void(*callback)(void*data,int e,chubby_string_t*answer),void*data,chubby_udp_buffer_t*b);
int chubby_asynchronous_stat(chubby_connection_t*thiz,void(*callback)(void*data,int e,struct chubby_stat*stat),void*data,const char*name);

struct chubby_two_int{
    int done;
    int e;
};

int chubby_two_int_finish(chubby_t*thiz,struct chubby_two_int*data,int e);
void chubby_two_int_callback(void*_data,int e);

struct super_data{
    chubby_connection_t*c;
    void(*free)(void*data);
    void*callback;
    void*data;
};

void*super_data_new(int size,chubby_connection_t*c,void*callback,void*dat,void(*free)(void*data));
void super_data_done_nothing(struct super_data*data,int e);
void super_data_delete(struct super_data*data);
void super_data_done_stat_string(struct super_data*data,int e,struct chubby_stat*stat,chubby_string_t*string);
void super_data_done_charstarstar(struct super_data*data,int e,char**charstarstar);
void super_data_done_stat(struct super_data*data,int e,struct chubby_stat*stat);
#endif
