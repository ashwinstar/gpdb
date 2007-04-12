/*-------------------------------------------------------------------------
 *
 * procarray.c
 *	  POSTGRES process array code.
 *
 *
 * This module maintains an unsorted array of the PGPROC structures for all
 * active backends.  Although there are several uses for this, the principal
 * one is as a means of determining the set of currently running transactions.
 *
 * Because of various subtle race conditions it is critical that a backend
 * hold the correct locks while setting or clearing its MyProc->xid field.
 * See notes in GetSnapshotData.
 *
 * The process array now also includes PGPROC structures representing
 * prepared transactions.  The xid and subxids fields of these are valid,
 * as are the myProcLocks lists.  They can be distinguished from regular
 * backend PGPROCs at need by checking for pid == 0.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/procarray.c,v 1.19 2006/11/05 22:42:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/cdbdistributedxidmap.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/twophase.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/tqual.h"
#include "cdb/cdbtm.h"


#include "access/xact.h"		/* setting the shared xid */

#include "cdb/cdbvars.h"

/* Our shared memory area */
typedef struct ProcArrayStruct
{
	int			numProcs;		/* number of valid procs entries */
	int			maxProcs;		/* allocated size of procs array */

	/*
	 * We declare procs[] as 1 entry because C wants a fixed-size array, but
	 * actually it is maxProcs entries long.
	 */
	PGPROC	   *procs[1];		/* VARIABLE LENGTH ARRAY */
}	ProcArrayStruct;

static ProcArrayStruct *procArray;


#ifdef XIDCACHE_DEBUG

/* counters for XidCache measurement */
static long xc_by_recent_xmin = 0;
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()		(xc_by_recent_xmin++)
#define xc_by_main_xid_inc()		(xc_by_main_xid++)
#define xc_by_child_xid_inc()		(xc_by_child_xid++)
#define xc_slow_answer_inc()		(xc_slow_answer++)

static void DisplayXidCache(void);

#else							/* !XIDCACHE_DEBUG */

#define xc_by_recent_xmin_inc()		((void) 0)
#define xc_by_main_xid_inc()		((void) 0)
#define xc_by_child_xid_inc()		((void) 0)
#define xc_slow_answer_inc()		((void) 0)
#endif   /* XIDCACHE_DEBUG */


/*
 * Report shared-memory space needed by CreateSharedProcArray.
 */
Size
ProcArrayShmemSize(void)
{
	Size		size;

	size = offsetof(ProcArrayStruct, procs);
	size = add_size(size, mul_size(sizeof(PGPROC *),
								 add_size(MaxBackends, max_prepared_xacts)));

	return size;
}

/*
 * Initialize the shared PGPROC array during postmaster startup.
 */
void
CreateSharedProcArray(void)
{
	bool		found;

	/* Create or attach to the ProcArray shared structure */
	procArray = (ProcArrayStruct *)
		ShmemInitStruct("Proc Array", ProcArrayShmemSize(), &found);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		procArray->numProcs = 0;
		procArray->maxProcs = MaxBackends + max_prepared_xacts;
	}
}

/*
 * Add the specified PGPROC to the shared array.
 */
void
ProcArrayAdd(PGPROC *proc)
{
	ProcArrayStruct *arrayP = procArray;

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	if (arrayP->numProcs >= arrayP->maxProcs)
	{
		/*
		 * Ooops, no room.	(This really shouldn't happen, since there is a
		 * fixed supply of PGPROC structs too, and so we should have failed
		 * earlier.)
		 */
		LWLockRelease(ProcArrayLock);
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already")));
	}

	arrayP->procs[arrayP->numProcs] = proc;
	arrayP->numProcs++;

	LWLockRelease(ProcArrayLock);
}

/*
 * Remove the specified PGPROC from the shared array.
 */
void
ProcArrayRemove(PGPROC *proc)
{
	ProcArrayStruct *arrayP = procArray;
	int			index;

#ifdef XIDCACHE_DEBUG
	/* dump stats at backend shutdown, but not prepared-xact end */
	if (proc->pid != 0)
		DisplayXidCache();
#endif

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		if (arrayP->procs[index] == proc)
		{
			arrayP->procs[index] = arrayP->procs[arrayP->numProcs - 1];
			arrayP->numProcs--;
			LWLockRelease(ProcArrayLock);
			return;
		}
	}

	/* Ooops */
	LWLockRelease(ProcArrayLock);

	elog(LOG, "failed to find proc %p in ProcArray", proc);
}


/*
 * TransactionIdIsInProgress -- is given transaction running in some backend
 *
 * There are three possibilities for finding a running transaction:
 *
 * 1. the given Xid is a main transaction Id.  We will find this out cheaply
 * by looking at the PGPROC struct for each backend.
 *
 * 2. the given Xid is one of the cached subxact Xids in the PGPROC array.
 * We can find this out cheaply too.
 *
 * 3. Search the SubTrans tree to find the Xid's topmost parent, and then
 * see if that is running according to PGPROC.	This is the slowest, but
 * sadly it has to be done always if the other two failed, unless we see
 * that the cached subxact sets are complete (none have overflowed).
 *
 * ProcArrayLock has to be held while we do 1 and 2.  If we save the top Xids
 * while doing 1, we can release the ProcArrayLock while we do 3.  This buys
 * back some concurrency (we can't retrieve the main Xids from PGPROC again
 * anyway; see GetNewTransactionId).
 */
