/*-------------------------------------------------------------------------
 *
 * cdbllize.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2005-2007, greenplum inc
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

#include "optimizer/planmain.h" /* for is_projection_capable_plan() */
#include "optimizer/var.h"      /* for contain_vars_of_level_or_above() */
#include "parser/parsetree.h"	/* for rt_fetch() */
#include "utils/lsyscache.h"	/* for getatttypetypmod() */
#include "parser/parse_oper.h"	/* for compatible_oper_opid() */
#include "nodes/makefuncs.h"	/* for makeVar() */
#include "nodes/value.h"		/* for makeString() */
#include "utils/relcache.h"     /* RelationGetPartitioningKey() */

#include "cdb/cdbvars.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbpullup.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbmutate.h"

#define ARRAYCOPY(to, from, sz) \
	do { \
		Size	_size = (sz) * sizeof((to)[0]); \
		(to) = palloc(_size); \
		memcpy((to), (from), _size); \
	} while (0)

/*
 * Substructure for a PlanProfile to hold "stackable" information like
 * the range table.
 */
typedef struct LocalProfile
{
	List	   *rtable;			/* The current range table. */

	struct LocalProfile *outerFrame;	/* The next more global context or
										 * NULL. */

}	LocalProfile;

/*
 * A PlanProfile holds state for recursive prescan_walker().
 */
typedef struct PlanProfile
{
	/*
	 * The result flags refer to relation in the result of an insert, update,
	 * or delete
	 */
	bool	resultSegments;
	
	bool	dispatchParallel;

	/* Context */
	LocalProfile *local;

	int sliceDepth;
	
	bool hasNonRootSubplan;

}	PlanProfile;



/*
 * Forward Declarations
 */
static void prescan(Plan *plan, PlanProfile * context);
static bool prescan_walker(Node *node, PlanProfile * context);

static void pushLocals(PlanProfile * profile, List *rtable);
static void popLocals(PlanProfile * profile);
static List *localRangeTable(PlanProfile * profile);

static bool
adjustPlanFlow(Plan        *plan,
               bool         live,
               bool         stable,
               bool         rescannable,
               Movement     req_move,
               List        *hashExpr);

static void motion_sanity_check(Plan *plan);

/* ------------------------------------------------------------------------- *
 * Function cdbparallelize() is the main entry point.
 *
 * Parallelize a single raw PostgreSQL sequential plan. The input plan
 * is modified in that Flow nodes are attached to Plan nodes in the input
 * as appropriate and the top-level plan node's dispatch field is set
 * to specify how the plan should be handled by the executor.
 *
 * If the dispatch field of the top-level plan is set to a non-default
 * value on entry, the plan is returned unmodified.  This guarantees that
 * we won't parallelize the same input plan more than once, provides a way
 * for the planner to create a plan that this function will recognize as
 * already final.
 *
 * The result is [a copy of] the input plan with alterations to make the
 * plan suitable for execution by Greenplum Database.
 *
 *
 * TODO: Much investigation remains in the areas of
 *
 *		- initialization plans,
 *		- plan parameters,
 *		- subqueries,
 *		- ordering and hash attributes,
 *		- etc.
 *
 *		The current implementation is little more than a rough sketch.
 *
 *
 * Outline of processing:
 *
 * - Return the input plan unmodified, if it's dispatch field is set.
 *
 * - Scan (scanForManagedTables) the input plan looking for references to
 *	 managed tables.  If none are found, mark the plan for sequential
 *	 dispatch and return it.
 *
 * - Augment the input plan (prescan) by attaching Flow nodes to each of
 *	 its Plan nodes.  This is where all the decisions about parallelizing
 *	 the query occur.
 *
 * - Implement the decisions recorded in the Flow nodes (apply_motion)
 *	 to produce a modified copy of the plan.  Mark the copy for parallel
 *	 dispatch and return it.
 * ------------------------------------------------------------------------- *
 */
