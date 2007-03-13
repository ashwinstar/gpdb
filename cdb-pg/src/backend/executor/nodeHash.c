/*-------------------------------------------------------------------------
 *
 * nodeHash.c
 *	  Routines to hash relations for hashjoin
 *
 * Portions Copyright (c) 2006-2007, greenplum inc
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeHash.c,v 1.107 2006/07/14 04:44:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		MultiExecHash	- generate an in-memory hash table of the relation
 *		ExecInitHash	- initialize node and subnodes
 *		ExecEndHash		- shutdown node and subnodes
 */

#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "executor/execdebug.h"
#include "executor/hashjoin.h"
#include "executor/instrument.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"


static void ExecHashIncreaseNumBatches(HashJoinTable hashtable);
static void ExecHashTableExplainEnd(PlanState *planstate, struct StringInfoData *buf);


/* ----------------------------------------------------------------
 *		ExecHash
 *
 *		stub for pro forma compliance
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecHash(HashState *node)
{
	elog(ERROR, "Hash node does not support ExecProcNode call convention");
	return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecHash
 *
 *		build hash table for hashjoin, doing partitioning if more
 *		than one batch is required.
 * ----------------------------------------------------------------
 */
Node *
MultiExecHash(HashState *node)
{
	PlanState  *outerNode;
	List	   *hashkeys;
	HashJoinTable hashtable;
	TupleTableSlot *slot;
	ExprContext *econtext;
	uint32		hashvalue;

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	/*
	 * get state info from node
	 */
	outerNode = outerPlanState(node);
	hashtable = node->hashtable;

	/*
	 * set expression context
	 */
	hashkeys = node->hashkeys;
	econtext = node->ps.ps_ExprContext;

	/*
	 * get all inner tuples and insert into the hash table (or temp files)
	 */
	for (;;)
	{
		slot = ExecProcNode(outerNode);
		if (TupIsNull(slot))
			break;
		hashtable->totalTuples += 1;
		/* We have to compute the hash value */
		econtext->ecxt_innertuple = slot;
		hashvalue = ExecHashGetHashValue(hashtable, econtext, hashkeys);
		ExecHashTableInsert(hashtable, slot, hashvalue);
	}

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, hashtable->totalTuples);

	/*
	 * We do not return the hash table directly because it's not a subtype of
	 * Node, and so would violate the MultiExecProcNode API.  Instead, our
	 * parent Hashjoin node is expected to know how to fish it out of our node
	 * state.  Ugly but not really worth cleaning up, since Hashjoin knows
	 * quite a bit more about Hash besides that.
	 */
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitHash
 *
 *		Init routine for Hash node
 * ----------------------------------------------------------------
 */
HashState *
ExecInitHash(Hash *node, EState *estate, int eflags)
{
	HashState  *hashstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	hashstate = makeNode(HashState);
	hashstate->ps.plan = (Plan *) node;
	hashstate->ps.state = estate;
	hashstate->hashtable = NULL;
	hashstate->hashkeys = NIL;	/* will be set by parent HashJoin */

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hashstate->ps);

#define HASH_NSLOTS 1

	/*
	 * initialize our result slot
	 */
	ExecInitResultTupleSlot(estate, &hashstate->ps);

	/*
	 * initialize child expressions
	 */
	hashstate->ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) hashstate);
	hashstate->ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) hashstate);

	/*
	 * initialize child nodes
	 */
	outerPlanState(hashstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * initialize tuple type. no need to initialize projection info because
	 * this node doesn't do projections
	 */
	ExecAssignResultTypeFromTL(&hashstate->ps);
	hashstate->ps.ps_ProjInfo = NULL;

	return hashstate;
}

int
ExecCountSlotsHash(Hash *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		HASH_NSLOTS;
}

/* ---------------------------------------------------------------
 *		ExecEndHash
 *
 *		clean up routine for Hash node
 * ----------------------------------------------------------------
 */
void
ExecEndHash(HashState *node)
{
	PlanState  *outerPlan;

	/*
	 * free exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * shut down the subplan
	 */
	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}


/* ----------------------------------------------------------------
 *		ExecHashTableCreate
 *
 *		create an empty hashtable data structure for hashjoin.
 * ----------------------------------------------------------------
 */
