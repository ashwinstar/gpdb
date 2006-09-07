/*-------------------------------------------------------------------------
 *
 * pathnode.h
 *	  prototypes for pathnode.c, relnode.c.
 *
 *
 * Portions Copyright (c) 2005-2006, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/pathnode.h,v 1.62 2005/10/15 02:49:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODE_H
#define PATHNODE_H

#include "nodes/relation.h"
#include "cdb/cdbdef.h"                 /* CdbVisitOpt */

/*
 * prototypes for pathnode.c
 */
extern int compare_path_costs(Path *path1, Path *path2,
				   CostSelector criterion);
extern int compare_fractional_path_costs(Path *path1, Path *path2,
							  double fraction);
extern void set_cheapest(RelOptInfo *parent_rel);
extern void add_path(PlannerInfo *root, RelOptInfo *parent_rel, Path *new_path);

extern Path *create_seqscan_path(PlannerInfo *root, RelOptInfo *rel);
extern IndexPath *create_index_path(PlannerInfo *root,
				  IndexOptInfo *index,
				  List *clause_groups,
				  List *pathkeys,
				  ScanDirection indexscandir,
				  bool isjoininner);
extern BitmapHeapPath *create_bitmap_heap_path(PlannerInfo *root,
						RelOptInfo *rel,
						Path *bitmapqual,
						bool isjoininner);
extern BitmapAndPath *create_bitmap_and_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   List *bitmapquals);
extern BitmapOrPath *create_bitmap_or_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  List *bitmapquals);
extern TidPath *create_tidscan_path(PlannerInfo *root, RelOptInfo *rel,
					List *tideval);
extern AppendPath *create_append_path(RelOptInfo *rel, List *subpaths);
extern ResultPath *create_result_path(RelOptInfo *rel, Path *subpath,
				   List *quals);
extern MaterialPath *create_material_path(RelOptInfo *rel, Path *subpath);
extern UniquePath *create_unique_path(PlannerInfo *root, RelOptInfo *rel,
				   Path *subpath);
UniquePath *create_unique_rowid_path(PlannerInfo *root, 
                                     Path        *subpath, 
                                     Relids       dedup_relids);
UniquePath *create_unique_common_path(PlannerInfo *root,
                                      Path        *subpath,
                                      List        *distinct_on_exprs,
                                      Relids       dedup_relids);
extern Path *create_subqueryscan_path(PlannerInfo *root, RelOptInfo *rel, List *pathkeys);
extern Path *create_functionscan_path(PlannerInfo *root, RelOptInfo *rel);

extern NestPath *create_nestloop_path(PlannerInfo *root,
									  RelOptInfo *joinrel,
									  JoinType jointype,
									  Path *outer_path,
									  Path *inner_path,
									  List *restrict_clauses,
									  List *mergeclause_list,    /*CDB*/
									  List *pathkeys,
									  Path *alternate); /*CDB*/

extern MergePath *create_mergejoin_path(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  JoinType jointype,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  List *mergeclauses,
                      List *allmergeclauses,    /*CDB*/
					  List *outersortkeys,
					  List *innersortkeys);

extern HashPath *create_hashjoin_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
                     List *mergeclause_list,    /*CDB*/
					 List *hashclauses);

CdbVisitOpt
pathnode_walk_node(Path            *path,
			       CdbVisitOpt    (*walker)(Path *path, void *context),    
			       void            *context);
CdbVisitOpt
pathnode_walk_kids(Path            *path,
			       CdbVisitOpt    (*walker)(Path *path, void *context),    
			       void            *context);
CdbVisitOpt
pathnode_walk_list(List            *pathlist,
			       CdbVisitOpt    (*walker)(Path *path, void *context),    
			       void            *context);


/*
 * prototypes for relnode.c
 */
extern void build_base_rel(PlannerInfo *root, int relid);
extern RelOptInfo *build_other_rel(PlannerInfo *root, int relid);
extern RelOptInfo *find_base_rel(PlannerInfo *root, int relid);
extern RelOptInfo *find_join_rel(PlannerInfo *root, Relids relids);
extern RelOptInfo *build_join_rel(PlannerInfo *root,
			   Relids joinrelids,
			   RelOptInfo *outer_rel,
			   RelOptInfo *inner_rel,
			   JoinType jointype,
			   List **restrictlist_ptr);

#endif   /* PATHNODE_H */
