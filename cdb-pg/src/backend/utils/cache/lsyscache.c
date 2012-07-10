/*-------------------------------------------------------------------------
 *
 * lsyscache.c
 *	  Convenience routines for common queries in the system catalog cache.
 *
 * Portions Copyright (c) 2007-2009, Greenplum inc
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/cache/lsyscache.c,v 1.138.2.1 2007/03/19 16:30:40 tgl Exp $
 *
 * NOTES
 *	  Eventually, the index information should go through here, too.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/genam.h"
#include "bootstrap/bootstrap.h"
#include "catalog/heap.h"                   /* SystemAttributeDefinition() */    
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "cdb/cdbpartition.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_clause.h"			/* for sort_op_can_sort() */
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"
#include "funcapi.h"

/*				---------- AMOP CACHES ----------						 */

/*
 * op_in_opclass
 *
 *		Return t iff operator 'opno' is in operator class 'opclass'.
 */
bool
op_in_opclass(Oid opno, Oid opclass)
{
	return SearchSysCacheExists(AMOPOPID,
								ObjectIdGetDatum(opno),
								ObjectIdGetDatum(opclass),
								0, 0);
}

/*
 * get_op_opclass_strategy
 *
 *		Get the operator's strategy number within the specified opclass,
 *		or 0 if it's not a member of the opclass.
 */
int
get_op_opclass_strategy(Oid opno, Oid opclass)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	int			result;

	tp = SearchSysCache(AMOPOPID,
						ObjectIdGetDatum(opno),
						ObjectIdGetDatum(opclass),
						0, 0);
	if (!HeapTupleIsValid(tp))
		return 0;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopstrategy;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_op_opclass_properties
 *
 *		Get the operator's strategy number, subtype, and recheck (lossy) flag
 *		within the specified opclass.
 *
 * Caller should already have verified that opno is a member of opclass,
 * therefore we raise an error if the tuple is not found.
 */
void
get_op_opclass_properties(Oid opno, Oid opclass,
						  int *strategy, Oid *subtype, bool *recheck)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;

	tp = SearchSysCache(AMOPOPID,
						ObjectIdGetDatum(opno),
						ObjectIdGetDatum(opclass),
						0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "operator %u is not a member of opclass %u",
			 opno, opclass);
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	*strategy = amop_tup->amopstrategy;
	*subtype = amop_tup->amopsubtype;
	*recheck = amop_tup->amopreqcheck;
	ReleaseSysCache(tp);
}

/*
 * get_opclass_member
 *		Get the OID of the operator that implements the specified strategy
 *		with the specified subtype for the specified opclass.
 *
 * Returns InvalidOid if there is no pg_amop entry for the given keys.
 */
Oid
get_opclass_member(Oid opclass, Oid subtype, int16 strategy)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	Oid			result;

	tp = SearchSysCache(AMOPSTRATEGY,
						ObjectIdGetDatum(opclass),
						ObjectIdGetDatum(subtype),
						Int16GetDatum(strategy),
						0);
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopopr;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_op_hash_function
 *		Get the OID of the datatype-specific hash function associated with
 *		a hashable equality operator.
 *
 * Returns InvalidOid if no hash function can be found.  (This indicates
 * that the operator should not have been marked oprcanhash.)
 */
Oid
get_op_hash_function(Oid opno)
{
	CatCList   *catlist;
	int			i;
	Oid			opclass = InvalidOid;

	/*
	 * Search pg_amop to see if the target operator is registered as the "="
	 * operator of any hash opclass.  If the operator is registered in
	 * multiple opclasses, assume we can use the associated hash function from
	 * any one.
	 */
	catlist = SearchSysCacheList(AMOPOPID, 1,
								 ObjectIdGetDatum(opno),
								 0, 0, 0);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (aform->amopstrategy == HTEqualStrategyNumber &&
			opclass_is_hash(aform->amopclaid))
		{
			opclass = aform->amopclaid;
			break;
		}
	}

	ReleaseSysCacheList(catlist);

	if (OidIsValid(opclass))
	{
		/* Found a suitable opclass, get its default hash support function */
		return get_opclass_proc(opclass, InvalidOid, HASHPROC);
	}

	/* Didn't find a match... */
	return InvalidOid;
}

/*
 * get_op_btree_interpretation
 *		Given an operator's OID, find out which btree opclasses it belongs to,
 *		and what strategy number it has within each one.  The results are
 *		returned as an OID list and a parallel integer list.
 *
 * In addition to the normal btree operators, we consider a <> operator to be
 * a "member" of an opclass if its negator is the opclass' equality operator.
 * ROWCOMPARE_NE is returned as the strategy number for this case.
 */
void
get_op_btree_interpretation(Oid opno, List **opclasses, List **opstrats)
{
	Oid			lefttype,
				righttype;
	CatCList   *catlist;
	bool		op_negated;
	int			i;

	*opclasses = NIL;
	*opstrats = NIL;

	/*
	 * Get the nominal left-hand input type of the operator; we will ignore
	 * opclasses that don't have that as the expected input datatype.  This is
	 * a kluge to avoid being confused by binary-compatible opclasses (such as
	 * text_ops and varchar_ops, which share the same operators).
	 */
	op_input_types(opno, &lefttype, &righttype);
	Assert(OidIsValid(lefttype));

	/*
	 * Find all the pg_amop entries containing the operator.
	 */
	catlist = SearchSysCacheList(AMOPOPID, 1,
								 ObjectIdGetDatum(opno),
								 0, 0, 0);

	/*
	 * If we can't find any opclass containing the op, perhaps it is a <>
	 * operator.  See if it has a negator that is in an opclass.
	 */
	op_negated = false;
	if (catlist->n_members == 0)
	{
		Oid			op_negator = get_negator(opno);

		if (OidIsValid(op_negator))
		{
			op_negated = true;
			ReleaseSysCacheList(catlist);
			catlist = SearchSysCacheList(AMOPOPID, 1,
										 ObjectIdGetDatum(op_negator),
										 0, 0, 0);
		}
	}

	/* Now search the opclasses */
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	op_tuple = &catlist->members[i]->tuple;
		Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);
		Oid			opclass_id;
		StrategyNumber op_strategy;

		opclass_id = op_form->amopclaid;

		/* must be btree */
		if (!opclass_is_btree(opclass_id))
			continue;

		/* must match operator input type exactly */
		if (get_opclass_input_type(opclass_id) != lefttype)
			continue;

		/* Get the operator's btree strategy number */
		op_strategy = (StrategyNumber) op_form->amopstrategy;
		Assert(op_strategy >= 1 && op_strategy <= 5);

		if (op_negated)
		{
			/* Only consider negators that are = */
			if (op_strategy != BTEqualStrategyNumber)
				continue;
			op_strategy = ROWCOMPARE_NE;
		}

		*opclasses = lappend_oid(*opclasses, opclass_id);
		*opstrats = lappend_int(*opstrats, op_strategy);
	}

	ReleaseSysCacheList(catlist);
}


