/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  transform the parse tree into a query tree
 *
 * Portions Copyright (c) 2005-2006, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$PostgreSQL: pgsql/src/backend/parser/analyze.c,v 1.353 2006/10/11 16:42:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/prepare.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbhash.h"
#include "cdb/cdbsreh.h"

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
	List	   *fwconstraints;	/* Forward referencing FOREIGN KEY constraints */
	List	   *alters;			/* Generated ALTER items (from the above) */
	List	   *ixconstraints;	/* index-creating constraints */
	List	   *blist;			/* "before list" of things to do before
								 * creating the schema */
	List	   *alist;			/* "after list" of things to do after creating
								 * the schema */
} CreateSchemaStmtContext;

/* State shared by transformCreateStmt and its subroutines */
typedef struct
{
	const char *stmtType;		/* "CREATE TABLE" or "ALTER TABLE" */
	RangeVar   *relation;		/* relation to create */
	List	   *inhRelations;	/* relations to inherit from */
	bool		hasoids;		/* does relation have an OID column? */
	bool		isalter;		/* true if altering existing table */
	List	   *columns;		/* ColumnDef items */
	List	   *ckconstraints;	/* CHECK constraints */
	List	   *fkconstraints;	/* FOREIGN KEY constraints */
	List	   *ixconstraints;	/* index-creating constraints */
	List	   *blist;			/* "before list" of things to do before
								 * creating the table */
	List	   *alist;			/* "after list" of things to do after creating
								 * the table */
	IndexStmt  *pkey;			/* PRIMARY KEY index, if any */
} CreateStmtContext;

typedef struct
{
	Oid		   *paramTypes;
	int			numParams;
} check_parameter_resolution_context;

typedef struct
{
	AttrNumber subattno; /* current attno for sub query */
	List	  *subtlist; /* target list for sub query */
} grouped_window_ctx;

typedef struct
{
	Index *sortref;
	Index size;
	int level;
	int levelsup;
} sortref_ctx;

static List *do_parse_analyze(Node *parseTree, ParseState *pstate);
static Query *transformStmt(ParseState *pstate, Node *stmt,
			  List **extras_before, List **extras_after);
static Query *transformViewStmt(ParseState *pstate, ViewStmt *stmt,
				  List **extras_before, List **extras_after);
static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, InsertStmt *stmt,
					List **extras_before, List **extras_after);
static List *transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos);
static List *transformReturningList(ParseState *pstate, List *returningList);
static Query *transformIndexStmt(ParseState *pstate, IndexStmt *stmt);
static Query *transformRuleStmt(ParseState *query, RuleStmt *stmt,
				  List **extras_before, List **extras_after);
static Query *transformSelectStmt(ParseState *pstate, SelectStmt *stmt);
static Query *transformValuesClause(ParseState *pstate, SelectStmt *stmt);
static Query *transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt);
static Node *transformSetOperationTree(ParseState *pstate, SelectStmt *stmt);
static Query *transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt);
static Query *transformDeclareCursorStmt(ParseState *pstate,
						   DeclareCursorStmt *stmt);
static Query *transformPrepareStmt(ParseState *pstate, PrepareStmt *stmt);
static Query *transformExecuteStmt(ParseState *pstate, ExecuteStmt *stmt);
static Query *transformCreateStmt(ParseState *pstate, CreateStmt *stmt,
					List **extras_before, List **extras_after);
static Query *transformCreateExternalStmt(ParseState *pstate, CreateExternalStmt *stmt,
										  List **extras_before, List **extras_after);
static Query *transformAlterTableStmt(ParseState *pstate, AlterTableStmt *stmt,
						List **extras_before, List **extras_after);
static void transformColumnDefinition(ParseState *pstate,
						  CreateStmtContext *cxt,
						  ColumnDef *column);
static void transformTableConstraint(ParseState *pstate,
						 CreateStmtContext *cxt,
						 Constraint *constraint);
static void transformInhRelation(ParseState *pstate, CreateStmtContext *cxt,
					 InhRelation *inhrelation);
static void transformIndexConstraints(ParseState *pstate,
						  CreateStmtContext *cxt);
static void transformDistributedBy(ParseState *pstate,
		   CreateStmtContext *cxt, List *distributedBy, GpPolicy ** policy, List *likeDistributedBy);
static void transformPartitionBy(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, GpPolicy *policy);

static void transformFKConstraints(ParseState *pstate,
					   CreateStmtContext *cxt,
					   bool skipValidation,
					   bool isAddConstraint);
static void applyColumnNames(List *dst, List *src);
static void getSetColTypes(ParseState *pstate, Node *node,
			   List **colTypes, List **colTypmods);
static void transformLockingClause(Query *qry, LockingClause *lc);
static void transformConstraintAttrs(List *constraintList);
static void transformColumnType(ParseState *pstate, ColumnDef *column);
static void release_pstate_resources(ParseState *pstate);
static FromExpr *makeFromExpr(List *fromlist, Node *quals);
static bool check_parameter_resolution_walker(Node *node,
								check_parameter_resolution_context *context);

static void setQryDistributionPolicy(SelectStmt *stmt, Query *qry);
static void transformSingleRowErrorHandling(ParseState *pstate, CreateStmtContext *cxt,
											SingleRowErrorDesc *sreh);

static Node *make_partition_rules(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
 		 char *at_depth, char *child_name_str);

static int deparse_partition_rule(Node *pNode, char *outbuf, size_t outsize);

/*
 * parse_analyze
 *		Analyze a raw parse tree and transform it to Query form.
 *
 * If available, pass the source text from which the raw parse tree was
 * generated; it's OK to pass NULL if this is not available.
 *
 * Optionally, information about $n parameter types can be supplied.
 * References to $n indexes not defined by paramTypes[] are disallowed.
 *
 * The result is a List of Query nodes (we need a list since some commands
 * produce multiple Queries).  Optimizable statements require considerable
 * transformation, while many utility-type statements are simply hung off
 * a dummy CMD_UTILITY Query node.
 */
List *
parse_analyze(Node *parseTree, const char *sourceText,
			  Oid *paramTypes, int numParams)
{
	ParseState *pstate = make_parsestate(NULL);
	List	   *result;

	pstate->p_sourcetext = sourceText;
	pstate->p_paramtypes = paramTypes;
	pstate->p_numparams = numParams;
	pstate->p_variableparams = false;

	result = do_parse_analyze(parseTree, pstate);

	pfree(pstate);

	return result;
}

/*
 * parse_analyze_varparams
 *
 * This variant is used when it's okay to deduce information about $n
 * symbol datatypes from context.  The passed-in paramTypes[] array can
 * be modified or enlarged (via repalloc).
 */
List *
parse_analyze_varparams(Node *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams)
{
	ParseState *pstate = make_parsestate(NULL);
	List	   *result;

	pstate->p_sourcetext = sourceText;
	pstate->p_paramtypes = *paramTypes;
	pstate->p_numparams = *numParams;
	pstate->p_variableparams = true;

	result = do_parse_analyze(parseTree, pstate);

	*paramTypes = pstate->p_paramtypes;
	*numParams = pstate->p_numparams;

	pfree(pstate);

	/* make sure all is well with parameter types */
	if (*numParams > 0)
	{
		check_parameter_resolution_context context;

		context.paramTypes = *paramTypes;
		context.numParams = *numParams;
		check_parameter_resolution_walker((Node *) result, &context);
	}

	return result;
}

/*
 * parse_sub_analyze
 *		Entry point for recursively analyzing a sub-statement.
 */
List *
parse_sub_analyze(Node *parseTree, ParseState *parentParseState)
{
	ParseState *pstate = make_parsestate(parentParseState);
	List	   *result;

	result = do_parse_analyze(parseTree, pstate);

	pfree(pstate);

	return result;
}

/*
 * do_parse_analyze
 *		Workhorse code shared by the above variants of parse_analyze.
 */
static List *
do_parse_analyze(Node *parseTree, ParseState *pstate)
{
	List	   *result = NIL;

	/* Lists to return extra commands from transformation */
	List	   *extras_before = NIL;
	List	   *extras_after = NIL;
	Query	   *query;
	ListCell   *l;

	query = transformStmt(pstate, parseTree, &extras_before, &extras_after);

	/* don't need to access result relation any more */
	release_pstate_resources(pstate);

	foreach(l, extras_before)
		result = list_concat(result, parse_sub_analyze(lfirst(l), pstate));

	result = lappend(result, query);

	foreach(l, extras_after)
		result = list_concat(result, parse_sub_analyze(lfirst(l), pstate));

	/*
	 * Make sure that only the original query is marked original. We have to
	 * do this explicitly since recursive calls of do_parse_analyze will have
	 * marked some of the added-on queries as "original".  Also mark only the
	 * original query as allowed to set the command-result tag.
	 */
	foreach(l, result)
	{
		Query	   *q = lfirst(l);

		if (q == query)
		{
			q->querySource = QSRC_ORIGINAL;
			q->canSetTag = true;
		}
		else
		{
			q->querySource = QSRC_PARSER;
			q->canSetTag = false;
		}
	}

	return result;
}

static void
release_pstate_resources(ParseState *pstate)
{
	if (pstate->p_target_relation != NULL)
		heap_close(pstate->p_target_relation, NoLock);
	pstate->p_target_relation = NULL;
	pstate->p_target_rangetblentry = NULL;
}

/*
 * transformStmt -
 *	  transform a Parse tree into a Query tree.
 */
static Query *
transformStmt(ParseState *pstate, Node *parseTree,
			  List **extras_before, List **extras_after)
{
	Query	   *result = NULL;

	switch (nodeTag(parseTree))
	{
			/*
			 * Non-optimizable statements
			 */
		case T_CreateStmt:
			result = transformCreateStmt(pstate, (CreateStmt *) parseTree,
										 extras_before, extras_after);
			break;

		case T_CreateExternalStmt:
			result = transformCreateExternalStmt(pstate, (CreateExternalStmt *) parseTree,
												 extras_before, extras_after);
			break;

		case T_IndexStmt:
			result = transformIndexStmt(pstate, (IndexStmt *) parseTree);
			break;

		case T_RuleStmt:
			result = transformRuleStmt(pstate, (RuleStmt *) parseTree,
									   extras_before, extras_after);
			break;

		case T_ViewStmt:
			result = transformViewStmt(pstate, (ViewStmt *) parseTree,
									   extras_before, extras_after);
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *n = (ExplainStmt *) parseTree;

				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				n->query = transformStmt(pstate, (Node *) n->query,
										 extras_before, extras_after);
				result->utilityStmt = (Node *) parseTree;
			}
			break;

		case T_CopyStmt:
			{
				CopyStmt   *n = (CopyStmt *) parseTree;

				/*
				 * Check if we need to create an error table. If so, add it to the 
				 * before list.
				 */
				if(n->sreh && ((SingleRowErrorDesc *)n->sreh)->errtable)
				{					
					CreateStmtContext cxt;
					cxt.blist = NIL;
					cxt.alist = NIL;

					
					transformSingleRowErrorHandling(pstate, &cxt,
													(SingleRowErrorDesc *)n->sreh);
					*extras_before = list_concat(*extras_before, cxt.blist);
				}
				
				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				if (n->query)
					n->query = transformStmt(pstate, (Node *) n->query,
											 extras_before, extras_after);
				result->utilityStmt = (Node *) parseTree;
			}
			break;

		case T_AlterTableStmt:
			result = transformAlterTableStmt(pstate,
											 (AlterTableStmt *) parseTree,
											 extras_before, extras_after);
			break;

		case T_PrepareStmt:
			result = transformPrepareStmt(pstate, (PrepareStmt *) parseTree);
			break;

		case T_ExecuteStmt:
			result = transformExecuteStmt(pstate, (ExecuteStmt *) parseTree);
			break;

			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
			result = transformInsertStmt(pstate, (InsertStmt *) parseTree,
										 extras_before, extras_after);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_UpdateStmt:
			result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
			break;

		case T_SelectStmt:
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
					result = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE)
					result = transformSelectStmt(pstate, n);
				else
					result = transformSetOperationStmt(pstate, n);
			}
			break;

		case T_DeclareCursorStmt:
			result = transformDeclareCursorStmt(pstate,
											(DeclareCursorStmt *) parseTree);
			break;

		default:

			/*
			 * other statements don't require any transformation-- just return
			 * the original parsetree, yea!
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}

	/* Mark as original query until we learn differently */
	result->querySource = QSRC_ORIGINAL;
	result->canSetTag = true;

	/*
	 * Check that we did not produce too many resnos; at the very least we
	 * cannot allow more than 2^16, since that would exceed the range of a
	 * AttrNumber. It seems safest to use MaxTupleAttributeNumber.
	 */
	if (pstate->p_next_resno - 1 > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("target lists can have at most %d entries",
						MaxTupleAttributeNumber)));

	return result;
}

