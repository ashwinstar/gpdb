/*-------------------------------------------------------------------------
 *
 * cdbmutate.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2004-2006, Greenplum
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/clauses.h"
#include "parser/parsetree.h"	/* for rt_fetch() */
#include "utils/lsyscache.h"	/* for getatttypetypmod() */
#include "nodes/makefuncs.h"	/* for makeVar() */
#include "parser/parse_expr.h"	/* for expr_type() */
#include "parser/parse_oper.h"	/* for compatible_oper_opid() */
#include "utils/relcache.h"     /* RelationGetPartitioningKey() */
#include "optimizer/tlist.h"	/* get_sortgroupclause_tle() */

#include "commands/tablespace.h"
#include "catalog/catalog.h"

#include "cdb/cdbfts.h"         /* isFTSEnabled() */
#include "cdb/cdbllize.h"
#include "cdb/cdbmutate.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbpullup.h"
#include "cdb/cdbvars.h"

/*
 * An ApplyMotionState holds state for the recursive apply_motion_mutator().
 * It is externalized here to make it shareable by helper code in other
 * modules, e.g., cdbaggmutate.c.
 */
typedef struct ApplyMotionState
{
	int			nextMotionID;
    int         sliceDepth;
    int         numInitPlanMotionNodes;
    List       *initPlans;
}	ApplyMotionState;


/*
 * Forward Declarations
 */
static void assignMotionID(Node *newnode, ApplyMotionState * context, Node *oldnode);

static List *
getExprListFromTargetList(List         *tlist,
                          int           numCols,
                          AttrNumber   *colIdx,
                          bool          useExecutorVarFormat);

static int *makeDefaultSegIdxArray(int numSegs);

static Motion *
make_motion(Plan       *lefttree,
            MotionType  motionType,
            bool        sendToSecondary,
            bool        sendSorted,
            int         numInputSegs,
            List       *hashExpr,
            int         numOutputSegs,
            int        *outputSegIdx,
            int         numSortCols,
            AttrNumber *sortColIdx,
            Oid        *sortOperators);
			
static Node *apply_motion_mutator(Node *node, ApplyMotionState * context);

static void validateMotionNodeParms(MotionType motionType,
						bool sendToSecondary, bool sendSorted,
						List *hashExpr,
						int numOutputSegs, int *outputSegIdx,
				int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators);
static bool motion_expr_walker(Node *node, char *not_used);

static bool
doesUpdateAffectPartitionCols(Plan         *plan,
                              Query        *query,
                              int           nattrs,
                              AttrNumber   *attrs);
static void
warn_ctid_without_segid(Query *query, Plan *plan);

static bool has_ctid_outside_tlist_walker(Node *node, void* unused);


/* ------------------------------------------------------------------------- *
 * Function apply_motion() and apply_motion_mutator() add motion nodes to
 * a top-level Plan tree as directed by the Flow nodes in the plan.
 *
 * In addition, if the argument Plan produces a partitioned flow, a Motion
 * appropriate to the command is added to the top. For example, to consolidate
 * the tuple streams from a SELECT command into a single tuple stream on the
 * dispatcher.
 *
 * The result is a deep copy of the argument Plan tree with added Motion
 * nodes.
 *
 * TODO Maybe add more context to argument list!
 * ------------------------------------------------------------------------- *
 */
