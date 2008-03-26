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
    char*addresses;
    char id[CHUBBY_ID_SIZE];
    int64_t count;
    int leaderValid;
    struct sockaddr_in leader;
};

void chubby_counted_set(void*a,void*b);
void chubby_counted_close(void*a);
chubby_connection_t*chubby_connection_new(chubby_t*chubby);
void chubby_getNewId(chubby_t*thiz,char*a);
int chubby_propose(chubby_t*thiz,chubby_string_t**answer,chubby_string_t*what);

int chubby_udp_buffer_isOverflowed(chubby_udp_buffer_t*thiz);
void chubby_udp_buffer_format(chubby_udp_buffer_t*thiz,char*format,...);
void chubby_udp_buffer_cat(chubby_udp_buffer_t*thiz,chubby_string_t*a);

chubby_string_t*chubby_string_format(const char*format,...);

#endif
