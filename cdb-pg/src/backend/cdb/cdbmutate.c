/*-------------------------------------------------------------------------
 *
 * cdbmutate.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2004-2008, Greenplum inc
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "commands/tablespace.h"
#include "nodes/makefuncs.h"	/* for makeVar() */
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"	/* get_sortgroupclause_tle() */
#include "parser/parsetree.h"	/* for rt_fetch() */
#include "parser/parse_expr.h"	/* for expr_type() */
#include "parser/parse_oper.h"	/* for compatible_oper_opid() */
#include "tcop/utility.h"       /* CreateCommandTag() */
#include "utils/lsyscache.h"	/* for getatttypetypmod() */
#include "utils/relcache.h"     /* RelationGetPartitioningKey() */

#include "cdb/cdbllize.h"
#include "cdb/cdbmutate.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbpullup.h"
#include "cdb/cdbvars.h"

/*
 * An ApplyMotionState holds state for the recursive apply_motion_mutator().
 */
typedef struct ApplyMotionState
{
    PlannedStmt    *plannedStmt;
    int			    nextMotionID;
    int             sliceDepth;
}	ApplyMotionState;


typedef struct shareinput_validator_context
{
	List           *sharedNodes;
} shareinput_validator_context;


/*
 * Forward Declarations
 */
static void assignMotionID(Node *newnode, ApplyMotionState * context, Node *oldnode);

static int *makeDefaultSegIdxArray(int numSegs);

static Motion *
make_motion(Plan       *lefttree,
            bool        sendToSecondary,
            bool        sendSorted,
            int         numInputSegs,
            int         numSortCols,
            AttrNumber *sortColIdx,
            Oid        *sortOperators,
			bool	   *nullsFirst);
	
static Node *apply_motion_mutator(Node *node, ApplyMotionState * context);

static void warn_ctid_without_segid(Plan *plan, List *rangetable);

static bool has_ctid_outside_tlist_walker(Node *node, void* context);

static bool 
shareinput_validator(PlannerGlobal *glob, Plan *plan, List *sharedNodes);
static bool 
shareinput_validator_walker(Node *node, void *context);

#if 0
static bool DoesDistributionHaveDependencies(Oid relOid, GpPolicy * policy);
#endif

/* ------------------------------------------------------------------------- *
 * Function apply_motion() and apply_motion_mutator() add motion nodes to
 * a top-level Plan tree as directed by the Flow nodes in the plan.
 *
 * The result is a deep copy of the argument Plan tree with added Motion
 * nodes.
 *
 * Note: Formerly this function included some logic to move the final query
 * result to the QD or target writer gang, and disallow updating of the
 * distribution key columns.  Now that is done earlier by cdbparallelize().
 * ------------------------------------------------------------------------- *
 */
Plan * 
apply_motion(PlannedStmt *plannedStmt)
{
    Plan               *plan = plannedStmt->planTree;
	Plan	           *result;
    CdbTopPlan         *top;
    ListCell           *cell;
	ApplyMotionState    state;
    int                 itop;
    int                 nSlice;

    /* Initialize mutator context. */
    state.plannedStmt = plannedStmt;
    state.nextMotionID = 1;		/* Start at 1 so zero will mean "unassigned". */
    state.sliceDepth = 0;

	Assert(is_plan_node((Node *) plan));
    Assert(plan->flow && plan->flow->flotype != FLOW_REPLICATED);

    /* 
     * Process the main plan first. 
     */
    result = (Plan *) apply_motion_mutator((Node *)plan, &state);
    Assert(state.sliceDepth == 0);

    /* Note the number of Motion operators in the main plan. */
    top = plannedStmt->topPlans[0];
    Assert(top &&
           IsA(top, CdbTopPlan));
    top->nMotionNodes = state.nextMotionID - 1;

    /* Main plan consists of one root slice plus one slice per Motion node. */
    nSlice = 1 + top->nMotionNodes;

    /* 
     * Process subquery plans. 
     */
    itop = 0;
    foreach(cell, plannedStmt->subplans)
    {
        Plan       *subqueryplan = (Plan *)lfirst(cell);
        int         saveMotionID = state.nextMotionID;

        subqueryplan = (Plan *)apply_motion_mutator((Node *)subqueryplan, &state);
        lfirst(cell) = subqueryplan;
        Assert(state.sliceDepth == 0);

        /* Note the number of Motion operators in the subquery plan. */
        top = plannedStmt->topPlans[++itop];
        Assert(top && IsA(top, CdbTopPlan));
        top->nMotionNodes = state.nextMotionID - saveMotionID;

        /* All subqueries should have been reduced to joins or initplans. */
        if (!top->isInitPlan)
            ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
                            errmsg("%s statement contains a subquery " 
                                   "of a kind not yet supported in this "
                                   "version of " PACKAGE_NAME ".",
                                   CreateCommandTag((Node *)plannedStmt))
                            ));

        /* ctid syscol should only be used in combination with gp_segment_id */
        warn_ctid_without_segid(subqueryplan, plannedStmt->rtable);
    }

    /* Assign root slice numbers to the initplans. */
    for (itop = 1; itop < list_length(plannedStmt->subplans); itop++)
    {
        top = plannedStmt->topPlans[itop];

        if (top->isInitPlan)
        {
            top->qDispSliceId = state.nextMotionID++;
            nSlice += 1 + top->nMotionNodes;
        }
    }

    /* Now we know the total number of slices. */
    plannedStmt->nSlice = nSlice;

	/* Last minute check for Greenplum Database badness. */
	if (plannedStmt->commandType == CMD_DELETE ||
        plannedStmt->commandType == CMD_UPDATE)
	{
        const char *cmd;

		/*
		 * Currently, we only support update or delete involving 1 gang
         * (not counting Init Plans).
		 */
		if (plannedStmt->topPlans[0]->nMotionNodes > 0)
		{
            cmd = CreateCommandTag((Node *)plannedStmt);
			ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
					        errmsg("Cannot parallelize that %s yet", cmd),
                            errdetail("Passage of data from one segment to another "
                                      "is not yet supported during %s operations.",
                                      cmd),
                            errhint("The WHERE condition must specify equality "
                                    "between corresponding DISTRIBUTED BY columns "
                                    "of the target table and all joined tables.")
					));
		}

		if (has_ctid_outside_tlist_walker((Node *)result, NULL))
		{
            cmd = CreateCommandTag((Node *)plannedStmt);
			ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
							errmsg("Cannot parallelize that %s yet", cmd),
							errdetail("Result depends on values of CTID system column.")
							));
		}
	}

	return result;
}                               /* apply_motion */


