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

/* These functions are defined in mlapi_access.c */

extern Datum MLAPI_InitMLNode__int1_bool1_regtype1(PG_FUNCTION_ARGS);
extern Datum MLAPI_EndMLNode__int1(PG_FUNCTION_ARGS);

extern Datum MLAPI_SendTuple__int1_text1(PG_FUNCTION_ARGS);
extern Datum MLAPI_FinishSendTuple(PG_FUNCTION_ARGS);
extern Datum MLAPI_RecvTuple__int1(PG_FUNCTION_ARGS);
extern Datum MLAPI_GetLastRecvCode(PG_FUNCTION_ARGS);

/* These functions are defined in hashapi_access.c */

extern Datum HASHAPI_HashText__int1(PG_FUNCTION_ARGS);


#endif /* CDBTEST_H */
