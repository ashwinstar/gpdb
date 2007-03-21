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
#define PROVIDE_64BIT_CRC
#include "postgres.h"

#include <time.h>

#include "cdb/cdbtm.h"
#include "cdb/cdbtmlog.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbdispatchresult.h"		/* cdbdisp_dumpDispatchResults() */
#include "cdb/cdbvars.h"
#include "libpq-fe.h"
#include "access/xact.h"
#include "cdb/cdbfts.h"
#include "lib/stringinfo.h"
#include "access/twophase.h"


static LWLockId shmControlLock;
static bool *shmTmRecoverred;
static int *shmGIDSeq;
static int *shmNumGxacts;
int		   *shmLatestLogFileNo;
int		   *shmEarliestLogFileNo;

int		   *shmLastFlushedFileNo;
int		   *shmLastFlushedFileOffset;

static int	ControlLockCount = 0;

char		dtm_dir[MAXPGPATH];

/*global transaction array */
static TMGXACT **shmGxactArray,
		   *currentGxact;

static bool duringRecovery = false;
static int	max_tm_gxacts = 100;
static int	checkpoint_interval = 5000;

/* for semantic checking - debugging purpose */
static int	lastGxactState;

static bool isDirty = false;
static bool resolveDtx = false;

/* used for keeping track of whether we have kicked off a BEGIN for this
 * transaction on remote QEs.  This actually gets set just before it is
 * dispatched so you need to be careful exactly how you use this.
 */
static bool txnBegan;

/* i really hate to do this, but until things get completely untangled in
 * here and we get REAL states for 1 phase ROLLBACK and 2-phase rollback
 * we need a way to do the right 2-phase ROLLBACK when abort in-doubt
 * transactions that the DTM has no knowledge of.
 */
static bool twoPhaseRollback = false;


typedef struct InDoubtDtx
{
	char		gid[TMGIDSIZE];
}	InDoubtDtx;


/* state map showing which transitions are valid and which are invalid.
 * entries marked with a 1 are valid.
 */
static const int transitionState[8][8] =
{
/*											NEW STATE
						IN-PROG PREPARE 1P-CMT	2P-CMT ROLLBACK ABORT  CMTDONE	RBDONE */
/* O	 IN_PROG  */	{1,		1,		1,		1,		1,		1,		0,		0},
/* L	 PREPARE  */	{0,		0,		0,		1,		1,		1,		0,		0},
/* D	 1P_CMT   */	{0,		0,		1,		0,		1,		1,		1,		0},
		/* 2P_CMT */	{0,		0,		0,		1,		1,		1,		1,		0},
/* S	 ROLLBACK */	{0,		0,		0,		0,		1,		1,		0,		1},
/* T	 ABORT	  */	{0,		0,		0,		0,		1,		1,		0,		1},
/* A	 CMTDONE  */	{0,		0,		0,		0,		0,		0,		0,		0},
/* T	 RBDONE   */	{0,		0,		0,		0,		0,		0,		0,		0}
/* E			  */
};

#define CHECK_DTX_STATE(curState, newState)  (transitionState[curState][newState])
#define SET_DTX_STATE(gxact, newState)		 (gxact->state=newState)


/*
   #define GET_SEQ_NO(shmGIDSeq)					(shmGIDSeq & 0x00FFFFFF)
   #define GET_SEQ_NODE_NO(shmGIDSeq)				((segno & 0xFF000000) >> 24)
*/

/* here are some flag options relationed to the txnOptions field of 
 * PQsendMppQuery
 */
 
/* bit 1 is for auto-explicit begin
 * bits 2-3 for iso level  00 read-committed
 *						   01 read-uncommitted
 *						   10 repeatable-read
 *						   11 serializable
 * bit 4 is for read-only
 */
#define MPP_OPT_AUTOBEGIN         0x0001   /* auto-explicitly start a transaction. */
#define MPP_OPT_READ_COMMITTED    0x0002   /* iso level to read-committed 
											(only useful with autobegin) */
#define MPP_OPT_READ_UNCOMMITTED  0x0004
#define MPP_OPT_REPEATABLE_READ   0x0006
#define MPP_OPT_SERIALIZABLE 	  0x0008

#define MPP_OPT_READ_ONLY         0x0010



/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */
static const char *formatDtxState(DtxState state);
static int	execDtxCommand(void);
static void initGxact(TMGXACT * gxact);
static TMGXACT *allocGxact(void);

static void releaseGxact(void);
static void writeToTmLog(TMGXACT * gxact, DtxState state);
static void updateDtxState(DtxState state);
static void generateGID(char *gid);

static void recoverTM(void);
static bool recoverInDoubtTransactions(void);
static HTAB *gatherRMInDoubtTransactions(void);
static void abortRMInDoubtTransactions(HTAB *htab);
static bool checkRMs(HTAB *htab, char *gid);
static void removeInDoubtTransaction(HTAB *htab, char *gid);
static void cleanRMResultSets(PGresult **pgresultSets);
static void recoverCommittedDtx(void);
static void recoverRollbackedDtx(void);

static void dumpAllDtx(void);
static void resolveInDoubtDtx(void);
static void dumpRMOnlyDtx(HTAB *htab, StringInfoData *buff);

void		redoTmLog(TmLogRecord * glog);

/*=========================================================================
 * VISIBLE FUNCTIONS
 */

/*
 *	starting a global transaction, called by user begin
 *	or by CdbDispatchCommand
 *
 * dispatchBegin - In general, this should be false as cdbdisp has some
 *					special logic to try and optimize things a bit by not
 *					seperately dispatching BEGIN.  This is mostly here because
 *					cdbcopy doesn't use cdbdisp's services.
 */