HashJoinTable
ExecHashTableCreate(Hash *node, List *hashOperators)
{
	HashJoinTable hashtable;
	Plan	   *outerNode;
	int			nbuckets;
	int			nbatch;
	int			nkeys;
	int			i;
	ListCell   *ho;
	MemoryContext oldcxt;

	/*
	 * Get information about the size of the relation to be hashed (it's the
	 * "outer" subtree of this node, but the inner relation of the hashjoin).
	 * Compute the appropriate size of the hash table.
	 */
	outerNode = outerPlan(node);

	ExecChooseHashTableSize(outerNode->plan_rows, outerNode->plan_width,
							&nbuckets, &nbatch);

#ifdef HJDEBUG
    elog(DEBUG4, "HJ: nbatch = %d, nbuckets = %d\n", nbatch, nbuckets);
#endif

	/*
	 * Initialize the hash table control block.
	 *
	 * The hashtable control block is just palloc'd from the executor's
	 * per-query memory context.
	 */
	hashtable = (HashJoinTable) palloc(sizeof(HashJoinTableData));
	hashtable->nbuckets = nbuckets;
	hashtable->buckets = NULL;
	hashtable->nbatch = nbatch;
	hashtable->curbatch = 0;
	hashtable->nbatch_original = nbatch;
	hashtable->nbatch_outstart = nbatch;
	hashtable->growEnabled = true;
	hashtable->totalTuples = 0;
	hashtable->innerBatchFile = NULL;
	hashtable->outerBatchFile = NULL;
	hashtable->spaceUsed = 0;
	hashtable->spaceAllowed = work_mem * 1024L;
    hashtable->spaceFreed = 0;
    hashtable->stats = NULL;

	/*
	 * Get info about the hash functions to be used for each hash key.
	 */
	nkeys = list_length(hashOperators);
	hashtable->hashfunctions = (FmgrInfo *) palloc(nkeys * sizeof(FmgrInfo));
	i = 0;
	foreach(ho, hashOperators)
	{
		Oid			hashfn;

		hashfn = get_op_hash_function(lfirst_oid(ho));
		if (!OidIsValid(hashfn))
			elog(ERROR, "could not find hash function for hash operator %u",
				 lfirst_oid(ho));
		fmgr_info(hashfn, &hashtable->hashfunctions[i]);
		i++;
	}

	/*
	 * Create temporary memory contexts in which to keep the hashtable working
	 * storage.  See notes in executor/hashjoin.h.
	 */
	hashtable->hashCxt = AllocSetContextCreate(CurrentMemoryContext,
											   "HashTableContext",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	hashtable->batchCxt = AllocSetContextCreate(hashtable->hashCxt,
												"HashBatchContext",
												ALLOCSET_DEFAULT_MINSIZE,
												ALLOCSET_DEFAULT_INITSIZE,
												ALLOCSET_DEFAULT_MAXSIZE);

	/* Allocate data that will live for the life of the hashjoin */

	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	if (nbatch > 1)
	{
		/*
		 * allocate and initialize the file arrays in hashCxt
		 */
		hashtable->innerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		/* The files will not be opened until needed... */
	}

	/*
	 * Prepare context for the first-scan space allocations; allocate the
	 * hashbucket array therein, and set each bucket "empty".
	 */
	MemoryContextSwitchTo(hashtable->batchCxt);

	hashtable->buckets = (HashJoinTuple *)
		palloc0(nbuckets * sizeof(HashJoinTuple));

	MemoryContextSwitchTo(oldcxt);

	return hashtable;
}


/*
 * Compute appropriate size for hashtable given the estimated size of the
 * relation to be hashed (number of rows and average row width).
 *
 * This is exported so that the planner's costsize.c can use it.
 */

/* Target bucket loading (tuples per bucket) */
#define NTUP_PER_BUCKET			10

/* Prime numbers that we like to use as nbuckets values */
static const int hprimes[] = {
	1033, 2063, 4111, 8219, 16417, 32779, 65539, 131111,
	262151, 524341, 1048589, 2097211, 4194329, 8388619, 16777289, 33554473,
	67108913, 134217773, 268435463, 536870951, 1073741831
};

void
ExecChooseHashTableSize(double ntuples, int tupwidth,
						int *numbuckets,
						int *numbatches)
{
	int			tupsize;
	double		inner_rel_bytes;
	long		hash_table_bytes;
	int			nbatch;
	int			nbuckets;
	int			i;

	/* Force a plausible relation size if no info */
	if (ntuples <= 0.0)
		ntuples = 1000.0;

	/*
	 * Estimate tupsize based on footprint of tuple in hashtable... note this
	 * does not allow for any palloc overhead.	The manipulations of spaceUsed
	 * don't count palloc overhead either.
	 */
	tupsize = HJTUPLE_OVERHEAD +
		MAXALIGN(sizeof(MinimalTupleData)) +
		MAXALIGN(tupwidth);
	inner_rel_bytes = ntuples * tupsize;

	/*
	 * Target in-memory hashtable size is work_mem kilobytes.
	 */
	hash_table_bytes = work_mem * 1024L;

	/*
	 * Set nbuckets to achieve an average bucket load of NTUP_PER_BUCKET when
	 * memory is filled.  Set nbatch to the smallest power of 2 that appears
	 * sufficient.
	 */
	if (inner_rel_bytes > hash_table_bytes)
	{
		/* We'll need multiple batches */
		long		lbuckets;
		double		dbatch;
		int			minbatch;

		lbuckets = (hash_table_bytes / tupsize) / NTUP_PER_BUCKET;
		lbuckets = Min(lbuckets, INT_MAX);
		nbuckets = (int) lbuckets;

		dbatch = ceil(inner_rel_bytes / hash_table_bytes);
		dbatch = Min(dbatch, INT_MAX / 2);
		minbatch = (int) dbatch;
		nbatch = 2;
		while (nbatch < minbatch)
			nbatch <<= 1;
	}
	else
	{
		/* We expect the hashtable to fit in memory, we want to use
		 * more buckets if we have memory to spare */
		double		dbuckets_lower;
		double		dbuckets_upper;
		double		dbuckets;

		/* divide our tuple row-count estimate by our the number of
		 * tuples we'd like in a bucket: this produces a small bucket
		 * count independent of our work_mem setting */
		dbuckets_lower = (double)ntuples / (double)NTUP_PER_BUCKET;

		/* if we have work_mem to spare, we'd like to use it -- so
		 * divide up our memory evenly (see the spill case above) */
		dbuckets_upper = (double)hash_table_bytes / ((double)tupsize * NTUP_PER_BUCKET);

		/* we'll use our "lower" work_mem independent guess as a lower
		 * limit; but if we've got memory to spare we'll take the mean
		 * of the lower-limit and the upper-limit */
		if (dbuckets_upper > dbuckets_lower)
			dbuckets = (dbuckets_lower + dbuckets_upper)/2.0;
		else
			dbuckets = dbuckets_lower;

		dbuckets = ceil(dbuckets);
		dbuckets = Min(dbuckets, INT_MAX);

		nbuckets = (int)dbuckets;

		nbatch = 1;
	}

	/*
	 * We want nbuckets to be prime so as to avoid having bucket and batch
	 * numbers depend on only some bits of the hash code.  Choose the next
	 * larger prime from the list in hprimes[].  (This also enforces that
	 * nbuckets is not very small, by the simple expedient of not putting any
	 * very small entries in hprimes[].)
	 */
	for (i = 0; i < (int) lengthof(hprimes); i++)
	{
		if (hprimes[i] >= nbuckets)
		{
			nbuckets = hprimes[i];
			break;
		}
	}

	*numbuckets = nbuckets;
	*numbatches = nbatch;
}


/* ----------------------------------------------------------------
 *		ExecHashTableDestroy
 *
 *		destroy a hash table
 * ----------------------------------------------------------------
 */
void
ExecHashTableDestroy(HashJoinTable hashtable)
{
	int			i;

	/*
	 * Make sure all the temp files are closed.  We skip batch 0, since it
	 * can't have any temp files (and the arrays might not even exist if
	 * nbatch is only 1).
	 */
	for (i = 1; i < hashtable->nbatch; i++)
	{
		if (hashtable->innerBatchFile[i])
			BufFileClose(hashtable->innerBatchFile[i]);
		if (hashtable->outerBatchFile[i])
			BufFileClose(hashtable->outerBatchFile[i]);
	}

	/* Release working memory (batchCxt is a child, so it goes away too) */
	MemoryContextDelete(hashtable->hashCxt);

	/* And drop the control block */
	pfree(hashtable);
}

/*
 * ExecHashIncreaseNumBatches
 *		increase the original number of batches in order to reduce
 *		current memory consumption
 */
static void
ExecHashIncreaseNumBatches(HashJoinTable hashtable)
{
	int			oldnbatch = hashtable->nbatch;
	int			curbatch = hashtable->curbatch;
	int			nbatch;
	int			i;
	MemoryContext oldcxt;
	long		ninmemory;
	long		nfreed;
    Size        spaceFreed = 0;
    Size        sz;
    int         batchnomin = -1;
    int         batchnomax = -1;

	/* do nothing if we've decided to shut off growth */
	if (!hashtable->growEnabled)
		return;

	/* safety check to avoid overflow */
	if (oldnbatch > INT_MAX / 2)
		return;

	nbatch = oldnbatch * 2;
	Assert(nbatch > 1);

    /* Note work_mem high-water mark before freeing anything. */
    if (hashtable->stats)
        hashtable->stats->workmem_max =
            Max(hashtable->stats->workmem_max,
                hashtable->spaceUsed - hashtable->spaceFreed);

#ifdef HJDEBUG
    elog(DEBUG3, "HJ: Increasing nbatch to %d because space = %lu",
		   nbatch, (unsigned long) hashtable->spaceUsed);
#endif

	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	if (hashtable->innerBatchFile == NULL)
	{
		/* we had no file arrays before */
		hashtable->innerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
	}
	else
	{
		/* enlarge arrays and zero out added entries */
		hashtable->innerBatchFile = (BufFile **)
			repalloc(hashtable->innerBatchFile, nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			repalloc(hashtable->outerBatchFile, nbatch * sizeof(BufFile *));
		MemSet(hashtable->innerBatchFile + oldnbatch, 0,
			   (nbatch - oldnbatch) * sizeof(BufFile *));
		MemSet(hashtable->outerBatchFile + oldnbatch, 0,
			   (nbatch - oldnbatch) * sizeof(BufFile *));
	}

    if (hashtable->stats &&
        hashtable->stats->nbatchstats < nbatch)
    {
        sz = nbatch * sizeof(hashtable->stats->batchstats[0]);
        hashtable->stats->batchstats =
            (HashJoinBatchStats *)repalloc(hashtable->stats->batchstats, sz);
        sz = (nbatch - hashtable->stats->nbatchstats) * sizeof(hashtable->stats->batchstats[0]);
		memset(hashtable->stats->batchstats + hashtable->stats->nbatchstats, 0, sz);
        hashtable->stats->nbatchstats = nbatch;
    }

    MemoryContextSwitchTo(oldcxt);

	hashtable->nbatch = nbatch;

	/*
	 * Scan through the existing hash table entries and dump out any that are
	 * no longer of the current batch.
	 */
	ninmemory = nfreed = 0;

	for (i = 0; i < hashtable->nbuckets; i++)
	{
		HashJoinTuple prevtuple;
		HashJoinTuple tuple;

		prevtuple = NULL;
		tuple = hashtable->buckets[i];

		while (tuple != NULL)
		{
			/* save link in case we delete */
			HashJoinTuple nexttuple = tuple->next;
			int			bucketno;
			int			batchno;

			ninmemory++;
			ExecHashGetBucketAndBatch(hashtable, tuple->hashvalue,
									  &bucketno, &batchno);
			Assert(bucketno == i);
			if (batchno == curbatch)
			{
				/* keep tuple */
				prevtuple = tuple;
			}
			else
			{
				/* dump it out */
				Assert(batchno > curbatch);

                /* save lowest and highest new batch number */
                if (batchnomax < batchno)
                {
                    batchnomax = batchno;
                    if (batchnomin < 0)
                        batchnomin = batchno;
                }
                else if (batchnomin > batchno)
                    batchnomin = batchno;

				ExecHashJoinSaveTuple(HJTUPLE_MINTUPLE(tuple),
									  tuple->hashvalue,
									  &hashtable->innerBatchFile[batchno]);
				/* and remove from hash table */
				if (prevtuple)
					prevtuple->next = nexttuple;
				else
					hashtable->buckets[i] = nexttuple;
				/* prevtuple doesn't change */
				spaceFreed += HJTUPLE_OVERHEAD + HJTUPLE_MINTUPLE(tuple)->t_len;
				pfree(tuple);
				nfreed++;
			}

			tuple = nexttuple;
		}
	}

    /* Report the overflow for EXPLAIN ANALYZE. */
    if (hashtable->stats)
    {
        StringInfo  buf = hashtable->stats->joinexplainbuf;

        appendStringInfo(buf, "Hash table > work_mem.  ");
        appendStringInfo(buf, "%ld rows (%ldK bytes) of inner table shifted",
                         nfreed,
                         (long)((spaceFreed+1023)/1024));
        if (batchnomin < batchnomax)
            appendStringInfo(buf, " into batches %d..%d", batchnomin, batchnomax);
        else if (batchnomax >= 0)
            appendStringInfo(buf, " into batch %d", batchnomax);
        appendStringInfo(buf, ", leaving %ld rows (%ldK bytes) in batch %d.\n",
                         ninmemory - nfreed,
                         (long)((hashtable->spaceUsed - hashtable->spaceFreed -
                                    spaceFreed + 1023)/1024),
                         curbatch);
    }

#ifdef HJDEBUG
    elog(DEBUG3, "HJ: Freed %ld of %ld tuples, %lu of %lu bytes, space now %lu",
		 nfreed,
         ninmemory,
         (unsigned long)spaceFreed,
         (unsigned long)(hashtable->spaceUsed - hashtable->spaceFreed),
         (unsigned long)(hashtable->spaceUsed - hashtable->spaceFreed - spaceFreed));
#endif

    /* Allow reuse of the space that has just been freed. */
    hashtable->spaceAllowed += spaceFreed;
    hashtable->spaceFreed += spaceFreed;

	/*
	 * If we dumped out either all or none of the tuples in the table, disable
	 * further expansion of nbatch.  This situation implies that we have
	 * enough tuples of identical hashvalues to overflow spaceAllowed.
	 * Increasing nbatch will not fix it since there's no way to subdivide the
	 * group any more finely. We have to just gut it out and hope the server
	 * has enough RAM.
	 */
	if (nfreed == 0 || nfreed == ninmemory)
	{
		hashtable->growEnabled = false;
#ifdef HJDEBUG
        elog(DEBUG3, "HJ: Disabling further increase of nbatch");
#endif
	}
}

/*
 * ExecHashTableInsert
 *		insert a tuple into the hash table depending on the hash value
 *		it may just go to a temp file for later batches
 *
 * Note: the passed TupleTableSlot may contain a regular, minimal, or virtual
 * tuple; the minimal case in particular is certain to happen while reloading
 * tuples from batch files.  We could save some cycles in the regular-tuple
 * case by not forcing the slot contents into minimal form; not clear if it's
 * worth the messiness required.
 */
void
ExecHashTableInsert(HashJoinTable hashtable,
					TupleTableSlot *slot,
					uint32 hashvalue)
{
	MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot);
	int			bucketno;
	int			batchno;

	ExecHashGetBucketAndBatch(hashtable, hashvalue,
							  &bucketno, &batchno);

	/*
	 * decide whether to put the tuple in the hash table or a temp file
	 */
	if (batchno == hashtable->curbatch)
	{
		/*
		 * put the tuple in hash table
		 */
		HashJoinTuple hashTuple;
		int			hashTupleSize;

		hashTupleSize = HJTUPLE_OVERHEAD + tuple->t_len;
		hashTuple = (HashJoinTuple) MemoryContextAlloc(hashtable->batchCxt,
													   hashTupleSize);
		hashTuple->hashvalue = hashvalue;
		memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);
		hashTuple->next = hashtable->buckets[bucketno];
		hashtable->buckets[bucketno] = hashTuple;
		hashtable->spaceUsed += hashTupleSize;
		if (hashtable->spaceUsed > hashtable->spaceAllowed)
			ExecHashIncreaseNumBatches(hashtable);
	}
	else
	{
		/*
		 * put the tuple into a temp file for later batches
		 */
		Assert(batchno > hashtable->curbatch);
		ExecHashJoinSaveTuple(tuple,
							  hashvalue,
							  &hashtable->innerBatchFile[batchno]);
	}
}

