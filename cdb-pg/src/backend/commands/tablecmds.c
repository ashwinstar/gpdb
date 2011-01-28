/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  Commands for creating and altering table structures and settings
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/tablecmds.c,v 1.206.2.5 2008/05/09 22:37:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <fcntl.h>
#include <locale.h>
#include <unistd.h>

#include "access/appendonlywriter.h"
#include "access/bitmap.h"
#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_appendonly.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_exttable.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_resqueue.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_partition.h"
#include "catalog/pg_partition_rule.h"
#include "catalog/toasting.h"
#include "cdb/cdbappendonlyam.h"
#include "cdb/cdbaocsam.h"
#include "commands/cluster.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/print.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "storage/backendid.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/uri.h"
#include "mb/pg_wchar.h"

#include "cdb/cdbdisp.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbrelsize.h"
#include "cdb/cdboidsync.h"
#include "cdb/cdbsreh.h"

#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbmirroredbufferpool.h"
#include "cdb/cdbmirroredappendonly.h"
#include "cdb/cdbpersistentfilesysobj.h"

typedef enum
{
	PART_STATUS_NONE,	/* a regular table */
	PART_STATUS_ROOT,	/* a partitioned table */
	PART_STATUS_INTERIOR,	/* a non-leaf part of a partitioned table */
	PART_STATUS_LEAF	/* a leaf part of a partitioned table */
} PartStatus;


/*
 * ON COMMIT action list
 */
typedef struct OnCommitItem
{
	Oid			relid;			/* relid of relation */
	OnCommitAction oncommit;	/* what to do at end of xact */

	/*
	 * If this entry was created during the current transaction,
	 * creating_subid is the ID of the creating subxact; if created in a prior
	 * transaction, creating_subid is zero.  If deleted during the current
	 * transaction, deleting_subid is the ID of the deleting subxact; if no
	 * deletion request is pending, deleting_subid is zero.
	 */
	SubTransactionId creating_subid;
	SubTransactionId deleting_subid;
} OnCommitItem;

static List *on_commits = NIL;

/*
 * State information for ALTER TABLE
 *
 * The pending-work queue for an ALTER TABLE is a List of AlteredTableInfo
 * structs, one for each table modified by the operation (the named table
 * plus any child tables that are affected).  We save lists of subcommands
 * to apply to this table (possibly modified by parse transformation steps);
 * these lists will be executed in Phase 2.  If a Phase 3 step is needed,
 * necessary information is stored in the constraints and newvals lists.
 *
 * Phase 2 is divided into multiple passes; subcommands are executed in
 * a pass determined by subcommand type.
 */

#define AT_PASS_DROP			0		/* DROP (all flavors) */
#define AT_PASS_ALTER_TYPE		1		/* ALTER COLUMN TYPE */
#define AT_PASS_OLD_INDEX		2		/* re-add existing indexes */
#define AT_PASS_OLD_CONSTR		3		/* re-add existing constraints */
#define AT_PASS_COL_ATTRS		4		/* set other column attributes */
/* We could support a RENAME COLUMN pass here, but not currently used */
#define AT_PASS_ADD_COL			5		/* ADD COLUMN */
#define AT_PASS_ADD_INDEX		6		/* ADD indexes */
#define AT_PASS_ADD_CONSTR		7		/* ADD constraints, defaults */
#define AT_PASS_MISC			8		/* other stuff */
#define AT_NUM_PASSES			9

typedef struct AlteredTableInfo
{
	/* Information saved before any work commences: */
	Oid			relid;			/* Relation to work on */
	char		relkind;		/* Its relkind */
	TupleDesc	oldDesc;		/* Pre-modification tuple descriptor */
	/* Information saved by Phase 1 for Phase 2: */
	List	   *subcmds[AT_NUM_PASSES]; /* Lists of AlterTableCmd */
	/* Information saved by Phases 1/2 for Phase 3: */
	List	   *constraints;	/* List of NewConstraint */
	List	   *newvals;		/* List of NewColumnValue */
	bool		new_notnull;	/* T if we added new NOT NULL constraints */
	bool		new_dropoids;	/* T if we dropped the OID column */
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
	Oid			exchange_relid;	/* for EXCHANGE, the exchanged in rel */
	/* Objects to rebuild after completing ALTER TYPE operations */
	List	   *changedConstraintOids;	/* OIDs of constraints to rebuild */
	List	   *changedConstraintDefs;	/* string definitions of same */
	List	   *changedIndexOids;		/* OIDs of indexes to rebuild */
	List	   *changedIndexDefs;		/* string definitions of same */
} AlteredTableInfo;

/* Struct describing one new constraint to check in Phase 3 scan */
/* Note: new NOT NULL constraints are handled elsewhere */
typedef struct NewConstraint
{
	char	   *name;			/* Constraint name, or NULL if none */
	ConstrType	contype;		/* CHECK or FOREIGN */
	Oid			refrelid;		/* PK rel, if FOREIGN */
	Node	   *qual;			/* Check expr or FkConstraint struct */
	List	   *qualstate;		/* Execution state for CHECK */
} NewConstraint;

/*
 * Struct describing one new column value that needs to be computed during
 * Phase 3 copy (this could be either a new column with a non-null default, or
 * a column that we're changing the type of).  Columns without such an entry
 * are just copied from the old table during ATRewriteTable.  Note that the
 * expr is an expression over *old* table values.
 */
typedef struct NewColumnValue
{
	AttrNumber	attnum;			/* which column */
	Expr	   *expr;			/* expression to compute */
	ExprState  *exprstate;		/* execution state */
} NewColumnValue;

/* cache of function lookups for range partition selection */
typedef struct PartitionRangeState
{
	FmgrInfo *ltfuncs;
	bool *ltinit;
	FmgrInfo *lefuncs;
	bool *leinit;
	int last_rule; /* cache offset to the last rule and test if it matches */
	PartitionRule **rules;
} PartitionRangeState;

/* likewise, for list */
typedef struct PartitionListState
{
	FmgrInfo *eqfuncs;
	bool *eqinit;
} PartitionListState;

/* likewise, for hash */
typedef struct PartitionHashState
{
	FmgrInfo *hashfuncs;
	bool *hashinit;
} PartitionHashState;

/* part rule update */
typedef struct part_rule_cxt
{
	Oid old;
	Oid new;
} part_rule_cxt;

static void truncate_check_rel(Relation rel);
static List *MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr, int *supOidCount, GpPolicy *policy);
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel);
static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel, List *inhAttrNameList,
										bool is_partition);
static void add_nonduplicate_constraint(Constraint *cdef,
										ConstrCheck *check, int *ncheck);
static bool change_varattnos_walker(Node *node, const AttrNumber *newattno);
static void StoreCatalogInheritance(Oid relationId, List *supers);
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int16 seqNumber, Relation inhRelation,
						 bool is_partition);
static int	findAttrByName(const char *attributeName, List *schema);
static void setRelhassubclassInRelation(Oid relationId, bool relhassubclass);
static void AlterIndexNamespaces(Relation classRel, Relation rel,
					 Oid oldNspOid, Oid newNspOid);
static void AlterSeqNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid,
				   const char *newNspName);
static int transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids);
static int transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids,
						   Oid *opclasses);
static Oid transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						Oid *opclasses);
static void validateForeignKeyConstraint(FkConstraint *fkconstraint,
							 Relation rel, Relation pkrel);
static void createForeignKeyTriggers(Relation rel, FkConstraint *fkconstraint,
						 Oid constrOid);
static char *fkMatchTypeToString(char match_type);
static void ATController(Relation rel, List *cmds, bool recurse, 
						 int * oidInfoCount, TableOidInfo ** oidInfo, List **poidmap);
static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing);
static void ATRewriteCatalogs(List **wqueue, 
							  int oidInfoCount, TableOidInfo * oidInfo, List **poidmap);
static void ATAddToastIfNeeded(List **wqueue, 
							   int oidInfoCount,TableOidInfo * oidInfo, List **poidmap);
static void ATExecCmd(List **wqueue,
					  AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd);
static void ATRewriteTables(List **wqueue, 
							int oidInfoCount, TableOidInfo * oidInfo, List **poidmap);
static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap);
static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
static void ATSimplePermissions(Relation rel, bool allowView);
static void ATSimplePermissionsRelationOrIndex(Relation rel);
static void ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse);
static void ATOneLevelRecursion(List **wqueue, Relation rel,
					AlterTableCmd *cmd);
static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse,
				AlterTableCmd *cmd);
static void ATExecAddColumn(AlteredTableInfo *tab, Relation rel,
				ColumnDef *colDef);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void ATExecDropNotNull(Relation rel, const char *colName);
static void ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName);
static void ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault);
static void ATPrepSetStatistics(Relation rel, const char *colName,
					Node *flagValue);
static void ATExecSetStatistics(Relation rel, const char *colName,
					Node *newValue);
static void ATExecSetStorage(Relation rel, const char *colName,
				 Node *newValue);
static void ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing);
static void ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild, bool part_expanded);
static void ATExecAddConstraint(AlteredTableInfo *tab, Relation rel,
					Node *newConstraint);
static void ATAddForeignKeyConstraint(AlteredTableInfo *tab, Relation rel,
						  FkConstraint *fkconstraint);
static void ATPrepDropConstraint(List **wqueue, Relation rel,
					 bool recurse, AlterTableCmd *cmd);
static void ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior, bool quiet);
static void ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd);
static void ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  const char *colName, TypeName *typname);
static void ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab);
static void ATPostAlterTypeParse(char *cmd, List **wqueue);
static void change_owner_recurse_to_sequences(Oid relationOid,
								  Oid newOwnerId);
static void ATExecClusterOn(Relation rel, const char *indexName);
static void ATExecDropCluster(Relation rel);
static void ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel,
					char *tablespacename);
static void ATPartsPrepSetTableSpace(List **wqueue, Relation rel, AlterTableCmd *cmd, 
									 List *oids);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, 
								TableOidInfo *oidInfo);
static void ATExecSetRelOptions(Relation rel, List *defList, bool isReset);
static void ATExecEnableDisableTrigger(Relation rel, char *trigname,
						   bool enable, bool skip_system);
static void ATExecAddInherit(Relation rel, Node *node);
static void ATExecDropInherit(Relation rel, RangeVar *parent, bool is_partition);
static void ATExecSetDistributedBy(Relation rel, Node *node,
								   AlterTableCmd *cmd);
static List *all_prule_relids(PartitionRule *prule);
static PartStatus rel_part_status(Oid relid);
static bool can_implement_dist_on_part(Relation rel, List *dist_cnames);
static void ATPrepExchange(Relation rel, AlterPartitionCmd *pc);
static bool is_exchangeable(Relation rel, Relation oldrel, Relation newrel, bool throw);

static const char* synthetic_sql = "(internally generated SQL command)";

/* ALTER TABLE ... PARTITION */
static char *
ChoosePartitionName(const char *tablename, int partDepth,
					const char *partname, Oid namespace);

static Node *atpxPartAddList(AlteredTableInfo *tab,
							 Relation rel, AlterPartitionCmd *pc,
							 PartitionNode *pNode, Node *pUtl,
							 Node *partName, bool isDefault,
							 PartitionElem *pelem,
							 PartitionByType part_type,
							 PgPartRule* par_prule,
							 char *lrelname,
							 bool bSetTemplate,
							 Oid ownerid);

static void ATPExecPartAdd(AlteredTableInfo *tab,
						   Relation rel,
                           AlterPartitionCmd *pc,
						   AlterTableType att);				/* Add */
static void ATPExecPartAlter(List **wqueue, AlteredTableInfo *tab, 
							 Relation rel,
                             AlterPartitionCmd *pc);		/* Alter */
static void ATPExecPartCoalesce(Relation rel,
                                AlterPartitionCmd *pc);		/* Coalesce */
static void ATPExecPartDrop(Relation rel,
                            AlterPartitionCmd *pc);			/* Drop */
static void ATPExecPartExchange(AlteredTableInfo *tab,
								Relation rel,
                                AlterPartitionCmd *pc);		/* Exchange */
static void ATPExecPartMerge(Relation rel,
                             AlterPartitionCmd *pc);		/* Merge */
static void ATPExecPartModify(Relation rel,
                              AlterPartitionCmd *pc);		/* Modify */
static void ATPExecPartRename(Relation rel,
                              AlterPartitionCmd *pc);		/* Rename */

static void ATPExecPartSetTemplate(AlteredTableInfo *tab,   /* Set */
								   Relation rel,            /* Subpartition */
                                   AlterPartitionCmd *pc);	/* Template */
static void ATPExecPartSplit(Relation rel,
                             AlterPartitionCmd *pc);		/* Split */
static List *
atpxTruncateList(Relation rel, PartitionNode *pNode);
static void ATPExecPartTruncate(Relation rel,
                                AlterPartitionCmd *pc);		/* Truncate */
static void
copy_buffer_pool_data(
	Relation 	rel,

	SMgrRelation dst,

	ItemPointer persistentTid,

	int64 		persistentSerialNum,

	bool		useWal);
static void update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname);
static Datum transformLocationUris(List *locs, bool isweb, bool iswritable);
static Datum transformExecOnClause(List	*on_clause);
static char transformFormatType(char *formatname);
static Datum transformFormatOpts(char formattype, List *formatOpts, int numcols, bool iswritable);

static Oid DefineRelation_int(CreateStmt *stmt, char relkind, char relstorage, CdbDispatcherState *ds);

static void ATExecPartAddInternal(Relation rel, Node *def);
static void exchange_part_rule(Oid oldrelid, Oid newrelid);
static List *exchange_part_constraints(List *pcols, 
									   Relation oldrel, AttrMap *oldmap, 
									   Relation newrel, AttrMap *newmap,
									   bool validate);

static void
AlterRelationNamespaceInternalTwo(Relation rel,
								  Oid relid,
								  Oid oldNspOid, Oid newNspOid,
								  bool hasDependEntry,
								  const char *newschema);

static List *get_part_constraints(List *part, Relation pgcon, Relation rel);

static List *rel_get_part_path(Oid relid);
static char *rel_get_part_path_pretty(Oid relid,
									  char *separator,
									  char *lastsep);

static List *atpxRenameList(Relation rel, PartitionNode *pNode,
							char *old_parentname, const char *new_parentname,
							int *skipped);
static RangeVar *make_temp_table_name(Relation rel, BackendId id);
static bool prebuild_temp_table(Relation rel, RangeVar *tmpname, List *distro,
								List *opts, List **hidden_types, bool isTmpTableAo);
static void ATPartitionCheck(AlterTableType subtype, Relation rel, bool rejectroot, bool recursing);
static Oid get_settable_tablespace_oid(char *tablespacename);
static List *make_dist_clause(Relation rel);

#define parkindIsHash(c)   ((c) == 'h')
#define parkindIsRange(c)  ((c) == 'r')
#define parkindIsList(c)   ((c) == 'l')

static char *alterTableCmdString(AlterTableType subtype);

/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 *
 * If successful, returns the OID of the new relation.
 * ----------------------------------------------------------------
 */
Oid
DefineRelation(CreateStmt *stmt, char relkind, char relstorage)
{
	volatile struct CdbDispatcherState ds = {NULL, NULL};

    Oid reloid = 0;
    Assert(stmt->relation->schemaname == NULL || strlen(stmt->relation->schemaname)>0);

    PG_TRY();
    {
        reloid = DefineRelation_int(stmt, relkind, relstorage, (struct CdbDispatcherState *)&ds);
    }
	PG_CATCH();
	{
        /* If dispatched, stop QEs and clean up after them. */
        if (ds.primaryResults)
            cdbdisp_handleError((struct CdbDispatcherState *)&ds);

        /* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * We successfully completed our work.	Now check the results from the
	 * qExecs, if dispatched.  This waits for them to all finish, and exits
	 * via ereport(ERROR,...) if unsuccessful.
	 */
	cdbdisp_finishCommand((struct CdbDispatcherState *)&ds, NULL, NULL);

    return reloid;
}


Oid
DefineRelation_int(CreateStmt *stmt,
                   char relkind,
                   char relstorage,
				   CdbDispatcherState *ds)
{
	char		relname[NAMEDATALEN];
	Oid			namespaceId;
	List	   *schema = stmt->tableElts;
	Oid			relationId = InvalidOid;
	Oid			tablespaceId;
	Relation	rel;
	TupleDesc	descriptor;
	List	   *inheritOids;
	List	   *old_constraints;
	bool		localHasOids;
	int			parentOidCount;
	List	   *rawDefaults;
	Datum		reloptions;
	ListCell   *listptr;
	AttrNumber	attnum;
	bool		shared_relation = false;

	bool		shouldDispatch = Gp_role == GP_ROLE_DISPATCH &&
                                 IsNormalProcessingMode() &&
                                 relkind != RELKIND_SEQUENCE &&
                                 relkind != RELKIND_VIEW;

	/*
	 * Truncate relname to appropriate length (probably a waste of time, as
	 * parser should have done this already).
	 */
	StrNCpy(relname, stmt->relation->relname, NAMEDATALEN);

	/*
	 * Check consistency of arguments
	 */
	if (stmt->oncommit != ONCOMMIT_NOOP && !stmt->relation->istemp)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("ON COMMIT can only be used on temporary tables"),
						   errOmitLocation(true)));

	/*
	 * Look up the namespace in which we are supposed to create the relation.
	 * Check we have permission to create there. Skip check if bootstrapping,
	 * since permissions machinery may not be working yet.
	 */
	namespaceId = RangeVarGetCreationNamespace(stmt->relation);

	if (!IsBootstrapProcessingMode())
	{
		AclResult	aclresult;

		aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(),
										  ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(namespaceId));
	}

	/*
	 * Select tablespace to use.  If not specified, use default_tablespace
	 * (which may in turn default to database's default).
	 *
	 * Note: This code duplicates code in indexcmds.c
	 */
	if (relkind == RELKIND_SEQUENCE || 
		relkind == RELKIND_VIEW ||
		relkind == RELKIND_COMPOSITE_TYPE ||
		(relkind == RELKIND_RELATION && (
			relstorage == RELSTORAGE_EXTERNAL ||
			relstorage == RELSTORAGE_FOREIGN  ||
			relstorage == RELSTORAGE_VIRTUAL)))
	{
		/* 
		 * MPP-8262: unable to create sequence with default_tablespace
		 *
		 * These relkinds have no storage, and thus do not support tablespaces.
		 * We shouldn't go through the regular default case for these because we
		 * don't want to pick up the value from the default_tablespace guc.
		 */
		Assert(!stmt->tablespacename);
		tablespaceId = InvalidOid;
	}
	else if (stmt->tablespacename)
	{
		/*
		 * Tablespace specified on the command line, or was passed down by
		 * dispatch.
		 */
		tablespaceId = get_tablespace_oid(stmt->tablespacename);
		if (!OidIsValid(tablespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							stmt->tablespacename),
					 errOmitLocation(true)));
	}
	else
	{
		/*
		 * Get the default tablespace specified via default_tablespace, or fall
		 * back on the database tablespace.
		 */
		tablespaceId = GetDefaultTablespace();
		
		/* Need the real tablespace id for dispatch */
		if (!OidIsValid(tablespaceId)) 
			tablespaceId = MyDatabaseTableSpace;

		/* 
		 * MPP-8238 : inconsistent tablespaces between segments and master 
		 */
		if (shouldDispatch)
			stmt->tablespacename = get_tablespace_name(tablespaceId);
	}

	/*
	 * Parse and validate reloptions, if any.
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options, true, false);

	/* Check permissions except when using database's default */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/*
	 * Look up inheritance ancestors and generate relation schema, including
	 * inherited attributes. Update the offsets of the distribution attributes
	 * in GpPolicy if necessary.
	 */
	schema = MergeAttributes(schema, stmt->inhRelations,
							 stmt->relation->istemp,
							 &inheritOids, &old_constraints, &parentOidCount, stmt->policy);

	/*
	 * Create a relation descriptor from the relation schema and create the
	 * relation.  Note that in this stage only inherited (pre-cooked) defaults
	 * and constraints will be included into the new relation.
	 * (BuildDescForRelation takes care of the inherited defaults, but we have
	 * to copy inherited constraints here.)
	 */
	descriptor = BuildDescForRelation(schema);

	localHasOids = interpretOidsOption(stmt->options);
	descriptor->tdhasoid = (localHasOids || parentOidCount > 0);

	if (old_constraints || stmt->constraints)
	{
		ConstrCheck *check;
		int			ncheck = 0;

		/* make array that's certainly big enough */
		check = (ConstrCheck *)
			palloc((list_length(old_constraints) +
					list_length(stmt->constraints)) * sizeof(ConstrCheck));
		/* deal with constraints from MergeAttributes */
		foreach(listptr, old_constraints)
		{
			Constraint *cdef = (Constraint *) lfirst(listptr);

			if (cdef->contype == CONSTR_CHECK)
				add_nonduplicate_constraint(cdef, check, &ncheck);
		}
		/*
		 * analyze.c might have passed some precooked constraints too,
		 * due to LIKE tab INCLUDING CONSTRAINTS
		 */
		foreach(listptr, stmt->constraints)
		{
			Constraint *cdef = (Constraint *) lfirst(listptr);

			if (cdef->contype == CONSTR_CHECK && cdef->cooked_expr != NULL)
				add_nonduplicate_constraint(cdef, check, &ncheck);
		}
		/* if we found any, insert 'em into the descriptor */
		if (ncheck > 0)
		{
			if (descriptor->constr == NULL)
			{
				descriptor->constr = (TupleConstr *) palloc(sizeof(TupleConstr));
				descriptor->constr->defval = NULL;
				descriptor->constr->num_defval = 0;
				descriptor->constr->has_not_null = false;
			}
			descriptor->constr->num_check = ncheck;
			descriptor->constr->check = check;
		}
	}


	if (shouldDispatch)
	{
		Oid			toastRelationId = InvalidOid;
		Oid			aosegRelationId = InvalidOid;
		Oid			aoblkdirRelationId = InvalidOid;
		Relation	pg_class_desc;
		Relation	pg_type_desc;
		Oid 		comptypeOid = InvalidOid;
		Oid			toastIndexId = InvalidOid;
		Oid			aosegIndexId = InvalidOid;
		Oid			aoblkdirIndexId = InvalidOid;

		MemoryContext oldContext;

		/*
		 * We use a RowExclusiveLock but hold it till end of transaction so that two
		 * DDL operations will not deadlock between QEs
		 */
		pg_class_desc = heap_open(RelationRelationId, RowExclusiveLock);
		pg_type_desc = heap_open(TypeRelationId, RowExclusiveLock);

		LockRelationOid(DependRelationId, RowExclusiveLock);

		cdb_sync_oid_to_segments();

		/*
		 * Pre-Allocate an OID for the relfilenode, plus one for the toast table
		 * and another for an aoseg table (just in case).
		 *
		 * The OID will be the relfilenode as well, so make sure it doesn't collide
		 * with either pg_class OIDs or existing physical files.
		 */
		relationId = GetNewRelFileNode(tablespaceId, false,  pg_class_desc);
		comptypeOid = GetNewRelFileNode(tablespaceId, false,  pg_type_desc);
		toastRelationId = GetNewRelFileNode(tablespaceId, false,  pg_class_desc);
		toastIndexId = GetNewRelFileNode(tablespaceId, false,  pg_class_desc);
		aosegRelationId = GetNewRelFileNode(tablespaceId, false,  pg_class_desc);
		aosegIndexId = GetNewRelFileNode(tablespaceId, false,  pg_class_desc);
		aoblkdirRelationId = GetNewRelFileNode(tablespaceId, false,  pg_class_desc);
		aoblkdirIndexId = GetNewRelFileNode(tablespaceId, false,  pg_class_desc);

		heap_close(pg_class_desc, NoLock);  /* gonna update, so don't unlock */

		stmt->oidInfo.relOid = relationId;
		stmt->oidInfo.comptypeOid = comptypeOid;
		stmt->oidInfo.toastOid = toastRelationId;
		stmt->oidInfo.toastIndexOid = toastIndexId;
		stmt->oidInfo.toastComptypeOid = GetNewRelFileNode(tablespaceId, false,  pg_type_desc);
		stmt->oidInfo.aosegOid = aosegRelationId;
		stmt->oidInfo.aosegIndexOid = aosegIndexId;
		stmt->oidInfo.aosegComptypeOid = GetNewRelFileNode(tablespaceId, false,  pg_type_desc);
		stmt->oidInfo.aoblkdirOid = aoblkdirRelationId;
		stmt->oidInfo.aoblkdirIndexOid = aoblkdirIndexId;
		stmt->oidInfo.aoblkdirComptypeOid = GetNewRelFileNode(tablespaceId, false,  pg_type_desc);

		stmt->relKind = relkind;
		stmt->relStorage = relstorage;

		if (!OidIsValid(stmt->ownerid))
			stmt->ownerid = GetUserId();

		oldContext = MemoryContextSwitchTo(CacheMemoryContext);
		stmt->relation->schemaname = get_namespace_name(namespaceId);
		MemoryContextSwitchTo(oldContext);

		heap_close(pg_type_desc, NoLock);
	}
	else if (Gp_role == GP_ROLE_EXECUTE)
	{
		Assert(stmt->oidInfo.relOid != 0);
		relationId = stmt->oidInfo.relOid;
	}
	else
	{
		stmt->oidInfo.toastOid = InvalidOid;
		stmt->oidInfo.toastIndexOid = InvalidOid;
		stmt->oidInfo.aosegOid = InvalidOid;
		stmt->oidInfo.aosegIndexOid = InvalidOid;
		stmt->oidInfo.aoblkdirOid = InvalidOid;
		stmt->oidInfo.aoblkdirIndexOid = InvalidOid;
		stmt->ownerid = GetUserId();
	}

	/* MPP-8405: disallow OIDS on partitioned tables */
	if ((stmt->partitionBy ||
		 stmt->is_part_child) &&
		descriptor->tdhasoid && 
		IsNormalProcessingMode() &&
        (Gp_role == GP_ROLE_DISPATCH))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg(
							 "OIDS=TRUE is not allowed on partitioned tables. "
							 "Use OIDS=FALSE"
							 ),
					 errOmitLocation(true)));

    if (stmt->oidInfo.relOid)
        elog(DEBUG4, "DefineRelation relOid=%d schemaname=%s shouldDispatch=%d",
             stmt->oidInfo.relOid,
             stmt->relation->schemaname ? stmt->relation->schemaname : "",
             shouldDispatch);

	relationId = heap_create_with_catalog(relname,
										  namespaceId,
										  tablespaceId,
										  stmt->oidInfo.relOid,
										  stmt->ownerid,
										  descriptor,
										  /* relam */ InvalidOid,
										  relkind,
										  relstorage,
										  shared_relation,
										  localHasOids,
										  /* bufferPoolBulkLoad */ false,
										  parentOidCount,
										  stmt->oncommit,
                                          stmt->policy,  /*CDB*/
                                          reloptions,
										  allowSystemTableMods,
										  &stmt->oidInfo.comptypeOid,
										  /* persistentTid */ NULL,
										  /* persistentSerialNum */ NULL);

	StoreCatalogInheritance(relationId, inheritOids);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire exclusive lock on it, unless we're
	 * doing partition.
	 *
	 * This isn't even necessary in the case of normal relations since other
	 * backends can't see the new rel anyway until we commit), but it keeps
	 * the lock manager from complaining about deadlock risks.
	 */
	if (stmt->is_part_child)
		rel = relation_open(relationId, NoLock);
	else
		rel = relation_open(relationId, AccessExclusiveLock);

	/*
	 * Now add any newly specified column default values and CHECK constraints
	 * to the new relation.  These are passed to us in the form of raw
	 * parsetrees; we need to transform them to executable expression trees
	 * before they can be added. The most convenient way to do that is to
	 * apply the parser's transformExpr routine, but transformExpr doesn't
	 * work unless we have a pre-existing relation. So, the transformation has
	 * to be postponed to this final step of CREATE TABLE.
	 *
	 * First, scan schema to find new column defaults.
	 */
	rawDefaults = NIL;
	attnum = 0;

	foreach(listptr, schema)
	{
		ColumnDef  *colDef = lfirst(listptr);

		attnum++;

		if (colDef->raw_default != NULL)
		{
			RawColumnDefault *rawEnt;

			Assert(colDef->cooked_default == NULL);

			rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
			rawEnt->attnum = attnum;
			rawEnt->raw_default = colDef->raw_default;
			rawDefaults = lappend(rawDefaults, rawEnt);
		}
	}

	/*
	 * Parse and add the defaults/constraints, if any.
	 */
	if (rawDefaults || stmt->constraints)
		AddRelationRawConstraints(rel, rawDefaults, stmt->constraints);

	/* It is now safe to dispatch */
	if (shouldDispatch)
	{

		/* Dispatch the statement tree to all primary and mirror segdbs.
		 * Doesn't wait for the QEs to finish execution.
		 */
		cdbdisp_dispatchUtilityStatement((Node *)stmt,
										 true,      /* cancelOnError */
										 true,      /* startTransaction */
										 true,      /* withSnapshot */
										 ds,
										 "DefineRelation_int");

	}

	/*
	 * Clean up.  We keep lock on new relation (although it shouldn't be
	 * visible to anyone else anyway, until commit).
	 */
	relation_close(rel, NoLock);

	return relationId;
}

/* ----------------------------------------------------------------
*		DefineExternalRelation
*				Creates a new external relation.
*
* In here we first dispatch a normal DefineRelation() (with relstorage
* external) in order to create the external relation entries in pg_class
* pg_type etc. Then once this is done we dispatch ourselves (DefineExternalRelation)
* in order to create the pg_exttable entry accross the gp array and we
* also record a dependency with the error table, if one exists.
*
* Why don't we just do all of this in one dispatch run? because that
* involves duplicating the DefineRelation() code or severly modifying it
* to have special cases for external tables. IMHO it's better and cleaner
* to leave it intact and do another dispatch.
* ----------------------------------------------------------------
*/
extern void
DefineExternalRelation(CreateExternalStmt *createExtStmt)
{
	volatile struct CdbDispatcherState ds = {NULL, NULL};
	CreateStmt				  *createStmt = makeNode(CreateStmt);
	ExtTableTypeDesc 		  *exttypeDesc = (ExtTableTypeDesc *)createExtStmt->exttypedesc;
	SingleRowErrorDesc 		  *singlerowerrorDesc = NULL;
	DefElem    				  *dencoding = NULL;
	ListCell				  *option;
	Oid						  reloid = 0;
	Oid						  fmtErrTblOid = InvalidOid;
	Datum					  formatOptStr;
	Datum					  locationUris = 0;
	Datum					  locationExec = 0;
	char*					  commandString = NULL;
	char					  rejectlimittype = '\0';
	char					  formattype;
	int						  rejectlimit = -1;
	int						  encoding = -1;
	bool					  issreh = false; /* is single row error handling requested? */
	bool					  iswritable = createExtStmt->iswritable;
	bool					  isweb = createExtStmt->isweb;
	bool					  shouldDispatch = (Gp_role == GP_ROLE_DISPATCH &&
												IsNormalProcessingMode());
	
	/*
	 * now set the parameters for keys/inheritance etc. Most of these are
	 * uninteresting for external relations...
	 */
	createStmt->relation = createExtStmt->relation;
	createStmt->tableElts = createExtStmt->tableElts;
	createStmt->inhRelations = NIL;
	createStmt->constraints = NIL;
	createStmt->options = NIL;
	createStmt->oncommit = ONCOMMIT_NOOP;
	createStmt->tablespacename = NULL;
	createStmt->policy = createExtStmt->policy; /* policy was set in transform */
	
		
	switch(exttypeDesc->exttabletype)
	{
		case EXTTBL_TYPE_LOCATION:

			/* Parse and validate URI strings (LOCATION clause) */
			locationUris = transformLocationUris(exttypeDesc->location_list,
												 isweb, iswritable);
			break;

		case EXTTBL_TYPE_EXECUTE:
			locationExec = transformExecOnClause(exttypeDesc->on_clause);
			commandString = exttypeDesc->command_string;

			if(strlen(commandString) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("Invalid EXECUTE clause. Found an empty command string"),
								   errOmitLocation(true)));

			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Internal error: unknown external table type"),
							   errOmitLocation(true)));
	}

	/*
	 * check permissions to create this external table.
	 *
	 * - Always allow if superuser.
	 * - Never allow EXECUTE or 'file' exttables if not superuser.
	 * - Allow http or gpfdist tables if pg_auth has the right permissions
	 *   for this role and for this type of table, or if gp_external_grant_privileges 
	 *   is on (gp_external_grant_privileges should be deprecated at some point).
	 */
	if(!superuser() && Gp_role == GP_ROLE_DISPATCH)
	{		
		if(exttypeDesc->exttabletype == EXTTBL_TYPE_EXECUTE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to create an EXECUTE external web table"),
							   errOmitLocation(true)));
		}
		else
		{
			ListCell   *first_uri = list_head(exttypeDesc->location_list);
			Value		*v = lfirst(first_uri);
			char		*uri_str = pstrdup(v->val.str);
			Uri			*uri = ParseUriString(uri_str);

			Assert(exttypeDesc->exttabletype == EXTTBL_TYPE_LOCATION);

			if(uri->protocol == URI_FILE)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be superuser to create an external table with a file protocol"),
								   errOmitLocation(true)));
			}
			else if(!gp_external_grant_privileges)
			{
				/*
				 * Check if this role has the proper 'gpfdist' or 'http'
				 * permissions in pg_auth for creating this table.
				 */

				bool		 isnull;				
				Oid			 userid = GetUserId();
				Relation	 pg_authid_rel = heap_open(AuthIdRelationId, RowExclusiveLock);
				TupleDesc	 pg_authid_dsc = RelationGetDescr(pg_authid_rel);
				HeapTuple	 tuple = 		 SearchSysCache(AUTHOID,
											 ObjectIdGetDatum(userid),
											 0, 0, 0);
								
				if (!HeapTupleIsValid(tuple))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("role \"%s\" does not exist (in DefineExternalRelation)", 
									 GetUserNameFromId(userid))));

				if (uri->protocol == URI_GPFDIST && iswritable)
				{
					Datum 	d_wextgpfd = heap_getattr(tuple, Anum_pg_authid_rolcreatewextgpfd, pg_authid_dsc, &isnull);
					bool	createwextgpfd = (isnull ? false : DatumGetBool(d_wextgpfd));
					
					if (!createwextgpfd)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								errmsg("permission denied: no privilege to create a writable gpfdist external table"),
										errOmitLocation(true)));
				}
				else if (uri->protocol == URI_GPFDIST && !iswritable)
				{
					Datum 	d_rextgpfd = heap_getattr(tuple, Anum_pg_authid_rolcreaterextgpfd, pg_authid_dsc, &isnull);
					bool	createrextgpfd = (isnull ? false : DatumGetBool(d_rextgpfd));

					if (!createrextgpfd)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								errmsg("permission denied: no privilege to create a readable gpfdist external table"),
										errOmitLocation(true)));

				}
				else if (uri->protocol == URI_GPHDFS && iswritable)
				{
					Datum 	d_wexthdfs = heap_getattr(tuple, Anum_pg_authid_rolcreatewexthdfs, pg_authid_dsc, &isnull);
					bool	createwexthdfs = (isnull ? false : DatumGetBool(d_wexthdfs));

					if (!createwexthdfs)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								errmsg("permission denied: no privilege to create a writable gphdfs external table"),
										errOmitLocation(true)));
				}
				else if (uri->protocol == URI_GPHDFS && !iswritable)
				{
					Datum 	d_rexthdfs = heap_getattr(tuple, Anum_pg_authid_rolcreaterexthdfs, pg_authid_dsc, &isnull);
					bool	createrexthdfs = (isnull ? false : DatumGetBool(d_rexthdfs));

					if (!createrexthdfs)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								errmsg("permission denied: no privilege to create a readable gphdfs external table"),
										errOmitLocation(true)));

				}
				else if (uri->protocol == URI_HTTP && !iswritable)
				{
					Datum 	d_exthttp = heap_getattr(tuple, Anum_pg_authid_rolcreaterexthttp, pg_authid_dsc, &isnull);
					bool	createrexthttp = (isnull ? false : DatumGetBool(d_exthttp));

					if (!createrexthttp)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								errmsg("permission denied: no privilege to create an http external table"),
										errOmitLocation(true)));
				}
				else
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("internal error in DefineExternalRelation. "
								   "protocol is %d, writable is %d", 
								   uri->protocol, iswritable)));
				}
				
				ReleaseSysCache(tuple);
				heap_close(pg_authid_rel, RowExclusiveLock);
			}
			pfree(uri);
			pfree(uri_str);
		}
	}

	/*
	 * Parse and validate FORMAT clause.
	 */
	formattype = transformFormatType(createExtStmt->format);
	formatOptStr = transformFormatOpts(formattype,
									   createExtStmt->formatOpts,
									   list_length(createExtStmt->tableElts),
									   iswritable);

	/*
	 * Parse single row error handling info if available
	 */
	singlerowerrorDesc = (SingleRowErrorDesc *)createExtStmt->sreh;

	if(singlerowerrorDesc)
	{
		Assert(!iswritable);
		
		issreh = true;

		/* get reject limit, and reject limit type */
		rejectlimit = singlerowerrorDesc->rejectlimit;
		rejectlimittype = (singlerowerrorDesc->is_limit_in_rows ? 'r' : 'p');
		VerifyRejectLimit(rejectlimittype, rejectlimit);

		/* KEEP only allowed for COPY */
		if(singlerowerrorDesc->is_keep)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("KEEP is not supported in the external table definition. "
							"Error table will be dropped when its external table is "
							"dropped."),
									   errOmitLocation(true)));

		if(singlerowerrorDesc->errtable)
			fmtErrTblOid = RangeVarGetRelid(singlerowerrorDesc->errtable, true);
		else
			fmtErrTblOid = InvalidOid; /* no err tbl was requested */
	}

	/*
	 * Parse external table data encoding
	 */
	foreach(option, createExtStmt->encoding)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		Assert(strcmp(defel->defname, "encoding") == 0);

		if (dencoding)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting or redundant ENCODING specification"),
							   errOmitLocation(true)));
		dencoding = defel;
	}

	if (dencoding && dencoding->arg)
	{
		const char *encoding_name;

		if (IsA(dencoding->arg, Integer))
		{
			encoding = intVal(dencoding->arg);
			encoding_name = pg_encoding_to_char(encoding);
			if (strcmp(encoding_name, "") == 0 ||
				pg_valid_client_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%d is not a valid encoding code",
								encoding),
										   errOmitLocation(true)));
		}
		else if (IsA(dencoding->arg, String))
		{
			encoding_name = strVal(dencoding->arg);
			if (pg_valid_client_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%s is not a valid encoding name",
								encoding_name),
										   errOmitLocation(true)));
			encoding = pg_char_to_encoding(encoding_name);
		}
		else
			elog(ERROR, "unrecognized node type: %d",
				 nodeTag(dencoding->arg));
	}

	/* If encoding is defaulted, use database encoding */
	if (encoding < 0)
		encoding = pg_get_client_encoding();


    /*
	 * First, create the pg_class and other regular relation catalog entries.
	 * Under the covers this will dispatch a CREATE TABLE statement to all the
	 * QEs.
	 */
	if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_UTILITY)
		reloid = DefineRelation(createStmt, RELKIND_RELATION, RELSTORAGE_EXTERNAL);

	/*
	 * Now we take care of pg_exttable and dependency with error table (if any).
	 */
    PG_TRY();
    {

		/*
		 * get our pg_class external rel OID. If we're the QD we just created
		 * it above. If we're a QE DefineRelation() was already dispatched to
		 * us and therefore we have a local entry in pg_class. get the OID
		 * from cache.
		 */
		if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_UTILITY)
			Assert(reloid != InvalidOid);
		else
			reloid = RangeVarGetRelid(createExtStmt->relation, true);

		/*
		 * create a pg_exttable entry for this external table.
		 */
		InsertExtTableEntry(reloid, 
							iswritable, 
							isweb,
							issreh,
							formattype,
							rejectlimittype,
							commandString,
							rejectlimit,
							fmtErrTblOid,
							encoding,
							formatOptStr,
							locationExec,
							locationUris);
		
		/*
		 * record a dependency between the external table and its error table (if one exists)
		 */
		if(singlerowerrorDesc && singlerowerrorDesc->errtable)
		{
			ObjectAddress	myself,
			referenced;
			Oid		fmtErrTblOid = RangeVarGetRelid(((SingleRowErrorDesc *)createExtStmt->sreh)->errtable, true);

			Assert(!createExtStmt->iswritable);
			Assert(fmtErrTblOid != InvalidOid);

			myself.classId = RelationRelationId;
			myself.objectId = reloid;
			myself.objectSubId = 0;
			referenced.classId = RelationRelationId;
			referenced.objectId = fmtErrTblOid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}

		if (shouldDispatch)
		{

			/*
			 * Dispatch the statement tree to all primary segdbs.
			 * Doesn't wait for the QEs to finish execution.
			 */
			cdbdisp_dispatchUtilityStatement((Node *)createExtStmt,
											 true,      /* cancelOnError */
											 true,      /* startTransaction */
											 true,      /* withSnapshot */
											 (struct CdbDispatcherState *)&ds,
											 "DefineExternalRelation");
		}
    }
	PG_CATCH();
	{
        /* If dispatched, stop QEs and clean up after them. */
        if (ds.primaryResults)
            cdbdisp_handleError((struct CdbDispatcherState *)&ds);

        /* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * We successfully completed our work.	Now check the results from the
	 * qExecs, if dispatched.  This waits for them to all finish, and exits
	 * via ereport(ERROR,...) if unsuccessful.
	 */
	cdbdisp_finishCommand((struct CdbDispatcherState *)&ds, NULL, NULL);
}

extern void
DefineForeignRelation(CreateForeignStmt *createForeignStmt)
{
	CdbDispatcherState		  ds = {NULL, NULL};
	CreateStmt				  *createStmt = makeNode(CreateStmt);
	Oid						  reloid = 0;
	bool					  shouldDispatch = (Gp_role == GP_ROLE_DISPATCH &&
												IsNormalProcessingMode());
	
	/*
	 * now set the parameters for keys/inheritance etc. Most of these are
	 * uninteresting for external relations...
	 */
	createStmt->relation = createForeignStmt->relation;
	createStmt->tableElts = createForeignStmt->tableElts;
	createStmt->inhRelations = NIL;
	createStmt->constraints = NIL;
	createStmt->options = NIL;
	createStmt->oncommit = ONCOMMIT_NOOP;
	createStmt->tablespacename = NULL;
	createStmt->policy = NULL; /* for now, we use "master only" type of distribution */
	
	/* (permissions are checked in foreigncmd.c:InsertForeignTableEntry() ) */

    /*
	 * First, create the pg_class and other regular relation catalog entries.
	 * Under the covers this will dispatch a CREATE TABLE statement to all the
	 * QEs.
	 */
	if(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_UTILITY)
		reloid = DefineRelation(createStmt, RELKIND_RELATION, RELSTORAGE_FOREIGN);

	/*
	 * Now we take care of pg_foreign_table
	 */
    PG_TRY();
    {
    	ObjectAddress myself;
    	ObjectAddress referenced;
    	Oid	ownerId = GetUserId();
    	ForeignServer *srv = GetForeignServerByName(createForeignStmt->srvname, false);
    	
		/*
		 * get our pg_class foreign rel OID. If we're the QD we just created
		 * it above. If we're a QE DefineRelation() was already dispatched to
		 * us and therefore we have a local entry in pg_class. get the OID
		 * from cache.
		 */
		if(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_UTILITY)
			Assert(reloid != InvalidOid);
		else
			reloid = RangeVarGetRelid(createForeignStmt->relation, true);

		/* Add dependency on SERVER and owner */
		myself.classId = RelationRelationId;
		myself.objectId = reloid;
		myself.objectSubId = 0;

		referenced.classId = ForeignServerRelationId;
		referenced.objectId = srv->serverid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		recordDependencyOnOwner(RelationRelationId, reloid, ownerId);

		/* create a pg_exttable entry for this foreign table.*/
		InsertForeignTableEntry(reloid, 
								createForeignStmt->srvname, 
								createForeignStmt->options);
				

		if (shouldDispatch)
		{

			/* Dispatch the statement tree to all primary and mirror segdbs.
			 * Doesn't wait for the QEs to finish execution.
			 */
			cdbdisp_dispatchUtilityStatement((Node *)createForeignStmt,
											 true,      /* cancelOnError */
											 true,      /* startTransaction */
											 true,      /* withSnapshot */
											 &ds,
											 "DefineForeignRelation");
		}
    }
	PG_CATCH();
	{
        /* If dispatched, stop QEs and clean up after them. */
        if (ds.primaryResults)
            cdbdisp_handleError(&ds);

        /* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * We successfully completed our work.	Now check the results from the
	 * qExecs, if dispatched.  This waits for them to all finish, and exits
	 * via ereport(ERROR,...) if unsuccessful.
	 */
	cdbdisp_finishCommand(&ds, NULL, NULL);

}

/* ----------------------------------------------------------------
*		DefinePartitionedRelation
*				Create the rewrite rule for a partitioned table
*
* parse/analyze.c/transformPartitionBy does the bulk of the work for
* partitioned tables, converting a single CREATE TABLE into a series
* of statements to create the child tables for each partition.  Each
* child table has a check constraint and a rewrite rule to ensure that
* INSERTs to the parent end up in the correct child table (partition).
* However, we cannot add a RuleStmt for a non-existent table to the
* a(fter)list for the Create statement (believe me, I tried, really
* hard).  Thus, we create the "parsed" RuleStmt in analyze, and
* finally parse_analyze it here *after* the relation is created, the
* use process_utility to dispatch.
* ----------------------------------------------------------------
*/
void
DefinePartitionedRelation(CreateStmt *stmt, Oid relOid)
{
	if (stmt->postCreate)
	{
		List *pQry;
		Node *pUtl;
		DestReceiver *dest = None_Receiver;
		List *pL1 = (List *)stmt->postCreate;

		pQry = parse_analyze(linitial(pL1), NULL, NULL, 0);

		if (!pQry)
			return;

		/* just grab the first guy - "There Can Be Only One"(TM) */
		pUtl = linitial(pQry);

		if (!pUtl)
			return;

		Assert(IsA(pUtl, Query));
        Assert(((Query *)pUtl)->commandType == CMD_UTILITY);

		ProcessUtility((Node *)(((Query *)pUtl)->utilityStmt),
					   synthetic_sql,
					   NULL, 
					   false, /* not top level */
					   dest,
					   NULL);

	}
}

/* Don't track internal namespaces for toast, bitmap, aoseg */
#define METATRACK_VALIDNAMESPACE(namespaceId) \
	(namespaceId != PG_TOAST_NAMESPACE &&	\
	 namespaceId != PG_BITMAPINDEX_NAMESPACE && \
	 namespaceId != PG_AOSEGMENT_NAMESPACE )

/* check for valid namespace and valid relkind */
static 
bool
MetaTrackValidKindNsp(Form_pg_class rd_rel)
{
	Oid nsp = rd_rel->relnamespace;

	if (PG_CATALOG_NAMESPACE == nsp)
	{
		/* MPP-7773: don't track objects in system namespace
		 * if modifying system tables (eg during upgrade)  
		 */
		if (allowSystemTableMods)
			return (false);
	}

	/* MPP-7599: watch out for toast indexes */
	return (METATRACK_VALIDNAMESPACE(nsp)
			&& MetaTrackValidRelkind(rd_rel->relkind)
			/* MPP-7572: not valid if in any temporary namespace */
			&& (!(isAnyTempNamespace(nsp))));
}

/*
 * RemoveRelation
 *		Deletes a relation.
 */
void
RemoveRelation(const RangeVar *relation, DropBehavior behavior,
			   DropStmt *stmt)
{
	Oid			relOid;
	ObjectAddress object;
	HeapTuple tuple;

	AcceptInvalidationMessages();

	relOid = RangeVarGetRelid(relation, false);

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		LockRelationOid(RelationRelationId, RowExclusiveLock);
		LockRelationOid(TypeRelationId, RowExclusiveLock);
		LockRelationOid(DependRelationId, RowExclusiveLock);
	}

	LockRelationOid(relOid, AccessExclusiveLock);

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "relation \"%s\" does not exist", relation->relname);

	/* MPP-3260: disallow direct DROP TABLE of a partition */
	if (stmt && rel_is_child_partition(relOid) && !stmt->bAllowPartn)
	{
		Oid		 master = rel_partition_get_master(relOid);
		char	*pretty	= rel_get_part_path_pretty(relOid,
												   " ALTER PARTITION ",
												   " DROP PARTITION ");

		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				errmsg("cannot drop partition \"%s\" directly",
					   get_rel_name(relOid)),
				errhint("Table \"%s\" is a child partition of table "
						"\"%s\".  To drop it, use ALTER TABLE \"%s\"%s...",
						get_rel_name(relOid), get_rel_name(master),
						get_rel_name(master), pretty ? pretty : "" ),
								   errOmitLocation(true)));

	}

	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;

	ReleaseSysCache(tuple);

	/* if we got here then we should proceed. */
	performDeletion(&object, behavior);
}

/*
 * RelationToRemoveIsTemp
 *		Checks if an object being targeted for drop is a temporary table.
 */
bool
RelationToRemoveIsTemp(const RangeVar *relation, DropBehavior behavior)
{
	Oid			relOid;
	Oid			recheckoid;
	ObjectAddress object;
	HeapTuple	relTup;
	Form_pg_class relForm;
	char	   *nspname;
	char	   *relname;
	bool		isTemp;

	elog(DEBUG5, "Relation to remove catalogname %s, schemaname %s, relname %s",
		 (relation->catalogname == NULL ? "<empty>" : relation->catalogname),
		 (relation->schemaname == NULL ? "<empty>" : relation->schemaname),
		 (relation->relname == NULL ? "<empty>" : relation->relname));

	// UNDONE: Not sure how to interpret 'behavior'...

	relOid = RangeVarGetRelid(relation, false);

	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;

	/*
	 * Lock down the object to stablize it before we examine its
	 * charactistics.
	 */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		LockRelationOid(RelationRelationId, RowExclusiveLock);
		LockRelationOid(TypeRelationId, RowExclusiveLock);
		LockRelationOid(DependRelationId, RowExclusiveLock);
	}

	/* Lock the relation to be dropped */
	LockRelationOid(relOid, AccessExclusiveLock);

	/*
	 * When we got the relOid lock, it is possible that the relation has gone away.
	 * this will throw Error if the relation is already deleted.
	 */
	recheckoid = RangeVarGetRelid(relation, false);

	/* if we got here then we should proceed. */

	relTup = SearchSysCache(RELOID,
							ObjectIdGetDatum(relOid),
							0, 0, 0);
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	relForm = (Form_pg_class) GETSTRUCT(relTup);

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(relOid))
		nspname = NULL;
	else
		nspname = get_namespace_name(relForm->relnamespace);

	relname = quote_qualified_identifier(nspname, NameStr(relForm->relname));

	isTemp = isTempNamespace(relForm->relnamespace);

	elog(DEBUG5, "Relation name is %s, namespace %s, isTemp = %s",
	     relname,
	     (nspname == NULL ? "<null>" : nspname),
	     (isTemp ? "true" : "false"));

	ReleaseSysCache(relTup);

	return isTemp;
}


/*
 * ExecuteTruncate
 *		Executes a TRUNCATE command.
 *
 * This is a multi-relation truncate.  We first open and grab exclusive
 * lock on all relations involved, checking permissions and otherwise
 * verifying that the relation is OK for truncation.  In CASCADE mode,
 * relations having FK references to the targeted relations are automatically
 * added to the group; in RESTRICT mode, we check that all FK references are
 * internal to the group that's being truncated.  Finally all the relations
 * are truncated and reindexed.
 */
void
ExecuteTruncate(TruncateStmt *stmt)
{
	List	   *rels = NIL;
	List	   *relids = NIL;
	List	   *meta_relids = NIL;
	List	   *new_heap_oids = NIL;
	List	   *new_toast_oids = NIL;
	List	   *new_aoseg_oids = NIL;
	List       *new_aoblkdir_oids = NIL;
	List	   *new_ind_oids = NIL;
	ListCell   *cell;
    int partcheck = 2;
	List *partList = NIL;

	/*
	 * Open, exclusive-lock, and check all the explicitly-specified relations
	 *
	 * Check if table has partitions and add them too
	 */
	while (partcheck)
	{
		foreach(cell, stmt->relations)
		{
			RangeVar   *rv = lfirst(cell);
			Relation	rel;
			PartitionNode *pNode;

			PG_TRY();
			{
				rel = heap_openrv(rv, AccessExclusiveLock);
			}
			
			PG_CATCH();
			{
				/* 
				 * In the case of the table being dropped concurrently, 
				 * throw a friendlier error than:
				 * 
				 * "could not open relation with relid 1234"
				 */
				if (rv->schemaname)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_TABLE),
							 errmsg("relation \"%s.%s\" does not exist",
									rv->schemaname, rv->relname),
							 errOmitLocation(true)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_TABLE),
							 errmsg("relation \"%s\" does not exist",
									rv->relname),
							 errOmitLocation(true)));
				PG_RE_THROW();
			}
			PG_END_TRY();
			
			truncate_check_rel(rel);

			if (partcheck == 2)
			{
				pNode = RelationBuildPartitionDesc(rel, false);

				if (pNode)
				{
					List *plist = atpxTruncateList(rel, pNode);

					if (plist)
					{
						if (partList)
							partList = list_concat(partList, plist);
						else
							partList = plist;
					}
				}
			}
			heap_close(rel, NoLock);
		}

		partcheck--;

		if (partList)
		{
			/* add the partitions to the relation list and try again */
			if (partcheck == 1)
				stmt->relations = list_concat(partList, stmt->relations);
		}
		else
			/* no partitions - no need to try again */
			partcheck = 0;
	} /* end while partcheck */

	/*
	 * Open, exclusive-lock, and check all the explicitly-specified relations
	 */
	foreach(cell, stmt->relations)
	{
		RangeVar   *rv = lfirst(cell);
		Relation	rel;

		rel = heap_openrv(rv, AccessExclusiveLock);
		truncate_check_rel(rel);
		rels = lappend(rels, rel);
		relids = lappend_oid(relids, RelationGetRelid(rel));

		if (MetaTrackValidKindNsp(rel->rd_rel))
			meta_relids = lappend_oid(meta_relids, RelationGetRelid(rel));
	}


	/*
	 * In CASCADE mode, suck in all referencing relations as well.	This
	 * requires multiple iterations to find indirectly-dependent relations. At
	 * each phase, we need to exclusive-lock new rels before looking for their
	 * dependencies, else we might miss something.	Also, we check each rel as
	 * soon as we open it, to avoid a faux pas such as holding lock for a long
	 * time on a rel we have no permissions for.
	 */
	if (stmt->behavior == DROP_CASCADE)
	{
		for (;;)
		{
			List	   *newrelids;

			newrelids = heap_truncate_find_FKs(relids);
			if (newrelids == NIL)
				break;			/* nothing else to add */

			foreach(cell, newrelids)
			{
				Oid			relid = lfirst_oid(cell);
				Relation	rel;

				rel = heap_open(relid, AccessExclusiveLock);
				ereport(NOTICE,
						(errmsg("truncate cascades to table \"%s\"",
								RelationGetRelationName(rel)),
										   errOmitLocation(true)));
				truncate_check_rel(rel);
				rels = lappend(rels, rel);
				relids = lappend_oid(relids, relid);

				if (MetaTrackValidKindNsp(rel->rd_rel))
					meta_relids = lappend_oid(meta_relids, 
											  RelationGetRelid(rel));
			}
		}
	}


	/*
	 * Check foreign key references.  In CASCADE mode, this should be
	 * unnecessary since we just pulled in all the references; but as a
	 * cross-check, do it anyway if in an Assert-enabled build.
	 */
#ifdef USE_ASSERT_CHECKING
	heap_truncate_check_FKs(rels, false);
#else
	if (stmt->behavior == DROP_RESTRICT)
		heap_truncate_check_FKs(rels, false);
#endif

	/*
	 * OK, truncate each table.
	 */
	if (Gp_role != GP_ROLE_EXECUTE)
	{
		cdb_sync_oid_to_segments();

		foreach(cell, rels)
		{
			Relation	rel = (Relation) lfirst(cell);
			Oid			heap_relid;
			Oid			toast_relid;
			Oid			aoseg_relid = InvalidOid;
			Oid			aoblkdir_relid = InvalidOid;
			Oid			new_heap_oid = InvalidOid;
			Oid			new_toast_oid = InvalidOid;
			Oid			new_aoseg_oid = InvalidOid;
			Oid			new_aoblkdir_oid = InvalidOid;
			List	   *indoids = NIL;

			/*
			 * Create a new empty storage file for the relation, and assign it
			 * as the relfilenode value. The old storage file is scheduled for
			 * deletion at commit.
			 */
			new_heap_oid = setNewRelfilenode(rel);

			new_heap_oids = lappend_oid(new_heap_oids, new_heap_oid);

			heap_relid = RelationGetRelid(rel);
			toast_relid = rel->rd_rel->reltoastrelid;
			heap_close(rel, NoLock);

			if (RelationIsAoRows(rel) ||
				RelationIsAoCols(rel))
				GetAppendOnlyEntryAuxOids(heap_relid, &aoseg_relid, NULL,
										  &aoblkdir_relid, NULL);

			/*
			 * The same for the toast table, if any.
			 */
			if (OidIsValid(toast_relid))
			{
				rel = relation_open(toast_relid, AccessExclusiveLock);
				new_toast_oid = setNewRelfilenode(rel);
				heap_close(rel, NoLock);
			}

			/*
			 * The same for the aoseg table, if any.
			 */
			if (OidIsValid(aoseg_relid))
			{
				rel = relation_open(aoseg_relid, AccessExclusiveLock);
				new_aoseg_oid = setNewRelfilenode(rel);
				heap_close(rel, NoLock);
			}

			if (OidIsValid(aoblkdir_relid))
			{
				rel = relation_open(aoblkdir_relid, AccessExclusiveLock);
				new_aoblkdir_oid = setNewRelfilenode(rel);
				heap_close(rel, NoLock);
			}

			new_toast_oids = lappend_oid(new_toast_oids, new_toast_oid);
			new_aoseg_oids = lappend_oid(new_aoseg_oids, new_aoseg_oid);
			new_aoblkdir_oids = lappend_oid(new_aoblkdir_oids, new_aoblkdir_oid);

			elog(DEBUG5, "Truncate in DISPATCH mode: old heap oid %u "
				 "toast %u aoseg %u aoblkdir %u new heap oid %u, new toast oid %u "
				 "aoseg oid %u aoblkdir oid %u",
				 heap_relid, toast_relid, aoseg_relid, aoblkdir_relid,
				 new_heap_oid, new_toast_oid, new_aoseg_oid, new_aoblkdir_oid);

			/*
			 * Reconstruct the indexes to match, and we're done.
			 */
			reindex_relation(heap_relid, true, true, true, &indoids, true);
			new_ind_oids = lappend(new_ind_oids, indoids);
		}

		if (Gp_role == GP_ROLE_DISPATCH)
		{
			ListCell	*lc;

			stmt->relids = relids;
			stmt->new_heap_oids = new_heap_oids;
			stmt->new_toast_oids = new_toast_oids;
			stmt->new_aoseg_oids = new_aoseg_oids;
			stmt->new_aoblkdir_oids = new_aoblkdir_oids;
			stmt->new_ind_oids = new_ind_oids;

			CdbDispatchUtilityStatement((Node *) stmt, "ExecuteTruncate");

			/* MPP-6929: metadata tracking */
			foreach(lc, meta_relids)
			{
				Oid			a_relid = lfirst_oid(lc);

				MetaTrackUpdObject(RelationRelationId,
								   a_relid,
								   GetUserId(),
								   "VACUUM", "TRUNCATE");
			}

		}
	}
	else /* role=execute */
	{
		ListCell   *cell2;
		ListCell   *cell3;
		ListCell   *cell4;
		ListCell   *cell5;
		ListCell   *cell6;
		ListCell   *ind_cell;

#ifdef USE_ASSERT_CHECKING
		int			len = list_length(relids);
		Assert(len == list_length(stmt->relids));
		Assert(len == list_length(stmt->new_heap_oids));
		Assert(len == list_length(stmt->new_toast_oids));
		Assert(len == list_length(stmt->new_aoseg_oids));
		Assert(len == list_length(stmt->new_aoblkdir_oids));
		Assert(len == list_length(stmt->new_ind_oids));
#endif /* USE_ASSERT_CHECKING */

		cell2 = list_head(stmt->relids);
		cell3 = list_head(stmt->new_heap_oids);
		cell4 = list_head(stmt->new_toast_oids);
		cell5 = list_head(stmt->new_aoseg_oids);
		cell6 = list_head(stmt->new_aoblkdir_oids);

		ind_cell = list_head(stmt->new_ind_oids);

		foreach(cell, rels)
		{
			Relation	rel = (Relation) lfirst(cell);
			Oid			heap_relid = lfirst_oid(cell2);
			Oid			toast_relid;
			Oid			aoseg_relid = InvalidOid;
			Oid			aoblkdir_relid = InvalidOid;
			Oid			new_heap_oid = lfirst_oid(cell3);
			Oid			new_toast_oid = lfirst_oid(cell4);
			Oid			new_aoseg_oid = lfirst_oid(cell5);
			Oid			new_aoblkdir_oid = lfirst_oid(cell6);
			List	   *ind_oids = ind_cell ? lfirst(ind_cell) : NIL;

			/*
			 * Create a new empty storage file for the relation, and assign it
			 * as the relfilenode value. The old storage file is scheduled for
			 * deletion at commit.
			 */
			setNewRelfilenodeToOid(rel, new_heap_oid);

			heap_relid = RelationGetRelid(rel);
			toast_relid = rel->rd_rel->reltoastrelid;
			heap_close(rel, NoLock);

			if (RelationIsAoRows(rel) || RelationIsAoCols(rel))
				GetAppendOnlyEntryAuxOids(heap_relid, &aoseg_relid, NULL,
										  &aoblkdir_relid, NULL);

			/*
			 * The same for the toast table, if any.
			 */
			if (OidIsValid(toast_relid))
			{
				rel = relation_open(toast_relid, AccessExclusiveLock);
				setNewRelfilenodeToOid(rel, new_toast_oid);
				heap_close(rel, NoLock);
			}

			/*
			 * The same for the aoseg table, if any.
			 */

			if (OidIsValid(aoseg_relid))
			{
				rel = relation_open(aoseg_relid, AccessExclusiveLock);
				setNewRelfilenodeToOid(rel, new_aoseg_oid);
				heap_close(rel, NoLock);
			}

			if (OidIsValid(aoblkdir_relid))
			{
				rel = relation_open(aoblkdir_relid, AccessExclusiveLock);
				setNewRelfilenodeToOid(rel, new_aoblkdir_oid);
				heap_close(rel, NoLock);
			}

			elog(DEBUG5, "Truncate in EXECUTE mode: old heap oid %u "
				 "toast %u aoseg %u aoblkdir %u new heap oid %u, new toast oid %u "
				 "aoseg oid %u aoblkdir oid %u",
				 heap_relid, toast_relid, aoseg_relid, aoblkdir_relid,
				 new_heap_oid, new_toast_oid, new_aoseg_oid, new_aoblkdir_oid);

			/*
			 * Reconstruct the indexes to match, and we're done.
			 */
			reindex_relation(heap_relid, true, true, true, &ind_oids, false);

			cell2 = lnext(cell2);
			cell3 = lnext(cell3);
			cell4 = lnext(cell4);
			cell5 = lnext(cell5);
			cell6 = lnext(cell6);
			ind_cell = ind_cell ? lnext(ind_cell) : NULL;
		}
	}
}

/*
 * Check that a given rel is safe to truncate.	Subroutine for ExecuteTruncate
 */
static void
truncate_check_rel(Relation rel)
{
	/* Only allow truncate on regular or append-only tables */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table",
							RelationGetRelationName(rel)),
									   errOmitLocation(true)));

	if (RelationIsExternal(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an external relation and can't be truncated",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));


	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	/*
	 * We can never allow truncation of shared or nailed-in-cache relations,
	 * because we can't support changing their relfilenode values.
	 */
	if (rel->rd_rel->relisshared || rel->rd_isnailed)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot truncate system relation \"%s\"",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	/*
	 * Don't allow truncate on temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("cannot truncate temporary tables of other sessions"),
					   errOmitLocation(true)));

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 */
	CheckTableNotInUse(rel, "TRUNCATE");
}

/*----------
 * MergeAttributes
 *		Returns new schema given initial schema and superclasses.
 *
 * Input arguments:
 * 'schema' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'supers' is a list of names (as RangeVar nodes) of parent relations.
 * 'istemp' is TRUE if we are creating a temp relation.
 * 'GpPolicy *' is NULL if the distribution policy is not to be updated
 *
 * Output arguments:
 * 'supOids' receives a list of the OIDs of the parent relations.
 * 'supconstr' receives a list of constraints belonging to the parents,
 *		updated as necessary to be valid for the child.
 * 'supOidCount' is set to the number of parents that have OID columns.
 * 'GpPolicy' is updated with the offsets of the distribution
 *      attributes in the new schema
 *
 * Return value:
 * Completed schema list.
 *
 * Notes:
 *	  The order in which the attributes are inherited is very important.
 *	  Intuitively, the inherited attributes should come first. If a table
 *	  inherits from multiple parents, the order of those attributes are
 *	  according to the order of the parents specified in CREATE TABLE.
 *
 *	  Here's an example:
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  The order of the attributes of stud_emp is:
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   If the same attribute name appears multiple times, then it appears
 *	   in the result table in the proper location for its first appearance.
 *
 *	   Constraints (including NOT NULL constraints) for the child table
 *	   are the union of all relevant constraints, from both the child schema
 *	   and parent tables.
 *
 *	   The default value for a child column is defined as:
 *		(1) If the child schema specifies a default, that value is used.
 *		(2) If neither the child nor any parent specifies a default, then
 *			the column will not have a default.
 *		(3) If conflicting defaults are inherited from different parents
 *			(and not overridden by the child), an error is raised.
 *		(4) Otherwise the inherited default is used.
 *		Rule (3) is new in Postgres 7.1; in earlier releases you got a
 *		rather arbitrary choice of which parent default to use.
 *----------
 */
static List *
MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr, int *supOidCount, GpPolicy *policy)
{
	ListCell   *entry;
	List	   *inhSchema = NIL;
	List	   *parentOids = NIL;
	List	   *constraints = NIL;
	int			parentsWithOids = 0;
	bool		have_bogus_defaults = false;
	char	   *bogus_marker = "Bogus!";		/* marks conflicting defaults */
	int			child_attno;

	/*
	 * Check for and reject tables with too many columns. We perform this
	 * check relatively early for two reasons: (a) we don't run the risk of
	 * overflowing an AttrNumber in subsequent code (b) an O(n^2) algorithm is
	 * okay if we're processing <= 1600 columns, but could take minutes to
	 * execute if the user attempts to create a table with hundreds of
	 * thousands of columns.
	 *
	 * Note that we also need to check that any we do not exceed this figure
	 * after including columns from inherited relations.
	 */
	if (list_length(schema) > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * Check for duplicate names in the explicit list of attributes.
	 *
	 * Although we might consider merging such entries in the same way that we
	 * handle name conflicts for inherited attributes, it seems to make more
	 * sense to assume such conflicts are errors.
	 */
	foreach(entry, schema)
	{
		ColumnDef  *coldef = lfirst(entry);
		ListCell   *rest;

		for_each_cell(rest, lnext(entry))
		{
			ColumnDef  *restdef = lfirst(rest);

			if (strcmp(coldef->colname, restdef->colname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" duplicated",
								coldef->colname),
										   errOmitLocation(true)));
		}
	}

	/*
	 * Scan the parents left-to-right, and merge their attributes to form a
	 * list of inherited attributes (inhSchema).  Also check to see if we need
	 * to inherit an OID column.
	 */
	child_attno = 0;
	foreach(entry, supers)
	{
		RangeVar   *parent = (RangeVar *) lfirst(entry);
		Relation	relation;
		TupleDesc	tupleDesc;
		TupleConstr *constr;
		AttrNumber *newattno;
		AttrNumber	parent_attno;

		relation = heap_openrv(parent, AccessShareLock);

		if (relation->rd_rel->relkind != RELKIND_RELATION)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("inherited relation \"%s\" is not a table",
							parent->relname),
									   errOmitLocation(true)));
		/* Permanent rels cannot inherit from temporary ones */
		if (!istemp && isTempNamespace(RelationGetNamespace(relation)))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from temporary relation \"%s\"",
							parent->relname),
									   errOmitLocation(true)));

		/*
		 * We should have an UNDER permission flag for this, but for now,
		 * demand that creator of a child table own the parent.
		 */
		if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
						   RelationGetRelationName(relation));

		/*
		 * Reject duplications in the list of parents.
		 */
		if (list_member_oid(parentOids, RelationGetRelid(relation)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("inherited relation \"%s\" duplicated",
							parent->relname),
									   errOmitLocation(true)));

		parentOids = lappend_oid(parentOids, RelationGetRelid(relation));

		if (relation->rd_rel->relhasoids)
			parentsWithOids++;

		tupleDesc = RelationGetDescr(relation);
		constr = tupleDesc->constr;

		/*
		 * newattno[] will contain the child-table attribute numbers for the
		 * attributes of this parent table.  (They are not the same for
		 * parents after the first one, nor if we have dropped columns.)
		 */
		newattno = (AttrNumber *)
			palloc(tupleDesc->natts * sizeof(AttrNumber));

		for (parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
			char	   *attributeName = NameStr(attribute->attname);
			int			exist_attno;
			ColumnDef  *def;

			/*
			 * Ignore dropped columns in the parent.
			 */
			if (attribute->attisdropped)
			{
				/*
				 * change_varattnos_of_a_node asserts that this is greater
				 * than zero, so if anything tries to use it, we should find
				 * out.
				 */
				newattno[parent_attno - 1] = 0;
				continue;
			}

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				if (Gp_role == GP_ROLE_EXECUTE)
				{
					ereport(DEBUG1,
						(errmsg("merging multiple inherited definitions of column \"%s\"",
								attributeName)));
				}
				else
				ereport(NOTICE,
						(errmsg("merging multiple inherited definitions of column \"%s\"",
								attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				if (typenameTypeId(NULL, def->typname) != attribute->atttypid ||
					def->typname->typmod != attribute->atttypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("inherited column \"%s\" has a type conflict",
							   attributeName),
							 errdetail("%s versus %s",
									   TypeNameToString(def->typname),
									   format_type_be(attribute->atttypid)),
											   errOmitLocation(true)));
				def->inhcount++;
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= attribute->attnotnull;
				/* Default and other constraints are handled below */
				newattno[parent_attno - 1] = exist_attno;

				/*
				 * Update GpPolicy
				 */
				if (policy != NULL)
				{
					int attr_ofst = 0;

					Assert(policy->nattrs >= 0 && "the number of distribution attributes is not negative");

					/* Iterate over all distribution attribute offsets */
					for (attr_ofst = 0; attr_ofst < policy->nattrs; attr_ofst++)
					{
						/* Check if any distribution attribute has higher offset than the current */
						if (policy->attrs[attr_ofst] > child_attno)
						{
							Assert(policy->attrs[attr_ofst] > 0 && "index should not become negative");
							policy->attrs[attr_ofst]--;
						}
					}
				}

			}
			else
			{
				/*
				 * No, create a new inherited column
				 */
				def = makeNode(ColumnDef);
				def->colname = pstrdup(attributeName);
				def->typname = makeTypeNameFromOid(attribute->atttypid,
													attribute->atttypmod);
				def->inhcount = 1;
				def->is_local = false;
				def->is_not_null = attribute->attnotnull;
				def->raw_default = NULL;
				def->cooked_default = NULL;
				def->constraints = NIL;
				inhSchema = lappend(inhSchema, def);
				newattno[parent_attno - 1] = ++child_attno;
			}

			/*
			 * Copy default if any
			 */
			if (attribute->atthasdef)
			{
				char	   *this_default = NULL;
				AttrDefault *attrdef;
				int			i;

				/* Find default in constraint structure */
				Assert(constr != NULL);
				attrdef = constr->defval;
				for (i = 0; i < constr->num_defval; i++)
				{
					if (attrdef[i].adnum == parent_attno)
					{
						this_default = attrdef[i].adbin;
						break;
					}
				}
				Assert(this_default != NULL);

				/*
				 * If default expr could contain any vars, we'd need to fix
				 * 'em, but it can't; so default is ready to apply to child.
				 *
				 * If we already had a default from some prior parent, check
				 * to see if they are the same.  If so, no problem; if not,
				 * mark the column as having a bogus default. Below, we will
				 * complain if the bogus default isn't overridden by the child
				 * schema.
				 */
				Assert(def->raw_default == NULL);
				if (def->cooked_default == NULL)
					def->cooked_default = pstrdup(this_default);
				else if (strcmp(def->cooked_default, this_default) != 0)
				{
					def->cooked_default = bogus_marker;
					have_bogus_defaults = true;
				}
			}
		}

		/*
		 * Now copy the constraints of this parent, adjusting attnos using the
		 * completed newattno[] map
		 */
		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;
			int			i;

			for (i = 0; i < constr->num_check; i++)
			{
				Constraint *cdef = makeNode(Constraint);
				Node	   *expr;

				cdef->contype = CONSTR_CHECK;
				cdef->name = pstrdup(check[i].ccname);
				cdef->raw_expr = NULL;
				/* adjust varattnos of ccbin here */
				expr = stringToNode(check[i].ccbin);
				change_varattnos_of_a_node(expr, newattno);
				cdef->cooked_expr = nodeToString(expr);
				constraints = lappend(constraints, cdef);
			}
		}

		pfree(newattno);

		/*
		 * Close the parent rel, but keep our AccessShareLock on it until xact
		 * commit.	That will prevent someone else from deleting or ALTERing
		 * the parent before the child is committed.
		 */
		heap_close(relation, NoLock);
	}

	/*
	 * If we had no inherited attributes, the result schema is just the
	 * explicitly declared columns.  Otherwise, we need to merge the declared
	 * columns into the inherited schema list.
	 */
	if (inhSchema != NIL)
	{
		foreach(entry, schema)
		{
			ColumnDef  *newdef = lfirst(entry);
			char	   *attributeName = newdef->colname;
			int			exist_attno;

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				ColumnDef  *def;

				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				if (Gp_role == GP_ROLE_EXECUTE)
				{
					ereport(DEBUG1,
					   (errmsg("merging column \"%s\" with inherited definition",
							   attributeName)));
				}
				else
				ereport(NOTICE,
				   (errmsg("merging column \"%s\" with inherited definition",
						   attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				if (typenameTypeId(NULL, def->typname) != typenameTypeId(NULL, newdef->typname) ||
					def->typname->typmod != newdef->typname->typmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("column \"%s\" has a type conflict",
									attributeName),
							 errdetail("%s versus %s",
									   TypeNameToString(def->typname),
									   TypeNameToString(newdef->typname))));
				/* Mark the column as locally defined */
				def->is_local = true;
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= newdef->is_not_null;
				/* If new def has a default, override previous default */
				if (newdef->raw_default != NULL)
				{
					def->raw_default = newdef->raw_default;
					def->cooked_default = newdef->cooked_default;
				}
			}
			else
			{
				/*
				 * No, attach new column to result schema
				 */
				inhSchema = lappend(inhSchema, newdef);
			}
		}

		schema = inhSchema;

		/*
		 * Check that we haven't exceeded the legal # of columns after merging
		 * in inherited columns.
		 */
		if (list_length(schema) > MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("tables can have at most %d columns",
							MaxHeapAttributeNumber)));
	}

	/*
	 * If we found any conflicting parent default values, check to make sure
	 * they were overridden by the child.
	 */
	if (have_bogus_defaults)
	{
		foreach(entry, schema)
		{
			ColumnDef  *def = lfirst(entry);

			if (def->cooked_default == bogus_marker)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				  errmsg("column \"%s\" inherits conflicting default values",
						 def->colname),
						 errhint("To resolve the conflict, specify a default explicitly.")));
		}
	}

	*supOids = parentOids;
	*supconstr = constraints;
	*supOidCount = parentsWithOids;
	return schema;
}


/*
 * In multiple-inheritance situations, it's possible to inherit
 * the same grandparent constraint through multiple parents.
 * Hence, we want to discard inherited constraints that match as to
 * both name and expression.  Otherwise, gripe if there are conflicting
 * names.  Nonconflicting constraints are added to the array check[]
 * of length *ncheck ... caller must ensure there is room!
 */
static void
add_nonduplicate_constraint(Constraint *cdef, ConstrCheck *check, int *ncheck)
{
	int			i;

	/* Should only see precooked constraints here */
	Assert(cdef->contype == CONSTR_CHECK);
	Assert(cdef->name != NULL);
	Assert(cdef->raw_expr == NULL && cdef->cooked_expr != NULL);

	for (i = 0; i < *ncheck; i++)
	{
		if (strcmp(check[i].ccname, cdef->name) != 0)
			continue;
		if (strcmp(check[i].ccbin, cdef->cooked_expr) == 0)
			return;				/* duplicate constraint, so ignore it */
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("duplicate check constraint name \"%s\"",
						cdef->name)));
	}
	/* No match on name, so add it to array */
	check[*ncheck].ccname = cdef->name;
	check[*ncheck].ccbin = pstrdup(cdef->cooked_expr);
	(*ncheck)++;
}


/*
 * Replace varattno values in an expression tree according to the given
 * map array, that is, varattno N is replaced by newattno[N-1].  It is
 * caller's responsibility to ensure that the array is long enough to
 * define values for all user varattnos present in the tree.  System column
 * attnos remain unchanged.
 *
 * Note that the passed node tree is modified in-place!
 */
void
change_varattnos_of_a_node(Node *node, const AttrNumber *newattno)
{
	/* no setup needed, so away we go */
	(void) change_varattnos_walker(node, newattno);
}

static bool
change_varattnos_walker(Node *node, const AttrNumber *newattno)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0 && var->varno == 1 &&
			var->varattno > 0)
		{
			/*
			 * ??? the following may be a problem when the node is multiply
			 * referenced though stringToNode() doesn't create such a node
			 * currently.
			 */
			Assert(newattno[var->varattno - 1] > 0);
			var->varattno = var->varoattno = newattno[var->varattno - 1];
		}
		return false;
	}
	return expression_tree_walker(node, change_varattnos_walker,
								  (void *) newattno);
}

/*
 * Generate a map for change_varattnos_of_a_node from old and new TupleDesc's,
 * matching according to column name.
 */
AttrNumber *
varattnos_map(TupleDesc old, TupleDesc new)
{
	AttrNumber *attmap;
	int			i,
				j;

	attmap = (AttrNumber *) palloc0(sizeof(AttrNumber) * old->natts);
	for (i = 1; i <= old->natts; i++)
	{
		if (old->attrs[i - 1]->attisdropped)
			continue;			/* leave the entry as zero */

		for (j = 1; j <= new->natts; j++)
		{
			if (strcmp(NameStr(old->attrs[i - 1]->attname),
					   NameStr(new->attrs[j - 1]->attname)) == 0)
			{
				attmap[i - 1] = j;
				break;
			}
		}
	}
	return attmap;
}

/*
 * Generate a map for change_varattnos_of_a_node from a TupleDesc and a list
 * of ColumnDefs
 */
AttrNumber *
varattnos_map_schema(TupleDesc old, List *schema)
{
	AttrNumber *attmap;
	int			i;

	attmap = (AttrNumber *) palloc0(sizeof(AttrNumber) * old->natts);
	for (i = 1; i <= old->natts; i++)
	{
		if (old->attrs[i - 1]->attisdropped)
			continue;			/* leave the entry as zero */

		attmap[i - 1] = findAttrByName(NameStr(old->attrs[i - 1]->attname),
									   schema);
	}
	return attmap;
}


/*
 * StoreCatalogInheritance
 *		Updates the system catalogs with proper inheritance information.
 *
 * supers is a list of the OIDs of the new relation's direct ancestors.
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers)
{
	Relation	relation;
	int16		seqNumber;
	ListCell   *entry;

	/*
	 * sanity checks
	 */
	AssertArg(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/*
	 * Store INHERITS information in pg_inherits using direct ancestors only.
	 * Also enter dependencies on the direct ancestors, and make sure they are
	 * marked with relhassubclass = true.
	 *
	 * (Once upon a time, both direct and indirect ancestors were found here
	 * and then entered into pg_ipl.  Since that catalog doesn't exist
	 * anymore, there's no need to look for indirect ancestors.)
	 */
	relation = heap_open(InheritsRelationId, RowExclusiveLock);

	seqNumber = 1;
	foreach(entry, supers)
	{
		Oid			parentOid = lfirst_oid(entry);

		StoreCatalogInheritance1(relationId, parentOid, seqNumber, relation,
								 false);
		seqNumber++;
	}

	heap_close(relation, RowExclusiveLock);
}

/*
 * Make catalog entries showing relationId as being an inheritance child
 * of parentOid.  inhRelation is the already-opened pg_inherits catalog.
 */
static void
StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int16 seqNumber, Relation inhRelation,
						 bool is_partition)
{
	TupleDesc	desc = RelationGetDescr(inhRelation);
	Datum		datum[Natts_pg_inherits];
	bool		nullarr[Natts_pg_inherits];
	ObjectAddress childobject,
				parentobject;
	HeapTuple	tuple;

	/*
	 * Make the pg_inherits entry
	 */
	datum[0] = ObjectIdGetDatum(relationId);	/* inhrelid */
	datum[1] = ObjectIdGetDatum(parentOid);		/* inhparent */
	datum[2] = Int16GetDatum(seqNumber);		/* inhseqno */

	nullarr[0] = false;
	nullarr[1] = false;
	nullarr[2] = false;

	tuple = heap_form_tuple(desc, datum, nullarr);

	simple_heap_insert(inhRelation, tuple);

	CatalogUpdateIndexes(inhRelation, tuple);

	heap_freetuple(tuple);

	/*
	 * Store a dependency too
	 */
	parentobject.classId = RelationRelationId;
	parentobject.objectId = parentOid;
	parentobject.objectSubId = 0;
	childobject.classId = RelationRelationId;
	childobject.objectId = relationId;
	childobject.objectSubId = 0;

	recordDependencyOn(&childobject, &parentobject,
					   is_partition ? DEPENDENCY_AUTO : DEPENDENCY_NORMAL);

	/*
	 * Mark the parent as having subclasses.
	 */
	setRelhassubclassInRelation(parentOid, true);
}

/*
 * Look for an existing schema entry with the given name.
 *
 * Returns the index (starting with 1) if attribute already exists in schema,
 * 0 if it doesn't.
 */
static int
findAttrByName(const char *attributeName, List *schema)
{
	ListCell   *s;
	int			i = 1;

	foreach(s, schema)
	{
		ColumnDef  *def = lfirst(s);

		if (strcmp(attributeName, def->colname) == 0)
			return i;

		i++;
	}
	return 0;
}

/*
 * Update a relation's pg_class.relhassubclass entry to the given value
 */
static void
setRelhassubclassInRelation(Oid relationId, bool relhassubclass)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Form_pg_class classtuple;

	/*
	 * Fetch a modifiable copy of the tuple, modify it, update pg_class.
	 *
	 * If the tuple already has the right relhassubclass setting, we don't
	 * need to update it, but we still need to issue an SI inval message.
	 */
	relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(relationId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	classtuple = (Form_pg_class) GETSTRUCT(tuple);

	if (classtuple->relhassubclass != relhassubclass)
	{
		classtuple->relhassubclass = relhassubclass;
		simple_heap_update(relationRelation, &tuple->t_self, tuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relationRelation, tuple);
	}
	else
	{
		/* no need to change tuple, but force relcache rebuild anyway */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);
	heap_close(relationRelation, NoLock);
}


/*
 *		renameatt		- changes the name of a attribute in a relation
 *
 *		Attname attribute is changed in attribute catalog.
 *		No record of the previous attname is kept (correct?).
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan attribute catalog
 *				for name conflict (within rel)
 *				for original attribute (if not arg)
 *		modify attname in attribute tuple
 *		insert modified attribute in attribute catalog
 *		delete original attribute from attribute catalog
 */
void
renameatt(Oid myrelid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse,
		  bool recursing)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	atttup;
	Form_pg_attribute attform;
	int			attnum;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);

	/*
	 * permissions checking.  this would normally be done in utility.c, but
	 * this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!pg_class_ownercheck(myrelid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(targetrelation));
	if (!allowSystemTableMods && IsSystemRelation(targetrelation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(targetrelation))));

	/*
	 * if the 'recurse' flag is set then we are supposed to rename this
	 * attribute in all classes that inherit from 'relname' (as well as in
	 * 'relname').
	 *
	 * any permissions or problems with duplicate attributes will cause the
	 * whole transaction to abort, which is what we want -- all or nothing.
	 */
	if (recurse)
	{
		ListCell   *child;
		List	   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);

			if (childrelid == myrelid)
				continue;
			/* note we need not recurse again */
			renameatt(childrelid, oldattname, newattname, false, true);
		}
	}
	else
	{
		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables; else the rename would put them out of step.
		 */
		if (!recursing &&
			find_inheritance_children(myrelid) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("inherited column \"%s\" must be renamed in child tables too",
							oldattname)));
	}

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	atttup = SearchSysCacheCopyAttName(myrelid, oldattname);
	if (!HeapTupleIsValid(atttup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist",
						oldattname),
				 errOmitLocation(true)));
	attform = (Form_pg_attribute) GETSTRUCT(atttup);

	attnum = attform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot rename system column \"%s\"",
						oldattname)));

	/*
	 * if the attribute is inherited, forbid the renaming, unless we are
	 * already inside a recursive rename.
	 */
	if (attform->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot rename inherited column \"%s\"",
						oldattname)));

	/* should not already exist */
	/* this test is deliberately not attisdropped-aware */
	if (SearchSysCacheExists(ATTNAME,
							 ObjectIdGetDatum(myrelid),
							 PointerGetDatum((char *) newattname),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
					  newattname, RelationGetRelationName(targetrelation)),
				 errOmitLocation(true)));

	namestrcpy(&(attform->attname), newattname);

	simple_heap_update(attrelation, &atttup->t_self, atttup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, atttup);

	heap_freetuple(atttup);

	/*
	 * Update column names of indexes that refer to the column being renamed.
	 */
	indexoidlist = RelationGetIndexList(targetrelation);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indextup;
		Form_pg_index indexform;
		int			i;

		/*
		 * Scan through index columns to see if there's any simple index
		 * entries for this attribute.	We ignore expressional entries.
		 */
		indextup = SearchSysCache(INDEXRELID,
								  ObjectIdGetDatum(indexoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(indextup))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexform = (Form_pg_index) GETSTRUCT(indextup);

		for (i = 0; i < indexform->indnatts; i++)
		{
			if (attnum != indexform->indkey.values[i])
				continue;

			/*
			 * Found one, rename it.
			 */
			atttup = SearchSysCacheCopy(ATTNUM,
										ObjectIdGetDatum(indexoid),
										Int16GetDatum(i + 1),
										0, 0);
			if (!HeapTupleIsValid(atttup))
				continue;		/* should we raise an error? */

			/*
			 * Update the (copied) attribute tuple.
			 */
			namestrcpy(&(((Form_pg_attribute) GETSTRUCT(atttup))->attname),
					   newattname);

			simple_heap_update(attrelation, &atttup->t_self, atttup);

			/* keep system catalog indexes current */
			CatalogUpdateIndexes(attrelation, atttup);

			heap_freetuple(atttup);
		}

		ReleaseSysCache(indextup);
	}

	list_free(indexoidlist);

	heap_close(attrelation, RowExclusiveLock);

	/*
	 * Update att name in any RI triggers associated with the relation.
	 */
	if (targetrelation->rd_rel->reltriggers > 0)
	{
		/* update tgargs column reference where att is primary key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   false, false);
		/* update tgargs column reference where att is foreign key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   true, false);
	}

	/* MPP-6929, MPP-7600: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(targetrelation->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(targetrelation),
						   GetUserId(),
						   "ALTER", "RENAME COLUMN"
				);


	relation_close(targetrelation, NoLock);		/* close rel but keep lock */
}

/*
 *		renamerel		- change the name of a relation
 *
 *		XXX - When renaming sequences, we don't bother to modify the
 *			  sequence name that is stored within the sequence itself
 *			  (this would cause problems with MVCC). In the future,
 *			  the sequence name should probably be removed from the
 *			  sequence, AFAIK there's no need for it to be there.
 */
void
renamerel(Oid myrelid, const char *newrelname, RenameStmt *stmt)
{
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	reltup;
	Oid			namespaceId;
	char	   *oldrelname;
	char		relkind;
	bool		relhastriggers;

	/*
	 * Grab an exclusive lock on the target table or index, which we will NOT
	 * release until end of transaction.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);

	/* if this is a child table of a partitioning configuration, complain */
	if (stmt && rel_is_child_partition(myrelid) && !stmt->bAllowPartn)
	{
		Oid		 master = rel_partition_get_master(myrelid);
		char	*pretty	= rel_get_part_path_pretty(myrelid,
												   " ALTER PARTITION ",
												   " RENAME PARTITION ");

		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				errmsg("cannot rename partition \"%s\" directly",
					   get_rel_name(myrelid)),
				errhint("Table \"%s\" is a child partition of table "
						"\"%s\".  To rename it, use ALTER TABLE \"%s\"%s...",
						get_rel_name(myrelid), get_rel_name(master),
						get_rel_name(master), pretty ? pretty : "" ),
				errOmitLocation(true)));
	}

	oldrelname = pstrdup(RelationGetRelationName(targetrelation));
	namespaceId = RelationGetNamespace(targetrelation);

	if (!allowSystemTableMods && IsSystemRelation(targetrelation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(targetrelation))));

	relkind = targetrelation->rd_rel->relkind;
	relhastriggers = (targetrelation->rd_rel->reltriggers > 0);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in use.
	 */
	relrelation = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(myrelid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						newrelname),
				 errOmitLocation(true)));

	/*
	 * Update pg_class tuple with new relname.	(Scribbling on reltup is OK
	 * because it's a copy...)
	 */
	namestrcpy(&(((Form_pg_class) GETSTRUCT(reltup))->relname), newrelname);

	simple_heap_update(relrelation, &reltup->t_self, reltup);

	/* keep the system catalog indexes current */
	CatalogUpdateIndexes(relrelation, reltup);

	heap_freetuple(reltup);
	heap_close(relrelation, NoLock);

	/*
	 * Also rename the associated type, if any.
	 */
	if (relkind != RELKIND_INDEX)
		TypeRename(oldrelname, namespaceId, newrelname);

	/*
	 * Update rel name in any RI triggers associated with the relation.
	 */
	if (relhastriggers)
	{
		/* update tgargs where relname is primary key */
		update_ri_trigger_args(myrelid,
							   oldrelname,
							   newrelname,
							   false, true);
		/* update tgargs where relname is foreign key */
		update_ri_trigger_args(myrelid,
							   oldrelname,
							   newrelname,
							   true, true);
	}

	/* MPP-3059: recursive rename of partitioned table */
	/* Note: the top-level RENAME has bAllowPartn=FALSE, while the
	 * generated statements from this block set it to TRUE.  That way,
	 * the rename of each partition is allowed, but this block doesn't
	 * get invoked recursively.
	 */
	if (stmt && !rel_is_child_partition(myrelid) && !stmt->bAllowPartn &&
		(Gp_role == GP_ROLE_DISPATCH))
	{
		PartitionNode *pNode;

		pNode = RelationBuildPartitionDesc(targetrelation, false);

		if (pNode)
		{
			RenameStmt 			   	*renStmt 	 = makeNode(RenameStmt);
			DestReceiver 		   	*dest  		 = None_Receiver;
			List 					*renList 	 = NIL;
			int 					 skipped 	 = 0;
			int 					 renamed 	 = 0;

			renStmt->renameType = OBJECT_TABLE;
			renStmt->subname = NULL;
			renStmt->bAllowPartn = true; /* allow rename of partitions */

			/* rename the children as well */
			renList = atpxRenameList(targetrelation,
									 pNode, oldrelname, newrelname, &skipped);

			/* process children if there are any */
			if (renList)
			{
				ListCell 		*lc;

				foreach(lc, renList)
				{
					ListCell 		*lc2;
					List  			*lpair = lfirst(lc);

					lc2 = list_head(lpair);

					renStmt->relation = (RangeVar *)lfirst(lc2);
					lc2 = lnext(lc2);
					renStmt->newname = (char *)lfirst(lc2);

					ProcessUtility((Node *) renStmt,
								   synthetic_sql,
								   NULL, 
								   false, /* not top level */
								   dest, 
								   NULL);
					renamed++;
				}

				/* MPP-3542: warn when skip child partitions */
				if (skipped)
				{
					ereport(WARNING,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("renamed %d partitions, "
									"skipped %d child partitions "
									"due to name truncation",
									renamed, skipped),
							 errOmitLocation(true)));

				}

			}


		}


	}

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& 
		/* MPP-7773: don't track objects in system namespace
		 * if modifying system tables (eg during upgrade)  
		 */
		( ! ( (PG_CATALOG_NAMESPACE == namespaceId) && (allowSystemTableMods)))
		&& (   MetaTrackValidRelkind(relkind)
			&& METATRACK_VALIDNAMESPACE(namespaceId)
			   && (!(isAnyTempNamespace(namespaceId)))
				))
		MetaTrackUpdObject(RelationRelationId,
						   myrelid,
						   GetUserId(),
						   "ALTER", "RENAME"
				);


	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrelation, NoLock);
}

/*
 * Scan pg_trigger for RI triggers that are on the specified relation
 * (if fk_scan is false) or have it as the tgconstrrel (if fk_scan
 * is true).  Update RI trigger args fields matching oldname to contain
 * newname instead.  If update_relname is true, examine the relname
 * fields; otherwise examine the attname fields.
 */
static void
update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname)
{
	Relation	tgrel;
	ScanKeyData skey[1];
	SysScanDesc trigscan;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	bool		replaces[Natts_pg_trigger];

	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);
	if (fk_scan)
	{
		ScanKeyInit(&skey[0],
					Anum_pg_trigger_tgconstrrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		trigscan = systable_beginscan(tgrel, TriggerConstrRelidIndexId,
									  true, SnapshotNow,
									  1, skey);
	}
	else
	{
		ScanKeyInit(&skey[0],
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		trigscan = systable_beginscan(tgrel, TriggerRelidNameIndexId,
									  true, SnapshotNow,
									  1, skey);
	}

	while ((tuple = systable_getnext(trigscan)) != NULL)
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);
		bytea	   *val;
		bytea	   *newtgargs;
		bool		isnull;
		int			tg_type;
		bool		examine_pk;
		bool		changed;
		int			tgnargs;
		int			i;
		int			newlen;
		const char *arga[RI_MAX_ARGUMENTS];
		const char *argp;

		tg_type = RI_FKey_trigger_type(pg_trigger->tgfoid);
		if (tg_type == RI_TRIGGER_NONE)
		{
			/* Not an RI trigger, forget it */
			continue;
		}

		/*
		 * It is an RI trigger, so parse the tgargs bytea.
		 *
		 * NB: we assume the field will never be compressed or moved out of
		 * line; so does trigger.c ...
		 */
		tgnargs = pg_trigger->tgnargs;
		val = DatumGetByteaP(fastgetattr(tuple,
										Anum_pg_trigger_tgargs,
										tgrel->rd_att, &isnull));
		if (isnull || tgnargs < RI_FIRST_ATTNAME_ARGNO ||
			tgnargs > RI_MAX_ARGUMENTS)
		{
			/* This probably shouldn't happen, but ignore busted triggers */
			continue;
		}
		argp = (const char *) VARDATA(val);
		for (i = 0; i < tgnargs; i++)
		{
			arga[i] = argp;
			argp += strlen(argp) + 1;
		}

		/*
		 * Figure out which item(s) to look at.  If the trigger is primary-key
		 * type and attached to my rel, I should look at the PK fields; if it
		 * is foreign-key type and attached to my rel, I should look at the FK
		 * fields.	But the opposite rule holds when examining triggers found
		 * by tgconstrrel search.
		 */
		examine_pk = (tg_type == RI_TRIGGER_PK) == (!fk_scan);

		changed = false;
		if (update_relname)
		{
			/* Change the relname if needed */
			i = examine_pk ? RI_PK_RELNAME_ARGNO : RI_FK_RELNAME_ARGNO;
			if (strcmp(arga[i], oldname) == 0)
			{
				arga[i] = newname;
				changed = true;
			}
		}
		else
		{
			/* Change attname(s) if needed */
			i = examine_pk ? RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_PK_IDX :
				RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_FK_IDX;
			for (; i < tgnargs; i += 2)
			{
				if (strcmp(arga[i], oldname) == 0)
				{
					arga[i] = newname;
					changed = true;
				}
			}
		}

		if (!changed)
		{
			/* Don't need to update this tuple */
			continue;
		}

		/*
		 * Construct modified tgargs bytea.
		 */
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
			newlen += strlen(arga[i]) + 1;
		newtgargs = (bytea *) palloc(newlen);
		SET_VARSIZE(newtgargs, newlen);
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
		{
			strcpy(((char *) newtgargs) + newlen, arga[i]);
			newlen += strlen(arga[i]) + 1;
		}

		/*
		 * Build modified tuple.
		 */
		for (i = 0; i < Natts_pg_trigger; i++)
		{
			values[i] = (Datum) 0;
			replaces[i] = false;
			nulls[i] = false;
		}
		values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum(newtgargs);
		replaces[Anum_pg_trigger_tgargs - 1] = true;

		tuple = heap_modify_tuple(tuple, RelationGetDescr(tgrel), values, nulls, replaces);

		/*
		 * Update pg_trigger and its indexes
		 */
		simple_heap_update(tgrel, &tuple->t_self, tuple);

		CatalogUpdateIndexes(tgrel, tuple);

		/*
		 * Invalidate trigger's relation's relcache entry so that other
		 * backends (and this one too!) are sent SI message to make them
		 * rebuild relcache entries.  (Ideally this should happen
		 * automatically...)
		 *
		 * We can skip this for triggers on relid itself, since that relcache
		 * flush will happen anyway due to the table or column rename.	We
		 * just need to catch the far ends of RI relationships.
		 */
		pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);
		if (pg_trigger->tgrelid != relid)
			CacheInvalidateRelcacheByRelid(pg_trigger->tgrelid);

		/* free up our scratch memory */
		pfree(newtgargs);
		heap_freetuple(tuple);
	}

	systable_endscan(trigscan);

	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Increment cmd counter to make updates visible; this is needed in case
	 * the same tuple has to be updated again by next pass (can happen in case
	 * of a self-referential FK relationship).
	 */
	CommandCounterIncrement();
}

/*
 * Disallow ALTER TABLE (and similar commands) when the current backend has
 * any open reference to the target table besides the one just acquired by
 * the calling command; this implies there's an open cursor or active plan.
 * We need this check because our AccessExclusiveLock doesn't protect us
 * against stomping on our own foot, only other people's feet!
 *
 * For ALTER TABLE, the only case known to cause serious trouble is ALTER
 * COLUMN TYPE, and some changes are obviously pretty benign, so this could
 * possibly be relaxed to only error out for certain types of alterations.
 * But the use-case for allowing any of these things is not obvious, so we
 * won't work hard at it for now.
 *
 * We also reject these commands if there are any pending AFTER trigger events
 * for the rel.  This is certainly necessary for the rewriting variants of
 * ALTER TABLE, because they don't preserve tuple TIDs and so the pending
 * events would try to fetch the wrong tuples.  It might be overly cautious
 * in other cases, but again it seems better to err on the side of paranoia.
 *
 * REINDEX calls this with "rel" referencing the index to be rebuilt; here
 * we are worried about active indexscans on the index.  The trigger-event
 * check can be skipped, since we are doing no damage to the parent table.
 *
 * The statement name (eg, "ALTER TABLE") is passed for use in error messages.
 */
void
CheckTableNotInUse(Relation rel, const char *stmt)
{
	int			expected_refcnt;

	expected_refcnt = rel->rd_isnailed ? 2 : 1;

	/*
	 * XXX For a bitmap index, since vacuum (or vacuum full) is currently done through
	 * reindex_index, the reference count could be 2 (or 3). We set it
	 * here until vacuum is done properly.
	 */
	if (expected_refcnt == 1 &&
		RelationIsBitmapIndex(rel) &&
		(rel->rd_refcnt == 2 || rel->rd_refcnt == 3))
		expected_refcnt = rel->rd_refcnt;

	if (rel->rd_refcnt != expected_refcnt)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 /* translator: first %s is a SQL command, eg ALTER TABLE */
				 errmsg("cannot %s \"%s\" because "
						"it is being used by active queries in this session",
						stmt, RelationGetRelationName(rel))));

	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		AfterTriggerPendingOnRel(RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 /* translator: first %s is a SQL command, eg ALTER TABLE */
				 errmsg("cannot %s \"%s\" because "
						"it has pending trigger events",
						stmt, RelationGetRelationName(rel))));
}

static
void ATVerifyObject(AlterTableStmt *stmt, Relation rel)
{

	/* Nothing to check for ALTER INDEX */
	if(stmt->relkind == OBJECT_INDEX)
		return;
	

	/* 
	 * Verify the object specified against relstorage in the catalog.
	 * Enforce correct syntax usage. 
	 */
	if (RelationIsForeign(rel) && stmt->relkind != OBJECT_FOREIGNTABLE)
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a foreign table", RelationGetRelationName(rel)),
				 errhint("Use ALTER FOREIGN TABLE instead"),
				 errOmitLocation(true)));
	}
	else if (RelationIsExternal(rel) && stmt->relkind != OBJECT_EXTTABLE)
	{
		/*
		 * special case: in order to support 3.3 dumps with ALTER TABLE OWNER of
		 * external tables, we will allow using ALTER TABLE (without EXTERNAL) 
		 * temporarily, and use a deprecation warning. This should be removed
		 * in future releases.
		 */
		if(stmt->relkind == OBJECT_TABLE)
		{
			if (Gp_role == GP_ROLE_DISPATCH) /* why are WARNINGs emitted from all segments? */
				ereport(WARNING,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is an external table. ALTER TABLE for external tables is deprecated.", RelationGetRelationName(rel)),
						 errhint("Use ALTER EXTERNAL TABLE instead"),
						 errOmitLocation(true)));						
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is an external table", RelationGetRelationName(rel)),
					 errhint("Use ALTER EXTERNAL TABLE instead"),
					 errOmitLocation(true)));			
		}
	}
	else if (!RelationIsExternal(rel) && !RelationIsForeign(rel) && stmt->relkind != OBJECT_TABLE)
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a base table", RelationGetRelationName(rel)),
				 errhint("Use ALTER TABLE instead"),
				 errOmitLocation(true)));
	}
	
	/*
	 * Check the ALTER command type is supported for this object
	 */
	if (RelationIsForeign(rel) || RelationIsExternal(rel))
	{
		ListCell *lcmd;
		
		foreach(lcmd, stmt->cmds)
		{
			AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

			switch(cmd->subtype)
			{
				/* FOREIGN and EXTERNAL tables doesn't support the following AT */
				case AT_ColumnDefault:		
				case AT_DropNotNull:			
				case AT_SetNotNull:			
				case AT_SetStatistics:		
				case AT_SetStorage:			
				case AT_AddIndex:			
				case AT_ReAddIndex:			
				case AT_AddConstraint:		
				case AT_ProcessedConstraint:	
				case AT_DropConstraint:		
				case AT_DropConstraintQuietly:			
				case AT_ClusterOn:			
				case AT_DropCluster:			
				case AT_DropOids:			
				case AT_SetTableSpace:		
				case AT_SetRelOptions:		
				case AT_ResetRelOptions:		
				case AT_EnableTrig:			
				case AT_DisableTrig:			
				case AT_EnableTrigAll:		
				case AT_DisableTrigAll:		
				case AT_EnableTrigUser:		
				case AT_DisableTrigUser:		
				case AT_AddInherit:			
				case AT_DropInherit:			
				case AT_SetDistributedBy:	
				case AT_PartAdd:				
				case AT_PartAlter:			
				case AT_PartCoalesce:		
				case AT_PartDrop:			
				case AT_PartExchange:		
				case AT_PartMerge:			
				case AT_PartModify:			
				case AT_PartRename:			
				case AT_PartSetTemplate:		
				case AT_PartSplit:			
				case AT_PartTruncate:		
				case AT_PartAddInternal:		
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
							 errmsg("Unsupported ALTER command for table type %s", 
									 (RelationIsExternal(rel) ? "external" : "foreign")),
							 errOmitLocation(true)));
					break;
				
				case AT_AddColumn: /* check no constraint is added too */
					if(((ColumnDef *) cmd->def)->constraints != NIL ||
					   ((ColumnDef *) cmd->def)->is_not_null ||
					   ((ColumnDef *) cmd->def)->raw_default)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
								 errmsg("Unsupported ALTER command for table type %s. No constraints allowed.", 
										 (RelationIsExternal(rel) ? "external" : "foreign")),
								 errOmitLocation(true)));
					break;
					
				default:
					/* ALTER type supported */
					break;
			}
			
			
		}

	}

	/* treat as a normall table */
	//stmt->relkind = OBJECT_TABLE;
	
}

/*
 * AlterTable
 *		Execute ALTER TABLE, which can be a list of subcommands
 *
 * ALTER TABLE is performed in three phases:
 *		1. Examine subcommands and perform pre-transformation checking.
 *		2. Update system catalogs.
 *		3. Scan table(s) to check new constraints, and optionally recopy
 *		   the data into new table(s).
 * Phase 3 is not performed unless one or more of the subcommands requires
 * it.	The intention of this design is to allow multiple independent
 * updates of the table schema to be performed with only one pass over the
 * data.
 *
 * ATPrepCmd performs phase 1.	A "work queue" entry is created for
 * each table to be affected (there may be multiple affected tables if the
 * commands traverse a table inheritance hierarchy).  Also we do preliminary
 * validation of the subcommands, including parse transformation of those
 * expressions that need to be evaluated with respect to the old table
 * schema.
 *
 * ATRewriteCatalogs performs phase 2 for each affected table (note that
 * phases 2 and 3 do no explicit recursion, since phase 1 already did it).
 * Certain subcommands need to be performed before others to avoid
 * unnecessary conflicts; for example, DROP COLUMN should come before
 * ADD COLUMN.	Therefore phase 1 divides the subcommands into multiple
 * lists, one for each logical "pass" of phase 2.
 *
 * ATRewriteTables performs phase 3 for those tables that need it.
 *
 * Thanks to the magic of MVCC, an error anywhere along the way rolls back
 * the whole operation; we don't have to do anything special to clean up.
 */
void
AlterTable(AlterTableStmt *stmt)
{
	Relation rel = relation_openrv(stmt->relation, AccessExclusiveLock);
	int			expected_refcnt;

	ATVerifyObject(stmt, rel);
	
	/*
	 * Disallow ALTER TABLE when the current backend has any open reference
	 * to it besides the one we just got (such as an open cursor or active
	 * plan); our AccessExclusiveLock doesn't protect us against stomping on
	 * our own foot, only other people's feet!
	 *
	 * Note: the only case known to cause serious trouble is ALTER COLUMN TYPE,
	 * and some changes are obviously pretty benign, so this could possibly
	 * be relaxed to only error out for certain types of alterations.  But
	 * the use-case for allowing any of these things is not obvious, so we
	 * won't work hard at it for now.
	 */
	expected_refcnt = rel->rd_isnailed ? 2 : 1;
	if (rel->rd_refcnt != expected_refcnt)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("relation \"%s\" is being used by active queries in this session",
						RelationGetRelationName(rel))));

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		LockRelationOid(RelationRelationId, RowExclusiveLock);
		LockRelationOid(TypeRelationId, RowExclusiveLock);
		LockRelationOid(DependRelationId, RowExclusiveLock);
	}

	ATController(rel,
				 stmt->cmds,
				 interpretInhOption(stmt->relation->inhOpt),
				 &stmt->oidInfoCount,
				 &stmt->oidInfo,
				 &stmt->oidmap);

	if (Gp_role == GP_ROLE_DISPATCH)
		CdbDispatchUtilityStatement((Node *) stmt, "AlterTable");
}

/*
 * AlterTableInternal
 *
 * ALTER TABLE with target specified by OID
 *
 * We do not reject if the relation is already open, because it's quite likely
 * that one or more layers of caller have it open.  That means it is unsafe to
 * use this entry point for alterations that could break existing query plans.
 * On the assumption it's not used for such, we don't have to reject pending
 * AFTER triggers, either.
 *
 * It is also unsafe to use this function for any Alter Table subcommand that
 * requires rewriting the table or creating toast tables, because that requires
 * creating relfilenodes outside of a context that understands dispatch.
 * Commands that rewrite the table include: adding or altering columns, changing
 * the tablespace, etc.
 */
void
AlterTableInternal(Oid relid, List *cmds, bool recurse)
{
	ATController(relation_open(relid, AccessExclusiveLock),
				 cmds,
				 recurse,
				 0,
				 NULL,
				 NULL);
}

static void
ATController(Relation rel, List *cmds, bool recurse, 
			 int * oidInfoCount, TableOidInfo ** oidInfo, List **poidmap)
{
	List	   *wqueue = NIL;
	ListCell   *lcmd;
	int			ocount = 0;
	TableOidInfo * oids = NULL;
	bool is_partition = false;
	bool is_data_remote = (RelationIsExternal(rel) || RelationIsForeign(rel));
#ifdef USE_ASSERT_CHECKING
	Oid			relid = RelationGetRelid(rel);
#endif

	cdb_sync_oid_to_segments();

	/* Phase 1: preliminary examination of commands, create work queue */
	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		if (cmd->subtype == AT_PartAddInternal)
			is_partition = true;

		ATPrepCmd(&wqueue, rel, cmd, recurse, false);
	}

	if (is_partition)
		relation_close(rel, AccessExclusiveLock);
	else
		/* Close the relation, but keep lock until commit */
		relation_close(rel, NoLock);

	if (Gp_role == GP_ROLE_DISPATCH && oidInfoCount != NULL)
	{
		*oidInfoCount = list_length(wqueue);
		*oidInfo = palloc0(sizeof(TableOidInfo)*(*oidInfoCount));
	}

	if (oidInfoCount != NULL)
		ocount = *oidInfoCount;
	if (oidInfo != NULL)
		oids = *oidInfo;

	/* Phase 2: update system catalogs */
	ATRewriteCatalogs(&wqueue, ocount, oids, poidmap);

	/*
	 * Build OID map info.
	 *
	 * If we've recursed (i.e., expanded) an ALTER TABLE SET DISTRIBUTED
	 * command from a master table to its children, then we need to extract
	 * the OID map (used to ensure that all segments map old index OIDs to
	 * new index OIDs) for each of the sub commands and put them in the
	 * master command. This is because we only dispatch the command on the
	 * master table to the segments, not each expanded command. We expect
	 * the segments to do the same expansion.
	 */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		List *umap = NIL; /* the full OID map for all relations */
		ListCell *lc;
		AlterTableCmd *mastercmd = NULL; /* the command which was expanded */

		/* 
		 * Iterate over the work queue looking for SET WITH DISTRIBUTED
		 * statements.
		 */
		foreach(lc, wqueue)
		{
			ListCell *lc2;
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(lc);

			/* AT_PASS_MISC is used for SET DISTRIBUTED BY */
			List *subcmds = (List *)tab->subcmds[AT_PASS_MISC];

			foreach(lc2, subcmds)
			{
				AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc2);

				if (cmd->subtype == AT_SetDistributedBy)
				{
					if (!mastercmd)
					{
						Assert(tab->relid == relid);
						mastercmd = cmd;
					}

					/*
					 * cmd->def stores the data we're sending to the QEs.
					 * The second element is where we stash QE data to drive
					 * the command. The second element of qe_data is our
					 * OID map. See ATExecSetDistributedBy().
					 *
					 * Yes, this is begging to be improved.
					 */
					List *data = (List *)cmd->def;
					List *qe_data = lsecond(data);

					if (qe_data && list_length(qe_data) >= 2)
					{
						List *oidmap = lsecond(qe_data);

						umap = list_concat(umap, oidmap);
					}
				}
			}
		}

		/* Finally, push it all back into the master command */
		if (mastercmd && umap)
		{
			List *qe_data = lsecond((List *)mastercmd->def);

			if (list_length(qe_data) >= 2)
				lsecond(qe_data) = umap;
			else if (list_length(qe_data) == 1)
			{
				qe_data = lappend(qe_data, umap);

				/* 
				 * We've changed the pointer, save it back to the master
				 * definition.
				 */
				lsecond((List *)mastercmd->def) = qe_data;
			}
		}
	}

	/* 
	 * Phase 3: scan/rewrite tables as needed. If the data is in an external
	 * table, no need to rewrite it or to add toast.
	 */
	if (!is_data_remote)
	{
		ATRewriteTables(&wqueue, ocount, oids, poidmap);

		ATAddToastIfNeeded(&wqueue, ocount, oids, poidmap);		
	}

}

static bool
relation_has_supers(Oid relid)
{
	ScanKeyData key;
	SysScanDesc scan;
	bool found = false;

	Relation catalogRelation = heap_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, SnapshotNow, 1, &key);

	if (HeapTupleIsValid(systable_getnext(scan)))
		found = true;

	systable_endscan(scan);
	heap_close(catalogRelation, AccessShareLock);

	return found;
}

/* Construct the PgPartRule for a branch of a partitioning hierarchy.
 *
 * 		rel - the partitioned relation (top-level)
 *		cmd - an AlterTableCmd, possibly nested in type AT_PartAlter AlterTableCmds
 *            identifying a subset of the parts of the partitioned relation.
 */
static PgPartRule *
get_pprule_from_ATC(Relation rel, AlterTableCmd *cmd)
{
	List *pids = NIL; /* of AlterPartitionId */
	AlterPartitionId *pid = NULL;
	PgPartRule *pprule = NULL;
	AlterPartitionId *work_partid = NULL;
	
	AlterTableCmd *atc = cmd;
	
	
	/* Get list of enclosing ALTER PARTITION ids. */
	while ( atc->subtype == AT_PartAlter )
	{
		AlterPartitionCmd *apc = (AlterPartitionCmd*)atc->def;
		
		pid = (AlterPartitionId*)apc->partid;
		Insist(IsA(pid, AlterPartitionId));
		
		atc = (AlterTableCmd*)apc->arg1;
		Insist(IsA(atc, AlterTableCmd));
		
		pids = lappend(pids, pid);
	}
	
	/* The effective ALTER TABLE command is in atc.
	 * The pids list (of AlterPartitionId nodes) represents the path to 
	 * top partitioning branch of rel.  Since we are only called for
	 * branches and leaves (never the root) of the partition, the pid
	 * list should not empty.
	 *
	 * Use the AlterPartitionId interpretter, get_part_rule, to do
	 * the interpretation.
	 */
	Insist( list_length(pids) > 0 );
	
	work_partid = makeNode(AlterPartitionId);
	
	work_partid->idtype = AT_AP_IDList;
	work_partid->partiddef = (Node*)pids;
	work_partid->location = -1;
	
	pprule = get_part_rule(rel, 
						   work_partid, 
						   true, true, /* parts must exist */
						   CurrentMemoryContext,
						   NULL, /* no implicit results */
						   false /* no template rules */
						   );
	
	return pprule;
}

/* Return the pg_class OIDs of the relations representing the parts of
 * a partitioned table designated by the given AlterTable command.  
 *
 * 		rel - the partitioned relation (top-level)
 *		cmd - an AlterTableCmd, possibly nested in type AT_PartAlter AlterTableCmds
 *            identifying a subset of the parts of the partitioned relation.
 */
static List *
basic_AT_oids(Relation rel, AlterTableCmd *cmd)
{
	PgPartRule *pprule = get_pprule_from_ATC(rel, cmd);
	
	if ( ! pprule )
		return NIL;
	
	return all_prule_relids(pprule->topRule);	
}

/*
 * Return the basic AlterTableCmd found by peeling off intervening layers of
 * ALTER PARTITION from the given AlterTableCmd.
 */
static AlterTableCmd *basic_AT_cmd(AlterTableCmd *cmd)
{
	while ( cmd->subtype == AT_PartAlter )
	{
		AlterPartitionCmd *apc = (AlterPartitionCmd*)cmd->def;
		Insist(IsA(apc, AlterPartitionCmd));
		cmd = (AlterTableCmd*)apc->arg1;
		Insist(IsA(cmd, AlterTableCmd));
	}
	return cmd;
}

/*
 * ATPrepCmd
 *
 * Traffic cop for ALTER TABLE Phase 1 operations, including simple
 * recursion and permission checks.
 *
 * Caller must have acquired AccessExclusiveLock on relation already.
 * This lock should be held until commit.
 */
static void
ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing)
{
	AlteredTableInfo *tab;
	int			pass;

	/* Find or create work queue entry for this table */
	tab = ATGetQueueEntry(wqueue, rel);

	/*
	 * Copy the original subcommand for each table.  This avoids conflicts
	 * when different child tables need to make different parse
	 * transformations (for example, the same column may have different column
	 * numbers in different children).
	 */
	if (recursing)
		cmd = copyObject(cmd);

	/*
	 * Do permissions checking, recursion to child tables if needed, and any
	 * additional phase-1 processing needed.
	 */
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
			ATSimplePermissions(rel, false);

			/*
			 * test that this is allowed for partitioning, but only if we aren't
			 * recursing.
			 */
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			/* Performs own recursion */
			ATPrepAddColumn(wqueue, rel, recurse, cmd);
			pass = AT_PASS_ADD_COL;
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */

			/*
			 * We allow defaults on views so that INSERT into a view can have
			 * default-ish behavior.  This works because the rewriter
			 * substitutes default values into INSERTs before it expands
			 * rules.
			 */
			ATSimplePermissions(rel, true);
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = cmd->def ? AT_PASS_ADD_CONSTR : AT_PASS_DROP;
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			ATSimplePermissions(rel, false);
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_DROP;
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			if (!cmd->part_expanded)
				ATPartitionCheck(cmd->subtype, rel, false, recursing);
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_SetStatistics:	/* ALTER COLUMN STATISTICS */
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* Performs own permission checks */
			ATPrepSetStatistics(rel, cmd->name, cmd->def);
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_SetStorage:		/* ALTER COLUMN STORAGE */
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_DropColumn:		/* DROP COLUMN */
		case AT_DropColumnRecurse:
			ATSimplePermissions(rel, false);
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			/* Recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_DropColumnRecurse;
			pass = AT_PASS_DROP;
			break;
		case AT_AddIndex:		/* ADD INDEX */
			ATSimplePermissions(rel, false);
			/* Any recursion for partitioning is done in ATExecAddIndex() itself */
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_INDEX;
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			ATSimplePermissions(rel, false);
			/* (!recurse &&  !recursing) is supposed to detect the ONLY clause.
			 * We allow operations on the root of a partitioning hierarchy, but
			 * not ONLY the root.
			 */
			ATPartitionCheck(cmd->subtype, rel, (!recurse && !recursing), recursing);

			/*
			 * Currently we recurse only for CHECK constraints, never for
			 * foreign-key constraints.  UNIQUE/PKEY constraints won't be seen
			 * here.
			 */
			if (IsA(cmd->def, Constraint))
				ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_DropConstraint:	/* DROP CONSTRAINT */
			ATSimplePermissions(rel, false);
			/* (!recurse &&  !recursing) is supposed to detect the ONLY clause.
			 * We allow operations on the root of a partitioning hierarchy, but
			 * not ONLY the root.
			 */
			ATPartitionCheck(cmd->subtype, rel, (!recurse && !recursing), recursing);
			/* Performs own recursion */
			ATPrepDropConstraint(wqueue, rel, recurse, cmd);
			pass = AT_PASS_DROP;
			break;
		case AT_DropConstraintQuietly:	/* DROP CONSTRAINT for child */
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_DROP;
			break;
		case AT_AlterColumnType:		/* ALTER COLUMN TYPE */
			ATSimplePermissions(rel, false);
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			/* Performs own recursion */
			ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd);
			pass = AT_PASS_ALTER_TYPE;
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			{
				bool do_recurse = false;

				if (Gp_role == GP_ROLE_DISPATCH)
				{
					ATPartitionCheck(cmd->subtype, rel, false, recursing);
					if (rel_is_partitioned(RelationGetRelid(rel)))
					{
						do_recurse = true;
						/* tell QE to recurse */
						cmd->def = (Node *)makeInteger(1);
					}
				}
				else if (Gp_role == GP_ROLE_EXECUTE)
				{
					if (cmd->def && intVal(cmd->def))
						do_recurse = true;
				}

				if (do_recurse)
					ATSimpleRecursion(wqueue, rel, cmd, recurse);

				pass = AT_PASS_MISC;
			}
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATSimplePermissions(rel, false);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
			ATSimplePermissions(rel, false);
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			/* Performs own recursion */
			if (rel->rd_rel->relhasoids)
			{
				AlterTableCmd *dropCmd = makeNode(AlterTableCmd);

				dropCmd->subtype = AT_DropColumn;
				dropCmd->name = pstrdup("oid");
				dropCmd->behavior = cmd->behavior;
				ATPrepCmd(wqueue, rel, dropCmd, recurse, false);
			}
			pass = AT_PASS_DROP;
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */
			ATSimplePermissionsRelationOrIndex(rel);
			/* This command never recurses, but the offered relation may be partitioned, 
			 * in which case, we need to act as if the command specified the top-level
			 * list of parts.
			 */
			if (Gp_role == GP_ROLE_DISPATCH && rel_is_partitioned(rel->rd_id) )
			{
				PartitionNode *pnode = NULL;
				List *all_oids = NIL;
				
				pnode = get_parts(rel->rd_id, 
								  0, /**/
								  InvalidOid, /**/
								  false, /**/
								  CurrentMemoryContext);
				
				all_oids = lcons_oid(rel->rd_id, all_partition_relids(pnode));
				
				Assert( cmd->partoids == NIL );
				cmd->partoids = all_oids;
				
				ATPartsPrepSetTableSpace(wqueue, rel, cmd, all_oids);
			}
			else if (Gp_role == GP_ROLE_EXECUTE && cmd->partoids)
			{
				ATPartsPrepSetTableSpace(wqueue, rel, cmd, cmd->partoids);
			}
			else if (Gp_role == GP_ROLE_UTILITY)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
 						 errmsg("SET TABLESPACE is not supported in utility mode")));
			}
			else
				ATPrepSetTableSpace(tab, rel, cmd->name);
			pass = AT_PASS_MISC;	/* doesn't actually matter */
			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:		/* RESET (...) */
			ATSimplePermissionsRelationOrIndex(rel);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_SetDistributedBy:	/* SET DISTRIBUTED BY */
			if ( !recursing ) /* MPP-5772, MPP-5784 */
			{
				Oid relid = RelationGetRelid(rel);
				PartStatus ps = rel_part_status(relid);

				if ( recurse ) /* Normal ALTER TABLE */
				{
					switch (ps)
					{
						case PART_STATUS_NONE:
						case PART_STATUS_ROOT:
						case PART_STATUS_LEAF:
							break;

						case PART_STATUS_INTERIOR:
							/*Reject interior branches of partitioned tables.*/
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
									 errmsg("can't set the distribution"
											" policy of \"%s\"",
											RelationGetRelationName(rel)),
									 errhint("Distribution policy may be set"
											 " for an entire partitioned table"
											 " or one of its leaf parts;"
											 " not for an interior branch."
											 ),
									 errOmitLocation(true)));
							break; /* tidy */
					}
				}
				else /* ALTER TABLE ONLY */
				{
					switch (ps)
					{
						case PART_STATUS_NONE:
						case PART_STATUS_LEAF:
							break;

						case PART_STATUS_ROOT:
						case PART_STATUS_INTERIOR:
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
									 errmsg("can't set the distribution policy "
											"of ONLY \"%s\"",
											RelationGetRelationName(rel)),
									 errhint("Distribution policy may be set "
											 "for an entire partitioned table"
											 " or one of its leaf parts."
											 ),
									 errOmitLocation(true)));
							break; /* tidy */
					}
				}

				if ( ps == PART_STATUS_LEAF )
				{
					Oid ptrelid = rel_partition_get_master(relid);
					List *dist_cnames = lsecond((List*)cmd->def);

					/*	might be null if no policy set, e.g. just
					 *  a change of storage options...
					 */
					if (dist_cnames)
					{
						Relation ptrel = heap_open(ptrelid,
												   AccessShareLock);
						Assert(IsA(dist_cnames, List));

						if (! can_implement_dist_on_part(ptrel,
														 dist_cnames) )
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("cannot SET DISTRIBUTED BY for %s",
											RelationGetRelationName(rel)),
									 errhint("Leaf distribution policy must be"
											 " random or match that of the"
											 " entire partitioned table."),
									 errOmitLocation(true)
									 ));
						heap_close(ptrel, AccessShareLock);
					}
				}
			}
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			pass = AT_PASS_MISC;
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER variants */
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* DISABLE TRIGGER variants */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
			ATSimplePermissions(rel, false);
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_AddInherit:		/* INHERIT / NO INHERIT */
		case AT_DropInherit:
			ATSimplePermissions(rel, false);
			ATPartitionCheck(cmd->subtype, rel, true, recursing);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
			/* CDB: Partitioned Table commands */
		case AT_PartExchange:			/* Exchange */
			
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
			
			if (Gp_role == GP_ROLE_UTILITY)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
 						 errmsg("EXCHANGE is not supported in utility mode")));
			
			pass = AT_PASS_MISC;
			
			ATPrepExchange(rel, (AlterPartitionCmd *)cmd->def);
			
			break;
		case AT_PartMerge:
		{
			AlterPartitionCmd  *pc    	= (AlterPartitionCmd *) cmd->def;
			PgPartRule   	   *prule1	= NULL;
			PgPartRule		   *prule2	= NULL;
			PgPartRule		   *prule3	= NULL;
			ListCell		   *lc;
			List			   *source_rels = NIL;
			Relation			target = NULL;

			ATPartitionCheck(cmd->subtype, rel, false, recursing);

			if (Gp_role == GP_ROLE_UTILITY)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
 						 errmsg("MERGE is not supported in utility mode")));
			else if (Gp_role == GP_ROLE_DISPATCH)
			{
				Relation srel;
				AlterPartitionId *pid = (AlterPartitionId *) pc->partid;

				prule1 = get_part_rule(rel, pid, true, true,
									   CurrentMemoryContext, NULL, false);

				prule2 = get_part_rule(rel, (AlterPartitionId *)pc->arg1,
									   true, true,
									   CurrentMemoryContext, NULL, false);

				if (pc->arg2)
				{
					AlterPartitionCmd *pc2 = (AlterPartitionCmd *)pc->arg2;

					prule3 = get_part_rule(rel,
										   (AlterPartitionId *)pc2->partid,
										   true, true,
										   CurrentMemoryContext, NULL, false);
				}

				if (prule1)
				{
					/* cannot exchange a hash partition */
					if ('h' == prule1->pNode->part->parkind)
						ereport(ERROR,
							(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
								 errmsg("cannot merge HASH partition%s "
									    "of relation \"%s\"",
										prule1->partIdStr,
										RelationGetRelationName(rel))));
				}

				srel = heap_open(prule1->topRule->parchildrelid,
								 AccessExclusiveLock);
				source_rels = lappend(source_rels, srel);

				srel = heap_open(prule2->topRule->parchildrelid,
								 AccessExclusiveLock);
				if (prule3)
				{
					source_rels = lappend(source_rels, srel);
					target = heap_open(prule3->topRule->parchildrelid,
									   AccessExclusiveLock);
				}
				else
					target = srel;
			}
			else
			{
				Relation srel;
				srel = heap_openrv((RangeVar *)pc->partid,
								   AccessExclusiveLock);
				source_rels = lappend(source_rels, srel);

				srel = heap_openrv((RangeVar *)pc->arg1, AccessExclusiveLock);

				if (pc->arg2)
				{
					source_rels = lappend(source_rels, srel);
					target = heap_openrv((RangeVar *)pc->arg2,
										 AccessExclusiveLock);
				}
				else
					target = heap_openrv((RangeVar *)pc->arg1,
										 AccessExclusiveLock);
			}

			ATSimplePermissions(rel, false);

			foreach(lc, source_rels)
				ATSimplePermissions(lfirst(lc), false);

			ATSimplePermissions(target, false);

			foreach(lc, source_rels)
				heap_close(lfirst(lc), NoLock);

			heap_close(target, NoLock);

			pass = AT_PASS_MISC;
			break;
		}

		case AT_PartSplit:				/* Split */
		{
			AlterPartitionCmd	*pc    	= (AlterPartitionCmd *) cmd->def;
			PgPartRule			*prule1	= NULL;
			PgPartRule			*prule2	= NULL;
			PgPartRule			*prule3	= NULL;
			Relation			 target = NULL;

			ATPartitionCheck(cmd->subtype, rel, false, recursing);

			if (Gp_role == GP_ROLE_UTILITY)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
 						 errmsg("SPLIT is not supported in utility mode")));
			else if (Gp_role == GP_ROLE_DISPATCH)
			{
				AlterPartitionId *pid = (AlterPartitionId *) pc->partid;
				AlterPartitionCmd *cmd2;
				Node *at;
				bool is_at = true;
				List *todo;
				ListCell *l;
				bool rollup_vals = false;

				/*
				 * Need to do a bunch of validation:
				 * 0) Target exists
				 * 0.5) Shouldn't have children
				 * 1) The usual permissions checks
				 * 2) Not called on HASH
				 * 3) AT () parameter falls into constraint specified
				 * 4) INTO partitions don't exist
				 */

				/* We'll error out if it doesn't exist */
				prule1 = get_part_rule(rel, pid, true, true,
									   CurrentMemoryContext, NULL, false);

				if (prule1->topRule->children)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot split partition with child "
									"partitions"),
							 errhint("Try splitting the child partitions."),
							 errOmitLocation(true)));

				target = heap_open(prule1->topRule->parchildrelid,
								   AccessExclusiveLock);

				if (linitial((List *)pc->arg1))
					is_at = false;

				if (prule1->topRule->parisdefault &&
					prule1->pNode->part->parkind == 'r' &&
					is_at)
				{
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("AT clause cannot be used when splitting "
									"a default RANGE partition"),
							 errOmitLocation(true)));
				}
				else if (prule1->pNode->part->parkind == 'l' && !is_at)
				{
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("cannot SPLIT DEFAULT PARTITION "
									"with LIST"),
							errhint("Use SPLIT with the AT clause instead."),
							errOmitLocation(true)));

				}

				if (prule1->pNode->part->parkind == 'h')
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("SPLIT is not supported for "
									"HASH partitions"),
							 errOmitLocation(true)));

				if (linitial((List *)pc->arg1))
					is_at = false;

				todo = (List *)pc->arg1;

				pc->arg1 = (Node *)NIL;
				foreach(l, todo)
				{
					List *l1;
					ListCell *lc;
					Node *t = lfirst(l);

					if (!t)
					{
						pc->arg1 = (Node *)lappend((List *)pc->arg1, NULL);
						continue;
					}
					else if (is_at)
						l1 = (List *)t;
					else if (!is_at)
					{
						Node *n = t;
						PartitionRangeItem *ri = (PartitionRangeItem *)n;

						Assert(IsA(n, PartitionRangeItem));

						l1 = ri->partRangeVal;
					}

					if (IsA(linitial(l1), A_Const) ||
						IsA(linitial(l1), TypeCast) ||
						IsA(linitial(l1), FuncExpr) ||
						IsA(linitial(l1), OpExpr))
					{
						List *new = NIL;
						ListCell *lc;

						foreach(lc, l1)
							new = lappend(new, list_make1(lfirst(lc)));

						l1 = new;
						rollup_vals = true;
					}
					else
						Insist(IsA(linitial(l1), List));

					foreach(lc, l1)
					{
						List *vals = lfirst(lc);
						ListCell *lc2;
						int i = 0;
						ParseState *pstate = make_parsestate(NULL);

						if (prule1->pNode->part->parnatts != list_length(vals))
						{
							AttrNumber parnatts = prule1->pNode->part->parnatts;
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("partition being split has "
											"%i column%s but parameter "
											"has %i column%s",
											parnatts,
											parnatts > 1 ? "s" : "",
											list_length(vals),
										   list_length(vals) > 1 ? "s" : ""),
									  errOmitLocation(true)));
						}

						vals = (List *)transformExpressionList(pstate, vals);

						free_parsestate(&pstate);

						foreach(lc2, vals)
						{
							Node *n = lfirst(lc2);
							AttrNumber attnum =
									prule1->pNode->part->paratts[i];
							Oid typid =
									rel->rd_att->attrs[attnum - 1]->atttypid;
							int32 typmod =
									rel->rd_att->attrs[attnum - 1]->atttypmod;
							char parkind = prule1->pNode->part->parkind;
							PartitionByType t = (parkind == 'r') ?
												 PARTTYP_RANGE : PARTTYP_LIST;

							n = coerce_partition_value(n, typid, typmod, t);

							lfirst(lc2) = n;

							i++;
						}
						lfirst(lc) = vals;
					}

					if (rollup_vals)
					{
						/* now, unroll */
						if (prule1->pNode->part->parkind == 'r')
						{
							List *new = NIL;
							ListCell *lc;
							Node *n;

							foreach(lc, l1)
								new = lappend(new, linitial(lfirst(lc)));

							if (is_at)
								n = (Node *)new;
							else
							{
								PartitionRangeItem *ri = lfirst(l);

								ri->partRangeVal = new;
								n = (Node *)ri;
							}
							pc->arg1 = (Node *)lappend((List *)pc->arg1, n);
						}
						else
						{
							/* for LIST, leave it as is */
							pc->arg1 = (Node *)lappend((List *)pc->arg1,
														l1);
						}
					}
					else
						pc->arg1 = (Node *)lappend((List *)pc->arg1, l1);
				}

				at = lsecond((List *)pc->arg1);
				if (prule1->pNode->part->parkind == 'l')
				{

					if (prule1->topRule->parisdefault)
					{
						/*
						 * Check that AT() value doesn't exist in any existing
						 * definition.
						 */

					}
					else
					{
						/* all elements in AT() must match */
						ListCell *lcat;
						List *lv = (List *)prule1->topRule->parlistvalues;
						int16 nkeys = prule1->pNode->part->parnatts;
						List *atlist = (List *)at;
						int nmatches = 0;

						foreach(lcat, atlist)
						{
							List *cols = lfirst(lcat);
							ListCell *parvals = list_head(lv);
							bool found = false;

							Assert(list_length(cols) == nkeys);

							while (parvals)
							{
								List *vals = lfirst(parvals);
								ListCell *lcv = list_head(vals);
								ListCell *lcc = list_head(cols);
								int parcol;
								bool matched = true;

								for (parcol = 0; parcol < nkeys; parcol++)
								{
									Oid opclass =
										prule1->pNode->part->parclass[parcol];
									Oid funcid = get_opclass_proc(opclass, 0,
																 BTORDER_PROC);
									Const *v = lfirst(lcv);
									Const *c = lfirst(lcc);
									Datum d;

									if (v->constisnull && c->constisnull)
										continue;
									else if (v->constisnull || c->constisnull)
									{
										matched = false;
										break;
									}

									d = OidFunctionCall2(funcid, c->constvalue,
														 v->constvalue);

									if (DatumGetInt32(d) != 0)
									{
										matched = false;
										break;
									}

									lcv = lnext(lcv);
									lcc = lnext(lcc);
								}

								if (!matched)
								{
									parvals = lnext(parvals);
									continue;
								}
								else
								{
									found = true;
									nmatches++;
									break;
								}
							}
							if (!found)
								ereport(ERROR,
										(errcode(ERRCODE_WRONG_OBJECT_TYPE),
										errmsg("AT clause parameter is not "
											   "a member of the target "
											   "partition specification"),
										errOmitLocation(true)));


						}

						if (nmatches >= list_length(lv))
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("AT clause cannot contain all "
											"values in partition%s",
											prule1->partIdStr),
									 errOmitLocation(true)));

					}
				}
				else
				{
					/* XXX: handle split of default */
					if (prule1->topRule->parisdefault)
					{
					}
					else
					{
						/* at must be in range */
						Const	*start	  = NULL;
						Const	*end	  = NULL;
						Const	*atval	  = NULL;
						Oid		 opclass  = prule1->pNode->part->parclass[0];
						Oid		 funcid	  = get_opclass_proc(opclass, 0,
															 BTORDER_PROC);
						Node	*n;
						Datum	 res;
						int32	 ret;
						bool	 in_range = true;

						n = (Node *)linitial((List *)at);

						Assert(IsA(n, Const));

						atval = (Const *)n;

						/* XXX: ignore composite key range for now */
						if (prule1->topRule->parrangestart)
							start = linitial((List *)prule1->topRule->parrangestart);

						/* MPP-6589: allow "open" start or end */
						if (start)
						{
							Insist(exprType((Node *)n) == exprType((Node *)start));
							res = OidFunctionCall2(funcid, start->constvalue,
												   atval->constvalue);

							ret = DatumGetInt32(res);
						}
						else
						{
							/* MPP-6589: no start - check end */
							Assert(prule1->topRule->parrangeend);
							ret = -1;
						}

						if (ret > 0)
							in_range = false;
						else if (ret == 0)
						{
							if (!prule1->topRule->parrangestartincl)
								in_range = false;
						}
						else
						{
							/* in the range, test end */
							if (prule1->topRule->parrangeend)
							{
								end = linitial((List *)prule1->topRule->parrangeend);
								res = OidFunctionCall2(funcid,
													   atval->constvalue,
											   		   end->constvalue);

								ret = DatumGetInt32(res);

								if (ret > 0)
									in_range = false;
								else if (ret == 0)
									if (!prule1->topRule->parrangeendincl)
										in_range = false;
							}
							/* if no end, remains "in range" */
						}

						if (!in_range)
						{
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
									errmsg("AT clause parameter is not "
										   "a member of the target "
										   "partition specification"),
									errOmitLocation(true)));
						}
					}
				}

				/*
				 * pc->partid == target
				 * pc->arg1 == AT
				 * pc->arg2 == AlterPartitionCmd (partid == 1, arg1 = 2)
				 */
				if (PointerIsValid(pc->arg2))
				{
					cmd2 = (AlterPartitionCmd *)pc->arg2;

					prule2 = get_part_rule(rel,
										   (AlterPartitionId *)cmd2->partid,
										   false, false, CurrentMemoryContext,
										   NULL, false);

					prule3 = get_part_rule(rel, (AlterPartitionId *)cmd2->arg1,
										   false, false, CurrentMemoryContext,
										   NULL, false);
					/* both can't exist */
					if (prule2 && prule3)
					{
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("both INTO partitions "
										"already exist"),
								 errOmitLocation(true)));
					}
					else if (prule1->topRule->parisdefault &&
							 !prule2 && !prule3)
					{
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("default partition name missing "
										"from INTO clause"),
								 errOmitLocation(true)));

					}
				}
				ATSimplePermissions(target, false);
				heap_close(target, NoLock);
			}
			else
			{
				Insist(IsA(pc->partid, Integer));
				Insist(IsA(pc->arg1, RangeVar));
				Insist(IsA(pc->arg2, RangeVar));
			}
			pass = AT_PASS_MISC;
			break;
		}

		case AT_PartAlter:				/* Alter */
			if ( Gp_role == GP_ROLE_DISPATCH && recurse && ! recursing )
			{
				AlterTableCmd *basecmd = basic_AT_cmd(cmd);
				
				switch ( basecmd->subtype )
				{
					case AT_SetTableSpace:
						cmd->partoids = basic_AT_oids(rel,cmd);
						ATPartsPrepSetTableSpace(wqueue, rel, basecmd, cmd->partoids);
						break;
					
					default:
						/* Not a for-each-subsumed-part-type command. */
						break;
				}
			}
			else if (Gp_role == GP_ROLE_EXECUTE && cmd->partoids)
			{
				AlterTableCmd *basecmd = basic_AT_cmd(cmd);

				switch ( basecmd->subtype )
				{
					case AT_SetTableSpace:
						ATPartsPrepSetTableSpace(wqueue, rel, basecmd, cmd->partoids);
						break;
						
					default:
						/* Not a for-each-subsumed-part-type command. */
						break;
				}
			}
			else if (Gp_role == GP_ROLE_UTILITY)
			{
				if (basic_AT_cmd(cmd)->subtype == AT_SetTableSpace)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("SET TABLESPACE is not supported in utility mode")));
			}
			pass = AT_PASS_MISC;
			break;				
			
		case AT_PartAdd:				/* Add */
		case AT_PartCoalesce:			/* Coalesce */
		case AT_PartDrop:				/* Drop */
		case AT_PartRename:				/* Rename */
		case AT_PartSetTemplate:		/* Set Subpartition Template */
		case AT_PartTruncate:			/* Truncate */
		case AT_PartAddInternal:		/* internal partition creation */
		case AT_PartModify:             /* Modify */
			ATSimplePermissions(rel, false);
			ATPartitionCheck(cmd->subtype, rel, false, recursing);
				/* XXX XXX XXX */
			/* This command never recurses */
			/* No command-specific prep needed */
			/* XXX: check permissions */
			pass = AT_PASS_MISC;
			break;

		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			pass = 0;			/* keep compiler quiet */
			break;
	}

	/* Add the subcommand to the appropriate list for phase 2 */
	tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}

/*
 * ATRewriteCatalogs
 *
 * Traffic cop for ALTER TABLE Phase 2 operations.	Subcommands are
 * dispatched in a "safe" execution order (designed to avoid unnecessary
 * conflicts).
 */
static void
ATRewriteCatalogs(List **wqueue, 
				  int oidInfoCount, TableOidInfo * oidInfo, List **poidmap)
{
	int			pass;
	ListCell   *ltab;

	/*
	 * We process all the tables "in parallel", one pass at a time.  This is
	 * needed because we may have to propagate work from one table to another
	 * (specifically, ALTER TYPE on a foreign key's PK has to dispatch the
	 * re-adding of the foreign key constraint to the other table).  Work can
	 * only be propagated into later passes, however.
	 */
	for (pass = 0; pass < AT_NUM_PASSES; pass++)
	{
		/* Go through each table that needs to be processed */
		foreach(ltab, *wqueue)
		{
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
			List	   *subcmds = tab->subcmds[pass];
			Relation	rel;
			ListCell   *lcmd;

			if (subcmds == NIL)
				continue;

			/*
			 * Exclusive lock was obtained by phase 1, needn't get it again
			 */
			rel = relation_open(tab->relid, NoLock);

			foreach(lcmd, subcmds)
			{
				AlterTableCmd *atc = lfirst(lcmd);

				ATExecCmd(wqueue, tab, rel, atc);

				/*
				 * SET DISTRIBUTED BY() calls RelationForgetRelation(),
				 * which will scribble on rel, so we must re-open it.
				 */
				if (atc->subtype == AT_SetDistributedBy)
					rel = relation_open(tab->relid, NoLock);
			}
			/*
			 * After the ALTER TYPE pass, do cleanup work (this is not done in
			 * ATExecAlterColumnType since it should be done only once if
			 * multiple columns of a table are altered).
			 */
			if (pass == AT_PASS_ALTER_TYPE)
				ATPostAlterTypeCleanup(wqueue, tab);

			relation_close(rel, NoLock);
		}
	}

}

static void
ATAddToastIfNeeded(List **wqueue, 
				   int oidInfoCount, TableOidInfo * oidInfo, List **poidmap)
{

	ListCell   *ltab;
	int			m = 0;
	
	/*
	 * Check to see if a toast table must be added, if we executed any
	 * subcommands that might have added a column or changed column storage.
	 */
	foreach(ltab, *wqueue)
	{
		Oid tOid = InvalidOid;
		Oid tiOid = InvalidOid;
		Oid	*toastComptypeOid = NULL;
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		bool is_part = !rel_needs_long_lock(tab->relid);

		if (tab->relkind == RELKIND_RELATION &&
			(tab->subcmds[AT_PASS_ADD_COL] ||
			 tab->subcmds[AT_PASS_ALTER_TYPE] ||
			 tab->subcmds[AT_PASS_COL_ATTRS]))
		{
			Relation rel;
			Oid reltablespace;

			/*
			 * For upgrade, don't create a toast table.
			 */
			if (gp_upgrade_mode)
				continue;

			/* 
			 * Determine if we need to create a toast table.
			 */
			rel = heap_open(tab->relid, NoLock);
			if (rel->rd_rel->reltoastrelid != InvalidOid)
			{  /* Already have one */
				heap_close(rel, NoLock);
				continue;
			}
			if (!RelationNeedsToastTable(rel))
			{  /* Don't need one */
				heap_close(rel, NoLock);
				continue;
			}
			reltablespace = rel->rd_rel->reltablespace;
			heap_close(rel, NoLock);
			
			/* 
			 * We do not currently have a toast table, but we need one.  If we
			 * have not already allocated oids in the oidinfo then we need to do
			 * so now.
			 */
			if (Gp_role == GP_ROLE_DISPATCH)
			{
				Relation	pg_class_desc = NULL;
				Relation    pg_type_desc = NULL;

				Assert(oidInfo);
				Assert(m < oidInfoCount);

				if (oidInfo[m].toastOid == InvalidOid)
				{
					pg_class_desc = heap_open(RelationRelationId, RowExclusiveLock);
					oidInfo[m].toastOid = GetNewRelFileNode(reltablespace, false, pg_class_desc);
				}
				if (oidInfo[m].toastIndexOid == InvalidOid)
				{
					if (!pg_class_desc)
						pg_class_desc = heap_open(RelationRelationId, RowExclusiveLock);
					oidInfo[m].toastIndexOid = GetNewRelFileNode(reltablespace, false, pg_class_desc);
				}
				if (oidInfo[m].toastComptypeOid == InvalidOid)
				{
					pg_type_desc = heap_open(RelationRelationId, RowExclusiveLock);
					oidInfo[m].toastIndexOid = GetNewRelFileNode(reltablespace, false, pg_type_desc);
				}
				if (pg_class_desc)
					heap_close(pg_class_desc, NoLock);
				if (pg_type_desc)
					heap_close(pg_type_desc, NoLock);
			}

			/*
			 * Normal dispatch/execute mode will always have an oidInfo, but if
			 * we are in utility mode or bootstrap then we might not.  In this
			 * event we fall back to the InvalidOid as initialized above.
			 */
			if (oidInfo != NULL)
			{
				Assert(m < oidInfoCount);
				tOid = oidInfo[m].toastOid;
				tiOid = oidInfo[m].toastIndexOid;
				toastComptypeOid = &oidInfo[m].toastComptypeOid;
			}

			/* Finally create the toast table */
			elog(DEBUG2,"Create Toast %d with Oid %u", m, tOid);
			AlterTableCreateToastTableWithOid(tab->relid, tOid, tiOid, 
											  toastComptypeOid, is_part);
			m++;

		}
	}
}

/*
 * ATExecCmd: dispatch a subcommand to appropriate execution routine
 */
static void
ATExecCmd(List **wqueue, AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd)
{
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
			ATExecAddColumn(tab, rel, (ColumnDef *) cmd->def);
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */
			ATExecColumnDefault(rel, cmd->name, cmd->def);
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			ATExecDropNotNull(rel, cmd->name);
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			ATExecSetNotNull(tab, rel, cmd->name);
			break;
		case AT_SetStatistics:	/* ALTER COLUMN STATISTICS */
			ATExecSetStatistics(rel, cmd->name, cmd->def);
			break;
		case AT_SetStorage:		/* ALTER COLUMN STORAGE */
			ATExecSetStorage(rel, cmd->name, cmd->def);
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			ATExecDropColumn(wqueue, rel, cmd->name, cmd->behavior, false, false);
			break;
		case AT_DropColumnRecurse:		/* DROP COLUMN with recursion */
			ATExecDropColumn(wqueue, rel, cmd->name, cmd->behavior, true, false);
			break;
		case AT_AddIndex:		/* ADD INDEX */
			ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, false,
						   cmd->part_expanded);
			break;
		case AT_ReAddIndex:		/* ADD INDEX */
			ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, true,
						   cmd->part_expanded);
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			ATExecAddConstraint(tab, rel, cmd->def);
			break;
		case AT_DropConstraint:	/* DROP CONSTRAINT */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior, false);
			break;
		case AT_DropConstraintQuietly:	/* DROP CONSTRAINT for child */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior, true);
			break;
		case AT_AlterColumnType:		/* ALTER COLUMN TYPE */
			ATExecAlterColumnType(tab, rel, cmd->name, (TypeName *) cmd->def);
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			ATExecChangeOwner(RelationGetRelid(rel),
							  get_roleid_checked(cmd->name),
							  false);
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
			ATExecClusterOn(rel, cmd->name);
			break;
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATExecDropCluster(rel);
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */

			/*
			 * Nothing to do here; we'll have generated a DropColumn
			 * subcommand to do the real work
			 */
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */

			/*
			 * Nothing to do here; Phase 3 does the work
			 */
			break;
		case AT_SetRelOptions:	/* SET (...) */
			ATExecSetRelOptions(rel, (List *) cmd->def, false);
			break;
		case AT_ResetRelOptions:		/* RESET (...) */
			ATExecSetRelOptions(rel, (List *) cmd->def, true);
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name, true, false);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name, false, false);
			break;
		case AT_EnableTrigAll:	/* ENABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL, true, false);
			break;
		case AT_DisableTrigAll:	/* DISABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL, false, false);
			break;
		case AT_EnableTrigUser:	/* ENABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL, true, true);
			break;
		case AT_DisableTrigUser:		/* DISABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL, false, true);
			break;
		case AT_AddInherit:
			ATExecAddInherit(rel, (Node *) cmd->def);
			break;
		case AT_DropInherit:
			ATExecDropInherit(rel, (RangeVar *) cmd->def, false);
			break;
		case AT_SetDistributedBy:	/* SET DISTRIBUTED BY */
			ATExecSetDistributedBy(rel, (Node *) cmd->def, cmd);
			break;
			/* CDB: Partitioned Table commands */
		case AT_PartAdd:				/* Add */
			ATPExecPartAdd(tab, rel, (AlterPartitionCmd *) cmd->def,
						   cmd->subtype);
            break;
		case AT_PartAlter:				/* Alter */
            ATPExecPartAlter(wqueue, tab, rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartCoalesce:			/* Coalesce */
            ATPExecPartCoalesce(rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartDrop:				/* Drop */
            ATPExecPartDrop(rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartExchange:			/* Exchange */
            ATPExecPartExchange(tab, rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartMerge:				/* Merge */
            ATPExecPartMerge(rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartModify:				/* Modify */
            ATPExecPartModify(rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartRename:				/* Rename */
            ATPExecPartRename(rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartSetTemplate:		/* Set Subpartition Template */
			ATPExecPartSetTemplate(tab, rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartSplit:				/* Split */
            ATPExecPartSplit(rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartTruncate:			/* Truncate */
			ATPExecPartTruncate(rel, (AlterPartitionCmd *) cmd->def);
            break;
		case AT_PartAddInternal:
			ATExecPartAddInternal(rel, cmd->def);
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			break;
	}

	/*
	 * Bump the command counter to ensure the next subcommand in the sequence
	 * can see the changes so far
	 */
	CommandCounterIncrement();
}

/*
 * ATRewriteTables: ALTER TABLE phase 3
 */
static void
ATRewriteTables(List **wqueue, 
				int oidInfoCount, TableOidInfo * oidInfo, List **poidmap)
{
	ListCell   *ltab;
	int			m = 0;

	/* Go through each table that needs to be checked or rewritten */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		Relation rel;
		bool relisshared;
		bool relisnailed;
		Oid  newTableSpace;
		Oid  oldTableSpace;
		Oid  relNamespace;

		/* We will lock the table iff we decide to actually rewrite it */
		rel = relation_open(tab->relid, NoLock);
		relisshared   = rel->rd_rel->relisshared;
		relisnailed   = rel->rd_isnailed;
		relNamespace  = RelationGetNamespace(rel);
		oldTableSpace = rel->rd_rel->reltablespace;
		newTableSpace = tab->newTableSpace ? tab->newTableSpace : oldTableSpace;

		/*
		 * There are two cases where we will rewrite the table, for these cases
		 * run the necessary sanity checks.
		 */
		if (tab->newvals != NIL || tab->newTableSpace)
		{
			/* 
			 * AlterTableInternal is not allowed to execute alter table
			 * subcommands that require allocating new relfilenodes since it is
			 * executed in a context that doesn't dispatch the Alter Table.
			 *
			 * This should never occur.
			 */
			if (!oidInfo)
				elog(ERROR, 
					 "internal alter table cannot allocate new relfilenodes");
			Assert(m < oidInfoCount);

			/*
			 * We can never allow rewriting of shared or nailed-in-cache
			 * relations, because we can't support changing their relfilenode
			 * values.
			 */
			if (relisshared || relisnailed)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite system relation \"%s\"",
								RelationGetRelationName(rel))));

			/*
			 * Don't allow rewrite on temp tables of other backends ... their
			 * local buffer manager is not going to cope.
			 */
			if (isOtherTempNamespace(relNamespace))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot rewrite temporary tables of other sessions")));

		}
		heap_close(rel, NoLock);

		/*
		 * We only need to rewrite the table if at least one column needs to be
		 * recomputed, or we are removing the OID column.
		 */
		if (tab->newvals != NIL || tab->new_dropoids)
		{
			/* Build a temporary relation and copy data */
			char		NewHeapName[NAMEDATALEN];
			Oid         OIDNewHeap;
			ObjectAddress object;

			/*
			 * Create the new heap, using a temporary name in the same
			 * namespace as the existing table.  NOTE: there is some risk of
			 * collision with user relnames.  Working around this seems more
			 * trouble than it's worth; in particular, we can't create the new
			 * heap in a different namespace from the old, or we will have
			 * problems with the TEMP status of temp tables.
			 */
			snprintf(NewHeapName, sizeof(NewHeapName),
					 "pg_temp_%u", tab->relid);

            List *indexIds = RelationGetIndexList(rel);
            OIDNewHeap = make_new_heap(tab->relid, NewHeapName, newTableSpace,
                                       &oidInfo[m], list_length(indexIds) > 0);
            list_free(indexIds);

			/*
			 * Copy the heap data into the new table with the desired
			 * modifications, and test the current data within the table
			 * against new constraints generated by ALTER TABLE commands.
			 */
			ATRewriteTable(tab, OIDNewHeap);

			/* Swap the physical files of the old and new heaps. */
			swap_relation_files(tab->relid, OIDNewHeap, true);

			CommandCounterIncrement();

			/* Destroy new heap with old filenode */
			object.classId = RelationRelationId;
			object.objectId = OIDNewHeap;
			object.objectSubId = 0;

			/*
			 * The new relation is local to our transaction and we know
			 * nothing depends on it, so DROP_RESTRICT should be OK.
			 */
			performDeletion(&object, DROP_RESTRICT);
			/* performDeletion does CommandCounterIncrement at end */

			/*
			 * Rebuild each index on the relation (but not the toast table,
			 * which is all-new anyway).  We do not need
			 * CommandCounterIncrement() because reindex_relation does it.
			 * reindex_relation also checks for non-NULL poidmap, so we don't
			 * need to.
			 *	
			 * It is not legal to reindex without dispatch of the associated
			 * oids!
			 */
			reindex_relation(tab->relid, false, false, false, 
							 poidmap, poidmap && Gp_role == GP_ROLE_DISPATCH);
		}
		else
		{
			/*
			 * Test the current data within the table against new constraints
			 * generated by ALTER TABLE commands, but don't rebuild data.
			 */
			if (tab->constraints != NIL || tab->new_notnull)
				ATRewriteTable(tab, InvalidOid);

			/*
			 * If we had SET TABLESPACE but no reason to reconstruct tuples,
			 * just do a block-by-block copy.
			 */
			if (tab->newTableSpace)
			{
				populate_oidInfo(&oidInfo[m], newTableSpace, relisshared, 
								 false);
				
				ATExecSetTableSpace(tab->relid, tab->newTableSpace, 
									&oidInfo[m]);
			}
		}
		m++;

	}

	/*
	 * Foreign key constraints are checked in a final pass, since (a) it's
	 * generally best to examine each one separately, and (b) it's at least
	 * theoretically possible that we have changed both relations of the
	 * foreign key, and we'd better have finished both rewrites before we try
	 * to read the tables.
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		Relation	rel = NULL;
		ListCell   *lcon;

		foreach(lcon, tab->constraints)
		{
			NewConstraint *con = lfirst(lcon);

			if (con->contype == CONSTR_FOREIGN)
			{
				FkConstraint *fkconstraint = (FkConstraint *) con->qual;
				Relation	refrel;

				if (rel == NULL)
				{
					/* Long since locked, no need for another */
					rel = heap_open(tab->relid, NoLock);
				}

				refrel = heap_open(con->refrelid, RowShareLock);

				validateForeignKeyConstraint(fkconstraint, rel, refrel);

				heap_close(refrel, NoLock);
			}
		}

		if (rel)
			heap_close(rel, NoLock);
	}
}

/*
 * ATRewriteTable: scan or rewrite one table
 *
 * OIDNewHeap is InvalidOid if we don't need to rewrite
 */
static void
ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap)
{
	Relation	oldrel;
	Relation	newrel;
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	bool		needscan = false;
	List	   *notnull_attrs;
	int			i;
	ListCell   *l;
	EState	   *estate;

	/*
	 * Open the relation(s).  We have surely already locked the existing
	 * table.
	 */
	if (OidIsValid(tab->exchange_relid))
		oldrel = heap_open(tab->exchange_relid, NoLock);
	else
		oldrel = heap_open(tab->relid, NoLock);

	oldTupDesc = tab->oldDesc;
	newTupDesc = RelationGetDescr(oldrel);		/* includes all mods */

	if (OidIsValid(OIDNewHeap))
		newrel = heap_open(OIDNewHeap, AccessExclusiveLock);
	else
		newrel = NULL;

	/*
	 * If we need to rewrite the table, the operation has to be propagated to
	 * tables that use this table's rowtype as a column type.
	 *
	 * (Eventually this will probably become true for scans as well, but at
	 * the moment a composite type does not enforce any constraints, so it's
	 * not necessary/appropriate to enforce them just during ALTER.)
	 */
	if (newrel)
		find_composite_type_dependencies(oldrel->rd_rel->reltype,
										 RelationGetRelationName(oldrel),
										 NULL);

	/*
	 * Generate the constraint and default execution states
	 */

	estate = CreateExecutorState();

	/* Build the needed expression execution states */
	foreach(l, tab->constraints)
	{
		NewConstraint *con = lfirst(l);

		switch (con->contype)
		{
			case CONSTR_CHECK:
				needscan = true;
				con->qualstate = (List *)
					ExecPrepareExpr((Expr *) con->qual, estate);
				break;
			case CONSTR_FOREIGN:
				/* Nothing to do here */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	foreach(l, tab->newvals)
	{
		NewColumnValue *ex = lfirst(l);

		needscan = true;

		ex->exprstate = ExecPrepareExpr((Expr *) ex->expr, estate);
	}

	notnull_attrs = NIL;
	if (newrel || tab->new_notnull)
	{
		/*
		 * If we are rebuilding the tuples OR if we added any new NOT NULL
		 * constraints, check all not-null constraints.  This is a bit of
		 * overkill but it minimizes risk of bugs, and heap_attisnull is a
		 * pretty cheap test anyway.
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (newTupDesc->attrs[i]->attnotnull &&
				!newTupDesc->attrs[i]->attisdropped)
				notnull_attrs = lappend_int(notnull_attrs, i);
		}
		if (notnull_attrs)
			needscan = true;
	}

	if (newrel || needscan)
	{
		ExprContext *econtext;
		Datum	   *values;
		bool	   *isnull;
		TupleTableSlot *oldslot;
		TupleTableSlot *newslot;
		HeapScanDesc heapscan = NULL;
		AppendOnlyScanDesc aoscan = NULL;
		HeapTuple	htuple = NULL;
		MemTuple	mtuple = NULL;
		MemoryContext oldCxt;
		List	   *dropped_attrs = NIL;
		ListCell   *lc;
		char		relstorage = oldrel->rd_rel->relstorage;

		econtext = GetPerTupleExprContext(estate);

		/*
		 * Make tuple slots for old and new tuples.  Note that even when the
		 * tuples are the same, the tupDescs might not be (consider ADD COLUMN
		 * without a default).
		 */
		oldslot = MakeSingleTupleTableSlot(oldTupDesc);
		newslot = MakeSingleTupleTableSlot(newTupDesc);

		/* Preallocate values/isnull arrays */
		i = Max(newTupDesc->natts, oldTupDesc->natts);
		values = (Datum *) palloc0(i * sizeof(Datum));
		isnull = (bool *) palloc(i * sizeof(bool));
		memset(isnull, true, i * sizeof(bool));

		/*
		 * Any attributes that are dropped according to the new tuple
		 * descriptor can be set to NULL. We precompute the list of dropped
		 * attributes to avoid needing to do so in the per-tuple loop.
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (newTupDesc->attrs[i]->attisdropped)
				dropped_attrs = lappend_int(dropped_attrs, i);
		}


		/*
		 * Scan through the rows, generating a new row if needed and then
		 * checking all the constraints.
		 */
		if(relstorage == RELSTORAGE_HEAP)
		{
			heapscan = heap_beginscan(oldrel, SnapshotNow, 0, NULL);

			/*
			 * Switch to per-tuple memory context and reset it for each tuple
			 * produced, so we don't leak memory.
			 */
			oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

			while ((htuple = heap_getnext(heapscan, ForwardScanDirection)) != NULL)
			{
				if (newrel)
				{
					Oid			tupOid = InvalidOid;

					/* Extract data from old tuple */
					heap_deform_tuple(htuple, oldTupDesc, values, isnull);
					if (oldTupDesc->tdhasoid)
						tupOid = HeapTupleGetOid(htuple);

					/* Set dropped attributes to null in new tuple */
					foreach(lc, dropped_attrs)
						isnull[lfirst_int(lc)] = true;

					/*
					 * Process supplied expressions to replace selected columns.
					 * Expression inputs come from the old tuple.
					 */
					ExecStoreHeapTuple(htuple, oldslot, InvalidBuffer, false);
					econtext->ecxt_scantuple = oldslot;

					foreach(l, tab->newvals)
					{
						NewColumnValue *ex = lfirst(l);

						values[ex->attnum - 1] = ExecEvalExpr(ex->exprstate,
															  econtext,
															  &isnull[ex->attnum - 1],
															  NULL);
					}

					/*
					 * Form the new tuple. Note that we don't explicitly pfree it,
					 * since the per-tuple memory context will be reset shortly.
					 */
					htuple = heap_form_tuple(newTupDesc, values, isnull);

					/* Preserve OID, if any */
					if (newTupDesc->tdhasoid)
						HeapTupleSetOid(htuple, tupOid);
				}

				/* Now check any constraints on the possibly-changed tuple */
				ExecStoreHeapTuple(htuple, newslot, InvalidBuffer, false);
				econtext->ecxt_scantuple = newslot;

				foreach(l, notnull_attrs)
				{
					int			attn = lfirst_int(l);

					if (heap_attisnull(htuple, attn + 1))
						ereport(ERROR,
								(errcode(ERRCODE_NOT_NULL_VIOLATION),
								 errmsg("column \"%s\" contains null values",
										NameStr(newTupDesc->attrs[attn]->attname)),
								 errOmitLocation(true)));
				}

				foreach(l, tab->constraints)
				{
					NewConstraint *con = lfirst(l);

					switch (con->contype)
					{
						case CONSTR_CHECK:
							if (!ExecQual(con->qualstate, econtext, true))
							{
								if (OidIsValid(tab->exchange_relid))
									ereport(ERROR,
											(errcode(ERRCODE_CHECK_VIOLATION),
											 errmsg("exchange table contains "
													"a row which violates the "
													"partitioning "
													"specification of \"%s\"",
													get_rel_name(tab->relid)),
											 errOmitLocation(true)));
								else
									ereport(ERROR,
											(errcode(ERRCODE_CHECK_VIOLATION),
											 errmsg("check constraint \"%s\" is violated by some row",
												con->name),
											 errOmitLocation(true)));
							}
							break;
						case CONSTR_FOREIGN:
							/* Nothing to do here */
							break;
						default:
							elog(ERROR, "unrecognized constraint type: %d",
								 (int) con->contype);
					}
				}

				/* Write the tuple out to the new relation */
				if (newrel)
					simple_heap_insert(newrel, htuple);

				ResetExprContext(econtext);

				CHECK_FOR_INTERRUPTS();
			}

			MemoryContextSwitchTo(oldCxt);
			heap_endscan(heapscan);

		}
		else if(relstorage == RELSTORAGE_AOROWS && Gp_role != GP_ROLE_DISPATCH)
		{
			/*
			 * When rewriting an appendonly table we choose to always insert
			 * into the segfile reserved for special purposes (segno #0).
			 */
			int 					segno = RESERVED_SEGNO;
			AppendOnlyInsertDesc 	aoInsertDesc = NULL;
			MemTupleBinding*		mt_bind;

			if(newrel)
				aoInsertDesc = appendonly_insert_init(newrel, segno);

			mt_bind = (newrel ? aoInsertDesc->mt_bind : create_memtuple_binding(newTupDesc));

			aoscan = appendonly_beginscan(oldrel, SnapshotNow, 0, NULL);

			/*
			 * Switch to per-tuple memory context and reset it for each tuple
			 * produced, so we don't leak memory.
			 */
			oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

			while ((mtuple = appendonly_getnext(aoscan, ForwardScanDirection, oldslot)) != NULL)
			{
				if (newrel)
				{
					Oid			tupOid = InvalidOid;

					TupClearShouldFree(oldslot);
					econtext->ecxt_scantuple = oldslot;

					/* Extract data from old tuple */
					for(i = 0; i < oldslot->tts_tupleDescriptor->natts ; i++)
						values[i] = slot_getattr(oldslot, i+1, &isnull[i]);

					if (oldTupDesc->tdhasoid)
						tupOid = MemTupleGetOid(mtuple, mt_bind);

					/* Set dropped attributes to null in new tuple */
					foreach(lc, dropped_attrs)
						isnull[lfirst_int(lc)] = true;

					/*
					 * Process supplied expressions to replace selected columns.
					 * Expression inputs come from the old tuple.
					 */
					foreach(l, tab->newvals)
					{
						NewColumnValue *ex = lfirst(l);

						values[ex->attnum - 1] = ExecEvalExpr(ex->exprstate,
															  econtext,
															  &isnull[ex->attnum - 1],
															  NULL);
					}

					/*
					 * Form the new tuple. Note that we don't explicitly pfree it,
					 * since the per-tuple memory context will be reset shortly.
					 */
					mtuple = memtuple_form_to(mt_bind,
											  values, isnull,
											  NULL, NULL, false);


					/* Preserve OID, if any */
					if (newTupDesc->tdhasoid)
						MemTupleSetOid(mtuple, mt_bind, tupOid);
				}

				/* Now check any constraints on the possibly-changed tuple */
				ExecStoreMemTuple(mtuple, newslot, false);
				econtext->ecxt_scantuple = newslot;

				foreach(l, notnull_attrs)
				{
					int					attn = lfirst_int(l);
					
					if (memtuple_attisnull(mtuple, mt_bind, attn + 1))
						ereport(ERROR,
								(errcode(ERRCODE_NOT_NULL_VIOLATION),
								 errmsg("column \"%s\" contains null values",
										NameStr(newTupDesc->attrs[attn]->attname)),
								 errOmitLocation(true)));
				}

				foreach(l, tab->constraints)
				{
					NewConstraint *con = lfirst(l);

					switch (con->contype)
					{
						case CONSTR_CHECK:
							if (!ExecQual(con->qualstate, econtext, true))
								ereport(ERROR,
										(errcode(ERRCODE_CHECK_VIOLATION),
										 errmsg("check constraint \"%s\" is violated by some row",
												con->name),
										 errOmitLocation(true)));
							break;
						case CONSTR_FOREIGN:
							/* Nothing to do here */
							break;
						default:
							elog(ERROR, "unrecognized constraint type: %d",
								 (int) con->contype);
					}
				}

				/* Write the tuple out to the new relation */
				if (newrel)
				{
					Oid 		tupleOid;
					AOTupleId	aoTupleId;
					
					appendonly_insert(aoInsertDesc, mtuple, &tupleOid, &aoTupleId);
				}

				ResetExprContext(econtext);

				CHECK_FOR_INTERRUPTS();

			}

			MemoryContextSwitchTo(oldCxt);
			appendonly_endscan(aoscan);

			if(newrel)
				appendonly_insert_finish(aoInsertDesc);
		}
		else if(relstorage == RELSTORAGE_AOCOLS && Gp_role != GP_ROLE_DISPATCH)
		{
			int segno = RESERVED_SEGNO;
			AOCSInsertDesc idesc = NULL;
			AOCSScanDesc sdesc = NULL;
			int nvp = oldrel->rd_att->natts;
			bool *proj = palloc0(sizeof(bool) * nvp);

			if (oldTupDesc)
			{
				Assert(oldTupDesc->natts <= nvp);
				memset(proj, true, oldTupDesc->natts);
			}
			else
				memset(proj, true, nvp);

			if(newrel)
				idesc = aocs_insert_init(newrel, segno);

			sdesc = aocs_beginscan(oldrel, SnapshotNow, tab->oldDesc, proj);

			aocs_getnext(sdesc, ForwardScanDirection, oldslot);

			while(!TupIsNull(oldslot))
			{
				oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
				econtext->ecxt_scantuple = oldslot;

				if(newrel)
				{
					Datum *slotvalues = slot_get_values(oldslot);
					bool  *slotnulls = slot_get_isnull(oldslot);
					Datum *newslotvalues = slot_get_values(newslot);
					bool *newslotnulls = slot_get_isnull(newslot);

					int nv = Min(oldslot->tts_tupleDescriptor->natts, newslot->tts_tupleDescriptor->natts);

					/* aocs does not do oid yet */
					Assert(!oldTupDesc->tdhasoid);
					Assert(!newTupDesc->tdhasoid);

					/* Dropped att should be set correctly by aocs_getnext */
					/* transfer values from old to new slot */

					memcpy(newslotvalues, slotvalues, sizeof(Datum) * nv);
					memcpy(newslotnulls, slotnulls, sizeof(bool) * nv);
					TupSetVirtualTupleNValid(newslot, nv);

					/* Process supplied expressions */
					foreach(l, tab->newvals)
					{
						NewColumnValue *ex = lfirst(l);

						newslotvalues[ex->attnum-1] =
							ExecEvalExpr(ex->exprstate,
										 econtext,
										 &newslotnulls[ex->attnum-1],
										 NULL
								);

						if (ex->attnum > nv)
							TupSetVirtualTupleNValid(newslot, ex->attnum);
					}

					/* Use the modified tuple to check constraints below */
					econtext->ecxt_scantuple = newslot;
				}

				/* Check constraints */
				foreach (l, tab->constraints)
				{
					NewConstraint *con = lfirst(l);
					switch(con->contype)
					{
						case CONSTR_CHECK:
							if(!ExecQual(con->qualstate, econtext, true))
								ereport(ERROR,
										(errcode(ERRCODE_CHECK_VIOLATION),
										 errmsg("check constraint \"%s\" is violated",
												con->name
											 ),
										 errOmitLocation(true)));
							break;
						case CONSTR_FOREIGN:
							/* Nothing to do */
							break;
						default:
							elog(ERROR, "Unrecognized constraint type: %d",
								 (int) con->contype);
					}
				}

				if(newrel)
				{
					MemoryContextSwitchTo(oldCxt);
					aocs_insert(idesc, newslot);
				}

				ResetExprContext(econtext);
				CHECK_FOR_INTERRUPTS();

				MemoryContextSwitchTo(oldCxt);
				aocs_getnext(sdesc, ForwardScanDirection, oldslot);
			}

			aocs_endscan(sdesc);

			if(newrel)
				aocs_insert_finish(idesc);

			pfree(proj);
		}
		else if(relstorage_is_ao(relstorage) && Gp_role == GP_ROLE_DISPATCH)
		{
			/*
			 * All we want to do on the QD during an AO table rewrite
			 * is to drop the shared memory hash table entry for this
			 * table if it exists. We must do so since before the
			 * rewrite we probably have few non-zero segfile entries
			 * for this table while after the rewrite only segno zero
			 * will be full and the others will be empty. By dropping
			 * the hash entry we force refreshing the entry from the
			 * catalog the next time a write into this AO table comes
			 * along.
			 *
			 * Note that ALTER already took an exclusive lock on the
			 * old relation so we are guaranteed to not drop the hash
			 * entry from under any concurrent operation.
			 */
			LWLockAcquire(AOSegFileLock, LW_EXCLUSIVE);
			AORelRemoveHashEntry(RelationGetRelid(oldrel));
			LWLockRelease(AOSegFileLock);
		}
		else
		{
			Assert(!"Invalid relstorage type");
		}

		ExecDropSingleTupleTableSlot(oldslot);
		ExecDropSingleTupleTableSlot(newslot);
	}

	FreeExecutorState(estate);

	heap_close(oldrel, NoLock);
	if (newrel)
		heap_close(newrel, NoLock);
}

/*
 * ATGetQueueEntry: find or create an entry in the ALTER TABLE work queue
 */
static AlteredTableInfo *
ATGetQueueEntry(List **wqueue, Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	AlteredTableInfo *tab;
	ListCell   *ltab;

	foreach(ltab, *wqueue)
	{
		tab = (AlteredTableInfo *) lfirst(ltab);
		if (tab->relid == relid)
			return tab;
	}

	/*
	 * Not there, so add it.  Note that we make a copy of the relation's
	 * existing descriptor before anything interesting can happen to it.
	 */
	tab = (AlteredTableInfo *) palloc0(sizeof(AlteredTableInfo));
	tab->relid = relid;
	tab->relkind = rel->rd_rel->relkind;
	tab->oldDesc = CreateTupleDescCopy(RelationGetDescr(rel));

	*wqueue = lappend(*wqueue, tab);

	return tab;
}

/*
 * Issue an error, if this is a part of a partitioned table or, in case 
 * rejectroot is true, if this is a partitioned table itself.
 *
 * Usually called from ATPrepCmd to validate a subcommand of an ALTER
 * TABLE command.  Many commands may be invoked on an entire partitioned
 * table, but not on just a part.  These specify rejectroot as false.
 */
static void
ATPartitionCheck(AlterTableType subtype, Relation rel, bool rejectroot, bool recursing)
{
	if (recursing)
		return;

	if (rejectroot &&
		(rel_is_child_partition(RelationGetRelid(rel)) ||
		 rel_is_partitioned(RelationGetRelid(rel))))
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("can't %s \"%s\"; it is a partitioned table or part thereof",
						alterTableCmdString(subtype),
						RelationGetRelationName(rel)),
				 errOmitLocation(true)));
	}
	else if (rel_is_child_partition(RelationGetRelid(rel)))
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("can't %s \"%s\"; it is part of a partitioned table",
						alterTableCmdString(subtype),
						RelationGetRelationName(rel)),
				 errhint("You may be able to perform the operation on the partitioned table as a whole."),
				 errOmitLocation(true)));
	}
}

/*
 * ATSimplePermissions
 *
 * - Ensure that it is a relation (or possibly a view)
 * - Ensure this user is the owner
 * - Ensure that it is not a system table
 */
static void
ATSimplePermissions(Relation rel, bool allowView)
{
	if (rel->rd_rel->relkind != RELKIND_RELATION)
	{
		if (allowView)
		{
			if (rel->rd_rel->relkind != RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a table or view",
								RelationGetRelationName(rel)),
						 errOmitLocation(true)));
		}
		else if (!IsUnderPostmaster &&
				 (rel->rd_rel->relkind == RELKIND_AOSEGMENTS ||
				  rel->rd_rel->relkind == RELKIND_AOBLOCKDIR))
		{
			/*
			 * Allow ALTER TABLE operations in standard alone mode on
			 * AO segment tables.
			 */
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table",
							RelationGetRelationName(rel)),
					 errOmitLocation(true)));
	}

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));
}

/*
 * ATSimplePermissionsRelationOrIndex
 *
 * - Ensure that it is a relation or an index
 * - Ensure this user is the owner
 * - Ensure that it is not a system table
 */
static void
ATSimplePermissionsRelationOrIndex(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or index",
						RelationGetRelationName(rel)),
				 errOmitLocation(true)));

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));
}

/*
 * ATSimpleRecursion
 *
 * Simple table recursion sufficient for most ALTER TABLE operations.
 * All direct and indirect children are processed in an unspecified order.
 * Note that if a child inherits from the original table via multiple
 * inheritance paths, it will be visited just once.
 */
static void
ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse)
{
	/*
	 * Propagate to children if desired.  Non-table relations never have
	 * children, so no need to search in that case.
	 */
	if (recurse && rel->rd_rel->relkind == RELKIND_RELATION)
	{
		Oid			relid = RelationGetRelid(rel);
		ListCell   *child;
		List	   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(relid);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;

			if (childrelid == relid)
				continue;
			childrel = relation_open(childrelid, AccessExclusiveLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");
			ATPrepCmd(wqueue, childrel, cmd, false, true);
			relation_close(childrel, NoLock);
		}
	}
}

/*
 * ATOneLevelRecursion
 *
 * Here, we visit only direct inheritance children.  It is expected that
 * the command's prep routine will recurse again to find indirect children.
 * When using this technique, a multiply-inheriting child will be visited
 * multiple times.
 */
static void
ATOneLevelRecursion(List **wqueue, Relation rel,
					AlterTableCmd *cmd)
{
	Oid			relid = RelationGetRelid(rel);
	ListCell   *child;
	List	   *children;

	/* this routine is actually in the planner */
	children = find_inheritance_children(relid);

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;

		childrel = relation_open(childrelid, AccessExclusiveLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");
		ATPrepCmd(wqueue, childrel, cmd, true, true);
		relation_close(childrel, NoLock);
	}
}


/*
 * find_composite_type_dependencies
 *
 * Check to see if a composite type is being used as a column in some
 * other table (possibly nested several levels deep in composite types!).
 * Eventually, we'd like to propagate the check or rewrite operation
 * into other such tables, but for now, just error out if we find any.
 *
 * Caller should provide either a table name or a type name (not both) to
 * report in the error message, if any.
 *
 * We assume that functions and views depending on the type are not reasons
 * to reject the ALTER.  (How safe is this really?)
 */
void
find_composite_type_dependencies(Oid typeOid,
								 const char *origTblName,
								 const char *origTypeName)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	/*
	 * We scan pg_depend to find those things that depend on the rowtype. (We
	 * assume we can ignore refobjsubid for a rowtype.)
	 */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 SnapshotNow, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		Relation	rel;
		Form_pg_attribute att;

		/* Ignore dependees that aren't user columns of relations */
		/* (we assume system columns are never of rowtypes) */
		if (pg_depend->classid != RelationRelationId ||
			pg_depend->objsubid <= 0)
			continue;

		rel = relation_open(pg_depend->objid, AccessShareLock);
		att = rel->rd_att->attrs[pg_depend->objsubid - 1];

		if (rel->rd_rel->relkind == RELKIND_RELATION)
		{
			if (origTblName)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter table \"%s\" because column \"%s\".\"%s\" uses its rowtype",
								origTblName,
								RelationGetRelationName(rel),
								NameStr(att->attname)),
										   errOmitLocation(true)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s\".\"%s\" uses it",
								origTypeName,
								RelationGetRelationName(rel),
								NameStr(att->attname)),
										   errOmitLocation(true)));
		}
		else if (OidIsValid(rel->rd_rel->reltype))
		{
			/*
			 * A view or composite type itself isn't a problem, but we must
			 * recursively check for indirect dependencies via its rowtype.
			 */
			find_composite_type_dependencies(rel->rd_rel->reltype,
											 origTblName, origTypeName);
		}

		relation_close(rel, AccessShareLock);
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);
}


/*
 * ALTER TABLE ADD COLUMN
 *
 * Adds an additional attribute to a relation making the assumption that
 * CHECK, NOT NULL, and FOREIGN KEY constraints will be removed from the
 * AT_AddColumn AlterTableCmd by analyze.c and added as independent
 * AlterTableCmd's.
 */
static void
ATPrepAddColumn(List **wqueue, Relation rel, bool recurse,
				AlterTableCmd *cmd)
{
	/*
	 * Recurse to add the column to child classes, if requested.
	 *
	 * We must recurse one level at a time, so that multiply-inheriting
	 * children are visited the right number of times and end up with the
	 * right attinhcount.
	 */
	if (recurse)
	{
		AlterTableCmd *childCmd = copyObject(cmd);
		ColumnDef  *colDefChild = (ColumnDef *) childCmd->def;

		/* Child should see column as singly inherited */
		colDefChild->inhcount = 1;
		colDefChild->is_local = false;

		ATOneLevelRecursion(wqueue, rel, childCmd);
	}
	else
	{
		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables; else the addition would put them out of step.
		 */
		if (find_inheritance_children(RelationGetRelid(rel)) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column must be added to child tables too")));
	}
}

void
ATAddColumn(Relation rel, ColumnDef *colDef)
{
	ATExecAddColumn((AlteredTableInfo*)NULL, rel, colDef);
}

static void
ATExecAddColumn(AlteredTableInfo *tab, Relation rel,
				ColumnDef *colDef)
{
	Oid			myrelid = RelationGetRelid(rel);
	Relation	pgclass,
				attrdesc;
	HeapTuple	reltup;
	HeapTuple	attributeTuple;
	Form_pg_attribute attribute;
	FormData_pg_attribute attributeD;
	int			i;
	int			minattnum,
				maxatts;
	HeapTuple	typeTuple;
	Oid			typeOid;
	Form_pg_type tform;
	Expr	   *defval;

	/*
	 * Append Only tables don't support ADD COLUMN when no default value is
	 * specified. This is because this scenario makes a schema change that
	 * memtuples can't deal with yet. If a default value is specified than it
	 * it ok because the whole table will get re-written in phase 3. Let's
	 * catch this now before doing any real work.
	 */
	if ((RelationIsAoRows(rel) || RelationIsAoCols(rel)) && !colDef->raw_default)
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_FEATURE_NOT_YET),
				 errmsg("ADD COLUMN with no default value in append-only tables"
						" is not yet supported."),
								   errOmitLocation(true)
                ));
	}

	attrdesc = heap_open(AttributeRelationId, RowExclusiveLock);

	/*
	 * Are we adding the column to a recursion child?  If so, check whether to
	 * merge with an existing definition for the column.
	 */
	if (colDef->inhcount > 0)
	{
		HeapTuple	tuple;

		/* Does child already have a column by this name? */
		tuple = SearchSysCacheCopyAttName(myrelid, colDef->colname);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			/* Okay if child matches by type */
			if (typenameTypeId(NULL, colDef->typname) != childatt->atttypid ||
				colDef->typname->typmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
							RelationGetRelationName(rel), colDef->colname),
									   errOmitLocation(true)));

			/* Bump the existing child att's inhcount */
			childatt->attinhcount++;
			simple_heap_update(attrdesc, &tuple->t_self, tuple);
			CatalogUpdateIndexes(attrdesc, tuple);

			heap_freetuple(tuple);

			/* Inform the user about the merge */
			if (Gp_role == GP_ROLE_EXECUTE)
			{
				ereport(DEBUG1,
			  (errmsg("merging definition of column \"%s\" for child \"%s\"",
					  colDef->colname, RelationGetRelationName(rel)),
							   errOmitLocation(true)));

			}
			else
			ereport(NOTICE,
			  (errmsg("merging definition of column \"%s\" for child \"%s\"",
					  colDef->colname, RelationGetRelationName(rel)),
							   errOmitLocation(true)));

			heap_close(attrdesc, RowExclusiveLock);
			return;
		}
	}

	pgclass = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(myrelid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", myrelid);

	/*
	 * this test is deliberately not attisdropped-aware, since if one tries to
	 * add a column matching a dropped column name, it's gonna fail anyway.
	 *
	 * For GPDB upgrade, we know that some columns don't exist, so don't go to
	 * the waste of looking them up. This could be a major cost if we're adding
	 * 20,000 columns for AO, for example.
	 */
	if (!(!IsUnderPostmaster && (rel->rd_rel->relkind == RELKIND_AOSEGMENTS ||
								 rel->rd_rel->relkind == RELKIND_AOBLOCKDIR)))
	{
		if (SearchSysCacheExists(ATTNAME,
								 ObjectIdGetDatum(myrelid),
								 PointerGetDatum(colDef->colname),
								 0, 0))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" already exists",
							colDef->colname, RelationGetRelationName(rel)),
					 errOmitLocation(true)));
	}
	minattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
	maxatts = minattnum + 1;
	if (maxatts > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));
	i = minattnum + 1;

	typeTuple = typenameType(NULL, colDef->typname);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	typeOid = HeapTupleGetOid(typeTuple);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colDef->colname, typeOid);

	attributeTuple = heap_addheader(Natts_pg_attribute,
									false,
									ATTRIBUTE_TUPLE_SIZE,
									(void *) &attributeD);

	attribute = (Form_pg_attribute) GETSTRUCT(attributeTuple);

	attribute->attrelid = myrelid;
	namestrcpy(&(attribute->attname), colDef->colname);
	attribute->atttypid = typeOid;
	attribute->attstattarget = -1;
	attribute->attlen = tform->typlen;
	attribute->attcacheoff = -1;
	attribute->atttypmod = colDef->typname->typmod;
	attribute->attnum = i;
	attribute->attbyval = tform->typbyval;
	attribute->attndims = list_length(colDef->typname->arrayBounds);
	attribute->attstorage = tform->typstorage;
	attribute->attalign = tform->typalign;
	attribute->attnotnull = colDef->is_not_null;
	attribute->atthasdef = false;
	attribute->attisdropped = false;
	attribute->attislocal = colDef->is_local;
	attribute->attinhcount = colDef->inhcount;

	ReleaseSysCache(typeTuple);

	simple_heap_insert(attrdesc, attributeTuple);

	/* Update indexes on pg_attribute */
	CatalogUpdateIndexes(attrdesc, attributeTuple);

	heap_close(attrdesc, RowExclusiveLock);

	/*
	 * Update number of attributes in pg_class tuple
	 */
	((Form_pg_class) GETSTRUCT(reltup))->relnatts = maxatts;

	simple_heap_update(pgclass, &reltup->t_self, reltup);

	/* keep catalog indexes current */
	CatalogUpdateIndexes(pgclass, reltup);

	heap_freetuple(reltup);

	heap_close(pgclass, RowExclusiveLock);

	/* Make the attribute's catalog entry visible */
	CommandCounterIncrement();

	/*
	 * Store the DEFAULT, if any, in the catalogs
	 */
	if (colDef->raw_default)
	{
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attribute->attnum;
		rawEnt->raw_default = copyObject(colDef->raw_default);

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationRawConstraints(rel, list_make1(rawEnt), NIL);

		/* Make the additional catalog changes visible */
		CommandCounterIncrement();
	}

	/*
	 * Tell Phase 3 to fill in the default expression, if there is one.
	 *
	 * If there is no default, Phase 3 doesn't have to do anything, because
	 * that effectively means that the default is NULL.  The heap tuple access
	 * routines always check for attnum > # of attributes in tuple, and return
	 * NULL if so, so without any modification of the tuple data we will get
	 * the effect of NULL values in the new column.
	 *
	 * An exception occurs when the new column is of a domain type: the domain
	 * might have a NOT NULL constraint, or a check constraint that indirectly
	 * rejects nulls.  If there are any domain constraints then we construct
	 * an explicit NULL default value that will be passed through
	 * CoerceToDomain processing.  (This is a tad inefficient, since it causes
	 * rewriting the table which we really don't have to do, but the present
	 * design of domain processing doesn't offer any simple way of checking
	 * the constraints more directly.)
	 *
	 * Note: we use build_column_default, and not just the cooked default
	 * returned by AddRelationRawConstraints, so that the right thing happens
	 * when a datatype's default applies.
	 */
	defval = (Expr *) build_column_default(rel, attribute->attnum);

	if (!defval && GetDomainConstraints(typeOid) != NIL)
	{
		Oid			basetype = getBaseType(typeOid);

		defval = (Expr *) makeNullConst(basetype, -1);
		defval = (Expr *) coerce_to_target_type(NULL,
												(Node *) defval,
												basetype,
												typeOid,
												colDef->typname->typmod,
												COERCION_ASSIGNMENT,
												COERCE_IMPLICIT_CAST,
												-1);
		if (defval == NULL)		/* should not happen */
			elog(ERROR, "failed to coerce base type to domain");
	}

	if (defval)
	{
		NewColumnValue *newval;

		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attribute->attnum;
		newval->expr = defval;

		/* tab is null if this is called by "create or replace view"
		 * which can't have any default value.
		 */
		Assert(tab);
		tab->newvals = lappend(tab->newvals, newval);
	}

	/*
	 * If the new column is NOT NULL, tell Phase 3 it needs to test that.
	 * Also, "create or replace view" won't have constraint on the column.
	 */
	Assert(!colDef->is_not_null || tab);
	if (tab)
		tab->new_notnull |= colDef->is_not_null;

	/*
	 * Add needed dependency entries for the new column.
	 */
	add_column_datatype_dependency(myrelid, i, attribute->atttypid);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ADD COLUMN"
				);

}

/*
 * Install a column's dependency on its datatype.
 */
static void
add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid)
{
	ObjectAddress myself,
				referenced;

	myself.classId = RelationRelationId;
	myself.objectId = relid;
	myself.objectSubId = attnum;
	referenced.classId = TypeRelationId;
	referenced.objectId = typid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 */
static void
ATExecDropNotNull(Relation rel, const char *colName)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * lookup the attribute
	 */
	attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel)),
				 errOmitLocation(true)));

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Check that the attribute is not in a primary key
	 */

	/* Loop over all indexes on the relation */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;
		int			i;

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/* If the index is not a primary key, skip the check */
		if (indexStruct->indisprimary)
		{
			/*
			 * Loop over each attribute in the primary key and see if it
			 * matches the to-be-altered attribute
			 */
			for (i = 0; i < indexStruct->indnatts; i++)
			{
				if (indexStruct->indkey.values[i] == attnum)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("column \"%s\" is in a primary key",
									colName),
							 errOmitLocation(true)));
			}
		}

		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/*
	 * Okay, actually perform the catalog change ... if needed
	 */
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
	{
		((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = FALSE;

		simple_heap_update(attr_rel, &tuple->t_self, tuple);

		/* keep the system catalog indexes current */
		CatalogUpdateIndexes(attr_rel, tuple);
	}

	heap_close(attr_rel, RowExclusiveLock);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ALTER COLUMN DROP NOT NULL"
				);

}

/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 */
static void
ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;

	/*
	 * lookup the attribute
	 */
	attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel)),
			     errOmitLocation(true)));

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Okay, actually perform the catalog change ... if needed
	 */
	if (!((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
	{
		((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = TRUE;

		simple_heap_update(attr_rel, &tuple->t_self, tuple);

		/* keep the system catalog indexes current */
		CatalogUpdateIndexes(attr_rel, tuple);

		/* Tell Phase 3 it needs to test the constraint */
		tab->new_notnull = true;
	}

	heap_close(attr_rel, RowExclusiveLock);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ALTER COLUMN SET NOT NULL"
				);

}

/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 */
static void
ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault)
{
	AttrNumber	attnum;

	/*
	 * get the number of the attribute
	 */
	attnum = get_attnum(RelationGetRelid(rel), colName);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel)),
				 errOmitLocation(true)));

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Remove any old default for the column.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false);

	if (newDefault)
	{
		/* SET DEFAULT */
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationRawConstraints(rel, list_make1(rawEnt), NIL);
	}

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ALTER COLUMN DEFAULT"
				);


}

/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 */
static void
ATPrepSetStatistics(Relation rel, const char *colName, Node *flagValue)
{
	/*
	 * We do our own permission checking because (a) we want to allow SET
	 * STATISTICS on indexes (for expressional index columns), and (b) we want
	 * to allow SET STATISTICS on system catalogs without requiring
	 * allowSystemTableMods to be turned on.
	 */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or index",
						RelationGetRelationName(rel)),
				 errOmitLocation(true)));

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));
}

static void
ATExecSetStatistics(Relation rel, const char *colName, Node *newValue)
{
	int			newtarget;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;

	Assert(IsA(newValue, Integer));
	newtarget = intVal(newValue);

	/*
	 * Limit target to a sane range
	 */
	if (newtarget < -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("statistics target %d is too low",
						newtarget)));
	}
	else if (newtarget > 1000)
	{
		newtarget = 1000;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lowering statistics target to %d",
						newtarget)));
	}

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	if (attrtuple->attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	attrtuple->attstattarget = newtarget;

	simple_heap_update(attrelation, &tuple->t_self, tuple);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, tuple);

	heap_freetuple(tuple);

	heap_close(attrelation, RowExclusiveLock);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ALTER COLUMN SET STATISTICS"
				);


}

/*
 * ALTER TABLE ALTER COLUMN SET STORAGE
 */
static void
ATExecSetStorage(Relation rel, const char *colName, Node *newValue)
{
	char	   *storagemode;
	char		newstorage;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;

	Assert(IsA(newValue, String));
	storagemode = strVal(newValue);

	if (pg_strcasecmp(storagemode, "plain") == 0)
		newstorage = 'p';
	else if (pg_strcasecmp(storagemode, "external") == 0)
		newstorage = 'e';
	else if (pg_strcasecmp(storagemode, "extended") == 0)
		newstorage = 'x';
	else if (pg_strcasecmp(storagemode, "main") == 0)
		newstorage = 'm';
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid storage type \"%s\"",
						storagemode)));
		newstorage = 0;			/* keep compiler quiet */
	}

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel)),
				 errOmitLocation(true)));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	if (attrtuple->attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * safety check: do not allow toasted storage modes unless column datatype
	 * is TOAST-aware.
	 */
	if (newstorage == 'p' || TypeIsToastable(attrtuple->atttypid))
		attrtuple->attstorage = newstorage;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s can only have storage PLAIN",
						format_type_be(attrtuple->atttypid))));

	simple_heap_update(attrelation, &tuple->t_self, tuple);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, tuple);

	heap_freetuple(tuple);

	heap_close(attrelation, RowExclusiveLock);
	
	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ALTER COLUMN SET STORAGE"
				);
	
}


/*
 * ALTER TABLE DROP COLUMN
 *
 * DROP COLUMN cannot use the normal ALTER TABLE recursion mechanism,
 * because we have to decide at runtime whether to recurse or not depending
 * on whether attinhcount goes to zero or not.	(We can't check this in a
 * static pre-pass because it won't handle multiple inheritance situations
 * correctly.)
 */
static void
ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing)
{
	HeapTuple	tuple;
	Form_pg_attribute targetatt;
	AttrNumber	attnum;
	List	   *children;
	ObjectAddress object;
	GpPolicy  *policy;
	PartitionNode *pn;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, false);

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	targetatt = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = targetatt->attnum;

	/* Can't drop a system attribute, except OID */
	if (attnum <= 0 && attnum != ObjectIdAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop system column \"%s\"",
						colName),
								   errOmitLocation(true)));

	/* Don't drop inherited columns */
	if (targetatt->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop inherited column \"%s\"",
						colName),
								   errOmitLocation(true)));

	/* better not be a column we partition on */
	pn = RelationBuildPartitionDesc(rel, false);
	if (pn)
	{
		List *patts = get_partition_attrs(pn);

		if (list_member_int(patts, attnum))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot drop partitioning column \"%s\"",
							colName),
									   errOmitLocation(true)));
	}

	ReleaseSysCache(tuple);

	policy = rel->rd_cdbpolicy;
	if (policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED)
	{
		int			ia = 0;

		for (ia = 0; ia < policy->nattrs; ia++)
		{
			if (attnum == policy->attrs[ia])
			{
				policy->nattrs = 0;
				rel->rd_cdbpolicy = GpPolicyCopy(GetMemoryChunkContext(rel), policy);
				GpPolicyReplace(rel->rd_id, policy);
				if (Gp_role != GP_ROLE_EXECUTE)
				    ereport(NOTICE,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Dropping a column that is part of the "
								"distribution policy forces a "
								"NULL distribution policy"),
										   errOmitLocation(true)));
			}
		}
	}

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	children = find_inheritance_children(RelationGetRelid(rel));

	if (children)
	{
		Relation	attr_rel;
		ListCell   *child;

		attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;
			Form_pg_attribute childatt;

			childrel = heap_open(childrelid, AccessExclusiveLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");

			tuple = SearchSysCacheCopyAttName(childrelid, colName);
			if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
				elog(ERROR, "cache lookup failed for attribute \"%s\" of relation %u",
					 colName, childrelid);
			childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (childatt->attinhcount <= 0)		/* shouldn't happen */
				elog(ERROR, "relation %u has non-inherited attribute \"%s\"",
					 childrelid, colName);

			if (recurse)
			{
				/*
				 * If the child column has other definition sources,
				 * just decrement its inheritance count;
				 * if not or if this is part of a partition
				 * configuration, recurse to delete it.
				 */
				if ((childatt->attinhcount == 1 && !childatt->attislocal) ||
					pn)
				{
					/* Time to delete this child column, too */
					ATExecDropColumn(wqueue, childrel, colName, behavior, true, true);
				}
				else
				{
					/* Child column must survive my deletion */
					childatt->attinhcount--;

					simple_heap_update(attr_rel, &tuple->t_self, tuple);

					/* keep the system catalog indexes current */
					CatalogUpdateIndexes(attr_rel, tuple);

					/* Make update visible */
					CommandCounterIncrement();
				}
			}
			else
			{
				/*
				 * If we were told to drop ONLY in this table (no recursion),
				 * we need to mark the inheritors' attribute as locally
				 * defined rather than inherited.
				 */
				childatt->attinhcount--;
				childatt->attislocal = true;

				simple_heap_update(attr_rel, &tuple->t_self, tuple);

				/* keep the system catalog indexes current */
				CatalogUpdateIndexes(attr_rel, tuple);

				/* Make update visible */
				CommandCounterIncrement();
			}

			heap_freetuple(tuple);

			heap_close(childrel, NoLock);
		}
		heap_close(attr_rel, RowExclusiveLock);
	}

	/*
	 * Perform the actual column deletion
	 */
	object.classId = RelationRelationId;
	object.objectId = RelationGetRelid(rel);
	object.objectSubId = attnum;

	performDeletion(&object, behavior);

	/*
	 * If we dropped the OID column, must adjust pg_class.relhasoids and
	 * tell Phase 3 to physically get rid of the column.
	 */
	if (attnum == ObjectIdAttributeNumber)
	{
		Relation	class_rel;
		Form_pg_class tuple_class;
		AlteredTableInfo *tab;

		class_rel = heap_open(RelationRelationId, RowExclusiveLock);

		tuple = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(RelationGetRelid(rel)),
								   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u",
				 RelationGetRelid(rel));
		tuple_class = (Form_pg_class) GETSTRUCT(tuple);

		tuple_class->relhasoids = false;
		simple_heap_update(class_rel, &tuple->t_self, tuple);

		/* Keep the catalog indexes up to date */
		CatalogUpdateIndexes(class_rel, tuple);

		heap_close(class_rel, RowExclusiveLock);
		
		/* Find or create work queue entry for this table */
		tab = ATGetQueueEntry(wqueue, rel);
		
		/* Tell Phase 3 to physically remove the OID column */
		tab->new_dropoids = true;
	}

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "DROP COLUMN"
				);

}

/*
 * ALTER TABLE ADD INDEX
 *
 * There is no such command in the grammar, but the parser converts UNIQUE
 * and PRIMARY KEY constraints into AT_AddIndex subcommands.  This lets us
 * schedule creation of the index at the appropriate time during ALTER.
 */
static void
ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild, bool part_expanded)
{
	bool		check_rights;
	bool		skip_build;
	bool		quiet;

	Assert(IsA(stmt, IndexStmt));

	/* The index should already be built if we are a QE */
	if (Gp_role == GP_ROLE_EXECUTE)
		return;

	/* suppress schema rights check when rebuilding existing index */
	check_rights = !is_rebuild;
	/* skip index build if phase 3 will have to rewrite table anyway */
	skip_build = (tab->newvals != NIL);
	/* suppress notices when rebuilding existing index */
	quiet = is_rebuild;

	DefineIndex(stmt->relation, /* relation */
				stmt->idxname,	/* index name */
				InvalidOid,		/* no predefined OID */
				stmt->accessMethod,		/* am name */
				stmt->tableSpace,
				stmt->indexParams,		/* parameters */
				(Expr *) stmt->whereClause,
				stmt->rangetable,
				stmt->options,
				stmt->unique,
				stmt->primary,
				stmt->isconstraint,
				true,			/* is_alter_table */
				check_rights,
				skip_build,
				quiet,
				false,
				part_expanded,
				stmt);

	/* 
	 * If we're the master and this is a partitioned table, cascade index
	 * creation for all children.
	 */
	if (Gp_role == GP_ROLE_DISPATCH &&
		rel_is_partitioned(RelationGetRelid(rel)))
	{
		List *children = find_all_inheritors(RelationGetRelid(rel));
		ListCell *lc;
		bool prefix_match = false;
		char *pname = RelationGetRelationName(rel);
		char *iname = stmt->idxname ? stmt->idxname : "idx";
		DestReceiver *dest = None_Receiver;

		/* is the parent relation name a prefix of the index name? */
		if (strlen(iname) > strlen(pname) &&
			strncmp(pname, iname, strlen(pname)) == 0)
			prefix_match = true;

		foreach(lc, children)
		{
			Oid relid = lfirst_oid(lc);
			Relation crel;
			IndexStmt *istmt;
			AlterTableStmt *ats;
			AlterTableCmd *atc;
			RangeVar *rv;
			char *idxname;

			if (relid == RelationGetRelid(rel))
				continue;

 			crel = heap_open(relid, AccessShareLock);
			istmt = copyObject(stmt);
			istmt->idxOids = NIL;

			ats = makeNode(AlterTableStmt);
			atc = makeNode(AlterTableCmd);

			rv = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
							  get_rel_name(relid), -1);
			istmt->relation = rv;
			if (prefix_match)
			{
				/* 
				 * If the next character in the index name is '_', absorb
				 * it, as ChooseRelationName() will add another.
				 */
				int off = 0;
				if (iname[strlen(pname)] == '_')
					off = 1;
				idxname = ChooseRelationName(RelationGetRelationName(crel),
											 NULL,
											 (iname + strlen(pname) + off),
											 RelationGetNamespace(crel),
											 NULL);
			}
			else
				idxname = ChooseRelationName(RelationGetRelationName(crel),
											 NULL,
											 iname,
											 RelationGetNamespace(crel),
											 NULL);
			istmt->idxname = idxname;
			atc->subtype = AT_AddIndex;
			atc->def = (Node *)istmt;
			atc->part_expanded = true;

			ats->relation = copyObject(istmt->relation);
			ats->cmds = list_make1(atc);
			ats->relkind = OBJECT_TABLE;

			heap_close(crel, AccessShareLock); /* already locked master */

			ProcessUtility((Node *)ats, 
						   synthetic_sql,
						   NULL, 
						   false, /* not top level */
						   dest, 
						   NULL);
		}
	}

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ADD INDEX"
				);

}

/*
 * ALTER TABLE ADD CONSTRAINT
 */
static void
ATExecAddConstraint(AlteredTableInfo *tab, Relation rel, Node *newConstraint)
{
	switch (nodeTag(newConstraint))
	{
		case T_Constraint:
			{
				Constraint *constr = (Constraint *) newConstraint;

				/*
				 * Currently, we only expect to see CONSTR_CHECK nodes
				 * arriving here (see the preprocessing done in
				 * parser/analyze.c).  Use a switch anyway to make it easier
				 * to add more code later.
				 */
				switch (constr->contype)
				{
					case CONSTR_CHECK:
						{
							List	   *newcons;
							ListCell   *lcon;

							/*
							 * Call AddRelationRawConstraints to do the work.
							 * It returns a list of cooked constraints.
							 */
							newcons = AddRelationRawConstraints(rel, NIL,
														 list_make1(constr));
							/* Add each constraint to Phase 3's queue */
							foreach(lcon, newcons)
							{
								CookedConstraint *ccon = (CookedConstraint *) lfirst(lcon);
								NewConstraint *newcon;

								newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
								newcon->name = ccon->name;
								newcon->contype = ccon->contype;
								/* ExecQual wants implicit-AND format */
								newcon->qual = (Node *)
									make_ands_implicit((Expr *) ccon->expr);

								tab->constraints = lappend(tab->constraints,
														   newcon);
							}
							break;
						}
					default:
						elog(ERROR, "unrecognized constraint type: %d",
							 (int) constr->contype);
				}
				break;
			}
		case T_FkConstraint:
			{
				FkConstraint *fkconstraint = (FkConstraint *) newConstraint;

				/*
				 * Assign or validate constraint name
				 */
				if (fkconstraint->constr_name)
				{
					if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
											 RelationGetRelid(rel),
											 RelationGetNamespace(rel),
											 fkconstraint->constr_name))
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("constraint \"%s\" for relation \"%s\" already exists",
										fkconstraint->constr_name,
										RelationGetRelationName(rel)),
												   errOmitLocation(true)));
				}
				else
					fkconstraint->constr_name =
						ChooseConstraintName(RelationGetRelationName(rel),
									strVal(linitial(fkconstraint->fk_attrs)),
											 "fkey",
											 RelationGetNamespace(rel),
											 NIL);

				ATAddForeignKeyConstraint(tab, rel, fkconstraint);

				break;
			}
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(newConstraint));
	}

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ADD CONSTRAINT"
				);

}

/*
 * Add a foreign-key constraint to a single table
 *
 * Subroutine for ATExecAddConstraint.	Must already hold exclusive
 * lock on the rel, and have done appropriate validity/permissions checks
 * for it.
 */
static void
ATAddForeignKeyConstraint(AlteredTableInfo *tab, Relation rel,
						  FkConstraint *fkconstraint)
{
	Relation	pkrel;
	AclResult	aclresult;
	int16		pkattnum[INDEX_MAX_KEYS];
	int16		fkattnum[INDEX_MAX_KEYS];
	Oid			pktypoid[INDEX_MAX_KEYS];
	Oid			fktypoid[INDEX_MAX_KEYS];
	Oid			opclasses[INDEX_MAX_KEYS];
	int			i;
	int			numfks,
				numpks;
	Oid			indexOid;
	Oid			constrOid;

	/*
	 * Grab an exclusive lock on the pk table, so that someone doesn't delete
	 * rows out from under us. (Although a lesser lock would do for that
	 * purpose, we'll need exclusive lock anyway to add triggers to the pk
	 * table; trying to start with a lesser lock will just create a risk of
	 * deadlock.)
	 */
	pkrel = heap_openrv(fkconstraint->pktable, AccessExclusiveLock);

	/*
	 * Validity and permissions checks
	 *
	 * Note: REFERENCES permissions checks are redundant with CREATE TRIGGER,
	 * but we may as well error out sooner instead of later.
	 */
	if (pkrel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel)),
								   errOmitLocation(true)));

	aclresult = pg_class_aclcheck(RelationGetRelid(pkrel), GetUserId(),
								  ACL_REFERENCES);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(pkrel));

	if (!allowSystemTableMods && IsSystemRelation(pkrel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(pkrel)),
								   errOmitLocation(true)));

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_REFERENCES);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	/*
	 * Disallow reference from permanent table to temp table or vice versa.
	 * (The ban on perm->temp is for fairly obvious reasons.  The ban on
	 * temp->perm is because other backends might need to run the RI triggers
	 * on the perm table, but they can't reliably see tuples the owning
	 * backend has created in the temp table, because non-shared buffers are
	 * used for temp tables.)
	 */
	if (isTempNamespace(RelationGetNamespace(pkrel)))
	{
		if (!isTempNamespace(RelationGetNamespace(rel)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot reference temporary table from permanent table constraint"),
							   errOmitLocation(true)));
	}
	else
	{
		if (isTempNamespace(RelationGetNamespace(rel)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot reference permanent table from temporary table constraint"),
							   errOmitLocation(true)));
	}

	/*
	 * Look up the referencing attributes to make sure they exist, and record
	 * their attnums and type OIDs.
	 */
	MemSet(pkattnum, 0, sizeof(pkattnum));
	MemSet(fkattnum, 0, sizeof(fkattnum));
	MemSet(pktypoid, 0, sizeof(pktypoid));
	MemSet(fktypoid, 0, sizeof(fktypoid));
	MemSet(opclasses, 0, sizeof(opclasses));

	numfks = transformColumnNameList(RelationGetRelid(rel),
									 fkconstraint->fk_attrs,
									 fkattnum, fktypoid);

	/*
	 * If the attribute list for the referenced table was omitted, lookup the
	 * definition of the primary key and use it.  Otherwise, validate the
	 * supplied attribute list.  In either case, discover the index OID and
	 * index opclasses, and the attnums and type OIDs of the attributes.
	 */
	if (fkconstraint->pk_attrs == NIL)
	{
		numpks = transformFkeyGetPrimaryKey(pkrel, &indexOid,
											&fkconstraint->pk_attrs,
											pkattnum, pktypoid,
											opclasses);
	}
	else
	{
		numpks = transformColumnNameList(RelationGetRelid(pkrel),
										 fkconstraint->pk_attrs,
										 pkattnum, pktypoid);
		/* Look for an index matching the column list */
		indexOid = transformFkeyCheckAttrs(pkrel, numpks, pkattnum,
										   opclasses);
	}

	/* Be sure referencing and referenced column types are comparable */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree"),
						   errOmitLocation(true)));

	for (i = 0; i < numpks; i++)
	{
		/*
		 * pktypoid[i] is the primary key table's i'th key's type fktypoid[i]
		 * is the foreign key table's i'th key's type
		 *
		 * Note that we look for an operator with the PK type on the left;
		 * when the types are different this is critical because the PK index
		 * will need operators with the indexkey on the left. (Ordinarily both
		 * commutator operators will exist if either does, but we won't get
		 * the right answer from the test below on opclass membership unless
		 * we select the proper operator.)
		 */
		Operator	o = oper(NULL, list_make1(makeString("=")),
							 pktypoid[i], fktypoid[i],
							 true, -1);

		if (o == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("foreign key constraint \"%s\" "
							"cannot be implemented",
							fkconstraint->constr_name),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of incompatible types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktypoid[i]),
							   format_type_be(pktypoid[i])),
									   errOmitLocation(true)));

		/*
		 * Check that the found operator is compatible with the PK index, and
		 * generate a warning if not, since otherwise costly seqscans will be
		 * incurred to check FK validity.
		 */
		if (Gp_role != GP_ROLE_EXECUTE && !op_in_opclass(oprid(o), opclasses[i]))
			ereport(WARNING,
					(errmsg("foreign key constraint \"%s\" "
							"will require costly sequential scans",
							fkconstraint->constr_name),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of different types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktypoid[i]),
							   format_type_be(pktypoid[i])),
									   errOmitLocation(true)));

		ReleaseSysCache(o);
	}

	/*
	 * Tell Phase 3 to check that the constraint is satisfied by existing rows
	 * (we can skip this during table creation).
	 */
	if (!fkconstraint->skip_validation)
	{
		NewConstraint *newcon;

		newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
		newcon->name = fkconstraint->constr_name;
		newcon->contype = CONSTR_FOREIGN;
		newcon->refrelid = RelationGetRelid(pkrel);
		newcon->qual = (Node *) fkconstraint;

		tab->constraints = lappend(tab->constraints, newcon);
	}

	/*
	 * Record the FK constraint in pg_constraint.
	 */
	constrOid = CreateConstraintEntry(fkconstraint->constr_name,
									  RelationGetNamespace(rel),
									  CONSTRAINT_FOREIGN,
									  fkconstraint->deferrable,
									  fkconstraint->initdeferred,
									  RelationGetRelid(rel),
									  fkattnum,
									  numfks,
									  InvalidOid,		/* not a domain
														 * constraint */
									  RelationGetRelid(pkrel),
									  pkattnum,
									  numpks,
									  fkconstraint->fk_upd_action,
									  fkconstraint->fk_del_action,
									  fkconstraint->fk_matchtype,
									  indexOid,
									  NULL,		/* no check constraint */
									  NULL,
									  NULL);

	/*
	 * Create the triggers that will enforce the constraint.
	 */
	createForeignKeyTriggers(rel, fkconstraint, constrOid);

	/*
	 * Close pk table, but keep lock until we've committed.
	 */
	heap_close(pkrel, NoLock);
}


/*
 * transformColumnNameList - transform list of column names
 *
 * Lookup each name and return its attnum and type OID
 */
static int
transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids)
{
	ListCell   *l;
	int			attnum;

	attnum = 0;
	foreach(l, colList)
	{
		char	   *attname = strVal(lfirst(l));
		HeapTuple	atttuple;

		atttuple = SearchSysCacheAttName(relId, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" referenced in foreign key constraint does not exist",
							attname),
									   errOmitLocation(true)));
		if (attnum >= INDEX_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d keys in a foreign key",
							INDEX_MAX_KEYS)));
		attnums[attnum] = ((Form_pg_attribute) GETSTRUCT(atttuple))->attnum;
		atttypids[attnum] = ((Form_pg_attribute) GETSTRUCT(atttuple))->atttypid;
		ReleaseSysCache(atttuple);
		attnum++;
	}

	return attnum;
}

/*
 * transformFkeyGetPrimaryKey -
 *
 *	Look up the names, attnums, and types of the primary key attributes
 *	for the pkrel.	Also return the index OID and index opclasses of the
 *	index supporting the primary key.
 *
 *	All parameters except pkrel are output parameters.	Also, the function
 *	return value is the number of attributes in the primary key.
 *
 *	Used when the column list in the REFERENCES specification is omitted.
 */
static int
transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids,
						   Oid *opclasses)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	HeapTuple	indexTuple = NULL;
	Form_pg_index indexStruct = NULL;
	Datum		indclassDatum;
	bool		isnull;
	oidvector  *indclass;
	int			i;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).
	 */
	*indexOid = InvalidOid;

	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);
		if (indexStruct->indisprimary)
		{
			*indexOid = indexoid;
			break;
		}
		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/*
	 * Check that we found it
	 */
	if (!OidIsValid(*indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no primary key for referenced table \"%s\"",
						RelationGetRelationName(pkrel)),
								   errOmitLocation(true)));

	/* Must get indclass the hard way */
	indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
									Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * Now build the list of PK attributes from the indkey definition (we
	 * assume a primary key cannot have expressional elements)
	 */
	*attnamelist = NIL;
	for (i = 0; i < indexStruct->indnatts; i++)
	{
		int			pkattno = indexStruct->indkey.values[i];

		attnums[i] = pkattno;
		atttypids[i] = attnumTypeId(pkrel, pkattno);
		opclasses[i] = indclass->values[i];
		*attnamelist = lappend(*attnamelist,
			   makeString(pstrdup(NameStr(*attnumAttName(pkrel, pkattno)))));
	}

	ReleaseSysCache(indexTuple);

	return i;
}

/*
 * transformFkeyCheckAttrs -
 *
 *	Make sure that the attributes of a referenced table belong to a unique
 *	(or primary key) constraint.  Return the OID of the index supporting
 *	the constraint, as well as the opclasses associated with the index
 *	columns.
 */
static Oid
transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						Oid *opclasses) /* output parameter */
{
	Oid			indexoid = InvalidOid;
	bool		found = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache, and match unique indexes to the list
	 * of attnums we are given.
	 */
	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;
		int			i,
					j;

		indexoid = lfirst_oid(indexoidscan);
		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Must have the right number of columns; must be unique and not a
		 * partial index; forget it if there are any expressions, too
		 */
		if (indexStruct->indnatts == numattrs &&
			indexStruct->indisunique &&
			heap_attisnull(indexTuple, Anum_pg_index_indpred) &&
			heap_attisnull(indexTuple, Anum_pg_index_indexprs))
		{
			/* Must get indclass the hard way */
			Datum		indclassDatum;
			bool		isnull;
			oidvector  *indclass;

			indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
											Anum_pg_index_indclass, &isnull);
			Assert(!isnull);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/*
			 * The given attnum list may match the index columns in any order.
			 * Check that each list is a subset of the other.
			 */
			for (i = 0; i < numattrs; i++)
			{
				found = false;
				for (j = 0; j < numattrs; j++)
				{
					if (attnums[i] == indexStruct->indkey.values[j])
					{
						found = true;
						break;
					}
				}
				if (!found)
					break;
			}
			if (found)
			{
				for (i = 0; i < numattrs; i++)
				{
					found = false;
					for (j = 0; j < numattrs; j++)
					{
						if (attnums[j] == indexStruct->indkey.values[i])
						{
							opclasses[j] = indclass->values[i];
							found = true;
							break;
						}
					}
					if (!found)
						break;
				}
			}
		}
		ReleaseSysCache(indexTuple);
		if (found)
			break;
	}

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("there is no unique constraint matching given keys for referenced table \"%s\"",
						RelationGetRelationName(pkrel)),
								   errOmitLocation(true)));

	list_free(indexoidlist);

	return indexoid;
}

/*
 * Scan the existing rows in a table to verify they meet a proposed FK
 * constraint.
 *
 * Caller must have opened and locked both relations.
 */
static void
validateForeignKeyConstraint(FkConstraint *fkconstraint,
							 Relation rel,
							 Relation pkrel)
{
	HeapScanDesc scan;
	HeapTuple	tuple;
	Trigger		trig;
	ListCell   *list;
	int			count;

	/*
	 * See if we can do it with a single LEFT JOIN query.  A FALSE result
	 * indicates we must proceed with the fire-the-trigger method.
	 */
	if (RI_Initial_Check(fkconstraint, rel, pkrel))
		return;

	/*
	 * Scan through each tuple, calling RI_FKey_check_ins (insert trigger) as
	 * if that tuple had just been inserted.  If any of those fail, it should
	 * ereport(ERROR) and that's that.
	 */
	MemSet(&trig, 0, sizeof(trig));
	trig.tgoid = InvalidOid;
	trig.tgname = fkconstraint->constr_name;
	trig.tgenabled = TRUE;
	trig.tgisconstraint = TRUE;
	trig.tgconstrrelid = RelationGetRelid(pkrel);
	trig.tgdeferrable = FALSE;
	trig.tginitdeferred = FALSE;

	trig.tgargs = (char **) palloc(sizeof(char *) *
								   (4 + list_length(fkconstraint->fk_attrs)
									+ list_length(fkconstraint->pk_attrs)));

	trig.tgargs[0] = trig.tgname;
	trig.tgargs[1] = RelationGetRelationName(rel);
	trig.tgargs[2] = RelationGetRelationName(pkrel);
	trig.tgargs[3] = fkMatchTypeToString(fkconstraint->fk_matchtype);
	count = 4;
	foreach(list, fkconstraint->fk_attrs)
	{
		char	   *fk_at = strVal(lfirst(list));

		trig.tgargs[count] = fk_at;
		count += 2;
	}
	count = 5;
	foreach(list, fkconstraint->pk_attrs)
	{
		char	   *pk_at = strVal(lfirst(list));

		trig.tgargs[count] = pk_at;
		count += 2;
	}
	trig.tgnargs = count - 1;

	scan = heap_beginscan(rel, SnapshotNow, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		FunctionCallInfoData fcinfo;
		TriggerData trigdata;

		/*
		 * Make a call to the trigger function
		 *
		 * No parameters are passed, but we do set a context
		 */
		MemSet(&fcinfo, 0, sizeof(fcinfo));

		/*
		 * We assume RI_FKey_check_ins won't look at flinfo...
		 */
		trigdata.type = T_TriggerData;
		trigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
		trigdata.tg_relation = rel;
		trigdata.tg_trigtuple = tuple;
		trigdata.tg_newtuple = NULL;
		trigdata.tg_trigger = &trig;
		trigdata.tg_trigtuplebuf = scan->rs_cbuf;
		trigdata.tg_newtuplebuf = InvalidBuffer;

		fcinfo.context = (Node *) &trigdata;

		RI_FKey_check_ins(&fcinfo);
	}

	heap_endscan(scan);

	pfree(trig.tgargs);
}

static void
CreateFKCheckTrigger(RangeVar *myRel, FkConstraint *fkconstraint,
					 ObjectAddress *constrobj, ObjectAddress *trigobj,
					 bool on_insert)
{
	CreateTrigStmt *fk_trigger;
	ListCell   *fk_attr;
	ListCell   *pk_attr;

	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = fkconstraint->constr_name;
	fk_trigger->relation = myRel;
	fk_trigger->before = false;
	fk_trigger->row = true;

	/* Either ON INSERT or ON UPDATE */
	if (on_insert)
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_ins");
		fk_trigger->actions[0] = 'i';
		fk_trigger->trigOid = fkconstraint->trig1Oid;
	}
	else
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_upd");
		fk_trigger->actions[0] = 'u';
		fk_trigger->trigOid = fkconstraint->trig2Oid;
	}
	fk_trigger->actions[1] = '\0';

	fk_trigger->isconstraint = true;
	fk_trigger->deferrable = fkconstraint->deferrable;
	fk_trigger->initdeferred = fkconstraint->initdeferred;
	fk_trigger->constrrel = fkconstraint->pktable;

	fk_trigger->args = NIL;
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->constr_name));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(myRel->relname));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->pktable->relname));
	fk_trigger->args = lappend(fk_trigger->args,
				makeString(fkMatchTypeToString(fkconstraint->fk_matchtype)));
	if (list_length(fkconstraint->fk_attrs) != list_length(fkconstraint->pk_attrs))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree"),
						   errOmitLocation(true)));

	forboth(fk_attr, fkconstraint->fk_attrs,
			pk_attr, fkconstraint->pk_attrs)
	{
		fk_trigger->args = lappend(fk_trigger->args, lfirst(fk_attr));
		fk_trigger->args = lappend(fk_trigger->args, lfirst(pk_attr));
	}

	trigobj->objectId = CreateTrigger(fk_trigger, true);

	if (on_insert)
		fkconstraint->trig1Oid = trigobj->objectId;
	else
		fkconstraint->trig2Oid = trigobj->objectId;

	/* Register dependency from trigger to constraint */
	recordDependencyOn(trigobj, constrobj, DEPENDENCY_INTERNAL);

	/* Make changes-so-far visible */
	CommandCounterIncrement();
}

/*
 * Create the triggers that implement an FK constraint.
 */
static void
createForeignKeyTriggers(Relation rel, FkConstraint *fkconstraint,
						 Oid constrOid)
{
	RangeVar   *myRel;
	CreateTrigStmt *fk_trigger;
	ListCell   *fk_attr;
	ListCell   *pk_attr;
	ObjectAddress trigobj,
				constrobj;

	/*
	 * Special for Greenplum Database: Ignore foreign keys for now, with warning
	 */
	if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_EXECUTE)
	{
		if (Gp_role == GP_ROLE_DISPATCH)
			ereport(WARNING,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Referential integrity (FOREIGN KEY) constraints are not supported in Greenplum Database, will not be enforced."),
						   errOmitLocation(true)));
	}

	/*
	 * Reconstruct a RangeVar for my relation (not passed in, unfortunately).
	 */
	myRel = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
						 pstrdup(RelationGetRelationName(rel)), -1);

	/*
	 * Preset objectAddress fields
	 */
	constrobj.classId = ConstraintRelationId;
	constrobj.objectId = constrOid;
	constrobj.objectSubId = 0;
	trigobj.classId = TriggerRelationId;
	trigobj.objectSubId = 0;

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the CHECK
	 * action for both INSERTs and UPDATEs on the referencing table.
	 */
	CreateFKCheckTrigger(myRel, fkconstraint, &constrobj, &trigobj, true);
	CreateFKCheckTrigger(myRel, fkconstraint, &constrobj, &trigobj, false);

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * DELETE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = fkconstraint->constr_name;
	fk_trigger->relation = fkconstraint->pktable;
	fk_trigger->before = false;
	fk_trigger->row = true;
	fk_trigger->actions[0] = 'd';
	fk_trigger->actions[1] = '\0';

	fk_trigger->isconstraint = true;
	fk_trigger->constrrel = myRel;
	switch (fkconstraint->fk_del_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_del");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_del");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_del");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_del");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_del");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_del_action);
			break;
	}

	fk_trigger->args = NIL;
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->constr_name));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(myRel->relname));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->pktable->relname));
	fk_trigger->args = lappend(fk_trigger->args,
				makeString(fkMatchTypeToString(fkconstraint->fk_matchtype)));
	forboth(fk_attr, fkconstraint->fk_attrs,
			pk_attr, fkconstraint->pk_attrs)
	{
		fk_trigger->args = lappend(fk_trigger->args, lfirst(fk_attr));
		fk_trigger->args = lappend(fk_trigger->args, lfirst(pk_attr));
	}

	fk_trigger->trigOid = fkconstraint->trig3Oid;

	trigobj.objectId = CreateTrigger(fk_trigger, true);

	fkconstraint->trig3Oid = trigobj.objectId;

	/* Register dependency from trigger to constraint */
	recordDependencyOn(&trigobj, &constrobj, DEPENDENCY_INTERNAL);

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * UPDATE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = fkconstraint->constr_name;
	fk_trigger->relation = fkconstraint->pktable;
	fk_trigger->before = false;
	fk_trigger->row = true;
	fk_trigger->actions[0] = 'u';
	fk_trigger->actions[1] = '\0';
	fk_trigger->isconstraint = true;
	fk_trigger->constrrel = myRel;
	switch (fkconstraint->fk_upd_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_upd");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_upd");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_upd");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_upd");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_upd");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_upd_action);
			break;
	}

	fk_trigger->args = NIL;
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->constr_name));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(myRel->relname));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->pktable->relname));
	fk_trigger->args = lappend(fk_trigger->args,
				makeString(fkMatchTypeToString(fkconstraint->fk_matchtype)));
	forboth(fk_attr, fkconstraint->fk_attrs,
			pk_attr, fkconstraint->pk_attrs)
	{
		fk_trigger->args = lappend(fk_trigger->args, lfirst(fk_attr));
		fk_trigger->args = lappend(fk_trigger->args, lfirst(pk_attr));
	}

	fk_trigger->trigOid = fkconstraint->trig4Oid;

	trigobj.objectId = CreateTrigger(fk_trigger, true);

	fkconstraint->trig4Oid = trigobj.objectId;

	/* Register dependency from trigger to constraint */
	recordDependencyOn(&trigobj, &constrobj, DEPENDENCY_INTERNAL);
}

/*
 * fkMatchTypeToString -
 *	  convert FKCONSTR_MATCH_xxx code to string to use in trigger args
 */
static char *
fkMatchTypeToString(char match_type)
{
	switch (match_type)
	{
		case FKCONSTR_MATCH_FULL:
			return pstrdup("FULL");
		case FKCONSTR_MATCH_PARTIAL:
			return pstrdup("PARTIAL");
		case FKCONSTR_MATCH_UNSPECIFIED:
			return pstrdup("UNSPECIFIED");
		default:
			elog(ERROR, "unrecognized match type: %d",
				 (int) match_type);
	}
	return NULL;				/* can't get here */
}

/*
 * ALTER TABLE DROP CONSTRAINT
 */
static void
ATPrepDropConstraint(List **wqueue, Relation rel,
					 bool recurse, AlterTableCmd *cmd)
{
	/*
	 * We don't want errors or noise from child tables, so we have to pass
	 * down a modified command.
	 *
	 * This can't result in dropping the partitioning rule because the
	 * partitioning rule exists only on the part, and ATPrepCmd guards
	 * against dropping directly from parts.
	 */
	if (recurse)
	{
		AlterTableCmd *childCmd = copyObject(cmd);

		childCmd->subtype = AT_DropConstraintQuietly;
		ATSimpleRecursion(wqueue, rel, childCmd, recurse);
	}
}

static void
ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior, bool quiet)
{
	int			deleted;

	deleted = RemoveRelConstraints(rel, constrName, behavior);

	if (!quiet)
	{
		/* If zero constraints deleted, complain */
		if (deleted == 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" does not exist",
							constrName),
									   errOmitLocation(true)));
		/* Otherwise if more than one constraint deleted, notify */
		else if (deleted > 1 && Gp_role != GP_ROLE_EXECUTE)
			ereport(NOTICE,
					(errmsg("multiple constraints named \"%s\" were dropped",
							constrName),
									   errOmitLocation(true)));
	}

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "DROP CONSTRAINT"
				);

}

/*
 * ALTER COLUMN TYPE
 */
static void
ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd)
{
	char	   *colName = cmd->name;
	TypeName   *typname = (TypeName *) cmd->def;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Oid			targettype;
	Node	   *transform;
	NewColumnValue *newval;
	ParseState *pstate = make_parsestate(NULL);

	/* lookup the attribute so we can check inheritance status */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel)),
								   errOmitLocation(true)));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't alter a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName),
								   errOmitLocation(true)));

	/* Don't alter inherited columns */
	if (attTup->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter inherited column \"%s\"",
						colName),
								   errOmitLocation(true)));

	/* Look up the target type */
	targettype = typenameTypeId(NULL, typname);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colName, targettype);

	/*
	 * Set up an expression to transform the old data value to the new type.
	 * If a USING option was given, transform and use that expression, else
	 * just take the old value and try to coerce it.  We do this first so that
	 * type incompatibility can be detected before we waste effort, and
	 * because we need the expression to be parsed against the original table
	 * rowtype.
	 */

	/* GPDB: we always need the RTE */
	{
		RangeTblEntry *rte;

		/* Expression must be able to access vars of old table */
		rte = addRangeTableEntryForRelation(pstate,
											rel,
											NULL,
											false,
											true);
		addRTEtoQuery(pstate, rte, false, true, true);
	}

	if (cmd->transform)
	{
		transform = transformExpr(pstate, cmd->transform);
		/* It can't return a set */
		if (expression_returns_set(transform))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("transform expression must not return a set"),
							   errOmitLocation(true)));

		/* No subplans or aggregates, either... */
		if (pstate->p_hasSubLinks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot use subquery in transform expression"),
							   errOmitLocation(true)));
		if (pstate->p_hasAggs)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
			errmsg("cannot use aggregate function in transform expression"),
					   errOmitLocation(true)));
		if (pstate->p_hasWindFuncs)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("cannot use window function in transform expression"),
					   errOmitLocation(true)));

	}
	else
	{
		transform = (Node *) makeVar(1, attnum,
									 attTup->atttypid, attTup->atttypmod,
									 0);
	}

	transform = coerce_to_target_type(pstate,
									  transform, exprType(transform),
									  targettype, typname->typmod,
									  COERCION_ASSIGNMENT,
									  COERCE_IMPLICIT_CAST,
									  -1);
	if (transform == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" cannot be cast to type \"%s\"",
						colName, TypeNameToString(typname)),
								   errOmitLocation(true)));

	free_parsestate(&pstate);

	/*
	 * Add a work queue item to make ATRewriteTable update the column
	 * contents.
	 */
	newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
	newval->attnum = attnum;
	newval->expr = (Expr *) transform;

	tab->newvals = lappend(tab->newvals, newval);

	ReleaseSysCache(tuple);

	/*
	 * The recursion case is handled by ATSimpleRecursion.	However, if we are
	 * told not to recurse, there had better not be any child tables; else the
	 * alter would put them out of step.
	 */
	if (recurse)
		ATSimpleRecursion(wqueue, rel, cmd, recurse);
	else if (!recursing &&
			 find_inheritance_children(RelationGetRelid(rel)) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("type of inherited column \"%s\" must be changed in child tables too",
						colName),
								   errOmitLocation(true)));
}

static void
ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  const char *colName, TypeName *typname)
{
	HeapTuple	heapTup;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	HeapTuple	typeTuple;
	Form_pg_type tform;
	Oid			targettype;
	Node	   *defaultexpr;
	Relation	attrelation;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;
	GpPolicy * policy = NULL;
	bool		sourceIsInt = false;
	bool		targetIsInt = false;
	bool		sourceIsVarlenA = false;
	bool		targetIsVarlenA = false;
	bool		hashCompatible = false;

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	/* Look up the target column */
	heapTup = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(heapTup))		/* shouldn't happen */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel)),
								   errOmitLocation(true)));
	attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
	attnum = attTup->attnum;

	/* Check for multiple ALTER TYPE on same column --- can't cope */
	if (attTup->atttypid != tab->oldDesc->attrs[attnum - 1]->atttypid ||
		attTup->atttypmod != tab->oldDesc->attrs[attnum - 1]->atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter type of column \"%s\" twice",
						colName),
								   errOmitLocation(true)));

	/* Look up the target type (should not fail, since prep found it) */
	typeTuple = typenameType(NULL, typname);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	targettype = HeapTupleGetOid(typeTuple);

	if (targettype == INT4OID ||
		targettype == INT2OID ||
		targettype == INT8OID)
		sourceIsInt = true;

	if (attTup->atttypid == INT4OID ||
		attTup->atttypid == INT2OID ||
		attTup->atttypid == INT8OID)
		targetIsInt = true;

	if (targettype == VARCHAROID ||
		targettype == CHAROID ||
		targettype == TEXTOID)
		targetIsVarlenA = true;

	if (attTup->atttypid == VARCHAROID ||
		attTup->atttypid == CHAROID ||
		attTup->atttypid == TEXTOID)
		sourceIsVarlenA = true;

	if (sourceIsInt && targetIsInt)
		hashCompatible = true;
	else if (sourceIsVarlenA && targetIsVarlenA)
		hashCompatible = true;

	/*
	 * If there is a default expression for the column, get it and ensure we
	 * can coerce it to the new datatype.  (We must do this before changing
	 * the column type, because build_column_default itself will try to
	 * coerce, and will not issue the error message we want if it fails.)
	 *
	 * We remove any implicit coercion steps at the top level of the old
	 * default expression; this has been agreed to satisfy the principle of
	 * least surprise.	(The conversion to the new column type should act like
	 * it started from what the user sees as the stored expression, and the
	 * implicit coercions aren't going to be shown.)
	 */
	if (attTup->atthasdef)
	{
		defaultexpr = build_column_default(rel, attnum);
		Assert(defaultexpr);
		defaultexpr = strip_implicit_coercions(defaultexpr);
		defaultexpr = coerce_to_target_type(NULL,		/* no UNKNOWN params */
										  defaultexpr, exprType(defaultexpr),
											targettype, typname->typmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (defaultexpr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			errmsg("default for column \"%s\" cannot be cast to type \"%s\"",
				   colName, TypeNameToString(typname)),
						   errOmitLocation(true)));
	}
	else
		defaultexpr = NULL;

	/*
	 * Find everything that depends on the column (constraints, indexes, etc),
	 * and record enough information to let us recreate the objects.
	 *
	 * The actual recreation does not happen here, but only after we have
	 * performed all the individual ALTER TYPE operations.	We have to save
	 * the info before executing ALTER TYPE, though, else the deparser will
	 * get confused.
	 *
	 * There could be multiple entries for the same object, so we must check
	 * to ensure we process each one only once.  Note: we assume that an index
	 * that implements a constraint will not show a direct dependency on the
	 * column.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		/* We don't expect any PIN dependencies on columns */
		if (foundDep->deptype == DEPENDENCY_PIN)
			elog(ERROR, "cannot alter type of a pinned column");

		foundObject.classId = foundDep->classid;
		foundObject.objectId = foundDep->objid;
		foundObject.objectSubId = foundDep->objsubid;

		switch (getObjectClass(&foundObject))
		{
			case OCLASS_CLASS:
				{
					char		relKind = get_rel_relkind(foundObject.objectId);

					if (relKind == RELKIND_INDEX)
					{
						Assert(foundObject.objectSubId == 0);
						if (!list_member_oid(tab->changedIndexOids, foundObject.objectId))
						{
							char * indexdefstring = pg_get_indexdef_string(foundObject.objectId);
							tab->changedIndexOids = lappend_oid(tab->changedIndexOids,
													   foundObject.objectId);
							tab->changedIndexDefs = lappend(tab->changedIndexDefs,indexdefstring);

							if (Gp_role == GP_ROLE_DISPATCH && indexdefstring &&strstr(indexdefstring," UNIQUE ")!=0 && !hashCompatible)
							{
								policy = rel->rd_cdbpolicy;
								if (policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED)
								{
									int			ia = 0;

									if (cdbRelSize(rel->rd_id) != 0)

									for (ia = 0; ia < policy->nattrs; ia++)
									{
										if (attnum == policy->attrs[ia])
										{
											ereport(ERROR,
													(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
													 errmsg("Changing the type of a column that is part of the distribution policy and used in a unique index is not allowed"),
															   errOmitLocation(true)));
										}
									}
								}
							}
						}
					}
					else if (relKind == RELKIND_SEQUENCE)
					{
						/*
						 * This must be a SERIAL column's sequence.  We need
						 * not do anything to it.
						 */
						Assert(foundObject.objectSubId == 0);
					}
					else
					{
						/* Not expecting any other direct dependencies... */
						elog(ERROR, "unexpected object depending on column: %s",
							 getObjectDescription(&foundObject));
					}
					break;
				}

			case OCLASS_CONSTRAINT:
				Assert(foundObject.objectSubId == 0);
				if (!list_member_oid(tab->changedConstraintOids,
									 foundObject.objectId))
				{
					char	   *defstring = pg_get_constraintdef_string(foundObject.objectId);

					if (Gp_role == GP_ROLE_DISPATCH && !hashCompatible)
					if (strstr(defstring," UNIQUE")!=0 ||
						strstr(defstring,"PRIMARY KEY")!=0 )
					{
						policy = rel->rd_cdbpolicy;
						if (policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED)
						{
							int			ia = 0;

							if (cdbRelSize(rel->rd_id) != 0)

							for (ia = 0; ia < policy->nattrs; ia++)
							{
								if (attnum == policy->attrs[ia])
								{
									ereport(ERROR,
											(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
											 errmsg("Changing the type of a column that is used in a UNIQUE or PRIMARY KEY constraint is not allowed"),
													   errOmitLocation(true)));
								}
							}
						}
					}

					/*
					 * Put NORMAL dependencies at the front of the list and
					 * AUTO dependencies at the back.  This makes sure that
					 * foreign-key constraints depending on this column will
					 * be dropped before unique or primary-key constraints of
					 * the column; which we must have because the FK
					 * constraints depend on the indexes belonging to the
					 * unique constraints.
					 */
					if (foundDep->deptype == DEPENDENCY_NORMAL)
					{
						tab->changedConstraintOids =
							lcons_oid(foundObject.objectId,
									  tab->changedConstraintOids);
						tab->changedConstraintDefs =
							lcons(defstring,
								  tab->changedConstraintDefs);
					}
					else
					{
						tab->changedConstraintOids =
							lappend_oid(tab->changedConstraintOids,
													   foundObject.objectId);
						tab->changedConstraintDefs =
							lappend(tab->changedConstraintDefs,
									defstring);
				}
				}
				break;

			case OCLASS_REWRITE:
				/* XXX someday see if we can cope with revising views */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used by a view or rule"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject),
								   colName),
										   errOmitLocation(true)));
				break;

			case OCLASS_DEFAULT:

				/*
				 * Ignore the column's default expression, since we will fix
				 * it below.
				 */
				Assert(defaultexpr);
				break;

			case OCLASS_PROC:
			case OCLASS_TYPE:
			case OCLASS_CAST:
			case OCLASS_CONVERSION:
			case OCLASS_LANGUAGE:
			case OCLASS_OPERATOR:
			case OCLASS_OPCLASS:
			case OCLASS_TRIGGER:
			case OCLASS_SCHEMA:

				/*
				 * We don't expect any of these sorts of objects to depend on
				 * a column.
				 */
				elog(ERROR, "unexpected object depending on column: %s",
					 getObjectDescription(&foundObject));
				break;

			default:
				elog(ERROR, "unrecognized object class: %u",
					 foundObject.classId);
		}
	}

	systable_endscan(scan);

	/*
	 * Now scan for dependencies of this column on other things.  The only
	 * thing we should find is the dependency on the column datatype, which we
	 * want to remove.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);

		if (foundDep->deptype != DEPENDENCY_NORMAL)
			elog(ERROR, "found unexpected dependency type '%c'",
				 foundDep->deptype);
		if (foundDep->refclassid != TypeRelationId ||
			foundDep->refobjid != attTup->atttypid)
			elog(ERROR, "found unexpected dependency for column");

		simple_heap_delete(depRel, &depTup->t_self);
	}

	systable_endscan(scan);

	heap_close(depRel, RowExclusiveLock);

	policy = rel->rd_cdbpolicy;
	if (policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED &&
		!hashCompatible)
	{
		int			ia = 0;

		if (cdbRelSize(rel->rd_id) != 0)
		{
			for (ia = 0; ia < policy->nattrs; ia++)
			{
				if (attnum == policy->attrs[ia])
				{
					if (Gp_role != GP_ROLE_EXECUTE)
						ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter type of a column used in "
									"a distribution policy"),
											   errOmitLocation(true)));
				}
			}
		}
	}

	/*
	 * Here we go --- change the recorded column type.	(Note heapTup is a
	 * copy of the syscache entry, so okay to scribble on.)
	 */
	attTup->atttypid = targettype;
	attTup->atttypmod = typname->typmod;
	attTup->attndims = list_length(typname->arrayBounds);
	attTup->attlen = tform->typlen;
	attTup->attbyval = tform->typbyval;
	attTup->attalign = tform->typalign;
	attTup->attstorage = tform->typstorage;

	ReleaseSysCache(typeTuple);

	simple_heap_update(attrelation, &heapTup->t_self, heapTup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, heapTup);

	heap_close(attrelation, RowExclusiveLock);

	/* Install dependency on new datatype */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);

	/*
	 * Drop any pg_statistic entry for the column, since it's now wrong type
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	/*
	 * Update the default, if present, by brute force --- remove and re-add
	 * the default.  Probably unsafe to take shortcuts, since the new version
	 * may well have additional dependencies.  (It's okay to do this now,
	 * rather than after other ALTER TYPE commands, since the default won't
	 * depend on other column types.)
	 */
	if (defaultexpr)
	{
		/* Must make new row visible since it will be updated again */
		CommandCounterIncrement();

		/*
		 * We use RESTRICT here for safety, but at present we do not expect
		 * anything to depend on the default.
		 */
		RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true);

		StoreAttrDefault(rel, attnum, nodeToString(defaultexpr));
	}

	/* Cleanup */
	heap_freetuple(heapTup);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "ALTER COLUMN TYPE"
				);


}

/*
 * Cleanup after we've finished all the ALTER TYPE operations for a
 * particular relation.  We have to drop and recreate all the indexes
 * and constraints that depend on the altered columns.
 */
static void
ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab)
{
	ObjectAddress obj;
	ListCell   *l;

	/*
	 * Re-parse the index and constraint definitions, and attach them to the
	 * appropriate work queue entries.	We do this before dropping because in
	 * the case of a FOREIGN KEY constraint, we might not yet have exclusive
	 * lock on the table the constraint is attached to, and we need to get
	 * that before dropping.  It's safe because the parser won't actually look
	 * at the catalogs to detect the existing entry.
	 */
	foreach(l, tab->changedIndexDefs)
	{
		/*
		 * Temporary workaround for MPP-1318. INDEX CREATE is dispatched
		 * immediately, which unfortunately breaks the ALTER work queue.
		 */
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot alter indexed column"),
			 errhint("DROP the index first, and recreate it after the ALTER"),
					   errOmitLocation(true)));
		/*ATPostAlterTypeParse((char *) lfirst(l), wqueue);*/
	}
	foreach(l, tab->changedConstraintDefs)
		ATPostAlterTypeParse((char *) lfirst(l), wqueue);

	/*
	 * Now we can drop the existing constraints and indexes --- constraints
	 * first, since some of them might depend on the indexes.  In fact, we
	 * have to delete FOREIGN KEY constraints before UNIQUE constraints, but
	 * we already ordered the constraint list to ensure that would happen. It
	 * should be okay to use DROP_RESTRICT here, since nothing else should be
	 * depending on these objects.
	 */
	foreach(l, tab->changedConstraintOids)
	{
		obj.classId = ConstraintRelationId;
		obj.objectId = lfirst_oid(l);
		obj.objectSubId = 0;
		performDeletion(&obj, DROP_RESTRICT);
	}

	foreach(l, tab->changedIndexOids)
	{
		obj.classId = RelationRelationId;
		obj.objectId = lfirst_oid(l);
		obj.objectSubId = 0;
		performDeletion(&obj, DROP_RESTRICT);
	}

	/*
	 * The objects will get recreated during subsequent passes over the work
	 * queue.
	 */
}

static void
ATPostAlterTypeParse(char *cmd, List **wqueue)
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *list_item;

	/*
	 * We expect that we only have to do raw parsing and parse analysis, not
	 * any rule rewriting, since these will all be utility statements.
	 */
	raw_parsetree_list = raw_parser(cmd);
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);

		querytree_list = list_concat(querytree_list,
									 parse_analyze(parsetree, cmd, NULL, 0));
	}

	/*
	 * Attach each generated command to the proper place in the work queue.
	 * Note this could result in creation of entirely new work-queue entries.
	 */
	foreach(list_item, querytree_list)
	{
		Query	   *query = (Query *) lfirst(list_item);
		Relation	rel;
		AlteredTableInfo *tab;

		Assert(IsA(query, Query));
		Assert(query->commandType == CMD_UTILITY);
		switch (nodeTag(query->utilityStmt))
		{
			case T_IndexStmt:
				{
					IndexStmt  *stmt = (IndexStmt *) query->utilityStmt;
					AlterTableCmd *newcmd;

					rel = relation_openrv(stmt->relation, AccessExclusiveLock);
					tab = ATGetQueueEntry(wqueue, rel);
					newcmd = makeNode(AlterTableCmd);
					newcmd->subtype = AT_ReAddIndex;
					newcmd->def = (Node *) stmt;
					tab->subcmds[AT_PASS_OLD_INDEX] =
						lappend(tab->subcmds[AT_PASS_OLD_INDEX], newcmd);
					relation_close(rel, NoLock);
					break;
				}
			case T_AlterTableStmt:
				{
					AlterTableStmt *stmt = (AlterTableStmt *) query->utilityStmt;
					ListCell   *lcmd;

					rel = relation_openrv(stmt->relation, AccessExclusiveLock);
					tab = ATGetQueueEntry(wqueue, rel);
					foreach(lcmd, stmt->cmds)
					{
						AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

						switch (cmd->subtype)
						{
							case AT_AddIndex:
								cmd->subtype = AT_ReAddIndex;
								tab->subcmds[AT_PASS_OLD_INDEX] =
									lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);
								break;
							case AT_AddConstraint:
								tab->subcmds[AT_PASS_OLD_CONSTR] =
									lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
								break;
							default:
								elog(ERROR, "unexpected statement type: %d",
									 (int) cmd->subtype);
						}
					}
					relation_close(rel, NoLock);
					break;
				}
			default:
				elog(ERROR, "unexpected statement type: %d",
					 (int) nodeTag(query->utilityStmt));
		}
	}
}


/*
 * ALTER TABLE OWNER
 *
 * recursing is true if we are recursing from a table to its indexes,
 * sequences, or toast table.  We don't allow the ownership of those things to
 * be changed separately from the parent table.  Also, we can skip permission
 * checks (this is necessary not just an optimization, else we'd fail to
 * handle toast tables properly).
 *
 * recursing is also true if ALTER TYPE OWNER is calling us to fix up a
 * free-standing composite type.
 */
void
ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing)
{
	Relation	target_rel;
	Relation	class_rel;
	HeapTuple	tuple;
	Form_pg_class tuple_class;

	/*
	 * Get exclusive lock till end of transaction on the target table. Use
	 * relation_open so that we can work on indexes and sequences.
	 */
	target_rel = relation_open(relationOid, AccessExclusiveLock);

	/* Get its pg_class tuple, too */
	class_rel = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relationOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationOid);
	tuple_class = (Form_pg_class) GETSTRUCT(tuple);

	/* Can we change the ownership of this tuple? */
	switch (tuple_class->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
			/* ok to change owner */
			break;
		case RELKIND_INDEX:
			if (!recursing)
			{
				/*
				 * Because ALTER INDEX OWNER used to be allowed, and in fact
				 * is generated by old versions of pg_dump, we give a warning
				 * and do nothing rather than erroring out.  Also, to avoid
				 * unnecessary chatter while restoring those old dumps, say
				 * nothing at all if the command would be a no-op anyway.
				 */
				if (tuple_class->relowner != newOwnerId)
					ereport(WARNING,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("cannot change owner of index \"%s\"",
									NameStr(tuple_class->relname)),
							 errhint("Change the ownership of the index's table, instead."),
									   errOmitLocation(true)));
				/* quick hack to exit via the no-op path */
				newOwnerId = tuple_class->relowner;
			}
			break;
		case RELKIND_SEQUENCE:
			if (!recursing &&
				tuple_class->relowner != newOwnerId)
			{
				/* if it's an owned sequence, disallow changing it by itself */
				Oid			tableId;
				int32		colId;

				if (sequenceIsOwned(relationOid, &tableId, &colId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot change owner of sequence \"%s\"",
									NameStr(tuple_class->relname)),
					  errdetail("Sequence \"%s\" is linked to table \"%s\".",
								NameStr(tuple_class->relname),
								get_rel_name(tableId)),
										   errOmitLocation(true)));
			}
			break;
		case RELKIND_TOASTVALUE:
		case RELKIND_AOSEGMENTS:
		case RELKIND_AOBLOCKDIR:
		case RELKIND_COMPOSITE_TYPE:
			if (recursing)
				break;
			/* FALL THRU */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table, view, or sequence",
							NameStr(tuple_class->relname)),
									   errOmitLocation(true)));
	}

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (tuple_class->relowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_class];
		bool		repl_null[Natts_pg_class];
		bool		repl_repl[Natts_pg_class];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* skip permission checks when recursing to index or toast table */
		if (!recursing)
		{
			/* Superusers can always do it */
			if (!superuser())
			{
				Oid			namespaceOid = tuple_class->relnamespace;
				AclResult	aclresult;

				/* Otherwise, must be owner of the existing object */
				if (!pg_class_ownercheck(relationOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
								   RelationGetRelationName(target_rel));

				/* Must be able to become new owner */
				check_is_member_of_role(GetUserId(), newOwnerId);

				/* New owner must have CREATE privilege on namespace */
				aclresult = pg_namespace_aclcheck(namespaceOid, newOwnerId,
												  ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
								   get_namespace_name(namespaceOid));
			}
		}

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_class_relowner - 1] = true;
		repl_val[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = SysCacheGetAttr(RELOID, tuple,
								   Anum_pg_class_relacl,
								   &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 tuple_class->relowner, newOwnerId);
			repl_repl[Anum_pg_class_relacl - 1] = true;
			repl_val[Anum_pg_class_relacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(class_rel), repl_val, repl_null, repl_repl);

		simple_heap_update(class_rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(class_rel, newtuple);

		heap_freetuple(newtuple);

		/*
		 * Update owner dependency reference, if any.  A composite type has
		 * none, because it's tracked for the pg_type entry instead of here;
		 * indexes don't have their own entries either.
		 */
		if (tuple_class->relkind != RELKIND_COMPOSITE_TYPE &&
			tuple_class->relkind != RELKIND_INDEX &&
			tuple_class->relkind != RELKIND_TOASTVALUE)
			changeDependencyOnOwner(RelationRelationId, relationOid,
									newOwnerId);

		/*
		 * Also change the ownership of the table's rowtype, if it has one
		 */
		if (tuple_class->relkind != RELKIND_INDEX)
			AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId,
							tuple_class->relkind == RELKIND_COMPOSITE_TYPE);

		/*
		 * If we are operating on a table, also change the ownership of any
		 * indexes and sequences that belong to the table, as well as the
		 * table's toast table (if it has one)
		 */
		if (tuple_class->relkind == RELKIND_RELATION ||
			tuple_class->relkind == RELKIND_TOASTVALUE ||
			tuple_class->relkind == RELKIND_AOSEGMENTS ||
			tuple_class->relkind == RELKIND_AOBLOCKDIR)
		{
			List	   *index_oid_list;
			ListCell   *i;

			/* Find all the indexes belonging to this relation */
			index_oid_list = RelationGetIndexList(target_rel);

			/* For each index, recursively change its ownership */
			foreach(i, index_oid_list)
				ATExecChangeOwner(lfirst_oid(i), newOwnerId, true);

			list_free(index_oid_list);
		}

		if (tuple_class->relkind == RELKIND_RELATION)
		{
			/* If it has a toast table, recurse to change its ownership */
			if (tuple_class->reltoastrelid != InvalidOid)
				ATExecChangeOwner(tuple_class->reltoastrelid, newOwnerId,
								  true);

			if(RelationIsAoRows(target_rel) || RelationIsAoCols(target_rel))
			{
				Oid segrelid, blkdirrelid;
				GetAppendOnlyEntryAuxOids(relationOid, &segrelid, NULL,
										  &blkdirrelid, NULL);
				
				/* If it has an AO segment table, recurse to change its
				 * ownership */
				if (segrelid != InvalidOid)
					ATExecChangeOwner(segrelid, newOwnerId, true);

				/* If it has an AO block directory table, recurse to change its
				 * ownership */
				if (blkdirrelid != InvalidOid)
					ATExecChangeOwner(blkdirrelid, newOwnerId, true);
			}
			
			/* If it has dependent sequences, recurse to change them too */
			change_owner_recurse_to_sequences(relationOid, newOwnerId);
		}
	}

	ReleaseSysCache(tuple);
	heap_close(class_rel, RowExclusiveLock);
	relation_close(target_rel, NoLock);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(tuple_class)
			)
		MetaTrackUpdObject(RelationRelationId,
						   relationOid,
						   GetUserId(),
						   "ALTER", "OWNER"
				);

}

/*
 * change_owner_recurse_to_sequences
 *
 * Helper function for ATExecChangeOwner.  Examines pg_depend searching
 * for sequences that are dependent on serial columns, and changes their
 * ownership.
 */
static void
change_owner_recurse_to_sequences(Oid relationOid, Oid newOwnerId)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL sequences are those having an auto dependency on one of the
	 * table's columns (we don't care *which* column, exactly).
	 */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationOid));
	/* we leave refobjsubid unspecified */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* skip dependencies other than auto dependencies on columns */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			depForm->deptype != DEPENDENCY_AUTO)
			continue;

		/* Use relation_open just in case it's an index */
		seqRel = relation_open(depForm->objid, AccessExclusiveLock);

		/* skip non-sequence relations */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			relation_close(seqRel, AccessExclusiveLock);
			continue;
		}

		/* We don't need to close the sequence while we alter it. */
		ATExecChangeOwner(depForm->objid, newOwnerId, true);

		/* Now we can close it.  Keep the lock till end of transaction. */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}

/*
 * ALTER TABLE CLUSTER ON
 *
 * The only thing we have to do is to change the indisclustered bits.
 */
static void
ATExecClusterOn(Relation rel, const char *indexName)
{
	Oid			indexOid;

	indexOid = get_relname_relid(indexName, rel->rd_rel->relnamespace);

	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						indexName, RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	/* Check index is valid to cluster on */
	check_index_is_clusterable(rel, indexOid, false);

	/* And do the work */
	mark_index_clustered(rel, indexOid);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "CLUSTER ON"
				);
}

/*
 * ALTER TABLE SET WITHOUT CLUSTER
 *
 * We have to find any indexes on the table that have indisclustered bit
 * set and turn it off.
 */
static void
ATExecDropCluster(Relation rel)
{
	mark_index_clustered(rel, InvalidOid);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "SET WITHOUT CLUSTER"
				);

}

/*
 * ALTER TABLE SET TABLESPACE
 */

/* 
 * Convert tablespace name to pg_tablespace Oid.  Error out if not valid and
 * settable by the current user.
 */
static Oid
get_settable_tablespace_oid(char *tablespacename)
{
	Oid			tablespaceId;
	AclResult	aclresult;
	
	/* Check that the tablespace exists */
	tablespaceId = get_tablespace_oid(tablespacename);
	if (!OidIsValid(tablespaceId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist", tablespacename)));
	
	/* Check its permissions */
	aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_TABLESPACE, tablespacename);
	
	return tablespaceId;
}

static void
ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel, char *tablespacename)
{
	Oid			tablespaceId;

	tablespaceId = get_settable_tablespace_oid(tablespacename);

	/* Save info for Phase 3 to do the real work */
	if (OidIsValid(tab->newTableSpace))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot have multiple SET TABLESPACE subcommands"),
						   errOmitLocation(true)));
	tab->newTableSpace = tablespaceId;
}

/* 
 * ATPartsPrepSetTableSpace is like ATPrepSetTableSpace except that it generates work queue
 * entries for the command (an ALTER TABLE ... SET TABLESPACE ...)  for each part within the 
 * sub-hierarchy indicated by the oid list. 
 *
 * Designed to be called from the AT_PartAlter case of ATPrepCmd.
 */
static void
ATPartsPrepSetTableSpace(List **wqueue, Relation rel, AlterTableCmd *cmd, List *oids)
{
	ListCell *lc;
	Oid tablespaceId;
	int pass = AT_PASS_MISC;
	
	Assert( cmd && cmd->subtype == AT_SetTableSpace );
	Assert( oids );
	
	tablespaceId = get_settable_tablespace_oid(cmd->name);
	Assert(tablespaceId);
	
	ereport(DEBUG1,
			(errmsg("Expanding ALTER TABLE %s SET TABLESPACE...", 
					RelationGetRelationName(rel))
			 ));
	
	foreach(lc, oids)
	{
		Oid partrelid = lfirst_oid(lc);
		Relation partrel = relation_open(partrelid, NoLock); 
		/* NoLock because we should be holding AccessExclusiveLock on parent */
		AlterTableCmd *partcmd = copyObject(cmd);
		AlteredTableInfo *parttab = ATGetQueueEntry(wqueue, partrel);
		if( OidIsValid(parttab->newTableSpace) )
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting SET TABLESPACE subcommands for \"%s\"",
							RelationGetRelationName(partrel)),
					 errOmitLocation(true)));
		
		parttab->newTableSpace = tablespaceId;
		parttab->subcmds[pass] = lappend(parttab->subcmds[pass], partcmd);
		
		ereport(DEBUG1,
				(errmsg("Will SET TABLESPACE on \"%s\"", 
						RelationGetRelationName(partrel))));
		
		relation_close(partrel, NoLock);
	}
}

/*
 * ALTER TABLE/INDEX SET (...) or RESET (...)
 */
static void
ATExecSetRelOptions(Relation rel, List *defList, bool isReset)
{
	Oid			relid;
	Relation	pgclass;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		datum;
	bool		isnull;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_class];
	bool		repl_null[Natts_pg_class];
	bool		repl_repl[Natts_pg_class];

	if (defList == NIL)
		return;					/* nothing to do */

	pgclass = heap_open(RelationRelationId, RowExclusiveLock);

	/* Get the old reloptions */
	relid = RelationGetRelid(rel);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);

	/* MPP-5777: disallow all options except fillfactor.
	 * Future work: could convert from SET to SET WITH codepath which
	 * can support additional reloption types
	 */
	if ((defList != NIL)
/*		&& ((rel->rd_rel->relkind == RELKIND_RELATION)
		|| (rel->rd_rel->relkind == RELKIND_TOASTVALUE)) */
			)
	{
		ListCell   *cell;

		foreach(cell, defList)
		{
			DefElem    *def = lfirst(cell);
			int			kw_len = strlen(def->defname);
			char	   *text_str = "fillfactor";
			int			text_len = strlen(text_str);

			if ((text_len != kw_len) ||
				(pg_strncasecmp(text_str, def->defname, kw_len) != 0))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot SET reloption \"%s\"",
								def->defname)));

		}
	}


	/* Generate new proposed reloptions (text array) */
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 defList, false, isReset);

	/* Validate */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_AOSEGMENTS:
		case RELKIND_AOBLOCKDIR:
			if(RelationIsAoRows(rel) || RelationIsAoCols(rel))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("altering reloptions for append only tables"
								" is not permitted"),
										   errOmitLocation(true)));

			(void) heap_reloptions(rel->rd_rel->relkind, newOptions, true);
			break;
		case RELKIND_INDEX:
			(void) index_reloptions(rel->rd_am->amoptions, newOptions, true);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table, index, or TOAST table",
							RelationGetRelationName(rel)),
									   errOmitLocation(true)));
			break;
	}

	/*
	 * All we need do here is update the pg_class row; the new options will be
	 * propagated into relcaches during post-commit cache inval.
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newOptions;
	else
		repl_null[Anum_pg_class_reloptions - 1] = true;

	repl_repl[Anum_pg_class_reloptions - 1] = true;

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
								repl_val, repl_null, repl_repl);

	simple_heap_update(pgclass, &newtuple->t_self, newtuple);

	CatalogUpdateIndexes(pgclass, newtuple);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	heap_close(pgclass, RowExclusiveLock);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", isReset ? "RESET" : "SET" 
				);


}

static void 
copy_append_only_data(
	RelFileNode		*oldRelFileNode,
	
	RelFileNode		*newRelFileNode,
	
	int32			segmentFileNum,

	char			*relationName,

	int64			eof,

	ItemPointer		persistentTid,
	
	int64			persistentSerialNum,
	
	char			*buffer)
{
	MIRRORED_LOCK_DECLARE;

	char srcFileName[MAXPGPATH];
	char dstFileName[MAXPGPATH];
	char extension[12];

	File		srcFile;

	MirroredAppendOnlyOpen mirroredDstOpen;

	int64	endOffset;
	int64	readOffset;
	int32	bufferLen;
	int 	retval;

	int 		primaryError;

	bool mirrorDataLossOccurred;

	bool mirrorCatchupRequired;

	MirrorDataLossTrackingState 	originalMirrorDataLossTrackingState;
	int64 							originalMirrorDataLossTrackingSessionNum;

	if (segmentFileNum > 0)
	{
		sprintf(extension, ".%u", segmentFileNum);
	}
	else
		extension[0] = '\0';

	CopyRelPath(srcFileName, MAXPGPATH, *oldRelFileNode);
	if (segmentFileNum > 0)
	{
		strcat(srcFileName, extension);
	}

	/*
	 * Open the files
	 */
	srcFile = PathNameOpenFile(srcFileName, O_RDONLY | PG_BINARY, 0);
	if (srcFile < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", srcFileName)));


	CopyRelPath(dstFileName, MAXPGPATH, *newRelFileNode);
	if (segmentFileNum > 0)
	{
		strcat(dstFileName, extension);
	}

	MirroredAppendOnly_OpenReadWrite(
								&mirroredDstOpen,
								newRelFileNode,
								segmentFileNum,
								relationName,
								/* logicalEof */ 0, // NOTE: This is the START EOF.  Since we are copying, we start at 0.
								/* traceOpenFlags */ false,
								persistentTid,
								persistentSerialNum,
								&primaryError);
	if (primaryError != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for relation '%s': %s",
						dstFileName,
						relationName,
						strerror(primaryError))));
	
	/*
	 * Do the data copying.
	 */
	endOffset = eof;
	readOffset = 0;
	bufferLen = (Size) Min(2*BLCKSZ, endOffset);
	while (readOffset < endOffset)
	{
		CHECK_FOR_INTERRUPTS();
		
		retval = FileRead(srcFile, buffer, bufferLen);
		if (retval != bufferLen) 
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from position: " INT64_FORMAT " in file '%s' : %m",
							readOffset, 
							srcFileName)));
			
			break;
		}						
		
		MirroredAppendOnly_Append(
							  &mirroredDstOpen,
							  buffer,
							  bufferLen,
							  &primaryError,
							  &mirrorDataLossOccurred);
		if (primaryError != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %s",
							dstFileName,
							strerror(primaryError))));
		
		readOffset += bufferLen;
		
		bufferLen = (Size) Min(2*BLCKSZ, endOffset - readOffset); 					
	}
	
	/*
	 * Use the MirroredLock here to cover the flush (and close) and evaluation below whether
	 * we must catchup the mirror.
	 */
	MIRRORED_LOCK;

	MirroredAppendOnly_FlushAndClose(
							&mirroredDstOpen,
							&primaryError,
							&mirrorDataLossOccurred,
							&mirrorCatchupRequired,
							&originalMirrorDataLossTrackingState,
							&originalMirrorDataLossTrackingSessionNum);
	if (primaryError != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not flush (fsync) file \"%s\" for relation '%s': %s",
						dstFileName,
						relationName,
						strerror(primaryError))));

	FileClose(srcFile);

	if (eof > 0)
	{
		/* 
		 * This routine will handle both updating the persistent information about the
		 * new EOFs and copy data to the mirror if we are now in synchronized state.
		 */
		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(),
				 "copy_append_only_data: %u/%u/%u, segment file #%d, serial number " INT64_FORMAT ", TID %s, mirror catchup required %s, "
				 "mirror data loss tracking (state '%s', session num " INT64_FORMAT "), mirror new EOF " INT64_FORMAT,
				 newRelFileNode->spcNode,
				 newRelFileNode->dbNode,
				 newRelFileNode->relNode,
				 segmentFileNum,
				 persistentSerialNum,
				 ItemPointerToString(persistentTid),
				 (mirrorCatchupRequired ? "true" : "false"),
				 MirrorDataLossTrackingState_Name(originalMirrorDataLossTrackingState),
				 originalMirrorDataLossTrackingSessionNum,
				 eof);
		MirroredAppendOnly_AddMirrorResyncEofs(
										newRelFileNode,
										segmentFileNum,
										relationName,
										persistentTid,
										persistentSerialNum,
										&mirroredLockLocalVars,
										mirrorCatchupRequired,
										originalMirrorDataLossTrackingState,
										originalMirrorDataLossTrackingSessionNum,
										eof);

	}
	
	MIRRORED_UNLOCK;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
			 "ALTER TABLE SET TABLESPACE: Copied Append-Only segment file #%d EOF " INT64_FORMAT,
			 segmentFileNum,
			 eof);
}

static void
ATExecSetTableSpace_AppendOnly(
	Oid				tableOid,
	Relation		rel,
	Relation		gp_relation_node,
	RelFileNode		*newRelFileNode)
{
	Oid			oldTablespace;

	char *buffer;

	GpRelationNodeScan 	gpRelationNodeScan;

	HeapTuple tuple;

	int32 segmentFileNum;

	ItemPointerData oldPersistentTid;
	int64 oldPersistentSerialNum;

	ItemPointerData newPersistentTid;
	int64 newPersistentSerialNum;

	HeapTuple tupleCopy;

	AppendOnlyEntry *aoEntry;

	oldTablespace = rel->rd_rel->reltablespace ? rel->rd_rel->reltablespace : MyDatabaseTableSpace;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
			 "ALTER TABLE SET TABLESPACE: Append-Only "
			 "relation id %u, old path '%s', old relfilenode %u, old reltablespace %u, "
			 "new path '%s', new relfilenode %u, new reltablespace %u",
			 rel->rd_id,
			 relpath(rel->rd_node),
			 rel->rd_rel->relfilenode,
			 oldTablespace,
			 relpath(*newRelFileNode),
			 newRelFileNode->relNode,
			 newRelFileNode->spcNode);

	/* Use palloc to ensure we get a maxaligned buffer */		
	buffer = palloc(2*BLCKSZ);

	aoEntry = GetAppendOnlyEntry(RelationGetRelid(rel));

	/*
	 * Loop through segment files
	 *    Create segment file in new tablespace under transaction,
	 *    Copy Append-Only segment file data and fsync,
	 *    Update old gp_relation_node with new relfilenode and persistent information,
	 *    Schedule old segment file for drop under transaction,
	 */
	GpRelationNodeBeginScan(
					gp_relation_node,
					tableOid,
					rel->rd_rel->relfilenode,
					&gpRelationNodeScan);

	while ((tuple = GpRelationNodeGetNext(
							&gpRelationNodeScan,
							&segmentFileNum,
							&oldPersistentTid,
							&oldPersistentSerialNum)))
	{
		int64 eof = 0;

		/*
		 * Create segment file in new tablespace under transaction.
		 */
		MirroredFileSysObj_TransactionCreateAppendOnlyFile(
											newRelFileNode,
											segmentFileNum,
											rel->rd_rel->relname.data,
											/* doJustInTimeDirCreate */ true,
											&newPersistentTid,
											&newPersistentSerialNum);

		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "ALTER TABLE SET TABLESPACE: Create for Append-Only '%s', segment file #%d "
				 "persistent TID %s and serial number " INT64_FORMAT,
				 relpath(*newRelFileNode),
				 segmentFileNum,
				 ItemPointerToString(&newPersistentTid),
				 newPersistentSerialNum);

		/*
		 * Update gp_relation_node with new persistent information.
		 */
		tupleCopy = heap_copytuple(tuple);

		UpdateGpRelationNodeTuple(
							gp_relation_node,
							tupleCopy,
							newRelFileNode->relNode,
							segmentFileNum,
							&newPersistentTid,
							newPersistentSerialNum);

		/*
		 * Schedule old segment file for drop under transaction.
		 */
		MirroredFileSysObj_ScheduleDropAppendOnlyFile(
											&rel->rd_node,
											segmentFileNum,
											rel->rd_rel->relname.data,
											&oldPersistentTid,
											oldPersistentSerialNum);

		if (Debug_persistent_print)
			elog(Persistent_DebugPrintLevel(), 
				 "ALTER TABLE SET TABLESPACE: Drop for Append-Only '%s', segment file #%d "
				 "persistent TID %s and serial number " INT64_FORMAT,
				 relpath(rel->rd_node),
				 segmentFileNum,
				 ItemPointerToString(&oldPersistentTid),
				 oldPersistentSerialNum);

		/*
		 * Copy Append-Only segment file data and fsync.
		 */
		if (RelationIsAoRows(rel))
		{
			FileSegInfo *fileSegInfo;

			fileSegInfo = GetFileSegInfo(rel, aoEntry, ActiveSnapshot, segmentFileNum);
			if (fileSegInfo == NULL)
			{
				/*
				 * Segment file #0 is special and can exist without an entry.
				 */
				if (segmentFileNum == 0)
					eof = 0;
				else
					ereport(ERROR,
							(errmsg("Append-only row segment file #%d information missing",
									segmentFileNum)));
			}
			else
			{
				eof = fileSegInfo->eof;

				pfree(fileSegInfo);
			}
		}
		else 
		{
			int32 actualSegmentNum;
			int columnNum;

			AOCSFileSegInfo *aocsFileSegInfo;

			Assert(RelationIsAoCols(rel));

			// UNDONE: This is inefficient.

			columnNum = segmentFileNum / AOTupleId_MultiplierSegmentFileNum;

			actualSegmentNum = segmentFileNum % AOTupleId_MultiplierSegmentFileNum;
			
			aocsFileSegInfo = GetAOCSFileSegInfo(rel, aoEntry, ActiveSnapshot, actualSegmentNum);
			if (aocsFileSegInfo == NULL)
			{
				/*
				 * Segment file #0 is special and can exist without an entry.
				 */
				if (segmentFileNum == 0)
					eof = 0;
				else
					ereport(ERROR,
							(errmsg("Append-only column logical segment file #%d information for missing (physical segment #%d, column %d)",
									actualSegmentNum,
									segmentFileNum,
									columnNum)));
			}
			else
			{
				eof = aocsFileSegInfo->vpinfo.entry[columnNum].eof;
			
				pfree(aocsFileSegInfo);
			}
		}
		
		copy_append_only_data(
						&rel->rd_node,
						newRelFileNode,
						segmentFileNum,
						rel->rd_rel->relname.data,
						eof,
						&newPersistentTid,
						newPersistentSerialNum,
						buffer);
	}

	GpRelationNodeEndScan(&gpRelationNodeScan);

	pfree(aoEntry);

	pfree(buffer);
}

static void
ATExecSetTableSpace_BufferPool(
	Oid				tableOid,
	Relation		rel,
	Relation		gp_relation_node,
	RelFileNode		*newRelFileNode)
{
	Oid			oldTablespace;
	HeapTuple	nodeTuple;
	ItemPointerData newPersistentTid;
	int64 newPersistentSerialNum;
	SMgrRelation dstrel;
	bool useWal;
	
	PersistentFileSysRelStorageMgr localRelStorageMgr;
	PersistentFileSysRelBufpoolKind relBufpoolKind;
	
	oldTablespace = rel->rd_rel->reltablespace ? rel->rd_rel->reltablespace : MyDatabaseTableSpace;

	/*
	 * We need to log the copied data in WAL enabled AND
	 * it's not a temp rel.
	 */
	useWal = !XLog_CanBypassWal() && !rel->rd_istemp;

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
			 "ALTER TABLE SET TABLESPACE: Buffer Pool managed "
			 "relation id %u, old path '%s', old relfilenode %u, old reltablespace %u, "
			 "new path '%s', new relfilenode %u, new reltablespace %u, "
			 "bulk load %s",
			 rel->rd_id,
			 relpath(rel->rd_node),
			 rel->rd_rel->relfilenode,
			 oldTablespace,
			 relpath(*newRelFileNode),
			 newRelFileNode->relNode,
			 newRelFileNode->spcNode,
			 (!useWal ? "true" : "false"));
	
	/* Fetch relation's gp_relation_node row */
	nodeTuple = ScanGpRelationNodeTuple(
							gp_relation_node,
							rel->rd_rel->relfilenode,
							/* segmentFileNum */ 0);
	if (!HeapTupleIsValid(nodeTuple))
		elog(ERROR, "cache lookup failed for relation %u, tablespace %u, relation file node %u", 
		     tableOid,
		     oldTablespace,
		     rel->rd_rel->relfilenode);

	GpPersistentRelationNode_GetRelationInfo(
										rel->rd_rel->relkind,
										rel->rd_rel->relstorage,
										rel->rd_rel->relam,
										&localRelStorageMgr,
										&relBufpoolKind);
	Assert(localRelStorageMgr == PersistentFileSysRelStorageMgr_BufferPool);
	
	dstrel = smgropen(*newRelFileNode);
	MirroredFileSysObj_TransactionCreateBufferPoolFile(
											dstrel,
											relBufpoolKind,
											rel->rd_isLocalBuf,
											rel->rd_rel->relname.data,
											/* doJustInTimeDirCreate */ true,
											/* bufferPoolBulkLoad */ !useWal,
											&newPersistentTid,
											&newPersistentSerialNum);

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
		     "ALTER TABLE SET TABLESPACE: Create for Buffer Pool managed '%s' "
			 "persistent TID %s and serial number " INT64_FORMAT,
			 relpath(*newRelFileNode),
			 ItemPointerToString(&newPersistentTid),
			 newPersistentSerialNum);

	/* copy relation data to the new physical file */
	copy_buffer_pool_data(
					rel, 
					dstrel,
					&newPersistentTid,
					newPersistentSerialNum,
					useWal);

	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
			 "ALTER TABLE SET TABLESPACE: Scheduling drop for '%s' "
			 "persistent TID %s and serial number " INT64_FORMAT,
			 relpath(*newRelFileNode),
			 ItemPointerToString(&rel->rd_segfile0_relationnodeinfo.persistentTid),
			 rel->rd_segfile0_relationnodeinfo.persistentSerialNum);


	/* schedule unlinking old physical file */
	MirroredFileSysObj_ScheduleDropBufferPoolRel(rel);

	/*
	 * Now drop smgr references.  The source was already dropped by
	 * smgrscheduleunlink.
	 */
	smgrclose(dstrel);

	/* Update gp_relation_node row. */
	UpdateGpRelationNodeTuple(
						gp_relation_node,
						nodeTuple,
						newRelFileNode->relNode,
						/* segmentFileNum */ 0,
						&newPersistentTid,
						newPersistentSerialNum);

}

static bool
ATExecSetTableSpace_Relation(Oid tableOid, Oid newTableSpace, Oid newrelfilenode)
{
	Relation    rel;
	Relation	pg_class;
	Oid			oldTableSpace;
	HeapTuple	tuple;
	Form_pg_class rd_rel;
	Relation	gp_relation_node;
	RelFileNode newrnode;

	rel = relation_open(tableOid, AccessExclusiveLock);

	/*
	 * We can never allow moving of shared or nailed-in-cache relations,
	 * because we can't support changing their reltablespace values.
	 */
	if (rel->rd_rel->relisshared || rel->rd_isnailed)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move system relation \"%s\"",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	/*
	 * Don't allow moving temp tables of other backends ... their local buffer
	 * manager is not going to cope.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move temporary tables of other sessions"),
						   errOmitLocation(true)));

	/*
	 * No work if no change in tablespace.
	 */
	oldTableSpace = rel->rd_rel->reltablespace;
	if (newTableSpace == oldTableSpace ||
		(newTableSpace == MyDatabaseTableSpace && oldTableSpace == 0))
	{
		/* XXX - Why do we hold the lock if we aren't changing anything? */
		relation_close(rel, NoLock);
		return false;
	}

	/* Fetch relation's pg_class row */
	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(tableOid),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", tableOid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	gp_relation_node = heap_open(GpRelationNodeRelationId, RowExclusiveLock);

	/* create another storage file. Is it a little ugly ? */
	/* NOTE: any conflict in relfilenode value will be caught here */
	newrnode = rel->rd_node;
	newrnode.relNode = newrelfilenode;
	newrnode.spcNode = newTableSpace;

	/* update the pg_class row */
	rd_rel->reltablespace = (newTableSpace == MyDatabaseTableSpace) ? InvalidOid : newTableSpace;
	rd_rel->relfilenode = newrelfilenode;
	
	if (Debug_persistent_print)
		elog(Persistent_DebugPrintLevel(), 
			 "ALTER TABLE SET TABLESPACE: Update pg_class for relation id %u --> relfilenode to %u, reltablespace to %u",
			 tableOid,
			 rd_rel->relfilenode,
			 rd_rel->reltablespace);

	simple_heap_update(pg_class, &tuple->t_self, tuple);
	CatalogUpdateIndexes(pg_class, tuple);
	heap_freetuple(tuple);

	if (RelationIsAoRows(rel) || RelationIsAoCols(rel))
		ATExecSetTableSpace_AppendOnly(
								tableOid,
								rel, 
								gp_relation_node, 
								&newrnode);
	else
		ATExecSetTableSpace_BufferPool(
								tableOid,
								rel, 
								gp_relation_node, 
								&newrnode);


	heap_close(pg_class, RowExclusiveLock);

	heap_close(gp_relation_node, RowExclusiveLock);

	/* Make sure the reltablespace change is visible */
	CommandCounterIncrement();

	/* Hold the lock until commit */
	relation_close(rel, NoLock);

	return true;
}

/*
 * Execute ALTER TABLE SET TABLESPACE for cases where there is no tuple
 * rewriting to be done, so we just want to copy the data as fast as possible.
 */
static void
ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, TableOidInfo *oidInfo)
{
	Relation	rel;
	Oid         newrelfilenode;
	Oid			reltoastrelid = InvalidOid;
	Oid			reltoastidxid = InvalidOid;
	Oid			relaosegrelid = InvalidOid;
	Oid			relaosegidxid = InvalidOid;
	Oid			relaoblkdirrelid = InvalidOid;
	Oid			relaoblkdiridxid = InvalidOid;
	Oid			relbmrelid = InvalidOid;
	Oid			relbmidxid = InvalidOid;


	/* Ensure valid input */
	Assert(OidIsValid(tableOid));
	Assert(OidIsValid(newTableSpace));
	Assert(oidInfo);

	newrelfilenode = oidInfo->relOid;

	/*
	 * Need lock here in case we are recursing to toast table or index
	 */
	rel = relation_open(tableOid, AccessExclusiveLock);

	reltoastrelid = rel->rd_rel->reltoastrelid;

	/* Find the toast relation index */
	if (reltoastrelid)
	{
		Relation toastrel;

		toastrel = relation_open(reltoastrelid, NoLock);
		reltoastidxid = toastrel->rd_rel->reltoastidxid;
		relation_close(toastrel, NoLock);
	}

	/* Get the ao sub objects */
	if (RelationIsAoRows(rel) || RelationIsAoCols(rel))
		GetAppendOnlyEntryAuxOids(tableOid, &relaosegrelid, &relaosegidxid,
								  &relaoblkdirrelid, &relaoblkdiridxid);

	/* Get the bitmap sub objects */
	if (RelationIsBitmapIndex(rel))
		GetBitmapIndexAuxOids(rel, &relbmrelid, &relbmidxid);

	/* Move the main table */
	if (!ATExecSetTableSpace_Relation(tableOid, newTableSpace, newrelfilenode))
	{
		/* XXX - Why do we hold the lock if we aren't changing anything? */
		relation_close(rel, NoLock);
		return;
	}

	/* Move associated toast/toast_index/ao subobjects */
	if (OidIsValid(reltoastrelid))
		ATExecSetTableSpace_Relation(reltoastrelid, newTableSpace, 
									 oidInfo->toastOid);
	if (OidIsValid(reltoastidxid))
		ATExecSetTableSpace_Relation(reltoastidxid, newTableSpace,
									 oidInfo->toastIndexOid);
	if (OidIsValid(relaosegrelid))
		ATExecSetTableSpace_Relation(relaosegrelid, newTableSpace,
									 oidInfo->aosegOid);
	if (OidIsValid(relaosegidxid))
		ATExecSetTableSpace_Relation(relaosegidxid, newTableSpace,
									 oidInfo->aosegIndexOid);
	if (OidIsValid(relaoblkdirrelid))
		ATExecSetTableSpace_Relation(relaoblkdirrelid, newTableSpace,
									 oidInfo->aoblkdirOid);
	if (OidIsValid(relaoblkdiridxid))
		ATExecSetTableSpace_Relation(relaoblkdiridxid, newTableSpace,
									 oidInfo->aoblkdirIndexOid);

	/* 
	 * MPP-7996 - bitmap index subobjects w/Alter Table Set tablespace
	 *
	 * Minor hack: Bitmap indexes are never AO tables.  Rather than extending
	 * OidInfo with yet more oids we simply overload aosegOid/aosegIndexOid
	 * to serve double purpose as oids for the bitmap subobjects.
	 */
	if (OidIsValid(relbmrelid))
	{
		Assert(!relaosegrelid);
		ATExecSetTableSpace_Relation(relbmrelid, newTableSpace,
									 oidInfo->aosegOid);
	}
	if (OidIsValid(relbmidxid))
	{
		Assert(!relaosegidxid);
		ATExecSetTableSpace_Relation(relbmidxid, newTableSpace,
									 oidInfo->aosegIndexOid);
	}

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH) && MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "SET TABLESPACE"
			);

	/* Hold the lock until commit */
	relation_close(rel, NoLock);
}

/*
 * Copy data, block by block
 */
static void
copy_buffer_pool_data(
	Relation 	rel,

	SMgrRelation dst,

	ItemPointer persistentTid,

	int64 		persistentSerialNum,

	bool		useWal)
{
	SMgrRelation src;
	BlockNumber nblocks;
	BlockNumber blkno;
	char		buf[BLCKSZ];
	Page		page = (Page) buf;

	MirroredBufferPoolBulkLoadInfo bulkLoadInfo;

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	nblocks = RelationGetNumberOfBlocks(rel);

	/* RelationGetNumberOfBlocks will certainly have opened rd_smgr */
	src = rel->rd_smgr;

	if (!useWal)
	{
		MirroredBufferPool_BeginBulkLoad(
								&rel->rd_node,
								persistentTid,
								persistentSerialNum,
								&bulkLoadInfo);
	}
	else
	{
		if (Debug_persistent_print)
		{
			elog(Persistent_DebugPrintLevel(),
				 "copy_buffer_pool_data %u/%u/%u: not bypassing the WAL -- not using bulk load, persistent serial num " INT64_FORMAT ", TID %s",
				 rel->rd_node.spcNode,
				 rel->rd_node.dbNode,
				 rel->rd_node.relNode,
				 persistentSerialNum,
				 ItemPointerToString(persistentTid));
		}
		MemSet(&bulkLoadInfo, 0, sizeof(MirroredBufferPoolBulkLoadInfo));
	}
	
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		smgrread(src, blkno, buf);

		/* XLOG stuff */
		if (useWal)
		{
			xl_heap_newpage xlrec;
			XLogRecPtr	recptr;
			XLogRecData rdata[2];

			/* NO ELOG(ERROR) from here till newpage op is logged */
			START_CRIT_SECTION();

			xlrec.heapnode.node = dst->smgr_rnode;
			xlrec.heapnode.persistentTid = *persistentTid;
			xlrec.heapnode.persistentSerialNum = persistentSerialNum;
			xlrec.blkno = blkno;

			rdata[0].data = (char *) &xlrec;
			rdata[0].len = SizeOfHeapNewpage;
			rdata[0].buffer = InvalidBuffer;
			rdata[0].next = &(rdata[1]);

			rdata[1].data = (char *) page;
			rdata[1].len = BLCKSZ;
			rdata[1].buffer = InvalidBuffer;
			rdata[1].next = NULL;

			recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_NEWPAGE, rdata);

			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);

			END_CRIT_SECTION();
		}

		
		// -------- MirroredLock ----------
		LWLockAcquire(MirroredLock, LW_SHARED);
		
		/*
		 * Now write the page.	We say isTemp = true even if it's not a temp
		 * rel, because there's no need for smgr to schedule an fsync for this
		 * write; we'll do it ourselves below.
		 */
		smgrwrite(dst, blkno, buf, true);
		
		LWLockRelease(MirroredLock);
		// -------- MirroredLock ----------
	}

	/*
	 * If the rel isn't temp, we must fsync it down to disk before it's safe
	 * to commit the transaction.  (For a temp rel we don't care since the rel
	 * will be uninteresting after a crash anyway.)
	 *
	 * It's obvious that we must do this when not WAL-logging the copy. It's
	 * less obvious that we have to do it even if we did WAL-log the copied
	 * pages. The reason is that since we're copying outside shared buffers, a
	 * CHECKPOINT occurring during the copy has no way to flush the previously
	 * written data to disk (indeed it won't know the new rel even exists).  A
	 * crash later on would replay WAL from the checkpoint, therefore it
	 * wouldn't replay our earlier WAL entries. If we do not fsync those pages
	 * here, they might still not be on disk when the crash occurs.
	 */
	
	// -------- MirroredLock ----------
	LWLockAcquire(MirroredLock, LW_SHARED);
	
	if (!rel->rd_istemp)
		smgrimmedsync(dst);
	
	LWLockRelease(MirroredLock);
	// -------- MirroredLock ----------

	if (!useWal)
	{
		bool mirrorDataLossOccurred;
	
		/*
		 * We may have to catch-up the mirror since bulk loading of data is
		 * ignored by resynchronize.
		 */
		while (true)
		{
			bool bulkLoadFinished;
	
			bulkLoadFinished = 
				MirroredBufferPool_EvaluateBulkLoadFinish(
												&bulkLoadInfo);
	
			if (bulkLoadFinished)
			{
				/*
				 * The flush was successful to the mirror (or the mirror is
				 * not configured).
				 *
				 * We have done a state-change from 'Bulk Load Create Pending'
				 * to 'Create Pending'.
				 */
				break;
			}
	
			/*
			 * Copy primary data to mirror and flush.
			 */
			MirroredBufferPool_CopyToMirror(
									&rel->rd_node,
									rel->rd_rel->relname.data,
									persistentTid,
									persistentSerialNum,
									bulkLoadInfo.mirrorDataLossTrackingState,
									bulkLoadInfo.mirrorDataLossTrackingSessionNum,
									nblocks,
									&mirrorDataLossOccurred);
		}
	}
	else
	{
		if (Debug_persistent_print)
		{
			elog(Persistent_DebugPrintLevel(),
				 "copy_buffer_pool_data %u/%u/%u: did not bypass the WAL -- did not use bulk load, persistent serial num " INT64_FORMAT ", TID %s",
				 rel->rd_node.spcNode,
				 rel->rd_node.dbNode,
				 rel->rd_node.relNode,
				 persistentSerialNum,
				 ItemPointerToString(persistentTid));
		}
	}
}

/*
 * ALTER TABLE ENABLE/DISABLE TRIGGER
 *
 * We just pass this off to trigger.c.
 */
static void
ATExecEnableDisableTrigger(Relation rel, char *trigname,
						   bool enable, bool skip_system)
{
	EnableDisableTrigger(rel, trigname, enable, skip_system);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", 
						   enable ? "ENABLE TRIGGER" : 
						   "DISABLE TRIGGER"
				);

}

static void
inherit_parent(Relation parent_rel, Relation child_rel, bool is_partition, List *inhAttrNameList)
{
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	int32		inhseqno;
	List	   *children;
	Relation	catalogRelation;

	/*
	 * Check for duplicates in the list of parents, and determine the highest
	 * inhseqno already present; we'll use the next one for the new parent.
	 * (Note: get RowExclusiveLock because we will write pg_inherits below.)
	 *
	 * Note: we do not reject the case where the child already inherits from
	 * the parent indirectly; CREATE TABLE doesn't reject comparable cases.
	 */
	catalogRelation = heap_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, SnapshotNow, 1, &key);

	/* inhseqno sequences start at 1 */
	inhseqno = 0;
	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inheritsTuple);

		if (inh->inhparent == RelationGetRelid(parent_rel))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("inherited relation \"%s\" duplicated",
							RelationGetRelationName(parent_rel)),
									   errOmitLocation(true)));
		if (inh->inhseqno > inhseqno)
			inhseqno = inh->inhseqno;
	}
	systable_endscan(scan);

	/*
	 * Prevent circularity by seeing if proposed parent inherits from child.
	 * (In particular, this disallows making a rel inherit from itself.)
	 *
	 * This is not completely bulletproof because of race conditions: in
	 * multi-level inheritance trees, someone else could concurrently
	 * be making another inheritance link that closes the loop but does
	 * not join either of the rels we have locked.  Preventing that seems
	 * to require exclusive locks on the entire inheritance tree, which is
	 * a cure worse than the disease.  find_all_inheritors() will cope with
	 * circularity anyway, so don't sweat it too much.
	 */
	children = find_all_inheritors(RelationGetRelid(child_rel));

	if (list_member_oid(children, RelationGetRelid(parent_rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   RelationGetRelationName(parent_rel),
						   RelationGetRelationName(child_rel)),
								   errOmitLocation(true)));

	/* If parent has OIDs then child must have OIDs */
	if (parent_rel->rd_rel->relhasoids && !child_rel->rd_rel->relhasoids)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("table \"%s\" without OIDs cannot inherit from table \"%s\" with OIDs",
						RelationGetRelationName(child_rel),
						RelationGetRelationName(parent_rel)),
								   errOmitLocation(true)));

	/* Match up the columns and bump attinhcount and attislocal */
	MergeAttributesIntoExisting(child_rel, parent_rel, inhAttrNameList, is_partition);

	/* Match up the constraints and make sure they're present in child */
	MergeConstraintsIntoExisting(child_rel, parent_rel);

	/*
	 * OK, it looks valid.  Make the catalog entries that show inheritance.
	 */
	StoreCatalogInheritance1(RelationGetRelid(child_rel),
							 RelationGetRelid(parent_rel),
							 inhseqno + 1,
							 catalogRelation, is_partition);

	/* Now we're done with pg_inherits */
	heap_close(catalogRelation, RowExclusiveLock);

}

/*
 * ALTER TABLE INHERIT
 *
 * Add a parent to the child's parents. This verifies that all the columns and
 * check constraints of the parent appear in the child and that they have the
 * same data types and expressions.
 */
static void
ATExecAddInherit(Relation child_rel, Node *node)
{
	Relation	parent_rel;
	bool		is_partition;
	RangeVar   *parent;
	List	   *inhAttrNameList = NIL;

	Assert(PointerIsValid(node));

	if (IsA(node, InheritPartitionCmd))
	{
		parent = ((InheritPartitionCmd *)node)->parent;
		is_partition = true;
	}
	else
	{
		List *inhParms;
		Assert(IsA(node, List));
		inhParms = (List *)node;

		Insist(list_length(inhParms) == 2);
		Assert(IsA(linitial(inhParms), RangeVar));
		Assert(lsecond(inhParms) == NIL || IsA(lsecond(inhParms), List));

		parent = (RangeVar *)linitial(inhParms);
		inhAttrNameList = (List *)lsecond(inhParms);
		is_partition = false;
	}

	/*
	 * AccessShareLock on the parent is what's obtained during normal CREATE
	 * TABLE ... INHERITS ..., so should be enough here.
	 */
	parent_rel = heap_openrv(parent, AccessShareLock);

	/*
	 * Must be owner of both parent and child -- child was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(parent_rel, false);

	/* Permanent rels cannot inherit from temporary ones */
	if (!isTempNamespace(RelationGetNamespace(child_rel)) &&
		isTempNamespace(RelationGetNamespace(parent_rel)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation \"%s\"",
						RelationGetRelationName(parent_rel)),
								   errOmitLocation(true)));

	if (is_partition)
	{
		/* lookup all attrs */
		int attno;

		for (attno = 0; attno < parent_rel->rd_att->natts; attno++)
		{
			Form_pg_attribute	 attribute = parent_rel->rd_att->attrs[attno];
			char				*attributeName = NameStr(attribute->attname);

			/* MPP-5397: ignore dropped cols */
			if (!attribute->attisdropped)
				inhAttrNameList =
						lappend(inhAttrNameList,
								makeString(attributeName));
		}

	}
	inherit_parent(parent_rel, child_rel, is_partition, inhAttrNameList);

	/*
	 * Keep our lock on the parent relation until commit, unless we're
	 * doing partitioning, in which case the parent is sufficiently locked.
	 * We want to unlock here in case we're doing deep sub partitioning. We do
	 * not want to acquire too many locks since we're overflow the lock buffer.
	 * An exclusive lock on the parent table is sufficient to guard against
	 * concurrency issues.
	 */
	if (is_partition)
		heap_close(parent_rel, AccessShareLock);
	else
		heap_close(parent_rel, NoLock);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(child_rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(child_rel),
						   GetUserId(),
						   "ALTER", "INHERIT"
				);
}

/*
 * Obtain the source-text form of the constraint expression for a check
 * constraint, given its pg_constraint tuple
 */
static char *
decompile_conbin(HeapTuple contup, TupleDesc tupdesc)
{
	Form_pg_constraint con;
	bool		isnull;
	Datum		attr;
	Datum		expr;

	con = (Form_pg_constraint) GETSTRUCT(contup);
	attr = heap_getattr(contup, Anum_pg_constraint_conbin, tupdesc, &isnull);
	if (isnull)
		elog(ERROR, "null conbin for constraint %u", HeapTupleGetOid(contup));

	expr = DirectFunctionCall2(pg_get_expr, attr,
							   ObjectIdGetDatum(con->conrelid));
	return DatumGetCString(DirectFunctionCall1(textout, expr));
}
/*
	toast_idxid = index_create(toast_relid, toast_idxname, newIndexOid,
							   indexInfo,
							   BTREE_AM_OID,
							   rel->rd_rel->reltablespace,
							   classObjectId,
							   true, false, true, false);*/

/*
 * Check columns in child table match up with columns in parent, and increment
 * their attinhcount.
 *
 * Called by ATExecAddInherit
 *
 * Currently all parent columns must be found in child. Missing columns are an
 * error.  One day we might consider creating new columns like CREATE TABLE
 * does.  However, that is widely unpopular --- in the common use case of
 * partitioned tables it's a foot-gun.
 *
 * The data type must match exactly. If the parent column is NOT NULL then
 * the child must be as well. Defaults are not compared, however.
 */
static void
MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel, List *inhAttrNameList,
							bool is_partition)
{
	Relation	attrrel;
	AttrNumber	parent_attno;
	int			parent_natts;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	HeapTuple	tuple;
	ListCell	*attNameCell;

	attrrel = heap_open(AttributeRelationId, RowExclusiveLock);

	tupleDesc = RelationGetDescr(parent_rel);
	parent_natts = tupleDesc->natts;
	constr = tupleDesc->constr;

	/*
	 * If we have an inherited column list, ensure all named columns exist in parent
	 * and that the list excludes system columns.
	 */
	foreach( attNameCell, inhAttrNameList )
	{
		bool columnDefined = false;
		char *inhAttrName = strVal((Value *)lfirst(attNameCell));

		for (parent_attno = 1; parent_attno <= parent_natts && !columnDefined; parent_attno++)
		{
			char *attributeName;
			Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
			if (attribute->attisdropped)
				continue;
			attributeName = NameStr(attribute->attname);
			columnDefined = (strcmp(inhAttrName, attributeName) == 0);
		}

		if (!columnDefined)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" does not exist in parent table \"%s\"",
							inhAttrName,
							RelationGetRelationName(parent_rel)),
									   errOmitLocation(true)));
	}

	for (parent_attno = 1; parent_attno <= parent_natts; parent_attno++)
	{
		Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore dropped columns in the parent. */
		if (attribute->attisdropped)
			continue;

		/* Find same column in child (matching on column name). */
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(child_rel),
										  attributeName);
		if (HeapTupleIsValid(tuple))
		{
			/* Check they are same type and typmod */
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (attribute->atttypid != childatt->atttypid ||
				attribute->atttypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName),
										   errOmitLocation(true)));

			if (attribute->attnotnull && !childatt->attnotnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
					  errmsg("column \"%s\" in child table must be marked NOT NULL",
							 attributeName),
									   errOmitLocation(true)));

			/*
			 * OK, bump the child column's inheritance count.  (If we fail
			 * later on, this change will just roll back.)
			 */
			childatt->attinhcount++;

			/*
			 * For locally-defined attributes, check to see if the attribute is named
			 * in the "fully-inherited" list.  If so, mark the child attribute as
			 * not locally defined.  (Default/standard behaviour is to leave the
			 * attribute locally defined.)
			 */
			if (childatt->attislocal)
			{
				/* never local when we're doing partitioning */
				if (is_partition)
					childatt->attislocal = false;
				else
				{
					foreach( attNameCell, inhAttrNameList )
					{
						char *inhAttrName = strVal((Value *)lfirst(attNameCell));
						if (strcmp(attributeName, inhAttrName) == 0)
						{
							childatt->attislocal = false;
							break;
						}
					}
				}
			}

			simple_heap_update(attrrel, &tuple->t_self, tuple);
			CatalogUpdateIndexes(attrrel, tuple);
			heap_freetuple(tuple);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing column \"%s\"",
							attributeName),
									   errOmitLocation(true)));
		}
	}

	heap_close(attrrel, RowExclusiveLock);
}

/*
 * Check constraints in child table match up with constraints in parent
 *
 * Called by ATExecAddInherit
 *
 * Currently all constraints in parent must be present in the child. One day we
 * may consider adding new constraints like CREATE TABLE does. We may also want
 * to allow an optional flag on parent table constraints indicating they are
 * intended to ONLY apply to the master table, not to the children. That would
 * make it possible to ensure no records are mistakenly inserted into the
 * master in partitioned tables rather than the appropriate child.
 *
 * XXX This is O(N^2) which may be an issue with tables with hundreds of
 * constraints. As long as tables have more like 10 constraints it shouldn't be
 * a problem though. Even 100 constraints ought not be the end of the world.
 */
static void
MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	TupleDesc	tupleDesc;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	constraintTuple;
	ListCell   *elem;
	List	   *constraints;

	/* First gather up the child's constraint definitions */
	catalogRelation = heap_open(ConstraintRelationId, AccessShareLock);
	tupleDesc = RelationGetDescr(catalogRelation);

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId,
							  true, SnapshotNow, 1, &key);

	constraints = NIL;
	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);

		if (con->contype != CONSTRAINT_CHECK)
			continue;

		constraints = lappend(constraints, heap_copytuple(constraintTuple));
	}

	systable_endscan(scan);

	/* Then scan through the parent's constraints looking for matches */
	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId, true,
							  SnapshotNow, 1, &key);

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint parent_con = (Form_pg_constraint) GETSTRUCT(constraintTuple);
		bool		found = false;
		Form_pg_constraint child_con = NULL;
		HeapTuple	child_contuple = NULL;

		if (parent_con->contype != CONSTRAINT_CHECK)
			continue;

		foreach(elem, constraints)
		{
			child_contuple = (HeapTuple) lfirst(elem);
			child_con = (Form_pg_constraint) GETSTRUCT(child_contuple);
			if (strcmp(NameStr(parent_con->conname),
					   NameStr(child_con->conname)) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing constraint \"%s\"",
							NameStr(parent_con->conname)),
									   errOmitLocation(true)));

		if (parent_con->condeferrable != child_con->condeferrable ||
			parent_con->condeferred != child_con->condeferred ||
			strcmp(decompile_conbin(constraintTuple, tupleDesc),
				   decompile_conbin(child_contuple, tupleDesc)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("constraint definition for check constraint \"%s\" does not match",
							NameStr(parent_con->conname)),
									   errOmitLocation(true)));

		/*
		 * TODO: add conislocal,coninhcount to constraints. This is where we
		 * would have to bump them just like attributes
		 */
	}

	systable_endscan(scan);
	heap_close(catalogRelation, AccessShareLock);
}

/*
 * ALTER TABLE NO INHERIT
 *
 * Drop a parent from the child's parents. This just adjusts the attinhcount
 * and attislocal of the columns and removes the pg_inherit and pg_depend
 * entries.
 *
 * If attinhcount goes to 0 then attislocal gets set to true. If it goes back
 * up attislocal stays true, which means if a child is ever removed from a
 * parent then its columns will never be automatically dropped which may
 * surprise. But at least we'll never surprise by dropping columns someone
 * isn't expecting to be dropped which would actually mean data loss.
 */
static void
ATExecDropInherit(Relation rel, RangeVar *parent, bool is_partition)
{
	Relation	parent_rel;
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	inheritsTuple,
				attributeTuple,
				depTuple;
	bool		found = false;

	/*
	 * AccessShareLock on the parent is probably enough, seeing that DROP TABLE
	 * doesn't lock parent tables at all.  We need some lock since we'll be
	 * inspecting the parent's schema.
	 */
	parent_rel = heap_openrv(parent, AccessShareLock);

	/*
	 * We don't bother to check ownership of the parent table --- ownership
	 * of the child is presumed enough rights.
	 */

	/*
	 * Find and destroy the pg_inherits entry linking the two, or error out
	 * if there is none.
	 */
	catalogRelation = heap_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, SnapshotNow, 1, key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Oid			inhparent;

		inhparent = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhparent;
		if (inhparent == RelationGetRelid(parent_rel))
		{
			simple_heap_delete(catalogRelation, &inheritsTuple->t_self);
			found = true;
			break;
		}
	}

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" is not a parent of relation \"%s\"",
						RelationGetRelationName(parent_rel),
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	/*
	 * Search through child columns looking for ones matching parent rel
	 */
	catalogRelation = heap_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(catalogRelation, AttributeRelidNumIndexId,
							  true, SnapshotNow, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);

		/* Ignore if dropped or not inherited */
		if (att->attisdropped)
			continue;
		if (att->attinhcount <= 0)
			continue;

		if (SearchSysCacheExistsAttName(RelationGetRelid(parent_rel),
										NameStr(att->attname)))
		{
			/* Decrement inhcount and possibly set islocal to true */
			HeapTuple	copyTuple = heap_copytuple(attributeTuple);
			Form_pg_attribute copy_att = (Form_pg_attribute) GETSTRUCT(copyTuple);

			copy_att->attinhcount--;
			if (copy_att->attinhcount == 0)
				copy_att->attislocal = true;

			simple_heap_update(catalogRelation, &copyTuple->t_self, copyTuple);
			CatalogUpdateIndexes(catalogRelation, copyTuple);
			heap_freetuple(copyTuple);
		}
	}
	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);

	/*
	 * Drop the dependency
	 *
	 * There's no convenient way to do this, so go trawling through pg_depend
	 */
	catalogRelation = heap_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(catalogRelation, DependDependerIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(depTuple);

		if (dep->refclassid == RelationRelationId &&
			dep->refobjid == RelationGetRelid(parent_rel) &&
			dep->refobjsubid == 0 &&
			((dep->deptype == DEPENDENCY_NORMAL && !is_partition) ||
			 (dep->deptype == DEPENDENCY_AUTO && is_partition)))
			simple_heap_delete(catalogRelation, &depTuple->t_self);
	}

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);

	/* keep our lock on the parent relation until commit */
	heap_close(parent_rel, NoLock);

	/* MPP-6929: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "NO INHERIT"
				);


}

static StdRdOptions *
relopts_for_ctas(Relation rel)
{
	/*
	 * AO table so go get the options out of pg_appendonly. The
	 * class reloptions isn't to be trusted.
	 */
	StdRdOptions *out = palloc0(sizeof(StdRdOptions));
	if (RelationIsAoRows(rel) || RelationIsAoCols(rel))
	{
		AppendOnlyEntry *opts = GetAppendOnlyEntry(RelationGetRelid(rel));

		out->appendonly = true;
		out->blocksize = opts->blocksize;
		out->compresslevel = opts->compresslevel;
		out->checksum = opts->checksum;
		out->compresstype = opts->compresstype ? pstrdup(opts->compresstype) : NULL;
        out->columnstore = opts->columnstore;
	}
	else
	{
		if (rel->rd_options)
			memcpy(out, rel->rd_options, sizeof(StdRdOptions));
	}
	return out;
}

static List *
stdoptions_to_list(Relation rel, StdRdOptions *opts)
{
	List *wo = NIL;

	/* WITH options */
	if (rel->rd_rel->relhasoids)
		wo = lappend(wo, defWithOids(true));
	if (opts)
	{
		if (!opts->appendonly)
		{
			if (opts->fillfactor)
					wo = lappend(wo, makeDefElem("fillfactor",
												 (Node *)makeInteger(opts->fillfactor)));

		}
		else
		{
			wo = lappend(wo, makeDefElem("appendonly",
										 (Node *)makeString("true")));
			if (opts->blocksize)
				wo = lappend(wo, makeDefElem("blocksize",
											 (Node *)makeInteger(opts->blocksize)));
			if (opts->compresslevel)
				wo = lappend(wo, makeDefElem("compresslevel",
											 (Node *)makeInteger(opts->compresslevel)));
			if (opts->compresstype)
				wo = lappend(wo, makeDefElem("compresstype",
											 (Node *)makeString(pstrdup(opts->compresstype))));

			if (opts->checksum)
				wo = lappend(wo, makeDefElem("checksum",
											 (Node *)makeString("true")));

            if (opts->columnstore)
                wo = lappend(wo, makeDefElem("orientation", (Node *) makeString("column")));
            else
                wo = lappend(wo, makeDefElem("orientation", (Node *) makeString("row")));
		}
	}
	return wo;
}

/*
 * Build:
 *
 * CREATE TABLE pg_temp_<NNNN> AS SELECT * FROM rel
 *   DISTRIBUTED BY dist_clause
 */
static QueryDesc *
build_ctas_with_dist(Relation rel, List *dist_clause,
					 StdRdOptions *newopts, RangeVar **tmprv, List **hidden_types)
{
	Query *q;
	SelectStmt *s = makeNode(SelectStmt);
	Node *n;
	List	*parsetrees;
	RangeVar *from_tbl;
	List *rewritten;
	PlannedStmt *stmt;
	DestReceiver *dest;
	QueryDesc *queryDesc;
	List *wo = NIL;
	StdRdOptions *opts = newopts ? newopts :
			(StdRdOptions *)relopts_for_ctas(rel);
	RangeVar *tmprel = make_temp_table_name(rel, MyBackendId);
	bool pre_built;

	{
		ResTarget *t = makeNode(ResTarget);
	    ColumnRef *c = makeNode(ColumnRef);
        c->fields = list_make1(makeString("*"));
        c->location = -1;
        t->val = (Node *)c;
        t->location = -1;
		s->targetList = list_make1(t);
	}

	from_tbl = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
							pstrdup(RelationGetRelationName(rel)), -1);
	from_tbl->inhOpt = INH_NO; /* MPP-5300: turn off inheritance -
								* Otherwise, the data from the child
								* tables is added to the parent!
								*/
	s->fromClause = list_make1(from_tbl);
	wo = stdoptions_to_list(rel, opts);

	pre_built = prebuild_temp_table(rel, tmprel, dist_clause,
									wo, hidden_types, opts->appendonly);
	if (pre_built)
	{
		InsertStmt *i = makeNode(InsertStmt);

		i->relation = tmprel;
		i->selectStmt = (Node *)s;
		n = (Node *)i;
	}
	else
	{
		Oid tblspc = rel->rd_rel->reltablespace;

		s->intoClause = makeNode(IntoClause);
		s->intoClause->rel = tmprel;
		s->intoClause->options = wo;
		s->intoClause->tableSpaceName = get_tablespace_name(tblspc);
		s->distributedBy = dist_clause;
		n = (Node *)s;
	}
	*tmprv = tmprel;

	parsetrees = parse_analyze((Node *)n, NULL, NULL, 0);
	Assert(list_length(parsetrees) == 1);
	q = (Query *)linitial(parsetrees);

	AcquireRewriteLocks(q);

	/* Rewrite through rule system */
	rewritten = QueryRewrite(q);

	/* We don't expect more or less than one result query */
	Assert(list_length(rewritten) == 1);

	q = (Query *) linitial(rewritten);
	Assert(q->commandType == CMD_SELECT || q->commandType == CMD_INSERT);

	/* plan the query */
	stmt = planner(q, 0, NULL);

	/*
	 * Update snapshot command ID to ensure this query sees results of any
	 * previously executed queries.
	 */
	//ActiveSnapshot->curcid = GetCurrentCommandId();

	/* Create dest receiver for COPY OUT */
	dest = CreateDestReceiver(DestIntoRel, NULL);

	/* Create a QueryDesc requesting no output */
	queryDesc = CreateQueryDesc(stmt,  pstrdup("(internal SELECT INTO query)"),
								ActiveSnapshot, InvalidSnapshot,
								dest, NULL, false);
	return queryDesc;
}

static Datum
new_rel_opts(Relation rel, List *lwith)
{
	Datum newOptions = PointerGetDatum(NULL);

	if (lwith && list_length(lwith))
	{
		Relation pg_class = heap_open(RelationRelationId, RowExclusiveLock);
		bool make_heap = false;
		ListCell *lc;
		bool new_opts = false;

		/*
		 * See if user has specified appendonly = false. If so, we
		 * have no use for the existing reloptions
		 */
		foreach(lc, lwith)
		{
			DefElem *e = lfirst(lc);

			if (pg_strcasecmp(e->defname, "appendonly") == 0 &&
				pg_strcasecmp(defGetString(e), "false") == 0)
			{
				make_heap = true;
				break;
			}
		}

		if (!make_heap)
		{
			/* Get the old reloptions */
			bool isnull;
			Oid relid = RelationGetRelid(rel);
			HeapTuple optsTuple = SearchSysCache(RELOID,
									   ObjectIdGetDatum(relid),
									   0, 0, 0);
			if (!HeapTupleIsValid(optsTuple))
					elog(ERROR, "cache lookup failed for relation %u", relid);

			newOptions = SysCacheGetAttr(RELOID, optsTuple,
										 Anum_pg_class_reloptions, &isnull);
			new_opts = true && !isnull;
			ReleaseSysCache(optsTuple);
		}

		/* Generate new proposed reloptions (text array) */
		newOptions = transformRelOptions(new_opts ? newOptions : (Datum) 0,
										 lwith, false, false);

		heap_close(pg_class, RowExclusiveLock);
	}
	else
	{
		/* just do the current options */

	}
	return newOptions;
}

static RangeVar *
make_temp_table_name(Relation rel, BackendId id)
{
	char *nspname,
		 tmpname[NAMEDATALEN];
	/* temporary enough */
	snprintf(tmpname, NAMEDATALEN, "pg_temp_%u_%i", RelationGetRelid(rel), id);
	nspname = get_namespace_name(RelationGetNamespace(rel));

	return makeRangeVar(nspname, pstrdup(tmpname), -1);
}

/* Fill-in the names field of the given TypeName node with the name
 * of an existing type that matches the given internal length, and alignment.
 *
 * If none can be found, return NULL, else return the given TypeName
 * pointer.
 *
 * It may seem as if we should include by-value in the test, but built-in
 * types cover the allowable internal lengths for these and we don't want
 * to second guess.
 *
 * Specifically for use by make_typename.
 */
static TypeName *
pick_name_of_similar_type(TypeName *tname, int2 typlen, char typalign)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;

	catalogRelation = heap_open(TypeRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_type_typlen,
				InvalidStrategy, F_INT2EQ,
				Int16GetDatum(typlen));

	ScanKeyInit(&key[1],
				Anum_pg_type_typalign,
				InvalidStrategy, F_CHAREQ,
				CharGetDatum(typalign));

	scan = systable_beginscan(catalogRelation, InvalidOid, false,
							  SnapshotNow, 2, &key[0]);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_type typtuple = (Form_pg_type)GETSTRUCT(tuple);
		tname->names =
			list_make2(makeString(get_namespace_name(typtuple->typnamespace)),
					   makeString(pstrdup(NameStr(typtuple->typname))));
	}
	else
		tname = NULL;

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);
	return tname;
}

/*
 * Build the basic CreateFunctionStmt statement for dispatch as part of
 * build_hidden_type().
 */
static CreateFunctionStmt *
build_iofunc(char *newname, char *iotype, char *baseio, char *returns)
{
	CreateFunctionStmt *iofunc;
	char ioname[NAMEDATALEN];
	FunctionParameter *param;
	List *args;

	if (strcmp("in", iotype) == 0)
	   args = list_make2(makeString("pg_catalog"), makeString("cstring"));
	else
		args = list_make1(makeString(newname));

	/*
	 * CREATE FUNCTION F(cstring) RETURNS shelltype AS 'int4(cstring)' language
	 * internal;
	 */
	iofunc = makeNode(CreateFunctionStmt);
	snprintf(ioname, NAMEDATALEN, "%s_%s", newname, iotype);
	iofunc->funcname = list_make1(makeString(pstrdup(ioname)));
	param = makeNode(FunctionParameter);
	param->argType = makeTypeNameFromNameList(args);
	param->mode = FUNC_PARAM_IN;
	iofunc->parameters = list_make1(param);
	iofunc->returnType = makeTypeName(returns);
	iofunc->options = list_make2(makeDefElem("as", (Node *)list_make1(makeString(baseio))),
								 makeDefElem("language", (Node *)makeString("internal")));

	return iofunc;
}

/*
 * Build a temporary hidden type for dropped columns for which there is no
 * suitable pg_type entry. We find these "ghost types" when a user has created a
 * type, dropped a column using it and THEN dropped the type. There is a kind of
 * "homeopathic echo" here, where we have the structural details of the type in
 * pg_attribute but no way of pin pointing the type configuration by name.
 *
 * Procedure:
 * a) Build a shell type
 * b) Create IO functions
 * c) Fill out the shell type with IO functions and correct length and alignment
 * data
 *
 * The IO functions do not have to do anything because all values in the column
 * will be NULL, the functions will never be called.
 */
static TypeName *
build_hidden_type(Form_pg_attribute att)
{
	DefineStmt *newtype;
	CreateFunctionStmt *iofunc;
	Value *inputname;
	Value *outputname;
	List *parsetrees;
	DestReceiver *dest = None_Receiver;
	Query *q;
	char *alignname = NULL;
	TypeName *typname;
	char newname[NAMEDATALEN];

	Assert(Gp_role == GP_ROLE_DISPATCH);

	snprintf(newname, NAMEDATALEN, "pg_atsdb_%u_%i_%u", att->attrelid,
			 att->attnum, MyBackendId);

	/* shell type */
	newtype = makeNode(DefineStmt);
	newtype->kind = OBJECT_TYPE;
	newtype->defnames = list_make1(makeString(newname));

	/* that's all that's needed for the shell! */
	parsetrees = parse_analyze((Node *)newtype, NULL, NULL, 0);
	Assert(list_length(parsetrees) == 1);
	q = (Query *)linitial(parsetrees);
	ProcessUtility((Node *)q->utilityStmt, 
				   synthetic_sql,
				   NULL, 
				   false, /* not top level */
				   dest, 
				   NULL);
	CommandCounterIncrement();

	/*
	 * Now for the input function.
	 */
	iofunc = build_iofunc(newname, "in", "int4in", newname);
	inputname = copyObject(linitial(iofunc->funcname));
	parsetrees = parse_analyze((Node *)iofunc, NULL, NULL, 0);
	q = (Query *)linitial(parsetrees);
	ProcessUtility((Node *)q->utilityStmt, 
				   synthetic_sql,
				   NULL, 
				   false, /* not top level */
				   dest, 
				   NULL);
	CommandCounterIncrement();

	/* and then output */
	/* output function accepts shellname as its argument */
	iofunc = build_iofunc(newname, "out", "int4out", "cstring");
	outputname = copyObject(linitial(iofunc->funcname));
	parsetrees = parse_analyze((Node *)iofunc, NULL, NULL, 0);
	q = (Query *)linitial(parsetrees);
	ProcessUtility((Node *)q->utilityStmt,
				   synthetic_sql,
				   NULL, 
				   false, /* not top level */
				   dest, 
				   NULL);
	CommandCounterIncrement();

	/* now build full type */
	newtype = makeNode(DefineStmt);
	newtype->kind = OBJECT_TYPE;
	newtype->defnames = list_make1(makeString(newname));

	newtype->definition = lappend(newtype->definition, makeDefElem("input", (Node *)inputname));
	newtype->definition = lappend(newtype->definition, makeDefElem("output", (Node *)outputname));

	if (att->attbyval)
		newtype->definition = lappend(newtype->definition, makeDefElem("passedbyvalue",
																	   (Node *)makeInteger(1)));

	if (att->attlen == -1)
		newtype->definition = lappend(newtype->definition, makeDefElem("internallength",
																	   (Node *)makeString("variable")));
	else
		newtype->definition = lappend(newtype->definition, makeDefElem("internallength",
																	   (Node *)makeInteger(att->attlen)));

	if (att->attalign == 'd')
		alignname = "double";
	else if (att->attalign == 'i')
		alignname = "int4";
	else if (att->attalign == 's')
		alignname = "int2";
	else if (att->attalign == 'c')
		alignname = "char";
	else
		Assert(false); /* shouldn't get here */

	newtype->definition = lappend(newtype->definition, makeDefElem("alignment",
																   (Node *)makeString(alignname)));

	parsetrees = parse_analyze((Node *)newtype, NULL, NULL, 0);
	q = (Query *)linitial(parsetrees);
	ProcessUtility((Node *)q->utilityStmt,
				   synthetic_sql,
				   NULL, 
				   false, /* not top level */
				   dest, 
				   NULL);
	CommandCounterIncrement();

	typname = makeNode(TypeName);
	typname->names = list_make1(makeString(pstrdup(newname)));

	return typname;
}

/* Allocate and return a pointer to a TypeName node specifying the
 * qualified name of an existing type that matches the internal length
 * and alignment of the given pg_attribute tuple.
 *
 * First we try a few hard-coded builtin types. Internal types exist for
 *    select distinct typlen, typalign
 *    from pg_type where typlen > 0;
 * on a new catalog.  Names are not unique.
 *
 * MPP-5483: If that fails, we look in pg_type for a match.  If even that
 * fails, we issue an error.
 *
 * Specifically for use by prebuilt_temp_table.
 */
static TypeName *
make_typname(Form_pg_attribute att, bool *built)
{
	TypeName *tname = makeNode(TypeName);
	int2 attlen = att->attlen;
	Value *typname = NULL;
	bool dig_in_catalog = false;

	Assert(att->attisdropped); /* better not be here unless */

	tname->typmod = att->atttypmod;

	if (attlen == -1)
	{
		if (att->attalign == 'i')
		{
			if (att->atttypmod < MaxAttrSize)
				typname = makeString(pstrdup("varchar"));
			else
				typname = makeString(pstrdup("numeric"));

			/* 
			 * We're building dropped columns. Dropped columns should never
			 * create a TOAST table because they only ever contain NULL. So,
			 * even if the original typmod was -1 (variable length), set it to a
			 * real value so that we avoid TOAST creation. To do this, we must
			 * be one byte longer than the maximum variable length header size.
			 * See needs_toast_table().
			 *
			 * XXX: there may be occassions where tables with dropped TOASTable
			 * columns do still have TOAST tables. Need to explore those.
			 */
			tname->typmod = 1 + Max(NUMERIC_HDRSZ, VARHDRSZ);
		}
		else if (att->attalign == 'd')
			typname = makeString(pstrdup("path"));
		else
			dig_in_catalog = true;
	}
	else if (attlen == 1 && att->attalign == 'c')
    {
		typname = makeString(pstrdup("char"));
    }
	else if (attlen == 2 && att->attalign == 's')
    {
		typname = makeString(pstrdup("int2"));
    }
	else if (attlen == 4 && att->attalign == 'i')
    {
		typname = makeString(pstrdup("int4"));
    }
	else if (attlen == 6 && att->attalign == 's')
    {
            typname = makeString(pstrdup("tid"));
	}
    else if (attlen == 6 && att->attalign == 'i')
	{
            typname = makeString(pstrdup("macaddr"));
    }
	else if (attlen == 8 && att->attalign == 'd')
    {
		typname = makeString(pstrdup("int8"));
    }
	else if (attlen == 12 && att->attalign == 'd')
    {
			typname = makeString(pstrdup("timetz"));
	}
	else if (attlen == 12 && att->attalign == 'i')
	{
		typname = makeString(pstrdup("tinterval"));
    }
	else if (attlen == 16 && att->attalign == 'd')
    {
		typname = makeString(pstrdup("interval"));
    }
	else if (attlen == 24 && att->attalign == 'd')
    {
		typname = makeString(pstrdup("circle"));
    }
	else if (attlen == 32 && att->attalign == 'd')
    {
		typname = makeString(pstrdup("box"));
    }
	else if (attlen == 64 && att->attalign == 'i')
    {
		typname = makeString(pstrdup("name"));
    }
	else
	{
		dig_in_catalog = true;
    }

	*built = false; /* assume we found an existing type */
	if ( dig_in_catalog )
	{
		tname = pick_name_of_similar_type(tname, attlen, att->attalign);
		if (!tname)
		{
			*built = true;
			tname = build_hidden_type(att);
		}
	}
	else
	{
		tname->names = list_make2(makeString(pstrdup("pg_catalog")), typname);
	}

	tname->location = -1;
	return tname;
}

/*
 * If the table has dropped columns, we must create the table and
 * drop the columns before we can dispatch the select statement.
 * Return true if we do it, false if we do not. If we return false,
 * there are no dropped columns and we can do a SELECT INTO later.
 * If we need to do it, but fail, issue an error. (See make_type.)
 *
 * Specifically for build_ctas_with_dist.
 *
 * Note that the caller should guarantee that isTmpTableAo has
 * a value that matches 'opts'.
 */
static bool
prebuild_temp_table(Relation rel, RangeVar *tmpname, List *distro, List *opts,
					List **hidden_types, bool isTmpTableAo)
{
	bool need_rebuild = false;
	int attno = 0;
	TupleDesc tupdesc = RelationGetDescr(rel);
	List *drop_atts = NIL; /* attnums where we want an attribute to drop */

	for (attno = 0; attno < tupdesc->natts; attno++)
	{
		if (tupdesc->attrs[attno]->attisdropped)
		{
			need_rebuild = true;
			break;
		}
	}

	/*
	 * If the new table is an AO table with indexes, always use
	 * Create Table + Insert Into. During Create Table phase,
	 * we determine whether to create the block directory
	 * depending on whether the original table has indexes. It is
	 * important to create the block directory to support the reindex
	 * later. See MPP-9545 for more info.
	 */
	if (isTmpTableAo &&
		rel->rd_rel->relhasindex)
		need_rebuild = true;

	if (need_rebuild)
	{
		CreateStmt *cs = makeNode(CreateStmt);
		List *parsetrees;
		Query *q;
		char *dstr = "__gp_atsdb_droppedcol";
		DestReceiver *dest = None_Receiver;

		cs->relKind = RELKIND_RELATION;
		cs->distributedBy = distro;
		cs->options = opts;
		cs->relation = tmpname;
		cs->ownerid = rel->rd_rel->relowner;
		cs->tablespacename = get_tablespace_name(rel->rd_rel->reltablespace);
		cs->buildAoBlkdir = false;

		if (isTmpTableAo &&
			rel->rd_rel->relhasindex)
			cs->buildAoBlkdir = true;

		for (attno = 0; attno < tupdesc->natts; attno++)
		{
			ColumnDef *cd = makeNode(ColumnDef);
			TypeName *tname = NULL;
			Form_pg_attribute att = tupdesc->attrs[attno];

			cd->is_local = true;

			if (att->attisdropped)
			{
				NameData alias;
				bool built;

				tname = make_typname(att, &built);
				snprintf(NameStr(alias), sizeof(alias), "%s%d", dstr, attno);
				cd->colname = pstrdup(NameStr(alias));
				drop_atts = lappend_int(drop_atts, attno);

				if (built)
					*hidden_types = lappend(*hidden_types, tname->names);
			}
			else
			{
				Type typ = typeidType(att->atttypid);
				Oid typnamespace = ((Form_pg_type) GETSTRUCT(typ))->typnamespace;
				char *nspname = get_namespace_name(typnamespace);
				int arno;
				char *typstr;
				int4 ndims = att->attndims;

				tname = makeNode(TypeName);

				if (!PointerIsValid(nspname))
					elog(ERROR, "could not lookup namespace %d", typnamespace);
				cd->colname = pstrdup(NameStr(att->attname));
				typstr = typeTypeName(typ);
				tname->names = list_make2(makeString(nspname),
										  makeString(typstr));
				ReleaseSysCache(typ);
				tname->typmod = att->atttypmod;

				/*
				 * If this is a built in array type, like _int4, then reduce
				 * the array dimensions by 1. This is an annoying postgres
				 * hack which I wish would go away.
				 */
				if (typstr && typstr[0] == '_' && ndims > 0)
					ndims--;

				for (arno = 0; arno < ndims; arno++)
					/* bound of -1 are fine because this has no effect on data */
					tname->arrayBounds = lappend(tname->arrayBounds,
												 makeInteger(-1));
			}
			tname->location = -1;
			cd->typname = tname;
			cs->tableElts = lappend(cs->tableElts, cd);
		}
		parsetrees = parse_analyze((Node *)cs, NULL, NULL, 0);
		Assert(list_length(parsetrees) == 1);
		q = (Query *)linitial(parsetrees);
		ProcessUtility((Node *)q->utilityStmt, 
					   synthetic_sql,
					   NULL, 
					   false, /* not top level */
					   dest, 
					   NULL);
		CommandCounterIncrement();

		/* should always be true */
		if (drop_atts)
		{
			/* Build an ALTER TABLE DROP COLUMN statement */
			AlterTableStmt *ats = makeNode(AlterTableStmt);
			ListCell *lc;

			ats->relkind = OBJECT_TABLE;
			ats->relation = tmpname;

			foreach(lc, drop_atts)
			{
				AlterTableCmd *atc = makeNode(AlterTableCmd);
				NameData colname;

				attno = lfirst_int(lc);
				atc->subtype = AT_DropColumn;
				snprintf(NameStr(colname), NAMEDATALEN, "%s%d", dstr, attno);
				atc->name = pstrdup(NameStr(colname));
				ats->cmds = lappend(ats->cmds, atc);
			}
#ifdef NOT_USED
			elog_node_display(NOTICE, "DROP STATEMENT", ats, true);
#endif

			parsetrees = parse_analyze((Node *)ats, NULL, NULL, 0);
			Assert(list_length(parsetrees) == 1);
			q = (Query *)linitial(parsetrees);
			ProcessUtility((Node *)q->utilityStmt, 
						   synthetic_sql,
						   NULL, 
						   false, /* not top level */
						   dest, 
						   NULL);
			CommandCounterIncrement();
		}
	}
	return need_rebuild;
}

/* Build a human readable tag for what we're doing */
static char *
make_distro_str(List *lwith, List *ldistro)
{
    char       *distro_str = "SET WITH DISTRIBUTED BY";

	if (lwith && ldistro)
		distro_str = "SET WITH DISTRIBUTED BY";
	else
	{
		if (lwith)
			distro_str = "SET WITH";
		else if (ldistro)
			distro_str = "SET DISTRIBUTED BY";
	}
	return pstrdup(distro_str); /* don't return a stack address */
}

/*
 * ALTER TABLE SET DISTRIBUTED BY
 *
 * set distribution policy for rel
 */
static void
ATExecSetDistributedBy(Relation rel, Node *node, AlterTableCmd *cmd)
{
	List 	   *lprime;
	List 	   *lwith, *ldistro;
	List	   *cols = NIL;
	ListCell   *lc;
	GpPolicy   *policy = NULL;
	QueryDesc  *queryDesc;
	RangeVar   *tmprv;
	Oid			tmprelid;
	Oid			tarrelid = RelationGetRelid(rel);
	List	   *oid_map = NIL;
	List	   *qe_data = NIL; /* data to pass to QEs */
	bool        rand_pol = false;
	bool        force_reorg = false;
	StdRdOptions *newopts = NULL;
	Datum		newOptions = PointerGetDatum(NULL);
	bool		change_policy = false;
	bool		is_ao = false;
    bool        is_aocs = false;
    char        relstorage = RELSTORAGE_HEAP;
	List	   *hidden_types = NIL; /* types we need to build for dropped columns */

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	/* Can't ALTER TABLE SET system catalogs */
	if (IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	Assert(PointerIsValid(node));
	Assert(IsA(node, List));

	lprime = (List *)node;

	/* 
	 * First element is the WITH clause, second element is the actual
	 * distribution clause.
	 */
	lwith   = (List *)linitial(lprime);
	ldistro = (List *)lsecond(lprime);

	if (Gp_role == GP_ROLE_UTILITY)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SET DISTRIBUTED BY not supported in utility mode")));

	/* we only support fully distributed tables */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		if (rel->rd_cdbpolicy->ptype != POLICYTYPE_PARTITIONED)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s not supported on non-distributed tables",
							ldistro ? "SET DISTRIBUTED BY" : "SET WITH"),
									   errOmitLocation(true)));
	}

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		if (lwith)
		{
			bool		 seen_reorg = false;
			ListCell	*lc;
			char		*reorg_str = "reorganize";
			List		*nlist = NIL;

			/* remove the "REORGANIZE=true/false" from the WITH clause */
			foreach(lc, lwith)
			{
				DefElem	*def = lfirst(lc);

				if (pg_strcasecmp(reorg_str, def->defname) != 0)
				{
					/* MPP-7770: disable changing storage options for now */
					if (!gp_setwith_alter_storage)
                        ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("option \"%s\" not supported",
										 def->defname),
								 errOmitLocation(true)));

					if (pg_strcasecmp(def->defname, "appendonly") == 0)
					{
						if (pg_strcasecmp(strVal(def->arg), "true") == 0)
                        {
							is_ao = true;
                            relstorage = RELSTORAGE_AOROWS;
                        }
						else
							is_ao = false;
					}

                    if (pg_strcasecmp(def->defname, "orientation") == 0)
                    {
						if (pg_strcasecmp(strVal(def->arg), "column") == 0)
                        {
                            is_aocs = true;
                            relstorage = RELSTORAGE_AOCOLS;
                        }
                        else
                        {
                            if (pg_strcasecmp(strVal(def->arg), "row") != 0)
                                ereport(ERROR,
                                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                         errmsg("valid orientation option is \"column\" or \"row\""),
                                  			   errOmitLocation(true)));
                        }
                    }

					nlist = lappend(nlist, def);
				}
				else
				{
					/* have we been here before ? */
					if (seen_reorg)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("\"%s\" specified more than once",
										reorg_str),
												   errOmitLocation(true)));

					seen_reorg = true;
					if (!def->arg)
						force_reorg = true;
					else
					{
						if (pg_strcasecmp("TRUE", strVal(def->arg)) == 0)
							force_reorg = true;
						else if (pg_strcasecmp("FALSE", strVal(def->arg)) == 0)
							force_reorg = false;
						else
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("invalid %s option \"%s\"",
											reorg_str, strVal(def->arg)),
													   errOmitLocation(true)));
					}
				}
			}

            if (is_aocs && !is_ao)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("specify orientation requires appendonly"),
                  			   errOmitLocation(true)));
            }

			lwith = nlist;

			/*
			 * If there are other storage options, but REORGANIZE is not
			 * specified, then the storage must be re-org'd.  But if
			 * REORGANIZE was specified use that setting.
			 *
			 * If the user specified we not force a reorg but there are other
			 * WITH clause items, then we cannot honour what the user has
			 * requested.
			 */
			if (!seen_reorg && list_length(lwith))
				force_reorg = true;
			else if (seen_reorg && force_reorg == false && list_length(lwith))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("%s must be set to true when changing storage "
								 "type", reorg_str),
										   errOmitLocation(true)));

			newOptions = new_rel_opts(rel, lwith);
			if (newOptions)
				newopts =
					(StdRdOptions *) heap_reloptions(rel->rd_rel->relkind,
													 newOptions, true);
		}

		if (ldistro)
			change_policy = true;

		if (ldistro && linitial(ldistro) == NULL)
		{
			Insist(list_length(ldistro) == 1);

			rand_pol = true;

			if (!force_reorg)
			{
				if (rel->rd_cdbpolicy->nattrs == 0)
					ereport(WARNING,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("distribution policy of relation \"%s\" "
									"already set to DISTRIBUTED RANDOMLY",
									RelationGetRelationName(rel)),
							 errhint("Use ALTER TABLE \"%s\" "
									 "SET WITH (REORGANIZE=TRUE) "
									 "DISTRIBUTED RANDOMLY "
									 "to force a random redistribution",
									 RelationGetRelationName(rel)),
											   errOmitLocation(true)));
			}

			policy = (GpPolicy *) palloc(sizeof(GpPolicy));
			policy->ptype = POLICYTYPE_PARTITIONED;
			policy->nattrs = 0;

			rel->rd_cdbpolicy = GpPolicyCopy(GetMemoryChunkContext(rel),
										 policy);
			GpPolicyReplace(RelationGetRelid(rel), policy);

			/* only need to rebuild if have new storage options */
			if (!(newopts || force_reorg))
			{
				/*
				 * caller expects ATExecSetDistributedBy() to close rel
				 * (see the non-random distribution case below for why.
				 */
				heap_close(rel, NoLock);
				goto l_distro_fini;
			}
		}
	}

	/*
	 * Changing a table from random distribution to a specific distribution
	 * policy is the hard bit. For that, we must do the following:
	 *
	 * a) Ensure that the proposed policy is sensible
	 * b) Create a temporary table and reorganise data according to
	 * our desired distribution policy. To do this, we build a Query
	 * node which expresses the query CREATE TABLE tmp_table_name AS
	 * SELECT * FROM cur_table DISTRIBUTED BY (policy)
	 * c) Execute the query across all nodes
	 * d) Update our parse tree to include the details of the newly created
	 * table
	 * e) Update the ownership of the temporary table
	 * f) Swap the relfilenodes of the existing table and the temporary table
	 * g) Update the policy on the QD to reflect the underlying data
	 * h) Drop the temporary table -- and with it, the old copy of the data
	 */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		volatile Snapshot saveSnapshot = NULL;
		if (change_policy)
		{
			policy = palloc(sizeof(GpPolicy) +
							sizeof(policy->attrs[0]) * list_length(ldistro));
			policy->ptype = POLICYTYPE_PARTITIONED;
			policy->nattrs = 0;

			/* Step (a) */
			if (!rand_pol)
			{
				foreach(lc, ldistro)
				{
					char *colName = strVal((Value *)lfirst(lc));
					HeapTuple tuple =
						SearchSysCacheAttName(RelationGetRelid(rel),
											  colName);
					AttrNumber	attnum;

					if (list_member(cols, lfirst(lc)))
							ereport(ERROR,
									(errcode(ERRCODE_DUPLICATE_OBJECT),
									 errmsg("distribution policy must be a "
											"unique set of columns"),
									 errhint("Column \"%s\" appears "
											 "more than once", colName),
													   errOmitLocation(true)));
					if (!HeapTupleIsValid(tuple))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_COLUMN),
									 errmsg("column \"%s\" of "
											"relation \"%s\" does not exist",
											colName,
											RelationGetRelationName(rel)),
													   errOmitLocation(true)));

					attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

					/* Prevent them from altering a system attribute */
					if (attnum <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						    errmsg("cannot distribute by system column \"%s\"",
									colName),
											   errOmitLocation(true)));

					policy->attrs[policy->nattrs++] = attnum;
					ReleaseSysCache(tuple);
					cols = lappend(cols, lfirst(lc));
				} /* end foreach */

				Assert(policy->nattrs > 0);

				/*
				 * See if the the old policy is the same as the new one but
				 * remember, we still might have to rebuild if there are new
				 * storage options.
				 */
				if (!newopts && !force_reorg &&
					(policy->nattrs == rel->rd_cdbpolicy->nattrs))
				{
					int i;
					bool diff = false;

					for (i = 0; i < policy->nattrs; i++)
					{
						if (policy->attrs[i] != rel->rd_cdbpolicy->attrs[i])
						{
							diff = true;
							break;
						}
					}
					if (!diff)
					{
						char *dist =
							palloc(list_length(ldistro) * (NAMEDATALEN + 1));

						dist[0] = '\0';

						foreach(lc, ldistro)
						{
							if (lc != list_head(ldistro))
								strcat(dist, ",");
							strcat(dist, strVal(lfirst(lc)));
						}
						ereport(WARNING,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("distribution policy of relation \"%s\" "
									"already set to (%s)",
									RelationGetRelationName(rel),
									dist),
							 errhint("Use ALTER TABLE \"%s\" "
									 "SET WITH (REORGANIZE=TRUE) "
									 "DISTRIBUTED BY (%s) "
									 "to force redistribution",
									 RelationGetRelationName(rel),
									 dist),
											   errOmitLocation(true)));
						heap_close(rel, NoLock);
						/* Tell QEs to do nothing */
						linitial(lprime) = NULL;
						lsecond(lprime) = list_make1(NULL);

						return;
						/* don't goto l_distro_fini -- didn't do anything! */
					}
				}
			}
		}

		if (!ldistro)
			ldistro = make_dist_clause(rel);

		/* Step (b) - build CTAS */
		queryDesc = build_ctas_with_dist(rel, ldistro, newopts, &tmprv,
										 &hidden_types);

		PG_TRY();
		{
			/* 
			 * We need to update our snapshot here to make sure we see all
			 * committed work. We have an exclusive lock on the table so no one
			 * will be able to access the table now.
			 */
			saveSnapshot = ActiveSnapshot;
			ActiveSnapshot = CopySnapshot(GetLatestSnapshot());

	
			/* Step (c) - run on all nodes */
			ExecutorStart(queryDesc, 0);
			ExecutorRun(queryDesc, ForwardScanDirection, 0L);
			ExecutorEnd(queryDesc);
			FreeQueryDesc(queryDesc);

			/* Restore the old snapshot */
			ActiveSnapshot = saveSnapshot;
		}
		PG_CATCH();
		{
			ActiveSnapshot = saveSnapshot;
			PG_RE_THROW();
		}
		PG_END_TRY();

		CommandCounterIncrement(); /* see the effects of the command */

		/*
		 * Step (d) - tell the seg nodes about the temporary relation. This
		 * involves stomping on the node we've been given
		 */
		qe_data = lappend(qe_data, makeInteger(MyBackendId));
	}
	else
	{
		int backend_id;
		bool reorg = true;

		Assert(list_length(lprime) >= 2);

		lwith = linitial(lprime);
		qe_data = lsecond(lprime);

		if (lwith)
		{
			if (list_length(lwith))
			{
				DefElem *e = linitial(lwith);
				if (pg_strcasecmp(e->defname, "reorganize") == 0 &&
					pg_strcasecmp(strVal(e->arg), "false") == 0)
					reorg = false;
			}
		}
		else if (!lwith)
			reorg = false;

		/* Set to random distribution on master with no reorganisation */
		if (!reorg && list_length(qe_data) == 1 && linitial(qe_data) == NULL)
		{
			/* caller expects rel to be closed for this AT type */
			heap_close(rel, NoLock);
			goto l_distro_fini;			
		}

		backend_id = intVal(linitial(qe_data));
		tmprv = make_temp_table_name(rel, backend_id);
		oid_map = lsecond(qe_data);
		if (list_length(qe_data) == 3)
			hidden_types = lthird(qe_data);

		if (list_length(lprime) == 3)
		{
			Value *v = lthird(lprime);
			if (intVal(v) == 1)
            {
				is_ao = true;
                relstorage = RELSTORAGE_AOROWS;
            }
            else if (intVal(v) == 2)
            {
                is_ao = true;
                is_aocs = true;
                relstorage = RELSTORAGE_AOCOLS;
            }
			else
            {
				is_ao = false;
                relstorage = RELSTORAGE_HEAP;
            }
		}

		newOptions = new_rel_opts(rel, lwith);
	}

	/*
	 * Step (e) - Correct ownership on temporary table:
	 *   necessary so that the toast tables/indices have the correct
	 *   owner after we swap them.
	 *
	 * Note: ATExecChangeOwner does NOT dispatch, so this does not
	 * belong in the dispatch block above (MPP-9663).
	 */
	ATExecChangeOwner(RangeVarGetRelid(tmprv, false),
					  rel->rd_rel->relowner, true);
	CommandCounterIncrement(); /* see the effects of the command */

	/*
	 * Step (f) - swap relfilenodes and MORE !!!
	 *
	 * Just lookup the Oid and pass it to swap_relation_files(). To do
	 * this we must close the rel, since it needs to be forgotten by
	 * the cache, we keep the lock though. ATRewriteCatalogs() knows
	 * that we've closed the relation here.
	 */
	heap_close(rel, NoLock);
   	tmprelid = RangeVarGetRelid(tmprv, false);
	swap_relation_files(tarrelid, tmprelid, false);

	if (DatumGetPointer(newOptions))
	{
		Datum		repl_val[Natts_pg_class];
		bool		repl_null[Natts_pg_class];
		bool		repl_repl[Natts_pg_class];
		HeapTuple	newOptsTuple;
		Relation	class = heap_open(RelationRelationId, RowExclusiveLock);
		HeapTuple	tuple;

		/*
		 * All we need do here is update the pg_class row; the new
		 * options will be propagated into relcaches during
		 * post-commit cache inval.
		 */
		MemSet(repl_val, 0, sizeof(repl_val));
		MemSet(repl_null, false, sizeof(repl_null));
		MemSet(repl_repl, false, sizeof(repl_repl));

		if (newOptions != (Datum) 0)
			repl_val[Anum_pg_class_reloptions - 1] = newOptions;
		else
			repl_null[Anum_pg_class_reloptions - 1] = true;

		repl_repl[Anum_pg_class_reloptions - 1] = true;

		tuple = SearchSysCache(RELOID,
								   ObjectIdGetDatum(tarrelid),
								   0, 0, 0);
		Insist(HeapTupleIsValid(tuple));
		newOptsTuple = heap_modify_tuple(tuple, RelationGetDescr(class),
									repl_val, repl_null, repl_repl);

		simple_heap_update(class, &newOptsTuple->t_self, newOptsTuple);
		CatalogUpdateIndexes(class, newOptsTuple);

		heap_freetuple(newOptsTuple);
		ReleaseSysCache(tuple);

		heap_close(class, RowExclusiveLock);

		/*
		 * Increment cmd counter to make updates visible; this is
		 * needed because the same tuple has to be updated again
		 */
		CommandCounterIncrement();
	}

	/* now, reindex */
	reindex_relation(tarrelid, false, false /* ao_segs ? */, false,
					 &oid_map, Gp_role == GP_ROLE_DISPATCH);

	/* Step (g) */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		if (change_policy)
			GpPolicyReplace(tarrelid, policy);

		qe_data = lappend(qe_data, oid_map);

		if (hidden_types)
			qe_data = lappend(qe_data, hidden_types);
		linitial(lprime) = lwith;
		lsecond(lprime) = qe_data;
		lprime = lappend(lprime, makeInteger(is_ao ? (is_aocs ? 2 : 1) : 0));
	}

	/* Step (h) Drop the table */
	{
		ObjectAddress object;
		object.classId = RelationRelationId;
		object.objectId = tmprelid;
		object.objectSubId = 0;

		performDeletion(&object, DROP_RESTRICT);

		if (hidden_types)
		{
			object.classId = TypeRelationId;

			foreach(lc, hidden_types)
			{
				TypeName *tname = makeTypeNameFromNameList(lfirst(lc));
				Oid typid = typenameTypeId(NULL, tname);

				object.objectId = typid;

				/* cascade to get rid of functions */
				performDeletion(&object, DROP_CASCADE);
			}
		}
	}

l_distro_fini:

	/* MPP-6929: metadata tracking */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		char *distro_str = make_distro_str(lwith, ldistro);
		/* don't check relkind - must be a table */
		MetaTrackUpdObject(RelationRelationId,
						   tarrelid,
						   GetUserId(),
						   "ALTER", distro_str
				);
	}

}

static char
parttype_to_char(PartitionByType type)
{
	char c;

	switch (type)
	{
		case PARTTYP_HASH: c = 'h'; break;
		case PARTTYP_RANGE: c = 'r'; break;
		case PARTTYP_LIST: c = 'l'; break;
		default:
			c = 0; /* quieten compiler */
			elog(ERROR, "unknown partitioning type %i", type);

	}
	return c;
}

#ifdef NEVER_EVER

static char*
parttypeGetName(PartitionByType type)
{
	char *pName;

	switch (type)
	{
		case PARTTYP_HASH:  pName = pstrdup("HASH"); break;
		case PARTTYP_RANGE: pName = pstrdup("RANGE"); break;
		case PARTTYP_LIST:  pName = pstrdup("LIST"); break;
		default:
			/* should never happen */
			elog(ERROR, "unknown partitioning type %i", type);
			Assert(false);
	}
	return pName;
}

#endif

/*
 * Translate a catalog partition type indication ('r', 'h' or 'l') to the
 * internal representation.
 */
static PartitionByType
char_to_parttype(char c)
{
	PartitionByType pt = PARTTYP_RANGE; /* just to shut GCC up */

	switch(c)
	{
		case 'h': /* hash */
			pt = PARTTYP_HASH;
			break;
		case 'r': /* range */
			pt = PARTTYP_RANGE;
			break;
		case 'l': /* list */
			pt = PARTTYP_LIST;
			break;
		default:
			elog(ERROR, "unrecognized partitioning kind '%c'",
				 c);
			Assert(false);
			break;
	} /* end switch */

	return pt;
}


static void
add_partition(Partition *part)
{
	Datum values[Natts_pg_partition];
	bool isnull[Natts_pg_partition];
	Relation partrel;
	HeapTuple tup;
	oidvector *opclass;
	int2vector *attnums;

	MemSet(isnull, 0, sizeof(bool) * Natts_pg_partition);

	values[Anum_pg_partition_parrelid - 1] = ObjectIdGetDatum(part->parrelid);
	values[Anum_pg_partition_parkind - 1] = CharGetDatum(part->parkind);
	values[Anum_pg_partition_parlevel - 1] = Int16GetDatum(part->parlevel);
	values[Anum_pg_partition_paristemplate - 1] =
		BoolGetDatum(part->paristemplate);
	values[Anum_pg_partition_parnatts - 1] = Int16GetDatum(part->parnatts);

	attnums = buildint2vector(part->paratts, part->parnatts);
	values[Anum_pg_partition_paratts - 1] = PointerGetDatum(attnums);

	opclass = buildoidvector(part->parclass, part->parnatts);
	values[Anum_pg_partition_parclass - 1] = PointerGetDatum(opclass);

	partrel = heap_open(PartitionRelationId, RowExclusiveLock);

	tup = heap_form_tuple(RelationGetDescr(partrel), values, isnull);

	part->partid = simple_heap_insert(partrel, tup);

	/* Update indexes */
	CatalogUpdateIndexes(partrel, tup);

	heap_close(partrel, NoLock);
}

static void
add_partition_rule(PartitionRule *rule)
{
	Datum values[Natts_pg_partition_rule];
	bool isnull[Natts_pg_partition_rule];
	Relation rulerel;
	HeapTuple tup;
	NameData name;

	MemSet(isnull, 0, sizeof(bool) * Natts_pg_partition_rule);

	values[Anum_pg_partition_rule_paroid - 1] = ObjectIdGetDatum(rule->paroid);
	values[Anum_pg_partition_rule_parchildrelid - 1] =
		ObjectIdGetDatum(rule->parchildrelid);
	values[Anum_pg_partition_rule_parparentrule - 1] =
		ObjectIdGetDatum(rule->parparentoid);

	name.data[0] = '\0';
	namestrcpy(&name, rule->parname);
	values[Anum_pg_partition_rule_parname - 1] = NameGetDatum(&name);

	values[Anum_pg_partition_rule_parisdefault - 1] =
		BoolGetDatum(rule->parisdefault);
	values[Anum_pg_partition_rule_parruleord - 1] =
		Int16GetDatum(rule->parruleord);
	values[Anum_pg_partition_rule_parrangestartincl - 1] =
		BoolGetDatum(rule->parrangestartincl);
	values[Anum_pg_partition_rule_parrangeendincl - 1] =
		BoolGetDatum(rule->parrangeendincl);

	values[Anum_pg_partition_rule_parrangestart - 1] =
		DirectFunctionCall1(textin,
							CStringGetDatum(nodeToString(rule->parrangestart)));
	values[Anum_pg_partition_rule_parrangeend - 1] =
		DirectFunctionCall1(textin,
							CStringGetDatum(nodeToString(rule->parrangeend)));
	values[Anum_pg_partition_rule_parrangeevery - 1] =
		DirectFunctionCall1(textin,
							CStringGetDatum(nodeToString(rule->parrangeevery)));
	values[Anum_pg_partition_rule_parlistvalues - 1] =
		DirectFunctionCall1(textin,
							CStringGetDatum(nodeToString(rule->parlistvalues)));
	if (rule->parreloptions)
		values[Anum_pg_partition_rule_parreloptions - 1] =
			 transformRelOptions((Datum) 0, rule->parreloptions, true, false);
	else
		isnull[Anum_pg_partition_rule_parreloptions - 1] = true;
	
	values[Anum_pg_partition_rule_partemplatespace -1] = 
			ObjectIdGetDatum(rule->partemplatespaceId);

	rulerel = heap_open(PartitionRuleRelationId, RowExclusiveLock);

	tup = heap_form_tuple(RelationGetDescr(rulerel), values, isnull);

	rule->parruleid = simple_heap_insert(rulerel, tup);

	/* Update indexes */
	CatalogUpdateIndexes(rulerel, tup);

	heap_close(rulerel, NoLock);
}

/*
 * Oid of the row of pg_partition corresponding to the given relation and level.
 */
static Oid
get_part_oid(Oid rootrelid, int16 parlevel, bool istemplate)
{
	HeapTuple tuple;
	Oid paroid = InvalidOid;
	ScanKeyData key[3];
	SysScanDesc scan;
	
	/* select oid
	 * from pg_partition
	 * where
	 *     parrelid = :rootrelid and
	 *     parlevel = :parlevel and
	 *     paristemplate = :istemplate;
	 */
	Relation rel = heap_open(PartitionRelationId, AccessShareLock);
	
	/* pg_partition and  pg_partition_rule are populated only on the 
	 * entry database, so our result is only meaningful there. */
	Insist(Gp_segment == -1);
	
	ScanKeyInit(&key[0],
				Anum_pg_partition_parrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rootrelid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_parlevel,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(parlevel));
	ScanKeyInit(&key[2],
				Anum_pg_partition_paristemplate,
				BTEqualStrategyNumber, F_BOOLEQ,
				Int16GetDatum(istemplate));

	scan = systable_beginscan(rel,
							  PartitionParrelidParlevelParistemplateIndexId,
							  true, SnapshotSelf, 3, key);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		paroid = HeapTupleGetOid(tuple);

	systable_endscan(scan);
	heap_close(rel, NoLock);

	return paroid;
}

/* delete the template for a partition */
static int
del_part_template(Oid rootrelid, int16 parlevel, Oid parent)
{
	bool istemplate = true;
	HeapTuple tuple;
	Oid paroid = InvalidOid;
	ScanKeyData key[3];
	SysScanDesc scan;
	Relation rel = heap_open(PartitionRelationId, RowExclusiveLock);
	int ii = 0;

	ScanKeyInit(&key[0],
				Anum_pg_partition_parrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rootrelid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_parlevel,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(parlevel));
	ScanKeyInit(&key[2],
				Anum_pg_partition_paristemplate,
				BTEqualStrategyNumber, F_BOOLEQ,
				Int16GetDatum(istemplate));

	scan = systable_beginscan(rel,
							  PartitionParrelidParlevelParistemplateIndexId,
							  true, SnapshotNow, 3, key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		if (HeapTupleIsValid(tuple))
		{
			paroid = HeapTupleGetOid(tuple);
			ii++;
		}
	}
	systable_endscan(scan);
	heap_close(rel, RowExclusiveLock);

	/* should only be one matching template per level */
	if (ii != 1)
		return ii;

	rel = heap_open(PartitionRuleRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_partition_rule_paroid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(paroid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_rule_parparentrule,
				BTEqualStrategyNumber, F_INT2EQ,
				ObjectIdGetDatum(parent));

	scan = systable_beginscan(rel, 
						  PartitionRuleParoidParparentruleParruleordIndexId,
							  true, SnapshotNow, 2, key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		if (HeapTupleIsValid(tuple))
		{
			simple_heap_delete(rel, &tuple->t_self);
			ii++;
		}
	}
	systable_endscan(scan);
	heap_close(rel, RowExclusiveLock);

	/* now delete the pg_partition entry */
	rel = heap_open(PartitionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_partition_parrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rootrelid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_parlevel,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(parlevel));
	ScanKeyInit(&key[2],
				Anum_pg_partition_paristemplate,
				BTEqualStrategyNumber, F_BOOLEQ,
				Int16GetDatum(istemplate));

	scan = systable_beginscan(rel,
							  PartitionParrelidParlevelParistemplateIndexId,
							  true, SnapshotNow, 3, key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		if (HeapTupleIsValid(tuple))
		{
			paroid = HeapTupleGetOid(tuple);

			simple_heap_delete(rel, &tuple->t_self);
			ii++;
		}
	}
	systable_endscan(scan);
	heap_close(rel, RowExclusiveLock);

	/* make visible */
	CommandCounterIncrement();

	return 1;
} /* end del_part_template */

/*
 * add_part_to_catalog() - add a partition to the catalog
 *
 *

 * NOTE: If bTemplate_Only = false, add both actual partitions and the
 * template definitions (if specified).  However, if bTemplate_Only =
 * true, then only treat the partition spec as a template.
 */
static void
add_part_to_catalog(Oid relid, PartitionBy *pby,
					bool bTemplate_Only /* = false */)
{
	char pt = parttype_to_char(pby->partType);
	ListCell *lc;
	PartitionSpec *spec;
	Oid paroid = InvalidOid;
	Oid rootrelid = InvalidOid;
	ScanKeyData key[3];
	HeapTuple tuple;
	Relation rel;
	SysScanDesc scan;
	Oid parttemplid = InvalidOid;
	bool add_temp = bTemplate_Only; /* normally false */

	/* only create partition catalog entries on the master */
	if (Gp_role == GP_ROLE_EXECUTE)
		return;

	/*
	 * Get the partitioned table relid.
	 */
	rootrelid = RangeVarGetRelid(pby->parentRel, false);
	paroid = get_part_oid(rootrelid, pby->partDepth,
						  bTemplate_Only /* = false */);

	/* create a partition for this level, if one doesn't exist */
	if (!OidIsValid(paroid))
	{
		AttrNumber *attnums;
		Oid *parclass;
		Partition *part = makeNode(Partition);
		int i = 0;

		part->parrelid = rootrelid;
		part->parkind = pt;
		part->parlevel = pby->partDepth;

		if (pby->partSpec)
			part->paristemplate = ((PartitionSpec *)pby->partSpec)->istemplate;
		else
			part->paristemplate = false;

		part->parnatts = list_length(pby->keys);

		attnums = palloc(list_length(pby->keys) * sizeof(AttrNumber));

		foreach(lc, pby->keys)
		{
			int colnum = lfirst_int(lc);

			attnums[i++] = colnum;
		}

		part->paratts = attnums;

		parclass = palloc(list_length(pby->keys) * sizeof(Oid));

		i = 0;
		foreach(lc, pby->keyopclass)
		{
			Oid opclass = lfirst_oid(lc);

			parclass[i++] = opclass;
		}
		part->parclass = parclass;

		add_partition(part);

		/*
		 * If we added a template, we treat that as a 'virtual' entry and then
		 * add a modifiable entry, which is not a template.
		 */
		if (part->paristemplate)
		{
			add_temp = true;
			parttemplid = part->partid;

			/* if only building a template, don't add "real" entries */
			if (!bTemplate_Only)
			{
				part->paristemplate = false;
				add_partition(part);
			}
		}
		paroid = part->partid;
	}
	else
		/* oid of the template accompanying the real partition */
		parttemplid = get_part_oid(rootrelid, pby->partDepth, true);

	/* create partition rule */
	spec = (PartitionSpec *)pby->partSpec;
	if (spec)
	{
		Node *listvalues = NULL;
		Node *rangestart = NULL;
		Node *rangeend = NULL;
		Node *rangeevery = NULL;
		bool rangestartinc = false;
		bool rangeendinc = false;
		int2 parruleord = 0;
		PartitionRule *rule = makeNode(PartitionRule);
		PartitionElem *el;
		char *parname = NULL;
		Oid parentoid = InvalidOid;

		Assert(list_length(spec->partElem) == 1);

		el = linitial(spec->partElem);

		parruleord = el->partno;

		if (el->partName)
			parname = strVal(el->partName);

		switch (pby->partType)
		{
			case PARTTYP_HASH: break;
			case PARTTYP_LIST:
				{
					PartitionValuesSpec *vspec =
						(PartitionValuesSpec *)el->boundSpec;

					/* might be NULL if this is a default spec */
					if (vspec)
						listvalues = (Node *)vspec->partValues;
				}
				break;
			case PARTTYP_RANGE:
				{
					PartitionBoundSpec *bspec =
						(PartitionBoundSpec *)el->boundSpec;
					PartitionRangeItem *ri;

					/* remember, could be a default clause */
					if (bspec)
					{
						Assert(IsA(bspec, PartitionBoundSpec));
						ri = (PartitionRangeItem *)bspec->partStart;
						if (ri)
						{
							Assert(ri->partedge == PART_EDGE_INCLUSIVE ||
								   ri->partedge == PART_EDGE_EXCLUSIVE);

							rangestartinc = ri->partedge == PART_EDGE_INCLUSIVE;
							rangestart = (Node *)ri->partRangeVal;
						}

						ri = (PartitionRangeItem *)bspec->partEnd;
						if (ri)
						{
							Assert(ri->partedge == PART_EDGE_INCLUSIVE ||
							   ri->partedge == PART_EDGE_EXCLUSIVE);

							rangeendinc = ri->partedge == PART_EDGE_INCLUSIVE;
							rangeend = (Node *)ri->partRangeVal;
						}

						if (bspec->partEvery)
						{
							ri = (PartitionRangeItem *)bspec->partEvery;
							rangeevery = (Node *)ri->partRangeVal;
						}
						else
							rangeevery = NULL;
					}
				}
				break;
			default:
				elog(ERROR, "unknown partitioning type %i", pby->partType);
				break;
		}

		/* Find our parent */
		if (!bTemplate_Only && (pby->partDepth > 0))
		{
			Form_pg_inherits inh;
			Oid inhoid;

			rel = heap_open(InheritsRelationId, AccessShareLock);
			ScanKeyInit(&key[0],
						Anum_pg_inherits_inhrelid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(relid));
			scan = systable_beginscan(rel, InheritsRelidSeqnoIndexId,
									  true, SnapshotAny, 1, key);

			tuple = systable_getnext(scan);
			Assert(HeapTupleIsValid(tuple));
			inh = (Form_pg_inherits) GETSTRUCT(tuple);

			inhoid = inh->inhparent;
			systable_endscan(scan);
			heap_close(rel, NoLock);

			rel = heap_open(PartitionRuleRelationId, AccessShareLock);

			ScanKeyInit(&key[0],
						Anum_pg_partition_rule_parchildrelid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(inhoid));
			scan = systable_beginscan(rel, PartitionRuleParchildrelidIndexId,
									  true, SnapshotAny, 1, key);

			tuple = systable_getnext(scan);

			Assert(HeapTupleIsValid(tuple));

			parentoid = HeapTupleGetOid(tuple);

			systable_endscan(scan);
			heap_close(rel, NoLock);
		}
		else
			add_temp = true;

		/* we still might have to add template rules */
		if (!add_temp && OidIsValid(parttemplid))
		{
			rel = heap_open(PartitionRuleRelationId, AccessShareLock);
			ScanKeyInit(&key[0],
						Anum_pg_partition_rule_paroid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(parttemplid));

			ScanKeyInit(&key[1],
						Anum_pg_partition_rule_parparentrule,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(InvalidOid));

			ScanKeyInit(&key[2],
						Anum_pg_partition_rule_parruleord,
						BTEqualStrategyNumber, F_OIDEQ,
						Int16GetDatum(parruleord));

			scan = systable_beginscan(rel,
						  PartitionRuleParoidParparentruleParruleordIndexId,
									  true, SnapshotAny, 3, key);

			tuple = systable_getnext(scan);
			if (HeapTupleIsValid(tuple))
				Assert(
     ((Form_pg_partition_rule)GETSTRUCT(tuple))->parchildrelid == InvalidOid);
			else
				add_temp = true;

			systable_endscan(scan);
			heap_close(rel, AccessShareLock);
		}

		rule->paroid = paroid;
		rule->parchildrelid = relid;
		rule->parparentoid = parentoid;
		rule->parisdefault = el->isDefault;
		rule->parname = parname;
		rule->parruleord = parruleord;
		rule->parrangestartincl = rangestartinc;
		rule->parrangestart = rangestart;
		rule->parrangeendincl = rangeendinc;
		rule->parrangeend = rangeend;
		rule->parrangeevery = rangeevery;
		rule->parlistvalues = (List *)listvalues;
		rule->partemplatespaceId = InvalidOid; /* only valid for template */

		if (!bTemplate_Only)
			add_partition_rule(rule);

		if (OidIsValid(parttemplid) && add_temp)
		{
			rule->paroid = parttemplid;
			rule->parparentoid = InvalidOid;
			rule->parchildrelid = InvalidOid;

			if (el->storeAttr)
			{
				if (((AlterPartitionCmd *)el->storeAttr)->arg1)
					rule->parreloptions =
							(List *)((AlterPartitionCmd *)el->storeAttr)->arg1;
				if (((AlterPartitionCmd *)el->storeAttr)->arg2)
				{
					Oid			tablespaceId;

					tablespaceId = 
							get_settable_tablespace_oid(
						strVal(((AlterPartitionCmd *)el->storeAttr)->arg2));
					
					/* get_settable_tablespace_oid will error out for us */
					Assert(OidIsValid(tablespaceId)); 

					/* only valid for template definitions */
					rule->partemplatespaceId = tablespaceId;
				}
			}
			add_partition_rule(rule);
		}
	}

	/* allow subsequent callers to see our work */
	CommandCounterIncrement();
} /* end add_part_to_catalog */


/*
 * Add partition hierarchy to catalogs.
 *
 * parts is a list, with a partitioning rule and potentially sub-partitions.
 */
static void
ATExecPartAddInternal(Relation rel, Node *def)
{
	PartitionBy *part = (PartitionBy *)def;
	add_part_to_catalog(RelationGetRelid(rel), part, false);
}

/*
 * parruleord_reset_rank
 *
 * iterate over the specified set of range partitions (in ascending
 * order) in pg_partition_rule and reset the parruleord to start at 1
 * and continue in an ascending sequence.
 */
static void
parruleord_reset_rank(Oid partid, int2 level, Oid parent, int2 ruleord,
					  MemoryContext mcxt)
{
	ScanKeyData key[3];
	HeapTuple tuple;
	Relation rel;
	SysScanDesc scan;
	int ii = 1;

	rel = heap_open(PartitionRuleRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_partition_rule_paroid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_rule_parparentrule,
				BTEqualStrategyNumber, F_INT2EQ,
				ObjectIdGetDatum(parent));
	ScanKeyInit(&key[2],
                Anum_pg_partition_rule_parruleord,
				BTGreaterEqualStrategyNumber, F_INT2GE,
				Int16GetDatum(ruleord));

	scan = systable_beginscan(rel, 
						  PartitionRuleParoidParparentruleParruleordIndexId,
							  true, SnapshotNow, 3, key);

	while ((tuple = systable_getnext(scan)))
	{
		Form_pg_partition_rule rule_desc;

		Insist(HeapTupleIsValid(tuple));

		tuple = heap_copytuple(tuple);

		rule_desc =
			(Form_pg_partition_rule)GETSTRUCT(tuple);

		rule_desc->parruleord = ii;
		ii++;

		simple_heap_update(rel, &tuple->t_self, tuple);
	    CatalogUpdateIndexes(rel, tuple);

	}
	systable_endscan(scan);
	heap_close(rel, AccessShareLock);
} /* end parruleord_reset_rank */

/*
 * parruleord_open_gap
 *
 * iterate over the specified set of range partitions (in *DESCENDING*
 * order) in pg_partition_rule and increment the parruleord in order
 * to open a "gap" for a new partition.  The stopkey is inclusive: to
 * insert a new partition at parruleord=5, set the stopkey to 5.  The
 * current partition at parruleord=5 (and all subsequent partitions)
 * are incremented by 1 to allow the insertion of the new partition.
 *
 */
static void
parruleord_open_gap(Oid partid, int2 level, Oid parent, int2 ruleord,
					int stopkey,
					MemoryContext mcxt)
{
	ScanKeyData key[3];
	HeapTuple tuple;
	Relation rel;
	SysScanDesc scan;

	rel = heap_open(PartitionRuleRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_partition_rule_paroid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_rule_parparentrule,
				BTEqualStrategyNumber, F_INT2EQ,
				ObjectIdGetDatum(parent));
	ScanKeyInit(&key[2],
                Anum_pg_partition_rule_parruleord,
				BTLessEqualStrategyNumber, F_INT2LE,
				Int16GetDatum(ruleord));

	scan = systable_beginscan(rel, 
						  PartitionRuleParoidParparentruleParruleordIndexId,
							  true, SnapshotNow, 3, key);

	while ((tuple = systable_getprev(scan)))
	{
		int old_ruleord;
		Form_pg_partition_rule rule_desc;

		Insist(HeapTupleIsValid(tuple));

		tuple = heap_copytuple(tuple);

		rule_desc =
			(Form_pg_partition_rule)GETSTRUCT(tuple);

		old_ruleord = rule_desc->parruleord;
		rule_desc->parruleord++;

		simple_heap_update(rel, &tuple->t_self, tuple);
	    CatalogUpdateIndexes(rel, tuple);

		if (old_ruleord <= stopkey)
			break;
	}
	systable_endscan(scan);
	heap_close(rel, AccessShareLock);
} /* end parruleord_open_gap */

/*
 * Build up a PartitionRule based on a tuple from pg_partition_rule
 * Exported for ruleutils.c
 */
PartitionRule *
ruleMakePartitionRule(HeapTuple tuple, TupleDesc tupdesc, MemoryContext mcxt)
{
	Form_pg_partition_rule rule_desc =
		(Form_pg_partition_rule)GETSTRUCT(tuple);
	text *rule_text;
	char *rule_str;
	Datum rule_datum;
	bool isnull;
	MemoryContext oldcxt;
	PartitionRule *rule;

	oldcxt = MemoryContextSwitchTo(mcxt);
	rule = makeNode(PartitionRule);
	MemoryContextSwitchTo(oldcxt);

	rule->parruleid = HeapTupleGetOid(tuple);
	rule->paroid = rule_desc->paroid;
	rule->parchildrelid = rule_desc->parchildrelid;
	rule->parparentoid = rule_desc->parparentrule;
	rule->parisdefault = rule_desc->parisdefault;

	rule->parname = MemoryContextStrdup(mcxt, NameStr(rule_desc->parname));

	rule->parruleord = rule_desc->parruleord;
	rule->parrangestartincl = rule_desc->parrangestartincl;
	rule->parrangeendincl = rule_desc->parrangeendincl;

	/* start range */
	rule_datum = heap_getattr(tuple,
							  Anum_pg_partition_rule_parrangestart,
							  tupdesc,
							  &isnull);
	Assert(!isnull);
	rule_text = DatumGetTextP(rule_datum);
	rule_str = DatumGetCString(DirectFunctionCall1(textout,
					PointerGetDatum(rule_text)));

	oldcxt = MemoryContextSwitchTo(mcxt);
	rule->parrangestart = stringToNode(rule_str);
	MemoryContextSwitchTo(oldcxt);

	pfree(rule_str);

	/* end range */
	rule_datum = heap_getattr(tuple,
							  Anum_pg_partition_rule_parrangeend,
							  tupdesc,
							  &isnull);
	Assert(!isnull);
	rule_text = DatumGetTextP(rule_datum);
	rule_str = DatumGetCString(DirectFunctionCall1(textout,
					PointerGetDatum(rule_text)));

	oldcxt = MemoryContextSwitchTo(mcxt);
	rule->parrangeend = stringToNode(rule_str);
	MemoryContextSwitchTo(oldcxt);

	pfree(rule_str);

	/* every */
	rule_datum = heap_getattr(tuple,
							  Anum_pg_partition_rule_parrangeevery,
							  tupdesc,
							  &isnull);
	Assert(!isnull);
	rule_text = DatumGetTextP(rule_datum);
	rule_str = DatumGetCString(DirectFunctionCall1(textout,
					PointerGetDatum(rule_text)));

	oldcxt = MemoryContextSwitchTo(mcxt);
	rule->parrangeevery = stringToNode(rule_str);
	MemoryContextSwitchTo(oldcxt);

	/* list values */
	rule_datum = heap_getattr(tuple,
							  Anum_pg_partition_rule_parlistvalues,
							  tupdesc,
							  &isnull);
	Assert(!isnull);
	rule_text = DatumGetTextP(rule_datum);
	rule_str = DatumGetCString(DirectFunctionCall1(textout,
					PointerGetDatum(rule_text)));

	oldcxt = MemoryContextSwitchTo(mcxt);
	rule->parlistvalues = stringToNode(rule_str);
	MemoryContextSwitchTo(oldcxt);

	pfree(rule_str);

	if (rule->parlistvalues)
		Assert(IsA(rule->parlistvalues, List));

	rule_datum = heap_getattr(tuple,
							  Anum_pg_partition_rule_parreloptions,
							  tupdesc,
							  &isnull);

	if (isnull)
		rule->parreloptions = NIL;
	else
	{
		ArrayType  *array = DatumGetArrayTypeP(rule_datum);
		Datum	   *options;
		int			noptions;
		List	   *opts = NIL;
		int i;

		/* XXX XXX: why not use untransformRelOptions ? */

		Assert(ARR_ELEMTYPE(array) == TEXTOID);

		deconstruct_array(array, TEXTOID, -1, false, 'i',
						  &options, NULL, &noptions);

		/* key/value pairs for storage clause */
		for (i = 0; i < noptions; i++)
		{
			char *n = DatumGetCString(DirectFunctionCall1(textout, options[i]));
			Value *v = NULL;
			char *s;

			s = strchr(n, '=');

			if (s)
			{
				*s = '\0';
				s++;
				if (*s)
					v = makeString(s);
			}

			opts = lappend(opts, makeDefElem(n, (Node *)v));
		}
		rule->parreloptions = opts;

	}

	rule_datum = heap_getattr(tuple,
							  Anum_pg_partition_rule_partemplatespace,
							  tupdesc,
							  &isnull);
	if (isnull)
		rule->partemplatespaceId = InvalidOid;
	else
		rule->partemplatespaceId = DatumGetObjectId(rule_datum);

	return rule;
}

/*
 * Construct a Partition node from a pg_partition tuple and its description.
 * Result is in the given memory context.
 */
Partition *
partMakePartition(HeapTuple tuple, TupleDesc tupdesc, MemoryContext mcxt)
{
	oidvector *oids;
	int2vector *atts;
	bool isnull;
	Form_pg_partition partrow = (Form_pg_partition)GETSTRUCT(tuple);
	MemoryContext oldcxt;
	Partition *p;

	oldcxt = MemoryContextSwitchTo(mcxt);
	p = makeNode(Partition);
	MemoryContextSwitchTo(oldcxt);

	p->partid = HeapTupleGetOid(tuple);
	p->parrelid = partrow->parrelid;
	p->parkind = partrow->parkind;
	p->parlevel = partrow->parlevel;
	p->paristemplate = partrow->paristemplate;
	p->parnatts = partrow->parnatts;

	atts = DatumGetPointer(heap_getattr(tuple, Anum_pg_partition_paratts,
										tupdesc, &isnull));
	Assert(!isnull);
	oids = DatumGetPointer(heap_getattr(tuple, Anum_pg_partition_parclass,
										tupdesc, &isnull));
	Assert(!isnull);

	oldcxt = MemoryContextSwitchTo(mcxt);
	p->paratts = palloc(sizeof(int2) * p->parnatts);
	p->parclass = palloc(sizeof(Oid) * p->parnatts);
	MemoryContextSwitchTo(oldcxt);

	memcpy(p->paratts, atts->values, sizeof(int2) * p->parnatts);
	memcpy(p->parclass, oids->values, sizeof(Oid) * p->parnatts);

	return p;
}

/*
 * Construct a PartitionNode-PartitionRule tree for the given part. 
 * Recurs to contruct branches.  Note that the ParitionRule (and,
 * hence, the Oid) of the given part itself is not included in the
 * result.
 *
 *    relid			--	pg_class.oid of the partitioned table
 *    level			--	partitioning level
 *    parent		--	pg_partition_rule.oid of the parent of 
 *    inctemplate	--	should the tree include template rules?
 */
PartitionNode *
get_parts(Oid relid, int2 level, Oid parent, bool inctemplate,
		  MemoryContext mcxt)
{
	PartitionNode *pnode = NULL;
	MemoryContext oldcxt;
	ScanKeyData key[3];
	HeapTuple tuple;
	Relation rel;
	SysScanDesc scan;
	List *rules = NIL;
	
	/* Though pg_partition and  pg_partition_rule are populated only
	 * on the entry database, we accept calls from QEs running on a
	 * segment database, but always return NULL; so our result is
	 * only meaningful on the entry database. */
	if (Gp_segment != -1)
		return pnode;
	
	/* select oid as partid, * 
	 * from pg_partition
	 * where 
	 *     parrelid = :relid and 
	 *     parlevel = :level and 
	 *     par_istemplate = :inctemplate;
	 */
	rel = heap_open(PartitionRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_partition_parrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_parlevel,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(level));
	ScanKeyInit(&key[2],
				Anum_pg_partition_paristemplate,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(inctemplate));

	scan = systable_beginscan(rel,
							  PartitionParrelidParlevelParistemplateIndexId,
							  true, SnapshotNow, 3, key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
	{
		pnode = makeNode(PartitionNode);
		pnode->part = partMakePartition(tuple, RelationGetDescr(rel), mcxt);
	}

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);
	
	if ( ! pnode )
		return pnode;

	/* select * 
	 * from pg_partition_rule 
	 * where 
	 *     paroid = :pnode->part->partid and -- pg_partition.oid
	 *     parparentrule = :parent;
	 */
	rel = heap_open(PartitionRuleRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_partition_rule_paroid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pnode->part->partid));
	ScanKeyInit(&key[1],
				Anum_pg_partition_rule_parparentrule,
				BTEqualStrategyNumber, F_INT2EQ,
				ObjectIdGetDatum(parent));

	scan = systable_beginscan(rel, 
						  PartitionRuleParoidParparentruleParruleordIndexId,
							  true, SnapshotNow, 2, key);

	while ((tuple = systable_getnext(scan)))
	{
		PartitionRule *rule;

		rule = ruleMakePartitionRule(tuple, RelationGetDescr(rel), mcxt);
		rule->children = get_parts(relid, level + 1, rule->parruleid,
								   inctemplate, mcxt);

		if (rule->parisdefault)
			pnode->default_part = rule;
		else
		{
			oldcxt = MemoryContextSwitchTo(mcxt);
			rules = lappend(rules, rule);
			MemoryContextSwitchTo(oldcxt);
		}
	}
	/* NOTE: this assert is valid, except for the case of splitting
	 * the very last partition of a table.  For that case, we must
	 * drop the last partition before re-adding the new pieces, which
	 * violates this invariant
	 */
/*	Assert(inctemplate || list_length(rules) || pnode->default_part); */
	pnode->rules = rules;

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);
	return pnode;
}

PartitionNode *
RelationBuildPartitionDesc(Relation rel, bool inctemplate)
{
	PartitionNode *n;
	MemoryContext mcxt = CurrentMemoryContext;

	n = get_parts(RelationGetRelid(rel), 0, 0, inctemplate, mcxt);

	return n;
}

/*
 * Return a list of partition attributes. Order is not guaranteed.
 * Caller must free the result.
 */
List *
get_partition_attrs(PartitionNode *pn)
{
	List *attrs = NIL;
	int i;

	if (!pn)
		return NIL;

	for (i = 0; i < pn->part->parnatts; i++)
		attrs = lappend_int(attrs, pn->part->paratts[i]);

	/* We don't want duplicates, do just go down a single branch */
	if (list_length(pn->rules))
	{
		PartitionRule *rule = linitial(pn->rules);

		return list_concat_unique_int(attrs, get_partition_attrs(rule->children));
	}
	else
		return attrs;
}

void
partition_get_policies_attrs(PartitionNode *pn, GpPolicy *master_policy,
							 List **cols)
{
	if (!pn)
		return;
	else
	{
		ListCell *lc;

		/*
		 * We use master_policy as a fast path. The assumption is that most
		 * child partitions look like the master so we don't want to enter
		 * the O(N^2) loop below if we can avoid it. Firstly, though, we must
		 * copy the master policy into the list.
		 */
		if (*cols == NIL && master_policy->nattrs)
		{
			int attno;

			for (attno = 0; attno < master_policy->nattrs; attno++)
				*cols = lappend_int(*cols, master_policy->attrs[attno]);
		}

		foreach(lc, pn->rules)
		{
			PartitionRule *rule = lfirst(lc);
			Relation rel = heap_open(rule->parchildrelid, NoLock);

			if (master_policy->nattrs != rel->rd_cdbpolicy->nattrs ||
				memcmp(master_policy->attrs, rel->rd_cdbpolicy->attrs,
					   (master_policy->nattrs * sizeof(AttrNumber))))
			{
				int attno;

				for (attno = 0; attno < rel->rd_cdbpolicy->nattrs; attno++)
				{
					if (!list_member_int(*cols,
										 rel->rd_cdbpolicy->attrs[attno]))
						*cols = lappend_int(*cols,
											rel->rd_cdbpolicy->attrs[attno]);
				}
			}
			heap_close(rel, NoLock);

			partition_get_policies_attrs(rule->children, master_policy,
										 cols);
		}
	}
}

bool
partition_policies_equal(GpPolicy *p, PartitionNode *pn)
{
	if (!pn)
		return true;

	if (pn->rules)
	{
		ListCell *lc;

		foreach(lc, pn->rules)
		{
			PartitionRule *rule = lfirst(lc);
			Relation rel = heap_open(rule->parchildrelid, NoLock);

			if (p->nattrs != rel->rd_cdbpolicy->nattrs)
			{
				heap_close(rel, NoLock);
				return false;
			}
			else
			{
				if (p->attrs == 0)
					/* random policy, skip */
					;
				if (memcmp(p->attrs, rel->rd_cdbpolicy->attrs,
						   (sizeof(AttrNumber) * p->nattrs)))
				{
					heap_close(rel, NoLock);
					return false;
				}
			}
			if (!partition_policies_equal(p, rule->children))
			{
				heap_close(rel, NoLock);
				return false;
			}
			heap_close(rel, NoLock);
		}
	}
	return true;
}

AttrNumber
max_partition_attr(PartitionNode *pn)
{
	AttrNumber n = 0;
	List *l = get_partition_attrs(pn);

	if (l)
	{
		ListCell *lc;

		foreach(lc, l)
		{
			AttrNumber att = lfirst_int(lc);

			n = Max(att, n);
		}
		pfree(l);
	}
	return n;
}

int
num_partition_levels(PartitionNode *pn)
{
	PartitionNode *tmp;
	int level = 0;

	tmp = pn;

	/* just descend one branch of the tree until we hit a leaf */
	while (tmp)
	{
		level++;
		if (tmp->rules)
		{
			PartitionRule *rule = linitial(tmp->rules);
			tmp = rule->children;
		}
		else if (tmp->default_part)
		{
			PartitionRule *rule = tmp->default_part;
			tmp = rule->children;
		}
		else
			tmp = NULL;
	}

	return level;
}

/* Return the pg_class Oids of the relations representing parts of the
 * PartitionNode tree headed by the argument PartitionNode.
 */
List *
all_partition_relids(PartitionNode *pn)
{
	if (!pn)
		return NIL;
	else
	{
		ListCell *lc;
		List *out = NIL;

		foreach(lc, pn->rules)
		{
			PartitionRule *rule = lfirst(lc);

			Assert(OidIsValid(rule->parchildrelid));
			out = lappend_oid(out, rule->parchildrelid);

			out = list_concat(out, all_partition_relids(rule->children));
		}
		if (pn->default_part)
		{
			out = lappend_oid(out, pn->default_part->parchildrelid);
			out = list_concat(out,
							  all_partition_relids(pn->default_part->children));
		}
		return out;
	}
}

/* Return the pg_class Oids of the relations representing the parts
 * of the PartitionRule tree headed by the argument PartitionRule.
 *
 * This local function is similar to all_partition_relids but enters
 * at a PartitionRule which is more convenient in some cases, e.g.,
 * on the topRule of a PgPartRule.
 */
static List *
all_prule_relids(PartitionRule *prule)
{
	ListCell *lcr;
	PartitionNode *pnode = NULL;
	
	List *oids = NIL; /* of pg_class Oid */
	
	if ( prule )
	{
		oids = lappend_oid(oids, prule->parchildrelid);
		
		pnode = prule->children;
		if ( pnode )
		{
			oids = list_concat(oids, all_prule_relids(pnode->default_part));
			foreach (lcr, pnode->rules)
			{
				PartitionRule *child = (PartitionRule*)lfirst(lcr);
				oids = list_concat(oids, all_prule_relids(child));
			}
		}
	}
	return oids;
}

/* Is the given relation the top relation of a partitioned table?
 *
 *   exists (select *
 *           from pg_partition
 *           where parrelid = relid)
 *
 * False for interior branches and leaves or when called other
 * then on the entry database, i.e., only meaningful on the
 * entry database.
 */
bool
rel_is_partitioned(Oid relid)
{
	Relation rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple tuple;
	bool found = false;
	
	/* Though pg_partition and pg_partition_rule are only populated
	 * on the entry database, we accept calls from QEs running a
	 * segment, but return false.
	 */
	if (Gp_segment != -1)
		return false;

	rel = heap_open(PartitionRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_partition_parrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, PartitionParrelidIndexId, true,
							  SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
		found = true;

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);
	return found;
}

/* Get the top relation of the partitioned table of which the given
 * relation is a part, or error.
 *
 *   select parrelid
 *   from pg_partition
 *   where oid = (
 *     select paroid
 *     from pg_partition_rule
 *     where parchildrelid = relid);
 */
Oid
rel_partition_get_master(Oid relid)
{
	Relation rel = heap_open(PartitionRuleRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple tuple;
	Oid paroid = InvalidOid;
	Oid masteroid = InvalidOid;
	
	/* pg_partition and  pg_partition_rule are populated only on the 
	 * entry database, so our result is only meaningful there. */
	Insist(Gp_segment == -1);
	
	ScanKeyInit(&key,
				Anum_pg_partition_rule_parchildrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(rel, PartitionRuleParchildrelidIndexId, true,
							  SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
		paroid = ((Form_pg_partition_rule)GETSTRUCT(tuple))->paroid;

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);

	if (!OidIsValid(paroid))
		return InvalidOid;

	rel = heap_open(PartitionRelationId, AccessShareLock);

	ScanKeyInit(&key,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(paroid));

	scan = systable_beginscan(rel, PartitionOidIndexId, true,
							  SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
		masteroid = ((Form_pg_partition)GETSTRUCT(tuple))->parrelid;
	else
		elog(ERROR, "could not find pg_partition entry with oid %d for "
			 "pg_partition_rule with child table %d", paroid, relid);

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);

	return masteroid;

} /* end rel_partition_get_master */

/* given a relid, build a path list from the master tablename down to
 * the partition for that relation, using partition names if possible,
 * else rank or value expressions.
 */
static List *rel_get_part_path1(Oid relid)
{
	Relation	 rel    = heap_open(PartitionRuleRelationId, AccessShareLock);
	ScanKeyData	 key;
	SysScanDesc	 scan;
	HeapTuple	 tuple;
	Oid			 paroid		   = InvalidOid;
	Oid			 parparentrule = InvalidOid;
	List		*lrelid		   = NIL;
	

	/* pg_partition and  pg_partition_rule are populated only on the 
	 * entry database, so our result is only meaningful there. */
	Insist(Gp_segment == -1);
	
	/* use the relid of the table to find the first rule */
	ScanKeyInit(&key,
				Anum_pg_partition_rule_parchildrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(rel, PartitionRuleParchildrelidIndexId, true,
							  SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_partition_rule rule_desc =
				(Form_pg_partition_rule)GETSTRUCT(tuple);

		paroid = rule_desc->paroid;
		parparentrule = rule_desc->parparentrule;

		/* prepend relid of child table to list */
		lrelid = lcons_oid(rule_desc->parchildrelid, lrelid);

	}

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);

	if (!OidIsValid(paroid))
		return NIL;

	/* walk up the tree using the parent rule oid */

	while (OidIsValid(parparentrule))
	{
		rel = heap_open(PartitionRuleRelationId, AccessShareLock);

		ScanKeyInit(&key,
					ObjectIdAttributeNumber,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(parparentrule));
		scan = systable_beginscan(rel, PartitionRuleOidIndexId, true,
								  SnapshotNow, 1, &key);

		tuple = systable_getnext(scan);

		if (HeapTupleIsValid(tuple))
		{
			Form_pg_partition_rule rule_desc =
					(Form_pg_partition_rule)GETSTRUCT(tuple);

			paroid = rule_desc->paroid;
			parparentrule = rule_desc->parparentrule;

			/* prepend relid of child table to list */
			lrelid = lcons_oid(rule_desc->parchildrelid, lrelid);
		}
		else
			parparentrule = InvalidOid; /* we are done */

		heap_close(rel, AccessShareLock);
	}

	return lrelid;

} /* end rel_get_part_path1 */

static List *rel_get_part_path(Oid relid)
{
	PartitionNode 		*pNode	   = NULL;
	Partition			*part	   = NULL;
	MemoryContext		 mcxt	   = CurrentMemoryContext;
	List				*lrelid	   = NIL;
	List				*lnamerank = NIL;
	List				*lnrv	   = NIL;
	ListCell			*lc, *lc2;
	Oid					 masteroid = InvalidOid;

	masteroid = rel_partition_get_master(relid);

	if (!OidIsValid(masteroid))
		return NIL;

	/* call the guts of RelationBuildPartitionDesc */
	pNode = get_parts(masteroid, 0, 0, false, mcxt);

	if (!pNode)
		return NIL;

	part = pNode->part;

	/* get the relids for each table that corresponds to the partition
	 * heirarchy from the master to the specified partition
	 */
	lrelid = rel_get_part_path1(relid);

	/* walk the partition tree, finding the partition for each relid,
	 * and extract useful information (name, rank, value)
	 */
	foreach(lc, lrelid)
	{
		Oid parrelid = lfirst_oid(lc);
		PartitionRule *prule;
		int rulerank = 1;

		Assert(pNode);

		part = pNode->part;

		rulerank = 1;

		foreach (lc2, pNode->rules)
		{
			prule = (PartitionRule *)lfirst(lc2);

			if (parrelid == prule->parchildrelid)
			{
				pNode = prule->children;
				goto L_rel_get_part_path_match;
			}
			rulerank++;
		}

		/* if we get here, then must match the default partition */
		prule = pNode->default_part;

		Assert(parrelid == prule->parchildrelid);

		/* default partition must have a name (and no rank) */
		Assert (prule->parname && strlen(prule->parname));

		pNode = prule->children;
		rulerank = 0;

	L_rel_get_part_path_match:

		if (!rulerank) /* must be default, so it has a name, but no
						* rank or value */
		{
			lnrv = list_make3(prule->parname, NULL, NULL);
		}
		else if (part->parkind == 'l') /* list partition by value */
		{
			char				*idval	= NULL;
			ListCell			*lc3;
			List				*l1		= (List *)prule->parlistvalues;
			StringInfoData       sid1;
			int2				 nkeys  = part->parnatts;
			int2				 parcol = 0;

			initStringInfo(&sid1);

/*			foreach(lc3, l1) */
			/* don't loop -- just need first set of values */

			lc3 = list_head(l1);

			if (lc3)
			{
				List		*vals = lfirst(lc3);
				ListCell	*lcv  = list_head(vals);

				/* Note: similar code in
				 * ruleutils.c:partition_rule_def_worker
				 */

				for (parcol = 0; parcol < nkeys; parcol++)
				{
					Const *con = lfirst(lcv);

					if (lcv != list_head(vals))
							appendStringInfoString(&sid1, ", ");

					idval =
							deparse_expression((Node*)con,
							   deparse_context_for(get_rel_name(relid),
												   relid),
											   false, false);

					appendStringInfo(&sid1, "%s", idval);

					lcv = lnext(lcv);
				} /* end for parcol */
			}
			/* list - no rank */
			lnrv = list_make3(prule->parname, NULL, sid1.data);
		}
		else /* range (or hash) - use rank (though rank is not really
			  * appropriate for hash)
			  */
		{
			char *rtxt = palloc(NAMEDATALEN);

			sprintf(rtxt, "%d", rulerank);

			/* not list - rank, but no value */
			lnrv = list_make3(prule->parname, rtxt, NULL);
		}

		/* build the list of (lists of name, rank, value) for each level */
		lnamerank = lappend(lnamerank, lnrv);

	} /* end foreach lc (walking list of relids) */

	return lnamerank;
} /* end rel_get_part_path */

static char *rel_get_part_path_pretty(Oid relid,
									  char *separator,
									  char *lastsep)
{
	List				*lnamerank = NIL;
	List				*lnrv	   = NIL;
	ListCell			*lc, *lc2;
	int maxlen;
	StringInfoData      	 sid1, sid2;

	lnamerank = rel_get_part_path(relid);

	maxlen = list_length(lnamerank);

	if (!maxlen)
		return NULL;

	Assert(separator);
	Assert(lastsep);

	initStringInfo(&sid1);
	initStringInfo(&sid2);

	foreach(lc, lnamerank)
	{
		int lcnt = 0;

		lnrv = (List *)lfirst(lc);

		maxlen--;

		appendStringInfo(&sid1, "%s", maxlen ? separator : lastsep);

		lcnt = 0;

		foreach (lc2, lnrv)
		{
			char *str = (char *)lfirst(lc2);

			truncateStringInfo(&sid2, 0);

			switch(lcnt)
			{
				case 0:
					if (str && strlen(str))
					{
						appendStringInfo(&sid2, "\"%s\"", str);
						goto l_pretty;
					}
					break;

				case 1:
					if (str && strlen(str))
					{
						appendStringInfo(&sid2, "FOR(RANK(%s))", str);
						goto l_pretty;
					}
					break;

				case 2:
					if (str && strlen(str))
					{
						appendStringInfo(&sid2, "FOR(%s)", str);
						goto l_pretty;
					}
					break;
				default:
					break;
			}
			lcnt++;
		}

		l_pretty:

		appendStringInfo(&sid1, "%s", sid2.data);
	}

	return sid1.data;
} /* end rel_get_part_path_pretty */


/*
 * rel could be a toast table, toast table index or index on a
 * table. Get that table's OID.
 */
static Oid
rel_get_table_oid(Relation rel)
{
	Oid toid = RelationGetRelid(rel);
	Relation thisrel = NULL;

	if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		HeapTuple tuple = SearchSysCache(INDEXRELID, ObjectIdGetDatum(toid),
										 0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failure: cannot find pg_index entry for OID %u",
				 toid);
		toid = ((Form_pg_index)GETSTRUCT(tuple))->indrelid;
		ReleaseSysCache(tuple);

		thisrel = relation_open(toid, NoLock);
		toid = rel_get_table_oid(thisrel);
		relation_close(thisrel, NoLock);
		return toid;
	}
	else if (rel->rd_rel->relkind == RELKIND_AOSEGMENTS ||
			 rel->rd_rel->relkind == RELKIND_AOBLOCKDIR ||
			 rel->rd_rel->relkind == RELKIND_TOASTVALUE)
	{
		/* use pg_depend to find parent */
		ScanKeyData key[2];
		HeapTuple tup;
		SysScanDesc scan;
		Relation depRel = heap_open(DependRelationId, AccessShareLock);

		ScanKeyInit(&key[0],
					Anum_pg_depend_classid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_depend_objid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(toid));

		/*
		 * We use SnapshotAny because the ordering of the dependency code means
		 * that some times we've already deleted the pg_depend tuple. So, we do
		 * an extra test below to see that, if this tuple is deleted, it was
		 * done so by our xid, otherwise we overlook it.
		 */
		scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  SnapshotAny, 2, key);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);
			HeapTupleHeader htup = tup->t_data;

			if (!TransactionIdIsNormal(HeapTupleHeaderGetXmax(htup)) ||
				TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(htup)))
			{
				if (foundDep->deptype == DEPENDENCY_INTERNAL)
				{
					toid = foundDep->refobjid;
					break;
				}
			}
		}
		systable_endscan(scan);
		heap_close(depRel, AccessShareLock);
	}
	return toid;
}

/*
 * partition children, toast tables and indexes, and indexes on partition
 * children do not long lived locks because the lock on the partition master
 * protects us.
 */
bool
rel_needs_long_lock(Oid relid)
{
	bool needs_lock = true;
	Relation rel = relation_open(relid, NoLock);

	relid = rel_get_table_oid(rel);

	relation_close(rel, NoLock);

	if (Gp_role == GP_ROLE_DISPATCH)
		needs_lock = !rel_is_child_partition(relid);
	else
	{
		HeapTuple tup = SearchSysCache(INHRELID, ObjectIdGetDatum(relid),
									   Int32GetDatum(1), 0, 0);
		if (HeapTupleIsValid(tup))
		{
			needs_lock = false;
			ReleaseSysCache(tup);
		}
	}
	return needs_lock;
}

/*
 *
 */
bool
rel_is_child_partition(Oid relid)
{
	Relation rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple tuple;
	bool found;
	
	/* Though pg_partition and  pg_partition_rule are populated only on the 
	 * entry database, are some unguarded calles that may come from segments,
	 * so we return false, even though we don't actually know. */
	if (Gp_segment != -1)
		return false;
	
	rel = heap_open(PartitionRuleRelationId, AccessShareLock);
	
	ScanKeyInit(&key,
				Anum_pg_partition_rule_parchildrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(rel, PartitionRuleParchildrelidIndexId, true,
							  SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	found = HeapTupleIsValid(tuple);

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);

	return found;
}

/*
 * Is relid a leaf node of a partitioning hierarchy? If no, it does not follow
 * that it is the root.
 *
 * To determine if it is a leaf or not, we need to find the depth of our
 * partition and compare this to the maximum depth of the partition set itself.
 * If they're equal, we have a leaf, otherwise, something else.
 */
bool
rel_is_leaf_partition(Oid relid)
{
	Relation rel = heap_open(PartitionRuleRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple tuple;
	Oid paroid = InvalidOid;
	int maxdepth = 0;
	int mylevel = 0;
	Oid partitioned_rel = InvalidOid; /* OID of the root table of the
									   * partition set
									   */
	
	/* pg_partition and  pg_partition_rule are populated only on the 
	 * entry database, so our result is only meaningful there. */
	Insist(Gp_segment == -1);
	
	/*
	 * Find the pg_partition_rule entry to see if this is a child at
	 * all and, if so, to locate the OID for the pg_partition entry.
	 */
	ScanKeyInit(&key,
				Anum_pg_partition_rule_parchildrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(rel, PartitionRuleParchildrelidIndexId, true,
							  SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
		paroid = ((Form_pg_partition_rule)GETSTRUCT(tuple))->paroid;

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);

	if (!OidIsValid(paroid))
		return false;

	rel = heap_open(PartitionRelationId, AccessShareLock);

	/* now look up pg_partitions by OID */
	ScanKeyInit(&key,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(paroid));

	scan = systable_beginscan(rel, PartitionOidIndexId, true,
							  SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	Insist(HeapTupleIsValid(tuple));

	mylevel = ((Form_pg_partition)GETSTRUCT(tuple))->parlevel;
	partitioned_rel = ((Form_pg_partition)GETSTRUCT(tuple))->parrelid;
	systable_endscan(scan);

	/* We have the depth, now we must lookup how deep the partition set is */
	ScanKeyInit(&key,
				Anum_pg_partition_parrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partitioned_rel));

	scan = systable_beginscan(rel, PartitionParrelidIndexId,
							  true, SnapshotNow, 1, &key);

	/*
	 * Of course, we could just maxdepth++ but this seems safer -- we
	 * don't have to worry about the starting depth being 0, 1 or
	 * something else.
	 */
	while ((tuple = systable_getnext(scan)))
	{
		/* not interested in templates */
		if (((Form_pg_partition)GETSTRUCT(tuple))->paristemplate == false)
		{
			int depth = ((Form_pg_partition)GETSTRUCT(tuple))->parlevel;
			maxdepth = Max(maxdepth, depth);
		}
	}

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);
	return maxdepth == mylevel;
}

static Oid
get_opfuncid_by_opname(List *opname, Oid lhsid, Oid rhsid)
{
	Oid opfuncid;
	Operator op;

	op = oper(NULL, opname, lhsid, rhsid, false, -1);

	if (op == NULL)	  /* should not fail */
		elog(ERROR, "could not find operator");

	opfuncid = ((Form_pg_operator)GETSTRUCT(op))->oprcode;

	ReleaseSysCache(op);
	return opfuncid;
}

/*
 * ChoosePartitionName: given a table name, partition "depth", and a
 * partition name, generate a unique name using ChooseRelationName.
 * The partition depth is the raw parlevel (zero-based), which is
 * incremented by 1 to be one-based.
 *
 * Note: calls CommandCounterIncrement.
 *
 * Returns a palloc'd string (from ChooseRelationName)
 */
static char *
ChoosePartitionName(const char *tablename, int partDepth,
					const char *partname, Oid namespaceId)
{
	char  	*relname;
	char	 depthstr[NAMEDATALEN];
	char     prtstr[NAMEDATALEN];

	/* build a relation name (see transformPartitionBy */
	snprintf(depthstr, sizeof(depthstr), "%d", partDepth+1);
	snprintf(prtstr, sizeof(prtstr), "prt_%s", partname);

	relname = ChooseRelationName(tablename,
								 depthstr, /* depth */
								 prtstr, 	/* part spec */
								 namespaceId,
								 NULL);
	CommandCounterIncrement();

	return relname;
}

/*
 * Given a constant expression, build a datum according to
 * part->paratts and the relation tupledesc.  Needs work for
 * type_coercion, multicol, etc.  The returned Datum * is suitable for
 * use in SelectPartition
 */
static Datum *
magic_expr_to_datum(Relation rel, PartitionNode *partnode,
					Node *expr, bool **ppisnull)
{
	Partition  *part = partnode->part;
	TupleDesc	tupleDesc;
	Datum	   *values;
	bool	   *isnull;
	int ii, jj;

	Assert(rel);

	tupleDesc = RelationGetDescr(rel);

	/* Preallocate values/isnull arrays */
	ii = tupleDesc->natts;
	values = (Datum *) palloc(ii * sizeof(Datum));
	isnull = (bool *) palloc(ii * sizeof(bool));
	memset(values, 0, ii * sizeof(Datum));
	memset(isnull, true, ii * sizeof(bool));

	*ppisnull = isnull;

	Assert (IsA(expr, List));

	jj = list_length((List *)expr);

	if (jj > ii)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("too many columns in boundary specification (%d > %d)",
						jj, ii)));

	if (jj > part->parnatts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("too many columns in boundary specification (%d > %d)",
						jj, part->parnatts)));

	{
		ListCell   *lc;
		int i = 0;

		foreach(lc, (List *)expr)
		{
			Node *n1 = (Node *) lfirst(lc);
			Const *c1;
			AttrNumber attno = part->paratts[i++];
			Form_pg_attribute attribute = tupleDesc->attrs[attno - 1];
			Oid lhsid = attribute->atttypid;

			if (!IsA(n1, Const))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("Not a constant expression")));

			c1 = (Const *)n1;

			if (lhsid != c1->consttype)
			{
				/* see coerce_partition_value */
				Node *out;

				out = coerce_partition_value(n1, lhsid, attribute->atttypmod,
											 char_to_parttype(partnode->part->parkind));
				if (!out)
					ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot coerce column type (%s versus %s)",
								format_type_be(c1->consttype),
								format_type_be(lhsid))));

				Assert(IsA(out, Const));

				c1 = (Const *)out;
			}

			/* XXX: cache */
			values[attno - 1] = c1->constvalue;
			isnull[attno - 1] = c1->constisnull;
		}
	}

	return values;
} /* end magic_expr_to_datum */

/*
 * Assume the partition rules are in the "correct" order and return
 * the nth rule (1-based).  If rnk is negative start from the end, ie
 * -1 is the last rule.
 */
static Oid
selectPartitionByRank(PartitionNode *partnode, int rnk)
{
	Oid   relid = InvalidOid;
	List *rules = partnode->rules;
	PartitionRule *rule;

	Assert(partnode->part->parkind == 'r');

	if (rnk > list_length(rules))
		return relid;

	if (rnk == 0)
		return relid;

	if (rnk > 0)
		rnk--; /* list_nth is zero-based, not one-based */
	else if (rnk < 0)
	{
		rnk = list_length(rules) + rnk; /* if negative go from end */

		/* mpp-3265 */
		if (rnk < 0) /* oops -- too negative */
			return relid;
	}

	rule = (PartitionRule *)list_nth(rules, rnk);

	return rule->parchildrelid;
} /* end selectPartitionByRank */

static bool compare_partn_opfuncid(PartitionNode *partnode,
								   char *pub, char *compare_op,
								   List *colvals,
								   Datum *values, bool *isnull,
								   TupleDesc tupdesc)

{
	Partition 	*part 		 = partnode->part;
	List 		*last_opname = list_make2(makeString(pub),
										  makeString(compare_op));
	List 		*opname 	 = NIL;
	ListCell 	*lc;
	int			 numCols  	 = 0;
	int			 colCnt   	 = 0;
	int  		 ii 		 = 0;

	if (1 == strlen(compare_op))
	{
		/* handle case of less than or greater than */
		if (0 == strcmp("<", compare_op))
			compare_op = "<=";
		if (0 == strcmp(">", compare_op))
			compare_op = ">=";

		/* for a list of values, when performing less than or greater
		 * than comparison, only the final value is compared using
		 * less than or greater.  All prior values must be compared
		 * with LTE/GTE.  For example, comparing the list (1,2,3) to
		 * see if it is less than (1,2,4), we see that 1 <= 1, 2 <= 2,
		 * and 3 < 4.  So the last_opname is the specified compare_op,
		 * and the prior opnames are LTE or GTE.
		 */

	}

	opname = list_make2(makeString(pub), makeString(compare_op));

	colCnt = numCols = list_length(colvals);

	foreach(lc, colvals)
	{
		Const *c = lfirst(lc);
		AttrNumber attno = part->paratts[ii];

		if (isnull && isnull[attno - 1])
		{
			if (!c->constisnull)
				return false;
		}
		else
		{
			Oid lhsid = tupdesc->attrs[attno - 1]->atttypid;
			Oid rhsid = lhsid;
			Oid opfuncid;
			Datum res;
			Datum d = values[attno - 1];

			if (1 == colCnt)
			{
				opname = last_opname;
			}

			opfuncid = get_opfuncid_by_opname(opname, lhsid, rhsid);
			res = OidFunctionCall2(opfuncid, c->constvalue, d);

			if (!DatumGetBool(res))
				return false;
		}

		ii++;
		colCnt--;
	} /* end foreach */

	return true;
} /* end compare_partn_opfuncid */

static PartitionNode *
selectListPartition(PartitionNode *partnode, Datum *values, bool *isnull,
					TupleDesc tupdesc, PartitionState *pstate, Oid *found)
{
	ListCell *lc;
	Partition *part = partnode->part;
	MemoryContext oldcxt = NULL;
	PartitionListState *ls;

	if (pstate && pstate->amstate[partnode->part->parlevel])
		ls = (PartitionListState *)pstate->amstate[partnode->part->parlevel];
	else
	{
		int natts = partnode->part->parnatts;

		ls = palloc(sizeof(PartitionListState));

		ls->eqfuncs = palloc(sizeof(FmgrInfo) * natts);
		ls->eqinit = palloc0(sizeof(bool) * natts);

		if (pstate)
			pstate->amstate[partnode->part->parlevel] = (void *)ls;
	}

	if (pstate && pstate->part_cxt)
		oldcxt = MemoryContextSwitchTo(pstate->part_cxt);

	*found = InvalidOid;

	/* With LIST, we have no choice at the moment except to be exhaustive */
	foreach(lc, partnode->rules)
	{
		PartitionRule *rule = lfirst(lc);
		List *vals = rule->parlistvalues;
		ListCell *lc2;
		bool matched = false;

		/*
		 * list values are stored in a list of lists to support multi column
		 * partitions.
		 *
		 * At this level, we're processing the list of possible values for the
		 * given rule, for example:
		 *     values(1, 2, 3)
		 *     values((1, '2005-01-01'), (2, '2006-01-01'))
		 *
		 * Each iteraction is one element of the values list. In the first
		 * example, we iterate '1', '2' then '3'. For the second, we iterate
		 * through '(1, '2005-01-01')' then '(2, '2006-01-01')'. It's for that
		 * reason that we need the inner loop.
		 */
		foreach(lc2, vals)
		{
			ListCell *lc3;
			List *colvals = (List *)lfirst(lc2);
			int i = 0;

		   	matched = true; /* prove untrue */

			foreach(lc3, colvals)
			{
				Const *c = lfirst(lc3);
				AttrNumber attno = part->paratts[i];

				if (isnull[attno - 1])
				{
					if (!c->constisnull)
					{
						matched = false;
						break;
					}
				}
				else if (c->constisnull)
				{
					/* constant is null but datum isn't so break */
					matched = false;
					break;
				}
				else
				{
					Datum res;
					Datum d = values[attno - 1];
					FmgrInfo *finfo;

					if (!ls->eqinit[i])
					{
						Oid opfuncid;
						Oid lhsid = tupdesc->attrs[attno - 1]->atttypid;
						Oid rhsid = lhsid;
						List *opname = list_make2(makeString("pg_catalog"),
												  makeString("="));


						opfuncid = get_opfuncid_by_opname(opname, lhsid, rhsid);
						fmgr_info(opfuncid, &(ls->eqfuncs[i]));
						ls->eqinit[i] = true;
					}

					finfo = &(ls->eqfuncs[i]);
					res = FunctionCall2(finfo, d, c->constvalue);

					if (!DatumGetBool(res))
					{
						/* found it */
						matched = false;
						break;
					}
				}
				i++;
			}
			if (matched)
				break;
		}

		if (matched)
		{
			*found = rule->parchildrelid;

			/* go to the next level */
			if (oldcxt)
				MemoryContextSwitchTo(oldcxt);
			return rule->children;
		}
	}

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return NULL;

}

static void
get_range_oper(int keyno, PartitionRangeState *rs, Oid typid, bool is_lt)
{
	Value *str = is_lt ? makeString("<") : makeString("<=");
	Value *pub = makeString("pg_catalog");
	List *opname = list_make2(pub, str);
	Oid funcid;

	funcid = get_opfuncid_by_opname(opname, typid, typid);
	list_free_deep(opname);

	if (is_lt)
	{
		rs->ltinit[keyno] = true;
		fmgr_info(funcid, &(rs->ltfuncs[keyno]));
	}
	else
	{
		rs->leinit[keyno] = true;
		fmgr_info(funcid, &(rs->lefuncs[keyno]));
	}
}

static int
range_test(Datum tupval, Oid typid, PartitionRangeState *rs, int keyno,
		   PartitionRule *rule)
{
	Const *c = NULL;
	FmgrInfo *finfo;
	Datum res;

	Assert(PointerIsValid(rule->parrangestart) ||
		   PointerIsValid(rule->parrangeend));

	/* might be a rule with no START value */
	if (PointerIsValid(rule->parrangestart))
	{
		Assert(IsA(rule->parrangestart, List));
#if NOT_YET
		c = (Const *)list_nth((List *)rule->parrangestart, keyno);
#else
		c = (Const *)linitial((List *)rule->parrangestart);
#endif

		/* is the value in the range? */
		if (rule->parrangestartincl)
		{
			if (!rs->leinit[keyno])
				get_range_oper(keyno, rs, typid, false);

			finfo = &(rs->lefuncs[keyno]);
		}
		else
		{
			if (!rs->ltinit[keyno])
				get_range_oper(keyno, rs, typid, true);

			finfo = &(rs->ltfuncs[keyno]);
		}

		res = FunctionCall2(finfo, c->constvalue, tupval);

		if (!DatumGetBool(res))
			return -1;
	}

	/* There might be no END value */
	if (PointerIsValid(rule->parrangeend))
	{
#if NOT_YET
		c = (Const *)list_nth((List *)rule->parrangeend, keyno);
#else
		c = (Const *)linitial((List *)rule->parrangeend);
#endif

		/* might be in the range, lets see */
		if (rule->parrangeendincl)
		{
			if (!rs->leinit[keyno])
				get_range_oper(keyno, rs, typid, false);

			finfo = &(rs->lefuncs[keyno]);
		}
		else
		{
			if (!rs->ltinit[keyno])
				get_range_oper(keyno, rs, typid, true);

			finfo = &(rs->ltfuncs[keyno]);
		}

		res = FunctionCall2(finfo, tupval, c->constvalue);

		if (!DatumGetBool(res))
			return 1;
	}
	return 0;
}

/*
 * Given a partition specific part, a tuple as represented by d and isnull and
 * a list of rules, return an Oid in *found or the next set of rules.
 */
static PartitionNode *
selectRangePartition(PartitionNode *partnode, Datum *values, bool *isnull,
					 TupleDesc tupdesc, PartitionState *pstate,
					 Oid *found, int *pSearch)
{
	List *rules = partnode->rules;
	int high = list_length(rules) - 1;
	int low = 0;
    int searchpoint = 0;
	int mid = 0;
	bool matched = false;
	PartitionRule *rule = NULL;
	PartitionNode *pNode = NULL;
	PartitionRangeState *rs = NULL;
	MemoryContext oldcxt = NULL;

	Assert(partnode->part->parkind == 'r');

	if (pstate && pstate->amstate[partnode->part->parlevel])
		rs = (PartitionRangeState *)pstate->amstate[partnode->part->parlevel];
	else
	{
		int natts = partnode->part->parnatts;

		/*
		 * We're still in our caller's memory context so
		 * the memory will persist long enough for us.
		 */
		rs = palloc(sizeof(PartitionRangeState));
		rs->lefuncs = palloc(sizeof(FmgrInfo) * natts);
		rs->ltfuncs = palloc(sizeof(FmgrInfo) * natts);
		rs->ltinit = palloc0(sizeof(bool) * natts);
		rs->leinit = palloc0(sizeof(bool) * natts);

		/*
		 * Unrolling the rules into an array currently works for the
		 * top level partition only
		 */
		if (partnode->part->parlevel == 0)
		{
			int i = 0;
			ListCell *lc;

			rs->rules = palloc(sizeof(PartitionRule *) * list_length(rules));

			foreach(lc, rules)
				rs->rules[i++] = (PartitionRule *)lfirst(lc);
		}
		else
			rs->rules = NULL;
	}

	if (pstate && pstate->part_cxt)
		oldcxt = MemoryContextSwitchTo(pstate->part_cxt);

	*found = InvalidOid;

	/*
	 * Use a binary search to try and pin point the region within the set of
	 * rules where the rule is. If the partition is across a single column,
	 * the rule located by the binary search is the only possible candidate.
	 * If the partition is across more than one column, we need to search
	 * sequentially to either side of the rule to see if the match is there.
	 * The reason for this complexity is the nature of find a point within an
	 * interval.
	 *
	 * Consider the following intervals:
	 *
	 * 1. start( 1, 8)   end( 10,  9)
	 * 2. start( 1, 9)   end( 15, 10)
	 * 3. start( 1, 11)  end(100, 12)
	 * 4. start(15, 10)  end( 30, 11)
	 *
	 * If we were to try and find the partition for a tuple (25, 10), using the
	 * binary search for the first element, we'd select partition 3 but
	 * partition 4 is also a candidate. It is only when we look at the second
	 * element that we find the single definitive rule.
	 */
	while (low <= high)
	{
		AttrNumber attno = partnode->part->paratts[0];
		Datum d = values[attno - 1];
		int ret;

		mid = low + (high - low)/2;

		if (rs->rules)
			rule = rs->rules[mid];
		else
			rule = (PartitionRule *)list_nth(rules, mid);

		if (isnull[attno - 1])
		{
			pNode = NULL;
			goto l_fin_range;
		}

		ret = range_test(d, tupdesc->attrs[attno - 1]->atttypid,
						 rs, 0, rule);

		if (ret > 0)
		{
			searchpoint = mid;
			low = mid + 1;
			continue;
		}
		else if (ret < 0)
		{
			high = mid - 1;
			continue;
		}
		else
		{
			matched = true;
			break;
		}
	}

	if (matched)
	{
		int j;

		/* special case, we matched */
		if (partnode->part->parnatts == 1)
		{
			*found = rule->parchildrelid;

			pNode = rule->children;
			goto l_fin_range;
		}

		j = mid;
		do
		{
			int i;
			bool matched = true;
			bool first_fail = false;

			for (i = 0; i < partnode->part->parnatts; i++)
			{
				AttrNumber attno = partnode->part->paratts[i];
				Datum d = values[attno - 1];
				int ret;

				if (j != mid)
					rule = (PartitionRule *)list_nth(rules, j);

				if (isnull[attno - 1])
				{
					pNode = NULL;
					goto l_fin_range;
				}

				ret = range_test(d, tupdesc->attrs[attno - 1]->atttypid,
								 rs, i, rule);
				if (ret != 0)
				{
					matched = false;

					/*
					 * If we've gone beyond the boundary of rules which match
					 * the first tuple, no use looking further
					 */
					if (i == 0)
						first_fail = true;

					break;
				}
			}

			if (first_fail)
				break;

			if (matched)
			{
				*found = rule->parchildrelid;

				pNode = rule->children;
				goto l_fin_range;
			}

		}
		while (--j >= 0);

		j = mid;
		do
		{
			int i;
			bool matched = true;
			bool first_fail = false;

			for (i = 0; i < partnode->part->parnatts; i++)
			{
				AttrNumber attno = partnode->part->paratts[i];
				Datum d = values[attno - 1];
				int ret;

				rule = (PartitionRule *)list_nth(rules, j);

				if (isnull[attno - 1])
				{
					pNode = NULL;
					goto l_fin_range;
				}

				ret = range_test(d, tupdesc->attrs[attno - 1]->atttypid,
								 rs, i, rule);
				if (ret != 0)
				{
					matched = false;

					/*
					 * If we've gone beyond the boundary of rules which match
					 * the first tuple, no use looking further
					 */
					if (i == 0)
						first_fail = true;

					break;
				}
			}

			if (first_fail)
				break;
			if (matched)
			{
				*found = rule->parchildrelid;

				pNode = rule->children;
				goto l_fin_range;
			}

		}
		while (++j < list_length(rules));
	} /* end if matched */

	pNode = NULL;

 l_fin_range:
	if (pSearch)
		*pSearch = searchpoint;

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	if (pstate)
		pstate->amstate[partnode->part->parlevel] = (void *)rs;

	return pNode;
} /* end selectrangepartition */

/* select partition via hash */
static PartitionNode *
selectHashPartition(PartitionNode *partnode, Datum *values, bool *isnull,
					TupleDesc tupdesc, PartitionState *pstate, Oid *found)
{
	uint32 hash = 0;
	int i;
	int part;
	PartitionRule *rule;
	MemoryContext oldcxt = NULL;

	if (pstate && pstate->part_cxt)
		oldcxt = MemoryContextSwitchTo(pstate->part_cxt);

	for (i = 0; i < partnode->part->parnatts; i++)
	{
		AttrNumber attnum = partnode->part->paratts[i];

		/* rotate hash left 1 bit at each step */
		hash = (hash << 1) | ((hash & 0x80000000) ? 1 : 0);

		/*
		 * If we found a NULL, just pretend it has a hashcode of 0 (like the
		 * hash join code does.
		 */
		if (isnull[attnum - 1])
			continue;
		else
		{
			Oid opclass = partnode->part->parclass[i];
			Oid hashfunc = get_opclass_proc(opclass, 0, HASHPROC);
			Datum d = values[attnum - 1];
			hash ^= DatumGetUInt32(OidFunctionCall1(hashfunc, d));
		}
	}

	part = hash % list_length(partnode->rules);

	rule = (PartitionRule *)list_nth(partnode->rules, part);

	*found = rule->parchildrelid;

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return rule->children;
}

/*
 * selectPartition1()
 *
 * Given pdata and prules, try and find a suitable partition for the input key.
 * values is an array of datums representing the partitioning key, isnull
 * tells us which of those is NULL. pSearch allows the caller to get the
 * position in the partition range where the key falls (might be hypothetical).
 */
static Oid
selectPartition1(PartitionNode *partnode, Datum *values, bool *isnull,
				TupleDesc tupdesc, PartitionState *pstate,
				 int *pSearch,
				 PartitionNode **ppn_out)
{
	Oid relid = InvalidOid;
	Partition *part = partnode->part;
	PartitionNode *pn = NULL;

	if (ppn_out)
		*ppn_out = NULL;

	/* what kind of partition? */
	switch (part->parkind)
	{
		case 'r': /* range */
			pn = selectRangePartition(partnode, values, isnull, tupdesc,
									  pstate, &relid, pSearch);
			break;
		case 'h': /* hash */
			pn = selectHashPartition(partnode, values, isnull, tupdesc,
									 pstate, &relid);
			break;
		case 'l': /* list */
			pn = selectListPartition(partnode, values, isnull, tupdesc,
									 pstate, &relid);
			break;
		default:
			elog(ERROR, "unrecognized partitioning kind '%c'",
				 part->parkind);
			break;
	}

	if (pn)
	{
		if (ppn_out)
		{
			*ppn_out = pn;
			return relid;
		}
		return selectPartition1(pn, values, isnull, tupdesc, pstate,
								pSearch, ppn_out);
	}
	else
	{
		/* retry a default */
		if (partnode->default_part && !OidIsValid(relid))
		{
			if (partnode->default_part->children)
			{
				if (ppn_out)
				{
					*ppn_out = partnode->default_part->children;

					/* don't return the relid, it is invalid -- return
					 * the relid of the default partition instead
					 */
					return partnode->default_part->parchildrelid;
				}
				return selectPartition1(partnode->default_part->children,
										values, isnull, tupdesc, pstate,
										pSearch, ppn_out);
			}
			else
				return partnode->default_part->parchildrelid;
		}
		else
			return relid;
	}
}

Oid
selectPartition(PartitionNode *partnode, Datum *values, bool *isnull,
				TupleDesc tupdesc, PartitionState *pstate)
{
	return selectPartition1(partnode, values, isnull, tupdesc, pstate,
							NULL, NULL);
}

/*
 * get_part_rule
 *
 * find PartitionNode and the PartitionRule for a partition if it
 * exists.
 *
 * If bExistError is not set, just return the PgPartRule.
 * If bExistError is set, return an error message based upon
 * bMustExist.  That is, if the table does not exist and
 * bMustExist=true then return an error.  Conversely, if the table
 * *does* exist and bMustExist=false then return an error.
 *
 * If pSearch is set, return a ptr to the position where the pid
 * *might* be.  For get_part_rule1, pNode is the position to start at
 * (ie not necessarily at the top).  relname is a char string to
 * describe the current relation/partition for error messages, eg
 * 'relation "foo"' or 'partition "baz" of relation "foo"'.
 *
 */
PgPartRule*
get_part_rule1(Relation rel, AlterPartitionId *pid,
			  bool bExistError, bool bMustExist,
			  MemoryContext mcxt, int *pSearch,
			   PartitionNode 		*pNode,
			   char *relname,
			   PartitionNode 		**ppNode
			   )
{
	char		   namBuf[NAMEDATALEN]; /* the real partition name */

	/* a textual representation of the partition id (for error msgs) */
	char		   partIdStr[(NAMEDATALEN * 2)];

	PgPartRule  		*prule 	   = NULL;

	Oid 			 	 partrelid = InvalidOid;
	int 			 	 idrank    = 0;	/* only set for range partns by rank */

	if (!pid)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("No partition id specified for %s",
						relname),
								   errOmitLocation(true)));

	namBuf[0] = 0;

	/* build the partition "id string" for error messages as a
	 * partition name, value, or rank.
	 *
	 * Later on, if we discover
	 *   (the partition exists) and
	 *   (it has a name)
	 * then we update the partIdStr to the name
	 */
	switch (pid->idtype)
	{
		case AT_AP_IDNone:				/* no ID */
			/* should never happen */
			partIdStr[0] = 0;

			break;
		case AT_AP_IDName:				/* IDentify by Name */
			snprintf(partIdStr, sizeof(partIdStr), " \"%s\"",
					 strVal(pid->partiddef));
			snprintf(namBuf, sizeof(namBuf), "%s",
					 strVal(pid->partiddef));
			break;
		case AT_AP_IDValue:				/* IDentifier FOR Value */
			snprintf(partIdStr, sizeof(partIdStr), " for specified value");
			break;
		case AT_AP_IDRank:				/* IDentifier FOR Rank */
		{
			snprintf(partIdStr, sizeof(partIdStr), " for specified rank");

#ifdef WIN32
#define round(x) (x+0.5)
#endif
			if (IsA(pid->partiddef, Integer))
				idrank = intVal(pid->partiddef);
			else if (IsA(pid->partiddef, Float))
				idrank = floor(floatVal(pid->partiddef));
			else
				Assert(false);

			snprintf(partIdStr, sizeof(partIdStr),
					 " for rank %d",
					 idrank);
		}
			break;
		case AT_AP_ID_oid:				/* IDentifier by oid */
			snprintf(partIdStr, sizeof(partIdStr), " for oid %u",
					 *((Oid *)(pid->partiddef)));
			break;
		case AT_AP_IDDefault:			/* IDentify DEFAULT partition */
			snprintf(partIdStr, sizeof(partIdStr), " for DEFAULT");
			break;
		case AT_AP_IDRule:
			{
				PgPartRule *p = linitial((List *)pid->partiddef);
				snprintf(partIdStr, sizeof(partIdStr), "%s",
						 p->partIdStr);
				return p;
				break;
			}
		default: /* XXX XXX */
			Assert(false);

	}

	if (bExistError && !pNode)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("%s is not partitioned",
						relname),
								   errOmitLocation(true)));

	/* if id is a value or rank, get the relid of the partition if
	 * it exists */
	if (pNode)
	{
		if (pid->idtype == AT_AP_IDValue)
		{
			TupleDesc			 tupledesc = RelationGetDescr(rel);
			bool				*isnull;
			PartitionNode 		*pNode2	   = NULL;
			Datum	*d	= magic_expr_to_datum(rel, pNode,
											  pid->partiddef, &isnull);

			/* MPP-4011: get right pid for FOR(value).  pass a pNode
			 * ptr down to prevent recursion in selectPartition -- we
			 * only want the top-most partition for the value in this
			 * case
			 */
			if (ppNode)
				partrelid = selectPartition1(pNode, d, isnull, tupledesc, NULL,
											 pSearch, ppNode);
			else
				partrelid = selectPartition1(pNode, d, isnull, tupledesc, NULL,
											 pSearch, &pNode2);

			/* build a string rep for the value */
			{
				ParseState 		*pstate = make_parsestate(NULL);
				Node 			*pval 	= (Node *)pid->partiddef;
                char 			*idval 	= NULL;

				pval = (Node *)transformExpressionList(pstate,
													   (List *)pval);

				free_parsestate(&pstate);

				idval =
						deparse_expression(pval,
                           deparse_context_for(RelationGetRelationName(rel),
                                               RelationGetRelid(rel)),
										   false, false);

				if (idval)
					snprintf(partIdStr, sizeof(partIdStr),
							 " for value (%s)",
							 idval);
			}

		}
		else if (pid->idtype == AT_AP_IDRank)
		{
			char *parTypName = "UNKNOWN";

			if (pNode->part->parkind != 'r')
			{
				switch (pNode->part->parkind)
				{
					case 'h': /* hash */
						parTypName = "HASH";
						break;
					case 'l': /* list */
						parTypName = "LIST";
						break;
				} /* end switch */

				ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot find partition by RANK -- "
								"%s is %s partitioned",
								relname,
								parTypName),
										   errOmitLocation(true)));

			}

			partrelid =	selectPartitionByRank(pNode, idrank);
		}
	}

	/* check thru the list of partition rules to match by relid or name */
	if (pNode)
	{
		ListCell *lc;
		int rulerank = 1;

		/* set up the relid for the default partition if necessary */
		if ((pid->idtype == AT_AP_IDDefault)
			&& pNode->default_part)
				partrelid = pNode->default_part->parchildrelid;

		foreach(lc, pNode->rules)
		{
			PartitionRule *rule = lfirst(lc);
			bool foundit = false;

			if ((pid->idtype == AT_AP_IDValue)
				|| (pid->idtype == AT_AP_IDRank))
			{
				if ((partrelid != InvalidOid)
					&& (partrelid == rule->parchildrelid))
				{
					foundit = true;

					if (strlen(rule->parname))
					{
						snprintf(partIdStr, sizeof(partIdStr), " \"%s\"",
								 rule->parname);
						snprintf(namBuf, sizeof(namBuf), "%s",
								 rule->parname);
					}
				}
			}
			else if (pid->idtype == AT_AP_IDName)
			{
				if (0 == strcmp(rule->parname, namBuf))
					foundit = true;
			}

			if (foundit)
			{
				prule = makeNode(PgPartRule);

				prule->pNode = pNode;
				prule->topRule = rule;
				prule->topRuleRank = rulerank; /* 1-based */
				prule->relname = relname;
				break;
			}
			rulerank++;
		} /* end foreach */

		/* if cannot find, check default partition */
		if (!prule && pNode->default_part)
		{
			PartitionRule *rule = pNode->default_part;
			bool foundit = false;

			if ((pid->idtype == AT_AP_IDValue)
				|| (pid->idtype == AT_AP_IDRank)
				|| (pid->idtype == AT_AP_IDDefault))
			{
				if ((partrelid != InvalidOid)
					&& (partrelid == rule->parchildrelid))
				{
					foundit = true;

					if (strlen(rule->parname))
					{
						snprintf(partIdStr, sizeof(partIdStr), " \"%s\"",
								 rule->parname);
						snprintf(namBuf, sizeof(namBuf), "%s",
								 rule->parname);
					}
				}
			}
			else if (pid->idtype == AT_AP_IDName)
			{
				if (0 == strcmp(rule->parname, namBuf))
					foundit = true;
			}

			if (foundit)
			{
				prule = makeNode(PgPartRule);

				prule->pNode = pNode;
				prule->topRule = rule;
				prule->topRuleRank = 0; /* 1-based -- 0 means no rank */
				prule->relname = relname;
			}
		}
	} /* end if pnode */

	/* if the partition exists, set the "id string" in prule and
	 * indicate whether it is the partition name.  The ATPExec
	 * commands will notify users of the "real" name if the original
	 * specification was by value or rank
	 */
	if (prule)
	{
		prule->partIdStr = pstrdup(partIdStr);
		prule->isName = (strlen(namBuf) > 0);
	}

	if (!bExistError)
		goto L_fin_partrule;

	/* MPP-3722: complain if for(value) matches the default partition */
	if ((pid->idtype == AT_AP_IDValue)
		&& prule &&
		(prule->topRule == prule->pNode->default_part))
	{
		if (bMustExist)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("FOR expression matches "
							"DEFAULT partition%s of %s",
							prule->isName ?
							partIdStr : "",
							relname),
					 errhint("FOR expression may only specify "
							 "a non-default partition in this context."),
									   errOmitLocation(true)));

	}

	if (bMustExist && !prule)
	{
		switch (pid->idtype)
		{
			case AT_AP_IDNone:				/* no ID */
				/* should never happen */
				Assert(false);
				break;
			case AT_AP_IDName:				/* IDentify by Name */
			case AT_AP_IDValue:				/* IDentifier FOR Value */
			case AT_AP_IDRank:				/* IDentifier FOR Rank */
			case AT_AP_ID_oid:				/* IDentifier by oid */
				ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("partition%s of %s does not exist",
							partIdStr,
							relname),
									   errOmitLocation(true)));
				break;
			case AT_AP_IDDefault:			/* IDentify DEFAULT partition */
				ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("DEFAULT partition of %s does not exist",
							relname),
									   errOmitLocation(true)));
				break;
			default: /* XXX XXX */
				Assert(false);

		}
	}
	else if (!bMustExist && prule)
	{
		switch (pid->idtype)
		{
			case AT_AP_IDNone:				/* no ID */
				/* should never happen */
				Assert(false);
				break;
			case AT_AP_IDName:				/* IDentify by Name */
			case AT_AP_IDValue:				/* IDentifier FOR Value */
			case AT_AP_IDRank:				/* IDentifier FOR Rank */
			case AT_AP_ID_oid:				/* IDentifier by oid */
				ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("partition%s of %s already exists",
							partIdStr,
							relname),
									   errOmitLocation(true)));
				break;
			case AT_AP_IDDefault:			/* IDentify DEFAULT partition */
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("DEFAULT partition%s of %s already exists",
							prule->isName ?
							partIdStr : "",
							relname),
									   errOmitLocation(true)));
				break;
			default: /* XXX XXX */
				Assert(false);

		}

	}

 L_fin_partrule:

	return prule;
} /* end get_part_rule1 */

PgPartRule *
get_part_rule(Relation rel, AlterPartitionId *pid,
			  bool bExistError, bool bMustExist,
			  MemoryContext mcxt, int *pSearch, bool inctemplate)
{
	PartitionNode 		*pNode 	= NULL;
	PartitionNode 		*pNode2 = NULL;
	char 				relnamBuf[(NAMEDATALEN * 2)];
	char *relname;

	snprintf(relnamBuf, sizeof(relnamBuf), "relation \"%s\"",
			 RelationGetRelationName(rel));

	relname = pstrdup(relnamBuf);

	pNode = RelationBuildPartitionDesc(rel, inctemplate);

	if (pid && ((pid->idtype != AT_AP_IDList)
				&& (pid->idtype != AT_AP_IDRule)))
		return get_part_rule1(rel,
							  pid,
							  bExistError, bMustExist,
							  mcxt, pSearch, pNode, relname, NULL);

	if (!pid)
		return NULL;

	if (pid->idtype == AT_AP_IDRule)
	{
		List 					*l1 	= (List *)pid->partiddef;
		ListCell 				*lc;
		AlterPartitionId 		*pid2 	= NULL;
		PgPartRule* 			 prule2 = NULL;
		StringInfoData      	 sid1, sid2;

		initStringInfo(&sid1);
		initStringInfo(&sid2);

		lc = list_head(l1);
		prule2 = (PgPartRule*) lfirst(lc);
		if (prule2 && prule2->topRule && prule2->topRule->children)
			pNode = prule2->topRule->children;

		truncateStringInfo(&sid1, 0);
		appendStringInfo(&sid1, "%s", prule2->relname);

		lc = lnext(lc);

		pid2 = (AlterPartitionId *)lfirst(lc);

		prule2 = get_part_rule1(rel,
								pid2,
								bExistError, bMustExist,
								mcxt, pSearch, pNode, sid1.data, &pNode2);

		pNode = pNode2;

		if (!pNode)
		{
			if (prule2 && prule2->topRule && prule2->topRule->children)
				pNode = prule2->topRule->children;
		}

		return prule2;
	}

	if (pid->idtype == AT_AP_IDList)
	{
		List 					*l1 	= (List *)pid->partiddef;
		ListCell 				*lc;
		AlterPartitionId 		*pid2 	= NULL;
		PgPartRule* 			 prule2 = NULL;
		StringInfoData      	 sid1, sid2;

		initStringInfo(&sid1);
		initStringInfo(&sid2);
		appendStringInfoString(&sid1, relnamBuf);

		foreach(lc, l1)
		{

			pid2 = (AlterPartitionId *)lfirst(lc);

			prule2 = get_part_rule1(rel,
									pid2,
									bExistError, bMustExist,
									mcxt, pSearch, pNode, sid1.data, &pNode2);

			pNode = pNode2;

			if (!pNode)
			{
				if (prule2 && prule2->topRule && prule2->topRule->children)
						pNode = prule2->topRule->children;
			}

			appendStringInfo(&sid2, "partition%s of %s",
							 prule2->partIdStr, sid1.data);
			truncateStringInfo(&sid1, 0);
			appendStringInfo(&sid1, "%s", sid2.data);
			truncateStringInfo(&sid2, 0);
		} /* end foreach */

		return prule2;
	}
	return NULL;
} /* end get_part_rule */


/*
 * ALTER TABLE ... ADD PARTITION
 *
 */

static 	AlterPartitionId *
wack_pid_relname(AlterPartitionId 		 *pid,
				 PartitionNode  		**ppNode,
				 Relation 				  rel,
				 PgPartRule 			**ppar_prule,
				 char 					**plrelname,
				 char 					 *lRelNameBuf)
{
	AlterPartitionId 	*locPid = pid;	/* local pid if IDRule */

	if (!pid)
		return NULL;

	if (AT_AP_IDRule == locPid->idtype)
	{
		List 		*l1		   = (List *)pid->partiddef;
		ListCell 	*lc;
		PgPartRule 	*par_prule = NULL;

		lc = list_head(l1);
		*ppar_prule = (PgPartRule*) lfirst(lc);

		par_prule = *ppar_prule;

		*plrelname = par_prule->relname;

		if (par_prule && par_prule->topRule && par_prule->topRule->children)
			*ppNode = par_prule->topRule->children;

		lc = lnext(lc);

		locPid = (AlterPartitionId *)lfirst(lc);

		Assert(locPid);
	}
	else
	{
		*ppNode = RelationBuildPartitionDesc(rel, false);

		snprintf(lRelNameBuf, (NAMEDATALEN*2),
					 "relation \"%s\"",
					 RelationGetRelationName(rel));
		*plrelname = lRelNameBuf;
	}

	return locPid;
}

static void
ATPExecPartAdd(AlteredTableInfo *tab,
			   Relation rel,
               AlterPartitionCmd *pc,
			   AlterTableType att)
{
	AlterPartitionId 	*pid 		= (AlterPartitionId *)pc->partid;
	PgPartRule   		*prule 		= NULL;
	PartitionNode  		*pNode 		= NULL;
	char           		*parTypName = NULL;
	char			 	 namBuf[NAMEDATALEN];
	AlterPartitionId 	*locPid 	= pid;	/* local pid if IDRule */
	PgPartRule* 		 par_prule 	= NULL;	/* prule for parent if IDRule */
	char 		 		 lRelNameBuf[(NAMEDATALEN*2)];
	char 				*lrelname   = NULL;
	Node 				*pSubSpec 	= NULL;
	AlterPartitionCmd   *pc2;
	bool				 is_split = false;
	bool				 bSetTemplate = (att == AT_PartSetTemplate);

	pc2 = (AlterPartitionCmd *)pc->arg2;

	if (pc2->partid)
		is_split = true;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	locPid =
			wack_pid_relname(pid,
							 &pNode,
							 rel,
							 &par_prule,
							 &lrelname,
							 lRelNameBuf);

	if (!pNode)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("%s is not partitioned",
						lrelname),
								   errOmitLocation(true)));

	switch (pNode->part->parkind)
	{
		case 'h': /* hash */
			parTypName = "HASH";
			break;
		case 'r': /* range */
			parTypName = "RANGE";
			break;
		case 'l': /* list */
			parTypName = "LIST";
			break;
		default:
			elog(ERROR, "unrecognized partitioning kind '%c'",
				 pNode->part->parkind);
			Assert(false);
			break;
	} /* end switch */


	if (locPid->idtype == AT_AP_IDName)
			snprintf(namBuf, sizeof(namBuf), " \"%s\"",
					 strVal(locPid->partiddef));
	else
			namBuf[0] = '\0';

	if ('h' == pNode->part->parkind)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot add HASH partition%s to %s",
						namBuf,
						lrelname),
				 errhint("recreate the table to increase the "
						 "number of partitions"),
								   errOmitLocation(true)));


	/* partition must have a valid name */
	if ((locPid->idtype != AT_AP_IDName)
		&& (locPid->idtype != AT_AP_IDNone))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot ADD partition%s to %s by rank or value",
						namBuf,
						lrelname),
					 errhint("use a named partition"),
							   errOmitLocation(true)));

	/* don't check if splitting or setting a subpartition template */
	if (!is_split && !bSetTemplate)
		/* We complain if partition already exists, so prule should be NULL */
		prule = get_part_rule(rel, pid, true, false,
							  CurrentMemoryContext, NULL,
							  false);

	if (!prule)
	{
		bool isDefault = intVal(pc->arg1);

		/* DEFAULT checks */
		if (!isDefault && (pNode->default_part) &&
			!is_split && !bSetTemplate) /* MPP-6093: ok to reset template */
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot add %s partition%s to "
						"%s with DEFAULT partition \"%s\"",
						parTypName,
						namBuf,
						lrelname,
						pNode->default_part->parname),
				 errhint("need to SPLIT partition \"%s\"",
						 pNode->default_part->parname),
								   errOmitLocation(true)));

		if (isDefault && !is_split)
		{
			if ('h' == pNode->part->parkind)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot add a DEFAULT partition%s "
								"to %s of type HASH",
								namBuf,
								lrelname),
										   errOmitLocation(true)));

			/* MPP-6093: ok to reset template */
			if (pNode->default_part && !bSetTemplate)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("DEFAULT partition \"%s\" for "
								"%s already exists",
								pNode->default_part->parname,
								lrelname),
										   errOmitLocation(true)));

			/* XXX XXX: move this check to gram.y ? */
			if (pc2->arg1)
			{
				PartitionElem *pelem = (PartitionElem *) pc2->arg1;

				if (pelem->boundSpec)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("invalid use of boundary specification "
									"for DEFAULT partition%s of %s",
									namBuf,
									lrelname),
											   errOmitLocation(true)));
			}
		}

		/* Do the real work for add ... */
		

		if ('r' == pNode->part->parkind)
		{
			pSubSpec =
			atpxPartAddList(tab, rel, pc, pNode,
							pc2->arg2, /* utl statement */
							(locPid->idtype == AT_AP_IDName) ?
							locPid->partiddef : NULL, /* partition name */
							isDefault, (PartitionElem *) pc2->arg1,
							PARTTYP_RANGE,
							par_prule,
							lrelname,
							bSetTemplate,
							rel->rd_rel->relowner);

		}
		else if ('l' == pNode->part->parkind)
		{
			pSubSpec =
			atpxPartAddList(tab, rel, pc, pNode,
							pc2->arg2, /* utl statement */
							(locPid->idtype == AT_AP_IDName) ?
							locPid->partiddef : NULL, /* partition name */
							isDefault, (PartitionElem *) pc2->arg1,
							PARTTYP_LIST,
							par_prule,
							lrelname,
							bSetTemplate,
							rel->rd_rel->relowner);
		}

	}

	/* MPP-6929: metadata tracking */
	if (!is_split && !bSetTemplate)
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "PARTITION", "ADD"
				);

} /* end ATPExecPartAdd */


/*
 * atpxPart_validate_spec: kludge up a PartitionBy statement and use
 * validate_partition_spec to transform and validate a partition
 * boundary spec.
 */

static int
atpxPart_validate_spec(
		PartitionBy 			*pBy,
		CreateStmtContext   	*pcxt,
		Relation 		 		 rel,
		CreateStmt 				*ct,
		PartitionElem 			*pelem,
		PartitionNode 			*pNode,
		Node 					*partName,
		bool 			 		 isDefault,
		PartitionByType  		 part_type,
		char 					*partDesc)
{
	PartitionSpec 		*spec  	= makeNode(PartitionSpec);
	ParseState 			*pstate = NULL;
	List	   			*schema = NIL;
	List	   			*inheritOids;
	List	   			*old_constraints;
	int					 parentOidCount;
	int                  result;
	PartitionNode 		*pNode_tmpl = NULL;

	/* get the table column defs */
	schema =
			MergeAttributes(schema,
							list_make1(
									makeRangeVar(
											get_namespace_name(
													RelationGetNamespace(rel)),
									pstrdup(RelationGetRelationName(rel)), -1)),
							false,
							&inheritOids, &old_constraints, &parentOidCount, NULL);

	pcxt->columns = schema;

	spec->partElem = list_make1(pelem);

	pelem->partName = partName ? copyObject(partName) : NULL;
	pelem->isDefault = isDefault;
	pelem->AddPartDesc = pstrdup(partDesc);

	/* generate a random name for the partition relation if necessary */
	if (partName)
		pelem->rrand = 0;
	else
		pelem->rrand = random();

 	pBy->partType = part_type;
	pBy->keys     = NULL;
	pBy->partNum  = 0;
	pBy->subPart  = NULL;
	pBy->partSpec = (Node *)spec;
	pBy->partDepth = pNode->part->parlevel;
	/* Note: pBy->partQuiet already set by caller */
	pBy->parentRel =
			makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
						 pstrdup(RelationGetRelationName(rel)), -1);
	pBy->location  = -1;
	pBy->partDefault = NULL;
	pBy->bKeepMe = true; /* nefarious: we need to keep the "top"
						  * partition by statement because
						  * analyze.c:do_parse_analyze needs to find
						  * it to re-order the ALTER statements
						  */

	/* fixup the pnode_tmpl to get the right parlevel */
	if (pNode && (pNode->rules || pNode->default_part))
	{
		PartitionRule *prule;

		if (pNode->default_part)
			prule = pNode->default_part;
		else
			prule = linitial(pNode->rules);

		pNode_tmpl = get_parts(
				pNode->part->parrelid,
				pNode->part->parlevel + 1,
				InvalidOid, /* no parent for template */
				true,
				CurrentMemoryContext
				);
	}

	{ /* find the partitioning keys (recursively) */

		PartitionBy 			*pBy2 = pBy;
		PartitionBy 			*parent_pBy2 = NULL;
		PartitionNode 			*pNode2 = pNode;

		int 			 ii;
		TupleDesc		 tupleDesc 		= RelationGetDescr(rel);
		List 			*pbykeys   		= NIL;
		List 			*pbyopclass 	= NIL;
		Oid 			 accessMethodId = BTREE_AM_OID;

		while (pNode2)
		{
			pbykeys    = NIL;
			pbyopclass = NIL;

			for (ii = 0; ii < pNode2->part->parnatts; ii++)
			{
				AttrNumber 		 	 attno 		   =
						pNode2->part->paratts[ii];
				Form_pg_attribute 	 attribute 	   =
						tupleDesc->attrs[attno - 1];
				char	   			*attributeName =
						NameStr(attribute->attname);
				Oid 				 opclass 	   =
						InvalidOid;

				opclass =
						GetDefaultOpClass(attribute->atttypid, accessMethodId);

				if (pbykeys)
				{
					pbykeys = lappend(pbykeys, makeString(attributeName));
					pbyopclass = lappend_oid(pbyopclass, opclass);
				}
				else
				{
					pbykeys = list_make1(makeString(attributeName));
					pbyopclass = list_make1_oid(opclass);
				}
			} /* end for */

			pBy2->keys = pbykeys;
			pBy2->keyopclass = pbyopclass;

			if (parent_pBy2)
				parent_pBy2->subPart = (Node *)pBy2;

			parent_pBy2 = pBy2;

			if (pNode2 && (pNode2->rules || pNode2->default_part))
			{
				PartitionRule *prule;
				PartitionElem *el = NULL; /* for the subpartn template */

				if (pNode2->default_part)
					prule = pNode2->default_part;
				else
					prule = linitial(pNode2->rules);

				if (prule && prule->children)
				{
					pNode2 = prule->children;

					/* XXX XXX: make this work for HASH at some point */

					Assert(
							('l' == pNode2->part->parkind) ||
							('r' == pNode2->part->parkind));

					pBy2 = makeNode(PartitionBy);
					pBy2->partType 	=
							('r' == pNode2->part->parkind) ?
							PARTTYP_RANGE :
							PARTTYP_LIST;
					pBy2->keys     	= NULL;
					pBy2->partNum  	= 0;
					pBy2->subPart  	= NULL;
					pBy2->partSpec 	= NULL;
					pBy2->partDepth = pNode2->part->parlevel;
					pBy2->partQuiet = pBy->partQuiet;
					pBy2->parentRel =
							makeRangeVar(
									get_namespace_name(
											RelationGetNamespace(rel)),
									pstrdup(RelationGetRelationName(rel)), -1);
					pBy2->location  = -1;
					pBy2->partDefault = NULL;

					el = NULL;

					/* build the template (if it exists) */
					if (pNode_tmpl)
					{
						PartitionSpec *spec_tmpl = makeNode(PartitionSpec);
						ListCell *lc;

						spec_tmpl->istemplate = true;

						/* add entries for rules at current level */
						foreach(lc, pNode_tmpl->rules)
						{
							PartitionRule *rule_tmpl = lfirst(lc);

							el = makeNode(PartitionElem);

							if (rule_tmpl->parname &&
								strlen(rule_tmpl->parname))
								el->partName =
										(Node*)makeString(rule_tmpl->parname);

							el->isDefault = rule_tmpl->parisdefault;

							/* MPP-6904: use storage options from template */
							if (rule_tmpl->parreloptions ||
								rule_tmpl->partemplatespaceId)
							{
								Node *tspaceName = NULL;
								AlterPartitionCmd *apc = 
										makeNode(AlterPartitionCmd);

								el->storeAttr = (Node *)apc;

								if (rule_tmpl->partemplatespaceId)
									tspaceName = 
										(Node*)makeString(
												get_tablespace_name(
											rule_tmpl->partemplatespaceId
														));

								apc->partid = NULL;
								apc->arg2	= tspaceName;
								apc->arg1	= (Node *)rule_tmpl->parreloptions;

							}

							/* LIST */
							if (rule_tmpl->parlistvalues)
							{
								PartitionValuesSpec *vspec =
										makeNode(PartitionValuesSpec);

								el->boundSpec = (Node*)vspec;

								vspec->partValues = rule_tmpl->parlistvalues;
							}

							/* RANGE */
							if (rule_tmpl->parrangestart ||
								rule_tmpl->parrangeend)
							{
								PartitionBoundSpec *bspec =
										makeNode(PartitionBoundSpec);
								PartitionRangeItem *ri;

								if (rule_tmpl->parrangestart)
								{
									ri =
										makeNode(PartitionRangeItem);

									ri->partedge =
											rule_tmpl->parrangestartincl ?
											PART_EDGE_INCLUSIVE :
											PART_EDGE_EXCLUSIVE ;

									ri->partRangeVal =
											(List *)rule_tmpl->parrangestart;

									bspec->partStart = (Node*)ri;

								}
								if (rule_tmpl->parrangeend)
								{
									ri =
										makeNode(PartitionRangeItem);

									ri->partedge =
											rule_tmpl->parrangeendincl ?
											PART_EDGE_INCLUSIVE :
											PART_EDGE_EXCLUSIVE ;

									ri->partRangeVal =
											(List *)rule_tmpl->parrangeend;

									bspec->partEnd = (Node*)ri;

								}
								if (rule_tmpl->parrangeevery)
								{
									ri =
										makeNode(PartitionRangeItem);

									ri->partRangeVal =
											(List *)rule_tmpl->parrangeevery;

									bspec->partEvery = (Node*)ri;

								}

								el->boundSpec = (Node*)bspec;

							} /* end if RANGE */

							spec_tmpl->partElem = lappend(spec_tmpl->partElem,
														  el);
						} /* end foreach */

						/* MPP-4725 */
						/* and the default partition */
						if (pNode_tmpl->default_part)
						{
							PartitionRule *rule_tmpl =
									pNode_tmpl->default_part;

							el = makeNode(PartitionElem);

							if (rule_tmpl->parname &&
								strlen(rule_tmpl->parname))
								el->partName =
										(Node*)makeString(rule_tmpl->parname);

							el->isDefault = rule_tmpl->parisdefault;

							spec_tmpl->partElem = lappend(spec_tmpl->partElem,
														  el);

						}

						/* the PartitionElem should hang off the pby
						 * partspec, and subsequent templates should
						 * hang off the subspec for the prior
						 * PartitionElem.
						 */

						pBy2->partSpec 	= (Node *)spec_tmpl;

					} /* end if pNode_tmpl */

					/* fixup the pnode_tmpl to get the right parlevel */
					if (pNode2 && (pNode2->rules || pNode2->default_part))
					{
						PartitionRule *prule66;

						if (pNode2->default_part)
							prule66 = pNode2->default_part;
						else
							prule66 = linitial(pNode2->rules);

						pNode_tmpl = get_parts(
								pNode2->part->parrelid,
								pNode2->part->parlevel + 1,
								InvalidOid, /* no parent for template */
								true,
								CurrentMemoryContext
								);
					}

				}
				else
					pNode2 = NULL;
			}
			else
				pNode2 = NULL;

		} /* end while */
	}

	pstate = make_parsestate(NULL);
	result = validate_partition_spec(pstate, pcxt, ct, pBy, "", -1);
	free_parsestate(&pstate);

	return result;
} /* end atpxPart_validate_spec */

static Node *
atpxPartAddList(AlteredTableInfo *tab,
				Relation rel,
				AlterPartitionCmd *pc,
				PartitionNode  *pNode,
				Node *pUtl,     /* pc2->arg2 */
				Node *partName, /* pid->partiddef (or NULL) */
				bool isDefault,
				PartitionElem *pelem,
				PartitionByType part_type,
				PgPartRule* par_prule,
				char *lrelname,
				bool bSetTemplate,
				Oid ownerid)
{
	CreateStmt 			*ct    	   = NULL;
	DestReceiver 		*dest  	   = None_Receiver;
	int 				 partno    = 0;
	int 				 maxpartno = 0;
	bool 				 bOpenGap  = false;
	PartitionBy 		*pBy   	   = makeNode(PartitionBy);
	CreateStmtContext    cxt;
	List				*utils 	   = (List *)pUtl;
	Node 				*pSubSpec  = NULL;	/* return the subpartition spec */
	Relation 			 par_rel   = rel;
	PartitionNode 		 pNodebuf;
	PartitionNode 		*pNode2 = &pNodebuf;
	AlterPartitionCmd   *pc2 = (AlterPartitionCmd *)pc->arg2;
	bool is_split = PointerIsValid(pc2->partid);

	/* get the relation for the parent of the new partition */
	if (par_prule && par_prule->topRule)
		par_rel =
				heap_open(par_prule->topRule->parchildrelid, AccessShareLock);

	MemSet(&cxt, 0, sizeof(cxt));

	Assert( (PARTTYP_LIST == part_type) || (PARTTYP_RANGE == part_type) );

	Assert(IsA(pUtl, List));

	ct = (CreateStmt *)linitial(utils);
	Assert(IsA(ct, CreateStmt));

	ct->ownerid = ownerid;

	if (!ct->distributedBy)
		ct->distributedBy = make_dist_clause(rel);

	if (bSetTemplate)
		/* if creating a template, silence partition name messages */
		pBy->partQuiet = PART_VERBO_NOPARTNAME;
	else
		/* just silence distribution policy messages */
		pBy->partQuiet = PART_VERBO_NODISTRO;

	/* XXX XXX: handle case of missing boundary spec for range with EVERY */

	if (pelem && pelem->boundSpec)
	{
		if (PARTTYP_RANGE == part_type)
		{
			PartitionBoundSpec 	*pbs   	   = NULL;
			PgPartRule   		*prule 	   = NULL;
			AlterPartitionId 	 pid;
			ParseState 			*pstate    = make_parsestate(NULL);
			TupleDesc 		 	 tupledesc = RelationGetDescr(rel);

			MemSet(&pid, 0, sizeof(AlterPartitionId));

			pid.idtype = AT_AP_IDRank;
			pid.location  = -1;

			Assert (IsA(pelem->boundSpec, PartitionBoundSpec));

			pbs = (PartitionBoundSpec *)pelem->boundSpec;
			pSubSpec = pelem->subSpec; /* look for subpartition spec */

			/* no EVERY */
			if (pbs->partEvery)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("cannot specify EVERY when adding "
								"RANGE partition to %s",
								lrelname),
						 errOmitLocation(true)));

			if (!(pbs->partStart || pbs->partEnd ))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("Need START or END when adding "
								"RANGE partition to %s",
								lrelname),
						 errOmitLocation(true)));

			/* if no START, then START after last partition */

			if (!(pbs->partStart))
			{
				Datum 					*d_end 	   = NULL;
				bool 					*isnull;
				bool 					 bstat;

				pid.partiddef = (Node *)makeInteger(-1);

				prule = get_part_rule1(rel, &pid, false, false,
									   CurrentMemoryContext, NULL,
									   pNode,
									   lrelname,
									   &pNode2);

				/* ok if no prior -- just means this is first
				 * partition (XXX XXX though should always have 1
				 * partition in the table...)
				 */

				if (!(prule && prule->topRule))
				{
					maxpartno = 1;
					bOpenGap = true;
					goto L_fin_no_start;
				}

				{
					Node *n1;
					PartitionRangeItem *ri =
							(PartitionRangeItem *)pbs->partEnd;
					PartitionRangeItemIsValid(NULL, ri);

					n1 = (Node *)copyObject(ri->partRangeVal);
					n1 = (Node *)transformExpressionList(pstate,
														 (List *)n1);

					d_end =
							magic_expr_to_datum(rel, pNode,
												n1, &isnull);
				}

				if (prule && prule->topRule && prule->topRule->parrangeend
					&& list_length((List *)prule->topRule->parrangeend))
				{
					bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "<",
										   (List *)prule->topRule->parrangeend,
												   d_end, isnull, tupledesc);

					/* if the current end is less than the new end
					 * then use it as the start of the new
					 * partition
					 */

					if (bstat)
					{
						PartitionRangeItem *ri = makeNode(PartitionRangeItem);

						ri->location = -1;

						ri->partRangeVal =
								copyObject(prule->topRule->parrangeend);

						/* invert the inclusive/exclusive */
						ri->partedge = prule->topRule->parrangeendincl ?
								PART_EDGE_EXCLUSIVE :
								PART_EDGE_INCLUSIVE;

						/* should be final partition */
						maxpartno = prule->topRule->parruleord + 1;

						pbs->partStart = (Node *)ri;
						goto L_fin_no_start;
					}
				}

				/* if the last partition doesn't have an end, or the
				 * end isn't less than the new end, check if new end
				 * is less than current start
				 */

				pid.partiddef = (Node *)makeInteger(1);

				prule = get_part_rule1(rel, &pid, false, false,
									   CurrentMemoryContext, NULL,
									   pNode,
									   lrelname,
									   &pNode2);

				if (!(prule && prule->topRule && prule->topRule->parrangestart
					  && list_length((List *)prule->topRule->parrangestart)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("new partition overlaps existing "
									"partition"),
											   errOmitLocation(true)));

				bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   ">",
									   (List *)prule->topRule->parrangestart,
											   d_end, isnull, tupledesc);
				if (bstat)
				{
					/* should be first partition */
					maxpartno = prule->topRule->parruleord - 1;
					if (0 == maxpartno)
					{
						maxpartno = 1;
						bOpenGap = true;
					}

				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("new partition overlaps existing "
									"partition"),
											   errOmitLocation(true)));

			L_fin_no_start:
				bstat = false; /* fix warning */

			}
			else if (!(pbs->partEnd))
			{ /* if no END, then END before first partition
			     **ONLY IF**
				 * START of this partition is before first partition ... */

				Datum 					*d_start   = NULL;
				bool 					*isnull;
				bool 					 bstat;

				pid.partiddef = (Node *)makeInteger(1);

				prule = get_part_rule1(rel, &pid, false, false,
									   CurrentMemoryContext, NULL,
									   pNode,
									   lrelname,
									   &pNode2);

				/* NOTE: invert all the logic of case of missing partStart */

				/* ok if no successor [?] -- just means this is first
				 * partition (XXX XXX though should always have 1
				 * partition in the table... [XXX XXX unless did a
				 * SPLIT of a single partition !! ])
				 */

				if (!(prule && prule->topRule))
				{
					maxpartno = 1;
					bOpenGap = true;
					goto L_fin_no_end;
				}

				{
					Node *n1;
					PartitionRangeItem *ri =
							(PartitionRangeItem *)pbs->partStart;

					PartitionRangeItemIsValid(NULL, ri);
					n1 = (Node *)copyObject(ri->partRangeVal);
					n1 = (Node *)transformExpressionList(pstate,
														 (List *)n1);

					d_start =
							magic_expr_to_datum(rel, pNode,
												n1, &isnull);
				}


				if (prule && prule->topRule && prule->topRule->parrangestart
					&& list_length((List *)prule->topRule->parrangestart))
				{
					bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   ">",
									   (List *)prule->topRule->parrangestart,
												   d_start, isnull, tupledesc);

					/* if the current start is greater than the new start
					 * then use the current start as the end of the new
					 * partition
					 */

					if (bstat)
					{
						PartitionRangeItem *ri = makeNode(PartitionRangeItem);

						ri->location = -1;

						ri->partRangeVal =
								copyObject(prule->topRule->parrangestart);

						/* invert the inclusive/exclusive */
						ri->partedge = prule->topRule->parrangestartincl ?
								PART_EDGE_EXCLUSIVE :
								PART_EDGE_INCLUSIVE;

						/* should be first partition */
						maxpartno = prule->topRule->parruleord - 1;
						if (0 == maxpartno)
						{
							maxpartno = 1;
							bOpenGap = true;
						}

						pbs->partEnd = (Node *)ri;
						goto L_fin_no_end;
					}
				}

				/* if the first partition doesn't have an start, or the
				 * start isn't greater than the new start, check if new start
				 * is greater than current end
				 */

				pid.partiddef = (Node *)makeInteger(-1);

				prule = get_part_rule1(rel, &pid, false, false,
									   CurrentMemoryContext, NULL,
									   pNode,
									   lrelname,
									   &pNode2);

				if (!(prule && prule->topRule && prule->topRule->parrangeend
					  && list_length((List *)prule->topRule->parrangeend)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("new partition overlaps existing "
									"partition"),
											   errOmitLocation(true)));

				bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   "<",
									   (List *)prule->topRule->parrangeend,
											   d_start, isnull, tupledesc);
				if (bstat)
				{
					/* should be final partition */
					maxpartno = prule->topRule->parruleord + 1;
				}
				else
				{
					PartitionRangeItem *ri =
							(PartitionRangeItem *)pbs->partStart;

					/* check for equality */
					bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "=",
									   (List *)prule->topRule->parrangeend,
											   d_start, isnull, tupledesc);
					
					/* if new start not >= to current end, then 
					 * new start < current end, so it overlaps. Or if
					 * new start == current end, but the
					 * inclusivity is not opposite for the boundaries
					 * (eg inclusive end abuts inclusive start for
					 * same start/end value) then it overlaps
					 */
					if (!bstat || 
						(bstat && 
						 (prule->topRule->parrangeendincl == 
						  (ri->partedge == PART_EDGE_INCLUSIVE)))
							)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("new partition overlaps existing "
										"partition"),
								 errOmitLocation(true)));

					/* doesn't overlap, should be final partition */
					maxpartno = prule->topRule->parruleord + 1;

				}
			L_fin_no_end:
				bstat = false; /* fix warning */

			}
			else
			{ 					/* both start and end are specified */
				PartitionRangeItem 		*ri;
				bool 					 bOverlap  = false;
				int isMiddle  = 0;	/* -1 for new first, 1 for new last */
				bool 					*isnull;
				int 			 		 startSearchpoint;
				int 			 		 endSearchpoint;
				Datum 					*d_start   = NULL;
				Datum 					*d_end 	   = NULL;

				/* see if start or end overlaps */
				pid.idtype = AT_AP_IDValue;

				/* check the start */

				ri = (PartitionRangeItem *)pbs->partStart;
				PartitionRangeItemIsValid(NULL, ri);

				pid.partiddef = (Node *)copyObject(ri->partRangeVal);
				pid.partiddef =
						(Node *)transformExpressionList(pstate,
														(List *)pid.partiddef);

				prule = get_part_rule1(rel, &pid, false, false,
									   CurrentMemoryContext, &startSearchpoint,
									   pNode,
									   lrelname,
									   &pNode2);

				/* found match for start value in rules */
				if (prule && !(prule->topRule->parisdefault && is_split))
				{
					bool bstat;
					PartitionRule *a_rule = prule->topRule;
					d_start =
							magic_expr_to_datum(rel, pNode,
												pid.partiddef, &isnull);

					/* if start value was inclusive then it definitely
					 * overlaps
					 */
					if (ri->partedge == PART_EDGE_INCLUSIVE)
					{
						bOverlap = true;
						goto L_end_overlap;
					}

					/* not inclusive -- check harder if START really
					 * overlaps
					 */

					if (0 ==
						list_length((List *)a_rule->parrangeend))
					{
						/* infinite end > new start - overlap */
						bOverlap = true;
						goto L_end_overlap;
					}

					bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   ">",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc);
					if (bstat)
					{
						/* end > new start - overlap */
						bOverlap = true;
						goto L_end_overlap;
					}

					/* Must be the case that new start == end of
					 * a_rule (because if the end < new start then how
					 * could we find it in the interval for prule ?)
					 * This is ok if they have opposite
					 * INCLUSIVE/EXCLUSIVE ->  New partition does not
					 * overlap.
					 */

					Assert (compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "=",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc));

					if (a_rule->parrangeendincl == 
						(ri->partedge == PART_EDGE_INCLUSIVE))
					{
						/* start and end must be of opposite
						 * types, else they overlap
						 */
						bOverlap = true;
						goto L_end_overlap;
					}

					/* opposite inclusive/exclusive, so in middle of
					 * range of existing partitions
					 */
					isMiddle = 0;
					goto L_check_end;
				} /* end if prule */

				/* check for basic case of START > last partition */
				if (pNode && pNode->rules && list_length(pNode->rules))
				{
					bool bstat;
					PartitionRule *a_rule = /* get last rule */
						(PartitionRule *)list_nth(pNode->rules,
											  list_length(pNode->rules) - 1);
					d_start =
							magic_expr_to_datum(rel, pNode,
												pid.partiddef, &isnull);

					if (0 ==
						list_length((List *)a_rule->parrangeend))
					{
						/* infinite end > new start */
						bstat = false;
					}
					else
						bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   "<",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc);

					/* if the new partition start > end of the last
					 * partition then it is the new final partition.
					 * Don't bother checking the new end for overlap
					 * (just check if end > start in validation
					 * phase
					 */
					if (bstat)
					{
						isMiddle = 1;

						/* should be final partition */
						maxpartno = a_rule->parruleord + 1;

						goto L_end_overlap;
					}

					/* could be the case that new start == end of
					 * last.  This is ok if they have opposite
					 * INCLUSIVE/EXCLUSIVE.  New partition is still
					 * final partition for this case
					 */

					if (0 ==
						list_length((List *)a_rule->parrangeend))
					{
						/* infinite end > new start */
						bstat = false;
					}
					else
						bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   "=",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc);
					if (bstat)
					{
						if (a_rule->parrangeendincl == 
							(ri->partedge == PART_EDGE_INCLUSIVE))
						{
							/* start and end must be of opposite
							 * types, else they overlap */
							bOverlap = true;
							goto L_end_overlap;
						}

						isMiddle = 1;

						/* should be final partition */
						maxpartno = a_rule->parruleord + 1;

						goto L_end_overlap;
					}
					else
					{
						/* tricky case: the new start is less than the
						 * end of the final partition, but it does not
						 * intersect any existing partitions.  So we
						 * are trying to add a partition in the middle
						 * of the existing partitions or before the
						 * first partition.
						 */
						a_rule = /* get first rule */
								(PartitionRule *)list_nth(pNode->rules, 0);

					if (0 ==
						list_length((List *)a_rule->parrangestart))
					{
						/* new start > negative infinite start */
						bstat = false;
					}
					else
						bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   ">",
											   (List *)a_rule->parrangestart,
												   d_start, isnull, tupledesc);

						/* if the new partition start < start of the
						 * first partition then it is the new first
						 * partition.  Check the new end for overlap.
						 *
						 * NOTE: ignore the case where
						 * new start == 1st start and
						 * inclusive vs exclusive because that is just
						 * stupid.
						 *
						 */
						if (bstat)
						{
							isMiddle = -1;

							/* should be first partition */
							maxpartno = a_rule->parruleord - 1;
							if (0 == maxpartno)
							{
								maxpartno = 1;
								bOpenGap = true;
							}

						}
						else
						{
							isMiddle = 0;
						}
					}
				}
				else
				{
					/* if no "rules", then this is the first partition */

					isMiddle = 1;

					/* should be final partition */
					maxpartno = 1;

					goto L_end_overlap;
				}

			L_check_end:
				/* check the end */
				/* check for basic case of END < first partition (the
				   opposite of START > last partition) */

				ri = (PartitionRangeItem *)pbs->partEnd;
				PartitionRangeItemIsValid(NULL, ri);

				pid.partiddef = (Node *)copyObject(ri->partRangeVal);
				pid.partiddef =
						(Node *)transformExpressionList(pstate,
														(List *)pid.partiddef);

				prule = get_part_rule1(rel, &pid, false, false,
									   CurrentMemoryContext, &endSearchpoint,
									   pNode,
									   lrelname,
									   &pNode2);

				/* found match for end value in rules */
				if (prule && !(prule->topRule->parisdefault &&
							   is_split))
				{
					bool bstat;
					PartitionRule *a_rule = prule->topRule;
					d_end =
							magic_expr_to_datum(rel, pNode,
												pid.partiddef, &isnull);

					/* if end value was inclusive then it definitely
					 * overlaps
					 */
					if (ri->partedge == PART_EDGE_INCLUSIVE)
					{
						bOverlap = true;
						goto L_end_overlap;
					}

					/* not inclusive -- check harder if END really
					 * overlaps
					 */
					if (0 ==
						list_length((List *)a_rule->parrangestart))
					{
						/* -infinite start < new end - overlap */
						bOverlap = true;
						goto L_end_overlap;
					}

					bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "<",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc);
					if (bstat)
					{
						/* start < new end - overlap */
						bOverlap = true;
						goto L_end_overlap;
					}

					/* Must be the case that new end = start of
					 * a_rule (because if the start > new end then how
					 * could we find it in the interval for prule ?)
					 * This is ok if they have opposite
					 * INCLUSIVE/EXCLUSIVE ->  New partition does not
					 * overlap.
					 */

					Assert (compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "=",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc));

					if (a_rule->parrangestartincl == 
						(ri->partedge == PART_EDGE_INCLUSIVE))
					{
						/* start and end must be of opposite
						 * types, else they overlap
						 */
						bOverlap = true;
						goto L_end_overlap;
					}

					/* opposite inclusive/exclusive, so in middle of
					 * range of existing partitions
					 */
					isMiddle = 0;
				} /* end if prule */


				/* check for case of END < first partition */
				if (pNode && pNode->rules && list_length(pNode->rules))
				{
					bool bstat;
					PartitionRule *a_rule = /* get first rule */
							(PartitionRule *)list_nth(pNode->rules, 0);
					d_end =
							magic_expr_to_datum(rel, pNode,
												pid.partiddef, &isnull);

					if (0 ==
						list_length((List *)a_rule->parrangestart))
					{
						/* new end > negative infinite start */
						bstat = false;
					}
					else
					bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   ">",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc);

					/* if the new partition end < start of the first
					 * partition then it is the new first partition.
					 */
					if (bstat)
					{
						/* check if start was also ok for first partition */
						switch (isMiddle)
						{
							case -1:
								/* since new start < first start and
								 * new end < first start should be
								 * first.
								 */

								/* should be first partition */
								maxpartno = a_rule->parruleord - 1;
								if (0 == maxpartno)
								{
									maxpartno = 1;
									bOpenGap = true;
								}

								break;
							case 0:
							case 1:
							default:
								/* new end is less than first
								 * partition start but new start isn't
								 * -- must be end < start
								 */
								break;
						}
						goto L_end_overlap;
					}

					/* could be the case that new end == start of
					 * first.  This is ok if they have opposite
					 * INCLUSIVE/EXCLUSIVE.  New partition is still
					 * first partition for this case
					 */

					if (0 ==
						list_length((List *)a_rule->parrangestart))
					{
						/* new end > negative infinite start */
						bstat = false;
					}
					else
					bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   "=",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc);
					if (bstat)
					{
						if (a_rule->parrangestartincl == 
							(ri->partedge == PART_EDGE_INCLUSIVE))
						{
							/* start and end must be of opposite
							 * types, else they overlap */
							bOverlap = true;
							goto L_end_overlap;
						}
						/* check if start was also ok for first partition */
						switch (isMiddle)
						{
							case -1:
								/* since new start < first start and
								 * new end < first start should be
								 * first.
								 */

								/* should be first partition */
								maxpartno = a_rule->parruleord - 1;
								if (0 == maxpartno)
								{
									maxpartno = 1;
									bOpenGap = true;
								}

								break;
							case 0:
							case 1:
							default:
								/* new end is less than first
								 * partition start but new start isn't
								 * -- must be end < start
								 */
								break;
						}
						goto L_end_overlap;
					}
					else
					{
						/* tricky case: the new end is greater than the
						 * start of the first partition, but it does not
						 * intersect any existing partitions.  So we
						 * are trying to add a partition in the middle
						 * of the existing partitions or after the
						 * last partition.
						 */
						a_rule = /* get last rule */
							(PartitionRule *)list_nth(pNode->rules,
											  list_length(pNode->rules) - 1);
					if (0 ==
						list_length((List *)a_rule->parrangeend))
					{
						/* new end < infinite end */
						bstat = false;
					}
					else

						bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "<",
											   (List *)a_rule->parrangeend,
												   d_end, isnull, tupledesc);

						/* if the new partition end > end of the
						 * last partition then it is the new last
						 * partition (maybe)
						 *
						 * NOTE: ignore the case where
						 * new end == last end and
						 * inclusive vs exclusive because that is just
						 * stupid.
						 *
						 */
						if (bstat)
						{
							switch (isMiddle)
							{
								case 1:
									/* since new start > last end and
									 * new end > last end should be
									 * last.
									 */

									/* should be last partition */
									maxpartno = a_rule->parruleord + 1;

									break;
								case -1:
									/* since new start < first start
									 * and new end > last end we would
									 * overlap all partitions!!!
									 */
								case 0:
									/* since new start < last end
									 * and new end > last end we would
									 * overlap last partition
									 */
									bOverlap = true;
									goto L_end_overlap;
									break;
								default:
									/* new end is less than last
									 * partition end but new start isn't
									 * -- must be end < start
									 */
									break;
							}
						}
						else
						{
							switch (isMiddle)
							{
								case -1:
									/* since new start < first start
									 * and new end in middle we overlap
									 */
									bOverlap = true;
									goto L_end_overlap;

									break;
								case 0:
									/* both start and end in middle */
									break;
								case 1:
								default:
									/* since new start > last end and
									 * new end in middle
									 * -- must be end < start
									 */
									break;
							}
						}
					}
				}
				else
				{
					/* if no "rules", then this is the first partition */

					isMiddle = 1;

					/* should be final partition */
					maxpartno = 1;

					goto L_end_overlap;
				}


				/* if the individual start and end values don't
				 * intersect an existing partition, make sure they
				 * don't define a range which contains an existing
				 * partition, ie new start < existing start and new
				 * end > existing end
				 */

				if (!bOverlap && (0 == isMiddle))
				{
					bOpenGap = true;

/*
  hmm, not always true.  see MPP-3667, MPP-3636, MPP-3593

					if (startSearchpoint != endSearchpoint)
					{
						bOverlap = true;
						goto L_end_overlap;
					}

*/
					while (1)
					{
						bool bstat;
						PartitionRule *a_rule = /* get the rule */
								(PartitionRule *)list_nth(pNode->rules,
														  startSearchpoint);

						/* MPP-3621: fix ADD for open intervals */

						if (0 ==
							list_length((List *)a_rule->parrangeend))
						{
							/* new end < infinite end */
							bstat = false;
						}
						else
							bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "<=",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc);

						if (bstat)
						{
							startSearchpoint++;
							Assert(startSearchpoint
								   <= list_length(pNode->rules));
							continue;
						}

						/* if previous partition was less than
						   current, then this one should be larger.
						   if not, then it overlaps...
						*/
						if (
							(0 ==
							 list_length((List *)a_rule->parrangestart))
								||
								!compare_partn_opfuncid(pNode,
													"pg_catalog",
													">=",
												(List *)a_rule->parrangestart,
													d_end, isnull, tupledesc))
						{
							prule = NULL; /* could get the right prule... */
							bOverlap = true;
							goto L_end_overlap;
						}

						/* shift a_rule up so new rule has a place to fit */
						maxpartno = a_rule->parruleord;

						break;
					} /* end while */

				} /* end 0 == middle */

			L_end_overlap:
				if (bOverlap)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("new partition overlaps existing "
									"partition%s",
									(prule && prule->isName) ?
									prule->partIdStr : ""),
											   errOmitLocation(true)));

			}

			free_parsestate(&pstate);
		} /* end if parttype_range */
	} /* end if pelem && pelem->boundspec */

	/* this function does transformExpr on the boundary specs */
	partno = atpxPart_validate_spec(pBy,
									&cxt,
									rel,
									ct,
									pelem,
									pNode,
									partName,
									isDefault,
									part_type,
									"");

	if (pelem && pelem->boundSpec)
	{
		if (PARTTYP_LIST == part_type)
		{
			ListCell 			*lc;
			PartitionValuesSpec *spec;
			AlterPartitionId 	 pid;

			Assert (IsA(pelem->boundSpec, PartitionValuesSpec));

			MemSet(&pid, 0, sizeof(AlterPartitionId));

			spec = (PartitionValuesSpec *)pelem->boundSpec;

			/* only check this if we aren't doing split */
			if (1)
			{
				foreach(lc, spec->partValues)
				{
					List			*vals  = lfirst(lc);
					PgPartRule   	*prule = NULL;

					pid.idtype = AT_AP_IDValue;
					pid.partiddef = (Node *)vals;
					pid.location  = -1;

					prule = get_part_rule1(rel, &pid, false, false,
										   CurrentMemoryContext, NULL,
										   pNode,
										   lrelname,
										   &pNode2);

					if (prule && !(prule->topRule->parisdefault && is_split))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("new partition definition overlaps "
										"existing partition%s of %s",
										prule->partIdStr,
										lrelname),
												   errOmitLocation(true)));

				} /* end foreach */
			}

			/* give a new maxpartno for the list partition */
			if (pNode && pNode->rules && list_length(pNode->rules))
			{
				ListCell *lc;

				foreach(lc, pNode->rules)
				{
					PartitionRule *rule = lfirst(lc);

					/* find the highest parruleord */
					if (rule->parruleord > maxpartno)
						maxpartno = rule->parruleord;
				}
				maxpartno++;
			}

		}

	}

	/* create the partition */

	ct->relation = makeRangeVar(
			get_namespace_name(RelationGetNamespace(par_rel)),
			RelationGetRelationName(par_rel), -1);

	if (bOpenGap) /* might need a gap to insert the new partition */
	{
		AlterPartitionId 	 	 pid;
		PgPartRule   			*prule = NULL;

		pid.idtype = AT_AP_IDRank;
		pid.partiddef = (Node *)makeInteger(-1);
		pid.location  = -1;

		/* get the last rule, and increment each parruleord until
		 * reach parruleord == maxpartno
		 */
		prule = get_part_rule1(rel, &pid, false, false,
							   CurrentMemoryContext, NULL,
							   pNode,
							   lrelname,
							   &pNode2);

		/* NOTE: this assert is valid, except for the case of splitting
		 * the very last partition of a table.  For that case, we must
		 * drop the last partition before re-adding the new pieces, which
		 * violates this invariant
		 */
/*	Assert(prule && prule->topRule && prule->pNode && prule->pNode->part); */

		if (prule && prule->topRule && prule->pNode && prule->pNode->part)
			parruleord_open_gap(
					prule->pNode->part->partid,
					prule->pNode->part->parlevel,
					prule->topRule->parparentoid,
					prule->topRule->parruleord,
					maxpartno,
					CurrentMemoryContext);
	}

	{
		List			*l1;
		ListCell		*lc;
		int 			 ii					 = 0;
		bool			 bFixFirstATS		 = true;
		bool			 bFirst_TemplateOnly = true;   /* ignore dummy entry */
		int 			 pby_templ_depth	 = 0;	   /* template partdepth */
		Oid 			 skipTableRelid	 	 = InvalidOid; 

		ct->partitionBy = (Node *)pBy;

		l1 = parse_analyze((Node *)ct, NULL, NULL, 0);

		/*
		 * Look for the first CreateStmt and generate a GrantStmt
		 * based on the RangeVar in it.
		 */
		foreach(lc, l1)
		{
			Node *s = lfirst(lc);

			/* skip the first one, it's the fake create table */
			if (lc == list_head(l1))
				continue;

			if (IsA(s, Query) && IsA(((Query *)s)->utilityStmt, CreateStmt))
			{
				HeapTuple tuple;
				Datum aclDatum;
				bool isNull;
				CreateStmt *t = (CreateStmt *)((Query *)s)->utilityStmt;


				tuple = SearchSysCache(RELOID,
									   ObjectIdGetDatum(RelationGetRelid(rel)),
									   0, 0, 0);
				if (!HeapTupleIsValid(tuple))
					elog(ERROR, "cache lookup failed for relation %u",
						 RelationGetRelid(rel));

				aclDatum = SysCacheGetAttr(RELOID, tuple,
										   Anum_pg_class_relacl,
										   &isNull);
				if (!isNull)
				{
					List *cp = NIL;
					int i, num;
					Acl *acl;
					AclItem *aidat;

					acl = DatumGetAclP(aclDatum);

					num = ACL_NUM(acl);
					aidat = ACL_DAT(acl);

					for (i = 0; i < num; i++)
					{
						AclItem	*aidata = &aidat[i];
						Datum d;
						char *str;

						d = DirectFunctionCall1(aclitemout,
												PointerGetDatum(aidata));
						str = DatumGetCString(d);

						cp = lappend(cp, makeString(str));

					}

					if (list_length(cp))
					{
						GrantStmt *gs = makeNode(GrantStmt);
						List *pt;

						gs->is_grant = true;
						gs->objtype = ACL_OBJECT_RELATION;
						gs->cooked_privs = cp;

						gs->objects = list_make1(copyObject(t->relation));

						pt = parse_analyze((Node *)gs, NULL, NULL, 0);
						l1 = list_concat(l1, pt);
					}
				}
				ReleaseSysCache(tuple);
				break;
			}
		}

		/* skip the first cell because the table already exists --
		 * don't recreate it
		 */
		lc = list_head(l1);

		if (lc)
		{
			Node *s = lfirst(lc);

			/* MPP-10421: but save the relid of the skipped table,
			 * because we skip indexes associated with it... 
			 */
			if (IsA(s, Query) && IsA(((Query *)s)->utilityStmt, CreateStmt))
			{
				CreateStmt *t = (CreateStmt *)((Query *)s)->utilityStmt;

				skipTableRelid = RangeVarGetRelid(t->relation, true);
			}
		}


		for_each_cell(lc, lnext(lc))
		{
			Query *q = lfirst(lc);

			/*
			 * MPP-6379, MPP-10421: If the statement is an expanded
			 * index creation statement on the parent (or the "skipped
			 * table"), ignore it. We get into this situation when the
			 * parent has one or more indexes on it that our new
			 * partition is inheriting.
			 */
			if (IsA(q->utilityStmt, IndexStmt))
			{
				IndexStmt *istmt	 = (IndexStmt *)q->utilityStmt;
				Oid		   idxRelid	 = RangeVarGetRelid(istmt->relation, true);

				if (idxRelid == RelationGetRelid(rel))
					continue;

				if (OidIsValid(skipTableRelid) &&
					(idxRelid == skipTableRelid))
					continue;
			}

			/* XXX XXX: fix the first Alter Table Statement to have
			 * the correct maxpartno.  Whoohoo!!
			 */
			if (bFixFirstATS && q && q->utilityStmt
				&& IsA(q->utilityStmt, AlterTableStmt))
			{
				PartitionSpec 	*spec = NULL;
				AlterTableStmt 	*ats;
				AlterTableCmd  	*atc;
				List 			*cmds;

				bFixFirstATS = false;

				ats = (AlterTableStmt *)q->utilityStmt;
				Assert(IsA(ats, AlterTableStmt));

				cmds = ats->cmds;

				Assert(cmds && (list_length(cmds) > 1));

				atc = (AlterTableCmd *)lsecond(cmds);

				Assert(atc->def);

				pBy = (PartitionBy *)atc->def;

				Assert(IsA(pBy, PartitionBy));

				spec = (PartitionSpec *)pBy->partSpec;

				if (spec)
				{
					List 				*l2 = spec->partElem;
					PartitionElem 		*pel;

					if (l2 && list_length(l2))
					{
						pel = (PartitionElem *)linitial(l2);

						pel->partno = maxpartno;
					}

				}

			} /* end first alter table fixup */
			else if (IsA(q->utilityStmt, CreateStmt))
			{
				/* propagate owner */
				((CreateStmt *)q->utilityStmt)->ownerid = ownerid;
			}

			/* normal case - add partitions using CREATE statements
			 * that get dispatched to the segments
			 */
			if (!bSetTemplate)
				ProcessUtility(q->utilityStmt, 
							   synthetic_sql,
							   NULL, 
							   false, /* not top level */
							   dest, 
							   NULL);
			else
			{ /* setting subpartition template only */

				/* find all the alter table statements that contain
				 * partaddinternal, and extract the definitions.  Only
				 * build the catalog entries for subpartition
				 * templates, not "real" table entries.
				 */
				if (q && q->utilityStmt
					&& IsA(q->utilityStmt, AlterTableStmt))
				{
					AlterTableStmt *at2 = (AlterTableStmt *)q->utilityStmt;
					List *l2 = at2->cmds;
					ListCell *lc2;

					foreach(lc2, l2)
					{
						AlterTableCmd *ac2 = (AlterTableCmd *) lfirst(lc2);

						if (ac2->subtype == AT_PartAddInternal)
						{
							PartitionBy *templ_pby =
									(PartitionBy *)ac2->def;

							Assert(IsA(templ_pby, PartitionBy));

							/* skip the first one because it's the
							 * fake parent partition definition for
							 * the subpartition template entries
							 */

							if (bFirst_TemplateOnly)
							{
								bFirst_TemplateOnly = false;

								/* MPP-5992: only set one level of
								 * templates -- we might have
								 * templates for subpartitions of the
								 * subpartitions, which would add
								 * duplicate templates into the table.
								 * Only add templates of the specified
								 * depth and skip deeper template
								 * definitions.
								 */
								pby_templ_depth = templ_pby->partDepth + 1;

							}
							else
							{
								if (templ_pby->partDepth == pby_templ_depth)
									add_part_to_catalog(
											RelationGetRelid(rel),
											(PartitionBy *)ac2->def,
											true);
							}

						}
					} /* end foreach lc2 l2 */
				}
			} /* end else setting subpartition templates only */

			ii++;
		} /* end for each cell */

	}

	if (par_prule && par_prule->topRule)
		heap_close(par_rel, NoLock);

	return pSubSpec;
} /* end atpxPartAddList */

/* ALTER TABLE ... ALTER PARTITION */
static void
ATPExecPartAlter(List **wqueue, AlteredTableInfo *tab, Relation rel,
                 AlterPartitionCmd *pc)
{
	AlterPartitionId 	*pid		   = (AlterPartitionId *)pc->partid;
	AlterTableCmd 		*atc		   = (AlterTableCmd *)pc->arg1;
	PgPartRule   		*prule		   = NULL;
	List 				*pidlst		   = NIL;
	AlterPartitionId 	*pid2		   = makeNode(AlterPartitionId);
	AlterPartitionCmd 	*pc2		   = NULL;
	bool				 bPartitionCmd = true;	/* true if a "partition" cmd */
	Relation			 rel2		   = rel;

	while (1)
	{
		pidlst = lappend(pidlst, pid);

		if (atc->subtype != AT_PartAlter)
			break;
		pc2 = (AlterPartitionCmd *)atc->def;
		pid = (AlterPartitionId *)pc2->partid;
		atc = (AlterTableCmd *)pc2->arg1;
	}

	/* let split, exchange through */
	if (!(atc->subtype == AT_PartExchange ||
		  atc->subtype == AT_PartSplit ||
		  atc->subtype == AT_SetDistributedBy) &&
		Gp_role != GP_ROLE_DISPATCH)
		return;

	switch (atc->subtype)
	{
		case AT_PartAdd:				/* Add */
		case AT_PartCoalesce:			/* Coalesce */
		case AT_PartDrop:				/* Drop */
		case AT_PartExchange:			/* Exchange */
		case AT_PartMerge:				/* Merge */
		case AT_PartModify:				/* Modify */
		case AT_PartRename:	 			/* Rename */

				/* XXX XXX: treat set subpartition template special:

				need to pass the pNode to ATPExecPartSetTemplate and bypass
				ATExecCmd ...

				*/
		case AT_PartSetTemplate:		/* Set Subpartition Template */

		case AT_PartSplit:				/* Split */
		case AT_PartTruncate:			/* Truncate */
				break;

		/* Next, list of ALTER TABLE commands applicable to a child table */
		case AT_SetTableSpace:			/* Set Tablespace */
		case AT_SetDistributedBy:	/* SET DISTRIBUTED BY */
				bPartitionCmd = false;
				break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot ALTER PARTITION for relation \"%s\"",
							RelationGetRelationName(rel)),
									   errOmitLocation(true)));
	}

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		pid2->idtype = AT_AP_IDList;
		pid2->partiddef = (Node *)pidlst;
		pid2->location  = -1;

		prule = get_part_rule(rel, pid2, true, true,
							  CurrentMemoryContext, NULL,
							  false);

		if (!prule)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot ALTER PARTITION for relation \"%s\"",
							RelationGetRelationName(rel)),
									   errOmitLocation(true)));
		if (bPartitionCmd)
		{
			/* build the IDRule for the nested ALTER PARTITION cmd ... */
			Assert(IsA(atc->def, AlterPartitionCmd));

			pc2 = (AlterPartitionCmd *)atc->def;
			pid = (AlterPartitionId *)pc2->partid;

			pid2->idtype = AT_AP_IDRule;
			pid2->partiddef = (Node *)list_make2((Node *)prule, pid);
			pid2->location  = -1;

			pc2->partid = (Node *)pid2;
		}
		else /* treat as a table */
		{
			/* Update PID for use on QEs */
			pid2->idtype = AT_AP_IDRule;
			pid2->partiddef = (Node *)list_make2((Node *)prule, pid);
			pid2->location  = -1;
			pc->partid = (Node *)pid2;

			/* get the child table relid */
			rel2 = heap_open(prule->topRule->parchildrelid,
							 AccessExclusiveLock);

			/* MPP-5524: check if can change distribution policy */
			if (atc->subtype == AT_SetDistributedBy)
			{
				List *dist_cnames = NIL;
				Assert(IsA(atc->def, List));

				dist_cnames = lsecond((List*)atc->def);

				/*	might be null if no policy set, e.g. just a change
				 *	of storage options...
				 */
				if (dist_cnames)
				{
					Assert(IsA(dist_cnames, List));

					if (! can_implement_dist_on_part(rel, dist_cnames) )
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot ALTER PARTITION ... SET "
										"DISTRIBUTED BY for %s",
										prule->relname),
										errhint("distribution policy of "
												"partition must match parent"),
														   errOmitLocation(true)
										));
				}

			}

			/*
			 * Give the notice first, because it looks weird if it
			 * comes after a failure message
			 */
			ereport(NOTICE,
					(errmsg("altering table \"%s\" "
							"(%s)",
							RelationGetRelationName(rel2),
							prule->relname)));
		}
	}
	else if (Gp_role == GP_ROLE_EXECUTE && atc->subtype == AT_SetDistributedBy)
	{
		pid = (AlterPartitionId *)pc->partid;
		Assert(IsA(pid->partiddef, List));
		prule = (PgPartRule *)linitial((List *)pid->partiddef);
		/* get the child table relid */
		rel2 = heap_open(prule->topRule->parchildrelid,
						 AccessExclusiveLock);
		bPartitionCmd = false;
	}
	/* execute the command */
	ATExecCmd(wqueue, tab, rel2, atc);

	if (!bPartitionCmd)
	{
		/* NOTE: for the case of Set Distro,
		 * ATExecSetDistributedBy rebuilds the relation, so rel2
		 * is already gone!
		 */
		if (atc->subtype != AT_SetDistributedBy)
			heap_close(rel2, NoLock);
	}

	/* MPP-6929: metadata tracking - don't track this! */

} /* end ATPExecPartAlter */


/* ALTER TABLE ... COALESCE PARTITION */
static void
ATPExecPartCoalesce(Relation rel,
                    AlterPartitionCmd *pc)
{
	AlterPartitionId *pid = (AlterPartitionId *)pc->partid;
	PgPartRule   *prule = NULL;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	prule = get_part_rule(rel, pid, true, true, CurrentMemoryContext, NULL,
						  false);

	if (0)
	{

		parruleord_open_gap(
				prule->pNode->part->partid,
				prule->pNode->part->parlevel,
				prule->topRule->parparentoid,
				prule->topRule->parruleord,
				0,
				CurrentMemoryContext);

	}

    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot COALESCE PARTITION for relation \"%s\"",
                    RelationGetRelationName(rel)),
             			   errOmitLocation(true)));

}
/* ALTER TABLE ... DROP PARTITION */
static List *
atpxDropList(Relation rel, PartitionNode *pNode)
{
	List *l1 = NIL;
	ListCell *lc;

	if (!pNode)
		return l1;

	/* add the child lists first */
	foreach(lc, pNode->rules)
	{
		PartitionRule *rule = lfirst(lc);
		List *l2 = NIL;

		if (rule->children)
			l2 = atpxDropList(rel, rule->children);
		else
			l2 = NIL;

		if (l2)
		{
			if (l1)
				l1 = list_concat(l1, l2);
			else
				l1 = l2;
		}
	}

	/* and the default partition */
	if (pNode->default_part)
	{
		PartitionRule *rule = pNode->default_part;
		List *l2 = NIL;

		if (rule->children)
			l2 = atpxDropList(rel, rule->children);
		else
			l2 = NIL;

		if (l2)
		{
			if (l1)
				l1 = list_concat(l1, l2);
			else
				l1 = l2;
		}
	}

	/* add entries for rules at current level */
	foreach(lc, pNode->rules)
	{
		PartitionRule *rule = lfirst(lc);
		char	*prelname;
		char 	*nspname;
		Relation	rel;

		rel = heap_open(rule->parchildrelid, AccessShareLock);
		prelname = pstrdup(RelationGetRelationName(rel));
		nspname = pstrdup(get_namespace_name(RelationGetNamespace(rel)));
		heap_close(rel, NoLock);

		if (l1)
			l1 = lappend(l1, list_make2(makeString(nspname),
										makeString(prelname)));
		else
			l1 = list_make1(list_make2(makeString(nspname),
									   makeString(prelname)));
	}

	/* and the default partition */
	if (pNode->default_part)
	{
		PartitionRule *rule = pNode->default_part;
		char *prelname;
		char *nspname;
		Relation	rel;

		rel = heap_open(rule->parchildrelid, AccessShareLock);
		prelname = pstrdup(RelationGetRelationName(rel));
		nspname = pstrdup(get_namespace_name(RelationGetNamespace(rel)));
		heap_close(rel, NoLock);

		if (l1)
			l1 = lappend(l1, list_make2(makeString(nspname),
										makeString(prelname)));
		else
			l1 = list_make1(list_make2(makeString(nspname),
									   makeString(prelname)));
	}

	return l1;
} /* end atpxDropList */

static void
ATPExecPartDrop(Relation rel,
                AlterPartitionCmd *pc)
{
	AlterPartitionId 	*pid 		 = (AlterPartitionId *)pc->partid;
	PgPartRule   		*prule 		 = NULL;
	PartitionNode  		*pNode  	 = NULL;
	DropStmt 			*ds 		 = (DropStmt *)pc->arg1;
	bool 				 bCheckMaybe = !(ds->missing_ok);
	AlterPartitionId 	*locPid 	 = pid;  /* local pid if IDRule */
	PgPartRule* 		 par_prule 	 = NULL; /* prule for parent if IDRule */
	char 		 		 lRelNameBuf[(NAMEDATALEN*2)];
	char 				*lrelname	= NULL;
	bool 				 bForceDrop	= false;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	if (pc->arg2 &&
		IsA(pc->arg2, AlterPartitionCmd))
	{
		/* NOTE: Ugh, I hate this hack.  Normally, PartDrop has a null
		 * pc->arg2 (the DropStmt is on arg1).  However, for SPLIT, we
		 * have the case where we may need to DROP that last partition
		 * of a table, which in only ok because we will re-ADD two
		 * partitions to replace it.  So allow bForceDrop only for
		 * this case.  We need a better way to decorate the ALTER cmd
		 * structs to annotate these special cases.
		 */
		bForceDrop = true;
	}

	/* missing partition id only ok for range partitions -- just get
	 * first one */

	locPid =
			wack_pid_relname(pid,
							 &pNode,
							 rel,
							 &par_prule,
							 &lrelname,
							 lRelNameBuf);

	if (AT_AP_IDNone == locPid->idtype)
	{
		if (pNode && pNode->part && (pNode->part->parkind != 'r'))
		{
			if (strlen(lrelname))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("missing name or value for DROP for %s",
								lrelname),
										   errOmitLocation(true)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("missing name or value for DROP"),
								   errOmitLocation(true)));
		}

		/* if a range partition, and not specified, just get the first one */
		locPid->idtype = AT_AP_IDRank;
		locPid->partiddef = (Node *)makeInteger(1);
	}

	prule = get_part_rule(rel, pid, bCheckMaybe, true,
						  CurrentMemoryContext, NULL, false);

	/* MPP-3722: complain if for(value) matches the default partition */
	if ((locPid->idtype == AT_AP_IDValue)
		&& prule &&
		(prule->topRule == prule->pNode->default_part))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("FOR expression matches "
							"DEFAULT partition%s of %s",
							prule->partIdStr,
							prule->relname),
					 errhint("FOR expression may only specify "
							 "a non-default partition in this context."),
									   errOmitLocation(true)));

	if (!prule)
	{
		Assert(ds->missing_ok);
		switch (locPid->idtype)
		{
			case AT_AP_IDNone:				/* no ID */
				/* should never happen */
				Assert(false);
				break;
			case AT_AP_IDName:				/* IDentify by Name */
				ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("partition \"%s\" of %s does not "
							"exist, skipping",
							strVal(locPid->partiddef),
							lrelname
							),
									   errOmitLocation(true)));

				break;
			case AT_AP_IDValue:				/* IDentifier FOR Value */
				ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("partition for specified value of "
							"%s does not exist, skipping",
							lrelname
							 ),
									   errOmitLocation(true)));

				break;
			case AT_AP_IDRank:				/* IDentifier FOR Rank */
				ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("partition for specified rank of "
							"%s does not exist, skipping",
							lrelname
							 ),
									   errOmitLocation(true)));

				break;
			case AT_AP_ID_oid:				/* IDentifier by oid */
				ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("partition for specified oid of "
							"%s does not exist, skipping",
							lrelname
							 ),
									   errOmitLocation(true)));
				break;
			case AT_AP_IDDefault:			/* IDentify DEFAULT partition */
				ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("DEFAULT partition for "
							"%s does not exist, skipping",
							lrelname
							 ),
									   errOmitLocation(true)));
				break;
			default: /* XXX XXX */
				Assert(false);
		}
		return;
	}
	else
	{
		char* prelname;
		int   numParts = list_length(prule->pNode->rules);
		DestReceiver 	*dest = None_Receiver;
		Relation rel2;
		RangeVar *relation;
		char *namespace_name;

		/* add the default partition to the count of partitions */
		if (prule->pNode->default_part)
			numParts++;

		/* maybe ERRCODE_INVALID_TABLE_DEFINITION ? */

		/* cannot drop a hash partition */
		if ('h' == prule->pNode->part->parkind)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop HASH partition%s of %s",
							prule->partIdStr,
							prule->relname),
					 errhint("recreate the table to reduce the "
							 "number of partitions"),
									   errOmitLocation(true)));

		/* cannot drop last partition of table */
		if (!bForceDrop && (numParts <= 1))
		{
			if (AT_AP_IDRule != pid->idtype)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot drop partition%s of "
								"%s -- only one remains",
								prule->partIdStr,
								prule->relname),
						 errhint("Use DROP TABLE \"%s\" to remove the "
								 "table and the final partition ",
								 RelationGetRelationName(rel)),
										   errOmitLocation(true)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot drop partition%s of "
								"%s -- only one remains",
								prule->partIdStr,
								prule->relname),
						 errhint("DROP the parent partition to remove the "
								 "final partition "),
										   errOmitLocation(true)));

		}
		rel2 = heap_open(prule->topRule->parchildrelid, NoLock);

		elog(DEBUG5, "dropping partition oid %u", prule->topRule->parchildrelid);
		prelname = pstrdup(RelationGetRelationName(rel2));
		namespace_name = get_namespace_name(rel2->rd_rel->relnamespace);

		/* XXX XXX : don't need "relation" unless fix to use removerelation */
		relation = makeRangeVar(namespace_name, prelname, -1);
		relation->location = pc->location;

		heap_close(rel2, NoLock);

		ds->removeType = OBJECT_TABLE;
		ds->bAllowPartn = true; /* allow drop of partitions */

		if (prule->topRule->children)
		{
			List *l1 = atpxDropList(rel2, prule->topRule->children);

			ds->objects = lappend(l1,
								  list_make2(makeString(namespace_name),
											 makeString(prelname)));
		}
		else
			ds->objects = list_make1(list_make2(makeString(namespace_name),
												makeString(prelname)));

		ProcessUtility((Node *) ds,
					   synthetic_sql,
					   NULL, 
					   false, /* not top level */
					   dest, 
					   NULL);

		/* Notify of name if did not use name for partition id spec */
		if (prule && prule->topRule && prule->topRule->children
			&& (ds->behavior != DROP_CASCADE ))
		{
			ereport(NOTICE,
					(errmsg("dropped partition%s for %s and its children",
							prule->partIdStr,
							prule->relname)));
		}
		else if ((pid->idtype != AT_AP_IDName)
				 && prule->isName)
				ereport(NOTICE,
						(errmsg("dropped partition%s for %s",
								prule->partIdStr,
								prule->relname)));


		/* MPP-6929: metadata tracking */
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "PARTITION", "DROP"
				);
	}

} /* end ATPExecPartDrop */

static void
exchange_part_inheritance(Oid oldrelid, Oid newrelid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple tuple;
	Oid parentid;
	Relation oldrel;
	Relation newrel;
	Relation parent;
	Relation catalogRelation = heap_open(InheritsRelationId, AccessShareLock);

	oldrel = heap_open(oldrelid, AccessExclusiveLock);
	newrel = heap_open(newrelid, AccessExclusiveLock);

	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldrelid));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);
	Assert(HeapTupleIsValid(tuple));

	parentid = ((Form_pg_inherits)GETSTRUCT(tuple))->inhparent;

	/* should only be one parent when it comes to inheritance */
	Assert(!HeapTupleIsValid(systable_getnext(scan)));
	systable_endscan(scan);
	heap_close(catalogRelation, AccessShareLock);

	parent = heap_open(parentid, AccessShareLock); /* should be enough */
	ATExecDropInherit(oldrel,
			makeRangeVar(get_namespace_name(parent->rd_rel->relnamespace),
					     RelationGetRelationName(parent), -1),
			true);

	inherit_parent(parent, newrel, true /* it's a partition */, NIL);
	heap_close(parent, NoLock);
	heap_close(oldrel, NoLock);
	heap_close(newrel, NoLock);
}

/*
 * Get CHECK constraints on columns specified in the attrs argument.
 *
 * This is basically a subroutine used by exchange_part_constraints
 * to obtain  the constraints on a part's partitioning columns that 
 * guard its partition rule.
 *
 * Note that pgcon (the ConstraintRelationId appropriately locked) 
 * is supplied externally for efficiency.  No other relation should
 * be used.
 */
static List *
get_part_constraints(List *attrs, Relation pgcon, Relation rel)
{
	List 	   *cons;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	scan = systable_beginscan(pgcon, ConstraintRelidIndexId,
							  true, SnapshotNow, 1, &key);

	cons = NIL;
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
		bool isnull;
		Datum val;
		Datum *keys;
		int nkeys;
		bool matched = false;
		ListCell *lc;

		if (con->contype != CONSTRAINT_CHECK)
			continue;

		/* only copy constraints which range across our partitioning columns */
		val = heap_getattr(tuple, Anum_pg_constraint_conkey,
						   RelationGetDescr(pgcon), &isnull);
		if (isnull)
			continue;

		deconstruct_array(DatumGetArrayTypeP(val),
						  INT2OID, 2, true, 's',
					      &keys, NULL, &nkeys);

		/* use naive search since number of columns is presumed small */
		foreach(lc, attrs)
		{
			AttrNumber attno = lfirst_int(lc);
			int j;
			bool found = false;

			for (j = 0; j < nkeys; j++)
			{
				if (DatumGetInt16(keys[j]) == attno)
				{
					found = true;
					break;
				}
			}

			if (found)
			{
				matched = true;
				break;
			}
		}
		pfree(keys);

		if (!matched)
			continue;

		cons = lappend(cons, heap_copytuple(tuple));
	}
	systable_endscan(scan);

	return cons;
}

/* Subroutine of ATPExecPartExchange used to swap constraints on existing
 * part and candidate part.
 *
 * part -- an integer list of partitioning attribute numbers relative
 *         to the parent partitioned table.
 * oldrel -- open relation of existing part to exchange
 * oldmap -- maps part list to partitioning attribute number relative
 *           to the existing part.
 * newrel -- open relation of the candidate part
 * newmap -- maps part list to partitioning attribute number relative
 *           to the table to exchange
 * validate -- whether to collect added constraints into result list
 *			   for phase 3 enforcement
 */
static List *
exchange_part_constraints(List *part, 
						  Relation oldrel, AttrMap *oldmap,
						  Relation newrel, AttrMap *newmap,
						  bool validate)
{
	List *oldpart = part;
	List *newpart = part;
	List *oldcons;
	List *newcons;
	ListCell *oldlc;
	Relation pgcon;
	bool add_cons = true;
	List *outcons = NIL;

	pgcon = heap_open(ConstraintRelationId, AccessShareLock);

	oldpart = attrMapIntList(oldmap, part);
	newpart = attrMapIntList(newmap, part);

	oldcons = get_part_constraints(oldpart, pgcon, oldrel);	
	newcons = get_part_constraints(newpart, pgcon, newrel);

	/*
	 * If we found constraints for the new table, they better be identical
	 * to the partitioning constraints. If they are, we need not add them,
	 */
	if (newcons)
	{
		foreach(oldlc, oldcons)
		{
			ListCell *newlc;
			HeapTuple oldtup = lfirst(oldlc);
			bool isnull;
			Oid conid;
			char *oldconstr;
			char *newconstr;
			
			conid = DatumGetObjectId(heap_getattr(oldtup, 
												  ObjectIdAttributeNumber,
												  RelationGetDescr(pgcon), 
												  &isnull));
			Assert(!isnull);
			oldconstr = pg_get_constraintexpr_string(conid);

			foreach(newlc, newcons)
			{
				HeapTuple newtup = lfirst(newlc);
				Oid newconid;
				bool match;

				newconid = DatumGetObjectId(heap_getattr(newtup, 
														 ObjectIdAttributeNumber,
														 RelationGetDescr(pgcon), 
														 &isnull));
				Assert(!isnull);

				newconstr = pg_get_constraintexpr_string(newconid);
				match = ( 0 == strcmp(oldconstr, newconstr));
											
											
				if ( ! match )
				{
					Form_pg_constraint con =
						(Form_pg_constraint)GETSTRUCT(newtup);
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("constraint \"%s\" on table \"%s\" "
									"conflicts with partitioning rule",
									NameStr(con->conname),
									RelationGetRelationName(newrel)),
											   errOmitLocation(true)));
				}

			}
		}

		list_free_deep(newcons);
		add_cons = false;
	}

	foreach (oldlc, oldcons)
	{
		HeapTuple tuple = lfirst(oldlc);
		ObjectAddress conobj;
		if (add_cons)
		{
			Form_pg_constraint con = (Form_pg_constraint)GETSTRUCT(tuple);
			Datum *dats;
			int16 *keys;
			Datum val;
			bool isnull;
			Node *conexpr;
			char *consrc;
			char *conbin;
			int nkeys;
			int i;
			AttrMap *imap = NULL;
			AttrMap *xmap = NULL;
			
			/* xmap: old (part) to new (candidate) attributes by way of parent. */
			imap = invertedAttrMap(oldmap, 0);
			xmap = compositeAttrMap(imap, newmap);
			if ( imap != NULL ) pfree(imap);

			/* Translate pg_constraint.conkey */
			val = heap_getattr(tuple, Anum_pg_constraint_conkey,
							   RelationGetDescr(pgcon), &isnull);

			Assert(!isnull);

			deconstruct_array(DatumGetArrayTypeP(val),
							  INT2OID, 2, true, 's',
						      &dats, NULL, &nkeys);

			keys = palloc(sizeof(int16) * nkeys);
			for (i = 0; i < nkeys; i++)
			{
				int16 key =  DatumGetInt16(dats[i]);
				keys[i] = (int16)attrMap(xmap, key);
			}

			/* Translate pg_constraint.conbin */
			val = heap_getattr(tuple, Anum_pg_constraint_conbin,
							   RelationGetDescr(pgcon), &isnull);
			Assert(!isnull);

			conbin = DatumGetCString(DirectFunctionCall1(textout, val));
			conexpr = stringToNode(conbin);
			conexpr = attrMapExpr(xmap, conexpr);
			conbin = nodeToString(conexpr);

			/* Don't translate pg_constraint.consrc -- per doc'n, use original */
			val = heap_getattr(tuple, Anum_pg_constraint_consrc,
							   RelationGetDescr(pgcon), &isnull);

			Assert(!isnull);
			consrc = DatumGetCString(DirectFunctionCall1(textout, val));

			/* Apply translated constraint to candidate. */
			CreateConstraintEntry(NameStr(con->conname),
								  con->connamespace,
								  con->contype,
								  con->condeferrable,
								  con->condeferred,
								  RelationGetRelid(newrel),
								  keys,
								  nkeys,
								  InvalidOid,
								  InvalidOid,
								  NULL,
								  0,
								  ' ',
								  ' ',
								  ' ',
								  InvalidOid,
								  conexpr,
								  conbin,
								  consrc);

			if (validate)
			{
				NewConstraint *newcon;
				newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
				newcon->name = pstrdup(NameStr(con->conname));
				newcon->contype = CONSTR_CHECK;
				Assert(con->contype == CONSTRAINT_CHECK);

				/* ExecQual wants implicit-AND format */
				newcon->qual = (Node *)make_ands_implicit((Expr *)conexpr);

				outcons = lappend(outcons, newcon);
			}
			
			if ( xmap != NULL ) pfree(xmap);
		}

		/* remove old constraint */
		conobj.classId = ConstraintRelationId;
		conobj.objectId = HeapTupleGetOid(tuple);
		conobj.objectSubId = 0;

		performDeletion(&conobj, DROP_RESTRICT);
	}

	if (add_cons)
		SetRelationNumChecks(newrel, newrel->rd_rel->relchecks +
							 list_length(oldcons));


	list_free_deep(oldcons);
	heap_close(pgcon, AccessShareLock);

	return outcons;
}

static void
exchange_part_rule(Oid oldrelid, Oid newrelid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple tuple;
	Relation catalogRelation;
	
	/* pg_partition and  pg_partition_rule are populated only on the 
	 * entry database, so our result is only meaningful there. */
	Insist(Gp_segment == -1);
	
	catalogRelation = heap_open(PartitionRuleRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_partition_rule_parchildrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldrelid));
	scan = systable_beginscan(catalogRelation,
							  PartitionRuleParchildrelidIndexId,
							  true, SnapshotNow, 1, &key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
	{
		tuple = heap_copytuple(tuple);

		((Form_pg_partition_rule)GETSTRUCT(tuple))->parchildrelid = newrelid;

		simple_heap_update(catalogRelation, &tuple->t_self, tuple);
		CatalogUpdateIndexes(catalogRelation, tuple);
		heap_freetuple(tuple);
	}

	systable_endscan(scan);
	heap_close(catalogRelation, NoLock);
}

static void
exchange_permissions(Oid oldrelid, Oid newrelid)
{
	HeapTuple oldtuple;
	HeapTuple newtuple;
	Datum save;
	bool saveisnull;
	Datum values[Natts_pg_class];
	bool nulls[Natts_pg_class];
	bool replaces[Natts_pg_class];
	HeapTuple replace_tuple;
	bool isnull;
	Relation rel = heap_open(RelationRelationId, RowExclusiveLock);

	oldtuple = SearchSysCache(RELOID, ObjectIdGetDatum(oldrelid),
							  0, 0, 0);
	if (!HeapTupleIsValid(oldtuple))
		elog(ERROR, "cache lookup failed for relation %u", oldrelid);

	save = SysCacheGetAttr(RELOID, oldtuple, Anum_pg_class_relacl,
						   &saveisnull);

	newtuple = SearchSysCache(RELOID, ObjectIdGetDatum(newrelid),
							  0, 0, 0);
	if (!HeapTupleIsValid(newtuple))
		elog(ERROR, "cache lookup failed for relation %u", newrelid);

	/* finished building new ACL value, now insert it */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	MemSet(replaces, false, sizeof(replaces));

	replaces[Anum_pg_class_relacl - 1] = true;
	values[Anum_pg_class_relacl - 1] = SysCacheGetAttr(RELOID, newtuple,
													   Anum_pg_class_relacl,
													   &isnull);
	if (isnull)
		nulls[Anum_pg_class_relacl - 1] = true;

	replace_tuple = heap_modify_tuple(oldtuple, RelationGetDescr(rel),
									 values, nulls, replaces);

	simple_heap_update(rel, &replace_tuple->t_self, replace_tuple);

	/* keep the catalog indexes up to date */
	CatalogUpdateIndexes(rel, replace_tuple);

	/* XXX: Update the shared dependency ACL info */

	/* finished building new ACL value, now insert it */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	MemSet(replaces, false, sizeof(replaces));

	replaces[Anum_pg_class_relacl - 1] = true;
	values[Anum_pg_class_relacl - 1] = save;

	if (saveisnull)
		nulls[Anum_pg_class_relacl - 1] = true;

	replace_tuple = heap_modify_tuple(newtuple, RelationGetDescr(rel),
									 values, nulls, replaces);

	simple_heap_update(rel, &replace_tuple->t_self, replace_tuple);

	/* keep the catalog indexes up to date */
	CatalogUpdateIndexes(rel, replace_tuple);

	/* update shared dependency */

	ReleaseSysCache(oldtuple);
	ReleaseSysCache(newtuple);
	heap_close(rel, NoLock);
}

/* ALTER TABLE ... EXCHANGE PARTITION 
 * 
 * Do the exchange that was validated earlier (in ATPrepPartExchange).
 */
static void
ATPExecPartExchange(AlteredTableInfo *tab, Relation rel, AlterPartitionCmd *pc)
{
	Oid					 oldrelid 	= InvalidOid;
	Oid					 newrelid 	= InvalidOid;
	PgPartRule   	    *orig_prule = NULL;
	AlterPartitionCmd	*pc2 = NULL;
	List				*pcols = NIL; /* partitioned attributes of rel */
	AlterPartitionIdType orig_pid_type = AT_AP_IDNone;	/* save for NOTICE msg at end... */

	if (Gp_role == GP_ROLE_UTILITY)
		return;

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		AlterPartitionId   *pid   = (AlterPartitionId *)pc->partid;
		PgPartRule   	   *prule = NULL;
		RangeVar       	   *newrelrv  = (RangeVar *)pc->arg1;
		RangeVar		   *oldrelrv;
		PartitionNode 	   *pn;
		Relation			oldrel;

		pn = RelationBuildPartitionDesc(rel, false);
		pcols = get_partition_attrs(pn);

		prule = get_part_rule(rel, pid, true, true,
							  CurrentMemoryContext, NULL, false);

		if (!prule)
			return;

		if (prule && prule->topRule && prule->topRule->children)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot EXCHANGE PARTITION for "
							"%s -- partition has children",
							prule->relname
							 ),
									   errOmitLocation(true)));

		orig_pid_type = pid->idtype;
		orig_prule = prule;

		oldrelid = prule->topRule->parchildrelid;
		newrelid = RangeVarGetRelid(newrelrv, false);

		pfree(pc->partid);

		oldrel = heap_open(oldrelid, NoLock);
		oldrelrv =
			makeRangeVar(get_namespace_name(RelationGetNamespace(oldrel)),
						 get_rel_name(oldrelid), -1);
		heap_close(oldrel, NoLock);

		pc->partid = (Node *)oldrelrv;
		pc2 = (AlterPartitionCmd *)pc->arg2;
		pc2->arg2 = (Node *)pcols; /* for execute nodes */

		/* MPP-6929: metadata tracking */
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "PARTITION", "EXCHANGE"
				);
	}
	else if (Gp_role == GP_ROLE_EXECUTE)
	{
		RangeVar *oldrelrv;
		RangeVar *newrelrv;

		Assert(IsA(pc->partid, RangeVar));
		Assert(IsA(pc->arg1, RangeVar));
		oldrelrv = (RangeVar *)pc->partid;
		newrelrv = (RangeVar *)pc->arg1;
		oldrelid = RangeVarGetRelid(oldrelrv, false);
		newrelid = RangeVarGetRelid(newrelrv, false);
		pc2 = (AlterPartitionCmd *)pc->arg2;
		pcols = (List *)pc2->arg2;
	}

	Assert(OidIsValid(oldrelid));
	Assert(OidIsValid(newrelid));

#if IF_ONLY_IT_WAS_THAT_SIMPLE
	swap_relation_files(oldrelid, newrelid, false);
	CommandCounterIncrement();
#else
	/*
	 * It would be nice to just swap the relfilenodes. In fact, we could
	 * do that in most cases, the exceptions being tables with dropped 
	 * columns and append only tables.
	 *
	 * So instead, we swap the names of the tables, the type names, the 
	 * constraints, inheritance. We do not swap indexes, ao information
	 * or statistics.
	 *
	 * Note that the state, whether QD or QE, at this point is
	 * - rel -- relid of partitioned table
	 * - oldrelid -- relid of part currently in place
	 * - newrelid -- relid of candidate part to exchange in
	 * - orig_pid_type -- what kind of AlterTablePartitionId id'd the part
	 * - orig_prule -- Used in issuing notice in occasional case.
	 * - pc2->arg1 -- integer Value node: 1 validate, else 0.
	 * - pcols -- integer list of master partitioning columns
	 */
	{
		char			 tmpname1[NAMEDATALEN];
		char			 tmpname2[NAMEDATALEN];
		char			*newname;
		char			*oldname;
		Relation		 newrel;
		Relation		 oldrel;
		AttrMap			*newmap;
		AttrMap			*oldmap;
		List			*newcons;
		bool			 ok;
		bool			 validate	= intVal(pc2->arg1) ? true : false;
		Oid				 oldnspid	= InvalidOid;
		Oid				 newnspid	= InvalidOid;
		char			*newNspName = NULL;
		char			*oldNspName = NULL;

		newrel = heap_open(newrelid, AccessExclusiveLock);
		oldrel = heap_open(oldrelid, AccessExclusiveLock);

		oldnspid = RelationGetNamespace(oldrel);
		newnspid = RelationGetNamespace(newrel);

		if (oldnspid != newnspid)
		{
			newNspName = pstrdup(get_namespace_name(newnspid));
			oldNspName = pstrdup(get_namespace_name(oldnspid));
		}

		newname = pstrdup(RelationGetRelationName(newrel));
		oldname = pstrdup(RelationGetRelationName(oldrel));
		
		ok = map_part_attrs(rel, newrel, &newmap, FALSE);
		Assert(ok);
		ok = map_part_attrs(rel, oldrel, &oldmap, FALSE);
		Assert(ok);

		newcons = exchange_part_constraints(pcols, oldrel, oldmap, newrel, newmap, validate);
		tab->constraints = list_concat(tab->constraints, newcons);
		CommandCounterIncrement();

		exchange_part_inheritance(RelationGetRelid(oldrel), RelationGetRelid(newrel));
		CommandCounterIncrement();

		snprintf(tmpname1, sizeof(tmpname1), "pg_temp_%u", oldrelid);
		snprintf(tmpname2, sizeof(tmpname2), "pg_temp_%u", newrelid);

		exchange_permissions(RelationGetRelid(oldrel),
							 RelationGetRelid(newrel));
		CommandCounterIncrement();

		heap_close(newrel, NoLock);
		heap_close(oldrel, NoLock);

		/* rename rel renames the type too */
		renamerel(oldrelid, tmpname1, NULL);
		CommandCounterIncrement();
		RelationForgetRelation(oldrelid);

		/* MPP-6979: if the namespaces are different, switch them */
		if (newNspName)
		{
			/* move the old partition (which has a temporary name) to
			 * the new namespace 
			 */
			oldrel = heap_open(oldrelid, AccessExclusiveLock);
			AlterRelationNamespaceInternalTwo(oldrel,
											  oldrelid,
											  oldnspid, newnspid,
											  true,
											  newNspName);
			heap_close(oldrel, NoLock);
			CommandCounterIncrement();
			RelationForgetRelation(oldrelid);

			/* before we move the new table to the old namespace,
			 * rename it to a temporary name to avoid a name
			 * collision.  It would be nice to have an atomic
			 * operation to rename and renamespace a relation... 
			 */
			renamerel(newrelid, tmpname2, NULL);
			CommandCounterIncrement();
			RelationForgetRelation(newrelid);

			newrel = heap_open(newrelid, AccessExclusiveLock);
			AlterRelationNamespaceInternalTwo(newrel,
											  newrelid,
											  newnspid, oldnspid,
											  true,
											  oldNspName);
			heap_close(newrel, NoLock);
			CommandCounterIncrement();
			RelationForgetRelation(newrelid);
		}

		renamerel(newrelid, oldname, NULL);
		CommandCounterIncrement();
		RelationForgetRelation(newrelid);

		renamerel(oldrelid, newname, NULL);
		CommandCounterIncrement();
		RelationForgetRelation(oldrelid);

		CommandCounterIncrement();

		/* fix up partitioning rule if we're on the QD*/
		if (Gp_role == GP_ROLE_DISPATCH)
		{
			exchange_part_rule(oldrelid, newrelid);
			CommandCounterIncrement();

			/* Notify of name if did not use name for partition id spec */
			if ( orig_pid_type != AT_AP_IDName && orig_prule->isName )
				ereport(NOTICE,
						(errmsg("exchanged partition%s of "
								"%s with relation \"%s\"",
								orig_prule->partIdStr,
								orig_prule->relname,
								newname),
						 errOmitLocation(true)));
		}
	}
	tab->exchange_relid = newrelid;
#endif
}

/* ALTER TABLE ... MERGE PARTITION */
static void
ATPExecPartMerge(Relation rel,
                 AlterPartitionCmd *pc)
{
	Relation source = NULL;
	Relation target = NULL;

	if (Gp_role == GP_ROLE_UTILITY)
		return;
	else if (Gp_role == GP_ROLE_DISPATCH)
	{
		AlterPartitionId   *pid   = (AlterPartitionId *)pc->partid;
		PgPartRule   	   *prule1 = NULL;
		PgPartRule   	   *prule2 = NULL;
		PgPartRule   	   *prule3 = NULL;
		RangeVar		   *relrv1, *relrv2;
		Relation			r1, r2;

		prule1 = get_part_rule(rel, pid, true, true,
							   CurrentMemoryContext, NULL, false);

		/*
		 * XXX: get_namespace_name(prule1->topRule->parchildrelid) is wrong
		 * need the relnamespace not relid
		 */
		relrv1 = makeRangeVar(
					get_namespace_name(prule1->topRule->parchildrelid),
					get_rel_name(prule1->topRule->parchildrelid), -1);

		r1 = heap_openrv(relrv1, AccessExclusiveLock);

		prule2 = get_part_rule(rel, (AlterPartitionId *)pc->arg1,
							   true, true,
							   CurrentMemoryContext, NULL, false);

		relrv2 = makeRangeVar(
					get_namespace_name(prule2->topRule->parchildrelid),
					get_rel_name(prule2->topRule->parchildrelid), -1);

		r2 = heap_openrv(relrv2, AccessExclusiveLock);

		if (pc->arg2)
		{
			AlterPartitionCmd *pc2 = (AlterPartitionCmd *)pc->arg2;

			prule3 = get_part_rule(rel, (AlterPartitionId *)pc2->partid,
								   true, true, CurrentMemoryContext,
								   NULL, false);

			Assert(PointerIsValid(prule3));

			if (prule3->topRule->parchildrelid == prule1->topRule->parchildrelid)
			{
				source = r1;
				pc->partid = (Node *)relrv1;
				target = r2;
				pc->arg1 = (Node *)relrv2;
			}
			else if (prule3->topRule->parchildrelid == prule2->topRule->parchildrelid)
			{
				source = r2;
				pc->partid = (Node *)relrv2;
				target = r1;
				pc->arg1 = (Node *)relrv1;
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("target of MERGE must be either \"%s\" or "
								"\"%s\"",
								get_rel_name(prule1->topRule->parchildrelid),
								get_rel_name(prule2->topRule->parchildrelid)),
										   errOmitLocation(true)));

			}
		}
		else
		{
			source = r1;
			target = r2;
			pc->partid = (Node *)relrv1;
			pc->arg1 = (Node *)relrv2;
		}

		/* MPP-6929: metadata tracking */
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "PARTITION", "MERGE"
				);
	}
	else if (Gp_role == GP_ROLE_EXECUTE)
	{
		RangeVar *relrv;

		Assert(IsA(pc->partid, RangeVar));
		Assert(IsA(pc->arg1, RangeVar));

		relrv = (RangeVar *)pc->partid;
		source = heap_openrv(relrv, AccessExclusiveLock);

		relrv = (RangeVar *)pc->arg1;
		target = heap_openrv(relrv, AccessExclusiveLock);
	}

	elog(NOTICE, "merging");
	elog(NOTICE, "%s", RelationGetRelationName(source));

	elog(NOTICE, "with %s", RelationGetRelationName(target));

    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot MERGE PARTITION for relation \"%s\"",
                    RelationGetRelationName(rel)),
             			   errOmitLocation(true)));

}

static bool
atpxModifyListOverlap (Relation rel,
					   AlterPartitionId *pid,
					   PgPartRule   	*prule,
					   PartitionElem 	*pelem,
					   bool bAdd)
{
	if (prule->pNode->default_part && bAdd)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot MODIFY %s partition%s for "
						"relation \"%s\" to ADD values -- would "
						"overlap DEFAULT partition \"%s\"",
						"LIST",
						prule->partIdStr,
						RelationGetRelationName(rel),
						prule->pNode->default_part->parname),
				 errhint("need to SPLIT partition \"%s\"",
						 prule->pNode->default_part->parname),
								   errOmitLocation(true)));

	{
		ListCell 				*lc;
		PartitionValuesSpec 	*pVSpec;
		AlterPartitionId 	 	 pid2;
		PartitionNode 			*pNode 	= prule->pNode;
		int 				 	 partno = 0;
		CreateStmtContext    	 cxt;

		MemSet(&cxt, 0, sizeof(cxt));

		Assert (IsA(pelem->boundSpec, PartitionValuesSpec));

		MemSet(&pid2, 0, sizeof(AlterPartitionId));

		/* this function does transformExpr on the boundary specs */
		partno = atpxPart_validate_spec(makeNode(PartitionBy),
										&cxt,
										rel,
										NULL,		 /* CreateStmt */
										pelem,
										pNode,
										(pid->idtype == AT_AP_IDName) ?
										pid->partiddef : NULL,
										false,		 /* isDefault */
										PARTTYP_LIST, /* part_type */
										prule->partIdStr);

		pVSpec = (PartitionValuesSpec *)pelem->boundSpec;

		foreach(lc, pVSpec->partValues)
		{
			List			*vals  = lfirst(lc);
			PgPartRule   	*prule2 = NULL;

			pid2.idtype = AT_AP_IDValue;
			pid2.partiddef = (Node *)vals;
			pid2.location  = -1;

			prule2 = get_part_rule(rel, &pid2, false, false,
								   CurrentMemoryContext, NULL, false);

			if (bAdd)
			{
				/* if ADDing a value, should not match */

				if (prule2)
				{
					if (prule2->topRuleRank != prule->topRuleRank)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("cannot MODIFY LIST partition%s for "
										"relation \"%s\" -- "
										"would overlap "
										"existing partition%s",
										prule->partIdStr,
										RelationGetRelationName(rel),
										prule2->isName ?
										prule2->partIdStr : ""),
												   errOmitLocation(true)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("cannot MODIFY LIST partition%s for "
										"relation \"%s\" -- "
										"ADD value has duplicate in "
										"existing partition",
										prule->partIdStr,
										RelationGetRelationName(rel)),
												   errOmitLocation(true)));
				}
			}
			else /* DROP values */
			{
				/* if DROPping a value, it should only be in the
				 * specified partition
				 */

				if (!prule2)
				{
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("cannot MODIFY LIST partition%s for "
									"relation \"%s\" -- DROP value not found",
									prule->partIdStr,
									RelationGetRelationName(rel)),
											   errOmitLocation(true)));
				}

				if (prule2 && (prule2->topRuleRank != prule->topRuleRank))
				{
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("cannot MODIFY LIST partition%s for "
									"relation \"%s\" -- "
									"found DROP value in%s partition%s",
									prule->partIdStr,
									RelationGetRelationName(rel),
									prule2->isName ? "" : "other",
									prule2->isName ? prule2->partIdStr : ""),
											   errOmitLocation(true)));
				}
			}

		} /* end foreach */
	}

	return false;
} /* end atpxModifyListOverlap */

static bool
atpxModifyRangeOverlap (Relation 		 		 rel,
						AlterPartitionId 		*pid,
						PgPartRule   			*prule,
						PartitionElem 			*pelem)
{
	PgPartRule   		*prule2 	 = NULL;
	AlterPartitionId 	 pid2;
	PartitionNode 		*pNode 		 = prule->pNode;
	bool 				 bCheckStart = true;
	PartitionBoundSpec 	*pbs   		 = NULL;
	ParseState 			*pstate    	 = NULL;
	bool 				 bOverlap 	 = false;
	bool 				*isnull;
	TupleDesc 		 	 tupledesc 	 = RelationGetDescr(rel);
	Datum 				*d_start   	 = NULL;
	Datum 				*d_end 	   	 = NULL;
	Node *pRangeValList = NULL;
	int ii;

	Assert (IsA(pelem->boundSpec, PartitionBoundSpec));

	pbs = (PartitionBoundSpec *)pelem->boundSpec;

	for (ii = 0; ii < 2 ; ii++)
	{
		PartitionRangeItem 		*ri;

		if (bCheckStart) /* check START first, then END */
		{
			if (!(pbs->partStart))
			{
				bCheckStart = false;

				if (prule->topRule->parrangestart)
				{
					PartitionRangeItem *ri = makeNode(PartitionRangeItem);

					ri->location = -1;

					ri->partRangeVal =
							copyObject(prule->topRule->parrangestart);

					ri->partedge = prule->topRule->parrangestartincl ?
							PART_EDGE_INCLUSIVE :
							PART_EDGE_EXCLUSIVE;

					/* no start, so use current start */
					pbs->partStart = (Node *)ri;
				}

				continue;
			}
			ri = (PartitionRangeItem *)pbs->partStart;
		}
		else
		{
			/* no END, so we are done */
			if (!(pbs->partEnd))
			{

				if (prule->topRule->parrangeend)
				{
					PartitionRangeItem *ri = makeNode(PartitionRangeItem);

					ri->location = -1;

					ri->partRangeVal = copyObject(prule->topRule->parrangeend);

					ri->partedge = prule->topRule->parrangeendincl ?
							PART_EDGE_INCLUSIVE :
							PART_EDGE_EXCLUSIVE;

					/* no end, so use current end */
					pbs->partEnd = (Node *)ri;
				}

				break;
			}
			ri = (PartitionRangeItem *)pbs->partEnd;
		}

		MemSet(&pid2, 0, sizeof(AlterPartitionId));

		pid2.idtype = AT_AP_IDValue;
		pstate = make_parsestate(NULL);
		pRangeValList = (Node *) copyObject(ri->partRangeVal);
		pRangeValList = (Node *)
			transformExpressionList(pstate, (List *)pRangeValList);
		free_parsestate(&pstate);
		pid2.partiddef = pRangeValList;
		pid2.location  = -1;

		prule2 = get_part_rule(rel, &pid2, false, false,
							   CurrentMemoryContext, NULL, false);

		if (!prule2)
		{
			/* no rules matched -- this is ok as long as no
			 * default partition
			 */
			if (prule->pNode->default_part)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot MODIFY %s partition%s for "
								"relation \"%s\" to extend range -- would "
								"overlap DEFAULT partition \"%s\"",
								"RANGE",
								prule->partIdStr,
								RelationGetRelationName(rel),
								prule->pNode->default_part->parname),
						 errhint("need to SPLIT partition \"%s\"",
								 prule->pNode->default_part->parname),
										   errOmitLocation(true)));

			/* if only 1 partition no further check needed */
			if (1 == list_length(pNode->rules))
				continue;

			if (bCheckStart)
			{
				bool bstat;
				PartitionRule *a_rule;

				/* check for adjacent partition */
				if (1 == prule->topRuleRank)
					continue; /* no previous, so changing start is ok */

				MemSet(&pid2, 0, sizeof(AlterPartitionId));

				pid2.idtype = AT_AP_IDRank;
				pid2.partiddef = (Node *)makeInteger(prule->topRuleRank - 1);
				pid2.location  = -1;

				prule2 = get_part_rule(rel, &pid2, false, false,
									   CurrentMemoryContext, NULL, false);

				Assert(prule2);

				a_rule = prule2->topRule;

				/* just check against end of adjacent partition */
				d_start =
						magic_expr_to_datum(rel, pNode,
											pRangeValList, &isnull);

				bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   ">",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc);
				if (bstat)
				{
					/* end > new start - overlap */
					bOverlap = true;
					break;
				}

				/* could be the case that new start == end of
				 * previous.  This is ok if they have opposite
				 * INCLUSIVE/EXCLUSIVE.
				 */

				bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   "=",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc);

				if (bstat)
				{
					if (a_rule->parrangeendincl == 
						(ri->partedge == PART_EDGE_INCLUSIVE))
					{
						/* start and end must be of opposite
						 * types, else they overlap */
						bOverlap = true;
						break;
					}
				}
			}
			else /* check the end */
			{
				bool bstat;
				PartitionRule *a_rule;

				/* check for adjacent partition */
				if (list_length(pNode->rules) == prule->topRuleRank)
					continue; /* no next, so changing end is ok */

				MemSet(&pid2, 0, sizeof(AlterPartitionId));

				pid2.idtype = AT_AP_IDRank;
				pid2.partiddef = (Node *)makeInteger(prule->topRuleRank + 1);
				pid2.location  = -1;

				prule2 = get_part_rule(rel, &pid2, false, false,
									   CurrentMemoryContext, NULL, false);

				Assert(prule2);

				a_rule = prule2->topRule;

				/* just check against start of adjacent partition */
				d_end =
						magic_expr_to_datum(rel, pNode,
											pRangeValList, &isnull);

				bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   "<",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc);
				if (bstat)
				{
					/* start < new end - overlap */
					bOverlap = true;
					break;
				}

				/* could be the case that new end == start of
				 * next.  This is ok if they have opposite
				 * INCLUSIVE/EXCLUSIVE.
				 */

				bstat =
						compare_partn_opfuncid(pNode,
											   "pg_catalog",
											   "=",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc);

				if (bstat)
				{
					if (a_rule->parrangeendincl == 
						(ri->partedge == PART_EDGE_INCLUSIVE))
					{
						/* start and end must be of opposite
						 * types, else they overlap */
						bOverlap = true;
						break;
					}
				}
			} /* end else check the end */

		}
		else
		{
			/* matched a rule - definitely a problem if the range was
			 * inclusive
			 */
			if (prule2->topRuleRank != prule->topRuleRank)
			{
				if (0 == prule2->topRuleRank)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("cannot MODIFY %s partition%s for "
									"relation \"%s\" to extend range -- would "
									"overlap DEFAULT partition \"%s\"",
									"RANGE",
									prule->partIdStr,
									RelationGetRelationName(rel),
									prule->pNode->default_part->parname),
							 errhint("need to SPLIT partition \"%s\"",
									 prule->pNode->default_part->parname),
											   errOmitLocation(true)));


				if (ri->partedge == PART_EDGE_INCLUSIVE)
				{
					bOverlap = true;
					break;
				}

				/* range was exclusive -- need to do some checking */
				if (bCheckStart)
				{
					bool bstat;
					PartitionRule *a_rule = prule2->topRule;

					/* check for adjacent partition */
					if ((prule->topRuleRank - 1) != prule2->topRuleRank)
					{
						bOverlap = true;
						break;
					}

					/* just check against end of adjacent partition */
					d_start =
							magic_expr_to_datum(rel, pNode,
												pid2.partiddef, &isnull);

					bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   ">",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc);
					if (bstat)
					{
						/* end > new start - overlap */
						bOverlap = true;
						break;
					}

					/* Must be the case that new start == end of
					 * a_rule (because if the end < new start then how
					 * could we find it in the interval for prule ?)
					 * This is ok if they have opposite
					 * INCLUSIVE/EXCLUSIVE ->  New partition does not
					 * overlap.
					 */

					Assert (compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "=",
											   (List *)a_rule->parrangeend,
											   d_start, isnull, tupledesc));

					if (a_rule->parrangeendincl == 
						(ri->partedge == PART_EDGE_INCLUSIVE))
					{
						/* start and end must be of opposite
						 * types, else they overlap
						 */
						bOverlap = true;
						break;
					}
				}
				else /* check the end */
				{
					bool bstat;
					PartitionRule *a_rule = prule2->topRule;

					/* check for adjacent partition */
					if ((prule->topRuleRank + 1) != prule2->topRuleRank)
					{
						bOverlap = true;
						break;
					}

					/* just check against start of adjacent partition */
					d_end =
							magic_expr_to_datum(rel, pNode,
												pid2.partiddef, &isnull);

					bstat =
							compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "<",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc);
					if (bstat)
					{
						/* start < new end - overlap */
						bOverlap = true;
						break;
					}

					/* Must be the case that new end = start of
					 * a_rule (because if the start > new end then how
					 * could we find it in the interval for prule ?)
					 * This is ok if they have opposite
					 * INCLUSIVE/EXCLUSIVE ->  New partition does not
					 * overlap.
					 */

					Assert (compare_partn_opfuncid(pNode,
												   "pg_catalog",
												   "=",
											   (List *)a_rule->parrangestart,
											   d_end, isnull, tupledesc));

					if (a_rule->parrangestartincl == 
						(ri->partedge == PART_EDGE_INCLUSIVE))
					{
						/* start and end must be of opposite
						 * types, else they overlap
						 */
						bOverlap = true;
						break;
					}
				} /* end else check the end */
			} /* end if (prule2->topRuleRank != prule->topRuleRank) */
		}

		/* if checked START, then check END.  If checked END, then done */
		if (!bCheckStart)
			break;
		if (bCheckStart)
			bCheckStart = false;
	} /* end for */

	if (bOverlap)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot MODIFY RANGE partition%s for "
						"relation \"%s\" -- "
						"would overlap "
						"existing partition%s",
						prule->partIdStr,
						RelationGetRelationName(rel),
						(prule2 && prule2->isName) ?
						prule2->partIdStr : ""),
								   errOmitLocation(true)));

	{
		int 				 	 partno = 0;
		CreateStmtContext    	 cxt;

		MemSet(&cxt, 0, sizeof(cxt));

		/* this function does transformExpr on the boundary specs */
		partno = atpxPart_validate_spec(makeNode(PartitionBy),
										&cxt,
										rel,
										NULL,		 /* CreateStmt */
										pelem,
										pNode,
										(pid->idtype == AT_AP_IDName) ?
										pid->partiddef : NULL,
										false,		 /* isDefault */
										PARTTYP_RANGE, /* part_type */
										prule->partIdStr);
	}


	return false;
} /* end atpxModifyRangeOverlap */


/* ALTER TABLE ... MODIFY PARTITION */
static void
ATPExecPartModify(Relation rel,
                  AlterPartitionCmd *pc)
{
	AlterPartitionId *pid = (AlterPartitionId *)pc->partid;
	PgPartRule   *prule = NULL;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	prule = get_part_rule(rel, pid, true, true, CurrentMemoryContext, NULL,
						  false);

	if (prule)
	{
		AlterPartitionCmd 		*pc2 	= (AlterPartitionCmd *)pc->arg1;
		AlterPartitionCmd 		*pc3 	= (AlterPartitionCmd *)pc2;
		DefElem    				*def 	= (DefElem *)pc3->arg1;
		bool 					 bStart = false;
		bool 					 bEnd 	= false;
		bool 					 bAdd 	= false;
		bool 					 bDrop 	= false;
		char           *parTypName = NULL;

		if (0 == strcmp(def->defname, "START"))
			bStart = true;
		else if (0 == strcmp(def->defname, "END"))
			bEnd = true;
		else if (0 == strcmp(def->defname, "ADD"))
			bAdd = true;
		else if (0 == strcmp(def->defname, "DROP"))
			bDrop = true;
		else Assert(false);

		switch (prule->pNode->part->parkind)
		{
			case 'h': /* hash */
				parTypName = "HASH";
				break;
			case 'r': /* range */
				parTypName = "RANGE";
				break;
			case 'l': /* list */
				parTypName = "LIST";
				break;
			default:
				elog(ERROR, "unrecognized partitioning kind '%c'",
					 prule->pNode->part->parkind);
				Assert(false);
				break;
		} /* end switch */

		/* cannot modify default partitions */
		if (prule->pNode->default_part == prule->topRule)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot MODIFY DEFAULT %s partition%s "
							"for relation \"%s\"",
							parTypName,
							((prule && prule->isName) ? prule->partIdStr : ""),
							RelationGetRelationName(rel)),
									   errOmitLocation(true)));

		if ((bStart || bEnd) && ('r' != prule->pNode->part->parkind))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid use of %s boundary specification "
							"in partition%s of type %s",
							"RANGE",
							((prule && prule->isName) ? prule->partIdStr : ""),
							parTypName
							 ),
									   errOmitLocation(true)));

		if ((bAdd || bDrop) && ('l' != prule->pNode->part->parkind))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid use of %s boundary specification "
							"in partition%s of type %s",
							"LIST",
							((prule && prule->isName) ? prule->partIdStr : ""),
							parTypName
							 ),
									   errOmitLocation(true)));

		if (bAdd || bDrop)
		{
			bool stat = atpxModifyListOverlap(rel, pid, prule,
											  (PartitionElem *)pc3->arg2,
											  bAdd);
			stat = false;
		}
		if (bStart || bEnd)
		{
			bool stat = atpxModifyRangeOverlap(rel, pid, prule,
											   (PartitionElem *)pc3->arg2);
			stat = false;
		}


		if (0)
		parruleord_reset_rank(
				prule->pNode->part->partid,
				prule->pNode->part->parlevel,
				prule->topRule->parparentoid,
				prule->topRule->parruleord,
				CurrentMemoryContext);

		/* MPP-6929: metadata tracking */
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "PARTITION", "MODIFY"
				);
	}

	if (0)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot MODIFY PARTITION for relation \"%s\"",
                    RelationGetRelationName(rel)),
             			   errOmitLocation(true)));

}

/* ALTER TABLE ... RENAME PARTITION */
static void atpxSkipper(PartitionNode *pNode, int *skipped)
{
	ListCell *lc;

	if (!pNode) return;

	/* add entries for rules at current level */
	foreach(lc, pNode->rules)
	{
		PartitionRule 	*rule = lfirst(lc);

		if (skipped) *skipped += 1;

		if (rule->children)
			atpxSkipper(rule->children, skipped);
	} /* end foreach */

	/* and the default partition */
	if (pNode->default_part)
	{
		PartitionRule 	*rule = pNode->default_part;

		if (skipped) *skipped += 1;

		if (rule->children)
			atpxSkipper(rule->children, skipped);
	}
} /* end atpxSkipper */

static List *
build_rename_part_recurse(PartitionRule *rule, const char *old_parentname,
						  const char *new_parentname,
						  int *skipped)
{

	RangeVar 		*rv;
	Relation		 rel;
	char *relname = NULL;
	char newRelNameBuf[(NAMEDATALEN*2)];
	List *l1 = NIL;

	rel = heap_open(rule->parchildrelid, AccessShareLock);

	relname = pstrdup(RelationGetRelationName(rel));

	rv = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
					  relname, -1);

	/* unlock, because we have a lock on the master */
	heap_close(rel, AccessShareLock);

	/* 
	 * The child name should contain the old parent name as a
	 * prefix - check the length and compare to make sure.
	 *
	 * To build the new child name, just use the new name as a
	 * prefix, and use the remainder of the child name (the part
	 * after the old parent name prefix) as the suffix.
	 */
	if (strlen(old_parentname) > strlen(relname))
	{
		if (skipped)
			*skipped += 1;

		atpxSkipper(rule->children, skipped);
	}
	else
	{
		if (0 != (strncmp(old_parentname, relname, strlen(old_parentname))))
		{
			if (skipped)
				*skipped += 1;
			atpxSkipper(rule->children, skipped);
		}
		else
		{
			snprintf(newRelNameBuf, sizeof(newRelNameBuf), "%s%s",
					 new_parentname, relname + strlen(old_parentname));
	
			if (strlen(newRelNameBuf) > NAMEDATALEN)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("relation name \"%s\" for child partition "
								"is too long",
								newRelNameBuf)));
		
			l1 = lappend(l1, list_make2(rv, pstrdup(newRelNameBuf)));
		
			/* add the child lists next (not first) */
			{
				List *l2 = NIL;
		
				if (rule->children)
					l2 = atpxRenameList(rel, rule->children,
										relname, newRelNameBuf, skipped);
		
				if (l2)
					l1 = list_concat(l1, l2);
			}
		}
	}
	return l1;
}

static List *
atpxRenameList(Relation rel, PartitionNode *pNode,
			   char *old_parentname, const char *new_parentname, int *skipped)
{
	List *l1 = NIL;
	ListCell *lc;

	if (!pNode)
		return l1;

	/* add entries for rules at current level */
	foreach(lc, pNode->rules)
	{
		PartitionRule 	*rule = lfirst(lc);

		l1 = list_concat(l1,
						 build_rename_part_recurse(rule,
												   old_parentname,
												   new_parentname,
												   skipped));
	} /* end foreach */

	/* and the default partition */
	if (pNode->default_part)
	{
		PartitionRule 	*rule = pNode->default_part;

		l1 = list_concat(l1,
						 build_rename_part_recurse(rule,
												   old_parentname,
												   new_parentname,
												   skipped));
	}

	return l1;
} /* end atpxRenameList */

static void
ATPExecPartRename(Relation rel,
                  AlterPartitionCmd *pc)
{
	AlterPartitionId 	*pid 	   = (AlterPartitionId *)pc->partid;
	PgPartRule   		*prule 	   = NULL;
	PartitionNode  		*pNode     = NULL;
	AlterPartitionId 	*locPid    = pid;	/* local pid if IDRule */
	PgPartRule* 		 par_prule = NULL;	/* prule for parent if IDRule */
	char 		 		 lRelNameBuf[(NAMEDATALEN*2)];
	char 				*lrelname=NULL;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	locPid =
			wack_pid_relname(pid,
							 &pNode,
							 rel,
							 &par_prule,
							 &lrelname,
							 lRelNameBuf);

	prule = get_part_rule(rel, pid, true, true, CurrentMemoryContext, NULL,
						  false);

	if (prule)
	{
		AlterPartitionId		 newpid;
		PgPartRule   		   	*prule2 	 = NULL;
		Relation			 	 targetrelation;
		char        		 	 targetrelname[NAMEDATALEN];
		Relation			 	 parentrelation;
		Oid						 namespaceId;
		char	   			   	*newpartname = strVal(pc->arg1);
		char	   			   	*relname;
		char	  			 	 parentname[NAMEDATALEN];
		int 				 	 partDepth 	 = prule->pNode->part->parlevel;
		RenameStmt 			   	*renStmt 	 = makeNode(RenameStmt);
		DestReceiver 		   	*dest  		 = None_Receiver;
		Relation				 part_rel;
		SysScanDesc				 scan;
		ScanKeyData				 key;
		HeapTuple				 tuple;
		Form_pg_partition_rule	 pgrule;
		List 					*renList 	 = NIL;
		int 					 skipped 	 = 0;
		int 					 renamed 	 = 0;

		newpid.idtype = AT_AP_IDName;
		newpid.partiddef = pc->arg1;
		newpid.location = -1;

		/* ERROR if exists */
		prule2 = get_part_rule1(rel, &newpid, true, false,
								CurrentMemoryContext, NULL,
								pNode,
								lrelname,
								NULL);

		targetrelation = relation_open(prule->topRule->parchildrelid,
									   AccessExclusiveLock);

		StrNCpy(targetrelname, RelationGetRelationName(targetrelation),
				NAMEDATALEN);

		namespaceId = RelationGetNamespace(targetrelation);

		relation_close(targetrelation, AccessExclusiveLock);

		if (0 == prule->topRule->parparentoid)
		{
			StrNCpy(parentname,
					RelationGetRelationName(rel), NAMEDATALEN);
		}
		else
		{
			Assert(par_prule);
			if (par_prule)
			{
				/* look in the parent prule */
				parentrelation =
					RelationIdGetRelation(par_prule->topRule->parchildrelid);
				StrNCpy(parentname,
						RelationGetRelationName(parentrelation), NAMEDATALEN);
				RelationClose(parentrelation);
			}
		}

		/* MPP-3523: the "label" portion of the new relation is
		 * prt_`newpartname', and makeObjectName won't truncate this
		 * portion of the partition name -- it will assert instead.
		 */
		if (strlen(newpartname) > (NAMEDATALEN - 8))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("name \"%s\" for child partition "
							"is too long",
							newpartname)));

		relname = ChoosePartitionName(parentname,
									  partDepth,
									  newpartname,
									  namespaceId);
		/* does CommandCounterIncrement */

		renStmt->renameType = OBJECT_TABLE;
		renStmt->relation = makeRangeVar(get_namespace_name(namespaceId),
										 pstrdup(targetrelname), -1);

		renStmt->subname = NULL;
		renStmt->newname = relname;
		renStmt->bAllowPartn = true; /* allow rename of partitions */

		if (prule && prule->topRule && prule->topRule->children)
				pNode = prule->topRule->children;
		else
				pNode = NULL;

		/* rename the children as well */
		renList = atpxRenameList(rel, pNode, targetrelname, relname, &skipped);

		part_rel = heap_open(PartitionRuleRelationId, RowExclusiveLock);

		ScanKeyInit(&key,
					ObjectIdAttributeNumber,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(prule->topRule->parruleid));
		scan = systable_beginscan(part_rel, PartitionRuleOidIndexId, true,
								  SnapshotNow, 1, &key);

		tuple = systable_getnext(scan);
		Insist(HeapTupleIsValid(tuple));

		tuple = heap_copytuple(tuple);
		pgrule = (Form_pg_partition_rule)GETSTRUCT(tuple);
		namestrcpy(&(pgrule->parname), newpartname);

		simple_heap_update(part_rel, &tuple->t_self, tuple);
	    CatalogUpdateIndexes(part_rel, tuple);

		systable_endscan(scan);
		heap_freetuple(tuple);
		heap_close(part_rel, NoLock);

		CommandCounterIncrement();

		ProcessUtility((Node *) renStmt,
					   synthetic_sql,
					   NULL,
					   false, /* not top level */
					   dest, 
					   NULL);

		/* process children if there are any */
		if (renList)
		{
			ListCell 		*lc;

			foreach(lc, renList)
			{
				ListCell 		*lc2;
				List  			*lpair = lfirst(lc);

				lc2 = list_head(lpair);

				renStmt->relation = (RangeVar *)lfirst(lc2);
				lc2 = lnext(lc2);
				renStmt->newname = (char *)lfirst(lc2);

				ProcessUtility((Node *) renStmt,
							   synthetic_sql,
							   NULL, 
							   false, /* not top level */
							   dest, 
							   NULL);
				renamed++;
			}
		}

		/* MPP-6929: metadata tracking */
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "PARTITION", "RENAME"
				);

		/* Notify of name if did not use name for partition id spec */
		if ((pid->idtype != AT_AP_IDName)
			&& prule->isName)
			ereport(NOTICE,
					(errmsg("renamed partition%s to \"%s\" for %s",
							prule->partIdStr,
							newpartname,
							prule->relname)));

		/* MPP-3542: warn when skip child partitions */
		if (skipped)
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("renamed %d partitions, skipped %d child partitions due to name truncation",
							renamed, skipped),
									   errOmitLocation(true)));
		}

	}

} /* end ATPExecPartRename */


/* ALTER TABLE ... SET SUBPARTITION TEMPLATE  */
static void
ATPExecPartSetTemplate(AlteredTableInfo *tab,
					   Relation rel,
                       AlterPartitionCmd *pc)
{
	AlterPartitionId	*pid   = (AlterPartitionId *)pc->partid;
	PgPartRule			*prule = NULL;
	int					 lvl   = 1;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	/* set template for top level table */
    if (pid && (pid->idtype != AT_AP_IDName))
    {
		Assert((pid->idtype == AT_AP_IDRule) && IsA(pid->partiddef, List));

		/* MPP-5941: work correctly with many levels of templates */
		/* wah! the idrule is invalid, so can't use get_part_rule.
		 * So pull the pgpartrule directly from the idrule (yuck!)
		 */
		prule = (PgPartRule *)linitial((List*)pid->partiddef);

		Assert(prule);

		/* current pnode level is one below current (our parent), and
		 * we want the one above us (our subpartition), so add 2
		 */
		lvl = prule->pNode->part->parlevel + 2;

		Assert (lvl > 1);
	}

	{
		/* relid, level, no parent */
		switch (del_part_template(RelationGetRelid(rel), lvl, 0))
		{
			case 0:
				if (pc->arg1)
				{
					/* no prior template - just add new one */
					ereport(NOTICE,
							(errmsg("%s level %d "
									"subpartition template specification "
									"for relation \"%s\"",
									"adding",
									lvl,
									RelationGetRelationName(rel))));
				}
				else
				{
					/* tried to drop non-existent template */
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("relation \"%s\" does not have a "
									"level %d "
									"subpartition template specification",
									RelationGetRelationName(rel),
									lvl),
											   errOmitLocation(true)));
				}
				break;
			case 1:
					/* if have new spec,
					 * note old spec is being replaced,
					 * else just note it is dropped
					 */
					ereport(NOTICE,
							(errmsg("%s level %d "
									"subpartition template specification "
									"for relation \"%s\"",
									(pc->arg1) ? "replacing" : "dropped",
									lvl,
									RelationGetRelationName(rel))));
				break;
			default:
					elog(ERROR,
						 "could not drop "
						 "level %d "
						 "subpartition template specification "
						 "for relation \"%s\"",
						 lvl,
						 RelationGetRelationName(rel));
					break;
		}

	}

	if (pc->arg1)
		ATPExecPartAdd(tab, rel, pc, AT_PartSetTemplate);

	/* MPP-6929: metadata tracking */
	MetaTrackUpdObject(RelationRelationId,
					   RelationGetRelid(rel),
					   GetUserId(),
					   "ALTER", "SET SUBPARTITION TEMPLATE"
			);


} /* end ATPExecPartSetTemplate */

static bool
partrule_walker(Node *node, void *context)
{
	part_rule_cxt *p = (part_rule_cxt *)context;

	if (node == NULL)
		return false;
	if (IsA(node, PgPartRule))
	{
		PgPartRule *pg = (PgPartRule *)node;

		partrule_walker((Node *)pg->topRule, p);
		partrule_walker((Node *)pg->pNode, p);
		return false;
	}
	else if (IsA(node, Partition))
	{
		return false;
	}
	else if (IsA(node, PartitionRule))
	{
		PartitionRule *pr = (PartitionRule *)node;

		if (pr->parchildrelid == p->old)
			pr->parchildrelid = p->new;

		return partrule_walker((Node *)pr->children, p);

	}
	else if (IsA(node, PartitionNode))
	{
		PartitionNode *pn = (PartitionNode *)node;
		ListCell *lc;

		partrule_walker((Node *)pn->default_part, p);
		foreach(lc, pn->rules)
		{
			PartitionRule *r = lfirst(lc);
			partrule_walker((Node *)r, p);
		}
		return false;
	}

	return expression_tree_walker(node, partrule_walker, p);
}

/* 
 * Build a basic ResultRelInfo for executing split. We only need
 * the relation descriptor and index information.
 */
static ResultRelInfo *
make_split_resultrel(Relation rel)
{
	ResultRelInfo *rri;

	rri = palloc0(sizeof(ResultRelInfo));
	rri->type = T_ResultRelInfo;
	rri->ri_RelationDesc = rel;
	rri->ri_NumIndices = 0;

	ExecOpenIndices(rri);
	return rri;
}

/*
 * Close indexes and free memory
 */
static void
destroy_split_resultrel(ResultRelInfo *rri)
{
	ExecCloseIndices(rri);

	/* 
	 * Don't do anything with the relation descriptor, that's our caller's job
	 */
	pfree(rri);

}

static void
split_rows(Relation intoa, Relation intob, Relation temprel)
{
	ResultRelInfo *rria = make_split_resultrel(intoa);
	ResultRelInfo *rrib = make_split_resultrel(intob);
	EState *estate = CreateExecutorState();
	TupleTableSlot *tts = MakeSingleTupleTableSlot(temprel->rd_att);
	HeapScanDesc heapscan = NULL;
	AppendOnlyScanDesc aoscan = NULL;
    AOCSScanDesc aocsscan = NULL;
    bool *aocsproj = NULL;
	MemoryContext oldCxt;
	AppendOnlyInsertDesc aoinsertdesc_a = NULL;
	AppendOnlyInsertDesc aoinsertdesc_b = NULL;
    AOCSInsertDesc aocsinsertdesc_a = NULL;
    AOCSInsertDesc aocsinsertdesc_b = NULL;
	ExprState *achk = NULL;
	ExprState *bchk = NULL;

	/* constr might not be defined if this is a default partition */
	if (intoa->rd_att->constr && intoa->rd_att->constr->num_check)
	{
		Node *bin = (Node *)make_ands_implicit(
				(Expr *)stringToNode(intoa->rd_att->constr->check[0].ccbin));
		achk = ExecPrepareExpr((Expr *)bin, estate);
	}

	if (intob->rd_att->constr && intob->rd_att->constr->num_check)
	{
		Node *bin = (Node *)make_ands_implicit(
				(Expr *)stringToNode(intob->rd_att->constr->check[0].ccbin));
		bchk = ExecPrepareExpr((Expr *)bin, estate);
	}

	/* be careful about AO vs. normal heap tables */
	if (RelationIsHeap(temprel))
		heapscan = heap_beginscan(temprel, SnapshotNow, 0, NULL);
	else if (RelationIsAoRows(temprel))
		aoscan = appendonly_beginscan(temprel, SnapshotNow, 0, NULL);
	else if (RelationIsAoCols(temprel))
	{
		int nvp = temprel->rd_att->natts;
		int i;

		aocsproj = (bool *) palloc(sizeof(bool) * nvp);
		for(i=0; i<nvp; ++i)
			aocsproj[i] = true;

		aocsscan = aocs_beginscan(temprel, SnapshotNow, NULL /* relationTupleDesc */, aocsproj);
	}
	oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	while (true)
	{
		ExprContext *econtext = GetPerTupleExprContext(estate);
		bool targetIsA;
		Relation targetRelation;
		AppendOnlyInsertDesc *targetAODescPtr;
		AOCSInsertDesc *targetAOCSDescPtr;
		ItemPointer tid;
		ResultRelInfo *currri = NULL;

		/* read next tuple from temprel */
		if (RelationIsHeap(temprel))
		{
			HeapTuple tuple;

			tuple = heap_getnext(heapscan, ForwardScanDirection);
			if (!HeapTupleIsValid(tuple))
				break;

			tuple = heap_copytuple(tuple);
			ExecStoreHeapTuple(tuple, tts, InvalidBuffer, false);
		}
		else if (RelationIsAoRows(temprel))
		{
			MemTuple mtuple;

			mtuple = appendonly_getnext(aoscan, ForwardScanDirection, tts);
			if (!PointerIsValid(mtuple))
				break;

			TupClearShouldFree(tts);
		}
		else if (RelationIsAoCols(temprel))
		{
			aocs_getnext(aocsscan, ForwardScanDirection, tts);
			if (TupIsNull(tts))
				break;
		}

		econtext->ecxt_scantuple = tts;

		/* determine if we are inserting into a or b */
		if (achk)
		{
			targetIsA = ExecQual((List *)achk, econtext, false);
		}
		else
		{
			Assert(PointerIsValid(bchk));

			targetIsA = ! ExecQual((List *)bchk, econtext, false);
		}

		/* load variables for the specific target */
		if (targetIsA)
		{
			targetRelation = intoa;
			targetAODescPtr = &aoinsertdesc_a;
			targetAOCSDescPtr = &aocsinsertdesc_a;
			currri = rria;
		}
		else
		{
			targetRelation = intob;
			targetAODescPtr = &aoinsertdesc_b;
			targetAOCSDescPtr = &aocsinsertdesc_b;
			currri = rrib;
		}

		/* insert into the target table */

		if (RelationIsAoRows(targetRelation))
		{
			MemTuple mtuple = ExecFetchSlotMemTuple(tts, false);
			Oid 		tupleOid;
			AOTupleId	aoTupleId;

			if (!(*targetAODescPtr))
			{
				MemoryContextSwitchTo(oldCxt);
				*targetAODescPtr = appendonly_insert_init(targetRelation,
														  RESERVED_SEGNO);
				MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
			}

			appendonly_insert(*targetAODescPtr, mtuple, &tupleOid, &aoTupleId);

			/* cache TID for later updating of indexes */
			tid = (ItemPointer)&aoTupleId;
		}
		else if (RelationIsAoCols(targetRelation))
		{
			AOTupleId	aoTupleId;

			if (!*targetAOCSDescPtr)
			{
				MemoryContextSwitchTo(oldCxt);
				*targetAOCSDescPtr = aocs_insert_init(targetRelation,
													  RESERVED_SEGNO);
				MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
			}

			aocs_insert(*targetAOCSDescPtr, tts);
			aoTupleId = *((AOTupleId*)slot_get_ctid(tts));

			/* cache TID for later updating of indexes */
			tid = (ItemPointer)&aoTupleId;
		}
		else
		{
			HeapTuple tuple;

			/* Better be a heap table ... */
			Assert(RelationIsHeap(targetRelation));

			/* guaranteed to be a copy, see above */
			tuple = ExecFetchSlotHeapTuple(tts);
			simple_heap_insert(targetRelation, tuple);

			/* cache TID for later updating of indexes */
			tid = &(((HeapTuple)tuple)->t_self);
			setLastTid(tid);
		}

		if (currri->ri_NumIndices > 0)
		{
			estate->es_result_relation_info = currri;
			ExecInsertIndexTuples(tts, tid, estate, false);
			estate->es_result_relation_info = NULL;
		}

		/* done, clean up context for this pass */
		ResetExprContext(econtext);
	}

	if (aoinsertdesc_a)
		appendonly_insert_finish(aoinsertdesc_a);
	if (aoinsertdesc_b)
		appendonly_insert_finish(aoinsertdesc_b);
	if (aocsinsertdesc_a)
		aocs_insert_finish(aocsinsertdesc_a);
	if (aocsinsertdesc_b)
		aocs_insert_finish(aocsinsertdesc_b);

	MemoryContextSwitchTo(oldCxt);
	ExecDropSingleTupleTableSlot(tts);

	if (RelationIsHeap(temprel))
		heap_endscan(heapscan);
	else if (RelationIsAoRows(temprel))
		appendonly_endscan(aoscan);
	else if (RelationIsAoCols(temprel))
	{
		pfree(aocsproj);
		aocs_endscan(aocsscan);
	}

	destroy_split_resultrel(rria);
	destroy_split_resultrel(rrib);
}

/* ALTER TABLE ... SPLIT PARTITION */

/* 
   atpxSplitDropRule: walk the partition rules, eliminating rules that
   match parruleid and paroid.  If topRule is supplied, walk his
   subtree as well (it may differ from pnode if the PgPartRule was
   copied).
*/
static void
atpxSplitDropRule(PartitionNode *pNode, PartitionRule *topRule,
				  Oid parruleid, Oid paroid)
{
	ListCell *lc;
	ListCell *lcprev = NULL, *lcdel = NULL;

	if (!pNode)
		return;

	foreach(lc, pNode->rules)
	{
		PartitionRule *rule = lfirst(lc);

		if (rule->children)
			atpxSplitDropRule(rule->children, NULL, 
							  parruleid, paroid);

		if ((parruleid == rule->parruleid) &&
			(paroid == rule->paroid))
		{
			lcdel = lc; /* should only be one match */
			break;
		}
		lcprev = lc;
	}
	if (lcdel)
		pNode->rules = list_delete_cell(pNode->rules, lcdel, lcprev);

	/* and the default partition */
	if (pNode->default_part)
	{
		PartitionRule *rule = pNode->default_part;

		if (rule->children)
				atpxSplitDropRule(rule->children, NULL,
								  parruleid, paroid);

		if ((parruleid == rule->parruleid) &&
			(paroid == rule->paroid))
			pNode->default_part = NULL;
	}

	/* check optional topRule */
	if (topRule && topRule->children)
		atpxSplitDropRule(topRule->children, NULL,
						  parruleid, paroid);

} /* end atpxSplitDropRule */

/* Given a Relation, make a distributed by () clause for parser consumption. */
static List *
make_dist_clause(Relation rel)
{
	int i;
	List *distro = NIL;

	for (i = 0; i < rel->rd_cdbpolicy->nattrs; i++)
	{
		AttrNumber attno = rel->rd_cdbpolicy->attrs[i];
		TupleDesc tupdesc = RelationGetDescr(rel);
		Value *attstr;
		NameData attname;

		attname = tupdesc->attrs[attno - 1]->attname;
		attstr = makeString(pstrdup(NameStr(attname)));

		distro = lappend(distro, attstr);
	}

	if (!distro)
	{
		/* must be random distribution */
		distro = list_make1(NULL);
	}
	return distro;
}

static void
ATPExecPartSplit(Relation rel,
                 AlterPartitionCmd *pc)
{
	Relation temprel = NULL;
	Relation intoa = NULL;
	Relation intob = NULL;
	Oid temprelid;

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CreateStmt *ct = makeNode(CreateStmt);
		char tmpname[NAMEDATALEN];
		InhRelation *inh = makeNode(InhRelation);
		DestReceiver *dest = None_Receiver;
		Node *at = lsecond((List *)pc->arg1);
		AlterPartitionCmd *pc2 = (AlterPartitionCmd *)pc->arg2;
		AlterPartitionId *pid = (AlterPartitionId *)pc->partid;
		AlterTableStmt *ats = makeNode(AlterTableStmt);
		RangeVar *rv;
		AlterTableCmd *cmd = makeNode(AlterTableCmd);
		AlterPartitionCmd *mypc = makeNode(AlterPartitionCmd);
		AlterPartitionCmd *mypc2 = makeNode(AlterPartitionCmd);
		AlterPartitionId *idpid;
		RangeVar *tmprv;
		PgPartRule *prule;
		DropStmt *ds = makeNode(DropStmt);
		List *parsetrees;
		Query *q;
		char *nspname = get_namespace_name(RelationGetNamespace(rel));
		char *relname = get_rel_name(RelationGetRelid(rel));
		Oid relid = RelationGetRelid(rel);
		RangeVar *rva = NULL;
		RangeVar *rvb = NULL;
		int into_exists = 0; /* which into partition exists? */
		int i;
		AlterPartitionId *intopid1 = NULL;
		AlterPartitionId *intopid2 = NULL;
		int default_pos = 0;
		Oid rel_to_drop = InvalidOid;
		AlterPartitionId *aapid = NULL; /* just for alter partition pids */
		Relation existrel;
		List *existstorage_opts;
		StdRdOptions *existopts;
		ListCell *lc;
		char *defparname = NULL; /* name of default partition (if specified) */
		List *distro = NIL;

		/* Get target meta data */
		prule = get_part_rule(rel, pid, true, true,
							  CurrentMemoryContext, NULL, false);
		/*
		 * In order to implement SPLIT, we do the following:
		 *
		 * 1) Build a temporary table T on all nodes.
		 * 2) Exchange that table with the target partition P
		 *    Now, T has all the data or P
		 * 3) Drop partition P
		 * 4) Create two new partitions in the place of the old one
		 */

		/* look up INTO clause info, if the user supplied it */
		if (!pc2) /* no INTO */
		{
			if (prule->topRule->parisdefault)
			{
				defparname = pstrdup(prule->topRule->parname);
				into_exists = 2;
			}
		}
		else /* has INTO clause */
		{
			bool isdef = false;
			bool exists = false;
			char *parname = NULL;

			/*
			 * If we're working on a subpartition, the INTO partition is
			 * actually a child partition of the parent identified by
			 * prule. So, we cannot just use get_part_rule() to determine
			 * if one of them is a default.
			 */

			/* first item */
			if (pid->idtype == AT_AP_IDRule)
			{
				if (prule->pNode->default_part &&
					((AlterPartitionId *)pc2->partid)->idtype == AT_AP_IDDefault)
				{
					isdef = true;
					exists = true;
					parname = prule->pNode->default_part->parname;
					if (!defparname && isdef) 
						defparname = pstrdup(parname);
				}
				else
				{
					ListCell *rc;
					AlterPartitionId *id = (AlterPartitionId *)pc2->partid;

					if (id->idtype == AT_AP_IDDefault)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("relation \"%s\" does not have a "
										"default partition",
										RelationGetRelationName(rel)),
												   errOmitLocation(true)));

					foreach(rc, prule->pNode->rules)
					{
						PartitionRule *r = lfirst(rc);

						if (strcmp(r->parname, strVal((Value *)id->partiddef)) == 0)
						{
							isdef = false;
							exists = true;
							parname = r->parname;
						}
					}

					if (prule->pNode->default_part &&
						strcmp(prule->pNode->default_part->parname,
							   strVal((Value *)id->partiddef)) == 0)
					{
						isdef = true;
						exists = true;
						parname = prule->pNode->default_part->parname;
						if (!defparname && isdef) 
							defparname = pstrdup(parname);
					}
				}
			}
			else /* not a AT_AP_IDRule */
			{
				PgPartRule *tmprule;
				tmprule = get_part_rule(rel, (AlterPartitionId *)pc2->partid,
										false, false, CurrentMemoryContext,
										NULL, false);
				if (tmprule)
				{
					isdef = tmprule->topRule->parisdefault;
					exists = true;
					parname = tmprule->topRule->parname;
					if (!defparname && isdef) 
						defparname = pstrdup(parname);
				}
			}

			if (exists && isdef)
			{
				default_pos = 1;
				intopid2 = (AlterPartitionId *)pc2->partid;
				intopid1 = (AlterPartitionId *)pc2->arg1;
				into_exists = 2;

				if (intopid2->idtype == AT_AP_IDDefault)
					 intopid2->partiddef = (Node *)makeString(pstrdup(parname));
			}
			else
			{
				if (exists)
					into_exists = 1;

				intopid1 = (AlterPartitionId *)pc2->partid;
				intopid2 = (AlterPartitionId *)pc2->arg1;
			}

			/* second item */
			exists = false;
			isdef = false;
			parname = NULL;
			if (pid->idtype == AT_AP_IDRule)
			{
				if (prule->pNode->default_part &&
					((AlterPartitionId *)pc2->arg1)->idtype == AT_AP_IDDefault)
				{
					isdef = true;
					exists = true;
					parname = prule->pNode->default_part->parname;
					if (!defparname && isdef) 
						defparname = pstrdup(parname);
				}
				else
				{
					ListCell *rc;
					AlterPartitionId *id = (AlterPartitionId *)pc2->arg1;

					if (id->idtype == AT_AP_IDDefault)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("relation \"%s\" does not have a "
										"default partition",
										RelationGetRelationName(rel)),
												   errOmitLocation(true)));

					foreach(rc, prule->pNode->rules)
					{
						PartitionRule *r = lfirst(rc);

						if (strcmp(r->parname, strVal((Value *)id->partiddef)) == 0)
						{
							isdef = false;
							exists = true;
							parname = r->parname;
						}
					}

					if (prule->pNode->default_part &&
						strcmp(prule->pNode->default_part->parname,
							   strVal((Value *)id->partiddef)) == 0)
					{
						isdef = true;
						exists = true;
						parname = prule->pNode->default_part->parname;
						if (!defparname && isdef) 
							defparname = pstrdup(parname);
					}

				}
			}
			else
			{
				PgPartRule *tmprule;
				tmprule = get_part_rule(rel, (AlterPartitionId *)pc2->arg1,
										false, false, CurrentMemoryContext,
										NULL, false);
				if (tmprule)
				{
					isdef = tmprule->topRule->parisdefault;
					exists = true;
					parname = tmprule->topRule->parname;
					if (!defparname && isdef) 
						defparname = pstrdup(parname);
				}
			}

			if (exists)
			{
				if (into_exists != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("both INTO partitions "
									"already exist"),
							 errOmitLocation(true)));

				into_exists = 2;
				intopid1 = (AlterPartitionId *)pc2->partid;
				intopid2 = (AlterPartitionId *)pc2->arg1;

				if (isdef)
				{
					default_pos = 2;

					if (intopid2->idtype == AT_AP_IDDefault)
						 intopid2->partiddef = (Node *)makeString(parname);
				}
			}
		}

		existrel = heap_open(prule->topRule->parchildrelid, NoLock);
		existopts = relopts_for_ctas(existrel);
		existstorage_opts = stdoptions_to_list(existrel, existopts);
		distro = make_dist_clause(existrel);

		heap_close(existrel, NoLock);

		/* 1) Create temp table */
		rv = makeRangeVar(nspname, relname, -1);
		inh->relation = copyObject(rv);
        inh->options = list_make3_int(CREATE_TABLE_LIKE_INCLUDING_DEFAULTS,
									  CREATE_TABLE_LIKE_INCLUDING_CONSTRAINTS,
									  CREATE_TABLE_LIKE_INCLUDING_INDEXES);
		ct->tableElts = list_make1(inh);
		ct->distributedBy = list_copy(distro); /* must preserve the list for later */

		/* should be unique enough */
		snprintf(tmpname, NAMEDATALEN, "pg_temp_%u", relid);
		tmprv = makeRangeVar(nspname, tmpname, -1);
		ct->relation = tmprv;
		ct->relKind = RELKIND_RELATION;
		ct->ownerid = rel->rd_rel->relowner;
		parsetrees = parse_analyze((Node *)ct, NULL, NULL, 0);

		q = (Query *)linitial(parsetrees);
		ProcessUtility((Node *)q->utilityStmt,
					   synthetic_sql,
					   NULL,
					   false, /* not top level */
					   dest, 
					   NULL);
		CommandCounterIncrement();

		/* get the oid of the temporary table */
		temprelid = get_relname_relid(tmpname,
									  RelationGetNamespace(rel));

		if (pid->idtype == AT_AP_IDRule)
		{
			idpid = copyObject(pid);
		}
		else
		{
			idpid = makeNode(AlterPartitionId);
			idpid->idtype = AT_AP_IDRule;
			idpid->partiddef = (Node *)list_make2((Node *)prule, pid);
			idpid->location  = -1;
		}

		/* 2) EXCHANGE temp with target */
		rel_to_drop = prule->topRule->parchildrelid;

		ats->relation = copyObject(rv);
		ats->relkind = OBJECT_TABLE;

		cmd->subtype = AT_PartExchange;

		mypc->partid = (Node *)idpid;
		mypc->arg1 = (Node *)tmprv;
		mypc->arg2 = (Node *)mypc2;

		mypc2->arg1 = (Node *)makeInteger(0);
		mypc2->arg2 = (Node *)makeInteger(1); /* tell them we're SPLIT */
		cmd->def = (Node *)mypc;
		ats->cmds = list_make1(cmd);
		parsetrees = parse_analyze((Node *)ats, NULL, NULL, 0);
		Assert(list_length(parsetrees) == 1);
		q = (Query *)linitial(parsetrees);

		heap_close(rel, NoLock);
		ProcessUtility((Node *)q->utilityStmt,
					   synthetic_sql,
					   NULL, 
					   false, /* not top level */
					   dest, 
					   NULL);
		rel = heap_open(relid, AccessExclusiveLock);
		CommandCounterIncrement();

		/* 3) drop the old partition */
		if (pid->idtype == AT_AP_IDRule)
		{
			PgPartRule *pr;
			part_rule_cxt cxt;

			idpid = copyObject(pid);


			/* need to update the OID reference */
			pr = linitial((List *)idpid->partiddef);
			cxt.old = rel_to_drop;
			cxt.new = temprelid;
			partrule_walker((Node *)pr, (void *)&cxt);
			elog(DEBUG5, "dropping OID %u", temprelid);
		}
		else
		{
			/* refresh prule, out of date due to EXCHANGE */
			prule = get_part_rule(rel, pid, true, true,
								  CurrentMemoryContext, NULL, false);
			idpid = makeNode(AlterPartitionId);
			idpid->idtype = AT_AP_IDRule;
			idpid->partiddef = (Node *)list_make2((Node *)prule, pid);
			idpid->location  = -1;
		}

		cmd->subtype = AT_PartDrop;

		ds->missing_ok = false;
		ds->behavior = DROP_RESTRICT;
		ds->removeType = OBJECT_TABLE;

		mypc->partid = (Node *)idpid;
		mypc->arg1 = (Node *)ds;
		/* MPP-6589: make DROP work, even if last one
		 * NOTE: hateful hackery.  Normally, the arg2 for the PartDrop
		 * cmd is null.  But since SPLIT may need to DROP the last
		 * partition before it re-ADDs two new ones, we pass a non-null
		 * arg2 as a flag to enable ForceDrop in ATPExecPartDrop().  
		*/
		mypc->arg2 = (Node *)makeNode(AlterPartitionCmd);

		cmd->def = (Node *)mypc;
		parsetrees = parse_analyze((Node *)ats, NULL, NULL, 0);
		heap_close(rel, NoLock);

		/* 
		 * Might have expanded to multiple statements if, for example, the
		 * master table has indexes on it.
		 */
		foreach(lc, parsetrees)
		{
			q = (Query *)lfirst(lc);

			ProcessUtility((Node *)q->utilityStmt, 
						   synthetic_sql,
						   NULL, 
						   false, /* not top level */
						   dest, 
						   NULL);
		}

		rel = heap_open(relid, AccessExclusiveLock);
		CommandCounterIncrement();

		/*
		 * Now that we've dropped the partition, we need to handle updating
		 * the PgPartRule pid for the case where it is a pid representing
		 * ALTER PARTITION ... ALTER PARTITION ...
		 *
		 * For the normal case, we'll just run get_part_rule() again deeper
		 * in the code.
		 */
		if (pid->idtype == AT_AP_IDRule)
		{
			AlterPartitionId *tmppid = copyObject(pid);

			/* MPP-10223: pid contains a "stale" pNode with a
			 * partition rule for the partition we just dropped.
			 * Delve deep into pNode to eliminate the topRule.
			 */
			if (1)
			{
				AlterPartitionId		*newpid	 = tmppid;
				PgPartRule				*tmprule = NULL;

				while (tmppid->idtype == AT_AP_IDRule)
				{
					List *l = (List *)tmppid->partiddef;

					/* wipe out the topRule because the partition was
					 * dropped, or PartAdd will find it and complain!!
					 * Note that because the pid was copied,
					 * tmprule->topRule has a separate copy of the
					 * prule subtree, so we need to fix this one, too.
					 */
					tmprule = (PgPartRule *)linitial(l);

					atpxSplitDropRule(tmprule->pNode, 
									  tmprule->topRule,
									  prule->topRule->parruleid, 
									  prule->topRule->paroid);
					tmppid = lsecond(l);
				}
				
				tmppid = newpid;
			}

			aapid = tmppid;
		}

		/* Add two new partitions */
		elog(DEBUG5, "Split partition: adding two new partitions");

		/* 4) add two new partitions, via a loop to reduce code duplication */
		for (i = 1; i <= 2; i++)
		{
			/* build up commands for adding two. */
			AlterPartitionId *mypid = makeNode(AlterPartitionId);
			CreateStmt *mycs = makeNode(CreateStmt);
			char *parname = NULL;
			AlterPartitionId *intopid = NULL;
			PartitionElem *pelem = makeNode(PartitionElem);
			Oid newchildrelid = InvalidOid;
			AlterPartitionCmd *storenode = NULL;
			List *intostorage_opts;
			StdRdOptions *intoopts;

			/* use storage options for existing rel */
			if (into_exists == i)
				intostorage_opts = existstorage_opts;
			else
			{
				intoopts = relopts_for_ctas(rel);
				intostorage_opts = stdoptions_to_list(rel, intoopts);
			}

			if (intostorage_opts)
			{
				storenode = makeNode(AlterPartitionCmd);
				storenode->arg1 = (Node *)intostorage_opts;
				storenode->location = -1;
			}
			pelem->storeAttr = (Node *)storenode;

			if (pc2)
			{
				if (i == 1)
					intopid = intopid1;
				else
					intopid = intopid2;
			}
			else
			{
				if (into_exists == i && prule->topRule->parname)
					parname = pstrdup(prule->topRule->parname);
			}

			mycs->relation = makeRangeVar(NULL, "fake_partition_name", -1);
			mycs->relKind = RELKIND_RELATION;
			mypid->idtype = AT_AP_IDNone;
			mypid->location = -1;
			mypid->partiddef = NULL;

			mypc->partid = (Node *)mypid;

			if (intopid)
				parname = strVal(intopid->partiddef);

			if (prule->topRule->parisdefault && i == into_exists)
			{
				/* nothing to do */
			}
			else
			{
				if (prule->pNode->part->parkind == 'r')
				{
					PartitionBoundSpec *a = makeNode(PartitionBoundSpec);

					if (prule->topRule->parisdefault)
					{
						a->partStart = linitial((List *)pc->arg1);
						a->partEnd = lsecond((List *)pc->arg1);
					}
					else if (i == 1)
					{
						PartitionRangeItem *ri;

						/* MPP-6589: if the partition has an "open"
						 * START, pass a NULL partStart 
						 */
						if (prule->topRule &&
							prule->topRule->parrangestart)
						{
							ri = makeNode(PartitionRangeItem);
							ri->location = -1;
							ri->everycount = 0;
							ri->partRangeVal = 
									copyObject(prule->topRule->parrangestart);
							ri->partedge =
									prule->topRule->parrangestartincl ? 
									PART_EDGE_INCLUSIVE : PART_EDGE_EXCLUSIVE;

							a->partStart = (Node *)ri;
						}
						else
							a->partStart = NULL;

						ri = makeNode(PartitionRangeItem);
						ri->location = -1;
						ri->everycount = 0;
						ri->partRangeVal = copyObject(at);
						ri->partedge = PART_EDGE_EXCLUSIVE;

						a->partEnd = (Node *)ri;
					}
					else if (i == 2)
					{
						PartitionRangeItem *ri;
						ri = makeNode(PartitionRangeItem);
						ri->location = -1;
						ri->everycount = 0;
						ri->partRangeVal = copyObject(at);
						ri->partedge = PART_EDGE_INCLUSIVE;

						a->partStart = (Node *)ri;

						/* MPP-6589: if the partition has an "open"
						 * END, pass a NULL partEnd 
						 */
						if (prule->topRule &&
							prule->topRule->parrangeend)
						{
							ri = makeNode(PartitionRangeItem);
							ri->location = -1;
							ri->everycount = 0;
							ri->partRangeVal =
									copyObject(prule->topRule->parrangeend);
							ri->partedge = prule->topRule->parrangeendincl ? 
									PART_EDGE_INCLUSIVE : PART_EDGE_EXCLUSIVE;

							a->partEnd = (Node *)ri;
						}
						else
							a->partEnd = NULL;

					}
					pelem->boundSpec = (Node *)a;
				}
				else
				{
					if ((into_exists && into_exists != i &&
						 prule->topRule->parisdefault) ||
						(i == 2 && !prule->topRule->parisdefault))
					{
						PartitionValuesSpec *valuesspec =
							makeNode(PartitionValuesSpec);

						valuesspec->partValues = copyObject(at);
						valuesspec->location = -1;
						pelem->boundSpec = (Node *)valuesspec;
					}
					else
					{
						PartitionValuesSpec *valuesspec =
							makeNode(PartitionValuesSpec);
						List *lv = prule->topRule->parlistvalues;
						List *newvals = NIL;
						ListCell *lc;
						List *atlist = (List *)at;

						/* must be LIST */
						Assert(prule->pNode->part->parkind == 'l');

						/*
						 * Iterate through list of existing constraints and
						 * pick out those not in AT() clause
						 */
						foreach(lc, lv)
						{
							List *cols = lfirst(lc);
							ListCell *parvals = list_head(atlist);
							int16 nkeys = prule->pNode->part->parnatts;
							bool found = false;

							while (parvals)
							{
								List *vals = lfirst(parvals);
								ListCell *lcv = list_head(vals);
								ListCell *lcc = list_head(cols);
								int parcol;
								bool matched = true;

								for (parcol = 0; parcol < nkeys; parcol++)
								{
									Oid opclass =
										prule->pNode->part->parclass[parcol];
									Oid funcid = get_opclass_proc(opclass, 0,
																  BTORDER_PROC);
									Const *v = lfirst(lcv);
									Const *c = lfirst(lcc);
									Datum d;

									if (v->constisnull && c->constisnull)
										continue;
									else if (v->constisnull || c->constisnull)
									{
										matched = false;
										break;
									}

									d = OidFunctionCall2(funcid, c->constvalue,
														 v->constvalue);

									if (DatumGetInt32(d) != 0)
									{
										matched = false;
										break;
									}

									lcv = lnext(lcv);
									lcc = lnext(lcc);
								}

								if (matched)
								{
									found = true;
									break;
								}

								parvals = lnext(parvals);
							}
							if (!found)
							{
								/* Not in AT() clause, so keep it */
								newvals = lappend(newvals,
										copyObject((Node *)cols));
							}
						}
						valuesspec->partValues = newvals;
						valuesspec->location = -1;
						pelem->boundSpec = (Node *)valuesspec;
					}
				}
			}

			/*
			 * We always want to create a partition name because we
			 * need to recall the partition details below.
			 */
			if (!parname)
			{
				char n[NAMEDATALEN];

				snprintf(n, NAMEDATALEN, "r%lu", random());
				parname = pstrdup(n);

			}

			pelem->partName = (Node *)makeString(parname);
			mypid->partiddef = pelem->partName;
			mypid->idtype = AT_AP_IDName;

			pelem->location  = -1;
			/* MPP-10421: determine if new partition, re-use of old
			 * partition, and/or is default partition 
			 */
			pelem->isDefault = (defparname && 
								parname &&
								(0 == strcmp(parname, defparname)));

			/* tell ADD PARTITION we're doing a SPLIT */
			mypc2->partid = (Node *)makeInteger(1);

			mypc2->arg1 = (Node *)pelem;
			mypc2->arg2 = (Node *)list_make1(mycs);
			mypc2->location = -1;

			mypc->arg1 = (Node *)makeInteger(pelem->isDefault ? 1 : 0);
			mypc->arg2 = (Node *)mypc2;
			mypc->location = -1;

			cmd->subtype = AT_PartAdd;
			cmd->def = (Node *)mypc;

			/* turn this into ALTER PARTITION if need be */
			if (pid->idtype == AT_AP_IDRule)
			{
				AlterTableCmd *ac = makeNode(AlterTableCmd);
				AlterPartitionCmd *ap = makeNode(AlterPartitionCmd);

				ac->subtype = AT_PartAlter;
				ac->def = (Node *)ap;
				ap->partid = (Node *)aapid;
				ap->arg1 = (Node *)cmd; /* embed the real command */
				ap->location = -1;

				cmd = ac;
			}

			ats->cmds = list_make1(cmd);
			parsetrees = parse_analyze((Node *)ats, NULL, NULL, 0);
			Assert(list_length(parsetrees) == 1);
			q = (Query *)linitial(parsetrees);

			heap_close(rel, NoLock);
			ProcessUtility((Node *)q->utilityStmt, 
						   synthetic_sql,
						   NULL, 
						   false, /* not top level */
						   dest, 
						   NULL);
			rel = heap_open(relid, AccessExclusiveLock);

			/* make our change visible */
			CommandCounterIncrement();

			/* get the rule back which ADD PARTITION just created */
			if (pid->idtype == AT_AP_IDRule)
			{
				ScanKeyData key[3];
				SysScanDesc scan;
				HeapTuple tuple;
				Relation rulerel = heap_open(PartitionRuleRelationId,
											 AccessShareLock);

				Datum d = DirectFunctionCall1(namein,
							CStringGetDatum(strVal(pelem->partName)));

				ScanKeyInit(&key[0],
							Anum_pg_partition_rule_paroid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(prule->topRule->paroid));

				ScanKeyInit(&key[1],
							Anum_pg_partition_rule_parparentrule,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(prule->topRule->parparentoid));

				ScanKeyInit(&key[2],
							Anum_pg_partition_rule_parname,
							BTEqualStrategyNumber, F_NAMEEQ,
							d);

				scan = systable_beginscan(rulerel, InvalidOid,
										  false, SnapshotSelf, 3, key);

				tuple = systable_getnext(scan);

				Insist(HeapTupleIsValid(tuple));

				newchildrelid =
					((Form_pg_partition_rule)GETSTRUCT(tuple))->parchildrelid;

				systable_endscan(scan);
				heap_close(rulerel, NoLock);
			}
			else
			{
				PgPartRule *tmprule;
				tmprule = get_part_rule(rel, mypid, true, true,
									    CurrentMemoryContext, NULL, false);
				newchildrelid = tmprule->topRule->parchildrelid;
			}

			Assert(OidIsValid(newchildrelid));

			if (i == 1)
			{
				intoa = heap_open(newchildrelid,
								  AccessExclusiveLock);

				rva = makeRangeVar(
							get_namespace_name(RelationGetNamespace(intoa)),
						    get_rel_name(RelationGetRelid(intoa)), -1);
			}
			else
			{
				intob = heap_open(newchildrelid,
								  AccessExclusiveLock);

				rvb = makeRangeVar(
							get_namespace_name(RelationGetNamespace(intob)),
						    get_rel_name(RelationGetRelid(intob)), -1);

			}
		}

		temprel = heap_open(rel_to_drop, AccessExclusiveLock);

		/* update parse tree with info for the QEs */
		Assert(PointerIsValid(rva));
		Assert(PointerIsValid(rvb));

		/* update for consumption by QEs */
		pc->partid = (Node *)makeInteger(RelationGetRelid(temprel));
		pc->arg1 = (Node *)copyObject(rva);
		pc->arg2 = (Node *)copyObject(rvb);

		/* MPP-6929: metadata tracking */
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "PARTITION", "SPLIT"
				);

	} /* end if dispatch */
	else if (Gp_role == GP_ROLE_EXECUTE)
	{
		Oid temprelid;
		RangeVar *rva;
		RangeVar *rvb;

		temprelid = (Oid)intVal((Value *)pc->partid);
		temprel = heap_open(temprelid, AccessExclusiveLock);
		rva = (RangeVar *)pc->arg1;
		intoa = heap_openrv(rva, AccessExclusiveLock);
		rvb = (RangeVar *)pc->arg2;
		intob = heap_openrv(rvb, AccessExclusiveLock);
	}
	else
		return;

	/*
	 * Now, on every node, scan the exchanged out table, splitting data
	 * between two new partitions.
	 *
	 * To do this, we use the CHECK constraints we just created on the
	 * tables via ADD PARTITION.
	 */
	split_rows(intoa, intob, temprel);

	elog(DEBUG5, "dropping temp rel %s", RelationGetRelationName(temprel));
	temprelid = RelationGetRelid(temprel);
	heap_close(temprel, NoLock);

	/* drop temp table */
	{
		ObjectAddress addr;
		addr.classId = RelationRelationId;
		addr.objectId = temprelid;
		addr.objectSubId = 0;

		performDeletion(&addr, DROP_RESTRICT);
	}

	heap_close(intoa, NoLock);
	heap_close(intob, NoLock);
}

/* ALTER TABLE ... TRUNCATE PARTITION */
static List *
atpxTruncateList(Relation rel, PartitionNode *pNode)
{
	List *l1 = NIL;
	ListCell *lc;

	if (!pNode)
		return l1;

	/* add the child lists first */
	foreach(lc, pNode->rules)
	{
		PartitionRule *rule = lfirst(lc);
		List *l2 = NIL;

		if (rule->children)
			l2 = atpxTruncateList(rel, rule->children);
		else
			l2 = NIL;

		if (l2)
		{
			if (l1)
				l1 = list_concat(l1, l2);
			else
				l1 = l2;
		}
	}

	/* and the default partition */
	if (pNode->default_part)
	{
		PartitionRule *rule = pNode->default_part;
		List *l2 = NIL;

		if (rule->children)
			l2 = atpxTruncateList(rel, rule->children);
		else
			l2 = NIL;

		if (l2)
		{
			if (l1)
				l1 = list_concat(l1, l2);
			else
				l1 = l2;
		}
	}

	/* add entries for rules at current level */
	foreach(lc, pNode->rules)
	{
		PartitionRule 	*rule = lfirst(lc);
		RangeVar 		*rv;
		Relation		 rel;

		rel = heap_open(rule->parchildrelid, AccessShareLock);

		rv = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
						  pstrdup(RelationGetRelationName(rel)), -1);

		heap_close(rel, NoLock);

		if (l1)
			l1 = lappend(l1, rv);
		else
			l1 = list_make1(rv);
	}

	/* and the default partition */
	if (pNode->default_part)
	{
		PartitionRule 	*rule = pNode->default_part;
		RangeVar 		*rv;
		Relation		 rel;

		rel = heap_open(rule->parchildrelid, AccessShareLock);
		rv = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
						  pstrdup(RelationGetRelationName(rel)), -1);

		heap_close(rel, NoLock);

		if (l1)
			l1 = lappend(l1, rv);
		else
			l1 = list_make1(rv);
	}

	return l1;
} /* end atpxTruncateList */

static void
ATPExecPartTruncate(Relation rel,
                    AlterPartitionCmd *pc)
{
	AlterPartitionId *pid = (AlterPartitionId *)pc->partid;
	PgPartRule   *prule = NULL;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	prule = get_part_rule(rel, pid, true, true, CurrentMemoryContext, NULL,
						  false);

	if (prule)
	{
		RangeVar 		*rv;
		TruncateStmt 	*ts   = (TruncateStmt *)pc->arg1;
		DestReceiver 	*dest = None_Receiver;
		Relation	  	 rel2;

		rel2 = heap_open(prule->topRule->parchildrelid, AccessShareLock);
		rv = makeRangeVar(get_namespace_name(RelationGetNamespace(rel2)),
						  pstrdup(RelationGetRelationName(rel2)), -1);

		rv->location = pc->location;

		if (prule->topRule->children)
		{
			List *l1 = atpxTruncateList(rel2, prule->topRule->children);

			ts->relations = lappend(l1, rv);
		}
		else
			ts->relations = list_make1(rv);

		heap_close(rel2, NoLock);

		ProcessUtility( (Node *) ts,
					   synthetic_sql,
					   NULL, 
					   false, /* not top level */
					   dest, 
					   NULL);

		/* Notify of name if did not use name for partition id spec */
		if (prule && prule->topRule && prule->topRule->children
			&& (ts->behavior != DROP_CASCADE ))
		{
			ereport(NOTICE,
					(errmsg("truncated partition%s for %s and its children",
							prule->partIdStr,
							prule->relname),
									   errOmitLocation(true)));
		}
		else if ((pid->idtype != AT_AP_IDName)
				 && prule->isName)
				ereport(NOTICE,
						(errmsg("truncated partition%s for %s",
								prule->partIdStr,
								prule->relname),
										   errOmitLocation(true)));
	}

} /* end ATPExecPartTruncate */

/*
 * Execute ALTER TABLE SET SCHEMA
 *
 * Note: caller must have checked ownership of the relation already
 */
void
AlterTableNamespace(RangeVar *relation, const char *newschema)
{
	Relation	rel;
	Oid			relid;
	Oid			oldNspOid;
	Oid			nspOid;

	rel = heap_openrv(relation, AccessExclusiveLock);

	relid = RelationGetRelid(rel);
	oldNspOid = RelationGetNamespace(rel);

	/* heap_openrv allows TOAST, but we don't want to */
	if (rel->rd_rel->relkind == RELKIND_TOASTVALUE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a TOAST relation",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	if (rel->rd_rel->relkind == RELKIND_AOSEGMENTS)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an Append Only segment listing relation",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	if (rel->rd_rel->relkind == RELKIND_AOBLOCKDIR)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an Append Only block directory relation",
						RelationGetRelationName(rel)),
								   errOmitLocation(true)));

	/* if it's an owned sequence, disallow moving it by itself */
	if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
	{
		Oid			tableId;
		int32		colId;

		if (sequenceIsOwned(relid, &tableId, &colId))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move an owned sequence into another schema"),
					 errdetail("Sequence \"%s\" is linked to table \"%s\".",
							   RelationGetRelationName(rel),
							   get_rel_name(tableId)),
									   errOmitLocation(true)));
	}

	/* get schema OID and check its permissions */
	nspOid = LookupCreationNamespace(newschema);

	if (oldNspOid == nspOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" is already in schema \"%s\"",
						RelationGetRelationName(rel),
						newschema),
								   errOmitLocation(true)));

	/* disallow renaming into or out of temp schemas */
	if (isAnyTempNamespace(nspOid) || isAnyTempNamespace(oldNspOid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("cannot move objects into or out of temporary schemas"),
					   errOmitLocation(true)));

	/* same for TOAST schema */
	if (nspOid == PG_TOAST_NAMESPACE || oldNspOid == PG_TOAST_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of TOAST schema"),
						   errOmitLocation(true)));

	/* same for AO SEGMENT schema */
	if (nspOid == PG_AOSEGMENT_NAMESPACE || oldNspOid == PG_AOSEGMENT_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of AO SEGMENT schema"),
						   errOmitLocation(true)));


	/* OK, modify the pg_class row and pg_depend entry */
	AlterRelationNamespaceInternalTwo(rel, relid,
									  oldNspOid, nspOid,
									  true, newschema);
	
	/* MPP-7825, MPP-6929, MPP-7600: metadata tracking */
	if ((Gp_role == GP_ROLE_DISPATCH)
		&& MetaTrackValidKindNsp(rel->rd_rel))
		MetaTrackUpdObject(RelationRelationId,
						   RelationGetRelid(rel),
						   GetUserId(),
						   "ALTER", "SET SCHEMA"
				);

	/* close rel, but keep lock until commit */
	relation_close(rel, NoLock);
}

static void
AlterRelationNamespaceInternalTwo(Relation rel,
								  Oid relid,
								  Oid oldNspOid, Oid newNspOid,
								  bool hasDependEntry,
								  const char *newschema)
{
	Relation classRel;

	/* OK, modify the pg_class row and pg_depend entry */
	classRel = heap_open(RelationRelationId, RowExclusiveLock);

	AlterRelationNamespaceInternal(classRel, relid, oldNspOid, newNspOid, 
								   hasDependEntry);

	/* Fix the table's rowtype too */
	AlterTypeNamespaceInternal(rel->rd_rel->reltype, newNspOid, false);

	/* Fix other dependent stuff */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		AlterIndexNamespaces(classRel, rel, oldNspOid, newNspOid);
		AlterSeqNamespaces(classRel, rel, oldNspOid, newNspOid, newschema);
		AlterConstraintNamespaces(relid, oldNspOid, newNspOid, false);
	}
	heap_close(classRel, RowExclusiveLock);
}

/*
 * The guts of relocating a relation to another namespace: fix the pg_class
 * entry, and the pg_depend entry if any.  Caller must already have
 * opened and write-locked pg_class.
 */
void
AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry)
{
	HeapTuple	classTup;
	Form_pg_class classForm;

	classTup = SearchSysCacheCopy(RELOID,
								  ObjectIdGetDatum(relOid),
								  0, 0, 0);
	if (!HeapTupleIsValid(classTup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	Assert(classForm->relnamespace == oldNspOid);

	/* check for duplicate name (more friendly than unique-index failure) */
	if (get_relname_relid(NameStr(classForm->relname),
						  newNspOid) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists in schema \"%s\"",
						NameStr(classForm->relname),
						get_namespace_name(newNspOid)),
								   errOmitLocation(true)));

	/* classTup is a copy, so OK to scribble on */
	classForm->relnamespace = newNspOid;

	simple_heap_update(classRel, &classTup->t_self, classTup);
	CatalogUpdateIndexes(classRel, classTup);

	/* Update dependency on schema if caller said so */
	if (hasDependEntry &&
		changeDependencyFor(RelationRelationId, relOid,
							NamespaceRelationId, oldNspOid, newNspOid) != 1)
		elog(ERROR, "failed to change schema dependency for relation \"%s\"",
			 NameStr(classForm->relname));

	heap_freetuple(classTup);
}

/*
 * Move all indexes for the specified relation to another namespace.
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 */
static void
AlterIndexNamespaces(Relation classRel, Relation rel,
					 Oid oldNspOid, Oid newNspOid)
{
	List	   *indexList;
	ListCell   *l;

	indexList = RelationGetIndexList(rel);

	foreach(l, indexList)
	{
		Oid			indexOid = lfirst_oid(l);

		/*
		 * Note: currently, the index will not have its own dependency on the
		 * namespace, so we don't need to do changeDependencyFor(). There's no
		 * rowtype in pg_type, either.
		 */
		AlterRelationNamespaceInternal(classRel, indexOid,
									   oldNspOid, newNspOid,
									   false);
	}

	list_free(indexList);
}

/*
 * Move all SERIAL-column sequences of the specified relation to another
 * namespace.
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 */
static void
AlterSeqNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid, const char *newNspName)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL sequences are those having an auto dependency on one of the
	 * table's columns (we don't care *which* column, exactly).
	 */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	/* we leave refobjsubid unspecified */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* skip dependencies other than auto dependencies on columns */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			depForm->deptype != DEPENDENCY_AUTO)
			continue;

		/* Use relation_open just in case it's an index */
		seqRel = relation_open(depForm->objid, AccessExclusiveLock);

		/* skip non-sequence relations */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			relation_close(seqRel, AccessExclusiveLock);
			continue;
		}

		/* Fix the pg_class and pg_depend entries */
		AlterRelationNamespaceInternal(classRel, depForm->objid,
									   oldNspOid, newNspOid,
									   true);

		/*
		 * Sequences have entries in pg_type. We need to be careful to move
		 * them to the new namespace, too.
		 */
		AlterTypeNamespaceInternal(RelationGetForm(seqRel)->reltype,
								   newNspOid, false);

		/* Now we can close it.  Keep the lock till end of transaction. */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}


/*
 * This code supports
 *	CREATE TEMP TABLE ... ON COMMIT { DROP | PRESERVE ROWS | DELETE ROWS }
 *
 * Because we only support this for TEMP tables, it's sufficient to remember
 * the state in a backend-local data structure.
 */

/*
 * Register a newly-created relation's ON COMMIT action.
 */
void
register_on_commit_action(Oid relid, OnCommitAction action)
{
	OnCommitItem *oc;
	MemoryContext oldcxt;

	/*
	 * We needn't bother registering the relation unless there is an ON COMMIT
	 * action we need to take.
	 */
	if (action == ONCOMMIT_NOOP || action == ONCOMMIT_PRESERVE_ROWS)
		return;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	oc = (OnCommitItem *) palloc(sizeof(OnCommitItem));
	oc->relid = relid;
	oc->oncommit = action;
	oc->creating_subid = GetCurrentSubTransactionId();
	oc->deleting_subid = InvalidSubTransactionId;

	on_commits = lcons(oc, on_commits);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Unregister any ON COMMIT action when a relation is deleted.
 *
 * Actually, we only mark the OnCommitItem entry as to be deleted after commit.
 */
void
remove_on_commit_action(Oid relid)
{
	ListCell   *l;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		if (oc->relid == relid)
		{
			oc->deleting_subid = GetCurrentSubTransactionId();
			break;
		}
	}
}

/*
 * Perform ON COMMIT actions.
 *
 * This is invoked just before actually committing, since it's possible
 * to encounter errors.
 */
void
PreCommit_on_commit_actions(void)
{
	ListCell   *l;
	List	   *oids_to_truncate = NIL;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		/* Ignore entry if already dropped in this xact */
		if (oc->deleting_subid != InvalidSubTransactionId)
			continue;

		switch (oc->oncommit)
		{
			case ONCOMMIT_NOOP:
			case ONCOMMIT_PRESERVE_ROWS:
				/* Do nothing (there shouldn't be such entries, actually) */
				break;
			case ONCOMMIT_DELETE_ROWS:
				oids_to_truncate = lappend_oid(oids_to_truncate, oc->relid);
				break;
			case ONCOMMIT_DROP:
				{
					ObjectAddress object;

					object.classId = RelationRelationId;
					object.objectId = oc->relid;
					object.objectSubId = 0;
					performDeletion(&object, DROP_CASCADE);

					/*
					 * Note that table deletion will call
					 * remove_on_commit_action, so the entry should get marked
					 * as deleted.
					 */
					Assert(oc->deleting_subid != InvalidSubTransactionId);
					break;
				}
		}
	}
	if (oids_to_truncate != NIL)
	{
		heap_truncate(oids_to_truncate);
		CommandCounterIncrement();		/* XXX needed? */
	}
}

/*
 * Post-commit or post-abort cleanup for ON COMMIT management.
 *
 * All we do here is remove no-longer-needed OnCommitItem entries.
 *
 * During commit, remove entries that were deleted during this transaction;
 * during abort, remove those created during this transaction.
 */
void
AtEOXact_on_commit_actions(bool isCommit)
{
	ListCell   *cur_item;
	ListCell   *prev_item;

	prev_item = NULL;
	cur_item = list_head(on_commits);

	while (cur_item != NULL)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (isCommit ? oc->deleting_subid != InvalidSubTransactionId :
			oc->creating_subid != InvalidSubTransactionId)
		{
			/* cur_item must be removed */
			on_commits = list_delete_cell(on_commits, cur_item, prev_item);
			pfree(oc);
			if (prev_item)
				cur_item = lnext(prev_item);
			else
				cur_item = list_head(on_commits);
		}
		else
		{
			/* cur_item must be preserved */
			oc->creating_subid = InvalidSubTransactionId;
			oc->deleting_subid = InvalidSubTransactionId;
			prev_item = cur_item;
			cur_item = lnext(prev_item);
		}
	}
}

/*
 * Post-subcommit or post-subabort cleanup for ON COMMIT management.
 *
 * During subabort, we can immediately remove entries created during this
 * subtransaction.	During subcommit, just relabel entries marked during
 * this subtransaction as being the parent's responsibility.
 */
void
AtEOSubXact_on_commit_actions(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid)
{
	ListCell   *cur_item;
	ListCell   *prev_item;

	prev_item = NULL;
	cur_item = list_head(on_commits);

	while (cur_item != NULL)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (!isCommit && oc->creating_subid == mySubid)
		{
			/* cur_item must be removed */
			on_commits = list_delete_cell(on_commits, cur_item, prev_item);
			pfree(oc);
			if (prev_item)
				cur_item = lnext(prev_item);
			else
				cur_item = list_head(on_commits);
		}
		else
		{
			/* cur_item must be preserved */
			if (oc->creating_subid == mySubid)
				oc->creating_subid = parentSubid;
			if (oc->deleting_subid == mySubid)
				oc->deleting_subid = isCommit ? parentSubid : InvalidSubTransactionId;
			prev_item = cur_item;
			cur_item = lnext(prev_item);
		}
	}
}


/*
 * Transform the URI string list into a text array (the form that is
 * used in the catalog table pg_exttable). While at it we validate
 * the URI strings.
 *
 * The result is a text array but we declare it as Datum to avoid
 * including array.h in analyze.h.
 */
static Datum transformLocationUris(List *locs, bool isweb, bool iswritable)
{
	ListCell   *cell;
	ArrayBuildState *astate;
	Datum		result;
	UriProtocol first_protocol = URI_FILE; /* initialize to keep gcc quiet */
	bool		first_uri = true;

#define FDIST_DEF_PORT 8080
#define HDFS_DEF_PORT  9000

	/* Parser should not let this happen */
	Assert(locs != NIL);

	/* We build new array using accumArrayResult */
	astate = NULL;

	/*
	 * first, check for duplicate URI entries
	 */
	foreach(cell, locs)
	{
		Value		*v1 = lfirst(cell);
		const char	*uri1 = v1->val.str;
		ListCell   *rest;

		for_each_cell(rest, lnext(cell))
		{
			Value		*v2 = lfirst(rest);
			const char	*uri2 = v2->val.str;

			if (strcmp(uri1, uri2) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("location uri \"%s\" appears more than once",
								uri1),
										   errOmitLocation(true)));
		}
	}

	/*
	 * iterate through the user supplied URI list from LOCATION clause.
	 */
	foreach(cell, locs)
	{
		Uri			*uri;
		text		*t;
		char		*uri_str_orig;
		char		*uri_str_final;
		Size		len;
		Value		*v = lfirst(cell);

		/* get the current URI string from the command */
		uri_str_orig = pstrdup(v->val.str);

		/* parse it to its components */
		uri = ParseUriString(uri_str_orig);

		/* allocate memory for a modified URI string (if needs modification) */
		uri_str_final = (char *) palloc(strlen(uri_str_orig) *
						  sizeof(char) +
						  1 + 4 + 1 /* default port if added */);

		/*
		 * in here edit the uri string if needed
		 */

		/* no port was specified for gpfdist or hdfs. add the default */
		if(uri->protocol == URI_GPFDIST && uri->port == -1)
		{
			char *at_hostname = (char *) uri_str_orig + strlen("gpfdist://");
			char *after_hostname = strchr(at_hostname, '/');
			int  len = after_hostname - at_hostname;
			char *hostname = pstrdup(at_hostname);

			hostname[len] = '\0';

			/* add the default port number to the uri string */
			sprintf(uri_str_final, "%s%s:%d%s", PROTOCOL_GPFDIST, hostname,
					FDIST_DEF_PORT, after_hostname);

			pfree(hostname);
		}
		else if(uri->protocol == URI_GPHDFS && uri->port == -1)
		{
			char *at_hostname = (char *) uri_str_orig + strlen("gphdfs://");
			char *after_hostname = strchr(at_hostname, '/');
			int  len = after_hostname - at_hostname;
			char *hostname = pstrdup(at_hostname);

			hostname[len] = '\0';

			/* add the default port number to the uri string */
			sprintf(uri_str_final, "%s%s:%d%s", PROTOCOL_GPHDFS, hostname,
					HDFS_DEF_PORT, after_hostname);

			pfree(hostname);
		}
		else
		{
			/* no changes to original uri string */
			uri_str_final = (char *) uri_str_orig;
		}

		/*
		 * check for various errors
		 */
		if (!first_uri && uri->protocol == URI_GPHDFS)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("GPHDFS can only have one location list"),
					errhint("Combine multiple HDFS files into a single file"),
					errOmitLocation(true)));


		if(first_uri)
		{
		    first_protocol = uri->protocol;
		    first_uri = false;
		}

		if(uri->protocol != first_protocol)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("URI protocols must be the same for all data sources"),
					 errhint("Available protocols are 'http', 'file' and 'gpfdist'"),
							   errOmitLocation(true)));

		}

		if(uri->protocol != URI_FILE &&
		   uri->protocol != URI_HTTP &&
		   uri->protocol != URI_GPHDFS &&
		   uri->protocol != URI_GPFDIST)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported URI protocol for \'%s\'", uri_str_final),
					 errhint("Use 'file', 'http', 'hdfs' or 'gpfdist' protocol instead."),
							   errOmitLocation(true)));
		}

		if(uri->protocol != URI_HTTP && isweb)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("an EXTERNAL WEB TABLE may only use http URI\'s, problem in: \'%s\'", uri_str_final),
					 errhint("Use CREATE EXTERNAL TABLE instead."),
							   errOmitLocation(true)));

		if(uri->protocol == URI_HTTP && !isweb)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("http URI\'s can only be used in an external web table"),
					errhint("Use CREATE EXTERNAL WEB TABLE instead."),
							   errOmitLocation(true)));

		if(iswritable && (uri->protocol == URI_HTTP || uri->protocol == URI_FILE))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported URI protocol \'%s\' for writable external table", 
							(uri->protocol == URI_HTTP ? "http" : "file")),
					 errhint("Writable external tables may use \'gpfdist\' URIs only."),
							   errOmitLocation(true)));

		if(iswritable && strchr(uri->path, '*'))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("Unsupported use of wildcard in a writable external web table definition: "
							"\'%s\'", uri_str_final),
					errhint("Specify the explicit path and file name to write into.")));
		
		if(iswritable && uri->path[strlen(uri->path) - 1] == '/')
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("Unsupported use of a directory name in a writable external web table definition: "
							"\'%s\'", uri_str_final),
					errhint("Specify the explicit path and file name to write into."),
							   errOmitLocation(true)));

		len = VARHDRSZ + strlen(uri_str_final);

		/* +1 leaves room for sprintf's trailing null */
		t = (text *) palloc(len + 1);
		SET_VARSIZE(t, len);
		sprintf((char *) VARDATA(t), "%s", uri_str_final);


		astate = accumArrayResult(astate, PointerGetDatum(t),
								  false, TEXTOID,
								  CurrentMemoryContext);

		pfree(uri);
		pfree(uri_str_final);
	}

	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;

	return result;

}

static Datum transformExecOnClause(List	*on_clause)
{
	ArrayBuildState *astate;
	Datum		result;

	ListCell   *exec_location_opt;
	char	   *exec_location_str = NULL;
	char	   *value_str = NULL;
	int			value_int;
	Size		len;
	text		*t;

	/*
	 * Extract options from the statement node tree
	 * NOTE: as of now we only support one option in the ON clause
	 * and therefore more than one is an error (check here in case
	 * the sql parser isn't strict enough).
	 */
	foreach(exec_location_opt, on_clause)
	{
		DefElem    *defel = (DefElem *) lfirst(exec_location_opt);

		/* only one element is allowed! */
		if(exec_location_str)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("ON clause must not have more than one element."),
							   errOmitLocation(true)));

		if (strcmp(defel->defname, "all") == 0)
		{
			/* result: "ALL_SEGMENTS" */
			exec_location_str = (char *) palloc(12 + 1);
			exec_location_str = "ALL_SEGMENTS";
		}
		else if (strcmp(defel->defname, "hostname") == 0)
		{
			/* result: "HOST:<hostname>" */
			value_str = strVal(defel->arg);
			exec_location_str = (char *) palloc(5 + 1 + strlen(value_str) + 1);
			sprintf((char *) exec_location_str, "HOST:%s", value_str);
		}
		else if (strcmp(defel->defname, "eachhost") == 0)
		{
			/* result: "PER_HOST" */
			exec_location_str = (char *) palloc(8 + 1);
			exec_location_str = "PER_HOST";
		}
		else if (strcmp(defel->defname, "master") == 0)
		{
			/* result: "MASTER_ONLY" */
			exec_location_str = (char *) palloc(11 + 1);
			exec_location_str = "MASTER_ONLY";
		}
		else if (strcmp(defel->defname, "segment") == 0)
		{
			/* result: "SEGMENT_ID:<segid>" */
			value_int = intVal(defel->arg);
			exec_location_str = (char *) palloc(10 + 1 + 8 + 1);
			sprintf((char *) exec_location_str, "SEGMENT_ID:%d", value_int);
		}
		else if (strcmp(defel->defname, "random") == 0)
		{
			/* result: "TOTAL_SEGS:<number>" */
			value_int = intVal(defel->arg);
			exec_location_str = (char *) palloc(10 + 1 + 8 + 1);
			sprintf((char *) exec_location_str, "TOTAL_SEGS:%d", value_int);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Unknown location code for EXECUTE in tablecmds."),
							   errOmitLocation(true)));
		}
	}

	/* convert to text[] */
	astate = NULL;
	len = VARHDRSZ + strlen(exec_location_str);
	t = (text *) palloc(len + 1);
	SET_VARSIZE(t, len);
	sprintf((char *) VARDATA(t), "%s", exec_location_str);


	astate = accumArrayResult(astate, PointerGetDatum(t),
							  false, TEXTOID,
							  CurrentMemoryContext);

	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;

	return result;
}

/*
 * transform format name to format code and validate that
 * the format is supported. Currently the only supported formats
 * are "text" (type 't') and "csv" (type 'c')
 */
static char transformFormatType(char *formatname)
{
	char	result = '\0';

	if(pg_strcasecmp(formatname, "text") == 0)
		result = 't';
	else if(pg_strcasecmp(formatname, "csv") == 0)
		result = 'c';
	else
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unsupported format '%s'", formatname),
				 errhint("available formats are \"text\" or \"csv\""),
						   errOmitLocation(true)));

	return result;
}


/*
 * Transform the FORMAT options into a text field. Parse the
 * options and validate them for their respective format type.
 *
 * NOTE: Lot's of code here is duplicated with parts of DoCopy (copy.c).
 * However, when we switch over to user defined formats this function
 * won't be necessary anymore, as the SQL parser will not parse
 * the format options at all but instead will already treat them
 * as one string.
 *
 * The result is a text field that includes the format string.
 */
static Datum transformFormatOpts(char formattype, List *formatOpts, int numcols, bool iswritable)
{
	ListCell   *option;
	Datum		result;
	char *format_str = NULL;
	char *delim = NULL;
	char *null_print = NULL;
	char *quote = NULL;
	char *escape = NULL;
	char *eol_str = NULL;
	bool header_line = false;
	bool csv_mode;
	bool fill_missing = false;
	List *force_notnull = NIL;
	List *force_quote = NIL;
	Size len;
	StringInfoData fnn, fq, nl;

	Assert(formattype == 'c' || formattype == 't');

	csv_mode = (formattype == 'c' ? true : false);

	/* Extract options from the statement node tree */
	foreach(option, formatOpts)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (delim)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
			delim = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (null_print)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
			null_print = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			if (header_line)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
			header_line = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
			quote = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (escape)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
			escape = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "force_notnull") == 0)
		{
			if (force_notnull)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
				
			force_notnull = (List *) defel->arg;
		}
		else if (strcmp(defel->defname, "force_quote") == 0)
		{
			if (force_quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
				
			force_quote = (List *) defel->arg;
		}
		else if (strcmp(defel->defname, "fill_missing_fields") == 0)
		{
			if (fill_missing)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
			fill_missing = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "newline") == 0)
		{
			if (eol_str)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
								   errOmitLocation(true)));
			eol_str = strVal(defel->arg);
		}

		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * Set defaults
	 */
	if (!delim)
		delim = csv_mode ? "," : "\t";

	if (!null_print)
		null_print = csv_mode ? "" : "\\N";

	if (csv_mode)
	{
		if (!quote)
			quote = "\"";
		if (!escape)
			escape = quote;
	}

	if (!csv_mode && !escape)
		escape = "\\";			/* default escape for text mode */

	/*
	 * re-construct the FORCE NOT NULL list string.
	 * TODO: is there no existing util function that does this? can't find.
	 */
	if(force_notnull)
	{
		ListCell   *l;
		bool 		is_first_col = true;

		initStringInfo(&fnn);
		appendStringInfo(&fnn, " force not null");

		foreach(l, force_notnull)
		{
			const char	   *col_name = strVal(lfirst(l));

			appendStringInfo(&fnn, (is_first_col ? " %s" : ",%s"),
							 quote_identifier(col_name));
			is_first_col = false;
		}
	}

	/*
	 * re-construct the FORCE QUOTE list string.
	 */
	if(force_quote)
	{
		ListCell   *l;
		bool 		is_first_col = true;

		initStringInfo(&fq);
		appendStringInfo(&fq, " force quote");

		foreach(l, force_quote)
		{
			const char	   *col_name = strVal(lfirst(l));

			appendStringInfo(&fq, (is_first_col ? " %s" : ",%s"),
							 quote_identifier(col_name));
			is_first_col = false;
		}
	}

	if(eol_str)
	{
		initStringInfo(&nl);
		appendStringInfo(&nl, " newline '%s'", eol_str);
	}
	
	/* verify all user supplied control char combinations are legal */
	ValidateControlChars(false,
						 !iswritable,
						 csv_mode,
						 delim,
						 null_print,
						 quote,
						 escape,
						 force_quote,
						 force_notnull,
						 header_line,
						 fill_missing,
						 eol_str,
						 numcols);

	/*
	 * build the format option string that will get stored in the catalog.
	 */

	len = 9 + 1 + 1 + strlen(delim) + 1 +		/* "delimiter 'x' of 'off'" */
		  1 +									/* space					*/
		  4 + 1 + 1 + strlen(null_print) + 1 +	/* "null 'str'"				*/
		  1 +									/* space					*/
		  6 + 1 + 1 + strlen(escape) + 1;		/* "escape 'c' or 'off' 	*/

	if (formattype == 'c')
		len +=	1 +								/* space					*/
				5 + 1 + 3;						/* "quote 'x'"				*/

	len += header_line ? strlen(" header") : 0;
	len += fill_missing ? strlen(" fill missing fields") : 0;
	len += force_notnull ? strlen(fnn.data) : 0;
	len += force_quote ? strlen(fq.data) : 0;
	len += (eol_str ? (1 + 7 + 1 + 1 + strlen(eol_str) + 1) : 0); /* space x 2, newline 'xx/xxxx' */

	/* +1 leaves room for sprintf's trailing null */
	format_str = (char *) palloc(len + 1);

	switch(formattype)
	{
		case 't':
			sprintf((char *) format_str, "delimiter '%s' null '%s' escape '%s'%s%s%s",
					delim, null_print, escape, (header_line ? " header":""),
					(fill_missing ? " fill missing fields":""), (eol_str ?  nl.data : ""));
			break;

		case 'c':
			sprintf((char *) format_str, "delimiter '%s' null '%s' escape '%s' quote '%s'%s%s%s%s%s",
					delim, null_print, escape, quote, (header_line ? " header" : ""),
					(fill_missing ? " fill missing fields":""),
					(force_notnull ? fnn.data :""), (force_quote ? fq.data :""),
					(eol_str ?  nl.data : ""));
			break;

		default:
			/* should never happen */
			Assert(false);
	}

	/* convert c string to text datum */
	result = DirectFunctionCall1(textin, CStringGetDatum(format_str));

	/* clean up */
	if (force_notnull) pfree(fnn.data);
	if (force_quote) pfree(fq.data);
	if (eol_str) pfree(nl.data);
	
	return result;

}

/* Determine the status of the given table with respect to partitioning.
 *
 */
PartStatus rel_part_status(Oid relid)
{
	if (Gp_role != GP_ROLE_DISPATCH)
	{
		ereport(DEBUG1,
				(errmsg("requesting part status outside dispatch - returning none")));
		return PART_STATUS_NONE;
	}

	if ( rel_is_partitioned(relid) )
	{
		Assert( !rel_is_child_partition(relid) && !rel_is_leaf_partition(relid) );
		return PART_STATUS_ROOT;
	}
	else /* not an actual partitioned table root */
	{
		if ( rel_is_child_partition(relid) )
			return rel_is_leaf_partition(relid) ? PART_STATUS_LEAF : PART_STATUS_INTERIOR;
		else /* not a part of a partitioned table */
			Assert( !rel_is_child_partition(relid) );
	}
	return PART_STATUS_NONE;
}

/* Determine whether we can implement a requested distribution on a part of
 * the specified partitioned table.
 *
 * In 3.3
 *   DISTRIBUTED RANDOMLY or distributed just like the whole partitioned
 *   table is implementable.  Anything else is not.
 *
 * rel              Pointer to cache entry for the whole partitioned table
 * dist_cnames      List of column names proposed for distribution some part
 */
bool can_implement_dist_on_part(Relation rel, List *dist_cnames)
{
	ListCell *lc;
	int i;

	if (Gp_role != GP_ROLE_DISPATCH)
	{
		ereport(DEBUG1,
				(errmsg("requesting redistribution outside dispatch - returning no")));
		return false;
	}

	/* Random is okay.  It is represented by a list of one empty list. */
	if ( list_length(dist_cnames) == 1 && linitial(dist_cnames) == NIL )
			return true;

	/* Require an exact match to the policy of the parent. */
	if ( list_length(dist_cnames) != rel->rd_cdbpolicy->nattrs )
			return false;

	i = 0;
	foreach(lc, dist_cnames)
	{
		AttrNumber     attnum;
		char *cname;
		HeapTuple tuple;
		Node *item = lfirst(lc);
		bool ok = false;

		if ( !(item && IsA(item, String)) )
				return false;

		cname = strVal((Value *)item);
		tuple = SearchSysCacheAttName(RelationGetRelid(rel), cname);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							cname,
							RelationGetRelationName(rel)),
									   errOmitLocation(true)));

		attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;
		ok = attnum == rel->rd_cdbpolicy->attrs[i++];
		ReleaseSysCache(tuple);

		if ( ! ok )
			return false;
	}
	return true;
}

/* ALTER TABLE EXCHANGE
 *
 * NB different signature from non-partitioned table Prep functions.
 */
static void
ATPrepExchange(Relation rel, AlterPartitionCmd *pc)
{
	PgPartRule   	   	*prule 	= NULL;
	Relation			 oldrel = NULL;
	Relation			 newrel = NULL;
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		AlterPartitionId *pid = (AlterPartitionId *) pc->partid;
		bool				is_split;
		
		is_split = ((AlterPartitionCmd *)pc->arg2)->arg2 != NULL;
		
		if (is_split)
			return;
		
		prule = get_part_rule(rel, pid, true, true,
							  CurrentMemoryContext, NULL, false);
		if (prule)
		{
			/* cannot exchange a hash partition */
			if ('h' == prule->pNode->part->parkind)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot exchange HASH partition%s "
								"of relation \"%s\"", prule->partIdStr,
								RelationGetRelationName(rel))));
		}
		oldrel = heap_open(prule->topRule->parchildrelid,
						   AccessExclusiveLock);
		newrel = heap_openrv((RangeVar *)pc->arg1,
							 AccessExclusiveLock);
	}
	else
	{
		oldrel = heap_openrv((RangeVar *)pc->partid,
							 AccessExclusiveLock);
		newrel = heap_openrv((RangeVar *)pc->arg1,
							 AccessExclusiveLock);
		
	}
	ATSimplePermissions(rel, false);
	ATSimplePermissions(oldrel, false);
	ATSimplePermissions(newrel, false);
	
	/* Check that old and new look the same, error if not. */
	is_exchangeable(rel, oldrel, newrel, true);
	
	heap_close(oldrel, NoLock);
	heap_close(newrel, NoLock);
}


/* Test whether we can exchange newrel into oldrel's space within the 
 * partitioning hierarchy of rel.
 *
 * If throw is true, throw an appropriate error in case the answer is
 * "no, can't exchange".  If throw is false, just return the answer
 * quietly.
 */
static bool
is_exchangeable(Relation rel, Relation oldrel, Relation newrel, bool throw)
{	
	AttrMap *map_new = NULL;
	AttrMap *map_old = NULL;
	bool congruent = TRUE;
	
	/* Both parts must be relations. */
	if (!(oldrel->rd_rel->relkind == RELKIND_RELATION ||
		  newrel->rd_rel->relkind == RELKIND_RELATION))
	{
		congruent = FALSE;
		if ( throw )
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot exchange relation "
							"which is not a table")));
	}

	/* Attributes of the existing part (oldrel) must be compatible with the 
	 * partitioned table as a whole.  This might be an assertion, but we don't
	 * want this case to pass in a production build, so we use an internal
	 * error.
	 */
	if (congruent && ! map_part_attrs(rel, oldrel, &map_old, FALSE) )
	{
		congruent = FALSE;
		if ( throw )
			elog(ERROR, "existing part \"%s\" not congruent with"
						"partitioned table \"%s\"",
						RelationGetRelationName(oldrel),
						RelationGetRelationName(rel));
	}

	/* From here on we need to be careful to free the maps. */
	
	/* Attributes of new part must be compatible with the partitioned table.
	 * (We assume that the attributes of the old part are compatible.)
	 */
	if ( congruent && ! map_part_attrs(rel, newrel, &map_new, throw) )
		congruent = FALSE;
	
	/* Both parts must have the same owner. */
	if ( congruent && oldrel->rd_rel->relowner != newrel->rd_rel->relowner)
	{
		congruent = FALSE;
		if ( throw )
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("owner of \"%s\" must be the same as that "
							"of \"%s\"",
							RelationGetRelationName(newrel),
							RelationGetRelationName(rel)),
					 errOmitLocation(true)));
	}
	
	/* Both part tables must have the same "WITH OID"s setting */
	if (congruent && oldrel->rd_rel->relhasoids != newrel->rd_rel->relhasoids)
	{
		congruent = FALSE;
		if ( throw )
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("\"%s\" and \"%s\" must have same OIDs setting",
							RelationGetRelationName(rel),
							RelationGetRelationName(newrel)),
					 errOmitLocation(true)));
	}
	
	/* The new part table must not be involved in inheritance. */
	if ( congruent && has_subclass(RelationGetRelid(newrel)))
	{
		congruent = FALSE;
		if ( throw )
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot EXCHANGE table \"%s\" as it has "
							"child table(s)",
							RelationGetRelationName(newrel)),
					 errOmitLocation(true)));
	}
	
	if (congruent && relation_has_supers(RelationGetRelid(newrel)))
	{
		congruent = FALSE;
		if ( throw )
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot exchange table \"%s\" as it "
							"inherits other table(s)",
							RelationGetRelationName(newrel)),
					 errOmitLocation(true)));
	}
	
	/* The new part table must not have rules on it. */
	if ( congruent && ( newrel->rd_rules || oldrel->rd_rules ) )
	{
		congruent = FALSE;
		if ( throw )
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot exchange table which has rules "
							"defined on it"),
					 errOmitLocation(true)));
	}
	
	/* The distribution policies of the existing part (oldpart) and the
	 * candidate part (newpart) must match that of the whole partitioned
	 * table.  However, we can only check this where the policy table is 
	 * populated, i.e., on the entry database.  Note checking the policy
	 * of the existing part is defensive.  It SHOULD match.
	 */
	if (congruent && Gp_role == GP_ROLE_DISPATCH)
	{
		GpPolicy *parpol = rel->rd_cdbpolicy;
		GpPolicy *oldpol = oldrel->rd_cdbpolicy;
		GpPolicy *newpol = newrel->rd_cdbpolicy;
		GpPolicy *adjpol = NULL;
		
		if ( map_old != NULL )
		{
			int i;
			AttrNumber remapped_parent_attr = 0;
			
			for ( i = 0; i < parpol->nattrs; i++ )
			{
				remapped_parent_attr = attrMap(map_old, parpol->attrs[i]);
				
				if ( ! (parpol->attrs[i] > 0  /* assert parent live */
						&& oldpol->attrs[i] > 0   /* assert old part live */
						&& remapped_parent_attr == oldpol->attrs[i] /* assert match */
					))
					elog(ERROR, 
						 "discrepancy in partitioning policy of \"%s\"",
						 RelationGetRelationName(rel));
			}
		}
		else
		{
			if (! GpPolicyEqual(parpol, oldpol) )
				elog(ERROR, 
					 "discrepancy in partitioning policy of \"%s\"",
					 RelationGetRelationName(rel));
		}
		
		if ( map_new != NULL )
		{
			int i;
			adjpol = GpPolicyCopy(CurrentMemoryContext, parpol);
			
			for ( i = 0; i < adjpol->nattrs; i++ )
			{
				adjpol->attrs[i] = attrMap(map_new, parpol->attrs[i]);
				Assert(newpol->attrs[i] > 0); /* check new part */
			}
		}
		else
		{
			adjpol = parpol;
		}
		
		if (! GpPolicyEqual(adjpol, newpol) )
		{
			congruent = FALSE;
			if ( throw )
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("distribution policy for \"%s\" "
								"must be the same as that for \"%s\"",
								RelationGetRelationName(newrel),
								RelationGetRelationName(rel)),
						 errOmitLocation(true)));
		}
		else if (false && memcmp(oldpol->attrs, newpol->attrs,
						sizeof(AttrNumber) * adjpol->nattrs))
		{
			congruent = FALSE;
			if ( throw )
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("distribution policy matches but implementation lags"),
						 errOmitLocation(true)));
		}
	}
	
	if ( map_old != NULL ) pfree(map_old);
	if ( map_new != NULL ) pfree(map_new);
	
	return congruent;
}

/*
 * Return a palloc'd string representing an AlterTableCmd type for use
 * in a message pattern like "can't %s a partitioned table".  The default
 * return string is "alter".
 *
 * This may pose a challenge for localization.  
 */
static
char *alterTableCmdString(AlterTableType subtype)
{
	char *cmdstring = NULL;
	
	switch (subtype)
	{
		case AT_AddColumn: /* add column */
			cmdstring = pstrdup("add a column to");
			break;
			
		case AT_ColumnDefault: /* alter column default */
			cmdstring = pstrdup("alter a column default of");
			break;
			
		case AT_DropNotNull: /* alter column drop not null */
		case AT_SetNotNull: /* alter column set not null */
			cmdstring = pstrdup("alter a column null setting of");
			break;
			
		case AT_SetStatistics: /* alter column statistics */
		case AT_SetStorage: /* alter column storage */
			break;
			
		case AT_DropColumn: /* drop column */
		case AT_DropColumnRecurse: /* internal to commands/tablecmds.c */
			cmdstring = pstrdup("drop a column from");
			break;
			
		case AT_AddIndex: /* add index */
		case AT_ReAddIndex: /* internal to commands/tablecmds.c */
			break;
			
		case AT_AddConstraint: /* add constraint */
			cmdstring = pstrdup("add a constraint to");
			break;
			
		case AT_ProcessedConstraint: /* pre-processed add constraint (local in parser/analyze.c) */
			break;
			
		case AT_DropConstraint: /* drop constraint */
			cmdstring = pstrdup("drop a constraint from");
			break;
			
		case AT_DropConstraintQuietly: /* drop constraint, no error/warning (local in commands/tablecmds.c) */
			break;
			
		case AT_AlterColumnType: /* alter column type */
			cmdstring = pstrdup("alter a column datatype of");
			break;
			
		case AT_ChangeOwner: /* change owner */
			cmdstring = pstrdup("alter the owner of");
			break;
			
		case AT_ClusterOn: /* CLUSTER ON */
		case AT_DropCluster: /* SET WITHOUT CLUSTER */
			break;
			
		case AT_DropOids: /* SET WITHOUT OIDS */
			cmdstring = pstrdup("alter the oid setting of");
			break;
			
		case AT_SetTableSpace: /* SET TABLESPACE */
		case AT_SetRelOptions: /* SET (...) -- AM specific parameters */
		case AT_ResetRelOptions: /* RESET (...) -- AM specific parameters */
			break;
			
		case AT_EnableTrig: /* ENABLE TRIGGER name */
		case AT_DisableTrig: /* DISABLE TRIGGER name */
		case AT_EnableTrigAll: /* ENABLE TRIGGER ALL */
		case AT_DisableTrigAll: /* DISABLE TRIGGER ALL */
		case AT_EnableTrigUser: /* ENABLE TRIGGER USER */
		case AT_DisableTrigUser: /* DISABLE TRIGGER USER */
			cmdstring = pstrdup("enable or disable triggers on");
			break;
			
		case AT_AddInherit: /* INHERIT parent */
		case AT_DropInherit: /* NO INHERIT parent */
			cmdstring = pstrdup("alter inheritance on");
			break;
			
		case AT_SetDistributedBy: /* SET DISTRIBUTED BY */
			break;
			
		case AT_PartAdd: /* Add */
		case AT_PartAlter: /* Alter */
		case AT_PartCoalesce: /* Coalesce */
		case AT_PartDrop: /* Drop */
			break;
			
		case AT_PartExchange: /* Exchange */
			cmdstring = pstrdup("exchange a part into");
			break;
			
		case AT_PartMerge: /* Merge */
			cmdstring = pstrdup("merge parts of");
			break;
			
		case AT_PartModify: /* Modify */
		case AT_PartRename: /* Rename */
		case AT_PartSetTemplate: /* Set Subpartition Template */
			break;
			
		case AT_PartSplit: /* Split */
			cmdstring = pstrdup("split parts of");
			break;
			
		case AT_PartTruncate: /* Truncate */
		case AT_PartAddInternal: /* CREATE TABLE time partition addition */
			break;
	}
	
	if ( cmdstring == NULL )
	{
		cmdstring = pstrdup("alter");
	}
	
	return cmdstring;
}
