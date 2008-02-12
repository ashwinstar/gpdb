/*-------------------------------------------------------------------------
 *
 * parse_utilcmd.c
 *	  Perform parse analysis work for various utility commands
 *
 * Formerly we did this work during parse_analyze() in analyze.c.  However
 * that is fairly unsafe in the presence of querytree caching, since any
 * database state that we depend on in making the transformations might be
 * obsolete by the time the utility command is executed; and utility commands
 * have no infrastructure for holding locks or rechecking plan validity.
 * Hence these functions are now called at the start of execution of their
 * respective utility commands.
 *
 * NOTE: in general we must avoid scribbling on the passed-in raw parse
 * tree, since it might be in a plan cache.  The simplest solution is
 * a quick copyObject() call before manipulating the query tree.
 *
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$PostgreSQL: pgsql/src/backend/parser/parse_utilcmd.c,v 2.8 2008/01/01 19:45:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbhash.h"
#include "cdb/cdbsreh.h"

/* XXX XXX #include "executor/spi.h" */


/* State shared by transformCreateStmt and its subroutines */
typedef struct
{
	const char *stmtType;		/* "CREATE TABLE" or "ALTER TABLE" */
	RangeVar   *relation;		/* relation to create */
	Relation	rel;			/* opened/locked rel, if ALTER */
	List	   *inhRelations;	/* relations to inherit from */
	bool		isalter;		/* true if altering existing table */
	bool		hasoids;		/* does relation have an OID column? */
	List	   *columns;		/* ColumnDef items */
	List	   *ckconstraints;	/* CHECK constraints */
	List	   *fkconstraints;	/* FOREIGN KEY constraints */
	List	   *ixconstraints;	/* index-creating constraints */
	List	   *inh_indexes;	/* cloned indexes from INCLUDING INDEXES */
	List	   *blist;			/* "before list" of things to do before
								 * creating the table */
	List	   *alist;			/* "after list" of things to do after creating
								 * the table */
	IndexStmt  *pkey;			/* PRIMARY KEY index, if any */
} CreateStmtContext;

/* State shared by transformCreateSchemaStmt and its subroutines */
typedef struct
{
	const char *stmtType;		/* "CREATE SCHEMA" or "ALTER SCHEMA" */
	char	   *schemaname;		/* name of schema */
	char	   *authid;			/* owner of schema */
	List	   *sequences;		/* CREATE SEQUENCE items */
	List	   *tables;			/* CREATE TABLE items */
	List	   *views;			/* CREATE VIEW items */
	List	   *indexes;		/* CREATE INDEX items */
	List	   *triggers;		/* CREATE TRIGGER items */
	List	   *grants;			/* GRANT items */
} CreateSchemaStmtContext;

/* state structures for validing parsed partition specifications */
typedef struct
{
	ParseState *pstate;
	CreateStmtContext *cxt;
	CreateStmt *stmt;
	PartitionBy *pBy;
	PartitionElem *pElem;
	PartitionElem *prevElem;
	PartitionBoundSpec *pBSpec;
	int partNumber;
	char *namBuf;
	char *at_depth;
	bool prevHadName;
	int prevStartEnd;
	List *allRangeVals;
	List *allListVals;
} partValidationState;

static void transformColumnDefinition(ParseState *pstate,
						  CreateStmtContext *cxt,
						  ColumnDef *column);
static void transformTableConstraint(ParseState *pstate,
						 CreateStmtContext *cxt,
						 Constraint *constraint);
static void transformInhRelation(ParseState *pstate, CreateStmtContext *cxt,
					 InhRelation *inhrelation);
static IndexStmt *generateClonedIndexStmt(CreateStmtContext *cxt,
						Relation parent_index, AttrNumber *attmap);
static List *get_opclass(Oid opclass, Oid actual_datatype);
static void transformIndexConstraints(ParseState *pstate,
						  CreateStmtContext *cxt);
static IndexStmt *transformIndexConstraint(Constraint *constraint,
						 CreateStmtContext *cxt);
static void transformFKConstraints(ParseState *pstate,
					   CreateStmtContext *cxt,
					   bool skipValidation,
					   bool isAddConstraint);
static void transformConstraintAttrs(List *constraintList);
static void transformColumnType(ParseState *pstate, ColumnDef *column);
static void setSchemaName(char *context_schema, char **stmt_schema_name);

static void transformDistributedBy(ParseState *pstate,
		   CreateStmtContext *cxt, List *distributedBy, GpPolicy ** policy, List *likeDistributedBy);

static void transformPartitionBy(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, GpPolicy *policy);

static void setQryDistributionPolicy(SelectStmt *stmt, Query *qry);
static void transformSingleRowErrorHandling(ParseState *pstate, CreateStmtContext *cxt,
											SingleRowErrorDesc *sreh);

static List *make_partition_rules(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
 		 char *at_depth, char *child_name_str, 
		 int partNumId, int maxPartNum, 
		 int everyOffset, int maxEveryOffset,
		 ListCell	**pp_lc_anp);

static Node *make_prule_catalog(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
 		 char *at_depth, char *child_name_str, 
		 char *exprBuf,
		 Node *pWhere							 
		);

static Node *make_prule_rulestmt(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
 		 char *at_depth, char *child_name_str, 
		 char *exprBuf,
		 Node *pWhere							 
		);

static A_Const *make_a_const(A_Const *pValConst, char *val_str);

static int deparse_partition_rule(Node *pNode, char *outbuf, size_t outsize);

static int partition_range_compare(ParseState *pstate,
								   CreateStmtContext *cxt, CreateStmt *stmt, 
								   PartitionBy *pBy,
								   char	   *at_depth,
								   int		partNumber,
								   char	   *compare_op,
								   PartitionRangeItem *pRI1,
								   PartitionRangeItem *pRI2);

static int validate_partition_spec(ParseState *pstate,
								   CreateStmtContext *cxt, CreateStmt *stmt, 
								   PartitionBy *partitionBy, 	
								   char	   *at_depth,
								   int		partNumber);

static int partition_range_every(ParseState *pstate,
								 CreateStmtContext *cxt, CreateStmt *stmt, 
								 PartitionBy *partitionBy, 	
								 char	   *at_depth);
static Datum eval_basic_opexpr(ParseState *pstate, List *oprname, 
							   Node *leftarg, Node *rightarg, Oid *restypid);

/*
 * transformCreateStmt -
 *	  parse analysis for CREATE TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed CreateStmt, but there may be additional actions
 * to be done before and after the actual DefineRelation() call.
 *
 * SQL92 allows constraints to be scattered all over, so thumb through
 * the columns and collect all constraints into one place.
 * If there are any implied indices (e.g. UNIQUE or PRIMARY KEY)
 * then expand those into multiple IndexStmt blocks.
 *	  - thomas 1997-12-02
 */
List *
transformCreateStmt(CreateStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *elements;
	List  	   *likeDistributedBy = NIL;

	/*
	 * We must not scribble on the passed-in CreateStmt, so copy it.  (This is
	 * overkill, but easy.)
	 */
	stmt = (CreateStmt *) copyObject(stmt);

	/*
	 * If the target relation name isn't schema-qualified, make it so.  This
	 * prevents some corner cases in which added-on rewritten commands might
	 * think they should apply to other relations that have the same name and
	 * are earlier in the search path.	"istemp" is equivalent to a
	 * specification of pg_temp, so no need for anything extra in that case.
	 */
	if (stmt->relation->schemaname == NULL && !stmt->relation->istemp)
	{
		Oid			namespaceid = RangeVarGetCreationNamespace(stmt->relation);

		stmt->relation->schemaname = get_namespace_name(namespaceid);
	}

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	cxt.stmtType = "CREATE TABLE";
	cxt.relation = stmt->relation;
	cxt.rel = NULL;
	cxt.inhRelations = stmt->inhRelations;
	cxt.isalter = false;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.inh_indexes = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.hasoids = interpretOidsOption(stmt->options);

	stmt->policy = NULL;

	/*
	 * Run through each primary element in the table creation clause. Separate
	 * column defs from constraints, and do preliminary analysis.
	 */
	foreach(elements, stmt->tableElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_ColumnDef:
				transformColumnDefinition(pstate, &cxt,
										  (ColumnDef *) element);
				break;

			case T_Constraint:
				transformTableConstraint(pstate, &cxt,
										 (Constraint *) element);
				break;

			case T_FkConstraint:
				/* No pre-transformation needed */
				cxt.fkconstraints = lappend(cxt.fkconstraints, element);
				break;

			case T_InhRelation:
				{
					bool		isBeginning = (cxt.columns == NIL);

					transformInhRelation(pstate, &cxt,
										 (InhRelation *) element);

					/*
					 * For Greenplum Database distributed tables, default to 
					 * the same distribution as the first LIKE table, unless 
					 * we also have INHERITS
					 */
					if (Gp_role == GP_ROLE_DISPATCH && 
						stmt->distributedBy == NIL && 
						stmt->inhRelations == NIL && isBeginning && 
						stmt->policy == NULL)
					{
						InhRelation *e = (InhRelation *) element;
						Oid	relId = RangeVarGetRelid(e->relation, false);
						GpPolicy  *oldTablePolicy = 
							GpPolicyFetch(CurrentMemoryContext, relId);

						if (oldTablePolicy != NULL && 
							oldTablePolicy->ptype == POLICYTYPE_PARTITIONED)
						{
							int ia;
						
							if (oldTablePolicy->nattrs > 0)
							{
								for (ia=0;ia<oldTablePolicy->nattrs;ia++)
								{
									char *attname = get_attname(relId, oldTablePolicy->attrs[ia]);
									if (likeDistributedBy)
										likeDistributedBy = lappend(likeDistributedBy, (Node *) makeString(attname));
									else
										likeDistributedBy = list_make1((Node *) makeString(attname));
								}
							}
							else
							{	/* old table is distributed randomly. */
								likeDistributedBy = list_make1((Node *) NULL);
							}
						}
					}
				}
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
				break;
		}
	}

	/*
	 * transformIndexConstraints wants cxt.alist to contain only index
	 * statements, so transfer anything we already have into save_alist.
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	Assert(stmt->constraints == NIL);

	/*
	 * Postprocess constraints that give rise to index definitions.
	 */
	transformIndexConstraints(pstate, &cxt);

	/*
	 * Postprocess foreign-key constraints.
	 */
	transformFKConstraints(pstate, &cxt, true, false);

	/*
	 * Postprocess Greenplum Database distribution columns
	 */
	transformDistributedBy(pstate, &cxt, stmt->distributedBy, &stmt->policy, likeDistributedBy);

	/*
	 * Postprocess Greenplum Database partitioned tables
	 */
	transformPartitionBy(pstate, &cxt, stmt, stmt->partitionBy, stmt->policy);

	/*
	 * Output results.
	 */
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}

List *
transformCreateExternalStmt(CreateExternalStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *elements;

	/*
	 * We must not scribble on the passed-in CreateExternalStmt, so copy it.  (This is
	 * overkill, but easy.)
	 */
	stmt = (CreateExternalStmt *) copyObject(stmt);

	/*
	 * If the target relation name isn't schema-qualified, make it so.  This
	 * prevents some corner cases in which added-on rewritten commands might
	 * think they should apply to other relations that have the same name and
	 * are earlier in the search path.	"istemp" is equivalent to a
	 * specification of pg_temp, so no need for anything extra in that case.
	 */
	if (stmt->relation->schemaname == NULL && !stmt->relation->istemp)
	{
		Oid			namespaceid = RangeVarGetCreationNamespace(stmt->relation);

		stmt->relation->schemaname = get_namespace_name(namespaceid);
	}

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	cxt.stmtType = "CREATE EXTERNAL TABLE";
	cxt.relation = stmt->relation;
	cxt.inhRelations = NIL;
	cxt.hasoids = false;
	cxt.isalter = false;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	
	cxt.blist = NIL;
	cxt.alist = NIL;
	
	/*
	 * Run through each primary element in the table creation clause. Separate
	 * column defs from constraints, and do preliminary analysis.
	 */
	foreach(elements, stmt->tableElts)
	{
		Node	   *element = lfirst(elements);
		
		switch (nodeTag(element))
		{
			case T_ColumnDef:
				transformColumnDefinition(pstate, &cxt,
										  (ColumnDef *) element);
				break;
				
			case T_Constraint:
			case T_FkConstraint:
			case T_InhRelation:
				/* should never happen. If it does fix gram.y */
				elog(ERROR, "node type %d not allowed for external table definition",
					 (int) nodeTag(element));
				break;
				
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
				break;
		}
	}
	
	/*
	 * Check if we need to create an error table. If so, add it to the 
	 * before list.
	 */
	if(stmt->sreh && ((SingleRowErrorDesc *)stmt->sreh)->errtable)
		transformSingleRowErrorHandling(pstate, &cxt,
										(SingleRowErrorDesc *) stmt->sreh);
	
	Assert(cxt.ckconstraints == NIL);
	Assert(cxt.fkconstraints == NIL);
	Assert(cxt.ixconstraints == NIL);
	
	/*
	 * Output results.
	 */
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}

/*
 * transformColumnDefinition -
 *		transform a single ColumnDef within CREATE TABLE
 *		Also used in ALTER TABLE ADD COLUMN
 */
static void
transformColumnDefinition(ParseState *pstate, CreateStmtContext *cxt,
						  ColumnDef *column)
{
	bool		is_serial;
	bool		saw_nullable;
	bool		saw_default;
	Constraint *constraint;
	ListCell   *clist;

	cxt->columns = lappend(cxt->columns, column);

	/* Check for SERIAL pseudo-types */
	is_serial = false;
	if (list_length(column->typname->names) == 1)
	{
		char	   *typname = strVal(linitial(column->typname->names));

		if (strcmp(typname, "serial") == 0 ||
			strcmp(typname, "serial4") == 0)
		{
			is_serial = true;
			column->typname->names = NIL;
			column->typname->typid = INT4OID;
		}
		else if (strcmp(typname, "bigserial") == 0 ||
				 strcmp(typname, "serial8") == 0)
		{
			is_serial = true;
			column->typname->names = NIL;
			column->typname->typid = INT8OID;
		}
	}

	/* Do necessary work on the column type declaration */
	transformColumnType(pstate, column);

	/* Special actions for SERIAL pseudo-types */
	if (is_serial)
	{
		Oid			snamespaceid;
		char	   *snamespace;
		char	   *sname;
		char	   *qstring;
		A_Const    *snamenode;
		FuncCall   *funccallnode;
		CreateSeqStmt *seqstmt;
		AlterSeqStmt *altseqstmt;
		List	   *attnamelist;

		/*
		 * Determine namespace and name to use for the sequence.
		 *
		 * Although we use ChooseRelationName, it's not guaranteed that the
		 * selected sequence name won't conflict; given sufficiently long
		 * field names, two different serial columns in the same table could
		 * be assigned the same sequence name, and we'd not notice since we
		 * aren't creating the sequence quite yet.  In practice this seems
		 * quite unlikely to be a problem, especially since few people would
		 * need two serial columns in one table.
		 */
		if (cxt->rel)
			snamespaceid = RelationGetNamespace(cxt->rel);
		else
			snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
		snamespace = get_namespace_name(snamespaceid);
		sname = ChooseRelationName(cxt->relation->relname,
								   column->colname,
								   "seq",
								   snamespaceid);

		ereport(NOTICE,
				(errmsg("%s will create implicit sequence \"%s\" for serial column \"%s.%s\"",
						cxt->stmtType, sname,
						cxt->relation->relname, column->colname)));

		/*
		 * Build a CREATE SEQUENCE command to create the sequence object, and
		 * add it to the list of things to be done before this CREATE/ALTER
		 * TABLE.
		 */
		seqstmt = makeNode(CreateSeqStmt);
		seqstmt->sequence = makeRangeVar(snamespace, sname);
		seqstmt->sequence->istemp = cxt->relation->istemp;
		seqstmt->options = NIL;
		

		cxt->blist = lappend(cxt->blist, seqstmt);

		/*
		 * Build an ALTER SEQUENCE ... OWNED BY command to mark the sequence
		 * as owned by this column, and add it to the list of things to be
		 * done after this CREATE/ALTER TABLE.
		 */
		altseqstmt = makeNode(AlterSeqStmt);
		altseqstmt->sequence = makeRangeVar(snamespace, sname);
		attnamelist = list_make3(makeString(snamespace),
								 makeString(cxt->relation->relname),
								 makeString(column->colname));
		altseqstmt->options = list_make1(makeDefElem("owned_by",
													 (Node *) attnamelist));

		cxt->alist = lappend(cxt->alist, altseqstmt);

		/*
		 * Create appropriate constraints for SERIAL.  We do this in full,
		 * rather than shortcutting, so that we will detect any conflicting
		 * constraints the user wrote (like a different DEFAULT).
		 *
		 * Create an expression tree representing the function call
		 * nextval('sequencename').  We cannot reduce the raw tree to cooked
		 * form until after the sequence is created, but there's no need to do
		 * so.
		 */
		qstring = quote_qualified_identifier(snamespace, sname);
		snamenode = makeNode(A_Const);
		snamenode->val.type = T_String;
		snamenode->val.val.str = qstring;
		snamenode->typname = SystemTypeName("regclass");
		funccallnode = makeNode(FuncCall);
		funccallnode->funcname = SystemFuncName("nextval");
		funccallnode->args = list_make1(snamenode);
		funccallnode->agg_star = false;
		funccallnode->agg_distinct = false;
		funccallnode->location = -1;

		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_DEFAULT;
		constraint->raw_expr = (Node *) funccallnode;
		constraint->cooked_expr = NULL;
		constraint->keys = NIL;
		column->constraints = lappend(column->constraints, constraint);

		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_NOTNULL;
		column->constraints = lappend(column->constraints, constraint);
	}

	/* Process column constraints, if any... */
	transformConstraintAttrs(column->constraints);

	saw_nullable = false;
	saw_default = false;

	foreach(clist, column->constraints)
	{
		constraint = lfirst(clist);

		/*
		 * If this column constraint is a FOREIGN KEY constraint, then we fill
		 * in the current attribute's name and throw it into the list of FK
		 * constraints to be processed later.
		 */
		if (IsA(constraint, FkConstraint))
		{
			FkConstraint *fkconstraint = (FkConstraint *) constraint;

			fkconstraint->fk_attrs = list_make1(makeString(column->colname));
			cxt->fkconstraints = lappend(cxt->fkconstraints, fkconstraint);
			continue;
		}

		Assert(IsA(constraint, Constraint));

		switch (constraint->contype)
		{
			case CONSTR_NULL:
				if (saw_nullable && column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
								  column->colname, cxt->relation->relname)));
				column->is_not_null = FALSE;
				saw_nullable = true;
				break;

			case CONSTR_NOTNULL:
				if (saw_nullable && !column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
								  column->colname, cxt->relation->relname)));
				column->is_not_null = TRUE;
				saw_nullable = true;
				break;

			case CONSTR_DEFAULT:
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
								  column->colname, cxt->relation->relname)));
				/* Note: DEFAULT NULL maps to constraint->raw_expr == NULL */
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_default = true;
				break;

			case CONSTR_PRIMARY:
			case CONSTR_UNIQUE:
				if (constraint->keys == NIL)
					constraint->keys = list_make1(makeString(column->colname));
				cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
				break;

			case CONSTR_CHECK:
				cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				/* transformConstraintAttrs took care of these */
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 constraint->contype);
				break;
		}
	}
}

/*
 * transformTableConstraint
 *		transform a Constraint node within CREATE TABLE or ALTER TABLE
 */
static void
transformTableConstraint(ParseState *pstate, CreateStmtContext *cxt,
						 Constraint *constraint)
{
	switch (constraint->contype)
	{
		case CONSTR_PRIMARY:
		case CONSTR_UNIQUE:
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_CHECK:
			cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
			break;

		case CONSTR_NULL:
		case CONSTR_NOTNULL:
		case CONSTR_DEFAULT:
		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			elog(ERROR, "invalid context for constraint type %d",
				 constraint->contype);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 constraint->contype);
			break;
	}
}

/*
 * transformInhRelation
 *
 * Change the LIKE <subtable> portion of a CREATE TABLE statement into
 * column definitions which recreate the user defined column portions of
 * <subtable>.
 */
