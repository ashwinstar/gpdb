/*-------------------------------------------------------------------------
*
* hashapi_access.c
*     Test functions for accessing the Cdb Hash API.
*
* Copyright (c) 2005, Metapa
*
* IDENTIFICATION
*     $Id$
*
*-------------------------------------------------------------------------
*/
#include "postgres.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/memutils.h"
#include "cdb/cdbhash.h"
#include "cdbtest.h"

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

/*
 * Pointer to the hash session characteristics. 
 */
static CdbHash *h;

/* 
 * HASHAPI_HashText__int1
 * Perform a simple 1-value text hash 
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_HashText__int1);
Datum HASHAPI_HashText__int1(PG_FUNCTION_ARGS)
{
    int num_segs;
    unsigned int targetbucket; /* 0-based */
	
    /* Motion node ID */
    num_segs = PG_GETARG_INT32(0);
	
	/* create a CdbHash for this hash test. */
    h = makeCdbHash(num_segs);
	
	/* todo: add hash values here... */
	
	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
	
    PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}
