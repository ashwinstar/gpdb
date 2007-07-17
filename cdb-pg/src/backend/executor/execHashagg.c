/*-------------------------------------------------------------------------
 *
 * execHashagg.c
 *		CDB additions to support hybrid hash aggregation algorithm.
 *		This file could be merged into nodeAgg.c.  The separation is
 *		only to help isolate Greenplum Database-only code from future merges with
 *		PG code.  Note, however, that nodeAgg.c is also modified to
 *		make use of this code.
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
#include "postgres.h"

#include <math.h>                           /* log2 */

#include "fmgr.h"
#include "miscadmin.h" /* work_mem */
#include "nodes/execnodes.h"
#include "executor/executor.h"
#include "executor/instrument.h"            /* Instrumentation */
#include "executor/tuptable.h"
#include "executor/nodeAgg.h"
#include "lib/stringinfo.h"                 /* StringInfo */
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "access/htup.h"
#include "cdb/cdbvars.h"

#include "executor/execHashagg.h"

static void reset_agg_hash_table(AggState *aggstate);
static bool agg_hash_reload(AggState *aggstate);

static void create_batches(HashAggTable *hashtable, unsigned int nbatch);
static void destroy_batches(HashAggTable *hashtable);
static void batch_put_tuple(HashAggTable *hashtable, unsigned int batch, TupleTableSlot *tupleSlot);
static void batch_rewind(HashAggTable *hashtable, unsigned int batch);
static void batch_close(HashAggTable *hashtable, unsigned int batch);

static TupleTableSlot *batch_get_tuple(HashAggTable *hashtable, unsigned int batch,  TupleTableSlot *tupleSlot);
static void init_agg_hash_iter(HashAggTable* ht);
static HashAggEntry *lookup_agg_hash_entry(AggState *aggstate, TupleTableSlot *inputslot, uint32 *hashkeyp, bool *isNew);
static uint32 calc_hash_value(AggState* aggstate, TupleTableSlot *inputslot);
static HashAggEntry *makeHashAggEntry(AggState *aggstate, TupleTableSlot *inputslot, uint32 hashkey);

static void agg_hash_table_stat_upd(HashAggTable *ht);

static double calcBatchCount(double memquota, double ngroups, double zH, double zB, bool force);

#define HHA_MSG_LVL DEBUG2

/* Function: est_hash_tuple_size
 *
 * Estimate the average memory requirement for the grouping key HeapTuple
 * in a hash table entry.
 *
 * This function purports to know (perhaps too much) about the format in
 * which a tuple representing a grouping key (non-aggregated attrbutes,
 * actually) will be stored.  But it's all guess work, so no sense being
 * too fussy.
 */
