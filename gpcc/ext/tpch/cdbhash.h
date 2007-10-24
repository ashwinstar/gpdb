/*--------------------------------------------------------------------------
*
* cdbhash.h
*	 Definitions and API functions for cdbhash.c
*
* Copyright (c) 2005, Greenplum
*
* IDENTIFICATION
*	 $Id$
*
*--------------------------------------------------------------------------
*/
#ifndef CDBHASH_H
#define CDBHASH_H

/*
 * hashing algorithms.
 */
typedef enum
{
	HASH_FNV_1 = 1,
	HASH_FNV_1A
} CdbHashAlg;

/*
 * reduction methods.
 */
typedef enum
{
	REDUCE_LAZYMOD = 1,
	REDUCE_BITMASK
} CdbHashReduce;

typedef unsigned int CdbHashFn (void *, size_t, unsigned int);

/*
 * Structure that holds MPP hashing information.
 */
typedef struct CdbHash
{
	unsigned int		hash;			/* The result hash value							*/
	int			numsegs;		/* number of segments in MPP used for
								 * partitioning  */
	CdbHashAlg	hashalg;		/* the hashing algorithm							*/
	CdbHashFn  *hashfn;			/* hashing function for the selected hash
								 * algorithm */
	CdbHashReduce reducealg;	/* the algorithm used for reducing to buckets		*/

} CdbHash;



/*
 * Create and initialize a CdbHash in the current memory context.
 * Parameter numsegs - number of segments in MPP.
 * Parameter algorithm - the hash algorithm, either HASH_FNV_1 or HASH_FNV_1A
 */
extern CdbHash *makeCdbHash(int numsegs, CdbHashAlg algorithm);

/*
 * Initialize CdbHash for hashing the next tuple values.
 */
extern void cdbhashinit(CdbHash *h);

/*
 * Add an attribute to the hash calculation.
 */
extern void cdbhash(CdbHash *h, long long val);

/*
 * Add a NULL attribute to the hash calculation.
 */
extern void cdbhashnull(CdbHash *h);

/*
 * Reduce the hash to a segment number.
 */
extern unsigned int cdbhashreduce(CdbHash *h);

#endif   /* CDBHASH_H */
