#include "gnet.h"

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

PG_FUNCTION_INFO_V1(float8_mregr_accum);     /* returns float8[] */
PG_FUNCTION_INFO_V1(float8_mregr_combine);   /* returns float8[] */
PG_FUNCTION_INFO_V1(float8_mregr_coef);      /* returns float8[] */
PG_FUNCTION_INFO_V1(float8_mregr_r2);        /* returns float8   */

Datum
float8_mregr_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *state;
	float8     *stateData, *newData, *matrix;
	float8      newY;
	ArrayType  *newX;
	int         len, statelen;	
	int         i,j;

	/* We should be strict, but it doesn't hurt to be paranoid */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(0));
	}
	
	state = PG_GETARG_ARRAYTYPE_P(0);	
	newY  = PG_GETARG_FLOAT8(1);
    newX  = PG_GETARG_ARRAYTYPE_P(2);

	/* Ensure that both arrays are single dimensional float8[] arrays */
	if (ARR_NULLBITMAP(state) || ARR_NDIM(state) != 1 || 
		ARR_ELEMTYPE(state) != FLOAT8OID)
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid transition state")));
	if (ARR_NDIM(newX) != 1 || ARR_ELEMTYPE(newX) != FLOAT8OID)
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("data array must be a single dimensional float8[]")));
	
	/* Only callable as a transition function */
	if (!(fcinfo->context && IsA(fcinfo->context, AggState)))
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_accum called from invalid context")));

	/* newX with nulls will be ignored */
	if (ARR_NULLBITMAP(newX))
		PG_RETURN_ARRAYTYPE_P(state);

	/*
	 * If length(state) == 1 then it is an unitialized state, extend as
	 * needed, we use this instead of NULL so that we can declare the
	 * function as strict.
	 */
	len = ARR_DIMS(newX)[0];
	statelen = 4 + len + len*len;
	if (ARR_DIMS(state)[0] == 1)
	{
		int size = statelen * sizeof(int64) + ARR_OVERHEAD_NONULLS(1);
		state = (ArrayType *) palloc(size);
		SET_VARSIZE(state, size);
		state->ndim = 1;
		state->dataoffset = 0;
		state->elemtype = FLOAT8OID;
		ARR_DIMS(state)[0] = statelen;
		ARR_LBOUND(state)[0] = 1;
		stateData = (float8*) ARR_DATA_PTR(state);
		memset(stateData, 0, statelen * sizeof(int64));
		stateData[0] = len;
	}

	/* 
	 * Contents of 'state' are as follows:
	 *   [0]     = len(X[])
	 *   [1]     = count
	 *   [2]     = sum(y)
	 *   [3]     = sum(y*y)
	 *   [4:N]   = sum(y * X[]) 
	 *   [N+1:M] = sum(X[] * X'[])
	 *   N       = 3 + len(X)
	 *   M       = N + len(X)*len(X)
	 */
	newData   = (float8*) ARR_DATA_PTR(newX);
	stateData = (float8*) ARR_DATA_PTR(state);
	
	/* It is an error if the number of indepent variables is not constant */
	if (stateData[0] != len)
	{
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("independent variable array not constant width")));
	}

	/* Something is seriously fishy if our state has the wrong length */
	if (ARR_DIMS(state)[0] != statelen)
	{
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_accum malformed state value")));
	}
	
	/* Okay... All's good now do the work */
	stateData[1]++;
	stateData[2] += newY;
	stateData[3] += newY * newY;
	for (i = 0; i < len; i++)
		stateData[4+i] += newY * newData[i];

	/* Compute the matrix X[] * X'[] and add it in */
	matrix = stateData + 4 + len;
	for (i = 0; i < len; i++)
		for (j = 0; j < len; j++)
			matrix[i*len + j] += newData[i] * newData[j];
	
	PG_RETURN_ARRAYTYPE_P(state);
}