static Size est_hash_tuple_size(TupleTableSlot *hashslot, List *hash_needed)
{
	Form_pg_attribute *attrs;
	Form_pg_attribute attr;
	int natts;
	ListCell *lc;
	Size len;
	TupleDesc tupleDescriptor;
	
	
	tupleDescriptor = hashslot->tts_tupleDescriptor;
	attrs = tupleDescriptor->attrs;
	natts = tupleDescriptor->natts;
	
	len = offsetof(HeapTupleHeaderData, t_bits);
	len += BITMAPLEN(natts);
	
	if (tupleDescriptor->tdhasoid)
		len += sizeof(Oid);
	
	len = MAXALIGN(len);
	
	/* Add data sizes to len. */
	foreach( lc, hash_needed )
	{
		int varNumber = lfirst_int(lc) - 1;
		attr = attrs[varNumber];
		
		Assert( !attr->attisdropped );
		
		len = att_align(len, attr->attalign);
		len += get_typavgwidth(attr->atttypid, attr->atttypmod);
	}

	len = MAXALIGN(HEAPTUPLESIZE + len);
	
	return len;
}

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
    HashAggTableSizes   hats;
	MemoryContext oldcontext;
	Agg *agg = (Agg*)aggstate->ss.ps.plan;
	
	
	if (!calcHashAggTableSizes(1024.0 * (double)work_mem,
							   (double)agg->numGroups,
							   aggstate->numaggs,
							   est_hash_tuple_size(aggstate->ss.ss_ScanTupleSlot, aggstate->hash_needed),
							   agg->transSpace,
							   true,
							   &hats))
	{
		elog(ERROR, "unable to implement hashed aggregation strategy");
	}
	
	/* NOTE: This is a good place to force the hash table parameters --
	 *       est_nbucket, est_nentry, and est_nbatch -- for testing the
	 *       algorithm.  For example,
	 *
	 *				est_nbucket = 5;
	 *				est_nentry = 10;
	 *				est_nbatch = 2;
	 *       will provoke queries with more than 20 input and output
	 *		 tuples to operate in 2 passes and to spill tuples back
	 *       and forth between batch files a few times.
	 *
	 *				est_nbucket = 10;
	 *				est_nentry = 100;
	 *				est_nbatch = 10;
	 *       will provoke queries with about 1000 input tuples and
	 *		 100 plus a few output tuples to spread their few overflow
	 *		 tuples thinly across the 9 primary overflow batches,
	 *		 probably leaving some batches empty.
	 *
	 *		 In actual practice, we want to maximize memory used by the
	 *       hash while producing overflow batches that we expect will
	 *		 fill (but not overflow) the hash table.  This is just what
	 *		 function calcHashAggTableSizes is for.
	 */
	
	oldcontext = MemoryContextSwitchTo(aggstate->aggcontext);
	
	ht = (HashAggTable*)palloc0(sizeof(HashAggTable));
	
    ht->nbucket = hats.nbuckets;
	ht->buckets = (HashAggEntry**)palloc0(ht->nbucket * sizeof(HashAggEntry*));
	
	ht->max_entries = hats.nentries;
	ht->nentries = 0;
	
	create_batches(ht, hats.nbatches);
	
	/* To calculate a batch index, we use the hash key after rotating
	 * the bits used in calculating the bucket to the front.  These
	 * are the shift arguments.
	 */
#if defined(_WIN32) || defined(__FreeBSD__)
	ht->lshift = ceil(log(ht->nbucket)/log(2));	/* No log2 function. */
#else
	ht->lshift = ceil(log2(ht->nbucket));
#endif
	ht->rshift = 32 - ht->lshift;
	
	ht->ngroup = ht->ntuple = 0;
	
	ht->state = HASHAGG_BEFORE_FIRST_PASS;
	ht->pass = 0;
	
	MemoryContextSwitchTo(oldcontext);
	
	init_agg_hash_iter(ht);
	
    ht->hats = hats;

    return ht;
}


/* Function: reset_agg_hash_table
 *
 * Clear the hash table content anchored by the bucket array.
 */
void reset_agg_hash_table(AggState *aggstate)
{
	HashAggTable *ht = aggstate->hhashtable;
	
	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: resetting %d-entry hash table",
		ht->nentries);
	
	MemSet(ht->buckets, 0, ht->nbucket * sizeof(HashAggEntry*));
	MemoryContextReset(aggstate->transcontext);
	ht->nentries = 0;
	init_agg_hash_iter(ht);
}

/* Function: destroy_agg_hash_table
 *
 * Give back the resources anchored by the hash table.  Ok to call, even
 * if the hash table isn't set up.
 */
