#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "postgres.h"
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "utils/numeric.h"
#include "utils/builtins.h"

#include "sparse_vector.h"

void check_dimension(SvecType *svec1, SvecType *svec2, char *msg);

Datum svec_dimension(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_dimension );
Datum
svec_dimension(PG_FUNCTION_ARGS)
{
	SvecType *svec = PG_GETARG_SVECTYPE_P(0);
	PG_RETURN_INT32(svec->dimension);
}

Datum svec_concat_replicate(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_concat_replicate);
Datum
svec_concat_replicate(PG_FUNCTION_ARGS)
{
	int multiplier = PG_GETARG_INT32(0);
	SvecType *svec = PG_GETARG_SVECTYPE_P(1);
	SparseData left  = sdata_from_svec(svec);
	SparseData sdata = makeEmptySparseData();
	char *vals,*index;
	int l_val_len = left->vals->len;
	int l_ind_len = left->index->len;
	int val_len=l_val_len*multiplier;
	int ind_len=l_ind_len*multiplier;

	vals = (char *)palloc(sizeof(char)*val_len);
	index = (char *)palloc(sizeof(char)*ind_len);

	for (int i=0;i<multiplier;i++)
	{
		memcpy(vals+i*l_val_len,left->vals->data,l_val_len);
		memcpy(index+i*l_ind_len,left->index->data,l_ind_len);
	}

	sdata->vals  = makeStringInfoFromData(vals,val_len);
	sdata->index = makeStringInfoFromData(index,ind_len);
	sdata->type_of_data = FLOAT8OID;
	sdata->unique_value_count = multiplier * left->unique_value_count;
	sdata->total_value_count  = multiplier * left->total_value_count;

	PG_RETURN_SVECTYPE_P(svec_from_sparsedata(sdata));
}


Datum svec_concat(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_concat );
Datum
svec_concat(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	SparseData left  = sdata_from_svec(svec1);
	SparseData right = sdata_from_svec(svec2);
	SparseData sdata = makeEmptySparseData();
	char *vals,*index;
	int l_val_len = left->vals->len;
	int r_val_len = right->vals->len;
	int l_ind_len = left->index->len;
	int r_ind_len = right->index->len;
	int val_len=l_val_len+r_val_len;
	int ind_len=l_ind_len+r_ind_len;

	vals = (char *)palloc(sizeof(char)*val_len);
	index = (char *)palloc(sizeof(char)*ind_len);

	memcpy(vals          ,left->vals->data,l_val_len);
	memcpy(vals+l_val_len,right->vals->data,r_val_len);
	memcpy(index,          left->index->data,l_ind_len);
	memcpy(index+l_ind_len,right->index->data,r_ind_len);

	sdata->vals  = makeStringInfoFromData(vals,val_len);
	sdata->index = makeStringInfoFromData(index,ind_len);
	sdata->type_of_data = FLOAT8OID;
	sdata->unique_value_count = left->unique_value_count+
				    right->unique_value_count;
	sdata->total_value_count  = left->total_value_count+
				    right->total_value_count;

	PG_RETURN_SVECTYPE_P(svec_from_sparsedata(sdata));
}


Datum svec_eq(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_eq );
Datum
svec_eq(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	SparseData left  = sdata_from_svec(svec1);
	SparseData right = sdata_from_svec(svec2);
	PG_RETURN_BOOL(sparsedata_eq(left,right));
}

