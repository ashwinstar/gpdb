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
#include "cdb/cdbvars.h"
#include "optimizer/geqo_random.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"

/*
 * Global Variables
 */
static HTAB *AppendOnlyHash;			/* Hash of AO tables */

/*
 * GUC variables
 */
int		MaxAppendOnlyTables;		/* Max # of tables */


/* ------------------------------------------------------------------------
 *
 * FUNCTIONS FOR MANIPULATING AND QUERYING AO SEGMENT FILE CATALOG TABLES
 *
 * ------------------------------------------------------------------------
 */

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
 * GetAllFileSegInfo
 *
 * Get all catalog entries for an appendonly relation from the
 * pg_aoseg_* relation that belongs to the currently used 
 * AppendOnly table. This is basically a physical snapshot that a
 * scanner can use to scan all the data in a local segment database.
 */
FileSegInfo *GetAllFileSegInfo(Relation parentrel, int totalsegs)
{
	
	Relation		pg_aoseg_rel;
	TupleDesc		pg_aoseg_dsc;
	HeapTuple		tuple;
	HeapScanDesc	aoscan;
	FileSegInfo		*allseginfo;
	FileSegInfo		*oneseginfo;	
	Datum			segno,
					eof,
					tupcount,
					varblockcount;
	bool			isNull;
	int				totalcount = 0;
	
	allseginfo = (FileSegInfo *) palloc0(sizeof(FileSegInfo) * totalsegs);
	oneseginfo = allseginfo;
	
	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, AccessShareLock);
	pg_aoseg_dsc = pg_aoseg_rel->rd_att;
	
	aoscan = heap_beginscan(pg_aoseg_rel, SnapshotNow, 0, NULL);
	
	while ((tuple = heap_getnext(aoscan, ForwardScanDirection)) != NULL)
	{	
		segno = fastgetattr(tuple, Anum_pg_aoseg_segno, pg_aoseg_dsc, &isNull);
		eof = fastgetattr(tuple, Anum_pg_aoseg_eof, pg_aoseg_dsc, &isNull);
		tupcount = fastgetattr(tuple, Anum_pg_aoseg_tupcount, pg_aoseg_dsc, &isNull);
		varblockcount = fastgetattr(tuple, Anum_pg_aoseg_varblockcount, pg_aoseg_dsc, &isNull);
		
		oneseginfo->segno = DatumGetInt32(segno);
		oneseginfo->eof += DatumGetFloat8(eof);
		oneseginfo->tupcount += DatumGetFloat8(tupcount);
		oneseginfo->varblockcount += DatumGetFloat8(varblockcount);
		
		if (Debug_appendonly_print_scan)
			elog(LOG,"Append-only found existing segno %d with eof " INT64_FORMAT " for table '%s' out of total %d segs",
				 oneseginfo->segno,
				 (int64)oneseginfo->eof,
			     NameStr(parentrel->rd_rel->relname),
				 totalsegs);
		
		oneseginfo++;
		totalcount++;
		
		CHECK_FOR_INTERRUPTS();
	}
	
	heap_endscan(aoscan);			
	heap_close(pg_aoseg_rel, AccessShareLock);
	
	/* make sure our caller didn't lie */ 
	Assert(totalcount == totalsegs);
	
	return allseginfo;
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

/*
 * UpdateMasterAosegTotals
 *
 * Update the aoseg table on the master node with the updated row count of
 * the whole distributed relation. We use this information later on to keep
 * track of file 'segments' and their EOF's and decide which segno to use in
 * future writes into the table.
 */
void UpdateMasterAosegTotals(Relation parentrel, int segno, uint64 tupcount)
{
	FileSegInfo		*fsinfo;
	float8			tupcount_float8;
	
	Assert(Gp_role == GP_ROLE_DISPATCH);
	Assert(segno >= 0);
	
	/* 
	 * convert the tup count to float8 
	 */
	if((int64) tupcount < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("int64 out of range")));
	
	tupcount_float8 = DatumGetFloat8(DirectFunctionCall1(i8tod, 
														 (int64) tupcount));
		
	/* get the information for the file segment we inserted into */
	fsinfo = GetFileSegInfo(parentrel, segno);
			
	/* 
	 * Update the master AO segment info table with correct tuple count total
	 */
	if(!fsinfo)
	{
		/* insert the first entry into pg_aoseg_* with our new tup count */
		InsertFileSegInfo(parentrel, segno, 0, tupcount_float8, 0);
	}
	else
	{
		/* update the existing pg_aoseg_* with our added tupcount */
		UpdateFileSegInfo(parentrel, segno, 0, tupcount_float8, 0);
		pfree(fsinfo);
	}
	
	
}

/* ------------------------------------------------------------------------
 *
 * FUNCTIONS FOR SUPPORTING AO CONCURRENT WRITES (VIA SHARED MEM HASH TBL)
 *
 * ------------------------------------------------------------------------
 */

/*
 * SetSegnoForWrite
 *
 * Depending on the gp role and existingsegno make a decision on which
 * segment nubmer should be used to write into during the COPY or INSERT
 * operation we're executing.
 *
 * 'existingsegno' may or may not be valid. If we are a QE it must be a valid
 * segment number sent to us from the QD and we must use it. Otherwize, it must
 * be invalid since we haven't yet assigned any.
 */
int SetSegnoForWrite(int existingsegno)
{
	switch(Gp_role)
	{
		/* find a seg number to tell the QE's to use */ 
		case GP_ROLE_DISPATCH:
			Assert(existingsegno == InvalidFileSegNumber);
			return geqo_randint(0, 10); /* TEMP... replace me with func call! */
			
		/* use the seg number the QD sent us */
		case GP_ROLE_EXECUTE:
			Assert(existingsegno != InvalidFileSegNumber);
			return existingsegno;
			
		/* always use seg number 0 in utility mode */
		case GP_ROLE_UTILITY:
			Assert(existingsegno == InvalidFileSegNumber);
			return 0;
			
		/* fix this for dispatch agent. for now it's broken anyway. */
		default:
			Assert(false);
			return InvalidFileSegNumber;
	}
	
}

/*
 * AOHashTableInit
 * 
 * initialize the hash table of AO rels and their fileseg entries.
 */
bool
AOHashTableInit(void)
{
	
	HASHCTL			info;
	int				hash_flags;
	
	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	info.entrysize = sizeof(AORelHashEntry);
	info.hash = tag_hash;
	hash_flags = (HASH_ELEM | HASH_FUNCTION);
	
	elog(DEBUG1, "Creating hash table for %d append only tables", MaxAppendOnlyTables);

	AppendOnlyHash = ShmemInitHash("Append Only Hash",
								   MaxAppendOnlyTables,
								   MaxAppendOnlyTables,
								   &info,
								   hash_flags);
	
	if (!AppendOnlyHash)
		return false;
	
	return true;
}