Plan *
apply_motion(Plan *plan, Query *query)
{
	Plan	           *result;
    ListCell           *cell;
    MppPolicy          *targetPolicy = NULL;
    MppPolicyType       targetPolicyType = POLICYTYPE_ENTRY;
	ApplyMotionState    state;

    /* Initialize mutator context. */
    state.nextMotionID = 1;		/* Start at 1 so zero will mean "unassigned". */
    state.sliceDepth = 0;
    state.numInitPlanMotionNodes = 0;
    state.initPlans = NIL;

	Assert(is_plan_node((Node *) plan) && IsA(query, Query));
    Assert(plan->flow && plan->flow->flotype != FLOW_REPLICATED);

	/* Does query have a target relation?  (INSERT/DELETE/UPDATE) */
    if (query->resultRelation > 0)
    {
	    RangeTblEntry  *rte;
        Relation        relation;

        rte = rt_fetch(query->resultRelation, query->rtable);
	    Assert(rte->rtekind == RTE_RELATION);

        /* Get a copy of the rel's MppPolicy from the relcache. */
        relation = relation_open(rte->relid, NoLock);
        targetPolicy = RelationGetPartitioningKey(relation);
        relation_close(relation, NoLock);

        if (targetPolicy)
            targetPolicyType = targetPolicy->ptype;
    }

	switch (query->commandType)
	{
		case CMD_SELECT:
			if (plan->flow->flotype == FLOW_PARTITIONED)
			{

				if (query->into)
				{
					List   *hashExpr;
					ListCell *exp1;
					int			maxattrs = 200;
					
					if (query->intoPolicy != NULL)
						targetPolicy = query->intoPolicy;
					else
					{
						/* User did not specify a DISTRIBUTED BY clause */
					
						targetPolicy = palloc0(sizeof(MppPolicy)- sizeof(targetPolicy->attrs) + maxattrs * sizeof(targetPolicy->attrs[0]));
						targetPolicy->nattrs = 0;
						
						targetPolicy->ptype = POLICYTYPE_PARTITIONED;
						
						/* Find out what the flow is partitioned on */
						hashExpr = plan->flow->hashExpr;
						
						if(hashExpr)
						foreach(exp1, hashExpr)
						{
							AttrNumber	n;
							/* See if this Expr is a column of the result table */
							
							for(n=1;n<=list_length(plan->targetlist);n++)
							{
								Var		   *new_var = NULL;
								
								TargetEntry *target = get_tle_by_resno(plan->targetlist, n);
								
								/* If subplan expr is a Var, copy to preserve its EXPLAIN info. */
					            if (IsA(target->expr, Var))
					            {
					                new_var = copyObject(target->expr);
					                new_var->varno = OUTER;
					                new_var->varattno = n;
					            }
					
					            /*
						         * Make a Var that references the target list entry at this offset,
						         * using OUTER as the varno
						         */
					            else
							        new_var = makeVar(OUTER,
					                                  n,
					                                  exprType((Node *) target->expr),
					                                  exprTypmod((Node *) target->expr),
					                                  0);
					
								if (equal(lfirst(exp1),new_var))
								{
									/* If it is, use it to partition the result table, to avoid
									 * unnecessary redistibution of data */
									
									targetPolicy->attrs[targetPolicy->nattrs++] = n;
									break;
								}
								
								
							}
						}
	
						/* do we know how to partition? */
						if (targetPolicy->nattrs == 0)
						{
							/* If not, Distribute on column 1 */
							targetPolicy->attrs[targetPolicy->nattrs++] = 1;
						}
						
						if (query->intoPolicy == NULL)
						{
							char columns[255];
							int i;
							columns[0] = '\0';
							for (i=0;i<targetPolicy->nattrs;i++)
							{
								TargetEntry *target = get_tle_by_resno(plan->targetlist, targetPolicy->attrs[i]);
								if (i>0)
								strcat(columns, ", ");
								strcat(columns, target->resname);
							
							}
						
							ereport(NOTICE,
									(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
									 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column(s) "
										    "named '%s' as the Bizgres MPP data distribution key for this "
										    "table. ", columns),
								     errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
								     		 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
						}
					}
					
					targetPolicyType = targetPolicy->ptype;
					
					query->intoPolicy = targetPolicy;
					
					/* This probably should be done somewhere better */
					if (!OidIsValid(query->intoOid))
					{
						Relation	pg_class_desc;

						Oid reltablespace = GetDefaultTablespace();
					
						pg_class_desc = heap_open(RelationRelationId, RowExclusiveLock);
											
						query->intoOid = GetNewRelFileNode(reltablespace, false, pg_class_desc);
						
						heap_close(pg_class_desc, RowExclusiveLock);
						
					}
					
                    /*
                     * If mirroring is enabled, force Redistribute Motion
                     * even if the query result is already partitioned on the
                     * same key as the target.  Need this to make sure that any
                     * function calls in the SELECT targetlist are executed
                     * on primaries only, and the result is then sent to
                     * primaries and mirrors to be inserted.
                     */
                    if (isFTSEnabled())
                        plan->flow->hashExpr = NIL;

                    /*
		             * Make sure the top level flow is partitioned on the
                     * partitioning key of the target relation.	Since this
                     * is a SELECT INTO (basically same as an INSERT) command, the target list will correspond
                     * to the attributes of the target relation in order.
		             */
                    hashExpr = getExprListFromTargetList(plan->targetlist,
                                                         targetPolicy->nattrs,
                                                         targetPolicy->attrs,
                                                         true);
                    if (!repartitionPlan(plan, true, false, false, hashExpr))
                        ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
                                        errmsg("Cannot parallelize that SELECT INTO yet")
                                ));

                    Assert(query->intoPolicy->ptype == POLICYTYPE_PARTITIONED);

				}
				else
				{
			    /*
                 * Query result needs to be brought back to the QD.
                 * Ask for motion to a single QE.  Later, apply_motion
                 * will override that to bring it to the QD instead.
                 *
                 * If the query has an ORDER BY clause, use Merge Receive to
                 * preserve the ordering.
                 */
                if (query->sortClause)
                {
					/* Since some node above the sort may have projected,
					 * we need to ralign the sort specifications. */
					ListCell *cell;
					TargetEntry *tle;
					SortClause *sortcl;
					int i = 0, n = list_length(query->sortClause);
					Assert( n == plan->flow->numSortCols);
					foreach (cell, query->sortClause)
					{
						sortcl = (SortClause*)lfirst(cell);
						/* this will issue an error if not found */
						tle = get_sortgroupclause_tle(sortcl, plan->targetlist);
						if ( tle->resno != plan->flow->sortColIdx[i] )
						{
							ereport(DEBUG4, (
								errmsg("cdbmutate realigned top-level Sort "
								       "operator %d column index: old = %d, new = %d",
									    i, plan->flow->sortColIdx[i], tle->resno) ));
										
							plan->flow->sortColIdx[i] = tle->resno;
						}
						i++;
					}
					
                    Insist(focusPlan(plan, true, true, false));
                    Insist(plan->flow->numSortCols > 0);
                }
                else
                    Insist(focusPlan(plan, true, false, false));
				}
			}
			break;

		case CMD_INSERT:
			{
				switch (targetPolicyType)
				{
					case POLICYTYPE_PARTITIONED:
                    {
                        List   *hashExpr;

                        /*
                         * If mirroring is enabled, force Redistribute Motion
                         * even if the query result is already partitioned on the
                         * same key as the target.  Need this to make sure any
                         * function calls in the SELECT targetlist are executed
                         * on primaries only, and the result is then sent to
                         * primaries and mirrors to be inserted.
                         */
                        if (isFTSEnabled())
                            plan->flow->hashExpr = NIL;

			            /*
			             * Make sure the top level flow is partitioned on the
                         * partitioning key of the target relation.	Since this
                         * is an INSERT command, the target list will correspond
                         * to the attributes of the target relation in order.
			             */
                        hashExpr = getExprListFromTargetList(plan->targetlist,
                                                             targetPolicy->nattrs,
                                                             targetPolicy->attrs,
                                                             true);
                        if (!repartitionPlan(plan, true, false, false, hashExpr))
                            ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
                                            errmsg("Cannot parallelize that INSERT yet")
                                    ));
                        break;
                    }

					case POLICYTYPE_ENTRY:
                        /* All's well if query result is already on the QD. */
                        if (plan->flow->flotype == FLOW_SINGLETON &&
                            plan->flow->segindex < 0)
                            break;

			            /*
                         * Query result needs to be brought back to the QD.
                         * Ask for motion to a single QE.  Later, apply_motion
                         * will override that to bring it to the QD instead.
                         */
                        if (!focusPlan(plan, true, false, false))
							ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
								            errmsg("Cannot parallelize that INSERT yet")
									));
						break;

					default:
						Insist(0);
				}
			}
			break;

		case CMD_UPDATE:
		case CMD_DELETE:
			/*
			 * For updates and deletes on partitioned tables, we restrict the query
			 * to only those with a partioned flow, and involving no other tables.
			 * In this manner , we avoid any motion nodes.	If there are subqueries,
			 * the query will get rejected by the parallelizer unless they are initPlan subqueries.
			 * If the table is an entry database table, we reject the query unless the
			 * flow is a singleton on the entry database.
			 */
			{
				/* Distributed target table? */
                if (targetPolicyType == POLICYTYPE_PARTITIONED)
                {
					/*
					 * Make sure we are not updating any of the partitioning columns
					 */
					if (query->commandType == CMD_UPDATE &&
						doesUpdateAffectPartitionCols(plan,
                                                      query,
                                                      targetPolicy->nattrs,
                                                      targetPolicy->attrs))
					{
						ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
										errmsg("Cannot parallelize an UPDATE statement that updates the distribution columns")
										));
					}
					if (plan->flow->flotype != FLOW_PARTITIONED)
					{
						ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
								 errmsg("Cannot parallelize that %s yet",
										query->commandType == CMD_UPDATE ? "UPDATE" : "DELETE")
										));
					}
                }

                /* Target table is not distributed.  Must be in entry db. */
                else
                {
					if (plan->flow->flotype == FLOW_PARTITIONED
						|| plan->flow->flotype == FLOW_REPLICATED
						|| (plan->flow->flotype == FLOW_SINGLETON && plan->flow->segindex != -1)
						)
					{
						ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
								 errmsg("Cannot parallelize that %s yet",
										query->commandType == CMD_UPDATE ? "UPDATE" : "DELETE")
										));
					}
					/*
					* Source is, presumably, a dispatcher singleton.
					*/
					plan->flow->req_move = MOVEMENT_NONE;
					break;
                }
            }
			break;

		default:
			Insist(0);		/* Never see non-DML in here! */
	}

	result = (Plan *) apply_motion_mutator((Node *) plan, &state);

	Assert(result->nMotionNodes == state.nextMotionID - 1);
    Assert(result->nInitPlans == list_length(state.initPlans));

    /* Assign slice numbers to the initplans. */
    foreach(cell, state.initPlans)
    {
        SubPlan    *subplan = (SubPlan *)lfirst(cell);

        Assert(IsA(subplan, SubPlan) && subplan->qDispSliceId == 0);
        subplan->qDispSliceId = state.nextMotionID++;
    }

	/* Last minute check for MPP badness. */
	if (query->commandType == CMD_DELETE ||
        query->commandType == CMD_UPDATE)
	{
        char   *cmd = (query->commandType == CMD_UPDATE) ? "UPDATE" : "DELETE";

		/*
		 * Currently, we only support update or delete involving 1 gang
         * (not counting Init Plans).
		 */
		if (result->nMotionNodes > state.numInitPlanMotionNodes)
		{
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
			ereport(ERROR, (
							errcode(ERRCODE_CDB_FEATURE_NOT_YET),
							errmsg("Cannot parallelize that %s yet",
					 query->commandType == CMD_UPDATE ? "UPDATE" : "DELETE"),
							errdetail("Result depends on values of CTID system column.  Support for CTID system column may change in the future.")
							));
		}
	}
	else
        warn_ctid_without_segid(query, result);

	return result;
}


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
    int     saveNumInitPlans;
    int     saveSliceDepth;
    bool    sendToSecondary = false;    /* always for now */

	if (node == NULL)
		return NULL;
	
    /* Mutate SubPlan manually cause plan_tree_mutator won't visit its plan. */
    if ( IsA(node, SubPlan) )
	{
		SubPlan *subplan = (SubPlan*)node;
		SubPlan *newsub;
		
        /* For now, the top slice of a SubPlan always executes on the qDisp. */
        saveSliceDepth = context->sliceDepth;
        context->sliceDepth = 0;

		newsub = makeNode(SubPlan);
		memcpy(newsub, node, sizeof(SubPlan));
		
		newsub->exprs = (List*) apply_motion_mutator((Node*)subplan->exprs, context);
		newsub->plan = (Plan*) apply_motion_mutator((Node*)subplan->plan, context);
		newsub->rtable = (List*) apply_motion_mutator((Node*)subplan->rtable, context);
		newsub->args = (List*) apply_motion_mutator((Node*)subplan->args, context);

        /* An IntList isn't interesting to mutate; just copy them. */
		newsub->paramIds = (List *)copyObject(subplan->paramIds);
		newsub->setParam = (List *)copyObject(subplan->setParam);
		newsub->parParam = (List *)copyObject(subplan->parParam);

        context->sliceDepth = saveSliceDepth;
		return (Node*)newsub;
	}

    /* An expression node might have subtrees containing plans to be mutated. */
    if (!is_plan_node(node))
		return plan_tree_mutator(node, apply_motion_mutator, context);

    plan = (Plan *)node;
    flow = plan->flow;

    saveNextMotionID = context->nextMotionID;
    saveNumInitPlans = list_length(context->initPlans);

    /* Descending into a subquery or a new slice? */
    saveSliceDepth = context->sliceDepth;
	Assert( !IsA(node,Query) ); /* TODO validate that following test is vestigial. */
    if (IsA(node, Query))
        context->sliceDepth = 0;
    else if (IsA(node, Motion))
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

    /* Make one big list of all of the initplans. */
    if (plan->initPlan)
    {
        ListCell   *cell;
        SubPlan    *subplan;
        int         nMotion = 0;

        foreach(cell, plan->initPlan)
        {
            subplan = (SubPlan *)lfirst(cell);
            Assert(IsA(subplan, SubPlan));
            nMotion += subplan->plan->nMotionNodes;
            context->initPlans = lappend(context->initPlans, subplan);
        }

        /* Keep track of the number of Motion nodes found in Init Plans. */
        context->numInitPlanMotionNodes += nMotion;
    }

    /* Pre-existing Motion nodes must be renumbered. */
    if (IsA(newnode, Motion))
    {
        Motion *motion = (Motion *)newnode;

        /* Motion nodes can't specify motion. */
        Assert(flow == NULL ||
               flow->req_move == MOVEMENT_NONE);

        /* Assign unique node number to the new node. */
        assignMotionID(newnode, context, node);

        /* If top slice marked as singleton, make it a dispatcher singleton. */
        if (motion->motionType == MOTIONTYPE_FIXED &&
            motion->numOutputSegs == 1 &&
            motion->outputSegIdx[0] >= 0 &&
            context->sliceDepth == 0)
        {
            if (flow)
            {
                Assert(flow->flotype == FLOW_SINGLETON &&
                       flow->segindex == motion->outputSegIdx[0]);
                flow->segindex = -1;
            }
            motion->outputSegIdx[0] = -1;
        }
        goto done;
    }

    /*
     * If no Flow node, we're in a portion of the tree that was
     * finished by the planner and contains no Motion nodes.
     */
    if (!flow)
        goto done;

    Assert(IsA(flow, Flow));

    /* How many sending processes? */
    numInputSegs = 1;
    if (flow->req_move != MOVEMENT_NONE &&
        flow->base_flotype != FLOW_SINGLETON)
    {
        Assert(flow->base_flotype != FLOW_UNDEFINED);
        numInputSegs = getMPPSegmentCount();
    }

    /* Add motion atop the copied node, if necessary. */
    switch (flow->req_move)
    {
        case MOVEMENT_FOCUS:
            /*
             * TODO Fix this branch (which requires, in turn,
             * extending our Flow support) to specify sort order,
             * whether to send to secondary, and which segment is
             * this one.  (Or should we specify "dispatcher" and
             * let plan state specify the actual segment number?).
             *
             * So make_union_motion( newnode, flow->sendToSecondary,
             * ThisSegmentNumber ); or something.
             */

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
                                                           flow->sortOperators);
            }
            else
            {
                newnode = (Node *)make_union_motion(plan,
                                                    sendToSecondary,
                                                    numInputSegs,
                                                    flow->segindex);
            }
            /* Assign unique node number to the new node. */
            assignMotionID(newnode, context, NULL);
            break;

        case MOVEMENT_BROADCAST:
            newnode = (Node *) make_broadcast_motion(plan, sendToSecondary,
                                                     numInputSegs);
            /* Assign unique node number to the new node. */
            assignMotionID(newnode, context, NULL);
            break;

        case MOVEMENT_REPARTITION:
            newnode = (Node *)make_hashed_motion(plan,
                                                 sendToSecondary,
                                                 numInputSegs,
                                                 flow->hashExpr
                                                 );
            /* Assign unique node number to the new node. */
            assignMotionID(newnode, context, NULL);
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

