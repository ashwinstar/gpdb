/*-------------------------------------------------------------------------
 *
 * resscheduler.c
 *	  POSTGRES resource scheduling management code.
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

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_resqueue.h"
#include "catalog/pg_tablespace.h"
#include "executor/executor.h"
#include "optimizer/planner.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/resscheduler.h"
#include "utils/syscache.h"

/*
 * GUC variables.
 */
bool	ResourceScheduler;						/* Is scheduling enabled? */
int		MaxResourceQueues;						/* Max # of queues. */
int		MaxResourcePortalsPerXact;				/* Max # tracked portals -
												 * per backend . */
bool	ResourceSelectOnly;						/* Only lock SELECT/DECLARE? */


/*
 * Global variables
 */
Oid				MyQueueId;			/* resource queue for current role. */
static uint32	portalId = 0;		/* id of portal, for tracking cursors. */
static uint32	lastPortalId = 0;	/* id found by last portal search. */


/*
 * ResSchedulerShmemSize -- estimate size the schedular structures will need in
 *	shared memory.
 *
 */
Size
ResSchedulerShmemSize(void)
{
	Size		size;

	/* The hash of queues.*/
	size = hash_estimate_size((Size)MaxResourceQueues, sizeof(ResQueueData));

	/* The scheduler structure. */
	size = add_size(size, sizeof(ResSchedulerData));

	/* Add a safety margin */
	size = add_size(size, size / 10);


	return size;
}


/*
 * ResPortalIncrementShmemSize -- Estimate the size of the increment hash.
 *
 * Notes
 *	We allow one extra slot for unnamed portals, as it is simpler to not
 *	count them at all.
 */
Size 
ResPortalIncrementShmemSize(void)
{

	Size	size;
	Size	max_table_size = mul_size(add_size(MaxResourcePortalsPerXact, 1), 
									  MaxBackends);

	size = hash_estimate_size(max_table_size, sizeof(ResPortalIncrement));

	/* Add a safety margin */
	size = add_size(size, size / 10);


	return size;
}


/*
 * InitResScheduler -- initialize the schedular queues hash in shared memory.
 *
 * The queuek hash table has no data in it as yet (InitResQueues cannot be 
 * called until catalog access is available.
 *
 */
void
InitResScheduler(void)
{
	bool		found;

	/* Create the scheduler structure. */
	ResScheduler = (ResSchedulerData *)
		ShmemInitStruct("Resource Scheduler Data",
						sizeof(ResSchedulerData),
						&found);

	/* Specify that we have no initialized queues yet. */
	ResScheduler->num_queues = 0;

	if (found)
		return;			/* We are already initialized. */

	/* Create the resource queue hash (empty at this point). */
	found = ResQueueHashTableInit();
	if (!found)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough shared memory for resource scheduler")));

	elog(LOG, "initialized resource scheduler");
	

	return;
}


/*
 * InitResPortalIncrementHash - Initialize the portal increment hash.
 *
 * Notes:
 *	Simply calls the internal initialization function.
 */
void
InitResPortalIncrementHash(void)
{
	bool	result;


	result = ResPortalIncrementHashTableInit();

	if (!result)
		elog(FATAL, "could not initialize portal increment hash");

	elog(LOG, "initialized portal increment hash");
}


/*
 * InitResQueues -- initialize the resource queues in shared memory.
 *
 */
void
InitResQueues(void)
{

	ResourceOwner		owner;
	RelFileNode			rnode;
	Relation			relQueue;
	HeapScanDesc		scan;
	HeapTuple			tuple;
	Form_pg_resqueue	queueform;
	Oid					queueid;
	int					numQueues = 0;
	bool				overcommit;
	Cost				thresholds[NUM_RES_LIMIT_TYPES];
	char				*queuename;
	bool				queuesok = true;
	
	/*
	 * Use the same fake-relcache facility that WAL replay uses.
	 */
	XLogInitRelationCache();

	/*
	 * Need a resource owner to keep the heapam code happy.
	 */
	owner = ResourceOwnerCreate(NULL, "InitQueues");
	CurrentResourceOwner = owner;

	/* Set the path to the resource queue relation */
	rnode.spcNode = GLOBALTABLESPACE_OID;
	rnode.dbNode = 0;
	rnode.relNode = ResQueueRelationId;

	/* No locking is needed because no one else is alive yet */
	relQueue = XLogOpenRelation(rnode);

	/*
	 * setup a queue in shared memory for each record in the queue
	 * catalog.
	 */
	scan = heap_beginscan(relQueue, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		numQueues++;

		queueform = (Form_pg_resqueue) GETSTRUCT(tuple);

		queueid = HeapTupleGetOid(tuple);
		queuename = NameStr(queueform->rsqname);
		thresholds[RES_COUNT_LIMIT] = queueform->rsqcountlimit;
		thresholds[RES_COST_LIMIT] = queueform->rsqcostlimit;
		overcommit = queueform->rsqovercommit;

		queuesok = ResCreateQueue(queueid, thresholds, overcommit);
	}
	heap_endscan(scan);
	

	/* Shut down the fake cache. */
	CurrentResourceOwner = NULL;
	ResourceOwnerDelete(owner);

	XLogCloseRelationCache();
	
	if (!queuesok)
		ereport(PANIC,
			(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			 errmsg("insufficient resource queues available"),
		errhint("Increase max_resource_queues to %d.", numQueues)));

	elog(LOG,"initialized %d resource queues", numQueues);

	return;
}


