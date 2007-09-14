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
#include "optimizer/clauses.h"
#include "nodes/makefuncs.h"
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

#ifdef OLD_WINDOWNODE

/* 
 * A struct to hold all the data relevant to a transition value, used for
 * buffering.
 */
typedef struct WindowTransValue
{
	Datum				transValue;
	bool				transValueIsNull;
	bool				noTransValue;
} WindowTransValue;

/*
 * WindowStatePerLevelData - per-level working state
 */
typedef struct WindowStatePerLevelData
{
	Index	    level;			/* index of WindowKey for this level */
	FmgrInfo   *eqfunctions;	/* equality fns for partial key */
	FmgrInfo   *ltfunctions;	/* less-than functions for partial key */
	bool		need_peercount;

	int64		group_index;
	int64		prior_non_peer_count;
	int64		peer_index;
	int64		peer_count;
	int64		rank;
	int64		dense_rank;
	int64		prior_rank;
	int64		prior_dense_rank;
	/* indexes into WindowState->func_state for functions at this level */
	List	   *level_funcs;

	/* if the user didn't specify a frame, we use the default */
	bool default_frame;

	/* user can specify ascending or descending order, record it here */
	bool *col_sort_asc;

	/* column type data */
	Oid *col_types;
	int2 *col_typlens;
	bool *col_typbyvals;

	/* 
	 * Frame is contradictory and no tuples will fall into the frame, for
	 * example: BETWEEN 10 FOLLOWING AND 10 PRECEDING.
	 */
	bool	empty_frame;

	bool is_rows;

	/* state for rows frame is simple (for now) */
	long int trail_rows;
	long int lead_rows;

	/* state for range frame is more complex */
	Datum trail_range;
	Datum lead_range;

	/* state for frame edges */
	ExprState *trail_expr;
	ExprState *lead_expr;

	int nsortcols; /* number of sort columns in trial/lead edge data */
	AttrNumber *sortcolidx; /* sort column indexes */
	Datum *trail_edge_d;
	bool *trail_edge_nulls;
	bool trail_edge_set;
	
	Datum *lead_edge_d;
	bool *lead_edge_nulls;
	bool lead_edge_set;

	Datum *prev_trail_d;
	bool *prev_trail_nulls;
	bool prev_trail_set;
	
	Datum *prev_lead_d;
	bool *prev_lead_nulls;
	bool prev_lead_set;
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
	
	/* Aggregate-derived window function */
	Oid					transfn_oid;
	Oid					finalfn_oid;
	Oid					prelimfn_oid;
	Oid					invtransfn_oid;
	Oid					invprelimfn_oid;
	
	FmgrInfo			transfn;
	FmgrInfo			finalfn;
	FmgrInfo			prelimfn;
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

	/* actual frame buffers */
	void **frame_trans_vals;
	int32 frame_mem_size;
	int32 frame_mem_used;

	/* offsets into the frame buffers */
	int32 prev_trail_edge;
	int32 cur_trail_edge;
	bool cur_trail_edge_equal; /* edge is exactly matched */
	
	int32 prev_lead_edge;
	int32 cur_lead_edge;
	bool cur_lead_edge_equal; /* edge is exactly matched */

	/* where we've processed up to */
	int32 trail_done;
	int32 lead_done;
} WindowStatePerFunctionData;

/* 
 * We buffer two kinds windowed tuples: those for RANGE frames and those
 * for ROWS frames. RANGE frames need their key and transition value,
 * ROWS frames, only the latter.
 */

typedef struct WindowRangeBuffer
{
	Datum *key; /* one of more attributes of the key */
	bool *nulls;
	WindowTransValue *transval;
} WindowRangeBuffer;

typedef struct WindowRowsBuffer
{
	int64 row_index;
	WindowTransValue *transval;
} WindowRowsBuffer;

/*
 * We special case buffers for frames referenced by window functions
 * supporting frames.
 */
typedef struct WinFuncBufferData
{
	Datum *arg;
	bool *argnull;
} WinFuncBufferData;

typedef struct WinFuncRowBuffer
{
	WinFuncBufferData dat; /* must be first */
	int64 row_index;
} WinFuncRowBuffer;

typedef struct WinFuncRangeBuffer
{
	WinFuncBufferData dat; /* must be first */
	Datum *vals;
	bool *nulls;
} WinFuncRangeBuffer;

typedef struct WindowBreakInfo
{
	bool		part_break;
	int			min_key_break;
} WindowBreakInfo;

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

#define DB_TTS(slot) \
do \
{ \
	char *__buf = tup2str(slot); \
	elog(DEBUG1, "slot = %s", __buf); \
	pfree(__buf); \
} while(0)
	
/****************************************************
 * Window Input Buffer and its APIs
 ***************************************************/
/*
 * WindowInputBuffer: this buffer holds input tuples that fit in the
 * partition which is currently processed by the window node. This
 * buffer also stores the first tuple in the next partition, which
 * is always the last tuple in the buffer.
 */
typedef struct WindowInputBufferData
{
	Tuplestorestate *tuplestorestate;

	/* the number of tuples that are not before the current read
	 * position.
	 */
	long int ntuples;

	/* the number of tuples not before the mark pos. */
	long int ntuples_markpos;

	/* Indicate if the last tuple in the buffer is the tuple that
	 * breaks the partition key.
	 */
	bool part_break;
} WindowInputBufferData;

static void markposInputBuffer(WindowState *wstate);  /* moveFrameBuf */
static void restoreposInputBuffer(WindowState *wstate);
static void resetInputBuffer(WindowState *wstate); /* resetPartFrameBuf */
static void freeInputBuffer(WindowState *wstate); /* freeFrameBuf */

static WindowState *makeWindowState(Window *window, EState *estate);
static void initializePartition(WindowState *wstate);
static void advanceKeyLevelState(WindowState *wstate, int min_level); 
static void resetPerFunctionAggStates(WindowState *wstate, Index minlevel);
static void resetManagedFrameAggStates(WindowState *wstate);
static TupleTableSlot *nextRow(WindowState *wstate, ExprContext *econtext);
static MinimalTuple fetchTupleThroughBuf(WindowState *wstate);
static void init_frames(WindowState *wstate);
static WindowTransValue *get_last_trans_val(WindowStatePerFunction funcstate);
static void bufferAddRowWindowFuncTuple(WindowState *wstate,
										WindowStatePerFunction funcstate,
										Datum *args, bool *nulls, 
										int64 lead_row_index,
										bool tuple_in_frame);
static void bufferAddRangeWindowFuncTuple(WindowState *wstate, 
							  			  WindowStatePerFunction funcstate,
							  			  Datum *args, bool *argnulls,
							  			  Datum *vals, bool *nulls,
							  			  bool tuple_in_frame);
static long int init_rows_edge_value(ExprState *rows_expr, 
									 WindowBoundingKind kind,
									 ExprContext *econtext);

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
	Window *window = (Window *)wstate->ps.plan;
	
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
		WindowKey *key;
		
		level_state->group_index = 0;
		level_state->prior_non_peer_count = 0;
		level_state->peer_index = 0;
		level_state->peer_count = -1;
		level_state->rank = 1;
		level_state->dense_rank = 1;
		level_state->prior_rank = 1;
		level_state->prior_dense_rank = 1;
		
		key = list_nth(window->windowKeys, level);

		Insist(PointerIsValid(key));
		
		if (key->frame && key->frame->is_rows)
		{
			if (EDGE_IS_BOUND(key->frame->trail))
			{
				level_state->trail_rows = 
					init_rows_edge_value(level_state->trail_expr, 
										 key->frame->trail->kind,
										 wstate->ps.ps_ExprContext);
				level_state->trail_edge_set = true;
			}

			if (EDGE_IS_BOUND(key->frame->lead))
			{
				level_state->lead_rows = 
					init_rows_edge_value(level_state->lead_expr,
										 key->frame->lead->kind,
										 wstate->ps.ps_ExprContext);
				level_state->lead_edge_set = true;
			}
		}
	}
	
	/* Per-partition input buffer management reinitialzation. */
	resetInputBuffer(wstate);
	
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
	MinimalTuple priorwt = NULL;
	char *buf;
	bool seen_null = false;
	
	elog(DEBUG1, "called %s", PG_FUNCNAME_MACRO);


	/* Mark the current position in the input buffer */
	markposInputBuffer(wstate);
	
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

		if (level_state->need_peercount)
		{
			WindowKey  *key;

			/* we already have one peer in cur_slot, so adjust */
			Assert(level_state->peer_count == -1);
			level_state->peer_count = peer_count;

			if (!seen_null)
			{
				Assert(list_length(window->windowKeys) > i);

				key = (WindowKey *)list_nth(window->windowKeys, i);


				for (;;)
				{
					bool match;
					TupleTableSlot *slot;
					MinimalTuple wt;
	
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
			
			elog(DEBUG1, "peer count = %lld", (long long)level_state->peer_count);
		}
	}
	elog(DEBUG1, "total peer count %lld, %li", (long long)peer_count, nfetches);

	if (nfetches)
	{
		elog(DEBUG1, "rewinding %li", nfetches);
		restoreposInputBuffer(wstate);
	}
}

static void
resetPartFuncFrameBuf(WindowState *wstate)
{
	int funcno;

#define WINDOW_INIT_FRAME_BUF_SIZE 8

	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];

		if (funcstate->frame_mem_used == 0 && funcstate->frame_mem_size > 0)
		{
			/* be paranoid */
			funcstate->prev_trail_edge = funcstate->cur_trail_edge = -1;
			funcstate->prev_lead_edge = funcstate->cur_lead_edge = -1;
			funcstate->trail_done = -1;
			funcstate->lead_done = -1;
			funcstate->cur_trail_edge_equal = false;
			funcstate->cur_lead_edge_equal = false;
		}
		else
		{
			Size sz = WINDOW_INIT_FRAME_BUF_SIZE * sizeof(void *);
					
			funcstate->frame_trans_vals = palloc(sz);

			funcstate->frame_mem_size = WINDOW_INIT_FRAME_BUF_SIZE;
			funcstate->frame_mem_used = 0;

			funcstate->prev_trail_edge = funcstate->cur_trail_edge = -1;
			funcstate->prev_lead_edge = funcstate->cur_lead_edge = -1;

			funcstate->trail_done = -1;
			funcstate->lead_done = -1;

			funcstate->cur_trail_edge_equal = false;
			funcstate->cur_lead_edge_equal = false;
		
		}
	}
}


/*
 * Reset an input buffer after a partition boundary has been passed. We
 * must keep the last tuple fetched -- it is the tuple which caused the
 * key break -- and put it in the new buffer.
 *
 * Of course, we could get called to initialise the buffer as well.
 */
static void
resetInputBuffer(WindowState *wstate)
{
	if (wstate->input_buffer)
	{
		bool save_firsttuple = wstate->input_buffer->part_break;
		if (save_firsttuple)
		{
			tuplestore_gettupleslot(wstate->input_buffer->tuplestorestate,
									true, wstate->priorslot);
			wstate->curslot = ExecCopySlot(wstate->curslot, wstate->priorslot);
		}
		
#if 0
		tuplestore_reset(wstate->input_buffer->tuplestorestate);
#else
		tuplestore_end(wstate->input_buffer->tuplestorestate);
		wstate->input_buffer->tuplestorestate = 
			tuplestore_begin_heap(true, false, work_mem/2 /* XXX correct? */);
#endif

		wstate->input_buffer->ntuples = 0;
		wstate->input_buffer->ntuples_markpos = 0;
		wstate->input_buffer->part_break = false;

		wstate->cur_slot_is_new = false;
		wstate->cur_slot_part_break = false;
		wstate->cur_slot_key_break = -1;

		if (save_firsttuple)
		{
			wstate->input_buffer->ntuples++;
			wstate->input_buffer->ntuples_markpos++;

			tuplestore_puttupleslot(wstate->input_buffer->tuplestorestate,
									wstate->curslot);
			wstate->cur_slot_is_new = true;
		}
	}

	else
	{
		wstate->input_buffer = (WindowInputBuffer)palloc0(sizeof(WindowInputBufferData));
		wstate->input_buffer->tuplestorestate =
			tuplestore_begin_heap(true, false, work_mem/2 /* XXX correct? */);
		wstate->input_buffer->ntuples = 0;
		wstate->input_buffer->ntuples_markpos = 0;
	}

	resetPartFuncFrameBuf(wstate);
}

