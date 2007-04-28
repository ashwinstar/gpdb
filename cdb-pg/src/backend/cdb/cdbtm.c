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
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

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

/*
 * Directory where Utility Mode DTM REDO file reside within PGDATA
 */
#define UTILITYMODEDTMREDO_DIR "pg_utilitymodedtmredo"

/*
 * File name for Utility Mode DTM REDO
 */
#define UTILITYMODEDTMREDO_FILE "savedtmredo.file"


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

static bool resolveDtx = false;

static int	redoFileFD = -1;
static int  redoFileOffset;

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
#define GP_OPT_NEED_TWO_PHASE                           0x0001

#define GP_OPT_READ_COMMITTED    						0x0002
#define GP_OPT_READ_UNCOMMITTED  						0x0004
#define GP_OPT_REPEATABLE_READ   						0x0006
#define GP_OPT_SERIALIZABLE 	  						0x0008

#define GP_OPT_READ_ONLY         						0x0010

#define GP_OPT_EXPLICT_BEGIN      						0x0020

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
				int flags,
				char* gid, DistributedTransactionId gxid,
				bool dispatchToMirrors, bool raiseError);
static void doCommitAllReadOnly(void);
static void doPrepareTransaction(void);
static void doInsertForgetCommitted(void);
static void doNotifyingCommitted(void);
static void doNotifyingAborted(bool noPreparesIssued);
static bool doNotifyCommittedInDoubt(char *gid);
static void doAbortInDoubt(char *gid);
static void doQEAddLocalXidsForDistributedIndoubt();
static void doQEDistributedExplicitBegin(int txnOptions);

static void resetDtxContextInfo(DtxContextInfo *dtxContextInfo);
static bool isDtxQueryDispatcher(void);
static void PleaseDebugMe(char *caller);
static void ReplayRedoFromUtilityMode(void);

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
		
	case DTX_STATE_INSERTING_FORGET_COMMITTED:
		return "Inserting Forget Committed";
		
	case DTX_STATE_INSERTED_FORGET_COMMITTED:
		return "Inserted Forget Committed";
		
	case DTX_STATE_NOTIFYING_ABORTED:
		return "Notifying Aborted";
		
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

	case DTX_PROTOCOL_COMMAND_RECOVERY_COMMIT_PREPARED:
		return "Recovery Commit Prepared";
		
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
 * Called by prepareDtxTransaction.
 */
static void
summarizeParticipation(void)
{
	Assert(currentGxact != NULL);
	Assert(currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED);

	if (currentGxact->participationSummary != DTX_PARTICIPATION_SUMMARY_NONE)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"summarizeParticipation already called???");
		return;
	}

	// FUTURE: Collect information on whether particular segments were dirty
	currentGxact->participationSummary = DTX_PARTICIPATION_SUMMARY_MULTIPLE_SEGMENTS_DIRTY;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"summarizeParticipation participationSummary = \"%s\"",
		 DtxParticipationSummaryToString(currentGxact->participationSummary));
	
}

bool
isPreparedDtxTransaction(void)
{
	if (Gp_role != GP_ROLE_DISPATCH ||
		DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE ||
		currentGxact == NULL)
		return false;

	return (currentGxact->state == DTX_STATE_PREPARED);
}

void
getDtxLogInfo(TMGXACT_LOG *gxact_log)
{
	if (currentGxact == NULL)
	{
		elog(FATAL, "getDtxLogInfo found current distributed transaction is NULL");
	}

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
	    elog((Debug_print_full_dtm ? LOG : DEBUG5),"notifyCommittedDtxTransaction nothing to do (DistributedTransactionContext = '%s')", 
			 DtxContextToString(DistributedTransactionContext));
		return;
	}

	if(currentGxact == NULL)
	{
	    elog((Debug_print_full_dtm ? LOG : DEBUG5),"notifyCommittedDtxTransaction nothing to do (currentGxact == NULL)");
		return;
	}

	doNotifyingCommitted();
	return;
}

static void
PleaseDebugMe(char *caller)
{
	int i;
	
	for (i = 0; i < 300;i++)
	{
		elog(LOG,"%s  --> Now would be a good time to debug pid = %d",caller, MyProcPid);
		elog(NOTICE,"%s  --> Now would be a good time to debug pid = %d",caller, MyProcPid);
		pg_usleep(1000000L);
	}
}

/* 
 * Marks the current Distributed Transaction as needing to use the
 * 2 phase commit protocol.
 */
void
setCurrentDtxTwoPhase(const char* debugCaller, const char* debugDetail)
{
	Assert(debugCaller != NULL);
	Assert(debugDetail != NULL);

	if (currentGxact != NULL)
	{
		if (currentGxact->state == DTX_STATE_ACTIVE_NOT_DISTRIBUTED)
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"setCurrentDtxTwoPhase going distributed for gid = %s (%s, detail = '%s')", 
			     currentGxact->gid, debugCaller, debugDetail);
			currentGxact->state = DTX_STATE_ACTIVE_DISTRIBUTED;
		}
	 	else if (currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED)
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"setCurrentDtxTwoPhase gid = %s is already distributed (%s, detail = '%s')", 
			     currentGxact->gid, debugCaller, debugDetail);
		}
		else
		{
			elog(ERROR,"DTM transaction is not active (state = %s, %s, detail = '%s')", 
				DtxStateToString(currentGxact->state), debugCaller, debugDetail);
		}
	}
	else
	{
		elog(ERROR,"DTM transaction is not active (%s, detail = '%s')", debugCaller, debugDetail);
	}
}

/*
 * The executor can avoid starting a distributed transaction if it knows that
 * the current dtx is clean and we aren't in a user-started global transaction.
 */
bool
isCurrentDtxTwoPhase(void)
{
	if (currentGxact == NULL)
	{
		return false;
	}
	else
	{
		return currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED;
	}
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
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doCommitAllReadOnly entering in state = %s", 
		 DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED);
	currentGxact->state = DTX_STATE_COMMITTING_ALL_READ_ONLY;
	releaseTmLock();

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doCommitAllReadOnly moved to state = %s", 
		 DtxStateToString(currentGxact->state));

	/*
	 * Do Read-Only COMMIT broadcast.
	 */
	// UNDONE: What about dispatchToMirrors parameter?
	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_READ_ONLY_COMMIT, /* flags */ 0,
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ false, /* raiseError */ false);
	if (!succeeded)
	{
		elog(ERROR, "The distributed transaction 'Read-Only Commit' broadcast to one or more segments failed for gid = %s.",
			 currentGxact->gid);
	}
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "The distributed transaction 'Read-Only Commit' broadcast to the segments succeeded for gid = %s.",
		 currentGxact->gid);
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_COMMITTING_ALL_READ_ONLY);
	currentGxact->state = DTX_STATE_COMMITTED_ALL_READ_ONLY;
	releaseGxact();
	releaseTmLock();

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doCommitAllReadOnly called releaseGxact");

}