/*
 * ResCreateQueue -- initialize the elements for a resource queue.
 *
 * Notes:
 *	It is expected that the appropriate lightweight lock is held before
 *	calling this - unless we are the startup process.
 */
bool
ResCreateQueue(Oid queueid, Cost limits[NUM_RES_LIMIT_TYPES], bool overcommit)
{

	ResQueue		queue = ResQueueHashNew(queueid);
	int				i;
	
	/* If the new queue pointer is NULL, then we are out of queueus. */
	if (!queue || ResScheduler->num_queues >= MaxResourceQueues)
		return false;

	/* Set queue oid and offset in the schedular array */
	queue->queueid = queueid;

	/* Set the number of limits 0 initially. */
	queue->num_limits = 0;

	/* Set overcommit.*/
	queue->overcommit = overcommit;

	/* Now run through all the possible limit types.*/
	for (i = 0 ; i < NUM_RES_LIMIT_TYPES; i++)
	{
		/*
		 * Specific initializations for the limit types (the current two are
		 * in fact the same).
		 */
		switch (i)
		{
			case RES_COUNT_LIMIT:
			case RES_COST_LIMIT:
			{
				queue->num_limits++;
				queue->limits[i].type = i;
				queue->limits[i].threshold_value = limits[i];
				queue->limits[i].current_value = 0;
				queue->limits[i].threshold_is_max = true;
			}
			break;

			default:
				elog(ERROR, "unknown resource limit type %d", i);
			break;
		}


	}
	ResScheduler->num_queues++;
	return true;
}


/*
 * ResAlterQueue -- Change thresholds, overcommit for a resource queue.
 *
 * Notes:
 *	It is expected that the appropriate lightweight lock is held before
 *	calling this.
 */
ResAlterQueueResult
ResAlterQueue(Oid queueid, Cost limits[NUM_RES_LIMIT_TYPES], bool overcommit)
{

	ResQueue		queue = ResQueueHashFind(queueid);
	bool			result = ALTERQUEUE_OK;
	int				i;
	

	/* Handed an Oid for something other than a queue, bail out! */
	if (queue == NULL)
		return ALTERQUEUE_ERROR;


	/* 
	 * Now run through all the possible limit types, checking all the
	 * intended changes are ok.
	 */
	for (i = 0 ; i < NUM_RES_LIMIT_TYPES; i++)
	{
		/* Don't sanity check thresholds if that are about to be disabled. */
		if (limits[i] == INVALID_RES_LIMIT_THRESHOLD)
			continue;

		/*
		 * Sanity checks for the limit types: 
		 * We want to ensure that the new thresholds are not less than the 
		 * current values. 
		 * For overcommittable limits we need to ensure the queue is not in 
		 * an overcommited state if we about to turn overcommit off.
		 */
		switch (i)
		{

			case RES_COUNT_LIMIT:
			{
				/* Don't change a threshold to below the current value. */
				if ((queue->limits[i].threshold_value != limits[i]) &&
					(limits[i] <= queue->limits[i].current_value))
					result = ALTERQUEUE_SMALL_THRESHOLD;
			}
			break;

			case RES_COST_LIMIT:
			{
				/* Don't change a threshold to below the current value. */
				if ((queue->limits[i].threshold_value != limits[i]) &&
					(limits[i] <= queue->limits[i].current_value))
					result = ALTERQUEUE_SMALL_THRESHOLD;

				/* 
				 * Don't turn overcommit off if queue could be overcommitted.
				 * We err on the side of caution in this case and demand that
				 * the queue is idle - we don't want to leave a query waiting 
				 * forever in the wait queue. We use the same roundoff limit
				 * value (0.1) as ResLockCheckLimit does.
				 */
				if (!overcommit && queue->overcommit && 
					(queue->limits[i].current_value > 0.1))
					result = ALTERQUEUE_OVERCOMMITTED;
			}
			break;
		}


	}

	/*
	 * If threshold and overcommit alterations are all ok, do the the changes.
	 */
	if (result == ALTERQUEUE_OK)
	{
		for (i = 0 ; i < NUM_RES_LIMIT_TYPES; i++)
		{
			/*
			 * Assign the new thresholds.
			 */
			switch (i)
			{
				case RES_COUNT_LIMIT:
				case RES_COST_LIMIT:
				{
					
					if (queue->limits[i].threshold_value != limits[i])
						queue->limits[i].threshold_value = limits[i];
				}
				break;
			}
	
		}

		/* Set overcommit if that has changed. */
		if (queue->overcommit != overcommit)
			queue->overcommit = overcommit;
	}
	
	return result;
	
}


