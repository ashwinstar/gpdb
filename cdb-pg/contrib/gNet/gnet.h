#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

/* Functions defined in bayes.c */
extern Datum nb_classify_accum(PG_FUNCTION_ARGS);
extern Datum nb_classify_combine(PG_FUNCTION_ARGS);
extern Datum nb_classify_final(PG_FUNCTION_ARGS);

/* Functions defined in matrix.c */
extern Datum matrix_transpose(PG_FUNCTION_ARGS);
extern Datum matrix_multiply(PG_FUNCTION_ARGS);
extern Datum matrix_add(PG_FUNCTION_ARGS);
extern Datum int8_matrix_smultiply(PG_FUNCTION_ARGS);
extern Datum float8_matrix_smultiply(PG_FUNCTION_ARGS);

/* functions defined in pinv.c */
extern Datum pseudoinverse(PG_FUNCTION_ARGS);

/* Functions defined in regress.c */
extern Datum float8_mregr_accum(PG_FUNCTION_ARGS);
extern Datum float8_mregr_combine(PG_FUNCTION_ARGS);
extern Datum float8_mregr_coef(PG_FUNCTION_ARGS);
extern Datum float8_mregr_r2(PG_FUNCTION_ARGS);