/*
 * Function apply_motion_mutator() is the workhorse for apply_motion().
 */
Node *
apply_motion_mutator(Node *node, ApplyMotionState * context)
{
	Node   *newnode;
    Plan   *plan;
    Flow   *flow;
    int     numInputSegs;
    int     saveNextMotionID;
    int     saveSliceDepth;
    bool    sendToSecondary = false;    /* always for now */

	if (node == NULL)
		return NULL;
	
    /* Return expression nodes unchanged; don't bother to copy them. */
    if (!is_plan_node(node))
		return node;

    plan = (Plan *)node;
    flow = plan->flow;

    saveNextMotionID = context->nextMotionID;

    /* Descending into a subquery or a new slice? */
    saveSliceDepth = context->sliceDepth;
	Assert( !IsA(node,Query) );
    if (IsA(node, Motion))
        context->sliceDepth++;
    else if (flow && flow->req_move != MOVEMENT_NONE )
        context->sliceDepth++;

	/*
	 * Copy this node and mutate its children. Afterward, this node should be
	 * an exact image of the input node, except that contained nodes requiring
	 * parallelization will have had it applied.
	 */
    newnode = plan_tree_mutator(node, apply_motion_mutator, context);

    context->sliceDepth = saveSliceDepth;

    plan = (Plan *)newnode;
    flow = plan->flow;

    /* Pre-existing Motion nodes must be renumbered. */
    if (IsA(newnode, Motion))
    {
        Motion *motion = (Motion *)newnode;

		/* Sub plan must have flow */
        Assert(flow && motion->plan.lefttree->flow);
		Assert(flow->req_move == MOVEMENT_NONE && !flow->flow_before_req_move);

        /* Assign unique node number to the new node. */
        assignMotionID(newnode, context, node);

        /* If top slice marked as singleton, make it a dispatcher singleton. */
		if(motion->motionType == MOTIONTYPE_FIXED
				&& motion->numOutputSegs == 1 
				&& motion->outputSegIdx[0] >= 0 
				&& context->sliceDepth == 0)
		{
			Assert(flow->flotype == FLOW_SINGLETON &&
					flow->segindex == motion->outputSegIdx[0]);
					flow->segindex = -1;
					motion->outputSegIdx[0] = -1;
		}
        goto done;
    }

    /*
     * If no Flow node, we're in a portion of the tree that was finished by
     * the planner and already contains any needed Motion nodes.
     */
    if (!flow)
        goto done;

    Assert(IsA(flow, Flow));

    /* How many sending processes? */
    numInputSegs = 1;
    if (flow->flow_before_req_move &&
        flow->flow_before_req_move->flotype != FLOW_SINGLETON)
        numInputSegs = getgpsegmentCount();

    /* Add motion atop the copied node, if necessary. */
    switch (flow->req_move)
    {
        case MOVEMENT_FOCUS:
            /* If top slice is a singleton, let it execute on qDisp. */
            if (flow->segindex >= 0 &&
                context->sliceDepth == 0)
                flow->segindex = -1;

            if (flow->numSortCols > 0)
            {
                newnode = (Node *)make_sorted_union_motion(plan,
                                                           sendToSecondary,
                                                           numInputSegs,
                                                           flow->segindex,
                                                           flow->numSortCols,
                                                           flow->sortColIdx,
                                                           flow->sortOperators,
														   flow->nullsFirst);
            }
            else
            {
                newnode = (Node *)make_union_motion(plan,
                                                    sendToSecondary,
                                                    numInputSegs,
                                                    flow->segindex);
            }
            break;

        case MOVEMENT_BROADCAST:
            newnode = (Node *) make_broadcast_motion(plan, sendToSecondary,
                                                     numInputSegs);
            break;

        case MOVEMENT_REPARTITION:
            newnode = (Node *)make_hashed_motion(plan,
                                                 sendToSecondary,
                                                 numInputSegs,
                                                 flow->hashExpr
                                                 );
            break;

        case MOVEMENT_NONE:
            /* Update flow if reassigning singleton top slice to qDisp. */
            if (flow->flotype == FLOW_SINGLETON &&
                flow->segindex >= 0 &&
                context->sliceDepth == 0)
            {
                flow->segindex = -1;
            }
            break;

        default:
            Insist(0);
            break;
    }

    /*
     * After adding Motion node, assign slice id and restore subplan's Flow.
     */
    if (flow->flow_before_req_move)
    {
        Assert(flow->req_move != MOVEMENT_NONE &&
               IsA(newnode, Motion) &&
               plan == ((Motion *)newnode)->plan.lefttree);

        /* Assign unique node number to the new node. */
        assignMotionID(newnode, context, NULL);

        /* Replace the subplan's modified Flow with the original. */
        plan->flow = flow->flow_before_req_move;
    }
    else
        Assert(flow->req_move == MOVEMENT_NONE);

done:
	return newnode;
}                               /* apply_motion_mutator */