/*				---------- AMPROC CACHES ----------						 */

/*
 * get_opclass_proc
 *		Get the OID of the specified support function
 *		for the specified opclass and subtype.
 *
 * Returns InvalidOid if there is no pg_amproc entry for the given keys.
 */
Oid
get_opclass_proc(Oid opclass, Oid subtype, int16 procnum)
{
	HeapTuple	tp;
	Form_pg_amproc amproc_tup;
	RegProcedure result;

	tp = SearchSysCache(AMPROCNUM,
						ObjectIdGetDatum(opclass),
						ObjectIdGetDatum(subtype),
						Int16GetDatum(procnum),
						0);
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amproc_tup = (Form_pg_amproc) GETSTRUCT(tp);
	result = amproc_tup->amproc;
	ReleaseSysCache(tp);
	return result;
}


/*				---------- ATTRIBUTE CACHES ----------					 */

/*
 * get_attname
 *		Given the relation id and the attribute number,
 *		return the "attname" field from the attribute relation.
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such attribute.
 */
char *
get_attname(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNUM,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(att_tup->attname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_relid_attribute_name
 *
 * Same as above routine get_attname(), except that error
 * is handled by elog() instead of returning NULL.
 */
char *
get_relid_attribute_name(Oid relid, AttrNumber attnum)
{
	char	   *attname;

	attname = get_attname(relid, attnum);
	if (attname == NULL)
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	return attname;
}

/*
 * get_attnum
 *
 *		Given the relation id and the attribute name,
 *		return the "attnum" field from the attribute relation.
 *
 *		Returns InvalidAttrNumber if the attr doesn't exist (or is dropped).
 */
AttrNumber
get_attnum(Oid relid, const char *attname)
{
	HeapTuple	tp;

	tp = SearchSysCacheAttName(relid, attname);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		AttrNumber	result;

		result = att_tup->attnum;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}

/*
 * get_atttype
 *
 *		Given the relation OID and the attribute number with the relation,
 *		return the attribute type OID.
 */
Oid
get_atttype(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNUM,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		Oid			result;

		result = att_tup->atttypid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_atttypmod
 *
 *		Given the relation id and the attribute number,
 *		return the "atttypmod" field from the attribute relation.
 */
int32
get_atttypmod(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNUM,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		int32		result;

		result = att_tup->atttypmod;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return -1;
}

/*
 * get_atttypetypmod
 *
 *		A two-fer: given the relation id and the attribute number,
 *		fetch both type OID and atttypmod in a single cache lookup.
 *
 * Unlike the otherwise-similar get_atttype/get_atttypmod, this routine
 * raises an error if it can't obtain the information.
 */
void
get_atttypetypmod(Oid relid, AttrNumber attnum,
				  Oid *typid, int32 *typmod)
{
	HeapTuple	tp;
	Form_pg_attribute att_tup;

    /* CDB: Get type for sysattr even if relid is no good (e.g. SubqueryScan) */
    if (attnum < 0 &&
        attnum > FirstLowInvalidHeapAttributeNumber)
    {
        att_tup = SystemAttributeDefinition(attnum, true);
	    *typid = att_tup->atttypid;
	    *typmod = att_tup->atttypmod;
        return;
    }

	tp = SearchSysCache(ATTNUM,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	att_tup = (Form_pg_attribute) GETSTRUCT(tp);

	*typid = att_tup->atttypid;
	*typmod = att_tup->atttypmod;
	ReleaseSysCache(tp);
}

/*				---------- INDEX CACHE ----------						 */

/*		watch this space...
 */

/*				---------- OPCLASS CACHE ----------						 */

/*
 * opclass_is_btree
 *
 *		Returns TRUE iff the specified opclass is associated with the
 *		btree index access method.
 */
bool
opclass_is_btree(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	bool		result;

	tp = SearchSysCache(CLAOID,
						ObjectIdGetDatum(opclass),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = (cla_tup->opcamid == BTREE_AM_OID);
	ReleaseSysCache(tp);
	return result;
}

/*
 * opclass_is_hash
 *
 *		Returns TRUE iff the specified opclass is associated with the
 *		hash index access method.
 */
bool
opclass_is_hash(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	bool		result;

	tp = SearchSysCache(CLAOID,
						ObjectIdGetDatum(opclass),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = (cla_tup->opcamid == HASH_AM_OID);
	ReleaseSysCache(tp);
	return result;
}

/*
 * opclass_is_default
 *
 *		Returns TRUE iff the specified opclass is the default for its
 *		index access method and input data type.
 */
bool
opclass_is_default(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	bool		result;

	tp = SearchSysCache(CLAOID,
						ObjectIdGetDatum(opclass),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = cla_tup->opcdefault;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_opclass_input_type
 *
 *		Returns the OID of the datatype the opclass indexes.
 */
Oid
get_opclass_input_type(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	Oid			result;

	tp = SearchSysCache(CLAOID,
						ObjectIdGetDatum(opclass),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = cla_tup->opcintype;
	ReleaseSysCache(tp);
	return result;
}

/*				---------- OPERATOR CACHE ----------					 */

/*
 * get_opcode
 *
 *		Returns the regproc id of the routine used to implement an
 *		operator given the operator oid.
 */
RegProcedure
get_opcode(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprcode;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_opname
 *	  returns the name of the operator with the given opno
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such operator.
 */
char *
get_opname(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(optup->oprname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * op_input_types
 *
 *		Returns the left and right input datatypes for an operator
 *		(InvalidOid if not relevant).
 */
void
op_input_types(Oid opno, Oid *lefttype, Oid *righttype)
{
	HeapTuple	tp;
	Form_pg_operator optup;
	*lefttype = InvalidOid;
	*righttype = InvalidOid;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		optup = (Form_pg_operator) GETSTRUCT(tp);
		*lefttype = optup->oprleft;
		*righttype = optup->oprright;
		ReleaseSysCache(tp);
	}
}

/*
 * op_mergejoinable
 *
 *		Returns the left and right sort operators corresponding to a
 *		mergejoinable operator, or false if the operator is not mergejoinable.
 */
bool
op_mergejoinable(Oid opno, Oid *leftOp, Oid *rightOp)
{
	HeapTuple	tp;
	bool		result = false;

	if (!sort_op_can_sort(opno, true))
		return result;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

		if (optup->oprlsortop &&
			optup->oprrsortop)
		{
			*leftOp = optup->oprlsortop;
			*rightOp = optup->oprrsortop;
			result = true;
		}
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * op_mergejoin_crossops
 *
 *		Returns the cross-type comparison operators (ltype "<" rtype and
 *		ltype ">" rtype) for an operator previously determined to be
 *		mergejoinable.	Optionally, fetches the regproc ids of these
 *		operators, as well as their operator OIDs.
 */
void
op_mergejoin_crossops(Oid opno, Oid *ltop, Oid *gtop,
					  RegProcedure *ltproc, RegProcedure *gtproc)
{
	HeapTuple	tp;
	Form_pg_operator optup;

	/*
	 * Get the declared comparison operators of the operator.
	 */
	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for operator %u", opno);
	optup = (Form_pg_operator) GETSTRUCT(tp);
	*ltop = optup->oprltcmpop;
	*gtop = optup->oprgtcmpop;
	ReleaseSysCache(tp);

	/* Check < op provided */
	if (!OidIsValid(*ltop))
		elog(ERROR, "mergejoin operator %u has no matching < operator",
			 opno);
	if (ltproc)
		*ltproc = get_opcode(*ltop);

	/* Check > op provided */
	if (!OidIsValid(*gtop))
		elog(ERROR, "mergejoin operator %u has no matching > operator",
			 opno);
	if (gtproc)
		*gtproc = get_opcode(*gtop);
}

/*
 * op_hashjoinable
 *
 * Returns true if the operator is hashjoinable.
 */
bool
op_hashjoinable(Oid opno)
{
	HeapTuple	tp;
	bool		result = false;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

		result = optup->oprcanhash;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * op_strict
 *
 * Get the proisstrict flag for the operator's underlying function.
 */
bool
op_strict(Oid opno)
{
	RegProcedure funcid = get_opcode(opno);

	if (funcid == (RegProcedure) InvalidOid)
		elog(ERROR, "operator %u does not exist", opno);

	return func_strict((Oid) funcid);
}

/*
 * op_volatile
 *
 * Get the provolatile flag for the operator's underlying function.
 */
char
op_volatile(Oid opno)
{
	RegProcedure funcid = get_opcode(opno);

	if (funcid == (RegProcedure) InvalidOid)
		elog(ERROR, "operator %u does not exist", opno);

	return func_volatile((Oid) funcid);
}

/*
 * get_commutator
 *
 *		Returns the corresponding commutator of an operator.
 */
Oid
get_commutator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid			result;

		result = optup->oprcom;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_negator
 *
 *		Returns the corresponding negator of an operator.
 */
Oid
get_negator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid			result;

		result = optup->oprnegate;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_oprrest
 *
 *		Returns procedure id for computing selectivity of an operator.
 */
RegProcedure
get_oprrest(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprrest;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_oprjoin
 *
 *		Returns procedure id for computing selectivity of a join.
 */
RegProcedure
get_oprjoin(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprjoin;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*				---------- FUNCTION CACHE ----------					 */

/*
 * get_func_name
 *	  returns the name of the function with the given funcid
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such function.
 */
char *
get_func_name(Oid funcid)
{
	HeapTuple	tp;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_proc functup = (Form_pg_proc) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(functup->proname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_type_name
 *	  returns the name of the type with the given oid
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such type.
 */
char *
get_type_name(Oid oid)
{
	// get type name
	HeapTuple	tp;
	
	tp = SearchSysCache(TYPEOID, oid, 0, 0, 0);
	
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char *result = pstrdup(NameStr(typtup->typname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_func_rettype
 *		Given procedure id, return the function's result type.
 */
Oid
get_func_rettype(Oid funcid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->prorettype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_agg_transtype
 *		Given aggregate id, return the aggregate transition function's result type.
 */
Oid
get_agg_transtype(Oid aggid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache(AGGFNOID,
						ObjectIdGetDatum(aggid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);

	Form_pg_aggregate aggform = (Form_pg_aggregate) GETSTRUCT(tp);
	result = aggform->aggtranstype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_nargs
 *		Given procedure id, return the number of arguments.
 */
int
get_func_nargs(Oid funcid)
{
	HeapTuple	tp;
	int			result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->pronargs;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_signature
 *		Given procedure id, return the function's argument and result types.
 *		(The return value is the result type.)
 *
 * The arguments are returned as a palloc'd array.
 */
Oid
get_func_signature(Oid funcid, Oid **argtypes, int *nargs)
{
	HeapTuple	tp;
	Form_pg_proc procstruct;
	Oid			result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	procstruct = (Form_pg_proc) GETSTRUCT(tp);

	result = procstruct->prorettype;
	*nargs = (int) procstruct->pronargs;
	Assert(*nargs == procstruct->proargtypes.dim1);
	*argtypes = (Oid *) palloc(*nargs * sizeof(Oid));
	memcpy(*argtypes, procstruct->proargtypes.values, *nargs * sizeof(Oid));

	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_output_arg_types
 *		Given procedure id, return the function's output argument types
 */
List *
get_func_output_arg_types(Oid funcid)
{
	HeapTuple	tp;
	int		numargs;
	Oid		*argtypes = NULL;
	char	**argnames = NULL;
	char	*argmodes = NULL;
	List	*l_argtypes = NULL;
	int		i;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);

	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	numargs = get_func_arg_info(tp, &argtypes, &argnames, &argmodes);

	if (NULL == argmodes)
	{
		Assert(NULL == argnames);
		if (NULL != argtypes)
		{
			pfree(argtypes);
		}
		ReleaseSysCache(tp);
		return NULL;
	}

	for (i = 0; i < numargs; i++)
	{
		Oid argtype = argtypes[i];
		char argmode = argmodes[i];

		if (PROARGMODE_INOUT == argmode || PROARGMODE_OUT == argmode)
		{
			l_argtypes = lappend_oid(l_argtypes, argtype);
		}
		pfree(argnames[i]);
	}

	pfree(argtypes);
	pfree(argnames);
	pfree(argmodes);

	ReleaseSysCache(tp);
	return l_argtypes;
}

/*
 * get_func_retset
 *		Given procedure id, return the function's proretset flag.
 */
bool
get_func_retset(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proretset;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_strict
 *		Given procedure id, return the function's proisstrict flag.
 */
bool
func_strict(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proisstrict;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_volatile
 *		Given procedure id, return the function's provolatile flag.
 */
char
func_volatile(Oid funcid)
{
	HeapTuple	tp;
	char		result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->provolatile;
	ReleaseSysCache(tp);
	return result;
}

/*				---------- RELATION CACHE ----------					 */

/*
 * get_relname_relid
 *		Given name and namespace of a relation, look up the OID.
 *
 * Returns InvalidOid if there is no such relation.
 */
Oid
get_relname_relid(const char *relname, Oid relnamespace)
{
	return GetSysCacheOid(RELNAMENSP,
						  PointerGetDatum((char *) relname),
						  ObjectIdGetDatum(relnamespace),
						  0, 0);
}

#ifdef NOT_USED
/*
 * get_relnatts
 *
 *		Returns the number of attributes for a given relation.
 */
int
get_relnatts(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		int			result;

		result = reltup->relnatts;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}
#endif

/*
 * get_rel_name
 *		Returns the name of a given relation.
 *
 * Returns a palloc'd copy of the string, or NULL if no such relation.
 *
 * NOTE: since relation name is not unique, be wary of code that uses this
 * for anything except preparing error messages.
 */
char *
get_rel_name(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(reltup->relname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}


/*
 * get_rel_name_partition
 *		Returns the name of a given relation plus its parent name, if it is a partition table.
 *		If it not a partition table, it returns the relation name only.
 *
 *	Returns a palloc'd copy of the string, or NULL if no such relation.
 *	The caller is responsible for releasing the palloc'd memory.
 */
char *
get_rel_name_partition(Oid relid)
{
	char *rel_name = get_rel_name(relid);

	if (rel_name == NULL) return NULL;

	if (rel_is_child_partition(relid))
	{
		char *result;

		Oid parent_oid = rel_partition_get_master(relid);
		Assert(parent_oid != InvalidOid);

		char *parent_name = get_rel_name(parent_oid);
		Assert(parent_name);

		char *partition_name = "";

		StringInfo buffer = makeStringInfo();
		Assert(buffer);

		appendStringInfo(buffer, "\"%s\" (partition%s of relation \"%s\")",
							     rel_name, partition_name, parent_name);

		result = pstrdup(buffer->data);

		pfree(rel_name);
		pfree(parent_name);
		pfree(buffer);

		rel_name = NULL;
		parent_name = NULL;
		buffer = NULL;

		return result;
	}
	return rel_name;
}


/*
 * get_rel_namespace
 *
 *		Returns the pg_namespace OID associated with a given relation.
 */
Oid
get_rel_namespace(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->relnamespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_rel_type_id
 *
 *		Returns the pg_type OID associated with a given relation.
 *
 * Note: not all pg_class entries have associated pg_type OIDs; so be
 * careful to check for InvalidOid result.
 */
Oid
get_rel_type_id(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->reltype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_rel_relkind
 *
 *		Returns the relkind associated with a given relation.
 */
char
get_rel_relkind(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char		result;

		result = reltup->relkind;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

/*
 * get_rel_relstorage
 *
 *		Returns the relstorage associated with a given relation.
 */
char
get_rel_relstorage(Oid relid)
{
	HeapTuple	tp;
	
	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char		result;
		
		result = reltup->relstorage;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

/*
 * get_rel_tablespace
 *
 *		Returns the pg_tablespace OID associated with a given relation.
 *
 * Note: InvalidOid might mean either that we couldn't find the relation,
 * or that it is in the database's default tablespace.
 */
Oid
get_rel_tablespace(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->reltablespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}


/*				---------- TYPE CACHE ----------						 */

/*
 * get_typisdefined
 *
 *		Given the type OID, determine whether the type is defined
 *		(if not, it's only a shell).
 */
bool
get_typisdefined(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		bool		result;

		result = typtup->typisdefined;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_typlen
 *
 *		Given the type OID, return the length of the type.
 */
int16
get_typlen(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		int16		result;

		result = typtup->typlen;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 0;
}

/*
 * get_typbyval
 *
 *		Given the type OID, determine whether the type is returned by value or
 *		not.  Returns true if by value, false if by reference.
 */
bool
get_typbyval(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		bool		result;

		result = typtup->typbyval;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_typlenbyval
 *
 *		A two-fer: given the type OID, return both typlen and typbyval.
 *
 *		Since both pieces of info are needed to know how to copy a Datum,
 *		many places need both.	Might as well get them with one cache lookup
 *		instead of two.  Also, this routine raises an error instead of
 *		returning a bogus value when given a bad type OID.
 */
void
get_typlenbyval(Oid typid, int16 *typlen, bool *typbyval)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typlen = typtup->typlen;
	*typbyval = typtup->typbyval;
	ReleaseSysCache(tp);
}

/*
 * get_typlenbyvalalign
 *
 *		A three-fer: given the type OID, return typlen, typbyval, typalign.
 */
void
get_typlenbyvalalign(Oid typid, int16 *typlen, bool *typbyval,
					 char *typalign)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typlen = typtup->typlen;
	*typbyval = typtup->typbyval;
	*typalign = typtup->typalign;
	ReleaseSysCache(tp);
}

/*
 * getTypeIOParam
 *		Given a pg_type row, select the type OID to pass to I/O functions
 *
 * Formerly, all I/O functions were passed pg_type.typelem as their second
 * parameter, but we now have a more complex rule about what to pass.
 * This knowledge is intended to be centralized here --- direct references
 * to typelem elsewhere in the code are wrong, if they are associated with
 * I/O calls and not with actual subscripting operations!  (But see
 * bootstrap.c's boot_get_type_io_data() if you need to change this.)
 *
 * As of PostgreSQL 8.1, output functions receive only the value itself
 * and not any auxiliary parameters, so the name of this routine is now
 * a bit of a misnomer ... it should be getTypeInputParam.
 */
Oid
getTypeIOParam(HeapTuple typeTuple)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * Array types get their typelem as parameter; everybody else gets their
	 * own type OID as parameter.  (As of 8.2, domains must get their own OID
	 * even if their base type is an array.)
	 */
	if (typeStruct->typtype == 'b' && OidIsValid(typeStruct->typelem))
		return typeStruct->typelem;
	else
		return HeapTupleGetOid(typeTuple);
}

/*
 * get_type_io_data
 *
 *		A six-fer:	given the type OID, return typlen, typbyval, typalign,
 *					typdelim, typioparam, and IO function OID. The IO function
 *					returned is controlled by IOFuncSelector
 */
void
get_type_io_data(Oid typid,
				 IOFuncSelector which_func,
				 int16 *typlen,
				 bool *typbyval,
				 char *typalign,
				 char *typdelim,
				 Oid *typioparam,
				 Oid *func)
{
	HeapTuple	typeTuple;
	Form_pg_type typeStruct;

	/*
	 * In bootstrap mode, pass it off to bootstrap.c.  This hack allows us to
	 * use array_in and array_out during bootstrap.
	 */
	if (IsBootstrapProcessingMode())
	{
		Oid			typinput;
		Oid			typoutput;

		boot_get_type_io_data(typid,
							  typlen,
							  typbyval,
							  typalign,
							  typdelim,
							  typioparam,
							  &typinput,
							  &typoutput);
		switch (which_func)
		{
			case IOFunc_input:
				*func = typinput;
				break;
			case IOFunc_output:
				*func = typoutput;
				break;
			default:
				elog(ERROR, "binary I/O not supported during bootstrap");
				break;
		}
		return;
	}

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(typid),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	*typlen = typeStruct->typlen;
	*typbyval = typeStruct->typbyval;
	*typalign = typeStruct->typalign;
	*typdelim = typeStruct->typdelim;
	*typioparam = getTypeIOParam(typeTuple);
	switch (which_func)
	{
		case IOFunc_input:
			*func = typeStruct->typinput;
			break;
		case IOFunc_output:
			*func = typeStruct->typoutput;
			break;
		case IOFunc_receive:
			*func = typeStruct->typreceive;
			break;
		case IOFunc_send:
			*func = typeStruct->typsend;
			break;
	}
	ReleaseSysCache(typeTuple);
}

#ifdef NOT_USED
char
get_typalign(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typalign;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 'i';
}
#endif

char
get_typstorage(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typstorage;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 'p';
}

/*
 * get_typdefault
 *	  Given a type OID, return the type's default value, if any.
 *
 *	  The result is a palloc'd expression node tree, or NULL if there
 *	  is no defined default for the datatype.
 *
 * NB: caller should be prepared to coerce result to correct datatype;
 * the returned expression tree might produce something of the wrong type.
 */
Node *
get_typdefault(Oid typid)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	Datum		datum;
	bool		isNull;
	Node	   *expr;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(typid),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", typid);
	type = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * typdefault and typdefaultbin are potentially null, so don't try to
	 * access 'em as struct fields. Must do it the hard way with
	 * SysCacheGetAttr.
	 */
	datum = SysCacheGetAttr(TYPEOID,
							typeTuple,
							Anum_pg_type_typdefaultbin,
							&isNull);

	if (!isNull)
	{
		/* We have an expression default */
		expr = stringToNode(TextDatumGetCString(datum));
	}
	else
	{
		/* Perhaps we have a plain literal default */
		datum = SysCacheGetAttr(TYPEOID,
								typeTuple,
								Anum_pg_type_typdefault,
								&isNull);

		if (!isNull)
		{
			char	   *strDefaultVal;

			/* Convert text datum to C string */
			strDefaultVal = TextDatumGetCString(datum);
			/* Convert C string to a value of the given type */
			datum = OidInputFunctionCall(type->typinput, strDefaultVal,
										 getTypeIOParam(typeTuple), -1);
			/* Build a Const node containing the value */
			expr = (Node *) makeConst(typid, -1,
									  type->typlen,
									  datum,
									  false,
									  type->typbyval);
			pfree(strDefaultVal);
		}
		else
		{
			/* No default */
			expr = NULL;
		}
	}

	ReleaseSysCache(typeTuple);

	return expr;
}

/*
 * getBaseType
 *		If the given type is a domain, return its base type;
 *		otherwise return the type's own OID.
 */
Oid
getBaseType(Oid typid)
{
	int32		typmod = -1;

	return getBaseTypeAndTypmod(typid, &typmod);
}

/*
 * getBaseTypeAndTypmod
 *		If the given type is a domain, return its base type and typmod;
 *		otherwise return the type's own OID, and leave *typmod unchanged.
 *
 * Note that the "applied typmod" should be -1 for every domain level
 * above the bottommost; therefore, if the passed-in typid is indeed
 * a domain, *typmod should be -1.
 */
Oid
getBaseTypeAndTypmod(Oid typid, int32 *typmod)
{
	/*
	 * We loop to find the bottom base type in a stack of domains.
	 */
	for (;;)
	{
		HeapTuple	tup;
		Form_pg_type typTup;

		tup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(typid),
							 0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typid);
		typTup = (Form_pg_type) GETSTRUCT(tup);
		if (typTup->typtype != 'd')
		{
			/* Not a domain, so done */
			ReleaseSysCache(tup);
			break;
		}

		Assert(*typmod == -1);
		typid = typTup->typbasetype;
		*typmod = typTup->typtypmod;

		ReleaseSysCache(tup);
	}

	return typid;
}

/*
 * get_typavgwidth
 *
 *	  Given a type OID and a typmod value (pass -1 if typmod is unknown),
 *	  estimate the average width of values of the type.  This is used by
 *	  the planner, which doesn't require absolutely correct results;
 *	  it's OK (and expected) to guess if we don't know for sure.
 */
int32
get_typavgwidth(Oid typid, int32 typmod)
{
	int			typlen = get_typlen(typid);
	int32		maxwidth;

	/*
	 * Easy if it's a fixed-width type
	 */
	if (typlen > 0)
		return typlen;

	/*
	 * type_maximum_size knows the encoding of typmod for some datatypes;
	 * don't duplicate that knowledge here.
	 */
	maxwidth = type_maximum_size(typid, typmod);
	if (maxwidth > 0)
	{
		/*
		 * For BPCHAR, the max width is also the only width.  Otherwise we
		 * need to guess about the typical data width given the max. A sliding
		 * scale for percentage of max width seems reasonable.
		 */
		if (typid == BPCHAROID)
			return maxwidth;
		if (maxwidth <= 32)
			return maxwidth;	/* assume full width */
		if (maxwidth < 1000)
			return 32 + (maxwidth - 32) / 2;	/* assume 50% */

		/*
		 * Beyond 1000, assume we're looking at something like
		 * "varchar(10000)" where the limit isn't actually reached often, and
		 * use a fixed estimate.
		 */
		return 32 + (1000 - 32) / 2;
	}

	/*
	 * Ooops, we have no idea ... wild guess time.
	 */
	return 32;
}

/*
 * get_typtype
 *
 *		Given the type OID, find if it is a basic type, a complex type, etc.
 *		It returns the null char if the cache lookup fails...
 */
char
get_typtype(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typtype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

/*
 * type_is_rowtype
 *
 *		Convenience function to determine whether a type OID represents
 *		a "rowtype" type --- either RECORD or a named composite type.
 */
bool
type_is_rowtype(Oid typid)
{
	return (typid == RECORDOID || get_typtype(typid) == 'c');
}

/*
 * get_typ_typrelid
 *
 *		Given the type OID, get the typrelid (InvalidOid if not a complex
 *		type).
 */
Oid
get_typ_typrelid(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typrelid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_element_type
 *
 *		Given the type OID, get the typelem (InvalidOid if not an array type).
 *
 * NB: this only considers varlena arrays to be true arrays; InvalidOid is
 * returned if the input is a fixed-length array type.
 */
Oid
get_element_type(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		if (typtup->typlen == -1)
			result = typtup->typelem;
		else
			result = InvalidOid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_array_type
 *
 *		Given the type OID, get the corresponding array type.
 *		Returns InvalidOid if no array type can be found.
 *
 * NB: this only considers varlena arrays to be true arrays.
 */
Oid
get_array_type(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char	   *array_typename;
		Oid			namespaceId;

		array_typename = makeArrayTypeName(NameStr(typtup->typname));
		namespaceId = typtup->typnamespace;
		ReleaseSysCache(tp);

		tp = SearchSysCache(TYPENAMENSP,
							PointerGetDatum(array_typename),
							ObjectIdGetDatum(namespaceId),
							0, 0);

		pfree(array_typename);

		if (HeapTupleIsValid(tp))
		{
			Oid			result;

			typtup = (Form_pg_type) GETSTRUCT(tp);
			if (typtup->typlen == -1 && typtup->typelem == typid)
				result = HeapTupleGetOid(tp);
			else
				result = InvalidOid;
			ReleaseSysCache(tp);
			return result;
		}
	}
	return InvalidOid;
}

/*
 * get_base_element_type
 *		Given the type OID, get the typelem, looking "through" any domain
 *		to its underlying array type.
 *
 * This is equivalent to get_element_type(getBaseType(typid)), but avoids
 * an extra cache lookup.  Note that it fails to provide any information
 * about the typmod of the array.
 */
Oid
get_base_element_type(Oid typid)
{
	/*
	 * We loop to find the bottom base type in a stack of domains.
	 */
	for (;;)
	{
		HeapTuple	tup;
		Form_pg_type typTup;

		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			break;
		typTup = (Form_pg_type) GETSTRUCT(tup);
		if (typTup->typtype != TYPTYPE_DOMAIN)
		{
			/* Not a domain, so stop descending */
			Oid			result;

			/* This test must match get_element_type */
			if (typTup->typlen == -1)
				result = typTup->typelem;
			else
				result = InvalidOid;
			ReleaseSysCache(tup);
			return result;
		}

		typid = typTup->typbasetype;
		ReleaseSysCache(tup);
	}

	/* Like get_element_type, silently return InvalidOid for bogus input */
	return InvalidOid;
}

/*
 * getTypeInputInfo
 *
 *		Get info needed for converting values of a type to internal form
 */
void
getTypeInputInfo(Oid type, Oid *typInput, Oid *typIOParam)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type)),
								   errOmitLocation(true)));
	if (!OidIsValid(pt->typinput))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no input function available for type %s",
						format_type_be(type)),
								   errOmitLocation(true)));

	*typInput = pt->typinput;
	*typIOParam = getTypeIOParam(typeTuple);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeOutputInfo
 *
 *		Get info needed for printing values of a type
 */
void
getTypeOutputInfo(Oid type, Oid *typOutput, bool *typIsVarlena)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type)),
								   errOmitLocation(true)));
	if (!OidIsValid(pt->typoutput))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no output function available for type %s",
						format_type_be(type)),
								   errOmitLocation(true)));

	*typOutput = pt->typoutput;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeBinaryInputInfo
 *
 *		Get info needed for binary input of values of a type
 */
void
getTypeBinaryInputInfo(Oid type, Oid *typReceive, Oid *typIOParam)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type)),
								   errOmitLocation(true)));
	if (!OidIsValid(pt->typreceive))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no binary input function available for type %s",
						format_type_be(type)),
								   errOmitLocation(true)));

	*typReceive = pt->typreceive;
	*typIOParam = getTypeIOParam(typeTuple);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeBinaryOutputInfo
 *
 *		Get info needed for binary output of values of a type
 */
void
getTypeBinaryOutputInfo(Oid type, Oid *typSend, bool *typIsVarlena)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type)),
								   errOmitLocation(true)));
	if (!OidIsValid(pt->typsend))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no binary output function available for type %s",
						format_type_be(type)),
								   errOmitLocation(true)));

	*typSend = pt->typsend;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);

	ReleaseSysCache(typeTuple);
}


