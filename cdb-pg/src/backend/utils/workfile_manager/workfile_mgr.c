/*-------------------------------------------------------------------------
 *
 * workfile_mgr.c
 *	 Implementation of workfile manager and workfile caching.
 *
 * Copyright (c) 2011, EMC Corp.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <unistd.h>
#include <sys/stat.h>
#include "utils/workfile_mgr.h"
#include "miscadmin.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbsrlz.h"
#include "nodes/print.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "postmaster/primary_mirror_mode.h"
#include "libpq/libpq.h"

#include "utils/lsyscache.h"
#include "catalog/pg_proc.h"

#define WORKFILE_SET_MASK  "XXXXXXXXXX"

/* Type of temp file to use for storing the plan */
#define WORKFILE_PLAN_FILE_TYPE BUFFILE

/* Maximum number of workfile sets in the cache */
#define WORKFILE_CACHE_MAX_SIZE 8192

/* Forward declarations */
static workfile_set *workfile_mgr_lookup_set(PlanState *ps);
static bool workfile_mgr_is_reusable(PlanState *ps);
static bool workfile_mgr_is_volatile_plan(PlanState *ps);
static workfile_set_hashkey_t workfile_mgr_hash_key(workfile_set_plan *plan);
static workfile_set_plan *workfile_mgr_serialize_plan(PlanState *ps);
static void workfile_mgr_free_plan(workfile_set_plan *splan);
static void workfile_mgr_save_plan(workfile_set *work_set, workfile_set_plan *sf_plan);
static bool workfile_set_equivalent(const void *virtual_resource, const void *physical_resource);
static bool workfile_mgr_compare_plan(workfile_set *work_set, workfile_set_plan *sf_plan);
static void workfile_mgr_cleanup_set(const void *resource);
static void workfile_mgr_delete_set_directory(char *workset_path);
static void workfile_mgr_unlink_directory(const char *dirpath);
static char * get_name_from_nodeType(const NodeTag node_type);

/* Workfile manager cache is stored here, once attached to */
Cache *workfile_mgr_cache = NULL;

/*
 * Initialize the cache in shared memory, or attach to an existing one
 *
 */
void
workfile_mgr_cache_init(void)
{
	CacheCtl cacheCtl;
	MemSet(&cacheCtl, 0, sizeof(CacheCtl));

	cacheCtl.maxSize = WORKFILE_CACHE_MAX_SIZE;
	cacheCtl.cacheName = "Workfile Manager Cache";
	cacheCtl.entrySize = sizeof(workfile_set);
	cacheCtl.keySize = sizeof(((workfile_set *)0)->key);
	cacheCtl.keyOffset = GPDB_OFFSET(workfile_set, key);

	cacheCtl.hash = int32_hash;
	cacheCtl.keyCopy = (HashCopyFunc) memcpy;
	cacheCtl.match = (HashCompareFunc) memcmp;
	cacheCtl.equivalentEntries = workfile_set_equivalent;
	cacheCtl.cleanupEntry = workfile_mgr_cleanup_set;

	cacheCtl.baseLWLockId = FirstWorkfileMgrLock;
	cacheCtl.numPartitions = NUM_WORKFILEMGR_PARTITIONS;

	workfile_mgr_cache = Cache_Create(&cacheCtl);
	Assert(NULL != workfile_mgr_cache);
}

/*
 * Returns pointer to the workfile manager cache
 */
Cache *
workfile_mgr_get_cache(void)
{
	Assert(NULL != workfile_mgr_cache);
	return workfile_mgr_cache;
}

/*
 * compute the size of shared memory for the workfile manager
 */
Size
workfile_mgr_shmem_size(void)
{
	return Cache_SharedMemSize(WORKFILE_CACHE_MAX_SIZE, sizeof(workfile_set));
}


/*
 *  Retrieves the name of the file_no file in the workset. If the file was not
 * 	already in the work_set, it is added.
 * 	This can create gaps in the work_set if file_no > work_set->no_files.
 *
 *  workfile_name is allocated by the caller. It will contain the name for
 *  the new file.
 */
