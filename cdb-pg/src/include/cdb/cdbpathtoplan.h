/*-------------------------------------------------------------------------
 *
 * cdbpathtoplan.h
 *
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPATHTOPLAN_H
#define CDBPATHTOPLAN_H


Flow *
cdbpathtoplan_create_flow(Query    *root,
                          Path     *path,
                          Plan     *plan);

Motion *
cdbpathtoplan_create_motion_plan(Query         *root,
                                 CdbMotionPath *path,
                                 Plan          *subplan);

#endif   /* CDBPATHTOPLAN_H */
