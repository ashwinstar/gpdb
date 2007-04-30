/*-------------------------------------------------------------------------
 *
 * cdbgroup.h
 *	  prototypes for cdbgroup.c.
 *
 *
 * Portions Copyright (c) 2006-2007, Greenplum
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBGROUP_H
#define CDBGROUP_H

#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

#include "optimizer/clauses.h" /* AggClauseCount */

extern Plan *
cdb_grouping_planner(PlannerInfo* root, 
					AggClauseCounts *agg_counts,
					List *tlist,
					Path *best_path,
					Path *cheapest_path,
					bool use_hashed_grouping,
					double tuple_fraction,
					double dNumGroups,
					List **pcurrent_pathkeys);

extern bool cdbpathlocus_collocates(CdbPathLocus locus, List *pathkeys);
extern CdbPathLocus cdbpathlocus_from_flow(Flow *flow);



#endif   /* CDBGROUP_H */
