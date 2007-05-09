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
#include "access/xlogdefs.h"


#define TMGIDSIZE 22


/* DTX states */
typedef enum
{
	DTX_STATE_NONE = 0,
	DTX_STATE_ACTIVE_NOT_DISTRIBUTED,
	DTX_STATE_ACTIVE_DISTRIBUTED,
	DTX_STATE_PREPARING,
	DTX_STATE_PREPARED,
	DTX_STATE_INSERTING_COMMITTED,
	DTX_STATE_INSERTED_COMMITTED,
	DTX_STATE_FORCED_COMMITTED,
	DTX_STATE_NOTIFYING_COMMIT_PREPARED,
	DTX_STATE_INSERTING_FORGET_COMMITTED,
	DTX_STATE_INSERTED_FORGET_COMMITTED,
	DTX_STATE_NOTIFYING_ABORT_NO_PREPARED,
	DTX_STATE_NOTIFYING_ABORT_SOME_PREPARED,
	DTX_STATE_NOTIFYING_ABORT_PREPARED,
	DTX_STATE_RETRY_COMMIT_PREPARED,
	DTX_STATE_RETRY_ABORT_PREPARED,
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
	DTX_PROTOCOL_COMMAND_ABORT_NO_PREPARED,
	DTX_PROTOCOL_COMMAND_PREPARE,
	DTX_PROTOCOL_COMMAND_ABORT_SOME_PREPARED,
	DTX_PROTOCOL_COMMAND_COMMIT_PREPARED,
	DTX_PROTOCOL_COMMAND_ABORT_PREPARED,
	DTX_PROTOCOL_COMMAND_READ_ONLY_COMMIT,
	DTX_PROTOCOL_COMMAND_RETRY_COMMIT_PREPARED,
	DTX_PROTOCOL_COMMAND_RETRY_ABORT_PREPARED,
	DTX_PROTOCOL_COMMAND_RECOVERY_COMMIT_PREPARED,
	DTX_PROTOCOL_COMMAND_RECOVERY_ABORT_PREPARED
} DtxProtocolCommand;

/* DTX Context above xact.c */
typedef enum
{
	DTX_CONTEXT_LOCAL_ONLY,
	DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE,
	DTX_CONTEXT_QD_RETRY_PHASE_2,
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

typedef struct TMGXACT_UTILITY_MODE_REDO
{
	bool		committed;
	TMGXACT_LOG	gxact_log;
}	TMGXACT_UTILITY_MODE_REDO;

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

	int							sessionId;
	
	bool						explicitBeginRemembered;

	DistributedTransactionId	xminDistributedSnapshot;

	bool						bumpedPhase1Count;

	bool						badPrepareGangs;

	bool						retryPhase2RecursionStop;
	
	DtxParticipationSummary		participationSummary;

	int							debugIndex;
	
}	TMGXACT;

typedef struct TMGXACTSTATUS
{
	DistributedTransactionId	gxid;
	char						gid[TMGIDSIZE];
 	DtxState					state;
	int							sessionId;
	DistributedTransactionId	xminDistributedSnapshot;
} TMGXACTSTATUS;

typedef struct TMGALLXACTSTATUS
{
	int		next;
	int		count;

	TMGXACTSTATUS		*statusArray;
} TMGALLXACTSTATUS;


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
	int							currentPhase1Count;
									/* Current count of how many DTX 
									 * transactions are between 'PREPARING'
									 * and confirmed 'COMMIT' or 'ABORT'.
									 */
	TMGXACT  			*gxact_array[0];
}	TmControlBlock;

/*
 * Maximum number of distributed transactions in progress under MPP until
 * we can move away from a static length SharedSnaphot structure.
 */
#define MaxGpDistributedInProgress 100


typedef struct DistributedSnapshotData
{
	DistributedTransactionId 	xmin;	/* XID < xmin are visible to me */
	DistributedTransactionId 	xmax;	/* XID >= xmax are invisible to me */
	uint32						xcnt;	/* # of xact ids in xip[] */
	
	DistributedTransactionId 	xip[MaxGpDistributedInProgress];
										/* array of xact IDs in progress */

	CommandId					curcid;	/* in my xact, CID < curcid are visible */

										
} DistributedSnapshotData;

typedef DistributedSnapshotData *DistributedSnapshot;

