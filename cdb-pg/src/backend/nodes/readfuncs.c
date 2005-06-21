/*-------------------------------------------------------------------------
 *
 * readfuncs.c
 *	  Reader functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/readfuncs.c,v 1.175 2004/12/31 21:59:55 pgsql Exp $
 *
 * NOTES
 *	  Path and Plan nodes do not have any readfuncs support, because we
 *	  never have occasion to read them in.	(There was once code here that
 *	  claimed to read them, but it was broken as well as unused.)  We
 *	  never read executor state trees, either.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "nodes/parsenodes.h"
#include "nodes/readfuncs.h"
#include "utils/lsyscache.h"  /* For get_typlenbyval */


/*
 * Macros to simplify reading of different kinds of fields.  Use these
 * wherever possible to reduce the chance for silly typos.	Note that these
 * hard-wire conventions about the names of the local variables in a Read
 * routine.
 */

/* Macros for declaring appropriate local variables */

/* A few guys need only local_node */
#define READ_LOCALS_NO_FIELDS(nodeTypeName) \
	nodeTypeName *local_node = makeNode(nodeTypeName)

/* And a few guys need only the pg_strtok support fields */
#define READ_TEMP_LOCALS()	\
	char	   *token;		\
	int			length

/* ... but most need both */
#define READ_LOCALS(nodeTypeName)			\
	READ_LOCALS_NO_FIELDS(nodeTypeName);	\
	READ_TEMP_LOCALS()

/* Read an integer field (anything written as ":fldname %d") */
#define READ_INT_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = atoi(token)

/* Read an unsigned integer field (anything written as ":fldname %u") */
#define READ_UINT_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = atoui(token)

/* Read an OID field (don't hard-wire assumption that OID is same as uint) */
#define READ_OID_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = atooid(token)

/* Read a long-integer field (anything written as ":fldname %ld") */
#define READ_LONG_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = atol(token)

/* Read a char field (ie, one ascii character) */
#define READ_CHAR_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = token[0]

/* Read an enumerated-type field that was written as an integer code */
#define READ_ENUM_FIELD(fldname, enumtype) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = (enumtype) atoi(token)

/* Read a float field */
#define READ_FLOAT_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = atof(token)

/* Read a boolean field */
#define READ_BOOL_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = strtobool(token)

/* Read a character-string field */
#define READ_STRING_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = nullable_string(token, length)

/* Read a Node field */
#define READ_NODE_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	local_node->fldname = nodeRead(NULL, 0)

/* Write a bitmapset field */
#define READ_BITMAPSET_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	 local_node->fldname = bitmapsetRead()

/* Routine exit */
#define READ_DONE() \
	return local_node

/* Read an integer array (anything written as ":fldname %d %d ...") */
#define READ_INT_ARRAY(fldname, count, Type) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	if ( local_node->count > 0 ) \
	{ \
		int i; \
		local_node->fldname = (Type *)palloc(local_node->count * sizeof(Type)); \
		for(i=0; i<local_node->count; i++) \
		{ \
			token = pg_strtok(&length);		/* get field value */ \
			local_node->fldname[i] = atoi(token); \
		} \
	}

/* Read an Oid array (written as ":fldname %u %u ...") */
#define READ_OID_ARRAY(fldname, count) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	if ( local_node->count > 0 ) \
	{ \
		int i; \
		local_node->fldname = (Oid *)palloc(local_node->count * sizeof(Oid)); \
		for(i=0; i<local_node->count; i++) \
		{ \
			token = pg_strtok(&length);		/* get field value */ \
			local_node->fldname[i] = atooid(token); \
		} \
	}

/*
 * NOTE: use atoi() to read values written with %d, or atoui() to read
 * values written with %u in outfuncs.c.  An exception is OID values,
 * for which use atooid().	(As of 7.1, outfuncs.c writes OIDs as %u,
 * but this will probably change in the future.)
 */
#define atoui(x)  ((unsigned int) strtoul((x), NULL, 10))

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

#define strtobool(x)  ((*(x) == 't') ? true : false)

#define nullable_string(token,length)  \
	((length) == 0 ? NULL : debackslash(token, length))


static Datum readDatum(bool typbyval);


/*
 * _readQuery
 */
static Query *
_readQuery(void)
{
	READ_LOCALS(Query);

	READ_ENUM_FIELD(commandType, CmdType);
	READ_ENUM_FIELD(querySource, QuerySource);
	READ_BOOL_FIELD(canSetTag);
	READ_NODE_FIELD(utilityStmt);
	READ_INT_FIELD(resultRelation);
	READ_NODE_FIELD(into);
	READ_BOOL_FIELD(hasAggs);
	READ_BOOL_FIELD(hasSubLinks);
	READ_NODE_FIELD(rtable);
	READ_NODE_FIELD(jointree);
	READ_NODE_FIELD(rowMarks);
	READ_NODE_FIELD(targetList);
	READ_NODE_FIELD(groupClause);
	READ_NODE_FIELD(havingQual);
	READ_NODE_FIELD(distinctClause);
	READ_NODE_FIELD(sortClause);
	READ_NODE_FIELD(limitOffset);
	READ_NODE_FIELD(limitCount);
	READ_NODE_FIELD(setOperations);
	READ_NODE_FIELD(resultRelations);

	/* planner-internal fields are left zero */

	READ_DONE();
}

/*
 * _readNotifyStmt
 */
static NotifyStmt *
_readNotifyStmt(void)
{
	READ_LOCALS(NotifyStmt);

	READ_NODE_FIELD(relation);

	READ_DONE();
}

/*
 * _readDeclareCursorStmt
 */
static DeclareCursorStmt *
_readDeclareCursorStmt(void)
{
	READ_LOCALS(DeclareCursorStmt);

	READ_STRING_FIELD(portalname);
	READ_INT_FIELD(options);
	READ_NODE_FIELD(query);

	READ_DONE();
}

/*
 * _readSortClause
 */
static SortClause *
_readSortClause(void)
{
	READ_LOCALS(SortClause);

	READ_UINT_FIELD(tleSortGroupRef);
	READ_OID_FIELD(sortop);

	READ_DONE();
}

/*
 * _readGroupClause
 */
