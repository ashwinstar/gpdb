/*-------------------------------------------------------------------------
 *
 * cdbjoin.c
 *	  Generate join strategies.
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
#include "optimizer/clauses.h"
#include "parser/parse_oper.h"
#include "utils/lsyscache.h"
#include "cdb/cdbjoin.h"
#include "cdb/cdbvars.h"

static double disable_cost = 1.0E10;

typedef enum {
		RxR = 0,	RxL,	RxP,
		LxR,		LxL,	LxP,
		PxR,		PxL,	PxP
} JoinCombination;
	
typedef enum {
		InnerJoin,
		OuterLeft,
		OuterRight,
		OuterBoth,
		InJoin
} JoinDirection;

static CdbJoinStrategy *makeJoinStrategy(Movement left, Movement right, FlowType result);
static List *costStrategies(List *strategies, Join *join);
static double costMovement(Movement move, Plan *plan, JoinType jointype);
static List *sort_list(List *l, int (*cmpfn)(const void *, const void *));
static int cmpCdbJoinStrategy( const void *l, const void *r );

/*
 * Function getJoinStrategy generates a palloc'd strategy indicating how
 * the given Join node should be processed.  If no strategy can be found,
 * it returns NULL.
 *
 * The returned strategy specifies motions to apply to the join arguments
 * and output flow characteristics that will result.
 *
 * Note that this function expects a Join node whose children have been 
 * prescaned (see cdbllize.c) and, hence, have their output Flow node fully
 * specified.
 *
 * Later, we might consider making CdbJoinStrategy a Node and returning 
 * a list of strategies for the caller to choose from. We would probably
 * do this in conjuction with changing the flow field of Plan to contain
 * a list of possible flow specifications.
 *
 */