bool
TransactionIdIsInProgress(TransactionId xid)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			i,
				j;
	int			nxids = 0;
	TransactionId *xids;
	TransactionId topxid;
	bool		locked;

	/*
	 * Don't bother checking a transaction older than RecentXmin; it could not
	 * possibly still be running.  (Note: in particular, this guarantees
	 * that we reject InvalidTransactionId, FrozenTransactionId, etc as
	 * not running.)
	 */
	if (TransactionIdPrecedes(xid, RecentXmin))
	{
		xc_by_recent_xmin_inc();
		return false;
	}

	/* Get workspace to remember main XIDs in */
	xids = (TransactionId *) palloc(sizeof(TransactionId) * arrayP->maxProcs);

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	locked = true;

	for (i = 0; i < arrayP->numProcs; i++)
	{
		PGPROC	   *proc = arrayP->procs[i];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId pxid = proc->xid;

		if (!TransactionIdIsValid(pxid))
			continue;

		/*
		 * Step 1: check the main Xid
		 */
		if (TransactionIdEquals(pxid, xid))
		{
			xc_by_main_xid_inc();
			result = true;
			goto result_known;
		}

		/*
		 * We can ignore main Xids that are younger than the target Xid, since
		 * the target could not possibly be their child.
		 */
		if (TransactionIdPrecedes(xid, pxid))
			continue;

		/*
		 * Step 2: check the cached child-Xids arrays
		 */
		for (j = proc->subxids.nxids - 1; j >= 0; j--)
		{
			/* Fetch xid just once - see GetNewTransactionId */
			TransactionId cxid = proc->subxids.xids[j];

			if (TransactionIdEquals(cxid, xid))
			{
				xc_by_child_xid_inc();
				result = true;
				goto result_known;
			}
		}

		/*
		 * Save the main Xid for step 3.  We only need to remember main Xids
		 * that have uncached children.  (Note: there is no race condition
		 * here because the overflowed flag cannot be cleared, only set, while
		 * we hold ProcArrayLock.  So we can't miss an Xid that we need to
		 * worry about.)
		 */
		if (proc->subxids.overflowed)
			xids[nxids++] = pxid;
	}

	LWLockRelease(ProcArrayLock);
	locked = false;

	/*
	 * If none of the relevant caches overflowed, we know the Xid is not
	 * running without looking at pg_subtrans.
	 */
	if (nxids == 0)
		goto result_known;

	/*
	 * Step 3: have to check pg_subtrans.
	 *
	 * At this point, we know it's either a subtransaction of one of the Xids
	 * in xids[], or it's not running.  If it's an already-failed
	 * subtransaction, we want to say "not running" even though its parent may
	 * still be running.  So first, check pg_clog to see if it's been aborted.
	 */
	xc_slow_answer_inc();

	if (TransactionIdDidAbort(xid))
		goto result_known;

	/*
	 * It isn't aborted, so check whether the transaction tree it belongs to
	 * is still running (or, more precisely, whether it was running when this
	 * routine started -- note that we already released ProcArrayLock).
	 */
	topxid = SubTransGetTopmostTransaction(xid);
	Assert(TransactionIdIsValid(topxid));
	if (!TransactionIdEquals(topxid, xid))
	{
		for (i = 0; i < nxids; i++)
		{
			if (TransactionIdEquals(xids[i], topxid))
			{
				result = true;
				break;
			}
		}
	}

result_known:
	if (locked)
		LWLockRelease(ProcArrayLock);

	pfree(xids);

	return result;
}

/*
 * TransactionIdIsActive -- is xid the top-level XID of an active backend?
 *
 * This differs from TransactionIdIsInProgress in that it ignores prepared
 * transactions.  Also, we ignore subtransactions since that's not needed
 * for current uses.
 */
