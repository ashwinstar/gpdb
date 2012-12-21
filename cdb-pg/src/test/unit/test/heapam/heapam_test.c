#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/gist_private.h"
#include "storage/bufpage.h"
#include "access/heapam.h"

/*
 * Test case for http://git.postgresql.org/gitweb/?p=postgresql.git;a=commitdiff;h=fb28d2604a419c48b0f6f8df46198a90d8343209
 */
void test_heap_xlog_newpage(void** state)
{
	disable_unit_test();

	InterruptHoldoffCount = 1;
	InRecovery = true;
	ThisTimeLineID = 1;

	char* record_buffer = malloc(64 * 1024);
	memset(record_buffer, 0, 64 * 1024);
	XLogRecord* record = (XLogRecord*)record_buffer;
	record->xl_info |= XLOG_HEAP_NEWPAGE;
	record->xl_len = SizeOfHeapNewpage + BLCKSZ;
	xl_heap_newpage *xldata = (xl_heap_newpage *) XLogRecGetData(record);
	xldata->blkno = 1;
	XLogRecPtr beginLoc;
	XLogRecPtr lsn;
	memset(&lsn, 1, sizeof(lsn));
	XLogRecPtr compareLsn;
	memset(&compareLsn, 0, sizeof(compareLsn));

	char* page_buffer = malloc(64 * 1024);
	memset(page_buffer, 0, 64 * 1024);

	/* The usual redo stuff */
	will_return(XLogOpenRelation, NULL);
	expect_any(XLogOpenRelation, rnode.dbNode);
	expect_any(XLogOpenRelation, rnode.relNode);
	expect_any(XLogOpenRelation, rnode.spcNode);

	expect_any(LWLockHeldByMe, lockid);
	will_return(LWLockHeldByMe, true);
	will_return(FileRepPrimary_IsResyncManagerOrWorker, false);

	expect_any(XLogReadBuffer, reln);
	expect_any(XLogReadBuffer, blkno);
	expect_value(XLogReadBuffer, init, true);
	will_return(XLogReadBuffer, 1);

	Page mockPage = (Page)page_buffer;
	will_return(BufferGetPage, mockPage);
	expect_value(BufferGetPage, buffer, 1);

	expect_value(MarkBufferDirty, buffer, 1);
	will_be_called(MarkBufferDirty);

	expect_value(UnlockReleaseBuffer, buffer, 1);
	will_be_called(UnlockReleaseBuffer);

	heap_redo(beginLoc, lsn, record);

	// PageSetLSN(page, lsn) should not be called
	// PageSetTLI(page, ThisTimeLineID) should not be called
	XLogRecPtr actualLsn = PageGetLSN(mockPage);
	assert_memory_equal(&actualLsn, &compareLsn, sizeof(compareLsn));
	assert_int_equal(PageGetTLI(mockPage), 0);
}

int main(int argc, char* argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test_heap_xlog_newpage)
	};
	return run_tests(tests);
}
