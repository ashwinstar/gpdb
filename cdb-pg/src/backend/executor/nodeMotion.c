/*-------------------------------------------------------------------------
 *
 * nodeMotion.c
 *	  Routines to handle moving tuples around in cdb.
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeMotion.h"
#include "optimizer/clauses.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h" /* for ReleaseSysCache */
#include "miscadmin.h"

/* 
 * Helper functions 
 */ 
static int *makeDefaultSegIdxArray(int numSegs);
static void validateMotionNodeParms(MotionType motionType,
			bool sendToSecondary, bool sendSorted, 
			HashMotionDefn *pHashDefn, FixedMotionDefn* pFixedDefn,
			SortedMotionDefn *pSortDefn);
static List *getExprListFromCondList(List *clist, HashMotionInfoType type);
static List *getOpListFromCondList(List *clist);
static List *getExprListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx);
static List *getOpListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx);
static bool motion_expr_walker(Node *node, char *not_used);
static uint32 evalHashKey(ExprContext *econtext, List *hashkeys, FmgrInfo *hashfunctions);


/* ----------------------------------------------------------------
 *		ExecMotion
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecMotion(MotionState *node)
{
	TupleTableSlot *slot;
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerNode;
	HeapTuple tuple;
	ExprContext *econtext = node->ps.ps_ExprContext;
	Motion *motion = (Motion *)node->ps.plan;

	SO1_printf("ExecMotion: %s\n",
			   "entering routine");

	/*
	 * grab TupleTableSlot from our child.
	 */
	outerNode = outerPlanState(node);
	
	outerTupleSlot = ExecProcNode(outerNode);
	if ( TupIsNull(outerTupleSlot) )
		return NULL;
		
	/*
	 * make a copy of the tuple
	 */
	tuple = heap_copytuple(outerTupleSlot->val);
	
	uint32 hval = 0;
	if ( motion->motionType == MOTIONTYPE_HASH )
	{
		econtext->ecxt_outertuple = outerTupleSlot;
		hval = evalHashKey(econtext, node->hashExprState, node->hashfunctions);
	}

	slot = node->ps.ps_ResultTupleSlot;
	
	/*
	 * store it in our result slot and return this.
	 */
	return ExecStoreTuple(tuple, slot, InvalidBuffer, true);
}

/* ----------------------------------------------------------------
 *		ExecInitMotion
 * ----------------------------------------------------------------
 */
MotionState *
ExecInitMotion(Motion *node, EState *estate)
{
	MotionState* motionstate;
	
	SO_printf("ExecInitMotion: initializing motion node\n");
	
	/*
	 * create state structure
	 */
	motionstate = makeNode(MotionState);
	motionstate->ps.plan = (Plan *) node;
	motionstate->ps.state = estate;
	motionstate->noMoreChildTuples = false;
	motionstate->noMoreAMSTuples = false;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &motionstate->ps);

#define MOTION_NSLOTS 1

	/*
	 * tuple table initialization
	 *
	 */
	ExecInitResultTupleSlot(estate, &motionstate->ps);

	/*
	 * initializes child nodes
	 */
	outerPlanState(motionstate) = ExecInitNode(outerPlan(node), estate);

	/*
	 * initialize tuple type.  no need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecAssignResultTypeFromOuterPlan(&motionstate->ps);
	motionstate->ps.ps_ProjInfo = NULL;
	motionstate->hashfunctions = NULL;
	motionstate->hashExprState = NULL;
		
	if ( node->motionType == MOTIONTYPE_HASH )
	{
		int			nkeys;
		int			i;
		ListCell	*ho;
		
		/*
		 * Get info about the hash functions to be used for each hash key.
		 */
		HashMotionDefn *pHashDefn = node->pHashDefn;
		Assert( pHashDefn != NULL && pHashDefn->hashOp != NULL );
		 
		nkeys = list_length(pHashDefn->hashOp);
		motionstate->hashfunctions = (FmgrInfo *) palloc(nkeys * sizeof(FmgrInfo));
		i = 0;
		foreach(ho, pHashDefn->hashOp)
		{
			Oid			hashfn;
	
			hashfn = get_op_hash_function(lfirst_oid(ho));
			if (!OidIsValid(hashfn))
				elog(ERROR, "could not find hash function for hash operator %u",
					 lfirst_oid(ho));
			fmgr_info(hashfn, &motionstate->hashfunctions[i]);
			i++;
		}
		
		motionstate->hashExprState = (List *)ExecInitExpr( (Expr *)pHashDefn->hashExpr,
														 (PlanState *) motionstate);
	}
		
	SO1_printf("ExecInitMotion: %s\n",
			   "motion node initialized");

	return motionstate;
}

