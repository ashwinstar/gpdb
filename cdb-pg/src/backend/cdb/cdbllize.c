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
} ApplyMotionState;


/*
 * Forward Declarations
 */
static bool prescan(Plan *plan, Query *query, PlanProfile *context);
static bool prescan_walker(Node *node, PlanProfile *context);
static bool prescan_hash_join(Plan *plan, PlanProfile *context);
static void psError(PlanProfile *context, unsigned code, 
		const char* message, const char* detail);
static void issuePsError(PlanProfile *context);
static Flow *makeFlow(FlowType flotype, bool islocal);
static Flow *makeFlowForRelation(Oid reloid);
static void noteFlow(Flow *flow, PlanProfile *context);
static Plan* apply_motion(Plan*  plan);
static Node* apply_motion_mutator(Node*  node, ApplyMotionState *context);
static bool is_plan_node(Node* node);


/* TODO Find a way to make these values available. */

static unsigned CdbNumSegments = 1;
static unsigned CdbThisSegment = 0;

/* ------------------------------------------------------------------------- *
 * Function cdbparallelize() is the main entry point.
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
#ifndef CDB_DEVELOPER
	return plan;
#endif

	PlanProfile profile; /* Will be initialized by prescan(). */
	
	if ( !prescan(plan, query, &profile) )
		issuePsError(&profile);	

	ereport(NOTICE,  (
		errmsg("CDB Notice: Plan has %s partitioned and %s local flows.",
			profile.hasPartitionedFlow ? "some" : "no",
			profile.hasLocalFlow ? "some" : "no" )
		));

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
						 
						 /* TODO Designate flow as ordered! */

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
						 * At present it is a MYSTERY how plans specify what
						 * to do with subquery RTEs.
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
				Assert( child != NULL );
				
				/* TODO In case the argument flow is partitioned, assure 
				 *      that tuples are collocated on the 
				 *     ((Unique*)node)->numCols attributes of the array
				 *     ((Unique*)node)->uniqColIdx of AttrNumber
				 */
				 
				/* Note the input flow is certainly sorted this way and
				 * that no projection is done by this node.
				 */
				plan->flow = copyObject(child->flow);
				noteFlow(plan->flow, context);
			}
			return false;
		
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
					
						/* TODO Can't be sure that the join is hashable so
						 *      need to figure that out.  
						 */
						
						/* TODO Set flows!
						 */
						break; /* Error */
						
					case T_MergeJoin:
						/* ((MergeJoin*)join)->mergeclauses */
						
						/* TODO Set flows!
						 */
						break; /* Error */
						
					case T_HashJoin:
						/* Sets its own error information, if necessary. */
						return prescan_hash_join(plan, context);
						
					default:
						break; /* Error */
				}
			}
			detail = "Query involves a join.";
			break; /* Fall through to report error. */
		
		case T_SetOp:
			/*
			 * A SetOp node appears to have only a lefttree like this Lisp:
			 * (Sort (Append (List (SubQueryScan ...) (SubQueryScan ....)))).
			 *
			 * It does no projection.
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
			detail = "Query involves aggregration.";
			break; /* Fall through to report error. */
		
		case T_Group:
			detail = "Query involves grouping.";
			break; /* Fall through to report error. */
		
		case T_Limit:
			detail = "Query involves LIMIT and OFFSET.";
			break; /* Fall through to report error. */
			
		default:
			/* Let plan_tree_walker() deal with other node types. */
			return plan_tree_walker(node, prescan_walker, context);

	}

	 /* Breaking out of the above switch will generate an error report
	  * with whatever detail was set above. 
	  */
	 psError(context, ERRCODE_CDB_FEATURE_NOT_YET,
			"CDB cannot parallelize that query yet.", detail);
		
	return true; /* Never get here, but keep's the compiler happy.. */
}

/* Helper for prescan_walker() does processing for a HashJoin.
 *
 * Note that prescan_walker has already finished scaning the child nodes
 * of the join.
 *
 * Return false to indicate successful prescan.  Else set error info
 * in the context and return true.
 */
