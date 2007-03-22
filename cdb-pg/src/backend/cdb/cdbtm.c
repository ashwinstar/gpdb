/*-------------------------------------------------------------------------
 *
 * cdtm.c
 *	  Provides routines for performing distributed transaction
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include "cdb/cdbtm.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbdispatchresult.h"		/* cdbdisp_dumpDispatchResults() */
#include "cdb/cdbvars.h"
#include "libpq-fe.h"
#include "access/transam.h"
#include "access/xact.h"
#include "cdb/cdbfts.h"
#include "lib/stringinfo.h"
#include "access/twophase.h"
#include "access/cdbdistributedxidmap.h"

static LWLockId shmControlLock;
static bool *shmTmRecoverred;
static int *shmGIDSeq;
static int *shmNumGxacts;

static int	ControlLockCount = 0;

int 		*shmSegmentCount;
int 		*shmSegmentStatesByteLen;

DistributedTransactionId *shmXminAllDistributedSnapshots;

bool 		*shmDtmStarted;

/*global transaction array */
static TMGXACT **shmGxactArray;
static TMGXACT *currentGxact;

static bool duringRecovery = false;
static int	max_tm_gxacts = 100;

static bool isDirty = false;
static bool resolveDtx = false;

typedef struct InDoubtDtx
{
	char		gid[TMGIDSIZE];
}	InDoubtDtx;


/* here are some flag options relationed to the txnOptions field of 
 * PQsendGpQuery
 */
 
/* bit 1 is for statement wants DTX transaction
 *
 * bits 2-3 for iso level  00 read-committed
 *						   01 read-uncommitted
 *						   10 repeatable-read
 *						   11 serializable
 * bit 4 is for read-only
 */
#define GP_OPT_STATEMENT_WANTS_DTX_TRANSACTION         0x0001

#define GP_OPT_READ_COMMITTED    						0x0002
#define GP_OPT_READ_UNCOMMITTED  						0x0004
#define GP_OPT_REPEATABLE_READ   						0x0006
#define GP_OPT_SERIALIZABLE 	  						0x0008

#define GP_OPT_READ_ONLY         						0x0010

#define GP_OPT_DIRTY             						0x0020

#define GP_OPT_EXPLICT_BEGIN      						0x0040

/*
 * These DistributedSnapshotData structs are static to simplify memory allocation
 * (see the hack in GetSnapshotData to avoid repeated malloc/free).
 */
static DistributedSnapshotData CurrentDistributedSnapshotData;

/* Externally visible pointers to valid snapshots: */
DistributedSnapshot	CurrentDistributedSnapshot = &CurrentDistributedSnapshotData;

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static void initGxact(TMGXACT * gxact);
static void releaseGxact(void);
static void generateGID(char *gid, DistributedTransactionId *gxid);

static void recoverTM(void);
static bool recoverInDoubtTransactions(void);
static HTAB *gatherRMInDoubtTransactions(void);
static void abortRMInDoubtTransactions(HTAB *htab);
static void removeInDoubtTransaction(HTAB *htab, char *gid);
static void cleanRMResultSets(PGresult **pgresultSets);

static void dumpAllDtx(void);
// static void resolveInDoubtDtx(void);
static void dumpRMOnlyDtx(HTAB *htab, StringInfoData *buff);

static void clearDistributedSnapshot(DistributedSnapshot distributedSnapshot);
static void summarizeParticipation(void);
static bool doDispatchDtxProtocolCommand(
				DtxProtocolCommand dtxProtocolCommand, 
				char* gid, DistributedTransactionId gxid,
				bool dispatchToMirrors, bool raiseError);
static void doCommitAllReadOnly(void);
static void doPrepareTransaction(void);
static void doInsertForgetCommitted(void);
static void doNotifyingCommitted(void);
static bool doRetryingNotifyCommitted(void);
static void doNotifyingAborted(bool noPreparesIssued);
static void doRetryingNotifyAborted(void);
static void doAbortInDoubt(char *gid);
static void condPrepareDtxTransaction(void);
static void	condRollbackDtxTransaction(void);
static void doQEAddLocalXidsForDistributedIndoubt();
static void doQEDistributedExplicitBegin();

/*=========================================================================
 * VISIBLE FUNCTIONS
 */

char* DtxStateToString(DtxState state)
{
	switch (state)
	{
	case DTX_STATE_NONE:
		return "None";
		
	case DTX_STATE_ACTIVE_NOT_DISTRIBUTED:
		return "Active Not Distributed";
		
	case DTX_STATE_ACTIVE_DISTRIBUTED:
		return "Active Distributed";
		
	case DTX_STATE_COMMITTING_ALL_READ_ONLY:
		return "Committing All Read Only";
		
	case DTX_STATE_COMMITTED_ALL_READ_ONLY:
		return "Committed All Read Only";
		
	case DTX_STATE_PREPARING:
		return "Preparing";
		
	case DTX_STATE_PREPARED:
		return "Prepared";
		
	case DTX_STATE_INSERTING_COMMITTED:
		return "Inserting Committed";
		
	case DTX_STATE_INSERTED_COMMITTED:
		return "Inserted Committed";
		
	case DTX_STATE_FORCED_COMMITTED:
		return "Forced Committed";
		
	case DTX_STATE_NOTIFYING_COMMITTED:
		return "Notifying Committed";
		
	case DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED:
		return "Notify Committed Retry Needed";
		
	case DTX_STATE_RETRYING_NOTIFY_COMMITTED:
		return "Retrying Notify Committed";
		
	case DTX_STATE_INSERTING_FORGET_COMMITTED:
		return "Inserting Forget Committed";
		
	case DTX_STATE_INSERTED_FORGET_COMMITTED:
		return "Inserted Forget Committed";
		
	case DTX_STATE_NOTIFYING_ABORTED:
		return "Notifying Aborted";
		
	case DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED:
		return "Notify Aborted Retry Needed";
		
	case DTX_STATE_RETRYING_NOTIFY_ABORTED:
		return "Retrying Notify Aborted";
		
	case DTX_STATE_CRASH_COMMITTED:
		return "Crash Committed";
		
	default:
		return "Unknown";
	}
}


char* DtxParticipationSummaryToString(DtxParticipationSummary summary)
{
	switch (summary)
	{
	case DTX_PARTICIPATION_SUMMARY_NONE:
		return "None";
		
	case DTX_PARTICIPATION_SUMMARY_ALL_READ_ONLY:
		return "All Read Only";
		
	case DTX_PARTICIPATION_SUMMARY_ONE_SEGMENT_DIRTY:
		return "One Segment Dirty";
		
	case DTX_PARTICIPATION_SUMMARY_MULTIPLE_SEGMENTS_DIRTY:
		return "Multiple Segments Dirty";
		
	default:
		return "Unknown";
	}
}

char* DtxProtocolCommandToString(DtxProtocolCommand command)
{
	switch (command)
	{
	case DTX_PROTOCOL_COMMAND_NONE:
		return "None";
		
	case DTX_PROTOCOL_COMMAND_ABORT:
		return "Distributed Abort";
		
	case DTX_PROTOCOL_COMMAND_PREPARE:
		return "Distributed Prepare";
		
	case DTX_PROTOCOL_COMMAND_COMMIT_PREPARED:
		return "Distributed Commit Prepared";
		
	case DTX_PROTOCOL_COMMAND_ABORT_PREPARED:
		return "Distributed Abort Prepared";
		
	case DTX_PROTOCOL_COMMAND_READ_ONLY_COMMIT:
		return "Distributed Read Only Commit";
		
	case DTX_PROTOCOL_COMMAND_RETRY_COMMIT_PREPARED:
		return "Retry Distributed Commit Prepared";
		
	case DTX_PROTOCOL_COMMAND_RETRY_ABORT_PREPARED:
		return "Retry Distributed Abort Prepared";

	case DTX_PROTOCOL_COMMAND_RECOVERY_ABORT_PREPARED:
		return "Recovery Abort Prepared";
		
	default:
		return "Unknown";
	}
}

char* DtxContextToString(DtxContext context)
{
	switch (context)
	{
	case DTX_CONTEXT_LOCAL_ONLY:
		return "Local Only";
		
	case DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE:
		return "QD Distributed-Capable";

	case DTX_CONTEXT_QE_ENTRY_DB_SINGLETON:
		return "QE Entry DB Singleton";
		
	case DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT:
		return "QE Auto-Commit Implicit";
		
	case DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER:
		return "QE Two-Phase Explicit Writer";
		
	case DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER:
		return "QE Two-Phase Implicit Writer";
		
	case DTX_CONTEXT_QE_READER:
		return "QE Reader";
		
	case DTX_CONTEXT_QE_PREPARED:
		return "QE Prepared";
		
	default:
		return "Unknown";
	}
}

DistributedTransactionId
getDistributedTransactionId(void)
{
	if (currentGxact == NULL)
	{
		return InvalidDistributedTransactionId;
	}
	else
	{
		return currentGxact->gxid;
	}
}


/*
 * Called by condPrepareDtxTransaction.
 */
static void
summarizeParticipation(void)
{
	if (DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE)
	{
 		Assert(currentGxact == NULL);
		elog(LOG,"summarizeParticipation DistributedTransactionContext is not 'QD Distributed'");
		return;
	}

	if (currentGxact != NULL)
	{
		if (currentGxact->state == DTX_STATE_ACTIVE_NOT_DISTRIBUTED)
		{
			elog(LOG,"summarizeParticipation active not distributed");
			return;
		}
		if (currentGxact->participationSummary != DTX_PARTICIPATION_SUMMARY_NONE)
		{
			elog(LOG,"summarizeParticipation already called???");
			return;
		}

		// UNDONE:
		if (isDirty)
		{
			currentGxact->participationSummary = DTX_PARTICIPATION_SUMMARY_MULTIPLE_SEGMENTS_DIRTY;
		}
		else
		{
			currentGxact->participationSummary = DTX_PARTICIPATION_SUMMARY_ALL_READ_ONLY;
		}


		elog(LOG,"summarizeParticipation participationSummary = \"%s\"",
			 DtxParticipationSummaryToString(currentGxact->participationSummary));
		
	}
	else
	{
		elog(LOG,"summarizeParticipation currentGxact is NULL???");
	}
}

/*
 * prepare a global transaction, called by user commit
 * or by CommitTransaction
 */
void
prepareDtxTransaction(void)
{
	if(DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE)
	{
	    elog(DEBUG5,"prepareDtxTransaction nothing to do (DistributedTransactionContext = '%s')", 
			 DtxContextToString(DistributedTransactionContext));
		return;
	}
	
	condPrepareDtxTransaction();
}

bool
isPreparedDtxTransaction(void)
{
	if (Gp_role != GP_ROLE_DISPATCH || DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE || currentGxact == NULL)
		return false;

	return (currentGxact->state == DTX_STATE_PREPARED);
}

void
getDtxLogInfo(TMGXACT_LOG *gxact_log)
{
	Assert(currentGxact != NULL);

	memcpy(gxact_log->gid, currentGxact->gid, TMGIDSIZE);
	gxact_log->gxid = currentGxact->gxid;
}


/*
 * Notify commited a global transaction, called by user commit
 * or by CommitTransaction
 */
void
notifyCommittedDtxTransaction(void)
{
	if(DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE)
	{
	    elog(DEBUG5,"notifyCommittedDtxTransaction nothing to do (DistributedTransactionContext = '%s')", 
			 DtxContextToString(DistributedTransactionContext));
		return;
	}

	// UNDONE: return condNotifyCommittedDtxTransaction();
	if(currentGxact == NULL)
	{
	    elog(DEBUG5,"rollbackDtxTransaction nothing to do (currentGxact == NULL)");
		return;
	}

	elog(DEBUG5,"notifyCommittedDtxTransaction calling doNotifyingCommitted... for gid = %s", currentGxact->gid);

	doNotifyingCommitted();
	return;
}

