/*-------------------------------------------------------------------------
 *
 * queue.c
 *	  Commands for manipulating resource queues.
 *
 * Copyright (c) 2006, Greenplum, Inc.
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_resqueue.h"
#include "cdb/cdbvars.h"
#include "commands/defrem.h"
#include "commands/queue.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/resscheduler.h"
#include "utils/syscache.h"


/*
 * CREATE RESOURCE QUEUE
 */
void
CreateQueue(CreateQueueStmt *stmt)
{
	Relation	pg_resqueue_rel;
	TupleDesc	pg_resqueue_dsc;
	HeapTuple	tuple;
	ScanKeyData	key;
	SysScanDesc	rsqscan;
	Oid			queueid;
	Cost		thresholds[NUM_RES_LIMIT_TYPES];
	Datum		new_record[Natts_pg_resqueue];
	char		new_record_nulls[Natts_pg_resqueue];
	ListCell    *option;

	/* thresholds for active and cost limiters. */
	Cost		activelimit = INVALID_RES_LIMIT_THRESHOLD;	
	Cost		costlimit = INVALID_RES_LIMIT_THRESHOLD;

	/* overcommit indicator */
	bool		overcommit = false;

	/* cost ignore limit for queries */
	float4		ignorelimit = 0;

	DefElem     *dactivelimit = NULL;
	DefElem     *dcostlimit = NULL;
	DefElem     *dovercommit = NULL;
	DefElem     *dignorelimit = NULL;
	bool		queueok = false;


	/* Permission check - only superuser can create queues. */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create resource queues")));

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "activelimit") == 0)
		{
			if (dactivelimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dactivelimit = defel;
		}
		else if (strcmp(defel->defname, "costlimit") == 0)
		{
			if (dcostlimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcostlimit = defel;
		}
		else if (strcmp(defel->defname, "overcommit") == 0)
		{
			if (dovercommit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dovercommit = defel;
		}
		else if (strcmp(defel->defname, "ignorecostlimit") == 0)
		{
			if (dignorelimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dignorelimit = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/* Perform range checks on the various thresholds.*/
	if (dactivelimit)
	{
		activelimit = (Cost) defGetInt64(dactivelimit);
		if (!(activelimit == INVALID_RES_LIMIT_THRESHOLD || (activelimit > 0)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("active threshold cannot be less than %d or equal to 0", 
							INVALID_RES_LIMIT_THRESHOLD)));
	}

	if (dcostlimit)
	{
		costlimit = (Cost) defGetNumeric(dcostlimit);
		if (!(costlimit == INVALID_RES_LIMIT_THRESHOLD || (costlimit > 0)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cost threshold cannot be less than %d or equal to 0", 
							INVALID_RES_LIMIT_THRESHOLD)));
	}

	if(dovercommit)
	{
		overcommit = intVal(dovercommit->arg);
	}

	if(dignorelimit)
	{
		ignorelimit = (Cost) defGetNumeric(dignorelimit);
		if (ignorelimit < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ignore cost threshold cannot be less than %d or equal to 0", 
							INVALID_RES_LIMIT_THRESHOLD)));
	}


	/*
	 * Check that at least one threshold is to be set.
	 */
	if ( !dactivelimit && !dcostlimit)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("at least one threshold must be specified")));

	/* Check all thresholds are not invalid. */
	if (activelimit == INVALID_RES_LIMIT_THRESHOLD &&
		costlimit   == INVALID_RES_LIMIT_THRESHOLD)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("at least one valid threshold must be specified")));

	/*
	 * Check for an illegal name ('none' is used to signify no queue in ALTER
	 * ROLE).
	 */
	if (strcmp(stmt->queue, "none") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("resource queue name \"%s\" is reserved",
						stmt->queue)));

	/*
	 * Check the pg_resqueue relation to be certain the queue doesn't already
	 * exist. 
	 */
	pg_resqueue_rel = heap_open(ResQueueRelationId, RowExclusiveLock);
	pg_resqueue_dsc = RelationGetDescr(pg_resqueue_rel);

	ScanKeyInit(&key,
				Anum_pg_resqueue_rsqname,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				CStringGetDatum(stmt->queue));

    rsqscan = systable_beginscan(pg_resqueue_rel, ResQueueRsqnameIndexId, true,
                                SnapshotNow, 1, &key);

    tuple = systable_getnext(rsqscan);
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("resource queue \"%s\" already exists",
						stmt->queue)));

	/*
	 * Build a tuple to insert
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));

	new_record[Anum_pg_resqueue_rsqname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->queue));

	new_record[Anum_pg_resqueue_rsqcountlimit - 1] = 
		Float4GetDatum(activelimit);

	new_record[Anum_pg_resqueue_rsqcostlimit - 1] = 
		Float4GetDatum(costlimit);

	new_record[Anum_pg_resqueue_rsqovercommit - 1] = 
		BoolGetDatum(overcommit);

	new_record[Anum_pg_resqueue_rsqignorecostlimit - 1] = 
		Float4GetDatum(ignorelimit);


	tuple = heap_formtuple(pg_resqueue_dsc, new_record, new_record_nulls);

	/*
	 * Insert new record in the pg_resqueue table
	 */
	queueid = simple_heap_insert(pg_resqueue_rel, tuple);

	/*
	 * Create the in-memory resource queue, if resource scheduling is on,
	 * otherwise don't - and gripe a little about it.
	 */
	if (Gp_role == GP_ROLE_DISPATCH && ResourceScheduler)
	{
		LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

		thresholds[RES_COUNT_LIMIT] = activelimit;
		thresholds[RES_COST_LIMIT] = costlimit;

		queueok = ResCreateQueue(queueid, thresholds, overcommit, ignorelimit);

		LWLockRelease(ResQueueLock);

		if (!queueok)
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					errmsg("insufficient resource queues available"),
					errhint("Increase max_resource_queues")));
	}
	else
	{
		ereport(WARNING,
				(errmsg("resource scheduling is disabled"),
				 errhint("To enable set resource_scheduler=on")));
	}

	/* Finish up scan and close resource queue catalog. */
	systable_endscan(rsqscan);

	CatalogUpdateIndexes(pg_resqueue_rel, tuple);

	heap_close(pg_resqueue_rel, NoLock);
}