done:
    /*
     * Label the Plan node with the number of Motion nodes and Init Plans
     * in this subtree, inclusive of the node itself.  This info is used
     * only in the top node of a query or subquery.  Someday, find a better
     * place to keep it.
     */
    plan = (Plan *)newnode;
    plan->nMotionNodes = context->nextMotionID - saveNextMotionID;
    plan->nInitPlans = list_length(context->initPlans) - saveNumInitPlans;
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
 * --------------------------------------------------------------------
 */
Motion *
make_motion(Plan       *lefttree,
            MotionType  motionType,
			bool        sendToSecondary,
            bool        sendSorted,
            int         numInputSegs,
			List       *hashExpr,
			int         numOutputSegs,
            int        *outputSegIdx,
			int         numSortCols,
            AttrNumber *sortColIdx,
            Oid        *sortOperators)
{
	Flow *flow;
    Motion *node = makeNode(Motion);
    Plan   *plan = &node->plan;
    List   *hashDataTypes = NIL;

    validateMotionNodeParms(motionType,	sendToSecondary, sendSorted,
			hashExpr,
			numOutputSegs, outputSegIdx,
			numSortCols, sortColIdx, sortOperators);

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
            hashDataTypes = lappend_oid(hashDataTypes, lefttype);
        }
        list_free_deep(eq);
    }

	plan->startup_cost = lefttree->startup_cost;
	plan->total_cost = lefttree->total_cost;
	plan->plan_rows = lefttree->plan_rows;
	plan->plan_width = lefttree->plan_width;
	plan->targetlist = cdbpullup_targetlist(lefttree,
                        cdbpullup_exprHasSubplanRef((Expr *)lefttree->targetlist));
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	plan->dispatch = DISPATCH_PARALLEL;

	node->motionType = motionType;
	node->sendToSecondary = sendToSecondary;
	node->sendSorted = sendSorted;

    node->numInputSegs = numInputSegs;
    node->inputSegIdx = NULL;

	node->hashExpr = hashExpr;
	node->hashDataTypes = hashDataTypes;
	node->numOutputSegs = numOutputSegs;
	node->outputSegIdx = outputSegIdx;

	node->numSortCols = numSortCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;

	plan->extParam = bms_copy(lefttree->extParam);
	plan->allParam = bms_copy(lefttree->allParam);

	/* If hashed redistribution, fill in the receiving segment info. */
	node->numHashSegs = 0;
	node->hashSegIdx = NULL;
	if (motionType == MOTIONTYPE_HASH)
	{
		node->numHashSegs = getMPPSegmentCount();
		node->hashSegIdx = makeDefaultSegIdxArray(getMPPSegmentCount());
	}

    /* Attach a descriptive Flow. */
	switch ( node->motionType )
	{
	case MOTIONTYPE_HASH:
		flow = makeFlow(FLOW_PARTITIONED);
		flow->hashExpr = copyObject(node->hashExpr);
		flow->locustype = CdbLocusType_Hashed;
		break;
	case MOTIONTYPE_FIXED:
		if ( node->numOutputSegs == 0 )
		{
			flow = makeFlow(FLOW_REPLICATED);
			flow->locustype = CdbLocusType_Replicated;
		}
		else if ( node->numOutputSegs == 1 )
		{
			flow = makeFlow(FLOW_SINGLETON);
			flow->segindex = node->outputSegIdx[0];
			flow->locustype = ( flow->segindex<0 ) ?
								CdbLocusType_Root :
								CdbLocusType_SingleQE;
		}
		else
			flow = NULL;
		break;
	default:
		flow = NULL;
		break;
	}
	
	if ( flow != NULL && node->sendSorted && node->numSortCols > 0 )
	{
		int i, n;
		n = node->numSortCols;
		flow->numSortCols = n;
		flow->sortColIdx = palloc(n * sizeof(AttrNumber));
		flow->sortOperators = palloc (n * sizeof(Oid));
		for ( i = 0; i < n; i++ )
		{
			flow->sortColIdx[i] = node->sortColIdx[i];
			flow->sortOperators[i] = node->sortOperators[i];
		}
	}
	
	plan->flow = flow;

	return node;
}

