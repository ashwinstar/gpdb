/*
 * nodeIndexOnlyScan.c
 *   Routines to support scanning indexes without looking at the corresponding
 *   table tuples.
 *
 * Copyright (c) 2012 - present, EMC/Greenplum
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/itup.h"
#include "executor/execIndexScan.h"
#include "executor/executor.h"
#include "executor/nodeIndexOnlyScan.h"
#include "utils/lsyscache.h"

/*
 * Define the number of tuple table slots allocated for IndexOnlyScan node.
 */
#define INDEXONLYSCAN_NSLOTS 2

/*
 * Initialize the index scan descriptor if it is not initialized.
 */
static inline void
InitScanDesc(IndexOnlyScanState *indexstate)
{
	Assert(indexstate != NULL &&
		   IsA(indexstate, IndexOnlyScanState));
		   
	Relation currentRelation = indexstate->ss.ss_currentRelation;
	EState *estate = indexstate->ss.ps.state;
	
	if (indexstate->iss_ScanDesc == NULL)
	{
		/*
		 * Initialize scan descriptor.
		 */
		indexstate->iss_ScanDesc = indexonly_beginscan(currentRelation,
													   indexstate->iss_RelationDesc,
													   estate->es_snapshot,
													   indexstate->iss_NumScanKeys,
													   indexstate->iss_ScanKeys);
	}
}
	
/*
 * Free the index scan descriptor.
 */
static inline void
FreeScanDesc(IndexOnlyScanState *indexstate)
{
	Assert(indexstate != NULL &&
		   IsA(indexstate, IndexOnlyScanState));

	if (indexstate->iss_ScanDesc != NULL)
	{
		indexonly_endscan(indexstate->iss_ScanDesc);
		indexstate->iss_ScanDesc = NULL;
	}
}

/*
 * ExecInitIndexOnlyScan
 *    Initialize IndexOnlyScanState, open the index relation and
 * create index scan keys and runtime keys.
 */
IndexOnlyScanState *
ExecInitIndexOnlyScan(IndexOnlyScan *node, EState *estate, int eflags)
{
	IndexOnlyScanState *indexstate = makeNode(IndexOnlyScanState);
	InitCommonIndexScanState((IndexScanState *)indexstate, (IndexScan *)node, estate, eflags);
	
	/*
	 * Open the underlying table with appropriate lock on it, and also assign
	 * the scan type from the table descriptor.
	 */
	Relation currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid);
	indexstate->ss.ss_currentRelation = currentRelation;

	indexstate->iss_RelationDesc = 
		OpenIndexRelation(estate, node->indexid, node->scan.scanrelid);
	
	/*
	 * For IndexOnlyScan node, the scan type is from the index descriptor.
	 */
	ExecAssignScanType(&indexstate->ss, RelationGetDescr(indexstate->iss_RelationDesc));
	
	/*
	 * build the index scan keys from the index qualification
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexstate->iss_RelationDesc,
						   node->indexqual,
						   node->indexstrategy,
						   node->indexsubtype,
						   &indexstate->iss_ScanKeys,
						   &indexstate->iss_NumScanKeys,
						   &indexstate->iss_RuntimeKeys,
						   &indexstate->iss_NumRuntimeKeys,
						   NULL,	/* no ArrayKeys */
						   NULL);
	
	InitRuntimeKeysContext(indexstate);
	indexstate->iss_RuntimeKeysReady = false;

	/* Initialize the result tuple type and projection info. */
	ExecAssignResultType(&indexstate->ss.ps, RelationGetDescr(indexstate->iss_RelationDesc));
	ExecAssignScanProjectionInfo(&indexstate->ss);

	initGpmonPktForIndexOnlyScan((Plan *)node, &indexstate->ss.ps.gpmon_pkt, estate);
	
	return indexstate;
}

/*
 * initGpmonPktForIndexOnlyScan
 *    Initiaize a gpmon packet for IndexOnlyScan, and send it off.
 */
void
initGpmonPktForIndexOnlyScan(Plan *planNode, gpmon_packet_t *gpmon_pkt, EState *estate)
{
	Assert(planNode != NULL && gpmon_pkt != NULL && IsA(planNode, IndexOnlyScan));

	char *relname = get_rel_name(((IndexOnlyScan *)planNode)->indexid);
	
	Assert(GPMON_INDEXONLYSCAN_TOTAL <= GPMON_QEXEC_M_COUNT);
	InitPlanNodeGpmonPkt(planNode, gpmon_pkt, estate, PMNT_IndexOnlyScan,
						 (int64)planNode->plan_rows, 
						 relname);
	if (relname)
	{
		pfree(relname);
	}
}

/*
 * ExecStoreIndexTuple
 *   Store the given index tuple to a slot.
 *
 * Note that the slot's tuple descriptor must be the same as the index
 * tuple descriptor.
 */
