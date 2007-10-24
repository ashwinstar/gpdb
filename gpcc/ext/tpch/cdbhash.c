/*--------------------------------------------------------------------------
 *
 * cdbhash.c
 *	  Rrovides hashing routines to support consistant data distribution/location within MPP.
 *
 * Copyright (c) 2005, Greenplum
 *
 * IDENTIFICATION
 *	  $Id$
 *
 *--------------------------------------------------------------------------
 */
//#include "postgres.h"

#include <ctype.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>

/*
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "utils/numeric.h"
#include "utils/inet.h"
#include "utils/timestamp.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/varbit.h"
#include "fmgr.h"
*/

#include "cdbhash.h"

/*
 * 32 bit FNV-1 and FNV-1a non-zero initial basis
 * NOTE: The FNV-1a initial basis is the same value as FNV-1 by definition.
 */
#define FNV1_32_INIT ((unsigned int)0x811c9dc5)
#define FNV1_32A_INIT FNV1_32_INIT

/* Constant prime value used for an FNV1/FNV1A hash */
#define FNV_32_PRIME ((unsigned int)0x01000193)

/* Constant used for hashing a NULL value */
#define NULL_VAL ((unsigned int)0XF0F0F0F1)

/* Constant used for hashing a NAN value  */
#define NAN_VAL ((unsigned int)0XE0E0E0E1)

/* Fast mod using a bit mask, assuming that y is a power of 2 */
#define FASTMOD(x,y)		((x) & ((y)-1))

/* local function declarations */
unsigned int		fnv1_32_buf(void *buf, size_t len, unsigned int hashval);
unsigned int		fnv1a_32_buf(void *buf, size_t len, unsigned int hashval);
//int			inet_getkey(inet *addr, unsigned char *inet_key, int key_size);
int			ignoreblanks(char *data, int len);
int			ispowof2(int numsegs);


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
 * 1 - number of segments in MPP.
 * 2 - hashingalgorithm used.
 * 3 - reduction method.
 *
 * The hash value itself will be initialized for every tuple in cdbhashinit()
 */
CdbHash *
makeCdbHash(int numsegs, CdbHashAlg algorithm)
{
	CdbHash    *h;

	assert(numsegs > 0);		/* verify number of segments is legal. */
	assert(algorithm == HASH_FNV_1 || algorithm == HASH_FNV_1A);

	/* Create a pointer to a CdbHash that includes the hash properties */
	h = malloc(sizeof(CdbHash));

	/*
	 * set this hash session characteristics.
	 */
	h->hash = 0;
	h->numsegs = numsegs;
	h->hashalg = algorithm;

	if (h->hashalg == HASH_FNV_1)
		h->hashfn = &fnv1_32_buf;
	else if (h->hashalg == HASH_FNV_1A)
		h->hashfn = &fnv1a_32_buf;

	/*
	 * set the reduction algorithm: If num_segs is power of 2 use bit mask,
	 * else use lazy mod (h mod n)
	 */
	if (ispowof2(numsegs))
	{
		h->reducealg = REDUCE_BITMASK;
	}
	else
	{
		h->reducealg = REDUCE_LAZYMOD;
	}

	fprintf(stderr,"\nCDBHASH started using algorithm %d into %d segment databases\n",
					h->hashalg,
					h->numsegs);

	return h;
}


/*
 * Initialize CdbHash for hashing the next tuple values.
 */
void
cdbhashinit(CdbHash *h)
{
	/* reset the hash value to the initial offset basis */
	h->hash = FNV1_32_INIT;
}


/*
 * Add an attribute to the hash calculation.
 */
