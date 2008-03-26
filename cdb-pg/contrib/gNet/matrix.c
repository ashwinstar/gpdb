#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* External functions */
PG_FUNCTION_INFO_V1(matrix_transpose);
PG_FUNCTION_INFO_V1(matrix_multiply);
PG_FUNCTION_INFO_V1(int8_matrix_smultiply);
PG_FUNCTION_INFO_V1(int4_matrix_add);
PG_FUNCTION_INFO_V1(int8_matrix_add);
PG_FUNCTION_INFO_V1(float8_matrix_add);

Datum matrix_transpose(PG_FUNCTION_ARGS);
Datum matrix_multiply(PG_FUNCTION_ARGS);
Datum int8_matrix_smultiply(PG_FUNCTION_ARGS);
Datum int4_matrix_add(PG_FUNCTION_ARGS);
Datum int8_matrix_add(PG_FUNCTION_ARGS);
Datum float8_matrix_add(PG_FUNCTION_ARGS);

/*
 * matrix_transpose - transpose an array[x][y] into an array[y][x].
 */
Datum 
matrix_transpose(PG_FUNCTION_ARGS)
{
	ArrayType   *m, *result;
	Oid         eltype;
	int         elsize, size, ndim, databytes;
	int         i,j;
	int16       typlen;
	bool        typbyval;
	char        typalign;
	char       *data_m, *data_r;
	
    m = PG_GETARG_ARRAYTYPE_P(0);
	ndim = ARR_NDIM(m);
	
	/* Sanity check, transpose only valid for two dimensional arrays. */
	if (ndim != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument must be a two dimensional array")));
	if (m->dataoffset || ARR_NULLBITMAP(m))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null array element not allowed in this context")));
	if (ARR_ELEMTYPE(m) == InvalidOid)
		ereport(ERROR, 
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not determine anyarray/anyelement type because "
						"input has type \"unknown\"")));
	
	/* datatype and size of input matrix */
	eltype = ARR_ELEMTYPE(m);
	size   = ARR_SIZE(m);
	databytes = size - ARR_DATA_OFFSET(m);
	
	/* Allocate the result matrix */
	result = (ArrayType *) palloc(size);
	SET_VARSIZE(result, size);
	result->ndim = ndim;
	result->dataoffset = 0;      /* We disallowed arrays with NULLS */
	result->elemtype = eltype;
	ARR_DIMS(result)[1] = ARR_DIMS(m)[0];
	ARR_DIMS(result)[0] = ARR_DIMS(m)[1];
	ARR_LBOUND(result)[1] = ARR_LBOUND(m)[0];
	ARR_LBOUND(result)[0] = ARR_LBOUND(m)[1];
	
	/* 
	 * If one of the dimensions was length 1 then it is a simple transpose
	 * and we can directly copy the data 
	 */
	if (ARR_DIMS(m)[0] == 1 || ARR_DIMS(m)[1] == 1)
	{
		memcpy(ARR_DATA_PTR(result), ARR_DATA_PTR(m), databytes);
		PG_RETURN_ARRAYTYPE_P(result);
	}
	
	/* Otherwise we have a more complicated data reshuffle */
	get_typlenbyvalalign(eltype, &typlen, &typbyval, &typalign);
	elsize = att_align(typlen, typalign);
	data_m = ARR_DATA_PTR(m);
	data_r = ARR_DATA_PTR(result);
	for (i = 0; i < ARR_DIMS(m)[1]; i++)
	{
		for (j = 0; j < ARR_DIMS(m)[0]; j++)
		{
			int index_m = i + j * ARR_DIMS(m)[1];
			char *seek = data_m + (typalign * index_m);
			if (typlen > 0)
			{
				memcpy(data_r, seek, elsize);
				data_r = (char *) att_align(data_r+elsize, typalign);
			}
			else 
			{
				elsize = att_addlength(0, typlen, PointerGetDatum(seek));
				memcpy(data_r, seek, elsize);
				data_r = (char *) att_align(data_r+elsize, typalign);
			}
		}
	}
	Assert(data_r - ARR_DATA_PTR(result) == databytes);
	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * matrix_multipy - array multiplication of two input arrays
 */
