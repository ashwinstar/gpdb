/*-------------------------------------------------------------------------
*
* cdbaggmutate.c
*	  Restructure an Agg node into partial and final Agg nodes with
*     an intervening Motion node.
*
* Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
* Portions Copyright (c) 2005, Metapa Inc
*
*
* IDENTIFICATION
*	  $Header$
*
*-------------------------------------------------------------------------
*/
#include "postgres.h"

#include <ctype.h>

#include "lib/stringinfo.h"
#include "optimizer/clauses.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "nodes/makefuncs.h"
#include "nodes/print.h"
#include "access/relscan.h"
#include "access/tupdesc.h"
#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/guc.h"
#include "utils/datum.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

#include "parser/parse_expr.h" /* exprType() and exprTypmod(). */
#include "cdb/cdbagg.h"


/* --------------------------------------------------------------------------
 * Utility structures
 * --------------------------------------------------------------------------
 */

typedef struct
SplitAggsContext
{
	int attr_count; /* Number of child attributes. */
	AttrNumber *attr_map; /* Child grouped attribute i is Intermediate attribute attr_map[i] */
	
	List *inner_tlist; /* This will be the target list of the intermediate. */
	
	bool mutatingAggref;
	
	/* Implicit result of any helper. */
	char * errmsg;
	
} SplitAggsContext;


/* Declare forward. */
static void initSplitAggsContext(SplitAggsContext* context);
static Node *splitAggs(Node *node, SplitAggsContext *context);
static Node *splitAggs_mutator(Node* node, SplitAggsContext *context);
static Var *addInnerTle(Expr* expr, SplitAggsContext* context);


/* 
 * Function: initSplitAggsContext
 *
 * Helper for function: xformAggregate
 *
 * Initialize a SplitAggsContext structure.
 */
static void initSplitAggsContext(SplitAggsContext* context)
{
	context->attr_map = NULL;
	context->inner_tlist = NIL;
	
	context->mutatingAggref = FALSE;

	context->errmsg = NULL;
}


/*
 * Function: addInnerTle
 * 
 * Helper for function:  xformAggregate 
 *
 * Add the given expression to the the inner query target list currently 
 * under construction in the given context.  Return a Var node by which 
 * the outer query may refer to the new target entry in the inner query.
 *
 * TODO Do we need more info for the Var or the Resdom?
 */
static Var * 
addInnerTle(Expr* expr, SplitAggsContext* context)
{
	TargetEntry *inner_tle;
	Resdom *resdom;
	AttrNumber attno;
	char *attname;
	StringInfoData str;
	
	/* This will be our attribute number in the intermediate. */
	attno = list_length(context->inner_tlist);
	
	/* Make up a name for the attribute. */
	initStringInfo(&str);
	appendStringInfo(&str, "agg_%d", attno);
	attname = str.data;

	/* Create a new Resdom node for the TLE. */
	resdom = makeResdom(attno,
						exprType((Node*)expr),
						exprTypmod((Node*)expr),
						attname,
						FALSE);
	
	resdom->ressortgroupref = 0;		
	
	/* Make the new target list entry. */
	inner_tle = makeNode(TargetEntry);
	inner_tle->expr = expr;
	inner_tle->resdom = resdom;
	context->inner_tlist = lappend(context->inner_tlist, inner_tle);
	
	/* Return a Var node by which the outer query can refer to this */
	Var *var = makeVar(0,
					   attno,
					   resdom->restype,
					   resdom->restypmod,
					   0);
	
	return var;
}


 /*
  * Function: apply_agg_motion
  *
  * An AGGREGATE transformation restructures a given Agg node into an 
  * intermediate Agg node that computes a partial result (per segment
  * database) and a final Agg node that computes final result (per segment
  * or on a specific segment).  The result of the final Agg is just the
  * the desired result of the given Agg node.
  *
  * In addition, a Motion node is introduced between the Agg nodes.  This
  * either focuses the partial results onto to a single database process
  * or repartitions the partial results on the grouping attributes.
  *
  * The flow characteristics of the final Agg node depend on the motion.
  * In the repartitioning case, the hash specifications must be in terms
  * of the final Agg (which, of course, is congruent to the original.)
  */