/*
 * ResDestroyQueue -- destroy a resource queue.
 *
 * Notes:
 *	It is expected that the appropriate lightweight lock is held before
 *	calling this.
 */
bool
ResDestroyQueue(Oid queueid)
{

	ResQueue		queue = ResQueueHashFind(queueid);
	bool			allzero = true;
	bool			result;
	int				i;
	

	/* Handed an Oid for something other than a queue, bail out! */
	if (queue == NULL)
		return false;


	/* 
	 * Now run through all the possible limit types, checking all the
	 * current counters are zero.
	 */
	for (i = 0 ; i < NUM_RES_LIMIT_TYPES; i++)
	{
		/*
		 * Specific checks for the limit types (the current two are
		 * in fact the same).
		 */
		switch (i)
		{
			case RES_COUNT_LIMIT:
			case RES_COST_LIMIT:
			{
				
				if (queue->limits[i].current_value != 0)
					allzero = false;
			}
			break;
		}


	}

	/*
	 * No non-zero current values for the queue, are able to destroy it.
	 *
	 * Some care is needed, as this leaves the possibility of a connected
	 * user still using this queue via a ALTER ROLE ... RESOURCE QUEUE while
	 * he is connected. However this is no worse than being able to DROP a 
	 * ROLE that is corrently connected!
	 */
	if (allzero)
	{
		result = ResQueueHashRemove(queueid);
		if (result)
			ResScheduler->num_queues--;

		return result;
	}
	else
	{
		return false;
	}
	
}


/*
 * ResLockPortal -- get a resource lock for a PortalRun.
 *
 * Returns:
 *	true if the lock has been taken
 *	false if the lock has been skipped.
 *
 * Bugs:
 *	server side cursors are not handled well (need to push down the taking of 
 *	the lock for that case).
 */