/*
 * ALTER RESOURCE QUEUE
 */
void
AlterQueue(AlterQueueStmt *stmt)
{
	Relation	pg_resqueue_rel;
	TupleDesc	pg_resqueue_dsc;
	HeapTuple	tuple, new_tuple;
	ScanKeyData	key;
	SysScanDesc	rsqscan;
	Oid			queueid;
	Cost		thresholds[NUM_RES_LIMIT_TYPES];
	Datum		new_record[Natts_pg_resqueue];
	char		new_record_nulls[Natts_pg_resqueue];
	char		new_record_repl[Natts_pg_resqueue];
	ListCell    *option;

	/* thresholds for active and cost limiters. */
	Cost		activelimit = INVALID_RES_LIMIT_THRESHOLD;
	Cost		costlimit = INVALID_RES_LIMIT_THRESHOLD;

	/* overcommit indicator */
	bool		overcommit = false;

	/* cost ignore limit for queries */
	float4		ignorelimit = 0;

	DefElem     *dactivelimit = NULL;
	DefElem     *dcostlimit = NULL;
	DefElem     *dovercommit = NULL;
	DefElem     *dignorelimit = NULL;
	ResAlterQueueResult		queueok;


	/* Permission check - only superuser can alter queues. */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to alter resource queues")));

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "activelimit") == 0)
		{
			if (dactivelimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dactivelimit = defel;
		}
		else if (strcmp(defel->defname, "costlimit") == 0)
		{
			if (dcostlimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcostlimit = defel;
		}
		else if (strcmp(defel->defname, "overcommit") == 0)
		{
			if (dovercommit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dovercommit = defel;
		}
		else if (strcmp(defel->defname, "ignorecostlimit") == 0)
		{
			if (dignorelimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dignorelimit = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/* Perform range checks on the various thresholds. */
	if (dactivelimit)
	{
		activelimit = (Cost) defGetInt64(dactivelimit);
		if (!(activelimit == INVALID_RES_LIMIT_THRESHOLD || (activelimit > 0)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("active threshold cannot be less than %d or equal to 0", 
							INVALID_RES_LIMIT_THRESHOLD)));
	}

	if (dcostlimit)
	{
		costlimit = (Cost) defGetNumeric(dcostlimit);
		if (!(costlimit == INVALID_RES_LIMIT_THRESHOLD || (costlimit > 0)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cost threshold must be equal to %d or greater than 0", 
							INVALID_RES_LIMIT_THRESHOLD)));
	}

	if (dovercommit)
	{
		overcommit = intVal(dovercommit->arg);
	}

	if(dignorelimit)
	{
		ignorelimit = (Cost) defGetNumeric(dignorelimit);
		if (ignorelimit < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ignore cost threshold cannot be less than %d or equal to 0", 
							INVALID_RES_LIMIT_THRESHOLD)));
	}

	/*
	 * Check that at least one threshold is to be set.
	 */
	if (!dactivelimit && !dcostlimit && !dovercommit && !dignorelimit)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("at least one threshold, overcommit or ignore limit must be specified")));


	/*
	 * Check the pg_resqueue relation to be certain the queue already
	 * exists. 
	 */
	pg_resqueue_rel = heap_open(ResQueueRelationId, RowExclusiveLock);
	pg_resqueue_dsc = RelationGetDescr(pg_resqueue_rel);

	ScanKeyInit(&key,
				Anum_pg_resqueue_rsqname,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				CStringGetDatum(stmt->queue));

    rsqscan = systable_beginscan(pg_resqueue_rel, ResQueueRsqnameIndexId, true,
                                SnapshotNow, 1, &key);

    tuple = systable_getnext(rsqscan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource queue \"%s\" does not exist",
						stmt->queue)));

	/*
	 * Remember the Oid and current thresholds, for updating the in-memory
	 * queue later.
	 */
	queueid = HeapTupleGetOid(tuple);
	thresholds[RES_COUNT_LIMIT] = 
		((Form_pg_resqueue) GETSTRUCT(tuple))->rsqcountlimit;
	thresholds[RES_COST_LIMIT] = 
		((Form_pg_resqueue) GETSTRUCT(tuple))->rsqcostlimit;

	/* Also set overcommit if it was *not* specified. */
	if (!dovercommit)
		overcommit = ((Form_pg_resqueue) GETSTRUCT(tuple))->rsqovercommit;

	/* Also set ignore cost limit if it was *not* specified. */
	if (!dignorelimit)
		ignorelimit = ((Form_pg_resqueue) GETSTRUCT(tuple))->rsqignorecostlimit;


	/*
	 * Build a tuple to update.
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	if (dactivelimit)
	{
		new_record[Anum_pg_resqueue_rsqcountlimit - 1] = 
			Float4GetDatum(activelimit);
		new_record_repl[Anum_pg_resqueue_rsqcountlimit - 1] = 'r';

		thresholds[RES_COUNT_LIMIT] = activelimit;
		
	}

	if (dcostlimit)
	{
		new_record[Anum_pg_resqueue_rsqcostlimit - 1] = 
			Float4GetDatum(costlimit);
		new_record_repl[Anum_pg_resqueue_rsqcostlimit - 1] = 'r';

		thresholds[RES_COST_LIMIT] = costlimit;
	}

	if (dovercommit)
	{
		new_record[Anum_pg_resqueue_rsqovercommit - 1] = 
			BoolGetDatum(overcommit);
		new_record_repl[Anum_pg_resqueue_rsqovercommit - 1] = 'r';

	}

	if (dignorelimit)
	{
		new_record[Anum_pg_resqueue_rsqignorecostlimit - 1] = 
			Float4GetDatum(ignorelimit);
		new_record_repl[Anum_pg_resqueue_rsqignorecostlimit - 1] = 'r';
	}


	/*
	 * Check we are not going to set all the thresholds to be invalid.
	 */
	if (thresholds[RES_COUNT_LIMIT] == INVALID_RES_LIMIT_THRESHOLD &&
		thresholds[RES_COST_LIMIT] == INVALID_RES_LIMIT_THRESHOLD)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("at least one threshold must be different from %d",
						INVALID_RES_LIMIT_THRESHOLD)));

	/*
	 * update the tuple in the pg_resqueue table
	 */
	new_tuple = heap_modifytuple(tuple, pg_resqueue_dsc, new_record, 
								 new_record_nulls, new_record_repl);

	simple_heap_update(pg_resqueue_rel, &tuple->t_self, new_tuple);
	CatalogUpdateIndexes(pg_resqueue_rel, new_tuple);

	heap_freetuple(new_tuple);

	/*
	 * If resource scheduling is on, see if we can change the threshold(s)
	 * for the in-memory queue.
	 * otherwise don't - and gripe a little about it.
	 */
	if (Gp_role == GP_ROLE_DISPATCH && ResourceScheduler)
	{
		LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

		queueok = ResAlterQueue(queueid, thresholds, overcommit, ignorelimit);

		LWLockRelease(ResQueueLock);

		if (queueok != ALTERQUEUE_OK)
		{
			if (queueok == ALTERQUEUE_SMALL_THRESHOLD)
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_LIMIT_VALUE),
						errmsg("thresholds cannot be less than current values")));
			else if (queueok == ALTERQUEUE_OVERCOMMITTED)
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_LIMIT_VALUE),
						errmsg("disabling overcommit cannot leave queue in possibly overcommitted state")));
			else
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("queue hash table corrupted")));

		}

	}
	else
	{
		ereport(WARNING,
				(errmsg("resource scheduling is disabled"),
				 errhint("To enable set resource_scheduler=on")));
	}

	/* Finish up scan and close resource queue catalog. */
	systable_endscan(rsqscan);

	heap_close(pg_resqueue_rel, NoLock);
}