void
workfile_mgr_retrieve_file_no(workfile_set *work_set, uint32 file_no, char *workfile_name, uint32 workfile_name_len)
{
	Assert(work_set);
	Assert(workfile_name);

	if (file_no >= work_set->no_files)
	{
		/* Retrieving file beyond the end of the set. Adjusting the size of the set */
		work_set->no_files = file_no + 1;

	}
	snprintf(workfile_name, workfile_name_len,
			"%s/spillfile_f%u", work_set->path, file_no);
}

/*
 *   Creates a new file in a spill file set.
 *   workfile_name is allocated by the caller. It will contain the name for
 *   the new file.
 */
void
workfile_mgr_create_file(workfile_set *work_set, char *workfile_name, uint32 workfile_name_len)
{
	Assert(work_set);
	Assert(workfile_name);
	work_set->no_files++;
	workfile_mgr_retrieve_file_no(work_set, work_set->no_files - 1, workfile_name, workfile_name_len);
}

/*
 * Retrieves the operator name.
 * Result is palloc-ed in the current memory context.
 */
static char *
get_name_from_nodeType(const NodeTag node_type)
{
	char *operator_name = (char *) palloc(MAXPGPATH);

	switch ( node_type )
	{
		case T_AggState:
			snprintf(operator_name, MAXPGPATH,"Agg");
			break;
		case T_HashJoinState:
			snprintf(operator_name, MAXPGPATH,"HashJoin");
			break;
		case T_MaterialState:
			snprintf(operator_name, MAXPGPATH,"Material");
			break;
		case T_SortState:
			snprintf(operator_name, MAXPGPATH,"Sort");
			break;
		default:
			Assert(false && "Operator not implemented");
	}

	return operator_name;
}


/*
 * Create a new file set
 *   type is the WorkFileType for the files: BUFFILE or BFZ
 *   can_be_reused: if set to false, then we don't insert this set into the cache,
 *     since the caller is telling us there is no point. This can happen for
 *     example when spilling during index creation.
 *   plan is the subplan for the operator
 *   snapshot contains snapshot information for the current transaction
 *
 */
