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
	

/*
 * Initialize a CdbHash for this session.
 *
 * CdbHash is initialized an maintains the following information about the hash:
 * 1 - a the current hash value.
 * 2 - number of segments in CDB.
 * 3 - hashingalgorithm used.
 * 4 - reduction method.
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
 * Add an attribute to the hash calculation.
 */
void cdbhash(CdbHash *h, Datum val, Oid typid, short typmod)
{
	
	ereport(NOTICE,  (
		errmsg("CDB Hash: Adding an attribute to the hash.") /* add more detail later */
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

