/*-------------------------------------------------------------------------
*
* cdbaggmutate.c
*	  Restructure an Agg node into partial and final Agg nodes with
*	  an intervening Motion node.  The main entry point is the function
*	  apply_agg_motion.
*
*	  Note that there are other strategies	for Agg nodes (e.g., partition
*	  on grouping attributes). There are handled directly by cdbagg.c.	The
*	  functions in this file deal strictly with the 2-stage approach to
*	  aggregation.
*
* Copyright (c) 2005-2006, Greenplum Inc
*
* IDENTIFICATION
*	  $Header$
*
*-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "lib/stringinfo.h"
#include "optimizer/clauses.h"  /* expression_tree_mutator() etc */
#include "nodes/makefuncs.h"
#include "parser/parse_expr.h"	/* exprType() and exprTypmod(). */
#include "parser/parsetree.h"   /* get_tle_by_resno() */

#include "cdb/cdbagg.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbpullup.h"
#include "cdb/cdbvars.h"

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

/* --------------------------------------------------------------------------
 * Utility structures
 * --------------------------------------------------------------------------
 */

typedef struct
SplitAggsContext
{
	List	   *inner_tlist;	/* This will be the target list of the
								 * intermediate. */

    bool        useExecutorVarFormat;

	/* Implicit result of any helper. */
	char	   *errmsg;

}	SplitAggsContext;


/* Declare forward. */
static Node *splitAggs(Node *node, SplitAggsContext * context);
static Node *splitAggs_mutator(Node *node, SplitAggsContext * context);


/*
 * Function: addInnerTle
 *
 * Helper for function apply_agg_motion.
 *
 * Add the given expression to the the inner query target list currently
 * under construction in the given context.  Return an expr by which
 * the outer query may refer to the new target entry in the inner query.
 *
 * TODO Do we need more info for the Var or the Resdom?
 */
static Expr *
addInnerTle(Expr *expr, SplitAggsContext * context)
{
	TargetEntry *inner_tle;
	AttrNumber	attno;
	char	   *attname;
	StringInfoData str;

	/* This will be our attribute number in the intermediate. */
	attno = 1 + list_length(context->inner_tlist);

	/* Make up a name for the attribute. */
	initStringInfo(&str);
	appendStringInfo(&str, "agg_%d", attno);
	attname = str.data;

	/* Make the new target list entry. */
	inner_tle = makeTargetEntry(expr,
				attno,
				attname,
				FALSE);
				
	context->inner_tlist = lappend(context->inner_tlist, inner_tle);

	/* Return a Var node by which the outer query can refer to this */
    if (context->useExecutorVarFormat)
	    return (Expr *)makeVar(0,
				               attno,
				               exprType((Node *) expr),
				               exprTypmod((Node *) expr),
				               0);
    else
        return (Expr *)copyObject(expr);
}


/*
 * insertInnerAgg
 *
 * Make a new (inner) Agg node and insert it beneath the given (outer) Agg node.
 * Returns the new inner Agg.
 */
static Agg*
insertInnerAgg(Agg* outer_agg)
{
    Agg    *inner_agg;
    Agg     temp_agg;

    /* Make a local working copy of the original (outer) Agg node. */
    temp_agg = *outer_agg;

    /* Subplan will be reattached beneath the new inner Agg.  Don't copy it. */
    temp_agg.plan.lefttree = NULL;

    /* Init plans will stay with the outer Agg.  Don't copy them. */
    temp_agg.plan.initPlan = NIL;

    /* Caller will build new targetlist and qual for inner Agg.  Don't copy. */
    temp_agg.plan.targetlist = NIL;		
    temp_agg.plan.qual = NIL;		

    /* Make inner Agg by copying our temp Agg and any remaining substructures. */
    inner_agg = (Agg *)copyObject(&temp_agg);

    /* Splice new inner Agg into the tree beneath the outer Agg. */
    inner_agg->plan.lefttree = outer_agg->plan.lefttree;
    outer_agg->plan.lefttree = (Plan *)inner_agg;

    return inner_agg;
}                               /* insertInnerAgg */


