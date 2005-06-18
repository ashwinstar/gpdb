/*-------------------------------------------------------------------------
 *
 * cdbllize.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2005, greenplum inc
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "nodes/print.h"

#include "optimizer/planmain.h" /* for is_projection_capable_plan() */
#include "parser/parsetree.h" /* for rt_fetch() */
#include "utils/lsyscache.h" /* for getatttypetypmod() */
#include "parser/parse_oper.h" /* for compatible_oper_opid() */
#include "nodes/makefuncs.h" /* for makeVar() */
#include "nodes/value.h" /* for makeString() */

#include "cdb/cdbvars.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbjoin.h"
#include "cdb/cdbagg.h"
#include "cdb/cdbmutate.h"

#define ARRAYCOPY(to, from, sz) \
	do { \
		Size	_size = (sz); \
		(to) = palloc(_size); \
		memcpy((to), (from), _size); \
	} while (0)

/*
 * Substructure for a PlanProfile to hold "stackable" information like
 * the range table.  
 */
typedef struct LocalProfile
{
	List *rtable; /* The current range table. */
	bool finished; /* Are we inside a finished (by the planner) branch? */
	
	struct LocalProfile *outerFrame; /* The next more global context or NULL. */

} LocalProfile;

/* 
 * A PlanProfile holds state for recursive prescan_walker(). 
 */
typedef struct PlanProfile
{	
	/* The result flags refer to relation in the result of an insert, update, or delete */
	bool	resultSegments;
	
	/* Context */
	LocalProfile *local;
	
	/* Message data */
	unsigned code;
	const char *message;
	const char *detail;
	
} PlanProfile;


/*
 * Forward Declarations
 */
static bool prescan(Plan *plan, Query *query, PlanProfile *context);
static bool prescan_walker(Node *node, PlanProfile *context);

static bool prescan_hashjoin(Plan *plan, PlanProfile *context);
static bool prescan_mergejoin(Plan *plan, PlanProfile *context);
static bool prescan_nestloop(Plan *plan, PlanProfile *context);
static bool prescan_agg(Plan *plan, PlanProfile *context);

static void psError(PlanProfile *context, unsigned code, const char* message, const char* detail);
static void psErrorNonce(PlanProfile *context,  const char* detail);
static void psErrorInternal(PlanProfile *context,  const char* detail);
static void issuePsError(PlanProfile *context);

static bool initPlanProfile(PlanProfile *context, Query *query);
static void pushLocals(PlanProfile *profile, List *rtable);
static void popLocals(PlanProfile *profile);
static  List *localRangeTable(PlanProfile *profile);
static bool isPlanFinished(Node *node, PlanProfile *profile);

static bool scanForManagedTables_walker(Node *node, PlanProfile *context);
static bool scanForManagedTables(Plan *plan, Query *query);

static void initTargetFlow(Plan *plan, Flow *target, bool stable);
static bool focusPlan(Plan* plan, bool live, bool stable, bool rescanable, int segidx);
static bool broadcastPlan(Plan* plan, bool live, bool stable, bool rescanable);
static bool repartitionPlan(Plan* plan, bool live, bool stable, bool rescanable, List *hashExpr);
static bool adjustPlanFlow(Plan *plan, Flow *target, bool stable, bool rescanable, bool live);
static bool focusBasePlan(Plan *plan, Flow *target, bool stable, bool rescanable, bool live);
static bool broadcastBasePlan(Plan *plan, Flow *target, bool stable, bool rescanable, bool live);
static bool repartitionBasePlan(Plan *plan, Flow *target, bool stable, bool rescanable, bool live);

/* ------------------------------------------------------------------------- *
 * Function cdbparallelize() is the main entry point.  
 *
 * Parallelize a single raw PostgreSQL sequential plan. The input plan
 * is modified in that Flow nodes are attached to Plan nodes in the input
 * as appropriate and the top-level plan node's dispatch field is set
 * to specify how the plan should be handled by the executor.
 *
 * If the dispatch field of the top-level plan is set to a non-default
 * value on entry, the plan is returned unmodified.  This guarantees that
 * we won't parallelize the same input plan more than once, provides a way
 * for the planner to create a plan that this function will recognize as
 * already final.
 *
 * The result is [a copy of] the input plan with alterations to make the 
 * plan suitable for execution by CDB.
 *
 *
 * TODO: Much investigation remains in the areas of 
 *
 *      - initialization plans,
 *      - plan parameters, 
 *      - subqueries,
 *      - ordering and hash attributes,
 *      - etc.
 *
 *      The current implementation is little more than a rough sketch.
 *
 *
 * Outline of processing:
 *
 * - Return the input plan unmodified, if it's dispatch field is set.
 *
 * - Scan (scanForManagedTables) the input plan looking for references to
 *   managed tables.  If none are found, mark the plan for sequential
 *   dispatch and return it.
 *     
 * - Augment the input plan (prescan) by attaching Flow nodes to each of
 *   its Plan nodes.  This is where all the decisions about parallelizing
 *   the query occur.
 *
 * - Implement the decisions recorded in the Flow nodes (apply_motion) 
 *   to produce a modified copy of the plan.  Mark the copy for parallel 
 *   dispatch and return it.
 * ------------------------------------------------------------------------- *
 */
Plan *
cdbparallelize(	Plan* plan, 
				Query *query, 
				bool isCursor, 
				int cursorOptions, 
				ParamListInfo boundParams
			)
{
	PlanProfile profile; /* Will be initialized by prescan(). */
	int nParamExec;
	
	Assert( is_plan_node((Node*)plan) );
		/*
	 * Print plan if debugging.
	 */
	if (Debug_print_prelim_plan)
		elog_node_display(DEBUG1, "preliminary plan", plan, Debug_pretty_print);

	
	Assert( query != NULL && IsA(query, Query) );
	
	/* If the plan is already marked sequential, just return it. */
	if ( plan->dispatch == DISPATCH_SEQUENTIAL )
		return plan;

	/* Do a quick walk of the plan to see whether it references
	 * any managed tables.  If not, we don't need to parallelize it.
	 */
	if ( !scanForManagedTables(plan, query) )
	{
		plan->dispatch = DISPATCH_SEQUENTIAL;
		return plan;
	}
	
	/* Save the global count of PARAM_EXEC from the top plan node. */
	nParamExec = plan->nParamExec;
	
	/* Prescan the plan and attach Flow nodes to its Plan nodes. These
	 * nodes describe how to parallelize the plan.  We also accumulate
	 * summary information in the profile stucture.
	 */
	if ( !prescan(plan, query, &profile) )
		issuePsError(&profile);	

	/* Implement the parallelizing directions in the attached flows.
	 */
	plan = apply_motion(plan, query);
	plan->dispatch = DISPATCH_PARALLEL;
	
	/* Restore the global count of PARAM_EXEC from the top plan node. */
	plan->nParamExec = nParamExec;

	return plan;
}


/* ----------------------------------------------------------------------- *
 * Functions prescan() and prescan_walker() use the plan_tree_walker() 
 * framework to visit the nodes of a plan tree to be parallelized and
 * add flow information to the leaves for later use.  The walker may 
 * also choose to exit with an error, usually to report an unsupported
 * feature used in the plan tree or an unexpected problem.
 *
 * The plan and query arguments should not be null.
 *
 * Returns true if the scan results in a parallelization strategy or
 * no parallelization is required;  false otherwise.
 *
 * In case the result is false, the caller should issue an error.
 * ----------------------------------------------------------------------- *
 */
bool
prescan(Plan *plan, Query *query, PlanProfile *context)
{
	/* Initialize the PlanProfile result. */
	if ( !initPlanProfile(context, query) )
		return false;
	
	/*
	 * Do the prescan.
	 */
	return !prescan_walker((Node*)plan, context);
}


/*
 * Function prescan_walker is the workhorse of prescan.
 *
 * The driving function, prescan(), should be called only once on the
 * sequential plan produced by the the Postgres optimizer.  In particular,
 * this function assumes it will see a tree will no Flow nodes or Motion
 * nodes in it.  It will add Flow nodes which, in turn, will specify
 * where Motion nodes should go.
 *
 * The general pattern for assigning flows to plan nodes is to examine
 * leaf nodes (scans) and assign them a flow prior to descending into
 * children (expressions), and to descend into children of interior
 * nodes (in order to assign their children's flows) before examining 
 * them and assigning their flows.
 *
 * This means that child (lefttree, righttree, etc)  Plan nodes of the 
 * current Plan node (the one under investigation) will have had their 
 * Flow nodes assigned already while the current Plan node will not. 
 * Note, however, that the child plans may have their Flow nodes adjusted, 
 * to meet the requirements of the current node.
 */
