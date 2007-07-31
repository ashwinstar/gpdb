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
	int driver_slice = 0;

	ShareInputScan * sisc = (ShareInputScan *) node->ss.ps.plan;

	ShareType share_type = sisc->share_type;
	Material *sharedMaterial = NULL;
	Sort *sharedSort = NULL;
	

	/* 
	 * get state info from node
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);

	Assert(sisc->share_type == SHARE_MATERIAL || sisc->share_type == SHARE_SORT);

	/* if first time call, need to init underlying material node */
	if(node->ts_state == NULL)
	{
		
		ShareNodeEntry *snEntry = ExecGetShareNodeEntry(estate, sisc->share_id, false); 
		PlanState* snState = NULL;
		
		if(snEntry) 
		{
			snState = (PlanState *)snEntry->shareState;
			slot = ExecProcNode(snState);

			Assert(snState->plan != NULL);

			if(share_type == SHARE_MATERIAL)
			{
				sharedMaterial = (Material *) snState->plan;
				Assert(IsA(sharedMaterial, Material));
				Assert(sisc->share_id == sharedMaterial->share_id);
				share_type = sharedMaterial->share_type;
				driver_slice = sharedMaterial->driver_slice;
			}
			else
			{
				sharedSort = (Sort *) snState->plan;
				Assert(IsA(sharedSort, Sort));
				Assert(sisc->share_id == sharedSort->share_id);
				share_type = sharedSort->share_type;
				driver_slice = sharedSort->driver_slice;
			}
		}

		if(share_type == SHARE_MATERIAL_XSLICE)
		{
			char rwfile_prefix[60];
			shareinput_create_bufname_prefix(rwfile_prefix, sizeof(rwfile_prefix), sisc->share_id);
			
			if(driver_slice != currentSliceId)
				node->share_lk_ctxt = shareinput_reader_waitready(sisc->share_id);
			else
			{
				Assert(snState && IsA(snState, MaterialState));
				((MaterialState *) snState)->nsharer_inslice++;
			}

			node->ts_state = tuplestore_begin_heap_file_readerwriter(rwfile_prefix, false);
			tuplestore_begin_pos((Tuplestorestate *) node->ts_state, (TuplestorePos **)(&node->ts_pos));
			tuplestore_rescan_pos((Tuplestorestate *) node->ts_state, (TuplestorePos *)node->ts_pos); 
		}
		else if(share_type == SHARE_MATERIAL)
		{
			Assert(snState != NULL && sharedMaterial != NULL);
			node->ts_state = ((MaterialState *)snState)->tuplestorestate;

			tuplestore_begin_pos((Tuplestorestate *) node->ts_state, (TuplestorePos **)(&node->ts_pos));
			tuplestore_rescan_pos((Tuplestorestate *) node->ts_state, (TuplestorePos *)node->ts_pos); 
		}
		else if(share_type == SHARE_SORT_XSLICE)
		{
			char rwfile_prefix[60];
			shareinput_create_bufname_prefix(rwfile_prefix, sizeof(rwfile_prefix), sisc->share_id);
			
			if(driver_slice != currentSliceId)
				node->share_lk_ctxt = shareinput_reader_waitready(sisc->share_id);
			else
			{
				Assert(snState && IsA(snState, SortState));
				((SortState *) snState)->nsharer_inslice++;
			}

			node->ts_state = tuplesort_begin_heap_file_readerwriter(
					rwfile_prefix, false,
					NULL, 0, NULL, NULL, work_mem/1024, true);
			tuplesort_begin_pos((Tuplesortstate *) node->ts_state, (TuplesortPos **)(&node->ts_pos));
			tuplesort_rescan_pos((Tuplesortstate *) node->ts_state, (TuplesortPos *)node->ts_pos); 
		}
		else 
		{
			Assert(sisc->share_type == SHARE_SORT);
			Assert(sharedSort != NULL && snState != NULL);
			node->ts_state = ((SortState *)snState)->tuplesortstate;
			tuplesort_begin_pos((Tuplesortstate *) node->ts_state, (TuplesortPos **)(&node->ts_pos));
			tuplesort_rescan_pos((Tuplesortstate *) node->ts_state, (TuplesortPos *)node->ts_pos);
		}
	}

	slot = node->ss.ps.ps_ResultTupleSlot;

	while(1)
	{
		bool gotOK = false;

		if(share_type == SHARE_MATERIAL || share_type == SHARE_MATERIAL_XSLICE) 
			gotOK = tuplestore_gettupleslot_pos( (Tuplestorestate *)node->ts_state, (TuplestorePos *)node->ts_pos, forward, slot);
		else
			gotOK = tuplesort_gettupleslot_pos((Tuplesortstate *)node->ts_state, (TuplesortPos *)node->ts_pos, forward, slot);

		if(!gotOK)
		{
			/* tuplesort_gettupleslot_pos will clear the tuple, so no need to do ExecClearTuple */ 
			if(sisc->share_type == SHARE_MATERIAL)
				return ExecClearTuple(slot);
		}

		return slot;
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
	ShareInputScanState *sisstate;
	Plan *outerPlan;
	TupleDesc tupDesc;

	Assert(innerPlan(node) == NULL);
	
	/* create state data structure */
	sisstate = makeNode(ShareInputScanState);
	sisstate->ss.ps.plan = (Plan *) node;
	sisstate->ss.ps.state = estate;
	
	sisstate->ts_state = NULL;
	sisstate->ts_pos = NULL;

	sisstate->share_lk_ctxt = NULL;

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

	/* Misc initialization 
	 * 
	 * Create expression context 
	 */
	ExecAssignExprContext(estate, &sisstate->ss.ps);

	/* tuple table init */
	ExecInitResultTupleSlot(estate, &sisstate->ss.ps);
	ExecInitScanTupleSlot(estate, &sisstate->ss);

	/* 
	 * init tuple type.
	 */
	ExecAssignResultTypeFromTL(&sisstate->ss.ps);

	{
		bool hasoid;
		if (!ExecContextForcesOids(&sisstate->ss.ps, &hasoid))
			hasoid = false;

		tupDesc = ExecTypeFromTL(node->plan.targetlist, hasoid);
	}
		
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
	ShareInputScan * sisc = (ShareInputScan *) node->ss.ps.plan;

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
	
	if(node->share_lk_ctxt)
		shareinput_reader_notifydone(node->share_lk_ctxt, sisc->share_id);

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
	ShareInputScan *sisc = (ShareInputScan *) node->ss.ps.plan;
	Assert(node->ts_state); 

	if(sisc->share_type == SHARE_MATERIAL)
		tuplestore_markpos_pos((Tuplestorestate *) node->ts_state,
			(TuplestorePos *) node->ts_pos);
	else if(sisc->share_type == SHARE_SORT) 
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
	ShareInputScan *sisc = (ShareInputScan *) node->ss.ps.plan;
	Assert(node->ts_state); 

	if(sisc->share_type == SHARE_MATERIAL)
		tuplestore_restorepos_pos((Tuplestorestate *) node->ts_state,
				(TuplestorePos *) node->ts_pos);
	else if(sisc->share_type == SHARE_SORT) 
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
	ShareInputScan *sisc = (ShareInputScan *) node->ss.ps.plan;
	Assert(node->ts_state);

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	if(sisc->share_type == SHARE_MATERIAL)
		tuplestore_rescan_pos((Tuplestorestate *) node->ts_state,
				(TuplestorePos *) node->ts_pos);
	else if (sisc->share_type == SHARE_SORT)
		tuplesort_rescan_pos((Tuplesortstate *)node->ts_state,
				(TuplesortPos *) node->ts_pos);
	else
		Assert(!"ExecShareInputScanReScan: invalid share type ");
}

