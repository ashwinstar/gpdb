/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 *	  ExecAgg evaluates each aggregate in the following steps:
 *
 *		 transvalue = initcond
 *		 foreach input_tuple do
 *			transvalue = transfunc(transvalue, input_value(s))
 *		 result = finalfunc(transvalue)
 *
 *	  If a finalfunc is not supplied then the result is just the ending
 *	  value of transvalue.
 *
 *	  If transfunc is marked "strict" in pg_proc and initcond is NULL,
 *	  then the first non-NULL input_value is assigned directly to transvalue,
 *	  and transfunc isn't applied until the second non-NULL input_value.
 *	  The agg's first input type and transtype must be the same in this case!
 *
 *	  If transfunc is marked "strict" then NULL input_values are skipped,
 *	  keeping the previous transvalue.	If transfunc is not strict then it
 *	  is called for every input tuple and must deal with NULL initcond
 *	  or NULL input_values for itself.
 *
 *	  If finalfunc is marked "strict" then it is not called when the
 *	  ending transvalue is NULL, instead a NULL result is created
 *	  automatically (this is just the usual handling of strict functions,
 *	  of course).  A non-strict finalfunc can make its own choice of
 *	  what to return for a NULL ending transvalue.
 *
 *	  We compute aggregate input expressions and run the transition functions
 *	  in a temporary econtext (aggstate->tmpcontext).  This is reset at
 *	  least once per input tuple, so when the transvalue datatype is
 *	  pass-by-reference, we have to be careful to copy it into a longer-lived
 *	  memory context, and free the prior value to avoid memory leakage.
 *
 *    Postgres stores transvalues in the memory context aggstate->aggcontext,
 *	  which is also used for the hashtable structures in AGG_HASHED mode.
 *    MPP (in order to support hybrid hash aggregation) stores hash table
 *    entries and associated transition values in aggstate->transcontext.
 *    In AGG_PLAIN and AGG_SORTED cases, these contexts are the same, but
 *    for AGG_HASHED they differ.
 *
 *	  The node's regular econtext (aggstate->csstate.cstate.cs_ExprContext)
 *	  is used to run finalize functions and compute the output tuple;
 *	  this context can be reset once per output tuple.
 *
 *	  Beginning in PostgreSQL 8.1, the executor's AggState node is passed as
 *	  the fmgr "context" value in all transfunc and finalfunc calls.  It is
 *	  not really intended that the transition functions will look into the
 *	  AggState node, but they can use code like
 *			if (fcinfo->context && IsA(fcinfo->context, AggState))
 *	  to verify that they are being called by nodeAgg.c and not as ordinary
 *	  SQL functions.  The main reason a transition function might want to know
 *	  that is that it can avoid palloc'ing a fixed-size pass-by-ref transition
 *	  value on every call: it can instead just scribble on and return its left
 *	  input.  Ordinarily it is completely forbidden for functions to modify
 *	  pass-by-ref inputs, but in the aggregate case we know the left input is
 *	  either the initial transition value or a previous function result, and
 *	  in either case its value need not be preserved.  See int8inc() for an
 *	  example.	Notice that advance_transition_function() is coded to avoid a
 *	  data copy step when the previous transition value pointer is returned.
 *
 *
 * Portions Copyright (c) 2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeAgg.c,v 1.146.2.1 2007/02/02 00:07:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/execHashagg.h"
#include "executor/nodeAgg.h"
#include "lib/stringinfo.h"             /* StringInfo */
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/datum.h"

#include "cdb/cdbvars.h" /* mpp_hybrid_hash_agg */

#if 0
/* debugging ... */
#include "utils/debugutils.h"
#endif

/*
 * AggStatePerAggData - per-aggregate working state for the Agg scan
 */
typedef struct AggStatePerAggData
{
	/*
	 * These values are set up during ExecInitAgg() and do not change
	 * thereafter:
	 */

	/* Links to Aggref expr and state nodes this working state is for */
	AggrefExprState *aggrefstate;
	Aggref	   *aggref;

	/* number of input arguments for aggregate */
	int			numArguments;

	/* Oids of transfer functions */
	Oid			transfn_oid;
	Oid			finalfn_oid;	/* may be InvalidOid */

	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid.  Note in particular that fn_strict
	 * flags are kept here.
	 */
	FmgrInfo	transfn;
	FmgrInfo	finalfn;

	/*
	 * Type of input data and Oid of sort operator to use for it; only
	 * set/used when aggregate has DISTINCT flag.  (These are not used
	 * directly by nodeAgg, but must be passed to the Tuplesort object.)
	 */
	Oid			inputType;
	Oid			sortOperator;

	/*
	 * fmgr lookup data for input type's equality operator --- only set/used
	 * when aggregate has DISTINCT flag.
	 */
	FmgrInfo	equalfn;

	/*
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * We need the len and byval info for the agg's input, result, and
	 * transition data types in order to know how to copy/delete values.
	 */
	int16		inputtypeLen,
				resulttypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				resulttypeByVal,
				transtypeByVal;

	/*
	 * These values are working state that is initialized at the start of an
	 * input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT) aggregate, we just feed the input values
	 * straight to the transition function.  If it's DISTINCT, we pass the
	 * input values into a Tuplesort object; then at completion of the input
	 * tuple group, we scan the sorted values, eliminate duplicates, and run
	 * the transition function on the rest.
	 */

	Tuplesortstate *sortstate;	/* sort object, if a DISTINCT agg */
} AggStatePerAggData;

/*
 * AggStatePerGroupData - per-aggregate-per-group working state
 *
 * Definition moved to nodeAgg.c to provide visibility to execHashagg.c
 */



/*
 * To implement hashed aggregation, we need a hashtable that stores a
 * representative tuple and an array of AggStatePerGroup structs for each
 * distinct set of GROUP BY column values.	We compute the hash key from
 * the GROUP BY columns.
 */
typedef struct AggHashEntryData *AggHashEntry;

typedef struct AggHashEntryData
{
	TupleHashEntryData shared;	/* common header for hash table entries */
	/* per-aggregate transition status array - must be last! */
	AggStatePerGroupData pergroup[1];	/* VARIABLE LENGTH ARRAY */
} AggHashEntryData;				/* VARIABLE LENGTH STRUCT */

static void advance_transition_function(AggState *aggstate,
							AggStatePerAgg peraggstate,
							AggStatePerGroup pergroupstate,
							FunctionCallInfoData *fcinfo);
static void process_sorted_aggregate(AggState *aggstate,
						 AggStatePerAgg peraggstate,
						 AggStatePerGroup pergroupstate);
static void finalize_aggregate(AggState *aggstate,
				   AggStatePerAgg peraggstate,
				   AggStatePerGroup pergroupstate,
				   Datum *resultVal, bool *resultIsNull);
static Bitmapset *find_unaggregated_cols(AggState *aggstate);
static bool find_unaggregated_cols_walker(Node *node, Bitmapset **colnos);
static TupleTableSlot *agg_retrieve_direct(AggState *aggstate);
static TupleTableSlot *agg_retrieve_hash_table(AggState *aggstate);
static void ExecAggExplainEnd(PlanState *planstate, struct StringInfoData *buf);