bool
ResLockPortal(Portal portal)
{

	Cost		cost = 0;
	ListCell	*planlist_item;	
	Plan		*plan;
	bool		returnReleaseOk = false;	/* Release resource lock? */
	bool		takeLock;					/* Take resource lock? */
	LOCKTAG		tag;
	Oid			queueid;
	int32		lockResult = 0;
	ResPortalIncrement	incData;


	queueid = portal->queueId;

	/* 
	 * Check we have a valid queue before going any further.
	 */
	if (queueid != InvalidOid)
	{

		/*
		 * Check the source tag to see if the original statement is suitable for
		 * locking. 
		 */
		switch (portal->sourceTag)
		{

			/*
			 * For INSERT/UPDATE/DELETE Skip if we have specified only SELECT,
			 * otherwise drop through to handle like a SELECT.
			 */
			case T_InsertStmt:
			case T_DeleteStmt:
			case T_UpdateStmt:
			{
				if (ResourceSelectOnly)
				{
					takeLock = false;
					returnReleaseOk = false;
					break;
				}
			}


			case T_SelectStmt:
			{
		
				/*
				 * Get the cost from the first element with a plan - the queries
				 * added by various rules have the complete cost charged to them
				 * (TODO Check this formally).
				 */
				foreach(planlist_item, portal->planTrees)
				{
					plan = (Plan *) lfirst(planlist_item);
			
	
					if (plan)
					{
						cost += plan->total_cost;
						break;
					}
	
				}
	
	
				/*
				 * Setup the resource portal increments, ready to be added.
				 */
				incData.pid = MyProc->pid;
				incData.portalId = portal->portalId;
				incData.increments[RES_COUNT_LIMIT] = 1;
				incData.increments[RES_COST_LIMIT] =  cost;
	
				takeLock = true;
				returnReleaseOk = true;
			}
			break;
	
			/*
			 * We are declaring a cursor, we get the lock, but do not let it 
			 * get released yet.
			 */
			case T_DeclareCursorStmt:
			{
	
				DeclareCursorStmt	*stmt = (DeclareCursorStmt *)((Query *)linitial(portal->parseTrees))->utilityStmt;
				Query		*query;
				List		*rewritten;
	
				/*
				 * We plan the query here, this is wasteful, as it will get 
				 * done again during ProcessUtility, but we need the plan now!
				 * (TODO push taking the lock down further for this case).
				 */
				query = copyObject(stmt->query);
	
				AcquireRewriteLocks(query);
				rewritten = QueryRewrite(query);
				query = (Query *) linitial(rewritten);
				plan = planner(query, true, stmt->options, NULL);
	
				cost = plan->total_cost;
	
	
				/*
				 * Update this unnamed portal with the (yet to be created) 
				 * cursor portal's id (we *know* that the cursor's portal is 
				 * created immediately after this, so we just give it the next 
				 * possible portal id). (TODO verify)
				 */
				portal->portalId = ResCreatePortalId("dummy", false);
	
	
				/*
				 * Setup the resource increments and add them.
				 */
				incData.pid = MyProc->pid;
				incData.portalId = portal->portalId;
				incData.increments[RES_COUNT_LIMIT] = 1;
				incData.increments[RES_COST_LIMIT] = cost;
	
	
				takeLock = true;
				returnReleaseOk = false;
			}
			break;
	
			/*
			 * We are closing a cursor, we hold a lock already, so indicate that
			 * unlock should happen.
			 */
			case T_ClosePortalStmt:
			{
	 			Query		*query;
	 			ClosePortalStmt	*stmt;
	 			Portal		cursorPortal;
	 
	 
	 			/* Find the named portal for this cursor. */
	 			query = (Query *)(linitial(portal->parseTrees));
	 			stmt = (ClosePortalStmt *) (query->utilityStmt);
	 
	 			cursorPortal = GetPortalByName(stmt->portalname);
	 			if (!PortalIsValid(cursorPortal))
	 			{
	 				ereport(ERROR,
	 						(errcode(ERRCODE_UNDEFINED_CURSOR),
	 						 errmsg("cursor \"%s\" does not exist", 
	 								stmt->portalname)));
	 			}       
	 
	 
	 			/* 
				 * Update this unnamed portal with the cursor portal's id
				 * and queue id, so we can find the cursor's increments 
				 * and subtract then off the various limit counters for the
				 * correct queue.
				 */
	 			portal->portalId = cursorPortal->portalId;
				portal->queueId = cursorPortal->queueId;
	
				takeLock = false;
				returnReleaseOk = true;
			}
			break;
	
			/*
			 * We do not want to lock any of these query types.
			 */
			default:
			{
	
				takeLock = false;
				returnReleaseOk = false;
			}
			break;
	
		}
	
	
		/*
		 * Get the resource lock.
		 */
		if(takeLock)
		{
			elog(DEBUG1, "acquire resource lock for queue %u (portal %u)", 
				 queueid, portal->portalId);
			SET_LOCKTAG_RESOURCE_QUEUE(tag, queueid);
			lockResult = ResLockAcquire(&tag, &incData);
		}

	}
	return returnReleaseOk;

}


/*
 * ResUnLockPortal -- release a resource lock for a portal.
 */
void
ResUnLockPortal(Portal portal)
{

	LOCKTAG					tag;
	Oid						queueid;

	queueid = portal->queueId;

	/* 
	 * Check we have a valid queue before going any further.
	 */
	if (queueid != InvalidOid)
	{
		elog(DEBUG1, "release resource lock for queue %u (portal %u)", 
				 queueid, portal->portalId);
		SET_LOCKTAG_RESOURCE_QUEUE(tag, queueid);


		ResLockRelease(&tag, portal->portalId);
	}

	return;
}


/*
 * GetResQueueForRole -- determine what resource queue a role is going to use.
 *
 * Notes
 *	This could be called for each of ResLockPortal and ResUnLockPortal, but we 
 *	can eliminate a relation open and lock if it is cached.
 */
