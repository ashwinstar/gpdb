/*-------------------------------------------------------------------------
*
* aosegfiles.c
*	  routines to support manipulation of the pg_aoseg_<oid> relation
*	  that accompanies each Append Only relation.
*
* Portions Copyright (c) 2008, Greenplum Inc
* Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
*
*-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "miscadmin.h"
#include "access/heapam.h"
#include "access/aosegfiles.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"

/*
 * GetBlankFileSegInfo
 *
 * get a blank file segment entry. usually called as a workaround for non
 * existing entry (this is a temporary workaround, this function will probably
 * be removed in the near future).
 */
FileSegInfo *
GetBlankFileSegInfo(void)
{	
	FileSegInfo *fsinfo = (FileSegInfo *) palloc(sizeof(FileSegInfo));

	/* initial file segment */
	fsinfo->segno = 0;
	fsinfo->eof = 0;
	fsinfo->tupcount = 0;
	fsinfo->varblockcount = 0;
	
	return fsinfo;
}

/*
 * InsertFileSegInfo
 *
 * Adds an entry into the pg_aoseg_*  table for this Append 
 * Only relation
 */
void
InsertFileSegInfo(Relation parentrel, 
				  int segno, 
				  float8 eof,
				  float8 tupcount,
				  float8 varblockcount)
{
	Relation	pg_aoseg_rel;
	Relation	pg_aoseg_idx;
	TupleDesc	pg_aoseg_dsc;	
	HeapTuple	pg_aoseg_tuple = NULL;
	char		nulls[Natts_pg_aoseg];
	Datum		values[Natts_pg_aoseg];
	
 	MemSet(values, 0, sizeof(values));
	MemSet(nulls, ' ', sizeof(nulls));
	
	/*
	 * Open the toast relation and its index.  We can use the index to check
	 * uniqueness of the OID we assign to the toasted item, even though it has
	 * additional columns besides OID.
	 */
	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, RowExclusiveLock);

	pg_aoseg_dsc = pg_aoseg_rel->rd_att;
	pg_aoseg_idx = index_open(pg_aoseg_rel->rd_rel->relaosegidxid, RowExclusiveLock);
		
	values[Anum_pg_aoseg_segno - 1] = ObjectIdGetDatum(segno);
	values[Anum_pg_aoseg_eof - 1] = Float8GetDatum(eof);
	values[Anum_pg_aoseg_tupcount - 1] = Float8GetDatum(tupcount);
	values[Anum_pg_aoseg_varblockcount - 1] = Float8GetDatum(varblockcount);
	
	/*
	 * form the tuple and insert it
	 */
	pg_aoseg_tuple = heap_formtuple(pg_aoseg_dsc, values, nulls);
	if (!HeapTupleIsValid(pg_aoseg_tuple))
		elog(ERROR, "failed to build AO file segment tuple");

	simple_heap_insert(pg_aoseg_rel, pg_aoseg_tuple);
	CatalogUpdateIndexes(pg_aoseg_rel, pg_aoseg_tuple);
	
	heap_freetuple(pg_aoseg_tuple);
	
	index_close(pg_aoseg_idx, RowExclusiveLock);
	heap_close(pg_aoseg_rel, RowExclusiveLock);
}

/*
 * GetFileSegInfo
 *
 * Get the catalog entry for an appendonly relation from the
 * pg_aoseg_* relation that belongs to the currently used 
 * AppendOnly table.
 *
 */
