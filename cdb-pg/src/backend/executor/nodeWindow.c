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
#include "miscadmin.h"

/* XXX include list is speculative -- bhagenbuch */
#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_window.h"
#include "cdb/cdbvars.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeWindow.h"
#include "lib/dllist.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/debugutils.h"
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
	bool		need_peer_count;

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
	bool				allowframe;
	int					numargs; /* explicit, caller-supplied args */
	int16				resulttypeLen;
	bool				resulttypeByVal;
	
	/* Ordinary window function */
	Datum				win_value;
	bool				win_value_is_null;
	Oid					windowfn_oid;
	FmgrInfo			windowfn;
	bool				need_peercount;
	
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

	/* 
	 * When calculating RANGE frames, we continue to emit the same
	 * transition value for all peers. We keep the peer field in 
	 * peer_value to test inside the framing code.
	 */
	Datum 				peer_value;

	bool				frame_tuple_processed;
	/* frame does not require buffering and complexity of invokeWindowFuncs() */
	bool				trivial_frame;
} WindowStatePerFunctionData;

typedef struct WindowFrameStateData
{
	/* if the user didn't specify a frame, we use the default */
	bool default_frame;

	/* 
	 * Frame is contradictory and no tuples will fall into the frame, for
	 * example: BETWEEN 10 FOLLOWING AND 10 PRECEDING.
	 */
	bool				empty_frame;

	/* state for rows frame is simple (for now) */
	long int trail_rows;
	long int lead_rows;

	/* state for range frame is more complex */
	Datum trail_range;
	Datum lead_range;

	/* 
	 * RANGE frames also need to do addition and subtraction of the above
	 * for each new row so we must lookup and remember the relevant functions.
	 */
	FmgrInfo add_fn;
	FmgrInfo sub_fn;

	/*
	 * We also need less than and greater than.
	 */
	FmgrInfo gt_fn;
	FmgrInfo lt_fn;

	/* state for frame edges */
	Datum trail_edge_d;
	bool trail_edge_set;
	
	Datum lead_edge_d;
	bool lead_edge_set;

	Datum prev_trail_d;
	bool prev_trail_set;
	
	Datum prev_lead_d;
	bool prev_lead_set;
	/* 
	 * For RANGE frames, all peers must emit the same result. We store the
	 * peer, as it store last time we emitted a row, in last_peer_value. We
	 * store the value to be emitted for all peers in peer_result.
	 *
	 * These values are uninitialised at the time we process the first row and
	 * so we look at row_index in the WindowState to handle that special case.
	 *
	 * Be careful to free saved datums and to copy them in the relevant
	 * context!
	 */
	Datum last_peer_value;
	Datum peer_result;
} WindowFrameStateData;

/*
 * We need to buffer tuples and meta data inside the framing code. We use
 * this structure for that purpose.
 */
typedef struct WindowTuple
{
	HeapTuple 	tup;
	bool		part_break;
	int			min_key_break;
} WindowTuple;

#define FRAME_TRAIL_ROWS 	0
#define FRAME_TRAIL_RANGE	1
#define FRAME_LEAD_ROWS		2
#define FRAME_LEAD_RANGE	3

#define EDGE_IS_BOUND(e) \
	(e->kind == WINDOW_BOUND_PRECEDING || \
	 e->kind == WINDOW_BOUND_FOLLOWING)

#define FRAME_NEEDS_FILL(wstate, key, funcstate) \
	((wstate->row_index == 0 && EDGE_IS_BOUND(key->frame->lead)) \
	 || FUNC_HAS_MAN_FRAME(funcstate))
	
#define FRAME_IS_COMPLEX(f, fs) (!(f->trivial_frame || fs->empty_frame))

#define FUNC_HAS_MAN_FRAME(f) \
	(funcstate->isAgg && !OidIsValid(funcstate->invtransfn_oid))

#define ABS(x) (x < 0 ? -x : x)
#define MIN(x, y) (x =< y ? x : y)

static WindowState *makeWindowState(Window *window, EState *estate);
static void initializePartition(WindowState *wstate);
static void advanceKeyLevelState(WindowState *wstate, int min_level); 
static void resetPerFunctionAggStates(WindowState *wstate, Index minlevel);
static void resetManagedFrameAggStates(WindowState *wstate);
static TupleTableSlot *nextRow(WindowState *wstate, ExprContext *econtext);
static WindowTuple *fetchTupleThroughBuf(WindowState *wstate);
static void moveFrameBuf(WindowState *wstate, long int ntuples);
static void freeFrameBuf(WindowState *wstate);
static void init_frames(WindowState *wstate);

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
	
	elog(DEBUG1, "initializing partition");
	
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
		level_state->peer_index = 0;
		level_state->peer_count = -1;
		level_state->rank = 1;
		level_state->dense_rank = 1;
		level_state->prior_rank = 1;
		level_state->prior_dense_rank = 1;
		
		/* TODO per-key-level buffer management reinitialization */
	}
	
	/* Per-partition buffer management reinitialzation. */
	freeFrameBuf(wstate);
	wstate->trail = DLNewList();
	wstate->lead = DLNewList();
	wstate->frame_trail_edge = 0;
	wstate->frame_lead_edge = 0;
	
	if (wstate->numlevels > 0)
		resetPerFunctionAggStates(wstate, 0);
}