/*
 * ExecHashGetHashValue
 *		Compute the hash value for a tuple
 *
 * The tuple to be tested must be in either econtext->ecxt_outertuple or
 * econtext->ecxt_innertuple.  Vars in the hashkeys expressions reference
 * either OUTER or INNER.
 */
uint32
ExecHashGetHashValue(HashJoinTable hashtable,
					 ExprContext *econtext,
					 List *hashkeys)
{
	uint32		hashkey = 0;
	ListCell   *hk;
	int			i = 0;
	MemoryContext oldContext;

	/*
	 * We reset the eval context each time to reclaim any memory leaked in the
	 * hashkey expressions.
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	foreach(hk, hashkeys)
	{
		ExprState  *keyexpr = (ExprState *) lfirst(hk);
		Datum		keyval;
		bool		isNull;

		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		/*
		 * Get the join attribute value of the tuple
		 */
		keyval = ExecEvalExpr(keyexpr, econtext, &isNull, NULL);

		/*
		 * Compute the hash function
		 */
		if (!isNull)			/* treat nulls as having hash key 0 */
		{
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1(&hashtable->hashfunctions[i],
												keyval));
			hashkey ^= hkey;
		}

		i++;
	}

	MemoryContextSwitchTo(oldContext);

	return hashkey;
}

