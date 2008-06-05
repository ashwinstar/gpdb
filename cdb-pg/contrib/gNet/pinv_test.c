#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

//We store our 2D data in column fastest order
#define index2d(i,j,nj)  (j+ nj*i)
#define index3d(i,j,k,nj,nk)  (k+ nk*j+ nk*nj*i)

#define max(a,b) (a > b ? a : b)
#define min(a,b) (a < b ? a : b)
#define ABS(a) (a < 0 ? -a : a)

PG_FUNCTION_INFO_V1(pinv_test);
Datum pinv_test(PG_FUNCTION_ARGS);

ArrayType *pinv(int rows, int columns, float8 *A, float8 *Aplus);

ArrayType *pinv_test_alloc_2darray(int rows, int columns, char *copyfrom, int elemtype);
void printmat(int m, int n, float8 *matrix, char *name);
float8 *pinv_matmult(int rowsA, int columnsA_rowsB, int columnsB, float8 *A, float8 *B);
float8 *pinv_transpose(int rows, int columns, float8 *A);
float8 *pinv_matsub(int rows, int columns, float8 *A, float8 *B);
int pinv_testmatiszero(int rows, int columns, float8 *Z, char *test);
int pinv_test_internal(int m, int n, float8 *A);
int pinv_tests(void);
int pinv_coverage_pinv_tests(int maxrows, int maxcolumns);

/* If copyfrom is non-null, copy the data from it into a newly allocated data area
 * Otherwise, initialize the data area to zero
 */
ArrayType *pinv_test_alloc_2darray(int rows, int columns, char *copyfrom, int elemtype) {
	int size,elemsize=0;
	ArrayType *result;
	char *data_r;

    size = ARR_OVERHEAD_NONULLS(2);  /* overhead for two dimensional array */
    switch (elemtype) {
        case INT2OID:
			elemsize = sizeof(int16);
            break;
        case INT4OID:
            elemsize = sizeof(int32);
            break;
        case INT8OID:
            elemsize = sizeof(int64);
            break;
        case FLOAT4OID:
            elemsize = sizeof(float4);
            break;
        case FLOAT8OID:
            elemsize = sizeof(float8);
            break;
        default:
            Assert(false);
    }
	size += rows * columns * elemsize;

    result = (ArrayType *) palloc(size);
    SET_VARSIZE(result, size);
    result->ndim = 2;
    result->dataoffset = 0;          /* We dissallowed arrays with NULLS */
    ARR_DIMS(result)[0] = rows;
    ARR_DIMS(result)[1] = columns;
    ARR_LBOUND(result)[0] = 1;       /* may need better lbound handling? */
    ARR_LBOUND(result)[1] = 1;

    result->elemtype = elemtype;

	elog(NOTICE,"Element size = %d, Rows = %d, Columns = %d",elemsize,rows,columns);
    data_r = ARR_DATA_PTR(result);
	if (copyfrom == NULL) {
      memset(data_r, 0, rows * columns * elemsize);
	} else {
	  memcpy(data_r,copyfrom,rows * columns * elemsize);
	}

	return (result);
}


void printmat(int m, int n, float8 *matrix, char *name) {
  int i,j;
  // Print in column first order
	elog(NOTICE,"--------------------------------");
  for (i=0; i<m; i++) {
    for (j=0; j<n; j++) {
			elog(NOTICE," %s[%d,%d] = %4.2e ",name,i+1,j+1,matrix[j+n*i]);
	}
  }
  elog(NOTICE,"--------------------------------");
}


float8 *pinv_transpose(int rows, int columns, float8 *A) {
  int i,j;
  float8 *B =(float8 *)palloc(sizeof(float8)*rows*columns);

  for (i=0; i<rows; i++) {
		  for (j=0; j<columns; j++) {
				  B[index2d(j,i,rows)] = A[index2d(i,j,columns)];
  } }

  return (B);

}

float8 *pinv_matmult(int rowsA, int columnsA_rowsB, int columnsB, float8 *A, float8 *B) {
  int i,j,k;
  float8 *result =(float8 *)palloc(sizeof(float8)*rowsA*columnsB);
  float8 aa;

  for (i=0;i<rowsA;i++) {
    for (j=0;j<columnsB;j++) {
      aa = 0.;
      for (k=0;k<columnsA_rowsB;k++) {
        aa += A[index2d(i,k,columnsA_rowsB)] * B[index2d(k,j,columnsB)];
      }
      result[index2d(i,j,columnsB)] = aa;
  } }

  return result;

}

float8 *pinv_matsub(int rows, int columns, float8 *A, float8 *B) {
  int i;
  float8 *result = (float8 *)palloc(sizeof(float8)*rows*columns);
  for (i=0;i<rows*columns;i++) result[i] = A[i] - B[i];
  return(result);
}

