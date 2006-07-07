/*-------------------------------------------------------------------------
 *
 * var.h
 *	  prototypes for optimizer/util/var.c.
 *
 *
 * Portions Copyright (c) 2006, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/var.h,v 1.33 2005/06/05 22:32:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

#include "nodes/relation.h"


typedef bool (*Cdb_walk_vars_callback_Aggref)(Aggref *aggref, void *context, int sublevelsup);
typedef bool (*Cdb_walk_vars_callback_Var)(Var *var, void *context, int sublevelsup);
bool        cdb_walk_vars(Node                         *node,
                          Cdb_walk_vars_callback_Var    callback_var,
                          Cdb_walk_vars_callback_Aggref callback_aggref,
                          void                         *context,
                          int                           levelsup);

extern Relids pull_varnos(Node *node);
extern Relids pull_varnos_of_level(Node *node, int levelsup);   /*CDB*/
extern bool contain_var_reference(Node *node, int varno, int varattno,
					  int levelsup);
extern bool contain_var_clause(Node *node);
extern bool contain_vars_of_level(Node *node, int levelsup);
extern bool contain_vars_above_level(Node *node, int levelsup);
extern int	find_minimum_var_level(Node *node);
extern List *pull_var_clause(Node *node, bool includeUpperVars);

extern Node *flatten_join_alias_vars(PlannerInfo *root, Node *node);

#endif   /* VAR_H */