workfile_set *
workfile_mgr_create_set(enum ExecWorkFileType type, bool can_be_reused, PlanState *ps, workfile_set_snapshot snapshot)
{
	Assert(NULL != ps);
	Assert(NULL != workfile_mgr_cache);
	Assert(NULL != MyProcPort);

	workfile_set_plan *s_plan = NULL;
	Plan *plan = ps->plan;

	AssertImply(can_be_reused, plan != NULL);

	CacheEntry *newEntry = Cache_AcquireEntry(workfile_mgr_cache);
	Assert(NULL != newEntry);
	workfile_set *work_set = CACHE_ENTRY_PAYLOAD(newEntry);
	Assert(work_set != NULL);

	work_set->complete = false;
	work_set->no_files = 0;
	work_set->next = NULL;
	work_set->metadata.type = type;
	work_set->metadata.bfz_compress_type = gp_workfile_compress_algorithm;
	work_set->metadata.snapshot = snapshot;
	work_set->metadata.num_leaf_files = 0;
	work_set->on_disk = false;
	work_set->set_plan = NULL;
	work_set->node_type = ps->type;
	work_set->slice_id = currentSliceId;
	work_set->session_id = gp_session_id;
	work_set->command_count = gp_command_count;
	work_set->session_start_time = GetCurrentTimestamp();
	memcpy(work_set->database_name, MyProcPort->database_name, NAMEDATALEN-1);
	work_set->database_name[NAMEDATALEN-1]='\0';
	memcpy(work_set->user_name, MyProcPort->user_name, NAMEDATALEN-1);
	work_set->user_name[NAMEDATALEN-1]='\0';


	/*
	 * Determine operatorMemKB for this operator.
	 * For HashJoin, this is given by the right child, for everyone else it is the actual node.
	 */
	if ((IsA(ps, HashJoinState)))
	{
		Assert(IsA(ps->righttree, HashState));
		work_set->metadata.operator_work_mem = ps->righttree->plan->operatorMemKB;
	}
	else
	{
		work_set->metadata.operator_work_mem = plan->operatorMemKB;
	}

	/* If workfile caching is disabled, nothing should be re-used, so override whatever the caller says */
	work_set->can_be_reused = gp_workfile_caching && can_be_reused && workfile_mgr_is_reusable(ps);

	/* Create base directory here. We need database relative path */
	char *tmp_dirpath = (char*)palloc(MAXPGPATH);
	if (snprintf(tmp_dirpath, MAXPGPATH, "%s/%s", getCurrentTempFilePath, PG_TEMP_FILES_DIR) > MAXPGPATH)
	{
		ereport(ERROR, (errmsg("cannot generate path %s/%s",
				getCurrentTempFilePath,
				PG_TEMP_FILES_DIR)));
	}
	mkdir(tmp_dirpath, S_IRWXU);
	pfree(tmp_dirpath);

	/* Create workset directory here */
	char *workset_path_masked = (char *) palloc(MAXPGPATH);
	char *operator_name = get_name_from_nodeType(work_set->node_type);

	if (snprintf(workset_path_masked, MAXPGPATH,
			"%s/%s/%s_%s_Slice%d.%s",
			getCurrentTempFilePath,
			PG_TEMP_FILES_DIR,
			WORKFILE_SET_PREFIX,
			operator_name,
			work_set->slice_id,
			WORKFILE_SET_MASK) > MAXPGPATH)
	{
		ereport(ERROR, (errmsg("cannot generate path %s/%s/%s_%s_Slice%d.%s",
			getCurrentTempFilePath,
			PG_TEMP_FILES_DIR,
			WORKFILE_SET_PREFIX,
			operator_name,
			work_set->slice_id,
			WORKFILE_SET_MASK)));
	}

	char *workset_path_unmasked = mkdtemp(workset_path_masked);
	if (workset_path_unmasked == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				errmsg("could not create spill file directory: %m")));
	}

	/* Initialize workset path. Strip prefix from path since bfz/fd add the getCurrentTempFilePath to it */
	strncpy(work_set->path,
			workset_path_unmasked + strlen(getCurrentTempFilePath) + 1,
			MAXPGPATH);

	/* Path is now valid. Mark this work_set as physical now */
	work_set->on_disk = true;

	pfree(workset_path_masked);
	pfree(operator_name);

	if (work_set->can_be_reused)
	{
		Assert(plan != NULL);
		Assert(nodeTag(plan) >= T_Plan && nodeTag(plan) < T_PlanInvalItem);

		s_plan = workfile_mgr_serialize_plan(ps);
		work_set->key = workfile_mgr_hash_key(s_plan);
		workfile_mgr_save_plan(work_set, s_plan);
		workfile_mgr_free_plan(s_plan);
	}

	elog(gp_workfile_caching_loglevel, "new spill file set. key=0x%x can_be_reused=%d prefix=%s opMemKB=" INT64_FORMAT,
			work_set->key, work_set->can_be_reused, work_set->path, work_set->metadata.operator_work_mem);

	return work_set;
}

/*
 * Look up file set in cache given a certain plan. Check if it's usable
 * for the current query. If not found, or not re-usable, return NULL.
 */
workfile_set *
workfile_mgr_find_set(PlanState *ps)
{
	Assert(NULL != ps);

	/* Check if there is any point in looking this up in the cache. Some subplans are just not reusable */
	if (!workfile_mgr_is_reusable(ps))
	{
		return NULL;
	}

	/* Look up plan in cache */
	workfile_set *work_set = workfile_mgr_lookup_set(ps);
	if (work_set == NULL)
	{
		return NULL;
	}

	/* Check to see if we can reuse what we found */
	if (workfile_mgr_can_reuse(work_set, ps))
	{
		return work_set;
	}
	else
	{
		elog(gp_workfile_caching_loglevel, "Found matching work_set but cannot reuse");
		return NULL;
	}
}