static Query *
transformViewStmt(ParseState *pstate, ViewStmt *stmt,
				  List **extras_before, List **extras_after)
{
	Query	   *result = makeNode(Query);

	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	stmt->query = transformStmt(pstate, (Node *) stmt->query,
								extras_before, extras_after);

	/*
	 * If a list of column names was given, run through and insert these into
	 * the actual query tree. - thomas 2000-03-08
	 *
	 * Outer loop is over targetlist to make it easier to skip junk targetlist
	 * entries.
	 */
	if (stmt->aliases != NIL)
	{
		ListCell   *alist_item = list_head(stmt->aliases);
		ListCell   *targetList;

		foreach(targetList, stmt->query->targetList)
		{
			TargetEntry *te = (TargetEntry *) lfirst(targetList);

			Assert(IsA(te, TargetEntry));
			/* junk columns don't get aliases */
			if (te->resjunk)
				continue;
			te->resname = pstrdup(strVal(lfirst(alist_item)));
			alist_item = lnext(alist_item);
			if (alist_item == NULL)
				break;			/* done assigning aliases */
		}

		if (alist_item != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("CREATE VIEW specifies more column "
							"names than columns")));
	}

	return result;
}

/*
 * transformDeleteStmt -
 *	  transforms a Delete Statement
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;

	qry->commandType = CMD_DELETE;

	/* set up range table with just the result rel */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
								  interpretInhOption(stmt->relation->inhOpt),
										 true,
										 ACL_DELETE);

	qry->distinctClause = NIL;

	/*
	 * The USING clause is non-standard SQL syntax, and is equivalent in
	 * functionality to the FROM list that can be specified for UPDATE. The
	 * USING keyword is used rather than FROM because FROM is already a
	 * keyword in the DELETE syntax.
	 */
	transformFromClause(pstate, stmt->usingClause);

	qual = transformWhereClause(pstate, stmt->whereClause, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * transformInsertStmt -
 *	  transform an Insert Statement
 */
static Query *
transformInsertStmt(ParseState *pstate, InsertStmt *stmt,
					List **extras_before, List **extras_after)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
	List	   *exprList = NIL;
	bool		isGeneralSelect;
	List	   *sub_rtable;
	List	   *sub_relnamespace;
	List	   *sub_varnamespace;
	List	   *icolumns;
	List	   *attrnos;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	ListCell   *icols;
	ListCell   *attnos;
	ListCell   *lc;

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/*
	 * We have three cases to deal with: DEFAULT VALUES (selectStmt == NULL),
	 * VALUES list, or general SELECT input.  We special-case VALUES, both for
	 * efficiency and so we can handle DEFAULT specifications.
	 */
	isGeneralSelect = (selectStmt && selectStmt->valuesLists == NIL);

	/*
	 * If a non-nil rangetable/namespace was passed in, and we are doing
	 * INSERT/SELECT, arrange to pass the rangetable/namespace down to the
	 * SELECT.	This can only happen if we are inside a CREATE RULE, and in
	 * that case we want the rule's OLD and NEW rtable entries to appear as
	 * part of the SELECT's rtable, not as outer references for it.  (Kluge!)
	 * The SELECT's joinlist is not affected however.  We must do this before
	 * adding the target table to the INSERT's rtable.
	 */
	if (isGeneralSelect)
	{
		sub_rtable = pstate->p_rtable;
		pstate->p_rtable = NIL;
		sub_relnamespace = pstate->p_relnamespace;
		pstate->p_relnamespace = NIL;
		sub_varnamespace = pstate->p_varnamespace;
		pstate->p_varnamespace = NIL;
	}
	else
	{
		sub_rtable = NIL;		/* not used, but keep compiler quiet */
		sub_relnamespace = NIL;
		sub_varnamespace = NIL;
	}

	/*
	 * Must get write lock on INSERT target table before scanning SELECT, else
	 * we will grab the wrong kind of initial lock if the target table is also
	 * mentioned in the SELECT part.  Note that the target table is not added
	 * to the joinlist or namespace.
	 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false, false, ACL_INSERT);

	/* Validate stmt->cols list, or build default list if no list given */
	icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
	Assert(list_length(icolumns) == list_length(attrnos));

	/*
	 * Determine which variant of INSERT we have.
	 */
	if (selectStmt == NULL)
	{
		/*
		 * We have INSERT ... DEFAULT VALUES.  We can handle this case by
		 * emitting an empty targetlist --- all columns will be defaulted when
		 * the planner expands the targetlist.
		 */
		exprList = NIL;
	}
	else if (isGeneralSelect)
	{
		/*
		 * We make the sub-pstate a child of the outer pstate so that it can
		 * see any Param definitions supplied from above.  Since the outer
		 * pstate's rtable and namespace are presently empty, there are no
		 * side-effects of exposing names the sub-SELECT shouldn't be able to
		 * see.
		 */
		ParseState *sub_pstate = make_parsestate(pstate);
		Query	   *selectQuery;

		/*
		 * Process the source SELECT.
		 *
		 * It is important that this be handled just like a standalone SELECT;
		 * otherwise the behavior of SELECT within INSERT might be different
		 * from a stand-alone SELECT. (Indeed, Postgres up through 6.5 had
		 * bugs of just that nature...)
		 */
		sub_pstate->p_rtable = sub_rtable;
		sub_pstate->p_relnamespace = sub_relnamespace;
		sub_pstate->p_varnamespace = sub_varnamespace;

		/*
		 * Note: we are not expecting that extras_before and extras_after are
		 * going to be used by the transformation of the SELECT statement.
		 */
		selectQuery = transformStmt(sub_pstate, stmt->selectStmt,
									extras_before, extras_after);

		release_pstate_resources(sub_pstate);
		pfree(sub_pstate);

		Assert(IsA(selectQuery, Query));
		Assert(selectQuery->commandType == CMD_SELECT);
		if (selectQuery->into)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("INSERT ... SELECT may not specify INTO")));

		/*
		 * Make the source be a subquery in the INSERT's rangetable, and add
		 * it to the INSERT's joinlist.
		 */
		rte = addRangeTableEntryForSubquery(pstate,
											selectQuery,
											makeAlias("*SELECT*", NIL),
											false);
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

		/*----------
		 * Generate an expression list for the INSERT that selects all the
		 * non-resjunk columns from the subquery.  (INSERT's tlist must be
		 * separate from the subquery's tlist because we may add columns,
		 * insert datatype coercions, etc.)
		 *
		 * HACK: unknown-type constants and params in the SELECT's targetlist
		 * are copied up as-is rather than being referenced as subquery
		 * outputs.  This is to ensure that when we try to coerce them to
		 * the target column's datatype, the right things happen (see
		 * special cases in coerce_type).  Otherwise, this fails:
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *----------
		 */
		exprList = NIL;
		foreach(lc, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (tle->expr &&
				(IsA(tle->expr, Const) ||IsA(tle->expr, Param)) &&
				exprType((Node *) tle->expr) == UNKNOWNOID)
				expr = tle->expr;
			else
				expr = (Expr *) makeVar(rtr->rtindex,
										tle->resno,
										exprType((Node *) tle->expr),
										exprTypmod((Node *) tle->expr),
										0);
			exprList = lappend(exprList, expr);
		}

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos);
	}
	else if (list_length(selectStmt->valuesLists) > 1)
	{
		/*
		 * Process INSERT ... VALUES with multiple VALUES sublists. We
		 * generate a VALUES RTE holding the transformed expression lists, and
		 * build up a targetlist containing Vars that reference the VALUES
		 * RTE.
		 */
		List	   *exprsLists = NIL;
		int			sublist_length = -1;

		foreach(lc, selectStmt->valuesLists)
		{
			List	   *sublist = (List *) lfirst(lc);

			/* Do basic expression transformation (same as a ROW() expr) */
			sublist = transformExpressionList(pstate, sublist);

			/*
			 * All the sublists must be the same length, *after*
			 * transformation (which might expand '*' into multiple items).
			 * The VALUES RTE can't handle anything different.
			 */
			if (sublist_length < 0)
			{
				/* Remember post-transformation length of first sublist */
				sublist_length = list_length(sublist);
			}
			else if (sublist_length != list_length(sublist))
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("VALUES lists must all be the same length")));
			}

			/* Prepare row for assignment to target table */
			sublist = transformInsertRow(pstate, sublist,
										 stmt->cols,
										 icolumns, attrnos);

			exprsLists = lappend(exprsLists, sublist);
		}

		/*
		 * There mustn't have been any table references in the expressions,
		 * else strange things would happen, like Cartesian products of those
		 * tables with the VALUES list ...
		 */
		if (pstate->p_joinlist != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("VALUES must not contain table references")));

		/*
		 * Another thing we can't currently support is NEW/OLD references in
		 * rules --- seems we'd need something like SQL99's LATERAL construct
		 * to ensure that the values would be available while evaluating the
		 * VALUES RTE.	This is a shame.  FIXME
		 */
		if (list_length(pstate->p_rtable) != 1 &&
			contain_vars_of_level((Node *) exprsLists, 0))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("VALUES must not contain OLD or NEW references"),
					 errhint("Use SELECT ... UNION ALL ... instead.")));

		/*
		 * Generate the VALUES RTE
		 */
		rte = addRangeTableEntryForValues(pstate, exprsLists, NULL, true);
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

		/*
		 * Generate list of Vars referencing the RTE
		 */
		expandRTE(rte, rtr->rtindex, 0, false, NULL, &exprList);
	}
	else
	{
		/*----------
		 * Process INSERT ... VALUES with a single VALUES sublist.
		 * We treat this separately for efficiency and for historical
		 * compatibility --- specifically, allowing table references,
		 * such as
		 *			INSERT INTO foo VALUES(bar.*)
		 *
		 * The sublist is just computed directly as the Query's targetlist,
		 * with no VALUES RTE.	So it works just like SELECT without FROM.
		 *----------
		 */
		List	   *valuesLists = selectStmt->valuesLists;

		Assert(list_length(valuesLists) == 1);

		/* Do basic expression transformation (same as a ROW() expr) */
		exprList = transformExpressionList(pstate,
										   (List *) linitial(valuesLists));

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos);
	}

	/*
	 * Generate query's target list using the computed list of expressions.
	 */
	qry->targetList = NIL;
	icols = list_head(icolumns);
	attnos = list_head(attrnos);
	foreach(lc, exprList)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col;
		TargetEntry *tle;

		col = (ResTarget *) lfirst(icols);
		Assert(IsA(col, ResTarget));

		tle = makeTargetEntry(expr,
							  (AttrNumber) lfirst_int(attnos),
							  col->name,
							  false);
		qry->targetList = lappend(qry->targetList, tle);

		icols = lnext(icols);
		attnos = lnext(attnos);
	}

	/*
	 * If we have a RETURNING clause, we need to add the target relation to
	 * the query namespace before processing it, so that Var references in
	 * RETURNING will work.  Also, remove any namespace entries added in a
	 * sub-SELECT or VALUES list.
	 */
	if (stmt->returningList)
	{
		pstate->p_relnamespace = NIL;
		pstate->p_varnamespace = NIL;
		addRTEtoQuery(pstate, pstate->p_target_rangetblentry,
					  false, true, true);
		qry->returningList = transformReturningList(pstate,
													stmt->returningList);
	}

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in VALUES")));
	if (pstate->p_hasWindFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use window function in VALUES")));

	return qry;
}

/*
 * Prepare an INSERT row for assignment to the target table.
 *
 * The row might be either a VALUES row, or variables referencing a
 * sub-SELECT output.
 */