static int32_t
svec_l2_cmp_internal(SvecType *svec1, SvecType *svec2)
{
	SparseData left  = sdata_from_svec(svec1);
	SparseData right = sdata_from_svec(svec2);
	double magleft  = l2norm_sdata_values_double(left);
	double magright = l2norm_sdata_values_double(right);
	int result;
	if (magleft < magright)
	{
		result = -1;
	}
	else if (magleft > magright)
	{
		result = 1;
	}
	else
	{
		result = 0;
	}
	PG_RETURN_INT32(result);
}
Datum svec_l2_cmp(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_l2_cmp );
Datum svec_l2_cmp(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	PG_RETURN_INT32(svec_l2_cmp_internal(svec1,svec2));
}
Datum svec_l2_lt(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_l2_lt );
Datum svec_l2_lt(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	int result = svec_l2_cmp_internal(svec1,svec2);
	PG_RETURN_BOOL((result == -1) ? 1 : 0);
}
Datum svec_l2_le(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_l2_le );
Datum svec_l2_le(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	int result = svec_l2_cmp_internal(svec1,svec2);
	PG_RETURN_BOOL(((result == -1)||(result == 0)) ? 1 : 0);
}
Datum svec_l2_eq(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_l2_eq );
Datum svec_l2_eq(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	int result = svec_l2_cmp_internal(svec1,svec2);
	PG_RETURN_BOOL((result == 0) ? 1 : 0);
}
Datum svec_l2_ne(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_l2_ne );
Datum svec_l2_ne(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	int result = svec_l2_cmp_internal(svec1,svec2);
	PG_RETURN_BOOL((result != 0) ? 1 : 0);
}
Datum svec_l2_gt(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_l2_gt );
Datum svec_l2_gt(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	int result = svec_l2_cmp_internal(svec1,svec2);
	PG_RETURN_BOOL((result == 1) ? 1 : 0);
}
Datum svec_l2_ge(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1( svec_l2_ge );
Datum svec_l2_ge(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	int result = svec_l2_cmp_internal(svec1,svec2);
	PG_RETURN_BOOL(((result == 0) || (result == 1)) ? 1 : 0);
}

/*
 * Do one of subtract, add, multiply, divide or modulo depending on value
 * of operation(0,1,2,3,4)
 */

SvecType *
op_svec_by_svec_internal(int operation, SvecType *svec1, SvecType *svec2)
{
	SparseData left  = sdata_from_svec(svec1);
	SparseData right = sdata_from_svec(svec2);
	SparseData sdata=NULL;
	double *left_vals =(double *)(left->vals->data);
	double *right_vals=(double *)(right->vals->data);
	double data_result;

	int scalar_args=check_scalar(svec1,svec2);

	switch(scalar_args) {
		case 0: 		//neither arg is scalar
			sdata = op_sdata_by_sdata(operation,left,right);
			break;
		case 1:			//left arg is scalar
			
			sdata = op_sdata_by_scalar_copy(operation,left_vals[0],
					right,1);
			break;
		case 2:			//right arg is scalar
			sdata = op_sdata_by_scalar_copy(operation,right_vals[0],
					left,2);
			break;
		case 3:			//both args are scalar
			switch (operation)
			{
				case 1:
				default:
					data_result = left_vals[0]+right_vals[0];
					break;
				case 2:
					data_result = left_vals[0]*right_vals[0];
					break;
				case 3:
					data_result = left_vals[0]/right_vals[0];
					break;
				case 4:
					data_result = ((int)left_vals[0])%((int)right_vals[0]);
					break;
			}
			return(svec_make_scalar(data_result,1));
			break;
	}

	return(svec_from_sparsedata(sdata));
}

/*
 * Do exponentiation, only makes sense if the left is a vector and the right
 * is a scalar or if both are scalar
 */
static SvecType *
pow_svec_by_scalar_internal(SvecType *svec1, SvecType *svec2)
{
	SparseData left  = sdata_from_svec(svec1);
	SparseData right = sdata_from_svec(svec2);
	SparseData sdata=NULL;
	double *left_vals=(double *)(left->vals->data);
	double *right_vals=(double *)(right->vals->data);
	double data_result;

	int scalar_args=check_scalar(svec1,svec2);

	switch(scalar_args) {
		case 0: 		//neither arg is scalar
		case 1:			//left arg is scalar
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errOmitLocation(true),
				 errmsg("Svec exponentiation is undefined when the right argument is a vector")));
			break;
		case 2:			//right arg is scalar
			if (right_vals[0] == 2.) //recognize the squared case as special
			{
			  sdata = square_sdata(left);
			}  else if (right_vals[0] == 3.) //recognize the cubed case as special
			{
			  sdata = cube_sdata(left);
			}  else if (right_vals[0] == 4.) //recognize the quad case as special
			{
			  sdata = quad_sdata(left);
			} else {
			  sdata = pow_sdata_by_scalar(left,right_vals[0]);
			}
			break;
		case 3:			//both args are scalar
			data_result = pow(left_vals[0],right_vals[0]);
			return(svec_make_scalar(data_result,1));
			break;
	}

	return(svec_from_sparsedata(sdata));
}

