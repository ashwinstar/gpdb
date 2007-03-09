/*-------------------------------------------------------------------------
 *
 * readfuncs.c
 *	  Binary Reader functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * These routines must be exactly the inverse of the routines in
 * outfast.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "nodes/parsenodes.h"

#include "nodes/execnodes.h" /* Just for Slice and SliceTable */
#include "nodes/readfuncs.h"



/*
 * Macros to simplify reading of different kinds of fields.  Use these
 * wherever possible to reduce the chance for silly typos.	
 */


#define READ_LOCALS(nodeTypeName) \
	nodeTypeName *local_node = makeNode(nodeTypeName)


/* Read an integer field  */
#define READ_INT_FIELD(fldname) \
	memcpy(&local_node->fldname, *str, sizeof(int));  (*str)+=sizeof(int)

/* Read an unsigned integer field) */
#define READ_UINT_FIELD(fldname) \
	memcpy(&local_node->fldname, *str, sizeof(int)); (*str)+=sizeof(int)

/* Read an OID field (don't hard-wire assumption that OID is same as uint) */
#define READ_OID_FIELD(fldname) \
	memcpy(&local_node->fldname, *str, sizeof(Oid)); (*str)+=sizeof(Oid)

/* Read a long-integer field  */
#define READ_LONG_FIELD(fldname) \
	memcpy(&local_node->fldname, *str, sizeof(long)); (*str)+=sizeof(long)

/* Read a char field (ie, one ascii character) */
#define READ_CHAR_FIELD(fldname) \
	memcpy(&local_node->fldname, *str, 1); (*str)++

/* Read an enumerated-type field that was written as a short integer code */
#define READ_ENUM_FIELD(fldname, enumtype) \
	{ int16 ent; memcpy(&ent, *str, sizeof(int16));  (*str)+=sizeof(int16);local_node->fldname = (enumtype) ent; }

/* Read a float field */
#define READ_FLOAT_FIELD(fldname) \
	memcpy(&local_node->fldname, *str, sizeof(double)); (*str)+=sizeof(double)

/* Read a boolean field */
#define READ_BOOL_FIELD(fldname) \
	local_node->fldname = (*str)[0] != 0;  Assert((*str)[0]==1 || (*str)[0]==0); (*str)++

/* Read a character-string field */
#define READ_STRING_FIELD(fldname) \
	{ int slen; char * nn = NULL; \
		memcpy(&slen, *str, sizeof(int)); \
		(*str)+=sizeof(int); \
		if (slen>0) { \
		    nn = palloc(slen+1); \
		    memcpy(nn,*str,slen); \
		    (*str)+=(slen); nn[slen]='\0'; \
		} \
		local_node->fldname = nn;  }

/* Read a Node field */
#define READ_NODE_FIELD(fldname) \
	local_node->fldname = readNodeBinary(str)

/* Read a bitmapset field */
#define READ_BITMAPSET_FIELD(fldname) \
	 local_node->fldname = bitmapsetRead(str)

/* Read a dummy field */
#define READ_DUMMY_FIELD(fldname,fldvalue) \
	{ local_node->fldname = (0); /*(*str) += sizeof(int*);*/ }

/* Routine exit */
#define READ_DONE() \
	return local_node

/* Read an integer array  */
#define READ_INT_ARRAY(fldname, count, Type) \
	if ( local_node->count > 0 ) \
	{ \
		int i; \
		local_node->fldname = (Type *)palloc(local_node->count * sizeof(Type)); \
		for(i=0; i<local_node->count; i++) \
		{ \
			memcpy(&local_node->fldname[i], *str, sizeof(Type)); (*str)+=sizeof(Type); \
		} \
	}



/* Read an Trasnaction ID array  */
#define READ_XID_ARRAY(fldname, count) \
	if ( local_node->count > 0 ) \
	{ \
		int i; \
		local_node->fldname = (TransactionId *)palloc(local_node->count * sizeof(TransactionId)); \
		for(i=0; i<local_node->count; i++) \
		{ \
			memcpy(&local_node->fldname[i], *str, sizeof(TransactionId)); (*str)+=sizeof(TransactionId); \
		} \
	}



/* Read an Oid array  */
#define READ_OID_ARRAY(fldname, count) \
	if ( local_node->count > 0 ) \
	{ \
		int i; \
		local_node->fldname = (Oid *)palloc(local_node->count * sizeof(Oid)); \
		for(i=0; i<local_node->count; i++) \
		{ \
			memcpy(&local_node->fldname[i], *str, sizeof(Oid)); (*str)+=sizeof(Oid); \
		} \
	}


static void * readNodeBinary(const char ** str);

static Datum readDatum(const char ** str, bool typbyval);


/*
 * _readQuery
 */
static Query *
_readQuery(const char ** str)
{
	READ_LOCALS(Query);

	READ_ENUM_FIELD(commandType, CmdType); Assert(local_node->commandType <= CMD_NOTHING);	
	READ_ENUM_FIELD(querySource, QuerySource); 	Assert(local_node->querySource <= QSRC_PLANNER);
	READ_BOOL_FIELD(canSetTag);
	READ_NODE_FIELD(utilityStmt);
	READ_INT_FIELD(resultRelation);
	READ_NODE_FIELD(into);
	READ_NODE_FIELD(intoOptions);
	READ_ENUM_FIELD(intoOnCommit, OnCommitAction);
	READ_STRING_FIELD(intoTableSpaceName);
	READ_BOOL_FIELD(hasAggs);
	READ_BOOL_FIELD(hasSubLinks);
	READ_NODE_FIELD(rtable);
	READ_NODE_FIELD(jointree);
	READ_NODE_FIELD(targetList);
	READ_NODE_FIELD(returningList);
	READ_NODE_FIELD(groupClause);
	READ_NODE_FIELD(havingQual);
	READ_NODE_FIELD(distinctClause);
	READ_NODE_FIELD(sortClause);
	READ_NODE_FIELD(limitOffset);
	READ_NODE_FIELD(limitCount);
	READ_NODE_FIELD(rowMarks);
	READ_NODE_FIELD(setOperations);
	READ_NODE_FIELD(resultRelations);
	READ_NODE_FIELD(returningLists);
	                     
    READ_OID_FIELD(intoOidInfo.relOid);
    READ_OID_FIELD(intoOidInfo.comptypeOid);                     
    READ_OID_FIELD(intoOidInfo.toastOid);                  
    READ_OID_FIELD(intoOidInfo.toastIndexOid);                 
    READ_OID_FIELD(intoOidInfo.toastComptypeOid);
    
	local_node->intoPolicy = NULL;
	

	READ_DONE();
}

/*
 * _readNotifyStmt
 */
static NotifyStmt *
_readNotifyStmt(const char ** str)
{
	READ_LOCALS(NotifyStmt);

	READ_NODE_FIELD(relation);

	READ_DONE();
}

/*
 * _readDeclareCursorStmt
 */
static DeclareCursorStmt *
_readDeclareCursorStmt(const char ** str)
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
_readSortClause(const char ** str)
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
_readGroupClause(const char ** str)
{
	READ_LOCALS(GroupClause);

	READ_UINT_FIELD(tleSortGroupRef);
	READ_OID_FIELD(sortop);

	READ_DONE();
}

/*
 * _readRowMarkClause
 */
static RowMarkClause *
_readRowMarkClause(const char ** str)
{
	READ_LOCALS(RowMarkClause);

	READ_UINT_FIELD(rti);
	READ_BOOL_FIELD(forUpdate);
	READ_BOOL_FIELD(noWait);

	READ_DONE();
}

/*
 * _readSetOperationStmt
 */
static SetOperationStmt *
_readSetOperationStmt(const char ** str)
{
	READ_LOCALS(SetOperationStmt);

	READ_ENUM_FIELD(op, SetOperation);
	READ_BOOL_FIELD(all);
	READ_NODE_FIELD(larg);
	READ_NODE_FIELD(rarg);
	READ_NODE_FIELD(colTypes);
	READ_NODE_FIELD(colTypmods);

	READ_DONE();
}


/*
 *	Stuff from primnodes.h.
 */

static Alias *
_readAlias(const char ** str)
{
	READ_LOCALS(Alias);

	READ_STRING_FIELD(aliasname);
	READ_NODE_FIELD(colnames);

	READ_DONE();
}

static RangeVar *
_readRangeVar(const char ** str)
{
	READ_LOCALS(RangeVar);

	local_node->catalogname = NULL;		/* not currently saved in output
										 * format */

	READ_STRING_FIELD(schemaname);
	READ_STRING_FIELD(relname);
	READ_ENUM_FIELD(inhOpt, InhOption); Assert(local_node->inhOpt <= INH_DEFAULT);
	READ_BOOL_FIELD(istemp);
	READ_NODE_FIELD(alias);

	READ_DONE();
}

/*
 * _readVar
 */
static Var *
_readVar(const char ** str)
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
_readConst(const char ** str)
{
	READ_LOCALS(Const);

	READ_OID_FIELD(consttype);
	READ_INT_FIELD(constlen);
	READ_BOOL_FIELD(constbyval);
	READ_BOOL_FIELD(constisnull);

	if (local_node->constisnull)
		local_node->constvalue = 0;
	else
		local_node->constvalue = readDatum(str,local_node->constbyval);

	READ_DONE();
}