static void
transformInhRelation(ParseState *pstate, CreateStmtContext *cxt,
					 InhRelation *inhRelation)
{
	AttrNumber	parent_attno;
	Relation	relation;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	AclResult	aclresult;
	bool		including_defaults = false;
	bool		including_constraints = false;
	bool		including_indexes = false;
	ListCell   *elem;

	relation = heap_openrv(inhRelation->relation, AccessShareLock);

	if (relation->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("inherited relation \"%s\" is not a table",
						inhRelation->relation->relname)));

	/*
	 * Check for SELECT privilages
	 */
	aclresult = pg_class_aclcheck(RelationGetRelid(relation), GetUserId(),
								  ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(relation));

	tupleDesc = RelationGetDescr(relation);
	constr = tupleDesc->constr;

	foreach(elem, inhRelation->options)
	{
		int			option = lfirst_int(elem);

		switch (option)
		{
			case CREATE_TABLE_LIKE_INCLUDING_DEFAULTS:
				including_defaults = true;
				break;
			case CREATE_TABLE_LIKE_EXCLUDING_DEFAULTS:
				including_defaults = false;
				break;
			case CREATE_TABLE_LIKE_INCLUDING_CONSTRAINTS:
				including_constraints = true;
				break;
			case CREATE_TABLE_LIKE_EXCLUDING_CONSTRAINTS:
				including_constraints = false;
				break;
			case CREATE_TABLE_LIKE_INCLUDING_INDEXES:
				including_indexes = true;
				break;
			case CREATE_TABLE_LIKE_EXCLUDING_INDEXES:
				including_indexes = false;
				break;
			default:
				elog(ERROR, "unrecognized CREATE TABLE LIKE option: %d",
					 option);
		}
	}

	/*
	 * Insert the copied attributes into the cxt for the new table definition.
	 */
	for (parent_attno = 1; parent_attno <= tupleDesc->natts;
		 parent_attno++)
	{
		Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
		char	   *attributeName = NameStr(attribute->attname);
		ColumnDef  *def;

		/*
		 * Ignore dropped columns in the parent.
		 */
		if (attribute->attisdropped)
			continue;

		/*
		 * Create a new column, which is marked as NOT inherited.
		 *
		 * For constraints, ONLY the NOT NULL constraint is inherited by the
		 * new column definition per SQL99.
		 */
		def = makeNode(ColumnDef);
		def->colname = pstrdup(attributeName);
		def->typname = makeTypeNameFromOid(attribute->atttypid,
											attribute->atttypmod);
		def->inhcount = 0;
		def->is_local = true;
		def->is_not_null = attribute->attnotnull;
		def->raw_default = NULL;
		def->cooked_default = NULL;
		def->constraints = NIL;

		/*
		 * Add to column list
		 */
		cxt->columns = lappend(cxt->columns, def);

		/*
		 * Copy default, if present and the default has been requested
		 */
		if (attribute->atthasdef && including_defaults)
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
			 * If default expr could contain any vars, we'd need to fix 'em,
			 * but it can't; so default is ready to apply to child.
			 */

			def->cooked_default = pstrdup(this_default);
		}
	}

	/*
	 * Copy CHECK constraints if requested, being careful to adjust attribute
	 * numbers
	 */
	if (including_constraints && tupleDesc->constr)
	{
		AttrNumber *attmap = varattnos_map_schema(tupleDesc, cxt->columns);
		int			ccnum;

		for (ccnum = 0; ccnum < tupleDesc->constr->num_check; ccnum++)
		{
			char	   *ccname = tupleDesc->constr->check[ccnum].ccname;
			char	   *ccbin = tupleDesc->constr->check[ccnum].ccbin;
			Node	   *ccbin_node = stringToNode(ccbin);
			Constraint *n = makeNode(Constraint);

			change_varattnos_of_a_node(ccbin_node, attmap);

			n->contype = CONSTR_CHECK;
			n->name = pstrdup(ccname);
			n->raw_expr = NULL;
			n->cooked_expr = nodeToString(ccbin_node);
			n->indexspace = NULL;
			cxt->ckconstraints = lappend(cxt->ckconstraints, (Node *) n);
		}
	}

	/*
	 * Likewise, copy indexes if requested
	 */
	if (including_indexes && relation->rd_rel->relhasindex)
	{
		AttrNumber *attmap = varattnos_map_schema(tupleDesc, cxt->columns);
		List	   *parent_indexes;
		ListCell   *l;

		parent_indexes = RelationGetIndexList(relation);

		foreach(l, parent_indexes)
		{
			Oid			parent_index_oid = lfirst_oid(l);
			Relation	parent_index;
			IndexStmt  *index_stmt;

			parent_index = index_open(parent_index_oid, AccessShareLock);

			/* Build CREATE INDEX statement to recreate the parent_index */
			index_stmt = generateClonedIndexStmt(cxt, parent_index, attmap);

			/* Save it in the inh_indexes list for the time being */
			cxt->inh_indexes = lappend(cxt->inh_indexes, index_stmt);

			index_close(parent_index, AccessShareLock);
		}
	}

	/*
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.	That will prevent someone else from deleting or ALTERing the
	 * parent before the child is committed.
	 */
	heap_close(relation, NoLock);
}

/*
 * Generate an IndexStmt node using information from an already existing index
 * "source_idx".  Attribute numbers should be adjusted according to attmap.
 */
static IndexStmt *
generateClonedIndexStmt(CreateStmtContext *cxt, Relation source_idx,
						AttrNumber *attmap)
{
	Oid			source_relid = RelationGetRelid(source_idx);
	HeapTuple	ht_idxrel;
	HeapTuple	ht_idx;
	Form_pg_class idxrelrec;
	Form_pg_index idxrec;
	Form_pg_am	amrec;
	oidvector  *indclass;
	IndexStmt  *index;
	List	   *indexprs;
	ListCell   *indexpr_item;
	Oid			indrelid;
	int			keyno;
	Oid			keycoltype;
	Datum		datum;
	bool		isnull;

	/*
	 * Fetch pg_class tuple of source index.  We can't use the copy in the
	 * relcache entry because it doesn't include optional fields.
	 */
	ht_idxrel = SearchSysCache(RELOID,
							   ObjectIdGetDatum(source_relid),
							   0, 0, 0);
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", source_relid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* Fetch pg_index tuple for source index from relcache entry */
	ht_idx = source_idx->rd_indextuple;
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);
	indrelid = idxrec->indrelid;

	/* Fetch pg_am tuple for source index from relcache entry */
	amrec = source_idx->rd_am;

	/* Must get indclass the hard way, since it's not stored in relcache */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(datum);

	/* Begin building the IndexStmt */
	index = makeNode(IndexStmt);
    index->idxOids = NULL;

	index->relation = cxt->relation;
	index->accessMethod = pstrdup(NameStr(amrec->amname));
	index->tableSpace = get_tablespace_name(source_idx->rd_node.spcNode);
	index->unique = idxrec->indisunique;
	index->primary = idxrec->indisprimary;
	index->concurrent = false;

	/*
	 * We don't try to preserve the name of the source index; instead, just
	 * let DefineIndex() choose a reasonable name.
	 */
	index->idxname = NULL;

	/*
	 * If the index is marked PRIMARY, it's certainly from a constraint;
	 * else, if it's not marked UNIQUE, it certainly isn't; else, we have
	 * to search pg_depend to see if there's an associated unique constraint.
	 */
	if (index->primary)
		index->isconstraint = true;
	else if (!index->unique)
		index->isconstraint = false;
	else
		index->isconstraint = OidIsValid(get_index_constraint(source_relid));

	/* Get the index expressions, if any */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indexprs, &isnull);
	if (!isnull)
	{
		char	   *exprsString;

		exprsString = DatumGetCString(DirectFunctionCall1(textout, datum));
		indexprs = (List *) stringToNode(exprsString);
	}
	else
		indexprs = NIL;

	/* Build the list of IndexElem */
	index->indexParams = NIL;

	indexpr_item = list_head(indexprs);
	for (keyno = 0; keyno < idxrec->indnatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		int16		opt = source_idx->rd_indoption[keyno];

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* Simple index column */
			char	   *attname;

			attname = get_relid_attribute_name(indrelid, attnum);
			keycoltype = get_atttype(indrelid, attnum);

			iparam->name = attname;
			iparam->expr = NULL;
		}
		else
		{
			/* Expressional index */
			Node	   *indexkey;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexpr_item);

			/* OK to modify indexkey since we are working on a private copy */
			change_varattnos_of_a_node(indexkey, attmap);

			iparam->name = NULL;
			iparam->expr = indexkey;

			keycoltype = exprType(indexkey);
		}

		/* Add the operator class name, if non-default */
		iparam->opclass = get_opclass(indclass->values[keyno], keycoltype);

		iparam->ordering = SORTBY_DEFAULT;
		iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;

		/* Adjust options if necessary */
		if (amrec->amcanorder)
		{
			/*
			 * If it supports sort ordering, copy DESC and NULLS opts.
			 * Don't set non-default settings unnecessarily, though,
			 * so as to improve the chance of recognizing equivalence
			 * to constraint indexes.
			 */
			if (opt & INDOPTION_DESC)
			{
				iparam->ordering = SORTBY_DESC;
				if ((opt & INDOPTION_NULLS_FIRST) == 0)
					iparam->nulls_ordering = SORTBY_NULLS_LAST;
			}
			else
			{
				if (opt & INDOPTION_NULLS_FIRST)
					iparam->nulls_ordering = SORTBY_NULLS_FIRST;
			}
		}

		index->indexParams = lappend(index->indexParams, iparam);
	}

	/* Copy reloptions if any */
	datum = SysCacheGetAttr(RELOID, ht_idxrel,
							Anum_pg_class_reloptions, &isnull);
	if (!isnull)
		index->options = untransformRelOptions(datum);

	/* If it's a partial index, decompile and append the predicate */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indpred, &isnull);
	if (!isnull)
	{
		char	   *pred_str;

		/* Convert text string to node tree */
		pred_str = DatumGetCString(DirectFunctionCall1(textout, datum));
		index->whereClause = (Node *) stringToNode(pred_str);
		/* Adjust attribute numbers */
		change_varattnos_of_a_node(index->whereClause, attmap);
	}

	/* Clean up */
	ReleaseSysCache(ht_idxrel);

	return index;
}

/*
 * get_opclass			- fetch name of an index operator class
 *
 * If the opclass is the default for the given actual_datatype, then
 * the return value is NIL.
 */
static List *
get_opclass(Oid opclass, Oid actual_datatype)
{
	HeapTuple	ht_opc;
	Form_pg_opclass opc_rec;
	List	   *result = NIL;

	ht_opc = SearchSysCache(CLAOID,
							ObjectIdGetDatum(opclass),
							0, 0, 0);
	if (!HeapTupleIsValid(ht_opc))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	opc_rec = (Form_pg_opclass) GETSTRUCT(ht_opc);

	if (GetDefaultOpClass(actual_datatype, opc_rec->opcmethod) != opclass)
	{
		/* For simplicity, we always schema-qualify the name */
		char	   *nsp_name = get_namespace_name(opc_rec->opcnamespace);
		char	   *opc_name = pstrdup(NameStr(opc_rec->opcname));

		result = list_make2(makeString(nsp_name), makeString(opc_name));
	}

	ReleaseSysCache(ht_opc);
	return result;
}


/*
 * transformIndexConstraints
 *		Handle UNIQUE and PRIMARY KEY constraints, which create indexes.
 *		We also merge in any index definitions arising from
 *		LIKE ... INCLUDING INDEXES.
 */
static void
transformIndexConstraints(ParseState *pstate, CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *indexlist = NIL;
	ListCell   *lc;

	/*
	 * Run through the constraints that need to generate an index. For PRIMARY
	 * KEY, mark each column as NOT NULL and create an index. For UNIQUE,
	 * create an index as for PRIMARY KEY, but do not insist on NOT NULL.
	 */
	foreach(lc, cxt->ixconstraints)
	{
		Constraint *constraint = (Constraint *) lfirst(lc);

		Assert(IsA(constraint, Constraint));
		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE);

		index = transformIndexConstraint(constraint, cxt);

		indexlist = lappend(indexlist, index);
	}

	/* Add in any indexes defined by LIKE ... INCLUDING INDEXES */
	foreach(lc, cxt->inh_indexes)
	{
		index = (IndexStmt *) lfirst(lc);

		if (index->primary)
		{
			if (cxt->pkey != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("multiple primary keys for table \"%s\" are not allowed",
								cxt->relation->relname)));
			cxt->pkey = index;
		}

		indexlist = lappend(indexlist, index);
	}

	/*
	 * Scan the index list and remove any redundant index specifications. This
	 * can happen if, for instance, the user writes UNIQUE PRIMARY KEY. A
	 * strict reading of SQL92 would suggest raising an error instead, but
	 * that strikes me as too anal-retentive. - tgl 2001-02-14
	 *
	 * XXX in ALTER TABLE case, it'd be nice to look for duplicate
	 * pre-existing indexes, too.
	 */
	Assert(cxt->alist == NIL);
	if (cxt->pkey != NULL)
	{
		/* Make sure we keep the PKEY index in preference to others... */
		cxt->alist = list_make1(cxt->pkey);
	}

	foreach(lc, indexlist)
	{
		bool		keep = true;
		ListCell   *k;

		index = lfirst(lc);

		/* if it's pkey, it's already in cxt->alist */
		if (index == cxt->pkey)
			continue;

		foreach(k, cxt->alist)
		{
			IndexStmt  *priorindex = lfirst(k);

			if (equal(index->indexParams, priorindex->indexParams) &&
				equal(index->whereClause, priorindex->whereClause) &&
				strcmp(index->accessMethod, priorindex->accessMethod) == 0)
			{
				priorindex->unique |= index->unique;
				/*
				 * If the prior index is as yet unnamed, and this one is
				 * named, then transfer the name to the prior index. This
				 * ensures that if we have named and unnamed constraints,
				 * we'll use (at least one of) the names for the index.
				 */
				if (priorindex->idxname == NULL)
					priorindex->idxname = index->idxname;
				keep = false;
				break;
			}
		}

		if (keep)
			cxt->alist = lappend(cxt->alist, index);
	}
}

/*
 * transformIndexConstraint
 *		Transform one UNIQUE or PRIMARY KEY constraint for
 *		transformIndexConstraints.
 */
static IndexStmt *
transformIndexConstraint(Constraint *constraint, CreateStmtContext *cxt)
{
	IndexStmt  *index;
	ListCell   *keys;
	IndexElem  *iparam;

	index = makeNode(IndexStmt);
    index->idxOids = NULL;

	index->unique = true;
	index->primary = (constraint->contype == CONSTR_PRIMARY);
	if (index->primary)
	{
		if (cxt->pkey != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
			 errmsg("multiple primary keys for table \"%s\" are not allowed",
					cxt->relation->relname)));
		cxt->pkey = index;

		/*
		 * In ALTER TABLE case, a primary index might already exist, but
		 * DefineIndex will check for it.
		 */
	}
	index->isconstraint = true;

	if (constraint->name != NULL)
		index->idxname = pstrdup(constraint->name);
	else
		index->idxname = NULL;	/* DefineIndex will choose name */

	index->relation = cxt->relation;
	index->accessMethod = DEFAULT_INDEX_TYPE;
	index->options = constraint->options;
	index->tableSpace = constraint->indexspace;
	index->indexParams = NIL;
	index->whereClause = NULL;
	index->concurrent = false;

	/*
	 * Make sure referenced keys exist.  If we are making a PRIMARY KEY index,
	 * also make sure they are NOT NULL, if possible. (Although we could leave
	 * it to DefineIndex to mark the columns NOT NULL, it's more efficient to
	 * get it right the first time.)
	 */
	foreach(keys, constraint->keys)
	{
		char	   *key = strVal(lfirst(keys));
		bool		found = false;
		ColumnDef  *column = NULL;
		ListCell   *columns;

		foreach(columns, cxt->columns)
		{
			column = (ColumnDef *) lfirst(columns);
			Assert(IsA(column, ColumnDef));
			if (strcmp(column->colname, key) == 0)
			{
				found = true;
				break;
			}
		}
		if (found)
		{
			/* found column in the new table; force it to be NOT NULL */
			if (constraint->contype == CONSTR_PRIMARY)
				column->is_not_null = TRUE;
		}
		else if (SystemAttributeByName(key, cxt->hasoids) != NULL)
		{
			/*
			 * column will be a system column in the new table, so accept it.
			 * System columns can't ever be null, so no need to worry about
			 * PRIMARY/NOT NULL constraint.
			 */
			found = true;
		}
		else if (cxt->inhRelations)
		{
			/* try inherited tables */
			ListCell   *inher;

			foreach(inher, cxt->inhRelations)
			{
				RangeVar   *inh = (RangeVar *) lfirst(inher);
				Relation	rel;
				int			count;

				Assert(IsA(inh, RangeVar));
				rel = heap_openrv(inh, AccessShareLock);
				if (rel->rd_rel->relkind != RELKIND_RELATION)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						   errmsg("inherited relation \"%s\" is not a table",
								  inh->relname)));
				for (count = 0; count < rel->rd_att->natts; count++)
				{
					Form_pg_attribute inhattr = rel->rd_att->attrs[count];
					char	   *inhname = NameStr(inhattr->attname);

					if (inhattr->attisdropped)
						continue;
					if (strcmp(key, inhname) == 0)
					{
						found = true;

						/*
						 * We currently have no easy way to force an inherited
						 * column to be NOT NULL at creation, if its parent
						 * wasn't so already. We leave it to DefineIndex to
						 * fix things up in this case.
						 */
						break;
					}
				}
				heap_close(rel, NoLock);
				if (found)
					break;
			}
		}

		/*
		 * In the ALTER TABLE case, don't complain about index keys not
		 * created in the command; they may well exist already. DefineIndex
		 * will complain about them if not, and will also take care of marking
		 * them NOT NULL.
		 */
		if (!found && !cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" named in key does not exist",
							key)));

		/* Check for PRIMARY KEY(foo, foo) */
		foreach(columns, index->indexParams)
		{
			iparam = (IndexElem *) lfirst(columns);
			if (iparam->name && strcmp(key, iparam->name) == 0)
			{
				if (index->primary)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" appears twice in primary key constraint",
									key)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
					errmsg("column \"%s\" appears twice in unique constraint",
						   key)));
			}
		}

		/* OK, add it to the index definition */
		iparam = makeNode(IndexElem);
		iparam->name = pstrdup(key);
		iparam->expr = NULL;
		iparam->opclass = NIL;
		iparam->ordering = SORTBY_DEFAULT;
		iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
		index->indexParams = lappend(index->indexParams, iparam);
	}

	return index;
}

/*
 * transformFKConstraints
 *		handle FOREIGN KEY constraints
 */
static void
transformFKConstraints(ParseState *pstate, CreateStmtContext *cxt,
					   bool skipValidation, bool isAddConstraint)
{
	ListCell   *fkclist;

	if (cxt->fkconstraints == NIL)
		return;

	/*
	 * If CREATE TABLE or adding a column with NULL default, we can safely
	 * skip validation of the constraint.
	 */
	if (skipValidation)
	{
		foreach(fkclist, cxt->fkconstraints)
		{
			FkConstraint *fkconstraint = (FkConstraint *) lfirst(fkclist);

			fkconstraint->skip_validation = true;
		}
	}

	/*
	 * For CREATE TABLE or ALTER TABLE ADD COLUMN, gin up an ALTER TABLE ADD
	 * CONSTRAINT command to execute after the basic command is complete. (If
	 * called from ADD CONSTRAINT, that routine will add the FK constraints to
	 * its own subcommand list.)
	 *
	 * Note: the ADD CONSTRAINT command must also execute after any index
	 * creation commands.  Thus, this should run after
	 * transformIndexConstraints, so that the CREATE INDEX commands are
	 * already in cxt->alist.
	 */
	if (!isAddConstraint)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = cxt->relation;
		alterstmt->cmds = NIL;
		alterstmt->relkind = OBJECT_TABLE;

		foreach(fkclist, cxt->fkconstraints)
		{
			FkConstraint *fkconstraint = (FkConstraint *) lfirst(fkclist);
			AlterTableCmd *altercmd = makeNode(AlterTableCmd);

			altercmd->subtype = AT_ProcessedConstraint;
			altercmd->name = NULL;
			altercmd->def = (Node *) fkconstraint;
			alterstmt->cmds = lappend(alterstmt->cmds, altercmd);
		}

		cxt->alist = lappend(cxt->alist, alterstmt);
	}
}

/*
 * transformIndexStmt - parse analysis for CREATE INDEX
 *
 * Note: this is a no-op for an index not using either index expressions or
 * a predicate expression.	There are several code paths that create indexes
 * without bothering to call this, because they know they don't have any
 * such expressions to deal with.
 */