static void
processPeerCounts(WindowState *wstate, int min_level, 
				  TupleTableSlot *cur_slot, MemoryContext eval_ctx)
{
	int i;
	uint64 peer_count = 1;
	long int nfetches = 0;
	Window *window = (Window *)wstate->ps.plan;
	TupleTableSlot *priorslot = NULL;
	WindowTuple *priorwt = NULL;
	char *buf;
	bool seen_null = false;
	
	elog(DEBUG1, "called %s", PG_FUNCNAME_MACRO);


#if 0 /* temporarily disabled */	
	Assert(ExecSupportsMarkRestore(nodeTag(outerPlan->plan)));
	ExecMarkPos(outerPlan);
#endif
	
	buf = tup2str(cur_slot);
	elog(DEBUG1, "first = %s", buf);
	pfree(buf);

	/* 
	 * We start from the upper most level because peer counts will get
	 * larger as we descend.
	 */
	for (i = wstate->numlevels - 1; i >= min_level; i--)
	{
		WindowStatePerLevel level_state = &wstate->level_state[i];

		/*
	 	 * If there is a function at this level which needs a peer count, we
	 	 * must pull tuples from the outer plan, incrementing the peer_count,
	 	 * until there is a key change.
	 	 */

		if (level_state->need_peer_count)
		{
			WindowKey  *key;
//			char *buf;

			/* we already have one peer in cur_slot, so adjust */
			Assert(level_state->peer_count == -1);
			level_state->peer_count = peer_count;

#if 0
			elog(DEBUG1, "level %i needs peer", i);
			buf = tup2str(cur_slot);
			elog(DEBUG1, "first peer: %s", buf);
			pfree(buf);
#endif
			if (!seen_null)
			{
				Assert(list_length(window->windowKeys) > i);

				key = (WindowKey *)list_nth(window->windowKeys, i);


				for (;;)
				{
					bool match;
					TupleTableSlot *slot;
					WindowTuple *wt;
	
					if (priorslot)
					{
						slot = priorslot;
						priorslot = NULL;
						wt = priorwt;
						priorwt = NULL;
					}
					else
					{
						//slot = ExecProcNode(outerPlan);
						wt = fetchTupleThroughBuf(wstate);
						slot = wstate->curslot;
						nfetches++;
					}
	
					if (!PointerIsValid(wt) || TupIsNull(slot))
					{
						priorslot = slot;
						priorwt = wt;
						nfetches--;
						seen_null = true;
						elog(DEBUG1, "slot is null");
						break;
					}
					else
					{
					}
	
					/* XXX: shouldn't this be priorslot instead of cur_slot */
					match = execTuplesMatch(cur_slot, slot, 
											key->numSortCols, key->sortColIdx,
											level_state->eqfunctions,
											eval_ctx);
	
					if (!match)
					{
						priorslot = slot;
						break;
					}
	
					peer_count++;
				}
			}
			level_state->peer_count = peer_count;
			
	//		peer_count++; /* update to reflect the tuple we just pulled */

			elog(DEBUG1, "peer count = %lld", (long long)level_state->peer_count);
		}
	}
	elog(DEBUG1, "total peer count %lld, %li", (long long)peer_count, nfetches);
	if (nfetches)
	{
	//	ExecRestrPos(outerPlan);
		elog(DEBUG1, "rewinding %li", nfetches);
		moveFrameBuf(wstate, -nfetches);
	}
}

static void
freeFrameBuf(WindowState *wstate)
{
	/* TODO: free manually, to free HeapTuples */
	if (wstate->trail)
	{
		DLFreeList(wstate->trail);
		wstate->trail = NULL;
	}
	if (wstate->lead)
	{
		DLFreeList(wstate->lead);
		wstate->lead = NULL;
	}
}
	
/*
 * Fetch a tuple in a frame buffer aware way. If set_cur_row is true, the 
 * row fetched is the current row the window is focussed around.
 */
static WindowTuple *
fetchTupleThroughBuf(WindowState *wstate)
{
	WindowTuple *wt;
	Window *window = (Window *)wstate->ps.plan;	
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	//MemoryContext oldcxt;

//	oldcxt = MemoryContextSwitchTo(QueryContext);

	if (!DLGetHead(wstate->lead))
	{
		TupleTableSlot *slot = ExecProcNode(outerPlanState(wstate));	
		WindowTuple *prev_wt = NULL;

		if (TupIsNull(slot))
		{
	//		MemoryContextSwitchTo(oldcxt);
			return NULL;
		}

		if (wstate->spare->tts_tupleDescriptor == NULL)
		{
			ExecSetSlotDescriptor(wstate->spare, 
								  slot->tts_tupleDescriptor);
		}

		wt = palloc(sizeof(WindowTuple) * 10);
		
		/* get prior tuple */
		if (DLGetTail(wstate->trail))
			prev_wt = (WindowTuple *) DLE_VAL(DLGetTail(wstate->trail));
#if 0
			tup = (HeapTuple) DLE_VAL(DLGetTail(wstate->trail));
#endif
		
		wt->min_key_break = -1;		
		
		if (PointerIsValid(prev_wt))
#if 0
		if (PointerIsValid(tup))
#endif
		{
#if 0
			save = ExecFetchSlotTuple(wstate->priorslot);
			ExecClearTuple(wstate->priorslot);
			ExecStoreTuple(prev_wt->tup, wstate->priorslot, 
#endif
			ExecStoreTuple(prev_wt->tup, wstate->spare,
						   InvalidBuffer, false);

			wt->part_break = (window->numPartCols && 
							  (!execTuplesMatch(wstate->spare, slot, 
								window->numPartCols, window->partColIdx, 
								wstate->eqfunctions,
								econtext->ecxt_per_tuple_memory)));
		
			if (!wt->part_break)
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
				
					if (match)
					{
						match = execTuplesMatch(wstate->spare, slot, 
												key->numSortCols, 
												key->sortColIdx,
												ls->eqfunctions,
												econtext->ecxt_per_tuple_memory);
						elog(DEBUG1, "here, match is %s", match ? "true" : "false");
						if (!match)
							wt->min_key_break = level;
					}

					level++;
				}
			}
#if 0
			ExecClearTuple(wstate->priorslot);
			ExecStoreTuple(save, wstate->priorslot, InvalidBuffer, false);
#endif
		}
		else
		{
			/* must be the first tuple. */
			wt->part_break = window->numPartCols > 0;
			wt->min_key_break = -1;
			wstate->curslot = slot;
		}
	
		wstate->curslot = slot;
		wt->tup = ExecCopySlotTuple(wstate->curslot);
		DLAddTail(wstate->trail, DLNewElem(wt));
#if 0		
		tup = ExecCopySlotTuple(wstate->curslot);
		DLAddTail(wstate->trail, DLNewElem(tup));
#endif
		elog(DEBUG1, "%s: added to buf", PG_FUNCNAME_MACRO);
	}
	else
	{
		Dlelem *el = DLRemHead(wstate->lead);
		wt = (WindowTuple *)DLE_VAL(el);
		
		DLAddTail(wstate->trail, el);

		ExecStoreTuple(wt->tup, 
					   wstate->curslot, InvalidBuffer,
					   false);
#if 0
		tup = (HeapTuple)DLE_VAL(el);
		
		DLAddTail(wstate->trail, el);

		ExecClearTuple(wstate->curslot);
		ExecStoreTuple(tup, 
					   wstate->curslot, InvalidBuffer,
					   false);
		wt->part_break = false;
		wt->min_key_break = -1;
		wt->tup = tup;
#endif
	}

//	MemoryContextSwitchTo(oldcxt);
	return wt;
}