/*
 * _readConstraint
 */
static Constraint *
_readConstraint(const char ** str)
{

	READ_LOCALS(Constraint);

	READ_STRING_FIELD(name);			/* name, or NULL if unnamed */
	
	READ_ENUM_FIELD(contype,ConstrType);
	Assert(local_node->contype <= CONSTR_ATTR_IMMEDIATE);
	 

	switch (local_node->contype)
	{
		case CONSTR_UNIQUE:
		case CONSTR_PRIMARY:
			READ_NODE_FIELD(keys);
			READ_NODE_FIELD(options);
			READ_STRING_FIELD(indexspace);
		break;
		
		case CONSTR_CHECK:
		case CONSTR_DEFAULT:
			READ_NODE_FIELD(raw_expr);
			READ_STRING_FIELD(cooked_expr);
		break;
		
		case CONSTR_NULL:
		case CONSTR_NOTNULL:
		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
		break;
		
		default:
			elog(WARNING,"Can't deserialize constraint %d",(int)local_node->contype);
			break;
	}

	
	READ_DONE();
}

static IndexStmt *
_readIndexStmt(const char ** str)
{
	READ_LOCALS(IndexStmt);

	READ_STRING_FIELD(idxname);
	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(accessMethod);
	READ_STRING_FIELD(tableSpace);
	READ_NODE_FIELD(indexParams);
	READ_NODE_FIELD(options);
	READ_NODE_FIELD(whereClause);
	READ_NODE_FIELD(rangetable);
	READ_BOOL_FIELD(unique);
	READ_BOOL_FIELD(primary);
	READ_BOOL_FIELD(isconstraint);
	READ_BOOL_FIELD(concurrent);
	READ_NODE_FIELD(idxOids);

	READ_DONE();
}

static IndexElem *
_readIndexElem(const char ** str)
{
	READ_LOCALS(IndexElem);

	READ_STRING_FIELD(name);
	READ_NODE_FIELD(expr);
	READ_NODE_FIELD(opclass);


	READ_DONE();
}

static ReindexStmt *
_readReindexStmt(const char ** str)
{
	READ_LOCALS(ReindexStmt);
	
	READ_ENUM_FIELD(kind,ObjectType); Assert(local_node->kind <= OBJECT_VIEW);
	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(name);
	READ_BOOL_FIELD(do_system);
	READ_BOOL_FIELD(do_user);
	
	READ_DONE();
	
}

static ViewStmt *
_readViewStmt(const char ** str)
{
	READ_LOCALS(ViewStmt);

	READ_NODE_FIELD(view);
	READ_NODE_FIELD(aliases);
	READ_NODE_FIELD(query);
	READ_BOOL_FIELD(replace);
	READ_OID_FIELD(relOid);
	READ_OID_FIELD(comptypeOid);

	READ_DONE();
}

static RuleStmt *
_readRuleStmt(const char ** str)
{
	READ_LOCALS(RuleStmt);

	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(rulename);	
	READ_NODE_FIELD(whereClause);
	READ_ENUM_FIELD(event,CmdType);
	READ_BOOL_FIELD(instead);
	READ_NODE_FIELD(actions);	
	READ_BOOL_FIELD(replace);	
	
	READ_DONE();
}

static DropStmt *
_readDropStmt(const char ** str)
{
	READ_LOCALS(DropStmt);

	READ_NODE_FIELD(objects);
	READ_ENUM_FIELD(removeType,ObjectType);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
	READ_BOOL_FIELD(missing_ok);

	READ_DONE();
}

static DropPropertyStmt *
_readDropPropertyStmt(const char ** str)
{
	READ_LOCALS(DropPropertyStmt);

	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(property);
	READ_ENUM_FIELD(removeType,ObjectType);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
	READ_BOOL_FIELD(missing_ok);

	READ_DONE();
}

static TruncateStmt *
_readTruncateStmt(const char ** str)
{
	READ_LOCALS(TruncateStmt);

	READ_NODE_FIELD(relations);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);

	READ_DONE();
}

static AlterTableStmt *
_readAlterTableStmt(const char ** str)
{
	int m;
	READ_LOCALS(AlterTableStmt);

	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(cmds);
	READ_ENUM_FIELD(relkind, ObjectType); Assert(local_node->relkind <= OBJECT_VIEW);
	READ_INT_FIELD(oidInfoCount);
	local_node->oidInfo = NULL;
	if (local_node->oidInfoCount > 0)
	{
		local_node->oidInfo = palloc0(sizeof(TableOidInfo) * local_node->oidInfoCount);
		for (m = 0; m < local_node->oidInfoCount; m++)
		{
			READ_OID_FIELD(oidInfo[m].relOid);
			READ_OID_FIELD(oidInfo[m].comptypeOid);
			READ_OID_FIELD(oidInfo[m].toastOid);
			READ_OID_FIELD(oidInfo[m].toastIndexOid);
			READ_OID_FIELD(oidInfo[m].toastComptypeOid);
		}
	}
	
	READ_DONE();
}

static AlterTableCmd *
_readAlterTableCmd(const char ** str)
{
	READ_LOCALS(AlterTableCmd);

	READ_ENUM_FIELD(subtype, AlterTableType);
	READ_STRING_FIELD(name);
	READ_NODE_FIELD(def);
	READ_NODE_FIELD(transform);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
	
	READ_DONE();
}


static CreateRoleStmt *
_readCreateRoleStmt(const char ** str)
{
	READ_LOCALS(CreateRoleStmt);
	
	READ_ENUM_FIELD(stmt_type, RoleStmtType);
	READ_STRING_FIELD(role);
	READ_NODE_FIELD(options);
	READ_OID_FIELD(roleOid);

	READ_DONE();
}

static DropRoleStmt *
_readDropRoleStmt(const char ** str)
{
	READ_LOCALS(DropRoleStmt);

	READ_NODE_FIELD(roles);
	READ_BOOL_FIELD(missing_ok);

	READ_DONE();
	
}

static  AlterRoleStmt *
_readAlterRoleStmt(const char ** str)
{
	READ_LOCALS(AlterRoleStmt);

	READ_STRING_FIELD(role);
	READ_NODE_FIELD(options);
	READ_INT_FIELD(action);
	READ_DONE();
	
}

static  AlterRoleSetStmt *
_readAlterRoleSetStmt(const char ** str)
{
	READ_LOCALS(AlterRoleSetStmt);

	READ_STRING_FIELD(role);
	READ_STRING_FIELD(variable);
	READ_NODE_FIELD(value);
	
	READ_DONE();
	
}

static AlterObjectSchemaStmt *
_readAlterObjectSchemaStmt(const char ** str)
{
	READ_LOCALS(AlterObjectSchemaStmt);
	
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(object);
	READ_NODE_FIELD(objarg);
	READ_STRING_FIELD(addname);
	READ_STRING_FIELD(newschema);
	READ_ENUM_FIELD(objectType,ObjectType); Assert(local_node->objectType <= OBJECT_VIEW);
	
	READ_DONE();
}



static AlterOwnerStmt *
_readAlterOwnerStmt(const char ** str)
{
	READ_LOCALS(AlterOwnerStmt);

	READ_ENUM_FIELD(objectType,ObjectType); Assert(local_node->objectType <= OBJECT_VIEW);
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(object);
	READ_NODE_FIELD(objarg);
	READ_STRING_FIELD(addname);
	READ_STRING_FIELD(newowner);

	READ_DONE();
}


static RenameStmt *
_readRenameStmt(const char ** str)
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
_readFuncCall(const char ** str)
{
	READ_LOCALS(FuncCall);

	READ_NODE_FIELD(funcname);
	READ_NODE_FIELD(args);
	READ_BOOL_FIELD(agg_star);
	READ_BOOL_FIELD(agg_distinct);
	READ_INT_FIELD(location);
	READ_DONE();
}

static DefElem *
_readDefElem(const char ** str)
{
	READ_LOCALS(DefElem);

	READ_STRING_FIELD(defname);
	READ_NODE_FIELD(arg);
	READ_DONE();
}

static A_Const *
_readAConst(const char ** str)
{
	READ_LOCALS(A_Const);

	READ_ENUM_FIELD(val.type, NodeTag);
	 

	
	switch (local_node->val.type)
	{
		case T_Integer:
			memcpy(&local_node->val.val.ival, *str, sizeof(long)); (*str)+=sizeof(long);
			break;
		case T_Float:
		case T_String:
		case T_BitString:
		{
			int slen; char * nn;  
			memcpy(&slen, *str, sizeof(int));  
			(*str)+=sizeof(int);  
			nn = palloc(slen+1);   
			memcpy(nn,*str,slen); 
			nn[slen] = '\0'; 
			local_node->val.val.str = nn; (*str)+=slen;
		}
			break;
	 	case T_Null:
	 	default:
	 		break;
	}
		
	
	local_node->typname = NULL;
	READ_NODE_FIELD(typname);
	READ_DONE();
}