/* ----------------------------------------------------------------
 *		ExecCountSlotsMotion
 * ----------------------------------------------------------------
 */
int
ExecCountSlotsMotion(Motion *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) +
		MOTION_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndMotion(node)
 * ----------------------------------------------------------------
 */
void
ExecEndMotion(MotionState *node)
{
	SO1_printf("ExecEndMotion: %s\n",
			   "shutting down motion node");

	ExecFreeExprContext(&node->ps);
	
	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

	SO1_printf("ExecEndMotion: %s\n",
			   "motion node shutdown");
}

/* -----------------------------------------------------------------------
 * Helper functions for extracting Lists of join expressions or operators
 * from nodes, where the logic varies by node type
 * -----------------------------------------------------------------------
 */
List *getHashMotionInfoList( Plan *node, HashMotionInfoType type )
{
	List *hashinfo = NIL;
	
	switch ( nodeTag(node) )
	{
		case T_HashJoin:
			/*
			 * Deconstruct the hash clauses into outer and inner argument values,
			 * so that we can evaluate those subexpressions separately.  
			 */
			{
				HashJoin *hjnode = (HashJoin *)node;
				List *hclist = hjnode->hashclauses;
				if ( hclist == NIL )
					elog( ERROR, "Empty hashclauses list encountered in getHashMotionInfoList for HashJoin node");
					
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromCondList(hclist, type);
				else	
					hashinfo = getOpListFromCondList(hclist);
			}
				
			break;
			
		case T_Hash:
			elog( ERROR, "Musn't use a Hash Node in getHashMotionInfoList.  Use the parent HashJoin node instead." );
			break;
			
		case T_MergeJoin:
			/*
			 * Deconstruct the merge clauses into outer and inner argument values,
			 * so that we can evaluate those subexpressions separately.
			 */
			{
				MergeJoin *mjnode = (MergeJoin *)node;
				List *mclist = mjnode->mergeclauses;
				if ( mclist == NIL )
					elog( ERROR, "Empty mergeclauses list encountered in getHashMotionInfoList for MergeJoin node");
				
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromCondList(mclist, type);
				else	
					hashinfo = getOpListFromCondList(mclist);
			}
				
			break;
			
		case T_NestLoop:
			{
				NestLoop *nlnode = (NestLoop *)node;
				List *jqlist = nlnode->join.joinqual;
				if ( jqlist == NIL )
					elog( ERROR, 
							"Not yet implemented: calling getHashMotionInfoList for a NestLoop node with no joinqual list.  "
							"This can happen with an IndexScan node as the inner child." );
				
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromCondList(jqlist, type);
				else	
					hashinfo = getOpListFromCondList(jqlist);
			}
				
			break;
			
		case T_Sort:
			{
				Sort		*snode = (Sort *)node;
				int			numCols = snode->numCols;
				AttrNumber *sortColIdx = snode->sortColIdx;
				
				if ( numCols == 0 || sortColIdx == NULL )
					elog( ERROR, 
							"Empty sortColIdx array encountered in getHashExprList for Sort node" );
							
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromTargetList(snode->plan.targetlist, numCols, sortColIdx);
				else
					hashinfo = getOpListFromTargetList(snode->plan.targetlist, numCols, sortColIdx);
					
			}
				
			break;
			
		case T_Agg:
			{
				Agg			*aggnode = (Agg *)node;
				if ( aggnode->aggstrategy == AGG_PLAIN )
					elog( ERROR, "there is no hash expression list for Plain Aggregate nodes");	
				
				int			numCols = aggnode->numCols;
				AttrNumber *grpColIdx = aggnode->grpColIdx;

				if ( numCols == 0 || grpColIdx == NULL )				
					elog( ERROR, 
						"Empty grpColIdx array encountered in getHashExprList for Agg node" );
						
				if ( type == INFOTYPE_LEFTEXPR || type == INFOTYPE_RIGHTEXPR )
					hashinfo = getExprListFromTargetList(aggnode->plan.lefttree->targetlist, numCols, grpColIdx);
				else
					hashinfo = getOpListFromTargetList(aggnode->plan.lefttree->targetlist, numCols, grpColIdx);
			}
				
			break;
			
		default:
			elog( WARNING, "getHashMotionInfoList called with an unsupported node type %d", nodeTag(node) );
			break;
	}	
	
	return hashinfo;
} 

