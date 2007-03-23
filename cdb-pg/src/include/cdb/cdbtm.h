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
#include "lib/stringinfo.h"

#define TMGIDSIZE 22


/* DTX states */
typedef enum
{
	DTX_STATE_NONE = 0,
	DTX_STATE_ACTIVE_NOT_DISTRIBUTED,
	DTX_STATE_ACTIVE_DISTRIBUTED,
	DTX_STATE_COMMITTING_ALL_READ_ONLY,
	DTX_STATE_COMMITTED_ALL_READ_ONLY,
	DTX_STATE_PREPARING,
	DTX_STATE_PREPARED,
	DTX_STATE_INSERTING_COMMITTED,
	DTX_STATE_INSERTED_COMMITTED,
	DTX_STATE_FORCED_COMMITTED,
	DTX_STATE_NOTIFYING_COMMITTED,
	DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED,
	DTX_STATE_RETRYING_NOTIFY_COMMITTED,
	DTX_STATE_INSERTING_FORGET_COMMITTED,
	DTX_STATE_INSERTED_FORGET_COMMITTED,
	DTX_STATE_NOTIFYING_ABORTED,
	DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED,
	DTX_STATE_RETRYING_NOTIFY_ABORTED,
	DTX_STATE_CRASH_COMMITTED
}	DtxState;

/* DTX participation summary */
typedef enum
{
	DTX_PARTICIPATION_SUMMARY_NONE = 0,
	DTX_PARTICIPATION_SUMMARY_ALL_READ_ONLY,
	DTX_PARTICIPATION_SUMMARY_ONE_SEGMENT_DIRTY,
	DTX_PARTICIPATION_SUMMARY_MULTIPLE_SEGMENTS_DIRTY
}	DtxParticipationSummary;

/* TM QD to QE protocol commands */
typedef enum
{
	DTX_PROTOCOL_COMMAND_NONE,
	DTX_PROTOCOL_COMMAND_ABORT,
	DTX_PROTOCOL_COMMAND_PREPARE,
	DTX_PROTOCOL_COMMAND_COMMIT_PREPARED,
	DTX_PROTOCOL_COMMAND_ABORT_PREPARED,
	DTX_PROTOCOL_COMMAND_READ_ONLY_COMMIT,
	DTX_PROTOCOL_COMMAND_RETRY_COMMIT_PREPARED,
	DTX_PROTOCOL_COMMAND_RETRY_ABORT_PREPARED,
	DTX_PROTOCOL_COMMAND_RECOVERY_ABORT_PREPARED
} DtxProtocolCommand;

/* DTX Context above xact.c */
typedef enum
{
	DTX_CONTEXT_LOCAL_ONLY,
	DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE,
	DTX_CONTEXT_QE_ENTRY_DB_SINGLETON,
	DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT,
	DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER,
	DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER,
	DTX_CONTEXT_QE_READER,
	DTX_CONTEXT_QE_PREPARED
} DtxContext;
	
typedef struct TMGXACT_LOG
{
	char						gid[TMGIDSIZE];
	DistributedTransactionId	gxid;
}	TMGXACT_LOG;

typedef struct TMGXACT_CHECKPOINT
{
	int						committedCount;
	int						segmentCount;
	TMGXACT_LOG				committedGxactArray[0];
}	TMGXACT_CHECKPOINT;

typedef struct TMGXACT
{
	/*
	 * These fields will be recorded in the log.  They are the same
	 * as those in the TMGXACT_LOG struct.  We will be copying the
	 * fields individually, so they dont have to match the same order,
	 * but it a good idea.
	 */
	char						gid[TMGIDSIZE];
	DistributedTransactionId	gxid;

	/*
	 * Memory only fields.
	 */
 	DtxState				state;

	bool						explicitBeginRemembered;

	DistributedTransactionId	xminGlobalSnapshot;

	DtxParticipationSummary		participationSummary;

	int							debugIndex;
	
}	TMGXACT;


typedef struct TmControlBlock
{
	LWLockId					ControlLock;
	bool						recoverred;
	int							seqno;
	bool						DtmStarted;
	int							SegmentCount;
	int							SegmentsStatesByteLen;
	DistributedTransactionId	XminAllDistributedSnapshots;
	int							num_active_xacts;
	TMGXACT  			*gxact_array[0];
}	TmControlBlock;