bool 
prescan_walker(Node *node, PlanProfile *context)
{	
	bool finished;
    bool parent_finished;
	const char *detail = NULL;
	
	if ( node == NULL )
		return false;
	
	parent_finished = context->local->finished;
    finished = parent_finished || isPlanFinished(node, context);
	
	switch (nodeTag(node))
	{
		case T_SubPlan:
			/* A SubPlan node is not a Plan node, but an Expr node that
			 * contains a Plan and its range table.  It corresponds to a
			 * SubLink in the query tree (a SQL subquery expression) or a
			 * plan in an initplan list.
			 *
			 * TODO Add support for multirow and correlated subqueries.
			 *      Will this require additional context to distinguish
			 *      "in an initPlan" from "in an ordinary expression"? 
			 */
			{
				/* SubPlans establish a local state for the contained plan,
				 * notably the range table.
				 */
				SubPlan    *subplan = (SubPlan*) node;

				/* Nip processing of correlated subquery in the bud. */
				if ( subplan->parParam != NIL || subplan->args != NIL )
				{
					detail = "Query involves a correlated subquery.";
					break; /* Fall thru to error. */
				}
				
				/* We're not ready to dispatch multirow subqueries (which
				 * would be embeded in plans) yet, so cut them off.
				 *
				 * TODO Verify that noncorrelated,  single row subqueries
				 *      will be pull off into initPlans, which is all we're
				 *      ready for.
				 */
				if ( !( subplan->subLinkType==EXISTS_SUBLINK
						|| subplan->subLinkType==MULTIEXPR_SUBLINK
						|| subplan->subLinkType==EXPR_SUBLINK )
					)
				{
					detail = "Query involves multi-row subquery expression.";
					break; /* Fall thru to error. */
				}

				/* Recur into the exprs list (as usual). */
				if (expression_tree_walker((Node *) subplan->exprs, prescan_walker, context))
					goto return_true;
				
				/* Set local state for the subplan.  This has two effects. 
				 * The local state now has the new rangetable.  Also, the 
				 * "finished" indicator is cleared, so that when recur to
				 * parallelize the plan in the SubPlan expression, we will
				 * make a fresh determination of how to interpret the plan: 
				 * as sequential or as already parallel (down to the next 
				 * level of SubPlan).
				 */
				pushLocals(context, subplan->rtable);
				
				/* Parallelize the subplan.  */
				if ( prescan_walker((Node*)subplan->plan, context) )
					goto return_true;
				
				Assert(subplan->plan->flow != NULL && subplan->plan->flow->req_move == MOVEMENT_NONE);
				
				/* Adjust the subplan flow, if necessary. */
				if ( subplan->plan->flow->flotype == FLOW_PARTITIONED 
					|| ( subplan->plan->flow->flotype == FLOW_SINGLETON 
						&& subplan->plan->flow->segindex  != -1 )
					)
				{
					/* Flow is partitioned or singleton on a segment.  Refocus. */
					subplan->plan->flow->req_move = MOVEMENT_FOCUS;
					subplan->plan->flow->segindex = -1;
				}
				else if ( subplan->plan->flow->flotype == FLOW_SINGLETON 
					&& subplan->plan->flow->segindex == -1 )
				{
					/* Flow is singleton on dispatcher.  Already focused. */
				}
				else
				{
					/* 
					 * TODO  Are the other flows we might legitimately see at the
					 * top level.  E.g., a replicated flow that we would convert
					 * with some hypothetical REPLICATED-to-SINGLETON(-1) motion! 
					 */
					 ereport(ERROR, (
						errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("unexpected flow type at top of subplan")
					 ));
				}
				
				/* Restore state for the parent plan. */
				popLocals(context);
				
				/* Recur into the args list. */
				if (expression_tree_walker((Node *) subplan->args, prescan_walker, context))
					goto return_true;
			}
			goto return_false;
			
		case T_Flow:
			/* Since this function is where Flow nodes are generated, we can't 
			 * rely on this case to see them all.  Instead we handle them as 
			 * they are created.
			 */
			goto return_false;
		
		case T_Motion:
			/* If we see a Motion node, it must be part of a finished plan
			 * produced by the planner.  We still need to walk into it,  
			 * though, in case it contains an unfinished subplan.
			 */
			Assert(finished);
			if (plan_tree_walker(node, prescan_walker, context))
                goto return_true;
            goto return_false;
		
		case T_SeqScan:
		case T_TidScan:
		case T_IndexScan:
		case T_FunctionScan:
		case T_SubqueryScan:
			/* A Scan is a source of tuples and thus occurs as a leaf of the
			 * plan tree.  Note, however, that scans may include SubPlan nodes 
			 * (directly as in a Subquery scan or indirectly through a SubPlan
			 * node in an expresion) so they aren't necessarily a base case
			 * for recursion.
			 */
			{
				Plan *plan = (Plan*)node;
				Scan *scan = (Scan*)plan;
				Index rtindex = scan->scanrelid;
                RangeTblEntry *rte;
				
				/* Make sure this is first time. 
				 * TODO: Clean-up this mess. Our contract with the planner
				 *       specifies that that there will be a finish flow on
				 *       a relation scanning node so we shouldn't need to
				 *       look up relation policy, etc.  But we have to keep
				 *       working until the planner is ready.  Another part
				 *       of the contract says that a node in an unfinished
				 *       part of the plan will have a flow allocated but with
				 *       a type of FLOW_UNDEFINED. So we accept that but
				 *       just orphan it to be cleaned up with the memory
				 *       context.
				 */
				Assert( finished || plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED );
				
				/* Make sure we have a valid index. */
				Assert( 0<rtindex && rtindex<=list_length(localRangeTable(context)) );
				
				/* All scan types have a range table entry. */
				rte = rt_fetch(rtindex, localRangeTable(context));
				
				/* The entry type is related to the to the scan type. */
				switch (nodeTag(node))
				{
					case T_SeqScan:
					case T_TidScan:
					case T_IndexScan:
                    case T_FunctionScan:
                        Assert(finished);
						if ( plan_tree_walker(node, prescan_walker, context) )
							goto return_true;
						goto return_false;
										
					case T_SubqueryScan:
						Assert( rte->rtekind == RTE_SUBQUERY );
						/*
						 * Parallelize a Subquery in the FROM clause.
						 *
						 * TODO Is there any special handling needed for
						 *      parameters?
						 */
						{
							SubqueryScan *subqueryscan = (SubqueryScan*)node;
							Plan *subplan = subqueryscan->subplan;
							
							/* The subplan uses the range table of the
							 * Query node atttached to the subquery RTE.
							 * Also, the local context forces reexamination
							 * of the contained query.
							 */
							pushLocals(context, rte->subquery->rtable);
							if ( prescan_walker((Node*)subplan, context) )
								goto return_true;
							popLocals(context);
							
                            Assert(!finished);
                            if ( !finished )
							{
								plan->flow = pull_up_Flow(plan, subqueryscan->subplan, true);
                                Assert(plan->flow);
								/* Don't use expressions from the wrong range table */
								plan->flow->hashExpr = NIL; 
							}
							
							if ( ((Plan*)node)->flow == NULL || ((Plan*)node)->flow->flotype == FLOW_UNDEFINED )
							{
								detail = "Can't characterize flow from SubqueryScan.";
								break; /* Fail */
							}
							
							goto return_false;
						}
					
					default: /* Never happens, but makes the compiler happy. */
						Assert( false );
				} /* inner switch */
			}
			break;  /* Fall through to report error. */
		

		case T_Result:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				goto return_true;
			if ( !finished )
			{
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				if ( child != NULL )
				{
					/* Might scan child, so inherit flow characteristics. */
					plan->flow = pull_up_Flow(plan, plan->lefttree, false);
					
					if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
					{
						detail = "Can't characterize flow from Result node.";
						break;
					}
					
					/* We aren't sure the partitioning attributes are in
					 * our targetlist, so we shouldn't claim they are.
					 */
					plan->flow->hashExpr = NIL;
				}
				else
				{
					/* Treat Var-free target list as replicated. */
					plan->flow = makeFlow(FLOW_REPLICATED);
				}
			}
			goto return_false;

		case T_Material:
		case T_Hash:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				goto return_true;
			if ( !finished )
			{
				/* Flow is same as leftree's Flow. */
				Plan *plan = (Plan*)node;
				plan->flow = pull_up_Flow(plan, plan->lefttree, false);
					
				if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
				{
					detail = "Can't characterize flow from Hash or Material node.";
					break;
				}
			}
			goto return_false;
		
		case T_Sort:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				goto return_true;
			if ( ! finished )
			{
                Sort *sort; 

                /* Flow is same as leftree's Flow ... */
				Plan *plan = (Plan*)node;
				plan->flow = pull_up_Flow(plan, plan->lefttree, false);
					
				if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
				{
					detail = "Can't characterize flow from Sort node.";
					break;
				}
				
				/* ... but reordered. */
				sort = (Sort*)node;
				plan->flow->numSortCols = sort->numCols;
				ARRAYCOPY(plan->flow->sortColIdx, sort->sortColIdx, 
					sort->numCols*sizeof(AttrNumber) );
				ARRAYCOPY(plan->flow->sortOperators, sort->sortOperators, 
					sort->numCols*sizeof(Oid) );
			}
			goto return_false;
		
		case T_Unique:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				goto return_true;
			if ( finished )
				goto return_false;
				
			{
				/* Flow is same as leftree's Flow. */
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);

				Assert( child != NULL && child->flow != NULL );
				if ( child->flow->flotype == FLOW_REPLICATED
					|| child ->flow->flotype == FLOW_SINGLETON )
				{
					/* No motion required so inherit flow characteristics. */
					plan->flow = pull_up_Flow(plan, child, true);
						
					if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
					{
						detail = "Can't characterize flow from Unique node.";
						break;
					}
					goto return_false;
				}
				
				/* The argument flow is partitioned, so we need to make sure
				 * that tuples are collocated on the input attributes to be
				 * uniqued, i.e., the numCols element array of AttrNumber as
				 * uniqColIdx.
				 *
				 * Note the input flow is certainly sorted this way and
				 * that no projection is done by this node, however, we
				 * must prove that the unique attributes are a prefix of
				 * the hash attributes.
				 */
				/* TODO - Implement the above. */
			}
			detail = "Query involves partitioned duplicate removal.";
			break; /* Fall through to report error. */
		
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				Plan *plan = (Plan*)node;
				Join *join = (Join*)plan;
				
				Assert ( join->jointype == JOIN_INNER
					|| join->jointype == JOIN_LEFT
					|| join->jointype == JOIN_FULL
					|| join->jointype == JOIN_RIGHT
					|| join->jointype == JOIN_IN ); /* Remove later. */
						
				/* Walk into subnodes */
				if ( plan_tree_walker(node, prescan_walker, context) )
					goto return_true;
				
				if ( finished )
					goto return_false;
					
				Assert( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED ); 
				
				switch (nodeTag(node))
				{
					case T_NestLoop:
						/* Sets its own error information, if necessary. */
						if (prescan_nestloop(plan, context))
                            goto return_true;
                        goto return_false;
						
					case T_MergeJoin:
						/* Sets its own error information, if necessary. */
						if (prescan_mergejoin(plan, context))
                            goto return_true;
                        goto return_false;
						
					case T_HashJoin:
						/* Sets its own error information, if necessary. */
				        if (prescan_hashjoin(plan, context))
                            goto return_true;
                        goto return_false;
						
					default:
						break; /* Can't happen but compiler likes it. */
				}
			}
			break; /* Fall through to report error. */
		
		case T_SetOp:
			/*
			 * A SetOp node appears to have only a lefttree like this Lisp:
			 * (Sort (Append (List (SubQueryScan ...) (SubQueryScan ....)))).
			 * It does no projection.
			 * 
			 * TODO Implment special handling.
			 */
				
			detail = "Query involves a set or multiset operation.";
			break; /* Fall through to report error. */			 
		
		case T_Append:
			/*
			 * Append is used in several contexts, e.g. unions, inheritance
			 * sets, or, when ((Append*)node)->isTarget is true, to hold
			 * the sequence of tables to be updated or deleted.
			 */
            if ( plan_tree_walker(node, prescan_walker, context) )
                goto return_true;
            if (finished)
                goto return_false;
			detail = "Query involves multiset union.";
			break; /* Fall through to report error. */
		
		case T_Agg:
			{
				/* First prescan children */
				if ( plan_tree_walker(node, prescan_walker, context) )
					goto return_true;
				
				if ( finished )
					goto return_false;
					
				if ( !prescan_agg((Plan*)node, context) )
					goto return_false;
				detail = "Query involves complex aggregation.";
			}
			break; /* Fall through to report error. */
		
		case T_Group:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				goto return_true;
			if ( finished )
				goto return_false;
				
			{
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				Assert( child != NULL && child->flow != NULL );
				
				if ( child->flow->flotype == FLOW_REPLICATED
					|| child ->flow->flotype == FLOW_SINGLETON )
				{
					/* No motion required so inherit flow characteristics. */
					plan->flow = pull_up_Flow(plan, child, true);
						
					if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
					{
						detail = "Can't characterize flow from Group node.";
						break;
					}
					goto return_false;
				}
			}
			/* TODO Deal with partitioned aggregration. */
			detail = "Query involves partitioned grouping.";
			break; /* Fall through to report error. */
		
		case T_Limit:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				goto return_true;
			if ( finished )
				goto return_false;
			
			{
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				Assert( child != NULL && child->flow != NULL );
				
				if ( child->flow->flotype == FLOW_REPLICATED
					|| child ->flow->flotype == FLOW_SINGLETON )
				{
					/* No motion required so inherit flow characteristics. */
					plan->flow = pull_up_Flow(plan, child, true);
						
					if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
					{
						detail = "Can't characterize flow from Limit node.";
						break;
					}
					goto return_false;
				}
			}
			/*  Deal with partitioned LIMIT and OFFSET. 
			 */
			 
			/* There are currently a number of restrictions due to
			 * inadequacies in our implementation or understanding.
			 */
			{
				Limit *limit = (Limit*)node;
				
				if ( (limit->limitOffset != NULL && !IsA(limit->limitOffset, Const))
					|| (limit->limitCount != NULL && !IsA(limit->limitCount, Const)) )
				{
					detail ="LIMIT offset or count not a constant";
					break; /* Fall through to report error. */
				}
	
				if ( limit->plan.lefttree->flow->numSortCols < 1 )
				{
					detail = "LIMIT sort order uncertain";
					break; /* Fall through to report error. */
				}

				/* Specify restructuring.  The resulting flow will retain
				 * any sort specified in the input plan, but will become
				 * singleton.  The restructuring is flagged by the xform
				 * movement request. */
				limit->plan.flow = pull_up_Flow((Plan*)limit, limit->plan.lefttree, true);
					
				if ( limit->plan.flow == NULL || limit->plan.flow->flotype == FLOW_UNDEFINED )
				{
					detail = "Sort criteria missing from Limit node.";
					break;
				}
				limit->plan.flow->flotype = FLOW_SINGLETON;
				limit->plan.flow->req_move = MOVEMENT_NONE;
				limit->plan.flow->segindex = -1;
				limit->plan.flow->hashExpr = NIL;
				limit->plan.flow->req_xform = MOVEMENT_LIM_RESTRUCT;
			}
			goto return_false;

		default:
			/* Let plan_tree_walker() deal with other node types. */
			if (plan_tree_walker(node, prescan_walker, context))
                goto return_true;
            goto return_false;

	}

	 /* Breaking out of the above switch will setup an error report
	  * with whatever detail was set above. 
	  */
	 psErrorNonce(context, detail);