Motion *
make_union_motion(Plan *lefttree, bool sendToSecondary,
                  int numInputSegs, int destSegIndex)
{
	int		   *outputSegIdx = palloc(sizeof(int));

	outputSegIdx[0] = destSegIndex;

	return make_motion(lefttree, MOTIONTYPE_FIXED,
					   sendToSecondary, false, numInputSegs,
					   NIL,
					   1, outputSegIdx,
					   0, NULL, NULL);
}

Motion *
make_sorted_union_motion(Plan *lefttree, bool sendToSecondary,
                         int numInputSegs, int destSegIndex,
				 int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators)
{
	int		   *outputSegIdx = palloc(sizeof(int));

	outputSegIdx[0] = destSegIndex;

	return make_motion(lefttree, MOTIONTYPE_FIXED,
					   sendToSecondary, true, numInputSegs,
					   NIL,
					   1, outputSegIdx,
					   numSortCols, sortColIdx, sortOperators);
}

Motion *
make_hashed_motion(Plan *lefttree, bool sendToSecondary, int numInputSegs,
				   List *hashExpr)
{
	return make_motion(lefttree, MOTIONTYPE_HASH,
					   sendToSecondary, false, numInputSegs,
					   hashExpr,
					   0, NULL, 0, NULL, NULL);
}