IndexStmt *
transformIndexStmt(IndexStmt *stmt, const char *queryString)
{
	Relation	rel;
	ParseState *pstate;
	RangeTblEntry *rte;
	ListCell   *l;

	/*
	 * We must not scribble on the passed-in IndexStmt, so copy it.  (This is
	 * overkill, but easy.)
	 */
	stmt = (IndexStmt *) copyObject(stmt);

	/*
	 * Open the parent table with appropriate locking.	We must do this
	 * because addRangeTableEntry() would acquire only AccessShareLock,
	 * leaving DefineIndex() needing to do a lock upgrade with consequent risk
	 * of deadlock.  Make sure this stays in sync with the type of lock
	 * DefineIndex() wants.
	 */
	rel = heap_openrv(stmt->relation,
				  (stmt->concurrent ? ShareUpdateExclusiveLock : ShareLock));

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Put the parent table into the rtable so that the expressions can refer
	 * to its fields without qualification.
	 */
	rte = addRangeTableEntry(pstate, stmt->relation, NULL, false, true);

	/* no to join list, yes to namespaces */
	addRTEtoQuery(pstate, rte, false, true, true);

	/* take care of the where clause */
	if (stmt->whereClause)
		stmt->whereClause = transformWhereClause(pstate,
												 stmt->whereClause,
												 "WHERE");

	/* take care of any index expressions */
	foreach(l, stmt->indexParams)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(l);

		if (ielem->expr)
		{
			ielem->expr = transformExpr(pstate, ielem->expr);

			/*
			 * We check only that the result type is legitimate; this is for
			 * consistency with what transformWhereClause() checks for the
			 * predicate.  DefineIndex() will make more checks.
			 */
			if (expression_returns_set(ielem->expr))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("index expression cannot return a set")));
		}
	}

	/*
	 * Check that only the base rel is mentioned.
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("index expressions and predicates can refer only to the table being indexed")));

	free_parsestate(pstate);

	/* Close relation, but keep the lock */
	heap_close(rel, NoLock);

	return stmt;
}


/*
 * transformRuleStmt -
 *	  transform a CREATE RULE Statement. The action is a list of parse
 *	  trees which is transformed into a list of query trees, and we also
 *	  transform the WHERE clause if any.
 *
 * actions and whereClause are output parameters that receive the
 * transformed results.
 *
 * Note that we must not scribble on the passed-in RuleStmt, so we do
 * copyObject() on the actions and WHERE clause.
 */
void
transformRuleStmt(RuleStmt *stmt, const char *queryString,
				  List **actions, Node **whereClause)
{
	Relation	rel;
	ParseState *pstate;
	RangeTblEntry *oldrte;
	RangeTblEntry *newrte;

	/*
	 * To avoid deadlock, make sure the first thing we do is grab
	 * AccessExclusiveLock on the target relation.	This will be needed by
	 * DefineQueryRewrite(), and we don't want to grab a lesser lock
	 * beforehand.
	 */
	rel = heap_openrv(stmt->relation, AccessExclusiveLock);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * NOTE: 'OLD' must always have a varno equal to 1 and 'NEW' equal to 2.
	 * Set up their RTEs in the main pstate for use in parsing the rule
	 * qualification.
	 */
	oldrte = addRangeTableEntryForRelation(pstate, rel,
										   makeAlias("*OLD*", NIL),
										   false, false);
	newrte = addRangeTableEntryForRelation(pstate, rel,
										   makeAlias("*NEW*", NIL),
										   false, false);
	/* Must override addRangeTableEntry's default access-check flags */
	oldrte->requiredPerms = 0;
	newrte->requiredPerms = 0;

	/*
	 * They must be in the namespace too for lookup purposes, but only add the
	 * one(s) that are relevant for the current kind of rule.  In an UPDATE
	 * rule, quals must refer to OLD.field or NEW.field to be unambiguous, but
	 * there's no need to be so picky for INSERT & DELETE.  We do not add them
	 * to the joinlist.
	 */
	switch (stmt->event)
	{
		case CMD_SELECT:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			break;
		case CMD_UPDATE:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			addRTEtoQuery(pstate, newrte, false, true, true);
			break;
		case CMD_INSERT:
			addRTEtoQuery(pstate, newrte, false, true, true);
			break;
		case CMD_DELETE:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			break;
		default:
			elog(ERROR, "unrecognized event type: %d",
				 (int) stmt->event);
			break;
	}

	/* take care of the where clause */
	*whereClause = transformWhereClause(pstate,
									  (Node *) copyObject(stmt->whereClause),
										"WHERE");

	if (list_length(pstate->p_rtable) != 2)		/* naughty, naughty... */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("rule WHERE condition cannot contain references to other relations")));

	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
		   errmsg("cannot use aggregate function in rule WHERE condition")));

	if (pstate->p_hasWindFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use window function in rule WHERE condition")));

	/*
	 * 'instead nothing' rules with a qualification need a query rangetable so
	 * the rewrite handler can add the negated rule qualification to the
	 * original query. We create a query with the new command type CMD_NOTHING
	 * here that is treated specially by the rewrite system.
	 */
	if (stmt->actions == NIL)
	{
		Query	   *nothing_qry = makeNode(Query);

		nothing_qry->commandType = CMD_NOTHING;
		nothing_qry->rtable = pstate->p_rtable;
		nothing_qry->jointree = makeFromExpr(NIL, NULL);		/* no join wanted */

		*actions = list_make1(nothing_qry);
	}
	else
	{
		ListCell   *l;
		List	   *newactions = NIL;

		/*
		 * transform each statement, like parse_sub_analyze()
		 */
		foreach(l, stmt->actions)
		{
			Node	   *action = (Node *) lfirst(l);
			ParseState *sub_pstate = make_parsestate(NULL);
			Query	   *sub_qry,
					   *top_subqry;
			bool		has_old,
						has_new;

			/*
			 * Since outer ParseState isn't parent of inner, have to pass down
			 * the query text by hand.
			 */
			sub_pstate->p_sourcetext = queryString;

			/*
			 * Set up OLD/NEW in the rtable for this statement.  The entries
			 * are added only to relnamespace, not varnamespace, because we
			 * don't want them to be referred to by unqualified field names
			 * nor "*" in the rule actions.  We decide later whether to put
			 * them in the joinlist.
			 */
			oldrte = addRangeTableEntryForRelation(sub_pstate, rel,
												   makeAlias("*OLD*", NIL),
												   false, false);
			newrte = addRangeTableEntryForRelation(sub_pstate, rel,
												   makeAlias("*NEW*", NIL),
												   false, false);
			oldrte->requiredPerms = 0;
			newrte->requiredPerms = 0;
			addRTEtoQuery(sub_pstate, oldrte, false, true, false);
			addRTEtoQuery(sub_pstate, newrte, false, true, false);

			/* Transform the rule action statement */
			top_subqry = transformStmt(sub_pstate,
									   (Node *) copyObject(action));

			/*
			 * We cannot support utility-statement actions (eg NOTIFY) with
			 * nonempty rule WHERE conditions, because there's no way to make
			 * the utility action execute conditionally.
			 */
			if (top_subqry->commandType == CMD_UTILITY &&
				*whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("rules with WHERE conditions can only have SELECT, INSERT, UPDATE, or DELETE actions")));

			/*
			 * If the action is INSERT...SELECT, OLD/NEW have been pushed down
			 * into the SELECT, and that's what we need to look at. (Ugly
			 * kluge ... try to fix this when we redesign querytrees.)
			 */
			sub_qry = getInsertSelectQuery(top_subqry, NULL);

			/*
			 * If the sub_qry is a setop, we cannot attach any qualifications
			 * to it, because the planner won't notice them.  This could
			 * perhaps be relaxed someday, but for now, we may as well reject
			 * such a rule immediately.
			 */
			if (sub_qry->setOperations != NULL && *whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			/*
			 * Validate action's use of OLD/NEW, qual too
			 */
			has_old =
				rangeTableEntry_used((Node *) sub_qry, PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_OLD_VARNO, 0);
			has_new =
				rangeTableEntry_used((Node *) sub_qry, PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_NEW_VARNO, 0);

			switch (stmt->event)
			{
				case CMD_SELECT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use OLD")));
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use NEW")));
					break;
				case CMD_UPDATE:
					/* both are OK */
					break;
				case CMD_INSERT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON INSERT rule cannot use OLD")));
					break;
				case CMD_DELETE:
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON DELETE rule cannot use NEW")));
					break;
				default:
					elog(ERROR, "unrecognized event type: %d",
						 (int) stmt->event);
					break;
			}

			/*
			 * For efficiency's sake, add OLD to the rule action's jointree
			 * only if it was actually referenced in the statement or qual.
			 *
			 * For INSERT, NEW is not really a relation (only a reference to
			 * the to-be-inserted tuple) and should never be added to the
			 * jointree.
			 *
			 * For UPDATE, we treat NEW as being another kind of reference to
			 * OLD, because it represents references to *transformed* tuples
			 * of the existing relation.  It would be wrong to enter NEW
			 * separately in the jointree, since that would cause a double
			 * join of the updated relation.  It's also wrong to fail to make
			 * a jointree entry if only NEW and not OLD is mentioned.
			 */
			if (has_old || (has_new && stmt->event == CMD_UPDATE))
			{
				/*
				 * If sub_qry is a setop, manipulating its jointree will do no
				 * good at all, because the jointree is dummy. (This should be
				 * a can't-happen case because of prior tests.)
				 */
				if (sub_qry->setOperations != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
				/* hack so we can use addRTEtoQuery() */
				sub_pstate->p_rtable = sub_qry->rtable;
				sub_pstate->p_joinlist = sub_qry->jointree->fromlist;
				addRTEtoQuery(sub_pstate, oldrte, true, false, false);
				sub_qry->jointree->fromlist = sub_pstate->p_joinlist;
			}

			newactions = lappend(newactions, top_subqry);

			free_parsestate(sub_pstate);
		}

		*actions = newactions;
	}

	free_parsestate(pstate);

	/* Close relation, but keep the exclusive lock */
	heap_close(rel, NoLock);
}


/*
 * transformAlterTableStmt -
 *		parse analysis for ALTER TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed AlterTableStmt, but there may be additional actions
 * to be done before and after the actual AlterTable() call.
 */
List *
transformAlterTableStmt(AlterTableStmt *stmt, const char *queryString)
{
	Relation	rel;
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *lcmd,
			   *l;
	List	   *newcmds = NIL;
	bool		skipValidation = true;
	AlterTableCmd *newcmd;

	/*
	 * We must not scribble on the passed-in AlterTableStmt, so copy it. (This
	 * is overkill, but easy.)
	 */
	stmt = (AlterTableStmt *) copyObject(stmt);

	/*
	 * Acquire exclusive lock on the target relation, which will be held until
	 * end of transaction.	This ensures any decisions we make here based on
	 * the state of the relation will still be good at execution. We must get
	 * exclusive lock now because execution will; taking a lower grade lock
	 * now and trying to upgrade later risks deadlock.
	 */
	rel = relation_openrv(stmt->relation, AccessExclusiveLock);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	cxt.stmtType = "ALTER TABLE";
	cxt.relation = stmt->relation;
	cxt.rel = rel;
	cxt.inhRelations = NIL;
	cxt.isalter = true;
	cxt.hasoids = false;		/* need not be right */
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.inh_indexes = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;

	/*
	 * The only subtypes that currently require parse transformation handling
	 * are ADD COLUMN and ADD CONSTRAINT.  These largely re-use code from
	 * CREATE TABLE.
	 */
	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		switch (cmd->subtype)
		{
			case AT_AddColumn:
				{
					ColumnDef  *def = (ColumnDef *) cmd->def;

					Assert(IsA(cmd->def, ColumnDef));
					
					/*
					 * Disallow adding a column with primary key constraint
					 */
					if (Gp_role == GP_ROLE_DISPATCH)
					{
						ListCell *c;
						foreach(c, def->constraints)
						{
							Constraint	   *cons = (Constraint *) lfirst(c);
							if(cons->contype == CONSTR_PRIMARY)
								elog(ERROR, "Cannot add column with primary "
									 "key constraint");
							if(cons->contype == CONSTR_UNIQUE)
								elog(ERROR, "Cannot add column with unique "
									 "constraint");
							
						}
					}
					
					transformColumnDefinition(pstate, &cxt,
											  (ColumnDef *) cmd->def);

					/*
					 * If the column has a non-null default, we can't skip
					 * validation of foreign keys.
					 */
					if (((ColumnDef *) cmd->def)->raw_default != NULL)
						skipValidation = false;

					newcmds = lappend(newcmds, cmd);

					/*
					 * Convert an ADD COLUMN ... NOT NULL constraint to a
					 * separate command
					 */
					if (def->is_not_null)
					{
						/* Remove NOT NULL from AddColumn */
						def->is_not_null = false;

						/* Add as a separate AlterTableCmd */
						newcmd = makeNode(AlterTableCmd);
						newcmd->subtype = AT_SetNotNull;
						newcmd->name = pstrdup(def->colname);
						newcmds = lappend(newcmds, newcmd);
					}

					/*
					 * All constraints are processed in other ways. Remove the
					 * original list
					 */
					def->constraints = NIL;

					break;
				}
			case AT_AddConstraint:

				/*
				 * The original AddConstraint cmd node doesn't go to newcmds
				 */

				if (IsA(cmd->def, Constraint))
					transformTableConstraint(pstate, &cxt,
											 (Constraint *) cmd->def);
				else if (IsA(cmd->def, FkConstraint))
				{
					cxt.fkconstraints = lappend(cxt.fkconstraints, cmd->def);
					skipValidation = false;
				}
				else
					elog(ERROR, "unrecognized node type: %d",
						 (int) nodeTag(cmd->def));
				break;

			case AT_ProcessedConstraint:

				/*
				 * Already-transformed ADD CONSTRAINT, so just make it look
				 * like the standard case.
				 */
				cmd->subtype = AT_AddConstraint;
				newcmds = lappend(newcmds, cmd);
				break;

			default:
				newcmds = lappend(newcmds, cmd);
				break;
		}
	}

	/*
	 * transformIndexConstraints wants cxt.alist to contain only index
	 * statements, so transfer anything we already have into save_alist
	 * immediately.
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	/* Postprocess index and FK constraints */
	transformIndexConstraints(pstate, &cxt);

	transformFKConstraints(pstate, &cxt, skipValidation, true);

	/*
	 * Push any index-creation commands into the ALTER, so that they can be
	 * scheduled nicely by tablecmds.c.  Note that tablecmds.c assumes that
	 * the IndexStmt attached to an AT_AddIndex subcommand has already been
	 * through transformIndexStmt.
	 */
	foreach(l, cxt.alist)
	{
		Node	   *idxstmt = (Node *) lfirst(l);

		Assert(IsA(idxstmt, IndexStmt));
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddIndex;
		newcmd->def = (Node *) transformIndexStmt((IndexStmt *) idxstmt,
												  queryString);
		newcmds = lappend(newcmds, newcmd);
	}
	cxt.alist = NIL;

	/* Append any CHECK or FK constraints to the commands list */
	foreach(l, cxt.ckconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}
	foreach(l, cxt.fkconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}

	/* Close rel but keep lock */
	relation_close(rel, NoLock);

	/*
	 * Output results.
	 */
	stmt->cmds = newcmds;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}


/*
 * Preprocess a list of column constraint clauses
 * to attach constraint attributes to their primary constraint nodes
 * and detect inconsistent/misplaced constraint attributes.
 *
 * NOTE: currently, attributes are only supported for FOREIGN KEY primary
 * constraints, but someday they ought to be supported for other constraints.
 */
static void
transformConstraintAttrs(List *constraintList)
{
	Node	   *lastprimarynode = NULL;
	bool		saw_deferrability = false;
	bool		saw_initially = false;
	ListCell   *clist;

	foreach(clist, constraintList)
	{
		Node	   *node = lfirst(clist);

		if (!IsA(node, Constraint))
		{
			lastprimarynode = node;
			/* reset flags for new primary node */
			saw_deferrability = false;
			saw_initially = false;
		}
		else
		{
			Constraint *con = (Constraint *) node;

			switch (con->contype)
			{
				case CONSTR_ATTR_DEFERRABLE:
					if (lastprimarynode == NULL ||
						!IsA(lastprimarynode, FkConstraint))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("misplaced DEFERRABLE clause")));
					if (saw_deferrability)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed")));
					saw_deferrability = true;
					((FkConstraint *) lastprimarynode)->deferrable = true;
					break;
				case CONSTR_ATTR_NOT_DEFERRABLE:
					if (lastprimarynode == NULL ||
						!IsA(lastprimarynode, FkConstraint))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("misplaced NOT DEFERRABLE clause")));
					if (saw_deferrability)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed")));
					saw_deferrability = true;
					((FkConstraint *) lastprimarynode)->deferrable = false;
					if (saw_initially &&
						((FkConstraint *) lastprimarynode)->initdeferred)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE")));
					break;
				case CONSTR_ATTR_DEFERRED:
					if (lastprimarynode == NULL ||
						!IsA(lastprimarynode, FkConstraint))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY DEFERRED clause")));
					if (saw_initially)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed")));
					saw_initially = true;
					((FkConstraint *) lastprimarynode)->initdeferred = true;

					/*
					 * If only INITIALLY DEFERRED appears, assume DEFERRABLE
					 */
					if (!saw_deferrability)
						((FkConstraint *) lastprimarynode)->deferrable = true;
					else if (!((FkConstraint *) lastprimarynode)->deferrable)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE")));
					break;
				case CONSTR_ATTR_IMMEDIATE:
					if (lastprimarynode == NULL ||
						!IsA(lastprimarynode, FkConstraint))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("misplaced INITIALLY IMMEDIATE clause")));
					if (saw_initially)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed")));
					saw_initially = true;
					((FkConstraint *) lastprimarynode)->initdeferred = false;
					break;
				default:
					/* Otherwise it's not an attribute */
					lastprimarynode = node;
					/* reset flags for new primary node */
					saw_deferrability = false;
					saw_initially = false;
					break;
			}
		}
	}
}

/*
 * Special handling of type definition for a column
 */
static void
transformColumnType(ParseState *pstate, ColumnDef *column)
{
	/*
	 * All we really need to do here is verify that the type is valid.
	 */
	Type		ctype = typenameType(pstate, column->typname, NULL);

	ReleaseSysCache(ctype);
}


/*
 * transformCreateSchemaStmt -
 *	  analyzes the CREATE SCHEMA statement
 *
 * Split the schema element list into individual commands and place
 * them in the result list in an order such that there are no forward
 * references (e.g. GRANT to a table created later in the list). Note
 * that the logic we use for determining forward references is
 * presently quite incomplete.
 *
 * SQL92 also allows constraints to make forward references, so thumb through
 * the table columns and move forward references to a posterior alter-table
 * command.
 *
 * The result is a list of parse nodes that still need to be analyzed ---
 * but we can't analyze the later commands until we've executed the earlier
 * ones, because of possible inter-object references.
 *
 * Note: this breaks the rules a little bit by modifying schema-name fields
 * within passed-in structs.  However, the transformation would be the same
 * if done over, so it should be all right to scribble on the input to this
 * extent.
 */
