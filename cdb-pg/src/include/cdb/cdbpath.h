/*-------------------------------------------------------------------------
 *
 * cdbpath.h
 *
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPATH_H
#define CDBPATH_H

#include "cdb/cdbvars.h"        /* CdbIdentity */


/* globals */
extern Cost     cdb_motion_tuple_cost;

extern int getCdbSegmentCount(void);


Path *
cdbpath_create_motion_path(Query           *root,
                           Path            *subpath,
                           List            *pathkeys,
                           bool             require_existing_order,
                           CdbPathLocus     locus);

CdbPathLocus
cdbpath_motion_for_join(Query      *root,
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

    if (IsA(path, UniquePath))
        rows = ((UniquePath *)path)->rows;
    else
        rows = path->parent->rows;

    /* Just one partition, or whole rel? */
    if (CdbPathLocus_IsRoot(path->locus) ||
        CdbPathLocus_IsReplicated(path->locus))
    {
    	if (Cdb_role == CDB_ROLE_DISPATCH)
        {
            int numSegments = getCdbSegmentCount();
            if (numSegments > 1)
                rows *= numSegments;
        }
    }

    return rows;
}                               // cdbpath_rows

#endif   /* CDBPATH_H */