/*
 * rollback a global transaction, called by user rollback
 * or by AbortTransaction during Postgres automatic rollback
 */
void
rollbackDtxTransaction(void)
{
	// UNDONE: Why is this being done????
	isDirty = false;
	
	if(DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE)
	{
	    elog(DEBUG5,"rollbackDtxTransaction nothing to do (DistributedTransactionContext = '%s')", 
			 DtxContextToString(DistributedTransactionContext));
		return;
	}
	if(currentGxact == NULL)
	{
	    elog(DEBUG5,"rollbackDtxTransaction nothing to do (currentGxact == NULL)");
		return;
	}

	elog(DEBUG5,"rollbackDtxTransaction calling condRollbackDtxTransaction... for gid = '%s'", currentGxact->gid);

    condRollbackDtxTransaction();
}

/*
 * markDtxAsDistributed:
 *    Take our always started and available DTM transaction and
 * make it ready to go distributed to the segments.
 */
void
markDtxAsDistributed(void)
{
	Assert (currentGxact != NULL);

	if (currentGxact->state == DTX_STATE_ACTIVE_NOT_DISTRIBUTED)
	{
		currentGxact->state = DTX_STATE_ACTIVE_DISTRIBUTED;
		elog(LOG,"markDtxAsDistributed going distributed for gid = %s", currentGxact->gid);
		
		/* UNDONE: Used to check if we have in-doubt transactions to resolve */
	//	if (resolveDtx)
	//		resolveInDoubtDtx();
	}
 	else if (currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED)
	{
		elog(DEBUG5,"markDtxAsDistributed gid = %s is already distributed", currentGxact->gid);
	}
	else
	{
		elog(ERROR,"DTM transaction is not active");
	}
}


/* marks the current Distrubuted Transaction as "dirty", meaning that actual
 * work has been performed and we'll need to use 2pc on commit and log to our
 * tmlog.
 */
void
setCurrentDtxDirty(void)
{
	isDirty = true;
}

/*
 * The executor can avoid starting a distributed transaction if it knows that
 * the current dtx is clean and we aren't in a user-started global transaction.
 */
bool
isCurrentDtxDirty(void)
{
	return isDirty || (currentGxact != NULL && currentGxact->state != DTX_STATE_ACTIVE_NOT_DISTRIBUTED);
}

/*
 * When cleaning up FTS needs to know if there is a transaction active.
 */
bool
isCurrentDtxActive(void)
{
	return (currentGxact != NULL);
}

static void
doCommitAllReadOnly(void)
{
	bool succeeded;
	
	elog(LOG,"doCommitAllReadOnly entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED);
	currentGxact->state = DTX_STATE_COMMITTING_ALL_READ_ONLY;
	releaseTmLock();

	elog(LOG,"doCommitAllReadOnly moved to state = %s", DtxStateToString(currentGxact->state));

	/*
	 * Do Read-Only COMMIT broadcast.
	 */
	// UNDONE: What about dispatchToMirrors parameter?
	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_READ_ONLY_COMMIT,
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ false, /* raiseError */ false);
	if (!succeeded)
	{
		elog(ERROR, "Commit of read-only segments failed");
	}
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_COMMITTING_ALL_READ_ONLY);
	currentGxact->state = DTX_STATE_COMMITTED_ALL_READ_ONLY;
	releaseGxact();
	releaseTmLock();

	elog(LOG,"doCommitAllReadOnly called releaseGxact");

}

static void
doPrepareTransaction(void)
{
	bool succeeded;
	
	elog(LOG,"doPrepareTransaction entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED);
	currentGxact->state = DTX_STATE_PREPARING;
	releaseTmLock();

	elog(DEBUG5,"doPrepareTransaction moved to state = %s", DtxStateToString(currentGxact->state));

	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_PREPARE, 
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(ERROR, "Preparation of the distributed transaction gid = %s on one or more segments failed", currentGxact->gid);
	}
	elog(LOG, "Preparation of the distributed transaction gid = %s succeeded", currentGxact->gid);
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_PREPARING);
	currentGxact->state = DTX_STATE_PREPARED;
	releaseTmLock();
	
	elog(DEBUG5,"doPrepareTransaction leaving in state = %s", DtxStateToString(currentGxact->state));
}

/*
 * Insert FORGET COMMITTED into the xlog.
 * Call with DTM lock held.
 */
static void
doInsertForgetCommitted(void)
{
	TMGXACT_LOG gxact_log;
	
	elog(DEBUG5,"doInsertForgetCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	currentGxact->state = DTX_STATE_INSERTING_FORGET_COMMITTED;

	memcpy(&gxact_log.gid, currentGxact->gid, TMGIDSIZE);
	gxact_log.gxid = currentGxact->gxid;
	
	RecordDistributedForgetCommitted(&gxact_log);
	
	currentGxact->state = DTX_STATE_INSERTED_FORGET_COMMITTED;

	releaseGxact();
	
	elog(LOG,"doInsertForgetCommitted called releaseGxact");
}

static void
doNotifyingCommitted(void)
{
	bool succeeded;
	
	elog(DEBUG5,"doNotifyingCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_FORCED_COMMITTED);
	currentGxact->state = DTX_STATE_NOTIFYING_COMMITTED;
	releaseTmLock();

	elog(DEBUG5,"doNotifyingCommitted moved to state = %s", DtxStateToString(currentGxact->state));
	
	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_COMMIT_PREPARED,  
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(WARNING, "Commit notification for gid = %s failed to one or more segments.  System will retry again later", currentGxact->gid);
	}
	else
	{
		elog(LOG, "Commit notification for gid = %s succeeded", currentGxact->gid);
	}

	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFYING_COMMITTED);

	if (succeeded)
	{
		doInsertForgetCommitted();
	}
	else
	{
		currentGxact->state = DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED;
		currentGxact = NULL;	// Let go so another process can retry.
	}
	
	releaseTmLock();
}

static bool
doRetryingNotifyCommitted(void)
{
	bool succeeded;
	
	elog(DEBUG5,"doRetryingNotifyCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED);
	currentGxact->state = DTX_STATE_RETRYING_NOTIFY_COMMITTED;
	releaseTmLock();

	elog(DEBUG5,"doRetryingNotifyCommitted moved to state = %s", DtxStateToString(currentGxact->state));
	
	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_RETRY_COMMIT_PREPARED,
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(WARNING, "System retry delivery of commit notification for gid =%s failed to one or more segments.  System will retry again later", currentGxact->gid);
	}
	else
	{
		elog(LOG, "System retry delivery of commit notification for gid =%s succeeded", currentGxact->gid);
	}
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_RETRYING_NOTIFY_COMMITTED);
	
	if (succeeded)
	{
		doInsertForgetCommitted();
	}
	else
	{
		currentGxact->state = DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED;
		currentGxact = NULL;	// Let go so another process can retry.
	}
	
	releaseTmLock();

	return succeeded;
}

static void
doNotifyingAborted(bool noPreparesIssued)
{
	bool succeeded;
	
	elog(DEBUG5,"doNotifyingAborted entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFYING_ABORTED);
	releaseTmLock();

	if (noPreparesIssued)
	{
		succeeded = doDispatchDtxProtocolCommand(
						DTX_PROTOCOL_COMMAND_ABORT,  
						currentGxact->gid, currentGxact->gxid,
						/* dispatchToMirrors */ true, /* raiseError */ false);
		if (!succeeded)
		{
			elog(ERROR, "Abort of the distributed transaction on one or more segments failed");
		}
		elog(LOG, "Abort of the distributed transaction succeeded");
	}
	else
	{
		succeeded = doDispatchDtxProtocolCommand(
						DTX_PROTOCOL_COMMAND_ABORT_PREPARED,
						currentGxact->gid, currentGxact->gxid,
						/* dispatchToMirrors */ true, /* raiseError */ false);
		if (!succeeded)
		{
			elog(ERROR, "Abort prepared of the distributed transaction on one or more segments failed");
		}
		elog(LOG, "Abort prepared of the distributed transaction succeeded");
	}
		
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFYING_ABORTED);

	if (succeeded)
	{
		releaseGxact();
	}
	else
	{
		currentGxact->state = DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED;
		currentGxact = NULL;	// Let go so another process can retry.
	}
	
	releaseTmLock();
	
	if (succeeded)
	{
		elog(LOG,"doNotifyingAborted called releaseGxact");
	}
}


static void
doRetryingNotifyAborted(void)
{
	bool succeeded;
	
	elog(DEBUG5,"doRetryingNotifyAborted entering in state = %s", 
		 DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED);
	currentGxact->state = DTX_STATE_RETRYING_NOTIFY_ABORTED;
	releaseTmLock();

	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_RETRY_ABORT_PREPARED,
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(WARNING, "System retry delivery of abort notification for gid =%s failed to one or more segments.  System will retry again later", 
			 currentGxact->gid);
	}
	else
	{
		elog(LOG, "System retry delivery of abort notification for gid =%s succeeded", 
			 currentGxact->gid);
	}
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_RETRYING_NOTIFY_ABORTED);

	if (succeeded)
	{
		releaseGxact();
	}
	else
	{
		currentGxact->state = DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED;
		currentGxact = NULL;	// Let go so another process can retry.
	}
	
	releaseTmLock();
	
	if (succeeded)
	{
		elog(LOG,"doRetryingNotifyAborted called releaseGxact");
	}
}


static void
doAbortInDoubt(char *gid)
{
	bool succeeded;

	// UNDONE: Pass real gxid instead of InvalidDistributedTransactionId.
	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_RECOVERY_ABORT_PREPARED,
					gid, InvalidDistributedTransactionId,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(WARNING, "System retry delivery of abort notification for gid =%s failed to one or more segments.  System will retry again later", 
			 gid);
	}
	else
	{
		elog(LOG, "System retry delivery of abort notification for gid =%s succeeded", 
			 gid);
	}
}

/*
 * prepare a global transaction, called by internal
 * this method starts a two-phase commit.
 */
static void 
condPrepareDtxTransaction(void)
{
	if (currentGxact == NULL)
	{
		return;
	}

	
	if (currentGxact->state == DTX_STATE_ACTIVE_NOT_DISTRIBUTED)
	{
		/*
		 * This transaction did not go distributed.
		 */
		elog(DEBUG5,"condPrepareDtxTransaction ignoring not distributed gid = %s", currentGxact->gid);
		releaseGxact();
		return;
	}
	
	elog(LOG,"condPrepareDtxTransaction called with state = %s",
		 DtxStateToString(currentGxact->state));
	
	Assert(currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED);
	
	summarizeParticipation();

	switch (currentGxact->participationSummary)
	{
	case DTX_PARTICIPATION_SUMMARY_ALL_READ_ONLY: 
		/*
		 * Broadcast COMMIT to read-only segments.
		 */
		doCommitAllReadOnly();
		break;
		
	case DTX_PARTICIPATION_SUMMARY_ONE_SEGMENT_DIRTY: 
	case DTX_PARTICIPATION_SUMMARY_MULTIPLE_SEGMENTS_DIRTY: 
		/*
		 * Broadcast PREPARE TRANSACTION to segments.
		 */
		doPrepareTransaction();
		break;
		
	default: 
		elog(FATAL, "Unrecognized dtx participation summary: %d",
			(int) currentGxact->participationSummary);
		break;
	}

	return;
}

/*
 * rolling back a global transaction, called by internal
 */
