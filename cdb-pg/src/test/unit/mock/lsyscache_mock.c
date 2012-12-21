/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/lsyscache.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/hash.h"
#include "access/genam.h"
#include "bootstrap/bootstrap.h"
#include "catalog/heap.h"	/* SystemAttributeDefinition() */
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
#include "parser/parse_clause.h"/* for sort_op_can_sort() */
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"
#include "funcapi.h"

bool 
op_in_opclass(Oid opno, Oid opclass)
{

    check_expected(opno);
    check_expected(opclass);

    return (bool) mock();

}

int 
get_op_opclass_strategy(Oid opno, Oid opclass)
{

    check_expected(opno);
    check_expected(opclass);

    return (int) mock();

}

void 
get_op_opclass_properties(Oid opno, Oid opclass, int *strategy, Oid * subtype, bool * recheck)
{

    check_expected(opno);
    check_expected(opclass);
    check_expected(strategy);
    check_expected(subtype);
    check_expected(recheck);
    optional_assignment(strategy);
    optional_assignment(subtype);
    optional_assignment(recheck);
    mock();

}

Oid 
get_opclass_member(Oid opclass, Oid subtype, int16 strategy)
{

    check_expected(opclass);
    check_expected(subtype);
    check_expected(strategy);

    return (Oid) mock();

}

Oid 
get_op_hash_function(Oid opno)
{

    check_expected(opno);

    return (Oid) mock();

}

void 
get_op_btree_interpretation(Oid opno, List ** opclasses, List ** opstrats)
{

    check_expected(opno);
    check_expected(opclasses);
    check_expected(opstrats);
    optional_assignment(opclasses);
    optional_assignment(opstrats);
    mock();

}

Oid 
get_opclass_proc(Oid opclass, Oid subtype, int16 procnum)
{

    check_expected(opclass);
    check_expected(subtype);
    check_expected(procnum);

    return (Oid) mock();

}

char           *
get_attname(Oid relid, AttrNumber attnum)
{

    check_expected(relid);
    check_expected(attnum);

    return (char *) mock();

}

char           *
get_relid_attribute_name(Oid relid, AttrNumber attnum)
{

    check_expected(relid);
    check_expected(attnum);

    return (char *) mock();

}

AttrNumber 
get_attnum(Oid relid, const char *attname)
{

    check_expected(relid);
    check_expected(attname);

    return (AttrNumber) mock();

}

Oid 
get_atttype(Oid relid, AttrNumber attnum)
{

    check_expected(relid);
    check_expected(attnum);

    return (Oid) mock();

}

int32 
get_atttypmod(Oid relid, AttrNumber attnum)
{

    check_expected(relid);
    check_expected(attnum);

    return (int32) mock();

}

void 
get_atttypetypmod(Oid relid, AttrNumber attnum, Oid * typid, int32 * typmod)
{

    check_expected(relid);
    check_expected(attnum);
    check_expected(typid);
    check_expected(typmod);
    optional_assignment(typid);
    optional_assignment(typmod);
    mock();

}

bool 
opclass_is_btree(Oid opclass)
{

    check_expected(opclass);

    return (bool) mock();

}

bool 
opclass_is_hash(Oid opclass)
{

    check_expected(opclass);

    return (bool) mock();

}

bool 
opclass_is_default(Oid opclass)
{

    check_expected(opclass);

    return (bool) mock();

}

Oid 
get_opclass_input_type(Oid opclass)
{

    check_expected(opclass);

    return (Oid) mock();

}

RegProcedure 
get_opcode(Oid opno)
{

    check_expected(opno);

    return (RegProcedure) mock();

}

char           *
get_opname(Oid opno)
{

    check_expected(opno);

    return (char *) mock();

}

void 
op_input_types(Oid opno, Oid * lefttype, Oid * righttype)
{

    check_expected(opno);
    check_expected(lefttype);
    check_expected(righttype);
    optional_assignment(lefttype);
    optional_assignment(righttype);
    mock();

}

bool 
op_mergejoinable(Oid opno, Oid * leftOp, Oid * rightOp)
{

    check_expected(opno);
    check_expected(leftOp);
    check_expected(rightOp);
    optional_assignment(leftOp);
    optional_assignment(rightOp);
    return (bool) mock();

}

void 
op_mergejoin_crossops(Oid opno, Oid * ltop, Oid * gtop, RegProcedure * ltproc, RegProcedure * gtproc)
{

    check_expected(opno);
    check_expected(ltop);
    check_expected(gtop);
    check_expected(ltproc);
    check_expected(gtproc);
    optional_assignment(ltop);
    optional_assignment(gtop);
    optional_assignment(ltproc);
    optional_assignment(gtproc);
    mock();

}

bool 
op_hashjoinable(Oid opno)
{

    check_expected(opno);

    return (bool) mock();

}