static void
moveFrameBuf(WindowState *wstate, long int ntuples)
{
	long int tup = 0;
	bool forward = ntuples >= 0 ? true : false;
	long int tupcount = forward ? ntuples : -ntuples;
	elog(DEBUG1, "moving from %li", ntuples);

	if (ntuples == 0)
		return;
	
	if (wstate->lead == NULL)
		wstate->lead = DLNewList();
	
	while (tupcount-- > 0)
	{
		Dlelem *el;
	   
		if (forward)
			el = DLRemHead(wstate->lead);
		else
			el = DLRemTail(wstate->trail);

		if (el == NULL)
			elog(ERROR, "edge is null, direction = %s, tups %li, moved %li",
				 forward ? "forward" : "back", ntuples, tup);

		if (forward)
			DLAddTail(wstate->trail, el);
		else
			DLAddHead(wstate->lead, el);

		tup++;
	}
	elog(DEBUG1, "moved %li", tup);
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
advanceKeyLevelState(WindowState *wstate, int min_level)
{
	WindowStatePerLevel level_state;
	int64 cur_row_index;
	int i;
	
	Assert(-1 <= min_level && min_level < wstate->numlevels);

	cur_row_index = 1 + wstate->row_index;

	for (i = 0; i < wstate->numlevels; i++)
	{
		level_state = &wstate->level_state[i];
		
		if (i >= min_level && min_level != -1)
		{
			/* The level data is still set for the previous row. 
			 * Remember the rank and dense_rank values.
			 */
			level_state->prior_rank = level_state->rank;
			level_state->prior_dense_rank = level_state->dense_rank;
		
		/* Advance level data for first peer in next group. */
#if 0
		elog(DEBUG1, "== prior %llu, index = %lli",
			 level_state->prior_non_peer_count, level_state->peer_index);
#endif
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
}

/*
 * Restore a given aggregate function's state to its initial value.
 */
static void
resetAggState(WindowState *wstate, WindowStatePerFunction funcstate)
{
	Assert(funcstate->isAgg);

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

/*
 * Aggregates which do not support inversion must be reset before processing
 * a new frame.
 */
static void
resetManagedFrameAggStates(WindowState *wstate)
{
	int funcno;

	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];

		/* only continue if we're framed */
		if (PointerIsValid(funcstate->wlevel))
		{
			WindowFrameState fs = 
				&wstate->frame_states[funcstate->wlevel->level];

			if (funcstate->isAgg && !OidIsValid(funcstate->invtransfn_oid) &&
				FRAME_IS_COMPLEX(funcstate, fs))
			{
				elog(DEBUG1, "resetting managed agg");
				resetAggState(wstate, funcstate);
			}
		}
	}
}

/* 
 * Reset the per-function aggregate states at or beyond the specified
 * key level.  This function should be called once per partition-start
 * and once per row that causes a key break.
 */
static void 
resetPerFunctionAggStates(WindowState *wstate, Index minlevel)
{
	int funcno;
	
	elog(DEBUG1, "saw keybreak at %u, resetting", minlevel);
	/* reset transition values for each aggregate */
	for ( funcno = 0; funcno < wstate->numfuncs; funcno++ )
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		
		if (funcstate->isAgg && funcstate->wlevel->level >= minlevel)
			resetAggState(wstate, funcstate);
	}
}

/*
 * Pull a row from the outer plan.
 */
static TupleTableSlot *
nextRow(WindowState *wstate, ExprContext *econtext)
{
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *priorTupleSlot = wstate->priorslot;
	int min_key_break = -1;
	WindowTuple *wt;

	/* Get the new current row into the outer tuple slot. */
	wt = fetchTupleThroughBuf(wstate);

	if (!PointerIsValid(wt))
		return NULL;

	/* If first call, initialize priorslot. */
	if (wstate->priorslot->tts_tupleDescriptor == NULL)
	{
		ExecSetSlotDescriptor(wstate->priorslot, 
							  wstate->curslot->tts_tupleDescriptor);
	}
	
	outerTupleSlot = wstate->curslot;

	/* At the first row of a partition, we're already initialised. */
	if (wstate->row_index >= 0)
	{
		if (wt->part_break)
			initializePartition(wstate);
		else
			advanceKeyLevelState(wstate, wt->min_key_break);
	}

	wstate->row_index++;
	
	/* 
	 * If we saw a key change, reset the aggregates. If we didn't, reset any
	 * aggregates with "managed frames".
	 */
	if (wt->min_key_break > 0)
		resetPerFunctionAggStates(wstate, wt->min_key_break);
	else
		resetManagedFrameAggStates(wstate);

	ExecCopySlot(priorTupleSlot, outerTupleSlot); /* for next time through */

	if (wt->min_key_break >= 0 || wstate->row_index == 0 /* first row */
		|| wt->part_break)
	{
		int level = min_key_break != -1 ? min_key_break : 0;
		processPeerCounts(wstate, level, priorTupleSlot,
						  econtext->ecxt_per_tuple_memory);
	}

	return priorTupleSlot;
}

static void
invert_tuple_from_trans(WindowStatePerFunction funcstate, WindowState *wstate,
					    ExprContext *econtext)
{
	WindowRefExprState *wrxstate = funcstate->wrxstate;
	FunctionCallInfoData fcinfo;
	ListCell   *lcarg;
	int			argno;

	funcstate->frame_tuple_processed = true;

	/* only aggs are inverted */
	if (!funcstate->isAgg || !OidIsValid(funcstate->invtransfn_oid))
		return;

	/* Evaluate function arguments, save in fcinfo. */

	/* If this is an agg, the first arg will be the trans value */
	argno = 1;
	
	foreach (lcarg, wrxstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(lcarg);
		fcinfo.arg[argno] = ExecEvalExpr(argstate, econtext,
										 fcinfo.argnull + argno, NULL);
		argno++;
	}
			
	funcstate->aggTransValue = 
		invoke_agg_trans_func(funcstate->invtransfn,
							  funcstate->numargs,
							  funcstate->aggTransValue,
							  &(funcstate->aggNoTransValue),
							  &(funcstate->aggTransValueIsNull),
							  funcstate->aggTranstypeByVal,
							  funcstate->aggTranstypeLen,
							  &fcinfo, (void *)wstate,
							  wstate->transcontext,
							  econtext->ecxt_per_tuple_memory);
}

static void
add_tuple_to_trans(WindowStatePerFunction funcstate, WindowState *wstate,
				   ExprContext *econtext)
{
	WindowRefExprState *wrxstate = funcstate->wrxstate;
	FunctionCallInfoData fcinfo;
	ListCell   *lcarg;
	int argno;
			
	funcstate->frame_tuple_processed = true;
	/* Evaluate function arguments, save in fcinfo. */
	
	/* If this is an agg, the first arg will be the trans value */
	if (funcstate->isAgg)
		argno = 1;
	else
		argno = 0;
	
	foreach (lcarg, wrxstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(lcarg);
		fcinfo.arg[argno] = ExecEvalExpr(argstate, econtext,
										 fcinfo.argnull + argno, NULL);
		argno++;
	}
	
	if (funcstate->isAgg)
	{
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
		
	}
	else /* ordinary window function */
	{
		/* XXX: fix me to stop temporarily in state */
		InitFunctionCallInfoData(fcinfo, 
								 &funcstate->windowfn,
								 argno,
								 (void *) wrxstate, 
								 NULL);
		{
			
			funcstate->win_value = FunctionCallInvoke(&fcinfo);
			funcstate->win_value_is_null = fcinfo.isnull;
		}
	}

}

static void
resetFrameEdgeState(WindowState *wstate)
{
	int level;
	Window *window = (Window *)wstate->ps.plan;

	for (level = 0; level < list_length(window->windowKeys); level++)
	{
		WindowFrameState fs = &wstate->frame_states[level];

		/* might not be ROWS, do it anyway */
		fs->trail_edge_set = false;
		fs->lead_edge_set = false;
	}
}

static void
resetFuncProcessed(WindowState *wstate)
{
	int funcno;

	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		funcstate->frame_tuple_processed = false;
	}
}