void
condRollbackDtxTransaction(void)
{
	bool noPreparesIssued = false;
	
	if (currentGxact == NULL)
	{
		return;
	}

	elog(DEBUG5,"condRollbackDtxTransaction called with state = %s",
		 DtxStateToString(currentGxact->state));
	
	switch (currentGxact->state)
	{
	case DTX_STATE_ACTIVE_NOT_DISTRIBUTED:
	case DTX_STATE_COMMITTED_ALL_READ_ONLY:
		/*
		 * Let go of these...
		 */
		releaseGxact();
		return;
		
	case DTX_STATE_ACTIVE_DISTRIBUTED:
	case DTX_STATE_COMMITTING_ALL_READ_ONLY:
		noPreparesIssued = true;
	case DTX_STATE_PREPARING:
	case DTX_STATE_PREPARED:
		currentGxact->state = DTX_STATE_NOTIFYING_ABORTED;
		break;
		
	case DTX_STATE_NOTIFYING_ABORTED:
	case DTX_STATE_RETRYING_NOTIFY_ABORTED:

		currentGxact->state = DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED;
	case DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED:
		elog(LOG,"condRollbackDtxTransaction breaking recursion and returning");
		return;
		
	case DTX_STATE_FORCED_COMMITTED:
	case DTX_STATE_NOTIFYING_COMMITTED:
	case DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED:
	case DTX_STATE_RETRYING_NOTIFY_COMMITTED:
	case DTX_STATE_INSERTING_COMMITTED:
	case DTX_STATE_INSERTED_COMMITTED:
	case DTX_STATE_INSERTING_FORGET_COMMITTED:
	case DTX_STATE_INSERTED_FORGET_COMMITTED:
	case DTX_STATE_CRASH_COMMITTED:
		elog(LOG, "Dtx state \"%s\" not expected here",
			 DtxStateToString(currentGxact->state));
		releaseGxact();
		return;
		
	default:
		elog(FATAL, "Unrecognized dtx state: %d",
			(int) currentGxact->state);
		break;
	}


	Assert(currentGxact->state == DTX_STATE_NOTIFYING_ABORTED ||
		   currentGxact->state == DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED);
	/*
	 * if the process is in the middle of blowing up... then we don't do
	 * anything here.  we can resolve any in-doubt transactions later.
	 * 
	 * We can't dispatch -- but we *do* need to free up shared-memory
	 * entries.
	 */
	if (proc_exit_inprogress)
	{
		/* we're about to drop connections to any gangs -- so we'll free up
		 * our structure too, while we're at it.
		 *
		 * Other states will have to be managed by recovery-mode ?*/
		elog(LOG,"condRollbackDtxTransaction has proc_exit_inprogress = true with state = %s",
			 DtxStateToString(currentGxact->state));

		releaseGxact();
		return;
	}

	ereport(DEBUG5,
			(errmsg("condRollbackDtxTransaction:  %s", currentGxact->gid)));
	
	if (currentGxact->state == DTX_STATE_NOTIFYING_ABORTED)
	{
		doNotifyingAborted(noPreparesIssued);
	}
	else
	{
		doRetryingNotifyAborted();
	}

	return;
}

/*
 * initialize TM, called by CdbInit for each process.
 * first call to this method will trigger tm recovery.
 */
void
initTM(void)
{
	Assert(shmTmRecoverred!=NULL);
	if (!*shmTmRecoverred)
	{
		getTmLock();
		if (!*shmTmRecoverred)
		{
			recoverTM();
			*shmTmRecoverred = true;
		}
		max_tm_gxacts = max_prepared_xacts;
		releaseTmLock();
	}
}

/* get tm share memory size */
int
tmShmemSize(void)
{
	if (Gp_role != GP_ROLE_DISPATCH)
		return 0;
	return MAXALIGN(sizeof(TmControlBlock) +
					max_tm_gxacts * (sizeof(TMGXACT *) + sizeof(TMGXACT)));
}


/*
 * tmShmemInit - should be called only once from postmaster and inherit by all
 * postgres processes
 */
void
tmShmemInit(void)
{
	bool		found;
	TmControlBlock *shared;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	shared = (TmControlBlock *) ShmemInitStruct("Transaction manager", tmShmemSize(), &found);
	if (!shared)
		elog(FATAL, "could not initialize transaction manager share memory");

	shmControlLock = shared->ControlLock;
	shmTmRecoverred = &shared->recoverred;
	shmGIDSeq = &shared->seqno;
	*shmGIDSeq = FirstDistributedTransactionId;
	shmDtmStarted = &shared->DtmStarted;
	shmSegmentCount = &shared->SegmentCount;
	shmSegmentStatesByteLen = &shared->SegmentsStatesByteLen;
	shmXminAllDistributedSnapshots = &shared->XminAllDistributedSnapshots;
	shmNumGxacts = &shared->num_active_xacts;
	shmGxactArray = shared->gxact_array;

	if (!IsUnderPostmaster)
		/* Initialize locks and shared memory area */
	{
		int				  i = 0;
		TMGXACT    *gxact = NULL;

		shared->ControlLock = LWLockAssign();
		shmControlLock = shared->ControlLock;

		/* initialize gxact array */
		gxact = (TMGXACT *) (shmGxactArray + max_tm_gxacts);
		for (i = 0; i < max_tm_gxacts; i++)
		{
			gxact->debugIndex = i;
			shmGxactArray[i] = gxact++;
		}
	}
}

/* shutdown tm, called when process exits */
void
shutdownTM(void)
{
	elog(LOG, "shutdownTM");
}

/*
 * restore global transaction during tm log recovery
 */
void
restoreGxact(TMGXACT_LOG * gxact_log, DtxState state)
{
	Assert (gxact_log != NULL);

	initGxact(currentGxact);
	
	/*
	 * Copy the log fields.
	 */
	memcpy(currentGxact->gid, gxact_log->gid, TMGIDSIZE);
	currentGxact->gxid = gxact_log->gxid;

	currentGxact->state = state;
}

/* mppTxnOptions:
 * Generates an int containing the appropriate flags to direct the remote
 * segdb QE process to perform any needed transaction commands before or
 * after the statement.
 */
int
mppTxnOptions(bool statementWantsDtxTransaction)
{
	int options = 0;

	if (statementWantsDtxTransaction)
		options |= GP_OPT_STATEMENT_WANTS_DTX_TRANSACTION;

	if (XactIsoLevel == XACT_READ_COMMITTED)
		options |= GP_OPT_READ_COMMITTED;
	else if (XactIsoLevel == GP_OPT_REPEATABLE_READ)
		options |= GP_OPT_REPEATABLE_READ;
	else if (XactIsoLevel == GP_OPT_SERIALIZABLE)
		options |= GP_OPT_SERIALIZABLE;

	if (XactReadOnly)
		options |= GP_OPT_READ_ONLY;
		
	if (isDirty)
		options |= GP_OPT_DIRTY;

	if (currentGxact != NULL && currentGxact->explicitBeginRemembered)
		options |= GP_OPT_EXPLICT_BEGIN;

	return options;
	
}

/* unpackMppTxnOptions:
 * Unpack an int containing the appropriate flags to direct the remote
 * segdb QE process to perform any needed transaction commands before or
 * after the statement.
 */
void
unpackMppTxnOptions(int txnOptions, int *isoLevel, bool *readOnly)
{

	if (txnOptions & GP_OPT_READ_COMMITTED)
		*isoLevel = XACT_READ_COMMITTED;
	else if (txnOptions & GP_OPT_SERIALIZABLE)
		*isoLevel = XACT_SERIALIZABLE;
	else if( txnOptions & GP_OPT_REPEATABLE_READ)
		*isoLevel = XACT_REPEATABLE_READ;
	else
		*isoLevel = XACT_READ_UNCOMMITTED;

	if (txnOptions & GP_OPT_READ_ONLY)
		*readOnly = true;
	else
		*readOnly = false;

}

/* isMppTxOptions_StatementWantsDtxTransaction:
 * Return the StatementWantsDtxTransaction flag.
 */
bool
isMppTxOptions_StatementWantsDtxTransaction(int txnOptions)
{
	return ((txnOptions & GP_OPT_STATEMENT_WANTS_DTX_TRANSACTION) != 0);
}

/* isMppTxOptions_Dirty:
 * Return the Dirty flag.
 */
bool
isMppTxOptions_Dirty(int txnOptions)
{
	return ((txnOptions & GP_OPT_DIRTY) != 0);
}

/* isMppTxOptions_ExplicitBegin:
 * Return the ExplicitBegin flag.
 */
bool
isMppTxOptions_ExplicitBegin(int txnOptions)
{
	return ((txnOptions & GP_OPT_EXPLICT_BEGIN) != 0);
}


/* acquire tm lw lock */
void
getTmLock(void)
{

	if (ControlLockCount++ == 0)
		LWLockAcquire(shmControlLock, LW_EXCLUSIVE);
}

/* release tm lw lock */
void
releaseTmLock(void)
{
	if (--ControlLockCount == 0)
		LWLockRelease(shmControlLock);

}

bool
isTMInRecovery(void)
{
	return duringRecovery;
}


void
setRecoverTMOn(void)
{
	resolveDtx = true;
}

/*
 * Validate redo transaction commit log record size.
 */
bool
validateRedoDtxCheckPointSize(char *dtxCheckpointBuffer, int dtxCheckpointBufferSize)
{
	TMGXACT_CHECKPOINT * gxact_checkpoint;
	int committedCount;

	elog(DEBUG5, "redoDtxCheckPoint has size %d", dtxCheckpointBufferSize);
	gxact_checkpoint = (TMGXACT_CHECKPOINT *)dtxCheckpointBuffer;
	
	committedCount = gxact_checkpoint->committedCount;
	elog(DEBUG5, "redoDtxCheckPoint has committedCount = %d",
		 committedCount);

	return (dtxCheckpointBufferSize == 
		    offsetof(TMGXACT_CHECKPOINT,committedGxactArray) + committedCount * sizeof(TMGXACT_LOG));
}


/*
 * Redo transaction commit log record.
 */
void
redoDtxCheckPoint(char *dtxCheckpointBuffer, int dtxCheckpointBufferSize)
{
	TMGXACT_CHECKPOINT * gxact_checkpoint;
	int committedCount;
	int segmentCount;
	
	int i;

	gxact_checkpoint = (TMGXACT_CHECKPOINT *)dtxCheckpointBuffer;
	
	committedCount = gxact_checkpoint->committedCount;
	segmentCount = gxact_checkpoint->segmentCount;
	elog(DEBUG5, "redoDtxCheckPoint has size %d, committedCount = %d, segmentCount = %d",
		 dtxCheckpointBufferSize, committedCount, segmentCount);
	
	for (i = 0; i < committedCount; i++)
	{
		redoDistributedCommitRecord(&gxact_checkpoint->committedGxactArray[i]);
	}
}

/*
 * Redo transaction commit log record.
 */
void
redoDistributedCommitRecord(TMGXACT_LOG * gxact_log)
{
	int			i;

	for (i = 0; i < *shmNumGxacts; i++)
	{
		if (strcmp(gxact_log->gid, shmGxactArray[i]->gid) == 0)
		{
			/* found an active global transaction */
			currentGxact = shmGxactArray[i];
			break;
		}
	}
	if (i == *shmNumGxacts)
	{
		/*
		 * Transaction not found, this is the first log of this transaction.
		 */
		if (*shmNumGxacts >= max_tm_gxacts)
		{
			ereport(ERROR,
					(errmsg("cannot allocate global transaction")));
		}

		currentGxact = shmGxactArray[(*shmNumGxacts)++];
	}
	
	restoreGxact(gxact_log, DTX_STATE_CRASH_COMMITTED);

	elog(DEBUG5, "redoDistributedCommitRecord added crash committed gid = %s",currentGxact->gid);
}