void destroy_agg_hash_table(AggState *aggstate)
{
	Agg *agg = (Agg*)aggstate->ss.ps.plan;
	
	if ( agg->aggstrategy == AGG_HASHED && aggstate->hhashtable != NULL )
	{
		reset_agg_hash_table(aggstate);

		elog(HHA_MSG_LVL,
			"hybrid hash aggregation: destroying hash table -- ngroup=%ld ntuple=%ld",
			aggstate->hhashtable->ngroup,
			aggstate->hhashtable->ntuple);
		
		destroy_batches(aggstate->hhashtable);
		pfree(aggstate->hhashtable->buckets);
		pfree(aggstate->hhashtable);
		aggstate->hhashtable = NULL;
	}
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

	elog(HHA_MSG_LVL,
		 "hybrid hash aggregation: initial pass -- beginning to load hash table");

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

		if (entry != NULL)
		{
			if (isNew)
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
			uint32 rotkey;
			unsigned batch;

			if (!spilling)
			{
				double act_tuples_per_group;
				double groups, new_batches;
				double entry_size;

				PlanState *outer = outerPlanState(aggstate);
				double est_tuples_per_group;

				spilling = true;

				elog(HHA_MSG_LVL,
					 "hybrid hash aggregation: initial pass -- beginning to spill to disk");

				/* We can re-evaluate density, and may decide to
				 * increase/decrease the number of batches.
				 * 
				 * NOTE: increasing the number of batches *will*
				 * exceed the memory quota.
				 */
				if (gp_hashagg_recalc_density)
				{
					act_tuples_per_group = ((double)ht->ntuple)/(double)ht->ngroup;

					est_tuples_per_group = outer->plan->plan_rows / aggstate->ss.ps.plan->plan_rows;

					/* assume that we're 10% ? */
					groups = Max((outer->plan->plan_rows /* est_tuples */ / act_tuples_per_group),
								 (10.0 * (double)ht->ntuple / act_tuples_per_group));

					elog(HHA_MSG_LVL,
						 "hybrid hash aggregation: recalculate batch count using %f groups (est density %f act density %f)",
						 groups, est_tuples_per_group, act_tuples_per_group);

					entry_size = ht->hats.workmem_per_entry;

					new_batches = calcBatchCount(work_mem * 1024.0, 
												 groups,
												 entry_size,
												 sizeBufFile(NULL),
												 true);

					if (new_batches > ht->nbatch)
					{
						/* Don't use more than 1024 batches if we weren't already using lots of batches */
						if (new_batches > 1024 && ht->nbatch <= 512)
							new_batches = 1024;

						elog(HHA_MSG_LVL,
							 "hybrid hash aggregation: increasing batch count from %d -> %f",
							 ht->nbatch, new_batches);
						destroy_batches(ht);
						create_batches(ht, new_batches);
					}
				}
				/* NOTE: we never reduce the number of batches, respill passes are too risky */
			}
			noverflow++;

			rotkey = hashkey << ht->lshift | hashkey >> ht->rshift;
			batch = 1 + rotkey % (ht->nbatch-1);

			Assert(batch > 0 && batch < ht->nbatch);
			
			/* Write the tuple to the appropriate overflow batch. */
			batch_put_tuple(ht, batch, outerslot);
		}
	}

	/* Initialize to walk the hash table */
	init_agg_hash_iter(aggstate->hhashtable);
	
	elog(HHA_MSG_LVL,
		 "hybrid hash aggregation: %ld tuples loaded to %ld hash table entries",
		 ht->ntuple, ht->ngroup);
	elog(HHA_MSG_LVL,
		 "hybrid hash aggregation: %ld tuples spilled to disk",
		 noverflow);

    /* CDB: Report statistics for EXPLAIN ANALYZE. */
    if (aggstate->ss.ps.instrument)
    {
        ht->num_groups_total = ht->ngroup;
        ht->ngroup_initial = ht->ngroup;
        ht->ntuple_initial = ht->ntuple;
        ht->noverflow_initial = noverflow;
        agg_hash_table_stat_upd(ht);
    }
}


/* Function: agg_hash_next_pass
 *
 * Performs ExecAgg initialization for the second and subsequent passes
 * of the hashed case via function exec_hybrid_hash_agg():
 * - clears the previous (exhausted) hash table and initialize a new one,
 * - reads the input tuples for the next batch,
 * - builds a hash table with an entry per group,
 * - spills tuples for groups that won't fit in the hash table to an
 *   overflow batch to process later in the same pass (following a call
 *   to agg_hash_continue_pass).
 */