Plan *
cdbparallelize(Plan *plan,
			   Query *query,
			   bool isCursor,
			   int cursorOptions,
			   ParamListInfo boundParams,
			   bool uses_correlation
)
{
	int			nParamExec;
	PlanProfile profile;
	PlanProfile *context = &profile;
	
	/* Make sure we're called correctly (and need to be called).  */
	switch ( Gp_role )
	{
	case GP_ROLE_DISPATCH:
		break;
	case GP_ROLE_UTILITY:
		return plan;
	case GP_ROLE_EXECUTE:
	case GP_ROLE_UNDEFINED:
		Insist(0);
	}
	Assert(is_plan_node((Node *) plan));
	Assert(query != NULL && IsA(query, Query));
	
	/* Print plan if debugging. */
	if (Debug_print_prelim_plan)
		elog_node_display(DEBUG1, "preliminary plan", plan, Debug_pretty_print);



    /* Initialize the PlanProfile result ... */
	context->resultSegments = false;
	context->hasNonRootSubplan = false;

	context->local = NULL;
	pushLocals(context, query->rtable);

	switch (query->commandType)
	{
		case CMD_SELECT:
			if (query->into)
			{
				/* SELECT INTO always created partitioned tables. */
				context->resultSegments = true;	
			}
			break;

		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			{
				/* Since this is a data modification command, we need to
				 * include information about the result relation in the
				 * summary of relations touched. */
				Oid			reloid;
                Relation    relation;
				GpPolicy  *policy = NULL;

                /* Get Oid of target relation. */
                Insist(query->resultRelation > 0 &&
				       query->resultRelation <= list_length(localRangeTable(context)));
				reloid = getrelid(query->resultRelation, localRangeTable(context));

                /* Get a copy of the rel's GpPolicy from the relcache. */
                relation = relation_open(reloid, NoLock);
                policy = RelationGetPartitioningKey(relation);
                relation_close(relation, NoLock);

                /* Tell caller if target rel is distributed. */
				if (policy &&
                    policy->ptype == POLICYTYPE_PARTITIONED)
					context->resultSegments = true;

                if (policy)
				    pfree(policy);

                /* RETURNING is not yet implemented for partitioned tables. */
                if (query->returningList &&
                    context->resultSegments)
                {
                    const char *cmd = (query->commandType == CMD_INSERT) ? "INSERT" 
                                    : (query->commandType == CMD_UPDATE) ? "UPDATE" 
                                    :                                      "DELETE";

                    ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
                                    errmsg("The RETURNING clause of the %s "
                                           "statement is not yet supported in "
                                           "this version of " PACKAGE_NAME ".",
                                           cmd) 
                                    ));
                }
			}
			break;

		default:
            Insist(0);
	}


	/* We need to keep track of whether any part of the plan needs to be
	 * dispatched in parallel. */
	context->dispatchParallel = context->resultSegments;
	
	/* Save the global count of PARAM_EXEC from the top plan node. */
	nParamExec = plan->nParamExec;

	/*
	 * Prescan the plan and attach Flow nodes to its Plan nodes. These nodes
	 * describe how to parallelize the plan.  We also accumulate summary
	 * information in the profile stucture.
	 */
	prescan(plan, context);
	
	if ( context->dispatchParallel )
	{
		const char *notyet="Greenplum Database does not yet support that query.";
		
		if ( uses_correlation )
		{
			ereport(ERROR, (
					errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					errmsg(notyet),
					errdetail("The query contains a correlated subquery.")
			));
		}
		else if ( context->hasNonRootSubplan )
		{
			ereport(ERROR, (
					errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					errmsg(notyet),
					errdetail("The query contains a multi-row subquery.")
			));
		}
		
		/* From this point on, since part of plan is parallel, we have to
		 * regard the whole plan as parallel. */
		plan->dispatch = DISPATCH_PARALLEL;

		/*
		 * Implement the parallelizing directions in the Flow nodes attached
		 * to the root plan node of each root slice of the plan.
		 */
		plan = apply_motion(plan, query);
	}


	/* Restore the global count of PARAM_EXEC from the top plan node. */
	plan->nParamExec = nParamExec;

	if (gp_enable_motion_deadlock_sanity)
		motion_sanity_check(plan);

	return plan;
}


