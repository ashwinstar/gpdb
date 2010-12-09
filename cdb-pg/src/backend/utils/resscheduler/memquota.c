/*-------------------------------------------------------------------------
 *
 * memquota.c
 * Routines related to memory quota for queries.
 *
 *
 * Copyright (c) 2010, Greenplum inc
 * 
 *-------------------------------------------------------------------------*/
 
#include "cdb/memquota.h"
#include "cdb/cdbllize.h"
#include "storage/lwlock.h"
#include "commands/queue.h"
#include "catalog/pg_resqueue.h"
#include "utils/relcache.h"
#include "executor/execdesc.h"
#include "utils/resscheduler.h"
#include "access/heapam.h"
#include "miscadmin.h"
#include "utils/guc_tables.h"
#include "cdb/cdbvars.h"
#include "optimizer/clauses.h"

/**
 * Policy Auto. This contains information that will be used by Policy AUTO 
 */
typedef struct PolicyAutoContext
{
	plan_tree_base_prefix base; /* Required prefix for plan_tree_walker/mutator */
	uint64 numNonMemIntensiveOperators; /* number of non-blocking operators */
	uint64 numMemIntensiveOperators; /* number of blocking operators */
	uint64 queryMemKB;
} PolicyAutoContext;

/**
 * Forward declarations.
 */
static bool PolicyAutoPrelimWalker(Node *node, PolicyAutoContext *context);
static bool	PolicyAutoAssignWalker(Node *node, PolicyAutoContext *context);
static bool IsAggMemoryIntensive(Agg *agg);
static bool IsMemoryIntensiveOperator(Node *node);
static double minDouble(double a, double b);

/**
 * GUCs
 */
char                		*gp_resqueue_memory_policy_str = NULL;
ResQueueMemoryPolicy		gp_resqueue_memory_policy = RESQUEUE_MEMORY_POLICY_NONE;
bool						gp_log_resqueue_memory = false;
int							gp_resqueue_memory_policy_auto_fixed_mem;
const int					gp_resqueue_memory_log_level=NOTICE;


/**
 * Minimum of two doubles 
 */
static double minDouble(double a, double b)
{
	if (a < b) 
		return a; 
	else 
		return b;
}

/**
 * Is an agg operator memory intensive? The following cases mean it is:
 * 1. If agg strategy is hashed
 * 2. If targetlist or qual contains a DQA
 * 3. If there is an ordered aggregated.
 */
static bool IsAggMemoryIntensive(Agg *agg)
{
	Assert(agg);

	/* Case 1 */
	if (agg->aggstrategy == AGG_HASHED)
	{
		return true;
	}

	AggClauseCounts aggInfo;
	
	/* Zero it out */
	MemSet(&aggInfo, 0, sizeof(aggInfo));

	Plan *plan = (Plan *) &(agg->plan);
	count_agg_clauses((Node *) plan->targetlist, &aggInfo);
	count_agg_clauses((Node *) plan->qual, &aggInfo);
	
	/* Case 2 */
	if (aggInfo.numDistinctAggs >0)
	{
		return true;
	}
	
	/* Case 3 */
	if (aggInfo.aggOrder != NIL )
	{
		return true;
	}
	
	return false;
}

/**
 * Is an operator memory intensive?
 */
static bool IsMemoryIntensiveOperator(Node *node)
{
	Assert(is_plan_node(node));
	switch(nodeTag(node))
	{
		case T_Material:
		case T_Sort:
		case T_ShareInputScan:
		case T_Hash:
		case T_BitmapIndexScan:
		case T_FunctionScan:
		case T_Window:
			return true;
		case T_Agg:
			{
				Agg *agg = (Agg *) node;
				return IsAggMemoryIntensive(agg);
			}
		default:
			return false;
	}
}

/**
 * This walker counts the number of memory intensive and non-memory intensive operators
 * in a plan.
 */

static bool PolicyAutoPrelimWalker(Node *node, PolicyAutoContext *context)
{
	if (node == NULL)
	{
		return false;
	}
	
	Assert(node);
	Assert(context);	
	if (is_plan_node(node))
	{
		if (IsMemoryIntensiveOperator(node))
		{
			context->numMemIntensiveOperators++;
		}
		else
		{
			context->numNonMemIntensiveOperators++;
		}
	}
	return plan_tree_walker(node, PolicyAutoPrelimWalker, context);
}

/**
 * This walker assigns specific amount of memory to each operator in a plan.
 * It allocates a fixed size to each non-memory intensive operator and distributes
 * the rest among memory intensive operators.
 */