/*
 * Check to see if a plan is can ever generate reusable workfiles.
 * For example, we cannot store workfiles for queries containing parameters
 * or external table scans.
 */
static bool
workfile_mgr_is_reusable(PlanState *ps)
{
	Assert(NULL != ps);

	Plan *plan = ps->plan;

	/* Don't allow caching of workfiles for parameterized queries */
	bool parameterized = (plan->allParam != NULL) || (plan->extParam != NULL);
	if (parameterized)
	{
		elog(gp_workfile_caching_loglevel, "Parameterized plan not considered for workfile caching");
		return false;
	}

	bool volatile_fns = workfile_mgr_is_volatile_plan(ps);
	if (volatile_fns)
	{
		elog(gp_workfile_caching_loglevel, "Plan using volatile functions not considered for workfile caching");
		return false;
	}

	return true;
}

/*
 * Walker function to test if a plan node uses volatile function or external
 * table scan
 *
 * Returns CdbVisit_Failure if it finds an offending node
 */
static CdbVisitOpt
checkVolatileNodeWalker(PlanState *ps,
				  void *context)
{
	if (IsA(ps, FunctionScanState))
	{

		/* Get the Oid of the function */
		Expr *fn_expr = ((FunctionScanState *) ps)->funcexpr->expr;
		Assert(IsA(fn_expr, FuncExpr));
		Oid fn_oid = ((FuncExpr *) fn_expr)->funcid;

		/* Check provolatile column for function */
		char fn_provolatile = func_volatile(fn_oid);

		if (fn_provolatile != PROVOLATILE_IMMUTABLE)
		{
			Assert(fn_provolatile == PROVOLATILE_STABLE || fn_provolatile == PROVOLATILE_VOLATILE);
			return CdbVisit_Failure;
		}
	}

	if (IsA(ps, TableFunctionState))
	{
		Oid fn_oid = ((TableFunctionState *) ps)->fcache->func.fn_oid;

		/* Check provolatile column for function */
		char fn_provolatile = func_volatile(fn_oid);

		if (fn_provolatile != PROVOLATILE_IMMUTABLE)
		{
			Assert(fn_provolatile == PROVOLATILE_STABLE || fn_provolatile == PROVOLATILE_VOLATILE);
			return CdbVisit_Failure;
		}

	}

	if (IsA(ps, ExternalScanState))
	{
		return CdbVisit_Failure;
	}

	/* Continue walk */
	return CdbVisit_Walk;
}

/*
 * Check if a plan contains any volatile functions or external table scans
 */
static bool
workfile_mgr_is_volatile_plan(PlanState *ps)
{
	Assert(NULL != ps);

	CdbVisitOpt status = planstate_walk_node(ps, checkVolatileNodeWalker, NULL);

	/* status == CdbVisit_Failure means we found a volatile node */
	return (status == CdbVisit_Failure);
}

/*
 *  Look up file set the cache given a certain PlanState.
 *  Return NULL if not found.
 */
static workfile_set *
workfile_mgr_lookup_set(PlanState *ps)
{
	Assert(NULL != ps);
	Assert(NULL != workfile_mgr_cache);
	Assert(NULL != ps->plan);
	Assert(nodeTag(ps->plan) >= T_Plan && nodeTag(ps->plan) < T_PlanInvalItem);

	workfile_set_plan *s_plan = workfile_mgr_serialize_plan(ps);
	Assert(s_plan != NULL);

	CacheEntry *localEntry = Cache_AcquireEntry(workfile_mgr_cache);
	workfile_set *local_work_set = (workfile_set *) CACHE_ENTRY_PAYLOAD(localEntry);

	/* This is not a physical workset, it's only used for look-ups */
	local_work_set->on_disk = false;
	local_work_set->set_plan = s_plan;
	local_work_set->key = workfile_mgr_hash_key(s_plan);

	CacheEntry *cachedEntry = Cache_Lookup(workfile_mgr_cache, localEntry);

	/* Release local entry and free up plan memory. We don't need it anymore */
	Cache_SurrenderEntry(workfile_mgr_cache, localEntry);

	workfile_set *work_set = NULL;
	if (NULL != cachedEntry)
	{
		work_set = (workfile_set *) CACHE_ENTRY_PAYLOAD(cachedEntry);
	}

	return work_set;
}

