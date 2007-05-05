/*-------------------------------------------------------------------------
 *
 * nodeWindow.c
 *	  Routines to handle window nodes.
 *
 *
 * Copyright (c) 2007, greenplum inc *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/* XXX include list is speculative -- bhagenbuch */
#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_window.h"
#include "cdb/cdbvars.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeWindow.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/syscache.h"

/*
 * WindowStatePerLevelData - per-level working state
 */
typedef struct WindowStatePerLevelData
{
	Index	    level;			/* index of WindowKey for this level */
	FmgrInfo   *eqfunctions;	/* equality fns for partial key */

	int64		group_index;
	int64		prior_non_peer_count;
	int64		peer_index;
	int64		peer_count;
	int64		rank;
	int64		dense_rank;
	int64		prior_rank;
	int64		prior_dense_rank;
} WindowStatePerLevelData;

/* 
 * WindowStatePerFunctionData
 */
typedef struct WindowStatePerFunctionData
{
	WindowRefExprState *wrxstate;
	WindowRef		   *wref;
	WindowStatePerLevelData *wlevel;
	bool				isAgg;
	int					numargs; /* explicit, caller-supplied args */
	int16				resulttypeLen;
	bool				resulttypeByVal;
	
	/* Ordinary window function */
	Oid					windowfn_oid;
	FmgrInfo			windowfn;
	
	/* Aggregate-derived window function */
	Oid					transfn_oid;
	Oid					finalfn_oid;
	Oid					invtransfn_oid;
	Oid					invprelimfn_oid;
	
	FmgrInfo			transfn;
	FmgrInfo			finalfn;
	FmgrInfo			invtransfn;
	FmgrInfo			invprelimfn;

	Datum				aggInitValue;
	bool				aggInitValueIsNull;

	int16				aggTranstypeLen;
	bool				aggTranstypeByVal;

	/* the actual transition value */
	Datum				aggTransValue;
	bool				aggTransValueIsNull;
	bool				aggNoTransValue;
} WindowStatePerFunctionData;

static WindowState *makeWindowState(Window *window, EState *estate);
static void initializePartition(WindowState *wstate);
static void advanceKeyLevelState(WindowState *wstate, int level, bool key_changed);
static void resetPerFunctionAggStates(WindowState *wstate, Index minlevel);
static TupleTableSlot *nextRow(WindowState *wstate, ExprContext *econtext);

/* WindowState constructor.
 */
static WindowState *
makeWindowState(Window *window, EState *estate)
{
	int numlevels;
	WindowState *wstate = makeNode(WindowState);
	
	wstate->ps.plan = (Plan *) window;
	wstate->ps.state = estate;

	wstate->wrxstates = NIL;
	wstate->eqfunctions = NULL;
	wstate->numfuncs = 0;
	wstate->func_state = NULL;
	
	wstate->row_index = 0;
	wstate->level_state = NULL;
	
	numlevels = list_length(window->windowKeys);
	if ( numlevels > 0 )
		wstate->level_state = (WindowStatePerLevel)
			palloc0(numlevels * sizeof(WindowStatePerLevelData));
	else
		wstate->level_state = NULL;
	wstate->numlevels = numlevels;
	
	/* XXX: only do this if we're processing an Agg */
	wstate->transcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "TransContext",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);
	return wstate;
}

/* [Re]initialize prior to processing a new partition.
 */
static void
initializePartition(WindowState *wstate)
{
	int level;
	
	/* Reset partition primitive values. Note row_index is -1 to flag 
	 * "before first row of partition" and so that it increments to 0.
	 */
	wstate->row_index = -1; 
	
	 /* Reset key-level primitive values.
	  */
	for ( level = 0; level < wstate->numlevels; level++ )
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];
		
		level_state->group_index = 0;
		level_state->prior_non_peer_count = 0;
		level_state->peer_index = -1;
		level_state->peer_count = -1;
		level_state->rank = 1;
		level_state->dense_rank = 1;
		level_state->prior_rank = 1;
		level_state->prior_dense_rank = 1;
		
		/* TODO per-key-level buffer management reinitialization */
	}
	
	/* Per-partition buffer management reinitialzation. */
	
	if ( wstate->numlevels > 0 )
		resetPerFunctionAggStates(wstate,0);
}

/* Advance the "primitive" values that drive the calculation of window
 * functions for the window ordering of the specified level.  
 * 
 * On entry these values are as set by partition initialization or as 
 * left by the  previous call to this function. Per-partition state 
 * has not yet been advanced by function nextRow(), our caller.
 *
 * The "primitive" values managed here are read-only outside the window 
 * framework implemented by this function, initializePartition(), and 
 * nextRow().
 *
 * Note that the content of different key levels is independent.  The
 * function nextRow() is responsible for calling this function as needed
 * for related key levels.
 */

