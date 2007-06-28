/*-------------------------------------------------------------------------
 *
 * nodeShareInputScan.c
 *
 * Portions Copyright (c) 2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

/*
 * INTERFACE ROUNTINES
 * 	ExecCountSlotsShareInputScan
 *	ExecInitShareInputScan
 * 	ExecShareInputScan
 * 	ExecEndShareInputScan
 * 	ExecShareInputMarkPosScan
 * 	ExecShareInputRestrPosScan
 * 	ExecShareInputReScanScanv
 */

#include "postgres.h"

#include "cdb/cdbvars.h"
#include "executor/executor.h"
#include "executor/nodeShareInputScan.h"
#include "miscadmin.h"

static TupleTableSlot *ShareInputNext(ShareInputScanState *node);

/* ------------------------------------------------------------------
 * 	ExecShareInputScan 
 * ------------------------------------------------------------------
 */
TupleTableSlot *ExecShareInputScan(ShareInputScanState *node)
{
	return ExecScan(&node->ss, (ExecScanAccessMtd) ShareInputNext);
}

/* ------------------------------------------------------------------
 * ShareInputNext
 * 	Retrieve a tuple from the ShareInputScan
 * ------------------------------------------------------------------
 */
TupleTableSlot * 
ShareInputNext(ShareInputScanState *node)
{
	EState *estate;
	ScanDirection dir;
	bool forward;
	TupleTableSlot *slot;

	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	ShareInputScan * sisc = (ShareInputScan *) node->ss.ps.plan;

	/* 
	 * get state info from node
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);

	/* if first time call, need to init underlying material node */
	if(node->ts_state == NULL)
	{
		ShareNodeEntry *snEntry = ExecGetShareNodeEntry(estate, node->ss.ps.plan->underlying_share_id, false);
		PlanState* snState = (PlanState *)snEntry->shareState;

		/* 
		 * set up underlying material node properly.  In case of shared node, 
		 * ExecProcNode does not return a tuple, it just build the material or sort.
		 */
		slot = ExecProcNode(snState);

		if(node->share_type == SHARENODE_MATERIAL)
		{
			node->ts_state = ((MaterialState *)snState)->tuplestorestate;
			tuplestore_begin_pos((Tuplestorestate *) node->ts_state, (TuplestorePos **)(&node->ts_pos));
			tuplestore_rescan_pos((Tuplestorestate *) node->ts_state, (TuplestorePos *)node->ts_pos); 
		}
		else 
		{
			Assert(node->share_type == SHARENODE_SORT);
			node->ts_state = ((SortState *)snState)->tuplesortstate;
			tuplesort_begin_pos((Tuplesortstate *) node->ts_state, (TuplesortPos **)(&node->ts_pos));
			tuplesort_rescan_pos((Tuplesortstate *) node->ts_state, (TuplesortPos *)node->ts_pos);
		}
	}

	slot = node->ss.ps.ps_ResultTupleSlot;

	Assert(node->share_type == SHARENODE_MATERIAL || node->share_type == SHARENODE_SORT);

	while(1)
	{
		bool gotOK = false;

		if(node->share_type == SHARENODE_MATERIAL) 
			gotOK = tuplestore_gettupleslot_pos( (Tuplestorestate *)node->ts_state, (TuplestorePos *)node->ts_pos, forward, slot);
		else
			gotOK = tuplesort_gettupleslot_pos((Tuplesortstate *)node->ts_state, (TuplesortPos *)node->ts_pos, forward, slot);

		if(!gotOK)
		{
			/* tuplesort_gettupleslot_pos will clear the tuple, so no need to do ExecClearTuple */ 
			if(node->share_type == SHARENODE_MATERIAL)
				return ExecClearTuple(slot);
			else 
				return slot;
		}

		if(sisc->hashDataTypes)
		{
			uint32 hval = 0;

			Assert(node->cdbhash && node->hashExpr);
			econtext->ecxt_outertuple = slot;
			hval = evalHashKey(econtext, node->hashExpr,
					sisc->hashDataTypes, node->cdbhash);

			/* Check hval to see if it is for this segment.
			 * The check works because the following assumption:
			 * 	1. In Motion code, we determin targetRoute using sliceEntry->hashSegIdx.
			 *  2. The hashSegIdx, is constructed makeDefaultSegIdxArray, which is simply
			 * 		hashSegIdx[i] = i;
			 *  3. targetRoute, is really an index into the conns (ml_ipc.c)
			 *  4. which is really the Gp_segment
			 *
			 * All these are quite twisted way of saying hval == Gp_segment.  However, 
			 * if the assumption changed, so should be changed here
			 */
			if(hval == Gp_segment)
				return slot;
		} 
		else
		{
			Assert(!node->cdbhash && !node->hashExpr);
			return slot;
		}
	}

	Assert(!"should not be here");
	return NULL;
}

/*  ------------------------------------------------------------------
 * 	ExecInitShareInputScan 
 * ------------------------------------------------------------------
 */
