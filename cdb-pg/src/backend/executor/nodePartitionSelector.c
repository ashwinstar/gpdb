/*-------------------------------------------------------------------------
 *
 * nodePartitionSelector.c
 *	  implement the execution of PartitionSelector for selecting partition
 *	  Oids based on a given set of predicates. It works for both constant
 *	  partition elimination and join partition elimination
 *
 * Copyright (c) 2014, Pivotal Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "cdb/cdbpartition.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodePartitionSelector.h"
#include "utils/memutils.h"

static List *
construct_part_constraints(PartitionRule *rule, char parkind);

static PartitionConstraints *
construct_part_constraints_range(PartitionRule *rule);

static List *
construct_part_constraints_list(PartitionRule *rule);

static List *
partition_constraints(PartitionNode *pn);

static List *
partition_constraints_range(PartitionNode *pn);

static List *
partition_constraints_list(PartitionNode *pn);

static PartitionRule*
partition_selection(PartitionNode *pn, PartitionAccessMethods *accessMethods, Oid root_oid, Datum value, bool isNull);

static void
processLevel(PartitionSelectorState *node, int level, TupleTableSlot *inputTuple);

static void
partition_propagation(PartitionSelectorState *node, Oid part_oid);

static bool
eval_part_qual(ExprState *exprstate, PartitionSelectorState *node, TupleTableSlot *inputTuple);

static List *
partition_constraints_for_equality_predicate(PartitionSelectorState *node, int level,
						TupleTableSlot *inputTuple, PartitionNode *parentNode);

static List *
partition_constraints_for_general_predicate(PartitionSelectorState *node, int level,
						TupleTableSlot *inputTuple, PartitionNode *parentNode);


/* PartitionSelector Slots */
#define PARTITIONSELECTOR_NSLOTS 1

/* Return number of TupleTableSlots used by nodePartitionSelector.*/
int
ExecCountSlotsPartitionSelector(PartitionSelector *node)
{
	if (NULL != outerPlan(node))
	{
		return ExecCountSlotsNode(outerPlan(node)) + PARTITIONSELECTOR_NSLOTS;
	}
	return PARTITIONSELECTOR_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecInitPartitionSelector
 *
 *		Create the run-time state information for PartitionSelector node
 *		produced by Orca and initializes outer child if exists.
 *
 * ----------------------------------------------------------------
 */
PartitionSelectorState *
ExecInitPartitionSelector(PartitionSelector *node, EState *estate, int eflags)
{

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)));

	/* create and initialize PartitionSelectorState structure */
	PartitionSelectorState *psstate = makeNode(PartitionSelectorState);
	psstate->ps.plan = (Plan *) node;
	psstate->ps.state = estate;
	psstate->levelPartConstraints = (PartitionConstraints**) palloc0(node->nLevels * sizeof(PartitionConstraints*));

	/* ExprContext initialization */
	ExecAssignExprContext(estate, &psstate->ps);

	/* initialize ExprState for evaluating expressions */
	ListCell *lc = NULL;
	foreach (lc, node->levelEqExpressions)
	{
		Expr *eqExpr = (Expr *) lfirst(lc);
		psstate->levelEqExprStates = lappend(psstate->levelEqExprStates,
								ExecInitExpr(eqExpr, (PlanState *) psstate));
	}

	foreach (lc, node->levelExpressions)
	{
		Expr *generalExpr = (Expr *) lfirst(lc);
		psstate->levelExprStates = lappend(psstate->levelExprStates,
								ExecInitExpr(generalExpr, (PlanState *) psstate));
	}

	psstate->residualPredicateExprState = ExecInitExpr((Expr *) node->residualPredicate,
									(PlanState *) psstate);
	psstate->propagationExprState = ExecInitExpr((Expr *) node->propagationExpression,
									(PlanState *) psstate);

	psstate->ps.targetlist = (List *) ExecInitExpr((Expr *) node->plan.targetlist,
									(PlanState *) psstate);
	/* tuple table initialization */
	ExecInitResultTupleSlot(estate, &psstate->ps);
	ExecAssignResultTypeFromTL(&psstate->ps);
	ExecAssignProjectionInfo(&psstate->ps, NULL);

	/* initialize child nodes */
	/* No inner plan for PartitionSelector */
	Assert(NULL == innerPlan(node));
	if (NULL != outerPlan(node))
	{
		outerPlanState(psstate) = ExecInitNode(outerPlan(node), estate, eflags);
	}

	initGpmonPktForPartitionSelector((Plan *)node, &psstate->ps.gpmon_pkt, estate);

	return psstate;
}

