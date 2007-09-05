/*-------------------------------------------------------------------------
 *
 *  execHashagg.h
 *		CDB addition to support hybrid hash aggregation algorithm.
 *
 *
 * Portions Copyright (c) 2006-2007, Greenplum
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECHASHAGG_H
#define EXECHASHAGG_H

#include "storage/buffile.h"
#include "executor/nodeAgg.h" /* Must see AggStatePerGroupData. */

#include "cdb/cdbexplain.h"                 /* CdbExplain_Agg */

typedef struct BatchFileInfo
{
	long ntuples;
	BufFile *file;
} BatchFileInfo;

typedef struct BatchTupleHeader
{
	uint32		tuplen;	
	uint16		natts;			/* number of attributes */
	uint16		infomask;		/* various flag bits */
	/* nulls bitmap follows if HEAP_HASNULL, then actual tuple data */
} BatchTupleHeader;

/* An entry in an Agg hash table.
 * 
 * Each such entry corresponds to a single group and includes the grouping
 * key value, the hash value of that key, and the transition data for each
 * aggregate being evaluated.
 *
 * When accounting for the memory used by a HashAggEntry, include the
 * size of HashAggEntry structure plus an AggStatePerGroupData for each
 * aggregate function plus the size of the HeapTupleData used to hold the
 * value of the grouping key.  Additional space is used for and pass-by-
 * reference Datum values in the grouping key and in transValues
 * in the per-group structure.
 */
typedef struct HashAggEntry
{
	struct HashAggEntry	   *next; /* Next entry in chain. */
	uint32					hashvalue;
	HeapTuple				heaptuple; /* Tuple representing the grouping key. */
	AggStatePerGroupData	pergroup[1]; /* 0 or more AggStatePerGroupData */
} HashAggEntry;


typedef struct HashAggTableSizes
{
    unsigned             nbuckets;   /* Calculated # of hash buckets. */
    unsigned             nentries;   /* Calculated # of hash entries. */
    unsigned             nbatches;   /* Calculated # of passes. */
    unsigned             workmem_initial;    /* estimated work_mem bytes at #entries=0 */
    unsigned             workmem_per_entry;  /* additional work_mem bytes per entry */
    bool            spill;      /* do we expect to spill ? */
} HashAggTableSizes;


typedef enum HashAggState
{
	HASHAGG_BEFORE_FIRST_PASS,
	HASHAGG_IN_A_PASS,
	HASHAGG_BETWEEN_PASSES,
	HASHAGG_END_OF_PASSES
} HashAggState;

/* An Agg hash table with associated overflow batches and processing
 * state.  
 * 
 * The table holds information specific to hybrid hash aggregate, but
 * relies on the parent AggState or grandparent Agg nodes for many items,
 * e.g., description of input and output tuples, tuple slots, expression 
 * and memory contexts, grouping key information (including hash and 
 * equality functions), etc. Thus it is very tightly coupled with them.
 */
typedef struct HashAggTable
{
	/* Hash table */
	unsigned		nbucket;
	HashAggEntry  **buckets;
	uint64         *bloom;
	unsigned		max_entries;
	unsigned		nentries;
	
	/* Overflow batches */
	unsigned			nbatch;
	BatchFileInfo	   *batches;
	int					lshift; /* for calculating batch from hash */
	int					rshift;
	
	/* Number of distinct grouping key values and input tuples */
	unsigned long		ngroup;
	unsigned long		ntuple;
	
	HashAggState		state;	/* state of processing */
	unsigned			pass;	/* current pass (0 is initial) */
 	unsigned			source;	/* source batch file for this pass */
	unsigned			overflow; /* overflow batch file for this pass */
	
	/* Hash table iterator */
	unsigned			curr_bucket_idx;
	HashAggEntry		*next_entry;	

    /* CDB: Statistics for EXPLAIN ANALYZE */
    HashAggTableSizes   hats;
	unsigned long		ngroup_initial;
	unsigned long		ntuple_initial;
    unsigned long       noverflow_initial;
    unsigned            num_reloads;
    double              num_overflow_rows;
    double              num_groups_total;
    CdbExplain_Agg      chainlength;
} HashAggTable;

extern HashAggTable *create_agg_hash_table(AggState *aggstate);
extern void agg_hash_initial_pass(AggState *aggstate);
extern bool agg_hash_next_pass(AggState *aggstate);
extern bool agg_hash_continue_pass(AggState *aggstate);

extern HashAggEntry *agg_hash_iter(HashAggTable* ht);

extern void destroy_agg_hash_table(AggState *aggstate);

extern bool 
calcHashAggTableSizes(double memquota,	/* Memory quota in bytes. */
					   double ngroups,	/* Est # of groups. */
					   int numaggs,		/* Est # of aggregate functions */
					   int keywidth,	/* Est per entry size of hash key. */
					   int transpace,	/* Est per entry size of by-ref values. */
                       bool force,      /* true => succeed even if work_mem too small */
                       HashAggTableSizes   *out_hats);
					   
#endif   /* EXECHASHAGG_H */