Motion *
make_broadcast_motion(Plan *lefttree, bool sendToSecondary, int numInputSegs)
{
	return make_motion(lefttree, MOTIONTYPE_FIXED,
					   sendToSecondary, false, numInputSegs,
					   NIL,
					   0, NULL,
					   0, NULL, NULL);
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

/* --------------------------------------------------------------------
 *
 *	Static Helper routines
 * --------------------------------------------------------------------
 */

/* --------------------------------------------------------------------
 * validateMotionNodeParms -- validates the parameters passed to
 * make_motion
 * --------------------------------------------------------------------
 */
void
validateMotionNodeParms(MotionType motionType,
						bool sendToSecondary, bool sendSorted,
						List *hashExpr,
						int numOutputSegs, int *outputSegIdx,
				 int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators)
{
	/*
	 *	Perform validation based on motionType.
	 */
	switch (motionType)
	{
		case MOTIONTYPE_HASH:

			/*
			 *	A Hash motion node shouldn't have FixedMotionDefn specified.
			 */
			Assert(numOutputSegs == 0 && outputSegIdx == NULL);
			break;

		case MOTIONTYPE_FIXED:

			/*
			 * Fixed motion node must have output Segs info specified.
             * Singleton destination must be QD or mpp_singleton_segindex.
			 */
			Assert((numOutputSegs == 0              /* destination: broadcast */
                     && outputSegIdx == NULL) ||
				   (numOutputSegs == 1              /* destination: singleton */
                     && outputSegIdx != NULL
                     && (outputSegIdx[0] == -1 ||
                         outputSegIdx[0] == mpp_singleton_segindex)));

			/*
			 *	A Fixed motion node shouldn't have HashMotionDefn specified.
			 */
			Assert(!hashExpr);
			break;

		default:
			/*
			 *	Invalid motion node type.
			 */
			Assert(false);
			break;
	}

	/*
	 *	Perform validation based on sendSorted flag
	 */
	if (sendSorted)
	{
		/*
		 *	A sorted motion node must have sort Cols specified
		 */
		Assert(numSortCols > 0 && sortColIdx && sortOperators);
	}
	else
	{
		/*
		 *	A non-sorted motion node should not have SortMotionDefn specified
		 */
		Assert(numSortCols == 0 && sortColIdx == NULL && sortOperators == NULL);
	}
}

/* --------------------------------------------------------------------
 * motion_expr_walker -- replaces occurrences of INNER with OUTER
 * for Var's varno in hashexpr for motion nodes.
 * This came about because the join nodes args for the right subtree were
 * changed by setrefs to be INNER.	We grabbed them after that change.
 * But the motion node has no inner subtree, just an outer.
 * --------------------------------------------------------------------
 */
bool
motion_expr_walker(Node *node, char *not_used)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		((Var *)node)->varno = OUTER;
		return false;
	}
	else
	{
		return expression_tree_walker(node, motion_expr_walker, (void *) not_used);
	}

}

