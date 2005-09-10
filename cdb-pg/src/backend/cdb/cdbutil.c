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
#include "cdb/cdblink.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbgang.h"
#include "cdb/ml_ipc.h"   /*listener_setup*/
#include "cdb/cdbtm.h"




/*
 * Helper Functions
 */
static int CdbComponentDatabaseInfoCompare(const void *p1, const void *p2);
static void freeCdbComponentDatabaseInfo( CdbComponentDatabaseInfo *cdi );

/*
 * getCdbCdb() causes the cdb.cdb table's singleton row to be read into a global structure
 * This is called from cdblink_setup().
 * The global variable is named CdbIdentity, and is declared in cdbvars.h and defined in cdbvars.c.
 * There is one member variable that is a string.  It is allocated from the TopMemoryContext
 * because this information is intended to last for the life of the connection.
 * This call is made whether the database is an entry database or a segment database.
 */
void getCdbCdb(void)
{
	Relation			cdb_cdb_rel;
	HeapTuple			cdb_cdb_tuple = NULL;
	HeapScanDesc		cdb_cdb_scan;
	Oid					cdb_namespace_oid;
	Oid 				cdb_oid;
	bool				isNull;
	Datum				attr;
	Name cdbname;
	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;
	
	if (Cdb_role == CDB_ROLE_UTILITY)
		elog(DEBUG1, "WARNING: called getCdbCdb when in utility mode");

	if (Cdb_role == CDB_ROLE_DISPATCH && Cdb_segment < -1)
		Cdb_segment = -1;
		
	/*
	 * Release storage for the cdbname if not NULL
	 */
	if ( CdbIdentity.cdbname != NULL )
	{
		pfree( CdbIdentity.cdbname );
		CdbIdentity.cdbname = NULL;
	}

	/*
	 * Get oid of cdb namespace
	 */
	cdb_namespace_oid = getCDBNamespaceOid();
	if ( cdb_namespace_oid == InvalidOid )
	{
		ereport(WARNING,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Cannot retrieve oid of %s namespace", 
				 		CdbNamespaceName )));
		return;
	}
	
	cdb_oid = get_relname_relid(CdbCdbRelationName, cdb_namespace_oid);
	
	/*
	 * If the cdb.cdb table isn't there we continue if not dispatch.
	 */	
	if ( cdb_oid == InvalidOid )
	{
		ereport(WARNING,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Table does not exist %s.%s.",
				 CdbNamespaceName, CdbCdbRelationName)));
		return;
	}
		

	/*
	 * Open the cdb.cdb table relation
	 */
	cdb_cdb_rel = HeapOpenCDBObject( CdbCdbRelationName, cdb_namespace_oid, true );

	/*
	 * Read the one row table cdb.cdb.
	 * We currently require that this be there or else we ereport an ERROR.
	 */
	cdb_cdb_scan = heap_beginscan(cdb_cdb_rel, SnapshotNow, 0, NULL);
	cdb_cdb_tuple = heap_getnext(cdb_cdb_scan, ForwardScanDirection);
	if ( !HeapTupleIsValid(cdb_cdb_tuple) )
	{
		ereport(WARNING,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("No rows found in %s.%s.",
				 CdbNamespaceName, CdbCdbRelationName)));
		return;
	}
	
	/*
	* Grab the fields that we need from cdb.cdb
	*/

	/*
	* cdbname
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_cdbname, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	cdbname = DatumGetName(attr);

	


	/*
	* Switch to the TopMemoryContext for the cdbname, so it lasts for the life
	* of the connection.  It is released in freeCdbCdb, which is called from 
	* cdblink_disconnect().
	*/
    oldContext = MemoryContextSwitchTo(permanentContext);
	CdbIdentity.cdbname = pstrdup( cdbname->data );
    MemoryContextSwitchTo(oldContext);
		
	/*
	* numsegments
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_numsegments, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	CdbIdentity.numsegments = DatumGetInt32(attr);
	
	/*
	* dbid
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_dbid, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	CdbIdentity.dbid = DatumGetInt32(attr);
		
	/*
	* content
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_content, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	CdbIdentity.segindex = DatumGetInt32(attr);

	/*
	 * End the heapscan and close the relation that we opened.
	 */
	heap_endscan(cdb_cdb_scan);
	heap_close(cdb_cdb_rel, AccessShareLock);

	/*
	 * Validate the cdb_role against the content.
	 * If the role is DISPATCH, we must be an entry database,
	 * and if the role is EXECUTE, we must be a segment database.
	 */
	if ( Cdb_role == CDB_ROLE_DISPATCH && CdbIdentity.segindex >= 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
				 errmsg("Cannot have a Cdb_role of dispatch when connected to a segment database." 
				 		)));
	}
	else if ( Cdb_role == CDB_ROLE_EXECUTE && CdbIdentity.segindex < 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
				 errmsg("Cannot have a Cdb_role of execute when connected to an entry database." 
				 		)));
	}
	
	if ( Cdb_role != CDB_ROLE_UTILITY && CdbIdentity.segindex != Cdb_segment )
	{
		if (Cdb_segment < -1)
		{
			Cdb_segment = CdbIdentity.segindex;
			ereport(WARNING,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("Cdb.Cdb segindex not same as Cdb_segment." 
					 		)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("Cdb.Cdb segindex not same as Cdb_segment." 
					 		)));
		}
		
	}
}