static void
doPrepareTransaction(void)
{
	bool succeeded;
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doPrepareTransaction entering in state = %s", 
	     DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_ACTIVE_DISTRIBUTED);
	currentGxact->state = DTX_STATE_PREPARING;
	releaseTmLock();

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doPrepareTransaction moved to state = %s", DtxStateToString(currentGxact->state));

	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_PREPARE, /* flags */ 0, 
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(ERROR, "The distributed transaction 'Prepare' broadcast to one or more segments failed for gid = %s.",
			 currentGxact->gid);
	}
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "The distributed transaction 'Prepare' broadcast to the segments succeeded for gid = %s.",
		 currentGxact->gid);
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_PREPARING);
	currentGxact->state = DTX_STATE_PREPARED;
	releaseTmLock();
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doPrepareTransaction leaving in state = %s", DtxStateToString(currentGxact->state));
}

/*
 * Insert FORGET COMMITTED into the xlog.
 * Call with DTM lock held.
 */
static void
doInsertForgetCommitted(void)
{
	TMGXACT_LOG gxact_log;
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doInsertForgetCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	currentGxact->state = DTX_STATE_INSERTING_FORGET_COMMITTED;

	memcpy(&gxact_log.gid, currentGxact->gid, TMGIDSIZE);
	gxact_log.gxid = currentGxact->gxid;
	
	RecordDistributedForgetCommitted(&gxact_log);
	
	currentGxact->state = DTX_STATE_INSERTED_FORGET_COMMITTED;

	releaseGxact();
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doInsertForgetCommitted called releaseGxact");
}

static void
doNotifyingCommitted(void)
{
	bool succeeded;
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doNotifyingCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_FORCED_COMMITTED);
	currentGxact->state = DTX_STATE_NOTIFYING_COMMITTED;
	releaseTmLock();

	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_COMMIT_PREPARED, /* flags */ 0,  
					currentGxact->gid, currentGxact->gxid,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(WARNING, "The distributed transaction 'Commit Notification' broadcast to one or more segments failed for gid = %s.",
			 currentGxact->gid);
	}
	else
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "The distributed transaction 'Commit Notification' broadcast to the segments succeeded for gid = %s.",
			 currentGxact->gid);
	}

	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFYING_COMMITTED);

	doInsertForgetCommitted();
	
	releaseTmLock();
}

static void
doNotifyingAborted(bool noPreparesIssued)
{
	bool succeeded;
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doNotifyingAborted entering in state = %s", DtxStateToString(currentGxact->state));
	
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFYING_ABORTED);
	releaseTmLock();

	if (noPreparesIssued)
	{
		succeeded = doDispatchDtxProtocolCommand(
						DTX_PROTOCOL_COMMAND_ABORT, /* flags */ 0,  
						currentGxact->gid, currentGxact->gxid,
						/* dispatchToMirrors */ true, /* raiseError */ false);
		if (!succeeded)
		{
			elog(WARNING, "The distributed transaction 'Abort' broadcast to one or more segments failed for gid = %s.",
				 currentGxact->gid);
		}
		else
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5), "The distributed transaction 'Abort' broadcast to the segments succeeded for gid = %s.",
				 currentGxact->gid);
		}
	}
	else
	{
		succeeded = doDispatchDtxProtocolCommand(
						DTX_PROTOCOL_COMMAND_ABORT_PREPARED, /* flags */ 0,
						currentGxact->gid, currentGxact->gxid,
						/* dispatchToMirrors */ true, /* raiseError */ false);
		if (!succeeded)
		{
			elog(WARNING, "The distributed transaction 'Abort' prepared broadcast to one or more segments failed for gid = %s.",
				 currentGxact->gid);
		}
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "The distributed transaction 'Abort Prepared' broadcast to the segments succeeded for gid = %s.",
			 currentGxact->gid);
	}
		
	getTmLock();
	Assert(currentGxact->state == DTX_STATE_NOTIFYING_ABORTED);

	releaseGxact();
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"doNotifyingAborted called releaseGxact");
	
	releaseTmLock();
}

static bool
doNotifyCommittedInDoubt(char *gid)
{
	bool succeeded;

	// UNDONE: Pass real gxid instead of InvalidDistributedTransactionId.
	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_RECOVERY_COMMIT_PREPARED, /* flags */ 0,
					gid, InvalidDistributedTransactionId,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(WARNING, "Crash recovery retry of the distributed transaction 'Commit Prepared' broadcast for gid = %s failed to one or more segments.  System will retry again later", 
			 gid);
	}
	else
	{
		elog(INFO, "Crash recovery retry of the distributed transaction 'Commit Prepared' broadcast for gid = %s succeeded", 
			 gid);
	}

	return succeeded;
}

static void
doAbortInDoubt(char *gid)
{
	bool succeeded;

	// UNDONE: Pass real gxid instead of InvalidDistributedTransactionId.
	succeeded = doDispatchDtxProtocolCommand(
					DTX_PROTOCOL_COMMAND_RECOVERY_ABORT_PREPARED, /* flags */ 0,
					gid, InvalidDistributedTransactionId,
					/* dispatchToMirrors */ true, /* raiseError */ false);
	if (!succeeded)
	{
		elog(WARNING, "Crash recovery retry of the distributed transaction 'Abort Prepared' broadcast for gid = %s failed to one or more segments.  System will retry again later", 
			 gid);
	}
	else
	{
		elog(INFO, "Crash recovery retry of the distributed transaction 'Abort Prepared' broadcast for gid = %s succeeded", 
			 gid);
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
	    elog((Debug_print_full_dtm ? LOG : DEBUG5),"prepareDtxTransaction nothing to do (DistributedTransactionContext = '%s')", 
			 DtxContextToString(DistributedTransactionContext));
		return;
	}
	
	if (currentGxact == NULL)
	{
		return;
	}
	
	if (currentGxact->state == DTX_STATE_ACTIVE_NOT_DISTRIBUTED)
	{
		/*
		 * This transaction did not go distributed.
		 */
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"prepareDtxTransaction ignoring not distributed gid = %s", currentGxact->gid);
		releaseGxact();
		return;
	}
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"prepareDtxTransaction called with state = %s",
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
 * rollback a global transaction, called by user rollback
 * or by AbortTransaction during Postgres automatic rollback
 */
