/*-------------------------------------------------------------------------
 *
 * cdbplan.c
 *	  Provides routines supporting plan tree manipulation.
 *
 * Copyright (c) 2004, Metapa
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *	See src/backend/optimizer/util/clauses.c for background information
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "cdb/cdbplan.h"

static bool walk_plan_node_fields(Plan *plan, bool (*walker) (), void *context);
static bool walk_scan_node_fields(Scan *scan, bool (*walker) (), void *context);
static bool walk_join_node_fields(Join *join, bool (*walker) (), void *context);

static bool plan_inventory_walker(Node *node, PlanInventory *pinventory);

/* Functions plan_inventory() and plan_inventory_walker() demonstrate use
 * of the plan_tree_walker() framework and will eventually be replaced by
 * one or more useful tools.
 *
 * For now plan_inventory() just counts the Var and Scan nodes in a plan
 * tree and reports back via a PlanInventory structure allocated by the
 * caller.  Just a toy for now.
 */
bool
plan_inventory(Plan *plan, PlanInventory *pinventory)
{
	pinventory->vars = 0;
	pinventory->scans = 0;
	
	return ! plan_inventory_walker((Node*)plan, pinventory);
}

bool 
plan_inventory_walker(Node *node, PlanInventory *pinventory)
{	
	if ( node == NULL )
		return false;
	
	switch (nodeTag(node))
	{
		case T_Var:

			/* Process the Var node. */
			(pinventory->vars)++;
			
			/* Var is primitive, no need to recur. */
			return false; 
			
		case T_SeqScan:
		case T_FunctionScan:
		case T_IndexScan:
		case T_TidScan:
		case T_SubqueryScan:

			/* Process the Scan node */
			(pinventory->scans)++;
			
			/* Scan nodes are complex so let plan_tree_walker() recur on 
			 * their sub-nodes. 
			 */
			break;
			
		default:
			/* Let plan_tree_walker() deal with other node types. */
			break; 
	}
	return plan_tree_walker(node, plan_inventory_walker, pinventory);

}

/* Function walk_plan_node_fields is a subroutine used by plan_tree_walker() 
 * to walk the fields of Plan nodes.  Plan is actually an abstract superclass 
 * of all plan nodes and this function encapsulates the common structure.
 *
 * Most specific walkers won't need to call this function, but complicated
 * ones may find it a useful utility.  
 *
 * Caution: walk_scan_node_fields and walk_join_node_fields call this 
 * function.  Use only the most specific function.
 */
bool
walk_plan_node_fields(Plan *plan, 
						bool (*walker) (), 
						void *context)
{
	/* target list to be computed at this node */
	if (walker((Node *)(plan->targetlist), context))
		return true;
	
	/* implicitly ANDed qual conditions */
	if (walker((Node *)(plan->qual), context))
		return true;
	
	/* input plan tree(s) */
	if (walker((Node *)(plan->lefttree), context))
		return true;
	
	/* target list to be computed at this node */
	if (walker((Node *)(plan->righttree), context))
		return true;
	
	/* target list to be computed at this node */
	if (walker((Node *)(plan->targetlist), context))
		return true;
	
	/* Init Plan nodes (uncorrelated expr subselects */
	if (walker((Node *)(plan->initPlan), context))
		return true;
		
	return false;
}


/* Function walk_scan_node_fields is a subroutine used by plan_tree_walker() 
 * to walk the fields of Scan nodes.  Scan is actually an abstract superclass 
 * of all scan nodes and a subclass of Plan.  This function encapsulates the 
 * common structure.
 *
 * Most specific walkers won't need to call this function, but complicated
 * ones may find it a useful utility.  
 *
 * Caution: This function calls walk_plan_node_fields so callers shouldn't,
 * else they will walk common plan fields twice.
 */
bool
walk_scan_node_fields(Scan *scan,
						bool (*walker) (),
						void *context)
{
	/* A Scan node is a kind of Plan node. */
	if ( walk_plan_node_fields((Plan*)scan,  walker, context) )
		return true;
	
	/* The only additional field is an Index so no extra walking. */
	return false;
}


/* Function walk_join_node_fields is a subroutine used by plan_tree_walker() 
 * to walk the fields of Join nodes.  Join is actually an abstract superclass 
 * of all join nodes and a subclass of Plan.  This function encapsulates the 
 * common structure.
 *
 * Most specific walkers won't need to call this function, but complicated
 * ones may find it a useful utility.  
 *
 * Caution: This function calls walk_plan_node_fields so callers shouldn't,
 * else they will walk common plan fields twice.
 */
bool
walk_join_node_fields(Join *join,
						bool (*walker) (),
						void *context)
{
	/* A Join node is a kind of Plan node. */
	if ( walk_plan_node_fields((Plan*)join, walker, context) )
		return true;
	
	return walker((Node*)(join->joinqual), walker, context);
}


