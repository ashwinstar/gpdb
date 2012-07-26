/*
 * Auto-generated Mocking Source
 * 
 * Source: catalog/namespace.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/genam.h"
#include "access/catquery.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/schemacmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "cdb/cdbvars.h"
#include "tcop/utility.h"
#include "cdb/cdbtm.h"

Oid 
RangeVarGetRelid(const RangeVar * relation, bool failOK)
{

    check_expected(relation);
    check_expected(failOK);

    return (Oid) mock();

}

Oid 
RangeVarGetCreationNamespace(const RangeVar * newRelation)
{

    check_expected(newRelation);

    return (Oid) mock();

}

Oid 
RelnameGetRelid(const char *relname)
{

    check_expected(relname);

    return (Oid) mock();

}

bool 
RelationIsVisible(Oid relid)
{

    check_expected(relid);

    return (bool) mock();

}

Oid 
TypenameGetTypid(const char *typname)
{

    check_expected(typname);

    return (Oid) mock();

}

bool 
TypeIsVisible(Oid typid)
{

    check_expected(typid);

    return (bool) mock();

}

FuncCandidateList 
FuncnameGetCandidates(List * names, int nargs, List * argnames, bool expand_variadic, bool expand_defaults)
{

    check_expected(names);
    check_expected(nargs);
    check_expected(argnames);
    check_expected(expand_variadic);
    check_expected(expand_defaults);
    optional_assignment(names);
    optional_assignment(argnames);
    return (FuncCandidateList) mock();

}

bool 
FunctionIsVisible(Oid funcid)
{

    check_expected(funcid);

    return (bool) mock();

}

Oid 
OpernameGetOprid(List * names, Oid oprleft, Oid oprright)
{

    check_expected(names);
    check_expected(oprleft);
    check_expected(oprright);
    optional_assignment(names);
    return (Oid) mock();

}

FuncCandidateList 
OpernameGetCandidates(List * names, char oprkind)
{

    check_expected(names);
    check_expected(oprkind);
    optional_assignment(names);
    return (FuncCandidateList) mock();

}

bool 
OperatorIsVisible(Oid oprid)
{

    check_expected(oprid);

    return (bool) mock();

}

Oid 
OpclassnameGetOpcid(Oid amid, const char *opcname)
{

    check_expected(amid);
    check_expected(opcname);

    return (Oid) mock();

}

bool 
OpclassIsVisible(Oid opcid)
{

    check_expected(opcid);

    return (bool) mock();

}

Oid 
ConversionGetConid(const char *conname)
{

    check_expected(conname);

    return (Oid) mock();

}

bool 
ConversionIsVisible(Oid conid)
{

    check_expected(conid);

    return (bool) mock();

}

void 
DeconstructQualifiedName(List * names, char **nspname_p, char **objname_p)
{

    check_expected(names);
    check_expected(nspname_p);
    check_expected(objname_p);
    optional_assignment(names);
    optional_assignment(nspname_p);
    optional_assignment(objname_p);
    mock();

}

Oid 
LookupExplicitNamespace(const char *nspname)
{

    check_expected(nspname);

    return (Oid) mock();

}

bool 
ExistsCommittedTempNamespace(void)
{



    return (bool) mock();

}

void 
DropTempTableNamespaceForResetSession(Oid namespaceOid)
{

    check_expected(namespaceOid);

    mock();

}

void 
SetTempNamespace(Oid namespaceOid)
{

    check_expected(namespaceOid);

    mock();

}

Oid 
ResetTempNamespace(void)
{



    return (Oid) mock();

}

bool 
TempNamespaceOidIsValid(void)
{



    return (bool) mock();

}

Oid 
LookupCreationNamespace(const char *nspname)
{

    check_expected(nspname);

    return (Oid) mock();

}

Oid 
QualifiedNameGetCreationNamespace(List * names, char **objname_p)
{

    check_expected(names);
    check_expected(objname_p);
    optional_assignment(names);
    optional_assignment(objname_p);
    return (Oid) mock();

}

RangeVar       *
makeRangeVarFromNameList(List * names)
{

    check_expected(names);
    optional_assignment(names);
    return (RangeVar *) mock();

}

char           *
NameListToString(List * names)
{

    check_expected(names);
    optional_assignment(names);
    return (char *) mock();

}

char           *
NameListToQuotedString(List * names)
{

    check_expected(names);
    optional_assignment(names);
    return (char *) mock();

}

bool 
isTempNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    return (bool) mock();

}

bool 
isAnyTempNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    return (bool) mock();

}

bool 
isOtherTempNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    return (bool) mock();

}

void 
PushSpecialNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    mock();

}

void 
PopSpecialNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    mock();

}

Oid 
FindConversionByName(List * conname)
{

    check_expected(conname);
    optional_assignment(conname);
    return (Oid) mock();

}

Oid 
FindDefaultConversionProc(int4 for_encoding, int4 to_encoding)
{

    check_expected(for_encoding);
    check_expected(to_encoding);

    return (Oid) mock();

}

void 
InitializeSearchPath(void)
{



    mock();

}

void 
AtEOXact_Namespace(bool isCommit)
{

    check_expected(isCommit);

    mock();

}

void 
AtEOSubXact_Namespace(bool isCommit, SubTransactionId mySubid, SubTransactionId parentSubid)
{

    check_expected(isCommit);
    check_expected(mySubid);
    check_expected(parentSubid);

    mock();

}

char           *namespace_search_path;
List           *
fetch_search_path(bool includeImplicit)
{

    check_expected(includeImplicit);

    return (List *) mock();

}