static void
freeInputBuffer(WindowState *wstate)
{
	tuplestore_end(wstate->input_buffer->tuplestorestate);
	pfree(wstate->input_buffer);
	wstate->input_buffer = NULL;
}

/*
 * Fetch a tuple in a frame buffer aware way.
 *
 * If the input buffer is not empty, the current read position to the input buffer is
 * the previous current_row. 
 * 
 */
static MinimalTuple
fetchTupleThroughBuf(WindowState *wstate)
{
	MinimalTuple wt = NULL;
	Window *window = (Window *)wstate->ps.plan;	
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	/* Read the previous current_row tuple */
	if (wstate->input_buffer->ntuples >= 1)
	{
		tuplestore_gettupleslot(wstate->input_buffer->tuplestorestate,
								true, wstate->priorslot);
	}

	if (wstate->input_buffer->ntuples <= 1)
	{
		/* if part_break is set, the tuple to be returned is the tuple
		 * in the input buffer.
		 */
		if (wstate->input_buffer->part_break)
		{
			Assert(wstate->input_buffer->ntuples == 1);

			wstate->curslot = ExecCopySlot(wstate->curslot, wstate->priorslot);
			wstate->input_buffer->part_break = false;
		}
		else
		{
			TupleTableSlot *slot = ExecProcNode(outerPlanState(wstate));	

			if (TupIsNull(slot))
				return NULL;

			if (wstate->priorslot->tts_tupleDescriptor == NULL)
			{
				ExecSetSlotDescriptor(wstate->priorslot, 
									  slot->tts_tupleDescriptor);
			}

			/* Put this new tuple into the input buffer. */
			tuplestore_puttupleslot(wstate->input_buffer->tuplestorestate, slot);

#if 0
			/* Read this tuple out of the input buffer */
			tuplestore_gettupleslot(wstate->input_buffer->tuplestorestate, true, wstate->curslot);
			wt = ExecFetchSlotMinimalTuple(wstate->curslot);
#else
			/* XXX Use this temporarily. We will switch to the above one after
			 * new tuplestore is working.
			 */
			wstate->curslot = slot;

			wt = ExecCopySlotMinimalTuple(wstate->curslot);
#endif
			wstate->input_buffer->ntuples++;
			wstate->input_buffer->ntuples_markpos++;
		}
		wstate->cur_slot_is_new = true;
	}
	else
	{
		/* Read the next tuple from the input buffer. */
		tuplestore_gettupleslot(wstate->input_buffer->tuplestorestate,
								true, wstate->curslot);

		/* XXX Since we always want the read position of tuplestore to point
		 * to the previous current_rows, we backup one position. We do not
		 * need to do this with the new API though.
		 */
		tuplestore_advance(wstate->input_buffer->tuplestorestate, false);
		
		wt = ExecFetchSlotMinimalTuple(wstate->curslot);

		wstate->cur_slot_is_new = false;
	}

	wstate->cur_slot_part_break = false;
	wstate->cur_slot_key_break = -1;

	if (wstate->input_buffer->ntuples > 1)
	{
		wstate->cur_slot_part_break =
			(window->numPartCols &&
			 (!execTuplesMatch(wstate->priorslot, wstate->curslot, 
							   window->numPartCols, window->partColIdx, 
							   wstate->eqfunctions,
							   econtext->ecxt_per_tuple_memory)));

		if (wstate->cur_slot_part_break)
			wstate->input_buffer->part_break = true;
		
		if (!wstate->cur_slot_part_break)
		{
			ListCell *lc;
			int level = 0;
			bool match = true;
			
			/* Process ordering partial key that breaks. */
			foreach (lc, window->windowKeys)
			{
				WindowKey *key = (WindowKey *)lfirst(lc);

				WindowStatePerLevel ls = &wstate->level_state[level];

				if (match)
				{
					match = execTuplesMatch(wstate->priorslot, wstate->curslot, 
											key->numSortCols, 
											key->sortColIdx,
											ls->eqfunctions,
											econtext->ecxt_per_tuple_memory);
					elog(DEBUG1, "here, match is %s", match ? "true" : "false");
					if (!match)
						wstate->cur_slot_key_break = level;
				}
						
				level++;
			}
		}
	}

	if (wstate->input_buffer->ntuples > 1)
		wstate->input_buffer->ntuples--;

	
	elog(DEBUG1, "input_buffer->ntuples_markpos = %ld, part_break=%d, min_key_break=%d", 
		 wstate->input_buffer->ntuples_markpos,
		 wstate->cur_slot_part_break,
		 wstate->cur_slot_key_break);

	return wt;
}

/*
 * Mark the current position in the input buffer.
 */
static void
markposInputBuffer(WindowState *wstate)
{
	tuplestore_markpos(wstate->input_buffer->tuplestorestate);
	wstate->input_buffer->ntuples_markpos = wstate->input_buffer->ntuples;
}

/*
 * Restore the last marked position in the input buffer.
 */
static void
restoreposInputBuffer(WindowState *wstate)
{
	tuplestore_restorepos(wstate->input_buffer->tuplestorestate);
	wstate->input_buffer->ntuples = wstate->input_buffer->ntuples_markpos;
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
resetAggState(WindowState *wstate, Datum *transVal, bool *transValIsNull, 
			  bool *noTransValue, bool initValIsNull, Datum initVal, 
			  bool typeByVal, int transTypeLen)
{
	/* free existing value, if any */
	if (!typeByVal && !*transValIsNull &&
		DatumGetPointer(*transVal) != NULL)
	{
		pfree(DatumGetPointer(*transVal));
	}

	if (initValIsNull)
		*transVal = initVal;
	else
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(wstate->transcontext);
		*transVal = datumCopy(initVal, typeByVal, transTypeLen);
		MemoryContextSwitchTo(oldcxt);
	}
	
	*transValIsNull = initValIsNull;
	*noTransValue = initValIsNull;
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
			WindowStatePerLevel level_state = 
				&wstate->level_state[funcstate->wlevel->level];

			if (funcstate->isAgg && !OidIsValid(funcstate->invprelimfn_oid) &&
				FRAME_IS_COMPLEX(funcstate, level_state))
			{
				elog(DEBUG1, "resetting managed agg");
				resetAggState(wstate, &funcstate->aggTransValue,
							  &funcstate->aggTransValueIsNull,
							  &funcstate->aggNoTransValue,
							  funcstate->aggInitValueIsNull,
							  funcstate->aggInitValue,
							  funcstate->aggTranstypeByVal,
							  funcstate->aggTranstypeLen);
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
			resetAggState(wstate, &funcstate->aggTransValue,
							  &funcstate->aggTransValueIsNull,
							  &funcstate->aggNoTransValue,
							  funcstate->aggInitValueIsNull,
							  funcstate->aggInitValue,
							  funcstate->aggTranstypeByVal,
							  funcstate->aggTranstypeLen);
	}
}

/*
 * Pull a row from the outer plan.
 */
static TupleTableSlot *
nextRow(WindowState *wstate, ExprContext *econtext)
{
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *saveSlot = wstate->saveslot;
	MinimalTuple wt;

	/* Get the new current row into the outer tuple slot. */
	wt = fetchTupleThroughBuf(wstate);

	if (!PointerIsValid(wt))
		return NULL;

	/* If first call, initialize saveslot. */
	if (wstate->saveslot->tts_tupleDescriptor == NULL)
	{
		ExecSetSlotDescriptor(wstate->saveslot, 
							  wstate->curslot->tts_tupleDescriptor);
	}
	
	outerTupleSlot = wstate->curslot;

	/* At the first row of a partition, we're already initialised. */
	if (wstate->row_index >= 0)
	{
		if (wstate->cur_slot_part_break)
			initializePartition(wstate);
		else
			advanceKeyLevelState(wstate, wstate->cur_slot_key_break);
	}

	wstate->row_index++;
	
	/* 
	 * If we saw a key change, reset the aggregates. If we didn't, reset any
	 * aggregates with "managed frames".
	 */
	if (wstate->cur_slot_key_break > 0)
		resetPerFunctionAggStates(wstate, wstate->cur_slot_key_break);
	else
		resetManagedFrameAggStates(wstate);

	ExecCopySlot(saveSlot, outerTupleSlot); /* for next time through */

	if (wstate->need_peercount &&
		(wstate->cur_slot_key_break >= 0 || wstate->row_index == 0 /* first row */
		 || wstate->cur_slot_part_break))
	{
		int level = wstate->cur_slot_key_break != -1 ? wstate->cur_slot_key_break : 0;
		processPeerCounts(wstate, level, saveSlot,
						  econtext->ecxt_per_tuple_memory);
	}

	return outerTupleSlot;
}

static int
initFcinfo(WindowRefExprState *wrxstate, FunctionCallInfoData *fcinfo,
		   WindowStatePerFunction funcstate, ExprContext *econtext)
{
	ListCell   *lcarg;
	int argno;
			
	/* If this is an agg, the first arg will be the trans value */
	if (funcstate->isAgg)
		argno = 1;
	else
		argno = 0;
	
	foreach (lcarg, wrxstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(lcarg);
		fcinfo->arg[argno] = ExecEvalExpr(argstate, econtext,
										  fcinfo->argnull + argno, NULL);
		argno++;
	}
	return argno;
}

static void
add_tuple_to_trans(WindowStatePerFunction funcstate, WindowState *wstate,
				   ExprContext *econtext)
{
	int argno;
	FunctionCallInfoData fcinfo;
			
	funcstate->frame_tuple_processed = true;
	/* Evaluate function arguments, save in fcinfo. */
	
	argno = initFcinfo(funcstate->wrxstate, &fcinfo, funcstate, econtext);

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
		elog(DEBUG1, "add_tuple_to_trans calls invoke_agg_trans_func");
	}
	else /* ordinary window function */
	{
		InitFunctionCallInfoData(fcinfo, 
								 &funcstate->windowfn,
								 argno,
								 (void *) funcstate->wrxstate, 
								 NULL);
		{
			
			funcstate->win_value = FunctionCallInvoke(&fcinfo);
			funcstate->win_value_is_null = fcinfo.isnull;
		}
	}

}

static void
invokeWindowedFunc(WindowStatePerFunction funcstate, FmgrInfo fn,
				   WindowTransValue *wtv,
				   WindowState *wstate, ExprContext *econtext)
{
	FunctionCallInfoData fcinfo;
	int argno;
			
	/* Evaluate function arguments, save in fcinfo. */
	argno = initFcinfo(funcstate->wrxstate, &fcinfo, funcstate, econtext);

	if (funcstate->isAgg)
	{
		wtv->transValue = 
			invoke_agg_trans_func(fn,
								  funcstate->numargs,
								  wtv->transValue,
								  &(wtv->noTransValue),
								  &(wtv->transValueIsNull),
								  funcstate->aggTranstypeByVal,
								  funcstate->aggTranstypeLen,
								  &fcinfo, (void *)wstate,
								  wstate->transcontext,
								  econtext->ecxt_per_tuple_memory);
		elog(DEBUG1, "invokeWindowedFunc calls invoke_agg_trans_func");
	}
	else /* ordinary window function */
	{
		/* XXX: fix me to stop temporarily in state */
		InitFunctionCallInfoData(fcinfo, 
								 &funcstate->windowfn,
								 argno,
								 (void *) funcstate->wrxstate, 
								 NULL);
		{
			
			wtv->transValue = FunctionCallInvoke(&fcinfo);
			wtv->transValueIsNull = fcinfo.isnull;
		}
	}
}