void
rollbackDtxTransaction(void)
{
	bool noPreparesIssued = false;
	
	if(DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE)
	{
	    elog((Debug_print_full_dtm ? LOG : DEBUG5),"rollbackDtxTransaction nothing to do (DistributedTransactionContext = '%s')", 
			 DtxContextToString(DistributedTransactionContext));
		return;
	}
	if(currentGxact == NULL)
	{
	    elog((Debug_print_full_dtm ? LOG : DEBUG5),"rollbackDtxTransaction nothing to do (currentGxact == NULL)");
		return;
	}

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"rollbackDtxTransaction called with state = %s, gid = %s",
		 DtxStateToString(currentGxact->state), currentGxact->gid);
	
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
		
	case DTX_STATE_FORCED_COMMITTED:
	case DTX_STATE_NOTIFYING_COMMITTED:
	case DTX_STATE_NOTIFYING_ABORTED:
	case DTX_STATE_INSERTING_COMMITTED:
	case DTX_STATE_INSERTED_COMMITTED:
	case DTX_STATE_INSERTING_FORGET_COMMITTED:
	case DTX_STATE_INSERTED_FORGET_COMMITTED:
	case DTX_STATE_CRASH_COMMITTED:
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "rollbackDtxTransaction dtx state \"%s\" not expected here",
			 DtxStateToString(currentGxact->state));
		releaseGxact();
		return;
		
	default:
		elog(FATAL, "Unrecognized dtx state: %d",
			(int) currentGxact->state);
		break;
	}


	Assert(currentGxact->state == DTX_STATE_NOTIFYING_ABORTED);
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
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"rollbackDtxTransaction has proc_exit_inprogress = true with state = %s",
			 DtxStateToString(currentGxact->state));

		releaseGxact();
		return;
	}

	doNotifyingAborted(noPreparesIssued);

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
	if ((Gp_role != GP_ROLE_DISPATCH) && (Gp_role != GP_ROLE_UTILITY))
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

	if ((Gp_role != GP_ROLE_DISPATCH) && (Gp_role != GP_ROLE_UTILITY))
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
mppTxnOptions(bool needTwoPhase)
{
	int options = 0;

	if (needTwoPhase)
		options |= GP_OPT_NEED_TWO_PHASE;

	if (XactIsoLevel == XACT_READ_COMMITTED)
		options |= GP_OPT_READ_COMMITTED;
	else if (XactIsoLevel == GP_OPT_REPEATABLE_READ)
		options |= GP_OPT_REPEATABLE_READ;
	else if (XactIsoLevel == GP_OPT_SERIALIZABLE)
		options |= GP_OPT_SERIALIZABLE;

	if (XactReadOnly)
		options |= GP_OPT_READ_ONLY;
		
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
 * Return the NeedTwoPhase flag.
 */
bool
isMppTxOptions_NeedTwoPhase(int txnOptions)
{
	return ((txnOptions & GP_OPT_NEED_TWO_PHASE) != 0);
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

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "redoDtxCheckPoint has size %d", dtxCheckpointBufferSize);
	gxact_checkpoint = (TMGXACT_CHECKPOINT *)dtxCheckpointBuffer;
	
	committedCount = gxact_checkpoint->committedCount;
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "redoDtxCheckPoint has committedCount = %d",
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

	if (Gp_role == GP_ROLE_UTILITY)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG3), "DB in Utility mode.  Defer DTM recovery till later.");
		return;
	}
	
	gxact_checkpoint = (TMGXACT_CHECKPOINT *)dtxCheckpointBuffer;
	
	committedCount = gxact_checkpoint->committedCount;
	segmentCount = gxact_checkpoint->segmentCount;
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "redoDtxCheckPoint has size %d, committedCount = %d, segmentCount = %d",
		 dtxCheckpointBufferSize, committedCount, segmentCount);
	
	for (i = 0; i < committedCount; i++)
	{
		redoDistributedCommitRecord(&gxact_checkpoint->committedGxactArray[i]);
	}
}

static void
GetRedoFileName(char *path)
{
	snprintf(path, MAXPGPATH, 
		     "%s/" UTILITYMODEDTMREDO_DIR "/" UTILITYMODEDTMREDO_FILE, DataDir);
	elog((Debug_print_full_dtm ? LOG : DEBUG3),"Returning save DTM redo file path = %s",path);
}

void
UtilityModeFindOrCreateDtmRedoFile(void)
{
	char		path[MAXPGPATH];

	if (Gp_role != GP_ROLE_UTILITY)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG3),"Not in Utility Mode (role = %s) -- skipping finding or creating DTM redo file",
			 role_to_string(Gp_role));
		return;
	}
	GetRedoFileName(path);
	
	redoFileFD = open(path, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (redoFileFD < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create save DTM redo file \"%s\"",
						path)));
	}

	redoFileOffset = lseek(redoFileFD, 0, SEEK_END);
	elog((Debug_print_full_dtm ? LOG : DEBUG3),"Succesfully opened DTM redo file %s (end offset %d)",
		 path, redoFileOffset);
}

/*
 *
 */
void
UtilityModeSaveRedo(bool committed, TMGXACT_LOG * gxact_log)
{
	TMGXACT_UTILITY_MODE_REDO utilityModeRedo;
	int write_len;

	utilityModeRedo.committed = committed;
	memcpy(&utilityModeRedo.gxact_log, gxact_log, sizeof(TMGXACT_LOG));

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"Writing {committed = %s, gid = %s, gxid = %u} to DTM redo file",
	     (utilityModeRedo.committed ? "true" : "false"),
	     utilityModeRedo.gxact_log.gid,
	     utilityModeRedo.gxact_log.gxid);

	write_len = write(redoFileFD, &utilityModeRedo, sizeof(TMGXACT_UTILITY_MODE_REDO));
	if (write_len != sizeof(TMGXACT_UTILITY_MODE_REDO))
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write save DTM redo file : %m")));
	}

}

void
UtilityModeCloseDtmRedoFile(void)
{
	if (Gp_role != GP_ROLE_UTILITY)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG3),"Not in Utility Mode (role = %s)-- skipping closing DTM redo file",
			 role_to_string(Gp_role));
		return;
	}
	elog((Debug_print_full_dtm ? LOG : DEBUG3),"Closing DTM redo file");
	close(redoFileFD);
}

static void
ReplayRedoFromUtilityMode(void)
{
	TMGXACT_UTILITY_MODE_REDO utilityModeRedo;

	int			fd;
	int			read_len;
	int	        errno;
	char		path[MAXPGPATH];
	int			entries;

	entries = 0;

	GetRedoFileName(path);

	fd = open(path, O_RDONLY, 0);
	if (fd < 0)
	{
		// UNDONE: Distinquish "not found" from other errors.
		elog((Debug_print_full_dtm ? LOG : DEBUG3),"Could not open DTM redo file %s for reading",
		     path);
		return;
	}

	elog((Debug_print_full_dtm ? LOG : DEBUG3),"Succesfully opened DTM redo file %s for reading",
		 path);
	
	while(true)
	{
		errno = 0;
		read_len = read(fd, &utilityModeRedo, sizeof(TMGXACT_UTILITY_MODE_REDO));

		if (read_len == 0)
			break;
		else if (read_len != sizeof(TMGXACT_UTILITY_MODE_REDO) && errno == 0)
			elog(ERROR,"Bad redo length (expected %d and found %d)",
			     (int)sizeof(TMGXACT_UTILITY_MODE_REDO), read_len);
		else if (errno != 0)
		{
			close(fd);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("error reading DTM redo file: %m")));
		}

		elog((Debug_print_full_dtm ? LOG : DEBUG5),"Read {committed = %s, gid = %s, gxid = %u} from DTM redo file",
		     (utilityModeRedo.committed ? "true" : "false"),
		     utilityModeRedo.gxact_log.gid,
		     utilityModeRedo.gxact_log.gxid);
		if (utilityModeRedo.committed)
		{
			redoDistributedCommitRecord(&utilityModeRedo.gxact_log);
		}
		else
		{
			redoDistributedForgetCommitRecord(&utilityModeRedo.gxact_log);
		}

		entries++;
	}

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"Processed %d entries from DTM redo file",
		 entries);
	close(fd);

}