int pinv_testmatiszero(int rows, int columns, float8 *Z, char *test) {
  int i,iszero;
  float8 result;
  float8 epsilon = pow(2,1-56); //Machine epsilon
  float8 tolerance = epsilon*pow(max(rows,columns),3)*10000.;

  result = 0.;
  iszero = 1;

  for (i=0;i<rows*columns;i++) {
    if (ABS(Z[i]) > tolerance ) {
      iszero=0;
      result=max(result,ABS(Z[i]));
    }
  }

  if (iszero) {
//    elog(NOTICE,"Test succeeds: pseudo inverse is correct for %s test using tolerance = %4.2e.",test,tolerance);
  } else {
    elog(NOTICE,"Test fails: pseudo inverse is not correct for %s test., max tolerance = %4.2e",test,result);
    elog(NOTICE,"Note that this test has an arbitrary tolerance for \"zero\", so you should evaluate this result to see if it's \"close enough\"/n");
    printmat(rows,columns,Z,"Z");
  }

  return(iszero);
}

int pinv_test_internal(int m, int n, float8 *A) {
  int success[4]={1,1,1,1};
  int i;
  float8 *Aplus, *Aplus_A, *A_Aplus_A, *Aplus_A_Aplus, *A_Aplus, *A_Aplus_t, *Aplus_A_t;
  float8 *zero;

  //Convert the incoming float8 * array to a PG array
//  A_PG = pinv_test_alloc_2darray(m,n,(char *)A,FLOAT8OID);
//  printmat(m,n,(float8 *)ARR_DATA_PTR(A_PG),"A_PG");

  //Calculate the pseudo inverse of A, returns a PG array
  Aplus = (float8 *)palloc(sizeof(float8)*m*n);
  pinv(m,n,A,Aplus);

//* Test #1 of the pseudo inverse: A x ( A+ x A ) = A
  Aplus_A = pinv_matmult(n,m,n,Aplus,A);
  A_Aplus_A = pinv_matmult(m,n,n,A,Aplus_A);
  zero=pinv_matsub(m,n,A_Aplus_A,A);
  success[0] = pinv_testmatiszero(m,n,zero,"A_A+_A - A");
  pfree(zero);

//* Test #2 of the pseudo inverse: A+ x A x A+ = A+
  Aplus_A_Aplus = pinv_matmult(n,n,m,Aplus_A,Aplus);
  zero=pinv_matsub(m,n,Aplus_A_Aplus,Aplus);
  success[1] = pinv_testmatiszero(m,n,zero,"A+_A_A+ - A+");
  pfree(zero);

//* Test #3 of the pseudo inverse: (A x A+)t = A x A+
  A_Aplus = pinv_matmult(m,n,m,A,Aplus);
  A_Aplus_t = pinv_transpose(m,m,A_Aplus);
  zero=pinv_matsub(m,m,A_Aplus_t,A_Aplus);
  success[2] = pinv_testmatiszero(m,m,zero,"(A_A+)t - A_A+");
  pfree(zero);

//* Test #4 (and last test) of the pseudo inverse: (A+ x A)t = A+ x A
  Aplus_A_t = pinv_transpose(n,n,Aplus_A);
  zero=pinv_matsub(n,n,Aplus_A_t,Aplus_A);
  success[3] = pinv_testmatiszero(n,n,zero,"(A+_A)t - A+_A");
  pfree(zero);

  pfree(Aplus_A); pfree(A_Aplus_A); pfree(Aplus_A_Aplus); pfree(A_Aplus); pfree(A_Aplus_t);
  pfree(Aplus_A_t);
  pfree(Aplus);

  for (i=1;i<4;i++) success[0] = min(success[0],success[i]);

  return(success[0]);
}

int pinv_tests(void) {
  int i,success[4];
  float8 matrix1[] = { 1,1,0, 1,1,0 };
  float8 matrix2[] = { 1,2, 1,2 };
  float8 matrix3[] = { 1,2, 3,4 };
  float8 matrix4[] = { 1,2, 3,4, 5,6, 7,8 };
  success[0] = pinv_test_internal(2,3,matrix1);
  success[1] = pinv_test_internal(2,2,matrix2);
  success[2] = pinv_test_internal(2,2,matrix3);
  success[3] = pinv_test_internal(4,2,matrix4);
  for (i=1;i<4;i++) success[0] = min(success[0],success[i]);

  return(success[0]);
}
  
int pinv_coverage_pinv_tests(int maxrows, int maxcolumns) {
  float8 *matrix;
  int i,j;
  int success=1;

  matrix = (float8 *)palloc(sizeof(float8)*maxrows*maxcolumns);
  for (i=0;i<maxrows;i++) {
    for (j=0;j<maxcolumns;j++) {
      matrix[j+i*maxcolumns] = (i+1)*(j+1);
  } }

  for (i=1;i<maxrows+1;i++) {
    for (j=1;j<maxcolumns+1;j++) {
//      elog(NOTICE,"rows = %d, columns = %d",i,j);
      success = min(success,pinv_test_internal(i,j,matrix));
  } }

  return(success);
}

/*  This should be defined with a BOOL return type and VOID arguments */
Datum pinv_test(PG_FUNCTION_ARGS) {
  int mm=50;
  if (! pinv_tests()) {
		  elog(NOTICE,"Failed basic pinv_tests, stopping.");
		  PG_RETURN_BOOL(false);
  }

  elog(NOTICE,"Running coverage pinv_tests:");
  if (! pinv_coverage_pinv_tests(mm,mm)) {
		  elog(NOTICE,"Failed coverage pinv_tests, stopping.");
		  PG_RETURN_BOOL(false);
  }

  elog(NOTICE,"\n\n Finished running %d pinv_tests.",mm*mm);
  PG_RETURN_BOOL(true);
}