/*
 * Redo transaction forget commit log record.
 */
void
redoDistributedForgetCommitRecord(TMGXACT_LOG * gxact_log)
{
	int			i;

	for (i = 0; i < *shmNumGxacts; i++)
	{
		if (strcmp(gxact_log->gid, shmGxactArray[i]->gid) == 0)
		{
			/* found an active global transaction */
			currentGxact = shmGxactArray[i];
			elog(DEBUG5, "redoDistributedForgetCommitRecord removed gid = %s for forget",currentGxact->gid);
			releaseGxact();
			return;
		}
	}

	elog(ERROR, "redoDistributedForgetCommitRecord did not find gid = %s for forget",gxact_log->gid);
}

static void
descGxactLog(StringInfo buf, TMGXACT_LOG * gxact_log)
{
	appendStringInfo(buf, " gid = %s, gxid = %u",
					 gxact_log->gid, gxact_log->gxid);
}

/*
 * Describe redo transaction commit log record.
 */
void
descDistributedCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log)
{
	descGxactLog(buf, gxact_log);
}

/*
 * Describe redo transaction forget commit log record.
 */
void
descDistributedForgetCommitRecord(StringInfo buf, TMGXACT_LOG * gxact_log)
{
	descGxactLog(buf, gxact_log);
}


/*=========================================================================
 * HELPER FUNCTIONS
 */

static bool
doDispatchDtxProtocolCommand(
			DtxProtocolCommand dtxProtocolCommand, 
			char* gid, DistributedTransactionId gxid,
			bool dispatchToMirrors, bool raiseError)
{
	int		i, resultCount, numOfFailed = 0;

	char *dtxProtocolCommandStr;

	struct pg_result **results = NULL;
	StringInfoData errbuf;

	dtxProtocolCommandStr = DtxProtocolCommandToString(dtxProtocolCommand);

	elog(DEBUG5, "dispatchDtxProtocolCommand: %d ('%s') willMirror: %s", dtxProtocolCommand, dtxProtocolCommandStr, dispatchToMirrors ? "yes" : "no");

	initStringInfo(&errbuf);
	results = cdbdisp_dispatchDtxProtocolCommand(dtxProtocolCommand,
										dtxProtocolCommandStr,
										gid,
										gxid,
										dispatchToMirrors,
										&errbuf, &resultCount);

	if (errbuf.len > 0)
	{
		ereport((raiseError ? ERROR : WARNING), (errmsg("DTM error (gathered %d results from cmd '%s')", resultCount, dtxProtocolCommandStr),
						errdetail("%s", errbuf.data)));
		return false;
	}

	Assert(results != NULL);
	if (results == NULL)
	{
		numOfFailed++; /* If we got no results, we need to treat it as an error! */		
	}
	for (i = 0; i < resultCount; i++)
	{
		char			*cmdStatus;
		ExecStatusType	resultStatus;

		/* note: PQresultStatus() is smart enough to deal with results[i] == NULL */
		resultStatus = PQresultStatus(results[i]);
		if (resultStatus != PGRES_COMMAND_OK &&
			resultStatus != PGRES_TUPLES_OK)
		{
			numOfFailed++;
		}
		else
		{
			/*
			 * success ? If an error happened during a transaction which
			 * hasn't already been caught when we try a prepare we'll get a
			 * rollback from our prepare ON ONE SEGMENT: so we go look at the
			 * status, otherwise we could issue a COMMIT when we don't want
			 * to!
			 */
			cmdStatus = PQcmdStatus(results[i]);

			elog(DEBUG3, "DTM: status message cmd '%s' [%d] result '%s'", dtxProtocolCommandStr, i, cmdStatus);
			if (strncmp(cmdStatus, dtxProtocolCommandStr, strlen(cmdStatus)) != 0)
			{
				/* failed */
				numOfFailed++;
			}
		}
	}

	/* discard the errbuf text */
	pfree(errbuf.data);

	/* Now we clean up the results array. */
	cleanRMResultSets(results);

	return (numOfFailed == 0);
}


bool
dispatchDtxCommand(const char *cmd, bool dispatchToMirrors, bool withSnapshot, bool raiseError)
{
	int		i, resultCount, numOfFailed = 0;

	struct pg_result **results = NULL;
	StringInfoData errbuf;

	elog(DEBUG5, "dispatchDtxCommand: '%s' willMirror: %s", cmd, dispatchToMirrors ? "yes" : "no");

	initStringInfo(&errbuf);
	results = cdbdisp_dispatchRMCommand(cmd,
										withSnapshot,
										dispatchToMirrors,
										&errbuf, &resultCount);

	if (errbuf.len > 0)
	{
		ereport((raiseError ? ERROR : WARNING), (errmsg("DTM error (gathered %d results from cmd '%s')", resultCount, cmd),
						errdetail("%s", errbuf.data)));
		return false;
	}

	Assert(results != NULL);
	if (results == NULL)
	{
		numOfFailed++; /* If we got no results, we need to treat it as an error! */		
	}
	for (i = 0; i < resultCount; i++)
	{
		char			*cmdStatus;
		ExecStatusType	resultStatus;

		/* note: PQresultStatus() is smart enough to deal with results[i] == NULL */
		resultStatus = PQresultStatus(results[i]);
		if (resultStatus != PGRES_COMMAND_OK &&
			resultStatus != PGRES_TUPLES_OK)
		{
			numOfFailed++;
		}
		else
		{
			/*
			 * success ? If an error happened during a transaction which
			 * hasn't already been caught when we try a prepare we'll get a
			 * rollback from our prepare ON ONE SEGMENT: so we go look at the
			 * status, otherwise we could issue a COMMIT when we don't want
			 * to!
			 */
			cmdStatus = PQcmdStatus(results[i]);

			elog(DEBUG3, "DTM: status message cmd '%s' [%d] result '%s'", cmd, i, cmdStatus);
			if (strncmp(cmdStatus, cmd, strlen(cmdStatus)) != 0)
			{
				/* failed */
				numOfFailed++;
			}
		}
	}

	/* discard the errbuf text */
	pfree(errbuf.data);

	/* Now we clean up the results array. */
	cleanRMResultSets(results);

	return (numOfFailed == 0);
}

/* initialize a global transaction context */
static void
initGxact(TMGXACT * gxact)
{
	MemSet(gxact->gid, 0, TMGIDSIZE);
	gxact->gxid = InvalidDistributedTransactionId;
	gxact->state = DTX_STATE_NONE;

	/*
	 * Memory only fields.
	 */
	gxact->explicitBeginRemembered = false;
	
    gxact->xminGlobalSnapshot = 0;

	gxact->participationSummary = DTX_PARTICIPATION_SUMMARY_NONE;
	
	elog(DEBUG5,"initGxact gid = %s, gxid = %u (index = %d)", gxact->gid, gxact->gxid, gxact->debugIndex);
}

// UNDONE: Call this from where regular snapshots get created...
static void
createDtxSnapshot(DistributedSnapshot distributedSnapshot)
{
	int 						count;
	int                         i;
	TMGXACT    			*gxact_candidate;
	DtxState      		state;
	DistributedTransactionId 	indoubtxid;
	
	if (distributedSnapshot->xip == NULL)
	{
		distributedSnapshot->xip = (DistributedTransactionId *)
			malloc(max_tm_gxacts * sizeof(DistributedTransactionId));
	}

	/*
	 * Gather up current in-progress global transactions for the
	 * distributed snapshot.  Exclude ourselves.
	 */
	count = *shmNumGxacts - 1;
	distributedSnapshot->xmax = 0;
	distributedSnapshot->xcnt = 0;
	distributedSnapshot->xmin = UINT_MAX;
	for (i = 0; i < count; i++)
	{
		gxact_candidate = shmGxactArray[i];

		if (gxact_candidate == currentGxact)
			continue;

		state = gxact_candidate->state;
		switch (state)
		{
		case DTX_STATE_ACTIVE_NOT_DISTRIBUTED:
		case DTX_STATE_ACTIVE_DISTRIBUTED:
		case DTX_STATE_COMMITTING_ALL_READ_ONLY:
		case DTX_STATE_PREPARING:
		case DTX_STATE_PREPARED:
		case DTX_STATE_INSERTED_COMMITTED:
		case DTX_STATE_FORCED_COMMITTED:
		case DTX_STATE_NOTIFYING_COMMITTED:
		case DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED:
		case DTX_STATE_RETRYING_NOTIFY_COMMITTED: 
		case DTX_STATE_NOTIFYING_ABORTED:
		case DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED:
			/*
			 * Active or commit/abort not complete.  Keep this transaction
			 * considered for distributed snapshots.
			 */
			break;

		case DTX_STATE_COMMITTED_ALL_READ_ONLY:
		case DTX_STATE_INSERTED_FORGET_COMMITTED:
			elog(FATAL, "Should not see this transitional state with TM lock held -- dtx state \"%s\" not expected here",
				 DtxStateToString(state));
			break;
			
		case DTX_STATE_INSERTING_COMMITTED:
			elog(FATAL, "Cannot also be inserting COMMITTED into log buffer from another process with TM lock held");
			break;
			
		case DTX_STATE_INSERTING_FORGET_COMMITTED:
			elog(FATAL, "Cannot also be inserting FORGET COMMITTED into log buffer from another process with TM lock held");
			break;
			
		case DTX_STATE_CRASH_COMMITTED:
			/*
			 * From a previous system incarnation.
			 */
			continue;
			
		default:
			elog(FATAL, "Unexpected dtm state: %d",
				(int) state);
			break;
		}

		indoubtxid = gxact_candidate->gxid;
		if (indoubtxid < distributedSnapshot->xmin)
		{
			distributedSnapshot->xmin = indoubtxid;
		}
		if (indoubtxid > distributedSnapshot->xmax)
		{
			distributedSnapshot->xmax = indoubtxid;
		}

		distributedSnapshot->xip[distributedSnapshot->xcnt++] = indoubtxid;
		elog(LOG,"createDtx added gxid = %u to distributed snapshot", indoubtxid);
	}
	
	if (distributedSnapshot->xmin == UINT_MAX);
		distributedSnapshot->xmin = InvalidDistributedTransactionId;


	elog(LOG,"createDtx distributed snapshot has xmin = %u, xcnt = %u, xmax = %u.", 
		 distributedSnapshot->xmin, distributedSnapshot->xcnt, distributedSnapshot->xmax);
}

/*
 * Create a global transaction context from share memory.
 */
void
createDtx(TransactionId *pLocalXid)
{
	TMGXACT    	*gxact;
	
	/* UNDONE: Used to check if we have in-doubt transactions to resolve */
//	if (resolveDtx)
//		resolveInDoubtDtx();

	/* reset some transaction state variables */
	isDirty = false;

	getTmLock();

	if (*shmNumGxacts >= max_tm_gxacts)
	{
		dumpAllDtx();
		releaseTmLock();
		ereport(ERROR,
				(errmsg("cannot allocate global transaction")));
	}

	gxact = shmGxactArray[(*shmNumGxacts)++];
	initGxact(gxact);
	generateGID(gxact->gid, &gxact->gxid);
	
	AllocOrGetLocalXidForDistributedTransaction(gxact->gxid,pLocalXid);

	// UNDONE: Temporarily...
	createDtxSnapshot(CurrentDistributedSnapshot);
	
	gxact->xminGlobalSnapshot = CurrentDistributedSnapshot->xmin;

	Assert(gxact->participationSummary == DTX_PARTICIPATION_SUMMARY_NONE);
	
	gxact->state = DTX_STATE_ACTIVE_NOT_DISTRIBUTED;
	
	releaseTmLock();

	currentGxact = gxact;
}


/*
 * release global transaction to share memory
 */