void
RemoveRedoUtilityModeFile(void)
{
	char		path[MAXPGPATH];
	bool        removed;

	GetRedoFileName(path);
	removed = (unlink(path) == 0);
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"Removed DTM redo file %s (%s)",
		 path, (removed ? "true" : "false"));
}

/*
 * Redo transaction commit log record.
 */
void
redoDistributedCommitRecord(TMGXACT_LOG * gxact_log)
{

	int			i;

	if (Gp_role == GP_ROLE_UTILITY)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG3), "DB in Utility mode.  Save DTM disributed commit until later.");
		UtilityModeSaveRedo(true, gxact_log);
		return;
	}
	
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

	elog((Debug_print_full_dtm ? INFO : DEBUG5), "Crash recovery redo added committed distributed transaction gid = %s",currentGxact->gid);

}

/*
 * Redo transaction forget commit log record.
 */
void
redoDistributedForgetCommitRecord(TMGXACT_LOG * gxact_log)
{
	int			i;

	if (Gp_role == GP_ROLE_UTILITY)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG3), "DB in Utility mode.  Save DTM disributed forget until later.");
		UtilityModeSaveRedo(false, gxact_log);
		return;
	}
	
	for (i = 0; i < *shmNumGxacts; i++)
	{
		if (strcmp(gxact_log->gid, shmGxactArray[i]->gid) == 0)
		{
			/* found an active global transaction */
			currentGxact = shmGxactArray[i];
			elog((Debug_print_full_dtm ? INFO : DEBUG5), "Crash recovery redo removed committed distributed transaction gid = %s for forget",
			     currentGxact->gid);
			releaseGxact();
			return;
		}
	}

	elog((Debug_print_full_dtm ? WARNING : DEBUG5), "Crash recovery redo did not find committed distributed transaction gid = %s for forget",
		 gxact_log->gid);

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
			int flags,
			char* gid, DistributedTransactionId gxid,
			bool dispatchToMirrors, bool raiseError)
{
	int		i, resultCount, numOfFailed = 0;

	char *dtxProtocolCommandStr;

	struct pg_result **results = NULL;
	StringInfoData errbuf;

	dtxProtocolCommandStr = DtxProtocolCommandToString(dtxProtocolCommand);

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "dispatchDtxProtocolCommand: %d ('%s') willMirror: %s", dtxProtocolCommand, dtxProtocolCommandStr, dispatchToMirrors ? "yes" : "no");

	initStringInfo(&errbuf);
	results = cdbdisp_dispatchDtxProtocolCommand(dtxProtocolCommand,
										flags,
										dtxProtocolCommandStr,
										gid,
										gxid,
										dispatchToMirrors,
										&errbuf, &resultCount);

	if (errbuf.len > 0)
	{
		ereport((raiseError ? ERROR : LOG), (errmsg("DTM error (gathered %d results from cmd '%s')", resultCount, dtxProtocolCommandStr),
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

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "dispatchDtxCommand: '%s' willMirror: %s", cmd, dispatchToMirrors ? "yes" : "no");

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
	
    gxact->xminDistributedSnapshot = 0;

	gxact->participationSummary = DTX_PARTICIPATION_SUMMARY_NONE;
}

// UNDONE: Call this from where regular snapshots get created...
bool
createDtxSnapshot(DistributedSnapshot distributedSnapshot)
{
	int 						count;
	int                         i;
	TMGXACT    			*gxact_candidate;
	DtxState      		state;
	DistributedTransactionId 	indoubtxid;

	if(currentGxact == NULL)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"createDtxSnapshot found currentGxact is NULL");
		return false;
	}

	getTmLock();

	/*
	 * Gather up current in-progress global transactions for the
	 * distributed snapshot.
	 */
	count = *shmNumGxacts;
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
		case DTX_STATE_NOTIFYING_ABORTED:
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

		// UNDONE: Temporary hard coded limit
		if(distributedSnapshot->xcnt >= MaxGpDistributedInProgress)
			elog(ERROR,"Too many distributed transactions for snapshot");

		distributedSnapshot->xip[distributedSnapshot->xcnt++] = indoubtxid;
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"createDtxSnapshot added gxid = %u to distributed snapshot", indoubtxid);
	}
	
	if (distributedSnapshot->xmin == UINT_MAX)
	{
		distributedSnapshot->xmin = InvalidDistributedTransactionId;
	}
	else if (distributedSnapshot->xmin < currentGxact->xminDistributedSnapshot)
	{
		currentGxact->xminDistributedSnapshot = distributedSnapshot->xmin;

		// UNDONE: Update global xmin???
	}

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"createDtxSnapshot distributed snapshot has xmin = %u, xcnt = %u, xmax = %u.", 
		 distributedSnapshot->xmin, distributedSnapshot->xcnt, distributedSnapshot->xmax);


	releaseTmLock();

	return true;
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

	/*
	 * Until we get our first distributed snapshot, we use our
	 * distributed transaction identifier for the minimum.
	 */
	gxact->xminDistributedSnapshot = gxact->gxid;

	Assert(gxact->participationSummary == DTX_PARTICIPATION_SUMMARY_NONE);
	
	gxact->state = DTX_STATE_ACTIVE_NOT_DISTRIBUTED;
	
	releaseTmLock();

	currentGxact = gxact;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"createDtx created new distributed transaction gid = %s, gxid = %u.", 
		 currentGxact->gid, currentGxact->gxid);


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
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "releaseGxact called for gid = %s (index = %d)", currentGxact->gid, currentGxact->debugIndex);
	
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
			if (shmGxactArray[i]->xminDistributedSnapshot < *shmXminAllDistributedSnapshots)
			{
				*shmXminAllDistributedSnapshots = shmGxactArray[i]->xminDistributedSnapshot;
			}
		}
	}

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"releaseGxact re-calculated all distributed snapshots xmin = %u for %d global transactions.",
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
}

/*
 * Get lock that serializes commits with DTM checkpoint info.
 * Change state to DTX_STATE_INSERTING_COMMITTED.
 */