static void
invokeTrivialFuncs(WindowState *wstate)
{
	int funcno;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	econtext->ecxt_outertuple = wstate->priorslot;
	econtext->ecxt_scantuple = wstate->priorslot;

	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		WindowFrameState fs = NULL;

		if (PointerIsValid(funcstate->wlevel))
			fs = &wstate->frame_states[funcstate->wlevel->level];
		
		if (!fs || !FRAME_IS_COMPLEX(funcstate, fs))
			add_tuple_to_trans(funcstate, wstate, econtext);
	}
}

/*
 * We must evaluate functions in a 'frame aware' way.
 *
 * UNBOUNDED PRECEDING frame edges are managed by passing the transition value 
 * from the last call to the function. In this way, we trivially evaluate
 * all rows in the frame which have already passed. It gets more complex
 * for RANGE UNBOUNDED PRECEDING frames because all peers must emit the same
 * value -- so, we remember this in our state.
 *
 * Bounded PRECEDING and FOLLOWING edges are harder. We have two cases.
 * 
 * With ROWS bounded frames, we must detect our position from the current
 * rows. If we are earlier on in the tuple count than the frame edge, those
 * tuples must be removed from the the transition value using the inversion
 * method for the function. If one doesn't exist, we do something even more
 * complex. See below. Once in the frame, we only care about the leading
 * frame edge (it could be the current row or some rows from it). So, we must
 * be careful about that.
 *
 * With RANGE bounded frames, we determine the edge with the equality
 * function(s) of the key level. Before and after the frame edges, we do the 
 * same as for ROWS frames but remember, we must emit the same value for
 * all peers.
 *
 * If inversion functions do exist for a function, we must evaluate the frame
 * every single time it moves.
 */
static void
invokeWindowFuncs(WindowState *wstate)
{	
	long int pos;
	//long int trail_edge = 0;
	//long int lead_edge = 0;
	Window *window = (Window *)wstate->ps.plan;
	int funcno;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	bool found_lower_edge = false;

	elog(DEBUG1, "======= START FRAME PROCESS =====");
	elog(DEBUG1, "row index %lli, trail edge = %li", (long long int) wstate->row_index,
		 wstate->frame_trail_edge);
	
	/*
	 * Process any functions with trivial frames.
	 */
#if 0
	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
			
		if (!FRAME_IS_COMPLEX(funcstate))
		{
			elog(DEBUG1, "processing trivial frame");
			add_tuple_to_trans(funcstate, wstate, econtext);
		}
	}
