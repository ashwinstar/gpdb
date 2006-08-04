/*-------------------------------------------------------------------------
 *
 * execBatch.c
 *		CDB addition to support hybrid hash aggregation algorithm.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2006, Greenplum
 *
 * IDENTIFICATION
 *	  $ID$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h" /* work_mem */
#include "nodes/execnodes.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "executor/nodeAgg.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "access/htup.h"

#include "executor/execHashagg.h"

#define NGROUP_PER_BUCKET 10.0

#define OVERHEAD_PER_ENTRY ( 1.0/NGROUP_PER_BUCKET + sizeof(HashAggEntry) )

static void reset_agg_hash_table(AggState *aggstate);
static bool agg_hash_reload(AggState *aggstate);

static void create_batches(HashAggTable *hashtable, int nbatch);
static void destroy_batches(HashAggTable *hashtable);
static void batch_put_tuple(HashAggTable *hashtable, int batch, TupleTableSlot *tupleSlot);
static void batch_rewind(HashAggTable *hashtable, int batch);
static void batch_close(HashAggTable *hashtable, int batch);

static TupleTableSlot *batch_get_tuple(HashAggTable *hashtable, int batch,  TupleTableSlot *tupleSlot);
static void init_agg_hash_iter(HashAggTable* ht);
static HashAggEntry *lookup_agg_hash_entry(AggState *aggstate, TupleTableSlot *inputslot, uint32 *hashkeyp, bool *isNew);
static uint32 get_hash_value(AggState* aggstate, TupleTableSlot *inputslot);
static HashAggEntry *makeHashAggEntry(AggState *aggstate, TupleTableSlot *inputslot, uint32 hashkey);

/* Function: create_agg_hash_table
 *
 * Creates and initializes a hash table for the given AggState.  Should be
 * called after the rest of the AggState is initialized.  The resulting table 
 * should be installed in the AggState.
 *
 * The main control structure for the hash table is allocated in the memory
 * context aggstate->aggcontext as is the bucket array and the items related
 * to overflow files.  The contents of the hash table will be allocated in 
 * child context aggstate->transcontext to allow quick resets.
 */
HashAggTable *
create_agg_hash_table(AggState *aggstate)
{
	HashAggTable *ht;
	int est_nbucket;
	int est_nentry;
	int est_nbatch;
	int est_entry_width = 1024; /* TODO */
	Agg *agg = (Agg*)aggstate->ss.ps.plan;
	MemoryContext oldcontext;
	
	if ( !calcHashAggTableSizes( 1024.0 * (double)work_mem, 
								(double)agg->numGroups, 
								est_entry_width,
								&est_nbucket, 
								&est_nbatch, 
								&est_nbatch ) )
	{
		elog(ERROR, "unable to implement hashed aggregation strategy");
	}
	
	oldcontext = MemoryContextSwitchTo(aggstate->aggcontext);
	
	ht = (HashAggTable*)palloc0(sizeof(HashAggTable));
	
	ht->nbucket = est_nbucket;
	ht->buckets = (HashAggEntry**)palloc0(ht->nbucket * sizeof(HashAggEntry*));
	
	ht->max_entries = est_nentry;
	ht->nentries = 0;
	
	create_batches(ht, est_nbatch);
	
	/* To calculate a batch index, we use the hash key after rotating
	 * the bits used in calculating the bucket to the front.  These
	 * are the shift arguments.
	 */
	ht->lshift = ceil(log2(ht->nbucket));
	ht->rshift = 32 - ht->lshift;
	
	ht->ngroup = ht->ntuple = 0;
	
	ht->state = HASHAGG_BEFORE_FIRST_PASS;
	ht->pass = 0;
	ht->last_pass = ht->nbatch;
	
	MemoryContextSwitchTo(oldcontext);
	
	init_agg_hash_iter(ht);
	
	return ht;
}


/* Function: reset_agg_hash_table
 *
 * Clear the hash table content anchored by the bucket array.
 */
void reset_agg_hash_table(AggState *aggstate)
{
	HashAggTable *ht = aggstate->hhashtable;
	
	MemSet(ht->buckets, 0, ht->nbucket * sizeof(HashAggEntry*));
	MemoryContextReset(aggstate->transcontext);
	init_agg_hash_iter(ht);
}



