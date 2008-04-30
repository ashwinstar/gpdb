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

#define CHUBBY_CHUNK_SIZE	4000

#define CHUBBY_E_NOENT		1
#define CHUBBY_E_ISDIR		2
#define CHUBBY_E_IFFAILED	3
#define CHUBBY_E_INVAL		4
#define CHUBBY_E_LOCKEXPIRED	CHUBBY_E_IFFAILED
#define CHUBBY_E_TIMEOUT	101
#define CHUBBY_E_OOM		102
#define CHUBBY_E_IO		103

typedef struct chubby_connection chubby_connection_t;

typedef struct{
    int length;
    char a[1];
}chubby_string_t;

struct chubby_stat{
    int64_t decreeNumber;
    int size;
};

void chubby_close(zhandle_t*thiz);
chubby_connection_t*chubby_makeConnection(zhandle_t*thiz);

int chubby_connection_get(chubby_connection_t*thiz,struct chubby_stat*stat,chubby_string_t**answer,const char*name);
int chubby_connection_stat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name);
int chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_string_t*contents);
int chubby_connection_putBigFile(chubby_connection_t*thiz,const char*name,chubby_string_t*contents);
int chubby_connection_remove(chubby_connection_t*thiz,const char*name);
int chubby_connection_list(chubby_connection_t*thiz,char***answer,const char*name);
void chubby_connection_listFree(chubby_connection_t*thiz,char**answer);
chubby_connection_t*chubby_connection_getLockFilenameDecreeNumberConnection(chubby_connection_t*thiz,const char*name,int64_t decreeNumber);
int chubby_connection_getLockStringConnection(chubby_connection_t*thiz,chubby_connection_t**answer,const char*lockString);
int chubby_connection_getLockString(chubby_connection_t*thiz,chubby_string_t**answer);
int chubby_connection_checkLock(chubby_connection_t*thiz);
int chubby_connection_lock(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int timeout,int lockTime);
int chubby_connection_renew(chubby_connection_t*thiz,int lockTime);
int chubby_connection_getLockExpireTime(chubby_connection_t*thiz,time_t*time);
void chubby_connection_giveUpLock(chubby_connection_t*thiz);
void chubby_connection_close(chubby_connection_t*thiz);

extern void*(*chubby_malloc)(size_t);
extern void(*chubby_free)(void*);

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

struct _zhandle{
    struct chubby_counted super;
    char*addresses;
    char id[CHUBBY_ID_SIZE];
    int64_t count;
    int leaderValid;
    struct sockaddr_in leader;
};

void chubby_counted_set(void*a,void*b);
void chubby_counted_close(void*a);
chubby_connection_t*chubby_connection_new(zhandle_t*chubby);
void chubby_getNewId(zhandle_t*thiz,char*a);
int chubby_propose(zhandle_t*thiz,chubby_string_t**answer,chubby_string_t*what);

int chubby_udp_buffer_isOverflowed(chubby_udp_buffer_t*thiz);
void chubby_udp_buffer_format(chubby_udp_buffer_t*thiz,char*format,...);
void chubby_udp_buffer_cat(chubby_udp_buffer_t*thiz,chubby_string_t*a);

chubby_string_t*chubby_string_format(zhandle_t*chubby,const char*format,...);

#endif