static GroupClause *
_readGroupClause(void)
{
	READ_LOCALS(GroupClause);

	READ_UINT_FIELD(tleSortGroupRef);
	READ_OID_FIELD(sortop);

	READ_DONE();
}

/*
 * _readSetOperationStmt
 */
static SetOperationStmt *
_readSetOperationStmt(void)
{
	READ_LOCALS(SetOperationStmt);

	READ_ENUM_FIELD(op, SetOperation);
	READ_BOOL_FIELD(all);
	READ_NODE_FIELD(larg);
	READ_NODE_FIELD(rarg);
	READ_NODE_FIELD(colTypes);

	READ_DONE();
}


/*
 *	Stuff from primnodes.h.
 */

/*
 * _readResdom
 */
static Resdom *
_readResdom(void)
{
	READ_LOCALS(Resdom);

	READ_INT_FIELD(resno);
	READ_OID_FIELD(restype);
	READ_INT_FIELD(restypmod);
	READ_STRING_FIELD(resname);
	READ_UINT_FIELD(ressortgroupref);
	READ_OID_FIELD(resorigtbl);
	READ_INT_FIELD(resorigcol);
	READ_BOOL_FIELD(resjunk);

	READ_DONE();
}

static Alias *
_readAlias(void)
{
	READ_LOCALS(Alias);

	READ_STRING_FIELD(aliasname);
	READ_NODE_FIELD(colnames);

	READ_DONE();
}

static RangeVar *
_readRangeVar(void)
{
	READ_LOCALS(RangeVar);

	local_node->catalogname = NULL;		/* not currently saved in output
										 * format */

	READ_STRING_FIELD(schemaname);
	READ_STRING_FIELD(relname);
	READ_ENUM_FIELD(inhOpt, InhOption);
	READ_BOOL_FIELD(istemp);
	READ_NODE_FIELD(alias);

	READ_DONE();
}

/*
 * _readVar
 */
static Var *
_readVar(void)
{
	READ_LOCALS(Var);

	READ_UINT_FIELD(varno);
	READ_INT_FIELD(varattno);
	READ_OID_FIELD(vartype);
	READ_INT_FIELD(vartypmod);
	READ_UINT_FIELD(varlevelsup);
	READ_UINT_FIELD(varnoold);
	READ_INT_FIELD(varoattno);

	READ_DONE();
}

/*
 * _readConst
 */
static Const *
_readConst(void)
{
	READ_LOCALS(Const);

	READ_OID_FIELD(consttype);
	READ_INT_FIELD(constlen);
	READ_BOOL_FIELD(constbyval);
	READ_BOOL_FIELD(constisnull);

	token = pg_strtok(&length); /* skip :constvalue */
	if (local_node->constisnull)
		token = pg_strtok(&length);		/* skip "<>" */
	else
		local_node->constvalue = readDatum(local_node->constbyval);

	READ_DONE();
}

/*
 * _readConstraint
 */
static Constraint *
_readConstraint(void)
{

	READ_LOCALS(Constraint);

	READ_STRING_FIELD(name);			/* name, or NULL if unnamed */
	token = pg_strtok(&length);			/* skip:  :contype */
	

	token = pg_strtok(&length);

	

	if (strncmp(token, "PRIMARY_KEY", length)==0)
	{
		local_node->contype = CONSTR_PRIMARY;
		/*elog(NOTICE, "primary key");*/
		READ_NODE_FIELD(keys);
		READ_STRING_FIELD(indexspace);
	}
	else if (strncmp(token, "UNIQUE", length)==0)
	{
		local_node->contype = CONSTR_UNIQUE;
		READ_NODE_FIELD(keys);
		READ_STRING_FIELD(indexspace);
	}
	else if (strncmp(token, "CHECK", length)==0)
	{
		local_node->contype = CONSTR_CHECK;
		READ_NODE_FIELD(raw_expr);
		READ_STRING_FIELD(cooked_expr);
	}
	else if (strncmp(token, "DEFAULT", length)==0)
	{
		local_node->contype = CONSTR_DEFAULT;
		READ_NODE_FIELD(raw_expr);
		READ_STRING_FIELD(cooked_expr);
	}
	else if (strncmp(token, "NOT_NULL", length)==0)
	{
		local_node->contype = CONSTR_NOTNULL;
	}

	
	READ_DONE();
}

static IndexStmt *
_readIndexStmt(void)
{
	READ_LOCALS(IndexStmt);

	READ_STRING_FIELD(idxname);
	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(accessMethod);
	READ_STRING_FIELD(tableSpace);
	READ_NODE_FIELD(indexParams);
	READ_NODE_FIELD(whereClause);
	READ_NODE_FIELD(rangetable);
	READ_BOOL_FIELD(unique);
	READ_BOOL_FIELD(primary);
	READ_BOOL_FIELD(isconstraint);
	READ_OID_FIELD(idxOid);

	READ_DONE();
}

static IndexElem *
_readIndexElem(void)
{
	READ_LOCALS(IndexElem);

	READ_STRING_FIELD(name);
	READ_NODE_FIELD(expr);
	READ_NODE_FIELD(opclass);


	READ_DONE();
}

static ViewStmt *
_readViewStmt(void)
{
	READ_LOCALS(ViewStmt);

	READ_NODE_FIELD(view);
	READ_NODE_FIELD(aliases);
	READ_NODE_FIELD(query);
	READ_BOOL_FIELD(replace);

	READ_DONE();
}

static DropStmt *
_readDropStmt(void)
{
	READ_LOCALS(DropStmt);

	READ_NODE_FIELD(objects);
	READ_ENUM_FIELD(removeType,ObjectType);
	READ_ENUM_FIELD(behavior,DropBehavior);

	READ_DONE();
}


static CreateUserStmt *
_readCreateUserStmt(void)
{
	READ_LOCALS(CreateUserStmt);

	READ_STRING_FIELD(user);
	READ_NODE_FIELD(options);

	READ_DONE();
}

static DropUserStmt *
_readDropUserStmt(void)
{
	READ_LOCALS(DropUserStmt);

	READ_NODE_FIELD(users);

	READ_DONE();
	
}