bool prescan_hash_join(Plan *plan, PlanProfile *context)
{	
	Plan *outer = outerPlan(plan);
	Plan *inner = innerPlan(plan);
	Flow *joinflow = NULL;
	
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
	 
	/* If either argument is replicated, we can use the other flow
	 * to specify the output flow type of the join.  No motion is
	 * required.
	 */
	if ( outer->flow->flotype == FLOW_REPLICATED )
	{
		joinflow = makeFlow(inner->flow->flotype, inner->flow->islocal);
	}
	else if ( inner->flow->flotype == FLOW_REPLICATED )
	{
		joinflow = makeFlow(outer->flow->flotype, outer->flow->islocal);
	}

	 /* Else If both argument flows are singleton and local, so is the
	  * result flow.  No motion is required.
	  */
	else if ( outer->flow->flotype == FLOW_SINGLETON && outer->flow->islocal
		&&  inner->flow->flotype == FLOW_SINGLETON && inner->flow->islocal )
	{
		joinflow = makeDispatcherFlow();
	}
	
	/* Else If one argument flow is singleton and local and the other is
	 * partitioned, we need to focus the partitioned flow on the 
	 * dispatcher.  (The alternative of broadcasting the singleton is
	 * not available because it is local.  Someday soon we'll want to
	 * support non-local singletons, but we don't at the moment.)
	 */
	else if (  outer->flow->flotype == FLOW_PARTITIONED 
			&& inner->flow->flotype == FLOW_SINGLETON  
			&& inner->flow->islocal )
	{
		/* Request motion to focus the outer flow on the dispatcher. */
		outer->flow->req_move = MOVEMENT_FOCUS;
		outer->flow->flotype = FLOW_SINGLETON;
		outer->flow->islocal = true;
		
		/* The result flow of the HashJoin is also local. */
		joinflow = makeDispatcherFlow();
	}
	else if (  outer->flow->flotype == FLOW_SINGLETON
			&& outer->flow->islocal
			&& inner->flow->flotype == FLOW_PARTITIONED )
	{
		/* Request motion to focus the inner flow on the dispatcher. */
		inner->lefttree->flow->req_move = MOVEMENT_FOCUS;
		inner->lefttree->flow->flotype = FLOW_SINGLETON;
		inner->lefttree->flow->islocal = true;
		
		/* Adjust the flow type of the intermediate Hash node. */
		inner->flow->flotype = FLOW_SINGLETON;
		inner->flow->islocal = true;
		
		/* The result flow of the HashJoin is also local. */
		joinflow = makeDispatcherFlow();
	}

	/* Else If both argument flows are partitioned...
	 */
	else if (  inner->flow->flotype == FLOW_PARTITIONED
			&& outer->flow->flotype == FLOW_PARTITIONED )
	{
		/* We aren't clever enough (yet) to know on what they're
		 * partitioned or to make a smart decision about how best to
		 * bring them into useful alignment, so we just repartition
		 * both sides.
		 */
		
		/* Get the hash expressions.  Note that the left set will also apply
		 * to the child of the Hash (since Hash nodes do no projection).
		 */
		List *outerexpr = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
		List *innerexpr = getHashMotionInfoList(plan, INFOTYPE_RIGHTEXPR);
		List *hashOp = getHashMotionInfoList(plan, INFOTYPE_OP);
		
		outer->flow->req_move = MOVEMENT_REPARTITION;
		outer->flow->hashExpr = outerexpr;
		outer->flow->hashOp = hashOp;
		
		inner->lefttree->flow->req_move = MOVEMENT_REPARTITION;
		inner->lefttree->flow->hashExpr = innerexpr;
		inner->lefttree->flow->hashOp = hashOp;
		
		/* The Hash node, inner, is already designated partitioned. */
		 
		 joinflow = makePartitionedFlow();
	}
	
	/* At this point, on the assumption that all singleton flows are local,
	 * there are no remaining cases to consider.
	 */

	if ( joinflow != NULL )
	{
		/* We were suceeded in defining the flow. */
		plan->flow = joinflow; 
		return false;
	}

	psError(context, ERRCODE_CDB_FEATURE_NOT_YET, 
		"CDB cannot parallelize that query yet.", 
		"Query hash joins subqueries of unexpected type."
		);
	return true;	 
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
				
				SortedMotionDefn *sortdef = NULL;
				if ( flow->req_move!=MOVEMENT_NONE && flow->numSortCols>0 )
				{
					sortdef = makeSimpleSortedMotionDefn(
						flow->numSortCols,
						flow->sortColIdx,
						flow->sortOperators,
						CdbNumSegments
						);
				}
				
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
						if ( sortdef != NULL )
						{
							newnode = (Node*)make_sorted_union_motion(
								plan,
								sendToSecondary,
								CdbThisSegment,
								sortdef
								);
						}
						else
						{
							newnode = (Node*)make_union_motion(
								plan, 
								sendToSecondary, 
								CdbThisSegment
								);
						}
						break;
						
					case MOVEMENT_BROADCAST:
						if ( sortdef != NULL )
						{
							newnode = (Node*)make_sorted_broadcast_motion(
								plan,
								sendToSecondary,
								sortdef
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
						if ( sortdef != NULL )
						{
							newnode = (Node*)make_sorted_hashed_motion(
								plan,
								sendToSecondary,
								makeSimpleHashMotionDefn(
									flow->hashExpr,
									flow->hashOp,
									CdbNumSegments),
								sortdef
								);
						}
						else
						{
							newnode = (Node*)make_hashed_motion(
								plan, 
								sendToSecondary,
								makeSimpleHashMotionDefn(
									flow->hashExpr,
									flow->hashOp,
									CdbNumSegments)
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

