/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c
 *	  Routines to handle hash join nodes
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeHashjoin.c,v 1.85.2.1 2007/02/02 00:07:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/instrument.h"        /* Instrumentation */
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "utils/memutils.h"

#include "miscadmin.h" /* work_mem */

static TupleTableSlot *ExecHashJoinOuterGetTuple(PlanState *outerNode,
						  HashJoinState *hjstate,
						  uint32 *hashvalue);
static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinBatchSide *side,
						  uint32 *hashvalue,
						  TupleTableSlot *tupleSlot);
static int	ExecHashJoinNewBatch(HashJoinState *hjstate);


/* ----------------------------------------------------------------
 *		ExecHashJoin
 *
 *		This function implements the Hybrid Hashjoin algorithm.
 *
 *		Note: the relation we build hash table on is the "inner"
 *			  the other one is "outer".
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecHashJoin(HashJoinState *node)
{
	EState	   *estate;
	PlanState  *outerNode;
	HashState  *hashNode;
	List	   *joinqual;
	List	   *otherqual;
	TupleTableSlot *inntuple;
	ExprContext *econtext;
	HashJoinTable hashtable;
	HashJoinTuple curtuple;
	TupleTableSlot *outerTupleSlot;
	uint32		hashvalue;
	int			batchno;

	/*
	 * get information from HashJoin node
	 */
	estate = node->js.ps.state;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	hashNode = (HashState *) innerPlanState(node);
	outerNode = outerPlanState(node);

	/*
	 * get information from HashJoin state
	 */
	hashtable = node->hj_HashTable;
	econtext = node->js.ps.ps_ExprContext;

	/*
	 * If we're doing an IN join, we want to return at most one row per outer
	 * tuple; so we can stop scanning the inner scan if we matched on the
	 * previous try.
	 */
	if (node->js.jointype == JOIN_IN && node->hj_MatchedOuter)
		node->hj_NeedNewOuter = true;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note this can't happen
	 * until we're done projecting out tuples from a join tuple.
	 */
	ResetExprContext(econtext);

	/*
	 * if this is the first call, build the hash table for inner relation
	 */
	if (hashtable == NULL)
	{
		/* MPP-989: for MPP reading from the outer here causes a
		 * deadlock hazard. We need to fix this properly at some point
		 * -- so that we'll be able to avoid building the hash table
		 * when we have an empty outer relation */
#if 0 /* MPP-989 */
		/*
		 * If the outer relation is completely empty, we can quit without
		 * building the hash table.  However, for an inner join it is only a
		 * win to check this when the outer relation's startup cost is less
		 * than the projected cost of building the hash table.	Otherwise it's
		 * best to build the hash table first and see if the inner relation is
		 * empty.  (When it's an outer join, we should always make this check,
		 * since we aren't going to be able to skip the join on the strength
		 * of an empty inner relation anyway.)
		 *
		 * If we are rescanning the join, we make use of information gained on
		 * the previous scan: don't bother to try the prefetch if the previous
		 * scan found the outer relation nonempty.	This is not 100% reliable
		 * since with new parameters the outer relation might yield different
		 * results, but it's a good heuristic.
		 *
		 * The only way to make the check is to try to fetch a tuple from the
		 * outer plan node.  If we succeed, we have to stash it away for later
		 * consumption by ExecHashJoinOuterGetTuple.
		 */
		if (node->js.jointype == JOIN_LEFT ||
			(outerNode->plan->startup_cost < hashNode->ps.plan->total_cost &&
			 !node->hj_OuterNotEmpty))
		{
			TupleTableSlot *slot;

			if (node->outer_q != NULL)
				slot = dequeueTQ(node->outer_q);
			else
				slot = ExecProcNode(outerNode);

			node->hj_FirstOuterTupleSlot = slot;
			if (TupIsNull(node->hj_FirstOuterTupleSlot))
			{
				node->hj_OuterNotEmpty = false;

                /* CDB: Tell inner subtree that its data will not be needed. */
			    ExecSquelchNode((PlanState *)hashNode);

				return NULL;
			}
			else
				node->hj_OuterNotEmpty = true;
		}
		else
			node->hj_FirstOuterTupleSlot = NULL;
#endif
		/* see MPP-989 comment above, for now we assume that we have
		 * at least one row on the outer. */
		node->hj_FirstOuterTupleSlot = NULL;

		/*
		 * create the hash table
		 */
		hashtable = ExecHashTableCreate((Hash *) hashNode->ps.plan,
										node->hj_HashOperators);
		node->hj_HashTable = hashtable;

        /*
         * CDB: Offer extra info for EXPLAIN ANALYZE.
         */
        if (estate->es_instrument)
            ExecHashTableExplainInit(node, hashtable);

		/*
		 * execute the Hash node, to build the hash table
		 */
		hashNode->hashtable = hashtable;
		(void) MultiExecProcNode((PlanState *) hashNode);

		/*
		 * If the inner relation is completely empty, and we're not doing an
		 * outer join, we can quit without scanning the outer relation.
		 */
		if (node->js.jointype != JOIN_LEFT)
        {
            int         i;

            /* Is there a nonempty batch? */
            for (i = 0; i < hashtable->nbatch; i++)
                if (hashtable->batches[i]->innertuples)
                    break;

		    if (i == hashtable->nbatch)
		    {
			    /*
			     * CDB: We'll read no more from outer subtree. To keep sibling
			     * QEs from being starved, tell source QEs not to clog up the
			     * pipeline with our never-to-be-consumed data.
			     */
			    ExecSquelchNode(outerNode);

			    return NULL;
		    }
        }

		/*
		 * Reset OuterNotEmpty for scan.  (It's OK if we fetched a tuple
		 * above, because ExecHashJoinOuterGetTuple will immediately set it
		 * again.)
		 */
		node->hj_OuterNotEmpty = false;

#ifdef HJDEBUG
		elog(DEBUG4, "HashJoin built table with %f tuples", hashtable->totalTuples);
#endif
	}

	/*
	 * run the hash join process
	 */
	for (;;)
	{
		/*
		 * If we don't have an outer tuple, get the next one
		 */
		if (node->hj_NeedNewOuter)
		{
			outerTupleSlot = ExecHashJoinOuterGetTuple(outerNode,
													   node,
													   &hashvalue);
			if (TupIsNull(outerTupleSlot))
			{
				/* end of join */
				return NULL;
			}

			node->js.ps.ps_OuterTupleSlot = outerTupleSlot;
			econtext->ecxt_outertuple = outerTupleSlot;
			node->hj_NeedNewOuter = false;
			node->hj_MatchedOuter = false;

			/*
			 * now we have an outer tuple, find the corresponding bucket for
			 * this tuple from the hash table
			 */
			node->hj_CurHashValue = hashvalue;
			ExecHashGetBucketAndBatch(hashtable, hashvalue,
									  &node->hj_CurBucketNo, &batchno);
			node->hj_CurTuple = NULL;

			/*
			 * Now we've got an outer tuple and the corresponding hash bucket,
			 * but this tuple may not belong to the current batch.
			 */
			if (batchno != hashtable->curbatch)
			{
				/*
				 * Need to postpone this outer tuple to a later batch. Save it
				 * in the corresponding outer-batch file.
				 */
				Assert(batchno > hashtable->curbatch);
				ExecHashJoinSaveTuple(ExecFetchSlotMinimalTuple(outerTupleSlot),
									  hashvalue,
                                      &hashtable->batches[batchno]->outerside);
				node->hj_NeedNewOuter = true;
				continue;		/* loop around for a new outer tuple */
			}
		}

		/*
		 * OK, scan the selected hash bucket for matches
		 */
		for (;;)
		{
			curtuple = ExecScanHashBucket(node, econtext);
			if (curtuple == NULL)
				break;			/* out of matches */

			/*
			 * we've got a match, but still need to test non-hashed quals
			 */
			inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(curtuple),
											 node->hj_HashTupleSlot,
											 false);	/* don't pfree */
			econtext->ecxt_innertuple = inntuple;

			/* reset temp memory each time to avoid leaks from qual expr */
			ResetExprContext(econtext);

			/*
			 * if we pass the qual, then save state for next call and have
			 * ExecProject form the projection, store it in the tuple table,
			 * and return the slot.
			 *
			 * Only the joinquals determine MatchedOuter status, but all quals
			 * must pass to actually return the tuple.
			 */
			if (joinqual == NIL || ExecQual(joinqual, econtext, false))
			{
				node->hj_MatchedOuter = true;

				if (otherqual == NIL || ExecQual(otherqual, econtext, false))
					return ExecProject(node->js.ps.ps_ProjInfo, NULL);

				/*
				 * If we didn't return a tuple, may need to set NeedNewOuter
				 */
				if (node->js.jointype == JOIN_IN)
				{
					node->hj_NeedNewOuter = true;
					break;		/* out of loop over hash bucket */
				}
			}
		}

		/*
		 * Now the current outer tuple has run out of matches, so check
		 * whether to emit a dummy outer-join tuple. If not, loop around to
		 * get a new outer tuple.
		 */
		node->hj_NeedNewOuter = true;

		if (!node->hj_MatchedOuter &&
			node->js.jointype == JOIN_LEFT)
		{
			/*
			 * We are doing an outer join and there were no join matches for
			 * this outer tuple.  Generate a fake join tuple with nulls for
			 * the inner tuple, and return it if it passes the non-join quals.
			 */
			econtext->ecxt_innertuple = node->hj_NullInnerTupleSlot;

			if (ExecQual(otherqual, econtext, false))
				return ExecProject(node->js.ps.ps_ProjInfo, NULL);
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecInitHashJoin
 *
 *		Init routine for HashJoin node.
 * ----------------------------------------------------------------
 */
HashJoinState *
ExecInitHashJoin(HashJoin *node, EState *estate, int eflags)
{
	HashJoinState *hjstate;
	Plan	   *outerNode;
	Hash	   *hashNode;
	List	   *lclauses;
	List	   *rclauses;
	List	   *hoperators;
	ListCell   *l;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	hjstate = makeNode(HashJoinState);
	hjstate->js.ps.plan = (Plan *) node;
	hjstate->js.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hjstate->js.ps);

	hjstate->outer_q = NULL;

	/*
	 * initialize child expressions
	 */
	hjstate->js.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->join.plan.targetlist,
					 (PlanState *) hjstate);
	hjstate->js.ps.qual = (List *)
		ExecInitExpr((Expr *) node->join.plan.qual,
					 (PlanState *) hjstate);
	hjstate->js.jointype = node->join.jointype;
	hjstate->js.joinqual = (List *)
		ExecInitExpr((Expr *) node->join.joinqual,
					 (PlanState *) hjstate);
	hjstate->hashclauses = (List *)
		ExecInitExpr((Expr *) node->hashclauses,
					 (PlanState *) hjstate);

	/*
	 * initialize child nodes
	 *
	 * Note: we could suppress the REWIND flag for the inner input, which
	 * would amount to betting that the hash will be a single batch.  Not
	 * clear if this would be a win or not.
	 */
	outerNode = outerPlan(node);
	hashNode = (Hash *) innerPlan(node);

	outerPlanState(hjstate) = ExecInitNode(outerNode, estate, eflags);
	innerPlanState(hjstate) = ExecInitNode((Plan *) hashNode, estate, eflags);