/*
 * Helper code for ApplyMotionState -- Assign motion id to new Motion node.
 */
void
assignMotionID(Node *newnode, ApplyMotionState * context, Node *oldnode)
{
	Assert(IsA(newnode, Motion) && context != NULL);
	Assert(oldnode == NULL || (IsA(oldnode, Motion) && oldnode != newnode));

	/* Assign unique node number to the new node. */
	((Motion *) newnode)->motionID = context->nextMotionID++;

	/* Debugging hint that we have reassigned a previously assigned ID. */
	if (oldnode != NULL && ((Motion *) oldnode)->motionID != 0)
	{
		ereport(DEBUG2, (
						 errmsg("Motion node renumbered: old=%d new=%d.",
								((Motion *) oldnode)->motionID,
								((Motion *) newnode)->motionID)
						 ));
	}
}


/* --------------------------------------------------------------------
 * make_motion -- creates a Motion node.
 * Caller must have built the pHashDefn, pFixedDefn,
 * and pSortDefn structs already.
 *
 * This call only make a motion node, without filling in flow info
 * After calling this function, caller need to call add_slice_to_motion
 * --------------------------------------------------------------------
 */
Motion *
make_motion(Plan       *lefttree,
			bool        sendToSecondary,
            bool        sendSorted,
            int         numInputSegs,
			int         numSortCols,
            AttrNumber *sortColIdx,
            Oid        *sortOperators,
			bool	   *nullsFirst)
{
    Motion *node = makeNode(Motion);
    Plan   *plan = &node->plan;

	Assert(lefttree);
	Assert(!IsA(lefttree, Motion));
	AssertImply(sendSorted, (numSortCols > 0 && sortColIdx != NULL && sortOperators != NULL));
	AssertImply(!sendSorted, (numSortCols == 0 && sortColIdx == NULL && sortOperators == NULL));

	plan->startup_cost = lefttree->startup_cost;
	plan->total_cost = lefttree->total_cost;
	plan->plan_rows = lefttree->plan_rows;
	plan->plan_width = lefttree->plan_width;
	plan->targetlist = cdbpullup_targetlist(lefttree,
                        cdbpullup_exprHasSubplanRef((Expr *)lefttree->targetlist));
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->sendToSecondary = sendToSecondary;
	node->sendSorted = sendSorted;

    node->numInputSegs = numInputSegs;
    node->inputSegIdx = NULL;

	node->numSortCols = numSortCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;
	node->nullsFirst = nullsFirst;

	plan->extParam = bms_copy(lefttree->extParam);
	plan->allParam = bms_copy(lefttree->allParam);

	plan->flow = NULL;

	return node;
}

