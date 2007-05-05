/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.c
 *	  Support routines for scanning RangeFunctions (functions in rangetable).
 *
 * Portions Copyright (c) 2006-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeFunctionscan.c,v 1.40.2.1 2006/12/26 19:26:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecFunctionScan		scans a function.
 *		ExecFunctionNext		retrieve next tuple in sequential order.
 *		ExecInitFunctionScan	creates and initializes a functionscan node.
 *		ExecEndFunctionScan		releases any storage allocated.
 *		ExecFunctionReScan		rescans the function
 */
#include "postgres.h"

#include "executor/nodeFunctionscan.h"
#include "funcapi.h"
#include "optimizer/var.h"              /* CDB: contain_var_reference() */
#include "parser/parsetree.h"
#include "utils/builtins.h"


static TupleTableSlot *FunctionNext(FunctionScanState *node);
static void tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc);
static void ExecFunctionScanExplainEnd(PlanState *planstate, struct StringInfoData *buf);


/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		FunctionNext
 *
 *		This is a workhorse for ExecFunctionScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
FunctionNext(FunctionScanState *node)
{
	TupleTableSlot *slot;
	EState	   *estate;
	ScanDirection direction;
	Tuplestorestate *tuplestorestate;

	/*
	 * get information from the estate and scan state
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;

	tuplestorestate = node->tuplestorestate;

	/*
	 * If first time through, read all tuples from function and put them in a
	 * tuplestore. Subsequent calls just fetch tuples from tuplestore.
	 */
	if (tuplestorestate == NULL)
	{
		ExprContext *econtext = node->ss.ps.ps_ExprContext;
		TupleDesc	funcTupdesc;

		tuplestorestate = ExecMakeTableFunctionResult(
				node->funcexpr,
				econtext,
				node->tupdesc,
				&funcTupdesc);
		node->tuplestorestate = tuplestorestate;

        /* CDB: Offer extra info for EXPLAIN ANALYZE. */
        if (node->ss.ps.instrument)
        {
            /* Let the tuplestore share our Instrumentation object. */
            tuplestore_set_instrument(tuplestorestate, node->ss.ps.instrument);

            /* Request a callback at end of query. */
            node->ss.ps.cdbexplainfun = ExecFunctionScanExplainEnd;
        }

        /* CDB: Offer extra info for EXPLAIN ANALYZE. */
        if (node->ss.ps.instrument)
        {
            /* Let the tuplestore share our Instrumentation object. */
            tuplestore_set_instrument(tuplestorestate, node->ss.ps.instrument);

            /* Request a callback at end of query. */
            node->ss.ps.cdbexplainfun = ExecFunctionScanExplainEnd;
        }

		/*
		 * If function provided a tupdesc, cross-check it.	We only really
		 * need to do this for functions returning RECORD, but might as well
		 * do it always.
		 */
		if (funcTupdesc)
			tupledesc_match(node->tupdesc, funcTupdesc);
	}

	/*
	 * Get the next tuple from tuplestore. Return NULL if no more tuples.
	 */
	slot = node->ss.ss_ScanTupleSlot;
	if (tuplestore_gettupleslot(tuplestorestate, 
								ScanDirectionIsForward(direction),
								slot))
	{
        /* CDB: Label each row with a synthetic ctid for subquery dedup. */
        if (node->cdb_want_ctid)
        {
            HeapTuple   tuple = ExecMaterializeSlot(slot);

            /* Increment 48-bit row count */
            node->cdb_fake_ctid.ip_posid++;
            if (node->cdb_fake_ctid.ip_posid == 0)
                ItemPointerSetBlockNumber(&node->cdb_fake_ctid,
                                          1 + ItemPointerGetBlockNumber(&node->cdb_fake_ctid));

            tuple->t_self = node->cdb_fake_ctid;
            tuple->t_tableOid = node->cdb_fake_tableoid;
        }
    }
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecFunctionScan(node)
 *
 *		Scans the function sequentially and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieves tuples sequentially.
 *
 */

TupleTableSlot *
ExecFunctionScan(FunctionScanState *node)
{
	/*
	 * use FunctionNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) FunctionNext);
}

/* ----------------------------------------------------------------
 *		ExecInitFunctionScan
 * ----------------------------------------------------------------
 */
FunctionScanState *
ExecInitFunctionScan(FunctionScan *node, EState *estate, int eflags)
{
	FunctionScanState *scanstate;
	RangeTblEntry *rte;
	Oid			funcrettype;
	TypeFuncClass functypclass;
	TupleDesc	tupdesc = NULL;
    List       *tlist;

	/*
	 * FunctionScan should not have any children.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create new ScanState for node
	 */
	scanstate = makeNode(FunctionScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

#define FUNCTIONSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) scanstate);

    /* CDB: Does targetlist contain a Var node referencing the ctid column? */
    tlist = node->scan.plan.targetlist;
    scanstate->cdb_want_ctid = contain_var_reference((Node *)tlist,
                                                     node->scan.scanrelid,
                                                     SelfItemPointerAttributeNumber,
                                                     0);
    ItemPointerSet(&scanstate->cdb_fake_ctid, 0, 0);
    ItemPointerSet(&scanstate->cdb_mark_ctid, 0, 0);
    scanstate->cdb_fake_tableoid = (Oid)node->scan.scanrelid;

	/*
	 * get info about function
	 */
	rte = rt_fetch(node->scan.scanrelid, estate->es_range_table);
	Assert(rte->rtekind == RTE_FUNCTION);

	/*
	 * Now determine if the function returns a simple or composite type, and
	 * build an appropriate tupdesc.
	 */
	functypclass = get_expr_result_type(rte->funcexpr,
										&funcrettype,
										&tupdesc);

	if (functypclass == TYPEFUNC_COMPOSITE)
	{
		/* Composite data type, e.g. a table's row type */
		Assert(tupdesc);
		/* Must copy it out of typcache for safety */
		tupdesc = CreateTupleDescCopy(tupdesc);
	}
	else if (functypclass == TYPEFUNC_SCALAR)
	{
		/* Base data type, i.e. scalar */
		char	   *attname = strVal(linitial(rte->eref->colnames));

		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   funcrettype,
						   -1,
						   0);
	}
	else if (functypclass == TYPEFUNC_RECORD)
	{
		tupdesc = BuildDescFromLists(rte->eref->colnames,
									 rte->funccoltypes,
									 rte->funccoltypmods);
	}
	else
	{
		/* crummy error message, but parser should have caught this */
		elog(ERROR, "function in FROM has unsupported return type");
	}

	/*
	 * For RECORD results, make sure a typmod has been assigned.  (The
	 * function should do this for itself, but let's cover things in case it
	 * doesn't.)
	 */
	BlessTupleDesc(tupdesc);

	scanstate->tupdesc = tupdesc;
	ExecAssignScanType(&scanstate->ss, tupdesc);

	/*
	 * Other node-specific setup
	 */
	scanstate->tuplestorestate = NULL;
	scanstate->funcexpr = ExecInitExpr((Expr *) rte->funcexpr,
									   (PlanState *) scanstate);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	return scanstate;
}

int
ExecCountSlotsFunctionScan(FunctionScan *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		FUNCTIONSCAN_NSLOTS;
}


/*
 * ExecFunctionScanExplainEnd
 *      Called before ExecutorEnd to finish EXPLAIN ANALYZE reporting.
 *
 * The cleanup that ordinarily would occur during ExecutorEnd() needs to be 
 * done earlier in order to report statistics to EXPLAIN ANALYZE.  Note that 
 * ExecEndFunctionScan() will be called for a second time during ExecutorEnd().
 */
void
ExecFunctionScanExplainEnd(PlanState *planstate, struct StringInfoData *buf)
{
    ExecEndFunctionScan((FunctionScanState *)planstate);
}                               /* ExecFunctionScanExplainEnd */


/* ----------------------------------------------------------------
 *		ExecEndFunctionScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndFunctionScan(FunctionScanState *node)
{
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
	 * Release tuplestore resources
	 */
	if (node->tuplestorestate != NULL)
		tuplestore_end(node->tuplestorestate); 
	node->tuplestorestate = NULL;
}

/* ----------------------------------------------------------------
 *		ExecFunctionMarkPos
 *
 *		Calls tuplestore to save the current position in the stored file.
 * ----------------------------------------------------------------
 */
void
ExecFunctionMarkPos(FunctionScanState *node)
{
	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

    node->cdb_mark_ctid = node->cdb_fake_ctid;

	tuplestore_markpos(node->tuplestorestate); 
}

/* ----------------------------------------------------------------
 *		ExecFunctionRestrPos
 *
 *		Calls tuplestore to restore the last saved file position.
 * ----------------------------------------------------------------
 */
void
ExecFunctionRestrPos(FunctionScanState *node)
{
	/*
	 * if we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

    node->cdb_fake_ctid = node->cdb_mark_ctid;

	tuplestore_restorepos(node->tuplestorestate);
}

/* ----------------------------------------------------------------
 *		ExecFunctionReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecFunctionReScan(FunctionScanState *node, ExprContext *exprCtxt)
{
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	/*node->ss.ps.ps_TupFromTlist = false;*/

	/*
	 * If we haven't materialized yet, just return.
	 */
	if (!node->tuplestorestate)
		return;

    ItemPointerSet(&node->cdb_fake_ctid, 0, 0);

    /*
	 * Here we have a choice whether to drop the tuplestore (and recompute the
	 * function outputs) or just rescan it.  This should depend on whether the
	 * function expression contains parameters and/or is marked volatile.
	 * FIXME soon.
	 */
	if (node->ss.ps.chgParam != NULL)
	{
		tuplestore_end(node->tuplestorestate);
		node->tuplestorestate = NULL;
	}
	else
		tuplestore_rescan(node->tuplestorestate);
}

/*
 * Check that function result tuple type (src_tupdesc) matches or can
 * be considered to match what the query expects (dst_tupdesc). If
 * they don't match, ereport.
 *
 * We really only care about number of attributes and data type.
 * Also, we can ignore type mismatch on columns that are dropped in the
 * destination type, so long as the physical storage matches.  This is
 * helpful in some cases involving out-of-date cached plans.
 */
static void
tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc)
{
	int			i;

	if (dst_tupdesc->natts != src_tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function return row and query-specified return row do not match"),
				 errdetail("Returned row contains %d attributes, but query expects %d.",
						   src_tupdesc->natts, dst_tupdesc->natts)));

	for (i = 0; i < dst_tupdesc->natts; i++)
	{
		Form_pg_attribute dattr = dst_tupdesc->attrs[i];
		Form_pg_attribute sattr = src_tupdesc->attrs[i];

		if (dattr->atttypid == sattr->atttypid)
			continue;			/* no worries */
		if (!dattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Returned type %s at ordinal position %d, but query expects %s.",
							   format_type_be(sattr->atttypid),
							   i + 1,
							   format_type_be(dattr->atttypid))));

		if (dattr->attlen != sattr->attlen ||
			dattr->attalign != sattr->attalign)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
							   i + 1)));
	}
}
