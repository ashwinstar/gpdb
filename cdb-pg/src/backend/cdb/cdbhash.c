/*--------------------------------------------------------------------------
 *
 * cdbhash.c
 *    Rrovides hashing routines to support consistant data distribution/location within CDB.
 *
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION
 *    $Id$
 *
 *--------------------------------------------------------------------------
 */
#include "postgres.h"
#include <ctype.h>
#include <assert.h>
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "cdb/cdbhash.h"

/* Constant values used for an FNV1/FNV1A hash */
#define FNV_32_PRIME ((uint32)16777619)
#define FNV1_32_INIT ((uint32)2166136261UL)

/* FNV macros for hashing */
#define FNV1_OP(hash, octet)	( ( ((uint32)hash) * FNV_32_PRIME) ^ ((uint8)octet))
#define FNV1A_OP(hash, octet)   ( (((uint32)hash) ^ ((uint8)octet)) * FNV_32_PRIME )

/* local function declarations */
uint32 cdbhash_int2(uint32 hash, Datum datum);
uint32 cdbhash_int4(uint32 hash, Datum datum);
uint32 cdbhash_int8(uint32 hash, Datum datum);
uint32 cdbhash_bpchar(uint32 hash, Datum datum);
uint32 cdbhash_text(uint32 hash, Datum datum);
uint32 cdbhash_varchar(uint32 hash, Datum datum);
uint32 fnv_8byte(uint32 hash, int64 val);
uint32 fnv_1byte(uint32 hash, int8 val);

/*
 * Create a CdbHash for this session.
 *
 * CdbHash maintains the following information about the hash.
 * In here we set the variables that should not change in the scope of the newly created
 * CdbHash, these are:
 *
 * 1 - number of segments in CDB.
 * 2 - hashingalgorithm used.
 * 3 - reduction method.
 *
 * The hash value itself will be initialized for every tuple in cdbhashinit()
 */
CdbHash *makeCdbHash(int numsegs)
{	
	
	ereport(NOTICE, (
		errmsg("CDBHASH - Creating a CdbHash with %d buckets, using FNV hash "
			   "and modulus reduction" /* algorithm names hard coded for now... */
			   ,numsegs)
					));
			
	/*
	 * verify number of segments is legal.
	 */
	if(numsegs <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("CDBHASH - number of segments in CDB must be more than 0. got %d",						 
						numsegs)));
	
	  
	/* Create a pointer to a CdbHash that includes the hash properties */ 
	CdbHash *h = palloc(sizeof(CdbHash));
	
	/*
	 * set this hash session characteristics.
	 */
    h->hash = 0;	/* temporary initialization to 0 for testing purposes. Will change soon */
	h->numsegs = numsegs;
	h->reduce_algorithm = REDUCE_LAZYMOD; /* hard coded for now */
    h->hash_algorithm = HASH_FNV;         /* hard coded for now */
	
	return h;	
}


/*
 * Initialize CdbHash for hashing the next tuple values.
 */
void cdbhashinit(CdbHash *h)
{
	/* reset the hash value to the initial offset basis */
	h->hash = FNV1_32_INIT;
}


/*
 * Add an attribute to the hash calculation.
 */