/* ----------------------------------------------------------------
 *		ExecPartitionSelector(node)
 *
 *		Compute and propagate partition table Oids that will be
 *		used by Dynamic table scan. There are two ways of
 *		executing PartitionSelector.
 *
 *		1. Constant partition elimination
 *		Plan structure:
 *			Sequence
 *				|--PartitionSelector
 *				|--DynamicTableScan
 *		In this case, PartitionSelector evaluates constant partition
 *		constraints to compute and propagate partition table Oids.
 *		It only need to be called once.
 *
 *		2. Join partition elimination
 *		Plan structure:
 *			...:
 *				|--DynamicTableScan
 *				|--...
 *					|--PartitionSelector
 *						|--...
 *		In this case, PartitionSelector is in the same slice as
 *		DynamicTableScan, DynamicIndexScan or DynamicBitmapHeapScan.
 *		It is executed for each tuple coming from its child node.
 *		It evaluates partition constraints with the input tuple and
 *		propagate matched partition table Oids.
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecPartitionSelector(PartitionSelectorState *node)
{
	/* Retrieve PartitionNode and access method from root table.
	 * We cannot do it during node initialization as
	 * DynamicTableScanInfo is not properly initialized yet.
	 */
	if (NULL == node->rootPartitionNode)
	{
		PartitionSelector *ps = (PartitionSelector *) node->ps.plan;
		Assert(NULL != dynamicTableScanInfo);
		Assert(NULL != dynamicTableScanInfo->memoryContext);
		Assert(NULL != dynamicTableScanInfo->partsMetadata);
		findPartitionMetadataEntry(dynamicTableScanInfo->partsMetadata,
									ps->relid,
									&node->rootPartitionNode,
									&node->accessMethods);
		Assert(NULL != node->rootPartitionNode);
		Assert(NULL != node->accessMethods);
		node->accessMethods->part_cxt = dynamicTableScanInfo->memoryContext;
	}

	/* Join partition elimination */
	if (NULL != outerPlanState(node))
	{
		/* get tuple from outer children */
		PlanState *outerPlan = outerPlanState(node);
		Assert(outerPlan);
		TupleTableSlot *inputSlot = ExecProcNode(outerPlan);

		if (TupIsNull(inputSlot))
		{
			/* no more tuples from outerPlan */
			return NULL;
		}

		/* partition elimination with the given input tuple */
		processLevel(node, 0 /* level */, inputSlot);

		ExprContext *econtext = node->ps.ps_ExprContext;
		ResetExprContext(econtext);
		node->ps.ps_OuterTupleSlot = inputSlot;
		econtext->ecxt_outertuple = inputSlot;
		econtext->ecxt_scantuple = inputSlot;

		ExprDoneCond isDone = ExprSingleResult;
		TupleTableSlot *candidateOutputSlot = ExecProject(node->ps.ps_ProjInfo, &isDone);
		Assert (ExprSingleResult == isDone);

		/* reset acceptedLeafPart */
		node->acceptedLeafPart = NULL;

		return candidateOutputSlot;
	}

	/* Constant partition elimination */
	processLevel(node, 0 /* level */, NULL);

	/* reset acceptedLeafPart */
	node->acceptedLeafPart = NULL;

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecReScanPartitionSelector(node)
 *
 *		ExecReScan routine for PartitionSelector.
 * ----------------------------------------------------------------
 */
void
ExecReScanPartitionSelector(PartitionSelectorState *node, ExprContext *exprCtxt)
{
	/* reset PartitionSelectorState */
	PartitionSelector *ps = (PartitionSelector *) node->ps.plan;
	node->acceptedLeafPart = NULL;
	for(int iter = 0; iter < ps->nLevels; iter++)
	{
		node->levelPartConstraints[iter] = NULL;
	}

	/* free result tuple slot */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * If we are being passed an outer tuple, link it into the "regular"
	 * per-tuple econtext for possible qual eval.
	 */
	if (exprCtxt != NULL)
	{
		ExprContext *stdecontext = node->ps.ps_ExprContext;
		stdecontext->ecxt_outertuple = exprCtxt->ecxt_outertuple;
	}

	/* If the PartitionSelector is in the inner side of a nest loop join,
	 * it should be constant partition elimination and thus has no child node.*/
#if USE_ASSERT_CHECKING
	PlanState  *outerPlan = outerPlanState(node);
	Assert (NULL == outerPlan);
#endif

}

