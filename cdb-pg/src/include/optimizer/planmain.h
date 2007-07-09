/*-------------------------------------------------------------------------
 *
 * planmain.h
 *	  prototypes for various files in optimizer/plan
 *
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/planmain.h,v 1.95 2006/08/12 02:52:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANMAIN_H
#define PLANMAIN_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h" /* AggClauseCounts */

/*
 * prototypes for plan/planmain.c
 */
extern void query_planner(PlannerInfo *root, List *tlist,
			  double tuple_fraction,
			  Path **cheapest_path, Path **sorted_path,
			  double *num_groups);

/*
 * prototypes for plan/planagg.c
 */
extern Plan *optimize_minmax_aggregates(PlannerInfo *root, List *tlist,
						   Path *best_path);

/*
 * prototype for plan/plangroupexp.c
 */
extern Plan *make_distinctaggs_for_rollup(PlannerInfo *root, bool is_agg,
										  List *tlist, bool twostage, List *sub_tlist,
										  List *qual, AggStrategy aggstrategy,
										  int numGroupCols, AttrNumber *grpColIdx,
										  double numGroups, int *rollup_gs_times,
										  int numAggs, int transSpace,
										  double *p_dNumGroups,
										  List **p_current_pathkeys,
										  Plan *lefttree);

/*
 * prototypes for plan/planwindow.c
 */
extern Plan *window_planner(PlannerInfo *root, double tuple_fraction, List **pathkeys_ptr);

/*
 * prototype for plan/plangroupexp.c
 */
extern Plan *plan_grouping_extension(PlannerInfo *root,
									 Path *path,
									 List **p_tlist, List *sub_tlist,
									 bool is_agg, bool twostage,
									 List *qual,
									 int *p_numGroupCols, AttrNumber **p_grpColIdx,
									 AggClauseCounts *agg_counts,
									 CanonicalGroupingSets *canonical_grpsets,
									 double *p_dNumGroups,
									 bool *querynode_changed,
									 List **p_current_pathkeys,
									 Plan *lefttree);

/*
 * prototypes for plan/createplan.c
 */
extern Plan *create_plan(PlannerInfo *root, Path *path);
extern SubqueryScan *make_subqueryscan(List *qptlist, List *qpqual,
				  Index scanrelid, Plan *subplan);
extern Append *make_append(List *appendplans, bool isTarget, List *tlist);
extern Sort *make_sort_from_sortclauses(PlannerInfo *root, List *sortcls,
						   Plan *lefttree);
extern Sort *make_sort_from_groupcols(PlannerInfo *root, List *groupcls,
									  AttrNumber *grpColIdx, bool appendGrouping,
									  Plan *lefttree);
extern Sort *make_sort_from_reordered_groupcols(PlannerInfo *root,
												List *groupcls,
												AttrNumber *orig_grpColIdx,
												AttrNumber *new_grpColIdx,
												bool appendGrouping,
												Plan *lefttree);
extern Agg *make_agg(PlannerInfo *root, List *tlist, List *qual,
					 AggStrategy aggstrategy,
					 int numGroupCols, AttrNumber *grpColIdx,
					 long numGroups, int numNullCols,
					 uint64 inputGrouping, uint64 grouping,
					 int rollupGSTimes,
					 int numAggs, int transSpace,
					 Plan *lefttree);
extern Group *make_group(PlannerInfo *root, List *tlist, List *qual,
						 int numGroupCols, AttrNumber *grpColIdx,
						 double numGroups, int numNullCols,
						 uint64 inputGrouping,  uint64 grouping,
						 int rollupGSTimes,
						 Plan *lefttree);
extern Window *make_window(PlannerInfo *root, List *tlist,
		   int numPartCols, AttrNumber *partColIdx, List *windowKeys,
		   Plan *lefttree);
extern Material *make_material(Plan *lefttree);
extern Plan *materialize_finished_plan(Plan *subplan);
extern Unique *make_unique(Plan *lefttree, List *distinctList);
extern Limit *make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount,
		   int64 offset_est, int64 count_est);
extern SetOp *make_setop(SetOpCmd cmd, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx);
extern Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);
extern bool is_projection_capable_plan(Plan *plan);
extern Sort *make_sort_from_pathkeys(PlannerInfo *root, Plan *lefttree,
                                     List *pathkeys, Relids relids,
                                     bool add_keys_to_targetlist);
extern Plan *plan_pushdown_tlist(Plan *plan, List *tlist);      /*CDB*/

/*
 * prototypes for plan/initsplan.c
 */
extern int	from_collapse_limit;
extern int	join_collapse_limit;

extern void add_base_rels_to_query(PlannerInfo *root, Node *jtnode);
extern void build_base_rel_tlists(PlannerInfo *root, List *final_tlist);
extern void add_vars_to_targetlist(PlannerInfo *root, List *vars,
                                   Relids where_needed);
extern List *deconstruct_jointree(PlannerInfo *root);
extern void process_implied_equality(PlannerInfo *root,
						 Node *item1, Node *item2,
						 Oid sortop1, Oid sortop2,
						 Relids item1_relids, Relids item2_relids,
						 bool delete_it);

/*
 * prototypes for plan/setrefs.c
 */
extern Plan *set_plan_references_qry(Plan *plan, Query *qry); 
extern List *set_returning_clause_references(List *rlist,
								Plan *topplan,
								Index resultRelation); 
extern void fix_opfuncids(Node *node);
extern void set_opfuncid(OpExpr *opexpr);
extern bool cdb_expr_requires_full_eval(Node *node);

extern int num_distcols_in_grouplist(List *gc);

#endif   /* PLANMAIN_H */
