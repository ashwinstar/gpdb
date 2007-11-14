/*-------------------------------------------------------------------------
 *
 * cdbutil.c
 *	  Internal utility support functions for Greenplum Database/PostgreSQL.
 *
 * Copyright (c) 2005-2006, greenplum inc
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
#include "optimizer/geqo_random.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "catalog/gp_configuration.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbutil.h"
#include "cdb/cdblink.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbdisp.h"
#include "cdb/ml_ipc.h"			/* listener_setup */
#include "cdb/cdbtm.h"
#include "cdb/cdbfts.h"
#include "libpq/ip.h"
#include "postmaster/bgwriter.h"

/*
 * Helper Functions
 */
static int	CdbComponentDatabaseInfoCompare(const void *p1, const void *p2);
static void freeCdbComponentDatabaseInfo(CdbComponentDatabaseInfo *cdi);

static char **segment_ip_cache = NULL;
static int segment_ip_cache_limit = 0;

/*
 * getGpId() causes the gp_id table's singleton row to be read into a global structure
 * This is called from cdblink_setup().
 * The global variable is named GpIdentity, and is declared in cdbvars.h and defined in cdbvars.c.
 * There is one member variable that is a string.  It is allocated from the TopMemoryContext
 * because this information is intended to last for the life of the connection.
 * This call is made whether the database is an entry database or a segment database.
 */
void
getGpId(void)
{
	Relation	gp_id_rel;
	HeapTuple	gp_id_tuple = NULL;
	HeapScanDesc gp_id_scan;
	bool		isNull;
	Datum		attr;
	Name		gpname;
	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;

	if (Gp_role == GP_ROLE_UTILITY)
		elog(DEBUG1, "WARNING: called getGpId when in utility mode");

	if (Gp_role == GP_ROLE_DISPATCH && Gp_segment < -1)
		Gp_segment = -1;
	else if (Gp_role == GP_ROLE_DISPATCHAGENT && Gp_segment < -2)
		Gp_segment = -2;

	/*
	 * Release storage for the cdbname if not NULL
	 */
	if (GpIdentity.gpname != NULL)
	{
		pfree(GpIdentity.gpname);
		GpIdentity.gpname = NULL;
	}

	/*
	 * Open the gp_id table relation
	 */
	gp_id_rel = heap_open(GpIdRelationId, AccessShareLock);

	/*
	 * Read the one row table gp_id.
	 * We currently require that this be there or else we ereport an ERROR.
	 */
	gp_id_scan = heap_beginscan(gp_id_rel, SnapshotNow, 0, NULL);
	gp_id_tuple = heap_getnext(gp_id_scan, ForwardScanDirection);
	if (!HeapTupleIsValid(gp_id_tuple))
	{
		ereport(WARNING,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("No rows found in pg_catalog.%s.",
						GpIdRelationName)));
		heap_endscan(gp_id_scan);
		heap_close(gp_id_rel, AccessShareLock);
		return;
	}

	/*
	 * Grab the fields that we need from gp_id
	 */

	/*
	 * gpname
	 */
	attr = heap_getattr(gp_id_tuple, Anum_gp_id_gpname, gp_id_rel->rd_att, &isNull);
	Assert(!isNull);
	gpname = DatumGetName(attr);

	/*
	 * Switch to the TopMemoryContext for the cdbname, so it lasts for the life
	 * of the connection.  It is released in freeGpId, which is called from
	 * cdblink_disconnect().
	 */
	oldContext = MemoryContextSwitchTo(permanentContext);
	GpIdentity.gpname = pstrdup(gpname->data);
	MemoryContextSwitchTo(oldContext);

	/*
	 * numsegments
	 */
	attr = heap_getattr(gp_id_tuple, Anum_gp_id_numsegments, gp_id_rel->rd_att, &isNull);
	Assert(!isNull);
	GpIdentity.numsegments = DatumGetInt16(attr);

	/*
	 * dbid
	 */
	attr = heap_getattr(gp_id_tuple, Anum_gp_id_dbid, gp_id_rel->rd_att, &isNull);
	Assert(!isNull);
	GpIdentity.dbid = DatumGetInt16(attr);

	/*
	 * content
	 */
	attr = heap_getattr(gp_id_tuple, Anum_gp_id_content, gp_id_rel->rd_att, &isNull);
	Assert(!isNull);
	GpIdentity.segindex = DatumGetInt16(attr);

	/*
	 * End the heapscan and close the relation that we opened.
	 */
	heap_endscan(gp_id_scan);
	heap_close(gp_id_rel, AccessShareLock);

	/*
	 * Validate the gp_role against the content.
	 * If the role is DISPATCH, we must be an entry database,
	 * If the role is EXECUTE, we can be either a segment database or an
     * entry database.
	 */
	if (Gp_role == GP_ROLE_DISPATCH && GpIdentity.segindex >= 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
				 errmsg("Cannot have a Gp_role of dispatch when connected to a segment database."
					 )));
	}

	if (Gp_role != GP_ROLE_UTILITY && GpIdentity.segindex != Gp_segment)
	{
		if (Gp_segment < -1)
		{
			Gp_segment = GpIdentity.segindex;
			ereport(WARNING,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("gp_id segindex not same as Gp_segment."
						 )));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("gp_id segindex not same as Gp_segment."
						 )));
		}
	}

}