return_true:
    context->local->finished = parent_finished;
    return true;
return_false:
    context->local->finished = parent_finished;
    return false;
}


/* Helper for prescan_walker() does processing for a HashJoin.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the join.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_hashjoin(Plan *plan, PlanProfile *context)
{	
	List *strategies;
	ListCell *cell;
	CdbJoinStrategy *strategy = NULL;

	List *outerexpr = NULL;
	List *innerexpr = NULL;
	
	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);
	
	/* The expected pattern is:
	 *
	 *          plan 
	 *       (HashJoin)
	 *        /     \ 
	 *     outer   inner
	 *    (Plan)   (Hash)
	 *               |              
	 *          inner->lefttree 
	 *             (Plan)
	 */
	Assert( IsA(plan, HashJoin) );
	Assert( IsA(inner, Hash) );
	Assert( is_plan_node((Node*)(inner->lefttree)) );
	Assert( is_plan_node((Node*)outer) );
	
	
	/* Get a list of possible strategies sorted by cost and 
	 * use the first strategy that we can actually implement
	 * for a hash join in this context. 
	 */
	strategies = getJoinStrategyList((Join*)plan);
	
	foreach ( cell, strategies )
	{
		bool canUse;
		CdbJoinStrategy *s = (CdbJoinStrategy*)lfirst(cell);
		
		/* Pre-flight check the outer relation. */
		switch ( s->leftmove )
		{
			case MOVEMENT_NONE:
				canUse = true;
				break;
				
			case MOVEMENT_FOCUS:
				canUse = focusPlan(plan->lefttree, false, false, false, 0);
				break;
				
			case MOVEMENT_BROADCAST:
				canUse = broadcastPlan(plan->lefttree, false, false, false);
				break;
				
			case MOVEMENT_REPARTITION:
				outerexpr = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
				canUse = repartitionPlan(plan->lefttree, false, false, false, outerexpr);
				break;
				
			default:
				canUse = false;
				break;
		}
		
		if ( !canUse )
			continue; /* next strategy */
		
		/* Pre-flight check the inner relation. */
		switch ( s->rightmove )
		{
			case MOVEMENT_NONE:
				canUse = true;
				break;
				
			case MOVEMENT_FOCUS:
				canUse = focusPlan(plan->righttree, false, false, false, 0);
				break;
				
			case MOVEMENT_BROADCAST:
				canUse = broadcastPlan(plan->righttree, false, false, false);
				break;
				
			case MOVEMENT_REPARTITION:
				innerexpr = getHashMotionInfoList(plan, INFOTYPE_RIGHTEXPR);
				canUse = repartitionPlan(plan->righttree, false, false, false, innerexpr);
				break;
				
			default:
				canUse = false;
				break;
		}
		
		if ( canUse )
		{
			strategy = copyJoinStrategy(s);
			break;
		}
		
		/* Else, next strategy. */
	}
	
	/* Clean up */
	if ( strategies != NIL )
	{
		freeJoinStrategyList(strategies);
		strategies = NIL;
	}
	
	/* If we didn't find a useable strategy, bail out. */
	if ( strategy == NULL )
	{
		psErrorNonce(context, "No strategy available to parallelize that hash join.");
		/* Hint: Try again with set enable_hashjoin = false */
		return true;	 
	}
	

	/*
	 * Annotate the outer plan by updating Flow nodes as necessary.
	 */
	switch ( strategy->leftmove )
	{
		case MOVEMENT_NONE:
			break;
			
		case MOVEMENT_FOCUS:
			focusPlan(plan->lefttree, true, false, false, 0);
			break;
			
		case MOVEMENT_BROADCAST:
			broadcastPlan(plan->lefttree, true, false, false);
			break;
			
		case MOVEMENT_REPARTITION:
			repartitionPlan(plan->lefttree, true, false, false, outerexpr);
			break;
			
		default:
			Assert(false);
			break;
	}
	
	/*
	 * Annotate the inner plan by updating Flow nodes as necessary.
	 */
	switch ( strategy->rightmove )
	{
		case MOVEMENT_NONE:
			break;
			
		case MOVEMENT_FOCUS:
			focusPlan(plan->righttree, true, false, false, 0);
			break;
			
		case MOVEMENT_BROADCAST:
			broadcastPlan(plan->righttree, true, false, false);
			break;
			
		case MOVEMENT_REPARTITION:
			repartitionPlan(plan->righttree, true, false, false, innerexpr);
			break;
			
		default:
			Assert(false);
			break;
	}
	
	/*
	 * Annotate this HashJoin plan -- attach a Flow describing the result
	 * of adopting the proposed strategy. 
	 *
	 * TODO It might be a good idea to try to track whether the join
	 *      will preserve sort order or partitioning (or even to add
	 *      junk attributes to facilitate it), but for now we track
	 *      only flow type.
	 */
	plan->flow = makeFlow(strategy->resultflowtype);
	return false;	 
}