/*
 * ExecHashGetBucketAndBatch
 *		Determine the bucket number and batch number for a hash value
 *
 * Note: on-the-fly increases of nbatch must not change the bucket number
 * for a given hash code (since we don't move tuples to different hash
 * chains), and must only cause the batch number to remain the same or
 * increase.  Our algorithm is
 *		bucketno = hashvalue MOD nbuckets
 *		batchno = (hashvalue DIV nbuckets) MOD nbatch
 * where nbuckets should preferably be prime so that all bits of the
 * hash value can affect both bucketno and batchno.
 * nbuckets doesn't change over the course of the join.
 *
 * nbatch is always a power of 2; we increase it only by doubling it.  This
 * effectively adds one more bit to the top of the batchno.
 */
void
ExecHashGetBucketAndBatch(HashJoinTable hashtable,
						  uint32 hashvalue,
						  int *bucketno,
						  int *batchno)
{
	uint32		nbuckets = (uint32) hashtable->nbuckets;
	uint32		nbatch = (uint32) hashtable->nbatch;

	if (nbatch > 1)
	{
		*bucketno = hashvalue % nbuckets;
		/* since nbatch is a power of 2, can do MOD by masking */
		*batchno = (hashvalue / nbuckets) & (nbatch - 1);
	}
	else
	{
		*bucketno = hashvalue % nbuckets;
		*batchno = 0;
	}
}