/*
 * freeGpId() causes the global variable holding the gp_id table's singleton row to be
 * reinitialized to empty, releasing memory that was allocated when it was loaded.
 * It is called from cdblink_disconnect().
 */
void
freeGpId(void)
{
	if (GpIdentity.gpname != NULL)
	{
		pfree(GpIdentity.gpname);
		GpIdentity.gpname = NULL;
	}

	if (Gp_role == GP_ROLE_DISPATCH && segment_ip_cache != NULL)
	{
		int i;

		for (i=0; i < segment_ip_cache_limit; i++)
		{
			if (segment_ip_cache[i])
			{
				pfree(segment_ip_cache[i]);
				segment_ip_cache[i] = NULL;
			}
		}
		pfree(segment_ip_cache);
		segment_ip_cache = NULL;
		segment_ip_cache_limit = 0;
	}

	GpIdentity.numsegments = 0;
	GpIdentity.dbid = -1;
	GpIdentity.segindex = -999;
}

/*
 * getCdbComponentDatabases
 *
 *
 * Storage for the SegmentInstances block and all subsidiary
 * strucures are allocated from the caller's context.
 */
CdbComponentDatabases *
getCdbComponentInfo(void)
{
	CdbComponentDatabases *component_databases = NULL;

	Relation	gp_configuration_rel;
	HeapTuple	gp_configuration_tuple = NULL;
	HeapScanDesc gp_configuration_scan;

	/*
	 * Initial size for info arrays.
	 */
	int			segment_array_size = 500;
	int			entry_array_size = 4; /* we currently support a max of 2 */

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

	gp_configuration_rel = heap_open(GpConfigurationRelationId, AccessShareLock);

	gp_configuration_scan = heap_beginscan(gp_configuration_rel, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(gp_configuration_tuple = heap_getnext(gp_configuration_scan, ForwardScanDirection)))
	{
		/*
		 * Grab the fields that we need from gp_configuration.
		 * We do this first, because until we read them, we don't know whether
		 * this is an entry database row or a segment database row.
		 */
		CdbComponentDatabaseInfo *pRow;

		/*
		 * content
		 */
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_content, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		content = DatumGetInt16(attr);

		/*
		 * dbid
		 */
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_dbid, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		dbid = DatumGetInt16(attr);

		/*
		 * definedprimary
		 */
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_definedprimary, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		definedprimary = DatumGetBool(attr);

		/*
		 * isprimary
		 */
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_isprimary, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		isprimary = DatumGetBool(attr);

		/*
		 * valid
		 */
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_valid, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		valid = DatumGetBool(attr);

		/*
		 * Determine which array to place this rows data in: content or segment, based on the content field.
		 */

		if (content >= 0)
		{
			/* if we have a dbid bigger than our array we'll have to grow the array. (MPP-2104) */
			if (dbid >= segment_array_size || component_databases->total_segment_dbs >= segment_array_size)
			{
				CdbComponentDatabaseInfo *pOld;

				/*
				 * Expand CdbComponentDatabaseInfo array if we've used up currently allocated space
				 */
				segment_array_size = Max((segment_array_size * 2), dbid * 2);
				pOld = component_databases->segment_db_info;
				component_databases->segment_db_info = (CdbComponentDatabaseInfo *)
					repalloc(pOld, sizeof(CdbComponentDatabaseInfo) * segment_array_size);

				/* MPP-2104: We have to grow this as well. */
				if (segment_ip_cache != NULL)
				{
					segment_ip_cache = repalloc(segment_ip_cache, sizeof(char *) * segment_array_size);
					for (i=segment_ip_cache_limit; i < segment_array_size; i++)
						segment_ip_cache[i] = NULL;
					segment_ip_cache_limit = segment_array_size;
				}
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
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_hostname, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		pRow->hostname = pstrdup(DatumGetCString(attr));
		
		/*
		 * port
		 */
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_port, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		pRow->port = DatumGetInt32(attr);

		/*
		 * datadir
		 */
		attr = heap_getattr(gp_configuration_tuple, Anum_gp_configuration_datadir, gp_configuration_rel->rd_att, &isNull);
		Assert(!isNull);
		pRow->datadir = DatumGetCString(DirectFunctionCall1(textout, attr));

		if (segment_ip_cache == NULL)
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(TopMemoryContext);
			/* Max number of nodes is the size of the segment array (plus up to two QDs) */
			segment_ip_cache_limit = segment_array_size + 10;   /* memory is cheap */
			segment_ip_cache = palloc0(segment_ip_cache_limit * sizeof(char *));
			MemoryContextSwitchTo(oldContext);
		}

        /* Note dbid is 1-based, so segment_ip_cache[0] is unused */
		Assert(dbid < segment_ip_cache_limit);
		if (segment_ip_cache[dbid] == NULL)
		{
			MemoryContext oldContext;
			int			ret;
			char		portNumberStr[32];
			char	   *service;
			struct addrinfo *addrs = NULL,
					   *addr;
			struct addrinfo hint;
			
			/* Initialize hint structure */
			MemSet(&hint, 0, sizeof(hint));
			hint.ai_socktype = SOCK_STREAM;
			hint.ai_family = AF_UNSPEC;
			
			snprintf(portNumberStr, sizeof(portNumberStr), "%d", pRow->port);
			service = portNumberStr;
				
			ret = pg_getaddrinfo_all(pRow->hostname, service, &hint, &addrs);
			if (ret || !addrs)
			{
				if (addrs)
					pg_freeaddrinfo_all(hint.ai_family, addrs);
				ereport(ERROR,
						(errmsg("could not translate host name \"%s\", port \"%d\" to address: %s",
								pRow->hostname, pRow->port, gai_strerror(ret))));
			}
			
			/* save in the cache context */
			oldContext = MemoryContextSwitchTo(TopMemoryContext);
			
			for (addr = addrs; addr; addr = addr->ai_next)
			{
#ifdef HAVE_UNIX_SOCKETS
				/* Ignore AF_UNIX sockets, if any are returned. */
				if (addr->ai_family == AF_UNIX)
					continue;
#endif
				if (addr->ai_family == AF_INET) /* IPv4 address */
				{
					char		hostinfo[NI_MAXHOST];
					pg_getnameinfo_all((struct sockaddr_storage *)addr->ai_addr, addr->ai_addrlen,
							hostinfo, sizeof(hostinfo),
												   NULL, 0,
												   NI_NUMERICHOST);
					segment_ip_cache[dbid] = pstrdup(hostinfo);
					break;
				}
			}
			
			MemoryContextSwitchTo(oldContext);
			
#ifdef HAVE_IPV6
			if (segment_ip_cache[dbid] == NULL && addrs->ai_family == AF_INET6)
				elog(ERROR,"host %s only has IPv6 address, not yet supported",pRow->hostname);
#endif
			
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		}
		
		pRow->hostip = pstrdup(segment_ip_cache[dbid]);

		
	}

	/*
	 * Validate that there exists at least one entry and one segment database in the configuration
	 */
	if (component_databases->total_segment_dbs == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Greenplum Database number of segment databases cannot be 0"
					 )));
	}
	if (component_databases->total_entry_dbs == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Greenplum Database number of entry databases cannot be 0"
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


	if (GpIdentity.numsegments <= 0)	/* gp_id not available or not read in yet */
	{
		GpIdentity.numsegments = component_databases->total_segments;
		GpIdentity.segindex = -1;
		GpIdentity.dbid = 1;
	}

	/*
	 *	Validate that gp_numsegments == segment_databases->total_segment_dbs
	 */
	if (getgpsegmentCount() != component_databases->total_segments)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("Greenplum Database number of segments inconsistency: count is %d from pg_catalog.%s table, but %d from getCdbComponentDatabases()",
						getgpsegmentCount(),  GpIdRelationName, component_databases->total_segments)));
	}

	/*
	 * Now validate that our identity is present in the entry databases
	 */
	for (i = 0; i < component_databases->total_entry_dbs; i++)
	{
		CdbComponentDatabaseInfo *pInfo = &component_databases->entry_db_info[i];

		if (pInfo->dbid == GpIdentity.dbid && pInfo->segindex == Gp_segment)
		{
			break;
		}
	}
	if (i == component_databases->total_entry_dbs)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("Cannot locate entry database represented by gp_id row in gp_configuration: dbid %d content %d",
						GpIdentity.dbid, Gp_segment)));
	}

	/*
	 * Now validate that the segindexes for the segment databases are between 0 and GpIdentity.numsegments-1 inclusive,
	 * and that we hit them all. Since it's sorted, this is relatively easy.
	 */
	x = 0;
	for (i = 0; i < getgpsegmentCount(); i++)
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
						 errmsg("Content values not valid in gp_configuration table.  They must be in the range 0 to %d inclusive",
								getgpsegmentCount() - 1)));
			}
		}
		if (this_segindex != i)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("Content values not valid in gp_configuration table.  They must be in the range 0 to %d inclusive",
							getgpsegmentCount() - 1)));
		}
	}

	/*
	 * Cleanup invoves ending the heap scan of gp_configuration, and closing all the relations we opened.
	 */
	heap_endscan(gp_configuration_scan);

	heap_close(gp_configuration_rel, AccessShareLock);


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
	int	i;

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
 * performs all necessary setup required for Greenplum Database mode.
 *
 * This includes cdblink_setup() and initializing the Motion Layer.
 */
