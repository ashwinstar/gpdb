/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.c
 *	  Support routines for sequential scans of relations.
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeSeqscan.c,v 1.61.2.1 2006/12/26 19:26:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSeqScan				sequentially scans a relation.
 *		ExecSeqNext				retrieve next tuple in sequential order.
 *		ExecInitSeqScan			creates and initializes a seqscan node.
 *		ExecEndSeqScan			releases any storage allocated.
 *		ExecSeqReScan			rescans the relation
 *		ExecSeqMarkPos			marks scan position
 *		ExecSeqRestrPos			restores scan position
 */
#include "postgres.h"

#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeSeqscan.h"

#include "cdb/cdbvars.h"
#include "miscadmin.h"

static void InitScanRelation(SeqScanState *node, EState *estate);
static TupleTableSlot *SeqNext(SeqScanState *node);

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
SeqNext(SeqScanState *node)
{
	HeapTuple	tuple;
	HeapScanDesc scandesc;
	Index		scanrelid;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ps.state;
	scandesc = node->ss_currentScanDesc;
	scanrelid = ((SeqScan *) node->ps.plan)->scanrelid;
	direction = estate->es_direction;
	slot = node->ss_ScanTupleSlot;

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle SeqScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		if (estate->es_evTupleNull[scanrelid - 1])
			return ExecClearTuple(slot);

		ExecStoreGenericTuple(estate->es_evTuple[scanrelid - 1], slot, false);

		/*
		 * Note that unlike IndexScan, SeqScan never use keys in
		 * heap_beginscan (and this is very bad) - so, here we do not check
		 * are keys ok or not.
		 */

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[scanrelid - 1] = true;
		return slot;
	}

	/*
	 * get the next tuple from the access methods
	 */
	/* CKTAN- */
	if (node->ss_heapTupleData.bot == node->ss_heapTupleData.top 
		&& !node->ss_heapTupleData.seen_EOS)
	{
		node->ss_heapTupleData.last = NULL;
		node->ss_heapTupleData.bot = 0;
		node->ss_heapTupleData.top = lengthof(node->ss_heapTupleData.item);
		heap_getnextx(scandesc, direction, node->ss_heapTupleData.item,
					  &node->ss_heapTupleData.top, 
					  &node->ss_heapTupleData.seen_EOS);

		/* Gpmon */
		if(scandesc->rs_pageatatime)
			Gpmon_M_Incr(GpmonPktFromSeqScanState(node), GPMON_SEQSCAN_PAGE);

		CheckSendPlanStateGpmonPkt(&node->ps);
	}

	node->ss_heapTupleData.last = 
		((node->ss_heapTupleData.bot < node->ss_heapTupleData.top) 
		 ? &node->ss_heapTupleData.item[node->ss_heapTupleData.bot++]
		 : 0);
	tuple = node->ss_heapTupleData.last;

	/*
	 * save the tuple and the buffer returned to us by the access methods in
	 * our scan tuple slot and return the slot.  Note: we pass 'false' because
	 * tuples returned by heap_getnext() are pointers onto disk pages and were
	 * not created with palloc() and so should not be pfree()'d.  Note also
	 * that ExecStoreTuple will increment the refcount of the buffer; the
	 * refcount will not be dropped until the tuple table slot is cleared.
	 */
	if (tuple)
		ExecStoreHeapTuple(tuple,	/* tuple to store */
					   slot,	/* slot to store in */
					   scandesc->rs_cbuf,		/* buffer associated with this
												 * tuple */
					   false);	/* don't pfree this pointer */
	else
		ExecClearTuple(slot);

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecSeqScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieve tuples sequentially.
 *
 */

TupleTableSlot *
ExecSeqScan(SeqScanState *node)
{
	/*
	 * use SeqNext as access method
	 */
	TupleTableSlot *slot = ExecScan((ScanState *) node, (ExecScanAccessMtd) SeqNext);
	Gpmon_M_Incr(GpmonPktFromSeqScanState(node), GPMON_QEXEC_M_ROWSOUT); 

	return slot;
}

/* ----------------------------------------------------------------
 *		InitScanRelation
 *
 *		This does the initialization for scan relations and
 *		subplans of scans.
 * ----------------------------------------------------------------
 */