/*
 * ExecScanHashBucket
 *		scan a hash bucket for matches to the current outer tuple
 *
 * The current outer tuple must be stored in econtext->ecxt_outertuple.
 */
HashJoinTuple
ExecScanHashBucket(HashJoinState *hjstate,
				   ExprContext *econtext)
{
	List	   *hjclauses = hjstate->hashclauses;
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;
	uint32		hashvalue = hjstate->hj_CurHashValue;

	/*
	 * hj_CurTuple is NULL to start scanning a new bucket, or the address of
	 * the last tuple returned from the current bucket.
	 */
	if (hashTuple == NULL)
		hashTuple = hashtable->buckets[hjstate->hj_CurBucketNo];
	else
		hashTuple = hashTuple->next;

	while (hashTuple != NULL)
	{
		if (hashTuple->hashvalue == hashvalue)
		{
			TupleTableSlot *inntuple;

			/* insert hashtable's tuple into exec slot so ExecQual sees it */
			inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
											 hjstate->hj_HashTupleSlot,
											 false);	/* do not pfree */
			econtext->ecxt_innertuple = inntuple;

			/* reset temp memory each time to avoid leaks from qual expr */
			ResetExprContext(econtext);

			if (ExecQual(hjclauses, econtext, false))
			{
				hjstate->hj_CurTuple = hashTuple;
				return hashTuple;
			}
		}

		hashTuple = hashTuple->next;
	}

	/*
	 * no match
	 */
	return NULL;
}

