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
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"



/* ------------------------------------------------------------------------
 *
 * FUNCTIONS FOR MANIPULATING AND QUERYING AO SEGMENT FILE CATALOG TABLES
 *
 * ------------------------------------------------------------------------
 */

/*
 * InsertFileSegInfo
 *
 * Adds an entry into the pg_aoseg_*  table for this Append 
 * Only relation. Use use frozen_heap_insert so the tuple is
 * frozen on insert.
 */
void
InsertInitialSegnoEntry(Relation parentrel, 
						int segno)
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
	values[Anum_pg_aoseg_eof - 1] = Float8GetDatum(0);
	values[Anum_pg_aoseg_tupcount - 1] = Float8GetDatum(0);
	values[Anum_pg_aoseg_varblockcount - 1] = Float8GetDatum(0);
	
	/*
	 * form the tuple and insert it
	 */
	pg_aoseg_tuple = heap_formtuple(pg_aoseg_dsc, values, nulls);
	if (!HeapTupleIsValid(pg_aoseg_tuple))
		elog(ERROR, "failed to build AO file segment tuple");

	frozen_heap_insert(pg_aoseg_rel, pg_aoseg_tuple);
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
 * if a caller intends to update this file segment entry they must
 * pass true for 'forupdate'. Otherwise, pass false for read only. When
 * we intend to update an entry we lock it for the duration of the operation.
 */
FileSegInfo *
GetFileSegInfo(Relation parentrel, int segno, bool forupdate)
{
	
	Relation		pg_aoseg_rel;
	Relation		pg_aoseg_idx;
	TupleDesc		pg_aoseg_dsc;
	HeapTuple		tuple;
	HeapTupleData 	tupdata;
	ScanKeyData		key;
	IndexScanDesc	aoscan;
	Datum			eof, tupcount, varbcount;
	bool			isNull;
	FileSegInfo 	*fsinfo;
	LOCKMODE		lockmode = (forupdate ? RowExclusiveLock : AccessShareLock);
	
	Buffer			buffer;
	ItemPointerData update_ctid;
	TransactionId 	update_xmax;
	HTSU_Result 	test;

	if(forupdate)
		LWLockAcquire(AppendOnlyLock, LW_EXCLUSIVE);
	
	/*
	 * Check the pg_aoseg relation to be certain the ao table 
	 * is there. 
	 */
	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, lockmode);
	pg_aoseg_dsc = pg_aoseg_rel->rd_att;
	pg_aoseg_idx = index_open(pg_aoseg_rel->rd_rel->relaosegidxid, lockmode);
	
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
		/* aoseg tuple for this segno doesn't exist */
		
		index_endscan(aoscan);
		index_close(pg_aoseg_idx, lockmode);
		heap_close(pg_aoseg_rel, lockmode);
	
		if(forupdate)
		{
		    /*
		     * This operation intends to update the aoseg record for 'segno'.
		     * But no entry exists yet for 'segno' in the aoseg table. This 
		     * is unacceptable since we want to lock this aoseg entry tuple 
		     * until the end of operation (see lock note below). Therefore we 
		     * must create an empty entry. We insert the blank entry and 
		     * freeze it and recursively call ourselves. The next time around 
		     * the aoseg entry will exist.
		     */
			if(Gp_role == GP_ROLE_EXECUTE && !Gp_is_writer)
				elog(ERROR,"Internal Error: AO INSERT must be executed by a "
						   "writer gang");

			InsertInitialSegnoEntry(parentrel, segno);
			LWLockRelease(AppendOnlyLock);
			return GetFileSegInfo(parentrel, segno, true); /* recurse */			
		}
		else
		{
			return NULL;	
		}
	}	
	
	if(forupdate)
		LWLockRelease(AppendOnlyLock);
	
	Assert(HeapTupleIsValid(tuple));
	
	/*
	 * Lock aoseg tuple for update
	 * 
	 * The update lock would protect us from 2 situations: 
	 * 
	 * 1) concurrent utility mode writers. Since UTILITY mode always uses the
	 * same reseved segno and doesn't go through the QD AO concurrency logic, 
	 * we must not allow concurrency in utility mode. serialize the updates.
	 * 
	 * 2) avoid the situation where a distributed transaction that aborted in 
	 * the master and is being ignored by a segment because it is too busy still 
	 * writing some segment file for that aborted transaction.  
	 * 
	 * Both are obviously very bad and must be prevented or will mix data in 
	 * one segment file and be corrupted.
	 * 
	 * NOTE: this piece of code is largely borrowed from ExecutePlan() or
	 * ExecuteUpdate() and is basically identical to SELECT FOR UPDATE.
	 */
	if(forupdate)
	{
lmark:	;
		tupdata = *tuple;
		Assert(ItemPointerIsValid(&tupdata.t_self));
		
		test = heap_lock_tuple(pg_aoseg_rel, &tupdata, &buffer,
							   &update_ctid, &update_xmax,
							   GetCurrentCommandId() /* TODO: is that right? */, 
							   LockTupleExclusive, 
							   false);
		ReleaseBuffer(buffer);
		switch (test)
		{
			case HeapTupleSelfUpdated:
				/* treat it as deleted; do not process */
				Assert(false); /* can this happen? */ 
				break;
				
			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				if (IsXactIsoLevelSerializable)
					ereport(ERROR,
					 (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					  errmsg("could not serialize access due to concurrent update")));
				
				if (!ItemPointerEquals(&update_ctid, &tupdata.t_self))
				{
					/* updated, so look at updated version */
					
					HeapTuple updatedtup;
					
					updatedtup = GetUpdatedTuple_Int(pg_aoseg_rel,
													 &update_ctid,
													 update_xmax,
													 GetCurrentCommandId());

					if (updatedtup)
					{
						tuple = updatedtup;
						goto lmark;
					}
				}
				else
				{
					/* no aoseg tuple should ever get deleted */
					Insist(false);
				}

			default:
				elog(ERROR, "unrecognized heap_lock_tuple status: %u", test);
				return NULL;
		}
		
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
	index_close(pg_aoseg_idx, lockmode);
	heap_close(pg_aoseg_rel, lockmode);
	
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
FileSegInfo *GetAllFileSegInfo(Relation parentrel, int *totalsegs)
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
			
	pg_aoseg_rel = heap_open(parentrel->rd_rel->relaosegrelid, AccessShareLock);
	pg_aoseg_dsc = pg_aoseg_rel->rd_att;
	
	/*
	 * Get number of segment files for this relation
	 */
	aoscan = heap_beginscan(pg_aoseg_rel, SnapshotNow, 0, NULL);
	
	while ((tuple = heap_getnext(aoscan, ForwardScanDirection)) != NULL)
	{
		(*totalsegs)++;
	}
	
	heap_endscan(aoscan);			
	
	/*
	 * Initialize the segment file information array according to the seg count
	 */
	allseginfo = (FileSegInfo *) palloc0(sizeof(FileSegInfo) * (*totalsegs));
	oneseginfo = allseginfo;
	
	/*
	 * Now get the actual segfile information 
	 * (TODO: should combine both scans to 1. since they are very short it's ok for now).
	 */
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
				 *totalsegs);
		
		oneseginfo++;
		
		CHECK_FOR_INTERRUPTS();
	}
	
	heap_endscan(aoscan);			
	heap_close(pg_aoseg_rel, AccessShareLock);
		
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
	
	/* overflow sanity checks. don't check the same for tuples_added,
	 * it may be coming as a negative diff from gp_update_ao_master_stats */
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
	
	aoscan = heap_beginscan(pg_aoseg_rel, GetTransactionSnapshot(), 0, NULL);
				
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

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