/*
 * freeCdbCdb() causes the global variable holding the cdb.cdb table's singleton row to be 
 * reinitialized to empty, releasing memory that was allocated when it was loaded.
 * It is called from cdblink_disconnect().
 */
void freeCdbCdb(void)
{
	 
	if ( CdbIdentity.cdbname != NULL )
	{
		pfree( CdbIdentity.cdbname );
		CdbIdentity.cdbname = NULL;
	}

	CdbIdentity.numsegments = 0;
	CdbIdentity.dbid = -1;
	CdbIdentity.segindex = -999;
}


/*
 * getCdbComponentDatabases
 *
 * Obtains a structure representing the following JOIN:
 *
 *	 SELECT 
 *			 d.dbid,
 *			 d.content,
 * 			 d.isprimary, 
 * 			 h.hostname,
 * 			 l.port, 
 * 			 l.datadir, 
 * 			 l.listenerid,
 * 			 l.cdbsuperuser,
 * 			 l.cdbsuperpswd
 *		 FROM
 *			 cdb.host h,
 *			 cdb.listener l,
 *			 cdb.pgdatabase d,
 *		 WHERE
 *			 h.hostid = l.hostid
 *			 AND l.listenerid = d.listenerid
 *		 ORDER BY 
 *			 d.dbid, d.isprimary DESC
 *
 * Storage for the SegmentInstances block and all subsidiary
 * strucures are allocated from the caller's context.
 */