/* ----------------------------------------------------------------------- *
 * Functions prescan() and prescan_walker() use the plan_tree_walker()
 * framework to visit the nodes of the plan tree in order to check for
 * unsupported usages and collect information for use in finalizing the
 * plan.  The walker exits with an error only due an unexpected problems.
 *
 * The plan and query arguments should not be null.
 * ----------------------------------------------------------------------- *
 */
void
prescan(Plan *plan, PlanProfile * context)
{
	if ( prescan_walker((Node *) plan, context) )
	{
		Insist(0);
	}
}

/*
 * Function prescan_walker is the workhorse of prescan.
 *
 * The driving function, prescan(), should be called only once on a
 * plan produced by the Greenplum Database optimizer for dispatch on the QD.  There
 * are two main task performed:
 *
 * 1. Since the optimizer isn't in a position to view plan globally,
 *    it may generate plans that we can't execute.  This function is
 *    responsible for detecting potential problem usages:
 *
 *    Example: Non-initplan subplans are legal in sequential contexts
 *    but not in parallel contexs.
 *
 * 2. The function specifies (by marking Flow nodes) what (if any)
 *    motions need to be applied to initplans prior to dispatch.
 *    As in the case of the main plan, the actual Motion node is
 *    attached later.
 */
bool
prescan_walker(Node *node, PlanProfile * context)
{
	if (node == NULL)
		return false;

    if (is_plan_node(node))
    {
        Plan   *plan = (Plan *)node;

        if (plan->dispatch == DISPATCH_PARALLEL)
            context->dispatchParallel = true;
    }

	switch (nodeTag(node))
	{
		case T_SubPlan:

			/*
			 * A SubPlan node is not a Plan node, but an Expr node that
			 * contains a Plan and its range table.  It corresponds to a
			 * SubLink in the query tree (a SQL subquery expression) and
			 * may occur in an expression or an initplan list.
			 */

			{
				/*
				 * SubPlans establish a local state for the contained plan,
				 * notably the range table.
				 */
				SubPlan    *subplan = (SubPlan *) node;

				/*
				 * Note subqueries that have not been pulled off into
				 * initPlans.  We're not ready for these yet.
				 */
				if ( subplan->is_multirow || ! subplan->is_initplan )
				{
					context->hasNonRootSubplan = TRUE;
				}

				/* Recur into the exprs list (as usual). */
				if (expression_tree_walker((Node *) subplan->testexpr, prescan_walker, context))
					return true;

				/*
				 * Set local state for the subplan to the new rangetable.
				 */
				pushLocals(context, subplan->rtable);

				/* Parallelize the subplan.  */
				if (prescan_walker((Node *) subplan->plan, context))
					return true;

                Assert(subplan->plan->flow &&
                       subplan->plan->flow->flotype != FLOW_UNDEFINED);

				/*
                 * Subquery result needs to be brought back to the QD.
                 * Ask for motion to a single QE.  Later, apply_motion
                 * will override that to bring it to the QD instead.
                 */
                Insist(focusPlan(subplan->plan, true, false, false));

				/* Restore state for the parent plan. */
				popLocals(context);

				/* Recur into the args list. */
				return expression_tree_walker((Node *) subplan->args, prescan_walker, context);
			}

		case T_Flow:
			/*
			 * Since this function is where Flow nodes are generated, we can't
			 * rely on this case to see them all.  Instead we handle them as
			 * they are created.
			 */
			return false;

		case T_Motion:
			context->dispatchParallel = TRUE;
			context->sliceDepth++;
			
			if (plan_tree_walker(node, prescan_walker, context))
				return true;
				
			context->sliceDepth--;
			return false;

		case T_FunctionScan:
        {
            /*
             * Disallow outer references in function scans, for now.
             *
             * NB: A Var node with varlevelsup==0 in the function expr can only
             * come from an outer reference in a subquery that was pulled up
             * into the current query level.
             *
             * CDB TODO: Build plans such that a function scan's required
             * inputs are produced upstream of the function invocation.
             */
            RangeTblEntry  *rte = rt_fetch(((Scan *)node)->scanrelid,
                                           localRangeTable(context));

            Assert(rte->rtekind == RTE_FUNCTION);

            if (rte->funcexpr &&
                IsA(rte->funcexpr, FuncExpr) &&
                contain_vars_of_level_or_above(rte->funcexpr, 0))
            {
			    ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
					            errmsg("Cannot parallelize that query yet."),
                                errdetail("In a subquery FROM clause, a "
                                          "function invocation cannot contain "
                                          "a correlated reference.")
					    ));
            }

			return plan_tree_walker(node, prescan_walker, context);
        }

		case T_SubqueryScan:

			/*
			 * A Scan is a source of tuples and thus occurs as a leaf of the
			 * plan tree.  Note, however, that scans may include SubPlan nodes
			 * (directly as in a Subquery scan or indirectly through a SubPlan
			 * node in an expresion) so they aren't necessarily a base case
			 * for recursion.
			 */
			{
				Plan	   *plan = (Plan *) node;
				Scan	   *scan = (Scan *) plan;
				Index		rtindex = scan->scanrelid;
				RangeTblEntry *rte;
				SubqueryScan *subqueryscan = (SubqueryScan *) node;
				Plan	   *subplan;

                /* Make sure we have a valid index. */
				Assert(0 < rtindex && rtindex <= list_length(localRangeTable(context)));

				/* All scan types have a range table entry. */
				rte = rt_fetch(rtindex, localRangeTable(context));

				/* The entry type is related to the to the scan type. */
				Assert(rte->rtekind == RTE_SUBQUERY);

				/*
				 * Parallelize a Subquery in the FROM clause.
				 *
				 * TODO Is there any special handling needed for
				 *		parameters?
				 */
				subplan = subqueryscan->subplan;

				/*
				 * The subplan uses the range table of the Query node
				 * attached to the subquery RTE. Also, the local context
				 * forces reexamination of the contained query.
				 */
				pushLocals(context, rte->subquery->rtable);
				if (prescan_walker((Node *) subplan, context))
					return true;
				popLocals(context);
				
				 return false;
			}
			break;				/* Fall through to report error. */

		default:
			return plan_tree_walker(node, prescan_walker, context);
	}
    /* not reached */
}