/* Helper for prescan_walker() does processing for a MergeJoin.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the join.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_mergejoin(Plan *plan, PlanProfile *context)
{	
	List *strategies;
	ListCell *cell;
	CdbJoinStrategy *strategy = NULL;
	
	List *outerexpr = NULL;
	List *innerexpr = NULL;

	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);
	
	/* This is the expected pattern:
	 *
	 *          plan 
	 *       (MergeJoin)
	 *        /      \ 
	 *     outer    inner
	 *    (Plan)    (Plan)
	 *    ordered   ordered       
	 *              rescan (via mark,restore)               
	 */
	Assert( IsA(plan, MergeJoin) );	
	Assert( is_plan_node((Node*)inner) && inner->flow != NULL );
	Assert( is_plan_node((Node*)outer) && outer->flow != NULL );

	/* Get a list of possible strategies sorted by cost and 
	 * use the first strategy that we can actually implement
	 * for a hash join in this context. 
	 */
	strategies = getJoinStrategyList((Join*)plan);
	
	foreach( cell, strategies)
	{
		bool canUse;
		CdbJoinStrategy *s = (CdbJoinStrategy*)lfirst(cell);
		
		/* Make sure we have ordering information for any argument we
		 * need to apply motion to. 
		 */
		if ( s->leftmove != MOVEMENT_NONE && outer->flow->numSortCols < 1 )
			continue;
		if ( s->rightmove != MOVEMENT_NONE && inner->flow->numSortCols < 1 )
			continue;
	
		/* Pre-flight check the outer relation. */
		switch ( s->leftmove )
		{
			case MOVEMENT_NONE:
				canUse = true;
				break;
				
			case MOVEMENT_FOCUS:
				canUse = focusPlan(plan->lefttree, false, true, false, 0);
				break;
				
			case MOVEMENT_BROADCAST:
				canUse = broadcastPlan(plan->lefttree, false, true, false);
				break;
				
			case MOVEMENT_REPARTITION:
				outerexpr = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
				canUse = repartitionPlan(plan->lefttree, false, true, false, outerexpr);
				break;
				
			default:
				canUse = false;
				break;
		}
		
		if ( !canUse )
			continue; /* next strategy */
		
		/* Pre-flight check the inner relation. */
		switch ( s->rightmove )
		{
			case MOVEMENT_NONE:
				canUse = true;
				break;
				
			case MOVEMENT_FOCUS:
				canUse = focusPlan(plan->righttree, false, true, true, 0);
				break;
				
			case MOVEMENT_BROADCAST:
				canUse = broadcastPlan(plan->righttree, false, true, true);
				break;
				
			case MOVEMENT_REPARTITION:
				innerexpr = getHashMotionInfoList(plan, INFOTYPE_RIGHTEXPR);
				canUse = repartitionPlan(plan->righttree, false, true, true, innerexpr);
				break;
				
			default:
				canUse = false;
				break;
		}
		
		if ( canUse )
		{
			strategy = copyJoinStrategy(s);
			break;
		}
		
		/* Else, next strategy. */
	}		

	/* Clean up */
	if ( strategies != NIL )
	{
		freeJoinStrategyList(strategies);
		strategies = NIL;
	}
	
	/* If we didn't find a useable strategy, bail out. */
	if ( strategy == NULL )
	{
		psErrorNonce(context, "No strategy available to parallelize that merge join.");
		/* Hint: Try again with set enable_mergejoin = false */
		return true;	 
	}
	
	/*
	 * Annotate the outer plan by updating Flow nodes as necessary.
	 */
	switch ( strategy->leftmove )
	{
		case MOVEMENT_NONE:
			break;
			
		case MOVEMENT_FOCUS:
			focusPlan(plan->lefttree, true, true, false, 0);
			break;
			
		case MOVEMENT_BROADCAST:
			broadcastPlan(plan->lefttree, true, true, false);
			break;
			
		case MOVEMENT_REPARTITION:
			repartitionPlan(plan->lefttree, true, true, false, outerexpr);
			break;
			
		default:
			Assert(false);
			break;
	}
	
	/*
	 * Annotate the inner plan by updating Flow nodes as necessary.
	 */
	switch ( strategy->rightmove )
	{
		case MOVEMENT_NONE:
			break;
			
		case MOVEMENT_FOCUS:
			focusPlan(plan->righttree, true, true, true, 0);
			break;
			
		case MOVEMENT_BROADCAST:
			broadcastPlan(plan->righttree, true, true, true);
			break;
			
		case MOVEMENT_REPARTITION:
			repartitionPlan(plan->righttree, true, true, true, innerexpr);
			break;
			
		default:
			Assert(false);
			break;
	}
	
	/*
	 * Annotate this MergeJoin plan -- attach a Flow describing the result
	 * of adopting the proposed strategy. 
	 *
	 * TODO It might be a good idea to try to track whether the join
	 *      will preserve sort order or partitioning (or even to add
	 *      junk attributes to facilitate it), but for now we track
	 *      only flow type.
	 */
	plan->flow = makeFlow(strategy->resultflowtype);
	return false;	 
}