Datum matrix_multiply(PG_FUNCTION_ARGS)
{
	ArrayType  *m, *n, *result;
	Oid         eltype;
	int         elsize, size;
	int         i,j,k;
	int16       typlen;
	bool        typbyval;
	char        typalign;
	char       *data_m, *data_n, *data_r;
	
    m = PG_GETARG_ARRAYTYPE_P(0);
    n = PG_GETARG_ARRAYTYPE_P(1);
	eltype = ARR_ELEMTYPE(m);
	
	/* Do all error checking */
	if (ARR_NDIM(m) != 2 || ARR_NDIM(n) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument must be a two dimensional array")));
	if (ARR_DIMS(m)[1] != ARR_DIMS(n)[0])
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("non-conformable arrays")));
	if (m->dataoffset || ARR_NULLBITMAP(m) ||
		n->dataoffset || ARR_NULLBITMAP(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null array element not allowed in this context")));
	if (!OidIsValid(ARR_ELEMTYPE(m)) || !OidIsValid(ARR_ELEMTYPE(n)))
		ereport(ERROR, 
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not determine anyarray/anyelement type because "
						"input has type \"unknown\"")));
	if (ARR_ELEMTYPE(m) != ARR_ELEMTYPE(n))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot multiply arrays of different element types")));
	if (eltype != INT2OID && eltype != INT4OID && eltype != INT8OID &&
		eltype != FLOAT4OID && eltype != FLOAT8OID /* && eltype != NUMERICOID */)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("datatype not supported for array multiplication")));
	
	/* datatype and size of input matrix */
	get_typlenbyvalalign(eltype, &typlen, &typbyval, &typalign);
	elsize = att_align(typlen, typalign);
	
	size = ARR_OVERHEAD_NONULLS(2);  /* overhead for two dimensional array */
	size += ARR_DIMS(m)[0] * ARR_DIMS(n)[1] * elsize;
	result = (ArrayType *) palloc(size);
	SET_VARSIZE(result, size);
	result->ndim = 2;
	result->dataoffset = 0;          /* We dissallowed arrays with NULLS */
	result->elemtype = eltype;
	ARR_DIMS(result)[0] = ARR_DIMS(m)[0];
	ARR_DIMS(result)[1] = ARR_DIMS(n)[1];
	ARR_LBOUND(result)[0] = 1;       /* may need better lbound handling? */
	ARR_LBOUND(result)[1] = 1;
	
	/* 
	 * Three dimensional loop over result arrays two dimensional space and
	 * over the intersection dimension of the two input matrices.
	 * Locality is preserved for the m matrix.
	 *
	 * This will need to be reworked for variable length numeric elements.
	 */
	data_r = ARR_DATA_PTR(result);
	for (i = 0; i < ARR_DIMS(result)[1]; i++)
	{
		for (j = 0; j < ARR_DIMS(result)[0]; j++)
		{
			data_m = ARR_DATA_PTR(m) + j * ARR_DIMS(m)[1] * elsize;
			memset(data_r, 0, elsize);
			for (k = 0; k < ARR_DIMS(m)[1]; k++)
			{
				data_n = ARR_DATA_PTR(n) + k * ARR_DIMS(n)[1] * elsize;
				switch (eltype) {
					case INT2OID:
						*(int16*)data_r += 
							((int16*)data_n)[i] * ((int16*)data_m)[k];
						break;
					case INT4OID:
						*(int32*)data_r += 
							((int32*)data_n)[i] * ((int32*)data_m)[k];
						break;
					case INT8OID:
						*(int64*)data_r += 
							((int64*)data_n)[i] * ((int64*)data_m)[k];
						break;
					case FLOAT4OID:
						*(float*)data_r += 
							((float*)data_n)[i] * ((float*)data_m)[k];
						break;
					case FLOAT8OID:
						*(float8*)data_r += 
							((float8*)data_n)[i] * ((float8*)data_m)[k];
						break;
					default:
						Assert(false);
				}
			}
			data_r += elsize;
		}
	}
	PG_RETURN_ARRAYTYPE_P(result);
}


/*
 * int8_matrix_smultiply - scalar multiple of input array by an int8
 */