/* Function plan_tree_walker is a general walker for Plan trees.
 *
 * It is based on (and uses) the expression_tree_walker() framework from 
 * src/backend/optimizer/util/clauses.c.  See the comments there for more
 * insight into how this function works.
 *
 * The basic idea is that this function (and its helpers) walk plan-specific
 * nodes and delegate other nodes to expression_tree_walker().  The caller
 * may supply a specialized walker
 */
bool
plan_tree_walker(Node *node,
					   bool (*walker) (),
					   void *context)
{
	/*
	 * The walker has already visited the current node, and so we need
	 * only recurse into any sub-nodes it has.
	 *
	 * We assume that the walker is not interested in List nodes per se, so
	 * when we expect a List we just recurse directly to self without
	 * bothering to call the walker.
	 */
	if (node == NULL)
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		/* Plan nodes aren't handled by expression_tree_walker, so we
		 * need to do them here.
		 */
		case T_Plan:
			/* Abstract: really should see only subclasses. */
			return walk_plan_node_fields((Plan*)node, walker, context);
			
		case T_Result:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			if ( walker((Node*)((Result*)node)->resconstantqual, context) )
				return true;
			break;
				
		case T_Append:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			if ( walker((Node*)((Append*)node)->appendplans, context) )
				return true;
			break;
				
		case T_Scan: 
			/* Abstract: really should see only subclasses. */
			return walk_scan_node_fields((Scan*)node, walker, context);
			
		case T_SeqScan:
		case T_FunctionScan:
			if ( walk_scan_node_fields((Scan*)node, walker, context) )
				return true;
			break;
			
		case T_IndexScan:
			if ( walk_scan_node_fields((Scan*)node, walker, context) )
				return true;
			if ( walker((Node *)((IndexScan*)node)->indxqual, context))
				return true;
			/* Other fields are lists of basic items, nothing to walk. */
			break;
				
		case T_TidScan:
			if ( walk_scan_node_fields((Scan*)node, walker, context) )
				return true;
			if ( walker((Node *)((TidScan*)node)->tideval, context) )
				return true;
			break;
				
		case T_SubqueryScan:
			if ( walk_scan_node_fields((Scan*)node, walker, context) )
				return true;
			if ( walker((Node *)((SubqueryScan*)node)->subplan, context) )
				return true;
			break;
				
		case T_Join:
			/* Abstract: really should see only subclasses. */
			return walk_join_node_fields((Join*)node, walker, context);
						
		case T_NestLoop:
			if ( walk_join_node_fields((Join*)node, walker, context) )
				return true;
			break;
				
		case T_MergeJoin:
			if ( walk_join_node_fields((Join*)node, walker, context) )
				return true;
			if ( walker((Node*)((MergeJoin*)node)->mergeclauses, context) )
				return true;
			break;
				
		case T_HashJoin:
			if ( walk_join_node_fields((Join*)node, walker, context) )
				return true;
			if ( walker((Node *)((HashJoin*)node)->hashclauses, context) )
				return true;
			break;
				
		case T_Material:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			break;
				
		case T_Sort:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			/* Other fields are simple counts and lists of indexes and oids. */
			break;
			
		case T_Group:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			/* Other fields are simple count and lists of indexes. */
			break;
			
		case T_Agg:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;
			
		case T_Unique:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;
			
		case T_Hash:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			/* Other info is in parent HashJoin node. */
			break;
			
		case T_SetOp:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;
			
		case T_Limit:
			if (  walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;
			
		case T_Motion:
			if ( walk_plan_node_fields((Plan*)node, walker, context) )
				return true;
			{
				Motion *motion = (Motion*)node;
				
				if ( motion->pHashDefn != NULL )
				{
					if ( walker(motion->pHashDefn->hashExpr, context) )
						return true;
				}
				/* Nothing to walk in FixedMotionDefn or SortMotionDefn. */
			}
			break;

	
		/* Query nodes are handled by the Postgres query_tree_walker.
		 * We just use it without setting any ignore flags.  The caller
		 * must handle query nodes specially to get other behavior, 
		 * e.g. by calling query_tree_walker directly.
		 */
		case T_Query:
			return query_tree_walker((Query*)node, walker, context, 0);
		
		/* The following cases are handled by expression_tree_walker.  In 
		 * addition, we let expression_tree_walker handle unrecognized nodes.
		 *
		 * TODO: Identify node types that should never appear in plan trees
		 *		 and disallow them here by issuing an error or asserting 
		 *		 false.
		 */
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_CaseTestExpr:
		case T_SetToDefault:
		case T_RangeTblRef:
		case T_Aggref:
		case T_ArrayRef:
		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_ScalarArrayOpExpr:
		case T_BoolExpr:
		case T_SubLink:
		case T_SubPlan:
		case T_FieldSelect:
		case T_FieldStore:
		case T_RelabelType:
		case T_CaseExpr:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_NullIfExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CoerceToDomain:
		case T_TargetEntry:
		case T_List:
		case T_FromExpr:
		case T_JoinExpr:
		case T_SetOperationStmt:
		case T_InClauseInfo:
			
		default:
			return expression_tree_walker(node, walker, context);
	}
	return false;
}