List *getJoinStrategyList(Join* join)
{
	JoinCombination jcomb;
	JoinDirection argreqs;
	List *result = NIL;
	
	Assert( ((Plan*)join)->lefttree->flow != NULL );
	Assert( ((Plan*)join)->righttree->flow != NULL );
	
	/* The following just calculates an index into the 3 x 3 matrix defined
	 * by the JoinCombination enumeration.  (The matrix is indexed as a 9
	 * element array.)  We use an enumeration instead of an array because
	 * we want to write chatty comments and because there are sub-cases 
	 * within some of the combinations.
	 */
	switch ( ((Plan*)join)->lefttree->flow->flotype )
	{
		case FLOW_REPLICATED:
			jcomb = 0*3;
			break;
		case FLOW_SINGLETON:
			jcomb = 1*3;
			break;
		case FLOW_PARTITIONED:
			jcomb = 2*3;
			break;
		case FLOW_UNDEFINED:
		default:
			return NIL;
	}
	
	switch (((Plan*)join)->righttree->flow->flotype )
	{
		case FLOW_REPLICATED:
			jcomb += 0;
			break;
		case FLOW_SINGLETON:
			jcomb += 1;
			break;
		case FLOW_PARTITIONED:
			jcomb += 2;
			break;
		case FLOW_UNDEFINED:
		default:
			return NIL;
	}

	/* Next determine what argument requirements are introduced by outer joins.
	 * Basically, if a join argument must produce at least one tuple due to an 
	 * outer join, then we need to be sure we see all the tuples of the other
	 * argument that might match it.  For example, to evaluate
	 *
	 *    L LEFT JOIN R ON L.A=R.A
	 *
	 * we need to be sure we see the full set of rows from R that might
	 * match a row from L.  If L were replicated and R were partitioned
	 * on A, then the join on a particular segment would produce spurious
	 * tuples  <L,null> corresponding to L tuples that match R tuples on
	 * other segments but not local R tuples.
	 */
	switch ( join->jointype )
	{
		case JOIN_INNER: 
			argreqs = InnerJoin;
			break;
		
		case JOIN_IN:
			argreqs = InJoin;
			break;
		
		case JOIN_LEFT: 
			argreqs = OuterLeft;
			break;
		
		case JOIN_FULL: 
			argreqs = OuterBoth;
			break;
		
		case JOIN_RIGHT:
			argreqs = OuterRight;
			break;

		case JOIN_REVERSE_IN:
		case JOIN_UNIQUE_INNER:
		case JOIN_UNIQUE_OUTER:
		case JOIN_UNION:
		default:
			/* These shouldn't happen in an executor-ready plan. */
			return NIL;
			break;
	}
	
	switch ( jcomb )
	{
		case RxR:
			/* Replicated flows can join in parallel.   The result is a
			 * replicated flow.  This case may never occur. */
			result = lappend(result,makeJoinStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_REPLICATED));
			break;
		
		case RxL:
		case LxR:
			/* Replicated and singleton where the singleton is on a segment
			 * can join in parallel, producing a singleton.  Currently,
			 * though, the only singleton flows are on the entry database,
			 * so the direct strategy is closed for now.  */
			 break;
			 
		case LxL:
			result = lappend(result,makeJoinStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_SINGLETON));
			break;
		
		case RxP:
			/* A replicated flow can join directly to a partitioned flow
			 * producing a partitioned flow, unless the replicated flow is on
			 * the "driving side" of an outer join.
			 */
			switch ( argreqs )
			{
				case InnerJoin:
				case OuterRight:
					/* Replicated flow isn't on the driving side. */
					result = lappend(result,makeJoinStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_PARTITIONED));
					/* Could also broadcast right side, but why? */
					break;
				
				case OuterLeft:
				case OuterBoth:
					/* Replicated flow is on the driving side.  We don't have
					 * a motion that can convert a replicated flow to a 
					 * partitioned one, so the only thing we can do is to
					 * broadcast the partitioned side making it replicated.
					 */
					result = lappend(result,makeJoinStrategy(MOVEMENT_NONE, MOVEMENT_BROADCAST, FLOW_REPLICATED));
					break;
				
				case InJoin:
					/* If partitioning is on join attributes, this would
					 * work and produce a partitioned result, but we don't
					 * check that case yet.
					 */
					break;
			}
			break;
		
		case PxR:
			/* A partitioned flow can join directly to a replicated flow
			 * producing partitioned flow, unless the replicated flow is on
			 * the "driving side" of an outer join.
			 */
			switch ( argreqs )
			{
				case InnerJoin:
				case InJoin:
				case OuterLeft:
					/* Replicated flow isn't on the driving side. */
					result = lappend(result,makeJoinStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_PARTITIONED));
					/* Could also broadcast left side, but why? */
					break;
				
				case OuterRight:
				case OuterBoth:
					/* Replicated flow is on the driving side.  We don't have
					 * a motion that can convert a replicated flow to a 
					 * partitioned one, so the only thing we can do is to
					 * broadcast the partitioned side making it replicated.
					 */
					result = lappend(result,makeJoinStrategy(MOVEMENT_BROADCAST, MOVEMENT_NONE, FLOW_REPLICATED));
					break;
			}
			break;
			
		/* Since tuples of a local flow may not be meaningful outside the
		 * current environment, to join a local flow to partitioned flow,
		 * we need to bring all the rows of the partitioned flow to the
		 * stream that carries the local flow. */		
		case LxP:
			result = lappend(result,makeJoinStrategy(MOVEMENT_NONE, MOVEMENT_FOCUS, FLOW_SINGLETON));
			break;		
		case PxL:
			/* Motion: Focus left on dispatcher. */
			/* Result: Singleton, Local */
			result = lappend(result,makeJoinStrategy(MOVEMENT_FOCUS, MOVEMENT_NONE, FLOW_SINGLETON));
			break;
			
		case PxP:
			/* Join two partitioned flows by repartitioning  them both on the
			 * join key. (It would be great if we were smart enought to notice
			 * whether one or both flow were already partitioned that way, but
			 * we're not, so both sides get repartitioned. */
			result = lappend(result,makeJoinStrategy(MOVEMENT_REPARTITION, MOVEMENT_REPARTITION, FLOW_PARTITIONED));
			if ( argreqs!=OuterRight && argreqs!=OuterBoth)
				result = lappend(result,makeJoinStrategy(MOVEMENT_NONE, MOVEMENT_BROADCAST, FLOW_PARTITIONED));
			if ( argreqs!=OuterLeft && argreqs!=OuterBoth)
				result = lappend(result,makeJoinStrategy(MOVEMENT_BROADCAST, MOVEMENT_NONE, FLOW_PARTITIONED));
			break;
	}

	/*
	 * evaluate estimated cost, store in est_ams_cost, and sort result list in order
	 * of ascending est_ams_cost
	 */
	result = costStrategies( result, join );

	return result;
}


/*
 * Helper for getJoinStrategy -- palloc and initialize a strategy.
 */