Plan *
apply_agg_motion(Plan *plan, ApplyMotionState *state)
{
	Agg *agg;
	Agg *inner_agg;
	Plan *inner_plan;
	int i, n;
	ListCell *cell;
	TargetEntry *tle;
	SplitAggsContext context;
	
	/* Sanity checks */
	Assert( 
		plan != NULL
		&& IsA(plan,Agg) 
		&& plan->flow->req_move == MOVEMENT_AGG_RESTRUCT 
		&& plan->lefttree != NULL
		&& plan->righttree == NULL
		&& plan->initPlan == NIL
		&& bms_is_empty(plan->extParam)
		&& bms_is_empty(plan->allParam)
	);

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )
	
	/* The idea is to incrementally transform the argument Agg into what
	 * will serve as the outer Agg and to construct the inner Agg anew.
	 */
	 
	agg = (Agg*)plan;
	FLATCOPY(inner_agg, agg, Agg);
	inner_plan = (Plan*)inner_agg;
	
	inner_plan->targetlist = NIL;
	inner_plan->qual = NIL;
	
	/* Create our local context. */
	initSplitAggsContext(&context);
	
	/*
	 * Begin constructing the target list for the intermediate (partial) Agg 
	 * node by adding entries corresponding to the grouping attributes.  We 
	 * can take advantage of (and verify) the fact that the targetlist of the
	 * given Agg node must contain simple, Var-type target list entries for 
	 * the grouping columns--we can just find them and copy them to the list
	 * we're buidling.  
	 *
	 * Note that we also record the location of the grouping attributes of the 
	 * intermediate Agg in a lookup table (attr_map) so we can convert Var 
	 * references from referring-to-child-plan to referring-to-intermediate.
	 * This also gives us a way to check whether a Var reference is to a
	 * grouped or ungrouped attribute.
	 */
	context.attr_count = list_length(inner_plan->targetlist);
	n = 1 + context.attr_count;
	context.attr_map = (AttrNumber*)palloc(n * sizeof(AttrNumber));

	
	for ( i = 0; i < n; i++ )
	{
		context.attr_map[i] = -1; /* Initially flag all as non-grouping. */
	}
	for ( i = 0; i <  agg->numCols; i++ )
	{
		AttrNumber g = agg->grpColIdx[i];
		bool found = false;
		foreach( cell, plan->targetlist )
		{
			tle = (TargetEntry*)lfirst(cell);
			if ( IsA(tle->expr,Var) && ((Var*)tle->expr)->varattno == g )
			{
				found = true;
				break;
			}	
		}
		Assert(found); /* TODO Should we issue an error instead? */
		Assert( 0 < g && g < n );
		
		context.inner_tlist = lappend(context.inner_tlist, copyObject(tle));
		context.attr_map[g] = list_length(inner_plan->targetlist);
	}
	
	
	/* Now mutate the given Agg's target list and qual (the HAVING clause)
	 * to refer to the result of the intermediate Agg (rather than the child 
	 * plan).  As we go, we add any necessary attributes (Aggref expressions)
	 * to the intermediate Agg target list so we can refer to them in the 
	 * mutated outer target list (or qual).
	 */
	foreach( cell, plan->targetlist)
	{		
		tle = (TargetEntry*)(lfirst(cell));
		tle->expr = (Expr*)splitAggs((Node*)tle->expr, &context);
	}
	
	plan->qual = (List*)splitAggs((Node*)plan->qual, &context);
	
	
	/* Compose the inner query and make it into a subquery RTE, the sole
	 * entry in the outer query's range table.
	 */
	inner_plan->targetlist = context.inner_tlist;
	context.inner_tlist = NIL;
	
	
	elog_node_display(DEBUG1, "CDB apply_agg_motion Outer: ", plan, Debug_pretty_print);
	elog_node_display(DEBUG1, "CDB apply_agg_motion Inner: ", inner_plan, Debug_pretty_print);
	
	return plan;
}


/* 
 * Function: splitAggs
 *
 * Given an expression node from the given Agg node, return a mutated version
 * of the node in which references to grouping attributes or Aggref nodes
 * are replaced by suitable references to attributes in the target list
 * being constructed in the inner_tlist field of the context.
 * 
 * Add any needed partial aggregation expressions to the inner_tlist. 
 *
 * The function is expected to be called either with the expression of
 * a target list entry from in the given Agg, or with the boolean
 * expression contained in the qual of the given Agg.
 */
static Node *
splitAggs(Node *node, SplitAggsContext *context)
{
	context->mutatingAggref = FALSE; /* Reset state. */
	return splitAggs_mutator(node, context);
}

/*
 * Function: splitAggs_mutator
 *
 * This is the work horse for function splitAggs.
 * 
 * Deep copy the given expression tree down to any Aggref nodes. 
 *
 * On encountering an Aggref node:
 *
 *    Based on the type of aggregation, update the inner_tlist under
 *    construction in the context by adding TargetEntry nodes for any 
 *    partial aggregation results needed to compute that Aggref's result.
 *    These will eventually become the target list of the intermediate plan.
 *
 *    Instead of returning the Aggref, return a pointer to a new expression 
 *    that computes the desired Aggref result from the partial results just
 *    added to (or found in) the inner_tlist.
 *
 * On encountering a Var node outside an Aggref:
 *
 *    Check that the Var refers to a grouping attribute and return a
 *    Var referring to the corresponding grouping attribute in the 
 *    inner_tlist
 *
 */
