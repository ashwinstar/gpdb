/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbappendonlyblockdirectory.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "cdb/cdbappendonlyblockdirectory.h"
#include "catalog/aoblkdir.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "catalog/indexing.h"
#include "parser/parse_oper.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/guc.h"
#include "cdb/cdbappendonlyam.h"

int             gp_blockdirectory_entry_min_range;
int             gp_blockdirectory_minipage_size;
void 
AppendOnlyBlockDirectoryEntry_GetBeginRange(AppendOnlyBlockDirectoryEntry * directoryEntry, int64 * fileOffset, int64 * firstRowNum)
{

    check_expected(directoryEntry);
    check_expected(fileOffset);
    check_expected(firstRowNum);
    optional_assignment(directoryEntry);
    optional_assignment(fileOffset);
    optional_assignment(firstRowNum);
    mock();

}

void 
AppendOnlyBlockDirectoryEntry_GetEndRange(AppendOnlyBlockDirectoryEntry * directoryEntry, int64 * afterFileOffset, int64 * lastRowNum)
{

    check_expected(directoryEntry);
    check_expected(afterFileOffset);
    check_expected(lastRowNum);
    optional_assignment(directoryEntry);
    optional_assignment(afterFileOffset);
    optional_assignment(lastRowNum);
    mock();

}

bool 
AppendOnlyBlockDirectoryEntry_RangeHasRow(AppendOnlyBlockDirectoryEntry * directoryEntry, int64 checkRowNum)
{

    check_expected(directoryEntry);
    check_expected(checkRowNum);
    optional_assignment(directoryEntry);
    return (bool) mock();

}

bool 
AppendOnlyBlockDirectory_GetEntry(AppendOnlyBlockDirectory * blockDirectory, AOTupleId * aoTupleId, int columnGroupNo, AppendOnlyBlockDirectoryEntry * directoryEntry)
{

    check_expected(blockDirectory);
    check_expected(aoTupleId);
    check_expected(columnGroupNo);
    check_expected(directoryEntry);
    optional_assignment(blockDirectory);
    optional_assignment(aoTupleId);
    optional_assignment(directoryEntry);
    return (bool) mock();

}

void 
AppendOnlyBlockDirectory_Init_forInsert(AppendOnlyBlockDirectory * blockDirectory, AppendOnlyEntry * aoEntry, Snapshot appendOnlyMetaDataSnapshot, FileSegInfo * segmentFileInfo, int64 lastSequence, Relation aoRel, int segno, int numColumnGroups, bool isAOCol)
{

    check_expected(blockDirectory);
    check_expected(aoEntry);
    check_expected(appendOnlyMetaDataSnapshot);
    check_expected(segmentFileInfo);
    check_expected(lastSequence);
    check_expected(aoRel);
    check_expected(segno);
    check_expected(numColumnGroups);
    check_expected(isAOCol);
    optional_assignment(blockDirectory);
    optional_assignment(aoEntry);
    optional_assignment(segmentFileInfo);
    mock();

}

void 
AppendOnlyBlockDirectory_Init_forSearch(AppendOnlyBlockDirectory * blockDirectory, AppendOnlyEntry * aoEntry, Snapshot appendOnlyMetaDataSnapshot, FileSegInfo ** segmentFileInfo, int totalSegfiles, Relation aoRel, int numColumnGroups, bool isAOCol)
{

    check_expected(blockDirectory);
    check_expected(aoEntry);
    check_expected(appendOnlyMetaDataSnapshot);
    check_expected(segmentFileInfo);
    check_expected(totalSegfiles);
    check_expected(aoRel);
    check_expected(numColumnGroups);
    check_expected(isAOCol);
    optional_assignment(blockDirectory);
    optional_assignment(aoEntry);
    optional_assignment(segmentFileInfo);
    mock();

}

bool 
AppendOnlyBlockDirectory_InsertEntry(AppendOnlyBlockDirectory * blockDirectory, int columnGroupNo, int64 firstRowNum, int64 fileOffset, int64 rowCount)
{

    check_expected(blockDirectory);
    check_expected(columnGroupNo);
    check_expected(firstRowNum);
    check_expected(fileOffset);
    check_expected(rowCount);
    optional_assignment(blockDirectory);
    return (bool) mock();

}

void 
AppendOnlyBlockDirectory_End_forInsert(AppendOnlyBlockDirectory * blockDirectory)
{

    check_expected(blockDirectory);
    optional_assignment(blockDirectory);
    mock();

}

void 
AppendOnlyBlockDirectory_End_forSearch(AppendOnlyBlockDirectory * blockDirectory)
{

    check_expected(blockDirectory);
    optional_assignment(blockDirectory);
    mock();

}