static void
invokeTrivialFuncs(WindowState *wstate, bool *found)
{
	int funcno;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	econtext->ecxt_outertuple = wstate->saveslot;
	econtext->ecxt_scantuple = wstate->saveslot;

	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		WindowStatePerLevel level_state = NULL;
		bool add = false;

		if (PointerIsValid(funcstate->wlevel))
			level_state = &wstate->level_state[funcstate->wlevel->level];
		
		if (!level_state)
			add = true;
		else if (!level_state->empty_frame && funcstate->trivial_frame)
			add = true;

		if (add)
			add_tuple_to_trans(funcstate, wstate, econtext);
		else
			*found = true;
	}
}


static bool
cmp_deformed_tuple(Datum *a, bool *a_nulls, Datum *b, bool *b_nulls,
				   bool *asc_cols, int ncols, FmgrInfo *ltfuncs, 
				   FmgrInfo *eqfuncs, MemoryContext evalContext, bool is_equal)
{
	MemoryContext oldContext;
	bool		result;
	int			i, j;
	FmgrInfo *funcs;

	/* Reset and switch into the temp context. */
	MemoryContextReset(evalContext);
	oldContext = MemoryContextSwitchTo(evalContext);

	if (is_equal)
	{
		funcs = eqfuncs;
		result = true;
	}
	else
	{
		funcs = ltfuncs;
		result = true;
	}

	/*
	 * For equality testing, we can will most likely find an inequality
	 * most quickly by working from right to left. But, we might not be
	 * doing equality testing in which case we must move from left to 
	 * right.
	 */
	if (is_equal)
		i = ncols - 1;
	else
		i = 0;

	for (j = 0; j < ncols; j++, is_equal ? i-- : i++)
	{
		Datum		attr1,
					attr2;
		bool		isNull1,
					isNull2;
		bool 		res;

		attr1 = a[i];
		isNull1 = a_nulls[i];
		
		attr2 = b[i];
		isNull2 = b_nulls[i];

		if (isNull1 != isNull2)
		{
			result = false;		/* one null and one not; they aren't equal */
			break;
		}

		if (isNull1)
			continue;			/* both are null, treat as equal */

		/* 
		 * As we're not doing equality tests, be aware of user specified
		 * ordering. If the user specified DESC, swap the order of arguments
		 * for this test.
		 */
		if (!is_equal)
		{
			if (!asc_cols[i])
			{
				Datum tmp;

				tmp = attr1;
				attr1 = attr2;
				attr2 = tmp;
			}
		}
		
		/* Apply the type-specific function */
		res = DatumGetBool(FunctionCall2(&funcs[i], attr1, attr2));

		if (is_equal && !res)
		{
			/* we wanted equality but didn't get it */
			result = false;
			break;
		}
		else if (!is_equal)
		{
			if (!res)
			{
				/* 
				 * We wanted it to be less, but it wasn't. Continue only if
				 * they're equal.
				 */
				res = DatumGetBool(FunctionCall2(&eqfuncs[i], attr1, attr2));
				if (res)
				{
					/* 
					 * Because we found the previous key equal, set the
					 * return value to false.
					 */
					result = false;
					continue;
				}
				else
					/* LHS is greater than RHS */
					result = false;
			}
			else
			{
				/* because all keys are sorted, we return straight away */
				result = true;
			}
			break;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;

}

/*
 * Like heap_deform_tuple(), but we only extract those fields listed in
 * attnums.
 *
 * values and nulls should be allocated memory by the caller. Further, it
 * is the caller's responsibility to make a copy of each datum if they must
 * survive a long time.
 */
static void
deform_window_tuple(TupleTableSlot *slot, int nattrs, AttrNumber *attnums, 
					Datum *values, bool *nulls)
{
	int i;

	for (i = 0; i < nattrs; i++)
	{
		AttrNumber attnum = attnums[i];
		bool isnull = false;
		Datum d = slot_getattr(slot, attnum, &isnull);

		/* caller is responsible for making a copy */
		values[i] = d;
		nulls[i] = isnull;
	}
}

static void
set_edge_val(WindowState *wstate, WindowStatePerLevel level_state, 
			 WindowFrameEdge *e, int ncols, Datum *vals, bool *nulls, 
			 bool is_trail, bool is_rows)
{
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	if (is_rows)
	{
		if (EDGE_IS_BOUND(e))
		{
			/* already initialised for first row */
			if (wstate->row_index > 0)
			{
				if (is_trail)
				{
					Assert(level_state->trail_edge_set);

					level_state->trail_rows++;
					elog(DEBUG1, "trail edge row = %li", 
						 level_state->trail_rows);
				}
				else
				{
					Assert(level_state->lead_edge_set);

					level_state->lead_rows++;
					elog(DEBUG1, "lead edge row = %li", level_state->lead_rows);
				}
			}
		}
		else if (e->kind == WINDOW_CURRENT_ROW)
		{
			if (is_trail)
				level_state->trail_rows = (long int)wstate->row_index;
			else
				level_state->lead_rows = (long int)wstate->row_index;
		}
	}
	else
	{
		if (EDGE_IS_BOUND(e))
		{
			Datum result;
			
			if (is_trail)
			{
				bool isnull = false;

				result = ExecEvalExpr(level_state->trail_expr, econtext,
									  &isnull, NULL);
				level_state->trail_edge_d[0] = result;
				level_state->trail_edge_nulls[0] = isnull;
				level_state->trail_edge_set = true;
	
			}
			else
			{
				bool isnull = false;

				result = ExecEvalExpr(level_state->lead_expr, econtext,
									  &isnull, NULL);
				level_state->lead_edge_d[0] = result;
				level_state->lead_edge_nulls[0] = isnull;
				level_state->lead_edge_set = true;
			}
		}
		else if (e->kind == WINDOW_CURRENT_ROW)
		{
			/* for current row, just copy things directly */
			Datum *d;
			bool *n;
			MemoryContext oldctx;
			int i;
			
			if (is_trail)
			{
				d = level_state->trail_edge_d;
				n = level_state->trail_edge_nulls;
				level_state->trail_edge_set = true;
			}
			else
			{
				d = level_state->lead_edge_d;
				n = level_state->lead_edge_nulls;
				level_state->lead_edge_set = true;
			}

			oldctx = MemoryContextSwitchTo(wstate->transcontext);

			for (i = 0; i < ncols; i++)
			{
				/* 
				 * Copy the tuple since current it's the tuple directly
				 * from the buffer pool.
				 */
				d[i] = datumCopy(vals[i], level_state->col_typbyvals[i],
								level_state->col_typlens[i]);
				n[i] = nulls[i];
			}
			MemoryContextSwitchTo(wstate->transcontext);
		}
	}
}

static void
eval_copy_winfunc_args(WindowState *wstate, WindowRefExprState *wrxstate,
					   Datum *vals, bool *nulls)
{
	ListCell *lc;
	int argno = 0;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	
	foreach(lc, wrxstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(lc);
		MemoryContext oldctx;

		/* skip the internal arg */
		if (argno < 1)
		{
			argno++;
			continue;
		}

		vals[argno - 1] = ExecEvalExpr(argstate, econtext,
								       nulls + argno - 1, NULL);

		oldctx = MemoryContextSwitchTo(wstate->transcontext);
		vals[argno - 1] = datumCopy(vals[argno - 1], 
									wrxstate->argtypbyval[argno],
									wrxstate->argtyplen[argno]);
		
		MemoryContextSwitchTo(oldctx);

		argno++;
	}
}

static void
bufEnsureSpace(WindowStatePerFunction funcstate, WindowState *wstate,
			   size_t elem_sz)
{
	if (funcstate->frame_mem_used + 1 >= funcstate->frame_mem_size)
	{
		/* we should guard against allocating greater than 2^31 bytes! */
		MemoryContext oldctx;
		int32 newsize;

		oldctx = MemoryContextSwitchTo(wstate->transcontext);
		newsize = funcstate->frame_mem_size * 2;
		funcstate->frame_trans_vals = repalloc(funcstate->frame_trans_vals,
											   newsize * elem_sz);
		funcstate->frame_mem_size = newsize;
	}
}

static void
funcFrameBufAppend(WindowState *wstate, WindowStatePerFunction funcstate, 
				   void *data, bool tuple_in_frame)
{

	/* XXX: be careful about size below */
	bufEnsureSpace(funcstate, wstate, sizeof(data));
	
	funcstate->frame_mem_used++;
	funcstate->frame_trans_vals[funcstate->frame_mem_used - 1] = data;

	if (tuple_in_frame)
	{
		funcstate->cur_lead_edge = funcstate->frame_mem_used - 1;

		elog(DEBUG1, "setting lead: prev = %i, lead = %i",
			 funcstate->prev_lead_edge, funcstate->cur_lead_edge);
	}
}

static WindowTransValue *
get_last_trans_val(WindowStatePerFunction funcstate)
{
	WindowRangeBuffer *wrb;

	Insist(funcstate->frame_mem_used > 0);
	wrb = (WindowRangeBuffer *)funcstate->frame_trans_vals[funcstate->frame_mem_used - 1];
	return wrb->transval;
}

static void
bufferAddRangeTransValue(WindowState *wstate, int ncols, 
						 WindowStatePerFunction funcstate,
						 WindowTransValue *wtv, Datum *vals, bool *nulls,
						 bool tuple_before_edge)
{
	MemoryContext oldctx;
	WindowRangeBuffer *wrb;
	WindowStatePerLevel level_state;
	int i;

	Insist(PointerIsValid(funcstate->wlevel));
	level_state = funcstate->wlevel;

	oldctx = MemoryContextSwitchTo(wstate->transcontext);

	wrb = palloc(sizeof(WindowRangeBuffer));
	wrb->key = palloc(sizeof(Datum) * ncols);
	wrb->nulls = palloc(sizeof(bool) * ncols);

	for (i = 0; i < ncols; i++)
	{
		wrb->key[i] = datumCopy(vals[i], level_state->col_typbyvals[i],
								level_state->col_typlens[i]);
		wrb->nulls[i] = nulls[i];
	}

	MemoryContextSwitchTo(oldctx);

	wrb->transval = wtv;

	funcFrameBufAppend(wstate, funcstate, (void *)wrb, tuple_before_edge);

	if (funcstate->prev_lead_edge == -1 && tuple_before_edge)
		funcstate->prev_lead_edge = 0;
}

/*
 * Add a tuple top a RANGE based frame buffer for each function in funcs.
 * Return true if the tuple was inside the frame -- before the end of the
 * leading edge.
 *
 * If is_peer is true, the tuple is a peer of the previously seen tuple, 
 * so we simply pass the transition value for that function to
 * invokeWindowedFunc().
 *
 * eq_lead_edge tells appendRangeTuple() is equal to the current edge.
 */
static void
appendRangeTuple(WindowState *wstate, WindowFrame *frame, 
				 WindowStatePerLevel level_state,
				 Datum *vals, bool *nulls, int level, List *funcs,
				 bool is_peer, bool tuple_before_edge, bool eq_lead_edge)
{
	ListCell *lc;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	foreach(lc, funcs)
	{
		int funcno = lfirst_int(lc);
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];

		if (funcstate->trivial_frame ||
			(!funcstate->isAgg && !funcstate->allowframe))
		{
			continue;
		}

		if (tuple_before_edge)
			funcstate->cur_lead_edge_equal = eq_lead_edge;

		if (funcstate->isAgg)
		{
			WindowTransValue *wtv;
			bool append = true;
			if (is_peer && funcstate->frame_mem_used > 0)
			{
				
				wtv = get_last_trans_val(funcstate);
				append = false;
			}
			else
			{
				MemoryContext oldcxt;
	
				oldcxt = MemoryContextSwitchTo(wstate->transcontext);
				wtv = palloc(sizeof(WindowTransValue));
				wtv->noTransValue = funcstate->aggInitValueIsNull;
	
				wtv->transValue = datumCopy(funcstate->aggInitValue, 
											funcstate->aggTranstypeByVal,
											funcstate->aggTranstypeLen);
				wtv->transValueIsNull = funcstate->aggInitValueIsNull;
				MemoryContextSwitchTo(oldcxt);
			}

			invokeWindowedFunc(funcstate, funcstate->transfn, wtv, 
							   wstate, econtext);

			if (append)
			{
				int ncols = level_state->nsortcols;

				elog(DEBUG1, "append a new transition value.");
				bufferAddRangeTransValue(wstate, ncols, funcstate, wtv, vals, 
										 nulls, tuple_before_edge);
			}
		}
		else
		{
			Datum *args;
			bool *argnulls;
			int nargs;
			MemoryContext oldctx;
			int argno;
			WindowRefExprState *wrxstate = funcstate->wrxstate;

			/* 
			 * We're actually buffering the arguments to the function too,
			 * instead of passing them to a transition function as we would
			 * for an aggregate-as-window-function. So, we pull the args
			 * out and save them.
			 */
			nargs = list_length(wrxstate->args) - 1;
			argno = -1;
			
			oldctx = MemoryContextSwitchTo(wstate->transcontext);
			
			args = palloc(nargs * sizeof(Datum));
			argnulls = palloc(nargs * sizeof(bool));
			
			MemoryContextSwitchTo(oldctx);

			eval_copy_winfunc_args(wstate, wrxstate, args, argnulls);
			
			bufferAddRangeWindowFuncTuple(wstate, funcstate, args, argnulls,
										  vals, nulls, tuple_before_edge);
		}
	}
}

static void
bufferAddRowWindowFuncTuple(WindowState *wstate, 
							WindowStatePerFunction funcstate,
							Datum *args, bool *nulls, int64 lead_row_index,
							bool tuple_in_frame)
{
	WinFuncRowBuffer *wrb = MemoryContextAlloc(wstate->transcontext,
											    sizeof(WinFuncRowBuffer));

	wrb->row_index = lead_row_index;
	wrb->dat.arg = args;
	wrb->dat.argnull = nulls;
	funcFrameBufAppend(wstate, funcstate, (void *)wrb, tuple_in_frame);
}

static void
bufferAddRangeWindowFuncTuple(WindowState *wstate, 
							  WindowStatePerFunction funcstate,
							  Datum *args, bool *argnulls,
							  Datum *vals, bool *nulls,
							  bool tuple_in_frame)
{
	WinFuncRangeBuffer *wrb = MemoryContextAlloc(wstate->transcontext,
												 sizeof(WinFuncRangeBuffer));

	wrb->dat.arg = args;
	wrb->dat.argnull = nulls;
	wrb->vals = vals;
	wrb->nulls = nulls;
	funcFrameBufAppend(wstate, funcstate, (void *)wrb, tuple_in_frame);
}

static void
bufferAddRowTransValue(WindowState *wstate, WindowStatePerFunction funcstate,
					   WindowTransValue *wtv, int64 lead_row_index,
					   bool tuple_in_frame)
{
	WindowRowsBuffer *wrb = MemoryContextAlloc(wstate->transcontext,
											   sizeof(WindowRowsBuffer));

	wrb->row_index = lead_row_index;
	wrb->transval = wtv;

	funcFrameBufAppend(wstate, funcstate, (void *)wrb, tuple_in_frame);
}

/*
 * Add a tuple to the frame buffer for each function in funcs. Handling
 * here is a bit different because we are processing a row based frame.
 */
static bool
appendRowTuple(WindowState *wstate, WindowFrame *frame,
			   WindowStatePerLevel level_state, TupleTableSlot *slot, 
			   List *funcs, int64 lead_row_index)
{
	ListCell *lc;
	bool tuple_in_frame = false;
	bool tuple_before_lead = false;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	if (EDGE_IS_BOUND(frame->lead) || frame->lead->kind == WINDOW_CURRENT_ROW)
	{
		elog(DEBUG1, "lead row index = %lli, lead param = %li",
			(long long) lead_row_index, level_state->lead_rows);
		
		if (lead_row_index <= level_state->lead_rows)
		{
			tuple_before_lead = true;
			
			if (/*level_state->trail_rows >= 0 &&  */
				lead_row_index >= level_state->trail_rows)
				tuple_in_frame = true;
		}

	}
	else
	{
		Assert(frame->lead->kind == WINDOW_UNBOUND_FOLLOWING);

		tuple_in_frame = true;
		tuple_before_lead = true;
	}

	foreach(lc, funcs)
	{
		int funcno = lfirst_int(lc);
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];

		if (funcstate->isAgg)
		{
			WindowTransValue *wtv; 
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(wstate->transcontext);
			wtv = palloc(sizeof(WindowTransValue));
			wtv->noTransValue = funcstate->aggInitValueIsNull;

			wtv->transValue = datumCopy(funcstate->aggInitValue, 
										funcstate->aggTranstypeByVal,
										funcstate->aggTranstypeLen);
			wtv->transValueIsNull = funcstate->aggInitValueIsNull;
			MemoryContextSwitchTo(oldcxt);
		
			invokeWindowedFunc(funcstate, funcstate->transfn, wtv,
							   wstate, econtext);

			bufferAddRowTransValue(wstate, funcstate, wtv, lead_row_index,
								   tuple_in_frame);
		}
		else
		{
			Datum *args;
			bool *argnulls;
			int nargs;
			MemoryContext oldctx;
			int argno;
			WindowRefExprState *wrxstate = funcstate->wrxstate;

			/* 
			 * We're actually buffering the arguments to the function too,
			 * instead of passing them to a transition function as we would
			 * for an aggregate-as-window-function. So, we pull the args
			 * out and save them.
			 */
			nargs = list_length(wrxstate->args) - 1;
			argno = 0;
			
			oldctx = MemoryContextSwitchTo(wstate->transcontext);
			
			args = palloc(nargs * sizeof(Datum));
			argnulls = palloc(nargs * sizeof(bool));
			
			MemoryContextSwitchTo(oldctx);

			eval_copy_winfunc_args(wstate, wrxstate, args, argnulls);

			bufferAddRowWindowFuncTuple(wstate, funcstate, args, argnulls,
										lead_row_index, tuple_in_frame);
	
		}

		/* we're in business, ensure everything else knows about it */
		if (EDGE_IS_BOUND(frame->trail) &&
			funcstate->prev_lead_edge == -1)
		{
			if (frame->trail->kind == WINDOW_BOUND_PRECEDING)
				funcstate->prev_lead_edge = 0;
			else
			{
				if (funcstate->cur_trail_edge > 0)
					funcstate->prev_lead_edge = funcstate->cur_trail_edge;
				else
				{
					long int rows_param;
					bool isnull = false;
	
					rows_param = ExecEvalExpr(level_state->trail_expr,
											  econtext,
											  &isnull,
											  NULL);
					funcstate->prev_lead_edge = 0;
					if (funcstate->cur_trail_edge == -1)
					{
						funcstate->cur_trail_edge = rows_param;
						funcstate->prev_trail_edge = 0;
					}
				}
			}
		}
		else if ((frame->trail->kind == WINDOW_UNBOUND_PRECEDING ||
				  frame->trail->kind == WINDOW_CURRENT_ROW) &&
				 funcstate->prev_lead_edge == -1)
		{
			funcstate->prev_lead_edge = 0;
		}
	}

	return tuple_before_lead;
}

