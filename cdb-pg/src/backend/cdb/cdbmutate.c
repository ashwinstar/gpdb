/*-------------------------------------------------------------------------
 *
 * cdbmutate.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2005, greenplum inc
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
#include "nodes/makefuncs.h" /* for makeVar() */
#include "parser/parse_expr.h" /* for expr_type() */
#include "parser/parse_oper.h" /* for compatible_oper_opid() */
#include "nodes/makefuncs.h" /* for makeVar() */
#include "nodes/value.h" /* for makeString() */
#include "utils/syscache.h" /* for ReleaseSysCache */

#include "cdb/cdbvars.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbagg.h"
#include "cdb/cdbmutate.h"


/*
 * Forward Declarations
 */
static Node* apply_motion_mutator(Node*  node, ApplyMotionState *context);

/* TODO Check suitability of the approach taken in these functions. */
//static void set_hash_in_flow(Plan *plan, int nattrs, int *attrs);
//static bool force_vars_walker(Node *node, void *varno);

static void validateMotionNodeParms(MotionType motionType,
			bool sendToSecondary, bool sendSorted,
			List *hashExpr,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators );
static bool motion_expr_walker(Node *node, char *not_used);
#ifdef UNUSED
static List * makeMotionTargetList(Plan *arg);
#endif /*UNUSED*/
static bool doesUpdateAffectPartitionCols( CdbPolicy *policy, List *rte );


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
	Plan *result;
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
			 *
			 * TODO If the top node is a Material, we should reach below
			 *      it for our focusing.  Doing so would conserve
			 *      ExecSupportsBackwardScan-ness.
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
				
				switch ( policy->ptype )
				{
					case POLICYTYPE_PARTITIONED:
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
						}
						else if ( plan->flow->flotype == FLOW_REPLICATED )
						{
							/*
							* Source is replicated over segments. Apply "deep-six".
							*/
							plan->flow->req_move = MOVEMENT_REP2PARTITIONED;
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

						plan->flow->hashExpr = getExprListFromTargetList(plan->targetlist, policy->nattrs, policy->attrs);
						break;

					case POLICYTYPE_ENTRY:
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
							plan->flow->req_move = MOVEMENT_FOCUS;
							plan->flow->segindex = -1;
						}
						else if ( plan->flow->flotype == FLOW_REPLICATED )
						{
							/*
							* Source is replicated over segments.
							* We cannot currently handle insert into a local table
							* from a replicated flow.
							*/
							ereport(ERROR, (
								errcode(ERRCODE_CDB_FEATURE_NOT_YET),
								errmsg("CDB cannot parallelize that INSERT yet")
							));
						}
						else
						{
							/*
							* Source is, presumably, a dispatcher singleton.
							*/
							plan->flow->req_move = MOVEMENT_NONE;
						}
						break;
					default:
						Assert( false );
						break;
				}
			}
			break;
			
		case CMD_UPDATE:
		case CMD_DELETE:
			/*
			 * For updates and deletes on partitioned tables, we restrict the query
			 * to only those with a partioned flow, and involving no other tables.
			 * In this manner , we avoid any motion nodes.  If there are subqueries,
			 * the query will get rejected by the parallelizer unless they are initPlan subqueries.
			 * If the table is an entry database table, we reject the query unless the
			 * flow is a singleton on the entry database.
			 */
			{
				RangeTblEntry *rte;
				CdbPolicy *policy;

				rte = rt_fetch(query->resultRelation, query->rtable);
				Assert( rte->rtekind == RTE_RELATION);
				policy = get_table_policy(rte->relid);
				
				switch ( policy->ptype )
				{
					case POLICYTYPE_PARTITIONED:
						/*
						 * Currently, we only support update or delete involving 1 table
						 */
						if ( list_length(query->rtable) > 1 )
						{
							ereport(ERROR, (
								errcode(ERRCODE_CDB_FEATURE_NOT_YET),
									errmsg("CDB cannot parallelize that %s yet",
										query->commandType==CMD_UPDATE ? "UPDATE" : "DELETE")
							));
						}

						/*
						 * Make sure we are not updating any of the partitioning columns
						 */
						if ( query->commandType == CMD_UPDATE &&
							doesUpdateAffectPartitionCols( policy, plan->targetlist ) )
						{
							ereport(ERROR, (
								errcode(ERRCODE_CDB_FEATURE_NOT_YET),
									errmsg("CDB cannot parallelize an update that involves the partitioning columns")
							));
						}

						if ( plan->flow->flotype == FLOW_PARTITIONED )
						{
							plan->flow->req_move = MOVEMENT_NONE;
						}
						else if ( plan->flow->flotype == FLOW_REPLICATED
							|| plan->flow->flotype == FLOW_SINGLETON )
						{
							ereport(ERROR, (
								errcode(ERRCODE_CDB_FEATURE_NOT_YET),
									errmsg("CDB cannot parallelize that %s yet",
										query->commandType==CMD_UPDATE ? "UPDATE" : "DELETE")
							));
						}

						break;

					case POLICYTYPE_ENTRY:
						if ( plan->flow->flotype == FLOW_PARTITIONED
							|| plan->flow->flotype == FLOW_REPLICATED
							|| ( plan->flow->flotype == FLOW_SINGLETON && plan->flow->segindex != -1 )
							)
						{
							ereport(ERROR, (
								errcode(ERRCODE_CDB_FEATURE_NOT_YET),
									errmsg("CDB cannot parallelize that %s yet",
										query->commandType==CMD_UPDATE ? "UPDATE" : "DELETE")
							));
						}
						/*
						* Source is, presumably, a dispatcher singleton.
						*/
						plan->flow->req_move = MOVEMENT_NONE;
						break;
					default:
						Assert( false );
						break;
				}
			}
			break;
			
		default:
			Assert(false); /* Never see non-DML in here! */
			return NULL;
	}
	
	result = (Plan*)apply_motion_mutator((Node*)plan, &state);
	
	result->nMotionNodes = state.nextMotionID - 1;
	return result;
}