static List *
transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos)
{
	List	   *result;
	ListCell   *lc;
	ListCell   *icols;
	ListCell   *attnos;

	/*
	 * Check length of expr list.  It must not have more expressions than
	 * there are target columns.  We allow fewer, but only if no explicit
	 * columns list was given (the remaining columns are implicitly
	 * defaulted).	Note we must check this *after* transformation because
	 * that could expand '*' into multiple items.
	 */
	if (list_length(exprlist) > list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more expressions than target columns")));
	if (stmtcols != NIL &&
		list_length(exprlist) < list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more target columns than expressions")));

	/*
	 * Prepare columns for assignment to target table.
	 */
	result = NIL;
	icols = list_head(icolumns);
	attnos = list_head(attrnos);
	foreach(lc, exprlist)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col;

		col = (ResTarget *) lfirst(icols);
		Assert(IsA(col, ResTarget));

		expr = transformAssignedExpr(pstate, expr,
									 col->name,
									 lfirst_int(attnos),
									 col->indirection,
									 col->location);

		result = lappend(result, expr);

		icols = lnext(icols);
		attnos = lnext(attnos);
	}

	return result;
}

/*
 * transformCreateStmt -
 *	  transforms the "create table" statement
 *	  SQL92 allows constraints to be scattered all over, so thumb through
 *	   the columns and collect all constraints into one place.
 *	  If there are any implied indices (e.g. UNIQUE or PRIMARY KEY)
 *	   then expand those into multiple IndexStmt blocks.
 *	  - thomas 1997-12-02
 */
static Query *
transformCreateStmt(ParseState *pstate, CreateStmt *stmt,
					List **extras_before, List **extras_after)
{
	CreateStmtContext cxt;
	Query	   *q;
	ListCell   *elements;
	List  	   *likeDistributedBy = NIL;

	cxt.stmtType = "CREATE TABLE";
	cxt.relation = stmt->relation;
	cxt.inhRelations = stmt->inhRelations;
	cxt.isalter = false;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
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
					 * For Greenplum Database distributed tables, default to the same distribution as the first LIKE table,
					 * unless we also have INHERITS
					 */
					if (Gp_role == GP_ROLE_DISPATCH && stmt->distributedBy == NIL && stmt->inhRelations == NIL && isBeginning && stmt->policy == NULL)
					{
						InhRelation *e = (InhRelation *) element;
						Oid			relId = RangeVarGetRelid(e->relation, false);

						GpPolicy  *oldTablePolicy = GpPolicyFetch(CurrentMemoryContext, relId);

						if (oldTablePolicy != NULL && oldTablePolicy->ptype == POLICYTYPE_PARTITIONED)
						{
							int ia;
					
							for (ia=0;ia<oldTablePolicy->nattrs;ia++)
							{
								char * attname = get_attname(relId, oldTablePolicy->attrs[ia]);
								if (likeDistributedBy)
										likeDistributedBy = lappend(likeDistributedBy, (Node *) makeString(attname));
									else
										likeDistributedBy = list_make1((Node *) makeString(attname));
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
	 * statements, so transfer anything we already have into extras_after
	 * immediately.
	 */
	*extras_after = list_concat(cxt.alist, *extras_after);
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
	q = makeNode(Query);
	q->commandType = CMD_UTILITY;
	q->utilityStmt = (Node *) stmt;
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;
	*extras_before = list_concat(*extras_before, cxt.blist);
	*extras_after = list_concat(cxt.alist, *extras_after);

	return q;
}

static Query *
transformCreateExternalStmt(ParseState *pstate, CreateExternalStmt *stmt,
							List **extras_before, List **extras_after)
{
	CreateStmtContext cxt;
	Query	   *q;
	ListCell   *elements;
	
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
	q = makeNode(Query);
	q->commandType = CMD_UTILITY;
	q->utilityStmt = (Node *) stmt;
	stmt->tableElts = cxt.columns;
	*extras_before = list_concat(*extras_before, cxt.blist);
	*extras_after = list_concat(cxt.alist, *extras_after);
	
	return q;
}

static void
transformColumnDefinition(ParseState *pstate, CreateStmtContext *cxt,
						  ColumnDef *column)
{
	bool		is_serial;
	bool		saw_nullable;
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
				if (column->raw_default != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
								  column->colname, cxt->relation->relname)));
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
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
 * Change the LIKE <subtable> portion of a CREATE TABLE statement into the
 * column definitions which recreate the user defined column portions of <subtable>.
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

	if (including_indexes)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE INCLUDING INDEXES is not implemented")));

	/*
	 * Insert the copied attributes into the cxt for the new table
	 * definition.
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
	 * Copy CHECK constraints if requested, being careful to adjust
	 * attribute numbers
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
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.	That will prevent someone else from deleting or ALTERing the
	 * parent before the child is committed.
	 */
	heap_close(relation, NoLock);
}

static void
transformIndexConstraints(ParseState *pstate, CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *indexlist = NIL;
	ListCell   *listptr;
	ListCell   *l;

	/*
	 * Run through the constraints that need to generate an index. For PRIMARY
	 * KEY, mark each column as NOT NULL and create an index. For UNIQUE,
	 * create an index as for PRIMARY KEY, but do not insist on NOT NULL.
	 */
	foreach(listptr, cxt->ixconstraints)
	{
		Constraint *constraint = lfirst(listptr);
		ListCell   *keys;
		IndexElem  *iparam;

		Assert(IsA(constraint, Constraint));
		Assert((constraint->contype == CONSTR_PRIMARY)
			   || (constraint->contype == CONSTR_UNIQUE));

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
			index->idxname = NULL;		/* DefineIndex will choose name */

		index->relation = cxt->relation;
		index->accessMethod = DEFAULT_INDEX_TYPE;
		index->options = constraint->options;
		index->tableSpace = constraint->indexspace;
		index->indexParams = NIL;
		index->whereClause = NULL;
		index->concurrent = false;

		/*
		 * Make sure referenced keys exist.  If we are making a PRIMARY KEY
		 * index, also make sure they are NOT NULL, if possible. (Although we
		 * could leave it to DefineIndex to mark the columns NOT NULL, it's
		 * more efficient to get it right the first time.)
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
				 * column will be a system column in the new table, so accept
				 * it.	System columns can't ever be null, so no need to worry
				 * about PRIMARY/NOT NULL constraint.
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
							 * We currently have no easy way to force an
							 * inherited column to be NOT NULL at creation, if
							 * its parent wasn't so already. We leave it to
							 * DefineIndex to fix things up in this case.
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
			 * created in the command; they may well exist already.
			 * DefineIndex will complain about them if not, and will also take
			 * care of marking them NOT NULL.
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
			index->indexParams = lappend(index->indexParams, iparam);
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

	foreach(l, indexlist)
	{
		bool		keep = true;
		ListCell   *k;

		index = lfirst(l);

		/* if it's pkey, it's already in cxt->alist */
		if (index == cxt->pkey)
			continue;

		foreach(k, cxt->alist)
		{
			IndexStmt  *priorindex = lfirst(k);

			if (equal(index->indexParams, priorindex->indexParams))
			{
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
	
					typeOid = typenameTypeId(NULL, column->typname);
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
						Oid typeOid = typenameTypeId(NULL, column->typname);
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

static int deparse_partition_rule(Node *pNode, char *outbuf, size_t outsize)
{
	if (!pNode)
		return 0;

	switch (nodeTag(pNode))
	{
		case T_Value:
			break;
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

static Node *make_partition_rules(ParseState *pstate,
		 CreateStmtContext *cxt, CreateStmt *stmt, 
		 Node *partitionBy, PartitionElem *pElem,
 		 char  *at_depth, char *child_name_str
		)
{
	PartitionBy			*pBy    = (PartitionBy *)partitionBy;
	PartitionBoundSpec	*pBSpec = NULL;
	Node				*pRule  = NULL;
	InsertStmt			*pIns   = NULL;
	RangeVar			*parent_tab_name;
	RangeVar			*child_tab_name;

	if (!pElem)
		return NULL;

	pBSpec = (PartitionBoundSpec *)pElem->boundSpec;

	if (pBy->partType == PARTTYP_LIST)	
	{
		Node			*pIndAND = NULL;
		Node			*pIndOR  = NULL;
		List			*colElts = pBy->keys;
		ListCell		*lc      = NULL;
		List			*valElts = pBSpec->partStart;
		ListCell		*lc_val  = NULL;
		char			 exprBuf[10000];
		char			 ANDBuf[10000];
		char			 ORBuf[10000];
		char			 ruleBuf[10000];

		exprBuf[0] = '0';
		ANDBuf[0]  = '0';
		ORBuf[0]   = '0';

		Assert(pBSpec->partStart);

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

				if (NULL == lc_val)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("Mismatched columns for VALUES%s",
									at_depth),
							 parser_errposition(pstate, pBSpec->location)));
				}

				/* equality expression: column = value */
				pEq = (Node *) makeSimpleA_Expr(AEXPR_OP, "=", 
												pCol,
												lfirst(lc_val),
												-1 /* position */);
				pColConst = (Value *)pCol;
				pValConst = (A_Const *)lfirst(lc_val);

				Assert(pColConst->type == T_String);

				if (pValConst->type != T_A_Const)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("Invalid use of non-constant type in boundary specification%s",
									at_depth),
							 parser_errposition(pstate, pBSpec->location)));
						
				}


				if (!deparse_partition_rule((Node *)pEq, 
											exprBuf, sizeof(exprBuf)))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("Unknown type or operator%s",
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

		parent_tab_name = makeNode(RangeVar);
		parent_tab_name->catalogname = cxt->relation->catalogname;
		parent_tab_name->schemaname  = cxt->relation->schemaname;
		parent_tab_name->relname     = cxt->relation->relname;

		child_tab_name = makeNode(RangeVar);
		child_tab_name->catalogname = cxt->relation->catalogname;
		child_tab_name->schemaname  = cxt->relation->schemaname;
		child_tab_name->relname     = child_name_str;

		snprintf(ruleBuf, sizeof(ruleBuf), 
				 "CREATE RULE %s AS ON INSERT to %s WHERE %s DO INSTEAD INSERT INTO %s ", child_name_str, parent_tab_name->relname, ORBuf, child_name_str);

#ifdef NEVER_EVER
		pRule = makeNode(RuleStmt);
		pRule->replace = false; /* do not replace */
		pRule->relation = parent_tab_name;
		pRule->rulename = pstrdup(child_name_str);
		pRule->whereClause = pIndOR;
		pRule->event = CMD_INSERT;
		pRule->instead = true; /* do instead */
		pRule->actions = NIL; /* XXX XXX XXX XXX */
#endif

		pIns = makeNode(InsertStmt);
		pRule = (Node *)pIns;

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
			acs->val.val.str = pstrdup(ORBuf);
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


	} /* end if LIST */

	if (pBy->partType == PARTTYP_RANGE)	
	{
		Node			*pIndAND = NULL;
		Node			*pIndOR  = NULL;
		List			*colElts = pBy->keys;
		ListCell		*lc      = NULL;
		List			*valElts = pBSpec->partStart;
		ListCell		*lc_val  = NULL;
		char			 exprBuf[10000];
		char			 ANDBuf[10000];
		char			 ORBuf[10000];
		char			 ruleBuf[10000];
		int				 range_idx;

		exprBuf[0] = '0';
		ANDBuf[0]  = '0';
		ORBuf[0]   = '0';

		/* number of values must equal of the number of columns */

		for (range_idx = 0; range_idx < 2; range_idx++)
		{
			char *expr_op = ">";

			if (range_idx)
			{
				valElts = pBSpec->partEnd;
				expr_op = "<";
			}

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

					if (NULL == lc_val)
					{
						char *st_end = "START";

						if (range_idx)
						{
							st_end = "END";
						}

						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("Too few columns in %s specification%s",
										st_end,
										at_depth),
								 parser_errposition(pstate, pBSpec->location)));
					}

					/* equality expression: column = value */
					pEq = (Node *) makeSimpleA_Expr(AEXPR_OP, 
													expr_op, 
													pCol,
													lfirst(lc_val),
													-1 /* position */);
					pColConst = (Value *)pCol;
					pValConst = (A_Const *)lfirst(lc_val);

					Assert(pColConst->type == T_String);

					if (pValConst->type != T_A_Const)
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("Invalid use of non-constant type in boundary specification%s",
										at_depth),
								 parser_errposition(pstate, pBSpec->location)));
						
					}

					if (!deparse_partition_rule((Node *)pEq, 
												exprBuf, sizeof(exprBuf)))
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("Unknown type or operator%s",
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
							 errmsg("Too many columns in %s specification%s",
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

		parent_tab_name = makeNode(RangeVar);
		parent_tab_name->catalogname = cxt->relation->catalogname;
		parent_tab_name->schemaname  = cxt->relation->schemaname;
		parent_tab_name->relname     = cxt->relation->relname;

		child_tab_name = makeNode(RangeVar);
		child_tab_name->catalogname = cxt->relation->catalogname;
		child_tab_name->schemaname  = cxt->relation->schemaname;
		child_tab_name->relname     = child_name_str;

		snprintf(ruleBuf, sizeof(ruleBuf), 
				 "CREATE RULE %s AS ON INSERT to %s WHERE %s DO INSTEAD INSERT INTO %s ", child_name_str, parent_tab_name->relname, ORBuf, child_name_str);

#ifdef NEVER_EVER
		pRule = makeNode(RuleStmt);
		pRule->replace = false; /* do not replace */
		pRule->relation = parent_tab_name;
		pRule->rulename = pstrdup(child_name_str);
		pRule->whereClause = pIndOR;
		pRule->event = CMD_INSERT;
		pRule->instead = true; /* do instead */
		pRule->actions = NIL; /* XXX XXX XXX XXX */
#endif

		pIns = makeNode(InsertStmt);
		pRule = (Node *)pIns;

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
			acs->val.val.str = pstrdup(ORBuf);
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


	} /* end if RANGE */


	return pRule;
} /* end make_partition_rules */



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

static void transformPartitionBy(ParseState *pstate,
		   CreateStmtContext *cxt, CreateStmt *stmt, 
		   Node *partitionBy, GpPolicy *policy)
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
				 errmsg("Too many subpartitions: partition depth > 99"
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

		Assert(con->val.type == T_Integer);

		/* XXX XXX IntVal ?? */
		partNumber = con->val.val.ival;

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

	/* find number of partitions in the specification, and match it up
	 * with the PARTITIONS clause if partitioned by HASH, and
	 * determine the subpartition specifications.  Perform basic error
	 * checking on boundary specifications.
	 */
	if (pBy->partSpec)
	{
		PartitionSpec	*pSpec    = (PartitionSpec *)pBy->partSpec;
		char			 namBuf[NAMEDATALEN];

		/* NOTE: a top-level PartitionSpec never has a subSpec, but a
		 * PartitionSpec derived from an inline SubPartition
		 * specification might have one  
		 */

		int			numElts = 0;

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
		bool		prevHadName  = false;
		int			prevStartEnd = 0; /* add 1 for start, 2 for end */

		partElts = pSpec->partElem;

		/* XXX XXX: should check for duplicate names as well !! */

		foreach(lc, partElts)
		{
			PartitionElem *pElem = (PartitionElem *)lfirst(lc);
			PartitionBoundSpec *pBSpec = NULL;

			pBSpec = NULL;

			numElts++;

			if (pElem)
			{
				pBSpec = (PartitionBoundSpec *)pElem->boundSpec;

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
			else
			{
				snprintf(namBuf, sizeof(namBuf), " number %d", 
							 numElts);
			}

			switch (pBy->partType)
			{
				case PARTTYP_HASH:
					if (pBSpec)
					{
						char *specTName = "RANGE";

						if (pBSpec->partType == PARTTYP_LIST)
						{
								specTName = "LIST";
						}

						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("Invalid use of %s boundary specification in partition%s of type HASH%s",
										specTName,
										namBuf,
										at_depth),
								 parser_errposition(pstate, pBSpec->location)));
					}
					break;
				case PARTTYP_RANGE:
					if (pBSpec)
					{
						char *specTName = "LIST";

						if (pBSpec->partType == PARTTYP_LIST)
						{
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
									 errmsg("Invalid use of %s boundary specification in partition%s of type RANGE%s",
											specTName,
											namBuf,
											at_depth),
									 parser_errposition(pstate, pBSpec->location)));
							
						}
					}
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("Missing boundary specification in partition%s of type RANGE%s",
										namBuf,
										at_depth),
									 parser_errposition(pstate, pElem->location)));

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

						if (pBSpec->partStart)
							currStartEnd += 1;
						if (pBSpec->partEnd)
							currStartEnd += 2;

						/* Note: for first loop, prevStartEnd = 0, so
						 * this test is ok */

						if ((prevHadName && !(pElem->partName))
							&&
							(((prevStartEnd == 1)
							 && (currStartEnd == 2))
							||
							((prevStartEnd == 2)
							 && (currStartEnd == 1)))
							)
						{
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
									 errmsg("Invalid use of mixed named and unnamed RANGE boundary specifications%s",
											at_depth),
									 parser_errposition(pstate, 
														pBSpec->location)));

						}

						prevStartEnd = currStartEnd;
					}

					prevHadName  = !!(pElem->partName); /* bool t/f */

					break;

				case PARTTYP_LIST:
					if (pBSpec)
					{
						char *specTName = "RANGE";

						if (pBSpec->partType == PARTTYP_RANGE)
						{
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
									 errmsg("Invalid use of %s boundary specification in partition%s of type LIST%s",
											specTName,
											namBuf,
											at_depth),
									 parser_errposition(pstate, pBSpec->location)));
							
						}
					}
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("Missing boundary specification in partition%s of type LIST%s",
										namBuf,
										at_depth),
								 parser_errposition(pstate, pElem->location)));

					}
					break;

				case PARTTYP_REFERENCE: /* for future use... */
				default:
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("Unknown partition type%s",
									at_depth),
									 parser_errposition(pstate, pBy->location)));
					break;

			}
		} /* end foreach(lc, partElts) */

		if ((partNumber > -1) && (numElts != partNumber))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg(
							 "PARTITIONS \"%d\" must match \"%d\" elements in specification%s",
							 partNumber, numElts, at_depth),
					 parser_errposition(pstate, pBy->location)));
		}

		partNumber = numElts;
	}

	if (partNumber < 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("No partitions specified at depth %d",
						partDepth),
				 parser_errposition(pstate, pBy->location)));
	}

	if (partNumber > 999999)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Too many partitions: partition number > 999999 at depth %d",
						partDepth),
				 parser_errposition(pstate, pBy->location)));
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
						 errmsg("Missing SUBPARTITION BY clause for subpartition specification%s",
								at_depth),
						 parser_errposition(pstate, pBy->location)));
			}
			if (psubBy && psubBy->partSpec)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("Subpartition specification conflict%s",
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
		char	  	 prtstr[NAMEDATALEN];			
		PartitionBy	*newSub = NULL;
		Node		*pRule  = NULL;
		bool		 makeRule = false;
		PartitionElem *pElem = NULL;

		makeRule = false;

		if (lc)
		{
			pElem = (PartitionElem *)lfirst(lc);
			
			if (pElem->partName)
			{
				/* use the partition name as part of the child table name */
				char *pName = strVal(pElem->partName);
				snprintf(prtstr, sizeof(prtstr), "prt_%s", pName);
			}
			else
			{
				snprintf(prtstr, sizeof(prtstr), "prt_%d", ii+1);
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
							 errmsg("Missing SUBPARTITION BY clause for subpartition specification%s",
									at_depth),
							 parser_errposition(pstate, pElem->location)));
				}

				if (psubBy && psubBy->partSpec)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("Subpartition specification conflict%s",
									at_depth),
							 parser_errposition(pstate, psubBy->location)));
				}

				newSub = makeNode(PartitionBy);

				newSub->keys 	  = psubBy->keys ;
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
		} /* end not lc */

		child_tab_name = makeNode(RangeVar);
		child_tab_name->catalogname = cxt->relation->catalogname;
		child_tab_name->schemaname  = cxt->relation->schemaname;

		tab_name_str = 
			ChooseRelationName(cxt->relation->relname,
							   depthstr,	/* depth */
							   prtstr, 		/* object "type" = 
											   partition + (number or name) */
							   snamespaceid);

		ereport(NOTICE,
				(errmsg("%s will create child table \"%s\" for table \"%s\"",
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
			pRule = make_partition_rules(pstate,
										 cxt, stmt, 
										 partitionBy, pElem, at_depth, 
										 tab_name_str);
		}

		child_tab_stmt = makeNode(CreateStmt);
		child_tab_stmt->relation = child_tab_name;

		child_tab_stmt->tableElts = stmt->tableElts;

		/* XXX XXX: inheriting the parent causes a headache in
		 * transformDistributedBy, since it assumes the parent exists
		 * already.  Just add an "ALTER TABLE...INHERIT parent" after
		 * the create child table
		 */
/*		child_tab_stmt->inhRelations = list_make1(parent_tab_name); */
		child_tab_stmt->inhRelations = stmt->inhRelations; 

		child_tab_stmt->constraints = stmt->constraints;
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
		if (pRule)
		{
			cxt->alist = lappend(cxt->alist, pRule);
		}
		pRule  = NULL;

		if (lc)
		{
			lc = lnext(lc);
		}
	} /* end for ii */


} /* end transformPartitionBy */


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
 * transformIndexStmt -
 *	  transforms the qualification of the index statement
 */
