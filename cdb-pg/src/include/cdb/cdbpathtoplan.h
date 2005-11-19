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
cdbpathtoplan_create_flow(PlannerInfo   *root,
                          Path     *path,
                          List     *pathkeys,
                          Plan     *plan);

Motion *
cdbpathtoplan_create_motion_plan(PlannerInfo   *root,
                                 CdbMotionPath *path,
                                 Plan          *subplan);

#endif   /* CDBPATHTOPLAN_H */
