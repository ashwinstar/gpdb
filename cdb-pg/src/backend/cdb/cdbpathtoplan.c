/*-------------------------------------------------------------------------
 *
 * cdbpathtoplan.c
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/relation.h"     /* RelOptInfo */
#include "optimizer/pathnode.h" /* Path */
#include "optimizer/planmain.h" /* make_sort_from_pathkeys() */

#include "cdb/cdbllize.h"       /* makeFlow() */
#include "cdb/cdbmutate.h"      /* make_*_motion() */

#include "cdb/cdbpathtoplan.h"  /* me */


/*
 * cdbpathtoplan_create_flow
 */
Flow *
cdbpathtoplan_create_flow(PlannerInfo    *root,
                          Path     *path,
                          List     *pathkeys,
                          Plan     *plan)
{
    Flow       *flow = NULL;

    /* Distribution */
    if (CdbPathLocus_IsRoot(path->locus))
    {
        flow = makeFlow(FLOW_SINGLETON);
        flow->segindex = -1;
    }
    else if (CdbPathLocus_IsSingleQE(path->locus))
    {
        flow = makeFlow(FLOW_SINGLETON);
        flow->segindex = 0;
    }
    else if (CdbPathLocus_IsReplicated(path->locus) ||
             CdbPathLocus_IsGeneral(path->locus))
    {
        flow = makeFlow(FLOW_REPLICATED);
    }
    else if (CdbPathLocus_IsHashed(path->locus) ||
             CdbPathLocus_IsHashedOJ(path->locus))
    {
        flow = makeFlow(FLOW_PARTITIONED);
        flow->hashExpr = cdbpathlocus_get_partkey_exprs(path->locus,
                                                        path->parent->relids,
                                                        plan->targetlist);
    }
    else if (CdbPathLocus_IsStrewn(path->locus))
        flow = makeFlow(FLOW_PARTITIONED);
    else
        Assert(false);

    /* Ordering */
    if (pathkeys)
    {
        Sort   *sort = make_sort_from_pathkeys(root, plan, pathkeys);
        flow->numSortCols = sort->numCols;
        flow->sortColIdx = sort->sortColIdx;
        flow->sortOperators = sort->sortOperators;
    }

    flow->req_move = MOVEMENT_NONE;
    flow->req_xform = MOVEMENT_NONE;
    return flow;
}                               /* cdbpathtoplan_create_flow */


/*
 * cdbpathtoplan_create_motion_plan
 */
Motion *
cdbpathtoplan_create_motion_plan(PlannerInfo         *root,
                                 CdbMotionPath *path,
                                 Plan          *subplan)
{
    Motion *motion = NULL;

    /* Decorate the subplan with a Flow node telling the plan slicer
     * what kind of gang will be needed to execute the subplan.
     */
    subplan->flow = cdbpathtoplan_create_flow(root,
                                              path->subpath,
                                              path->subpath->pathkeys,
                                              subplan);

    /* Send all tuples to a single process? */
    if (CdbPathLocus_IsBottleneck(path->path.locus))
    {
        int destSegIndex = -1;                          /* to dispatcher */

        if (CdbPathLocus_IsSingleQE(path->path.locus))
            destSegIndex = 0;                           /* to singleton qExec */

        if (path->path.pathkeys)
        {
            Sort   *sort = make_sort_from_pathkeys(root,
                                                   subplan,
                                                   path->path.pathkeys);
            motion = make_sorted_union_motion(subplan,
                                              false,    /* sendToSecondary */
                                              destSegIndex,
                                              sort->numCols,
                                              sort->sortColIdx,
                                              sort->sortOperators);
        }
        else
            motion = make_union_motion(subplan,
                                       false,           /* sendToSecondary */
                                       destSegIndex);
    }

    /* Send all of the tuples to all of the QEs in gang above... */
    else if (CdbPathLocus_IsReplicated(path->path.locus))
            motion = make_broadcast_motion(subplan,
                                           false);      /* sendToSecondary */

    /* Hashed redistribution to all QEs in gang above... */
    else if (CdbPathLocus_IsHashed(path->path.locus) ||
             CdbPathLocus_IsHashedOJ(path->path.locus))
    {
        List   *hashExpr = cdbpathlocus_get_partkey_exprs(path->path.locus,
                                                          path->path.parent->relids,
                                                          subplan->targetlist);
        Assert(hashExpr);
        motion = make_hashed_motion(subplan,
                                    false,              /* sendToSecondary */
                                    hashExpr);
    }
    else
        Assert(false);

    /* Should sender discard rows that hash to other partitions? */
    if (path->hashToSelfOnly)
        setHashToSelfOnly(motion);

    return motion;
}                               /* cdbpathtoplan_create_motion_plan */