List *
transformCreateSchemaStmt(CreateSchemaStmt *stmt)
{
	CreateSchemaStmtContext cxt;
	List	   *result;
	ListCell   *elements;

	cxt.stmtType = "CREATE SCHEMA";
	cxt.schemaname = stmt->schemaname;
	cxt.authid = stmt->authid;
	cxt.sequences = NIL;
	cxt.tables = NIL;
	cxt.views = NIL;
	cxt.indexes = NIL;
	cxt.triggers = NIL;
	cxt.grants = NIL;

	/*
	 * Run through each schema element in the schema element list. Separate
	 * statements by type, and do preliminary analysis.
	 */
	foreach(elements, stmt->schemaElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_CreateSeqStmt:
				{
					CreateSeqStmt *elp = (CreateSeqStmt *) element;

					setSchemaName(cxt.schemaname, &elp->sequence->schemaname);
					cxt.sequences = lappend(cxt.sequences, element);
				}
				break;

			case T_CreateStmt:
				{
					CreateStmt *elp = (CreateStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);

					/*
					 * XXX todo: deal with constraints
					 */
					cxt.tables = lappend(cxt.tables, element);
				}
				break;

			case T_ViewStmt:
				{
					ViewStmt   *elp = (ViewStmt *) element;

					setSchemaName(cxt.schemaname, &elp->view->schemaname);

					/*
					 * XXX todo: deal with references between views
					 */
					cxt.views = lappend(cxt.views, element);
				}
				break;

			case T_IndexStmt:
				{
					IndexStmt  *elp = (IndexStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.indexes = lappend(cxt.indexes, element);
				}
				break;

			case T_CreateTrigStmt:
				{
					CreateTrigStmt *elp = (CreateTrigStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.triggers = lappend(cxt.triggers, element);
				}
				break;

			case T_GrantStmt:
				cxt.grants = lappend(cxt.grants, element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
		}
	}

	result = NIL;
	result = list_concat(result, cxt.sequences);
	result = list_concat(result, cxt.tables);
	result = list_concat(result, cxt.views);
	result = list_concat(result, cxt.indexes);
	result = list_concat(result, cxt.triggers);
	result = list_concat(result, cxt.grants);

	return result;
}

/*
 * setSchemaName
 *		Set or check schema name in an element of a CREATE SCHEMA command
 */
static void
setSchemaName(char *context_schema, char **stmt_schema_name)
{
	if (*stmt_schema_name == NULL)
		*stmt_schema_name = context_schema;
	else if (strcmp(context_schema, *stmt_schema_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_SCHEMA_DEFINITION),
				 errmsg("CREATE specifies a schema (%s) "
						"different from the one being created (%s)",
						*stmt_schema_name, context_schema)));
}


/****************stmt->policy*********************/
static void
transformDistributedBy(ParseState *pstate, CreateStmtContext *cxt, List *distributedBy, GpPolicy ** policyp, List *likeDistributedBy)
{


	ListCell   *keys = NULL;

	GpPolicy  *policy = NULL;
	int			colindex = 0;
	int			maxattrs = 200;
	int			numUniqueIndexes = 0;
	Constraint *uniqueindex = NULL;

	/*
	 * utility mode creates can't have a policy.  Only the QD can have policies
	 *
	 */
	if (Gp_role != GP_ROLE_DISPATCH)
	{
		*policyp = NULL;
		return;
	}

 
 


		policy = (GpPolicy *) palloc(sizeof(GpPolicy) + maxattrs * sizeof(policy->attrs[0]));
		policy->ptype = POLICYTYPE_PARTITIONED;
		policy->nattrs = 0;
		policy->attrs[0] = 1;
	
		/*
		 * If distributedBy is NIL, the user did not explicitly say what he wanted for
		 * a distribution policy.  So, we need to assign one.
		 */
		 
		
	
		if (distributedBy == NIL && cxt && cxt->pkey != NULL)
		{
			/*
			 * We have a PRIMARY KEY, so let's assign the default distribution to be the key
			 */
	
			IndexStmt  *index = cxt->pkey;
			List	   *indexParams;
			ListCell   *ip = NULL;
	
			Assert(index->indexParams != NULL);
			indexParams = index->indexParams;
	
			foreach(ip, indexParams)
			{
				IndexElem  *iparam = lfirst(ip);
	
				if (iparam && iparam->name != 0)
				{
	
					if (distributedBy)
						distributedBy = lappend(distributedBy, (Node *) makeString(iparam->name));
					else
						distributedBy = list_make1((Node *) makeString(iparam->name));
	
				}
			}
	
		}
	
	
		if (cxt && cxt->ixconstraints != NULL)
		{
	
			ListCell   *lc = NULL;
	
			foreach(lc, cxt->ixconstraints)
			{
				Constraint *cons = lfirst(lc);
	
				if (cons->contype == CONSTR_UNIQUE)
				{

					if (uniqueindex == NULL)
						uniqueindex = cons;
					numUniqueIndexes++;
					if (cxt->pkey)
					{
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("Greenplum Database does not allow having both PRIMARY KEY and UNIQUE contraints")));
					}
				}
			}
			if (numUniqueIndexes > 1)
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("Greenplum Database does not allow having multiple UNIQUE contraints")));
			}
		}
	
		if (distributedBy == NIL && cxt && cxt->ixconstraints != NULL && numUniqueIndexes > 0)
		{
			/*
			 * No explicit distributed by clause, and no primary key.
			 * If there is a UNIQUE clause, let's use that
			 */
	
			ListCell   *lc = NULL;
	
			foreach(lc, cxt->ixconstraints)
			{
	
				Constraint *constraint = lfirst(lc);
	
				if (constraint->contype == CONSTR_UNIQUE)
				{
	
					ListCell   *ip = NULL;
	
	
					foreach(ip, constraint->keys)
					{
						Value	   *v = lfirst(ip);
	
						if (v && v->val.str != 0)
						{
	
							if (distributedBy)
								distributedBy = lappend(distributedBy, (Node *) makeString(v->val.str));
							else
								distributedBy = list_make1((Node *) makeString(v->val.str));
	
						}
					}
				}
			}
			
		}
		
		/*
		 * If new table INHERITS from one or more parent tables, check parents.
		 */
		if (cxt->inhRelations != NIL)
		{
			ListCell   *entry;
			
			foreach(entry, cxt->inhRelations)
			{
				RangeVar   *parent = (RangeVar *) lfirst(entry);
				Oid			relId = RangeVarGetRelid(parent, false);
				GpPolicy  *oldTablePolicy = GpPolicyFetch(CurrentMemoryContext, relId);

                /* Partitioned child must have partitioned parents. */
                if (oldTablePolicy == NULL ||
                     oldTablePolicy->ptype != POLICYTYPE_PARTITIONED)
                    ereport(ERROR, (errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
                                    errmsg("Cannot inherit from catalog table \"%s\" to create table \"%s\".",
                                           parent->relname, cxt->relation->relname),
                                    errdetail("An inheritance hierarchy cannot contain a "
                                              "mixture of distributed and non-distributed tables.")
                            ));
		        /*
		         * if we still don't know what distribution to use, and this is an inherited table,
		         * set the distribution based on the parent (or one of the parents)
		         */
				if (distributedBy == NIL &&
                    oldTablePolicy->nattrs >= 0)
				{
					int ia;
					
					if (oldTablePolicy->nattrs > 0)
					{
						for (ia=0; ia<oldTablePolicy->nattrs; ia++)
						{
							char * attname = get_attname(relId, oldTablePolicy->attrs[ia]);

							distributedBy = lappend(distributedBy, (Node *) makeString(attname));
						}
					}
					else
					{
						/* strewn parent */
						distributedBy = lappend(distributedBy, (Node *)NULL);
					}
					elog(NOTICE, "Table has parent, setting distribution columns to match parent table");
				}

                pfree(oldTablePolicy);
			}
		}
		
		if (distributedBy == NIL && likeDistributedBy != NIL)
		{
			distributedBy = likeDistributedBy;
			elog(NOTICE, "Table doesn't have 'distributed by' clause, defaulting to distribution columns from LIKE table");
		}
	
		if (distributedBy == NIL)
		{
			/*
			 * if we get here, we haven't a clue what to use for the distribution columns.
			 */
	
			bool	assignedDefault = false;

			/*
			 * table has one or more attributes and there is still no distribution
			 * key. pick a default one. the winner is the first attribute that is
			 * an Greenplum Database-hashable data type.
			 */
			
			ListCell   *columns;
			ColumnDef  *column = NULL;
			
			/* If we have no attributes, make sure that we don't create a bogus policy! */
			if (cxt && list_length(cxt->columns) == 0 && !cxt->inhRelations)
			{
				elog(NOTICE, "Table has no attributes to distribute on.");
				pfree(policy);
				return;
			}

			/* we will distribute on at most one column */
			policy->nattrs = 1;
			
			colindex = 0;
			
			if (cxt->inhRelations)
			{
				bool		found = false;
				/* try inherited tables */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = (RangeVar *) lfirst(inher);
					Relation	rel;
					int			count;

					Assert(IsA(inh, RangeVar));
					rel = heap_openrv(inh, AccessShareLock);
					if (rel->rd_rel->relkind != RELKIND_RELATION)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						   errmsg("inherited relation \"%s\" is not a table",
								  inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = rel->rd_att->attrs[count];
						Oid typeOid = inhattr->atttypid;
						bool isArray = inhattr->attndims > 0;
												
						if (inhattr->attisdropped)
							continue;
						colindex++;
						if(isGreenplumDbHashable(typeOid) || isArray)
						{
							char	   *inhname = NameStr(inhattr->attname);
							policy->attrs[0] = colindex;
							assignedDefault = true;
							ereport(NOTICE,
									(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
									 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column " 
										    "named '%s' from parent table as the Greenplum Database data distribution key for this "
										    "table. ", inhname),
								     errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
								     		 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
							found = true;
							break;
						}			
					}
					heap_close(rel, NoLock);

					if (assignedDefault)
						break;
				}

			}
				 
				
				
			 
			if (!assignedDefault)
				foreach(columns, cxt->columns)
				{
					Oid typeOid;
					bool isArray;
	
					column = (ColumnDef *) lfirst(columns);
					colindex++;
	
					typeOid = typenameTypeId(NULL, column->typname, NULL);
					isArray = (column->typname->arrayBounds == NIL ? false : true);
						
					/*
					 * if we can hash on this type, or if it's an array type (which
					 * we can always hash) this column with be our default key.
					 */
					if(isGreenplumDbHashable(typeOid) || isArray)
					{
						policy->attrs[0] = colindex;
						assignedDefault = true;
						ereport(NOTICE,
								(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
								 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column " 
									    "named '%s' as the Greenplum Database data distribution key for this "
									    "table. ", column->colname),
							     errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
							     		 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
						break;
					}			
					
				}
				
			if(!assignedDefault)
			{
				/*
				 * There was no eligible distribution column to default to. This table
				 * will be partitioned on an empty distribution key list. In other words
				 * tuples coming into the system will be randomly assigned a bucket.
				 */
				policy->nattrs = 0;
				elog(NOTICE, "Table doesn't have 'distributed by' clause, and no column type is suitable for a distribution key. Creating a NULL policy entry.");
			}
			
		}
		else 
		{
			/*
			 * We have a DISTRIBUTED BY column list, either specified by the user
			 * or defaulted to a primary key or unique column. Process it now and
			 * set the distribution policy.
			 */
			policy->nattrs = 0;
			if (distributedBy->length == 1 && (distributedBy->head == NULL || distributedBy->head->data.ptr_value == NULL))
			{ 
				if (cxt && cxt->pkey)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("PRIMARY KEY and DISTRIBUTED RANDOMLY are incompatible"
									)));
				else if (uniqueindex)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("UNIQUE and DISTRIBUTED RANDOMLY are incompatible"
									)));
			}
			else
			foreach(keys, distributedBy)
			{
				char	   *key = strVal(lfirst(keys));
				bool		found = false;
				ColumnDef  *column = NULL;
				ListCell   *columns;
	
				colindex = 0;
				
				if (cxt->inhRelations)
				{
					/* try inherited tables */
					ListCell   *inher;
	
					foreach(inher, cxt->inhRelations)
					{
						RangeVar   *inh = (RangeVar *) lfirst(inher);
						Relation	rel;
						int			count;
	
						Assert(IsA(inh, RangeVar));
						rel = heap_openrv(inh, AccessShareLock);
						if (rel->rd_rel->relkind != RELKIND_RELATION)
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							   errmsg("inherited relation \"%s\" is not a table",
									  inh->relname)));
						for (count = 0; count < rel->rd_att->natts; count++)
						{
							Form_pg_attribute inhattr = rel->rd_att->attrs[count];
							char	   *inhname = NameStr(inhattr->attname);
	
							if (inhattr->attisdropped)
								continue;
							colindex++;
							if (strcmp(key, inhname) == 0)
							{
								found = true;
	
								break;
							}
						}
						heap_close(rel, NoLock);
						if (found)
							elog(DEBUG1, "'distributed by' clause refers to columns of inherited table");
	
						if (found)
							break;
	
	
					}
				}
				
				if (!found)
				foreach(columns, cxt->columns)
				{
					column = (ColumnDef *) lfirst(columns);
					Assert(IsA(column, ColumnDef));
					colindex++;

					if (strcmp(column->colname, key) == 0)
					{
						Oid typeOid = typenameTypeId(NULL, column->typname, NULL);
						bool isArray = (column->typname->arrayBounds == NIL ? false : true);
												
						/*
						 * To be a part of a distribution key, this type must be supported
						 * for hashing internally in Greenplum Database. We check if the base type is
						 * supported for hashing or if it is an array type (we support
						 * hashing on all array types).
						 */
						if(!isGreenplumDbHashable(typeOid) && !isArray)
						{
							ereport(ERROR,
									(errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
									 errmsg("type \"%s\" can't be a part of a distribution key",
											strVal(linitial(column->typname->names)))));						
						}
						
						found = true;
						break;
					}
				}
	
				if (found)
				{
					/* good, the columns exists */
				}
	#if 0
				else if (SystemAttributeByName(key, cxt->hasoids) != NULL)
				{
					/*
					* column will be a system column in the new table, so
					* accept it.
					*/
					found = true;
				}
	#endif
				
	
				/*
				* In the ALTER TABLE case, don't complain about index keys
				* not created in the command; they may well exist already.
				* DefineIndex will complain about them if not, and will also
				* take care of marking them NOT NULL.
				*/
				if (!found && !cxt->isalter)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" named in 'DISTRIBUTED BY' clause does not exist",
									key)));
	
				policy->attrs[policy->nattrs++] = colindex;
	
	
				/* we should check for duplicate column usage */
			}
	
		}
	
	
		*policyp = policy;	
 
	
	if (cxt && cxt->pkey)		/* Primary key	specified.	Make sure
								 * distribution columns match */
	{
		int			i = 0;
		IndexStmt  *index = cxt->pkey;
		List	   *indexParams = index->indexParams;
		ListCell   *ip;

		foreach(ip, indexParams)
		{
			IndexElem  *iparam;

			if (i >= policy->nattrs)
				break;

			iparam = lfirst(ip);
			if (iparam->name != 0)
			{
				bool	found = false;
				ColumnDef  *column = NULL;
				ListCell   *columns;

				colindex = 0;
				
				if (cxt->inhRelations)
				{
					/* try inherited tables */
					ListCell   *inher;
	
					foreach(inher, cxt->inhRelations)
					{
						RangeVar   *inh = (RangeVar *) lfirst(inher);
						Relation	rel;
						int			count;
	
						Assert(IsA(inh, RangeVar));
						rel = heap_openrv(inh, AccessShareLock);
						if (rel->rd_rel->relkind != RELKIND_RELATION)
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							   errmsg("inherited relation \"%s\" is not a table",
									  inh->relname)));
						for (count = 0; count < rel->rd_att->natts; count++)
						{
							Form_pg_attribute inhattr = rel->rd_att->attrs[count];
							char	   *inhname = NameStr(inhattr->attname);
	
							if (inhattr->attisdropped)
								continue;
							colindex++;
	
							if (strcmp(iparam->name, inhname) == 0)
							{
								found = true;
	
								break;
							}
						}
						heap_close(rel, NoLock);
						if (found)
							elog(DEBUG1, "'distributed by' clause refers to columns of inherited table");
	
						if (found)
							break;
	
	
					}
				}
				
				if (!found)
				foreach(columns, cxt->columns)
				{
					column = (ColumnDef *) lfirst(columns);
					Assert(IsA(column, ColumnDef));
					colindex++;
					if (strcmp(column->colname, iparam->name) == 0)
					{
						found = true;
						break;
					}
				}

				if (colindex != policy->attrs[i])
				{

					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("When there is both a PRIMARY KEY, and a DISTRIBUTED BY clause, the DISTRIBUTED BY clause must be equal to or a left-subset of the PRIMARY KEY"
									)));

				}

				i++;

			}
		}
	}



	if (uniqueindex)			/* UNIQUE specified.  Make sure distribution
								 * columns match */
	{
		int			i = 0;

		List	   *keys = uniqueindex->keys;
		ListCell   *ip;

		foreach(ip, keys)
		{
			IndexElem  *iparam;

			if (i >= policy->nattrs)
				break;

			iparam = lfirst(ip);
			if (iparam->name != 0)
			{
				bool	found = false;
				ColumnDef  *column = NULL;
				ListCell   *columns;

				colindex = 0;
				
				if (cxt->inhRelations)
				{
					/* try inherited tables */
					ListCell   *inher;
	
					foreach(inher, cxt->inhRelations)
					{
						RangeVar   *inh = (RangeVar *) lfirst(inher);
						Relation	rel;
						int			count;
	
						Assert(IsA(inh, RangeVar));
						rel = heap_openrv(inh, AccessShareLock);
						if (rel->rd_rel->relkind != RELKIND_RELATION)
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							   errmsg("inherited relation \"%s\" is not a table",
									  inh->relname)));
						for (count = 0; count < rel->rd_att->natts; count++)
						{
							Form_pg_attribute inhattr = rel->rd_att->attrs[count];
							char	   *inhname = NameStr(inhattr->attname);
	
							if (inhattr->attisdropped)
								continue;
							colindex++;
	
							if (strcmp(iparam->name, inhname) == 0)
							{
								found = true;
	
								break;
							}
						}
						heap_close(rel, NoLock);
						if (found)
							elog(NOTICE, "'distributed by' clause refers to columns of inherited table");
	
						if (found)
							break;
	
	
					}
				}
				
				if (!found)
				foreach(columns, cxt->columns)
				{
					column = (ColumnDef *) lfirst(columns);
					Assert(IsA(column, ColumnDef));
					colindex++;
					if (strcmp(column->colname, iparam->name) == 0)
					{
						found = true;
						break;
					}
				}

				if (colindex != policy->attrs[i])
				{

					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("When there is both a UNIQUE contraint, and a DISTRIBUTED BY clause, the DISTRIBUTED BY clause must be equal to or a left-subset of the UNIQUE columns"
									)));
				}

				i++;

			}
		}
	}
	


}

static A_Const *make_a_const(A_Const *pValConst, char *val_str)
{
	A_Const *pNewValConst = makeNode(A_Const);
	int estat = 0;

	Assert(val_str);

	pNewValConst->val.type = pValConst->val.type;
	pNewValConst->typname = pValConst->typname;
	
	switch (pValConst->val.type)
	{
		case T_Integer:
			estat = sscanf(val_str, "%ld", 
						   &(pNewValConst->val.val.ival));
			break;
		case T_Float:
/*			estat = scanf(val_str, "%f", 
			(pNewValConst->val.val.str)); */

		case T_String:
			pNewValConst->val.val.str = val_str;

			estat = 1;
			break;

		default:

			break;
	}

	if (estat < 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("unknown type or operator: %s",
						val_str)));
	}
	
	return (pNewValConst);
} /* end make_a_const */

static int 
deparse_partition_rule(Node *pNode, char *outbuf, size_t outsize)
{
	if (!pNode)
		return 0;

	switch (nodeTag(pNode))
	{
		case T_Value:

				/* XXX XXX XXX ??? */

			break;
		case T_ColumnRef:
		{
			ColumnRef		*pCRef = (ColumnRef *)pNode;
			List			*coldefs = pCRef->fields;
			ListCell		*lc      = NULL;
			StringInfoData   sid;
			int colcnt = 0;

			initStringInfo(&sid);

			lc = list_head(coldefs);

			for (; lc; lc = lnext(lc)) /* for all cols */
			{
				Node *pCol = lfirst(lc);
				char	 leftBuf[10000];			

				if (!deparse_partition_rule(pCol, leftBuf, sizeof(leftBuf)))
						return 0;

				if (colcnt)
				{
					appendStringInfo(&sid, ".");
				}

				appendStringInfo(&sid, "%s", leftBuf);

				colcnt++;
			} /* end for all cols */

			outbuf[0] = '\0';
			snprintf(outbuf, outsize, "%s", sid.data);
			pfree(sid.data);

			break;
		}
		case T_String:
				snprintf(outbuf, outsize, "%s", 
						 strVal(pNode));
			break;
		case T_Integer:
				snprintf(outbuf, outsize, "%ld", 
						 intVal(pNode));
			break;
		case T_Float:
				snprintf(outbuf, outsize, "%f", 
						 floatVal(pNode));
			break;
		case T_A_Const:
		{
			A_Const *acs = (A_Const *)pNode;

			if (acs->val.type == T_String)
			{
				if (acs->typname) /* deal with explicit types */
				{
					/* XXX XXX: simple types only -- need to 
					 * handle Interval, etc */

					snprintf(outbuf, outsize, "\'%s\'::%s", 
							 acs->val.val.str, 
							 TypeNameToString(acs->typname));
				}
				else
				{
					snprintf(outbuf, outsize, "\'%s\'", 
							 acs->val.val.str);
				}

				return 1;
			}
			return (deparse_partition_rule((Node *)&(acs->val), 
										   outbuf, outsize));
		}
			break;
		case T_A_Expr:
		{
			A_Expr	*ax = (A_Expr *)pNode;
			char	 leftBuf[10000];			
			char	 rightBuf[10000];			
			char    *infix_op;
			
			switch (ax->kind)
			{
				case AEXPR_OP:			/* normal operator */
				case AEXPR_AND:			/* booleans - name field is unused */
				case AEXPR_OR:
					break;
				default:
					return 0;
			}
			if (!deparse_partition_rule(ax->lexpr, leftBuf, sizeof(leftBuf)))
				return 0;
			if (!deparse_partition_rule(ax->rexpr, rightBuf, sizeof(rightBuf)))
				return 0;
			switch (ax->kind)
			{
				case AEXPR_OP:			/* normal operator */
					infix_op = strVal(lfirst(list_head(ax->name)));
					break;
				case AEXPR_AND:			/* booleans - name field is unused */
					infix_op = "AND";
					break;
				case AEXPR_OR:
					infix_op = "OR";
					break;
				default:
					return 0;
			}
			snprintf(outbuf, outsize, "(%s %s %s)",
					 leftBuf, infix_op, rightBuf);

		}
			break;
		case T_Var:
			{
/*				Var		   *var = (Var *) node; */
			}
		default:
				break;
	}
	return 1;
}

