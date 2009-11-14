/*-------------------------------------------------------------------------
 *
 * cdbtargeteddispatch.c
 *	  Assign targeted dispatching information to a plan
 *
 * Copyright (c) 2004-2009, Greenplum inc
 *
 *	         $Id: //cdb2/main/cdb-pg/src/backend/cdb/cdbmutate.c#141 $
 *       $Change: 37101 $
 *     $DateTime: 2009/09/01 13:49:08 $
 *       $Author: cwhipkey $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "cdb/cdbtargeteddispatch.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"	/* for rt_fetch() */
#include "nodes/makefuncs.h"	/* for makeVar() */
#include "utils/relcache.h"     /* RelationGetPartitioningKey() */
#include "optimizer/predtest.h"

#include "catalog/gp_policy.h"
#include "catalog/pg_type.h"

#include "catalog/pg_proc.h"
#include "utils/syscache.h"

#include "cdb/cdbdisp.h"
#include "cdb/cdbfts.h"         /* isFTSEnabled() */
#include "cdb/cdbhash.h"        /* isGreenplumDbHashable() */
#include "cdb/cdbllize.h"
#include "cdb/cdbmutate.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbvars.h"

#include "executor/executor.h"
#include "optimizer/geqo_random.h"

#define PRINT_DISPATCH_DECISIONS_STRING ("print_dispatch_decisions")

/**
 * test cases needed:
 *   # basic cases
 *   # subquery in the select list or the predicate: make sure we assign that one properly
 *   # select portion as part of larger plan
 *   # bitmap scans (including and/or)
 */

static char *gp_test_options = ""; //PRINT_DISPATCH_DECISIONS_STRING; // todo: make GUC instead

/**
 * Used when building up all of the composite distribution keys.
 */
typedef struct PartitionKeyInfo
{
	Form_pg_attribute attr;
	Node **values;
	int counter;
	int numValues;
} PartitionKeyInfo;

/**
 * Targeted dispatch info: used when building the dispatch information for a given qual or slice
 */
typedef struct DirectDispatchCalculationInfo
{
	/*
	 * in this use of the  DirectDispatchInfo structure, a NULL contentIds means that
	 *    ANY contentID is valid (when dd->isDirectDispatch is true)
	 * note also that we don't free dd->contentIds.  We let memory context cleanup handle that.
	 */
	DirectDispatchInfo dd;
	bool haveProcessedAnyCalculations;
} DirectDispatchCalculationInfo;

/**
 * ContentIdAssignmentData: used as the context parameter to the plan walking function that assigns
 *   targeted dispatch values to slices
 */
typedef struct ContentIdAssignmentData
{
	/* the range table! note, for subqueries will the range table be the same throughout?  */
	List *rtable;

	/* of DirectDispatchCalculationInfo pointers */
	List *sliceStack;

	/* for use when constructing the output data structure */
	MemoryContext memoryContextForOutput;
} ContentIdAssignmentData;

static bool AssignContentIdsToPlanData_Walker(Node *node, void *context);

/**
 * Add the given constant to the given hash, properly checking for null and
 *   using the constant's type and value otherwise
 */
static inline
void cdbhashconstvalue(CdbHash *h, Const *c)
{
	if ( c->constisnull)
	{
		cdbhashnull(h);
	}
	else
	{
		cdbhash(h, c->constvalue, typeIsArrayType(c->consttype) ? ANYARRAYOID : c->consttype);
	}
}

/**
 * Initialize a DirectDispatchCalculationInfo.
 */
static void
InitDirectDispatchCalculationInfo( DirectDispatchCalculationInfo *data)
{
	data->dd.isDirectDispatch = false;
	data->dd.contentIds = NULL;
	data->haveProcessedAnyCalculations = false;
}

/**
 * Mark a DirectDispatchCalculationInfo as having targeted dispatch disabled
 */
static void
DisableTargetedDispatch( DirectDispatchCalculationInfo *data)
{
	data->dd.isDirectDispatch = false;
	data->dd.contentIds = NULL; /* leaks but it's okay, we made a new memory context for the entire calculation */
	data->haveProcessedAnyCalculations = true;
}

/**
 * helper function for AssignContentIdsFromUpdateDeleteQualification
 */