/* 
 * Set the current buffer position to the previous one and find the new
 * position, if it exists in the buffer
 */
static void
adjustEdges(WindowState *wstate)
{
	Window *window = (Window *)wstate->ps.plan;
	int level = -1;
	ListCell *lkey;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	foreach(lkey, window->windowKeys)
	{
		WindowKey *key = lfirst(lkey);
		WindowStatePerLevel level_state;
		ListCell *lc;
	   	
		level++;
		
		level_state = &wstate->level_state[level];

		foreach(lc, level_state->level_funcs)
		{
			int funcno = lfirst_int(lc);
			WindowStatePerFunction funcstate = &wstate->func_state[funcno];
			
			if (key->frame->is_rows)
			{
				/* 
				 * If we've processed the first row, set the prev to the
				 * edge of the first row
				 */
				if (funcstate->cur_trail_edge != -1)
				{
					if (funcstate->prev_trail_edge != -1)
						funcstate->prev_trail_edge = funcstate->cur_trail_edge;
					else
						funcstate->prev_trail_edge = 0;
				}
				if (key->frame->trail->kind == WINDOW_CURRENT_ROW)
				{
				   	if (funcstate->cur_trail_edge == -1)
					{
						funcstate->cur_trail_edge = 0;
				//		funcstate->prev_trail_edge = 0;
					}
					else
					{
						funcstate->cur_trail_edge++;
						//funcstate->prev_trail_edge++;
					}
				}
				else if (key->frame->trail->kind == WINDOW_BOUND_PRECEDING)
				{
					if (funcstate->cur_trail_edge == -1)
					{
						/* we've come into frame now */
						if (level_state->trail_rows == 0)
						{
							funcstate->cur_trail_edge = 0;
							//funcstate->prev_trail_edge = 0;
						}
					}
					else
					{
						funcstate->cur_trail_edge++;
						//funcstate->prev_trail_edge++;
					}
				}
				else if (key->frame->trail->kind == WINDOW_BOUND_FOLLOWING)
				{
					if (funcstate->cur_trail_edge == -1)
					{
						bool isnull = false;
						long int rows_param = 
							ExecEvalExpr(level_state->trail_expr,
										 econtext, &isnull, NULL);

						if (rows_param == wstate->row_index)
						{
							/* this is legal since we know we're N rows ahead */
							funcstate->cur_trail_edge = rows_param;
							funcstate->prev_trail_edge = rows_param;
						}
					}
					else if (funcstate->cur_trail_edge != -1)
					{
						funcstate->cur_trail_edge++;
						//funcstate->prev_trail_edge++;
					}
					/* do nothing if we didn't find out edge */
				}
	
				/* now for the lead */
				if (funcstate->cur_lead_edge != -1)
				{
					if (funcstate->prev_lead_edge != -1)
						funcstate->prev_lead_edge = funcstate->cur_lead_edge;
					else
						funcstate->prev_lead_edge = 0;
				}
				else if(key->frame->trail->kind == WINDOW_BOUND_PRECEDING)
				{
					funcstate->prev_lead_edge = 0;
				}

				if (key->frame->lead->kind == WINDOW_CURRENT_ROW)
				{
				   	if (funcstate->cur_lead_edge == -1)
					{
						funcstate->cur_lead_edge = 0;
						funcstate->prev_lead_edge = 0;
					}
					else
					{
						funcstate->cur_lead_edge++;
						funcstate->prev_lead_edge++;
					}
				}
				else if (key->frame->lead->kind == WINDOW_BOUND_PRECEDING)
				{
					if (funcstate->cur_lead_edge == -1)
					{
						/* we've come into frame now */
						if (level_state->lead_rows == 0)
						{
							funcstate->cur_lead_edge = 0;
							funcstate->prev_lead_edge = 0;
						}
					}
					else
					{
						funcstate->cur_lead_edge++;
						funcstate->prev_lead_edge++;
					}
				}
				else if (key->frame->lead->kind == WINDOW_BOUND_FOLLOWING)
				{
					if (funcstate->cur_lead_edge == -1 &&
						funcstate->frame_mem_used)
					{
						WindowRowsBuffer *wrb;

						wrb = funcstate->frame_trans_vals[0];

						if (wrb->row_index == wstate->row_index)
						{
							funcstate->cur_lead_edge = 0;
							funcstate->prev_lead_edge = 0;
						}
					}
					else if (funcstate->cur_lead_edge != -1)
					{
						funcstate->cur_lead_edge++;
						funcstate->prev_lead_edge++;
					}
					/* do nothing if we didn't find out edge */
				}
			}
			else
			{
				long int pos = 0;
				bool found_lead = false;

				/* 
				 * First, do trail. prev_trail_edge might be -1, so be
				 * careful.
				 */
				if (key->frame->trail->kind != WINDOW_UNBOUND_PRECEDING)
				{
					Insist(key->frame->trail->kind != WINDOW_UNBOUND_FOLLOWING);
					
					
					funcstate->prev_trail_edge = funcstate->cur_trail_edge;
					/* it was greater than the edge, so go past it */
					if (!funcstate->cur_trail_edge_equal && 
						funcstate->prev_trail_edge != -1)
					{
						funcstate->prev_trail_edge++;
					}
					elog(DEBUG1, "prev trail = %i", funcstate->prev_trail_edge);
	
					if (funcstate->prev_trail_edge > 0)
						pos = funcstate->prev_trail_edge;
	
					while (pos <= (funcstate->frame_mem_used - 1))
					{
						bool res;
						Datum *keys;
						bool *nulls;

						/* 
						 * These buffers have a different physical layout so
						 * we must interact with them seperately.
						 */
						if (funcstate->isAgg)
						{
							WindowRangeBuffer *wrb;

							wrb = funcstate->frame_trans_vals[pos];
							keys = wrb->key;
							nulls = wrb->nulls;
						}
						else
						{
							WinFuncRangeBuffer *wrb;

							wrb = funcstate->frame_trans_vals[pos];
							keys = wrb->vals;
							nulls = wrb->nulls;
						}
	
						res = cmp_deformed_tuple(keys, nulls,
												 level_state->trail_edge_d,
												 level_state->trail_edge_nulls,
												 level_state->col_sort_asc,
												 level_state->nsortcols,
												 level_state->ltfunctions,
												 level_state->eqfunctions,
											 econtext->ecxt_per_tuple_memory,
												 false);
						/* add equal test */
						if (res)
						{
							if (funcstate->prev_trail_edge == -1)
								funcstate->prev_trail_edge = 0;
	
							funcstate->cur_trail_edge = pos;
							funcstate->cur_trail_edge_equal = false;
						}
						else
						{
							res =  cmp_deformed_tuple(keys, nulls,
												 level_state->trail_edge_d,
												 level_state->trail_edge_nulls,
												 level_state->col_sort_asc,
												 level_state->nsortcols,
												 level_state->ltfunctions,
												 level_state->eqfunctions,
											 econtext->ecxt_per_tuple_memory,
												 true);
							if (res)
							{
								if (funcstate->prev_trail_edge == -1)
									funcstate->prev_trail_edge = 0;
	
								funcstate->cur_trail_edge = pos;
								funcstate->cur_trail_edge_equal = true;
							}
							/* we must break either way */
							break;
						}
	
						pos++;
					}
				}
	
				/* now, leading edge */
				funcstate->prev_lead_edge = funcstate->cur_lead_edge;


				if (funcstate->cur_lead_edge > 0)
					pos = funcstate->cur_lead_edge;
				else
					pos = 0; /* reset */

				while (pos <= (funcstate->frame_mem_used - 1))
				{
					bool res1 = false, res2 = false;
					Datum *keys;
					bool *nulls;

					/* 
					 * These buffers have a different physical layout so
					 * we must interact with them seperately.
					 */
					if (funcstate->isAgg)
					{
						WindowRangeBuffer *wrb;

						wrb = funcstate->frame_trans_vals[pos];
						keys = wrb->key;
						nulls = wrb->nulls;
					}
					else
					{
						WinFuncRangeBuffer *wrb;
						wrb = funcstate->frame_trans_vals[pos];
						keys = wrb->vals;
						nulls = wrb->nulls;
					}

					res1 = cmp_deformed_tuple(keys, nulls,
											 level_state->lead_edge_d,
											 level_state->lead_edge_nulls,
											 level_state->col_sort_asc,
											 level_state->nsortcols,
											 level_state->ltfunctions,
											 level_state->eqfunctions,
											 econtext->ecxt_per_tuple_memory,
											 false);
					
					if (!res1)
						res2 = cmp_deformed_tuple(keys, nulls,
												 level_state->lead_edge_d,
												 level_state->lead_edge_nulls,
												 level_state->col_sort_asc,
												 level_state->nsortcols,
												 level_state->ltfunctions,
												 level_state->eqfunctions,
												 econtext->ecxt_per_tuple_memory,
												 true);
					
					if (res1 || res2)
					{

						funcstate->cur_lead_edge = pos;
						found_lead = true;
						if (res2)
							funcstate->cur_lead_edge_equal = true;
						else
							funcstate->cur_lead_edge_equal = false;

						/* 
						 * If values if just come into the frame, set the
						 * edge to be that of the current trail. If that is
						 * not set (must be unbounded preceding), set it
						 * to the current lead value (which would be zero).
						 */
						if (funcstate->prev_lead_edge == -1)
						{
							if (funcstate->cur_trail_edge != -1)
							{
								funcstate->prev_lead_edge = 
									funcstate->cur_trail_edge;
							}
							else
							{
								Insist(funcstate->cur_lead_edge == 0);
								funcstate->prev_lead_edge = 
									funcstate->cur_lead_edge;
							}
						}

						/* 
						 * We found a match so the next key will be past the
						 * edge.
						 */
						if (res2)
							break;
					}
					else
						break;

					pos++;
				}

				/* 
				 * If the lead edge was equal, go past it with the previous
				 * position so we do not count it twice.
				 */
				if (funcstate->prev_lead_edge != -1 &&
					funcstate->prev_lead_edge < funcstate->cur_lead_edge)
				{
					funcstate->prev_lead_edge++;
				}
			}
		}
	}
}