/* --------------------------------------------------------------------
 * makeHashMotionDefn -- creates a HashMotionDefn*
 * Caller must have built the hashExpr and hashOp lists and 
 * hashSegIdx array already.
 * --------------------------------------------------------------------
 */
HashMotionDefn* makeHashMotionDefn( List* hashExpr, List* hashOp, int numHashSegs, int *hashSegIdx)
{
	HashMotionDefn *pHashDefn = palloc0(sizeof(HashMotionDefn));
	
	if ( numHashSegs < 0 )
		numHashSegs = 0;
		
	pHashDefn->numHashSegs = numHashSegs;
	
	if ( numHashSegs > 0 )
		pHashDefn->hashSegIdx = hashSegIdx;
	
	pHashDefn->hashExpr = hashExpr;
	pHashDefn->hashOp	= hashOp;
	
	return pHashDefn;
}

/* ----------------------------------------------------------------
 * makeSimpleHashMotionDefn -- 
 * Convenience routine to create a HashMotionDefn that
 * uses all of the segindexes for the hashSegIdx array.
 * Caller must have built the hashExpr and hashOp lists already.
 * ----------------------------------------------------------------
 */
HashMotionDefn *makeSimpleHashMotionDefn( List* hashExpr, List* hashOp, int numSegs)
{
	int *hashSegIdx = NULL;
	
	if ( numSegs > 0 )
	{
		hashSegIdx = makeDefaultSegIdxArray(numSegs);
	}
	
	return makeHashMotionDefn( hashExpr, hashOp, numSegs, hashSegIdx);
}

/* --------------------------------------------------------------------
 * makeFixedMotionDefn -- creates a FixedMotionDefn*
 * Caller must have built the outputSegIdx array already.
 * --------------------------------------------------------------------
 */
FixedMotionDefn* makeFixedMotionDefn(int numOutputSegs, int *outputSegIdx)
{
	FixedMotionDefn *pFixedDefn = palloc0(sizeof(FixedMotionDefn));
	
	if ( numOutputSegs < 0 )
		numOutputSegs = 0;
		
	pFixedDefn->numOutputSegs = numOutputSegs;
	
	if ( numOutputSegs > 0 )
		pFixedDefn->outputSegIdx = outputSegIdx;
	
	return pFixedDefn;
}

/* --------------------------------------------------------------------
 * makeSimpleFixedMotionDefn --
 * Convenience routine to create a FixedMotionDefn that
 * has exactly one output segindex, e.g. for a union motion node
 * --------------------------------------------------------------------
 */
FixedMotionDefn* makeSimpleFixedMotionDefn( int SegIdx )
{
	int *outputSegIdx = palloc( sizeof(int) );
	*outputSegIdx = SegIdx;

	return makeFixedMotionDefn(1, outputSegIdx);
}

/* --------------------------------------------------------------------
 * makeSortedMotionDefn -- creates a SortedMotionDefn*
 * Caller must have built the sortColIdx and sortOperators 
 * and inputSegIdx arrays already.
 * --------------------------------------------------------------------
 */
SortedMotionDefn* makeSortedMotionDefn(int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, int numInputSegs, int *inputSegIdx)
{
	SortedMotionDefn *pSortDefn = palloc0(sizeof(SortedMotionDefn));
	
	if ( numSortCols < 0 )
		numSortCols = 0;
		
	pSortDefn->numSortCols = numSortCols;
	
	if ( numSortCols > 0 )
	{
		pSortDefn->sortColIdx = sortColIdx;
		pSortDefn->sortOperators = sortOperators;
	}
	
	if ( numInputSegs < 0 )
		numInputSegs = 0;
		
	pSortDefn->numInputSegs = numInputSegs;
	
	if ( numInputSegs > 0 )
		pSortDefn->inputSegIdx = inputSegIdx;
	
	return pSortDefn;
}

/* --------------------------------------------------------------------
 * makeSimpleSortedMotionDefn -- 
 * Convenience routine to create a SortedMotionDefn that
 * uses all of the segindexes for the inputSegIdx array.
 * Caller must have built the sortColIdx and sortOperators arrays already.
 * --------------------------------------------------------------------
 */
