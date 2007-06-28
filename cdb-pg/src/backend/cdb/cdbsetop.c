/*-------------------------------------------------------------------------
 *
 * cdbsetop.c
 *	  Routines to aid in planning set-operation queries for parallel
 *    execution.  This is, essentially, an extension of the file
 *    optimizer/prep/prepunion.c, although some functions are not
 *    externalized.
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"

#include "cdb/cdblink.h" /* getgpsegmentCount */
#include "cdb/cdbllize.h"
#include "cdb/cdbmutate.h"
#include "cdb/cdbsetop.h"
#include "cdb/cdbvars.h"

#include "optimizer/tlist.h" /* tlist_member */
#include "parser/parse_expr.h" /* exprType and exprTypmod */
#include "parser/parsetree.h" /* get_tle_by_resno */

static Flow *copyFlow(Flow *model_flow, bool withExprs, bool withSort);
static List *makeHashExprsFromNonjunkTargets(List *targetList);

#define ARRAYCOPY(to, from, sz) \
	do { \
		Size	_size = (sz); \
		(to) = palloc(_size); \
		memcpy((to), (from), _size); \
	} while (0)

/*
 * Function: choose_setop_type
 *
 * Decide what type of plan to use for a set operation based on the loci of
 * the node list input to the set operation.
 *
 * See the comments in cdbsetop.h for discussion of types of setop plan.
 */
GpSetOpType choose_setop_type(List *planlist)
{
	ListCell *cell;
	Plan *subplan = NULL;
	bool ok_general = TRUE;
	bool ok_partitioned = TRUE;
	bool ok_replicated = TRUE;
	bool ok_single_qd = TRUE;
	bool ok_single_qe = TRUE;
	bool has_partitioned = FALSE;
	
	Assert( Gp_role == GP_ROLE_DISPATCH );
	
	foreach(cell, planlist)
	{
		Flow *subplanflow;
		subplan = (Plan*)lfirst(cell);		
		subplanflow = get_plan_flow(subplan);

		Assert(is_plan_node((Node*)subplan));
		Assert(subplanflow != NULL);
		switch ( subplanflow->locustype )
		{
			case CdbLocusType_Hashed:
			case CdbLocusType_HashedOJ:
			case CdbLocusType_Strewn:
				ok_general = ok_replicated = FALSE;
				has_partitioned = TRUE;
				break;
				
			case CdbLocusType_Entry:
				ok_general = ok_partitioned = ok_replicated = ok_single_qe = FALSE;
				break;
				
			case CdbLocusType_SingleQE:
				ok_general = ok_replicated = FALSE;
				break;
				
			case CdbLocusType_General:
				break;

			case CdbLocusType_Null:
			case CdbLocusType_Replicated:
			default:
				return PSETOP_NONE;
		}
	}
	
	if ( ok_general )
		return PSETOP_GENERAL;
	else if ( ok_partitioned && has_partitioned )
		return PSETOP_PARALLEL_PARTITIONED;
	else if ( ok_single_qe )
		return PSETOP_SEQUENTIAL_QE;
	else if ( ok_single_qd )
		return PSETOP_SEQUENTIAL_QD;
	
	return PSETOP_NONE;
}


