/*-------------------------------------------------------------------------
*
* cdblimmutate.c
*	  Restructure a Limit node into partial and final Limit nodes with
*     an intervening Motion node.  The main entry point is the function
*     apply_limit_motion.
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
#include "cdb/cdbvars.h"
#include "cdb/cdbmutate.h"
#include "cdb/cdbllize.h"


 /*
  * Function: apply_limit_motion
  *
  * A limit transformation restructures a given Limit node returning a
  * partitioned result into a preliminary Limit node that returns "enough" 
  * rows from each segment of the partitioned result, an order-preserving
  * fixed Motion node to bring the results together on a single process,
  * and a copy of the original Limit node to do the final thinning of
  * the result.
  *
  * The flow characteristics of the final Limit node are just like those
  * of the input Limit node except for partitioning.
  */
Plan *
apply_limit_motion(Plan *plan, ApplyMotionState *state)
{
	Limit *limit;
	Limit *prelimit;
	Plan *input_plan;
	List *input_initPlan;
	Flow *output_flow;
	
	/* Sanity checks */
	Assert( 
		plan != NULL
		&& IsA(plan,Limit) 
		&& plan->flow->req_xform == MOVEMENT_LIM_RESTRUCT 
		&& plan->lefttree != NULL
		&& plan->lefttree->flow != NULL
		&& plan->lefttree->flow->flotype == FLOW_PARTITIONED
		&& plan->righttree == NULL
	);

	limit = (Limit*)plan;
	input_plan = plan->lefttree;
	input_initPlan = plan->initPlan;
	output_flow = plan->flow;
	
	/* Reassert what  prescan should have checked. */
	Assert( limit->limitOffset == NULL || IsA(limit->limitOffset, Const) );
	Assert( limit->limitCount == NULL || IsA(limit->limitCount, Const) );
	
	/* TODO - If we KNOW the input is unordered, we can allow it.  The problem
	 *        is that we don't currently distinguish unknown from unordered.
	 */
	Assert( input_plan->flow->numSortCols > 0 );
	
	/* Make a copy of the given Limit node to serve as the preliminary
	 * Limit node.  First,  remove stuff we don't want in the copy.
	 */
	plan->flow = NULL;
	plan->lefttree = NULL;
	plan->initPlan = NIL;
	prelimit = copyObject(limit);
	
	/* The attached xform flow is no longer interesting. */
	output_flow->req_xform = MOVEMENT_NONE;
	
	/* Reestablish flows and things we omitted from the copy. */
	
	plan->flow = output_flow; /* prescan set this up correctly. */
	
	prelimit->plan.lefttree = input_plan;
	prelimit->plan.initPlan = input_initPlan;
	prelimit->plan.flow = pull_up_Flow((Plan*)prelimit, true);
	
	/* In preliminary (sorted), count must specify offset+count tuples
	 * from each segment of the partitioned input (just in case).
	 */
	if ( prelimit->limitOffset != NULL )
	{
		if ( prelimit->limitCount == NULL )
		{
			prelimit->limitCount = prelimit->limitOffset;
			prelimit->limitOffset = NULL;
		}
		else
		{
			/* TODO Set prelimit->limitCount to sum of offset and limit 
			 *      when we smart enough to do so.  Until then...
			 */
			ereport(ERROR, (
				errcode(ERRCODE_CDB_FEATURE_NOT_YET),
				errmsg("CDB cannot parallelize that command yet"),
				errdetail("LIMIT specifies both offset and count")
			));
		}
	}
	
	
	/* Splice in Motion. */
	plan->lefttree = (Plan*)make_sorted_union_motion(
		(Plan*)prelimit, 
		false, 
		-1,
		input_plan->flow->numSortCols,
		input_plan->flow->sortColIdx,
		input_plan->flow->sortOperators,
		CdbIdentity.numsegments, 
		makeDefaultSegIdxArray(CdbIdentity.numsegments)
		);
		

	assignMotionID((Node*)plan->lefttree, state, NULL);
	
	/* TODO - Adjust costs for explain */
	
	return plan;
}