int
startDtxTransaction(bool dispatchBegin)
{
	int			ret = DTX_OK;


	if (Mpp_role != MPP_ROLE_DISPATCH)
		return DTX_OK;

	/* reset some transaction state variables */
	isDirty = false;
	txnBegan = false;


	if (currentGxact == NULL)
	{
		ret = condStartDtxTransaction();

		if (dispatchBegin)
		{
			if (execDtxCommand() != DTX_OK)
			{
				ereport(ERROR, (errmsg("Global transaction failed on command \"BEGIN %s\" due to error",
									   currentGxact->gid)));
			}
		}
	}

	/*
	 * this happens when the user explictly calls BEGIN and for certain
	 * utility statements (like COPY) we need to make sure we kick off the
	 * begin even though the currentGxact is not null
	 */
	else if (dispatchBegin && !txnBegan)
	{
		if (execDtxCommand() != DTX_OK)
		{
			ereport(ERROR, (errmsg("Global transaction failed on command \"BEGIN %s\" due to error",
								   currentGxact->gid)));
		}
	}

	Assert(currentGxact->infomask == 0);
	return ret;
}

/*
 * committing a global transaction, called by user commit
 * or by CommitTransaction
 */
int
commitDtxTransaction(void)
{
	if (Mpp_role != MPP_ROLE_DISPATCH || currentGxact == NULL)
		return DTX_OK;

	return condCommitDtxTransaction(!isDirty);
}

/*
 * rollback a global transaction, called by user rollback
 * or by AbortTransaction during Postgres automatic rollback
 */
int
rollbackDtxTransaction(void)
{
	isDirty = false;
	if (Mpp_role != MPP_ROLE_DISPATCH || currentGxact == NULL)
		return DTX_OK;


	/*
	 * temporary ignore this call if it is in phase 2 commit, which cannot be
	 * rolled back. We may need to handle this case for FTS
	 */
	if (currentGxact->state == DTX_STATE_2P_COMMIT)
		return DTX_OK;


	return condRollbackDtxTransaction();
}

/*
 * condStartDtxTransaction:
 * Starts a global transaction if one is not already started.
 *
 * We don't dispatch BEGIN statements separately at the start of a DTM
 * trasaction anymore.	They are piggy-backed with the first real statement
 * that gets dispatched to the qExecs.	This eliminates an extra dispatch
 * and also defers taking a Snapshot till the first statement, which more
 * closely matches the normal Postgres method.	If we didn't do this, things like
 * SET TRANSACTION ISOLATION LEVEL would complain.
 */
int
condStartDtxTransaction(void)
{

	if (currentGxact != NULL)
	{
		/*
		 * If the currentGxact isn't NULl then we sure better be IN PROGRESS.
		 * This allows us to call this function all the time whenever we are
		 * about to dispatch a statement
		 */
		if (currentGxact->state != DTX_STATE_IN_PROGRESS)
			elog(ERROR, "DTM can't start a Dtx if one is already in progress "
				 " and in the midst of being finished");

		return DTX_OK;
	}

	/* Check if we have in-doubt transactions to resolve */
	if (resolveDtx)
		resolveInDoubtDtx();


	currentGxact = allocGxact();

	elog(DEBUG2, "condStartDtxTransaction:  %s", currentGxact->gid);

	return DTX_OK;
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
	return isDirty || (currentGxact != NULL);
}

/*
 * When cleaning up FTS needs to know if there is a transaction active.
 */
bool
isCurrentDtxActive(void)
{
	return (currentGxact != NULL);
}

/*
 * committing a global transaction, called by internal
 * this method does a two-phase commit.
 */
int
condCommitDtxTransaction(bool onephase)
{
	if (currentGxact == NULL)
	{
		return DTX_ERR_PROTO;
	}
	elog(DEBUG2, "condCommitDtxTransaction:  %s", currentGxact->gid);

	if (onephase)
	{
		/* one phase commit case */
		updateDtxState(DTX_STATE_1P_COMMIT);

		if (execDtxCommand() != DTX_OK)
		{
			ereport(ERROR,
					(errmsg("Global transaction failed on command \"COMMIT %s\" due to error",
							currentGxact->gid)));
		}
	}
	else
	{
		/* update the tm state first, so we know the latest state of the txn */
		updateDtxState(DTX_STATE_PREPARED);

		/*
		 * set the transaction to be prepared, so it will not be
		 * removed before all qe finish (we must do this before we
		 * send the "PREPARE TRANSACTION" to even one QE so that we
		 * can properly do a "ROLLBACK PREPARED" for the case where a
		 * prepare fails on any QE).
		 */
		currentGxact->infomask |= GXACT_STATUS_PERSISTENT;

		/* send prepare to all RM */
		if (execDtxCommand() != DTX_OK)
		{
			ereport(ERROR,
					(errmsg("Global transaction failed on command \"PREPARE TRANSACTION %s\" due to error",
							currentGxact->gid)));
		}

		/* send phase 2 commit to all RM */
		updateDtxState(DTX_STATE_2P_COMMIT);
		if (execDtxCommand() != DTX_OK)
		{
			ereport(ERROR,
					(errmsg("Global transaction failed on command \"COMMIT PREPARED %s\" due to error",
							currentGxact->gid)));
		}
	}

	/* set txn tm state to be done, so it will be removed from cache */
	updateDtxState(DTX_STATE_COMMIT_DONE);
	return DTX_OK;
}

/*
 * rolling back a global transaction, called by internal
 */
