/*-------------------------------------------------------------------------
 *
 * subselect.h
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/subselect.h,v 1.30 2008/01/01 19:45:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBSELECT_H
#define SUBSELECT_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"

Node *
convert_testexpr(PlannerInfo *root,
				 Node *testexpr,
				 List *subst_nodes);
List *
generate_subquery_vars(PlannerInfo *root, 
                       List        *tlist, 
                       Index        varno);
extern Node *SS_replace_correlation_vars(PlannerInfo *root, Node *expr);
extern Node *SS_process_sublinks(PlannerInfo *root, Node *expr, bool isQual);
extern void SS_finalize_plan(PlannerInfo *root, Plan *plan);
extern Param *SS_make_initplan_from_plan(PlannerInfo *root, Plan *plan,
						   Oid resulttype, int32 resulttypmod);
extern bool has_correlation_params(List *planner_param_items); /* MPP */

#endif   /* SUBSELECT_H */