void
cdb_setup(void)
{
	elog(DEBUG1, "Initializing Greenplum components...");

	cdblink_setup();

	/* If gp_role is UTILITY, skip this call. */
	if (Gp_role != GP_ROLE_UTILITY  && Gp_role != GP_ROLE_DISPATCHAGENT)
	{
		/* Initialize the Motion Layer IPC subsystem. */
		InitMotionLayerIPC();
	}

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		/* check mirrored entry db configuration */
		buildMirrorQDDefinition();

		if (isQDMirroringPendingCatchup())
		{
			/* 
			 * Do a checkpoint to cause a checkpoint xlog record to
			 * to be written and force the QD mirror to get
			 * synchronized.
			 */
			RequestCheckpoint(true, false);
		}
		
		/* initialize TM */
		initTM();
	}
}


/*
 * performs all necessary cleanup required when leaving Greenplum Database mode.  This is also
 * called when the process exits.
 *
 * NOTE: the arguments to this function are here only so that we can
 *		 register it with on_proc_exit().  These parameters should not be
 *		 used since there are some callers to this that pass them as NULL.
 *
 */
void
cdb_cleanup(int code __attribute__((unused)) , Datum arg __attribute__((unused)) )
{
	elog(DEBUG1, "Cleaning up Greenplum components...");

	disconnectAndDestroyAllGangs();

	disconnectMirrorQD();

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		if (cdb_total_plans > 0)
			elog(DEBUG1, "session dispatched %d plans %d slices (%f), largest plan %d",
				 cdb_total_plans, cdb_total_slices,
				 ((double)cdb_total_slices/(double)cdb_total_plans),
				 cdb_max_slices);
	}

	if (Gp_role != GP_ROLE_UTILITY && Gp_role != GP_ROLE_DISPATCHAGENT)
	{
		/* shutdown our listener socket */
		CleanUpMotionLayerIPC();
	}
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

