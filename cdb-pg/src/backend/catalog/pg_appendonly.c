/*-------------------------------------------------------------------------
*
* pg_appendonly.c
*	  routines to support manipulation of the pg_appendonly relation
*
* Portions Copyright (c) 2008, Greenplum Inc
* Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"

#include "catalog/pg_appendonly.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"

/*
 * Adds an entry into the pg_appendonly catalog table. The entry
 * includes the new relfilenode of the appendonly relation that 
 * was just created and an initial eof and reltuples values of 0
 */
void
InsertAppendOnlyEntry(Oid relid, 
					  int blocksize, 
					  int safefswritesize, 
					  int compresslevel,
					  bool checksum)
{
	Relation	pg_appendonly_rel;
	HeapTuple	pg_appendonly_tuple = NULL;
	char		nulls[Natts_pg_appendonly];
	Datum		values[Natts_pg_appendonly];
	
	/* these are hardcoded for now. in the future should get passed in */
	const int	majorversion = 1;
	const int	minorversion = 0;
	
 	MemSet(values, 0, sizeof(values));
	MemSet(nulls, ' ', sizeof(nulls));
	
    /*
     * Open and lock the pg_appendonly catalog.
     */
	pg_appendonly_rel = heap_open(AppendOnlyRelationId, RowExclusiveLock);
	
	values[Anum_pg_appendonly_relid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_appendonly_blocksize - 1] = Int32GetDatum(blocksize);
	values[Anum_pg_appendonly_safefswritesize - 1] = Int32GetDatum(safefswritesize);
	values[Anum_pg_appendonly_compresslevel - 1] = Int32GetDatum(compresslevel);
	values[Anum_pg_appendonly_majorversion - 1] = Int32GetDatum(majorversion);
	values[Anum_pg_appendonly_minorversion - 1] = Int32GetDatum(minorversion);
	values[Anum_pg_appendonly_checksum - 1] = BoolGetDatum(checksum);
	
	/*
	 * form the tuple and insert it
	 */
	pg_appendonly_tuple = heap_formtuple(RelationGetDescr(pg_appendonly_rel), 
										 values, nulls);
	simple_heap_insert(pg_appendonly_rel, pg_appendonly_tuple);
	CatalogUpdateIndexes(pg_appendonly_rel, pg_appendonly_tuple);
	
	/*
     * Close the pg_appendonly_rel relcache entry without unlocking.
     * We have updated the catalog: consequently the lock must be 
	 * held until end of transaction.
     */
    heap_close(pg_appendonly_rel, NoLock);
	
}

/*
 * Get the catalog entry for an appendonly relation
 */
AppendOnlyEntry *
GetAppendOnlyEntry(Oid relid)
{
	
	Relation	pg_appendonly_rel;
	TupleDesc	pg_appendonly_dsc;
	HeapTuple	tuple;
	ScanKeyData	key;
	SysScanDesc	aoscan;
	Datum		blocksize,
				safefswritesize,
				compresslevel,
				majorversion,
				minorversion;
	bool		checksum,
				isNull;
	AppendOnlyEntry *aoentry;
	
	/*
	 * Check the pg_appendonly relation to be certain the ao table 
	 * is there. 
	 */
	pg_appendonly_rel = heap_open(AppendOnlyRelationId, RowExclusiveLock);
	pg_appendonly_dsc = RelationGetDescr(pg_appendonly_rel);
	
	ScanKeyInit(&key,
				Anum_pg_appendonly_relid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relid));
	
    aoscan = systable_beginscan(pg_appendonly_rel, 
								AppendOnlyRelidIndexId, 
								true,
								SnapshotNow, 
								1, 
								&key);
	
    tuple = systable_getnext(aoscan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("append-only table relid \"%d\" does not existin pg_appendonly",
						relid)));
	
	
	aoentry = (AppendOnlyEntry *) palloc(sizeof(AppendOnlyEntry));
	
	/* get the blocksize */
	blocksize = heap_getattr(tuple, 
							 Anum_pg_appendonly_blocksize, 
							 pg_appendonly_rel->rd_att, 
							 &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid blocksize value: NULL")));	
	
	/* get the safefswritesize */
	safefswritesize = heap_getattr(tuple, 
								   Anum_pg_appendonly_safefswritesize, 
								   pg_appendonly_rel->rd_att, 
								   &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid safe fs write size value: NULL")));	
	
	/* get the compresslevel */
	compresslevel = heap_getattr(tuple, 
								 Anum_pg_appendonly_compresslevel, 
								 pg_appendonly_rel->rd_att, 
								 &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid compresslevel value: NULL")));	

	/* get the majorversion */
	majorversion = heap_getattr(tuple, 
								 Anum_pg_appendonly_majorversion, 
								 pg_appendonly_rel->rd_att, 
								 &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid major version value: NULL")));	

	/* get the minorversion */
	minorversion = heap_getattr(tuple, 
								Anum_pg_appendonly_minorversion, 
								pg_appendonly_rel->rd_att, 
								&isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid minor version value: NULL")));	
	
	checksum = heap_getattr(tuple, 
							Anum_pg_appendonly_checksum, 
							pg_appendonly_rel->rd_att, 
							&isNull);
	
	aoentry->blocksize = DatumGetInt32(blocksize);
	aoentry->safefswritesize = DatumGetInt32(safefswritesize);
	aoentry->compresslevel = DatumGetInt16(compresslevel);
	aoentry->majorversion = DatumGetInt16(majorversion);
	aoentry->minorversion = DatumGetInt16(minorversion);
	aoentry->checksum = DatumGetBool(checksum);
	
	/* Finish up scan and close appendonly catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly_rel, RowExclusiveLock);
	
	return aoentry;
}

/*
 * Remove all pg_appendonly entries that the table we are DROPing
 * refers to (using the table's relfilenode)
 */
void
RemoveAppendonlyEntry(Oid relid)
{
	Relation	pg_appendonly_rel;
	TupleDesc	pg_appendonly_dsc;
	HeapTuple	tuple;
	ScanKeyData	key;
	SysScanDesc	aoscan;	
	
	/*
	 * now remove the pg_appendonly entry 
	 */
	pg_appendonly_rel = heap_open(AppendOnlyRelationId, RowExclusiveLock);
	pg_appendonly_dsc = RelationGetDescr(pg_appendonly_rel);
	
	ScanKeyInit(&key,
				Anum_pg_appendonly_relid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relid));
	
	
	aoscan = systable_beginscan(pg_appendonly_rel, 
								AppendOnlyRelidIndexId, 
								true,
								SnapshotNow, 
								1, 
								&key);
	
	tuple = systable_getnext(aoscan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("appendonly table relid \"%d\" does not exist in "
						"pg_appendonly", relid)));
	
	/*
	 * Delete the appendonly table entry from the catalog (pg_appendonly).
	 */
	simple_heap_delete(pg_appendonly_rel, &tuple->t_self);
	
	
	/* Finish up scan and close appendonly catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly_rel, NoLock);
}


