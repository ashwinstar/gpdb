/*-------------------------------------------------------------------------
 *
 * cdbllize.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2004, Metapa
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
#include "executor/nodeMotion.h"
#include "parser/parsetree.h" /* for rt_fetch() */
#include "cdb/cdbplan.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbjoin.h"

#define ARRAYCOPY(to, from, sz) \
	do { \
		Size	_size = (sz); \
		(to) = palloc(_size); \
		memcpy((to), (from), _size); \
	} while (0)

/* 
 * A PlanProfile holds state for recursive prescan_walker(). 
 */
typedef struct PlanProfile
{
	/* Summary status. */
	bool	hasPartitionedFlow;
	bool	hasLocalFlow;
	
	/* Context */
	List	*rtable; /* The current range table. */
	
	/* TODO Validate the assumption that plans don't have Var nodes
	 *      that refer to outer range tables (i.e., varlevelsup is
	 *      either 0 or ignored.  This would make it legitimate to
	 *      store only the current range table (as above) rather
	 *      than a stack of them. */
	
	/* Message data */
	unsigned code;
	const char *message;
	const char *detail;
	
} PlanProfile;


/*
 * An ApplyMotionState holds state for the recursive apply_motion_mutator().
 */
typedef struct ApplyMotionState
{
	int dummy;  
	
	/* TODO Finish the mutator which will eventually need some state:
	 *      At a minimum, a range table, and perhaps more.
	 */
} ApplyMotionState;


/*
 * Forward Declarations
 */
static bool prescan(Plan *plan, Query *query, PlanProfile *context);
static bool prescan_walker(Node *node, PlanProfile *context);
static bool prescan_hashjoin(Plan *plan, PlanProfile *context);
static bool prescan_mergejoin(Plan *plan, PlanProfile *context);
static bool prescan_nestloop(Plan *plan, PlanProfile *context);

static bool repartitionSort(Sort *sort, List *exprs, List *ops, PlanProfile *context);
static bool focusSort(Sort *sort, PlanProfile *context);
static bool broadcastSort(Sort *sort, PlanProfile *context);

static void psError(PlanProfile *context, unsigned code, const char* message, const char* detail);
static void psErrorNonce(PlanProfile *context,  const char* detail);
static void psErrorInternal(PlanProfile *context,  const char* detail);
static void issuePsError(PlanProfile *context);

static Flow *makeFlow(FlowType flotype, bool islocal);
static Flow *makeFlowForRelation(Oid reloid);
static void noteFlow(Flow *flow, PlanProfile *context);

static Plan* apply_motion(Plan*  plan);
static Node* apply_motion_mutator(Node*  node, ApplyMotionState *context);
static bool is_plan_node(Node* node);


/* TODO Find a way to make these values real and globally available.  
 *      May also need others such as CdbHomeSegment, etc.  
 */
static unsigned CdbNumSegments = 1;
static unsigned CdbThisSegment = 0;



/* ------------------------------------------------------------------------- *
 * Function cdbparallelize() is the main entry point.  
 *
 * Parallelize a single raw PostgreSQL sequential plan. The input plan
 * is modified in that Flow nodes are attached to Plan nodes in the input.
 * So don't parallelize the same input plan more than once (unless you
 * have set all the flow fields back to NULL).
 *
 * The result is a copy of the input plan with alterations (principally
 * added Motion nodes) to make the plan suitable for parallel execution
 * by CDB.
 *
 * TODO Much investigation remains in the areas of 
 *
 *      - initialization plans,
 *      - plan parameters, 
 *      - subqueries,
 *      - ordering and hash attributes,
 *      - etc.
 *
 *      The current implementation is little more than a rough sketch.
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

	if ( !prescan(plan, query, &profile) )
		issuePsError(&profile);	

#ifdef CDB_DEVELOPER
	ereport(NOTICE,  (
		errmsg("CDB Notice: Plan has %s partitioned and %s local flows.",
			profile.hasPartitionedFlow ? "some" : "no",
			profile.hasLocalFlow ? "some" : "no" )
		));
#endif

	if ( ! profile.hasPartitionedFlow )
		return plan;
	
	return apply_motion(plan);
}


/* ----------------------------------------------------------------------- *
 * Functions prescan() and prescan_walker() use the plan_tree_walker() 
 * framework to visit the nodes of a plan tree to be parallelized and
 * add flow information to the leaves for later use.  The walker may 
 * also choose to exit with an error, usually to report an unsupported
 * feature used in the plan tree or an unexpected problem.
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
	context->hasPartitionedFlow = false;
	context->hasLocalFlow = false;
	
	/* Initialize the current range table. */
	context->rtable = query->rtable;
	
	psError(context,  ERRCODE_CDB_INTERNAL_ERROR,
		"Unexpected failure parallelizing query.", NULL );
	
	/* At the moment, we only accept SELECT statements. */
	Assert( query != NULL && IsA(query, Query) );