static RenameStmt *
_readRenameStmt(void)
{
	READ_LOCALS(RenameStmt);

	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(object);
	READ_NODE_FIELD(objarg);
	READ_STRING_FIELD(subname);
	READ_STRING_FIELD(newname);
	READ_ENUM_FIELD(renameType,ObjectType);

	READ_DONE();
}


static FuncCall *
_readFuncCall(void)
{
	READ_LOCALS(FuncCall);

	READ_NODE_FIELD(funcname);
	READ_NODE_FIELD(args);
	READ_BOOL_FIELD(agg_star);
	READ_BOOL_FIELD(agg_distinct);
	READ_DONE();
}

static A_Const *
_readAConst(void)
{
	READ_LOCALS(A_Const);  

	token = pg_strtok(&length);	
	local_node->val.type = T_String;
	 
	local_node->val.val.str = palloc(length + 1);
	strcpy(local_node->val.val.str , token);
	
	 
	READ_DONE();
}

/*
 * _readParam
 */
static Param *
_readParam(void)
{
	READ_LOCALS(Param);

	READ_INT_FIELD(paramkind);
	READ_INT_FIELD(paramid);
	READ_STRING_FIELD(paramname);
	READ_OID_FIELD(paramtype);

	READ_DONE();
}

/*
 * _readAggref
 */
static Aggref *
_readAggref(void)
{
	READ_LOCALS(Aggref);

	READ_OID_FIELD(aggfnoid);
	READ_OID_FIELD(aggtype);
	READ_NODE_FIELD(target);
	READ_UINT_FIELD(agglevelsup);
	READ_BOOL_FIELD(aggstar);
	READ_BOOL_FIELD(aggdistinct);

	READ_DONE();
}

/*
 * _readArrayRef
 */
static ArrayRef *
_readArrayRef(void)
{
	READ_LOCALS(ArrayRef);

	READ_OID_FIELD(refrestype);
	READ_OID_FIELD(refarraytype);
	READ_OID_FIELD(refelemtype);
	READ_NODE_FIELD(refupperindexpr);
	READ_NODE_FIELD(reflowerindexpr);
	READ_NODE_FIELD(refexpr);
	READ_NODE_FIELD(refassgnexpr);

	READ_DONE();
}

/*
 * _readFuncExpr
 */