/* ----------------------------------------------------------------
 *		ExecEndPartitionSelector(node)
 *
 *		ExecEnd routine for PartitionSelector. Free resources
 *		and clear tuple.
 *
 * ----------------------------------------------------------------
 */
void
ExecEndPartitionSelector(PartitionSelectorState *node)
{
	ExecFreeExprContext(&node->ps);

	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/* clean child node */
	if (NULL != outerPlanState(node))
	{
		ExecEndNode(outerPlanState(node));
	}

	EndPlanStateGpmonPkt(&node->ps);
}

/* ----------------------------------------------------------------
 *		processLevel
 *
 *		find out satisfied PartOids for the given predicates in the
 *		given partition level
 *
 *		The function is recursively called:
 *		1. If we are in the intermediate level, we register the
 *		satisfied PartOids and continue with the next level
 *		2. If we are in the leaf level, we will propagate satisfied
 *		PartOids.
 *
 *		Input parameters:
 *		node: PartitionSelectorState
 *		level: the current partition level, starting with 0.
 *		inputTuple: input tuple from outer child for join partition
 *		elimination
 *
 * ----------------------------------------------------------------
 */
static void
processLevel(PartitionSelectorState *node, int level, TupleTableSlot *inputTuple)
{
	Assert (NULL != node->ps.plan);
	PartitionSelector *ps = (PartitionSelector *) node->ps.plan;
	Assert (level < ps->nLevels);

	/* get equality and general predicate for the current level */
	Expr *equalityPredicate = (Expr *) lfirst(list_nth_cell(ps->levelEqExpressions, level));
	Expr *generalPredicate = (Expr *) lfirst(list_nth_cell(ps->levelExpressions, level));

	/* get parent PartitionNode if in level 0, it's the root PartitionNode */
	PartitionNode *parentNode = node->rootPartitionNode;
	if (0 != level)
	{
		Assert (NULL != node->levelPartConstraints[level - 1]);
		parentNode = node->levelPartConstraints[level - 1]->pRule->children;
	}

	/* list of PartitionConstraints that satisfied the predicates */
	List *satisfiedPartConstraints = NULL;

	/* If equalityPredicate exists */
	if (NULL != equalityPredicate)
	{
		Assert (NULL == generalPredicate);

		List *partConstraints = partition_constraints_for_equality_predicate(node, level, inputTuple, parentNode);
		satisfiedPartConstraints = list_concat(satisfiedPartConstraints, partConstraints);
	}
	/* If generalPredicate exists */
	else if (NULL != generalPredicate)
	{
		List *partConstraints = partition_constraints_for_general_predicate(node, level, inputTuple, parentNode);
		satisfiedPartConstraints = list_concat(satisfiedPartConstraints, partConstraints);
	}
	/* None of the predicate exists */
	else
	{
		/*
		 * Neither equality predicate nor general predicate
		 * exists. Return all the next level PartitionConstraintss.
		 */
		satisfiedPartConstraints = partition_constraints(parentNode);
	}

	/* Based on the satisfied PartitionRules, go to next
	 * level or propagate PartOids if we are in the leaf level
	 */
	ListCell* lc = NULL;
	foreach (lc, satisfiedPartConstraints)
	{
		PartitionConstraints *partConstraint = (PartitionConstraints *) lfirst(lc);
		node->levelPartConstraints[level] = partConstraint;

		/* If we already in the leaf level */
		if (level == ps->nLevels - 1)
		{
			bool shouldPropagate = true;

			/* if residual predicate exists */
			if (NULL != ps->residualPredicate)
			{
				/* evaluate residualPredicate */
				ExprState *exprstate = node->residualPredicateExprState;
				shouldPropagate = eval_part_qual(exprstate, node, inputTuple);
			}

			if (shouldPropagate  && NULL != ps->propagationExpression)
			{
				partition_propagation(node, partConstraint->pRule->parchildrelid);
				node->acceptedLeafPart = partConstraint;
			}
		}
		/* Recursively call this function for next level's partition elimination */
		else
		{
			processLevel(node, level+1, inputTuple);
		}
	}

	/* After finish iteration, reset this level's PartitionConstraints */
	node->levelPartConstraints[level] = NULL;
}