static void
releaseGxact(void)
{
	int			i;
	int			curr;


	if (currentGxact == NULL)
	{
		elog(FATAL, "releaseGxact expected currentGxact to not be NULL");
	}
	
	elog(DEBUG5, "releaseGxact called for gid = %s (index = %d)", currentGxact->gid, currentGxact->debugIndex);
	
	getTmLock();
	/* find slot of current transaction */

	/*
	 * To maintain shmXminAllDistributedSnapshots we need to re-calculate,
	 * so we must scan the whole array for that purpose.  When we see
	 * the current one, we note its index and exclude it from the xmin
	 * calculation.
	 */
	curr = *shmNumGxacts;	// A bad value we can safely test.
	*shmXminAllDistributedSnapshots = UINT_MAX;
	for (i = 0; i < *shmNumGxacts; i++)
	{
		if (shmGxactArray[i] == currentGxact)
		{
			curr = i;
		}
		else
		{
			if (shmGxactArray[i]->xminGlobalSnapshot < *shmXminAllDistributedSnapshots)
			{
				*shmXminAllDistributedSnapshots = shmGxactArray[i]->xminGlobalSnapshot;
			}
		}
	}

	elog(DEBUG5,"releaseGxact re-calculated all distributed snapshots xmin = %u for %d global transactions.",
		 *shmXminAllDistributedSnapshots, *shmNumGxacts-1);

	/* move this to the next available slot */
	(*shmNumGxacts)--;
	if (curr != *shmNumGxacts)
	{
		shmGxactArray[curr] = shmGxactArray[*shmNumGxacts];
		shmGxactArray[*shmNumGxacts] = currentGxact;
	}
	currentGxact = NULL;
	releaseTmLock();
	isDirty = false;
}

/*
 * Get lock that serializes commits with DTM checkpoint info.
 * Change state to DTX_STATE_INSERTING_COMMITTED.
 */
void
insertingDistributedCommitted(void)
{
	elog(LOG,"insertingDistributedCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_PREPARED);
	currentGxact->state = DTX_STATE_INSERTING_COMMITTED;
}
				
/*
 * Change state to DTX_STATE_INSERTED_COMMITTED.
 * Release lock.
 */
void
insertedDistributedCommitted(void)
{
	elog(LOG,"insertedDistributedCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	Assert(currentGxact->state == DTX_STATE_INSERTING_COMMITTED);
	currentGxact->state = DTX_STATE_INSERTED_COMMITTED;
	releaseTmLock();
}


/*
 * Change state to DTX_STATE_FORCED_COMMITTED.
 */
void
forcedDistributedCommitted(void)
{
	elog(LOG,"forcedDistributedCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_INSERTED_COMMITTED);
	currentGxact->state = DTX_STATE_FORCED_COMMITTED;
	releaseTmLock();
}
				


/*
 * Get check point information
 */
void
getDtxCheckPointInfoAndLock(char **result, int *result_size)
{
	TMGXACT    	*gxact;
	DtxState	  	state;
	int				  	n;
	int					segmentCount;
	TMGXACT_CHECKPOINT 	*gxact_checkpoint;
	TMGXACT_LOG 	  	*gxact_log_array;
	int				  	i;
	int				  	actual;
	TMGXACT_LOG       	*gxact_log;

	if (shmDtmStarted == NULL ||
		(*shmDtmStarted && *shmSegmentCount == 0))
	{
		*result_size = offsetof(TMGXACT_CHECKPOINT, committedGxactArray);
		gxact_checkpoint = palloc(*result_size);
		gxact_checkpoint->committedCount = 0;
		gxact_checkpoint->segmentCount = 0;
		*result = (char*)gxact_checkpoint;

		getTmLock();	// Return withh lock held.
		return;
	}

	getTmLock();	// We will return with lock held below.
	n = *shmNumGxacts;
	segmentCount = *shmSegmentCount;
	
	gxact_checkpoint = palloc(offsetof(TMGXACT_CHECKPOINT, committedGxactArray) + n * sizeof(TMGXACT_LOG));
	gxact_log_array = &gxact_checkpoint->committedGxactArray[0];

	actual = 0;
	for (i = 0; i < n; i++)
	{
		gxact = shmGxactArray[i];

		gxact_log = &gxact_log_array[actual];

		state = gxact->state;
		switch (state)
		{
		case DTX_STATE_ACTIVE_NOT_DISTRIBUTED:
		case DTX_STATE_ACTIVE_DISTRIBUTED:
		case DTX_STATE_COMMITTING_ALL_READ_ONLY:
		case DTX_STATE_PREPARING:
		case DTX_STATE_PREPARED:
			/*
			 * Active or attempting to commit -- ignore.
			 */
			continue;

		case DTX_STATE_INSERTED_FORGET_COMMITTED:
		case DTX_STATE_COMMITTED_ALL_READ_ONLY:
			elog(FATAL, "Should not see this transitional state with TM lock held -- dtx state \"%s\" not expected here",
				 DtxStateToString(state));
			continue;
			
		case DTX_STATE_INSERTING_COMMITTED:
			elog(FATAL, "Cannot also be buffering COMMITTED from another process with TM lock held");
			continue;
			
		case DTX_STATE_INSERTING_FORGET_COMMITTED:
			elog(FATAL, "Cannot also be buffering FORGET COMMITTED from another process with TM lock held");
			continue;
			
		case DTX_STATE_FORCED_COMMITTED:
		case DTX_STATE_NOTIFYING_COMMITTED:
		case DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED:
		case DTX_STATE_RETRYING_NOTIFY_COMMITTED:
		case DTX_STATE_CRASH_COMMITTED:
			break;
						
		case DTX_STATE_NOTIFYING_ABORTED:
		case DTX_STATE_NOTIFY_ABORTED_RETRY_NEEDED:
			/*
			 * We don't checkpoint abort.
			 */
			continue;
			
		default:
			elog(FATAL, "Unexpected dtm state: %d",
				(int) state);
			break;
		}
		
		memcpy(gxact_log->gid, gxact->gid, TMGIDSIZE);
		gxact_log->gxid = gxact->gxid;

		actual++;
	}
	
	gxact_checkpoint->committedCount = actual;
	if (gxact_checkpoint->committedCount == 0)
	{
		gxact_checkpoint->segmentCount = 0;
	}
	else
	{
		gxact_checkpoint->segmentCount = *shmSegmentCount;
	}

	*result = (char*)gxact_checkpoint;
	*result_size = offsetof(TMGXACT_CHECKPOINT, committedGxactArray) + actual * sizeof(TMGXACT_LOG);

	// Return with lock held.
}

void 
freeDtxCheckPointInfoAndUnlock(char *info, int info_size)
{
	pfree(info);

	releaseTmLock();
}

/* generate global transaction id */
static void
generateGID(char *gid, DistributedTransactionId *gxid)
{
	time_t		t = time(NULL);

	if (t == (time_t) -1)
	{
		elog(PANIC, "cannot generate global transaction id");
	}

	/* tm lock acquired by caller */
	*gxid = (*shmGIDSeq)++;
	sprintf(gid, "%ld-%.10d", t, (*gxid));
}



/*
 * recoverTM:
 * perform TM recovery, this connects to all QE and resolve all in-doubt txn.
 *
 * This gets called when there is not any other DTM activity going on.
 *
 * First, we'll replay the dtm log and get our shmem as up to date as possible
 * in order to help resolve in-doubt transactions.	Then we'll go through and
 * try and resolve in-doubt transactions based on information in the DTM log.
 * The remaining in-doubt transactions that remain (ones the DTM doesn't know
 * about) are all ABORTed.
 */
static void
recoverTM(void)
{

	/* intialize fts sync count */
	verifyFtsSyncCount();

	elog(DEBUG3, "Starting to Recover DTM...");

	/*
	 * do not do recovery if read only mode is set.
	 * in this case, there may be in-doubt transaction in down segdb
	 * , which will not be resolved at this time.
	 */
	if (isFtsReadOnlySet())
	{
		elog(DEBUG3, "FTS is Read Only.  Defer DTM recovery till later.");
		currentGxact = NULL;
		return;
	}


	/*
	 * Attempt to recover all in-doubt transactions.
	 *
	 * first resolve all in-doubt transactions from the DTM's perspective
	 * and then resolve any remaining in-doubt transactions that the
	 * RMs have.
	 */
	recoverInDoubtTransactions();


	/* finished recovery successfully. */

	*shmDtmStarted = true;
	elog(LOG,"DTM Started");

}



/* recoverInDoubtTransactions:
 * Go through all in-doubt transactions that the DTM knows about and
 * resolve them.
 */
static bool
recoverInDoubtTransactions(void)
{
	HTAB *htab;
	int skipIndex;

	elog(DEBUG5,"recoverInDoubtTransactions entering...");
	
	/*
	 * For each committed transaction found in the redo pass that was not
	 * matched by a forget committed record, change its state indicating
	 * committed notification needed.  Attempt a notification.
	 */
	skipIndex = 0;
	while (skipIndex < *shmNumGxacts)
	{
		currentGxact = shmGxactArray[skipIndex];

		if (currentGxact->state != DTX_STATE_CRASH_COMMITTED)
		{
			elog(PANIC, "recoverInDoubtTransactions found transaction in '%s' state and was expecting it to be in '%s' state",
				 DtxStateToString(currentGxact->state), DtxStateToString(DTX_STATE_CRASH_COMMITTED));
		}
		currentGxact->state = DTX_STATE_NOTIFY_COMMITTED_RETRY_NEEDED;

		elog(DEBUG5,"recoverInDoubtTransactions calling doRetryingNotifyCommitted for gid = %s", currentGxact->gid);
		if (!doRetryingNotifyCommitted())
		{
			skipIndex++;
		}
	}

	dumpAllDtx();

	// UNDONE: Thus, any in-doubt transctions found will be for aborted transactions.
	// UNDONE: Gather in-boubt transactions and issue aborts.
	elog(DEBUG5,"recoverInDoubtTransactions calling gatherRMInDoubtTransactions...");
	htab = gatherRMInDoubtTransactions();

	/*
	 * go through and resolve any remaining in-doubt transactions that the
	 * RM's have AFTER recoverDTMInDoubtTransactions.  ALL of these in doubt
	 * transactions will be ABORT'd.  The fact that the DTM doesn't know about
	 * them means that something bad happened before everybody voted to
	 * COMMIT.
	 */
	abortRMInDoubtTransactions(htab);

	/* get rid of the hashtable */
	hash_destroy(htab);

	/* yes... we are paranoid and will double check */
	htab = gatherRMInDoubtTransactions();


	/*
	 * Hmm.  we still have some remaining indoubt transactions.  For now we
	 * dont have an automated way to clean this mess up.  So we'll have to
	 * rely on smart Admins to do the job manually.  We'll error out of here
	 * and try and provide as much info as possible.
	 *
	 * TODO: We really want to be able to say this particular segdb has these
	 * remaining in-doubt transactions.
	 */
	if (htab != NULL)
	{
		StringInfoData indoubtBuff;

		initStringInfo(&indoubtBuff);

		dumpAllDtx();
		dumpRMOnlyDtx(htab, &indoubtBuff);

		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			   errmsg("DTM Log recovery failed.  There are still unresolved "
					  "in-doubt transactions on some of the segment databaes "
				"that were not able to be resolved for an unknown reason. "),
		errdetail("Here is a list of in-doubt transactions in the system: %s",
				  indoubtBuff.data),
			errhint("Try restarting the Greenplum Database array.  If the problem persists "
				 " an Administrator will need to resolve these transactions "
					" manually.")));

	}

	return true;
}



/*
 * gatherRMInDoubtTransactions:
 * Builds a hashtable of all of the in-doubt transactions that exist on the
 * segment databases.  The hashtable basically just serves as a single list
 * without duplicates of all the in-doubt transactions.  It does not keep track
 * of which seg db's have which transactions in-doubt.  It currently doesn't
 * need to due to the way we handle this information later.
 */
static HTAB *
gatherRMInDoubtTransactions(void)
{
	bool		dispatchToMirrors = true;
	PGresult  **pgresultSets = NULL;
	StringInfoData errmsgbuf;
	const char *cmdbuf = "select gid from pg_prepared_xacts";
	PGresult  **rs;

	InDoubtDtx *lastDtx = NULL;

	HASHCTL		hctl;
	HTAB	   *htab = NULL;

	int			j,
				rows;
	bool		found;

	initStringInfo(&errmsgbuf);

	/* call to all QE to get in-doubt transactions */
	pgresultSets = cdbdisp_dispatchRMCommand(cmdbuf,
											 /* withSnapshot */false,
											 dispatchToMirrors,
											 &errmsgbuf, NULL);

	/* display error messages */
	if (errmsgbuf.len > 0)
	{
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					errmsg("Unable to collect list of prepared transactions "
						   "from all segment databases."),
						errdetail("%s", errmsgbuf.data)));

		pfree(errmsgbuf.data);

		/* need recovery */
		return NULL;
	}


	pfree(errmsgbuf.data);


	/* If any result set is nonempty, there are in-doubt transactions. */
	for (rs = pgresultSets; *rs; ++rs)
	{
		rows = PQntuples(*rs);

		for (j = 0; j < rows; j++)
		{
			char	   *gid;

			/*
			 * we dont setup our hashtable until we know we have at least one
			 * in doubt transaction
			 */
			if (htab == NULL)
			{

				/* setup a hash table */
				hctl.keysize = TMGIDSIZE;		/* GID */
				hctl.entrysize = sizeof(InDoubtDtx);


				htab = hash_create("InDoubtDtxHash", 10, &hctl, HASH_ELEM);

				if (htab == NULL)
				{
					ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
									errmsg("DTM could not allocate hash table for InDoubtDtxList.")));
				}


			}

			gid = PQgetvalue(*rs, j, 0);


			/* Now we can add entry to hash table */
			lastDtx = (InDoubtDtx *) hash_search(htab, gid, HASH_ENTER, &found);

			/*
			 * only need to bother doing work if there isn't already an entry
			 * for our GID
			 */
			if (!found)
			{
				elog(DEBUG3, "Found in-doubt transaction with GID: %s on remote RM", gid);

				strcpy(lastDtx->gid, gid);
			}

		}
	}

	cleanRMResultSets(pgresultSets);

	return htab;
}