void adjust_setop_arguments(List *planlist, GpSetOpType setop_type)
{
	ListCell *cell;
	Plan *subplan;
	Plan *adjusted_plan;
	
	foreach ( cell, planlist )
	{
		Flow* subplanflow;
		subplan = (Plan*)lfirst(cell);
		subplanflow = get_plan_flow(subplan);

		Assert(is_plan_node((Node*)subplan));
		Assert(subplanflow != NULL);
	
		adjusted_plan = subplan;
		switch ( setop_type )
		{
		case PSETOP_GENERAL:
			/* This only occurs when all arguments are general. */
			break;

		case PSETOP_PARALLEL_PARTITIONED:
			switch ( subplanflow->locustype )
			{
				case CdbLocusType_Hashed:
				case CdbLocusType_HashedOJ:
				case CdbLocusType_Strewn:
					Assert( subplanflow->flotype == FLOW_PARTITIONED );
					break;
				case CdbLocusType_SingleQE:
				case CdbLocusType_General:
					Assert( subplanflow->flotype == FLOW_SINGLETON && subplanflow->segindex > -1 );
					/* The setop itself will run on an N-gang, so we need to
					 * arrange for the singleton input to be separately dispatched
					 * to a 1-gang and collect its result on one of our N QEs.
					 * Hence ... */
					adjusted_plan = (Plan *)make_motion_hash_all_targets(NULL, subplan);
					break;
				case CdbLocusType_Null:
				case CdbLocusType_Entry:
				case CdbLocusType_Replicated:
				default:
					ereport(ERROR, (
						errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("unexpected argument locus to set operation") ));
					break;
			}
			break;
			
		case PSETOP_SEQUENTIAL_QD:
			switch ( subplanflow->locustype )
			{
				case CdbLocusType_Hashed:
				case CdbLocusType_HashedOJ:
				case CdbLocusType_Strewn:
					Assert( subplanflow->flotype == FLOW_PARTITIONED );
					adjusted_plan = (Plan*)make_motion_gather_to_QD(subplan, false);				
					break;
					
				case CdbLocusType_SingleQE:
					Assert( subplanflow->flotype == FLOW_SINGLETON && subplanflow->segindex == 0 );
					adjusted_plan = (Plan*)make_motion_gather_to_QD(subplan, false);				
					break;

				case CdbLocusType_Entry:
				case CdbLocusType_General:
					break;
					
				case CdbLocusType_Null:
				case CdbLocusType_Replicated:
				default:
					ereport(ERROR, (
						errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("unexpected argument locus to set operation") ));
					break;
			}
			break;
			
		case PSETOP_SEQUENTIAL_QE:
			switch ( subplanflow->locustype )
			{
				case CdbLocusType_Hashed:
				case CdbLocusType_HashedOJ:
				case CdbLocusType_Strewn:
					Assert( subplanflow->flotype == FLOW_PARTITIONED );
					/* Gather to QE.  No need to keep ordering. */
					adjusted_plan = (Plan*)make_motion_gather_to_QE(subplan, false);				
					break;
					
				case CdbLocusType_SingleQE:
					Assert( subplanflow->flotype == FLOW_SINGLETON && subplanflow->segindex != -1 );
					break;

				case CdbLocusType_General:
					break;
					
				case CdbLocusType_Entry:
				case CdbLocusType_Null:
				case CdbLocusType_Replicated:
				default:
					ereport(ERROR, (
						errcode(ERRCODE_CDB_INTERNAL_ERROR),
						errmsg("unexpected argument locus to set operation") ));
					break;
			}
			break;
			
		case PSETOP_PARALLEL_REPLICATED:
			/* Only when all args are replicated. */
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("unexpected replicated intermediate result"),
				errdetail("argument to set operation may not be replicated") ));
			break;
			
		default:
			/* Can't happen! */
			ereport(ERROR, (
				errcode(ERRCODE_CDB_INTERNAL_ERROR),
				errmsg("unexpected arguments to set operation") ));
			break;
		}
		
		/* If we made changes, inject them into the argument list. */
		if ( subplan != adjusted_plan )
		{
			subplan = adjusted_plan;
			cell->data.ptr_value = subplan;
		}
	}
	
	return;
}


/*
 * Copy a Flow node.  Only the declarative part is preserved.  Not, e.g.,
 * any required movement or transformation.  Hash information is preserved
 * only if withExprs is true. Sort specifications are preserved only
 * if withSort is true.
 *
 * A NULL result indicates either a NULL argument or a problem.
 */
Flow *copyFlow(Flow *model_flow, bool withExprs, bool withSort)
{
	Flow *new_flow = NULL;
	
	if (model_flow == NULL)
		return NULL;

	new_flow = makeFlow(model_flow->flotype);
	new_flow->locustype = model_flow->locustype;

	if (model_flow->flotype == FLOW_PARTITIONED)
	{
		/* Copy hash attribute definitions, if wanted and available. */
		if (withExprs && model_flow->hashExpr != NULL)
		{
			new_flow->hashExpr = copyObject(model_flow->hashExpr);
		}
	}
	else if (model_flow->flotype == FLOW_SINGLETON)
	{
		/* Propagate segment definition. */
		new_flow->segindex = model_flow->segindex;
	}
	else if (model_flow->flotype != FLOW_REPLICATED)
	{
		/* Clean up and give up. This isn't one of our blessed types. */
		pfree(new_flow);
		return NULL;
	}

	/* Propogate sort attributes, if wanted and available. */
	if (withSort && model_flow->numSortCols > 0)
	{
		new_flow->numSortCols = model_flow->numSortCols;
		ARRAYCOPY(
			new_flow->sortColIdx,
			model_flow->sortColIdx,
			model_flow->numSortCols * sizeof(AttrNumber) );
		ARRAYCOPY(
			new_flow->sortOperators,
			model_flow->sortOperators,
			model_flow->numSortCols * sizeof(Oid) );
	}

	return new_flow;
}