Datum int8_matrix_smultiply(PG_FUNCTION_ARGS)
{
	ArrayType  *m, *result;
	int64      *data_r;
	int64       scalar;
	int         size, nelem, ndim;
	int         i;
	
	if (PG_NARGS() != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("matrix_smultiply_int8 called with %d arguments", 
						PG_NARGS())));
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	
	scalar = PG_GETARG_INT64(1);
    m = PG_GETARG_ARRAYTYPE_P(0);
	ndim = ARR_NDIM(m);
	
	/* Do all error checking */
	if (ARR_ELEMTYPE(m) != INT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("matrix_smultiply_int8 datatype mismatch")));
	
	/* count number of elements */
	for (nelem = 1, i = 0; i < ndim; i++)
		nelem *= ARR_DIMS(m)[i];
	
	/* Make a copy of the input matrix */
	size = VARSIZE(m);
	result = (ArrayType *) palloc(size);
	SET_VARSIZE(result, size);
	memcpy(result, m, size);
	
	/* And multiply by the scalar in place */
	data_r = (int64*) ARR_DATA_PTR(result);
	for (i = 0; i < nelem; i++)
		data_r[i] *= scalar;
	PG_RETURN_ARRAYTYPE_P(result);
}


/*
 * matrix_add - array summation over two input arrays
 */
