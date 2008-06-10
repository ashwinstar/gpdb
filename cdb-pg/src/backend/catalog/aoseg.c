/*-------------------------------------------------------------------------
 *
 * aoseg.c
 *	  This file contains routines to support creation of append-only segment
 *    entry tables. This file is identical in functionality to toasting.c that
 *    exists in the same directory. One is in charge of creating toast tables
 *    (pg_toast_<reloid>) and the other append only segment position tables 
 *    (pg_aoseg_<reloid>).
 *
 * Portions Copyright (c) 2008, Greenplum Inc.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "access/aosegfiles.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/aoseg.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


static bool create_aoseg_table(Relation rel, Oid aosegOid, Oid aosegIndexOid, Oid * comptypeOid);
static bool needs_aoseg_table(Relation rel);

/*
 * AlterTableCreateAoSegTable
 *		If the table needs an AO segment table, and doesn't already have one,
 *		then create a aoseg table for it.
 *
 * We expect the caller to have verified that the relation is AO table and have
 * already done any necessary permission checks.  Callers expect this function
 * to end with CommandCounterIncrement if it makes any changes.
 */
void
AlterTableCreateAoSegTable(Oid relOid)
{
	Relation	rel;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.  (This is probably redundant in all present
	 * uses...)
	 */
	rel = heap_open(relOid, AccessExclusiveLock);

	/* create_aoseg_table does all the work */
	(void) create_aoseg_table(rel, InvalidOid, InvalidOid, NULL);

	heap_close(rel, NoLock);
}

void
AlterTableCreateAoSegTableWithOid(Oid relOid, Oid newOid, Oid newIndexOid, Oid * comptypeOid)
{
	Relation	rel;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.  (This is probably redundant in all present
	 * uses...)
	 */
	rel = heap_open(relOid, AccessExclusiveLock);
	
	/* create_aoseg_table does all the work */
	(void) create_aoseg_table(rel, newOid, newIndexOid, comptypeOid);

	heap_close(rel, NoLock);
}

/*
 * create_aoseg_table --- internal workhorse
 *
 * rel is already opened and exclusive-locked
 * aosegOid and aosegIndexOid are normally InvalidOid
 */
static bool
create_aoseg_table(Relation rel, Oid aosegOid, Oid aosegIndexOid, Oid * comptypeOid)
{
	Oid			relOid = RelationGetRelid(rel);
	HeapTuple	reltup;
	TupleDesc	tupdesc;
	bool		shared_relation;
	Relation	class_rel;
	Oid			aoseg_relid;
	Oid			aoseg_idxid;
	char		aoseg_relname[NAMEDATALEN];
	char		aoseg_idxname[NAMEDATALEN];
	IndexInfo  *indexInfo;
	Oid			classObjectId[1];
	ObjectAddress baseobject,
				aosegobject;

	shared_relation = rel->rd_rel->relisshared;
	
	/* can't have shared AO tables after initdb */
	/* TODO: disallow it at CREATE TABLE time */
	Assert(!(shared_relation && !IsBootstrapProcessingMode()) );

	/*
	 * Was a aoseg table already created?
	 */
	if (rel->rd_rel->relaosegrelid != InvalidOid)
		return false;

	/*
	 * Check to see whether the table actually needs a TOAST table.
	 */
	if (!needs_aoseg_table(rel))
		return false;

	/* can aoseg ever be created during bootstrap? don't think so */
	Assert(!IsBootstrapProcessingMode());

	/*
	 * Create the aoseg table and its index
	 */
	snprintf(aoseg_relname, sizeof(aoseg_relname),
			 "pg_aoseg_%u", relOid);
	snprintf(aoseg_idxname, sizeof(aoseg_idxname),
			 "pg_aoseg_%u_index", relOid);

	/* this is pretty painful...  need a tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(4, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1,
					   "segno",
					   INT4OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2,
					   "eof",
					   FLOAT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3,
					   "tupcount",
					   FLOAT8OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4,
					   "varblockcount",
					   FLOAT8OID,
					   -1, 0);

	/*
	 * Note: the aoseg relation is placed in the regular pg_aoseg namespace
	 * even if its master relation is a temp table.  There cannot be any
	 * naming collision, and the aoseg rel will be destroyed when its master
	 * is, so there's no need to handle the aoseg rel as temp.
	 *
	 * XXX would it make sense to apply the master's reloptions to the aoseg
	 * table?
	 */
	aoseg_relid = heap_create_with_catalog(aoseg_relname,
										   PG_AOSEGMENT_NAMESPACE,
										   rel->rd_rel->reltablespace,
										   aosegOid,
										   rel->rd_rel->relowner,
										   tupdesc,
										   RELKIND_AOSEGMENTS,
										   shared_relation,
										   true,
										   0,
										   ONCOMMIT_NOOP,
										   NULL, /* CDB POLICY */
										   (Datum) 0,
										   true,
										   comptypeOid);

	/* make the toast relation visible, else index creation will fail */
	CommandCounterIncrement();

	/*
	 * Create unique index on segno.
	 */
	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = 1;
	indexInfo->ii_KeyAttrNumbers[0] = 1;
	indexInfo->ii_Expressions = NIL;
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_Predicate = NIL;
	indexInfo->ii_PredicateState = NIL;
	indexInfo->ii_Unique = true;
	indexInfo->ii_Concurrent = false;

	classObjectId[0] = INT4_BTREE_OPS_OID;

	aoseg_idxid = index_create(aoseg_relid, aoseg_idxname, aosegIndexOid,
							   indexInfo,
							   BTREE_AM_OID,
							   rel->rd_rel->reltablespace,
							   classObjectId, (Datum) 0,
							   true, false, true, false, false);

	/*
	 * Store the aoseg table's OID in the parent relation's pg_class row
	 */
	class_rel = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(relOid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	((Form_pg_class) GETSTRUCT(reltup))->relaosegrelid = aoseg_relid;

	
	/* normal case, use a transactional update */
	simple_heap_update(class_rel, &reltup->t_self, reltup);

	/* Keep catalog indexes current */
	CatalogUpdateIndexes(class_rel, reltup);

	heap_freetuple(reltup);

	heap_close(class_rel, RowExclusiveLock);

	/*
	 * Register dependency from the aoseg table to the master, so that the
	 * aoseg table will be deleted if the master is.  	 
	 */
	baseobject.classId = RelationRelationId;
	baseobject.objectId = relOid;
	baseobject.objectSubId = 0;
	aosegobject.classId = RelationRelationId;
	aosegobject.objectId = aoseg_relid;
	aosegobject.objectSubId = 0;

	recordDependencyOn(&aosegobject, &baseobject, DEPENDENCY_INTERNAL);

	/*
	 * Make changes visible
	 */
	CommandCounterIncrement();
	
	return true;
}

/*
 * Check to see whether the table needs an aoseg table.	It does only if it is
 * an append-only relation.
 */
static bool
needs_aoseg_table(Relation rel)
{
	return (rel->rd_rel->relkind == RELKIND_APPEND_ONLY);
}
