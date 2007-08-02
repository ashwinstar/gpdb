/*-------------------------------------------------------------------------
 *
 * readfuncs.c
 *	  Reader functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/readfuncs.c,v 1.195 2006/08/12 02:52:04 tgl Exp $
 *
 * NOTES
 *	  Path and Plan nodes do not need to have any readfuncs support, because we
 *	  never have occasion to read them in.	 We never read executor state trees, either.
 *    
 *    But due to the use of this routine in older version of CDB/MPP/GPDB, 
 *    there are routines that do read those types of nodes (unlike PostgreSQL)
 *    Those routines never actually get called.
 * 
 *    We could go back and remove them, but they don't hurt anything.
 * 
 *    The purpose of these routines is to read serialized trees that were stored 
 *    in the catalog, and reconstruct the trees.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "nodes/parsenodes.h"

#include "nodes/execnodes.h" /* Just for Slice and SliceTable */
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

/* Read an uint64 field (anything written as ":fldname %ll") */
#ifndef WIN32
#define READ_UINT64_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = atoll(token)
#else
#define READ_UINT64_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = _atoi64(token)
#endif

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

/* Read a bitmapset field */
#define READ_BITMAPSET_FIELD(fldname) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	 local_node->fldname = bitmapsetRead()

/* Read a dummy field */
#define READ_DUMMY_FIELD(fldname,fldvalue) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	token = pg_strtok(&length);		/* get field value */ \
	local_node->fldname = (fldvalue)

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
			local_node->fldname[i] = (Type) atoi(token); \
		} \
	}

/* Read optional integer array (":fldname %d %d ..." or ":fldname <>") */
#define READ_INT_ARRAY_OR_NULL(fldname, count, Type) \
	if ( local_node->count > 0 && \
         NULL != (token = pg_strtok(&length)) &&    /* skip :fldname */ \
         NULL != (token = pg_strtok(&length)) &&    /* first value or <> */ \
         length > 0 )                               /* proceed if isn't <> */ \
	{ \
		int i; \
		local_node->fldname = (Type *)palloc(local_node->count * sizeof(Type)); \
        local_node->fldname[0] = (Type) atoi(token); \
		for(i=1; i<local_node->count; i++) \
		{ \
            token = pg_strtok(&length);		/* get field value */ \
            local_node->fldname[i] = (Type) atoi(token); \
		} \
	}

/* Read an unsigned integer array (anything written as ":fldname %u %u ...") */
#define READ_UINT_ARRAY(fldname, count, Type) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	if ( local_node->count > 0 ) \
	{ \
		int i; \
		local_node->fldname = (Type *)palloc(local_node->count * sizeof(Type)); \
		for(i=0; i<local_node->count; i++) \
		{ \
			token = pg_strtok(&length);		/* get field value */ \
			local_node->fldname[i] = (Type) atoui(token); \
		} \
	}

/* Read optional unsigned integer array (":fldname %u %u ..." or ":fldname <>") */
#define READ_UINT_ARRAY_OR_NULL(fldname, count, Type) \
	if ( local_node->count > 0 && \
         NULL != (token = pg_strtok(&length)) &&    /* skip :fldname */ \
         NULL != (token = pg_strtok(&length)) &&    /* first value or <> */ \
         length > 0 )                               /* proceed if isn't <> */ \
	{ \
		int i; \
		local_node->fldname = (Type *)palloc(local_node->count * sizeof(Type)); \
        local_node->fldname[0] = (Type) atoui(token); \
		for(i=1; i<local_node->count; i++) \
		{ \
            token = pg_strtok(&length);		/* get field value */ \
            local_node->fldname[i] = (Type) atoui(token); \
		} \
	}


