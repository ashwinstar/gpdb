/*-------------------------------------------------------------------------
 *
 * resqueue.c
 *	  POSTGRES internals code for resource queues and locks.
 *
 *
 * Copyright (c) 2006, Greenplum, Inc.
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include "pgstat.h"
#include "access/heapam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lock.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/resscheduler.h"


/* Columns in pg_resqueue_status() */
#define	NUM_QUEUE_STATUS_ELEM	(3 + NUM_RES_LIMIT_TYPES)

/*
 * Global Variables
 */
static HTAB *ResPortalIncrementHash;		/* Hash of resource increments. */
static HTAB *ResQueueHash;					/* Hash of resource queues. */


/*
 * Record structure holding the to be exposed per queue data, used by
 * pg_resqueue_status().
 */
typedef struct
{
	Oid		queueid;
	float4	queuecountthreshold;
	float4	queuecostthreshold;
	float4	queuecountvalue;
	float4	queuecostvalue;
	int		queuewaiters;
	int		queueholders;
}	QueueStatusRec;


/*
 * Function context for data persisting over repeated calls, used by 
 * pg_resqueue_status().
 */
typedef struct
{
	TupleDesc		tupdesc;
	QueueStatusRec	*record;
	
}	QueueStatusContext;


/*
 * ResLockAcquire -- acquire a resource lock.
 *
 * Notes and critisms:
 *
 *	Returns LOCKACQUIRE_OK.
 *
 *	Analogous to LockAcquire, but the lockmode and session boolean are not
 *	required in the function prototype as we are *always* lockmode ExclusiveLock
 *	and have no session locks. 
 *
 *	The semantics of resource locks mean that lockmode has minimal meaning -
 *	the conflict rules are determined by the state of the counters of the
 *	corresponding queue. We are maintaining the lock lockmode and related
 *	elements (holdmask etc), in order to ease comparison with standard locks
 *	at deadlock check time (well, so we hope anyway.)
 *
 */