static void
advanceKeyLevelState(WindowState *wstate, int level, bool key_changed)
{
	WindowStatePerLevel level_state;
	int64 cur_row_index;
	
	Assert(0 <= level && level <= wstate->numlevels);

	level_state = &wstate->level_state[level];
	cur_row_index = 1 + wstate->row_index;

	if (key_changed)
	{
		/* The level data is still set for the previous row. 
		 * Remember the rank and dense_rank values.
		 */
		level_state->prior_rank = level_state->rank;
		level_state->prior_dense_rank = level_state->dense_rank;
		
		/* Advance level data for first peer in next group. */
		level_state->prior_non_peer_count += level_state->peer_index + 1;
		level_state->group_index++;
		level_state->peer_index = 0;
		level_state->peer_count = -1; /* flags "not set" */
		level_state->rank = 1 + cur_row_index; 
		level_state->dense_rank = 1 + level_state->group_index;
	}
	else
		/* Advance level data for next peer in current group. */
		level_state->peer_index++;
}


/* Reset the per-function aggregate states at or beyond the specified
 * key level.  This function should be called once per partition-start
 * and once per row that causes a key break.
 */
static void 
resetPerFunctionAggStates(WindowState *wstate, Index minlevel)
{
	int funcno;
	
	
	/* reset transition values for each aggregate */

	for ( funcno = 0; funcno < wstate->numfuncs; funcno++ )
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		
		if (funcstate->isAgg && funcstate->wlevel->level >= minlevel)
		{
			/* free existing value, if any */
			if (!funcstate->aggTranstypeByVal &&
				!funcstate->aggTransValueIsNull &&
				DatumGetPointer(funcstate->aggTransValue) != NULL)
			{
	            pfree(DatumGetPointer(funcstate->aggTransValue));
			}

			if (funcstate->aggInitValueIsNull)
				funcstate->aggTransValue = funcstate->aggInitValue;
			else
			{
				MemoryContext oldcxt;

				oldcxt = MemoryContextSwitchTo(wstate->transcontext);
				funcstate->aggTransValue = datumCopy(funcstate->aggInitValue,
												  funcstate->aggTranstypeByVal,
												  funcstate->aggTranstypeLen);
				MemoryContextSwitchTo(oldcxt);
			}
			funcstate->aggTransValueIsNull = funcstate->aggInitValueIsNull;
			funcstate->aggNoTransValue = funcstate->aggInitValueIsNull;
		}
	}
}

/*
 * Pull a row from the outer plan.
 */
static TupleTableSlot *
nextRow(WindowState *wstate, ExprContext *econtext)
{
	Window *window = (Window *)wstate->ps.plan;
	PlanState  *outerPlan = outerPlanState(wstate);
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *priorTupleSlot = wstate->priorslot;
	bool part_break;
	int min_key_break = -1;

	/* Get the new current row into the outer tuple slot. 
	 *
	 * For now, that just means fetch a row from the outer plan. 
	 * 
	 * TODO Support buffering, etc for framing and fetch ahead for
	 *      peer count when specified.
	 */
	outerTupleSlot = ExecProcNode(outerPlan);	
	
	if ( TupIsNull(outerTupleSlot) )
		return outerTupleSlot;
	
	/* If first call, initialize priorslot. */
	if (priorTupleSlot->tts_tupleDescriptor == NULL)
	{
		ExecSetSlotDescriptor(priorTupleSlot, outerTupleSlot->tts_tupleDescriptor);
	}

	/*
	 * Process state information for window functions (unless we are at the
	 * start of a partition).
	 */
	if ( wstate->row_index >= 0 )
	{
		part_break = (window->numPartCols && 
					  (!execTuplesMatch(priorTupleSlot, outerTupleSlot, 
										window->numPartCols, window->partColIdx, 
										wstate->eqfunctions,
										econtext->ecxt_per_tuple_memory)));
		if (part_break)
			initializePartition(wstate);
		else
		{
			ListCell *lc;
			int level = 0;
			bool match = true;
			
			/* Process ordering partial key that breaks. */
			foreach (lc, window->windowKeys)
			{
				WindowKey *key = (WindowKey *)lfirst(lc);
				/* XXX: should just put the eqfunctions in WindowKey */
				WindowStatePerLevel ls = &wstate->level_state[level];
				
				if ( match )
				{
					match = execTuplesMatch(priorTupleSlot, outerTupleSlot, 
											key->numSortCols, key->sortColIdx,
											ls->eqfunctions,
											econtext->ecxt_per_tuple_memory);
					if ( !match )
						min_key_break = level;
				}

				advanceKeyLevelState(wstate, level, !match);

				level++;
			}
		}
	}
	wstate->row_index++;
	
	if ( min_key_break > 0 )
		resetPerFunctionAggStates(wstate, min_key_break);

	ExecCopySlot(priorTupleSlot, outerTupleSlot); /* for next time through */
	
	return outerTupleSlot;
}

