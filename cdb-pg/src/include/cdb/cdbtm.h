/*-------------------------------------------------------------------------
 * cdbtm.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBTM_H
#define CDBTM_H

#include "storage/lwlock.h"
#include "utils/hsearch.h"

#define TMGIDSIZE 22


/* TM  states */
typedef enum
{
    DTX_STATE_IN_PROGRESS,    /* in progress */
    DTX_STATE_PREPARED,       /* prepared */
    DTX_STATE_1P_COMMIT,      /* 1p commit */
    DTX_STATE_2P_COMMIT,      /* 2p commit */
    DTX_STATE_ROLLBACK,       /* rollback */
    DTX_STATE_ABORT,          /* abort */
    DTX_STATE_COMMIT_DONE,    /* commit done */
    DTX_STATE_ROLLBACK_DONE   /* rollback done */
} DtxState;;


#define GXACT_STATUS_PERSISTENT   0x01

typedef struct TMGXACT {
    char gid[TMGIDSIZE];
    DtxState state;
    char     infomask;
    uint32   fileno;
    uint32   offset;
} TMGXACT;

typedef struct TmControlBlock
{
    LWLockId    ControlLock;
    bool        recoverred;
    int         seqno;
    int         latestLogFileNo;
    int         earliestLogFileNo;
    int         num_active_xacts;
    TMGXACT     *gxact_array[0];
} TmControlBlock;

/* error code of DTX */
typedef enum
{
    DTX_OK,              /* successful */
	DTX_ERR_COMM,		/* Communication error */
	DTX_ERR_NOTA,		/* No such transaction */
	DTX_ERR_PROTO,	/* Invalid Operation */
	DTX_ERR_FAIL,		/* Operation failed */
	DTX_ERR_INTERR	/* Internal error */
} DTXErrorCode;

extern int startDtxTransaction(void);
extern int commitDtxTransaction(bool onephase);
extern int rollbackDtxTransaction(void);
extern int condStartDtxTransaction(void);
extern int condCommitDtxTransaction(bool onephase);
extern int condRollbackDtxTransaction(void);
extern int startTMXactScan(void);
extern TMGXACT * getNextTMXact(int index);
extern void endTMXactScan(void);
extern void tmShmemInit(void);
extern void restoreGxact(TMGXACT *gxact);
extern void initTM(void);
extern void semanticCheckingOnQE(bool commit);
extern void discardDtxTransaction(void);

extern int *shmLatestLogFileNo;
extern int *shmEarliestLogFileNo;

extern char dtm_dir[MAXPGPATH];
extern int tmShmemSize(void);


#endif /* CDBTM_H */
