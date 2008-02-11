/*-------------------------------------------------------------------------
 *
 * parse_type.c
 *		handle type operations for parser
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_type.c,v 1.94 2008/01/01 19:45:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "parser/parser.h"
#include "parser/parse_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static int32 typenameTypeMod(ParseState *pstate, const TypeName *typname,
				Type typ);


/*
 * LookupTypeName
 *		Given a TypeName object, lookup the pg_type syscache entry of the type.
 *		Returns NULL if no such type can be found.	If the type is found,
 *		the typmod value represented in the TypeName struct is computed and
 *		stored into *typmod_p.
 *
 * NB: on success, the caller must ReleaseSysCache the type tuple when done
 * with it.
 *
 * NB: direct callers of this function MUST check typisdefined before assuming
 * that the type is fully valid.  Most code should go through typenameType
 * or typenameTypeId instead.
 *
 * typmod_p can be passed as NULL if the caller does not care to know the
 * typmod value, but the typmod decoration (if any) will be validated anyway,
 * except in the case where the type is not found.	Note that if the type is
 * found but is a shell, and there is typmod decoration, an error will be
 * thrown --- this is intentional.
 *
 * pstate is only used for error location info, and may be NULL.
 */
Type
LookupTypeName(ParseState *pstate, const TypeName *typname,
			   int32 *typmod_p)
{
	Oid			typoid;
	HeapTuple	tup;
	int32		typmod;

	if (typname->names == NIL)
	{
		/* We have the OID already if it's an internally generated TypeName */
		typoid = typname->typid;
	}
	else if (typname->pct_type)
	{
		/* Handle %TYPE reference to type of an existing field */
		RangeVar   *rel = makeRangeVar(NULL, NULL);
		char	   *field = NULL;
		Oid			relid;
		AttrNumber	attnum;

		/* deconstruct the name list */
		switch (list_length(typname->names))
		{
			case 1:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("improper %%TYPE reference (too few dotted names): %s",
					   NameListToString(typname->names)),
						 parser_errposition(pstate, typname->location)));
				break;
			case 2:
				rel->relname = strVal(linitial(typname->names));
				field = strVal(lsecond(typname->names));
				break;
			case 3:
				rel->schemaname = strVal(linitial(typname->names));
				rel->relname = strVal(lsecond(typname->names));
				field = strVal(lthird(typname->names));
				break;
			case 4:
				rel->catalogname = strVal(linitial(typname->names));
				rel->schemaname = strVal(lsecond(typname->names));
				rel->relname = strVal(lthird(typname->names));
				field = strVal(lfourth(typname->names));
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("improper %%TYPE reference (too many dotted names): %s",
								NameListToString(typname->names)),
						 parser_errposition(pstate, typname->location)));
				break;
		}

		/* look up the field */
		relid = RangeVarGetRelid(rel, false);
		attnum = get_attnum(relid, field);
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							field, rel->relname),
					 parser_errposition(pstate, typname->location)));
		typoid = get_atttype(relid, attnum);

		/* this construct should never have an array indicator */
		Assert(typname->arrayBounds == NIL);

		/* emit nuisance notice */
		ereport(NOTICE,
				(errmsg("type reference %s converted to %s",
						TypeNameToString(typname),
						format_type_be(typoid))));
	}
	else
	{
		/* Normal reference to a type name */
		char	   *schemaname;
		char	   *tname;

		/* deconstruct the name list */
		DeconstructQualifiedName(typname->names, &schemaname, &tname);

		if (schemaname)
		{
			/* Look in specific schema only */
			Oid			namespaceId;

			namespaceId = LookupExplicitNamespace(schemaname);
			typoid = GetSysCacheOid(TYPENAMENSP,
									PointerGetDatum(tname),
									ObjectIdGetDatum(namespaceId),
									0, 0);
		}
		else
		{
			/* Unqualified type name, so search the search path */
			typoid = TypenameGetTypid(tname);
		}

		/* If an array reference, return the array type instead */
		if (typname->arrayBounds != NIL)
			typoid = get_array_type(typoid);
	}

	if (!OidIsValid(typoid))
	{
		if (typmod_p)
			*typmod_p = -1;
		return NULL;
	}

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for type %u", typoid);

	typmod = typenameTypeMod(pstate, typname, (Type) tup);

	if (typmod_p)
		*typmod_p = typmod;

	return (Type) tup;
}