/*
 * Function apply_motion_mutator() is the workhorse for apply_motion().
 */
Node* apply_motion_mutator(Node*  node, ApplyMotionState *context)
{
	Node   *newnode;

    if ( node == NULL )
		return NULL;
	
	/* Copy this node and mutate its children. Afterward, this node should
	 * be an exact image of the input node, except that contained nodes
	 * requiring parallelization will have had it applied.
	 */
	newnode = plan_tree_mutator(node, apply_motion_mutator, context);
	
	/* Perform special transformation, if specified. */
	if ( is_plan_node(node) &&
         ((Plan*)node)->flow != NULL &&
         ((Plan*)node)->flow->req_xform != MOVEMENT_NONE )
	{
		Plan *plan = (Plan*)node;
		
		/* Special aggregrate transformation? */
		if ( IsA(node,Agg) && plan->flow->req_xform == MOVEMENT_AGG_RESTRUCT )
		{
			newnode = (Node*)apply_agg_motion((Plan*)newnode, context);
		}
		/* Special limit transformation? */
		else if ( IsA(node,Limit) && plan->flow->req_xform == MOVEMENT_LIM_RESTRUCT )
		{
			newnode = (Node*)apply_limit_motion((Plan*)newnode, context);
		}
		else
		{
			 ereport(ERROR, (
				errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("Unknown CDB restructuring transformation %d for node type %d",
					plan->flow->req_xform,
					node->type)
			 ));
		}
	}
	
	/* Add motion atop the copied node, if necessary. */
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
			Assert( ((Plan*)newnode)->flow == NULL ||
                    ((Plan*)newnode)->flow->req_move == MOVEMENT_NONE );
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
		/* case T_SubPlan: Important, but handled in expression_tree_mutator. */
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				Plan *plan = (Plan*)newnode;
				Flow *flow = plan->flow;
				
				bool sendToSecondary = false; /* always for now */
				
                /* If no Flow node, we're in a portion of the tree
                 * that was finished by the planner.
                 */
                if (!flow)
                    break;

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
								flow->sortOperators
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
								flow->sortOperators
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
					case MOVEMENT_REP2PARTITIONED:
						Assert( flow->hashExpr != NULL );
						if ( flow->req_move!=MOVEMENT_NONE && flow->numSortCols>0 )
						{
							Assert(false); /* This case disallowed. */
							newnode = (Node*)make_sorted_hashed_motion(
								plan,
								sendToSecondary,
								flow->hashExpr,
								flow->numSortCols,
								flow->sortColIdx,
								flow->sortOperators
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
						}
						else
						{
							newnode = (Node*)make_hashed_motion(
								plan,
								sendToSecondary,
								flow->hashExpr
								);
							/* Assign unique node number to the new node. */
							assignMotionID(newnode, context, NULL);
						}

						if ( flow->req_move == MOVEMENT_REP2PARTITIONED )
							setHashToSelfOnly( (Motion *)newnode );

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
 * Given the targetlist of a [potentially] projecting Plan operator and
 * a vector of attribute numbers of the single argument relation of the
 * plan, try to create a palloc'd vector of attribute numbers of the given
 * targetlist.
 *
 * A result of true indicates sucess, in which casea pointer to the new
 * vector is stored at location pinverse.  Note that  a NULL pointer may
 * occur with as a sucess, iff the number of attributes if zero.
 *
 * A result of false indicates failure which may be due to a failure to
 * find a targetlist entry corresponding to a given argument attribute (the
 * attribute may have been projected out) or due to some irregularity such
 * as a mismatch between a targetlist index and the resno declared in the
 * targetlist entry itself.
 */
bool invertColIdx(List *targetlist, int numCols, AttrNumber *colIdx, AttrNumber **pinverse)
{
	ListCell *cell;
	int i;
	AttrNumber tleidx, *invGrpColIdx;
	TargetEntry *tle;
	bool aok = TRUE;
	
	*pinverse = NULL;
	if ( numCols == 0 )
		return aok;
		
	invGrpColIdx = palloc(numCols * sizeof(AttrNumber));
	
	for ( i = 0; i < numCols; i++ )
	{
		AttrNumber child_tleidx = colIdx[i];
		tleidx = 0;
		foreach ( cell, targetlist )
		{
			tle = (TargetEntry*)lfirst(cell);
			tleidx++;
			if ( IsA(tle->expr, Var) && ((Var*)tle->expr)->varattno == child_tleidx )
			{
				if (tleidx != tle->resdom->resno)
				{
					aok = FALSE;
					break;
				}
				invGrpColIdx[i] = tleidx;
			}
		}
		if ( !aok ) break;
	}
	
	if ( aok )
	{
		*pinverse = invGrpColIdx;
	}
	
	return  aok;
}

/*
 * Create a palloc'd vector of Atttribute numbers over the targetlist
 * of the given Agg node corresponding to its grpColIdx vector (which
 * contains Attribute numbers over the targetlist of its child).
 *
 * A pointer to the new array or a NULL (if there are no grouping
 * attributes) is stored at location pinverse.
 *
 * Result = TRUE indicates success.  Note that a NULL pointer at location
 * pinverse counts as a success, iff there are no grouping attributes.
 *
 * Failure results from a mismatch between the targetlist index and the
 * resno declared in the targetlist entries themselves.
 */
bool invertGrpColIdx(Agg *agg, AttrNumber **pinverse)
{
	return invertColIdx(agg->plan.targetlist, agg->numCols, agg->grpColIdx,  pinverse);
}


/* -----------------------------------------------------------------------
 * Helper functions for extracting Lists of join expressions
 * from nodes, where the logic varies by node type
 * -----------------------------------------------------------------------
 */
List *getHashMotionInfoList( Plan *node, HashMotionInfoType type )
{
	List *hashinfo = NIL;
	
	switch ( nodeTag(node) )
	{
		case T_Sort:
			{
				Sort		*snode = (Sort *)node;
				int			numCols = snode->numCols;
				AttrNumber *sortColIdx = snode->sortColIdx;
				
				if ( numCols == 0 || sortColIdx == NULL )
					elog( ERROR,
							"Empty sortColIdx array encountered in getHashExprList for Sort node" );
							
				hashinfo = getExprListFromTargetList(snode->plan.targetlist, numCols, sortColIdx);
			}
				
			break;
			
		case T_Agg:
			{
				Agg			*aggnode = (Agg *)node;
				int			numCols = aggnode->numCols;
				AttrNumber *grpColIdx = NULL;
				
                if ( aggnode->aggstrategy == AGG_PLAIN )
                    elog( ERROR, "there is no hash expression list for Plain Aggregate nodes");	

				if ( !invertGrpColIdx(aggnode, &grpColIdx) )
				{
					elog( ERROR,
						"Unexpected grpColIdx array contents in getHashExprList for Agg node" );
				}

				if ( numCols == 0 || grpColIdx == NULL )				
					elog( ERROR,
						"Empty grpColIdx array encountered in getHashExprList for Agg node" );
						
				hashinfo = getExprListFromTargetList(aggnode->plan.targetlist, numCols, grpColIdx);
			}
				
			break;
			
		default:
			elog( WARNING, "getHashMotionInfoList called with an unsupported node type %d", nodeTag(node) );
			break;
	}	
	
	return hashinfo;
}

/* --------------------------------------------------------------------
 * make_motion -- creates a Motion node.
 * Caller must have built the pHashDefn, pFixedDefn,
 * and pSortDefn structs already.
 * --------------------------------------------------------------------
 */
Motion *make_motion(Plan *lefttree, MotionType motionType,
			bool sendToSecondary, bool sendSorted,
			List *hashExpr,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators )
{
    Motion *node = makeNode(Motion);
    Plan   *plan = &node->plan;
    List   *hashDataTypes = NIL;

    validateMotionNodeParms(motionType,	sendToSecondary, sendSorted,
			hashExpr,
			numOutputSegs, outputSegIdx,
			numSortCols, sortColIdx, sortOperators);

    /* Build list of hash key expression data types. */
    if (hashExpr)
    {
        List       *eq = list_make1(makeString("="));
        ListCell   *cell;
        foreach(cell, hashExpr)
        {
            Node   *expr = (Node *)lfirst(cell);
            Oid     typeoid = exprType(expr);
            Oid	    eqopoid;
            Oid     lefttype;
            Oid     righttype;

            /* Get oid of the equality operator for this data type. */
            eqopoid = compatible_oper_opid(eq, typeoid, typeoid, true);
            if ( eqopoid == InvalidOid )
                ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
                                errmsg("no equality operator for typid %d",
                                       typeoid)));

            /* Get the equality operator's operand type. */
            op_input_types( eqopoid, &lefttype, &righttype );
            Assert( lefttype == righttype );
            hashDataTypes = lappend_oid(hashDataTypes, lefttype);
        }
        list_free_deep(eq);
    }

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

	node->hashToSelfOnly = false;
	node->fixedToSelfOnly = false;

	node->hashExpr = hashExpr;
	node->hashDataTypes = hashDataTypes;
	node->numOutputSegs = numOutputSegs;
	node->outputSegIdx = outputSegIdx;

	node->numSortCols = numSortCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;

	plan->extParam = bms_copy(lefttree->extParam);
	plan->allParam = bms_copy(lefttree->allParam);
	
    /* If hashed redistribution, fill in the receiving segment info. */
	node->numHashSegs = 0;
	node->hashSegIdx = NULL;
	if ( motionType == MOTIONTYPE_HASH )
    {
        node->numHashSegs = getCdbSegmentCount();
        node->hashSegIdx = makeDefaultSegIdxArray(getCdbSegmentCount());
    }

    /* If merging, fill in the sending segment info. */
    node->numInputSegs = 0;
    node->inputSegIdx = NULL;
	if ( sendSorted )
    {
        node->numInputSegs = getCdbSegmentCount();
        node->inputSegIdx = makeDefaultSegIdxArray(getCdbSegmentCount());
    }

    /* TODO Should we attach a descriptive Flow here?
	 *      What about setXxxToSelfOnly() calls?
	 */

	return node;
}