static DirectDispatchCalculationInfo
GetContentIdsFromPlanForSingleRelation(List *rtable, Plan *plan, int rangeTableIndex)
{
	GpPolicy  *policy = NULL;
	PartitionKeyInfo *parts = NULL;
	int i;

	DirectDispatchCalculationInfo result;
	RangeTblEntry  *rte;

	InitDirectDispatchCalculationInfo(&result);

	if ( nodeTag((Node*)plan) == T_BitmapHeapScan)
	{
		/* do not assert for bitmap heap scan --> it can have a child which is an index scan */
		/* in fact, checking the quals for the bitmap heap scan are redundant with checking
		 *  them on the child scan.  But it won't cause any harm since we will recurse to the
		 *  child scan.
		 */
	}
	else
	{
		Assert(plan->lefttree == NULL);
	}
	Assert(plan->righttree == NULL);

	/* open and get relation info */
	{
		Relation relation;

		rte = rt_fetch(rangeTableIndex, rtable);
		if(rte->rtekind == RTE_RELATION)
		{
			/* Get a copy of the rel's GpPolicy from the relcache. */
			relation = relation_open(rte->relid, NoLock);
			policy = RelationGetPartitioningKey(relation);

			if ( policy != NULL)
			{
				parts = (PartitionKeyInfo*)palloc(policy->nattrs * sizeof(PartitionKeyInfo));
				for ( i = 0; i < policy->nattrs; i++)
				{
					parts[i].attr = relation->rd_att->attrs[policy->attrs[i]-1];
					parts[i].values = NULL;
					parts[i].numValues = 0;
					parts[i].counter = 0;
				}
			}
			relation_close(relation, NoLock);
		}
		else
		{
			/* fall through, policy will be NULL so we won't direct dispatch */
		}
	}

	if ( rte->forceDistRandom ||
			policy == NULL ||
			policy->nattrs == 0 ||
			policy->ptype != POLICYTYPE_PARTITIONED )
	{
		result.dd.isDirectDispatch = false;
	}
	else
	{
		long totalCombinations = 1;

		/* calculate possible value set for each partitioning attribute */
		for ( i = 0; i < policy->nattrs; i++)
		{
			Var *var;
			PossibleValueSet pvs;

			var = makeVar(rangeTableIndex, policy->attrs[i], parts[i].attr->atttypid, parts[i].attr->atttypmod, 0);

			/**
			 * Note that right now we only examine the plan qual.  This is okay because if there are other
			 *   quals on the plan then those would be ANDed with the qual, which can only narrow our choice
			 *   of segment and not expand it.
			 */
			pvs = DeterminePossibleValueSet((Node*)plan->qual, (Node *)var );

			if ( pvs.isAnyValuePossible)
			{
				/* can't isolate to single statement -- totalCombinations = -1 will signal this */
				DeletePossibleValueSetData(&pvs);
				totalCombinations = -1;
				break;
			}
			else
			{
				parts[i].values = GetPossibleValuesAsArray( &pvs, &parts[i].numValues );
				totalCombinations *= parts[i].numValues;
				DeletePossibleValueSetData(&pvs);
			}
		}

		/* calculate possible target content ids from the combinations of partitioning attributes */
		if ( totalCombinations == 0 )
		{
			/* one of the possible sets was empty and so we don't care where we run this */
			result.dd.isDirectDispatch = true;
			Assert(result.dd.contentIds == NULL); /* direct dispatch but no specific content at all! */
		}
		else if ( totalCombinations > 0 &&
				/* don't bother for ones which will likely hash to many segments */
				totalCombinations < GpIdentity.numsegments * 3 )
		{
			CdbHash *h = makeCdbHash(GpIdentity.numsegments, HASH_FNV_1);
			long index = 0;

			result.dd.isDirectDispatch = true;
			result.dd.contentIds = NULL;

			/* for each combination of keys calculate target segment */
			for ( index = 0; index < totalCombinations; index++ )
			{
				long curIndex = index;
				int hashCode;

				/* hash the attribute values */
				cdbhashinit(h);

				for ( i = 0; i < policy->nattrs; i++)
				{
					int numValues = parts[i].numValues;
					int which = curIndex % numValues;
					Node *val = parts[i].values[which];

					if ( IsA(val, Const))
					{
						cdbhashconstvalue(h, (Const*) val);
					}
					else
					{
						result.dd.isDirectDispatch = false;
						break;
					}
					curIndex /= numValues;
				}

				if ( ! result.dd.isDirectDispatch)
					break;

				hashCode = cdbhashreduce(h);

				/*
				 * right now we only allow ONE contentid
				 */
				if ( result.dd.contentIds == NULL )
					result.dd.contentIds = list_make1_int(hashCode);
				else if ( linitial_int(result.dd.contentIds) != hashCode)
				{
					result.dd.isDirectDispatch = false;
					break;
				}
			}
		}
		else
		{
			// know nothing, can't do directed dispatch
			result.dd.isDirectDispatch = false;
		}
	}

	result.haveProcessedAnyCalculations = true;
	return result;
}

