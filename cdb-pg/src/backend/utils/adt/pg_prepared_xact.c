/*-------------------------------------------------------------------------
 *
 * pg_prepared_xact.c
 *		Set-returning function to view all transactions that are in
 *      prepared state.
 *
 * IDENTIFICATION
 *		$Header$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "access/twophase.h"
#include "utils/builtins.h"


/* Working status for pg_prepared_xact */
typedef struct
{
	PreparedXactArray *gxact_array;
	int currIdx;
} Working_State;

/*
 * pg_prepared_xact - produce a view with one row per prepared transaction
 */
Datum
pg_prepared_xact(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Working_State *mystatus;
	PreparedXactArray *gxact_array;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function
		 * calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match pg_prepared_xacts view in  system_views.sql */
		tupdesc = CreateTemplateTupleDesc(3, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "xid",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "gid",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "usesysid",
						   INT4OID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect all the locking information that we will format and
		 * send out as a result set.
		 */
		mystatus = (Working_State *) palloc(sizeof(Working_State));
		funcctx->user_fctx = (void *) mystatus;

		mystatus->gxact_array = GetPreparedTransactionList();
		mystatus->currIdx = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	mystatus = (Working_State *) funcctx->user_fctx;
	gxact_array = mystatus->gxact_array;

	while (gxact_array != NULL && mystatus->currIdx < gxact_array->nelements)
	{
		Datum		values[3];
		char		nulls[3];
		HeapTuple	tuple;
		Datum		result;
		GXACT *gxact = &gxact_array->elements[mystatus->currIdx++];

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));

		values[0] = TransactionIdGetDatum(gxact->xid);
		values[1] = DirectFunctionCall1(textin,
								 CStringGetDatum(gxact->gid));
		values[2] = AclIdGetDatum(gxact->owner);

		tuple = heap_formtuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}