void add_slice_to_motion(Motion *motion,
		MotionType motionType, List *hashExpr,
		int numOutputSegs, int *outputSegIdx)
{
	/* sanity checks */
	/* check numOutputSegs and outputSegIdx are in sync.  */
	AssertEquivalent(numOutputSegs == 0, outputSegIdx == NULL);

	/* numOutputSegs are either 0, for hash or broadcast, or 1, for focus */
	Assert(numOutputSegs == 0 || numOutputSegs == 1);
	AssertImply(motionType == MOTIONTYPE_HASH, numOutputSegs == 0); 
	AssertImply(numOutputSegs == 1, (outputSegIdx[0] == -1 || outputSegIdx[0] == gp_singleton_segindex));

	motion->motionType = motionType;
	motion->hashExpr = hashExpr;
	motion->hashDataTypes = NULL; 
	motion->numOutputSegs = numOutputSegs;
	motion->outputSegIdx = outputSegIdx;
	motion->numHashSegs = 0;
	motion->hashSegIdx = NULL;

	/* Build list of hash key expression data types. */
    if (hashExpr)
    {
        List       *eq = list_make1(makeString("="));
        ListCell   *cell;
        foreach(cell, hashExpr)
        {
            Node   *expr = (Node *)lfirst(cell);
            Oid     typeoid = exprType(expr);
            Oid	    eqopoid;
            Oid     lefttype;
            Oid     righttype;

            /* Get oid of the equality operator for this data type. */
            eqopoid = compatible_oper_opid(eq, typeoid, typeoid, true);
            if ( eqopoid == InvalidOid )
                ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
                                errmsg("no equality operator for typid %d",
                                       typeoid)));

            /* Get the equality operator's operand type. */
            op_input_types( eqopoid, &lefttype, &righttype );
            Assert( lefttype == righttype );

			/* If this type is a domain type, get its base type. */
			if (get_typtype(lefttype) == 'd')
				lefttype = getBaseType(lefttype);

            motion->hashDataTypes = lappend_oid(motion->hashDataTypes, lefttype);
        }
        list_free_deep(eq);
    }

	/* If hashed redistribution, fill in the receiving segment info. */
	if(motion->motionType == MOTIONTYPE_HASH)
	{
		motion->numHashSegs = getgpsegmentCount();
		motion->hashSegIdx = makeDefaultSegIdxArray(getgpsegmentCount());
	}

	/* Attach a descriptive Flow. */
	switch(motion->motionType)
	{
		case MOTIONTYPE_HASH:
			motion->plan.flow = makeFlow(FLOW_PARTITIONED);
			motion->plan.flow->locustype = CdbLocusType_Hashed;
			motion->plan.flow->hashExpr = copyObject(motion->hashExpr);
			break;
		case MOTIONTYPE_FIXED:
			if (motion->numOutputSegs == 0 )
			{
				/* broadcast */
				motion->plan.flow = makeFlow(FLOW_REPLICATED);
				motion->plan.flow->locustype = CdbLocusType_Replicated;
			}
			else if (motion->numOutputSegs == 1 )
			{
				/* Focus motion */
				motion->plan.flow = makeFlow(FLOW_SINGLETON);
				motion->plan.flow->segindex = motion->outputSegIdx[0];
				motion->plan.flow->locustype = ( motion->plan.flow->segindex<0 ) ?
					CdbLocusType_Entry :
					CdbLocusType_SingleQE;
			}
			else
			{
				Assert(!"Invalid numoutputSegs for motion type fixed");
				motion->plan.flow = NULL;
			}
			break;
		default:
			Assert(!"Invalide motion type");
			motion->plan.flow = NULL;
			break;
	}

	Assert(motion->plan.flow);

	/* if the motion has a sort order, preserve it in each route */
	if(motion->sendSorted && motion->numSortCols > 0)
	{
		int i, n;
		n = motion->numSortCols;
		motion->plan.flow->numSortCols = n;
		motion->plan.flow->sortColIdx = palloc(n * sizeof(AttrNumber));
		motion->plan.flow->sortOperators = palloc (n * sizeof(Oid));
		motion->plan.flow->nullsFirst = palloc (n * sizeof(bool));
		for ( i = 0; i < n; i++ )
		{
			motion->plan.flow->sortColIdx[i] = motion->sortColIdx[i];
			motion->plan.flow->sortOperators[i] = motion->sortOperators[i];
			motion->plan.flow->nullsFirst[i] = motion->nullsFirst[i];
		}
	}
}

Motion *
make_union_motion(Plan *lefttree, bool sendToSecondary,
                  int numInputSegs, int destSegIndex)
{
	Motion 	*motion;
	int		*outSegIdx = (int *) palloc(sizeof(int)); 
	outSegIdx[0] = destSegIndex; 

	motion = make_motion(lefttree, sendToSecondary, false, numInputSegs,
			0, NULL, /* numSortCols, sortColIdx, */
			NULL, NULL /* sortOperators, nullsFirst */
			);
	add_slice_to_motion(motion, MOTIONTYPE_FIXED, 
			NULL, 1, outSegIdx);
	return motion;
}

Motion *
make_sorted_union_motion(Plan *lefttree, bool sendToSecondary,
                         int numInputSegs, int destSegIndex,
						 int numSortCols, AttrNumber *sortColIdx, 
						 Oid *sortOperators, bool *nullsFirst)
{
	Motion 	*motion;
	int		*outSegIdx = (int *) palloc(sizeof(int)); 
	outSegIdx[0] = destSegIndex; 

	motion = make_motion(lefttree, sendToSecondary, true, numInputSegs,
			numSortCols, sortColIdx, sortOperators, nullsFirst);

	add_slice_to_motion(motion, MOTIONTYPE_FIXED,
			NULL, 1, outSegIdx); 
	return motion;
}

Motion *
make_hashed_motion(Plan *lefttree, bool sendToSecondary, int numInputSegs,
				   List *hashExpr)
{
	Motion *motion;

	motion = make_motion(lefttree, sendToSecondary, false, numInputSegs,
			0, NULL, NULL, NULL);
	add_slice_to_motion(motion, MOTIONTYPE_HASH, 
			hashExpr,
			0, NULL);
	return motion;
}

Motion *
make_broadcast_motion(Plan *lefttree, bool sendToSecondary, int numInputSegs)
{
	Motion *motion;

	motion = make_motion(lefttree, sendToSecondary, false, numInputSegs,
			0, NULL, NULL, NULL);

	add_slice_to_motion(motion, MOTIONTYPE_FIXED,
			NULL, 0, NULL);
	return motion;
}

/* --------------------------------------------------------------------
 * makeDefaultSegIdxArray -- creates an int array with numSegs elements,
 * with values between 0 and numSegs-1
 * --------------------------------------------------------------------
 */
int *
makeDefaultSegIdxArray(int numSegs)
{
	int		   *segIdx = palloc(numSegs * sizeof(int));

	/*
	 * The following assumes that the default is to have segindexes
	 * numbered sequentially starting at 0.
	 */
	int			i;

	for (i = 0; i < numSegs; i++)
		segIdx[i] = i;

	return segIdx;
}


/* ----------------------------------------------------------------------- *
 * Functions hasCtid(), hasCtid_outside_tlist() and helper function
 * ctid_inventory_walker() search a plan tree for a use of the CTID
 * pseudo-attribute.
 *
 * warn_ctid_without_segid() warns the user if the plan refers to a
 * partitioned table's ctid column without also referencing its
 * gp_segment_id column.
 *
 * hasCtid_outside_tlist() detects a CTID reference outside of simple
 * Var in the targetlist of the top-level plan node.  The use of this
 * function is pretty much restricted to the case of a UPDATE or DELETE
 * in Greenplum Database first release.
 * ----------------------------------------------------------------------- *
 */
