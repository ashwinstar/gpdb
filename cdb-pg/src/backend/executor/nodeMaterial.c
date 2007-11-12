/*-------------------------------------------------------------------------
 *
 * nodeMaterial.c
 *	  Routines to handle materialization nodes.
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeMaterial.c,v 1.57 2006/10/04 00:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecMaterial			- materialize the result of a subplan
 *		ExecInitMaterial		- initialize node and subnodes
 *		ExecEndMaterial			- shutdown node and subnodes
 *
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeMaterial.h"
#include "utils/tuplestorenew.h"

#include "miscadmin.h"

#include "cdb/cdbvars.h"

static void ExecMaterialExplainEnd(PlanState *planstate, struct StringInfoData *buf);


/* ----------------------------------------------------------------
 *		ExecMaterial
 *
 *		As long as we are at the end of the data collected in the tuplestore,
 *		we collect one new row from the subplan on each call, and stash it
 *		aside in the tuplestore before returning it.  The tuplestore is
 *		only read if we are asked to scan backwards, rescan, or mark/restore.
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* result tuple from subplan */
ExecMaterial(MaterialState *node)
{
	EState	   *estate;
	ScanDirection dir;
	bool		forward;

#ifdef USE_OLD_TUPLESTORE
	Tuplestorestate *tuplestorestate;
#else
	NTupleStore *ts;
	NTupleStoreAccessor *tsa;
#endif

	bool		eof_tuplestore;
	TupleTableSlot *slot;
	Material *ma;
	
	/*
	 * get state info from node
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);

#ifdef USE_OLD_TUPLESTORE
	tuplestorestate = (Tuplestorestate *) node->ts_state;
#else
	ts = (NTupleStore *) node->ts_state;
	tsa = (NTupleStoreAccessor *) node->ts_pos;
#endif

	ma = (Material *) node->ss.ps.plan;
	Assert(IsA(ma, Material));

	/*
	 * If first time through, and we need a tuplestore, initialize it.
	 */
	if (
#ifdef USE_OLD_TUPLESTORE
			tuplestorestate == NULL 
#else
			ts == NULL
#endif
			&& (ma->share_type != SHARE_NOTSHARED || node->randomAccess)
		)
	{
		/* 
		 * For cross slice material, we only run ExecMaterial on DriverSlice 
		 */
		if(ma->share_type == SHARE_MATERIAL_XSLICE)
		{
			char rwfile_prefix[60];

			if(ma->driver_slice != currentSliceId)
			{
				elog(LOG, "Material Exec on CrossSlice, current slice %d", currentSliceId);
				return NULL;
			}
			
			shareinput_create_bufname_prefix(rwfile_prefix, sizeof(rwfile_prefix), ma->share_id); 
			elog(LOG, "Material node creates shareinput rwfile %s", rwfile_prefix);
#ifdef USE_OLD_TUPLESTORE
			tuplestorestate = tuplestore_begin_heap_file_readerwriter(rwfile_prefix, true);
#else
			ts = ntuplestore_create_readerwriter(rwfile_prefix, work_mem * 1024, true);
			tsa = ntuplestore_create_accessor(ts, true);
#endif
		}
		else
		{
#ifdef USE_OLD_TUPLESTORE
			tuplestorestate = tuplestore_begin_heap(true, false, work_mem); 
#else
			ts = ntuplestore_create(work_mem * 1024);
			tsa = ntuplestore_create_accessor(ts, true);
#endif
		}
		
#ifdef USE_OLD_TUPLESTORE
		Assert(tuplestorestate);
		node->ts_state = (void *) tuplestorestate;
#else
		Assert(ts && tsa);
		node->ts_state = (void *) ts;
		node->ts_pos = (void *) tsa;
#endif

        /* CDB: Offer extra info for EXPLAIN ANALYZE. */
        if (node->ss.ps.instrument)
        {
            /* Let the tuplestore share our Instrumentation object. */
#ifdef USE_OLD_TUPLESTORE
            tuplestore_set_instrument(tuplestorestate, node->ss.ps.instrument);
#else
			ntuplestore_setinstrument(ts, node->ss.ps.instrument);
#endif
            /* Request a callback at end of query. */
            node->ss.ps.cdbexplainfun = ExecMaterialExplainEnd;
        }

		/*
		 * MPP: If requested, fetch all rows from subplan and put them
		 * in the tuplestore.  This decouples a middle slice's receiving
		 * and sending Motion operators to neutralize a deadlock hazard.
		 * MPP TODO: Remove when a better solution is implemented.
		 *
		 * ShareInput: if the material node
		 * is used to share input, we will need to fetch all rows and put
		 * them in tuple store
		 */
		while (((Material *) node->ss.ps.plan)->cdb_strict 
				|| ma->share_type != SHARE_NOTSHARED)
		{
			TupleTableSlot *outerslot = ExecProcNode(outerPlanState(node));

			if (TupIsNull(outerslot))
			{
				node->eof_underlying = true;
#ifdef USE_OLD_TUPLESTORE
				tuplestore_rescan(tuplestorestate);
#else
				ntuplestore_acc_seek_bof(tsa);
#endif

				break;
			}
#ifdef USE_OLD_TUPLESTORE
			tuplestore_puttupleslot(tuplestorestate, outerslot);
#else
			ntuplestore_acc_put_tupleslot(tsa, outerslot);
#endif
		}
	
#ifndef USE_OLD_TUPLESTORE
		if(forward)
			ntuplestore_acc_seek_bof(tsa);
		else
			ntuplestore_acc_seek_eof(tsa);
#endif

		/* for share input, material do not need to return any tuple */
		if(ma->share_type != SHARE_NOTSHARED)
		{
			Assert(ma->share_type == SHARE_MATERIAL || ma->share_type == SHARE_MATERIAL_XSLICE);
			/* 
			 * if the material is shared across slice, notify consumers that
			 * it is ready.
			 */
			if(ma->share_type == SHARE_MATERIAL_XSLICE) 
			{
				if (ma->driver_slice == currentSliceId)
				{
#ifdef USE_OLD_TUPLESTORE
					tuplestore_flush(tuplestorestate);
#else
					ntuplestore_flush(ts);
#endif
					node->share_lk_ctxt = shareinput_writer_notifyready(ma->share_id, ma->nsharer_xslice); 
				}
			}
			return NULL;
		}
	}

	if(ma->share_type != SHARE_NOTSHARED)
		return NULL;

	/*
	 * If we are not at the end of the tuplestore, or are going backwards, try
	 * to fetch a tuple from tuplestore.
	 */
#ifdef USE_OLD_TUPLESTORE
	eof_tuplestore = (tuplestorestate == NULL) || tuplestore_ateof(tuplestorestate);

	if (!forward && eof_tuplestore)
	{
		if (!node->eof_underlying)
		{
			/*
			 * When reversing direction at tuplestore EOF, the first
			 * gettupleslot call will fetch the last-added tuple; but we want
			 * to return the one before that, if possible. So do an extra
			 * fetch.
			 */
			if (!tuplestore_advance(tuplestorestate, forward))
				return NULL;	/* the tuplestore must be empty */
		}
		eof_tuplestore = false;
	}
#endif

	/*
	 * If we can fetch another tuple from the tuplestore, return it.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
#ifdef USE_OLD_TUPLESTORE
	if (!eof_tuplestore)
	{
		if (tuplestore_gettupleslot(tuplestorestate, forward, slot))
			return slot;
		if (forward)
			eof_tuplestore = true;
	}
#else

	if(forward)
		eof_tuplestore = (tsa == NULL) || !ntuplestore_acc_advance(tsa, 1);
	else
		eof_tuplestore = (tsa == NULL) || !ntuplestore_acc_advance(tsa, -1);

	if(tsa!=NULL && ntuplestore_acc_tell(tsa, NULL))
	{
		ntuplestore_acc_current_tupleslot(tsa, slot);
		return slot;
	}
#endif

	/*
	 * If necessary, try to fetch another row from the subplan.
	 *
	 * Note: the eof_underlying state variable exists to short-circuit further
	 * subplan calls.  It's not optional, unfortunately, because some plan
	 * node types are not robust about being called again when they've already
	 * returned NULL.
	 */
	if (eof_tuplestore && !node->eof_underlying)
	{
		PlanState  *outerNode;
		TupleTableSlot *outerslot;

		/*
		 * We can only get here with forward==true, so no need to worry about
		 * which direction the subplan will go.
		 */
		outerNode = outerPlanState(node);
		outerslot = ExecProcNode(outerNode);
		if (TupIsNull(outerslot))
		{
			node->eof_underlying = true;
			return NULL;
		}

#ifdef USE_OLD_TUPLESTORE
		/*
		 * Append returned tuple to tuplestore.  NOTE: because the tuplestore
		 * is certainly in EOF state, its read position will move forward over
		 * the added tuple.  This is what we want.
		 */
		if (tuplestorestate)
			tuplestore_puttupleslot(tuplestorestate, outerslot);
#else
		if (tsa)
			ntuplestore_acc_put_tupleslot(tsa, outerslot);
#endif

		/*
		 * And return a copy of the tuple.	(XXX couldn't we just return the
		 * outerslot?)
		 */
		return ExecCopySlot(slot, outerslot); 
	}

	/*
	 * Nothing left ...
	 */
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitMaterial
 * ----------------------------------------------------------------
 */
MaterialState *
ExecInitMaterial(Material *node, EState *estate, int eflags)
{
	MaterialState *matstate;
	Plan	   *outerPlan;

	/*
	 * create state structure
	 */
	matstate = makeNode(MaterialState);
	matstate->ss.ps.plan = (Plan *) node;
	matstate->ss.ps.state = estate;

	/*
	 * We must have random access to the subplan output to do backward scan or
	 * mark/restore.  We also prefer to materialize the subplan output if we
	 * might be called on to rewind and replay it many times. However, if none
	 * of these cases apply, we can skip storing the data.
	 */
	matstate->randomAccess = (eflags & (EXEC_FLAG_REWIND |
										EXEC_FLAG_BACKWARD |
										EXEC_FLAG_MARK)) != 0;

	matstate->eof_underlying = false;
	matstate->ts_state = NULL;
	matstate->ts_pos = NULL;
	matstate->ts_markpos = NULL;
	matstate->share_lk_ctxt = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * Materialization nodes don't need ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

#define MATERIAL_NSLOTS 2

	/*
	 * tuple table initialization
	 *
	 * material nodes only return tuples from their materialized relation.
	 */
	ExecInitResultTupleSlot(estate, &matstate->ss.ps);
	ExecInitScanTupleSlot(estate, &matstate->ss);

	/*
	 * initialize child nodes
	 *
	 * We shield the child node from the need to support REWIND, BACKWARD, or
	 * MARK/RESTORE.
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlan = outerPlan(node);
	outerPlanState(matstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * initialize tuple type.  no need to initialize projection info because
	 * this node doesn't do projections.
	 */
	ExecAssignResultTypeFromTL(&matstate->ss.ps);
	ExecAssignScanTypeFromOuterPlan(&matstate->ss);
	matstate->ss.ps.ps_ProjInfo = NULL;

	/*
	 * If share input, need to register with range table entry
	 */
	if(node->share_type != SHARE_NOTSHARED) 
	{
		ShareNodeEntry *snEntry = ExecGetShareNodeEntry(estate, node->share_id, true); 
		snEntry->sharePlan = (Node *) node;
		snEntry->shareState = (Node *) matstate;
	}

	return matstate;
}

int
ExecCountSlotsMaterial(Material *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) +
		MATERIAL_NSLOTS;
}


