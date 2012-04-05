/*
 * gp_partition_functions.c
 *    Define dynamic partition selection related functions in GPDB.
 *
 * gp_partition_propagation: This function accumulates unique partition
 *    oids for a specified dynamic table scan. A dynamic table scan node
 *    will be executed only after this function is called.
 *
 * Copyright(c) 2012 - present, EMC/Greenplum
 */

#include "postgres.h"

#include "utils/array.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "nodes/execnodes.h"

/*
 * increaseScanArraySize
 *   Increase the array size for dynamic table scans.
 *
 * The final array size is the maximum of the following two values:
 *   (1) (dynamicTableScanInfo->numScans + NUM_PID_INDEXES_ADDED)
 *   (2) newMaxPartIndex + 1.
 */
static void
increaseScanArraySize(int newMaxPartIndex)
{
	int oldNumScans = dynamicTableScanInfo->numScans;
	int newNumScans = oldNumScans + NUM_PID_INDEXES_ADDED;
	if (newNumScans < newMaxPartIndex)
	{
		newNumScans = newMaxPartIndex + 1;
	}
	
	dynamicTableScanInfo->numScans = newNumScans;
	
	if (dynamicTableScanInfo->pidIndexes == NULL)
	{
		dynamicTableScanInfo->pidIndexes = (HTAB **)
			palloc0(dynamicTableScanInfo->numScans * sizeof(HTAB*));
	}
	else
	{
		dynamicTableScanInfo->pidIndexes = (HTAB **)
			repalloc(dynamicTableScanInfo->pidIndexes,
					 dynamicTableScanInfo->numScans * sizeof(HTAB*));

		for (int scanNo = oldNumScans; scanNo < dynamicTableScanInfo->numScans; scanNo++)
		{
			dynamicTableScanInfo->pidIndexes[scanNo] = NULL;
		}
	}
}

/*
 * createPidIndex
 *   Create the pid index for a given dynamic table scan.
 */
static HTAB *
createPidIndex(int index)
{
	Assert((dynamicTableScanInfo->pidIndexes)[index] == NULL);

	HASHCTL hashCtl;
	MemSet(&hashCtl, 0, sizeof(HASHCTL));
	hashCtl.keysize = sizeof(Oid);
	hashCtl.entrysize = sizeof(Oid);
	hashCtl.hash = oid_hash;
	hashCtl.hcxt = dynamicTableScanInfo->memoryContext;
	
	return hash_create("Dynamic Table Scan Pid Index",
					   INITIAL_NUM_PIDS,
					   &hashCtl,
					   HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION);
}

PG_FUNCTION_INFO_V1(gp_partition_propagation);

/*
 * gp_partition_propagation
 *    Insert an array of pids into its pid-index.
 */
Datum
gp_partition_propagation(PG_FUNCTION_ARGS)
{
	int32 index = PG_GETARG_INT32(0);
	ArrayType *pids = PG_GETARG_ARRAYTYPE_P(1);

	Assert(ARR_NDIM(pids) == 1);
	Assert(ARR_ELEMTYPE(pids) == OIDOID);
	Assert(dynamicTableScanInfo != NULL &&
		   dynamicTableScanInfo->memoryContext != NULL);

	if (index >= dynamicTableScanInfo->numScans)
	{
		increaseScanArraySize(index);
	}
	
	Assert(index < dynamicTableScanInfo->numScans);
	if ((dynamicTableScanInfo->pidIndexes)[index] == NULL)
	{
		dynamicTableScanInfo->pidIndexes[index] = createPidIndex(index);
	}

	Assert(dynamicTableScanInfo->pidIndexes[index] != NULL);
	
	int numPids = ARR_DIMS(pids)[0];
	Oid *partOids = (Oid *)ARR_LBOUND(pids);
	
	for (int pidNo = 1; pidNo <= numPids; pidNo++)
	{
		Oid pid = partOids[pidNo];

#ifdef USE_ASSERT_CHECKING
		Oid *pidInHash = 
#endif
			hash_search(dynamicTableScanInfo->pidIndexes[index],
						&pid, HASH_ENTER, NULL /* foundPtr */);

		Assert(*pidInHash == pid);
	}

	PG_RETURN_VOID();
}

/*
 * dumpDynamicTableScanPidIndex
 *   Write out pids for a given dynamic table scan.
 */
void
dumpDynamicTableScanPidIndex(int index)
{
	if (index < 0 ||
		dynamicTableScanInfo == NULL ||
		index > dynamicTableScanInfo->numScans ||
		dynamicTableScanInfo->pidIndexes[index] == NULL)
	{
		return;
	}
	
	Assert(dynamicTableScanInfo != NULL &&
		   index < dynamicTableScanInfo->numScans &&
		   dynamicTableScanInfo->pidIndexes[index] != NULL);
	
	HASH_SEQ_STATUS status;
	hash_seq_init(&status, dynamicTableScanInfo->pidIndexes[index]);

	StringInfoData pids;
	initStringInfo(&pids);

	Oid *partOid = NULL;
	while ((partOid = (Oid *)hash_seq_search(&status)) != NULL)
	{
		appendStringInfo(&pids, "%d ", *partOid);
	}

	elog(LOG, "Dynamic Table Scan %d pids: %s", index, pids.data);
	pfree(pids.data);
}