int
condRollbackDtxTransaction(void)
{
	int			ret = DTX_OK;
	bool		ignoreError;
	int			previous_state;
	StringInfoData errMessage;

	if (currentGxact == NULL)
	{
		return DTX_ERR_PROTO;
	}


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
		if (currentGxact->state == DTX_STATE_IN_PROGRESS)
			releaseGxact();

		return DTX_OK;
	}


	ereport(DEBUG2,
			(errmsg("condRollbackDtxTransaction:  %s", currentGxact->gid)));

	memset(&errMessage, 0, sizeof(errMessage));

	/*
	 * keep previous state, the transaction has been aborted or rollback has
	 * been called.
	 */
	ignoreError = (currentGxact->state == DTX_STATE_ABORT ||
				   currentGxact->state == DTX_STATE_ROLLBACK);
	previous_state = currentGxact->state;

	/* update tm state first */
	updateDtxState(DTX_STATE_ROLLBACK);

	/* send rollback to all RM */
	if (previous_state == DTX_STATE_ROLLBACK ||
		(ret = execDtxCommand()) != DTX_OK)
	{
		char	   *gid = currentGxact->gid;

		/*
		 * clean up gxact even if rollback failed, txn on QE will be
		 * automatocally rollback, so we need to clean txn in DTM
		 */
		if ((currentGxact->infomask & GXACT_STATUS_PERSISTENT) == 0)
			updateDtxState(DTX_STATE_ROLLBACK_DONE);
		else
		{
			/* leave it in the list for recovery */
			currentGxact->infomask &= GXACT_STATUS_UNRESOLVED;
			currentGxact = NULL;
		}

		/* destroy all gangs to clean up connections */
		disconnectAndDestroyAllGangs();

		/* if the first time failure, throw exception, otherwise keep silent */
		if (!ignoreError)
			ereport(ERROR,
					(errmsg("Global transaction failed on command \"ROLLBACK PREPARED %s\"", gid),
					 errdetail("%s", errMessage.data)));

		if (errMessage.data)
			pfree(errMessage.data);

	}
	else
		/* remove gxact either rollback is successful or xact is not prepared. */
		updateDtxState(DTX_STATE_ROLLBACK_DONE);

	return ret;
}



/*
 * start scan of current active global transactions.
 */
int
startTMXactScan(void)
{
	getTmLock();
	return *shmNumGxacts;
}

/*
 * scaning current active global transaction.
 */
TMGXACT *
getNextTMXact(int index)
{
	return shmGxactArray[index];
}

/*
 * end scaning current active global transaction.
 */
void
endTMXactScan(void)
{
	releaseTmLock();
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
	else
	{
		getTmLock();
		openTmLogFile(*shmLatestLogFileNo);
		releaseTmLock();
	}
}

/* get tm share memory size */
int
tmShmemSize(void)
{
	if (Mpp_role != MPP_ROLE_DISPATCH)
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

	if (Mpp_role != MPP_ROLE_DISPATCH)
		return;

	shared = (TmControlBlock *) ShmemInitStruct("Transaction manager", tmShmemSize(), &found);
	if (!shared)
		elog(FATAL, "could not initialize transaction manager share memory");

	shmControlLock = shared->ControlLock;
	shmTmRecoverred = &shared->recoverred;
	shmGIDSeq = &shared->seqno;
	shmLatestLogFileNo = &shared->latestLogFileNo;
	shmEarliestLogFileNo = &shared->earliestLogFileNo;
	shmLastFlushedFileNo = &shared->lastFlushedFileNo;
	shmLastFlushedFileOffset = &shared->lastFlushedFileOffset;
	shmNumGxacts = &shared->num_active_xacts;
	shmGxactArray = shared->gxact_array;

	if (!IsUnderPostmaster)
		/* Initialize locks and shared memory area */
	{
		int			i = 0;
		TMGXACT    *gxact = NULL;

		shared->ControlLock = LWLockAssign();
		shmControlLock = shared->ControlLock;

		/* initialize gxact array */
		gxact = (TMGXACT *) (shmGxactArray + max_tm_gxacts);
		for (i = 0; i < max_tm_gxacts; i++)
			shmGxactArray[i] = gxact++;
		*shmEarliestLogFileNo = *shmLatestLogFileNo = -1;
	}

	/* Init directory path */
	snprintf(dtm_dir, MAXPGPATH, "%s/pg_dtm", DataDir);
}

/* shutdown tm, called when process exits */
void
shutdownTM(void)
{

	if (*shmLatestLogFileNo == -1)
		return;

	getTmLock();

	if (*shmLatestLogFileNo != -1)
	{
		if (*shmNumGxacts != 0)
		{
			releaseTmLock();
			return;
		}
		checkPointTm();
	}

	releaseTmLock();
}


/*
 * restore global transaction during tm log recovery
 */
void
restoreGxact(TMGXACT * gxact)
{
	if (*shmNumGxacts >= max_tm_gxacts)
	{
		ereport(ERROR,
				(errmsg("cannot allocate global transaction")));
	}

	currentGxact = shmGxactArray[(*shmNumGxacts)++];
	if (gxact != NULL)
		memcpy(currentGxact, gxact, sizeof(TMGXACT));
}

/* mppTxnOptions:
 * Generates an int containing the appropriate flags to direct the remote
 * segdb QE process to perform any needed transaction commands before or
 * after the statement.
 */
int
mppTxnOptions(bool autoBegin)
{
	int options = 0;

	if (autoBegin)
		options |= MPP_OPT_AUTOBEGIN;

	if (XactIsoLevel == XACT_READ_COMMITTED)
		options |= MPP_OPT_READ_COMMITTED;
	else if (XactIsoLevel == MPP_OPT_REPEATABLE_READ)
		options |= MPP_OPT_REPEATABLE_READ;
	else if (XactIsoLevel == MPP_OPT_SERIALIZABLE)
		options |= MPP_OPT_SERIALIZABLE;

	if (XactReadOnly)
		options |= MPP_OPT_READ_ONLY;
		
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

	if (txnOptions & MPP_OPT_READ_COMMITTED)
		*isoLevel = XACT_READ_COMMITTED;
	else if (txnOptions & MPP_OPT_SERIALIZABLE)
		*isoLevel = XACT_SERIALIZABLE;
	else if( txnOptions & MPP_OPT_REPEATABLE_READ)
		*isoLevel = XACT_REPEATABLE_READ;
	else
		*isoLevel = XACT_READ_UNCOMMITTED;

	if (txnOptions & MPP_OPT_READ_ONLY)
		*readOnly = true;
	else
		*readOnly = false;

}