CdbJoinStrategy *makeJoinStrategy(Movement left, Movement right, FlowType result)
{
	CdbJoinStrategy *js = palloc(sizeof(CdbJoinStrategy));
	js->leftmove = left;
	js->rightmove = right;
	js->resultflowtype = result;
	js->est_ams_cost = 0.0;
	return js;
}	
/*
 * Make a palloc'd copy of the argument strategy.
 */
CdbJoinStrategy *copyJoinStrategy(CdbJoinStrategy *old)
{
	CdbJoinStrategy *js = NULL;
	
	if  ( old != NULL )
	{
		js = makeJoinStrategy(old->leftmove, old->rightmove, old->resultflowtype);
	}
	return js;
}	

/*
 * Free a result of getJoinStrategyList().  Caller should NIL out any
 * copies of pointers to the list.
 */
void freeJoinStrategyList(List* jslist)
{
	list_free_deep(jslist);
}

/*
 * costStrategies - 
 * This iterates over the strategies in the strategies List
 * and calls costMovement for the left and right sides.  
 * The costs are added and stored in est_ams_cost.
 * At the end, the strategy list is sorted based on
 * increasing est_ams_cost.
 */
List *costStrategies(List *strategies, Join *join)
{
	ListCell *cell;
	Plan *lefttree;
	Plan *righttree;

	if ( strategies == NIL )
		return NIL;

	Assert( join != NULL );

	lefttree = ((Plan *)join)->lefttree;
	righttree = ((Plan *)join)->righttree;
	
	foreach( cell, strategies )
	{
		CdbJoinStrategy *s = (CdbJoinStrategy*)lfirst(cell);
		double cost = 0.0;

		cost += costMovement(s->leftmove, lefttree, join->jointype);
		cost += costMovement(s->rightmove, righttree, join->jointype);

		s->est_ams_cost = cost;
	}

	/*
	 * sort_list creates a new list out of the same pointers
	 * contained in the passed-in list, in sorted order
	 * (based on the cmpCdbJoinStrategy function
	 */
	strategies = sort_list( strategies, cmpCdbJoinStrategy );

	return strategies;
}

/*
 * costMovement - 
 * This estimates the total number of bytes that have to be moved through
 * ams for the Plan node and movement type parameters.
 * Currently, the jointype is ignored, but may be used in the future
 * to refine the estimate.
 *
 * The formula starts with the estimated result bytes per segment (result_bytes_per_segment)
 * for the plan node result set.  This is equal to:
 *			rows * width
 * since it is assumed that the Plan structure's plan_rows field refers to
 * an average row number per segment.
 *
 * Then the total_result_bytes for all segments (total_result_bytes)
 *  is estimated as:
 *			 result_bytes_per_segment * numSegs
 * unless the flotype is a singleton, in which case the total result
 * bytes is set equal to the result_bytes_per_segment.
 *
 * Then, based on the Movement type, the final estimate (bytes_thru_ams) is calculated:
 *
 * For a focus movement:
 *			bytes_thru_ams = total_result_bytes
 * since all results are moved.
 *
 * For a repartition movement, 
 *			bytes_thru_ams = total_result_bytes *  (1.0 - 1.0/numSegs)
 * since is is assumed that the probablilty of a row hashing to its own segment
 * is 1/numSegs.
 *
 * For a broadcast movement,
 *			bytes_thru_ams = total_result_bytes *  (numSegs-1)
 * since every tuple has to be moved to each other segment.
 *
 * In the spirit of the postgres enable_hashjoin, enable_indexscan, etc guc variables,
 * there are two cdb guc variables that influence the cost for either repartitioning or
 * broadcast: enable_cdbrepartition and enable_cdbbroadcast.
 * If either is set off, a large constant is added to the cost for that type of Movement.
 * 
 */
