/*-------------------------------------------------------------------------
 *
 * cdbmutate.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2004-2007, Greenplum
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
#include "catalog/pg_type.h"

#include "cdb/cdbfts.h"         /* isFTSEnabled() */
#include "cdb/cdbhash.h"        /* isGreenplumDbHashable() */
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
            bool        sendToSecondary,
            bool        sendSorted,
            int         numInputSegs,
            int         numSortCols,
            AttrNumber *sortColIdx,
            Oid        *sortOperators);
	
static Node *apply_motion_mutator(Node *node, ApplyMotionState * context);

static bool
doesUpdateAffectPartitionCols(Plan         *plan,
                              Query        *query);
static void
warn_ctid_without_segid(Query *query, Plan *plan);

static bool has_ctid_outside_tlist_walker(Node *node, void* unused);

#if 0
static bool DoesDistributionHaveDependencies(Oid relOid, GpPolicy * policy);
#endif

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
    GpPolicy          *targetPolicy = NULL;
    GpPolicyType       targetPolicyType = POLICYTYPE_ENTRY;
	ApplyMotionState    state;

    /* Initialize mutator context. */
    state.nextMotionID = 1;		/* Start at 1 so zero will mean "unassigned". */
    state.sliceDepth = 0;
    state.numInitPlanMotionNodes = 0;
    state.initPlans = NIL;

	Assert(is_plan_node((Node *) plan) && IsA(query, Query));
    Assert(plan->flow && plan->flow->flotype != FLOW_REPLICATED);

	/* Does query have a target relation?  (INSERT/DELETE/UPDATE) */
	
	/*
	 *  NOTE: This code makes the assumption that if we are working on a heirarchy of tables,
	 *  all the tables are distributed, or all are on the entry DB.  Any mixture will fail
	 */
    if (query->resultRelation > 0)
    {
	    RangeTblEntry  *rte;
        Relation        relation;
		
        rte = rt_fetch(query->resultRelation, query->rtable);
	    Assert(rte->rtekind == RTE_RELATION);

        /* Get a copy of the rel's GpPolicy from the relcache. */
        relation = relation_open(rte->relid, NoLock);
        targetPolicy = RelationGetPartitioningKey(relation);
        relation_close(relation, NoLock);

        if (targetPolicy)
            targetPolicyType = targetPolicy->ptype;
    }

	switch (query->commandType)
	{
		case CMD_SELECT:
			

			if (query->into)
			{
				List   *hashExpr;
				ListCell *exp1;
				int			maxattrs = 200;
				
				if (query->intoPolicy != NULL)
				{
					targetPolicy = query->intoPolicy;
					Assert(query->intoPolicy->ptype == POLICYTYPE_PARTITIONED);
					Assert(query->intoPolicy->nattrs >= 0);
					Assert(query->intoPolicy->nattrs <= 1024);
				}
				else
				{
					/* User did not specify a DISTRIBUTED BY clause */
				
					targetPolicy = palloc0(sizeof(GpPolicy)- sizeof(targetPolicy->attrs) + maxattrs * sizeof(targetPolicy->attrs[0]));
					targetPolicy->nattrs = 0;
					
					targetPolicy->ptype = POLICYTYPE_PARTITIONED;
					
					/* Find out what the flow is partitioned on */
					hashExpr = plan->flow->hashExpr;
					
					if(hashExpr)
					foreach(exp1, hashExpr)
					{
						AttrNumber	n;
						bool found_expr = false;
						/* See if this Expr is a column of the result table */
						
						for(n=1;n<=list_length(plan->targetlist);n++)
						{
							Var		   *new_var = NULL;
							
							TargetEntry *target = get_tle_by_resno(plan->targetlist, n);
							
							if (!target->resjunk)
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
					
								if (equal(lfirst(exp1),new_var))
								{
									/* If it is, use it to partition the result table, to avoid
									 * unnecessary redistibution of data */
									targetPolicy->attrs[targetPolicy->nattrs++] = n;
									found_expr = true;
									break;
									
								}
								
							}
						}
						
						if (!found_expr)
							break;
					}

					/* do we know how to partition? */
					if (targetPolicy->nattrs == 0)
					{
						/*
						 * hash on the first hashable column we can find.
						 */
						int i;
						for (i = 0; i < list_length(plan->targetlist); i++)
						{
							TargetEntry *target = get_tle_by_resno(plan->targetlist, targetPolicy->attrs[i]);
							if (target)
							{
								Oid typeOid = exprType((Node *) target->expr);
								bool isArray = false;  /* How do I find this out? */
								
								if(isGreenplumDbHashable(typeOid) || isArray)
								{
									targetPolicy->attrs[targetPolicy->nattrs++] = i+1;
									break;
								}
							}
						}
					}
					
					if (query->intoPolicy == NULL)
					{
						char * columns;
						int i;
						columns = palloc((NAMEDATALEN+3)*targetPolicy->nattrs+1);
						columns[0] = '\0';
						for (i=0;i<targetPolicy->nattrs;i++)
						{
							TargetEntry *target = get_tle_by_resno(plan->targetlist, targetPolicy->attrs[i]);
							if (i>0)
								strcat(columns, ", ");
							if (target->resname)
								strcat(columns, target->resname);
							else
								strcat(columns, "???");
						
						}
					
						ereport(NOTICE,
								(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
								 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column(s) "
									    "named '%s' as the Greenplum Database data distribution key for this "
									    "table. ", columns),
							     errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
							     		 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
					}
				}
				
				query->intoPolicy = targetPolicy;
				
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
			
			if (plan->flow->flotype == FLOW_PARTITIONED && !query->into)
			{
			    /*
                 * Query result needs to be brought back to the QD.
                 * Ask for motion to a single QE.  Later, apply_motion
                 * will override that to bring it to the QD instead.
                 *
                 * If the query has an ORDER BY clause, use Merge Receive to
                 * preserve the ordering.
                 *
                 * The plan has already been set up to ensure each qExec's 
                 * result is properly ordered according to the ORDER BY 
                 * specification.  The existing ordering could be stronger 
                 * than required; if so, omit the extra trailing columns 
                 * from the Merge Receive key.
                 *
                 * An unordered result is ok if the ORDER BY ordering is
                 * degenerate (on constant exprs) or the result is known to
                 * have at most one row. 
                 */
                if (query->sortClause &&
                    plan->flow->numSortCols > 0)
                {
                    if (plan->flow->numSortCols > list_length(query->sortClause))
                        plan->flow->numSortCols = list_length(query->sortClause);

                    Insist(focusPlan(plan, true, true, false));
                }

                /* Use UNION RECEIVE.  Does not preserve ordering. */
                else
                    Insist(focusPlan(plan, true, false, false));
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
						doesUpdateAffectPartitionCols(plan,query))
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

	/* Last minute check for Greenplum Database badness. */
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

    /* 
	 * Discard subtrees of Query node that aren't needed for execution. 
	 * Note the targetlist (query->targetList) is used in execution of
	 * prepared statements, so we leave that alone.
	 */
    query->jointree = NULL;
    query->groupClause = NIL;
    query->havingQual = NULL;
    query->distinctClause = NIL;
    query->sortClause = NIL;
    query->limitCount = NULL;
    query->limitOffset = NULL;
    query->setOperations = NULL;

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
		
		newsub->testexpr = (Node*) apply_motion_mutator((Node*)subplan->testexpr, context);
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

#ifdef USE_ASSERT_CHECKING
		/* Sanity check */
		/* Sub plan must have flow */
        Assert(flow && motion->plan.lefttree->flow);
		Assert(flow->req_move == MOVEMENT_NONE && !flow->flow_before_req_move);
#endif

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
                                                           flow->sortOperators);
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
            Oid        *sortOperators)
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
	plan->dispatch = DISPATCH_PARALLEL;

	node->sendToSecondary = sendToSecondary;
	node->sendSorted = sendSorted;

    node->numInputSegs = numInputSegs;
    node->inputSegIdx = NULL;

	node->numSortCols = numSortCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;

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
		for ( i = 0; i < n; i++ )
		{
			motion->plan.flow->sortColIdx[i] = motion->sortColIdx[i];
			motion->plan.flow->sortOperators[i] = motion->sortOperators[i];
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
			0, NULL, NULL /* numSortCols, sortColIdx, sortOperaotrs */
			);
	add_slice_to_motion(motion, MOTIONTYPE_FIXED, 
			NULL, 1, outSegIdx);
	return motion;
}