void
insertingDistributedCommitted(void)
{
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"insertingDistributedCommitted entering in state = %s", 
	     DtxStateToString(currentGxact->state));
	
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
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"insertedDistributedCommitted entering in state = %s", 
	     DtxStateToString(currentGxact->state));
	
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
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"forcedDistributedCommitted entering in state = %s", DtxStateToString(currentGxact->state));
	
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
		case DTX_STATE_CRASH_COMMITTED:
			break;
						
		case DTX_STATE_NOTIFYING_ABORTED:
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

	elog((Debug_print_full_dtm ? LOG : DEBUG3), "Starting to Recover DTM...");

	/*
	 * do not do recovery if read only mode is set.
	 * in this case, there may be in-doubt transaction in down segdb
	 * , which will not be resolved at this time.
	 */
	if (isFtsReadOnlySet())
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG3), "FTS is Read Only.  Defer DTM recovery till later.");
		if (currentGxact != NULL)
			elog((Debug_print_full_dtm ? LOG : DEBUG5), "recoverTM setting currentGxact to NULL for gid = %s (index = %d)", currentGxact->gid, currentGxact->debugIndex);
		currentGxact = NULL;
		return;
	}

	if (Gp_role == GP_ROLE_UTILITY)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG3), "DB in Utility mode.  Defer DTM recovery till later.");
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

	elog((Debug_print_full_dtm ? LOG : DEBUG3),"recover in-doubt distributed transactions");

	ReplayRedoFromUtilityMode();
	
	/*
	 * For each committed transaction found in the redo pass that was not
	 * matched by a forget committed record, change its state indicating
	 * committed notification needed.  Attempt a notification.
	 */

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"Going to retry commit notification for distributed transactions (count = %d)",
	     *shmNumGxacts);
	dumpAllDtx();
	
	while (*shmNumGxacts > 0)
	{
		TMGXACT *gxact = shmGxactArray[0];

		if (gxact->state != DTX_STATE_CRASH_COMMITTED)
		{
			dumpAllDtx();
			elog(PANIC, "recoverInDoubtTransactions found transaction in '%s' state and was expecting it to be in '%s' state",
				 DtxStateToString(gxact->state), DtxStateToString(DTX_STATE_CRASH_COMMITTED));
		}

		elog((Debug_print_full_dtm ? LOG : DEBUG5),"Rcovering committed distributed transaction gid = %s",
		     gxact->gid);

		/*
		 * What if we encounter an error here???
		 */
		doNotifyCommittedInDoubt(gxact->gid);

		currentGxact = gxact;
		doInsertForgetCommitted();
	}

	dumpAllDtx();

	// UNDONE: Thus, any in-doubt transctions found will be for aborted transactions.
	// UNDONE: Gather in-boubt transactions and issue aborts.
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

	RemoveRedoUtilityModeFile();

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
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"Skipping in-doubt transaction gid = %s since it is \"%s\"",
				 gxact->gid, DtxStateToString(gxact->state));
			continue;
		}
	
		sprintf(cmdbuf, "ROLLBACK PREPARED '%s'", entry->gid);
		cmd = cmdbuf;

		elog((Debug_print_full_dtm ? LOG : DEBUG3), "Aborting in-doubt transaction with gid = %s", entry->gid);
		
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
qdCreateDtxContextInfo(DtxContextInfo *dtxContextInfo, DistributedSnapshot distributedSnapshot, int txnOptions)
{
	int i;
	
	resetDtxContextInfo(dtxContextInfo);

	if (distributedSnapshot != NULL)
	{
		if (currentGxact != NULL)
		{
			dtxContextInfo->distributedXid = currentGxact->gxid;

			if (dtxContextInfo->distributedXid != InvalidDistributedTransactionId)
			{
				getTmLock();
				dtxContextInfo->xminAllDistributedSnapshots = 
						(*shmXminAllDistributedSnapshots == UINT_MAX ? 0 : *shmXminAllDistributedSnapshots);
				releaseTmLock();

				memcpy(&dtxContextInfo->distributedSnapshotData,
					   distributedSnapshot,
					   sizeof(DistributedSnapshotData));

				elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdCreateDtxContextInfo Gp_role is DISPATCH and have currentGxact = %s, gxid = %u --> have distributed snapshot",
				 	currentGxact->gid, currentGxact->gxid);
				elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdCreateDtxContextInfo distributedXid = %u, "
				     "xminAllDistributedSnapshots = %u, "
				     "distributedSnapshotData (xmin = %u, xmax = %u, xcnt = %u)",
					dtxContextInfo->distributedXid, 
					dtxContextInfo->xminAllDistributedSnapshots, 
					dtxContextInfo->distributedSnapshotData.xmin, 
					dtxContextInfo->distributedSnapshotData.xmax,
					dtxContextInfo->distributedSnapshotData.xcnt);
				for (i = 0; i < dtxContextInfo->distributedSnapshotData.xcnt; i++)
				{
					elog((Debug_print_full_dtm ? LOG : DEBUG5),"....    distributedSnapshotData->xip[%d] = %u", 
					     i, dtxContextInfo->distributedSnapshotData.xip[i]);
				}
				elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdCreateDtxContextInfo curcid = %u", dtxContextInfo->distributedSnapshotData.curcid);
				
			}
			else
			{
				elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdCreateDtxContextInfo Gp_role is DISPATCH and currentGxact is non-NULL, but gxid is InvalidDistributedTransactionId");
			}
		}
		else
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdCreateDtxContextInfo Gp_role is DISPATCH but has distributed snapshot but no currentGxact..."); 
		}
	}
	else
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdCreateDtxContextInfo Gp_role is DISPATCH but no distributed snapshot"); 
	}

	dtxContextInfo->distributedTxnOptions = txnOptions;
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"qdCreateDtxContextInfo txnOptions = 0x%x", txnOptions); 
}

int 
dtxContextInfoSerializeSize(DtxContextInfo *dtxContextInfo)
{
	int size = 0;

	size += sizeof(DistributedTransactionId);

	if (dtxContextInfo->distributedXid != InvalidDistributedTransactionId)
	{
		size += sizeof(DistributedTransactionId) +	  // xminAllDistributedSnapshots
		        sizeof(CommandId) +					  // curcid
	            2 * sizeof(DistributedTransactionId); // xmin, max

		size += sizeof(uint32) + 					  // xcnt
			    sizeof(DistributedTransactionId) *    
			        dtxContextInfo->distributedSnapshotData.xcnt;  // xip
	}

	size += sizeof(int);							  // distributedTxnOptions
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"dtxContextInfoSerializeSize is returning size = %d", size);

	return size;
}