static bool
FindRetryNotifyCommitted(char* gid, TMGXACT **gxact)
{
	int i;
	
	for (i = 0; i < *shmNumGxacts; i++)
	{
		if (strcmp(gid, shmGxactArray[i]->gid) == 0)
		{
			/* found an retry notify committed */
			*gxact = shmGxactArray[i];
			return true;
		}
	}

	return false;
}

/*
 * abortRMInDoubtTransactions:
 * Goes through all the InDoubtDtx's in the provided htab and ABORTs them
 * across all of the QEs by sending a ROLLBACK PREPARED.
 *
 */
static void
abortRMInDoubtTransactions(HTAB *htab)
{
	HASH_SEQ_STATUS status;
	char	   *cmd = NULL,
				cmdbuf[100];
	InDoubtDtx *entry = NULL;
	TMGXACT *gxact;

	if (htab == NULL)
		return;

	/*
	 * now we have a nice hashtable full of in-doubt dtx's that we need to
	 * resolve.  so we'll use a nice big hammer to get this job done.  instead
	 * of keeping track of which QEs have a prepared txn to be aborted and
	 * which ones don't.  we just issue a ROLLBACK to all of them and ignore
	 * any pesky errors.  This is certainly not an elegant solution but is OK
	 * for now.
	 */
	hash_seq_init(&status, htab);


	while ((entry = (InDoubtDtx *) hash_seq_search(&status)) != NULL)
	{
		if (FindRetryNotifyCommitted(entry->gid, &gxact))
		{
			elog(DEBUG5,"abortRMInDoubtTransactions skipping gid = %s since it is \"%s\"",
				 gxact->gid, DtxStateToString(gxact->state));
			continue;
		}
	
		sprintf(cmdbuf, "ROLLBACK PREPARED '%s'", entry->gid);
		cmd = cmdbuf;

		elog(DEBUG3, "Aborting in-doubt transaction with GID: %s", entry->gid);
		
		doAbortInDoubt(entry->gid);

	}
}


/*
 * removeInDoubtTransactions:
 * Used during TM recovery to remove the in-doubt transaction in the provided
 * htab that has the same gid as the one provided.
 */
static void
removeInDoubtTransaction(HTAB *htab, char *gid)
{
	bool		found;
	InDoubtDtx *dtx;

	if (htab != NULL && gid != NULL)
		dtx = hash_search(htab, gid, HASH_REMOVE, &found);
}


/*
 * cleanRMResultSets:
 * Goes the set of PGresults passed in and clears and frees them.
 */
static void
cleanRMResultSets(PGresult **pgresultSets)
{
	/* TODO: need to handle multiple dbs in a segment case */
	/*
	 int tolaldbs = CdbClusterDefinition->segment_count * CdbClusterDefinition->db_count;
	 */
	int			i = 0;
	PGresult   *rs = pgresultSets[0];

	do
	{
		PQclear(rs);
	} while ((rs = pgresultSets[++i]) != NULL);
	free(pgresultSets);
}

/*
 * dumpAllDtx:
 * used to log the current state (according to the DTM) of all the
 * global distributed transactions that are still active (in shmem).
 */
static void
dumpAllDtx(void)
{
	int			i;

	elog(LOG, "dumping all global transactions: ");
	for (i = 0; i < *shmNumGxacts; i++)
	{
		elog(LOG, "%d - GID: %s  STATE: %s",
			 i, shmGxactArray[i]->gid, 
			 DtxStateToString(shmGxactArray[i]->state));
	}
}


static void
dumpRMOnlyDtx(HTAB *htab, StringInfoData *buff)
{
	HASH_SEQ_STATUS status;
	InDoubtDtx *entry = NULL;

	if (htab == NULL)
		return;

	hash_seq_init(&status, htab);

	appendStringInfo(buff, "List of In-doubt transactions remaining across the segdbs: (");

	while ((entry = (InDoubtDtx *) hash_seq_search(&status)) != NULL)
	{
		appendStringInfo(buff, "\"%s\" , ", entry->gid);
	}

	appendStringInfo(buff, ")");

}


/*
 * When called, a SET command is dispatched and the writer gang
 * writes the shared snapshot. This function actually does nothing
 * useful besides making sure that a writer gang is alive and has
 * set the shared snapshot so that the readers could access it.
 *
 * At this point this function is added as a helper for cursor 
 * query execution since in MPP cursor queries don't use writer
 * gangs. However, it could be used for other purposes as well.
 *
 * See declaration of assign_gp_write_shared_snapshot(...) for more
 * information.
 */
void verify_shared_snapshot_ready(void)
{
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		char cmdbuf[35];
		strcpy(cmdbuf, "set gp_write_shared_snapshot=true" );
		
		CdbDoCommand(cmdbuf, true, true, true, true);
	}
}

/*
 * Force the writer QE to write the shared snapshot. Will get called
 * after a "set gp_write_shared_snapshot=<true/false>" is executed 
 * in dispatch mode.
 *
 * See verify_shared_snapshot_ready(...) for additional information.
 */
bool
assign_gp_write_shared_snapshot(bool newval, bool doit, GucSource source)
{
	
#if FALSE
	elog(DEBUG1, "SET gp_write_shared_snapshot: %s, doit=%s",
		 (newval ? "true" : "false"), (doit ? "true" : "false"));
#endif
	/* make sure newval is "true". if it's "false" this could be a part
	 * of a ROLLBACK so we don't want to set the snapshot then.
	 */
	if (doit && newval)
	{
		if(Gp_role == GP_ROLE_EXECUTE)
			ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());			
	}

	return true;
}

void
getDistributedSnapshotSerializeInfo(int *serializedLen,
                             DistributedTransactionId *currentDistributedTransactionId,
                             DistributedTransactionId *xminAllDistributedSnapshots,
                             int *qdSharedLocalSnapshotSlotId,
                             DistributedSnapshot *distributedSnapshot)
{
	
	*serializedLen = 0;
	*currentDistributedTransactionId = InvalidDistributedTransactionId;
	*xminAllDistributedSnapshots = InvalidDistributedTransactionId;
	*qdSharedLocalSnapshotSlotId = 0;
	*distributedSnapshot = NULL;
	

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		if (currentGxact != NULL)
		{
			*currentDistributedTransactionId = currentGxact->gxid;

			if (*currentDistributedTransactionId != InvalidDistributedTransactionId)
			{
				getTmLock();
				*xminAllDistributedSnapshots = (*shmXminAllDistributedSnapshots == UINT_MAX ? 0 : *shmXminAllDistributedSnapshots);
				releaseTmLock();

				*qdSharedLocalSnapshotSlotId = gp_session_id;

				*distributedSnapshot = CurrentDistributedSnapshot;
				elog(DEBUG5,"getDistributedSnapshotSerializeInfo Gp_role is DISPATCH and have currentGxact = %s, gxid = %u --> have distributed snapshot",
				 	currentGxact->gid, currentGxact->gxid); 
			}
			else
			{
				elog(DEBUG5,"getDistributedSnapshotSerializeInfo Gp_role is DISPATCH and currentGxact is non-NULL, but gxid is InvalidDistributedTransactionId");
			}
		}
		else
		{
			elog(DEBUG5,"getDistributedSnapshotSerializeInfo Gp_role is DISPATCH but no currentGxact --> no distributed snapshot"); 
		}
	}
	else if (Gp_role == GP_ROLE_EXECUTE)
	{
		*currentDistributedTransactionId = QDSentDistributedXid;
		*qdSharedLocalSnapshotSlotId = QDSentSharedLocalSnapshotSlotId;
		*distributedSnapshot = QDSentDistributedSnapshot;
		elog(DEBUG5,"getDistributedSnapshotSerializeInfo Gp_role is EXECUTE QDSentDistributedXid = %u --> have distributed snapshot",
			 QDSentDistributedXid); 
	}

	if (*currentDistributedTransactionId != InvalidDistributedTransactionId)
	{
		*serializedLen = sizeof(DistributedTransactionId)*4 + 
			             sizeof(int) + /* slot id */
						 sizeof(uint32) + /*xcnt*/
						 (*distributedSnapshot)->xcnt*sizeof(DistributedTransactionId) +
						 sizeof(CommandId);
	}
	else
	{
		*serializedLen = sizeof(DistributedTransactionId);
	}

	elog(DEBUG5,"getDistributedSnapshotSerializeInfo is returning serializedLen = %d", *serializedLen);
}