/* Helper for prescan_walker() does processing for a NestLoop join.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the join.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_nestloop(Plan *plan, PlanProfile *context)
{	
	List *strategies;
	ListCell *cell;
	CdbJoinStrategy *strategy = NULL;

	List *outerexpr = NIL;
	List *innerexpr = NIL;
	List *hashExpr = NIL;
	bool attemptedHashExtract = false;

	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);	
			
	/* There are two expected patterns:
	 *
	 *          plan 
	 *       (NestLoop)
	 *        /     \ 
	 *     outer   inner
	 *    (Plan) (IndexScan)
	 *  
	 *
	 *          plan 
	 *       (NestLoop)
	 *        /     \ 
	 *     outer   inner
	 *    (Plan)   (Plan)
	 *             rescan
	 *
	 * The first pattern is special because the NestLoop passes its
	 * context down to IndexScan which uses it to obtain the index
	 * key.  Therefore, we can't insert a motion in between.
	 *
	 * In the second pattern the inner plan will usually be a Material
	 * node but we can't assume this. The real requirement is that it
	 * be rescanable.
	 *			
	 * TODO: Need a way to communicate (in a Flow) to the mutator
	 *       that it should add a Material node above an added Motion.
	 *       This would avoid the special requirement.  Note that we
	 *       eliminate strategies that don't match the pattern in
	 *       the filter stage above, so the Assert should never trip.
	 */
	Assert( IsA(plan, NestLoop) && plan->flow == NULL );
	Assert( is_plan_node((Node*)inner) && inner->flow != NULL );
	Assert( is_plan_node((Node*)outer) && outer->flow != NULL );
	
	
	/* Get a list of possible strategies sorted by cost and 
	 * use the first strategy that we can actually implement
	 * for a hash join in this context. 
	 */
	strategies = getJoinStrategyList((Join*)plan);
	
	foreach( cell, strategies)
	{
		bool canUse;
		CdbJoinStrategy *s = (CdbJoinStrategy*)lfirst(cell);
		
		
		/*
		 * If the strategy requires repartitioning, we can only do it if we
		 * can extract hashing information from the joinqual.
		 */
		if ( s->leftmove == MOVEMENT_REPARTITION || s->rightmove == MOVEMENT_REPARTITION )
		{
			if ( hashExpr == NIL && ! attemptedHashExtract )
			{
				ListCell *xc;
				/* Try to extract hash criteria from joinqual. */
				if ( extractHashExprFromQual( ((Join*)plan)->joinqual, &hashExpr ))
				{
					/* Repartitioning is possible, dissect expressions. */
					foreach ( xc, hashExpr )
					{
						OpExpr *op = (OpExpr*)lfirst(xc);
						Assert( list_length(op->args) == 2 );
						outerexpr = lappend(outerexpr, linitial(op->args));
						innerexpr = lappend(innerexpr, lsecond(op->args));
					}
				}
				else
				{
					hashExpr = NIL;  /* Just to be sure. */
				}
				attemptedHashExtract = true;
            }
			
			if ( hashExpr == NIL )
				continue; /* Not hashable, so can't use this strategy. */
		}
		
		/*
		 * If the inner relation is an IndexScan, the NestLoop is tightly 
		 * coupled to it (needs to pass index keys on ExecReScan calls) 
		 * so we can't insert any motion in between.
		 */
		 if ( IsA(inner, IndexScan) && s->rightmove != MOVEMENT_NONE )
			continue;
			
	
		/* Pre-flight check the outer relation. */
		switch ( s->leftmove )
		{
			case MOVEMENT_NONE:
				canUse = true;
				break;
				
			case MOVEMENT_FOCUS:
				canUse = focusPlan(plan->lefttree, false, false, false, 0);
				break;
				
			case MOVEMENT_BROADCAST:
				canUse = broadcastPlan(plan->lefttree, false, false, false);
				break;
				
			case MOVEMENT_REPARTITION:
				canUse = repartitionPlan(plan->lefttree, false, false, false, outerexpr);
				break;
				
			default:
				canUse = false;
				break;
		}
		
		if ( !canUse )
			continue; /* next strategy */
		
		/* Pre-flight check the inner relation. */
		switch ( s->rightmove )
		{
			case MOVEMENT_NONE:
				canUse = true;
				break;
				
			case MOVEMENT_FOCUS:
				canUse = focusPlan(plan->righttree, false, false, true, 0);
				break;
				
			case MOVEMENT_BROADCAST:
				canUse = broadcastPlan(plan->righttree, false, false, true);
				break;
				
			case MOVEMENT_REPARTITION:
				canUse = repartitionPlan(plan->righttree, false, false, true, innerexpr);
				break;
				
			default:
				canUse = false;
				break;
		}
		
		if ( canUse )
		{
			strategy = copyJoinStrategy(s);
			break;
		}
		
		/* Else, next strategy. */
	}
	
	/* Clean up */
	if ( strategies != NIL )
	{
		freeJoinStrategyList(strategies);
		strategies = NIL;
	}
	
	/* If we didn't find a useable strategy, bail out. */
	if ( strategy == NULL )
	{
		psErrorNonce(context, "No strategy available to parallelize that nested-loop join.");
		/* Hint: Try again with set enable_nestloop = false */
		return true;	 
	}
	
	/*
	 * Annotate the outer plan by updating Flow nodes as necessary.
	 */
	switch ( strategy->leftmove )
	{
		case MOVEMENT_NONE:
			break;
			
		case MOVEMENT_FOCUS:
			focusPlan(plan->lefttree, true, false, false, 0);
			break;
			
		case MOVEMENT_BROADCAST:
			broadcastPlan(plan->lefttree, true, false, false);
			break;
			
		case MOVEMENT_REPARTITION:
			repartitionPlan(plan->lefttree, true, false, false, outerexpr);
			break;
			
		default:
			Assert(false);
			break;
	}
	
	
	/*
	 * Annotate the inner plan by updating Flow nodes as necessary.
	 */
	switch ( strategy->rightmove )
	{
		case MOVEMENT_NONE:
			break;
			
		case MOVEMENT_FOCUS:
			focusPlan(plan->righttree, true, false, true, 0);
			break;
			
		case MOVEMENT_BROADCAST:
			broadcastPlan(plan->righttree, true, false, true);
			break;
			
		case MOVEMENT_REPARTITION:
			repartitionPlan(plan->righttree, true, false, true, innerexpr);
			break;
			
		default:
			Assert(false);
			break;
	}
	
	/*
	 * Annotate this NestLoop plan -- attach a Flow describing the result
	 * of adopting the proposed strategy. 
	 *
	 * TODO It might be a good idea to try to track whether the join
	 *      will preserve sort order or partitioning (or even to add
	 *      junk attributes to facilitate it), but for now we track
	 *      only flow type.
	 */
    plan->flow = makeFlow(strategy->resultflowtype);
	return false;	 
}



/* Helper for prescan_walker() does processing for an Agg node.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the node.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_agg(Plan *plan, PlanProfile *context)
{	
	List *strategies;
	ListCell *cell;
	CdbAggStrategy *strategy = NULL;
	
	Agg *agg;
	Plan *child;
	int i;

	AttrNumber *sortattrs = NULL;
	Oid * sortoprs = NULL;
	Flow *newflow = NULL;
	
	List *hashExpr = NIL;
	
	/* Argument is a prescaned plan. */
	child = outerPlan((Node*)plan);
	Assert( is_plan_node((Node*)child) && child->flow != NULL  );
	agg = (Agg*)plan;
	
	
	/* If the node is of type AGG_SORTED, we need to preserve the sort order 
	 * of its input and output since an ancestor node may assume it.  If we
	 * can't gather the necessary information, we have to reject the plan as
	 * not currently parallelizable.
	 */
	if ( agg->aggstrategy == AGG_SORTED )
	{
		/* Get the attribute numbers of the grouping attributes in the 
		 * target list of the given Agg node.  These correspond to the 
		 * grouping attributes of the Agg node's child, which are recorded
		 * the sort fields of the Agg node, but they may differ in position.
		 *
		 * Note that, hereafter, (sortattrs != NULL) flags an AGG_SORTED. 
		 */
		bool gotSortInfo;
		
		/* Translate child node attribute numbers of the sort attributes 
		 * into attribute numbers in the targetlist of this Agg node. 
		 * Get sort operators from the child flow (and check assumption
		 * that the attribute lists match).
		 */
		gotSortInfo = invertGrpColIdx(agg, &sortattrs);

		if ( gotSortInfo && child->flow != NULL && child->flow->numSortCols >= agg->numCols )
		{
			sortoprs = (Oid*)palloc0(agg->numCols*sizeof(Oid));
			for ( i = 0; i < agg->numCols; i++ )
			{
				if ( agg->grpColIdx[i] != child->flow->sortColIdx[i] )
				{
					gotSortInfo = FALSE;
					break; /* for loop */
				}
				sortoprs[i] = child->flow->sortOperators[i];
			}
		}
		if ( !gotSortInfo )
		{
			if ( sortattrs != NULL )
			{
				pfree(sortattrs);
				sortattrs = NULL;
			}
			if ( sortoprs != NULL )
			{
				pfree(sortoprs);
				sortoprs = NULL;
			}
			psErrorNonce(context, "Ordering for sorted aggregation unknown");
			return true;
		}
	}
	
	
	/* Get a list of possible strategies and use the first strategy 
	 * that we can actually implement for this Agg in this context. 
	 */
	strategies = getAggStrategyList(agg);

	foreach( cell, strategies)
	{
		bool canUse;
		CdbAggStrategy *s = (CdbAggStrategy*)lfirst(cell);
		
		/* Pre-flight check required changes to the child. */
		switch ( s->leftmove )
		{
			case MOVEMENT_NONE:
			case MOVEMENT_AGG_RESTRUCT:
				canUse = true;
				break;
				
			case MOVEMENT_FOCUS:
				canUse = focusPlan(child, false, sortattrs!=NULL, false, -1);
				break;
				
			case MOVEMENT_BROADCAST:
				canUse = broadcastPlan(child, false, sortattrs!=NULL, false);
				break;
				
			case MOVEMENT_REPARTITION:
				hashExpr = getExprListFromTargetList(child->targetlist, agg->numCols, agg->grpColIdx);
				canUse = repartitionPlan(child, false, sortattrs!=NULL, false, hashExpr);
				break;
				
			default:
				canUse = false;
				break;
		}
		
		if (canUse)
		{
			strategy = copyAggStrategy(s);
			break; /* foreach */
		}
	}
	
	/* Clean up */
	if ( strategies != NIL )
	{
		freeAggStrategyList(strategies);
		strategies = NIL;
	}
	
	/* If we didn't find a useable strategy, bail out. */
	if ( strategy == NULL )
	{
		psErrorNonce(context, "Cannot find aggregation stategy");
		return true;	 
	}
	
	/*
	 * A strategy has been chosen. We need to adjust the flow on the
	 * input (child) node to suit the needs of the strategy and create
	 * a new flow for the given (Agg) node based on the result of the
	 * strategy.
	 */
	switch ( strategy->leftmove )
	{
		case MOVEMENT_NONE:
			/* No motion required so inherit flow characteristics. */
			newflow = pull_up_Flow(plan, plan->lefttree, true);
			break;
		
		case MOVEMENT_FOCUS:
			/* Focus motion required */
			focusPlan(child, true, sortattrs!=NULL, false, -1);
			newflow = makeFlow(FLOW_SINGLETON);
			break;
			
		case MOVEMENT_BROADCAST: 
			/* Broadcast motion required. */
			broadcastPlan(child, true, sortattrs!=NULL, false);
			newflow = makeFlow(FLOW_REPLICATED);
			break;
			
		case MOVEMENT_REPARTITION:
			/* Repartition motion required */
			repartitionPlan(child, true, sortattrs!=NULL, false, hashExpr);
			newflow = pull_up_Flow(plan, plan->lefttree, true);
			break;
		
		case MOVEMENT_AGG_RESTRUCT:
			newflow = makeFlow(strategy->resultflowtype);
			
			/* Flag the required transformation.  */
			newflow->req_xform = MOVEMENT_AGG_RESTRUCT;

			/* No child adjustments. */
			break;
		
		default:
			psErrorNonce(context, "Cannot implement aggregation strategy");
			return true;
	}

	
	/* Set the flow characteristics of the result based on the strategy
	 * we ultimately adopted and the flow characteristics of the child. 
	 */
	plan->flow = newflow;
	return false;	 
}


