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
#include "catalog/gp_fastsequence.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
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
					  bool checksum,
                      bool columnstore,
					  char* compresstype,
					  Oid segrelid,
					  Oid segidxid,
					  Oid blkdirrelid,
					  Oid blkdiridxid)
{
	Relation	pg_appendonly_rel;
	HeapTuple	pg_appendonly_tuple = NULL;
	bool	   *nulls;
	Datum	   *values;
	int			natts = 0;
	
	/* these are hardcoded for now. in the future should get passed in */
	const int	majorversion = 1;
	const int	minorversion = 0;
	
    /*
     * Open and lock the pg_appendonly catalog.
     */
	pg_appendonly_rel = heap_open(AppendOnlyRelationId, RowExclusiveLock);
	natts = RelationGetDescr(pg_appendonly_rel)->natts;
	values = palloc0(sizeof(Datum) * natts);
	nulls = palloc0(sizeof(bool) * natts);
	
	values[Anum_pg_appendonly_relid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_appendonly_blocksize - 1] = Int32GetDatum(blocksize);
	values[Anum_pg_appendonly_safefswritesize - 1] = Int32GetDatum(safefswritesize);
	values[Anum_pg_appendonly_compresslevel - 1] = Int32GetDatum(compresslevel);
	values[Anum_pg_appendonly_majorversion - 1] = Int32GetDatum(majorversion);
	values[Anum_pg_appendonly_minorversion - 1] = Int32GetDatum(minorversion);
	values[Anum_pg_appendonly_checksum - 1] = BoolGetDatum(checksum);
	values[Anum_pg_appendonly_columnstore - 1] = BoolGetDatum(columnstore);
	values[Anum_pg_appendonly_segrelid - 1] = ObjectIdGetDatum(segrelid);
	values[Anum_pg_appendonly_segidxid - 1] = ObjectIdGetDatum(segidxid);
	values[Anum_pg_appendonly_blkdirrelid - 1] = ObjectIdGetDatum(blkdirrelid);
	values[Anum_pg_appendonly_blkdiridxid - 1] = ObjectIdGetDatum(blkdiridxid);
	
	if(compresstype)
	{
		/*
		 * check compresstype string length.
		 * 
		 * when compresstype column was added to pg_appendonly (release 3.3) we
		 * skipped the creation of a toast table even though it's normally
		 * required for tables with text columns. In this case it's not
		 * necessary because compresstype is always very short (or NULL) and we
		 * never expect it to be toasted. so, for sanity, make sure that it's 
		 * indeed very short. otherwise, it's an internal error.
		 */
		Insist(strlen(compresstype) < 100);
		
		values[Anum_pg_appendonly_compresstype - 1] = DirectFunctionCall1(textin,
																		  CStringGetDatum(compresstype));
					
	}
	else
	{
		Assert(compresslevel == 0);
		nulls[Anum_pg_appendonly_compresstype - 1] = true;
	}
	
	
	/*
	 * form the tuple and insert it
	 */
	pg_appendonly_tuple = heaptuple_form_to(RelationGetDescr(pg_appendonly_rel), 
										 values, nulls, NULL, NULL);
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
 * Get the OIDs of the auxiliary relations and their indexes for an appendonly
 * relation.
 *
 * The OIDs will be retrieved only when the corresponding output variable is
 * not NULL.
 */
void
GetAppendOnlyEntryAuxOids(Oid relid,
						  Oid *segrelid,
						  Oid *segidxid,
						  Oid *blkdirrelid,
						  Oid *blkdiridxid)
{
	Relation	pg_appendonly_rel;
	TupleDesc	pg_appendonly_dsc;
	HeapTuple	tuple;
	ScanKeyData	key;
	SysScanDesc	aoscan;
	Datum auxOid;
	bool isNull;
	
	/*
	 * Check the pg_appendonly relation to be certain the ao table 
	 * is there. 
	 */
	pg_appendonly_rel = heap_open(AppendOnlyRelationId, AccessShareLock);
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
				 errmsg("missing pg_appendonly entry for relation \"%s\"",
						get_rel_name(relid))));
	
	if (segrelid != NULL)
	{
		auxOid = heap_getattr(tuple, 
							  Anum_pg_appendonly_segrelid,
							  pg_appendonly_rel->rd_att, 
							  &isNull);
		Assert(!isNull);
		if(isNull)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("got invalid segrelid value: NULL")));	
		
		*segrelid = DatumGetObjectId(auxOid);
	}
	
	if (segidxid != NULL)
	{
		auxOid = heap_getattr(tuple, 
							  Anum_pg_appendonly_segidxid,
							  pg_appendonly_rel->rd_att, 
							  &isNull);
		Assert(!isNull);
		if(isNull)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("got invalid segidxid value: NULL")));	
		
		*segidxid = DatumGetObjectId(auxOid);
	}
	
	if (blkdirrelid != NULL)
	{
		auxOid = heap_getattr(tuple, 
							  Anum_pg_appendonly_blkdirrelid,
							  pg_appendonly_rel->rd_att, 
							  &isNull);
		Assert(!isNull);
		if(isNull)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("got invalid blkdirrelid value: NULL")));	
		
		*blkdirrelid = DatumGetObjectId(auxOid);
	}

	if (blkdiridxid != NULL)
	{
		auxOid = heap_getattr(tuple, 
							  Anum_pg_appendonly_blkdiridxid,
							  pg_appendonly_rel->rd_att, 
							  &isNull);
		Assert(!isNull);
		if(isNull)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("got invalid blkdiridxid value: NULL")));	
		
		*blkdiridxid = DatumGetObjectId(auxOid);
	}

	/* Finish up scan and close pg_appendonly catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly_rel, AccessShareLock);
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
				compresstype,
				majorversion,
				minorversion,
                checksum,
				columnstore,
				segrelid,
				segidxid,
				blkdirrelid,
				blkdiridxid
				;

	bool		isNull;
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
				 errmsg("missing pg_appendonly entry for relation \"%s\"",
						get_rel_name(relid))));
	
	
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
	
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid checksum value: NULL")));	

	columnstore = heap_getattr(tuple, 
							Anum_pg_appendonly_columnstore, 
							pg_appendonly_rel->rd_att, 
							&isNull);
    Assert(!isNull);
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid columnstore value: NULL")));	

	/* get the compressor type */
	compresstype = heap_getattr(tuple, 
								Anum_pg_appendonly_compresstype, 
								pg_appendonly_rel->rd_att, 
								&isNull);
	
	if(isNull)
	{
		Assert(DatumGetInt16(compresslevel) == 0);
		aoentry->compresstype = NULL;
	}
	else
		aoentry->compresstype = DatumGetCString(DirectFunctionCall1(textout, compresstype));
	
    segrelid = heap_getattr(tuple, 
							Anum_pg_appendonly_segrelid,
							pg_appendonly_rel->rd_att, 
							&isNull);
    Assert(!isNull);
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid segrelid value: NULL")));	

    segidxid = heap_getattr(tuple, 
							Anum_pg_appendonly_segidxid,
							pg_appendonly_rel->rd_att, 
							&isNull);
    Assert(!isNull);
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid segidxid value: NULL")));	

    blkdirrelid = heap_getattr(tuple, 
							   Anum_pg_appendonly_blkdirrelid,
							   pg_appendonly_rel->rd_att, 
							   &isNull);
    Assert(!isNull);
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid blkdirrelid value: NULL")));	

    blkdiridxid = heap_getattr(tuple, 
							   Anum_pg_appendonly_blkdiridxid,
							   pg_appendonly_rel->rd_att, 
							   &isNull);
    Assert(!isNull);
	if(isNull)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("got invalid blkdiridxid value: NULL")));	

	aoentry->blocksize = DatumGetInt32(blocksize);
	aoentry->safefswritesize = DatumGetInt32(safefswritesize);
	aoentry->compresslevel = DatumGetInt16(compresslevel);
	aoentry->majorversion = DatumGetInt16(majorversion);
	aoentry->minorversion = DatumGetInt16(minorversion);
	aoentry->checksum = DatumGetBool(checksum);
    aoentry->columnstore = DatumGetBool(columnstore);
    aoentry->segrelid = DatumGetObjectId(segrelid);
    aoentry->segidxid = DatumGetObjectId(segidxid);
    aoentry->blkdirrelid = DatumGetObjectId(blkdirrelid);
    aoentry->blkdiridxid = DatumGetObjectId(blkdiridxid);
	
	/* Finish up scan and close pg_appendonly catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly_rel, RowExclusiveLock);
	
	return aoentry;
}

/*
 * Update the segrelid and/or blkdirrelid if the input new values
 * are valid OIDs.
 */