/* isMppTxOptionsAutoBegin:
 * Return the AutoBegin flag.
 */
bool
isMppTxOptionsAutoBegin(int txnOptions)
{
	return ((txnOptions & MPP_OPT_AUTOBEGIN) != 0);
}

								

/* Marks the state such that a BEGIN has been executed on
 * the remote QEs.	This is part of the code that supports the DTM from
 * not having to explicitly issue a BEGIN at the beginning of a tranaction
 * but instead have cdbdisp mark an option such that the remote QE will
 * see that option and do a BEGIN just before it executes the first
 * statement of the new transaction.  This cuts down on a whole set of
 * dispatching overhead.
 */
void
setTxnBegan(void)
{
	txnBegan = true;
}


/* Asks whether a BEGIN statement has already been executed on the remote
 * QEs.  This is part of the code that supports the DTM not having to dispatch
 * begins but instead have it be piggy-backed with the first statement.
 */
bool
hasTxnBegan(void)
{
	return txnBegan;
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

/*
 * Semantic Checking - all this from QD to make sure both QD and QE transaction
 * are all commit or all rollback
 * --	For debugging purpose
 */
void
semanticCheckingOnQE(bool commit)
{
	if (commit)
		Assert(lastGxactState == DTX_STATE_2P_COMMIT ||
			   lastGxactState == DTX_STATE_COMMIT_DONE);
	else
		Assert(lastGxactState == DTX_STATE_ROLLBACK ||
			   lastGxactState == DTX_STATE_ROLLBACK_DONE);
}

/*
 * discardDtxTransaction: called by fts to discard it
 */
void
discardDtxTransaction(void)
{
	if (currentGxact == NULL)
		return;


	if (currentGxact->state == DTX_STATE_2P_COMMIT)
	{
		/*
		 * This means the current transaction has bee sucessfully prepared on
		 * all of the needed qExecs.  We need to try and go ahead and finish
		 * this off by committing it.
		 *
		 * TODO: Its possible for this to get called while in the middle of
		 * issuing the final COMMIT PREPARED to the qExecs.  Since this only
		 * gets called by FTS when something is wrong it seems unlikely that
		 * the Dtx will be able to be properly recovered. I think that this
		 * results in a mis-handling of this case. It seems like some serious
		 * bookkeeping needs to occur in the case that we have a transaction
		 * partially committing across the cluster. I would think this would
		 * involve communicating this back to the FTS system.
		 */
		recoverCommittedDtx();
	}
	else if (currentGxact->state == DTX_STATE_PREPARED ||
			 currentGxact->state == DTX_STATE_1P_COMMIT)
	{
		/* update tm state first and send rollback to all qe */
		updateDtxState(DTX_STATE_ABORT);
		condRollbackDtxTransaction();
	}
	else
	{
		/*
		 * discard the transaction, update tm state only. in this case,
		 * all connection should be restarted and this global transaction
		 * should be automatically rollback
		 */
		updateDtxState(DTX_STATE_ROLLBACK);
		updateDtxState(DTX_STATE_ROLLBACK_DONE);
	}

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
 * redo transaction log
 */
void
redoTmLog(TmLogRecord * glog)
{
	int			i;

	for (i = 0; i < *shmNumGxacts; i++)
	{
		if (strcmp(glog->header.gid, shmGxactArray[i]->gid) == 0)
		{
			/* found an active global transaction */
			currentGxact = shmGxactArray[i];
			break;
		}
	}
	if (i == *shmNumGxacts)
	{
		/*
		 * transaction not found, this is the first log of this transaction,
		 * amd it should be a prepare log, rollback log or 1p commit log.
		 */
		Assert(glog->header.state == DTX_STATE_PREPARED ||
			   glog->header.state == DTX_STATE_1P_COMMIT ||
			   glog->header.state == DTX_STATE_ROLLBACK);
		restoreGxact(NULL);
		memcpy(currentGxact->gid, glog->header.gid, TMGIDSIZE);
	}

	/* use set state instead of update state, so log will not written again */
	SET_DTX_STATE(currentGxact, glog->header.state);
	currentGxact->fileno = glog->header.thisFileNo;
	currentGxact->offset = glog->header.thisOffset;

	/* release it if it's done */
	if (currentGxact->state == DTX_STATE_ROLLBACK_DONE ||
		currentGxact->state == DTX_STATE_COMMIT_DONE)
		releaseGxact();
}


/*=========================================================================
 * HELPER FUNCTIONS
 */


/*
 * formatDtxState:
 * Map transaction state to a string.
 */
static const char *
formatDtxState(DtxState state)
{
	const char *s;

	switch (state)
	{
		case DTX_STATE_IN_PROGRESS:
			s = "IN_PROGRESS";
			break;
		case DTX_STATE_PREPARED:
			s = "PREPARED";
			break;
		case DTX_STATE_1P_COMMIT:
			s = "1P_COMMIT";
			break;
		case DTX_STATE_2P_COMMIT:
			s = "2P_COMMIT";
			break;
		case DTX_STATE_ROLLBACK:
			s = "ROLLBACK";
			break;
		case DTX_STATE_ABORT:
			s = "ABORT";
			break;
		case DTX_STATE_COMMIT_DONE:
			s = "COMMIT_DONE";
			break;
		case DTX_STATE_ROLLBACK_DONE:
			s = "ROLLBACK_DONE";
			break;
		default:
			s = "?DtxState?";
	}
	return s;
}	/* formatDtxState */

bool
dispatchDtxCommand(const char *cmd, bool dispatchToMirrors, bool withSnapshot)
{
	int		i, resultCount, numOfFailed = 0;

	struct pg_result **results = NULL;
	StringInfoData errbuf;

	elog(DEBUG3, "dispatchDtxCommand: '%s' willMirror: %s", cmd, dispatchToMirrors ? "yes" : "no");

	initStringInfo(&errbuf);
	results = cdbdisp_dispatchRMCommand(cmd,
										withSnapshot,
										dispatchToMirrors,
										&errbuf, &resultCount);

	if (errbuf.len > 0)
		ereport(ERROR, (errmsg("DTM error (gathered %d results from cmd '%s')", resultCount, cmd),
						errdetail("%s", errbuf.data)));

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

/*
 * execute a transaction command on all QE
 */
static int
execDtxCommand(void)
{
	char		*cmd = NULL,
				cmdbuf[100];
	int			ret = DTX_OK;
	bool		dispatchToMirrors = true;
	bool		needSnapshot = false;

	switch (currentGxact->state)
	{
		case DTX_STATE_IN_PROGRESS:

			/*
			 * Temporarly, don't add options unless they differ from
			 * the default...  In some cases we are issuing 2 BEGINs in a
			 * row.  The 2nd BEGIN will only get a warning if it has no options...
			 */
			if (DefaultXactIsoLevel != XactIsoLevel || DefaultXactReadOnly != XactReadOnly)
			{
				/* 
				 * Be explicit about both the isolation level and the access mode
				 * since in MPP our QEs are in a another process.
				 */
				sprintf(cmdbuf, "BEGIN ISOLATION LEVEL %s, READ %s", 
						IsoLevelAsUpperString(XactIsoLevel),
						(XactReadOnly ? "ONLY" : "WRITE"));
				cmd = cmdbuf;
			}
			else
				cmd = "BEGIN";

			elog(DEBUG3, "execDtxCommand %s.", cmdbuf);

			needSnapshot = true;

			setTxnBegan();

			break;
		case DTX_STATE_PREPARED:
			if (!txnBegan)
				return DTX_OK;
			sprintf(cmdbuf, "PREPARE TRANSACTION '%s'", currentGxact->gid);
			cmd = cmdbuf;
			break;
		case DTX_STATE_1P_COMMIT:
			if (!txnBegan)
				return DTX_OK;

			/*
			 * In the case of a 1P commit being sent out, we don't need to
			 * dispatch a commit to the mirrors since they never received a
			 * statement to begin a txn
			 */
			dispatchToMirrors = false;
			cmd = "COMMIT";
			break;
		case DTX_STATE_2P_COMMIT:
			if (!txnBegan)
				return DTX_OK;
			sprintf(cmdbuf, "COMMIT PREPARED '%s'", currentGxact->gid);
			cmd = cmdbuf;
			break;
		case DTX_STATE_ROLLBACK:

			/*
			 * BUG? shouldn't there be a 1P rollback and a 2P rollback?
			 * Anyway, the new 8.1 postgres code needs a "ROLLBACK PREPARED"
			 * if and only if we've done the prepare, else it needs a normal
			 * ROLLBACK.
			 *
			 * TODO: need to break out and add states for 1P rollback and 2P
			 * rollback. this will require untangling the infomask... but
			 * really needs to be done.  once done, should be able to get rid
			 * of the twoPhaseRollback flag.
			 *
			 */
			if (twoPhaseRollback)
			{
				sprintf(cmdbuf, "ROLLBACK PREPARED '%s'", currentGxact->gid);
				cmd = cmdbuf;
			}
			else if ((currentGxact->infomask & GXACT_STATUS_PERSISTENT) == 0)
				/* ? ? ? ? ? ? Bad way to check if we 've prepared? */
			{
				elog(DEBUG3, "execDtxCommand about to issue a ROLLBACK command with currState = DTX_STATE_ROLLBACK");

				cmd = "ROLLBACK";
			}
			else
			{
				sprintf(cmdbuf, "ROLLBACK PREPARED '%s'", currentGxact->gid);
				cmd = cmdbuf;
			}
			break;
		default:
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
							errmsg_internal("Transaction %s invalid state %s",
											currentGxact->gid,
									  formatDtxState(currentGxact->state))));
	}

	if (dispatchDtxCommand(cmd, dispatchToMirrors, needSnapshot))
	{
		/* everything went swell. */
		return DTX_OK;
	}

	/* Something horrible happened inside our dispatch ? */
	switch (currentGxact->state)
	{
		case DTX_STATE_PREPARED:
			elog(DEBUG1, "encountered an error during prepare statement (%s)", cmd);
		case DTX_STATE_IN_PROGRESS:
		case DTX_STATE_ROLLBACK:
		case DTX_STATE_1P_COMMIT:
			updateDtxState(DTX_STATE_ABORT);
			ret = DTX_ERR_COMM;
			break;
		case DTX_STATE_2P_COMMIT:
			/* keep this transaction for recovery */
			ret = DTX_ERR_COMM;
			break;
		default:
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
							errmsg_internal("Transaction %s invalid state %s",
											currentGxact->gid,
											formatDtxState(currentGxact->state))));
	}

	return ret;
}

