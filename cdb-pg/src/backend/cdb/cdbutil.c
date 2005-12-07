/*-------------------------------------------------------------------------
 *
 * cdbutil.c
 *	  Internal utility support functions for mpp/PostgreSQL.
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
#include "utils/memutils.h"
#include "catalog/mpp_configuration.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbutil.h"
#include "cdb/cdblink.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbgang.h"
#include "cdb/ml_ipc.h"			/* listener_setup */
#include "cdb/cdbtm.h"
#include "cdb/cdbfts.h"




/*
 * Helper Functions
 */
static int	CdbComponentDatabaseInfoCompare(const void *p1, const void *p2);
static void freeCdbComponentDatabaseInfo(CdbComponentDatabaseInfo *cdi);
static CdbComponentDatabases *getCdbComponentInfo(void);

/*
 * getMppMpp() causes the mpp.mpp table's singleton row to be read into a global structure
 * This is called from cdblink_setup().
 * The global variable is named MppIdentity, and is declared in cdbvars.h and defined in cdbvars.c.
 * There is one member variable that is a string.  It is allocated from the TopMemoryContext
 * because this information is intended to last for the life of the connection.
 * This call is made whether the database is an entry database or a segment database.
 */
void
getMppMpp(void)
{
	Relation	mpp_mpp_rel;
	HeapTuple	mpp_mpp_tuple = NULL;
	HeapScanDesc mpp_mpp_scan;
	bool		isNull;
	Datum		attr;
	Name		mppname;
	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;

	if (Mpp_role == MPP_ROLE_UTILITY)
		elog(DEBUG1, "WARNING: called getMppMpp when in utility mode");

	if (Mpp_role == MPP_ROLE_DISPATCH && Mpp_segment < -1)
		Mpp_segment = -1;

	/*
	 * Release storage for the cdbname if not NULL
	 */
	if (MppIdentity.mppname != NULL)
	{
		pfree(MppIdentity.mppname);
		MppIdentity.mppname = NULL;
	}




	/*
	 * Open the mpp table relation
	 */
	mpp_mpp_rel = heap_open(MppMppRelationId, AccessShareLock);

	/*
	 * Read the one row table mpp.mpp.
	 * We currently require that this be there or else we ereport an ERROR.
	 */
	mpp_mpp_scan = heap_beginscan(mpp_mpp_rel, SnapshotNow, 0, NULL);
	mpp_mpp_tuple = heap_getnext(mpp_mpp_scan, ForwardScanDirection);
	if (!HeapTupleIsValid(mpp_mpp_tuple))
	{
		ereport(WARNING,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("No rows found in pg_catalog.%s.",
						MppMppRelationName)));
		heap_endscan(mpp_mpp_scan);
		heap_close(mpp_mpp_rel, AccessShareLock);
		return;
	}

	/*
	* Grab the fields that we need from mpp
	*/

	/*
	* mppname
	*/
	attr = heap_getattr(mpp_mpp_tuple, Anum_mpp_mpp_mppname, mpp_mpp_rel->rd_att, &isNull);
	Assert(!isNull);
	mppname = DatumGetName(attr);




	/*
	* Switch to the TopMemoryContext for the cdbname, so it lasts for the life
	* of the connection.  It is released in freeMppMpp, which is called from
	* cdblink_disconnect().
	*/
	oldContext = MemoryContextSwitchTo(permanentContext);
	MppIdentity.mppname = pstrdup(mppname->data);
	MemoryContextSwitchTo(oldContext);

	/*
	* numsegments
	*/
	attr = heap_getattr(mpp_mpp_tuple, Anum_mpp_mpp_numsegments, mpp_mpp_rel->rd_att, &isNull);
	Assert(!isNull);
	MppIdentity.numsegments = DatumGetInt16(attr);

	/*
	* dbid
	*/
	attr = heap_getattr(mpp_mpp_tuple, Anum_mpp_mpp_dbid, mpp_mpp_rel->rd_att, &isNull);
	Assert(!isNull);
	MppIdentity.dbid = DatumGetInt16(attr);

	/*
	* content
	*/
	attr = heap_getattr(mpp_mpp_tuple, Anum_mpp_mpp_content, mpp_mpp_rel->rd_att, &isNull);
	Assert(!isNull);
	MppIdentity.segindex = DatumGetInt16(attr);

	/*
	 * End the heapscan and close the relation that we opened.
	 */
	heap_endscan(mpp_mpp_scan);
	heap_close(mpp_mpp_rel, AccessShareLock);

	/*
	 * Validate the mpp_role against the content.
	 * If the role is DISPATCH, we must be an entry database,
	 * and if the role is EXECUTE, we must be a segment database.
	 */
	if (Mpp_role == MPP_ROLE_DISPATCH && MppIdentity.segindex >= 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
				 errmsg("Cannot have a Mpp_role of dispatch when connected to a segment database."
						)));
	}
	else if (Mpp_role == MPP_ROLE_EXECUTE && MppIdentity.segindex < 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
				 errmsg("Cannot have a Mpp_role of execute when connected to an entry database."
						)));
	}

	if (Mpp_role != MPP_ROLE_UTILITY && MppIdentity.segindex != Mpp_segment)
	{
		if (Mpp_segment < -1)
		{
			Mpp_segment = MppIdentity.segindex;
			ereport(WARNING,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("Mpp.Mpp segindex not same as Mpp_segment."
							)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("mpp.mpp segindex not same as Mpp_segment."
							)));
		}

	}
}

