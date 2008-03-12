/* chubby.h */

#ifndef CHUBBY_H
#define CHUBBY_H

#define CHUBBY_CHUNK_SIZE	60000

#define CHUBBY_E_NOENT		1
#define CHUBBY_E_ISDIR		2
#define CHUBBY_E_IFFAILED	3
#define CHUBBY_E_INVAL		4
#define CHUBBY_E_LOCKEXPIRED	101
#define CHUBBY_E_TIMEOUT	102
#define CHUBBY_E_OOM		103
#define CHUBBY_E_IO		104

typedef struct chubby_sb{
    char*s;
    int length;
    int size;
    int invalid;
}chubby_sb_t;

typedef struct chubby chubby_t;
typedef struct chubby_connection chubby_connection_t;
struct chubby_stat{
    int64_t decreeNumber;
    int size;
};

void chubby_sb_init(chubby_sb_t*thiz);
void chubby_sb_free(chubby_sb_t*thiz);
void chubby_sb_append_sb(chubby_sb_t*thiz,chubby_sb_t*a);
void chubby_sb_append_format(chubby_sb_t*thiz,const char*fmt,...);
void chubby_sb_append_c(chubby_sb_t*thiz,char c);
void chubby_sb_ensureSize(chubby_sb_t*thiz,int size);

extern chubby_t*chubby_new(const char*addresses);
extern void chubby_close(chubby_t*thiz);
extern chubby_connection_t*chubby_makeConnection(chubby_t*thiz);

extern int chubby_connection_get(chubby_connection_t*thiz,chubby_sb_t*result,const char*name);
extern int chubby_connection_stat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name);
extern int chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_sb_t*contents);
extern int chubby_connection_putBigFile(chubby_connection_t*thiz,const char*name,chubby_sb_t*contents);
extern int chubby_connection_remove(chubby_connection_t*thiz,const char*name);
extern int chubby_connection_list(chubby_connection_t*thiz,chubby_sb_t*result,const char*name);
extern chubby_connection_t*chubby_connection_getLockFilenameDecreeNumberConnection(chubby_connection_t*thiz,const char*name,int64_t decreeNumber);
extern chubby_connection_t*chubby_connection_getLockStringConnection(chubby_connection_t*thiz,const char*lockString);
extern const char*chubby_connection_getLockString(chubby_connection_t*thiz);
extern int chubby_connection_isLockValid(chubby_connection_t*thiz);
extern int chubby_connection_lock(chubby_connection_t*thiz,const char*name,int timeout,int lockTime);
extern int chubby_connection_renew(chubby_connection_t*thiz,int lockTime);
extern time_t chubby_connection_getLockExpireTime(chubby_connection_t*thiz);
extern void chubby_connection_giveUpLock(chubby_connection_t*thiz);
extern void chubby_connection_close(chubby_connection_t*thiz);

#endif