static void
MergeDirectDispatchCalculationInfo( DirectDispatchCalculationInfo *to, DirectDispatchCalculationInfo *from )
{
	if ( ! from->dd.isDirectDispatch )
	{
		/* from eliminates all options so take it */
		to->dd.isDirectDispatch = false;
	}
	else if ( ! to->haveProcessedAnyCalculations )
	{
		/* to has no data, so just take from */
		*to = *from;
	}
	else if ( ! to->dd.isDirectDispatch )
	{
		/* to cannot get better -- leave it alone */
	}
	else if ( from->dd.contentIds == NULL )
	{
		/* from says that it doesn't need to run anywhere -- so we accept to */
	}
	else if ( to->dd.contentIds == NULL )
	{
		/* to didn't even think it needed to run so accept from */
		to->dd.contentIds = from->dd.contentIds;
	}
	else if ( linitial_int(to->dd.contentIds) != linitial_int( from->dd.contentIds ))
	{
		/* we only support dispatch to single segment or all segments, so can't support this right now */
		/* but this needs to be a union of the segments */
		to->dd.isDirectDispatch = false;
	}
	else
	{
		/* they matched -- no merge required! */

		Assert(list_length(to->dd.contentIds) == list_length(from->dd.contentIds));
	}

	to->haveProcessedAnyCalculations = true;
}

/**
 * returns true if we should print test messages.  Note for clients: for multi-slice queries then messages will print in
 *   the order of processing which may not always be deterministic (single joins can be rearranged by the planner,
 *   for example).
 */
static bool
ShouldPrintTestMessages()
{
	return gp_test_options && strstr(gp_test_options, PRINT_DISPATCH_DECISIONS_STRING ) != NULL;
}

/**
 * If node is not a plan then no direct dispatch data is copied in.
 *
 * Otherwise, overrides any previous direct dispatch data on the given node and updates it
 *   with the targeted dispatch info from the ContentIdAssignmentData
 *
 */
static void
FinalizeDirectDispatchDataForSlice(Node *node, ContentIdAssignmentData *data, bool isFromTopRoot)
{
	/* pop it from the top and set it on the root of the slice */
	DirectDispatchCalculationInfo *ddcr = (DirectDispatchCalculationInfo*) lfirst(list_tail(data->sliceStack));
	data->sliceStack = list_truncate(data->sliceStack, list_length(data->sliceStack) - 1);

	if ( is_plan_node(node))
	{
		Plan *plan = (Plan*)node;

		if ( plan->directDispatch.contentIds != NULL)
		{
			list_free(plan->directDispatch.contentIds);
			plan->directDispatch.contentIds = NULL;
		}

		if ( ddcr->haveProcessedAnyCalculations)
		{
			plan->directDispatch.isDirectDispatch = ddcr->dd.isDirectDispatch;
			if ( ddcr->dd.isDirectDispatch)
			{
				MemoryContext oldContext;
				if ( ddcr->dd.contentIds == NULL )
				{
					ddcr->dd.contentIds = list_make1_int( geqo_randint(GpIdentity.numsegments - 1, 0));
					if (  ShouldPrintTestMessages())
						elog(INFO, "DDCR learned no content dispatch is required");
				}
				else
				{
					if (  ShouldPrintTestMessages())
						elog(INFO, "DDCR learned dispatch to content %d", linitial_int(ddcr->dd.contentIds));
				}

				oldContext = MemoryContextSwitchTo(data->memoryContextForOutput);
				plan->directDispatch.contentIds = list_copy(ddcr->dd.contentIds);
				MemoryContextSwitchTo(oldContext);
			}
			else
			{
				if (  ShouldPrintTestMessages())
					elog(INFO, "DDCR learned full dispatch is required");
			}
		}
		else
		{
			if (  ShouldPrintTestMessages())
				elog(INFO, "DDCR learned no information: default to full dispatch");
			plan->directDispatch.isDirectDispatch = false;
		}
	}

	pfree(ddcr);
}