/*
 * freeMppMpp() causes the global variable holding the mpp.mpp table's singleton row to be
 * reinitialized to empty, releasing memory that was allocated when it was loaded.
 * It is called from cdblink_disconnect().
 */
void
freeMppMpp(void)
{

	if (MppIdentity.mppname != NULL)
	{
		pfree(MppIdentity.mppname);
		MppIdentity.mppname = NULL;
	}

	MppIdentity.numsegments = 0;
	MppIdentity.dbid = -1;
	MppIdentity.segindex = -999;
}


/*
 * getCdbComponentDatabases
 *
 *
 * Storage for the SegmentInstances block and all subsidiary
 * strucures are allocated from the caller's context.
 */
static CdbComponentDatabases *
getCdbComponentInfo(void)
{
	CdbComponentDatabases *component_databases = NULL;

	Relation	mpp_configuration_rel;
	HeapTuple	mpp_configuration_tuple = NULL;
	HeapScanDesc mpp_configuration_scan;

	/*
	 * Initial size for info arrays.
	 */
	int			segment_array_size = 128;
	int			entry_array_size = 4;

	/*
	 * isNull and attr are used when getting the data for a specific column from a HeapTuple
	 */
	bool		isNull;
	Datum		attr;

	/*
	 * Local variables for fields from the rows of the tables that we are reading.
	 */
	int			dbid;
	int			content;
	bool		isprimary;
	bool		valid;
	bool		definedprimary;


	int			i;
	int			x = 0;

	/*
	 * Allocate component_databases return structure and
	 * component_databases->segment_db_info array with an initial size
	 * of 128, and component_databases->entry_db_info with an initial size of 4.
	 * If necessary during row fetching, we grow these by doubling each time we run out.
	 */
	component_databases = palloc0(sizeof(CdbComponentDatabases));

	component_databases->segment_db_info =
		(CdbComponentDatabaseInfo *) palloc0(sizeof(CdbComponentDatabaseInfo) * segment_array_size);

	component_databases->entry_db_info =
		(CdbComponentDatabaseInfo *) palloc0(sizeof(CdbComponentDatabaseInfo) * entry_array_size);


	mpp_configuration_rel = heap_open(MppConfigurationRelationId, AccessShareLock);
	
	mpp_configuration_scan = heap_beginscan(mpp_configuration_rel, SnapshotNow, 0, NULL);
	
	while (HeapTupleIsValid(mpp_configuration_tuple = heap_getnext(mpp_configuration_scan, ForwardScanDirection)))
	{
		/*
		* Grab the fields that we need from mpp_configuration.
		* We do this first, because until we read them, we don't know whether
		* this is an entry database row or a segment database row.
		*/
		CdbComponentDatabaseInfo *pRow;
		
		/*
		* content
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_content, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		content = DatumGetInt16(attr);
		
		/*
		* dbid
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_dbid, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		dbid = DatumGetInt16(attr);
		
		
		/*
		* definedprimary
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_definedprimary, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		definedprimary = DatumGetBool(attr);
		
		/*
		* isprimary
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_isprimary, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		isprimary = DatumGetBool(attr);
		
		/*
		* valid
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_valid, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		valid = DatumGetBool(attr);
		
		/*
		* Determine which array to place this rows data in: content or segment, based on the content field.
		*/

		if (content >= 0)
		{
			if (component_databases->total_segment_dbs >= segment_array_size)
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
			if (component_databases->total_entry_dbs >= entry_array_size)
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
		pRow->segindex = content;
		pRow->isprimary = isprimary;
		pRow->valid = valid;
		pRow->definedprimary = definedprimary;
		
		/*
		* hostname
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_hostname, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		pRow->hostname = DatumGetCString(DirectFunctionCall1(textout, attr));
		
		/*
		* port
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_port, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		pRow->port = DatumGetInt32(attr);
		
		/*
		* datadir
		*/
		attr = heap_getattr(mpp_configuration_tuple, Anum_mpp_configuration_datadir, mpp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		pRow->datadir = DatumGetCString(DirectFunctionCall1(textout, attr));
		
	}



	/*
	 * Validate that there exists at least one entry and one segment database in the configuration
	 */
	if (component_databases->total_segment_dbs == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("MPP number of segment databases cannot be 0"
						)));
	}
	if (component_databases->total_entry_dbs == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("MPP number of entry databases cannot be 0"
						)));
	}

	/*
	 * Now sort the data by segindex, isprimary desc
	 */
	qsort(component_databases->segment_db_info,
	component_databases->total_segment_dbs, sizeof(CdbComponentDatabaseInfo),
		  CdbComponentDatabaseInfoCompare);

	qsort(component_databases->entry_db_info,
	  component_databases->total_entry_dbs, sizeof(CdbComponentDatabaseInfo),
		  CdbComponentDatabaseInfoCompare);

	/*
	* Now count the number of distinct segindexes.
	* Since it's sorted, this is easy.
	*/
	for (i = 0; i < component_databases->total_segment_dbs; i++)
	{
		if (i == 0 ||
			(component_databases->segment_db_info[i].segindex !=
			 component_databases->segment_db_info[i - 1].segindex))
			component_databases->total_segments++;
	}


	if (MppIdentity.numsegments <= 0)	/* mpp.mpp not available or not read in yet */
	{
		MppIdentity.numsegments = component_databases->total_segments;
		MppIdentity.segindex = -1;
		MppIdentity.dbid = 1;
	}

	/*
	 *	Validate that mpp_numsegments == segment_databases->total_segment_dbs
	 */
	if (getMPPSegmentCount() != component_databases->total_segments)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("MPP number of segments inconsistency: count is %d from pg_catalog.%s table, but %d from getCdbComponentDatabases()",
						getMPPSegmentCount(),  MppMppRelationName, component_databases->total_segments)));
	}

	/*
	* Now validate that our identity is present in the entry databases
	*/
	for (i = 0; i < component_databases->total_entry_dbs; i++)
	{
		CdbComponentDatabaseInfo *pInfo = &component_databases->entry_db_info[i];

		if (pInfo->dbid == MppIdentity.dbid && pInfo->segindex == Mpp_segment)
		{
			break;
		}
	}
	if (i == component_databases->total_entry_dbs)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("Cannot locate entry database represented by mpp_mpp row in mpp_configuration: dbid %d content %d",
						MppIdentity.dbid, Mpp_segment)));
	}

	/*
	 * Now validate that the segindexes for the segment databases are between 0 and MppIdentity.numsegments-1 inclusive,
	 * and that we hit them all. Since it's sorted, this is relatively easy.
	 */
	x = 0;
	for (i = 0; i < getMPPSegmentCount(); i++)
	{
		int			this_segindex = -1;

		while (x < component_databases->total_segment_dbs)
		{
			this_segindex = component_databases->segment_db_info[x].segindex;
			if (this_segindex < i)
				x++;
			else if (this_segindex == i)
				break;
			else if (this_segindex > i)
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("Content values not valid in mpp_configuration table.  They must be in the range 0 to %d inclusive",
								getMPPSegmentCount() - 1)));
			}
		}
		if (this_segindex != i)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("Content values not valid in mpp_configuration table.  They must be in the range 0 to %d inclusive",
							getMPPSegmentCount() - 1)));
		}
	}

	/*
	 * Cleanup invoves ending the heap scan of mpp_configuration, and closing all the relations we opened.
	 */
	heap_endscan(mpp_configuration_scan);

	heap_close(mpp_configuration_rel, AccessShareLock);


	return component_databases;
}