typedef struct ctid_inv_data
{
	Relids      ctid_relids;
    Relids      segid_relids;
}	ctid_inv_data;

static bool ctid_inventory_walker(Node *node, ctid_inv_data * inv);

static void 
warn_ctid_without_segid(Plan *plan, List *rangetable)
{
	ctid_inv_data   invdata;
    Relids          warn_relids;
    RangeTblEntry  *rte;
    int             relid;
	
    invdata.ctid_relids = NULL;
	invdata.segid_relids = NULL;

	ctid_inventory_walker((Node *) plan, &invdata);

    if (invdata.ctid_relids == NULL)
        return;

    warn_relids = bms_difference(invdata.ctid_relids, invdata.segid_relids);

    bms_foreach(relid, warn_relids)
    {
	    rte = rt_fetch(relid, rangetable);
        if (rte->rtekind == RTE_RELATION)
        {
            Relation    relation;
            bool        isPartitioned;

            relation = relation_open(rte->relid, NoLock);
            isPartitioned = RelationIsDistributed(relation);
            relation_close(relation, NoLock);

            if (isPartitioned)
            {
                /* Get name from catalog, not relcache, in case it changed. */
                char   *relname = get_rel_name(rte->relid);

                ereport(NOTICE,
                        (errmsg("Query uses system column \"%s.ctid\"",
                                relname),
                         errdetail("To uniquely identify a row within a "
                                   "distributed table, use the "
                                   "\"gp_segment_id\" system column "
                                   "together with the \"ctid\" column.")
                         ));
                if (relname)
                    pfree(relname);
            }
        }
    }

    bms_free(warn_relids);
    bms_free(invdata.ctid_relids);
    bms_free(invdata.segid_relids);
}                               /* warn_ctid_without_segid */

bool
ctid_inventory_walker(Node *node, ctid_inv_data * inv)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
        {
            Var    *var = (Var *)node;

			switch (var->varattno)
            {
                case SelfItemPointerAttributeNumber:    /* ctid */
				    inv->ctid_relids = bms_add_member(inv->ctid_relids, var->varno);
                    break;
                case gpsegmentIdAttributeNumber:       /* gp_segment_id */
				    inv->segid_relids = bms_add_member(inv->segid_relids, var->varno);
                    break;
                default:
                    break;
            }
			/* Var is primitive, no need to recur, but keep walking. */
			return false;
        }
		default:
			/* Let plan_tree_walker() deal with other node types. */
			break;
	}
	return plan_tree_walker(node, ctid_inventory_walker, inv);
}

bool
has_ctid_outside_tlist_walker(Node *node, void* context)
{
    UnusedArg(context);

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_TargetEntry:
            /* Ignore Var nodes at top level of target list. */
            if (IsA(((TargetEntry *)node)->expr, Var))
                return false;
            break;

		case T_Var:
			/* Return true if found CTID outside target list. */
			if (((Var *) node)->varoattno == SelfItemPointerAttributeNumber)
                return true;
			return false;

		default:
			/* Let plan_tree_walker() deal with other node types. */
			break;
	}
	return plan_tree_walker(node, has_ctid_outside_tlist_walker, NULL);
}                               /* has_ctid_outside_tlist_walker */

#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/dependency.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

#if 0
bool
DoesDistributionHaveDependencies(Oid relOid, GpPolicy * policy)
{

	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;
	AttrNumber  attnum;
	int			ia;

	if (Gp_role != GP_ROLE_DISPATCH || policy == NULL)
		return false;
	
	/*
	 * Find everything that depends on the column (constraints, indexes, etc)
	 * See if we have any UNIQUE indexes, or UNIQUE or PRIMARY KEY constraints
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);
	
	for (ia=0;ia < policy->nattrs; ia++)
	{
		attnum = policy->attrs[ia];

		ScanKeyInit(&key[0],
					Anum_pg_depend_refclassid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_depend_refobjid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relOid));
		ScanKeyInit(&key[2],
					Anum_pg_depend_refobjsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum((int32) attnum));
	
		scan = systable_beginscan(depRel, DependReferenceIndexId, true,
								  SnapshotNow, 3, key);
	
		while (HeapTupleIsValid(depTup = systable_getnext(scan)))
		{
			Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
			ObjectAddress foundObject;
	
			/* We don't expect any PIN dependencies on columns */
			if (foundDep->deptype == DEPENDENCY_PIN)
				return true;
	
			foundObject.classId = foundDep->classid;
			foundObject.objectId = foundDep->objid;
			foundObject.objectSubId = foundDep->objsubid;
	
			switch (getObjectClass(&foundObject))
			{
				case OCLASS_CLASS:
					{
						char		relKind = get_rel_relkind(foundObject.objectId);
	
						if (relKind == RELKIND_INDEX)
						{
	
							char * indexdefstring = pg_get_indexdef_string(foundObject.objectId);
						
							if (indexdefstring &&strstr(indexdefstring," UNIQUE ")!=0)
							{
								systable_endscan(scan);
	
								heap_close(depRel, RowExclusiveLock);
								return true;
							}
							
						}
						else if (relKind == RELKIND_SEQUENCE)
						{
							/*
							 * This must be a SERIAL column's sequence.  We need
							 * not do anything to it.
							 */
							Assert(foundObject.objectSubId == 0);
						}
						else
						{
							/* Not expecting any other direct dependencies... */
							elog(ERROR, "unexpected object depending on column: %s",
								 getObjectDescription(&foundObject));
						}
						break;
					}
	
				case OCLASS_CONSTRAINT:
					{
						char *defstring = pg_get_constraintdef_string(foundObject.objectId);
		
						if (strstr(defstring," UNIQUE")!=0 ||
							strstr(defstring,"PRIMARY KEY")!=0 )
						{
							systable_endscan(scan);
		
							heap_close(depRel, RowExclusiveLock);
							return true;
						}
					}
	
					break;
	
				case OCLASS_REWRITE:
				case OCLASS_DEFAULT:
				case OCLASS_PROC:
				case OCLASS_TYPE:
				case OCLASS_CAST:
				case OCLASS_CONVERSION:
				case OCLASS_LANGUAGE:
				case OCLASS_OPERATOR:
				case OCLASS_OPCLASS:
				case OCLASS_TRIGGER:
				case OCLASS_SCHEMA:
					break;
	
				default:
					elog(ERROR, "unrecognized object class: %u",
						 foundObject.classId);
			}
		}
	
		systable_endscan(scan);
	}

	heap_close(depRel, RowExclusiveLock);

	return false;
}
#endif 