CdbComponentDatabases *getCdbComponentDatabases(void)
{
	CdbComponentDatabases	*component_databases = NULL;

	/*
	 * Relations for each of the 3 tables we need to read
	 */
	Relation			cdb_pgdatabase_rel;
	Relation			cdb_listener_rel;
	Relation			cdb_host_rel;

	/*
	 * HeapTuples for each of the 3 tables we need to read
	 */
	HeapTuple			cdb_pgdatabase_tuple = NULL;
	HeapTuple			cdb_listener_tuple = NULL;
	HeapTuple			cdb_host_tuple = NULL;
	
	/*
	 * ScanDescs for each of the 3 tables we need to read
	 */
	HeapScanDesc		cdb_pgdatabase_scan;
	SysScanDesc			cdb_listener_scan;
	SysScanDesc			cdb_host_scan;
	
	/*
	 * Index Oids for each of the 2 indexes we use
	 */
	Oid					cdb_listener_index_oid;
	Oid					cdb_host_index_oid;

	/*
	 * Oid for cdb namespace
	 */
	Oid					cdb_namespace_oid;
	
	/*
	 * Initial size for info arrays.
	 */
	int					segment_array_size = 128;
	int					entry_array_size = 4;

	/*
	 * ScanKeyDatas for the 2 scans we do.
	 */
	ScanKeyData			listener_key[1];
	ScanKeyData			host_key[1];
	
	/*
	 * isNull and attr are used when getting the data for a specific column from a HeapTuple
	 */
	bool				isNull;
	Datum				attr;
	
	/*
	 * Local variables for fields from the rows of the tables that we are reading.
	 */
	int					dbid;
	int					content;
	int					listenerid;
	int					hostid;
	bool				isprimary;
    bool             valid;
    bool             definedprimary;


	int i;
	int x = 0;

	/*
	 * Allocate component_databases return structure and 
	 * component_databases->segment_db_info array with an initial size
	 * of 128, and component_databases->entry_db_info with an initial size of 4.  
	 * If necessary during row fetching, we grow these by doubling each time we run out.
	 */
	component_databases = palloc0( sizeof(CdbComponentDatabases) );

	component_databases->segment_db_info	= 
		(CdbComponentDatabaseInfo *)palloc0(sizeof(CdbComponentDatabaseInfo) * segment_array_size);

	component_databases->entry_db_info		= 
		(CdbComponentDatabaseInfo *)palloc0(sizeof(CdbComponentDatabaseInfo) * entry_array_size);

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
	 * The strategy is to scan through cdb.pgdatabases, and, for each row, 
	 * do lookups into other tables.
	 * We do this by index or heap scans with scan keys on the other tables.
	 */

	/*
	 * First, open all the table relations.
	 * This will ereport an ERROR is unsuccessful in opening the relation.
	 */
	cdb_pgdatabase_rel = HeapOpenCDBObject( CdbSegmentRelationName, cdb_namespace_oid, true );
	cdb_listener_rel = HeapOpenCDBObject( CdbListenerRelationName, cdb_namespace_oid, true );
	cdb_host_rel = HeapOpenCDBObject( CdbHostRelationName, cdb_namespace_oid, true );
	
	/*
	 * Convert index names to Oids
	 */
	cdb_listener_index_oid = get_relname_relid(CdbListenerPrimaryKeyIndexName, 
												cdb_namespace_oid);
	cdb_host_index_oid = get_relname_relid(CdbHostPrimaryKeyIndexName, 
												cdb_namespace_oid);
	
	/*
	 * Main loop: loop through cdb.pgdatabase by doing a heap scan
	 */
	cdb_pgdatabase_scan = heap_beginscan(cdb_pgdatabase_rel, SnapshotNow, 0, NULL);
	
	while ( HeapTupleIsValid(cdb_pgdatabase_tuple = heap_getnext(cdb_pgdatabase_scan, ForwardScanDirection)) )
	{
		/*
		* Grab the fields that we need from cdb.pgdatabase.
		* We do this first, because until we read them, we don't know whether
		* this is an entry database row or a segment database row.
		*/
		CdbComponentDatabaseInfo *pRow;

		/*
		* dbid
		*/
		attr = heap_getattr(cdb_pgdatabase_tuple, Anum_cdb_pgdatabase_dbid, cdb_pgdatabase_rel->rd_att, &isNull);
		Assert( !isNull );
		dbid = DatumGetInt32(attr);
		
		/*
		* listenerid
		*/
		attr = heap_getattr(cdb_pgdatabase_tuple, Anum_cdb_pgdatabase_listenerid, cdb_pgdatabase_rel->rd_att, &isNull);
		Assert( !isNull );
		listenerid = DatumGetInt32(attr);

		/*
		* content
		*/
		attr = heap_getattr(cdb_pgdatabase_tuple, Anum_cdb_pgdatabase_content, cdb_pgdatabase_rel->rd_att, &isNull);
		Assert( !isNull );
		content = DatumGetInt32(attr);

		/*
		* isprimary
		*/
		attr = heap_getattr(cdb_pgdatabase_tuple, Anum_cdb_pgdatabase_isprimary, cdb_pgdatabase_rel->rd_att, &isNull);
		Assert( !isNull );
		isprimary = DatumGetBool(attr);

        /*
        * valid
        */
        attr = heap_getattr(cdb_pgdatabase_tuple, Anum_cdb_pgdatabase_valid, cdb_pgdatabase_rel->rd_att, &isNull);
        Assert( !isNull );
        valid = DatumGetBool(attr);
        
        /*
        * isprimary
        */
        attr = heap_getattr(cdb_pgdatabase_tuple, Anum_cdb_pgdatabase_definedprimary, cdb_pgdatabase_rel->rd_att, &isNull);
        Assert( !isNull );
        definedprimary = DatumGetBool(attr);


		/*
		* Determine which array to place this rows data in: content or segment, based on the content field.
		*/
		
		if ( content >= 0 )
		{
			if ( component_databases->total_segment_dbs >= segment_array_size )
			{
				CdbComponentDatabaseInfo *pOld;
				/*
				* Expand CdbComponentDatabaseInfo array if we've used up currently allocated space
				*/
				segment_array_size *= 2;
				pOld = component_databases->segment_db_info;
				component_databases->segment_db_info = (CdbComponentDatabaseInfo *)
													repalloc(pOld, sizeof(CdbComponentDatabaseInfo) * segment_array_size);
			}
			
			pRow = &component_databases->segment_db_info[component_databases->total_segment_dbs];
			component_databases->total_segment_dbs++;
		}
		else
		{
			if ( component_databases->total_entry_dbs >= entry_array_size )
			{
				CdbComponentDatabaseInfo *pOld;
				/*
				* Expand CdbComponentDatabaseInfo array if we've used up currently allocated space
				*/
				entry_array_size *= 2;
				pOld = component_databases->entry_db_info;
				component_databases->entry_db_info = (CdbComponentDatabaseInfo *)
													repalloc(pOld, sizeof(CdbComponentDatabaseInfo) * entry_array_size);
			}
			
			pRow = &component_databases->entry_db_info[component_databases->total_entry_dbs];
			component_databases->total_entry_dbs++;
		}

		pRow->dbid = dbid;
		pRow->listenerid = listenerid;
		pRow->segindex = content;
		pRow->isprimary = isprimary;
        pRow->valid = valid;
        pRow->definedprimary = definedprimary;

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

		/*
		* cdbsuperuser
		*/
		attr = heap_getattr(cdb_listener_tuple, Anum_cdb_listener_cdbsuperuser, cdb_listener_rel->rd_att, &isNull);
		Assert( !isNull );
		pRow->cdbsuperuser = DatumGetCString(DirectFunctionCall1(textout, attr));
		
		/*
		* cdbsuperpswd
		*/
		attr = heap_getattr(cdb_listener_tuple, Anum_cdb_listener_cdbsuperpswd, cdb_listener_rel->rd_att, &isNull);
		if ( !isNull )
			pRow->cdbsuperpswd = DatumGetCString(DirectFunctionCall1(textout, attr));

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
	}
	
	/*
	 * Validate that there exists at least one entry and one segment database in the configuration
	 */
	if ( component_databases->total_segment_dbs == 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("MPP number of segment databases cannot be 0"
		     )));
	}
	if ( component_databases->total_entry_dbs == 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("MPP number of entry databases cannot be 0"
		     )));
	}

	/*
	 * Now sort the data by segindex, isprimary desc
	 */
	qsort( component_databases->segment_db_info, 
			component_databases->total_segment_dbs, sizeof(CdbComponentDatabaseInfo),
   			  CdbComponentDatabaseInfoCompare);
    		  
	qsort( component_databases->entry_db_info, 
			component_databases->total_entry_dbs, sizeof(CdbComponentDatabaseInfo),
   			  CdbComponentDatabaseInfoCompare);

	 /*
	 * Now count the number of distinct segindexes.
	 * Since it's sorted, this is easy.
	 */
	for ( i=0; i<component_databases->total_segment_dbs; i++ )
	{
		if ( i == 0 || 
		 	(component_databases->segment_db_info[i].segindex != 
		 	component_databases->segment_db_info[i-1].segindex ) )
			component_databases->total_segments++;	 		
	}
	 
	 
	 if ( CdbIdentity.numsegments <= 0 ) /* cdb.cdb not available or not read in yet */
	 {
	 	CdbIdentity.numsegments = component_databases->total_segments;
	 	CdbIdentity.segindex = -1;
	 	CdbIdentity.dbid = 1;
	 }
	 
	 /*
	  *  Validate that cdb_numsegments == segment_databases->total_segment_dbs
	  */
	 if ( getCdbSegmentCount() != component_databases->total_segments )
	 {
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("MPP number of segments inconsistency: count is %d from %s.%s table, but %d from getCdbComponentDatabases()",
		    getCdbSegmentCount(), CdbNamespaceName, CdbCdbRelationName, component_databases->total_segments )));
	 }

	/*
	* Now validate that our identity is present in the entry databases
	*/
	for ( i=0; i<component_databases->total_entry_dbs; i++ )
	{
		CdbComponentDatabaseInfo* pInfo = &component_databases->entry_db_info[i];
		if ( pInfo->dbid == CdbIdentity.dbid && pInfo->segindex == Cdb_segment )
		{
			break;
		}
	}
	if ( i == component_databases->total_entry_dbs )
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				errmsg("Cannot locate entry database represented by cdb.cdb row in cdb.pgdatabase: dbid %d content %d",
				CdbIdentity.dbid, Cdb_segment )));
	}

	/*
	 * Now validate that the segindexes for the segment databases are between 0 and CdbIdentity.numsegments-1 inclusive,
	 * and that we hit them all. Since it's sorted, this is relatively easy.
	 */
	x = 0;
	for ( i=0; i<getCdbSegmentCount(); i++ )
	{
		int this_segindex=-1;
		while ( x < component_databases->total_segment_dbs )
		{
			this_segindex = component_databases->segment_db_info[x].segindex;
			if ( this_segindex < i )
				x++;
			else if ( this_segindex == i )
				break;
			else if ( this_segindex > i )
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Content values not valid in cdb.pgdatabase table.  They must be in the range 0 to %d inclusive",
						getCdbSegmentCount() - 1 )));
			}
		}
		if ( this_segindex != i )
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					errmsg("Content values not valid in cdb.pgdatabase table.  They must be in the range 0 to %d inclusive",
					getCdbSegmentCount() - 1 )));
		}
	}

	/*
	 * Cleanup invoves ending the heap scan of cdb.pgdatabase, and closing all the relations we opened.
	 */
	heap_endscan(cdb_pgdatabase_scan);

	heap_close(cdb_pgdatabase_rel, AccessShareLock);
	heap_close(cdb_listener_rel, AccessShareLock);
	heap_close(cdb_host_rel, AccessShareLock);
	
	return component_databases;
}