/* Function: agg_hash_initial_pass
 *
 * Performs ExecAgg initialization for the first pass of the hashed case
 * via function exec_hybrid_hash_agg(): 
 * - reads the input tuples, 
 * - builds a hash table with an entry per group, 
 * - spills tuples for groups that won't fit in the hash table to one of 
 *   several overflow batches to process during later passes.
 *
 * Note that overflowed input tuples are distributed to batches in such 
 * a way that tuples with matching grouping keys will be in the same
 * batch.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
void
agg_hash_initial_pass(AggState *aggstate)
{
	PlanState  *outer_ps;
	ExprContext *tmpcontext;
	HashAggTable *ht;
	HashAggEntry *entry;
	TupleTableSlot *outerslot;
	uint32 hashkey;
	unsigned long noverflow = 0;
	bool isNew = false;
	bool spilling = false;

	/* 
	 * Get state info from node. 
	 */
	outer_ps = outerPlanState(aggstate);
	tmpcontext = aggstate->tmpcontext; /* per-input-tuple expr context */
	ht = aggstate->hhashtable;
	Assert(ht && ht->state == HASHAGG_BEFORE_FIRST_PASS);

	/*
	 * Process outer-plan tuples, until we exhaust the outer plan.
	 */
	ht->pass = 0;
	for (;;)
	{
		outerslot = ExecProcNode(outer_ps);
		if (TupIsNull(outerslot))
			break;
			
		/* set up for advance_aggregates call */
		tmpcontext->ecxt_scantuple = outerslot;
		 
		/* Find or (if there's room) build a hash table entry for the 
		 * input tuple's group. */
		entry = lookup_agg_hash_entry(aggstate, outerslot, &hashkey, &isNew);

		if ( entry != NULL )
		{
			if ( isNew )
			{
				initialize_aggregates(aggstate, aggstate->peragg, entry->pergroup);
				ht->ngroup++;
			}
			ht->ntuple++;
			
			/* Advance the aggregates */
			advance_aggregates(aggstate, entry->pergroup);

			/* Reset per-input-tuple context after each tuple */
			ResetExprContext(tmpcontext);
		}
		else
		{
			/* No room for a new entry, write entry to overflow batch. */
			uint32 rotkey = hashkey << ht->lshift | hashkey >> ht->rshift;
			unsigned batch = 1 + rotkey % (ht->nbatch-1);
			
			Assert( 0 < batch && batch < ht->nbatch );
			
			if ( ! spilling )
			{
				/* TODO:  HERE'S A GOOD PLACE TO REEVALUATE DENSITY AND,
				 *        MAYBE, SHRINK NUMBER OF BATCHES.
				 */	
				double act_tuples_per_group = ((double)ht->ntuple)/(double)ht->ngroup;
				
				spilling = true;
			}
			noverflow++;
			
			/* Write the tuple to the appropriate overflow batch. */
			batch_put_tuple(ht, batch, outerslot);
		}
	}

	/* Initialize to walk the hash table */
	init_agg_hash_iter(aggstate->hhashtable);

}


/* Function: agg_hash_next_pass
 *
 * Performs ExecAgg initialization for the second and subsequent passes
 * of the hashed case via function exec_hybrid_hash_agg(): 
 * - clears the previous (exhausted) hash table and initialize a new one,
 * - reads the input tuples for the next batch, 
 * - builds a hash table with an entry per group, 
 * - spills tuples for groups that won't fit in the hash table to an 
 *   overflow batch to process later in the pass.
 */
bool
agg_hash_next_pass(AggState *aggstate)
{
	reset_agg_hash_table(aggstate);

	if ( aggstate->hhashtable->pass++ > aggstate->hhashtable->last_pass )
		return false;

	aggstate->hhashtable->source = aggstate->hhashtable->pass;
	aggstate->hhashtable->overflow = 0;
	
	return agg_hash_reload(aggstate);
}

/* Function: agg_hash_continue_pass
 *
 * Performs ExecAgg initialization for the later phases (dealing with second
 * level overflow) of a pass of the hashed case via function exec_hybrid_hash_agg(): 
 * - reads the input tuples from the most recent overflow, 
 * - builds a hash table with an entry per group, 
 * - spills tuples for groups that won't fit in the hash table to an 
 *   overflow batch to process later in the pass.
 */