void cdbhash(CdbHash *h, Datum datum, Oid type, short typmod)
{
	/*
	 * The current hash value (either INIT value or hash from previous attributes).
	 */
	uint32 currenthash = h->hash;
	
	/*
	 * The newly calculated hash value after adding this attribute.
	 */
	uint32 newhash;
	
	ereport(NOTICE,  (
		errmsg("CDB Hash: Adding an attribute of type %d to the hash.", (int)type) /* add more detail later */
					));
			
	/*
	 * Select the hash to be performed according to the field type we are adding to the 
	 * hash. 
	 *
	 * Before calculating the hash we make sure to convert a data type that belogs to a type
	 * group into the pre-decided type. For example, an integer type group may include types int2,int4,int8. 
	 * int2 and int4 will be cast to an int8 and hashed as an int8. 
	 *
	 * The reason for doing so is that arguments to equality omparisons that don't match on 
	 * type are never hashable by PostgreSQL (e.g., int4 = int8). PostgreSQL will never choose hash join to 
	 * implement a join on equality of an int4 attribute to an int8 attribute. It will choose another approach 
	 * and will perform the comparison using the procedure "int48eq".
	 * As of PostgreSQL 8.0 there are 55 built-in equality comparisons, each of which may or may
	 * not be a candidate for a PostgreSQL hash join. Therefore, the CDB hash function can do a better job (at the 
	 * expense of some datatype generality) by recognizing the comparability of closely related types.
	 */
	
	switch( type )
	{
	   /*=====================================================
		* integer type group: cast to 64 bit integer and hash
		*=====================================================
		*/
		case INT2OID: /* -32 thousand to 32 thousand, 2-byte storage */
			newhash = cdbhash_int2( currenthash, datum );
			break;
			
		case INT4OID: /* -2 billion to 2 billion integer, 4-byte storage */
			newhash = cdbhash_int4( currenthash, datum );
			break;
					
		case INT8OID: /* ~18 digit integer, 8-byte storage */ 
			newhash = cdbhash_int8( currenthash, datum );
			break;
						
		case BOOLOID: /* boolean, 'true'/'false' */
			//newhash = FNV_32_INT8( currenthash, datum ? 1 : 0 );
			break;
						
		case CHAROID:   /* char(1), single character */ 
		case BPCHAROID: /* char(n), blank-padded string, fixed storage length */
			newhash = cdbhash_bpchar( currenthash, datum );
			break;

		case TEXTOID: /* variable-length string, no limit specified */
			newhash = cdbhash_text( currenthash, datum );
			break;
			
		case VARCHAROID: /* varchar(length), non-blank-padded string, variable storage length */
			newhash = cdbhash_varchar( currenthash, datum );
			break;
				
		case OIDOID: /* object identifier(oid), maximum 4 billion */
			//newhash = FNV_32_UINT32( currenthash, (uint32)DatumGetObjectId(val) );
			break;
			
		case FLOAT4OID: /* single-precision floating point number, 4-byte storage */
			//newhash = FNV_32_FLOAT4( currenthash, DatumGetFloat4(val) );
			break;
			
		case FLOAT8OID: /* double-precision floating point number, 8-byte storage */
			//newhash = FNV_32_FLOAT8( currenthash, DatumGetFloat8(val) );
			break;
			
		default:
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
						errmsg("Type %u can't be hashed yet.  Sorry.", type )));
					
	}/* switch */
	

	/* update hash to the newly calculated hash value */		
	h->hash = newhash;		
			
	ereport(NOTICE,  (
		errmsg("CDB Hash: New hash value %u.", newhash) /* add more detail later */
			));
					
}

/*
 * Add a NULL attribute to the hash calculation.
 */
void cdbhashnull(CdbHash *h)
{
	ereport(NOTICE,  (
		errmsg("CDB Hash: Adding a NULL attribute to the hash.") /* add more detail later */
					));
}

/*
 * Reduce the hash to a segment number.
 */
unsigned int cdbhashreduce(CdbHash *h)
{
	ereport(NOTICE,  (
		errmsg("CDB Hash: Reducing hash value to one of %d buckets.",
			   h->numsegs) 
					));
			
	return ((h->hash) % (h->numsegs)); /* (0-based). temporary solution, enhance later */
}


/*================================================================
 * PRE-HASH-EXECUTION TYPE OPERATIONS 
 *================================================================
 */

/*
 * Hash an INT2 (int16) into a 32 bit hash code.
 */
uint32 cdbhash_int2(uint32 hash, Datum val)
{
	int64 num = (int64)DatumGetInt16(val); /* cast to 8 byte before hashing */
    return fnv_8byte(hash, (int64)val);
}

/*
 * Hash an INT4 (int32) into a 32 bit hash code.
 */
uint32 cdbhash_int4(uint32 hash, Datum val)
{
	int64 num = (int64)DatumGetInt32(val); /* cast to 8 byte before hashing */
	return fnv_8byte(hash, num);
}

/*
 * Hash an INT8 (int64) into a 32 bit hash code.
 */
uint32 cdbhash_int8(uint32 hash, Datum val)
{
	int64 num = DatumGetInt64(val);
	return fnv_8byte(hash, num);
}

/*
 * Hash a blank padded character(s) into a 32 bit hash code.
 * That includes the following PG types:
 * - CHAR
 * - CHAR(n)
 */