/*
 * ExecWindow
 *
 */
TupleTableSlot *
ExecWindow(WindowState *wstate)
{
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *resultSlot;
	ExprContext *econtext;
	ExprDoneCond isDone;
	int funcno;

	econtext = wstate->ps.ps_ExprContext;
	
	/* Next input tuple. */
	outerTupleSlot = nextRow(wstate, econtext);
	
	if (TupIsNull(outerTupleSlot))
		return NULL;
	
	wstate->ps.ps_OuterTupleSlot = outerTupleSlot;

	/* BORROWED FROM nodeResult.c for now :-(
	 *
	 * XXX gross hack. use outer tuple as scan tuple for projection
	 */
	econtext->ecxt_outertuple = outerTupleSlot;
	econtext->ecxt_scantuple = outerTupleSlot;
		
	/* Evaluate the function underlying each WindowRef and put its
	 * result in the aggvalues/aggnulls array of the expression
	 * context. 
	 */
	for ( funcno = 0; funcno < wstate->numfuncs; funcno++ )
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		WindowRefExprState *wrxstate = funcstate->wrxstate;
		FunctionCallInfoData fcinfo;
		ListCell   *lcarg;
		int			argno;
		
		/* Evaluate function arguments, save in fcinfo. */

		/* If this is an agg, the first arg will be the trans value */
		if (funcstate->isAgg)
			argno = 1;
		else
			argno = 0;

		foreach ( lcarg, wrxstate->args )
		{
			ExprState  *argstate = (ExprState *) lfirst(lcarg);
			fcinfo.arg[argno] = ExecEvalExpr(argstate, econtext,
										 fcinfo.argnull + argno, NULL);
			argno++;
		}
		
		if (funcstate->isAgg)
		{
			Datum *vals = econtext->ecxt_aggvalues;
			bool *isnulls = econtext->ecxt_aggnulls;

			funcstate->aggTransValue =
				invoke_agg_trans_func(funcstate->transfn,
									  funcstate->numargs,
									  funcstate->aggTransValue,
									  &(funcstate->aggNoTransValue),
									  &(funcstate->aggTransValueIsNull),
									  funcstate->aggTranstypeByVal,
									  funcstate->aggTranstypeLen,
									  &fcinfo, (void *)wstate,
									  wstate->transcontext,
									  econtext->ecxt_per_tuple_memory);
			
			if (OidIsValid(funcstate->finalfn_oid))
			{
				FunctionCallInfoData fcinfo;

				InitFunctionCallInfoData(fcinfo, &(funcstate->finalfn), 1,
										 (void *) wstate, NULL);
				fcinfo.arg[0] = funcstate->aggTransValue;
				fcinfo.argnull[0] = funcstate->aggTransValueIsNull;
				if (fcinfo.flinfo->fn_strict && funcstate->aggTransValueIsNull)
				{
					/* don't call a strict function with NULL inputs */
					vals[funcno] = (Datum) 0;
					isnulls[funcno] = true;
				}
				else
				{
					vals[funcno] = FunctionCallInvoke(&fcinfo);
					isnulls[funcno] = fcinfo.isnull;
				}
			}
			else
			{
				vals[funcno] = funcstate->aggTransValue;
				isnulls[funcno] = fcinfo.isnull;
			}
		}
		else /* ordinary window function */
		{
			InitFunctionCallInfoData(fcinfo, 
									 &funcstate->windowfn,
									 argno,
									 (void *) wrxstate, 
									 NULL);
			{
				Datum *vals = econtext->ecxt_aggvalues;
				bool *isnulls = econtext->ecxt_aggnulls;
				
				vals[funcno] = FunctionCallInvoke(&fcinfo);
				isnulls[funcno] = fcinfo.isnull;
			}
		}
	}
	
	/*
	 * Form the result tuple using ExecProject(), and return it.
	 */
	resultSlot = ExecProject(wstate->ps.ps_ProjInfo, &isDone);

	return resultSlot;
}

/* -----------------
 * ExecInitWindow
 *
 *	Creates the run-time information for the window node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
WindowState *
ExecInitWindow(Window *node, EState *estate, int eflags)
{
	WindowState *wstate;
	Plan *outerPlan;
	ExprContext  *econtext;
	int i, refno, numrefs;
	ListCell *lc;

	/* Check for unsupported flags. */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/* Create state structure. */
	wstate = makeWindowState(node, estate);

	/* Create expression context. */
	ExecAssignExprContext(estate, &wstate->ps);