Datum int4_matrix_add(PG_FUNCTION_ARGS)
{
	ArrayType  *m, *n;
	int32      *data_m, *data_n;
	int         i, ndim, len;
	bool        transition_function;
	
	/* If we're in a transition function we can be smarter */
	transition_function = fcinfo->context && IsA(fcinfo->context, AggState);
	
	/* Validate arguments */
	if (PG_NARGS() != 2) 
    {
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("matrix_sum called with %d arguments", PG_NARGS())));
	}   
	
	/* Both are NULL - easy */
	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
		PG_RETURN_NULL();
	
	/* One is null, but not the other */
	if (PG_ARGISNULL(1) || (PG_ARGISNULL(0) && !transition_function))
	{
		if (PG_ARGISNULL(1))
			m = PG_GETARG_ARRAYTYPE_P(0);
		else
			m = PG_GETARG_ARRAYTYPE_P(1);
		if (ARR_ELEMTYPE(m) != INT4OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("input did not match bigint[]")));
		if (ARR_NULLBITMAP(m))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null array element not allowed in this context")));
		
		PG_RETURN_ARRAYTYPE_P(m);
	}
	
	/* When the state is null and we are called as a transition function we
	   can't simply return the later pointer because we will try to update the
	   state in place on future calls, in this case we must allocate a new
	   array and copy the old values */
	if (PG_ARGISNULL(0))
	{
		int size;
		
		n = PG_GETARG_ARRAYTYPE_P(1);
		if (ARR_ELEMTYPE(n) != INT4OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("input did not match bigint[]")));
		if (ARR_NULLBITMAP(n))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null array element not allowed in this context")));
		
		/* count total number of elements */
		ndim = ARR_NDIM(n);
		for (len = 1, i = 0; i < ndim; i++)
			len *= ARR_DIMS(n)[i];
		
		size = ARR_OVERHEAD_NONULLS(ndim); 
		size += len * sizeof(int32);
		m = (ArrayType *) palloc(size);
		memcpy(m, n, size);
		PG_RETURN_ARRAYTYPE_P(m);
	}
	
	/* Both non-null we have work to do */
	m = PG_GETARG_ARRAYTYPE_P(0);
	n = PG_GETARG_ARRAYTYPE_P(1);
	data_n = (int32*) ARR_DATA_PTR(n);
	data_m = (int32*) ARR_DATA_PTR(m);
	
	ndim = ARR_NDIM(m);
	if (ndim != ARR_NDIM(n))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Dimensionality of both arrays must match")));
	for (i = 0; i < ndim; i++)
	{
		if (ARR_DIMS(m)[i] != ARR_DIMS(n)[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("non-conformable arrays")));
	}
	if (ARR_ELEMTYPE(m) != INT4OID || ARR_ELEMTYPE(n) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("input did not match bigint[]")));
	if (ARR_NULLBITMAP(m) || ARR_NULLBITMAP(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null array element not allowed in this context")));
	
	/* count total number of elements */
	for (len = 1, i = 0; i < ndim; i++)
		len *= ARR_DIMS(n)[i];
	
	/* Transition function updates in place, otherwise allocate result */
	if (transition_function) 
	{
		for (i = 0; i < len; i++)
			data_m[i] += data_n[i];
		PG_RETURN_ARRAYTYPE_P(m);
	}
	else
	{
		ArrayType *result;
		int32     *data_r;
		int        size;
		
		size = ARR_OVERHEAD_NONULLS(ndim); 
		size += len * sizeof(int32);
		result = (ArrayType *) palloc(size);
		SET_VARSIZE(result, size);
		result->ndim = ndim;
		result->dataoffset = 0;          /* We dissallowed arrays with NULLS */
		result->elemtype = INT4OID;
		for (i = 0; i < ndim; i++)
		{
			ARR_DIMS(result)[i] = ARR_DIMS(n)[i];
			ARR_LBOUND(result)[i] = 1;
		}
		data_r = (int32*) ARR_DATA_PTR(result);
		for (i = 0; i < len; i++)
			data_r[i] = data_m[i] + data_n[i];
		
		PG_RETURN_ARRAYTYPE_P(result);
	} 
}


/*
 * matrix_add - array summation over two input arrays
 */
Datum int8_matrix_add(PG_FUNCTION_ARGS)
{
	ArrayType  *m, *n;
	int64      *data_m, *data_n;
	int         i, ndim, len;
	bool        transition_function;
	
	/* If we're in a transition function we can be smarter */
	transition_function = fcinfo->context && IsA(fcinfo->context, AggState);
	
	/* Validate arguments */
	if (PG_NARGS() != 2) 
    {
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("matrix_sum called with %d arguments", PG_NARGS())));
	}   
	
	/* Both are NULL - easy */
	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
		PG_RETURN_NULL();
	
	/* One is null, but not the other */
	if (PG_ARGISNULL(1) || (PG_ARGISNULL(0) && !transition_function))
	{
		if (PG_ARGISNULL(1))
			m = PG_GETARG_ARRAYTYPE_P(0);
		else
			m = PG_GETARG_ARRAYTYPE_P(1);
		if (ARR_ELEMTYPE(m) != INT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("input did not match bigint[]")));
		if (ARR_NULLBITMAP(m))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null array element not allowed in this context")));
		
		PG_RETURN_ARRAYTYPE_P(m);
	}
	
	/* When the state is null and we are called as a transition function we
	   can't simply return the later pointer because we will try to update the
	   state in place on future calls, in this case we must allocate a new
	   array and copy the old values */
	if (PG_ARGISNULL(0))
	{
		int size;
		
		n = PG_GETARG_ARRAYTYPE_P(1);
		if (ARR_ELEMTYPE(n) != INT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("input did not match bigint[]")));
		if (ARR_NULLBITMAP(n))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null array element not allowed in this context")));
		
		/* count total number of elements */
		ndim = ARR_NDIM(n);
		for (len = 1, i = 0; i < ndim; i++)
			len *= ARR_DIMS(n)[i];
		
		size = ARR_OVERHEAD_NONULLS(ndim); 
		size += len * sizeof(int64);
		m = (ArrayType *) palloc(size);
		memcpy(m, n, size);
		PG_RETURN_ARRAYTYPE_P(m);
	}
	
	/* Both non-null we have work to do */
	m = PG_GETARG_ARRAYTYPE_P(0);
	n = PG_GETARG_ARRAYTYPE_P(1);
	data_n = (int64*) ARR_DATA_PTR(n);
	data_m = (int64*) ARR_DATA_PTR(m);
	
	ndim = ARR_NDIM(m);
	if (ndim != ARR_NDIM(n))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Dimensionality of both arrays must match")));
	for (i = 0; i < ndim; i++)
	{
		if (ARR_DIMS(m)[i] != ARR_DIMS(n)[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("non-conformable arrays")));
	}
	if (ARR_ELEMTYPE(m) != INT8OID || ARR_ELEMTYPE(n) != INT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("input did not match bigint[]")));
	if (ARR_NULLBITMAP(m) || ARR_NULLBITMAP(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null array element not allowed in this context")));
	
	/* count total number of elements */
	for (len = 1, i = 0; i < ndim; i++)
		len *= ARR_DIMS(n)[i];
	
	/* Transition function updates in place, otherwise allocate result */
	if (transition_function) 
	{
		for (i = 0; i < len; i++)
			data_m[i] += data_n[i];
		PG_RETURN_ARRAYTYPE_P(m);
	}
	else
	{
		ArrayType *result;
		int64     *data_r;
		int        size;
		
		size = ARR_OVERHEAD_NONULLS(ndim); 
		size += len * sizeof(int64);
		result = (ArrayType *) palloc(size);
		SET_VARSIZE(result, size);
		result->ndim = ndim;
		result->dataoffset = 0;          /* We dissallowed arrays with NULLS */
		result->elemtype = INT8OID;
		for (i = 0; i < ndim; i++)
		{
			ARR_DIMS(result)[i] = ARR_DIMS(n)[i];
			ARR_LBOUND(result)[i] = 1;
		}
		data_r = (int64*) ARR_DATA_PTR(result);
		for (i = 0; i < len; i++)
			data_r[i] = data_m[i] + data_n[i];
		
		PG_RETURN_ARRAYTYPE_P(result);
	} 
}

/*
 * matrix_add - array summation over two input arrays
 */
Datum float8_matrix_add(PG_FUNCTION_ARGS)
{
	ArrayType  *m, *n;
	float8     *data_m, *data_n;
	int         i, ndim, len;
	bool        transition_function;
	
	/* If we're in a transition function we can be smarter */
	transition_function = fcinfo->context && IsA(fcinfo->context, AggState);
	
	/* Validate arguments */
	if (PG_NARGS() != 2) 
    {
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("matrix_sum called with %d arguments", PG_NARGS())));
	}   
	
	/* Both are NULL - easy */
	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
		PG_RETURN_NULL();
	
	/* One is null, but not the other */
	if (PG_ARGISNULL(1) || (PG_ARGISNULL(0) && !transition_function))
	{
		if (PG_ARGISNULL(1))
			m = PG_GETARG_ARRAYTYPE_P(0);
		else
			m = PG_GETARG_ARRAYTYPE_P(1);
		if (ARR_ELEMTYPE(m) != FLOAT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("input did not match bigint[]")));
		if (ARR_NULLBITMAP(m))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null array element not allowed in this context")));
		
		PG_RETURN_ARRAYTYPE_P(m);
	}
	
	/* When the state is null and we are called as a transition function we
	   can't simply return the later pointer because we will try to update the
	   state in place on future calls, in this case we must allocate a new
	   array and copy the old values */
	if (PG_ARGISNULL(0))
	{
		int size;
		
		n = PG_GETARG_ARRAYTYPE_P(1);
		if (ARR_ELEMTYPE(n) != FLOAT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("input did not match bigint[]")));
		if (ARR_NULLBITMAP(n))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null array element not allowed in this context")));
		
		/* count total number of elements */
		ndim = ARR_NDIM(n);
		for (len = 1, i = 0; i < ndim; i++)
			len *= ARR_DIMS(n)[i];
		
		size = ARR_OVERHEAD_NONULLS(ndim); 
		size += len * sizeof(float8);
		m = (ArrayType *) palloc(size);
		memcpy(m, n, size);
		PG_RETURN_ARRAYTYPE_P(m);
	}
	
	/* Both non-null we have work to do */
	m = PG_GETARG_ARRAYTYPE_P(0);
	n = PG_GETARG_ARRAYTYPE_P(1);
	data_n = (float8*) ARR_DATA_PTR(n);
	data_m = (float8*) ARR_DATA_PTR(m);
	
	ndim = ARR_NDIM(m);
	if (ndim != ARR_NDIM(n))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Dimensionality of both arrays must match")));
	for (i = 0; i < ndim; i++)
	{
		if (ARR_DIMS(m)[i] != ARR_DIMS(n)[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("non-conformable arrays")));
	}
	if (ARR_ELEMTYPE(m) != FLOAT8OID || ARR_ELEMTYPE(n) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("input did not match bigint[]")));
	if (ARR_NULLBITMAP(m) || ARR_NULLBITMAP(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null array element not allowed in this context")));
	
	/* count total number of elements */
	for (len = 1, i = 0; i < ndim; i++)
		len *= ARR_DIMS(n)[i];
	
	/* Transition function updates in place, otherwise allocate result */
	if (transition_function) 
	{
		for (i = 0; i < len; i++)
			data_m[i] += data_n[i];
		PG_RETURN_ARRAYTYPE_P(m);
	}
	else
	{
		ArrayType *result;
		float8    *data_r;
		int        size;
		
		size = ARR_OVERHEAD_NONULLS(ndim); 
		size += len * sizeof(float8);
		result = (ArrayType *) palloc(size);
		SET_VARSIZE(result, size);
		result->ndim = ndim;
		result->dataoffset = 0;          /* We dissallowed arrays with NULLS */
		result->elemtype = FLOAT8OID;
		for (i = 0; i < ndim; i++)
		{
			ARR_DIMS(result)[i] = ARR_DIMS(n)[i];
			ARR_LBOUND(result)[i] = 1;
		}
		data_r = (float8*) ARR_DATA_PTR(result);
		for (i = 0; i < len; i++)
			data_r[i] = data_m[i] + data_n[i];
		
		PG_RETURN_ARRAYTYPE_P(result);
	} 
}
