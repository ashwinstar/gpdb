/*-------------------------------------------------------------------------
 *
 * cdbagg.c
 *	  Generate aggregation strategies.
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
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "optimizer/clauses.h"
#include "parser/parse_oper.h"
#include "parser/parse_expr.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h" /* for ReleaseSysCache */
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbagg.h"
#include "executor/nodeMotion.h"

/* 
 * Context structure for helper function analyzeAggExpr_walker
 */
typedef struct
AggExprAnalysis 
{
	/* These fields are arguments of analysis. */
	int tleidx;
	bool is_grp_attr;
	
	/* These fields hold the cumulative results of the analysis. */
	int ndistinct; 
	Aggref *aggref_distinct; /* Working storage for distinct scanning.  */
	
	/* These are work fields for the analyzeAggExpr_walker. */
	int aggdepth; /* Number of Aggref nodes in stack. */
		
} AggExprAnalysis;


/* Helper Declarations */
static CdbAggStrategy *makeAggStrategy(Movement left, FlowType result);
static void initAggExprAnalysis(AggExprAnalysis *aa);
static bool analyzeAggExpr(Expr* expr, Index tleidx, bool is_grp_attr, AggExprAnalysis *state);
static bool analyzeAggExpr_walker (Node *node, void *context);


void 
initAggExprAnalysis(AggExprAnalysis *aa)
{
	aa->tleidx = 0;
	aa->is_grp_attr = FALSE;
	aa->ndistinct = 0;
	aa->aggref_distinct = NULL;
	aa->aggdepth = 0;
}



/*
 * Function: analyzeAggExpr
 *
 * Return TRUE to indicate that analysis completed successfully, FALSE
 * to indicate that problems prevented completion.
 */
bool
analyzeAggExpr(Expr* expr, Index tleidx, bool is_grp_attr, AggExprAnalysis *state)
{
	bool result = TRUE;
	Node * node;
	
	Assert( state != NULL );
	node = (Node*)expr;
	
	/* Add arguments to state for walk.  */
	state->tleidx = tleidx;
	state->is_grp_attr = is_grp_attr;
	

	/* Recur into the expression. */
	result = !analyzeAggExpr_walker(node, state);
	
	return result;
}


/*
 * Do the heavy lifting for function analyzeAggExpr.
 */
static bool analyzeAggExpr_walker (Node *node, void *context)
{
	AggExprAnalysis* state = (AggExprAnalysis*)context;
	
	if (node == NULL)
		return false;
	
	switch ( nodeTag(node) )
	{
		case T_Aggref:
		{
			Aggref* aggref = (Aggref*) node;
			Assert( state->aggdepth == 0 ); /* No nesting Aggrefs! */
			Assert( !state->is_grp_attr ); /* No Aggrefs in grouping exprs. */
				
			if ( aggref->aggdistinct )
			{
				Assert( !aggref->aggstar ); /* No DISTINCT *! */
				state->ndistinct++;
				if ( state->ndistinct == 1 )
				{
					state->aggref_distinct = copyObject(aggref->target);
				}
				if ( state->ndistinct > 1 && state->aggref_distinct != NULL )
				{
					pfree( state->aggref_distinct );
					state->aggref_distinct = NULL;
				}
			}
			
			/* Recur to check subexpressions. */
			state->aggdepth++;
			if ( analyzeAggExpr_walker((Node*)(aggref->target), context) )
				return TRUE;
			state->aggdepth--;
		}
		break;
		
		default:
		{
			if ( expression_tree_walker(node, analyzeAggExpr_walker, context) )
				return true;
		}
		break;
	}
	return FALSE;
}


/*
 * Function getAggStrategyList generates a List of palloc'd CdbAggStrategy 
 * structures indicating how the given Agg node should be processed.  If no 
 * strategy can be found, it returns NULL.
 *
 * The returned strategy specifies motions and other transformations to apply 
 * to the aggregation argument and output flow characteristics that will 
 * result.
 *
 * Note that this function expects an Agg node whose child has been prescaned 
 * (see cdbllize.c) and, hence, have their output Flow node fully specified.
 *
 * Later, we might consider making CdbJoinStrategy a Node and returning 
 * a list of strategies for the caller to choose from. We would probably
 * do this in conjuction with changing the flow field of Plan to contain
 * a list of possible flow specifications.
 */
