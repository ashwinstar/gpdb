#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/gist_private.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include "storage/off.h"

void test_gistxlog_continue_insert(void** state) {
	/* General setup */
	InterruptHoldoffCount = 1;
	InRecovery = true;
	expect_any_count(MarkBufferDirty, buffer, -1);
	will_be_called_count(MarkBufferDirty, -1);
	expect_any_count(MemoryContextDeleteImpl, context, -1);
	expect_any_count(MemoryContextDeleteImpl, sfile, -1);
	expect_any_count(MemoryContextDeleteImpl, func, -1);
	expect_any_count(MemoryContextDeleteImpl, sline, -1);
	will_be_called_count(MemoryContextDeleteImpl, -1);
	expect_any_count(MemoryContextReset, context, -1);
	will_be_called_count(MemoryContextReset, -1);

	char* record_buffer = malloc(32 * 1024);
	memset(record_buffer, 0, 32 * 1024);
	XLogRecord* record = (XLogRecord*) record_buffer;
	record->xl_info |= XLOG_GIST_PAGE_UPDATE;
	gistxlogPageUpdate *xldata = (gistxlogPageUpdate *) XLogRecGetData(record);
	xldata->blkno = 1;
	xldata->key.ip_blkid.bi_hi = 0;
	xldata->key.ip_blkid.bi_lo = 1;
	xldata->key.ip_posid = 16;

	char* gist_page = malloc(32 * 1024);
	memset(gist_page, 0, 32 * 1024);
	PageHeader gist_page_header = (PageHeader) gist_page;
	gist_page_header->pd_lower = SizeOfPageHeaderData + sizeof(ItemIdData);

	char* gist_new_page = malloc(32 * 1024);
	memset(gist_new_page, 0, 32 * 1024);
	PageHeader gist_new_page_header = (PageHeader) gist_new_page;

	XLogRecPtr beginLoc;
	XLogRecPtr lsn;

	RelationData indexRelationData;
	memset(&indexRelationData, 0, sizeof(indexRelationData));

	will_return(XLogOpenRelation, NULL);
	expect_any(XLogOpenRelation, rnode.dbNode);
	expect_any(XLogOpenRelation, rnode.relNode);
	expect_any(XLogOpenRelation, rnode.spcNode);

	expect_any(LWLockHeldByMe, lockid);
	will_return(LWLockHeldByMe, true);
	will_return(FileRepPrimary_IsResyncManagerOrWorker, false);

	expect_any(XLogReadBuffer, reln);
	expect_any(XLogReadBuffer, blkno);
	expect_value(XLogReadBuffer, init, false);
	will_return(XLogReadBuffer, 1);

	Page mockPage = (Page) record_buffer;
	will_return(BufferGetPage, mockPage);
	expect_value(BufferGetPage, buffer, 1);

	expect_value(UnlockReleaseBuffer, buffer, 1);
	will_be_called(UnlockReleaseBuffer);

	// Here we are in incomplete insert cleanup land
	will_return(XLogOpenRelation, &indexRelationData);
	expect_any(XLogOpenRelation, rnode.dbNode);
	expect_any(XLogOpenRelation, rnode.relNode);
	expect_any(XLogOpenRelation, rnode.spcNode);

	expect_any(LWLockHeldByMe, lockid);
	will_return(LWLockHeldByMe, true);
	will_return(FileRepPrimary_IsResyncManagerOrWorker, false);

	expect_any(LWLockHeldByMe, lockid);
	will_return(LWLockHeldByMe, true);
	will_return(FileRepPrimary_IsResyncManagerOrWorker, false);

	GISTInsertStack mockPathStack;
	mockPathStack.parent = NULL;
	mockPathStack.blkno = 2;
	expect_any(gistFindPath, r);
	expect_any(gistFindPath, child);
	will_return(gistFindPath, &mockPathStack);

	expect_any(ReadBuffer, reln);
	expect_value(ReadBuffer, blockNum, 2);
	will_return(ReadBuffer, 2);

	expect_value(LockBuffer, buffer, 2);
	expect_value(LockBuffer, mode, GIST_EXCLUSIVE);
	will_be_called(LockBuffer);

	expect_any(gistcheckpage, rel);
	expect_any(gistcheckpage, buf);
	will_be_called(gistcheckpage);

	Page mockGistPage = (Page) gist_page;
	PageHeader mockGistPageHeader = (PageHeader) gist_page;
	mockGistPageHeader->pd_lower = SizeOfPageHeaderData + sizeof(ItemId);
	mockGistPageHeader->pd_upper = SizeOfPageHeaderData + (2 * sizeof(ItemId));

	// Prepere for PageGetItemId
	mockGistPageHeader->pd_linp[FirstOffsetNumber - 1].lp_off = 32;

	// Prepare for PageGetItem
	IndexTupleData idxtup;
	idxtup.t_tid.ip_blkid.bi_hi = 0;
	idxtup.t_tid.ip_blkid.bi_lo = 1;
	idxtup.t_tid.ip_posid = 16;
	memcpy(mockGistPage + mockGistPageHeader->pd_linp[0].lp_off, &idxtup,
			sizeof(IndexTupleData));
	expect_value(BufferGetPage, buffer, 2);
	will_return(BufferGetPage, mockGistPage);

	expect_any(gistnospace, page);
	expect_any(gistnospace, itvec);
	expect_any(gistnospace, len);
	expect_any(gistnospace, todelete);
	expect_any(gistnospace, freespace);
	will_return(gistnospace, true);

	expect_any(ReadBuffer, reln);
	expect_value(ReadBuffer, blockNum, P_NEW);
	will_return(ReadBuffer, 3);

	expect_value(LockBuffer, buffer, 3);
	expect_value(LockBuffer, mode, GIST_EXCLUSIVE);
	will_be_called(LockBuffer);

	expect_any(GISTInitBuffer, b);
	expect_any(GISTInitBuffer, f);
	will_be_called(GISTInitBuffer);

	Page mockNewGistPage = (Page) gist_new_page;
	expect_value(BufferGetPage, buffer, 3);
	will_return(BufferGetPage, mockNewGistPage);

	expect_any(gistfillbuffer, r);
	expect_any(gistfillbuffer, page);
	expect_any(gistfillbuffer, itup);
	expect_any(gistfillbuffer, len);
	expect_any(gistfillbuffer, off);
	will_return(gistfillbuffer, 16);

	expect_value(BufferGetBlockNumber, buffer, 2);
	will_return(BufferGetBlockNumber, 16);

	expect_any(PageIndexTupleDelete, page);
	expect_any(PageIndexTupleDelete, offnum);
	will_be_called(PageIndexTupleDelete);

	/* Split page construction */
	expect_value(BufferGetPage, buffer, 3);
	will_return(BufferGetPage, mockNewGistPage);
	expect_value(BufferGetBlockNumber, buffer, 3);
	will_return(BufferGetBlockNumber, 3);

	expect_any(gistextractpage, page);
	expect_any(gistextractpage, len);
	will_assign_value(gistextractpage, len, 0);
	will_return(gistextractpage, NULL);

	expect_any(gistfillitupvec, vec);
	expect_any(gistfillitupvec, veclen);
	expect_any(gistfillitupvec, memlen);
	will_assign_value(gistfillitupvec, memlen, 0);
	will_return(gistfillitupvec, NULL);

	expect_value(BufferGetPage, buffer, 2);
	will_return(BufferGetPage, mockNewGistPage);
	expect_value(BufferGetBlockNumber, buffer, 2);
	will_return(BufferGetBlockNumber, 2);

	expect_any(gistextractpage, page);
	expect_any(gistextractpage, len);
	will_assign_value(gistextractpage, len, 0);
	will_return(gistextractpage, NULL);

	expect_any(gistfillitupvec, vec);
	expect_any(gistfillitupvec, veclen);
	expect_any(gistfillitupvec, memlen);
	will_assign_value(gistfillitupvec, memlen, 0);
	will_return(gistfillitupvec, NULL);

	expect_value(XLogInsert, rmid, RM_GIST_ID);
	expect_value(XLogInsert, info, XLOG_GIST_PAGE_SPLIT);
	expect_any(XLogInsert, rdata);
	will_return(XLogInsert, 0);

	expect_value(BufferGetBlockNumber, buffer, 2);
	will_return(BufferGetBlockNumber, 2);
	expect_value(UnlockReleaseBuffer, buffer, 2);
	will_be_called(UnlockReleaseBuffer);
	expect_value(BufferGetBlockNumber, buffer, 3);
	will_return(BufferGetBlockNumber, 3);
	expect_value(UnlockReleaseBuffer, buffer, 3);
	will_be_called(UnlockReleaseBuffer);

	gist_redo(beginLoc, lsn, record);
	gist_xlog_cleanup();
}

int main(int argc, char* argv[]) {
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = { unit_test(test_gistxlog_continue_insert) };
	return run_tests(tests);
}