Motion *make_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex )
{
	int *outputSegIdx = palloc( sizeof(int) );
	outputSegIdx[0] = destSegIndex;

	return 	make_motion(lefttree, MOTIONTYPE_FIXED,
						sendToSecondary, false,
						NIL,
						1, outputSegIdx,
						0, NULL, NULL );
}

Motion *make_sorted_union_motion(Plan *lefttree, bool sendToSecondary, int destSegIndex,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators )
{
	int *outputSegIdx = palloc( sizeof(int) );
    int mySegIdx;

    /* TODO:  Pete needs to work with Brian on this part.  Donnie is clueless. */
    mySegIdx = Cdb_segment;
	outputSegIdx[0] = mySegIdx;

	return 	make_motion(lefttree, MOTIONTYPE_FIXED,
						sendToSecondary, true,
						NIL,
						1, outputSegIdx,
						numSortCols, sortColIdx, sortOperators);
}

Motion *make_hashed_motion(Plan *lefttree, bool sendToSecondary,
						   List *hashExpr)
{
	return make_motion(lefttree, MOTIONTYPE_HASH,
						sendToSecondary, false,
						hashExpr,
						0, NULL, 0, NULL, NULL);
}

Motion *make_sorted_hashed_motion(Plan *lefttree, bool sendToSecondary,
								List *hashExpr,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators )
{
	return make_motion(lefttree, MOTIONTYPE_HASH,
						sendToSecondary, true,
						hashExpr,
						0, NULL,
						numSortCols, sortColIdx, sortOperators );
}