/*
 * Thoroughly checks if an existing workfile_set can be used for the current
 * subplan.
 */
bool
workfile_mgr_can_reuse(workfile_set *work_set, PlanState *ps)
{
	Assert(NULL != work_set);
	Assert(NULL != ps);

	uint64 operatorMemKB = 0;

	if (IsA(ps, HashJoinState))
	{
		/* For HashJoin, we need to look at the HashNode operator_work_mem, because that's the memory intensive operator */
		Assert(IsA(ps->righttree, HashState));
		operatorMemKB = ps->righttree->plan->operatorMemKB;
	}
	else
	{
		operatorMemKB = ps->plan->operatorMemKB;
	}

	Assert(operatorMemKB > 0);
	if (operatorMemKB < work_set->metadata.operator_work_mem)
	{
		return false;
	}

	return true;
}

/*
 * Clears entire workfile cache.
 */
Datum
gp_workfile_manager_clear_cache(PG_FUNCTION_ARGS)
{
	Assert(false && "Clear cache not implemented");
	PG_RETURN_BOOL(true);
}

/*
 * Physically delete a spill set. Path must not include database prefix.
 */
static void
workfile_mgr_delete_set_directory(char *workset_path)
{
	/* Add filespace prefix to path */
	char *reldirpath = (char*)palloc(PATH_MAX);
	if (snprintf(reldirpath, PATH_MAX, "%s/%s", getCurrentTempFilePath, workset_path) > PATH_MAX)
	{
		ereport(ERROR, (errmsg("cannot generate path %s/%s", getCurrentTempFilePath,
                        workset_path)));
	}

	Assert(reldirpath != NULL);

	workfile_mgr_unlink_directory(reldirpath);
	pfree(reldirpath);
}

/*
 * Physically delete a spill file set. Path is assumed to be database relative.
 */
static void
workfile_mgr_unlink_directory(const char *dirpath)
{

	elog(gp_workfile_caching_loglevel, "deleting spill file set directory %s", dirpath);

	int res = rmtree(dirpath,true);

	if (!res)
	{
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				errmsg("could not remove spill file directory")));
	}

}

/*
 * Workfile-manager specific function to clean up before releasing a
 * workfile set from the cache.
 *
 */
static void
workfile_mgr_cleanup_set(const void *resource)
{
	workfile_set *work_set = (workfile_set *) resource;

	if (work_set->on_disk)
	{
		elog(gp_workfile_caching_loglevel, "workfile mgr cleanup deleting set: key=0x%0xd path=%s", work_set->key, work_set->path);
		Assert(NULL == work_set->set_plan);
		workfile_mgr_delete_set_directory(work_set->path);
	}
	else
	{
		/* Non-physical workfile set, we need to free up the plan memory */
		if (NULL != work_set->set_plan)
		{
			pfree(work_set->set_plan->serialized_plan);
			pfree(work_set->set_plan);
		}
	}
}

/*
 * Close a spill file set. If we're planning to re-use it, insert it in the
 * cache. If not, let the cleanup routine delete the files and free up memory.
 */
void
workfile_mgr_close_set(workfile_set *work_set)
{
	Assert(work_set!=NULL);

	elog(gp_workfile_caching_loglevel, "closing workfile set: can_be_reused=%d complete=%d location: %s",
		 work_set->can_be_reused, work_set->complete, work_set->path);

	CacheEntry *cache_entry = CACHE_ENTRY_HEADER(work_set);

	if (Cache_IsCached(cache_entry))
	{
		/* Workset came from cache. Just release it, nothing to do */
		Cache_Release(workfile_mgr_cache, cache_entry);
		return;
	}

	if (work_set->complete && work_set->can_be_reused)
	{
		/* We want to keep this one around. Insert into cache */
		Cache_Insert(workfile_mgr_cache, cache_entry);
		Cache_Release(workfile_mgr_cache, cache_entry);
		return;
	}


	Assert(!work_set->complete || !work_set->can_be_reused);
	/*
	 * Fall-through case:
	 * We need to delete this work_set, as it's not reusable. Leave it registered and
	 * don't do anything here. We don't know if the files are all closed, so wait
	 * until EOXact to actually remove the files part of clean-up.
	 */
}

