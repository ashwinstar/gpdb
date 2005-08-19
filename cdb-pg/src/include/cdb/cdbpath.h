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
                           CdbPathLocus     locus);

CdbPathLocus
cdbpath_motion_for_join(Query      *root,
                        JoinType    jointype,
                        Path      **outer_path,         /* INOUT */
                        Path      **inner_path,         /* INOUT */
                        List       *mergeclause_list,
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
        if (getCdbSegmentCount() > 1)
            rows *= getCdbSegmentCount();
    }

    return rows;
}                               // cdbpath_rows

#endif   /* CDBPATH_H */
