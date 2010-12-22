/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/tablecmds.h,v 1.31.2.1 2007/05/11 20:18:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "access/attnum.h"
#include "catalog/gp_policy.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "parser/parse_node.h"

extern Oid	DefineRelation(CreateStmt *stmt, char relkind, char relstorage);

extern void	DefineExternalRelation(CreateExternalStmt *stmt);

extern void DefineForeignRelation(CreateForeignStmt *createForeignStmt);

extern void	DefinePartitionedRelation(CreateStmt *stmt, Oid reloid);

extern void RemoveRelation(const RangeVar *relation, DropBehavior behavior,
						   DropStmt *stmt /* MPP */);

extern bool RelationToRemoveIsTemp(const RangeVar *relation, DropBehavior behavior);

extern void AlterTable(AlterTableStmt *stmt);

extern void ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing);

extern void AlterTableInternal(Oid relid, List *cmds, bool recurse);

extern void AlterTableNamespace(RangeVar *relation, const char *newschema);

extern void AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry);

extern void ATAddColumn(Relation rel, ColumnDef *colDef);

extern void CheckTableNotInUse(Relation rel, const char *stmt);

extern void ExecuteTruncate(TruncateStmt *stmt);

extern void renameatt(Oid myrelid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse,
		  bool recursing);

extern void renamerel(Oid myrelid,
		  const char *newrelname,
		  RenameStmt *stmt /* MPP */);

extern void find_composite_type_dependencies(Oid typeOid,
											 const char *origTblName,
											 const char *origTypeName);

extern AttrNumber *varattnos_map(TupleDesc old, TupleDesc new);
extern AttrNumber *varattnos_map_schema(TupleDesc old, List *schema);
extern void change_varattnos_of_a_node(Node *node, const AttrNumber *newattno);

extern void register_on_commit_action(Oid relid, OnCommitAction action);
extern void remove_on_commit_action(Oid relid);

extern void PreCommit_on_commit_actions(void);
extern void AtEOXact_on_commit_actions(bool isCommit);
extern void AtEOSubXact_on_commit_actions(bool isCommit,
							  SubTransactionId mySubid,
							  SubTransactionId parentSubid);
extern Oid selectPartition(PartitionNode *partnode, Datum *values,
						   bool *isnull, TupleDesc tupdesc,
						   PartitionState *pstate);
extern PartitionNode *RelationBuildPartitionDesc(Relation rel,
												 bool inctemplate);

/* RelationBuildPartitionDesc is built from get_parts */
extern PartitionNode *
get_parts(Oid relid, int2 level, Oid parent, bool inctemplate,
		  MemoryContext mcxt);

extern AttrNumber max_partition_attr(PartitionNode *pn);
extern List *get_partition_attrs(PartitionNode *pn);
extern int num_partition_levels(PartitionNode *pn);
extern bool rel_is_child_partition(Oid relid);
extern PartitionRule *ruleMakePartitionRule(HeapTuple tuple,
											TupleDesc tupdesc,
											MemoryContext mcxt);
extern Partition *partMakePartition(HeapTuple tuple,
									TupleDesc tupdesc,
									MemoryContext mcxt);
extern List *all_partition_relids(PartitionNode *pn);
extern bool rel_is_partitioned(Oid relid);
extern PgPartRule *get_part_rule1(Relation rel, AlterPartitionId *pid,
								  bool bExistError, bool bMustExist,
								  MemoryContext mcxt, int *pSearch,
								  PartitionNode *pNode,
								  char *relname, PartitionNode **ppNode);
extern PgPartRule *get_part_rule(Relation rel, AlterPartitionId *pid,
								 bool bExistError, bool bMustExist,
								 MemoryContext mcxt, int *pSearch,
								 bool inctemplate);
extern bool rel_is_child_partition(Oid relid);
extern bool partition_policies_equal(GpPolicy *p, PartitionNode *pn);
extern void partition_get_policies_attrs(PartitionNode *pn,
										 GpPolicy *master_policy,
							             List **cols);
extern bool rel_needs_long_lock(Oid relid);
extern bool rel_is_leaf_partition(Oid relid);
extern Oid  rel_partition_get_master(Oid relid);

extern void PartitionRangeItemIsValid(ParseState *pstate, PartitionRangeItem *pri);
#endif   /* TABLECMDS_H */
