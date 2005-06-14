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


/* globals */
extern Cost     cdb_motion_tuple_cost;


void
cdbpath_add_flow_to_plan(Query *root,
                         Path  *path,
                         Plan  *plan);

Path *
cdbpath_create_motion_path(Path            *subpath,    
                           List            *pathkeys,   
                           CdbPathLocus     locus);

CdbPathLocus
cdbpath_motion_for_append(List *subpaths);

CdbPathLocus
cdbpath_motion_for_join(Query  *root,
                        Path  **outer_path,         /* INOUT */
                        Path  **inner_path,         /* INOUT */
                        List   *mergeclause_list,   
                        List   *outer_pathkeys,
                        List   *inner_pathkeys,
                        bool    must_preserve_order);

#endif   /* CDBPATH_H */
