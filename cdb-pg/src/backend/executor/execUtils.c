/*-------------------------------------------------------------------------
 *
 * execUtils.c
 *	  miscellaneous executor utility routines
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/execUtils.c,v 1.126.2.3 2005/11/23 20:28:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		CreateExecutorState		Create/delete executor working state
 *		FreeExecutorState
 *		CreateExprContext
 *		FreeExprContext
 *		ReScanExprContext
 *
 *		ExecAssignExprContext	Common code for plan node init routines.
 *		ExecAssignResultType
 *		etc
 *
 *		ExecOpenIndices			\
 *		ExecCloseIndices		 | referenced by InitPlan, EndPlan,
 *		ExecInsertIndexTuples	/  ExecInsert, ExecUpdate
 *
 *		RegisterExprContextCallback    Register function shutdown callback
 *		UnregisterExprContextCallback  Deregister function shutdown callback
 *
 *	 NOTES
 *		This file has traditionally been the place to stick misc.
 *		executor support stuff that doesn't really go anyplace else.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "catalog/catalog.h"
#include "catalog/pg_index.h"
#include "executor/execdebug.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"
#include "nodes/primnodes.h"
#include "nodes/execnodes.h"

#include "cdb/cdbutil.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"
#include "cdb/ml_ipc.h"
#include "cdb/cdbmotion.h"

#include "utils/elog.h"

/* ----------------------------------------------------------------
 *		global counters for number of tuples processed, retrieved,
 *		appended, replaced, deleted.
 * ----------------------------------------------------------------
 */
int			NTupleProcessed;
int			NTupleRetrieved;
int			NTupleReplaced;
int			NTupleAppended;
int			NTupleDeleted;
int			NIndexTupleInserted;
extern int	NIndexTupleProcessed;		/* have to be defined in the access
										 * method level so that the
										 * cinterface.a will link ok. */


static void ShutdownExprContext(ExprContext *econtext);


/* ----------------------------------------------------------------
 *						statistic functions
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ResetTupleCount
 * ----------------------------------------------------------------
 */
#ifdef NOT_USED
void
ResetTupleCount(void)
{
	NTupleProcessed = 0;
	NTupleRetrieved = 0;
	NTupleAppended = 0;
	NTupleDeleted = 0;
	NTupleReplaced = 0;
	NIndexTupleProcessed = 0;
}
#endif

/* ----------------------------------------------------------------
 *		PrintTupleCount
 * ----------------------------------------------------------------
 */
#ifdef NOT_USED
void
DisplayTupleCount(FILE *statfp)
{
	if (NTupleProcessed > 0)
		fprintf(statfp, "!\t%d tuple%s processed, ", NTupleProcessed,
				(NTupleProcessed == 1) ? "" : "s");
	else
	{
		fprintf(statfp, "!\tno tuples processed.\n");
		return;
	}
	if (NIndexTupleProcessed > 0)
		fprintf(statfp, "%d indextuple%s processed, ", NIndexTupleProcessed,
				(NIndexTupleProcessed == 1) ? "" : "s");
	if (NIndexTupleInserted > 0)
		fprintf(statfp, "%d indextuple%s inserted, ", NIndexTupleInserted,
				(NIndexTupleInserted == 1) ? "" : "s");
	if (NTupleRetrieved > 0)
		fprintf(statfp, "%d tuple%s retrieved. ", NTupleRetrieved,
				(NTupleRetrieved == 1) ? "" : "s");
	if (NTupleAppended > 0)
		fprintf(statfp, "%d tuple%s appended. ", NTupleAppended,
				(NTupleAppended == 1) ? "" : "s");
	if (NTupleDeleted > 0)
		fprintf(statfp, "%d tuple%s deleted. ", NTupleDeleted,
				(NTupleDeleted == 1) ? "" : "s");
	if (NTupleReplaced > 0)
		fprintf(statfp, "%d tuple%s replaced. ", NTupleReplaced,
				(NTupleReplaced == 1) ? "" : "s");
	fprintf(statfp, "\n");
}
#endif


/* ----------------------------------------------------------------
 *				 Executor state and memory management functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		CreateExecutorState
 *
 *		Create and initialize an EState node, which is the root of
 *		working storage for an entire Executor invocation.
 *
 * Principally, this creates the per-query memory context that will be
 * used to hold all working data that lives till the end of the query.
 * Note that the per-query context will become a child of the caller's
 * CurrentMemoryContext.
 * ----------------
 */