/*
 * Construct a new Flow in the current memory context.
 */
Flow *
makeFlow(FlowType flotype)
{
	Flow	   *flow = makeNode(Flow);

	flow->flotype = flotype;
	flow->req_move = MOVEMENT_NONE;
	flow->locustype = CdbLocusType_Null;

	return flow;
}


/*
 * Create a flow for the given Plan node based on the flow in its
 * lefttree (outer) plan.  Partitioning information is preserved.
 * Sort specifications are preserved only if withSort is true.
 *
 * NB: At one time this function was called during cdbparallelize(), after
 * transformation of the plan by set_plan_references().  Later, calls were
 * added upstream of set_plan_references(); only these remain at present.
 */
Flow *
pull_up_Flow(Plan *plan, Plan *subplan, bool withSort)
{
	Flow	   *model_flow;
	Flow	   *new_flow;

	Insist(subplan);

	if(plan->sliceNum > 0)
	{
		Assert(IsA(subplan, Motion));
		model_flow = get_motion_flow((Motion *) subplan, plan->sliceNum);
	}
	else
		model_flow = get_plan_flow(subplan);

	Insist(model_flow);

	new_flow = makeFlow(model_flow->flotype);

    if (model_flow->flotype == FLOW_SINGLETON)
        new_flow->segindex = model_flow->segindex;

    /* Pull up hash key exprs, if they're all present in the plan's result. */
    else if (model_flow->flotype == FLOW_PARTITIONED &&
             model_flow->hashExpr)
	{
		if (!is_projection_capable_plan(plan) ||
            cdbpullup_isExprCoveredByTargetlist((Expr *)model_flow->hashExpr,
                                                plan->targetlist))
            new_flow->hashExpr = copyObject(model_flow->hashExpr);
	}

	/* Pull up sort key info if requested. */
    if (withSort)
	{
        if (IsA(plan, Sort))
        {
	        Sort   *sort = (Sort *)plan;
       	
	        new_flow->numSortCols = sort->numCols;
	        ARRAYCOPY(new_flow->sortColIdx, sort->sortColIdx, sort->numCols);
	        ARRAYCOPY(new_flow->sortOperators, sort->sortOperators, sort->numCols);
        }
        else if (model_flow->numSortCols == 0)
            new_flow->numSortCols = 0;
        else if (is_projection_capable_plan(plan))
	    {
		    /*
             * Try to derive a sortColIdx to align with projected targetlist.
             * It might be shorter than the subplan's sortColIdx, maybe even
             * empty, if not all of the subplan's sort cols are projected.
             */
		    new_flow->numSortCols = cdbpullup_colIdx(plan,
                                                     subplan,
                                                     model_flow->numSortCols,
                                                     model_flow->sortColIdx,
                                                     &new_flow->sortColIdx);
		    if (new_flow->numSortCols > 0)
                ARRAYCOPY(new_flow->sortOperators,
                          model_flow->sortOperators,
                          new_flow->numSortCols);
	    }
	    else
	    {
		    /* Non-projecting so sort attributes are the same. */
            new_flow->numSortCols = model_flow->numSortCols;
		    ARRAYCOPY(new_flow->sortColIdx,
                      model_flow->sortColIdx,
                      new_flow->numSortCols);
            ARRAYCOPY(new_flow->sortOperators,
                      model_flow->sortOperators,
                      new_flow->numSortCols);
	    }
	}   /* withSort */

	new_flow->locustype = model_flow->locustype;
	
	return new_flow;
}


