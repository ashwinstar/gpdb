/*-------------------------------------------------------------------------
 *
 * cdbutil.c
 *	  Internal utility support functions for Greenplum Database/PostgreSQL.
 *
 * Copyright (c) 2005-2008, Greenplum inc
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
#include "nodes/execnodes.h"	/* Slice, SliceTable */
#include "cdb/cdbmotion.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbdisp.h"
#include "cdb/ml_ipc.h"			/* listener_setup */
#include "cdb/cdbtm.h"
#include "cdb/cdbfts.h"
#include "libpq/ip.h"
#include "postmaster/bgwriter.h"
#include "catalog/indexing.h"
#include "utils/faultinjection.h"

/*
 * Helper Functions
 */
static int	CdbComponentDatabaseInfoCompare(const void *p1, const void *p2);
static void freeCdbComponentDatabaseInfo(CdbComponentDatabaseInfo *cdi);

static void getAddressesForDBid(CdbComponentDatabaseInfo *c, int elevel);

static HTAB *segment_ip_cache_htab = NULL;

struct segment_ip_cache_entry {
	char key[NAMEDATALEN];
	char hostinfo[NI_MAXHOST];
};

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
				 errmsg("Cannot have a Gp_role of dispatch when connected to a segment database.")));
	}

	if (Gp_role != GP_ROLE_UTILITY && GpIdentity.segindex != Gp_segment)
	{
		if (Gp_segment < -1)
		{
			Gp_segment = GpIdentity.segindex;
			ereport(WARNING,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("gp_id segindex not same as Gp_segment.")));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					 errmsg("gp_id segindex not same as Gp_segment.")));
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

	if (Gp_role == GP_ROLE_DISPATCH && segment_ip_cache_htab != NULL)
	{
		hash_destroy(segment_ip_cache_htab);
		segment_ip_cache_htab = NULL;
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
getCdbComponentInfo(bool DNSLookupAsError)
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
	 * of 128, and component_databases->entry_db_info with an initial
	 * size of 4.  If necessary during row fetching, we grow these by
	 * doubling each time we run out.
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
		 * Grab the fields that we need from gp_configuration.  We do
		 * this first, because until we read them, we don't know
		 * whether this is an entry database row or a segment database
		 * row.
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

#ifdef USE_FAULT_INJECTION
		if (gp_fault_inject_segment_failure && gp_fault_inject_segment_failure_segment_id == pRow->segindex)
		{
			pRow->valid = false;
			elog(LOG, "Insert a failure to segment %d", gp_fault_inject_segment_failure_segment_id);
		}
#endif

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


		getAddressesForDBid(pRow, DNSLookupAsError ? ERROR : LOG);
		pRow->hostip = pRow->hostaddrs[0];
	}

	/*
	 * Validate that there exists at least one entry and one segment database in the configuration
	 */
	if (component_databases->total_segment_dbs == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Greenplum Database number of segment databases cannot be 0")));
	}
	if (component_databases->total_entry_dbs == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("Greenplum Database number of entry databases cannot be 0")));
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
			(component_databases->segment_db_info[i].segindex != component_databases->segment_db_info[i - 1].segindex))
		{
			component_databases->total_segments++;
		}
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
	 * Now validate that the segindexes for the segment databases are
	 * between 0 and GpIdentity.numsegments-1 inclusive, and that we
	 * hit them all. Since it's sorted, this is relatively easy.
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
	return applyTransientState(getCdbComponentInfo(true));
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

		/*
		 * This call will generate a warning if master mirroring
		 * is not synchronized.
		 */
		QDMirroringWriteCheck();
		
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

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		if (cdb_total_plans > 0)
		{
			elog(DEBUG1, "session dispatched %d plans %d slices (%f), largest plan %d",
				 cdb_total_plans, cdb_total_slices,
				 ((double)cdb_total_slices/(double)cdb_total_plans),
				 cdb_max_slices);
		}
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
 * We're going to sort interface-ids by priority.  So we need a little
 * struct, and a comparison function to hand-off to qsort().
 */
struct priority_iface {
	int priority;
	int interface_id;
};

/*
 * iface_priority_compare() A compare function for interface-priority
 * structs.  for use with qsort.
 */