EState *
CreateExecutorState(void)
{
	EState	   *estate;
	MemoryContext qcontext;
	MemoryContext oldcontext;

	/*
	 * Create the per-query context for this Executor run.
	 */
	qcontext = AllocSetContextCreate(CurrentMemoryContext,
									 "ExecutorState",
									 ALLOCSET_DEFAULT_MINSIZE,
									 ALLOCSET_DEFAULT_INITSIZE,
									 ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Make the EState node within the per-query context.  This way, we don't
	 * need a separate pfree() operation for it at shutdown.
	 */
	oldcontext = MemoryContextSwitchTo(qcontext);

	estate = makeNode(EState);

	/*
	 * Initialize all fields of the Executor State structure
	 */
	estate->es_direction = ForwardScanDirection;
	estate->es_snapshot = SnapshotNow;
	estate->es_crosscheck_snapshot = InvalidSnapshot;	/* no crosscheck */
	estate->es_range_table = NIL;

	estate->es_result_relations = NULL;
	estate->es_num_result_relations = 0;
	estate->es_result_relation_info = NULL;

	estate->es_junkFilter = NULL;

	estate->es_trig_tuple_slot = NULL;

	estate->es_into_relation_descriptor = NULL;
	estate->es_into_relation_use_wal = false;

	estate->es_param_list_info = NULL;
	estate->es_param_exec_vals = NULL;

	estate->es_query_cxt = qcontext;

	estate->es_tupleTable = NULL;

	estate->es_processed = 0;
	estate->es_lastoid = InvalidOid;
	estate->es_rowMarks = NIL;
	estate->es_forUpdate = false;
	estate->es_rowNoWait = false;

	estate->es_instrument = false;
	estate->es_select_into = false;
	estate->es_into_oids = false;

	estate->es_exprcontexts = NIL;

	estate->es_per_tuple_exprcontext = NULL;

	estate->es_topPlan = NULL;
	estate->es_evalPlanQual = NULL;
	estate->es_evTupleNull = NULL;
	estate->es_evTuple = NULL;
	estate->es_useEvalPlan = false;

	estate->es_cur_slice_idx = 0;
	estate->es_cur_root_idx = 0;
	estate->es_sliceTable = NULL;
	estate->es_slice_map = NULL;
	estate->es_interconnect_is_setup = false;
	//estate->es_got_eos = false;
	estate->primaryResults = NULL;
	estate->mirrorResults = NULL;
	

	/*
	 * Return the executor state structure
	 */
	MemoryContextSwitchTo(oldcontext);

	return estate;
}

/* ----------------
 *		FreeExecutorState
 *
 *		Release an EState along with all remaining working storage.
 *
 * Note: this is not responsible for releasing non-memory resources,
 * such as open relations or buffer pins.  But it will shut down any
 * still-active ExprContexts within the EState.  That is sufficient
 * cleanup for situations where the EState has only been used for expression
 * evaluation, and not to run a complete Plan.
 *
 * This can be called in any memory context ... so long as it's not one
 * of the ones to be freed.
 * ----------------
 */
void
FreeExecutorState(EState *estate)
{
	/*
	 * Shut down and free any remaining ExprContexts.  We do this explicitly
	 * to ensure that any remaining shutdown callbacks get called (since they
	 * might need to release resources that aren't simply memory within the
	 * per-query memory context).
	 */
	while (estate->es_exprcontexts)
	{
		/*
		 * XXX: seems there ought to be a faster way to implement this than
		 * repeated list_delete(), no?
		 */
		FreeExprContext((ExprContext *) linitial(estate->es_exprcontexts));
		/* FreeExprContext removed the list link for us */
	}

	/*
	 * Free the per-query memory context, thereby releasing all working
	 * memory, including the EState node itself.
	 */
	MemoryContextDelete(estate->es_query_cxt);
}

/* ----------------
 *		CreateExprContext
 *
 *		Create a context for expression evaluation within an EState.
 *
 * An executor run may require multiple ExprContexts (we usually make one
 * for each Plan node, and a separate one for per-output-tuple processing
 * such as constraint checking).  Each ExprContext has its own "per-tuple"
 * memory context.
 *
 * Note we make no assumption about the caller's memory context.
 * ----------------
 */
ExprContext *
CreateExprContext(EState *estate)
{
	ExprContext *econtext;
	MemoryContext oldcontext;

	/* Create the ExprContext node within the per-query memory context */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	econtext = makeNode(ExprContext);

	/* Initialize fields of ExprContext */
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;

	econtext->ecxt_per_query_memory = estate->es_query_cxt;

	/*
	 * Create working memory for expression evaluation in this context.
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(estate->es_query_cxt,
							  "ExprContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
	econtext->ecxt_param_list_info = estate->es_param_list_info;

	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	econtext->caseValue_datum = (Datum) 0;
	econtext->caseValue_isNull = true;

	econtext->domainValue_datum = (Datum) 0;
	econtext->domainValue_isNull = true;

	econtext->ecxt_estate = estate;

	econtext->ecxt_callbacks = NULL;

	/*
	 * Link the ExprContext into the EState to ensure it is shut down when the
	 * EState is freed.  Because we use lcons(), shutdowns will occur in
	 * reverse order of creation, which may not be essential but can't hurt.
	 */
	estate->es_exprcontexts = lcons(econtext, estate->es_exprcontexts);

	MemoryContextSwitchTo(oldcontext);

	return econtext;
}

/* ----------------
 *		FreeExprContext
 *
 *		Free an expression context, including calling any remaining
 *		shutdown callbacks.
 *
 * Since we free the temporary context used for expression evaluation,
 * any previously computed pass-by-reference expression result will go away!
 *
 * Note we make no assumption about the caller's memory context.
 * ----------------
 */
void
FreeExprContext(ExprContext *econtext)
{
	EState	   *estate;

	/* Call any registered callbacks */
	ShutdownExprContext(econtext);
	/* And clean up the memory used */
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	/* Unlink self from owning EState */
	estate = econtext->ecxt_estate;
	estate->es_exprcontexts = list_delete_ptr(estate->es_exprcontexts, econtext);
	/* And delete the ExprContext node */
	pfree(econtext);
}

/*
 * ReScanExprContext
 *
 *		Reset an expression context in preparation for a rescan of its
 *		plan node.	This requires calling any registered shutdown callbacks,
 *		since any partially complete set-returning-functions must be canceled.
 *
 * Note we make no assumption about the caller's memory context.
 */
void
ReScanExprContext(ExprContext *econtext)
{
	/* Call any registered callbacks */
	ShutdownExprContext(econtext);
	/* And clean up the memory used */
	MemoryContextReset(econtext->ecxt_per_tuple_memory);
}

/*
 * Build a per-output-tuple ExprContext for an EState.
 *
 * This is normally invoked via GetPerTupleExprContext() macro,
 * not directly.
 */
ExprContext *
MakePerTupleExprContext(EState *estate)
{
	if (estate->es_per_tuple_exprcontext == NULL)
		estate->es_per_tuple_exprcontext = CreateExprContext(estate);

	return estate->es_per_tuple_exprcontext;
}


/* ----------------------------------------------------------------
 *				 miscellaneous node-init support functions
 *
 * Note: all of these are expected to be called with CurrentMemoryContext
 * equal to the per-query memory context.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignExprContext
 *
 *		This initializes the ps_ExprContext field.	It is only necessary
 *		to do this for nodes which use ExecQual or ExecProject
 *		because those routines require an econtext. Other nodes that
 *		don't have to evaluate expressions don't need to do this.
 * ----------------
 */
void
ExecAssignExprContext(EState *estate, PlanState *planstate)
{
	planstate->ps_ExprContext = CreateExprContext(estate);
}

/* ----------------
 *		ExecAssignResultType
 * ----------------
 */
void
ExecAssignResultType(PlanState *planstate,
					 TupleDesc tupDesc, bool shouldFree)
{
	TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc, shouldFree);
}

