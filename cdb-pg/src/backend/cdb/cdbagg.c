/*-------------------------------------------------------------------------
 *
 * cdbagg.c
 *	  Generate aggregation strategies.
 *
 * Copyright (c) 2005-2006, Greenplum
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/clauses.h"
#include "cdb/cdbagg.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbvars.h"


/*
 * Context structure for helper function analyzeAggExpr_walker
 */
typedef struct
CdbAggExprAnalysis
{
	/* The cumulative results of the analysis. */
	int         ndistinct;          /* Number of Aggref specifying DISTINCT. */
	Expr       *aggref_distinct;    /* The target expr if ndistinct == 1 */
	
	/* These are working storage for analyzeAggExpr_walker. */
	int			aggdepth;		    /* Number of Aggref nodes in stack. */

} CdbAggExprAnalysis;

static bool analyzeExprs(Agg *agg, CdbAggExprAnalysis *state);
static bool analyzeAggExpr_walker(Node *node, void *context);


/*
 * cdbagg_prescan
 *
 * Helper for prescan_walker() does processing for an Agg node.
 *
 * Note that prescan_walker has already finished scanning the child nodes
 * of the node.
 *
 * Returns true if successful, false if the given Agg is not supported.
 */
bool
cdbagg_prescan(Agg *agg)
{
	Plan	   *child = agg->plan.lefttree;
	List	   *hashExpr = NIL;
    bool        aOk = false;
    bool        sorted = false;
    CdbAggExprAnalysis analysis;

	/* Argument is a prescanned plan. */
	Assert(child->flow && child->flow->flotype != FLOW_UNDEFINED);

	/* Sorted aggregation assumes ordered input & produces ordered result. */
	if (agg->aggstrategy == AGG_SORTED)
        sorted = true;

    /*
     * If source is replicated or already on dispatcher, leave it
     * where it is and do 1-stage serial aggregation there.
     */
    if ( child->flow->flotype == FLOW_REPLICATED ||
         (child->flow->flotype == FLOW_SINGLETON && child->flow->segindex < 0) )
    {
        agg->plan.flow = pull_up_Flow((Plan *)agg, child, true);
        return true;            /* success */
    }

    /* Survey the Agg node's target list and search condition. */
    aOk = analyzeExprs(agg, &analysis);

    /*
     * Consider 2-stage aggregation of a partitioned source.
     *
     *   - Execution begins by computing partial aggregates within each
     *     source partition.
     *
     *   - Then, for ungrouped ("plain") aggregation, we'll gather the
     *     partially summarized results (one row per partition) to a
     *     single process for final aggregation.  Final result is just
     *     one row (or empty).
     *
     *   - Or, for hashed aggregation, we could repartition the partially
     *     summarized results on the grouping exprs for final hashed
     *     aggregation in parallel.  (CDB TODO: Could do this for
     *     sorted aggregation also, if downstream nodes don't care
     *     about the ordering.)
     *
     *   - Or, for sorted aggregation, we could merge the partially
     *     summarized results to a single process for final sorted
     *     aggregation.
     */
    if (aOk &&
        analysis.ndistinct == 0 &&
        child->flow->flotype == FLOW_PARTITIONED)
    {
        agg->plan.flow = (agg->aggstrategy == AGG_HASHED)
                            ? makeFlow(FLOW_PARTITIONED)
                            : makeFlow(FLOW_SINGLETON);

        /* Tell mutator to split the Agg into Agg<-Motion<-Agg. */
        agg->plan.flow->req_xform = MOVEMENT_AGG_RESTRUCT;

        /* No need to set hash fields, transform code can deduce. */
        /* No child adjustments. */
        return true;            /* success */
    }

    /*
     * If nothing but one ungrouped DISTINCT aggregate, consider
     * repartitioning on its argument expr, followed by 2-stage
     * "plain" aggregation.
     *
     * The 2nd stage will gather partially summarized results (one
     * row per partition) to a single process for final non-DISTINCT
     * summarization.  Final result is just one row (or empty).
     */
    if (aOk &&
        agg->numCols == 0 &&
        analysis.ndistinct == 1)
    {
        Assert(agg->aggstrategy == AGG_PLAIN);

        hashExpr = list_make1(getHashExprForExpression(child, analysis.aggref_distinct));

        /* Repartition the input (does nothing if can't repartition). */
        if (repartitionPlan(child, true, sorted, false, hashExpr))
        {
            /* ok, next step is 2-stage parallel aggregation. */
            agg->plan.flow = pull_up_Flow((Plan *)agg, child, true);

            /* Tell mutator to build the 2-stage plan. */
            agg->plan.flow->req_xform = MOVEMENT_AGG_RESTRUCT;

            /* Final result ends up on a single QE. */
            agg->plan.flow->flotype = FLOW_SINGLETON;
            agg->plan.flow->segindex = 0;
            return true;            /* success */
        }
    }

    /*
     * Consider repartitioning on the grouping exprs.
     */
    if (agg->numCols > 0)
    {
        hashExpr = getExprListFromTargetList(child->targetlist, agg->numCols, agg->grpColIdx);

        /* Repartition the input (does nothing if can't repartition). */
        if (repartitionPlan(child, true, sorted, false, hashExpr))
        {
            /* ok, 1-stage parallel aggregation will be used. */
            agg->plan.flow = pull_up_Flow((Plan *)agg, child, true);
            return true;            /* success */
        }
    }

    /*
     * Consider gathering the source rows to a single process.  (Last resort!)
     */
    if (focusPlan(child, true, sorted, false))
    {
        /* ok, conventional 1-stage serial aggregation will be used. */
        agg->plan.flow = pull_up_Flow((Plan *)agg, child, true);
        return true;            /* success */
    }

    /* Stumped! */
    return false;               /* failure */
}                               /* cdbagg_prescan */