#ifndef CDB_DEVELOPER
	switch ( query->commandType )
	{
		case CMD_SELECT:
			break;
			
		case CMD_UPDATE:
			psError(context,  ERRCODE_CDB_FEATURE_NOT_YET,
				"CDB does not support UPDATE yet.", NULL );
			return false;
			
		case CMD_DELETE:
			psError(context,  ERRCODE_CDB_FEATURE_NOT_YET,
				"CDB does not support DELETE yet.", NULL );
			return false;
			
		case CMD_INSERT:
			psError(context,  ERRCODE_CDB_FEATURE_NOT_YET,
				"CDB does not support INSERT yet.", NULL );
			return false;
			
		default:
			psError(context,  ERRCODE_CDB_INTERNAL_ERROR,
				"CDB cannot parallelize unrecognized query type.", NULL );
			return false;
	}
#endif

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
 */
bool 
prescan_walker(Node *node, PlanProfile *context)
{	
	const char *detail = NULL;
	
	if ( node == NULL )
		return false;
	
	switch (nodeTag(node))
	{
		case T_SubPlan:
			/* TODO Figure out whether this code is safe and efficacious. */
			detail = "Query involves a sublink expression.";
			break; /* Error */
			{
				/* SubPlans establish a local state for the plan contain,
				 * notably the range table.
				 */
				SubPlan    *subplan = (SubPlan*) node;

				/* Recur into the exprs list (as usual). */
				if (expression_tree_walker((Node *) subplan->exprs, prescan_walker, context))
					return true;
				
				/* Set local state for the subplan. */
				List *outer_rtable = context->rtable;
				context->rtable = subplan->rtable;
				
				/* Recur into the subplan.  */
				if ( prescan_walker((Node*)subplan->plan, context) )
					return true;
				
				/* Restore state for the parent plan. */
				context->rtable = outer_rtable;
				
				/* Recur into the args list. */
				if (expression_tree_walker((Node *) subplan->args, prescan_walker, context))
					return true;
				
				/* Transmit flow characterization upward. */
				((Plan*)subplan)->flow = copyObject(subplan->plan->flow);
				noteFlow( ((Plan*)subplan)->flow, context );
			}
			return false;
			
		case T_Flow:
			/* Since this function is where Flow nodes are generated, we can't 
			 * rely on this case to see them all.  Instead we handle them as 
			 * they are created.
			 */
			return false;
		
		case T_Motion:
			/* Since this function is called to begin deciding where to put
			 * Motion nodes, we shouldn't see any!
			 */
			Assert(false);
			break;
		
		case T_SeqScan:
		case T_TidScan:
		case T_IndexScan:
		case T_FunctionScan:
		case T_SubqueryScan:
			/* All scan types have a range table entry:	 */
			{
				Plan *plan = (Plan*)node;
				Scan *scan = (Scan*)plan;
				Index rtindex = scan->scanrelid;
				Oid reloid = InvalidOid;
				
				Assert( plan->flow == NULL ); /* Make sure this is first time. */
				Assert( scan->scanrelid > 0 ); /* Make sure we have an index. */
				Assert( 0<rtindex && rtindex<=list_length(context->rtable) );
				
				RangeTblEntry *rte = rt_fetch(rtindex, context->rtable);
				
				switch (nodeTag(node))
				{
					case T_SeqScan:
					case T_TidScan:
						Assert( rte->rtekind == RTE_RELATION );
						reloid = rte->relid;
						plan->flow = makeFlowForRelation(reloid);
						noteFlow(plan->flow, context);
						
						/* Walk into subnodes */
						return plan_tree_walker(node, prescan_walker, context);
						
					case T_IndexScan:
						Assert( rte->rtekind == RTE_RELATION );
						reloid = rte->relid;
						plan->flow = makeFlowForRelation(reloid);
						/*  
						 * IndexScan *s = (IndexScan*)node;
						 *
						 * s->indxid is a list of index oids so, for example,
						 *   get_rel_name(linitial_oid(s->indxid) is the name of the 
						 *   first index.
						 *
						 * s->indxorderdir is the scan order so for example,
						 *   ScanDirectionIsBackward(s->indxorderdir) tests for
						 *   backward.
						 */
						 
						 /* TODO Decide whether/how to record the fact that
						  *      the flow is ordered! 
						  */
						noteFlow(plan->flow, context);
						
						/* Walk into subnodes */
						return plan_tree_walker(node, prescan_walker, context);
				
					case T_FunctionScan:
						Assert( rte->rtekind == RTE_FUNCTION );
						/*
						 * Note that the optimizer may have simplified away 
						 * the function call, so rte->funcexpr may not be a
						 * FuncExpr (or anything) anymore.
						 *
						 * In any case, we need to walk the expression to 
						 * make sure we can allow whatever it does (e.g.,
						 * no volatile stuff in non-singleton flows).
						 *
						 * But for now ... */
						detail = "Query contains a set returning function.";
						break; /* Fall through to report error. */
					
					case T_SubqueryScan:
						Assert( rte->rtekind == RTE_SUBQUERY );
						/*
						 * It appears that a subquery uses the range table
						 * of the retained Query node in the subquery RTE to
						 * resolve the Var nodes in the Plan.
						 * 
						 * TODO Investigate whether a  SubqueryScan can
						 *      have parameters.  Devise way to parallelize.
						 *      Implement range table push/pop code.
						 */
						detail = "Query contains subqueries.";
						break; /* Fall through to report error. */
					
					default: /* Never happens, but makes the compiler happy. */
						Assert( false );
				}
			}
			break;  /* Fall through to report error. */
		

		case T_Result:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				return true;
			{
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				if ( child != NULL )
				{
					/* Might scan child, so inherit flow characteristics. */
					plan->flow = copyObject(child->flow);
				}
				else
				{
					/* Treat Var-free target list as replicated. */
					plan->flow = makeReplicatedFlow();
				}
				noteFlow(plan->flow, context);
			}
			return false;

		case T_Material:
		case T_Hash:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				return true;
			{
				/* Flow is same as leftree's Flow. */
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				Assert( child != NULL );
				plan->flow = copyObject(child->flow);
			}
			return false;
		
		case T_Sort:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				return true;
			{
				/* Flow is same as leftree's Flow ... */
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				Assert( child != NULL );
				plan->flow = copyObject(child->flow);
				
				/* ... but reordered. */
				Sort *sort = (Sort*)node;
				plan->flow->numSortCols = sort->numCols;
				ARRAYCOPY(plan->flow->sortColIdx, sort->sortColIdx, 
					sort->numCols*sizeof(AttrNumber) );
				ARRAYCOPY(plan->flow->sortOperators, sort->sortOperators, 
					sort->numCols*sizeof(Oid) );
				
				noteFlow(plan->flow, context);
			}
			return false;
		
		case T_Unique:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				return true;
			{
				/* Flow is same as leftree's Flow. */
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				
				/* TODO In case the argument flow is partitioned, assure 
				 *      that tuples are collocated on the 
				 *     ((Unique*)node)->numCols attributes of the array
				 *     ((Unique*)node)->uniqColIdx of AttrNumber
				 *
				 * Note the input flow is certainly sorted this way and
				 * that no projection is done by this node.
				 */

				Assert( child != NULL && child->flow != NULL );
				if ( child->flow->flotype == FLOW_REPLICATED
					|| child ->flow->flotype == FLOW_SINGLETON )
				{
					/* No motion required so inherit flow characteristics. */
					plan->flow = copyObject(child->flow);
					noteFlow(plan->flow, context);
					return false;
				}
			}
			detail = "Query involves partitioned duplicate removal.";
			return true;
		
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				Plan *plan = (Plan*)node;
				Join *join = (Join*)plan;
				
				Assert( plan->flow == NULL ); /* Make sure this is first time. */
				Assert ( join->jointype == JOIN_INNER
					|| join->jointype == JOIN_LEFT
					|| join->jointype == JOIN_FULL
					|| join->jointype == JOIN_RIGHT
					|| join->jointype == JOIN_IN ); /* Remove later. */
						
				/* Walk into subnodes */
				if ( plan_tree_walker(node, prescan_walker, context) )
					return true;
				
				switch (nodeTag(node))
				{
					case T_NestLoop:
						/* Sets its own error information, if necessary. */
						return prescan_nestloop(plan, context);
						
					case T_MergeJoin:
						/* Sets its own error information, if necessary. */
						return prescan_mergejoin(plan, context);
						
					case T_HashJoin:
						/* Sets its own error information, if necessary. */
						return prescan_hashjoin(plan, context);
						
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
			 * TODO Determine whether special handling is indicated or whether
			 */
				
			detail = "Query involves a set or multiset operation.";
			break; /* Fall through to report error. */			 
		
		case T_Append:
			/*
			 * Append is used in several contexts, e.g. unions, inheritance
			 * sets, or, when ((Append*)node)->isTarget is true, to hold
			 * the sequence of tables to be updated or deleted.
			 */
			detail = "Query involves inheritance or multiset union.";
			break; /* Fall through to report error. */
		
		case T_Agg:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				return true;
			{
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				Assert( child != NULL && child->flow != NULL );
				
				if ( child->flow->flotype == FLOW_REPLICATED
					|| child ->flow->flotype == FLOW_SINGLETON )
				{
					/* No motion required so inherit flow characteristics. */
					plan->flow = copyObject(child->flow);
					noteFlow(plan->flow, context);
					return false;
				}
			}
			/* TODO Deal with partitioned aggregration. */
			detail = "Query involves partitioned aggregration.";
			break; /* Fall through to report error. */
		
		case T_Group:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				return true;
			{
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				Assert( child != NULL && child->flow != NULL );
				
				if ( child->flow->flotype == FLOW_REPLICATED
					|| child ->flow->flotype == FLOW_SINGLETON )
				{
					/* No motion required so inherit flow characteristics. */
					plan->flow = copyObject(child->flow);
					noteFlow(plan->flow, context);
					return false;
				}
			}
			/* TODO Deal with partitioned aggregration. */
			detail = "Query involves partitioned grouping.";
			break; /* Fall through to report error. */
		
		case T_Limit:
			/* First prescan children */
			if ( plan_tree_walker(node, prescan_walker, context) )
				return true;
			{
				Plan *plan = (Plan*)node;
				Plan *child = outerPlan(node);
				Assert( child != NULL && child->flow != NULL );
				
				if ( child->flow->flotype == FLOW_REPLICATED
					|| child ->flow->flotype == FLOW_SINGLETON )
				{
					/* No motion required so inherit flow characteristics. */
					plan->flow = copyObject(child->flow);
					noteFlow(plan->flow, context);
					return false;
				}
			}
			/* TODO Deal with partitioned LIMIT and OFFSET. */
			detail = "Query involves partitioned LIMIT and OFFSET.";
			break; /* Fall through to report error. */
			
		default:
			/* Let plan_tree_walker() deal with other node types. */
			return plan_tree_walker(node, prescan_walker, context);

	}

	 /* Breaking out of the above switch will setup an error report
	  * with whatever detail was set above. 
	  */
	 psErrorNonce(context, detail);
	return true;
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
	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);
	
	/* Assure that we have the expected pattern:
	 *
	 *          plan 
	 *       (HashTree)
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
	
	/* Figure out what to do. */
	CdbJoinStrategy *strategy = getJoinStrategy((Join*)plan);
	
	if ( strategy == NULL )
	{
		psErrorNonce(context, "No strategy available to parallelize that hash join.");
		/* Hint: Try again with set enable_hashjoin = false */
		return true;	 
	}
	
	List *outerexpr = NULL;
	List *innerexpr = NULL;
	List *hashOp = NULL;

	if ( strategy->leftmove != MOVEMENT_NONE )
	{
		/* Implement left argument movement. */
		switch ( strategy->leftmove )
		{
			case MOVEMENT_FOCUS:
				/* Request motion to focus the outer flow on the dispatcher. */
				outer->flow->req_move = MOVEMENT_FOCUS;
				outer->flow->flotype = FLOW_SINGLETON;
				outer->flow->islocal = true;
				break;
					
			case MOVEMENT_BROADCAST:
				/* Request motion to focus the outer flow on the dispatcher. */
				outer->flow->req_move = MOVEMENT_BROADCAST;
				outer->flow->flotype = FLOW_REPLICATED;
				outer->flow->islocal = false;
				break;
					
			case MOVEMENT_REPARTITION:
				/* Get outer hash information in hand. */
				outerexpr = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
				hashOp = getHashMotionInfoList(plan, INFOTYPE_OP);
				
				/* Request motion to repartition outer flow on join key. */
				outer->flow->req_move = MOVEMENT_REPARTITION;
				outer->flow->flotype = FLOW_PARTITIONED;
				outer->flow->islocal = false;
				outer->flow->hashExpr = outerexpr;
				outer->flow->hashOp = hashOp;
				break;
				
			default:
				Assert(false);
				break;
		}
	}
	
	if ( strategy->rightmove != MOVEMENT_NONE )
	{
		/* Implement right argument movement. */
		switch ( strategy->rightmove )
		{
			case MOVEMENT_FOCUS:
				/* Request motion to focus the inner flow on the dispatcher. */
				inner->lefttree->flow->req_move = MOVEMENT_FOCUS;
				inner->lefttree->flow->flotype = FLOW_SINGLETON;
				inner->lefttree->flow->islocal = true;
				
				/* Adjust the flow type of the intermediate Hash node. */
				inner->flow->flotype = FLOW_SINGLETON;
				inner->flow->islocal = true;
				break;
					
			case MOVEMENT_BROADCAST:
				/* Request motion to broadcast the inner flow. */
				inner->lefttree->flow->req_move = MOVEMENT_BROADCAST;
				inner->lefttree->flow->flotype = FLOW_REPLICATED;
				inner->lefttree->flow->islocal = false;
				
				/* Adjust the flow type of the intermediate Hash node. */
				inner->flow->flotype = FLOW_REPLICATED;
				inner->flow->islocal = false;
				break;
					
			case MOVEMENT_REPARTITION:
				/* Get inner hash information in hand. */
				innerexpr = getHashMotionInfoList(plan, INFOTYPE_RIGHTEXPR);
				if ( hashOp == NULL )
					hashOp = getHashMotionInfoList(plan, INFOTYPE_OP);
					
				/* Request motion ot repartition inner flow on join key. */
				inner->lefttree->flow->req_move = MOVEMENT_REPARTITION;
				inner->lefttree->flow->flotype = FLOW_PARTITIONED;
				inner->lefttree->flow->hashExpr = innerexpr;
				inner->lefttree->flow->hashOp = hashOp;
				
				/* Adjust the flow type of the intermediate Hash node. */
				inner->flow->flotype = FLOW_PARTITIONED;
				inner->flow->islocal = false;
				break;
				
			default:
				Assert(false);
				break;
		}
	}
	
	/* Adopt proposed flow characteristics. */
	plan->flow = makeFlow(strategy->resultflowtype, strategy->resultislocal);
	noteFlow(plan->flow, context);
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
	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);
	
	/* Check assumptions.
	 */
	Assert( IsA(plan, MergeJoin) );
	Assert( IsA(inner,Sort)|| IsA(inner,IndexScan) );
	Assert( IsA(outer,Sort)|| IsA(outer,IndexScan) );
	
	/* Figure out what to do. */
	CdbJoinStrategy *strategy = getJoinStrategy((Join*)plan);
	
	if ( strategy == NULL )
	{
		psErrorNonce(context, "No strategy available to parallelize that merge join.");
		/* Hint: Try again with set enable_mergejoin = false */
		return true;	 
	}
	
	List *outerexpr = NULL;
	List *innerexpr = NULL;
	List *hashOp = NULL;

	if ( strategy->leftmove != MOVEMENT_NONE )
	{
		/* Implement left argument movement. */
		switch ( outer->type )
		{
			case T_Sort:
				switch ( strategy->leftmove )
				{
					case MOVEMENT_FOCUS:
						if ( focusSort((Sort*)outer, context) )
							return true;
						break;
							
					case MOVEMENT_BROADCAST:
						if ( broadcastSort((Sort*)outer, context) )
							return true;
						break;
							
					case MOVEMENT_REPARTITION:
						outerexpr = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
						hashOp = getHashMotionInfoList(plan, INFOTYPE_OP);
						if ( repartitionSort((Sort*)outer, outerexpr, hashOp, context) )
							return true;
						break;
						
					default:
						Assert(false);
						break;
				}
			case T_IndexScan:
				psErrorNonce(context, "Cannot parallelize IndexScan.");
				/* Hint: Try again with set enable_mergescan = false */
				return true;	 
			
			default:
				psErrorInternal(context, "Unexpected left argument to merge join.");
				/* Hint: Try again with set enable_mergescan = false */
				return true;	 
		}
	}

	
	if ( strategy->rightmove != MOVEMENT_NONE )
	{
		/* Implement right argument movement. */
		switch ( inner->type )
		{
			case T_Sort:
				switch ( strategy->rightmove )
				{
					case MOVEMENT_FOCUS:
						if ( focusSort((Sort*)inner, context) )
							return true;
						break;
							
					case MOVEMENT_BROADCAST:
						if ( broadcastSort((Sort*)inner, context) )
							return true;
						break;
							
					case MOVEMENT_REPARTITION:
						innerexpr = getHashMotionInfoList(plan, INFOTYPE_RIGHTEXPR);
						if ( hashOp == NULL )
							hashOp = getHashMotionInfoList(plan, INFOTYPE_OP);
						if ( repartitionSort((Sort*)inner, innerexpr, hashOp, context) )
							return true;
						break;
						
					default:
						Assert(false);
						break;
				}
			case T_IndexScan:
				psErrorNonce(context, "Cannot parallelize IndexScan.");
				/* Hint: Try again with set enable_mergescan = false */
				return true;	 
			
			default:
				psErrorInternal(context, "Unexpected right argument to merge join.");
				/* Hint: Try again with set enable_mergescan = false */
				return true;	 
		}
	}
	
	/* Adopt proposed flow characteristics. */
	plan->flow = makeFlow(strategy->resultflowtype, strategy->resultislocal);
	noteFlow(plan->flow, context);
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
	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);
	
	/* Check assumptions.
	 */
	Assert( IsA(plan, NestLoop) );
	Assert( is_plan_node((Node*)inner) && inner->flow != NULL );
	Assert( is_plan_node((Node*)outer) && outer->flow != NULL );
	
	CdbJoinStrategy *strategy = getJoinStrategy((Join*)plan);
	
	if ( strategy == NULL )
	{
		psErrorNonce(context, "No strategy available to parallelize that nested-loop join.");
		/* Hint: Try again with set enable_nestloop = false */
		return true;	 
	}
	
	if ( strategy->leftmove != MOVEMENT_NONE 
		|| strategy->rightmove != MOVEMENT_NONE  )
	{
		psErrorNonce(context, "Query involves partitioned nested-loop join.");
		/* Hint: Try again with set enable_nestloop = false */
		return true;	 
	}
	
	/* No motion required so just adopt flow characteristics. */
	plan->flow = makeFlow(strategy->resultflowtype, strategy->resultislocal);
	noteFlow(plan->flow, context);
	return false;	 
}