/*
 * ExecMaterialExplainEnd
 *      Called before ExecutorEnd to finish EXPLAIN ANALYZE reporting.
 *
 * Some of the cleanup that ordinarily would occur during ExecEndMaterial()
 * needs to be done earlier in order to report statistics to EXPLAIN ANALYZE.  
 * Note that ExecEndMaterial() will be called again during ExecutorEnd().
 */
void
ExecMaterialExplainEnd(PlanState *planstate, struct StringInfoData *buf)
{
    PlanState *savechild = outerPlanState(planstate);

    outerPlanState(planstate) = NULL;       /* don't shut down subplan yet */
    ExecEndMaterial((MaterialState *)planstate);
    outerPlanState(planstate) = savechild;  /* restore */
}                               /* ExecMaterialExplainEnd */


/* ----------------------------------------------------------------
 *		ExecEndMaterial
 * ----------------------------------------------------------------
 */
void
ExecEndMaterial(MaterialState *node)
{
	Material *ma = (Material *) node->ss.ps.plan;

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * Release tuplestore resources
	 */
	if (node->ts_state != NULL)
	{
		if(ma->share_type == SHARE_MATERIAL_XSLICE)
			shareinput_writer_waitdone(node->share_lk_ctxt, ma->share_id, ma->nsharer_xslice);

#ifdef USE_OLD_TUPLESTORE
		tuplestore_end((Tuplestorestate *) node->tuplestorestate); 
		node->tuplestorestate = NULL;
#else
		Assert(node->ts_pos);
		ntuplestore_destroy_accessor((NTupleStoreAccessor *) node->ts_pos);
		ntuplestore_destroy((NTupleStore *) node->ts_state);

		node->ts_pos = NULL;
		node->ts_state = NULL;

		if(node->ts_markpos)
			pfree(node->ts_markpos);
		node->ts_markpos = NULL;
#endif
	}

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecMaterialMarkPos
 *
 *		Calls tuplestore to save the current position in the stored file.
 * ----------------------------------------------------------------
 */
void
ExecMaterialMarkPos(MaterialState *node)
{
	Assert(node->randomAccess);

#ifdef DEBUG
	{
		/* share input should never call this */
		Material *ma = (Material *) node->ss.ps.plan;
		Assert(ma->share_type == SHARE_NOTSHARED);
	}
#endif

	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->ts_state)
		return;

#ifdef USE_OLD_TUPLESTORE
	tuplestore_markpos((Tuplestorestate *) node->ts_state); 
#else
	Assert(node->ts_pos);

	if(node->ts_markpos == NULL)
	{
		node->ts_markpos = palloc(sizeof(NTupleStorePos));
	}

	ntuplestore_acc_tell((NTupleStoreAccessor *) node->ts_pos, (NTupleStorePos *) node->ts_markpos);
#endif
}