static Node *make_prule_catalog(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
 		 char *at_depth, char *child_name_str, 
		 char *exprBuf,
		 Node *pWhere							 
		 )
{
	Node				*pResult = NULL;
	InsertStmt			*pIns    = NULL;
	RangeVar			*parent_tab_name;
	RangeVar			*child_tab_name;
	char				 ruleBuf[10000];
	char				 newVals[10000];

	{
		List			*coldefs = stmt->tableElts;
		ListCell		*lc      = NULL;
		StringInfoData   sid;
		int colcnt = 0;

		initStringInfo(&sid);

		lc = list_head(coldefs);

		for (; lc; lc = lnext(lc)) /* for all cols */
		{
			Node *pCol = lfirst(lc);
			ColumnDef *pColDef;

			if (nodeTag(pCol) != T_ColumnDef) /* avoid constraints, etc */
				continue;

			pColDef = (ColumnDef *)pCol;

			if (colcnt)
			{
				appendStringInfo(&sid, ", ");
			}

			appendStringInfo(&sid, "new.%s", pColDef->colname);

			colcnt++;
		} /* end for all cols */

		newVals[0] = '\0';
		snprintf(newVals, sizeof(newVals), "VALUES (%s)", sid.data);
		pfree(sid.data);
	}

	parent_tab_name = makeNode(RangeVar);
	parent_tab_name->catalogname = cxt->relation->catalogname;
	parent_tab_name->schemaname  = cxt->relation->schemaname;
	parent_tab_name->relname     = cxt->relation->relname; 

	child_tab_name = makeNode(RangeVar);
	child_tab_name->catalogname = cxt->relation->catalogname;
	child_tab_name->schemaname  = cxt->relation->schemaname;
	child_tab_name->relname     = child_name_str;  

	snprintf(ruleBuf, sizeof(ruleBuf), 
			 "CREATE RULE %s AS ON INSERT to %s WHERE %s DO INSTEAD INSERT INTO %s %s", child_name_str, parent_tab_name->relname, exprBuf, child_name_str, newVals);


	pIns = makeNode(InsertStmt);

	pResult = (Node *)pIns;
	pIns->relation = makeNode(RangeVar);
	pIns->relation->catalogname = NULL;
	pIns->relation->schemaname = NULL;
	pIns->relation->relname = "partition_rule";
	pIns->returningList = NULL;

	pIns->cols = NIL;

	if (1)
	{
		List *vl1 = NULL;

		A_Const *acs = makeNode(A_Const);
		acs->val.type = T_String;
		acs->val.val.str = pstrdup(parent_tab_name->relname);
		acs->typname = SystemTypeName("text");

		vl1 = list_make1(acs);

		acs = makeNode(A_Const);
		acs->val.type = T_String;
		acs->val.val.str = pstrdup(child_name_str);
		acs->typname = SystemTypeName("text");
		
		vl1 = lappend(vl1, acs);

		acs = makeNode(A_Const);
		acs->val.type = T_String;
		acs->val.val.str = pstrdup(exprBuf);
		acs->typname = SystemTypeName("text");

		vl1 = lappend(vl1, acs);

		acs = makeNode(A_Const);
		acs->val.type = T_String;
		acs->val.val.str = pstrdup(ruleBuf);
		acs->typname = SystemTypeName("text");

		vl1 = lappend(vl1, acs);
				
		pIns->selectStmt = (Node *)makeNode(SelectStmt);
		((SelectStmt *)pIns->selectStmt)->valuesLists = 
				list_make1(vl1);

	}

	return (pResult);
} /* end make_prule_catalog */

static Node *make_prule_rulestmt(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
 		 char *at_depth, char *child_name_str, 
		 char *exprBuf,
		 Node *pWhere							 
		 )
{
	Node				*pResult = NULL;
	RuleStmt			*pRule   = NULL;
	InsertStmt			*pIns    = NULL;
	RangeVar			*parent_tab_name;
	RangeVar			*child_tab_name;

	parent_tab_name = makeNode(RangeVar);
	parent_tab_name->catalogname = cxt->relation->catalogname;
	parent_tab_name->schemaname  = cxt->relation->schemaname;
	parent_tab_name->relname     = cxt->relation->relname; 

	child_tab_name = makeNode(RangeVar);
	child_tab_name->catalogname = cxt->relation->catalogname;
	child_tab_name->schemaname  = cxt->relation->schemaname;
	child_tab_name->relname     = child_name_str;  

	pIns = makeNode(InsertStmt);

	pRule = makeNode(RuleStmt);
	pRule->replace = false; /* do not replace */
	pRule->relation = parent_tab_name;
	pRule->rulename = pstrdup(child_name_str);
	pRule->whereClause = pWhere;
	pRule->event = CMD_INSERT;
	pRule->instead = true; /* do instead */
	pRule->actions = list_make1(pIns);

	pResult = (Node *)pRule;

	pIns->relation = makeNode(RangeVar);
	pIns->relation->catalogname = cxt->relation->catalogname;
	pIns->relation->schemaname  = cxt->relation->schemaname;
	pIns->relation->relname     = child_name_str;

	pIns->returningList = NULL;

	pIns->cols = NIL;

	if (1)
	{
		List			*coldefs = stmt->tableElts;
		ListCell		*lc      = NULL;
		List *vl1 = NULL;

		lc = list_head(coldefs);

		for (; lc; lc = lnext(lc)) /* for all cols */
		{
			Node *pCol = lfirst(lc);
			ColumnDef *pColDef;
			ColumnRef *pCRef;

			if (nodeTag(pCol) != T_ColumnDef) /* avoid constraints, etc */
				continue;

			pCRef = makeNode(ColumnRef);

			pColDef = (ColumnDef *)pCol;

			pCRef->location = -1;
			/* NOTE: gram.y uses "*NEW*" for "new" */
			pCRef->fields = list_make2(makeString("*NEW*"),
									   makeString(pColDef->colname));

			vl1 = lappend(vl1, pCRef);
		}
				
		pIns->selectStmt = (Node *)makeNode(SelectStmt);
		((SelectStmt *)pIns->selectStmt)->valuesLists = 
				list_make1(vl1);

	}


	return (pResult);
} /* end make_prule_rulestmt */



static List *make_partition_rules(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
		 char  *at_depth, char *child_name_str, 
		 int partNumId, int maxPartNum, 
		 int everyOffset, int maxEveryOffset,
		 ListCell	**pp_lc_anp
		)
{
	PartitionBy			*pBy    = (PartitionBy *)partitionBy;
	PartitionBoundSpec	*pBSpec = NULL;
	Node				*pRule  = NULL;
	List				*allRules  = NULL;

	if (pBy->partType != PARTTYP_HASH)	
	{
		Assert(pElem);
	}

	if (pElem)
	{
		pBSpec = (PartitionBoundSpec *)pElem->boundSpec;
	}

	if (pBy->partType == PARTTYP_HASH)	
	{
		List			*colElts = pBy->keys;
		ListCell		*lc      = NULL;
		char			 exprBuf[10000];
		StringInfoData   sid;
		int colcnt = 0;
		Node			*pHashArgs = NULL;
		Node			*pExpr = NULL;

		exprBuf[0] = '\0';

		initStringInfo(&sid);

		lc = list_head(colElts);

		for (; lc; lc = lnext(lc)) /* for all cols */
		{
			Node *pCol = lfirst(lc);
			Value *pColConst;
			ColumnRef *pCRef = NULL;

			pColConst = (Value *)pCol;

			Assert(IsA(pColConst, String));

			if (!deparse_partition_rule((Node *)pCol,
										exprBuf, sizeof(exprBuf)))
			{
				int loco = pBy->location;

				if (pBSpec)
					loco = pBSpec->location;

				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("unknown type or operator%s",
								at_depth),
						 parser_errposition(pstate, loco)));
			}

			pCRef = makeNode(ColumnRef);
			pCRef->location = -1;
			pCRef->fields = list_make1(pCol);

			if (colcnt)
			{
				Node *pAppOp = NULL;

				pAppOp =
						(Node *) makeSimpleA_Expr(AEXPR_OP, "||", 
												  pHashArgs,
												  (Node *)pCRef, 
												  -1);
				pHashArgs = pAppOp;

				appendStringInfo(&sid, "||");
			}
			else
			{
				pHashArgs = (Node *)pCRef;
			}

			appendStringInfo(&sid, "%s", exprBuf);

			colcnt++;
		} /* end for all cols */

		/* magic_hash: maximum number of partitions is maxPartNum
		   current partition number is parNumId
		*/

		/* modulus arithmetic is 0 to N-1, so go to (partNumId-1).
		 * Also, hashtext can return a negative, so fix it up */

		snprintf(exprBuf, sizeof(exprBuf), 
/*				 "magic_hash(%d, %s) = %d", */
/* double % for % literal - ((hash(cols)%max + max) % max) */
				 "(hashtext(%s)%%%d + %d)%%%d = %d",
				 sid.data,
				 maxPartNum,
				 maxPartNum,
				 maxPartNum,
				 (partNumId-1)
				);

		pfree(sid.data);

		{
			Node *pPlusOp  = NULL;
			Node *pModOp  = NULL;
			A_Const *pModBy  = NULL;
			A_Const *pPartID = NULL;
			FuncCall *pFC = makeNode(FuncCall);

			pFC->funcname = list_make1(makeString("hashtext"));
			pFC->args = list_make1(pHashArgs);
			pFC->agg_star = FALSE;
			pFC->agg_distinct = FALSE;
			pFC->location = -1;
			pFC->over = NULL;

			pModBy  = makeNode(A_Const);
			pModBy->val.type = T_Integer;
			pModBy->val.val.ival = maxPartNum;

			pPartID  = makeNode(A_Const);
			pPartID->val.type = T_Integer;
			pPartID->val.val.ival = (partNumId - 1);

			pModOp = (Node *)makeSimpleA_Expr(AEXPR_OP, "%", 
									  (Node *)pFC, (Node *)pModBy, -1);

			pPlusOp = (Node *)makeSimpleA_Expr(AEXPR_OP, "+", 
									  (Node *)pModOp, (Node *)pModBy, -1);

			pModOp = (Node *)makeSimpleA_Expr(AEXPR_OP, "%", 
									  (Node *)pPlusOp, (Node *)pModBy, -1);

			pExpr = (Node *)makeSimpleA_Expr(AEXPR_OP, "=", 
									 pModOp, (Node *)pPartID, -1);

		}

		pRule = make_prule_catalog(pstate,
								   cxt, stmt, 
								   partitionBy, pElem,
								   at_depth, child_name_str, 
								   exprBuf,
								   pExpr);

		/* first the INSERT statement, then the CHECK constraint, then
		 * the RULE statement 
		 */
		allRules = list_make1(pRule);
		allRules = lappend(allRules, pExpr); 

		pRule = make_prule_rulestmt(pstate,
									cxt, stmt, 
									partitionBy, pElem,
									at_depth, child_name_str, 
									exprBuf,
									pExpr);
		allRules = lappend(allRules, pRule);


	} /* end if HASH */

	if (pBy->partType == PARTTYP_LIST)	
	{
		Node			*pIndAND = NULL;
		Node			*pIndOR  = NULL;
		List			*colElts = pBy->keys;
		ListCell		*lc      = NULL;
		List			*valElts = NULL;
		ListCell		*lc_val  = NULL;
		PartitionRangeItem *pRI  = NULL;
		char			 exprBuf[10000];
		char			 ANDBuf[10000];
		char			 ORBuf[10000];

		exprBuf[0] = '\0';
		ANDBuf[0]  = '\0';
		ORBuf[0]   = '\0';

		Assert(pBSpec->partStart);
		pRI = (PartitionRangeItem *)(pBSpec->partStart);
		Assert(pRI->partRangeVal);

		valElts = pRI->partRangeVal;
		lc_val = list_head(valElts);

		/* number of values is a multiple of the number of columns */

		for ( ; lc_val ; ) /* for all vals */
		{
			lc = list_head(colElts);
			pIndAND = NULL;

			for (; lc; lc = lnext(lc)) /* for all cols */
			{
				Node *pCol = lfirst(lc);
				Node *pEq  = NULL;
				Value *pColConst;
				A_Const *pValConst;
				ColumnRef *pCRef;

				pCRef = makeNode(ColumnRef); /* need columnref for WHERE */

				if (NULL == lc_val)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("mismatched columns for VALUES%s",
									at_depth),
							 parser_errposition(pstate, pBSpec->location)));
				}

				pColConst = (Value *)pCol;
				pValConst = (A_Const *)lfirst(lc_val);

				Assert(IsA(pColConst, String));

				pCRef->location = -1;
				pCRef->fields = list_make1(pColConst);

				if (!(IsA(pValConst, A_Const)))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("invalid use of non-constant type "
									"in boundary specification%s",
									at_depth),
							 parser_errposition(pstate, pBSpec->location)));
						
				}

				/* equality expression: column = value */
				pEq = (Node *) makeSimpleA_Expr(AEXPR_OP, "=", 
												(Node *)pCRef,
												(Node *)pValConst,
												-1 /* position */);


				if (!deparse_partition_rule((Node *)pEq, 
											exprBuf, sizeof(exprBuf)))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("unknown type or operator%s",
									at_depth),
							 parser_errposition(pstate, pBSpec->location)));

				}
				
				/* for multiple cols - AND the matches eg: 
				   (col = value) AND (col = value) 
				*/
				if (pIndAND)
				{
					char *pfoo = pstrdup(ANDBuf);

					pIndAND =
						(Node *) makeA_Expr(AEXPR_AND, NIL, 
											pIndAND,
											pEq,
											-1 /* position */);

					snprintf(ANDBuf, sizeof(ANDBuf), "((%s) and %s)",
							 exprBuf,
							 pfoo
						 );

					pfree(pfoo);
				}
				else
				{
					pIndAND = pEq;
					snprintf(ANDBuf, sizeof(ANDBuf), "(%s)", exprBuf);
				}

				lc_val = lnext(lc_val);
			} /* end for all cols */

			/* if more VALUES than columns, then multiple matching
			   conditions, so OR them eg: 
			  
			   ((col = value) AND (col = value)) OR
			   ((col = value) AND (col = value)) OR
			   ((col = value) AND (col = value))
			   
			*/

			if (pIndOR)
			{
				char *pfoo = pstrdup(ORBuf);

				pIndOR =
					(Node *) makeA_Expr(AEXPR_OR, NIL, 
										pIndOR,
										pIndAND,
										-1 /* position */);

				snprintf(ORBuf, sizeof(ORBuf), "((%s) OR %s)",
						 ANDBuf,
						 pfoo
						 );

					pfree(pfoo);

			}
			else
			{
				pIndOR = pIndAND;
				snprintf(ORBuf, sizeof(ORBuf), "(%s)", ANDBuf);
			}

		} /* end for all vals */

		pRule = make_prule_catalog(pstate,
								   cxt, stmt, 
								   partitionBy, pElem,
								   at_depth, child_name_str, 
								   ORBuf,
								   pIndOR);

		/* first the INSERT statement, then the CHECK constraint, then
		 * the RULE statement 
		 */
		allRules = list_make1(pRule);
		allRules = lappend(allRules, pIndOR);

		pRule = make_prule_rulestmt(pstate,
									cxt, stmt, 
									partitionBy, pElem,
									at_depth, child_name_str, 
									ORBuf,
									pIndOR);
		allRules = lappend(allRules, pRule);

	} /* end if LIST */

	/*
	     For RANGE partitions with an EVERY clause, the following 
		 fields are defined:
		 int everyOffset - 0 for no EVERY, else 1 to maxEveryOffset
		 int maxEveryOffset - 1 for no EVERY, else >1
		 ListCell	**pp_lc_anp - the pointer to the pointer 
		                          to the ListCell of [A]ll[N]ew[P]artitions,
								  a list of lists of stringified values
								  (as opposed to A_Const's).
	*/

	if (pBy->partType == PARTTYP_RANGE)	
	{
		Node			*pIndAND = NULL;
		Node			*pIndOR  = NULL;
		List			*colElts = pBy->keys;
		ListCell		*lc      = NULL;
		List			*valElts = NULL;
		ListCell		*lc_val  = NULL;
		PartitionRangeItem *pRI  = NULL;
		char			 exprBuf[10000];
		char			 ANDBuf[10000];
		char			 ORBuf[10000];
		int				 range_idx;

		ListCell		*lc_every_val = NULL;
		List *allNewCols = NIL;

		exprBuf[0] = '\0';
		ANDBuf[0]  = '\0';
		ORBuf[0]   = '\0';

		pRI = (PartitionRangeItem *)(pBSpec->partStart);
		
		if (pRI)
			valElts = pRI->partRangeVal;
		else
			valElts = NULL;						

		if (maxEveryOffset > 1) /* if have an EVERY clause */
		{
			ListCell	*lc_anp = NULL;

			/* check the list of "all new partitions" */
			if (pp_lc_anp)
			{
				lc_anp = *pp_lc_anp;
				if (lc_anp)
				{
					allNewCols = lfirst(lc_anp);

					/* find the list of columns for a new partition */
					if (allNewCols)
					{
						lc_every_val = list_head(allNewCols);
					}
				}
			}
		} /* end if every */

		/* number of values must equal of the number of columns */

		for (range_idx = 0; range_idx < 2; range_idx++)
		{
			char *expr_op = ">";

			if (everyOffset > 0) /* for generated START for EVERY */
				expr_op = ">=";  /* always be inclusive           */
			else 
				/* only inclusive set that way */
				if (pRI && (1 == pRI->partInclusive))
					expr_op = ">=";

			if (range_idx)
			{
				pRI = (PartitionRangeItem *)(pBSpec->partEnd);

				if (pRI)
					valElts = pRI->partRangeVal;
				else
					valElts = NULL;						

				expr_op = "<";

				/* for generated END for EVERY always be exclusive */
				if ((everyOffset + 1) <  maxEveryOffset) 
					expr_op = "<";
				else 
					/* only be inclusive if set that way */
					if (pRI && (1 == pRI->partInclusive))
						expr_op = "<=";

				/* If have EVERY, and not the very first START or last END */
				if ((0 != everyOffset) && (everyOffset+1 < maxEveryOffset))
				{
					if (*pp_lc_anp)
					{
						*pp_lc_anp = lnext(*pp_lc_anp);

						if (*pp_lc_anp)
						{
							allNewCols = lfirst(*pp_lc_anp);

							if (allNewCols)
								lc_every_val = list_head(allNewCols);
						}
					}
				}
			} /* end if range_idx != 0 */

			lc_val = list_head(valElts);

			for ( ; lc_val ; ) /* for all vals */
			{
				lc = list_head(colElts);
				pIndAND = NULL;

				for (; lc; lc = lnext(lc)) /* for all cols */
				{
					Node *pCol = lfirst(lc);
					Node *pEq  = NULL;
					Value *pColConst;
					A_Const *pValConst;
					ColumnRef *pCRef;

					pCRef = makeNode(ColumnRef); /* need columnref for WHERE */

					if (NULL == lc_val)
					{
						char *st_end = "START";

						if (range_idx)
						{
							st_end = "END";
						}

						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("too few columns in %s specification%s",
										st_end,
										at_depth),
								 parser_errposition(pstate, pBSpec->location)));
					}

					pColConst = (Value *)pCol;
					pValConst = (A_Const *)lfirst(lc_val);

					Assert(IsA(pColConst, String));

					pCRef->location = -1;
					pCRef->fields = list_make1(pColConst);

					if (pValConst->type != T_A_Const)
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("invalid use of non-constant type "
										"in boundary specification%s",
										at_depth),
								 parser_errposition(pstate, pBSpec->location)));
						
					}

					/* if have an EVERY, and not the first START or
					 * last END, then use the generated, stringified
					 * value from partition_range_every to build a new
					 * a_const in the expression.
					 */
					if (((0 == range_idx)
						 && (everyOffset > 0))
						|| ((1 == range_idx)
							&& (everyOffset+1 < maxEveryOffset))
							)
					{
						char *val_str;
						A_Const *pNewValConst;

						val_str = lfirst(lc_every_val);

						pNewValConst = make_a_const(pValConst, val_str);

						pValConst = pNewValConst;
					} /* end redo work for EVERY */

					/* equality expression: column = value */
					pEq = (Node *) makeSimpleA_Expr(AEXPR_OP, 
													expr_op, 
													(Node *)pCRef,
													(Node *)pValConst,
													-1 /* position */);

					if (!deparse_partition_rule((Node *)pEq, 
												exprBuf, sizeof(exprBuf)))
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("unknown type or operator%s",
										at_depth),
								 parser_errposition(pstate, pBSpec->location)));
						
					}


					/* for multiple cols - AND the matches eg: 
					   (col = value) AND (col = value) 
					*/
					if (pIndAND)
					{
						char *pfoo = pstrdup(ANDBuf);

						pIndAND =
							(Node *) makeA_Expr(AEXPR_AND, NIL, 
												pIndAND,
												pEq,
												-1 /* position */);

						snprintf(ANDBuf, sizeof(ANDBuf), "((%s) and %s)",
								 exprBuf,
								 pfoo
							 );

						pfree(pfoo);
					}
					else
					{
						pIndAND = pEq;
						snprintf(ANDBuf, sizeof(ANDBuf), "(%s)", exprBuf);
					}

					lc_val = lnext(lc_val);

					if (((0 == range_idx)
						 && (everyOffset > 0))
						|| ((1 == range_idx)
							&& (everyOffset+1 < maxEveryOffset))
							)
					{

						if (lc_every_val)
						{
							lc_every_val = lnext(lc_every_val);
						}
					}
				} /* end for all cols */

				/* if more VALUES than columns, then complain
				*/
				if (lc_val)
				{
					char *st_end = "START";

					if (range_idx)
					{
						st_end = "END";
					}

					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("too many columns in %s specification%s",
									st_end,
									at_depth),
							 parser_errposition(pstate, pBSpec->location)));
				}

				if (pIndOR)
				{
					char *pfoo = pstrdup(ORBuf);
					/* XXX XXX build an AND for now.  But later we
					 * split this to distinguish START and END
					 * conditions */

					pIndOR =
						(Node *) makeA_Expr(AEXPR_AND, NIL, 
											pIndOR,
											pIndAND,
											-1 /* position */);

					snprintf(ORBuf, sizeof(ORBuf), "((%s) AND %s)",
							 ANDBuf,
							 pfoo
							 );

						pfree(pfoo);

				}
				else
				{
					pIndOR = pIndAND;
					snprintf(ORBuf, sizeof(ORBuf), "(%s)", ANDBuf);
				}

			} /* end for all vals */

		} /* end for range_idx */

		pRule = make_prule_catalog(pstate,
								   cxt, stmt, 
								   partitionBy, pElem,
								   at_depth, child_name_str, 
								   ORBuf,
								   pIndOR);

		/* first the INSERT statement, then the CHECK constraint, then
		 * the RULE statement 
		 */

		allRules = list_make1(pRule);
		allRules = lappend(allRules, pIndOR);

		pRule = make_prule_rulestmt(pstate,
									cxt, stmt, 
									partitionBy, pElem,
									at_depth, child_name_str, 
									ORBuf,
									pIndOR);
		allRules = lappend(allRules, pRule);

	} /* end if RANGE */

	return allRules;
} /* end make_partition_rules */