void
UpdateAppendOnlyEntryAuxOids(Oid relid,
							 Oid newSegrelid,
							 Oid newSegidxid,
							 Oid newBlkdirrelid,
							 Oid newBlkdiridxid)
{
	Relation	pg_appendonly_rel;
	TupleDesc	pg_appendonly_dsc;
	HeapTuple	tuple, newTuple;
	ScanKeyData	key;
	SysScanDesc	aoscan;
	Datum *newValues;
	bool *newNulls;
	bool *replace;
	
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
				 errmsg("missing pg_appendonly entry for relation \"%s\"",
						get_rel_name(relid))));

	newValues = palloc0(pg_appendonly_dsc->natts * sizeof(Datum));
	newNulls = palloc0(pg_appendonly_dsc->natts * sizeof(bool));
	replace = palloc0(pg_appendonly_dsc->natts * sizeof(bool));
	
	if (OidIsValid(newSegrelid))
	{
		replace[Anum_pg_appendonly_segrelid - 1] = true;
		newValues[Anum_pg_appendonly_segrelid - 1] = newSegrelid;
	}
	
	if (OidIsValid(newSegidxid))
	{
		replace[Anum_pg_appendonly_segidxid - 1] = true;
		newValues[Anum_pg_appendonly_segidxid - 1] = newSegidxid;
	}
	
	if (OidIsValid(newBlkdirrelid))
	{
		replace[Anum_pg_appendonly_blkdirrelid - 1] = true;
		newValues[Anum_pg_appendonly_blkdirrelid - 1] = newBlkdirrelid;
	}
	
	if (OidIsValid(newBlkdiridxid))
	{
		replace[Anum_pg_appendonly_blkdiridxid - 1] = true;
		newValues[Anum_pg_appendonly_blkdiridxid - 1] = newBlkdiridxid;
	}
	
	newTuple = heap_modify_tuple(tuple, pg_appendonly_dsc,
								 newValues, newNulls, replace);
	simple_heap_update(pg_appendonly_rel, &tuple->t_self, newTuple);
	CatalogUpdateIndexes(pg_appendonly_rel, newTuple);

	heap_freetuple(newTuple);
	
	systable_endscan(aoscan);

	heap_close(pg_appendonly_rel, RowExclusiveLock);
}

