/*-------------------------------------------------------------------------
 *
 * cdbutil.c
 *	  Internal utility support functions for CDB/PostgreSQL.
 *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *	- According to src/backend/executor/nodeSeqScan.c 
 *		"tuples returned by heap_getnext() are pointers onto disk
 *		pages and were not created with palloc() and so should not 
 *		be pfree()'d"
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbutil.h"

/*
 * Helper Functions
 */
static int SegmentDatabaseInfoCompare(const void *p1, const void *p2);

/*
 * getCDbSegmentDatabases
 *
 * Obtains a structure representing the following JOIN:
 *
 *	 SELECT 
 *			 d.segindex, 
 * 			 s.dbname, 
 * 			 d.isprimary, 
 * 			 h.hostname,
 * 			 l.port, 
 * 			 l.datadir, 
 * 			 d.isvalid
 * 			 c.cdbsuperuser,
 * 			 c.cdbsuperpswd
 *		 FROM
 *			 cdb.host h,
 *			 cdb.listener l,
 *			 cdb.segment s,
 * 			 cdb.segdb d
 * 			 cdb.cdb c
 *		 WHERE
 *			 h.hostid = l.hostid
 *			 AND l.listenerid = d.listenerid
 * 			 AND s.segindex = d.segindex
 *		 ORDER BY 
 *			 s.segindex, isprimary DESC
 *
 * Storage for the SegmentInstances block and all subsidiary
 * strucures are allocated from the caller's context.
 */