/* initialize a global transaction context */
static void
initGxact(TMGXACT * gxact)
{
	generateGID(gxact->gid);
	gxact->state = DTX_STATE_IN_PROGRESS;
	gxact->infomask = 0;
	gxact->fileno = 0;
	gxact->offset = 0;
}


/*
 * allocate a global transaction context from share memory.
 */
static TMGXACT *
allocGxact(void)
{
	TMGXACT    *gxact;

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
	releaseTmLock();

	return gxact;
}


/*
 * release global transaction to share memory
 */
static void
releaseGxact(void)
{
	int			i;

	getTmLock();
	/* find slot of current transaction */

	Assert(currentGxact != NULL);

	for (i = 0; i < *shmNumGxacts; i++)
		if (shmGxactArray[i] == currentGxact)
			break;

	/* move this to the next available slot */
	(*shmNumGxacts)--;
	if (i != *shmNumGxacts)
	{
		shmGxactArray[i] = shmGxactArray[*shmNumGxacts];
		shmGxactArray[*shmNumGxacts] = currentGxact;
	}
	currentGxact = NULL;
	releaseTmLock();
	isDirty = false;
}


/*
 * writeToTmLog:
 * Writes the current state of the transaction to the dtm log file. This
 * DOES NOT guarantee those records are sync'd to disk.
 */