bool
TransactionIdIsActive(TransactionId xid)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			i;

	/*
	 * Don't bother checking a transaction older than RecentXmin; it could not
	 * possibly still be running.
	 */
	if (TransactionIdPrecedes(xid, RecentXmin))
		return false;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (i = 0; i < arrayP->numProcs; i++)
	{
		PGPROC	   *proc = arrayP->procs[i];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId pxid = proc->xid;

		if (!TransactionIdIsValid(pxid))
			continue;

		if (proc->pid == 0)
			continue;			/* ignore prepared transactions */

		if (TransactionIdEquals(pxid, xid))
		{
			result = true;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}


/*
 * GetOldestXmin -- returns oldest transaction that was running
 *					when any current transaction was started.
 *
 * If allDbs is TRUE then all backends are considered; if allDbs is FALSE
 * then only backends running in my own database are considered.
 *
 * If ignoreVacuum is TRUE then backends with inVacuum set are ignored.
 *
 * This is used by VACUUM to decide which deleted tuples must be preserved
 * in a table.	allDbs = TRUE is needed for shared relations, but allDbs =
 * FALSE is sufficient for non-shared relations, since only backends in my
 * own database could ever see the tuples in them.	Also, we can ignore
 * concurrently running lazy VACUUMs because (a) they must be working on other
 * tables, and (b) they don't need to do snapshot-based lookups.
 *
 * This is also used to determine where to truncate pg_subtrans.  allDbs
 * must be TRUE for that case, and ignoreVacuum FALSE.
 *
 * Note: we include the currently running xids in the set of considered xids.
 * This ensures that if a just-started xact has not yet set its snapshot,
 * when it does set the snapshot it cannot set xmin less than what we compute.
 */
TransactionId
GetOldestXmin(bool allDbs, bool ignoreVacuum)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId result;
	int			index;

	/*
	 * Normally we start the min() calculation with our own XID.  But if
	 * called by checkpointer, we will not be inside a transaction, so use
	 * next XID as starting point for min() calculation.  (Note that if there
	 * are no xacts running at all, that will be the subtrans truncation
	 * point!)
	 */
	if (IsTransactionState())
		result = GetTopTransactionId();
	else
		result = ReadNewTransactionId();

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (ignoreVacuum && proc->inVacuum)
			continue;

		if (allDbs || proc->databaseId == MyDatabaseId)
		{
			/* Fetch xid just once - see GetNewTransactionId */
			TransactionId xid = proc->xid;

			if (TransactionIdIsNormal(xid))
			{
				/* First consider the transaction own's Xid */
				if (TransactionIdPrecedes(xid, result))
					result = xid;

				/*
				 * Also consider the transaction's Xmin, if set.
				 *
				 * We must check both Xid and Xmin because there is a window
				 * where an xact's Xid is set but Xmin isn't yet.
				 */
				xid = proc->xmin;
				if (TransactionIdIsNormal(xid))
					if (TransactionIdPrecedes(xid, result))
						result = xid;
			}
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

void
updateSharedLocalSnapshot(DtxContextInfo *dtxContextInfo, Snapshot snapshot)
{
	Assert(SharedLocalSnapshotSlot != NULL);
	Assert(snapshot != NULL);
	
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "updateSharedLocalSnapshot for DistributedTransactionContext = '%s' passed local snapshot (xmin: %u xmax: %u xcnt: %u) curcid: %d", 
		 DtxContextToString(DistributedTransactionContext),
		 snapshot->xmin,
		 snapshot->xmax,
		 snapshot->xcnt,
		 snapshot->curcid);
			 
	SharedLocalSnapshotSlot->snapshot.xmin = snapshot->xmin;
	SharedLocalSnapshotSlot->snapshot.xmax = snapshot->xmax;
	SharedLocalSnapshotSlot->snapshot.xcnt = snapshot->xcnt;


	// UNDONE: Are xip and subxids broken in the SharedLocalSnapshotSlot?

	/*
	 * Copy all active subtransctions to shared snapshot.
	 *
	 */
    GetSubtransactionsForSharedSnapshot(
    	&SharedLocalSnapshotSlot->subcnt,
    	SharedLocalSnapshotSlot->subxids);
	elog((Debug_print_full_dtm ? LOG : DEBUG5),"updateSharedLocalSnapshot count of subtransaction ids %u",SharedLocalSnapshotSlot->subcnt);

	if (snapshot->xcnt > 0)
	{
		/* Verify xip array pointer to space allocated after slot struct */
		Assert(SharedLocalSnapshotSlot->snapshot.xip == ((char *)SharedLocalSnapshotSlot) + sizeof(SharedSnapshotSlot));
		Assert(snapshot->xip != NULL);
	
		elog((Debug_print_full_dtm ? LOG : DEBUG5),"updateSharedLocalSnapshot count of in-doubt ids %u",SharedLocalSnapshotSlot->snapshot.xcnt);

		memcpy(SharedLocalSnapshotSlot->snapshot.xip, snapshot->xip, snapshot->xcnt * sizeof(TransactionId));
	}
	
	SharedLocalSnapshotSlot->snapshot.curcid = snapshot->curcid;

	SetSharedTransactionId();
	
	SharedLocalSnapshotSlot->QDcid = dtxContextInfo->distributedSnapshotData.curcid;
	SharedLocalSnapshotSlot->QDxid = dtxContextInfo->distributedXid;
		
	SharedLocalSnapshotSlot->ready = true;
	elog((Debug_print_full_dtm ? LOG : DEBUG5), "updateSharedLocalSnapshot for DistributedTransactionContext = '%s' setting shared local snapshot xid = %u (xmin: %u xmax: %u xcnt: %u) curcid: %d, QDxid = %u, QDcid = %u", 
		 DtxContextToString(DistributedTransactionContext),
		 SharedLocalSnapshotSlot->xid,
		 SharedLocalSnapshotSlot->snapshot.xmin,
		 SharedLocalSnapshotSlot->snapshot.xmax,
		 SharedLocalSnapshotSlot->snapshot.xcnt,
		 SharedLocalSnapshotSlot->snapshot.curcid,
		 SharedLocalSnapshotSlot->QDxid,
		 SharedLocalSnapshotSlot->QDcid);
}
	

/*----------
 * GetSnapshotData -- returns information about running transactions.
 *
 * The returned snapshot includes xmin (lowest still-running xact ID),
 * xmax (next xact ID to be assigned), and a list of running xact IDs
 * in the range xmin <= xid < xmax.  It is used as follows:
 *		All xact IDs < xmin are considered finished.
 *		All xact IDs >= xmax are considered still running.
 *		For an xact ID xmin <= xid < xmax, consult list to see whether
 *		it is considered running or not.
 * This ensures that the set of transactions seen as "running" by the
 * current xact will not change after it takes the snapshot.
 *
 * All running top-level XIDs are included in the snapshot.  We also try
 * to include running subtransaction XIDs, but since PGPROC has only a
 * limited cache area for subxact XIDs, full information may not be
 * available.  If we find any overflowed subxid arrays, we have to mark
 * the snapshot's subxid data as overflowed, and extra work will need to
 * be done to determine what's running (see XidInSnapshot() in tqual.c).
 *
 * We also update the following backend-global variables:
 *		TransactionXmin: the oldest xmin of any snapshot in use in the
 *			current transaction (this is the same as MyProc->xmin).  This
 *			is just the xmin computed for the first, serializable snapshot.
 *		RecentXmin: the xmin computed for the most recent snapshot.  XIDs
 *			older than this are known not running any more.
 *		RecentGlobalXmin: the global xmin (oldest TransactionXmin across all
 *			running transactions, except those running LAZY VACUUM).  This is
 *			the same computation done by GetOldestXmin(true, true).
 *----------
 */
Snapshot
GetSnapshotData(Snapshot snapshot, bool serializable,
                DistributedSnapshot *distributedSnapshot,
                DistributedSnapshotData *distributedSnapshotData)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId xmin,
				xmax;
	TransactionId globalxmin;
	int			index;
	int			count = 0;
	int			subcount = 0;

	Assert(snapshot != NULL);
	Assert(distributedSnapshot != NULL);
	Assert(distributedSnapshotData != NULL);

	if (DistributedTransactionContext == DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE)
	{
		if(createDtxSnapshot(distributedSnapshotData))
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"GetSnapshotData got a distributed snapshot from createDtxSnapshot!");
			*distributedSnapshot = distributedSnapshotData;
		}
		else
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5),"GetSnapshotData got no distributed snapshot from createDtxSnapshot");
			
			*distributedSnapshot = NULL;
		}
	}
	else
	{
		*distributedSnapshot = NULL;
	}

	/*
	 * MPP Addition.  if we are in EXECUTE mode and not the writer... then we
	 * want to just get the shared snapshot and make it our own.
	 *
	 * code for the writer is at the bottom of this function.
	 *
	 * NOTE: we could be dispatched and get here before the WRITER can set the
	 * shared snapshot.  if this happens we'll have to wait around, hopefully
	 * its never for a very long time.
	 *
	 */
	if (DistributedTransactionContext == DTX_CONTEXT_QE_READER ||
		DistributedTransactionContext == DTX_CONTEXT_QE_ENTRY_DB_SINGLETON)
	{
		/* the pg_usleep() call below is in units of us (microsseconds), interconnect
		 * timeout is in seconds.  Start with 1 millisecond. */
		uint32     sleep_per_check_us = 1 * 1000;
		
		uint32		segmate_timeout_us = (3 * interconnect_setup_timeout * 1000* 1000) / 4;

		uint32	   	total_sleep_time_us = 0;
		uint32		warning_sleep_time_us = 0;
		
		
		for (;;)
		{
			if (SharedLocalSnapshotSlot == NULL)
				elog(ERROR, "SharedLocalSnapshotSlot is NULL");

			if (QEDtxContextInfo.distributedXid == SharedLocalSnapshotSlot->QDxid &&
				QEDtxContextInfo.distributedSnapshotData.curcid== SharedLocalSnapshotSlot->QDcid &&
				SharedLocalSnapshotSlot->ready)
			{
				/*
				 * YAY we found it.  set the contents of the
				 * SharedLocalSnapshot to this and move on.
				 */
				snapshot->xmin = SharedLocalSnapshotSlot->snapshot.xmin;
				snapshot->xmax = SharedLocalSnapshotSlot->snapshot.xmax;
				snapshot->xcnt = SharedLocalSnapshotSlot->snapshot.xcnt;

				/*
				 * We don't need to worry about making our copy of the xip
				 * array since we don't dispatch the next stmt to the WRITER
				 * until everybody finishes then this won't change out from
				 * under us.
				 *
				 * UNDONE: Presumably, we will not try to deallocate snapshot->xip...
				 */
				snapshot->xip = SharedLocalSnapshotSlot->snapshot.xip;

				snapshot->curcid = SharedLocalSnapshotSlot->snapshot.curcid;

				SetSharedTransactionId();

				elog(DEBUG5, "Reader qExec setting shared local snapshot to: xmin: %d xmax: %d curcid: %d",
					 snapshot->xmin, snapshot->xmax, snapshot->curcid);
				
				if (Debug_print_full_dtm)
				{
					ShowSubtransactionsForSharedSnapshot(SharedLocalSnapshotSlot->subcnt,SharedLocalSnapshotSlot->subxids);
				}
	
				return snapshot;
			}
			else
			{
				/*
				 * didn't find it.  we'll sleep for a small amount of time and
				 * then try again.
				 *
				 * TODO: is there a semaphore or something better we can do ehre.
				 */
				pg_usleep(sleep_per_check_us);

				CHECK_FOR_INTERRUPTS();

				warning_sleep_time_us += sleep_per_check_us;
				total_sleep_time_us += sleep_per_check_us;

				if (total_sleep_time_us >= segmate_timeout_us)
				{
					ereport(ERROR,
							(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 errmsg("GetSnapshotData timed out waiting for Writer to set the shared snapshot."),
							 errdetail("We are waiting for the shared snapshot to have XID: %d but the value "
									   "is currently: %d." 
									   " waiting for cid to be %d but is currently %d.  ready=%d."
									   "DistributedTransactionContext = %s. "
									   " Our slotindex is: %d \n"
									   "Dump of all sharedsnapshots in shmem: %s",
									   QEDtxContextInfo.distributedXid, SharedLocalSnapshotSlot->QDxid,
									   QEDtxContextInfo.distributedSnapshotData.curcid, SharedLocalSnapshotSlot->QDcid,  SharedLocalSnapshotSlot->ready,
									   DtxContextToString(DistributedTransactionContext),
									   SharedLocalSnapshotSlot->slotindex, SharedSnapshotDump())));
				}
				else if (warning_sleep_time_us > 1000 * 1000)
				{
					/*
					 * Every second issue warning.
					 */
					elog(LOG,"GetSnapshotData did not find shared local snapshot information. "
							 "We are waiting for the shared snapshot to have XID: %d but the value "
												   "is currently: %d." 
												   " waiting for cid to be %d but is currently %d.  ready=%d."
												   " Our slotindex is: %d \n"
												   "DistributedTransactionContext = %s.",
												   QEDtxContextInfo.distributedXid, SharedLocalSnapshotSlot->QDxid,
												   QEDtxContextInfo.distributedSnapshotData.curcid, SharedLocalSnapshotSlot->QDcid,
												   SharedLocalSnapshotSlot->ready,
												   SharedLocalSnapshotSlot->slotindex,
												   DtxContextToString(DistributedTransactionContext));
					warning_sleep_time_us = 0;
				}

				// UNDONE: Back-off from checking every millisecond...

			}
		}
	}

	/* Serializable snapshot must be computed before any other... */
	Assert(serializable ?
		   !TransactionIdIsValid(MyProc->xmin) :
		   TransactionIdIsValid(MyProc->xmin));

	/*
	 * Allocating space for maxProcs xids is usually overkill; numProcs would
	 * be sufficient.  But it seems better to do the malloc while not holding
	 * the lock, so we can't look at numProcs.  Likewise, we allocate much
	 * more subxip storage than is probably needed.
	 *
	 * This does open a possibility for avoiding repeated malloc/free: since
	 * maxProcs does not change at runtime, we can simply reuse the previous
	 * xip arrays if any.  (This relies on the fact that all callers pass
	 * static SnapshotData structs.)
	 */
	if (snapshot->xip == NULL)
	{
		/*
		 * First call for this snapshot
		 */
		snapshot->xip = (TransactionId *)
			malloc(arrayP->maxProcs * sizeof(TransactionId));
		if (snapshot->xip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		Assert(snapshot->subxip == NULL);
		snapshot->subxip = (TransactionId *)
			malloc(arrayP->maxProcs * PGPROC_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
		if (snapshot->subxip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	globalxmin = xmin = GetTopTransactionId();

	/*
	 * It is sufficient to get shared lock on ProcArrayLock, even if we are
	 * computing a serializable snapshot and therefore will be setting
	 * MyProc->xmin.  This is because any two backends that have overlapping
	 * shared holds on ProcArrayLock will certainly compute the same xmin
	 * (since no xact, in particular not the oldest, can exit the set of
	 * running transactions while we hold ProcArrayLock --- see further
	 * discussion just below).	So it doesn't matter whether another backend
	 * concurrently doing GetSnapshotData or GetOldestXmin sees our xmin as
	 * set or not; he'd compute the same xmin for himself either way.
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	/*--------------------
	 * Unfortunately, we have to call ReadNewTransactionId() after acquiring
	 * ProcArrayLock above.  It's not good because ReadNewTransactionId() does
	 * LWLockAcquire(XidGenLock), but *necessary*.	We need to be sure that
	 * no transactions exit the set of currently-running transactions
	 * between the time we fetch xmax and the time we finish building our
	 * snapshot.  Otherwise we could have a situation like this:
	 *
	 *		1. Tx Old is running (in Read Committed mode).
	 *		2. Tx S reads new transaction ID into xmax, then
	 *		   is swapped out before acquiring ProcArrayLock.
	 *		3. Tx New gets new transaction ID (>= S' xmax),
	 *		   makes changes and commits.
	 *		4. Tx Old changes some row R changed by Tx New and commits.
	 *		5. Tx S finishes getting its snapshot data.  It sees Tx Old as
	 *		   done, but sees Tx New as still running (since New >= xmax).
	 *
	 * Now S will see R changed by both Tx Old and Tx New, *but* does not
	 * see other changes made by Tx New.  If S is supposed to be in
	 * Serializable mode, this is wrong.
	 *
	 * By locking ProcArrayLock before we read xmax, we ensure that TX Old
	 * cannot exit the set of running transactions seen by Tx S.  Therefore
	 * both Old and New will be seen as still running => no inconsistency.
	 *--------------------
	 */

	xmax = ReadNewTransactionId();

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"GetSnapshotData finds Debug_disable_distributed_snapshot = %s",
		 (Debug_disable_distributed_snapshot ? "true" : "false"));
	if (!Debug_disable_distributed_snapshot)
	{
		if (DistributedTransactionContext == DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE &&
			*distributedSnapshot != NULL)
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData going to check CurrentDistributedSnapshot in-doubt xids (count = %d)",
			     distributedSnapshotData->xcnt);
			for (index = 0; index < distributedSnapshotData->xcnt; index++)
			{
				DistributedTransactionId dxid = distributedSnapshotData->xip[index];
				TransactionId xid = GetLocalXidForDistributedTransaction(dxid);

				if (xid != InvalidTransactionId)
				{
					elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData adding CurrentDistributedSnapshot in-doubt xid = %u as local xid = %u",
						 dxid, xid);
					
					if (TransactionIdPrecedes(xid, xmin))
						xmin = xid;
					snapshot->xip[count] = xid;
					count++;
				}
				else
				{
					elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData found CurrentDistributedSnapshot in-doubt xid = %u was invalid",
						 dxid);
				}

				// UNDONE: Unclear how to update globalxmin.
			}
		}
		
		if (DistributedTransactionContext == DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER ||
			DistributedTransactionContext == DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER||
			DistributedTransactionContext == DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT)
		{
			elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData going to check distributedSnapshot in-doubt xids (count = %d)",
			     QEDtxContextInfo.distributedSnapshotData.xcnt);
			for (index = 0; index < QEDtxContextInfo.distributedSnapshotData.xcnt; index++)
			{
				TransactionId dxid = QEDtxContextInfo.distributedSnapshotData.xip[index];
				TransactionId xid = GetLocalXidForDistributedTransaction(dxid);

				if (xid != InvalidTransactionId)
				{
					elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData adding distributedSnapshotData in-doubt xid = %u as local xid = %u",
						 dxid, xid);
					
					if (TransactionIdPrecedes(xid, xmin))
						xmin = xid;
					snapshot->xip[count] = xid;
					count++;
				}
				else
				{
					elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData found distributedSnapshotData in-doubt xid = %u was invalid",
						 dxid);
				}

				// UNDONE: Unclear how to update globalxmin.
			}
		}
	}
	
	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		/* Fetch xid just once - see GetNewTransactionId */
		TransactionId xid = proc->xid;

		/*
		 * Ignore my own proc (dealt with my xid above), procs not running a
		 * transaction, xacts started since we read the next transaction ID,
		 * and xacts executing LAZY VACUUM. There's no need to store XIDs
		 * above what we got from ReadNewTransactionId, since we'll treat them
		 * as running anyway.  We also assume that such xacts can't compute an
		 * xmin older than ours, so they needn't be considered in computing
		 * globalxmin.
		 */
		if (proc == MyProc ||
			!TransactionIdIsNormal(xid) ||
			TransactionIdFollowsOrEquals(xid, xmax) ||
			proc->inVacuum)
			continue;

		if (!Debug_disable_distributed_snapshot)
		{
			/*
			 * Exclude distributed transactions since they were added before
			 * the proc scan for loop.
			 */
			elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData checking proc->pid = %d, xid = %u, mppDistributedTransactionId = %u",
			     proc->pid, xid, proc->mppDistributedTransactionId);
			if (proc->mppDistributedTransactionId != InvalidDistributedTransactionId &&
				(DistributedTransactionContext == DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE ||
			     DistributedTransactionContext == DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER ||
			     DistributedTransactionContext == DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER ||
			     DistributedTransactionContext == DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT))
			{
				elog((Debug_print_full_dtm ? LOG : DEBUG5), "GetSnapshotData excluding local xid = %u for distributed xid = %u",
				     xid, proc->mppDistributedTransactionId);
				continue;
			}
		}
		
		if (TransactionIdPrecedes(xid, xmin))
			xmin = xid;
		snapshot->xip[count] = xid;
		count++;

		/* Update globalxmin to be the smallest valid xmin */
		xid = proc->xmin;
		if (TransactionIdIsNormal(xid))
			if (TransactionIdPrecedes(xid, globalxmin))
				globalxmin = xid;

		/*
		 * Save subtransaction XIDs if possible (if we've already overflowed,
		 * there's no point).  Note that the subxact XIDs must be later than
		 * their parent, so no need to check them against xmin.
		 *
		 * The other backend can add more subxids concurrently, but cannot
		 * remove any.	Hence it's important to fetch nxids just once. Should
		 * be safe to use memcpy, though.  (We needn't worry about missing any
		 * xids added concurrently, because they must postdate xmax.)
		 */
		if (subcount >= 0)
		{
			if (proc->subxids.overflowed)
				subcount = -1;	/* overflowed */
			else
			{
				int			nxids = proc->subxids.nxids;

				if (nxids > 0)
				{
					memcpy(snapshot->subxip + subcount,
						   proc->subxids.xids,
						   nxids * sizeof(TransactionId));
					subcount += nxids;
				}
			}
		}
	}

	if (serializable)
		MyProc->xmin = TransactionXmin = xmin;

	LWLockRelease(ProcArrayLock);

	/*
	 * Update globalxmin to include actual process xids.  This is a slightly
	 * different way of computing it than GetOldestXmin uses, but should give
	 * the same result.
	 */
	if (TransactionIdPrecedes(xmin, globalxmin))
		globalxmin = xmin;

	/* Update global variables too */
	RecentGlobalXmin = globalxmin;
	RecentXmin = xmin;

	snapshot->xmin = xmin;
	snapshot->xmax = xmax;
	snapshot->xcnt = count;
	snapshot->subxcnt = subcount;

	snapshot->curcid = GetCurrentCommandId();
	if (*distributedSnapshot != NULL)
	{
		distributedSnapshotData->curcid = snapshot->curcid;
	}

	/*
	 * MPP Addition.  If we are the chief then we'll save our local snapshot
	 * into the shared snapshot.  Note: we need to use the shared local
	 * snapshot for the "Local Implicit using Distributed Snapshot" case, too.
	 */
	if ((DistributedTransactionContext == DTX_CONTEXT_QE_TWO_PHASE_EXPLICIT_WRITER ||
		 DistributedTransactionContext == DTX_CONTEXT_QE_TWO_PHASE_IMPLICIT_WRITER ||
		 DistributedTransactionContext == DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT) &&
		 SharedLocalSnapshotSlot != NULL)
	{
		updateSharedLocalSnapshot(&QEDtxContextInfo, snapshot);
	}

	return snapshot;
}