double costMovement(Movement move, Plan *plan, JoinType	jointype)
{
	/*
	 *	Assume that plan_rows is the estimate of the
	 *  average number of rows per segment.
	 */
	double rows = plan->plan_rows;
	double width = (double)plan->plan_width;
	FlowType flotype = plan->flow->flotype;
	int numSegs = CdbIdentity.numsegments;

	double result_bytes_per_segment;
	double total_result_bytes;
	double bytes_thru_ams;

	Assert( numSegs > 0 );
	Assert( flotype != FLOW_UNDEFINED );

	result_bytes_per_segment = rows * width;

	total_result_bytes = result_bytes_per_segment;
	if ( flotype != FLOW_SINGLETON )
		total_result_bytes *= numSegs;

	switch ( move )
	{
		case MOVEMENT_NONE:
			bytes_thru_ams = 0.0;
			break;

		/*
		 *	When focusing back to the dispatcher, all rows are moved
		 */
		case MOVEMENT_FOCUS:
			bytes_thru_ams = total_result_bytes;
			break;

		/*
		 *	When broadcasting, all rows are moved to all other segments
		 */
		case MOVEMENT_BROADCAST:
			bytes_thru_ams = total_result_bytes * (numSegs-1);
			if ( !enable_cdbbroadcast )
				bytes_thru_ams += disable_cost;
			break;

		/*
		 *	When repartitioning, each row is moved to one other segment,
		 *  except when it hashes to itself.  Assume the prob of this
		 * is uniform, i.e. 1/numsegments
		 */
		case MOVEMENT_REPARTITION:
			bytes_thru_ams = total_result_bytes *  (1.0 - 1.0/numSegs);
			if ( !enable_cdbrepartition )
				bytes_thru_ams += disable_cost;
			break;

		/*
		 * When repartitioning from a replicated flow, no rows are moved.
		 * Those tuples that hash to a different segindex that thier present one
		 * are deep-sixed instead of moved.
		 */
		case MOVEMENT_REP2PARTITIONED:
			bytes_thru_ams = 0.0;
			if ( !enable_cdbrepartition )
				bytes_thru_ams += disable_cost;
			break;
		
		/* 
		 *
		 */
		case MOVEMENT_AGG_RESTRUCT:
		case MOVEMENT_LIM_RESTRUCT:
			Assert( FALSE ); /* Not a valid strategy for a join. */
			break;
	}

	return bytes_thru_ams;
}

/*
 * Generic list sorting routine for lists of pointers.
 * The cmpfn should be the same type of compare function that
 * is used in qsort.  In this case, it should expect
 * pointers to whatever type of pointer is in the List for its first and secong arguments.
 * (See cmpCdbJoinStrategy for an example when the List is a List of CdbJoinStrategy *'s.).
 * The passed in list is destroyed.  The caller should not use it again.
 * The new list is returned.  The new list is a list of the same pointers that were in the old
 * list, just rearranged in sorted order.
 *
 * The strategy is to create an array of pointers out of the original list's pointers,
 * sort the array using qsort with the indicated compare function, and then create a
 * new list out of the sorted pointers.
 */
List *sort_list(List *l, int (*cmpfn)(const void *, const void *))
{
	void **cellvalues = NULL;
	int i;
	ListCell *cell;

	Assert( l != NIL );
	Assert( IsA(l, List) );

	int n = list_length(l);
	Assert( n > 0 );

	/*
	 * Allocate an array of pointers to void to hold
	 * the pointers from the list.
	 */
	cellvalues = (void **)palloc0( n * sizeof(void *) );

	/*
	 * Grab the pointers out of the list, and put them into the array.
	 */
	i=0;
	foreach(cell, l)
	{
		cellvalues[i++] = lfirst(cell);
	}

	/*
	 * Free the memory for the passed in list, without freeing the pointers inside the list.
	 */
	list_free(l);
	l = NIL;

	/*
	 * Sort the pointers using the passed-in compare function.
	 */
	qsort(cellvalues, n, sizeof(void *), cmpfn);

	/*
	 * Create a list from the sorted pointers
	 */
	for ( i=0; i<n; i++ )
	{
		l = lappend( l, cellvalues[i] );		
	}

	/*
	 * Free the array of pointers.
	 */
	pfree(cellvalues);

	return l;
}

/*
 * cmpCdbJoinStrategy.
 * 
 * This is a compare function that accepts pointers to pointers
 * to CdbJoinStrategy structs, and compares them based on the 
 * est_ams_cost field.
 */
int cmpCdbJoinStrategy( const void *l, const void *r ) 
{
	CdbJoinStrategy *lhs;
	CdbJoinStrategy *rhs;

	Assert( l != NULL && r != NULL );

	lhs = *(CdbJoinStrategy **)l;
	rhs = *(CdbJoinStrategy **)r;

	Assert( lhs != NULL && rhs != NULL );

	double diff = lhs->est_ams_cost - rhs->est_ams_cost;

	if ( diff < 0.0 )
		return -1;
	else if ( diff == 0.0 )
		return 0;
	else 
		return 1;
}