/**
 * Recurse properly into a subquery node, handling pushing of range table at the right time.
 */
static bool
AssignContentIdsToPlanData_SubqueryScan(Node *node, void *context)
{
	SubqueryScan *subqueryScan;
	ContentIdAssignmentData *data;
	RangeTblEntry *rte;
	List *oldRtable;
	bool result;

	Assert( IsA(node, SubqueryScan));
	subqueryScan = (SubqueryScan*)node;
	data = (ContentIdAssignmentData *) context;
	oldRtable = data->rtable;

	/* All scan types have a range table entry. */
	rte = rt_fetch(subqueryScan->scan.scanrelid, data->rtable);
	Assert(rte->rtekind == RTE_SUBQUERY);

	/**
	 * (from code for MPP-1980 elsewhere)
	 * todo: how to trigger this test case?
	 */
	result = AssignContentIdsToPlanData_Walker((Node*)(subqueryScan->scan.plan.targetlist), context);
	Assert(!result);

	result = AssignContentIdsToPlanData_Walker((Node*)(subqueryScan->scan.plan.qual), context);
	Assert(!result);

	result = AssignContentIdsToPlanData_Walker((Node*)(subqueryScan->scan.plan.initPlan), context);
	Assert(!result);

	/*
	 * The subplan uses the range table of the Query node
	 * attached to the subquery RTE.
	 */
	data->rtable = rte->subquery->rtable;
	result = AssignContentIdsToPlanData_Walker((Node*)subqueryScan->subplan, context);
	Assert(!result);
	data->rtable = oldRtable;

	return false;
}

/**
 * Recursively assign content ids to the motion nodes of each slice.
 */
