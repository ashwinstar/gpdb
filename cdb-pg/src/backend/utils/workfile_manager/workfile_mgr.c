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
#include "utils/debugbreak.h"

#include "utils/lsyscache.h"
#include "catalog/pg_proc.h"

#define WORKFILE_SET_MASK  "XXXXXXXXXX"

/* Type of temp file to use for storing the plan */
#define WORKFILE_PLAN_FILE_TYPE BUFFILE

/* Forward declarations */
static workfile_set *workfile_mgr_lookup_set(PlanState *ps);
static bool workfile_mgr_is_reusable(PlanState *ps);
static bool workfile_mgr_is_cacheable_plan(PlanState *ps);
static workfile_set_hashkey_t workfile_mgr_hash_key(workfile_set_plan *plan);
static workfile_set_plan *workfile_mgr_serialize_plan(PlanState *ps);
static void workfile_mgr_free_plan(workfile_set_plan *splan);
static void workfile_mgr_save_plan(workfile_set *work_set, workfile_set_plan *sf_plan);
static bool workfile_set_equivalent(const void *virtual_resource, const void *physical_resource);
static bool workfile_mgr_compare_plan(workfile_set *work_set, workfile_set_plan *sf_plan);
static void workfile_mgr_cleanup_set(const void *resource);
static void workfile_mgr_delete_set_directory(char *workset_path);
static void workfile_mgr_unlink_directory(const char *dirpath);
static StringInfo get_name_from_nodeType(const NodeTag node_type);
static uint64 get_operator_work_mem(PlanState *ps);

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

	/*
	 * Initialize the WorkfileDiskspace and WorkfileQueryspace APIs
	 * to track disk space usage
	 */
	WorkfileDiskspace_Init();
	WorkfileQueryspace_Init();
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
	return Cache_SharedMemSize(WORKFILE_CACHE_MAX_SIZE, sizeof(workfile_set)) +
			WorkfileDiskspace_ShMemSize() + WorkfileQueryspace_ShMemSize();
}



/*
 * Retrieves the operator name.
 * Result is palloc-ed in the current memory context.
 */
static StringInfo
get_name_from_nodeType(const NodeTag node_type)
{
	StringInfo operator_name = makeStringInfo();

	switch ( node_type )
	{
		case T_AggState:
			appendStringInfoString(operator_name,"Agg");
			break;
		case T_HashJoinState:
			appendStringInfoString(operator_name,"HashJoin");
			break;
		case T_MaterialState:
			appendStringInfoString(operator_name,"Material");
			break;
		case T_SortState:
			appendStringInfoString(operator_name,"Sort");
			break;
		case T_Invalid:
			/* When spilling from a builtin function, we don't have a valid node type */
			appendStringInfoString(operator_name,"BuiltinFunction");
			break;
		default:
			Assert(false && "Operator not supported by the workfile manager");
	}

	return operator_name;
}


/*
 * Create a new file set
 *   type is the WorkFileType for the files: BUFFILE or BFZ
 *   can_be_reused: if set to false, then we don't insert this set into the cache,
 *     since the caller is telling us there is no point. This can happen for
 *     example when spilling during index creation.
 *   ps is the PlanState for the subtree rooted at the operator
 *   snapshot contains snapshot information for the current transaction
 *
 */