/* ----------------
 *		ExecAssignResultTypeFromTL
 * ----------------
 */
void
ExecAssignResultTypeFromTL(PlanState *planstate)
{
	bool		hasoid;
	TupleDesc	tupDesc;

	if (ExecContextForcesOids(planstate, &hasoid))
	{
		/* context forces OID choice; hasoid is now set correctly */
	}
	else
	{
		/* given free choice, don't leave space for OIDs in result tuples */
		hasoid = false;
	}

	/*
	 * ExecTypeFromTL needs the parse-time representation of the tlist, not a
	 * list of ExprStates.	This is good because some plan nodes don't bother
	 * to set up planstate->targetlist ...
	 */
	tupDesc = ExecTypeFromTL(planstate->plan->targetlist, hasoid);
	ExecAssignResultType(planstate, tupDesc, true);
}

/* ----------------
 *		ExecGetResultType
 * ----------------
 */
TupleDesc
ExecGetResultType(PlanState *planstate)
{
	TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

	return slot->tts_tupleDescriptor;
}

/* ----------------
 *		ExecBuildProjectionInfo
 *
 * Build a ProjectionInfo node for evaluating the given tlist in the given
 * econtext, and storing the result into the tuple slot.  (Caller must have
 * ensured that tuple slot has a descriptor matching the tlist!)  Note that
 * the given tlist should be a list of ExprState nodes, not Expr nodes.
 * ----------------
 */
ProjectionInfo *
ExecBuildProjectionInfo(List *targetList,
						ExprContext *econtext,
						TupleTableSlot *slot)
{
	ProjectionInfo *projInfo = makeNode(ProjectionInfo);
	int			len;
	bool		isVarList;
	ListCell   *tl;

	len = ExecTargetListLength(targetList);

	projInfo->pi_targetlist = targetList;
	projInfo->pi_exprContext = econtext;
	projInfo->pi_slot = slot;

	/*
	 * Determine whether the target list consists entirely of simple Var
	 * references (ie, references to non-system attributes).  If so, we can
	 * use the simpler ExecVariableList instead of ExecTargetList.
	 */
	isVarList = true;
	foreach(tl, targetList)
	{
		GenericExprState *gstate = (GenericExprState *) lfirst(tl);
		Var		   *variable = (Var *) gstate->arg->expr;

		if (variable == NULL ||
			!IsA(variable, Var) ||
			variable->varattno <= 0)
		{
			isVarList = false;
			break;
		}
	}
	projInfo->pi_isVarList = isVarList;

	if (isVarList)
	{
		int		   *varSlotOffsets;
		int		   *varNumbers;
		AttrNumber	lastInnerVar = 0;
		AttrNumber	lastOuterVar = 0;
		AttrNumber	lastScanVar = 0;

		projInfo->pi_itemIsDone = NULL; /* not needed */
		projInfo->pi_varSlotOffsets = varSlotOffsets = (int *)
			palloc0(len * sizeof(int));
		projInfo->pi_varNumbers = varNumbers = (int *)
			palloc0(len * sizeof(int));

		/*
		 * Set up the data needed by ExecVariableList.	The slots in which the
		 * variables can be found at runtime are denoted by the offsets of
		 * their slot pointers within the econtext.  This rather grotty
		 * representation is needed because the caller may not have given us
		 * the real econtext yet (see hacks in nodeSubplan.c).
		 */
		foreach(tl, targetList)
		{
			GenericExprState *gstate = (GenericExprState *) lfirst(tl);
			Var		   *variable = (Var *) gstate->arg->expr;
			AttrNumber	attnum = variable->varattno;
			TargetEntry *tle = (TargetEntry *) gstate->xprstate.expr;
			AttrNumber	resind = tle->resno - 1;

			Assert(resind >= 0 && resind < len);
			varNumbers[resind] = attnum;

			switch (variable->varno)
			{
				case INNER:
					varSlotOffsets[resind] = offsetof(ExprContext,
													  ecxt_innertuple);
					lastInnerVar = Max(lastInnerVar, attnum);
					break;

				case OUTER:
					varSlotOffsets[resind] = offsetof(ExprContext,
													  ecxt_outertuple);
					lastOuterVar = Max(lastOuterVar, attnum);
					break;

				default:
					varSlotOffsets[resind] = offsetof(ExprContext,
													  ecxt_scantuple);
					lastScanVar = Max(lastScanVar, attnum);
					break;
			}
		}
		projInfo->pi_lastInnerVar = lastInnerVar;
		projInfo->pi_lastOuterVar = lastOuterVar;
		projInfo->pi_lastScanVar = lastScanVar;
	}
	else
	{
		projInfo->pi_itemIsDone = (ExprDoneCond *)
			palloc(len * sizeof(ExprDoneCond));
		projInfo->pi_varSlotOffsets = NULL;
		projInfo->pi_varNumbers = NULL;
	}

	return projInfo;
}