Motion *make_broadcast_motion(Plan *lefttree, bool sendToSecondary)
{
	return make_motion(lefttree, MOTIONTYPE_FIXED,
						sendToSecondary, false,
						NIL,
						0, NULL,
						0, NULL, NULL );
}

Motion *make_sorted_broadcast_motion(Plan *lefttree, bool sendToSecondary,
								int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators )
{
	return make_motion(lefttree, MOTIONTYPE_FIXED,
						sendToSecondary, true,
						NIL,
						0, NULL,
						numSortCols, sortColIdx, sortOperators );
}

/*
 * setHashToSelfOnly
 * This enables "deep-sixing" of hash motion, whereby
 * a tuple is only passed along if it hashes to its own segindex.
 * This implements "replicated to partitioned" motion.
 *
 * The motion node must already have been created,
 * it must of course be a hash motion node,
 * and it must not be sorted (Currently sorted hash motion ins not permitted anyway).
 */
void setHashToSelfOnly( Motion *motion )
{
	Assert( motion != NULL );
	Assert( motion->motionType == MOTIONTYPE_HASH );
	Assert( motion->numHashSegs > 0 );
	Assert( motion->hashSegIdx != NULL );
	Assert( !motion->sendSorted );

	motion->hashToSelfOnly = true;
}