/*
 * ExecHashTableReset
 *
 *		reset hash table header for new batch
 */
void
ExecHashTableReset(HashJoinTable hashtable)
{
	MemoryContext oldcxt;
	int			nbuckets = hashtable->nbuckets;

	/*
	 * Release all the hash buckets and tuples acquired in the prior pass, and
	 * reinitialize the context for a new pass.
	 */
	MemoryContextReset(hashtable->batchCxt);
	oldcxt = MemoryContextSwitchTo(hashtable->batchCxt);

	/* Reallocate and reinitialize the hash bucket headers. */
	hashtable->buckets = (HashJoinTuple *)
		palloc0(nbuckets * sizeof(HashJoinTuple));

    hashtable->spaceAllowed -= hashtable->spaceFreed;
    hashtable->spaceFreed = 0;
	hashtable->spaceUsed = 0;

	MemoryContextSwitchTo(oldcxt);
}

void
ExecReScanHash(HashState *node, ExprContext *exprCtxt)
{
	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}


/*
 * ExecHashTableExplainInit
 *      Called after ExecHashTableCreate to set up EXPLAIN ANALYZE reporting.
 */
void
ExecHashTableExplainInit(HashJoinState *hjstate,
                         HashState     *hashstate,
                         HashJoinTable  hashtable)
{
    MemoryContext   oldcxt;
    int             nbatch = hashtable->nbatch;

    /* Switch to a memory context that survives until ExecutorEnd. */
    oldcxt = MemoryContextSwitchTo(hjstate->js.ps.state->es_query_cxt);

    /* On first call, set EXPLAIN ANALYZE hooks in the PlanState nodes. */
    if (!hjstate->js.ps.cdbexplainbuf)
    {
        Assert(!hashstate->ps.cdbexplainbuf && !hjstate->js.ps.cdbexplainfun);

	    /* Allocate string buffers. */
        hashstate->ps.cdbexplainbuf = makeStringInfo();
        hjstate->js.ps.cdbexplainbuf = makeStringInfo();

        /* Request a callback at end of query. */
        hjstate->js.ps.cdbexplainfun = ExecHashTableExplainEnd;
    }

    /* Create workarea and attach it to the HashJoinTable. */
    hashtable->stats = (HashJoinTableStats *)palloc0(sizeof(*hashtable->stats));
    hashtable->stats->joinexplainbuf = hjstate->js.ps.cdbexplainbuf;
    hashtable->stats->hashexplainbuf = hashstate->ps.cdbexplainbuf;
    hashtable->stats->endedbatch = -1;

    /* Create per-batch statistics array. */
    hashtable->stats->batchstats =
        (HashJoinBatchStats *)palloc0(nbatch * sizeof(hashtable->stats->batchstats[0]));
    hashtable->stats->nbatchstats = nbatch;

    /* Restore caller's memory context. */
    MemoryContextSwitchTo(oldcxt);
}                               /* ExecHashTableExplainInit */