LockAcquireResult
ResLockAcquire(LOCKTAG *locktag, ResPortalIncrement *incrementSet)
{
	LOCKMODE		lockmode = ExclusiveLock;
	LOCK			*lock;
	PROCLOCK		*proclock;
	PROCLOCKTAG		proclocktag;
	LOCALLOCKTAG	localtag;
	LOCALLOCK		*locallock;
	uint32			hashcode;
	uint32			proclock_hashcode;
	int				partition;
	LWLockId		partitionLock;
	bool			found;
	ResourceOwner	owner;
	int				status;

	/* Setup the lock method bits. */
	Assert(locktag->locktag_lockmethodid = RESOURCE_LOCKMETHOD);
	
	/* Provide a resource owner. */
	owner = CurrentResourceOwner;

	/*
	* Find or create a LOCALLOCK entry for this lock and lockmode
	*/
	MemSet(&localtag, 0, sizeof(localtag));     /* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  (void *) &localtag,
										  HASH_ENTER, &found);

	/*
	* if it's a new locallock object, initialize it, if it already exists
	* then that is enough for the resource locks.
	*/
	if (!found)
	{
		locallock->lock = NULL;
		locallock->proclock = NULL;
		locallock->hashcode = LockTagHashCode(&(localtag.lock));
		locallock->nLocks = 0;
		locallock->numLockOwners = 0;
		locallock->maxLockOwners = 8;
		locallock->lockOwners = NULL;
		locallock->lockOwners = (LOCALLOCKOWNER *)
		MemoryContextAlloc(TopMemoryContext,
		locallock->maxLockOwners * sizeof(LOCALLOCKOWNER));
	}

	/* We are going to examine the shared lock table. */
	hashcode = locallock->hashcode;
	partition = LockHashPartition(hashcode);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * 	Find or create a lock with this tag.
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												(void *) locktag,
												hashcode,
												HASH_ENTER_NULL,
												&found);

	 if (!lock)
	 {
	 	LWLockRelease(partitionLock);
		ereport(ERROR,
			  	(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You may need to increase max_resource_qeueues.")));
	}
	locallock->lock = lock;

	/*
	 * if it's a new lock object, initialize it.
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		SHMQueueInit(&(lock->procLocks));
		ProcQueueInit(&(lock->waitProcs));
		lock->nRequested = 0;
		lock->nGranted = 0;
		MemSet(lock->requested, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet(lock->granted, 0, sizeof(int) * MAX_LOCKMODES);
	}
	else
	{
		Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
		Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
		Assert(lock->nGranted <= lock->nRequested);
	}
	
	/*
	 *  Create the hash key for the proclock table.
	 */
	MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));	/* Clear padding.*/
	proclocktag.myLock = lock;
	proclocktag.myProc = MyProc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	/*
	 * Find or create a proclock entry with this tag. 
	 */ 
	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														(void *) &proclocktag,
														proclock_hashcode,
														HASH_ENTER_NULL,
														&found);
    if (!proclock)
	{
		/* Not enough shmem for the proclock. */
		if (lock->nRequested == 0)
		{
			/*
			 * There are no other requestors of this lock, so garbage-collect
			 * the lock object.  We *must* do this to avoid a permanent leak
			 * of shared memory, because there won't be anything to cause
			 * anyone to release the lock object later.
			 */
			Assert(SHMQueueEmpty(&(lock->procLocks)));
			if (!hash_search_with_hash_value(LockMethodLockHash,
											 (void *) &(lock->tag),
											 hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			errhint("You may need to increase max_resource_qeueues.")));
	}
	locallock->proclock = proclock;

	/*
	 * If new, initialize the new entry.
	 */
	if (!found)
	{
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* Add proclock to appropriate lists */
		SHMQueueInsertBefore(&lock->procLocks, &proclock->lockLink);
		SHMQueueInsertBefore(&(MyProc->myProcLocks[partition]), &proclock->procLink);
		proclock->nLocks = 0;
		SHMQueueInit(&(proclock->portalLinks));
	}
	else
	{
		Assert((proclock->holdMask & ~lock->grantMask) == 0);
		/* Could do a deadlock risk check here. */
	}

	/*
	 * lock->nRequested and lock->requested[] count the total number of
	 * requests, whether granted or waiting, so increment those immediately.
	 * The other counts don't increment till we get the lock.
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/*
	 * Add an increment to the increment hash for this process
	 */
	LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

	incrementSet = ResIncrementAdd(incrementSet, proclock, owner);
	if (!incrementSet)
	{
		LWLockRelease(ResQueueLock);
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory adding portal increments"),
			errhint("You may need to increase max_resource_portals_per_transaction.")));
	}


	/*
	 * Check if the lock can be acquired (i.e. if the resource the lock and 
	 * queue control is not exhausted). 
	 */

	status = ResLockCheckLimit(lock, proclock, incrementSet, true);
	if (status == STATUS_ERROR)
	{
		/*
		 * The requested lock has individual increments that are larger than
		 * some of the thresholds for the corrosponding queue, and overcommit
		 * is not enabled for them. So abort and clean up.
		 */
		ResPortalTag		portalTag;

		/* Adjust the counters as we no longer want this lock. */
		lock->nRequested--;
		lock->requested[lockmode]--;
		Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));

		/* Clean up this lock. */
		RemoveLocalLock(locallock);
		ResCleanUpLock(lock, proclock, hashcode);

		/* Kill off the increment. */
		MemSet(&portalTag, 0, sizeof(ResPortalTag));
		portalTag.pid = incrementSet->pid;
		portalTag.portalId = incrementSet->portalId;
		ResIncrementRemove(&portalTag);

		LWLockRelease(ResQueueLock);
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("statement requires more resources than resource queue allows")));
	}
	else if (status ==  STATUS_OK)
	{
		/* 
		 * The requested lock will *not* exhaust the limit for this resource
		 * queue, so record this in the local lock hash, and grant it.
		 */

		ResGrantLock(lock, proclock);
		ResLockUpdateLimit(lock, proclock, incrementSet, true);

		LWLockRelease(ResQueueLock);

		/* Note the start time for queue statistics. */
		pgstat_record_start_queue_exec(incrementSet->portalId,
									   locktag->locktag_field1);

	}
	else
	{
		Assert(status == STATUS_FOUND);

		/*
		 * The requested lock will exhaust the limit for this resource queue,
		 * so must wait.
		 */

		/* Set bitmask of locks this process already holds on this object. */
		MyProc->heldLocks = proclock->holdMask; /* Do we need to do this?*/
		

		/* 
		 * Set the portal id so we can identify what increments we are wanting
		 * to apply at wakeup.
		 */
		MyProc->waitPortalId = incrementSet->portalId;

		LWLockRelease(ResQueueLock);

		/* Note count and wait time for queue statistics. */
		pgstat_count_queue_wait(incrementSet->portalId,
								locktag->locktag_field1);
		pgstat_record_start_queue_wait(incrementSet->portalId,
									   locktag->locktag_field1);

		/*
		 * Sleep till someone wakes me up.
		 */
		ResWaitOnLock(locallock, owner);

		/*
		 * Have been awakened, check state is consistent.
		 */
		if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
		{
			LWLockRelease(partitionLock);
			elog(ERROR, "ResLockAcquire failed");
		}

		/* Reset the portal id. */
		MyProc->waitPortalId = INVALID_PORTALID;

		/* End wait time and start execute time statistics for this queue. */
		pgstat_record_end_queue_wait(incrementSet->portalId,
									 locktag->locktag_field1);
		pgstat_record_start_queue_exec(incrementSet->portalId,
									   locktag->locktag_field1);

	}


	/* Release the  partition lock. */
	LWLockRelease(partitionLock);

	return LOCKACQUIRE_OK;
}


/*
 * ResLockRelease -- release a resource lock.
 */