/*
 * Is the node a "subclass" of Plan?
 */
bool
is_plan_node(Node *node)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Result:
		case T_Append:
		case T_SeqScan:
		case T_ExternalScan:
		case T_IndexScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
        case T_ValuesScan:
		case T_AdaptiveNestLoop:
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
		case T_Material:
		case T_Sort:
		case T_Group:
		case T_Agg:
		case T_Unique:
		case T_Hash:
		case T_SetOp:
		case T_Limit:
		case T_Motion:
		case T_ShareInputScan:
		case T_Window:
			return true;

		default:
			break;
	}
	return false;
}


void
pushLocals(PlanProfile * profile, List *rtable)
{
    LocalProfile *newlocal = palloc(sizeof(LocalProfile));

	newlocal->rtable = rtable;
	newlocal->outerFrame = profile->local;

    profile->local = newlocal;
}

void
popLocals(PlanProfile * profile)
{
	LocalProfile *oldlocal = profile->local;

	if (oldlocal == NULL)
		return;

	profile->local = oldlocal->outerFrame;
	pfree(oldlocal);
}

List *
localRangeTable(PlanProfile * profile)
{
	if (profile->local == NULL)
		return NIL;
	return profile->local->rtable;
}

/* Functions focusPlan, broadcastPlan, and repartitionPlan annotate the top
 * of the given plan tree with a motion request for later implementation by
 * function apply_motion in cdbmutate.c.
 *
 * plan		   the plan to annotate
 *
 * live		   if false, don't actually make any change to the flow
 *			   specifications; just verify that they can be done.
 *
 * stable	   if true, the result plan must present the result in the
 *			   order specified in the flow (which, presumably, matches
 *			   the input order.
 *
 * rescannable	if true, the resulting plan must be rescannable, i.e.,
 *			   it must be possible to use it like a scan. (Note that
 *			   currently, no Motion nodes are rescannable.
 *
 * Remaining arguments, if any, determined by the requirements of the specific
 * function.
 *
 * The result is true if the requested annotation could be made (or was made
 * in case live == true).  Else, false.
 */

/*
 * Function: focusPlan
 */
