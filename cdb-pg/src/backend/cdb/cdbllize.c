/*-------------------------------------------------------------------------
 *
 * cdbllize.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"	/* makeVar() */
#include "nodes/print.h"        /* elog_node_display() */
#include "optimizer/planmain.h" /* is_projection_capable_plan() */
#include "optimizer/var.h"      /* contain_vars_of_level_or_above() */
#include "parser/parse_expr.h"	/* expr_type() */
#include "parser/parsetree.h"	/* rt_fetch(), get_tle_by_resno() */
#include "tcop/utility.h"       /* CreateCommandTag() */
#include "utils/lsyscache.h"	/* get_rel_name(), get_relid_attribute_name() */
#include "utils/relcache.h"     /* RelationIsDistributed() */

#include "cdb/cdbfts.h"         /* isFTSEnabled() */
#include "cdb/cdbhash.h"        /* isGreenplumDbHashable() */
#include "cdb/cdbllize.h"       /* me */
#include "cdb/cdbmutate.h"      /* apply_motion() */
#include "cdb/cdbplan.h"        /* plan_tree_walker() */
#include "cdb/cdbpullup.h"      /* cdbpullup_makeVar() */
#include "cdb/cdbvars.h"        /* Gp_role, GUCs */

#define ARRAYCOPY(to, from, sz) \
	do { \
		Size	_size = (sz) * sizeof((to)[0]); \
		(to) = palloc(_size); \
		memcpy((to), (from), _size); \
	} while (0)


/*
 * A PlanProfile holds state for recursive prescan_walker().
 */
typedef struct PlanProfile
{
    PlannedStmt    *plannedStmt;
    CdbTopPlan     *topPlan;
}	PlanProfile;


/*
 * Forward Declarations
 */
static void
bring_result_to_qd(Plan *plan, Query *query);
static bool
final_motion_for_insert(PlannedStmt *plannedStmt);
static bool
final_motion_for_select(PlannedStmt *plannedStmt, Query *query);
static bool
final_motion_for_upd_del(PlannedStmt *plannedStmt);
static List *
getExprListFromTargetList(List         *tlist,
                          int           numCols,
                          AttrNumber   *colIdx);
static bool 
has_correlation_params(List *planner_param_items);
static GpPolicy *
select_into_default_policy(Plan *plan);

static void 
prescan(PlannedStmt *plannedStmt, CdbTopPlan *topPlan, Plan *plan, PlanProfile *context);
static bool 
prescan_walker(Node *node, PlanProfile * context);

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
 * The result is [a copy of] the input plan with alterations to make the
 * plan suitable for execution by Greenplum Database.
 *
 * Outline of processing:
 *
 * - If the argument Plan produces a partitioned flow, a Motion appropriate 
 *   to the command is added to the top. For example, to consolidate the 
 *   tuple streams from a SELECT command into a single tuple stream on the
 *   dispatcher.
 *
 * - Scan the input plan looking for references to distributed tables.  
 *   If none are found, mark the plan for sequential dispatch and return it.
 *
 * - Implement the decisions recorded in the Flow nodes (apply_motion)
 *	 to produce a modified copy of the plan.  Mark the copy for parallel
 *	 dispatch and return it.
 * ------------------------------------------------------------------------- *
 */
struct Plan *
cdbparallelize(struct PlannerGlobal    *glob,
               struct PlannedStmt      *plannedStmt,
               struct Query            *query)
{
    Plan           *plan = plannedStmt->planTree;
	PlanProfile     profile;
	PlanProfile    *context = &profile;
    ListCell       *cell;
    bool            isTargetDistributed = false;
    int             itop;
		
	/* Make sure we're called correctly (and need to be called).  */
	switch ( Gp_role )
	{
	case GP_ROLE_DISPATCH:
	case GP_ROLE_DISPATCHAGENT:
		break;
	case GP_ROLE_UTILITY:
		return plan;
	case GP_ROLE_EXECUTE:
	case GP_ROLE_UNDEFINED:
		Insist(0);
	}
	Assert(is_plan_node((Node *) plan));
	
	/* Print plan if debugging. */
	if (Debug_print_prelim_plan)
		elog_node_display(DEBUG1, "preliminary plan", plan, Debug_pretty_print);

