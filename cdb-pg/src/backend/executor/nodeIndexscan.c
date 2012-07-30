/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.c
 *	  Routines to support indexed scans of relations
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeIndexscan.c,v 1.117.2.1 2006/12/26 19:26:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecIndexScan			scans a relation using indices
 *		ExecIndexNext			using index to retrieve next tuple
 *		ExecInitIndexScan		creates and initializes state info.
 *		ExecIndexReScan			rescans the indexed relation.
 *		ExecEndIndexScan		releases all storage.
 *		ExecIndexMarkPos		marks scan position.
 *		ExecIndexRestrPos		restores scan position.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "cdb/cdbvars.h"
#include "executor/execdebug.h"
#include "executor/execIndexScan.h"
#include "executor/nodeIndexscan.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#define INDEXSCAN_NSLOTS 2

static TupleTableSlot *IndexNext(IndexScanState *node);

/*
 * Initialize the index scan descriptor if it is not initialized.
 */
static inline void
InitScanDesc(IndexScanState *indexstate)
{
	Assert(indexstate != NULL &&
		   IsA(indexstate, IndexScanState));
		   
	Relation currentRelation = indexstate->ss.ss_currentRelation;
	EState *estate = indexstate->ss.ps.state;
	
	if (indexstate->iss_ScanDesc == NULL)
	{
		/*
		 * Initialize scan descriptor.
		 */
		indexstate->iss_ScanDesc = index_beginscan(currentRelation,
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
FreeScanDesc(IndexScanState *indexstate)
{
	Assert(indexstate != NULL &&
		   IsA(indexstate, IndexScanState));

	if (indexstate->iss_ScanDesc != NULL)
	{
		index_endscan(indexstate->iss_ScanDesc);
		indexstate->iss_ScanDesc = NULL;
	}
}

/* ----------------------------------------------------------------
 *		IndexNext
 *
 *		Retrieve a tuple from the IndexScan node's currentRelation
 *		using the index specified in the IndexScanState information.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
IndexNext(IndexScanState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	ScanDirection direction;
	IndexScanDesc scandesc;
	Index		scanrelid;
	HeapTuple	tuple;
	TupleTableSlot *slot;

	/*
	 * extract necessary information from index scan node
	 */
	estate = node->ss.ps.state;
	direction = GetScanDirection(estate, ((IndexScan *) node->ss.ps.plan)->indexorderdir);

	InitScanDesc(node);
	scandesc = node->iss_ScanDesc;
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;
	scanrelid = ((IndexScan *) node->ss.ps.plan)->scan.scanrelid;

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle IndexScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		if (estate->es_evTupleNull[scanrelid - 1])
		{
			if (!node->ss.ps.delayEagerFree)
			{
				ExecEagerFreeIndexScan(node);
			}
			
			return ExecClearTuple(slot);
		}

		ExecStoreGenericTuple(estate->es_evTuple[scanrelid - 1], slot, false);

		/* Does the tuple meet the indexqual condition? */
		econtext->ecxt_scantuple = slot;

		ResetExprContext(econtext);

		if (!ExecQual(node->indexqualorig, econtext, false))
		{
			if (!node->ss.ps.delayEagerFree)
			{
				ExecEagerFreeIndexScan(node);
			}
			
			ExecClearTuple(slot);		/* would not be returned by scan */
		}

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[scanrelid - 1] = true;

		Gpmon_M_Incr_Rows_Out(GpmonPktFromIndexScanState(node));
                CheckSendPlanStateGpmonPkt(&node->ss.ps);
		return slot;
	}

	/*
	 * ok, now that we have what we need, fetch the next tuple.
	 */
	if ((tuple = index_getnext(scandesc, direction)) != NULL)
	{
		/*
		 * Store the scanned tuple in the scan tuple slot of the scan state.
		 * Note: we pass 'false' because tuples returned by amgetnext are
		 * pointers onto disk pages and must not be pfree()'d.
		 */
		ExecStoreHeapTuple(tuple,	/* tuple to store */
				slot,	/* slot to store in */
					   scandesc->xs_cbuf,		/* buffer containing tuple */
					   false);	/* don't pfree */

		Gpmon_M_Incr_Rows_Out(GpmonPktFromIndexScanState(node));
                CheckSendPlanStateGpmonPkt(&node->ss.ps);
		return slot;
	}

	if (!node->ss.ps.delayEagerFree)
	{
		ExecEagerFreeIndexScan(node);
	}
	
	/*
	 * if we get here it means the index scan failed so we are at the end of
	 * the scan..
	 */
	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		ExecIndexScan(node)
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecIndexScan(IndexScanState *node)
{
	/*
	 * If we have runtime keys and they've not already been set up, do it now.
	 */
	if (node->iss_NumRuntimeKeys != 0 && !node->iss_RuntimeKeysReady)
		ExecReScan((PlanState *) node, NULL);

	/*
	 * use IndexNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) IndexNext);
}

/* ----------------------------------------------------------------
 *		ExecIndexReScan(node)
 *
 *		Recalculates the value of the scan keys whose value depends on
 *		information known at runtime and rescans the indexed relation.
 *		Updating the scan key was formerly done separately in
 *		ExecUpdateIndexScanKeys. Integrating it into ReScan makes
 *		rescans of indices and relations/general streams more uniform.
 * ----------------------------------------------------------------
 */
void
ExecIndexReScan(IndexScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	ExprContext *econtext;
	Index		scanrelid;

	InitScanDesc(node);

	estate = node->ss.ps.state;
	econtext = node->iss_RuntimeContext;		/* context for runtime keys */
	scanrelid = ((IndexScan *) node->ss.ps.plan)->scan.scanrelid;

	ReEvalRuntimeKeys(node, exprCtxt);

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}

	/* reset index scan */
	index_rescan(node->iss_ScanDesc, node->iss_ScanKeys);

	Gpmon_M_Incr(GpmonPktFromIndexScanState(node), GPMON_INDEXSCAN_RESCAN); 
	CheckSendPlanStateGpmonPkt(&node->ss.ps);
}



/* ----------------------------------------------------------------
 *		ExecEndIndexScan
 * ----------------------------------------------------------------
 */
void
ExecEndIndexScan(IndexScanState *node)
{
	FreeCommonIndexScanState(node);
	ExecEagerFreeIndexScan(node);

	/*
	 * close the index and table relation
	 */
	index_close(node->iss_RelationDesc, NoLock);
	ExecCloseScanRelation(node->ss.ss_currentRelation);

	EndPlanStateGpmonPkt(&node->ss.ps);
}

/* ----------------------------------------------------------------
 *		ExecIndexMarkPos
 * ----------------------------------------------------------------
 */
void
ExecIndexMarkPos(IndexScanState *node)
{
	index_markpos(node->iss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexRestrPos
 * ----------------------------------------------------------------
 */
void
ExecIndexRestrPos(IndexScanState *node)
{
	index_restrpos(node->iss_ScanDesc);
	Gpmon_M_Incr(GpmonPktFromIndexScanState(node), GPMON_INDEXSCAN_RESTOREPOS); 
	CheckSendPlanStateGpmonPkt(&node->ss.ps);
}

/* ----------------------------------------------------------------
 *		ExecInitIndexScan
 *
 *		Initializes the index scan's state information, creates
 *		scan keys, and opens the base and index relations.
 *
 *		Note: index scans have 2 sets of state information because
 *			  we have to keep track of the base relation and the
 *			  index relation.
 * ----------------------------------------------------------------
 */
IndexScanState *
ExecInitIndexScan(IndexScan *node, EState *estate, int eflags)
{
	/*
	 * create state structure
	 */
	IndexScanState *indexstate = makeNode(IndexScanState);
	InitCommonIndexScanState(indexstate, node, estate, eflags);

	/*
	 * Open the underlying table with appropriate lock on it, and also assign
	 * the scan type from the table descriptor.
	 */
	Relation currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid);
	indexstate->ss.ss_currentRelation = currentRelation;
	ExecAssignScanType(&indexstate->ss, RelationGetDescr(currentRelation));
	
	indexstate->iss_RelationDesc = 
		OpenIndexRelation(estate, node->indexid, node->scan.scanrelid);
	
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

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&indexstate->ss.ps);
	ExecAssignScanProjectionInfo(&indexstate->ss);

	initGpmonPktForIndexScan((Plan *)node, &indexstate->ss.ps.gpmon_pkt, estate);

	return indexstate;
}

int
ExecCountSlotsIndexScan(IndexScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + INDEXSCAN_NSLOTS;
}

void
initGpmonPktForIndexScan(Plan *planNode, gpmon_packet_t *gpmon_pkt, EState *estate)
{
	Assert(planNode != NULL && gpmon_pkt != NULL && IsA(planNode, IndexScan));

	{
		char *relname = get_rel_name(((IndexScan *)planNode)->indexid);
		
		Assert(GPMON_INDEXSCAN_TOTAL <= (int) GPMON_QEXEC_M_COUNT);
		InitPlanNodeGpmonPkt(planNode, gpmon_pkt, estate, PMNT_IndexScan,
							 (int64)planNode->plan_rows, 
							 relname);
		if (relname)
			pfree(relname);
	}
}

void
ExecEagerFreeIndexScan(IndexScanState *node)
{
	FreeScanDesc(node);
}