void
serializeDtxContextInfo(char *buffer, DtxContextInfo *dtxContextInfo)
{
	char *p = buffer;
	int i;
	int used;

	memcpy(p,&dtxContextInfo->distributedXid,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
	if (dtxContextInfo->distributedXid != InvalidDistributedTransactionId)
	{
		memcpy(p,&dtxContextInfo->xminAllDistributedSnapshots,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		
		memcpy(p,&dtxContextInfo->distributedSnapshotData.xmin,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		memcpy(p,&dtxContextInfo->distributedSnapshotData.xmax,sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		memcpy(p,&dtxContextInfo->distributedSnapshotData.xcnt,sizeof(uint32)); p += sizeof(uint32);

		// UNDONE: Temporary hard coded limit
		Assert(dtxContextInfo->distributedSnapshotData.xcnt <= MaxGpDistributedInProgress);

		for (i = 0; i < dtxContextInfo->distributedSnapshotData.xcnt; i++)
		{
			memcpy(p,&dtxContextInfo->distributedSnapshotData.xip[i],sizeof(DistributedTransactionId)); p += sizeof(DistributedTransactionId);
		}

		memcpy(p,&dtxContextInfo->distributedSnapshotData.curcid, sizeof(CommandId)); p += sizeof(CommandId);
		
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"serializeDtxContextInfo distributedXid = %u, "
		     "xminAllDistributedSnapshots = %u, "
		     "distributedSnapshotData (xmin = %u, xmax = %u, xcnt = %u)",
			dtxContextInfo->distributedXid, 
			dtxContextInfo->xminAllDistributedSnapshots, 
			dtxContextInfo->distributedSnapshotData.xmin, 
			dtxContextInfo->distributedSnapshotData.xmax,
			dtxContextInfo->distributedSnapshotData.xcnt);
		for (i = 0; i < dtxContextInfo->distributedSnapshotData.xcnt; i++)
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"....    distributedSnapshotData->xip[%d] = %u", 
			     i, dtxContextInfo->distributedSnapshotData.xip[i]);
		}
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"serializeDtxContextInfo curcid = %u", dtxContextInfo->distributedSnapshotData.curcid);

	}
	else
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"serializeDtxContextInfo only copied InvalidDistributedTransactionId");
	}

	memcpy(p,&dtxContextInfo->distributedTxnOptions,sizeof(int)); p += sizeof(int);
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"serializeDtxContextInfo txnOptions = 0x%x", dtxContextInfo->distributedTxnOptions);

	used = (p - buffer);
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"serializeDtxContextInfo copied %d bytes", used);
}

static void
clearDistributedSnapshot(DistributedSnapshot distributedSnapshot)
{
	distributedSnapshot->xmin = InvalidDistributedTransactionId;		
	distributedSnapshot->xmax = InvalidDistributedTransactionId;		
	distributedSnapshot->xcnt = 0;		
	MemSet(distributedSnapshot->xip, 0, sizeof(DistributedTransactionId)*MaxGpDistributedInProgress);
	distributedSnapshot->curcid = 0;
}

static void
resetDtxContextInfo(DtxContextInfo *dtxContextInfo)
{
	dtxContextInfo->distributedXid = InvalidDistributedTransactionId;
	dtxContextInfo->xminAllDistributedSnapshots = InvalidDistributedTransactionId;
	
	clearDistributedSnapshot(&dtxContextInfo->distributedSnapshotData);

	dtxContextInfo->distributedTxnOptions = 0;
}

void
deserializeDtxContextInfo(
				const char *serializedSnapshot, int serializedSnapshotlen,
				DtxContextInfo *dtxContextInfo)
{			
	int i;

	resetDtxContextInfo(dtxContextInfo);
	
	if (serializedSnapshotlen > 0)
	{			
		int xipsize;
		const char * p = serializedSnapshot;		
						
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"deserializeDtxContextInfo serializedSnapshotlen = %d.",
		     serializedSnapshotlen);
		
		memcpy(&dtxContextInfo->distributedXid,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
		if (dtxContextInfo->distributedXid != InvalidDistributedTransactionId)
		{
			memcpy(&dtxContextInfo->xminAllDistributedSnapshots,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
			memcpy(&dtxContextInfo->distributedSnapshotData.xmin,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
			memcpy(&dtxContextInfo->distributedSnapshotData.xmax,p,sizeof(DistributedTransactionId)); 	p+=sizeof(DistributedTransactionId);		
			memcpy(&dtxContextInfo->distributedSnapshotData.xcnt,p,sizeof(uint32)); 	p+=sizeof(uint32);		

			// UNDONE: Temporary hard coded limit check
			if(dtxContextInfo->distributedSnapshotData.xcnt > MaxGpDistributedInProgress)
				elog(ERROR,"Too many distributed transactions for snapshot");

			xipsize = sizeof(DistributedTransactionId) * dtxContextInfo->distributedSnapshotData.xcnt;
			memcpy(dtxContextInfo->distributedSnapshotData.xip,p,xipsize); p += xipsize;

			memcpy(&dtxContextInfo->distributedSnapshotData.curcid,p,sizeof(CommandId)); 	p+=sizeof(CommandId);	

			elog((Debug_print_full_dtm ? LOG : DEBUG5),"deserializeDtxContextInfo distributedXid = %u, "
			     "xminAllDistributedSnapshots = %u, "
			     "distributedSnapshotData (xmin = %u, xmax = %u, xcnt = %u)",
				dtxContextInfo->distributedXid, 
				dtxContextInfo->xminAllDistributedSnapshots, 
				dtxContextInfo->distributedSnapshotData.xmin, 
				dtxContextInfo->distributedSnapshotData.xmax,
				dtxContextInfo->distributedSnapshotData.xcnt);
			for (i = 0; i < dtxContextInfo->distributedSnapshotData.xcnt; i++)
			{
				elog((Debug_print_full_dtm ? LOG : DEBUG5),"....    distributedSnapshotData->xip[%d] = %u", 
				     i, dtxContextInfo->distributedSnapshotData.xip[i]);
			}
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"....distributedCommandId = %u", dtxContextInfo->distributedSnapshotData.curcid);

		}
		else
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"deserializeDtxContextInfo distributedXid was InvalidDistributedTransactionId");
		}
		
		memcpy(&dtxContextInfo->distributedTxnOptions,p,sizeof(int)); 	p+=sizeof(int);		
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"deserializeDtxContextInfo txnOptions = 0x%x", dtxContextInfo->distributedTxnOptions);
	}
	else
	{
		Assert(dtxContextInfo->distributedXid == InvalidDistributedTransactionId);
		Assert(dtxContextInfo->distributedTxnOptions == 0);
	}

	if (dtxContextInfo->distributedXid == InvalidDistributedTransactionId)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "deserializeDtxContextInfo no distributed snapshot");
		
	}
}

static void
doQEAddLocalXidsForDistributedIndoubt()
{
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "Calling AddLocalXidsForDistributedTransactions to map distributed in-doubt XIDs to local XIDs.");
	AddLocalXidsForDistributedTransactions(
		QEDtxContextInfo.distributedSnapshotData.xip, 
		QEDtxContextInfo.distributedSnapshotData.xcnt);

	if (QEDtxContextInfo.xminAllDistributedSnapshots != InvalidDistributedTransactionId)
	{
		Assert(QEDtxContextInfo.distributedXid >= QEDtxContextInfo.xminAllDistributedSnapshots);
		
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "Calling TruncateDistributedXidMap with %u.", 
			 QEDtxContextInfo.xminAllDistributedSnapshots);
		TruncateDistributedXidMap(QEDtxContextInfo.xminAllDistributedSnapshots);
	}
	else
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "Skipping call to TruncateDistributedXidMap");
}