/* ----------------------------------------------------------------
 * getExprListFromTargetList
 *
 * Creates a VAR that references the indicated entries from the target list,
 * sets the restype and restypmod fields from the target list info,
 * and puts them into a list.
 *
 * The AttrNumber indexes actually refer to the 1 based index into the
 * target list.
 *
 * The entries have the varno field replaced by references in OUTER.
 * ----------------------------------------------------------------
 */
List *
getExprListFromTargetList(List         *tlist,
                          int           numCols,
                          AttrNumber   *colIdx,
                          bool          useExecutorVarFormat)
{
	int			i;
	List	   *elist = NIL;

	for (i = 0; i < numCols; i++)
	{
		Var		   *new_var = NULL;

		/* Find expr in TargetList */
		AttrNumber	n = colIdx[i];

		TargetEntry *target = get_tle_by_resno(tlist, n);

		if (target == NULL)
			elog(ERROR, "no tlist entry for key %d", n);

        /* After set_plan_references(), make a Var referencing subplan tlist. */
        if (useExecutorVarFormat)
        {
            /* If subplan expr is a Var, copy to preserve its EXPLAIN info. */
            if (IsA(target->expr, Var))
            {
                new_var = copyObject(target->expr);
                new_var->varno = OUTER;
                new_var->varattno = n;
            }

            /*
	         * Make a Var that references the target list entry at this offset,
	         * using OUTER as the varno
	         */
            else
		        new_var = makeVar(OUTER,
                                  n,
                                  exprType((Node *) target->expr),
                                  exprTypmod((Node *) target->expr),
                                  0);
            elist = lappend(elist, new_var);
        }

        /* Before set_plan_references(), copy the subplan's result expr. */
        else
            elist = lappend(elist, copyObject(target->expr));
	}

	return elist;
}


