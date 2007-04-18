/*-------------------------------------------------------------------------
 *
 * nodeExternalscan.c
 *	  Support routines for scans of external relations (on flat files for example)
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */

/*
 * INTERFACE ROUTINES
 *		ExecExternalScan				sequentially scans a relation.
 *		ExecExternalNext				retrieve next tuple in sequential order.
 *		ExecInitExternalScan			creates and initializes a externalscan node.
 *		ExecEndExternalScan				releases any storage allocated.
 *		ExecExternalReScan				rescans the relation
 */
#include "postgres.h"

#include "access/fileam.h"
#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeExternalscan.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static TupleTableSlot *ExternalNext(ExternalScanState *node);

/* ----------------------------------------------------------------
*						Scan Support
* ----------------------------------------------------------------
*/
/* ----------------------------------------------------------------
*		ExternalNext
*
*		This is a workhorse for ExecExtScan
* ----------------------------------------------------------------
*/
static TupleTableSlot *
ExternalNext(ExternalScanState *node)
{
	HeapTuple	tuple;
	FileScanDesc scandesc;
	Index		scanrelid;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;
		
	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	scandesc = node->ess_ScanDesc;
	scanrelid = ((ExternalScan *) node->ss.ps.plan)->scan.scanrelid;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;
		
	/*
	 * get the next tuple from the file access methods
	 */
	tuple = file_getnext(scandesc, direction);
	
	/*
	 * save the tuple and the buffer returned to us by the access methods in
	 * our scan tuple slot and return the slot.  Note: we pass 'false' because
	 * tuples returned by heap_getnext() are pointers onto disk pages and were
	 * not created with palloc() and so should not be pfree()'d.  Note also
	 * that ExecStoreTuple will increment the refcount of the buffer; the
	 * refcount will not be dropped until the tuple table slot is cleared.
	 */
	if (tuple)
		ExecStoreTuple(tuple, slot, InvalidBuffer, true);	
	else
		ExecClearTuple(slot);
	
	return slot;
}

/* ----------------------------------------------------------------
*		ExecExternalScan(node)
*
*		Scans the external relation sequentially and returns the next qualifying
*		tuple.
*		It calls the ExecScan() routine and passes it the access method
*		which retrieve tuples sequentially.
*
*/

TupleTableSlot *
ExecExternalScan(ExternalScanState *node)
{
	/*
	 * use SeqNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) ExternalNext);
}


/* ----------------------------------------------------------------
*		ExecInitExternalScan
* ----------------------------------------------------------------
*/
ExternalScanState *
ExecInitExternalScan(ExternalScan *node, EState *estate, int eflags)
{
	ExternalScanState *externalstate;
	Relation	currentRelation;
	FileScanDesc currentScanDesc;
	
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);
	
	/*
	 * create state structure
	 */
	externalstate = makeNode(ExternalScanState);
	externalstate->ss.ps.plan = (Plan *) node;
	externalstate->ss.ps.state = estate;
	
	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &externalstate->ss.ps);
	
	/*
	 * initialize child expressions
	 */
	externalstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) externalstate);
	externalstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) externalstate);
	
#define EXTSCAN_NSLOTS 2
	
	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &externalstate->ss.ps);
	ExecInitScanTupleSlot(estate, &externalstate->ss);
	
	/*
	 * get the relation object id from the relid'th entry in the range table
	 * and open that relation.
	 */
	currentRelation = ExecOpenScanExternalRelation(estate, node->scan.scanrelid);	
	
	
	currentScanDesc = file_beginscan(currentRelation,
									 node->scan.scanrelid,
									 node->scancounter,
									 node->uriList,
									 node->fmtOpts,
									 node->isText,
									 0,
									 NULL);
	
	externalstate->ss.ss_currentRelation = currentRelation;
	externalstate->ess_ScanDesc = currentScanDesc;
	
	ExecAssignScanType(&externalstate->ss, RelationGetDescr(currentRelation));
	
	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&externalstate->ss.ps);
	ExecAssignScanProjectionInfo(&externalstate->ss);
	
	return externalstate;
}


int
ExecCountSlotsExternalScan(ExternalScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	EXTSCAN_NSLOTS;
}

/* ----------------------------------------------------------------
*		ExecEndExternalScan
*
*		frees any storage allocated through C routines.
* ----------------------------------------------------------------
*/
void
ExecEndExternalScan(ExternalScanState *node)
{
	Relation	relation;
	FileScanDesc fileScanDesc;
	
	/*
	 * get information from node
	 */
	relation = node->ss.ss_currentRelation;
	fileScanDesc = node->ess_ScanDesc;
	
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);
	
	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	
	/*
	 * close file scan
	 */
	file_endscan(fileScanDesc);
	
	/*
	 * close the external relation.
	 */
	ExecCloseScanExternalRelation(relation);
}

/* ----------------------------------------------------------------
*						Join Support
* ----------------------------------------------------------------
*/

/* ----------------------------------------------------------------
*		ExecExternalReScan
*
*		Rescans the relation.
* ----------------------------------------------------------------
*/
void
ExecExternalReScan(ExternalScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	Index		scanrelid;
	FileScanDesc fileScan;
	
	estate = node->ss.ps.state;
	scanrelid = ((SeqScan *) node->ss.ps.plan)->scanrelid;
	
	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}
	
	fileScan = node->ess_ScanDesc;
	
	file_rescan(fileScan,			/* scan desc */
				NULL);			/* new scan keys */
}