static void
writeToTmLog(TMGXACT * gxact, DtxState state)
{
	TmLogRecord record = {{0, 0, "", TM_LOG_XACT, 0, 0}};

	strcpy(record.header.gid, gxact->gid);
	record.header.state = state;

	/* synchronizing log file write */
	getTmLock();
	writeTmLogRecord(&record);
	currentGxact->fileno = record.header.thisFileNo;
	currentGxact->offset = record.header.thisOffset;
	releaseTmLock();
}


/*
 * updateDtxState:
 * This function manages all dtx state transistions and writes/flushes to
 * the logs as necessary.
 */
static void
updateDtxState(DtxState state)
{
	TMGXACT    *gxact = currentGxact;

	Assert(gxact != NULL);


	/* make sure the state transition is a valid one */
	if (!CHECK_DTX_STATE(gxact->state, state))
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg_internal("Transaction %s invalid state transition: new=%s old=%s",
										gxact->gid,
										formatDtxState(state),
										formatDtxState(gxact->state))));
	}


	/*
	 * update the actual state of the dtx and error out if its not a valid
	 * state change
	 */
	switch (state)
	{
		case DTX_STATE_PREPARED:		/* we've decided to kick off a
										 * PREPARED dispatch */

			if (isDirty)
				writeToTmLog(gxact, state);

			/* set memory state */
			SET_DTX_STATE(gxact, state);

			/*
			 * There isn't a reason to flush the log at this point.  If a
			 * crash happens before we dispatch the PREPARE stmt then all of
			 * the transactions on the QEs will abort anyway.  And if we crash
			 * after the PREPARE stmt has been dispatched, then on startup the
			 * QEs will have an in-doubt transaction that we have no knowledge
			 * of.	So we can abort that and everything is right.
			 */

			break;

		case DTX_STATE_1P_COMMIT:		/* only gonna send COMMIT */

			/* set memory state */
			SET_DTX_STATE(gxact, state);

			/*
			 * no reason to log 1P commit distributed transactions at this
			 * point.  they are only used for transactions that don't update
			 * anything.  this may change in the future if we get smart enough
			 * to use them for certain workloads.
			 */

			break;

		case DTX_STATE_2P_COMMIT:
			if (isDirty)
				writeToTmLog(gxact, state);

			/* set memory state */
			SET_DTX_STATE(gxact, state);

			/*
			 * We need to flush the log at the point that we've made a
			 * decision about the current dtx.	Here we've decided that we are
			 * going to go ahead and commit this dtx.  This means that ALL
			 * necessary QEs have come back and said they are ready to commit.
			 * If we crash before the this record is synced then that also
			 * means we've crashed before we've given the word to commit.
			 *
			 * So upon restart, the QEs will report an in-doubt transaction that
			 * is ready to commit.	If we have no record of them here in the
			 * DTM we can still be safe and just ABORT that work.  If there is
			 * an in-doubt DTX ready to commit thats reported by ALL of the
			 * QEs in the system, then we could chose to COMMIT (as an
			 * optimization). But since we don't have any records as to who
			 * the participants are, we would need EVERYBODY to know about the
			 * DTX.
			 *
			 * Finally, if we actually get around to sync'ing this log record
			 * that shows this DTX should be committed.  Then upon restart the
			 * DTM can resolve this in-doubt transaction without a problem.
			 */


			/*
			 * no need to check for isDirty, since all 2P commit transactions
			 * should be "dirty"
			 */
			getTmLock();
			flushTmLogFile(currentGxact->fileno, currentGxact->offset);
			releaseTmLock();

			break;

		case DTX_STATE_ROLLBACK:

			/*
			 * TODO: break this out into a 2PC ROLLBACK and a 1PC ROLLBACK for
			 * now, we only use 1PC when isDirty is false so we can use that
			 * as an way to distinguish between the two.
			 */
			if (isDirty)
				writeToTmLog(gxact, state);

			/* set memory state */
			SET_DTX_STATE(gxact, state);


			/*
			 * There is no need to flush our log here.	The reason is that at
			 * this point we've already recorded and flushed our decision
			 * about the transaction.  So if anything goes wrong, we pretty
			 * much know that QEs that don't know anything about this DTX have
			 * already committed.  And any ones that are still in-doubt should
			 * be committed.
			 */

			break;

		case DTX_STATE_ABORT:	/* something bad happened during PREPARE */

			if (isDirty)
				writeToTmLog(gxact, state);

			/* set memory state */
			SET_DTX_STATE(gxact, state);

			txnBegan = false;

			/*
			 * There is no need to flush our log here.	The reason is that at
			 * this point we've already recorded and flushed our decision
			 * about the transaction.  So if anything goes wrong, we pretty
			 * much know that QEs that don't know anything about this DTX have
			 * already committed.  And any ones that are still in-doubt should
			 * be committed.
			 */

			break;

		case DTX_STATE_COMMIT_DONE:
		case DTX_STATE_ROLLBACK_DONE:
			if (isDirty)
				writeToTmLog(gxact, state);

			/* set memory state */
			SET_DTX_STATE(gxact, state);

			txnBegan = false;

			releaseGxact();


			/*
			 * Again, no flush is needed.  we have recorded with certainty the
			 * fate of the transaction. so we can always re-query the system
			 * to figure out if the transaction ended properly or not.
			 */

			break;

		default:
			/* TODO: improve this error message quite a bit */
			elog(ERROR, "Internal Error: Invalid DTM state: %d", state);
			break;
	}



	/* Every so often we checkpoint the DTM log. */
	if (!duringRecovery && ((*shmGIDSeq) % checkpoint_interval) == 0)
	{
		getTmLock();
		checkPointTm();
		(*shmGIDSeq)++;
		releaseTmLock();
	}


	lastGxactState = state;
}


