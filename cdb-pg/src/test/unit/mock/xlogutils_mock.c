/*
 * Auto-generated Mocking Source
 * 
 * Source: access/xlogutils.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "postgres.h"
#include "access/xlogutils.h"
#include "storage/bufpage.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbpersistentrecovery.h"
#include "cdb/cdbpersistenttablespace.h"
#include "utils/guc.h"
#include "postmaster/postmaster.h"

void 
XLogInitRelationCache(void)
{



    mock();

}

void 
XLogCheckInvalidPages(void)
{



    mock();

}

void 
XLogCloseRelationCache(void)
{



    mock();

}

Relation 
XLogOpenRelation(RelFileNode rnode)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);

    return (Relation) mock();

}

void 
XLogDropRelation(RelFileNode rnode)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);

    mock();

}

void 
XLogDropDatabase(Oid tblspc, Oid dbid)
{

    check_expected(tblspc);
    check_expected(dbid);

    mock();

}

void 
XLogTruncateRelation(RelFileNode rnode, BlockNumber nblocks)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);
    check_expected(nblocks);

    mock();

}

Buffer 
XLogReadBuffer(Relation reln, BlockNumber blkno, bool init)
{

    check_expected(reln);
    check_expected(blkno);
    check_expected(init);

    return (Buffer) mock();

}