/*
 * Initialize all aggregates for a new group of input values.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
void
initialize_aggregates(AggState *aggstate,
					  AggStatePerAgg peragg,
					  AggStatePerGroup pergroup)
{
	int			aggno;

	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &peragg[aggno];
		AggStatePerGroup pergroupstate = &pergroup[aggno];
		Aggref	   *aggref = peraggstate->aggref;

		/*
		 * Start a fresh sort operation for each DISTINCT aggregate.
		 */
		if (aggref->aggdistinct)
		{
			/*
			 * In case of rescan, maybe there could be an uncompleted sort
			 * operation?  Clean it up if so.
			 */
			if (peraggstate->sortstate)
				tuplesort_end(peraggstate->sortstate);

			peraggstate->sortstate =
				tuplesort_begin_datum(peraggstate->inputType,
									  peraggstate->sortOperator,
									  work_mem, false);

            /* 
             * CDB: If EXPLAIN ANALYZE, let all of our tuplesort operations
             * share our Instrumentation object and message buffer.
             */
            if (aggstate->ss.ps.instrument)
                tuplesort_set_instrument(peraggstate->sortstate,
                                         aggstate->ss.ps.instrument,
                                         aggstate->ss.ps.cdbexplainbuf);

            /* CDB: Set enhanced sort options. */
			{
				int64 		limit = 0;
				int64 		offset = 0;
				int 		unique = gp_enable_sort_distinct ? 1 : 0;
				int 		sort_flags = gp_sort_flags; /* get the guc */
				int         maxdistinct = gp_sort_max_distinct; /* get guc */

				cdb_tuplesort_init(peraggstate->sortstate, 
								   offset, limit, unique, 
								   sort_flags, maxdistinct);
			}
		}

		/*
		 * If we are reinitializing after a group boundary, we have to free
		 * any prior transValue to avoid memory leakage.  We must check not
		 * only the isnull flag but whether the pointer is NULL; since
		 * pergroupstate is initialized with palloc0, the initial condition
		 * has isnull = 0 and null pointer.
		 */
		if (!peraggstate->transtypeByVal &&
			!pergroupstate->transValueIsNull &&
			DatumGetPointer(pergroupstate->transValue) != NULL)
			pfree(DatumGetPointer(pergroupstate->transValue));

		/*
		 * (Re)set transValue to the initial value.
		 *
		 * Note that when the initial value is pass-by-ref, we must copy it
		 * (into the transcontext) since we will pfree the transValue later.
		 */
		if (peraggstate->initValueIsNull)
			pergroupstate->transValue = peraggstate->initValue;
		else
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(aggstate->transcontext);
			pergroupstate->transValue = datumCopy(peraggstate->initValue,
												  peraggstate->transtypeByVal,
												  peraggstate->transtypeLen);
			MemoryContextSwitchTo(oldContext);
		}
		pergroupstate->transValueIsNull = peraggstate->initValueIsNull;

		/*
		 * If the initial value for the transition state doesn't exist in the
		 * pg_aggregate table then we will let the first non-NULL value
		 * returned from the outer procNode become the initial value. (This is
		 * useful for aggregates like max() and min().) The noTransValue flag
		 * signals that we still need to do this.
		 */
		pergroupstate->noTransValue = peraggstate->initValueIsNull;
	}
}

/*
 * Given new input value(s), advance the transition function of an aggregate.
 *
 * The new values (and null flags) have been preloaded into argument positions
 * 1 and up in fcinfo, so that we needn't copy them again to pass to the
 * transition function.  No other fields of fcinfo are assumed valid.
 *
 * It doesn't matter which memory context this is called in.
 */
static void
advance_transition_function(AggState *aggstate,
							AggStatePerAgg peraggstate,
							AggStatePerGroup pergroupstate,
							FunctionCallInfoData *fcinfo)
{
	pergroupstate->transValue = 
		invoke_agg_trans_func(peraggstate->transfn, 
							  peraggstate->numArguments,
							  pergroupstate->transValue,
							  &(pergroupstate->noTransValue),
							  &(pergroupstate->transValueIsNull),
							  peraggstate->transtypeByVal,
							  peraggstate->transtypeLen,
							  fcinfo, (void *)aggstate, aggstate->transcontext, 
							  aggstate->tmpcontext->ecxt_per_tuple_memory);
}

Datum
invoke_agg_trans_func(FmgrInfo transfn, int numargs, Datum transValue,
					  bool *noTransvalue, bool *transValueIsNull,
					  bool transtypeByVal, int16 transtypeLen, 
					  FunctionCallInfoData *fcinfo, void *funcctx,
					  MemoryContext transcontext, MemoryContext tuplecontext)
{
	MemoryContext oldContext;
	Datum		newVal;
	int			i;

	if (transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		for (i = 1; i <= numargs; i++)
		{
			if (fcinfo->argnull[i])
				return transValue;
		}
		if (*noTransvalue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into transcontext if it is pass-by-ref.
			 * We do not need to pfree the old transValue, since it's NULL.
			 */
			oldContext = MemoryContextSwitchTo(transcontext);
			newVal = datumCopy(fcinfo->arg[1], transtypeByVal,
							   transtypeLen);
			*transValueIsNull = false;
			*noTransvalue = false;
			fcinfo->isnull = false;
			MemoryContextSwitchTo(oldContext);
			return newVal;
		}
		if (*transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			fcinfo->isnull = true;
			return transValue;
		}
	}

	/* We run the transition functions in per-input-tuple memory context */
	oldContext = MemoryContextSwitchTo(tuplecontext);

	/*
	 * OK to call the transition function
	 */
	InitFunctionCallInfoData(*fcinfo, &transfn,
							 numargs + 1,
							 (void *) funcctx, NULL);
	fcinfo->arg[0] = transValue;
	fcinfo->argnull[0] = *transValueIsNull;

	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into transcontext and
	 * pfree the prior transValue.	But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!transtypeByVal && 
		DatumGetPointer(newVal) != DatumGetPointer(transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(transcontext);
			newVal = datumCopy(newVal, transtypeByVal, transtypeLen);
		}
		if (!(*transValueIsNull))
			pfree(DatumGetPointer(transValue));
	}

	*transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
	return newVal;
}

/*
 * Advance all the aggregates for one input tuple.	The input tuple
 * has been stored in tmpcontext->ecxt_scantuple, so that it is accessible
 * to ExecEvalExpr.  pergroup is the array of per-group structs to use
 * (this might be in a hashtable entry).
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
void
advance_aggregates(AggState *aggstate, AggStatePerGroup pergroup)
{
	ExprContext *econtext = aggstate->tmpcontext;
	int			aggno;

	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &aggstate->peragg[aggno];
		AggStatePerGroup pergroupstate = &pergroup[aggno];
		AggrefExprState *aggrefstate = peraggstate->aggrefstate;
		Aggref	   *aggref = peraggstate->aggref;
		FunctionCallInfoData fcinfo;
		int			i;
		ListCell   *arg;
		MemoryContext oldContext;

		/* Switch memory context just once for all args */
		oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/* Evaluate inputs and save in fcinfo */
		/* We start from 1, since the 0th arg will be the transition value */
		i = 1;
		foreach(arg, aggrefstate->args)
		{
			ExprState  *argstate = (ExprState *) lfirst(arg);

			fcinfo.arg[i] = ExecEvalExpr(argstate, econtext,
										 fcinfo.argnull + i, NULL);
			i++;
		}

		/* Switch back */
		MemoryContextSwitchTo(oldContext);

		if (aggref->aggdistinct)
		{
			/* in DISTINCT mode, we may ignore nulls */
			/* XXX we assume there is only one input column */
			if (fcinfo.argnull[1])
				continue;
			tuplesort_putdatum(peraggstate->sortstate, fcinfo.arg[1],
							   fcinfo.argnull[1]);
		}
		else
		{
			advance_transition_function(aggstate, peraggstate, pergroupstate,
										&fcinfo);
		}
	}
}

/*
 * Run the transition function for a DISTINCT aggregate.  This is called
 * after we have completed entering all the input values into the sort
 * object.	We complete the sort, read out the values in sorted order,
 * and run the transition function on each non-duplicate value.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
process_sorted_aggregate(AggState *aggstate,
						 AggStatePerAgg peraggstate,
						 AggStatePerGroup pergroupstate)
{
	Datum		oldVal = (Datum) 0;
	bool		haveOldVal = false;
	MemoryContext workcontext = aggstate->tmpcontext->ecxt_per_tuple_memory;
	MemoryContext oldContext;
	Datum	   *newVal;
	bool	   *isNull;
	FunctionCallInfoData fcinfo;

	tuplesort_performsort(peraggstate->sortstate);

	newVal = fcinfo.arg + 1;
	isNull = fcinfo.argnull + 1;

	/*
	 * Note: if input type is pass-by-ref, the datums returned by the sort are
	 * freshly palloc'd in the per-query context, so we must be careful to
	 * pfree them when they are no longer needed.
	 */

	while (tuplesort_getdatum(peraggstate->sortstate, true,
							  newVal, isNull))
	{
		/*
		 * DISTINCT always suppresses nulls, per SQL spec, regardless of the
		 * transition function's strictness.
		 */
		if (*isNull)
			continue;

		/*
		 * Clear and select the working context for evaluation of the equality
		 * function and transition function.
		 */
		MemoryContextReset(workcontext);
		oldContext = MemoryContextSwitchTo(workcontext);

		if (haveOldVal &&
			DatumGetBool(FunctionCall2(&peraggstate->equalfn,
									   oldVal, *newVal)))
		{
			/* equal to prior, so forget this one */
			if (!peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(*newVal));
		}
		else
		{
			advance_transition_function(aggstate, peraggstate, pergroupstate,
										&fcinfo);
			/* forget the old value, if any */
			if (haveOldVal && !peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(oldVal));
			/* and remember the new one for subsequent equality checks */
			oldVal = *newVal;
			haveOldVal = true;
		}

		MemoryContextSwitchTo(oldContext);
	}

	if (haveOldVal && !peraggstate->inputtypeByVal)
		pfree(DatumGetPointer(oldVal));

	tuplesort_end(peraggstate->sortstate);
	peraggstate->sortstate = NULL;
}