Datum
float8_mregr_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *state1, *state2, *result;
	float8     *state1Data, *state2Data, *resultData;
	int         i, size;
	int         len, statelen;

	/* We should be strict, but it doesn't hurt to be paranoid */
	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(1));
	}
	if (PG_ARGISNULL(1))
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(0));
	
	state1 = PG_GETARG_ARRAYTYPE_P(0);	
	state2 = PG_GETARG_ARRAYTYPE_P(1);

	/* Ensure that both arrays are single dimensional float8[] arrays */
	if (ARR_NULLBITMAP(state1) || ARR_NULLBITMAP(state2) || 
		ARR_NDIM(state1) != 1 || ARR_NDIM(state2) != 1 || 
		ARR_ELEMTYPE(state1) != FLOAT8OID || ARR_ELEMTYPE(state2) != FLOAT8OID)
	{
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid transition state")));
	}
	
	/* 
	 * Remember that we initialized to {0}, so if either array is still at
	 * the initial value then just return the other one 
	 */
	if (ARR_DIMS(state1)[0] == 1)
		PG_RETURN_ARRAYTYPE_P(state2);
	if (ARR_DIMS(state2)[0] == 1)
		PG_RETURN_ARRAYTYPE_P(state1);

	state1Data = (float8*) ARR_DATA_PTR(state1);
	state2Data = (float8*) ARR_DATA_PTR(state2);

	if (ARR_DIMS(state1)[0] != ARR_DIMS(state2)[0] || 
		state1Data[0] != state2Data[0])
	{
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("independent variable array not constant width")));
	}
	len = state1Data[0];
	statelen = 4 + len + len*len;
	if (ARR_DIMS(state1)[0] != statelen)
	{
		ereport(ERROR, 
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("mregr_combine: invalid state")));
	}

	/* Validations pass, allocate memory for result and do work */
	size = statelen * sizeof(int64) + ARR_OVERHEAD_NONULLS(1);
	result = (ArrayType *) palloc(size);
	SET_VARSIZE(result, size);
	result->ndim = 1;
	result->dataoffset = 0;
	result->elemtype = FLOAT8OID;
	ARR_DIMS(result)[0] = statelen;
	ARR_LBOUND(result)[0] = 1;
	resultData = (float8*) ARR_DATA_PTR(result);
	memset(resultData, 0, statelen * sizeof(int64));

	/* 
	 * Contents of 'state' are as follows:
	 *   [0]     = len(X[])
	 *   [1]     = count
	 *   [2]     = sum(y)
	 *   [3]     = sum(y*y)
	 *   [4:N]   = sum(y * X[]) 
	 *   [N+1:M] = sum(X[] * X'[])
	 *   N       = 3 + len(X)
	 *   M       = N + len(X)*len(X)
	 */
	resultData[0] = len;
	for (i = 1; i < statelen; i++)
		resultData[i] = state1Data[i] + state2Data[i];	
	PG_RETURN_ARRAYTYPE_P(result);
}


