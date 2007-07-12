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
	Tuplestorestate *tuplestorestate;
	bool		eof_tuplestore;
	TupleTableSlot *slot;
	Material *ma;
	

	/*
	 * get state info from node
	 */
	estate = node->ss.ps.state;
	dir = estate->es_direction;
	forward = ScanDirectionIsForward(dir);
	tuplestorestate = (Tuplestorestate *) node->tuplestorestate;

	ma = (Material *) node->ss.ps.plan;
	Assert(IsA(ma, Material));

	/*
	 * If first time through, and we need a tuplestore, initialize it.
	 */
	if (tuplestorestate == NULL && (ma->share_type != SHARE_NOTSHARED || node->randomAccess))
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
			tuplestorestate = tuplestore_begin_heap_file_readerwriter(rwfile_prefix, true);
		}
		else
		{
			tuplestorestate = tuplestore_begin_heap(true, false, work_mem); 
		}
		
		Assert(tuplestorestate);
		node->tuplestorestate = (void *) tuplestorestate;

        /* CDB: Offer extra info for EXPLAIN ANALYZE. */
        if (node->ss.ps.instrument)
        {
            /* Let the tuplestore share our Instrumentation object. */
            tuplestore_set_instrument(tuplestorestate, node->ss.ps.instrument);

            /* Request a callback at end of query. */
            node->ss.ps.cdbexplainfun = ExecMaterialExplainEnd;
        }

        /* CDB: Offer extra info for EXPLAIN ANALYZE. */
        if (node->ss.ps.instrument)
        {
            /* Let the tuplestore share our Instrumentation object. */
            tuplestore_set_instrument(tuplestorestate, node->ss.ps.instrument);

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
				tuplestore_rescan(tuplestorestate);
				break;
			}
			tuplestore_puttupleslot(tuplestorestate, outerslot);
		}

	}

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
				tuplestore_flush(tuplestorestate);
				shareinput_writer_notify(ma->share_id);
			}
		}

		return NULL;
	}

	/*
	 * If we are not at the end of the tuplestore, or are going backwards, try
	 * to fetch a tuple from tuplestore.
	 */
	eof_tuplestore = (tuplestorestate == NULL) ||
		tuplestore_ateof(tuplestorestate);

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

	/*
	 * If we can fetch another tuple from the tuplestore, return it.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	if (!eof_tuplestore)
	{
		if (tuplestore_gettupleslot(tuplestorestate, forward, slot))
			return slot;
		if (forward)
			eof_tuplestore = true;
	}

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

		/*
		 * Append returned tuple to tuplestore.  NOTE: because the tuplestore
		 * is certainly in EOF state, its read position will move forward over
		 * the added tuple.  This is what we want.
		 */
		if (tuplestorestate)
			tuplestore_puttupleslot(tuplestorestate, outerslot);

		/*
		 * And return a copy of the tuple.	(XXX couldn't we just return the
		 * outerslot?)
		 */
		return ExecCopySlot(slot, outerslot); 
	}

	/*
	 * Nothing left ...
	 */
	return ExecClearTuple(slot);
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
	matstate->tuplestorestate = NULL;

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
	/*
	 * clean out the tuple table.  Normal case ps_ResultTupleSlot has
	 * been cleared by Exec.  Only need to clear it for share input
	 */
	if(ma->share_type != SHARE_NOTSHARED) 
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * Release tuplestore resources
	 */
	if (node->tuplestorestate != NULL)
	{
		/* We do not release resource here.  If the material is cross slice
		 * other slice may require it to exist.  The underlying resource is 
		 * marked to be released at transaction commit time.
		 */
		if(ma->share_type != SHARE_MATERIAL_XSLICE) 
		{
			tuplestore_end((Tuplestorestate *) node->tuplestorestate); 
			node->tuplestorestate = NULL;
		}
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
	if (!node->tuplestorestate)
		return;

	tuplestore_markpos((Tuplestorestate *) node->tuplestorestate);
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
	if (!node->tuplestorestate)
		return;

	/*
	 * restore the scan to the previously marked position
	 */
	tuplestore_restorepos((Tuplestorestate *) node->tuplestorestate); 
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
		if (!node->tuplestorestate)
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
			tuplestore_end((Tuplestorestate *) node->tuplestorestate); 
			node->tuplestorestate = NULL;
			node->eof_underlying = false;
		}
		else
			tuplestore_rescan((Tuplestorestate *) node->tuplestorestate); 
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