SortedMotionDefn* makeSimpleSortedMotionDefn(int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators, int numSegs )
{
	int *inputSegIdx = NULL;
	
	if ( numSegs > 0 )
	{
		inputSegIdx = makeDefaultSegIdxArray(numSegs);
	}
	
	return makeSortedMotionDefn( numSortCols, sortColIdx, sortOperators, numSegs, inputSegIdx);
}

/* --------------------------------------------------------------------
 * make_motion -- creates a Motion node.
 * Caller must have built the pHashDefn, pFixedDefn,
 * and pSortDefn structs already.
 * --------------------------------------------------------------------
 */
Motion *make_motion(Plan *lefttree, MotionType motionType,
			bool sendToSecondary, bool sendSorted, 
			HashMotionDefn *pHashDefn, FixedMotionDefn* pFixedDefn,
			SortedMotionDefn *pSortDefn )
{
	validateMotionNodeParms(motionType,	sendToSecondary, sendSorted, 
			pHashDefn, pFixedDefn,	pSortDefn);
			
	Motion	   *node = makeNode(Motion);
	Plan	   *plan = &node->plan;

	plan->startup_cost = lefttree->startup_cost;
	plan->total_cost = lefttree->total_cost;
	plan->plan_rows = lefttree->plan_rows;
	plan->plan_width = lefttree->plan_width;
	plan->targetlist = copyObject(lefttree->targetlist);
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->motionType = motionType;
	node->sendToSecondary = sendToSecondary;
	node->sendSorted = sendSorted;
	node->pHashDefn = pHashDefn;
	node->pFixedDefn = pFixedDefn;
	node->pSortDefn = pSortDefn;
	node->motionID = 0;
	
	return node;
}

Motion *make_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex )
{
	return 	make_motion(lefttree, MOTIONTYPE_FIXED, 
						sendToSecondary, false, 
						NULL, makeSimpleFixedMotionDefn( destSegIndex ), NULL );
}

Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex, SortedMotionDefn *pSortDefn  )
{
	return 	make_motion(lefttree, MOTIONTYPE_FIXED, 
						sendToSecondary, true, 
						NULL, makeSimpleFixedMotionDefn( destSegIndex ), pSortDefn );
}

Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary, 
							HashMotionDefn *pHashDefn)
{
	return make_motion(lefttree, MOTIONTYPE_HASH,
						sendToSecondary, false, 
						pHashDefn, NULL, NULL );
}

Motion *make_sorted_hashed_motion(Plan *lefttree, bool sendToSecondary, 
								HashMotionDefn *pHashDefn, SortedMotionDefn *pSortDefn)
{
	return make_motion(lefttree, MOTIONTYPE_HASH,
						sendToSecondary, true, 
						pHashDefn, NULL, pSortDefn);
}

Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary)
{
	return make_motion(lefttree, MOTIONTYPE_BROADCAST,
						sendToSecondary, false, 
						NULL, NULL, NULL );
}

Motion *make_sorted_broadcast_motion(Plan *lefttree, bool sendToSecondary, SortedMotionDefn *pSortDefn )
{
	return make_motion(lefttree, MOTIONTYPE_BROADCAST,
						sendToSecondary, true, 
						NULL, NULL, pSortDefn );
}

/* --------------------------------------------------------------------
 * 
 *  Static Helper routines
 * --------------------------------------------------------------------
 */
 
/* --------------------------------------------------------------------
 * makeDefaultSegIdxArray -- creates an int array with numSegs elements,
 * with values between 0 and numSegs-1
 * --------------------------------------------------------------------
 */
static int *makeDefaultSegIdxArray(int numSegs)
{
	int *segIdx = palloc(numSegs * sizeof(int));

	/*
	 * The following assumes that the default is to have segindexes
	 * numbered sequentially starting at 0.
	 */
	int i;
	for ( i=0; i<numSegs; i++ )
		segIdx[i] = i;
		
	return segIdx;	
}

/* --------------------------------------------------------------------
 * validateMotionNodeParms -- validates the parameters passed to
 * make_motion
 * --------------------------------------------------------------------
 */
