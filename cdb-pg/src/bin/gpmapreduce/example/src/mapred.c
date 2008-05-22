#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/*
 * Map function
 *   INPUT - 1 row of type "weblog" which contains attribute "hostname"
 *   OUTPUT <key varchar, value int4>   (hostname,1)
 */
PG_FUNCTION_INFO_V1(map);
Datum map(PG_FUNCTION_ARGS)
{
	HeapTupleHeader  input = PG_GETARG_HEAPTUPLEHEADER(0);
	TupleDesc        tupdesc;
	HeapTuple        heaptup;
	Datum            output[2];
	bool             isnull[2];
	Datum            result;

	/* 
	 * Since we are returning a complex type (aka a tuple) with two fields
	 * <key, value> we must check that we have a valid place to write the
	 * output.
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc)
		!= TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context " 
						"that cannot accept type record")));
	tupdesc = BlessTupleDesc(tupdesc);

	/* Extract the hostname from the input and emit a tuple <hostname,1> */
	isnull[0] = isnull[1] = false;
	output[0] = GetAttributeByName(input, "hostname", &isnull[0]);
	output[1] = DatumGetInt32(1);  
	heaptup = heap_form_tuple(tupdesc, output, isnull);
	result = HeapTupleGetDatum(heaptup);
	PG_RETURN_DATUM(result);
}

/*
 * Reduce function 
 *   Input <cumvalue int8, newvalue int4>
 *   Output <cumvalue int8>  (cumvalue + newvalue)
 */
PG_FUNCTION_INFO_V1(reduce);
Datum reduce(PG_FUNCTION_ARGS)
{
	int64 cumvalue = PG_GETARG_INT64(0);
	int32 newvalue = PG_GETARG_INT32(1);
	PG_RETURN_INT64(cumvalue + (int64) newvalue);
}

/*
 * Combine function 
 *   Input <cumvalue int64, cumvalue2 int64>
 *   Output <cumvalue int64>  (cumvalue + cumvalue2)
 */
PG_FUNCTION_INFO_V1(combine);
Datum combine(PG_FUNCTION_ARGS)
{
	int64 cumvalue = PG_GETARG_INT64(0);
	int64 cumvalue2 = PG_GETARG_INT64(1);
	PG_RETURN_INT64(cumvalue + cumvalue2);
}

