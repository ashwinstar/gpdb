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
#include "utils/numeric.h"
#include "fmgr.h"

#include "cdb/cdbhash.h"

/* Constant values used for an FNV1/FNV1A hash */
#define FNV_32_PRIME ((uint32)16777619)
#define FNV1_32_INIT ((uint32)2166136261UL)

/* FNV macros for hashing */
#define FNV1_OP(hash, octet)	( ( ((uint32)hash) * FNV_32_PRIME) ^ ((uint8)octet))
#define FNV1A_OP(hash, octet)   ( (((uint32)hash) ^ ((uint8)octet)) * FNV_32_PRIME )

/* Fast mod using a bit mask, assuming that y is a power of 2 */
#define FASTMOD(x,y)			((x) & ((y)-1))

/* local function declarations */
uint32 cdbhash_int2(uint32 hash, Datum datum, int16 algorithm);
uint32 cdbhash_int4(uint32 hash, Datum datum, int16 algorithm);
uint32 cdbhash_int8(uint32 hash, Datum datum, int16 algorithm);
uint32 cdbhash_bpchar(uint32 hash, Datum datum, int16 algorithm);
uint32 cdbhash_text(uint32 hash, Datum datum, int16 algorithm);
uint32 cdbhash_varchar(uint32 hash, Datum datum, int16 algorithm);
uint32 cdbhash_float8(uint32 hash, Datum datum, int16 algorithm);
uint32 cdbhash_float4(uint32 hash, Datum datum, int16 algorithm);
uint32 fnv_8byte(uint32 hash, int64 val, int16 algorithm);
uint32 fnv_1byte(uint32 hash, int8 val, int16 algorithm);
int ignoreblanks(char *data, int len);
int ispowof2(int numsegs);


/*================================================================
 *
 * HASH API FUNCTIONS
 *
 *================================================================
 */

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
CdbHash *makeCdbHash(int numsegs, int16 algorithm)
{	
	
	ereport(NOTICE, (
		errmsg("CDBHASH - Creating a CdbHash with %d buckets, using FNV hash ", numsegs)
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
    h->hash = 0;
	h->numsegs = numsegs;
	
	/*
	 * set the reduction algorithm
	 * If num_segs is power of 2 use bit mask, else
	 * use lazy mod (h mod n)
	 */
	if(ispowof2(numsegs))
	{
		h->reduce_algorithm = REDUCE_BITMASK;
	}
	else
	{
		h->reduce_algorithm = REDUCE_LAZYMOD;
	}
	
	/*
	 * Set the hashing algoritm
	 */
	if((algorithm != HASH_FNV_1) && (algorithm != HASH_FNV_1A)) /* invalid choice */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("CDBHASH - Invalid choice for a cdb hash algorithm. got %d",algorithm),
				 errhint("Currently the following algorithms are supported: HASH_FNV_1, HASH_FNV_1A")
				 ));
	else 
		h->hash_algorithm = algorithm;
	
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
	
	/*
	 * Hashing algorithm to be used.
	 */
	int16 algorithm = h->hash_algorithm;
	
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
		* integer type group
		*=====================================================
		*/
		case INT2OID: /* -32 thousand to 32 thousand, 2-byte storage */
			newhash = cdbhash_int2( currenthash, datum, algorithm );
			break;
			
		case INT4OID: /* -2 billion to 2 billion integer, 4-byte storage */
			newhash = cdbhash_int4( currenthash, datum, algorithm );
			break;
					
		case INT8OID: /* ~18 digit integer, 8-byte storage */ 
			newhash = cdbhash_int8( currenthash, datum, algorithm );
			break;
						
	   /*=====================================================
		* character string type group
		*=====================================================
		*/
		case CHAROID:   /* char(1), single character */ 
		case BPCHAROID: /* char(n), blank-padded string, fixed storage length */
			newhash = cdbhash_bpchar( currenthash, datum, algorithm );
			break;

		case TEXTOID: /* variable-length string, no limit specified */
			newhash = cdbhash_text( currenthash, datum, algorithm );
			break;
			
		case VARCHAROID: /* varchar(length), non-blank-padded string, variable storage length */
			newhash = cdbhash_varchar( currenthash, datum, algorithm );
			break;
		
	   /*=====================================================
		* floating point type group
		*=====================================================
		*/
		case FLOAT4OID: /* single-precision floating point number, 4-byte storage */
			newhash = cdbhash_float4( currenthash, datum, algorithm );
			break;
			
		case FLOAT8OID: /* double-precision floating point number, 8-byte storage */
			newhash = cdbhash_float8( currenthash, datum, algorithm );
			break;
		
	   /*=====================================================
		* other types
		*=====================================================
		*/
		case NUMERICOID: 
		case BOOLOID: /* boolean, 'true'/'false' */
		case OIDOID:  /* object identifier(oid), maximum 4 billion */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("Type %u can't be hashed yet.", type)));
					
	}/* switch(type) */
	

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
	int result = 0; /* TODO: what is a good initialization value? could we 
					   guarantee at this point that there will not be a negative 
					   segid in CDB and therefore initialize to this value for error
					   checking? */
	
	ereport(NOTICE,  (
		errmsg("CDB Hash: Reducing hash value to one of %d segments.",
			   h->numsegs) 
					));
	
	/*
	 * Reduce our 32-bit hash value to a segment number
	 */
	if(h->reduce_algorithm == REDUCE_BITMASK)
	{	
		result = FASTMOD(h->hash, (uint32)h->numsegs); /* fast mod (bitmask) */
	} 
	else if(h->reduce_algorithm == REDUCE_LAZYMOD)
	{
		result = (h->hash) % (h->numsegs); /* simple mod */
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("CDBHASH - Reduction method not set to neither BITMASK or LAZYMOD. Value read is: %d",						 
						h->reduce_algorithm)));
			
	return result;
}