static int
iface_priority_compare(const void *p1, const void *p2)
{
	const struct priority_iface *obj1 = (struct priority_iface *) p1;
	const struct priority_iface *obj2 = (struct priority_iface *) p2;

	return (obj1->priority - obj2->priority);
}

/*
 * Maintain a cache of names.
 *
 * The keys are all NAMEDATALEN long.
 */
static char *
getDnsCachedAddress(char *name, int port, int elevel)
{
	struct segment_ip_cache_entry *e;

	if (segment_ip_cache_htab == NULL)
	{
		HASHCTL		hash_ctl;

		MemSet(&hash_ctl, 0, sizeof(hash_ctl));

		hash_ctl.keysize = NAMEDATALEN + 1;
		hash_ctl.entrysize = sizeof(struct segment_ip_cache_entry);

		segment_ip_cache_htab = hash_create("segment_dns_cache",
											256,
											&hash_ctl,
											HASH_ELEM);
		Assert(segment_ip_cache_htab != NULL);
	}

	e = (struct segment_ip_cache_entry *)hash_search(segment_ip_cache_htab,
													 name, HASH_FIND, NULL);

	/* not in our cache, we've got to actually do the name lookup. */
	if (e == NULL)
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
			
		snprintf(portNumberStr, sizeof(portNumberStr), "%d", port);
		service = portNumberStr;
				
		ret = pg_getaddrinfo_all(name, service, &hint, &addrs);
		if (ret || !addrs)
		{
			if (addrs)
				pg_freeaddrinfo_all(hint.ai_family, addrs);

			ereport(elevel,
					(errmsg("could not translate host name \"%s\", port \"%d\" to address: %s",
							name, port, gai_strerror(ret))));

			return NULL;
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

				/* INSERT INTO OUR CACHE HTAB HERE */

				e = (struct segment_ip_cache_entry *)hash_search(segment_ip_cache_htab,
																 name,
																 HASH_ENTER,
																 NULL);
				Assert(e != NULL);
				memcpy(e->hostinfo, hostinfo, sizeof(hostinfo));

				break;
			}
		}
			
		MemoryContextSwitchTo(oldContext);
			
		pg_freeaddrinfo_all(hint.ai_family, addrs);
	}

	/* return a pointer to our cache. */
	return e->hostinfo;
}

/*
 * Given a component-db in the system, find the addresses at which it
 * can be reached, appropriately populate the argument-structure, and
 * maintain the ip-lookup-cache.
 *
 * We get all of the interface-ids, sort them in priority order, then
 * go get their details ... and then make sure they're cached properly.
 */
