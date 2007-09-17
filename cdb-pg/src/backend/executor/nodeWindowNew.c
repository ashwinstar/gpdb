/*-------------------------------------------------------------------------
 *
 * nodeWindowNew.c
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
#include "utils/tuplestorenew.h"

#ifndef OLD_WINDOWNODE

/* The initial size (in bytes) for an entry in the frame buffer. */
#define FRAMEBUFFER_ENTRY_SIZE 1024

/* 
 * A struct to hold all the data relevant to a value, used for
 * buffering.
 */
typedef struct WindowValue
{
	Datum				value;
	bool				valueIsNull;
} WindowValue;

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

	/* List of functions in this key level */
	List	   *level_funcs;

	/* The framing clause */
	WindowFrame *frame;

	/* Indicate if all functions have trivial frames. */
	bool trivial_frames_only;

	/* if the user didn't specify a frame, we use the default */
	bool default_frame;

	/* Indicate if the frame is a ROW frame or a RANGE frame. */
	bool is_rows;

	/* Indicate if its functions are LEAD/LAG */
	bool has_lead_lag;

	/* user can specify ascending or descending order, record it here */
	bool *col_sort_asc;

	/* The partial order by keys. There is at least one sort key in this array. 
	 * It is used for convenience to find the order by keys since we need to
	 * store them in the frame buffer for RANGE-based framing.
	 */
	int numSortCols;
	AttrNumber *sortColIdx;
	Oid *sortOperators;

	/* column type data */
	Oid *col_types;
	int2 *col_typlens;
	bool *col_typbyvals;

	/* 
	 * Frame is contradictory and no tuples will fall into the frame, for
	 * example: BETWEEN 10 FOLLOWING AND 10 PRECEDING.
	 */
	bool	empty_frame;

	/* XXX: merge the following 4 fields into 2 */
	/* state for rows frame is simple (for now) */
	long int trail_rows;
	long int lead_rows;

	/* state for range frame is more complex */
	Datum trail_range;
	Datum lead_range;

	/*
	 * The number of rows between the trailing edge and the current_row,
	 * and between the leading edge and the current_row.
	 *
	 * For preceding edges, these numbers are 0 or negative.
	 */
	long int num_trail_rows;
	long int num_lead_rows;

	/* state for frame edges */
	ExprState *trail_expr;
	ExprState *lead_expr;

	/* The frame buffer to buffer the intermediate results for
	 * the functions in this level key.
	 */
	struct WindowFrameBufferData *frame_buffer;

	/* These two readers are pointing to the trailing and leading edges
	 * of this frame, respectively. 
	 */
	NTupleStoreAccessor *trail_reader;
	NTupleStoreAccessor *lead_reader;

	/* Indicate if lead_reader is pointing to the exact edge position.
	 * With this flag, we don't need to do the tuple comparison again
	 * during checking if there are sufficient data to generate an
	 * output.
	 */
	bool lead_ready;

	/* Indicate if funcstate->aggTransValue actually contains the aggregate
	 * value.
	 */
	bool agg_filled;
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

	/* Indicate if this function requires a range cumulative frame. */
	bool                cumul_frame;
	
	bool                winpeercount;

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

	/* the intermediate transition value */
	Datum				aggTransValue;
	bool				aggTransValueIsNull;
	bool				aggNoTransValue;

	/* the final transition value for output tuples */
	Datum               final_aggTransValue;
	bool                final_aggTransValueIsNull;
	bool                final_aggNoTransValue;

	/* frame does not require buffering and complexity of invokeWindowFuncs() */
	bool				trivial_frame;

	/* The index for the intermediate value of this function when serializing
	 * its value along with others before storing them into frame buffers.
	 *
	 * The initial value is -1.
	 */
	int                 serial_index;
} WindowStatePerFunctionData;

#define FRAME_TRAIL_ROWS 	0
#define FRAME_TRAIL_RANGE	1
#define FRAME_LEAD_ROWS		2
#define FRAME_LEAD_RANGE	3

#define EDGE_IS_BOUND(e) \
	(e->kind == WINDOW_BOUND_PRECEDING || \
	 e->kind == WINDOW_BOUND_FOLLOWING || \
     e->kind == WINDOW_DELAYED_BOUND_FOLLOWING || \
     e->kind == WINDOW_DELAYED_BOUND_PRECEDING)
#define EDGE_IS_CURRENT_ROW(e) \
	 (e->kind == WINDOW_CURRENT_ROW)
#define EDGE_IS_DELAYED(e) \
     (e->kind == WINDOW_DELAYED_BOUND_FOLLOWING || \
      e->kind == WINDOW_DELAYED_BOUND_PRECEDING)
#define EDGE_IS_BOUND_FOLLOWING(e) \
     (e->kind == WINDOW_BOUND_FOLLOWING || \
      e->kind == WINDOW_DELAYED_BOUND_FOLLOWING)
#define EDGE_IS_BOUND_PRECEDING(e) \
     (e->kind == WINDOW_BOUND_PRECEDING || \
      e->kind == WINDOW_DELAYED_BOUND_PRECEDING)
#define EDGE_IS_DELAYED_BOUND(e) \
     (e->kind == WINDOW_DELAYED_BOUND_PRECEDING || \
      e->kind == WINDOW_DELAYED_BOUND_FOLLOWING)
#define EDGE_EQ_CURRENT_ROW(level_state, e, is_lead) \
         (EDGE_IS_CURRENT_ROW(e) || \
		  ((EDGE_IS_BOUND_FOLLOWING(e) || \
			EDGE_IS_BOUND_PRECEDING(e)) && \
            ((!is_lead && \
		       (((level_state)->is_rows && (level_state)->trail_rows == 0) || \
				(!(level_state)->is_rows && (level_state)->trail_range == 0))) || \
             (is_lead && \
			  (((level_state)->is_rows && (level_state)->lead_rows == 0) || \
			   (!(level_state)->is_rows && (level_state)->lead_range == 0))))))

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
	NTupleStore *tuplestore;
	NTupleStoreAccessor *writer;

	/* The reader that is always pointed to the previous current_row */
	NTupleStoreAccessor *current_row_reader;

	/* The reader that reads tuples in the buffer */
	NTupleStoreAccessor *reader;

	/* Indicate if the last tuple in the buffer is the tuple that
	 * breaks the partition key.
	 */
	bool part_break;

	/* The total number of tuples in the store. */
	int64 num_tuples;
} WindowInputBufferData;

static void resetInputBuffer(WindowState *wstate);
static void freeInputBuffer(WindowState *wstate);

/********************************************************
 * Window Frame Buffer and its APIs
 *******************************************************/
/*
 * WindowFrameBuffer: this buffer holds intermediate aggregate values
 * or input arguments in order to generate the final aggregates for
 * the next output row.
 *
 * This buffer will trim old values when they are outside the lower bound
 * of the given frame provided to this buffer. This frame can be a RANGE
 * frame or a ROW frame. Note that if this frame is a RANGE frame, the
 * actual number of values in the buffer may vary over time.
 *
 * This buffer is designed to be one for each key level.  We could
 * also share one buffer among key levels if they have the same list
 * functions.
 */
typedef struct WindowFrameBufferData
{
	NTupleStore *tuplestore;
	NTupleStoreAccessor *writer;

	/* This reader is used to scan through the buffer to compute the final
	 * result.
	 */
	NTupleStoreAccessor *reader;

	/* Indicate if this buffer is for a RANGE frame or a ROWS frame. */
	bool is_rows;

	/* The trailing and leading number of rows from current_row if
	 * this frame is a ROW frame.
	 */
	long int trail_rows;
	long int lead_rows;

	/* The trailing and leading range from current_row if this frame
	 * is a RANGE frame.
	 */
	Datum trail_range;
	Datum lead_range;

	/* The reader that always points to the current_row. */
	NTupleStoreAccessor *current_row_reader;

	/* Pointer to the level state. Information about
	 * window functions can be found here.
	 */
	WindowStatePerLevel level_state;
} WindowFrameBufferData;
typedef WindowFrameBufferData *WindowFrameBuffer;

typedef struct FrameBufferEntryForRange
{
	/* the keys for the range frame. */
	Datum *keys;
	bool *nulls;
	List *func_values;
} FrameBufferEntryForRange;

typedef struct FrameBufferEntryForRows
{
	List *func_values;
} FrameBufferEntryForRows;

static WindowFrameBuffer createRangeFrameBuffer(Datum trail_range,
												Datum lead_range,
												int bytes);
static WindowFrameBuffer createRowsFrameBuffer(long int trail_rows,
											   long int lead_rows,
											   int bytes);
static WindowFrameBuffer resetFrameBuffer(WindowFrameBuffer buffer);
static void appendToFrameBuffer(WindowStatePerLevel level_state,
								WindowState *wstate,
								bool last_peer);
#if 0
static void trimFrameBuffer(WindowFrameBuffer buffer, NTupleStorePos *pos);
static List *getValueAtPos(WindowFrameBuffer buffer,
						   WindowStatePerLevel level_state,
						   NTupleStorePos *pos);
#endif
static void incrementCurrentRow(WindowFrameBuffer buffer, WindowState *wstate);
static bool hasEnoughDataInRange(WindowFrameBuffer buffer,
								 WindowStatePerLevel level_state,
								 ExprContext *econtext,
								 MemoryContext context,
								 Datum tail_range,
								 Datum lead_range);
static bool hasEnoughDataInRows(WindowFrameBuffer buffer,
								WindowStatePerLevel level_state,
								ExprContext *econtext,
								long int trail_rows,
								long int lead_rows);
static void computeFrameValue(WindowStatePerLevel level_state,
							  WindowState *wstate,
							  NTupleStoreAccessor *trail_reader,
							  NTupleStoreAccessor *lead_reader);

static void createFrameBuffers(WindowState *wstate);
static void resetFrameBuffers(WindowState *wstate);
static void resetTransValues(WindowStatePerLevel level_state,
							 MemoryContext context);
static void freeFrameBuffer(WindowFrameBuffer buffer);
static void freeFrameBuffers(WindowState *wstate);

/* the functions for the Window node */
static WindowState *makeWindowState(Window *window, EState *estate);
static void initializePartition(WindowState *wstate);
static bool checkOutputReady(WindowState *wstate);
static TupleTableSlot *fetchTupleSlotThroughBuf(WindowState *wstate);
static int initFcinfo(WindowRefExprState *wrxstate, FunctionCallInfoData *fcinfo,
					  WindowStatePerFunction funcstate, ExprContext *econtext);
static void adjustEdges(WindowFrameBuffer buffer, WindowState *wstate);
static TupleTableSlot *fetchCurrentRow(WindowState *wstate);
static TupleTableSlot *fetchTupleSlotThroughBuf(WindowState *wstate);
static void processTupleSlot(WindowState *wstate, TupleTableSlot *slot, bool last_peer);
static void * getCurrentValue(NTupleStoreAccessor *reader,
							  WindowStatePerLevel level_state);
static bool cmp_deformed_tuple(Datum *a, bool *a_nulls, Datum *b, bool *b_nulls,
							   bool *asc_cols, int ncols,
							   FmgrInfo *ltfuncs, FmgrInfo *eqfuncs,
							   MemoryContext evalContext, bool is_equal);
static FmgrInfo *get_ltfuncs(TupleDesc tupdesc, int numCols, AttrNumber *matchColIdx);
static void advanceKeyLevelState(WindowState *wstate, int min_level); 
static void init_frames(WindowState *wstate);
static void deform_window_tuple(TupleTableSlot *slot, int nattrs, AttrNumber *attnums, 
								Datum *values, bool *nulls, MemoryContext ctx);
static int findSerialIndex(WindowStatePerFunction funcstate,
						   WindowStatePerLevel level_state);
static void add_tuple_to_trans(WindowStatePerFunction funcstate, WindowState *wstate,
							   ExprContext *econtext);
static void set_lead_lag_framing(WindowStatePerLevel level_state,
								 WindowState *wstate);
static bool hasTuplesInFrame(WindowStatePerLevel level_state);
static Datum last_value_internal(WindowRefExprState *wrxstate, bool *isnull);
static Datum first_value_internal(WindowRefExprState *wrxstate, bool *isnull);

/*
 * initFrameBuffer -- initialize the frame buffer.
 *
 * Create the tuplestore and all accessors.
 */
static void
initFrameBuffer(WindowFrameBuffer buffer, int bytes)
{
	buffer->tuplestore = ntuplestore_create(bytes);

	/* Create accessors. */
	buffer->writer = ntuplestore_create_accessor(buffer->tuplestore, true);
	buffer->reader = ntuplestore_create_accessor(buffer->tuplestore, false);

	buffer->current_row_reader =
		ntuplestore_create_accessor(buffer->tuplestore, false);
}

/*
 * createRangeFrameBuffer -- create a new WindowFrameBuffer of the RANGE type.
 */
static WindowFrameBuffer
createRangeFrameBuffer(Datum trail_range, Datum lead_range, int bytes)
{
	WindowFrameBuffer buffer =
		(WindowFrameBuffer) palloc0(sizeof(WindowFrameBufferData));

	buffer->is_rows = false;
	buffer->trail_range = trail_range;
	buffer->lead_range = lead_range;

	initFrameBuffer(buffer, bytes);

	return buffer;
}

/*
 * createRowsFrameBuffer -- create a new WindowFrameBuffer of the ROWS type.
 */
static WindowFrameBuffer
createRowsFrameBuffer(long int trail_rows, long int lead_rows, int bytes)
{
	WindowFrameBuffer buffer =
		(WindowFrameBuffer) palloc0(sizeof(WindowFrameBufferData));

	buffer->is_rows = true;
	buffer->trail_rows = trail_rows;
	buffer->lead_rows = lead_rows;

	initFrameBuffer(buffer, bytes);

	return buffer;
}

/*
 * createFrameBuffers -- create frame buffers for all key levels inside
 * a given WindowState.
 */
static void
createFrameBuffers(WindowState *wstate)
{
	int level;
	int bytes;

	if (wstate->numlevels <= 0)
		return;
	
	bytes = ((work_mem * 1024L)/2) / wstate->numlevels;
	
	for (level=0; level < wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];

		Assert(level_state->frame_buffer == NULL);
		if (level_state->is_rows)
			level_state->frame_buffer =
				createRowsFrameBuffer(level_state->trail_rows,
									  level_state->lead_rows,
									  bytes);
		else
			level_state->frame_buffer =
				createRangeFrameBuffer(level_state->trail_range,
									   level_state->lead_range,
									   bytes);

		level_state->trail_reader =
			ntuplestore_create_accessor(level_state->frame_buffer->tuplestore, false);
		level_state->lead_reader =
			ntuplestore_create_accessor(level_state->frame_buffer->tuplestore, false);

		level_state->frame_buffer->level_state = level_state;
	}
}

/*
 * resetFrameBuffer -- reset or initialize a window frame buffer.
 *
 * Reset the tuplestore in the buffer, and re-create all accessors.
 *
 * The input argument 'buffer' can not be NULL.
 */
static WindowFrameBuffer
resetFrameBuffer(WindowFrameBuffer buffer)
{
	Assert(buffer != NULL);

	/* destroy all accessors */
	ntuplestore_destroy_accessor(buffer->writer);
	ntuplestore_destroy_accessor(buffer->reader);
	ntuplestore_destroy_accessor(buffer->current_row_reader);

	ntuplestore_reset(buffer->tuplestore);

	/* (Re)create accessors. */
	buffer->writer = ntuplestore_create_accessor(buffer->tuplestore, true);
	buffer->reader = ntuplestore_create_accessor(buffer->tuplestore, false);

	buffer->current_row_reader =
		ntuplestore_create_accessor(buffer->tuplestore, false);

	return buffer;
}