void
cdbhash(CdbHash *h, long long datum)   //  Datum datum, Oid type)
{

	void	   *buf = NULL;		/* pointer to the data */
	size_t		len = 0;		/* length for the data buffer */
	
	long long		intbuf;			/* an 8 byte buffer for all integer sizes */

#if 0
	BpChar	   *bp;				/* a variable length struct (varlena) for
								 * blank padded strings */
	text	   *txt;			/* a variable length struct (varlena) for text */
	VarChar    *vc;				/* a variable length struct (varlena) for
								 * varchar */
	bytea      *ba;	
		
	float		buf_f4;
	double		buf_f8;
	Timestamp	tsbuf;			/* timestamp data dype is either a double or
								 * int8 (determined in compile time) */
	TimestampTz tstzbuf;
	DateADT		datebuf;
	TimeADT		timebuf;
	TimeTzADT  *timetzptr;
	Interval   *intervalptr;
	
	Numeric		num;
	bool		bool_buf;
	Name		namebuf;
	unsigned int		nanbuf;
	
	ArrayType  *arrbuf;
	inet		 *inetptr; /* inet/cidr */
	unsigned char inet_hkey[sizeof(inet_struct)];
	macaddr		*macptr; /* MAC address */
	
	VarBit		*vbitptr;
	
	int2vector *i2vec_buf;
	oidvector  *oidvec_buf;

	/*
	 * Select the hash to be performed according to the field type we are adding to the
	 * hash.
	 */
	switch (type)
	{
		/*
		 * ======= NUMERIC TYPES ========
		 */
		case INT2OID:			/* -32 thousand to 32 thousand, 2-byte storage */
			intbuf = (long long) DatumGetInt16(datum);		/* cast to 8 byte before
														 * hashing */
			buf = &intbuf;
			len = sizeof(intbuf);
			break;

		case INT4OID:			/* -2 billion to 2 billion integer, 4-byte
								 * storage */
			intbuf = (long long) DatumGetInt32(datum);		/* cast to 8 byte before
														 * hashing */
			buf = &intbuf;
			len = sizeof(intbuf);
			break;
			
		case OIDOID:			/* object identifier(oid), maximum 4 billion */
			intbuf = (long long) DatumGetunsigned int(datum);		/* cast to 8 byte before
														 * hashing */
			buf = &intbuf;
			len = sizeof(intbuf);
			break;

		case INT8OID:			/* ~18 digit integer, 8-byte storage */
			intbuf = DatumGetlong long(datum);		/* cast to 8 byte before
												 * hashing */
			buf = &intbuf;
			len = sizeof(intbuf);
			break;

		case FLOAT4OID: /* single-precision floating point number,
								 * 4-byte storage */
			buf_f4 = DatumGetfloat(datum);

			/*
			 * On IEEE-float machines, minus zero and zero have different bit
			 * patterns but should compare as equal.  We must ensure that they
			 * have the same hash value, which is most easily done this way:
			 */
			if (buf_f4 == (float) 0)
				buf_f4 = 0.0;

			buf = &buf_f4;
			len = sizeof(buf_f4);
			break;

		case FLOAT8OID: /* double-precision floating point number,
								 * 8-byte storage */
			buf_f8 = DatumGetdouble(datum);

			/*
			 * On IEEE-float machines, minus zero and zero have different bit
			 * patterns but should compare as equal.  We must ensure that they
			 * have the same hash value, which is most easily done this way:
			 */
			if (buf_f8 == (double) 0)
				buf_f8 = 0.0;

			buf = &buf_f8;
			len = sizeof(buf_f8);
			break;

		case NUMERICOID:

			num = DatumGetNumeric(datum);

			if (NUMERIC_IS_NAN(num))
			{
				nanbuf = NAN_VAL;
				buf = &nanbuf;
				len = sizeof(nanbuf);
			}
			else
				/* not a nan */
			{
				buf = num->n_data;
				len = (num->varlen - NUMERIC_HDRSZ);
			}

			break;
		
		/*
		 * ====== CHARACTER TYPES =======
		 */
		case CHAROID:			/* char(1), single character */
		case BPCHAROID: /* char(n), blank-padded string, fixed storage
								 * length */
			bp = DatumGetBpCharP(datum);
			len = VARSIZE(bp) - VARHDRSZ;		
			buf = VARDATA(bp);

			/* adjust length to not include trailing blanks */
			if (len > 1)
				len = ignoreblanks((char *) buf, len);
			break;

		case TEXTOID:			/* variable-length string, no limit specified */
			txt = DatumGetTextP(datum); /* a varlena struct (includes data and
										 * len)  */
			len = VARSIZE(txt) - VARHDRSZ;
			buf = VARDATA(txt);

			/* adjust length to not include trailing blanks */
			if (len > 1)
				len = ignoreblanks((char *) buf, len);
			break;

		case VARCHAROID:		/* varchar(length), non-blank-padded string,
								 * variable storage length */
			vc = DatumGetVarCharP(datum);		/* a varlena struct (includes
												 * data and len)  */
			len = VARSIZE(vc) - VARHDRSZ;
			buf = VARDATA(vc);

			/* adjust length to not include trailing blanks */
			if (len > 1)
				len = ignoreblanks((char *) buf, len);
			break;

		case BYTEAOID:
			ba = DatumGetByteaP(datum);
			len = VARSIZE(ba) - VARHDRSZ;
			buf = VARDATA(ba);

		case NAMEOID:
			namebuf = DatumGetName(datum);
			len = NAMEDATALEN;
			buf = NameStr(*namebuf);

			/* adjust length to not include trailing blanks */
			if (len > 1)
				len = ignoreblanks((char *) buf, len);
			break;

		/*
		 * ====== DATE/TIME TYPES ======
		 */
		case TIMESTAMPOID:		/* date and time */
			tsbuf = DatumGetTimestamp(datum);
			buf = &tsbuf;
			len = sizeof(tsbuf);
			break;

		case TIMESTAMPTZOID:	/* date and time with time zone */
			tstzbuf = DatumGetTimestampTz(datum);
			buf = &tstzbuf;
			len = sizeof(tstzbuf);
			break;

		case DATEOID:			/* ANSI SQL date */
			datebuf = DatumGetDateADT(datum);
			buf = &datebuf;
			len = sizeof(datebuf);
			break;

		case TIMEOID:			/* hh:mm:ss, ANSI SQL time */
			timebuf = DatumGetTimeADT(datum);
			buf = &timebuf;
			len = sizeof(timebuf);
			break;

		case TIMETZOID: /* time with time zone */
			
			/*
			 * will not compare to TIMEOID on equal values.
			 * Postgres never attempts to compare the two as well.
			 */
			timetzptr = DatumGetTimeTzADTP(datum);
			buf = (unsigned char *) timetzptr;
			
			/*
			 * Specify hash length as sizeof(double) + sizeof(int4), not as
			 * sizeof(TimeTzADT), so that any garbage pad bytes in the structure
			 * won't be included in the hash!
			 */
			len = sizeof(timetzptr->time) + sizeof(timetzptr->zone);
			break;

		case INTERVALOID:		/* @ <number> <units>, time interval */
			intervalptr = DatumGetIntervalP(datum);
			buf = (unsigned char *) intervalptr;
			/*
			 * Specify hash length as sizeof(double) + sizeof(int4), not as
			 * sizeof(Interval), so that any garbage pad bytes in the structure
			 * won't be included in the hash!
			 */
			len = sizeof(intervalptr->time) + sizeof(intervalptr->month);
			break;

		/*
		 * ======= NETWORK TYPES ========
		 */
		case INETOID:
		case CIDROID:
			
			inetptr = DatumGetInetP(datum);
			len = inet_getkey(inetptr, inet_hkey, sizeof(inet_hkey)); /* fill-in inet_key & get len */
			buf = inet_hkey;
			break;
		
		case MACADDROID:
			
			macptr = DatumGetMacaddrP(datum);
			len = sizeof(macaddr);
			buf = (unsigned char *) macptr;
			break;
			
		/*
		 * ======== BIT STRINGS ========
		 */
		case BITOID:
		case VARBITOID:
			
			/*
			 * Note that these are essentially strings.
			 * we don't need to worry about '10' and '010'
			 * to compare, b/c they will not, by design.
			 * (see SQL standard, and varbit.c)
			 */
			vbitptr = DatumGetVarBitP(datum);
			len = VARBITBYTES(vbitptr);
			buf = (char *) VARBITS(vbitptr);
			break;
			
		/*
		 * ======= other types =======
		 */
		case BOOLOID:			/* boolean, 'true'/'false' */
			bool_buf = DatumGetBool(datum);
			buf = &bool_buf;
			len = sizeof(bool_buf);
			break;

		/*
		 * ANYARRAY is a pseudo-type. We use it to include
		 * any of the array types (OIDs 1007-1033 in pg_type.h).
		 * caller needs to be sure the type is ANYARRAYOID
		 * before calling cdbhash on an array (INSERT and COPY do so).
		 */
		case ANYARRAYOID:	
					
			arrbuf = DatumGetArrayTypeP(datum);
			len = VARSIZE(arrbuf) - VARHDRSZ;
			buf = VARDATA(arrbuf);
			break;
			
		case INT2VECTOROID:
			i2vec_buf = (int2vector *) DatumGetPointer(datum);
			len = i2vec_buf->dim1 * sizeof(int2);
			buf = (void *)i2vec_buf->values;
			break;
			
		case OIDVECTOROID:	
			oidvec_buf = (oidvector *) DatumGetPointer(datum);
			len = oidvec_buf->dim1 * sizeof(Oid);
			buf = oidvec_buf->values;
			break;
			
		default:
			ereport(ERROR,
					(errcode(ERRCODE_CDB_FEATURE_NOT_YET),
					 errmsg("Type %u can't be hashed yet.", type)));

	}							/* switch(type) */
#endif 
	intbuf = (long long) datum;
	buf = &intbuf;
	len = sizeof(intbuf);

	/* do the hash using the selected algorithm */
	h->hash = (h->hashfn) (buf, len, h->hash);
}