PG_FUNCTION_INFO_V1( svec_pow );
Datum
svec_pow(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	check_dimension(svec1,svec2,"svec_minus");
	SvecType *result = pow_svec_by_scalar_internal(svec1,svec2);
	PG_RETURN_SVECTYPE_P(result);
}
PG_FUNCTION_INFO_V1( svec_minus );
Datum
svec_minus(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	check_dimension(svec1,svec2,"svec_minus");
	SvecType *result = op_svec_by_svec_internal(0,svec1,svec2);
	PG_RETURN_SVECTYPE_P(result);
}
PG_FUNCTION_INFO_V1( svec_plus );
Datum
svec_plus(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	check_dimension(svec1,svec2,"svec_plus");
	SvecType *result = op_svec_by_svec_internal(1,svec1,svec2);
	PG_RETURN_SVECTYPE_P(result);
}
PG_FUNCTION_INFO_V1( svec_mult );
Datum
svec_mult(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	check_dimension(svec1,svec2,"svec_mult");
	SvecType *result = op_svec_by_svec_internal(2,svec1,svec2);
	PG_RETURN_SVECTYPE_P(result);
}
PG_FUNCTION_INFO_V1( svec_div );
Datum
svec_div(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	check_dimension(svec1,svec2,"svec_div");
	SvecType *result = op_svec_by_svec_internal(3,svec1,svec2);
	PG_RETURN_SVECTYPE_P(result);
}

/*
 * Count the number of non-zero entries in the input vector
 * Right argument is capped at 1, then added to the left
 */
PG_FUNCTION_INFO_V1( svec_count );

Datum
svec_count(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	SparseData left = makeInplaceSparseData(
		SVEC_VALS_PTR(svec1),SVEC_INDEX_PTR(svec1),
		SVEC_DATA_SIZE(svec1),SVEC_INDEX_SIZE(svec1),FLOAT8OID,
		SVEC_UNIQUE_VALCNT(svec1),SVEC_TOTAL_VALCNT(svec1));
	SparseData right = makeInplaceSparseData(
		SVEC_VALS_PTR(svec2),SVEC_INDEX_PTR(svec2),
		SVEC_DATA_SIZE(svec2),SVEC_INDEX_SIZE(svec2),FLOAT8OID,
		SVEC_UNIQUE_VALCNT(svec2),SVEC_TOTAL_VALCNT(svec2));
	double *right_vals=(double *)(right->vals->data);
	SvecType *result;
	double *clamped_vals;
	SparseData right_clamped,sdata_result;

	int scalar_args=check_scalar(svec1,svec2);
	check_dimension(svec1,svec2,"svec_count");

	/* Clamp the right vector values to 1.
	 */
	switch (scalar_args)
	{
		case 1:			//left arg is scalar
			/*
			 * If the left argument is a scalar, this is almost certainly the
			 * first call to the routine, and we need a zero vector for the
			 * beginning of the accumulation of the correct dimension.
			 */
			left = makeSparseDataFromDouble(0.,right->total_value_count);

		case 0: 		//neither arg is scalar
		case 2:			//right arg is scalar

			/* Create an array of values either 1 or 0 depending on whether
			 * the right vector has a non-zero value in it
			 */
			clamped_vals =
				(double *)palloc0(sizeof(double)*(right->unique_value_count));

			for (int i=0;i<(right->unique_value_count);i++)
			{
				if (right_vals[i]!=0.) clamped_vals[i]=1.;
			}
			right_clamped = makeInplaceSparseData((char *)clamped_vals,right->index->data,
					right->vals->len,right->index->len,FLOAT8OID,
					right->unique_value_count,right->total_value_count);

			/* Create the output SVEC */
			sdata_result = op_sdata_by_sdata(1,left,right_clamped);
			result = svec_from_sparsedata(sdata_result);

			pfree(clamped_vals);
			pfree(right_clamped);

			PG_RETURN_SVECTYPE_P(result);
			break;
		case 3:			//both args are scalar
		default:
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errOmitLocation(true),
				 errmsg("Svec count is undefined when both arguments are scalar")));
			PG_RETURN_SVECTYPE_P(svec1);
			break;
	}
}

PG_FUNCTION_INFO_V1( svec_dot );

Datum
svec_dot(PG_FUNCTION_ARGS)
{
	SvecType *svec1 = PG_GETARG_SVECTYPE_P(0);
	SvecType *svec2 = PG_GETARG_SVECTYPE_P(1);
	SparseData left  = sdata_from_svec(svec1);
	SparseData right = sdata_from_svec(svec2);
	SparseData mult_result;
	double accum;
	check_dimension(svec1,svec2,"svec_dot");

	mult_result = op_sdata_by_sdata(2,left,right);
	accum = sum_sdata_values_double(mult_result);

	PG_RETURN_FLOAT8(accum);
}