bool
agg_hash_continue_pass(AggState *aggstate)
{
	int x;
	
	reset_agg_hash_table(aggstate);
	
	/* Swap source and overflow files. */
	x = aggstate->hhashtable->overflow;
	aggstate->hhashtable->overflow = aggstate->hhashtable->source;
	aggstate->hhashtable->source = x;
	
	return agg_hash_reload(aggstate);
}


/*
 * Function: agg_hash_reload
 *
 * Common code for agg_hash_next_pass and agg_hash_continue_pass to process
 * an overflow batch.  Returns true if the batch was non-empty and thus
 * produced some completed hash table entries.  In this case, the hash
 * table's iterator is reinitialized. Returns false if the batch was empty.
 * 
 * Batch processing accumulates input tuples for as many groups as will fit
 * in the (initially empty) hash table and writes any remaining input to
 * another overflow batch.  Though we use planner estimates to try to avoid 
 * overflowing the same tuple multiple times, we'll do so in a pinch.
 */
bool
agg_hash_reload(AggState *aggstate)
{
	ExprContext *tmpcontext;
	HashAggTable *ht;
	BatchFileInfo *batches;
	int source, overflow;
	HashAggEntry *entry;
	TupleTableSlot *outerslot;

	/*
	 * get state info from node
	 */
	ht = aggstate->hhashtable;
	batches = ht->batches;
	source = ht->source;
	overflow = ht->overflow;
	tmpcontext = aggstate->tmpcontext; /* per-input-tuple expression context */
	
	 /* If batch aggstate->hhashtable->overflow isn't empty, the caller is confused. */
	Assert( batches[overflow].file == NULL );
	
	if ( batches[source].file == NULL )
		return false; /* No more tuples in batch */

	/*
	 * Process each source tuple either by accumulating it to the hash
	 * table or by writing it to the overflow batch.
	 */
	batch_rewind(ht, source);
	
	for (;;)
	{
		bool isNew;
		
		outerslot = batch_get_tuple(ht, source, tmpcontext->ecxt_scantuple);
		if (TupIsNull(outerslot))
			break;
			
		/* set up for advance_aggregates call */
		tmpcontext->ecxt_scantuple = outerslot;

		/* Find or build hhashtable entry for this tuple's group */
		entry = lookup_agg_hash_entry(aggstate, outerslot, NULL, &isNew);

		if ( entry != NULL )
		{			
			if ( isNew )
				initialize_aggregates(aggstate, aggstate->peragg, entry->pergroup);
				
			/* Advance the aggregates */
			advance_aggregates(aggstate, entry->pergroup);

			/* Reset per-input-tuple context after each tuple */
			ResetExprContext(tmpcontext);
		}
		else
		{			
			/* Write the input tuple to the overflow. */ 
			batch_put_tuple(ht, overflow, outerslot);
		}
	}

	/* Initialize to walk the hash table */
	init_agg_hash_iter(ht);
	
	return true; /* Got some more tuples. */
}

/* Function: init_agg_hash_iter
 *
 * Initialize the HashAggTable's (one and only) entry iterator. */
void init_agg_hash_iter(HashAggTable* ht)
{
	Assert( ht != NULL && ht->buckets != NULL && ht->nbucket > 0 );
	
	ht->curr_bucket_idx = -1;
	ht->next_entry = NULL;
}

/* Function: agg_hash_iter
 *
 * Returns a pointer to the next HashAggEntry on the given HashAggTable's 
 * iterator and advances the iterator.  Returns NULL when there are no more
 * entries.  Be sure to call init_agg_hash_iter before the first call here.
 */
HashAggEntry *agg_hash_iter(HashAggTable* ht)
{
	HashAggEntry *entry = NULL;
	
	Assert( ht != NULL && ht->buckets != NULL && ht->nbucket > 0 );
	
	while ( ht->next_entry == NULL && ht->nbucket > ++ht->curr_bucket_idx )
		ht->next_entry = ht->buckets[ht->curr_bucket_idx];
	
	entry = ht->next_entry;
	
	if ( entry != NULL )
		ht->next_entry = entry->next;
	
	return entry;
}


/*
 * Function: get_hash_value
 *
 * Calculate the hash value for the given input tuple.  
 */