/*
  * Function: cdbaggmutate_transformToTwoStage
  *
  * A 2-stage aggregate transformation restructures a given Agg node into
  * an intermediate Agg node that computes a partial result (per segment
  * database) and a final Agg node that computes final result (per segment
  * or on a specific segment).	The final Agg node is the given node.
  *
  * In addition, a Motion node is introduced between the Agg nodes.  This
  * either focuses the partial results onto to a single database process
  * or repartitions the partial results on the grouping attributes.
  *
  * The flow characteristics of the final Agg node depend on the motion.
  * In the repartitioning case, the hash specifications must be in terms
  * of the final Agg (which, of course, is congruent to the original.)
  *
  * The general idea is to split the argument Agg node in two.  The
  * original (agg) will be incrementally transformed into the final
  * aggregation, the copy (inner_agg) into the partial or intermediate
  * aggregation.
  *
  * 'useExecutorVarFormat' must be true if called after optimization, when
  * set_plan_references() has been called and Var nodes have been adjusted to
  * refer to items in the subplan's targetlist.  It must be false before the
  * end of optimization, when Var nodes are still in parser format, referring
  * to RTE items.
  *
 */

void
cdbaggmutate_transformToTwoStage(Agg *agg, bool useExecutorVarFormat)
{
    Plan       *child_plan = agg->plan.lefttree;
    List       *hashExpr = NIL;
    Agg		   *inner_agg;
    AttrNumber *sortColIdx = NULL;
    AttrNumber *resGrpColIdx = NULL;
    Oid        *sortOps = NULL;
	int			i;
    ListCell   *cell;
	TargetEntry *tle;
	SplitAggsContext context;

    Assert(!agg->plan.flow &&
           child_plan->flow &&
           child_plan->flow->flotype == FLOW_PARTITIONED);

    /*
     * Make inner Agg node and splice it beneath the outer Agg.
     */
    inner_agg = insertInnerAgg(agg);

    /* Allocate sort info arrays for the inner Agg's Flow. */
    if (agg->aggstrategy == AGG_SORTED)
    {
        Assert(agg->numCols <= child_plan->flow->numSortCols);
        sortColIdx = palloc(agg->numCols * sizeof(*sortColIdx));
        sortOps = palloc(agg->numCols * sizeof(*sortOps));
    }

	/*
     * All the grouping cols are in the original Agg's targetlist.  Find them.
     *
     * We'll take the grouping cols from the result targetlist instead of
     * the child targetlist so that EXPLAIN can display a column alias which
     * might be present in the result TargetEntry.
     */
    i = cdbpullup_colIdx((Plan *)agg,
                         child_plan,
                         agg->numCols,
                         agg->grpColIdx,
                         &resGrpColIdx);
    Assert(i == agg->numCols);

    /*
	 * Begin constructing the target list for the intermediate (partial) Agg
	 * node by adding entries corresponding to the grouping attributes.
	 */
    context.inner_tlist = NIL;
    context.useExecutorVarFormat = useExecutorVarFormat;
    context.errmsg = NULL;

    /*
     * Build a new targetlist for the inner Agg, starting with the
     * grouping columns.  Also build hash/sort keys for the Motion
     * node that will eventually be inserted between the two Agg nodes.
     */
    for (i = 0; i < agg->numCols; i++)
	{
        TargetEntry    *new_tle;

        /* Find this grouping column in the outer Agg's targetlist. */
        tle = get_tle_by_resno(agg->plan.targetlist, resGrpColIdx[i]);
        Assert(tle);

        /* Add the grouping column to the inner Agg's targetlist. */
        new_tle = copyObject(tle);
        new_tle->resno = i + 1;
        context.inner_tlist = lappend(context.inner_tlist, new_tle);

        /* Grouping cols become hash/sort keys for Motion node. */
        if (agg->aggstrategy == AGG_HASHED)
        {
            Expr   *hx = copyObject(new_tle->expr);
            Var    *var;

            /* Adjust Var node to reference the inner Agg's targetlist entry. */
            if (useExecutorVarFormat)
            {
                var = (Var *)hx;
                Assert(IsA(var, Var) && var->varno == 0);
                var->varno = OUTER;
                var->varattno = new_tle->resno;
            }

            hashExpr = lappend(hashExpr, hx);
        }
        else if (agg->aggstrategy == AGG_SORTED)
        {
            Assert(inner_agg->grpColIdx[i] == child_plan->flow->sortColIdx[i]);
            sortOps[i] = child_plan->flow->sortOperators[i];
            sortColIdx[i] = new_tle->resno;
        }

	    /*
	     * Since we just put the grouping attributes on front of the inner
	     * targetlist, and since Motion nodes are non-projecting, we know that
	     * the grouping attributes of the outer Agg's input (the Motion) will be
	     * 1,2,3,....
	     */
        agg->grpColIdx[i] = new_tle->resno;
    }

	/*
	 * Now mutate the given Agg's target list and qual (the HAVING clause) to
	 * refer to the result of the intermediate Agg (rather than the child
	 * plan).  As we go, we add any necessary attributes (Aggref expressions)
	 * to the intermediate Agg target list so we can refer to them in the
	 * mutated outer target list (or qual).
	 *
	 * This can fail since there may be aggregate functions we don't know how to
	 * split.  Therefore, we need to check for sucess as we go.
	 */
	foreach(cell, agg->plan.targetlist)
	{
		tle = (TargetEntry *) (lfirst(cell));
		tle->expr = (Expr *) splitAggs((Node *) tle->expr, &context);
	}

	/*
	 * TODO Push qual conjuncts without Aggrefs down to inner_plan!
	 *		This would eliminate groups at the inner plan and avoid
	 *		shipping, consolidating, and discarding them on the outer.
	 */

	agg->plan.qual = (List *) splitAggs((Node *) agg->plan.qual, &context);

	if (context.errmsg != NULL)
	{
		ereport(ERROR, (
						errcode(ERRCODE_CDB_FEATURE_NOT_YET),
						errmsg("Cannot parallelize that command yet"),
						errdetail(context.errmsg)
						));
	}

	/* Finish composing the inner plan. */
	inner_agg->plan.targetlist = context.inner_tlist;

	/* Set inner Flow to request insertion of Motion above the inner Agg. */
    inner_agg->plan.flow = makeFlow(child_plan->flow->flotype);
	switch (agg->aggstrategy)
	{
		case AGG_PLAIN:
            /* Move all rows to a single process in any order. */
            Insist(focusPlan((Plan *)inner_agg, true, false, false));
            break;

        case AGG_SORTED:
            /* Move all rows to a single process, ordered on grouping cols. */
            Insist(focusPlan((Plan *)inner_agg, true, true, false));
            inner_agg->plan.flow->numSortCols = agg->numCols;
            inner_agg->plan.flow->sortColIdx = sortColIdx;
            inner_agg->plan.flow->sortOperators = sortOps;
            break;

		case AGG_HASHED:
            /* Repartition on the grouping cols. */
            Insist(repartitionPlan((Plan *)inner_agg, true, false, false, hashExpr));
            break;

		default:
			Insist(0);
	}

    /* Outer Agg inherits the flow that will result from the Motion. */
    agg->plan.flow = pull_up_Flow((Plan *)agg, (Plan *)inner_agg,
                                  agg->aggstrategy == AGG_SORTED);

    /* TODO - Adjust costs for explain */
}                               /* cdbaggmutate_transformToTwoStage */


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
splitAggs(Node *node, SplitAggsContext * context)
{
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
 *	  Based on the type of aggregation, update the inner_tlist under
 *	  construction in the context by adding TargetEntry nodes for any
 *	  partial aggregation results needed to compute that Aggref's result.
 *	  These will eventually become the target list of the intermediate plan.
 *
 *	  Instead of returning the Aggref, return a pointer to a new expression
 *	  that computes the desired Aggref result from the partial results just
 *	  added to (or found in) the inner_tlist.
 *
 * On encountering a Var node outside an Aggref:
 *
 *	  Check that the Var refers to a grouping attribute and return a
 *	  Var referring to the corresponding grouping attribute in the
 *	  inner_tlist
 *
 */
static Node *
splitAggs_mutator(Node *node, SplitAggsContext * context)
{
	if (node == NULL || context->errmsg != NULL)
		return NULL;

	if (IsA(node, Aggref))
	{
		int i;
		Aggref* aggref = (Aggref*)node;
        CdbAggTactic *tactic;
        Node	   *new_expr;
		
		/*
		 * Get the specific tactic by which to parallelize this aggregate
		 * function call.
		 */
		tactic = getAggTactic(aggref, &(context->errmsg));
		if (context->errmsg != NULL || tactic == NULL)
		{
			freeAggTactic(tactic);
			if (context->errmsg == NULL)
				context->errmsg = "No tactic for implementing this aggregation";
			ereport(ERROR, (
							errcode(ERRCODE_CDB_FEATURE_NOT_YET),
							errmsg("Cannot parallelize that command yet"),
							errdetail(context->errmsg)
							));
			return NULL;
		}

		/*
		 * Record partial aggregation(s) for inner target list. This involves
		 * construction of one or more partial aggregration expressions for
		 * execution on the tails and Var nodes by which the head query may
		 * refer to their results.
		 */
		for (i = 0; i < tactic->numPartials; i++)
		{
			CdbAggPartial *partial = &tactic->partials[i];

			Aggref	   *partial_agg;

			FLATCOPY(partial_agg, aggref, Aggref);
			partial_agg->aggfnoid = partial->aggfnoid;
			partial_agg->aggtype = partial->rettype;

			if (partial->argfnoid != InvalidOid)
			{
				/* Apply the function to the argument. */
				FuncExpr   *expr = makeFuncExpr(partial->argfnoid,
												partial->argtype,
											 list_make1(partial_agg->target),
												COERCE_EXPLICIT_CALL);

				partial_agg->target = (Expr *) expr;
				Assert(!partial_agg->aggstar);
			}

			partial->expr = addInnerTle((Expr *) partial_agg, context);
		}

		/*
		 * Revise argument aggregate call to compute result from fragments.
		 * This involves construction of one or more final aggregation
		 * expressions that take their arguments from the partial aggregation
		 * expressions constructed above and the construction of the outer
		 * consolidating expression that calculates the result.
		 */
		for (i = 0; i < tactic->numFinals; i++)
		{
			CdbAggFinal *final = &tactic->finals[i];
			CdbAggPartial *partial = &tactic->partials[final->argindex];

			Aggref	   *final_agg = makeNode(Aggref);

			final_agg->aggfnoid = final->aggfnoid;
			final_agg->aggtype = final->rettype;
			final_agg->agglevelsup = 0;
			final_agg->aggstar = FALSE;
			final_agg->aggdistinct = FALSE;

			if (final->argfnoid != InvalidOid)
			{
				/* Apply the function to the argument. */
				FuncExpr   *expr = makeFuncExpr(final->argfnoid,
												final->argtype,
												list_make1(partial->expr),
												COERCE_EXPLICIT_CALL);

				final_agg->target = (Expr *) expr;
			}
			else
			{
				final_agg->target = (Expr *) (partial->expr);
			}

			/* Cache the Expr for use in the consolidator call. */
			final->expr = (Expr *) final_agg;
		}

		/*
		 * Finally we're ready to put together the call to the consolidator in
		 * order to create the result of the aggregate from the final
		 * aggregation results developed above.
		 */
		if (tactic->consolidator == InvalidOid)
		{
			/* Use the single final aggregate as the result. */
			Assert(tactic->numFinals == 1);
			new_expr = (Node *) (tactic->finals[0].expr);
		}
		else
		{
			FuncExpr   *expr;
			List	   *args = NIL;

			for (i = 0; i < tactic->numFinals; i++)
			{
				args = lappend(args, tactic->finals[i].expr);
			}

			expr = makeFuncExpr(tactic->consolidator,
								aggref->aggtype,
								args,
								COERCE_EXPLICIT_CALL);
			new_expr = (Node *) expr;
		}
		return new_expr;
	}

    /* Var nodes can reference grouping columns only. */
    if (IsA(node, Var))
	{
        Var            *var = (Var *)copyObject(node);
        TargetEntry    *tle;

        /* If executor format, pull up to reference inner Agg's targetlist. */
        if (var->varno == 0)
        {
            tle = cdbpullup_findSubplanRefInTargetList(var->varattno,
                                                       context->inner_tlist);
            Assert(tle);
            var->varattno = tle->resno;
        }
        return (Node *)var;
	}

	/* By default, just keep mutating. */
	return expression_tree_mutator(node, splitAggs_mutator, (void *) context);
}