workfile_set *
workfile_mgr_create_set(enum ExecWorkFileType type, bool can_be_reused, PlanState *ps, workfile_set_snapshot snapshot)
{
	Assert(NULL != workfile_mgr_cache);

	Plan *plan = NULL;
	if (ps != NULL)
	{
		plan = ps->plan;
	}

	AssertImply(can_be_reused, plan != NULL);

	CacheEntry *newEntry = Cache_AcquireEntry(workfile_mgr_cache);
	if (NULL == newEntry)
	{
		/* Could not add another entry to the cache - we filled it up */
		elog(ERROR, "could not create workfile manager entry: too many running queries");
	}

	workfile_set *work_set = CACHE_ENTRY_PAYLOAD(newEntry);
	Assert(work_set != NULL);

	work_set->complete = false;
	work_set->no_files = 0;
	work_set->size = 0L;
	work_set->metadata.type = type;
	work_set->metadata.bfz_compress_type = gp_workfile_compress_algorithm;
	work_set->metadata.snapshot = snapshot;
	work_set->metadata.num_leaf_files = 0;
	work_set->on_disk = false;
	work_set->set_plan = NULL;
	work_set->node_type = T_Invalid;
	if (ps != NULL)
	{
		work_set->node_type = ps->type;
	}
	work_set->slice_id = currentSliceId;
	work_set->session_id = gp_session_id;
	work_set->command_count = gp_command_count;

	work_set->metadata.operator_work_mem = get_operator_work_mem(ps);


	/* If workfile caching is disabled, nothing should be re-used, so override whatever the caller says */
	work_set->can_be_reused = gp_workfile_caching && can_be_reused && workfile_mgr_is_reusable(ps);

	/* Create base directory here. We need database relative path */
	StringInfo tmp_dirpath = makeStringInfo();

	appendStringInfo(tmp_dirpath,
					"%s/%s",
					getCurrentTempFilePath,
					PG_TEMP_FILES_DIR);

	if (tmp_dirpath->len > MAXPGPATH)
	{
		ereport(ERROR, (errmsg("cannot generate path %s/%s",
				getCurrentTempFilePath,
				PG_TEMP_FILES_DIR)));
	}

	mkdir(tmp_dirpath->data, S_IRWXU);
	pfree(tmp_dirpath->data);
	pfree(tmp_dirpath);

	/* Create workset directory here */
	StringInfo operator_name = get_name_from_nodeType(work_set->node_type);
 	StringInfo workset_path_masked = makeStringInfo();

	appendStringInfo(workset_path_masked,
			"%s/%s/%s_%s_Slice%d.%s",
			 getCurrentTempFilePath,
			 PG_TEMP_FILES_DIR,
			 WORKFILE_SET_PREFIX,
			 operator_name->data,
			 work_set->slice_id,
			 WORKFILE_SET_MASK);

	if (workset_path_masked->len > MAXPGPATH)
	{
		ereport(ERROR, (errmsg("cannot generate path %s/%s/%s_%s_Slice%d.%s",
			getCurrentTempFilePath,
			PG_TEMP_FILES_DIR,
			WORKFILE_SET_PREFIX,
			operator_name->data,
			work_set->slice_id,
			WORKFILE_SET_MASK)));
	}

	char *workset_path_unmasked = gp_mkdtemp(workset_path_masked->data);
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

	if ( strlen(workset_path_unmasked + strlen(getCurrentTempFilePath) + 1)
			> MAXPGPATH )
	{
			ereport(ERROR, (errmsg("cannot generate path %s",
					workset_path_unmasked + strlen(getCurrentTempFilePath) + 1)));
	}

	/* Path is now valid. Mark this work_set as physical now */
	work_set->on_disk = true;

	pfree(workset_path_masked->data);
	pfree(workset_path_masked);
	pfree(operator_name->data);
	pfree(operator_name);

	if (work_set->can_be_reused)
	{
		Assert(plan != NULL);
		Assert(nodeTag(plan) >= T_Plan && nodeTag(plan) < T_PlanInvalItem);

		workfile_set_plan *s_plan = workfile_mgr_serialize_plan(ps);
		work_set->key = workfile_mgr_hash_key(s_plan);
		workfile_mgr_save_plan(work_set, s_plan);
		workfile_mgr_free_plan(s_plan);
	}

	elog(gp_workfile_caching_loglevel, "new spill file set. key=0x%x can_be_reused=%d prefix=%s opMemKB=" INT64_FORMAT,
			work_set->key, work_set->can_be_reused, work_set->path, work_set->metadata.operator_work_mem);

	return work_set;
}

/*
 * Determine operatorMemKB for this operator.
 * For HashJoin, this is given by the right child, for everyone else it is the actual node.
 *
 * If PlanState is NULL (e.g. when spilling from a built-in function), return 0.
 */
static uint64
get_operator_work_mem(PlanState *ps)
{
	if (NULL == ps)
	{
		return 0;
	}

	uint64 operatorMemKB = 0;
	if ((IsA(ps, HashJoinState)))
	{
		Assert(IsA(ps->righttree, HashState));
		operatorMemKB = ps->righttree->plan->operatorMemKB;
	}
	else
	{
		Assert(NULL != ps->plan);
		operatorMemKB = ps->plan->operatorMemKB;
	}

	return operatorMemKB;
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

	bool cacheable_plan = workfile_mgr_is_cacheable_plan(ps);
	if (!cacheable_plan)
	{
		elog(gp_workfile_caching_loglevel, "Plan not considered for workfile caching");
		return false;
	}

	return true;
}

/*
 * Walker function to test if a subplan contains any operators that cannot
 * be cached:
 *  - node uses volatile function
 *  - external table scan
 *  - share input scan (because of synchronization issues)
 *
 * Returns CdbVisit_Failure if it finds an offending node
 */
static CdbVisitOpt
checkNoncacheableNodeWalker(PlanState *ps,
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

	if (IsA(ps, ExternalScanState) || IsA(ps,ShareInputScanState))
	{
		return CdbVisit_Failure;
	}

	/* Continue walk */
	return CdbVisit_Walk;
}

/*
 * Check if a plan contains any operators that cannot be cached
 *
 * Returns true if subplan can be cached, false otherwise
 */
