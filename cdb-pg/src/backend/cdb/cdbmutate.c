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

/*
 * An ApplyMotionState holds state for the recursive apply_motion_mutator().
 */
typedef struct ApplyMotionState
{
	int nextMotionID;  
	
	/* TODO Finish the mutator which will eventually need some state:
	 *      At a minimum, a range table, and perhaps more.
	 */
} ApplyMotionState;


/*
 * Forward Declarations
 */
static Node* apply_motion_mutator(Node*  node, ApplyMotionState *context);
static void assignMotionID(Node* newnode, ApplyMotionState* context, Node* oldnode);

/* TODO Check suitability of the approach taken in these functions. */
static void set_hash_in_flow(Plan *plan, int nattrs, int *attrs);
static bool force_vars_walker(Node *node, void *varno);



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
Plan* apply_motion(Plan*  plan, Query* query)
{
	ApplyMotionState state;
	state.nextMotionID = 1; /* Start at 1 so zero will mean "unassigned". */
	
	Assert( is_plan_node((Node*)plan) && IsA(query,Query) );
	
	switch ( query->commandType )
	{
		case CMD_SELECT:
			/*
			 * [Re]focus the flow on the dispatch stream.  Note that, if
			 * the top node's Flow is marked sorted, the movement request
			 * becomes a request for ordered motion.
			 *
			 * TODO Fix top-level sort handling.
			 *      Currently there is a good  chance that we'll encounter
			 *      a flow that the optimizer knew was ordered but that
			 *      we have failed to flag as such.  As an ugly stop-gap,
			 *      we might check the query for an ORDER BY and slap on a
			 *      Sort node.  This would only be a band-aid, though, since 
			 *      it doesn't address deeper problems involving interior
			 *      nodes that depend on order, e.g.,  LIMIT and OFFSET in  
			 *      ordered subqueries.
			 */
			if ( plan->flow->flotype == FLOW_PARTITIONED 
				|| ( plan->flow->flotype == FLOW_SINGLETON && plan->flow->segindex  != -1 )
				)
			{
				/* Flow is partitioned or singleton on a segment.  Refocus. */
				plan->flow->req_move = MOVEMENT_FOCUS;
				plan->flow->segindex = -1;
			}
			else if ( plan->flow->flotype == FLOW_SINGLETON && plan->flow->segindex == -1 )
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
					errmsg("unexpected flow type at top of plan")
				 ));
			}
			break;
		
		case CMD_INSERT:
			{
				/*
				 * TODO Remove this block to enable INSERT processing.
				 */
				ereport(ERROR, (
					errcode(ERRCODE_CDB_FEATURE_NOT_YET),
					errmsg("CDB cannot parallelize INSERTs yet")
				));
			}
			/*
			 * Make sure the top level flow is partitioned on the partitioning
			 * key of the target relation.  Since this is an INSERT command,
			 * the target list will correspond to the attributes of the target
			 * relation in order.
			 */
			{
				RangeTblEntry *rte;
				CdbPolicy *policy;
				
				/* TODO Are we sure the INSERT target isn't an inheritance set?
				 *      Should we check, or would it work fine anyway?
				 */
				rte = rt_fetch(query->resultRelation, query->rtable);
				Assert( rte->rtekind == RTE_RELATION);
				policy = get_table_policy(rte->relid);
				 
				/* TODO Generalize this to deal with dispatcher singletons
				 *      as the target relation.  For now, mixed queries are
				 *      not allowed, so...
				 */
				Assert( policy->ptype == POLICYTYPE_PARTITIONED ); 				
			
				/*
				 * Deal with the source of the INSERT based on its flow
				 * characteristics.
				 */
				if ( plan->flow->flotype == FLOW_PARTITIONED 
					|| ( plan->flow->flotype == FLOW_SINGLETON && plan->flow->segindex != -1 )
					)
				{
					/* 
					 * Source is partitioned or singleton on a segment. 
					 * Refocus. 
					 */
					plan->flow->req_move = MOVEMENT_REPARTITION;
					set_hash_in_flow(plan, policy->nattrs, policy->attrs);
					
					/* Tried externalizing these instead, but EXPLAIN still failed.
					plan->flow->hashExpr = getExprListFromTargetList(plan->targetlist, policy->nattrs, policy->attrs);
					plan->flow->hashOp = getOpListFromTargetList(plan->targetlist, policy->nattrs, policy->attrs);
					*/
				}
				else if ( plan->flow->flotype == FLOW_REPLICATED )
				{
					/* 
					 * Source is replicated over segments. Apply "deep-six". 
					 */
					ereport(ERROR, (
						errcode(ERRCODE_CDB_FEATURE_NOT_YET),
						errmsg("CDB cannot parallelize INSERTs from replicated flows yet")
					));
				}
				else
				{
					/*
					 * Source is, presumably, a dispatcher singleton.
					 */
					ereport(ERROR, (
						errcode(ERRCODE_CDB_FEATURE_NOT_YET),
						errmsg("CDB cannot parallelize that INSERT yet")
					));
				}
			}
			break;
			
		case CMD_UPDATE:
		case CMD_DELETE:
			/*
			 * TODO Appropriate top-of-plan processing for other commands.  
			 *      Some UPDATEs and DELETEs may want the top-level partitioned 
			 *      flow unmodified (i.e., when it is effectively partitioned 
			 *      on tuple location).
			 */
			ereport(ERROR, (
				errcode(ERRCODE_CDB_FEATURE_NOT_YET),
				errmsg("CDB cannot parallelize %ss yet", 
					query->commandType==CMD_UPDATE ? "UPDATE" : "DELETE")
			));
			 
		default:
			Assert(false); /* Never see non-DML in here! */
			return NULL; 
	}
	
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
			
		/* Pre-existing Motion nodes must be renumbered. */
		case T_Motion:
			/* Motion nodes can't specify motion. */
			Assert( ((Plan*)newnode)->flow->req_move == MOVEMENT_NONE );
			/* Assign unique node number to the new node. */
			assignMotionID(newnode, context, node);
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
								flow->segindex, /* TODO if 0, adjust to "chosen" segment. */
								flow->numSortCols,
								flow->sortColIdx,
								flow->sortOperators,
								CdbIdentity.numsegments,
								makeDefaultSegIdxArray(CdbIdentity.numsegments)
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
						}
						else
						{
							newnode = (Node*)make_union_motion(
								plan, 
								sendToSecondary, 
								flow->segindex /* TODO if 0, adjust to "chosen" segment. */
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
						}
						break;
						
					case MOVEMENT_BROADCAST:
						if ( flow->req_move!=MOVEMENT_NONE && flow->numSortCols>0 )
						{
							Assert(false); /* This case disallowed. */
							newnode = (Node*)make_sorted_broadcast_motion(
								plan,
								sendToSecondary,
								flow->numSortCols,
								flow->sortColIdx,
								flow->sortOperators,
								CdbIdentity.numsegments,
								makeDefaultSegIdxArray(CdbIdentity.numsegments)
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
						}
						else
						{
							newnode = (Node*)make_broadcast_motion(
								plan, 
								sendToSecondary
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
						}
						break;
						
					case MOVEMENT_REPARTITION:
						Assert( flow->hashExpr != NULL && flow->hashOp != NULL );
						if ( flow->req_move!=MOVEMENT_NONE && flow->numSortCols>0 )
						{
							Assert(false); /* This case disallowed. */
							newnode = (Node*)make_sorted_hashed_motion(
								plan,
								sendToSecondary,
								CdbIdentity.numsegments,
								makeDefaultSegIdxArray(CdbIdentity.numsegments),
								flow->hashExpr,
								flow->hashOp,
								flow->numSortCols,
								flow->sortColIdx,
								flow->sortOperators,
								CdbIdentity.numsegments,
								makeDefaultSegIdxArray(CdbIdentity.numsegments)
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
						}
						else
						{
							newnode = (Node*)make_hashed_motion(
								plan, 
								sendToSecondary,
								CdbIdentity.numsegments,
								makeDefaultSegIdxArray(CdbIdentity.numsegments),
								flow->hashExpr,
								flow->hashOp								
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
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
 * Helper code for ApplyMotionState -- Assign motion id to new Motion node. 
 */
void assignMotionID(Node* newnode, ApplyMotionState* context, Node* oldnode)
{
	Assert( IsA(newnode,Motion) && context != NULL);
	Assert( oldnode == NULL || (IsA(oldnode,Motion) && oldnode != newnode) );
	
	/* Assign unique node number to the new node. */
	((Motion*)newnode)->motionID = context->nextMotionID++;
	
	/* Debugging hint that we have reassigned a previously assigned ID. */
	if ( oldnode != NULL && ((Motion*)oldnode)->motionID != 0 )
	{
		ereport(DEBUG2, (
			errmsg("Motion node renumbered: old=%d new=%d.",
				((Motion*)oldnode)->motionID,
				((Motion*)newnode)->motionID )
		));
	}
}


/*
 * This function is problematic. It is written just to get the sample
 * INSERTs to the tiny schema to flow through EXPLAIN:
 *
 *   insert into sale 
 *     select * from sale where pn > 1000;
 *
 *   insert into sale(pn,vn,cn) 
 *     select p.pn, v.vn, c.cn 
 *     from customer c, vendor v, product p 
 *     where pn=0 and vn=0 and  cn=0;
 *
 * TODO This function needs to be revisited!
 */
void set_hash_in_flow(Plan *plan, int nattrs, int *attrs)
{
	int i;
	AttrNumber attr;
	TargetEntry *tle;
	Node *expr;
	Oid eq_oid;
	List *exprs = NIL;
	List *opers = NIL;
	List *eqlst = list_make1((Node*)makeString("="));
	
	for ( i = 0; i < nattrs; i++ )
	{
		attr = attrs[i];
		tle = get_tle_by_resno(plan->targetlist, attr);
		Assert( tle != NULL );
		
		eq_oid = compatible_oper_opid(eqlst, tle->resdom->restype, tle->resdom->restype, true);
		
		if ( eq_oid == InvalidOid )
		{
			ereport(ERROR, (	
				errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg(
					"no equality operator for partitioning attribute of typid %d", 
					tle->resdom->restype
				)));

		}
		
		expr = copyObject(tle->expr);
		Index x = OUTER;
		force_vars_walker(expr,(void*)x);
		
		exprs = lappend(exprs, expr);
		opers = lappend_oid(opers, eq_oid);
	}
	
	plan->flow->hashExpr = exprs;
	plan->flow->hashOp = opers;
	pfree(eqlst);
	return;
}

/*
 * This quick hack just forces all the Var nodes in the argument to 
 * specify the given varno.  It is supplied as (void*)(Index)x in the
 * caller, set_hash_in_flow().
 */
bool force_vars_walker(Node *node, void *varno)
{
	if (node == NULL)
		return false;
	
	if (IsA(node, Var))
	{
		((Var*)node)->varno = (Index)varno;
		return false;
	}
	return expression_tree_walker(node, force_vars_walker, varno);
}