#endif
	invokeTrivialFuncs(wstate);
	
	resetFrameEdgeState(wstate);

	/* rewind to the beginning of the frame */
	pos = wstate->frame_trail_edge;
	moveFrameBuf(wstate, pos - 1);
	
	for (;;)
	{
		WindowTuple *wt = fetchTupleThroughBuf(wstate); 
		bool passed_all_edges = true;
		TupleTableSlot *frameslot;

		frameslot = wstate->curslot;

		if (!PointerIsValid(wt) || TupIsNull(frameslot))
			break;
		else if (wt->part_break == true)
			break;

		{
#if 0
			char *buf;

			buf = tup2str(frameslot);
			elog(DEBUG1, "processing framed tuple %s", buf);
			pfree(buf);
#endif
		}
		
		/* reset frame_tuple_processed for all funcs */
		resetFuncProcessed(wstate);

		econtext->ecxt_outertuple = frameslot;
		econtext->ecxt_scantuple = frameslot;	
		
		/*
		 * We must evaluate bound frames first.
		 */

		if (wstate->frame_levels[FRAME_TRAIL_ROWS] != NIL)
		{
			ListCell *lc;

			foreach(lc, wstate->frame_levels[FRAME_TRAIL_ROWS])
			{
				long int lvl = lfirst_int(lc);
				long int rows_param;
				WindowFrameState fs;
				WindowKey *key;
				
				key = (WindowKey *)list_nth(window->windowKeys, lvl);
				if (!key->frame)
				{
					elog(DEBUG1, "key %li has null window frame", lvl);
					continue;
				}

				fs = &wstate->frame_states[lvl];

				if (key->frame->trail->kind == WINDOW_CURRENT_ROW)
					rows_param = 0L;
				else if (key->frame->trail->kind == WINDOW_UNBOUND_PRECEDING)
				{
					/* This will be picked up by the other edge of the frame */
					continue;
				}
				else
				{
					rows_param = fs->trail_rows;

					if (key->frame->trail->kind == WINDOW_BOUND_PRECEDING)
						rows_param = -rows_param;

				}
				
				elog(DEBUG1, "param = %li, pos = %li", rows_param, pos);
				for (funcno = 0; funcno < wstate->numfuncs; funcno++)
				{
					WindowStatePerFunction funcstate = 
						&wstate->func_state[funcno];

					if (funcstate->wlevel->level != lvl || 
						funcstate->frame_tuple_processed ||
						!FRAME_IS_COMPLEX(funcstate, fs))
						continue;

					elog(DEBUG1, "found my level at %li", lvl);

					if (pos < rows_param - 1)
					{
						/*
						 * If we haven't filled the frame yet OR if an
						 * EXCLUDING clause means the row will have never
						 * appeared in the frame, do not do anything with
						 * the row.
						 */
						funcstate->frame_tuple_processed = true;
					}
					else if (rows_param - 1 == pos)
					{
						/* 
						 * Don't invert a datum from a transition value until 
						 * we've actually initialised the trasition value.
						 * We wont be inverting any values at the time we 
						 * process the first row of the frame.
						 */
						if (wstate->row_index == 0)
							funcstate->frame_tuple_processed = true;
						else if (wstate->row_index == 1 && 
								 key->frame->exclude == WINDOW_EXCLUSION_CUR_ROW)
						{
							/* we never saw the first row, so don't invert */
							funcstate->frame_tuple_processed = true;
						}
						else
						{
							elog(DEBUG1, "inverting value");
							if (OidIsValid(funcstate->invtransfn_oid))
								invert_tuple_from_trans(funcstate, wstate,
														econtext);
							else
								elog(DEBUG1, "manage frame manually");
						}

						if (!found_lower_edge)
						{
							wstate->frame_trail_edge = pos;
							found_lower_edge = true;
						}
					}
				}
			}
		}

		if (wstate->frame_levels[FRAME_LEAD_ROWS] != NIL)
		{
			ListCell *lc;

			foreach(lc, wstate->frame_levels[FRAME_LEAD_ROWS])
			{
				long int lvl = lfirst_int(lc);
				long int rows_param;
				WindowKey *key;
				WindowFrameState fs;

				key = (WindowKey *)list_nth(window->windowKeys, lvl);
				
				if (!key->frame)
				{
					elog(DEBUG1, "key %li has null window frame", lvl);
					continue;
				}

				fs = &wstate->frame_states[lvl];
				
				if (key->frame->lead->kind == WINDOW_CURRENT_ROW)
					rows_param = 0L;
				else if (key->frame->lead->kind == WINDOW_BOUND_PRECEDING ||
						 key->frame->lead->kind == WINDOW_BOUND_FOLLOWING)
				{
					rows_param = fs->lead_rows;
				}

				if (key->frame->lead->kind == WINDOW_BOUND_PRECEDING)
					rows_param = -rows_param;

				elog(DEBUG1, "lead: param = %li, pos = %li", rows_param, pos);

				for (funcno = 0; funcno < wstate->numfuncs; funcno++)
				{
					WindowStatePerFunction funcstate = 
						&wstate->func_state[funcno];

					if (funcstate->wlevel->level != lvl ||
						!FRAME_IS_COMPLEX(funcstate, fs))
						continue;

					elog(DEBUG1, "found lead level");
					
					/* 
					 * If we haven't filled the frame yet, just add this
					 * value to the transition value.
					 */
					if (FRAME_NEEDS_FILL(wstate, key, funcstate))
					{
						elog(DEBUG1, "frame needs a fill");
						
						if (!funcstate->frame_tuple_processed)
						{	
							WindowFrameEdge *e = key->frame->lead;
							
							if (FUNC_HAS_MAN_FRAME(funcstate) ||
								((EDGE_IS_BOUND(e) && pos <= rows_param)) ||
								e->kind == WINDOW_UNBOUND_FOLLOWING)
							{
								elog(DEBUG1, "adding value to transition on lead - 1");
								add_tuple_to_trans(funcstate, wstate, econtext);
							}
						}
						passed_all_edges = false;
					}
					else if (pos == rows_param)
					{
						if (!funcstate->frame_tuple_processed)
						{
							elog(DEBUG1, "adding value to transition on lead");
							add_tuple_to_trans(funcstate, wstate, econtext);
							funcstate->frame_tuple_processed = true;
						}
						passed_all_edges = false;
					}
					else if (pos < rows_param)
						passed_all_edges = false;
				}
				if (passed_all_edges)
					break;
			}
		}

		if (wstate->frame_levels[FRAME_TRAIL_RANGE] != NIL)
		{
			ListCell *lc;

			foreach(lc, wstate->frame_levels[FRAME_TRAIL_RANGE])
			{
				long int lvl = lfirst_int(lc);
				WindowFrameState fs;
				WindowKey *key;
				Datum framefield;
				bool fieldeq; /* field is equal to edge value */
				bool fieldlt; /* field is lt edge value */
				WindowStatePerLevel level_state = &wstate->level_state[lvl];
				bool fieldisnull;

				key = (WindowKey *)list_nth(window->windowKeys, lvl);
				if (!key->frame)
				{
					elog(DEBUG1, "key %li has null window frame", lvl);
					continue;
				}

				framefield = slot_getattr(wstate->curslot,
									 	  key->sortColIdx[0],
										  &fieldisnull);
				
				fs = &wstate->frame_states[lvl];

				if (!fs->trail_edge_set)
				{
					if (EDGE_IS_BOUND(key->frame->trail))
					{
						Datum result;

						/* initial frame edge can be either kind */
						if (key->frame->trail->kind == WINDOW_BOUND_FOLLOWING)
						{
							result = FunctionCall2(&fs->add_fn, framefield, 
												   fs->trail_range); 
							fs->trail_edge_d = result;
							fs->trail_edge_set = true;
						}
						else
						{
							result = FunctionCall2(&fs->sub_fn, framefield, 
												   fs->trail_range); 
							fs->trail_edge_d = result;
							fs->trail_edge_set = true;
						}
					}
					else if (key->frame->trail->kind == WINDOW_CURRENT_ROW)
					{
						bool isnull;
						fs->trail_edge_d = slot_getattr(wstate->curslot,
														key->sortColIdx[0],
														&isnull);
						fs->trail_edge_set = true;
					}
				}

				if (EDGE_IS_BOUND(key->frame->trail) ||
					key->frame->trail->kind == WINDOW_CURRENT_ROW)
				{
#if 0
					fieldlt = DatumGetBool(FunctionCall2(&fs->lt_fn, framefield,
										   				 fs->trail_edge_d));
#endif
					fieldeq = DatumGetBool(
								FunctionCall2(&level_state->eqfunctions[0],
										      framefield,
										      fs->trail_edge_d));
				}

				if (key->frame->trail->kind == WINDOW_UNBOUND_PRECEDING)
				{
					/* This will be picked up by the other edge of the frame */
					continue;
				}
			
				//elog(DEBUG1, "param = %li, pos = %li", rows_param, pos);
				for (funcno = 0; funcno < wstate->numfuncs; funcno++)
				{
					WindowStatePerFunction funcstate = 
						&wstate->func_state[funcno];

					if (funcstate->wlevel->level != lvl || 
						funcstate->frame_tuple_processed ||
						!FRAME_IS_COMPLEX(funcstate, fs))
						continue;

					elog(DEBUG1, "found my level at %li", lvl);
					elog(DEBUG1, "trail field: %li, edge %li, lt = %i, eq = %i",
						 framefield, fs->trail_edge_d, fieldlt, fieldeq);

					if (EDGE_IS_BOUND(key->frame->trail) &&
						!fs->prev_trail_set && fieldlt)
					{
						funcstate->frame_tuple_processed = true;
					}
#if 0
					if (pos < rows_param - 1)
					{
						/*
						 * If we haven't filled the frame yet OR if an
						 * EXCLUDING clause means the row will have never
						 * appeared in the frame, do not do anything with
						 * the row.
						 */
						funcstate->frame_tuple_processed = true;
					}
					else if (rows_param - 1 == pos)
					{
						/* 
						 * Don't invert a datum from a transition value until 
						 * we've actually initialised the trasition value.
						 * We wont be inverting any values at the time we 
						 * process the first row of the frame.
						 */
						if (wstate->row_index == 0)
							funcstate->frame_tuple_processed = true;
						else if (wstate->row_index == 1 && 
								 key->frame->exclude == WINDOW_EXCLUSION_CUR_ROW)
						{
							/* we never saw the first row, so don't invert */
							funcstate->frame_tuple_processed = true;
						}
						else
						{
							elog(DEBUG1, "inverting value");
							if (OidIsValid(funcstate->invtransfn_oid))
								invert_tuple_from_trans(funcstate, wstate,
														econtext);
							else
								elog(DEBUG1, "manage frame manually");
						}

						if (!found_lower_edge)
						{
							wstate->frame_trail_edge = pos;
							found_lower_edge = true;
						}
					}
#endif
				}
			}
		}

		if (wstate->frame_levels[FRAME_LEAD_RANGE] != NIL)
		{
			ListCell *lc;

			foreach(lc, wstate->frame_levels[FRAME_LEAD_RANGE])
			{
				long int lvl = lfirst_int(lc);
				WindowKey *key;
				WindowFrameState fs;
				Datum framefield;
				bool fieldisnull;
				bool fieldeq; /* field is equal to edge value */
				bool fieldlt; /* field is lt edge value */
				WindowStatePerLevel level_state = &wstate->level_state[lvl];

				key = (WindowKey *)list_nth(window->windowKeys, lvl);
				if (!key->frame)
				{
					elog(DEBUG1, "key %li has null window frame", lvl);
					continue;
				}

				framefield = slot_getattr(wstate->curslot,
									 	  key->sortColIdx[0],
										  &fieldisnull);

				fs = &wstate->frame_states[lvl];

				if (!fs->lead_edge_set)
				{
					if (EDGE_IS_BOUND(key->frame->lead))
					{
						bool isnull;
						Datum field;
						Datum result;

						field = slot_getattr(wstate->curslot,
											 key->sortColIdx[0],
										 	&isnull);

						/*
						 * Remember, second frame edge doesn't have to be
						 * FOLLOWING
						 */
						if (key->frame->lead->kind == WINDOW_BOUND_FOLLOWING)
						{
							result = FunctionCall2(&fs->add_fn, field, 
												   fs->lead_range); 
							fs->lead_edge_d = result;
							fs->lead_edge_set = true;
						}
						else
						{
							result = FunctionCall2(&fs->sub_fn, field, 
												   fs->lead_range); 
							fs->lead_edge_d = result;
							fs->lead_edge_set = true;
						}
					}
					else if (key->frame->lead->kind == WINDOW_CURRENT_ROW)
					{
						bool isnull;
						fs->lead_edge_d = slot_getattr(wstate->curslot,
														key->sortColIdx[0],
														&isnull);
						fs->lead_edge_set = true;
					}
				}

				
				if (EDGE_IS_BOUND(key->frame->lead) ||
					key->frame->lead->kind == WINDOW_CURRENT_ROW)
				{
#if 0
					fieldlt = DatumGetBool(FunctionCall2(&fs->lt_fn, framefield,
										   				 fs->lead_edge_d));
#endif
					elog(DEBUG1, "framefield = %li, lead = %li",
						 framefield, fs->lead_edge_d);
					fieldeq = DatumGetBool(
								FunctionCall2(&level_state->eqfunctions[0],
										   	  framefield,
										      fs->lead_edge_d));
				}

				for (funcno = 0; funcno < wstate->numfuncs; funcno++)
				{
					WindowStatePerFunction funcstate = 
						&wstate->func_state[funcno];

					if (funcstate->wlevel->level != lvl ||
						!FRAME_IS_COMPLEX(funcstate, fs))
						continue;

					elog(DEBUG1, "found lead level");
					
					/* 
					 * If we haven't filled the frame yet, just add this
					 * value to the transition value.
					 */
					if (FRAME_NEEDS_FILL(wstate, key, funcstate))
					{
						elog(DEBUG1, "frame needs a fill");
						
						if (!funcstate->frame_tuple_processed)
						{	
							WindowFrameEdge *e = key->frame->lead;
							elog(DEBUG1, "key break level %li, %i, %i", 
								 lvl, fieldlt, fieldeq);
							
							if (FUNC_HAS_MAN_FRAME(funcstate) ||
								((EDGE_IS_BOUND(e) || key->frame->lead->kind == WINDOW_CURRENT_ROW)
								 && (fieldlt || fieldeq)) ||
								e->kind == WINDOW_UNBOUND_FOLLOWING)
							{
								elog(DEBUG1, "adding value to transition on lead - 1");
								add_tuple_to_trans(funcstate, wstate, econtext);
							}
						}
						passed_all_edges = false;
					}
					else if (fieldeq)
					{
						if (!funcstate->frame_tuple_processed)
						{
							elog(DEBUG1, "adding value to transition on lead");
							add_tuple_to_trans(funcstate, wstate, econtext);
							funcstate->frame_tuple_processed = true;
						}
						passed_all_edges = false;
					}
					else if (fieldlt)
						passed_all_edges = false;
				}
				if (passed_all_edges)
					break;
			}
		}
		pos++;
		ExecClearTuple(frameslot);
		if (passed_all_edges)
		{
			elog(DEBUG1, "pos = %li, lead edge passed", pos);
			break;
		}

	}
	/* for each func */

	/*
	 * Put the current transition value into the econtext
	 */
	elog(DEBUG1, "finalising %i", wstate->numfuncs);
	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		FunctionCallInfoData fcinfo;
			
		/* Evaluate function arguments, save in fcinfo. */
	
		if (funcstate->isAgg)
		{
			Datum *vals = econtext->ecxt_aggvalues;
			bool *isnulls = econtext->ecxt_aggnulls;
	
			if (OidIsValid(funcstate->finalfn_oid))
			{

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
				isnulls[funcno] = funcstate->aggTransValueIsNull;
			}
		}
		else /* ordinary window function */
		{
			Datum *vals = econtext->ecxt_aggvalues;
			bool *isnulls = econtext->ecxt_aggnulls;
				
			vals[funcno] = funcstate->win_value;
			isnulls[funcno] = funcstate->win_value_is_null;
		}
	}
	econtext->ecxt_outertuple = wstate->priorslot;
    econtext->ecxt_scantuple = wstate->priorslot;

	elog(DEBUG1, "out of loop, pos = %li", pos);