static bool
workfile_mgr_is_cacheable_plan(PlanState *ps)
{
	Assert(NULL != ps);

	CdbVisitOpt status = planstate_walk_node(ps, checkNoncacheableNodeWalker, NULL);

	/* status == CdbVisit_Failure means we found a volatile node */
	return (status != CdbVisit_Failure);
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
	local_work_set->metadata.operator_work_mem = get_operator_work_mem(ps);

	CacheEntry *cachedEntry = Cache_Lookup(workfile_mgr_cache, localEntry);

	/* Release local entry and free up plan memory. We don't need it anymore */
	Cache_Release(workfile_mgr_cache, localEntry);

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

	uint64 operatorMemKB = get_operator_work_mem(ps);
	Assert(operatorMemKB > 0);

	if (operatorMemKB < work_set->metadata.operator_work_mem)
	{
		return false;
	}

	return true;
}

/*
 * Clears entire contents of workfile cache
 *
 *   Returns the number of entries removed
 */
int32
workfile_mgr_clear_cache(void)
{
	Cache *cache = workfile_mgr_get_cache();
	elog(gp_workfile_caching_loglevel, "Running workfile_mgr_clear_cache");
	return Cache_Clear(cache);
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
		elog(gp_workfile_caching_loglevel, "workfile mgr cleanup deleting set: key=0x%0xd, size=" INT64_FORMAT " path=%s",
				work_set->key, work_set->size, work_set->path);
		Assert(NULL == work_set->set_plan);
		workfile_mgr_delete_set_directory(work_set->path);

		elog(gp_workfile_caching_loglevel, "Subtracting " INT64_FORMAT " from workfile diskspace", work_set->size);

		/*
		 * When subtracting the size of this workset from our accounting,
		 * only update the per-query counter if we created the workset.
		 * In that case, the state is ACQUIRED, otherwise is CACHED or DELETED
		 */
		CacheEntry *cacheEntry = CACHE_ENTRY_HEADER(resource);
		bool update_query_space = (cacheEntry->state == CACHE_ENTRY_ACQUIRED);

		WorkfileDiskspace_Commit(0, work_set->size, update_query_space);
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

	elog(gp_workfile_caching_loglevel, "closing workfile set: can_be_reused=%d complete=%d location: %s, size=" INT64_FORMAT,
		 work_set->can_be_reused, work_set->complete, work_set->path, work_set->size);

	CacheEntry *cache_entry = CACHE_ENTRY_HEADER(work_set);

	if (Cache_IsCached(cache_entry))
	{
		/* Workset came from cache. Just release it, nothing to do */
		Cache_Release(workfile_mgr_cache, cache_entry);
		return;
	}

	if (work_set->complete && work_set->can_be_reused)
	{
		cache_entry->size = work_set->size;

		/* We want to keep this one around. Insert into cache */
		Cache_Insert(workfile_mgr_cache, cache_entry);
		Cache_Release(workfile_mgr_cache, cache_entry);
		return;
	}


	/*
	 * Fall-through case: We need to delete this work_set, as it's not reusable.
	 */
	Assert(!work_set->complete || !work_set->can_be_reused);
	Cache_Release(workfile_mgr_cache, cache_entry);
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

	ExecWorkFile *plan_file = workfile_mgr_create_fileno(work_set, WORKFILE_NUM_ALL_PLAN);
	insist_log(plan_file != NULL, "Could not create temporary work file: %m");

	elog(gp_workfile_caching_loglevel, "Saving query plan to file %s", ExecWorkFile_GetFileName(plan_file));


	bool res = ExecWorkFile_Write(plan_file, sf_plan->serialized_plan,
			sf_plan->serialized_plan_len);
	insist_log(res, "Could not write to temporary work file: %m");

	workfile_mgr_close_file(work_set, plan_file);
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

	if (virtual_workset->metadata.operator_work_mem < physical_workset->metadata.operator_work_mem)
	{
		/*
		 * Found a potential match, but the work_mem with which it was spilled
		 * is too high, so we cannot load it with our current work_mem. Skip it.
		 */
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

	ExecWorkFile *plan_file = workfile_mgr_open_fileno(work_set, WORKFILE_NUM_ALL_PLAN);
	elog(gp_workfile_caching_loglevel, "Loading and comparing query plan from file %s",
			ExecWorkFile_GetFileName(plan_file));

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

	workfile_mgr_close_file(work_set, plan_file);
	return match;
}

/*
 * Runs the eviction algorithm to identify victims and evicts them. It attempts
 * to evict victims with cumulative size >= desiredSize
 *
 * Returns the actual cumulative size of all the sets evicted
 */
int64
workfile_mgr_evict(int64 size_requested)
{
	Assert(size_requested > 0);
	Assert(NULL != workfile_mgr_cache);

	int64 size_evicted = Cache_Evict(workfile_mgr_cache, size_requested);

	elog(gp_workfile_caching_loglevel, "Eviction: requested=" INT64_FORMAT " evicted=" INT64_FORMAT,
			size_requested, size_evicted);

	return size_evicted;
}

/* EOF */