static Query *
transformIndexStmt(ParseState *pstate, IndexStmt *stmt)
{
	Query	   *qry;
	RangeTblEntry *rte = NULL;
	ListCell   *l;

	qry = makeNode(Query);
	qry->commandType = CMD_UTILITY;

	/* take care of the where clause */
	if (stmt->whereClause)
	{
		/*
		 * Put the parent table into the rtable so that the WHERE clause can
		 * refer to its fields without qualification.  Note that this only
		 * works if the parent table already exists --- so we can't easily
		 * support predicates on indexes created implicitly by CREATE TABLE.
		 * Fortunately, that's not necessary.
		 */
		rte = addRangeTableEntry(pstate, stmt->relation, NULL, false, true);

		/* no to join list, yes to namespaces */
		addRTEtoQuery(pstate, rte, false, true, true);

		stmt->whereClause = transformWhereClause(pstate, stmt->whereClause,
												 "WHERE");
	}

	/* take care of any index expressions */
	foreach(l, stmt->indexParams)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(l);

		if (ielem->expr)
		{
			/* Set up rtable as for predicate, see notes above */
			if (rte == NULL)
			{
				rte = addRangeTableEntry(pstate, stmt->relation, NULL,
										 false, true);
				/* no to join list, yes to namespaces */
				addRTEtoQuery(pstate, rte, false, true, true);
			}
			ielem->expr = transformExpr(pstate, ielem->expr);

			/*
			 * We check only that the result type is legitimate; this is for
			 * consistency with what transformWhereClause() checks for the
			 * predicate.  DefineIndex() will make more checks.
			 */
			if (expression_returns_set(ielem->expr))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("index expression may not return a set")));
		}
	}

	qry->hasSubLinks = pstate->p_hasSubLinks;
	stmt->rangetable = pstate->p_rtable;

	qry->utilityStmt = (Node *) stmt;

	return qry;
}

/*
 * transformRuleStmt -
 *	  transform a Create Rule Statement. The actions is a list of parse
 *	  trees which is transformed into a list of query trees.
 */