bool
agg_hash_next_pass(AggState *aggstate)
{
	HashAggTable *ht = aggstate->hhashtable;
	unsigned int pass = aggstate->hhashtable->pass;
	unsigned int nbatch = aggstate->hhashtable->nbatch;
	BatchFileInfo *batches = aggstate->hhashtable->batches;
	
	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: pass %d -- advancing to next pass",
		aggstate->hhashtable->pass);

	reset_agg_hash_table(aggstate);

	for ( pass++; pass < nbatch; pass++ )
	{
		if ( batches[pass].file != NULL )
		{
			Assert(batches[pass].ntuples > 0);
			break;
		}
	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: pass %d -- no tuples",
		pass);
	}

	if ( pass < nbatch )
	{
		elog(HHA_MSG_LVL,
			"hybrid hash aggregation: pass %d -- %ld spilled tuples",
			pass, batches[pass].ntuples);	
		
		aggstate->hhashtable->source = aggstate->hhashtable->pass = pass;
		aggstate->hhashtable->overflow = 0;
		return agg_hash_reload(aggstate);
	}
	
	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: end of passes");	

    /* CDB: Report statistics for EXPLAIN ANALYZE. */
    if (aggstate->ss.ps.instrument)
    {
        Instrumentation    *instr = aggstate->ss.ps.instrument;
        StringInfo          hbuf = aggstate->ss.ps.cdbexplainbuf;
        Size                wmem;

        /* work_mem setting that would suffice to hold all groups in memory */
        if (ht->ngroup > ht->max_entries)
        {
            wmem = ht->ngroup * ht->hats.workmem_per_entry +
                   ht->hats.workmem_initial;
            instr->workmemwanted = Max(instr->workmemwanted, wmem);
        }

        /* work_mem quota to accommodate number of entries in initial pass */
        wmem = ht->ngroup_initial * ht->hats.workmem_per_entry + 
               ht->hats.workmem_initial;
        instr->workmemused = Max(instr->workmemused, wmem);

        /* Summary */
        if (ht->ngroup < ht->num_groups_total ||
            ht->num_reloads > 0)
        {
            /* Note change in terminology for user consumption: batch vs pass */
            appendStringInfo(hbuf, 
                             "%.0f groups total in %d batches"
                             "; %d respill passes" 
                             "; %.0f respill rows.",
                             ht->num_groups_total,
                             nbatch,
                             1 + ht->num_reloads - nbatch,
                             ht->num_overflow_rows);
            appendStringInfoChar(hbuf, '\n');
        }

        /* Initial pass */
        if (ht->noverflow_initial > 0)
            appendStringInfo(hbuf,
                             "Initial pass:  %ld groups made from %ld rows; "
                             "%ld rows spilled to workfile.\n",
                             ht->ngroup_initial,
                             ht->ntuple_initial,
                             ht->noverflow_initial);

        /* Hash chain statistics */
        if (ht->chainlength.vcnt > 0)
            appendStringInfo(hbuf,
                             "Hash chain length %.1f avg, %.0f max,"
                             " using %d of %d buckets.\n",
                             cdbexplain_agg_avg(&ht->chainlength),
                             ht->chainlength.vmax,
                             ht->chainlength.vcnt,
                             ht->nbucket * (ht->num_reloads + 1));
    }

	return false;
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

	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: pass %d -- continuing",
		aggstate->hhashtable->pass);
	
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
	unsigned int source, overflow;
	HashAggEntry *entry;
	TupleTableSlot *outerslot;
	unsigned long ntuple = 0;
	unsigned long ngroup = 0;
	unsigned long noverflow = 0;

	/*
	 * get state info from node
	 */
	ht = aggstate->hhashtable;
	batches = ht->batches;
	source = ht->source;
	overflow = ht->overflow;
	tmpcontext = aggstate->tmpcontext; /* per-input-tuple expression context */

	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: pass %d -- reloading hash table, source batch=%d, overflow batch=%d",
		ht->pass, source, overflow);
	
	 /* If batch aggstate->hhashtable->overflow isn't empty, the caller is confused. */
	Assert( batches[overflow].file == NULL );
	
	if ( batches[source].file == NULL )
	{
		elog(HHA_MSG_LVL,
			"hybrid hash aggregation: pass %d -- source batch is empty",
			ht->pass);		
		return false; /* No more tuples in batch */
	}	

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
			ntuple++;
			
			if ( isNew )
			{
				ngroup++;
				initialize_aggregates(aggstate, aggstate->peragg, entry->pergroup);
			}
				
			/* Advance the aggregates */
			advance_aggregates(aggstate, entry->pergroup);

			/* Reset per-input-tuple context after each tuple */
			ResetExprContext(tmpcontext);
		}
		else
		{			
			/* Write the input tuple to the overflow. */
			noverflow++;
			batch_put_tuple(ht, overflow, outerslot);
		}
	}
	ht->ntuple += ntuple;
	ht->ngroup += ngroup;

	/* Initialize to walk the hash table */
	init_agg_hash_iter(ht);	
	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: %ld tuples reloaded to %ld hash table entries",
		ntuple, ngroup);
	elog(HHA_MSG_LVL,
		"hybrid hash aggregation: %ld tuples respilled to disk",
		noverflow);
	
    /* CDB: Report statistics for EXPLAIN ANALYZE. */
    if (aggstate->ss.ps.instrument)
    {
        Instrumentation    *instr = aggstate->ss.ps.instrument;
        Size                workmemused;

        ht->num_reloads++;
        ht->num_overflow_rows += noverflow;
        ht->num_groups_total += ngroup;

        /* work_mem quota to accommodate number of entries in this pass */
        workmemused = ngroup * ht->hats.workmem_per_entry +
                      ht->hats.workmem_initial;
        instr->workmemused = Max(instr->workmemused, workmemused);

        /* hash chain statistics */
        agg_hash_table_stat_upd(ht);
    }

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
 * Function: calc_hash_value
 *
 * Calculate the hash value for the given input tuple.
 *
 * This based on but different from get_hash_value from the dynahash
 * API.  Use a different name to underline that we don't use dynahash.
 */
