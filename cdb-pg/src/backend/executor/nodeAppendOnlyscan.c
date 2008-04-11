/*-------------------------------------------------------------------------
 *
 * nodeAppendOnlyscan.c
 *	  Support routines for sequential scans of append-only relations.
 *
 * Portions Copyright (c) 2008, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecAppendOnlyScan				sequentially scans a relation.
 *		ExecAppendOnlyNext				retrieve next tuple in sequential order.
 *		ExecInitAppendOnlyScan			creates and initializes a seqscan node.
 *		ExecEndAppendOnlyScan			releases any storage allocated.
 *		ExecAppendOnlyReScan			rescans the relation
 */
#include "postgres.h"

#include "access/appendonlyam.h"
#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeAppendOnlyscan.h"

static TupleTableSlot *AppendOnlyNext(AppendOnlyScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		SeqNext
 *
 *		This is a workhorse for ExecSeqScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
AppendOnlyNext(AppendOnlyScanState *node)
{
	HeapTuple	tuple;
	AppendOnlyScanDesc scandesc;
	Index		scanrelid;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	scandesc = node->aos_ScanDesc;
	scanrelid = ((AppendOnlyScan *) node->ss.ps.plan)->scan.scanrelid;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;

	/*
	 * get the next tuple from the access methods
	 */
	tuple = appendonly_getnext(scandesc, direction);

	/*
	 * save the tuple and the buffer returned to us by the access methods 
	 * in our scan tuple slot and return the slot. 
	 */
	if (tuple)
		ExecStoreGenericTuple(tuple, slot, false);
	else
		ExecClearTuple(slot);

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecAppendOnlyScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieve tuples sequentially.
 *
 */

TupleTableSlot *
ExecAppendOnlyScan(AppendOnlyScanState *node)
{
	/*
	 * use AppendOnlyNext as access method
	 */
	return ExecScan((ScanState *) node, (ExecScanAccessMtd) AppendOnlyNext);
}


/* ----------------------------------------------------------------
 *		ExecInitAppendOnlyScan
 * ----------------------------------------------------------------
 */
AppendOnlyScanState *
ExecInitAppendOnlyScan(AppendOnlyScan *node, EState *estate, int eflags)
{
	AppendOnlyScanState	*appendonlystate;
	Relation			currentRelation;
	AppendOnlyScanDesc	currentScanDesc;
	
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	appendonlystate = makeNode(AppendOnlyScanState);
	appendonlystate->ss.ps.plan = (Plan *) node;
	appendonlystate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &appendonlystate->ss.ps);

	/*
	 * initialize child expressions
	 */
	appendonlystate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) appendonlystate);
	appendonlystate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) appendonlystate);

#define AOSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &appendonlystate->ss.ps);
	ExecInitScanTupleSlot(estate, &appendonlystate->ss);

	/*
	 * get the relation object id from the relid'th entry in the range table
	 * and open that relation.
	 */
	currentRelation = ExecOpenScanAppendOnlyRelation(estate, node->scan.scanrelid);	
	
	currentScanDesc = appendonly_beginscan(currentRelation, 0,NULL);
	
	appendonlystate->ss.ss_currentRelation = currentRelation;
	appendonlystate->aos_ScanDesc = currentScanDesc;
	
	ExecAssignScanType(&appendonlystate->ss, RelationGetDescr(currentRelation));
	
	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&appendonlystate->ss.ps);
	ExecAssignScanProjectionInfo(&appendonlystate->ss);

	return appendonlystate;
}

int
ExecCountSlotsAppendOnlyScan(AppendOnlyScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		AOSCAN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndAppendOnlyScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndAppendOnlyScan(AppendOnlyScanState *node)
{
	Relation			relation;
	AppendOnlyScanDesc	scanDesc;

	/*
	 * get information from node
	 */
	relation = node->ss.ss_currentRelation;
	scanDesc = node->aos_ScanDesc;

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
	 * close heap scan
	 */
	appendonly_endscan(scanDesc);

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(relation);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecAppendOnlyReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecAppendOnlyReScan(AppendOnlyScanState *node, ExprContext *exprCtxt)
{
	EState				*estate;
	Index				scanrelid;
	AppendOnlyScanDesc	scan;

	estate = node->ss.ps.state;
	scanrelid = ((AppendOnlyScan *) node->ss.ps.plan)->scan.scanrelid;

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}

	scan = node->aos_ScanDesc;

	appendonly_rescan(scan,			/* scan desc */
					  NULL);		/* new scan keys */
}