uint32
get_hash_value(AggState* aggstate, TupleTableSlot *inputslot)
{
	Agg *agg;
	ExprContext *econtext;
	MemoryContext oldContext;
	uint32		hashkey = 0;
	int			i;
	
	agg = (Agg*)aggstate->ss.ps.plan;
	econtext = aggstate->tmpcontext; /* short-lived, per-input-tuple */

	/*
	 * Reset the eval context each time to reclaim any memory leaked in the
	 * hashkey expressions.
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (i = 0; i < agg->numCols; i++)
	{
		AttrNumber	att = agg->grpColIdx[i];
		Datum		attr;
		bool		isNull;

		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		attr = slot_getattr(inputslot, att, &isNull);

		if (!isNull)			/* treat nulls as having hash key 0 */
		{
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1(&aggstate->hashfunctions[i],
												attr));
			hashkey ^= hkey;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return hashkey;
}


/* Function: lookup_agg_hash_entry
 *
 * Returns a pointer to the old or new hash table entry corresonding
 * to the given input tuple of the AggState or NULL, if there is no
 * such entry (and the table is full).
 *
 * If hashkeyp is non-NULL, (*hashekeyp) is set to the value of the key.
 *
 * If an entry is returned and isNew is non-NULL, (*isNew) is set to true 
 * or false depending on whether the returned entry is new.  Note that
 * a new entry will have uninitialized per-group data (Aggref states).
 * Initializing them is the callers responsibility.
 */
HashAggEntry *
lookup_agg_hash_entry(AggState *aggstate, TupleTableSlot *inputslot, uint32 *hashkeyp, bool *isNew)
{
	Agg *agg;
	ExprContext *econtext;
	MemoryContext oldContext;
	TupleTableSlot *hashslot  = aggstate->hashslot;
	HashAggTable *ht;
	HashAggEntry *entry;
	TupleDesc entry_tuple_desc;
	uint32 hashkey;
	int bucket_idx;
	bool match;
	
	agg = (Agg*)aggstate->ss.ps.plan;
	econtext = aggstate->tmpcontext; /* short-lived, per-input-tuple */
	ht = aggstate->hhashtable;
	entry_tuple_desc = aggstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	
	/* if first time through, initialize hashslot by cloning input slot.
	 * This is essential for proper functioning of get_hash_value, etc. */
	if (hashslot->tts_tupleDescriptor == NULL)
	{
		ExecSetSlotDescriptor(hashslot, inputslot->tts_tupleDescriptor, false);
		/* Make sure all unused columns are NULLs */
		ExecStoreAllNullTuple(hashslot);
	}

	ResetExprContext(econtext);
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* Get hash key and its bucket index. */
	hashkey = get_hash_value(aggstate, inputslot);
	if ( hashkeyp ) 
		*hashkeyp = hashkey;
	bucket_idx = hashkey % ht->nbucket;
	
	/* Search entry chain for the bucket. */
	for ( entry = ht->buckets[bucket_idx]; 
		  entry != NULL; 
		  entry = entry->next )
	{
		match = false;
		
		if ( hashkey == entry->hashvalue )
		{
			HeapTuple heapTuple = entry->heaptuple;
			int i;
			
			for (match = true, i = 0; match && i < agg->numCols; i++)
			{
				AttrNumber	att = agg->grpColIdx[i];
				Datum		input_datum, entry_datum;
				bool		input_isNull, entry_isNull;

				input_datum = slot_getattr(inputslot, att, &input_isNull);
				entry_datum = heap_getattr(heapTuple, att, entry_tuple_desc, &entry_isNull);
				
				if ( input_isNull && entry_isNull )
					continue; /* NULLs match in group keys. */
				if ( !(input_isNull || entry_isNull) &&
					 (DatumGetBool(FunctionCall2(&aggstate->eqfunctions[i],
												 input_datum,
												 entry_datum)) ) )
					continue; /* Both non-NULL and equal. */
				match = false;
			}
		}
		
		if ( match )
		{
			/* Found an existing matching entry. */
			if ( isNew != NULL )
				*isNew = false;
			break;
		}
	}
	
	if ( entry == NULL )
	{
		if ( ht->nentries < ht->max_entries ) /* If hash table has room */
		{
			/* Create a new matching entry. */
			entry = makeHashAggEntry(aggstate, inputslot, hashkey);
			entry->next = ht->buckets[bucket_idx];			
			ht->buckets[bucket_idx] = entry;
			ht->nentries++;
			
			if ( isNew != NULL )
				*isNew = true;
		}
		else
		{
			/* No matching entry and no room to create one. */
			entry = NULL;
			/* Don't change *isNew in this case. */
		}  
	} 
	
	(void) MemoryContextSwitchTo(oldContext);
	
	return entry;
}