uint32
calc_hash_value(AggState* aggstate, TupleTableSlot *inputslot)
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
 * Returns a pointer to the old or new hash table entry corresponding
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
	unsigned int bucket_idx;
	bool match;
	
	agg = (Agg*)aggstate->ss.ps.plan;
	econtext = aggstate->tmpcontext; /* short-lived, per-input-tuple */
	ht = aggstate->hhashtable;
	entry_tuple_desc = aggstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	
	/* if first time through, initialize hashslot by cloning input slot.
	 * This is essential for proper functioning of calc_hash_value, etc. */
	if (hashslot->tts_tupleDescriptor == NULL)
	{
		ExecSetSlotDescriptor(hashslot, inputslot->tts_tupleDescriptor);
		/* Make sure all unused columns are NULLs */
		ExecStoreAllNullTuple(hashslot);
	}

	ResetExprContext(econtext);
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* Get hash key and its bucket index. */
	hashkey = calc_hash_value(aggstate, inputslot);
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
 * Allocate a new hash agg entry for the given input tuple and hash key
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
	
	/* For now we just allocate single entries, but maybe we should consider
	 * allocating them all at once (since we know how many we're willing to
	 * hold in memory) or in blocks (to avoid tying up more virtual memory
	 * than necessary).  We should also consider the impact of holding on
	 * to hash entry memory longer than necessary.  Right now we release the
	 * contents of the hash table all at once, but (for a bit of extra
	 * processing  time) we could release them piecemeal, hopefully, to release
	 * memory for use by other QEs.
	 */
	MemoryContextSwitchTo(aggstate->transcontext);
	
	if (aggstate->numaggs == 0)
		entry = (HashAggEntry*)palloc0(sizeof(HashAggEntry));
	else
		entry = (HashAggEntry*)palloc0(sizeof(HashAggEntry) +
						(aggstate->numaggs-1) * sizeof(AggStatePerGroupData));
	entry->heaptuple = ExecCopySlotTuple(hashslot);
	entry->hashvalue = hashvalue;
	
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
 * This function must agree with the other code regarding memory usage,
 * e.g., for hash entries (execHashagg.c:makeHashAggEntry), batch files
 * (buffile.c:makeBufFile), etc.
 *
 * The approach taken is to try to minimize number of batches (nB)
 * thereby maximizing number of hash entries (nH) within the given
 * memory quota (usually just work_mem in bytes).
 *
 * Very roughly, with entry sizes zB and zH and memory quota Q,
 * nH = (Q - zB*nB)/zH.
 *
 * Memory used will be zB*nB + zH*nH = zB*nB + (Q-zB*nB).  We want this
 * to approximate Q.  Q =  zB*nB + (Q-zB*nB) is quadratic in nB and may
 * be solved by formula.
 *
 * The actual estimate is a little more complex.
 */