#define HASHJOIN_NSLOTS 3

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &hjstate->js.ps);
	hjstate->hj_OuterTupleSlot = ExecInitExtraTupleSlot(estate);

	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_IN:
			break;
		case JOIN_LEFT:
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
								 ExecGetResultType(innerPlanState(hjstate)));
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * now for some voodoo.  our temporary tuple slot is actually the result
	 * tuple slot of the Hash node (which is our inner plan).  we do this
	 * because Hash nodes don't return tuples via ExecProcNode() -- instead
	 * the hash join node uses ExecScanHashBucket() to get at the contents of
	 * the hash table.	-cim 6/9/91
	 */
	{
		HashState  *hashstate = (HashState *) innerPlanState(hjstate);
		TupleTableSlot *slot = hashstate->ps.ps_ResultTupleSlot;

		hjstate->hj_HashTupleSlot = slot;
	}

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&hjstate->js.ps);
	ExecAssignProjectionInfo(&hjstate->js.ps, NULL);

	ExecSetSlotDescriptor(hjstate->hj_OuterTupleSlot,
						  ExecGetResultType(outerPlanState(hjstate)));

	/*
	 * initialize hash-specific info
	 */
	hjstate->hj_HashTable = NULL;
	hjstate->hj_FirstOuterTupleSlot = NULL;

	hjstate->hj_CurHashValue = 0;
	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurTuple = NULL;

	/*
	 * Deconstruct the hash clauses into outer and inner argument values, so
	 * that we can evaluate those subexpressions separately.  Also make a list
	 * of the hash operator OIDs, in preparation for looking up the hash
	 * functions to use.
	 */
	lclauses = NIL;
	rclauses = NIL;
	hoperators = NIL;
	foreach(l, hjstate->hashclauses)
	{
		FuncExprState *fstate = (FuncExprState *) lfirst(l);
		OpExpr	   *hclause;

		Assert(IsA(fstate, FuncExprState));
		hclause = (OpExpr *) fstate->xprstate.expr;
		Assert(IsA(hclause, OpExpr));
		lclauses = lappend(lclauses, linitial(fstate->args));
		rclauses = lappend(rclauses, lsecond(fstate->args));
		hoperators = lappend_oid(hoperators, hclause->opno);
	}
	hjstate->hj_OuterHashKeys = lclauses;
	hjstate->hj_InnerHashKeys = rclauses;
	hjstate->hj_HashOperators = hoperators;
	/* child Hash node needs to evaluate inner hash keys, too */
	((HashState *) innerPlanState(hjstate))->hashkeys = rclauses;

	hjstate->js.ps.ps_OuterTupleSlot = NULL;
	hjstate->hj_NeedNewOuter = true;
	hjstate->hj_MatchedOuter = false;
	hjstate->hj_OuterNotEmpty = false;

	return hjstate;
}