/*
 * typenameType - given a TypeName, return a Type structure and typmod
 *
 * This is equivalent to LookupTypeName, except that this will report
 * a suitable error message if the type cannot be found or is not defined.
 * Callers of this can therefore assume the result is a fully valid type.
 */
Type
typenameType(ParseState *pstate, const TypeName *typname, int32 *typmod_p)
{
	Type		tup;

	tup = LookupTypeName(pstate, typname, typmod_p);
	if (tup == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typname)),
				 parser_errposition(pstate, typname->location)));
	if (!((Form_pg_type) GETSTRUCT(tup))->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is only a shell",
						TypeNameToString(typname)),
				 parser_errposition(pstate, typname->location)));
	return tup;
}

/*
 * typenameTypeId - given a TypeName, return the type's OID and typmod
 *
 * This is equivalent to typenameType, but we only hand back the type OID
 * not the syscache entry.
 */
Oid
typenameTypeId(ParseState *pstate, const TypeName *typname, int32 *typmod_p)
{
	Oid			typoid;
	Type		tup;

	tup = typenameType(pstate, typname, typmod_p);
	typoid = HeapTupleGetOid(tup);
	ReleaseSysCache(tup);

	return typoid;
}

/*
 * typenameTypeMod - given a TypeName, return the internal typmod value
 *
 * This will throw an error if the TypeName includes type modifiers that are
 * illegal for the data type.
 *
 * The actual type OID represented by the TypeName must already have been
 * looked up, and is passed as "typ".
 *
 * pstate is only used for error location info, and may be NULL.
 */
static int32
typenameTypeMod(ParseState *pstate, const TypeName *typname, Type typ)
{
	int32		result;
	Oid			typmodin;
	Datum	   *datums;
	int			n;
	ListCell   *l;
	ArrayType  *arrtypmod;

	/* Return prespecified typmod if no typmod expressions */
	if (typname->typmods == NIL)
		return typname->typmod;

	/*
	 * Else, type had better accept typmods.  We give a special error message
	 * for the shell-type case, since a shell couldn't possibly have a
	 * typmodin function.
	 */
	if (!((Form_pg_type) GETSTRUCT(typ))->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("type modifier cannot be specified for shell type \"%s\"",
				   TypeNameToString(typname)),
				 parser_errposition(pstate, typname->location)));

	typmodin = ((Form_pg_type) GETSTRUCT(typ))->typmodin;

	if (typmodin == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("type modifier is not allowed for type \"%s\"",
						TypeNameToString(typname)),
				 parser_errposition(pstate, typname->location)));

	/*
	 * Convert the list of raw-grammar-output expressions to a cstring array.
	 * Currently, we allow simple numeric constants, string literals, and
	 * identifiers; possibly this list could be extended.
	 */
	datums = (Datum *) palloc(list_length(typname->typmods) * sizeof(Datum));
	n = 0;
	foreach(l, typname->typmods)
	{
		Node	   *tm = (Node *) lfirst(l);
		char	   *cstr = NULL;

		if (IsA(tm, A_Const))
		{
			A_Const    *ac = (A_Const *) tm;

			/*
			 * The grammar hands back some integers with ::int4 attached, so
			 * allow a cast decoration if it's an Integer value, but not
			 * otherwise.
			 */
			if (IsA(&ac->val, Integer))
			{
				cstr = (char *) palloc(32);
				snprintf(cstr, 32, "%ld", (long) ac->val.val.ival);
			}
			else if (ac->typname == NULL)		/* no casts allowed */
			{
				/* otherwise we can just use the str field directly. */
				cstr = ac->val.val.str;
			}
		}
		else if (IsA(tm, ColumnRef))
		{
			ColumnRef  *cr = (ColumnRef *) tm;

			if (list_length(cr->fields) == 1)
				cstr = strVal(linitial(cr->fields));
		}
		if (!cstr)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("type modifiers must be simple constants or identifiers"),
					 parser_errposition(pstate, typname->location)));
		datums[n++] = CStringGetDatum(cstr);
	}

	/* hardwired knowledge about cstring's representation details here */
	arrtypmod = construct_array(datums, n, CSTRINGOID,
								-2, false, 'c');

	result = DatumGetInt32(OidFunctionCall1(typmodin,
											PointerGetDatum(arrtypmod)));

	pfree(datums);
	pfree(arrtypmod);

	return result;
}