	/* Don't parallelize plans rendered impotent be constraint exclusion.
	 * See MPP-2168.  We could fold this into prescan() but this quick
	 * check is effective in the only case we know of.
	 */
	if ( IsA(plan, Result) && plan->lefttree == NULL
						   && plan->targetlist == NULL
						   && plan->qual == NULL
						   && plan->initPlan == NULL )
		return plan;

    /* Initialize the PlanProfile result ... */
    memset(context, 0, sizeof(*context));

    /*
     * Does a final Motion operator need to be added on top of the plan?
     */
    switch (plannedStmt->commandType)
	{
		case CMD_SELECT:
            isTargetDistributed = final_motion_for_select(plannedStmt, query);
			break;

		case CMD_INSERT:
            isTargetDistributed = final_motion_for_insert(plannedStmt);
			break;

		case CMD_UPDATE:
		case CMD_DELETE:
            isTargetDistributed = final_motion_for_upd_del(plannedStmt);
            break;
        
        default:
            Insist(0);
	}

    /* RETURNING is not yet implemented for distributed tables. */
    if (isTargetDistributed &&
        plannedStmt->returningLists)
    {
        ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
                        errmsg("The RETURNING clause of the %s "
                               "statement is not yet supported in "
                               "this version of " PACKAGE_NAME ".",
                               CreateCommandTag((Node *)plannedStmt))
                        ));
    }

    /*
     * Examine the main query plan and all subquery plans to see if
     * they need to be dispatched to qExec processes for parallel
     * execution.  Check whether they request functionality for which
     * parallel execution is not yet supported.
     *
     * This sets the 'dispatchParallel' flags in the PlannedStmt and
     * CdbTopPlan nodes.
     */
    itop = 0;
	prescan(plannedStmt, plannedStmt->topPlans[itop++], plan, context);

    foreach(cell, plannedStmt->subplans)
    {
        Plan   *subqueryplan = (Plan *)lfirst(cell);

        /*
         * Subquery result needs to be brought back to the QD.  Modify 
         * its top Flow node to ask for motion to a single QE.  Later, 
         * apply_motion will override that to bring it to the QD instead.
         */
        bring_result_to_qd(subqueryplan, NULL);

        prescan(plannedStmt, plannedStmt->topPlans[itop++], subqueryplan, context);
    }
	
    if (isTargetDistributed)
    {
        plannedStmt->dispatchParallel = true;
        plannedStmt->topPlans[0]->dispatchParallel = true;
    }

    /*
     * Now we know whether the query requires worker processes.
     */
	if (plannedStmt->dispatchParallel)
	{
		/*
		 * Add Motion operators where requested.  Assign slice numbers.
		 */
		plan = apply_motion(plannedStmt);
		
		/* 
         * apply_motion() should have rejected unsupported subqueries,
         * but make an extra test to be safe.
         */
        if (has_correlation_params(glob->paramlist))
		{
			ereport(ERROR, (
					errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
					errmsg(PACKAGE_NAME " does not yet support that query."),
					errdetail("The query contains a correlated subquery.")
			));
		}
	}

	if (gp_enable_motion_deadlock_sanity)
		motion_sanity_check(plan);

	return plan;
}



/* --------------------------------------------------------------------
 *	Static Helper routines
 * --------------------------------------------------------------------
 */


/*
 * bring_result_to_qd
 *
 * Updates the plan's top Flow node to request delivery of the query
 * output to a single qExec.  Later, apply_motion() will override that 
 * and instead deliver the output back here to the qDisp.
 */
static void
bring_result_to_qd(Plan *plan, Query *query)
{
    /*
     * If the query has an ORDER BY clause, use Merge Receive to
     * preserve the ordering.
     *
     * The plan has already been set up to ensure each qExec's 
     * result is properly ordered according to the ORDER BY 
     * specification.  The existing ordering could be stronger 
     * than required; if so, omit the extra trailing columns 
     * from the Merge Receive key.
     *
     * An unordered result is ok if the ORDER BY ordering is
     * degenerate (on constant exprs) or the result is known to
     * have at most one row. 
     */
    if (query &&
        query->sortClause &&
        plan->flow->numSortCols > 0)
    {
        if (plan->flow->numSortCols > list_length(query->sortClause))
            plan->flow->numSortCols = list_length(query->sortClause);

        Insist(focusPlan(plan, true, true, false));
    }

    /* Use UNION RECEIVE.  Does not preserve ordering. */
    else
        Insist(focusPlan(plan, true, false, false));
}                               /* bring_result_to_qd */