/* 
 * Helper for prescan_mergescan() -- Request repartitioning of the flow from 
 * the node below the given Sort node according to the given join expressions 
 * and operators.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool repartitionSort(Sort *sort, List *exprs, List *ops, PlanProfile *context)
{
	int n = list_length(exprs);
	
	/* Test assumptions. */
	Assert( IsA(sort,Sort) );
	Assert( n == list_length(ops) );
	
	/* The partitioning expressions must match a prefix of the  sort 
	 * specifications for this to be a valid transformation.  Check!
	 */
	if ( sort->numCols < n )
	{
		psErrorInternal(context, "Conflicts between partitioning and sort.");
		return true;
	}
	int i = 0;
	ListCell *cell;
	
	foreach(cell, exprs)
	{
		/* This may eventually need to become smarter, e.g., accept simple
		 * casts of Var.  But for now...
		 */
		Node *x = (Node*)lfirst(cell);
		if ( !IsA(x,Var) || ((Var*)x)->varattno != sort->sortColIdx[i] )
		{
		psErrorInternal(context, "Conflicts between partitioning and sort.");
		return true;
		}
		i++;
	}
	
	/* Everything looks okay. */
	Plan *outer = outerPlan(sort);
	
	/* Request repartitioning. */
	outer->flow->req_move = MOVEMENT_REPARTITION;
	outer->flow->hashExpr = exprs;
	outer->flow->hashOp = ops;
	
	/* Note the both nodes are now partitioned. */
	outer->flow->flotype = FLOW_PARTITIONED;
	((Plan*)sort)->flow->flotype = FLOW_PARTITIONED;
	
	return false; /* Success. */
}