uint32 cdbhash_bpchar(uint32 hash, Datum datum)
{
	BpChar  *t = DatumGetBpCharP(datum);      /* a varlena struct (includes data and len)  */
	int		len;						      /* the string length                         */
	char    *data;
	int     index = 0;                        /* index into the data string                */
	
	/* get the string length */
	len = VARSIZE(t) - VARHDRSZ;
	
	/* get the string data   */
	data = VARDATA(t);				     
	
	/* if this is not a single character (but multiple characters)
	 *  look for trailing blanks and skip them in the hash calculation 
	 */
	if(len > 1)
	{
		while ( data[len - 1] == ' ')
		{
			len--;
			if(len == 1) /* if only 1 char is left, leave it alone! (the string is either empty or has 1 char) */
				break;
		}
	}
	/* ...now string length doesn't include trailing blanks */
	
	while ( index < len )
	{
		hash = fnv_1byte(hash, data[index]);
		index++;
	}
	
	return hash; 	
}

/*
 * Hash a text strings into a 32 bit hash code.
 * That includes the following PG types:
 * - TEXT
 */
uint32 cdbhash_text(uint32 hash, Datum datum)
{
	
	text	*t = DatumGetTextP(datum);   /* a varlena struct (includes data and len)  */
	int		len;						 /* the string length                         */
	char *  data;
	int     index = 0;                   /* index into the data string                */
	
	/* get the string length */
	len = VARSIZE(t) - VARHDRSZ;
	
	/* get the string data   */
	data = VARDATA(t);				     
	
	/* look for trailing blanks and skip them in the hash calculation */
	while ( data[len - 1] == ' ')
	{
		len--;
		if(len == 1) /* if only 1 char is left, leave it alone! (the string is either empty or has 1 char) */
			break;

	} 
	/* ...now string length doesn't include trailing blanks */
	
	while ( index < len )
	{
		hash = fnv_1byte(hash, data[index]);
		index++;
	}
	
	return hash; 	
	
}

/*
 * Hash a varchar into a 32 bit hash code.
 * That includes the following PG types:
 * - VARCHAR(n)
 */
uint32 cdbhash_varchar(uint32 hash, Datum datum)
{
	VarChar *t = DatumGetVarCharP(datum);/* a varlena struct (includes data and len)  */
	int		len;						 /* the string length                         */
	char *  data;
	int     index = 0;                   /* index into the data string                */
	
	/* get the string length */
	len = VARSIZE(t) - VARHDRSZ;
	
	/* get the string data   */
	data = VARDATA(t);				     
	
	/* look for trailing blanks and skip them in the hash calculation */
	while ( data[len - 1] == ' ')
	{
		len--;
		if(len == 1) /* if only 1 char is left, leave it alone! (the string is either empty or has 1 char) */
			break;
		
	} 
	/* ...now string length doesn't include trailing blanks */
	
	while ( index < len )
	{
		hash = fnv_1byte(hash, data[index]);
		index++;
	}
	
	return hash; 	
	
}



/*================================================================
 * HASH EXECUTION FOR ALL TYPES 
 *================================================================
 */

/*
 * Hash a 8 byte value into an unsigned 32bit hash.
 *
 * For every octet (8 bits) we use FNV to hash on it, and then shift to
 * the next 8 bits and hash again. We do that for all 64 bits.
 */
uint32 fnv_8byte(uint32 hash, int64 val)
{
	ereport(NOTICE,  (
		errmsg("CDB Hash: hash value before operation is %u", hash) /* add more detail later */
			          ));
			
	hash = FNV1_OP(hash, val);			
	val >>= 8;
	hash = FNV1_OP(hash, val);
	val >>= 8;
	hash = FNV1_OP(hash, val);
	val >>= 8;
	hash = FNV1_OP(hash, val);
	val >>= 8;
	hash = FNV1_OP(hash, val);
	val >>= 8;
	hash = FNV1_OP(hash, val);
	val >>= 8;
	hash = FNV1_OP(hash, val);
	val >>= 8;
	hash = FNV1_OP(hash, val);
			
	return hash;
}

/*
 * Hash a single byte value into an unsigned 32bit hash.
 *
 * We use a single FNV operation to hash on it.
 */
uint32 fnv_1byte(uint32 hash, int8 val)
{
	ereport(NOTICE,  (
		errmsg("CDB Hash: hash value before operation is %u", hash) /* add more detail later */
					));
	
	/* execute the hash on our octet */		
	hash = FNV1_OP(hash, val);			
			
    return hash;
}




