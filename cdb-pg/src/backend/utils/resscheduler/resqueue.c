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

#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/lmgr.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/resscheduler.h"

/*
 * Global Variables
 */
static HTAB *ResPortalIncrementHash;		/* Hash of resource increments. */
static HTAB *ResQueueHash;					/* Hash of resource queues. */


/*
 * ResLockAcquire -- acquire a resource lock.
 *
 * Notes and critisms:
 *
 *	Returns LOCKACQUIRE_OK.
 *
 *	Analogous to LockAcquire, but the lockmethodid and lockmode are not
 *	required in the function prototype as we are *always* using lockmethod
 *	RESOURCE_LOCKMETHOD and lockmode ExclusiveLock. 
 *
 *	The semantics of resource locks mean that lockmode has minimal meaning -
 *	the conflict rules are determined by the state of the counters of the
 *	corresponding queue. We are maintaining the lock lockmode and related
 *	elements (holdmask etc), in order to ease comparison with standard locks
 *	at deadlock check time (well, so we hope anyway.)
 *
 *	(hard coding lockmode to ExclusiveLock... TODO discuss)
 */
LockAcquireResult
ResLockAcquire(LOCKTAG *locktag, ResPortalIncrement *incrementSet)
{
	LOCKMODE		lockmode = ExclusiveLock;
	LOCK			*lock;
	PROCLOCK		*proclock;
	PROCLOCKTAG		proclocktag;
	bool			found;
	ResourceOwner	owner;
	LWLockId		masterLock;
	LockMethod		lockMethodTable;
	int				status;

	/* Setup the lock method bits. */
	locktag->locktag_lockmethodid = RESOURCE_LOCKMETHOD;
	

	lockMethodTable = LockMethods[RESOURCE_LOCKMETHOD];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", RESOURCE_LOCKMETHOD);

	/* Provide a resource owner. */
	owner = CurrentResourceOwner;

	/* We are going to examine the shared lock table. */
	masterLock = lockMethodTable->masterLock;
	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * Find or create a lock with this tag.
	 */
	lock = (LOCK *) hash_search(LockMethodLockHash[RESOURCE_LOCKMETHOD],
								(void *) locktag,
								HASH_ENTER_NULL, &found);
    if (!lock)
	{
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			 errhint("You may need to increase max_resource_qeueues.")));
	}

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
	proclocktag.lock = MAKE_OFFSET(lock);
	proclocktag.proc = MAKE_OFFSET(MyProc);

	/*
	 * Find or create a proclock entry with this tag. 
	 */ 
	proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[RESOURCE_LOCKMETHOD],
										(void *) &proclocktag,
										HASH_ENTER_NULL, &found);
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
			if (!hash_search(LockMethodLockHash[RESOURCE_LOCKMETHOD],
							(void *) &(lock->tag),
							HASH_REMOVE, NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			errhint("You may need to increase max_resource_qeueues.")));
	}

	/*
	 * If new, initialize the new entry.
	 */
	if (!found)
	{
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* Add proclock to appropriate lists */
		SHMQueueInsertBefore(&lock->procLocks, &proclock->lockLink);
		SHMQueueInsertBefore(&MyProc->procLocks, &proclock->procLink);
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
	incrementSet = ResIncrementAdd(incrementSet, proclock, owner);
	if (!incrementSet)
	{
		LWLockRelease(masterLock);
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
	if (status ==  STATUS_OK)
	{
		/* 
		 * The requested lock will *not* exhaust the limit for this resource
		 * queue, so just grant it and release the resource scheduler lwlock.
		 */
		ResGrantLock(lock, proclock);
		ResLockUpdateLimit(lock, proclock, incrementSet, true);

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

		/*
		 * Sleep till someone wakes me up.
		 */
		ResWaitOnLock(lock, proclock, owner);

		/*
		 * Have been awakened, check state is consistent.
		 */
		if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
		{
			LWLockRelease(masterLock);
			elog(ERROR, "ResLockAcquire failed");
		}

		/* Reset the portal id. */
		MyProc->waitPortalId = INVALID_PORTALID;

	}


	/* Release the masterlock. */
	LWLockRelease(masterLock);

	return LOCKACQUIRE_OK;
}


/*
 * ResLockRelease -- release a resource lock.
 *
 * Notes: (TODO)
 *	Analogous to LockRelease, but the lockmethodid and lockmode are not
 *	required in the function prototype as we are *always* using lockmethod
 *	RESOURCE_LOCKMETHOD and lockmode ExclusiveLock. 
 *
 * (See ResLockAcquire for more notes).
 *
 */
bool
ResLockRelease(LOCKTAG *locktag, uint32 resPortalId)
{
	LOCKMODE		lockmode = ExclusiveLock;
	LOCK			*lock;
	PROCLOCK		*proclock;
	PROCLOCKTAG		proclocktag;
	bool			found;
	ResourceOwner	owner;
	LWLockId		masterLock;
	LockMethod		lockMethodTable;
	/*int				status;*/
	ResPortalIncrement	*incrementSet;
	ResPortalTag		portalTag;

	/* Setup the lock method bits. */
	locktag->locktag_lockmethodid = RESOURCE_LOCKMETHOD;
	

	lockMethodTable = LockMethods[RESOURCE_LOCKMETHOD];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", RESOURCE_LOCKMETHOD);

	/* Provide a resource owner. */
	owner = CurrentResourceOwner;

	/* We are going to examine the shared lock table. */
	masterLock = lockMethodTable->masterLock;
	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	/*
	 * Find a lock with this tag, do not create one if there is nothing to 
	 * find - we cannot release a non-existent lock!
	 */
	lock = (LOCK *) hash_search(LockMethodLockHash[RESOURCE_LOCKMETHOD],
								(void *) locktag,
								HASH_FIND, &found);
    if (found && !lock)
	{
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			 errhint("You may need to increase max_resource_queues.")));
	}

	/* If we didn't find a lock to release, better say so! */
	if (!found)
	{
		LWLockRelease(masterLock);
		elog(ERROR, "you do not hold a lock on queue %d",
			locktag->locktag_field1);
			return FALSE;

	}

	/*
	 *  Create the hash key for the proclock table.
	 */
	MemSet(&proclocktag, 0, sizeof(PROCLOCKTAG));	/* Clear padding.*/
	proclocktag.lock = MAKE_OFFSET(lock);
	proclocktag.proc = MAKE_OFFSET(MyProc);

	/*
	 * Find a proclock entry with this tag. 
	 */ 
	proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash[RESOURCE_LOCKMETHOD],
										(void *) &proclocktag,
										HASH_FIND, &found);
    if (found && !proclock)
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
			if (!hash_search(LockMethodLockHash[RESOURCE_LOCKMETHOD],
							(void *) &(lock->tag),
							HASH_REMOVE, NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(masterLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
			errhint("You may need to increase max_resource_queues.")));
	}

	/* No proclock entry for us.*/
	if (!found)
	{
		LWLockRelease(masterLock);
		elog(ERROR, "failed to find PROCLOCK for queue %d",
			locktag->locktag_field1);
			return FALSE;
	}


	/*
	 * Double-check that we are actually holding a lock of the type we want to
	 * Release.
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		LWLockRelease(masterLock);
		elog(ERROR, "you don't hold a lock of type %d on queue %d", 
					lockmode, locktag->locktag_field1);
		return FALSE;
			
	}

	/*
	 * Find the increment for this portal and process.
	 */
	MemSet(&portalTag, 0, sizeof(ResPortalTag));
	portalTag.pid = MyProc->pid;
	portalTag.portalId = resPortalId;
	
	incrementSet = ResIncrementFind(&portalTag);
	if (!incrementSet)
	{
		LWLockRelease(masterLock);
		elog(ERROR, "no increment data for portal id %u and pid %d",
			 resPortalId, MyProc->pid);
	}


	/*
	 * Un-grant the lock.
	 */
	ResUnGrantLock(lock, proclock);
	ResLockUpdateLimit(lock, proclock, incrementSet, false);

	/*
	 * Perform clean-up, waking up any waiters!
	 */
	ResCleanUpLock(lock, proclock);

	/* 
	 * Clean up the increment set. 
	 */
	if (!ResIncrementRemove(&portalTag))
	{
		LWLockRelease(masterLock);
		elog(ERROR, "no increment to remove for portal id %u and pid %d",
			 resPortalId, MyProc->pid);
	}

	LWLockRelease(masterLock);

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
 * Warnings:
 *	It is expected that the locktable masterlock is held before calling this 
 *  function, as the various shared queue counts are inspected.
 */
