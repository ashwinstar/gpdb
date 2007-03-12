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
	DTX_STATE_IN_PROGRESS,		/* in progress */
	DTX_STATE_PREPARED,			/* prepared */
	DTX_STATE_1P_COMMIT,		/* 1p commit */
	DTX_STATE_2P_COMMIT,		/* 2p commit */
	DTX_STATE_ROLLBACK,			/* rollback */
	DTX_STATE_ABORT,			/* abort */
	DTX_STATE_COMMIT_DONE,		/* commit done */
	DTX_STATE_ROLLBACK_DONE		/* rollback done */
}	DtxState;


#define GXACT_STATUS_PERSISTENT   0x01
#define GXACT_STATUS_UNRESOLVED   0x02

typedef struct TMGXACT
{
	char		gid[TMGIDSIZE];
	DtxState	state;
	char		infomask;
	uint32		fileno;
	uint32		offset;
}	TMGXACT;

typedef struct TmControlBlock
{
	LWLockId	ControlLock;
	bool		recoverred;
	int			seqno;
	int			latestLogFileNo;
	int			earliestLogFileNo;
	int			lastFlushedFileNo;
	int			lastFlushedFileOffset;
	int			num_active_xacts;
	TMGXACT    *gxact_array[0];
}	TmControlBlock;

/* error code of DTX */
typedef enum
{
	DTX_OK,						/* successful */
	DTX_ERR_COMM,				/* Communication error */
	DTX_ERR_NOTA,				/* No such transaction */
	DTX_ERR_PROTO,				/* Invalid Operation */
	DTX_ERR_FAIL,				/* Operation failed */
	DTX_ERR_INTERR				/* Internal error */
}	DTXErrorCode;





/* state variables for how much of the log file has been flushed */
extern int *shmLastFlushedFileNo;
extern int *shmLastFlushedFileOffset;
extern int *shmLatestLogFileNo;
extern int *shmEarliestLogFileNo;
extern char dtm_dir[MAXPGPATH];


extern int	startDtxTransaction(bool dispatchBegin);
extern int	commitDtxTransaction(void);
extern int	rollbackDtxTransaction(void);
extern int	condStartDtxTransaction(void);
extern void setCurrentDtxDirty(void);
extern int	condCommitDtxTransaction(bool onephase);
extern int	condRollbackDtxTransaction(void);

extern bool isCurrentDtxDirty(void);
extern bool isCurrentDtxActive(void);

extern bool dispatchDtxCommand(const char *cmd, bool dispatchToMirrors, bool withSnapshot);

extern int	startTMXactScan(void);
extern TMGXACT *getNextTMXact(int index);
extern void endTMXactScan(void);

extern void tmShmemInit(void);
extern int	tmShmemSize(void);
extern void initTM(void);
extern void	shutdownTM(void);

extern void restoreGxact(TMGXACT * gxact);
extern void semanticCheckingOnQE(bool commit);
extern void discardDtxTransaction(void);

extern bool isTMInRecovery(void);
extern void setRecoverTMOn(void);

extern void verify_shared_snapshot_ready(void);

void		getTmLock(void);
void		releaseTmLock(void);

int			mppTxnOptions(bool autoBegin);
void		unpackMppTxnOptions(int txnOptions, int *isoLevel, bool *readOnly);
bool		isMppTxOptionsAutoBegin(int txnOptions);


/* Marks the state such that a BEGIN has been executed on
 * the remote QEs. 
 */
extern void setTxnBegan(void);

/* inquires as to whether a BEGIN has been executed on
 * the remote QEs.
 */
extern bool hasTxnBegan(void);



#endif   /* CDBTM_H */