/*
 * Compute the final value of one aggregate.
 *
 * The finalfunction will be run, and the result delivered, in the
 * output-tuple context; caller's CurrentMemoryContext does not matter.
 */
static void
finalize_aggregate(AggState *aggstate,
				   AggStatePerAgg peraggstate,
				   AggStatePerGroup pergroupstate,
				   Datum *resultVal, bool *resultIsNull)
{
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		FunctionCallInfoData fcinfo;

		InitFunctionCallInfoData(fcinfo, &(peraggstate->finalfn), 1,
								 (void *) aggstate, NULL);
		fcinfo.arg[0] = pergroupstate->transValue;
		fcinfo.argnull[0] = pergroupstate->transValueIsNull;
		if (fcinfo.flinfo->fn_strict && pergroupstate->transValueIsNull)
		{
			/* don't call a strict function with NULL inputs */
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			*resultVal = FunctionCallInvoke(&fcinfo);
			*resultIsNull = fcinfo.isnull;
		}
	}
	else
	{
		*resultVal = pergroupstate->transValue;
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	/*
	 * If result is pass-by-ref, make sure it is in the right context.
	 */
	if (!peraggstate->resulttypeByVal && !*resultIsNull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*resultVal)))
		*resultVal = datumCopy(*resultVal,
							   peraggstate->resulttypeByVal,
							   peraggstate->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}

/*
 * find_unaggregated_cols
 *	  Construct a bitmapset of the column numbers of un-aggregated Vars
 *	  appearing in our targetlist and qual (HAVING clause)
 */
static Bitmapset *
find_unaggregated_cols(AggState *aggstate)
{
	Agg		   *node = (Agg *) aggstate->ss.ps.plan;
	Bitmapset *colnos;

	colnos = NULL;
	(void) find_unaggregated_cols_walker((Node *) node->plan.targetlist,
										 &colnos);
	(void) find_unaggregated_cols_walker((Node *) node->plan.qual,
										 &colnos);
	return colnos;
}

static bool
find_unaggregated_cols_walker(Node *node, Bitmapset **colnos)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* setrefs.c should have set the varno to 0 */
		Assert(var->varno == 0);
		Assert(var->varlevelsup == 0);
		*colnos = bms_add_member(*colnos, var->varattno);
		return false;
	}
	if (IsA(node, Aggref))		/* do not descend into aggregate exprs */
		return false;
	return expression_tree_walker(node, find_unaggregated_cols_walker,
								  (void *) colnos);
}

/* 
 * Create a list of the tuple columns that actually need to be stored
 * in hashtable entries.  The incoming tuples from the child plan node
 * will contain grouping columns, other columns referenced in our
 * targetlist and qual, columns used to compute the aggregate functions,
 * and perhaps just junk columns we don't use at all.  Only columns of the
 * first two types need to be stored in the hashtable, and getting rid of
 * the others can make the table entries significantly smaller.  To avoid
 * messing up Var numbering, we keep the same tuple descriptor for
 * hashtable entries as the incoming tuples have, but set unwanted columns
 * to NULL in the tuples that go into the table.
 *
 * To eliminate duplicates, we build a bitmapset of the needed columns,
 * then convert it to an integer list (cheaper to scan at runtime).
 * The list is in decreasing order so that the first entry is the largest;
 * lookup_hash_entry depends on this to use slot_getsomeattrs correctly.
 *
 * Note: at present, searching the tlist/qual is not really necessary
 * since the parser should disallow any unaggregated references to
 * ungrouped columns.  However, the search will be needed when we add
 * support for SQL99 semantics that allow use of "functionally dependent"
 * columns that haven't been explicitly grouped by.
 */
List *
get_agg_hash_collist(AggState *aggstate)
{
	Agg		   *node = (Agg *) aggstate->ss.ps.plan;
	Bitmapset  *colnos;
	List	   *collist;
	int			i;

	/* Find Vars that will be needed in tlist and qual */
	colnos = find_unaggregated_cols(aggstate);
	/* Add in all the grouping columns */
	for (i = 0; i < node->numCols; i++)
		colnos = bms_add_member(colnos, node->grpColIdx[i]);
	/* Convert to list, using lcons so largest element ends up first */
	collist = NIL;
	while ((i = bms_first_member(colnos)) >= 0)
		collist = lcons_int(i, collist);
	return collist;
}


/*
 * Estimate per-hash-table-entry overhead for the planner.
 *
 * Note that the estimate does not include space for pass-by-reference
 * transition data values, nor for the representative tuple of each group.
 */
Size
hash_agg_entry_size(int numAggs)
{
	Size		entrysize;

	/* This must match build_hash_table */
	entrysize = sizeof(AggHashEntryData) +
		(numAggs - 1) *sizeof(AggStatePerGroupData);
	entrysize = MAXALIGN(entrysize);
	/* Account for hashtable overhead (assuming fill factor = 1) */
	entrysize += 3 * sizeof(void *);
	return entrysize;
}
#if 0 /* Not used by MPP */
/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static AggHashEntry
lookup_hash_entry(AggState *aggstate, TupleTableSlot *inputslot)
{
	TupleTableSlot *hashslot = aggstate->hashslot;
	ListCell   *l;
	AggHashEntry entry;
	bool		isnew;

	/* if first time through, initialize hashslot by cloning input slot */
	if (hashslot->tts_tupleDescriptor == NULL)
	{
		ExecSetSlotDescriptor(hashslot, inputslot->tts_tupleDescriptor);
		/* Make sure all unused columns are NULLs */
		ExecStoreAllNullTuple(hashslot);
	}

	/* transfer just the needed columns into hashslot */
	slot_getsomeattrs(inputslot, linitial_int(aggstate->hash_needed));
	foreach(l, aggstate->hash_needed)
	{
		int			varNumber = lfirst_int(l) - 1;

		hashslot->tts_values[varNumber] = inputslot->tts_values[varNumber];
		hashslot->tts_isnull[varNumber] = inputslot->tts_isnull[varNumber];
	}

	/* find or create the hashtable entry using the filtered tuple */
	entry = (AggHashEntry) LookupTupleHashEntry(aggstate->hashtable,
												hashslot,
												&isnew);

	if (isnew)
	{
		/* initialize aggregates for new tuple group */
		initialize_aggregates(aggstate, aggstate->peragg, entry->pergroup);
	}

	return entry;
}
#endif /* Not used by MPP */

/*
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each aggregate function use (Aggref
 *	  node) appearing in the targetlist or qual of the node.  The number
 *	  of tuples to aggregate over depends on whether grouped or plain
 *	  aggregation is selected.	In grouped aggregation, we produce a result
 *	  row for each group; in plain aggregation there's a single result row
 *	  for the whole query.	In either case, the value of each aggregate is
 *	  stored in the expression context to be used when ExecProject evaluates
 *	  the result tuple.
 */