/*
 * getCdbComponentDatabases
 *
 *
 * Storage for the SegmentInstances block and all subsidiary
 * strucures are allocated from the caller's context.
 */
CdbComponentDatabases *
getCdbComponentDatabases(void)
{
	return applyTransientState(getCdbComponentInfo());
}


/*
 * freeCdbComponentDatabases
 *
 * Releases the storage occupied by the CdbComponentDatabases
 * struct pointed to by the argument.
 */
void
freeCdbComponentDatabases(CdbComponentDatabases *pDBs)
{
	int			i;

	if (pDBs == NULL)
		return;

	if (pDBs->segment_db_info != NULL)
	{
		for (i = 0; i < pDBs->total_segment_dbs; i++)
		{
			CdbComponentDatabaseInfo *cdi = &pDBs->segment_db_info[i];

			freeCdbComponentDatabaseInfo(cdi);
		}

		pfree(pDBs->segment_db_info);
	}

	if (pDBs->entry_db_info != NULL)
	{
		for (i = 0; i < pDBs->total_entry_dbs; i++)
		{
			CdbComponentDatabaseInfo *cdi = &pDBs->entry_db_info[i];

			freeCdbComponentDatabaseInfo(cdi);
		}

		pfree(pDBs->entry_db_info);
	}

	pfree(pDBs);
}