/* Function: makeHashAggEntry
 *
 * Allocate a new hash agg entry for the given input tuple an hash key
 * of the given AggState. This includes installing the grouping key heap tuple.  
 *
 * It is the caller's responsibility to link the entry into the hash table
 * and to initialize the per group data.
 */
HashAggEntry *
makeHashAggEntry(AggState *aggstate, TupleTableSlot *inputslot, uint32 hashvalue)
{
	HashAggEntry *entry;
	MemoryContext oldContext;
	ListCell   *lc;
	TupleTableSlot *hashslot = aggstate->hashslot;

	/* Per query context. */
	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.state->es_query_cxt);

	/* Move the grouping key into the hash slot by transferring just the 
	 * needed columns.  (All columns were initialized to null earlier, 
	 * so this is sufficient.) */
	slot_getsomeattrs(inputslot, linitial_int(aggstate->hash_needed));
	foreach(lc, aggstate->hash_needed)
	{
		int		varNumber = lfirst_int(lc) - 1;

		hashslot->tts_values[varNumber] = inputslot->tts_values[varNumber];
		hashslot->tts_isnull[varNumber] = inputslot->tts_isnull[varNumber];
	}
	
	/* For now we just allocate single entries.
	*
	 * TODO -- Allocate them all at once (we know how many we're willing
	 *         to hold in memory) or in blocks (since we may tie up more
	 *         virtual memory than necessary, otherwise).
	 */
	MemoryContextSwitchTo(aggstate->transcontext);
	
	entry = (HashAggEntry*)palloc0(sizeof(HashAggEntry));
	entry->heaptuple = ExecCopySlotTuple(hashslot);
	entry->hashvalue = hashvalue;
	entry->pergroup = (AggStatePerGroup)palloc0(aggstate->numaggs * sizeof(AggStatePerGroupData));
	
	MemoryContextSwitchTo(oldContext);
	return entry;
}


/* Function: calcHashAggTableSizes
 *
 * Calculate the number of entries in the hash aggregation tables required
 * to handle the given number of groups with key tuples of the given size
 * within in the constraints of the givem memory quota.  If this is not
 * possible, return false.  Otherwise, return true and fill in the size
 * buffers for any non-NULL result pointer.
 *
 * Note that entrywidth should include the estimated size of an average
 * grouping key tuple, AggStatePerGroupData per aggregate function, and
 * any associated pass-by-reference values.
 *
 * This function must agree with the code in ...
 */
bool 
calcHashAggTableSizes(double memquota,	/* Memory quota in bytes. */
					   double ngroups,	/* Est # of groups. */
					   int entrywidth,	/* Est per entry size of key and aggs. */
					   int *p_nbuckets,	/* Calculated # of hash buckets. */
					   int *p_nentries,	/* Calculated # of hash entries. */
					   int *p_nbatches	/* Calculated # of passes. */
					   )
{
	double nK = ceil(ngroups/NGROUP_PER_BUCKET); /* Buckets */
	
	double nH, zH = OVERHEAD_PER_ENTRY + entrywidth; /* Hash Entries */
	
	double nB, zB = sizeBufFile(NULL); /* Passes = overflow batches */
	
	
	if ( memquota >= ngroups*zH + nK*zH + 2*zB )
	{
		/* Use default strategy when we expect the whole operation to 
		 * fit in memory.  This strategy hedges its bet by allocating 
		 * 2 batches in case the estimate is low and we overflow anyway.
		 */
		if ( nK > INT_MAX || ngroups > INT_MAX )
		{
			elog(DEBUG1, "hashagg table bigger than int type!");
			return false;
		}
		
		nB = 2;
	}
	else
	{
		/* The normal stategy expects to spill to disk, so we estimate
		 * how many passes we expect to need and allocate that many 
		 * batches.  Since the first pass will load the hash table 
		 * rather than a batch file, this leaves a batch file for
		 * overflow within a subsequent pass. 
		 */
		double a,d;
		
		a = memquota - zB;
		d = a*a + 4*zB*(memquota - ngroups*zH);
		
		if ( d < 0 || d > a )
		{
			elog(DEBUG1, "hashagg can't calculate memory usage!");
			return false;
		}
		
		nB = ceil((a - sqrt(d))/(2*zB));
	}
	
	nH = floor((memquota - nB*zB)/zH);
	
	if ( nB > INT_MAX || nH > INT_MAX )
	{
		return false; /* Too many groups. */
	}
	
	nK = ceil(nH/NGROUP_PER_BUCKET);
	
	Assert( memquota >= nH*zH + nB*zB );
	Assert( nK<INT_MAX && nH<INT_MAX && nB<INT_MAX );
			
	if (p_nbuckets) *p_nbuckets = (int)nK;
	if (p_nentries) *p_nentries = (int)nH;
	if (p_nbatches) *p_nbatches = (int)nB;
	
	return true;
}