/*				---------- STATISTICS CACHE ----------					 */

/*
 * get_attavgwidth
 *
 *	  Given the table and attribute number of a column, get the average
 *	  width of entries in the column.  Return zero if no data available.
 */
int32
get_attavgwidth(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache(STATRELATT,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		int32		stawidth = ((Form_pg_statistic) GETSTRUCT(tp))->stawidth;

		ReleaseSysCache(tp);
		if (stawidth > 0)
			return stawidth;
	}
	return 0;
}

/*
 * get_attstatsslot
 *
 *		Extract the contents of a "slot" of a pg_statistic tuple.
 *		Returns TRUE if requested slot type was found, else FALSE.
 *
 * Unlike other routines in this file, this takes a pointer to an
 * already-looked-up tuple in the pg_statistic cache.  We do this since
 * most callers will want to extract more than one value from the cache
 * entry, and we don't want to repeat the cache lookup unnecessarily.
 *
 * statstuple: pg_statistics tuple to be examined.
 * atttype: type OID of attribute (can be InvalidOid if values == NULL).
 * atttypmod: typmod of attribute (can be 0 if values == NULL).
 * reqkind: STAKIND code for desired statistics slot kind.
 * reqop: STAOP value wanted, or InvalidOid if don't care.
 * values, nvalues: if not NULL, the slot's stavalues are extracted.
 * numbers, nnumbers: if not NULL, the slot's stanumbers are extracted.
 *
 * If assigned, values and numbers are set to point to palloc'd arrays.
 * If the attribute type is pass-by-reference, the values referenced by
 * the values array are themselves palloc'd.  The palloc'd stuff can be
 * freed by calling free_attstatsslot.
 */