static void
doQEDistributedExplicitBegin(int txnOptions)
{
	int ExplicitIsoLevel;
	bool ExplicitReadOnly;
	
	/*
	 * Start a command.
	 */
	StartTransactionCommand();

	// Here is the explicit BEGIN.
	BeginTransactionBlock();

	unpackMppTxnOptions(txnOptions,
		&ExplicitIsoLevel, &ExplicitReadOnly);

	XactIsoLevel = ExplicitIsoLevel;
	XactReadOnly = ExplicitReadOnly;

	/*
	 * Finish the BEGIN command.  It will leave the explict transaction
	 * in-progress.
	 */
	CommitTransactionCommand();
	
}

static void
doQEUpdateDistributedSnapshot()
{
	doQEAddLocalXidsForDistributedIndoubt();

	// UNDONE
}

static bool 
isDtxQueryDispatcher(void)
{
	bool isDtmStarted;
	bool isSharedLocalSnapshotSlotPresent;
	
	isDtmStarted = (shmDtmStarted != NULL && *shmDtmStarted);
	isSharedLocalSnapshotSlotPresent = (SharedLocalSnapshotSlot != NULL);

 	return (Gp_role == GP_ROLE_DISPATCH &&
		    isDtmStarted &&
		    isSharedLocalSnapshotSlotPresent);
}

/*
 * Handle a request coming to the QD, or
 * a utility request coming to a QE.
 */
void
setupRegularDtxContext(void)
{
	if (DistributedTransactionContext == DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE)
	{
		// Continue in this context.  Do not touch QEDtxContextInfo, etc.
	}
	else if (DistributedTransactionContext == DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER)
	{
		// Allow this for copy...???  Do not touch QEDtxContextInfo, etc.
	}
	else if (DistributedTransactionContext != DTX_CONTEXT_LOCAL_ONLY)
	{
		elog(ERROR, "setupRegularDtxContext finds unexpected DistributedTransactionContext = '%s'",
			 DtxContextToString(DistributedTransactionContext));
	}
	else
	{
		Assert(QEDtxContextInfo.distributedXid == InvalidDistributedTransactionId);
		
		/*
		 * Determine if we are strictly local or a distributed capable QD.
		 */
		Assert(DistributedTransactionContext == DTX_CONTEXT_LOCAL_ONLY);

		if (isDtxQueryDispatcher())
		{
			DistributedTransactionContext = DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE;
		}
	}
	
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupRegularDtxContext leaving with DistributedTransactionContext = '%s'.",
		 DtxContextToString(DistributedTransactionContext));
}

void
setupQEDtxContext (DtxContextInfo *dtxContextInfo)
{
	DistributedSnapshotData *distributedSnapshotData;
	int txnOptions;
	bool needTwoPhase;
	bool explicitBegin;
	bool haveDistributedSnapshot;
	bool isEntryDbSingleton = false;
	bool isReaderQE = false;
	bool isWriterQE = false;
	bool isSharedLocalSnapshotSlotPresent;

	Assert (dtxContextInfo != NULL);

	/*
	 * DTX Context Info (even when empty) only comes in QE requests.
	 */
	distributedSnapshotData = &dtxContextInfo->distributedSnapshotData;
	txnOptions = dtxContextInfo->distributedTxnOptions;
	
	needTwoPhase = isMppTxOptions_NeedTwoPhase(txnOptions);
	explicitBegin = isMppTxOptions_ExplicitBegin(txnOptions);
	
	haveDistributedSnapshot = 
		(dtxContextInfo->distributedXid!= InvalidDistributedTransactionId);
	isSharedLocalSnapshotSlotPresent = (SharedLocalSnapshotSlot != NULL);

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext inputs (part 1): Gp_role = %s, Gp_is_writer = %s, txnOptions = 0x%x, needTwoPhase = %s, explicitBegin = %s, haveDistributedSnapshot = %s.",
		 role_to_string(Gp_role), (Gp_is_writer ? "true" : "false"), txnOptions,
		 (needTwoPhase ? "true" : "false"), (explicitBegin ? "true" : "false"), 
		 (haveDistributedSnapshot ? "true" : "false")); 
    elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext inputs (part 2): distributedXid = %u, isSharedLocalSnapshotSlotPresent = %s.",
		 dtxContextInfo->distributedXid,
	     (isSharedLocalSnapshotSlotPresent ? "true" : "false"));
	if (haveDistributedSnapshot)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext inputs (part 2a): distributedXid = %u, xminAllDistributedSnapshots = %u, distributedSnapshotData (xmin = %u, xmax = %u, xcnt = %u), distributedCommandId = %d",
	              dtxContextInfo->distributedXid, 
	              dtxContextInfo->xminAllDistributedSnapshots, 
	              distributedSnapshotData->xmin, distributedSnapshotData->xmax, distributedSnapshotData->xcnt,
	              dtxContextInfo->distributedSnapshotData.curcid);
	}
	if (isSharedLocalSnapshotSlotPresent)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext inputs (part 2b):  shared local snapshot xid = %u (xmin: %u xmax: %u xcnt: %u) curcid: %d, QDxid = %u, QDcid = %u", 
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
					elog(ERROR, "setupQEDtxContext not expecting distributed begin or snapshot when no Snapshot slot exists");
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
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext leaving context = 'Local Only' for Gp_role = %s", role_to_string(Gp_role) );
		return;
	}

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext intermediate result: isEntryDbSingleton = %s, isWriterQE = %s, isReaderQE = %s.", 
		 (isEntryDbSingleton ? "true" : "false"),
		 (isWriterQE ? "true" : "false"), (isReaderQE ? "true" : "false"));

	/*
	 * Copy to our private global variable.
	 */
	memcpy(&QEDtxContextInfo, dtxContextInfo, sizeof(DtxContextInfo));
	
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
		if (isEntryDbSingleton && haveDistributedSnapshot)
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
		else if (isWriterQE && (explicitBegin || needTwoPhase))
		{
			if (!haveDistributedSnapshot)
			{
				elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext QE Writer is involved in a distributed transaction without a distributed snapshot...");
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
				
				doQEDistributedExplicitBegin(txnOptions);
			}
			else
			{
				Assert(needTwoPhase);
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

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "setupQEDtxContext final result: DistributedTransactionContext = '%s'.", 
		 DtxContextToString(DistributedTransactionContext));
}

void
finishDistributedTransactionContext (char* debugCaller)
{
	if(currentGxact != NULL)
	{
//		PleaseDebugMe("finishDistributedTransactionContext currentGxact == NULL");
	
		elog(FATAL, "Expected currentGxact to be NULL at this point.  Found gid =%s, gxid = %u (caller = %s)",
			currentGxact->gid, currentGxact->gxid, debugCaller);
	}
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "finishDistributedTransactionContext called to change DistributedTransactionContext from %s to %s (caller = %s)",
		 DtxContextToString(DistributedTransactionContext), 
		 DtxContextToString(DTX_CONTEXT_LOCAL_ONLY), 
		 debugCaller);
	
	DistributedTransactionContext = DTX_CONTEXT_LOCAL_ONLY;

	resetDtxContextInfo(&QEDtxContextInfo);

}