/* Function: create_batches
 *
 * Initialize HashAggTable with the specified number of batch file slots. 
 */
void
create_batches(HashAggTable * hashtable, int nbatch)
{
	size_t sz;
	
	Assert( nbatch > 1 && hashtable->batches == NULL );

	sz = sizeof(BatchFileInfo) * (nbatch-1);	
	hashtable->batches = (BatchFileInfo*)palloc0(sz);
	hashtable->nbatch = nbatch;
}

/* Function: destroy_batches
 *
 * Clear HashAggTable of batch file slots and any asssociated batch files.
 */
void
destroy_batches(HashAggTable * hashtable)
{
	int i;
	
	for ( i = 0; i < hashtable->nbatch; i++ )
	{
		batch_close(hashtable, i);
	}
	pfree(hashtable->batches);
	hashtable->nbatch = 0;
	hashtable->batches = NULL;
}


/*
 * batch_put_tuple
 *		save a tuple to a batch file.
 *
 * The data recorded in the file for each tuple is just an image of
 * its HeapTupleData (with meaningless t_data pointer) followed by the 
 * HeapTupleHeader and tuple data.
 *
 * Note: it is important always to call this in the regular executor
 * context, not in a shorter-lived context; else the temp file buffers
 * will get messed up.
 */
void
batch_put_tuple(HashAggTable *hashtable,
			   int batch,
			   TupleTableSlot *tupleSlot)
{
	size_t		written;
	BatchFileInfo *info;
	BufFile    *file;
	BatchTupleHeader hth;
	HeapTupleHeader t_data;
	unsigned int	datalen;
	unsigned int	nullslen;
	HeapTuple heapTuple = tupleSlot->tts_tuple;
	
	static const char *msg = "could not write to temporary batch file: %m";

	info = &hashtable->batches[batch];
	file = info->file;
	t_data = heapTuple->t_data;

	if (file == NULL)
	{
		/* First write to this batch file, so open it. */
		file = BufFileCreateTemp(false);
		info->file = file;
		info->ntuples = 0L;
	}

	datalen = heapTuple->t_len - t_data->t_hoff;
	if (HeapTupleHasNulls(heapTuple))
		nullslen = BITMAPLEN(t_data->t_natts);
	else
		nullslen = 0;

	hth.tuplen = sizeof(BatchTupleHeader) + nullslen + datalen;
	hth.natts = t_data->t_natts;
	hth.infomask = t_data->t_infomask;

	written = BufFileWrite(file, (void *)&hth, sizeof(hth));
	if (written != sizeof(hth))
		ereport(ERROR, (errcode_for_file_access(), errmsg(msg)));

	/* Now the body of the tuple */
	if (nullslen)
	{
		written = BufFileWrite(file, (void *)t_data->t_bits, nullslen);
		if (written != nullslen)
			ereport(ERROR, (errcode_for_file_access(), errmsg(msg)));
	}

	written = BufFileWrite(file, (char *)t_data + t_data->t_hoff, datalen);
	if (written != datalen)
		ereport(ERROR, (errcode_for_file_access(), errmsg(msg)));
	
	info->ntuples++;
}