#define WINDOW_NSLOTS 2

	/* Initialize tuple table. */
	ExecInitResultTupleSlot(estate, &wstate->ps);
	wstate->priorslot = ExecInitExtraTupleSlot(estate);

	/* Initialize child expressions.
	 *
	 * ExecInitExpr adds WindowRefExprState nodes it encounters to the 
	 * wrxstates list.
	 */
	wstate->ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) wstate);
	numrefs = list_length(wstate->wrxstates);

	/* Initialize child nodes. */
	outerPlan = outerPlan(node);
	outerPlanState(wstate) = ExecInitNode(outerPlan, estate, eflags);
	Assert( innerPlan(node) == NULL );

	/* Initialize result tuple type and projection info. */
	ExecAssignResultTypeFromTL(&wstate->ps);
	ExecAssignProjectionInfo(&wstate->ps, NULL);


	/* Precompute fmgr lookup data for partition key equality function. */
	if ( node->numPartCols > 0 )
	{
		wstate->eqfunctions =
			execTuplesMatchPrepare(ExecGetResultType(wstate->ps.lefttree),
								   node->numPartCols,
								   node->partColIdx);
	}
	else
		wstate->eqfunctions = NULL;
		
	/* Need equality functions for window ordering partial keys as well. */
	i = -1;
	foreach(lc, node->windowKeys)
	{
		WindowKey *key = (WindowKey *) lfirst(lc);
		WindowStatePerLevel lvl = &wstate->level_state[++i];
		lvl->eqfunctions = 
			execTuplesMatchPrepare(ExecGetResultType(wstate->ps.lefttree),
								   key->numSortCols,
								   key->sortColIdx);
	}
	
	/* Allocate result storage and working storage per window ref.
	 * (Later we may shrink this if we notice duplicate function
	 * calls, but allocate for worst case.)  Note that we borrow
	 * aggregate result storage, since there are no non-window
	 * aggregates that might use it.
	 */
	econtext = wstate->ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum*) palloc0(sizeof(Datum) * numrefs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numrefs);
	
	wstate->numfuncs = 0;
	wstate->func_state = palloc0(sizeof(WindowStatePerFunctionData) *  numrefs);
	

	/* Initialize per window ref (both ordinary and aggregate derived). */
	refno = -1;
	foreach(lc, wstate->wrxstates)
	{
		WindowRefExprState *wrxstate = (WindowRefExprState *) lfirst(lc);
		WindowRef *winref = (WindowRef *) wrxstate->xprstate.expr;
		WindowStatePerFunction funcstate;
		Oid			inputTypes[FUNC_MAX_ARGS];
		Form_pg_proc proform;
		HeapTuple	heap_tuple;
		ListCell   *lcarg;
		int			i, funcno;
		Oid			winOwner,
					winResType;
		bool		isAgg, 
					isWin,
					isSet;
		AclResult	aclresult;

		refno++; /* First one is 0 */

		/* Register the parent window state with the window ref state node. */
		wrxstate->refno = refno;
		wrxstate->windowstate = wstate;

		/* Look for a previous duplicate window function */
		for (funcno = 0; funcno < refno; funcno++)
		{
			WindowRef *w = wstate->func_state[funcno].wref;
			if (equal(winref, w) &&
				!contain_volatile_functions((Node *) w))
				break;
		}
		if (funcno < refno)
		{
			/* Found a match to an existing entry, so just mark it */
			wrxstate->funcno = funcno;
			continue;
		}

		/* No match so this window ref represents a new function and we
		 * need to fill in its function state.
		 */
		funcstate = &wstate->func_state[funcno];
		funcstate->wrxstate = wrxstate;
		funcstate->wref = winref;
		if ( winref->winlevel < wstate->numlevels )
			funcstate->wlevel = &wstate->level_state[winref->winlevel];
		else
			funcstate->wlevel = NULL;
		wrxstate->funcno = funcno;
		wstate->numfuncs = funcno + 1;
		
		/* May we call the function?  Initially all window functions are
		 * built-in, however, at this point we haven't learned whether
		 * we're working on an ordinary window function or an aggregate.
		 */
		aclresult = pg_proc_aclcheck(winref->winfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(winref->winfnoid));

		/* Collect information about the window function's pg_proc entry. */
		heap_tuple = SearchSysCache(PROCOID,
								  ObjectIdGetDatum(winref->winfnoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(heap_tuple))
			elog(ERROR, "cache lookup failed for window function proc %u",
				 winref->winfnoid);
		proform = (Form_pg_proc) GETSTRUCT(heap_tuple);
		
		isAgg = proform->proisagg;
		isWin = proform->proiswin;
		isSet = proform->proretset;
		winResType = proform->prorettype;
		winOwner = proform->proowner;
		
		ReleaseSysCache(heap_tuple);
		
		Assert( isAgg != isWin );
		Assert( !isSet );


		/*
		 * Get actual datatypes of the inputs.	These could be different from
		 * the declared input types, when the function accepts ANY, ANYARRAY
		 * or ANYELEMENT.
		 */
		i = 0;
		foreach(lcarg, winref->args)
		{
			inputTypes[i++] = exprType((Node *) lfirst(lcarg));
		}
		funcstate->numargs = list_length(winref->args);
	
		funcstate->isAgg = isAgg;
				
		/* The rest depends on the type (agg-derived or ordinary). */
		if ( isAgg )
		{
			HeapTuple agg_tuple;
			Form_pg_aggregate aggform;
#define NUM_AGG_FNS_TO_CHECK 4
			int c;
			Oid to_check[NUM_AGG_FNS_TO_CHECK];
			Oid aggtranstype;
			Expr *transfnexpr,
				 *finalfnexpr,
				 *invtransfnexpr,
				 *invprelimfnexpr;
			Datum textInitVal;
			
			/* XXX In reality, we shouldn't see this condition. The planner
			 *     shouldn't produce it.  However, during development, this
			 *     run-time check compensates for the poor state of planning.
			 */
			if ( winref->winlevel >= wstate->numlevels )
				elog(ERROR,"Window function aggregation over entire partition not supported yet.");

			agg_tuple = SearchSysCache(AGGFNOID,
									   ObjectIdGetDatum(winref->winfnoid),
									   0, 0, 0);
			if (!HeapTupleIsValid(agg_tuple))
				elog(ERROR, "cache lookup failed for aggregate %u",
					 winref->winfnoid);
			aggform = (Form_pg_aggregate) GETSTRUCT(agg_tuple);

			/* Get the implementation functions and related information
			 * we need to handle aggregate-derived functions.
			 *
			 * TODO - Omit ones we're not interested in.
			 */
			to_check[0] = funcstate->transfn_oid = aggform->aggtransfn;
			to_check[1] = funcstate->finalfn_oid = aggform->aggfinalfn;
			to_check[2] = funcstate->invtransfn_oid = aggform->agginvtransfn;
			to_check[3] = funcstate->invprelimfn_oid = aggform->agginvprelimfn;
			
			
			/* Check that the aggregate owner has needed permissions. */
			for ( c = 0; c < NUM_AGG_FNS_TO_CHECK; c++ )
			{
				if ( ! OidIsValid(to_check[c]) )
					continue;
				aclresult = pg_proc_aclcheck(to_check[c], winOwner, ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, ACL_KIND_PROC,
								   get_func_name(to_check[c]));
			}
			
			aggtranstype = resolve_polymorphic_transtype(aggform->aggtranstype,
														 winref->winfnoid,
														 inputTypes);
			/* 
			 * Build expression trees using actual argument & result types.
			 * c.f., ExecInitAgg(). We should really reduce this to a 
			 * few common functions.
			 */
			build_aggregate_fnexprs(inputTypes, funcstate->numargs,
									aggtranstype, winref->restype,
									funcstate->transfn_oid,
									funcstate->finalfn_oid,
									funcstate->invtransfn_oid,
									funcstate->invprelimfn_oid,
									&transfnexpr, &finalfnexpr,
									&invtransfnexpr, &invprelimfnexpr);

			fmgr_info(funcstate->transfn_oid, &funcstate->transfn);
			funcstate->transfn.fn_expr = (Node *)transfnexpr;

			if (OidIsValid(funcstate->finalfn_oid))
			{
				fmgr_info(funcstate->finalfn_oid, &funcstate->finalfn);
				funcstate->finalfn.fn_expr = (Node *) finalfnexpr;
			}
			if (OidIsValid(funcstate->invtransfn_oid))
			{
				fmgr_info(funcstate->invtransfn_oid, &funcstate->invtransfn);
				funcstate->invtransfn.fn_expr = (Node *) invtransfnexpr;
			}
			if (OidIsValid(funcstate->invprelimfn_oid))
			{
				fmgr_info(funcstate->invprelimfn_oid, &funcstate->invprelimfn);
				funcstate->invprelimfn.fn_expr = (Node *) invprelimfnexpr;
			}

			/*
			 * TODO: add invtransfn
			 */
			get_typlenbyval(winref->restype,
							&funcstate->resulttypeLen,
							&funcstate->resulttypeByVal);
			get_typlenbyval(aggtranstype,
							&funcstate->aggTranstypeLen,
							&funcstate->aggTranstypeByVal);

			/*
			 * initval is potentially null, so don't try to access it as a 
			 * struct field. Must do it the hard way with SysCacheGetAttr.
		 	*/
			textInitVal = SysCacheGetAttr(AGGFNOID, agg_tuple,
										  Anum_pg_aggregate_agginitval,
										  &funcstate->aggInitValueIsNull);

			if (funcstate->aggInitValueIsNull)
				funcstate->aggInitValue = (Datum) 0;
			else
				funcstate->aggInitValue = GetAggInitVal(textInitVal,
													    aggtranstype);

			/*
			 * If the transfn is strict and the initval is NULL, make sure input
			 * type and transtype are the same (or at least binary-compatible), so
			 * that it's OK to use the first input value as the initial
			 * aggTransValue.  This should have been checked at agg definition time,
			 * but just in case...
		 	*/
			if (funcstate->transfn.fn_strict && funcstate->aggInitValueIsNull)
			{
				if (funcstate->numargs < 1 ||
					!IsBinaryCoercible(inputTypes[0], aggtranstype))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("aggregate %u needs to have compatible "
									"input type and transition type",
									winref->winfnoid)));
			}
			ReleaseSysCache(agg_tuple);
		}
		else if ( isWin )
		{
			HeapTuple win_tuple;
			Form_pg_window winform;
			Oid windowfn_oid;
			Const *refptr;
			ExprState *xtrastate;
			
			win_tuple = SearchSysCache(WINFNOID,
									   ObjectIdGetDatum(winref->winfnoid),
									   0, 0, 0);
			if (!HeapTupleIsValid(win_tuple))
				elog(ERROR, "cache lookup failed for window function %u",
					 winref->winfnoid);
			winform = (Form_pg_window) GETSTRUCT(win_tuple);

			/* Lookup ordinary window implementation function info.
			 */			
			switch ( winref->winstage)
			{
			case WINSTAGE_IMMEDIATE:
			case WINSTAGE_ROWKEY:
				windowfn_oid = winform->winfunc;
				break;
			case WINSTAGE_PRELIMINARY:
				windowfn_oid  = winform->winprefunc;
				break;
			}
			Assert(OidIsValid(windowfn_oid));

			/* Check that window function owner has permission to call the
			 * implementation function. 
			 *
			 * XXX initially all implementation functions are builtin and 
			 *     anyone can call them, but good to follow form.
			 */
			aclresult = pg_proc_aclcheck(windowfn_oid, 
										 winOwner, 
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_PROC,
							   get_func_name(windowfn_oid));
			
			ReleaseSysCache(win_tuple);
			
			funcstate->windowfn_oid = windowfn_oid;
			
			/* Inject extra argument. */
			refptr = makeNode(Const);
			refptr->consttype = INTERNALOID;
			refptr->constlen = 4;
			refptr->constvalue = PointerGetDatum(wrxstate);
			refptr->constisnull = false;
			refptr->constbyval = true;
			
			xtrastate = ExecInitExpr((Expr *) refptr, (PlanState *) wstate);
			
			wrxstate->args = lcons(xtrastate, wrxstate->args);
			fmgr_info(funcstate->windowfn_oid, &funcstate->windowfn);			
		}
		else
		{
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("inappropriate use of function as window function")));

		}
	} /* next WindowRefExprState */
	
	initializePartition(wstate);
	
	return wstate;
}