Oid	
GetResQueueForRole(Oid roleid)
{
	Relation		authrel;
	HeapTuple		tup;
	Datum			datum;
	bool			isnull;
	TupleDesc		tupdesc;
	Oid				queueid = InvalidOid;

	authrel = heap_open(AuthIdRelationId, AccessShareLock);

    tupdesc = RelationGetDescr(authrel);

	tup = SearchSysCache(AUTHOID,
 						 ObjectIdGetDatum(roleid),
						 0, 0, 0);
	if (HeapTupleIsValid(tup))
	{
		datum = heap_getattr(tup, Anum_pg_authid_rolresqueue, tupdesc, &isnull);
		if (!isnull)
		{
			queueid = DatumGetObjectId(datum);
		}
		ReleaseSysCache(tup);
	} 

	heap_close(authrel, AccessShareLock);

	return queueid;
	
}


/*
 * SetResQueueId -- set the cached value for the current resource queue.
 *
 * Notes
 *	Needs to be called at session initialization and after (or in) SET ROLE.
 */
void
SetResQueueId(void)
{
	MyQueueId = GetResQueueForRole(GetUserId());

	return;
}


/*
 * GetResQueueId -- return the current cached value for the resource queue.
 */
Oid
GetResQueueId(void)
{
	return MyQueueId;
}


/*
 * ResQueueIdForName -- Return the Oid for a resource queue name
 *
 * Notes:
 *	Used by the various admin commands to convert a user supplied queue name
 *	to Oid.
 */
Oid
GetResQueueIdForName(char	*name)
{
	Oid					queueid = InvalidOid;
	Relation			relQueue;
	HeapTuple			tuple;
	ScanKeyData			key;
	SysScanDesc			rsqscan;

	/* Open the queue catalog and index scan on the name index */
	relQueue = heap_open(ResQueueRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_resqueue_rsqname,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				CStringGetDatum(name));

	rsqscan = systable_beginscan(relQueue, ResQueueRsqnameIndexId, true, 
								SnapshotNow, 1, &key);

	tuple = systable_getnext(rsqscan);
	if (HeapTupleIsValid(tuple))
	{
		queueid = HeapTupleGetOid(tuple);
	}

	systable_endscan(rsqscan);


	heap_close(relQueue, AccessShareLock);

	return queueid;
}


/*
 * ResCreatePortalId -- return an id for a portal
 *
 * Notes
 * 	We return a new id for any named portal, but zero for an un-named one. 
 *	This is because we use the id to track cursors, and there can only be one
 *	un-named portal active.
 *
 *	The "create" flag is used to decide if we should merely return
 *	what will be the next id or actually update the internal portal 
 *	counter. This slightly odd construction is needed because DECLARE CURSOR
 *	is processed *before* the named portal for the cursor is created!
 */
uint32
ResCreatePortalId(const char *name, bool create)
{

	uint32				id;
	ResPortalTag		portalTag;
	bool				found = false;


	/* Unnamed portal. */
	if (name[0] == '\0')
	{
		id = 0;
		return id;
	}


	/* Named portal. */
	if (create && lastPortalId != 0)
	{
		id = lastPortalId;
		lastPortalId = 0;
		return id;
	}
			

	/* 
	 * Search for a id we can re-use from a closed portal - we want to do this
	 * so we can have MaxResourcePortalsPerXact *open* tracked (named) portals 
	 * in a transaction.
	 */
	for (id = 1; id <= portalId; id++)
	{
		MemSet(&portalTag, 0, sizeof(ResPortalTag));
		portalTag.pid = MyProc->pid;
		portalTag.portalId = id;
		if (!ResIncrementFind(&portalTag))
		{
			/* 
			 * If this is a probe to see what the next portal id will be 
			 * then remember it. Have to do this, as we have no way of knowing 
			 * which id was free for reuse.
			 */
			found = true;
			if (!create)
				lastPortalId = id;
			break;
		}
	}

	/*
	 * No re-usable portal ids, check we have not exhaused the # of trackable
	 * portals and return the next id.
	 */
	if (!found)
	{
		if (portalId >= MaxResourcePortalsPerXact)
		{
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient portal ids available"),
			errhint("Increase max_resource_portals_per_transaction.")));

		}

		/* If we are really creating a portal, then update the internal count.*/
		if (create)
		{
			id = (++portalId);
		}
		else
		{
			id = portalId + 1;
		}
	}

	return id;
}


/*
 * AtCommit_ResScheduler -- do any pre-commit processing for Resource 
 *	Scheduling.
 */
void
AtCommit_ResScheduler(void)
{

	/* reset the portal id. */
	portalId = 0;
}


/*
 * AtAbort_ResScheduler -- do any abort processing for Resource 
 *	Scheduling.
 */
void
AtAbort_ResScheduler(void)
{

	/* reset the portal id. */
	portalId = 0;
}