static void
InitScanRelation(SeqScanState *node, EState *estate)
{
	Relation	currentRelation;
	HeapScanDesc currentScanDesc;

	/*
	 * get the relation object id from the relid'th entry in the range table,
	 * open that relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate,
									 ((SeqScan *) node->ps.plan)->scanrelid);

	currentScanDesc = heap_beginscan(currentRelation,
									 estate->es_snapshot,
									 0,
									 NULL);

	node->ss_currentRelation = currentRelation;
	node->ss_currentScanDesc = currentScanDesc;
	/* CKTAN- */
	node->ss_heapTupleData.bot = node->ss_heapTupleData.top 
		= node->ss_heapTupleData.seen_EOS = 0;
	node->ss_heapTupleData.last = NULL;

	ExecAssignScanType(node, RelationGetDescr(currentRelation));
}


/* ----------------------------------------------------------------
 *		ExecInitSeqScan
 * ----------------------------------------------------------------
 */
SeqScanState *
ExecInitSeqScan(SeqScan *node, EState *estate, int eflags)
{
	SeqScanState *scanstate;

	/*
	 * Once upon a time it was possible to have an outerPlan of a SeqScan, but
	 * not any more.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SeqScanState);
	scanstate->ps.plan = (Plan *) node;
	scanstate->ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ps);

	/*
	 * initialize child expressions
	 */
	scanstate->ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) scanstate);

#define SEQSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ps);
	ExecInitScanTupleSlot(estate, scanstate);

	/*
	 * initialize scan relation
	 */
	InitScanRelation(scanstate, estate);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ps);
	ExecAssignScanProjectionInfo(scanstate);

	/* Set up gpmon */
	{
		Gpmon_NameUnit_MaxVal m[GPMON_SEQSCAN_TOTAL + 1] = {
			GPMON_QEXEC_M_INIT_ROWS(0, (int64) scanstate->ps.plan->plan_rows), 
			{ "SeqScan Page Stats", "Pages", 0 }, 
			{ "SeqScan Restore Pos", "Restores", 0},
			{ "SeqScan Rescan", "Rescans", 0},
			{ NULL, NULL, 0 } 
		};

		Assert(GPMON_SEQSCAN_TOTAL <= GPMON_QEXEC_M_COUNT);
		InitPlanStateGpmonPkt(&scanstate->ps, "Sequential Scan", m); 
	}

	return scanstate;
}