/*
 * batch_get_tuple
 *		read the next tuple from a batch file.	Return NULL if no more.
 *
 * On success the tuple is stored in the given slot.
 */
TupleTableSlot *
batch_get_tuple(HashAggTable *hashtable, 
				   int batch,
				   TupleTableSlot *tupleSlot)
{
	size_t		nread;
	HeapTuple	heapTuple;
	BatchTupleHeader hth;
	unsigned int	datalen;
	unsigned int	nullslen;
	unsigned int	hoff;
	HeapTupleHeader t_data;
	
	static const char *msg = "could not read from temporary batch file: %m";

	BatchFileInfo *info = &hashtable->batches[batch];
	BufFile *file = info->file;

	nread = BufFileRead(file, (void *)&hth, sizeof(hth));
	if (nread == 0)
	{
		/* Don't leave exhausted batch files lying around. */
		Assert(info->ntuples == 0);
		batch_close(hashtable, batch);
		return NULL; /* end of file */
	}
	if (nread != sizeof(hth))
		ereport(ERROR, (errcode_for_file_access(), errmsg(msg)));

	/* reconstruct lengths of null bitmap and data part */
	if (hth.infomask & HEAP_HASNULL)
		nullslen = BITMAPLEN(hth.natts);
	else
		nullslen = 0;
	datalen = hth.tuplen - sizeof(BatchTupleHeader) - nullslen;

	/* determine overhead size of tuple (should match heap_form_tuple) */
	hoff = offsetof(HeapTupleHeaderData, t_bits) + nullslen;
	if (hth.infomask & HEAP_HASOID)
		hoff += sizeof(Oid);
	hoff = MAXALIGN(hoff);

	/* Allocate the space in one chunk, like heap_form_tuple */
	heapTuple = (HeapTuple) palloc(HEAPTUPLESIZE + hoff + datalen);
	heapTuple->t_datamcxt = CurrentMemoryContext;

	t_data = (HeapTupleHeader) ((char *)heapTuple + HEAPTUPLESIZE);

	/* make sure unused header fields are zeroed */
	MemSetAligned(t_data, 0, hoff);
	/* reconstruct the HeapTupleData fields */
	heapTuple->t_len = hoff + datalen;
	ItemPointerSetInvalid(&(heapTuple->t_self));
	heapTuple->t_tableOid = InvalidOid;
	heapTuple->t_data = t_data;

	/* reconstruct the HeapTupleHeaderData fields */
	ItemPointerSetInvalid(&(t_data->t_ctid));
	t_data->t_natts = hth.natts;
	t_data->t_infomask = (hth.infomask & ~HEAP_XACT_MASK)
		| (HEAP_XMIN_INVALID | HEAP_XMAX_INVALID);
	t_data->t_hoff = hoff;

	if (nullslen)
	{
		nread = BufFileRead(file, (void *) t_data->t_bits, nullslen);
		if (nread != nullslen)
			ereport(ERROR, (errcode_for_file_access(), errmsg(msg)));
	}

	/* and now the data proper */
	nread = BufFileRead(file, (char *) t_data + hoff, datalen);
	if (nread != datalen)
		ereport(ERROR, (errcode_for_file_access(), errmsg(msg)));

	info->ntuples--;
	return ExecStoreTuple(heapTuple, tupleSlot, InvalidBuffer, true);
}

void
batch_rewind(HashAggTable *hashtable, int batch)
{
	int n;
	BatchFileInfo *info; 
	
	Assert( hashtable != NULL && 0 <= batch && batch < hashtable->nbatch );
	
	info = &hashtable->batches[batch];
	
	if ( info->file == NULL )
	{
		Assert(info->ntuples == 0);
		return;
	}
	
	n = BufFileSeek(info->file, 0, 0L, 0);
	
	Assert( n == 0);
}


void 
batch_close(HashAggTable *hashtable, int batch)
{
	BatchFileInfo *info; 
	
	Assert( hashtable != NULL && 0 <= batch && batch < hashtable->nbatch );
	
	info = &hashtable->batches[batch];
	
	if ( info->file != NULL )
	{
		BufFileClose(info->file);
		info->file = NULL;
	}
	
	info->ntuples = 0;
}