bool
ResLockRelease(LOCKTAG *locktag, uint32 resPortalId)
{
	LOCKMODE		lockmode = ExclusiveLock;
	LOCK			*lock;
	PROCLOCK		*proclock;
	LOCALLOCKTAG	localtag;
	LOCALLOCK		*locallock;
	uint32			hashcode;
	LWLockId		partitionLock;
	ResourceOwner	owner;
	/*int				status;*/
	ResPortalIncrement	*incrementSet;
	ResPortalTag		portalTag;

	/* Check the lock method bits. */
	Assert(locktag->locktag_lockmethodid == RESOURCE_LOCKMETHOD);

	/* Provide a resource owner. */
	owner = CurrentResourceOwner;

	/*
	 * Find the LOCALLOCK entry for this lock and lockmode
	 */
	MemSet(&localtag, 0, sizeof(localtag));		/* must clear padding */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  (void *) &localtag,
										  HASH_FIND, NULL);

	/*
	 * let the caller print its own error message, too. Do not ereport(ERROR).
	 */
	if (!locallock)
	{
		elog(WARNING, "you don't own a lock on queue %d",
		locktag->locktag_field1);
		return FALSE;
	}

	hashcode = locallock->hashcode;

	/* We are going to examine the shared lock table. */
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * We don't need to re-find the lock or proclock, since we kept their
	 * addresses in the locallock table, and they couldn't have been removed
	 * while we were holding a lock on them.
	 */
	lock = locallock->lock;
	proclock = locallock->proclock;
	
	Assert((lock != NULL) && (proclock != NULL));

	/*
	 * Double-check that we are actually holding a lock of the type we want to
	 * Release.
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		LWLockRelease(partitionLock);
		elog(ERROR, "you don't hold a lock of type %d on queue %d", 
					lockmode, locktag->locktag_field1);
		RemoveLocalLock(locallock);
		return FALSE;
			
	}

	/*
	 * Find the increment for this portal and process.
	 */
	MemSet(&portalTag, 0, sizeof(ResPortalTag));
	portalTag.pid = MyProc->pid;
	portalTag.portalId = resPortalId;
	
	LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

	incrementSet = ResIncrementFind(&portalTag);
	if (!incrementSet)
	{
		LWLockRelease(ResQueueLock);
		LWLockRelease(partitionLock);
		elog(ERROR, "no increment data for portal id %u and pid %d",
			 resPortalId, MyProc->pid);
	}


	/*
	 * Un-grant the lock.
	 */
	ResUnGrantLock(lock, proclock);
	ResLockUpdateLimit(lock, proclock, incrementSet, false);

	/* Remove the locallock if we are going to remove the shared one. */
	if(lock->nRequested == 0)
		RemoveLocalLock(locallock);

	/*
	 * Perform clean-up, waking up any waiters!
	 */
	ResCleanUpLock(lock, proclock, hashcode);

	/* 
	 * Clean up the increment set. 
	 */
	if (!ResIncrementRemove(&portalTag))
	{
		LWLockRelease(ResQueueLock);
		LWLockRelease(partitionLock);
		elog(ERROR, "no increment to remove for portal id %u and pid %d",
			 resPortalId, MyProc->pid);
	}

	LWLockRelease(ResQueueLock);
	LWLockRelease(partitionLock);

	/* Update execute statistics for this queue, count and elapsed time. */
	pgstat_count_queue_exec(resPortalId, locktag->locktag_field1);
	pgstat_record_end_queue_exec(resPortalId, locktag->locktag_field1);
		
	return true;
}


/*
 * ResLockCheckLimit -- test whether the given process acquiring the this lock 
 *	will cause a resource to exceed its limits.
 * 
 * Notes:
 *	Returns STATUS_FOUND if limit will be exhausted, STATUS_OK if not.
 *
 *	If increment is true, then the resource counter associated with the lock 
 *	is to be incremented, if false then decremented.
 *
 *	Named similarly to the LockCheckconflicts() for standard locks, but it is
 *	not checking a table of lock mode conflicts, but whether a shared counter
 *	for some resource is exhausted.
 *
 *	The resource queue lightweight lock (ResQueueLock) must be held while
 *	this function is called.
 *
 */
int
ResLockCheckLimit(LOCK *lock, PROCLOCK *proclock, 
				  ResPortalIncrement *incrementSet, bool increment)
{
	ResQueue		queue;
	ResLimit		limits;
	bool			over_limit = false;
	bool			will_overcommit = false;
	int				status = STATUS_OK;
	Cost			increment_amt;
	int				i;


	/* Get the queue for this lock.*/
	queue = GetResQueueFromLock(lock);
	limits = queue->limits;
	
	for (i = 0; i < NUM_RES_LIMIT_TYPES; i++)
	{
		/*
		 * Skip the default threshold, as it means 'no limit'.
		 */
		if (limits[i].threshold_value == INVALID_RES_LIMIT_THRESHOLD)
			continue;
			
		switch (limits[i].type)
		{
			case RES_COUNT_LIMIT:
			{
				Assert((limits[i].threshold_is_max));
				/* Setup whether to increment or decrement the # active. */
				if (increment)
				{
					increment_amt = incrementSet->increments[i];
				}
				else
				{
					increment_amt = -1 * incrementSet->increments[i];
				}
				if (limits[i].current_value + increment_amt > 
					limits[i].threshold_value)
					over_limit = true;

				elog(DEBUG1, "checking count limit threshold %.0f current %.0f",
					 limits[i].threshold_value, limits[i].current_value);
			}
			break;

			case RES_COST_LIMIT:
			{

				Assert((limits[i].threshold_is_max));
				/* Setup whether to increment or decrement the cost. */
				if (increment)
				{
					increment_amt = incrementSet->increments[i];
				}
				else
				{
					increment_amt = -1 * incrementSet->increments[i];
				}

				/* Check if this will overcommit */
				if (increment_amt > limits[i].threshold_value)
					will_overcommit = true;

				if (queue->overcommit)
				{
					/*
					 * Autocommit is enabled, allow statements that
					 * blowout the limit if noone else is active!
					 */
					if ((limits[i].current_value + increment_amt > 
						limits[i].threshold_value) &&
						(limits[i].current_value > 0.1))
						over_limit = true;
				} 
				else
				{
					/*
					 * No autocommit, so always fail statements that
					 * blowout the limit.
					 */
					if (limits[i].current_value + increment_amt > 
						limits[i].threshold_value)
						over_limit = true;
				}

				elog(DEBUG1, "checking cost limit threshold %.2f current %.2f",
					 limits[i].threshold_value, limits[i].current_value);
			}
			break;

			default:
			break;
		
		}
	}

	if (will_overcommit && !queue->overcommit)
		status = STATUS_ERROR;
	else if (over_limit)
		status = STATUS_FOUND;

	return status;

}


