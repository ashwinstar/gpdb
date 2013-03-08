/*-------------------------------------------------------------------------
 *
 * workfile_diskspace.c
 *	 Implementation of workfile manager disk space accounting
 *
 * Copyright (c) 2012, EMC Corp.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include "miscadmin.h"
#include "storage/shmem.h"
#include "executor/execWorkfile.h"
#include "utils/atomic.h"
#include "cdb/cdbvars.h"

/* Name to identify the WorkfileDiskspace shared memory area by */
#define WORKFILE_DISKSPACE_SHMEM_NAME "WorkfileDiskspace"

/* Pointer to the shared memory counter with the total used diskspace across segment */
static int64 *used_diskspace = NULL;

/*
 * Initialize shared memory area for the WorkfileDiskspace module
 */
void
WorkfileDiskspace_Init(void)
{
	bool attach = false;
	/* Allocate or attach to shared memory area */
	void *shmem_base = ShmemInitStruct(WORKFILE_DISKSPACE_SHMEM_NAME,
			WorkfileDiskspace_ShMemSize(),
			&attach);

	used_diskspace = (int64 *)shmem_base;

	Assert(0 == *used_diskspace);

}

/*
 * Returns the amount of shared memory needed for the WorkfileDiskspace module
 */
Size
WorkfileDiskspace_ShMemSize(void)
{
	return sizeof(*used_diskspace);
}

/*
 * Reserve 'bytes' bytes to write to disk
 *   This should be called before actually writing to disk
 *
 *   If enough disk space is available, increments the global counter and returns true
 *   Otherwise, returns false
 */
bool
WorkfileDiskspace_Reserve(int64 bytes)
{
	Assert(NULL != used_diskspace);

	int64 total = gp_atomic_add_64(used_diskspace, bytes);
	Assert(total >= (int64) 0);

	if (gp_workfile_max_diskspace_kb == 0)
	{
		/* not enforced */
		return true;
	}

	int64 max_allowed_diskspace = (int64) (gp_workfile_max_diskspace_kb * 1024);
	if (total <= max_allowed_diskspace)
	{
		return true;
	}
	else
	{
		(void) gp_atomic_add_64(used_diskspace, - bytes);
		elog(gp_workfile_caching_loglevel,
				"Failed to reserved size %lld. Reverted back to total %lld",
				(long long) bytes, (long long) *used_diskspace);
		return false;
	}
	/* Not reached */
}

/*
 * Notify of how many bytes were actually written to disk
 *
 * This should be called after writing to disk, with the actual number
 * of bytes written. This must be less or equal than the amount we reserved
 *
 * Returns the current used_diskspace after the commit
 */
int64
WorkfileDiskspace_Commit(int64 commit_bytes, int64 reserved_bytes)
{
	Assert(NULL != used_diskspace);
	Assert(reserved_bytes >= commit_bytes);

	int64 total = gp_atomic_add_64(used_diskspace, (commit_bytes - reserved_bytes));
	Assert(total >= (int64) 0);
	return total;
}

int64
WorkfileDiskspace_GetSize()
{
	Assert(NULL != used_diskspace);
	return *used_diskspace;
}

/* EOF */
