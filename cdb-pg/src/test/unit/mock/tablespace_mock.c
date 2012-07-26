/*
 * Auto-generated Mocking Source
 * 
 * Source: commands/tablespace.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "access/heapam.h"
#include "catalog/heap.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/catquery.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "cdb/cdbmirroredflatfile.h"
#include "commands/comment.h"
#include "commands/filespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"
#include "access/persistentfilesysobjname.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbmirroredfilesysobj.h"

void 
CreateTableSpace(CreateTableSpaceStmt * stmt)
{

    check_expected(stmt);
    optional_assignment(stmt);
    mock();

}

void 
RemoveTableSpace(List * names, DropBehavior behavior, bool missing_ok)
{

    check_expected(names);
    check_expected(behavior);
    check_expected(missing_ok);
    optional_assignment(names);
    mock();

}

void 
RenameTableSpace(const char *oldname, const char *newname)
{

    check_expected(oldname);
    check_expected(newname);

    mock();

}

void 
AlterTableSpaceOwner(const char *name, Oid newOwnerId)
{

    check_expected(name);
    check_expected(newOwnerId);

    mock();

}

void 
TablespaceCreateDbspace(Oid spcNode, Oid dbNode, bool isRedo)
{

    check_expected(spcNode);
    check_expected(dbNode);
    check_expected(isRedo);

    mock();

}

Oid 
GetDefaultTablespace(void)
{



    return (Oid) mock();

}

Oid 
get_tablespace_oid(const char *tablespacename)
{

    check_expected(tablespacename);

    return (Oid) mock();

}

char           *
get_tablespace_name(Oid spc_oid)
{

    check_expected(spc_oid);

    return (char *) mock();

}

bool 
directory_is_empty(const char *path)
{

    check_expected(path);

    return (bool) mock();

}

void 
tblspc_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord * rptr)
{

    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(lsn.xlogid);
    check_expected(lsn.xrecoff);
    check_expected(rptr);
    optional_assignment(rptr);
    mock();

}

void 
tblspc_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord * record)
{

    check_expected(buf);
    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}

void 
set_short_version(const char *path, DbDirNode * dbDirNode, bool mirror)
{

    check_expected(path);
    check_expected(dbDirNode);
    check_expected(mirror);
    optional_assignment(dbDirNode);
    mock();

}