/*
 * ResLockUpdateLimit -- update the resource counter for this lock with the
 *	increment for the process.
 *
 * Notes:
 *	If increment is true, then the resource counter associated with the lock 
 *	is to be incremented, if false then decremented.
 *
 * Warnings:
 *	The resource queue lightweight lock (ResQueueLock) must be held while
 *	this function is called.
 */
void
ResLockUpdateLimit(LOCK *lock, PROCLOCK *proclock, 
				   ResPortalIncrement *incrementSet, bool increment)
{
	ResQueue 		queue;
	ResLimit		limits;
	Cost			increment_amt;
	int				i;


	/* Get the queue for this lock.*/
	queue = GetResQueueFromLock(lock);
	limits = queue->limits;

	for (i = 0; i < NUM_RES_LIMIT_TYPES; i++)
	{
		/*
		 * Skip the default threshold, as it means 'no limit'.
		 */
		if (limits[i].threshold_value == INVALID_RES_LIMIT_THRESHOLD)
			continue;


		switch (limits[i].type)
		{
			case RES_COUNT_LIMIT:
			{
				Assert((limits[i].threshold_is_max));
				/* setup whether to increment or decrement the # active. */
				if (increment)
				{
					increment_amt = incrementSet->increments[i];
				}
				else
				{
					increment_amt = -1 * incrementSet->increments[i];
				}
				limits[i].current_value += increment_amt;
				Assert((limits[i].current_value <= limits[i].threshold_value));
				elog(DEBUG1, "setting count limit threshold %.0f current %.0f",
					 limits[i].threshold_value, limits[i].current_value);
			}
			break;

			case RES_COST_LIMIT:
			{
	
				Assert((limits[i].threshold_is_max));
				/* setup whether to increment or decrement the cost. */
				if (increment)
				{
					increment_amt = incrementSet->increments[i];
				}
				else
				{
					increment_amt = -1 * incrementSet->increments[i];
				}
				limits[i].current_value += increment_amt;
				Assert((limits[i].current_value <= limits[i].threshold_value) ||
					   ((limits[i].current_value - increment_amt < 0.1)
						&& queue->overcommit) ||
						((increment_amt - limits[i].current_value < 0.1)
						&& queue->overcommit));

				elog(DEBUG1, "setting cost limit threshold %.2f current %.2f",
					 limits[i].threshold_value, limits[i].current_value);
			}
			break;

			default:
			break;
		}
	}

	return;
}


/*
 * GetResQueueFromLock -- find the resource queue for a given lock;
 *
 * Notes:
 *	should be handed a locktag containing a valid queue id.
 * 	should hold the resource queue lightweight lock during this operation
 */
ResQueue
GetResQueueFromLock(LOCK *lock)
{
	
	ResQueue	queue = ResQueueHashFind(GET_RESOURCE_QUEUEID_FOR_LOCK(lock));

	if (queue == NULL)
		elog(ERROR, "cannot find queue id %d", 
					GET_RESOURCE_QUEUEID_FOR_LOCK(lock));

	return queue;
}


/*
 * ResGrantLock -- grant a resource lock. 
 *
 * Warnings:
 *	It is expected that the partition lock is held before calling this 
 *  function, as the various shared queue counts are inspected.
 */
void
ResGrantLock(LOCK *lock, PROCLOCK *proclock)
{
	
	LOCKMODE	lockmode = ExclusiveLock;

	/* Update the standard lock stuff, for locks and proclocks. */
	lock->nGranted++;  
	lock->granted[lockmode]++;
	lock->grantMask |= LOCKBIT_ON(lockmode);
	if (lock->granted[lockmode] == lock->requested[lockmode])
		lock->waitMask &= LOCKBIT_OFF(lockmode);	/* no more waiters.*/
	proclock->holdMask |= LOCKBIT_ON(lockmode);

	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);


	/* Update the holders count. */
	proclock->nLocks++;

	return;
}


/*
 * ResUnGrantLock --  opposite of ResGrantLock.
 *
 * Notes:
 *	The equivalant standard lock funtion returns true only if there are waiters,
 *	we don't do this.
 *
 * Warnings:
 *	It is expected that the partition lock held before calling this 
 *  function, as the various shared queue counts are inspected.
 */
bool
ResUnGrantLock(LOCK *lock, PROCLOCK *proclock)
{
	LOCKMODE	lockmode = ExclusiveLock;

	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));
	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);

	/* Update the standard lock stuff. */
	lock->nRequested--;
	lock->requested[lockmode]--;
	lock->nGranted--;
	lock->granted[lockmode]--;

	if (lock->granted[lockmode] == 0)
	{
		/* change the conflict mask.  No more of this lock type. */
		lock->grantMask &= LOCKBIT_OFF(lockmode);
	}

	/* Update the holders count. */
	proclock->nLocks--;

	/* Fix the per-proclock state. */
	if (proclock->nLocks == 0)
		proclock->holdMask &= LOCKBIT_OFF(lockmode);


	return true;
}


/*
 * ResCleanUpLock -- lock cleanup, remove entry from lock queues and start
 *	waking up waiters.
 */
