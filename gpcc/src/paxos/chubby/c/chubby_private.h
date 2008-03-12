/* chubby_private.h */

#ifndef CHUBBY_PRIVATE_H
#define CHUBBY_PRIVATE_H

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include "chubby.h"

struct chubby_counted{
    int count;
    int closed;
    void(*delet)(struct chubby_counted*thiz);
};

struct chubby{
    struct chubby_counted super;
    chubby_sb_t addresses;
    chubby_sb_t id;
    int64_t count;
    int leaderValid;
    struct sockaddr leader;
};

void chubby_counted_set(void*a,void*b);
void chubby_counted_close(void*a);
chubby_connection_t*chubby_connection_new(chubby_t*chubby);
int chubby_getId(chubby_t*thiz,chubby_sb_t*dest);

#endif