/* ----------------
 *		ExecAssignProjectionInfo
 *
 * forms the projection information from the node's targetlist
 * ----------------
 */
void
ExecAssignProjectionInfo(PlanState *planstate)
{
	planstate->ps_ProjInfo =
		ExecBuildProjectionInfo(planstate->targetlist,
								planstate->ps_ExprContext,
								planstate->ps_ResultTupleSlot);
}


/* ----------------
 *		ExecFreeExprContext
 *
 * A plan node's ExprContext should be freed explicitly during executor
 * shutdown because there may be shutdown callbacks to call.  (Other resources
 * made by the above routines, such as projection info, don't need to be freed
 * explicitly because they're just memory in the per-query memory context.)
 *
 * However ... there is no particular need to do it during ExecEndNode,
 * because FreeExecutorState will free any remaining ExprContexts within
 * the EState.	Letting FreeExecutorState do it allows the ExprContexts to
 * be freed in reverse order of creation, rather than order of creation as
 * will happen if we delete them here, which saves O(N^2) work in the list
 * cleanup inside FreeExprContext.
 * ----------------
 */
void
ExecFreeExprContext(PlanState *planstate)
{
	/*
	 * Per above discussion, don't actually delete the ExprContext. We do
	 * unlink it from the plan node, though.
	 */
	planstate->ps_ExprContext = NULL;
}

/* ----------------------------------------------------------------
 *		the following scan type support functions are for
 *		those nodes which are stubborn and return tuples in
 *		their Scan tuple slot instead of their Result tuple
 *		slot..	luck fur us, these nodes do not do projections
 *		so we don't have to worry about getting the ProjectionInfo
 *		right for them...  -cim 6/3/91
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecGetScanType
 * ----------------
 */
TupleDesc
ExecGetScanType(ScanState *scanstate)
{
	TupleTableSlot *slot = scanstate->ss_ScanTupleSlot;

	return slot->tts_tupleDescriptor;
}

/* ----------------
 *		ExecAssignScanType
 * ----------------
 */
void
ExecAssignScanType(ScanState *scanstate,
				   TupleDesc tupDesc, bool shouldFree)
{
	TupleTableSlot *slot = scanstate->ss_ScanTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc, shouldFree);
}