int
ExecCountSlotsWindow(Window *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		WINDOW_NSLOTS;
}

void
ExecEndWindow(WindowState *node)
{
	/*  Free the exprcontext */
	ExecFreeExprContext(&node->ps);

	/* clean out the tuple table */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/* shut down subplans */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanWindow(WindowState *node, ExprContext *exprCtxt)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("Illegal rescan of window node: invalid plan.")));
}

/*
 * window_dummy - dummy execution routine for window functions
 *
 * This function is listed as the implementation (prosrc field) of pg_proc
 * entries for window functions.  Its only purpose is to throw an error
 * if someone mistakenly executes such a function in the normal way.
 */
Datum
window_dummy(PG_FUNCTION_ARGS)
{
	elog(ERROR, "window function %u called as normal function",
		 fcinfo->flinfo->fn_oid);
	return (Datum) 0;			/* keep compiler quiet */
}

/*
 *
 */
Datum
mpp_execution_segment(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(Gp_segment);
}


/*
 * ROW_NUMBER() OVER (...) --> BIGINT
 *
 * Implement ROW_NUMBER for the given window state:
 *
 * row_number_immed(internal) --> bigint
 */
Datum
row_number_immed(PG_FUNCTION_ARGS)
{
	WindowRefExprState *ref_state = (WindowRefExprState *) PG_GETARG_POINTER(0);
	
	int64 result = 1 + ref_state->windowstate->row_index;

	PG_RETURN_INT64(result);
}