/* 
 * Helper for prescan() -- Write error info into a PlanProfile.
 */
void psError(PlanProfile *context, 
		unsigned code, 
		const char* message, 
		const char* detail)
{
	context->code = code;
	context->message = message;
	context->detail = detail;
}


/* 
 * Helper for prescan() -- Write error info into a PlanProfile.
 */
void psErrorNonce(PlanProfile *context,  const char* detail)
{
	context->code = ERRCODE_CDB_FEATURE_NOT_YET; 
	context->message = "CDB cannot parallelize that query yet.";
	context->detail = detail;
}


/* 
 * Helper for prescan() -- Write error info into a PlanProfile.
 */
void psErrorInternal(PlanProfile *context,  const char* detail)
{
	context->code = ERRCODE_CDB_INTERNAL_ERROR; 
	context->message ="Unexpected error parallelizing a query.";
	context->detail = detail;
}

/* 
 * Helper for prescan() -- Issue the error specified in a PlanProfile.
 */
void issuePsError(PlanProfile *context)
{
	if ( context->detail != NULL )
	{
		ereport(ERROR, (
			errcode(context->code),
			errmsg(context->message),
			errdetail(context->detail)
			));
	}
	else
	{
		ereport(ERROR, (
			errcode(context->code),
			errmsg(context->message)
			));
	}
	Assert( false ); /* Never return */
}


/*
 * Construct a new Flow in the current memory context.
 */
Flow *makeFlow(FlowType flotype)
{
	Flow *flow = makeNode(Flow);
	flow->flotype = flotype;
	flow->req_move = MOVEMENT_NONE;
	flow->req_xform = MOVEMENT_NONE;
	
	/* TODO
	 * Put this back to 0 and work out the problem
	 * with count(*) returning n-1 tuples with 0
	 * along with 1 tuple with the correct count
	 * on a select count(*) from foo query.
	 * Also, work out the AMS problem with
	 * the focus node (0) getting 1 more End of Stream notifications
	 * than there are segments, thereby causing an infinite loop.
	 * see cdbmotion.c in RecvTuple:
	 *     else if (pMNEntry->num_stream_ends_recvd == pMNEntry->num_senders)
	 * we were ending up with num_stream_ends_recvd == num_senders+1
	 */
	flow->segindex = -1;
	
	return flow;
}


/*
 * Create a flow for the given Plan node based on the flow in it's
 * lefttree (outer) plan.  Partitioning information is preserved for
 * non-projecting plan nodes. Sort specifications are preserved only
 * if withSort is true.
 */
Flow * pull_up_Flow(Plan *plan, Plan *subplan, bool withSort)
{
    Flow *model_flow;
    Flow *new_flow; 

    Assert(subplan);
    
    model_flow = subplan->flow;
	
	if ( model_flow == NULL )
		return NULL;
		
	new_flow = makeFlow(model_flow->flotype);
	
	if ( model_flow->flotype == FLOW_PARTITIONED && !is_projection_capable_plan(plan) )
	{
		/* Propagate hash attribute definitions. */
		new_flow->hashExpr = model_flow->hashExpr;
	}
	else if ( model_flow->flotype == FLOW_SINGLETON )
	{
		/* Propagate segment definitions. */
		new_flow->segindex = model_flow->segindex;
	}
	
	if ( withSort && model_flow->numSortCols >  0 )
	{		
		if ( is_projection_capable_plan(plan) )
		{
			/* Try to derive a sortColIdx to align with projected targetlist. */
			if ( ! invertColIdx(
					plan->targetlist, 
					model_flow->numSortCols, 
					model_flow->sortColIdx,
					&(new_flow->sortColIdx) )
				)
				return NULL;
		}
		else
		{
			/* Non-projecting so sort attributes are the same. */
			ARRAYCOPY(new_flow->sortColIdx, model_flow->sortColIdx, model_flow->numSortCols * sizeof(AttrNumber));
		}
		
		new_flow->numSortCols = model_flow->numSortCols;
		ARRAYCOPY(new_flow->sortOperators, model_flow->sortOperators, model_flow->numSortCols * sizeof(Oid));
	}
	
	return new_flow;
}


/*
 * Is the node a "subclass" of Plan?
 */
bool is_plan_node(Node* node)
{
	if ( node == NULL )
		return false;
		
	switch ( nodeTag(node) )
	{
		case T_Result:
		case T_Append:
		case T_SeqScan:
		case T_IndexScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
		case T_Material:
		case T_Sort:
		case T_Group:
		case T_Agg:
		case T_Unique:
		case T_Hash:
		case T_SetOp:
		case T_Limit:
		case T_Motion:
			return true;
			
		default: 
			break;
	}
	return false;
}


/* ----------------------------------------------------------------------- *
 * Functions scanForManagedTables() and scanForManagedTables_walker() 
 * use the plan_tree_walk() framework to visit the nodes of a plan tree 
 * to see if there are any managed tables referenced in any of the
 * plan scan nodes.
 *
 * The plan and query arguments should not be null.
 *
 * Returns true if the there exists at least one managed table referenced 
 * by the Plan, false otherwise.
 *
 * If an error is encountered, an ereport is issued directly.
 *
 * The purpose of scanForManagedTables is to do a quick scan to see whether 
 * there are no managed tables, in which case we can skip the parallelization
 * step.  Without this, we may generate errors during  parallelization that
 * will erroneously prevent catalog queries from running.
 * ----------------------------------------------------------------------- *
 */
bool
scanForManagedTables(Plan *plan, Query *query)
{
	PlanProfile profile;

	/* Initialize the PlanProfile result. */
	if ( !initPlanProfile(&profile, query) )
		issuePsError(&profile);	

	/* initPlanProfile may have already found a managed table. */
	if ( profile.resultSegments )
		return true;

	/*
	 * Do the scanForManagedTables_walker.
	 */
	return scanForManagedTables_walker((Node*)plan, &profile);
}