static int
partition_range_compare(ParseState *pstate, CreateStmtContext *cxt,
						CreateStmt *stmt, PartitionBy *pBy,
						char *at_depth, int partNumber,
						char *compare_op, /* =, <, > only */
						PartitionRangeItem *pRI1,
						PartitionRangeItem *pRI2)
{
	int rc = -1;
	ListCell		*lc1 = NULL;
	ListCell		*lc2 = NULL;
	List			*cop = lappend(NIL, makeString(compare_op));

	if (!pRI1 || !pRI2) /* error */
		return rc;

	lc1 = list_head(pRI1->partRangeVal);
	lc2 = list_head(pRI2->partRangeVal);

 L_redoLoop:
	for (; lc1 && lc2; )
	{
		Node *n1 = lfirst(lc1);
		Node *n2 = lfirst(lc2);

		if (!equal(n1, n2))
			break;

		lc1 = lnext(lc1);
		lc2 = lnext(lc2);
	}

	if (!lc1 && !lc2) /* both empty, so all values are equal */
	{
		if (strcmp("=", compare_op) == 0)
			return 1;
		else
			return 0;
	}
	else if (lc1 && lc2) /* both not empty, so last value was different */
	{
		Datum res;
		Node *n1 = transformExpr(pstate, lfirst(lc1));
		Node *n2 = transformExpr(pstate, lfirst(lc2));

		/* XXX XXX: can't trust equal() code on "typed" data like date
		 * types (because it compares things like "location") so do a
		 * real compare using eval .
		 */
		res = eval_basic_opexpr(pstate, cop, n1, n2, NULL);
		
		if (DatumGetBool(res) && strcmp("=", compare_op) == 0)
		{
			/* surprise! they were equal after all.  So keep going... */
			lc1 = lnext(lc1);
			lc2 = lnext(lc2);

			goto L_redoLoop;
		}

		return DatumGetBool(res);
	}
	else if (lc1 || lc2)
	{
		/* lists of different lengths */
		if (strcmp("=", compare_op) == 0)
			return 0;

		/* longer list is bigger? */

		if (strcmp("<", compare_op) == 0)
		{
			if (lc1)
				return 0;
			else
				return 1;
		}
		if (strcmp(">", compare_op) == 0)
		{
			if (lc1)
				return 1;
			else
				return 0;
		}
	}

	return rc;
} /* end partition_range_compare */

static void
validate_range_partition(partValidationState *vstate)
{
	bool bAppendRange = false;
	PartitionBoundSpec *prevBSpec = NULL;

	if (vstate->pBSpec)
	{
		/* XXX: create a type2name function */
		char *specTName = "LIST";

		if (vstate->pBSpec->partType == PARTTYP_LIST)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid use of %s boundary "
							"specification in partition%s of "
							"type RANGE%s",
							specTName,
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, vstate->pBSpec->location)));
			
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("missing boundary specification in "
						"partition%s of type RANGE%s",
						vstate->namBuf,
						vstate->at_depth),
				 parser_errposition(vstate->pstate, vstate->pElem->location)));
		
	}

	{
		/* if the previous partition was named, and
		 * current is not, and the prev and current
		 * use single, complementary START/END specs,
		 * then complain.
		 */

		/* must have a valid RANGE pBSpec now or else
		 * would have error'd out... 
		 */
		int currStartEnd = 0;

		if (vstate->pBSpec->partStart)
			currStartEnd += 1;
		if (vstate->pBSpec->partEnd)
			currStartEnd += 2;

		/* Note: for first loop, prevStartEnd = 0, so
		 * this test is ok */

		if ((vstate->prevHadName && !(vstate->pElem->partName))
			&&
			(((vstate->prevStartEnd == 1)
			  && (currStartEnd == 2))
			 ||
			 ((vstate->prevStartEnd == 2)
			  && (currStartEnd == 1)))
				)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid use of mixed named and "
							"unnamed RANGE boundary "
							"specifications%s",
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));

		}

		vstate->prevStartEnd = currStartEnd;
	}

	vstate->prevHadName  = !!(vstate->pElem->partName); /* bool t/f */

	/* fixup boundaries for previous partition ending
	 * if necessary 
	 */
	if (!vstate->prevElem)
	{
		bAppendRange = true;
		goto L_setprevElem;
	}

	prevBSpec = (PartitionBoundSpec *)vstate->prevElem->boundSpec;
				
	/* XXX XXX: can check for overlap here too */

	if (!prevBSpec)
	{
		/* XXX XXX: if the previous partition declaration
		 * does not have a boundary spec then its end
		 * needs to be the start of the current */

		bAppendRange = true;
		goto L_setprevElem;
	}

	if (vstate->pBSpec->partStart)
	{
		if (!prevBSpec->partEnd)
		{
			PartitionRangeItem *pRI =
					(PartitionRangeItem *)
					(vstate->pBSpec->partStart);
			PartitionRangeItem *prevRI = 
					makeNode(PartitionRangeItem); 
				
			if (prevBSpec->partStart)
			{
				int				 compareRc = 0;
				PartitionRangeItem *pRI1 = 
						(PartitionRangeItem *)vstate->pBSpec->partStart;
				PartitionRangeItem *pRI2 = 
						(PartitionRangeItem *)prevBSpec->partStart;

				compareRc = 
						partition_range_compare(vstate->pstate,
												vstate->cxt, vstate->stmt, 
												vstate->pBy,
												vstate->at_depth,
												vstate->partNumber,
												"<",
												pRI2,
												pRI1);

				if (1 != compareRc)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("START of partition%s less "
									"than START of previous%s",
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));

				}

			}

			prevRI->partEveryNum  = NULL; 
			prevRI->location  = 
					pRI->location;

			prevRI->partRangeVal  = 
					list_copy(pRI->partRangeVal);

			/* invert sense of inclusiveness */
			prevRI->partInclusive = 
					(1 == pRI->partInclusive) ? 
					2 : 1;

			prevBSpec->partEnd = (Node *)prevRI;

			/* don't need to check if range overlaps */
			bAppendRange = true;

		}
		else if (0) /* XXX XXX XXX XXX */
		{
			/* else if overlap complain */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("start of partition%s overlaps previous%s",
								vstate->namBuf,
								vstate->at_depth),
						 parser_errposition(vstate->pstate, 
											vstate->pBSpec->location)));
				
		}
		
	} /* end if current partStart */
	else /* no start, so use END of previous */
	{
		if (!prevBSpec->partEnd)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot derive starting value of "
							"partition%s based upon ending of "
							"previous%s",
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));
			
		}
		else /* build ri for current */
		{
			PartitionRangeItem *pRI =
					makeNode(PartitionRangeItem); 
			PartitionRangeItem *prevRI = 
					(PartitionRangeItem *)
					(prevBSpec->partEnd);
			
			pRI->partEveryNum  = NULL; 
			pRI->location  = 
					prevRI->location;
			
			pRI->partRangeVal  = 
					list_copy(prevRI->partRangeVal);

			/* invert sense of inclusiveness */
			pRI->partInclusive = 
					(1 == prevRI->partInclusive) ? 
					2 : 1;

			vstate->pBSpec->partStart = (Node *)pRI;

			/* don't need to check if range overlaps */
			bAppendRange = true;
		} /* end build ri for current */
	} /* end use END of previous */

L_setprevElem:

	/* check for overlap, 
	 * then add new partitions to sorted list
	 */

	if (vstate->pBSpec->partStart &&
		vstate->pBSpec->partEnd)
	{
		int				 compareRc = 0;
		PartitionRangeItem *pRI1 = 
				(PartitionRangeItem *)vstate->pBSpec->partStart;
		PartitionRangeItem *pRI2 = 
				(PartitionRangeItem *)vstate->pBSpec->partEnd;

		compareRc = 
				partition_range_compare(vstate->pstate,
										vstate->cxt, vstate->stmt, 
										vstate->pBy,
										vstate->at_depth,
										vstate->partNumber,
										">",
										pRI1,
										pRI2);

		if (0 != compareRc)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("START greater than END for partition%s%s",
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));

		}

	}
	
	if (NIL == vstate->allRangeVals)
		bAppendRange = true;

	/* check previous first */
	if (!bAppendRange &&
		(vstate->pBSpec->partStart &&
		 prevBSpec &&
		 prevBSpec->partEnd))
	{/* compare to last */
		int				 compareRc = 0;
		PartitionRangeItem *pRI1 = 
				(PartitionRangeItem *)vstate->pBSpec->partStart;
		PartitionRangeItem *pRI2 = 
				(PartitionRangeItem *)prevBSpec->partEnd;

		compareRc = 
				partition_range_compare(vstate->pstate,
										vstate->cxt, vstate->stmt, 
										vstate->pBy,
										vstate->at_depth,
										vstate->partNumber,
										"=",
										pRI1,
										pRI2);

		if (
			(pRI2->partInclusive == 1) &&
			(pRI1->partInclusive == 1) &&
			(1 == compareRc))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("starting value of partition%s "
							"overlaps previous range%s",
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));

		}

		/* if values are equal, but not inclusive to both,
		 * then append is possible */
		if (1 != compareRc)
			compareRc = 
				partition_range_compare(vstate->pstate,
										vstate->cxt, vstate->stmt, 
										vstate->pBy,
										vstate->at_depth,
										vstate->partNumber,
										">",
										pRI1,
										pRI2);

		if (1 == compareRc)
			bAppendRange = 1;


		/* else could be start out of sequence, so must
		 * search entire sorted list */

		if (0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("starting value of partition%s "
							"overlaps previous range%s",
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));
		}

	} /* end compare to last */


	if (bAppendRange)
	{
		vstate->allRangeVals = lappend(vstate->allRangeVals, vstate->pBSpec);
	}
	else
	{ /* find position for current range */
		ListCell		*lc_all  = list_head(vstate->allRangeVals);
		ListCell		*lc_allPrev = NULL;
		int				 compareRc = 0;
		/* set up pieces of error messages */
		char			*currPartRI   = "Ending";
		char			*otherPartPos = "next";

		/* linear search sorted list of range specs:

		   While the current start key is >= loop val
		   start key advance.  

		   If current start key < loop val start key,
		   then see if curr start key < *previous* loop
		   val end key, ie falls in previous range. 

		   If so, error out, else splice current range in
		   after previous.

		*/

		for ( ; lc_all ; lc_all = lnext(lc_all)) /* for lc_all */
		{
			PartitionBoundSpec *lcBSpec = 
					(PartitionBoundSpec *) lfirst(lc_all);
			PartitionRangeItem *pRI1 = 
					(PartitionRangeItem *)vstate->pBSpec->partStart;
			PartitionRangeItem *pRI2 = 
					(PartitionRangeItem *)lcBSpec->partStart;

			Assert(vstate->pBSpec->partStart);

			if (lcBSpec->partStart)
			{
				compareRc = 
					partition_range_compare(vstate->pstate,
											vstate->cxt, vstate->stmt, 
											vstate->pBy,
											vstate->at_depth,
											vstate->partNumber,
											"=",
											pRI1,
											pRI2);

				if (-1 == compareRc)
					break;

				if ((pRI2->partInclusive == 1) &&
					(pRI1->partInclusive == 1) &&
					(1 == compareRc))
				{
					currPartRI   = "Starting";
					otherPartPos = "previous";
					compareRc = -2;
					break;
				}

				compareRc = 
					partition_range_compare(vstate->pstate,
											vstate->cxt, vstate->stmt, 
											vstate->pBy,
											vstate->at_depth,
											vstate->partNumber,
											"<",
											pRI1,
											pRI2);

			}
			else
				/* if first range spec has no start 
				 * (ie start=MINVALUE) then current start
				 * must be after it 
				 */
				compareRc = 0; /* current > MINVALUE */

			if (-1 == compareRc)
				break;

			if (1 == compareRc) /* if curr less than loop val */
			{
				/* curr start is less than loop val, so
				 * check that curr end is less than loop
				 * val start (including equality case)
				 */

				pRI1 = (PartitionRangeItem *)vstate->pBSpec->partEnd;

				if (!pRI1)
				{
					/* if current start is less than
					 * loop val start but current end is
					 * unterminated (ie ending=MAXVALUE)
					 * then it must overlap
					 */
					currPartRI   = "Ending";
					otherPartPos = "next";
					compareRc = -2;
					break;
				}

				compareRc =
					partition_range_compare(vstate->pstate,
											vstate->cxt, vstate->stmt, 
											vstate->pBy,
											vstate->at_depth,
											vstate->partNumber,
											"=",
											pRI2,
											pRI1);
				
				if (-1 == compareRc)
					break;

				if (
					(pRI2->partInclusive == 1) &&
					(pRI1->partInclusive == 1) &&
					(1 == compareRc))
				{
					currPartRI   = "Ending";
					otherPartPos = "next";
					compareRc = -2;
					break;
				}

				compareRc = partition_range_compare(vstate->pstate,
													vstate->cxt, vstate->stmt, 
													vstate->pBy,
													vstate->at_depth,
													vstate->partNumber,
													"<",
													pRI2,
													pRI1);

				if (-1 == compareRc)
					break;

				if (1 == compareRc)
				{
					currPartRI   = "Ending";
					otherPartPos = "next";
					compareRc = -2;
					break;
				}
			
				/* if current is less than loop val and no
				 * previous, then append vstate->allRangeVals to
				 * the current, instead of vice versa 
				 */
				if (!lc_allPrev)
					break;

				lcBSpec = 
					(PartitionBoundSpec *) lfirst(lc_allPrev);

				pRI2 = 
					(PartitionRangeItem *)lcBSpec->partEnd;

				compareRc = 
						partition_range_compare(vstate->pstate,
												vstate->cxt, vstate->stmt, 
												vstate->pBy,
												vstate->at_depth,
												vstate->partNumber,
												"=",
												pRI1,
												pRI2);

				if (-1 == compareRc)
					break;

				if (
					(pRI2->partInclusive == 1) &&
					(pRI1->partInclusive == 1) &&
					(1 == compareRc))
				{
					currPartRI   = "Starting";
					otherPartPos = "previous";
					compareRc = -2;
					break;
				}

				compareRc = 
						partition_range_compare(vstate->pstate,
												vstate->cxt, vstate->stmt, 
												vstate->pBy,
												vstate->at_depth,
												vstate->partNumber,
												"<",
												pRI1,
												pRI2);

				if (-1 == compareRc)
					break;

				if (1 == compareRc)
				{
					currPartRI   = "Starting";
					otherPartPos = "previous";
					compareRc = -2;
					break;
				}
			} /* end if curr less than loop val */

			lc_allPrev = lc_all;
		} /* end for lc_all */

		if (-1 == compareRc)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid range comparison for partition%s%s",
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));

		}

		if (-2 == compareRc)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("%s value of partition%s overlaps %s range%s",
							currPartRI,
							vstate->namBuf,
							otherPartPos,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, 
										vstate->pBSpec->location)));
		}

		if (lc_allPrev)
		{
			/* append cell returns new cell, not list */
			lappend_cell(vstate->allRangeVals, 
						 lc_allPrev, 
						 vstate->pBSpec);
		}
		else /* no previous, so current is start */
			vstate->allRangeVals = list_concat(list_make1(vstate->pBSpec),
									   vstate->allRangeVals);
	} /* end find position for current range */
	vstate->prevElem = vstate->pElem;
}

static void
validate_list_partition(partValidationState *vstate)
{
	if (vstate->pBSpec)
	{
		char *specTName = "RANGE";
		
		if (vstate->pBSpec->partType == PARTTYP_RANGE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid use of %s boundary specification in "
							"partition%s of type LIST%s",
							specTName,
							vstate->namBuf,
							vstate->at_depth),
					 parser_errposition(vstate->pstate, vstate->pBSpec->location)));
			
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("missing boundary specification in "
						"partition%s of type LIST%s",
						vstate->namBuf,
						vstate->at_depth),
				 parser_errposition(vstate->pstate, vstate->pElem->location)));

	}

	if (vstate->pBSpec->partStart)
	{
		PartitionRangeItem *pRI  = NULL;

		pRI = (PartitionRangeItem *)vstate->pBSpec->partStart;

		if (pRI->partRangeVal)
		{
			List			*currVals = NULL;
			List			*currSub  = NULL;
			List			*valElts = NULL;
			ListCell		*lc_val  = NULL;
			int valCount = list_length(pRI->partRangeVal);
			int vcnt     = valCount;

			/* check if insufficient values, or 
			   (num values) modulo (num keys)
			*/
			if ((valCount < vstate->pBy->keyCount)
				|| (0 != (valCount % (vstate->pBy->keyCount))))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("mismatched columns in "
								"partition%s for VALUES%s",
								vstate->namBuf,
								vstate->at_depth),
						 parser_errposition(vstate->pstate, vstate->pBSpec->location)));
			}

			valElts = pRI->partRangeVal;
			lc_val = list_head(valElts);

			/* build temporary list of lists */
			for ( ; lc_val ; lc_val = lnext(lc_val)) /* for all vals */
			{
				currSub = lappend(currSub, lfirst(lc_val));
				vcnt--;

				if (!vcnt)
				{
					vcnt = valCount;
					currVals = lappend(currVals, currSub);
					currSub = NIL;
				}

			} /* end for all vals */

			if (NIL != vstate->allListVals)
			{
				ListCell		*lc_all  = list_head(vstate->allListVals);
				ListCell		*lc_curr;

				for ( ; lc_all ; lc_all = lnext(lc_all)) 
				{
					List	*l1_all = (List *)lfirst(lc_all);
					List	*l1_diff;
					
					lc_curr = list_head(currVals);

					for ( ; lc_curr ; lc_curr = lnext(lc_curr))
					{
						List *l1_curr = (List *)lfirst(lc_curr);

						l1_diff = list_difference(l1_curr, l1_all);

						if (l1_diff &&
							list_length(l1_diff))
						{
								/* ok */
						}
						else
						{
							/* XXX XXX: better message !! */

							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
									 errmsg("duplicate VALUES "
											"in partition%s%s",
											vstate->namBuf,
											vstate->at_depth),
									 parser_errposition(vstate->pstate, vstate->pBSpec->location)));


						}

						if (l1_diff)
							list_free(l1_diff);
					}

				}
			}
			vstate->allListVals = list_concat(vstate->allListVals, currVals);
		}
	}
}