static void
getAddressesForDBid(CdbComponentDatabaseInfo *c, int elevel)
{
	Relation	gp_db_interface_rel;
	Relation	gp_interface_rel;
	HeapTuple	tuple;
	ScanKeyData	key;
	SysScanDesc	dbscan, ifacescan;

	int			j, i=0;
	struct priority_iface *ifaces=NULL;
	int			iface_count, iface_max=0;

	Datum		attr;
	bool		isNull;

	int			dbid;
	int			iface_id;
	int			priority;

	char		*name;

	Assert(c != NULL);
		
	gp_db_interface_rel = heap_open(GpDbInterfacesRelationId, AccessShareLock);

	ScanKeyInit(&key, Anum_gp_db_interfaces_dbid,
				BTEqualStrategyNumber, F_INT2EQ,
				ObjectIdGetDatum(c->dbid));

	dbscan = systable_beginscan(gp_db_interface_rel, GpDbInterfacesIndexId,
								true, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(dbscan)))
	{
		i++;
		if (i > iface_max)
		{
			/* allocate 8-more slots */
			if (ifaces == NULL)
				ifaces = palloc((iface_max + 8) * sizeof(struct priority_iface));
			else
				ifaces = repalloc(ifaces, (iface_max + 8) * sizeof(struct priority_iface));

			memset(ifaces + iface_max, 0, 8 * sizeof(struct priority_iface));
			iface_max += 8;
		}

		/* dbid is for sanity-check on scan condition only */
		attr = heap_getattr(tuple, Anum_gp_db_interfaces_dbid, gp_db_interface_rel->rd_att, &isNull);
		Assert(!isNull);
		dbid = DatumGetInt16(attr);
		Assert(dbid == c->dbid);

		attr = heap_getattr(tuple, Anum_gp_db_interfaces_interfaceid, gp_db_interface_rel->rd_att, &isNull);
		Assert(!isNull);
		iface_id = DatumGetInt16(attr);

		attr = heap_getattr(tuple, Anum_gp_db_interfaces_priority, gp_db_interface_rel->rd_att, &isNull);
		Assert(!isNull);
		priority = DatumGetInt16(attr);

		ifaces[i-1].priority = priority;
		ifaces[i-1].interface_id = iface_id;
	}
	iface_count = i;

	/* Finish up scan and close appendonly catalog. */
	systable_endscan(dbscan);

	heap_close(gp_db_interface_rel, AccessShareLock);

	/* we now have the unsorted list, or an empty list. */
	do
	{
		/* fallback to using hostname if our list is empty */
		if (iface_count == 0)
			break;

		qsort(ifaces, iface_count, sizeof(struct priority_iface), iface_priority_compare);

		/* we now have interfaces, sorted by priority. */

		gp_interface_rel = heap_open(GpInterfacesRelationId, AccessShareLock);

		j=0;
		for (i=0; i < iface_count; i++)
		{
			int status=0;

			/* Start a new scan. */
			ScanKeyInit(&key, Anum_gp_interfaces_interfaceid,
						BTEqualStrategyNumber, F_INT2EQ,
						ObjectIdGetDatum(ifaces[i].interface_id));

			ifacescan = systable_beginscan(gp_interface_rel, GpInterfacesIndexId,
										   true, SnapshotNow, 1, &key);

			tuple = systable_getnext(ifacescan);

			Assert(HeapTupleIsValid(tuple));

			/* iface_id is for sanity-check on scan condition only */
			attr = heap_getattr(tuple, Anum_gp_interfaces_interfaceid, gp_interface_rel->rd_att, &isNull);
			Assert(!isNull);
			iface_id = DatumGetInt16(attr);
			Assert(iface_id == ifaces[i].interface_id); 

			attr = heap_getattr(tuple, Anum_gp_interfaces_status, gp_interface_rel->rd_att, &isNull);
			Assert(!isNull);
			status = DatumGetInt16(attr);

			/* if the status is "alive" use the interface. */
			if (status == 1)
			{
				attr = heap_getattr(tuple, Anum_gp_interfaces_address, gp_interface_rel->rd_att, &isNull);
				Assert(!isNull);
				name = getDnsCachedAddress(DatumGetCString(attr), c->port, elevel);
				if (name)
					c->hostaddrs[j++] = pstrdup(name);
			}

			systable_endscan(ifacescan);
		}

		heap_close(gp_interface_rel, AccessShareLock);

		/* fallback to using hostname if our list is empty */
		if (j == 0)
			break;

		/* successfully retrieved at least one entry. */

		return;
	}
	while (0);

	/* fallback to using hostname */
	memset(c->hostaddrs, 0, COMPONENT_DBS_MAX_ADDRS * sizeof(char *));

	/* add an entry, using the hostname as fallback. */
	name = getDnsCachedAddress(c->hostname, c->port, elevel);

	if (name)
		c->hostaddrs[0] = pstrdup(name);
	else
		c->hostaddrs[0] = NULL;

	return;
}

/*
 * Given total number of primary segment databases and a number of
 * segments to "skip" - this routine creates a boolean map (array) the
 * size of total number of segments and randomly selects several
 * entries (total number of total_to_skip) to be marked as
 * "skipped". This is used for external tables with the 'gpfdist'
 * protocol where we want to get a number of *random* segdbs to
 * connect to a gpfdist client.
 *
 * Caller of this function should pfree skip_map when done with it.
 */
bool *
makeRandomSegMap(int total_primaries, int total_to_skip)
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
		 * NOTE that the lower and upper limits in geqo_randint() are
		 * inclusive so we take them into account. In reality the
		 * chance of those limits to get selected by the random
		 * generator is extremely small, so we may want to find a
		 * better random generator some time (not critical though).
		 */
		randint = geqo_randint(0, total_primaries - 1);
		
		/*
		 * mark this random index 'true' in the skip map (marked to be
		 * skipped) unless it was already marked.
		 */
		if(skip_map[randint] == false)
		{
			skip_map[randint] = true;
			skipped++;
		}
	}	
	
	return skip_map;
}