/* ----------------
 *		ExecAssignScanTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignScanTypeFromOuterPlan(ScanState *scanstate)
{
	PlanState  *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlanState(scanstate);
	tupDesc = ExecGetResultType(outerPlan);

	ExecAssignScanType(scanstate, tupDesc, false);
}


/* ----------------------------------------------------------------
 *				  ExecInsertIndexTuples support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 *
 *		This used to be horribly ugly code, and slow too because it
 *		did a sequential scan of pg_index.	Now we rely on the relcache
 *		to cache a list of the OIDs of the indices associated with any
 *		specific relation, and we use the pg_index syscache to get the
 *		entries we need from pg_index.
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(ResultRelInfo *resultRelInfo)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *indexoidlist;
	ListCell   *l;
	int			len,
				i;
	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;

	resultRelInfo->ri_NumIndices = 0;

	/* fast path if no indexes */
	if (!RelationGetForm(resultRelation)->relhasindex)
		return;

	/*
	 * Get cached list of index OIDs
	 */
	indexoidlist = RelationGetIndexList(resultRelation);
	len = list_length(indexoidlist);
	if (len == 0)
		return;

	/*
	 * allocate space for result arrays
	 */
	relationDescs = (RelationPtr) palloc(len * sizeof(Relation));
	indexInfoArray = (IndexInfo **) palloc(len * sizeof(IndexInfo *));

	resultRelInfo->ri_NumIndices = len;
	resultRelInfo->ri_IndexRelationDescs = relationDescs;
	resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

	/*
	 * For each index, open the index relation and save pg_index info.
	 */
	i = 0;
	foreach(l, indexoidlist)
	{
		Oid			indexOid = lfirst_oid(l);
		Relation	indexDesc;
		IndexInfo  *ii;

		/*
		 * Open and lock the index relation
		 *
		 * If the index AM supports concurrent updates, obtain
		 * RowExclusiveLock to signify that we are updating the index.	This
		 * locks out only operations that need exclusive access, such as
		 * relocating the index to a new tablespace.
		 *
		 * If the index AM is not safe for concurrent updates, obtain an
		 * exclusive lock on the index to lock out other updaters as well as
		 * readers (index_beginscan places AccessShareLock).
		 *
		 * If there are multiple not-concurrent-safe indexes, all backends
		 * must lock the indexes in the same order or we will get deadlocks
		 * here. This is guaranteed by RelationGetIndexList(), which promises
		 * to return the index list in OID order.
		 *
		 * The locks will be released in ExecCloseIndices.
		 */
		indexDesc = index_open(indexOid);

		if (indexDesc->rd_am->amconcurrent)
			LockRelation(indexDesc, RowExclusiveLock);
		else
			LockRelation(indexDesc, AccessExclusiveLock);

		/* extract index key information from the index's pg_index info */
		ii = BuildIndexInfo(indexDesc);

		relationDescs[i] = indexDesc;
		indexInfoArray[i] = ii;
		i++;
	}

	list_free(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr indexDescs;

	numIndices = resultRelInfo->ri_NumIndices;
	indexDescs = resultRelInfo->ri_IndexRelationDescs;

	for (i = 0; i < numIndices; i++)
	{
		if (indexDescs[i] == NULL)
			continue;			/* shouldn't happen? */

		/* Drop lock acquired by ExecOpenIndices */
		if (indexDescs[i]->rd_am->amconcurrent)
			UnlockRelation(indexDescs[i], RowExclusiveLock);
		else
			UnlockRelation(indexDescs[i], AccessExclusiveLock);

		index_close(indexDescs[i]);
	}

	/*
	 * XXX should free indexInfo array here too?  Currently we assume that
	 * such stuff will be cleaned up automatically in FreeExecutorState.
	 */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		This routine takes care of inserting index tuples
 *		into all the relations indexing the result relation
 *		when a heap tuple is inserted into the result relation.
 *		Much of this code should be moved into the genam
 *		stuff as it only exists here because the genam stuff
 *		doesn't provide the functionality needed by the
 *		executor.. -cim 9/27/89
 * ----------------------------------------------------------------
 */
void
ExecInsertIndexTuples(TupleTableSlot *slot,
					  ItemPointer tupleid,
					  EState *estate,
					  bool is_vacuum)
{
	ResultRelInfo *resultRelInfo;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/*
	 * Get information from the result relation info structure.
	 */
	resultRelInfo = estate->es_result_relation_info;
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndices; i++)
	{
		IndexInfo  *indexInfo;

		if (relationDescs[i] == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			List	   *predicate;

			/*
			 * If predicate state not set up yet, create it (in the estate's
			 * per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NIL)
			{
				predicate = (List *)
					ExecPrepareExpr((Expr *) indexInfo->ii_Predicate,
									estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext, false))
				continue;
		}

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * The index AM does the rest.	Note we suppress unique-index checks
		 * if we are being called from VACUUM, since VACUUM may need to move
		 * dead tuples that have the same keys as live ones.
		 */
		index_insert(relationDescs[i],	/* index relation */
					 values,	/* array of index Datums */
					 isnull,	/* null flags */
					 tupleid,	/* tid of heap tuple */
					 heapRelation,
					 relationDescs[i]->rd_index->indisunique && !is_vacuum);

		/*
		 * keep track of index inserts for debugging
		 */
		IncrIndexInserted();
	}
}

/*
 * UpdateChangedParamSet
 *		Add changed parameters to a plan node's chgParam set
 */
void
UpdateChangedParamSet(PlanState *node, Bitmapset *newchg)
{
	Bitmapset  *parmset;

	/*
	 * The plan node only depends on params listed in its allParam set. Don't
	 * include anything else into its chgParam set.
	 */
	parmset = bms_intersect(node->plan->allParam, newchg);

	/*
	 * Keep node->chgParam == NULL if there's not actually any members; this
	 * allows the simplest possible tests in executor node files.
	 */
	if (!bms_is_empty(parmset))
		node->chgParam = bms_join(node->chgParam, parmset);
	else
		bms_free(parmset);
}

/*
 * Register a shutdown callback in an ExprContext.
 *
 * Shutdown callbacks will be called (in reverse order of registration)
 * when the ExprContext is deleted or rescanned.  This provides a hook
 * for functions called in the context to do any cleanup needed --- it's
 * particularly useful for functions returning sets.  Note that the
 * callback will *not* be called in the event that execution is aborted
 * by an error.
 */
void
RegisterExprContextCallback(ExprContext *econtext,
							ExprContextCallbackFunction function,
							Datum arg)
{
	ExprContext_CB *ecxt_callback;

	/* Save the info in appropriate memory context */
	ecxt_callback = (ExprContext_CB *)
		MemoryContextAlloc(econtext->ecxt_per_query_memory,
						   sizeof(ExprContext_CB));

	ecxt_callback->function = function;
	ecxt_callback->arg = arg;

	/* link to front of list for appropriate execution order */
	ecxt_callback->next = econtext->ecxt_callbacks;
	econtext->ecxt_callbacks = ecxt_callback;
}

/*
 * Deregister a shutdown callback in an ExprContext.
 *
 * Any list entries matching the function and arg will be removed.
 * This can be used if it's no longer necessary to call the callback.
 */
void
UnregisterExprContextCallback(ExprContext *econtext,
							  ExprContextCallbackFunction function,
							  Datum arg)
{
	ExprContext_CB **prev_callback;
	ExprContext_CB *ecxt_callback;

	prev_callback = &econtext->ecxt_callbacks;

	while ((ecxt_callback = *prev_callback) != NULL)
	{
		if (ecxt_callback->function == function && ecxt_callback->arg == arg)
		{
			*prev_callback = ecxt_callback->next;
			pfree(ecxt_callback);
		}
		else
			prev_callback = &ecxt_callback->next;
	}
}

/*
 * Call all the shutdown callbacks registered in an ExprContext.
 *
 * The callback list is emptied (important in case this is only a rescan
 * reset, and not deletion of the ExprContext).
 */
static void
ShutdownExprContext(ExprContext *econtext)
{
	ExprContext_CB *ecxt_callback;
	MemoryContext oldcontext;

	/* Fast path in normal case where there's nothing to do. */
	if (econtext->ecxt_callbacks == NULL)
		return;

	/*
	 * Call the callbacks in econtext's per-tuple context.  This ensures that
	 * any memory they might leak will get cleaned up.
	 */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Call each callback function in reverse registration order.
	 */
	while ((ecxt_callback = econtext->ecxt_callbacks) != NULL)
	{
		econtext->ecxt_callbacks = ecxt_callback->next;
		(*ecxt_callback->function) (ecxt_callback->arg);
		pfree(ecxt_callback);
	}

	MemoryContextSwitchTo(oldcontext);
}


/* ----------------------------------------------------------------
 *		CDB Slice Table utilities
 * ----------------------------------------------------------------
 */

/* Attach a slice table to the given Estate structure.	It should
 * consist of blank slices, one for the root plan, one for each
 * Motion node (which roots a slice with a send node), and one for
 * each subplan (which acts as an initplan root node).
 */
void
InitSliceTable(EState *estate, int nMotions, int nSubplans)
{
	SliceTable *table;
	Slice	   *slice;
	int			i,
				n;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	table = makeNode(SliceTable);
	table->nMotions = nMotions;
	table->nSubplans = nSubplans;
	table->slices = NIL;
	table->seqServerHost = NULL;
	table->seqServerPort = -1;
    table->doInstrument = false;

	n = 1 + nMotions + nSubplans;
	for (i = 0; i < n; i++)
	{
		slice = makeNode(Slice);

		slice->sliceIndex = i;
        slice->rootIndex = (i > 0 && i <= nMotions) ? -1 : i;
		slice->gangType = GANGTYPE_UNALLOCATED;
		slice->gangSize = 0;
		slice->primaryGang = NULL;
		slice->mirrorGang = NULL;
		slice->parentIndex = -1;
		slice->children = NIL;
		slice->primaryProcesses = NIL;
		slice->mirrorProcesses = NIL;

		table->slices = lappend(table->slices, slice);
	}

	estate->es_sliceTable = table;
	estate->es_cur_root_idx = 0;
	estate->es_cur_slice_idx = 0;

	MemoryContextSwitchTo(oldcontext);
}


/* Assign gang descriptions to the root slices of the slice forest.
 *
 * The root slices of initPlan slice trees will always run on the QD,
 * which, for the time being, we represent as
 *
 *	  (gangType, gangSize) = <GANGTYPE_UNALLOCATED, 0>.
 *
 * The root slice of the main plan wil run on the QD in case it's a
 * SELECT,	but will run on QE(s) in case it's an INSERT, UPDATE, or
 * DELETE. Because we restrict UPDATE and DELETE to have no motions
 * (i.e., one slice) and because INSERT must always route tuples,
 * the assigment for these will be primary and mirror writer gangs,
 * which we represent as
 *
 *	  (gangType, gangSize) =  <GANGTYPE_PRIMARY_WRITER, N>
 */
void
InitRootSlices(QueryDesc *queryDesc)
{
	EState	   *estate = queryDesc->estate;
	SliceTable *sliceTable = estate->es_sliceTable;
	ListCell   *cell;
	Slice	   *slice;
	int			i;

	foreach(cell, sliceTable->slices)
	{
		slice = (Slice *) lfirst(cell);
		i = slice->sliceIndex;
		if (i == 0)
		{
			/* Main plan root slice */
			switch (queryDesc->operation)
			{
				case CMD_SELECT:
					Assert(slice->gangType == GANGTYPE_UNALLOCATED && slice->gangSize == 0);
					break;

				case CMD_INSERT:
				case CMD_UPDATE:
				case CMD_DELETE:
					slice->gangType = GANGTYPE_PRIMARY_WRITER;
					slice->gangSize = getMPPSegmentCount();
					break;

				default:
					Assert(FALSE);
			}
		}
		if (i <= sliceTable->nMotions)
		{
			/* Non-root slice */
			continue;
		}
		else
		{
			/* InitPlan root slice */
			Assert(slice->gangType == GANGTYPE_UNALLOCATED && slice->gangSize == 0);
		}
	}
}




/*
 * Context for AssignGangs() and helper functions.
 */
typedef struct SliceReq
{
	int			numNgangs;
	int			num1gangs;
	int			nxtNgang;
	int			nxt1gang;
	Gang	  **vecNgangs;
	Gang	  **vec1gangs;
	Gang	   *mirrorWriterGang;
	bool		writer;

}	SliceReq;

/* Forward declarations */
static void InitSliceReq(SliceReq * req);
static void AccumSliceReq(SliceReq * inv, SliceReq * req);
static void InventorySliceTree(Slice ** sliceMap, int sliceIndex, SliceReq * req);
static void AssociateSlicesToProcesses(Slice ** sliceMap, int sliceIndex, SliceReq * req);


/* Function AssignGangs runs on the QD and finishes construction of the
 * global slice table for a plan by assigning gangs allocated by the
 * executor factory to the slices of the slice table.
 *
 * On entry, the slice table (at queryDesc->estate->es_sliceTable) has
 * the correct structure (established by InitSliceTable) and has correct
 * gang types (established by function InitRootSlices).
 *
 * Gang assignment involves taking an inventory of the requirements of
 * each slice tree in the slice table, asking the executor factory to
 * allocate a minimal set of gangs that can satisfy any of the slice trees,
 * and associating the allocated gangs with slices in the slice table.
 *
 * The argument utility_segment_index is the segment index to use for
 * 1-gangs that run on QEs.
 *
 * TODO Currently (July 2005) this argument is always supplied as 0, but
 *		there are no cases of the planner specifying a fixed Motion to a
 *		QE, so we don't know the case works.
 *
 * On successful exit, the CDBProcess lists (primaryProcesses, mirrorProcesses)
 * and the Gang pointers (primaryGang, mirrorGang) are set correctly in each
 * slice in the slice table.
 */
void
AssignGangs(QueryDesc *queryDesc, int utility_segment_index)
{
	EState	   *estate = queryDesc->estate;
	SliceTable *sliceTable = estate->es_sliceTable;
	ListCell   *cell;
	Slice	   *slice;
	int			i,
				nslices;
	Slice	  **sliceMap;
	SliceReq	req,
				inv;

	/* Make a map so we can access slices quickly by index.  */
	nslices = list_length(sliceTable->slices);
	sliceMap = (Slice **) palloc(nslices * sizeof(Slice *));
	i = 0;
	foreach(cell, sliceTable->slices)
	{
		slice = (Slice *) lfirst(cell);
		Assert(i == slice->sliceIndex);
		sliceMap[i] = slice;
		i++;
	}

	/* Initialize gang requirement inventory */
	InitSliceReq(&inv);

	/* Capture main slice tree requirement. */
	InventorySliceTree(sliceMap, 0, &inv);

	/* Capture initPlan slice tree requirements. */
	for (i = sliceTable->nMotions + 1; i < nslices; i++)
	{
		InitSliceReq(&req);
		InventorySliceTree(sliceMap, i, &req);
		AccumSliceReq(&inv, &req);
	}

	/* Get the gangs we'll use. */
	if (inv.numNgangs > 0)
	{
		inv.vecNgangs = (Gang **) palloc(sizeof(Gang *) * inv.numNgangs);
		for (i = 0; i < inv.numNgangs; i++)
		{
			if (i == 0)
			{
				inv.vecNgangs[i] = allocateGang(GANGTYPE_PRIMARY_WRITER, getMPPSegmentCount(), 0);
				Assert(inv.vecNgangs[i] != NULL);
				if (inv.writer)
				{
					inv.mirrorWriterGang = allocateGang(GANGTYPE_MIRROR_WRITER, getMPPSegmentCount(), 0);
				}
			}
			else
			{
				inv.vecNgangs[i] = allocateGang(GANGTYPE_READER, getMPPSegmentCount(), 0);
			}
		}
	}
	if (inv.num1gangs > 0)
	{
		inv.vec1gangs = (Gang **) palloc(sizeof(Gang *) * inv.num1gangs);
		for (i = 0; i < inv.num1gangs; i++)
		{
			inv.vec1gangs[i] = allocateGang(GANGTYPE_READER, 1, utility_segment_index);
		}
	}

	/* Use the gangs to construct the CdbProcess lists in slices. */

	inv.nxtNgang = inv.nxt1gang = 0;
	AssociateSlicesToProcesses(sliceMap, 0, &inv);		/* Main tree. */

	for (i = sliceTable->nMotions + 1; i < nslices; i++)
	{
		inv.nxtNgang = inv.nxt1gang = 0;
		AssociateSlicesToProcesses(sliceMap, i, &inv);	/* An initPlan */
	}

	/* Clean up */
	pfree(sliceMap);
	if (inv.vecNgangs != NULL)
		pfree(inv.vecNgangs);
	if (inv.vec1gangs != NULL)
		pfree(inv.vec1gangs);

}

void
ReleaseGangs(QueryDesc *queryDesc)
{
	/*
	 * TODO Depending on how we eventually support named Portals for
	 *		cursors, we may have to dig through the queryDesc in order
	 *		to do a more precise job. Here we just free all the gangs.
	 */
	freeGangs();
}


void
InitSliceReq(SliceReq * req)
{
	req->numNgangs = req->num1gangs = 0;
	req->writer = FALSE;
	req->vecNgangs = NULL;
	req->vec1gangs = NULL;
	req->mirrorWriterGang = NULL;
}

void
AccumSliceReq(SliceReq * inv, SliceReq * req)
{
	inv->numNgangs = (inv->numNgangs >= req->numNgangs) ? inv->numNgangs : req->numNgangs;
	inv->num1gangs = (inv->num1gangs >= req->num1gangs) ? inv->num1gangs : req->num1gangs;
	inv->writer = (inv->writer || req->writer);
}

/*
 * Helper for AssignGangs takes a simple inventory of the gangs required
 * by a slice tree.  Recursive.  Closely coupled with AssignGangs.	Not
 * generally useful.
 */
void
InventorySliceTree(Slice ** sliceMap, int sliceIndex, SliceReq * req)
{
	ListCell   *cell;
	int			childIndex;
	Slice	   *slice = sliceMap[sliceIndex];

	switch (slice->gangType)
	{
		case GANGTYPE_UNALLOCATED:
			/* Roots that run on the  QD don't need a gang. */
			break;

		case GANGTYPE_PRIMARY_WRITER:
			req->writer = TRUE;
			/* fall through */
		case GANGTYPE_READER:
			if (slice->gangSize == 1 && getMPPSegmentCount() != 1)
			{
				req->num1gangs++;
			}
			else
			{
				Assert(slice->gangSize == getMPPSegmentCount());
				req->numNgangs++;
			}
			break;

		case GANGTYPE_MIRROR_WRITER:
			/* Shouldn't see this in a slice->gangType. */
			Assert(FALSE);
			break;
	}

	foreach(cell, slice->children)
	{
		childIndex = lfirst_int(cell);
		InventorySliceTree(sliceMap, childIndex, req);
	}
}


/*
 * Helper for AssignGangs uses the gangs in the inventory to fill in the
 * CdbProcess lists in the slice tree.	Recursive.	Closely coupled with
 * AssignGangs.  Not generally useful.
 */
void
AssociateSlicesToProcesses(Slice ** sliceMap, int sliceIndex, SliceReq * req)
{
	ListCell   *cell;
	int			childIndex;
	Slice	   *slice = sliceMap[sliceIndex];

	switch (slice->gangType)
	{
		case GANGTYPE_UNALLOCATED:
			/* Roots that run on the  QD don't need a gang. */

			slice->primaryGang = NULL;
			slice->primaryProcesses = getCdbProcessesForQD(true);
			break;

		case GANGTYPE_PRIMARY_WRITER:
			Assert(slice->gangSize == getMPPSegmentCount());
			Assert(req->numNgangs > 0 && req->nxtNgang == 0 && req->writer);
			Assert(req->vecNgangs[0] != NULL);

			slice->primaryGang = req->vecNgangs[req->nxtNgang];
			Assert(slice->primaryGang != NULL);
			slice->primaryProcesses = getCdbProcessList(slice->primaryGang);
			slice->mirrorGang = req->mirrorWriterGang;
			if (slice->mirrorGang != NULL)
				slice->mirrorProcesses = getCdbProcessList(slice->mirrorGang);
			else
				slice->mirrorProcesses = NIL;

			req->nxtNgang++;
			break;

		case GANGTYPE_READER:
			if (slice->gangSize == 1 && getMPPSegmentCount() != 1)
			{
				slice->primaryGang = req->vec1gangs[req->nxt1gang];
				slice->primaryProcesses = getCdbProcessList(slice->primaryGang);
				req->nxt1gang++;
			}
			else
			{
				Assert(slice->gangSize == getMPPSegmentCount());
				slice->primaryGang = req->vecNgangs[req->nxtNgang];
				slice->primaryProcesses = getCdbProcessList(slice->primaryGang);
				req->nxtNgang++;
			}
			Assert(slice->primaryGang != NULL);
			break;

		case GANGTYPE_MIRROR_WRITER:
			/* Shouldn't see this in a slice->gangType. */
			Assert(FALSE);
			break;
	}

	foreach(cell, slice->children)
	{
		childIndex = lfirst_int(cell);
		AssociateSlicesToProcesses(sliceMap, childIndex, req);
	}

}

/*
 * A forgiving slice table indexer that returns the indexed Slice* or NULL
 */
Slice *
getCurrentSlice(EState *estate)
{
	SliceTable *sliceTable;
	int			sliceIdx;

	if (estate == NULL)
		return NULL;

	sliceTable = estate->es_sliceTable;
	sliceIdx = estate->es_cur_slice_idx;

	if (sliceTable == NULL)
		return NULL;

	if (sliceIdx < 0 || sliceIdx >= list_length(sliceTable->slices))
		return NULL;

	return (Slice *) list_nth(sliceTable->slices, sliceIdx);
}

/* Should the slice run on the QD?
 *
 * N.B. Not the same as !sliceRunsOnQE(slice), when slice is NULL.
 */
bool
sliceRunsOnQD(Slice * slice)
{
	return (slice != NULL && slice->gangType == GANGTYPE_UNALLOCATED);
}


/* Should the slice run on a QE?
 *
 * N.B. Not the same as !sliceRunsOnQD(slice), when slice is NULL.
 */
bool
sliceRunsOnQE(Slice * slice)
{
	return (slice != NULL && slice->gangType != GANGTYPE_UNALLOCATED);
}

/*
 * Choose the execution identity (who does this executor serve?).
 * There are types:
 *
 * 1. No-Op (ignore) -- this occurs when the specified direction is
 *	 NoMovementScanDirection or when Mpp_role is MPP_ROLE_DISPATCH
 *	 and the current slice belongs to a QE.
 *
 * 2. Executor serves a Root Slice -- this occurs when Mpp_role is
 *   MPP_ROLE_UTILITY or the current slice is a root.  It corresponds
 *   to the "normal" path through the executor in that we enter the plan
 *   at the top and count on the motion nodes at the fringe of the top
 *   slice to return without ever calling nodes below them.
 *
 * 3. Executor serves a Non-Root Slice on a QE -- this occurs when
 *   Mpp_role is MPP_ROLE_EXECUTE and the current slice is not a root
 *   slice. It corresponds to a QE running a slice with a motion node on
 *	 top.  The call, thus, returns no tuples (since they all go out
 *	 on the interconnect to the receiver version of the motion node),
 *	 but it does execute the indicated slice down to any fringe
 *	 motion nodes (as in case 2).
 */
MppExecIdentity
getMppExecIdentity(QueryDesc *queryDesc,
				   ScanDirection direction,
				   EState	   *estate)
{
	Slice              *currentSlice;
	
	currentSlice = getCurrentSlice(estate);
	if (currentSlice)
    {
        if (Mpp_role == MPP_ROLE_EXECUTE ||
            sliceRunsOnQD(currentSlice))
            currentSliceId = currentSlice->sliceIndex;
    }
	
	/* select the strategy */
	if (direction == NoMovementScanDirection)
	{
		return MPP_IGNORE;
	}
	else if (Mpp_role == MPP_ROLE_DISPATCH && sliceRunsOnQE(currentSlice))
	{
		return MPP_IGNORE;
	}
	else if (Mpp_role == MPP_ROLE_EXECUTE && estate->es_cur_slice_idx != estate->es_cur_root_idx)
	{
		return MPP_NON_ROOT_ON_QE;
	}
	else
	{
		return MPP_ROOT_SLICE;
	}
}

/*
 * Cleanup before exiting the executor. Will normally be
 * called from within a CATCH block.
 */
void mppExecCleanup(EState *estate)
{
	/*
	 * Request any commands still executing on qExecs to stop.
	 * Wait for them to finish and clean up the dispatching structures.
	 * Replace current error info with QE error info if more interesting.
	 */
	if (estate->primaryResults)
		cdbdisp_handleError(estate->primaryResults, estate->mirrorResults);
	
	/* teardown the sequence server. */
	TeardownSequenceServer();
	
	/* Clean up the interconnect. */
	if (estate->es_interconnect_is_setup)
	{
		TeardownInterconnect(true);
		estate->es_interconnect_is_setup = false;
	}
	
	/* Clean up our own motion layer. */
	if (Mpp_role != MPP_ROLE_UTILITY)
		ResetMotionLayer(true);
	
}