/*
 * Code that mutate the tree for share input
 *
 * After the planner, the plan is really a DAG.  SISCs will have valid
 * pointer to the underlying share.  However, other code (setrefs etc) 
 * depends on the fact that the plan is a tree.  We first mutate the DAG
 * to a tree.  
 * 
 * Next, we will need to decide if the share is cross slices.  If the share 
 * is not cross slice, we do not need the syncrhonization, and it is possible to 
 * keep the Material/Sort in memory to save a sort.
 *
 * It is essential that we walk the tree in the same order as the ExecProcNode start
 * execution, otherwise, deadlock may rise.
 */

/* Walk the tree for shareinput.
 * Shareinput fix shared_as_id and underlying_share_id of nodes in place.  We do not want to use
 * the ordinary tree walker as it is unnecessary to make copies etc.
 */
typedef bool (*SHAREINPUT_MUTATOR) (Node *node, ApplyShareInputContext *ctxt, bool fPop);
static void shareinput_walker(SHAREINPUT_MUTATOR f, Node *node, ApplyShareInputContext *ctxt)
{
	Plan *plan = NULL; 
	bool recursive_down;

	if(node == NULL)
		return;

    // TODO: Would it be possible to eliminate this case and instead 
    // require the top-level caller to loop over the glob->subplans list?
    // That would remove the possibility of inadvertently walking the same
    // subquery plan twice in case the SubPlan node happens to be duplicated
    // by some tree transformation.  
	if(IsA(node, SubPlan))
	{
		SubPlan    *subplan = (SubPlan *) node;
        Plan       *subqueryplan = exec_subplan_get_plan(ctxt->glob, subplan);

		shareinput_walker(f, (Node *)subqueryplan, ctxt); 
		return;
	}

	if(IsA(node, List))
	{
		List *l = (List *) node;
		ListCell *lc;
		foreach(lc, l)
		{
			Node* n = lfirst(lc);
			shareinput_walker(f, n, ctxt);
		}
		return;
	}

	if(!is_plan_node(node))
		return;

	plan = (Plan *) node;
	recursive_down = (*f)(node, ctxt, false);

	if(recursive_down) 
	{
		if(IsA(node, Append))
		{
			ListCell *cell;
			Append *app = (Append *) node;
			foreach(cell, app->appendplans)
				shareinput_walker(f, (Node *)lfirst(cell), ctxt);
		}

		else if(IsA(node, SubqueryScan))
		{
			SubqueryScan *subqscan = (SubqueryScan *) node;
			shareinput_walker(f, (Node *) subqscan->subplan, ctxt);
		}

		else if(IsA(node, NestLoop))
		{
			/* Nest loop join is strange.  Exec order depends on prefetch_inner */
			NestLoop *nl = (NestLoop *) node;
			if(nl->join.prefetch_inner)
			{
				shareinput_walker(f, (Node *)plan->righttree, ctxt);
				shareinput_walker(f, (Node *)plan->lefttree, ctxt);
			}
			else	
			{
				shareinput_walker(f, (Node *)plan->lefttree, ctxt);
				shareinput_walker(f, (Node *)plan->righttree, ctxt);
			}
		}

		else if(IsA(node, HashJoin))
		{
			/* Hash join the hash table is at inner */
			shareinput_walker(f, (Node *)plan->righttree, ctxt);
			shareinput_walker(f, (Node *)plan->lefttree, ctxt);
		}

		else
		{
			shareinput_walker(f, (Node *)plan->lefttree, ctxt);
			shareinput_walker(f, (Node *)plan->righttree, ctxt);
			shareinput_walker(f, (Node *)plan->initPlan, ctxt);
		}
	}

	(*f)(node, ctxt, true);
}

/* 
 * DAG to Tree
 * Zap children if I am not the first sharer (not recursive down).  
 * Assign share_id to both ShareInputScan and Material/Sort.
 */