/*
 * isAnyColChangedByUpdate
 */
static bool
isAnyColChangedByUpdate(Index       targetvarno,
                        List       *targetlist,
                        int         nattrs,
                        AttrNumber *attrs)
{
	/*
	 * Want to test whether an update statement possibly changes the
	 * value of any of the partitioning columns.
	 */

	/*
	 * For each partitioning column, the TargetListEntry for that varattno
	 * should be just a Var node with the same varattno.
	 */
	int     i;

	for (i = 0; i < nattrs; i++)
	{
		TargetEntry    *tle = get_tle_by_resno(targetlist, attrs[i]);
		Var		       *var;

		Insist(tle);

		if (!IsA(tle->expr, Var))
			return true;

		var = (Var *)tle->expr;
		if (var->varnoold != targetvarno ||
            var->varoattno != attrs[i])
			return true;
	}

	return false;
}                               /* isAnyColChangedByUpdate */


/*
 * doesUpdateAffectPartitionCols
 */
bool
doesUpdateAffectPartitionCols(Plan         *plan,
                              Query        *query,
                              int           nattrs,
                              AttrNumber   *attrs)
{
    Append     *append;
    ListCell   *rticell;
    ListCell   *plancell;

    /*
     * Single target relation?
     */
    if (query->resultRelations == NIL)
        return isAnyColChangedByUpdate(query->resultRelation,
                                       plan->targetlist,
                                       nattrs,
                                       attrs);

    /*
     * Multiple target relations (inheritance)...
     * Plan should have an Append node on top, with a subplan per target.
     */
    append = (Append *)plan;

	Insist(IsA(append, Append) &&
	       append->isTarget &&
           list_length(append->appendplans) == list_length(query->resultRelations));

    forboth(rticell, query->resultRelations,
            plancell, append->appendplans)
    {
        int     rti = lfirst_int(rticell);
        Plan   *subplan = (Plan *)lfirst(plancell);

        if (isAnyColChangedByUpdate(rti, subplan->targetlist, nattrs, attrs))
            return true;
    }

    return false;
}                               /* doesUpdateAffectPartitionCols */



