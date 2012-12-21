#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/gist_private.h"
#include "storage/bufpage.h"
#include "access/heapam.h"
#include "nodes/memnodes.h"

// Copied from smgr.c
#define XLOG_SMGR_TRUNCATE	0x20
typedef struct xl_smgr_truncate
{
	BlockNumber blkno;
	RelFileNode rnode;
	ItemPointerData persistentTid;
	int64 persistentSerialNum;
} xl_smgr_truncate;

/*
 * Test case for http://git.postgresql.org/gitweb/?p=postgresql.git;a=commitdiff;h=85506ffbd51e15dffbd307e4d1060cfffb24c9e0
 */
void test_smgr_redo_truncate_before_create(void** state)
{
	InterruptHoldoffCount = 1;

	char* record_buffer = malloc(64 * 1024);
	memset(record_buffer, 0, 64 * 1024);
	XLogRecord* record = (XLogRecord*)record_buffer;
	record->xl_info |= XLOG_SMGR_TRUNCATE;
	xl_smgr_truncate *xldata = (xl_smgr_truncate *) XLogRecGetData(record);
	xldata->blkno = 15;

	XLogRecPtr beginLoc;
	XLogRecPtr lsn;
	memset(&lsn, 1, sizeof(lsn));

	MemoryContextData hashTableMemoryContext;
	memset(&hashTableMemoryContext, 0, sizeof(hashTableMemoryContext));
	hashTableMemoryContext.type = T_AllocSetContext;

	// smgropen
	expect_any(AllocSetContextCreate, parent);
	expect_any(AllocSetContextCreate, name);
	expect_any(AllocSetContextCreate, minContextSize);
	expect_any(AllocSetContextCreate, initBlockSize);
	expect_any(AllocSetContextCreate, maxBlockSize);
	will_return(AllocSetContextCreate, &hashTableMemoryContext);

	expect_any(DropRelFileNodeBuffers, rnode.dbNode);
	expect_any(DropRelFileNodeBuffers, rnode.relNode);
	expect_any(DropRelFileNodeBuffers, rnode.spcNode);
	expect_any(DropRelFileNodeBuffers, istemp);
	expect_any(DropRelFileNodeBuffers, firstDelBlock);
	will_be_called(DropRelFileNodeBuffers);

	expect_any(FreeSpaceMapTruncateRel, rel);
	expect_value(FreeSpaceMapTruncateRel, nblocks, 15);
	will_be_called(FreeSpaceMapTruncateRel);

	expect_any(mdtruncate, reln);
	expect_value(mdtruncate, nblocks, 15);
	expect_value(mdtruncate, isTemp, false);
	will_return(mdtruncate, 4);

	expect_any(XLogTruncateRelation, rnode.dbNode);
	expect_any(XLogTruncateRelation, rnode.relNode);
	expect_any(XLogTruncateRelation, rnode.spcNode);
	expect_value(XLogTruncateRelation, nblocks, 15);
	will_be_called(XLogTruncateRelation);

	smgr_redo(beginLoc, lsn, record);
}

int main(int argc, char* argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test_smgr_redo_truncate_before_create)
	};
	return run_tests(tests);
}