typedef struct DtxContextInfo
{
	DistributedTransactionId distributedXid;
	DistributedTransactionId xminAllDistributedSnapshots;

	DistributedSnapshotData distributedSnapshotData;

	int distributedTxnOptions;

} DtxContextInfo;
	


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
extern bool createDtxSnapshot(DistributedSnapshot distributedSnapshot);
extern void	prepareDtxTransaction(void);
extern bool isPreparedDtxTransaction(void);
extern void getDtxLogInfo(TMGXACT_LOG *gxact_log);
extern void notifyCommittedDtxTransaction(void);
extern void	rollbackDtxTransaction(void);

extern void insertingDistributedCommitted(void);
extern void insertedDistributedCommitted(void);
extern void forcedDistributedCommitted(XLogRecPtr *recptr);

extern bool validateRedoDtxCheckPointSize(char *dtxCheckpointBuffer, int dtxCheckpointBufferSize);
extern void redoDtxCheckPoint(char *dtxCheckpointBuffer, int dtxCheckpointBufferSize);
extern void redoDistributedCommitRecord(TMGXACT_LOG * gxact_log);
extern void redoDistributedForgetCommitRecord(TMGXACT_LOG * gxact_log);
extern void descDistributedCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log);
extern void descDistributedForgetCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log);

extern void setCurrentDtxTwoPhase(const char* debugCaller, const char* debugDetail);
extern bool isCurrentDtxTwoPhase(void);
extern bool isCurrentDtxActive(void);
extern void rememberDtxExplicitBegin(void);
extern void sendDtxExplicitBegin(void);
extern int dtxCurrentPhase1Count(void);

extern bool dispatchDtxCommand(const char *cmd, bool dispatchToMirrors, bool withSnapshot, bool raiseError);

extern void tmShmemInit(void);
extern int	tmShmemSize(void);
extern void initTM(void);

extern void restoreGxact(TMGXACT_LOG * gxact, DtxState state);
extern void getDtxCheckPointInfoAndLock(char **result, int *result_size);
extern void freeDtxCheckPointInfoAndUnlock(char *info, int info_size, XLogRecPtr *recptr);

extern bool isTMInRecovery(void);
extern void setRecoverTMOn(void);

extern void verify_shared_snapshot_ready(void);

void		getTmLock(void);
void		releaseTmLock(void);

int			mppTxnOptions(bool needTwoPhase);
void		unpackMppTxnOptions(int txnOptions, int *isoLevel, bool *readOnly);
bool		isMppTxOptions_NeedTwoPhase(int txnOptions);
bool		isMppTxOptions_ExplicitBegin(int txnOptions);

extern void getAllDistributedXactStatus(TMGALLXACTSTATUS **allDistributedXactStatus);
extern bool getNextDistributedXactStatus(TMGALLXACTSTATUS *allDistributedXactStatus, TMGXACTSTATUS **distributedXactStatus);

extern void qdCreateDtxContextInfo(DtxContextInfo *dtxContextInfo, DistributedSnapshot distributedSnapshot, int txnOptions);
extern int dtxContextInfoSerializeSize(DtxContextInfo *dtxContextInfo);

extern void createDtxContextInfo(DtxContextInfo *dtxContextInfo);
extern int dtxContextInfoSize(DtxContextInfo *dtxContextInfo);
extern void serializeDtxContextInfo(char *buffer, DtxContextInfo *dtxContextInfo);
extern void deserializeDtxContextInfo(const char *serializedSnapshot, int serializedSnapshotlen,
							 DtxContextInfo *dtxContextInfo);
extern void setupRegularDtxContext (void);
extern void setupQEDtxContext (DtxContextInfo *dtxContextInfo);
extern void finishDistributedTransactionContext (char* debugCaller);
extern void rememberDtxExplicitBegin();
extern void performDtxProtocolCommand(DtxProtocolCommand dtxProtocolCommand,
					int flags,
					const char *loggingStr, const char *gid, 
					DistributedTransactionId gxid);
extern void doDtxPhase2Retry(void);
extern void UtilityModeFindOrCreateDtmRedoFile(void);
extern void UtilityModeCloseDtmRedoFile(void);
extern void PleaseDebugMe(char *caller);


#endif   /* CDBTM_H */