/*
 * Given total number of primary segment databases and a number of segments
 * to "skip" - this routine creates a boolean map (array) the size of total
 * number of segments and randomly selects several entries (total number of
 * total_to_skip) to be marked as "skipped". This is used for external tables
 * with the 'gpfdist' protocol where we want to get a number of *random* segdbs
 * to connect to a gpfdist client.
 *
 * Caller of this function should pfree skip_map when done with it.
 */
bool *makeRandomSegMap(int total_primaries, int total_to_skip)
{
	int			randint;     /* some random int representing a seg    */
	int			skipped = 0; /* num segs already marked to be skipped */
	bool		*skip_map;
	
	skip_map = (bool *) palloc(total_primaries * sizeof(bool));
	MemSet(skip_map, false, total_primaries * sizeof(bool));
	
	while(total_to_skip != skipped)
	{
		/*
		 * create a random int between 0 and (total_primaries - 1).
		 * 
		 * NOTE that the lower and upper limits in geqo_randint() 
		 * are inclusive so we take them into account. In reality
		 * the chance of those limits to get selected by the random
		 * generator is extremely small, so we may want to find a 
		 * better random generator some time (not critical though).
		 */
		randint = geqo_randint(0, total_primaries - 1);
		
		/*
		 * mark this random index 'true' in the skip map (marked to 
		 * be skipped) unless it was already marked.
		 */
		if(skip_map[randint] == false)
		{
			skip_map[randint] = true;
			skipped++;
		}
		
	}	
	
	return skip_map;
}
