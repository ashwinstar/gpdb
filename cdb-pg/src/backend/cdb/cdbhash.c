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

/*
 * 32 bit FNV-1 and FNV-1a non-zero initial basis
 * NOTE: The FNV-1a initial basis is the same value as FNV-1 by definition.
 */
#define FNV1_32_INIT ((uint32)0x811c9dc5)
#define FNV1_32A_INIT FNV1_32_INIT

/* Constant prime value used for an FNV1/FNV1A hash */
#define FNV_32_PRIME ((uint32)0x01000193)

/* Fast mod using a bit mask, assuming that y is a power of 2 */
#define FASTMOD(x,y)		((x) & ((y)-1))

/* local function declarations */
int ignoreblanks(char *data, int len);
int ispowof2(int numsegs);
uint32 fnv1_32_buf(void *buf, size_t len, uint32 hashval);
uint32 fnv1a_32_buf(void *buf, size_t len, uint32 hashval);


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
CdbHash *makeCdbHash(int numsegs, CdbHashAlg algorithm)
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
	 * Set the hashing algoritm
	 */
	assert( algorithm == HASH_FNV_1 || algorithm == HASH_FNV_1A );

	h->hashalg = algorithm;
	 
	if (h->hashalg == HASH_FNV_1)
		h->hashfn = &fnv1_32_buf;
	else if(h->hashalg == HASH_FNV_1A)
		h->hashfn = &fnv1a_32_buf;
	
	/*
	 * set the reduction algorithm
	 * If num_segs is power of 2 use bit mask, else
	 * use lazy mod (h mod n)
	 */
	if(ispowof2(numsegs))
	{
		h->reducealg = REDUCE_BITMASK;
	}
	else
	{
		h->reducealg = REDUCE_LAZYMOD;
	}
	
	
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
void cdbhash(CdbHash *h, Datum datum, Oid type)
{
	
	void *buf;    /* pointer to the data */
	size_t len;   /* length for the data buffer */
	int64 intbuf; /* an 8 byte buffer for all integer sizes */
	BpChar  *bp;  /* a variable length struct (varlena) for blank padded strings */
	text *txt;    /* a variable length struct (varlena) for text */
	VarChar *vc;  /* a variable length struct (varlena) for varchar */
	float4 buf_f4;    
	float8 buf_f8;
	
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
			intbuf = (int64)DatumGetInt16(datum); /* cast to 8 byte before hashing */
			buf = &intbuf;
			len = sizeof(intbuf);
			break;
			
		case INT4OID: /* -2 billion to 2 billion integer, 4-byte storage */
			intbuf = (int64)DatumGetInt32(datum); /* cast to 8 byte before hashing */
			buf = &intbuf;
			len = sizeof(intbuf);
			break;
					
		case INT8OID: /* ~18 digit integer, 8-byte storage */ 
			intbuf = DatumGetInt64(datum); /* cast to 8 byte before hashing */
			buf = &intbuf;
			len = sizeof(intbuf);
			break;
						
	   /*=====================================================
		* character string type group
		*=====================================================
		*/
		case CHAROID:   /* char(1), single character */ 
		case BPCHAROID: /* char(n), blank-padded string, fixed storage length */
			bp = DatumGetBpCharP(datum);      
			len = VARSIZE(bp) - VARHDRSZ; /* get the string length */
			buf = VARDATA(bp);  /* get the string data buf */
			
			/* adjust length to not include trailing blanks */
			if(len > 1)
				len = ignoreblanks((char *)buf, len); 
			break;

		case TEXTOID: /* variable-length string, no limit specified */
	        txt = DatumGetTextP(datum);   /* a varlena struct (includes data and len)  */
			
			/* get the string length */
			len = VARSIZE(txt) - VARHDRSZ;
			
			/* get the string data   */
			buf = VARDATA(txt);		   
			
			/* adjust length to not include trailing blanks */
			if(len > 1)
				len = ignoreblanks((char *)buf, len); 
			break;
			
		case VARCHAROID: /* varchar(length), non-blank-padded string, variable storage length */
			vc = DatumGetVarCharP(datum);/* a varlena struct (includes data and len)  */
				
			/* get the string length */
			len = VARSIZE(vc) - VARHDRSZ;
				
			/* get the string data   */
			buf = VARDATA(vc);		   
				
			/* adjust length to not include trailing blanks */
			if(len > 1)
				len = ignoreblanks((char *)buf, len); 
			break;
		
	   /*=====================================================
		* floating point type group
		*=====================================================
		*/
		case FLOAT4OID: /* single-precision floating point number, 4-byte storage */
			buf_f4 = DatumGetFloat4(datum);
			
			/*
			 * On IEEE-float machines, minus zero and zero have different bit
			 * patterns but should compare as equal.  We must ensure that they
			 * have the same hash value, which is most easily done this way:
			 */
			if (buf_f4 == (float4) 0)
				buf_f4 = 0.0;
				
			buf = &buf_f4;
			len = sizeof(buf_f4);
			break;
			
		case FLOAT8OID: /* double-precision floating point number, 8-byte storage */
			buf_f8 = DatumGetFloat8(datum);
				
			/*
			 * On IEEE-float machines, minus zero and zero have different bit
			 * patterns but should compare as equal.  We must ensure that they
			 * have the same hash value, which is most easily done this way:
			 */
			if (buf_f8 == (float8) 0)
				buf_f8 = 0.0;
					
				buf = &buf_f8;
			len = sizeof(buf_f8);
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
					(errcode(ERRCODE_CDB_FEATURE_NOT_YET),
						errmsg("Type %u can't be hashed yet.", type)));
					
	}/* switch(type) */
	
    /* do the hash using the selected algorithm */
    h->hash = (h->hashfn)(buf, len, h->hash); 
	
	ereport(NOTICE,  (
		errmsg("CDB Hash: New hash value %u.", h->hash) /* add more detail later */
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
	assert( h->reducealg == REDUCE_BITMASK ||  h->reducealg == REDUCE_LAZYMOD);
	
	int result = 0; /* TODO: what is a good initialization value? could we 
					   guarantee at this point that there will not be a negative 
					   segid in CDB and therefore initialize to this value for error
					   checking? */
	
	/*
	 * Reduce our 32-bit hash value to a segment number
	 */
	switch(h->reducealg)
	{
		case REDUCE_BITMASK:
			result = FASTMOD(h->hash, (uint32)h->numsegs); /* fast mod (bitmask) */
			break;
			
		case REDUCE_LAZYMOD:
			result = (h->hash) % (h->numsegs); /* simple mod */
			break;
	}
	
	return result;
}


/*================================================================
 *
 * HASH EXECUTION FOR ALL TYPES 
 *
 *================================================================
 */


/*
 * fnv1_32_buf - perform a 32 bit FNV 1 hash on a buffer
 *
 * input:
 *	buf	- start of buffer to hash
 *	len	- length of buffer in octets (bytes)
 *	hval	- previous hash value or FNV1_32_INIT if first call.
 *
 * returns:
 *	32 bit hash as a static hash type
 */
uint32
fnv1_32_buf(void *buf, size_t len, uint32 hval)
{
    unsigned char *bp = (unsigned char *)buf;	/* start of buffer */
    unsigned char *be = bp + len;		/* beyond end of buffer */
	
    /*
     * FNV-1 hash each octet in the buffer
     */
    while (bp < be) {
		
		/* multiply by the 32 bit FNV magic prime mod 2^32 */
#if defined(NO_FNV_GCC_OPTIMIZATION)
		hval *= FNV_32_PRIME;
#else
		hval += (hval<<1) + (hval<<4) + (hval<<7) + (hval<<8) + (hval<<24);
#endif
		
		/* xor the bottom with the current octet */
		hval ^= (uint32)*bp++;
    }
	
    /* return our new hash value */
    return hval;
}

/*
 * fnv1a_32_buf - perform a 32 bit FNV 1A hash on a buffer
 *
 * input:
 *	buf	- start of buffer to hash
 *	len	- length of buffer in octets (bytes)
 *	hval	- previous hash value or FNV1_32_INIT if first call.
 *
 * returns:
 *	32 bit hash as a static hash type
 */
uint32
fnv1a_32_buf(void *buf, size_t len, uint32 hval)
{
    unsigned char *bp = (unsigned char *)buf;	/* start of buffer */
    unsigned char *be = bp + len;		/* beyond end of buffer */
	
    /*
     * FNV-1 hash each octet in the buffer
     */
	while (bp < be) {
		
		/* xor the bottom with the current octet */
		hval ^= (uint32)*bp++;
		
		/* multiply by the 32 bit FNV magic prime mod 2^32 */
#if defined(NO_FNV_GCC_OPTIMIZATION)
		hval *= FNV_32_PRIME;
#else
		hval += (hval<<1) + (hval<<4) + (hval<<7) + (hval<<8) + (hval<<24);
#endif
	}
	
    /* return our new hash value */
    return hval;
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

