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

/*==============================================
 *
 * SINGLE VALUE HASHING
 *
 *==============================================
 */

/* 
 * HASHAPI_Hash_1_BigInt
 * Perform a simple 1-value bigint hash 
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_Hash_1_BigInt);
Datum HASHAPI_Hash_1_BigInt(PG_FUNCTION_ARGS)
{
    int32 num_segs; /* number of segments  */
	int64 val1;     /* big int input value */
    unsigned int targetbucket; /* 0-based  */
	
	/* Get number of segments */
    num_segs = PG_GETARG_INT32(0);
    
	/* Get the value to hash */
	val1 = PG_GETARG_INT64(1);
	
	Datum d1 = Int64GetDatum(val1);
	
	/* create a CdbHash for this hash test. */
    h = makeCdbHash(num_segs);
	
	/* init cdb hash */
	cdbhashinit(h);
	
	Oid oid = INT8OID;
	short typmod;
	
	cdbhash(h, d1, oid, typmod);
	
	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
	
    PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}


/* 
 * HASHAPI_Hash_1_Int
 * Perform a simple 1-value int4 hash 
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_Hash_1_Int);
Datum HASHAPI_Hash_1_Int(PG_FUNCTION_ARGS)
{
    int32 num_segs; /* number of segments  */
	int32 val1;     /* big int input value */
    unsigned int targetbucket; /* 0-based  */
	
	/* Get number of segments */
    num_segs = PG_GETARG_INT32(0);
    
	/* Get the value to hash */
	val1 = PG_GETARG_INT32(1);
	
	Datum d1 = Int32GetDatum(val1);
	
	/* create a CdbHash for this hash test. */
    h = makeCdbHash(num_segs);
	
	/* init cdb hash */
	cdbhashinit(h);
	
	Oid oid = INT4OID;
	short typmod;
	
	cdbhash(h, d1, oid, typmod);
	
	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
	
    PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}

/* 
 * HASHAPI_Hash_1_Char
 * Perform a simple 1 character hash 
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_Hash_1_BpChar);
Datum HASHAPI_Hash_1_BpChar(PG_FUNCTION_ARGS)
{
    int32 num_segs;            /* number of segments  */
	BpChar *val1;				   /* char value          */
    unsigned int targetbucket; /* 0-based             */
	
	/* Get number of segments */
    num_segs = PG_GETARG_INT32(0);
    
	/* Get the value to hash */
	val1 = PG_GETARG_BPCHAR_P(1);	
				
	Datum d1 = PointerGetDatum(val1);
	
	/* create a CdbHash for this hash test. */
    h = makeCdbHash(num_segs);
	
	/* init cdb hash */
	cdbhashinit(h);
	
	Oid oid = BPCHAROID;
	short typmod;
	
	cdbhash(h, d1, oid, typmod);
	
	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
	
    PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}

/* 
 * HASHAPI_Hash_1_Text
 * Perform a simple 1 string hash.
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_Hash_1_Text);
Datum HASHAPI_Hash_1_Text(PG_FUNCTION_ARGS)
{
    int32 num_segs;            /* number of segments  */
	text *val1;	        	   /* char value          */
    unsigned int targetbucket; /* 0-based             */
	
	/* Get number of segments */
    num_segs = PG_GETARG_INT32(0);
    
	/* Get the value to hash */
	val1 = PG_GETARG_TEXT_P(1);
	
			
	Datum d1 = PointerGetDatum(val1);
			
	/* create a CdbHash for this hash test. */
	h = makeCdbHash(num_segs);
			
	/* init cdb hash */
	cdbhashinit(h);
			
	Oid oid = TEXTOID;
	short typmod;
			
	cdbhash(h, d1, oid, typmod);
			
	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
			
	PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}

/* 
 * HASHAPI_Hash_1_Varchar
 * Perform a simple 1 string (of type VARCHAR) hash.
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_Hash_1_Varchar);
Datum HASHAPI_Hash_1_Varchar(PG_FUNCTION_ARGS)
{
    int32 num_segs;            /* number of segments  */
	VarChar    *val1;        	   /* varchar value		  */
    unsigned int targetbucket; /* 0-based             */
	
	/* Get number of segments */
    num_segs = PG_GETARG_INT32(0);
    
	/* Get the value to hash */
	val1 = PG_GETARG_VARCHAR_P(1);
	
	
	Datum d1 = PointerGetDatum(val1);
	
	/* create a CdbHash for this hash test. */
	h = makeCdbHash(num_segs);
	
	/* init cdb hash */
	cdbhashinit(h);
	
	Oid oid = VARCHAROID;
	short typmod;
	
	cdbhash(h, d1, oid, typmod);
	
	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
	
	PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}


/*==============================================
 *
 * MULTI VALUE HASHING
 *
 *==============================================
 */

/* 
 * HASHAPI_Hash_2_Int_BigInt
 * Perform a simple 1-value int4 hash 
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_Hash_2_Int_Int);
Datum HASHAPI_Hash_2_Int_Int(PG_FUNCTION_ARGS)
{
    int32 num_segs; /* number of segments  */
	int32 val1;     /* int input value     */
	int32 val2;     /* bigint input value  */
    unsigned int targetbucket; /* 0-based  */
	
	/* Get number of segments */
    num_segs = PG_GETARG_INT32(0);
    
	/* Get the values to hash */
	val1 = PG_GETARG_INT32(1);
	val2 = PG_GETARG_INT32(2);
	
	Datum d1 = Int32GetDatum(val1);
	Datum d2 = Int32GetDatum(val2);
	
	/* create a CdbHash for this hash test. */
    h = makeCdbHash(num_segs);
	
	/* init cdb hash */
	cdbhashinit(h);
	
	Oid oid1 = INT4OID;
	Oid oid2 = INT4OID;
	
	short typmod = 1; /* not used here... */
	
	cdbhash(h, d1, oid1, typmod);
	cdbhash(h, d2, oid2, typmod);

	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
	
    PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}

/* 
 * HASHAPI_Hash_2_Text_Text
 * Perform a simple 1 string hash.
 */ 
PG_FUNCTION_INFO_V1(HASHAPI_Hash_2_Text_Text);
Datum HASHAPI_Hash_2_Text_Text(PG_FUNCTION_ARGS)
{
    int32 num_segs;            /* number of segments  */
	text *val1;	        	   /* text value1         */
	text *val2;	        	   /* text value2         */
    unsigned int targetbucket; /* 0-based             */
	
	/* Get number of segments */
    num_segs = PG_GETARG_INT32(0);
    
	/* Get the value to hash */
	val1 = PG_GETARG_TEXT_P(1);
	val2 = PG_GETARG_TEXT_P(2);

	
	Datum d1 = PointerGetDatum(val1);
	Datum d2 = PointerGetDatum(val2);
	
	/* create a CdbHash for this hash test. */
	h = makeCdbHash(num_segs);
	
	/* init cdb hash */
	cdbhashinit(h);
	
	Oid oid = TEXTOID;
	short typmod;
	
	cdbhash(h, d1, oid, typmod);
	cdbhash(h, d2, oid, typmod);

	/* reduce the result hash value */
	targetbucket = cdbhashreduce(h);	
	
	PG_RETURN_INT32(targetbucket); /* return target bucket (segID) */
}




