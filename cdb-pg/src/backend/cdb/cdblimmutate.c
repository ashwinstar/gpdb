/*-------------------------------------------------------------------------
*
* cdblimmutate.c
*	  Restructure a Limit node into partial and final Limit nodes with
*	  an intervening Motion node.  The main entry point is the function
*	  apply_limit_motion.
*
* Portions Copyright (c) 2005-2006, Greenplum Inc
* Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
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

#include "parser/parse_expr.h"	/* exprType() and exprTypmod(). */
#include "parser/parse_oper.h"	/* compatible_oper(). */
#include "catalog/pg_operator.h"	/* Form_pg_operator */
#include "utils/syscache.h"		/* ReleaseSysCache() */
#include "cdb/cdbvars.h"
#include "cdb/cdbmutate.h"
#include "cdb/cdbllize.h"

static Node *addLimitExprs(Node *limitOffset, Node *limitCount);

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
apply_limit_motion(Plan *plan, ApplyMotionState * state)
{
	Limit	   *limit;
	Limit	   *prelimit;
	Plan	   *input_plan;
	List	   *input_initPlan;
	Flow	   *output_flow;
    int         numInputSegs = getMPPSegmentCount();

	/* Sanity checks */
	Assert(
		   plan != NULL
		   && IsA(plan, Limit)
		   && plan->flow->req_xform == MOVEMENT_LIM_RESTRUCT
		   && plan->lefttree != NULL
		   && plan->lefttree->flow != NULL
		   && plan->lefttree->flow->flotype == FLOW_PARTITIONED
		);

	limit = (Limit *) plan;
	input_plan = plan->lefttree;
	input_initPlan = plan->initPlan;
	output_flow = plan->flow;

	/* Reassert what  prescan should have checked. */
	Assert(limit->limitCount != NULL || limit->limitOffset != NULL);

	/*
	 * Make a copy of the given Limit node to serve as the preliminary Limit
	 * node.  First,  remove stuff we don't want in the copy.
	 */
	plan->flow = NULL;
	plan->lefttree = NULL;
	plan->initPlan = NIL;
	prelimit = copyObject(limit);

	/* The specified xform is no longer appropriate. */
	output_flow->req_xform = MOVEMENT_NONE;

	/* Reestablish flows and things we omitted from the copy. */

	plan->flow = output_flow;	/* prescan set this up correctly. */

	prelimit->plan.lefttree = input_plan;
	prelimit->plan.initPlan = input_initPlan;
	prelimit->plan.flow = pull_up_Flow((Plan *) prelimit, input_plan, true);
	prelimit->limitOffset = NULL;
	prelimit->limitCount = NULL;

	/*
	 * In preliminary (sorted), count must specify offset+count tuples from
	 * each segment of the partitioned input (just in case).
	 */
	if (limit->limitCount != NULL)
	{
		prelimit->limitCount = copyObject(limit->limitCount);
		if (limit->limitOffset != NULL)
		{
			/* Set prelimit->limitCount to sum of offset and limit. */
			prelimit->limitCount = addLimitExprs(limit->limitOffset, limit->limitCount);

			if (prelimit->limitCount == NULL)
			{
				ereport(ERROR, (
								errcode(ERRCODE_CDB_FEATURE_NOT_YET),
								errmsg("Cannot parallelize that command yet"),
								errdetail("Incompatible OFFSET and LIMIT")
								));
			}
		}
	}

	/* Splice in Motion. */
	if (input_plan->flow->numSortCols != 0)
	{
		plan->lefttree = (Plan *) make_sorted_union_motion(
														   (Plan *) prelimit,
														   false,
                                                           numInputSegs,
														   plan->flow->segindex,
											   input_plan->flow->numSortCols,
												input_plan->flow->sortColIdx,
											  input_plan->flow->sortOperators
			);
	}
	else
	{
		plan->lefttree = (Plan *) make_union_motion(
													(Plan *) prelimit,
													false,
                                                    numInputSegs,
													plan->flow->segindex
			);

		ereport(WARNING, (
						  errcode(ERRCODE_WARNING),
						  errmsg("LIMIT/OFFSET of unordered result.")
						  ));
	}

    /* Verify that the correct output flow type was set during prescan. */
    Assert(  (plan->flow->flotype == plan->lefttree->flow->flotype &&
              plan->flow->base_flotype == FLOW_UNDEFINED)
          || plan->flow->base_flotype == plan->lefttree->flow->flotype);

	assignMotionID((Node *) plan->lefttree, state, NULL);

	/* TODO - Adjust costs for explain */

	return plan;
}

/* Helper to generate a node to use as a limit count that represents the
 * sum of the given nodes representing limit offset and count.	Return
 * NULL in case of problems.
 */
Node *
addLimitExprs(Node *limitOffset, Node *limitCount)
{
	Node	   *result = NULL;

	Assert(limitOffset != NULL && limitCount != NULL);

	if (IsA(limitOffset, Const) &&IsA(limitCount, Const)
		&&((Const *) limitOffset)->consttype == INT4OID
		&& ((Const *) limitCount)->consttype == INT4OID)
	{
		int32		offval = DatumGetInt32(((Const *) limitOffset)->constvalue);
		int32		limval = DatumGetInt32(((Const *) limitCount)->constvalue);
		Const	   *c = copyObject(limitCount);

		c->constvalue = Int32GetDatum(offval + limval);
		result = (Node *) c;
	}
	else
	{
		Form_pg_operator opform = NULL;
		Operator	oper = NULL;
		List	   *opname = NIL;
		Oid			ltype,
					rtype;

		opname = list_make1(makeString(pstrdup("+")));
		ltype = exprType(limitOffset);
		rtype = exprType(limitCount);
		oper = compatible_oper(opname, ltype, rtype, false);
		if (oper != NULL)
		{
			opform = (Form_pg_operator) GETSTRUCT(oper);

			if (opform->oprleft == ltype && opform->oprright == rtype)
			{
				OpExpr	   *x = makeNode(OpExpr);

				x->opno = oprid(oper);
				x->opfuncid = opform->oprcode;
				x->opresulttype = opform->oprresult;
				x->opretset = FALSE;
				x->args = list_make2(copyObject(limitOffset), copyObject(limitCount));
				result = (Node *) x;
			}
			ReleaseSysCache(oper);
		}
	}

	return (Node *) result;
}
