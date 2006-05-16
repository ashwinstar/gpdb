/*-------------------------------------------------------------------------
 *
 * cdbllize.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2005-2006, greenplum inc
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
	bool		finished;		/* Are we inside a finished (by the planner)
								 * branch? */

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
	bool		resultSegments;

    /*
     * True if set_plan_references() has been called and Var nodes have been
     * adjusted to refer to items in the subplan's targetlist.  False before
     * the end of optimization, when Var nodes refer to RTE items.
     */
    bool        useExecutorVarFormat;

	/* Context */
	LocalProfile *local;

	/* Message data */
	unsigned	code;
	const char *message;
	const char *detail;

}	PlanProfile;



/*
 * Forward Declarations
 */
static bool prescan(Plan *plan, PlanProfile * context);
static bool prescan_walker(Node *node, PlanProfile * context);

static void psError(PlanProfile * context, unsigned code, const char *message, const char *detail);
static void psErrorNonce(PlanProfile * context, const char *detail);
static void issuePsError(PlanProfile * context);

static void initPlanProfile(PlanProfile * context, Query *query, bool useExecutorVarFormat);
static void pushLocals(PlanProfile * profile, List *rtable);
static void popLocals(PlanProfile * profile);
static List *localRangeTable(PlanProfile * profile);
static bool isPlanFinished(Node *node, PlanProfile * profile);

static bool scanForManagedTables_walker(Node *node, PlanProfile * context);
static bool scanForManagedTables(Plan *plan, PlanProfile *profile);

static bool
adjustPlanFlow(Plan        *plan,
               bool         live,
               bool         stable,
               bool         rescannable,
               Movement     req_move,
               List        *hashExpr);

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
 * plan suitable for execution by MPP.
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
	PlanProfile profile;		
	int			nParamExec;
	
	if ( Mpp_role != MPP_ROLE_DISPATCH )
	{
		return plan;
	}

	Assert(is_plan_node((Node *) plan));
	/*
     * Print plan if debugging.
     */
	if (Debug_print_prelim_plan)
		elog_node_display(DEBUG1, "preliminary plan", plan, Debug_pretty_print);


	Assert(query != NULL && IsA(query, Query));

	/* If the plan is already marked sequential, just return it. */
	if (plan->dispatch == DISPATCH_SEQUENTIAL)
		return plan;

    /* Initialize the PlanProfile result. */
    initPlanProfile(&profile, query, true);

	/*
	 * Do a quick walk of the plan to see whether it references any managed
	 * tables.	If not, we don't need to parallelize it.
	 */
	if (!scanForManagedTables(plan, &profile))
	{
		plan->dispatch = DISPATCH_SEQUENTIAL;
		return plan;
	}
	
	/*
	 * The plan refers to managed tables, so we can't allow it to use
	 * correlation.  (This may be more severe that necessary, however,
	 * we haven't done the work to discover "safe" uses, so we just
	 * disallow them all.)
	 */
	if ( uses_correlation )
	{
		ereport(ERROR, (
				errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
				errmsg("Correlated subqueries not yet supported.")
		));
	}

	/* Save the global count of PARAM_EXEC from the top plan node. */
	nParamExec = plan->nParamExec;

	/*
	 * Prescan the plan and attach Flow nodes to its Plan nodes. These nodes
	 * describe how to parallelize the plan.  We also accumulate summary
	 * information in the profile stucture.
	 */
	if (!prescan(plan, &profile))
		issuePsError(&profile);

	/*
	 * Implement the parallelizing directions in the attached flows.
	 */
	plan = apply_motion(plan, query);
	plan->dispatch = DISPATCH_PARALLEL;

	/* Restore the global count of PARAM_EXEC from the top plan node. */
	plan->nParamExec = nParamExec;

	return plan;
}


/* ----------------------------------------------------------------------- *
 * Functions prescan() and prescan_walker() use the plan_tree_walker()
 * framework to visit the nodes of a plan tree to be parallelized and
 * add flow information to the leaves for later use.  The walker may
 * also choose to exit with an error, usually to report an unsupported
 * feature used in the plan tree or an unexpected problem.
 *
 * The plan and query arguments should not be null.
 *
 * Returns true if the scan results in a parallelization strategy or
 * no parallelization is required;	false otherwise.
 *
 * In case the result is false, the caller should issue an error.
 * ----------------------------------------------------------------------- *
 */
