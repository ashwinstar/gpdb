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
#define HASH_FNV 1

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
	int hash;		       /* The result hash value                           */
	int numsegs;           /* number of segments in CDB used for partitioning */
	int hash_algorithm;    /* the hashing algorithm                           */
	int reduce_algorithm;  /* the algorithm used for reducing to buckets      */
	
} CdbHash;


/*
 * Create and initialize a CdbHash in the current memory context.
 * Parameter numsegs - number of segments in CDB. 
 */
extern CdbHash *makeCdbHash(int numsegs); 

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