/*
 * appendTypeNameToBuffer
 *		Append a string representing the name of a TypeName to a StringInfo.
 *		This is the shared guts of TypeNameToString and TypeNameListToString.
 *
 * NB: this must work on TypeNames that do not describe any actual type;
 * it is mostly used for reporting lookup errors.
 */
static void
appendTypeNameToBuffer(const TypeName *typname, StringInfo string)
{
	if (typname->names != NIL)
	{
		/* Emit possibly-qualified name as-is */
		ListCell   *l;

		foreach(l, typname->names)
		{
			if (l != list_head(typname->names))
				appendStringInfoChar(string, '.');
			appendStringInfoString(string, strVal(lfirst(l)));
		}
	}
	else
	{
		/* Look up internally-specified type */
		appendStringInfoString(string, format_type_be(typname->typid));
	}

	/*
	 * Add decoration as needed, but only for fields considered by
	 * LookupTypeName
	 */
	if (typname->pct_type)
		appendStringInfoString(string, "%TYPE");

	if (typname->arrayBounds != NIL)
		appendStringInfoString(string, "[]");
}

/*
 * TypeNameToString
 *		Produce a string representing the name of a TypeName.
 *
 * NB: this must work on TypeNames that do not describe any actual type;
 * it is mostly used for reporting lookup errors.
 */
char *
TypeNameToString(const TypeName *typname)
{
	StringInfoData string;

	initStringInfo(&string);
	appendTypeNameToBuffer(typname, &string);
	return string.data;
}

/*
 * TypeNameListToString
 *		Produce a string representing the name(s) of a List of TypeNames
 */
char *
TypeNameListToString(List *typnames)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);
	foreach(l, typnames)
	{
		TypeName   *typname = (TypeName *) lfirst(l);

		Assert(IsA(typname, TypeName));
		if (l != list_head(typnames))
			appendStringInfoChar(&string, ',');
		appendTypeNameToBuffer(typname, &string);
	}
	return string.data;
}

#ifdef  XXX_OBSOLETE
/*
 * typenameTypeId - given a TypeName, return the type's OID
 *
 * This is equivalent to LookupTypeName, except that this will report
 * a suitable error message if the type cannot be found or is not defined.
 */
Oid
typenameTypeId(ParseState *pstate, const TypeName *typname)
{
	Oid			typoid;

	typoid = LookupTypeName(pstate, typname);
	if (!OidIsValid(typoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typname)),
				 parser_errposition(pstate, typname->location)));

	if (!get_typisdefined(typoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is only a shell",
						TypeNameToString(typname)),
				 parser_errposition(pstate, typname->location)));
	return typoid;
}

/*
 * typenameType - given a TypeName, return a Type structure
 *
 * This is equivalent to typenameTypeId + syscache fetch of Type tuple.
 * NB: caller must ReleaseSysCache the type tuple when done with it.
 */
Type
typenameType(ParseState *pstate, const TypeName *typname)
{
	Oid			typoid;
	HeapTuple	tup;

	typoid = LookupTypeName(pstate, typname);
	if (!OidIsValid(typoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typname))));
	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for type %u", typoid);
	if (!((Form_pg_type) GETSTRUCT(tup))->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is only a shell",
						TypeNameToString(typname)),
				 parser_errposition(pstate, typname->location)));
	return (Type) tup;
}
#endif XXX_OBSOLETE

/* return a Type structure, given a type id */
/* NB: caller must ReleaseSysCache the type tuple when done with it */
Type
typeidType(Oid id)
{
	HeapTuple	tup;

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(id),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", id);
	return (Type) tup;
}

/* given type (as type struct), return the type OID */
Oid
typeTypeId(Type tp)
{
	if (tp == NULL)				/* probably useless */
		elog(ERROR, "typeTypeId() called with NULL type struct");
	return HeapTupleGetOid(tp);
}

/* given type (as type struct), return the length of type */
int16
typeLen(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typlen;
}

/* given type (as type struct), return the value of its 'byval' attribute.*/
bool
typeByVal(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typbyval;
}

#ifdef XXX_OBSOLETE
/* given type (as type struct), return the value of its 'typtype' attribute.*/
char
typeTypType(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typtype;
}
#endif  XXX_OBSOLETE
/* given type (as type struct), return the name of type */
char *
typeTypeName(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	/* pstrdup here because result may need to outlive the syscache entry */
	return pstrdup(NameStr(typ->typname));
}