/* Read an Trasnaction ID array (written as ":fldname %u %u ... ") */
#define READ_XID_ARRAY(fldname, count) \
	token = pg_strtok(&length);		/* skip :fldname */ \
	if ( local_node->count > 0 ) \
	{ \
		int i; \
		local_node->fldname = (TransactionId *)palloc(local_node->count * sizeof(TransactionId)); \
		for(i=0; i<local_node->count; i++) \
		{ \
			token = pg_strtok(&length);		/* get field value */ \
			local_node->fldname[i] = ((TransactionId) strtoul((token), NULL, 10)); \
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
	READ_NODE_FIELD(intoOptions);
	READ_ENUM_FIELD(intoOnCommit, OnCommitAction);
	READ_STRING_FIELD(intoTableSpaceName);
	READ_BOOL_FIELD(hasAggs);
	READ_BOOL_FIELD(hasWindFuncs);
	READ_BOOL_FIELD(hasSubLinks);
	READ_NODE_FIELD(rtable);
	READ_NODE_FIELD(jointree);
	READ_NODE_FIELD(targetList);
	READ_NODE_FIELD(returningList);
	READ_NODE_FIELD(groupClause);
	READ_NODE_FIELD(havingQual);
	READ_NODE_FIELD(windowClause);
	READ_NODE_FIELD(distinctClause);
	READ_NODE_FIELD(sortClause);
	READ_NODE_FIELD(limitOffset);
	READ_NODE_FIELD(limitCount);
	READ_NODE_FIELD(rowMarks);
	READ_NODE_FIELD(setOperations);
	READ_NODE_FIELD(resultRelations);
	READ_NODE_FIELD(returningLists);
	
	/*
	 * systems that we initdb's using prior versions don't have these field, so to avoid
	 * making the developers do an initdb, we will check.
	 * 
	 * This shouldn't matter for anyone who does an initDB in the future (with our production 3.0 release).
     */
    if (pg_strtok_peek_fldname("intoOidInfo.relOid"))
    {                            
        READ_OID_FIELD(intoOidInfo.relOid);
     
        READ_OID_FIELD(intoOidInfo.comptypeOid);
                             
        READ_OID_FIELD(intoOidInfo.toastOid);
                             
        READ_OID_FIELD(intoOidInfo.toastIndexOid);
                            
        READ_OID_FIELD(intoOidInfo.toastComptypeOid);
 
    }
	local_node->intoPolicy = NULL;
	
	/*
	 * If the Query was a CTAS, and the CTAS was stored in the catalog as part of a rule, we
	 * don't want to remember the OIDs assigned in the past.
	 * Not sure we can ever have that happen.
	 */
	Assert(local_node->intoOidInfo.relOid == InvalidOid);
	
	local_node->intoOidInfo.relOid = InvalidOid;
	local_node->intoOidInfo.comptypeOid = InvalidOid;
	local_node->intoOidInfo.toastOid = InvalidOid;
	local_node->intoOidInfo.toastIndexOid = InvalidOid;
	local_node->intoOidInfo.toastComptypeOid = InvalidOid;


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

static GroupingClause *
_readGroupingClause(void)
{
	READ_LOCALS(GroupingClause);

	READ_ENUM_FIELD(groupType, GroupingType);
	READ_NODE_FIELD(groupsets);

	READ_DONE();
}

static GroupingFunc *
_readGroupingFunc(void)
{
	READ_LOCALS(GroupingFunc);

	READ_NODE_FIELD(args);
	READ_INT_FIELD(ngrpcols);

	READ_DONE();
}

static Grouping *
_readGrouping(void)
{
	READ_LOCALS_NO_FIELDS(Grouping);

	READ_DONE();
}

static GroupId *
_readGroupId(void)
{
	READ_LOCALS_NO_FIELDS(GroupId);

	READ_DONE();
}

static WindowSpecParse *
_readWindowSpecParse(const char ** str)
{
	READ_LOCALS(WindowSpecParse);

	READ_STRING_FIELD(name);
	READ_NODE_FIELD(elems);

	READ_DONE();
}

static WindowSpec *
_readWindowSpec(const char ** str)
{
	READ_LOCALS(WindowSpec);

	READ_STRING_FIELD(name);
	READ_STRING_FIELD(parent);
	READ_NODE_FIELD(partition);
	READ_NODE_FIELD(order);
	READ_NODE_FIELD(frame);

	READ_DONE();
}

static WindowFrame *
_readWindowFrame(const char ** str)
{
	READ_LOCALS(WindowFrame);

	READ_BOOL_FIELD(is_rows);
	READ_BOOL_FIELD(is_between);
	READ_NODE_FIELD(trail);
	READ_NODE_FIELD(lead);
	READ_ENUM_FIELD(exclude, WindowExclusion);

	READ_DONE();
}

static WindowFrameEdge *
_readWindowFrameEdge(const char ** str)
{
	READ_LOCALS(WindowFrameEdge);

	READ_ENUM_FIELD(kind, WindowBoundingKind);
	READ_NODE_FIELD(val);

	READ_DONE();
}

/*
 * _readRowMarkClause
 */
static RowMarkClause *
_readRowMarkClause(void)
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
_readSetOperationStmt(void)
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
		READ_NODE_FIELD(keys);
		READ_NODE_FIELD(options);
		READ_STRING_FIELD(indexspace);
	}
	else if (strncmp(token, "UNIQUE", length)==0)
	{
		local_node->contype = CONSTR_UNIQUE;
		READ_NODE_FIELD(keys);
		READ_NODE_FIELD(options);
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
_readIndexElem(void)
{
	READ_LOCALS(IndexElem);

	READ_STRING_FIELD(name);
	READ_NODE_FIELD(expr);
	READ_NODE_FIELD(opclass);


	READ_DONE();
}

static ReindexStmt *
_readReindexStmt(void)
{
	READ_LOCALS(ReindexStmt);
	
	READ_ENUM_FIELD(kind,ObjectType);
	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(name);
	READ_BOOL_FIELD(do_system);
	READ_BOOL_FIELD(do_user);
	
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
	READ_OID_FIELD(relOid);
	READ_OID_FIELD(comptypeOid);

	READ_DONE();
}

static RuleStmt *
_readRuleStmt(void)
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
_readDropStmt(void)
{
	READ_LOCALS(DropStmt);

	READ_NODE_FIELD(objects);
	READ_ENUM_FIELD(removeType,ObjectType);
	READ_ENUM_FIELD(behavior,DropBehavior);
	READ_BOOL_FIELD(missing_ok);
	local_node->missing_ok=true;

	READ_DONE();
}

static DropPropertyStmt *
_readDropPropertyStmt(void)
{
	READ_LOCALS(DropPropertyStmt);

	READ_NODE_FIELD(relation);
	READ_STRING_FIELD(property);
	READ_ENUM_FIELD(removeType,ObjectType);
	READ_ENUM_FIELD(behavior,DropBehavior);
	READ_BOOL_FIELD(missing_ok);

	READ_DONE();
}

static TruncateStmt *
_readTruncateStmt(void)
{
	READ_LOCALS(TruncateStmt);

	READ_NODE_FIELD(relations);
	READ_ENUM_FIELD(behavior,DropBehavior);

	READ_DONE();
}

static AlterTableStmt *
_readAlterTableStmt(void)
{
	int m;
	READ_LOCALS(AlterTableStmt);

	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(cmds);
	READ_ENUM_FIELD(relkind, ObjectType);
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
_readAlterTableCmd(void)
{
	READ_LOCALS(AlterTableCmd);

	READ_ENUM_FIELD(subtype, AlterTableType);
	READ_STRING_FIELD(name);
	READ_NODE_FIELD(def);
	READ_NODE_FIELD(transform);
	READ_ENUM_FIELD(behavior, DropBehavior);
	
	READ_DONE();
}


static CreateRoleStmt *
_readCreateRoleStmt(void)
{
	READ_LOCALS(CreateRoleStmt);
	
	READ_ENUM_FIELD(stmt_type, RoleStmtType);
	READ_STRING_FIELD(role);
	READ_NODE_FIELD(options);
	READ_OID_FIELD(roleOid);

	READ_DONE();
}

static DropRoleStmt *
_readDropRoleStmt(void)
{
	READ_LOCALS(DropRoleStmt);

	READ_NODE_FIELD(roles);
	READ_BOOL_FIELD(missing_ok);

	READ_DONE();
	
}

static  AlterRoleStmt *
_readAlterRoleStmt(void)
{
	READ_LOCALS(AlterRoleStmt);

	READ_STRING_FIELD(role);
	READ_NODE_FIELD(options);
	READ_INT_FIELD(action);
	READ_DONE();
	
}

static  AlterRoleSetStmt *
_readAlterRoleSetStmt(void)
{
	READ_LOCALS(AlterRoleSetStmt);

	READ_STRING_FIELD(role);
	READ_STRING_FIELD(variable);
	READ_NODE_FIELD(value);
	
	READ_DONE();
	
}

static AlterObjectSchemaStmt *
_readAlterObjectSchemaStmt(void)
{
	READ_LOCALS(AlterObjectSchemaStmt);
	
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(object);
	READ_NODE_FIELD(objarg);
	READ_STRING_FIELD(addname);
	READ_STRING_FIELD(newschema);
	READ_ENUM_FIELD(objectType,ObjectType);
	
	READ_DONE();
}



static AlterOwnerStmt *
_readAlterOwnerStmt(void)
{
	READ_LOCALS(AlterOwnerStmt);

	READ_ENUM_FIELD(objectType,ObjectType);
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(object);
	READ_NODE_FIELD(objarg);
	READ_STRING_FIELD(addname);
	READ_STRING_FIELD(newowner);

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
	READ_NODE_FIELD(over);
	READ_INT_FIELD(location);
	READ_DONE();
}

static DefElem *
_readDefElem(void)
{
	READ_LOCALS(DefElem);

	READ_STRING_FIELD(defname);
	READ_NODE_FIELD(arg);
	READ_DONE();
}

static A_Const *
_readAConst(void)
{
	READ_LOCALS(A_Const);

	token = pg_strtok(&length);	
	token = debackslash(token,length);
	local_node->val.type = T_String;
	
	if (token[0] == '"')
	{
		local_node->val.val.str = palloc(length - 1);
		strncpy(local_node->val.val.str , token+1, strlen(token)-2);
		local_node->val.val.str[strlen(token)-2] = '\0';
	}
	else if (length > 2 && (token[0] == 'b'|| token[0] == 'B') && (token[1] == '\'' || token[1] == '"'))
	{
		local_node->val.type = T_BitString;
		local_node->val.val.str = palloc(length+1);
		strncpy(local_node->val.val.str , token, length);
		local_node->val.val.str[length] = '\0';
	}
	else
	{
		bool isInt = true;
		bool isFloat = true;
		int i = 0;
		if (token[i] == ' ')
			i++;
		if (token[i] == '-' || token[i] == '+')
			i++;
		for (; i < length; i++)
	 	   if (token[i] < '0' || token[i] > '9')
	 	   {
	 	   	 isInt = false;
	 	   	 if (token[i] != '.' && token[i] != 'e' && token[i] != 'E' && token[i] != '+' && token[i] != '-')
	 	   	 	isFloat = false;
	 	   }
	 	if (isInt)
		{
			local_node->val.type = T_Integer;
			local_node->val.val.ival = atol(token);
		}
		else if (isFloat)
		{
			local_node->val.type = T_Float;
			local_node->val.val.str = palloc(length + 1);
			strcpy(local_node->val.val.str , token);
		}
		else
		{
			elog(ERROR,"Deserialization problem:  A_Const not string, bitstring, float, or int");
			local_node->val.val.str = palloc(length + 1);
			strcpy(local_node->val.val.str , token);
		}
	}
	
	local_node->typname = NULL;
	READ_NODE_FIELD(typname);
	READ_DONE();
}


static A_Expr *
_readAExpr(void)
{
	READ_LOCALS(A_Expr);
	
	token = pg_strtok(&length);	
	
	if (strncmp(token,"OPER",length)==0)
	{
		local_node->kind = AEXPR_OP;
		READ_NODE_FIELD(name);
	}
	else if (strncmp(token,"AND",length)==0)
	{
		local_node->kind = AEXPR_AND;
	}
	else if (strncmp(token,"OR",length)==0)
	{
		local_node->kind = AEXPR_OR;
	}
	else if (strncmp(token,"NOT",length)==0)
	{
		local_node->kind = AEXPR_NOT;
	}
	else if (strncmp(token,"ANY",length)==0)
	{
		local_node->kind = AEXPR_OP_ANY;
		READ_NODE_FIELD(name);
	}
	else if (strncmp(token,"ALL",length)==0)
	{
		local_node->kind = AEXPR_OP_ALL;
		READ_NODE_FIELD(name);
	}
	else if (strncmp(token,"DISTINCT",length)==0)
	{
		local_node->kind = AEXPR_DISTINCT;
		READ_NODE_FIELD(name);
	}
	else if (strncmp(token,"NULLIF",length)==0)
	{
		local_node->kind = AEXPR_NULLIF;
		READ_NODE_FIELD(name);
	}
	else if (strncmp(token,"OF",length)==0)
	{
		local_node->kind = AEXPR_OF;
		READ_NODE_FIELD(name);
	}
	else if (strncmp(token,"IN",length)==0)
	{
		local_node->kind = AEXPR_IN;
		READ_NODE_FIELD(name);
	}
	else
	{
		elog(ERROR,"Unable to understand A_Expr node %.30s",token);
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
_readParam(void)
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
_readAggref(void)
{
	READ_LOCALS(Aggref);

	READ_OID_FIELD(aggfnoid);
	READ_OID_FIELD(aggtype);
	READ_NODE_FIELD(args);
	READ_UINT_FIELD(agglevelsup);
	READ_BOOL_FIELD(aggstar);
	READ_BOOL_FIELD(aggdistinct);

    /*
     * CDB: This field was added after the MPP 2.1p2 release.  It's filled in
     * by the planner and not present in nodes stored persistently.  So if
     * it's missing, just let the field stay 0.
     */
    if (pg_strtok_peek_fldname("aggstage"))
    {                           /* braces required, macro doesn't have 'em */
        READ_ENUM_FIELD(aggstage, AggStage);
    }

	READ_DONE();
}

/*
 * _readWindowRef
 */
static WindowRef *
_readWindowRef(void)
{
	READ_LOCALS(WindowRef);

	READ_OID_FIELD(winfnoid);
	READ_OID_FIELD(restype);
	READ_NODE_FIELD(args);
	READ_UINT_FIELD(winlevelsup);
	READ_UINT_FIELD(winspec);
	READ_UINT_FIELD(winindex);
    READ_ENUM_FIELD(winstage, WinStage);
	READ_UINT_FIELD(winlevel);

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
	 * The opfuncid is stored in the textual format primarily for debugging
	 * and documentation reasons.  We want to always read it as zero to force
	 * it to be re-looked-up in the pg_operator entry.	This ensures that
	 * stored rules don't have hidden dependencies on operators' functions.
	 * (We don't currently support an ALTER OPERATOR command, but might
	 * someday.)
	 */
	local_node->opfuncid = InvalidOid;

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
	 * The opfuncid is stored in the textual format primarily for debugging
	 * and documentation reasons.  We want to always read it as zero to force
	 * it to be re-looked-up in the pg_operator entry.	This ensures that
	 * stored rules don't have hidden dependencies on operators' functions.
	 * (We don't currently support an ALTER OPERATOR command, but might
	 * someday.)
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
	 * The opfuncid is stored in the textual format primarily for debugging
	 * and documentation reasons.  We want to always read it as zero to force
	 * it to be re-looked-up in the pg_operator entry.	This ensures that
	 * stored rules don't have hidden dependencies on operators' functions.
	 * (We don't currently support an ALTER OPERATOR command, but might
	 * someday.)
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
	READ_NODE_FIELD(testexpr);
	READ_NODE_FIELD(operName);
	READ_NODE_FIELD(subselect);

	READ_DONE();
}

/*
 * _readSubPlan is not needed since it doesn't appear in stored rules.
 */
static SubPlan *
_readSubPlan(void)
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
 * _readRowCompareExpr
 */
static RowCompareExpr *
_readRowCompareExpr(void)
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
_readCoalesceExpr(void)
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
_readMinMaxExpr(void)
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
_readNullIfExpr(void)
{
	READ_LOCALS(NullIfExpr);

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
    /* CDB: subqfromlist is used only within planner; don't need to read it */
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
_readColumnRef(void)
{
	READ_LOCALS(ColumnRef);

	READ_NODE_FIELD(fields);
	READ_INT_FIELD(location);

	READ_DONE();
}

static TypeName *
_readTypeName(void)
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
_readTypeCast(void)
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
 * Greenplum Database additions for serialization support
 * These are currently not used (see outfastc ad readfast.c) 
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
	Assert(local_node->oidInfo.relOid == InvalidOid);

	READ_DONE();
}

static FkConstraint *
_outFkConstraint(void)
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
_readCreateSchemaStmt(void)
{
	READ_LOCALS(CreateSchemaStmt);

	READ_STRING_FIELD(schemaname);
	READ_STRING_FIELD(authid);
	local_node->schemaElts = 0;
	READ_OID_FIELD(schemaOid);

	READ_DONE();
}


static CreatePLangStmt *
_readCreatePLangStmt(void)
{
	READ_LOCALS(CreatePLangStmt);
	
	READ_STRING_FIELD(plname);
	READ_NODE_FIELD(plhandler);
	READ_NODE_FIELD(plvalidator);
	READ_BOOL_FIELD(pltrusted);
	
	READ_DONE();

}

static DropPLangStmt *
_readDropPLangStmt(void)
{
	READ_LOCALS(DropPLangStmt);

	READ_STRING_FIELD(plname);
	READ_ENUM_FIELD(behavior,DropBehavior);
	READ_BOOL_FIELD(missing_ok);
	
	READ_DONE();

}

static CreateSeqStmt *
_readCreateSeqStmt(void)
{
	READ_LOCALS(CreateSeqStmt);
	READ_NODE_FIELD(sequence);
	READ_NODE_FIELD(options);
	
	READ_OID_FIELD(relOid);
	READ_OID_FIELD(comptypeOid);
	
	READ_DONE();
}

static AlterSeqStmt *
_readAlterSeqStmt(void)
{
	READ_LOCALS(AlterSeqStmt);
	READ_NODE_FIELD(sequence);
	READ_NODE_FIELD(options);

	READ_DONE();
}

static ClusterStmt *
_readClusterStmt(void)
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
_readCreatedbStmt(void)
{
	READ_LOCALS(CreatedbStmt);
	READ_STRING_FIELD(dbname);
	READ_NODE_FIELD(options);
	
	READ_OID_FIELD(dbOid);
	
	READ_DONE();
}

static DropdbStmt *
_readDropdbStmt(void)
{
	READ_LOCALS(DropdbStmt);
	
	READ_STRING_FIELD(dbname);
	READ_BOOL_FIELD(missing_ok);
	
	READ_DONE();
}

static CreateDomainStmt *
_readCreateDomainStmt(void)
{
	READ_LOCALS(CreateDomainStmt);

	READ_NODE_FIELD(domainname);
	READ_NODE_FIELD(typname);
	READ_NODE_FIELD(constraints);
	READ_OID_FIELD(domainOid);
		
	READ_DONE();
}

static AlterDomainStmt *
_readAlterDomainStmt(void)
{
	READ_LOCALS(AlterDomainStmt);

	READ_CHAR_FIELD(subtype);
	READ_NODE_FIELD(typname);
	READ_STRING_FIELD(name);
	READ_NODE_FIELD(def);
	READ_ENUM_FIELD(behavior, DropBehavior);
		
	READ_DONE();
}

static CreateFunctionStmt *
_readCreateFunctionStmt(void)
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
_readFunctionParameter(void)
{
	READ_LOCALS(FunctionParameter);
	READ_STRING_FIELD(name);
	READ_NODE_FIELD(argType);
	READ_ENUM_FIELD(mode, FunctionParameterMode);
			
	READ_DONE();
}

static RemoveFuncStmt *
_readRemoveFuncStmt(void)
{
	READ_LOCALS(RemoveFuncStmt);
	READ_ENUM_FIELD(kind,ObjectType);
	READ_NODE_FIELD(name);
	READ_NODE_FIELD(args);
	READ_ENUM_FIELD(behavior, DropBehavior);
	READ_BOOL_FIELD(missing_ok);
			
	READ_DONE();
}

static AlterFunctionStmt *
_readAlterFunctionStmt(void)
{
	READ_LOCALS(AlterFunctionStmt);
	READ_NODE_FIELD(func);
	READ_NODE_FIELD(actions);
	
	READ_DONE();
}


static DefineStmt *
_readDefineStmt(void)
{
	READ_LOCALS(DefineStmt);
	READ_ENUM_FIELD(kind, ObjectType);
	READ_BOOL_FIELD(oldstyle);
	READ_NODE_FIELD(defnames);
	READ_NODE_FIELD(args);
	READ_NODE_FIELD(definition);
	READ_OID_FIELD(newOid);
	READ_OID_FIELD(shadowOid);
	
	READ_DONE();
	
}

static CompositeTypeStmt *
_readCompositeTypeStmt(void)
{
	READ_LOCALS(CompositeTypeStmt);

	READ_NODE_FIELD(typevar);
	READ_NODE_FIELD(coldeflist);
	READ_OID_FIELD(comptypeOid);
	
	READ_DONE();
	
}

static CreateCastStmt *
_readCreateCastStmt(void)
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
_readDropCastStmt(void)
{
	READ_LOCALS(DropCastStmt);
	READ_NODE_FIELD(sourcetype);
	READ_NODE_FIELD(targettype);
	READ_ENUM_FIELD(behavior, DropBehavior);
	READ_BOOL_FIELD(missing_ok);
			
	READ_DONE();
}

static CreateOpClassStmt *
_readCreateOpClassStmt(void)
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
_readCreateOpClassItem(void)
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
_readRemoveOpClassStmt(void)
{
	READ_LOCALS(RemoveOpClassStmt);
	READ_NODE_FIELD(opclassname);
	READ_STRING_FIELD(amname);
	READ_ENUM_FIELD(behavior, DropBehavior);
	READ_BOOL_FIELD(missing_ok);
				
	READ_DONE();
}

static CreateConversionStmt *
_readCreateConversionStmt(void)
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
_readGrantStmt(void)
{
	READ_LOCALS(GrantStmt);
	
	READ_BOOL_FIELD(is_grant);		
	READ_ENUM_FIELD(objtype,GrantObjectType);	
	READ_NODE_FIELD(objects);		
	READ_NODE_FIELD(privileges);		
	READ_NODE_FIELD(grantees);		
	READ_BOOL_FIELD(grant_option);	
	READ_ENUM_FIELD(behavior, DropBehavior);	
	
	READ_DONE();	
}

static PrivGrantee *
_readPrivGrantee(void)
{
	READ_LOCALS(PrivGrantee);
	READ_STRING_FIELD(rolname);
		
	READ_DONE();		
}

static FuncWithArgs *
_readFuncWithArgs(void)
{
	READ_LOCALS(FuncWithArgs);
	READ_NODE_FIELD(funcname);		
	READ_NODE_FIELD(funcargs);	
		
	READ_DONE();		 	
}

static GrantRoleStmt *
_readGrantRoleStmt(void)
{
	READ_LOCALS(GrantRoleStmt);
	READ_NODE_FIELD(granted_roles);
	READ_NODE_FIELD(grantee_roles);
	READ_BOOL_FIELD(is_grant);
	READ_BOOL_FIELD(admin_opt);
	READ_STRING_FIELD(grantor);
	READ_ENUM_FIELD(behavior, DropBehavior);		
	
	READ_DONE();
}

static LockStmt *
_readLockStmt(void)
{
	READ_LOCALS(LockStmt);
	READ_NODE_FIELD(relations);
	READ_INT_FIELD(mode);
	READ_BOOL_FIELD(nowait);
		
	READ_DONE();
}

static ConstraintsSetStmt *
_readConstraintsSetStmt(void)
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
 * _readBitmapAnd, _readBitmapOr
 */
static BitmapAnd *
_readBitmapAnd(void)
{
	READ_LOCALS(BitmapAnd);

	readPlanInfo((Plan *)local_node);

	READ_NODE_FIELD(bitmapplans);

	READ_DONE();
}

static BitmapOr *
_readBitmapOr(void)
{
    READ_LOCALS(BitmapOr);

    readPlanInfo((Plan *)local_node);

    READ_NODE_FIELD(bitmapplans);

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
 * _readExternalScan
 */
static ExternalScan *
_readExternalScan(void)
{
	READ_LOCALS(ExternalScan);
	
	readScanInfo((Scan *)local_node);

	READ_NODE_FIELD(uriList);
	READ_NODE_FIELD(fmtOpts);
	READ_BOOL_FIELD(isText);
	READ_INT_FIELD(scancounter);
	
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

	READ_OID_FIELD(indexid);
	READ_NODE_FIELD(indexqual);
	READ_NODE_FIELD(indexqualorig);
	READ_NODE_FIELD(indexstrategy);
	READ_NODE_FIELD(indexsubtype);

	READ_ENUM_FIELD(indexorderdir, ScanDirection);

	READ_DONE();
}


static BitmapIndexScan *
_readBitmapIndexScan(void)
{
	READ_LOCALS(BitmapIndexScan);

	readScanInfo((Scan *)local_node);

	READ_OID_FIELD(indexid);
	READ_NODE_FIELD(indexqual);
	READ_NODE_FIELD(indexqualorig);
	READ_NODE_FIELD(indexstrategy);
	READ_NODE_FIELD(indexsubtype);
	
	READ_DONE();
}

static BitmapHeapScan *
_readBitmapHeapScan(void)
{
	READ_LOCALS(BitmapHeapScan);

	readScanInfo((Scan *)local_node);

	READ_NODE_FIELD(bitmapqualorig);
	
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

	READ_NODE_FIELD(tidquals);

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
 * _readValuesScan
 */
static ValuesScan *
_readValuesScan(void)
{
	READ_LOCALS_NO_FIELDS(ValuesScan);

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
	READ_LOCALS(NestLoop);

	readJoinInfo((Join *)local_node);

    READ_BOOL_FIELD(outernotreferencedbyinner); /*CDB*/
    READ_BOOL_FIELD(shared_outer);

    READ_DONE();
}

/*
 * _readAdaptiveNestLoop
 */
static AdaptiveNestLoop *
_readAdaptiveNestLoop(void)
{
	READ_LOCALS(AdaptiveNestLoop);

	readPlanInfo((Plan *)local_node);

	READ_INT_FIELD(nl_strategy_threshold);

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
	READ_INT_FIELD(transSpace);
	READ_INT_FIELD(numNullCols);
	READ_UINT64_FIELD(inputGrouping);
	READ_UINT64_FIELD(grouping);
	READ_BOOL_FIELD(inputHasGrouping);
	READ_INT_FIELD(rollupGSTimes);
	READ_BOOL_FIELD(lastAgg);

	READ_DONE();
}

/*
 * _readWindowKey
 */
static WindowKey *
_readWindowKey(void)
{
	READ_LOCALS(WindowKey);

	READ_INT_FIELD(numSortCols);
	READ_INT_ARRAY_OR_NULL(sortColIdx, numSortCols, AttrNumber);
	READ_OID_ARRAY(sortOperators, numSortCols);
	READ_NODE_FIELD(frame);

	READ_DONE();
}

/*
 * _readWindow
 */
static Window *
_readWindow(void)
{
	READ_LOCALS(Window);

	readPlanInfo((Plan *)local_node);

	READ_INT_FIELD(numPartCols);
	READ_INT_ARRAY_OR_NULL(partColIdx, numPartCols, AttrNumber);
	READ_NODE_FIELD(windowKeys);

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

	READ_INT_FIELD(numNullCols);
	READ_UINT64_FIELD(inputGrouping);
	READ_UINT64_FIELD(grouping);
	READ_BOOL_FIELD(inputHasGrouping);
	READ_INT_FIELD(rollupGSTimes);
	READ_BOOL_FIELD(lastGroup);
		
	READ_DONE();
}

/*
 * _readMaterial
 */
static Material *
_readMaterial(void)
{
	READ_LOCALS(Material);

    READ_BOOL_FIELD(cdb_strict);

	READ_ENUM_FIELD(share_type, ShareType);
	READ_INT_FIELD(share_id);
	READ_INT_FIELD(driver_slice);
	READ_INT_FIELD(nsharer);
	READ_INT_FIELD(nsharer_xslice_notify_ready);
	READ_INT_FIELD(nsharer_xslice_wait_done);

    readPlanInfo((Plan *)local_node);

	READ_DONE();
}

/*
 * _readShareInputScan
 */
static ShareInputScan *
_readShareInputScan(void)
{
	READ_LOCALS(ShareInputScan);

	READ_ENUM_FIELD(share_type, ShareType);
	READ_INT_FIELD(share_id);
	READ_INT_FIELD(driver_slice);
	READ_BOOL_FIELD(need_wait_ready);

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

    /* CDB */
	READ_NODE_FIELD(limitOffset);
	READ_NODE_FIELD(limitCount);
	READ_BOOL_FIELD(noduplicates);

	READ_ENUM_FIELD(share_type, ShareType);
	READ_INT_FIELD(share_id);
	READ_INT_FIELD(driver_slice);
	READ_INT_FIELD(nsharer);
	READ_INT_FIELD(nsharer_xslice_notify_ready);
	READ_INT_FIELD(nsharer_xslice_wait_done);

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

	READ_INT_FIELD(motionID);
	READ_ENUM_FIELD(motionType, MotionType);

	READ_BOOL_FIELD(sendToSecondary);
	READ_BOOL_FIELD(sendSorted);
	
    READ_INT_FIELD(numInputSegs);
    READ_INT_ARRAY_OR_NULL(inputSegIdx, numInputSegs, int);

	READ_INT_FIELD(numHashSegs);
	READ_INT_ARRAY(hashSegIdx, numHashSegs, int);

	READ_NODE_FIELD(hashExpr);
	READ_NODE_FIELD(hashDataTypes);

	READ_INT_FIELD(numOutputSegs);
	READ_INT_ARRAY(outputSegIdx, numOutputSegs, int);

	READ_INT_FIELD(numSortCols);
	READ_INT_ARRAY(sortColIdx, numSortCols, AttrNumber);
	READ_OID_ARRAY(sortOperators, numSortCols);

	readPlanInfo((Plan *)local_node);

	READ_DONE();
}

static Flow *
_readFlow(void)
{
	READ_LOCALS(Flow);

	READ_ENUM_FIELD(flotype, FlowType);
	READ_ENUM_FIELD(req_move, Movement);
	READ_ENUM_FIELD(locustype, CdbLocusType);
	READ_INT_FIELD(segindex);

	READ_INT_FIELD(numSortCols);
	READ_INT_ARRAY_OR_NULL(sortColIdx, numSortCols, AttrNumber);
	READ_UINT_ARRAY_OR_NULL(sortOperators, numSortCols, Oid);

	READ_NODE_FIELD(hashExpr);
	READ_NODE_FIELD(flow_before_req_move);

	READ_DONE();
}

/*
 * _readVacuumStmt
 */
static VacuumStmt *
_readVacuumStmt(void)
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
_readCdbProcess(void)
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
_readSlice(void)
{
	READ_LOCALS(Slice);
	
	READ_INT_FIELD(sliceIndex);
	READ_INT_FIELD(rootIndex);
	READ_ENUM_FIELD(gangType, GangType);
	READ_INT_FIELD(gangSize);
	READ_DUMMY_FIELD(primaryGang, NULL);
	READ_DUMMY_FIELD(mirrorGang, NULL);
	READ_INT_FIELD(primary_gang_id);
	READ_INT_FIELD(mirror_gang_id);
	READ_INT_FIELD(parentIndex); /* List of int index */
	READ_NODE_FIELD(children); /* List of int index */
	READ_NODE_FIELD(primaryProcesses); /* List of (CDBProcess *) */
	READ_NODE_FIELD(mirrorProcesses); /* List of (CDBProcess *) */
	
	READ_DONE();
}

static SliceTable *
_readSliceTable(void)
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
_readVariableResetStmt(void)
{
	READ_LOCALS(VariableResetStmt);
	READ_STRING_FIELD(name);
	
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
	READ_BITMAPSET_FIELD(extParam);
	READ_BITMAPSET_FIELD(allParam);
	READ_INT_FIELD(nParamExec);

	READ_NODE_FIELD(flow);
	READ_ENUM_FIELD(dispatch, DispatchMethod);
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

void readJoinInfo(Join *local_node)
{
	READ_TEMP_LOCALS();

	readPlanInfo((Plan *)local_node);

	READ_BOOL_FIELD(prefetch_inner);

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

static CreateTrigStmt *
_readCreateTrigStmt(void)
{
	READ_LOCALS(CreateTrigStmt);
	
	READ_STRING_FIELD(trigname);
	READ_NODE_FIELD(relation);
	READ_NODE_FIELD(funcname);
	READ_NODE_FIELD(args);
	READ_BOOL_FIELD(before);
	READ_BOOL_FIELD(row);
	token = pg_strtok(&length);		/* skip :fldname */ 
	token = pg_strtok(&length);		/* get field value */ 
	strcpy(local_node->actions, debackslash(token, length));
	READ_BOOL_FIELD(isconstraint);
	READ_BOOL_FIELD(deferrable);
	READ_BOOL_FIELD(initdeferred);
	READ_NODE_FIELD(constrrel);
	READ_OID_FIELD(trigOid);
	
	READ_DONE();

}
/*
 * Greenplum Database developers added code to improve performance over the
 * linear searching that existed in the postgres version of
 * parseNodeString.  We introduced a struct containing
 * the node type string and read function pointer.
 * We created a sorted array of these for all node types supported
 * by parseNodeString, with Greenplum Database extensions.
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
	{"ADAPTIVENESTLOOP", (ReadFn)_readAdaptiveNestLoop},
	{"AEXPR", (ReadFn)_readAExpr},
	{"AGG", (ReadFn)_readAgg},
	{"AGGREF", (ReadFn)_readAggref},
	{"ALIAS", (ReadFn)_readAlias},
	{"ALTERDOMAINSTMT", (ReadFn)_readAlterDomainStmt},
	{"ALTERFUNCTIONSTMT", (ReadFn)_readAlterFunctionStmt},
	{"ALTEROBJECTSCHEMASTMT", (ReadFn)_readAlterObjectSchemaStmt},
	{"ALTEROWNERSTMT", (ReadFn)_readAlterOwnerStmt},
	{"ALTERROLESETSTMT", (ReadFn)_readAlterRoleSetStmt},
	{"ALTERROLESTMT", (ReadFn)_readAlterRoleStmt},
	{"ALTERSEQSTMT", (ReadFn)_readAlterSeqStmt},
	{"ALTERTABLECMD", (ReadFn)_readAlterTableCmd},
	{"ALTERTABLESTMT", (ReadFn)_readAlterTableStmt},
	{"APPEND", (ReadFn)_readAppend},
	{"ARRAY", (ReadFn)_readArrayExpr},
	{"ARRAYREF", (ReadFn)_readArrayRef},
	{"A_CONST", (ReadFn)_readAConst},	
	{"BITMAPAND", (ReadFn)_readBitmapAnd},
	{"BITMAPHEAPSCAN", (ReadFn)_readBitmapHeapScan},
	{"BITMAPINDEXSCAN", (ReadFn)_readBitmapIndexScan},
	{"BITMAPOR", (ReadFn)_readBitmapOr},
	{"BOOLEANTEST", (ReadFn)_readBooleanTest},
	{"BOOLEXPR", (ReadFn)_readBoolExpr},
	{"CASE", (ReadFn)_readCaseExpr},
	{"CASETESTEXPR", (ReadFn)_readCaseTestExpr},
	{"CDBPROCESS", (ReadFn)_readCdbProcess},
	{"CLUSTERSTMT", (ReadFn)_readClusterStmt},
	{"COALESCE", (ReadFn)_readCoalesceExpr},
	{"COERCETODOMAIN", (ReadFn)_readCoerceToDomain},
	{"COERCETODOMAINVALUE", (ReadFn)_readCoerceToDomainValue},
	{"COLUMNDEF", (ReadFn)_readColumnDef},
	{"COLUMNREF", (ReadFn)_readColumnRef},
	{"COMPTYPESTMT", (ReadFn)_readCompositeTypeStmt},
	{"CONST", (ReadFn)_readConst},
	{"CONSTRAINT", (ReadFn)_readConstraint},
	{"CONSTRAINTSSETSTMT", (ReadFn)_readConstraintsSetStmt},
	{"CONVERTROWTYPEEXPR", (ReadFn)_readConvertRowtypeExpr},
	{"CREATECAST", (ReadFn)_readCreateCastStmt},
	{"CREATECONVERSION", (ReadFn)_readCreateConversionStmt},
	{"CREATEDBSTMT", (ReadFn)_readCreatedbStmt},
	{"CREATEDOMAINSTMT", (ReadFn)_readCreateDomainStmt},
	{"CREATEFUNCSTMT", (ReadFn)_readCreateFunctionStmt},
	{"CREATEOPCLASS", (ReadFn)_readCreateOpClassStmt},
	{"CREATEOPCLASSITEM", (ReadFn)_readCreateOpClassItem},
	{"CREATEPLANGSTMT", (ReadFn)_readCreatePLangStmt},
	{"CREATEROLESTMT", (ReadFn)_readCreateRoleStmt},
	{"CREATESCHEMASTMT", (ReadFn)_readCreateSchemaStmt},
	{"CREATESEQSTMT", (ReadFn)_readCreateSeqStmt},
	{"CREATESTMT", (ReadFn)_readCreateStmt},
	{"CREATETRIGSTMT", (ReadFn)_readCreateTrigStmt},
	{"DECLARECURSOR", (ReadFn)_readDeclareCursorStmt},
	{"DEFELEM", (ReadFn)_readDefElem},
	{"DEFINESTMT", (ReadFn)_readDefineStmt},
	{"DISTINCTEXPR", (ReadFn)_readDistinctExpr},
	{"DROPCAST", (ReadFn)_readDropCastStmt},
	{"DROPDBSTMT", (ReadFn)_readDropdbStmt},
	{"DROPPLANGSTMT", (ReadFn)_readDropPLangStmt},
	{"DROPPROPSTMT", (ReadFn)_readDropPropertyStmt},
	{"DROPROLESTMT", (ReadFn)_readDropRoleStmt},
	{"DROPSTMT", (ReadFn)_readDropStmt},
	{"EXTERNALSCAN", (ReadFn)_readExternalScan},
	{"FIELDSELECT", (ReadFn)_readFieldSelect},
	{"FIELDSTORE", (ReadFn)_readFieldStore},
	{"FKCONSTRAINT", (ReadFn)_outFkConstraint},
	{"FLOW", (ReadFn)_readFlow},
	{"FROMEXPR", (ReadFn)_readFromExpr},
	{"FUNCCALL", (ReadFn)_readFuncCall},
	{"FUNCEXPR", (ReadFn)_readFuncExpr},
	{"FUNCTIONPARAMETER", (ReadFn)_readFunctionParameter},
	{"FUNCTIONSCAN", (ReadFn)_readFunctionScan},
	{"FUNCWITHARGS", (ReadFn)_readFuncWithArgs},
	{"GRANTROLESTMT", (ReadFn)_readGrantRoleStmt},	
	{"GRANTSTMT", (ReadFn)_readGrantStmt},
	{"GROUP", (ReadFn)_readGroup},
	{"GROUPCLAUSE", (ReadFn)_readGroupClause},
	{"GROUPID", (ReadFn)_readGroupId},
	{"GROUPING", (ReadFn)_readGrouping},
	{"GROUPINGCLAUSE", (ReadFn)_readGroupingClause},
	{"GROUPINGFUNC", (ReadFn)_readGroupingFunc},
	{"HASH", (ReadFn)_readHash},
	{"HASHJOIN", (ReadFn)_readHashJoin},
	{"INDEXELEM", (ReadFn)_readIndexElem},
	{"INDEXSCAN", (ReadFn)_readIndexScan},
	{"INDEXSTMT", (ReadFn)_readIndexStmt},
	{"JOIN", (ReadFn)_readJoin},
	{"JOINEXPR", (ReadFn)_readJoinExpr},
	{"LIMIT", (ReadFn)_readLimit},
	{"LOCKSTMT", (ReadFn)_readLockStmt},
	{"MATERIAL", (ReadFn)_readMaterial},
	{"MERGEJOIN", (ReadFn)_readMergeJoin},
	{"MINMAX", (ReadFn)_readMinMaxExpr},
	{"MOTION", (ReadFn)_readMotion},
	{"NESTLOOP", (ReadFn)_readNestLoop},
	{"NOTIFY", (ReadFn)_readNotifyStmt},
	{"NULLIFEXPR", (ReadFn)_readNullIfExpr},
	{"NULLTEST", (ReadFn)_readNullTest},
	{"OPEXPR", (ReadFn)_readOpExpr},
	{"PARAM", (ReadFn)_readParam},
	{"PLAN", (ReadFn)_readPlan},
	{"PRIVGRANTEE", (ReadFn)_readPrivGrantee},
	{"QUERY", (ReadFn)_readQuery},
	{"RANGETBLREF", (ReadFn)_readRangeTblRef},
	{"RANGEVAR", (ReadFn)_readRangeVar},
	{"REINDEXSTMT", (ReadFn)_readReindexStmt},
	{"RELABELTYPE", (ReadFn)_readRelabelType},
	{"REMOVEFUNCSTMT", (ReadFn)_readRemoveFuncStmt},
	{"REMOVEOPCLASS", (ReadFn)_readRemoveOpClassStmt},
	{"RENAMESTMT", (ReadFn)_readRenameStmt},
	{"RESULT", (ReadFn)_readResult},
	{"ROW", (ReadFn)_readRowExpr},
	{"ROWCOMPAREEXPR", (ReadFn)_readRowCompareExpr},
	{"ROWMARKCLAUSE", (ReadFn)_readRowMarkClause},
	{"RTE", (ReadFn)_readRangeTblEntry},
	{"RULESTMT", (ReadFn)_readRuleStmt},
	{"SCALARARRAYOPEXPR", (ReadFn)_readScalarArrayOpExpr},
	{"SCAN", (ReadFn)_readScan},
	{"SEQSCAN", (ReadFn)_readSeqScan},
	{"SETOP", (ReadFn)_readSetOp},
	{"SETOPERATIONSTMT", (ReadFn)_readSetOperationStmt},
	{"SETTODEFAULT", (ReadFn)_readSetToDefault},
	{"SHAREINPUTSCAN", (ReadFn)_readShareInputScan},
	{"SLICE", (ReadFn)_readSlice},
	{"SLICETABLE", (ReadFn)_readSliceTable},
	{"SORT", (ReadFn)_readSort},
	{"SORTCLAUSE", (ReadFn)_readSortClause},
	{"SUBLINK", (ReadFn)_readSubLink},
	{"SUBPLAN", (ReadFn)_readSubPlan},
	{"SUBQUERYSCAN", (ReadFn)_readSubqueryScan},
	{"TARGETENTRY", (ReadFn)_readTargetEntry},
	{"TIDSCAN", (ReadFn)_readTidScan},
	{"TRUNCATESTMT", (ReadFn)_readTruncateStmt},
	{"TYPECAST", (ReadFn)_readTypeCast},
	{"TYPENAME", (ReadFn)_readTypeName},
	{"UNIQUE", (ReadFn)_readUnique},
	{"VACUUMSTMT", (ReadFn)_readVacuumStmt},
	{"VALUESSCAN", (ReadFn)_readValuesScan},    /*CDB*/
	{"VAR", (ReadFn)_readVar},
	{"VARIABLERESETSTMT", (ReadFn)_readVariableResetStmt},
	{"VIEWSTMT", (ReadFn)_readViewStmt},
	{"WHEN", (ReadFn)_readCaseWhen},
	{"WINDOW", (ReadFn)_readWindow},
	{"WINDOWFRAME", (ReadFn)_readWindowFrame},
	{"WINDOWFRAMEEDGE", (ReadFn)_readWindowFrameEdge},
	{"WINDOWKEY", (ReadFn)_readWindowKey},
	{"WINDOWREF", (ReadFn)_readWindowRef},
	{"WINDOWSPEC", (ReadFn)_readWindowSpec},
	{"WINDOWSPECPARSE", (ReadFn)_readWindowSpecParse},
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
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("This operation involves an internal data item "
                               "of a type called \"%.*s\" which is not "
                               "supported in this version of %s.",
                               length, token, PACKAGE_NAME)
                ));
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