static A_Expr *
_readAExpr(const char ** str)
{
	READ_LOCALS(A_Expr);
	
	READ_ENUM_FIELD(kind, A_Expr_Kind);
	
	Assert(local_node->kind <= AEXPR_IN);
	
	switch (local_node->kind)
	{
		case AEXPR_OP:

			READ_NODE_FIELD(name);
			break;
		case AEXPR_AND:

			break;
		case AEXPR_OR:

			break;
		case AEXPR_NOT:
			
			break;
		case AEXPR_OP_ANY:
		
			READ_NODE_FIELD(name);
		
			break;
		case AEXPR_OP_ALL:
	
			READ_NODE_FIELD(name);
		
			break;
		case AEXPR_DISTINCT:

			READ_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
	
			READ_NODE_FIELD(name);
			break;
		case AEXPR_OF:
		
			READ_NODE_FIELD(name);
			break;
		case AEXPR_IN:
		
			READ_NODE_FIELD(name);
			break;
		default:
			elog(ERROR,"Unable to understand A_Expr node ");
			break;
	}

 
	

	READ_NODE_FIELD(lexpr);
	READ_NODE_FIELD(rexpr);
	READ_INT_FIELD(location);
	
	READ_DONE();
}

/*
 * _readParam
 */
static Param *
_readParam(const char ** str)
{
	READ_LOCALS(Param);

	READ_ENUM_FIELD(paramkind, ParamKind);
	READ_INT_FIELD(paramid);
	READ_OID_FIELD(paramtype);

	READ_DONE();
}

/*
 * _readAggref
 */
static Aggref *
_readAggref(const char ** str)
{
	READ_LOCALS(Aggref);

	READ_OID_FIELD(aggfnoid);
	READ_OID_FIELD(aggtype);
	READ_NODE_FIELD(args);
	READ_UINT_FIELD(agglevelsup);
	READ_BOOL_FIELD(aggstar);
	READ_BOOL_FIELD(aggdistinct);
	READ_ENUM_FIELD(aggstage, AggStage);



	READ_DONE();
}

/*
 * _readArrayRef
 */