/*
 * DatabaseHasActiveBackends -- are there any backends running in the given DB
 *
 * If 'ignoreMyself' is TRUE, ignore this particular backend while checking
 * for backends in the target database.
 *
 * This function is used to interlock DROP DATABASE against there being
 * any active backends in the target DB --- dropping the DB while active
 * backends remain would be a Bad Thing.  Note that we cannot detect here
 * the possibility of a newly-started backend that is trying to connect
 * to the doomed database, so additional interlocking is needed during
 * backend startup.
 */
bool
DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->databaseId == databaseId)
		{
			if (ignoreMyself && proc == MyProc)
				continue;

			result = true;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * BackendPidGetProc -- get a backend's PGPROC given its PID
 *
 * Returns NULL if not found.  Note that it is up to the caller to be
 * sure that the question remains meaningful for long enough for the
 * answer to be used ...
 */
PGPROC *
BackendPidGetProc(int pid)
{
	PGPROC	   *result = NULL;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	if (pid == 0)				/* never match dummy PGPROCs */
		return NULL;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->pid == pid)
		{
			result = proc;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * BackendXidGetPid -- get a backend's pid given its XID
 *
 * Returns 0 if not found or it's a prepared transaction.  Note that
 * it is up to the caller to be sure that the question remains
 * meaningful for long enough for the answer to be used ...
 *
 * Only main transaction Ids are considered.  This function is mainly
 * useful for determining what backend owns a lock.
 */
int
BackendXidGetPid(TransactionId xid)
{
	int			result = 0;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	if (xid == InvalidTransactionId)	/* never match invalid xid */
		return 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->xid == xid)
		{
			result = proc->pid;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * IsBackendPid -- is a given pid a running backend
 */
bool
IsBackendPid(int pid)
{
	return (BackendPidGetProc(pid) != NULL);
}

/*
 * CountActiveBackends --- count backends (other than myself) that are in
 *		active transactions.  This is used as a heuristic to decide if
 *		a pre-XLOG-flush delay is worthwhile during commit.
 *
 * Do not count backends that are blocked waiting for locks, since they are
 * not going to get to run until someone else commits.
 */
int
CountActiveBackends(void)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/*
	 * Note: for speed, we don't acquire ProcArrayLock.  This is a little bit
	 * bogus, but since we are only testing fields for zero or nonzero, it
	 * should be OK.  The result is only used for heuristic purposes anyway...
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc == MyProc)
			continue;			/* do not count myself */
		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->xid == InvalidTransactionId)
			continue;			/* do not count if not in a transaction */
		if (proc->waitLock != NULL)
			continue;			/* do not count if blocked on a lock */
		count++;
	}

	return count;
}