/*
 * Add a NULL attribute to the hash calculation.
 */
void
cdbhashnull(CdbHash *h)
{
	unsigned int		nullbuf = NULL_VAL;		/* stores the constant value that
										 * represents a NULL */
	void	   *buf = &nullbuf; /* stores the address of the buffer					*/
	size_t		len = sizeof(nullbuf);	/* length of the value								*/

	/* do the hash using the selected algorithm */
	h->hash = (h->hashfn) (buf, len, h->hash);
}

/*
 * Reduce the hash to a segment number.
 */
unsigned int
cdbhashreduce(CdbHash *h)
{

	int			result = 0;		/* TODO: what is a good initialization value?
								 * could we guarantee at this point that there
								 * will not be a negative segid in MPP and
								 * therefore initialize to this value for
								 * error checking? */

	assert(h->reducealg == REDUCE_BITMASK || h->reducealg == REDUCE_LAZYMOD);

	/*
	 * Reduce our 32-bit hash value to a segment number
	 */
	switch (h->reducealg)
	{
		case REDUCE_BITMASK:
			result = FASTMOD(h->hash, (unsigned int) h->numsegs);		/* fast mod (bitmask) */
			break;

		case REDUCE_LAZYMOD:
			result = (h->hash) % (h->numsegs);	/* simple mod */
			break;
	}

	return result;
}


