/*-------------------------------------------------------------------------
 *
 * cdbpath.h
 *
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPATH_H
#define CDBPATH_H

#include "cdb/cdbvars.h"        /* getMPPSegmentCount, mpp_opt_segments, etc */

/* Globals */
extern Cost     cdbpath_motion_cost_per_row;
extern int      cdbpath_segments;

Path *
cdbpath_create_motion_path(PlannerInfo     *root,
                           Path            *subpath,
                           List            *pathkeys,
                           bool             require_existing_order,
                           CdbPathLocus     locus);

CdbPathLocus
cdbpath_motion_for_join(PlannerInfo  *root,
                        JoinType    jointype,           /* JOIN_INNER, JOIN_FULL, JOIN_LEFT, JOIN_RIGHT */
                        Path      **outer_path,         /* INOUT */
                        Path      **inner_path,         /* INOUT */
                        List       *mergeclause_list,   /* equijoin RestrictInfo list */
                        List       *outer_pathkeys,
                        List       *inner_pathkeys,
                        bool        outer_require_existing_order,
                        bool        inner_require_existing_order);

/*
 * cdbpath_rows
 *
 * Returns a Path's estimated number of result rows.
 */
static __inline__ double
cdbpath_rows(Path *path)
{
    double rows;

    rows = IsA(path, BitmapHeapPath)    ? ((BitmapHeapPath *)path)->rows
         : IsA(path, IndexPath)         ? ((IndexPath *)path)->rows
         : IsA(path, UniquePath)        ? ((UniquePath *)path)->rows
         : path->parent->rows;

    /* Just one partition, or whole rel? */
    if (CdbPathLocus_IsBottleneck(path->locus) ||
        CdbPathLocus_IsReplicated(path->locus))
        rows *= cdbpath_segments;

    return rows;
}                               /* cdbpath_rows */

#endif   /* CDBPATH_H */
