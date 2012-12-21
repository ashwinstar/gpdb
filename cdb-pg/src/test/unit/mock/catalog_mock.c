/*
 * Auto-generated Mocking Source
 * 
 * Source: catalog/catalog.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <fcntl.h>
#include <unistd.h>
#include "access/genam.h"
#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_pltemplate.h"
#include "catalog/pg_resqueue.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_filespace_entry.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_resqueue.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_tidycat.h"
#include "catalog/gp_configuration.h"
#include "catalog/gp_configuration.h"
#include "catalog/gp_segment_config.h"
#include "catalog/gp_san_config.h"
#include "catalog/gp_verification_history.h"
#include "catalog/gp_master_mirroring.h"
#include "catalog/gp_persistent.h"
#include "catalog/gp_global_sequence.h"
#include "catalog/gp_id.h"
#include "catalog/gp_version.h"
#include "catalog/toasting.h"
#include "catalog/gp_policy.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/fmgroids.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbvars.h"

char           *
relpath(RelFileNode rnode)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);

    return (char *) mock();

}

void 
CopyRelPath(char *target, int targetMaxLen, RelFileNode rnode)
{

    check_expected(target);
    check_expected(targetMaxLen);
    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);
    optional_assignment(target);
    mock();

}

char           *
GetDatabasePath(Oid dbNode, Oid spcNode)
{

    check_expected(dbNode);
    check_expected(spcNode);

    return (char *) mock();

}

void 
CopyDatabasePath(char *target, int targetMaxLen, Oid dbNode, Oid spcNode)
{

    check_expected(target);
    check_expected(targetMaxLen);
    check_expected(dbNode);
    check_expected(spcNode);
    optional_assignment(target);
    mock();

}

void 
FormDatabasePath(char *databasePath, char *filespaceLocation, Oid tablespaceOid, Oid databaseOid)
{

    check_expected(databasePath);
    check_expected(filespaceLocation);
    check_expected(tablespaceOid);
    check_expected(databaseOid);
    optional_assignment(databasePath);
    optional_assignment(filespaceLocation);
    mock();

}

void 
FormTablespacePath(char *tablespacePath, char *filespaceLocation, Oid tablespaceOid)
{

    check_expected(tablespacePath);
    check_expected(filespaceLocation);
    check_expected(tablespaceOid);
    optional_assignment(tablespacePath);
    optional_assignment(filespaceLocation);
    mock();

}

void 
FormRelationPath(char *relationPath, char *filespaceLocation, RelFileNode rnode)
{

    check_expected(relationPath);
    check_expected(filespaceLocation);
    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);
    optional_assignment(relationPath);
    optional_assignment(filespaceLocation);
    mock();

}

bool 
IsSystemRelation(Relation relation)
{

    check_expected(relation);

    return (bool) mock();

}

bool 
IsToastRelation(Relation relation)
{

    check_expected(relation);

    return (bool) mock();

}

bool 
IsAoSegmentRelation(Relation relation)
{

    check_expected(relation);

    return (bool) mock();

}

bool 
IsSystemClass(Form_pg_class reltuple)
{

    check_expected(reltuple);

    return (bool) mock();

}

bool 
IsToastClass(Form_pg_class reltuple)
{

    check_expected(reltuple);

    return (bool) mock();

}

bool 
IsAoSegmentClass(Form_pg_class reltuple)
{

    check_expected(reltuple);

    return (bool) mock();

}

bool 
IsSystemNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    return (bool) mock();

}

bool 
IsToastNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    return (bool) mock();

}

bool 
IsAoSegmentNamespace(Oid namespaceId)
{

    check_expected(namespaceId);

    return (bool) mock();

}

bool 
isMasterOnly(Oid relationOid)
{

    check_expected(relationOid);

    return (bool) mock();

}

bool 
IsReservedName(const char *name)
{

    check_expected(name);

    return (bool) mock();

}

char           *
GetReservedPrefix(const char *name)
{

    check_expected(name);

    return (char *) mock();

}

bool 
IsSharedRelation(Oid relationId)
{

    check_expected(relationId);

    return (bool) mock();

}

Oid 
GetNewOid(Relation relation)
{

    check_expected(relation);

    return (Oid) mock();

}

Oid 
GetNewOidWithIndex(Relation relation, Relation indexrel)
{

    check_expected(relation);
    check_expected(indexrel);

    return (Oid) mock();

}

Oid 
GetNewRelFileNode(Oid reltablespace, bool relisshared, Relation pg_class)
{

    check_expected(reltablespace);
    check_expected(relisshared);
    check_expected(pg_class);

    return (Oid) mock();

}

bool 
CheckNewRelFileNodeIsOk(Oid newOid, Oid reltablespace, bool relisshared, Relation pg_class)
{

    check_expected(newOid);
    check_expected(reltablespace);
    check_expected(relisshared);
    check_expected(pg_class);

    return (bool) mock();

}