/*
 * bring_result_to_target_table
 *
 * Updates the plan's top Flow node to request redistribution of the query
 * output to a writer gang for insertion into the target table.
 *
 * 'cmd' is the SQL statement keyword, such as "INSERT".
 */
static void
bring_result_to_target_table(PlannedStmt   *plannedStmt,
                             GpPolicy      *targetPolicy,
                             const char    *cmd)
{
    Plan       *plan = plannedStmt->planTree;
	List       *hashExpr;
	
	Assert(targetPolicy->ptype == POLICYTYPE_PARTITIONED);
	Assert(targetPolicy->nattrs >= 0);
	Assert(targetPolicy->nattrs <= 1024);

    /*
     * If mirroring is enabled, force Redistribute Motion
     * even if the query result is already partitioned on the
     * same key as the target.  Need this to make sure that any
     * function calls in the SELECT targetlist are executed
     * on primaries only, and the result is then sent to
     * primaries and mirrors to be inserted.
     */
    if (isFTSEnabled())
        plan->flow->hashExpr = NIL;

    /*
     * Make sure the top level flow is partitioned on the
     * partitioning key of the target relation.	Since this
     * is a SELECT INTO or INSERT command, the target list 
     * will correspond to the attributes of the target relation 
     * in order.
     */
    hashExpr = getExprListFromTargetList(plan->targetlist,
                                         targetPolicy->nattrs,
                                         targetPolicy->attrs);
    if (!repartitionPlan(plan, true, false, false, hashExpr))
        ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
                        errmsg("Cannot parallelize that %s yet", cmd)
                ));
}                               /* bring_result_to_target_table */


/*
 * colChangedByUpdate
 *
 * Want to test whether an update statement possibly changes the
 * value of any of the partitioning columns.
 *
 * For each partitioning column, the TargetListEntry for that varattno
 * should be just a Var node with the same varattno.
 */
static AttrNumber
colChangedByUpdate(Index        targetvarno,
                   List        *targetlist,
                   int          nattrs,
                   AttrNumber  *attrs)
{
	int     i;

	for (i = 0; i < nattrs; i++)
	{
		TargetEntry    *tle = get_tle_by_resno(targetlist, attrs[i]);
		Var		       *var;

		Insist(tle);

		if (!IsA(tle->expr, Var))
			return attrs[i];

		var = (Var *)tle->expr;
		if (var->varnoold != targetvarno ||
            var->varoattno != attrs[i])
			return attrs[i];
	}
	return InvalidAttrNumber;
}                               /* colChangedByUpdate */


/*
 * final_motion_for_insert
 *
 * If inserting into a distributed table, requests that the query 
 * result be distributed to the target segments, and returns true.
 *
 * If inserting into a local table, requests that the query result be
 * moved (if necessary) to the QD, and returns false.
 */
static bool
final_motion_for_insert(PlannedStmt *plannedStmt)
{
	Oid			reloid;
    Relation    relation;
    int         targetrelid;
    bool        isTargetDistributed = false;

    Assert(plannedStmt->commandType == CMD_INSERT);

    /* Get Oid of target relation. */
    Insist(list_length(plannedStmt->resultRelations) == 1);
    targetrelid = linitial_int(plannedStmt->resultRelations);
    Insist(targetrelid > 0 &&
	       targetrelid <= list_length(plannedStmt->rtable));
	reloid = getrelid(targetrelid, plannedStmt->rtable);

    /* Is target rel distributed? */
    relation = relation_open(reloid, NoLock);               
    if (RelationIsDistributed(relation))
    {
        isTargetDistributed = true;
        bring_result_to_target_table(plannedStmt,
                                     relation->rd_cdbpolicy,
                                     "INSERT");
    }
    else
        bring_result_to_qd(plannedStmt->planTree, NULL);

    relation_close(relation, NoLock);
    
    return isTargetDistributed;
}                               /* final_motion_for_insert */


/*
 * final_motion_for_select
 *
 * If SELECT INTO, requests that the query result be distributed to the 
 * target segments, and returns true.
 *
 * For an ordinary query, requests that the query result be moved to the 
 * QD, and returns false.
 */
