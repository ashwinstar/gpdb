/*-------------------------------------------------------------------------
 *
 * cdbdistributedxidmap.c
 *		Maps distributed (DTM) XIDs to local xids 
 *
 * The pg_distributedxidmap manager is a pg_subtrans-like manager that stores the
 * local transaction id for each global transaction.
 *
 * We only need to remember pg_distributedxidmap information for currently-open 
 * global transactions.  Thus, there is no need to preserve data over a crash
 * and restart.
 *
 * There are no XLOG interactions since we do not care about preserving
 * the map across crashes.  During database startup, we simply delete all
 * currently-active pages of DistributedXidMap and create an initial first page
 * of zeroes.
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slru.h"
#include "access/transam.h"
#include "access/cdbdistributedxidmap.h"
#include "utils/tqual.h"


/*
 * Defines for DistributedTransactionId page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: We are assuming that no more than 4 billion distributed transactions can
 * be generated during one greenplum array start.
 */

/* We need four bytes per xact */
#define LOCAL_XACTS_PER_PAGE (BLCKSZ / sizeof(TransactionId))

#define DistributedTransactionIdToPage(gxid) ((gxid) / (DistributedTransactionId) LOCAL_XACTS_PER_PAGE)
#define DistributedTransactionIdToEntry(gxid) ((gxid) % (DistributedTransactionId) LOCAL_XACTS_PER_PAGE)


/*
 * Link to shared-memory data structures for DistributedTransactionId control
 */
static SlruCtlData DistributedXidMapCtlData;

#define DistributedXidMapCtl  (&DistributedXidMapCtlData)

/*
 * We initialize the highest page to 0 since we allocate the first page
 * when starting.
 */
static int DistributedXidMapHighestPageNo = 0;

static int	ZeroDistributedXidMapPage(int pageno);
static bool DistributedXidMapPagePrecedes(int page1, int page2);


/*
 * Get the local XID associated with a distributed transaction.  We will
 * allocate the local XID and assign its value in the map if necessary.
 */
void
AllocOrGetLocalXidForDistributedTransaction(DistributedTransactionId gxid, TransactionId *xid)
{
	int			pageno = DistributedTransactionIdToPage(gxid);
	int			entryno = DistributedTransactionIdToEntry(gxid);
	int			newpageno;
	int			slotno;
	TransactionId *ptr;

	Assert(gxid != InvalidDistributedTransactionId);
	Assert(xid != NULL);

	elog(DEBUG5, "Entering AllocOrGetLocalXidForDistributedTransaction with distributed xid = %d (pageno = %d, entryno = %d, DistributedXidMapHighestPageNo = %d)",
		 gxid, pageno, entryno, DistributedXidMapHighestPageNo);
	
	LWLockAcquire(DistributedXidMapControlLock, LW_EXCLUSIVE);

	if (pageno > DistributedXidMapHighestPageNo)
	{
		/*
		 * Zero out the new page(s).
		 */
		for (newpageno = DistributedXidMapHighestPageNo + 1; newpageno <= pageno; newpageno++)
		{
			elog(DEBUG5, "AllocOrGetLocalXidForDistributedTransaction zeroing out page = %d", newpageno);
			
			/* Zero the page */
			ZeroDistributedXidMapPage(newpageno);
		}

		DistributedXidMapHighestPageNo = pageno;
	}

	slotno = SimpleLruReadPage(DistributedXidMapCtl, pageno, InvalidTransactionId);
	ptr = (TransactionId *) DistributedXidMapCtl->shared->page_buffer[slotno];
	ptr += entryno;

	if(*ptr == InvalidTransactionId)
	{
		/*
		 * Need to allocate a local XID and assign the map entry.
		 */
		*xid = GetNewTransactionId(false, false);	// NOT subtrans, DO NOT Set PROC struct xid
		*ptr = *xid;
		
		elog(DEBUG5, "AllocOrGetLocalXidForDistributedTransaction allocated local XID = %d", *xid);

		DistributedXidMapCtl->shared->page_dirty[slotno] = true;
	}
	else
	{
		/*
		 * The local XID was pre-allocated by another QE when it
		 * received a distributed snapshot that listed the distributed transaction
		 * in its in-doubt list.
		 */
		*xid = *ptr;
		elog(DEBUG5, "AllocOrGetLocalXidForDistributedTransaction found pre-allocated local XID = %d", *xid);
	}

	SetProcXid(*xid, gxid);

	LWLockRelease(DistributedXidMapControlLock);
}

/*
 * Add local XID for any new distributed transactions.
 */
void
AddLocalXidsForDistributedTransactions(DistributedTransactionId *gxidArray, uint32 count)
{
	int							i;
	DistributedTransactionId 	gxid;
	int							pageno;
	int							entryno;
	int							slotno;
	TransactionId 				*ptr;

	LWLockAcquire(DistributedXidMapControlLock, LW_EXCLUSIVE);
	
	for (i = 0; i < count; i++)
	{
		gxid = gxidArray[i];
		
		pageno = DistributedTransactionIdToPage(gxid);
		entryno = DistributedTransactionIdToEntry(gxid);
		
		slotno = SimpleLruReadPage(DistributedXidMapCtl, pageno, InvalidTransactionId);
		ptr = (TransactionId *) DistributedXidMapCtl->shared->page_buffer[slotno];
		ptr += entryno;

		if (*ptr == InvalidTransactionId)
		{
			*ptr = GetNewTransactionId(false, false);	// NOT subtrans, DO NOT Set PROC struct xid
			DistributedXidMapCtl->shared->page_dirty[slotno] = true;
			elog(DEBUG5, "AddLocalXidsForDistributedTransactions: Allocated local XID = %u for global XID = %u",
				 *ptr, gxid);
		}
	}
	
	LWLockRelease(DistributedXidMapControlLock);
}