/* generate global transaction id */
static void
generateGID(char *gid)
{
	time_t		t = time(NULL);

	if (t == (time_t) -1)
	{
		elog(PANIC, "cannot generate global transaction id");
	}

	/* tm lock acquired by caller */
	sprintf(gid, "%ld-%.10d", t, (*shmGIDSeq)++);
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

	bool		duringRecovery = true;

	/* intialize fts sync count */
	verifyFtsSyncCount();

	elog(DEBUG3, "Starting to Recover DTM...");


	/*
	 * phase 1 recovery read and roll forward tm log
	 */
	recoverTmLog();


	/*
	 * do not do recovery if read only mode is set.
	 * in this case, there may be in-doubt transaction in down segdb
	 * , which will not be resolved at this time.
	 */
	if (isFtsReadOnlySet())
	{
		elog(DEBUG3, "FTS is Read Only.  Defer DTM recovery till later.");
		checkPointTm();
		duringRecovery = false;
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


	/* finished recovery successfully. Checkpoint the DTM log. */
	checkPointTm();
	duringRecovery = false;

}



/* recoverInDoubtTransactions:
 * Go through all in-doubt transactions that the DTM knows about and
 * resolve them.
 */
static bool
recoverInDoubtTransactions(void)
{
	int			i;
	HTAB	   *htab;

	htab = gatherRMInDoubtTransactions();

	/*
	 * turn on the HACK flag. TODO: it really sucks that we have to use this.
	 * basically we don't have a separate state for 1-phase ROLLBACK and
	 * 2-phase ROLLBACK and the normal means of telling the different is
	 * broken for recovery.  since we never recover 1-phase ROLLBACKs (or
	 * 1-phase transactions for that matter), then we just force all ROLLBACKs
	 * to be 2-phase rollbacks for all of recovery.
	 */
	twoPhaseRollback = true;

	dumpAllDtx();

	/* recover in-doubt transaction in TM */
	for (i = 0; i < *shmNumGxacts; i++)
	{
		currentGxact = shmGxactArray[i];

		elog(DEBUG3, "Attempting to resolve in-doubt transaction from DTM state with GID: %s",
			 currentGxact->gid);

		switch (currentGxact->state)
		{
			case DTX_STATE_IN_PROGRESS:
			case DTX_STATE_PREPARED:
			case DTX_STATE_1P_COMMIT:
			case DTX_STATE_ROLLBACK:
				/* reset persistent flag, so currentGxact will be cleaned */
				currentGxact->infomask = 0;

				/* see if RMs have this GID outstanding or not. */
				if (checkRMs(htab, currentGxact->gid))
				{
					char		gid[TMGIDSIZE];

					elog(DEBUG3, "Remote RMs are in-doubt about this transaction too... will Rollback");

					/*
					 * unfortunately, currentGxact gets whacked during
					 * rollback
					 */

					strcpy(gid, currentGxact->gid);

					recoverRollbackedDtx();
					removeInDoubtTransaction(htab, gid);
				}
				else
				{
					elog(DEBUG3, "Only DTM knows about this dtx.");
					updateDtxState(DTX_STATE_ROLLBACK);
					updateDtxState(DTX_STATE_ROLLBACK_DONE);
				}
				break;

			case DTX_STATE_2P_COMMIT:
				if (checkRMs(htab, currentGxact->gid))
				{
					/*
					 * unfortunately, currentGxact gets whacked during
					 * rollback
					 */
					char		gid[TMGIDSIZE];

					strcpy(gid, currentGxact->gid);

					recoverCommittedDtx();
					removeInDoubtTransaction(htab, gid);
				}
				else
					updateDtxState(DTX_STATE_COMMIT_DONE);
				break;

			case DTX_STATE_ABORT:
			case DTX_STATE_COMMIT_DONE:
			case DTX_STATE_ROLLBACK_DONE:
			default:
				ereport(LOG,
				  (errmsg("invalid state during recovery gid: %s, state: %s",
				   currentGxact->gid, formatDtxState(currentGxact->state))));
				releaseGxact();
				break;
		}

		dumpAllDtx();
		i--;
	}



	currentGxact = NULL;

	dumpAllDtx();

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

	/* turn the hack flag off */
	twoPhaseRollback = false;

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
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
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


/*
 * abortRMInDoubtTransactions:
 * Goes through all the InDoubtDtx's in the provided htab and ABORTs them
 * across all of the QEs by sending a ROLLBACK PREPARED.
 *
 */
static void
abortRMInDoubtTransactions(HTAB *htab)
{
	struct CdbDispatchResults *primaryResults = NULL;
	struct CdbDispatchResults *mirrorResults = NULL;
	struct CdbDispatchCmdThreads  *dispatchThreads = NULL;
	SegmentDatabaseDescriptor **failedSegDB = NULL;
	HASH_SEQ_STATUS status;
	int			numOfFailed = 0;
	char	   *cmd = NULL,
				cmdbuf[100];
	InDoubtDtx *entry = NULL;

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
		sprintf(cmdbuf, "ROLLBACK PREPARED '%s'", entry->gid);
		cmd = cmdbuf;

		elog(DEBUG3, "Aborting in-doubt transaction with GID: %s", entry->gid);

		cdbdisp_dispatchCommand(cmd, NULL, 0,
								/* cancelonError */		false,
								/* startTrans */		false,
								/* withSnapshot */		false,
								/* toMirrors */			true,
								/* toPrimaries */		true,
								&primaryResults, &mirrorResults, &dispatchThreads);

		/*
		 * Wait for completion.  Don't cancel.
		 * CDB TODO: Eliminate the CdbCheckDispatchResultInt() interface and
		 * instead build the failedSegDB list from the CdbDispatchResults.
		 */
		CdbCheckDispatchResultInt(dispatchThreads, &failedSegDB, &numOfFailed, false);

		cdbdisp_destroyDispatchResults(primaryResults);
		cdbdisp_destroyDispatchResults(mirrorResults);

	}
}


/*
 * checkRMs:
 * Searches the provided htab to see if it contains an InDoubtDtx
 * with the same dtx global id (gid) as the one provided.
 */
static bool
checkRMs(HTAB *htab, char *gid)
{
	InDoubtDtx *dtx;
	bool		found;

	if (htab == NULL)
		found = false;
	else
		dtx = (InDoubtDtx *) hash_search(htab, gid, HASH_FIND, &found);

	return found;
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
 * recoverRollbackedDtx:
 * Attempts to recover the currentGxact (which is in-doubt) transaction that
 * should be ABORT'd. This just sends a ROLLBACK PREPARED to ALL seg dbs.
 *
 * The TODO: below in recoverCommittedDtx also applies to this.
 */
static void
recoverRollbackedDtx(void)
{
	ereport(DEBUG2,
	  (errmsg("recovery: rollback DtxTransaction:  %s", currentGxact->gid)));

	updateDtxState(DTX_STATE_ROLLBACK);

	/* ignore the error during recovery */
	execDtxCommand();

	updateDtxState(DTX_STATE_ROLLBACK_DONE);
}


/*
 * recoverCommittedDtx:
 * Attempts to recover the currentGxact (which is in-doubt) transaction that
 * should be committed.  This essentially just sends a COMMIT PREPARED to
 * ALL seg dbs.  Only those that are in-doubt will have any idea what to do
 * with this message (i.e. not generate an error) and should go ahead and
 * COMMIT the transaction.
 *
 * TODO: this function doesn't actually check that the transaction gets
 *		 committed everywhere.	the execDtxCommand() basically ignores any
 *		 error handling.  It does this now because the DTM doesn't actually
 *		 keep track of which participants are still in-doubt about the
 *		 transaction.  This really needs to be fixed so that we can get airtight
 *		 state tracking for distributed transactions.
 */
static void
recoverCommittedDtx(void)
{
	ereport(DEBUG2,
			(errmsg("recovery: commit phase 2 DtxTransaction:  %s", currentGxact->gid)));

	/* ignore the error during recovery */
	execDtxCommand();
	updateDtxState(DTX_STATE_COMMIT_DONE);
}


/*
 * resolveInDoubtDtx:
 * legacy function for resolving in doubt transactions. Only used for checking
 * and resolving in-doubt transactions at transaction start time.
 */
static void
resolveInDoubtDtx(void)
{
	int			i;
	TMGXACT    *gxact_saved;

	if (isFtsReadOnlySet())
	{
		/* can not resolve now, wait for next time */
		resolveDtx = false;
		return;
	}

	getTmLock();
	gxact_saved = currentGxact;
	for (i = 0; i < *shmNumGxacts; i++)
	{
		if (shmGxactArray[i]->infomask & GXACT_STATUS_UNRESOLVED)
		{
			/* resolve this */
			currentGxact = shmGxactArray[i];
			execDtxCommand();

			/*
			 * remove gxact either rollback is successful or xact is not
			 * prepared.
			 */
			updateDtxState(DTX_STATE_ROLLBACK_DONE);
		}
	}

	currentGxact = gxact_saved;
	resolveDtx = false;
	releaseTmLock();
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
		elog(LOG, "%d - GID: %s  STATE: %d  INFO: %d  LNO: %d  LOS: %d",
			 i, shmGxactArray[i]->gid, shmGxactArray[i]->state, shmGxactArray[i]->infomask,
			 shmGxactArray[i]->fileno, shmGxactArray[i]->offset);
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
 * See declaration of assign_mpp_write_shared_snapshot(...) for more
 * information.
 */
void verify_shared_snapshot_ready(void)
{
	if (Mpp_role == MPP_ROLE_DISPATCH)
	{
		char cmdbuf[35];
		strcpy(cmdbuf, "set mpp_write_shared_snapshot=true" );
		
		CdbDoCommand(cmdbuf, true, true, true, true);
	}
}

/*
 * Force the writer QE to write the shared snapshot. Will get called
 * after a "set mpp_write_shared_snapshot=<true/false>" is executed 
 * in dispatch mode.
 *
 * See verify_shared_snapshot_ready(...) for additional information.
 */
bool
assign_mpp_write_shared_snapshot(bool newval, bool doit, GucSource source)
{
	
#if FALSE
	elog(DEBUG1, "SET mpp_write_shared_snapshot: %s, doit=%s",
		 (newval ? "true" : "false"), (doit ? "true" : "false"));
#endif
	/* make sure newval is "true". if it's "false" this could be a part
	 * of a ROLLBACK so we don't want to set the snapshot then.
	 */
	if (doit && newval)
	{
		if(Mpp_role == MPP_ROLE_EXECUTE)
			ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());			
	}

	return true;
}