extern bool get_attstatsslot_desc(TupleDesc tupdesc, HeapTuple statstuple,
				 Oid atttype, int32 atttypmod,
				 int reqkind, Oid reqop,
				 Datum **values, int *nvalues,
				 float4 **numbers, int *nnumbers);

bool 
get_attstatsslot(HeapTuple statstuple,
		 Oid atttype, int32 atttypmod,
				 int reqkind, Oid reqop,
				 Datum **values, int *nvalues,
				 float4 **numbers, int *nnumbers)
{
	return get_attstatsslot_desc(NULL, statstuple,
			atttype, atttypmod, reqkind, reqop,
			values, nvalues, numbers, nnumbers
			);
}

bool
get_attstatsslot_desc(TupleDesc tupdesc, HeapTuple statstuple,
				 Oid atttype, int32 atttypmod,
				 int reqkind, Oid reqop,
				 Datum **values, int *nvalues,
				 float4 **numbers, int *nnumbers)
{
	Form_pg_statistic stats = (Form_pg_statistic) GETSTRUCT(statstuple);
	int			i,
				j;
	Datum		val;
	bool		isnull;
	ArrayType  *statarray;
	int			narrayelem;
	HeapTuple	typeTuple;
	Form_pg_type typeForm;

	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		if ((&stats->stakind1)[i] == reqkind &&
			(reqop == InvalidOid || (&stats->staop1)[i] == reqop))
			break;
	}
	if (i >= STATISTIC_NUM_SLOTS)
		return false;			/* not there */

	if (values)
	{
		*values = NULL;
		*nvalues = 0;

		if(tupdesc)
			val = heap_getattr(statstuple, Anum_pg_statistic_stavalues1 + i, 
					tupdesc, &isnull);
		else
			val = SysCacheGetAttr(STATRELATT, statstuple,
							  Anum_pg_statistic_stavalues1 + i,
							  &isnull);
		if (isnull)
			elog(ERROR, "stavalues is null");
		statarray = DatumGetArrayTypeP(val);

		/**
		 * Could be an empty array.
		 */
		if (ARR_NDIM(statarray) > 0)
		{

			/* Need to get info about the array element type */
			typeTuple = SearchSysCache(TYPEOID,
					ObjectIdGetDatum(atttype),
					0, 0, 0);
			if (!HeapTupleIsValid(typeTuple))
				elog(ERROR, "cache lookup failed for type %u", atttype);
			typeForm = (Form_pg_type) GETSTRUCT(typeTuple);

			/* Deconstruct array into Datum elements; NULLs not expected */
			deconstruct_array(statarray,
					atttype,
					typeForm->typlen,
					typeForm->typbyval,
					typeForm->typalign,
					values, NULL, nvalues);

			/*
			 * If the element type is pass-by-reference, we now have a bunch of
			 * Datums that are pointers into the syscache value.  Copy them to
			 * avoid problems if syscache decides to drop the entry.
			 */
			if (!typeForm->typbyval)
			{
				for (j = 0; j < *nvalues; j++)
				{
					(*values)[j] = datumCopy((*values)[j],
							typeForm->typbyval,
							typeForm->typlen);
				}
			}

			ReleaseSysCache(typeTuple);
		}
		/*
		 * Free statarray if it's a detoasted copy.
		 */
		if ((Pointer) statarray != DatumGetPointer(val))
			pfree(statarray);
	}

	if (numbers)
	{
		*numbers = NULL;
		*nnumbers = 0;

		if(tupdesc)
			val = heap_getattr(statstuple, Anum_pg_statistic_stanumbers1 + i, 
					tupdesc, &isnull);
		else
			val = SysCacheGetAttr(STATRELATT, statstuple,
							  Anum_pg_statistic_stanumbers1 + i,
							  &isnull);

		if (isnull)
			elog(ERROR, "stanumbers is null");
		statarray = DatumGetArrayTypeP(val);

		/**
		 * Could be an empty array.
		 */
		if (ARR_NDIM(statarray) > 0)
		{
			/*
			 * We expect the array to be a 1-D float4 array; verify that. We don't
			 * need to use deconstruct_array() since the array data is just going
			 * to look like a C array of float4 values.
			 */
			narrayelem = ARR_DIMS(statarray)[0];
			if (ARR_NDIM(statarray) != 1 || narrayelem <= 0 ||
					ARR_HASNULL(statarray) ||
					ARR_ELEMTYPE(statarray) != FLOAT4OID)
				elog(ERROR, "stanumbers is not a 1-D float4 array");
			*numbers = (float4 *) palloc(narrayelem * sizeof(float4));
			*nnumbers = narrayelem;
			memcpy(*numbers, ARR_DATA_PTR(statarray), narrayelem * sizeof(float4));
		}

		/*
		 * Free statarray if it's a detoasted copy.
		 */
		if ((Pointer) statarray != DatumGetPointer(val))
			pfree(statarray);
	}

	return true;
}