bool
prescan(Plan *plan, PlanProfile * context)
{
	return !prescan_walker((Node *) plan, context);
}


/*
 * Function prescan_walker is the workhorse of prescan.
 *
 * The driving function, prescan(), should be called only once on the
 * sequential plan produced by the the Postgres optimizer.	In particular,
 * this function assumes it will see a tree will no Flow nodes or Motion
 * nodes in it.  It will add Flow nodes which, in turn, will specify
 * where Motion nodes should go.
 *
 * The general pattern for assigning flows to plan nodes is to examine
 * leaf nodes (scans) and assign them a flow prior to descending into
 * children (expressions), and to descend into children of interior
 * nodes (in order to assign their children's flows) before examining
 * them and assigning their flows.
 *
 * This means that child (lefttree, righttree, etc)  Plan nodes of the
 * current Plan node (the one under investigation) will have had their
 * Flow nodes assigned already while the current Plan node will not.
 * Note, however, that the child plans may have their Flow nodes adjusted,
 * to meet the requirements of the current node.
 */
bool
prescan_walker(Node *node, PlanProfile * context)
{
	bool		finished;
	bool		parent_finished;
	const char *detail = NULL;

	if (node == NULL)
		return false;

	parent_finished = context->local->finished;
	finished = parent_finished || isPlanFinished(node, context);

	switch (nodeTag(node))
	{
		case T_SubPlan:

			/*
			 * A SubPlan node is not a Plan node, but an Expr node that
			 * contains a Plan and its range table.  It corresponds to a
			 * SubLink in the query tree (a SQL subquery expression) or a plan
			 * in an initplan list.
			 *
			 * TODO Add support for multirow and correlated subqueries. Will this
			 * require additional context to distinguish "in an initPlan" from
			 * "in an ordinary expression"?
			 */
			{
				/*
				 * SubPlans establish a local state for the contained plan,
				 * notably the range table.
				 */
				SubPlan    *subplan = (SubPlan *) node;

				/* Nip processing of correlated subquery in the bud. */
				if (subplan->parParam != NIL || subplan->args != NIL)
				{
					detail = "Query involves a correlated subquery.";
					break;		/* Fall thru to error. */
				}

				/*
				 * We're not ready to dispatch multirow subqueries (which
				 * would be embeded in plans) yet, so cut them off.
				 *
				 * TODO Verify that noncorrelated,	single row subqueries will be
				 * pull off into initPlans, which is all we're ready for.
				 */
				if (!(subplan->subLinkType == EXISTS_SUBLINK
					  || subplan->subLinkType == MULTIEXPR_SUBLINK
					  || subplan->subLinkType == EXPR_SUBLINK)
					)
				{
					detail = "Query involves multi-row subquery expression.";
					break;		/* Fall thru to error. */
				}

				/* Recur into the exprs list (as usual). */
				if (expression_tree_walker((Node *) subplan->exprs, prescan_walker, context))
					goto return_true;

				/*
				 * Set local state for the subplan.  This has two effects. The
				 * local state now has the new rangetable.	Also, the
				 * "finished" indicator is cleared, so that when we recur to
				 * parallelize the plan in the SubPlan expression, we will
				 * make a fresh determination of how to interpret the plan: as
				 * sequential or as already parallel (down to the next level
				 * of SubPlan).
				 */
				pushLocals(context, subplan->rtable);

				/* Parallelize the subplan.  */
				if (prescan_walker((Node *) subplan->plan, context))
					goto return_true;

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
				if (expression_tree_walker((Node *) subplan->args, prescan_walker, context))
					goto return_true;
			}
			goto return_false;

		case T_Flow:

			/*
			 * Since this function is where Flow nodes are generated, we can't
			 * rely on this case to see them all.  Instead we handle them as
			 * they are created.
			 */
			goto return_false;

		case T_SeqScan:
		case T_IndexScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_FunctionScan:
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
		case T_Hash:
		case T_Motion:

			/*
			 * Node is part of a finished plan produced by the planner. Keep
			 * walking in search of subqueries with unfinished plans.
			 */
			Assert(finished);
			if (plan_tree_walker(node, prescan_walker, context))
				goto return_true;
			goto return_false;

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
				 * atttached to the subquery RTE. Also, the local context
				 * forces reexamination of the contained query.
				 */
				pushLocals(context, rte->subquery->rtable);
				if (prescan_walker((Node *) subplan, context))
					goto return_true;
				popLocals(context);

				Assert(finished);
				
				goto return_false;
			}
			break;				/* Fall through to report error. */


		case T_Result:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_Append:
		case T_Agg:
		case T_Group:
		case T_Limit:
			/* First prescan children */
			if (plan_tree_walker(node, prescan_walker, context))
				goto return_true;
			
			Assert(finished); 
			goto return_false;

		default:
			/* Let plan_tree_walker() deal with other node types. */
			if (plan_tree_walker(node, prescan_walker, context))
				goto return_true;
			goto return_false;

	}

	/*
	 * Breaking out of the above switch will setup an error report with
	 * whatever detail was set above.
	 */
	psErrorNonce(context, detail);