/* ----------------------------------------------------------------
 *		ExecMaterialRestrPos
 *
 *		Calls tuplestore to restore the last saved file position.
 * ----------------------------------------------------------------
 */
void
ExecMaterialRestrPos(MaterialState *node)
{
	Assert(node->randomAccess);

#ifdef DEBUG
	{
		/* share input should never call this */
		Material *ma = (Material *) node->ss.ps.plan;
		Assert(ma->share_type == SHARE_NOTSHARED);
	}
#endif

	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->ts_state)
		return;

#ifdef USE_OLD_TUPLESTORE
	/*
	 * restore the scan to the previously marked position
	 */
	tuplestore_restorepos((Tuplestorestate *) node->tuplestorestate); 
#else
	Assert(node->ts_pos && node->ts_markpos);
	ntuplestore_acc_seek((NTupleStoreAccessor *) node->ts_pos, (NTupleStorePos *) node->ts_markpos);
#endif
}

/* ----------------------------------------------------------------
 *		ExecMaterialReScan
 *
 *		Rescans the materialized relation.
 * ----------------------------------------------------------------
 */
void
ExecMaterialReScan(MaterialState *node, ExprContext *exprCtxt)
{
#ifdef DEBUG
	{
		/* share input should never call this */
		Material *ma = (Material *) node->ss.ps.plan;
		Assert(ma->share_type == SHARE_NOTSHARED);
	}
#endif

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	if (node->randomAccess)
	{
		/*
		 * If we haven't materialized yet, just return. If outerplan' chgParam
		 * is not NULL then it will be re-scanned by ExecProcNode, else - no
		 * reason to re-scan it at all.
		 */
		if (!node->ts_state)
			return;

		/*
		 * If subnode is to be rescanned then we forget previous stored
		 * results; we have to re-read the subplan and re-store.
		 *
		 * Otherwise we can just rewind and rescan the stored output. The
		 * state of the subnode does not change.
		 */
		if (((PlanState *) node)->lefttree->chgParam != NULL)
		{
#ifdef USE_OLD_TUPLESTORE
			tuplestore_end((Tuplestorestate *) node->ts_state); 
			node->ts_state = NULL;
			node->eof_underlying = false;
#else
			ntuplestore_destroy_accessor((NTupleStoreAccessor *) node->ts_pos);
			ntuplestore_destroy((NTupleStore *) node->ts_state);
			if(node->ts_markpos)
				pfree(node->ts_markpos);

			node->ts_state = NULL;
			node->ts_pos = NULL;
			node->ts_markpos = NULL;
			node->eof_underlying = false;
#endif
		}
		else
		{
#ifdef USE_OLD_TUPLESTORE
			tuplestore_rescan((Tuplestorestate *) node->tuplestorestate); 
#else
			ntuplestore_acc_seek_bof((NTupleStoreAccessor *) node->ts_pos);
#endif
		}
	}
	else
	{
		/* In this case we are just passing on the subquery's output */

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (((PlanState *) node)->lefttree->chgParam == NULL)
			ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
		node->eof_underlying = false;
	}
}