bool focusSort(Sort *sort, PlanProfile *context)
{
	Plan *outer = outerPlan(sort);
		
	outer->flow->req_move = MOVEMENT_FOCUS;
	
	/* Motion node must preserve sort order. */
	int n = sort->numCols;
	outer->flow->numSortCols = n;
	ARRAYCOPY(outer->flow->sortColIdx, sort->sortColIdx, n*sizeof(AttrNumber));
	ARRAYCOPY(outer->flow->sortOperators, sort->sortOperators, n*sizeof(Oid));
	
	return false;
}

bool broadcastSort(Sort *sort, PlanProfile *context)
{
	Plan *outer = outerPlan(sort);
		
	outer->flow->req_move = MOVEMENT_BROADCAST;
	
	/* Motion node must preserve sort order. */
	int n = sort->numCols;
	outer->flow->numSortCols = n;
	ARRAYCOPY(outer->flow->sortColIdx, sort->sortColIdx, n*sizeof(AttrNumber));
	ARRAYCOPY(outer->flow->sortOperators, sort->sortOperators, n*sizeof(Oid));
	
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
 * Helper for prescan() -- Common summarization code for flow nodes.
 */
void noteFlow(Flow *flow, PlanProfile *context)
{
	 /* Note partitioned flows which force parallelization */
	if ( flow->flotype == FLOW_PARTITIONED )
		context->hasPartitionedFlow = true;
		
	 /* Note local flows which disallow parallelization */
	if ( flow->islocal  )
		context->hasLocalFlow = true;
}

/*
 * Construct a new replicated Flow in the current memory context.
 */
Flow *makeFlow(FlowType flotype, bool islocal)
{
	Flow *flow = makeNode(Flow);
	flow->flotype = flotype;
	flow->islocal = islocal;
	flow->req_move = MOVEMENT_NONE;
	return flow;
}

/*
 * Construct a new replicated Flow in the current memory context.
 */
Flow *makeReplicatedFlow()
{
	return makeFlow(FLOW_REPLICATED, false);
}


/*
 * Construct a new local singleton Flow in the current memory context.
 */
Flow *makeDispatcherFlow()
{
	return makeFlow(FLOW_SINGLETON, true);
}


/*
 * Construct a new simple partitioned Flow in the current memory context.
 */
Flow *makePartitionedFlow()
{
	return makeFlow(FLOW_PARTITIONED, false);
}


/* 
 * Construct a new Flow appropriate for the output of a Scan node
 * for the table specified in the argument.  Flow allocated in current
 * memory context.
 */
Flow *makeFlowForRelation(Oid reloid)
{
	Flow *flow;
	
	switch ( get_table_policytype(reloid) )
	{
		case POLICYTYPE_PARTITIONED:	
			flow = makePartitionedFlow();
			break;
		
		case POLICYTYPE_REPLICATED:	
			flow = makeReplicatedFlow();
			break;
		
		case POLICYTYPE_SPECIAL:
		case POLICYTYPE_UNDEFINED:
		default:
			flow = makeDispatcherFlow();
			break;
	}
	
	return flow;
}


/* ------------------------------------------------------------------------- *
 * Function apply_motion() and apply_motion_mutator() add motion nodes to 
 * a top-level Plan tree as directed by the Flow nodes in the plan.  
 *
 * In addition, if the argument Plan produces a partitioned flow, a Motion 
 * is added to consolidate the tuple streams that make up the flow into a 
 * single tuple stream on the dispatcher.
 *
 * The result is a deep copy of the argument Plan tree with added Motion 
 * nodes.
 * ------------------------------------------------------------------------- *
 */
Plan* apply_motion(Plan*  plan)
{
	ApplyMotionState state;
	state.dummy = 0;
	
	/* If the top plan node emits a parallel flow, refocus it on the
	 * dispatch stream.
	 */
	if ( plan->flow->flotype == FLOW_PARTITIONED )
		plan->flow->req_move = MOVEMENT_FOCUS;
	
	return (Plan*)apply_motion_mutator((Node*)plan, &state);
}


/* 
 * Function apply_motion_mutator() is the workhorse for apply_motion().
 */
Node* apply_motion_mutator(Node*  node, ApplyMotionState *context)
{
	if ( node == NULL )
		return NULL;
	
	/*  Copy this node and mutate its children. */
	Node *newnode = plan_tree_mutator(node, apply_motion_mutator, context);
	
	/* Add motion atop the copied node, if necessary. **/
	switch (nodeTag(newnode))
	{
		/* Abstract: really should see only subclasses. */
		case T_Plan:
		case T_Scan: 
		case T_Join:
			Assert(false);
			break; 
			
		/* Motion nodes can't specify motion. */
		case T_Motion:
			Assert( ((Plan*)newnode)->flow->req_move == MOVEMENT_NONE );
			break;
		
		/* Add any specified motion to the top of the plan subtree. */
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_Limit:
		case T_Append:		
		case T_Result:
		case T_SeqScan:
		case T_FunctionScan:
		case T_IndexScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_Group:
		case T_Agg:
		case T_SubPlan:
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				Plan *plan = (Plan*)newnode;
				Flow *flow = plan->flow;
				
				bool sendToSecondary = false; /* always for now */
				
				switch ( flow->req_move )
				{
					case MOVEMENT_FOCUS:
						/* TODO Fix this branch (which requires, in turn,
						 *      extending our Flow support) to specify
						 *      sort order, whether to send to secondary,
						 *      and which segment is this one.  (Or should
						 *      we specify "dispatcher" and let plan state
						 *      specify the actual segment number?).
						 *
						 *      So make_union_motion( newnode,
						 *                            flow->sendToSecondary,
						 *                            ThisSegmentNumber );
						 *      or something.
						 */
						if ( flow->req_move!=MOVEMENT_NONE && flow->numSortCols>0 )
						{
							newnode = (Node*)make_sorted_union_motion(
								plan,
								sendToSecondary,
								CdbThisSegment,
								flow->numSortCols,
								flow->sortColIdx,
								flow->sortOperators,
								CdbNumSegments,
								makeDefaultSegIdxArray(CdbNumSegments)
								);
						}
						else
						{
							/* TODO: get the segindex of me */
							int mySegIdx;
							getCdbIdentity( &mySegIdx, NULL );
							newnode = (Node*)make_union_motion(
								plan, 
								sendToSecondary, 
								mySegIdx
								);
						}
						break;
						
					case MOVEMENT_BROADCAST:
						if ( flow->req_move!=MOVEMENT_NONE && flow->numSortCols>0 )
						{
							newnode = (Node*)make_sorted_broadcast_motion(
								plan,
								sendToSecondary,
								flow->numSortCols,
								flow->sortColIdx,
								flow->sortOperators,
								CdbNumSegments,
								makeDefaultSegIdxArray(CdbNumSegments)
								);
						}
						else
						{
							newnode = (Node*)make_broadcast_motion(
								plan, 
								sendToSecondary
								);
						}
						break;
						
					case MOVEMENT_REPARTITION:
						Assert( flow->hashExpr != NULL && flow->hashOp != NULL );
						if ( flow->req_move!=MOVEMENT_NONE && flow->numSortCols>0 )
						{
							newnode = (Node*)make_sorted_hashed_motion(
								plan,
								sendToSecondary,
								CdbNumSegments,
								makeDefaultSegIdxArray(CdbNumSegments),
								flow->hashExpr,
								flow->hashOp,
								flow->numSortCols,
								flow->sortColIdx,
								flow->sortOperators,
								CdbNumSegments,
								makeDefaultSegIdxArray(CdbNumSegments)
								);
						}
						else
						{
							newnode = (Node*)make_hashed_motion(
								plan, 
								sendToSecondary,
								CdbNumSegments,
								makeDefaultSegIdxArray(CdbNumSegments),
								flow->hashExpr,
								flow->hashOp								
								);
						}
						break;
						
					
					case MOVEMENT_NONE:
						break;
						
					default:
						Assert(false);
						break;
				}
			}
		
		default:
			break;
	}
	
	return newnode;
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