int
ResLockCheckLimit(LOCK *lock, PROCLOCK *proclock, 
				  ResPortalIncrement *incrementSet, bool increment)
{
	bool			over_limit = false;
	int				status = STATUS_OK;
	Cost			increment_amt;
	int				i;

	/* Get the queue for this lock.*/
	ResQueue		queue = GetResQueueFromLock(lock);
	ResLimit		limits = queue->limits;
	

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
				/*
				 * A little bit of subtlety here, allow statements that
				 * blowout the limit if noone else is active!
				 */
				if ((limits[i].current_value + increment_amt > 
					limits[i].threshold_value) &&
					(limits[i].current_value > 0.1))
					over_limit = true;

				elog(DEBUG1, "checking cost limit threshold %.2f current %.2f",
					 limits[i].threshold_value, limits[i].current_value);
			}
			break;

			default:
			break;
		
		}
	}

	if (over_limit)
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
 *	It is expected that the locktable masterlock is held before calling this 
 *  function, as the various shared queue counts are inspected.
 */
void
ResLockUpdateLimit(LOCK *lock, PROCLOCK *proclock, 
				   ResPortalIncrement *incrementSet, bool increment)
{
	Cost			increment_amt;
	int				i;

	/* Get the queue for this lock.*/
	ResQueue		queue = GetResQueueFromLock(lock);
	ResLimit		limits = queue->limits;
	

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
					   (limits[i].current_value - increment_amt < 0.1) ||
						(increment_amt - limits[i].current_value < 0.1));

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
 * 
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
 *	It is expected that the locktable masterlock is held before calling this 
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
 * (maybe should return true only if there are waiters! TODO)
 *
 * Warnings:
 *	It is expected that the locktable masterlock is held before calling this 
 *  function, as the various shared queue counts are inspected.
 */