void
ResCleanUpLock(LOCK *lock, PROCLOCK *proclock, uint32 hashcode)
{
	/*LOCKMODE	lockmode = ExclusiveLock; */

	/*
	 * If this was my last hold on this lock, delete my entry in the proclock
	 * table.
	 */
	if (proclock->holdMask == 0 && proclock->nLocks == 0)
	{
		uint32		proclock_hashcode;

		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		proclock_hashcode = ProcLockHashCode(&proclock->tag, hashcode);
		if (!hash_search_with_hash_value(LockMethodProcLockHash,
						 				 (void *) &(proclock->tag),
						 			 	 proclock_hashcode,
										 HASH_REMOVE,
										 NULL))
			elog(PANIC, "proclock table corrupted");
	}
   
	if (lock->nRequested == 0)
	{
		/*
		 * The caller just released the last lock, so garbage-collect the lock
		 * object.
		 */
		Assert(SHMQueueEmpty(&(lock->procLocks)));
		if (!hash_search(LockMethodLockHash,
						 (void *) &(lock->tag),
						 HASH_REMOVE, NULL))
			 elog(PANIC, "lock table corrupted");

	}

	/*
	 * Awaken any waiters (we don't check if there are any)
	 */
	ResProcLockWakeup(lock);

	return;
}


/*
 * WaitOnResLock -- wait to acquire a resource lock.
 *
 * 
 * Warnings:
 *	It is expected that the partition lock is held before calling this 
 *  function, as the various shared queue counts are inspected.
 */
void
ResWaitOnLock(LOCALLOCK *locallock, ResourceOwner owner)
{

	uint32      hashcode = locallock->hashcode;
	LWLockId    partitionLock = LockHashPartitionLock(hashcode);
	const char	*old_status;
	char		*new_status = NULL;
	int			len;

	/* Report change to waiting status */
	if (update_process_title)
	{
		old_status = get_ps_display(&len);
		new_status = (char *) palloc(len + 8 + 1);
		memcpy(new_status, old_status, len);
		strcpy(new_status + len, " queuing");
		set_ps_display(new_status, false);	/* truncate off " queuing" */
		new_status[len] = '\0';
	}
	pgstat_report_waiting(true);

	awaitedLock = locallock;
	awaitedOwner = owner;

	/*
	 * Now sleep.
	 */
	if (ResProcSleep(ExclusiveLock, locallock) != STATUS_OK)
	{
		/*
		 * We failed as a result of a deadlock, see CheckDeadLock(). Quit now.
		 */
		LWLockRelease(partitionLock);
		DeadLockReport();
	}

	awaitedLock = NULL;

	/* Report change to non-waiting status */
	if (update_process_title)
	{
		set_ps_display(new_status, false);
		pfree(new_status);
	}
	pgstat_report_waiting(false);

	return;
}


/*
 * ResProcLockWakeup -- awaken any processses waiting on a resource lock.
 *
 * Notes:
 *	Need to only awaken enough as many waiters as the resource controlled by 
 *	the the lock should allow!
 */
void
ResProcLockWakeup(LOCK *lock)
{
	PROC_QUEUE	*waitQueue = &(lock->waitProcs);
	int			queue_size = waitQueue->size;
	PGPROC		*proc;
	uint32		hashcode;
	LWLockId	partitionLock;
	/*LOCKMASK	aheadRequests = 0; */
	int			status;
	Assert(queue_size >= 0);
	if (queue_size == 0)
		return;

	proc = (PGPROC *) MAKE_PTR(waitQueue->links.next);


	while (queue_size-- > 0)
	{

		/*
		 * Get the portal we are waiting on, and then its set of increments.
		 */
		ResPortalTag			portalTag;
		ResPortalIncrement		*incrementSet;

		MemSet(&portalTag, 0, sizeof(ResPortalTag));
		portalTag.pid = proc->pid;
		portalTag.portalId = proc->waitPortalId;
	
		incrementSet = ResIncrementFind(&portalTag);
		if (!incrementSet)
		{
			hashcode = LockTagHashCode(&(lock->tag));
			partitionLock = LockHashPartitionLock(hashcode);

			LWLockRelease(partitionLock);
			elog(ERROR, "no increment data for  portal id %u and pid %d",
				 proc->waitPortalId, proc->pid);
		}


		/*
		 * See if it is ok to wake this guy. (note that the wakeup writes to
		 * the wait list, and gives back a *new* next proc).
		 */
		status = ResLockCheckLimit(lock, (PROCLOCK *) proc->waitProcLock, incrementSet, true);
		if (status == STATUS_OK)
		{
			ResGrantLock(lock, (PROCLOCK *) proc->waitProcLock);
			ResLockUpdateLimit(lock, (PROCLOCK *) proc->waitProcLock, incrementSet, true);

			proc = ResProcWakeup(proc, STATUS_OK);

		}
		else
		/* Otherwise move on to the next guy. */
		{
			proc = (PGPROC *) MAKE_PTR(proc->links.next);
		}

	}


	Assert(waitQueue->size >= 0);
	
	return;
}


/*
 * ResProcWakeup -- wake a sleeping process.
 *
 * (could we just use ProcWakeup here?)
 */