/*
 * setFixedToSelfOnly
 * This enables "deep-sixing" of fixed motion, whereby
 * a tuple is only passed along if its destination is its own segindex.
 * This implements "replicated to singleton" motion.
 *
 * The motion node must already have been created,
 * it must of course be a fixed motion node,
 * and it must not be sorted,
 * and it must not be a broadcast fixed motion node.
 */
void setFixedToSelfOnly( Motion *motion )
{
	Assert( motion != NULL );
	Assert( motion->motionType == MOTIONTYPE_FIXED );
	Assert( motion->numOutputSegs > 0 );
	Assert( motion->outputSegIdx != NULL );
	Assert( !motion->sendSorted );

	motion->fixedToSelfOnly = true;
}

/* --------------------------------------------------------------------
 * makeDefaultSegIdxArray -- creates an int array with numSegs elements,
 * with values between 0 and numSegs-1
 * --------------------------------------------------------------------
 */
int *makeDefaultSegIdxArray(int numSegs)
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
 *
 *  Static Helper routines
 * --------------------------------------------------------------------
 */

/* --------------------------------------------------------------------
 * validateMotionNodeParms -- validates the parameters passed to
 * make_motion
 * --------------------------------------------------------------------
 */
void validateMotionNodeParms(MotionType motionType,
			bool sendToSecondary, bool sendSorted,
			List *hashExpr,
			int numOutputSegs, int *outputSegIdx,
			int numSortCols, AttrNumber *sortColIdx, Oid *sortOperators )
{
	/*
	 *  Perform validation based on motionType.
	 */
	switch( motionType )
	{
		case MOTIONTYPE_HASH:
	
		/*
		 *  A Hash motion node must have hash expr info specified.
		 */
			if ( hashExpr == NIL )
				elog(ERROR, "motion type 'hash' node must have non-NULL hashExpr");
				
		/*
		 *  A Hash motion node shouldn't have FixedMotionDefn specified.
		 */
			Assert(numOutputSegs == 0 && outputSegIdx == NULL);
			break;	
	
		case MOTIONTYPE_FIXED:

		/*
		 *  An Fixed motion node must have output Segs info specified.
		 */
			if ( numOutputSegs < 0 ||
				(numOutputSegs == 0 && outputSegIdx != NULL ) ||
				(numOutputSegs > 0 && outputSegIdx == NULL ) )
				elog(ERROR, "motion type 'fixed' node must have numOutputSegs > 0 and non-NULL outputSegIdx"
							" or numOutputSegs == 0 and NULL outputSegIdx");

		/*
		 *  A Fixed motion node shouldn't have HashMotionDefn specified.
		 */
			Assert( hashExpr == NIL);
				
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
		/*
		 *  A sorted motion node must have sort Cols specified
		 */
		if (numSortCols == 0 || sortColIdx == NULL || sortOperators == NULL)
			elog(ERROR, "sorted motion node must have numSortCols > 0 and non-NULL sortColIdx and sortOperators");
	}
	else
	{
		/*
		 *  A non-sorted motion node should not have SortMotionDefn specified
		 */
		Assert(numSortCols == 0 && sortColIdx == NULL && sortOperators == NULL);
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
 * getExprListFromTargetList
 *
 * Creates a VAR that references the indicated entries from the target list,
 * sets the restype and restypmod fields from the target list info,
 * and puts them into a list.
 *
 * The AttrNumber indexes actually refer to the 1 based index into the
 * target list.
 *
 * The entries have the varno field replaced by references in OUTER.
 * ----------------------------------------------------------------
 */
List *getExprListFromTargetList(List *tlist, int numCols, AttrNumber *colIdx)
{
	int		i;
	List	*elist = NIL;
	
	for (i=0; i<numCols; i++ )
	{
		Var *new_var;

		/* Find expr in TargetList */
		AttrNumber n = colIdx[i];

		TargetEntry* target = get_tle_by_resno( tlist, n );
		if ( target == NULL )
			elog( ERROR, "no tlist entry for key %d", n );

		/* Make a Var that references the target list entry at this offset, using OUTER as the varno */
		new_var = makeVar(OUTER, n, target->resdom->restype, target->resdom->restypmod, 0);
			
		elist = lappend(elist, new_var);
	}
	
	return elist;
}

#ifdef UNUSED
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
#endif /*UNUSED*/

#ifdef UNUSED
/* ----------------------------------------------------------------
 * makeMotionTargetList
 *
 * Returns a target list for a new motion node based on the
 * argument plan's target list.
 *
 * ----------------------------------------------------------------
 */
 List * makeMotionTargetList(Plan *arg)
 {
	TargetEntry *tle;
	Resdom *new_resdom, *resdom;
	Var *new_var;
	ListCell *cell;
	
	int attno = 0;
	List *new_tlist = NIL;
	
	foreach ( cell, arg->targetlist )
	{
		tle = (TargetEntry*)lfirst(cell);
		resdom = tle->resdom;
		attno++;
		
		Assert( attno == tle->resdom->resno );
		
		new_resdom = makeResdom(
			tle->resdom->resno,
			tle->resdom->restype,
			tle->resdom->restypmod,
			tle->resdom->resname? pstrdup(tle->resdom->resname): (char *) NULL,
			tle->resdom->resjunk
		);
			
		new_var = makeVar(
			0, attno,
			new_resdom->restype, new_resdom->restypmod, 0
		);		
		
		new_tlist = lappend(new_tlist, makeTargetEntry(resdom, (Expr*)new_var));
	}
	return new_tlist;
}
#endif /*UNUSED*/

/*
 * doesUpdateAffectPartitionCols:
 * This checks to see whether an update query's target list
 * shows that
 */
bool doesUpdateAffectPartitionCols( CdbPolicy *policy, List *targetlist )
{
	/*
	 * Want to test whether an update statement possibly changes the
	 * value of any of the partitioning columns.
	 */

	/*
	 * For each partitioning column, the TargetListEntry for that varattno
	 * should be just a Var node with the same varattno.
	 */
	int i;

	Assert( policy != NULL );
	Assert( targetlist != NIL );
	Assert( policy->ptype == POLICYTYPE_PARTITIONED );
	Assert( policy->nattrs > 0 );

	for ( i=0; i<policy->nattrs; i++ )
	{
		AttrNumber colno = policy->attrs[i];
		TargetEntry *entry = get_tle_by_resno( targetlist, colno );
        Var         *var;

		Assert( entry != NULL );
		if ( nodeTag(entry->expr) != T_Var )
			return true;

		var = (Var *)entry->expr;
		if ( var->varattno != colno )
			return true;
	}

	return false;
}