/*
 * freeCdbComponentDatabaseInfo:
 * Releases any storage allocated for members variables of a CdbComponentDatabaseInfo struct.
 */
void
freeCdbComponentDatabaseInfo(CdbComponentDatabaseInfo *cdi)
{
	if (cdi == NULL)
		return;

	if (cdi->hostname != NULL)
		pfree(cdi->hostname);
	if (cdi->datadir != NULL)
		pfree(cdi->datadir);

}

/*
 * performs all necessary setup required for mpp mode.
 *
 * This includes cdblink_setup() and initializing the Motion Layer.
 */
void
cdb_setup(void)
{
	elog(DEBUG1, "Initializing MPP components...");

	cdblink_setup();

	/* If mpp_role is UTILITY, skip this call. */
	if (Mpp_role != MPP_ROLE_UTILITY)
		InitMotionLayer();

	if (Mpp_role == MPP_ROLE_DISPATCH)
	{
		/* connect to mirrored entry db */
		buildMirrorQDDefinition();
		/* initialize TM */
		initTM();
	}
}


/*
 * performs all necessary cleanup required when leaving mpp mode.  This is also
 * called when the process exits.
 *
 * NOTE: the arguments to this function are here only so that we can
 *		 register it with on_proc_exit().  These parameters should not be
 *		 used since there are some callers to this that pass them as NULL.
 *
 */
void
cdb_cleanup(int code, Datum arg)
{
	elog(DEBUG1, "Cleaning up MPP components...");

	disconnectAndDestroyAllGangs();

	disconnectMirrorQD();

	if (Mpp_role != MPP_ROLE_UTILITY)
		CleanUpMotionLayer();
}


/*
 * CdbComponentDatabaseInfoCompare:
 * A compare function for CdbComponentDatabaseInfo structs
 * that compares based on , isprimary desc
 * for use with qsort.
 */
static int
CdbComponentDatabaseInfoCompare(const void *p1, const void *p2)
{
	const CdbComponentDatabaseInfo *obj1 = (CdbComponentDatabaseInfo *) p1;
	const CdbComponentDatabaseInfo *obj2 = (CdbComponentDatabaseInfo *) p2;

	int			cmp = obj1->segindex - obj2->segindex;

	if (cmp == 0)
		cmp = (int) obj2->isprimary - (int) obj1->isprimary;

	return cmp;
}
