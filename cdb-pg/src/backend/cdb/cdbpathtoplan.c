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
cdbpathtoplan_create_flow(Query    *root,
                          Path     *path,
                          Plan     *plan)
{
    Flow       *flow;

    /* Distribution */
    if (CdbPathLocus_IsRoot(path->locus))
    {
        flow = makeFlow(FLOW_SINGLETON);
        flow->segindex = -1;
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
    else
        Assert(false);

    /* Ordering */
    if (path->pathkeys)
    {
        Sort   *sort = make_sort_from_pathkeys(root, plan, path->pathkeys);
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
cdbpathtoplan_create_motion_plan(Query         *root,
                                 CdbMotionPath *path,
                                 Plan          *subplan)
{
    Motion *motion;

    /* Send all tuples to dispatcher... */
    if (CdbPathLocus_IsRoot(path->path.locus))
    {
        if (path->path.pathkeys)
        {
            Sort   *sort = make_sort_from_pathkeys(root,
                                                   subplan,
                                                   path->path.pathkeys);
            motion = make_sorted_union_motion(subplan,
                                              false,    /* sendToSecondary */
                                              -1,       /* destSegIndex => QD */
                                              sort->numCols,
                                              sort->sortColIdx,
                                              sort->sortOperators);
        }
        else
            motion = make_union_motion(subplan,
                                       false,           /* sendToSecondary */
                                       -1);             /* destSegIndex => QD */
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
        Assert(false);          /* CDB TODO: raise error here */

    return motion;
}                               /* cdbpathtoplan_create_motion_plan */