static FuncExpr *
_readFuncExpr(void)
{
	READ_LOCALS(FuncExpr);

	READ_OID_FIELD(funcid);
	READ_OID_FIELD(funcresulttype);
	READ_BOOL_FIELD(funcretset);
	READ_ENUM_FIELD(funcformat, CoercionForm);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readOpExpr
 */
static OpExpr *
_readOpExpr(void)
{
	READ_LOCALS(OpExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	/*
	 * The opfuncid is stored in the textual format primarily for
	 * debugging and documentation reasons.  We want to always read it as
	 * zero to force it to be re-looked-up in the pg_operator entry.  This
	 * ensures that stored rules don't have hidden dependencies on
	 * operators' functions. (We don't currently support an ALTER OPERATOR
	 * command, but might someday.)
	 */
/*	local_node->opfuncid = InvalidOid; */

	READ_OID_FIELD(opresulttype);
	READ_BOOL_FIELD(opretset);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readDistinctExpr
 */
static DistinctExpr *
_readDistinctExpr(void)
{
	READ_LOCALS(DistinctExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	/*
	 * The opfuncid is stored in the textual format primarily for
	 * debugging and documentation reasons.  We want to always read it as
	 * zero to force it to be re-looked-up in the pg_operator entry.  This
	 * ensures that stored rules don't have hidden dependencies on
	 * operators' functions. (We don't currently support an ALTER OPERATOR
	 * command, but might someday.)
	 */
	local_node->opfuncid = InvalidOid;

	READ_OID_FIELD(opresulttype);
	READ_BOOL_FIELD(opretset);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readScalarArrayOpExpr
 */
static ScalarArrayOpExpr *
_readScalarArrayOpExpr(void)
{
	READ_LOCALS(ScalarArrayOpExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	/*
	 * The opfuncid is stored in the textual format primarily for
	 * debugging and documentation reasons.  We want to always read it as
	 * zero to force it to be re-looked-up in the pg_operator entry.  This
	 * ensures that stored rules don't have hidden dependencies on
	 * operators' functions. (We don't currently support an ALTER OPERATOR
	 * command, but might someday.)
	 */
	local_node->opfuncid = InvalidOid;

	READ_BOOL_FIELD(useOr);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readBoolExpr
 */
static BoolExpr *
_readBoolExpr(void)
{
	READ_LOCALS(BoolExpr);

	/* do-it-yourself enum representation */
	token = pg_strtok(&length); /* skip :boolop */
	token = pg_strtok(&length); /* get field value */
	if (strncmp(token, "and", 3) == 0)
		local_node->boolop = AND_EXPR;
	else if (strncmp(token, "or", 2) == 0)
		local_node->boolop = OR_EXPR;
	else if (strncmp(token, "not", 3) == 0)
		local_node->boolop = NOT_EXPR;
	else
		elog(ERROR, "unrecognized boolop \"%.*s\"", length, token);

	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readSubLink
 */
static SubLink *
_readSubLink(void)
{
	READ_LOCALS(SubLink);

	READ_ENUM_FIELD(subLinkType, SubLinkType);
	READ_BOOL_FIELD(useOr);
	READ_NODE_FIELD(lefthand);
	READ_NODE_FIELD(operName);
	READ_NODE_FIELD(operOids);
	READ_NODE_FIELD(subselect);

	READ_DONE();
}

/*
 * _readSubPlan
 */
static SubPlan *
_readSubPlan(void)
{
	READ_LOCALS(SubPlan);

	READ_ENUM_FIELD(subLinkType, SubLinkType);
	READ_BOOL_FIELD(useOr);
	READ_NODE_FIELD(exprs);
	READ_NODE_FIELD(paramIds);
	READ_NODE_FIELD(plan);
	READ_INT_FIELD(plan_id);
	READ_NODE_FIELD(rtable);
	READ_BOOL_FIELD(useHashTable);
	READ_BOOL_FIELD(unknownEqFalse);
	READ_NODE_FIELD(setParam);
	READ_NODE_FIELD(parParam);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readFieldSelect
 */
static FieldSelect *
_readFieldSelect(void)
{
	READ_LOCALS(FieldSelect);

	READ_NODE_FIELD(arg);
	READ_INT_FIELD(fieldnum);
	READ_OID_FIELD(resulttype);
	READ_INT_FIELD(resulttypmod);

	READ_DONE();
}

/*
 * _readFieldStore
 */
static FieldStore *
_readFieldStore(void)
{
	READ_LOCALS(FieldStore);

	READ_NODE_FIELD(arg);
	READ_NODE_FIELD(newvals);
	READ_NODE_FIELD(fieldnums);
	READ_OID_FIELD(resulttype);

	READ_DONE();
}

/*
 * _readRelabelType
 */
static RelabelType *
_readRelabelType(void)
{
	READ_LOCALS(RelabelType);

	READ_NODE_FIELD(arg);
	READ_OID_FIELD(resulttype);
	READ_INT_FIELD(resulttypmod);
	READ_ENUM_FIELD(relabelformat, CoercionForm);

	READ_DONE();
}

/*
 * _readConvertRowtypeExpr
 */
static ConvertRowtypeExpr *
_readConvertRowtypeExpr(void)
{
	READ_LOCALS(ConvertRowtypeExpr);

	READ_NODE_FIELD(arg);
	READ_OID_FIELD(resulttype);
	READ_ENUM_FIELD(convertformat, CoercionForm);

	READ_DONE();
}

/*
 * _readCaseExpr
 */
static CaseExpr *
_readCaseExpr(void)
{
	READ_LOCALS(CaseExpr);

	READ_OID_FIELD(casetype);
	READ_NODE_FIELD(arg);
	READ_NODE_FIELD(args);
	READ_NODE_FIELD(defresult);

	READ_DONE();
}

/*
 * _readCaseWhen
 */
static CaseWhen *
_readCaseWhen(void)
{
	READ_LOCALS(CaseWhen);

	READ_NODE_FIELD(expr);
	READ_NODE_FIELD(result);

	READ_DONE();
}

/*
 * _readCaseTestExpr
 */
static CaseTestExpr *
_readCaseTestExpr(void)
{
	READ_LOCALS(CaseTestExpr);

	READ_OID_FIELD(typeId);
	READ_INT_FIELD(typeMod);

	READ_DONE();
}

/*
 * _readArrayExpr
 */
static ArrayExpr *
_readArrayExpr(void)
{
	READ_LOCALS(ArrayExpr);

	READ_OID_FIELD(array_typeid);
	READ_OID_FIELD(element_typeid);
	READ_NODE_FIELD(elements);
	READ_BOOL_FIELD(multidims);

	READ_DONE();
}

/*
 * _readRowExpr
 */
static RowExpr *
_readRowExpr(void)
{
	READ_LOCALS(RowExpr);

	READ_NODE_FIELD(args);
	READ_OID_FIELD(row_typeid);
	READ_ENUM_FIELD(row_format, CoercionForm);

	READ_DONE();
}

/*
 * _readCoalesceExpr
 */
static CoalesceExpr *
_readCoalesceExpr(void)
{
	READ_LOCALS(CoalesceExpr);

	READ_OID_FIELD(coalescetype);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readNullIfExpr
 */
static NullIfExpr *
_readNullIfExpr(void)
{
	READ_LOCALS(NullIfExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	/*
	 * The opfuncid is stored in the textual format primarily for
	 * debugging and documentation reasons.  We want to always read it as
	 * zero to force it to be re-looked-up in the pg_operator entry.  This
	 * ensures that stored rules don't have hidden dependencies on
	 * operators' functions. (We don't currently support an ALTER OPERATOR
	 * command, but might someday.)
	 */
	local_node->opfuncid = InvalidOid;

	READ_OID_FIELD(opresulttype);
	READ_BOOL_FIELD(opretset);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readNullTest
 */
static NullTest *
_readNullTest(void)
{
	READ_LOCALS(NullTest);

	READ_NODE_FIELD(arg);
	READ_ENUM_FIELD(nulltesttype, NullTestType);

	READ_DONE();
}

/*
 * _readBooleanTest
 */
static BooleanTest *
_readBooleanTest(void)
{
	READ_LOCALS(BooleanTest);

	READ_NODE_FIELD(arg);
	READ_ENUM_FIELD(booltesttype, BoolTestType);

	READ_DONE();
}

/*
 * _readCoerceToDomain
 */
static CoerceToDomain *
_readCoerceToDomain(void)
{
	READ_LOCALS(CoerceToDomain);

	READ_NODE_FIELD(arg);
	READ_OID_FIELD(resulttype);
	READ_INT_FIELD(resulttypmod);
	READ_ENUM_FIELD(coercionformat, CoercionForm);

	READ_DONE();
}

/*
 * _readCoerceToDomainValue
 */
static CoerceToDomainValue *
_readCoerceToDomainValue(void)
{
	READ_LOCALS(CoerceToDomainValue);

	READ_OID_FIELD(typeId);
	READ_INT_FIELD(typeMod);

	READ_DONE();
}

/*
 * _readSetToDefault
 */
static SetToDefault *
_readSetToDefault(void)
{
	READ_LOCALS(SetToDefault);

	READ_OID_FIELD(typeId);
	READ_INT_FIELD(typeMod);

	READ_DONE();
}

/*
 * _readTargetEntry
 */
static TargetEntry *
_readTargetEntry(void)
{
	READ_LOCALS(TargetEntry);

	READ_NODE_FIELD(resdom);
	READ_NODE_FIELD(expr);

	READ_DONE();
}

/*
 * _readRangeTblRef
 */
static RangeTblRef *
_readRangeTblRef(void)
{
	READ_LOCALS(RangeTblRef);

	READ_INT_FIELD(rtindex);

	READ_DONE();
}

/*
 * _readJoinExpr
 */
static JoinExpr *
_readJoinExpr(void)
{
	READ_LOCALS(JoinExpr);

	READ_ENUM_FIELD(jointype, JoinType);
	READ_BOOL_FIELD(isNatural);
	READ_NODE_FIELD(larg);
	READ_NODE_FIELD(rarg);
	READ_NODE_FIELD(using);
	READ_NODE_FIELD(quals);
	READ_NODE_FIELD(alias);
	READ_INT_FIELD(rtindex);

	READ_DONE();
}

/*
 * _readFromExpr
 */
static FromExpr *
_readFromExpr(void)
{
	READ_LOCALS(FromExpr);

	READ_NODE_FIELD(fromlist);
	READ_NODE_FIELD(quals);

	READ_DONE();
}


/*
 *	Stuff from parsenodes.h.
 */

static ColumnDef *
_readColumnDef(void)
{
	READ_LOCALS(ColumnDef);

	READ_STRING_FIELD(colname);
	READ_NODE_FIELD(typename);
	READ_INT_FIELD(inhcount);
	READ_BOOL_FIELD(is_local);
	READ_BOOL_FIELD(is_not_null);
	READ_NODE_FIELD(raw_default);
	READ_STRING_FIELD(cooked_default);
	READ_NODE_FIELD(constraints);
	READ_NODE_FIELD(support);

	READ_DONE();
}

static TypeName *
_readTypeName(void)
{
	READ_LOCALS(TypeName);

	READ_NODE_FIELD(names);
	READ_OID_FIELD(typeid);
	READ_BOOL_FIELD(timezone);
	READ_BOOL_FIELD(setof);
	READ_BOOL_FIELD(pct_type);
	READ_INT_FIELD(typmod);
	READ_NODE_FIELD(arrayBounds);

	READ_DONE();
}

/*
 * _readRangeTblEntry
 */
static RangeTblEntry *
_readRangeTblEntry(void)
{
	READ_LOCALS(RangeTblEntry);

	/* put alias + eref first to make dump more legible */
	READ_NODE_FIELD(alias);
	READ_NODE_FIELD(eref);
	READ_ENUM_FIELD(rtekind, RTEKind);

	switch (local_node->rtekind)
	{
		case RTE_RELATION:
		case RTE_SPECIAL:
			READ_OID_FIELD(relid);
			break;
		case RTE_SUBQUERY:
			READ_NODE_FIELD(subquery);
			break;
		case RTE_FUNCTION:
			READ_NODE_FIELD(funcexpr);
			READ_NODE_FIELD(coldeflist);
			break;
		case RTE_JOIN:
			READ_ENUM_FIELD(jointype, JoinType);
			READ_NODE_FIELD(joinaliasvars);
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d",
				 (int) local_node->rtekind);
			break;
	}

	READ_BOOL_FIELD(inh);
	READ_BOOL_FIELD(inFromCl);
	READ_UINT_FIELD(requiredPerms);
	READ_UINT_FIELD(checkAsUser);

	READ_DONE();
}

/*
 * Cdb additions for serialization support
 */
#include "nodes/plannodes.h"

static void readPlanInfo(Plan *local_node);
static void readScanInfo(Scan *local_node);
static void readJoinInfo(Join *local_node);
static Bitmapset *bitmapsetRead(void);


static CreateStmt *
_readCreateStmt(void)
{
	READ_LOCALS(CreateStmt);

	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(tableElts);
	READ_NODE_FIELD(inhRelations);
	READ_NODE_FIELD(constraints);

	READ_ENUM_FIELD(hasoids,ContainsOids);
	READ_ENUM_FIELD(oncommit,OnCommitAction);
	READ_STRING_FIELD(tablespacename);
	READ_NODE_FIELD(distributedBy);
	READ_OID_FIELD(relOid);
	READ_CHAR_FIELD(relKind);
	local_node->policy = NULL;

	READ_DONE();
}

static CreateSchemaStmt *
_readCreateSchemaStmt(void)
{
	READ_LOCALS(CreateSchemaStmt);

	READ_STRING_FIELD(schemaname);
	READ_STRING_FIELD(authid);
	local_node->schemaElts = 0;
	READ_OID_FIELD(schemaOid);

	READ_DONE();
}

/*
 * _readPlan
 */
static Plan *
_readPlan(void)
{
	READ_LOCALS_NO_FIELDS(Plan);

	readPlanInfo(local_node);

	READ_DONE();
}

/*
 * _readResult
 */
static Result *
_readResult(void)
{
	READ_LOCALS(Result);

	readPlanInfo((Plan *)local_node);

	READ_NODE_FIELD(resconstantqual);

	READ_DONE();
}

/*
 * _readAppend
 */
static Append *
_readAppend(void)
{
	READ_LOCALS(Append);

	readPlanInfo((Plan *)local_node);

	READ_NODE_FIELD(appendplans);
	READ_BOOL_FIELD(isTarget);

	READ_DONE();
}

/*
 * _readScan
 */
static Scan *
_readScan(void)
{
	READ_LOCALS_NO_FIELDS(Scan);

	readScanInfo(local_node);

	READ_DONE();
}

/*
 * _readSeqScan
 */
static SeqScan *
_readSeqScan(void)
{
	READ_LOCALS_NO_FIELDS(SeqScan);

	readScanInfo((Scan *)local_node);

	READ_DONE();
}

/*
 * _readIndexScan
 */
static IndexScan *
_readIndexScan(void)
{
	READ_LOCALS(IndexScan);

	readScanInfo((Scan *)local_node);

	READ_NODE_FIELD(indxid);
	READ_NODE_FIELD(indxqual);
	READ_NODE_FIELD(indxqualorig);
	READ_NODE_FIELD(indxstrategy);
	READ_NODE_FIELD(indxsubtype);
	READ_NODE_FIELD(indxlossy);
	READ_ENUM_FIELD(indxorderdir, ScanDirection);

	READ_DONE();
}

/*
 * _readTidScan
 */
static TidScan *
_readTidScan(void)
{
	READ_LOCALS(TidScan);

	readScanInfo((Scan *)local_node);

	READ_NODE_FIELD(tideval);

	READ_DONE();
}

/*
 * _readSubqueryScan
 */
static SubqueryScan *
_readSubqueryScan(void)
{
	READ_LOCALS(SubqueryScan);

	readScanInfo((Scan *)local_node);

	READ_NODE_FIELD(subplan);

	READ_DONE();
}

/*
 * _readFunctionScan
 */
static FunctionScan *
_readFunctionScan(void)
{
	READ_LOCALS_NO_FIELDS(FunctionScan);

	readScanInfo((Scan *)local_node);

	READ_DONE();
}

/*
 * _readJoin
 */
static Join *
_readJoin(void)
{
	READ_LOCALS_NO_FIELDS(Join);

	readJoinInfo((Join *)local_node);

	READ_DONE();
}

/*
 * _readNestLoop
 */
static NestLoop *
_readNestLoop(void)
{
	READ_LOCALS_NO_FIELDS(NestLoop);

	readJoinInfo((Join *)local_node);

	READ_DONE();
}

/*
 * _readMergeJoin
 */
static MergeJoin *
_readMergeJoin(void)
{
	READ_LOCALS(MergeJoin);

	readJoinInfo((Join *)local_node);

	READ_NODE_FIELD(mergeclauses);

	READ_DONE();
}

/*
 * _readHashJoin
 */
static HashJoin *
_readHashJoin(void)
{
	READ_LOCALS(HashJoin);

	readJoinInfo((Join *)local_node);

	READ_NODE_FIELD(hashclauses);

	READ_DONE();
}

/*
 * _readAgg
 */
static Agg *
_readAgg(void)
{
	READ_LOCALS(Agg);

	readPlanInfo((Plan *)local_node);

	READ_ENUM_FIELD(aggstrategy, AggStrategy);
	READ_INT_FIELD(numCols);
	READ_INT_ARRAY(grpColIdx, numCols, AttrNumber);
	READ_LONG_FIELD(numGroups);

	READ_DONE();
}

/*
 * _readGroup
 */
static Group *
_readGroup(void)
{
	READ_LOCALS(Group);

	readPlanInfo((Plan *)local_node);

	READ_INT_FIELD(numCols);

	READ_INT_ARRAY(grpColIdx, numCols, AttrNumber);
		
	READ_DONE();
}

/*
 * _readMaterial
 */
static Material *
_readMaterial(void)
{
	READ_LOCALS_NO_FIELDS(Material);

	readPlanInfo((Plan *)local_node);

	READ_DONE();
}

/*
 * _readSort
 */
static Sort *
_readSort(void)
{

	READ_LOCALS(Sort);

	readPlanInfo((Plan *)local_node);

	READ_INT_FIELD(numCols);

	READ_INT_ARRAY(sortColIdx, numCols, AttrNumber);

	READ_OID_ARRAY(sortOperators, numCols);

	READ_DONE();
}

/*
 * _readUnique
 */
static Unique *
_readUnique(void)
{
	READ_LOCALS(Unique);

	readPlanInfo((Plan *)local_node);

	READ_INT_FIELD(numCols);

	READ_INT_ARRAY(uniqColIdx, numCols, AttrNumber);

	READ_DONE();
}

/*
 * _readSetOp
 */
static SetOp *
_readSetOp(void)
{
	READ_LOCALS(SetOp);

	readPlanInfo((Plan *)local_node);

	READ_ENUM_FIELD(cmd, SetOpCmd);
	READ_INT_FIELD(numCols);

	READ_INT_ARRAY(dupColIdx, numCols, AttrNumber);

	READ_INT_FIELD(flagColIdx);

	READ_DONE();
}

/*
 * _readLimit
 */
static Limit *
_readLimit(void)
{
	READ_LOCALS(Limit);

	readPlanInfo((Plan *)local_node);

	READ_NODE_FIELD(limitOffset);
	READ_NODE_FIELD(limitCount);

	READ_DONE();
}
	

/*
 * _readHash
 */
static Hash *
_readHash(void)
{
	READ_LOCALS_NO_FIELDS(Hash);

	readPlanInfo((Plan *)local_node);

	READ_DONE();
}

/*
 * _readMotion
 */
static Motion *
_readMotion(void)
{
	READ_LOCALS(Motion);

	readPlanInfo((Plan *)local_node);

	READ_ENUM_FIELD(motionType, MotionType);
	READ_BOOL_FIELD(sendToSecondary);
	READ_BOOL_FIELD(sendSorted);
	READ_INT_FIELD(motionID);
	
	READ_INT_FIELD(numHashSegs);
	READ_INT_ARRAY(hashSegIdx, numHashSegs, int);

	READ_NODE_FIELD(hashExpr);
	READ_NODE_FIELD(hashDataTypes);
	READ_BOOL_FIELD(hashToSelfOnly);

	READ_INT_FIELD(numOutputSegs);
	READ_INT_ARRAY(outputSegIdx, numOutputSegs, int);
	READ_BOOL_FIELD(fixedToSelfOnly);

	READ_INT_FIELD(numSortCols);
	READ_INT_ARRAY(sortColIdx, numSortCols, AttrNumber);
	READ_OID_ARRAY(sortOperators, numSortCols);

	READ_INT_FIELD(numInputSegs);
	READ_INT_ARRAY(inputSegIdx, numInputSegs, int);

	READ_DONE();
}


/*
 * _readFlow
 */
static Flow *
_readFlow(void)
{
	READ_LOCALS(Flow);

	READ_ENUM_FIELD(flotype, FlowType);
	READ_ENUM_FIELD(req_move, Movement);
	READ_ENUM_FIELD(req_xform, Movement);
	READ_INT_FIELD(segindex);
	
	READ_INT_FIELD(numSortCols);
	READ_INT_ARRAY(sortColIdx, numSortCols, AttrNumber);
	READ_OID_ARRAY(sortOperators, numSortCols);

	READ_NODE_FIELD(hashExpr);

	READ_DONE();
}

/*
 * _readFlow
 */
static VacuumStmt *
_readVacuumStmt(void)
{
	READ_LOCALS(VacuumStmt);

	READ_BOOL_FIELD(vacuum);
	READ_BOOL_FIELD(full);
	READ_BOOL_FIELD(analyze);
	READ_BOOL_FIELD(freeze);
	READ_BOOL_FIELD(verbose);
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(va_cols);

	READ_DONE();
}

/*
 * _readRemoteParamData
 */
static RemoteParamData *
_readRemoteParamData(void)
{
	int16 typlen;
	bool typbyval;

	READ_LOCALS(RemoteParamData);

	READ_INT_FIELD(paramData.kind);
	READ_STRING_FIELD(paramData.name);
	READ_INT_FIELD(paramData.id);
	READ_OID_FIELD(paramData.ptype);
	READ_BOOL_FIELD(paramData.isnull);

	get_typlenbyval(local_node->paramData.ptype, &typlen, &typbyval);

	token = pg_strtok(&length); /* skip :value */

	if ( local_node->paramData.isnull )
		token = pg_strtok(&length);		/* skip "<>" */
	else
		local_node->paramData.value = readDatum(typbyval);

	READ_DONE();
}

void readScanInfo(Scan *local_node)
{
	READ_TEMP_LOCALS();

	readPlanInfo((Plan *)local_node);

	READ_UINT_FIELD(scanrelid);
}

void readPlanInfo(Plan *local_node)
{
	READ_TEMP_LOCALS();

	READ_FLOAT_FIELD(startup_cost);
	READ_FLOAT_FIELD(total_cost);
	READ_FLOAT_FIELD(plan_rows);
	READ_INT_FIELD(plan_width);
	READ_NODE_FIELD(targetlist);
	READ_NODE_FIELD(qual);
	READ_NODE_FIELD(lefttree);
	READ_NODE_FIELD(righttree);
	READ_NODE_FIELD(initPlan);
	READ_BITMAPSET_FIELD(extParam);
	READ_BITMAPSET_FIELD(allParam);
	READ_INT_FIELD(nParamExec);
	READ_NODE_FIELD(flow);
	READ_ENUM_FIELD(dispatch, DispatchMethod);
}

void readJoinInfo(Join *local_node)
{
	READ_TEMP_LOCALS();

	readPlanInfo((Plan *)local_node);

	READ_ENUM_FIELD(jointype, JoinType);
	READ_NODE_FIELD(joinqual);
}

Bitmapset  *
bitmapsetRead(void)
{
	READ_TEMP_LOCALS();

	Bitmapset *bms = NULL;
	
	token = pg_strtok(&length);	/* read ( */
	token = pg_strtok(&length);	/* read b */

	/* List of integers */
	for (;;)
	{
		int			val;
		char	   *endptr;

		token = pg_strtok(&length);
		if (token == NULL)
			elog(ERROR, "unterminated bitmapset structure");
		if (token[0] == ')')
			break;
		val = (int) strtol(token, &endptr, 10);
		if (endptr != token + length)
			elog(ERROR, "unrecognized integer: \"%.*s\"", length, token);
		bms = bms_add_member(bms, val);
	}

	return bms;
}

/*
 * Cdb developers added code to improve performance over the
 * linear searching that existed in the postgres version of
 * parseNodeString.  We introduced a struct containing
 * the node type string and read function pointer.
 * We created a sorted array of these for all node types supported
 * by parseNodeString, with cdb extensions.
 * This array is searched for a node type string.   If found,
 * the function pointer is excuted.
 */

/*
 * Typedefs used in binary searching for node type in parseNodeString.
 */

/*
 * ReadFn is the typedef of a read function for a node type.
 */
typedef void * (*ReadFn)(void);

/*
 * ParseNodeInfo is a struct containing the string and read function for a node type.
 */
typedef struct ParseNodeInfo
{
	char	*pzNodeName;
	ReadFn	readNode;
} ParseNodeInfo;

/*
 * infoAr is an array of the ParseNodeInfo for all node type that have read functions.
 * This array MUST be kept in sorted order (based on alphabetical order of the NodeName).
 */
static ParseNodeInfo infoAr[] =
{
	{"AGG", (ReadFn)_readAgg},
	{"AGGREF", (ReadFn)_readAggref},
	{"ALIAS", (ReadFn)_readAlias},
	{"APPEND", (ReadFn)_readAppend},
	{"ARRAY", (ReadFn)_readArrayExpr},
	{"ARRAYREF", (ReadFn)_readArrayRef},
	{"A_CONST", (ReadFn)_readAConst},
	{"BOOLEANTEST", (ReadFn)_readBooleanTest},
	{"BOOLEXPR", (ReadFn)_readBoolExpr},
	{"CASE", (ReadFn)_readCaseExpr},
	{"CASETESTEXPR", (ReadFn)_readCaseTestExpr},
	{"COALESCE", (ReadFn)_readCoalesceExpr},
	{"COERCETODOMAIN", (ReadFn)_readCoerceToDomain},
	{"COERCETODOMAINVALUE", (ReadFn)_readCoerceToDomainValue},
	{"COLUMNDEF", (ReadFn)_readColumnDef},
	{"CONST", (ReadFn)_readConst},
	{"CONSTRAINT", (ReadFn)_readConstraint},
	{"CONVERTROWTYPEEXPR", (ReadFn)_readConvertRowtypeExpr},
	{"CREATESCHEMASTMT", (ReadFn)_readCreateSchemaStmt},
	{"CREATESTMT", (ReadFn)_readCreateStmt},
	{"CREATEUSERSTMT", (ReadFn)_readCreateUserStmt},
	{"DECLARECURSOR", (ReadFn)_readDeclareCursorStmt},
	{"DISTINCTEXPR", (ReadFn)_readDistinctExpr},
	{"DROPSTMT", (ReadFn)_readDropStmt},
	{"DROPUSERSTMT", (ReadFn)_readDropUserStmt},
	{"FIELDSELECT", (ReadFn)_readFieldSelect},
	{"FIELDSTORE", (ReadFn)_readFieldStore},
	{"FLOW", (ReadFn)_readFlow},
	{"FROMEXPR", (ReadFn)_readFromExpr},
	{"FUNCCALL", (ReadFn)_readFuncCall},
	{"FUNCEXPR", (ReadFn)_readFuncExpr},
	{"FUNCTIONSCAN", (ReadFn)_readFunctionScan},
	{"GROUP", (ReadFn)_readGroup},
	{"GROUPCLAUSE", (ReadFn)_readGroupClause},
	{"HASH", (ReadFn)_readHash},
	{"HASHJOIN", (ReadFn)_readHashJoin},
	{"INDEXELEM", (ReadFn)_readIndexElem},
	{"INDEXSCAN", (ReadFn)_readIndexScan},
	{"INDEXSTMT", (ReadFn)_readIndexStmt},
	{"JOIN", (ReadFn)_readJoin},
	{"JOINEXPR", (ReadFn)_readJoinExpr},
	{"LIMIT", (ReadFn)_readLimit},
	{"MATERIAL", (ReadFn)_readMaterial},
	{"MERGEJOIN", (ReadFn)_readMergeJoin},
	{"MOTION", (ReadFn)_readMotion},
	{"NESTLOOP", (ReadFn)_readNestLoop},
	{"NOTIFY", (ReadFn)_readNotifyStmt},
	{"NULLIFEXPR", (ReadFn)_readNullIfExpr},
	{"NULLTEST", (ReadFn)_readNullTest},
	{"OPEXPR", (ReadFn)_readOpExpr},
	{"PARAM", (ReadFn)_readParam},
	{"PLAN", (ReadFn)_readPlan},
	{"QUERY", (ReadFn)_readQuery},
	{"RANGETBLREF", (ReadFn)_readRangeTblRef},
	{"RANGEVAR", (ReadFn)_readRangeVar},
	{"RELABELTYPE", (ReadFn)_readRelabelType},
	{"REMOTEPARAMDATA", (ReadFn)_readRemoteParamData},
	{"RENAMESTMT", (ReadFn)_readRenameStmt},
	{"RESDOM", (ReadFn)_readResdom},
	{"RESULT", (ReadFn)_readResult},
	{"ROW", (ReadFn)_readRowExpr},
	{"RTE", (ReadFn)_readRangeTblEntry},
	{"SCALARARRAYOPEXPR", (ReadFn)_readScalarArrayOpExpr},
	{"SCAN", (ReadFn)_readScan},
	{"SEQSCAN", (ReadFn)_readSeqScan},
	{"SETOP", (ReadFn)_readSetOp},
	{"SETOPERATIONSTMT", (ReadFn)_readSetOperationStmt},
	{"SETTODEFAULT", (ReadFn)_readSetToDefault},
	{"SORT", (ReadFn)_readSort},
	{"SORTCLAUSE", (ReadFn)_readSortClause},
	{"SUBLINK", (ReadFn)_readSubLink},
	{"SUBPLAN", (ReadFn)_readSubPlan},
	{"SUBQUERYSCAN", (ReadFn)_readSubqueryScan},
	{"TARGETENTRY", (ReadFn)_readTargetEntry},
	{"TIDSCAN", (ReadFn)_readTidScan},
	{"TYPENAME", (ReadFn)_readTypeName},
	{"UNIQUE", (ReadFn)_readUnique},
	{"VACUUMSTMT", (ReadFn)_readVacuumStmt},
	{"VAR", (ReadFn)_readVar},
	{"VIEWSTMT", (ReadFn)_readViewStmt},
	{"WHEN", (ReadFn)_readCaseWhen}
};

/*
 * cmpParseNodeInfo is the compare function for ParseNodeInfo, used in bsearch.
 * It compares based on the NodeName.
 */
static int cmpParseNodeInfo( const void *x, const void *y)
{
	const ParseNodeInfo *px = (const ParseNodeInfo *)x;
	const ParseNodeInfo *py = (const ParseNodeInfo *)y;

	return strcmp( px->pzNodeName, py->pzNodeName );
}

/*
 * parseNodeString
 *
 * Given a character string representing a node tree, parseNodeString creates
 * the internal node structure.
 *
 * The string to be read must already have been loaded into pg_strtok().
 */

Node *
parseNodeString(void)
{
	void	   *return_value;
	ParseNodeInfo pni;
	ParseNodeInfo *found;
	char *pztokname;

	READ_TEMP_LOCALS();

	token = pg_strtok(&length);

	/*
	 * Make a string with the token, for use in the binary search
	 * of the infoAr.  In this way, we find the read function
	 * for the node type represented by the token.
	 */
	pztokname = palloc( length + 1 );
	memcpy(pztokname, token, length);
	pztokname[length] = '\0';

	/*
	 * We have to search with a key that is the same data type as we have in the infoAr
	 * i.e. a ParseNodeInfo.  Since the compare function is based on name only,
	 * we can set the readNode element of the key to NULL.
	 */
	pni.pzNodeName = pztokname;
	pni.readNode = NULL;

	found = bsearch( &pni, infoAr, lengthof(infoAr), sizeof(ParseNodeInfo), cmpParseNodeInfo);
	
	pfree(pztokname);

	if ( found == NULL )
	{
		elog(ERROR, "badly formatted node string \"%.32s\"...", token);
		return_value = NULL;	/* keep compiler quiet */
	}
	else
	{
		/*
		* We matched the token with a node type.
		* Call the read function.
		*/
		return_value = (*found->readNode)();
	}

	return (Node *)return_value;
}


/*
 * readDatum
 *
 * Given a string representation of a constant, recreate the appropriate
 * Datum.  The string representation embeds length info, but not byValue,
 * so we must be told that.
 */
static Datum
readDatum(bool typbyval)
{
	Size		length,
				i;
	int			tokenLength;
	char	   *token;
	Datum		res;
	char	   *s;

	/*
	 * read the actual length of the value
	 */
	token = pg_strtok(&tokenLength);
	length = atoui(token);

	token = pg_strtok(&tokenLength);	/* read the '[' */
	if (token == NULL || token[0] != '[')
		elog(ERROR, "expected \"[\" to start datum, but got \"%s\"; length = %lu",
			 token ? (const char *) token : "[NULL]",
			 (unsigned long) length);

	if (typbyval)
	{
		if (length > (Size) sizeof(Datum))
			elog(ERROR, "byval datum but length = %lu",
				 (unsigned long) length);
		res = (Datum) 0;
		s = (char *) (&res);
		for (i = 0; i < (Size) sizeof(Datum); i++)
		{
			token = pg_strtok(&tokenLength);
			s[i] = (char) atoi(token);
		}
	}
	else if (length <= 0)
		res = (Datum) NULL;
	else
	{
		s = (char *) palloc(length);
		for (i = 0; i < length; i++)
		{
			token = pg_strtok(&tokenLength);
			s[i] = (char) atoi(token);
		}
		res = PointerGetDatum(s);
	}

	token = pg_strtok(&tokenLength);	/* read the ']' */
	if (token == NULL || token[0] != ']')
		elog(ERROR, "expected \"]\" to end datum, but got \"%s\"; length = %lu",
			 token ? (const char *) token : "[NULL]",
			 (unsigned long) length);

	return res;
}