/*
 * resetFrameBuffers -- reset all frame buffers in a WindowState.
 */
static void
resetFrameBuffers(WindowState *wstate)
{
	int level;
	
	for (level=0; level < wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];

		if (level_state->frame_buffer)
		{
			/* destroy its accessors */
			if (level_state->trail_reader)
				ntuplestore_destroy_accessor(level_state->trail_reader);
			if (level_state->lead_reader)
				ntuplestore_destroy_accessor(level_state->lead_reader);

			level_state->frame_buffer = resetFrameBuffer(level_state->frame_buffer);

			/* (Re)create accessors */
			level_state->trail_reader =
				ntuplestore_create_accessor(level_state->frame_buffer->tuplestore, false);
			level_state->lead_reader =
				ntuplestore_create_accessor(level_state->frame_buffer->tuplestore, false);
			level_state->num_trail_rows = 0;
			level_state->num_lead_rows = 0;
			level_state->lead_ready = false;
		}
	}
}

/*
 * freeFrameBuffer -- release the space for a given WindowFrameBuffer.
 */
static void
freeFrameBuffer(WindowFrameBuffer buffer)
{
	if (buffer == NULL)
		return;
	ntuplestore_destroy_accessor(buffer->writer);
	ntuplestore_destroy_accessor(buffer->reader);
	ntuplestore_destroy_accessor(buffer->current_row_reader);

	ntuplestore_destroy(buffer->tuplestore);

	pfree(buffer);
}

/*
 * freeFrameBuffers -- release the space for all frame buffers.
 */
static void
freeFrameBuffers(WindowState *wstate)
{
	int level;
	
	for (level=0; level < wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];

		if (level_state->frame_buffer)
		{
			/* destroy its accessors */
			if (level_state->trail_reader)
				ntuplestore_destroy_accessor(level_state->trail_reader);
			if (level_state->lead_reader)
				ntuplestore_destroy_accessor(level_state->lead_reader);

			freeFrameBuffer(level_state->frame_buffer);
			level_state->frame_buffer = NULL;
		}
		
	}
}

/*
 * ensureSpace -- ensure that there is enough space in the buffer.
 */
static inline void
ensureSpace(char **p_serial_entry, Size *p_max_size,
			char *written_pos, Size written_len)
{
	Size current_len = written_pos - *p_serial_entry;
	if (current_len + written_len > *p_max_size)
	{
		*p_max_size += FRAMEBUFFER_ENTRY_SIZE;
		*p_serial_entry = repalloc(*p_serial_entry, *p_max_size);
	}
}

/*
 * findSerialIndex -- find the index for a given function in a key level
 * when serializing all intermediate results before writing them to
 * the frame buffer.
 */
static int findSerialIndex(WindowStatePerFunction funcstate,
						   WindowStatePerLevel level_state)
{
	ListCell *lc;
	int serial_index = -1;
	foreach(lc, level_state->level_funcs)
	{
		WindowStatePerFunction curr_funcstate = (WindowStatePerFunction) lfirst(lc);
		if (curr_funcstate->trivial_frame &&
			!curr_funcstate->winpeercount)
		{
			/* ignore trivial window functions */
			continue;
		}

		serial_index++;
		if (curr_funcstate == funcstate)
			break;
	}
	return serial_index;
}

/*
 * serializeValue -- serialize a given value (Datum) into a char array.
 *
 * The return pointer points to the next char position in the
 * given char array after writing the serialized string of the
 * given value.
 */
static char *
serializeValue(Datum value, bool isnull,
			   bool byvalue, int16 typelen,
			   char **p_serial_entry, Size *p_max_size, Size *p_len,
			   Size start_pos)
{
	char *written_pos = (*p_serial_entry) + start_pos;
	
	ensureSpace(p_serial_entry, p_max_size,
				written_pos, sizeof(bool));

	memcpy(written_pos, &isnull, sizeof(bool));
	written_pos += sizeof(bool); /* isnull col */

	if (!isnull)
	{
		if (byvalue)
		{
			ensureSpace(p_serial_entry, p_max_size,
						written_pos, sizeof(Datum));
			
			memcpy(written_pos, &value, sizeof(Datum));
			written_pos += sizeof(Datum);
		}
		else
		{
			Size real_size = datumGetSize(value, byvalue, typelen);
					
			ensureSpace(p_serial_entry, p_max_size,
						written_pos, real_size);
			
			memcpy(written_pos,
				   DatumGetPointer(value),
				   real_size);
			
			written_pos += real_size;
		}
	}

	return written_pos;
}

/*
 * serializeFuncs -- serialize values for all functions in a given
 * level and put it into a given space, starting from 'start_pos'.
 */
static void
serializeFuncs(WindowStatePerLevel level_state,
			   ExprContext *econtext,
			   char **p_serial_entry, Size *p_max_size, Size *p_len,
			   Size start_pos)
{
	ListCell *lc;
	char *written_pos = (*p_serial_entry) + start_pos;
	int serial_index = -1;

	/* the value for each function. */
	foreach (lc, level_state->level_funcs)
	{
		WindowStatePerFunction funcstate = (WindowStatePerFunction) lfirst(lc);

		if (funcstate->trivial_frame)
		{
			/* ignore trivial window functions */
			continue;
		}

		/* functions that require peer counts. */
		else if (funcstate->winpeercount)
		{
			int16 typelen;
			bool byvalue;
			get_typlenbyval(CUME_DIST_PRELIM_TYPE,
							&typelen, &byvalue);

			written_pos =
				serializeValue(funcstate->win_value, funcstate->win_value_is_null,
							   byvalue, typelen,
							   p_serial_entry, p_max_size,
							   p_len, (written_pos - *p_serial_entry));
		}

		else if (IS_LEAD_LAG(funcstate->wrxstate->winkind) ||
				 IS_FIRST_LAST(funcstate->wrxstate->winkind))
		{
			WindowRefExprState *wrxstate;
			int nargs;
			int argno = 1;
			wrxstate = funcstate->wrxstate;
			nargs = list_length(wrxstate->args);
			Assert(2 <= nargs && nargs <= 4);

			written_pos = 
				serializeValue(funcstate->aggTransValue, funcstate->aggTransValueIsNull,
							   wrxstate->argtypbyval[argno], wrxstate->argtyplen[argno],
							   p_serial_entry, p_max_size,
							   p_len, (written_pos - *p_serial_entry));
		}

		/* the function which has an inverse preliminary function */
		else if (OidIsValid(funcstate->invprelimfn_oid) ||
				 OidIsValid(funcstate->prelimfn_oid))
		{
			written_pos = 
				serializeValue(funcstate->aggTransValue, funcstate->aggTransValueIsNull,
							   funcstate->aggTranstypeByVal, funcstate->aggTranstypeLen,
							   p_serial_entry, p_max_size,
							   p_len, (written_pos - *p_serial_entry));
		}
		
		/* the general case */
		else
		{
			WindowRefExprState *winref_state = funcstate->wrxstate;
			ListCell *ref_lc;
			Oid typid;
			bool byval;
			int16 arglen;
			
			/* Store the input arguments */
			foreach (ref_lc, winref_state->args)
			{
				ExprState *argstate = (ExprState *)lfirst(ref_lc);
				Datum value;
				bool isnull;

				typid = exprType((Node *)argstate->expr);
				get_typlenbyval(typid, &arglen, &byval);

				value = ExecEvalExpr(argstate, econtext, &isnull, NULL);

				written_pos = 
					serializeValue(value, isnull,
								   byval, arglen,
								   p_serial_entry, p_max_size,
								   p_len, (written_pos - *p_serial_entry));
			}
		}

		serial_index++;
		funcstate->serial_index = serial_index;
	}

	*p_len = written_pos - *p_serial_entry;
	Assert(*p_len <= *p_max_size);
}


/*
 * serializeEntryForRows -- serialize a buffer entry for rows.
 */
static void
serializeEntryForRows(WindowStatePerLevel level_state,
					  ExprContext *econtext,
					  char **p_serial_entry,
					  Size *p_max_size,
					  Size *p_len)
{
	*p_len = 0;
	Assert(level_state->is_rows);
	Assert(*p_serial_entry != NULL);

	/* Copy function values */
	serializeFuncs(level_state,
				   econtext,
				   p_serial_entry,
				   p_max_size,
				   p_len,
				   0);
}

/*
 * deserializeValue -- deserialize one WindowValue from a given char array.
 *
 * The return pointer points to the char position in the given char array
 * after reading one seralized string of WindowValue.
 *
 * Note that 'value' is allocated by the caller.
 */
static char *
deserializeValue(char *read_pos, WindowValue *value,
				 bool byvalue, int16 typelen)
{
	memcpy(&(value->valueIsNull), read_pos, sizeof(bool));
	read_pos += sizeof(bool);

	if (!value->valueIsNull)
	{
		if (byvalue)
		{
			memcpy(&(value->value), read_pos, sizeof(Datum));
			read_pos += sizeof(Datum);
		}
		else
		{
			value->value = PointerGetDatum(read_pos);
			read_pos += datumGetSize(value->value,
									 byvalue,
									 typelen);
		}
	}
	else
		value->value = 0;

	return read_pos;
}

/*
 * deserializeFuncs -- deserialize function values from a given array.
 */
static List *
deserializeFuncs(WindowStatePerLevel level_state,
				 char *serial_entry,
				 Size len)
{
	List *func_values = NIL;

	ListCell *lc;
	char *read_pos = serial_entry;

	/* the value for each function. */
	foreach (lc, level_state->level_funcs)
	{
		WindowStatePerFunction funcstate = (WindowStatePerFunction) lfirst(lc);
		WindowValue *value =
			(WindowValue *)palloc0(sizeof(WindowValue));

		/* skip window functions with trivial frames */
		if (funcstate->trivial_frame)
			continue;

		/* functions that require peer counts. */
		else if (funcstate->winpeercount)
		{
			int16 typelen;
			bool byvalue;
			get_typlenbyval(CUME_DIST_PRELIM_TYPE,
							&typelen, &byvalue);
			read_pos = deserializeValue(read_pos, value, byvalue, typelen);
		}

		else if (IS_LEAD_LAG(funcstate->wrxstate->winkind) ||
				 IS_FIRST_LAST(funcstate->wrxstate->winkind))
		{
			WindowRefExprState *wrxstate;
			int nargs;
			int argno = 1;
			wrxstate = funcstate->wrxstate;
			nargs = list_length(wrxstate->args);
			Assert(2 <= nargs && nargs <= 4);

			read_pos = deserializeValue(read_pos, value,
										wrxstate->argtypbyval[argno],
										wrxstate->argtyplen[argno]);
		}

		/* the function which has an inverse preliminary function.
		 */
		else if (OidIsValid(funcstate->invprelimfn_oid) ||
				 OidIsValid(funcstate->prelimfn_oid))
		{
			read_pos = deserializeValue(read_pos, value,
										funcstate->aggTranstypeByVal,
										funcstate->aggTranstypeLen);
		}

		/* the general case */
		else
		{
			WindowRefExprState *winref_state = funcstate->wrxstate;
			ListCell *ref_lc;
			Oid typid;
			bool byval;
			int16 arglen;

			foreach (ref_lc, winref_state->args)
			{
				ExprState *argstate = (ExprState *)lfirst(ref_lc);

				typid = exprType((Node *)argstate->expr);
				get_typlenbyval(typid, &arglen, &byval);

				read_pos = deserializeValue(read_pos, value, byval, arglen);
			}
		}

		func_values = lappend(func_values, value);
	}

	Assert((read_pos - serial_entry) == len);

	return func_values;
}

/*
 * deserializeEntryForRows -- deserialize a buffer entry for rows.
 *
 * The space for 'entry' is allocated here. The caller should release the space
 * after using it.
 */
static FrameBufferEntryForRows *
deserializeEntryForRows(WindowStatePerLevel level_state,
						char *serial_entry, Size len)
{
	FrameBufferEntryForRows *entry;
	char *read_pos = serial_entry;
	
	Assert(level_state->is_rows);

	entry = (FrameBufferEntryForRows *)palloc0(sizeof(FrameBufferEntryForRows));

	/* deserial function values */
	entry->func_values = deserializeFuncs(level_state, read_pos,
										  len);

	return entry;
}

/*
 * serializeEntryForRange -- serialize a buffer entry for range.
 */
static void
serializeEntryForRange(WindowStatePerLevel level_state,
					   ExprContext *econtext,
					   char **p_serial_entry,
					   Size *p_max_size,
					   Size *p_len)
{
	int key_no;
	char *written_pos;
	TupleTableSlot *slot = econtext->ecxt_scantuple;

	*p_len = 0;
	Assert(!level_state->is_rows);
	Assert(*p_serial_entry != NULL);
	
	written_pos = *p_serial_entry;
	
	/* Copy the keys */
	for (key_no=0; key_no < level_state->numSortCols; key_no++)
	{
		AttrNumber attnum = level_state->sortColIdx[key_no];
		Datum key;
		bool isnull;
		Size keylen = 0;

		key = slot_getattr(slot, attnum, &isnull);

		keylen += datumGetSize(key, level_state->col_typbyvals[key_no],
							   level_state->col_typlens[key_no]);
	
		ensureSpace(p_serial_entry, p_max_size, written_pos, keylen + sizeof(bool));

		if (level_state->col_typbyvals[key_no])
			memcpy(written_pos, &(key), sizeof(Datum));
		else
			memcpy(written_pos, DatumGetPointer(key), keylen);
		written_pos += keylen;
		
		memcpy(written_pos, &(isnull), sizeof(bool));
		written_pos += sizeof(bool);
	}
	
	/* Copy function values */
	serializeFuncs(level_state,
				   econtext,
				   p_serial_entry,
				   p_max_size,
				   p_len,
				   (written_pos - *p_serial_entry));
}

/*
 * deserializeEntryForRange -- deserialize a buffer entry for range.
 */
static FrameBufferEntryForRange *
deserializeEntryForRange(WindowStatePerLevel level_state,
						 char *serial_entry, Size len)
{
	FrameBufferEntryForRange *entry;
	char *read_pos = serial_entry;
	Size keylen = 0;
	int key_no;

	Assert(!level_state->is_rows);

	entry = (FrameBufferEntryForRange *)palloc0(sizeof(FrameBufferEntryForRange));
	entry->keys = (Datum *)palloc0(level_state->numSortCols * sizeof(Datum));
	entry->nulls = (bool *)palloc0(level_state->numSortCols * sizeof(bool));

	for (key_no=0; key_no < level_state->numSortCols; key_no++)
	{
		Size curr_keylen = 0;
		
		curr_keylen = datumGetSize(PointerGetDatum(read_pos),
								   level_state->col_typbyvals[key_no],
								   level_state->col_typlens[key_no]);

		if (level_state->col_typbyvals[key_no])
			memcpy(&(entry->keys[key_no]), read_pos, sizeof(Datum));
		else
			entry->keys[key_no] = PointerGetDatum(read_pos);
		read_pos += curr_keylen;

		keylen += curr_keylen;
		
		memcpy(&(entry->nulls[key_no]), read_pos, sizeof(bool));
		read_pos += sizeof(bool);
		keylen += sizeof(bool);
	}

	/* deserial function values */
	entry->func_values = deserializeFuncs(level_state, read_pos,
										  len - keylen);

	return entry;
}

/*
 * adjustEdgesAfterAppend -- adjust the trailing and leading edges for
 * a given level_state after appending a value into its frame buffer.
 */
