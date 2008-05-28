/* chubby.h */

#ifndef CHUBBY_H
#define CHUBBY_H

#include <sys/select.h>
#include <sys/types.h>

#define CHUBBY_CHUNK_SIZE	4000

#define CHUBBY_E_NOENT		1
#define CHUBBY_E_ISDIR		2
#define CHUBBY_E_IFFAILED	3
#define CHUBBY_E_INVAL		4
#define CHUBBY_E_LEASEEXPIRED	CHUBBY_E_IFFAILED
#define CHUBBY_E_TIMEOUT	101
#define CHUBBY_E_OOM		102
#define CHUBBY_E_IO		103
#define CHUBBY_E_CLOSED		104

typedef struct chubby chubby_t;
typedef struct chubby_connection chubby_connection_t;

typedef struct{
    int length;
    char a[1];
}chubby_string_t;

struct chubby_stat{
    int64_t decreeNumber;
    int64_t parentDecreeNumber;
    int size;
};

chubby_t*chubby_new(const char*addresses,int timeout,void*(*malloc)(size_t),void(*free)(void*));
void chubby_close(chubby_t*thiz);
chubby_connection_t*chubby_makeConnection(chubby_t*thiz);
void chubby_asynchronous_think(chubby_t*thiz,int*fd,struct timeval*tv);

int chubby_connection_asynchronous_sleep(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,time_t time);
int chubby_connection_asynchronous_get(chubby_connection_t*thiz,void(*callback)(void*data,int e,struct chubby_stat*stat,chubby_string_t*answer),void*data,const char*name,int maxsize);
int chubby_connection_asynchronous_move(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,const char*fromname,const char*toname);
int chubby_connection_asynchronous_mkdir(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,const char*name);
int chubby_connection_asynchronous_nonAtomicMakeManyDirs(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,const char*name);
int chubby_connection_asynchronous_rmdir(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,const char*name);
int chubby_connection_asynchronous_stat(chubby_connection_t*thiz,void(*callback)(void*data,int e,struct chubby_stat*stat),void*data,const char*name);
int chubby_connection_asynchronous_put(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,const char*name,chubby_string_t*contents);
int chubby_connection_asynchronous_nonAtomicPutBigFile(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,const char*name,chubby_string_t*contents);
int chubby_connection_asynchronous_remove(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,const char*name);
int chubby_connection_asynchronous_list(chubby_connection_t*thiz,void(*callback)(void*data,int e,struct chubby_stat*stat,char**answer),void*data,const char*name);
int chubby_connection_asynchronous_checkLease(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data);
int chubby_connection_asynchronous_getLease(chubby_connection_t*thiz,void(*callback)(void*data,int e,chubby_connection_t*answer),void*data,const char*name,int timeout,int leaseTime);
int chubby_connection_asynchronous_renew(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data,int leaseTime);
void chubby_connection_asynchronous_giveUpLease(chubby_connection_t*thiz,void(*callback)(void*data,int e),void*data);

int chubby_connection_get(chubby_connection_t*thiz,struct chubby_stat*stat,chubby_string_t**answer,const char*name,int maxsize);
int chubby_connection_move(chubby_connection_t*thiz,const char*fromname,const char*toname);
int chubby_connection_mkdir(chubby_connection_t*thiz,const char*name);
int chubby_connection_nonAtomicMakeManyDirs(chubby_connection_t*thiz,const char*name);
int chubby_connection_rmdir(chubby_connection_t*thiz,const char*name);
int chubby_connection_stat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name);
int chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_string_t*contents);
int chubby_connection_nonAtomicPutBigFile(chubby_connection_t*thiz,const char*name,chubby_string_t*contents);
int chubby_connection_remove(chubby_connection_t*thiz,const char*name);
int chubby_connection_list(chubby_connection_t*thiz,struct chubby_stat*stat,char***answer,const char*name);
void chubby_connection_listFree(chubby_connection_t*thiz,char**answer);
int chubby_connection_getLeaseFilenameDecreeNumberConnection(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int64_t decreeNumber);
int chubby_connection_getLeaseFilenameAndDecreeNumber(chubby_connection_t*thiz,char**filename,int64_t*decreeNumber);
int chubby_connection_checkLease(chubby_connection_t*thiz);
int chubby_connection_getLease(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int timeout,int leaseTime);
int chubby_connection_clone(chubby_connection_t*thiz,chubby_connection_t**answer);
int chubby_connection_renew(chubby_connection_t*thiz,int leaseTime);
int chubby_connection_getLeaseExpireTime(chubby_connection_t*thiz,time_t*time);
void chubby_connection_giveUpLease(chubby_connection_t*thiz);
void chubby_connection_close(chubby_connection_t*thiz);

#endif