/* ----------------------------------------------------------------
 *		partition_constraints_for_equality_predicate
 *
 *		Return list of PartitionConstraints for the equality predicate
 *		of current partition level
 *
 * ----------------------------------------------------------------
 */
static List *
partition_constraints_for_equality_predicate(PartitionSelectorState *node, int level,
						TupleTableSlot *inputTuple, PartitionNode *parentNode)
{
	Assert (NULL != node);
	Assert (NULL != node->ps.plan);
	Assert (NULL != parentNode);
	PartitionSelector *ps = (PartitionSelector *) node->ps.plan;
	Assert (level < ps->nLevels);

	/* evaluate equalityPredicate to get partition identifier value */
	ExprState *exprState = (ExprState *) lfirst(list_nth_cell(node->levelEqExprStates, level));

	ExprContext *econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);
	econtext->ecxt_outertuple = inputTuple;
	econtext->ecxt_scantuple = inputTuple;

	bool isNull = false;
	ExprDoneCond isDone = ExprSingleResult;
	Datum value = ExecEvalExpr(exprState, econtext, &isNull, &isDone);

	PartitionRule *partRule = partition_selection(parentNode, node->accessMethods, ps->relid, value, isNull);
	if (NULL != partRule)
	{
		return construct_part_constraints(partRule, parentNode->part->parkind);
	}
	return NIL;
}

/* ----------------------------------------------------------------
 *		partition_constraints_for_general_predicate
 *
 *		Return list of PartitionConstraints for the general predicate
 *		of current partition level
 *
 * ----------------------------------------------------------------
 */
static List *
partition_constraints_for_general_predicate(PartitionSelectorState *node, int level,
						TupleTableSlot *inputTuple, PartitionNode *parentNode)
{
	Assert (NULL != node);
	Assert (NULL != parentNode);

	List *partConstraints = partition_constraints(parentNode);
	List *result = NIL;
	ListCell *lc = NULL;
	foreach (lc, partConstraints)
	{
		PartitionConstraints *constraints = (PartitionConstraints *) lfirst(lc);
		/* We need to register it to allLevelParts to evaluate the current predicate */
		node->levelPartConstraints[level] = constraints;

		/* evaluate generalPredicate */
		ExprState *exprstate = (ExprState *) lfirst(list_nth_cell(node->levelExprStates, level));
		if (eval_part_qual(exprstate, node, inputTuple))
		{
			result = lappend(result, constraints);
		}
	}
	/* reset allLevelPartConstraints */
	node->levelPartConstraints[level] = NULL;

	return result;
}

void
initGpmonPktForPartitionSelector(Plan *planNode, gpmon_packet_t *gpmon_pkt, EState *estate)
{
	Assert(planNode != NULL && gpmon_pkt != NULL && IsA(planNode, PartitionSelector));

	InitPlanNodeGpmonPkt(planNode, gpmon_pkt, estate, PMNT_PartitionSelector,
							(int64)planNode->plan_rows,
							NULL);
}

/* ----------------------------------------------------------------
 *		partition_selection
 *
 *		It finds a child PartitionRule for a given parent partitionNode, which
 *		satisfies with the given partition key value.
 *
 *		If no such a child partitionRule is found, return NULL.
 *
 *		Input parameters:
 *		pn: parent PartitionNode
 *		accessMethods: PartitionAccessMethods
 *		root_oid: root table Oid
 *		value: partition key value
 *
 * ----------------------------------------------------------------
 */