/*
 *  RANK() OVER (... ORDER BY s) --> BIGINT 
 *
 * Implement RANK for the given WindowRefExprState.
 *
 * rank_immed(internal) --> bigint
 */
Datum
rank_immed(PG_FUNCTION_ARGS)
{
	int64 result;
	WindowRefExprState *ref_state = (WindowRefExprState *) PG_GETARG_POINTER(0);
	WindowState *window_state = ref_state->windowstate;
	WindowRef *ref = (WindowRef *)ref_state->xprstate.expr;
	WindowStatePerLevel level_state = &window_state->level_state[ref->winlevel];
	
	/* Don't advance prior_rank here, let the framework do this. */
	
	result = level_state->rank;
	
	PG_RETURN_INT64(result);
}


/*
 *  DENSE_RANK() OVER (... ORDER BY s) --> BIGINT
 *
 * Implement DENSE_RANK for the given WindowRefExprState.
 *
 * dense_rank_immed(internal) --> bigint
 */
Datum
dense_rank_immed(PG_FUNCTION_ARGS)
{
	int64 result;
	WindowRefExprState *ref_state = (WindowRefExprState *) PG_GETARG_POINTER(0);
	WindowState *window_state = ref_state->windowstate;
	WindowRef *ref = (WindowRef *)ref_state->xprstate.expr;
	WindowStatePerLevel level_state = &window_state->level_state[ref->winlevel];
	
	/* Don't advance prior_dense_rank here, let the framework do this. */
	
	result = level_state->dense_rank;
	
	PG_RETURN_INT64(result);
}