/*
 * populateFrameBuffers() adds new tuples/transition values to the
 * frame buffer for each function at each key level.level* Firstly, we calculate the new edges for each frame. Then, for each function,
 * if necessary, we add new values to the frame corresponding to the distance
 * between the old edge and new. Note that we must look at the "to process"
 * list of values which came after the last edge first.
 *
 * After this, we pull from the outer plan until the tuple we receive is
 * past the edge of all frames.
 *
 * We update logical pointers to the beginning and end of all frame edges for
 * each function and return to the caller.
 */
static void
populateFrameBuffers(WindowState *wstate)
{
	Window *window = (Window *)wstate->ps.plan;
	ListCell *lc;
	int level = -1;
	int64 lead_row_index = wstate->row_index;
	long int ntuples;
	bool process_cur_slot = wstate->cur_slot_is_new;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	Datum *save_vals;
	bool *save_nulls;
	int save_ncols = 0;
	
	save_vals = NULL;
	save_nulls = NULL;

	/* calculate (new) frame edges */
	foreach(lc, window->windowKeys)
	{
		WindowKey *key = lfirst(lc);
		WindowStatePerLevel level_state;
		Datum *vals;
		bool *nulls;
		int ncols;

		level++;
		level_state = &wstate->level_state[level];
		
		if (key->numSortCols)
		{
			if (PointerIsValid(save_vals))
			{
				Assert(PointerIsValid(save_nulls));

				pfree(save_vals);
				pfree(save_nulls);
			}

			save_vals = vals = palloc(sizeof(Datum) * key->numSortCols);
			save_nulls = nulls = palloc(sizeof(bool) * key->numSortCols);
			save_ncols = ncols = key->numSortCols;
		
			deform_window_tuple(wstate->curslot, key->numSortCols,
								key->sortColIdx, vals, nulls); 
		}
		else
		{
			vals = save_vals;
			nulls = save_nulls;
			ncols = save_ncols;
		}

		set_edge_val(wstate, level_state, key->frame->trail, ncols, vals, 
					 nulls, true, key->frame->is_rows);
		set_edge_val(wstate, level_state, key->frame->lead, ncols, vals, 
					 nulls, false, key->frame->is_rows);

	}

	pfree(save_vals);
	pfree(save_nulls);

	/* adjust the trailing edges, determining old and new edge positions */
	adjustEdges(wstate);

	/* save out position */
	markposInputBuffer(wstate);
	
	/* fast forward from current row */
	if (!wstate->input_buffer->part_break)
	{
		ntuples = wstate->input_buffer->ntuples - 1; /* ignore current row */
		wstate->input_buffer->ntuples = 1;
	}
	else
	{
		/* ignore current row and the tuple that breaks the key */
		ntuples = wstate->input_buffer->ntuples - 2;
		wstate->input_buffer->ntuples = 2;
	}
	
    {                                                                           
        int tuple_no;                                                           
                                                                                
        /* advance ntuples */                                                   
        for(tuple_no=0; tuple_no<ntuples; tuple_no++)                           
            tuplestore_advance(wstate->input_buffer->tuplestorestate, true);    
    }

	/* we've moved forward by ntuples so add that to the row index */
	lead_row_index += ntuples;

	elog(DEBUG1, "----populating frame");
	/* now for the hard bit, populating the actual frame */
	
	while (true)
	{
		bool passed_all_edges = true;
		TupleTableSlot *slot; // = ExecProcNode(outerPlanState(wstate));;
		WindowBreakInfo wbi;

		if (process_cur_slot)
		{
			slot = wstate->curslot;
			
			/* if null, exit */
			if (TupIsNull(slot))
				break;

			wbi.part_break = wstate->cur_slot_part_break;
			wbi.min_key_break = wstate->cur_slot_key_break;
		}
		else
		{
			MinimalTuple wt;
			wt = fetchTupleThroughBuf(wstate);

			if (!wt)
				break;
			
			slot = wstate->curslot;
			
			/* if null, exit */
			if (TupIsNull(slot))
				break;

			lead_row_index++;
			wbi.part_break = wstate->cur_slot_part_break;
			wbi.min_key_break = wstate->cur_slot_key_break;

			process_cur_slot = wstate->cur_slot_is_new;

			if (!wbi.part_break)
				ntuples++;
		}
		
		DB_TTS(slot);

		if (wbi.part_break)
		{
			elog(DEBUG1, "saw partition break");
			break;
		}

		/* make expr evalutators happy */
		econtext->ecxt_scantuple = slot;
		econtext->ecxt_outertuple = slot;

		/* 
		 * Test to see if the tuple falls into any frame?
		 *
		 * It might seem logical to search the windowKeys list in reverse
		 * because upper key levels are likely the break first. However,
		 * we do not care about that because we're going to process the tuple
		 * for each key level anyway.
		 */
		
		level = -1;
		foreach(lc, window->windowKeys)
		{
			WindowKey *key = lfirst(lc);
			WindowFrame *frame = key->frame;
			bool is_rows = frame->is_rows;
			bool tuple_in_frame = false;
			WindowStatePerLevel level_state;

			level++;
			
			level_state = &wstate->level_state[level];
	
			if (level_state->empty_frame)
				return;


			if (is_rows)
			{
				tuple_in_frame = appendRowTuple(wstate, key->frame,
												level_state, slot,
												level_state->level_funcs,
												lead_row_index);
				process_cur_slot = false;
			}
			else
			{
				bool is_peer = false;
				Datum *new_vals;
				bool *new_nulls;
				bool lttrail = false, eqtrail, ltlead, eqlead = false;
				bool tuple_before_edge = false;
				bool tuple_in_buf = false;
				
				/* if this is a peer of the previous tuple, just append */
				if (level < wbi.min_key_break || wbi.min_key_break == -1)
					is_peer = true;

				new_vals = palloc(sizeof(Datum) * level_state->nsortcols);
				new_nulls = palloc(sizeof(bool) * level_state->nsortcols);

				deform_window_tuple(slot, level_state->nsortcols, 
									level_state->sortcolidx,
									new_vals, new_nulls);
	
				if (key->frame->trail->kind == WINDOW_UNBOUND_PRECEDING)
					lttrail = false;
				else if (key->frame->trail->kind == WINDOW_CURRENT_ROW ||
						 EDGE_IS_BOUND(key->frame->trail))
				{
						 
					/* XXX: if this is a peer, why do this? */			
					lttrail = cmp_deformed_tuple(new_vals, new_nulls,
											   level_state->trail_edge_d,
											   level_state->trail_edge_nulls,
											   level_state->col_sort_asc,
											   level_state->nsortcols,
											   level_state->ltfunctions,
											   level_state->eqfunctions,
											   econtext->ecxt_per_tuple_memory,
											   false);
				}

				if (lttrail)
				{
					tuple_before_edge = true;
				}
				else
				{
					if (key->frame->trail->kind == WINDOW_UNBOUND_PRECEDING)
						eqtrail = false;
					else
						eqtrail = cmp_deformed_tuple(new_vals, new_nulls,
											   level_state->trail_edge_d,
											   level_state->trail_edge_nulls,
											   level_state->col_sort_asc,
											   level_state->nsortcols,
											   level_state->ltfunctions,
											   level_state->eqfunctions,
											   econtext->ecxt_per_tuple_memory,
											   true);

					if (eqtrail)
					{
						tuple_in_buf = true;
						tuple_before_edge = true;
					}
					else
					{
						if (key->frame->lead->kind == WINDOW_UNBOUND_FOLLOWING)
						{
							ltlead = true;
						}
						else
						{
							/* see if it is less than or equal to the lead */
							ltlead = cmp_deformed_tuple(new_vals, new_nulls,
											   level_state->lead_edge_d,
											   level_state->lead_edge_nulls,
											   level_state->col_sort_asc,
											   level_state->nsortcols,
											   level_state->ltfunctions,
											   level_state->eqfunctions,
										   econtext->ecxt_per_tuple_memory,
											   false);
						}
						
						if (ltlead)
						{
							tuple_in_buf = true;
							tuple_before_edge = true;
						}
						else
						{
							eqlead = 
								cmp_deformed_tuple(new_vals, 
												   new_nulls,
												   level_state->lead_edge_d,
												   level_state->lead_edge_nulls,
												   level_state->col_sort_asc,
												   level_state->nsortcols,
												   level_state->ltfunctions,
												   level_state->eqfunctions,
											   econtext->ecxt_per_tuple_memory,
											   	   true);

							if (eqlead)
							{
								tuple_in_buf = true;
								tuple_before_edge = true;
							}
						}
					}
				}

				appendRangeTuple(wstate, key->frame, level_state, 
								 new_vals, new_nulls, 
								 level, level_state->level_funcs,
								 is_peer, tuple_in_buf, eqlead);
				process_cur_slot = false;

				tuple_in_frame = tuple_before_edge;

			}

			if (tuple_in_frame)
				passed_all_edges = false;
		}

		if (passed_all_edges)
			break;
	}

	elog(DEBUG1, "moving frame by %li, lead row %lli", (-ntuples + 1),
		(long long) lead_row_index);

	restoreposInputBuffer(wstate);
	elog(DEBUG1, "----- end frame populate");

	/* restore old tuple */
	econtext->ecxt_outertuple = wstate->saveslot;
    econtext->ecxt_scantuple = wstate->saveslot;
}