Datum float8_mregr_coef(PG_FUNCTION_ARGS)
{
	Datum out;
	ArrayType *in, *b, *XtX, *XtXinv, *answer, *result;
	float8 *data, *b_in, *XtX_in;
	float8 count, sumy, sumy2;
	int    len;
	int    dims[2], lbs[2];

	if (PG_NARGS() != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_coef called with %d arguments", 
						PG_NARGS())));
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	in = PG_GETARG_ARRAYTYPE_P(0);
	if (ARR_ELEMTYPE(in) != FLOAT8OID || ARR_NDIM(in) != 1 || ARR_NULLBITMAP(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_coef called on invalid input array")));

	/* Validate the correct size input */
	data = (float8*) ARR_DATA_PTR(in);
	if (ARR_DIMS(in)[0] < 2)
		PG_RETURN_NULL();  /* no input */
	len   = (int) data[0]; /* scalar:           len(X[]) */
	if (ARR_DIMS(in)[0] != 4 + len + len*len) 
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_coef called on invalid input array")));

	count  = data[1];       /* scalar:           count(*) */
	sumy   = data[2];       /* scalar:           sum(y)   */
	sumy2  = data[3];       /* scalar:           sum(y*y) */
	b_in   = &data[4];      /* vector[len]:      sum(y * X[]) */
	XtX_in = &data[4+len];  /* matrix[len][len]: sum(X[] * X'[]) */
	
	/* 
	 * The various matrix routines all take ArrayTypes as input, so we need to add the
	 * various array headers to our vector and matrix 
	 */
	lbs[0] = 1;
	lbs[1] = 1;
	dims[0] = len;
	dims[1] = 1;
	b = construct_md_array((Datum *) b_in, NULL, 2, dims, lbs, FLOAT8OID, 
						   sizeof(float8), true, 'd');
	dims[1] = len;
	XtX = construct_md_array((Datum *) XtX_in, NULL, 2, dims, lbs, FLOAT8OID, 
							 sizeof(float8), true, 'd');

	/* 
	 * Coeficients are found via the formula:  
	 *      coef = (X'X)^-1 * b
	 */
	out = DirectFunctionCall1(pseudoinverse, PointerGetDatum(XtX));
	XtXinv = DatumGetArrayTypeP(out);
	out = DirectFunctionCall2(matrix_multiply, out, PointerGetDatum(b));
	answer = DatumGetArrayTypeP(out);

	/* Free our intermediate results */
	pfree(XtXinv);
	pfree(XtX);
	pfree(b);

	/* 
	 * We have the result, but we want to return it as a vector not as a matrix with
	 * a single column
	 */
	data = (float8 *) ARR_DATA_PTR(answer);
	result = construct_md_array((Datum *) data, NULL, 1, dims, lbs, FLOAT8OID, 
							 sizeof(float8), true, 'd');
	pfree(answer);

	PG_RETURN_ARRAYTYPE_P(result);
}


Datum float8_mregr_r2(PG_FUNCTION_ARGS)
{
	Datum out;
	ArrayType *in, *b, *bt, *XtX, *XtXinv, *coef, *cb;
	float8 *data, *b_in, *XtX_in;
	float8 count, sumy, sumy2;
	float8 ssr, tss, r2;
	int    len;
	int    dims[2], lbs[2];

	/* Begin unfortunate duplication of code */
	if (PG_NARGS() != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_r2 called with %d arguments", 
						PG_NARGS())));
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	in = PG_GETARG_ARRAYTYPE_P(0);
	if (ARR_ELEMTYPE(in) != FLOAT8OID || ARR_NDIM(in) != 1 || ARR_NULLBITMAP(in))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_r2 called on invalid input array")));

	/* Validate the correct size input */
	data = (float8*) ARR_DATA_PTR(in);
	if (ARR_DIMS(in)[0] < 2)
		PG_RETURN_NULL();  /* no input */
	len   = (int) data[0]; /* scalar:           len(X[]) */
	if (ARR_DIMS(in)[0] != 4 + len + len*len) 
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("float8_mregr_r2 called on invalid input array")));

	count  = data[1];       /* scalar:           count(*) */
	sumy   = data[2];       /* scalar:           sum(y)   */
	sumy2  = data[3];       /* scalar:           sum(y*y) */
	b_in   = &data[4];      /* vector[len]:      sum(y * X[]) */
	XtX_in = &data[4+len];  /* matrix[len][len]: sum(X[] * X'[]) */
	
	/* 
	 * The various matrix routines all take ArrayTypes as input, so we need to add the
	 * various array headers to our vector and matrix 
	 */
	lbs[0] = 1;
	lbs[1] = 1;

	dims[0] = len;
	dims[1] = 1;
	b = construct_md_array((Datum *) b_in, NULL, 2, dims, lbs, FLOAT8OID, 
						   sizeof(float8), true, 'd');

	dims[0] = 1;
	dims[1] = len;
	bt = construct_md_array((Datum *) b_in, NULL, 2, dims, lbs, FLOAT8OID, 
							sizeof(float8), true, 'd');

	dims[0] = len;
	dims[1] = len;
	XtX = construct_md_array((Datum *) XtX_in, NULL, 2, dims, lbs, FLOAT8OID, 
							 sizeof(float8), true, 'd');

	/* 
	 * Coeficients are found via the formula:  
	 *      coef = (X'X)^-1 * b
	 */
	out = DirectFunctionCall1(pseudoinverse, PointerGetDatum(XtX));
	XtXinv = DatumGetArrayTypeP(out);
	out = DirectFunctionCall2(matrix_multiply, out, PointerGetDatum(b));
	coef = DatumGetArrayTypeP(out);

	/* 
	 *  Here we actually diverge from what mregr_coefs did:
	 *     ssr = b' * coef - sum(y)^2/c
	 *     tss = sum(y^2) - sum(y)^2/c
     *     R^2 = ssr/tss
	 */
	out = DirectFunctionCall2(matrix_multiply, PointerGetDatum(bt), out); 
	cb  = DatumGetArrayTypeP(out);
	ssr = ((float8 *)ARR_DATA_PTR(cb))[0] - sumy*sumy/count;
	tss = sumy2 - sumy*sumy/count;
	r2  = ssr/tss;

	/* Free our intermediate results */
	pfree(XtXinv);
	pfree(XtX);
	pfree(b);
	pfree(bt);
	pfree(coef);
	pfree(cb);

	PG_RETURN_FLOAT8(r2);
}