static double
calcBatchCount(double memquota, double ngroups, double zH, double zB, bool force)
{
	double nB = 2.0;
	double rewrite_fudge = 1.0;

	/* The normal stategy expects to spill to disk, so we estimate
	 * how many passes we expect to need and allocate that many
	 * batches.  Since the first pass will load the hash table
	 * rather than a batch file, this leaves a batch file for
	 * overflow within a subsequent pass.
	 */
	double d = memquota*memquota - 4 * zB * ngroups * zH;

	/* By default we plan on writing input tuples back to disk
	 * once, but the user can increase this.  We creep up to
	 * user's limit in order the choose the smallest rewrite
	 * limit that will work.  (This tends to minimize the number
	 * of rewrites at the expense of a larger number of passes.
	 */
	while (d < 0 && rewrite_fudge < gp_hashagg_rewrite_limit)
	{
		rewrite_fudge++;
		rewrite_fudge = (rewrite_fudge > gp_hashagg_rewrite_limit) ?
			gp_hashagg_rewrite_limit :
			rewrite_fudge;
			
		d = memquota*memquota - (4 * zB * ngroups * zH)/rewrite_fudge;
	}
		
	/* The optimizer can decide against the strategy, but the executor
	 * has to go with the choice even if it doesn't look so good.
	 */
	if (d < 0)
	{
		if (force)
		{
			elog(DEBUG2, "hashagg: even after applying fudge, can't fit groups");
			d = 0;
		}
		else
		{
			elog(DEBUG2, "hashagg can't calculate memory usage!");
			return 0.0;
		}
	}

	nB = ceil((memquota-sqrt(d))/(2*zB));
		
	nB = Max(2.0, nB);

	elog(DEBUG1, "hashagg rewrite-fudge %f", rewrite_fudge);

	return nB;
}

#define NGROUP_PER_BUCKET 7.0
#define OVERHEAD_PER_ENTRY ((double)sizeof(uint32))/NGROUP_PER_BUCKET

bool
calcHashAggTableSizes(double memquota,	/* Memory quota in bytes. */
					  double ngroups,	/* Est # of groups. */
					  int numaggs,		/* Est # of aggregate functions */
					  int keywidth,	/* Est per entry size of hash key. */
					  int transpace,	/* Est per entry size of by-ref values. */
					  bool force,      /* true => succeed even if work_mem too small */
					  HashAggTableSizes   *out_hats)
{
	bool expectSpill = false;

	int entrywidth = sizeof(HashAggEntry)
		+ numaggs * sizeof(AggStatePerGroupData)
		+ keywidth
		+ transpace;
	
	double nK = ceil(ngroups/NGROUP_PER_BUCKET); /* Buckets */
	
	double nH, zH = OVERHEAD_PER_ENTRY + entrywidth; /* Hash Entries */
	
	double nB = 2.0;
	double zB = sizeBufFile(NULL); /* Passes = overflow batches */

	elog(DEBUG2, "hashagg ngroups = %g, numaggs = %d, keywidth = %d, transpace = %d", ngroups, numaggs, keywidth, transpace);
	elog(DEBUG2, "hashagg memquota = %g, zH = %g, zB = %g", memquota, zH, zB);


	if (nK > UINT_MAX || ngroups > UINT_MAX)
	{
		elog(DEBUG2, "hashagg table size bigger than int type!");
		return false;
	}

	/* Use default strategy when we expect the whole operation to
	 * fit in memory.  This strategy hedges its bet by allocating
	 * 2 batches in case the estimate is low and we overflow anyway.
	 *
	 * If we don't fit in memory we pick the number of batches such
	 * that each batch will fit in memory.
	 *
	 * Mq - 2zB >= zH * ngroups/nB
	 * solve for nB:
	 *    nB >= zH * ngroups / (Mq - 2zB)
	 */			
	if (memquota < ngroups*zH + 2*zB)
	{
		nB = calcBatchCount(memquota, ngroups, zH, zB, force);

		if (nB < 2.0)
			return false;
	}   

	nH = floor((memquota - nB*zB)/zH);
	
    /* Allocate at least a few hash entries regardless of memquota. */
    nH = Max(nH, NGROUP_PER_BUCKET);

	nK = ceil(nH/NGROUP_PER_BUCKET);

	if (nB > UINT_MAX || nH > UINT_MAX || nK > UINT_MAX)
	{
		if (force)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("hybrid hash aggregation faillure - too many passes or hash entries")));
		}
		else
		{
			elog(DEBUG2, "number of passes or hash entries bigger than int type!");
			return false; /* Too many groups. */
		}
	}
	
    if (out_hats)
    {
        out_hats->nbuckets = (unsigned)nK;
        out_hats->nentries = (unsigned)nH;
		out_hats->nbatches = (unsigned)nB;
		out_hats->spill = expectSpill;
        out_hats->workmem_initial = (unsigned)(nB*zB);
        out_hats->workmem_per_entry = (unsigned)zH;
    }
	
	elog(DEBUG2, "hashagg nbuckets = %d, nentries = %d, nbatches = %d", (int)nK, (int)nH, (int)nB);
	elog(DEBUG1, "hashagg expected memory footprint = %d",
		(int)( nH*entrywidth + nK*sizeof(HashAggEntry*) + nB*zB));
	
	return true;
}