static List *
func_get_vals(WindowStatePerFunction funcstate, long int start,
			  long int end, bool is_trail)
{
	List *ret = NIL;
	long int n;

	elog(DEBUG1, "-- processing %s - start %li, end %li",
		 is_trail ? "trail" : "lead", start, end);

	if (start == -1)
		return NIL;

	if (is_trail)
	{
		if (OidIsValid(funcstate->invprelimfn_oid))
		{
			if (funcstate->trail_done >= end)
				return NIL;

			if (start < funcstate->trail_done)
				start = funcstate->trail_done;
		}
	}
	else
	{
		if (OidIsValid(funcstate->invprelimfn_oid))
		{
			if (funcstate->lead_done >= end)
				return NIL;

			if (start < funcstate->lead_done)
				start = funcstate->lead_done;
		}
	}

	n = start;


	/* 
	 * If the frame is trailing, go up to the tuple before the edge, otherwise
	 * include the edge tuple.
	 */
	while (((is_trail && n < end) || (!is_trail && n <= end))
		   && n < funcstate->frame_mem_used)
	{
		elog(DEBUG1, "returning tuple at pos %li", n);
		ret = lappend(ret, funcstate->frame_trans_vals[n++]);
	}
	elog(DEBUG1, "--");

	if (is_trail)
	{
		elog(DEBUG1, "setting trail done to %li", n);
		funcstate->trail_done = n - 1;
	}
	else
	{
		elog(DEBUG1, "setting lead_done to %li", n);
		funcstate->lead_done = n - 1;
	}
	return ret;
}

static void
process_vals(WindowState *wstate, WindowStatePerFunction funcstate,
		 	 List *vals, bool is_rows, bool is_add)
{
	ListCell *lc;
	WindowTransValue tv;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	FmgrInfo *transfn = NULL;

	if (vals == NIL)
		return;

	tv.transValue = funcstate->aggTransValue;
	tv.transValueIsNull = funcstate->aggTransValueIsNull;
	tv.noTransValue = funcstate->aggNoTransValue;
	
	foreach (lc, vals)
	{
		FunctionCallInfoData fcinfo;

		if (is_rows)
		{
			WindowRowsBuffer *wrb = (WindowRowsBuffer *)lfirst(lc);
			/* put the buffered value in as the second arg */
			fcinfo.arg[1] = wrb->transval->transValue;
			fcinfo.argnull[1] = wrb->transval->transValueIsNull;
			elog(DEBUG1, "processing buffer for row %llu, is %s",
				(unsigned long long) wrb->row_index, is_add ? "add" : "inv");
		}
		else
		{
			WindowRangeBuffer *wrb = (WindowRangeBuffer *)lfirst(lc);
			fcinfo.arg[1] = wrb->transval->transValue;
			fcinfo.argnull[1] = wrb->transval->transValueIsNull;
		}

		if (!transfn)
		{
			if (is_add)
				transfn = &funcstate->prelimfn;
			else
				transfn = &funcstate->invprelimfn;
		}



		tv.transValue = invoke_agg_trans_func(*transfn,
								  funcstate->numargs,
								  tv.transValue,
								  &(tv.noTransValue),
								  &(tv.transValueIsNull),
								  funcstate->aggTranstypeByVal,
								  funcstate->aggTranstypeLen,
								  &fcinfo, (void *)wstate,
								  wstate->transcontext,
								  econtext->ecxt_per_tuple_memory);
		elog(DEBUG1, "process_vals calls invoke_agg_trans_func");
	}

	funcstate->aggTransValue = tv.transValue;
	funcstate->aggTransValueIsNull = tv.transValueIsNull;
	funcstate->aggNoTransValue = tv.noTransValue;
}

/*
 * Now that we've populated the frame buffers and adjusted the trail and
 * lead edges, it's time to calculate the actual transition values/
 * window function results for this outer row.
 */
static void
calculateTransVals(WindowState *wstate)
{
	int level = -1;
	Window *window = (Window *)wstate->ps.plan; 
	ListCell *lkey;

	foreach (lkey, window->windowKeys)
	{
		WindowKey *key = lfirst(lkey);
		ListCell *func;
		WindowStatePerLevel level_state = &wstate->level_state[++level];
		
		foreach(func, level_state->level_funcs)
		{
			int funcno = (int)lfirst_int(func);
			WindowStatePerFunction funcstate = &wstate->func_state[funcno];
			List *vals;

			if (!FRAME_IS_COMPLEX(funcstate, level_state))
				continue;

			if (funcstate->isAgg)
			{
				/*
				 * Here, we remove values from the transition value
				 * which have fallen out of the frame and add those 
				 * which have come into the frame.
				 *
				 * This works for functions which do and do not support
				 * inversion because func_get_vals_for_add() will return
				 * the whole from for functions which do not support inversion
				 * and only the difference between the previous frame edge and
				 * current frame edge for those that do.
				 */
				if (OidIsValid(funcstate->invprelimfn_oid))
				{
					long int end;
					
					/* 
					 * Process transition values then inverse transition
					 * values. This is important for processing of 
					 * BETWEEN FOLLOWING N AND FOLLOWING M frames because
					 * if we do inversion values first, they have no
					 * effect since the transition value is uninitialised.
					 */
					vals = func_get_vals(funcstate, funcstate->prev_lead_edge,
										 funcstate->cur_lead_edge, false);
	
					process_vals(wstate, funcstate, vals, key->frame->is_rows, 
								 true);

					end = funcstate->cur_trail_edge;
					if (!key->frame->is_rows)
					{
						/* The current trail edge hovers somewhere between the
						 * previous edge and the next value -- unless it is
						 * equal to it. If it is not equal, say it is so that
						 * process_vals() will return something.
						 */
						if (!funcstate->cur_trail_edge_equal)
							end++;
					}
					vals = func_get_vals(funcstate, funcstate->prev_trail_edge,
										 end, true);
	
					process_vals(wstate, funcstate, vals, key->frame->is_rows,
								 false);
				}
				else
				{
					/* 
					 * We must manually manage the frame so, give the range as
					 * the whole frame.
					 */
					long int start;
	
					if (funcstate->cur_trail_edge == -1)
						start = 0;
					else
						start = funcstate->cur_trail_edge;
					
					vals = func_get_vals(funcstate, start,
										 funcstate->cur_lead_edge, false);
					process_vals(wstate, funcstate, vals, key->frame->is_rows,
								 true);
				}
			}
			else
			{
				/* window function case */
				FunctionCallInfoData fcinfo;
				ExprContext *econtext = wstate->ps.ps_ExprContext;
				int argno = initFcinfo(funcstate->wrxstate, &fcinfo, 
									   funcstate, econtext);

				/* 
				 * Window functions like LEAD and LAG need to know their
				 * WindowStatePerFunction so that they can peek into the
				 * buffer.
				 */
				wstate->cur_funcstate = funcstate;

				InitFunctionCallInfoData(fcinfo, 
										 &funcstate->windowfn,
										 argno,
										 (void *) funcstate->wrxstate, 
										 NULL);
				{
			
					funcstate->win_value = FunctionCallInvoke(&fcinfo);
					funcstate->win_value_is_null = fcinfo.isnull;
				}
			}
		}
	}
}

static void
invokeNonTrivialFuncs(WindowState *wstate)
{
	/* firstly, populate the frames */
	elog(DEBUG1, "processing tuple");
	DB_TTS(wstate->curslot);
	populateFrameBuffers(wstate);

	/* next, calculate the transition value for each function */
	calculateTransVals(wstate);
}