typedef struct DistributedSnapshotData
{
	DistributedTransactionId 	xmin;	/* XID < xmin are visible to me */
	DistributedTransactionId 	xmax;	/* XID >= xmax are invisible to me */
	uint32						xcnt;	/* # of xact ids in xip[] */
	DistributedTransactionId 	*xip;	/* array of xact IDs in progress */
} DistributedSnapshotData;

typedef DistributedSnapshotData *DistributedSnapshot;

extern DLLIMPORT DistributedSnapshot CurrentDistributedSnapshot;


/* state variables for how much of the log file has been flushed */
extern bool *shmDtmStarted;
extern int *shmSegmentCount;
extern int *shmSegmentsStatesByteLen;
extern DistributedTransactionId *shmXminAllDistributedSnapshots;


extern char* DtxStateToString(DtxState state);
extern char* DtxParticipationSummaryToString(DtxParticipationSummary summary);
extern char* DtxProtocolCommandToString(DtxProtocolCommand command);
extern char* DtxContextToString(DtxContext context);
extern DistributedTransactionId getDistributedTransactionId(void);
extern void createDtx(TransactionId *pLocalXid);
extern void	prepareDtxTransaction(void);
extern bool isPreparedDtxTransaction(void);
extern void getDtxLogInfo(TMGXACT_LOG *gxact_log);
extern void notifyCommittedDtxTransaction(void);
extern void	rollbackDtxTransaction(void);
extern void	markDtxAsDistributed(void);
extern void setCurrentDtxDirty(void);

extern void insertingDistributedCommitted(void);
extern void insertedDistributedCommitted(void);
extern void forcedDistributedCommitted(void);

extern bool validateRedoDtxCheckPointSize(char *dtxCheckpointBuffer, int dtxCheckpointBufferSize);
extern void redoDtxCheckPoint(char *dtxCheckpointBuffer, int dtxCheckpointBufferSize);
extern void redoDistributedCommitRecord(TMGXACT_LOG * gxact_log);
extern void redoDistributedForgetCommitRecord(TMGXACT_LOG * gxact_log);
extern void descDistributedCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log);
extern void descDistributedForgetCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log);

extern bool isCurrentDtxDirty(void);
extern bool isCurrentDtxActive(void);

extern bool dispatchDtxCommand(const char *cmd, bool dispatchToMirrors, bool withSnapshot, bool raiseError);

extern void tmShmemInit(void);
extern int	tmShmemSize(void);
extern void initTM(void);
extern void	shutdownTM(void);

extern void restoreGxact(TMGXACT_LOG * gxact, DtxState state);
extern void getDtxCheckPointInfoAndLock(char **result, int *result_size);
extern void freeDtxCheckPointInfoAndUnlock(char *info, int info_size);

extern bool isTMInRecovery(void);
extern void setRecoverTMOn(void);

extern void verify_shared_snapshot_ready(void);

void		getTmLock(void);
void		releaseTmLock(void);

int			mppTxnOptions(bool statementWantsDtxTransaction);
void		unpackMppTxnOptions(int txnOptions, int *isoLevel, bool *readOnly);
bool		isMppTxOptions_StatementWantsDtxTransaction(int txnOptions);
bool		isMppTxOptions_Dirty(int txnOptions);
bool		isMppTxOptions_ExplicitBegin(int txnOptions);


extern void getDistributedSnapshotSerializeInfo(int *serializedLen,
                             DistributedTransactionId *currentDistributedTransactionId,
                             DistributedTransactionId *xminAllDistributedSnapshots,
                             int *qdSharedLocalSnapshotSlotId,
                             DistributedSnapshot *distributedSnapshot);
extern void serializeDistributedSnapshot(char **buffref,
                             DistributedTransactionId currentDistributedTransactionId,
                             DistributedTransactionId xminAllDistributedSnapshots,
                             int qdSharedLocalSnapshotSlotId,
                             DistributedSnapshot distributedSnapshot,
                             CommandId curcid);
extern void getGlobalSnapshot(const char *serializedSnapshot, int serializedSnapshotlen);
extern void determineDistributedTransactionContext (void);
extern void finishDistributedTransactionContext (void);
extern void rememberDtxExplicitBegin();
extern void performDtxProtocolCommand(DtxProtocolCommand dtxProtocolCommand,
					const char *loggingStr, const char *gid, 
					DistributedTransactionId gxid);

#endif   /* CDBTM_H */