bool 
op_strict(Oid opno)
{

    check_expected(opno);

    return (bool) mock();

}

char 
op_volatile(Oid opno)
{

    check_expected(opno);

    return (char) mock();

}

Oid 
get_commutator(Oid opno)
{

    check_expected(opno);

    return (Oid) mock();

}

Oid 
get_negator(Oid opno)
{

    check_expected(opno);

    return (Oid) mock();

}

RegProcedure 
get_oprrest(Oid opno)
{

    check_expected(opno);

    return (RegProcedure) mock();

}

RegProcedure 
get_oprjoin(Oid opno)
{

    check_expected(opno);

    return (RegProcedure) mock();

}

char           *
get_func_name(Oid funcid)
{

    check_expected(funcid);

    return (char *) mock();

}

Oid 
get_func_rettype(Oid funcid)
{

    check_expected(funcid);

    return (Oid) mock();

}

List           *
get_func_output_arg_types(Oid funcid)
{

    check_expected(funcid);

    return (List *) mock();

}

int 
get_func_nargs(Oid funcid)
{

    check_expected(funcid);

    return (int) mock();

}

Oid 
get_func_signature(Oid funcid, Oid ** argtypes, int *nargs)
{

    check_expected(funcid);
    check_expected(argtypes);
    check_expected(nargs);
    optional_assignment(argtypes);
    optional_assignment(nargs);
    return (Oid) mock();

}

bool 
get_func_retset(Oid funcid)
{

    check_expected(funcid);

    return (bool) mock();

}

bool 
func_strict(Oid funcid)
{

    check_expected(funcid);

    return (bool) mock();

}

char 
func_volatile(Oid funcid)
{

    check_expected(funcid);

    return (char) mock();

}

Oid 
get_agg_transtype(Oid aggid)
{

    check_expected(aggid);

    return (Oid) mock();

}

Oid 
get_relname_relid(const char *relname, Oid relnamespace)
{

    check_expected(relname);
    check_expected(relnamespace);

    return (Oid) mock();

}

char           *
get_rel_name(Oid relid)
{

    check_expected(relid);

    return (char *) mock();

}

char           *
get_rel_name_partition(Oid relid)
{

    check_expected(relid);

    return (char *) mock();

}

Oid 
get_rel_namespace(Oid relid)
{

    check_expected(relid);

    return (Oid) mock();

}

Oid 
get_rel_type_id(Oid relid)
{

    check_expected(relid);

    return (Oid) mock();

}

char 
get_rel_relkind(Oid relid)
{

    check_expected(relid);

    return (char) mock();

}

char 
get_rel_relstorage(Oid relid)
{

    check_expected(relid);

    return (char) mock();

}

Oid 
get_rel_tablespace(Oid relid)
{

    check_expected(relid);

    return (Oid) mock();

}

char           *
get_type_name(Oid typid)
{

    check_expected(typid);

    return (char *) mock();

}

bool 
get_typisdefined(Oid typid)
{

    check_expected(typid);

    return (bool) mock();

}

int16 
get_typlen(Oid typid)
{

    check_expected(typid);

    return (int16) mock();

}

bool 
get_typbyval(Oid typid)
{

    check_expected(typid);

    return (bool) mock();

}

void 
get_typlenbyval(Oid typid, int16 * typlen, bool * typbyval)
{

    check_expected(typid);
    check_expected(typlen);
    check_expected(typbyval);
    optional_assignment(typlen);
    optional_assignment(typbyval);
    mock();

}

void 
get_typlenbyvalalign(Oid typid, int16 * typlen, bool * typbyval, char *typalign)
{

    check_expected(typid);
    check_expected(typlen);
    check_expected(typbyval);
    check_expected(typalign);
    optional_assignment(typlen);
    optional_assignment(typbyval);
    optional_assignment(typalign);
    mock();

}

Oid 
getTypeIOParam(HeapTuple typeTuple)
{

    check_expected(typeTuple);

    return (Oid) mock();

}

void 
get_type_io_data(Oid typid, IOFuncSelector which_func, int16 * typlen, bool * typbyval, char *typalign, char *typdelim, Oid * typioparam, Oid * func)
{

    check_expected(typid);
    check_expected(which_func);
    check_expected(typlen);
    check_expected(typbyval);
    check_expected(typalign);
    check_expected(typdelim);
    check_expected(typioparam);
    check_expected(func);
    optional_assignment(typlen);
    optional_assignment(typbyval);
    optional_assignment(typalign);
    optional_assignment(typdelim);
    optional_assignment(typioparam);
    optional_assignment(func);
    mock();

}

char 
get_typstorage(Oid typid)
{

    check_expected(typid);

    return (char) mock();

}

Node           *
get_typdefault(Oid typid)
{

    check_expected(typid);

    return (Node *) mock();

}