static void
finaliseFuncs(WindowState *wstate)
{
	int funcno;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

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
	bool found_complex_func = false;

	elog(DEBUG1, "======= START FRAME PROCESS =====");
	elog(DEBUG1, "row index %lli",
		 (long long) wstate->row_index);
	
	invokeTrivialFuncs(wstate, &found_complex_func);

	/* 
	 * If there are no functions/frames which need non-trivial window
	 * management, we're done.
	 */
	if (found_complex_func)
		invokeNonTrivialFuncs(wstate);

	finaliseFuncs(wstate);
		

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

static void
init_bound_frame_edge_expr(WindowFrameEdge *edge, TupleDesc desc,
						   AttrNumber attnum,
						   bool is_trail,
						   WindowStatePerLevel level_state,
						   WindowState *wstate)
{
	ExprState *n;
	Expr *expr;
	Expr *varexpr;
	Oid exprrestype;
	Oid ltype, rtype;
	HeapTuple tup;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	bool isNull = false;
	char *oprname;
	Form_pg_operator opr;
	int32 vartypmod = desc->attrs[attnum - 1]->atttypmod;
	
	Insist(EDGE_IS_BOUND(edge));

	ltype = desc->attrs[attnum - 1]->atttypid;
	rtype = exprType(edge->val);
	
	varexpr = (Expr *)makeVar(0, attnum, ltype, vartypmod, 0);
	n = ExecInitExpr((Expr *)edge->val, (PlanState *) wstate);

	if (is_trail)
		level_state->trail_range = ExecEvalExpr(n, econtext, &isNull, NULL);
	else
		level_state->lead_range = ExecEvalExpr(n, econtext, &isNull, NULL);
	pfree(n);

	if ((edge->kind == WINDOW_BOUND_PRECEDING && 
		 level_state->col_sort_asc[0]) ||
		(edge->kind == WINDOW_BOUND_FOLLOWING &&
		 !level_state->col_sort_asc[0]))
	{
		oprname = "-";
	}
	else
	{
		oprname = "+";
	}

		
	tup = SearchSysCache(OPERNAMENSP,
						 CStringGetDatum(oprname),
						 ObjectIdGetDatum(ltype),
						 ObjectIdGetDatum(rtype),
						 ObjectIdGetDatum(PG_CATALOG_NAMESPACE));

	/*
	 * If we didn't find an operator, it's probably because the user has
	 * specified a RANGE parameter which does not have an implicit cast
	 * to the sorting column. So, see what operator the parser found
	 * via oper() and arrange for the LHS to be cast to that for the purpose
	 * of the expression evaluation.
	 */
	if (!HeapTupleIsValid(tup))
	{
		List *oprlist = lappend(NIL, makeString(oprname));

		tup = oper(NULL, oprlist, ltype, rtype, true, -1);

		list_free_deep(oprlist);

		/* We have to find it now */
		Insist(HeapTupleIsValid(tup));
		
		opr = (Form_pg_operator) GETSTRUCT(tup);

		/* this is why we're here */
		Insist(ltype != opr->oprleft);
		
		varexpr = (Expr *)coerce_to_target_type(NULL, (Node *)varexpr, 
												ltype, opr->oprleft, vartypmod,
												COERCION_EXPLICIT,
												COERCE_IMPLICIT_CAST);
	}
	else
		opr = ((Form_pg_operator) GETSTRUCT(tup));
	

	exprrestype = opr->oprresult;
	expr = make_opclause(HeapTupleGetOid(tup), exprrestype,
						 false, varexpr, 
						 (Expr *)edge->val);
	((OpExpr *)expr)->opfuncid = opr->oprcode;
	ReleaseSysCache(tup);

	/*
	 * If the frame edge operation returns a different type
	 * to the input type, we must coerce it back.
	 */
	if (ltype != exprrestype)
	{
		expr = 
			(Expr *)coerce_to_target_type(NULL,
										  (Node *)expr, 
									 	  exprrestype,
									 	  ltype, vartypmod,
									 	  COERCION_EXPLICIT,
										  COERCE_IMPLICIT_CAST);
	}

	Insist(PointerIsValid(expr));

	n = ExecInitExpr(expr, (PlanState *) wstate);
	
	if (is_trail)
		level_state->trail_expr = n;
	else
		level_state->lead_expr = n;
}

static long int
init_rows_edge_value(ExprState *rows_expr, WindowBoundingKind kind,
					 ExprContext *econtext)
{

	long int rows_param;
	bool isnull = false;
	
	rows_param = ExecEvalExpr(rows_expr,
							  econtext,
							  &isnull,
							  NULL);

	Insist(!isnull);

	if (kind == WINDOW_BOUND_PRECEDING)
		rows_param = -rows_param;

	return rows_param;
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
	int last_nattrs = 0; /* number of attrs in the last key which had attrs */
	AttrNumber *last_sortcolidx = NULL;

	resetPartFuncFrameBuf(wstate);

	foreach(lc, node->windowKeys)
	{
		WindowKey *key = lfirst(lc);
		WindowFrame *frame;
		int funcno;
		List *lvl_funcs = NIL;
		WindowStatePerLevel level_state;
		int ncols = key->numSortCols;
		int i;

		if (ncols)
		{
			last_nattrs = ncols;
			last_sortcolidx = key->sortColIdx;
		}

		level++;
		
		level_state = &wstate->level_state[level];

		level_state->nsortcols = last_nattrs;
		level_state->sortcolidx = last_sortcolidx;

		/* 
		 * We must allocate memory here, even if we have no sort columns 
		 * because we need to store the result of the frame edge calculation
		 * operations inside set_edge_val().
		 *
		 * XXX: we could special case CURRENT ROW.
		 */
		level_state->trail_edge_d = palloc(sizeof(Datum) * ncols);
		level_state->trail_edge_nulls = palloc(sizeof(bool) * ncols);
	
		level_state->lead_edge_d = palloc(sizeof(Datum) * ncols);
		level_state->lead_edge_nulls = palloc(sizeof(bool) * ncols);

		level_state->col_sort_asc = palloc(sizeof(bool) * ncols);

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
				
				if (!frame || (!funcstate->isAgg && !funcstate->allowframe &&
							   !IS_LEAD_LAG(funcstate->wrxstate->winkind)))
				{
					funcstate->trivial_frame = true;
				}
			}
	
			/* 
			 * We say the function is trivial to invoke if its frame is defined
			 * as ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW with no
			 * exclusion clause OR it is a window function without a frame.
			 */
			if (funcstate->isAgg && frame && frame->is_rows && 
				frame->trail->kind == WINDOW_UNBOUND_PRECEDING && 
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
					level_state->empty_frame = true;
				}
			}
		}

		level_state->level_funcs = lvl_funcs;

		level_state->is_rows = frame->is_rows;

		/* what sort order did the user specify for each key? */
		for (i = 0; i < ncols; i++)
		{
			Oid sortop = key->sortOperators[i];
			Form_pg_operator op;
			HeapTuple optup = SearchSysCache(OPEROID,
							   				 ObjectIdGetDatum(sortop),
							   				 0, 0, 0);
			Insist(HeapTupleIsValid(optup));
			op = (Form_pg_operator)GETSTRUCT(optup);
			ReleaseSysCache(optup);

			if (strcmp(NameStr(op->oprname), "<") == 0)
				level_state->col_sort_asc[i] = true;
			else
				level_state->col_sort_asc[i] = false;
		}
		
		/* now, initialize the actual frame */
		if (frame)
		{
			if (frame->is_rows)
			{
				ExprContext *econtext = wstate->ps.ps_ExprContext;

				if (EDGE_IS_BOUND(frame->trail))
				{
					level_state->trail_expr =
						ExecInitExpr((Expr *)key->frame->trail->val, 
									 (PlanState *) wstate); 

					level_state->trail_rows = 
						init_rows_edge_value(level_state->trail_expr,
											 frame->trail->kind,
											 econtext);
					level_state->trail_edge_set = true;
				}
				else
					level_state->trail_rows = 0L;

				if (EDGE_IS_BOUND(frame->lead))
				{
					level_state->lead_expr = 
						ExecInitExpr((Expr *)key->frame->lead->val, 
									 (PlanState *) wstate); 

					level_state->lead_rows = 
						init_rows_edge_value(level_state->lead_expr,
											 frame->lead->kind,
											 econtext);

					level_state->lead_edge_set = true;
				}
				else
					level_state->lead_rows = 0L;
			}
			else
			{
				TupleDesc desc = ExecGetResultType(wstate->ps.lefttree); 
				
				/* we only need the subtraction function for bound frames */
				if (EDGE_IS_BOUND(frame->trail))
				{
					init_bound_frame_edge_expr(frame->trail, desc,
											   level_state->sortcolidx[0],
											   true, level_state, wstate);
				}

				if (EDGE_IS_BOUND(frame->lead))
				{
					init_bound_frame_edge_expr(frame->lead, desc,
											   level_state->sortcolidx[0],
											   false, level_state, wstate);
				}
			}
		}

		/* look for empty frames */
		if (frame->trail->kind == WINDOW_UNBOUND_FOLLOWING ||
			frame->lead->kind == WINDOW_UNBOUND_PRECEDING ||
			(frame->trail->kind == WINDOW_BOUND_FOLLOWING &&
			 frame->lead->kind == WINDOW_BOUND_PRECEDING))
		{
			level_state->empty_frame = true;
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
					if (level_state->lead_rows < level_state->trail_rows)
						level_state->empty_frame = true;
				}
				else
				{
					if (level_state->lead_rows < level_state->trail_rows)
						level_state->empty_frame = true;
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


				attnum = level_state->sortcolidx[0];
				optup = ordering_oper(tup->attrs[attnum - 1]->atttypid,
									  false);
				
				ordfuncid = oprfuncid(optup); 
				elog(DEBUG1, "found operator %u", oprid(optup));
				ReleaseSysCache(optup);

				fmgr_info(ordfuncid, &fcinfo);
				
				/* is trail less than lead */
				if (level_state->col_sort_asc[0])
					datum = FunctionCall2(&fcinfo, level_state->trail_range,
										  level_state->lead_range);
				else
					/* reverse argument order */
					datum = FunctionCall2(&fcinfo, level_state->lead_range,
										  level_state->trail_range);
			


				if (is_pre)
				{
					if (DatumGetBool(datum))
						level_state->empty_frame = true;
				}
				else
				{
					if (!DatumGetBool(datum))
						level_state->empty_frame = true;
				}
			}
		}


	}
}