/*
 * free_attstatsslot
 *		Free data allocated by get_attstatsslot
 *
 * atttype need be valid only if values != NULL.
 */
void
free_attstatsslot(Oid atttype,
				  Datum *values, int nvalues,
				  float4 *numbers, int nnumbers)
{
	if (values)
	{
		if (!get_typbyval(atttype))
		{
			int			i;

			for (i = 0; i < nvalues; i++)
				pfree(DatumGetPointer(values[i]));
		}
		pfree(values);
	}
	if (numbers)
		pfree(numbers);
}

/*
 * get_att_stats
 *		Get attribute statistics. Return a copy of the HeapTuple object, or NULL
 *		if no stats found for attribute
 * 
 */
HeapTuple
get_att_stats(Oid relid, AttrNumber attrnum)
{
	return SearchSysCacheCopy(STATRELATT,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attrnum),
						0, 0);
}

/*				---------- PG_NAMESPACE CACHE ----------				 */

/*
 * get_namespace_name
 *		Returns the name of a given namespace
 *
 * Returns a palloc'd copy of the string, or NULL if no such namespace.
 */
char *
get_namespace_name(Oid nspid)
{
	HeapTuple	tp;

	tp = SearchSysCache(NAMESPACEOID,
						ObjectIdGetDatum(nspid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_namespace nsptup = (Form_pg_namespace) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(nsptup->nspname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*				---------- PG_AUTHID CACHE ----------					 */

/*
 * get_roleid
 *	  Given a role name, look up the role's OID.
 *	  Returns InvalidOid if no such role.
 */
Oid
get_roleid(const char *rolname)
{
	return GetSysCacheOid(AUTHNAME,
						  PointerGetDatum((char *) rolname),
						  0, 0, 0);
}

/*
 * get_roleid_checked
 *	  Given a role name, look up the role's OID.
 *	  ereports if no such role.
 */
Oid
get_roleid_checked(const char *rolname)
{
	Oid			roleid;

	roleid = get_roleid(rolname);
	if (!OidIsValid(roleid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", rolname),
						   errOmitLocation(true)));
	return roleid;
}


/*
 * relation_oids
 *	  Extract all relation oids from the catalog.
 */
List *
relation_oids()
{
	List			*relationOids = NIL;
	Relation		pgclass = NULL;
	HeapScanDesc 	scan = NULL;
	HeapTuple		tuple = NULL;

	pgclass = heap_open(RelationRelationId, AccessShareLock);

	scan = heap_beginscan(pgclass, SnapshotNow, 0 /* key length */, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class pgclassEntry = (Form_pg_class) GETSTRUCT(tuple);

		switch (pgclassEntry->relstorage)
		{
			case RELSTORAGE_HEAP:
			case RELSTORAGE_AOCOLS:
			case RELSTORAGE_AOROWS:
			case RELSTORAGE_EXTERNAL:
			{
				Oid relOid = HeapTupleGetOid(tuple);
				relationOids = lappend_oid(relationOids, relOid);
				break;
			}
			default:
				break;
		}
	}

	heap_endscan(scan);
	heap_close(pgclass, AccessShareLock);
	return relationOids;
}

/*
 * operator_oids
 *	  Extract all operator oids from the catalog.
 */
List *
operator_oids()
{
	List			*operatorOids = NIL;
	Relation		operatortable = NULL;
	HeapScanDesc 	scan = NULL;
	HeapTuple		tuple = NULL;

	operatortable = heap_open(OperatorRelationId, AccessShareLock);

	scan = heap_beginscan(operatortable, SnapshotNow, 0 /* key length */, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid opOid = HeapTupleGetOid(tuple);
		operatorOids = lappend_oid(operatorOids, opOid);
	}

	heap_endscan(scan);
	heap_close(operatortable, AccessShareLock);
	return operatorOids;
}

/*
 * function_oids
 *	  Extract all function oids from the catalog.
 */
List *
function_oids()
{
	List			*functionOids = NIL;
	Relation		functiontable = NULL;
	HeapScanDesc 	scan = NULL;
	HeapTuple		tuple = NULL;

	functiontable = heap_open(ProcedureRelationId, AccessShareLock);

	scan = heap_beginscan(functiontable, SnapshotNow, 0 /* key length */, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid funcOid = HeapTupleGetOid(tuple);
		functionOids = lappend_oid(functionOids, funcOid);
	}

	heap_endscan(scan);
	heap_close(functiontable, AccessShareLock);
	return functionOids;
}

/*
 * relation_exists
 *	  Is there a relation with the given oid
 */
bool
relation_exists(Oid oid)
{
	return SearchSysCacheExists(RELOID, oid, 0, 0, 0);
}

/*
 * index_exists
 *	  Is there an index with the given oid
 */
bool
index_exists(Oid oid)
{
	return SearchSysCacheExists(INDEXRELID, oid, 0, 0, 0);
}

/*
 * type_exists
 *	  Is there a type with the given oid
 */
bool
type_exists(Oid oid)
{
	return SearchSysCacheExists(TYPEOID, oid, 0, 0, 0);
}

/*
 * operator_exists
 *	  Is there an operator with the given oid
 */
bool
operator_exists(Oid oid)
{
	return SearchSysCacheExists(OPEROID, oid, 0, 0, 0);
}

/*
 * function_exists
 *	  Is there a function with the given oid
 */
bool
function_exists(Oid oid)
{
	return SearchSysCacheExists(PROCOID, oid, 0, 0, 0);
}

/*
 * aggregate_exists
 *	  Is there an aggregate with the given oid
 */
bool
aggregate_exists(Oid oid)
{
	return SearchSysCacheExists(AGGFNOID, oid, 0, 0, 0);
}

/*
 * get_relation_keys
 *	  Return a list of relation keys
 */
List *
get_relation_keys(Oid relid)
{
	List *keys = NIL;

	// lookup unique constraints for relation from the catalog table
	ScanKeyData skey[1];
	ScanKeyInit(&skey[0], Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, relid);

	Relation rel = heap_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan
						(
						rel, 
						ConstraintRelidIndexId, 
						true, 
						SnapshotNow, 
						1, 
						skey
						);
	
	HeapTuple	htup = NULL;

	while (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		Form_pg_constraint contuple = (Form_pg_constraint) GETSTRUCT(htup);

		// skip non-unique constraints
		if (CONSTRAINT_UNIQUE != contuple->contype &&
			CONSTRAINT_PRIMARY != contuple->contype)
		{
			continue;
		}
			
		// store key set in an array
		List *key = NIL;
		
		bool null = false;
		Datum dat = 
				heap_getattr(htup, Anum_pg_constraint_conkey, RelationGetDescr(rel), &null);
		
		Datum *dats = NULL;
		int numKeys = 0;

		// extract key elements
		deconstruct_array(DatumGetArrayTypeP(dat), INT2OID, 2, true, 's', &dats, NULL, &numKeys);
			
		for (int i = 0; i < numKeys; i++)
		{
			int16 key_elem =  DatumGetInt16(dats[i]);
			key = lappend_int(key, key_elem);
		}
		
		keys = lappend(keys, key);
	}

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);

	return keys;
}