Motion *
make_sorted_union_motion(Plan *lefttree, bool sendToSecondary,
                         int numInputSegs, int destSegIndex,
				 int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators)
{
	Motion 	*motion;
	int		*outSegIdx = (int *) palloc(sizeof(int)); 
	outSegIdx[0] = destSegIndex; 

	motion = make_motion(lefttree, sendToSecondary, true, numInputSegs,
			numSortCols, sortColIdx, sortOperators);

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
			0, NULL, NULL);
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
			0, NULL, NULL);

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

/* --------------------------------------------------------------------
 *
 *	Static Helper routines
 * --------------------------------------------------------------------
 */

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
		/* Find expr in TargetList */
		AttrNumber	n = colIdx[i];

		TargetEntry *target = get_tle_by_resno(tlist, n);

		if (target == NULL)
			elog(ERROR, "no tlist entry for key %d", n);

        /* After set_plan_references(), make a Var referencing subplan tlist. */
        if (useExecutorVarFormat)
            elist = lappend(elist, cdbpullup_makeVar(OUTER, n, NULL, target->expr));

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
                              Query        *query)
{
    Append     *append;
    ListCell   *rticell;
    ListCell   *plancell;
    GpPolicy  *policy;

    if (query->resultRelations == NIL)
    {
	    RangeTblEntry  *rte;
        Relation        relation;

        rte = rt_fetch(query->resultRelation, query->rtable);
	    Assert(rte->rtekind == RTE_RELATION);

        /* Get a copy of the rel's GpPolicy from the relcache. */
        relation = relation_open(rte->relid, NoLock);
        policy = RelationGetPartitioningKey(relation);
        relation_close(relation, NoLock);


	    /*
	     * Single target relation?
	     */

        return isAnyColChangedByUpdate(query->resultRelation,
                                       plan->targetlist,
                                       policy->nattrs,
                                       policy->attrs);
    }

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

        RangeTblEntry  *rte;
        Relation        relation;

        rte = rt_fetch(rti, query->rtable);
	    Assert(rte->rtekind == RTE_RELATION);

        /* Get a copy of the rel's GpPolicy from the relcache. */
        relation = relation_open(rte->relid, NoLock);
        policy = RelationGetPartitioningKey(relation);
        relation_close(relation, NoLock);

        if (isAnyColChangedByUpdate(rti, subplan->targetlist, policy->nattrs, policy->attrs))
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

	if(IsA(node, SubPlan))
	{
		SubPlan *subplan = (SubPlan *) node;
		shareinput_walker(f, (Node *)subplan->plan, ctxt); 
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
			return;
		}
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


static bool shareinput_mutator_xslice(Node* node, ApplyShareInputContext *ctxt, bool fPop)
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
		
		if(!shared)
		{
			ShareNodeWithSliceMark plan_slicemark;
			bool found = shareinput_find_sharenode(ctxt, sisc->share_id, &plan_slicemark);
			int  shareSliceId = get_plan_driver_slice(plan_slicemark.plan);
			Assert(found);

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

Plan *apply_shareinput_xslice(Plan *plan, ApplyShareInputContext *ctxt) 
{
	ctxt->sharedNodes = NULL;
	ctxt->sliceMarks = NULL;
	ctxt->motStack = NULL;

	shareinput_pushmot(ctxt, 0);
	
	shareinput_walker(shareinput_mutator_xslice, (Node *) plan, ctxt); 
	
	return plan;
}