//	moveFrameBuf(wstate, (pos > 0) ? (-pos + 1 ) : (pos - 1));
	moveFrameBuf(wstate, -pos + 1);
	
	if (!found_lower_edge)
	{
		wstate->frame_trail_edge--;
	}
	elog(DEBUG1, "======== END OF FRAME PROCESSING =======");
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
		
	invokeWindowFuncs(wstate);

	/*
	 * Form the result tuple using ExecProject(), and return it.
	 */
	resultSlot = ExecProject(wstate->ps.ps_ProjInfo, &isDone);

	return resultSlot;
}

/*
 * Initialise frame specific state.
 */

static void
init_frames(WindowState *wstate)
{
	Window *node = (Window *)wstate->ps.plan;
	int level = -1;
	ListCell *lc;
	ExprContext  *econtext = wstate->ps.ps_ExprContext;;

	foreach(lc, node->windowKeys)
	{
		WindowKey *key = lfirst(lc);
		WindowFrame *frame;
		WindowFrameState fs;
		int funcno;
		List *lvl_funcs = NIL;

		level++;
		fs = &wstate->frame_states[level];
		frame = key->frame;

		for (funcno = 0; funcno < wstate->numfuncs; funcno++)
		{
			WindowStatePerFunction funcstate = &wstate->func_state[funcno];

			/* 
			 * No level means null window specification -- i.e., OVER () --
			 * so mark it as trivial.
			 */
			if (!PointerIsValid(funcstate->wlevel))
			{
				funcstate->trivial_frame = true;
				continue;
			}

			if (level == funcstate->wlevel->level)
			{
				lvl_funcs = lappend_int(lvl_funcs, funcno);
				
				if (!frame || (fs->default_frame && !funcstate->allowframe))
					funcstate->trivial_frame = true;
			}
	
			/* 
			 * We say the function is trivial to invoke if its frame is defined
			 * as ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW with no
			 * exclusion clause OR it is a window function without a frame.
			 */
			if (frame && frame->trail->kind == WINDOW_UNBOUND_PRECEDING && 
				 frame->lead->kind == WINDOW_CURRENT_ROW &&
				 frame->exclude == WINDOW_EXCLUSION_NULL)
			{
				funcstate->trivial_frame = true;
			}
			else
			{
				/* look for empty frames */
				if (frame->trail->kind == WINDOW_UNBOUND_FOLLOWING ||
					frame->lead->kind == WINDOW_UNBOUND_PRECEDING ||
					(frame->trail->kind == WINDOW_BOUND_FOLLOWING &&
					 frame->lead->kind == WINDOW_BOUND_PRECEDING))
				{
					fs->empty_frame = true;
				}
			}
		}

		/* now, initialize the actual frame */
		if (frame)
		{
			if (frame->is_rows)
			{
				if (EDGE_IS_BOUND(frame->trail))
					fs->trail_rows = 
						((Const *)key->frame->trail->val)->constvalue;
				else
					fs->trail_rows = 0L;

				if (EDGE_IS_BOUND(frame->lead))
					fs->lead_rows = 
						((Const *)key->frame->lead->val)->constvalue;
				else
					fs->lead_rows = 0L;
			}
			else
			{
				ExprState *e;
				bool isNull;
				TupleDesc desc = ExecGetResultType(wstate->ps.lefttree); 
				HeapTuple tup;
				AttrNumber attnum;
				Oid ltype, rtype;

				/* we only need the subtraction function for bound frames */
				if (EDGE_IS_BOUND(frame->trail))
				{
					ExprState *e;
		
					e = ExecInitExpr((Expr *)key->frame->trail->val,
									  (PlanState *) wstate);

					fs->trail_range = ExecEvalExpr(e, econtext, &isNull, 
												   NULL);

					Insist(key->numSortCols == 1);
					attnum = key->sortColIdx[0];
					ltype = desc->attrs[attnum - 1]->atttypid;
					rtype = exprType(frame->lead->val);
					tup = SearchSysCache(OPERNAMENSP,
										 CStringGetDatum("-"),
										 ObjectIdGetDatum(ltype),
										 ObjectIdGetDatum(rtype),
										 ObjectIdGetDatum(PG_CATALOG_NAMESPACE));
	
					Insist(HeapTupleIsValid(tup));
					
					fmgr_info(((Form_pg_operator) GETSTRUCT(tup))->oprcode, 
							  &fs->sub_fn);
					ReleaseSysCache(tup);
					/* 
					 * XXX: we should actually use the result from the above as
					 * the LHS of this...
					 */
					tup = SearchSysCache(OPERNAMENSP,
										 CStringGetDatum(">"),
										 ObjectIdGetDatum(rtype),
										 ObjectIdGetDatum(rtype),
										 ObjectIdGetDatum(PG_CATALOG_NAMESPACE));

					Insist(HeapTupleIsValid(tup));
					
					fmgr_info(((Form_pg_operator) GETSTRUCT(tup))->oprcode,
							  &fs->gt_fn);
					ReleaseSysCache(tup);
				}

				if (EDGE_IS_BOUND(frame->lead))
				{
					Insist(key->numSortCols == 1);
					attnum = key->sortColIdx[0];
					ltype = desc->attrs[attnum - 1]->atttypid;

					e = ExecInitExpr((Expr *)key->frame->lead->val,
									 (PlanState *) wstate);

					fs->lead_range = ExecEvalExpr(e, econtext, &isNull, 
													  NULL);

					rtype = exprType(frame->lead->val);
					tup = SearchSysCache(OPERNAMENSP,
										 CStringGetDatum("+"),
										 ObjectIdGetDatum(ltype),
										 ObjectIdGetDatum(rtype),
										 ObjectIdGetDatum(PG_CATALOG_NAMESPACE));

					Insist(HeapTupleIsValid(tup));
					
					fmgr_info(
							((Form_pg_operator) GETSTRUCT(tup))->oprcode,
							&fs->add_fn);
						
					ReleaseSysCache(tup);
					
					/* now for greater-than */
					/* 
					 * XXX: we should actually use the result from the above as
					 * the LHS of this...
					 */
					tup = SearchSysCache(OPERNAMENSP,
										 CStringGetDatum("<"),
										 ObjectIdGetDatum(ltype),
										 ObjectIdGetDatum(ltype),
										 ObjectIdGetDatum(PG_CATALOG_NAMESPACE));

					Insist(HeapTupleIsValid(tup));
					
					fmgr_info(((Form_pg_operator) GETSTRUCT(tup))->oprcode,
							  &fs->lt_fn);
					ReleaseSysCache(tup);
				}
			}
		}
		
		{
			/* look for empty frames */
			if (frame->trail->kind == WINDOW_UNBOUND_FOLLOWING ||
				frame->lead->kind == WINDOW_UNBOUND_PRECEDING ||
				(frame->trail->kind == WINDOW_BOUND_FOLLOWING &&
				 frame->lead->kind == WINDOW_BOUND_PRECEDING))
			{
				fs->empty_frame = true;
			}
			else if ((frame->trail->kind == WINDOW_BOUND_PRECEDING &&
					 frame->lead->kind == WINDOW_BOUND_PRECEDING) ||
					 (frame->trail->kind == WINDOW_BOUND_FOLLOWING &&
					 frame->lead->kind == WINDOW_BOUND_FOLLOWING))
			{
				bool is_pre = 
					(frame->trail->kind == WINDOW_BOUND_PRECEDING &&
					 frame->lead->kind == WINDOW_BOUND_PRECEDING);
				
				if (frame->is_rows)
				{
					if (is_pre)
					{
						if (fs->lead_rows > fs->trail_rows)
							fs->empty_frame = true;
					}
					else
					{
						if (fs->lead_rows < fs->trail_rows)
							fs->empty_frame = true;
					}
				}
				else
				{
					Oid ordfuncid;
					TupleDesc tup = ExecGetResultType(wstate->ps.lefttree); 
					AttrNumber attnum;
					Datum datum;
					FmgrInfo fcinfo;
					Operator optup;


					attnum = key->sortColIdx[0];
					optup = ordering_oper(tup->attrs[attnum - 1]->atttypid,
										  false);
					
					ordfuncid = oprfuncid(optup); 
					elog(DEBUG1, "found operator %u", oprid(optup));
					ReleaseSysCache(optup);

					fmgr_info(ordfuncid, &fcinfo);
					
					/* is trail less than lead */
					Assert(PointerIsValid(fs->trail_range));
					Assert(PointerIsValid(fs->lead_range));
						   
					datum = FunctionCall2(&fcinfo, fs->trail_range,
										  fs->lead_range);

					if (is_pre)
					{
						if (DatumGetBool(datum))
							fs->empty_frame = true;
					}
					else
					{
						if (!DatumGetBool(datum))
							fs->empty_frame = true;
					}
				}
			}
		}
	}
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
	List *frame_trail_rows, 
		 *frame_trail_range, 
		 *frame_lead_rows, 
		 *frame_lead_range;

	/* Check for unsupported flags. */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/* Create state structure. */
	wstate = makeWindowState(node, estate);

	/* Create expression context. */
	ExecAssignExprContext(estate, &wstate->ps);

	/* XXX: temporarily high for debugging purposes */
#define WINDOW_NSLOTS 4

	/* Initialize tuple table. */
	ExecInitResultTupleSlot(estate, &wstate->ps);
	wstate->priorslot = ExecInitExtraTupleSlot(estate);
	wstate->spare = ExecInitExtraTupleSlot(estate);

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
	Assert(innerPlan(node) == NULL);

	/* Initialize result tuple type and projection info. */
	ExecAssignResultTypeFromTL(&wstate->ps);
	ExecAssignProjectionInfo(&wstate->ps, NULL);


	/* Precompute fmgr lookup data for partition key equality function. */
	if (node->numPartCols > 0)
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

	frame_trail_rows = frame_trail_range = frame_lead_rows 
		= frame_lead_range = NIL;
	
	wstate->frame_states = palloc0(sizeof(WindowFrameStateData) *
								   list_length(node->windowKeys) * 10);
	
	elog(DEBUG1, "allocating %i frame_states", 
		 list_length(node->windowKeys));

	foreach(lc, node->windowKeys)
	{
		WindowKey *key = (WindowKey *) lfirst(lc);
		WindowFrame *f;
		WindowStatePerLevel lvl = &wstate->level_state[++i];
		
		lvl->eqfunctions = 
			execTuplesMatchPrepare(ExecGetResultType(wstate->ps.lefttree),
								   key->numSortCols,
								   key->sortColIdx);

		if (!key->frame)
		{
			/* 
			 * User didn't specify a frame, so we add the default. We do not
			 * do this in the parser because otherwise we cannot handle
			 * situation such as the following query being turned into a view:
			 * 
			 *   SELECT sum(...) OVER (w1), RANK() OVER (w1) ...
			 *     WINDOW w1 AS (ORDER BY foo);
			 *
			 * When transformed to a view, we'd add the default framing clause
			 * and then the parser would barf when it encountered a 
			 * window clause for a RANK function. So, we do it here.
			 */
			WindowFrameState fs = &wstate->frame_states[lvl->level];

			/* tell others the user didn't specify a frame */
			fs->default_frame = true;

			f = key->frame = (WindowFrame *)makeNode(WindowFrame);
			f->trail = (WindowFrameEdge *)makeNode(WindowFrameEdge);
			f->lead = (WindowFrameEdge *)makeNode(WindowFrameEdge);

			/* 
			 * The default frame is:
			 *
			 *   RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
			 */
			f->trail->kind = WINDOW_UNBOUND_PRECEDING;
			f->lead->kind = WINDOW_CURRENT_ROW;

			/* 
			 * Note: we haven't set is_rows or exclude but the use of palloc0()
			 * inside of makeNode() ensures all fields of the struct are 
			 * zeros so we're golden.
			 */
		}
		else
			f = key->frame;

		Assert(PointerIsValid(f->trail));
		Assert(PointerIsValid(f->lead));

		if (f->is_rows)
		{
			frame_trail_rows = lappend_int(frame_trail_rows, i);
			frame_lead_rows = lappend_int(frame_lead_rows, i);
		}
		else
		{
			frame_trail_range = lappend_int(frame_trail_range, i);
			frame_lead_range = lappend_int(frame_lead_range, i);
		}
	}

	/* 
	 * Now, sort the frames in terms of their position relative to the 
	 * "current row". For example, the following frames are correctly ordered:
	 *
	 * ROWS UNBOUNDED PRECEDING
	 * ROWS 10 PRECEDING
	 * ROWS CURRENT ROW
	 * ROWS 10 FOLLOWING
	 *
	 * Things get a little more complex with RANGE frames.
	 *
	 * TODO: actually implement. For now, just assume they're in order!
	 */

	wstate->frame_levels[FRAME_TRAIL_ROWS] = frame_trail_rows;
	wstate->frame_levels[FRAME_LEAD_ROWS] = frame_lead_rows;
	wstate->frame_levels[FRAME_TRAIL_RANGE] = frame_trail_range;
	wstate->frame_levels[FRAME_LEAD_RANGE] = frame_lead_range;
	
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

	elog(DEBUG1, "allocating %i func states", numrefs);


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

		if (winref->winlevel < wstate->numlevels)
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
		else if (isWin)
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

			/* TODO: make this a bit cleaner */
			if (PointerIsValid(funcstate->wlevel))
				funcstate->wlevel->need_peer_count = winform->winpeercount;
		}
		else
		{
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("inappropriate use of function as window function")));

		}
	} /* next WindowRefExprState */

	/* Frame initialisation can take place now */	
	init_frames(wstate);

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

	freeFrameBuf(node);
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

	elog(DEBUG1, "--prior = %llu, peercount = %lli, result %llu",
		(long long unsigned int) level_state->prior_non_peer_count,
		(long long int) level_state->peer_count,
		(long long unsigned int) result);

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