static PartitionRule*
partition_selection(PartitionNode *pn, PartitionAccessMethods *accessMethods, Oid root_oid, Datum value, bool isNull)
{
	Assert (NULL != pn);
	Assert (NULL != accessMethods);
	Partition *part = pn->part;

	Assert (1 == part->parnatts);
	AttrNumber partAttno = part->paratts[0];
	Assert (0 < partAttno);

	Relation rel = relation_open(root_oid, NoLock);
	TupleDesc tupDesc = RelationGetDescr(rel);
	Assert(tupDesc->natts >= partAttno);

	Datum *values = NULL;
	bool *isnull = NULL;
	createValueArrays(partAttno, &values, &isnull);

	isnull[partAttno - 1] = isNull;
	values[partAttno - 1] = value;

	PartitionRule *result = get_next_level_matched_partition(pn, values, isnull, tupDesc, accessMethods);

	freeValueArrays(values, isnull);
	relation_close(rel, NoLock);

	return result;
}

/* ----------------------------------------------------------------
 *		partition_propagation
 *
 *		Propagate a leaf part Oid to the corresponding dynamic scan.
 *
 * ----------------------------------------------------------------
 */
static void
partition_propagation(PartitionSelectorState *node, Oid part_oid)
{
	/* evaluate propagationExpression to get index id */
	ExprState *propagationExprState = node->propagationExprState;

	ExprContext *econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);
	bool isNull = false;
	ExprDoneCond isDone = ExprSingleResult;
	Datum result = ExecEvalExpr(propagationExprState, econtext, &isNull, &isDone);
	int32 index = DatumGetInt32(result);

	InsertPidIntoDynamicTableScanInfo(index, part_oid);
}

/* ----------------------------------------------------------------
 *		partition_constraints
 *
 *		Returns a list of PartitionConstraints of all children PartitionRules
 *		with their constraints for a given parent PartitionNode
 *
 * ----------------------------------------------------------------
 */
static List *
partition_constraints(PartitionNode *pn)
{
	Assert (NULL != pn);
	Partition *part = pn->part;
	List *result = NIL;
	switch(part->parkind)
	{
		case 'r':
			result = partition_constraints_range(pn);
			break;
		case 'l':
			result = partition_constraints_list(pn);
			break;
		default:
			elog(ERROR,"unrecognized partitioning kind '%c'",
				part->parkind);
			break;
	}

	/* add default part if exists */
	if (NULL != pn->default_part)
	{
		PartitionConstraints *constraint = makeNode(PartitionConstraints);
		constraint->pRule = pn->default_part;
		constraint->lowerBound = NULL;
		constraint->upperBound = NULL;
		constraint->lbInclusive = false;
		constraint->upInclusive = false;
		result = lappend(result, constraint);
	}
	return result;
}

/* ----------------------------------------------------------------
 *		partition_constraints_range
 *
 *		Returns a list of PartitionConstraints of all children PartitionRules
 *		with their constraints for a given partition-by-range
 *		PartitionNode
 *
 * ----------------------------------------------------------------
 */
static List *
partition_constraints_range(PartitionNode *pn)
{
	Assert (NULL != pn && 'r' == pn->part->parkind);
	List *result = NIL;
	ListCell *lc;
	foreach (lc, pn->rules)
	{
		PartitionRule *rule = (PartitionRule *) lfirst(lc);
		PartitionConstraints *constraint = construct_part_constraints_range(rule);
		result = lappend(result, constraint);
	}
	return result;
}

/* ----------------------------------------------------------------
 *		partition_constraints_list
 *
 *		Returns a list of PartitionConstraints of all children PartitionRules
 *		with their constraints for a given partition-by-list
 *		PartitionNode
 *
 *		It generates one PartitionConstraints for each partition value in one
 *		PartitionRule
 *
 * ----------------------------------------------------------------
 */
static List *
partition_constraints_list(PartitionNode *pn)
{
	Assert (NULL != pn && 'l' == pn->part->parkind);
	List *result = NIL;
	ListCell *lc = NULL;
	foreach (lc, pn->rules)
	{
		PartitionRule *rule = (PartitionRule *) lfirst(lc);
		result = list_concat(result, construct_part_constraints_list(rule));
	}
	return result;
}

/* ----------------------------------------------------------------
 *		construct_partition_constraints
 *
 *		construct a list of PartitionConstraints node given a PartitionRule
 *		and its partition type
 *
 *		caller is responsible for free the PartitionConstraintss generated
 *
 * ----------------------------------------------------------------
 */
