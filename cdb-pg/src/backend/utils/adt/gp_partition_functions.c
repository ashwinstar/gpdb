/*
 * gp_partition_functions.c
 *    Define dynamic partition selection related functions in GPDB.
 *
 * gp_partition_propagation: This function accumulates unique partition
 *    oids for a specified dynamic table scan. A dynamic table scan node
 *    will be executed only after this function is called.
 *
 * gp_partition_selection: This function finds the child partition of
 *    a given parent partition oid, which satisfies a given partition
 *    key value.
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
#include "utils/guc.h"
#include "nodes/execnodes.h"
#include "cdb/cdbpartition.h"
#include "access/heapam.h"

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
 *    Insert a partition oid into its pid-index.
 */
Datum
gp_partition_propagation(PG_FUNCTION_ARGS)
{
	int32 index = PG_GETARG_INT32(0);
	Oid partOid = PG_GETARG_OID(1);

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
	
#ifdef USE_ASSERT_CHECKING
	Oid *pidInHash = 
#endif
		hash_search(dynamicTableScanInfo->pidIndexes[index],
					&partOid, HASH_ENTER, NULL /* foundPtr */);
	
	Assert(*pidInHash == partOid);

	PG_RETURN_VOID();
}

/*
 * findPartitionMetadataEntry
 *   Find PartitionMetadata object for a given partition oid from a list.
 */
static PartitionMetadata *
findPartitionMetadataEntry(List *partsMetadata, Oid partOid)
{
	ListCell *lc = NULL;
	PartitionMetadata *metadata = NULL;
	
	foreach (lc, partsMetadata)
	{
		metadata = (PartitionMetadata *)lfirst(lc);
		if (metadata->partsAndRules->part->parrelid == partOid)
		{
			return metadata;
		}
	}

	return NULL;
}

/*
 * createValueArrays
 *   Create an Datum/bool array that will be used to populate partition key value.
 *
 * The size of this array is based on the attribute number of the partition key.
 */
static void
createValueArrays(int keyAttno, Datum **values, bool **isnull)
{
	*values = palloc0(keyAttno * sizeof(Datum));
	*isnull = palloc(keyAttno * sizeof(bool));
	
	MemSet(*isnull, true, keyAttno * sizeof(bool));
}

/*
 * freeValueArrays
 *    Free Datum/bool array.
 */
static void
freeValueArrays(Datum *values, bool *isnull)
{
	pfree(values);
	pfree(isnull);
}

PG_FUNCTION_INFO_V1(gp_partition_selection);

/*
 * gp_partition_selection
 *    Find the child partition oid for a given parent partition, which
 * satisfies the given partition key value.
 *
 * This function assumes that there is only one partition key in this level.
 * Currently, this function also assumes that the parent partition is the root
 * partition.
 *
 * If no such a child partition is found, return NULL.
 */
Datum
gp_partition_selection(PG_FUNCTION_ARGS)
{
	Oid parentOid = PG_GETARG_OID(0);

	Assert(dynamicTableScanInfo != NULL);
	Assert(dynamicTableScanInfo->memoryContext != NULL);

	if (dynamicTableScanInfo->partsMetadata == NULL)
	{
		PG_RETURN_NULL();
	}
	
	PartitionMetadata *metadata =
		findPartitionMetadataEntry(dynamicTableScanInfo->partsMetadata,
								   parentOid);
	if (metadata == NULL)
	{
		PG_RETURN_NULL();
	}

	Assert(metadata->partsAndRules != NULL &&
		   metadata->accessMethods != NULL);
	Partition *part = metadata->partsAndRules->part;

	Assert(part->parnatts == 1);
	AttrNumber partAttno = part->paratts[0];

	Relation rel = relation_open(parentOid, NoLock);
	TupleDesc tupDesc = RelationGetDescr(rel);
	Assert(tupDesc->natts >= partAttno);

	Datum *values = NULL;
	bool *isnull = NULL;
	createValueArrays(partAttno, &values, &isnull);

	isnull[partAttno - 1] = PG_ARGISNULL(1);
	if (!isnull[partAttno - 1])
	{
		values[partAttno - 1] = PG_GETARG_DATUM(1);
	}

	/* set the memory context for the access methods */
	metadata->accessMethods->part_cxt = dynamicTableScanInfo->memoryContext;
	
	Oid childOid = selectPartition(metadata->partsAndRules,
								   values,
								   isnull,
								   tupDesc,
								   metadata->accessMethods);

	freeValueArrays(values, isnull);
	
	relation_close(rel, NoLock);

	if (gp_partitioning_dynamic_selection_log)
	{
		elog(LOG, "Partition Dynamic Selection: find child partition %d for parent partition %d",
			 parentOid, childOid);
	}
	
	PG_RETURN_OID(childOid);
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