/*
 * Mark a workfile_set as complete. This means it should be cached upon closing,
 * as it can be re-used.
 * If the operator is canceled or fails after this, the workfile set can
 * still be re-used.
 * This should be done after all the data has been flushed to disk.
 * After this point, assume anyone can read and re-use this set.
 */

void workfile_mgr_mark_complete(workfile_set *work_set)
{
	Assert(work_set != NULL);
	Assert(!work_set->complete);

	work_set->complete = true;
}

/*
 * Deletes all directories containing workfile sets under tmpdirname
 */
void
workfile_mgr_cleanup_files(const char *tmpdirname)
{
	DIR	*temp_dir = AllocateDir(tmpdirname);
	if (temp_dir == NULL)
	{
		/* anything except ENOENT is fishy */
		if (errno != ENOENT)
			elog(LOG,
				 "could not open temporary-files directory \"%s\": %m",
				 tmpdirname);
		return;
	}

	struct dirent *temp_de;
	while ((temp_de = ReadDir(temp_dir, tmpdirname)) != NULL)
	{

		if (strcmp(temp_de->d_name, ".") == 0 ||
			strcmp(temp_de->d_name, "..") == 0)
		{
			continue;
		}

		if (strncmp(temp_de->d_name, WORKFILE_SET_PREFIX, strlen(WORKFILE_SET_PREFIX)) == 0)
		{
			char dirbuf[MAXPGPATH];
			snprintf(dirbuf, MAXPGPATH, "%s/%s", tmpdirname, temp_de->d_name);
			workfile_mgr_unlink_directory(dirbuf);
		}
	}

	FreeDir(temp_dir);
}

/*
 * This function is called at transaction commit or abort to delete closed
 * workfiles.
 */
void
workfile_mgr_cleanup(void)
{
	Assert(NULL != workfile_mgr_cache);
	Cache_SurrenderClientEntries(workfile_mgr_cache);
}

/*
 * Create a hash value based on a workfile_set_plan's signature.
 */
static workfile_set_hashkey_t
workfile_mgr_hash_key(workfile_set_plan *plan)
{
	int key_len = plan->serialized_plan_len;
	return tag_hash(plan->serialized_plan, key_len);
}

/*
 * Serializes a given plan node for hashing and matching.
 */
static workfile_set_plan *
workfile_mgr_serialize_plan(PlanState *ps)
{
	Assert(ps);
	Plan *plan = ps->plan;
	workfile_set_plan *splan = NULL;
	splan = (workfile_set_plan *) palloc(sizeof(workfile_set_plan));

	Assert(nodeTag(plan) >= T_Plan && nodeTag(plan) < T_PlanInvalItem);

	/* serialize plan, without outputting the variable fields */
	outfast_workfile_mgr_init(ps->state->es_range_table);

	char *serialized_plan = NULL;
	int plan_len = 0;
	PG_TRY();
	{
		serialized_plan = nodeToBinaryStringFast(plan, &plan_len);
		Assert(plan_len > 0);
	}
	PG_CATCH();
	{
		outfast_workfile_mgr_end();
		PG_RE_THROW();
	}
	PG_END_TRY();

	outfast_workfile_mgr_end();

	Assert(serialized_plan);
	splan->serialized_plan = serialized_plan;
	splan->serialized_plan_len = plan_len;

	return splan;
}

/*
 * Free up a workfile manager plan structure.
 */