bool
focusPlan(Plan *plan, bool live, bool stable, bool rescannable)
{
    Assert(plan->flow && plan->flow->flotype != FLOW_UNDEFINED);

    /* Already focused?  Do nothing. */
    if (plan->flow->flotype == FLOW_SINGLETON)
        return true;

    /* TODO How specify deep-six? */
    if (plan->flow->flotype == FLOW_REPLICATED)
        return false;		

    return adjustPlanFlow(plan, live, stable, rescannable, MOVEMENT_FOCUS, NIL);
}


/*
 * Function: broadcastPlan
 */
bool
broadcastPlan(Plan *plan, bool live, bool stable, bool rescannable)
{
    Assert(plan->flow && plan->flow->flotype != FLOW_UNDEFINED);

    /* Replicated -> Replicated is a no-op. */
    if (plan->flow->flotype == FLOW_REPLICATED)
        return true;

    return adjustPlanFlow(plan, live, stable, rescannable, MOVEMENT_BROADCAST, NIL);
}


/*
 * Function: repartitionPlan
 */
bool
repartitionPlan(Plan *plan, bool live, bool stable, bool rescannable, List *hashExpr)
{
    Movement    req_move = MOVEMENT_REPARTITION;

    Assert(plan->flow);
    Assert(plan->flow->flotype == FLOW_PARTITIONED ||
           plan->flow->flotype == FLOW_SINGLETON);

    /* Already partitioned on the given hashExpr?  Do nothing. */
    if (hashExpr &&
        equal(hashExpr, plan->flow->hashExpr))
        return true;

    return adjustPlanFlow(plan, live, stable, rescannable, req_move, hashExpr);
}


/*
 * Helper Function: adjustPlanFlow
 *
 * Recursively pushes a motion request down the plan tree which allows us
 * to remove contraints in some cases.	For example, pushing a motion below
 * a Sort removes the stability constraint.
 *
 * Returns true if successful.  Returns false and doesn't change anything
 * if the request cannot be honored.
 */