static int
validate_partition_spec(ParseState *pstate, CreateStmtContext *cxt, 
						CreateStmt *stmt, PartitionBy *pBy,
						char *at_depth, int	partNumber)
{
	PartitionSpec	*pSpec;
	char			 namBuf[NAMEDATALEN];
	int				 numElts = 0;
	List			*partElts;
	ListCell		*lc = NULL;
	List			*allPartNames = NIL;
	partValidationState *vstate;

	vstate = palloc0(sizeof(partValidationState));

	vstate->pstate = pstate;
	vstate->cxt = cxt;
	vstate->stmt = stmt;
	vstate->pBy = pBy;
	vstate->at_depth = at_depth;
	vstate->partNumber = partNumber;
	
	Assert(pBy);
	pSpec = (PartitionSpec *)pBy->partSpec;

	/* find number of partitions in the specification, and match it up
	 * with the PARTITIONS clause if partitioned by HASH, and
	 * determine the subpartition specifications.  Perform basic error
	 * checking on boundary specifications.
	 */

	/* NOTE: a top-level PartitionSpec never has a subSpec, but a
	 * PartitionSpec derived from an inline SubPartition
	 * specification might have one  
	 */


	/* track previous boundary spec to check for this subtle problem:

	(
	partition aa start (2007,1) end (2008,2),
	partition bb start (2008,2) end (2009,3)
	);
	This declaration would create four partitions:
	1. named aa, starting at 2007, 1
	2. unnamed, ending at 2008, 2
	3. named bb, starting at 2008, 2
	4. unnamed, ending at 2009, 3

	Warn user if they do this, since they probably wanted
	1. named aa, starting at 2007, 1 and ending at 2008, 2
	2. named bb, starting at 2008, 2 and ending at 2009, 3
		   
	The extra comma between the start and end is the problem.
		   
	*/

	partElts = pSpec->partElem;

	foreach(lc, partElts)
	{
		PartitionElem *pElem = (PartitionElem *)lfirst(lc);
		PartitionBoundSpec *pBSpec = NULL;

		vstate->pElem = pElem;
		
		numElts++;

		if (pElem)
		{
			vstate->pBSpec = pBSpec = (PartitionBoundSpec *)pElem->boundSpec;

			if (pElem->partName)
			{
				snprintf(namBuf, sizeof(namBuf), " \"%s\"", 
						 strVal(pElem->partName));

				if (allPartNames && list_member(allPartNames, pElem->partName))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("duplicate partition name "
									"for partition%s%s",
									namBuf,
									at_depth),
							 parser_errposition(pstate, pElem->location)));
				}
				allPartNames = lappend(allPartNames, pElem->partName);
			}
			else
			{
				snprintf(namBuf, sizeof(namBuf), " number %d", 
						 numElts);
			}
			
		}
		else
		{
			snprintf(namBuf, sizeof(namBuf), " number %d", 
					 numElts);
		}

		vstate->namBuf = namBuf;

		switch (pBy->partType)
		{
			case PARTTYP_HASH:
				if (vstate->pBSpec)
				{
					char *specTName = "RANGE";
					
					if (vstate->pBSpec->partType == PARTTYP_LIST)
					{
						specTName = "LIST";
					}

					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("invalid use of %s boundary specification "
									"in partition%s of type HASH%s",
									specTName,
									vstate->namBuf,
									vstate->at_depth),
							 parser_errposition(pstate, pBSpec->location)));
				}
				break;
			case PARTTYP_RANGE:
				validate_range_partition(vstate);
				break;

			case PARTTYP_LIST:
				validate_list_partition(vstate);
				break;

			case PARTTYP_REFERENCE: /* for future use... */
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("unknown partition type %d %s",
								pBy->partType, at_depth),
						 parser_errposition(pstate, pBy->location)));
				break;
				
		}
	} /* end foreach(lc, partElts) */

	if ((partNumber > -1) && (numElts != partNumber))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("PARTITIONS \"%d\" must match \"%d\" elements "
						"in specification%s",
						 partNumber, numElts, vstate->at_depth),
				 parser_errposition(pstate, pBy->location)));
	}

	/* cleanup temporary lists */
	if (vstate->allListVals)
	{
		ListCell		*lc_all  = list_head(vstate->allListVals);

		for ( ; lc_all ; lc_all = lnext(lc_all)) 
		{
			List	*l1_all = (List *)lfirst(lc_all);
			list_free(l1_all);
		}
		list_free(vstate->allListVals);
	}

	if (vstate->allRangeVals)
	{
		list_free(vstate->allRangeVals);
	}

	if (allPartNames)
	{
		list_free(allPartNames);
	}

	return numElts;
} /* end validate_partition_spec */

/* 
 * Get the actual value from the expression. There are only a limited range
 * of cases we must cover because the parser guarantees constant input.
 */
static Datum
arg_get_val(Node *node)
{
	switch (nodeTag(node))
	{
		case T_FuncExpr:
			{
				/* must have been cast to the operator. */
				FuncExpr *fe = (FuncExpr *)node;
				Datum d;
				Const *c;
				
				Assert(list_length(fe->args) == 1);
				Assert(IsA(linitial(fe->args), Const));
				
				c = (Const *)linitial(fe->args);
				d = OidFunctionCall1(fe->funcid, c->constvalue);
				return d;
			}
			break;
		case T_Const:
			return ((Const *)node)->constvalue;
			break;
		default:
			elog(ERROR, "unknown partitioning argument type: %d",
				 nodeTag(node));
			break;

	}
	return 0; /* quieten GCC */
}

/*
 * Evaluate a basic operator expression from a partitioning specification.
 * The expression will only be an op expr but the sides might contain
 * a coercion function. The underlying value will be a simple constant,
 * however.
 *
 * If restypid is non-NULL and *restypid is set to InvalidOid, we tell the 
 * caller what the return type of the operator is. If it is anything but 
 * InvalidOid, coerce the operation's result to that type.
 */
static Datum
eval_basic_opexpr(ParseState *pstate, List *oprname, Node *leftarg, 
				  Node *rightarg, Oid *restypid)
{
	Datum res;
	Datum lhs;
	Datum rhs;
	OpExpr *opexpr;
	HeapTuple optup;
	
	opexpr = (OpExpr *)make_op(pstate, oprname, leftarg, rightarg, 0);
	optup = SearchSysCache(OPEROID, ObjectIdGetDatum(opexpr->opno),
						   0, 0, 0);

	if (optup == NULL)	  /* should not fail */
		elog(ERROR, "cache lookup failed for operator %u", opexpr->opno);
	opexpr->opfuncid = ((Form_pg_operator)GETSTRUCT(optup))->oprcode;

	ReleaseSysCache(optup);

	/* args to the function must be Const */
	lhs = arg_get_val((Node *)linitial(opexpr->args));
	rhs = arg_get_val((Node *)lsecond(opexpr->args));
	res = OidFunctionCall2(opexpr->opfuncid, lhs, rhs);

	/* If the caller supplied a target result type, coerce if necesssary */
	if (PointerIsValid(restypid))
	{
		if (OidIsValid(*restypid))
		{
			if (*restypid != opexpr->opresulttype)
			{
				Expr *e;
				Type typ = typeidType(opexpr->opresulttype);
				int32 typmod;
				Const *c;
	
				c = makeConst(opexpr->opresulttype, typeLen(typ), res,
							  false, typeByVal(typ));
				ReleaseSysCache(typ);

				typ = typeidType(*restypid);
				typmod = ((Form_pg_type)typ)->typtypmod;
				ReleaseSysCache(typ);

				e = (Expr *)coerce_to_target_type(NULL, (Node *)c, 
												  opexpr->opresulttype,
												  *restypid, typmod,
												  COERCION_EXPLICIT,
												  COERCE_IMPLICIT_CAST);
				Assert(PointerIsValid(e));
				res = arg_get_val((Node *)e);
			}
		}
		else
			*restypid = opexpr->opresulttype;
	}
	
	return res;
}

static int 
partition_range_every(ParseState *pstate, CreateStmtContext *cxt, 
					  CreateStmt *stmt, PartitionBy *pBy, char *at_depth)
{
	PartitionSpec	*pSpec;
	char			 namBuf[NAMEDATALEN];
	int				 numElts = 0;
	List			*partElts;
	ListCell		*lc = NULL;
	ListCell		*lc_prev = NULL;
		
	Assert(pBy);
	Assert(pBy->partType == PARTTYP_RANGE);

	pSpec    = (PartitionSpec *)pBy->partSpec;

	partElts = pSpec->partElem;

	lc = list_head(partElts);

	for ( ; lc; ) /* foreach */
	{
		PartitionElem		*pElem = (PartitionElem *)lfirst(lc);
		PartitionBoundSpec	*pBSpec = NULL;
		int					 numCols  = 0;
		int					 colCnt   = 0;
		Const				*everyCnt;
		PartitionRangeItem	*pRI_Start = NULL;
		PartitionRangeItem	*pRI_End   = NULL;
		PartitionRangeItem	*pRI_Every = NULL;
		ListCell			*lc_Start = NULL;
		ListCell			*lc_End   = NULL;
		ListCell			*lc_Every = NULL;
		List *allNewCols;
		List *allNewPartns = NIL;

		pBSpec = NULL;
		
		numElts++;

		if (!pElem)
			goto l_EveryLoopEnd;
		else
		{
			pBSpec = (PartitionBoundSpec *)pElem->boundSpec;

			if (!pBSpec)
				goto l_EveryLoopEnd;

			Assert (pBSpec->partType == PARTTYP_RANGE);

			if (pElem->partName)
			{
				snprintf(namBuf, sizeof(namBuf), " \"%s\"", 
						 strVal(pElem->partName));
			}
			else
			{
				snprintf(namBuf, sizeof(namBuf), " number %d", 
						 numElts);
			}
		}

		/* have a valid range bound spec */

		if (!pBSpec->partEvery)
			goto l_EveryLoopEnd;

		/* valid EVERY needs a START and END */
		if (!(pBSpec->partStart &&
			  pBSpec->partEnd))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("EVERY clause in partition%s "
							"requires START and END%s",
							namBuf,
							at_depth),
					 parser_errposition(pstate, pBSpec->location)));
		}

		everyCnt = make_const(makeInteger(1));

		pRI_Start = (PartitionRangeItem *)pBSpec->partStart;
		pRI_End   = (PartitionRangeItem *)pBSpec->partEnd;
		pRI_Every = (PartitionRangeItem *)pBSpec->partEvery;

		numCols = list_length(pRI_Every->partRangeVal);

		if ((numCols != list_length(pRI_Start->partRangeVal))
			|| (numCols != list_length(pRI_End->partRangeVal)))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("mismatch between EVERY, START and END "
							"in partition%s%s",
							namBuf,
							at_depth),
					 parser_errposition(pstate, pBSpec->location)));

		}

		for ( ; ; ) /* loop until exceed end */
		{
			int sqlRc = 0;
			lc_Start = list_head(pRI_Start->partRangeVal);
			lc_End   = list_head(pRI_End->partRangeVal);
			lc_Every = list_head(pRI_Every->partRangeVal);
			colCnt = numCols;
			allNewCols = NIL;

			for ( ; lc_Start && lc_End && lc_Every ; ) /* for all cols */
			{
				Node *n1 = lfirst(lc_Start);
				Node *n2 = lfirst(lc_End);
				Node *n3 = lfirst(lc_Every);
				char *compare_op = (1 == colCnt) ? "<" : "<=";
				Node *n1t, *n2t, *n3t;
				Datum res;
				Const *c;
				List *oprmul, *oprplus, *oprcompare;
				Oid restypid;
				Type typ;
				char *outputstr;

				oprmul = lappend(NIL, makeString("*"));
				oprplus = lappend(NIL, makeString("+"));
				oprcompare = lappend(NIL, makeString(compare_op));
				
				n1t = transformExpr(pstate, n1);
				n2t = transformExpr(pstate, n2);
				n3t = transformExpr(pstate, n3);

				Assert(IsA(n3t, Const));
				Assert(IsA(n2t, Const));
				Assert(IsA(n1t, Const));

				/* formula is n1t + (every_cnt * every_num * n3t) [<|<=] n2t */
				
				/* every_cnt * n3t */
				restypid = InvalidOid;
				res = eval_basic_opexpr(pstate, oprmul, (Node *)everyCnt, n3t,
										&restypid);
				typ = typeidType(restypid);
				c = makeConst(restypid, typeLen(typ), res, false,
							  typeByVal(typ));
				ReleaseSysCache(typ);
				
				/* every_num * (every_cnt * n3t) */
				if (pRI_Every->partEveryNum)
				{
					Node *et = transformExpr(pstate, pRI_Every->partEveryNum);
					
					restypid = InvalidOid;
					res = eval_basic_opexpr(pstate, oprmul, (Node *)c,
											et, &restypid);
					pfree(c);
					typ = typeidType(restypid);
					c = makeConst(restypid, typeLen(typ), res, false,
								  typeByVal(typ));
					ReleaseSysCache(typ);
				}
				
				/* 
				 * n1t + (...) 
				 * NOTE: order is important because several useful operators,
				 * like date + interval, only have a built in function when
				 * the order is this way (the reverse is implemented using
				 * an SQL function which we cannot evaluate at the moment).
				 */
				restypid = exprType(n1t);
				res = eval_basic_opexpr(pstate, oprplus, n1t, (Node *)c, 
										&restypid);
				typ = typeidType(restypid);
				c = makeConst(restypid, typeLen(typ), res, false,
							  typeByVal(typ));


				/* get the string for the calculated type */
				{
					Oid ooutput;
					bool isvarlena;
					FmgrInfo finfo;
 
					getTypeOutputInfo(restypid, &ooutput, &isvarlena);
					fmgr_info(ooutput, &finfo);
 
					if (isvarlena)
						res = PointerGetDatum(PG_DETOAST_DATUM(res));
 
					outputstr = OutputFunctionCall(&finfo, res);
				}

				ReleaseSysCache(typ);

				/* the comparison */
				restypid = InvalidOid;
				res = eval_basic_opexpr(pstate, oprcompare, (Node *)c, n2t,
										&restypid);
				
				if (!DatumGetBool(res))
				{
					if (outputstr)
						pfree(outputstr);

					sqlRc = 0;
					break;
				}
				else
				{
					allNewCols = lappend(allNewCols, pstrdup(outputstr));

					if (outputstr)
						pfree(outputstr);

					sqlRc = 1;
				}
				lc_Start = lnext(lc_Start);
				lc_End   = lnext(lc_End);
				lc_Every = lnext(lc_Every);
				colCnt--;
			} /* end for all cols */

			if (lc_Start || lc_End || lc_Every)
			{ /* allNewCols is incomplete - don't use */
				if (allNewCols)
					list_free_deep(allNewCols);
			}
			else
			{
				allNewPartns = lappend(allNewPartns, allNewCols);
			}

			if (!sqlRc)
				break;

			/* XXX: we're cheating here! */
			everyCnt->constvalue++;
		} /* end loop until exceed end */

	l_EveryLoopEnd:		
			
		pBSpec->everyGenList = allNewPartns;

		lc_prev = lc;
		lc = lnext(lc);
	} /* end foreach */


	return 1;
} /* end partition_range_every */



/*

transformPartitionBy

a clause like:

PARTITION BY col1 
  SUBPTN  BY col2 SUBTEMPLATE (Spec2), 
  SUBPTN  BY col3 SUBTEMPLATE (Spec3)
(Spec1)

becomes a chain of PartitionBy structs, each with a PartitionSpec:

  pBy -> (col1, spec1, NULL subspec)
   |
   v
  pBy -> (col2, spec2, NULL subspec)
   |
   v
  pBy -> (col3, spec3, NULL subspec)

This struct is easy to process recursively.  However, for the syntax:

PARTITION BY col1 
  SUBPTN  BY col2 
  SUBPTN  BY col3 
(
PTN AA (SUB CCC (SUB EE, SUB FF), SUB DDD (SUB EE, SUB FF))
PTN BB (SUB CCC (SUB EE, SUB FF), SUB DDD (SUB EE, SUB FF))
)

the struct is more like:

  pBy -> (col1, spec1, spec2->spec3)
   |
   v
  pBy -> (col2, NULL spec, NULL subspec)
   |
   v
  pBy -> (col3, NULL spec, NULL subspec)


We need to move the subpartition specifications to the correct spots.

*/

