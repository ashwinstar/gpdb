#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

/* Exported symbols */
PG_FUNCTION_INFO_V1(int4_pivot_accum);    /* returns int4[] */
PG_FUNCTION_INFO_V1(int8_pivot_accum);    /* returns int8[] */
PG_FUNCTION_INFO_V1(float8_pivot_accum);  /* returns float8[] */

Datum int4_pivot_accum(PG_FUNCTION_ARGS);
Datum int8_pivot_accum(PG_FUNCTION_ARGS);
Datum float8_pivot_accum(PG_FUNCTION_ARGS);

/* Internal static helper functions */
static Datum oid_pivot_accum(FunctionCallInfo fcinfo, Oid type);

/*
 * External facing wrapper functions to allow for proper type extraction
 */
Datum int4_pivot_accum(PG_FUNCTION_ARGS)
{
	return oid_pivot_accum(fcinfo, INT4OID);
}
Datum int8_pivot_accum(PG_FUNCTION_ARGS)
{
	return oid_pivot_accum(fcinfo, INT8OID);
}
Datum float8_pivot_accum(PG_FUNCTION_ARGS)
{
	return oid_pivot_accum(fcinfo, FLOAT8OID);
}

/* 
 * pivot_find() - Searchs an array of labels for a matching value.
 *
 * Returns: index of found value, or -1 if not found
 *
 * It may eventually do something smarter than a linear scan, but
 * for now this is sufficient given that we don't know anything about the
 * order of 'labels'.
 *
 */
static int pivot_find(ArrayType *labels, text *attr) 
{
	char    *labelsp;
	int      i, nelem, asize;
	int16    typlen;
	bool     typbyval;
	char     typalign;
	
	if (ARR_ELEMTYPE(labels) != TEXTOID)
		ereport(ERROR, 
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("pivot_accum: labels are not type text")));

	/* Text alignment properties */
	get_typlenbyvalalign(TEXTOID, &typlen, &typbyval, &typalign);
	
	/* Get the size of the input attribute, we'll use this for fast compares */
	asize = VARSIZE(attr);
	
	/* The labels array is an array of varying length text, scan it adding
	   the length of the previous entry until we are done or we have found
	   a match. */
	labelsp = (char *) ARR_DATA_PTR(labels);
	nelem = ARR_DIMS(labels)[0];
	for (i = 0; i < nelem; i++)
	{
		int lsize = VARSIZE(labelsp);
		if (asize == lsize && !memcmp(attr, labelsp, lsize))
			return i;  /* Found */
		labelsp  = labelsp + lsize;
		labelsp  = (char*) att_align(labelsp, typalign); 
	}
	return -1;  /* Not found */
}


/*
 * pivot_accum() - Pivot and accumulate
 */
static Datum oid_pivot_accum(FunctionCallInfo fcinfo, Oid type)
{
	ArrayType  *data;
	ArrayType  *labels;
	text       *attr;
	int         i;
	
	/* Simple argument validation */
	if (PG_NARGS() != 4)
		ereport(ERROR, 
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("pivot_accum called with %d input arguments",
						PG_NARGS())));
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(0));
	}
    labels = PG_GETARG_ARRAYTYPE_P(1);
	attr   = PG_GETARG_TEXT_P(2);
	
	/* Do nothing if the attr isn't in the labels array. */
	if ((i = pivot_find(labels, attr)) < 0)
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(0));
	}
	
	/* Get the data array, or make it if null */
	if (!PG_ARGISNULL(0))
	{
		data = PG_GETARG_ARRAYTYPE_P(0);
		Assert(ARR_DIMS(labels)[0] == ARR_DIMS(data)[0]);
	}
	else
	{
		int elsize, size, nelem;
		
		switch (type) {
			case INT4OID:
				elsize = 4; break;
			case INT8OID:
			case FLOAT8OID:
				elsize = 8; break;
			default:
				Assert(false);
		}
		nelem = ARR_DIMS(labels)[0];
		size = nelem * elsize + ARR_OVERHEAD_NONULLS(1);
		data = (ArrayType *) palloc(size);
		SET_VARSIZE(data, size);
		data->ndim = 1;
		data->dataoffset = 0;
		data->elemtype = type;
		ARR_DIMS(data)[0] = nelem;
		ARR_LBOUND(data)[0] = 1;
		memset(ARR_DATA_PTR(data), 0, nelem * elsize);
	}
	
	
	/*   
	 * Should we think about upconverting the arrays? Or is the assumption that 
	 * the pivot isn't usually doing much aggregation?
	 */
	switch (type) {
		case INT4OID:
		{
			int32 *datap = (int32*) ARR_DATA_PTR(data);
			int32  value = PG_GETARG_INT32(3);
			datap[i] += value;
			break;
		}
		case INT8OID:
		{
			int64 *datap = (int64*) ARR_DATA_PTR(data);
			int64  value = PG_GETARG_INT64(3);
			datap[i] += value;
			break;
		}
		case FLOAT8OID:
		{
			float8 *datap = (float8*) ARR_DATA_PTR(data);
			float8  value = PG_GETARG_FLOAT8(3);
			datap[i] += value;
			break;
		}
		default:
			Assert(false);
	}
	PG_RETURN_ARRAYTYPE_P(data);
}