/*
 * gp_update_ao_master_stats
 *
 * This function is mainly created to handle cases that our product allowed
 * loading data into an append only table in utility mode, and as a result
 * the QD gets out of sync as to the number of rows in this table for each
 * segment. An example for this scenario is gp_restore. running this function
 * puts the QD aoseg table back in sync.
 */
PG_FUNCTION_INFO_V1(gp_update_ao_master_stats__);
int
gp_update_ao_master_stats__(PG_FUNCTION_ARGS)
{
	StringInfoData 	sqlstmt;
	RangeVar		*parentrv;	
	Relation		parentrel;
	Relation		aosegrel;
	bool 			connected = false;
	char			aoseg_relname[32];
	char			*namespace = NULL;
	char			*relname = NULL;
	char			relstorage;
    short			nargs = PG_NARGS();
	int 			proc;
    int 			ret;
	float8			total_count = 0;
	
	Assert(Gp_role == GP_ROLE_DISPATCH);
	
	/* this check may be redundant. error is caught beforehand */
	if(nargs != 2)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("gp_update_ao_master_stats called with %d arguments", 
						nargs),
				 errhint("arguments should be (schemaname, tablename)")));
	}
	
	if (!PG_ARGISNULL(0))
		namespace = GET_STR(PG_GETARG_TEXT_P(0));
	
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("gp_update_ao_master_stats called with NULL relname")));
	
	relname = GET_STR(PG_GETARG_TEXT_P(1));
	
	parentrv = makeRangeVar(namespace, relname);
	
	/*
	 * get the relid of the parent (main) relation
	 */
	parentrel = heap_openrv(parentrv, RowExclusiveLock);
	
	relstorage = parentrel->rd_rel->relstorage;
	if(relstorage != RELSTORAGE_AO)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("gp_update_ao_master_stats called with non append "
						"only relation (rel '%s', storage '%c')", 
						relname, relstorage)));
	
	/*
	 * get the name of the aoseg relation
	 */
	aosegrel = heap_open(parentrel->rd_rel->relaosegrelid, AccessShareLock);
	sprintf(aoseg_relname, "%s", RelationGetRelationName(aosegrel));
	heap_close(aosegrel, AccessShareLock);
	
	/*
	 * assemble our query string
	 */
	initStringInfo(&sqlstmt);
	appendStringInfo(&sqlstmt, "select segno,sum(tupcount) " 
					 "from gp_dist_random('pg_aoseg.%s') "
					 "group by (segno)", aoseg_relname);
	
	
	PG_TRY();
	{
		
		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Unable to obtain AO relation information from segment databases."),
							errdetail("SPI_connect failed in gp_update_ao_master_stats")));
		}
		connected = true;
		
		/* Do the query. */
		ret = SPI_execute(sqlstmt.data, false, 0);
		proc = SPI_processed;
		
		
	    if (ret > 0 && SPI_tuptable != NULL)
	    {
	        TupleDesc tupdesc = SPI_tuptable->tupdesc;
	        SPITupleTable *tuptable = SPI_tuptable;
	        int i;
	        
		    /*
		     * Iterate through each result tuple
		     */
	        for (i = 0; i < proc; i++)
	        {
	            HeapTuple 	tuple = tuptable->vals[i];
	            FileSegInfo *fsinfo = NULL;
	            int			qe_segno;
	            float8 		qe_tupcount;
	            char 		*val_segno;
	            char 		*val_tupcount;
	            
	            /*
	             * Get totals from QE's for a specific segment
	             */
	            val_segno = SPI_getvalue(tuple, tupdesc, 1);
	            val_tupcount = SPI_getvalue(tuple, tupdesc, 2);
	            
	            /*
	             * Convert to desired data type
	             */
	            qe_segno = pg_atoi(val_segno, sizeof(int32), 0);
	            qe_tupcount = DatumGetFloat8(DirectFunctionCall1(float8in, 
															     CStringGetDatum(val_tupcount)));
				
	            total_count += qe_tupcount;
	            
	            /*
	             * Get the numbers on the QD for this segment
	             */
	            fsinfo = GetFileSegInfo(parentrel, qe_segno, true);
	            
	            /* since we passed 'true' a segfile record must exist now */
	        	Assert(fsinfo);
	        	
	        	/* 
	        	 * check if numbers match.
	        	 * NOTE: proper way is to use float8eq() but since we 
	        	 * don't expect any NAN's in here better do it directly 
	        	 */
				if(fsinfo->tupcount != qe_tupcount)
				{
					float8 	diff = qe_tupcount - fsinfo->tupcount;
					
					elog(DEBUG3, "gp_update_ao_master_stats: updating " 
						 "segno %d with tupcount %d", qe_segno, 
						 (int)qe_tupcount);
					
					/* 
					 * QD tup count !=  QE tup count. update QD count by 
					 * passing in the diff (may be negative sometimes).
					 */
					UpdateFileSegInfo(parentrel, qe_segno, 0, diff, 0);
				}
				else
					elog(DEBUG3, "gp_update_ao_master_stats: no need to " 
						 "update segno %d. it is synced", qe_segno);
				
				pfree(fsinfo);
	        	
	        	/*
	        	 * TODO: if an entry exists for this rel in the AO hash table
	        	 * need to also update that entry in shared memory. Need to
	        	 * figure out how to do this safely when concurrent operations
	        	 * are in progress. note that if no entry exists we are ok.
	        	 * 
	        	 * At this point this doesn't seem too urgent as we generally
	        	 * only expect this function to update segno 0 only and the QD
	        	 * never cares about segment 0 anyway.
	        	 */
	        }
	    }
		
		connected = false;
		SPI_finish();
	}
	
	/* Clean up in case of error. */
	PG_CATCH();
	{
		if (connected)
			SPI_finish();
		
		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();
	
	
	pfree(sqlstmt.data);
	heap_close(parentrel, RowExclusiveLock);
	
	return total_count;
}