/*
 * make_motion_gather_to_QD
 *		Add a Motion node atop the given subplan to gather the tuples
 *      from an input gang to the QD. This motion should only be applied to
 *      a non-replicated, non-root subplan.
 */
Motion* make_motion_gather_to_QD(Plan *subplan, bool keep_ordering)
{
	return make_motion_gather(subplan, -1, keep_ordering);
}

/*
 * make_motion_gather_to_QE
 *		Add a Motion node atop the given subplan to gather tuples to
 *      a single QE. This motion should only be applied to a partitioned
 *      subplan.
 */
Motion* make_motion_gather_to_QE(Plan *subplan, bool keep_ordering)
{
	return make_motion_gather(subplan, gp_singleton_segindex, keep_ordering);
}	

/*
 * make_motion_gather
 *		Add a Motion node atop the given subplan to gather tuples to
 *      a single process. This motion should only be applied to a partitioned
 *      subplan.
 */
Motion* make_motion_gather(Plan *subplan, int segindex, bool keep_ordering)
{
	Motion *motion;
	int numInputSegs;				

	Assert(subplan->flow != NULL);
	Assert(subplan->flow->flotype == FLOW_PARTITIONED);
	
	numInputSegs =  (subplan->flow->flotype == FLOW_SINGLETON) ? 1 : getgpsegmentCount();
	
	if ( keep_ordering && subplan->flow->numSortCols > 0 )
	{
		Flow *flow = subplan->flow;
			
		motion = make_sorted_union_motion(
			subplan,
			false, /* sendToSecondary */
			numInputSegs,
			segindex,
			flow->numSortCols, /* Motion and Flow can share arrays. */
			flow->sortColIdx,
			flow->sortOperators);
	}
	else
	{
		motion = make_union_motion(
			subplan,
			false, /* sendToSecondary */
			numInputSegs,
			segindex);
	}
	
	return motion;
}

/*
 * make_motion_hash_all_targets
 *		Add a Motion node atop the given subplan to hash collocate
 *      tuples non-distinct on the non-junk attributes.  This motion
 *      should only be applied to a non-replicated, non-root subplan.
 */
Motion* make_motion_hash_all_targets(PlannerInfo *root, Plan *subplan)
{
	List *hashexprs = makeHashExprsFromNonjunkTargets(subplan->targetlist);	
	return make_motion_hash(root, subplan, hashexprs);
}

/*
 * make_motion_hash
 *		Add a Motion node atop the given subplan to hash collocate
 *      tuples non-distinct on the values of the hash expressions.  This
 *      motion should only be applied to a non-replicated, non-root subplan.
 */
Motion* make_motion_hash(PlannerInfo *root, Plan *subplan, List *hashexprs)
{
	Motion *motion;
	int numInputSegs;
	
	Assert(subplan->flow != NULL);
	
	numInputSegs =  (subplan->flow->flotype == FLOW_SINGLETON) ? 1 : getgpsegmentCount();
	
	motion = make_hashed_motion(
		subplan,
		false, /* sendToSecondary */
		numInputSegs,
		hashexprs);

	return motion;
}

/* 
 * make_motion_multiplex
 *		Add a motion on top of the given subplan.
 *		This should only be applied on top of a non-replicated, non-root subplan.
 *		The motion node created here is a multiplex motion, but no slice, route.
 *		Slice and routes should be added later.  Also, this motion does NOT have a
 * 		flow in plan.flow.  The flow is also multiplexed for this motion and will be
 *      constructed later when slice/route are added.
 */
Motion *make_motion_multiplex(PlannerInfo *root, Plan *subplan)
{
	Motion *motion;
	int numInputSegs;
	Assert(subplan->flow != NULL);

	numInputSegs =  (subplan->flow->flotype == FLOW_SINGLETON) ? 1 : getgpsegmentCount();

	motion = make_multiplex_motion(subplan, numInputSegs);
	return motion;
}

/* Get flow of a motion node */
Flow *get_motion_flow(Motion *node, int sliceIdx)
{
	MotionSliceEntry *slice = (MotionSliceEntry *) list_nth(node->sliceEntries, sliceIdx);
	Assert(slice);
	return slice->flow;
}

Flow *get_plan_flow(Plan *node)
{
	if(IsA(node, Motion))
	{
		Motion *m = (Motion *) node;
		if(node->flow)
		{
			Assert(node->flow->flotype == FLOW_MULTIPLEXED);
			return node->flow;
		}

		Assert(list_length(m->sliceEntries) == 1);
		return get_motion_flow(m, 0);
	}

	return node->flow;
}