/*
 * freeCdbComponentDatabases
 *
 * Releases the storage occupied by the CdbComponentDatabases
 * struct pointed to by the argument.
 */
void freeCdbComponentDatabases(CdbComponentDatabases *pDBs)
{
	int i;
	
	if ( pDBs == NULL )
		return;
		
	if ( pDBs->segment_db_info != NULL )
	{
		for ( i=0; i<pDBs->total_segment_dbs; i++ )
		{
			CdbComponentDatabaseInfo *cdi = &pDBs->segment_db_info[i]; 
			freeCdbComponentDatabaseInfo( cdi );
		}
		
		pfree( pDBs->segment_db_info );
	}

	if ( pDBs->entry_db_info != NULL )
	{
		for ( i=0; i<pDBs->total_entry_dbs; i++ )
		{
			CdbComponentDatabaseInfo *cdi = &pDBs->entry_db_info[i]; 
			freeCdbComponentDatabaseInfo( cdi );
		}
		
		pfree( pDBs->entry_db_info );
	}

	pfree( pDBs );
}

/*
 * freeCdbComponentDatabaseInfo:
 * Releases any storage allocated for members variables of a CdbComponentDatabaseInfo struct.
 */
void freeCdbComponentDatabaseInfo( CdbComponentDatabaseInfo *cdi )
{
	if ( cdi == NULL )
		return;

	if ( cdi->hostname != NULL )
	    pfree(cdi->hostname);
	if ( cdi->datadir != NULL )
	    pfree(cdi->datadir);
	if ( cdi->cdbsuperuser != NULL )
	    pfree(cdi->cdbsuperuser);
	if ( cdi->cdbsuperpswd != NULL )
	    pfree(cdi->cdbsuperpswd);
}