/*
 * ExecHashTableExplainEnd
 *      Called before ExecutorEnd to finish EXPLAIN ANALYZE reporting.
 */
void
ExecHashTableExplainEnd(PlanState *planstate, struct StringInfoData *buf)
{
    HashJoinState      *hjstate = (HashJoinState *)planstate;
    HashJoinTable       hashtable = hjstate->hj_HashTable;
    HashJoinTableStats *stats;
    StringInfo          hbuf;
    StringInfo          jbuf;
    int                 total_buckets;
    int                 i;

    if (!hashtable ||
        !hashtable->stats ||
        hashtable->nbatch < 1)
        return;

    stats = hashtable->stats;
    hbuf = stats->hashexplainbuf;
    jbuf = stats->joinexplainbuf;

    /* Report on batch in progress, in case the join is being ended early. */
    ExecHashTableExplainBatchEnd(hashtable);

    /* How much work_mem would suffice to eliminate spill I/O? */
    if (hashtable->nbatch > 1)
    {
        /* If quitting before end of join, make an estimate. */
        if (hashtable->curbatch < hashtable->nbatch &&
            stats->nbatchstats >= hashtable->nbatch)
        {
            int64       innerbytes = stats->innerbytes_sum;

            /* Add size of spilled batches not processed yet. */
            for (i = hashtable->curbatch + 1; i < hashtable->nbatch; i++)
                innerbytes += stats->batchstats[i].innerfilesize;

            /* Don't guess less than the actual high-water mark. */
            if (innerbytes < stats->workmem_max)
                innerbytes = stats->workmem_max;

            /* Probably an underestimate because tuples are smaller on disk. */
            appendStringInfo(jbuf, "Suggest increasing work_mem above "
                             INT64_FORMAT "K bytes to reduce workfile I/O.",
                             (innerbytes + 1023) / 1024);
        }

        /* Exact work_mem requirement is known if join was run to completion. */
        else
            appendStringInfo(jbuf, "Would need work_mem = "
                             INT64_FORMAT "K bytes to avoid workfile I/O.",
                             (stats->innerbytes_sum + 1023) / 1024);
    }

    /* Report actual work_mem high water mark. */
    appendStringInfo(hbuf, "Used %dK bytes of work_mem.  ",
                     (int)((stats->workmem_max + 1023) / 1024));

    /* Report hash bucket statistics. */
    total_buckets = stats->nonemptybatches * hashtable->nbuckets;
    if (total_buckets > 0)
        appendStringInfo(hbuf,
                         "Hash chain length %.2f avg, %d max.  "
                         "%d nonempty buckets (of %d).",
                         stats->chainlength_sum / stats->nonemptybuckets,
                         stats->chainlength_max,
                         stats->nonemptybuckets,
                         total_buckets);
}                               /* ExecHashTableExplainEnd */


/*
 * ExecHashTableExplainBatchEnd
 *      Called at end of each batch to collect statistics for EXPLAIN ANALYZE.
 */