/*
 * Function scanForManagedTables_walker() is the recursive core of
 * scanForManagedTables(). It is mainly interested in SeqScans, TidScans, 
 * and Index Scans since these are the nodes that reference base relations.  
 * For these, it 
 * - gets the relid of the scan, 
 * - gets the RangeTableEntry from the context rtable, 
 * - gets the oid of the relation in the scan,
 * - calls get_table_policy to see whether this oid represents a
 *   managed table.  
 *
 * If so, we can exit the walk by returning true, otherwise we use the
 * plan_walker framework to walk the appropriate child nodes.
 *
 * The other node type of interest SubPlan, since this changes the current
 * range table.
 * 
 * For nodes other than these, we just use plan_walker to walk the appropriate 
 * child nodes.
 *
 * This walker uses the same PlanProfile structure as prescan_walker().
 * The only information we use from context PlanProfile is the current
 * range table.  
 *
 */
bool 
scanForManagedTables_walker(Node *node, PlanProfile *context)
{	
	if ( node == NULL )
		return false;
	
    switch (nodeTag(node))
	{
		case T_SeqScan:
		case T_TidScan:
		case T_IndexScan:
			{
                Plan   *plan = (Plan*)node;
                if (plan->dispatch == DISPATCH_PARALLEL)
                    return true;
			}
            return false;

		case T_SubqueryScan:
			{
				/* SubqueryScans have their own range table.  (See 
				 * ExecInitSubqueryScan for details.) We save the current
				 * range table, substitute the SubqueryScan's range table 
				 * in the context before we descend into the fields of the 
				 * SubqueryScan and then put the original one back.
				 */
				RangeTblEntry *rte;
				Plan *plan = (Plan*)node;
				Scan *scan = (Scan*)plan;
				Index rtindex = scan->scanrelid;
				
				rte = rt_fetch(rtindex, localRangeTable(context));
				Assert(rte->rtekind == RTE_SUBQUERY);
				
				Assert( rte->subquery->rtable != NULL );
				pushLocals(context, rte->subquery->rtable);
				
				if ( plan_tree_walker(node, scanForManagedTables_walker, context) )
					return true;

				popLocals(context);

				return false;
			}

			break;

		case T_SubPlan:
			{
				/* SubPlans establish a local state for the contained plan,
				 * notably the range table.
				 */
				SubPlan    *subplan = (SubPlan*) node;

				/* Recur into the exprs list (as usual). */
				if (expression_tree_walker((Node *) subplan->exprs, scanForManagedTables_walker, context))
					return true;
				
				/* Set local state for the subplan. */
				pushLocals(context, subplan->rtable);
				
				/* Walk the subplan.  */
				if ( scanForManagedTables_walker((Node*)subplan->plan, context) )
					return true;
				
				/* Restore state for the parent plan. */
				popLocals(context);
				
				/* Recur into the args list. */
				if (expression_tree_walker((Node *) subplan->args, scanForManagedTables_walker, context))
					return true;
			}
			return false;

		default:
			/* Let plan_tree_walker() deal with other node types. */
			break;

	}

	return plan_tree_walker(node, scanForManagedTables_walker, context);
}

/* 
 * initPlanProfile
 * initializes the PlanProfile struct fields
 * and checks to see whether there is a result relation 
 * (for an insert, update, or delete statement)
 * and if so, includes it in the summary of relations touched.
 */
bool initPlanProfile(PlanProfile *context, Query *query)
{
	Assert( context != NULL );

	context->resultSegments = false;
	
	/* Initialize the current range table. */
	context->local = NULL;
	pushLocals(context, query->rtable);
	
	switch ( query->commandType )
	{
		case CMD_SELECT:
			break;
			
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			if ( query->resultRelation <= 0 || query->resultRelations != NIL )
			{
				/* Perhaps this should be an Assert(?) rather than an error. */
				psError(context,  ERRCODE_CDB_INTERNAL_ERROR,
					"Unexpected relation specified as target of modification.", 
					"CDB expects a single result relation without inheritance." );
				return false;
			}
			else
			{
				/* 
				 * Since this is a data modification command, include the 
				 * result relation in the summary of relations touched.
				 */
				Oid reloid;
				RangeTblEntry *rte;
				CdbPolicy *policy;
				
				Assert( query->resultRelation<=list_length(localRangeTable(context)) );
				rte = rt_fetch(query->resultRelation, localRangeTable(context));
				Assert( rte->rtekind == RTE_RELATION );
				reloid = rte->relid;
				policy = get_table_policy(reloid);
				
				if ( policy->ptype == POLICYTYPE_PARTITIONED )
					context->resultSegments = true;
				else if ( policy->ptype != POLICYTYPE_ENTRY )
				{
					psError(context,  ERRCODE_CDB_INTERNAL_ERROR,
						"Unexpected data distribution policy.", NULL );
					return false;
				}
				pfree(policy); /* Or should we cache this? */
			}
			break;
		
		default:
			psError(context,  ERRCODE_CDB_INTERNAL_ERROR,
				"CDB cannot parallelize unrecognized query type.", NULL );
			return false;
	}
	
	/* 
	 * Initialize the error fields of the context, in case some failure
	 * in the prescan neglects to set them.
	 */
	psError(context,  ERRCODE_CDB_INTERNAL_ERROR,
		"Unexpected failure parallelizing query.", NULL );

	return true;
}

void pushLocals(PlanProfile *profile, List* rtable)
{
	LocalProfile *newlocal = palloc(sizeof(LocalProfile));
	
	newlocal->rtable = rtable;
	newlocal->finished = false;
	newlocal->outerFrame = profile->local;
	
	profile->local = newlocal;
}

void popLocals(PlanProfile *profile)
{
	LocalProfile *oldlocal = profile->local;
	
	if (oldlocal == NULL )
		return;
	
	profile->local = oldlocal->outerFrame;
	pfree(oldlocal);
}

List *localRangeTable(PlanProfile *profile)
{
	if ( profile->local == NULL )
		return NIL;
	return profile->local->rtable;
}

/* Has the plan in context at the given node been parallelized yet?
 * The node argument is only interesting if it's a Plan node and the
 * the context so far indicated "unfinished" in which case we check
 * for evidence of "finished" and update the context:
 *
 * - If the node is a Motion node, the branch must be finished.
 * - If the node has a defined Flow,  the branch must be finished.
 */
bool isPlanFinished(Node *node,  PlanProfile *profile)
{
	if ( is_plan_node(node) && !profile->local->finished )
	{
		Plan *plan = (Plan*)node; 
		profile->local->finished = node->type == T_Motion 
			|| (plan->flow != NULL && plan->flow->flotype != FLOW_UNDEFINED);
	}
	return profile->local->finished;
}

/* Functions focusPlan, broadcastPlan, and repartitionPlan annotate the top
 * of the given plan tree with a motion request for later implementation by
 * function apply_motion in cdbmutate.c.
 *
 * plan        the plan to annotate
 *
 * live        if false, don't actually make any change to the flow 
 *             specifications; just verify that they can be done.
 * 
 * stable      if true, the result plan must present the result in the
 *             order specified in the flow (which, presumably, matches
 *             the input order.
 *
 * rescanable  if true, the resulting plan must be rescanable, i.e., 
 *             it must be possible to use it like a scan. (Note that
 *             currently, no Motion nodes are rescanable.
 *	
 * Remaining arguments, if any, determined by the requirements of the specific
 * function.
 *
 * The result is true if the requested annotation could be made (or was made
 * in case live == true).  Else, false.
 */

/*
 * Function: focusPlan
 */
bool focusPlan(Plan* plan, bool live, bool stable, bool rescanable, int segidx)
{
	Flow target;
	
	if ( segidx < -1 || 0 < segidx )
		return false; /* Argument segindex must be -1 (QD) or 0 (some QE). */
	
	initTargetFlow(plan, &target, stable);
	target.flotype = FLOW_SINGLETON;
	target.segindex = segidx;
	
	return adjustPlanFlow(plan, &target, stable, rescanable, live);  
}


/*
 * Function: broadcastPlan
 */
bool broadcastPlan(Plan* plan, bool live, bool stable, bool rescanable)
{
	Flow target;
	
	initTargetFlow(plan, &target, stable);
	target.flotype = FLOW_REPLICATED;
	
	return adjustPlanFlow(plan, &target, stable, rescanable, live);  
}


/*
 * Function: repartitionPlan
 */
bool repartitionPlan(Plan* plan, bool live, bool stable, bool rescanable, List *hashExpr)
{
	Flow target;
	
	if ( list_length(hashExpr) < 1 )
		return false; /* Not much of a check, but it's something. */
	
	initTargetFlow(plan, &target, stable);
	target.flotype = FLOW_PARTITIONED;
	target.hashExpr = hashExpr;
	
	return adjustPlanFlow(plan, &target, stable, rescanable, live);  
}


/*
 * Helper Function: initTargetFlow
 *
 * Initialize a preallocated default Flow.
 */