FileSegInfo *
GetFileSegInfo(Relation parentrel, int segno)
{
	
	Relation	pg_aoseg_rel;
	Relation	pg_aoseg_idx;
	TupleDesc	pg_aoseg_dsc;
	HeapTuple	tuple;
	ScanKeyData	key;
	IndexScanDesc	aoscan;
	Datum		eof, tupcount, varbcount;
	bool		isNull;
	FileSegInfo *fsinfo;
		
	/*
	 * Check the pg_aoseg relation to be certain the ao table 
	 * is there. 
	 */
	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, AccessShareLock);
	pg_aoseg_dsc = pg_aoseg_rel->rd_att;
	pg_aoseg_idx = index_open(pg_aoseg_rel->rd_rel->relaosegidxid, AccessShareLock);
	
	/* 
	 * Setup a scan key to fetch from the index by segno.
	 */
	ScanKeyInit(&key,
				(AttrNumber) 1,
				BTEqualStrategyNumber, 
				F_OIDEQ,
				ObjectIdGetDatum(segno));
	
	aoscan = index_beginscan(pg_aoseg_rel, pg_aoseg_idx,
							 GetTransactionSnapshot(), 1, &key);
	
    tuple = index_getnext(aoscan, ForwardScanDirection);
	
	if (!HeapTupleIsValid(tuple))
	{
		/* 
		 * TEMP HACK: we should really show an error here (the one commented out 
		 * below) - however, until we have the fileseg manager process that
		 * knows which fileseg entries exists for a given relation we just
		 * return NULL and let the caller deal with it gracefully.
		 */
		index_endscan(aoscan);
		index_close(pg_aoseg_idx, AccessShareLock);
		heap_close(pg_aoseg_rel, AccessShareLock);
		return NULL;
				
		/* ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("append-only table \"%s\" file segment \"%d\" entry " 
						"does not exist.", RelationGetRelationName(parentrel),
						segno)));*/
	}	
	
	fsinfo = (FileSegInfo *) palloc(sizeof(FileSegInfo));
	
	/* get the eof */
	eof = fastgetattr(tuple, Anum_pg_aoseg_eof, pg_aoseg_dsc, &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid eof value: NULL")));	
	
	/* get the tupcount */
	tupcount = fastgetattr(tuple, Anum_pg_aoseg_tupcount, pg_aoseg_dsc, &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid tupcount value: NULL")));	
	
	/* get the varblock count */
	varbcount = fastgetattr(tuple, Anum_pg_aoseg_varblockcount, pg_aoseg_dsc, &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid varblockcount value: NULL")));	
	
	fsinfo->segno = segno;
	fsinfo->eof = DatumGetFloat8(eof);
	fsinfo->tupcount = DatumGetFloat8(tupcount);
	fsinfo->varblockcount = DatumGetFloat8(varbcount);
	
	if (fsinfo->eof < 0)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Invalid eof " INT64_FORMAT " for relation %s", 
						(int64)fsinfo->eof, NameStr(parentrel->rd_rel->relname))));
	
	/* Finish up scan and close appendonly catalog. */
	index_endscan(aoscan);
	index_close(pg_aoseg_idx, AccessShareLock);
	heap_close(pg_aoseg_rel, AccessShareLock);
	
	return fsinfo;
}

/*
 * Update the eof and filetupcount of an append only table.
 */
void
UpdateFileSegInfo(Relation parentrel,
				  int segno,
				  int64 eof, 
				  float8 tuples_added, 
				  float8 varblocks_added)
{
	Relation			pg_aoseg_rel;
	Relation			pg_aoseg_idx;
	TupleDesc			pg_aoseg_dsc;
	ScanKeyData			key;
	IndexScanDesc		aoscan;
	HeapTuple			tuple, new_tuple;
	Datum				filetupcount;
	Datum				filevarblockcount;
	Datum				new_tuple_count;
	Datum				new_varblock_count;
	Datum				new_record[Natts_pg_aoseg];
	char				new_record_nulls[Natts_pg_aoseg];
	char				new_record_repl[Natts_pg_aoseg];
	bool				isNull;
	
	/* overflow sanity checks */
	Assert(tuples_added >= 0);
	Assert(varblocks_added >= 0);
	Assert(eof >= 0);
	
	elog(DEBUG3, "UpdateFileSegInfo called. segno = %d", segno);
	
	/*
	 * Open the aoseg relation and its index.  
	 */	
	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, RowExclusiveLock);
	pg_aoseg_dsc = pg_aoseg_rel->rd_att;
	pg_aoseg_idx = index_open(pg_aoseg_rel->rd_rel->relaosegidxid, RowExclusiveLock);
	
	/* 
	 * Setup a scan key to fetch from the index by segno.
	 */
	ScanKeyInit(&key,
				(AttrNumber) 1,
				BTEqualStrategyNumber, 
				F_OIDEQ,
				ObjectIdGetDatum(segno));
	
	aoscan = index_beginscan(pg_aoseg_rel, pg_aoseg_idx,
							 GetTransactionSnapshot(), 1, &key);
	
    tuple = index_getnext(aoscan, ForwardScanDirection);
	
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("append-only table \"%s\" file segment \"%d\" entry " 
						"does not exist", RelationGetRelationName(parentrel),
						segno)));
	
	/* get the current tuple count so we can add to it */
	filetupcount = fastgetattr(tuple, 
								Anum_pg_aoseg_tupcount, 
								pg_aoseg_dsc, 
								&isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid pg_aoseg filetupcount value: NULL")));	
	
	/* calculate the new tuple count */
	new_tuple_count = DirectFunctionCall2(float8pl, 
										  filetupcount, 
										  Float8GetDatum(tuples_added));
	
	/* get the current varblock count so we can add to it */
	filevarblockcount = fastgetattr(tuple, 
									 Anum_pg_aoseg_varblockcount, 
									 pg_aoseg_dsc, 
									 &isNull);
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid pg_aoseg varblockcount value: NULL")));	
	
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
	
	new_record[Anum_pg_aoseg_eof - 1] = Float8GetDatum((float8)eof);
	new_record_repl[Anum_pg_aoseg_eof - 1] = 'r';
	
	new_record[Anum_pg_aoseg_tupcount - 1] = new_tuple_count;
	new_record_repl[Anum_pg_aoseg_tupcount - 1] = 'r';
	
	new_record[Anum_pg_aoseg_varblockcount - 1] = new_varblock_count;
	new_record_repl[Anum_pg_aoseg_varblockcount - 1] = 'r';
	
	/*
	 * update the tuple in the pg_aoseg table
	 */
	new_tuple = heap_modifytuple(tuple, pg_aoseg_dsc, new_record, 
								 new_record_nulls, new_record_repl);
	
	simple_heap_update(pg_aoseg_rel, &tuple->t_self, new_tuple);
	CatalogUpdateIndexes(pg_aoseg_rel, new_tuple);
	
	heap_freetuple(new_tuple);
	
	/* Finish up scan */
	index_endscan(aoscan);	
	index_close(pg_aoseg_idx, RowExclusiveLock);
	heap_close(pg_aoseg_rel, RowExclusiveLock);
	
}