/*
 * PERCENT_RANK() OVER (... ORDER BY s) -- FLOAT8
 *
 * Implement rank for the given WindowRefExprState.
 *
 * rank_immed(internal) --> bigint
 * percent_rank_final(bigint,bigint) --> float8 
 */
Datum
percent_rank_final(PG_FUNCTION_ARGS)
{
	int64 arg1 = PG_GETARG_INT64(0); /* rank in partition */
	int64 arg2 = PG_GETARG_INT64(1); /* partition row count */
	double result;
	
	if (arg1 < 1 || arg2 < 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("arguments invvalid or inconsistent"),
				 errhint("inappropriate call to window function implementation")));
	}
	else if ( arg1 == 1 && arg2 == 1 )
	{
		result = 1.0; /* or is it 0.0? */
	}
	else
	{
		/* Do division in double, then check for overflow */
		result = (double) (arg1 - 1) / (double) (arg2- 1);	
		if ( result > DBL_MAX )
		{
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value out of range: overflow")));
		}
	}
	
	PG_RETURN_FLOAT8(result);
}


/*
 *  CUME_DIST() OVER (... ORDER BY s) --> FLOAT8
 *
 * Implement CUME_DIST for the given WindowRefExprState.
 *
 * cume_dist_prelim(internal) --> bigint
 * cume_dist_final(bigint,bigint) --> float8
 */
Datum
cume_dist_prelim(PG_FUNCTION_ARGS)
{
	int64 result;
	WindowRefExprState *ref_state = (WindowRefExprState *) PG_GETARG_POINTER(0);
	WindowState *window_state = ref_state->windowstate;
	WindowRef *ref = (WindowRef *)ref_state->xprstate.expr;
	WindowStatePerLevel level_state = &window_state->level_state[ref->winlevel];
	
	result = level_state->prior_non_peer_count + level_state->peer_count;

	PG_RETURN_INT64(result);
}

Datum
cume_dist_final(PG_FUNCTION_ARGS)
{
	int64 arg1 = PG_GETARG_INT64(0); /* prior_non_peer_count + peer_count */
	int64 arg2 = PG_GETARG_INT64(1); /* partition row count */
	double result;
	
	if (arg1 < 1 || arg2 < 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("arguments invalid or inconsistent"),
				 errhint("inappropriate call to window function implementation")));
	}
	else if ( arg1 == 1 && arg2 == 1 )
	{
		result = 1.0; /* or is it 0.0? */
	}
	else
	{
		/* Do division in double, then check for overflow */
		result = (double) arg1 / (double) arg2;	
		if ( result > DBL_MAX )
		{
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value out of range: overflow")));
		}
	}
	
	PG_RETURN_FLOAT8((float8)result);
}