static bool
final_motion_for_select(PlannedStmt *plannedStmt, Query *query)
{
    Plan   *plan = plannedStmt->planTree;
    bool    isTargetDistributed = false;

    Assert(plannedStmt->commandType == CMD_SELECT);

	if (plannedStmt->intoClause)
	{
        IntoClause *intoClause = plannedStmt->intoClause;

		/* SELECT INTO always creates partitioned tables. */
		isTargetDistributed = true;

        /* If user did not specify DISTRIBUTED BY clause, get default */
        if (!intoClause->intoPolicy)
            intoClause->intoPolicy = select_into_default_policy(plan);
    	
        bring_result_to_target_table(plannedStmt,
                                     intoClause->intoPolicy,
                                     "SELECT INTO");
	}
	else 
        bring_result_to_qd(plan, query);
    
    return isTargetDistributed;
}                               /* final_motion_for_select */


/*
 * final_motion_for_upd_del
 *
 * If target table of UPDATE or DELETE is distributed:
 *
 * - Requests that the query result be distributed to the target segments.
 *
 * - Throws an error and does not return if a column to be updated is in 
 *   the table's DISTRIBUTED BY list.
 *
 * - Returns true.
 *
 * Returns false if target table is not distributed.
 */
static bool
final_motion_for_upd_del(PlannedStmt *plannedStmt)
{
    Plan       *plan = plannedStmt->planTree;
    List       *planlist;
    ListCell   *plancell;
    ListCell   *rticell;
    bool        isTargetDistributed = false;

    Assert(plannedStmt->commandType == CMD_UPDATE ||
           plannedStmt->commandType == CMD_DELETE);

    /* Updating a single table? */
    if (list_length(plannedStmt->resultRelations) == 1)
        planlist = list_make1(plan);

    /* Updating an inheritance family. */
    else
    {
        planlist = ((Append *)plan)->appendplans;
        Assert(IsA(plan, Append) &&
               ((Append *)plan)->isTarget &&
               list_length(planlist) == list_length(plannedStmt->resultRelations));
    }

    /* Loop over target tables and corresponding source plans. */
    forboth(plancell, planlist,
            rticell, plannedStmt->resultRelations)
    {
        Plan           *sourceplan = (Plan *)lfirst(plancell);
        int             targetrelid = lfirst_int(rticell);
        RangeTblEntry  *rte; 
        Relation        relation;

        Insist(targetrelid > 0 &&
		       targetrelid <= list_length(plannedStmt->rtable));
        rte = rt_fetch(targetrelid, plannedStmt->rtable);
        relation = relation_open(rte->relid, NoLock);               

        /* Distributed target table? */
        if (RelationIsDistributed(relation))
        {
            List           *hashExpr;

            isTargetDistributed = true;

            /* 
             * Disallow updating of the DISTRIBUTED BY columns. 
             */
            if (plannedStmt->commandType == CMD_UPDATE)
            {
                int icol = colChangedByUpdate(targetrelid, 
                                              sourceplan->targetlist, 
                                              relation->rd_cdbpolicy->nattrs, 
                                              relation->rd_cdbpolicy->attrs);

                if (icol != InvalidAttrNumber)
			        ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
							        errmsg("Column '%s' of '%s' cannot be updated "
                                           "because the table is DISTRIBUTED BY "
                                           "that column.",
                                           get_relid_attribute_name(rte->relid, icol),
                                           get_rel_name(rte->relid)),
                                    errhint("To change a row's distribution key, "
                                            "DELETE the old row and INSERT the "
                                            "new row.")
							        ));
            }

            /*
             * Make sure the top level flow is partitioned on the target's
             * DISTRIBUTED BY columns.
             */
            hashExpr = getExprListFromTargetList(plan->targetlist,
                                                 relation->rd_cdbpolicy->nattrs,
                                                 relation->rd_cdbpolicy->attrs);
            if (!repartitionPlan(plan, true, false, false, hashExpr))
                ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
                                errmsg("Cannot parallelize that %s yet",
                                       CreateCommandTag((Node*)plannedStmt))
                        ));
        }

        /* 
         * Not distributed.  UPDATE or DELETE takes place locally. 
         */
        else
            bring_result_to_qd(plan, NULL);

        relation_close(relation, NoLock);                
    }
    return isTargetDistributed;
}                               /* final_motion_for_upd_del */


/*
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
 */
static List *
getExprListFromTargetList(List         *tlist,
                          int           numCols,
                          AttrNumber   *colIdx)
{
	int			i;
	List	   *elist = NIL;

	for (i = 0; i < numCols; i++)
	{
		/* Find expr in TargetList */
		AttrNumber	n = colIdx[i];

		TargetEntry *target = get_tle_by_resno(tlist, n);

		if (target == NULL)
			elog(ERROR, "no tlist entry for key %d", n);

        /* Make a Var referencing subplan tlist. */
        elist = lappend(elist, cdbpullup_makeVar(OUTER, n, target->expr));
	}

	return elist;
}


