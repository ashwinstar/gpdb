/*--------------------------------------------------------------------------
*
* cdbhash.h
*    Definitions and API functions for cdbhash.c
*
* Copyright (c) 2005, Metapa
*
* IDENTIFICATION
*    $Id$
*
*--------------------------------------------------------------------------
*/
#ifndef CDBHASH_H
#define CDBHASH_H

/* 
 * hashing algorithms.
 */
#define HASH_FNV_1  1
#define HASH_FNV_1A 2

/*
 * reduction methods.
 */
#define REDUCE_LAZYMOD 1
#define REDUCE_BITMASK 2

/* 
 * Structure that holds CDB hashing information.
 */ 
typedef struct CdbHash
{
	uint32 hash;  		      /* The result hash value                           */
    int numsegs;              /* number of segments in CDB used for partitioning */
	int16 hash_algorithm;    /* the hashing algorithm                           */
	int16 reduce_algorithm;  /* the algorithm used for reducing to buckets      */
	
} CdbHash;


/*
 * Create and initialize a CdbHash in the current memory context.
 * Parameter numsegs - number of segments in CDB. 
 * Parameter algorithm - the hash algorithm, either HASH_FNV_1 or HASH_FNV_1A 
 */
extern CdbHash *makeCdbHash(int numsegs, int16 algorithm); 

/*
 * Initialize CdbHash for hashing the next tuple values.
 */
extern void cdbhashinit(CdbHash *h);

/*
 * Add an attribute to the hash calculation.
 */
extern void cdbhash(CdbHash *h, Datum val, Oid typid, short typmod); 

/*
 * Add a NULL attribute to the hash calculation.
 */
extern void cdbhashnull(CdbHash *h);

/*
 * Reduce the hash to a segment number.
 */
extern unsigned int cdbhashreduce(CdbHash *h); 

#endif   /* CDBHASH_H */