char 
get_typtype(Oid typid)
{

    check_expected(typid);

    return (char) mock();

}

bool 
type_is_rowtype(Oid typid)
{

    check_expected(typid);

    return (bool) mock();

}

Oid 
get_typ_typrelid(Oid typid)
{

    check_expected(typid);

    return (Oid) mock();

}

Oid 
get_element_type(Oid typid)
{

    check_expected(typid);

    return (Oid) mock();

}

Oid 
get_array_type(Oid typid)
{

    check_expected(typid);

    return (Oid) mock();

}

Oid 
get_base_element_type(Oid typid)
{

    check_expected(typid);

    return (Oid) mock();

}

void 
getTypeInputInfo(Oid type, Oid * typInput, Oid * typIOParam)
{

    check_expected(type);
    check_expected(typInput);
    check_expected(typIOParam);
    optional_assignment(typInput);
    optional_assignment(typIOParam);
    mock();

}

void 
getTypeOutputInfo(Oid type, Oid * typOutput, bool * typIsVarlena)
{

    check_expected(type);
    check_expected(typOutput);
    check_expected(typIsVarlena);
    optional_assignment(typOutput);
    optional_assignment(typIsVarlena);
    mock();

}

void 
getTypeBinaryInputInfo(Oid type, Oid * typReceive, Oid * typIOParam)
{

    check_expected(type);
    check_expected(typReceive);
    check_expected(typIOParam);
    optional_assignment(typReceive);
    optional_assignment(typIOParam);
    mock();

}

void 
getTypeBinaryOutputInfo(Oid type, Oid * typSend, bool * typIsVarlena)
{

    check_expected(type);
    check_expected(typSend);
    check_expected(typIsVarlena);
    optional_assignment(typSend);
    optional_assignment(typIsVarlena);
    mock();

}

Oid 
getBaseType(Oid typid)
{

    check_expected(typid);

    return (Oid) mock();

}

Oid 
getBaseTypeAndTypmod(Oid typid, int32 * typmod)
{

    check_expected(typid);
    check_expected(typmod);
    optional_assignment(typmod);
    return (Oid) mock();

}

int32 
get_typavgwidth(Oid typid, int32 typmod)
{

    check_expected(typid);
    check_expected(typmod);

    return (int32) mock();

}

int32 
get_attavgwidth(Oid relid, AttrNumber attnum)
{

    check_expected(relid);
    check_expected(attnum);

    return (int32) mock();

}

HeapTuple 
get_att_stats(Oid relid, AttrNumber attnum)
{

    check_expected(relid);
    check_expected(attnum);

    return (HeapTuple) mock();

}

bool 
get_attstatsslot(HeapTuple statstuple, Oid atttype, int32 atttypmod, int reqkind, Oid reqop, Datum ** values, int *nvalues, float4 ** numbers, int *nnumbers)
{

    check_expected(statstuple);
    check_expected(atttype);
    check_expected(atttypmod);
    check_expected(reqkind);
    check_expected(reqop);
    check_expected(values);
    check_expected(nvalues);
    check_expected(numbers);
    check_expected(nnumbers);
    optional_assignment(values);
    optional_assignment(nvalues);
    optional_assignment(numbers);
    optional_assignment(nnumbers);
    return (bool) mock();

}

void 
free_attstatsslot(Oid atttype, Datum * values, int nvalues, float4 * numbers, int nnumbers)
{

    check_expected(atttype);
    check_expected(values);
    check_expected(nvalues);
    check_expected(numbers);
    check_expected(nnumbers);
    optional_assignment(values);
    optional_assignment(numbers);
    mock();

}

char           *
get_namespace_name(Oid nspid)
{

    check_expected(nspid);

    return (char *) mock();

}

Oid 
get_roleid(const char *rolname)
{

    check_expected(rolname);

    return (Oid) mock();

}

Oid 
get_roleid_checked(const char *rolname)
{

    check_expected(rolname);

    return (Oid) mock();

}

List           *
relation_oids(void)
{



    return (List *) mock();

}

List           *
operator_oids(void)
{



    return (List *) mock();

}

List           *
function_oids(void)
{



    return (List *) mock();

}

bool 
relation_exists(Oid oid)
{

    check_expected(oid);

    return (bool) mock();

}

bool 
index_exists(Oid oid)
{

    check_expected(oid);

    return (bool) mock();

}

bool 
type_exists(Oid oid)
{

    check_expected(oid);

    return (bool) mock();

}

bool 
function_exists(Oid oid)
{

    check_expected(oid);

    return (bool) mock();

}

bool 
operator_exists(Oid oid)
{

    check_expected(oid);

    return (bool) mock();

}

bool 
aggregate_exists(Oid oid)
{

    check_expected(oid);

    return (bool) mock();

}

List           *
get_relation_keys(Oid relid)
{

    check_expected(relid);

    return (List *) mock();

}