static void
workfile_mgr_free_plan(workfile_set_plan *sf_plan)
{
	Assert(sf_plan != NULL);
	pfree(sf_plan->serialized_plan);
	pfree(sf_plan);
}

/*
 * Save the serialized plan to a file in the workfile set.
 * It will be used to do full plan matching before reusing.
 */
static void
workfile_mgr_save_plan(workfile_set *work_set, workfile_set_plan *sf_plan)
{
	Assert(work_set);
	Assert(sf_plan);

	char filename[MAXPGPATH];
	workfile_mgr_retrieve_file_no(work_set, WORKFILE_NUM_ALL_PLAN,
			filename, sizeof(filename));

	elog(gp_workfile_caching_loglevel, "Saving query plan to file %s", filename);

	ExecWorkFile *plan_file = ExecWorkFile_Create(filename,
			WORKFILE_PLAN_FILE_TYPE,
			false /* delOnClose */,
			work_set->metadata.bfz_compress_type);
	insist_log(plan_file != NULL, "Could not write to temporary work file: %m");

	bool res = ExecWorkFile_Write(plan_file, sf_plan->serialized_plan,
			sf_plan->serialized_plan_len);
	insist_log(res, "Could not write to temporary work file: %m");

	ExecWorkFile_Close(plan_file);
}

/*
 * Callback function to test if two cache resources are equivalent.
 */
static bool
workfile_set_equivalent(const void *virtual_resource, const void *physical_resource)
{
	Assert(NULL != virtual_resource);
	Assert(NULL != physical_resource);

	workfile_set *virtual_workset = (workfile_set *) virtual_resource;
	workfile_set *physical_workset = (workfile_set *) physical_resource;

	if (virtual_workset->key != physical_workset->key)
	{
		return false;
	}

	Assert(!virtual_workset->on_disk && physical_workset->on_disk && "comparing two physical or two virtual worksets not supported");
	Assert(NULL != virtual_workset->set_plan);

	return workfile_mgr_compare_plan(physical_workset, virtual_workset->set_plan);

}

/*
 * Do a byte-by-byte comparison between a given plan and a saved one.
 *
 * Returns true if identical, false otherwise
 *
 */
static bool
workfile_mgr_compare_plan(workfile_set *work_set, workfile_set_plan *sf_plan)
{
	Assert(NULL != work_set);
	Assert(NULL != sf_plan);

	char filename[MAXPGPATH];
	workfile_mgr_retrieve_file_no(work_set, WORKFILE_NUM_ALL_PLAN, filename, sizeof(filename));

	elog(gp_workfile_caching_loglevel, "Loading and comparing query plan from file %s", filename);

	ExecWorkFile *plan_file = ExecWorkFile_Open(filename,
			WORKFILE_PLAN_FILE_TYPE,
			false /*delOnClose */,
			work_set->metadata.bfz_compress_type);

	if (plan_file == NULL)
	{
		elog(gp_workfile_caching_loglevel, "could not open plan file for matching for set %s",
				work_set->path);
		return false;
	}

	char buffer[BLCKSZ];
	uint64 plan_offset = 0;
	bool match = false;

	while (true)
	{
		uint64 size_read = ExecWorkFile_Read(plan_file, buffer, sizeof(buffer));

		if (plan_offset + size_read > sf_plan->serialized_plan_len)
		{
			/* Disk plan is larger than new plan. No match */
			break;
		}

		if (size_read < sizeof(buffer) &&
				plan_offset + size_read < sf_plan->serialized_plan_len)
		{
			/* Disk plan is smaller than new plan. No match */
			break;
		}

		/* We have enough data in memory to compare */
		char *plan_pointer = ((char *) sf_plan->serialized_plan ) + plan_offset;
		if ( memcmp(buffer, plan_pointer, size_read) != 0)
		{
			break;
		}

		/* Reached the end of both streams, with no miss-match */
		if (size_read < sizeof(buffer))
		{
			match = true;
			break;
		}
		plan_offset += size_read;
	}

	ExecWorkFile_Close(plan_file);
	return match;
}


/* EOF */
