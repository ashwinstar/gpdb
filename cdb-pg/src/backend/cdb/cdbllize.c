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
 * A PlanProfile holds state for recursive prescan_walker(). 
 */
typedef struct PlanProfile
{
	/* Summary status. */
	bool	hasPartitionedFlow;
	bool	hasLocalFlow;
	
	/* The uses flags refer to relations in the query */
	bool	usesCatalog;
	bool	usesSegments;

	/* The result flags refer to relation in the result of an insert, update, or delete */
	bool	resultCatalog;
	bool	resultSegments;
	
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
 * Forward Declarations
 */
static bool prescan(Plan *plan, Query *query, PlanProfile *context);
static bool prescan_walker(Node *node, PlanProfile *context);
static bool prescan_hashjoin(Plan *plan, PlanProfile *context);
static bool prescan_mergejoin(Plan *plan, PlanProfile *context);
static bool prescan_nestloop(Plan *plan, PlanProfile *context);
static bool prescan_agg(Plan *plan, PlanProfile *context);

static bool repartitionSort(Sort *sort, List *exprs, List *ops, PlanProfile *context);
static bool focusSort(Sort *sort, PlanProfile *context);
static bool broadcastSort(Sort *sort, PlanProfile *context);

static void psError(PlanProfile *context, unsigned code, const char* message, const char* detail);
static void psErrorNonce(PlanProfile *context,  const char* detail);
static void psErrorInternal(PlanProfile *context,  const char* detail);
static void issuePsError(PlanProfile *context);

static Flow *makeFlowForRelation(Oid reloid, PlanProfile *context);
static void noteFlow(Flow *flow, PlanProfile *context);

static bool initPlanProfile(PlanProfile *context, Query *query);
static bool scanForManagedTables_walker(Node *node, PlanProfile *context);
static bool scanForManagedTables(Plan *plan, Query *query);

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
 * TODO: Split the prescan into 2 phases to avoid flunking QD-only queries.
 *
 *		The first scan (a true prescan) can just attach initial Flows
 *		to Plan leaves and take an inventory of relations touched.
 *      There no reason this would need to fail, so we can take an
 *      early exit for QD-only queries.
 *     
 *      The seconnd scan (a postscan) does what the current scan does
 *      minus attaching initial Flows to leaves.
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
	Assert( is_plan_node((Node*)plan) );
	Assert( query != NULL && IsA(query, Query) );
	
	/* If the plan is already marked dispatchable, just return it. */
	if ( plan->dispatch != DISPATCH_UNDETERMINED )
		return plan;

	/* 
	 * Do a quick walk of the plan to see whether there are
	 * any managed tables referenced.  If not, we don't need to
	 * perform the prescan.
	 */
	if ( !scanForManagedTables(plan, query) )
	{
		plan->dispatch = DISPATCH_SEQUENTIAL;
	}
	else
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
		ereport(NOTICE,  (
			errmsg("CDB Notice: Plan %s catalog and %s segments.",
				(profile.usesCatalog || profile.resultCatalog) ? "uses" : "doesn't use",
				(profile.usesSegments || profile.resultSegments) ? "uses" : "doesn't use" )
			));
	#endif

		/*
		 * Since we scanForManagedTables was true, we have to have usesSegments
		 * in either the query relations or the result relation.
		 */
		Assert( profile.usesSegments || profile.resultSegments );

		/*
		 * Since we know we are reference segments either in the source or target of the queries
		 * we know we cannot yet also useCatalog.
		 */
		if ( profile.usesCatalog )
		{
			ereport(ERROR,  (
				errcode(ERRCODE_CDB_FEATURE_NOT_YET),
				errmsg("Query references both catalog and data."),
				errhint("CDB requires separate queries.")
				));
		}

		plan = apply_motion(plan, query);
		plan->dispatch = DISPATCH_PARALLEL;
	}

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
			/* A SubPlan node is not a Plan node, but an Expr node that
			 * contains a Plan and its range table.  It corresponds to a
			 * SubLink in the query tree (a SQL subquery expression).
			 */
			 
			/* TODO Figure out whether this code is safe and efficacious. */
			detail = "Query involves a sublink expression.";
			break; /* Error */
			{
				/* SubPlans establish a local state for the contained plan,
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
						plan->flow = makeFlowForRelation(reloid, context);
						noteFlow(plan->flow, context);
						
						/* Walk into subnodes */
						return plan_tree_walker(node, prescan_walker, context);
						
					case T_IndexScan:
						Assert( rte->rtekind == RTE_RELATION );
						reloid = rte->relid;
						plan->flow = makeFlowForRelation(reloid, context);
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
						 * The subplan within a subquery uses the same range 
						 * table as the subquery itself.
						 * 
						 * TODO Investigate whether a  SubqueryScan can
						 *      have parameters.  Devise way to parallelize.
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
					plan->flow = makeFlow(FLOW_REPLICATED);
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
			{
				/* First prescan children */
				if ( plan_tree_walker(node, prescan_walker, context) )
					return true;
					
				return prescan_agg((Plan*)node, context);
			}
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
	List *strategies = NIL;
	CdbJoinStrategy *strategy = NULL;
	
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
	
	/* Use the first strategy. */
	strategies = getJoinStrategyList((Join*)plan);
	if ( strategies != NIL )
	{
		strategy = copyJoinStrategy((CdbJoinStrategy*)linitial(strategies));
		freeJoinStrategyList(strategies);
		strategies = NIL;
	}
	
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
				/* Request motion to focus the outer flow on a segment. */
				outer->flow->req_move = MOVEMENT_FOCUS;
				outer->flow->flotype = FLOW_SINGLETON;
				outer->flow->segindex = 0;
				break;
					
			case MOVEMENT_BROADCAST:
				/* Request motion to replicate the outer flow. */
				outer->flow->req_move = MOVEMENT_BROADCAST;
				outer->flow->flotype = FLOW_REPLICATED;
				outer->flow->segindex = 0;
				break;
					
			case MOVEMENT_REPARTITION:
				/* Get outer hash information in hand. */
				outerexpr = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
				hashOp = getHashMotionInfoList(plan, INFOTYPE_OP);
				
				/* Request motion to repartition outer flow on join key. */
				outer->flow->req_move = MOVEMENT_REPARTITION;
				outer->flow->flotype = FLOW_PARTITIONED;
				outer->flow->segindex = 0;
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
				/* Request motion to focus the inner flow on a segment. */
				inner->lefttree->flow->req_move = MOVEMENT_FOCUS;
				inner->lefttree->flow->flotype = FLOW_SINGLETON;
				inner->lefttree->flow->segindex = 0;
				
				/* Adjust the flow type of the intermediate Hash node. */
				inner->flow->flotype = FLOW_SINGLETON;
				inner->flow->segindex = 0;
				break;
					
			case MOVEMENT_BROADCAST:
				/* Request motion to broadcast the inner flow. */
				inner->lefttree->flow->req_move = MOVEMENT_BROADCAST;
				inner->lefttree->flow->flotype = FLOW_REPLICATED;
				inner->lefttree->flow->segindex = 0;
				
				/* Adjust the flow type of the intermediate Hash node. */
				inner->flow->flotype = FLOW_REPLICATED;
				inner->flow->segindex = 0;
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
				inner->flow->segindex = 0;
				break;
				
			default:
				Assert(false);
				break;
		}
	}
	
	/* Adopt proposed flow characteristics. */
	plan->flow = makeFlow(strategy->resultflowtype);
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
	List *strategies;
	ListCell *cell;
	CdbJoinStrategy *strategy = NULL;
	
	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);
	
	/* Check assumptions.
	 */
	Assert( IsA(plan, MergeJoin) );	

	strategies = getJoinStrategyList((Join*)plan);
	
	/*
	 * Use the first strategy we know how to implement. 
	 */
	foreach( cell, strategies)
	{
		CdbJoinStrategy *s = (CdbJoinStrategy*)lfirst(cell);
	
		/* Copy the first strategy we can use and exit the loop. */
		
		/* Order-preserving hash motion might deadlock.  Don't do it! 
		 * However, if the affected side is a Sort node, we know to push the
		 * motion below it.
		 */
		if ( s->leftmove == MOVEMENT_REPARTITION && !IsA(outer,Sort) )
			continue;
		if ( s->rightmove == MOVEMENT_REPARTITION && !IsA(inner,Sort) )
			continue;
		
		/* At the moment, we know how to extract ordering information only 
		 * from Sort nodes.  
		 *
		 * TODO: Learn to pull it from IndexScan and MergeJoin nodes
		 *       and any other node types known to give ordered results.
		 */
		if ( s->leftmove != MOVEMENT_NONE && !IsA(outer,Sort) )
			continue;
		if ( s->rightmove != MOVEMENT_NONE && !IsA(inner,Sort) )
			continue;
			
		strategy = copyJoinStrategy(s);
		break;
	}
	freeJoinStrategyList(strategies);
	strategies = NIL;
	
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
				break;
				
			case T_IndexScan:
			case T_MergeJoin:
				Assert(false); /* This case filtered out above. */
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
				break;
				
			case T_IndexScan:
			case T_MergeJoin:
				Assert(false); /* This case filtered out above. */
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
	plan->flow = makeFlow(strategy->resultflowtype);
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
	List *strategies;
	ListCell *cell;
	CdbJoinStrategy *strategy = NULL;


	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);	
			
	/* 
	 * Check assumptions.
	 */
	Assert( IsA(plan, NestLoop) && plan->flow == NULL );
	Assert( is_plan_node((Node*)inner) && inner->flow != NULL );
	Assert( is_plan_node((Node*)outer) && outer->flow != NULL );
	
	strategies = getJoinStrategyList((Join*)plan);
	
	List *hashExpr = NIL;
	List *hashOp = NIL;
	
	if ( extractHashExprFromQual( ((Join*)plan)->joinqual, &hashExpr, &hashOp ))
	{
		/* Yippee, we can use repartitioning  */
	}

	/*
	 * Use the first strategy we can implement. 
	 */
	foreach( cell, strategies)
	{
		CdbJoinStrategy *s = (CdbJoinStrategy*)lfirst(cell);
		
		/* 
		 * TODO Determine whether we can extract hashExpr and hashOp from
		 * the join conditions (qual and joinqual) which would allow us
		 * to use repartitioning.  In the meantime, we can't use it.
		 */
		if ( s->leftmove == MOVEMENT_REPARTITION || s->rightmove == MOVEMENT_REPARTITION )
			continue;
		
		/*
		 * If the inner relation is an IndexScan, the NestLoop is tightly 
		 * coupled to it (needs to pass index keys on ExecReScan calls) 
		 * so we can't insert any motion in between.
		 */
		 if ( IsA(inner, IndexScan) && s->rightmove != MOVEMENT_NONE )
			continue;
		/* 
		 * TODO: Remove restriction by learning to add Material over Motion
		 *       in the apply_motion_mutator().  The additional communication
		 *       this would need is unclear.  Also need to consider its
		 *       effect on costing.
		 */
		if (  ! IsA(inner, Material) && s->rightmove != MOVEMENT_NONE )
			continue;
			
		strategy = copyJoinStrategy(s);
		break;
	}
	freeJoinStrategyList(strategies);
	strategies = NIL;
	
	if ( strategy == NULL )
	{
		psErrorNonce(context, "No strategy available to parallelize that nested-loop join.");
		/* Hint: Try again with set enable_nestloop = false */
		return true;	 
	}
	else if ( strategy->leftmove == MOVEMENT_NONE && strategy->rightmove == MOVEMENT_NONE  )
	{
		plan->flow = makeFlow(strategy->resultflowtype);
	}
	else 
	{
		if ( strategy->leftmove != MOVEMENT_NONE )
		{
			/* Implement left argument movement. */
			switch ( strategy->leftmove )
			{
				case MOVEMENT_FOCUS:
					/* Request motion to focus the outer flow on a segment. */
					outer->flow->req_move = MOVEMENT_FOCUS;
					outer->flow->flotype = FLOW_SINGLETON;
					outer->flow->segindex = 0;
					break;
						
				case MOVEMENT_BROADCAST:
					/* Request motion to focus the outer flow on the dispatcher. */
					outer->flow->req_move = MOVEMENT_BROADCAST;
					outer->flow->flotype = FLOW_REPLICATED;
					outer->flow->segindex = 0;
					break;
						
				case MOVEMENT_REPARTITION:
				default:
					Assert(false);
					break;
			}
		}
		
		if ( strategy->rightmove != MOVEMENT_NONE )
		{
			/* Implement right argument movement. Since we discard strategies
			 * that would introduce Motion between a NestLoop and an inner
			 * IndexScan, it is likely that the inner relation will always 
			 * be a Material node, like this:
			 *
			 *          plan 
			 *       (NestLoop)
			 *        /     \ 
			 *     outer   inner
			 *    (Plan) (Material)
			 *               |              
			 *          inner->lefttree 
			 *             (Plan)
             *
			 * We don't assume this, but if it isn't the case we'll fail.
			 *			
			 * TODO: Need a way to communicate (in a Flow) to the mutator
			 *       that it should add a Material node above an added Motion.
			 *       This would avoid the special requirement.  Note that we
			 *       eliminate strategies that don't match the pattern in
			 *       the filter stage above, so the Assert should never trip.
			 */
			Assert ( IsA(inner, Material) );
			
			switch ( strategy->rightmove )
			{
				case MOVEMENT_FOCUS:
					/* Request motion to focus the inner flow on a segment. */
					inner->lefttree->flow->req_move = MOVEMENT_FOCUS;
					inner->lefttree->flow->flotype = FLOW_SINGLETON;
					inner->lefttree->flow->segindex = 0;
					
					/* Adjust the flow type of the intermediate Material node. */
					inner->flow->flotype = FLOW_SINGLETON;
					inner->flow->segindex = 0;
					break;
						
				case MOVEMENT_BROADCAST:
					/* Request motion to broadcast the inner flow. */
					inner->lefttree->flow->req_move = MOVEMENT_BROADCAST;
					inner->lefttree->flow->flotype = FLOW_REPLICATED;
					inner->lefttree->flow->segindex = 0;
					
					/* Adjust the flow type of the intermediate Material node. */
					inner->flow->flotype = FLOW_REPLICATED;
					inner->flow->segindex = 0;
					break;
						
				case MOVEMENT_REPARTITION:
				default:
					Assert(false);
					break;
			}
		}
	}
	
	/* Adopt flow characteristics of strategy for the Nestloop. */
	plan->flow = makeFlow(strategy->resultflowtype);
	noteFlow(plan->flow, context);
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
	CdbAggStrategy *strategy;
	Plan *child;
	
	/* Argument is a prescaned plan. */
	child = outerPlan((Node*)plan);
	Assert( is_plan_node((Node*)child) && child->flow != NULL  );
	
	/* Now get the stategy list. */
	strategies = getAggStrategyList((Agg*)plan);

	/* Use the first strategy we know how to implement. */
	strategy = NULL;
	foreach( cell, strategies)
	{
		/* Copy the first strategy we can use and exit the loop. */
		CdbAggStrategy *s = (CdbAggStrategy*)lfirst(cell);
		switch ( s->leftmove )
		{
			case MOVEMENT_NONE:
			case MOVEMENT_FOCUS:
			case MOVEMENT_BROADCAST:
			case MOVEMENT_REPARTITION:
				strategy = copyAggStrategy(s);
				break; /* switch */
			default:
				break; /* switch */
		}
		if ( strategy != NULL )
			break; /* foreach */
	}
	freeAggStrategyList(strategies);
	strategies = NIL;
	
	/* Use the chosen strategy to fill in the flow. */
	if ( strategy == NULL )
	{
		psErrorNonce(context, "Cannot find aggregation stategy");
		return true;	 
	}
		
	switch ( strategy->leftmove )
	{
		case MOVEMENT_NONE:
			/* No motion required so inherit flow characteristics. */
			plan->flow = copyObject(child->flow);
			noteFlow(plan->flow, context);
			return false;
			
		case MOVEMENT_BROADCAST: 
			child->flow = makeFlow(FLOW_REPLICATED);
			child->flow->req_move = strategy->leftmove;
			noteFlow(child->flow, context);
			break;
		
		case MOVEMENT_FOCUS:
			child->flow = makeFlow(FLOW_SINGLETON);  /* Focus on a segment. */
			child->flow->req_move = strategy->leftmove;
			noteFlow(child->flow, context);
			break;
			
		case MOVEMENT_REPARTITION:
			child->flow = makeFlow(FLOW_PARTITIONED);
			child->flow->req_move = strategy->leftmove;
			child->flow->hashExpr = strategy->hashexprs;
			child->flow->hashOp = strategy->hashtypes;
			noteFlow(child->flow, context);
			break;
		
		default:
			psErrorNonce(context, "Cannot implement aggregation strategy");
			return true;
	}

	
	/* Adopt flow characteristics of strategy for the Nestloop. */
	/* TODO We could do better, e.g., track partition expressions. */
	plan->flow = makeFlow(strategy->resultflowtype);
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
		/* TODO This may eventually need to become smarter, e.g., accept simple
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
	/* TODO: Have Brian look at this.  It seems wrong to me.  
	int n = sort->numCols;
	outer->flow->numSortCols = n;
	ARRAYCOPY(outer->flow->sortColIdx, sort->sortColIdx, n*sizeof(AttrNumber));
	ARRAYCOPY(outer->flow->sortOperators, sort->sortOperators, n*sizeof(Oid));
	*/

	return false;
}