/* 
 * A ExprVarProfile holds state for recursive expr_varlist_walker(). 
 */
typedef struct VarProfile
{
	List	*varnos;
} VarProfile;

static bool expr_varlist_walker(Node *node, VarProfile *context);
static int varwalk(Node *node);

/*
 * varwalk uses expr_varlist_walker to visit the nodes in an expression,
 * and create a list of all the varnos in any Var node therein.
 * We are interested in finding exactly one varno, so if there are
 * 0 or 2 or more, we return 0.  If one, we return that as the return value.
 */
int varwalk(Node *node)
{
	VarProfile	context;
	context.varnos			= NIL;

	int rtn = 0;

	if ( !expr_varlist_walker(node, &context) )
	{
		if ( context.varnos != NIL && list_length( context.varnos ) == 1 )
			rtn = lfirst_int(list_head(context.varnos));
	}

	if ( context.varnos != NIL )
		list_free(context.varnos);

	return rtn;
}

bool expr_varlist_walker(Node *node, VarProfile *context)
{
	if ( node == NULL )
		return false;
	
	switch (nodeTag(node))
	{
		case T_Var:
			{
				int varno = ((Var *)node)->varno;

				/* See whether varno is already in context->varnos */
				if ( !list_member_int( context->varnos, varno ) )
					context->varnos = lappend_int( context->varnos, varno );

				return false;
			}

		default:
			break;
	}

	return expression_tree_walker(node, expr_varlist_walker, context);
}


/*
 * extractHashExprFromQual
 * This is an attempt to take an arbitrary joinqual list, from a NestLoop,
 * and see whether each element of the list is an OpExpr, representing =,
 * and whose arguments satisfy the property that one references only Vars 
 * with varno of INNER and the other one references only Vars with varno of
 * OUTER.  Furthermore, we are currently requiring that the oid of the types of the left
 * and right arguments are the same, although this could be relaxed in the future.
 * When this is the case, we create two Lists: one a List of the OpExpr, and the other a List
 * of the oids of the types of the arguments to the OpExprs.  Furthermore, the List of OpExpr
 * is guarantted to always have OUTER as the varno of Vars in the first argument, and
 * INNER as the varno of the Vars in the second argument.
 * These Lists are returned in the out parameters pHashExpr, and pHashOp.
 * In this case the function returns true.
 * If these conditions are not satisfied, the function returns false.
 *
 * Examples: 
 * 1) r_regionkey=n_regionkey.  
 *		This results in a successful extraction.
 * 2) r_regionkey + n_regionkey = 2.  
 *		This does not result in a successful extraction, 
 *		because the lhs refers to 2 different tables,
 *      although it could be made successful by rewriting it as r_regionkey = 2 - n_regionkey.
 * 3) r_regionkey < n_regionkey. 
 *		This does not result in a successful extraction, 
 *		because it uses < instead of =.
 * 4) length( strX ) = length( strY ).  
 *		This results in a successful extraction.
 * 5) case when r_regionkey < 5 then r_regionkey + 7 else r_regionkey - 7 end = 
 *		case when n_regionkey < 5 then n_regionkey * n_regionkey else 1 end 
 *		This results in a successful extraction.
 * 6) r_regionkey = n_regionkey or n_regionkey=1 
 *		This does not result in a successful extraction, because the expression type is a BoolExpr
 * 7) l_orderkey = o_orderkey and o_custkey = l_partkey
 *		This does result in a successful extraction, but one of the the hashExpr OpExpr will
 * be reversed, so that the l_fields and the n_fields are on the same side in both expressions.
 */		