/*************************************************************************
 * XXX 
 * we need some IPC mechanism for shareinptu_read_wait/writer_notify.  Semaphore is
 * the first thing come to mind but it turns out postgres is very picky about
 * how to use semaphore and we do not want to mess up with it.
 *
 * Here we use the fact the for FIFO (named pipe), we can make reader block till
 * writer create the FIFO.  mkfifo is Posix.1 and should be available on any
 * reasonable Unix like system.
 * 
 * XXX However, it is always better to have this kind of stuff abstracted out
 * by the system.  
 **************************************************************************/

#include "fcntl.h"
#include "storage/fd.h"
#include "cdb/cdbselect.h"

void shareinput_create_bufname_prefix(char* p, int size, int share_id)
{
	snprintf(p, size, "SIRW_%d_%d_%d", gp_session_id, gp_command_count, share_id);
}

static void sisc_lockname(char* p, int size, int share_id, const char* name)
{
	snprintf(p, size,
			"gpcdb2.sisc_%d_%d_%d_%d_%s",
			Gp_segment, gp_session_id, gp_command_count, share_id, name
			);
}

typedef struct ShareInput_Lk_Context 
{
	File f;
} ShareInput_Lk_Context;

void *shareinput_reader_waitready(int share_id)
{
	char lkname[100];
	ShareInput_Lk_Context *pctxt = palloc(sizeof(ShareInput_Lk_Context));

	sisc_lockname(lkname, 100, share_id, "ready");

	CreateTempFifo(lkname);

	elog(LOG, "sisc wait ready on %s", lkname);
	pctxt->f = OpenTemporaryFileName(lkname, O_WRONLY, false, false);
	return (void *) pctxt;
}

void *shareinput_writer_notifyready(int share_id)
{
	char lkname[100];
	ShareInput_Lk_Context *pctxt = palloc(sizeof(ShareInput_Lk_Context));

	sisc_lockname(lkname, 100, share_id, "ready");

	CreateTempFifo(lkname);

	elog(LOG, "sisc notify ready %s", lkname);
	pctxt->f = OpenTemporaryFileName(lkname, O_RDWR, false, true);
	return (void *) pctxt;
}

void shareinput_reader_notifydone(void *ctxt, int share_id)
{
	ShareInput_Lk_Context *pctxt = (ShareInput_Lk_Context *) ctxt;

	FileWrite(pctxt->f, "x", 1);
	elog(LOG, "sisc notify done");

	FileClose(pctxt->f); 
	pfree(ctxt);
}

void shareinput_writer_waitdone(void *ctxt, int share_id, int nsharer_xslice)
{
	int fd;
	mpp_fd_set rset;
	ShareInput_Lk_Context *pctxt = (ShareInput_Lk_Context *) ctxt;

	fd = FileGetFd(pctxt->f);
	
	while(nsharer_xslice > 0)
	{
		char x = 'a';
		int rsize = FileRead(pctxt->f, &x, 1);
		if(rsize == 1)
		{
			Assert(x == 'x');
			elog(LOG, "sisc wait done get 1 notification");
			--nsharer_xslice;
		}
		else
		{
			MPP_FD_ZERO(&rset);
			MPP_FD_SET(fd, &rset);

			elog(LOG, "sisc wait done select ... ");
			select(1, (fd_set *)&rset, NULL, NULL, NULL); 
			
			if(!MPP_FD_ISSET(fd, &rset))
				elog(ERROR, "shareinput_writer_waitdone failed");
		}
	}

	FileClose(pctxt->f);
	pfree(ctxt);
}