void validateMotionNodeParms(MotionType motionType,
			bool sendToSecondary, bool sendSorted, 
			HashMotionDefn *pHashDefn, FixedMotionDefn* pFixedDefn,
			SortedMotionDefn *pSortDefn)
{
	/*
	 *  Perform validation based on motionType.
	 */
	switch( motionType )
	{
		case MOTIONTYPE_HASH: 
	
			if ( pHashDefn == NULL )
				elog(ERROR, "motion type 'hash' node must have non-NULL pHashDefn");
		/*
		 *  A Hash motion node must have hash expr info specified.
		 */
			if ( pHashDefn->hashExpr == NULL )
				elog(ERROR, "motion type 'hash' node must have non-NULL hashExpr");
				
		/*
		 *  A Hash motion node must have hash operator info specified.
		 */
			if ( pHashDefn->hashOp == NULL )
				elog(ERROR, "motion type 'hash' node must have non-NULL hashOp");
				
		/*
		 *  A Hash motion node must have hash segment info specified.
		 */
			if ( pHashDefn->numHashSegs <= 0 || pHashDefn->hashSegIdx == NULL )
				elog(ERROR, "motion type 'hash' node must have numHashSegs > 0 and non-NULL hashSegIdx");

		/*
		 *  A Hash motion node shouldn't have FixedMotionDefn specified.
		 */
			Assert(pFixedDefn == NULL);
			break;	
	
		case MOTIONTYPE_BROADCAST: 
		/*
		 *  A Broadcast motion node shouldn't have HashMotionDefn specified.
		 */
			Assert( pHashDefn == NULL);

		/*
		 *  A Broadcast motion node shouldn't have FixedMotionDefn specified.
		 */
			Assert(pFixedDefn == NULL);
			break;	

		case MOTIONTYPE_FIXED: 

			if ( pFixedDefn == NULL )
				elog(ERROR, "motion type 'fixed' node must have non-NULL pFixedDefn");
		/*
		 *  An Fixed motion node must have output Segs info specified.
		 */
			if ( pFixedDefn->numOutputSegs <= 0 || pFixedDefn->outputSegIdx == NULL  )
				elog(ERROR, "motion type 'fixed' node must have numOutputSegs > 0 and non-NULL outputSegIdx");

		/*
		 *  An Fixed motion node shouldn't have HashMotionDefn specified.
		 */
			Assert( pHashDefn == NULL);
				
			break;	
			
		default:
		/*
		 *  Invalid motion node type.
		 */
			Assert( true );
			break;
	}
	
	/*
	 *  Perform validation based on sendSorted flag
	 */
	if ( sendSorted )
	{
		if ( pSortDefn == NULL )
			elog(ERROR, "sorted motion node must have non-NULL pSortDefn");
		
		/*
		 *  A sorted motion node must have sort Cols specified
		 */
		if (pSortDefn->numSortCols == 0 || pSortDefn->sortColIdx == NULL 
				|| pSortDefn->sortOperators == NULL)
			elog(ERROR, "sorted motion node must have numSortCols > 0 and non-NULL sortColIdx and sortOperators");

		/*
		 *  A sorted motion node must have input Segs info specified
		 */
		if (pSortDefn->numInputSegs == 0 || pSortDefn->inputSegIdx == NULL)
			elog(ERROR, "sorted motion node must have numInputSegs > 0 and non-NULL inputSegIdx");
	}
	else
	{
		/*
		 *  A non-sorted motion node should not have SortMotionDefn specified
		 */
		Assert( pSortDefn == NULL);
	}
}			

/* --------------------------------------------------------------------
 * motion_expr_walker -- replaces occurrences of INNER with OUTER
 * for Var's varno in hashexpr for motion nodes.
 * This came about because the join nodes args for the right subtree were
 * changed by setrefs to be INNER.  We grabbed them after that change.
 * But the motion node has no inner subtree, just an outer.
 * --------------------------------------------------------------------
 */
bool motion_expr_walker(Node *node, char *not_used)
{
	if (node == NULL)
		return false;
	
	// check for nodes that special work is required for, eg:
	if (IsA(node, Var))
	{
		Var* var = (Var *)node;
		switch( var->varno )
		{
			case INNER:
				var->varno = OUTER;
				break;

			default:
				var->varno = OUTER;
				break;
		}
		
		return false;
	}
	else
	{
		return expression_tree_walker(node, motion_expr_walker, (void *) not_used);
	}
	
}

/* ----------------------------------------------------------------
 * getExprListFromCondList
 *
 * Extracts either the left or right arguments or operators from 
 * the given join condition list.
 * 
 * The entries returned are copies of the expressions from the
 * condition list in which any Var references to INNER have been
 * replaced by references to OUTER.
 * ----------------------------------------------------------------
 */