/* Function: create_batches
 *
 * Initialize HashAggTable with the specified number of batch file slots.
 */
void
create_batches(HashAggTable *hashtable, unsigned int nbatch)
{
	size_t sz;
	
	Assert(nbatch > 1 && hashtable->batches == NULL);

	sz = nbatch * sizeof(BatchFileInfo);	
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
	unsigned int i;
	
	for (i = 0; i < hashtable->nbatch; i++)
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
			   unsigned int batch,
			   TupleTableSlot *tupleSlot)
{
	size_t		written;
	BatchFileInfo *info;
	BufFile    *file;
	BatchTupleHeader hth;
	HeapTupleHeader t_data;
	unsigned int	datalen;
	unsigned int	nullslen;
	/* HeapTuple heapTuple = tupleSlot->tts_tuple; */
	HeapTuple heapTuple = ExecFetchSlotTuple(tupleSlot);
	
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
		nullslen = BITMAPLEN(HeapTupleHeaderGetNatts(t_data));
	else
		nullslen = 0;

	hth.tuplen = sizeof(BatchTupleHeader) + nullslen + datalen;
	hth.natts = HeapTupleHeaderGetNatts(t_data);
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
				unsigned int batch,
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
	HeapTupleHeaderSetNatts(t_data, hth.natts);
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

/*
 * batch_rewind
 *		reposition batch so next read will start at the beginning
 */
void
batch_rewind(HashAggTable *hashtable, unsigned int batch)
{
	int n;
	BatchFileInfo *info;
	
	Assert(hashtable != NULL && 0 <= batch && batch < hashtable->nbatch);
	
	info = &hashtable->batches[batch];
	
	if ( info->file == NULL )
	{
		Assert(info->ntuples == 0);
		return;
	}
	
	n = BufFileSeek(info->file, 0, 0L, 0);
	
	Assert( n == 0);
}


/*
 * batch_close
 *		close a batch file thus releasing its file and memory space
 */
void
batch_close(HashAggTable *hashtable, unsigned int batch)
{
	BatchFileInfo *info;
	
	Assert(hashtable != NULL && 0 <= batch && batch < hashtable->nbatch);
	
	info = &hashtable->batches[batch];
	
	if (info->file != NULL)
	{
		BufFileClose(info->file);
		info->file = NULL;
	}
	
	info->ntuples = 0;
}


/*
 * agg_hash_table_stat_upd
 *      collect hash chain statistics for EXPLAIN ANALYZE
 */
void
agg_hash_table_stat_upd(HashAggTable *ht)
{
    unsigned int	i;

    for (i = 0; i < ht->nbucket; i++)
    {
        HashAggEntry   *entry = ht->buckets[i];
        int             chainlength = 0;

        if (entry)
        {
            for (chainlength = 0; entry; chainlength++)
                entry = entry->next;
            cdbexplain_agg_upd(&ht->chainlength, chainlength, i);
        }
    }
}                               /* agg_hash_table_stat_upd */
