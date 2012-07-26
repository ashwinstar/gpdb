/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/freespace.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <limits.h>
#include <math.h>
#include <unistd.h>
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "cdb/cdbvars.h"

int             MaxFSMRelations;
int             MaxFSMPages;
void 
InitFreeSpaceMap(void)
{



    mock();

}

Size 
FreeSpaceShmemSize(void)
{



    return (Size) mock();

}

FSMHeader      *
GetFreeSpaceMap(void)
{



    return (FSMHeader *) mock();

}

BlockNumber 
GetPageWithFreeSpace(RelFileNode * rel, Size spaceNeeded)
{

    check_expected(rel);
    check_expected(spaceNeeded);
    optional_assignment(rel);
    return (BlockNumber) mock();

}

BlockNumber 
RecordAndGetPageWithFreeSpace(RelFileNode * rel, BlockNumber oldPage, Size oldSpaceAvail, Size spaceNeeded)
{

    check_expected(rel);
    check_expected(oldPage);
    check_expected(oldSpaceAvail);
    check_expected(spaceNeeded);
    optional_assignment(rel);
    return (BlockNumber) mock();

}

Size 
GetAvgFSMRequestSize(RelFileNode * rel)
{

    check_expected(rel);
    optional_assignment(rel);
    return (Size) mock();

}

void 
RecordRelationFreeSpace(RelFileNode * rel, BlockNumber interestingPages, int nPages, PageFreeSpaceInfo * pageSpaces)
{

    check_expected(rel);
    check_expected(interestingPages);
    check_expected(nPages);
    check_expected(pageSpaces);
    optional_assignment(rel);
    optional_assignment(pageSpaces);
    mock();

}

BlockNumber 
GetFreeIndexPage(RelFileNode * rel)
{

    check_expected(rel);
    optional_assignment(rel);
    return (BlockNumber) mock();

}

void 
RecordIndexFreeSpace(RelFileNode * rel, BlockNumber interestingPages, int nPages, BlockNumber * pages)
{

    check_expected(rel);
    check_expected(interestingPages);
    check_expected(nPages);
    check_expected(pages);
    optional_assignment(rel);
    optional_assignment(pages);
    mock();

}

void 
FreeSpaceMapTruncateRel(RelFileNode * rel, BlockNumber nblocks)
{

    check_expected(rel);
    check_expected(nblocks);
    optional_assignment(rel);
    mock();

}

void 
FreeSpaceMapForgetRel(RelFileNode * rel)
{

    check_expected(rel);
    optional_assignment(rel);
    mock();

}

void 
FreeSpaceMapForgetDatabase(Oid tblspc, Oid dbid)
{

    check_expected(tblspc);
    check_expected(dbid);

    mock();

}

void 
PrintFreeSpaceMapStatistics(int elevel)
{

    check_expected(elevel);

    mock();

}

void 
DumpFreeSpaceMap(int code, Datum arg)
{

    check_expected(code);
    check_expected(arg);

    mock();

}

void 
LoadFreeSpaceMap(void)
{



    mock();

}

List           *
AppendRelToVacuumRels(Relation rel)
{

    check_expected(rel);

    return (List *) mock();

}

void 
ResetVacuumRels(void)
{



    mock();

}

void 
ClearFreeSpaceForVacuumRels(void)
{



    mock();

}
