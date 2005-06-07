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
#include "cdb/cdbconn.h"

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


typedef struct TMGXACT {
    char gid[TMGIDSIZE];
    DtxState state;
    bool     autocommit;
    bool     persistent;
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

//extern int startDtxTransaction( SegmentDatabaseDescriptor *dbs, int count );
extern int startDtxTransaction(void);
extern int commitDtxTransaction(bool onephase);
extern int rollbackDtxTransaction(void);
extern int condStartDtxTransaction(void);
extern int condCommitDtxTransaction(bool onephase, bool force);
extern int condRollbackDtxTransaction(bool force);
extern int startTMXactScan(void);
extern TMGXACT * getNextTMXact(int index);
extern void endTMXactScan(void);
extern void tmShmemInit(void);
extern void restoreGxact(TMGXACT *gxact);
extern void initTM(void);
extern void semanticCheckingOnQE(bool commit);

extern int *shmLatestLogFileNo;
extern int *shmEarliestLogFileNo;

int execCommand(void);
int tmShmemSize(void);
bool isInGlobalTransaction(void);
void initGxact( TMGXACT *gxact );
TMGXACT *allocGxact(void);
void restoreGxact(TMGXACT *gxact);
void releaseGxact(void);
int updateDtxState(DtxState state);
void generateGID(char *gid);
void shutdownTM(void);
void recoverTM(void);
bool phase2Recovery(void);
void cleanRMResultSets( PGresult **pgresultSets );
bool dumpRMxacts( PGresult **pgresultSets );
bool checkRM( PGresult **pgresultSets );
bool recoverRMs(PGresult ***pgresultSets);
bool handleCommittedDtx(void);
void getTmLock(void);
void releaseTmLock(void);

#endif /* CDBTM_H */