/*
 *  NTILE(BIGINT) OVER (... ORDER BY s) --> BIGINT
 *
 * Implement NTILE for the given WindowRefExprState.
 *
 * ntile_prelim_int(internal,int) --> bigint[]
 * ntile_prelim_bigint(internal,bigint) --> bigint[]
 * ntile_prelim_numeric(internal,numeric) --> bigint[]
 * ntile_final(bigint[],bigint) --> bigint
 */

/* Helper defined in src/backend/util/adt/numeric.c. */
extern int64 numeric_to_pos_int8_trunc(Numeric num);

/* Helper. */
static ArrayType *
do_ntile_prelim(WindowRefExprState *ref_state, int64 num_tiles)
{
	/* 
	 * Pack row_index from state and the argument num_tiles into a 
	 * two-element array of type bigint.
	 */

	Datum work[2];

	WindowState *window_state = ref_state->windowstate;
	
	if ( num_tiles < 0 )
		ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("argument value out of range"),
			 errhint("NTILE expects a positive integer argument.")));
		
	work[0] = Int64GetDatumFast(num_tiles); /* ntile argument */
	work[1] = Int64GetDatumFast(window_state->row_index);
		
	return construct_array(work, 2, INT8OID, 8, false, 'd');
}
 
Datum
ntile_prelim_int(PG_FUNCTION_ARGS)
{
	WindowRefExprState *rstate = (WindowRefExprState *) PG_GETARG_POINTER(0);
	int64 ntile_arg = (int64)PG_GETARG_INT32(1);

	PG_RETURN_ARRAYTYPE_P(do_ntile_prelim(rstate, ntile_arg));
}
 
Datum
ntile_prelim_bigint(PG_FUNCTION_ARGS)
{
	WindowRefExprState *rstate = (WindowRefExprState *) PG_GETARG_POINTER(0);
	int64 ntile_arg = PG_GETARG_INT64(1);

	PG_RETURN_ARRAYTYPE_P(do_ntile_prelim(rstate, ntile_arg));
}

Datum
ntile_prelim_numeric(PG_FUNCTION_ARGS)
{
	int64 num_tiles;
	WindowRefExprState *rstate = (WindowRefExprState *) PG_GETARG_POINTER(0);
	Numeric ntile_arg =  PG_GETARG_NUMERIC(1);
	
	/* Truncate ntile_arg to int8, put in num_tiles. */
	num_tiles = numeric_to_pos_int8_trunc(ntile_arg);
	if ( num_tiles < 1 )
		num_tiles = 0; /* Set value out of range to trigger error report. */

	PG_RETURN_ARRAYTYPE_P(do_ntile_prelim(rstate, num_tiles));
}

/*
 * To implement the semantics of NTILE correctly, tuples in the same bucket
 * must be clustered together according to the input ordering -- 
 * i.e., 1, 1, 1, 2, 2, 2, 3, 3, 4, 4.
 *
 * It's tempting to just do ((N - 1) mod M) + 1 but this will not meet
 * our requirement. Alternatively, we could just divide the partition
 * row count into N buckets and see if the row index fit into an individual
 * bucket. That would give us clustered results but it wouldn't handle the
 * situation where the number of rows in the partition did not divide evenly
 * into the number of buckets (notice that in the above example, these
 * larger-by-one buckets are the leading buckets).
 *
 * So, instead we identify the threshold (prefix_size) to which buckets will
 * have one more row and we divide the rows between them (row_index/max_size).
 * After the threshold, the next bucket will be number 1 + spares and
 * we divide the values evenly between each bucket.
 *
 * Note that row_index counts from 0.
 */

Datum
ntile_final(PG_FUNCTION_ARGS)
{
	int64 result;
	Datum *work;
	int len;
	int64 row_index, num_tiles, min_size, max_size, prefix_size, spares;
	
	ArrayType *pair = PG_GETARG_ARRAYTYPE_P(0); /* row_index, num_tiles */
	int64 partition_row_count = PG_GETARG_INT64(1); /* partition row count */
	
	/* we expect the input to be bigint[2] */
	deconstruct_array(pair, INT8OID, 8, false, 'd', &work, NULL, &len);
	if (len != 2)
		elog(ERROR, "expected 2-element int8 array");

	num_tiles = DatumGetInt64(work[0]);
	row_index = DatumGetInt64(work[1]);
	min_size = partition_row_count / num_tiles;

	spares = partition_row_count % num_tiles;
	max_size = min_size + 1;
	prefix_size = spares * max_size;
	
	if (row_index < prefix_size)
		result = 1 + row_index / max_size;
	else
		result = 1 + spares + 
			(row_index - prefix_size) / min_size;

	PG_RETURN_INT64(result);
}