static void
ExecStoreIndexTuple(TupleTableSlot *slot, IndexTuple indexTuple, TupleDesc indexTupleDesc)
{
	Datum *values = slot_get_values(slot);
	bool *isnull = slot_get_isnull(slot);
	TupleDesc tupleDesc = slot->tts_tupleDescriptor;

	Assert(tupleDesc->natts == indexTupleDesc->natts);

	ExecClearTuple(slot);
	for (int attno = 1; attno <= tupleDesc->natts; attno++)
	{
		values[attno - 1] = index_getattr(indexTuple, attno, tupleDesc, &isnull[attno - 1]);
	}
	
	ExecStoreVirtualTuple(slot);
}

/*
 * IndexOnlyNext
 *     Retrieve the next tuple from the index relation.
 */
static TupleTableSlot *
IndexOnlyNext(IndexOnlyScanState *node)
{
	EState *estate = node->ss.ps.state;
	Index scanrelid = ((IndexOnlyScan *) node->ss.ps.plan)->scan.scanrelid;
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		insist_log(false, "EvalPlanQual is not allowed in index-only scans");
	}
	
	InitScanDesc(node);

	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	ScanDirection direction =
		GetScanDirection(estate, ((IndexOnlyScan *) node->ss.ps.plan)->indexorderdir);
	if (indexonly_getnext(node->iss_ScanDesc, direction))
	{
		/*
		 * Construct the next tuple from the index tuple, store it into
		 * the given slot.
		 */
		ExecStoreIndexTuple(slot, node->iss_ScanDesc->xs_itup,
							RelationGetDescr(node->iss_RelationDesc));
		
		Gpmon_M_Incr_Rows_Out(GpmonPktFromIndexOnlyScanState(node));
		CheckSendPlanStateGpmonPkt(&node->ss.ps);
	}

	else
	{
		if (!node->ss.ps.delayEagerFree)
		{
			ExecEagerFreeIndexOnlyScan(node);
		}
	
		ExecClearTuple(slot);
	}
	
	return slot;
}

/*
 * ExecIndexOnlyScan
 *    Retrieve the next tuple for IndexOnlyScan.
 */
TupleTableSlot *
ExecIndexOnlyScan(IndexOnlyScanState *node)
{
	/*
	 * If we have runtime keys and they've not already been set up, do it now.
	 */
	if (node->iss_NumRuntimeKeys != 0 && !node->iss_RuntimeKeysReady)
	{
		ExecReScan((PlanState *) node, NULL /* exprCtxt */);
	}

	return ExecScan(&node->ss, (ExecScanAccessMtd) IndexOnlyNext);
}

/*
 * ExecIndexOnlyReScan
 *    Re-evaluate the runtime keys, and rescan the index.
 */
void
ExecIndexOnlyReScan(IndexOnlyScanState *node, ExprContext *exprCtxt)
{
	InitScanDesc(node);
	ReEvalRuntimeKeys(node, exprCtxt);
	
	index_rescan(node->iss_ScanDesc, node->iss_ScanKeys);
	
	Gpmon_M_Incr(GpmonPktFromIndexOnlyScanState(node), GPMON_INDEXONLYSCAN_RESCAN); 
	CheckSendPlanStateGpmonPkt(&node->ss.ps);
}

/*
 * ExecCountSlotsIndexOnlyScan
 *    Return the number of tuple table slots allocated for this IndexOnlyScan
 * node and all of its subplan nodes.
 */
int
ExecCountSlotsIndexOnlyScan(IndexOnlyScan *node)
{
	Assert(NULL == outerPlan((Plan *)node) &&
		   NULL == innerPlan((Plan *)node));
	
	return INDEXONLYSCAN_NSLOTS;
}

/*
 * ExecEndIndexOnlyScan
 *   Free IndexOnlyScanState, and close the index.
 */
void
ExecEndIndexOnlyScan(IndexOnlyScanState *node)
{
	FreeCommonIndexScanState((IndexScanState *)node);
	ExecEagerFreeIndexOnlyScan(node);

	/*
	 * close the index and table relation
	 */
	index_close(node->iss_RelationDesc, NoLock);
	ExecCloseScanRelation(node->ss.ss_currentRelation);
	
	EndPlanStateGpmonPkt(&node->ss.ps);
}

/*
 * ExecIndexOnlyMarkPos
 *   Mark a scan position in the index.
 */
void
ExecIndexOnlyMarkPos(IndexOnlyScanState *node)
{
	index_markpos(node->iss_ScanDesc);
}

/*
 * ExecIndexOnlyRestrPos
 *   Restore a scan position in the index.
 */
void
ExecIndexOnlyRestrPos(IndexOnlyScanState *node)
{
	index_restrpos(node->iss_ScanDesc);
	Gpmon_M_Incr(GpmonPktFromIndexOnlyScanState(node), GPMON_INDEXONLYSCAN_RESTOREPOS); 
	CheckSendPlanStateGpmonPkt(&node->ss.ps);
}

/*
 * ExecEagerFreeIndexOnlyScan
 *   Eagerly free space allocated in IndexOnlyScanState.
 */
void
ExecEagerFreeIndexOnlyScan(IndexOnlyScanState *node)
{
	FreeScanDesc(node);
}