/*
 * fnv1_32_buf - perform a 32 bit FNV 1 hash on a buffer
 *
 * input:
 *	buf - start of buffer to hash
 *	len - length of buffer in octets (bytes)
 *	hval	- previous hash value or FNV1_32_INIT if first call.
 *
 * returns:
 *	32 bit hash as a static hash type
 */
unsigned int
fnv1_32_buf(void *buf, size_t len, unsigned int hval)
{
	unsigned char *bp = (unsigned char *) buf;	/* start of buffer */
	unsigned char *be = bp + len;		/* beyond end of buffer */

	/*
	 * FNV-1 hash each octet in the buffer
	 */
	while (bp < be)
	{

		/* multiply by the 32 bit FNV magic prime mod 2^32 */
#if defined(NO_FNV_GCC_OPTIMIZATION)
		hval *= FNV_32_PRIME;
#else
		hval += (hval << 1) + (hval << 4) + (hval << 7) + (hval << 8) + (hval << 24);
#endif

		/* xor the bottom with the current octet */
		hval ^= (unsigned int) *bp++;
	}

	/* return our new hash value */
	return hval;
}

/*
 * fnv1a_32_buf - perform a 32 bit FNV 1A hash on a buffer
 *
 * input:
 *	buf - start of buffer to hash
 *	len - length of buffer in octets (bytes)
 *	hval	- previous hash value or FNV1_32_INIT if first call.
 *
 * returns:
 *	32 bit hash as a static hash type
 */
unsigned int
fnv1a_32_buf(void *buf, size_t len, unsigned int hval)
{
	unsigned char *bp = (unsigned char *) buf;	/* start of buffer */
	unsigned char *be = bp + len;		/* beyond end of buffer */

	/*
	 * FNV-1 hash each octet in the buffer
	 */
	while (bp < be)
	{

		/* xor the bottom with the current octet */
		hval ^= (unsigned int) *bp++;

		/* multiply by the 32 bit FNV magic prime mod 2^32 */
#if defined(NO_FNV_GCC_OPTIMIZATION)
		hval *= FNV_32_PRIME;
#else
		hval += (hval << 1) + (hval << 4) + (hval << 7) + (hval << 8) + (hval << 24);
#endif
	}

	/* return our new hash value */
	return hval;
}

#if 0
/*
 * Support function for hashing on inet/cidr (see network.c)
 *
 * Since network_cmp considers only ip_family, ip_bits, and ip_addr,
 * only these fields may be used in the hash; in particular don't use type.
 */
int inet_getkey(inet *addr, unsigned char *inet_key, int key_size)
{
	int			addrsize;
	
	switch (((inet_struct *)VARDATA(addr))->family)
	{
		case PGSQL_AF_INET:
			addrsize = 4;
			break;
		case PGSQL_AF_INET6:
			addrsize = 16;
			break;
		default:
			addrsize = 0;
	}
	
	Assert(addrsize + 2 <= key_size);
	inet_key[0] = ((inet_struct *)VARDATA(addr))->family;
	inet_key[1] = ((inet_struct *)VARDATA(addr))->bits;
	memcpy(inet_key + 2, ((inet_struct *)VARDATA(addr))->ipaddr, addrsize);
	
	return (addrsize + 2);
}
#endif

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
int
ignoreblanks(char *data, int len)
{

	/* look for trailing blanks and skip them in the hash calculation */
	while (data[len - 1] == ' ')
	{
		len--;
		if (len == 1)			/* if only 1 char is left, leave it alone!
								 * (the string is either empty or has 1 char) */
			break;
	}

	return len;

}

/*
 * returns 1 is the input int is a power of 2 and 0 otherwize.
 */
int
ispowof2(int numsegs)
{
	if (numsegs == 0)
		return 0;

	while (numsegs % 2 == 0)
	{
		numsegs >>= 1;			/* effectively divide by 2 */
	}

	return (numsegs == 1) ? 1 : 0;
}
