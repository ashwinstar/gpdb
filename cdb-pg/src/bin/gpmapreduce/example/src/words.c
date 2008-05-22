#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

/*
 * Map function
 *   INPUT - 1 row of text
 *   OUTPUT <key varchar, value int4>   (word,1)
 */
PG_FUNCTION_INFO_V1(words);
Datum words(PG_FUNCTION_ARGS)
{
	FuncCallContext *fctx;
	text            *in, *out;
	char            *start, *end;
	HeapTuple        heaptup;
	int32            tsize;
	Datum            output[2];
	bool             isnull[2];
	Datum            result;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldctx;

		fctx = SRF_FIRSTCALL_INIT();

		oldctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		/* Setup pointer to input data */
		in = PG_GETARG_TEXT_P(0);
		fctx->user_fctx = (void *) in;
		fctx->max_calls = VARSIZE(in) - VARHDRSZ;
		
		/* 
		 * Since we are returning a complex type (aka a tuple) with two fields
		 * <key, value> we must check that we have a valid place to write the
		 * output.
		 */
		if (get_call_result_type(fcinfo, NULL, &fctx->tuple_desc)
			!= TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context " 
							"that cannot accept type record")));
		fctx->tuple_desc = BlessTupleDesc(fctx->tuple_desc);
		
		MemoryContextSwitchTo(oldctx);
	}

	fctx = SRF_PERCALL_SETUP();
	
	/* This is very non-international, user code may want to be more complex */
	in = (text*) fctx->user_fctx;
	start = VARDATA(in) + VARSIZE(in) - fctx->max_calls - VARHDRSZ;
	while (fctx->max_calls > 0 && start[0] == ' ')
		start++, fctx->max_calls--;
	end = start;
	while (fctx->max_calls > 0 && end[0] != ' ')
		end++, fctx->max_calls--;
	if (end == start)
		SRF_RETURN_DONE(fctx);  /* ALL data scanned */
	out = (text *) palloc(end - start + VARHDRSZ);
	SET_VARSIZE(out, end - start + VARHDRSZ);
	memcpy(VARDATA(out), start, end - start);

	/* Build output tuple */
	isnull[0] = isnull[1] = false;
	output[0] = PointerGetDatum(out);
	output[1] = DatumGetInt32(1);
	heaptup = heap_form_tuple(fctx->tuple_desc, output, isnull);
	result = HeapTupleGetDatum(heaptup);
	SRF_RETURN_NEXT(fctx, result);
}