/*================================================================
 *
 * PRE-HASH TYPE-SPECIFIC OPERATIONS 
 *
 *================================================================
 */

/*
 * Hash an INT2 (int16) into a 32 bit hash code.
 */
uint32 cdbhash_int2(uint32 hash, Datum val, int16 algo)
{
	int64 num = (int64)DatumGetInt16(val); /* cast to 8 byte before hashing */
    return fnv_8byte(hash, num, algo);
}

/*
 * Hash an INT4 (int32) into a 32 bit hash code.
 */
uint32 cdbhash_int4(uint32 hash, Datum val, int16 algo)
{
	int64 num = (int64)DatumGetInt32(val); /* cast to 8 byte before hashing */
	return fnv_8byte(hash, num, algo);
}

/*
 * Hash an INT8 (int64) into a 32 bit hash code.
 */
uint32 cdbhash_int8(uint32 hash, Datum val, int16 algo)
{
	int64 num = DatumGetInt64(val);
	return fnv_8byte(hash, num, algo);
}

/*
 * Hash a blank padded character(s) into a 32 bit hash code.
 * That includes the following PG types:
 * - CHAR
 * - CHAR(n)
 */
uint32 cdbhash_bpchar(uint32 hash, Datum datum, int16 algo)
{
	BpChar  *t = DatumGetBpCharP(datum);      /* a varlena struct (includes data and len)  */
	int		len;						      /* the string length                         */
	char    *data;
	int     index = 0;                        /* index into the data string                */
	
	/* get the string length */
	len = VARSIZE(t) - VARHDRSZ;
	
	/* get the string data   */
	data = VARDATA(t);		   
	
	/* adjust length to not include trailing blanks */
	if(len > 1)
		len = ignoreblanks(data, len); 
	
	while ( index < len )
	{
		hash = fnv_1byte(hash, data[index], algo);
		index++;
	}
	
	
	return hash; 	
}

/*
 * Hash a text strings into a 32 bit hash code.
 * That includes the following PG types:
 * - TEXT
 */
uint32 cdbhash_text(uint32 hash, Datum datum, int16 algo)
{
	
	text	*t = DatumGetTextP(datum);   /* a varlena struct (includes data and len)  */
	int		len;						 /* the string length                         */
	char *  data;
	int     index = 0;                   /* index into the data string                */
	
	/* get the string length */
	len = VARSIZE(t) - VARHDRSZ;
	
	/* get the string data   */
	data = VARDATA(t);				     
	
	/* adjust length to not include trailing blanks */
	if(len > 1)
		len = ignoreblanks(data, len); 
	
	while ( index < len )
	{
		hash = fnv_1byte(hash, data[index], algo);
		index++;
	}
	
	return hash; 	
}

/*
 * Hash a varchar into a 32 bit hash code.
 * That includes the following PG types:
 * - VARCHAR(n)
 */
uint32 cdbhash_varchar(uint32 hash, Datum datum, int16 algo)
{
	VarChar *t = DatumGetVarCharP(datum);/* a varlena struct (includes data and len)  */
	int		len;						 /* the string length                         */
	char *  data;
	int     index = 0;                   /* index into the data string                */
	
	/* get the string length */
	len = VARSIZE(t) - VARHDRSZ;
	
	/* get the string data   */
	data = VARDATA(t);				     
	
    /* adjust length to not include trailing blanks */
	if(len > 1)
       len = ignoreblanks(data, len); 
	
	while ( index < len )
	{
		hash = fnv_1byte(hash, data[index], algo);
		index++;
	}
	
	return hash; 	
}