/*
 * DROP RESOURCE QUEUE
 */
void
DropQueue(DropQueueStmt *stmt)
{
	Relation	pg_resqueue_rel, pg_authid_rel;
	TupleDesc	pg_resqueue_dsc, pg_authid_dsc;
	HeapTuple	tuple, roltuple;
	ScanKeyData	key, rolekey;
	SysScanDesc	rsqscan, rolscan;
	Oid			queueid;
	bool		queueok = false;


	/* Permission check - only superuser can drop queues. */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop resource queues")));


	/*
	 * Check the pg_resqueue relation to be certain the queue already
	 * exists. 
	 */
	pg_resqueue_rel = heap_open(ResQueueRelationId, RowExclusiveLock);
	pg_resqueue_dsc = RelationGetDescr(pg_resqueue_rel);

	ScanKeyInit(&key,
				Anum_pg_resqueue_rsqname,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				CStringGetDatum(stmt->queue));

    rsqscan = systable_beginscan(pg_resqueue_rel, ResQueueRsqnameIndexId, true,
                                SnapshotNow, 1, &key);

    tuple = systable_getnext(rsqscan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource queue \"%s\" does not exist",
						stmt->queue)));

	/*
	 * Remember the Oid, for destroying the in-memory
	 * queue later.
	 */
	queueid = HeapTupleGetOid(tuple);


	/*
	 * Check to see if any roles are in this queue.
	 */
	pg_authid_rel = heap_open(AuthIdRelationId, AccessShareLock);
	pg_authid_dsc = RelationGetDescr(pg_authid_rel);

	ScanKeyInit(&rolekey,
				Anum_pg_authid_rolresqueue,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(queueid));

    rolscan = systable_beginscan(pg_authid_rel, AuthIdRolResQueueIndexId, true,
                                SnapshotNow, 1, &rolekey);
    roltuple = systable_getnext(rolscan);
	if (HeapTupleIsValid(roltuple))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("resource queue \"%s\" is used by at least one role",
						stmt->queue)));

	systable_endscan(rolscan);

	heap_close(pg_authid_rel, AccessShareLock);

	/*
	 * Delete the queue from the catalog.
	 */
	simple_heap_delete(pg_resqueue_rel, &tuple->t_self);

	/*
	 * If resource scheduling is on, see if we can destroy the in-memory queue.
	 * otherwise don't - and gripe a little about it.
	 */
	if (Gp_role == GP_ROLE_DISPATCH && ResourceScheduler)
	{
		LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

		queueok = ResDestroyQueue(queueid);

		LWLockRelease(ResQueueLock);

		if (!queueok)
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					errmsg("resource queue cannot be dropped as is in use")));
	}
	else
	{
		ereport(WARNING,
				(errmsg("resource scheduling is disabled"),
				 errhint("To enable set resource_scheduler=on")));
	}

	/* Finish up scan and close resource queue catalog. */
	systable_endscan(rsqscan);

	heap_close(pg_resqueue_rel, NoLock);
}