static bool
AssignContentIdsToPlanData_Walker(Node *node, void *context)
{
	ContentIdAssignmentData *data = (ContentIdAssignmentData *) context;
	DirectDispatchCalculationInfo *ddcr = NULL;
	DirectDispatchCalculationInfo dispatchInfo;
	bool pushNewDirectDispatchInfo = false;
	bool result;
	List *newRtable = NULL;
	List *oldRtable = NULL;

	if (node == NULL)
		return false;

	if (is_plan_node(node) || IsA(node, SubPlan))
	{
		InitDirectDispatchCalculationInfo(&dispatchInfo);

		switch (nodeTag(node))
		{
			case T_SubPlan:
				{
					/* note that we could look at testexpr itself to try to learn something but I don't think
					 *   that will yield more in the current system than looking at the underlying query itself.
					 */
					SubPlan *subPlan = (SubPlan *) node;
					newRtable = subPlan->rtable;
					pushNewDirectDispatchInfo = true;
				}
				break;
			case T_Result:
				/* disallow here because I haven't examined structure fully: so be conservative */
				DisableTargetedDispatch(&dispatchInfo);
				break;
			case T_Append:
				/* no change to dispatchInfo --> just iterate children */
				break;
			case T_BitmapAnd:
			case T_BitmapOr:
				/* no change to dispatchInfo --> just iterate children */
				break;
			case T_SeqScan:
			case T_ExternalScan:
			case T_AppendOnlyScan:
			case T_AOCSScan:
			case T_IndexScan:
			case T_BitmapIndexScan:
			case T_BitmapHeapScan:
				/* we can determine the dispatch data to merge by looking at the relation begin scanned */
				dispatchInfo = GetContentIdsFromPlanForSingleRelation(data->rtable, (Plan *)node, ((Scan*)node)->scanrelid);
				break;
			case T_SubqueryScan:
				/**
				 * DO NOT fall-through -- subquery scan manually recurses
				 */
				return AssignContentIdsToPlanData_SubqueryScan(node, context);
			case T_TidScan:
			case T_FunctionScan:
				DisableTargetedDispatch(&dispatchInfo);
				break;
			case T_ValuesScan:
				/* no change to dispatchInfo */
				break;
			case T_NestLoop:
			case T_MergeJoin:
			case T_HashJoin:
				/* join: no change to dispatchInfo --> just iterate children */
				/* note that we could want to look at the join qual but constant checks should have been pushed
				 * down to the underlying scans so we shouldn't learn anything */
				break;
			case T_Material:
			case T_Sort:
			case T_Group:
			case T_Agg:
			case T_Unique:
			case T_Hash:
			case T_SetOp:
			case T_Limit:
				break;
			case T_Motion:
				/* receiving end knows nothing (since we don't analyze the distribution key right now) */
				/* ideally we would take subsegments from analyzing the distribution key's distribution but
				 * that is not done right now */
				DisableTargetedDispatch(&dispatchInfo);

				/* and we must push a new slice */
				pushNewDirectDispatchInfo = true;
				break;
			case T_ShareInputScan:
				/* note: could try to peek into the building slice to get its direct dispatch values but we don't */
				DisableTargetedDispatch(&dispatchInfo);
				break;
			case T_Window:
			case T_Repeat:
				/* no change to dispatchInfo --> just iterate children */
				break;
			default:
				elog(ERROR, "Invalid plan node %d", nodeTag(node));
				break;
		}

		/* analyzed node type, now do the work (for all except subquery scan, which do work in the
		 * switch above and return */
		if ( dispatchInfo.haveProcessedAnyCalculations )
		{
			/* learned new info: merge it in */
			ddcr = llast(data->sliceStack);
			MergeDirectDispatchCalculationInfo(ddcr, &dispatchInfo );
		}

		if ( pushNewDirectDispatchInfo)
		{
			/* recursing to a child slice */
			ddcr = palloc(sizeof(DirectDispatchCalculationInfo));
			InitDirectDispatchCalculationInfo(ddcr);
			data->sliceStack = lappend(data->sliceStack, ddcr);
		}

		if ( newRtable != NULL)
		{
			oldRtable = data->rtable;
			data->rtable = newRtable;
		}
	}

	/* recurse -- must recurse even for non-plan nodes because of qualifications? */
	/* note that the SubqueryScan nodes do NOT reach here -- its children are managed in the switch above */
	result = plan_tree_walker(node, AssignContentIdsToPlanData_Walker, context);
	Assert( ! result );

	if ( pushNewDirectDispatchInfo)
	{
		FinalizeDirectDispatchDataForSlice(node, data, false);
	}
	if ( newRtable != NULL)
	{
		Assert(oldRtable != NULL);
		data->rtable = oldRtable;
	}

	return result;
}

/**
 * Update the plan and its descendants with markings telling which subsets of content the node can run on.
 *
 */
void
AssignContentIdsToPlanData(Query *query, Plan *plan)
{
	ContentIdAssignmentData data;
	MemoryContext oldContext, tmpContext;
	DirectDispatchCalculationInfo *ddcr;

	/* setup */
	tmpContext = AllocSetContextCreate(CurrentMemoryContext,
									   "AssignContentIdsToPlanData",
									   ALLOCSET_SMALL_MINSIZE,
									   ALLOCSET_SMALL_INITSIZE,
									   ALLOCSET_SMALL_MAXSIZE);
	oldContext = MemoryContextSwitchTo(tmpContext);

	ddcr = palloc(sizeof(DirectDispatchCalculationInfo));
	InitDirectDispatchCalculationInfo(ddcr);

	data.memoryContextForOutput = oldContext;
	data.sliceStack = list_make1(ddcr);
	data.rtable = query->rtable;

	/* Do it! */
	AssignContentIdsToPlanData_Walker((Node*)plan, &data);

	/** todo: this is hacky peek-ahead...find a cleaner way to do this */
	if ( ! IsA(plan, SubPlan) && ! IsA(plan, Motion))
	{
		FinalizeDirectDispatchDataForSlice((Node*)plan, &data, true);
	}

	/* cleanup */
	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(tmpContext);
}
