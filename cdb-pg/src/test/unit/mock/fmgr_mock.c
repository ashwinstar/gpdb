/*
 * Auto-generated Mocking Source
 * 
 * Source: fmgr.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/tuptoaster.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "executor/functions.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/fmgrtab.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "cdb/cdbvars.h"

void 
fmgr_info(Oid functionId, FmgrInfo * finfo)
{

    check_expected(functionId);
    check_expected(finfo);
    optional_assignment(finfo);
    mock();

}

void 
fmgr_info_cxt(Oid functionId, FmgrInfo * finfo, MemoryContext mcxt)
{

    check_expected(functionId);
    check_expected(finfo);
    check_expected(mcxt);
    optional_assignment(finfo);
    mock();

}

void 
fmgr_info_copy(FmgrInfo * dstinfo, FmgrInfo * srcinfo, MemoryContext destcxt)
{

    check_expected(dstinfo);
    check_expected(srcinfo);
    check_expected(destcxt);
    optional_assignment(dstinfo);
    optional_assignment(srcinfo);
    mock();

}

struct varlena *
pg_detoast_datum(struct varlena * datum)
{

    check_expected(datum);
    optional_assignment(datum);
    mock();

}

struct varlena *
pg_detoast_datum_copy(struct varlena * datum)
{

    check_expected(datum);
    optional_assignment(datum);
    mock();

}

struct varlena *
pg_detoast_datum_slice(struct varlena * datum, int32 first, int32 count)
{

    check_expected(datum);
    check_expected(first);
    check_expected(count);
    optional_assignment(datum);
    mock();

}

struct varlena *
pg_detoast_datum_packed(struct varlena * datum)
{

    check_expected(datum);
    optional_assignment(datum);
    mock();

}

Datum 
DirectFunctionCall1(PGFunction func, Datum arg1)
{

    check_expected(func);
    check_expected(arg1);

    return (Datum) mock();

}

Datum 
DirectFunctionCall2(PGFunction func, Datum arg1, Datum arg2)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);

    return (Datum) mock();

}

Datum 
DirectFunctionCall3(PGFunction func, Datum arg1, Datum arg2, Datum arg3)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);

    return (Datum) mock();

}

Datum 
DirectFunctionCall4(PGFunction func, Datum arg1, Datum arg2, Datum arg3, Datum arg4)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);

    return (Datum) mock();

}

Datum 
DirectFunctionCall5(PGFunction func, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);

    return (Datum) mock();

}

Datum 
DirectFunctionCall6(PGFunction func, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);

    return (Datum) mock();

}

Datum 
DirectFunctionCall7(PGFunction func, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);

    return (Datum) mock();

}

Datum 
DirectFunctionCall8(PGFunction func, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7, Datum arg8)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);
    check_expected(arg8);

    return (Datum) mock();

}

Datum 
DirectFunctionCall9(PGFunction func, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7, Datum arg8, Datum arg9)
{

    check_expected(func);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);
    check_expected(arg8);
    check_expected(arg9);

    return (Datum) mock();

}

Datum 
FunctionCall1(FmgrInfo * flinfo, Datum arg1)
{

    check_expected(flinfo);
    check_expected(arg1);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall2(FmgrInfo * flinfo, Datum arg1, Datum arg2)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall3(FmgrInfo * flinfo, Datum arg1, Datum arg2, Datum arg3)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall4(FmgrInfo * flinfo, Datum arg1, Datum arg2, Datum arg3, Datum arg4)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall5(FmgrInfo * flinfo, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall6(FmgrInfo * flinfo, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall7(FmgrInfo * flinfo, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall8(FmgrInfo * flinfo, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7, Datum arg8)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);
    check_expected(arg8);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
FunctionCall9(FmgrInfo * flinfo, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7, Datum arg8, Datum arg9)
{

    check_expected(flinfo);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);
    check_expected(arg8);
    check_expected(arg9);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
OidFunctionCall1(Oid functionId, Datum arg1)
{

    check_expected(functionId);
    check_expected(arg1);

    return (Datum) mock();

}

Datum 
OidFunctionCall2(Oid functionId, Datum arg1, Datum arg2)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);

    return (Datum) mock();

}

Datum 
OidFunctionCall3(Oid functionId, Datum arg1, Datum arg2, Datum arg3)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);

    return (Datum) mock();

}

Datum 
OidFunctionCall4(Oid functionId, Datum arg1, Datum arg2, Datum arg3, Datum arg4)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);

    return (Datum) mock();

}

Datum 
OidFunctionCall5(Oid functionId, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);

    return (Datum) mock();

}

Datum 
OidFunctionCall6(Oid functionId, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);

    return (Datum) mock();

}

Datum 
OidFunctionCall7(Oid functionId, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);

    return (Datum) mock();

}

Datum 
OidFunctionCall8(Oid functionId, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7, Datum arg8)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);
    check_expected(arg8);

    return (Datum) mock();

}

Datum 
OidFunctionCall9(Oid functionId, Datum arg1, Datum arg2, Datum arg3, Datum arg4, Datum arg5, Datum arg6, Datum arg7, Datum arg8, Datum arg9)
{

    check_expected(functionId);
    check_expected(arg1);
    check_expected(arg2);
    check_expected(arg3);
    check_expected(arg4);
    check_expected(arg5);
    check_expected(arg6);
    check_expected(arg7);
    check_expected(arg8);
    check_expected(arg9);

    return (Datum) mock();

}

Datum 
InputFunctionCall(FmgrInfo * flinfo, char *str, Oid typioparam, int32 typmod)
{

    check_expected(flinfo);
    check_expected(str);
    check_expected(typioparam);
    check_expected(typmod);
    optional_assignment(flinfo);
    optional_assignment(str);
    return (Datum) mock();

}

Datum 
OidInputFunctionCall(Oid functionId, char *str, Oid typioparam, int32 typmod)
{

    check_expected(functionId);
    check_expected(str);
    check_expected(typioparam);
    check_expected(typmod);
    optional_assignment(str);
    return (Datum) mock();

}

char           *
OutputFunctionCall(FmgrInfo * flinfo, Datum val)
{

    check_expected(flinfo);
    check_expected(val);
    optional_assignment(flinfo);
    return (char *) mock();

}

char           *
OidOutputFunctionCall(Oid functionId, Datum val)
{

    check_expected(functionId);
    check_expected(val);

    return (char *) mock();

}

Datum 
ReceiveFunctionCall(FmgrInfo * flinfo, fmStringInfo buf, Oid typioparam, int32 typmod)
{

    check_expected(flinfo);
    check_expected(buf);
    check_expected(typioparam);
    check_expected(typmod);
    optional_assignment(flinfo);
    return (Datum) mock();

}

Datum 
OidReceiveFunctionCall(Oid functionId, fmStringInfo buf, Oid typioparam, int32 typmod)
{

    check_expected(functionId);
    check_expected(buf);
    check_expected(typioparam);
    check_expected(typmod);

    return (Datum) mock();

}

bytea          *
SendFunctionCall(FmgrInfo * flinfo, Datum val)
{

    check_expected(flinfo);
    check_expected(val);
    optional_assignment(flinfo);
    return (bytea *) mock();

}

bytea          *
OidSendFunctionCall(Oid functionId, Datum val)
{

    check_expected(functionId);
    check_expected(val);

    return (bytea *) mock();

}

const Pg_finfo_record *
fetch_finfo_record(void *filehandle, char *funcname)
{

    check_expected(filehandle);
    check_expected(funcname);
    optional_assignment(filehandle);
    optional_assignment(funcname);
    return (Pg_finfo_record *) mock();

}

void 
clear_external_function_hash(void *filehandle)
{

    check_expected(filehandle);
    optional_assignment(filehandle);
    mock();

}

Oid 
fmgr_internal_function(const char *proname)
{

    check_expected(proname);

    return (Oid) mock();

}

Oid 
get_fn_expr_rettype(FmgrInfo * flinfo)
{

    check_expected(flinfo);
    optional_assignment(flinfo);
    return (Oid) mock();

}

Oid 
get_fn_expr_argtype(FmgrInfo * flinfo, int argnum)
{

    check_expected(flinfo);
    check_expected(argnum);
    optional_assignment(flinfo);
    return (Oid) mock();

}

Oid 
get_call_expr_argtype(fmNodePtr expr, int argnum)
{

    check_expected(expr);
    check_expected(argnum);

    return (Oid) mock();

}

char           *Dynamic_library_path;
PGFunction 
load_external_function(char *filename, char *funcname, bool signalNotFound, void **filehandle)
{

    check_expected(filename);
    check_expected(funcname);
    check_expected(signalNotFound);
    check_expected(filehandle);
    optional_assignment(filename);
    optional_assignment(funcname);
    optional_assignment(filehandle);
    return (PGFunction) mock();

}

PGFunction 
lookup_external_function(void *filehandle, char *funcname)
{

    check_expected(filehandle);
    check_expected(funcname);
    optional_assignment(filehandle);
    optional_assignment(funcname);
    return (PGFunction) mock();

}

void 
load_file(const char *filename, bool restricted)
{

    check_expected(filename);
    check_expected(restricted);

    mock();

}

void          **
find_rendezvous_variable(const char *varName)
{

    check_expected(varName);

    mock();

}