TupleTableSlot *
ExecAgg(AggState *node)
{
	if (node->agg_done && node->num_repeats == 0)
		return NULL;

	if (((Agg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		TupleTableSlot *tuple = NULL;

		/*
		 * ExecAgg processing for hashed aggregation -- returns the
		 * next result tuple or NULL. When returning NULL also sets
		 * aggstate to prevent future calls.
		 */
	
		if (node->hhashtable == NULL)
		{
			node->hhashtable = create_agg_hash_table(node);
			agg_hash_initial_pass(node);
			node->hhashtable->state = HASHAGG_BETWEEN_PASSES;
		}
	
		for (;;)
		{
			tuple = agg_retrieve_hash_table(node);
			node->agg_done = false; /* Not done 'til batches used up. */
		
			if (tuple != NULL)
				return tuple;
		
			switch (node->hhashtable->state)
			{
				case HASHAGG_BETWEEN_PASSES:
					if (!agg_hash_next_pass(node))
					{
						node->hhashtable->state = HASHAGG_END_OF_PASSES;
						continue;
					}
					node->hhashtable->state = HASHAGG_IN_A_PASS;
					continue;
		
				case HASHAGG_IN_A_PASS:
					if (!agg_hash_continue_pass(node))
						node->hhashtable->state = HASHAGG_BETWEEN_PASSES;
					continue;

				case HASHAGG_END_OF_PASSES:
					node->agg_done = true;
					return NULL;
			
				case HASHAGG_BEFORE_FIRST_PASS:
					elog(ERROR,"hybrid hash aggregation sequencing error");
			}
		}
	}
#if 1
	else
		return agg_retrieve_direct(node);
#else
	else {
		/* debugging */
		TupleTableSlot *out = agg_retrieve_direct(node);
		Agg *agg_node = (Agg *) ((AggState *)node)->ss.ps.plan;
		if (!TupIsNull(out))
		elog(LOG, "nodeAgg: numNullCols=%d, inputHasGrouping=%d, output tuple: %s",
			 agg_node->numNullCols, agg_node->inputHasGrouping, tup2str(out));
		return out;
	}
#endif
}

/*
 * ExecAgg for non-hashed case
 */
static TupleTableSlot *
agg_retrieve_direct(AggState *aggstate)
{
	Agg		   *node = (Agg *) aggstate->ss.ps.plan;
	PlanState  *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	AggStatePerGroup perpassthru;
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;
	int			aggno;

	HeapTuple   copytuple = NULL;
	bool        passthru_ready = false;
	bool        has_partial_agg = aggstate->has_partial_agg;
	TupleTableSlot *modified_slot;

	uint64      input_grouping = node->inputGrouping;
	bool        input_has_grouping = node->inputHasGrouping;
	bool        is_final_rollup_agg =
		(input_has_grouping && node->numNullCols == 0);
	bool        is_middle_rollup_agg =
		(input_has_grouping && node->numNullCols > 0);

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(aggstate);
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->ss.ps.ps_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;
	projInfo = aggstate->ss.ps.ps_ProjInfo;
	peragg = aggstate->peragg;
	pergroup = aggstate->pergroup;
	perpassthru = aggstate->perpassthru;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	/*
	 * If the previous aggregated result needs to be outputed
	 * several times, we simple return the previous output tuple.
	 */
	if (aggstate->num_repeats > 0)
	{
		/*
		 * XXX We do the projection here again. It may be cheaper to simply
		 * update the GROUP_ID column, but it seems that it is so much easier
		 * to just do the projection again.
		 */
		econtext->ecxt_scantuple = aggstate->prev_aggslot;
		econtext->group_id = aggstate->num_repeats;
		econtext->grouping =
			get_grouping(econtext->ecxt_scantuple,
						 node->grpColIdx[node->numCols-1]);

		while (econtext->group_id > 0)
		{
			econtext->group_id--;
			aggstate->num_repeats--;

			if (ExecQual(aggstate->ss.ps.qual, econtext, false))
				return ExecProject(projInfo, NULL);
		}
	}

	/* Do not need to release the space since this is just a pointer
	 * to a slot, which will be released somewhere else.
	 */
	aggstate->prev_aggslot = NULL;

	if (aggstate->agg_done)
		return NULL;
	
	/*
	 * We loop retrieving tuples until we find one that matches
	 * aggstate->ss.ps.qual, or find a pass-thru tuple when this Agg
	 * node is part of a ROLLUP query.
	 */
	while (!aggstate->agg_done)
	{
		/* Indicate if an input tuple may need to be pass-thru. */
		bool maybe_passthru = false;

		/*
		 * grp_firstTuple represents three types of input tuples:
		 *   (1) an input tuple that is the first tuple in a new
		 *       group and needs to be aggregated.
		 *   (2) an input tuple that only needs to be pass-thru.
		 *   (3) an input tuple that needs to be aggregated and
		 *       be pass-thru.
		 *
		 * When this value is set here, it means that we have previously
		 * read a tuple that crossed the grouping boundary, and this is that
		 * tuple. It could be any of the above three types.
		 *
		 * When this value is not set, there are two cases:
		 *  (1) If aggstate->has_parital_agg is false, then the first tuple
		 *      in a new group is not read.
		 *  (2) If aggstate->has_parital_agg is true, then the first tuple
		 *      in a new group has been read and aggregated into the result.
		 */
		if (aggstate->grp_firstTuple != NULL)
			maybe_passthru = true;

		/*
		 * If we don't already have the first tuple of the new group,
		 * fetch it from the outer plan.
		 */
		if (!aggstate->has_partial_agg && aggstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan);
			if (!TupIsNull(outerslot))
			{
				/*
				 * Make a copy of the first tuple; we will use this for
				 * comparison (in group mode) and for projection.
				 */
				aggstate->grp_firstTuple = ExecCopySlotTuple(outerslot);
			}
			else
			{
				/* outer plan produced no tuples at all */
				aggstate->agg_done = true;
				/* if we are grouping, we should produce no tuples too */
				if (node->aggstrategy != AGG_PLAIN)
					return NULL;
			}

#if 0
			if (!TupIsNull(outerslot))
				elog(LOG, "nodeAgg: numNullCols=%d, inputHasGrouping=%d, input tuple: %s",
					 node->numNullCols, node->inputHasGrouping, tup2str(outerslot));
#endif
		}
		
		if (!aggstate->has_partial_agg)
		{
			/*
			 * Clear the per-output-tuple context for each group
			 */
			ResetExprContext(econtext);

			/*
			 * Initialize working state for a new input tuple group
			 */
			initialize_aggregates(aggstate, peragg, pergroup);
		}

		/* Process the remaining tuples in the new group. */
		if (aggstate->has_partial_agg || aggstate->grp_firstTuple != NULL)
		{
			int outer_grouping = input_grouping;

			if (!aggstate->has_partial_agg)
			{
				/*
				 * Store the copied first input tuple in the tuple table slot
				 * reserved for it. The tuple will be deleted when it is cleared
				 * from the slot.
				 */
				ExecStoreTuple(aggstate->grp_firstTuple,
							   firstSlot,
							   InvalidBuffer,
							   true);
				aggstate->grp_firstTuple = NULL;        /* don't keep two pointers */

				/*
				 * Determine if this first tuple is a simple pass-thru tuple,
				 * or need to be aggregated.
				 */
				outer_grouping =
					tuple_grouping(firstSlot, node->numCols,
								   input_grouping, input_has_grouping,
								   node->numCols > 0 ? node->grpColIdx[node->numCols-1]: 0);

				Assert(outer_grouping <= input_grouping);
			}

			else if (aggstate->grp_firstTuple == NULL)
			{
				/*
				 * Here, in the previous call, we have read the first tuple in the new
				 * group and added it to the aggregate result. However, at this point,
				 * the firstSlot stores the original input tuple since this tuple is
				 * also a pass-thru tuple. We want firstSlot to be the modified tuple for
				 * comparison later. So we copy aggstate->modified_slot to firstSlot.
				 */
				if (node->numNullCols > 0)
					firstSlot = ExecCopySlot(firstSlot, aggstate->modified_slot);
			}
			
			if (outer_grouping < input_grouping)
			{
				/* This tuple is a pass-thru tuple. */
				passthru_ready = true;
				outerslot = firstSlot;
			}
			
			else
			{
				/* This tuple needs to be aggregated and may need to be passthru. */
				if (is_middle_rollup_agg &&
					!aggstate->has_partial_agg)
					maybe_passthru = true;
				
				/*
				 * For a ROLLUP query, we may need to modify the input tuple
				 * to match with the current rollup level. We initialize those
				 * arrays that are needed for this process.
				 */
				if (aggstate->num_attrs == 0 && node->numNullCols > 0)
				{
					int num_grpattrs = node->numCols;
					int attno;

					/* Ignore Grouping column */
					if (is_middle_rollup_agg)
						num_grpattrs--;
					aggstate->num_attrs =
						outerslot->tts_tupleDescriptor->natts;
					aggstate->replValues =
						palloc0(aggstate->num_attrs * sizeof(Datum));
					aggstate->replIsnull = palloc0(aggstate->num_attrs * sizeof(bool));
					aggstate->doReplace = palloc0(aggstate->num_attrs * sizeof(bool));

					Assert(num_grpattrs >= node->numNullCols);

					for (attno=num_grpattrs - node->numNullCols; attno<num_grpattrs; attno++)
					{
						aggstate->replIsnull[node->grpColIdx[attno] - 1] = true;
						aggstate->doReplace[node->grpColIdx[attno] - 1] = true;
					}
				}

				if (!aggstate->has_partial_agg)
				{
					/*
					 * Modify the input tuple when this node requires some grouping
					 * columns to be set to NULL.
					 */
					if (node->numNullCols > 0)
					{
						copytuple = ExecCopySlotTuple(firstSlot);
						copytuple =
							heap_modify_tuple(copytuple,
											  firstSlot->tts_tupleDescriptor,
											  aggstate->replValues,
											  aggstate->replIsnull,
											  aggstate->doReplace);
						ExecStoreTuple(copytuple,
									   aggstate->modified_slot,
									   InvalidBuffer,
									   true);
						/* set up for first advance aggregates call */
						tmpcontext->ecxt_scantuple = aggstate->modified_slot;
					}

					else
					{
						/* set up for first advance aggregates call */
						tmpcontext->ecxt_scantuple = firstSlot;
					}
				}
				
				/*
				 * Process each outer-plan tuple, and then fetch the next one,
				 * until we exhause the outer plan or cross a group boundary.
				 */
				for (;;)
				{
					if (!aggstate->has_partial_agg)
					{
						has_partial_agg = true;
						advance_aggregates(aggstate, pergroup);
					}

					/* Reset per-input-tuple context after each tuple */
					ResetExprContext(tmpcontext);

					if (is_middle_rollup_agg &&
						maybe_passthru)
					{
						outerslot = firstSlot;
						passthru_ready = true;
						aggstate->has_partial_agg = has_partial_agg;
						break;
					}

					if (!aggstate->has_partial_agg && node->numNullCols > 0)
						firstSlot = ExecCopySlot(firstSlot, aggstate->modified_slot);
					
					outerslot = ExecProcNode(outerPlan);
					if (TupIsNull(outerslot))
					{
						/* no more outer-plan tuples avaiable */
						aggstate->agg_done = true;
						break;
					}
					/* set up for next advance aggregates call */
					tmpcontext->ecxt_scantuple = outerslot;

#if 0
					elog(LOG, "nodeAgg: numNullCols=%d, inputHasGrouping=%d, input tuple: %s",
						 node->numNullCols, node->inputHasGrouping, tup2str(outerslot));
#endif

					/*
					 * If we are grouping, check whether we've crossed a group
					 * boundary.
					 */
					if (node->aggstrategy == AGG_SORTED)
					{
						bool tuple_match;
						int match_numcols;

						int outer_grouping =
							tuple_grouping(outerslot, node->numCols,
										   input_grouping, input_has_grouping,
										   node->numCols > 0 ? node->grpColIdx[node->numCols-1]: 0);

						Assert(outer_grouping <= input_grouping);

						/*
						 * We modify the tuple before checking if we've crossed
						 * a group boundary.
						 */
						if (node->numNullCols > 0)
						{
							copytuple = ExecCopySlotTuple(outerslot);
							copytuple =
								heap_modify_tuple(copytuple,
												  firstSlot->tts_tupleDescriptor,
												  aggstate->replValues,
												  aggstate->replIsnull,
												  aggstate->doReplace);
							ExecStoreTuple(copytuple,
										   aggstate->modified_slot,
										   InvalidBuffer,
										   true);
							modified_slot = aggstate->modified_slot;
						}
						else
						{
							modified_slot = outerslot;
						}
						
						if (outer_grouping == input_grouping)
							match_numcols = node->numCols;
						else
							match_numcols = node->numCols - 1; /* ignore Grouping column */
						
						tuple_match = execTuplesMatch(firstSlot, modified_slot,
													  match_numcols, node->grpColIdx,
													  aggstate->eqfunctions,
													  tmpcontext->ecxt_per_tuple_memory);

						if (!tuple_match)
						{
							/* We've crossed a group boundary. */
							/* Make a copy of the first tuple for the new group. */
							aggstate->grp_firstTuple = ExecCopySlotTuple(outerslot);
							aggstate->has_partial_agg = false;
							break;
						}

						if (is_middle_rollup_agg || outer_grouping < input_grouping)
						{
							/* We have a pass-through tuple.
							 * We will advance the aggregate result before passing it
							 * thru.
							 */
							if (aggstate->has_partial_agg &&
								outer_grouping == input_grouping)
							{
								has_partial_agg = true;
								tmpcontext->ecxt_scantuple = modified_slot;
								advance_aggregates(aggstate, pergroup);
							}

							if (aggstate->has_partial_agg &&
								outer_grouping < input_grouping)
							{
								/*
								 * When the tuple we just read is a pass-thru tuple,
								 * we found a match without the GROUPING column.
								 * However, the original first tuple, stored in firstSlot,
								 * in a new group may have different value for GROUPING with
								 * aggstate->modified_slot. We want to copy firstSlot into
								 * aggstate->modified_slot before our next call.
								 */
								aggstate->modified_slot =
									ExecCopySlot(aggstate->modified_slot, firstSlot);
							}
							
							passthru_ready = true;
							aggstate->has_partial_agg = has_partial_agg;
							break;
						}
					}
					
				}
				
			}
		}

		/*
		 * We found a pass-thru tuple. When this tuple appears in the middle
		 * Agg node in a ROLLUP, we can simply return outerslot. If this appears
		 * in the final Agg node in a ROLLUP, we need to finalize its
		 * aggregate value.
		 *
		 * If this pass-thru tuple needs to be aggregated into the result, at this
		 * point, it is done.
		 */
		if (passthru_ready)
		{
			if (is_middle_rollup_agg)
				return outerslot;

			/*
			 * For the top-level of a rollup, we need to finalize
			 * the aggregate value. First, we reset the context,
			 * and prepare the aggregate value.
			 */
			ResetExprContext(econtext);
			initialize_aggregates(aggstate, peragg, perpassthru);

			/* finalize the pass-through tuple */
			ResetExprContext(tmpcontext);
			tmpcontext->ecxt_scantuple = outerslot;

			advance_aggregates(aggstate, perpassthru);
		}
		

		/*
		 * Done scanning input tuple group. Finalize each aggregate
		 * calculation, and stash results in the per-output-tuple context.
		 */
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];
			AggStatePerGroup pergroupstate;

			if (!passthru_ready)
				pergroupstate = &pergroup[aggno];
			else
				pergroupstate = &perpassthru[aggno];

			if (peraggstate->aggref->aggdistinct)
				process_sorted_aggregate(aggstate, peraggstate, pergroupstate);

			finalize_aggregate(aggstate, peraggstate, pergroupstate,
							   &aggvalues[aggno], &aggnulls[aggno]);
		}

		/*
		 * Use the representative input tuple for any references to
		 * non-aggregated input columns in the qual and tlist.	(If we are not
		 * grouping, and there are no input rows at all, we will come here
		 * with an empty firstSlot ... but if not grouping, there can't be any
		 * references to non-aggregated input columns, so no problem.)
		 */
		if (passthru_ready)
			econtext->ecxt_scantuple = outerslot;
		else
			econtext->ecxt_scantuple = firstSlot;

		/*
		 * We obtain GROUP_ID from the input tuples when this is
		 * the middle Agg or final Agg in a ROLLUP.
		 */
		if (is_final_rollup_agg ||
			(passthru_ready && is_middle_rollup_agg))
		{
			uint32 gid;
			gid = get_group_id(econtext->ecxt_scantuple);
			econtext->group_id = gid;
		}
		else
			econtext->group_id = node->rollupGSTimes;

		/* Set GROUPING value */
		if (is_final_rollup_agg ||
			(passthru_ready && is_middle_rollup_agg))
			econtext->grouping =
				get_grouping(econtext->ecxt_scantuple,
							 node->grpColIdx[node->numCols-1]);
		else
			econtext->grouping = node->grouping;

		/*
		 * Since one tuple may be outputted several times with different
		 * GROUP_ID values, and when the GROUP_ID() is appeared in
		 * the HAVING clause, we want to loop here to find the first
		 * tuple that satisfies the qual.
		 */
		do
		{
			if (peragg->aggref->aggstage == AGGSTAGE_FINAL ||
				peragg->aggref->aggstage == AGGSTAGE_NORMAL)
			{
				aggstate->prev_aggslot = econtext->ecxt_scantuple;
				aggstate->num_repeats = econtext->group_id - 1;
				if (econtext->group_id > 0)
					econtext->group_id--;
			}

			/*
			 * Check the qual (HAVING clause); if the group does not match, ignore
			 * it and loop back to try to process another group.
			 */
			if (ExecQual(aggstate->ss.ps.qual, econtext, false))
			{
				/*
				 * Form and return a projection tuple using the aggregate results
				 * and the representative input tuple.	Note we do not support
				 * aggregates returning sets ...
				 */
				return ExecProject(projInfo, NULL);
			}
		} while (econtext->group_id > 0);
	}

	/* No more groups */
	return NULL;
}