PG_FUNCTION_INFO_V1( svec_l2norm );

Datum
svec_l2norm(PG_FUNCTION_ARGS)
{
	SvecType *svec = PG_GETARG_SVECTYPE_P(0);
	SparseData sdata  = sdata_from_svec(svec);
	double accum;
	accum = l2norm_sdata_values_double(sdata);

	PG_RETURN_FLOAT8(accum);
}


PG_FUNCTION_INFO_V1( svec_l1norm );

Datum
svec_l1norm(PG_FUNCTION_ARGS)
{
	SvecType *svec = PG_GETARG_SVECTYPE_P(0);
	SparseData sdata  = sdata_from_svec(svec);
	double accum;
	accum = l1norm_sdata_values_double(sdata);

	PG_RETURN_FLOAT8(accum);
}

PG_FUNCTION_INFO_V1( svec_log );

Datum
svec_log(PG_FUNCTION_ARGS)
{
	SvecType *svec = PG_GETARG_SVECTYPE_P_COPY(0);
	double *vals = (double *)SVEC_VALS_PTR(svec);
	int unique_value_count=SVEC_UNIQUE_VALCNT(svec);

	for (int i=0;i<unique_value_count;i++) vals[i] = log(vals[i]);

	PG_RETURN_SVECTYPE_P(svec);

}

/*
 * Cast from int2,int4,int8,float4,float8 scalar to SvecType
 */
PG_FUNCTION_INFO_V1( svec_cast_int2 );
Datum svec_cast_int2(PG_FUNCTION_ARGS) {
	float8 value=(float8 )PG_GETARG_INT16(0);
	PG_RETURN_SVECTYPE_P(svec_make_scalar(value,1));
}
PG_FUNCTION_INFO_V1( svec_cast_int4 );
Datum svec_cast_int4(PG_FUNCTION_ARGS) {
	float8 value=(float8 )PG_GETARG_INT32(0);
	PG_RETURN_SVECTYPE_P(svec_make_scalar(value,1));
}
PG_FUNCTION_INFO_V1( svec_cast_int8 );
Datum svec_cast_int8(PG_FUNCTION_ARGS) {
	float8 value=(float8 )PG_GETARG_INT64(0);
	PG_RETURN_SVECTYPE_P(svec_make_scalar(value,1));
}
PG_FUNCTION_INFO_V1( svec_cast_float4 );
Datum svec_cast_float4(PG_FUNCTION_ARGS) {
	float8 value=(float8 )PG_GETARG_FLOAT4(0);
	PG_RETURN_SVECTYPE_P(svec_make_scalar(value,1));
}
PG_FUNCTION_INFO_V1( svec_cast_float8 );
Datum svec_cast_float8(PG_FUNCTION_ARGS) {
	float8 value=PG_GETARG_FLOAT8(0);
	PG_RETURN_SVECTYPE_P(svec_make_scalar(value,1));
}
PG_FUNCTION_INFO_V1( svec_cast_numeric );
Datum svec_cast_numeric(PG_FUNCTION_ARGS) {
	Datum num=PG_GETARG_DATUM(0);
	float8 value;
	value = DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow,num));
	PG_RETURN_SVECTYPE_P(svec_make_scalar(value,1));
}

SvecType *svec_make_scalar(float8 value, int dimension) {
	SparseData sdata=float8arr_to_sdata(&value,1);
	SvecType *result=svec_from_sparsedata(sdata);
	result->dimension=-dimension;
	return result;
}

void check_dimension(SvecType *svec1, SvecType *svec2, char *msg) {
	/* If the array dimensions aren't the same, operation has no meaning unless one of 
	 * the inputs is a constant
	 */
	if ((!IS_SCALAR(svec1)) &&
	    (!IS_SCALAR(svec2)) &&
			(svec1->dimension != svec2->dimension)) {
//		elog(NOTICE,"svec1: %s",svec_out_internal(svec1));
//		elog(NOTICE,"svec2: %s",svec_out_internal(svec2));
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errOmitLocation(true),
			 errmsg("%s: array dimension of inputs are not the same: dim1=%d, dim2=%d\n",
			msg, svec1->dimension, svec2->dimension)));
	}
}

PG_FUNCTION_INFO_V1( svec_cast_float8arr );

