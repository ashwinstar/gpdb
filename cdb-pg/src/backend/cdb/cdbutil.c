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

/*
 * Helper Functions
 */
static int CdbComponentDatabaseInfoCompare(const void *p1, const void *p2);
static void freeCdbComponentDatabaseInfo( CdbComponentDatabaseInfo *cdi );

CdbCdb CdbIdentity = {NULL, 0, 0, 0};

void getCdbCdb(void)
{
	Relation			cdb_cdb_rel;
	HeapTuple			cdb_cdb_tuple = NULL;
	HeapScanDesc		cdb_cdb_scan;
	Oid					cdb_namespace_oid;
	bool				isNull;
	Datum				attr;

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
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Cannot retrieve oid of %s namespace", 
				 		CdbNamespaceName )));
	}

	cdb_cdb_rel = HeapOpenCDBObject( CdbCdbRelationName, cdb_namespace_oid, true );

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
	* cdbname
	*/
	attr = heap_getattr(cdb_cdb_tuple, Anum_cdb_cdb_cdbname, cdb_cdb_rel->rd_att, &isNull);
	Assert( !isNull );
	Name cdbname = DatumGetName(attr);

	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;
	
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

	heap_endscan(cdb_cdb_scan);
	heap_close(cdb_cdb_rel, AccessShareLock);
	
}

void freeCdbCdb(void)
{
	if ( CdbIdentity.cdbname != NULL )
	{
		pfree( CdbIdentity.cdbname );
		CdbIdentity.cdbname = NULL;
	}

	CdbIdentity.numsegments = 0;
	CdbIdentity.dbid = 0;
	CdbIdentity.segindex = 0;
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
 * 			 d.dbname, 
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
	
	int					dbid;
	int					content;
	int					listenerid;
	int					hostid;
	bool				isprimary;
	char				*dbname;

	/*
	 * Allocate segment_databases return structure and 
	 * segment_databases->segment_db_info array with an initial size
	 * of 128.  If necessary during row fetching, we grow this by
	 * doubling each time we run out.
	 */
	component_databases = palloc0( sizeof(CdbComponentDatabases) );
	component_databases->segment_db_info	= (CdbComponentDatabaseInfo *)palloc0(sizeof(CdbComponentDatabaseInfo) * segment_array_size);
	component_databases->entry_db_info		= (CdbComponentDatabaseInfo *)palloc0(sizeof(CdbComponentDatabaseInfo) * entry_array_size);

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
	 * This will ereport and ERROR is unsuccessful in opening the relation.
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
		* Grab the fields that we need from cdb.pgdatabase
		*/

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
		* dbname
		*/
		attr = heap_getattr(cdb_pgdatabase_tuple, Anum_cdb_pgdatabase_dbname, cdb_pgdatabase_rel->rd_att, &isNull);
		Assert( !isNull );
		dbname = DatumGetCString(DirectFunctionCall1(textout, attr));

		CdbComponentDatabaseInfo *pRow;
		if ( content >= 0 )
		{
			if ( component_databases->total_segment_dbs >= segment_array_size )
			{
				/*
				* Expand CdbComponentDatabaseInfo array if we've used up currently allocated space
				*/
				segment_array_size *= 2;
				CdbComponentDatabaseInfo *pOld = component_databases->segment_db_info;
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
				/*
				* Expand CdbComponentDatabaseInfo array if we've used up currently allocated space
				*/
				entry_array_size *= 2;
				CdbComponentDatabaseInfo *pOld = component_databases->entry_db_info;
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
		pRow->dbname = dbname;

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
	 int i;
	 
	 for ( i=0; i<component_databases->total_segment_dbs; i++ )
	 {
	 	if ( i == 0 || 
		 	(component_databases->segment_db_info[i].segindex != 
		 	component_databases->segment_db_info[i-1].segindex ) )
			component_databases->total_segments++;	 		
	 }
	 
	 /*
	  *  Validate that cdb_numsegments == segment_databases->total_segment_dbs
	  */
	 if ( CdbIdentity.numsegments != component_databases->total_segments )
	 {
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("CDB number of segments inconsistency: count is %d from %s.%s table, but %d from getCdbComponentDatabases()",
		    CdbIdentity.numsegments, CdbNamespaceName, CdbCdbRelationName, component_databases->total_segments )));
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

void freeCdbComponentDatabaseInfo( CdbComponentDatabaseInfo *cdi )
{
	if ( cdi == NULL )
		return;

	if ( cdi->dbname != NULL )
	    pfree(cdi->dbname);
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
 *
 */
void cdb_setup(void)
{
	elog( DEBUG1, "Initializing CDB components...");
	cdblink_setup();
	
	/* If cdb_role is UTILITY, skip this call. */
	if ( Cdb_role != CDB_ROLE_UTILITY )
		InitMotionLayer();
}


/*
 * performs all necessary cleanup required when leaving cdb mode.
 *
 */
void cdb_cleanup(void)
{
	elog( DEBUG1, "Cleaning up CDB components...");
	
	cdblink_disconnect( 0, 0 );
		
	//TODO: we probably need something to clean up the MotionLayer.
}


/*
 * CdbComponentDatabaseInfoCompare:
 * A compare function for SegmentDatabaseInfo structs
 * that compares based on segindex, isprimary desc
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