List *getAggStrategyList(Agg* agg)
{
	int i, n, tleidx;
	TargetEntry *tle;
	CdbAggStrategy *as;
	
	List *result = NIL;
	
	bool isgrouped = FALSE;
	Bitmapset* grpset = NULL;
	
	Plan *plan = (Plan*)agg;
	Plan *child;
	
	bool aOk = TRUE;
	AggExprAnalysis analysis;
	AggExprAnalysis *state = &analysis;
	
	Assert( plan && plan->lefttree->flow != NULL );
	
	child = plan->lefttree;	
		
	/* Find grouping expressions since we might want to hash on them. */
	 if ( agg->aggstrategy == AGG_SORTED || agg->aggstrategy == AGG_HASHED )
	 {
		n = list_length(plan->targetlist);
		for ( tleidx = 1; tleidx <= n; tleidx++ )
		{	
			tle = get_tle_by_resno( plan->targetlist, tleidx);
			
			if ( tle == NULL )
			{
				ereport(ERROR,(
					errcode(ERRCODE_CDB_INTERNAL_ERROR),
					errmsg("CDB: problem in Agg targetlist")
				));
			}
			
			if ( tle->resdom->ressortgroupref != 0 )
			{
				for ( i = 0; i < agg->numCols; i++ )
				{
					if ( tle->resdom->ressortgroupref == agg->grpColIdx[i] )
					{
						grpset = bms_add_member(grpset, tleidx);
						break; /* for */
					}
				}
			}
		}
	 }
	 isgrouped = !bms_is_empty(grpset);
	 
	 /*
	 grpexprs = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
	 grptypes = getHashMotionInfoList(plan, INFOTYPE_OP);
	 */
	 
	 /* Analyze the targetlist and qual for Aggref related information */
	 initAggExprAnalysis(state);
	 n = list_length(plan->targetlist);
	 for ( i = 0; aOk && i < n; i++ )
	 {
		tleidx = i+1;
		tle = (TargetEntry*)list_nth( plan->targetlist, i);
		aOk = aOk && analyzeAggExpr(tle->expr, tleidx, bms_is_member(tleidx, grpset), state);
	 }
	 aOk = aOk && analyzeAggExpr((Expr*)(plan->qual), 0, FALSE, state);

	
	/* First see whether we can get away with doing nothing. */
	if ( child->flow->flotype == FLOW_REPLICATED || child ->flow->flotype == FLOW_SINGLETON )
	{
		result = lappend(result,makeAggStrategy(MOVEMENT_NONE, child->flow->flotype));
	}
	
	if ( child->flow->flotype == FLOW_PARTITIONED )
	{
		if ( aOk )
		{
			if ( !isgrouped && state->ndistinct == 1  )
			{
				/* Can partition on the sole distinct expr. */
				as = makeAggStrategy(MOVEMENT_REPARTITION, FLOW_PARTITIONED);
				Assert( state->aggref_distinct != NULL );
				
				/* 
				 * TODO Externalize stuff in nodeMotion.c to extract expression from arbitrary spot. 
				 * The code below the error is incorrect.  The code in the isgrouped branch is good.
				 */
				ereport(ERROR,(
					errcode(ERRCODE_CDB_INTERNAL_ERROR),
					errmsg("can't extract hash expr for distinct aggregrate")
				));
				
				as->hashexprs = list_make1(state->aggref_distinct);
				as->hashtypes = list_make1_oid(exprType((Node*)(state->aggref_distinct)));
				result = lappend(result,as);
				as = NULL;
			}
			else if ( isgrouped && state->ndistinct == 0 )
			{
				/* Can partition on grouping exprs. */
				/* TODO -- Fix ressortgroupref bug in getHashMotioonInfoList */
				as = makeAggStrategy(MOVEMENT_REPARTITION, FLOW_PARTITIONED);
				as->hashexprs = getHashMotionInfoList(plan, INFOTYPE_LEFTEXPR);
				as->hashtypes = getHashMotionInfoList(plan, INFOTYPE_OP);
				result = lappend(result,as);
				as = NULL;
			}
		}
		
		/* In any case, can replicate or focus. */
		result = lappend(result, makeAggStrategy(MOVEMENT_FOCUS, FLOW_SINGLETON));
		result = lappend(result, makeAggStrategy(MOVEMENT_BROADCAST, FLOW_REPLICATED));
	}
	
	return result;
}


/*
 * Helper for getAggStrategyList -- palloc and initialize a strategy.
 */
CdbAggStrategy *makeAggStrategy(Movement left, FlowType result)
{
	CdbAggStrategy *as = palloc(sizeof(CdbAggStrategy));
	as->leftmove = left;
	as->resultflowtype = result;
	as->resultislocal = ( result == FLOW_SINGLETON );
	as->hashexprs = NIL;
	as->hashtypes = NIL;
	return as;
}	
/*
 * Make a palloc'd copy of the argument strategy.
 */
CdbAggStrategy *copyAggStrategy(CdbAggStrategy *old)
{
	CdbAggStrategy *as = NULL;
	
	if  ( old != NULL )
	{
		as = makeAggStrategy(old->leftmove, old->resultflowtype);
		as->resultislocal = old->resultislocal;
		as->hashexprs = (List*)copyObject(old->hashexprs);
		as->hashtypes = (List*)copyObject(old->hashtypes);
	}
	return as;
}	

/*
 * Free a result of getAggStrategyList().  Caller should NIL out any
 * copies of pointers to the list.
 */
void freeAggStrategyList(List* aslist)
{
	ListCell *cell;
	CdbAggStrategy *stgy;
	
	/* Free dependent items of each strategy. */
	foreach (cell, aslist)
	{
		stgy = (CdbAggStrategy*)lfirst(cell);
		if ( stgy->hashexprs != NULL )
		{
			pfree(stgy->hashexprs);
			stgy->hashexprs = NULL;
		}
		if ( stgy->hashtypes != NULL )
		{
			pfree(stgy->hashtypes);
			stgy->hashtypes = NULL;
		}
	}
	
	/* Free list and strategies. */
	list_free_deep(aslist);
}

