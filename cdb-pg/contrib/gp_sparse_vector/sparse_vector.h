/*------------------------------------------------------------------------------
 *
 * Persistent storage for the Sparse Vector Datatype
 *
 * Consists of the dimension of the vector (how many elements) and a SparseData
 * structure that stores the data in a compressed format.
 *
 * Copyright (c) 2010, Greenplum Software
 *
 * $$
 *------------------------------------------------------------------------------
 */

#ifndef SPARSEVECTOR_H
#define SPARSEVECTOR_H

#include "SparseData.h"
//#include "float_specials.h"

typedef struct {
	int4 vl_len_;
	int4 dimension; /* Number of elements in this vector, special case is
			   -1 indicates a scalar */
	char data[1]; /*
		       * We store the serialized SparseData representing the
		       * vector here.
		       */
} SvecType;

#define DatumGetSvecTypeP(X)            ((SvecType *) PG_DETOAST_DATUM(X))
#define DatumGetSvecTypePCopy(X)        ((SvecType *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_SVECTYPE_P(n)         DatumGetSvecTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_SVECTYPE_P_COPY(n) 	DatumGetSvecTypePCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_SVECTYPE_P(x)         PG_RETURN_POINTER(x)

/* Below are the locations of the SparseData values within the serialized
 * inline SparseData below the Svec header
 */
#define SVECHDRSIZE	(VARHDRSZ + sizeof(int4))
/* Beginning of the serialized SparseData */
#define SVEC_SDATAPTR(x)	((char *)(x)+SVECHDRSIZE)
#define SVEC_UNIQUE_VALCNT(x)	(SDATA_UNIQUE_VALCNT(SVEC_SDATAPTR(x)))
#define SVEC_TOTAL_VALCNT(x)	(SDATA_TOTAL_VALCNT(SVEC_SDATAPTR(x)))
#define SVEC_DATA_SIZE(x) 	(SDATA_DATA_SIZE(SVEC_SDATAPTR(x)))
#define SVEC_VALS_PTR(x)	(SDATA_VALS_PTR(SVEC_SDATAPTR(x)))
/* The size of the index is variable unlike the values, so in the serialized sparsedata
 * we include an int32 that indicates the size of the index.
 */
#define SVEC_INDEX_SIZE(x) 	(SDATA_INDEX_SIZE(SVEC_SDATAPTR(x)))
#define SVEC_INDEX_PTR(x) 	(SDATA_INDEX_PTR(SVEC_SDATAPTR(x)))

/* If the dimension is -1, this is a scalar */
#define IS_SCALAR(x)	(((x)->dimension) < 0 ? 1 : 0 )
#undef VERBOSE

static inline int check_scalar(SvecType *svec_1,SvecType *svec_2) {
	int i1=IS_SCALAR(svec_1),i2=IS_SCALAR(svec_2);
	if ((!i1) && (!i2)) {
		return(0);
	} else if (i1 && i2) {
		return(3);
	} else if (i1) {
		return(1);
	} else if (i2) {
		return(2);
	}
	return(0);
}

static inline SparseData sdata_from_svec(SvecType *svec)
{
	SparseData result = makeInplaceSparseData(
		SVEC_VALS_PTR(svec),SVEC_INDEX_PTR(svec),
		SVEC_DATA_SIZE(svec),SVEC_INDEX_SIZE(svec),FLOAT8OID,
		SVEC_UNIQUE_VALCNT(svec),SVEC_TOTAL_VALCNT(svec));
	return(result);
}

static inline void printout_svec(SvecType *svec, char *msg, int stop);
static inline void printout_svec(SvecType *svec, char *msg, int stop)
{
	printout_sdata((SparseData)SVEC_SDATAPTR(svec), msg, stop);
	elog(NOTICE,"len,dimension=%d,%d",VARSIZE(svec),svec->dimension);
}


char *svec_out_internal(SvecType *svec);
SvecType *svec_from_sparsedata(SparseData sdata);
ArrayType *svec_return_array_internal(SvecType *svec);
char *svec_out_internal(SvecType *svec);
SvecType *svec_make_scalar(float8 value, int dimension);
SvecType *svec_from_float8arr(float8 *array, int dimension);
SvecType *
op_svec_by_svec_internal(int operation, SvecType *svec1, SvecType *svec2);

//#define VERBOSE

Datum svec_in(PG_FUNCTION_ARGS);
Datum svec_out(PG_FUNCTION_ARGS);
Datum svec_return_vector(PG_FUNCTION_ARGS);
Datum svec_return_array(PG_FUNCTION_ARGS);
Datum svec_send(PG_FUNCTION_ARGS);
Datum svec_recv(PG_FUNCTION_ARGS);

// Operators
Datum svec_pow(PG_FUNCTION_ARGS);
Datum svec_equals(PG_FUNCTION_ARGS);
Datum svec_minus(PG_FUNCTION_ARGS);
Datum svec_plus(PG_FUNCTION_ARGS);
Datum svec_div(PG_FUNCTION_ARGS);
Datum svec_dot(PG_FUNCTION_ARGS);
Datum svec_l2norm(PG_FUNCTION_ARGS);
Datum svec_count(PG_FUNCTION_ARGS);
Datum svec_mult(PG_FUNCTION_ARGS);
Datum svec_log(PG_FUNCTION_ARGS);
Datum svec_l1norm(PG_FUNCTION_ARGS);

Datum float8arr_minus_float8arr(PG_FUNCTION_ARGS);
Datum svec_minus_float8arr(PG_FUNCTION_ARGS);
Datum float8arr_minus_svec(PG_FUNCTION_ARGS);
Datum float8arr_plus_float8arr(PG_FUNCTION_ARGS);
Datum svec_plus_float8arr(PG_FUNCTION_ARGS);
Datum float8arr_plus_svec(PG_FUNCTION_ARGS);
Datum float8arr_mult_float8arr(PG_FUNCTION_ARGS);
Datum svec_mult_float8arr(PG_FUNCTION_ARGS);
Datum float8arr_mult_svec(PG_FUNCTION_ARGS);
Datum float8arr_div_float8arr(PG_FUNCTION_ARGS);
Datum svec_div_float8arr(PG_FUNCTION_ARGS);
Datum float8arr_div_svec(PG_FUNCTION_ARGS);
Datum svec_dot_float8arr(PG_FUNCTION_ARGS);
Datum float8arr_dot_svec(PG_FUNCTION_ARGS);


// Casts
Datum svec_cast_int2(PG_FUNCTION_ARGS);
Datum svec_cast_int4(PG_FUNCTION_ARGS);
Datum svec_cast_int8(PG_FUNCTION_ARGS);
Datum svec_cast_float4(PG_FUNCTION_ARGS);
Datum svec_cast_float8(PG_FUNCTION_ARGS);
Datum svec_cast_float8arr(PG_FUNCTION_ARGS);
Datum svec_cast_numeric(PG_FUNCTION_ARGS);
Datum svec_unnest(PG_FUNCTION_ARGS);

#endif  /* SPARSEVECTOR_H */
