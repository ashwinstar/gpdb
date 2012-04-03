/*
 * nodeDynamicTableScan.c
 *    Support routines for scanning one or more relations that are determined
 * at run time. The relations could be Heap, AppendOnly Row, AppendOnly Columnar.
 *
 * DynamicTableScan node scans each relation one after the other. For each relation,
 * it opens the table, scans the tuple, and returns relevant tuples.
 *
 * Copyright (c) 2012 - present, EMC/Greenplum
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "executor/nodeDynamicTableScan.h"
#include "utils/hsearch.h"
#include "utils/elog.h"
#include "parser/parsetree.h"

#define DYNAMIC_TABLE_SCAN_NSLOTS 2

DynamicTableScanState *
ExecInitDynamicTableScan(DynamicTableScan *node, EState *estate, int eflags)
{
	Assert((eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) == 0);

	DynamicTableScanState *state = makeNode(DynamicTableScanState);
	
	InitScanStateInternal((ScanState *)state, (Plan *)node, estate, eflags);

	initGpmonPktForDynamicTableScan((Plan *)node, &state->tableScanState.ss.ps.gpmon_pkt, estate);
	
	return state;
}

/*
 * initNextTableToScan
 *   Find the next table to scan and initiate the scan if the previous table
 * is finished.
 *
 * If scanning on the current table is not finished, or a new table is found,
 * this function returns true.
 * If no more table is found, this function returns false.
 */
static bool
initNextTableToScan(DynamicTableScanState *node)
{
	ScanState *scanState = (ScanState *)node;

	if (scanState->scan_state == SCAN_INIT ||
		scanState->scan_state == SCAN_DONE)
	{
		Oid *pid = hash_seq_search(&node->pidStatus);
		if (pid == NULL)
		{
			return false;
		}
		
		scanState->ss_currentRelation = OpenScanRelationByOid(*pid);
		ExecAssignScanType(scanState, RelationGetDescr(scanState->ss_currentRelation));
		ExecAssignScanProjectionInfo(scanState);
		
		scanState->tableType = getTableType(scanState->ss_currentRelation);
		
		BeginTableScanRelation(scanState);
	}

	return true;
}

/*
 * setPidIndex
 *   Set the pid index for the given dynamic table.
 */
static void
setPidIndex(DynamicTableScanState *node)
{
	Assert(node->pidIndex == NULL);

	ScanState *scanState = (ScanState *)node;
	EState *estate = scanState->ps.state;
	DynamicTableScan *plan = (DynamicTableScan *)scanState->ps.plan;
	Assert(estate->dynamicTableScanInfo != NULL &&
		   estate->dynamicTableScanInfo->pidIndexes != NULL &&
		   estate->dynamicTableScanInfo->numScans > plan->partIndex);
	
	node->pidIndex = estate->dynamicTableScanInfo->pidIndexes[plan->partIndex];
}

TupleTableSlot *
ExecDynamicTableScan(DynamicTableScanState *node)
{
	ScanState *scanState = (ScanState *)node;
	TupleTableSlot *slot = NULL;
	
	/*
	 * If this is called the first time, find the pid index that contains all unique
	 * partition pids for this node to scan.
	 */
	if (node->pidIndex == NULL)
	{
		setPidIndex(node);
		Assert(node->pidIndex != NULL);
		hash_seq_init(&node->pidStatus, node->pidIndex);
	}

	/*
	 * Scan the table to find next tuple to return. If the current table
	 * is finished, close it and open the next table for scan.
	 */
	while (TupIsNull(slot) &&
		   initNextTableToScan(node))
	{
		slot = ExecTableScanRelation(scanState);
		
		if (!TupIsNull(slot))
		{
			Gpmon_M_Incr_Rows_Out(GpmonPktFromDynamicTableScanState(node));
			CheckSendPlanStateGpmonPkt(&scanState->ps);
		}
		
		else
		{
			EndTableScanRelation(scanState);

			Assert(scanState->ss_currentRelation != NULL);
			ExecCloseScanRelation(scanState->ss_currentRelation);
			scanState->ss_currentRelation = NULL;
		}
	}

	return slot;
}

void
ExecEndDynamicTableScan(DynamicTableScanState *node)
{
	FreeScanRelationInternal((ScanState *)node);
	EndPlanStateGpmonPkt(&node->tableScanState.ss.ps);
}

void
ExecDynamicTableReScan(DynamicTableScanState *node, ExprContext *exprCtxt)
{
	ReScanRelation((ScanState *)node);

	Gpmon_M_Incr(GpmonPktFromDynamicTableScanState(node), GPMON_DYNAMICTABLESCAN_RESCAN);
	CheckSendPlanStateGpmonPkt(&node->tableScanState.ss.ps);
}

void
ExecDynamicTableMarkPos(DynamicTableScanState *node)
{
	MarkRestrNotAllowed((ScanState *)node);
}

void
ExecDynamicTableRestrPos(DynamicTableScanState *node)
{
	MarkRestrNotAllowed((ScanState *)node);
}

int
ExecCountSlotsDynamicTableScan(DynamicTableScan *node)
{
	return DYNAMIC_TABLE_SCAN_NSLOTS;
}

void
initGpmonPktForDynamicTableScan(Plan *planNode, gpmon_packet_t *gpmon_pkt, EState *estate)
{
	Assert(planNode != NULL && gpmon_pkt != NULL && IsA(planNode, DynamicTableScan));

	{
		RangeTblEntry *rte = rt_fetch(((Scan *)planNode)->scanrelid, estate->es_range_table);
		char schema_rel_name[SCAN_REL_NAME_BUF_SIZE] = {0};
		
		Assert(GPMON_DYNAMICTABLESCAN_TOTAL <= (int)GPMON_QEXEC_M_COUNT);
		InitPlanNodeGpmonPkt(planNode, gpmon_pkt, estate, PMNT_DynamicTableScan,
							 (int64) planNode->plan_rows, GetScanRelNameGpmon(rte->relid, schema_rel_name));
	}
}