/*
 * ExecAgg for hashed case: retrieve groups from hash table
 */
static TupleTableSlot *
agg_retrieve_hash_table(AggState *aggstate)
{
	ExprContext *econtext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	TupleTableSlot *firstSlot;
	int			aggno;

	/*
	 * get state info from node
	 */
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->ss.ps.ps_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	projInfo = aggstate->ss.ps.ps_ProjInfo;
	peragg = aggstate->peragg;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one satisfying
	 * aggstate->ss.ps.qual
	 */
	while (!aggstate->agg_done)
	{
		HashAggEntry *entry = agg_hash_iter(aggstate->hhashtable);
			
		if (entry == NULL)
		{
			aggstate->agg_done = TRUE;
			return NULL;
		}
			
		ResetExprContext(econtext);

		/*
		 * Store the copied first input tuple in the tuple table slot reserved
		 * for it, so that it can be used in ExecProject.
		 */
		
		/* Retain "PostgreSQL way" as comment since we want to adopt the
		 * MinimalTuple approach eventually
		 
		ExecStoreMinimalTuple(entry->shared.firstTuple,
							  firstSlot,
							  false);
		 *
		 */
		ExecStoreTuple(entry->heaptuple, firstSlot, InvalidBuffer, false);

		pergroup = entry->pergroup;

		/*
		 * Finalize each aggregate calculation, and stash results in the
		 * per-output-tuple context.
		 */
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];
			AggStatePerGroup pergroupstate = &pergroup[aggno];

			Assert(!peraggstate->aggref->aggdistinct);
			finalize_aggregate(aggstate, peraggstate, pergroupstate,
							   &aggvalues[aggno], &aggnulls[aggno]);
		}

		/*
		 * Use the representative input tuple for any references to
		 * non-aggregated input columns in the qual and tlist.
		 */
		econtext->ecxt_scantuple = firstSlot;

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and loop back to try to process another group.
		 */
		if (ExecQual(aggstate->ss.ps.qual, econtext, false))
		{
			/*
			 * Form and return a projection tuple using the aggregate results
			 * and the representative input tuple.	Note we do not support
			 * aggregates returning sets ...
			 */
			return ExecProject(projInfo, NULL);
		}
	}

	/* No more groups */
	return NULL;
}

