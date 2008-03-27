/* chubby.h */

#ifndef CHUBBY_H
#define CHUBBY_H

#include <sys/types.h>

#define CHUBBY_CHUNK_SIZE	30000

#define CHUBBY_E_NOENT		1
#define CHUBBY_E_ISDIR		2
#define CHUBBY_E_IFFAILED	3
#define CHUBBY_E_INVAL		4
#define CHUBBY_E_LOCKEXPIRED	CHUBBY_E_IFFAILED
#define CHUBBY_E_TIMEOUT	101
#define CHUBBY_E_OOM		102
#define CHUBBY_E_IO		103

typedef struct chubby chubby_t;
typedef struct chubby_connection chubby_connection_t;

typedef struct{
    int length;
    char a[1];
}chubby_string_t;

struct chubby_stat{
    int64_t decreeNumber;
    int size;
};

chubby_t*chubby_new(const char*addresses,void*(*malloc)(size_t),void(*free)(void*));
void chubby_close(chubby_t*thiz);
chubby_connection_t*chubby_makeConnection(chubby_t*thiz);

int chubby_connection_get(chubby_connection_t*thiz,struct chubby_stat*stat,chubby_string_t**answer,const char*name);
int chubby_connection_stat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name);
int chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_string_t*contents);
int chubby_connection_putBigFile(chubby_connection_t*thiz,const char*name,chubby_string_t*contents);
int chubby_connection_remove(chubby_connection_t*thiz,const char*name);
int chubby_connection_list(chubby_connection_t*thiz,char***answer,const char*name);
void chubby_connection_listFree(chubby_connection_t*thiz,char**answer);
chubby_connection_t*chubby_connection_getLockFilenameDecreeNumberConnection(chubby_connection_t*thiz,const char*name,int64_t decreeNumber);
chubby_connection_t*chubby_connection_getLockStringConnection(chubby_connection_t*thiz,const char*lockString);
char*chubby_connection_getLockString(chubby_connection_t*thiz);
int chubby_connection_isLockValid(chubby_connection_t*thiz);
int chubby_connection_lock(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int timeout,int lockTime);
int chubby_connection_renew(chubby_connection_t*thiz,int lockTime);
time_t chubby_connection_getLockExpireTime(chubby_connection_t*thiz);
void chubby_connection_giveUpLock(chubby_connection_t*thiz);
void chubby_connection_close(chubby_connection_t*thiz);

#endif