void
rememberDtxExplicitBegin(void)
{
	if (currentGxact == NULL)
	{
		return;
	}

	if (!currentGxact->explicitBeginRemembered)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"rememberDtxExplicitBegin explicit BEGIN for gid = %s", 
			 currentGxact->gid);
		currentGxact->explicitBeginRemembered = true;
	}
	else
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"rememberDtxExplicitBegin already an explicit BEGIN for gid = %s",
			 currentGxact->gid);
	}
}

/*
 * This is mostly here because
 * cdbcopy doesn't use cdbdisp's services.
 */
void
sendDtxExplicitBegin(void)
{
	rememberDtxExplicitBegin();
	
	setCurrentDtxTwoPhase("sendDtxExplicitBegin", "(none)");
	
	/*
	 * dispatch a DTX command, in the event of an error, this call
	 * will either exit via elog()/ereport() or return false
	 */
	if (!dispatchDtxCommand("BEGIN", /* dispatchToMirrors */ true, /* withSnapshot */ true, /* raiseError */ false))
	{
			ereport(ERROR, (errmsg("Global transaction BEGIN failed failed for gid = \"%s\" due to error",
								   currentGxact->gid)));
	}
}

void
performDtxProtocolCommand(DtxProtocolCommand dtxProtocolCommand,
					int flags,
					const char *loggingStr, const char *gid, 
					DistributedTransactionId gxid)
{
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand called with DTX protocol = %s, QE distribute transaction context: '%s'",
		DtxProtocolCommandToString(dtxProtocolCommand), DtxContextToString(DistributedTransactionContext));
	
	switch (dtxProtocolCommand)
	{
	case DTX_PROTOCOL_COMMAND_ABORT:
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand going to call AbortOutOfAnyTransaction for distributed transaction %s", gid);
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
				elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand going to call PrepareTransactionBlock for distributed transaction %s", gid);
				if (!PrepareTransactionBlock((char *)gid))
				{
					elog(ERROR, "Prepare of distributed transaction %s failed", gid);
					break;
				}
			}
			else
			{
				elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand going to call EndTransactionBlock for distributed transaction %s", gid);
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
				elog((Debug_print_full_dtm ? LOG : DEBUG5), "Prepare of distributed transaction %s succeeded", gid);
				
				DistributedTransactionContext = DTX_CONTEXT_QE_PREPARED;
			}
			else
			{
				elog((Debug_print_full_dtm ? LOG : DEBUG5), "Read only commit of distributed transaction %s succeeded", gid);
				
				Assert(DistributedTransactionContext == DTX_CONTEXT_LOCAL_ONLY);
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

		elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand going to call FinishPreparedTransaction for distributed transaction %s", gid);

		/*
		 * Since this call may fail, lets setup a handler.
		 */
		PG_TRY();
		{
			FinishPreparedTransaction((char *)gid, /* isCommit */ true, /* raiseErrorIfNotFound */ true);
		}
		PG_CATCH();
		{
			finishDistributedTransactionContext("performDtxProtocolCommand -- Commit Prepared (error case)");
			PG_RE_THROW();
		}
		PG_END_TRY();
		
		finishDistributedTransactionContext("performDtxProtocolCommand -- Commit Prepared");
		break;

	case DTX_PROTOCOL_COMMAND_RETRY_COMMIT_PREPARED:
		/*
		 * We can be doing this on a different connection, so ignore DistributedTransactionContext.
		 */
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand going to call FinishPreparedTransaction for distributed transaction %s", gid);

		/*
		 * Since this call may fail, lets setup a handler.
		 */
		PG_TRY();
		{
			FinishPreparedTransaction((char *)gid, /* isCommit */ true, /* raiseErrorIfNotFound */ false);
		}
		PG_CATCH();
		{
			finishDistributedTransactionContext("performDtxProtocolCommand -- Retry Commit Prepared (error case)");
			PG_RE_THROW();
		}
		PG_END_TRY();

		finishDistributedTransactionContext("performDtxProtocolCommand -- Retry Commit Prepared");
		break;

	case DTX_PROTOCOL_COMMAND_ABORT_PREPARED:
	case DTX_PROTOCOL_COMMAND_RETRY_ABORT_PREPARED:
		/*
		 * The QD has directed us to abort an implicit or explict
		 * distributed transaction that may or may not have been prepared.
		 */
		switch (DistributedTransactionContext)
		{
		case DTX_CONTEXT_LOCAL_ONLY:
			if (dtxProtocolCommand == DTX_PROTOCOL_COMMAND_ABORT_PREPARED)
			{
				/*
				 * Spontaneously aborted while we were back at the QD?
				 */
			}
			else
			{
				/*
				 * We must be doing this on a different connection.
				 */
				FinishPreparedTransaction((char *)gid, /* isCommit */ false, /* raiseErrorIfNotFound */ false);
			}
			break;
			
		case DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER:
		case DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER:
			AbortOutOfAnyTransaction();

			// AbortCurrentTransaction resets DistributedTransactionContext.
			Assert(DistributedTransactionContext == DTX_CONTEXT_LOCAL_ONLY);
			break;
			
		case DTX_CONTEXT_QE_PREPARED:
			/*
			 * Since this call may fail, lets setup a handler.
			 */
			PG_TRY();
			{
				FinishPreparedTransaction((char *)gid, /* isCommit */ false, /* raiseErrorIfNotFound */ true);
			}
			PG_CATCH();
			{
				finishDistributedTransactionContext("performDtxProtocolCommand -- Abort Prepared (error case)");
				PG_RE_THROW();
			}
			PG_END_TRY();

			finishDistributedTransactionContext("performDtxProtocolCommand -- Abort Prepared");
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

	case DTX_PROTOCOL_COMMAND_RECOVERY_COMMIT_PREPARED:
	case DTX_PROTOCOL_COMMAND_RECOVERY_ABORT_PREPARED:
		if (DistributedTransactionContext != DTX_CONTEXT_LOCAL_ONLY)
		{
			elog(FATAL, "Expected QE distribute transaction context to be '%s', found '%s'",
				DtxContextToString(DTX_CONTEXT_LOCAL_ONLY),
				DtxContextToString(DistributedTransactionContext));
		}
		
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand going to call FinishPreparedTransaction for distributed transaction %s", gid);
		FinishPreparedTransaction((char *)gid, /* isCommit */ (dtxProtocolCommand == DTX_PROTOCOL_COMMAND_RECOVERY_COMMIT_PREPARED),
			                      /* raiseErrorIfNotFound */ false);
		break;

	default:
		elog(ERROR, "Unrecognized dtx protocol command: %d",
			(int) dtxProtocolCommand);
		break;
	}
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "performDtxProtocolCommand successful return for distributed transaction %s", gid);
}