return_true:
	context->local->finished = parent_finished;
	return true;
return_false:
	context->local->finished = parent_finished;
	return false;
}


/*
 * Helper for prescan() -- Write error info into a PlanProfile.
 */
void
psError(PlanProfile * context,
		unsigned code,
		const char *message,
		const char *detail)
{
	context->code = code;
	context->message = message;
	context->detail = detail;
}


/*
 * Helper for prescan() -- Write error info into a PlanProfile.
 */
void
psErrorNonce(PlanProfile * context, const char *detail)
{
	context->code = ERRCODE_CDB_FEATURE_NOT_YET;
	context->message = "Cannot parallelize that query yet.";
	context->detail = detail;
}

/*
 * Helper for prescan() -- Issue the error specified in a PlanProfile.
 */
void
issuePsError(PlanProfile * context)
{
	if (context->detail != NULL)
	{
		ereport(ERROR, (
						errcode(context->code),
						errmsg(context->message),
						errdetail(context->detail)
						));
	}
	else
	{
		ereport(ERROR, (
						errcode(context->code),
						errmsg(context->message)
						));
	}
	Insist(0);				/* Never return */
}


/*
 * Construct a new Flow in the current memory context.
 */
Flow *
makeFlow(FlowType flotype)
{
	Flow	   *flow = makeNode(Flow);

	flow->flotype = flotype;
    flow->base_flotype = FLOW_UNDEFINED;
	flow->req_move = MOVEMENT_NONE;
	flow->locustype = CdbLocusType_Null;

	return flow;
}


/*
 * Create a flow for the given Plan node based on the flow in it's
 * lefttree (outer) plan.  Partitioning information is preserved.
 * Sort specifications are preserved only if withSort is true.
 */
Flow *
pull_up_Flow(Plan *plan, Plan *subplan, bool withSort)
{
	Flow	   *model_flow;
	Flow	   *new_flow;

	Assert(subplan && subplan->flow);

	model_flow = subplan->flow;

	new_flow = makeFlow(model_flow->flotype);

    if (model_flow->flotype == FLOW_SINGLETON)
        new_flow->segindex = model_flow->segindex;

    /* Pull up hash key exprs, if they're all present in the plan's result. */
    else if (model_flow->flotype == FLOW_PARTITIONED &&
             model_flow->hashExpr)
	{
		if (is_projection_capable_plan(plan))
            new_flow->hashExpr = (List *)cdbpullup_expr((Expr *)model_flow->hashExpr,
                                                        plan->targetlist,
                                                        OUTER);
        else
            new_flow->hashExpr = copyObject(model_flow->hashExpr);
	}

	/* Pull up sort key info if requested. */
    if (withSort && model_flow->numSortCols > 0)
	{
		if (is_projection_capable_plan(plan))
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
		}
		else
		{
			/* Non-projecting so sort attributes are the same. */
            new_flow->numSortCols = model_flow->numSortCols;
			ARRAYCOPY(new_flow->sortColIdx,
                      model_flow->sortColIdx,
                      new_flow->numSortCols);
		}

		if (new_flow->numSortCols > 0)
            ARRAYCOPY(new_flow->sortOperators,
                      model_flow->sortOperators,
                      new_flow->numSortCols);
	}

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
		case T_IndexScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
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
			return true;

		default:
			break;
	}
	return false;
}