static Node * 
splitAggs_mutator(Node* node, SplitAggsContext *context)
{
	Node *new_expr;
	
	if (node == NULL)
		return NULL;
	
	if ( IsA(node, Aggref) )
	{
		int i;
		Aggref* aggref = (Aggref*)node;
		
		Assert( !context->mutatingAggref ); /* Aggrefs don't nest */
		Assert( !aggref->aggdistinct ); /* We're not smart enought to do this. */
		context->mutatingAggref = TRUE;
		
		/* Get the specific tactic by which to parallelize this aggregate
		 * function call. 
		 */
		CdbAggTactic *tactic = getAggTactic(aggref, &(context->errmsg));
		if ( context->errmsg != NULL || tactic == NULL ) 
		{
			freeAggTactic(tactic);
			if ( context->errmsg == NULL )
				context->errmsg = "CDB has no tactic for implementing this aggregation";
			context->mutatingAggref = FALSE;
			return NULL;
		}
		
		/* Record partial aggregation(s) for inner target list. 
		 * This involves construction of one or more partial aggregration 
		 * expressions for execution on the tails and Var nodes by which
		 * the head query may refer to their results.
		 */
		for ( i = 0; i < tactic->numPartials; i++ )
		{
			CdbAggPartial *partial = &tactic->partials[i];

			Aggref *partial_agg;
			FLATCOPY(partial_agg,aggref,Aggref);
			partial_agg->aggfnoid = partial->aggfnoid;
			partial_agg->aggtype = partial->rettype;
			
			if ( partial->argfnoid != InvalidOid )
			{
				Assert( !partial_agg->aggstar );
				
				/* Apply the function to the argument. */
				FuncExpr *expr = makeFuncExpr(partial->argfnoid,
											  partial->argtype,
											  list_make1(partial_agg->target),
											  COERCE_EXPLICIT_CALL);
				partial_agg->target = (Expr*)expr;
			}
			
			
			partial->var = addInnerTle((Expr*)partial_agg, context);
		}
		
		/* Revise argument aggregate call to compute result from fragments.
		 * This involves construction of one or more final aggregation
		 * expressions that take their arguments from the partial aggregation
		 * expressions constructed above and the construction of the outer
		 * consolidating expression that calculates the result.
		 */
		for ( i = 0; i < tactic->numFinals; i++ )
		{
			CdbAggFinal *final = &tactic->finals[i];
			CdbAggPartial*partial = &tactic->partials[final->argindex];
			
			Aggref *final_agg = makeNode(Aggref);
			final_agg->aggfnoid = final->aggfnoid;
			final_agg->aggtype = final->rettype;
			final_agg->agglevelsup = 0;
			final_agg->aggstar = FALSE;
			final_agg->aggdistinct = FALSE;
			
			if ( final->argfnoid != InvalidOid )
			{
				/* Apply the function to the argument. */
				FuncExpr *expr = makeFuncExpr(final->argfnoid,
											  final->argtype,
											  list_make1(partial->var),
											  COERCE_EXPLICIT_CALL);
				final_agg->target = (Expr*)expr;
			}
			else
			{
				final_agg->target = (Expr*)(partial->var);
			}
			
			/* Cache the Expr for use in the consolidator call. */
			final->expr = (Expr*)final_agg;
		}

		/* Finally we're ready to put together the call to the consolidator
		 * in order to create the result of the aggregate from the final
		 * aggregation results developed above.
		 */
		if ( tactic->consolidator == InvalidOid )
		{
			/* Use the single final aggregate as the result. */
			Assert( tactic->numFinals == 1 );
			new_expr = (Node*)(tactic->finals[0].expr);
		}
		else
		{
			List *args;
			
			for ( i = 0; i < tactic->numFinals; i++ )
			{
				args = lappend(args, tactic->finals[i].expr);
			}
			
			FuncExpr *expr = makeFuncExpr(tactic->consolidator,
										  aggref->aggtype,
										  args,
										  COERCE_EXPLICIT_CALL);
			new_expr = (Node*)expr;
		}
		
		context->mutatingAggref = FALSE;
		return new_expr;
	}
	else if ( IsA(node,Var) && !context->mutatingAggref  )
	{
		Var *var = (Var*)copyObject(node);
		Assert( 0 < var->varattno && var->varattno <= context->attr_count );
		
		var->varattno = context->attr_map[var->varattno]; /* Remap */
		
		Assert( var->varattno > 0 ); /* Better be a grouping attribute! */

		return (Node*)var;
	}
	
	/* By default, just keep mutating. */
	return expression_tree_mutator(node, splitAggs_mutator, (void*) context);
}