static Query *
transformRuleStmt(ParseState *pstate, RuleStmt *stmt,
				  List **extras_before, List **extras_after)
{
	Query	   *qry;
	Relation	rel;
	RangeTblEntry *oldrte;
	RangeTblEntry *newrte;

	qry = makeNode(Query);
	qry->commandType = CMD_UTILITY;
	qry->utilityStmt = (Node *) stmt;

	/*
	 * To avoid deadlock, make sure the first thing we do is grab
	 * AccessExclusiveLock on the target relation.	This will be needed by
	 * DefineQueryRewrite(), and we don't want to grab a lesser lock
	 * beforehand.
	 */
	rel = heap_openrv(stmt->relation, AccessExclusiveLock);

	/*
	 * NOTE: 'OLD' must always have a varno equal to 1 and 'NEW' equal to 2.
	 * Set up their RTEs in the main pstate for use in parsing the rule
	 * qualification.
	 */
	Assert(pstate->p_rtable == NIL);
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
	stmt->whereClause = transformWhereClause(pstate, stmt->whereClause,
											 "WHERE");

	if (list_length(pstate->p_rtable) != 2)		/* naughty, naughty... */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("rule WHERE condition may not contain references to other relations")));

	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
		   errmsg("cannot use aggregate function in rule WHERE condition")));

	if (pstate->p_hasWindFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use window function in rule WHERE condition")));


	/* save info about sublinks in where clause */
	qry->hasSubLinks = pstate->p_hasSubLinks;

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

		stmt->actions = list_make1(nothing_qry);
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
			ParseState *sub_pstate = make_parsestate(pstate->parentParseState);
			Query	   *sub_qry,
					   *top_subqry;
			bool		has_old,
						has_new;

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
			top_subqry = transformStmt(sub_pstate, action,
									   extras_before, extras_after);

			/*
			 * We cannot support utility-statement actions (eg NOTIFY) with
			 * nonempty rule WHERE conditions, because there's no way to make
			 * the utility action execute conditionally.
			 */
			if (top_subqry->commandType == CMD_UTILITY &&
				stmt->whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("rules with WHERE conditions may only have SELECT, INSERT, UPDATE, or DELETE actions")));

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
			if (sub_qry->setOperations != NULL && stmt->whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			/*
			 * Validate action's use of OLD/NEW, qual too
			 */
			has_old =
				rangeTableEntry_used((Node *) sub_qry, PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used(stmt->whereClause, PRS2_OLD_VARNO, 0);
			has_new =
				rangeTableEntry_used((Node *) sub_qry, PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used(stmt->whereClause, PRS2_NEW_VARNO, 0);

			switch (stmt->event)
			{
				case CMD_SELECT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule may not use OLD")));
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule may not use NEW")));
					break;
				case CMD_UPDATE:
					/* both are OK */
					break;
				case CMD_INSERT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON INSERT rule may not use OLD")));
					break;
				case CMD_DELETE:
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON DELETE rule may not use NEW")));
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

			release_pstate_resources(sub_pstate);
			pfree(sub_pstate);
		}

		stmt->actions = newactions;
	}

	/* Close relation, but keep the exclusive lock */
	heap_close(rel, NoLock);

	return qry;
}

/*
 * We use this mutator to transform queries of the form:
 * 		SELECT i, win(agg(j)) over (...)
 * to
 * 		SELECT i, win(var) over(...) FROM (select i, agg(j) as var ...)
 *
 * We perform a complex mutation on the target list: if we have encounter
 * Aggrefs in the target list, we must replace them with Var references to
 * the Aggref in the subquery. To do this, we create and save a TargetEntry
 * representing the Aggref in the context variable and replace the
 * Aggref with a Var.
 *
 * If the input node is a TargetEntry we save this into the sub query
 * target list and create a Var for the top level query to reference it.
 */

static Node *
wingroupmutator(Node *node, void *context)
{
	grouped_window_ctx *ctx = (grouped_window_ctx *)context;
	Node *result;

	if (!node)
		return NULL;
	
	/* We've encountered and Aggref, push it into the sub query */
	if (IsA(node, Aggref))
	{
		ListCell *tmp;
		TargetEntry *te;
		Var *v;

		/*
		 * If there's already a TargetEntry for this Aggref, just return
		 * a Var pointing to the entry in the sub query.
		 *
		 * The code further up the call tree should have been smart enough
		 * smart enough to produce unique Aggrefs for us but, just in case
		 * we should do this.
		 */
		foreach(tmp, ctx->subtlist)
		{
			TargetEntry *te = (TargetEntry *)lfirst(tmp);

			if (equal((Node *)te->expr, (Node *)node))
			{
				/* Found a match, time to construct the Var */
				v = makeVar(1, te->resno, exprType(node), exprTypmod(node), 0);

			}
		}
		
		te = makeTargetEntry((Expr *)node, ctx->subattno,
							 FigureColname(node), false);
		/* seems like we should see if we need to update sort group settings */
		ctx->subtlist = lappend(ctx->subtlist, te);
		
		v = makeVar(1, ctx->subattno, exprType(node), exprTypmod(node), 0);
		ctx->subattno++;

		return (Node *)v;
	}
	

	if (IsA(node, TargetEntry))
	{
		TargetEntry *te = (TargetEntry *)node;
		
		result = expression_tree_mutator((Node *)te->expr, wingroupmutator, 
										 context);
		te->expr = (Expr *)result;
		if (checkExprHasWindFuncs(result))
		{
			Assert(!checkExprHasAggs(result));

			/* we received a TE in, so a TE must go out */
			return (Node *)te;
		}
		else
		{
			Var *v = makeNode(Var);
			TargetEntry *subte = te;
			TargetEntry *topte;

			/* XXX: check if we have an entry already */
			v = makeVar(1, ctx->subattno, exprType((Node *)subte->expr),
						exprTypmod((Node *)subte->expr), 0);

			topte = makeTargetEntry((Expr *)v, subte->resno,
									subte->resname, subte->resjunk);
			topte->ressortgroupref = subte->ressortgroupref;

			subte->resno = ctx->subattno;
			ctx->subattno++;
			ctx->subtlist = lappend(ctx->subtlist, subte);

			return (Node *)topte;
		}
	}

	/* if we've reached this point, just pass the node to the walker */
	return expression_tree_mutator(node, wingroupmutator, context);
}

static bool
record_sortrefs_walker(Node *node, void *context)
{
	sortref_ctx *ctx = (sortref_ctx *)context;

	if (node == NULL)
		return true;
	
#define RECORD(ref) \
do \
{ \
	if (ref >= ctx->size) \
	{ \
		Index newsz = ref + 1; \
		Assert(ref > 0); /* check overflow */ \
		ctx->sortref = realloc(ctx->sortref, sizeof(Index) * newsz); \
		MemSet(&(ctx->sortref[ctx->size]), \
			   0, (newsz - ctx->size) * sizeof(Index)); \
		ctx->size = newsz; \
	} \
	ctx->sortref[ref] = ref; \
} \
while(0)
	
	if (IsA(node, SortClause))
	{
		Index ref = ((SortClause *)node)->tleSortGroupRef;
		RECORD(ref);
		return false;
	}
	else if (IsA(node, GroupClause))
	{
		/* 
		 * Could be okay to just cast this to SortClause in the above block
		 * as GroupClause is just an alias for SortClause. But, this might
		 * not always be so!
		 */
		Index ref = ((GroupClause *)node)->tleSortGroupRef;
		RECORD(ref);
		return false;	
	}
	else if(IsA(node, GroupingClause))
	{
		GroupingClause *g = (GroupingClause *)node;
			
		return expression_tree_walker((Node *)g->groupsets,
									  record_sortrefs_walker,
								   	  context);
	}
	return expression_tree_walker(node, record_sortrefs_walker, context);
}

static bool
rewrite_sortrefs_walker(Node *node, void *context)
{
	sortref_ctx *ctx = (sortref_ctx *)context; 
	
	if (node == NULL)
		return false;
	else if(IsA(node, GroupingClause))
	{
		GroupingClause *g = (GroupingClause *)node;
			
		return expression_tree_walker((Node *)g->groupsets, 
								      rewrite_sortrefs_walker,
								      context);
	}
	ctx->level++;
	if (ctx->level == ctx->levelsup)
	{
		if (IsA(node, TargetEntry))
		{
			TargetEntry *te = (TargetEntry *)node;
			te->ressortgroupref = ctx->sortref[te->ressortgroupref];
			ctx->level--;
			return false;
		}
		else if(IsA(node, SortClause))
		{
			SortClause *cl = (SortClause *)node;
			cl->tleSortGroupRef = ctx->sortref[cl->tleSortGroupRef];
			ctx->level--;
			return false;
		}
		else if(IsA(node, GroupClause))
		{
			SortClause *cl = (SortClause *)node;
			cl->tleSortGroupRef = ctx->sortref[cl->tleSortGroupRef];
			ctx->level--;
			return false;
		}
	}
	ctx->level--;
	return expression_tree_walker(node, rewrite_sortrefs_walker, context);
}

static void
rewrite_sortrefs(Query *qry)
{
	/*
	 * We must build up a mapping of sort refs from the old values to
	 * contiguous values. To do this, we build an array of the refs 
	 * registered in the relevant sort clauses in the query and then we map
	 * them to new references, from 1..N. Then we rewrite the target list
	 * using this.
	 */
	sortref_ctx ctx;
	Index max, i;
	
	ctx.size = 8; /* seems like this would cover most cases */
	ctx.sortref = (Index *)palloc0(ctx.size * sizeof(Index));
	ctx.levelsup = 1;
	ctx.level = 0;
	
	expression_tree_walker((Node *)qry->sortClause, record_sortrefs_walker, 
						   &ctx);
	expression_tree_walker((Node *)qry->distinctClause, record_sortrefs_walker,
						   &ctx);
	expression_tree_walker((Node *)qry->groupClause, record_sortrefs_walker,
						   &ctx);
	expression_tree_walker((Node *)qry->windowClause, record_sortrefs_walker,
						   &ctx);

	for (i = 0, max = 1; i < ctx.size; i++)
	{
		if (ctx.sortref[i])
			ctx.sortref[i] = max++;
	}
	query_tree_walker(qry, rewrite_sortrefs_walker, &ctx, 0);
	pfree(ctx.sortref);
}

/*
 * ANSI SQL allows users to specify a query of the form:
 *    SELECT deptno, sum(max(salary)) OVER (order by deptno) FROM employees
 *    GROUP BY deptno;
 * 
 * This is pretty hard for the planner to make sense of, so we rewrite it,
 * putting the grouped portion of the query in a subquery in the range table:
 *
 *    SELECT deptno, sum(max) FROM
 *      (SELECT deptno, max(salary) FROM employees GROUP BY deptno) AS foo;
 *
 * This means moving stuff from qry into a new Query node subq and
 * making that new node a sub query in the range table of qry. We move
 * 
 */ 
static void
transformGroupedWindows(Query *qry)
{
	Query *subq = makeNode(Query);
	List *newtl;
	grouped_window_ctx ctx;
	RangeTblEntry *rte;
	RangeTblRef *ref;
	Alias *a;
	ListCell *tmp;
	Index *sortrefmap;
	Index mapsize;
	
	Assert(qry->commandType == CMD_SELECT);
	subq->commandType = qry->commandType;
	subq->canSetTag = qry->canSetTag;

	Assert(!PointerIsValid(qry->utilityStmt));
	subq->utilityStmt = qry->utilityStmt;
	subq->resultRelation = 0;
	subq->canSetTag = true;
	subq->into = NULL;
	subq->intoOptions = NIL;
	subq->intoOnCommit = ONCOMMIT_NOOP;
	subq->intoTableSpaceName = NULL;
	subq->hasAggs = false;
	/* hasWindFuncs is covered later */
	subq->hasSubLinks = qry->hasSubLinks; /* If qry has them, we do now */
	subq->rtable = qry->rtable;
	subq->jointree = (FromExpr *)makeNode(FromExpr);
	subq->jointree->fromlist = qry->jointree->fromlist;
	subq->jointree->quals = qry->jointree->quals;

	/* target list later */
	subq->groupClause = qry->groupClause;
	subq->havingQual = qry->havingQual;

	/*
	 * Check if there is a window function in the join tree. If so
	 * we must mark hasWindFuncs in the sub query as well.
	 */
	if (checkExprHasWindFuncs((Node *)subq->jointree))
		subq->hasWindFuncs = true;

	/* 
	 * Rewrite the top level target list to point to the correct var nos
	 * in the sub query. Be very careful to update the arguments to
	 * window functions as well.
	 *
	 * The sub query must contain all columns referenced in the original
	 * target list. So:
	 *
	 *      SELECT DISTINCT a , avg(b), 
	 *      	row_number() over (partition by sum(c)), 
	 *      	ntile(d) over (order by e)
	 *          FROM r WHERE f < 10
	 *          GROUP BY a,d,e
	 *          HAVING max(f) > 0
	 *          ORDER BY a, avg(b)
	 *          OFFSET 10 LIMIT 10;
	 *
	 *   gets "rewritten" as
	 *
	 *      SELECT DISTINCT v1, v4, row_number() over (partition by v5),  
	 *          ntile(v2) over (order by v3)
	 *          FROM (
	 *            SELECT a, d, e, avg(b), sum(c)
	 *            FROM r
	 *            WHERE f < 10
	 *            GROUP BY a,d,e
	 *            HAVING max(f) > 0  ) v ( v1, v2, v3, v4, v5)
	 *          ORDER BY v1, v4
	 *          OFFSET 10 LIMIT 10;
	 */
	ctx.subattno = 1;
	ctx.subtlist = NIL;

	newtl = (List *)expression_tree_mutator((Node *)qry->targetList, 
											wingroupmutator, &ctx);
	Assert(PointerIsValid(newtl));
	Assert(IsA(newtl, List));
	qry->targetList = newtl;
	subq->targetList = ctx.subtlist;

	/* now add a range table entry */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = subq;
	rte->inFromCl = true;
	rte->requiredPerms |= ACL_SELECT;
	
	/* do aliases */
	a = makeNode(Alias);
	a->aliasname = pstrdup("Window");

	mapsize = 8;
	sortrefmap = (Index *)palloc0(mapsize * sizeof(Index));

	foreach(tmp, subq->targetList)
	{
		TargetEntry *te = (TargetEntry *)lfirst(tmp);
		char *resname;
		
		Assert(IsA(te, TargetEntry));

		/*
		 * Check if there is an Agg node in the targetlist. If so
		 * we mark hasAggs to true.
		 */
		if (checkExprHasAggs((Node *)te->expr))
			subq->hasAggs = true;

		/* 
	 	 * TEs generated for the purpose of sorting (PARTITION BY and 
		 * ORDER BY) will have resjunk = false. This is fine in the top query
		 * but not in the sub query. Change them while we're looping.
		 *
		 * This is very inefficient but hey, N is usually small.
	 	 */
		if (te->resjunk && te->ressortgroupref)
		{
			ListCell *tmp2;
			foreach(tmp2, qry->windowClause)
			{
				ListCell *tmp3;
				WindowSpec *s = lfirst(tmp2);
				
				foreach(tmp3, s->partition)
				{
					SortClause *sc = lfirst(tmp3);

					if (sc->tleSortGroupRef == te->ressortgroupref)
					{
						te->resjunk = false;
						break;
					}
				}
				
				if (!te->resjunk)
					break;
				
				foreach(tmp3, s->order)
				{
					SortClause *sc = lfirst(tmp3);

					if (sc->tleSortGroupRef == te->ressortgroupref)
					{
						te->resjunk = false;
						break;
					}
				}

				if (!te->resjunk)
					break;
			}
		}

		resname = te->resname ? te->resname : "?column?";
		a->colnames = lappend(a->colnames, makeString(pstrdup(resname)));
	}
	rte->eref = a;
	
	/* we forget the old rtable by overwriting it with this new RTE */
	qry->rtable = list_make1(rte);

	/* put a ref in the join tree to the new RTE */
	ref = makeNode(RangeTblRef);
	ref->rtindex = 1;
	qry->jointree->fromlist = list_make1(ref);
	qry->jointree->quals = NULL;
	qry->hasSubLinks = false;

	/* we can also forget about grouping in the top level query */
	qry->groupClause = NIL; 
	qry->havingQual = NULL;
	qry->hasAggs = false;
	
	rewrite_sortrefs(subq);
	rewrite_sortrefs(qry);
}

/*
 * transformSelectStmt -
 *	  transforms a Select Statement
 *
 * Note: this is also used for DECLARE CURSOR statements.
 */
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *l;

	qry->commandType = CMD_SELECT;

	/* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
	pstate->p_locking_clause = stmt->lockingClause;

	/* 
	 * Put WINDOW clause data into pstate so that window references know
	 * about them.
	 */
	pstate->p_win_clauses = stmt->windowClause;

	/* process the FROM clause */
	transformFromClause(pstate, stmt->fromClause);

	/* transform targetlist */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* mark column origins */
	markTargetListOrigins(pstate, qry->targetList);

	/* tidy up expressions in window clauses */
	transformWindowSpecExprs(pstate);

	/* transform WHERE */
	qual = transformWhereClause(pstate, stmt->whereClause, "WHERE");

	/*
	 * Initial processing of HAVING clause is just like WHERE clause.
	 */
	pstate->having_qual = transformWhereClause(pstate, stmt->havingClause,
										   "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  true /* fix unknowns */ );

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											&qry->targetList,
											qry->sortClause);

	qry->havingQual = pstate->having_qual;
	pstate->having_qual = NULL;

	/* 
	 * Process WINDOW clause.
	 */
	transformWindowClause(pstate, qry);

	qry->distinctClause = transformDistinctClause(pstate,
												  stmt->distinctClause,
												  &qry->targetList,
												  &qry->sortClause);

	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											"OFFSET");
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   "LIMIT");

	/* handle any SELECT INTO/CREATE TABLE AS spec */
	if (stmt->into)
	{
		qry->into = stmt->into;
		if (stmt->intoColNames)
			applyColumnNames(qry->targetList, stmt->intoColNames);
		qry->intoOptions = copyObject(stmt->intoOptions);
		qry->intoOnCommit = stmt->intoOnCommit;
		qry->intoTableSpaceName = stmt->intoTableSpaceName;

		if (stmt->distributedBy && Gp_role == GP_ROLE_DISPATCH)
			setQryDistributionPolicy(stmt, qry);
	}

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs || qry->groupClause || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	qry->hasWindFuncs = pstate->p_hasWindFuncs;
	if (pstate->p_hasWindFuncs)
		parseProcessWindFuncs(pstate, qry);
	

	foreach(l, stmt->lockingClause)
	{
		transformLockingClause(qry, (LockingClause *) lfirst(l));
	}

	/*
	 * If the query mixes window functions and aggregates, we need to
	 * transform it such that the grouped query appears as a subquery
	 */
	if (qry->hasWindFuncs && (qry->groupClause || qry->hasAggs))
		transformGroupedWindows(qry);

	return qry;
}