bool
ResUnGrantLock(LOCK *lock, PROCLOCK *proclock)
{
	LOCKMODE	lockmode = ExclusiveLock;

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
ResCleanUpLock(LOCK *lock, PROCLOCK *proclock)
{
	/*LOCKMODE	lockmode = ExclusiveLock; */

	/*
	 * If this was my last hold on this lock, delete my entry in the proclock
	 * table.
	 */
	if (proclock->holdMask == 0 && proclock->nLocks == 0)
	{
		SHMQueueDelete(&proclock->lockLink);
		SHMQueueDelete(&proclock->procLink);
		if (!hash_search(LockMethodProcLockHash[RESOURCE_LOCKMETHOD],
						 (void *) &(proclock->tag),
						 HASH_REMOVE, NULL))
			elog(PANIC, "proclock table corrupted");
	}
   
	if (lock->nRequested == 0)
	{
		/*
		 * The caller just released the last lock, so garbage-collect the lock
		 * object.
		 */
		Assert(SHMQueueEmpty(&(lock->procLocks)));
		if (!hash_search(LockMethodLockHash[RESOURCE_LOCKMETHOD],
						 (void *) &(lock->tag),
						 HASH_REMOVE, NULL))
			 elog(PANIC, "lock table corrupted");

	}

	/*
	 * Awaken any waiters (hmm maybe we should check if there are any TODO)
	 */
	ResProcLockWakeup(lock);

	return;
}


/*
 * WaitOnResLock -- wait to acquire a resource lock.
 *
 * 
 * Warnings:
 *	It is expected that the locktable masterlock is held before calling this 
 *  function, as the various shared queue counts are inspected.
 */
void
ResWaitOnLock(LOCK *lock, PROCLOCK *proclock, ResourceOwner owner)
{
	
	LockMethod  lockMethodTable = LockMethods[RESOURCE_LOCKMETHOD];
	const char	*old_status;
	char		*new_status;
	int			len;

	old_status = get_ps_display(&len);
	new_status = (char *) palloc(len + 8 + 1);
	memcpy(new_status, old_status, len);
	strcpy(new_status + len, " queuing");
	set_ps_display(new_status, false);
	new_status[len] = '\0';

	/*
	 * Now sleep.
	 */
	if (ResProcSleep(ExclusiveLock, lock, proclock) != STATUS_OK)
	{
		/*
		 * We failed as a result of a deadlock, see CheckDeadLock(). Quit now.
		 * (We won't get here yet, as the deadlock changes are TODO)
		 */
		LWLockRelease(lockMethodTable->masterLock);
		DeadLockReport();
	}

	set_ps_display(new_status, false);
	pfree(new_status);

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
			LWLockRelease(LockMgrLock);
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
ResRemoveFromWaitQueue(PGPROC *proc)
{
    LOCK			*waitLock = proc->waitLock;
    PROCLOCK		*proclock = proc->waitProcLock;
    LOCKMODE		lockmode = proc->waitLockMode;
    LOCKMETHODID	lockmethodid = LOCK_LOCKMETHOD(*waitLock);
	ResPortalTag	portalTag;

    /* Make sure proc is waiting */
	Assert(proc->links.next != INVALID_OFFSET);
	Assert(waitLock);
	Assert(waitLock->waitProcs.size > 0);
	Assert(0 < lockmethodid && lockmethodid < NumLockMethods);

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
   	ResCleanUpLock(waitLock, (PROCLOCK *) proclock);

}


/*
 * ResLockWaitCancel -- Cancel any pending wait for a resource lock, when 
 *	aborting a transaction.
 */
void
ResLockWaitCancel(void)
{
	
	/* Unlink myself from the wait queue, if on it (might not be anymore!) */
	LWLockAcquire(LockMgrLock, LW_EXCLUSIVE);

	if (MyProc->links.next != INVALID_OFFSET)
	{
		/* We could not have been granted the lock yet */
		Assert(MyProc->waitStatus == STATUS_ERROR);
		ResRemoveFromWaitQueue(MyProc);
	}
	else if (MyProc->waitPortalId != INVALID_PORTALID)
	{
		/*
		 * We have been kicked off the wait - possibly by LockWaitCancel,
		 * so clean the dangling portal increment.
		 */
		ResPortalTag	portalTag;

		MemSet(&portalTag, 0, sizeof(ResPortalTag));
		portalTag.pid = MyProc->pid;
		portalTag.portalId = MyProc->waitPortalId;
		ResIncrementRemove(&portalTag);
	}

	LWLockRelease(LockMgrLock);

	/*
	 * Reset the proc wait semaphore to zero. This is necessary in the
	 * scenario where someone else granted us the lock we wanted before we
	 * were able to remove ourselves from the wait-list.
	 */
	PGSemaphoreReset(&MyProc->sem);

	return;
}


/*
 * ResProcReleaseLocks -- release locks associated with current transaction
 *	at main transaction commit or abort.
 */
void
ResProcReleaseLocks(bool isCommit, bool releaseHold)
{
	if (!MyProc)
		return;

	/* Take ourselves out of the wait queue (if waiting). */
	ResLockWaitCancel();

	/* release resource locks*/
	ResLockReleaseAll(true, isCommit, releaseHold);
}


/*
 * ResProcReleaseCurrentOwner -- Release all resource locks that are held by 
 * the current resource owner for current process.
 */
void
ResProcReleaseCurrentOwner(bool isCommit, bool releaseHold)
{
	ResLockReleaseAll(false, isCommit, releaseHold);
}


/*
 * ResLockReleaseAll -- Release all resource locks that are held by the 
 *	current process, or just the current resourceowner if !isAllOwners.
 *
 * Notes
 *	The merging of ResProcReleaseCurrentOwner() with this function complicates
 *	the logic a little, but seems worthwhile as otherwise almost all of the 
*	code is duplicated in these two functions, which seems clumsy.
 */
void
ResLockReleaseAll(bool isAllOwners, bool isCommit, bool releaseHold)
{
	LOCKMODE		lockmode = ExclusiveLock;
	LockMethod		lockMethodTable;
	LWLockId		masterLock;
	LOCK			*lock;
	PROCLOCK		*proclock;
	SHM_QUEUE		*procLocks = &(MyProc->procLocks);
	SHM_QUEUE		*portalLinks;
	bool			foundHoldable;		/* Found lock for a holdable cursor? */

	/* Setup the lock method bits. */
	lockMethodTable = LockMethods[RESOURCE_LOCKMETHOD];
	if (!lockMethodTable)
		elog(ERROR, "unrecognized lock method: %d", RESOURCE_LOCKMETHOD);


	/* We are going to examine the shared lock table. */
	masterLock = lockMethodTable->masterLock;
	LWLockAcquire(masterLock, LW_EXCLUSIVE);

	proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
										 offsetof(PROCLOCK, procLink));

	while (proclock)
	{

		PROCLOCK	*nextplock;
		foundHoldable = false;

		/* Get link first, since we may unlink/delete this one. */
		nextplock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->procLink,
											  offsetof(PROCLOCK, procLink));

		Assert(proclock->tag.proc == MAKE_OFFSET(MyProc));
		
		lock = (LOCK *) MAKE_PTR(proclock->tag.lock);

		/*
		 * if the lock is a resource lock, then release it.
		 */
		if (lock->tag.locktag_lockmethodid == RESOURCE_LOCKMETHOD)
		{

			proclock->releaseMask = proclock->holdMask;

			Assert(lock->nRequested >= 0);
			Assert(lock->nGranted >= 0);
			Assert(lock->nGranted <= lock->nRequested);
			Assert((proclock->holdMask & ~lock->grantMask) == 0);


			/*
			 * Release the lock.
			 * (LockReleaseAll checks return of Ungrant... maybe you should too
			 * TODO - discuss)
			 *
			 * We need to release the lock as many times as we have acquired it,
			 * so need to iterate through the portal increment list.
			 */
			if (proclock->releaseMask & LOCKBIT_ON(lockmode))
			{

				ResPortalIncrement		*incrementSet;
				ResPortalIncrement		*nextiSet;
				ResPortalTag			portalTag;

				/* List of portal increments for this proclock. */
				portalLinks = &(proclock->portalLinks);

				incrementSet = (ResPortalIncrement *) SHMQueueNext(portalLinks, 
									portalLinks,
									offsetof(ResPortalIncrement, portalLink));

				while (incrementSet)
				{

					/* 
					 * Get link first, since we will unlink/delete this one,
					 * and compute a portalTag for removing the increment set
					 * itself.
					 */
					nextiSet = (ResPortalIncrement *) SHMQueueNext(portalLinks, 
									&incrementSet->portalLink,
									offsetof(ResPortalIncrement, portalLink));
					MemSet(&portalTag, 0, sizeof(ResPortalTag));
					portalTag.pid = MyProc->pid;
					portalTag.portalId = incrementSet->portalId;

					/* 
					 * If we are releasing all owners or otherwise
					 * if the increment is owned by current resource owner
					 * then release the corrosponding lock.
					 */
					if (isAllOwners ||
						incrementSet->owner == CurrentResourceOwner)
					{
						/*
						 * If we are aborting the transaction that created a
						 * holdabel portal, then it is going to be destroyed,
						 * otherwise we record that the portal has passed a
						 * commit.
						 */
						if (incrementSet->isHold && !releaseHold)
						{ 
							if (!isCommit && !incrementSet->isCommitted)
								incrementSet->isHold = false;
	
							else if (isCommit && !incrementSet->isCommitted)
							{
								incrementSet->isCommitted = true;
								foundHoldable = true;
							}
						}

						/*
						 * if we have specified that all resource locks are to 
						 * be released, then do them all, otherwise only do the
						 * non holdable portals.
						 */
						if (releaseHold ||
						    (!releaseHold && !incrementSet->isHold))
						{
							elog(DEBUG1, "releasing portal %u pid %d",
										 portalTag.portalId, portalTag.pid);

							/* 
							 * If still holding the lock now, ungrant and 
							 * decrement the limit counters. 
							 */
							if (incrementSet->portalId != MyProc->waitPortalId)
							{
								ResUnGrantLock(lock, proclock);
								ResLockUpdateLimit(lock, proclock, 
												  incrementSet, false);
							}

							/* 
							 * Unlink this portalId, and delete it from the 
							 * portal hash.
							 */
							if (!ResIncrementRemove(&portalTag))
							{
								LWLockRelease(masterLock);
								elog(ERROR, 
								 "no increment to remove for portal %u pid %d",
									 portalTag.portalId, portalTag.pid);
							}

						}

					}

					/* Get the next increment set to process. */
					incrementSet = nextiSet;
				}

			}

			Assert((lock->nRequested >= 0) && (lock->nGranted >= 0));
			Assert(lock->nGranted <= lock->nRequested);

			if (!foundHoldable || releaseHold)
			{
				proclock->releaseMask = 0;
				ResCleanUpLock(lock, proclock);
			}

		}

		/*
		 * Set next proclock.
		 */
		proclock = nextplock;

	}

	LWLockRelease(masterLock);

	return;
}