/*
 * has_correlation_params
 */
static bool
has_correlation_params(List *planner_param_items)
{
	ListCell *c;
	
	foreach (c, planner_param_items)
	{
		PlannerParamItem *pli = (PlannerParamItem*)lfirst(c);
		if ( IsA(pli->item,Var) || IsA(pli->item,Aggref) )
			return true;
	}
	return false;
}                               /* has_correlation_params */


/*
 * select_into_default_policy
 */
static GpPolicy *
select_into_default_policy(Plan *plan)
{
    GpPolicy   *targetPolicy;
	List       *hashExpr;
	ListCell   *exp1;
    char       *columns;
	int			maxattrs = 200;
    int         i;
	
    targetPolicy = palloc0(sizeof(GpPolicy)
                            - sizeof(targetPolicy->attrs) 
                            + maxattrs * sizeof(targetPolicy->attrs[0]));
    targetPolicy->nattrs = 0;

    targetPolicy->ptype = POLICYTYPE_PARTITIONED;

    /* Find out what the flow is partitioned on */
    hashExpr = plan->flow->hashExpr;
    foreach(exp1, hashExpr)
    {
	    AttrNumber  n;
	    bool        found_expr = false;
    	
	    /* See if this Expr is a column of the result table */
	    for(n=1;n<=list_length(plan->targetlist);n++)
	    {
		    Var		       *new_var = NULL;
		    TargetEntry    *target = get_tle_by_resno(plan->targetlist, n);
    		
		    if (!target->resjunk)
		    {
			    /* If subplan expr is a Var, copy to preserve its EXPLAIN info. */
                if (IsA(target->expr, Var))
                {
                    new_var = copyObject(target->expr);
                    new_var->varno = OUTER;
                    new_var->varattno = n;
                }

                /*
	             * Make a Var that references the target list entry at this offset,
	             * using OUTER as the varno
	             */
                else
		            new_var = makeVar(OUTER,
                                      n,
                                      exprType((Node *) target->expr),
                                      exprTypmod((Node *) target->expr),
                                      0);

			    if (equal(lfirst(exp1),new_var))
			    {
				    /* If it is, use it to partition the result table, to avoid
				     * unnecessary redistribution of data */
				    targetPolicy->attrs[targetPolicy->nattrs++] = n;
				    found_expr = true;
				    break;
			    }
		    }
	    }
	    if (!found_expr)
		    break;
    }

    /* do we know how to partition? */
    if (targetPolicy->nattrs == 0)
    {
	    /*
	     * hash on the first hashable column we can find.
	     */
	    int i;
	    for (i = 0; i < list_length(plan->targetlist); i++)
	    {
		    TargetEntry *target = get_tle_by_resno(plan->targetlist, targetPolicy->attrs[i]);
		    if (target)
		    {
			    Oid typeOid = exprType((Node *) target->expr);
			    bool isArray = false;  /* How do I find this out? */
    			
			    if(isGreenplumDbHashable(typeOid) || isArray)
			    {
				    targetPolicy->attrs[targetPolicy->nattrs++] = i+1;
				    break;
			    }
		    }
	    }
    }

    columns = palloc((NAMEDATALEN+3)*targetPolicy->nattrs+1);
    columns[0] = '\0';
    for (i=0;i<targetPolicy->nattrs;i++)
    {
	    TargetEntry *target = get_tle_by_resno(plan->targetlist, targetPolicy->attrs[i]);
	    if (i>0)
		    strcat(columns, ", ");
	    if (target->resname)
		    strcat(columns, target->resname);
	    else
		    strcat(columns, "???");
    }

    ereport(NOTICE,
		    (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
		     errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column(s) "
			        "named '%s' as the distribution key for this table.", columns),
	         errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
	     		     " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));

    return targetPolicy;
}                               /* select_into_default_policy */


/* ----------------------------------------------------------------------- *
 * Functions prescan() and prescan_walker() use the plan_tree_walker()
 * framework to visit the nodes of the plan tree in order to check for
 * unsupported usages and collect information for use in finalizing the
 * plan.  The walker exits with an error only due an unexpected problems.
 * ----------------------------------------------------------------------- *
 */
static void
prescan(PlannedStmt *plannedStmt, CdbTopPlan *topPlan, Plan *plan, PlanProfile *context)
{
    context->plannedStmt = plannedStmt;
    context->topPlan = topPlan;

    Assert(!topPlan->dispatchParallel);

    /* Top node of plan must already have a Flow node. */ 
    Assert(plan->flow &&
		   plan->flow->flotype != FLOW_UNDEFINED);

    /* Examine the plan. */
    if ( prescan_walker((Node *) plan, context) )
		Insist(0);

    /* Set summary flag if main or subquery plan is to be dispatched to QEs. */
    if (topPlan->dispatchParallel)
        plannedStmt->dispatchParallel = true;
}                               /* prescan */


/*
 * prescan_walker
 *
 * Finds out whether any part of the plan needs to be dispatched to QEs
 * for execution. 
 *
 * Since the optimizer isn't in a position to view plan globally,
 * it may generate plans that we can't execute.  This function is
 * responsible for detecting potential problem usages:
 *
 *    Example: Non-initplan subplans are legal in sequential contexts
 *    but not in parallel contexs.
 */
static bool
prescan_walker(Node *node, PlanProfile * context)
{
	if (node == NULL)
		return false;

    if (is_plan_node(node))
    {
        Plan   *plan = (Plan *)node;

        if (plan->flow)
        {
            /* Will apply_motion() add a Motion operator above this node? */
            if (plan->flow->req_move != MOVEMENT_NONE)
                context->topPlan->dispatchParallel = true;

            /* Does the Flow node indicate the result is distributed? */
            switch (plan->flow->flotype)
            {
                case FLOW_SINGLETON:
                    break;
                case FLOW_REPLICATED:
                case FLOW_PARTITIONED:
                    context->topPlan->dispatchParallel = true;
                    break;
                default:
                    Insist(0);
            }
        }
    }

	switch (nodeTag(node))
	{
		case T_SubPlan:
		{
			/*
			 * A SubPlan node is not a Plan node, but an Expr node that
			 * references a Plan.  (The plan itself is in a list attached 
             * to the PlannedStmt node.)  The SubPlan node corresponds to a
			 * SubLink in the query tree (a SQL subquery expression) and
			 * may occur in an expression or an initplan list.
			 */
			SubPlan    *subplan = (SubPlan *) node;
            CdbTopPlan *top = exec_subplan_get_top(context->plannedStmt, subplan);

            Assert(top &&
                   IsA(top, CdbTopPlan));

            /* 
             * Save some subplan info in the subquery's CdbTopPlan node so
             * we can find it later without walking the whole tree again.
             */
            top->isInitPlan = subplan->is_initplan;
            break;
		}

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
                                           context->plannedStmt->rtable);

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
            break;
        }

        case T_Motion:
            context->topPlan->dispatchParallel = true;
            break;

		default:
            break;
	}

    return plan_tree_walker(node, prescan_walker, context);
}                               /* prescan_walker */