/* ----------------------------------------------------------------------- *
 * Functions scanForManagedTables() and scanForManagedTables_walker()
 * use the plan_tree_walk() framework to visit the nodes of a plan tree
 * to see if there are any managed tables referenced in any of the
 * plan scan nodes.
 *
 * The plan and query arguments should not be null.
 *
 * Returns true if the there exists at least one managed table referenced
 * by the Plan, false otherwise.
 *
 * If an error is encountered, an ereport is issued directly.
 *
 * The purpose of scanForManagedTables is to do a quick scan to see whether
 * there are no managed tables, in which case we can skip the parallelization
 * step.  Without this, we may generate errors during  parallelization that
 * will erroneously prevent catalog queries from running.
 * ----------------------------------------------------------------------- *
 */
bool
scanForManagedTables(Plan *plan, PlanProfile *profile)
{
	/* initPlanProfile may have already found a managed table. */
	if (profile->resultSegments)
		return true;

	return scanForManagedTables_walker((Node *) plan, profile);
}


/*
 * Function scanForManagedTables_walker() is the recursive core of
 * scanForManagedTables(). It is mainly interested in SeqScans, TidScans,
 * and Index Scans since these are the nodes that reference base relations.
 * For these, it
 * - gets the relid of the scan,
 * - gets the RangeTableEntry from the context rtable,
 * - gets the oid of the relation in the scan,
 * - calls get_table_policy to see whether this oid represents a
 *	 managed table.
 *
 * If so, we can exit the walk by returning true, otherwise we use the
 * plan_walker framework to walk the appropriate child nodes.
 *
 * The other node type of interest SubPlan, since this changes the current
 * range table.
 *
 * For nodes other than these, we just use plan_walker to walk the appropriate
 * child nodes.
 *
 * This walker uses the same PlanProfile structure as prescan_walker().
 * The only information we use from context PlanProfile is the current
 * range table.
 *
 */
bool
scanForManagedTables_walker(Node *node, PlanProfile * context)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Motion:
			return true;

		case T_SeqScan:
		case T_TidScan:
		case T_IndexScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
			{
				Plan	   *plan = (Plan *) node;

				if (plan->dispatch == DISPATCH_PARALLEL)
					return true;
			}
			return false;

		case T_SubqueryScan:
			{
				/*
				 * SubqueryScans have their own range table.  (See
				 * ExecInitSubqueryScan for details.) We save the current
				 * range table, substitute the SubqueryScan's range table in
				 * the context before we descend into the fields of the
				 * SubqueryScan and then put the original one back.
				 */
                RangeTblEntry  *rte;
				Scan	       *scan = (Scan *)node;
				Index		    rtindex = scan->scanrelid;
                bool            done;

				rte = rt_fetch(rtindex, localRangeTable(context));
				Assert(rte->rtekind == RTE_SUBQUERY);

				pushLocals(context, rte->subquery->rtable);
				done = plan_tree_walker(node, scanForManagedTables_walker, context);
				popLocals(context);

                return done;
			}

		case T_SubPlan:
			{
				/*
				 * SubPlans establish a local state for the contained plan,
				 * notably the range table.
				 */
				SubPlan    *subplan = (SubPlan *) node;
                bool        done;

				/* Recur into the exprs list (as usual). */
				if (expression_tree_walker((Node *) subplan->exprs, scanForManagedTables_walker, context))
					return true;

				/* Walk the subplan. */
                pushLocals(context, subplan->rtable);
				done = scanForManagedTables_walker((Node *) subplan->plan, context);
				popLocals(context);

                if (done)
                    return true;

                /* Recur into the args list. */
				if (expression_tree_walker((Node *) subplan->args, scanForManagedTables_walker, context))
					return true;
			}
			return false;

		default:
			/* Let plan_tree_walker() deal with other node types. */
			break;

	}

	return plan_tree_walker(node, scanForManagedTables_walker, context);
}