/*
 * transformValuesClause -
 *	  transforms a VALUES clause that's being used as a standalone SELECT
 *
 * We build a Query containing a VALUES RTE, rather as if one had written
 *			SELECT * FROM (VALUES ...)
 */
static Query *
transformValuesClause(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	List	   *exprsLists = NIL;
	List	  **coltype_lists = NULL;
	Oid		   *coltypes = NULL;
	int			sublist_length = -1;
	List	   *newExprsLists;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	ListCell   *lc;
	ListCell   *lc2;
	int			i;

	qry->commandType = CMD_SELECT;

	/* Most SELECT stuff doesn't apply in a VALUES clause */
	Assert(stmt->distinctClause == NIL);
	Assert(stmt->targetList == NIL);
	Assert(stmt->fromClause == NIL);
	Assert(stmt->whereClause == NULL);
	Assert(stmt->groupClause == NIL);
	Assert(stmt->havingClause == NULL);
	Assert(stmt->op == SETOP_NONE);

	/*
	 * For each row of VALUES, transform the raw expressions and gather type
	 * information.  This is also a handy place to reject DEFAULT nodes, which
	 * the grammar allows for simplicity.
	 */
	foreach(lc, stmt->valuesLists)
	{
		List	   *sublist = (List *) lfirst(lc);

		/* Do basic expression transformation (same as a ROW() expr) */
		sublist = transformExpressionList(pstate, sublist);

		/*
		 * All the sublists must be the same length, *after* transformation
		 * (which might expand '*' into multiple items).  The VALUES RTE can't
		 * handle anything different.
		 */
		if (sublist_length < 0)
		{
			/* Remember post-transformation length of first sublist */
			sublist_length = list_length(sublist);
			/* and allocate arrays for column-type info */
			coltype_lists = (List **) palloc0(sublist_length * sizeof(List *));
			coltypes = (Oid *) palloc0(sublist_length * sizeof(Oid));
		}
		else if (sublist_length != list_length(sublist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("VALUES lists must all be the same length")));
		}

		exprsLists = lappend(exprsLists, sublist);

		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			if (IsA(col, SetToDefault))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("DEFAULT can only appear in a VALUES list within INSERT")));
			coltype_lists[i] = lappend_oid(coltype_lists[i], exprType(col));
			i++;
		}
	}

	/*
	 * Now resolve the common types of the columns, and coerce everything to
	 * those types.
	 */
	for (i = 0; i < sublist_length; i++)
	{
		coltypes[i] = select_common_type(coltype_lists[i], "VALUES");
	}

	newExprsLists = NIL;
	foreach(lc, exprsLists)
	{
		List	   *sublist = (List *) lfirst(lc);
		List	   *newsublist = NIL;

		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			col = coerce_to_common_type(pstate, col, coltypes[i], "VALUES");
			newsublist = lappend(newsublist, col);
			i++;
		}

		newExprsLists = lappend(newExprsLists, newsublist);
	}

	/*
	 * Generate the VALUES RTE
	 */
	rte = addRangeTableEntryForValues(pstate, newExprsLists, NULL, true);
	rtr = makeNode(RangeTblRef);
	/* assume new rte is at end */
	rtr->rtindex = list_length(pstate->p_rtable);
	Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
	pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	pstate->p_varnamespace = lappend(pstate->p_varnamespace, rte);

	/*
	 * Generate a targetlist as though expanding "*"
	 */
	Assert(pstate->p_next_resno == 1);
	qry->targetList = expandRelAttrs(pstate, rte, rtr->rtindex, 0);

	/*
	 * The grammar allows attaching ORDER BY, LIMIT, and FOR UPDATE to a
	 * VALUES, so cope.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  true /* fix unknowns */ );

	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											"OFFSET");
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   "LIMIT");

	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to VALUES")));
		
	if (stmt->distributedBy && Gp_role == GP_ROLE_DISPATCH)
		setQryDistributionPolicy(stmt, qry);

	/* handle any CREATE TABLE AS spec */
	if (stmt->into)
	{
		qry->into = stmt->into;
		if (stmt->intoColNames)
			applyColumnNames(qry->targetList, stmt->intoColNames);
		qry->intoOptions = copyObject(stmt->intoOptions);
		qry->intoOnCommit = stmt->intoOnCommit;
		qry->intoTableSpaceName = stmt->intoTableSpaceName;
	}

	/*
	 * There mustn't have been any table references in the expressions, else
	 * strange things would happen, like Cartesian products of those tables
	 * with the VALUES list.  We have to check this after parsing ORDER BY et
	 * al since those could insert more junk.
	 */
	if (list_length(pstate->p_joinlist) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VALUES must not contain table references")));

	/*
	 * Another thing we can't currently support is NEW/OLD references in rules
	 * --- seems we'd need something like SQL99's LATERAL construct to ensure
	 * that the values would be available while evaluating the VALUES RTE.
	 * This is a shame.  FIXME
	 */
	if (list_length(pstate->p_rtable) != 1 &&
		contain_vars_of_level((Node *) newExprsLists, 0))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VALUES must not contain OLD or NEW references"),
				 errhint("Use SELECT ... UNION ALL ... instead.")));

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in VALUES")));

	if (pstate->p_hasWindFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use window function in VALUES")));

	return qry;
}

/*
 * transformSetOperationsStmt -
 *	  transforms a set-operations tree
 *
 * A set-operation tree is just a SELECT, but with UNION/INTERSECT/EXCEPT
 * structure to it.  We must transform each leaf SELECT and build up a top-
 * level Query that contains the leaf SELECTs as subqueries in its rangetable.
 * The tree of set operations is converted into the setOperations field of
 * the top-level Query.
 */