/*
 * Remove all pg_appendonly entries that the table we are DROPing
 * refers to (using the table's relfilenode)
 *
 * The gp_fastsequence entries associate with the table is also
 * deleted here.
 */
void
RemoveAppendonlyEntry(Oid relid)
{
	Relation	pg_appendonly_rel;
	TupleDesc	pg_appendonly_dsc;
	HeapTuple	tuple;
	ScanKeyData	key;
	SysScanDesc	aoscan;
	Oid aosegrelid = InvalidOid;
	
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
	
	{
		bool isNull;
		Datum datum = fastgetattr(tuple, Anum_pg_appendonly_segrelid,
								  pg_appendonly_dsc, &isNull);
		Assert(!isNull);
		aosegrelid = DatumGetObjectId(datum);
		Assert(OidIsValid(aosegrelid));
	}

	/* Piggyback here to remove gp_fastsequence entries */
	RemoveFastSequenceEntry(aosegrelid);

	/*
	 * Delete the appendonly table entry from the catalog (pg_appendonly).
	 */
	simple_heap_delete(pg_appendonly_rel, &tuple->t_self);
	
	
	/* Finish up scan and close appendonly catalog. */
	systable_endscan(aoscan);
	
	heap_close(pg_appendonly_rel, NoLock);
}