static void
adjustEdgesAfterAppend(WindowStatePerLevel level_state,
					   WindowState *wstate,
					   bool last_peer)
{
	WindowFrameBuffer buffer = level_state->frame_buffer;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	TupleTableSlot *inserting_tuple = econtext->ecxt_scantuple;

	/* If the current_row_reader for the buffer is not set, set it to point to
	 * the last row.
	 */
	if (!ntuplestore_acc_tell(buffer->current_row_reader, NULL))
	{
		ntuplestore_acc_seek_last(buffer->current_row_reader);
	}

	/* Adjust the trailing edge if it is "ROWS x FOLLOWING". */
	if (level_state->is_rows &&
		EDGE_IS_BOUND(level_state->frame->trail) &&
		level_state->trail_rows > 0)
	{
		if (!ntuplestore_acc_tell(level_state->trail_reader, NULL))
		{
			if (!last_peer)
				ntuplestore_acc_seek_last(level_state->trail_reader);
		}
		
		else if (level_state->num_trail_rows < level_state->trail_rows - 1)
		{
			ntuplestore_acc_advance(level_state->trail_reader, 1);
			level_state->num_trail_rows++;
		}
	}

	else if (!level_state->is_rows &&
			 EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail))
	{
		Datum new_trail_key;
		bool new_trail_key_isnull;
		Datum curr_key;
		bool curr_key_isnull;
		
		Assert(level_state->numSortCols == 1);
				
		/* Obtain the key for the inserted row. */
		deform_window_tuple(inserting_tuple,
							level_state->numSortCols, level_state->sortColIdx,
							&curr_key, &curr_key_isnull,
							wstate->transcontext);

		/* Calculate the leading key based on the current_row. */
		econtext->ecxt_scantuple = wstate->curslot;
		new_trail_key = ExecEvalExpr(level_state->trail_expr, econtext,
									 &(new_trail_key_isnull), NULL);

		if (cmp_deformed_tuple(&curr_key, &curr_key_isnull,
							   &new_trail_key, &new_trail_key_isnull,
							   level_state->col_sort_asc,
							   level_state->numSortCols,
							   level_state->ltfunctions,
							   level_state->eqfunctions,
							   econtext->ecxt_per_tuple_memory,
							   false))
		{
			if (ntuplestore_acc_tell(level_state->trail_reader, NULL))
				ntuplestore_acc_advance(level_state->trail_reader, 1);
			else
				ntuplestore_acc_seek_last(level_state->trail_reader);
		}
	}

	/* If the leading edge is "x PRECEDING" and x > 0, simple return. */
	if (EDGE_IS_BOUND_PRECEDING(level_state->frame->lead) &&
		((level_state->is_rows && level_state->lead_rows != 0) ||
		 (!level_state->is_rows && level_state->lead_range != 0)))
		return;

	/* Adjust the leading edge when the edge is "x FOLLOWING" or
	 * "CURRENT ROW".
	 */
	if (level_state->is_rows)
	{
		if (level_state->num_lead_rows < level_state->lead_rows)
		{
			if (!ntuplestore_acc_tell(level_state->lead_reader, NULL))
			{
				if (!last_peer)
					level_state->num_lead_rows++;
			}
			
			else
			{
				bool found;
				found = ntuplestore_acc_advance(level_state->lead_reader, 1);
				Assert(found);
				level_state->num_lead_rows++;
			}
		}

		else if ((EDGE_IS_BOUND(level_state->frame->lead) ||
				  EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true)) &&
				 level_state->num_lead_rows == level_state->lead_rows)
		{
			/* If the leading edge is not set, set it here. */
			if (!ntuplestore_acc_tell(level_state->lead_reader, NULL))
				ntuplestore_acc_seek_last(level_state->lead_reader);
		}

		else if (!EDGE_IS_BOUND(level_state->frame->lead) &&
				 !EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
		{
			if (!ntuplestore_acc_tell(level_state->lead_reader, NULL))
				ntuplestore_acc_seek_last(level_state->lead_reader);
			else
			{
				bool found = ntuplestore_acc_advance(level_state->lead_reader, 1);
				if (found)
					level_state->num_lead_rows++;
			}
		}
	}
	
	else
	{
		if (EDGE_IS_BOUND(level_state->frame->lead))
		{
			Datum new_lead_key;
			bool new_lead_key_isnull;
			Datum curr_key;
			bool curr_key_isnull;
			bool is_lessthan;
			bool is_equal;
			
			Assert(level_state->numSortCols == 1);
				
			/* Obtain the key for the inserted row. */
			deform_window_tuple(inserting_tuple,
								level_state->numSortCols, level_state->sortColIdx,
								&curr_key, &curr_key_isnull,
								wstate->transcontext);

			/* Calculate the leading key based on the current_row. */
			econtext->ecxt_scantuple = wstate->curslot;
			new_lead_key = ExecEvalExpr(level_state->lead_expr, econtext,
										&(new_lead_key_isnull), NULL);

			is_lessthan = cmp_deformed_tuple(&curr_key, &curr_key_isnull,
											 &new_lead_key, &new_lead_key_isnull,
											 level_state->col_sort_asc,
											 level_state->numSortCols,
											 level_state->ltfunctions,
											 level_state->eqfunctions,
											 econtext->ecxt_per_tuple_memory,
											 false);
			is_equal = cmp_deformed_tuple(&curr_key, &curr_key_isnull,
										  &new_lead_key, &new_lead_key_isnull,
										  level_state->col_sort_asc,
										  level_state->numSortCols,
										  level_state->ltfunctions,
										  level_state->eqfunctions,
										  econtext->ecxt_per_tuple_memory,
										  true);

			if (!is_lessthan && !is_equal)
				level_state->lead_ready = true;
			else
			{
				level_state->lead_ready = false;

				if (ntuplestore_acc_tell(level_state->lead_reader, NULL))
					ntuplestore_acc_advance(level_state->lead_reader, 1);
				else if (is_equal)
				{
					level_state->lead_ready = true;
					ntuplestore_acc_seek_last(level_state->lead_reader);
				}
				
				else
				{
					bool found;
					
					/* Check if the last tuple in the input buffer is the leading
					 * edge.
					 */
					found = ntuplestore_acc_current_tupleslot(wstate->input_buffer->writer,
															  wstate->spare);
					Assert(found);
					
					deform_window_tuple(wstate->spare,
										level_state->numSortCols, level_state->sortColIdx,
										&curr_key, &curr_key_isnull,
										wstate->transcontext);

					if (!cmp_deformed_tuple(&curr_key, &curr_key_isnull,
											&new_lead_key, &new_lead_key_isnull,
											level_state->col_sort_asc,
											level_state->numSortCols,
											level_state->ltfunctions,
											level_state->eqfunctions,
											econtext->ecxt_per_tuple_memory,
											false) &&
						!cmp_deformed_tuple(&curr_key, &curr_key_isnull,
											&new_lead_key, &new_lead_key_isnull,
											level_state->col_sort_asc,
											level_state->numSortCols,
											level_state->ltfunctions,
											level_state->eqfunctions,
											econtext->ecxt_per_tuple_memory,
											true))
					{
						ntuplestore_acc_seek_last(level_state->lead_reader);
						level_state->lead_ready = true;
					}
				}
			}
		}
		else if (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
		{
			if (!ntuplestore_acc_tell(buffer->current_row_reader, NULL))
				ntuplestore_acc_seek_last(level_state->lead_reader);
			else
			{
				bool found;
				NTupleStorePos pos;
				found = ntuplestore_acc_tell(buffer->current_row_reader, &pos);
				Assert(found);
				ntuplestore_acc_seek(level_state->lead_reader, &pos);
			}

			level_state->lead_ready = true;
		}
		else
		{
			ntuplestore_acc_advance(level_state->lead_reader, 1);
			level_state->lead_ready = last_peer;
		}
	}
}

/*
 * appendToFrameBuffer -- append the intermediate values stored in level
 */
static void
appendToFrameBuffer(WindowStatePerLevel level_state,
					WindowState *wstate,
					bool last_peer)
{
	char *serial_entry = wstate->serial_array;
	Size len;
	WindowFrameBuffer buffer = level_state->frame_buffer;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	Assert(buffer->is_rows == level_state->is_rows);
	if (buffer->is_rows)
	{
		serializeEntryForRows(level_state, econtext,
							  &serial_entry, &(wstate->max_size), &len);
	}
	else
	{
		serializeEntryForRange(level_state, econtext,
							   &serial_entry, &(wstate->max_size), &len);
	}

	ntuplestore_acc_put_data(buffer->writer, (void*)serial_entry, len);

	adjustEdgesAfterAppend(level_state, wstate, last_peer);
}

#if 0
/*
 * trimFrameBuffer -- trim the old values in the buffer.
 */
static void
trimFrameBuffer(WindowFrameBuffer buffer, NTupleStorePos *pos)
{
	ntuplestore_trim(buffer->tuplestore, pos);
}

/*
 * getValueAtPos - return a value at the given position.
 */
static List *
getValueAtPos(WindowFrameBuffer buffer,
			  WindowStatePerLevel level_state,
			  NTupleStorePos *pos)
{
	void *data;
	int len;
	void *value;
	
	ntuplestore_acc_seek(buffer->reader, pos);
	ntuplestore_acc_current_data(buffer->reader, &data, &len);

	if (buffer->is_rows)
		value = (void *)deserializeEntryForRows(level_state, data, len);
	else
		value = (void *)deserializeEntryForRange(level_state, data, len);

	return value;
}
#endif

/*
 * getCurrentValue -- return a value at the current position for
 * an accessor.
 */
static void *
getCurrentValue(NTupleStoreAccessor *reader,
				WindowStatePerLevel level_state)
{
	void *data;
	int len;
	void *value;
	
	bool found = ntuplestore_acc_current_data(reader, &data, &len);

	if (!found)
		return NULL;
	
	if (level_state->is_rows)
		value = (void *)deserializeEntryForRows(level_state, data, len);
	else
		value = (void *)deserializeEntryForRange(level_state, data, len);

	return value;
}

				

/*
 * incrementCurrentRow -- increment the current_row in the buffer.
 */
static void
incrementCurrentRow(WindowFrameBuffer buffer,
					WindowState *wstate)
{
	WindowStatePerLevel level_state = buffer->level_state;

	/* Increment the current_row_reader */
	ntuplestore_acc_advance(buffer->current_row_reader, 1);

	if (level_state->is_rows)
	{
		level_state->num_trail_rows--;
		level_state->num_lead_rows--;
	}

   if (!buffer->level_state->trivial_frames_only)
	   /* Adjust all leading and trailig edges */
	   adjustEdges(buffer, wstate);

	/* We determine if we need to trim the buffer here. */
}

/*
 * hasEnoughDataInRange -- return true if there is enough data in
 * the buffer that satisfy a given value range.
 */
static bool
hasEnoughDataInRange(WindowFrameBuffer buffer,
					 WindowStatePerLevel level_state,
					 ExprContext *econtext,
					 MemoryContext context,
					 Datum trail_range, Datum lead_range)
{
	if (level_state->empty_frame)
		return true;

	if (!EDGE_IS_BOUND(level_state->frame->lead) &&
		!EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
		return false;
	
	return level_state->lead_ready;
}


/*
 * hasEnoughDataInRows -- return true if there is enough data in the
 * buffer that satisfy  a given row range.
 */
static bool
hasEnoughDataInRows(WindowFrameBuffer buffer,
					WindowStatePerLevel level_state,
					ExprContext *econtext,
					long int trail_rows, long int lead_rows)
{
	if (level_state->empty_frame)
		return true;

	if (!EDGE_IS_BOUND(level_state->frame->lead) &&
		!EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
		return false;

	if (trail_rows == 0 &&
		lead_rows == 0)
		return true;
	
	if (level_state->num_lead_rows >= lead_rows)
		return true;
	return false;
}

static void
freeFrameBufferEntryForRange(FrameBufferEntryForRange *range_entry)
{
	pfree(range_entry->keys);
	pfree(range_entry->nulls);

	list_free_deep(range_entry->func_values);
}

static void
freeFrameBufferEntryForRows(FrameBufferEntryForRows *rows_entry)
{
	list_free_deep(rows_entry->func_values);
}

/*
 * hasTuplesInFrame - Check if there are tuples in the current trailing
 * and leading edge in a frame.
 *
 * This function returns true if there are tuples in the current
 * frame. Otherwise, return false.
 */
static bool hasTuplesInFrame(WindowStatePerLevel level_state)
{
	bool has_tuples = true;
	NTupleStorePos orig_pos;
	bool trail_valid;

	/* Save the position for the trail_reader */
	trail_valid = ntuplestore_acc_tell(level_state->trail_reader, &orig_pos);

	if (trail_valid)
		ntuplestore_acc_advance(level_state->trail_reader, 1);
	else
	{
		if (!EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) ||
			EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->trail, false))
			ntuplestore_acc_seek_first(level_state->trail_reader);
	}
	
	if (ntuplestore_acc_tell(level_state->trail_reader, NULL) &&
		ntuplestore_acc_tell(level_state->lead_reader, NULL) &&
		ntuplestore_acc_is_before( level_state->lead_reader,
								   level_state->trail_reader))
		has_tuples = false;
	if (EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) &&
		((level_state->is_rows && 
		  level_state->num_lead_rows < level_state->trail_rows) ||
		 (!ntuplestore_acc_tell(level_state->trail_reader, NULL) &&
		  !level_state->is_rows &&
		  level_state->trail_range != 0)))
		has_tuples = false;
	if (!ntuplestore_acc_tell(level_state->lead_reader, NULL) &&
		(EDGE_IS_BOUND_PRECEDING(level_state->frame->lead) &&
		 ((level_state->is_rows && level_state->lead_rows != 0) ||
		  (!level_state->is_rows && level_state->lead_range != 0))))
		has_tuples = false;

	/* Restore trail_reader */
	if (trail_valid)
		ntuplestore_acc_seek(level_state->trail_reader, &orig_pos);
	else
		ntuplestore_acc_set_invalid(level_state->trail_reader);

	return has_tuples;
}


/*
 * computeFrameValue -- compute transition values for each function
 * in a given key level from the data stored in its frame buffer.
 *
 * The 'trail_reader' and 'lead_reader' point to the positions of
 * the trailing and leading edges.
 */