/*
 * Function: analyzeAggExpr
 *
 * Analyze an expression for getAggStrategyList.  The arguments are
 *
 * expr - the expression to analyze
 *
 * Return TRUE to indicate that analysis completed successfully, FALSE
 * to indicate that problems prevented completion.
 */
static bool
analyzeAggExpr(Expr *expr, CdbAggExprAnalysis * state)
{
	bool		result = TRUE;
	Node	   *node;

	Assert(state != NULL);
	node = (Node *) expr;

	/* Recur into the expression. */
	result = !analyzeAggExpr_walker(node, state);

	return result;
}


/*
 * Function: analyzeAggExpr_walker
 *
 * Recursive helper for function analyzeAggExpr.
 */
static bool
analyzeAggExpr_walker(Node *node, void *context)
{
	CdbAggExprAnalysis *state = (CdbAggExprAnalysis *) context;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;

				Assert(state->aggdepth == 0);	/* No nesting Aggrefs! */

				if (aggref->aggdistinct)
				{
					Assert(!aggref->aggstar);	/* No DISTINCT *! */
					state->ndistinct++;
					if (state->ndistinct == 1)
					{
						/* Copy 1st DISTINCT arg expr to state. */
						state->aggref_distinct = copyObject(aggref->target);
					}
					if (state->ndistinct > 1 && state->aggref_distinct != NULL)
					{
						/*
						 * If there's a 2nd DISTINCT, bail.  We're not that
						 * smart.
						 */
						pfree(state->aggref_distinct);
						state->aggref_distinct = NULL;
					}
				}

				/* Recur to check subexpressions. */
				state->aggdepth++;
				if (analyzeAggExpr_walker((Node *) (aggref->target), context))
					return TRUE;
				state->aggdepth--;
			}
			break;

		default:
			{
				if (expression_tree_walker(node, analyzeAggExpr_walker, context))
					return true;
			}
			break;
	}
	return FALSE;
}


/*
 * Function: analyzeExprs
 *
 * Examines an Agg node and fills in a CdbAggExprAnalysis structure
 * (*state) provided by the caller.
 *
 * Returns true if the examination finds no obstacles to two-stage aggregation.
 *
 * Note that this function expects an Agg node whose child has been prescanned
 * (see cdbllize.c) and, hence, has its output Flow node fully specified.
 */
bool
analyzeExprs(Agg *agg, CdbAggExprAnalysis *state)
{
	ListCell   *cell;

    /* Initialize context for expr walker */
    state->ndistinct = 0;
    state->aggref_distinct = NULL;
    state->aggdepth = 0;

	/*
	 * Analyze our own targetlist and qual for information about contained
	 * Aggref nodes.  At the moment this is just discovering whether or not
	 * there is a single Aggref that specifies DISTINCT and validating our
	 * assumptions about the expression (e.g., grouping attributes contain
	 * no Aggrefs, Aggrefs don't nest.)
	 *
	 * TODO Note whether there are Aggrefs whose functions disqualify the
	 *		2-stage (MOVEMENT_AGG_RESTRUCT) tactic. Need an API for that.
	 */
	foreach(cell, agg->plan.targetlist)
	{
        TargetEntry    *tle = (TargetEntry *)lfirst(cell);
        int             i;

        /* Skip tle if it is a grouping column. */
        for (i = 0; i < agg->numCols; i++)
        {
            if (tle->resno == agg->grpColIdx[i])
                break;
        }
		if (i < agg->numCols)
            continue;

        /* Look for Aggref nodes in the expr. */
        if (!analyzeAggExpr(tle->expr, state))
            return false;
	}

    /* Look for Aggref nodes in the search condition. */
	return analyzeAggExpr((Expr *)agg->plan.qual, state);
}                               /* analyzeExprs */