SegmentDatabases *getCdbSegmentDatabases(void)
{
	SegmentDatabases	*segment_databases = NULL;
	/*
	 * Relations for each of the 5 tables we need to read
	 */
	Relation			cdb_segdb_rel;
	Relation			cdb_segment_rel;
	Relation			cdb_listener_rel;
	Relation			cdb_host_rel;
	Relation			cdb_cdb_rel;
	/*
	 * HeapTuples for each of the 5 tables we need to read
	 */
	HeapTuple			cdb_segdb_tuple = NULL;
	HeapTuple			cdb_segment_tuple = NULL;
	HeapTuple			cdb_listener_tuple = NULL;
	HeapTuple			cdb_host_tuple = NULL;
	HeapTuple			cdb_cdb_tuple = NULL;
	
	/*
	 * ScanDescs for each of the 5 tables we need to read
	 */
	HeapScanDesc		cdb_segdb_scan;
	SysScanDesc			cdb_segment_scan;
	SysScanDesc			cdb_listener_scan;
	SysScanDesc			cdb_host_scan;
	HeapScanDesc		cdb_cdb_scan;
	
	/*
	 * Index Oids for each of the 3 indexes we use
	 */
	Oid					cdb_segment_index_oid;
	Oid					cdb_listener_index_oid;
	Oid					cdb_host_index_oid;

	/*
	 * Oid for cdb namespace
	 */
	Oid					cdb_namespace_oid;
	
	/*
	 * Initial size for segment_db_info array.
	 */
	int					array_size = 128;

	/*
	 * ScanKeyDatas for the 3 scans we do.
	 */
	ScanKeyData			segment_key[1];
	ScanKeyData			listener_key[1];
	ScanKeyData			host_key[1];
	
	/*
	 * isNull and attr are used when getting the data for a specific column from a HeapTuple
	 */
	bool				isNull;
	Datum				attr;
	
	char				*cdbsuperuser = NULL;
	char				*cdbsuperpswd = NULL;
	int					cdb_numsegments = 0;

	/*
	 * Allocate segment_databases return structure and 
	 * segment_databases->segment_db_info array with an initial size
	 * of 128.  If necessary during row fetching, we grow this by
	 * doubling each time we run out.
	 */
	segment_databases = palloc0( sizeof(SegmentDatabases) );
	segment_databases->segment_db_info		= (SegmentDatabaseInfo *)palloc0(sizeof(SegmentDatabaseInfo) * array_size);
	segment_databases->total_segment_dbs	= 0;
	segment_databases->total_segments		= 0;
	
	/*
	 * Get oid of cdb namespace
	 */
	cdb_namespace_oid = getCDBNamespaceOid();
	if ( cdb_namespace_oid == InvalidOid )
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Cannot retrieve oid of %s namespace", 
				 		CdbNamespaceName )));
	}
		
	/*
	 * The strategy is to scan through cdb.segdb, and, for each row, 
	 * do lookups into other tables.
	 * We do this by index or heap scans with scan keys on the other tables.
	 */

	/*
	 * First, open all the table relations.
	 * This will ereport and ERROR is unsuccessful in opening the relation.
	 */
	cdb_segdb_rel = HeapOpenCDBObject( CdbSegDBRelationName, cdb_namespace_oid, true );
	cdb_segment_rel = HeapOpenCDBObject( CdbSegmentRelationName, cdb_namespace_oid, true );
	cdb_listener_rel = HeapOpenCDBObject( CdbListenerRelationName, cdb_namespace_oid, true );
	cdb_host_rel = HeapOpenCDBObject( CdbHostRelationName, cdb_namespace_oid, true );
	cdb_cdb_rel = HeapOpenCDBObject( CdbCdbRelationName, cdb_namespace_oid, true );
	
	/*
	 * Convert index names to Oids
	 */
	cdb_segment_index_oid = get_relname_relid(CdbSegmentPrimaryKeyIndexName, 
												cdb_namespace_oid);
	cdb_listener_index_oid = get_relname_relid(CdbListenerPrimaryKeyIndexName, 
												cdb_namespace_oid);
	cdb_host_index_oid = get_relname_relid(CdbHostPrimaryKeyIndexName, 
												cdb_namespace_oid);
	
	/*
	 * Read the one row table cdb.cdb.
	 * We currently require that this be there or else we ereport an ERROR.
	 */
	cdb_cdb_scan = heap_beginscan(cdb_cdb_rel, SnapshotNow, 0, NULL);
	cdb_cdb_tuple = heap_getnext(cdb_cdb_scan, ForwardScanDirection);
	if ( !HeapTupleIsValid(cdb_cdb_tuple) )
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("No rows found in %s.%s",
				 CdbNamespaceName, CdbCdbRelationName)));
	}
	
	/*
	* Grab the fields that we need from cdb.cdb
	*/

	/*
	* cdbsuperuser
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_cdbsuperuser, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	cdbsuperuser = DatumGetCString(DirectFunctionCall1(textout, attr));
	
	/*
	* cdbsuperpswd
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_cdbsuperpswd, cdb_cdb_rel->rd_att, &isNull);
	if ( !isNull )
		cdbsuperpswd = DatumGetCString(DirectFunctionCall1(textout, attr));
	 
	/*
	* segindex
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_segindex, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	segment_databases->my_segindex = DatumGetInt32(attr);
		
	/*
	* isprimary
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_isprimary, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	segment_databases->my_isprimary = DatumGetBool(attr);
	
	/*
	* numsegments
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_numSegments, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	cdb_numsegments = DatumGetInt32(attr);
		
	heap_endscan(cdb_cdb_scan);
	heap_close(cdb_cdb_rel, AccessShareLock);
	
	/*
	 * Main loop: loop through cdb.segdb by doing a heap scan
	 */
	cdb_segdb_scan = heap_beginscan(cdb_segdb_rel, SnapshotNow, 0, NULL);
	
	while ( HeapTupleIsValid(cdb_segdb_tuple = heap_getnext(cdb_segdb_scan, ForwardScanDirection)) )
	{
		if ( segment_databases->total_segment_dbs >= array_size )
		{
			/*
			 * Expand SegmentDatabaseInfo array if we've used up currently allocated space
			 */
			array_size *= 2;
			SegmentDatabaseInfo *pOld = segment_databases->segment_db_info;
			segment_databases->segment_db_info = (SegmentDatabaseInfo *)
												repalloc(pOld, sizeof(SegmentDatabaseInfo) * array_size);
		}
		
		SegmentDatabaseInfo *pRow;
		pRow = &segment_databases->segment_db_info[segment_databases->total_segment_dbs];
		
		/*
		* Assign field from cdb.cdb that were already read in
		*/
		pRow->cdbsuperuser = pstrdup(cdbsuperuser);
		if ( cdbsuperpswd != NULL )
			pRow->cdbsuperpswd = pstrdup(cdbsuperpswd);
		
			
		/*
		* Grab the fields that we need from cdb.segdb
		*/
		int		segindex;
		int		listenerid;
		int		hostid;

		/*
		* segindex
		*/
		attr = heap_getattr(cdb_segdb_tuple, Anum_cdb_segdb_segindex, cdb_segdb_rel->rd_att, &isNull);
		Assert( !isNull );
		segindex = DatumGetInt32(attr);
		pRow->segindex = segindex;

		/*
		* isprimary
		*/
		attr = heap_getattr(cdb_segdb_tuple, Anum_cdb_segdb_isprimary, cdb_segdb_rel->rd_att, &isNull);
		Assert( !isNull );
		pRow->isprimary = DatumGetBool(attr);

		/*
		* isvalid
		*/
		attr = heap_getattr(cdb_segdb_tuple, Anum_cdb_segdb_isvalid, cdb_segdb_rel->rd_att, &isNull);
		Assert( !isNull );
		pRow->isvalid = DatumGetBool(attr);

		/*
		* listenerid
		*/
		attr = heap_getattr(cdb_segdb_tuple, Anum_cdb_segdb_listenerid, cdb_segdb_rel->rd_att, &isNull);
		Assert( !isNull );
		listenerid = DatumGetInt32(attr);

		/*
		* Now, scan the cdb.segment table for this segindex.
		* This will use the index if it's there and available.
		*/
		ScanKeyInit(&segment_key[0], Anum_cdb_segment_segindex, BTEqualStrategyNumber,
					F_INT4EQ, Int32GetDatum(segindex));
					
		cdb_segment_scan = cdbtable_beginscan(cdb_segment_rel, 
											  cdb_segment_index_oid, 
											   (cdb_segment_index_oid != InvalidOid), 
												SnapshotNow, 
											   1, 
											   segment_key);
		
		cdb_segment_tuple = systable_getnext(cdb_segment_scan);
		if ( !HeapTupleIsValid(cdb_segment_tuple) )
		{
			/*
			 * This should never happen because of the foreign key constraint we have on
			 * cdb.segment.segindex
			 */
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("Could not find segindex %d in %s.%s table",
					 segindex, CdbNamespaceName, CdbSegmentRelationName )));
		}
		
		/*
		* Grab the fields that we need from cdb.segment
		*/

		/*
		* dbname
		*/
		attr = heap_getattr(cdb_segment_tuple, Anum_cdb_segment_dbname, cdb_segment_rel->rd_att, &isNull);
		Assert( !isNull );
		pRow->dbname = DatumGetCString(DirectFunctionCall1(textout, attr));

		systable_endscan(cdb_segment_scan);
		
		/*
		* Now, scan the cdb.listener table for this listenerid
		* This will use the index if it's there and available.
		*/
		ScanKeyInit(&listener_key[0], Anum_cdb_listener_listenerid, BTEqualStrategyNumber,
					F_INT4EQ, Int32GetDatum(listenerid));
					
		cdb_listener_scan = cdbtable_beginscan(cdb_listener_rel, 
											  cdb_listener_index_oid, 
											   (cdb_listener_index_oid != InvalidOid), 
												SnapshotNow, 
											   1, 
											   listener_key);
		
		cdb_listener_tuple = systable_getnext(cdb_listener_scan);
		if ( !HeapTupleIsValid(cdb_listener_tuple) )
		{
			/*
			 * This should never happen because of the foreign key constraint we have on
			 * cdb.listener.listenerid
			 */
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("Could not find listenerid %d in %s.%s table",
					 listenerid, CdbNamespaceName, CdbListenerRelationName )));
		}

		/*
		* Grab the fields that we need from cdb.listener
		*/

		/*
		* port
		*/
		attr = heap_getattr(cdb_listener_tuple, Anum_cdb_listener_port, cdb_listener_rel->rd_att, &isNull);
		Assert( !isNull );
		pRow->port = DatumGetInt32(attr);

		/*
		* datadir
		*/
		attr = heap_getattr(cdb_listener_tuple, Anum_cdb_listener_datadir, cdb_listener_rel->rd_att, &isNull);
		Assert( !isNull );
		pRow->datadir = DatumGetCString(DirectFunctionCall1(textout, attr));

		/*
		* hostid
		*/
		attr = heap_getattr(cdb_listener_tuple, Anum_cdb_listener_hostid, cdb_listener_rel->rd_att, &isNull);
		Assert( !isNull );
		hostid = DatumGetInt32(attr);

		systable_endscan(cdb_listener_scan);
		
		/*
		* Now, scan the cdb.host table for this hostid
		* This will use the index if it's there and available.
		*/
		ScanKeyInit(&host_key[0], Anum_cdb_host_hostid, BTEqualStrategyNumber,
					F_INT4EQ, Int32GetDatum(hostid));
					
		cdb_host_scan = cdbtable_beginscan(cdb_host_rel, 
											  cdb_host_index_oid, 
											   (cdb_host_index_oid != InvalidOid), 
												SnapshotNow, 
											   1, 
											   host_key);
		
		cdb_host_tuple = systable_getnext(cdb_host_scan);
		if ( !HeapTupleIsValid(cdb_host_tuple))
		{
			/*
			 * This should never happen because of the foreign key constraint we have on
			 * cdb.host.hostid
			 */
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("Could not find hostid %d in %s.%s table",
					 listenerid, CdbNamespaceName, CdbHostRelationName )));
		}
			
		/*
		* Grab the fields that we need from cdb.host
		*/
		attr = heap_getattr(cdb_host_tuple, Anum_cdb_host_hostname, cdb_host_rel->rd_att, &isNull);
		Assert( !isNull );
		pRow->hostname = DatumGetCString(DirectFunctionCall1(textout, attr));

		systable_endscan(cdb_host_scan);
		
		segment_databases->total_segment_dbs++;
	}
	
	/*
	 * Now sort the data by segindex, isprimary desc
	 */
	 qsort( segment_databases->segment_db_info, 
			segment_databases->total_segment_dbs, sizeof(SegmentDatabaseInfo),
    		  SegmentDatabaseInfoCompare);
    		  
	/*
	 * Now count the number of distinct segindexes.
	 * Since it's sorted, this is easy.
	 */
	 int i;
	 
	 for ( i=0; i<segment_databases->total_segment_dbs; i++ )
	 {
	 	if ( i == 0 || 
		 	(segment_databases->segment_db_info[i].segindex != 
		 	segment_databases->segment_db_info[i-1].segindex ) )
			segment_databases->total_segments++;	 		
	 }
	 
	 /*
	  *  Validate that cdb_numsegments == segment_databases->total_segment_dbs
	  */
	 if ( cdb_numsegments != segment_databases->total_segments )
	 {
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("CDB number of segments inconsistency: count is %d from %s.%s table, but %d from getCdbSegmentDatabases()",
		    cdb_numsegments, CdbNamespaceName, CdbCdbRelationName, segment_databases->total_segments )));
	 }

	/*
	 * Cleanup invoves ending the heap scan of cdb.segdb, and closing all the relations we opened.
	 */
	heap_endscan(cdb_segdb_scan);

	heap_close(cdb_segdb_rel, AccessShareLock);
	heap_close(cdb_segment_rel, AccessShareLock);
	heap_close(cdb_listener_rel, AccessShareLock);
	heap_close(cdb_host_rel, AccessShareLock);
	
	if ( cdbsuperuser != NULL )
		pfree( cdbsuperuser );
		
	if ( cdbsuperpswd != NULL )
		pfree( cdbsuperpswd );

	return segment_databases;
}