static void
computeFrameValue(WindowStatePerLevel level_state,
				  WindowState *wstate,
				  NTupleStoreAccessor *trail_reader,
				  NTupleStoreAccessor *lead_reader)
{
	ListCell *lc;
	int value_no = 0;
	bool is_rows = level_state->is_rows;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	WindowFrameBuffer buffer = level_state->frame_buffer;
	FunctionCallInfoData fcinfo;
	
	FrameBufferEntryForRange *range_trail_entry = NULL;
	FrameBufferEntryForRange *range_lead_entry = NULL;
	FrameBufferEntryForRows *rows_trail_entry = NULL;
	FrameBufferEntryForRows *rows_lead_entry = NULL;
	FrameBufferEntryForRange *range_entry = NULL;
	FrameBufferEntryForRows *rows_entry = NULL;

	WindowValue *trail_value = NULL;
	WindowValue *lead_value = NULL;
	WindowValue *value = NULL;

	bool read_value = false;
	bool read_edge_value = false;

	Assert(level_state->is_rows == buffer->is_rows);

	foreach (lc, level_state->level_funcs)
	{
		WindowStatePerFunction funcstate = (WindowStatePerFunction) lfirst(lc);
		
		/* Ignore ordinary window functions, except for functions that
		 * require peer counts.
		 */
		if (funcstate->trivial_frame &&
			!funcstate->winpeercount)
			continue;

		/* functions that require peer counts. */
		else if (funcstate->winpeercount)
		{
			/* If we didn't read the value pointed by current_row_reader,
			 * read it now.
			 */
			if (!read_value)
			{
				read_value = true;
				
				if (is_rows)
					rows_entry = getCurrentValue(buffer->current_row_reader, level_state);
				else
					range_entry = getCurrentValue(buffer->current_row_reader, level_state);
			}
			
			if (is_rows && rows_entry != NULL)
				value = (WindowValue *)list_nth(rows_entry->func_values, value_no);
			else if (!is_rows && range_entry != NULL)	
				value = (WindowValue *)list_nth(range_entry->func_values, value_no);

			if (value != NULL)
			{
				funcstate->win_value = value->value;
				funcstate->win_value_is_null = value->valueIsNull;
			}
			else
			{
				TupleTableSlot *slot = econtext->ecxt_scantuple;
				
				ntuplestore_acc_current_tupleslot(wstate->input_buffer->writer,
												  wstate->spare);
				econtext->ecxt_scantuple = wstate->spare;
				add_tuple_to_trans(funcstate, wstate, econtext);

				/* Reset back to its orginial value */
				econtext->ecxt_scantuple = slot;
			}
		}

		/* Aggregate functions with inverse preliminary functions. */
		else if (funcstate->isAgg && OidIsValid(funcstate->invprelimfn_oid))
		{
			if (!read_edge_value)
			{
				read_edge_value = true;

				if (is_rows)
				{
					if (!EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) ||
						level_state->num_lead_rows >= level_state->trail_rows)
					{
						rows_trail_entry = getCurrentValue(level_state->trail_reader, level_state);
						rows_lead_entry = getCurrentValue(level_state->lead_reader, level_state);
					}
				}
				
				else
				{
					range_trail_entry = getCurrentValue(level_state->trail_reader, level_state);
					/* If the leading edge is UNBOUNDED FOLLOWING, the value in the leading edge
					 * is funcstate->aggTransValue. We don't need to read from the buffer.
					 */
					if (level_state->frame->lead->kind != WINDOW_UNBOUND_FOLLOWING)
						range_lead_entry = getCurrentValue(level_state->lead_reader, level_state);
				}
			}
			
			if (is_rows)
			{
				if (rows_trail_entry != NULL)
				{
					Assert(list_length(rows_trail_entry->func_values) > value_no);
					trail_value = 
						(WindowValue *)list_nth(rows_trail_entry->func_values, value_no);
				}
			
				if (rows_lead_entry != NULL)
				{
					Assert(list_length(rows_lead_entry->func_values) > value_no);
					lead_value =
						(WindowValue *)list_nth(rows_lead_entry->func_values, value_no);
				}
			}
			else
			{
				if (range_trail_entry != NULL)
				{
					Assert(list_length(range_trail_entry->func_values) > value_no);
					trail_value =
						(WindowValue *)list_nth(range_trail_entry->func_values, value_no);
				}

				if (range_lead_entry != NULL)
				{
					Assert(list_length(range_lead_entry->func_values) > value_no);
					lead_value =
						(WindowValue *)list_nth(range_lead_entry->func_values, value_no);
				}
			}
			
			if (trail_value != NULL)
			{
				fcinfo.arg[1] = trail_value->value;
				fcinfo.argnull[1] = trail_value->valueIsNull;
			}
			else
			{
				fcinfo.arg[1] = funcstate->aggInitValue;
				fcinfo.argnull[1] = funcstate->aggInitValueIsNull;
			}

			if (lead_value != NULL || trail_value != NULL)
			{
				MemoryContext oldcxt;

				oldcxt = MemoryContextSwitchTo(wstate->transcontext);
				if (lead_value != NULL)
				{
					funcstate->final_aggTransValue =
						datumCopy(lead_value->value,
								  funcstate->aggTranstypeByVal,
								  funcstate->aggTranstypeLen);
					funcstate->final_aggTransValueIsNull = lead_value->valueIsNull;
					funcstate->final_aggNoTransValue = false;
				}
				else
				{
					funcstate->final_aggTransValue = 
						datumCopy(funcstate->aggTransValue,
								  funcstate->aggTranstypeByVal,
								  funcstate->aggTranstypeLen);
					funcstate->final_aggTransValueIsNull = funcstate->aggTransValueIsNull;
					funcstate->final_aggNoTransValue = false;
				}
				MemoryContextSwitchTo(oldcxt);
			
				funcstate->final_aggTransValue =
					invoke_agg_trans_func(funcstate->invprelimfn,
										  funcstate->invprelimfn.fn_nargs - 1,
										  funcstate->final_aggTransValue,
										  &funcstate->final_aggNoTransValue,
										  &(funcstate->final_aggTransValueIsNull),
										  funcstate->aggTranstypeByVal,
										  funcstate->aggTranstypeLen,
										  &fcinfo, (void *)wstate,
										  wstate->transcontext,
										  econtext->ecxt_per_tuple_memory);
			}
			
			else
			{
				if ((EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) &&
					 ((level_state->is_rows && level_state->trail_rows != 0) ||
					  (!level_state->is_rows && level_state->trail_range != 0))) ||
					(EDGE_IS_BOUND_PRECEDING(level_state->frame->lead) &&
					 ((level_state->is_rows && level_state->lead_rows != 0) ||
					  (!level_state->is_rows && level_state->lead_range != 0))))
				{
					funcstate->final_aggTransValue = funcstate->aggInitValue;
					funcstate->final_aggTransValueIsNull = funcstate->aggInitValueIsNull;
					funcstate->final_aggNoTransValue = funcstate->aggInitValueIsNull;
				}
				else
				{
					funcstate->final_aggTransValue = funcstate->aggTransValue;
					funcstate->final_aggTransValueIsNull = funcstate->aggTransValueIsNull;
					funcstate->final_aggNoTransValue = funcstate->aggNoTransValue;
				}
			}
		}

		else if (funcstate->isAgg && OidIsValid(funcstate->prelimfn_oid))
		{
			FrameBufferEntryForRows *curr_rows_entry = NULL;
			FrameBufferEntryForRange *curr_range_entry = NULL;
			WindowValue *curr_value = NULL;
			MemoryContext oldcxt;
			
			/* Indicate if the current frame contains tuples at all. */
			bool has_tuples = true;
			NTupleStorePos orig_pos;

			if (EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) &&
				((level_state->is_rows && 
				  level_state->num_lead_rows < level_state->trail_rows) ||
				 (!ntuplestore_acc_tell(level_state->trail_reader, NULL) &&
				  !level_state->is_rows &&
				  level_state->trail_range != 0)))
				has_tuples = false;

			/* Remember the original trail_reader position. */
			ntuplestore_acc_tell(level_state->trail_reader, &orig_pos);

			/* Since the trail_reader points to the value before the trailing
			 * edge, we advance the trail_reader by one.
			 */
			if (ntuplestore_acc_tell(level_state->trail_reader, NULL))
				ntuplestore_acc_advance(level_state->trail_reader, 1);
			else
			{
				if (!ntuplestore_acc_tell(level_state->lead_reader, NULL) &&
					(EDGE_IS_BOUND_PRECEDING(level_state->frame->lead) &&
					 ((level_state->is_rows && level_state->lead_rows != 0) ||
					  (!level_state->is_rows && level_state->lead_range != 0))))
					has_tuples = false;
				else
					ntuplestore_acc_seek_first(level_state->trail_reader);
			}

			oldcxt = MemoryContextSwitchTo(wstate->transcontext);
			funcstate->final_aggTransValue = 
				datumCopy(funcstate->aggInitValue,
						  funcstate->aggTranstypeByVal,
						  funcstate->aggTranstypeLen);
			MemoryContextSwitchTo(oldcxt);
			funcstate->final_aggTransValueIsNull = funcstate->aggInitValueIsNull;
			funcstate->final_aggNoTransValue = funcstate->aggInitValueIsNull;

			if (has_tuples)
			{
				bool include_last_agg = false;
				
				while (ntuplestore_acc_tell(level_state->trail_reader, NULL))
				{
					if (ntuplestore_acc_tell(level_state->lead_reader, NULL) &&
						ntuplestore_count_slot_acc(level_state->frame_buffer->tuplestore,
												   level_state->trail_reader,
												   level_state->lead_reader) < 0)
						break;
					
					if (is_rows)
					{
						curr_rows_entry = getCurrentValue(level_state->trail_reader,
														  level_state);
						if (curr_rows_entry != NULL)
							curr_value = (WindowValue *)
								list_nth(curr_rows_entry->func_values, value_no);
					}
					else
					{
						curr_range_entry = getCurrentValue(level_state->trail_reader,
														   level_state);
						if (curr_range_entry != NULL)
							curr_value = (WindowValue *)
								list_nth(curr_range_entry->func_values, value_no);
					}

					if (curr_value != NULL)
					{
						fcinfo.arg[1] = curr_value->value;
						fcinfo.argnull[1] = curr_value->valueIsNull;

						funcstate->final_aggTransValue =
							invoke_agg_trans_func(funcstate->prelimfn,
												  funcstate->prelimfn.fn_nargs - 1,
												  funcstate->final_aggTransValue,
												  &funcstate->final_aggNoTransValue,
												  &(funcstate->final_aggTransValueIsNull),
												  funcstate->aggTranstypeByVal,
												  funcstate->aggTranstypeLen,
												  &fcinfo, (void *)wstate,
												  wstate->transcontext,
												  econtext->ecxt_per_tuple_memory);
					}

					ntuplestore_acc_advance(level_state->trail_reader, 1);

					if (curr_rows_entry != NULL)
						freeFrameBufferEntryForRows(curr_rows_entry);
					if (curr_range_entry != NULL)
						freeFrameBufferEntryForRange(curr_range_entry);
				}

				/* Add the funcstate->aggTransValue if it is in the current
				 * frame.
				 */
				if (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->trail, false))
					include_last_agg = true;
				if (!EDGE_IS_BOUND(level_state->frame->lead) ||
					EDGE_IS_BOUND_FOLLOWING(level_state->frame->lead)  ||
					EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
					include_last_agg = true;
				
				if (!ntuplestore_acc_tell(level_state->lead_reader, NULL) &&
					level_state->agg_filled && include_last_agg)
				{
					fcinfo.arg[1] = funcstate->aggTransValue;
					fcinfo.argnull[1] = funcstate->aggTransValueIsNull;

					funcstate->final_aggTransValue =
						invoke_agg_trans_func(funcstate->prelimfn,
											  funcstate->prelimfn.fn_nargs - 1,
											  funcstate->final_aggTransValue,
											  &funcstate->final_aggNoTransValue,
											  &(funcstate->final_aggTransValueIsNull),
											  funcstate->aggTranstypeByVal,
											  funcstate->aggTranstypeLen,
											  &fcinfo, (void *)wstate,
											  wstate->transcontext,
											  econtext->ecxt_per_tuple_memory);
				}
			}

			/* Reset the trail_reader to its original position. */
			if (!ntuplestore_acc_seek(level_state->trail_reader, &orig_pos))
				ntuplestore_acc_set_invalid(level_state->trail_reader);
		}

		else
		{
			/* Here are the general cases. The frame buffer stores the input
			 * arguments. For window aggregate functions, we scan through
			 * values between the trailing edge and the leading edge, and
			 * add them into the transition value. For ordinary window functions,
			 * we pass the frame buffer to its function handler to generate
			 * the output.
			 */
			if (!funcstate->isAgg)
			{
				int argno;
				FunctionCallInfoData fcinfo;

				argno = initFcinfo(funcstate->wrxstate, &fcinfo, funcstate, econtext);
				InitFunctionCallInfoData(fcinfo, 
										 &funcstate->windowfn,
										 argno,
										 (void *) funcstate->wrxstate, 
										 NULL);
				funcstate->win_value = FunctionCallInvoke(&fcinfo);
				funcstate->win_value_is_null = fcinfo.isnull;
			}

			else
				ereport(ERROR,
						(errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
						 errmsg("Not yet support aggregate functions that do not have "
								"invprelimfn or prelimfn")));
		}

		value_no++;
	}

	if (range_trail_entry != NULL)
		freeFrameBufferEntryForRange(range_trail_entry);
	if (range_lead_entry != NULL)
		freeFrameBufferEntryForRange(range_lead_entry);
	if (rows_trail_entry != NULL)
		freeFrameBufferEntryForRows(rows_trail_entry);
	if (rows_lead_entry != NULL)
		freeFrameBufferEntryForRows(rows_lead_entry);
	
}

/*
 * Initialize function state for each function in the Window node.
 */
static void
initWindowFuncState(WindowState *wstate, Window *node)
{
	int numrefs;
	int refno;
	ListCell *lc;

	if (wstate->wrxstates == NULL)
		return;

	wstate->numfuncs = 0;
	numrefs = list_length(wstate->wrxstates);
    Insist(numrefs > 0);

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

		funcstate->serial_index = -1;

		if (winref->winlevel >= wstate->numlevels)
		{
			funcstate->trivial_frame = true;
			funcstate->wlevel = NULL;
		}
			   
		else
		{
			WindowStatePerLevel level_state = &wstate->level_state[winref->winlevel];

			/* When this level has the range frame, this frame may not
			 * always match with the row_number function. For example,
			 *    row_number() over (order by cn), sum(qty) over (order by cn)
			 * Both of these functions point to the level "order by cn",
			 * which is used the default frame "range between unbounded
			 * preceding and current_row". This is not valid for row_number.
			 * We should not put row_number and sum in the same level.
			 * We can simply ignore row_number in this key level.
			 */
			if (!level_state->is_rows &&
				winref->winfnoid == ROW_NUMBER_OID)
			{
				funcstate->wlevel = NULL;
				funcstate->trivial_frame = true;
			}
			else
			{
				funcstate->wlevel = level_state;
				level_state->level_funcs = lappend(level_state->level_funcs,
												   funcstate);
			}
		}
		
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
			funcstate->allowframe = winform->winallowframe;
			
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

			/* 
			 * Initialize byval and typlen for framed funcs. Of course we're
			 * creating an entry here for the inserted refptr but we do so
			 * for consistency.
			 */
			if (funcstate->allowframe || IS_LEAD_LAG(wrxstate->winkind) ||
				IS_FIRST_LAST(wrxstate->winkind))
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
				funcstate->winpeercount = winform->winpeercount;
				wstate->need_peercount = winform->winpeercount;
			}

			funcstate->win_value = 0;
			funcstate->win_value_is_null = true;
		}
		else
		{
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("inappropriate use of function as window function")));

		}
	}
}

/*
 * Initialize WindowStatePerLevel in the Window node.
 */