int
ExecCountSlotsHashJoin(HashJoin *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		HASHJOIN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndHashJoin
 *
 *		clean up routine for HashJoin node
 * ----------------------------------------------------------------
 */
void
ExecEndHashJoin(HashJoinState *node)
{
	/*
	 * Free hash table
	 */
	if (node->hj_HashTable)
	{
		ExecHashTableDestroy(node->hj_HashTable);
		node->hj_HashTable = NULL;
	}

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->hj_OuterTupleSlot);
	ExecClearTuple(node->hj_HashTupleSlot);

	/*
	 * clean up subtrees
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/*
 * ExecHashJoinOuterGetTuple
 *
 *		get the next outer tuple for hashjoin: either by
 *		executing a plan node in the first pass, or from
 *		the temp files for the hashjoin batches.
 *
 * Returns a null slot if no more outer tuples.  On success, the tuple's
 * hash value is stored at *hashvalue --- this is either originally computed,
 * or re-read from the temp file.
 */
static TupleTableSlot *
ExecHashJoinOuterGetTuple(PlanState *outerNode,
						  HashJoinState *hjstate,
						  uint32 *hashvalue)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;

	if (curbatch == 0)
	{							/* if it is the first pass */

		/*
		 * Check to see if first outer tuple was already fetched by
		 * ExecHashJoin() and not used yet.
		 */
		slot = hjstate->hj_FirstOuterTupleSlot;
		if (!TupIsNull(slot))
			hjstate->hj_FirstOuterTupleSlot = NULL;
		else
		{
			if (hjstate->outer_q != NULL)
			{
				slot = dequeueTQ(hjstate->outer_q);
				if (slot == NULL)
				{
					/* exhausted queued tuples */
					pfree(hjstate->outer_q);
					hjstate->outer_q = NULL;

					slot = ExecProcNode(outerNode);
				}
			}
			else
				slot = ExecProcNode(outerNode);
		}
		if (!TupIsNull(slot))
		{
			/*
			 * We have to compute the tuple's hash value.
			 */
			ExprContext *econtext = hjstate->js.ps.ps_ExprContext;

			econtext->ecxt_outertuple = slot;
			*hashvalue = ExecHashGetHashValue(hashtable, econtext,
											  hjstate->hj_OuterHashKeys);

			/* remember outer relation is not empty for possible rescan */
			hjstate->hj_OuterNotEmpty = true;

			return slot;
		}

		/*
		 * We have just reached the end of the first pass. Try to switch to a
		 * saved batch.
		 */
		curbatch = ExecHashJoinNewBatch(hjstate);
	}

	/*
	 * Try to read from a temp file. Loop allows us to advance to new batches
	 * as needed.  NOTE: nbatch could increase inside ExecHashJoinNewBatch, so
	 * don't try to optimize this loop.
	 */
	while (curbatch < hashtable->nbatch)
	{
		slot = ExecHashJoinGetSavedTuple(&hashtable->batches[curbatch]->outerside,
										 hashvalue,
										 hjstate->hj_OuterTupleSlot);
		if (!TupIsNull(slot))
			return slot;
		curbatch = ExecHashJoinNewBatch(hjstate);
	}

	/* Out of batches... */
	return NULL;
}