static FmgrInfo *
get_ltfuncs(TupleDesc tupdesc, int numCols, AttrNumber *matchColIdx)
{
	FmgrInfo   *ltfunctions = (FmgrInfo *) palloc(numCols * sizeof(FmgrInfo));
	int			i;

	for (i = 0; i < numCols; i++)
	{
		AttrNumber	att = matchColIdx[i];
		Oid			typid = tupdesc->attrs[att - 1]->atttypid;
		Oid			lt_function;
    	Operator    optup;

	    optup = ordering_oper(typid, false);
	    lt_function = oprfuncid(optup);
		ReleaseSysCache(optup);
		fmgr_info(lt_function, &ltfunctions[i]);
	}

	return ltfunctions;
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
	TupleDesc desc;

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
	wstate->saveslot = ExecInitExtraTupleSlot(estate);

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

	desc = ExecGetResultType(wstate->ps.lefttree);

	/* Precompute fmgr lookup data for partition key equality function. */
	if (node->numPartCols > 0)
	{
		wstate->eqfunctions =
			execTuplesMatchPrepare(desc,
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
		WindowFrame *f;
		WindowStatePerLevel lvl = &wstate->level_state[++i];
		
		lvl->level = i;

		/* if empty, get prev key */
		if (key->numSortCols)
		{
			int j;
			lvl->eqfunctions = execTuplesMatchPrepare(desc,
									   				  key->numSortCols,
									   				  key->sortColIdx);

			lvl->ltfunctions = get_ltfuncs(desc,
										   key->numSortCols,
										   key->sortColIdx);

			lvl->col_types = palloc(sizeof(Oid) * key->numSortCols);
			lvl->col_typlens = palloc(sizeof(int2) * key->numSortCols);
			lvl->col_typbyvals = palloc(sizeof(bool) * key->numSortCols);

			for (j = 0; j < key->numSortCols; j++)
			{
				AttrNumber attnum = key->sortColIdx[j];
				Form_pg_attribute attr = desc->attrs[attnum - 1];

				lvl->col_types[j] = attr->atttypid;
				lvl->col_typlens[j] = attr->attlen;
				lvl->col_typbyvals[j] = attr->attbyval;
			}
		}
		else
		{
			if (i - 1 >= 0)
			{
				WindowStatePerLevel last_level;

				last_level = &wstate->level_state[i - 1];

				lvl->eqfunctions = last_level->eqfunctions;
				lvl->ltfunctions = last_level->ltfunctions;
				lvl->col_types = last_level->col_types;
				lvl->col_typlens = last_level->col_typlens;
				lvl->col_typbyvals = last_level->col_typbyvals;
			}
		}

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

			lvl->default_frame = true;

			f = key->frame = (WindowFrame *)makeNode(WindowFrame);
			f->trail = (WindowFrameEdge *)makeNode(WindowFrameEdge);
			f->lead = (WindowFrameEdge *)makeNode(WindowFrameEdge);

			/* 
			 * The default frame is:
			 *
			 *   RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
			 */
			f->is_rows = false;
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
	
	wstate->need_peercount = false;

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
		int			funcno;
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

		/* XXX should be an Assert here? */
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
#define NUM_AGG_FNS_TO_CHECK 5
			int c;
			Oid to_check[NUM_AGG_FNS_TO_CHECK];
			Oid aggtranstype;
			Expr *transfnexpr,
				 *finalfnexpr,
				 *invtransfnexpr,
				 *invprelimfnexpr,
				 *prelimfnexpr;
			Datum textInitVal;
			
			Insist(winref->winlevel < wstate->numlevels);

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
			to_check[4] = funcstate->prelimfn_oid = aggform->aggprelimfn;
			
			
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
									funcstate->prelimfn_oid,
									funcstate->invtransfn_oid,
									funcstate->invprelimfn_oid,
									&transfnexpr, &finalfnexpr, &prelimfnexpr,
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
			if (OidIsValid(funcstate->prelimfn_oid))
			{
				fmgr_info(funcstate->prelimfn_oid, &funcstate->prelimfn);
				funcstate->prelimfn.fn_expr = (Node *) prelimfnexpr;
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
			
			wrxstate->winkind = WINKIND_AGGREGATE;
		}
		else if (isWin)
		{
			HeapTuple win_tuple;
			Form_pg_window winform;
			Oid windowfn_oid = InvalidOid;
			Const *refptr;
			ExprState *xtrastate;
			
			win_tuple = SearchSysCache(WINFNOID,
									   ObjectIdGetDatum(winref->winfnoid),
									   0, 0, 0);
			if (!HeapTupleIsValid(win_tuple))
				elog(ERROR, "cache lookup failed for window function %u",
					 winref->winfnoid);

			winform = (Form_pg_window) GETSTRUCT(win_tuple);

			/* 
			 * Lookup ordinary window implementation function info.
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
			
			wrxstate->winkind = winform->winkind;

			Assert(OidIsValid(windowfn_oid));

			/* Check that window function owner has permission to call the
			 * implementation function. 
			 */
			aclresult = pg_proc_aclcheck(windowfn_oid, 
										 winOwner, 
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_PROC,
							   get_func_name(windowfn_oid));
			
			ReleaseSysCache(win_tuple);
			
			funcstate->windowfn_oid = windowfn_oid;
			funcstate->allowframe = winform->winallowframe;
			
			fmgr_info(funcstate->windowfn_oid, &funcstate->windowfn);			
			
			/* 
			 * Inject extra argument. If you change these, change them in the
			 * framed window function case below.
			 */
			refptr = makeNode(Const);
			refptr->consttype = INTERNALOID;
			refptr->constlen = 4;
			refptr->constvalue = PointerGetDatum(wrxstate);
			refptr->constisnull = false;
			refptr->constbyval = true;
			
			xtrastate = ExecInitExpr((Expr *) refptr, (PlanState *) wstate);
			
			/* 
			 * Save a pointer to the old args for use in the framed 
			 * case below
			 */
			wrxstate->args = lcons(xtrastate, wrxstate->args);

			/* 
			 * Initialize byval and typlen for framed funcs. Of course we're
			 * creating an entry here for the inserted refptr but we do so
			 * for consistency.
			 */
			if (funcstate->allowframe || IS_LEAD_LAG(wrxstate->winkind))
			{
				ListCell *lc;
				int numargs = list_length(wrxstate->args);
				int argno = 0;

				wrxstate->argtypbyval = palloc(sizeof(bool) * numargs);
				wrxstate->argtyplen = palloc(sizeof(int16) * numargs);

				foreach(lc, wrxstate->args)
				{
					if (argno == 0)
					{
						/* internal arg */
						wrxstate->argtypbyval[argno] = true;
						wrxstate->argtyplen[argno] = INTERNALOID;
					}
					else
					{
						Oid typid = inputTypes[argno - 1];
						int16 len;
						bool byval;

						get_typlenbyval(typid, &len, &byval);
						wrxstate->argtypbyval[argno] = byval;
						wrxstate->argtyplen[argno] = len;
					}
					argno++;
				}
			}
			
			/* 
			 * Only set the peer count if we're referencing an actual
			 * window key level and if the peer count is required. This
			 * ensures that subsequent iterations do not overwrite the peer
			 * count flag.
			 */
			if (PointerIsValid(funcstate->wlevel) && winform->winpeercount)
			{
				funcstate->wlevel->need_peercount = winform->winpeercount;
				wstate->need_peercount = winform->winpeercount;
			}
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
	freeInputBuffer(node);

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
		result = 0.0; /* quieten GCC */
	}
	else if ( arg1 == 1 && arg2 == 1 )
	{
		result = 0.0; /* per Oracle */
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
		result = 0.0; /* quieten GCC */
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
		
	return construct_array(work, 2, INT8OID, 8, true, 'd');
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
	deconstruct_array(pair, INT8OID, 8, true, 'd', &work, NULL, &len);
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

static Datum
lead_lag_internal(PG_FUNCTION_ARGS, bool is_lead, bool *isnull)
{
	int64 offset = 0;
	Datum val_expr = 0;
	bool val_expr_null = true;
	WindowRefExprState *ref_state = (WindowRefExprState *) PG_GETARG_POINTER(0);
	WindowState *window_state = ref_state->windowstate;
	WindowStatePerFunction funcstate = window_state->cur_funcstate;
	long int pos;

	Insist(PG_NARGS() >= 2);
	
	if (PG_NARGS() > 3)
	{
		val_expr = PG_GETARG_DATUM(3);
		val_expr_null = PG_ARGISNULL(3);
	}
	
	if (PG_NARGS() > 2)
	{
		if (PG_ARGISNULL(2))
		{
			if (is_lead)
				elog(ERROR, "LEAD offset cannot be NULL");
			else
				elog(ERROR, "LAG offset cannot be NULL");
		}

		offset = PG_GETARG_INT64(2);
	}
	
	if (PG_NARGS() == 2)
	{
	 	offset = 1; /* default */
	}

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s offset cannot be negative",
						is_lead ? "LEAD" : "LAG")));

	/* XXX: should check for overflow */
	if (is_lead)
		pos = funcstate->cur_trail_edge + (long int)offset;
	else
		pos = funcstate->cur_lead_edge - (long int)offset;
	
	if (pos > funcstate->frame_mem_used - 1 || pos < 0)
	{
		if (!val_expr_null)
			return val_expr;
		else
		{
			*isnull = true;
			return (Datum)0;
		}
	}
	else
	{
		Datum d;
		WinFuncBufferData *dat;

		dat = (WinFuncBufferData *)funcstate->frame_trans_vals[pos];

		if (dat->argnull[0])
		{
			*isnull = true;
			return (Datum)0;
		}
		
		d = dat->arg[0];
		return d;
	}
}

/*
 * Initial implementation of LEAD(col, offset, value_expr)
 */
Datum
lead_int(PG_FUNCTION_ARGS)
{
	bool isnull = false;
	Datum d;
	
	d = lead_lag_internal(fcinfo, true, &isnull);

	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(d);
	
}

Datum
lead_bool(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_char(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_cidr(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_circle(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_float4(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_float8(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_inet(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_interval(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_line(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_lseg(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_macaddr(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_smallint(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_int8(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_money(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_name(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_numeric(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_oid(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_path(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_point(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_polygon(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_reltime(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_text(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_tid(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_time(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_timestamp(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_timestamptz(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_timetz(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_varbit(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_varchar(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_xid(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_bool(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_char(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_cidr(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_circle(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_float4(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_float8(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_inet(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_interval(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_line(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_lseg(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_macaddr(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_smallint(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_int4(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_int8(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_money(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_name(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_numeric(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_oid(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_path(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_point(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_polygon(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_reltime(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_text(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_tid(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_time(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_timestamp(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_timestamptz(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_timetz(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_varbit(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_varchar(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_xid(PG_FUNCTION_ARGS)
{   
    bool isnull = false;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

static Datum
last_value_internal(WindowRefExprState *ref_state, bool *isnull)
{
	WindowState *window_state = ref_state->windowstate;
	WindowStatePerFunction funcstate = window_state->cur_funcstate;
	long int pos;
	WinFuncBufferData *dat;

	pos = funcstate->cur_lead_edge;

	if (pos >= funcstate->frame_mem_used &&
		(funcstate->frame_mem_used - 1) >= funcstate->cur_trail_edge)
		pos = funcstate->frame_mem_used - 1;
		
	if (pos < 0 || pos >= funcstate->frame_mem_used)
	{
		*isnull = true;
		return (Datum)0;
	}

	dat = (WinFuncBufferData *)funcstate->frame_trans_vals[pos];

	if (dat->argnull[0])
	{
		*isnull = true;
		return (Datum)0;
	}
	*isnull = false;
	return dat->arg[0];
}

Datum
last_value_smallint(PG_FUNCTION_ARGS)
{
	Datum d;
	bool isnull = false;
	
	d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
							&isnull);
	
	if (isnull)
	   PG_RETURN_NULL();

	return d;
}

Datum
last_value_int(PG_FUNCTION_ARGS)
{
	Datum d;
	bool isnull = false;
	
	d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
							&isnull);
	
	if (isnull)
	   PG_RETURN_NULL();

	return d;
}

Datum
last_value_bigint(PG_FUNCTION_ARGS)
{
	Datum d;
	bool isnull = false;
	
	d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
							&isnull);
	
	if (isnull)
	   PG_RETURN_NULL();

	return d;
}

Datum
last_value_numeric(PG_FUNCTION_ARGS)
{
	Datum d;
	bool isnull = false;
	
	d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
							&isnull);
	
	if (isnull)
	   PG_RETURN_NULL();

	return d;
}

Datum
last_value_text(PG_FUNCTION_ARGS)
{
	Datum d;
	bool isnull = false;
	
	d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
							&isnull);
	
	if (isnull)
	   PG_RETURN_NULL();

	return d;
}

Datum
last_value_bool(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_char(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_cidr(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_circle(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_float4(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_float8(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_inet(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_interval(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_line(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_lseg(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_macaddr(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_money(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_name(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_oid(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_path(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_point(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_polygon(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_reltime(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_tid(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_time(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_timestamp(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_timestamptz(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_timetz(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_varbit(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_varchar(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
last_value_xid(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = last_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}



static Datum
first_value_internal(WindowRefExprState *ref_state, bool *isnull)
{
	WindowState *window_state = ref_state->windowstate;
	WindowStatePerFunction funcstate = window_state->cur_funcstate;
	long int pos;
	WinFuncBufferData *dat;

	/* 
	 * The sematics of FIRST_VALUE in Oracle requires that if the trail
	 * edge is not in play yet (i.e., the frame is something like
	 * BETWEEN 2 PRECEDING AND 2 FOLLOWING) then we must use the first value
	 * in the FRAME.
	 *
	 * It gets even more difficult than that though. We want the first value
	 * in the frame but cur_trail_edge points to the value in memory at or
	 * just before the trail edge of the frame. So, if the trail edge is 
	 * equal to an actual value, use it. Otherwise, use the next value
	 * along.
	 */

	pos = funcstate->cur_trail_edge;

	if (funcstate->wlevel->is_rows == false &&
		funcstate->cur_trail_edge_equal == false &&
		pos + 1 < funcstate->frame_mem_used && 
		pos + 1 <= funcstate->cur_lead_edge)
	{
		pos++;
	}
	else if (funcstate->wlevel->is_rows == true &&
			 pos == -1)
	{
		pos++;
	}
	
	if (pos < 0 || pos > funcstate->cur_lead_edge ||
		pos >= funcstate->frame_mem_used)
	{
		*isnull = true;
		return (Datum)0;
	}

	dat = (WinFuncBufferData *)funcstate->frame_trans_vals[pos];

	if (dat->argnull[0])
	{
		*isnull = true;
		return (Datum)0;
	}
	*isnull = false;
	return dat->arg[0];
}

Datum
first_value_bool(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_char(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_cidr(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_circle(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_float4(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_float8(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_inet(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_interval(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_line(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_lseg(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_macaddr(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_smallint(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_int4(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_int8(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_money(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_name(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_numeric(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}


Datum
first_value_oid(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_path(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_point(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_polygon(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_reltime(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_text(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_tid(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_time(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_timestamp(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_timestamptz(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_timetz(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_varbit(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_varchar(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

Datum
first_value_xid(PG_FUNCTION_ARGS)
{
    Datum d;
    bool isnull = false;

    d = first_value_internal((WindowRefExprState *)PG_GETARG_POINTER(0),
                            &isnull);

    if (isnull)
       PG_RETURN_NULL();

    return d;
}

#endif