static void
initWindowStatePerLevel(WindowState *wstate, Window *node)
{
	ListCell *lc;
	int level_no = 0;
	WindowKey *prev_key = NULL;
	TupleDesc desc;
	int col_no;

	desc = ExecGetResultType(wstate->ps.lefttree);

	foreach(lc, node->windowKeys)
	{
		WindowKey *key = (WindowKey *) lfirst(lc);
		WindowFrame *frame;
		WindowStatePerLevel lvl =  &wstate->level_state[level_no++];

		lvl->has_lead_lag = false;

		/* Copy the sort columns for this level. If the window key contains
		 * an empty list of sort columns, we copy the previous non-empty
		 * list of sort columns here. This makes it easier to find keys to
		 * be stored in the frame buffer later.
		 */
		lvl->numSortCols = key->numSortCols;
		if (lvl->numSortCols == 0 && prev_key != NULL)
			lvl->numSortCols += prev_key->numSortCols;
		lvl->sortColIdx = (AttrNumber *)palloc0(lvl->numSortCols * sizeof(AttrNumber));
		lvl->sortOperators = (Oid *)palloc0(lvl->numSortCols *sizeof(Oid));

		if (lvl->numSortCols > key->numSortCols)
		{
			memcpy(lvl->sortColIdx, prev_key->sortColIdx,
				   prev_key->numSortCols * sizeof(AttrNumber));
			memcpy(lvl->sortOperators, prev_key->sortOperators,
				   prev_key->numSortCols * sizeof(Oid));
		}
			
		memcpy(lvl->sortColIdx + (lvl->numSortCols - key->numSortCols),
			   key->sortColIdx,
			   key->numSortCols * sizeof(AttrNumber));
		memcpy(lvl->sortOperators + (lvl->numSortCols - key->numSortCols),
			   key->sortOperators,
			   key->numSortCols * sizeof(Oid));

		if (key->numSortCols > 0)
			prev_key = key;

		/* Find comparison functions */
		lvl->eqfunctions = execTuplesMatchPrepare(desc,
								   				  lvl->numSortCols,
								   				  lvl->sortColIdx);

		lvl->ltfunctions = get_ltfuncs(desc,
									   lvl->numSortCols,
									   lvl->sortColIdx);

		/* Set the frame for this level */
		lvl->is_rows = false;

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

			frame = key->frame = (WindowFrame *)makeNode(WindowFrame);
			frame->trail = (WindowFrameEdge *)makeNode(WindowFrameEdge);
			frame->lead = (WindowFrameEdge *)makeNode(WindowFrameEdge);

			/* 
			 * The default frame is:
			 *
			 *   RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
			 */
			frame->is_rows = false;
			frame->trail->kind = WINDOW_UNBOUND_PRECEDING;
			frame->lead->kind = WINDOW_CURRENT_ROW;

			lvl->is_rows =  false;

			/* 
			 * Note: we haven't set is_rows or exclude but the use of palloc0()
			 * inside of makeNode() ensures all fields of the struct are 
			 * zeros so we're golden.
			 */
		}
		else
		{
			frame = key->frame;
			lvl->is_rows = key->frame->is_rows;
		}

		lvl->frame = frame;

		Assert(PointerIsValid(frame->trail));
		Assert(PointerIsValid(frame->lead));

		lvl->col_types = palloc(sizeof(Oid) * lvl->numSortCols);
		lvl->col_typlens = palloc(sizeof(int2) * lvl->numSortCols);
		lvl->col_typbyvals = palloc(sizeof(bool) * lvl->numSortCols);

		for (col_no = 0; col_no < lvl->numSortCols; col_no++)
		{
			AttrNumber attnum = lvl->sortColIdx[col_no];
			Form_pg_attribute attr = desc->attrs[attnum - 1];
			
			lvl->col_types[col_no] = attr->atttypid;
			lvl->col_typlens[col_no] = attr->attlen;
			lvl->col_typbyvals[col_no] = attr->attbyval;
		}
	}
}


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
	
	wstate->transcontext = AllocSetContextCreate(CurrentMemoryContext,
												 "TransContext",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * We allocate the buffer to be 1K initially, which should be
	 * sufficient for most cases.
	 */
	wstate->serial_array = palloc0(FRAMEBUFFER_ENTRY_SIZE);
	wstate->max_size = FRAMEBUFFER_ENTRY_SIZE;

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
		level_state->peer_index = 0;
		level_state->peer_count = 0;
		level_state->rank = 1;
		level_state->dense_rank = 1;
		level_state->prior_rank = 1;
		level_state->prior_dense_rank = 1;
	}
	
	/* Per-partition input buffer management reinitialzation. */
	resetInputBuffer(wstate);
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
	bool save_firsttuple = false;

	if (wstate->input_buffer)
	{
		int level;

		save_firsttuple = wstate->input_buffer->part_break;
		if (save_firsttuple)
		{
			ntuplestore_acc_current_tupleslot(wstate->input_buffer->writer,
											  wstate->spare);
			wstate->curslot = ExecCopySlot(wstate->curslot, wstate->spare);
		}
		ntuplestore_destroy_accessor(wstate->input_buffer->writer);
		ntuplestore_destroy_accessor(wstate->input_buffer->current_row_reader);
		ntuplestore_destroy_accessor(wstate->input_buffer->reader);
		
		ntuplestore_reset(wstate->input_buffer->tuplestore);

		wstate->input_buffer->part_break = false;
		wstate->input_buffer->num_tuples = 0;

		wstate->cur_slot_is_new = false;
		wstate->cur_slot_part_break = false;
		wstate->cur_slot_key_break = wstate->numlevels;

		resetFrameBuffers(wstate);

		/* reset transition values */
		for (level=0; level<wstate->numlevels; level++)
		{
			WindowStatePerLevel level_state =
				(WindowStatePerLevel)&wstate->level_state[level];
			
			resetTransValues(level_state, wstate->transcontext);
		}
	}

	else
	{
		wstate->input_buffer = (WindowInputBuffer)palloc0(sizeof(WindowInputBufferData));
		wstate->input_buffer->tuplestore =
			ntuplestore_create((work_mem * 1024L)/2); /* correct? */
		createFrameBuffers(wstate);
	}

	wstate->input_buffer->writer =
		ntuplestore_create_accessor(wstate->input_buffer->tuplestore, true);
	wstate->input_buffer->current_row_reader =
		ntuplestore_create_accessor(wstate->input_buffer->tuplestore, false);
	wstate->input_buffer->reader =
		ntuplestore_create_accessor(wstate->input_buffer->tuplestore, false);

	if (save_firsttuple)
	{
		ntuplestore_acc_put_tupleslot(wstate->input_buffer->writer,
									  wstate->curslot);
		wstate->input_buffer->num_tuples++;

		ntuplestore_acc_seek_first(wstate->input_buffer->current_row_reader);
		ntuplestore_acc_seek_first(wstate->input_buffer->reader);

		wstate->cur_slot_is_new = true;
		wstate->row_index++;
	}
}

static void
freeInputBuffer(WindowState *wstate)
{
	ntuplestore_destroy_accessor(wstate->input_buffer->writer);
	ntuplestore_destroy_accessor(wstate->input_buffer->current_row_reader);
	ntuplestore_destroy(wstate->input_buffer->tuplestore);

	pfree(wstate->input_buffer);
	wstate->input_buffer = NULL;
}

/*
 * Advance values related to computing peer count.
 */
static void
advancePeerCount(WindowState *wstate, int min_level)
{
	WindowStatePerLevel level_state;
	int i;
	
	for (i = 0; i < wstate->numlevels; i++)
	{
		level_state = &wstate->level_state[i];

		if (i >= min_level && min_level != -1)
		{
			level_state->prior_non_peer_count += level_state->peer_count + 1;
			level_state->peer_index = 0;
			level_state->peer_count = 0;
		}
	}
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

	cur_row_index = wstate->row_index;

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
			level_state->group_index++;
			level_state->rank = 1 + cur_row_index; 
			level_state->dense_rank = 1 + level_state->group_index;
		}
	}
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
	}
	else /* ordinary window function */
	{
		InitFunctionCallInfoData(fcinfo, 
								 &funcstate->windowfn,
								 argno,
								 (void *) funcstate->wrxstate, 
								 NULL);
		if (IS_LEAD_LAG(funcstate->wrxstate->winkind) ||
			IS_LAST_VALUE(funcstate->wrxstate->winkind) ||
			(IS_FIRST_VALUE(funcstate->wrxstate->winkind) &&
			 funcstate->aggNoTransValue))
		{
			int argno = 1;
			MemoryContext oldcxt;
			
			oldcxt = MemoryContextSwitchTo(wstate->transcontext);
			
			funcstate->aggTransValue = 
				datumCopy(fcinfo.arg[argno],
						  funcstate->wrxstate->argtypbyval[argno],
						  funcstate->wrxstate->argtyplen[argno]);
			funcstate->aggTransValueIsNull = fcinfo.argnull[argno];
			funcstate->aggNoTransValue = false;

			MemoryContextSwitchTo(oldcxt);
		}

		else
		{
			funcstate->win_value = FunctionCallInvoke(&fcinfo);
			funcstate->win_value_is_null = fcinfo.isnull;
		}
	}

}

static void
invokeTrivialFuncs(WindowState *wstate, bool *found)
{
	int funcno;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	for (funcno = 0; funcno < wstate->numfuncs; funcno++)
	{
		WindowStatePerFunction funcstate = &wstate->func_state[funcno];
		WindowStatePerLevel level_state = funcstate->wlevel;
		
		if (funcstate->trivial_frame &&
			!funcstate->winpeercount)
		{
			econtext->ecxt_scantuple = wstate->curslot;
			add_tuple_to_trans(funcstate, wstate, econtext);

			if (funcstate->isAgg)
			{
				funcstate->final_aggTransValue = funcstate->aggTransValue;
				funcstate->final_aggTransValueIsNull = funcstate->aggTransValueIsNull;
				funcstate->final_aggNoTransValue = funcstate->aggNoTransValue;
			}
		}

		else if (funcstate->cumul_frame)
		{
			WindowFrameBuffer frame_buffer;
			
			Assert(level_state != NULL);
			frame_buffer = level_state->frame_buffer;
			Assert(frame_buffer != NULL);

			/* When the current_row_reader is set, we retrieve the intermediate
			 * accumulated aggregate value from the buffer.
			 */
			if (ntuplestore_acc_tell(frame_buffer->current_row_reader, NULL))
			{
				FrameBufferEntryForRange *range_entry = NULL;
				FrameBufferEntryForRows *rows_entry = NULL;
				WindowValue *value = NULL;
				MemoryContext oldcxt;
				
				if (funcstate->serial_index == -1)
					funcstate->serial_index = findSerialIndex(funcstate, level_state);

				if (level_state->is_rows)
				{
					rows_entry = (FrameBufferEntryForRows *)
						getCurrentValue(frame_buffer->current_row_reader,
										level_state);
					Assert(rows_entry != NULL);
					value = (WindowValue *)list_nth(rows_entry->func_values,
													funcstate->serial_index);
				}
				
				else
				{
					range_entry = (FrameBufferEntryForRange *)
						getCurrentValue(frame_buffer->current_row_reader,
												  level_state);
					Assert(range_entry != NULL);
					value = (WindowValue *)list_nth(range_entry->func_values,
													funcstate->serial_index);
				}

				Assert(value != NULL);
				oldcxt = MemoryContextSwitchTo(wstate->transcontext);
				funcstate->final_aggTransValue = datumCopy(value->value,
														   funcstate->aggTranstypeByVal,
														   funcstate->aggTranstypeLen);
				MemoryContextSwitchTo(oldcxt);
				funcstate->final_aggTransValueIsNull = value->valueIsNull;
				funcstate->final_aggNoTransValue = false;

				if (range_entry != NULL)
					freeFrameBufferEntryForRange(range_entry);
				if (rows_entry != NULL)
					freeFrameBufferEntryForRows(rows_entry);
			}

			/* Otherwise, the value for the current_rows is stored in aggTransValue.
			 */
			else
			{
				funcstate->final_aggTransValue = funcstate->aggTransValue;
				funcstate->final_aggTransValueIsNull = funcstate->aggTransValueIsNull;
				funcstate->final_aggNoTransValue = funcstate->aggNoTransValue;
			}
		}
			
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
 * values and nulls should be allocated memory by the caller.
 */
static void
deform_window_tuple(TupleTableSlot *slot, int nattrs, AttrNumber *attnums, 
					Datum *values, bool *nulls, MemoryContext ctx)
{
	int i;
	MemoryContext oldctx = MemoryContextSwitchTo(ctx);

	for (i = 0; i < nattrs; i++)
	{
		AttrNumber attnum = attnums[i];
		bool isnull;
		Form_pg_attribute attr = slot->tts_tupleDescriptor->attrs[attnum - 1];
		Datum d = slot_getattr(slot, attnum, &isnull);

		/* 
		 * We must copy this tuple because we may need to keep it around
		 * a long time. Caller is responsible for freeing these values.
		 */
		values[i] = datumCopy(d, attr->attbyval, attr->attlen);
		nulls[i] = isnull;
	}

	MemoryContextSwitchTo(oldctx);
}

/*
 * Adjust the trailing and leading edge for ROWS-based framing.
 */
static void
adjustEdgesForRows(WindowFrameBuffer buffer,
				   WindowState *wstate)
{
	WindowStatePerLevel level_state = buffer->level_state;
	
	if (EDGE_IS_BOUND(level_state->frame->trail) ||
		EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->trail, false))
	{
		if (level_state->num_trail_rows <= level_state->trail_rows - 1)
		{
			bool found = false;
			if (ntuplestore_acc_tell(level_state->trail_reader, NULL))
				found = ntuplestore_acc_advance(level_state->trail_reader, 1);
			else if (!EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) ||
					 level_state->trail_rows == 0)
				found = ntuplestore_acc_seek_first(level_state->trail_reader);

			if (found ||
				EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail))
				level_state->num_trail_rows++;
		}
	}
	
	if (EDGE_IS_BOUND_PRECEDING(level_state->frame->lead) &&
		level_state->num_lead_rows == level_state->lead_rows)
	{
		ntuplestore_acc_seek_first(level_state->lead_reader);
	}
	
    else if ((!EDGE_IS_BOUND(level_state->frame->lead) &&
			  !EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true)) ||
		  level_state->num_lead_rows < level_state->lead_rows)
	{
		bool prev_set = ntuplestore_acc_tell(level_state->lead_reader, NULL);
		bool found = ntuplestore_acc_advance(level_state->lead_reader, 1);
		if (found || (prev_set && level_state->agg_filled))
			level_state->num_lead_rows++;
	}
}

/*
 * Adjust the trailing and leading edges for RANGE-based framing.
 */