/*
 * Get the local XID associated with a distributed transaction.
 */
TransactionId
GetLocalXidForDistributedTransaction(DistributedTransactionId gxid)
{
	int			pageno = DistributedTransactionIdToPage(gxid);
	int			entryno = DistributedTransactionIdToEntry(gxid);
	int			slotno;
	TransactionId *ptr;
	TransactionId xid;

	elog(DEBUG5, "Entering GetLocalXidForDistributedTransaction with distributed xid = %d (pageno = %d, entryno = %d, DistributedXidMapHighestPageNo = %d)",
		 gxid, pageno, entryno, DistributedXidMapHighestPageNo);
	
	LWLockAcquire(DistributedXidMapControlLock, LW_EXCLUSIVE);

	if (pageno > DistributedXidMapHighestPageNo)
	{
		return InvalidTransactionId;
	}

	slotno = SimpleLruReadPage(DistributedXidMapCtl, pageno, InvalidTransactionId);
	ptr = (TransactionId *) DistributedXidMapCtl->shared->page_buffer[slotno];
	ptr += entryno;

	xid = *ptr;

	LWLockRelease(DistributedXidMapControlLock);

	return xid;
}


/*
 * Initialization of shared memory for DistributedXidMap
 */
Size
DistributedXidMapShmemSize(void)
{
	return SimpleLruShmemSize(NUM_DISTRIBUTEDXIDMAP_BUFFERS);
}

void
DistributedXidMapShmemInit(void)
{
	DistributedXidMapCtl->PagePrecedes = DistributedXidMapPagePrecedes;
	SimpleLruInit(DistributedXidMapCtl, "DistributedXidMap Ctl", NUM_DISTRIBUTEDXIDMAP_BUFFERS,
				  DistributedXidMapControlLock, "pg_distributedxidmap");
	/* Override default assumption that writes should be fsync'd */
	DistributedXidMapCtl->do_fsync = false;
}

/*
 * This func must be called ONCE on system install.  It verifies
 * the DistributedXidMap directory exists.  (The DistributedXidMap directory is assumed to
 * have been created by the initdb shell script, and DistributedXidMapShmemInit
 * must have been called already.)
 */
void
BootStrapDistributedXidMap(void)
{
	LWLockAcquire(DistributedXidMapControlLock, LW_EXCLUSIVE);

//  SimpleLruVerifyDirExists(DistributedXidMapCtl);

	LWLockRelease(DistributedXidMapControlLock);
}

/*
 * Initialize (or reinitialize) a page of DistributedXidMap to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroDistributedXidMapPage(int pageno)
{
	return SimpleLruZeroPage(DistributedXidMapCtl, pageno);
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized ShmemVariableCache->nextXid.
 *
 * oldestActiveXID is the oldest XID of any prepared transaction, or nextXid
 * if there are none.
 */
void
StartupDistributedXidMap(void)
{
	/*
	 * Since we don't expect pg_distributedxidmap to be valid across crashes, we
	 * delete all segments.
	 */
	LWLockAcquire(DistributedXidMapControlLock, LW_EXCLUSIVE);

//  SimpleLruDeleteAllSegments(DistributedXidMapCtl);

	/* Zero the first page */
	ZeroDistributedXidMapPage(0);

	LWLockRelease(DistributedXidMapControlLock);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownDistributedXidMap(void)
{
	/*
	 * Flush dirty DistributedXidMap pages to disk
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely as a debugging aid.
	 */
	SimpleLruFlush(DistributedXidMapCtl, false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointDistributedXidMap(void)
{
	/*
	 * Flush dirty DistributedXidMap pages to disk
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely to improve the odds that writing of dirty pages is done by
	 * the checkpoint process and not by backends.
	 */
	SimpleLruFlush(DistributedXidMapCtl, true);
}


/*
 * Remove all DistributedXidMap segments before the one holding the passed
 * DTM Global XID.
 *
 * This is normally called when a distributed snapshot arrives indicating the
 * xmin of all current distributed snapshots has moved forward.
 */
void
TruncateDistributedXidMap(DistributedTransactionId oldestXact)
{
	int			cutoffPage;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoffPage = DistributedTransactionIdToPage(oldestXact);

	// UNDONE: Save cutoff page and don't call SimpleLruTruncate when there
	// is no change to the cuttoff page. 

	SimpleLruTruncate(DistributedXidMapCtl, cutoffPage);
}


/*
 * Decide which of two DistributedXidMap page numbers is "older" for truncation
 * purposes.
 *
 */
static bool
DistributedXidMapPagePrecedes(int page1, int page2)
{
	return (page1 < page2);
}