/*
 * freeCdbSegmentDatabases
 *
 * Releases the storage occupied by the SegmentDatabases
 * heap pointed to by the argument.
 */
void freeCdbSegmentDatabases(SegmentDatabases *sd)
{
	int i;
	
	if ( sd == NULL )
		return;
		
	if ( sd->segment_db_info != NULL )
	{
		for ( i=0; i<sd->total_segment_dbs; i++ )
		{
			SegmentDatabaseInfo *sdi = &sd->segment_db_info[i]; 
	    	if ( sdi->dbname != NULL )
	    		pfree(sdi->dbname);
	    	if ( sdi->hostname != NULL )
	    		pfree(sdi->hostname);
			if ( sdi->datadir != NULL )
	    		pfree(sdi->datadir);
			if ( sdi->cdbsuperuser != NULL )
	    		pfree(sdi->cdbsuperuser);
			if ( sdi->cdbsuperpswd != NULL )
	    		pfree(sdi->cdbsuperpswd);
		}
		
		pfree( sd->segment_db_info );
	}

	pfree( sd );
}

/*
 * SegmentDatabaseInfoCompare:
 * A compare function for SegmentDatabaseInfo structs
 * that compares based on segindex, isprimary desc
 * for use with qsort.
 */
static int SegmentDatabaseInfoCompare(const void *p1, const void *p2)
{
	const SegmentDatabaseInfo *obj1 = (SegmentDatabaseInfo *)p1;
	const SegmentDatabaseInfo *obj2 = (SegmentDatabaseInfo *)p2;
	
	int cmp = obj1->segindex - obj2->segindex;
	if ( cmp == 0 )
		cmp = (int)obj1->isprimary - (int)obj1->isprimary;
		
	return cmp;	
}