/*
 * CountDBBackends --- count backends that are using specified database
 */
int
CountDBBackends(Oid databaseid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->databaseId == databaseid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}

/*
 * CountUserBackends --- count backends that are used by specified user
 */
int
CountUserBackends(Oid roleid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = arrayP->procs[index];

		if (proc->pid == 0)
			continue;			/* do not count prepared xacts */
		if (proc->roleId == roleid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}


#define XidCacheRemove(i) \
	do { \
		MyProc->subxids.xids[i] = MyProc->subxids.xids[MyProc->subxids.nxids - 1]; \
		MyProc->subxids.nxids--; \
	} while (0)

/*
 * XidCacheRemoveRunningXids
 *
 * Remove a bunch of TransactionIds from the list of known-running
 * subtransactions for my backend.	Both the specified xid and those in
 * the xids[] array (of length nxids) are removed from the subxids cache.
 */
void
XidCacheRemoveRunningXids(TransactionId xid, int nxids, TransactionId *xids)
{
	int			i,
				j;

	Assert(TransactionIdIsValid(xid));

	/*
	 * We must hold ProcArrayLock exclusively in order to remove transactions
	 * from the PGPROC array.  (See notes in GetSnapshotData.)	It's possible
	 * this could be relaxed since we know this routine is only used to abort
	 * subtransactions, but pending closer analysis we'd best be conservative.
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * Under normal circumstances xid and xids[] will be in increasing order,
	 * as will be the entries in subxids.  Scan backwards to avoid O(N^2)
	 * behavior when removing a lot of xids.
	 */
	for (i = nxids - 1; i >= 0; i--)
	{
		TransactionId anxid = xids[i];

		for (j = MyProc->subxids.nxids - 1; j >= 0; j--)
		{
			if (TransactionIdEquals(MyProc->subxids.xids[j], anxid))
			{
				XidCacheRemove(j);
				break;
			}
		}

		/*
		 * Ordinarily we should have found it, unless the cache has
		 * overflowed. However it's also possible for this routine to be
		 * invoked multiple times for the same subtransaction, in case of an
		 * error during AbortSubTransaction.  So instead of Assert, emit a
		 * debug warning.
		 */
		if (j < 0 && !MyProc->subxids.overflowed)
			elog(WARNING, "did not find subXID %u in MyProc", anxid);
	}

	for (j = MyProc->subxids.nxids - 1; j >= 0; j--)
	{
		if (TransactionIdEquals(MyProc->subxids.xids[j], xid))
		{
			XidCacheRemove(j);
			break;
		}
	}
	/* Ordinarily we should have found it, unless the cache has overflowed */
	if (j < 0 && !MyProc->subxids.overflowed)
		elog(WARNING, "did not find subXID %u in MyProc", xid);

	LWLockRelease(ProcArrayLock);
}

#ifdef XIDCACHE_DEBUG

/*
 * Print stats about effectiveness of XID cache
 */
static void
DisplayXidCache(void)
{
	fprintf(stderr,
			"XidCache: xmin: %ld, mainxid: %ld, childxid: %ld, slow: %ld\n",
			xc_by_recent_xmin,
			xc_by_main_xid,
			xc_by_child_xid,
			xc_slow_answer);
}

#endif   /* XIDCACHE_DEBUG */

PGPROC *FindProcByGpSessionId(long gp_session_id)
{
		/* Find the guy who should manage our locks */
		ProcArrayStruct *arrayP = procArray;
		int			index;

		Assert(gp_session_id > 0);
		
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

		for (index = 0; index < arrayP->numProcs; index++)
		{
			PGPROC	   *proc = arrayP->procs[index];
			
			if (proc->pid == MyProc->pid)
				continue;
				
			if (!proc->mppIsWriter)
				continue;
				
			//if (proc->pid == 0)
			//	continue;			 /* skip prepared transactions ? */
			if (proc->mppSessionId == gp_session_id)
			{
				 
				LWLockRelease(ProcArrayLock);
				return proc;
			}
		}
		
		LWLockRelease(ProcArrayLock);
		return NULL;
	}