static bool PolicyAutoAssignWalker(Node *node, PolicyAutoContext *context)
{
	const uint64 nonBlockingOperatorMemKB = (uint64) gp_resqueue_memory_policy_auto_fixed_mem;

	if (node == NULL)
	{
		return false;
	}
	
	Assert(node);
	Assert(context);
	
	if (is_plan_node(node))
	{
		Plan *planNode = (Plan *) node;
		
		/**
		 * If the operator is not a memory intensive operator, give it fixed amount of memory.
		 */
		if (!IsMemoryIntensiveOperator(node))
		{
			planNode->operatorMemKB = nonBlockingOperatorMemKB;
		}
		else
		{
			planNode->operatorMemKB = (uint64) ( (double) context->queryMemKB 
					- (double) context->numNonMemIntensiveOperators * nonBlockingOperatorMemKB) 
					/ context->numMemIntensiveOperators;
		}

		Assert(planNode->operatorMemKB > 0);

		if (gp_log_resqueue_memory)
		{
			elog(gp_resqueue_memory_log_level, "assigning plan node memory = %dKB", (int )planNode->operatorMemKB);
		}
	}
	return plan_tree_walker(node, PolicyAutoAssignWalker, context);
}

/**
 * Main entry point for memory quota policy AUTO. It counts how many operators
 * there are in a plan. It walks the plan again and allocates a fixed amount to every non-memory intensive operators.
 * It distributes the rest of the memory available to other operators.
 */
 void PolicyAutoAssignOperatorMemoryKB(PlannedStmt *stmt, uint64 memAvailableBytes)
{
	 PolicyAutoContext ctx;
	 exec_init_plan_tree_base(&ctx.base, stmt);
	 ctx.queryMemKB = (uint64) (memAvailableBytes / 1024);
	 ctx.numMemIntensiveOperators = 0;
	 ctx.numNonMemIntensiveOperators = 0;
	 
#ifdef USE_ASSERT_CHECKING
	 bool result = 
#endif
			 PolicyAutoPrelimWalker((Node *) stmt->planTree, &ctx);
	 
	 Assert(!result);
	 Assert(ctx.numMemIntensiveOperators + ctx.numNonMemIntensiveOperators > 0);
	 
	 if (ctx.queryMemKB <= ctx.numNonMemIntensiveOperators * gp_resqueue_memory_policy_auto_fixed_mem)
	 {
		 elog(ERROR, "Unable to reserve sufficient memory for operators. Please increase amount of memory reserved for the statement.");
	 }
	 
#ifdef USE_ASSERT_CHECKING
	 result = 
#endif			 
			 PolicyAutoAssignWalker((Node *) stmt->planTree, &ctx);
	 
	 Assert(!result);
}
 
/**
 * What should be query mem such that memory intensive operators get a certain minimum amount of memory.
 * Return value is in KB.
 */
 uint64 PolicyAutoStatementMemForNoSpillKB(PlannedStmt *stmt, uint64 minOperatorMemKB)
 {
	 Assert(stmt);
	 Assert(minOperatorMemKB > 0);
	 
	 const uint64 nonBlockingOperatorMemKB = (uint64) gp_resqueue_memory_policy_auto_fixed_mem;

	 PolicyAutoContext ctx;
	 exec_init_plan_tree_base(&ctx.base, stmt);
	 ctx.queryMemKB = (uint64) (stmt->query_mem / 1024);
	 ctx.numMemIntensiveOperators = 0;
	 ctx.numNonMemIntensiveOperators = 0;
	 
#ifdef USE_ASSERT_CHECKING
	 bool result = 
#endif
			 PolicyAutoPrelimWalker((Node *) stmt->planTree, &ctx);
	 
	 Assert(!result);
	 Assert(ctx.numMemIntensiveOperators + ctx.numNonMemIntensiveOperators > 0);
	 
	 /**
	  * Right now, the inverse is straightforward.
	  * TODO: Siva - employ binary search to find the right value.
	  */
	 uint64 requiredStatementMemKB = ctx.numNonMemIntensiveOperators * nonBlockingOperatorMemKB 
			 + ctx.numMemIntensiveOperators * minOperatorMemKB;

	 return requiredStatementMemKB;
 }

 /**
  * What is the memory limit on a queue per the catalog in bytes. Returns -1 if not set.
  */
