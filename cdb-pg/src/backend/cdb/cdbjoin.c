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
#include "cdb/cdbjoin.h"

typedef enum {
		RxR = 0,	RxL,	RxP,
		LxR,		LxL,	LxP,
		PxR,		PxL,	PxP
} JoinCombination;
	
typedef enum {
		InnerJoin,
		OuterLeft,
		OuterRight,
		OuterBoth
} JoinDirection;

static CdbJoinStrategy *makeStrategy(Movement left, Movement right, FlowType result);


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
		case JOIN_IN:
		case JOIN_REVERSE_IN:
		case JOIN_UNIQUE_INNER:
			argreqs = InnerJoin;
			break;
		
		case JOIN_LEFT: 
			argreqs = OuterLeft;
			break;
		
		case JOIN_FULL: 
		case JOIN_UNIQUE_OUTER:
			argreqs = OuterBoth;
			break;
		
		case JOIN_RIGHT:
			argreqs = OuterRight;
			break;

		case JOIN_UNION:
		default:
			/* This shouldn't happen. */
			return NIL;
			break;
	}
	
	switch ( jcomb )
	{
		case RxR:
			/* Replicated flows can join in parallel.   The result is a
			 * replicated flow.  This case may never occur. */
			result = lappend(result,makeStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_REPLICATED));
			break;
		
		case RxL:
		case LxR:
			/* Replicated and singleton where the singleton is on a segment
			 * can join in parallel, producing a singleton.  Currently,
			 * though, the only singleton flows are on the entry database,
			 * so the direct strategy is closed for now.  */
			 break;
			 
		case LxL:
			result = lappend(result,makeStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_SINGLETON));
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
					result = lappend(result,makeStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_PARTITIONED));
					/* Could also broadcast right side, but why? */
					break;
				
				case OuterLeft:
				case OuterBoth:
					/* Replicated flow is on the driving side.  We don't have
					 * a motion that can convert a replicated flow to a 
					 * partitioned one, so the only thing we can do is to
					 * broadcast the partitioned side making it replicated.
					 */
					result = lappend(result,makeStrategy(MOVEMENT_NONE, MOVEMENT_BROADCAST, FLOW_REPLICATED));
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
				case OuterLeft:
					/* Replicated flow isn't on the driving side. */
					result = lappend(result,makeStrategy(MOVEMENT_NONE, MOVEMENT_NONE, FLOW_PARTITIONED));
					/* Could also broadcast left side, but why? */
					break;
				
				case OuterRight:
				case OuterBoth:
					/* Replicated flow is on the driving side.  We don't have
					 * a motion that can convert a replicated flow to a 
					 * partitioned one, so the only thing we can do is to
					 * broadcast the partitioned side making it replicated.
					 */
					result = lappend(result,makeStrategy(MOVEMENT_BROADCAST, MOVEMENT_NONE, FLOW_REPLICATED));
					break;
			}
			break;
			
		/* Since tuples of a local flow may not be meaningful outside the
		 * current environment, to join a local flow to partitioned flow,
		 * we need to bring all the rows of the partitioned flow to the
		 * stream that carries the local flow. */		
		case LxP:
			result = lappend(result,makeStrategy(MOVEMENT_NONE, MOVEMENT_FOCUS, FLOW_SINGLETON));
			break;		
		case PxL:
			/* Motion: Focus left on dispatcher. */
			/* Result: Singleton, Local */
			result = lappend(result,makeStrategy(MOVEMENT_FOCUS, MOVEMENT_NONE, FLOW_SINGLETON));
			break;
			
		case PxP:
			/* Join two partitioned flows by repartitioning  them both on the
			 * join key. (It would be great if we were smart enought to notice
			 * whether one or both flow were already partitioned that way, but
			 * we're not, so both sides get repartitioned. */
			result = lappend(result,makeStrategy(MOVEMENT_REPARTITION, MOVEMENT_REPARTITION, FLOW_PARTITIONED));
			if ( argreqs!=OuterRight && argreqs!=OuterBoth)
				result = lappend(result,makeStrategy(MOVEMENT_NONE, MOVEMENT_BROADCAST, FLOW_PARTITIONED));
			if ( argreqs!=OuterLeft && argreqs!=OuterBoth)
				result = lappend(result,makeStrategy(MOVEMENT_BROADCAST, MOVEMENT_NONE, FLOW_PARTITIONED));
			break;
	}
	return result;
}


/*
 * Helper for getJoinStrategy -- palloc and initialize a strategy.
 */
CdbJoinStrategy *makeStrategy(Movement left, Movement right, FlowType result)
{
	CdbJoinStrategy *js = palloc(sizeof(CdbJoinStrategy));
	js->leftmove = left;
	js->rightmove = right;
	js->resultflowtype = result;
	js->resultislocal = ( result == FLOW_SINGLETON );
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
		js = makeStrategy(old->leftmove, old->rightmove, old->resultflowtype);
		js->resultislocal = old->resultislocal;
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