/*
 * Hash a float8 into a 32 bit hash code.
 * That includes the following PG types:
 * - FLOAT8
 */
uint32 cdbhash_float8(uint32 hash, Datum datum, int16 algo)
{
	ereport(NOTICE,  (
		errmsg("CDB Hash: hash value before operation is %u", hash) /* add more detail later */
				));
			
	float8 num = DatumGetFloat8(datum);
	
   /*
	* On IEEE-float machines, minus zero and zero have different bit
	* patterns but should compare as equal.  We must ensure that they
	* have the same hash value, which is most easily done this way:
	*/
	if (num == (float8) 0)
		num = 0.0;
			
	char *val = (char *)&num;
	int index = 0;
	int len = sizeof(float8);
				
	while(index < len)
	{
		hash = fnv_1byte(hash, val[index], algo);
		index++;
	}
			
	return hash;
	
}

/*
 * Hash a float4 into a 32 bit hash code.
 * That includes the following PG types:
 * - FLOAT8
 */
uint32 cdbhash_float4(uint32 hash, Datum datum, int16 algo)
{
	ereport(NOTICE,  (
					  errmsg("CDB Hash: hash value before operation is %u", hash) /* add more detail later */
							 ));
			
	float4 num = DatumGetFloat4(datum);
			
   /*
	* On IEEE-float machines, minus zero and zero have different bit
	* patterns but should compare as equal.  We must ensure that they
	* have the same hash value, which is most easily done this way:
	*/
	if (num == (float4) 0)
		num = 0.0;
			
	char *val = (char *)&num;
	int index = 0;
	int len = sizeof(float4);
			
	while(index < len)
	{
		hash = fnv_1byte(hash, val[index], algo);
		index++;
	}
			
	return hash;
}

/*================================================================
 *
 * HASH EXECUTION FOR ALL TYPES 
 *
 *================================================================
 */

/*
 * Hash a 8 byte value into an unsigned 32bit hash.
 *
 * For every octet (8 bits) we use FNV to hash on it, and then shift to
 * the next 8 bits and hash again. We do that for all 64 bits.
 */
uint32 fnv_8byte(uint32 hash, int64 val, int16 algo)
{
	ereport(NOTICE,  (
		errmsg("CDB Hash: hash value before operation is %u", hash) /* add more detail later */
			          ));
	
	if(algo == HASH_FNV_1)
	{
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
	}
	else
	{
		hash = FNV1A_OP(hash, val);			
		val >>= 8;
		hash = FNV1A_OP(hash, val);
		val >>= 8;
		hash = FNV1A_OP(hash, val);
		val >>= 8;
		hash = FNV1A_OP(hash, val);
		val >>= 8;
		hash = FNV1A_OP(hash, val);
		val >>= 8;
		hash = FNV1A_OP(hash, val);
		val >>= 8;
		hash = FNV1A_OP(hash, val);
		val >>= 8;
		hash = FNV1A_OP(hash, val);
	}
			
	return hash;
}

/*
 * Hash a single byte value into an unsigned 32bit hash.
 */
uint32 fnv_1byte(uint32 hash, int8 val, int16 algo)
{
	ereport(NOTICE,  (
		errmsg("CDB Hash: hash value before operation is %u", hash) /* add more detail later */
					));
	
	/* execute the hash on our octet */		
	if(algo == HASH_FNV_1)
			hash = FNV1_OP(hash, val);			
	else
			hash = FNV1A_OP(hash, val);
			
    return hash;
}

/*================================================================
 *
 * GENERAL PURPOSE UTILS 
 *
 *================================================================
 */

/*
 * Given the original length of the data array this function is 
 * recalculating the length after ignoring any trailing blanks. The
 * actual data is remained unmodified.
 */
int ignoreblanks(char *data, int len)
{
	
    /* look for trailing blanks and skip them in the hash calculation */
    while ( data[len - 1] == ' ')
    {
	    len--;
	    if(len == 1) /* if only 1 char is left, leave it alone! (the string is either empty or has 1 char) */
		    break;
	}

    return len;

}

/*
 * returns 1 is the input int is a power of 2 and 0 otherwize.
 */
int ispowof2(int numsegs)
{
	if ( numsegs == 0 ) 
		return 0;
	
    while( numsegs % 2 == 0 ) 
    {
        numsegs >>= 1; /* effectively divide by 2 */
    }
    
	return (numsegs == 1) ? 1 : 0;
}