static Query *
transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *leftmostSelect;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	SetOperationStmt *sostmt;
	List	   *intoColNames = NIL;
	List	   *sortClause;
	Node	   *limitOffset;
	Node	   *limitCount;
	List	   *lockingClause;
	Node	   *node;
	ListCell   *left_tlist,
			   *lct,
			   *lcm,
			   *l;
	List	   *targetvars,
			   *targetnames,
			   *sv_relnamespace,
			   *sv_varnamespace,
			   *sv_rtable;
	RangeTblEntry *jrte;
	int			tllen;

	qry->commandType = CMD_SELECT;

	/*
	 * Find leftmost leaf SelectStmt; extract the one-time-only items from it
	 * and from the top-level node.  (Most of the INTO options can be
	 * transferred to the Query immediately, but intoColNames has to be saved
	 * to apply below.)
	 */
	leftmostSelect = stmt->larg;
	while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
		leftmostSelect = leftmostSelect->larg;
	Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
		   leftmostSelect->larg == NULL);
	if (leftmostSelect->into)
	{
		qry->into = leftmostSelect->into;
		intoColNames = leftmostSelect->intoColNames;
		qry->intoOptions = copyObject(leftmostSelect->intoOptions);
		qry->intoOnCommit = leftmostSelect->intoOnCommit;
		qry->intoTableSpaceName = leftmostSelect->intoTableSpaceName;
	}

	/* clear this to prevent complaints in transformSetOperationTree() */
	leftmostSelect->into = NULL;

	/*
	 * These are not one-time, exactly, but we want to process them here and
	 * not let transformSetOperationTree() see them --- else it'll just
	 * recurse right back here!
	 */
	sortClause = stmt->sortClause;
	limitOffset = stmt->limitOffset;
	limitCount = stmt->limitCount;
	lockingClause = stmt->lockingClause;

	stmt->sortClause = NIL;
	stmt->limitOffset = NULL;
	stmt->limitCount = NULL;
	stmt->lockingClause = NIL;

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));

	/*
	 * Recursively transform the components of the tree.
	 */
	sostmt = (SetOperationStmt *) transformSetOperationTree(pstate, stmt);
	Assert(sostmt && IsA(sostmt, SetOperationStmt));
	qry->setOperations = (Node *) sostmt;

	/*
	 * Re-find leftmost SELECT (now it's a sub-query in rangetable)
	 */
	node = sostmt->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist for outer query using column names of
	 * leftmost select and common datatypes of topmost set operation. Also
	 * make lists of the dummy vars and their names for use in parsing ORDER
	 * BY.
	 *
	 * Note: we use leftmostRTI as the varno of the dummy variables. It
	 * shouldn't matter too much which RT index they have, as long as they
	 * have one that corresponds to a real RT entry; else funny things may
	 * happen when the tree is mashed by rule rewriting.
	 */
	qry->targetList = NIL;
	targetvars = NIL;
	targetnames = NIL;
	left_tlist = list_head(leftmostQuery->targetList);

	forboth(lct, sostmt->colTypes, lcm, sostmt->colTypmods)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;
		Expr	   *expr;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		expr = (Expr *) makeVar(leftmostRTI,
								lefttle->resno,
								colType,
								colTypmod,
								0);
		tle = makeTargetEntry(expr,
							  (AttrNumber) pstate->p_next_resno++,
							  colName,
							  false);
		qry->targetList = lappend(qry->targetList, tle);
		targetvars = lappend(targetvars, expr);
		targetnames = lappend(targetnames, makeString(colName));
		left_tlist = lnext(left_tlist);
	}

	/*
	 * As a first step towards supporting sort clauses that are expressions
	 * using the output columns, generate a varnamespace entry that makes the
	 * output columns visible.	A Join RTE node is handy for this, since we
	 * can easily control the Vars generated upon matches.
	 *
	 * Note: we don't yet do anything useful with such cases, but at least
	 * "ORDER BY upper(foo)" will draw the right error message rather than
	 * "foo not found".
	 */
	jrte = addRangeTableEntryForJoin(NULL,
									 targetnames,
									 JOIN_INNER,
									 targetvars,
									 NULL,
									 false);

	sv_rtable = pstate->p_rtable;
	pstate->p_rtable = list_make1(jrte);

	sv_relnamespace = pstate->p_relnamespace;
	pstate->p_relnamespace = NIL;		/* no qualified names allowed */

	sv_varnamespace = pstate->p_varnamespace;
	pstate->p_varnamespace = list_make1(jrte);

	/*
	 * For now, we don't support resjunk sort clauses on the output of a
	 * setOperation tree --- you can only use the SQL92-spec options of
	 * selecting an output column by name or number.  Enforce by checking that
	 * transformSortClause doesn't add any items to tlist.
	 */
	tllen = list_length(qry->targetList);

	qry->sortClause = transformSortClause(pstate,
										  sortClause,
										  &qry->targetList,
										  false /* no unknowns expected */ );

	pstate->p_rtable = sv_rtable;
	pstate->p_relnamespace = sv_relnamespace;
	pstate->p_varnamespace = sv_varnamespace;

	if (tllen != list_length(qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ORDER BY on a UNION/INTERSECT/EXCEPT result must be on one of the result columns")));

	qry->limitOffset = transformLimitClause(pstate, limitOffset,
											"OFFSET");
	qry->limitCount = transformLimitClause(pstate, limitCount,
										   "LIMIT");

	/*
	 * Handle SELECT INTO/CREATE TABLE AS.
	 *
	 * Any column names from CREATE TABLE AS need to be attached to both the
	 * top level and the leftmost subquery.  We do not do this earlier because
	 * we do *not* want sortClause processing to be affected.
	 */
	if (intoColNames)
	{
		applyColumnNames(qry->targetList, intoColNames);
		applyColumnNames(leftmostQuery->targetList, intoColNames);
	}

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs || qry->groupClause || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	foreach(l, lockingClause)
	{
		transformLockingClause(qry, (LockingClause *) lfirst(l));
	}

	return qry;
}

/*
 * transformSetOperationTree
 *		Recursively transform leaves and internal nodes of a set-op tree
 */
static Node *
transformSetOperationTree(ParseState *pstate, SelectStmt *stmt)
{
	bool		isLeaf;

	Assert(stmt && IsA(stmt, SelectStmt));

	/*
	 * Validity-check both leaf and internal SELECTs for disallowed ops.
	 */
	if (stmt->into)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT")));
	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));

	/*
	 * If an internal node of a set-op tree has ORDER BY, UPDATE, or LIMIT
	 * clauses attached, we need to treat it like a leaf node to generate an
	 * independent sub-Query tree.	Otherwise, it can be represented by a
	 * SetOperationStmt node underneath the parent Query.
	 */
	if (stmt->op == SETOP_NONE)
	{
		Assert(stmt->larg == NULL && stmt->rarg == NULL);
		isLeaf = true;
	}
	else
	{
		Assert(stmt->larg != NULL && stmt->rarg != NULL);
		if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
			stmt->lockingClause)
			isLeaf = true;
		else
			isLeaf = false;
	}

	if (isLeaf)
	{
		/* Process leaf SELECT */
		List	   *selectList;
		Query	   *selectQuery;
		char		selectName[32];
		RangeTblEntry *rte;
		RangeTblRef *rtr;

		/*
		 * Transform SelectStmt into a Query.
		 *
		 * Note: previously transformed sub-queries don't affect the parsing
		 * of this sub-query, because they are not in the toplevel pstate's
		 * namespace list.
		 */
		selectList = parse_sub_analyze((Node *) stmt, pstate);

		Assert(list_length(selectList) == 1);
		selectQuery = (Query *) linitial(selectList);
		Assert(IsA(selectQuery, Query));

		/*
		 * Check for bogus references to Vars on the current query level (but
		 * upper-level references are okay). Normally this can't happen
		 * because the namespace will be empty, but it could happen if we are
		 * inside a rule.
		 */
		if (pstate->p_relnamespace || pstate->p_varnamespace)
		{
			if (contain_vars_of_level((Node *) selectQuery, 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("UNION/INTERSECT/EXCEPT member statement may not refer to other relations of same query level")));
		}

		/*
		 * Make the leaf query be a subquery in the top-level rangetable.
		 */
		snprintf(selectName, sizeof(selectName), "*SELECT* %d",
				 list_length(pstate->p_rtable) + 1);
		rte = addRangeTableEntryForSubquery(pstate,
											selectQuery,
											makeAlias(selectName, NIL),
											false);

		/*
		 * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
		 */
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		return (Node *) rtr;
	}
	else
	{
		/* Process an internal node (set operation node) */
		SetOperationStmt *op = makeNode(SetOperationStmt);
		List	   *lcoltypes;
		List	   *rcoltypes;
		List	   *lcoltypmods;
		List	   *rcoltypmods;
		ListCell   *lct;
		ListCell   *rct;
		ListCell   *lcm;
		ListCell   *rcm;
		const char *context;

		context = (stmt->op == SETOP_UNION ? "UNION" :
				   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
					"EXCEPT"));

		op->op = stmt->op;
		op->all = stmt->all;

		/*
		 * Recursively transform the child nodes.
		 */
		op->larg = transformSetOperationTree(pstate, stmt->larg);
		op->rarg = transformSetOperationTree(pstate, stmt->rarg);

		/*
		 * Verify that the two children have the same number of non-junk
		 * columns, and determine the types of the merged output columns.
		 */
		getSetColTypes(pstate, op->larg, &lcoltypes, &lcoltypmods);
		getSetColTypes(pstate, op->rarg, &rcoltypes, &rcoltypmods);
		if (list_length(lcoltypes) != list_length(rcoltypes))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("each %s query must have the same number of columns",
						context)));
		Assert(list_length(lcoltypes) == list_length(lcoltypmods));
		Assert(list_length(rcoltypes) == list_length(rcoltypmods));

		op->colTypes = NIL;
		op->colTypmods = NIL;
		/* don't have a "foreach4", so chase two of the lists by hand */
		lcm = list_head(lcoltypmods);
		rcm = list_head(rcoltypmods);
		forboth(lct, lcoltypes, rct, rcoltypes)
		{
			Oid			lcoltype = lfirst_oid(lct);
			Oid			rcoltype = lfirst_oid(rct);
			int32		lcoltypmod = lfirst_int(lcm);
			int32		rcoltypmod = lfirst_int(rcm);
			Oid			rescoltype;
			int32		rescoltypmod;

			/* select common type, same as CASE et al */
			rescoltype = select_common_type(list_make2_oid(lcoltype, rcoltype),
											context);
			/* if same type and same typmod, use typmod; else default */
			if (lcoltype == rcoltype && lcoltypmod == rcoltypmod)
				rescoltypmod = lcoltypmod;
			else
				rescoltypmod = -1;
			op->colTypes = lappend_oid(op->colTypes, rescoltype);
			op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);

			lcm = lnext(lcm);
			rcm = lnext(rcm);
		}

		return (Node *) op;
	}
}

/*
 * getSetColTypes
 *	  Get output column types/typmods of an (already transformed) set-op node
 */
static void
getSetColTypes(ParseState *pstate, Node *node,
			   List **colTypes, List **colTypmods)
{
	*colTypes = NIL;
	*colTypmods = NIL;
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) node;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, pstate->p_rtable);
		Query	   *selectQuery = rte->subquery;
		ListCell   *tl;

		Assert(selectQuery != NULL);
		/* Get types of non-junk columns */
		foreach(tl, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (tle->resjunk)
				continue;
			*colTypes = lappend_oid(*colTypes,
									exprType((Node *) tle->expr));
			*colTypmods = lappend_int(*colTypmods,
									  exprTypmod((Node *) tle->expr));
		}
	}
	else if (IsA(node, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) node;

		/* Result already computed during transformation of node */
		Assert(op->colTypes != NIL);
		*colTypes = op->colTypes;
		*colTypmods = op->colTypmods;
	}
	else
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
}

/* Attach column names from a ColumnDef list to a TargetEntry list */
static void
applyColumnNames(List *dst, List *src)
{
	ListCell   *dst_item;
	ListCell   *src_item;

	src_item = list_head(src);

	foreach(dst_item, dst)
	{
		TargetEntry *d = (TargetEntry *) lfirst(dst_item);
		ColumnDef  *s;

		/* junk targets don't count */
		if (d->resjunk)
			continue;

		/* fewer ColumnDefs than target entries is OK */
		if (src_item == NULL)
			break;

		s = (ColumnDef *) lfirst(src_item);
		src_item = lnext(src_item);

		d->resname = pstrdup(s->colname);
	}

	/* more ColumnDefs than target entries is not OK */
	if (src_item != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("CREATE TABLE AS specifies too many column names")));
}


/*
 * transformUpdateStmt -
 *	  transforms an update statement
 */
static Query *
transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *origTargetList;
	ListCell   *tl;

	qry->commandType = CMD_UPDATE;
	pstate->p_is_update = true;

	qry->resultRelation = setTargetTable(pstate, stmt->relation,
								  interpretInhOption(stmt->relation->inhOpt),
										 true,
										 ACL_UPDATE);

	/*
	 * the FROM clause is non-standard SQL syntax. We used to be able to do
	 * this with REPLACE in POSTQUEL so we keep the feature.
	 */
	transformFromClause(pstate, stmt->fromClause);

	qry->targetList = transformTargetList(pstate, stmt->targetList);

	qual = transformWhereClause(pstate, stmt->whereClause, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	/*
	 * Top-level aggregates are simply disallowed in UPDATE, per spec. (From
	 * an implementation point of view, this is forced because the implicit
	 * ctid reference would otherwise be an ungrouped variable.)
	 */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in UPDATE")));

	if (pstate->p_hasWindFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in UPDATE")));

	/*
	 * Now we are done with SELECT-like processing, and can get on with
	 * transforming the target list to match the UPDATE target columns.
	 */

	/* Prepare to assign non-conflicting resnos to resjunk attributes */
	if (pstate->p_next_resno <= pstate->p_target_relation->rd_rel->relnatts)
		pstate->p_next_resno = pstate->p_target_relation->rd_rel->relnatts + 1;

	/* Prepare non-junk columns for assignment to target table */
	origTargetList = list_head(stmt->targetList);

	foreach(tl, qry->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		ResTarget  *origTarget;
		int			attrno;

		if (tle->resjunk)
		{
			/*
			 * Resjunk nodes need no additional processing, but be sure they
			 * have resnos that do not match any target columns; else rewriter
			 * or planner might get confused.  They don't need a resname
			 * either.
			 */
			tle->resno = (AttrNumber) pstate->p_next_resno++;
			tle->resname = NULL;
			continue;
		}
		if (origTargetList == NULL)
			elog(ERROR, "UPDATE target count mismatch --- internal error");
		origTarget = (ResTarget *) lfirst(origTargetList);
		Assert(IsA(origTarget, ResTarget));

		attrno = attnameAttNum(pstate->p_target_relation,
							   origTarget->name, true);
		if (attrno == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							origTarget->name,
						 RelationGetRelationName(pstate->p_target_relation)),
					 parser_errposition(pstate, origTarget->location)));

		updateTargetListEntry(pstate, tle, origTarget->name,
							  attrno,
							  origTarget->indirection,
							  origTarget->location);

		origTargetList = lnext(origTargetList);
	}
	if (origTargetList != NULL)
		elog(ERROR, "UPDATE target count mismatch --- internal error");

	return qry;
}

/*
 * transformReturningList -
 *	handle a RETURNING clause in INSERT/UPDATE/DELETE
 */
static List *
transformReturningList(ParseState *pstate, List *returningList)
{
	List	   *rlist;
	int			save_next_resno;
	bool		save_hasAggs;
	int			length_rtable;

	if (returningList == NIL)
		return NIL;				/* nothing to do */

	/*
	 * We need to assign resnos starting at one in the RETURNING list. Save
	 * and restore the main tlist's value of p_next_resno, just in case
	 * someone looks at it later (probably won't happen).
	 */
	save_next_resno = pstate->p_next_resno;
	pstate->p_next_resno = 1;

	/* save other state so that we can detect disallowed stuff */
	save_hasAggs = pstate->p_hasAggs;
	pstate->p_hasAggs = false;
	length_rtable = list_length(pstate->p_rtable);

	/* transform RETURNING identically to a SELECT targetlist */
	rlist = transformTargetList(pstate, returningList);

	/* check for disallowed stuff */

	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in RETURNING")));

	if (pstate->p_hasWindFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use window function in RETURNING")));


	/* no new relation references please */
	if (list_length(pstate->p_rtable) != length_rtable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("RETURNING may not contain references to other relations")));

	/* mark column origins */
	markTargetListOrigins(pstate, rlist);

	/* restore state */
	pstate->p_next_resno = save_next_resno;
	pstate->p_hasAggs = save_hasAggs;

	return rlist;
}

/*
 * transformAlterTableStmt -
 *	transform an Alter Table Statement
 */
