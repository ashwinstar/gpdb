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
					 double *p_dNumGroups,
					 CanonicalGroupingSets *canonical_grpsets,
					 List **pcurrent_pathkeys);

extern bool cdbpathlocus_collocates(CdbPathLocus locus, List *pathkeys);
extern CdbPathLocus cdbpathlocus_from_flow(Flow *flow);
extern void adapt_flow_to_targetlist(Plan *plan);
extern void generate_two_stage_tlists(List *tlist, List *sub_tlist,
									  Node *havingQual,
									  int numGroupCols, AttrNumber *groupColIdx,
									  List **p_prelim_tlist, List **p_final_tlist,
									  List **p_final_qual);
extern void generate_three_tlists(List *tlist,
								  bool twostage,
								  List *sub_tlist,
								  Node *havingQual,
								  int numGroupCols,
								  AttrNumber *groupColIdx,
								  List **p_tlist1,
								  List **p_tlist2,
								  List **p_tlist3,
								  List **p_qual2,
								  List **p_qual3);
extern Plan *add_second_stage_agg(PlannerInfo *root,
								  bool is_agg,
								  List *prelim_tlist,
								  List *final_tlist,
								  List *final_qual,
								  AggStrategy aggstrategy,
								  int numGroupCols,
								  AttrNumber *prelimGroupColIdx,
								  int num_nullcols,
								  uint64 input_grouping,
								  uint64 grouping,
								  int rollup_gs_times,
								  double numGroups,
								  int numAggs,
								  int transSpace,
								  const char *alias,
								  List **p_current_pathkeys,
								  Plan *result_plan);
extern List *generate_subquery_tlist(Index varno, List *input_tlist);

#endif   /* CDBGROUP_H */