PGPROC *
ResProcWakeup(PGPROC *proc, int waitStatus)
{
	PGPROC		*retProc;
	/* Proc should be sleeping ... */
	if (proc->links.prev == INVALID_OFFSET ||
		proc->links.next == INVALID_OFFSET)
		return NULL;

	/* Save next process before we zap the list link */
	retProc = (PGPROC *) MAKE_PTR(proc->links.next);

	/* Remove process from wait queue */
	SHMQueueDelete(&(proc->links));
	(proc->waitLock->waitProcs.size)--;

	/* Clean up process' state and pass it the ok/fail signal */
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	proc->waitStatus = waitStatus;

	/* And awaken it */
	 PGSemaphoreUnlock(&proc->sem);

	return retProc;
}


/*
 * ResRemoveFromWaitQueue -- Remove a process from the wait queue, cleaing up
 *	any locks.
 */
void
ResRemoveFromWaitQueue(PGPROC *proc, uint32 hashcode)
{
    LOCK			*waitLock = proc->waitLock;
    PROCLOCK		*proclock = proc->waitProcLock;
    LOCKMODE		lockmode = proc->waitLockMode;
    LOCKMETHODID	lockmethodid = LOCK_LOCKMETHOD(*waitLock);
	ResPortalTag	portalTag;

	/* Make sure lockmethod is for a resource lock. */
	Assert(lockmethodid == RESOURCE_LOCKMETHOD);

    /* Make sure proc is waiting */
	Assert(proc->links.next != INVALID_OFFSET);
	Assert(waitLock);
	Assert(waitLock->waitProcs.size > 0);

	/* Remove proc from lock's wait queue */
	SHMQueueDelete(&(proc->links));
	waitLock->waitProcs.size--;

	/* Undo increments of request counts by waiting process */
	Assert(waitLock->nRequested > 0);
	Assert(waitLock->nRequested > proc->waitLock->nGranted);
	waitLock->nRequested--;
	Assert(waitLock->requested[lockmode] > 0);
	waitLock->requested[lockmode]--;
	/* don't forget to clear waitMask bit if appropriate */
	if (waitLock->granted[lockmode] == waitLock->requested[lockmode])
		waitLock->waitMask &= LOCKBIT_OFF(lockmode);

	/* Clean up the proc's own state */
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;

	/*
	 * Remove the waited on portal increment.
	 */
	MemSet(&portalTag, 0, sizeof(ResPortalTag));
	portalTag.pid = MyProc->pid;
	portalTag.portalId = MyProc->waitPortalId;
	ResIncrementRemove(&portalTag);

	/*
	 * Delete the proclock immediately if it represents no already-held 
	 * locks.
	 * (This must happen now because if the owner of the lock decides to
	 * release it, and the requested/granted counts then go to zero,
	 * LockRelease expects there to be no remaining proclocks.) Then see if
     * any other waiters for the lock can be woken up now.
   	 */
   	ResCleanUpLock(waitLock, (PROCLOCK *) proclock, hashcode);
}


/*
 * ResCheckSelfDeadLock -- Check to see if I am going to deadlock myself. 
 *
 * What happens here is we scan our own set of portals and total up the
 * increments. If this exceeds any of the thresholds for the queue then
 * we need to signal that a self deadlock is about to occurr.
 */
bool
ResCheckSelfDeadLock(LOCK *lock)
{
	ResQueue		queue;
	ResLimit		limits;
	int				i;
	Cost			incrementTotals[NUM_RES_LIMIT_TYPES];
	bool			result = false;

	/* Get the resource queue lock before checking the increments.*/
	LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

	/* Get the queue for this lock.*/
	queue = GetResQueueFromLock(lock);
	limits = queue->limits;

	/* Get the increment totals for this queue. */
	TotalResPortalIncrements(MyProc->pid, queue->queueid, incrementTotals);
	
	/* 
	 * Now check them against the thresholds using the same logic as
	 * ResLockCheckLimit.
	 */
	for (i = 0; i < NUM_RES_LIMIT_TYPES; i++)
	{
		if (limits[i].threshold_value == INVALID_RES_LIMIT_THRESHOLD)
			continue;

		switch (limits[i].type)
		{
			case RES_COUNT_LIMIT:
			{
				if (incrementTotals[i] > limits[i].threshold_value)
					result = true;
			}
			break;

			case RES_COST_LIMIT:
			{
				if (incrementTotals[i] > limits[i].threshold_value)
					result = true;
			}
			break;
		}
	
	}

	LWLockRelease(ResQueueLock);

	return result;

}


/*
 * ResPortalIncrementHashTableInit - Initialize the increment hash.
 *
 * Notes:
 *	This stores the possible increments that a given statement will cause to
 *	be added to the limits for a resource queue.
 *	We allocate one extra slot for each backend, to free us from counting
 *	un-named portals.
 */
bool
ResPortalIncrementHashTableInit()
{
	HASHCTL		info;
	Size		max_table_size = mul_size(add_size(MaxResourcePortalsPerXact,1),
										  MaxBackends);
	int			hash_flags;


	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(ResPortalTag);
	info.entrysize = sizeof(ResPortalIncrement);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	ResPortalIncrementHash = ShmemInitHash("Portal Increment Hash",
											max_table_size / 2,
											max_table_size,
											&info,
											hash_flags);

	if (!ResPortalIncrementHash)
		return false;


	return true;
}


/*
 * ResIncrementAdd -- Add a new incremnt element to the increment hash.
 *
 * Notes:
 *	Return a pointer to where the new increment is stored, or NULL if we 
 *	are out of memory (or if we find a duplicate portalid).
 *
 *	The resource queue lightweight lock (ResQueueLock) *must* be held for
 *	this operation.
 */