/*
 * TEMPORARY FUNCTION: until we have a process that manages the 'segment'
 * concurrency we just call this guy to find the total number of segnos on
 * this local segdb.
 */
int GetNumOfSegFiles(Relation parentrel)
{
	
	Relation		pg_aoseg_rel;
	HeapTuple		tuple;
	HeapScanDesc	aoscan;
	int32			numsegs = 0;
	
	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, AccessShareLock);
	aoscan = heap_beginscan(pg_aoseg_rel, SnapshotNow, 0, NULL);
				
	while ((tuple = heap_getnext(aoscan, ForwardScanDirection)) != NULL)
	{
		numsegs++; /* as long as numbers are consecutive we're ok */
		CHECK_FOR_INTERRUPTS();
	}
	
	heap_endscan(aoscan);			
	heap_close(pg_aoseg_rel, AccessShareLock);
	
	return numsegs;
}

/*
 * GetSegFilesTotals
 *
 * Get the total bytes, tuples, and varblocks for a specific AO table
 * from the pg_aoseg table on this local segdb.
 */
FileSegTotals *GetSegFilesTotals(Relation parentrel)
{
	
	Relation		pg_aoseg_rel;
	TupleDesc		pg_aoseg_dsc;
	HeapTuple		tuple;
	HeapScanDesc	aoscan;
	FileSegTotals  *result;
	Datum			eof,
					tupcount,
					varblockcount;
	bool			isNull;
	
	result = (FileSegTotals *) palloc0(sizeof(FileSegTotals));

	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, AccessShareLock);
	pg_aoseg_dsc = pg_aoseg_rel->rd_att;
	
	aoscan = heap_beginscan(pg_aoseg_rel, SnapshotNow, 0, NULL);
				
	while ((tuple = heap_getnext(aoscan, ForwardScanDirection)) != NULL)
	{
		eof = fastgetattr(tuple, Anum_pg_aoseg_eof, pg_aoseg_dsc, &isNull);
		tupcount = fastgetattr(tuple, Anum_pg_aoseg_tupcount, pg_aoseg_dsc, &isNull);
		varblockcount = fastgetattr(tuple, Anum_pg_aoseg_varblockcount, pg_aoseg_dsc, &isNull);
		
		result->totalbytes += DatumGetFloat8(eof);
		result->totaltuples += DatumGetFloat8(tupcount);
		result->totalvarblocks += DatumGetFloat8(varblockcount);
		result->totalfilesegs++;
		
		CHECK_FOR_INTERRUPTS();
	}
	
	heap_endscan(aoscan);			
	heap_close(pg_aoseg_rel, AccessShareLock);
	
	return result;
}