static ArrayRef *
_readArrayRef(const char ** str)
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
_readFuncExpr(const char ** str)
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
_readOpExpr(const char ** str)
{
	READ_LOCALS(OpExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	/*
	 * The opfuncid is stored in the textual format primarily for debugging
	 * and documentation reasons.  We want to always read it as zero to force
	 * it to be re-looked-up in the pg_operator entry.	This ensures that
	 * stored rules don't have hidden dependencies on operators' functions.
	 * (We don't currently support an ALTER OPERATOR command, but might
	 * someday.)
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
_readDistinctExpr(const char ** str)
{
	READ_LOCALS(DistinctExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	READ_OID_FIELD(opresulttype);
	READ_BOOL_FIELD(opretset);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readScalarArrayOpExpr
 */
static ScalarArrayOpExpr *
_readScalarArrayOpExpr(const char ** str)
{
	READ_LOCALS(ScalarArrayOpExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	READ_BOOL_FIELD(useOr);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readBoolExpr
 */
static BoolExpr *
_readBoolExpr(const char ** str)
{
	READ_LOCALS(BoolExpr);

	READ_ENUM_FIELD(boolop, BoolExprType);

	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readSubLink
 */
static SubLink *
_readSubLink(const char ** str)
{
	READ_LOCALS(SubLink);

	READ_ENUM_FIELD(subLinkType, SubLinkType);
	READ_NODE_FIELD(testexpr);
	READ_NODE_FIELD(operName);
	READ_NODE_FIELD(subselect);

	READ_DONE();
}

/*
 * _readSubPlan
 */
static SubPlan *
_readSubPlan(const char ** str)
{
	READ_LOCALS(SubPlan);

    READ_INT_FIELD(qDispSliceId);   /*CDB*/
	READ_ENUM_FIELD(subLinkType, SubLinkType);
	READ_NODE_FIELD(testexpr);
	READ_NODE_FIELD(paramIds);
	READ_NODE_FIELD(plan);
	READ_INT_FIELD(plan_id);
	READ_NODE_FIELD(rtable);
	READ_BOOL_FIELD(useHashTable);
	READ_BOOL_FIELD(unknownEqFalse);
	READ_BOOL_FIELD(is_initplan); /*CDB*/
	READ_BOOL_FIELD(is_multirow); /*CDB*/
	READ_NODE_FIELD(setParam);
	READ_NODE_FIELD(parParam);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readFieldSelect
 */
static FieldSelect *
_readFieldSelect(const char ** str)
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
_readFieldStore(const char ** str)
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
_readRelabelType(const char ** str)
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
_readConvertRowtypeExpr(const char ** str)
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
_readCaseExpr(const char ** str)
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
_readCaseWhen(const char ** str)
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
_readCaseTestExpr(const char ** str)
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
_readArrayExpr(const char ** str)
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
_readRowExpr(const char ** str)
{
	READ_LOCALS(RowExpr);

	READ_NODE_FIELD(args);
	READ_OID_FIELD(row_typeid);
	READ_ENUM_FIELD(row_format, CoercionForm);

	READ_DONE();
}

/*
 * _readRowCompareExpr
 */
static RowCompareExpr *
_readRowCompareExpr(const char ** str)
{
	READ_LOCALS(RowCompareExpr);

	READ_ENUM_FIELD(rctype, RowCompareType);
	READ_NODE_FIELD(opnos);
	READ_NODE_FIELD(opclasses);
	READ_NODE_FIELD(largs);
	READ_NODE_FIELD(rargs);

	READ_DONE();
}

/*
 * _readCoalesceExpr
 */
static CoalesceExpr *
_readCoalesceExpr(const char ** str)
{
	READ_LOCALS(CoalesceExpr);

	READ_OID_FIELD(coalescetype);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readMinMaxExpr
 */
static MinMaxExpr *
_readMinMaxExpr(const char ** str)
{
	READ_LOCALS(MinMaxExpr);

	READ_OID_FIELD(minmaxtype);
	READ_ENUM_FIELD(op, MinMaxOp);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readNullIfExpr
 */
static NullIfExpr *
_readNullIfExpr(const char ** str)
{
	READ_LOCALS(NullIfExpr);

	READ_OID_FIELD(opno);
	READ_OID_FIELD(opfuncid);

	READ_OID_FIELD(opresulttype);
	READ_BOOL_FIELD(opretset);
	READ_NODE_FIELD(args);

	READ_DONE();
}

/*
 * _readNullTest
 */
static NullTest *
_readNullTest(const char ** str)
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
_readBooleanTest(const char ** str)
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
_readCoerceToDomain(const char ** str)
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
_readCoerceToDomainValue(const char ** str)
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
_readSetToDefault(const char ** str)
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
_readTargetEntry(const char ** str)
{
	READ_LOCALS(TargetEntry);

	READ_NODE_FIELD(expr);
	READ_INT_FIELD(resno);
	READ_STRING_FIELD(resname);
	READ_UINT_FIELD(ressortgroupref);
	READ_OID_FIELD(resorigtbl);
	READ_INT_FIELD(resorigcol);
	READ_BOOL_FIELD(resjunk);

	READ_DONE();
}

/*
 * _readRangeTblRef
 */
static RangeTblRef *
_readRangeTblRef(const char ** str)
{
	READ_LOCALS(RangeTblRef);

	READ_INT_FIELD(rtindex);

	READ_DONE();
}

/*
 * _readJoinExpr
 */
static JoinExpr *
_readJoinExpr(const char ** str)
{
	READ_LOCALS(JoinExpr);

	READ_ENUM_FIELD(jointype, JoinType);
	READ_BOOL_FIELD(isNatural);
	READ_NODE_FIELD(larg);
	READ_NODE_FIELD(rarg);
	READ_NODE_FIELD(usingClause);   /*CDB*/
	READ_NODE_FIELD(quals);
	READ_NODE_FIELD(alias);
	READ_INT_FIELD(rtindex);

	READ_DONE();
}

/*
 * _readFromExpr
 */
static FromExpr *
_readFromExpr(const char ** str)
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
_readColumnDef(const char ** str)
{
	READ_LOCALS(ColumnDef);

	READ_STRING_FIELD(colname);
	READ_NODE_FIELD(typname);
	READ_INT_FIELD(inhcount);
	READ_BOOL_FIELD(is_local);
	READ_BOOL_FIELD(is_not_null);
	READ_NODE_FIELD(raw_default);
	READ_STRING_FIELD(cooked_default);
	READ_NODE_FIELD(constraints);

	READ_DONE();
}

static ColumnRef *
_readColumnRef(const char ** str)
{
	READ_LOCALS(ColumnRef);

	READ_NODE_FIELD(fields);
	READ_INT_FIELD(location);

	READ_DONE();
}

static TypeName *
_readTypeName(const char ** str)
{
	READ_LOCALS(TypeName);

	READ_NODE_FIELD(names);
	READ_OID_FIELD(typid);
	READ_BOOL_FIELD(timezone);
	READ_BOOL_FIELD(setof);
	READ_BOOL_FIELD(pct_type);
	READ_INT_FIELD(typmod);
	READ_NODE_FIELD(arrayBounds);
	READ_INT_FIELD(location);

	READ_DONE();
}

static TypeCast *
_readTypeCast(const char ** str)
{
	READ_LOCALS(TypeCast);

	READ_NODE_FIELD(arg);
	READ_NODE_FIELD(typname);

	READ_DONE();
}


/*
 * _readRangeTblEntry
 */
static RangeTblEntry *
_readRangeTblEntry(const char ** str)
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
			READ_NODE_FIELD(funccoltypes);
			READ_NODE_FIELD(funccoltypmods);
			break;
		case RTE_VALUES:
			READ_NODE_FIELD(values_lists);
			break;
		case RTE_JOIN:
			READ_ENUM_FIELD(jointype, JoinType);
			READ_NODE_FIELD(joinaliasvars);
			break;
        case RTE_VOID:                                                  /*CDB*/
            break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d",
				 (int) local_node->rtekind);
			break;
	}

	READ_BOOL_FIELD(inh);
	READ_BOOL_FIELD(inFromCl);
	READ_UINT_FIELD(requiredPerms);
	READ_OID_FIELD(checkAsUser);

	READ_DONE();
}

/*
 * MPP additions for serialization support
 */
#include "nodes/plannodes.h"

static void readPlanInfo(const char ** str, Plan *local_node);
static void readScanInfo(const char ** str, Scan *local_node);
static void readJoinInfo(const char ** str, Join *local_node);
static Bitmapset *bitmapsetRead(const char ** str);


static CreateStmt *
_readCreateStmt(const char ** str)
{
	READ_LOCALS(CreateStmt);

	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(tableElts);
	READ_NODE_FIELD(inhRelations);
	READ_NODE_FIELD(constraints);
	READ_NODE_FIELD(options);
	READ_ENUM_FIELD(oncommit,OnCommitAction);
	READ_STRING_FIELD(tablespacename);
	READ_NODE_FIELD(distributedBy);
	READ_OID_FIELD(oidInfo.relOid);
	READ_OID_FIELD(oidInfo.comptypeOid);
	READ_OID_FIELD(oidInfo.toastOid);
	READ_OID_FIELD(oidInfo.toastIndexOid);
	READ_OID_FIELD(oidInfo.toastComptypeOid);
	READ_CHAR_FIELD(relKind);
	local_node->policy = NULL;
	
	/*
	 * Some extra checks to make sure we didn't get lost
	 * during serialization/deserialization
	 */
	Assert(local_node->relKind == RELKIND_INDEX || 
			local_node->relKind == RELKIND_RELATION || 
			local_node->relKind == RELKIND_SEQUENCE || 
			local_node->relKind == RELKIND_UNCATALOGED || 
			local_node->relKind == RELKIND_TOASTVALUE || 
			local_node->relKind == RELKIND_VIEW ||
			local_node->relKind == RELKIND_EXTERNAL || 
			local_node->relKind == RELKIND_COMPOSITE_TYPE); 
	Assert(local_node->oncommit <= ONCOMMIT_DROP);

	READ_DONE();
}

static FkConstraint *
_readFkConstraint(const char ** str)
{
	READ_LOCALS(FkConstraint);

	READ_STRING_FIELD(constr_name);
	READ_NODE_FIELD(pktable);
	READ_NODE_FIELD(fk_attrs);
	READ_NODE_FIELD(pk_attrs);
	READ_CHAR_FIELD(fk_matchtype);
	READ_CHAR_FIELD(fk_upd_action);
	READ_CHAR_FIELD(fk_del_action);
	READ_BOOL_FIELD(deferrable);
	READ_BOOL_FIELD(initdeferred);
	READ_BOOL_FIELD(skip_validation);
	READ_OID_FIELD(trig1Oid);
	READ_OID_FIELD(trig2Oid);
	READ_OID_FIELD(trig3Oid);
	READ_OID_FIELD(trig4Oid);
	
	READ_DONE();
}

static CreateSchemaStmt *
_readCreateSchemaStmt(const char ** str)
{
	READ_LOCALS(CreateSchemaStmt);

	READ_STRING_FIELD(schemaname);
	READ_STRING_FIELD(authid);
	local_node->schemaElts = 0;
	READ_OID_FIELD(schemaOid);

	READ_DONE();
}


static CreatePLangStmt *
_readCreatePLangStmt(const char ** str)
{
	READ_LOCALS(CreatePLangStmt);
	
	READ_STRING_FIELD(plname);
	READ_NODE_FIELD(plhandler);
	READ_NODE_FIELD(plvalidator);
	READ_BOOL_FIELD(pltrusted);
	
	READ_DONE();

}

static DropPLangStmt *
_readDropPLangStmt(const char ** str)
{
	READ_LOCALS(DropPLangStmt);

	READ_STRING_FIELD(plname);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
	READ_BOOL_FIELD(missing_ok);
	
	READ_DONE();

}

static CreateSeqStmt *
_readCreateSeqStmt(const char ** str)
{
	READ_LOCALS(CreateSeqStmt);
	READ_NODE_FIELD(sequence);
	READ_NODE_FIELD(options);
	
	READ_OID_FIELD(relOid);
	READ_OID_FIELD(comptypeOid);
	
	READ_DONE();
}

static AlterSeqStmt *
_readAlterSeqStmt(const char ** str)
{
	READ_LOCALS(AlterSeqStmt);
	READ_NODE_FIELD(sequence);
	READ_NODE_FIELD(options);

	READ_DONE();
}

static ClusterStmt *
_readClusterStmt(const char ** str)
{
	READ_LOCALS(ClusterStmt);

	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(indexname);
	READ_OID_FIELD(oidInfo.relOid);
	READ_OID_FIELD(oidInfo.comptypeOid);
	READ_OID_FIELD(oidInfo.toastOid);
	READ_OID_FIELD(oidInfo.toastIndexOid);
	READ_OID_FIELD(oidInfo.toastComptypeOid);
	
	READ_DONE();
}

static CreatedbStmt *
_readCreatedbStmt(const char ** str)
{
	READ_LOCALS(CreatedbStmt);
	READ_STRING_FIELD(dbname);
	READ_NODE_FIELD(options);
	
	READ_OID_FIELD(dbOid);
	
	READ_DONE();
}

static DropdbStmt *
_readDropdbStmt(const char ** str)
{
	READ_LOCALS(DropdbStmt);
	
	READ_STRING_FIELD(dbname);
	READ_BOOL_FIELD(missing_ok);
	
	READ_DONE();
}

static CreateDomainStmt *
_readCreateDomainStmt(const char ** str)
{
	READ_LOCALS(CreateDomainStmt);

	READ_NODE_FIELD(domainname);
	READ_NODE_FIELD(typname);
	READ_NODE_FIELD(constraints);
	READ_OID_FIELD(domainOid);
		
	READ_DONE();
}

static AlterDomainStmt *
_readAlterDomainStmt(const char ** str)
{
	READ_LOCALS(AlterDomainStmt);

	READ_CHAR_FIELD(subtype);
	READ_NODE_FIELD(typname);
	READ_STRING_FIELD(name);
	READ_NODE_FIELD(def);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
		
	READ_DONE();
}

static CreateFunctionStmt *
_readCreateFunctionStmt(const char ** str)
{
	READ_LOCALS(CreateFunctionStmt);
	READ_BOOL_FIELD(replace);
	READ_NODE_FIELD(funcname);
	READ_NODE_FIELD(parameters);
	READ_NODE_FIELD(returnType);
	READ_NODE_FIELD(options);
	READ_NODE_FIELD(withClause);
	READ_OID_FIELD(funcOid);
	READ_OID_FIELD(shelltypeOid);
			
	READ_DONE();
}

static FunctionParameter *
_readFunctionParameter(const char ** str)
{
	READ_LOCALS(FunctionParameter);
	READ_STRING_FIELD(name);
	READ_NODE_FIELD(argType);
	READ_ENUM_FIELD(mode, FunctionParameterMode);
			
	READ_DONE();
}

static RemoveFuncStmt *
_readRemoveFuncStmt(const char ** str)
{
	READ_LOCALS(RemoveFuncStmt);
	READ_ENUM_FIELD(kind,ObjectType); Assert(local_node->kind <= OBJECT_VIEW);
	READ_NODE_FIELD(name);
	READ_NODE_FIELD(args);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
	READ_BOOL_FIELD(missing_ok);
			
	READ_DONE();
}

static AlterFunctionStmt *
_readAlterFunctionStmt(const char ** str)
{
	READ_LOCALS(AlterFunctionStmt);
	READ_NODE_FIELD(func);
	READ_NODE_FIELD(actions);
	
	READ_DONE();
}


static DefineStmt *
_readDefineStmt(const char ** str)
{
	READ_LOCALS(DefineStmt);
	READ_ENUM_FIELD(kind, ObjectType); Assert(local_node->kind <= OBJECT_VIEW);
	READ_BOOL_FIELD(oldstyle);
	READ_NODE_FIELD(defnames);
	READ_NODE_FIELD(args);
	READ_NODE_FIELD(definition);
	READ_OID_FIELD(newOid);
	READ_OID_FIELD(shadowOid);
	
	READ_DONE();
	
}

static CompositeTypeStmt *
_readCompositeTypeStmt(const char ** str)
{
	READ_LOCALS(CompositeTypeStmt);

	READ_NODE_FIELD(typevar);
	READ_NODE_FIELD(coldeflist);
	READ_OID_FIELD(comptypeOid);
	
	READ_DONE();
	
}

static CreateCastStmt *
_readCreateCastStmt(const char ** str)
{
	READ_LOCALS(CreateCastStmt);
	READ_NODE_FIELD(sourcetype);
	READ_NODE_FIELD(targettype);
	READ_NODE_FIELD(func);
	READ_ENUM_FIELD(context, CoercionContext);
	READ_OID_FIELD(castOid);
		
	READ_DONE();
}

static DropCastStmt *
_readDropCastStmt(const char ** str)
{
	READ_LOCALS(DropCastStmt);
	READ_NODE_FIELD(sourcetype);
	READ_NODE_FIELD(targettype);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
	READ_BOOL_FIELD(missing_ok);
			
	READ_DONE();
}

static CreateOpClassStmt *
_readCreateOpClassStmt(const char ** str)
{
	READ_LOCALS(CreateOpClassStmt);
	READ_NODE_FIELD(opclassname);
	READ_STRING_FIELD(amname);
	READ_NODE_FIELD(datatype);
	READ_NODE_FIELD(items);
	READ_BOOL_FIELD(isDefault);
	READ_OID_FIELD(opclassOid);
				
	READ_DONE();
}

static CreateOpClassItem *
_readCreateOpClassItem(const char ** str)
{
	READ_LOCALS(CreateOpClassItem);
	READ_INT_FIELD(itemtype);
	READ_NODE_FIELD(name);
	READ_NODE_FIELD(args);
	READ_INT_FIELD(number);
	READ_BOOL_FIELD(recheck);
	READ_NODE_FIELD(storedtype);
				
	READ_DONE();
}
	
static RemoveOpClassStmt *
_readRemoveOpClassStmt(const char ** str)
{
	READ_LOCALS(RemoveOpClassStmt);
	READ_NODE_FIELD(opclassname);
	READ_STRING_FIELD(amname);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);
	READ_BOOL_FIELD(missing_ok);
				
	READ_DONE();
}

static CreateConversionStmt *
_readCreateConversionStmt(const char ** str)
{
	READ_LOCALS(CreateConversionStmt);
	READ_NODE_FIELD(conversion_name);
	READ_STRING_FIELD(for_encoding_name);
	READ_STRING_FIELD(to_encoding_name);
	READ_NODE_FIELD(func_name);
	READ_BOOL_FIELD(def);
	READ_OID_FIELD(convOid);
					
	READ_DONE();
}

static GrantStmt *
_readGrantStmt(const char ** str)
{
	READ_LOCALS(GrantStmt);
	
	READ_BOOL_FIELD(is_grant);		
	READ_ENUM_FIELD(objtype,GrantObjectType);	
	READ_NODE_FIELD(objects);		
	READ_NODE_FIELD(privileges);		
	READ_NODE_FIELD(grantees);		
	READ_BOOL_FIELD(grant_option);	
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);	
	
	READ_DONE();	
}

static PrivGrantee *
_readPrivGrantee(const char ** str)
{
	READ_LOCALS(PrivGrantee);
	READ_STRING_FIELD(rolname);
		
	READ_DONE();		
}

static FuncWithArgs *
_readFuncWithArgs(const char ** str)
{
	READ_LOCALS(FuncWithArgs);
	READ_NODE_FIELD(funcname);		
	READ_NODE_FIELD(funcargs);	
		
	READ_DONE();		 	
}

static GrantRoleStmt *
_readGrantRoleStmt(const char ** str)
{
	READ_LOCALS(GrantRoleStmt);
	READ_NODE_FIELD(granted_roles);
	READ_NODE_FIELD(grantee_roles);
	READ_BOOL_FIELD(is_grant);
	READ_BOOL_FIELD(admin_opt);
	READ_STRING_FIELD(grantor);
	READ_ENUM_FIELD(behavior, DropBehavior); Assert(local_node->behavior <= DROP_CASCADE);		
	
	READ_DONE();
}

static LockStmt *
_readLockStmt(const char ** str)
{
	READ_LOCALS(LockStmt);
	READ_NODE_FIELD(relations);
	READ_INT_FIELD(mode);
	READ_BOOL_FIELD(nowait);
		
	READ_DONE();
}

static ConstraintsSetStmt *
_readConstraintsSetStmt(const char ** str)
{
	READ_LOCALS(ConstraintsSetStmt);
	READ_NODE_FIELD(constraints);
	READ_BOOL_FIELD(deferred);
	
	READ_DONE();
}


/*
 * _readPlan
 */
static Plan *
_readPlan(const char ** str)
{
	READ_LOCALS(Plan);

	readPlanInfo(str, local_node);

	READ_DONE();
}

/*
 * _readResult
 */
static Result *
_readResult(const char ** str)
{
	READ_LOCALS(Result);

	readPlanInfo(str, (Plan *)local_node);

	READ_NODE_FIELD(resconstantqual);

	READ_DONE();
}

/*
 * _readAppend
 */
static Append *
_readAppend(const char ** str)
{
	READ_LOCALS(Append);

	readPlanInfo(str, (Plan *)local_node);

	READ_NODE_FIELD(appendplans);
	READ_BOOL_FIELD(isTarget);

	READ_DONE();
}

/*
 * _readBitmapAnd, _readBitmapOr
 */
static BitmapAnd *
_readBitmapAnd(const char ** str)
{
	READ_LOCALS(BitmapAnd);

	readPlanInfo(str, (Plan *)local_node);

	READ_NODE_FIELD(bitmapplans);

	READ_DONE();
}

static BitmapOr *
_readBitmapOr(const char ** str)
{
    READ_LOCALS(BitmapOr);

    readPlanInfo(str, (Plan *)local_node);

    READ_NODE_FIELD(bitmapplans);

    READ_DONE();
}


/*
 * _readScan
 */
static Scan *
_readScan(const char ** str)
{
	READ_LOCALS(Scan);

	readScanInfo(str, local_node);

	READ_DONE();
}

/*
 * _readSeqScan
 */
static SeqScan *
_readSeqScan(const char ** str)
{
	READ_LOCALS(SeqScan);

	readScanInfo(str, (Scan *)local_node);

	READ_DONE();
}

/*
 * _readExternalScan
 */
static ExternalScan *
_readExternalScan(const char ** str)
{
	READ_LOCALS(ExternalScan);
	
	readScanInfo(str, (Scan *)local_node);
	
	READ_NODE_FIELD(uriList);
	READ_NODE_FIELD(fmtOpts);
	READ_BOOL_FIELD(isText);
	
	READ_DONE();
}

/*
 * _readIndexScan
 */
static IndexScan *
_readIndexScan(const char ** str)
{
	READ_LOCALS(IndexScan);

	readScanInfo(str, (Scan *)local_node);

	READ_OID_FIELD(indexid);
	READ_NODE_FIELD(indexqual);
	READ_NODE_FIELD(indexqualorig);
	READ_NODE_FIELD(indexstrategy);
	READ_NODE_FIELD(indexsubtype);

	READ_ENUM_FIELD(indexorderdir, ScanDirection);

	READ_DONE();
}


static BitmapIndexScan *
_readBitmapIndexScan(const char ** str)
{
	READ_LOCALS(BitmapIndexScan);

	readScanInfo(str, (Scan *)local_node);

	READ_OID_FIELD(indexid);
	READ_NODE_FIELD(indexqual);
	READ_NODE_FIELD(indexqualorig);
	READ_NODE_FIELD(indexstrategy);
	READ_NODE_FIELD(indexsubtype);
	
	READ_DONE();
}

static BitmapHeapScan *
_readBitmapHeapScan(const char ** str)
{
	READ_LOCALS(BitmapHeapScan);

	readScanInfo(str, (Scan *)local_node);

	READ_NODE_FIELD(bitmapqualorig);
	
	READ_DONE();
}


/*
 * _readTidScan
 */
static TidScan *
_readTidScan(const char ** str)
{
	READ_LOCALS(TidScan);

	readScanInfo(str, (Scan *)local_node);

	READ_NODE_FIELD(tidquals);

	READ_DONE();
}

/*
 * _readSubqueryScan
 */
static SubqueryScan *
_readSubqueryScan(const char ** str)
{
	READ_LOCALS(SubqueryScan);

	readScanInfo(str, (Scan *)local_node);

	READ_NODE_FIELD(subplan);

	READ_DONE();
}

/*
 * _readFunctionScan
 */
static FunctionScan *
_readFunctionScan(const char ** str)
{
	READ_LOCALS(FunctionScan);

	readScanInfo(str, (Scan *)local_node);

	READ_DONE();
}

/*
 * _readValuesScan
 */
static ValuesScan *
_readValuesScan(const char ** str)
{
	READ_LOCALS(ValuesScan);

	readScanInfo(str, (Scan *)local_node);

	READ_DONE();
}

/*
 * _readJoin
 */
static Join *
_readJoin(const char ** str)
{
	READ_LOCALS(Join);

	readJoinInfo(str, (Join *)local_node);

	READ_DONE();
}

/*
 * _readNestLoop
 */
static NestLoop *
_readNestLoop(const char ** str)
{
	READ_LOCALS(NestLoop);

	readJoinInfo(str, (Join *)local_node);

    READ_BOOL_FIELD(outernotreferencedbyinner); /*CDB*/
    READ_BOOL_FIELD(shared_outer);

    READ_DONE();
}

/*
 * _readAdaptiveNestLoop
 */
static AdaptiveNestLoop *
_readAdaptiveNestLoop(const char ** str)
{
	READ_LOCALS(AdaptiveNestLoop);

	readPlanInfo(str, (Plan *)local_node);

	READ_INT_FIELD(nl_strategy_threshold);

    READ_DONE();
}

/*
 * _readMergeJoin
 */
static MergeJoin *
_readMergeJoin(const char ** str)
{
	READ_LOCALS(MergeJoin);

	readJoinInfo(str, (Join *)local_node);

	READ_NODE_FIELD(mergeclauses);

	READ_DONE();
}

/*
 * _readHashJoin
 */
static HashJoin *
_readHashJoin(const char ** str)
{
	READ_LOCALS(HashJoin);

	readJoinInfo(str, (Join *)local_node);

	READ_NODE_FIELD(hashclauses);

	READ_DONE();
}

/*
 * _readAgg
 */
static Agg *
_readAgg(const char ** str)
{
	READ_LOCALS(Agg);

	readPlanInfo(str, (Plan *)local_node);

	READ_ENUM_FIELD(aggstrategy, AggStrategy);
	READ_INT_FIELD(numCols);
	READ_INT_ARRAY(grpColIdx, numCols, AttrNumber);
	READ_LONG_FIELD(numGroups);
	READ_INT_FIELD(transSpace);

	READ_DONE();
}

/*
 * _readGroup
 */
static Group *
_readGroup(const char ** str)
{
	READ_LOCALS(Group);

	readPlanInfo(str, (Plan *)local_node);

	READ_INT_FIELD(numCols);

	READ_INT_ARRAY(grpColIdx, numCols, AttrNumber);
		
	READ_DONE();
}

/*
 * _readMaterial
 */
static Material *
_readMaterial(const char ** str)
{
	READ_LOCALS(Material);

    READ_BOOL_FIELD(cdb_strict);
    readPlanInfo(str, (Plan *)local_node);

	READ_DONE();
}

/*
 * _readSort
 */
static Sort *
_readSort(const char ** str)
{

	READ_LOCALS(Sort);

	readPlanInfo(str, (Plan *)local_node);

	READ_INT_FIELD(numCols);

	READ_INT_ARRAY(sortColIdx, numCols, AttrNumber);

	READ_OID_ARRAY(sortOperators, numCols);

    /* CDB */
	READ_NODE_FIELD(limitOffset);
	READ_NODE_FIELD(limitCount);
	READ_BOOL_FIELD(noduplicates);

	READ_DONE();
}

/*
 * _readUnique
 */
static Unique *
_readUnique(const char ** str)
{
	READ_LOCALS(Unique);

	readPlanInfo(str, (Plan *)local_node);

	READ_INT_FIELD(numCols);

	READ_INT_ARRAY(uniqColIdx, numCols, AttrNumber);

	READ_DONE();
}

/*
 * _readSetOp
 */
static SetOp *
_readSetOp(const char ** str)
{
	READ_LOCALS(SetOp);

	readPlanInfo(str, (Plan *)local_node);

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
_readLimit(const char ** str)
{
	READ_LOCALS(Limit);

	readPlanInfo(str, (Plan *)local_node);

	READ_NODE_FIELD(limitOffset);
	READ_NODE_FIELD(limitCount);

	READ_DONE();
}
	

/*
 * _readHash
 */
static Hash *
_readHash(const char ** str)
{
	READ_LOCALS(Hash);

	readPlanInfo(str, (Plan *)local_node);

	READ_DONE();
}

/*
 * _readMotion
 */
static Motion *
_readMotion(const char ** str)
{
	READ_LOCALS(Motion);

	READ_INT_FIELD(motionID);
	READ_ENUM_FIELD(motionType, MotionType);  Assert(local_node->motionType <= MOTIONTYPE_FIXED);
	READ_BOOL_FIELD(sendToSecondary);
	READ_BOOL_FIELD(sendSorted);
	
    READ_INT_FIELD(numInputSegs);
    if (local_node->numInputSegs > 0)
    {
    	bool array_is_null;
    	array_is_null = ((*str)[0] != 0);
    	 	
    	(*str)++;
    	if (array_is_null)
    	{
    		local_node->inputSegIdx = NULL;
    	}
    	else
    	{
    		READ_INT_ARRAY(inputSegIdx, numInputSegs, int);
    	}
    }

	READ_INT_FIELD(numHashSegs);
	READ_INT_ARRAY(hashSegIdx, numHashSegs, int);

	READ_NODE_FIELD(hashExpr);
	READ_NODE_FIELD(hashDataTypes);

	READ_INT_FIELD(numOutputSegs);
	READ_INT_ARRAY(outputSegIdx, numOutputSegs, int);

	READ_INT_FIELD(numSortCols);
	READ_INT_ARRAY(sortColIdx, numSortCols, AttrNumber);
	READ_OID_ARRAY(sortOperators, numSortCols);

	readPlanInfo(str, (Plan *)local_node);

	READ_DONE();
}


/*
 * _readVacuumStmt
 */
static VacuumStmt *
_readVacuumStmt(const char ** str)
{
	READ_LOCALS(VacuumStmt);

	READ_BOOL_FIELD(vacuum);
	READ_BOOL_FIELD(full);
	READ_BOOL_FIELD(analyze);
	READ_BOOL_FIELD(verbose);
	READ_INT_FIELD(freeze_min_age);
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(va_cols);

	READ_DONE();
}


static CdbProcess *
_readCdbProcess(const char ** str)
{
	READ_LOCALS(CdbProcess);
	
	READ_STRING_FIELD(listenerAddr);
	READ_INT_FIELD(listenerPort);
	READ_INT_FIELD(pid);
	READ_INT_FIELD(contentid);
	READ_INT_FIELD(routeid);
	
	READ_DONE();
}

static Slice *
_readSlice(const char ** str)
{
	READ_LOCALS(Slice);
	
	READ_INT_FIELD(sliceIndex);
	READ_INT_FIELD(rootIndex);
	READ_ENUM_FIELD(gangType, GangType);  Assert(local_node->gangType <= GANGTYPE_MIRROR_WRITER);
	READ_INT_FIELD(gangSize);
	READ_DUMMY_FIELD(primaryGang, NULL);
	READ_DUMMY_FIELD(mirrorGang, NULL);
	READ_INT_FIELD(parentIndex);
	READ_NODE_FIELD(children); /* List of int index */
	READ_NODE_FIELD(primaryProcesses); /* List of (CDBProcess *) */
	READ_NODE_FIELD(mirrorProcesses); /* List of (CDBProcess *) */
	
	READ_DONE();
}

static SliceTable *
_readSliceTable(const char ** str)
{
	READ_LOCALS(SliceTable);
	
	READ_INT_FIELD(nMotions);
	READ_INT_FIELD(nSubplans);
	READ_INT_FIELD(localSlice);
	READ_NODE_FIELD(slices); /* List of Slice* */
	READ_STRING_FIELD(seqServerHost);
	READ_INT_FIELD(seqServerPort);
    READ_BOOL_FIELD(doInstrument);
	
	READ_DONE();
}



static VariableResetStmt *
_readVariableResetStmt(const char ** str)
{
	READ_LOCALS(VariableResetStmt);
	READ_STRING_FIELD(name);
	
	READ_DONE();
}


void readScanInfo(const char ** str, Scan *local_node)
{


	readPlanInfo(str,(Plan *)local_node);

	READ_UINT_FIELD(scanrelid);
}

void readPlanInfo(const char ** str, Plan *local_node)
{


	READ_FLOAT_FIELD(startup_cost);
	READ_FLOAT_FIELD(total_cost);
	READ_FLOAT_FIELD(plan_rows);
	READ_INT_FIELD(plan_width);
	READ_NODE_FIELD(targetlist);
	READ_NODE_FIELD(qual);
	READ_BITMAPSET_FIELD(extParam);
	READ_BITMAPSET_FIELD(allParam);
	READ_INT_FIELD(nParamExec);
	READ_INT_FIELD(nMotionNodes);
	READ_INT_FIELD(nInitPlans);
	READ_NODE_FIELD(sliceTable);

    READ_NODE_FIELD(lefttree);
    READ_NODE_FIELD(righttree);
    READ_NODE_FIELD(initPlan);

    /*
     * Fields that are used only by the qDisp don't need to be serialized for
     * dispatching to qExecs (in outfast.c), and are not read here:
     *      flow
     *      dispatch
     */
}

void readJoinInfo(const char ** str, Join *local_node)
{


	readPlanInfo(str,(Plan *)local_node);

	READ_BOOL_FIELD(prefetch_inner);

	READ_ENUM_FIELD(jointype, JoinType); Assert(local_node->jointype <= JOIN_UNIQUE_INNER);
	READ_NODE_FIELD(joinqual);
}

Bitmapset  *
bitmapsetRead(const char ** str)
{


	Bitmapset *bms = NULL;
	int nwords;
	int i;
	
	memcpy(&nwords, *str, sizeof(int)); (*str)+=sizeof(int);
	if (nwords==0)
		return bms;
		
	bms = palloc(sizeof(int)+nwords*sizeof(bitmapword));
	bms->nwords = nwords;
	for (i = 0; i < nwords; i++)
	{
		memcpy(&bms->words[i], *str, sizeof(bitmapword)); (*str)+=sizeof(bitmapword);
	}
		
	


	return bms;
}

static CreateTrigStmt *
_readCreateTrigStmt(const char ** str)
{
	READ_LOCALS(CreateTrigStmt);
	
	READ_STRING_FIELD(trigname);
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(funcname);
	READ_NODE_FIELD(args);
	READ_BOOL_FIELD(before);
	READ_BOOL_FIELD(row);
	{ int slen; 
		memcpy(&slen, *str, sizeof(int)); 
		(*str)+=sizeof(int); 
		memcpy(local_node->actions,*str,slen); 
		(*str)+=slen; }
	READ_BOOL_FIELD(isconstraint);
	READ_BOOL_FIELD(deferrable);
	READ_BOOL_FIELD(initdeferred);
	READ_NODE_FIELD(constrrel);
	READ_OID_FIELD(trigOid);
	
	READ_DONE();

}

static
Node * _readValue(const char ** str, NodeTag nt)
{
	Node * result = NULL;
	if (nt == T_Integer)
	{
		int ival;
		memcpy(&ival, *str, sizeof(long)); (*str)+=sizeof(long);
		result = (Node *) makeInteger(ival);
	}
	else
	{
		int slen; 
		char * nn = NULL; 
		memcpy(&slen, *str, sizeof(int)); 
		(*str)+=sizeof(int);  
		if (slen>0) 
		{  
		    nn = palloc(slen+1);  
		    memcpy(nn,*str,slen);  
		    (*str)+=(slen); nn[slen]='\0';  
		}  
 
		if (nt == T_Float)
			result = (Node *) makeFloat(nn);
		else if (nt == T_String)
			result = (Node *) makeString(nn);
		else if (nt == T_BitString)
			result = (Node *) makeBitString(nn);
		else
			elog(ERROR,"Souldn't get here");
	}
	
	return result;

}

void *
readNodeBinary(const char ** str)
{
	void	   *return_value;
	NodeTag 	nt;
	int16       ntt;

	if (str==NULL || *str==NULL)
		return NULL;
		
	memcpy(&ntt,*str,sizeof(int16));
	(*str)+=sizeof(int16);
	nt = (NodeTag) ntt;
	
	if (nt==0)
		return NULL;
		
	if (nt == T_List || nt == T_IntList || nt == T_OidList)
	{
		List	   *l = NIL;
		int listsize = 0;
		int i;
		
		memcpy(&listsize,*str,sizeof(int));
		(*str)+=sizeof(int);
		
		if (nt == T_IntList)
		{
			int val;
			for (i = 0; i < listsize; i++)
			{
				memcpy(&val,*str,sizeof(int));
				(*str)+=sizeof(int);
				l = lappend_int(l, val);
			}
		}
		else if (nt == T_OidList)
		{
			Oid val;
			for (i = 0; i < listsize; i++)
			{
				memcpy(&val,*str,sizeof(Oid));
				(*str)+=sizeof(Oid);
				l = lappend_oid(l, val);
			}
		}
		else 
		{
			
			for (i = 0; i < listsize; i++)
			{
				l = lappend(l, readNodeBinary(str));
			}
		}
		Assert(l->length==listsize);

		return l;
	}
	
	if (nt == T_Integer || nt == T_Float || nt == T_String || nt == T_BitString)
	{
		return _readValue(str, nt);
	}
	
	switch(nt)
	{
		case T_Plan:
				return_value = _readPlan(str);
				break;
			case T_Result:
				return_value = _readResult(str);
				break;
			case T_Append:
				return_value = _readAppend(str);
				break;
			case T_BitmapAnd:
				return_value = _readBitmapAnd(str);
				break;
			case T_BitmapOr:
				return_value = _readBitmapOr(str);
				break;
			case T_Scan:
				return_value = _readScan(str);
				break;
			case T_SeqScan:
				return_value = _readSeqScan(str);
				break;
			case T_ExternalScan:
				return_value = _readExternalScan(str);
				break;
			case T_IndexScan:
				return_value = _readIndexScan(str);
				break;
			case T_BitmapIndexScan:
				return_value = _readBitmapIndexScan(str);
				break;
			case T_BitmapHeapScan:
				return_value = _readBitmapHeapScan(str);
				break;
			case T_TidScan:
				return_value = _readTidScan(str);
				break;
			case T_SubqueryScan:
				return_value = _readSubqueryScan(str);
				break;
			case T_FunctionScan:
				return_value = _readFunctionScan(str);
				break;
			case T_ValuesScan:
				return_value = _readValuesScan(str);
				break;
			case T_Join:
				return_value = _readJoin(str);
				break;
			case T_NestLoop:
				return_value = _readNestLoop(str);
				break;
			case T_AdaptiveNestLoop:
				return_value = _readAdaptiveNestLoop(str);
				break;
			case T_MergeJoin:
				return_value = _readMergeJoin(str);
				break;
			case T_HashJoin:
				return_value = _readHashJoin(str);
				break;
			case T_Agg:
				return_value = _readAgg(str);
				break;
			case T_Group:
				return_value = _readGroup(str);
				break;
			case T_Material:
				return_value = _readMaterial(str);
				break;
			case T_Sort:
				return_value = _readSort(str);
				break;
			case T_Unique:
				return_value = _readUnique(str);
				break;
			case T_SetOp:
				return_value = _readSetOp(str);
				break;
			case T_Limit:
				return_value = _readLimit(str);
				break;
			case T_Hash:
				return_value = _readHash(str);
				break;
			case T_Motion:
				return_value = _readMotion(str);
				break;
			case T_Alias:
				return_value = _readAlias(str);
				break;
			case T_RangeVar:
				return_value = _readRangeVar(str);
				break;
			case T_Var:
				return_value = _readVar(str);
				break;
			case T_Const:
				return_value = _readConst(str);
				break;
			case T_Param:
				return_value = _readParam(str);
				break;
			case T_Aggref:
				return_value = _readAggref(str);
				break;
			case T_ArrayRef:
				return_value = _readArrayRef(str);
				break;
			case T_FuncExpr:
				return_value = _readFuncExpr(str);
				break;
			case T_OpExpr:
				return_value = _readOpExpr(str);
				break;
			case T_DistinctExpr:
				return_value = _readDistinctExpr(str);
				break;
			case T_ScalarArrayOpExpr:
				return_value = _readScalarArrayOpExpr(str);
				break;
			case T_BoolExpr:
				return_value = _readBoolExpr(str);
				break;
			case T_SubLink:
				return_value = _readSubLink(str);
				break;
			case T_SubPlan:
				return_value = _readSubPlan(str);
				break;
			case T_FieldSelect:
				return_value = _readFieldSelect(str);
				break;
			case T_FieldStore:
				return_value = _readFieldStore(str);
				break;
			case T_RelabelType:
				return_value = _readRelabelType(str);
				break;
			case T_ConvertRowtypeExpr:
				return_value = _readConvertRowtypeExpr(str);
				break;
			case T_CaseExpr:
				return_value = _readCaseExpr(str);
				break;
			case T_CaseWhen:
				return_value = _readCaseWhen(str);
				break;
			case T_CaseTestExpr:
				return_value = _readCaseTestExpr(str);
				break;
			case T_ArrayExpr:
				return_value = _readArrayExpr(str);
				break;
			case T_RowExpr:
				return_value = _readRowExpr(str);
				break;
			case T_RowCompareExpr:
				return_value = _readRowCompareExpr(str);
				break;
			case T_CoalesceExpr:
				return_value = _readCoalesceExpr(str);
				break;
			case T_MinMaxExpr:
				return_value = _readMinMaxExpr(str);
				break;
			case T_NullIfExpr:
				return_value = _readNullIfExpr(str);
				break;
			case T_NullTest:
				return_value = _readNullTest(str);
				break;
			case T_BooleanTest:
				return_value = _readBooleanTest(str);
				break;
			case T_CoerceToDomain:
				return_value = _readCoerceToDomain(str);
				break;
			case T_CoerceToDomainValue:
				return_value = _readCoerceToDomainValue(str);
				break;
			case T_SetToDefault:
				return_value = _readSetToDefault(str);
				break;
			case T_TargetEntry:
				return_value = _readTargetEntry(str);
				break;
			case T_RangeTblRef:
				return_value = _readRangeTblRef(str);
				break;
			case T_JoinExpr:
				return_value = _readJoinExpr(str);
				break;
			case T_FromExpr:
				return_value = _readFromExpr(str);
				break;
#if 0
			case T_Flow:
				return_value = _readFlow(str);
				break;

			case T_Path:
				return_value = _readPath(str);
				break;
			case T_IndexPath:
				return_value = _readIndexPath(str);
				break;
			case T_BitmapHeapPath:
				return_value = _readBitmapHeapPath(str);
				break;
			case T_BitmapAndPath:
				return_value = _readBitmapAndPath(str);
				break;
			case T_BitmapOrPath:
				return_value = _readBitmapOrPath(str);
				break;
			case T_TidPath:
				return_value = _readTidPath(str);
				break;
			case T_AppendPath:
				return_value = _readAppendPath(str);
				break;
			case T_ResultPath:
				return_value = _readResultPath(str);
				break;
			case T_MaterialPath:
				return_value = _readMaterialPath(str);
				break;
			case T_UniquePath:
				return_value = _readUniquePath(str);
				break;
			case T_NestPath:
				return_value = _readNestPath(str);
				break;
			case T_MergePath:
				return_value = _readMergePath(str);
				break;
			case T_HashPath:
				return_value = _readHashPath(str);
				break;
            case T_CdbMotionPath:
                return_value = _readCdbMotionPath(str);
                break;
			case T_PlannerInfo:
				return_value = _readPlannerInfo(str);
				break;
			case T_RelOptInfo:
				return_value = _readRelOptInfo(str);
				break;
			case T_IndexOptInfo:
				return_value = _readIndexOptInfo(str);
				break;
			case T_CdbRelDedupInfo:
				return_value = _readCdbRelDedupInfo(str);
				break;
			case T_PathKeyItem:
				return_value = _readPathKeyItem(str);
				break;
			case T_RestrictInfo:
				return_value = _readRestrictInfo(str);
				break;
			case T_InnerIndexscanInfo:
				return_value = _readInnerIndexscanInfo(str);
				break;
			case T_OuterJoinInfo:
				return_value = _readOuterJoinInfo(str);
				break;
			case T_InClauseInfo:
				return_value = _readInClauseInfo(str);
				break;
			case T_AppendRelInfo:
				return_value = _readAppendRelInfo(str);
				break;
#endif 			
				
			case T_GrantStmt:
				return_value = _readGrantStmt(str);
				break;
			case T_PrivGrantee:
				return_value = _readPrivGrantee(str);
				break;
			case T_FuncWithArgs:
				return_value = _readFuncWithArgs(str);
				break;
			case T_GrantRoleStmt:
				return_value = _readGrantRoleStmt(str);
				break;				
			case T_LockStmt:
				return_value = _readLockStmt(str);
				break;

			case T_CreateStmt:
				return_value = _readCreateStmt(str);
				break;
			case T_IndexStmt:
				return_value = _readIndexStmt(str);
				break;
			case T_ReindexStmt:
				return_value = _readReindexStmt(str);
				break;
				
			case T_ConstraintsSetStmt:
				return_value = _readConstraintsSetStmt(str);
				break;
				
			case T_CreateFunctionStmt:
				return_value = _readCreateFunctionStmt(str);
				break;
			case T_FunctionParameter:
				return_value = _readFunctionParameter(str);
				break;
			case T_RemoveFuncStmt:
				return_value = _readRemoveFuncStmt(str);
				break;
			case T_AlterFunctionStmt:
				return_value = _readAlterFunctionStmt(str);
				break;

			case T_DefineStmt:
				return_value = _readDefineStmt(str);
				break;
				
			case T_CompositeTypeStmt:
				return_value = _readCompositeTypeStmt(str);
				break;
			case T_CreateCastStmt:
				return_value = _readCreateCastStmt(str);
				break;
			case T_DropCastStmt:
				return_value = _readDropCastStmt(str);
				break;
			case T_CreateOpClassStmt:
				return_value = _readCreateOpClassStmt(str);
				break;
			case T_CreateOpClassItem:
				return_value = _readCreateOpClassItem(str);
				break;
			case T_RemoveOpClassStmt:
				return_value = _readRemoveOpClassStmt(str);
				break;
			case T_CreateConversionStmt:
				return_value = _readCreateConversionStmt(str);
				break;
			
				
			case T_ViewStmt:
				return_value = _readViewStmt(str);
				break;
			case T_RuleStmt:
				return_value = _readRuleStmt(str);
				break;
			case T_DropStmt:
				return_value = _readDropStmt(str);
				break;
			case T_DropPropertyStmt:
				return_value = _readDropPropertyStmt(str);
				break;
			case T_TruncateStmt:
				return_value = _readTruncateStmt(str);
				break;

			case T_AlterTableStmt:
				return_value = _readAlterTableStmt(str);
				break;
			case T_AlterTableCmd:
				return_value = _readAlterTableCmd(str);
				break;

			case T_CreateRoleStmt:
				return_value = _readCreateRoleStmt(str);
				break;
			case T_DropRoleStmt:
				return_value = _readDropRoleStmt(str);
				break;
			case T_AlterRoleStmt:
				return_value = _readAlterRoleStmt(str);
				break;
			case T_AlterRoleSetStmt:
				return_value = _readAlterRoleSetStmt(str);
				break;

			case T_AlterObjectSchemaStmt:
				return_value = _readAlterObjectSchemaStmt(str);
				break;
				
			case T_AlterOwnerStmt:
				return_value = _readAlterOwnerStmt(str);
				break;
				
			case T_RenameStmt:
				return_value = _readRenameStmt(str);
				break;
				
			case T_CreateSeqStmt:
				return_value = _readCreateSeqStmt(str);
				break;
			case T_AlterSeqStmt:
				return_value = _readAlterSeqStmt(str);
				break;
			case T_ClusterStmt:
				return_value = _readClusterStmt(str);
				break;
			case T_CreatedbStmt:
				return_value = _readCreatedbStmt(str);
				break;
			case T_DropdbStmt:
				return_value = _readDropdbStmt(str);
				break;
			case T_CreateDomainStmt:
				return_value = _readCreateDomainStmt(str);
				break;
			case T_AlterDomainStmt:
				return_value = _readAlterDomainStmt(str);
				break;

#if 0
			case T_TransactionStmt:
				return_value = _readTransactionStmt(str);
				break;
#endif 
			case T_NotifyStmt:
				return_value = _readNotifyStmt(str);
				break;
			case T_DeclareCursorStmt:
				return_value = _readDeclareCursorStmt(str);
				break;
#if 0
			case T_CopyStmt:
				return_value = _readCopyStmt(str);
				break;
			case T_SelectStmt:
				return_value = _readSelectStmt(str);
				break;
#endif 
			case T_ColumnDef:
				return_value = _readColumnDef(str);
				break;
			case T_TypeName:
				return_value = _readTypeName(str);
				break;
			case T_TypeCast:
				return_value = _readTypeCast(str);
				break;
			case T_IndexElem:
				return_value = _readIndexElem(str);
				break;
			case T_Query:
				return_value = _readQuery(str);
				break;
			case T_SortClause:
				return_value = _readSortClause(str);
				break;
			case T_GroupClause:
				return_value = _readGroupClause(str);
				break;
			case T_RowMarkClause:
				return_value = _readRowMarkClause(str);
				break;
			case T_SetOperationStmt:
				return_value = _readSetOperationStmt(str);
				break;
			case T_RangeTblEntry:
				return_value = _readRangeTblEntry(str);
				break;
			case T_A_Expr:
				return_value = _readAExpr(str);
				break;
			case T_ColumnRef:
				return_value = _readColumnRef(str);
				break;
#if 0
			case T_ParamRef:
				return_value = _readParamRef(str);
				break;
#endif 
			case T_A_Const:
				return_value = _readAConst(str);
				break;
#if 0 
			case T_A_Indices:
				return_value = _readA_Indices(str);
				break;
			case T_A_Indirection:
				return_value = _readA_Indirection(str);
				break;
			case T_ResTarget:
				return_value = _readResTarget(str);
				break;
#endif 
			case T_Constraint:
				return_value = _readConstraint(str);
				break;
			case T_FkConstraint:
				return_value = _readFkConstraint(str);
				break; 
			case T_FuncCall:
				return_value = _readFuncCall(str);
				break;
			case T_DefElem:
				return_value = _readDefElem(str);
				break;
			case T_CreateSchemaStmt:
				return_value = _readCreateSchemaStmt(str);
				break;
			case T_CreatePLangStmt:
				return_value = _readCreatePLangStmt(str);
				break;
			case T_DropPLangStmt:
				return_value = _readDropPLangStmt(str);
				break;
			case T_VacuumStmt:
				return_value = _readVacuumStmt(str);
				break;			
			case T_CdbProcess:
				return_value = _readCdbProcess(str);
				break;
			case T_Slice:
				return_value = _readSlice(str);
				break;
			case T_SliceTable:
				return_value = _readSliceTable(str);
				break;
			case T_VariableResetStmt:
				return_value = _readVariableResetStmt(str);
				break;
#if 0
			case T_LockingClause:
				return_value = _readLockingClause(str);
				break;
#endif
				
			case T_CreateTrigStmt:
				return_value = _readCreateTrigStmt(str);
				break;

			default:
				return_value = NULL; /* keep the compiler silent */
				elog(ERROR, "could not deserialize unrecognized node type: %d",
						 (int) nt);
				
				break;
	}

	return (Node *)return_value;
}

Node *
readNodeFromBinaryString(const char * str, int len)
{
	Node * node;
	int16 tg = 0;
	
	node = readNodeBinary(&str);
	
	memcpy(&tg, str, sizeof(int16)); 
	if (tg != (int16)0xDEAD)
		elog(ERROR,"Deserialization lost sync.");
	
	return node;
	
}
/*
 * readDatum
 *
 * Given a binary string representation of a constant, recreate the appropriate
 * Datum.  The string representation embeds length info, but not byValue,
 * so we must be told that.
 */
static Datum
readDatum(const char ** str, bool typbyval)
{
	Size		length;

	Datum		res;
	char	   *s;

	if (typbyval)
	{
		memcpy(&res, *str, sizeof(Datum)); (*str)+=sizeof(Datum);
	}
	else
	{
		memcpy(&length, *str, sizeof(Size)); (*str)+=sizeof(Size);
	  	if (length <= 0)
			res = (Datum) NULL;
		else
		{
			s = (char *) palloc(length+1);
			memcpy(s, *str, length); (*str)+=length;
			s[length]='\0';
			res = PointerGetDatum(s);
		}

	}

	return res;
}