ResPortalIncrement *
ResIncrementAdd(ResPortalIncrement *incSet,
				PROCLOCK *proclock,
				ResourceOwner owner)
{
	ResPortalIncrement	*incrementSet;
	ResPortalTag		portaltag;
	int					i;
	bool				found;


	/*  Set up the key.*/
	MemSet(&portaltag, 0, sizeof(ResPortalTag));
	portaltag.pid = incSet->pid;
	portaltag.portalId = incSet->portalId;
	
	/* Add (or find) the value. */
	incrementSet = (ResPortalIncrement *) hash_search( ResPortalIncrementHash,
													   (void *) &portaltag,
													   HASH_ENTER_NULL, &found);

	if (!incrementSet)
	{
		return NULL;
	}


	/*  Initialize it. */
	if (!found)
	{
		incrementSet->pid = incSet->pid;
		incrementSet->portalId = incSet->portalId;
		incrementSet->owner = owner;
		incrementSet->isHold = incSet->isHold;
		incrementSet->isCommitted = false;
		for (i = 0 ; i < NUM_RES_LIMIT_TYPES ; i++)
		{
			incrementSet->increments[i] = incSet->increments[i];
		}
		SHMQueueInsertBefore(&proclock->portalLinks, &incrementSet->portalLink);
	}
	else
	/* We have added this portId before - something has gone wrong! */
	{
		elog(WARNING, "duplicate portal id %u for proc %d", 
			 incSet->portalId, incSet->pid);
		incrementSet = NULL;
	}


	return incrementSet;
}


/*
 * ResIncrementFind -- Find the increment for a portal and process.
 *
 * Notes
 *	Return a pointer to where the new increment is stored (NULL if not found).
 *
 *	The resource queue lightweight lock (ResQueueLock) *must* be held for
 *	this operation.
 */
ResPortalIncrement *
ResIncrementFind(ResPortalTag *portaltag)
{

	ResPortalIncrement	*incrementSet;
	bool				found;

	
	incrementSet = (ResPortalIncrement *) hash_search( ResPortalIncrementHash,
													   (void *) portaltag,
													   HASH_FIND, &found);

	if (!incrementSet)
	{
		return NULL;
	}

	return incrementSet;
}


/*
 * ResIncrementRemove -- Remove a  increment for a portal and process.
 *
 * Notes
 *	The resource queue lightweight lock (ResQueueLock) *must* be held for
 *	this operation.
 */
bool
ResIncrementRemove(ResPortalTag *portaltag)
{

	ResPortalIncrement	*incrementSet;
	bool				found;

	
	incrementSet = (ResPortalIncrement *) hash_search( ResPortalIncrementHash,
													   (void *) portaltag,
													   HASH_FIND, &found);
	if (!incrementSet)
	{
		return false;
	}

	SHMQueueDelete(&incrementSet->portalLink);

	incrementSet = (ResPortalIncrement *) hash_search( ResPortalIncrementHash,
													   (void *) portaltag,
													   HASH_REMOVE, &found);


	return true;
}


/*
 * ResQueueHashTableInit -- initialize the hash table of resource queues.
 *
 * Notes:
 */
bool
ResQueueHashTableInit(void)
{

	HASHCTL			info;
	int				hash_flags;


	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	info.entrysize = sizeof(ResQueueData);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);

	elog(DEBUG1, "Creating hash table for %d queues", MaxResourceQueues);
	ResQueueHash = ShmemInitHash("Queue Hash",
								 MaxResourceQueues,
								 MaxResourceQueues,
								 &info,
								 hash_flags);

	if (!ResQueueHash)
		return false;


	return true;
}


/*
 * ResQueuehashNew -- return a new (empty) queue object to initialize.
 *
 * Notes
 *	The resource queue lightweight lock (ResQueueLock) *must* be held for
 *	this operation.
 */
ResQueue
ResQueueHashNew(Oid queueid)
{
	bool				found;
	ResQueueData		*queue;

	queue = (ResQueueData *) hash_search(ResQueueHash,
										 (void *) &queueid,
										 HASH_ENTER_NULL, &found);

	/* caller should test that the queue does not exist already */
	Assert(!found);

	if (!queue)
		return NULL;

	return (ResQueue) queue;
}


/*
 * ResQueueHashFind -- return the queue for a given oid.
 *
 * Notes
 *	The resource queue lightweight lock (ResQueueLock) *must* be held for
 *	this operation.
 */
ResQueue 
ResQueueHashFind(Oid queueid)
{
	bool				found;
	ResQueueData		*queue;

	queue = (ResQueueData *) hash_search(ResQueueHash,
										 (void *) &queueid,
										 HASH_FIND, &found);
	if (!queue)
		return NULL;

	return (ResQueue) queue;
}


/*
 * ResQueueHashRemove -- remove the queue for a given oid.
 *
 * Notes
 *	The resource queue lightweight lock (ResQueueLock) *must* be held for
 *	this operation.
 */
bool 
ResQueueHashRemove(Oid queueid)
{
	bool				found;
	void				*queue;

	queue = hash_search(ResQueueHash,
					    (void *) &queueid,
					    HASH_REMOVE, &found);
	if (!queue)
		return false;

	return true;
}


/*
 * pg_resqueue_status - produce a view with one row per resource queue
 *	showing internal information (counter values, waiters, holders).
 */