int
ExecCountSlotsSeqScan(SeqScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		SEQSCAN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndSeqScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSeqScan(SeqScanState *node)
{
	Relation	relation;
	HeapScanDesc scanDesc;

	/*
	 * get information from node
	 */
	relation = node->ss_currentRelation;
	scanDesc = node->ss_currentScanDesc;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	heap_endscan(scanDesc);
	/* CKTAN */
	node->ss_heapTupleData.bot = node->ss_heapTupleData.top = 0;
	node->ss_heapTupleData.seen_EOS = 1;
	node->ss_heapTupleData.last = NULL;

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(relation);

	/* gpmon */
	EndPlanStateGpmonPkt(&node->ps);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSeqReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecSeqReScan(SeqScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	Index		scanrelid;
	HeapScanDesc scan;

	estate = node->ps.state;
	scanrelid = ((SeqScan *) node->ps.plan)->scanrelid;

	/*node->ps.ps_TupFromTlist = false;*/

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}

	scan = node->ss_currentScanDesc;

	heap_rescan(scan,			/* scan desc */
				NULL);			/* new scan keys */
	/* CKTAN */
	node->ss_heapTupleData.bot = node->ss_heapTupleData.top 
		= node->ss_heapTupleData.seen_EOS = 0;
	node->ss_heapTupleData.last = NULL;

	Gpmon_M_Incr(GpmonPktFromSeqScanState(node), GPMON_SEQSCAN_RESCAN);
	CheckSendPlanStateGpmonPkt(&node->ps);
}

/* ----------------------------------------------------------------
 *		ExecSeqMarkPos(node)
 *
 *		Marks scan position.
 * ----------------------------------------------------------------
 */
void
ExecSeqMarkPos(SeqScanState *node)
{
	HeapScanDesc scan = node->ss_currentScanDesc;

	/* CKTAN
	heap_markpos(scan);
	*/
	heap_markposx(scan, node->ss_heapTupleData.last);
}

/* ----------------------------------------------------------------
 *		ExecSeqRestrPos
 *
 *		Restores scan position.
 * ----------------------------------------------------------------
 */
void
ExecSeqRestrPos(SeqScanState *node)
{
	HeapScanDesc scan = node->ss_currentScanDesc;

	/*
	 * Clear any reference to the previously returned tuple.  This is needed
	 * because the slot is simply pointing at scan->rs_cbuf, which
	 * heap_restrpos will change; we'd have an internally inconsistent slot if
	 * we didn't do this.
	 */
	ExecClearTuple(node->ss_ScanTupleSlot);

	heap_restrpos(scan);
	Gpmon_M_Incr(GpmonPktFromSeqScanState(node), GPMON_SEQSCAN_RESTOREPOS);
	CheckSendPlanStateGpmonPkt(&node->ps);
}

/* Gpmon helpers. */
void InitPlanStateGpmonPkt(PlanState *ps, const char *nodename, Gpmon_NameUnit_MaxVal *m) 
{

	if(!ps)
		return;

	memset(&ps->gpmon_pkt, 0, sizeof(ps->gpmon_pkt));

	ps->gpmon_plan_tick = 0;

	ps->gpmon_pkt.magic = GPMON_MAGIC;
	ps->gpmon_pkt.version = GPMON_PACKET_VERSION;
	ps->gpmon_pkt.pkttype = GPMON_PKTTYPE_QEXEC;

	gpmon_getgid(&ps->gpmon_pkt.u.qexec.key.tmid, &ps->gpmon_pkt.u.qexec.key.xid);
	ps->gpmon_pkt.u.qexec.key.cid = gp_command_count;
	ps->gpmon_pkt.u.qexec.key.segid = Gp_segment;
	ps->gpmon_pkt.u.qexec.key.pid = MyProcPid;
	ps->gpmon_pkt.u.qexec.key.nid = ps->plan->plan_node_id;

	ps->gpmon_pkt.u.qexec.pnid = ps->plan->plan_parent_node_id;


	snprintf(ps->gpmon_pkt.u.qexec.ntype, sizeof(ps->gpmon_pkt.u.qexec.ntype), nodename);

	if(m)
	{
		int i=0;
		for(i=0; i<GPMON_QEXEC_M_COUNT; ++i)
		{
			AssertImply(m[i].name, m[i].unit);
			if(m[i].name)
			{
				Assert(m[i].unit);
				snprintf(ps->gpmon_pkt.u.qexec.m[i].name, 
						sizeof(ps->gpmon_pkt.u.qexec.m[i].name), 
						m[i].name);
				snprintf(ps->gpmon_pkt.u.qexec.m[i].unit, 
						sizeof(ps->gpmon_pkt.u.qexec.m[i].unit), 
						m[i].unit);
				ps->gpmon_pkt.u.qexec.m[i].valuemax = m[i].maxval;
			}
			else
				break;
		}
	}

	snprintf(ps->gpmon_pkt.u.qexec.nstatus, sizeof(ps->gpmon_pkt.u.qexec.nstatus), "Initalize");

	if(gp_enable_gpmon)
		gpmon_send(&ps->gpmon_pkt);

	snprintf(ps->gpmon_pkt.u.qexec.nstatus, sizeof(ps->gpmon_pkt.u.qexec.nstatus), "Executing");
}

void CheckSendPlanStateGpmonPkt(PlanState *ps)
{
	if(!ps)
		return;

	if(gp_enable_gpmon)
	{
		if(ps->gpmon_plan_tick != gpmon_tick)
			gpmon_send(&ps->gpmon_pkt);

		ps->gpmon_plan_tick = gpmon_tick;
	}
}

void EndPlanStateGpmonPkt(PlanState *ps)
{
	if(!ps)
		return;

	snprintf(ps->gpmon_pkt.u.qexec.nstatus, sizeof(ps->gpmon_pkt.u.qexec.nstatus), "Finished");

	if(gp_enable_gpmon)
		gpmon_send(&ps->gpmon_pkt);
}