Oid
typeTypeRelid(Type typ)
{
	Form_pg_type typtup;

	typtup = (Form_pg_type) GETSTRUCT(typ);

	return typtup->typrelid;
}

/*
 * Given a type structure and a string, returns the internal representation
 * of that string.	The "string" can be NULL to perform conversion of a NULL
 * (which might result in failure, if the input function rejects NULLs).
 */
Datum
stringTypeDatum(Type tp, char *string, int32 atttypmod)
{
	Oid			typinput;
	Oid			typioparam;

	typinput = ((Form_pg_type) GETSTRUCT(tp))->typinput;
	typioparam = getTypeIOParam(tp);
	return OidInputFunctionCall(typinput, string,
								typioparam, atttypmod);
}

/* given a typeid, return the type's typrelid (associated relation, if any) */
Oid
typeidTypeRelid(Oid type_id)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	Oid			result;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type_id),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type_id);

	type = (Form_pg_type) GETSTRUCT(typeTuple);
	result = type->typrelid;
	ReleaseSysCache(typeTuple);
	return result;
}

/*
 * error context callback for parse failure during parseTypeString()
 */
static void
pts_error_callback(void *arg)
{
	const char *str = (const char *) arg;

	errcontext("invalid type name \"%s\"", str);

	/*
	 * Currently we just suppress any syntax error position report, rather
	 * than transforming to an "internal query" error.	It's unlikely that a
	 * type name is complex enough to need positioning.
	 */
	errposition(0);
}

/*
 * Given a string that is supposed to be a SQL-compatible type declaration,
 * such as "int4" or "integer" or "character varying(32)", parse
 * the string and convert it to a type OID and type modifier.
 */
void
parseTypeString(const char *str, Oid *type_id, int32 *typmod_p)
{
	StringInfoData buf;
	List	   *raw_parsetree_list;
	SelectStmt *stmt;
	ResTarget  *restarget;
	TypeCast   *typecast;
	TypeName   *typname;
	ErrorContextCallback ptserrcontext;

	/* make sure we give useful error for empty input */
	if (strspn(str, " \t\n\r\f") == strlen(str))
		goto fail;

	initStringInfo(&buf);
	appendStringInfo(&buf, "SELECT NULL::%s", str);

	/*
	 * Setup error traceback support in case of ereport() during parse
	 */
	ptserrcontext.callback = pts_error_callback;
	ptserrcontext.arg = (void *) str;
	ptserrcontext.previous = error_context_stack;
	error_context_stack = &ptserrcontext;

	raw_parsetree_list = raw_parser(buf.data);

	error_context_stack = ptserrcontext.previous;

	/*
	 * Make sure we got back exactly what we expected and no more; paranoia is
	 * justified since the string might contain anything.
	 */
	if (list_length(raw_parsetree_list) != 1)
		goto fail;
	stmt = (SelectStmt *) linitial(raw_parsetree_list);
	if (stmt == NULL ||
		!IsA(stmt, SelectStmt) ||
		stmt->distinctClause != NIL ||
		stmt->intoClause != NULL ||
		stmt->fromClause != NIL ||
		stmt->whereClause != NULL ||
		stmt->groupClause != NIL ||
		stmt->havingClause != NULL ||
		stmt->valuesLists != NIL ||
		stmt->sortClause != NIL ||
		stmt->limitOffset != NULL ||
		stmt->limitCount != NULL ||
		stmt->lockingClause != NIL ||
		stmt->op != SETOP_NONE)
		goto fail;
	if (list_length(stmt->targetList) != 1)
		goto fail;
	restarget = (ResTarget *) linitial(stmt->targetList);
	if (restarget == NULL ||
		!IsA(restarget, ResTarget) ||
		restarget->name != NULL ||
		restarget->indirection != NIL)
		goto fail;
	typecast = (TypeCast *) restarget->val;
	if (typecast == NULL ||
		!IsA(typecast, TypeCast) ||
		typecast->arg == NULL ||
		!IsA(typecast->arg, A_Const))
		goto fail;
	typname = typecast->typname;
	if (typname == NULL ||
		!IsA(typname, TypeName))
		goto fail;
	if (typname->setof)
		goto fail;

	*type_id = typenameTypeId(NULL, typname, typmod_p);

	pfree(buf.data);

	return;

fail:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid type name \"%s\"", str)));
}