bool
adjustPlanFlow(Plan        *plan,
               bool         live,
               bool         stable,
               bool         rescannable,
               Movement     req_move,
               List        *hashExpr)
{
    Flow       *flow = plan->flow;
    bool        disorder = false;
    bool		recur = false;
    bool        reorder = false;

    Assert(flow && flow->flotype != FLOW_UNDEFINED);

    /*
     * If the Flow was already adjusted due to an earlier motion request,
     * the original hash/sort key info might have been altered so that we
     * no longer have a proper description of the flow from the Plan node
     * into the proposed Motion operator.  Refuse further adjustments.
     */
    if (flow->req_move != MOVEMENT_NONE)
        return false;

    switch (req_move)
	{
        case MOVEMENT_FOCUS:
            /* Don't request Merge Receive unless ordering is significant. */
            if (!stable)
                disorder = true;
            break;

		case MOVEMENT_BROADCAST:
        case MOVEMENT_REPARTITION:
            if (flow->flotype == FLOW_PARTITIONED)
                disorder = true;
            break;

		default:
            break;
    }

    /*
	 * Decide whether to push the request further down the plan tree and
	 * possibly remove restrictions.
	 */
	switch (plan->type)
	{
		case T_Sort:
            if (!stable && live)
            {
                /*
                 * Q: If order doesn't matter, why is there a Sort here?
                 * A: Could be INSERT...SELECT...ORDER BY; each QE in the
                 * inserting gang will sort its own partition locally.  Weird,
                 * but could be used for clustering.  But note it in the debug
                 * log; some caller might specify stable = false by mistake.
                 */
                ereport(DEBUG4, (errmsg("adjustPlanFlow stable=false applied "
                                        "to Sort operator; req_move=%d",
                                        req_move) ));
            }

			if (req_move == MOVEMENT_FOCUS)
			{
				/*
				 * We'd rather do a merge of a parallel sort, than do a
				 * sequential sort. A fixed motion can do that as long as we
				 * don't need to rescan.
				 */
				break;
			}
			else
			{
				/* No need to preserve order or restartability below a sort. */
				recur = true;
                reorder = true;
                rescannable = false;
			}
			break;

		case T_Material:
			/* Always push motion below Material. */
			/* No need to preserve restartability below a materialization. */
			recur = true;
			rescannable = false;
			break;

		case T_Append:			/* Maybe handle specially some day. */
		default:
			break;
	}

    /*
     * Push motion down to a descendant of this node (= upstream).
     */
    if (recur)
	{
        Flow   *kidflow;

        Assert(plan->lefttree &&
               plan->lefttree->flow &&
               plan->lefttree->flow->flotype != FLOW_UNDEFINED);

        /* Descend to apply the requested Motion somewhere below this node. */
        if (!adjustPlanFlow(plan->lefttree,
                            live,
                            stable && !reorder,
                            rescannable,
                            req_move,
                            hashExpr))
            return false;

        /* Return if just window shopping.  Nothing has been changed yet. */
        if (!live)
            return true;        /* success; call me again to do it for real */

        /* After updating subplan, bubble new distribution back up the tree. */
        kidflow = plan->lefttree->flow;
        flow->flotype = kidflow->flotype;
        flow->segindex = kidflow->segindex;
        flow->hashExpr = copyObject(kidflow->hashExpr);
		plan->dispatch = plan->lefttree->dispatch;

        /* Zap sort cols if motion has destroyed the ordering. */
        if (disorder && !reorder)
        {
            flow->numSortCols = 0;
            flow->sortColIdx = NULL;
            flow->sortOperators = NULL;
        }

		return true;  /* success */
	}

	/*
     * The buck stops here.  We have arrived at the node whose output is
     * to be moved.  Return false if can't do the motion as requested.
     */

    /* Would motion mess up a required ordering? */
    if (stable && disorder)
        return false;	

    /* Adding a Motion node would make the result non-rescannable. */
    if (rescannable)
        return false;

    /*
     * Ok, we have decided it is alright to add Motion above this node.
     * If just a dry run, return success.  Nothing has been changed.
     */
    if (!live)
        return true;            /* success; call me again to do it for real */

    /*
     * Now we begin the transformation, if 'live'.
     *
     * Make our changes to a shallow copy of the original Flow node.
     * Later, after applying the requested motion, apply_motion_mutator()
     * will restore the original Flow ptr and discard the modified copy.
     *
     * Note that the original Flow node describes the present Plan node's
     * own result distribution and ordering: i.e. the input to the future
     * Motion.   The temporary modified Flow describes the result of the
     * requested Motion: the input to the next operator downstream.
     */
    if (!flow->flow_before_req_move)
    {
        plan->flow = (Flow *)palloc(sizeof(*plan->flow));
        *plan->flow = *flow;
        plan->flow->flow_before_req_move = flow;
        flow = plan->flow;
    }

    /*
     * Set flag to tell apply_motion() to insert a Motion node
     * immediately above this node (= downstream).  Overwrite the
     * present Plan node's Flow node to describe the distribution
     * and ordering that the future Motion node will produce.
     */
    flow->req_move = req_move;

    switch (req_move)
    {
        case MOVEMENT_FOCUS:
            /* Converge to a single QE (or QD; that choice is made later). */
            flow->flotype = FLOW_SINGLETON;
            flow->hashExpr = NIL;
            flow->segindex = 0;
            break;

        case MOVEMENT_BROADCAST:
            flow->flotype = FLOW_REPLICATED;
            flow->hashExpr = NIL;
            flow->segindex = 0;
            break;

        case MOVEMENT_REPARTITION:
            flow->flotype = FLOW_PARTITIONED;
            flow->hashExpr = copyObject(hashExpr);
            flow->segindex = 0;
            break;

        default:
            Assert(0);
    }

    /* Discard ordering info if the motion will disturb the order. */
    if (disorder)
    {
        flow->numSortCols = 0;
        flow->sortColIdx = NULL;
        flow->sortOperators = NULL;
    }
	
	/* Since we added Motion, dispatch must be parallel. */
	plan->dispatch = DISPATCH_PARALLEL;

    return true;                /* success */
}                               /* adjustPlanFlow */

