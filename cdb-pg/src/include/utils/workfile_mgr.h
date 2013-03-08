/*-------------------------------------------------------------------------
 *
 * workfile_mgr.h
 *	  Interface for workfile manager and workfile caching.
 *
 *
 * Copyright (c) 2011, EMC Corp.
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef __WORKFILE_MGR_H__
#define __WORKFILE_MGR_H__

#include "postgres.h"
#include "executor/execWorkfile.h"
#include "utils/sharedcache.h"
#include "nodes/execnodes.h"
#include "utils/timestamp.h"

#define	WORKFILE_SET_PREFIX "spillfile_set"

/* Fixed workfile numbers common for all operators */
#define WORKFILE_NUM_ALL_PLAN 0
/* Fixed workfile numbers for each operator type */
#define WORKFILE_NUM_HASHJOIN_METADATA 1
#define WORKFILE_NUM_HASHAGG_METADATA 1
#define WORKFILE_NUM_MKSORT_METADATA 1
#define WORKFILE_NUM_MKSORT_TAPESET 2
#define WORKFILE_NUM_TUPLESTORE_DATA 1
#define WORKFILE_NUM_TUPLESTORE_LOB 2

/* Placeholder snapshot type */
typedef uint32 workfile_set_snapshot;

/* placeholder snapshot information */
#define NULL_SNAPSHOT 0

typedef struct workfile_set_plan
{
	/* serialized representation of the subplan */
	void *serialized_plan;

	/* length of serialized subplan */
	int serialized_plan_len;

} workfile_set_plan;

typedef struct
{

	/* number of buckets in a spilled hashtable */
	uint32 buckets;

	/* number of leaf (that did not re-spill) files for a hashagg operator */
	uint32 num_leaf_files;

	/* type of work files used by this operator */
	enum ExecWorkFileType type;

	/* compression level used by bfz if applicable */
	int bfz_compress_type;

	/* Snapshot information associated with this spill file set */
	workfile_set_snapshot snapshot;

	/* work_mem for this operator at the time of the spill */
	uint64 operator_work_mem;

} workfile_set_op_metadata;

typedef uint32 workfile_set_hashkey_t;

typedef struct workfile_set
{

	/* hash value of this workfile set metadata */
	workfile_set_hashkey_t key;

	/* Number of files in set */
	uint32 no_files;

	/* Prefix of files in the workfile set */
	char path[MAXPGPATH];

	/* Name of database for workfile set */
	char database_name[NAMEDATALEN];

	/* Name of user that created the workfile set */
	char user_name[NAMEDATALEN];

	/* Type of operator creating the workfile set */
	NodeTag node_type;

	/* Slice in which the spilling operator was */
	int slice_id;

	/* Session id for the query creating the workfile set */
	int session_id;

	/* Command count for the query creating the workfile set */
	int command_count;

	/* Timestamp when the workfile set was created */
	TimestampTz session_start_time;

	/* Operator-specific metadata */
	workfile_set_op_metadata metadata;

	/* Set to true for workfile sets that are associated with physical files */
	bool on_disk;

	/* For non-physical workfile sets, pointer to the serialized plan */
	workfile_set_plan *set_plan;

	/* Indicates if this set can be reused */
	bool can_be_reused;

	/* Set to true during operator execution once set is complete */
	bool complete;

	/*
	 * Pointer to the next workfile_set in the list of active workfile_sets
	 * registered for cleanup. Unused when the set is in shared memory.
	 */
	struct workfile_set *next;
} workfile_set;

/* Workfile Set operations */
workfile_set *workfile_mgr_create_set(enum ExecWorkFileType type, bool can_be_reused,
		PlanState *ps, workfile_set_snapshot snapshot);
workfile_set *workfile_mgr_find_set(PlanState *ps);
void workfile_mgr_close_set(workfile_set *work_set);
void workfile_mgr_create_file(workfile_set *work_set, char * sf_name, uint32 sf_name_len);
void workfile_mgr_retrieve_file_no(workfile_set *work_set, uint32 file_no, char *workfile_name, uint32 workfile_name_len);
void workfile_mgr_cleanup_files(const char *tmpdirname);
void workfile_mgr_cleanup(void);
bool workfile_mgr_can_reuse(workfile_set *work_set, PlanState *ps);
Size workfile_mgr_shmem_size(void);
void workfile_mgr_cache_init(void);
void workfile_mgr_mark_complete(workfile_set *work_set);
Cache *workfile_mgr_get_cache(void);
int32 workfile_mgr_clear_cache(void);

/* Serialization functions */
void outfuncs_workfile_mgr_init(List *rtable);
void outfuncs_workfile_mgr_end(void);
void outfast_workfile_mgr_init(List *rtable);
void outfast_workfile_mgr_end(void);

/* Debugging functions */
void workfile_mgr_print_set(workfile_set *work_set);

#endif /* __WORKFILE_MGR_H__ */

/* EOF */
