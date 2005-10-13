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
 * For Append node prescan: Strategies we might adopt as well as
 * states/transitions for the FSM used to derive the strategy.
 */
typedef enum AppendStrategy
{
	AFS_0 = 0,	/* Null (pseudo-identity) */
	AFS_D,		/* Singleton on entry database */
	AFS_S,		/* Singleton on segment (assumed same thruout plan) */
	AFS_Pa, 	/* Partitioned on same attributes as initial term. */
	AFS_P,		/* Partitioned anonymously. */
	AFS_R,		/* Replicated. */
	AFS_XP,		/* Transform to AFS_P by adjusting non-partitioned terms. */
	AFS_X,		/* Invalid, no transformation known */
	AFS_COUNT,
	AFS_CP		/* AFS_Pa, if atttributes match, else AFS_P */
} AppendStrategy;

/*
 * For Append node prescan: An FSM used to derive the strategy to
 * use for parallelizing an Append node.  See fn chooseAppendStrategy.
 */
static const AppendStrategy append_FSM[][AFS_COUNT] = 
{
	{	AFS_0,	AFS_D,	AFS_S,	AFS_Pa,	AFS_P,	AFS_R,	AFS_XP,	AFS_X },
	{ 	AFS_D,	AFS_D,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_X },
	{ 	AFS_S,	AFS_XP,	AFS_S,	AFS_P,	AFS_P,	AFS_XP,	AFS_XP,	AFS_X },
	{ 	AFS_Pa,	AFS_XP,	AFS_P,	AFS_CP,	AFS_P,	AFS_XP,	AFS_XP,	AFS_X },
	{ 	AFS_P,	AFS_XP,	AFS_P,	AFS_P,	AFS_P,	AFS_XP,	AFS_XP,	AFS_X },
	{ 	AFS_R,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_R,	AFS_XP,	AFS_X },
	{ 	AFS_XP,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_XP,	AFS_X },
	{ 	AFS_X,	AFS_X,	AFS_X,	AFS_X,	AFS_X,	AFS_X,	AFS_X,	AFS_X }
};



/*
 * Forward Declarations
 */
static bool prescan(Plan *plan, Query *query, PlanProfile *context);
static bool prescan_walker(Node *node, PlanProfile *context);

static bool prescan_agg(Plan *plan, PlanProfile *context);
static bool prescan_unique(Plan *plan, PlanProfile *context);
static bool prescan_append(Plan *plan, PlanProfile *context);
static bool prescan_group(Plan *plan, PlanProfile *context);

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
static bool focusPlan(Plan* plan, bool live, bool stable, bool rescannable, int segidx);
static bool broadcastPlan(Plan* plan, bool live, bool stable, bool rescannable);
static bool repartitionPlan(Plan* plan, bool live, bool stable, bool rescannable, List *hashExpr);
static bool adjustPlanFlow(Plan *plan, Flow *target, bool stable, bool rescannable, bool live);
static bool focusBasePlan(Plan *plan, Flow *target, bool stable, bool rescannable, bool live);
static bool broadcastBasePlan(Plan *plan, Flow *target, bool stable, bool rescannable, bool live);
static bool repartitionBasePlan(Plan *plan, Flow *target, bool stable, bool rescannable, bool live);

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
 * plan suitable for execution by MPP.
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