bool broadcastSort(Sort *sort, PlanProfile *context)
{
	Plan *outer = outerPlan(sort);
		
	outer->flow->req_move = MOVEMENT_BROADCAST;
	
	/* Motion node must preserve sort order. */

	/* TODO: Have Brian look at this.  It seems wrong to me.  
	int n = sort->numCols;
	outer->flow->numSortCols = n;
	ARRAYCOPY(outer->flow->sortColIdx, sort->sortColIdx, n*sizeof(AttrNumber));
	ARRAYCOPY(outer->flow->sortOperators, sort->sortOperators, n*sizeof(Oid));
	*/

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
	{
		context->hasPartitionedFlow = true;
	}
		
	 /* Note dispatcher flows which disallow parallelization */
	if ( flow->flotype == FLOW_SINGLETON && flow->segindex == -1  )
	{
		context->hasLocalFlow = true;
	}
}

/*
 * Construct a new Flow in the current memory context.
 */
Flow *makeFlow(FlowType flotype)
{
	Flow *flow = makeNode(Flow);
	flow->flotype = flotype;
	flow->req_move = MOVEMENT_NONE;
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
 * Construct a new Flow appropriate for the output of a Scan node
 * for the table specified in the argument.  Flow allocated in current
 * memory context.
 */
Flow *makeFlowForRelation(Oid reloid, PlanProfile *context)
{
	Flow *flow;
	Oid typid;
	int32 typmod;
	Oid eq_oid;
	int i;
	Var *var;
	Node* eqstr = NULL;
	List* eqlst = NIL;
	
	CdbPolicy *policy = get_table_policy(reloid);
	
	switch ( policy->ptype )
	{
		case POLICYTYPE_PARTITIONED:	
			flow = makeFlow(FLOW_PARTITIONED);
			
			eqstr = (Node*)makeString("=");
			eqlst = list_make1(eqstr);
			
			for ( i = 0; i < policy->nattrs; i++ )
			{
				/* Get attribute datatype information. */
				typid = InvalidOid;
				typmod = -1;
				get_atttypetypmod(reloid, policy->attrs[i], &typid, &typmod);
				
				/* Get equality operator OID. */
				eq_oid = compatible_oper_opid(eqlst, typid, typid, true);
				
				if ( eq_oid == InvalidOid )
				{
					ereport(ERROR, (	
						errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("no equality operator for partitioned attribute typid %d", typid)
					));
				}
				
				/* makeVar(varno, attrno, typid, typmod, varlevelsup); */
				var = makeVar(1,  policy->attrs[i], typid, typmod, 0);
				
				flow->hashExpr = lappend(flow->hashExpr, var);
				flow->hashOp = lappend_oid(flow->hashOp, eq_oid);
				
			}
			if ( context != NULL )
				context->usesSegments = true;
			pfree(eqlst);
			pfree(eqstr);
			break;
		
		case POLICYTYPE_ENTRY:
			flow = makeFlow(FLOW_SINGLETON);
			flow->segindex = -1; /* This is a dispatcher singleton! */
			if ( context != NULL )
				context->usesCatalog = true;
			break;
		
		case POLICYTYPE_UNDEFINED:
		default:
			ereport(ERROR, (	
				errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("Unexpected CdbPolicyType: %d.", policy->ptype )
			));
			
	}
	
	return flow;
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
 * Returns true if the there exists at least one managed table referenced by the Plan,
 * false otherwise.
 *
 * If an error is encountered, an ereport is issued directly.
 *
 * The purpose of scanForManagedTables is to do a quick scan to see whether there
 * are no managed tables, in which case we can skip the parallelization step.
 * Without this, we may generate errors during the parallelization process
 * that will erroneously prevent catalog queries from running.
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
 * Function scanForManagedTables_walker is the workhorse of scanForManagedTables.
 * It only cares about SeqScans, TidScans, and Index Scans.
 * For these, it gets the relid of the scan, gets the RangeTableEntry
 * from the context rtable, and gets the oid of the relation in the scan.
 * It calls get_table_polict to see whether this oid represents a
 * managed table.  If so, we can exit the walk by returning true.
 * If not, we use plan_walker to walk the appropriate child nodes.
 * For nodes other than SeqScan, TidScan, and IndexScan, we just 
 * use plan_walker to walk the appropriate child nodes.
 *
 * The only field of the PlanProfile that we end up using in the walk is the
 * rtable.  For simplicity sake, we just reuse the PLanProfile
 * as the context of this walk, rather than create a different context structure
 * than that used by prescan_walker.
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
			/* All scan types have a range table entry:	 */
			{
				Plan *plan = (Plan*)node;
				Scan *scan = (Scan*)plan;
				Index rtindex = scan->scanrelid;
				Oid reloid = InvalidOid;
				CdbPolicy *policy;
				CdbPolicyType ptype;
				
				Assert( scan->scanrelid > 0 ); /* Make sure we have an index. */
				Assert( 0<rtindex && rtindex<=list_length(context->rtable) );
				
				RangeTblEntry *rte = rt_fetch(rtindex, context->rtable);
				
				reloid = rte->relid;				
				policy = get_table_policy(reloid);
				ptype = policy->ptype;
				pfree( policy );
	
				switch ( ptype )
				{
					case POLICYTYPE_PARTITIONED:	
						return true;
					
					case POLICYTYPE_ENTRY:
						break;
					
					case POLICYTYPE_UNDEFINED:
					default:
						ereport(ERROR, (	
							errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Unexpected CdbPolicyType: %d.", policy->ptype )
						));
				}
			}

			break;  

		case T_SubqueryScan:
			{
				/*
				 * SubqueryScans have thier own range table.  See ExecInitSubqueryScan
				 * for details.
				 * We save the context range table, substitute the SubqueryScan's range table
				 * in the context before we descend into the fields of the SubqueryScan
				 * and then put the original one back.
				 */
				RangeTblEntry *rte;
				List *rtOld;
				Plan *plan = (Plan*)node;
				Scan *scan = (Scan*)plan;
				Index rtindex = scan->scanrelid;
				
				rte = rt_fetch(rtindex, context->rtable);
				Assert(rte->rtekind == RTE_SUBQUERY);
				
				rtOld = context->rtable;
				Assert( rte->subquery->rtable != NULL );

				context->rtable = rte->subquery->rtable;
				
				if ( plan_tree_walker(node, scanForManagedTables_walker, context) )
					return true;

				context->rtable = rtOld;

				return false;
			}

			break;

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

	context->hasPartitionedFlow = false;
	context->hasLocalFlow = false;
	context->usesCatalog = false;
	context->usesSegments = false;
	context->resultCatalog = false;
	context->resultSegments = false;
	
	/* Initialize the current range table. */
	context->rtable = query->rtable;
	
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
				
				Assert( query->resultRelation<=list_length(context->rtable) );
				rte = rt_fetch(query->resultRelation, context->rtable);
				Assert( rte->rtekind == RTE_RELATION );
				reloid = rte->relid;
				policy = get_table_policy(reloid);
				
				if ( policy->ptype == POLICYTYPE_PARTITIONED )
					context->resultSegments = true;
				else if ( policy->ptype == POLICYTYPE_ENTRY )
					context->resultCatalog = true;
				else
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
