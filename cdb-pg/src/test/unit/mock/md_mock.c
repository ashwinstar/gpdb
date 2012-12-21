/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/smgr.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <glob.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "access/appendonlywriter.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "cdb/cdbmirroredbufferpool.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbfilerepprimary.h"
#include "utils/guc.h"
#include "catalog/pg_tablespace.h"

bool 
mdinit(void)
{



    return (bool) mock();

}

bool 
mdclose(SMgrRelation reln)
{

    check_expected(reln);

    return (bool) mock();

}

void 
mdcreatefilespacedir(Oid filespaceOid, char *primaryFilespaceLocation, char *mirrorFilespaceLocation, StorageManagerMirrorMode mirrorMode, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    check_expected(mirrorMode);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
mdcreatetablespacedir(Oid tablespaceOid, StorageManagerMirrorMode mirrorMode, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(tablespaceOid);
    check_expected(mirrorMode);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
mdcreatedbdir(DbDirNode * dbDirNode, StorageManagerMirrorMode mirrorMode, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(dbDirNode);
    check_expected(mirrorMode);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(dbDirNode);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
mdcreate(SMgrRelation reln, bool isLocalBuf, char *relationName, MirrorDataLossTrackingState mirrorDataLossTrackingState, int64 mirrorDataLossTrackingSessionNum, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(reln);
    check_expected(isLocalBuf);
    check_expected(relationName);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relationName);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

bool 
mdunlink(RelFileNode rnode, char *relationName, bool primaryOnly, bool isRedo, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);
    check_expected(relationName);
    check_expected(primaryOnly);
    check_expected(isRedo);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relationName);
    optional_assignment(mirrorDataLossOccurred);
    return (bool) mock();

}

bool 
mdrmfilespacedir(Oid filespaceOid, char *primaryFilespaceLocation, char *mirrorFilespaceLocation, bool primaryOnly, bool mirrorOnly, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    check_expected(primaryOnly);
    check_expected(mirrorOnly);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    optional_assignment(mirrorDataLossOccurred);
    return (bool) mock();

}

bool 
mdrmtablespacedir(Oid tablespaceOid, bool primaryOnly, bool mirrorOnly, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(tablespaceOid);
    check_expected(primaryOnly);
    check_expected(mirrorOnly);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(mirrorDataLossOccurred);
    return (bool) mock();

}

bool 
mdrmdbdir(DbDirNode * dbDirNode, bool primaryOnly, bool mirrorOnly, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(dbDirNode);
    check_expected(primaryOnly);
    check_expected(mirrorOnly);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(dbDirNode);
    optional_assignment(mirrorDataLossOccurred);
    return (bool) mock();

}

bool 
mdextend(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{

    check_expected(reln);
    check_expected(blocknum);
    check_expected(buffer);
    check_expected(isTemp);
    optional_assignment(buffer);
    return (bool) mock();

}

bool 
mdread(SMgrRelation reln, BlockNumber blocknum, char *buffer)
{

    check_expected(reln);
    check_expected(blocknum);
    check_expected(buffer);
    optional_assignment(buffer);
    return (bool) mock();

}

bool 
mdwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{

    check_expected(reln);
    check_expected(blocknum);
    check_expected(buffer);
    check_expected(isTemp);
    optional_assignment(buffer);
    return (bool) mock();

}

BlockNumber 
mdnblocks(SMgrRelation reln)
{

    check_expected(reln);

    return (BlockNumber) mock();

}

BlockNumber 
mdtruncate(SMgrRelation reln, BlockNumber nblocks, bool isTemp)
{

    check_expected(reln);
    check_expected(nblocks);
    check_expected(isTemp);

    return (BlockNumber) mock();

}

bool 
mdimmedsync(SMgrRelation reln)
{

    check_expected(reln);

    return (bool) mock();

}

bool 
mdsync(void)
{



    return (bool) mock();

}

void 
RememberFsyncRequest(RelFileNode rnode, BlockNumber segno)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);
    check_expected(segno);

    mock();

}

void 
ForgetRelationFsyncRequests(RelFileNode rnode)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);

    mock();

}

void 
ForgetDatabaseFsyncRequests(Oid tblspc, Oid dbid)
{

    check_expected(tblspc);
    check_expected(dbid);

    mock();

}