/* ------------------------------------------------------------------------- *
 * Function cdbparallelize_subquery() is an entry point for the planner.  
 *
 * Parallelize a single PostgreSQL sequential or partially parallel plan. 
 * The input plan is modified in that Flow nodes are attached to Plan nodes
 * in the input as appropriate.
 * 
 * The caller (planner) has the following responsibilities:
 * 
 * Memory context.
 * 
 * The argument Plan node must be suitable for use in a SubqueryScan 
 * node and the result Plan node should be installed in one.
 * 
 * The argument Query node must contain [a copy of] the range table 
 * used by the top-level Scans in the argument Plan node.
 * 
 * The argument Query node should have a commandType value of 
 * CMD_SELECT and should not specify a target (as would, e.g., 
 * an INSERT).
 * 
 * Various fields of the top node of the argument are assumed to be 
 * insignificant.  For example, nParamExec and dispatch are ignored.
 *
 * The input plan must be suitable to be parallelized.  E.g., don't call
 * this function with a plan that references catalog tables.
 * 
 * 
 * On successful return:
 * 
 * The result plan will be a "finished", annotated plan -- in 
 * particular, it will not yet contain any Motion nodes, only Flow 
 * nodes with instructions.
 * 
 * The result may or may not be at the same address as the input 
 * plan.  It may share nodes with the input plan.  (These are just 
 * defensive restrictions and may not be necessary.)
 * 
 * The flow field will be set to characterize the result of the 
 * subquery.  Note that "partitioned anonymously" is a possibility
 * and is represented by 
 *   flotype == FLOW_PARTITIONED
 *   hashExpr == NIL
 * 
 * The dispatch field of the resulting plan will not be set since it
 * assumed that that overall plan will specifiy DISPATCH_PARALLEL.
 * 
 * In case of failure, an error is thrown.
 * ----------------------------------------------------------------------- *
 */