int ResourceQueueGetMemoryLimitInCatalog(Oid queueId)
{
	Assert(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_UTILITY);
	
	int memoryLimitKB = -1;
	
	Assert(queueId != InvalidOid);
		
	List * capabilitiesList = GetResqueueCapabilityEntry(queueId); /* This is a list of lists */

	ListCell *le = NULL; 
	foreach(le, capabilitiesList)
	{
		List *entry = NULL;
		Value *key = NULL;		
		entry = (List *) lfirst(le);
		Assert(entry);
		key = (Value *) linitial(entry);
		Assert(key->type == T_Integer); /* This is resource type id */
		if (intVal(key) == PG_RESRCTYPE_MEMORY_LIMIT)
		{
			Value *val = lsecond(entry);
			Assert(val->type == T_String);

#ifdef USE_ASSERT_CHECKING			
			bool result = 

#endif
					parse_int(strVal(val), &memoryLimitKB, GUC_UNIT_KB, NULL);

			Assert(result);
		}
	}
	list_free(capabilitiesList);

	Assert(memoryLimitKB == -1 || memoryLimitKB > 0);
	
	if (memoryLimitKB == -1)
	{
		return -1;
	}
	
	return memoryLimitKB * 1024L;

}
 
/**
 * Get memory limit associated with queue in bytes. 
 * Returns -1 if a limit does not exist.
 */
int ResourceQueueGetMemoryLimit(Oid queueId)
{
	Assert(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_UTILITY);
	
	int memoryLimitBytes = -1;
	
	Assert(queueId != InvalidOid);
		
	if (gp_resqueue_memory_policy != RESQUEUE_MEMORY_POLICY_NONE)
	{
		memoryLimitBytes = ResourceQueueGetMemoryLimitInCatalog(queueId);
	}
		
	return memoryLimitBytes;
}

/**
 * Given a queueid, how much memory should a query take in bytes.
 */
uint64 ResourceQueueGetQueryMemoryLimit(PlannedStmt *stmt, Oid queueId)
{		
	Assert(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_UTILITY);
	Assert(gp_resqueue_memory_policy == RESQUEUE_MEMORY_POLICY_AUTO);
	Assert(queueId != InvalidOid);
	
	/** Assert that I do not hold lwlock */
	Assert(!LWLockHeldExclusiveByMe(ResQueueLock));

	int resqLimitBytes = ResourceQueueGetMemoryLimit(queueId);

	/**
	 * If there is no memory limit on the queue, simply use statement_mem.
	 */
	if (resqLimitBytes == -1)
	{
		return (uint64) statement_mem * 1024L;
	}
	
	/**
	 * This method should only be called while holding exclusive lock on ResourceQueues. This means
	 * that nobody can modify any resource queue while current process is performing this computation.
	 */
	LWLockAcquire(ResQueueLock, LW_EXCLUSIVE);

	ResQueue resQueue = ResQueueHashFind(queueId);
	
	LWLockRelease(ResQueueLock);

	Assert(resQueue);	
	int numSlots = (int) ceil(resQueue->limits[RES_COUNT_LIMIT].threshold_value);
	double costLimit = (double) resQueue->limits[RES_COST_LIMIT].threshold_value;
	double planCost = stmt->planTree->total_cost;
	
	if (planCost < 1.0)
		planCost = 1.0;

	Assert(planCost > 0.0);
	
	if (gp_log_resqueue_memory)
	{
		elog(gp_resqueue_memory_log_level, "numslots: %d, costlimit: %f", numSlots, costLimit);
	}
	
	if (numSlots < 1)
	{
		/** there is no statement limit set */
		numSlots = 1;
	}
	
	if (costLimit < 0.0)
	{
		/** there is no cost limit set */
		costLimit = planCost;
	}

	double minRatio = minDouble( 1.0/ (double) numSlots, planCost / costLimit);
	
	minRatio = minDouble(minRatio, 1.0);

	if (gp_log_resqueue_memory)
	{
		elog(gp_resqueue_memory_log_level, "slotratio: %0.3f, costratio: %0.3f, minratio: %0.3f",
				1.0/ (double) numSlots, planCost / costLimit, minRatio);
	}
	
	uint64 queryMem = (uint64) resqLimitBytes * minRatio;
	
	/**
	 * If user requests more using statement_mem, grant that.
	 */
	if (queryMem < statement_mem * 1024L)
	{
		queryMem = (uint64) statement_mem * 1024L;
	}
	
	return queryMem;
}

/**
 * How much memory should superuser queries get?
 */
uint64 ResourceQueueGetSuperuserQueryMemoryLimit(void)
{
	Assert(superuser());
	return (uint64) statement_mem * 1024L;
}