/*
 * performs all necessary setup required for cdb mode. 
 *
 * This includes cdblink_setup() and initializing the Motion Layer.
 */
void cdb_setup(void)
{
	elog( DEBUG1, "Initializing MPP components...");
	
	cdblink_setup();
	
	/* If cdb_role is UTILITY, skip this call. */
	if ( Cdb_role != CDB_ROLE_UTILITY )
		InitMotionLayer();
        
    if ( Cdb_role == CDB_ROLE_DISPATCH )
    {
        /* connect to mirrored entry db */
        buildMirrorQDDefinition();
        /* initialize TM */
        initTM();
    }
}


/*
 * performs all necessary cleanup required when leaving cdb mode.  This is also
 * called when the process exits.
 *
 * NOTE: the arguments to this function are here only so that we can
 *       register it with on_proc_exit().  These parameters should not be 
 *		 used since there are some callers to this that pass them as NULL.
 *
 */
void cdb_cleanup(int code, Datum arg)
{
	elog( DEBUG1, "Cleaning up MPP components...");
	
	disconnectAndDestroyAllGangs();
    
    disconnectMirrorQD();
	
	if (Cdb_role != CDB_ROLE_UTILITY)
		CleanUpMotionLayer();
}


/*
 * CdbComponentDatabaseInfoCompare:
 * A compare function for CdbComponentDatabaseInfo structs
 * that compares based on , isprimary desc
 * for use with qsort.
 */
static int CdbComponentDatabaseInfoCompare(const void *p1, const void *p2)
{
	const CdbComponentDatabaseInfo *obj1 = (CdbComponentDatabaseInfo *)p1;
	const CdbComponentDatabaseInfo *obj2 = (CdbComponentDatabaseInfo *)p2;
	
	int cmp = obj1->segindex - obj2->segindex;
	if ( cmp == 0 )
		cmp = (int)obj2->isprimary - (int)obj1->isprimary;
		
	return cmp;	
}