/*
 * ResPortalIncrementHashTableInit - Initialize the increment hash.
 *
 * Notes:
 *	This stores the possible increments that a given statement will cause to
 *	be added to the limits for a resource queue.
 */
bool
ResPortalIncrementHashTableInit()
{
	HASHCTL			info;
	Size			max_table_size = mul_size(MaxResourcePortalsPerXact, MaxBackends);
	int				hash_flags;


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
		incrementSet->isHold = false;		/* Is not set till actual open. */
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
		return NULL;

	return incrementSet;
}


/*
 * ResIncrementRemove -- Remove a  increment for a portal and process.
 *
 * Notes
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
		return false;

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
 * ResIncrementUpdate -- change Holdable and first commit flags for a portal
 *	increment.
 *
 * Notes
 *	Unlike most other such routines, the LW lock explicitly acquired.
 *	This is because it is called from places external to the resource lock
 *	setup/teardown.
 */
void
ResIncrementUpdate(int pid, uint32 portalId, bool isHold, bool isCommitted)
{
	ResPortalTag			portalTag;
	ResPortalIncrement		*incrementSet;

	/* Take LW lock before messing with the portal hash. */
	LWLockAcquire(LockMgrLock, LW_EXCLUSIVE);

	if ( pid < 0)
		pid = MyProc->pid;

	/* Prepare a search tag, find the increment and update it.*/
	MemSet(&portalTag, 0, sizeof(ResPortalTag));
	portalTag.pid = pid;
	portalTag.portalId = portalId;
	
	incrementSet = ResIncrementFind(&portalTag);
	
	if (incrementSet)
	{
		/* 
		 * Don't worry about the not found case - there are then more serious 
		 * problems that will be made apparent in later checks.
		 */
		incrementSet->isHold = isHold;
		incrementSet->isCommitted = isCommitted;
	}

	LWLockRelease(LockMgrLock);

	return;
}