/*
 * initPlanProfile
 * initializes the PlanProfile struct fields
 * and checks to see whether there is a result relation
 * (for an insert, update, or delete statement)
 * and if so, includes it in the summary of relations touched.
 */
void
initPlanProfile(PlanProfile * context, Query *query, bool useExecutorVarFormat)
{
	Assert(context != NULL);

	context->resultSegments = false;
    context->useExecutorVarFormat = useExecutorVarFormat;

	/* Initialize the current range table. */
	context->local = NULL;
	pushLocals(context, query->rtable);

	switch (query->commandType)
	{
		case CMD_SELECT:
			break;

		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			{
				/*
				 * Since this is a data modification command, include the
				 * result relation in the summary of relations touched.
				 */
				Oid			reloid;
                Relation    relation;
				MppPolicy  *policy = NULL;

                /* Get Oid of target relation. */
                Insist(query->resultRelation > 0 &&
				       query->resultRelation <= list_length(localRangeTable(context)));
				reloid = getrelid(query->resultRelation, localRangeTable(context));

                /* Get a copy of the rel's MppPolicy from the relcache. */
                relation = relation_open(reloid, NoLock);
                policy = RelationGetPartitioningKey(relation);
                relation_close(relation, NoLock);

                /* Tell caller if target rel is distributed. */
				if (policy &&
                    policy->ptype == POLICYTYPE_PARTITIONED)
					context->resultSegments = true;

                if (policy)
				    pfree(policy);	
			}
			break;

		default:
            Insist(0);
	}

	/*
	 * Initialize the error fields of the context, in case some failure
	 * in the prescan neglects to set them.
	 */
	psError(context, ERRCODE_INTERNAL_ERROR,
			"Unexpected failure parallelizing query.", NULL);
}

void
pushLocals(PlanProfile * profile, List *rtable)
{
    LocalProfile *newlocal = palloc(sizeof(LocalProfile));

	newlocal->rtable = rtable;
	newlocal->finished = false;
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

/* Has the plan in context at the given node been parallelized yet?
 * The node argument is only interesting if it's a Plan node and the
 * the context so far indicated "unfinished" in which case we check
 * for evidence of "finished" and update the context:
 *
 * - If the node has a defined Flow,  the branch must be finished.
 */
bool
isPlanFinished(Node *node, PlanProfile * profile)
{
	if (is_plan_node(node) && !profile->local->finished)
	{
		Plan	   *plan = (Plan *) node;

		profile->local->finished = plan->flow != NULL &&
                                   plan->flow->flotype != FLOW_UNDEFINED;
	}
	return profile->local->finished;
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

    /* Tuples cannot be sent from QD to QEs via Motion nodes (for now). */
    if (flow->flotype == FLOW_SINGLETON &&
        flow->segindex < 0)
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

    /*---------------
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

    /*---------------
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

        /* Zap sort cols if motion has destroyed the ordering. */
        if (disorder && !reorder)
        {
            flow->numSortCols = 0;
            flow->sortColIdx = NULL;
            flow->sortOperators = NULL;
        }

		return true;  /* success */
	}

	/*---------------
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
     * Set flag to tell apply_motion() to insert a Motion node
     * immediately above this node (= downstream).  Overwrite the
     * present Plan node's Flow node to describe the distribution
     * and ordering that the future Motion node will produce.
     *
     * On return there is no longer any Flow node that specifically
     * describes the present Plan node's own result distribution and
     * ordering, i.e. the input to the future Motion.
     */
    flow->req_move = req_move;
    if (flow->base_flotype == FLOW_UNDEFINED)
        flow->base_flotype = flow->flotype;

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

    return true;                /* success */
}                               /* adjustPlanFlow */