static void
adjustEdgesForRange(WindowFrameBuffer buffer,
					WindowState *wstate)
{
	WindowStatePerLevel level_state = buffer->level_state;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	
	/* For a RANGE frame, we need to compare the key values between
	 * the trailing edge and the leading edge with the current_row.
	 */
	FrameBufferEntryForRange *trail_row = NULL;
	FrameBufferEntryForRange *lead_row = NULL;

	Datum new_trail_key;
	bool new_trail_key_isnull;
	Datum new_lead_key;
	bool new_lead_key_isnull;
	
	bool need_adjust = true;

	if (EDGE_IS_BOUND(level_state->frame->trail))
	{
		Assert(level_state->numSortCols == 1);
		new_trail_key = ExecEvalExpr(level_state->trail_expr, econtext,
									 &(new_trail_key_isnull), NULL);
			
		/* Adjust the trailing edge */
		while (need_adjust)
		{
			trail_row = getCurrentValue(level_state->trail_reader, level_state);

			/* If trail_reader does not point to any tuple in the buffer, we
			 * check on the first tuple.
			 */
			if (trail_row == NULL)
			{
				ntuplestore_acc_seek_first(level_state->trail_reader);
				trail_row = getCurrentValue(level_state->trail_reader, level_state);
			}

			need_adjust = cmp_deformed_tuple(trail_row->keys, trail_row->nulls,
											 &new_trail_key, &new_trail_key_isnull,
											 level_state->col_sort_asc,
											 level_state->numSortCols,
											 level_state->ltfunctions,
											 level_state->eqfunctions,
											 econtext->ecxt_per_tuple_memory,
											 false);
			if (need_adjust)
			{
				bool found = ntuplestore_acc_advance(level_state->trail_reader, 1);
				if (!found)
					break;
			}
		}

		if (!need_adjust)
		{
			/* Point to the previous tuple */
			ntuplestore_acc_advance(level_state->trail_reader, -1);
		}
		else
		{
			Datum curr_key;
			bool curr_key_isnull;

			/* Check if the last row is the trailing edge. */
			if (wstate->input_buffer->part_break)
			{
				/* If the last tuple breaks the partition key, the tuple to check
				 * is the last second tuple in the input buffer.
				 */
				bool found = ntuplestore_acc_advance(wstate->input_buffer->writer, -1);
				Assert(found);
				ntuplestore_acc_current_tupleslot(wstate->input_buffer->writer,
												  wstate->spare);

				/* Put the writer pointer back to its original place. */
				found = ntuplestore_acc_advance(wstate->input_buffer->writer, 1);
				Assert(found);
			}

			else
			{
				ntuplestore_acc_current_tupleslot(wstate->input_buffer->writer,
												  wstate->spare);
			}
				
			econtext->ecxt_scantuple = wstate->spare;
			deform_window_tuple(econtext->ecxt_scantuple,
								level_state->numSortCols, level_state->sortColIdx,
								&curr_key, &curr_key_isnull,
								wstate->transcontext);
			if (!cmp_deformed_tuple(&curr_key, &curr_key_isnull,
									&new_trail_key, &new_trail_key_isnull,
									level_state->col_sort_asc,
									level_state->numSortCols,
									level_state->ltfunctions,
									level_state->eqfunctions,
									econtext->ecxt_per_tuple_memory,
									false))
				ntuplestore_acc_seek_last(level_state->trail_reader);
		}
	}
	
	else if (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->trail, false))
	{
		bool found = ntuplestore_acc_advance(level_state->trail_reader, 1);
		if (!found)
			ntuplestore_acc_seek_first(level_state->trail_reader);
	}

	/* Adjust the leading edge */
	if (EDGE_IS_BOUND(level_state->frame->lead))
	{
		Assert(level_state->numSortCols == 1);
		
		new_lead_key = ExecEvalExpr(level_state->lead_expr, econtext,
									&(new_lead_key_isnull), NULL);

		need_adjust = true;
		while (need_adjust)
		{
			bool key_equal;
				
			lead_row = getCurrentValue(level_state->lead_reader, level_state);

			if (lead_row == NULL)
			{
				if (!EDGE_IS_BOUND_PRECEDING(level_state->frame->lead))
					break;
					
				ntuplestore_acc_seek_first(level_state->lead_reader);
				lead_row = getCurrentValue(level_state->lead_reader, level_state);
			}
			
			need_adjust = cmp_deformed_tuple(lead_row->keys, lead_row->nulls,
											 &new_lead_key, &new_lead_key_isnull,
											 level_state->col_sort_asc,
											 level_state->numSortCols,
											 level_state->ltfunctions,
											 level_state->eqfunctions,
											 econtext->ecxt_per_tuple_memory,
											 false);
			key_equal = cmp_deformed_tuple(lead_row->keys, lead_row->nulls,
										   &new_lead_key, &new_lead_key_isnull,
										   level_state->col_sort_asc,
										   level_state->numSortCols,
										   level_state->ltfunctions,
										   level_state->eqfunctions,
										   econtext->ecxt_per_tuple_memory,
										   true);
			if (need_adjust && !key_equal)
			{
				bool found = ntuplestore_acc_advance(level_state->lead_reader, 1);
				if (!found)
					break;
			}
			else if (!need_adjust && !key_equal)
			{
				/* We need to back off one. */
				bool found = ntuplestore_acc_advance(level_state->lead_reader, -1);
				level_state->lead_ready = true;
				if (!found)
					break;
			}
		}

		if (need_adjust)
		{
			Datum curr_key;
			bool curr_key_isnull;

			/* Check if the last row is the leading edge */
			if (wstate->input_buffer->part_break)
			{
				/* If the last tuple breaks the partition key, the tuple to check
				 * is the last second tuple in the input buffer.
				 */
				bool found = ntuplestore_acc_advance(wstate->input_buffer->writer, -1);
				Assert(found);
				ntuplestore_acc_current_tupleslot(wstate->input_buffer->writer,
												  wstate->spare);

				/* Put the writer pointer back to its original place. */
				found = ntuplestore_acc_advance(wstate->input_buffer->writer, 1);
				Assert(found);
			}

			else
			{
				ntuplestore_acc_current_tupleslot(wstate->input_buffer->writer,
												  wstate->spare);
			}
				
			econtext->ecxt_scantuple = wstate->spare;
			deform_window_tuple(econtext->ecxt_scantuple,
								level_state->numSortCols, level_state->sortColIdx,
								&curr_key, &curr_key_isnull,
								wstate->transcontext);
			if (!cmp_deformed_tuple(&curr_key, &curr_key_isnull,
									&new_lead_key, &new_lead_key_isnull,
									level_state->col_sort_asc,
									level_state->numSortCols,
									level_state->ltfunctions,
									level_state->eqfunctions,
									econtext->ecxt_per_tuple_memory,
									false) &&
				!cmp_deformed_tuple(&curr_key, &curr_key_isnull,
									&new_lead_key, &new_lead_key_isnull,
									level_state->col_sort_asc,
									level_state->numSortCols,
									level_state->ltfunctions,
									level_state->eqfunctions,
									econtext->ecxt_per_tuple_memory,
									true))
			{
				ntuplestore_acc_seek_last(level_state->lead_reader);
				level_state->lead_ready = true;
			}
			else
				level_state->lead_ready = false;
		}
	}
	else if (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
	{
		bool found;
		if (ntuplestore_acc_tell(buffer->current_row_reader, NULL))
		{
			NTupleStorePos pos;
			found = ntuplestore_acc_tell(buffer->current_row_reader, &pos);
			Assert(found);
			ntuplestore_acc_seek(level_state->lead_reader, &pos);
		}
		else
			found = ntuplestore_acc_advance(level_state->lead_reader, 1);

		if (found)
			level_state->lead_ready = true;
		else
			level_state->lead_ready = false;
	}
	else
	{
		ntuplestore_acc_advance(level_state->lead_reader, 1);
		level_state->lead_ready = false;
	}

	if (trail_row != NULL)
		freeFrameBufferEntryForRange(trail_row);
	if (lead_row != NULL)
		freeFrameBufferEntryForRange(lead_row);
}


/*
 * Adjust the trailing and leading edge to the frame buffer for
 * its list of key levels.
 */
static void
adjustEdges(WindowFrameBuffer buffer,
			WindowState *wstate)
{
	WindowStatePerLevel level_state = buffer->level_state;

	if (level_state->is_rows)
		adjustEdgesForRows(buffer, wstate);
	else
		adjustEdgesForRange(buffer, wstate);
}

/*
 * invokeNonTrivialFuncs -- compute transition values for
 * non-trivial window functions.
 */
static void
invokeNonTrivialFuncs(WindowState *wstate)
{
	int level;

	for (level=0; level<wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];

		computeFrameValue(level_state,
						  wstate,
						  level_state->trail_reader,
						  level_state->lead_reader);
	}
}

static void
finaliseFuncs(WindowState *wstate)
{
	int funcno;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	/*
	 * Put the current transition value into the econtext
	 */
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
				fcinfo.arg[0] = funcstate->final_aggTransValue;
				fcinfo.argnull[0] = funcstate->final_aggTransValueIsNull;
				if (fcinfo.flinfo->fn_strict && funcstate->final_aggTransValueIsNull)
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
				vals[funcno] = funcstate->final_aggTransValue;
				isnulls[funcno] = funcstate->final_aggTransValueIsNull;
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

	invokeTrivialFuncs(wstate, &found_complex_func);

	/* 
	 * If there are no functions/frames which need non-trivial window
	 * management, we're done.
	 */
	if (found_complex_func)
		invokeNonTrivialFuncs(wstate);

	finaliseFuncs(wstate);
}

/*
 * fetchCurrentRow -- retrieve the current_row from the input buffer.
 * 
 * The current_row_reader always points to the previous current_row
 * position. When there is no rows in the buffer, this function reads
 * the first tuple from the outer plan.
 */
static TupleTableSlot *
fetchCurrentRow(WindowState *wstate)
{
	bool found = false;
	WindowInputBuffer buffer = wstate->input_buffer;
	Window *window = (Window *)wstate->ps.plan;	
	bool has_prior_tuple = false;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	int level;
	
	if (buffer != NULL)
	{
		/* read the previous slot */
		ntuplestore_acc_current_tupleslot(buffer->current_row_reader,
										  wstate->priorslot);
		has_prior_tuple = true;
		
		found = ntuplestore_acc_advance(buffer->current_row_reader, 1);
	}
	
	if (found)
		wstate->cur_slot_is_new = false;
	else
	{
		/* Fetch the first tuple from the outer plan */
		TupleTableSlot *slot = ExecProcNode(outerPlanState(wstate));
		if (TupIsNull(slot))
			return NULL;

		if (buffer == NULL)
		{
			initializePartition(wstate);
		}
		
		ntuplestore_acc_put_tupleslot(wstate->input_buffer->writer, slot);
		wstate->input_buffer->num_tuples++;

		if (buffer == NULL)
		{
			/* The first tuple will not break any keys. */
			wstate->cur_slot_key_break = wstate->numlevels;
			ntuplestore_acc_seek_first(wstate->input_buffer->current_row_reader);
			ntuplestore_acc_seek_first(wstate->input_buffer->reader);
		}
		
		else
			ntuplestore_acc_seek_last(wstate->input_buffer->current_row_reader);

		buffer = wstate->input_buffer;

		if (wstate->curslot->tts_tupleDescriptor == NULL)
			ExecSetSlotDescriptor(wstate->curslot, 
								  slot->tts_tupleDescriptor);
		if (wstate->priorslot->tts_tupleDescriptor == NULL)
			ExecSetSlotDescriptor(wstate->priorslot, 
								  slot->tts_tupleDescriptor);
		if (wstate->spare->tts_tupleDescriptor == NULL)
			ExecSetSlotDescriptor(wstate->spare, 
								  slot->tts_tupleDescriptor);

		wstate->cur_slot_is_new = true;
	}
	
	found = ntuplestore_acc_current_tupleslot(buffer->current_row_reader,
											  wstate->curslot);
	Assert(found);

	wstate->cur_slot_part_break = false;
	wstate->cur_slot_key_break = -1;

	/* Check if the partition key breaks */
	if (has_prior_tuple)
	{
		wstate->cur_slot_part_break =
			(window->numPartCols &&
			 (!execTuplesMatch(wstate->curslot, wstate->priorslot, 
							   window->numPartCols, window->partColIdx, 
							   wstate->eqfunctions,
							   econtext->ecxt_per_tuple_memory)));
		if (wstate->cur_slot_part_break)
			wstate->input_buffer->part_break = true;
		else
		{
			int level = 0;
			bool match = true;
			
			/* Process ordering partial key that breaks.
			 * We also increment the peer_count.
			 */
			for (level=0; level<wstate->numlevels; level++)
			{
				WindowStatePerLevel lvl = &wstate->level_state[level];

				match = execTuplesMatch(wstate->curslot, wstate->priorslot, 
										lvl->numSortCols, 
										lvl->sortColIdx,
										lvl->eqfunctions,
										econtext->ecxt_per_tuple_memory);
				if (!match)
				{
					wstate->cur_slot_key_break = level;
					break;
				}
				lvl->peer_index++;
			}
		}
	}

	wstate->row_index++;
	if (wstate->cur_slot_key_break != -1 &&
		wstate->cur_slot_key_break < wstate->numlevels)
		advanceKeyLevelState(wstate, wstate->cur_slot_key_break);

	/* If this tuple breaks the partition key, re-initialize
	 * the state.
	 */
	if (wstate->cur_slot_part_break)
		initializePartition(wstate);

	/* Evaluate the LEAD/LAG offset values, and change the
	 * framing clauses accordingly.
	 */
	for (level=0; level<wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];

		if (!level_state->has_lead_lag)
			continue;

		if (EDGE_IS_DELAYED(level_state->frame->lead))
		{
			WindowFrameBuffer frame_buffer = level_state->frame_buffer;
			
			set_lead_lag_framing(level_state, wstate);

			level_state->num_lead_rows = 0;

			/* Reset the leading edge pointer in the frame buffer. */
			if (EDGE_IS_BOUND_FOLLOWING(level_state->frame->lead))
			{
				NTupleStorePos curr_row_pos;
				NTupleStorePos last_pos;
				
				if (ntuplestore_acc_tell(frame_buffer->current_row_reader, &curr_row_pos))
				{
					int ntuples;

					bool found = ntuplestore_acc_seek_last(level_state->lead_reader);
					Assert(found);
					found = ntuplestore_acc_tell(level_state->lead_reader, &last_pos);
					Assert(found);
					ntuples = ntuplestore_count_slot(frame_buffer->tuplestore,
													 &curr_row_pos, &last_pos);
					Assert(ntuples >= 0);
					if (ntuples >= level_state->lead_rows)
					{
						found = ntuplestore_acc_seek(level_state->lead_reader, &curr_row_pos);
						Assert(found);
						found = ntuplestore_acc_advance(level_state->lead_reader, level_state->lead_rows);
						Assert(found);
						level_state->num_lead_rows = level_state->lead_rows;
					}
					else
					{
						ntuplestore_acc_advance(level_state->lead_reader, 1);
						level_state->num_lead_rows = ntuples;
						if (level_state->agg_filled)
							level_state->num_lead_rows++;
					}
				}

				else
				{
					ntuplestore_acc_set_invalid(level_state->lead_reader);
					level_state->num_lead_rows = 0;
				}
			}

			else
			{
				NTupleStorePos curr_row_pos;
				NTupleStorePos first_pos;
				int ntuples;

				if (ntuplestore_acc_tell(frame_buffer->current_row_reader, &curr_row_pos))
				{
					bool found = ntuplestore_acc_seek_first(level_state->lead_reader);
					Assert(found);
					found = ntuplestore_acc_tell(level_state->lead_reader, &first_pos);
					Assert(found);
					
					ntuples = ntuplestore_count_slot(frame_buffer->tuplestore,
													 &first_pos, &curr_row_pos);
					Assert(ntuples >= 0);

					if (ntuples >= 0 - level_state->lead_rows)
					{
						found = ntuplestore_acc_seek(level_state->lead_reader, &curr_row_pos);
						Assert(found);
						found = ntuplestore_acc_advance(level_state->lead_reader, level_state->lead_rows);
						Assert(found);
						level_state->num_lead_rows = level_state->lead_rows;
					}
					else
					{
						ntuplestore_acc_advance(level_state->lead_reader, -1);
						level_state->num_lead_rows = 0 - ntuples;
					}
				}
				else
				{
					NTupleStorePos last_pos;
					if (ntuplestore_acc_seek_last(level_state->lead_reader))
					{
						found = ntuplestore_acc_tell(level_state->lead_reader, &last_pos);
						Assert(found);
						found = ntuplestore_acc_seek_first(level_state->lead_reader);
						Assert(found);
						found = ntuplestore_acc_tell(level_state->lead_reader, &first_pos);
						Assert(found);
						ntuples = ntuplestore_count_slot(frame_buffer->tuplestore,
														 &first_pos, &last_pos) + 1;
						Assert(ntuples >= 0);

						if (ntuples >= 0 - level_state->lead_rows)
						{
							ntuplestore_acc_advance(level_state->lead_reader,
													ntuples + level_state->lead_rows);
							level_state->num_lead_rows = level_state->lead_rows;
						}
						else
						{
							ntuplestore_acc_advance(level_state->lead_reader, -1);
							level_state->num_lead_rows = 0 - ntuples;
						}
					}
				}
			}
		}
	}

	return wstate->curslot;
}

/*
 * fetchTupleSlotThroughBuf -- fetch a tuple through the reader of the
 * input buffer.
 */