ShareInputScanState *
ExecInitShareInputScan(ShareInputScan *node, EState *estate, int eflags)
{
	ShareNodeEntry *snEntry;
	ShareInputScanState *sisstate;
	Plan *outerPlan;
	PlanState* snState;	
	TupleDesc tupDesc;

	Assert(innerPlan(node) == NULL);
	
	/* create state data structure */
	sisstate = makeNode(ShareInputScanState);
	sisstate->ss.ps.plan = (Plan *) node;
	sisstate->ss.ps.state = estate;
	
	sisstate->ts_state = NULL;
	sisstate->ts_pos = NULL;

	sisstate->hashExpr = NULL;
	sisstate->cdbhash = NULL;

	if(node->hashDataTypes)
	{
		sisstate->hashExpr = (List *) ExecInitExpr((Expr *) node->hashExpr, (PlanState *) sisstate);
		sisstate->cdbhash = makeCdbHash(getgpsegmentCount(), HASH_FNV_1);
	}

	/* 
	 * init child node.  
	 * if outerPlan is NULL, this is no-op (so that the ShareInput node will be 
	 * only init-ed once).
	 */
	outerPlan = outerPlan(node);
	outerPlanState(sisstate) = ExecInitNode(outerPlan, estate, eflags);

	sisstate->ss.ps.targetlist = (List *) 
		ExecInitExpr((Expr *) node->plan.targetlist, (PlanState *) sisstate);
	Assert(node->plan.qual == NULL);
	sisstate->ss.ps.qual = NULL;

	/* tuple table init */
	ExecInitResultTupleSlot(estate, &sisstate->ss.ps);
	ExecInitScanTupleSlot(estate, &sisstate->ss);

	/* 
	 * init tuple type.
	 */
	ExecAssignResultTypeFromTL(&sisstate->ss.ps);

	/* Assign scan type from shared node */
	snEntry = ExecGetShareNodeEntry(estate, node->plan.underlying_share_id, false);
	Assert(snEntry);
	snState = (PlanState *) snEntry->shareState;
	Assert(snState); 
	if(IsA(snState, MaterialState))
		sisstate->share_type = SHARENODE_MATERIAL;
	else if(IsA(snState, SortState))
		sisstate->share_type = SHARENODE_SORT;
	else
		Assert(!"Not supported share node");

	tupDesc = ExecGetResultType(snState);
	ExecAssignScanType(&sisstate->ss, tupDesc);

	sisstate->ss.ps.ps_ProjInfo = NULL;
	return sisstate;
}

/* ------------------------------------------------------------------
 * 	ExecCountSlotsShareInputScan 
 * ------------------------------------------------------------------
 */
int 
ExecCountSlotsShareInputScan(ShareInputScan* node)
{
#define SHAREINPUT_NSLOTS 2
	return ExecCountSlotsNode(outerPlan((Plan *) node)) 
		+ SHAREINPUT_NSLOTS;
}

/* ------------------------------------------------------------------
 * 	ExecEndShareInputScan
 * ------------------------------------------------------------------
 */
void ExecEndShareInputScan(ShareInputScanState *node)
{
	/* clean up tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/* 
	 * no need to call tuplestore end.  Underlying ShareInput will take
	 * care of releasing tuplestore resources 
	 */
	/*
	 * XXX Do we need to pfree the tuplestore_state and pos? 
	 * XXX nodeMaterial.c does not, need to find out why
	 */
	if(node->share_type == SHARENODE_SORT)
	{
		if(node->ts_pos)
		{
			Assert(node->ts_state);
			tuplesort_close_pos((Tuplesortstate *)node->ts_state, (TuplesortPos *)node->ts_pos);
		}
	}

	node->ts_state = NULL;
	node->ts_pos = NULL;

	/* 
	 * shutdown subplan.  First scanner of underlying share input will
	 * do the shutdown, all other scanners are no-op because outerPlanState
	 * is NULL
	 */
	ExecEndNode(outerPlanState(node));
}

/* ------------------------------------------------------------------
 * 	ExecShareInputMarkPosScan
 * ------------------------------------------------------------------
 */
void ExecShareInputScanMarkPos(ShareInputScanState *node)
{
	Assert(node->ts_state); 

	if(node->share_type == SHARENODE_MATERIAL)
		tuplestore_markpos_pos((Tuplestorestate *) node->ts_state,
			(TuplestorePos *) node->ts_pos);
	else if(node->share_type == SHARENODE_SORT) 
		tuplesort_markpos_pos((Tuplesortstate *)node->ts_state,
				(TuplesortPos *) node->ts_pos);
	else
		Assert(!"ExecShareInputScanMarkPos: invalid share type");
}

/* ------------------------------------------------------------------
 * 	ExecShareInputRestrPosScan
 * ------------------------------------------------------------------
 */
void ExecShareInputScanRestrPos(ShareInputScanState *node)
{
	Assert(node->ts_state); 

	if(node->share_type == SHARENODE_MATERIAL)
		tuplestore_restorepos_pos((Tuplestorestate *) node->ts_state,
				(TuplestorePos *) node->ts_pos);
	else if(node->share_type == SHARENODE_SORT) 
		tuplesort_restorepos_pos((Tuplesortstate *) node->ts_state,
				(TuplesortPos *) node->ts_pos);
	else
		Assert(!"ExecShareInputScanRestrPos: invalid share type");
}

/* ------------------------------------------------------------------
 * 	ExecShareInputScanReScan
 * ------------------------------------------------------------------
 */
void ExecShareInputScanReScan(ShareInputScanState *node, ExprContext *exprCtxt)
{
	Assert(node->ts_state);

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	if(node->share_type == SHARENODE_MATERIAL)
		tuplestore_rescan_pos((Tuplestorestate *) node->ts_state,
				(TuplestorePos *) node->ts_pos);
	else if (node->share_type == SHARENODE_SORT)
		tuplesort_rescan_pos((Tuplesortstate *)node->ts_state,
				(TuplesortPos *) node->ts_pos);
	else
		Assert(!"ExecShareInputScanReScan: invalid share type ");
}