static Query *
transformAlterTableStmt(ParseState *pstate, AlterTableStmt *stmt,
						List **extras_before, List **extras_after)
{
	CreateStmtContext cxt;
	Query	   *qry;
	ListCell   *lcmd,
			   *l;
	List	   *newcmds = NIL;
	bool		skipValidation = true;
	AlterTableCmd *newcmd;

	cxt.stmtType = "ALTER TABLE";
	cxt.relation = stmt->relation;
	cxt.inhRelations = NIL;
	cxt.isalter = true;
	cxt.hasoids = false;		/* need not be right */
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
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
	 * statements, so transfer anything we already have into extras_after
	 * immediately.
	 */
	*extras_after = list_concat(cxt.alist, *extras_after);
	cxt.alist = NIL;

	/* Postprocess index and FK constraints */
	transformIndexConstraints(pstate, &cxt);

	transformFKConstraints(pstate, &cxt, skipValidation, true);

	/*
	 * Push any index-creation commands into the ALTER, so that they can be
	 * scheduled nicely by tablecmds.c.
	 */
	foreach(l, cxt.alist)
	{
		Node	   *idxstmt = (Node *) lfirst(l);

		Assert(IsA(idxstmt, IndexStmt));
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddIndex;
		newcmd->def = idxstmt;
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

	/* Update statement's commands list */
	stmt->cmds = newcmds;

	qry = makeNode(Query);
	qry->commandType = CMD_UTILITY;
	qry->utilityStmt = (Node *) stmt;

	*extras_before = list_concat(*extras_before, cxt.blist);
	*extras_after = list_concat(cxt.alist, *extras_after);

	return qry;
}

static Query *
transformDeclareCursorStmt(ParseState *pstate, DeclareCursorStmt *stmt)
{
	Query	   *result = makeNode(Query);
	List	   *extras_before = NIL,
			   *extras_after = NIL;

	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	/*
	 * Don't allow both SCROLL and NO SCROLL to be specified
	 */
	if ((stmt->options & CURSOR_OPT_SCROLL) &&
		(stmt->options & CURSOR_OPT_NO_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("cannot specify both SCROLL and NO SCROLL")));

	stmt->query = (Node *) transformStmt(pstate, stmt->query,
										 &extras_before, &extras_after);

	/* Shouldn't get any extras, since grammar only allows SelectStmt */
	if (extras_before || extras_after)
		elog(ERROR, "unexpected extra stuff in cursor statement");
	if (!IsA(stmt->query, Query) ||
		((Query *) stmt->query)->commandType != CMD_SELECT)
		elog(ERROR, "unexpected non-SELECT command in cursor statement");

	/* But we must explicitly disallow DECLARE CURSOR ... SELECT INTO */
	if (((Query *) stmt->query)->into)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("DECLARE CURSOR may not specify INTO")));

	return result;
}


static Query *
transformPrepareStmt(ParseState *pstate, PrepareStmt *stmt)
{
	Query	   *result = makeNode(Query);
	List	   *argtype_oids;	/* argtype OIDs in a list */
	Oid		   *argtoids = NULL;	/* and as an array */
	int			nargs;
	List	   *queries;
	int			i;

	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	/* Transform list of TypeNames to list (and array) of type OIDs */
	nargs = list_length(stmt->argtypes);

	if (nargs)
	{
		ListCell   *l;

		argtoids = (Oid *) palloc(nargs * sizeof(Oid));
		i = 0;

		foreach(l, stmt->argtypes)
		{
			TypeName   *tn = lfirst(l);
			Oid			toid = typenameTypeId(pstate, tn);

			argtoids[i++] = toid;
		}
	}

	/*
	 * Analyze the statement using these parameter types (any parameters
	 * passed in from above us will not be visible to it), allowing
	 * information about unknown parameters to be deduced from context.
	 */
	queries = parse_analyze_varparams((Node *) stmt->query,
									  pstate->p_sourcetext,
									  &argtoids, &nargs);

	/*
	 * Shouldn't get any extra statements, since grammar only allows
	 * OptimizableStmt
	 */
	if (list_length(queries) != 1)
		elog(ERROR, "unexpected extra stuff in prepared statement");

	/*
	 * Check that all parameter types were determined, and convert the array
	 * of OIDs into a list for storage.
	 */
	argtype_oids = NIL;
	for (i = 0; i < nargs; i++)
	{
		Oid			argtype = argtoids[i];

		if (argtype == InvalidOid || argtype == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of parameter $%d",
							i + 1)));

		argtype_oids = lappend_oid(argtype_oids, argtype);
	}

	stmt->argtype_oids = argtype_oids;
	stmt->query = linitial(queries);
	return result;
}

static Query *
transformExecuteStmt(ParseState *pstate, ExecuteStmt *stmt)
{
	Query	   *result = makeNode(Query);
	List	   *paramtypes;

	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	paramtypes = FetchPreparedStatementParams(stmt->name);

	if (stmt->params || paramtypes)
	{
		int			nparams = list_length(stmt->params);
		int			nexpected = list_length(paramtypes);
		ListCell   *l,
				   *l2;
		int			i = 1;

		if (nparams != nexpected)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("wrong number of parameters for prepared statement \"%s\"",
				   stmt->name),
					 errdetail("Expected %d parameters but got %d.",
							   nexpected, nparams)));

		forboth(l, stmt->params, l2, paramtypes)
		{
			Node	   *expr = lfirst(l);
			Oid			expected_type_id = lfirst_oid(l2);
			Oid			given_type_id;

			expr = transformExpr(pstate, expr);

			/* Cannot contain subselects or aggregates */
			if (pstate->p_hasSubLinks)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot use subquery in EXECUTE parameter")));
			if (pstate->p_hasAggs)
				ereport(ERROR,
						(errcode(ERRCODE_GROUPING_ERROR),
						 errmsg("cannot use aggregate function in EXECUTE parameter")));
			if (pstate->p_hasWindFuncs)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				 		 errmsg("cannot use window function in EXECUTE parameter")));


			given_type_id = exprType(expr);

			expr = coerce_to_target_type(pstate, expr, given_type_id,
										 expected_type_id, -1,
										 COERCION_ASSIGNMENT,
										 COERCE_IMPLICIT_CAST);

			if (expr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("parameter $%d of type %s cannot be coerced to the expected type %s",
								i,
								format_type_be(given_type_id),
								format_type_be(expected_type_id)),
				errhint("You will need to rewrite or cast the expression.")));

			lfirst(l) = expr;
			i++;
		}
	}

	return result;
}

/* exported so planner can check again after rewriting, query pullup, etc */
void
CheckSelectLocking(Query *qry)
{
	if (qry->setOperations)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));
	if (qry->distinctClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with DISTINCT clause")));
	if (qry->groupClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with GROUP BY clause")));
	if (qry->havingQual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("SELECT FOR UPDATE/SHARE is not allowed with HAVING clause")));
	if (qry->hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with aggregate functions")));
	if (qry->hasWindFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with window functions")));
}

/*
 * Transform a FOR UPDATE/SHARE clause
 *
 * This basically involves replacing names by integer relids.
 *
 * NB: if you need to change this, see also markQueryForLocking()
 * in rewriteHandler.c.
 */
static void
transformLockingClause(Query *qry, LockingClause *lc)
{
	List	   *lockedRels = lc->lockedRels;
	ListCell   *l;
	ListCell   *rt;
	Index		i;
	LockingClause *allrels;

	CheckSelectLocking(qry);

	/* make a clause we can pass down to subqueries to select all rels */
	allrels = makeNode(LockingClause);
	allrels->lockedRels = NIL;	/* indicates all rels */
	allrels->forUpdate = lc->forUpdate;
	allrels->noWait = lc->noWait;

	if (lockedRels == NIL)
	{
		/* all regular tables used in query */
		i = 0;
		foreach(rt, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			++i;
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					if(get_rel_relkind(rte->relid) == RELKIND_EXTERNAL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to external tables")));
					
					applyLockingClause(qry, i, lc->forUpdate, lc->noWait);
					rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
					break;
				case RTE_SUBQUERY:

					/*
					 * FOR UPDATE/SHARE of subquery is propagated to all of
					 * subquery's rels
					 */
					transformLockingClause(rte->subquery, allrels);
					break;
				default:
					/* ignore JOIN, SPECIAL, FUNCTION RTEs */
					break;
			}
		}
	}
	else
	{
		/* just the named tables */
		foreach(l, lockedRels)
		{
			char	   *relname = strVal(lfirst(l));

			i = 0;
			foreach(rt, qry->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

				++i;
				if (strcmp(rte->eref->aliasname, relname) == 0)
				{
					switch (rte->rtekind)
					{
						case RTE_RELATION:
							if(get_rel_relkind(rte->relid) == RELKIND_EXTERNAL)
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to external tables")));
							
							applyLockingClause(qry, i,
											   lc->forUpdate, lc->noWait);
							rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
							break;
						case RTE_SUBQUERY:

							/*
							 * FOR UPDATE/SHARE of subquery is propagated to
							 * all of subquery's rels
							 */
							transformLockingClause(rte->subquery, allrels);
							break;
						case RTE_JOIN:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to a join")));
							break;
						case RTE_SPECIAL:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to NEW or OLD")));
							break;
						case RTE_FUNCTION:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to a function")));
							break;
						case RTE_VALUES:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to VALUES")));
							break;
						default:
							elog(ERROR, "unrecognized RTE type: %d",
								 (int) rte->rtekind);
							break;
					}
					break;		/* out of foreach loop */
				}
			}
			if (rt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" in FOR UPDATE/SHARE clause not found in FROM clause",
								relname)));
		}
	}
}

/*
 * Record locking info for a single rangetable item
 */
void
applyLockingClause(Query *qry, Index rtindex, bool forUpdate, bool noWait)
{
	RowMarkClause *rc;

	/* Check for pre-existing entry for same rtindex */
	if ((rc = get_rowmark(qry, rtindex)) != NULL)
	{
		/*
		 * If the same RTE is specified both FOR UPDATE and FOR SHARE, treat
		 * it as FOR UPDATE.  (Reasonable, since you can't take both a shared
		 * and exclusive lock at the same time; it'll end up being exclusive
		 * anyway.)
		 *
		 * We also consider that NOWAIT wins if it's specified both ways. This
		 * is a bit more debatable but raising an error doesn't seem helpful.
		 * (Consider for instance SELECT FOR UPDATE NOWAIT from a view that
		 * internally contains a plain FOR UPDATE spec.)
		 */
		rc->forUpdate |= forUpdate;
		rc->noWait |= noWait;
		return;
	}

	/* Make a new RowMarkClause */
	rc = makeNode(RowMarkClause);
	rc->rti = rtindex;
	rc->forUpdate = forUpdate;
	rc->noWait = noWait;
	qry->rowMarks = lappend(qry->rowMarks, rc);
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

/* Build a FromExpr node */
static FromExpr *
makeFromExpr(List *fromlist, Node *quals)
{
	FromExpr   *f = makeNode(FromExpr);

	f->fromlist = fromlist;
	f->quals = quals;
	return f;
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
	Type		ctype = typenameType(NULL, column->typname);

	ReleaseSysCache(ctype);
}

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

/*
 * Traverse a fully-analyzed tree to verify that parameter symbols
 * match their types.  We need this because some Params might still
 * be UNKNOWN, if there wasn't anything to force their coercion,
 * and yet other instances seen later might have gotten coerced.
 */
static bool
check_parameter_resolution_walker(Node *node,
								  check_parameter_resolution_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
		{
			int			paramno = param->paramid;

			if (paramno <= 0 || /* shouldn't happen, but... */
				paramno > context->numParams)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_PARAMETER),
						 errmsg("there is no parameter $%d", paramno)));

			if (param->paramtype != context->paramTypes[paramno - 1])
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
					 errmsg("could not determine data type of parameter $%d",
							paramno)));
		}
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		return query_tree_walker((Query *) node,
								 check_parameter_resolution_walker,
								 (void *) context, 0);
	}
	return expression_tree_walker(node, check_parameter_resolution_walker,
								  (void *) context);
}

static void
setQryDistributionPolicy(SelectStmt *stmt, Query *qry)
{
	ListCell   *keys = NULL;

	GpPolicy  *policy = NULL;
	int			colindex = 0;
	int			maxattrs = 200;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	Assert(stmt->distributedBy);

	if (stmt->distributedBy)
	{
		/*
		 * We have a DISTRIBUTED BY column list specified by the user
		 * Process it now and set the distribution policy.
		 */
		 
		if (qry->into == NULL)
		{
			elog(ERROR,"DISTRIBUTED BY clause in SELECT statement only makes sense for SELECT INTO");
		}
		
		policy = (GpPolicy *) palloc(sizeof(GpPolicy) + maxattrs * sizeof(policy->attrs[0]));
		policy->ptype = POLICYTYPE_PARTITIONED;
		policy->nattrs = 0;
		policy->attrs[0] = 1;
		 
		if (stmt->distributedBy->length == 1 && (stmt->distributedBy->head == NULL || stmt->distributedBy->head->data.ptr_value == NULL))
		{
			/* distributed randomly */
			qry->intoPolicy = policy;
		}
		else
		foreach(keys, stmt->distributedBy)
		{
			char	   *key = strVal(lfirst(keys));
			bool		found = false;
		 
			AttrNumber	n;

			for(n=1;n<=list_length(qry->targetList);n++)
			{
								 
				TargetEntry *target = get_tle_by_resno(qry->targetList, n);
				colindex = n;

				if (target->resname && strcmp(target->resname, key) == 0)
				{
#if 0
					Oid typeOid = typenameTypeId(target->typname);
					bool isArray = (target->typname->arrayBounds == NIL ? false : true);
															
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
#endif 

					found = true;
					
				} /*if*/
				
				if (found)
					break;
					
			} /*for*/
			
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in 'DISTRIBUTED BY' clause does not exist",
								key)));
		
			policy->attrs[policy->nattrs++] = colindex;

		} /*foreach */
		if (policy->nattrs > 0)
			qry->intoPolicy = policy;
	}
}

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