static TupleTableSlot *
fetchTupleSlotThroughBuf(WindowState *wstate)
{
	WindowInputBuffer buffer = wstate->input_buffer;
	bool found = false;
	Window *window = (Window *)wstate->ps.plan;	
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	bool has_prior_tuple = false;

	/* Read the previous tupleslot if any */
	if (ntuplestore_acc_tell(buffer->reader, NULL))
	{
		found = ntuplestore_acc_current_tupleslot(buffer->reader,
												  wstate->priorslot);
		has_prior_tuple = found;
	}
	
	if (found)
	{
		/* Advance the reader */
		found = ntuplestore_acc_advance(buffer->reader, 1);

		if (found)
		{
			found = ntuplestore_acc_current_tupleslot(buffer->reader,
													  wstate->spare);
			Assert(found);
			wstate->cur_slot_is_new = false;
		}
	}
	
	
	if (!found)
	{
		TupleTableSlot *slot = ExecProcNode(outerPlanState(wstate));
		if (TupIsNull(slot))
			return NULL;

		/* Put the new tuple into the input buffer */
		ntuplestore_acc_put_tupleslot(buffer->writer, slot);
		buffer->num_tuples++;

		ntuplestore_acc_seek_last(buffer->reader);

		ntuplestore_acc_current_tupleslot(buffer->writer,
										  wstate->spare);
		wstate->cur_slot_is_new = true;
	}
	
	wstate->cur_slot_part_break = false;
	wstate->cur_slot_key_break = -1;

	if (!TupIsNull(wstate->priorslot)) 
	{
		wstate->cur_slot_part_break =
			(window->numPartCols &&
			 (!execTuplesMatch(wstate->spare, wstate->priorslot, 
							   window->numPartCols, window->partColIdx, 
							   wstate->eqfunctions,
							   econtext->ecxt_per_tuple_memory)));

		if (wstate->cur_slot_part_break)
			buffer->part_break = true;

		else
		{
			int level = 0;
			bool match;
			
			/* Process ordering partial key that breaks.
			 * We also increment the peer_index.
			 */
			for (level=0; level<wstate->numlevels; level++)
			{
				WindowStatePerLevel lvl = &wstate->level_state[level];

				match = execTuplesMatch(wstate->spare, wstate->priorslot, 
										lvl->numSortCols, 
										lvl->sortColIdx,
										lvl->eqfunctions,
										econtext->ecxt_per_tuple_memory);
				if (!match)
				{
					wstate->cur_slot_key_break = level;
					break;
				}

				lvl->peer_count++;
			}
		}
	}

	return wstate->spare;
}

/*
 * ExecWindow
 *
 */
TupleTableSlot *
ExecWindow(WindowState *wstate)
{
	bool output_ready;
	TupleTableSlot *next_tupleslot;
	TupleTableSlot *resultSlot;
	ExprContext *econtext;
	int level;
	ExprDoneCond isDone;
	bool last_peer = false;

	econtext = wstate->ps.ps_ExprContext;

	/* Fetch the current_row */
	econtext->ecxt_scantuple = fetchCurrentRow(wstate);
	
	econtext->ecxt_outertuple =
		econtext->ecxt_scantuple; /* XXX really need this? */

	if (TupIsNull(econtext->ecxt_scantuple))
		return NULL; /* we are done */

	/* Process the current_row if it has not been processed. */
	if (wstate->cur_slot_is_new)
	{
		processTupleSlot(wstate, wstate->curslot, false);
		wstate->cur_slot_is_new = false;
	}

	/* Increment the current_row in the frame buffer for each
	 * key level.
	 */
	for(level=0; level<wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];

		if (level_state->empty_frame)
			continue;

		if (!level_state->trivial_frames_only)
		{
			if ((wstate->input_buffer->num_tuples > 1) &&
				(level_state->is_rows ||
				 (wstate->cur_slot_key_break != -1 &&
				  level >= wstate->cur_slot_key_break)))
			{
				econtext->ecxt_scantuple = wstate->curslot;
				incrementCurrentRow(level_state->frame_buffer, wstate);
			}
		}
	}

	output_ready = checkOutputReady(wstate);
	
	while (!output_ready)
	{
		next_tupleslot = fetchTupleSlotThroughBuf(wstate);

		if (TupIsNull(next_tupleslot) || wstate->cur_slot_part_break)
			last_peer = true;

		processTupleSlot(wstate, next_tupleslot, last_peer);

		if (!TupIsNull(next_tupleslot))
			output_ready = checkOutputReady(wstate);
		else
			output_ready = true;
	}

	econtext->ecxt_scantuple = wstate->curslot;
	econtext->ecxt_outertuple = wstate->curslot;

	invokeWindowFuncs(wstate);
	
	/*
	 * Form the result tuple using ExecProject(), and return it.
	 */
	resultSlot = ExecProject(wstate->ps.ps_ProjInfo, &isDone);

	return resultSlot;
}

/*
 * resetTransValue -- reset the transition value for
 * a given function.
 */
static void
resetTransValue(WindowStatePerFunction funcstate,
				MemoryContext context)
{
	if (funcstate->isAgg)
	{
		if (!funcstate->aggTranstypeByVal  && !funcstate->aggTransValueIsNull &&
			DatumGetPointer(funcstate->aggTransValue) != NULL)
			pfree (DatumGetPointer(funcstate->aggTransValue));
		
		if (funcstate->aggInitValueIsNull)
			funcstate->aggTransValue = funcstate->aggInitValue;
		else
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(context);
			funcstate->aggTransValue = datumCopy(funcstate->aggInitValue,
												 funcstate->aggTranstypeByVal,
												 funcstate->aggTranstypeLen);
			MemoryContextSwitchTo(oldcxt);
		}
	
		funcstate->aggTransValueIsNull = funcstate->aggInitValueIsNull;
		funcstate->aggNoTransValue = funcstate->aggInitValueIsNull;
	}
	else
	{
		int argno = 1;
		WindowRefExprState *wrxstate = funcstate->wrxstate;

		if (wrxstate->argtypbyval &&
			!wrxstate->argtypbyval[argno] && !funcstate->aggTransValueIsNull &&
			DatumGetPointer(funcstate->aggTransValue) != NULL)
			pfree(DatumGetPointer(funcstate->aggTransValue));

		funcstate->aggTransValue = 0;
		funcstate->aggTransValueIsNull = true;
		funcstate->aggNoTransValue = true;
	}
}


/*
 * resetTransValues -- reset the transition values for
 * each function.
 */
static void
resetTransValues(WindowStatePerLevel level_state,
				 MemoryContext context)
{
	ListCell *lc;
	foreach (lc, level_state->level_funcs)
	{
		WindowStatePerFunction funcstate = (WindowStatePerFunction) lfirst(lc);
		resetTransValue(funcstate, context);
	}
}


/*
 * processTupleSlot -- process a new tuple.
 *
 * This function passes the given tuple to each function in every
 * key level to compute an intermediate result.
 *
 * We only handle non-trivial window functions here.
 *
 * 'last_peer' indicates if the tuple to be inserted is the last tuple from
 * the input. It is used to determine if the trailing edge and leading edge
 * need to be adjusted.
 */
static void
processTupleSlot(WindowState *wstate, TupleTableSlot *slot, bool last_peer)
{
	int level;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	for (level=0; level<wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];
		ListCell *lc;
		bool adv_peercount = false;

		if (level_state->empty_frame)
			continue;

		if (!level_state->trivial_frames_only)
		{
			bool write_pre_value = false;
			if (wstate->input_buffer->num_tuples > 1 &&
				wstate->cur_slot_is_new)
			{
				if (level_state->is_rows)
					write_pre_value = true;
				else if (wstate->cur_slot_part_break || TupIsNull(slot))
					write_pre_value = true;
				else if ((wstate->cur_slot_key_break != -1 &&
						  level >= wstate->cur_slot_key_break))
					write_pre_value = true;
			}
			
			/* If this tuple breaks the order by key in this level,
			 * we write out previous aggregate values if any.
			 */
			if (write_pre_value)
			{
				/*
				 * If there is a function that requires peer counts, 
				 * we need to compute its preliminary value before 
				 * appending it to the frame buffer.
				 */
				if (level_state->need_peercount)
				{
					foreach(lc, level_state->level_funcs)
					{
						WindowStatePerFunction funcstate =
							(WindowStatePerFunction) lfirst(lc);
						if (funcstate->winpeercount)
							add_tuple_to_trans(funcstate, wstate, econtext);
					}
				}

				/* Set econtext->ecxt_scantuple because the range
				 * frame needs this for order keys.
				 */
				econtext->ecxt_scantuple = wstate->priorslot;
				appendToFrameBuffer(level_state, wstate, last_peer);

				/* Reset the transition value for those functions which
				 * has preliminary functions, but not inverse preliminary
				 * functions.
				 */
				foreach (lc, level_state->level_funcs)
				{
					WindowStatePerFunction funcstate = (WindowStatePerFunction) lfirst(lc);
					if (funcstate->isAgg &&
						!funcstate->cumul_frame &&
						!OidIsValid(funcstate->invprelimfn_oid) &&
						OidIsValid(funcstate->prelimfn_oid))
						resetTransValue(funcstate, wstate->transcontext);

					else if ((IS_LEAD_LAG(funcstate->wrxstate->winkind) ||
							  IS_FIRST_LAST(funcstate->wrxstate->winkind)) && !last_peer)
						resetTransValue(funcstate, wstate->transcontext);
				}
			}

			/* If this slot causes the partition key to break, we don't add
			 * this tuple to the transition value.
			 */
			if (wstate->cur_slot_part_break || TupIsNull(slot))
			{
				if (write_pre_value)
					level_state->agg_filled = false;
				
				continue;
			}
			
			foreach(lc, level_state->level_funcs)
			{
				WindowStatePerFunction funcstate = (WindowStatePerFunction) lfirst(lc);
				ExprContext *econtext = wstate->ps.ps_ExprContext;
				
				if (funcstate->trivial_frame)
					continue;

				/* Increment the values for peer counts. There may be more than
				 * one such function in one key level. We should catch this in the
				 * earlier stage, but in case we didn't, we only do this once.
				 */
				if (!adv_peercount && funcstate->winpeercount)
				{
					if (wstate->cur_slot_is_new &&
						wstate->cur_slot_key_break != -1 &&
						wstate->cur_slot_key_break < wstate->numlevels)
						advancePeerCount(wstate, wstate->cur_slot_key_break);

					adv_peercount = true;
				}
				
				else
				{
					/* Add this tuple to its transition value */
					econtext->ecxt_scantuple = slot;
					econtext->ecxt_outertuple = slot; /* XXX really need this? */
					
					add_tuple_to_trans(funcstate, wstate, econtext);

					level_state->agg_filled = true;
				}
			}
		}
	}
}


/*
 * checkOutputReady -- check each key level to see if its frame buffer
 * has sufficient data to compute one output rows. If so, return true,
 * othwise, return false.
 */