/*
 * ExecHashJoinNewBatch
 *		switch to a new hashjoin batch
 *
 * Returns the number of the new batch (1..nbatch-1), or nbatch if no more.
 * We will never return a batch number that has an empty outer batch file.
 */
static int
ExecHashJoinNewBatch(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
    HashJoinBatchData  *batch;
	int			nbatch;
	int			curbatch;
	BufFile    *innerFile;
	TupleTableSlot *slot;
	uint32		hashvalue;

start_over:
	nbatch = hashtable->nbatch;
	curbatch = hashtable->curbatch;

    if (curbatch >= nbatch)
        return nbatch;

    if (hashtable->stats)
        ExecHashTableExplainBatchEnd(hashtable);

	if (curbatch > 0)
	{
		/*
		 * We no longer need the previous outer batch file; close it right
		 * away to free disk space.
		 */
        batch = hashtable->batches[curbatch];
		if (batch->outerside.bufFile)
			BufFileClose(batch->outerside.bufFile);
		batch->outerside.bufFile = NULL;
	}

	/*
	 * We can always skip over any batches that are completely empty on both
	 * sides.  We can sometimes skip over batches that are empty on only one
	 * side, but there are exceptions:
	 *
	 * 1. In a LEFT JOIN, we have to process outer batches even if the inner
	 * batch is empty.
	 *
	 * 2. If we have increased nbatch since the initial estimate, we have to
	 * scan inner batches since they might contain tuples that need to be
	 * reassigned to later inner batches.
	 *
	 * 3. Similarly, if we have increased nbatch since starting the outer
	 * scan, we have to rescan outer batches in case they contain tuples that
	 * need to be reassigned.
	 */
	curbatch++;
	while (curbatch < nbatch &&
		   (hashtable->batches[curbatch]->outerside.bufFile == NULL ||
			hashtable->batches[curbatch]->innerside.bufFile == NULL))
	{
        batch = hashtable->batches[curbatch];
		if (batch->outerside.bufFile &&
			hjstate->js.jointype == JOIN_LEFT)
			break;				/* must process due to rule 1 */
		if (batch->innerside.bufFile &&
			nbatch != hashtable->nbatch_original)
			break;				/* must process due to rule 2 */
		if (batch->outerside.bufFile &&
			nbatch != hashtable->nbatch_outstart)
			break;				/* must process due to rule 3 */
		/* We can ignore this batch. */
		/* Release associated temp files right away. */
		if (batch->innerside.bufFile)
			BufFileClose(batch->innerside.bufFile);
		batch->innerside.bufFile = NULL;
		if (batch->outerside.bufFile)
			BufFileClose(batch->outerside.bufFile);
		batch->outerside.bufFile = NULL;
		curbatch++;
	}

    hashtable->curbatch = curbatch;     /* CDB: upd before return, even if no
                                         * more data, so stats logic can see
                                         * whether join was run to completion
                                         */

	if (curbatch >= nbatch)
		return curbatch;		/* no more batches */

    batch = hashtable->batches[curbatch];

    /*
	 * Reload the hash table with the new inner batch (which could be empty)
	 */
	ExecHashTableReset(hashtable);

 	innerFile = batch->innerside.bufFile;

	if (innerFile != NULL)
	{
		if (BufFileSeek(innerFile, 0, 0L, SEEK_SET))
			ereport(ERROR,
					(errcode_for_file_access(),
				   errmsg("could not rewind hash-join temporary file: %m")));

		for (;;)
		{
	        slot = ExecHashJoinGetSavedTuple(&batch->innerside,
									         &hashvalue,
									         hjstate->hj_HashTupleSlot);
            if (!slot)
                break;

			/*
			 * NOTE: some tuples may be sent to future batches.  Also, it is
			 * possible for hashtable->nbatch to be increased here!
			 */
			ExecHashTableInsert(hashtable, slot, hashvalue);
		}

		/*
		 * after we build the hash table, the inner batch file is no longer
		 * needed
		 */
		BufFileClose(innerFile);
		batch->innerside.bufFile = NULL;
	}

	/*
	 * If there's no outer batch file, advance to next batch.
	 */
	if (batch->outerside.bufFile == NULL)
		goto start_over;

	/*
	 * Rewind outer batch file, so that we can start reading it.
	 */
	if (BufFileSeek(batch->outerside.bufFile, 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind hash-join temporary file: %m")));

	return curbatch;
}

/*
 * ExecHashJoinSaveTuple
 *		save a tuple to a batch file.
 *
 * The data recorded in the file for each tuple is its hash value,
 * then the tuple in MinimalTuple format.
 *
 * Note: it is important always to call this in the regular executor
 * context, not in a shorter-lived context; else the temp file buffers
 * will get messed up.
 */
void
ExecHashJoinSaveTuple(MinimalTuple tuple, uint32 hashvalue,
					  struct HashJoinBatchSide *batchside)
{
	BufFile    *file = batchside->bufFile;
	size_t		written;

	if (file == NULL)
	{
		/* First write to this batch file, so open it. */
		file = BufFileCreateTemp(false);
		batchside->bufFile = file;
	}

	written = BufFileWrite(file, (void *) &hashvalue, sizeof(uint32));
	if (written != sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to hash-join temporary file: %m")));

	written = BufFileWrite(file, (void *) tuple, tuple->t_len);
	if (written != tuple->t_len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to hash-join temporary file: %m")));
}

/*
 * ExecHashJoinGetSavedTuple
 *		read the next tuple from a batch file.	Return NULL if no more.
 *
 * On success, *hashvalue is set to the tuple's hash value, and the tuple
 * itself is stored in the given slot.
 */
static TupleTableSlot *
ExecHashJoinGetSavedTuple(HashJoinBatchSide *batchside,
						  uint32 *hashvalue,
						  TupleTableSlot *tupleSlot)
{
    BufFile    *file = batchside->bufFile;
	uint32		header[2];
	size_t		nread;
	MinimalTuple tuple;

	/*
	 * Since both the hash value and the MinimalTuple length word are uint32,
	 * we can read them both in one BufFileRead() call without any type
	 * cheating.
	 */
	nread = BufFileRead(file, (void *) header, sizeof(header));
	if (nread == 0)				/* end of file */
	{
		ExecClearTuple(tupleSlot);
		return NULL;
	}
	if (nread != sizeof(header))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: %m")));
	*hashvalue = header[0];
	tuple = (MinimalTuple) palloc(header[1]);
	tuple->t_len = header[1];
	nread = BufFileRead(file,
						(void *) ((char *) tuple + sizeof(uint32)),
						header[1] - sizeof(uint32));
	if (nread != header[1] - sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: %m")));
	return ExecStoreMinimalTuple(tuple, tupleSlot, true);
}


void
ExecReScanHashJoin(HashJoinState *node, ExprContext *exprCtxt)
{
	/*
	 * In a multi-batch join, we currently have to do rescans the hard way,
	 * primarily because batch temp files may have already been released. But
	 * if it's a single-batch join, and there is no parameter change for the
	 * inner subnode, then we can just re-use the existing hash table without
	 * rebuilding it.
	 */
	if (node->hj_HashTable != NULL)
	{
		if (node->hj_HashTable->nbatch == 1 &&
			((PlanState *) node)->righttree->chgParam == NULL)
		{
			/*
			 * okay to reuse the hash table; needn't rescan inner, either.
			 *
			 * What we do need to do is reset our state about the emptiness of
			 * the outer relation, so that the new scan of the outer will
			 * update it correctly if it turns out to be empty this time.
			 * (There's no harm in clearing it now because ExecHashJoin won't
			 * need the info.  In the other cases, where the hash table
			 * doesn't exist or we are destroying it, we leave this state
			 * alone because ExecHashJoin will need it the first time
			 * through.)
			 */
			node->hj_OuterNotEmpty = false;

			/* MPP-1600: reset the batch number */
			node->hj_HashTable->curbatch = 0;
		}
		else
		{
			/* must destroy and rebuild hash table */
			ExecHashTableDestroy(node->hj_HashTable);
			node->hj_HashTable = NULL;

			/*
			 * if chgParam of subnode is not null then plan will be re-scanned
			 * by first ExecProcNode.
			 */
			if (((PlanState *) node)->righttree->chgParam == NULL)
				ExecReScan(((PlanState *) node)->righttree, exprCtxt);
		}
	}

	/* Always reset intra-tuple state */
	node->hj_CurHashValue = 0;
	node->hj_CurBucketNo = 0;
	node->hj_CurTuple = NULL;

	node->js.ps.ps_OuterTupleSlot = NULL;
	node->hj_NeedNewOuter = true;
	node->hj_MatchedOuter = false;
	node->hj_FirstOuterTupleSlot = NULL;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}

executorTupleQueue *
makeTQ(int size, struct Instrumentation *instr)
{
	int tq_size;
	executorTupleQueue *tq = NULL;

	tq_size = sizeof(executorTupleQueue) + ((size-1) * sizeof(HeapTuple));

	if (tq_size > (work_mem*1024))
		return NULL;

    if (instr)
        instr->workmemused = Max(instr->workmemused, tq_size);

	tq = palloc0(tq_size);

	tq->maxsize = size;
	tq->size = 0;
	tq->pos = 0;
	tq->slot = NULL;

	return tq;
}

void
enqueueTQ(executorTupleQueue *tq, TupleTableSlot *slot)
{
	HeapTuple newt=NULL;

	Assert(tq != NULL);
	Assert(tq->size < tq->maxsize);

	if (slot == NULL)
		return;

	if (tq->slot == NULL)
		tq->slot = slot;

	newt = ExecFetchSlotTuple(slot);
	tq->tuples[tq->size] = heap_copytuple(newt);
	tq->size++;
}

TupleTableSlot *dequeueTQ(executorTupleQueue *tq)
{
	HeapTuple curtup;
	TupleTableSlot *tts;

	Assert(tq != NULL);

	if (tq->pos >= tq->size)
		return NULL;

	curtup = tq->tuples[tq->pos];
	if (curtup == NULL)
		return NULL;

	tq->pos++;

	tts = ExecStoreTuple(curtup, tq->slot, InvalidBuffer, true /* ignored, buffer is invalid */);

	return tts;
}

void releaseTQ(executorTupleQueue *tq)
{
	TupleTableSlot *tts;

	while ((tts = dequeueTQ(tq)) != NULL)
	{
		ExecClearTuple(tts);
	}
}
