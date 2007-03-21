/*-------------------------------------------------------------------------
 *
 * cdbpath.h
 *
 *
 * Copyright (c) 2005-2007, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPATH_H
#define CDBPATH_H

#include "cdb/cdbvars.h"        /* getgpsegmentCount, mpp_opt_segments, etc */

/* Globals */
extern Cost     cdbpath_motion_cost_per_row;
extern int      cdbpath_segments;

void
cdbpath_cost_motion(CdbMotionPath *motionpath);

Path *
cdbpath_create_motion_path(PlannerInfo     *root,
                           Path            *subpath,
                           List            *pathkeys,
                           bool             require_existing_order,
                           CdbPathLocus     locus);

CdbPathLocus
cdbpath_motion_for_join(PlannerInfo    *root,
                        JoinType        jointype,           /* JOIN_INNER/FULL/LEFT/RIGHT/IN */
                        Path          **p_outer_path,       /* INOUT */
                        Path          **p_inner_path,       /* INOUT */
                        List           *mergeclause_list,   /* equijoin RestrictInfo list */
                        List           *outer_pathkeys,
                        List           *inner_pathkeys,
                        bool            outer_require_existing_order,
                        bool            inner_require_existing_order);

void 
cdbpath_dedup_fixup(PlannerInfo *root, Path *path);

/*
 * cdbpath_rows
 *
 * Returns a Path's estimated number of result rows.
 */
static inline double
cdbpath_rows(Path *path)
{
    double rows;
    Path  *p;

    p = (IsA(path, CdbMotionPath))  ? ((CdbMotionPath *)path)->subpath
      : path;

    rows = IsA(p, BitmapHeapPath)   ? ((BitmapHeapPath *)p)->rows
         : IsA(p, IndexPath)        ? ((IndexPath *)p)->rows
         : IsA(p, UniquePath)       ? ((UniquePath *)p)->rows
         : (CdbPathLocus_IsBottleneck(path->locus) ||
            CdbPathLocus_IsReplicated(path->locus)) 
                                        ? path->parent->rows * cdbpath_segments
         : path->parent->rows;

    return rows;
}                               /* cdbpath_rows */

#endif   /* CDBPATH_H */