/* -----------------
 * ExecInitAgg
 *
 *	Creates the run-time information for the agg node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
AggState *
ExecInitAgg(Agg *node, EState *estate, int eflags)
{
	AggState   *aggstate;
	AggStatePerAgg peragg;
	Plan	   *outerPlan;
	ExprContext *econtext;
	int			numaggs,
				aggno;
	ListCell   *l;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	aggstate = makeNode(AggState);
	aggstate->ss.ps.plan = (Plan *) node;
	aggstate->ss.ps.state = estate;

	aggstate->aggs = NIL;
	aggstate->numaggs = 0;
	aggstate->eqfunctions = NULL;
	aggstate->hashfunctions = NULL;
	aggstate->peragg = NULL;
	aggstate->agg_done = false;
	aggstate->has_partial_agg = false;
	aggstate->pergroup = NULL;
	aggstate->grp_firstTuple = NULL;
	aggstate->hashtable = NULL;

	/*
	 * Create expression contexts.	We need two, one for per-input-tuple
	 * processing and one for per-output-tuple processing.	We cheat a little
	 * by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, &aggstate->ss.ps);
	aggstate->tmpcontext = aggstate->ss.ps.ps_ExprContext;
	ExecAssignExprContext(estate, &aggstate->ss.ps);

	/*
	 * We also need a long-lived memory context for holding hashtable data
	 * structures and transition values.  NOTE: the details of what is stored
	 * in aggcontext and what is stored in the regular per-query memory
	 * context are driven by a simple decision: we want to reset the
	 * aggcontext in ExecReScanAgg to recover no-longer-wanted space.
	 */
	aggstate->aggcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AggContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	if ( node->aggstrategy == AGG_HASHED )
	{
		aggstate->transcontext =
			AllocSetContextCreate(aggstate->aggcontext,
								  "TransContext",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
	}
	else
		aggstate->transcontext = aggstate->aggcontext;

#define AGG_NSLOTS 3

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &aggstate->ss);
	ExecInitResultTupleSlot(estate, &aggstate->ss.ps);
	aggstate->hashslot = ExecInitExtraTupleSlot(estate);
	/*
	 * initialize child expressions
	 *
	 * Note: ExecInitExpr finds Aggrefs for us, and also checks that no aggs
	 * contain other agg calls in their arguments.	This would make no sense
	 * under SQL semantics anyway (and it's forbidden by the spec). Because
	 * that is true, we don't need to worry about evaluating the aggs in any
	 * particular order.
	 */
	aggstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) aggstate);
	aggstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) aggstate);

    /* 
     * CDB: Offer extra info for EXPLAIN ANALYZE. 
     */
    if (estate->es_instrument)
    {
	    /* Allocate string buffer. */
        aggstate->ss.ps.cdbexplainbuf = makeStringInfo();        
        
        /* Request a callback at end of query. */
        aggstate->ss.ps.cdbexplainfun = ExecAggExplainEnd;
    }

	/*
	 * initialize child nodes
	 *
	 * If we are doing a hashed aggregation then the child plan does not need
	 * to handle REWIND efficiently; see ExecReScanAgg.
	 */
	if (node->aggstrategy == AGG_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlan = outerPlan(node);
	outerPlanState(aggstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * initialize source tuple type.
	 */
	ExecAssignScanTypeFromOuterPlan(&aggstate->ss);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&aggstate->ss.ps);
	ExecAssignProjectionInfo(&aggstate->ss.ps, NULL);

	/*
	 * get the count of aggregates in targetlist and quals
	 */
	numaggs = aggstate->numaggs;
	Assert(numaggs == list_length(aggstate->aggs));
	if (numaggs <= 0)
	{
		/*
		 * This is not an error condition: we might be using the Agg node just
		 * to do hash-based grouping.  Even in the regular case,
		 * constant-expression simplification could optimize away all of the
		 * Aggrefs in the targetlist and qual.	So keep going, but force local
		 * copy of numaggs positive so that palloc()s below don't choke.
		 */
		numaggs = 1;
	}

	/*
	 * If we are grouping, precompute fmgr lookup data for inner loop. We need
	 * both equality and hashing functions to do it by hashing, but only
	 * equality if not hashing.
	 */
	if (node->numCols > 0)
	{
		if (node->aggstrategy == AGG_HASHED)
			execTuplesHashPrepare(ExecGetScanType(&aggstate->ss),
								  node->numCols,
								  node->grpColIdx,
								  &aggstate->eqfunctions,
								  &aggstate->hashfunctions);
		else
			aggstate->eqfunctions =
				execTuplesMatchPrepare(ExecGetScanType(&aggstate->ss),
									   node->numCols,
									   node->grpColIdx);
	}

	/*
	 * Set up aggregate-result storage in the output expr context, and also
	 * allocate my private per-agg working storage
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc0(sizeof(Datum) * numaggs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numaggs);

	peragg = (AggStatePerAgg) palloc0(sizeof(AggStatePerAggData) * numaggs);
	aggstate->peragg = peragg;

	if (node->aggstrategy == AGG_HASHED)
	{
		aggstate->hash_needed = get_agg_hash_collist(aggstate);
		aggstate->table_filled = false;
	}
	else
	{
		AggStatePerGroup pergroup;

		pergroup = (AggStatePerGroup) palloc0(sizeof(AggStatePerGroupData) * numaggs);
		aggstate->pergroup = pergroup;
	}

	/*
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg data.  We also detect duplicate
	 * aggregates (for example, "SELECT sum(x) ... HAVING sum(x) > 0"). When
	 * duplicates are detected, we only make an AggStatePerAgg struct for the
	 * first one.  The clones are simply pointed at the same result entry by
	 * giving them duplicate aggno values.
	 */
	aggno = -1;
	foreach(l, aggstate->aggs)
	{
		AggrefExprState *aggrefstate = (AggrefExprState *) lfirst(l);
		Aggref	   *aggref = (Aggref *) aggrefstate->xprstate.expr;
		AggStatePerAgg peraggstate;
		Oid			inputTypes[FUNC_MAX_ARGS];
		int			numArguments;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		Oid			aggtranstype;
		AclResult	aclresult;
		Oid			transfn_oid = InvalidOid,
					finalfn_oid = InvalidOid;
		Expr	   *transfnexpr,
				   *finalfnexpr;
		Datum		textInitVal;
		int			i;
		ListCell   *lc;

		/* Planner should have assigned aggregate to correct level */
		Assert(aggref->agglevelsup == 0);

		/* Look for a previous duplicate aggregate */
		for (i = 0; i <= aggno; i++)
		{
			if (equal(aggref, peragg[i].aggref) &&
				!contain_volatile_functions((Node *) aggref))
				break;
		}
		if (i <= aggno)
		{
			/* Found a match to an existing entry, so just mark it */
			aggrefstate->aggno = i;
			continue;
		}

		/* Nope, so assign a new PerAgg record */
		peraggstate = &peragg[++aggno];

		/* Mark Aggref state node with assigned index in the result array */
		aggrefstate->aggno = aggno;

		/* Fill in the peraggstate data */
		peraggstate->aggrefstate = aggrefstate;
		peraggstate->aggref = aggref;
		numArguments = list_length(aggref->args);
		peraggstate->numArguments = numArguments;

		/*
		 * Get actual datatypes of the inputs.	These could be different from
		 * the agg's declared input types, when the agg accepts ANY, ANYARRAY
		 * or ANYELEMENT.
		 */
		i = 0;
		foreach(lc, aggref->args)
		{
			inputTypes[i++] = exprType((Node *) lfirst(lc));
		}

		aggTuple = SearchSysCache(AGGFNOID,
								  ObjectIdGetDatum(aggref->aggfnoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		/* Check permission to call aggregate function */
		aclresult = pg_proc_aclcheck(aggref->aggfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(aggref->aggfnoid));

		switch ( aggref->aggstage) /* MPP */
		{
		case AGGSTAGE_NORMAL: /* Single-stage aggregation */
			peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
			peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;
			break;
			
		case AGGSTAGE_PARTIAL: /* Two-stage aggregation or rollup - preliminary stage */
			/* the perliminary stage for two-stage aggregation */
			if (!node->inputHasGrouping || node->numNullCols == 0)
			{
				peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
				peraggstate->finalfn_oid = finalfn_oid = InvalidOid;
			}

			/* the preliminary stage for a rollup aggregation */
			else
			{
				peraggstate->transfn_oid = transfn_oid = aggform->aggprelimfn;
				peraggstate->finalfn_oid = finalfn_oid = InvalidOid;
			}
			break;

		case AGGSTAGE_FINAL: /* Two-stage aggregation - final stage */
			peraggstate->transfn_oid = transfn_oid = aggform->aggprelimfn;
			peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;
			break;
		}

		/* Check that aggregate owner has permission to call component fns */
		{
			HeapTuple	procTuple;
			Oid			aggOwner;

			procTuple = SearchSysCache(PROCOID,
									   ObjectIdGetDatum(aggref->aggfnoid),
									   0, 0, 0);
			if (!HeapTupleIsValid(procTuple))
				elog(ERROR, "cache lookup failed for function %u",
					 aggref->aggfnoid);
			aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
			ReleaseSysCache(procTuple);

			aclresult = pg_proc_aclcheck(transfn_oid, aggOwner,
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_PROC,
							   get_func_name(transfn_oid));
			if (OidIsValid(finalfn_oid))
			{
				aclresult = pg_proc_aclcheck(finalfn_oid, aggOwner,
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, ACL_KIND_PROC,
								   get_func_name(finalfn_oid));
			}
		}

		/* check if the transition type is polymorphic and if so resolve it */
		aggtranstype = resolve_polymorphic_transtype(aggform->aggtranstype, 
													 aggref->aggfnoid,
											   		 inputTypes);
											   
		/* build expression trees using actual argument & result types */
		build_aggregate_fnexprs(inputTypes,
								numArguments,
								aggtranstype,
								aggref->aggtype,
								transfn_oid,
								finalfn_oid,
								InvalidOid /* invtrans */,
								InvalidOid /* invprelim */,
								&transfnexpr,
								&finalfnexpr,
								NULL,
								NULL);

		fmgr_info(transfn_oid, &peraggstate->transfn);
		peraggstate->transfn.fn_expr = (Node *) transfnexpr;

		if (OidIsValid(finalfn_oid))
		{
			fmgr_info(finalfn_oid, &peraggstate->finalfn);
			peraggstate->finalfn.fn_expr = (Node *) finalfnexpr;
		}

		get_typlenbyval(aggref->aggtype,
						&peraggstate->resulttypeLen,
						&peraggstate->resulttypeByVal);
		get_typlenbyval(aggtranstype,
						&peraggstate->transtypeLen,
						&peraggstate->transtypeByVal);

		/*
		 * initval is potentially null, so don't try to access it as a struct
		 * field. Must do it the hard way with SysCacheGetAttr.
		 */
		textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
									  Anum_pg_aggregate_agginitval,
									  &peraggstate->initValueIsNull);

		if (peraggstate->initValueIsNull)
			peraggstate->initValue = (Datum) 0;
		else
			peraggstate->initValue = GetAggInitVal(textInitVal,
												   aggtranstype);

		/*
		 * If the transfn is strict and the initval is NULL, make sure input
		 * type and transtype are the same (or at least binary-compatible), so
		 * that it's OK to use the first input value as the initial
		 * transValue.	This should have been checked at agg definition time,
		 * but just in case...
		 */
		if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
		{
			if (numArguments < 1 ||
				!IsBinaryCoercible(inputTypes[0], aggtranstype))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate %u needs to have compatible input type and transition type",
								aggref->aggfnoid)));
		}

		if (aggref->aggdistinct)
		{
			Oid			eq_function;

			/* We don't implement DISTINCT aggs in the HASHED case */
			Assert(node->aggstrategy != AGG_HASHED);

			/*
			 * We don't currently implement DISTINCT aggs for aggs having more
			 * than one argument.  This isn't required for anything in the SQL
			 * spec, but really it ought to be implemented for
			 * feature-completeness.  FIXME someday.
			 */
			if (numArguments != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("DISTINCT is supported only for single-argument aggregates")));

			peraggstate->inputType = inputTypes[0];
			get_typlenbyval(inputTypes[0],
							&peraggstate->inputtypeLen,
							&peraggstate->inputtypeByVal);

			eq_function = equality_oper_funcid(inputTypes[0]);
			fmgr_info(eq_function, &(peraggstate->equalfn));
			peraggstate->sortOperator = ordering_oper_opid(inputTypes[0]);
			peraggstate->sortstate = NULL;
		}

		ReleaseSysCache(aggTuple);
	}

	/* Update numaggs to match number of unique aggregates found */
	aggstate->numaggs = aggno + 1;
	
	/* MPP */
	aggstate->hhashtable = NULL;

	/* ROLLUP */
	aggstate->perpassthru = NULL;

	if (node->inputHasGrouping)
	{
		AggStatePerGroup perpassthru;

		perpassthru = (AggStatePerGroup) palloc0(sizeof(AggStatePerGroupData) * numaggs);
		aggstate->perpassthru = perpassthru;

	}

	aggstate->num_attrs = 0;
	aggstate->modified_slot = 
		MakeSingleTupleTableSlot(aggstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);

	aggstate->prev_aggslot = NULL;
	aggstate->num_repeats = 0;

	return aggstate;
}

Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = DatumGetCString(DirectFunctionCall1(textout, textInitVal));
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