static bool shareinput_mutator_dag_to_tree(Node *node, ApplyShareInputContext* ctxt, bool fPop)
{
	Plan *plan = (Plan *) node;

	if(fPop)
		return true;

	if(IsA(plan, ShareInputScan))
	{
		Plan *subplan = plan->lefttree;

		Assert(subplan);
		Assert(IsA(subplan, Material) || IsA(subplan, Sort));
		Assert(get_plan_share_id(plan) == SHARE_ID_NOT_ASSIGNED);
		Assert(plan->righttree == NULL);
		Assert(plan->initPlan == NULL);

		if(list_member_ptr(ctxt->sharedNodes, subplan))
		{
			Assert(get_plan_share_id(subplan) >= 0);
			set_plan_share_id(plan, get_plan_share_id(subplan));
			plan->lefttree = NULL;
			return false;
		}
		else
		{
			Assert(get_plan_share_id(subplan) == SHARE_ID_NOT_ASSIGNED);
			set_plan_share_id(subplan, list_length(ctxt->sharedNodes));
			set_plan_share_id(plan, get_plan_share_id(subplan));
			ctxt->sharedNodes = lappend(ctxt->sharedNodes, subplan);
			return true;
		}
	}

	return true;
}

/* 
 * Apply the share input mutator.
 */
Plan *apply_shareinput_dag_to_tree(Plan *plan, ApplyShareInputContext *ctxt)
{
    shareinput_walker(shareinput_mutator_dag_to_tree, (Node *) plan, ctxt);
    
    Assert(shareinput_validator(ctxt->glob, plan, ctxt->sharedNodes));
    return plan;
}

/* Some helper: implements a stack using List. */
static void shareinput_pushmot(ApplyShareInputContext *ctxt, int motid)
{
	ctxt->motStack = lcons_int(motid, ctxt->motStack);
}
static void shareinput_popmot(ApplyShareInputContext *ctxt)
{
	list_delete_first(ctxt->motStack);
}
static int shareinput_peekmot(ApplyShareInputContext *ctxt)
{
	return linitial_int(ctxt->motStack);
}

typedef struct ShareNodeWithSliceMark
{
	Plan *plan;
	int slice_mark;
} ShareNodeWithSliceMark;

static bool shareinput_find_sharenode(ApplyShareInputContext *ctxt, int share_id, ShareNodeWithSliceMark *result)
{
	ListCell *lc;
	ListCell *lc_slicemark;
	
	if(ctxt->sharedNodes == NULL)
		return false;

	forboth(lc, ctxt->sharedNodes, lc_slicemark, ctxt->sliceMarks)
	{
		Plan *plan = (Plan *) lfirst(lc);

		Assert(IsA(plan, Material) || IsA(plan, Sort));
		if(get_plan_share_id(plan) == share_id)
		{
			if(result)
			{
				result->plan = plan;
				result->slice_mark = lfirst_int(lc_slicemark);
			}
			return true;
		}
	}

	return false;
}

/* 
 * First walk on shareinput xslice.  It does the following,
 *  	1. build the sharedNodes in context.
 *  	2. Build teh sliceMask in context.
 *  	3. If a shared is cross slice, mark the shared node xslice.
 *  	4. Build a list a share on QD
 */
static bool shareinput_mutator_xslice_1(Node* node, ApplyShareInputContext *ctxt, bool fPop)
{
	Plan *plan = (Plan *) node;

	if(fPop)
	{
		if(IsA(plan, Motion))
			shareinput_popmot(ctxt);
		return false;
	}

	if(IsA(plan, Motion))
	{
		Motion *motion = (Motion *) plan;
		shareinput_pushmot(ctxt, motion->motionID);
		return true;
	}

	if(IsA(plan, ShareInputScan))
	{
		ShareInputScan *sisc = (ShareInputScan *) plan;
		int motId = shareinput_peekmot(ctxt);
		Plan *shared = plan->lefttree;
		
		Assert(sisc->plan.flow);
		if(sisc->plan.flow->flotype == FLOW_SINGLETON)
		{
			if(sisc->plan.flow->segindex < 0) 
				ctxt->qdShares = list_append_unique_int(ctxt->qdShares, sisc->share_id);
		}

		if(!shared)
		{
			ShareNodeWithSliceMark plan_slicemark = {NULL /* plan */,0 /* slice_mark */};
			int  shareSliceId = 0;
			
			shareinput_find_sharenode(ctxt, sisc->share_id, &plan_slicemark);
			shareSliceId = get_plan_driver_slice(plan_slicemark.plan);

			if(shareSliceId != motId)
			{
				ShareType stype = get_plan_share_type(plan_slicemark.plan);
				if(stype == SHARE_MATERIAL || stype == SHARE_SORT)
					set_plan_share_type_xslice(plan_slicemark.plan);

				incr_plan_nsharer_xslice(plan_slicemark.plan);
				sisc->driver_slice = motId;
			}
		}
		else
		{
			Assert(shareinput_find_sharenode(ctxt, sisc->share_id, NULL) == false);
			Assert(get_plan_share_id(plan) == get_plan_share_id(shared));
			set_plan_driver_slice(shared, motId);
		
			ctxt->sharedNodes = lappend(ctxt->sharedNodes, shared);
			ctxt->sliceMarks = lappend_int(ctxt->sliceMarks, motId); 
		}
	}
				
	return true;
}

/* 
 * Second pass
 * 	1. Mark shareinput scan xslice,
 * 	2. Bulid a list of QD slices
 */