Plan *
cdbparallelize_subquery( Plan* plan, Query *query )
{
	PlanProfile profile; /* Will be initialized by prescan(). */
	
	Assert( is_plan_node((Node*)plan) );	
	Assert( query != NULL && IsA(query, Query) );
	
	/* TODO Remove this when the implementation is ready to test.
	 *      Until then issue an error to avoid masking caller bugs
	 *      with bugs presumed to be in this untested support.
	ereport(ERROR, (
			errcode(ERRCODE_CDB_FEATURE_NOT_YET),
			errmsg("Cannot parallelize that query yet"),
			errdetail("Unsupported subquery")
			));
	 */

	/* Prescan the plan and attach Flow nodes to its Plan nodes. These
	 * nodes describe how to parallelize the plan.  We also accumulate
	 * summary information in the profile stucture.
	 */
	if ( !prescan(plan, query, &profile) )
		issuePsError(&profile);	
	
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
				 * "finished" indicator is cleared, so that when we recur to
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
		
		case T_SeqScan:
        case T_IndexScan:
		case T_TidScan:
		case T_FunctionScan:
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin:
        case T_Hash:
        case T_Motion:
			/* Node is part of a finished plan produced by the planner.  
             * Keep walking in search of subqueries with unfinished plans.
			 */
            Assert(finished);
            if ( plan_tree_walker(node, prescan_walker, context) )
                goto return_true;
            goto return_false;

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
                RangeTblEntry  *rte;
                SubqueryScan   *subqueryscan = (SubqueryScan*)node;
                Plan           *subplan;
				
				/* Make sure this is first time. */
				Assert( finished || plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED );
				
				/* Make sure we have a valid index. */
				Assert( 0<rtindex && rtindex<=list_length(localRangeTable(context)) );
				
				/* All scan types have a range table entry. */
				rte = rt_fetch(rtindex, localRangeTable(context));
				
				/* The entry type is related to the to the scan type. */
                Assert( rte->rtekind == RTE_SUBQUERY );

                /*
                 * Parallelize a Subquery in the FROM clause.
                 *
                 * TODO Is there any special handling needed for
                 *      parameters?
                 */
                subplan = subqueryscan->subplan;

                /* The subplan uses the range table of the
                 * Query node atttached to the subquery RTE.
                 * Also, the local context forces reexamination
                 * of the contained query.
                 */
                pushLocals(context, rte->subquery->rtable);
                if ( prescan_walker((Node*)subplan, context) )
                    goto return_true;
                popLocals(context);

                if ( !finished )
                {
                    plan->flow = pull_up_Flow(plan, subqueryscan->subplan, true);
                    Assert(plan->flow);
                    /* Don't use expressions from the wrong range table */
                    plan->flow->hashExpr = NIL;
					if ( plan->flow->flotype == FLOW_PARTITIONED )
					{
						plan->flow->hashExpr = getExprListForSubqueryScan(
							subqueryscan->subplan->targetlist,
							subqueryscan->subplan->flow->hashExpr );
					}

                    if ( ((Plan*)node)->flow == NULL || ((Plan*)node)->flow->flotype == FLOW_UNDEFINED )
                    {
                        detail = "Can't characterize flow from SubqueryScan.";
                        break; /* Fail */
                    }
                }
                goto return_false;
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
					detail = "Can't characterize flow from Material node.";
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
				
			if ( prescan_unique((Plan*)node, context) )
			{
				goto return_true; /* prescan_unique sets its own detail in context. */
			}
			goto return_false;
		
		case T_SetOp:
			/* A SetOp node has only a lefttree headed by a Sort as in
			 *
			 *   (Sort (Append (SubqueryScan ...) (SubqueryScan ...)))
			 *
			 * The Sort argument is often, but not always, an Append of
			 * SubqueryScan nodes.  SetOp is non-projecting.
			 */
			 
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				goto return_true;
			if ( finished )
				goto return_false;

			 /* Default failure message for this case. */
			detail = "Cannot parallelize a set or multiset operation.";
				
			 {
				Plan *plan = (Plan*)node;
				Plan *child = plan->lefttree;				
				
				switch ( (child->flow==NULL)? FLOW_UNDEFINED: child->flow->flotype )
				{
				case FLOW_SINGLETON:
				case FLOW_REPLICATED:
						/* No motion required so inherit flow characteristics. */
						plan->flow = pull_up_Flow(plan, child, true);
							
						if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
						{
							detail = "Cannot characterize output of set operation.";
							break;
						}
						goto return_false;
				
				case FLOW_PARTITIONED:
					/* I think this could be an assertion, but play it safe. */
					if ( ! IsA(child, Sort) )
					{
						ereport(ERROR, (
							errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("unexpected argument to set operator")
						));
					}
					/* Since SetOp and Sort are non-projecting we can directly
					 * compare the Sort's hash expressions to the expressions
					 * in SetOp's target list. If the partitioning attributes 
					 * of the Sort are a non-empty subset of the "duplicated"
					 * attributes of the SetOp, then the existing partition
					 * is suitable, else we need to repartition. In that case,
					 * we choose to use the entire "duplicated" set in the
					 * hope that it will be less skew than a subset.
					 */	
					{
						SetOp *op;
						List *hx;
						int n;
						bool isSubset = FALSE;
					
						n = list_length(child->flow->hashExpr);
						if ( n > 0 )
						{
							AttrNumber * attrs = palloc(n * sizeof(AttrNumber));
							isSubset = findHashAttrsInTargetList(plan->targetlist, child->flow->hashExpr, attrs);
							pfree(attrs);
						}
						if ( isSubset )
						{
							plan->flow = pull_up_Flow(plan, child, true);
							
						if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
						{
							detail = "Can't characterize flow from SetOp node.";
							break;
						}
						goto return_false;
						}
					
						/* Repartition on the dupColIdx attrs of the SetOp. */
						op = (SetOp*)plan;
						hx = getExprListFromTargetList(child->targetlist, op->numCols, op->dupColIdx);
						if ( repartitionPlan(child, true, true, false, hx ) )
						{
							plan->flow = pull_up_Flow(plan, child, true);
							if (plan->flow != NULL )
								goto return_false;
						}
					}
					detail = "Can't characterize flow into SetOp node.";
					break;
				
				default:
					detail = "Can't characterize flow in argument to set operation.";
					break; /* Fall through to report error. */
				}
			}
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
			if ( prescan_append((Plan*)node, context) )
			{
				goto return_true;
			}
			goto return_false;
			
			//detail = "Query involves multiset union.";
			//break; /* Fall through to report error. */
		
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
				
			if ( prescan_group((Plan*)node, context) )
			{
				goto return_true; /* prescan_group sets its own detail in context. */
			}
			goto return_false;
		
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
			
			/*  Deal with partitioned LIMIT and OFFSET.  */
			{
				Limit *limit = (Limit*)node;
				
				/* Specify restructuring.  The resulting flow will retain
				 * any sort specified in the input plan, but will become
				 * singleton.  The restructuring is flagged by the xform
				 * movement request. */
				limit->plan.flow = pull_up_Flow((Plan*)limit, limit->plan.lefttree, true);
					
				if ( limit->plan.flow == NULL || limit->plan.flow->flotype == FLOW_UNDEFINED )
				{
					detail = "Flow criteria missing from Limit node.";
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

/* Helper for prescan_walker() does processing for a Unique node.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the node.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_unique(Plan *plan, PlanProfile *context)
{
	Unique *unique = (Unique*)plan;
	Plan *child = outerPlan((Node*)plan); /* already scanned */
	bool prefix_condition = true;
	bool match_condition = true;
	int i;
	
	/* Argument is a prescaned plan. */
	Assert( is_plan_node((Node*)child) && child->flow != NULL  );
	
	Assert( child->flow );
	
	if ( child->flow->flotype == FLOW_REPLICATED
		|| child ->flow->flotype == FLOW_SINGLETON )
	{
		/* No motion required so inherit flow characteristics. */
		plan->flow = pull_up_Flow(plan, child, true);
			
		if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
		{
			psErrorNonce(context, "Can't characterize flow from Unique node.");
			return true;
		}
		return false;
	}
	
	/* The argument flow is partitioned and ordered.  We need to make sure
	 * that tuples are collocated on the attributes to be "uniqued" and that
	 * the result order is preserved.
	 */
	 
	/* The parser/planner guarantees that the unique attributes are a prefix
	 * of the input sort attributes.  We should check, however.
	 */
	 prefix_condition = ( unique->numCols <= child->flow->numSortCols );
	 for ( i = 0; prefix_condition && i < unique->numCols; i++ )
	 {
	 	prefix_condition = (unique->uniqColIdx[i] == child->flow->sortColIdx[i] );
	 }
	 if ( !prefix_condition )
	 {
	 	psErrorInternal(context, "Plan violates DISTINCT/ORDER BY prefix condition");
	 	return true;
	 }
	 
	/* If the existing partition does not match the unique attributes, we need
	 * to repartition on the unique attributes and preserve the sort order.
	 */
	match_condition = ( unique->numCols == list_length(child->flow->hashExpr) );
	if ( match_condition )
	{
		int tidx;
		Node *hexpr;
		Node *texpr;	
		ListCell *cell;
		
		i = 0;
		foreach (cell, child->flow->hashExpr)
		{
			tidx = unique->uniqColIdx[i];
			match_condition = ( tidx < list_length(plan->targetlist) );
			if ( ! match_condition )
				break; /* foreach */
			texpr = (Node*)((TargetEntry*)list_nth(plan->targetlist, tidx))->expr;
			hexpr = lfirst(cell);
			match_condition = ( IsA(hexpr,Var) && IsA(texpr,Var) 
				&& ((Var*)hexpr)->varattno == ((Var*)texpr)->varattno );
			if ( !match_condition )
				break; /* foreach */
			i++;
		}
	}
	if ( !match_condition )
	{
		List *hashExpr = getExprListFromTargetList(child->targetlist, unique->numCols, unique->uniqColIdx);
		 
		if ( !repartitionPlan(child, false, true, false, hashExpr) )
		{
			psErrorNonce(context, "Can't implement that partitioned duplicate removal");
			return true;
		 }
		repartitionPlan(child, true, true, false, hashExpr);
	}
	plan->flow = pull_up_Flow(plan,child,true);
	
	return false;
	
	psErrorNonce(context, "Query plan employs partitioned duplicate removal");
	return true;
}




/* Helper for prescan_walker() does processing for an Append node.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the node.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_append(Plan *plan, PlanProfile *context)
{
	ListCell *cell;
	int nattrs;
	AttrNumber *attrs;
	AppendStrategy cumstate;
	Plan *prototype;
	
	Append *append = (Append*)plan;
	
	Assert ( IsA(append, Append) && plan->flow == NULL );

	/*
	 * Choose an approach to parallelizing the Append node by examining the
	 * flow characteristics of each item in the list of argument plans.  The 
	 * approach uses a state machine (append_FSM) to increment the strategy 
	 * chosen so far by the "natural" stategy of the next item.
	 * 
	 * If the result strategy is AFS_D, AFS_S, AFS_R, or AFS_P, then no
	 * additional determination is required, and the Append can marked as
	 * singleton (on dispatcher or on segment), replicated, or anonymously 
	 * partitioned, respectively.
	 * 
	 * If the result strategy is AFS_Pa, the hash expressions of the first
	 * argument plan can be copied and used for the Append.
	 * 
	 * If the result strategy is AFS_XP, then a transformation can be
	 * applied to make the the Append partitioned, but should be deferred
	 * in case the Append is an argument of a Sort/SetOp and, therefore, 
	 * has an external (and at this point unknown) partitioning requirement.
	 * 
	 * If the result strategy is AFS_X, then no strategy was chosen.
	 */
	cumstate = AFS_0;
	attrs = NULL;
	nattrs = 0;
	 
	foreach ( cell, append->appendplans )
	{	
		AppendStrategy nxtstate;
		int nxtnattrs;
		AttrNumber *nxtattrs;
		
		Plan *nxt = (Plan*)lfirst(cell);
		
		/* For the next Append element plan (nxt), determine the state value
		 * by which to increment the state machine. 
		 */
		switch ( nxt->flow->flotype )
		{
		case FLOW_UNDEFINED:
			nxtstate = AFS_X;
			break;
			
		case FLOW_SINGLETON:
			if ( nxt->flow->segindex == -1 )
			{
				nxtstate = AFS_D;
			}
			else
			{
				Assert( nxt->flow->segindex == 0 );
				nxtstate = AFS_S;
			}
			break;
			
		case FLOW_REPLICATED:
			nxtstate = AFS_R;
			break;
			
		case FLOW_PARTITIONED:
			if ( nxt->flow->hashExpr == NIL )
			{
				nxtstate = AFS_P;
			}
			else
			{
				nxtnattrs = list_length(nxt->flow->hashExpr);
				nxtattrs = (AttrNumber*)palloc0( sizeof(AttrNumber) * nxtnattrs);
				if ( findHashAttrsInTargetList(nxt->targetlist, nxt->flow->hashExpr, nxtattrs) )
				{
					nxtstate = AFS_Pa;	
				}
				else
				{
					pfree(nxtattrs);
					nxtattrs = NULL;
					nxtnattrs = 0;
					nxtstate = AFS_P;
				}
			}
			break;
		}
		
		/* Calculate the cumulative state.
		 */
		cumstate = append_FSM[cumstate][nxtstate];
		
		
		if ( cumstate == AFS_Pa) 
		{
			Assert( nxtattrs != NULL && attrs == NULL && nxtnattrs == list_length(nxt->flow->hashExpr) );
			nattrs = nxtnattrs;
			attrs = nxtattrs;
			nxtnattrs = 0;
			nxtattrs = NULL;
		}
		else if ( cumstate == AFS_CP )
		{
			bool match;
			int i;
			Assert( nxtattrs != NULL && attrs != NULL  );
			match = ( nattrs == nxtnattrs );
			for ( i = 0; match && i < nattrs; i++ )
			{
				match = ( attrs[i] == nxtattrs[i] );
			}
			nxtnattrs = 0;
			nxtattrs = NULL;
			
			cumstate = match ? AFS_Pa : AFS_P;
		}
		
		Assert ( AFS_0 < cumstate && cumstate < AFS_COUNT );
	}
	if ( attrs != NULL )
		pfree(attrs);
	
	switch ( cumstate )
	{
	case AFS_D: /* Singleton on entry database */
		plan->flow = makeFlow(FLOW_SINGLETON);
		plan->flow->segindex = -1;
		break;
		
	case AFS_S: /* Singleton on segment (assumed same thruout plan) */
		plan->flow = makeFlow(FLOW_SINGLETON);
		plan->flow->segindex = 0;
		break;
		
	case AFS_Pa: /* Partitioned on same attributes as initial term. */
		plan->flow = makeFlow(FLOW_PARTITIONED);
		prototype = (Plan*)linitial(append->appendplans);
		plan->flow->hashExpr = copyObject(prototype->flow->hashExpr);
		break;
		
	case AFS_P: /* Partitioned anonymously. */
		plan->flow = makeFlow(FLOW_PARTITIONED);
		break;
		
	case AFS_R: /* Replicated. */
		plan->flow = makeFlow(FLOW_REPLICATED);
		break;
		
	case AFS_XP: /* Transform to AFS_P by adjusting non-partitioned terms. */
	case AFS_X:/* Invalid, no transformation known */
	default:
		psErrorNonce(context, "Terms of multiset union are not congruent.");
		return true;
	}
	return false;
}


/* Helper for prescan_walker() does processing for a Group node.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the node.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_group(Plan *plan, PlanProfile *context)
{
	Group *group = (Group*)plan;
	Plan *child = outerPlan((Node*)plan); /* already scanned */
	bool prefix_condition = true;
	bool match_condition = true;
	int i;
	
	/* Argument is a prescaned Group plan. */
	Assert( is_plan_node((Node*)child) && child->flow != NULL  );
	
	
	if ( child->flow->flotype == FLOW_PARTITIONED )
	{
		/* The argument flow is partitioned and ordered.  We need to make sure
		 * that tuples are collocated on the attributes to be grouped.  Note
		 * that, unlike the Unique node, the Group node is projecting. The 
		 * resulting partial order is preserved.
		 */
		 
		/* The parser/planner guarantees that the grouping attributes are a prefix
		 * of the input sort attributes.  We should check, however.
		 */
		 prefix_condition = ( group->numCols <= child->flow->numSortCols );
		 for ( i = 0; prefix_condition && i < group->numCols; i++ )
		 {
			prefix_condition = (group->grpColIdx[i] == child->flow->sortColIdx[i] );
		 }
		 if ( !prefix_condition )
		 {
			psErrorInternal(context, "Plan violates GROUP/ORDER BY prefix condition");
			return true;
		 }
		 
		/* If the input partition does not match the grouping attributes, we
		 * must repartition on the grouping attributes and preserve the sort order.
		 */
		match_condition = ( group->numCols == list_length(child->flow->hashExpr) );
		if ( match_condition )
		{
			AttrNumber *hattrs = palloc0(sizeof(AttrNumber)*group->numCols);
			match_condition = findHashAttrsInTargetList(child->targetlist, child->flow->hashExpr, hattrs);
			for ( i = 0; match_condition && i < group->numCols; i++ )
			{
				match_condition = (group->grpColIdx[i] == hattrs[i]);
				if ( !match_condition )
					break; /* for */
			}
			pfree(hattrs);
		}
		if ( !match_condition )
		{
			List *hashExpr = getExprListFromTargetList(child->targetlist, group->numCols, group->grpColIdx);
			 
			if ( !repartitionPlan(child, false, true, false, hashExpr) )
			{
				psErrorNonce(context, "Can't implement that partitioned grouping");
				return true;
			 }
			repartitionPlan(child, true, true, false, hashExpr);
		}
	}
	else if ( child->flow->flotype != FLOW_REPLICATED && child->flow->flotype != FLOW_SINGLETON )
	{
		psErrorInternal(context, "Uncharacterized flow into Group node.");
		return true;
	}
	/* Else, Flow is replicated or singleton, so no motion required.
	 * Just inherit flow characteristics. 
	 */

	plan->flow = pull_up_Flow(plan,child,true);
			
	if ( plan->flow == NULL || plan->flow->flotype == FLOW_UNDEFINED )
	{
		psErrorNonce(context, "Can't characterize flow from Group node.");
		return true;
	}	
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
	context->message = "Cannot parallelize that query yet.";
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
	flow->base_flotype = flotype;
	flow->req_move = MOVEMENT_NONE;
	flow->req_xform = MOVEMENT_NONE;
	
	/* TODO
	 * Change this to default to 0.  But first figure out why doing so
	 * causes 
	 *     explain select count(*) from util;
	 *     PANIC:  building Gang:  No primary segdb found!!!!
	 *     server closed the connection unexpectedly
	 *
	 * For that matter, why are we getting gangs set up for a mere
	 * EXPLAIN?
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
	
	if ( model_flow->flotype == FLOW_PARTITIONED  )
	{
		/* Propagate hash attribute definitions, if they are available 
		 * and the plan is non-projecting. Else, leave them NIL. */
		if ( model_flow->hashExpr != NULL && !is_projection_capable_plan(plan) )
		{
			new_flow->hashExpr = copyObject(model_flow->hashExpr);
		}
	}
	else if ( model_flow->flotype == FLOW_SINGLETON )
	{
		/* Propagate segment definitions. */
		new_flow->segindex = model_flow->segindex;
	}
	else if ( model_flow->flotype != FLOW_REPLICATED )
	{
		if ( new_flow->hashExpr )
			pfree(new_flow->hashExpr);
		pfree(new_flow);
		return NULL;
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
			{
				if ( new_flow->hashExpr )
					pfree(new_flow->hashExpr);
				pfree(new_flow);
				return NULL;
			}
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
		case T_Motion:
            return true;

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
					"Target must be a single result relation without inheritance." );
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
				MppPolicy *policy;
				
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
				"Cannot parallelize unrecognized query type.", NULL );
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
 * rescannable  if true, the resulting plan must be rescannable, i.e., 
 *             it must be possible to use it like a scan. (Note that
 *             currently, no Motion nodes are rescannable.
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
bool focusPlan(Plan* plan, bool live, bool stable, bool rescannable, int segidx)
{
	Flow target;
	
	if ( segidx < -1 || 0 < segidx )
		return false; /* Argument segindex must be -1 (QD) or 0 (some QE). */
	
	initTargetFlow(plan, &target, stable);
	target.flotype = FLOW_SINGLETON;
	target.segindex = segidx;
	
	return adjustPlanFlow(plan, &target, stable, rescannable, live);  
}


/*
 * Function: broadcastPlan
 */
bool broadcastPlan(Plan* plan, bool live, bool stable, bool rescannable)
{
	Flow target;
	
	initTargetFlow(plan, &target, stable);
	target.flotype = FLOW_REPLICATED;
	
	return adjustPlanFlow(plan, &target, stable, rescannable, live);  
}


/*
 * Function: repartitionPlan
 */
bool repartitionPlan(Plan* plan, bool live, bool stable, bool rescannable, List *hashExpr)
{
	Flow target;
	
	if ( list_length(hashExpr) < 1 )
		return false; /* Not much of a check, but it's something. */
	
	initTargetFlow(plan, &target, stable);
	target.flotype = FLOW_PARTITIONED;
	target.hashExpr = hashExpr;
	
	return adjustPlanFlow(plan, &target, stable, rescannable, live);  
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
bool adjustPlanFlow(Plan *plan, Flow *target, bool stable, bool rescannable, bool live)
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
	r_rescan = rescannable;
	
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
			if ( target->flotype == FLOW_SINGLETON && !rescannable )
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
					plan->flow->hashExpr = copyObject(plan->lefttree->flow->hashExpr);
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
	
	if ( rescannable )
	{
		return false; /* Motion nodes aren't rescannable. */
	}
	
	switch ( target->flotype )
	{
		case FLOW_SINGLETON:
			return focusBasePlan(plan, target, stable, rescannable, live);	
				
		case FLOW_REPLICATED:
			if ( stable )
				return false; 
			return broadcastBasePlan(plan, target, stable, rescannable, live);
			
		case FLOW_PARTITIONED:
			if ( stable )
				return false; 
			return repartitionBasePlan(plan, target, stable, rescannable, live);
			
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
bool focusBasePlan(Plan *plan, Flow *target, bool stable, bool rescannable, bool live)
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
			if ( stable || rescannable )
				return false; 
			return false; /* TODO How specify deep-six? */
			break;
			
		case FLOW_PARTITIONED:
			if ( rescannable )
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
bool broadcastBasePlan(Plan *plan, Flow *target, bool stable, bool rescannable, bool live)
{
	switch ( plan->flow->flotype )
	{
		case FLOW_SINGLETON:
			if ( rescannable )
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
			if ( stable || rescannable )
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
bool repartitionBasePlan(Plan *plan, Flow *target, bool stable, bool rescannable, bool live)
{
	switch ( plan->flow->flotype )
	{
		case FLOW_SINGLETON:
			if ( rescannable )
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
			if ( rescannable )
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
			if ( stable || rescannable )
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