Datum
svec_cast_float8arr(PG_FUNCTION_ARGS) {
	ArrayType *A_PG = PG_GETARG_ARRAYTYPE_P(0);
	SvecType *output_svec;

	if (ARR_ELEMTYPE(A_PG) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errOmitLocation(true),
				 errmsg("svec_cast_float8arr only defined over float8[]")));
	if (ARR_NDIM(A_PG) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errOmitLocation(true),
				 errmsg("svec_cast_float8arr only defined over 1 dimensional arrays"))
		       );

	if (ARR_NULLBITMAP(A_PG))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errOmitLocation(true),
				 errmsg("svec_cast_float8arr does not allow null bitmaps on arrays"))
		       );


	/* Extract array */
	{
		int dimension = ARR_DIMS(A_PG)[0];
		float8 *array = (float8 *)ARR_DATA_PTR(A_PG);

		/* Create the output SVEC */
		SparseData sdata = float8arr_to_sdata(array,dimension);
		output_svec = svec_from_sparsedata(sdata);
	}

	PG_RETURN_SVECTYPE_P(output_svec);
}

/*
 * Provide some operators for Postgres FLOAT8OID arrays
 */
/*
 * Equality
 */
bool
float8arr_equals_internal(ArrayType *left, ArrayType *right)
{
        int dimleft = ARR_NDIM(left), dimright = ARR_NDIM(right);
        int *dimsleft = ARR_DIMS(left), *dimsright = ARR_DIMS(right);
	int numleft = ArrayGetNItems(dimleft,dimsleft);
	int numright = ArrayGetNItems(dimright,dimsright);
        double *vals_left=(double *)ARR_DATA_PTR(left), *vals_right=(double *)ARR_DATA_PTR(right);
        bits8 *bitmap_left=ARR_NULLBITMAP(left), *bitmap_right=ARR_NULLBITMAP(right);
        int   bitmask=1;

        if ((dimsleft!=dimsright) || (numleft!=numright))
	{
		return(false);
	}

	/*
	 * Note that we are only defined for FLOAT8OID
	 */
        //get_typlenbyvalalign(ARR_ELEMTYPE(array),
        //                                         &typlen, &typbyval, &typalign);

	/*
	 * First we'll check to see if the null bitmaps are equivalent
	 */
	if (bitmap_left)
		if (! bitmap_right) return(false);
	if (bitmap_right)
		if (! bitmap_left) return(false);

	if (bitmap_left)
	{
        	for (int i=0; i<numleft; i++)
		{
                	if ((*bitmap_left & bitmask) == 0)
                		if ((*bitmap_left & bitmask) != 0)
			  		return(false);
                        bitmask <<= 1;
                        if (bitmask == 0x100)
                        {
                                bitmap_left++;
                                bitmask = 1;
                        }
		}
	}

	/*
	 * Now we check for equality of all array values
	 */
       	for (int i=0; i<numleft; i++)
		if (vals_left[i] != vals_right[i]) return(false);

        return(true);
}

PG_FUNCTION_INFO_V1( float8arr_equals);

Datum
float8arr_equals(PG_FUNCTION_ARGS) {
	ArrayType *left  = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType *right = PG_GETARG_ARRAYTYPE_P(1);

	PG_RETURN_BOOL(float8arr_equals_internal(left,right));
}
 
/*
 * L2 Norm
 */
double
float8arr_l2norm_internal(ArrayType *left)
{
        int dimleft = ARR_NDIM(left);
        int *dimsleft = ARR_DIMS(left);
	int numleft = ArrayGetNItems(dimleft,dimsleft);
        double *vals_left=(double *)ARR_DATA_PTR(left);
        bits8 *bitmap_left=ARR_NULLBITMAP(left);
        int   bitmask=1;
	double accum=0.;

	/*
	 * Convert null items into zeros
	 */
	if (bitmap_left)
	{
        	for (int i=0; i<numleft; i++)
		{
                	if ((*bitmap_left & bitmask) == 0)
				vals_left[i] = 0.;
                        bitmask <<= 1;
                        if (bitmask == 0x100)
                        {
                                bitmap_left++;
                                bitmask = 1;
                        }
		}
	}

	/*
	 * Now we check for equality of all array values
	 */
       	for (int i=0; i<numleft; i++)
	{
		accum += vals_left[i] * vals_left[i];
	}
	accum = sqrt(accum);

        return(accum);
}

PG_FUNCTION_INFO_V1( float8arr_l2norm);

Datum
float8arr_l2norm(PG_FUNCTION_ARGS) {
	ArrayType *left  = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_FLOAT8(float8arr_l2norm_internal(left));
}
 