Datum
pg_resqueue_status(PG_FUNCTION_ARGS)
{

	FuncCallContext			*funcctx;
	Datum					result;
	MemoryContext			oldcontext;
	QueueStatusContext		*fctx;			/* User function context. */
	TupleDesc				tupledesc;
	HeapTuple				tuple;

	if (SRF_IS_FIRSTCALL())
	{
		int					i;
		int					num_calls;
		HASH_SEQ_STATUS		status;
		ResQueueData		*queue;
		int					partition;
		LWLockId			partitionLock;
		

		funcctx = SRF_FIRSTCALL_INIT();


		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		fctx = (QueueStatusContext *) palloc(sizeof(QueueStatusContext));
		funcctx->user_fctx = fctx;

		/* Construct a tuple descriptor for the result rows. */
		tupledesc = CreateTemplateTupleDesc(NUM_QUEUE_STATUS_ELEM, false);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "queueid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "queuecountvalue",
						   FLOAT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "queuecostvalue",
						   FLOAT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "queuewaiters",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "queueholders",
						   INT4OID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/*
		 * Allocate space for the per-call area - this overestimates, but
		 * means we can take the resource rescheduler lock after our
		 * memory context switching.
		 */
		fctx->record = (QueueStatusRec *) palloc(sizeof(QueueStatusRec) * 
					   MaxResourceQueues);

		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);

		/* 
		 * Take all the partition locks. This is necessary as we want to
		 * to use the same lock order as the rest of the code - i.e. partition
		 * locks *first* *then* the queue lock (otherwise we could deadlock
		 * ourselves). 
		 */
		for (partition = 0; partition < NUM_LOCK_PARTITIONS; partition++)
		{
			partitionLock = FirstLockMgrLock + partition;
			LWLockAcquire(partitionLock, LW_EXCLUSIVE);
		}

		/*
		 * Lock resource queue structures.
		 */
		LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

		/* Initialize for a sequential scan of the resource queue hash. */
		hash_seq_init(&status, ResQueueHash);
		num_calls = hash_get_num_entries(ResQueueHash);
		Assert(num_calls == ResScheduler->num_queues);

		i = 0;
		while ((queue = (ResQueueData *) hash_seq_search(&status)) != NULL)
		{
			int			j;
			ResLimit	limits;
			LOCKTAG		tag;
			LOCK		*lock;
			uint32		hashcode;
			bool		found;

			/* Get the thresholds and current values for each limit. */
			limits = queue->limits;
			
			fctx->record[i].queueid = queue->queueid;

			for (j = 0; j < NUM_RES_LIMIT_TYPES; j++)
			{
				switch (limits[j].type)
				{
					case RES_COUNT_LIMIT:
					{
						fctx->record[i].queuecountthreshold =
						limits[j].threshold_value;

						fctx->record[i].queuecountvalue =
						limits[j].current_value;
					}
					break;

					case RES_COST_LIMIT:
					{
						fctx->record[i].queuecostthreshold =
						limits[j].threshold_value;

						fctx->record[i].queuecostvalue =
						limits[j].current_value;
					}
					break;

					default:
					break;

				}
			}


			/* 
			 * Get the holders and waiters count for the corrosponding resource
			 * lock.
			 * Think about this - getting ResQueueLock then partitionLock is the
			 * opposite way from which the rest of your code does this... so
			 * you could perhaps deadlock here...!
			 * Might be better to grab *all* the partition locks first!
			 */

			SET_LOCKTAG_RESOURCE_QUEUE(tag, queue->queueid);
			hashcode = LockTagHashCode(&tag);

			
			lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
														(void *) &tag,
														hashcode,
														HASH_FIND,
														&found);

			if (!found || !lock)
			{
				fctx->record[i].queuewaiters = 0;
				fctx->record[i].queueholders = 0;
			}
			else
			{
				fctx->record[i].queuewaiters = lock->nRequested - lock->nGranted;
				fctx->record[i].queueholders = lock->nGranted;
			}

			i++;
		}

		/* Release the resource scheduler lock. */
		LWLockRelease(ResQueueLock);

		/* ...and the partition locks. */
		for (partition = 0; partition < NUM_LOCK_PARTITIONS; partition++)
		{
			partitionLock = FirstLockMgrLock + partition;
			LWLockRelease(partitionLock);
		}

		/* Set the real no. of calls as we know it now! */
		Assert(i <= num_calls);
		funcctx->max_calls = num_calls;
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state. */
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		int			i = funcctx->call_cntr;
		QueueStatusRec	*record = &fctx->record[i];
		Datum		values[NUM_QUEUE_STATUS_ELEM];
		bool		nulls[NUM_QUEUE_STATUS_ELEM];

		values[0] = ObjectIdGetDatum(record->queueid);
		nulls[0] = false;

		/* Make the counters null if the limit is disbaled. */
		if (record->queuecountthreshold != INVALID_RES_LIMIT_THRESHOLD)
		{
			values[1] = Float4GetDatum(record->queuecountvalue);
			nulls[1] = false;
		}
		else
			nulls[1] = true;

		if (record->queuecostthreshold != INVALID_RES_LIMIT_THRESHOLD)
		{
			values[2] = Float4GetDatum(record->queuecostvalue);
			nulls[2] = false;
		}
		else
			nulls[2] = true;
		

		values[3] = record->queuewaiters;
		nulls[3] = false;

		values[4] = record->queueholders;
		nulls[4] = false;
		
		/* Build and return the tuple. */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);

}