/* ----------------------------------------------------------------------- *
 * Functions hasCtid(), hasCtid_outside_tlist() and helper function
 * ctid_inventory_walker() search a plan tree for a use of the CTID
 * pseudo-attribute.
 *
 * warn_ctid_without_segid() warns the user if the plan refers to a
 * partitioned table's ctid column without also referencing its
 * mpp_segment_id column.
 *
 * hasCtid_outside_tlist() detects a CTID reference outside of simple
 * Var in the targetlist of the top-level plan node.  The use of this
 * function is pretty much restricted to the case of a UPDATE or DELETE
 * in MPP first release.
 * ----------------------------------------------------------------------- *
 */
typedef struct ctid_inv_data
{
	Relids      ctid_relids;
    Relids      segid_relids;
}	ctid_inv_data;

static bool ctid_inventory_walker(Node *node, ctid_inv_data * inv);

void
warn_ctid_without_segid(Query *query, Plan *plan)
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
	    rte = rt_fetch(relid, query->rtable);
        if (rte->rtekind == RTE_RELATION)
        {
            Relation    relation;
            bool        isPartitioned;

            relation = relation_open(rte->relid, NoLock);
            isPartitioned = (relation->rd_cdbpolicy &&
                             relation->rd_cdbpolicy->ptype == POLICYTYPE_PARTITIONED);
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
                                   "\"mpp_segment_id\" system column "
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
                case MppSegmentIdAttributeNumber:       /* mpp_segment_id */
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
has_ctid_outside_tlist_walker(Node *node, void* unused)
{
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

