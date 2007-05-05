/*-------------------------------------------------------------------------
 *
 * nodeAgg.h
 *	  prototypes for nodeAgg.c
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeAgg.h,v 1.27 2006/07/13 16:49:19 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAGG_H
#define NODEAGG_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsAgg(Agg *node);
extern AggState *ExecInitAgg(Agg *node, EState *estate, int eflags);
extern TupleTableSlot *ExecAgg(AggState *node);
extern void ExecEndAgg(AggState *node);
extern void ExecReScanAgg(AggState *node, ExprContext *exprCtxt);

extern Size hash_agg_entry_size(int numAggs);

extern Datum aggregate_dummy(PG_FUNCTION_ARGS);

/* MPP needs to see these in execHashAgg.c */

/*
 * AggStatePerGroupData - per-aggregate-per-group working state
 *
 * These values are working state that is initialized at the start of
 * an input tuple group and updated for each input tuple.
 *
 * In AGG_PLAIN and AGG_SORTED modes, we have a single array of these
 * structs (pointed to by aggstate->pergroup); we re-use the array for
 * each input group, if it's AGG_SORTED mode.  In AGG_HASHED mode, the
 * hash table contains an array of these structs for each tuple group.
 *
 * Logically, the sortstate field belongs in this struct, but we do not
 * keep it here for space reasons: we don't support DISTINCT aggregates
 * in AGG_HASHED mode, so there's no reason to use up a pointer field
 * in every entry of the hashtable.
 */
typedef struct AggStatePerGroupData
{
	Datum		transValue;		/* current transition value */
	bool		transValueIsNull;

	bool		noTransValue;	/* true if transValue not set yet */

	/*
	 * Note: noTransValue initially has the same value as transValueIsNull,
	 * and if true both are cleared to false at the same time.	They are not
	 * the same though: if transfn later returns a NULL, we want to keep that
	 * NULL and not auto-replace it with a later input value. Only the first
	 * non-NULL input will be auto-substituted.
	 */
} AggStatePerGroupData;

extern void 
initialize_aggregates(AggState *aggstate,
					  AggStatePerAgg peragg,
					  AggStatePerGroup pergroup);
extern void 
advance_aggregates(AggState *aggstate, AggStatePerGroup pergroup);

extern List *
get_agg_hash_collist(AggState *aggstate);

extern Oid resolve_polymorphic_transtype(Oid aggtranstype, Oid aggfnoid,
										 Oid *inputTypes);

extern Datum GetAggInitVal(Datum textInitVal, Oid transtype);

extern Datum invoke_agg_trans_func(FmgrInfo transfn, int numargs, 
								   Datum transValue, bool *noTransvalue, 
								   bool *transValueIsNull, bool transtypeByVal,
								   int16 transtypeLen,
								   FunctionCallInfoData *fcinfo, void *funcctx,
								   MemoryContext transcontext, 
								   MemoryContext tuplecontext);
#endif   /* NODEAGG_H */