List *getExprListFromCondList(List *clist, HashMotionInfoType type)
{
	List *elist = NIL;
	ListCell *l;
	Node *expr;
	Node *node;
		
	foreach(l, clist)
	{
		OpExpr	*opexpr = (OpExpr *) lfirst(l);

		Assert(IsA(opexpr, OpExpr));
		Assert(list_length(opexpr->args) == 2);
		
		if (type == INFOTYPE_LEFTEXPR)
			node = linitial(opexpr->args);
		else if (type == INFOTYPE_RIGHTEXPR)	
			node = lsecond(opexpr->args);
		else
			Assert(0);	
			
		/* Assert(IsA(node, Expr)); -- Expr is abstract, IsA is not smart enought to deal with this. */
		expr = copyObject(node);
		
		motion_expr_walker(expr, NULL);
		elist = lappend(elist, expr);
	}
	
	return elist;
}	

/* ----------------------------------------------------------------
 * getOpListFromCondList - -
 * this extracts the list of hash operator OIDs
 * appropriate to the datatype of condition in the clist
 * ----------------------------------------------------------------
 */
List *getOpListFromCondList(List *clist)
{
	List		*oplist = NIL;
	ListCell	*l;
	
	foreach(l, clist)
	{
		OpExpr	*opexpr = (OpExpr *) lfirst(l);
		Assert(IsA(opexpr, OpExpr));
		oplist = lappend_oid(oplist, opexpr->opno);
	}
	
	return oplist;
}

/* ----------------------------------------------------------------
 * getExprListFromTargetList
 *
 * Extracts the indicated entries from the target list, copies each,
 * and puts them into a list.
 *
 * The AttrNumber indexes actually refer to the resno value in the 
 * Resdom entries in the target list.  These are often just the
 * ordinal positions (origin 1) of the entries, but for non-query
 * DML it may not be.
 * 
 * The entries returned are copies of the expressions from the
 * target list in which any Var references to INNER have been
 * replaced by references in OUTER.
 * ----------------------------------------------------------------
 */
List *getExprListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx)
{
	int		i;
	List	*elist = NIL;
	Expr	*expr = NULL;
	
	for (i=0; i<numCols; i++ )
	{
		// Find expr in TargetList
		AttrNumber n = colIdx[i];

		TargetEntry* target = get_tle_by_resno( tlist, n );
		if ( target == NULL )
			elog( ERROR, "no tlist entry for key %d", n );
			
		expr = copyObject(target->expr);
		motion_expr_walker((Node*)expr, NULL);
		elist = lappend(elist, expr);
	}
	
	return elist;
}

/* ----------------------------------------------------------------
 * getOpListFromTargetList
 * 
 * Returns a list of oids or the equality operators for indicated
 * entries of the target list.
 *
 * The AttrNumber indexes actually refer to the resno value in the 
 * Resdom entries in the target list.  These are often just the
 * ordinal positions (origin 1) of the entries, but for non-query
 * DML it may not be.
 * 
 * ----------------------------------------------------------------
 */
List *getOpListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx)
{
	int			i;
	List		*oplist = NIL;
	Operator	eq_operator;
	Oid			typeoid;
	
	for (i=0; i<numCols; i++ )
	{
		// Find expr in TargetList
		AttrNumber n = colIdx[i];

		TargetEntry* target = get_tle_by_resno( tlist, n );
		if ( target == NULL )
			elog( ERROR, "no tlist entry for key %d", n );
			
		typeoid = target->resdom->restype;
		eq_operator = compatible_oper(list_make1(makeString("=")), typeoid, typeoid, true);
		oplist = lappend_oid(oplist, oprid(eq_operator));
		ReleaseSysCache(eq_operator);
	}
	
	return oplist;
}

/*
 * Experimental code that will be replaced later with new hashing mechanism
 */
uint32 evalHashKey(ExprContext *econtext, List *hashkeys, FmgrInfo *hashfunctions)
{
	uint32		hashkey = 0;
	ListCell   *hk;
	int			i = 0;
	MemoryContext oldContext;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	foreach(hk, hashkeys)
	{
		ExprState  *keyexpr = (ExprState *) lfirst(hk);
		Datum		keyval;
		bool		isNull;

		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		/*
		 * Get the attribute value of the tuple
		 */
		keyval = ExecEvalExpr(keyexpr, econtext, &isNull, NULL);

		/*
		 * Compute the hash function
		 */
		if (!isNull)			/* treat nulls as having hash key 0 */
		{
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1(&hashfunctions[i],keyval));
			hashkey ^= hkey;
		}

		i++;
	}
	
	MemoryContextSwitchTo(oldContext);
	
	return hashkey;
}