void initTargetFlow(Plan *plan, Flow *target, bool stable)
{
	target->type = T_Flow;
	target->flotype = FLOW_SINGLETON;
	target->req_move = target->req_xform = MOVEMENT_NONE;
	target->segindex = 0;
	if ( stable )
	{
		target->numSortCols = plan->flow->numSortCols;
		ARRAYCOPY(target->sortColIdx,  
			plan->flow->sortColIdx, 
			plan->flow->numSortCols * sizeof(AttrNumber));
		ARRAYCOPY(target->sortOperators, 
			plan->flow->sortOperators, 
			plan->flow->numSortCols * sizeof(Oid));
	}
	else
	{
		target->numSortCols = 0;
		target->sortColIdx = NULL;
		target->sortOperators = NULL;
	}
	target->hashExpr = NIL;
}


/*
 * Helper Function: adjustPlanFlow
 *
 * Recursively pushes a motion request down the plan tree which allows us
 * to remove contraints in some cases.  For example, pushing a motion below
 * a Sort removes the stability constraint.
 */
bool adjustPlanFlow(Plan *plan, Flow *target, bool stable, bool rescanable, bool live)
{
	bool recur, r_stable, r_rescan;
	
	/* If we can't honor this request, return false. */
	if ( plan->flow == NULL )
		return false; /* Must have a flow. */
	if ( plan->flow->req_move != MOVEMENT_NONE || plan->flow->req_xform != MOVEMENT_NONE )
		return false; /* Must not already have motion or transform assigned. */
		
	/* If we can, recur to push the request further down the plan tree and
	 * possibly remove restrictions. */
	recur = false;
	r_stable = stable;
	r_rescan = rescanable;
	
	switch ( plan->type )
	{
		case T_Hash:
			/* Always push motion below Hash. */
			/* No need to preserve order or restartability below hash. */
			recur = true;
			r_stable = false;
			r_rescan = false;
			break;
			
		case T_Sort:
			if ( target->flotype == FLOW_SINGLETON && !rescanable )
			{
				/* We'd rather do a merge of a parallel sort, than do a
				 * sequential sort. A fixed motion can do that as long as
				 * we don't need to rescan.  
				 */
				break;
			}
			else
			{
				/* No need to preserve order or restartability below a sort. */
				recur = true;
				r_stable = false;
				r_rescan = false;
			}
			break;
			
		case T_Material:
			/* Always push motion below Material. */
			/* No need to preserve restartability below a materialization. */
			recur = true;
			r_rescan = false;
			break;
	
		case T_Append: /* Maybe handle specially some day. */
		default:
			recur = false;
			break;
	}
	
	/* If recurring, adjust flow of this plan node on return. */
	
	if ( recur && adjustPlanFlow(plan->lefttree, target, r_stable, r_rescan, live) )
	{
		if ( live )
		{
			switch ( target->flotype )
			{
				case FLOW_SINGLETON:
					plan->flow->flotype = FLOW_SINGLETON;
					plan->flow->segindex = target->segindex;
					break;
				case FLOW_REPLICATED:
					plan->flow->flotype = FLOW_REPLICATED;
					break;
				case FLOW_PARTITIONED:
					plan->flow->flotype = FLOW_PARTITIONED;
					/* TODO - Can we fill in criteria? */
					break;
				default:
					return false; /* TODO */
			}
			
			if ( r_stable )
			{
				Flow * model_flow = plan->lefttree->flow;
				
				plan->flow->numSortCols = model_flow->numSortCols;
				
				if ( model_flow->numSortCols > 0 )
				{
					ARRAYCOPY(plan->flow->sortColIdx,  
						model_flow->sortColIdx, 
						model_flow->numSortCols * sizeof(AttrNumber));
					ARRAYCOPY(plan->flow->sortOperators, 
						model_flow->sortOperators, 
						model_flow->numSortCols * sizeof(Oid));
				}
				else
				{
					plan->flow->sortColIdx = NULL;
					plan->flow->sortOperators = NULL;
				}
			}
		}
		return  true;
	}
	
	/* Else this is the base case, so honor the request or fail. */
	
	if ( stable && plan->flow->numSortCols < 1 )
	{
		return false; /* No order so can't preserve one. */
	}
	
	if ( rescanable )
	{
		return false; /* Motion nodes aren't rescanable. */
	}
	
	switch ( target->flotype )
	{
		case FLOW_SINGLETON:
			return focusBasePlan(plan, target, stable, rescanable, live);	
				
		case FLOW_REPLICATED:
			if ( stable )
				return false; 
			return broadcastBasePlan(plan, target, stable, rescanable, live);
			
		case FLOW_PARTITIONED:
			if ( stable )
				return false; 
			return repartitionBasePlan(plan, target, stable, rescanable, live);
			
		case FLOW_UNDEFINED:
			break;
	}
	return false;
}	




/*
 * Helper Function: focusBasePlan
 *
 * Apply a motion request to focus the argument plan.
 */
bool focusBasePlan(Plan *plan, Flow *target, bool stable, bool rescanable, bool live)
{
	switch ( plan->flow->flotype )
	{
		case FLOW_SINGLETON:
			if ( plan->flow->segindex == 0 &&  target->segindex == -1 )
			 /* QE -> QD */
			 {
				if ( live )
				{
					plan->flow->req_move = MOVEMENT_FOCUS;
					plan->flow->flotype = FLOW_SINGLETON;
					plan->flow->segindex = target->segindex;
					/* Fixed motions preserve the specfied order. */
				}
				return true;
			}
			else if ( plan->flow->segindex == target->segindex )
			/* QE -> QE or QD -> QD */
			{
				return true; /* Bizarre, but ok. */
			}
			else
			/* QD -> QE */
			{
				return false; /* No motion *from* the QD is defined. */
			}
			break;
			
		case FLOW_REPLICATED:
			if ( stable || rescanable )
				return false; 
			return false; /* TODO How specify deep-six? */
			break;
			
		case FLOW_PARTITIONED:
			if ( rescanable )
				return false;
			if ( live )
			{
				plan->flow->req_move = MOVEMENT_FOCUS;
				plan->flow->flotype = FLOW_SINGLETON;
				plan->flow->segindex = target->segindex;
				/* Fixed motions preserve the order specified in the Flow. */
			}
			return true;
			break;
			
		case FLOW_UNDEFINED:
			break;
	}
	return false;
}


/*
 * Helper Function: broadcastBasePlan
 *
 * Apply a motion request to broadcast the argument plan.
 */
bool broadcastBasePlan(Plan *plan, Flow *target, bool stable, bool rescanable, bool live)
{
	switch ( plan->flow->flotype )
	{
		case FLOW_SINGLETON:
			if ( rescanable )
				return false;
			if ( plan->flow->segindex <  0 )
				return false; /* No motions from QD. */
			if ( live )
			{
				plan->flow->req_move = MOVEMENT_BROADCAST;
				plan->flow->flotype = FLOW_REPLICATED;
				/* Send of singleton preserves order. */
			}
			return true;
			
		case FLOW_REPLICATED:
			return true; /* Bizarre, but ok. */
			break;
			
		case FLOW_PARTITIONED:
			if ( stable || rescanable )
				return false; /* Can't preserve order in N-to-N motion.  */
			if ( live )
			{
				plan->flow->req_move = MOVEMENT_BROADCAST;
				plan->flow->flotype = FLOW_REPLICATED;
			}
			return true;
			
		case FLOW_UNDEFINED:
			break;
	}
	return false;
}


/*
 * Helper Function: repartitionBasePlan
 *
 * Apply a motion request to repartition the argument plan.
 */
bool repartitionBasePlan(Plan *plan, Flow *target, bool stable, bool rescanable, bool live)
{
	switch ( plan->flow->flotype )
	{
		case FLOW_SINGLETON:
			if ( rescanable )
				return  false;
			/* Send of singleton preserves order so stable is ok. */
			if ( plan->flow->segindex <  0 )
				return false; /* No motions from QD. */
			if ( live )
			{
				plan->flow->req_move = MOVEMENT_REPARTITION;
				plan->flow->flotype = FLOW_PARTITIONED;
				plan->flow->hashExpr = target->hashExpr;
			}
			return true;
			
		case FLOW_REPLICATED:
			if ( rescanable )
				return false; 
			/* Restriction on replicated preserves order so stable is ok. */
			return false; /* TODO How specify deep-six? MOVEMENT_REP2PARTITIONED */
			if ( live )
			{
				plan->flow->req_move = MOVEMENT_REP2PARTITIONED;
				plan->flow->flotype = FLOW_PARTITIONED;
				plan->flow->hashExpr = target->hashExpr;
			}
			return true;
			
		case FLOW_PARTITIONED:
			if ( stable || rescanable )
				return false; /* Can't preserve order in N-to-N motion.  */
			if ( live )
			{
				plan->flow->req_move = MOVEMENT_REPARTITION;
				plan->flow->flotype = FLOW_PARTITIONED;
				plan->flow->hashExpr = target->hashExpr;
			}
			return true;
			
		case FLOW_UNDEFINED:
			break;
	}
	return false;
}
