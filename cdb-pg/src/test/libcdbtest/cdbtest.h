/*-------------------------------------------------------------------------
 * cdbtest.h
 *     Declarations of functions in the cdbtest library.
 *
 * Copyright (c) 2005, Metapa
 *-------------------------------------------------------------------------
 */
#ifndef CDBTEST_H
#define CDBTEST_H

/* These functions are defined in chunkfuncs.c. */

extern Datum GetSerializedRows__text1(PG_FUNCTION_ARGS);
extern Datum GetChunkedRows__text1(PG_FUNCTION_ARGS);

extern Datum GetDeserializedRows__text1_regtype1(PG_FUNCTION_ARGS);
extern Datum GetDechunkedRows__text1_regtype1(PG_FUNCTION_ARGS);

/* These functions are defined in ipcfuncs.c */
extern Datum ml_ipc_getMsg__text1(PG_FUNCTION_ARGS);


#endif /* CDBTEST_H */