static void 
transformPartitionBy(ParseState *pstate, CreateStmtContext *cxt, 
					 CreateStmt *stmt, Node *partitionBy, GpPolicy *policy)
{
	CreateStmt	*child_tab_stmt;		
	RangeVar	*child_tab_name;
	RangeVar	*parent_tab_name;
	Oid			snamespaceid;
	char	   *snamespace;
	char	   *tab_name_str;
	int		  	partDepth;	/* depth (starting at zero, but display at 1) */  
	char	  	depthstr[4];
	char	   *at_depth = "";
	char	  	at_buf[NAMEDATALEN];
	int		  	partNumber = -1;	
	int		  	ii;
	List	   *partElts;
	ListCell   *lc = NULL;

	PartitionBy		*pBy;				/* the current partitioning clause */
	PartitionBy		*psubBy  = NULL;	/* child of current */

	if (NULL == partitionBy)
		return;

	pBy = (PartitionBy *)partitionBy;
	partDepth = pBy->partDepth;
	partDepth++;					/* increment depth for subpartition */

	if (partDepth > 99)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("too many subpartitions: partition depth > 99"
						 ),
				 parser_errposition(pstate, pBy->location)));
	}
	snprintf(depthstr, sizeof(depthstr), "%d", partDepth);

	if (partDepth != 1)
	{
		/* Only mention the depth 2 and greater to aid in debugging
		 * subpartitions
		 */
		snprintf(at_buf, sizeof(at_buf), 
				 " (at depth %d)", partDepth);
		at_depth = at_buf;
	}

	/* set the depth for the immediate subpartition */
	if (pBy->subPart)
	{
		/* recursively walk the subpartition clauses under PARTITION
		 * BY.  The SUBPARTITION TEMPLATE becomes the PartitionSpec of
		 * the new child partition.
		 */

		psubBy = (PartitionBy	*)pBy->subPart;
		psubBy->partDepth = partDepth;
	}

	/* derive the number of partitions from the PARTITIONS 
	 * clause (if it exists ) 
	 */
	if (pBy->partNum)
	{
		A_Const *con = (A_Const *)pBy->partNum;

		Assert(IsA(&con->val, Integer));

		partNumber = intVal(&con->val);

		if (pBy->partType != PARTTYP_HASH)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("PARTITIONS clause requires a HASH partition%s",
							at_depth),
					 parser_errposition(pstate, pBy->location)));
		}

		if (partNumber < 1)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("PARTITIONS number cannot be less than one%s",
							at_depth),
					 parser_errposition(pstate, pBy->location)));
		}
	} /* end partitions clause */

	pBy->keyCount = 0;

	if (pBy->keys)
	{
		List			*colElts = pBy->keys;

		pBy->keyCount = list_length(colElts);
	}

	/* 
	 * Find number of partitions in the specification, and match it up
	 * with the PARTITIONS clause if partitioned by HASH, and
	 * determine the subpartition specifications.  Perform basic error
	 * checking on boundary specifications.
	 */
	if (pBy->partSpec)
		partNumber = validate_partition_spec(pstate, cxt, stmt, pBy, at_depth, 
											 partNumber);

	if (partNumber < 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("no partitions specified at depth %d",
						partDepth),
				 parser_errposition(pstate, pBy->location)));
	}

	if (partNumber > 999999)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("too many partitions: partition number > 999999 at "
						"depth %d",
						partDepth),
				 parser_errposition(pstate, pBy->location)));
	}

	if (pBy->partType == PARTTYP_RANGE)	
	{
		int everyrc  = partition_range_every(pstate,cxt,stmt, pBy, at_depth);
		everyrc++;
	}


	/*
	 * Determine namespace and name to use for the child table.
	 *
	 * Although we use ChooseRelationName, it's not guaranteed
	 * that the selected name won't conflict.  EIT!
	 */

	snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
	snamespace = get_namespace_name(snamespaceid);

	/* set up the partition specification element list if it exists */
	if (pBy->partSpec)
	{
		PartitionSpec	*pSpec   = (PartitionSpec *)pBy->partSpec;
		PartitionSpec	*subSpec = (PartitionSpec *)pSpec->subSpec; 

		/* if the current specification has a subspec, then that is
		 * the spec for the child.  So we'd better have a SUBPARTITION
		 * BY clause for the child, and that clause cannot contain a
		 * SUBPARTITION TEMPLATE (which is a spec).  The subSpec
		 * becomes subBy's subpartition template spec, which is the
		 * partition specification for the child.
		 */

		if (subSpec)
		{
			if (NULL == psubBy)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("missing SUBPARTITION BY clause for "
								"subpartition specification%s",
								at_depth),
						 parser_errposition(pstate, pBy->location)));
			}
			if (psubBy && psubBy->partSpec)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("subpartition specification conflict%s",
								at_depth),
						 parser_errposition(pstate, psubBy->location)));
			}

			psubBy->partSpec = (Node *)subSpec;
		}

		partElts = pSpec->partElem;
		lc = list_head(partElts);
	}
	else
	{
		lc = NULL;
	}

	for (ii = 0; ii<partNumber; ii++)
	{
		int	  		 maxEveryNum = 1;
		int	  		 jj = 0;
		ListCell	*lc_anp = NULL; /* columns for generated EVERY */
			
		for ( ; jj < maxEveryNum; jj++)
		{
			char	  	 prtstr[NAMEDATALEN];			
			PartitionBy	*newSub = NULL;
			Node		*pRuleCatalog  = NULL;
			Node		*pPostCreate = NULL;
			Node		*pConstraint  = NULL;
			bool		 makeRule = false;
			PartitionElem *pElem = NULL;

			makeRule = false;

			if (lc)
			{
				PartitionBoundSpec	*pBSpec = NULL;

				pElem = (PartitionElem *)lfirst(lc);

				if (pElem)
					pBSpec = (PartitionBoundSpec *)pElem->boundSpec;

				/* check if have generated list of partitions from EVERY */
				if ((1 == maxEveryNum) && pBSpec && pBSpec->everyGenList)
				{
					/* set the maxEveryNum to (1 + list of "all new
					 * partitions").  Why?  Because for a range of a START
					 * and END, we create one table and one rule.  If we
					 * subdivide the range, we have 3 boundary values, but
					 * only 2 tables and 2 rules.  So for N boundaries we
					 * have N-1 tables and rules.  The total number of
					 * boundaries is 2 [start and end] plus the number of
					 * new partitions.  So just add one to the list
					 * length.
					*/

					maxEveryNum = list_length(pBSpec->everyGenList) + 1;
					lc_anp      = list_head(pBSpec->everyGenList);
				}

				if (pElem->partName)
				{
					/* use the partition name as part of the child
					 * table name */
					char *pName = strVal(pElem->partName);
					if (maxEveryNum == 1)
						snprintf(prtstr, sizeof(prtstr), "prt_%s", pName);
					else
						snprintf(prtstr, sizeof(prtstr), "prt_%s%d", pName, jj);
				}
				else
				{
					if (maxEveryNum == 1)
						snprintf(prtstr, sizeof(prtstr), "prt_%d", ii+1);
					else
						snprintf(prtstr, sizeof(prtstr), "prt_%d_%d", ii+1, jj);
				}

				if (pElem->subSpec)
				{
					/* if the current partition element has a subspec,
					 * then that is the spec for the child.  So we'd
					 * better have a SUBPARTITION BY clause for the child,
					 * and that clause cannot contain a SUBPARTITION
					 * TEMPLATE (which is a spec).  Note that we might
					 * have just updated the subBy partition spec with the
					 * subspec of the current spec, which would be a
					 * conflict.  If the psubBy has a null spec, we make a
					 * copy of the node and update it -- the subSpec
					 * becomes the specification for the child.  And if
					 * the subspec has a subspec it gets handled at the
					 * top of this loop as the subspec of the current spec
					 * when transformPartitionBy is re-invoked for the
					 * child table.
					*/

					if (NULL == psubBy)
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("missing SUBPARTITION BY clause "
										"for subpartition specification%s",
										at_depth),
								 parser_errposition(pstate, pElem->location)));
					}

					if (psubBy && psubBy->partSpec)
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("subpartition specification conflict%s",
										at_depth),
								 parser_errposition(pstate, psubBy->location)));
					}

					newSub = makeNode(PartitionBy);

					newSub->partType  = psubBy->partType;
					newSub->keys 	  = psubBy->keys;
					newSub->partNum	  = psubBy->partNum;
					newSub->subPart	  = psubBy->subPart;
					newSub->partSpec  = pElem->subSpec; /* use the subspec */
					newSub->partDepth = psubBy->partDepth;

					if (pElem->subSpec) /* get a good location for error msg */
					{
						/* use subspec location */
						newSub->location  = 
								((PartitionSpec *)pElem->subSpec)->location; 
					}
					else
					{
						newSub->location = 
								pElem->location; 
					}
				}

				makeRule = true; /* need child table name before make rule */

			} /* end lc */
			else
			{
				/* might not have partition spec for hash partitions -
				 * just name it using the loop number
				 */

				snprintf(prtstr, sizeof(prtstr), "prt_%d", ii+1);

				makeRule = true; /* need child table name before make rule */
			} /* end not lc */

			child_tab_name = makeNode(RangeVar);
			child_tab_name->catalogname = cxt->relation->catalogname;
			child_tab_name->schemaname  = cxt->relation->schemaname;

			tab_name_str = 
				ChooseRelationName(cxt->relation->relname,
								   depthstr, /* depth */
								   prtstr, 	/* object "type" = 
											   partition + (number or name) */
								   snamespaceid);

			ereport(NOTICE,
					(errmsg("%s will create child table \"%s\" for "
							"table \"%s\"",
							cxt->stmtType, tab_name_str,
							cxt->relation->relname)));

			child_tab_name->relname     = tab_name_str;

			parent_tab_name = makeNode(RangeVar);
			parent_tab_name->catalogname = cxt->relation->catalogname;
			parent_tab_name->schemaname  = cxt->relation->schemaname;
			parent_tab_name->relname     = cxt->relation->relname;

			/* now that we have the child table name, make the rule */

			if (makeRule)
			{
				ListCell *lc_rule = NULL;
				List *allRules =
						make_partition_rules(pstate,
											 cxt, stmt, 
											 partitionBy, pElem, at_depth, 
											 tab_name_str, ii+1, partNumber,
											 jj, maxEveryNum, &lc_anp);

				lc_rule = list_head(allRules);

				pRuleCatalog = lfirst(lc_rule);

				lc_rule = lnext(lc_rule);

				if (lc_rule)
				{
					pConstraint = lfirst(lc_rule);

					if (pConstraint)
					{
						StringInfoData   sid;
						Constraint *pCon = makeNode(Constraint);

						initStringInfo(&sid);

						appendStringInfo(&sid, "%s_%s", tab_name_str, "check");

						pCon->contype = CONSTR_CHECK;
						pCon->name = sid.data;
						pCon->raw_expr = pConstraint;
						pCon->cooked_expr = NULL;
						pCon->indexspace = NULL;

						pConstraint = (Node *)pCon;
					}

					/* look for a Rule statement to run after the
					 * relation is created 
					 * (see DefinePartitionedRelation in tablecmds.c)
					 */
					lc_rule = lnext(lc_rule);
					if (lc_rule)
					{
						List *pL1 = NULL;
						int  *pInt1;
						int  *pInt2;

						pInt1 = (int *)palloc(sizeof(int));
						pInt2 = (int *)palloc(sizeof(int));

						*pInt1 = ii+1;
						*pInt2 = jj;

						pL1 = list_make1(child_tab_name->relname); /* rule name */
						pL1 = lappend(pL1, pInt1);     /* partition position */
						pL1 = lappend(pL1, pInt2);     /* every position */

						if (pElem && pElem->partName)
						{
							char *pName = strVal(pElem->partName);
							
							pL1 = lappend(pL1, pName);
						}
						else
							pL1 = lappend(pL1, NULL);

						pL1 = lappend(pL1, child_tab_name->relname);  /* child name */
						pL1 = lappend(pL1, parent_tab_name->relname); /* parent name */

						pPostCreate = (Node *)list_make2(lfirst(lc_rule), pL1);
					}
				}

			}

			child_tab_stmt = makeNode(CreateStmt);
			child_tab_stmt->relation = child_tab_name;

			/* set the "Post Create" rule if it exists */
			child_tab_stmt->postCreate = pPostCreate; 

			/* shallow copy the parent list because we need to add
			 * constraints 
			 */
			child_tab_stmt->tableElts = list_copy(stmt->tableElts);

			if (pConstraint)
				child_tab_stmt->tableElts = 
						lappend(child_tab_stmt->tableElts,
								pConstraint);

			/* XXX XXX: inheriting the parent causes a headache in
			 * transformDistributedBy, since it assumes the parent exists
			 * already.  Just add an "ALTER TABLE...INHERIT parent" after
			 * the create child table
			 */
	/*		child_tab_stmt->inhRelations = list_make1(parent_tab_name); */
			child_tab_stmt->inhRelations = list_copy(stmt->inhRelations); 

			child_tab_stmt->constraints = list_copy(stmt->constraints);
			child_tab_stmt->options = stmt->options;
			child_tab_stmt->oncommit = stmt->oncommit;
			child_tab_stmt->tablespacename = stmt->tablespacename;
			child_tab_stmt->distributedBy = stmt->distributedBy;

			/* use the newSub as the partitionBy if the current 
			 * partition elem had an inline subpartition declaration 
			 */
			child_tab_stmt->partitionBy = (newSub)
					? (Node *) newSub : (Node *) psubBy;

			child_tab_stmt->oidInfo.relOid = 0;
			child_tab_stmt->oidInfo.comptypeOid = 0;
			child_tab_stmt->oidInfo.toastOid = 0;
			child_tab_stmt->oidInfo.toastIndexOid = 0;
			child_tab_stmt->oidInfo.toastComptypeOid = 0;
			child_tab_stmt->relKind = RELKIND_RELATION;
			child_tab_stmt->policy = 0;

			/* XXX XXX: it would be nice to copy the parent table's
			 * distribution policy if it has one.  Otherwise,
			 * transformDistributedBy will get annoyed that the parent
			 * table doesn't exist yet when it attempts to build the
			 * policy for the child table. However, transformCreateStmt
			 * will null out the policy, even if we supply it, so don't
			 * bother.  Add an "ALTER TABLE...INHERIT parent" after the
			 * create instead.
			 */
			if (0 && policy)
			{
				GpPolicy   *newpolicy = NULL;
				int			maxattrs  = 200;
				int			ii_attrs  = 0;

				newpolicy = 
					(GpPolicy *) palloc(
						sizeof(GpPolicy) + maxattrs * sizeof(newpolicy->attrs[0]));
				newpolicy->ptype  = policy->ptype;
				newpolicy->nattrs = policy->nattrs;

				for (ii_attrs = 0; ii_attrs < policy->nattrs; ii_attrs++)
				{
					newpolicy->attrs[ii_attrs] = policy->attrs[ii_attrs];
				}

				child_tab_stmt->policy = newpolicy;
			}

			cxt->alist = lappend(cxt->alist, child_tab_stmt);

			/* ALTER TABLE for inheritance after CREATE TABLE ... 

			   Could remove this code if inheritance work and pass copy of
			   parent policy to the create statement (and avoid
			   transformDistributedBy problems)

			*/
			if (1)
			{
				AlterTableCmd  *atc = makeNode(AlterTableCmd);
				AlterTableStmt *ats = makeNode(AlterTableStmt);

				/* alter table child inherits parent */
				atc->subtype = AT_AddInherit;
				atc->def = (Node *) parent_tab_name; 

				ats->relation = child_tab_name;
				ats->cmds 	  = list_make1((Node *)atc);
				ats->relkind  = OBJECT_TABLE;

				cxt->alist = lappend(cxt->alist, ats);
			}

			newSub = NULL; /* reset newSub */

			/* add a partitioning rule */
			/* XXX XXX: do not update partition_rule table for now */
			if (0 && pRuleCatalog)
			{
				cxt->alist = lappend(cxt->alist, pRuleCatalog);
			}
			pRuleCatalog  = NULL;
		} /* end jj < maxEverynum */
		if (lc)
		{
			lc = lnext(lc);
		}
	} /* end for ii */


} /* end transformPartitionBy */

#ifdef XXX_NEVER_WHERE_IS_THIS_NOW

/*
 * analyzeCreateSchemaStmt -
 *	  analyzes the "create schema" statement
 *
 * Split the schema element list into individual commands and place
 * them in the result list in an order such that there are no forward
 * references (e.g. GRANT to a table created later in the list). Note
 * that the logic we use for determining forward references is
 * presently quite incomplete.
 *
 * SQL92 also allows constraints to make forward references, so thumb through
 * the table columns and move forward references to a posterior alter-table
 * command.
 *
 * The result is a list of parse nodes that still need to be analyzed ---
 * but we can't analyze the later commands until we've executed the earlier
 * ones, because of possible inter-object references.
 *
 * Note: Called from commands/schemacmds.c
 */
List *
analyzeCreateSchemaStmt(CreateSchemaStmt *stmt)
{
	CreateSchemaStmtContext cxt;
	List	   *result;
	ListCell   *elements;

	cxt.stmtType = "CREATE SCHEMA";
	cxt.schemaname = stmt->schemaname;
	cxt.authid = stmt->authid;
	cxt.sequences = NIL;
	cxt.tables = NIL;
	cxt.views = NIL;
	cxt.indexes = NIL;
	cxt.grants = NIL;
	cxt.triggers = NIL;
	cxt.fwconstraints = NIL;
	cxt.alters = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;

	/*
	 * Run through each schema element in the schema element list. Separate
	 * statements by type, and do preliminary analysis.
	 */
	foreach(elements, stmt->schemaElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_CreateSeqStmt:
				{
					CreateSeqStmt *elp = (CreateSeqStmt *) element;

					setSchemaName(cxt.schemaname, &elp->sequence->schemaname);
					cxt.sequences = lappend(cxt.sequences, element);
				}
				break;

			case T_CreateStmt:
				{
					CreateStmt *elp = (CreateStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);

					/*
					 * XXX todo: deal with constraints
					 */
					cxt.tables = lappend(cxt.tables, element);
				}
				break;
/* XXX XXX handle this */
			case T_CreateExternalStmt:
				{
					CreateExternalStmt *elp = (CreateExternalStmt *) element;
					
					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					
					cxt.tables = lappend(cxt.tables, element);
				}
				break;
				
			case T_ViewStmt:
				{
					ViewStmt   *elp = (ViewStmt *) element;

					setSchemaName(cxt.schemaname, &elp->view->schemaname);

					/*
					 * XXX todo: deal with references between views
					 */
					cxt.views = lappend(cxt.views, element);
				}
				break;

			case T_IndexStmt:
				{
					IndexStmt  *elp = (IndexStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.indexes = lappend(cxt.indexes, element);
				}
				break;

			case T_CreateTrigStmt:
				{
					CreateTrigStmt *elp = (CreateTrigStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.triggers = lappend(cxt.triggers, element);
				}
				break;

			case T_GrantStmt:
				cxt.grants = lappend(cxt.grants, element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
		}
	}

	result = NIL;
	result = list_concat(result, cxt.sequences);
	result = list_concat(result, cxt.tables);
	result = list_concat(result, cxt.views);
	result = list_concat(result, cxt.indexes);
	result = list_concat(result, cxt.triggers);
	result = list_concat(result, cxt.grants);

	return result;
}

#endif XXX_NEVER_WHERE_IS_THIS_NOW

/*
 * transformSingleRowErrorHandling
 *
 * If Single row error handling was specified with an error table, we first
 * check if a table with the same name already exists. If yes, we verify that
 * it has the correct pre-defined error table metadata format (and throw an
 * error is it doesn't). If such a table doesn't exist, we add a CREATE TABLE
 * statement to the before list of the CREATE EXTERNAL TABLE so that it will
 * be created before the external table.
 */
static void
transformSingleRowErrorHandling(ParseState *pstate, CreateStmtContext *cxt,
								SingleRowErrorDesc *sreh)
{
	
	/* check if error relation exists already */
	Oid errreloid = RangeVarGetRelid(sreh->errtable, true);
	bool errrelexists = OidIsValid(errreloid);
	
	/*
	 * auto-generate a new error table or verify an existing one
	 */
	if(errrelexists)
	{
		/* 
		 * this table already exists in the database. Verify that specified 
		 * table is a valid error table 
		 */
		
		Relation rel;
				
		rel = heap_openrv(sreh->errtable, AccessShareLock);
		ValidateErrorTableMetaData(rel);
		relation_close(rel, AccessShareLock);
		
		sreh->reusing_existing_errtable = true;
	}
	else
	{
		/* 
		 * no such table. auto create it by adding a CREATE TABLE <errortable> 
		 * to the before list 
		 */

		CreateStmt *createStmt;
		List	   *attrList;
		int			i = 0;

		ereport(NOTICE, 
				(errmsg("Error table \"%s\" does not exist. Auto generating an "
						"error table with the same name", sreh->errtable->relname)));
		createStmt = makeNode(CreateStmt);
		
		/*
		 * create a list of ColumnDef nodes based on the names and types of the
		 * per-defined error table columns
		 */
		attrList = NIL;
		
		
		for (i = 1; i <= NUM_ERRORTABLE_ATTR; i++)
		{
			ColumnDef  *coldef = makeNode(ColumnDef);
			
			coldef->inhcount = 0;
			coldef->is_local = true;
			coldef->is_not_null = false;
			coldef->raw_default = NULL;
			coldef->cooked_default = NULL;
			coldef->constraints = NIL;
			
			switch (i)
			{
				case errtable_cmdtime:
					coldef->typname = makeTypeNameFromOid(TIMESTAMPTZOID, -1);
					coldef->colname = "cmdtime";
					break;
				case errtable_relname:
					coldef->typname = makeTypeNameFromOid(TEXTOID, -1);
					coldef->colname = "relname";
					break;
				case errtable_filename:
					coldef->typname = makeTypeNameFromOid(TEXTOID, -1);
					coldef->colname = "filename";
					break;
				case errtable_linenum:
					coldef->typname = makeTypeNameFromOid(INT4OID, -1);
					coldef->colname = "linenum";
					break;
				case errtable_bytenum:
					coldef->typname = makeTypeNameFromOid(INT4OID, -1);
					coldef->colname = "bytenum";
					break;
				case errtable_errmsg:
					coldef->typname = makeTypeNameFromOid(TEXTOID, -1);
					coldef->colname = "errmsg";
					break;
				case errtable_rawdata:
					coldef->typname = makeTypeNameFromOid(TEXTOID, -1);
					coldef->colname = "rawdata";
					break;
				case errtable_rawbytes:
					coldef->typname = makeTypeNameFromOid(BYTEAOID, -1);
					coldef->colname = "rawbytes";
					break;
			}
			attrList = lappend(attrList, coldef);
		}
		
		createStmt->relation = sreh->errtable;
		createStmt->tableElts = attrList;
		createStmt->inhRelations = NIL;
		createStmt->constraints = NIL;
		createStmt->options = NIL;
		createStmt->oncommit = ONCOMMIT_NOOP;
		createStmt->tablespacename = NULL;
		createStmt->relKind = RELKIND_RELATION;
		createStmt->distributedBy = list_make1(NULL); /* DISTRIBUTED RANDOMLY */
		
		
		cxt->blist = lappend(cxt->blist, createStmt);
		
		sreh->reusing_existing_errtable = false;
		
	}
	
}