static List *
construct_part_constraints(PartitionRule *rule, char parkind)
{
	List *result = NIL;
	switch(parkind)
	{
		case 'r':
			result = lappend(result, construct_part_constraints_range(rule));
			break;
		case 'l':
			result = construct_part_constraints_list(rule);
			break;
		default:
			elog(ERROR,"unrecognized partitioning kind '%c'", parkind);
			break;
	}
	return result;
}

/* ----------------------------------------------------------------
 *		construct_partition_constraints_range
 *
 *		construct a PartitionConstraints node given a PartitionRule for
 *		partition by range
 *
 *		caller is responsible for free the PartitionConstraints generated
 *
 * ----------------------------------------------------------------
 */
static PartitionConstraints *
construct_part_constraints_range(PartitionRule *rule)
{
	Assert (NULL != rule);
	PartitionConstraints *constraint = makeNode(PartitionConstraints);
	Assert (NULL != constraint);
	constraint->pRule = rule;

	/* retrieve lowerBound information */
	if (NULL == rule->parrangestart)
	{
		constraint->lowerBound = NULL;
		constraint->lbInclusive = false;
	}
	else
	{
		List *parrangeStart = (List *) rule->parrangestart;
		Assert (1 == list_length(parrangeStart));
		Node *lowerBound = (Node *) linitial(parrangeStart);
		Assert (IsA(lowerBound, Const));
		constraint->lowerBound = (Const *) lowerBound;
		constraint->lbInclusive = rule->parrangestartincl;
	}

	/* retrieve upperBound information */
	if (NULL == rule->parrangeend)
	{
		constraint->upperBound = NULL;
		constraint->upInclusive = false;
	}
	else
	{
		List *parrangeEnd = (List *) rule->parrangeend;
		Assert (1 == list_length(parrangeEnd));
		Node *upperBound = (Node *) linitial(parrangeEnd);
		Assert (IsA(upperBound, Const));
		constraint->upperBound = (Const *) upperBound;
		constraint->upInclusive = rule->parrangeendincl;
	}

	return constraint;
}

/* ----------------------------------------------------------------
 *		construct_partition_constraints_list
 *
 *		construct a list of PartitionConstraints node given a PartitionRule
 *		for partition by list
 *
 *		caller is responsible for free the PartitionConstraintss generated
 *
 * ----------------------------------------------------------------
 */
static List *
construct_part_constraints_list(PartitionRule *rule)
{
	List *result = NIL;

	/* default part */
	if (NULL == rule->parlistvalues)
	{
		PartitionConstraints *constraint = makeNode(PartitionConstraints);
		Assert (NULL != constraint);
		constraint->pRule = rule;
		constraint->lowerBound = NULL;
		constraint->upperBound = NULL;
		constraint->lbInclusive = false;
		constraint->upInclusive = false;
		result = lappend(result, constraint);
		return result;
	}

	ListCell *lc = NULL;
	foreach (lc, rule->parlistvalues)
	{
		List *values = (List *) lfirst(lc);
		/* make sure it is single-column partition */
		Assert (1 == list_length(values));
		Node *value = (Node *) lfirst(list_nth_cell(values, 0));
		Assert (IsA(value, Const));

		PartitionConstraints *constraint = makeNode(PartitionConstraints);
		Assert (NULL != constraint);
		constraint->pRule = rule;
		constraint->lowerBound = (Const *) value;
		constraint->lbInclusive = true;
		constraint->upperBound = (Const *) value;
		constraint->upInclusive = true;

		result = lappend(result, constraint);
	}
	return result;
}

/* ----------------------------------------------------------------
 *		eval_part_qual
 *
 *		Evaluate a qualification expression that consists of
 *		PartBoundExpr, PartBoundInclusionExpr
 *
 *		Return true is passed, otherwise false.
 *
 * ----------------------------------------------------------------
 */
static bool
eval_part_qual(ExprState *exprstate, PartitionSelectorState *node, TupleTableSlot *inputTuple)
{
	/* evaluate gneralPredicate */
	ExprContext *econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);
	econtext->ecxt_outertuple = inputTuple;
	econtext->ecxt_scantuple = inputTuple;

	List *qualList = list_make1(exprstate);

	return ExecQual(qualList, econtext, false /* result is not for null */);
}

/* EOF */