bool extractHashExprFromQual( List *joinqual, List **pHashExpr, List **pHashOp )
{
	ListCell	*c;
	int			i=0;
	List		*hashExpr = NIL;
	List		*hashOp = NIL;
	Expr		*e;
	Oid			opno;
	Oid			lefttype;
	Oid			righttype;
	Oid			temptype;
	Oid			eqoid;
	bool		rtn = false;

	Assert( pHashExpr != NULL );
	Assert( pHashOp != NULL );

	if ( joinqual == NIL )
	{
		elog( DEBUG1, "Ooops, joinqual == NIL" );
		goto cleanup;
	}

	/*
	 * Iterate through the list of Expressions in the 
	 * joinqual.
	 */
	foreach( c, joinqual )
	{
		i++;

		e = (Expr *)lfirst(c);

		/*
		* If it's not an OpExpr, we lose.
		*/
		if ( nodeTag(e) != T_OpExpr )
		{
			elog( DEBUG1, "Ooops, nodeTag was %d in test for separable nestloop joinqual expressions, loop %d", 
				nodeTag(e),
				i );
			goto cleanup;
		}

		/* The opno of the OpExpr must respresent the equality operator for the data types 
		 * in the argument list, or we lose.
		 */
		OpExpr *oexpr = (OpExpr *)e;
		if ( list_length(oexpr->args) != 2 )
		{
			elog( DEBUG1, "Ooops, list_length(oexpr->args) was %d in test for separable nestloop joinqual expressions, loop %d", 
						list_length(oexpr->args),
						i );
			goto cleanup;
		}

		opno = oexpr->opno;
		lefttype = InvalidOid;
		righttype = InvalidOid;
		eqoid = InvalidOid;

		op_input_types(opno, &lefttype, &righttype);

		/*
		 * TODO: We should be able to remove this restriction.  There is really no need to insist that they
		 * be the same.  However, currently we only have one List for the optypes.
		 */
		if ( lefttype != righttype )
		{
			elog( DEBUG1, "Ooops, op input types ( %d and %d) don't match for separable nestloop joinqual expressions, loop %d", 
						lefttype, righttype,
						i );
			goto cleanup;
		}

		eqoid = compatible_oper_opid(list_make1(makeString("=")), lefttype, righttype, true);

		if ( opno != eqoid )
		{
			elog( DEBUG1, "Ooops, opno != eqoid (%d != %d) in test for separable nestloop joinqual expressions, loop %d", 
						opno, eqoid,
						i );
			goto cleanup;
		}

		/* 
		 * Each argument must reference exactly one varno, or we lose.
		 * Furthermore, one argument must reference INNER (65000) and the other one OUTER (65001)
		 * or we lose.  
		 */
		int varno1, varno2;
		varno1 = varwalk(linitial(oexpr->args));
		varno2 = varwalk(lsecond(oexpr->args));
		if ( varno1 == 0 || varno2 == 0 )
		{
			elog( DEBUG1, "Ooops, varno1 (%d) or varno2 (%d) is 0 in test for separable nestloop joinqual expressions, loop %d", 
						varno1, varno2,
						i );
			goto cleanup;
		}

		if ( varno1 == varno2 )
		{
			elog( DEBUG1, "Ooops, varno1 (%d) == varno2 (%d) in test for separable nestloop joinqual expressions, loop %d", 
						varno1, varno2,
						i );
			goto cleanup;
		}

		/*
		* This is a little tricky.  We want to make sure that
		* 1) the new varnos are OUTER, INNER 
		* 2) they occur in the order OUTER, INNER.  If not, we need to create
		*		a new opexpression, with the arguments switched, and an eq operator appropriate
		*		to the switched arguments.
		* We do this so the resulting hashExpr List is in a canonical form for later use.
		*/
		if (	(varno1 != OUTER && varno2 != OUTER) 
			||	(varno1 != INNER && varno2 != INNER) )
		{
			elog( DEBUG1, "Ooops, missing either 65001 and 65000 in varnos (%d,%d) in test for separable nestloop joinqual expressions, loop %d", 
						varno1, varno2,
						i );
			goto cleanup;
		}
		
		if ( varno1 == OUTER )
		{
			/* Great news.  Order is correct */
			hashExpr = lappend( hashExpr, oexpr );
		}
		else
		{
			/*
				* We need to create a new opexr, switching the args, and
				* getting the correct opno for the switched args
				*/ 
			OpExpr *temp = makeNode(OpExpr);
			temp->opno = oexpr->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = oexpr->opresulttype;
			temp->opretset = oexpr->opretset;
			temp->args = list_copy(oexpr->args);
			CommuteClause(temp);
			hashExpr = lappend( hashExpr, temp );
			temptype = lefttype;
			lefttype = righttype;
			righttype = temptype;
		}

		hashOp = lappend_oid( hashOp, lefttype );
	}

	rtn = true;

cleanup:
	if ( !rtn )
	{
		list_free( hashExpr );
		list_free( hashOp );
	}
	else
	{
		*pHashExpr = hashExpr;
		*pHashOp = hashOp;
	}

	return rtn;

}