static bool shareinput_mutator_xslice_2(Node *node, ApplyShareInputContext *ctxt, bool fPop)
{
	Plan *plan = (Plan *) node;

	if(fPop)
	{
		if(IsA(plan, Motion))
			shareinput_popmot(ctxt);
		return false;
	}

	if(IsA(plan, Motion))
	{
		Motion *motion = (Motion *) plan;
		shareinput_pushmot(ctxt, motion->motionID);
		return true;
	}

	if(IsA(plan, ShareInputScan))
	{
		ShareInputScan *sisc = (ShareInputScan *) plan;
		int motId = shareinput_peekmot(ctxt);

		ShareNodeWithSliceMark plan_slicemark = { NULL, 0 };
		ShareType stype = SHARE_NOTSHARED;

		shareinput_find_sharenode(ctxt, sisc->share_id, &plan_slicemark);
		stype = get_plan_share_type(plan_slicemark.plan);

		switch(stype)
		{
			case SHARE_MATERIAL_XSLICE:
				Assert(sisc->share_type == SHARE_MATERIAL);
				set_plan_share_type_xslice(plan);
				break;
			case SHARE_SORT_XSLICE:
				Assert(sisc->share_type == SHARE_SORT);
				set_plan_share_type_xslice(plan);
				break;
			default:
				Assert(sisc->share_type == stype);
				break;
		}

		if(list_member_int(ctxt->qdShares, sisc->share_id))
		{
			Assert(sisc->plan.flow);
			Assert(sisc->plan.flow->flotype == FLOW_SINGLETON);
			ctxt->qdSlices = list_append_unique_int(ctxt->qdSlices, motId);
		}
	}
	return true;
}

/* 
 * The third pass.  If a shareinput is running on QD, then all slices in this share 
 * must be on QD.  Move them to QD.
 */

static bool shareinput_mutator_xslice_3(Node *node, ApplyShareInputContext *ctxt, bool fPop)
{
	Plan *plan = (Plan *) node;
	int motId = shareinput_peekmot(ctxt);

	if(fPop)
	{
		if(IsA(plan, Motion))
			shareinput_popmot(ctxt);
		return false;
	}

	if(IsA(plan, Motion))
	{
		Motion *motion = (Motion *) plan;
		shareinput_pushmot(ctxt, motion->motionID);
		/* Do not return.  Motion need to be adjusted as well */
	}

	/* Well, the following test can be optimized if we record the test
	 * result so we test just once for all node in one slice.  But this
	 * code is not perf critical so be lazy.
	 */
	if(list_member_int(ctxt->qdSlices, motId))
	{
		if(plan->flow)
		{
			Assert(plan->flow->flotype == FLOW_SINGLETON);
			plan->flow->segindex = -1;
		}
	}
	return true;
}

Plan *apply_shareinput_xslice(Plan *plan, ApplyShareInputContext *ctxt) 
{
	ctxt->sharedNodes = NULL;
	ctxt->sliceMarks = NULL;
	ctxt->motStack = NULL;
	ctxt->qdShares = NULL;
	ctxt->qdSlices = NULL;

	shareinput_pushmot(ctxt, 0);
	
	/* 
	 * Walk the tree.  See comment for each pass for what each pass will do.
	 * Note: We depends on the contxt of between the passes, so do not reset 
	 * the context between calls.
	 */
	shareinput_walker(shareinput_mutator_xslice_1, (Node *) plan, ctxt); 
	shareinput_walker(shareinput_mutator_xslice_2, (Node *) plan, ctxt); 
	shareinput_walker(shareinput_mutator_xslice_3, (Node *) plan, ctxt); 
	
	return plan;
}


/*
 * shareinput_validator
 *
 * Checks whether the DAG-to-tree conversion appears to have been 
 * performed correctly.  Returns true or does not return.
 *
 * This was formerly done by set_plan_refs().
 */
static bool 
shareinput_validator(PlannerGlobal *glob, Plan *plan, List *sharedNodes)
{
    ListCell                       *cell;
    shareinput_validator_context    context;

    context.sharedNodes = sharedNodes;
    
    shareinput_validator_walker((Node *)plan, &context);
    foreach(cell, glob->subplans)
        shareinput_validator_walker((Node *)lfirst(cell), &context);

    return true;
}                               /* shareinput_validator */

static bool 
shareinput_validator_walker(Node *node, void *context)
{
    Plan                           *plan = (Plan *)node;
    shareinput_validator_context   *ctx = (shareinput_validator_context *)context;

    if (node == NULL)
    	return false;

    switch (nodeTag(plan))
    {
		case T_ShareInputScan:
			{
				ShareInputScan *sisc = (ShareInputScan *) plan;
				Plan *childPlan = plan->lefttree;

				if(childPlan == NULL)
				{
					Assert(sisc->share_type != SHARE_NOTSHARED 
							&& sisc->share_id >= 0 
							&& ctx->sharedNodes); 
					childPlan = list_nth(ctx->sharedNodes, sisc->share_id);
				}

                Assert(childPlan && ( IsA(childPlan,Material) || IsA(childPlan, Sort)));
				if(IsA(childPlan, Material))
				{
					Material *shared = (Material *) childPlan;
					Assert(shared->share_type != SHARE_NOTSHARED
							&& shared->share_id == sisc->share_id);
				}
				else
				{
					Sort *shared = (Sort *) childPlan;
					Assert(shared->share_type != SHARE_NOTSHARED
							&& shared->share_id == sisc->share_id);
				}
			}
			break;

		default:
	        break;
	}   

    return plan_tree_walker(node, shareinput_validator_walker, ctx);
}                               /* shareinput_validator_walker */