#define SANITY_MOTION 0x1
#define SANITY_DEADLOCK 0x2

static bool
motion_sanity_walker(Node *node, int *result)
{
	int left_result=0;
	int right_result=0;
	bool deadlock_safe = false;
	char *branch_label;

	if (node == NULL)
		return false;

	if (!is_plan_node(node))
		return false;

	/* special handling for branch points */
	switch (nodeTag(node))
	{
		case T_HashJoin: /* Hash join can't deadlock -- it fully
						  * materializes its inner before switching to
						  * its outer. */
			branch_label = "HJ";
			deadlock_safe = true;
			break;
		case T_AdaptiveNestLoop:
			branch_label = "ANL";
			break;
		case T_NestLoop: /* Nested loop joins are safe only if the
						  * prefetch flag is set */
			branch_label = "NL";
			if (((NestLoop *)node)->join.prefetch_inner)
				deadlock_safe = true;
			break;
		case T_MergeJoin:
			branch_label = "MJ";
			break;
		default:
			branch_label = NULL;
			break;
	}

	/* now scan the subplans */
	switch (nodeTag(node))
	{
		case T_Result:
		case T_Append:
		case T_SeqScan:
		case T_ExternalScan:
		case T_IndexScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_Group:
		case T_Agg:
		case T_Unique:
		case T_Hash:
		case T_SetOp:
		case T_Limit:
			if (plan_tree_walker(node, motion_sanity_walker, result))
				return true;
			break;

		case T_Motion:
			if (plan_tree_walker(node, motion_sanity_walker, result))
				return true;
			*result |= SANITY_MOTION;
			elog(DEBUG5, "   found motion");
			break;

		case T_Sort:
			if (plan_tree_walker(node, motion_sanity_walker, result))
				return true;

			/* sort internally materializes */
			*result &= ~SANITY_MOTION;
			break;

		case T_Material:
			/* discover any deadlocks below */
			if (plan_tree_walker(node, motion_sanity_walker, result))
				return true;
			break;

		case T_HashJoin:
		case T_AdaptiveNestLoop:
		case T_NestLoop:
		case T_MergeJoin:
		{
			Plan *plan = (Plan *)node;

			elog(DEBUG5, "    %s going left", branch_label);
			if (motion_sanity_walker((Node *)plan->lefttree, &left_result))
				return true;
			elog(DEBUG5, "    %s going right", branch_label);
			if (motion_sanity_walker((Node *)plan->righttree, &right_result))
				return true;

			elog(DEBUG5, "    %s branch point left 0x%x right 0x%x",
				 branch_label, left_result, right_result);

			/* deadlocks get sent up immediately */
			if ((left_result & SANITY_DEADLOCK) ||
				(right_result & SANITY_DEADLOCK))
			{
				*result |= SANITY_DEADLOCK;
				break;
			}
			/* if this node is "deadlock safe" then even if we have
			 * motion on both sides we will not deadlock (because the
			 * access pattern is deadlock safe: all rows are retrieved
			 * from one side before the first row from the other). */
			if (!deadlock_safe && ((left_result & SANITY_MOTION) &&
								   (right_result & SANITY_MOTION)))
			{
				elog(LOG, "FOUND MOTION DEADLOCK in %s", branch_label);
				*result |= SANITY_DEADLOCK;
				break;
			}

			*result |= left_result | right_result;

			elog(DEBUG5, "    %s branch point left 0x%x right 0x%x res 0x%x",
				 branch_label, left_result, right_result, *result);
		}
		break;
		default:
			break;
	}
	return false;
}

static void
motion_sanity_check(Plan *plan)
{
	int sanity_result=0;

	elog(DEBUG5, "Motion Deadlock Sanity Check");

	if (motion_sanity_walker((Node *)plan, &sanity_result))
	{
		Insist(0);
	}

	if (sanity_result & SANITY_DEADLOCK)
		elog(ERROR, "Post-planning sanity check detected motion deadlock.");
}
