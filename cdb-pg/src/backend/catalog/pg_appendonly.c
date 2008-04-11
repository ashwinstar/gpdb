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
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"


/*
 * Get the catalog entry for an appendonly relation
 */
AppendOnlyEntry *
GetAppendOnlyEntry(Oid relfilenode)
{
	
	Relation	pg_appendonly_rel;
	TupleDesc	pg_appendonly_dsc;
	HeapTuple	tuple;
	ScanKeyData	key;
	SysScanDesc	aoscan;
	Datum		eof, tupcount, varbcount;
	bool		isNull;
	AppendOnlyEntry *aoentry;
	
	/*
	 * Check the pg_appendonly relation to be certain the ao table 
	 * is there. 
	 */
	pg_appendonly_rel = heap_open(AppendOnlyRelationId, RowExclusiveLock);
	pg_appendonly_dsc = RelationGetDescr(pg_appendonly_rel);
	
	ScanKeyInit(&key,
				Anum_pg_appendonly_relfilenode,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relfilenode));
	
    aoscan = systable_beginscan(pg_appendonly_rel, 
								AppendOnlyRelfilenodeIndexId, 
								true,
								SnapshotNow, 
								1, 
								&key);
	
    tuple = systable_getnext(aoscan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("append-only table relfilenode \"%d\" does not exist",
						relfilenode)));
	
	
	aoentry = (AppendOnlyEntry *) palloc(sizeof(AppendOnlyEntry));
	
	/* get the eof */
	eof = heap_getattr(tuple, 
					   Anum_pg_appendonly_eof, 
					   pg_appendonly_rel->rd_att, 
					   &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid eof value: NULL")));	

	/* get the tupcount */
	tupcount = heap_getattr(tuple, 
							Anum_pg_appendonly_tupcount, 
							pg_appendonly_rel->rd_att, 
							&isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid tupcount value: NULL")));	
	
	/* get the varblock count */
	varbcount = heap_getattr(tuple, 
		 					 Anum_pg_appendonly_varblockcount, 
							 pg_appendonly_rel->rd_att, 
							 &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid varblockcount value: NULL")));	
	
	aoentry->eof = DatumGetFloat8(eof);
	aoentry->tupcount = DatumGetFloat8(tupcount);
	aoentry->varblockcount = DatumGetFloat8(varbcount);
	
	/* Finish up scan and close appendonly catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly_rel, RowExclusiveLock);
	
	return aoentry;
}

/*
 * Update the eof and filetupcount of an append only table.
 */
void
UpdateAppendOnlyEntry(Oid relfilenode, 
					  int64 eof, 
					  float8 tuples_added, 
					  float8 varblocks_added)
{
	Relation			pg_appendonly;
	TupleDesc			pg_appendonly_dsc;
	ScanKeyData			key;
	SysScanDesc			aoscan;
	HeapTuple			tuple, new_tuple;
	Datum				filetupcount;
	Datum				filevarblockcount;
	Datum				new_tuple_count;
	Datum				new_varblock_count;
	Datum				new_record[Natts_pg_appendonly];
	char				new_record_nulls[Natts_pg_appendonly];
	char				new_record_repl[Natts_pg_appendonly];
	bool				isNull;
	
	elog(DEBUG3, "updateAppendOnlyEntry called. relfilenode = %d",
		 relfilenode);
	
	pg_appendonly = heap_open(AppendOnlyRelationId, RowExclusiveLock);
	pg_appendonly_dsc = RelationGetDescr(pg_appendonly);
	
	ScanKeyInit(&key,
				Anum_pg_appendonly_relfilenode,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relfilenode));
	
	aoscan = systable_beginscan(pg_appendonly, 
								AppendOnlyRelfilenodeIndexId, 
								true,
								SnapshotNow, 
								1, 
								&key);
	
	/*
	 * get the tuple to update
	 */
	tuple = systable_getnext(aoscan);
	
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("appendonly relfilenode \"%d\" does not exist",
						relfilenode)));
	
	/* TODO: get more tuples that qualify when enabling multiple inserters */
	
	
	/* get the current tuple count so we can add to it */
	filetupcount = heap_getattr(tuple, 
								Anum_pg_appendonly_tupcount, 
								pg_appendonly->rd_att, 
								&isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid pg_appendonly filetupcount value: NULL")));	
	
	/* calculate the new tuple count */
	new_tuple_count = DirectFunctionCall2(float8pl, 
										  filetupcount, 
										  Float8GetDatum(tuples_added));
	
	/* get the current varblock count so we can add to it */
	filevarblockcount = heap_getattr(tuple, 
									 Anum_pg_appendonly_varblockcount, 
									 pg_appendonly->rd_att, 
									 &isNull);
		
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid pg_appendonly varblockcount value: NULL")));	

	/* calculate the new tuple count */
	new_varblock_count = DirectFunctionCall2(float8pl, 
											 filevarblockcount, 
											 Float8GetDatum(varblocks_added));
	
		
	/* 
	 * Build a tuple to update 
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));
	
	new_record[Anum_pg_appendonly_eof - 1] = Float8GetDatum((float8)eof);
	new_record_repl[Anum_pg_appendonly_eof - 1] = 'r';
	
	new_record[Anum_pg_appendonly_tupcount - 1] = new_tuple_count;
	new_record_repl[Anum_pg_appendonly_tupcount - 1] = 'r';

	new_record[Anum_pg_appendonly_varblockcount - 1] = new_varblock_count;
	new_record_repl[Anum_pg_appendonly_varblockcount - 1] = 'r';

	/*
	 * update the tuple in the pg_appendonly table
	 */
	new_tuple = heap_modifytuple(tuple, pg_appendonly_dsc, new_record, 
								 new_record_nulls, new_record_repl);
	
	simple_heap_update(pg_appendonly, &tuple->t_self, new_tuple);
	CatalogUpdateIndexes(pg_appendonly, new_tuple);
	
	heap_freetuple(new_tuple);
	
	/* Finish up scan and close append-only catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly, RowExclusiveLock);	
}

/*
 * Remove all pg_appendonly entries that the table we are DROPing
 * refers to (using the table's relfilenode)
 */
void
RemoveAppendonlyEntries(Oid relfilenode)
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
				Anum_pg_appendonly_relfilenode,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relfilenode));
	
	
	aoscan = systable_beginscan(pg_appendonly_rel, 
								AppendOnlyRelfilenodeIndexId, 
								true,
								SnapshotNow, 
								1, 
								&key);
	
	tuple = systable_getnext(aoscan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("appendonly table relfilenode id \"%d\" does not exist",
						relfilenode)));
	
	/*
	 * Delete the appendonly table entry from the catalog (pg_appendonly).
	 */
	simple_heap_delete(pg_appendonly_rel, &tuple->t_self);
	
	
	/* Finish up scan and close appendonly catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly_rel, NoLock);
}