void
ExecHashTableExplainBatchEnd(HashJoinTable hashtable)
{
    HashJoinTableStats *stats = hashtable->stats;
    HashJoinBatchStats *batchstats = stats->batchstats;
    StringInfo          jbuf = stats->joinexplainbuf;
    Size                innerbytes = hashtable->spaceUsed - hashtable->spaceFreed;
    int                 curbatch = hashtable->curbatch;
    int                 chainlength_max = 0;
    int                 chainlength_sum = 0;
    int                 nonemptybuckets = 0;
    int                 i;

    /* Already reported on this batch? */
    if (stats->endedbatch == curbatch ||
        curbatch >= hashtable->nbatch)
        return;
    stats->endedbatch = curbatch;

    /* Keep quiet if empty inner rel or empty inner batch. */
    if (hashtable->nbatch == 0 ||
        hashtable->totalTuples == 0)
        return;

    stats->nonemptybatches++;

    /* Update high-water mark for work_mem actually used at one time. */
    if (stats->workmem_max < innerbytes)
        stats->workmem_max = innerbytes;

    /* Accumulate work_mem used to hold all hash table batches joined so far. */
    stats->innerbytes_sum += innerbytes;

    /* Report workfile I/O statistics. */
    if (hashtable->nbatch > 1)
    {
        uint64          irdbytes = 0;
        uint64          iwrbytes = 0;
        uint64          ordbytes = 0;
        uint64          owrbytes = 0;
        bool            rdsome;
        bool            wrsome;

        Assert(batchstats &&
               hashtable->nbatch <= stats->nbatchstats);

        appendStringInfo(jbuf, "batch %d ", curbatch);

        /* How much was read from workfiles for current batch? */
        if (curbatch > 0 &&
            hashtable->outerBatchFile &&
            hashtable->outerBatchFile[curbatch])
            ordbytes = BufFileTell64(hashtable->outerBatchFile[curbatch]);
        irdbytes = batchstats[curbatch].innerfilesize;
        rdsome = (ordbytes > 0 || irdbytes > 0);

        /* How much was written to workfiles for the remaining batches? */
        for (i = curbatch + 1; i < hashtable->nbatch; i++)
        {
            HashJoinBatchStats *bs = &batchstats[i];
            uint64              filebytes = 0;

            if (hashtable->outerBatchFile &&
                hashtable->outerBatchFile[i])
                filebytes = BufFileTell64(hashtable->outerBatchFile[i]);
            Assert(filebytes >= bs->outerfilesize);
            owrbytes += filebytes - bs->outerfilesize;
            bs->outerfilesize = filebytes;

            filebytes = 0;
            if (hashtable->innerBatchFile &&
                hashtable->innerBatchFile[i])
                filebytes = BufFileTell64(hashtable->innerBatchFile[i]);
            Assert(filebytes >= bs->innerfilesize);
            iwrbytes += filebytes - bs->innerfilesize;
            bs->innerfilesize = filebytes;
        }
        wrsome = (owrbytes > 0 || iwrbytes > 0);

        if (rdsome)
            appendStringInfo(jbuf,
                             "read "
                             INT64_FORMAT "K outer, "
                             INT64_FORMAT "K inner bytes from workfile",
                             (ordbytes + 1023)/1024,
                             (irdbytes + 1023)/1024);
        if (wrsome)
            appendStringInfo(jbuf,
                             "%swrote "
                             INT64_FORMAT "K outer, "
                             INT64_FORMAT "K inner bytes to workfile",
                             rdsome ? "; " : "",
                             (owrbytes + 1023)/1024,
                             (iwrbytes + 1023)/1024);
        appendStringInfoChar(jbuf, '\n');
    }                           /* give workfile I/O statistics */

    /* Collect hash chain statistics. */
    for (i = 0; i < hashtable->nbuckets; i++)
    {
        HashJoinTuple   hashtuple = hashtable->buckets[i];
        int             chainlength;

        if (hashtuple)
        {
            nonemptybuckets++;
            for (chainlength = 0; hashtuple; hashtuple = hashtuple->next)
                chainlength++;
            chainlength_sum += chainlength;
            if (chainlength_max < chainlength)
                chainlength_max = chainlength;
        }
    }
    stats->nonemptybuckets += nonemptybuckets;
    stats->chainlength_sum += chainlength_sum;
    if (stats->chainlength_max < chainlength_max)
        stats->chainlength_max = chainlength_max;
}                               /* ExecHashTableExplainBatchEnd */