void
serializeDistributedSnapshot(char **buffref,
                             DistributedTransactionId currentDistributedTransactionId,
                             DistributedTransactionId xminAllDistributedSnapshots,
                             int qdSharedLocalSnapshotSlotId,
                             DistributedSnapshot distributedSnapshot,
                             CommandId curcid)
{
	char *p = *buffref;
	int i;

	if (currentDistributedTransactionId != InvalidDistributedTransactionId)
	{
		Assert(distributedSnapshot != NULL);
		
		memcpy(p,&currentDistributedTransactionId,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);

		memcpy(p,&xminAllDistributedSnapshots,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		memcpy(p,&qdSharedLocalSnapshotSlotId,sizeof(int)); p += sizeof(int);
		
		memcpy(p,&distributedSnapshot->xmin,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		memcpy(p,&distributedSnapshot->xmax,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		memcpy(p,&distributedSnapshot->xcnt,sizeof(uint32)); p += sizeof(uint32);
		for (i = 0; i < distributedSnapshot->xcnt; i++)
		{
			memcpy(p,&distributedSnapshot->xip[i],sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		}

		memcpy(p,&curcid, sizeof(CommandId)); p += sizeof(CommandId);
		
		elog(DEBUG5,"serializeDistributedSnapshot copied information");
	}
	else
	{
		memcpy(p,&currentDistributedTransactionId,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		elog(DEBUG5,"serializeDistributedSnapshot only copied InvalidDistributedTransactionId");
	}


	*buffref = p;
}

static void
clearDistributedSnapshot(DistributedSnapshot distributedSnapshot)
{
	distributedSnapshot->xmin = InvalidDistributedTransactionId;		
	distributedSnapshot->xmax = InvalidDistributedTransactionId;		
	distributedSnapshot->xcnt = 0;		
	distributedSnapshot->xip = NULL;
}

void
getGlobalSnapshot(const char *serializedSnapshot, int serializedSnapshotlen)
{			
	int i;
				
	if (serializedSnapshotlen > 0)
	{			
		int xipsize;
		const char * p = serializedSnapshot;		
						
		memcpy(&QDSentDistributedXid,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
		if (QDSentDistributedXid != InvalidDistributedTransactionId)
		{
			memcpy(&QDSentXminAllDistributedSnapshots,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
			memcpy(&QDSentSharedLocalSnapshotSlotId,p,sizeof(int)); 							    p+=sizeof(int);		
			memcpy(&QDSentDistributedSnapshot->xmin,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
			memcpy(&QDSentDistributedSnapshot->xmax,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
			memcpy(&QDSentDistributedSnapshot->xcnt,p,sizeof(uint32)); 	p+=sizeof(uint32);		

			xipsize = sizeof(DistributedTransactionId)*QDSentDistributedSnapshot->xcnt;
			QDSentDistributedSnapshot->xip = palloc(xipsize);
			memcpy(QDSentDistributedSnapshot->xip,p,xipsize); p += xipsize;

			memcpy(&QDSentDistributedCommandId,p,sizeof(CommandId)); 	p+=sizeof(CommandId);	

			elog(LOG,"getGlobalSnapshot QDSentDistributedXid = %u, QDSentXminAllDistributedSnapshots = %u, QDSentDistributedSnapshot (xmin = %u, xmax = %u, xcnt = %u)",
				QDSentDistributedXid, QDSentXminAllDistributedSnapshots, 
				QDSentDistributedSnapshot->xmin, QDSentDistributedSnapshot->xmax, QDSentDistributedSnapshot->xcnt);
			for (i = 0; i < QDSentDistributedSnapshot->xcnt; i++)
			{
				elog(LOG,"....    QDSentDistributedSnapshot->xip[%d] = %u", i, QDSentDistributedSnapshot->xip[i]);
			}
			elog(LOG,"....QDSentDistributedCommandId = %u", QDSentDistributedCommandId);

		}
		else
		{
			elog(LOG,"getGlobalSnapshot QDSentDistributedXid was InvalidDistributedTransactionId");
		}


	}
	else
	{
		QDSentDistributedXid = InvalidDistributedTransactionId;
	}

	if (QDSentDistributedXid == InvalidDistributedTransactionId)
	{
		elog(LOG, "getGlobalSnapshot No distributed snapshot");
		
		QDSentXminAllDistributedSnapshots = InvalidDistributedTransactionId;
		QDSentSharedLocalSnapshotSlotId = 0;

		clearDistributedSnapshot(QDSentDistributedSnapshot);
		
		QDSentDistributedCommandId = 0;
	}
}

static void
doQEAddLocalXidsForDistributedIndoubt()
{
	elog(DEBUG5, "Calling AddLocalXidsForDistributedTransactions to map distributed in-doubt XIDs to local XIDs.");
	AddLocalXidsForDistributedTransactions(QDSentDistributedSnapshot->xip, QDSentDistributedSnapshot->xcnt);

	if (QDSentXminAllDistributedSnapshots != InvalidDistributedTransactionId)
	{
		Assert(QDSentDistributedXid >= QDSentXminAllDistributedSnapshots);
		
		elog(DEBUG5, "Calling TruncateDistributedXidMap with %u.", QDSentXminAllDistributedSnapshots);
		TruncateDistributedXidMap(QDSentXminAllDistributedSnapshots);
	}
	else
		elog(DEBUG5, "Skipping call to TruncateDistributedXidMap");
}


static void
doQEDistributedExplicitBegin()
{
	int ExplicitIsoLevel;
	bool ExplicitReadOnly;
	
	/*
	 * Start a command.
	 */
	StartTransactionCommand();

	// Here is the explicit BEGIN.
	BeginTransactionBlock();

	unpackMppTxnOptions(QDSentTxnOptions,
		&ExplicitIsoLevel, &ExplicitReadOnly);

	XactIsoLevel = ExplicitIsoLevel;
	XactReadOnly = ExplicitReadOnly;

	/*
	 * Finish the BEGIN command.  It will leave the explict transaction
	 * in-progress.
	 */
	CommitTransactionCommand();
	
}

void
doQEUpdateDistributedSnapshot()
{
	doQEAddLocalXidsForDistributedIndoubt();

	// UNDONE
}

void
determineDistributedTransactionContext (void)
{
	bool statementWantsDtxTransaction;
	bool dirty;
	bool explicitBegin;
	bool haveDistributedSnapshot;
	bool isQueryDispatcher = false;
	bool isEntryDbSingleton = false;
	bool isReaderQE = false;
	bool isWriterQE = false;
	bool isDtmStarted = false;
	bool isSharedLocalSnapshotSlotPresent;
	
	statementWantsDtxTransaction = isMppTxOptions_StatementWantsDtxTransaction(QDSentTxnOptions);
	dirty = isMppTxOptions_Dirty(QDSentTxnOptions);
	explicitBegin = isMppTxOptions_ExplicitBegin(QDSentTxnOptions);
	
	haveDistributedSnapshot = (QDSentDistributedXid != InvalidDistributedTransactionId);
	isDtmStarted = (shmDtmStarted != NULL && *shmDtmStarted);
	isSharedLocalSnapshotSlotPresent = (SharedLocalSnapshotSlot != NULL);

	elog(LOG, "determineDistributedTransactionContext inputs (part 1): Gp_role = %s, Gp_is_writer = %s, txnOptions = 0x%x, statementWantsDtxTransaction = %s, dirty = %s, explicitBegin = %s, haveDistributedSnapshot = %s, isDtmStarted = %s",
		 role_to_string(Gp_role), (Gp_is_writer ? "true" : "false"), QDSentTxnOptions,
		 (statementWantsDtxTransaction ? "true" : "false"), (dirty ? "true" : "false"), (explicitBegin ? "true" : "false"), 
		 (haveDistributedSnapshot ? "true" : "false"),
		 (isDtmStarted ? "true" : "false")); 
    elog(LOG, "determineDistributedTransactionContext inputs (part 2): QDSentDistributedXid = %u, isSharedLocalSnapshotSlotPresent = %s.",
		 QDSentDistributedXid,
	     (isSharedLocalSnapshotSlotPresent ? "true" : "false"));
	if (haveDistributedSnapshot)
	{
		elog(LOG, "determineDistributedTransactionContext inputs (part 2a): QDSentDistributedXid = %u, QDSentXminAllDistributedSnapshots = %u, QDSentDistributedSnapshot (xmin = %u, xmax = %u, xcnt = %u), QDSentDistributedCommandId = %d",
	              QDSentDistributedXid, 
	              QDSentXminAllDistributedSnapshots, 
	              QDSentDistributedSnapshot->xmin, QDSentDistributedSnapshot->xmax, QDSentDistributedSnapshot->xcnt,
	              QDSentDistributedCommandId);
	}
	if (isSharedLocalSnapshotSlotPresent)
	{
		elog(LOG, "determineDistributedTransactionContext inputs (part 2b):  shared local snapshot xid = %u (xmin: %u xmax: %u xcnt: %u) curcid: %d, QDxid = %u, QDcid = %u", 
			 SharedLocalSnapshotSlot->xid,
			 SharedLocalSnapshotSlot->snapshot.xmin,
			 SharedLocalSnapshotSlot->snapshot.xmax,
 			 SharedLocalSnapshotSlot->snapshot.xcnt,
			 SharedLocalSnapshotSlot->snapshot.curcid,
			 SharedLocalSnapshotSlot->QDxid,
			 SharedLocalSnapshotSlot->QDcid);
	}

	switch (Gp_role)
	{
	case GP_ROLE_DISPATCH:
		if (isDtmStarted)
		{
			isQueryDispatcher = true;
		}
		break;
		
	case GP_ROLE_EXECUTE:
		if (Gp_segment == -1 && !Gp_is_writer)
		{
			isEntryDbSingleton = true;
		}
		else
		{
			/*
			 * NOTE: this is a bit hackish. It appears as though
			 * StartTransaction() gets called during connection setup before
			 * we even have time to setup our shared snapshot slot.
			 */
			if (SharedLocalSnapshotSlot == NULL)
			{
				if (explicitBegin || haveDistributedSnapshot)
				{
					elog(ERROR, "determineDistributedTransactionContext not expecting distributed begin or snapshot when no Snapshot slot exists");
				}
			} 
			else 
			{
				if (Gp_is_writer)
				{
					isWriterQE = true;
				}
				else
				{
					isReaderQE = true;
				}
			}
		}
		break;
		
	default:
		Assert(DistributedTransactionContext == DTX_CONTEXT_LOCAL_ONLY);
		elog(DEBUG5, "determineDistributedTransactionContext leaving context = 'Local Only' for Gp_role = %s", role_to_string(Gp_role) );
		return;
	}

	elog(LOG, "determineDistributedTransactionContext intermediate result: isQueryDispatcher = %s, isEntryDbSingleton = %s, isWriterQE = %s, isReaderQE = %s.", 
		 (isQueryDispatcher ? "true" : "false"), (isEntryDbSingleton ? "true" : "false"),
		 (isWriterQE ? "true" : "false"), (isReaderQE ? "true" : "false"));

	if (haveDistributedSnapshot)
	{
		/*
		 * Compensate for the fact that distribute work arrives at the QEs
		 * at different times.
		 */
		doQEAddLocalXidsForDistributedIndoubt();
	}

	switch (DistributedTransactionContext)
	{
	case DTX_CONTEXT_LOCAL_ONLY:
		if (isQueryDispatcher)
		{
			DistributedTransactionContext = DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE;
		}
		else if (isEntryDbSingleton && haveDistributedSnapshot)
		{
			/*
			 * Later, in GetSnapshotData, we will adopt the QD's
			 * transaction and snapshot information.
			 */
			 
			DistributedTransactionContext = DTX_CONTEXT_QE_ENTRY_DB_SINGLETON;
		}
		else if (isReaderQE && haveDistributedSnapshot)
		{
			/*
			 * Later, in GetSnapshotData, we will adopt the QE Writer's
			 * transaction and snapshot information.
			 */
			 
			DistributedTransactionContext = DTX_CONTEXT_QE_READER;
		}
		else if (isWriterQE && (explicitBegin || statementWantsDtxTransaction))
		{
			if (!haveDistributedSnapshot)
			{
				elog(LOG, "QE Writer is involved in a distributed transaction without a distributed snapshot...");
			}
			
			if (IsTransactionOrTransactionBlock())
			{
				elog(ERROR, "Starting an explict distributed transaction in QE -- cannot already be in a transaction");
			}

			if (explicitBegin)
			{
				/*
				 * We set the DistributedTransactionContext BEFORE we create the
				 * transactions to influence the behavior of 
				 * StartTransaction.
				 */
				DistributedTransactionContext = DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER;
				
				doQEDistributedExplicitBegin();
			}
			else
			{
				Assert(statementWantsDtxTransaction);
				DistributedTransactionContext = DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER;
			}
		}
		else if (haveDistributedSnapshot)
		{
			if (IsTransactionOrTransactionBlock())
			{
				elog(ERROR,"Going to start a local implicit transaction in QE using a distribute snapshot -- cannot already be in a transaction");
			}

			/*
			 * Before executing the query, postgres.c make a standard call to
			 * StartTransactionCommand which will begin a local transaction with 
			 * StartTransaction.  This is fine.
			 *
			 * However, when the snapshot is created later, the state below will 
			 * tell GetSnapshotData to make the local snapshot from the
			 * distributed snapshot.
			 */
			DistributedTransactionContext = DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT;
		}
		else
		{
			Assert (!haveDistributedSnapshot);
			
			/*
			 * A local implicit transaction without reference to
			 * a distributed snapshot.  Stay in NONE state.
			 */
			Assert(DistributedTransactionContext == DTX_CONTEXT_LOCAL_ONLY);
		}
		break;

	case DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE:
		// Ok.
		break;

	case DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER:
//		elog(NOTICE, "We should have left this transistion state '%s' at the end of the previous command...",
//			 DtxContextToString(DistributedTransactionContext));
		Assert (IsTransactionOrTransactionBlock());
		
		if (explicitBegin)
		{
			elog(ERROR,"Cannot have an explicit BEGIN statement...");
		}

		if (haveDistributedSnapshot)
		{
			doQEUpdateDistributedSnapshot();
		}
		break;
		
	case DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT:
		elog(ERROR, "We should have left this transistion state '%s' at the end of the previous command",
			 DtxContextToString(DistributedTransactionContext));
		break;
		
	case DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER:
		Assert (IsTransactionOrTransactionBlock());
		
		if (haveDistributedSnapshot)
		{
			doQEUpdateDistributedSnapshot();
		}
		break;

	case DTX_CONTEXT_QE_ENTRY_DB_SINGLETON:
	case DTX_CONTEXT_QE_READER:
		/*
		 * We are playing games with the xact.c code, so we shouldn't test
		 * with the IsTransactionOrTransactionBlock() routine.
		 */
		
		if (haveDistributedSnapshot)
		{
			doQEUpdateDistributedSnapshot();
		}
		break;

	case DTX_CONTEXT_QE_PREPARED:
		elog(ERROR, "We should not be trying to execute a query in state '%s'",
			 DtxContextToString(DistributedTransactionContext));
		break;
		
	default:
		elog(FATAL, "Unexpected QE distribute transaction context value: %d",
			(int) DistributedTransactionContext);
		break;
	}

	elog(LOG, "determineDistributedTransactionContext final result: DistributedTransactionContext = '%s'.", 
		 DtxContextToString(DistributedTransactionContext));
	
//	if (Gp_role == GP_ROLE_DISPATCH)
//	{
//		Assert(false);		// UNDONE !!!!
//		elog(DEBUG1,"QD Got snapshot from QE, xid = %d",QDSentXID);
//		ActiveSnapshot = CopySnapshot(QDSentSnapshot);
//	}
}

void
finishDistributedTransactionContext (void)
{
	DistributedTransactionContext = DTX_CONTEXT_LOCAL_ONLY;

	QDSentTxnOptions = 0;
	QDSentDistributedXid = InvalidDistributedTransactionId;
	QDSentXminAllDistributedSnapshots = InvalidDistributedTransactionId;
	QDSentSharedLocalSnapshotSlotId = 0;
	clearDistributedSnapshot(QDSentDistributedSnapshot);

	if(currentGxact != NULL)
	{
		elog(FATAL, "Expected currentGxact to be NULL at this point.  Found gid =%s, gxid = %u",
			currentGxact->gid, currentGxact->gxid);
	}
}

void
rememberDtxExplicitBegin()
{
	if (currentGxact == NULL)
	{
		return;
	}

	if (currentGxact->state == DTX_STATE_ACTIVE_NOT_DISTRIBUTED)
	{
		/*
		 * We do not change the state to distributed until a query statement
		 * is sent.  We just remember the BEGIN occurred.
		 */
		elog(DEBUG5,"rememberDtxExplicitBegin explicit BEGIN for gid = %s", currentGxact->gid);

		currentGxact->explicitBeginRemembered = true;
	}
	else if (currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED)
	{
		elog(LOG,"rememberDtxExplicitBegin remembering an explicit BEGIN for gid = %s which is distribuuted (explicitBeginRemembered = %s",
			 currentGxact->gid, (currentGxact->explicitBeginRemembered ? "true" : "false"));
	}
	else
	{
		elog(ERROR,"DTM transaction is not active");
	}
}

void
performDtxProtocolCommand(DtxProtocolCommand dtxProtocolCommand,
					char* loggingStr, char* gid, DistributedTransactionId gxid)
{
	elog(LOG, "performDtxProtocolCommand called with DTX protocol = %s, QE distribute transaction context: '%s'",
		DtxProtocolCommandToString(dtxProtocolCommand), DtxContextToString(DistributedTransactionContext));
	
	switch (dtxProtocolCommand)
	{
	case DTX_PROTOCOL_COMMAND_ABORT:
		elog(LOG, "performDtxProtocolCommand going to call AbortOutOfAnyTransaction for distributed transaction %s", gid);
		AbortOutOfAnyTransaction();
		break;

	case DTX_PROTOCOL_COMMAND_READ_ONLY_COMMIT:
	case DTX_PROTOCOL_COMMAND_PREPARE:
		/*
		 * The QD has directed us to read-only commit or prepare an implicit or explict
		 * distributed transaction.
		 */
		switch (DistributedTransactionContext)
		{
		case DTX_CONTEXT_LOCAL_ONLY:
			/*
			 * Spontaneously aborted while we were back at the QD?
			 */
			elog(ERROR, "Distributed transaction %s not found", gid);
			break;
			
		case DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER:
		case DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER:
			StartTransactionCommand();

			if (dtxProtocolCommand == DTX_PROTOCOL_COMMAND_PREPARE)
			{
				elog(LOG, "performDtxProtocolCommand going to call PrepareTransactionBlock for distributed transaction %s", gid);
				if (!PrepareTransactionBlock(gid))
				{
					elog(ERROR, "Prepare of distributed transaction %s failed", gid);
					break;
				}
			}
			else
			{
				elog(LOG, "performDtxProtocolCommand going to call EndTransactionBlock for distributed transaction %s", gid);
				if (!EndTransactionBlock())
				{
					elog(ERROR, "Read only commit of distributed transaction %s failed", gid);
					break;
				}
			}

			/*
			 * Calling CommitTransactionCommand will cause the actual 
			 * COMMIT/PREPARE work to be performed.
			 */
			CommitTransactionCommand();

			if (dtxProtocolCommand == DTX_PROTOCOL_COMMAND_PREPARE)
			{
				elog(LOG, "Prepare of distributed transaction %s succeeded", gid);
				DistributedTransactionContext = DTX_CONTEXT_QE_PREPARED;
			}
			else
			{
				elog(LOG, "Read only commit of distributed transaction %s succeeded", gid);
				finishDistributedTransactionContext();
			}
			break;
			
		case DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE:
		case DTX_CONTEXT_QE_PREPARED:
		case DTX_CONTEXT_QE_ENTRY_DB_SINGLETON:
		case DTX_CONTEXT_QE_READER:
			elog(FATAL, "Unexpected QE distribute transaction context: '%s'",
				DtxContextToString(DistributedTransactionContext));

		default:
			elog(FATAL, "Unexpected QE distribute transaction context value: %d",
				(int) DistributedTransactionContext);
			break;
		}
		break;

	case DTX_PROTOCOL_COMMAND_COMMIT_PREPARED:
		if (DistributedTransactionContext != DTX_CONTEXT_QE_PREPARED)
		{
			elog(FATAL, "Expected QE distribute transaction context to be '%s', found '%s'",
				DtxContextToString(DTX_CONTEXT_QE_PREPARED),
				DtxContextToString(DistributedTransactionContext));
		}

		elog(LOG, "performDtxProtocolCommand going to call FinishPreparedTransaction for distributed transaction %s", gid);
		FinishPreparedTransaction(gid, /* isCommit */ true, /* raiseErrorIfNotFound */ true);
		finishDistributedTransactionContext();
		break;

	case DTX_PROTOCOL_COMMAND_RETRY_COMMIT_PREPARED:
		/*
		 * We can be doing this on a different connection, so ignore DistributedTransactionContext.
		 */
		elog(LOG, "performDtxProtocolCommand going to call FinishPreparedTransaction for distributed transaction %s", gid);
		FinishPreparedTransaction(gid, /* isCommit */ true, /* raiseErrorIfNotFound */ false);
		finishDistributedTransactionContext();
		break;

	case DTX_PROTOCOL_COMMAND_ABORT_PREPARED:
		/*
		 * The QD has directed us to abort an implicit or explict
		 * distributed transaction that may or may not have been prepared.
		 */
		switch (DistributedTransactionContext)
		{
		case DTX_CONTEXT_LOCAL_ONLY:
			/*
			 * Spontaneously aborted while we were back at the QD.
			 */
			break;
			
		case DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER:
		case DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER:
			AbortOutOfAnyTransaction();

			// AbortCurrentTransaction resets DistributedTransactionContext.
			Assert(DistributedTransactionContext == DTX_CONTEXT_LOCAL_ONLY);
			break;
			
		case DTX_CONTEXT_QE_PREPARED:
			FinishPreparedTransaction(gid, /* isCommit */ false, /* raiseErrorIfNotFound */ true);
			finishDistributedTransactionContext();
			break;
			
		case DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE:
		case DTX_CONTEXT_QE_ENTRY_DB_SINGLETON:
		case DTX_CONTEXT_QE_READER:
			elog(FATAL, "Unexpected QE distribute transaction context: '%s'",
				 DtxContextToString(DistributedTransactionContext));

		default:
			elog(FATAL, "Unexpected QE distribute transaction context value: %d",
				(int) DistributedTransactionContext);
			break;
		}
		break;

	case DTX_PROTOCOL_COMMAND_RETRY_ABORT_PREPARED:
		/*
		 * We can be doing this on a different connection, so ignore DistributedTransactionContext
		 * and try both possibilities.
		 */
		elog(LOG, "performDtxProtocolCommand going to call AbortOutOfAnyTransaction for distributed transaction %s", gid);
		AbortOutOfAnyTransaction();
		elog(LOG, "performDtxProtocolCommand going to call FinishPreparedTransaction for distributed transaction %s", gid);
		FinishPreparedTransaction(gid, /* isCommit */ false, /* raiseErrorIfNotFound */ false);
		finishDistributedTransactionContext();
		break;

	case DTX_PROTOCOL_COMMAND_RECOVERY_ABORT_PREPARED:
		Assert(DistributedTransactionContext == DTX_CONTEXT_LOCAL_ONLY);
		elog(LOG, "performDtxProtocolCommand going to call FinishPreparedTransaction for distributed transaction %s", gid);
		FinishPreparedTransaction(gid, /* isCommit */ false, /* raiseErrorIfNotFound */ false);
		break;

	default:
		elog(ERROR, "Unrecognized dtx protocol command: %d",
			(int) dtxProtocolCommand);
		break;
	}
	elog(LOG, "performDtxProtocolCommand successful return for distributed transaction %s", gid);
}