static bool
checkOutputReady(WindowState *wstate)
{
	int level;
	ExprContext *econtext = wstate->ps.ps_ExprContext;

	econtext->ecxt_scantuple = wstate->curslot;

	if (wstate->input_buffer->part_break)
		return true;

	for (level=0; level<wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];

		if (level_state->trivial_frames_only)
			continue;

		if (level_state->is_rows)
		{
			if (!hasEnoughDataInRows(level_state->frame_buffer,
									 level_state,
									 econtext,
									 level_state->trail_rows,
									 level_state->lead_rows))
				return false;
		}
		
		else
		{
			if (!hasEnoughDataInRange(level_state->frame_buffer,
									  level_state,
									  econtext,
									  wstate->transcontext,
									  level_state->trail_range,
									  level_state->lead_range))
				return false;
		}
	}

	return true;
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
	bool isNull;
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

	if ((EDGE_IS_BOUND_PRECEDING(edge) && 
		 level_state->col_sort_asc[0]) ||
		(EDGE_IS_BOUND_FOLLOWING(edge) &&
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
	
	ReleaseSysCache(tup);

	exprrestype = opr->oprresult;
	expr = make_opclause(HeapTupleGetOid(tup), exprrestype,
						 false, varexpr, 
						 (Expr *)edge->val);
	((OpExpr *)expr)->opfuncid = opr->oprcode;

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

/*
 * Initialise frame specific state.
 */

static void
init_frames(WindowState *wstate)
{
	int level = -1;
	ListCell *lc;
	int col_no;

	for(level=0; level<wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];
		int ncols = level_state->numSortCols;
		WindowFrame *frame = level_state->frame;

		level_state->col_sort_asc = palloc(sizeof(bool) * ncols);
		foreach (lc, level_state->level_funcs)
		{
			WindowStatePerFunction funcstate =
				(WindowStatePerFunction)lfirst(lc);

			if (!frame || (!funcstate->isAgg && !funcstate->allowframe &&
						   !IS_LEAD_LAG(funcstate->wrxstate->winkind) &&
						   !funcstate->winpeercount))
			{
				funcstate->trivial_frame = true;
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

			else if (funcstate->isAgg && frame && !frame->is_rows &&
					 frame->trail->kind == WINDOW_UNBOUND_PRECEDING && 
				frame->lead->kind == WINDOW_CURRENT_ROW &&
				frame->exclude == WINDOW_EXCLUSION_NULL)
			{
				funcstate->cumul_frame = true;
			}

			else
			{
				/* look for empty frames */
				if (frame->trail->kind == WINDOW_UNBOUND_FOLLOWING ||
					frame->lead->kind == WINDOW_UNBOUND_PRECEDING ||
					(EDGE_IS_BOUND_FOLLOWING(frame->trail) &&
					 EDGE_IS_BOUND_PRECEDING(frame->lead)))
				{
					level_state->empty_frame = true;
				}
			}
		}
		
		/* what sort order did the user specify for each key? */
		for (col_no = 0; col_no < ncols; col_no++)
		{
			Oid sortop = level_state->sortOperators[col_no];
			Form_pg_operator op;
			HeapTuple optup = SearchSysCache(OPEROID,
							   				 ObjectIdGetDatum(sortop),
							   				 0, 0, 0);
			Insist(HeapTupleIsValid(optup));
			op = (Form_pg_operator)GETSTRUCT(optup);
			ReleaseSysCache(optup);

			if (strcmp(NameStr(op->oprname), "<") == 0)
				level_state->col_sort_asc[col_no] = true;
			else
				level_state->col_sort_asc[col_no] = false;
		}

		/* now, initialize the actual frame */
		if (frame)
		{
			if (frame->is_rows)
			{
				ExprContext *econtext = wstate->ps.ps_ExprContext;

				if (EDGE_IS_BOUND(frame->trail) &&
					!EDGE_IS_DELAYED_BOUND(frame->trail) && /* XXX */
					level_state->frame->trail->val != NULL)
				{
					long int rows_param;
					bool isnull;
					
					level_state->trail_expr =
						ExecInitExpr((Expr *)level_state->frame->trail->val, 
									 (PlanState *) wstate); 

					rows_param = ExecEvalExpr(level_state->trail_expr,
											  econtext,
											  &isnull,
											  NULL);

					Insist(!isnull);

					if (frame->trail->kind == WINDOW_BOUND_PRECEDING)
						rows_param = -rows_param;

					level_state->trail_rows = rows_param;
				}
				else
					level_state->trail_rows = 0L;

				if (EDGE_IS_BOUND(frame->lead) &&
					!EDGE_IS_DELAYED_BOUND(frame->lead) && /* XXX */
					level_state->frame->lead->val != NULL)
				{
					long int rows_param;
					bool isnull;

					level_state->lead_expr = 
						ExecInitExpr((Expr *)level_state->frame->lead->val, 
									 (PlanState *) wstate); 

					rows_param = ExecEvalExpr(level_state->lead_expr,
											  econtext,
											  &isnull,
											  NULL);

					Insist(!isnull);

					if (frame->lead->kind == WINDOW_BOUND_PRECEDING)
						rows_param = -rows_param;
					
					level_state->lead_rows = rows_param;
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
											   level_state->sortColIdx[0],
											   true, level_state, wstate);
				}

				if (EDGE_IS_BOUND(frame->lead) &&
					!EDGE_IS_DELAYED_BOUND(frame->lead))
				{
					init_bound_frame_edge_expr(frame->lead, desc,
											   level_state->sortColIdx[0],
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
				if (level_state->lead_rows < level_state->trail_rows)
					level_state->empty_frame = true;
			}
			else
			{
				Oid ineq_ordfuncid;
				Oid eq_ordfuncid;
				TupleDesc tup = ExecGetResultType(wstate->ps.lefttree); 
				AttrNumber attnum;
				Datum ineq_datum;
				Datum eq_datum;
				FmgrInfo ineq_fcinfo;
				FmgrInfo eq_fcinfo;
				Operator ineq_optup;
				Operator eq_optup;

				attnum = level_state->sortColIdx[0];

				ineq_optup = ordering_oper(tup->attrs[attnum - 1]->atttypid,
										   false);
				ineq_ordfuncid = oprfuncid(ineq_optup); 
				ReleaseSysCache(ineq_optup);
				fmgr_info(ineq_ordfuncid, &ineq_fcinfo);

				eq_optup = equality_oper(tup->attrs[attnum - 1]->atttypid,
										 false);
				eq_ordfuncid = oprfuncid(eq_optup); 
				ReleaseSysCache(eq_optup);
				fmgr_info(eq_ordfuncid, &eq_fcinfo);

				/* is trail less than or equal to lead */
				ineq_datum = FunctionCall2(&ineq_fcinfo, level_state->trail_range,
										   level_state->lead_range);
				eq_datum = FunctionCall2(&eq_fcinfo, level_state->trail_range,
										 level_state->lead_range);
				if (is_pre)
				{
					if (DatumGetBool(ineq_datum) && !DatumGetBool(eq_datum))
						level_state->empty_frame = true;
				}
				else
				{
					if (!DatumGetBool(ineq_datum) && !DatumGetBool(eq_datum))
						level_state->empty_frame = true;
				}
			}
		}
	}

	/*
	 * Set trivial_frames_only in each level state if its functions
	 * all have trivial frames.
	 *
	 * Also initialize the transition values for functions
	 * in this level.
	 */
	for (level=0; level<wstate->numlevels; level++)
	{
		bool trivial_frames_only = true;
		WindowStatePerLevel level_state = &wstate->level_state[level];
		
		foreach(lc, level_state->level_funcs)
		{
			WindowStatePerFunction funcstate = (WindowStatePerFunction)
				lfirst(lc);
			
			if (!funcstate->trivial_frame)
			{
				trivial_frames_only = false;
				break;
			}
		}
		level_state->trivial_frames_only = trivial_frames_only;

		resetTransValues(level_state, wstate->transcontext);
	}

	/*
	 * Set has_lead_lag flag in each level state if all functions
	 * in this level are LEAD/LAGs with DELAYED framing.
	 */
	for (level=0; level<wstate->numlevels; level++)
	{
		WindowStatePerLevel level_state = &wstate->level_state[level];
		bool has_lead_lag = false;
		int funcno = 0;

		foreach(lc, level_state->level_funcs)
		{
			WindowStatePerFunction funcstate =
				(WindowStatePerFunction) lfirst(lc);

			if (IS_LEAD_LAG(funcstate->wrxstate->winkind))
			{
				Assert(funcno == 0 || has_lead_lag);
				has_lead_lag = true;
			}

			funcno++;
		}
		level_state->has_lead_lag = has_lead_lag;
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

/*
 * set_lead_lag_framing -- Set the framing for LEAD/LAG functions.
 */
static void
set_lead_lag_framing(WindowStatePerLevel level_state,
					 WindowState *wstate)
{
	WindowStatePerFunction funcstate;
	WindowRefExprState *wrxstate;
	int nargs;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	bool is_const = false;
		
	funcstate = (WindowStatePerFunction)
		linitial(level_state->level_funcs);

	/* Compute the offset value from the arguments of LEAD/LAG
	 * function. If such value is not provided, the default
	 * is 1.
	 */
	wrxstate = funcstate->wrxstate;
	nargs = list_length(wrxstate->args);
	Assert(2 <= nargs && nargs <= 4);

	if (list_length(wrxstate->args) == 2)
	{
		if (IS_LEAD(wrxstate->winkind))
			level_state->lead_rows = 1;
		else
			level_state->lead_rows = -1;

		is_const = true;
	}
		
	else
	{
		FunctionCallInfoData fcinfo;
		ExprState *argstate;
		int argno = 2; /* the index for the offset */
			
		Assert(nargs <= 4);
		argstate = (ExprState *)list_nth(wrxstate->args, argno);
		is_const = IsA(argstate->expr, Const);

		econtext->ecxt_scantuple = wstate->curslot;
		if (!TupIsNull(wstate->curslot) || is_const)
		{
			fcinfo.arg[argno] = ExecEvalExpr(argstate, econtext,
											 fcinfo.argnull + argno, NULL);

			Assert(!fcinfo.argnull[argno]);

			/* XXX We need to check on the type for this offset argument? */

			level_state->lead_rows = DatumGetInt64(fcinfo.arg[argno]);
			if (IS_LAG(funcstate->wrxstate->winkind))
				level_state->lead_rows = 0 - level_state->lead_rows;
		}
		else
			level_state->lead_rows = 0;
	}

	if (is_const)
	{
		if (level_state->lead_rows >= 0)
			level_state->frame->lead->kind = WINDOW_BOUND_FOLLOWING;
		else
			level_state->frame->lead->kind = WINDOW_BOUND_PRECEDING;
	}
	else
	{
		if (IS_LEAD(funcstate->wrxstate->winkind))
			level_state->frame->lead->kind = WINDOW_DELAYED_BOUND_FOLLOWING;
		else
			level_state->frame->lead->kind = WINDOW_DELAYED_BOUND_PRECEDING;
	}

	level_state->frame->trail->kind = level_state->frame->lead->kind;
	level_state->trail_rows = level_state->lead_rows;
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
	int numrefs;
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
	wstate->curslot = ExecInitExtraTupleSlot(estate);
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

	initWindowStatePerLevel(wstate, node);

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

	initWindowFuncState(wstate, node);

	/* Frame initialisation can take place now */	
	init_frames(wstate);

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
	if (node->input_buffer)
		freeInputBuffer(node);

	freeFrameBuffers(node);

	/*  Free the exprcontext */
	ExecFreeExprContext(&node->ps);

	/* clean out the tuple table */
	ExecClearTuple(node->curslot);
	ExecClearTuple(node->spare);
	ExecClearTuple(node->priorslot);
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	if (node->transcontext != NULL)
		MemoryContextDelete(node->transcontext);

	pfree(node->serial_array);

	if (node->numlevels > 0)
		pfree(node->level_state);
	if (node->func_state != NULL)
		pfree(node->func_state);

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
		result = 0.0;
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
	
	result = level_state->prior_non_peer_count + (level_state->peer_count + 1);

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
	WindowRefExprState *wrxstate =
		(WindowRefExprState *)PG_GETARG_POINTER(0);
	WindowState *wstate = wrxstate->windowstate;
	ExprContext *econtext = wstate->ps.ps_ExprContext;
	WindowStatePerFunction funcstate = &wstate->func_state[wrxstate->funcno];
	WindowStatePerLevel level_state = funcstate->wlevel;
	FrameBufferEntryForRows *rows_lead_entry = NULL;
	WindowValue *lead_value = NULL;

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

	Assert(level_state->is_rows);
	if (!EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) ||
		level_state->num_lead_rows >= level_state->trail_rows)
		rows_lead_entry = getCurrentValue(level_state->lead_reader, level_state);
			
	if (rows_lead_entry != NULL)
	{
		lead_value = (WindowValue *)list_nth(rows_lead_entry->func_values,
											 funcstate->serial_index);

		funcstate->win_value = lead_value->value;
		funcstate->win_value_is_null = lead_value->valueIsNull;
	}

	/* The value could be stored in aggTransValue. */
	else if (level_state->agg_filled &&
			 (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true) ||
			 (EDGE_IS_BOUND_FOLLOWING(level_state->frame->lead) &&
			  level_state->num_lead_rows >= level_state->trail_rows)))
	{
		funcstate->win_value = funcstate->aggTransValue;
		funcstate->win_value_is_null = funcstate->aggTransValueIsNull;
	}
	
	else
	{
		WindowRefExprState *wrxstate = funcstate->wrxstate;
		ExprState *argstate;
		int nargs = list_length(wrxstate->args);
		FunctionCallInfoData fcinfo;
		int argno = 3;

		Assert(nargs <= 4);

		if (level_state->lead_rows < 0 ||
			level_state->num_lead_rows < level_state->lead_rows)
		{
			/* Use the default value if any. */
			if (nargs == 4)
			{
				argstate = (ExprState *)list_nth(wrxstate->args, argno);
				funcstate->win_value = ExecEvalExpr(argstate, econtext,
														fcinfo.argnull + argno, NULL);
				funcstate->win_value_is_null = fcinfo.argnull[argno];
			}
			
			else
			{
				funcstate->win_value = 0;
				funcstate->win_value_is_null = true;
			}
		}

		else
		{
			funcstate->win_value = 0;
			funcstate->win_value_is_null = true;
		}
	}

	if (rows_lead_entry != NULL)
		freeFrameBufferEntryForRows(rows_lead_entry);

	*isnull = funcstate->win_value_is_null;
	
	return funcstate->win_value;
}

/*
 * Initial implementation of LEAD(col, offset, value_expr)
 */
Datum
lead_int(PG_FUNCTION_ARGS)
{
	bool isnull;
	Datum d;
	
	d = lead_lag_internal(fcinfo, true, &isnull);

	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(d);
	
}

Datum
lead_bool(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_char(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_cidr(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_circle(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_float4(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_float8(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_inet(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_interval(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_line(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_lseg(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_macaddr(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_smallint(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_int8(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_money(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_name(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_numeric(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_oid(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_path(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_point(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_polygon(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_reltime(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_text(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_tid(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_time(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_timestamp(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_timestamptz(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_timetz(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_varbit(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_varchar(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lead_xid(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, true, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_bool(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_char(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_cidr(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_circle(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_float4(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_float8(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_inet(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_interval(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_line(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_lseg(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_macaddr(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_smallint(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_int4(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_int8(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_money(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_name(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_numeric(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_oid(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_path(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_point(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_polygon(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_reltime(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_text(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_tid(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_time(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_timestamp(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_timestamptz(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_timetz(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_varbit(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_varchar(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

Datum
lag_xid(PG_FUNCTION_ARGS)
{   
    bool isnull;
    Datum d;
    
    d = lead_lag_internal(fcinfo, false, &isnull);
    
    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(d);
}

static Datum
last_value_internal(WindowRefExprState *wrxstate, bool *isnull)
{
	WindowState *wstate = wrxstate->windowstate;
	WindowStatePerFunction funcstate = &wstate->func_state[wrxstate->funcno];
	WindowStatePerLevel level_state = funcstate->wlevel;
	FrameBufferEntryForRows *rows_lead_entry = NULL;
	FrameBufferEntryForRange *range_lead_entry = NULL;
	WindowValue *lead_value = NULL;
	bool lead_valid = true;
	bool has_tuples = true;
	bool include_last_agg = false;

	has_tuples = hasTuplesInFrame(level_state);
		
	if (has_tuples)
	{
		if (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->trail, false))
			include_last_agg = true;

		if (!EDGE_IS_BOUND(level_state->frame->lead) ||
			EDGE_IS_BOUND_FOLLOWING(level_state->frame->lead)  ||
			EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
			include_last_agg = true;

		if (!level_state->agg_filled &&
			!ntuplestore_acc_tell(level_state->lead_reader, NULL))
		{
			lead_valid = false;
			ntuplestore_acc_seek_last(level_state->lead_reader);
		}

		if (level_state->is_rows)
		{
			if (!EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) ||
				level_state->num_lead_rows >= level_state->trail_rows)
			{
				rows_lead_entry = getCurrentValue(level_state->lead_reader, level_state);
				if (rows_lead_entry != NULL)
					lead_value = (WindowValue *)list_nth(rows_lead_entry->func_values,
														 funcstate->serial_index);
			}
		}
		else
		{
			range_lead_entry = getCurrentValue(level_state->lead_reader, level_state);
			if (range_lead_entry != NULL)
				lead_value = (WindowValue *)list_nth(range_lead_entry->func_values,
													 funcstate->serial_index);
		}
	}

	if (lead_value != NULL)
	{
		funcstate->win_value = lead_value->value;
		funcstate->win_value_is_null = lead_value->valueIsNull;
	}

	else
	{
		if (has_tuples && include_last_agg && level_state->agg_filled)
		{
			funcstate->win_value = funcstate->aggTransValue;
			funcstate->win_value_is_null = funcstate->aggTransValueIsNull;
		}

		else
		{
			funcstate->win_value = 0;
			funcstate->win_value_is_null = true;
		}
	}

	*isnull = funcstate->win_value_is_null;

	if (!lead_valid)
		ntuplestore_acc_set_invalid(level_state->lead_reader);

	return funcstate->win_value;
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
first_value_internal(WindowRefExprState *wrxstate, bool *isnull)
{
	WindowState *wstate = wrxstate->windowstate;
	WindowStatePerFunction funcstate = &wstate->func_state[wrxstate->funcno];
	WindowStatePerLevel level_state = funcstate->wlevel;
	FrameBufferEntryForRows *rows_trail_entry = NULL;
	FrameBufferEntryForRange *range_trail_entry = NULL;
	WindowValue *trail_value = NULL;
	NTupleStorePos orig_pos;
	bool trail_valid;
	bool has_tuples = true;

	has_tuples = hasTuplesInFrame(level_state);

	if (!has_tuples)
	{
		funcstate->win_value = 0;
		funcstate->win_value_is_null = true;

		*isnull = true;
		return funcstate->win_value;
	}
	
	/* Save the position for the trail_reader */
	trail_valid = ntuplestore_acc_tell(level_state->trail_reader, &orig_pos);

	/* Since the trail_reader points to the value that is right before
	 * the trailing edge, we advance the trail_reader by one.
	 */
	if (trail_valid)
		ntuplestore_acc_advance(level_state->trail_reader, 1);
	else
	{
		if (!EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) ||
			EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->trail, false))
			ntuplestore_acc_seek_first(level_state->trail_reader);
	}

	if (level_state->is_rows)
	{
		if (!EDGE_IS_BOUND_FOLLOWING(level_state->frame->trail) ||
			level_state->num_lead_rows >= level_state->trail_rows)
		{
			rows_trail_entry = getCurrentValue(level_state->trail_reader, level_state);
			if (rows_trail_entry != NULL)
				trail_value = (WindowValue *)list_nth(rows_trail_entry->func_values,
													  funcstate->serial_index);
		}
	}
	else
	{
		range_trail_entry = getCurrentValue(level_state->trail_reader, level_state);
		if (range_trail_entry != NULL)
			trail_value = (WindowValue *)list_nth(range_trail_entry->func_values,
												  funcstate->serial_index);
	}

	if (trail_value != NULL)
	{
		funcstate->win_value = trail_value->value;
		funcstate->win_value_is_null = trail_value->valueIsNull;
	}

	else
	{
		bool include_last_agg = false;
		
		if (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->trail, false))
			include_last_agg = true;

		if (EDGE_EQ_CURRENT_ROW(level_state, level_state->frame->lead, true))
			include_last_agg = true;

		if ((has_tuples || include_last_agg || trail_valid) &&
			level_state->agg_filled)
		{
			funcstate->win_value = funcstate->aggTransValue;
			funcstate->win_value_is_null = funcstate->aggTransValueIsNull;
		}
		
		else
		{
			funcstate->win_value = 0;
			funcstate->win_value_is_null = true;
		}
	}

	/* Reset the trail_reader to its original position. */
	if (!ntuplestore_acc_seek(level_state->trail_reader, &orig_pos))
		ntuplestore_acc_set_invalid(level_state->trail_reader);

	*isnull = funcstate->win_value_is_null;

	return funcstate->win_value;
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