/*
 * makeHashExprsFromNonjunkTargets
 *		Make a list of hash expressions over all non-resjunk targets in
 *		the targetlist are in the given target list.  This will align
 *		with the sort attributes used as input to a SetOp or Unique
 *		operator.
 *
 * Returns the newly allocate expression list for a Motion node.
 */
List *makeHashExprsFromNonjunkTargets(List *targetlist)
{
	ListCell   *cell;
	List *hashlist = NIL;

	foreach(cell, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(cell);

		if (!tle->resjunk)
		{
			hashlist = lappend(hashlist, copyObject(tle->expr));
		}
	}
	return hashlist;

}

/*
 *     Marks an Append plan with its locus based on the set operation
 *     type determined during examination of the arguments.
 */
void mark_append_locus(Plan *plan, GpSetOpType optype)
{
	switch ( optype  )
	{
	case PSETOP_GENERAL:
		mark_plan_general(plan);
		break;
	case PSETOP_PARALLEL_PARTITIONED:
		mark_plan_strewn(plan);
		break;
	case PSETOP_PARALLEL_REPLICATED:
		mark_plan_replicated(plan);
		break;
	case PSETOP_SEQUENTIAL_QD:
		mark_plan_entry(plan);
		break;
	case PSETOP_SEQUENTIAL_QE:
		mark_plan_singleQE(plan);
	case PSETOP_NONE:
		break;
	}
}

void mark_passthru_locus(Plan *plan, bool with_hash, bool with_sort)
{
	Flow *flow;
	Plan *subplan = NULL;
	bool is_subquery = IsA(plan, SubqueryScan);

	Assert( is_plan_node((Node*)plan) && get_plan_flow(plan) == NULL );
	
	if ( is_subquery )
	{
		subplan = ((SubqueryScan*)plan)->subplan;
	}
	else
	{
		subplan = plan->lefttree;
	}
	
	Assert( subplan != NULL && get_plan_flow(subplan) != NULL); 
	
	flow = copyFlow(get_plan_flow(subplan), with_hash && !is_subquery, with_sort);
	
	
	if ( is_subquery && with_hash && flow->flotype == FLOW_PARTITIONED )
	{
		ListCell *c;
		TargetEntry *tle;
		List *hash = NIL;
		Index i = 0;
		Flow *subplanflow = get_plan_flow(subplan);
		
		/* Make sure all the expressions the flow thinks we're hashed on
		 * occur, in order, at the beginning of the subplan targetlist. 
         * (Why does it matter that they are in order and at the beginning??)
		 */
		foreach( c, subplanflow->hashExpr )
		{
			Node *x = (Node*)lfirst(c);

            i++;
			tle = get_tle_by_resno(subplan->targetlist, i);
			
            Insist(tle 
                   && (equal(x, tle->expr)
                       || (IsA(x, RelabelType)
                           && equal(((RelabelType *)x)->arg, tle->expr) )));

            hash = lappend(hash, makeVar(1, i,
										exprType((Node*)tle->expr),
										exprTypmod((Node*)tle->expr), 0));
		}
		flow->hashExpr = hash;
	}

	plan->flow = flow;
}


void mark_sort_locus(Plan *plan)
{
	plan->flow = pull_up_Flow(plan, plan->lefttree, true);
}	

void mark_plan_general(Plan* plan)
{
	Assert( is_plan_node((Node*)plan) && plan->flow == NULL );
	plan->flow = makeFlow(FLOW_SINGLETON);
	plan->flow->segindex = 0;
	plan->flow->locustype = CdbLocusType_General;
}

void mark_plan_strewn(Plan* plan)
{
	Assert( is_plan_node((Node*)plan) && plan->flow == NULL );
	plan->flow = makeFlow(FLOW_PARTITIONED);
	plan->flow->locustype = CdbLocusType_Strewn;
}

void mark_plan_replicated(Plan* plan)
{
	Assert( is_plan_node((Node*)plan) && plan->flow == NULL );
	plan->flow = makeFlow(FLOW_REPLICATED);
	plan->flow->locustype = CdbLocusType_Replicated;
}

void mark_plan_entry(Plan* plan)
{
	Assert( is_plan_node((Node*)plan) && plan->flow == NULL );
	plan->flow = makeFlow(FLOW_SINGLETON);
	plan->flow->segindex = -1;
	plan->flow->locustype = CdbLocusType_Entry;
}

void mark_plan_singleQE(Plan* plan)
{
	Assert( is_plan_node((Node*)plan) && plan->flow == NULL );
	plan->flow = makeFlow(FLOW_SINGLETON);
	plan->flow->segindex = 0;
	plan->flow->locustype = CdbLocusType_SingleQE;
}