/*
 * Construct a new Flow in the current memory context.
 */
struct Flow *
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
pull_up_Flow(struct Plan *plan, struct Plan *subplan, bool withSort)
{
	Flow	   *model_flow;
	Flow	   *new_flow;

	Insist(subplan);

	model_flow = subplan->flow;

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
	        ARRAYCOPY(new_flow->nullsFirst, sort->nullsFirst, sort->numCols);
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
			{
                ARRAYCOPY(new_flow->sortOperators,
                          model_flow->sortOperators,
                          new_flow->numSortCols);
               ARRAYCOPY(new_flow->nullsFirst,
                          model_flow->nullsFirst,
                          new_flow->numSortCols);
			}
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
            ARRAYCOPY(new_flow->nullsFirst,
                      model_flow->nullsFirst,
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
focusPlan(struct Plan *plan, bool live, bool stable, bool rescannable)
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
broadcastPlan(struct Plan *plan, bool live, bool stable, bool rescannable)
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
repartitionPlan(struct Plan    *plan, 
                bool            live, 
                bool            stable, 
                bool            rescannable, 
                struct List    *hashExpr)
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

        /* Zap sort cols if motion has destroyed the ordering. */
        if (disorder && !reorder)
        {
            flow->numSortCols = 0;
            flow->sortColIdx = NULL;
            flow->sortOperators = NULL;
			flow->nullsFirst = NULL;
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
		flow->nullsFirst = NULL;
    }
	
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