int
ExecCountSlotsAgg(Agg *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		AGG_NSLOTS;
}

static void
ExecEndAgg_internal(AggState *node)
{
	int			aggno;

	/* Make sure we have closed any open tuplesorts */
	for (aggno = 0; aggno < node->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &node->peragg[aggno];

		if (peraggstate->sortstate)
        {
            tuplesort_end(peraggstate->sortstate);
            peraggstate->sortstate = NULL;
        }
	}
}

void
ExecEndAgg(AggState *node)
{
	PlanState  *outerPlan;

    ExecEndAgg_internal(node);

	/* Clean up hybrid hash agg. */
	if ( ((Agg*)node->ss.ps.plan)->aggstrategy == AGG_HASHED)
		destroy_agg_hash_table(node);

	/*
	 * Free both the expr contexts.
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->tmpcontext;
	ExecFreeExprContext(&node->ss.ps);

	/* clean up tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	if (node->num_attrs > 0)
	{
		pfree(node->replValues);
		pfree(node->replIsnull);
		pfree(node->doReplace);
	}

	ExecClearTuple(node->modified_slot);

	MemoryContextDelete(node->aggcontext);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

void
ExecReScanAgg(AggState *node, ExprContext *exprCtxt)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			aggno;

	node->agg_done = false;

	if (((Agg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		/* MPP-1140 */
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("Illegal rescan of hashagg node: invalid plan."),
				 errhint("likely caused by bad NL-join, try setting enable_nestloop off, or disabling hashagg")));

		/*
		 * In the hashed case, if we haven't yet built the hash table then we
		 * can just return; nothing done yet, so nothing to undo. If subnode's
		 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
		 * else no reason to re-scan it at all.
		 */
		if (!node->table_filled)
			return;

		/*
		 * If we do have the hash table and the subplan does not have any
		 * parameter changes, then we can just rescan the existing hash table;
		 * no need to build it again.
		 */
		if (((PlanState *) node)->lefttree->chgParam == NULL)
		{
			ResetTupleHashIterator(node->hashtable, &node->hashiter);
			return;
		}
	}

	/* Make sure we have closed any open tuplesorts */
	for (aggno = 0; aggno < node->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &node->peragg[aggno];

		if (peraggstate->sortstate)
			tuplesort_end(peraggstate->sortstate);
		peraggstate->sortstate = NULL;
	}

	/* Release first tuple of group, if we have made a copy */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->modified_slot);
	node->prev_aggslot = NULL;
	node->num_repeats = 0;
	node->has_partial_agg = false;

	/* Forget current agg values */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numaggs);

	/* Release all temp storage */
	MemoryContextReset(node->aggcontext);

	if (((Agg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		destroy_agg_hash_table(node);
		node->table_filled = false;
	}
	else
	{
		/*
		 * Reset the per-group state (in particular, mark transvalues null)
		 */
		MemSet(node->pergroup, 0,
			   sizeof(AggStatePerGroupData) * node->numaggs);
	}

	if (((Agg *) node->ss.ps.plan)->inputHasGrouping)
		/*
		 * Reset the per-passthru state (in particular, mark transvalues null)
		 */
		MemSet(node->perpassthru, 0,
			   sizeof(AggStatePerGroupData) * node->numaggs);
		

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}


/*
 * ExecAggExplainEnd
 *      Called before ExecutorEnd to finish EXPLAIN ANALYZE reporting.
 */
void
ExecAggExplainEnd(PlanState *planstate, struct StringInfoData *buf)
{
    AggState   *aggstate = (AggState *)planstate;

    /* Report executor memory used by our memory context. */
    planstate->instrument->execmemused += 
        (double)MemoryContextGetPeakSpace(aggstate->aggcontext);

    /*
     * Some of the cleanup that ordinarily would occur during ExecEndAgg()
     * needs to be done earlier (i.e. now) in order to report statistics to 
     * EXPLAIN ANALYZE.  Note that ExecEndAgg_internal() may be called twice:
     * first here, and again when ExecEndAgg is called during ExecutorEnd().
     */
    ExecEndAgg_internal(aggstate);
}                               /* ExecAggExplainEnd */

/*
 * aggregate_dummy - dummy execution routine for aggregate functions
 *
 * This function is listed as the implementation (prosrc field) of pg_proc
 * entries for aggregate functions.  Its only purpose is to throw an error
 * if someone mistakenly executes such a function in the normal way.
 *
 * Perhaps someday we could assign real meaning to the prosrc field of
 * an aggregate?
 */
Datum
aggregate_dummy(PG_FUNCTION_ARGS)
{
	elog(ERROR, "aggregate function %u called as normal function",
		 fcinfo->flinfo->fn_oid);
	return (Datum) 0;			/* keep compiler quiet */
}

/* resolve actual type of transition state, if polymorphic */
Oid
resolve_polymorphic_transtype(Oid aggtranstype, Oid aggfnoid,
							  Oid *inputTypes)
{
	if (aggtranstype == ANYARRAYOID || aggtranstype == ANYELEMENTOID)
	{
		/* have to fetch the agg's declared input types... */
		Oid		   *declaredArgTypes;
		int			agg_nargs;

		(void) get_func_signature(aggfnoid, &declaredArgTypes, &agg_nargs);
		aggtranstype = enforce_generic_type_consistency(inputTypes,
														declaredArgTypes,
														agg_nargs,
														aggtranstype);
		pfree(declaredArgTypes);
	}
	return aggtranstype;
}

/*
 * tuple_grouping - return the GROUPING value for an input tuple.
 *
 * This is used for a ROLLUP.
 */
int64
tuple_grouping(TupleTableSlot *outerslot, int numGroupCols,
			   int input_grouping, bool input_has_grouping,
			   int grpingIdx)
{
	Datum grping_datum;
	uint64 grping;
	bool isnull;

	/* Simple return 0 if input_grouping is 0. */
	if (input_grouping == 0)
		return 0;

	grping_datum = slot_getattr(outerslot, grpingIdx, &isnull);

	Assert(!isnull && grping_datum != 0);

	grping = DatumGetInt64(grping_datum);

	return grping;
}

/*
 * Extract the value for GROUP_ID from the given tupleslot.
 *
 * GROUP_ID should always the last column in the given tupleslot.
 */
uint32
get_group_id(TupleTableSlot *tupleslot)
{
	Datum gid_datum;
	uint32 gid;
	bool isnull;
	int natts;

	/* GROUP_ID should be the last column in tupleslot. Obtain
	 * the number of attributes for the given tupleslot.
	 */
	natts = tupleslot->tts_tupleDescriptor->natts;
	if (tupleslot->tts_tuple != NULL &&
		HeapTupleHeaderGetNatts(tupleslot->tts_tuple->t_data) < natts)
		natts = HeapTupleHeaderGetNatts(tupleslot->tts_tuple->t_data);
	
	/* Obtain GROUP_ID value, which should always the be last column
	 * in the input tuple.
	 */
	gid_datum = slot_getattr(tupleslot, natts, &isnull);

	Assert(!isnull && gid_datum != 0);
	
	gid = DatumGetInt32(gid_datum);

	return gid;
}

uint64
get_grouping(TupleTableSlot *slot, int grping_attno)
{
	bool isnull;
	uint64 grouping;
	
	/* Obtain grouping from input */
	Datum grping_datum = slot_getattr(slot,
									  grping_attno,
									  &isnull);
	Assert(!isnull);
	grouping = DatumGetInt64(grping_datum);

	return grouping;
}
