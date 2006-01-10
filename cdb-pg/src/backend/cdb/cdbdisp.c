/*-------------------------------------------------------------------------
 *
 * cdbdisp.c
 *	  Functions to dispatch commands to QExecutors
 *
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <pthread.h>

#include "executor/execdesc.h"	/* QueryDesc */
#include "storage/ipc.h"		/* For proc_exit_inprogress  */
#include "miscadmin.h"
#include "utils/memutils.h"

#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "utils/datum.h"
#include "executor/executor.h"
#include "optimizer/clauses.h"
#include "cdb/cdbplan.h"

#include "cdb/cdbdisp.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"
#include "cdb/cdblink.h"		/* just for our CdbProcess population hack. */
#include "cdb/cdbsrlz.h"
#include "cdb/cdbsubplan.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbtm.h"


/*
 * Parameter structure for the DispatchCommand threads
 */
typedef struct DispatchCommandParms
{
	/*
	 * The SQL command
	 */
	char	   *szCommand;

	/*
	 * db_count: The number of segdbs that this thread is responsible
	 * for dispatching the command to.
	 * Equals the count of segdbDescPtrArray below.
	 */
	int			db_count;

	/*
	 * dispatchResultPtrArray: Array[0..db_count-1] of CdbDispatchResult*
	 * Each CdbDispatchResult object points to a SegmentDatabaseDescriptor
	 * that this thread is responsible for dispatching the command to.
	 */
	CdbDispatchResult **dispatchResultPtrArray;

	/*
	 * The pthread_t thread handle.
	 */
	pthread_t	thread;

}	DispatchCommandParms;


/*
 * Static Helper functions
 */
static void *thread_DispatchCommand(void *arg);


static bool
			shouldStillDispatchCommand(CdbDispatchResult * dispatchResult);

static
void
dispatchCommand(CdbDispatchResult * dispatchResult,
				const char *szCommand);

static void handleSelectError(DispatchCommandParms * pParms,
				  int db_count,
				  int sock_errno);

static void handleSelectTimeout(DispatchCommandParms * pParms,
					int db_count,
					int *timeoutCounter);

static bool						/* returns true if command complete */
			processResults(CdbDispatchResult * dispatchResult);


static void addSegDBToDispatchThreadPool(DispatchCommandParms * ParmsAr,
							 int segdbs_in_thread_pool,
							 const char *szCommand,
							 CdbDispatchResult * dispatchResult);

static int fillSliceVector(SliceTable * sliceTable, int sliceIndex, Slice ** sliceVector,
				int len, int next);


typedef struct
{
	bool		single_row_insert;
	Node	   *nextval_replacement;
}	pre_dispatch_function_evaluation_context;

static Node *pre_dispatch_function_evaluation_mutator(Node *node,
						 pre_dispatch_function_evaluation_context * context);

/*
 * Static Variables.  These contain the thread count and array of
 * DispatchCommandParms structures after a call to CdbDispatchCommand.
 * They are used by CdbCheckDispatchResult to check thread termination and status.
 * Since postgres is single-threaded, this is OK.
 * We could have passed them back as out parameters from CdbDispatchCommand
 * and then passed them in to CdbCheckDispatchResult to avoid using static variables.
 * But keeping them static seems fine for now.
 *
 */
static DispatchCommandParms *dispatchCommandParmsAr = NULL;
static int	dispatchCommandParmsArSize = 0;
static int	threadCount = 0;
static volatile bool pleaseCancel = false;


/*--------------------------------------------------------------------*/


/* Start a global transaction and invoke FTS reconfiguration if needed. */
void
cdbdisp_startDtxTransaction(void)
{
	/*
	 * check fts sync count
	 */
	if (!verifyFtsSyncCount())
	{
		/*
		 * reconfig mpp connections
		 */
		if (!FtsReConfigureMPP())
			elog(PANIC, "FTS: reconfigMPP failed");
	}

	/*
	 * need to start distributed transaction
	 */
	condStartDtxTransaction();

}	/* cdbdisp_startDtxTransaction */



/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchToGang:
 * Send the szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * The caller must provide a CdbDispatchResults object having available
 * resultArray slots sufficient for the number of QEs to be dispatched:
 * i.e., resultCapacity - resultCount >= gp->size.	This function will
 * assign one resultArray slot per QE of the Gang, paralleling the Gang's
 * db_descriptors array.  Success or failure of each QE will be noted in
 * the QE's CdbDispatchResult entry; but before examining the results, the
 * caller must wait for execution to end by calling CdbCheckDispatchResult().
 *
 * The CdbDispatchResults object owns some malloc'ed storage, so the caller
 * must make certain to free it by calling cdbdisp_destroyDispatchResults().
 *
 * When dispatchResults->cancelOnError is false, szCommand is to be
 * dispatched to every connected gang member if possible, despite any
 * cancellation requests, QE errors, connection failures, etc.
 *
 * NB: This function should return normally even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 */
void
cdbdisp_dispatchToGang(const char *szCommand,
					   struct Gang * gp,
					   struct CdbDispatchResults * dispatchResults,
					   int sliceIndex)
{
	SegmentDatabaseDescriptor *segdbDesc;
	int			i;
	int			max_threads;
	int			segdbs_in_thread_pool = 0;
	int			x;
	int			newThreads = 0;

	Assert(Mpp_role == MPP_ROLE_DISPATCH &&
		   gp &&
		   gp->size > 0 &&
		   dispatchResults &&
		   dispatchResults->resultArray);

	if (DEBUG2 >= log_min_messages)
	{
		if (sliceIndex >= 0)
			elog(DEBUG2, "dispatchToGang: sliceIndex=%d gangsize=%d  %.100s",
				 sliceIndex, gp->size, szCommand);
		else
			elog(DEBUG2, "dispatchToGang: gangsize=%d  %.100s",
				 gp->size, szCommand);
	}

	/*
	 * The most threads we could have is segdb_count / mpp_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / mpp_connections_per_thread.
	 * We allocate enough memory for this many DispoatchCommandParms structures,
	 * even though we may not use them all.
	 */
	Assert(mpp_connections_per_thread > 0);

	segdbs_in_thread_pool = 0;
	max_threads = 1 + (gp->size - 1) / mpp_connections_per_thread;

	{
		MemoryContext oldContext = MemoryContextSwitchTo(TopMemoryContext);

		if (threadCount == 0)
		{
			dispatchCommandParmsAr =
				(DispatchCommandParms *) palloc0((max_threads) * 20 *
											   sizeof(DispatchCommandParms));
			dispatchCommandParmsArSize = max_threads * 20;
		}
		else
		{
			if (dispatchCommandParmsArSize < (threadCount + max_threads))
				elog(LOG, "BUG: Reallocating dispatchCommandParmsAr");

			dispatchCommandParmsAr =
				(DispatchCommandParms *) repalloc(dispatchCommandParmsAr, (threadCount + max_threads) *
											   sizeof(DispatchCommandParms));
			memset(dispatchCommandParmsAr + threadCount, 0, max_threads * sizeof(DispatchCommandParms));
		}

		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * Create the thread parms structures based targetSet parameter.
	 * This will add the segdbDesc pointers appropriate to the
	 * targetSet into the thread Parms structures, making sure that each thread
	 * handles mpp_connections_per_thread segdbs.
	 */
	for (x = 0; x < gp->size; x++)
	{
		CdbDispatchResult *qeResult;

		segdbDesc = &gp->db_descriptors[x];

		/* Initialize the QE's CdbDispatchResult object. */
		qeResult = cdbdisp_makeResult(dispatchResults, segdbDesc, sliceIndex);

		/* Transfer any connection errors from segdbDesc. */
		if (segdbDesc->errcode ||
			segdbDesc->error_message.len)
			cdbdisp_mergeConnectionErrors(qeResult, segdbDesc);

		addSegDBToDispatchThreadPool(dispatchCommandParmsAr + threadCount,
									 x,
									 szCommand,
									 qeResult);

		/*
		 * This CdbDispatchResult/SegmentDatabaseDescriptor pair will be
		 * dispatched and monitored by a thread to be started below. Only that
		 * thread should touch them until the thread is finished with them and
		 * resets the stillRunning flag. Caller must CdbCheckDispatchResult()
		 * to wait for completion.
		 */
		qeResult->stillRunning = true;
	}
	segdbs_in_thread_pool = x;

	/*
	 * Compute the thread count based on how many segdbs were added into the
	 * thread pool, knowing that each thread handles mpp_connections_per_thread
	 * segdbs.
	 */
	newThreads += 1 + (segdbs_in_thread_pool - 1) / mpp_connections_per_thread;

	/*
	 * Create the threads. (which also starts the dispatching).
	 */
	for (i = 0; i < newThreads; i++)
	{
		DispatchCommandParms *pParms = &(dispatchCommandParmsAr + threadCount)[i];

		Assert(pParms != NULL);

		elog(DEBUG4, "dispatchToGang: Creating thread %d of %d for CommandDispatch",
			 i + 1, newThreads);

		if (0 != pthread_create(&pParms->thread, NULL, thread_DispatchCommand, pParms))
		{
			elog(FATAL, "dispatchToGang: Failed to create thread %d of %d",
				 i + 1, newThreads);
		}

	}

	threadCount += newThreads;
}	/* cdbdisp_dispatchToGang */


/*
 * addSegDBToDispatchThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to have commands dispatched to.
 * It figures out which thread will handle it, based on the setting of
 * mpp_connections_per_thread.
 */
void
addSegDBToDispatchThreadPool(DispatchCommandParms * ParmsAr,
							 int segdbs_in_thread_pool,
							 const char *szCommand,
							 CdbDispatchResult * dispatchResult)
{
	DispatchCommandParms *pParms;
	int			ParmsIndex;

	/*
	 * The proper index into the DispatchCommandParms array is computed, based on
	 * having mpp_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % mpp_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	ParmsIndex = segdbs_in_thread_pool / mpp_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];

	if (segdbs_in_thread_pool % mpp_connections_per_thread == 0)
	{
		pParms->szCommand = pstrdup(szCommand);
		pParms->dispatchResultPtrArray =
			(CdbDispatchResult **) palloc0(mpp_connections_per_thread *
										   sizeof(CdbDispatchResult *));
		memset(&pParms->thread, 0, sizeof(pthread_t));
		pParms->db_count = 0;
	}

	/*
	 * Just add to the end of the used portion of the dispatchResultPtrArray
	 * and bump the count of members
	 */
	pParms->dispatchResultPtrArray[pParms->db_count++] = dispatchResult;

}	/* addSegDBToDispatchThreadPool */


/*
 * CdbCheckDispatchResult:
 *
 * Waits for completion of threads launched by cdbdisp_dispatchToGang().
 *
 * If 'cancelUnfinishedWork' is true, QEs that were dispatched with
 * 'cancelOnError' true and are not yet idle will be canceled.
 */
void
CdbCheckDispatchResult(bool cancelUnfinishedWork)
{
	CdbCheckDispatchResultInt(NULL, NULL, cancelUnfinishedWork);
}

void
CdbCheckDispatchResultInt(struct SegmentDatabaseDescriptor *** failedSegDB,
						  int *numOfFailed,
						  bool cancelUnfinishedWork)
{
	int			i;
	int			j;
	int			nFailed = 0;
	DispatchCommandParms *pParms;
	CdbDispatchResult *dispatchResult;
	SegmentDatabaseDescriptor *segdbDesc;

	if (failedSegDB)
		*failedSegDB = NULL;
	if (numOfFailed)
		*numOfFailed = 0;

	/* No-op if no work was dispatched since the last time we were called.	*/
	if (threadCount == 0)
	{
		elog(DEBUG5, "CheckDispatchResult: no threads active");
		return;
	}

	/* Does caller want to stop short? */
	if (cancelUnfinishedWork)
		pleaseCancel = true;

	/*
	 * Wait for threads to finish.
	 */
	for (i = 0; i < threadCount; i++)
	{							/* loop over threads */
		pParms = &dispatchCommandParmsAr[i];
		Assert(pParms != NULL);

		elog(DEBUG4, "CheckDispatchResult: Joining to thread %d of %d",
			 i + 1, threadCount);

		if (0 != pthread_join(pParms->thread, NULL))
			elog(FATAL, "CheckDispatchResult: pthread_join failed on thread %d of %d",
				 i + 1, threadCount);

		/*
		 * Examine the CdbDispatchResult objects containing the results
		 * from this thread's QEs.
		 */
		for (j = 0; j < pParms->db_count; j++)
		{						/* loop over QEs managed by one thread */
			dispatchResult = pParms->dispatchResultPtrArray[j];

			Assert(dispatchResult && dispatchResult->segdbDesc);

			segdbDesc = dispatchResult->segdbDesc;

			/* segdbDesc error message is unlikely here, but check anyway. */
			if (segdbDesc->errcode ||
				segdbDesc->error_message.len)
				cdbdisp_mergeConnectionErrors(dispatchResult, segdbDesc);

			/* Log the result */
			if (DEBUG2 >= log_min_messages)
				cdbdisp_debugDispatchResult(dispatchResult, DEBUG2, DEBUG3);

			/* Notify FTS to reconnect if connection lost or never connected. */
			if (failedSegDB &&
				PQstatus(segdbDesc->conn) == CONNECTION_BAD)
			{
				/* Allocate storage.  Caller should pfree() it. */
				if (!*failedSegDB)
					*failedSegDB = palloc(sizeof(**failedSegDB) *
										  (2 * getMPPSegmentCount() + 1));

				/* Append to broken connection list. */
				(*failedSegDB)[nFailed++] = segdbDesc;
				(*failedSegDB)[nFailed] = NULL;

				if (numOfFailed)
					*numOfFailed = nFailed;
			}

			/*
			 * Zap our SegmentDatabaseDescriptor ptr because it may be
			 * invalidated by the call to FtsHandleNetFailure() below.
			 * Anything we need from there, we should get before this.
			 */
			dispatchResult->segdbDesc = NULL;

		}						/* loop over QEs managed by one thread */
	}							/* loop over threads */

	/*
	 * pfree the memory allocated for the dispatchCommandParmsAr
	 */
	if (dispatchCommandParmsAr != NULL)
	{
		for (i = 0; i < threadCount; i++)
		{
			pParms = &dispatchCommandParmsAr[i];
			if (pParms->dispatchResultPtrArray)
			{
				pfree(pParms->dispatchResultPtrArray);
				pParms->dispatchResultPtrArray = NULL;
			}
			if (pParms->szCommand)
			{
				pfree(pParms->szCommand);
				pParms->szCommand = NULL;
			}
		}
		pfree(dispatchCommandParmsAr);
		dispatchCommandParmsAr = NULL;
	}

	/* Reset dispatcher state. */
	pleaseCancel = false;
	threadCount = 0;
}	/* cdbdisp_checkDispatchResults */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchRMCommand:
 * Sends a non-cancelable command to all segment dbs, primary and mirror.
 *
 * Returns a malloc'ed array containing the PGresult objects thus
 * produced; the caller must PQclear() them and free() the array.
 * A NULL entry follows the last used entry in the array.
 *
 * Any error messages - whether or not they are associated with
 * PGresult objects - are appended to a StringInfo buffer provided
 * by the caller.
 */
struct pg_result **				/* returns ptr to array of PGresult ptrs */
cdbdisp_dispatchRMCommand(const char *szCommand,
						  StringInfo errmsgbuf)
{
	CdbDispatchResults *gangResults;
	CdbDispatchResults *primaryResults;
	CdbDispatchResults *mirrorResults;
	CdbDispatchResult *dispatchResult;
	PGresult  **resultSets = NULL;
	int			nslots;
	int			nresults = 0;
	int			i;

	/* Launch the command.	Don't cancel on error. */
	cdbdisp_dispatchCommand(szCommand, false, &primaryResults, &mirrorResults);

	/* Wait for all QEs to finish.	Don't cancel. */
	CdbCheckDispatchResult(false);

	/*
	 * Allocate result set ptr array. Make room for one PGresult ptr per
	 * primary or mirror segment db, plus a null terminator slot after the
	 * last entry. The caller must PQclear() each PGresult and free() the
	 * array.
	 */
	nslots = 2 * getMPPSegmentCount() + 1;
	resultSets = (struct pg_result **) calloc(nslots, sizeof(*resultSets));

	/* Collect results from primary gang. */
	gangResults = primaryResults;
	if (gangResults)
	{
		for (i = 0; i < gangResults->resultCount; ++i)
		{
			dispatchResult = &gangResults->resultArray[i];

			/* Append error messages to caller's buffer. */
			cdbdisp_dumpDispatchResult(dispatchResult, false, errmsgbuf);

			/* Take ownership of this QE's PGresult object(s). */
			nresults += cdbdisp_snatchPGresults(dispatchResult,
												resultSets + nresults,
												nslots - nresults - 1);
		}
		cdbdisp_destroyDispatchResults(gangResults);
	}

	/* Collect results from mirror gang. */
	gangResults = mirrorResults;
	if (gangResults)
	{
		for (i = 0; i < gangResults->resultCount; ++i)
		{
			dispatchResult = &gangResults->resultArray[i];

			/* Append error messages to caller's buffer. */
			cdbdisp_dumpDispatchResult(dispatchResult, false, errmsgbuf);

			/* Take ownership of this QE's PGresult object(s). */
			nresults += cdbdisp_snatchPGresults(dispatchResult,
												resultSets + nresults,
												nslots - nresults - 1);
		}
		cdbdisp_destroyDispatchResults(gangResults);
	}

	/* Put a stopper at the end of the array. */
	Assert(nresults < nslots);
	resultSets[nresults] = NULL;

	return resultSets;
}	/* cdbdisp_dispatchRMCommand */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchCommand:
 * Send the szCommand SQL statement to all segdbs in the cluster
 * cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * The CdbDispatchResults objects allocated for the command
 * are returned in *pPrimaryResults and *pMirrorResults.
 * The caller, after calling CdbCheckDispatchResult(), can
 * examine the CdbDispatchResults objects, can keep them as
 * long as needed, and ultimately must free them with
 * cdbdisp_destroyDispatchResults() prior to deallocation
 * of the memory context from which they were allocated.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchCommand(const char *szCommand,
						bool cancelOnError,
						struct CdbDispatchResults ** pPrimaryResults,	/* OUT */
						struct CdbDispatchResults ** pMirrorResults)	/* OUT */
{
	Gang	   *primaryGang;
	Gang	   *mirrorGang;
	int			nsegdb = getMPPSegmentCount();

	elog(DEBUG3, "cdbdisp_dispatchCommand: %.50s", szCommand);

	*pPrimaryResults = NULL;
	if (pMirrorResults)
		*pMirrorResults = NULL;

	/*
	 * Allocate a primary and mirror QE for every available segDB in the system.
	 */
	primaryGang = allocateGang(GANGTYPE_PRIMARY_WRITER, nsegdb, -1);

	Assert(primaryGang);

	mirrorGang = allocateGang(GANGTYPE_MIRROR_WRITER, nsegdb, -1);

	if (mirrorGang &&
		(mirrorGang->segdb_count < nsegdb || mirrorGang->size < nsegdb))
	{
		elog(LOG, "Some segments are down: MIRROR nsegdb %d mirrrorGang->segdb_count: %d mirrorGang->size: %d", nsegdb, mirrorGang->segdb_count, mirrorGang->size);
		elog(LOG, "                        PRIMARYnsegdb %d mirrrorGang->segdb_count: %d mirrorGang->size: %d", nsegdb, primaryGang->segdb_count, primaryGang->size);
		elog(LOG, "Some segments are down.  Dispatching command %.50s to remaining segments only", szCommand);

		if (strncmp(szCommand, "BEGIN ", 6) != 0 &&
			strncmp(szCommand, "PREPARE ", 8) != 0 &&
			strncmp(szCommand, "COMMIT ", 6) != 0 &&
			strncmp(szCommand, "ROLLBACK ", 9) != 0)
		{
			if (strncmp(szCommand, "MPPEXEC ", 8) == 0 || strncmp(szCommand, "mppexec ", 8) == 0)
				elog(NOTICE, "Some segments are down.  Dispatching command to remaining segments only");
			else
				elog(NOTICE, "Some segments are down.  Dispatching command %.50s to remaining segments only", szCommand);
		}
	}

	/*
	 * Dispatch the command.
	 */
	*pPrimaryResults = cdbdisp_makeDispatchResults(nsegdb, 0, cancelOnError);
	cdbdisp_dispatchToGang(szCommand, primaryGang, *pPrimaryResults, -1);

	if (mirrorGang)
	{
		*pMirrorResults = cdbdisp_makeDispatchResults(nsegdb, 0, cancelOnError);
		cdbdisp_dispatchToGang(szCommand, mirrorGang, *pMirrorResults, -1);
	}

}	/* cdbdisp_dispatchCommand */


/* Wait for all QEs to finish, then report any errors from the given
 * CdbDispatchResults objects and free them.  If not all QEs in the
 * associated gang(s) executed the command successfully, throws an
 * error and does not return.  No-op if both CdbDispatchResults ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct CdbDispatchResults * primaryResults,
					  struct CdbDispatchResults * mirrorResults)
{
	StringInfoData buf;
	int			errorcode;

	/* If cdbdisp_dispatchToGang() wasn't called, don't wait. */
	if (!primaryResults &&
		!mirrorResults)
		return;

	/* Wait for all QEs to finish. Don't cancel them. */
	CdbCheckDispatchResult(false);

	/* If no errors, free the CdbDispatchResults objects and return. */
	errorcode = primaryResults->errcode;
	if (!errorcode && mirrorResults)
		errorcode = mirrorResults->errcode;

	if (!errorcode)
	{
		cdbdisp_destroyDispatchResults(primaryResults);
		cdbdisp_destroyDispatchResults(mirrorResults);
		return;
	}

	/* Format error messages from the primary gang. */
	initStringInfo(&buf);
	cdbdisp_dumpDispatchResults(primaryResults, &buf, false);

	/* Format mirror gang errors only if there were no primary errors. */
	if (buf.len == 0)
		cdbdisp_dumpDispatchResults(mirrorResults, &buf, false);

	cdbdisp_destroyDispatchResults(primaryResults);
	cdbdisp_destroyDispatchResults(mirrorResults);

	/* Too bad, our gang got an error. */
	PG_TRY();
	{
		ereport(ERROR, (errcode(errorcode),
						errmsg("%s", buf.data)));
	}
	PG_CATCH();
	{
		pfree(buf.data);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* not reached */
}	/* cdbdisp_finishCommand */


/*--------------------------------------------------------------------*/


/*
 * thread_DispatchCommand is the thread proc used to dispatch the command to one or more of the qExecs.
 *
 * NOTE: This function MUST NOT contain elog or ereport statements. (or most any other backend code)
 *		 elog is NOT thread-safe.  Developers should instead use something like:
 *
 *	if (DEBUG3 >= log_min_messages)
 *			fprintf(stderr,"my brilliant log statement here.\n");
 *
 * NOTE: In threads, we cannot use palloc, because it's not thread safe.
 */
void *
thread_DispatchCommand(void *arg)
{
	DispatchCommandParms *pParms = (DispatchCommandParms *) arg;
	int			db_count = pParms->db_count;
	SegmentDatabaseDescriptor *segdbDesc;
	CdbDispatchResult *dispatchResult;
	int			timeoutCounter = 0;
	int			i;

	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to send commands through to.
	 */
	for (i = 0; i < db_count; i++)
	{
		/* Don't use elog, it's not thread-safe */
		if (DEBUG5 >= log_min_messages)
			fprintf(stderr, "thread_DispatchCommand working on %d of %d commands\n", i + 1, db_count);

		dispatchResult = pParms->dispatchResultPtrArray[i];

		dispatchResult->hasDispatched = false;
		dispatchResult->requestedCancel = false;
		dispatchResult->wasCanceled = false;

		if (!shouldStillDispatchCommand(dispatchResult))
		{
			/* Don't dispatch if cancellation pending or no connection. */
			dispatchResult->stillRunning = false;
		}
		else
		{
			/*
			 * Kick off the command over the libpq connection. If
			 * unsuccessful, proceed anyway, and check for lost connection
			 * below.
			 */
			dispatchCommand(dispatchResult, pParms->szCommand);
			dispatchResult->hasDispatched = true;
		}
	}

	/*
	 * OK, we are finished submitting the command to the segdbs.
	 * Now, we have to wait for them to finish.
	 */
	for (;;)
	{							/* some QEs running */
		struct timeval tv;
		fd_set		input_mask;
		int			max_sock;
		int			sock;
		int			n;

		/*
		 * Which QEs are still running and could send results to us?
		 */
		max_sock = -1;
		FD_ZERO(&input_mask);

		for (i = 0; i < db_count; i++)
		{						/* loop to check connection status */
			dispatchResult = pParms->dispatchResultPtrArray[i];
			segdbDesc = dispatchResult->segdbDesc;

			/* Already finished with this QE? */
			if (!dispatchResult->stillRunning)
				continue;

			/* Add socket to fd_set if still connected. */
			sock = PQsocket(segdbDesc->conn);
			if (sock >= 0 &&
				PQstatus(segdbDesc->conn) != CONNECTION_BAD)
			{
				FD_SET(sock, &input_mask);
				max_sock = max_sock < sock ? sock : max_sock;
			}

			/* Lost the connection. */
			else
			{
				char	   *msg = PQerrorMessage(segdbDesc->conn);

				/* Save error info for later. */
				cdbdisp_appendMessage(dispatchResult, DEBUG1,
									  ERRCODE_MPP_INTERCONNECTION_ERROR,
									  "Lost connection to %s.  %s",
									  segdbDesc->whoami,
									  msg ? msg : "");

				/* Free the PGconn object. */
				PQfinish(segdbDesc->conn);
				segdbDesc->conn = NULL;

				dispatchResult->stillRunning = false;	/* he's dead, Jim */
			}
		}						/* loop to check connection status */

		/* Break out when no QEs still running. */
		if (max_sock < 0)
			break;

		/*
		 * Wait for results from QEs.
		 */

		/* Timeout should be reinitialized before each select(). */
		tv.tv_sec = 2;
		tv.tv_usec = 0;

		/* Block here until input is available. */
		n = select(max_sock + 1, &input_mask, NULL, NULL, &tv);

		if (n < 0)
		{
			int			sock_errno = SOCK_ERRNO;

			if (sock_errno == EINTR)
				continue;

			handleSelectError(pParms, db_count, sock_errno);
			return NULL;
		}

		if (n == 0)
		{
			handleSelectTimeout(pParms, db_count, &timeoutCounter);
			continue;
		}

		/*
		 * We have data waiting on one or more of the connections.
		 */
		for (i = 0; i < db_count; i++)
		{						/* input available; receive and process it */
			bool		finished;

			dispatchResult = pParms->dispatchResultPtrArray[i];
			segdbDesc = dispatchResult->segdbDesc;

			/* Skip if already finished or didn't dispatch. */
			if (!dispatchResult->stillRunning)
				continue;

			/* Skip this connection if it has no input available. */
			sock = PQsocket(segdbDesc->conn);
			if (sock >= 0 &&
				!FD_ISSET(sock, &input_mask))
				continue;

			/* Receive and process results from this QE. */
			finished = processResults(dispatchResult);

			/* Are we through with this QE now? */
			if (finished)
				dispatchResult->stillRunning = false;
		}						/* input available; receive and process it */

	}							/* some QEs running */

	if (DEBUG5 >= log_min_messages)
	{
		fprintf(stderr, "thread_DispatchCommand exit\n");
		fflush(stderr);
	}

	return (NULL);
}	/* thread_DispatchCommand */


/* Helper function to thread_DispatchCommand that decides if we should dispatch
 * to this segment database.
 *
 * (1) don't dispatch if there is already a query cancel notice pending.
 * (2) make sure our libpq connection is still good.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
bool
shouldStillDispatchCommand(CdbDispatchResult * dispatchResult)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	CdbDispatchResults *gangResults = dispatchResult->meleeResults;

	/* Don't dispatch to a QE that is not connected. */
	if (!segdbDesc->conn)
	{
		if (DEBUG3 >= log_min_messages)
		{
			fprintf(stderr, "Command not sent to %s; no connection.\n",
					segdbDesc->whoami);
			fflush(stderr);
		}
		return false;
	}

	/* Lost the connection? */
	if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
	{
		char	   *msg = PQerrorMessage(segdbDesc->conn);

		/* Save error info for later. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_MPP_INTERCONNECTION_ERROR,
							  "Lost connection to %s.  %s",
							  segdbDesc->whoami,
							  msg ? msg : "");

		/* Free the PGconn object at once whenever we notice it's gone bad. */
		PQfinish(segdbDesc->conn);
		segdbDesc->conn = NULL;

		return false;
	}

	/*
	 * Don't submit if already encountered an error. The error has already
	 * been noted, so just keep quiet.
	 */
	if (pleaseCancel ||
		gangResults->errcode)
	{
		if (gangResults->cancelOnError)
		{
			dispatchResult->wasCanceled = true;
			if (DEBUG4 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				fprintf(stderr, "Error cleanup in progress; command not sent to %s\n",
						segdbDesc->whoami);
				fflush(stderr);
			}
			return false;
		}
	}

	/*
	 * Don't submit if client told us to cancel. The cancellation request has
	 * already been noted, so hush.
	 */
	if (InterruptPending &&
		gangResults->cancelOnError)
	{
		dispatchResult->wasCanceled = true;
		if (DEBUG4 >= log_min_messages)
		{
			fprintf(stderr, "Cancellation request pending; command not sent to %s\n",
					segdbDesc->whoami);
			fflush(stderr);
		}
		return false;
	}

	return true;
}	/* shouldStillDispatchCommand */


/* Helper function to thread_DispatchCommand that actually kicks off the
 * command on the libpq connection.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
void
dispatchCommand(CdbDispatchResult * dispatchResult,
				const char *szCommand)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	PGconn	   *conn = segdbDesc->conn;

	/* Don't use elog, it's not thread-safe */
	if (DEBUG3 >= log_min_messages)
		fprintf(stderr, "%s <- %.120s\n",
				segdbDesc->whoami,
				szCommand);

	/*
	 * Submit the command asynchronously.
	 */
	if (0 == PQsendQuery(conn, szCommand))
	{
		/* Note the error. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_MPP_INTERCONNECTION_ERROR,
						   "Command could not be sent to segment db %s;  %s",
							  segdbDesc->whoami,
							  PQerrorMessage(conn));
	}

	/*
	 * We'll keep monitoring this QE -- whether or not the command
	 * was dispatched -- in order to check for a lost connection
	 * or any other errors that libpq might have in store for us.
	 */
}	/* dispatchCommand */


/* Helper function to thread_DispatchCommand that handles errors that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
void
handleSelectError(DispatchCommandParms * pParms,
				  int db_count,
				  int sock_errno)
{
	int			i;

	if (LOG >= log_min_messages)
	{
		/* Don't use elog, it's not thread-safe */
		fprintf(stderr, "thread_DispatchCommand select() failed; errno=%d\n",
				sock_errno);
		fflush(stderr);
	}

	/*
	 * Based on the select man page, we could get here with
	 * errno == EBADF (bad descriptor), EINVAL (highest descriptor negative or negative timeout)
	 * or ENOMEM (out of memory).
	 * Since this is most likely a programming error or a bad system
	 * failure, we will just error out all remaining threads.
	 */

	for (i = 0; i < db_count; i++)
	{
		CdbDispatchResult *dispatchResult = pParms->dispatchResultPtrArray[i];

		/* Skip if already finished or didn't dispatch. */
		if (!dispatchResult->stillRunning)
			continue;

		/* Note the error. */
		cdbdisp_appendMessage(dispatchResult, 0,
							  ERRCODE_MPP_INTERCONNECTION_ERROR,
							  "System error... select() errno=%d\n",
							  sock_errno);

		/* We're done with this QE, sadly. */
		dispatchResult->stillRunning = false;
	}

	/* No point in trying to cancel the other QEs with select() broken. */
}	/* handleSelectError */


/* Helper function to thread_DispatchCommand that handles timeouts that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *		 thread_DispatchCommand absolutely no elog'ing.
 */
void
handleSelectTimeout(DispatchCommandParms * pParms,
					int db_count,
					int *timeoutCounter)
{
	CdbDispatchResult *dispatchResult;
	CdbDispatchResults *meleeResults;
	SegmentDatabaseDescriptor *segdbDesc;
	int			i;

	/*
	 * Are there any QEs that should be canceled?
	 *
	 * CDB TODO: PQrequestCancel() is expensive, and we do them
	 *			 serially.	Just do a few each time; save some
	 *			 for the next timeout.
	 */
	for (i = 0; i < db_count; i++)
	{							/* loop to check connection status */
		dispatchResult = pParms->dispatchResultPtrArray[i];
		segdbDesc = dispatchResult->segdbDesc;
		meleeResults = dispatchResult->meleeResults;

		/* Already finished with this QE? */
		if (!dispatchResult->stillRunning)
			continue;

		/*
		 * Send cancellation request to this QE if an error has been reported
		 * by another QE in the gang; or if a cancellation request is pending
		 * here on the QD. Don't cancel if requestor said not to, if no longer
		 * connected, or if we have already sent a cancellation request.
		 */
		if (pleaseCancel ||
			InterruptPending ||
			meleeResults->errcode)
		{
			if (meleeResults->cancelOnError &&
				!dispatchResult->requestedCancel &&
				!dispatchResult->wasCanceled &&
				segdbDesc->conn &&
				PQstatus(segdbDesc->conn) != CONNECTION_BAD)
			{
				if (DEBUG4 >= log_min_messages)
				{
					fprintf(stderr, "Calling PQrequestCancel for %s\n",
							segdbDesc->whoami);
					fflush(stderr);
				}

				/*
				 * Block here until segment postmaster replies that it has
				 * carried out the cancellation request, which means signaling
				 * SIGINT to the QE process, something like: pid =
				 * PQbackendPID(segdbDesc->conn); kill(pid, SIGINT);
				 */
				dispatchResult->requestedCancel = true;
				PQrequestCancel(segdbDesc->conn);
			}
		}
	}

	/*
	 * check the connection still valid, set 1 min time interval
	  * this may affect performance, should turn it off if required.
	 */

	if ((*timeoutCounter)++ > 30)
	{
		bool		sampling = true;

		*timeoutCounter = 0;

		for (i = 0; i < db_count; i++)
		{
			dispatchResult = pParms->dispatchResultPtrArray[i];
			segdbDesc = dispatchResult->segdbDesc;

			/* Skip if already finished or didn't dispatch. */
			if (!dispatchResult->stillRunning)
				continue;

			if (!FtsTestConnection(segdbDesc->segment_database_info, segdbDesc->dbname))
			{
				/* Note the error. */
				cdbdisp_appendMessage(dispatchResult, DEBUG1,
									  ERRCODE_MPP_INTERCONNECTION_ERROR,
									  "Segment db no longer connected: %s\n",
									  segdbDesc->whoami);

				/*
				 * Not a good idea to store into the PGconn object. Instead,
				 * just close it.
				 */
				PQfinish(segdbDesc->conn);
				segdbDesc->conn = NULL;

				/* This connection is hosed. */
				dispatchResult->stillRunning = false;

				/* Better check all the others too. */
				sampling = false;
			}
			else
			{
				/*
				 * for performance reason, we do just sampling here for now,
				 * the first one is picked.
				 */
				if (sampling)
					break;
			}
		}
	}

}	/* handleSelectTimeout */


bool							/* returns true if command complete */
processResults(CdbDispatchResult * dispatchResult)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	char	   *errorMessage;
	int			rc;

	/* Receive input from QE. */
	rc = PQconsumeInput(segdbDesc->conn);

	/* If PQconsumeInput fails, we're hosed. */
	if (rc == 0)
	{							/* handle PQconsumeInput error */
		errorMessage = PQerrorMessage(segdbDesc->conn);

		/* Save error info for later. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_MPP_INTERCONNECTION_ERROR,
							  "Error on receive from %s: %s",
							  segdbDesc->whoami,
							  errorMessage ? errorMessage : "unknown error");

		/* Can't recover, so drop the connection. */
		PQfinish(segdbDesc->conn);
		segdbDesc->conn = NULL;

		return true;			/* done; stick a fork in it */
	}							/* handle PQconsumeInput error */

	/* If we have received one or more complete messages, process them. */
	while (!PQisBusy(segdbDesc->conn))
	{							/* loop to call PQgetResult; won't block */
		PGresult   *pRes;
		ExecStatusType resultStatus;
		int			resultIndex = cdbdisp_numPGresult(dispatchResult);

		/* Get one message. */
		pRes = PQgetResult(segdbDesc->conn);

		/*
		 * Command is complete when PGgetResult() returns NULL. It is critical
		 * that for any connection that had an asynchronous command sent thru
		 * it, we call PQgetResult until it returns NULL. Otherwise, the next
		 * time a command is sent to that connection, it will return an error
		 * that there's a command pending.
		 */
		if (!pRes)
		{						/* end of results */
			if (DEBUG4 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				fprintf(stderr, "%s -> idle\n", segdbDesc->whoami);
				fflush(stderr);
			}
			return true;		/* this is normal end of command */
		}						/* end of results */

		/* Attach the PGresult object to the CdbDispatchResult object. */
		cdbdisp_appendResult(dispatchResult, pRes);

		/* Did a command complete successfully? */
		resultStatus = PQresultStatus(pRes);
		if (resultStatus == PGRES_COMMAND_OK ||
			resultStatus == PGRES_TUPLES_OK)
		{						/* QE reported success */

			/*
			 * Save the index of the last successful PGresult. Can be given to
			 * cdbdisp_getPGresult() to get tuple count, etc.
			 */
			dispatchResult->okindex = resultIndex;

			if (DEBUG3 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				char	   *cmdStatus = PQcmdStatus(pRes);

				fprintf(stderr, "%s -> ok %s\n",
						segdbDesc->whoami,
						cmdStatus ? cmdStatus : "(no cmdStatus)");
				fflush(stderr);
			}
		}						/* QE reported success */

		/* Note QE error.  Cancel the whole statement if requested. */
		else
		{						/* QE reported an error */
			char	   *sqlstate = PQresultErrorField(pRes, PG_DIAG_SQLSTATE);
			int			errcode = 0;

			errorMessage = PQresultErrorMessage(pRes);

			if (DEBUG2 >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				fprintf(stderr, "%s -> %s %s  %s",
						segdbDesc->whoami,
						PQresStatus(resultStatus),
						sqlstate ? sqlstate : "(no SQLSTATE)",
						errorMessage ? errorMessage : "\n");
				fflush(stderr);
			}

			/*
			 * Convert SQLSTATE to an error code (ERRCODE_xxx). Use a generic
			 * nonzero error code if no SQLSTATE.
			 */
			if (sqlstate &&
				strlen(sqlstate) == 5)
				errcode = cdbdisp_sqlstate_to_errcode(sqlstate);

			/*
			 * Save first error code and the index of its PGresult buffer
			 * entry.
			 */
			cdbdisp_seterrcode(errcode, resultIndex, dispatchResult);
		}						/* QE reported an error */
	}							/* loop to call PQgetResult; won't block */

	return false;				/* we must keep on monitoring this socket */
}	/* processResults */


/*--------------------------------------------------------------------*/


/*
 * CdbDoCommandNoTxn:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommandNoTxn(const char *szCommand,
				  bool cancelOnError)
{
	CdbDispatchResults *primaryResults;
	CdbDispatchResults *mirrorResults;

	cdbdisp_dispatchCommand(szCommand, cancelOnError,
							&primaryResults, &mirrorResults);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(primaryResults, mirrorResults);

}	/* CdbDoCommandNoTxn */


/*
 * CdbDoCommand:
 * Called by general users, this method includes global transaction control.
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommand(const char *szCommand, bool cancelOnError)
{
	/* Start a global transaction and reconfigure the cluster if needed. */
	cdbdisp_startDtxTransaction();

	/* Dispatch the command, wait for it to finish, and check the results. */
	CdbDoCommandNoTxn(szCommand, cancelOnError);
}	/* CdbDoCommand */


/*--------------------------------------------------------------------*/



/*
 * Let's evaluate all STABLE functions that have constant args before dispatch, so we get a consistent
 * view across QEs
 *
 * Also, if this is a single_row insert, let's evaluate nextval() and currval() before dispatching
 *
 */

static Node *
pre_dispatch_function_evaluation_mutator(Node *node,
						  pre_dispatch_function_evaluation_context * context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		/* Not replaceable, so just copy the Param (no need to recurse) */
		return (Node *) copyObject(param);
	}
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;
		List	   *args;
		ListCell   *arg;
		Expr	   *simple;
		FuncExpr   *newexpr;
		bool		has_nonconst_input;

		Form_pg_proc funcform;
		EState	   *estate;
		ExprState  *exprstate;
		MemoryContext oldcontext;
		Datum		const_val;
		bool		const_is_null;
		int16		resultTypLen;
		bool		resultTypByVal;

		Oid			funcid;
		HeapTuple	func_tuple;

		/*
		 * Reduce constants in the FuncExpr's arguments.  We know args is
		 * either NIL or a List node, so we can call plan_tree_mutator
		 * directly rather than recursing to self.
		 */
		args = (List *) plan_tree_mutator((Node *) expr->args,
									pre_dispatch_function_evaluation_mutator,
										  (void *) context);

		funcid = expr->funcid;

		/*
		 * Check for constant inputs
		 */
		foreach(arg, args)
		{
			if (!IsA(lfirst(arg), Const))
				has_nonconst_input = true;
		}


		if (!has_nonconst_input)
		{

			func_tuple = SearchSysCache(PROCOID,
										ObjectIdGetDatum(funcid),
										0, 0, 0);
			if (!HeapTupleIsValid(func_tuple))
				elog(ERROR, "cache lookup failed for function %u", funcid);

			funcform = (Form_pg_proc) GETSTRUCT(func_tuple);

			/* if it returns a set, it's not a function we can handle */
			/* if it returns a record, it's not a function we can handle */
			if (funcform->proretset || funcform->prorettype == RECORDOID)
			{
				ReleaseSysCache(func_tuple);
				return node;
			}



			if (funcform->provolatile == PROVOLATILE_IMMUTABLE)
				 /* okay */ ;
			else if (funcform->provolatile == PROVOLATILE_STABLE)
				 /* okay */ ;
			else if (context->single_row_insert && (funcid >= 1574 && funcid <= 1576))	/* nextval, currval,
																						 * setval */
				;				/* Volatile, but special sequence function */
			else
			{
				ReleaseSysCache(func_tuple);
				return node;
			}

			ReleaseSysCache(func_tuple);

			/*
			 * Ok, we have a function that is STABLE (or IMMUTABLE), with constant args.
			 * Let's try to evaluate it.
			 */


			/*
			 * The targetlist happens in multiple places in the plan tree, but we don't
			 * want to execute nextval() every time we see it.	Rather than figure out
			 * which one matters, and somehow passing that in to here, I cache the first time
			 * we run nextval(), and just re-use that value.
			 *
			 * This does have a bug, if we really wanted nextval() to be evaluated more than
			 * once in this single row insert.
			 */

			if (funcid == 1574 && context->nextval_replacement != NULL)
				return context->nextval_replacement;

			newexpr = makeNode(FuncExpr);
			newexpr->funcid = funcid;
			newexpr->funcresulttype = expr->funcresulttype;
			newexpr->funcretset = false;
			newexpr->funcformat = COERCE_DONTCARE;		/* doesn't matter */
			newexpr->args = args;

			/*
			 * To use the executor, we need an EState.
			 */
			estate = CreateExecutorState();

			/* We can use the estate's working context to avoid memory leaks. */
			oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

			/*
			 * Prepare expr for execution.
			 */
			exprstate = ExecPrepareExpr((Expr *) newexpr, estate);

			/*
			 * And evaluate it.
			 *
			 * It is OK to use a default econtext because none of the ExecEvalExpr()
			 * code used in this situation will use econtext.  That might seem
			 * fortuitous, but it's not so unreasonable --- a constant expression does
			 * not depend on context, by definition, n'est ce pas?
			 */
			const_val = ExecEvalExprSwitchContext(exprstate,
											  GetPerTupleExprContext(estate),
												  &const_is_null, NULL);

			/* Get info needed about result datatype */
			get_typlenbyval(expr->funcresulttype, &resultTypLen, &resultTypByVal);

			/* Get back to outer memory context */
			MemoryContextSwitchTo(oldcontext);


			/* Must copy result out of sub-context used by expression eval */
			if (!const_is_null)
				const_val = datumCopy(const_val, resultTypByVal, resultTypLen);

			/* Release all the junk we just created */
			FreeExecutorState(estate);

			/*
			 * Make the constant result node.
			 */
			simple = (Expr *) makeConst(expr->funcresulttype, resultTypLen,
										const_val, const_is_null,
										resultTypByVal);



			/*
			 * If we just executed nextval(), save the result
			 */
			if (funcid == 1574 && context->nextval_replacement == NULL && simple)
				context->nextval_replacement = (Node *) simple;

			if (simple)			/* successfully simplified it */
				return (Node *) simple;
		}

		/*
		 * The expression cannot be simplified any further, so build and
		 * return a replacement FuncExpr node using the possibly-simplified
		 * arguments.
		 */
		newexpr = makeNode(FuncExpr);
		newexpr->funcid = expr->funcid;
		newexpr->funcresulttype = expr->funcresulttype;
		newexpr->funcretset = expr->funcretset;
		newexpr->funcformat = expr->funcformat;
		newexpr->args = args;
		return (Node *) newexpr;
	}


	/*
	 * For any node type not handled above, we recurse using
	 * plan_tree_mutator, which will copy the node unchanged but try to
	 * simplify its arguments (if any) using this routine.
	 */
	return plan_tree_mutator(node, pre_dispatch_function_evaluation_mutator,
							 (void *) context);
}



/*
 * Compose and dispatch the MPPEXEC commands corresponding to a plan tree
 * within a complete parallel plan.  (A plan tree will correspond either
 * to an initPlan or to the main plan.)
 *
 * If cancelOnError is true, then any dispatching error, a cancellation
 * request from the client, or an error from any QE in the plan, may cause
 * the unfinished portion of the plan to be abandoned or canceled; and
 * in the event this occurs before all gangs have been dispatched, this
 * function does not return, but waits for all QEs to stop and exits to
 * the caller's error catcher via ereport(ERROR,...).  Otherwise this
 * function returns normally and errors are not reported until later.
 *
 * If cancelOnError is false, the plan is to be dispatched as fully as
 * possible and the QEs allowed to proceed regardless of cancellation
 * requests, errors or connection failures from other QEs, etc.
 *
 * The CdbDispatchResults objects allocated for the plan are returned in
 * *pPrimaryResults and (if needed) *pMirrorResults.  The caller, after
 * calling CdbCheckDispatchResult(), can examine the CdbDispatchResults
 * objects, can keep them as long as needed, and ultimately must free them
 * with cdbdisp_destroyDispatchResults() prior to deallocation of the
 * caller's memory context.  Callers should use PG_TRY/PG_CATCH to
 * ensure proper cleanup.
 *
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 *
 * Note that the slice tree dispatched is the one specified in the EState
 * of the argument QueryDesc as es_cur_root_slice.
 *
 * Note that the QueryDesc params must include PARAM_EXEC_REMOTE parameters
 * containing the values of any initplans required by the slice to be run.
 * (This is handled by calls to addRemoteExecParamsToParamList() from the
 * functions preprocess_initplans() and ExecutorRun().)
 */
void
cdbdisp_dispatchPlan(struct QueryDesc *queryDesc,
					 bool cancelOnError,
					 struct CdbDispatchResults ** pPrimaryResults,		/* OUT */
					 struct CdbDispatchResults ** pMirrorResults)		/* OUT */
{
	StringInfoData strinfo;
	char	   *squery,
			   *splan,
			   *ssliceinfo;
	char	   *mppexecPrefix;
	int			squery_len,
				splan_len,
				mppexecPrefix_len,
				ssliceinfo_len;
	SliceTable *sliceTbl;
	int			rootIdx;
	int			oldLocalSlice;
	Slice	  **sliceVector = NULL;
	int			nSlices,
				sliceLim;
	int			iSlice;
	pre_dispatch_function_evaluation_context pcontext;

	*pPrimaryResults = NULL;
	if (pMirrorResults)
		*pMirrorResults = NULL;

	Assert(Mpp_role == MPP_ROLE_DISPATCH);
	Assert(queryDesc != NULL && queryDesc->estate != NULL);

	/*
	 * Later we'll need to operate with the slice table provided via the
	 * EState structure in the argument QueryDesc.	Cache this information
	 * locally and assert our expectations about it.
	 */
	sliceTbl = queryDesc->estate->es_sliceTable;
	rootIdx = queryDesc->estate->es_cur_root_idx;

	Assert(sliceTbl != NULL);
	Assert(rootIdx == 0
		   || (rootIdx > sliceTbl->nMotions
			   && rootIdx < sliceTbl->nextInitPlan));

	/*
	 * Keep old value so we can restore it.  We use this field as a parameter.
	 */
	oldLocalSlice = sliceTbl->localSlice;


	/*
	 * Let's evaluate STABLE functions now, so we get consistent values on the QEs
	 *
	 * Also, if this is a single-row INSERT statement, let's evaluate
	 * nextval() and currval() now, so that we get the QD's values, and a
	 * consistent value for everyone
	 *
	 */

	pcontext.single_row_insert = false;
	pcontext.nextval_replacement = NULL;

	if (queryDesc->operation == CMD_INSERT)
	{
		Assert(queryDesc->parsetree->commandType == CMD_INSERT);

		if (queryDesc->parsetree->jointree != NULL)
			if (queryDesc->parsetree->jointree->fromlist == NIL)
				pcontext.single_row_insert = true;
	}

	if (queryDesc->operation == CMD_INSERT ||
		queryDesc->operation == CMD_SELECT ||
		queryDesc->operation == CMD_UPDATE ||
		queryDesc->operation == CMD_DELETE)
	{
		/*
		 * The Query tree doesn't have to be mutated here, since it isn't used during
		 * the execution.
		 */
		/*
		  queryDesc->parsetree = (Query *)query_tree_mutator((Node *)queryDesc->parsetree,
		  pre_dispatch_function_evaluation_mutator,
		  &pcontext);
		*/

		queryDesc->plantree = (Plan *) plan_tree_mutator((Node *) queryDesc->plantree,
									pre_dispatch_function_evaluation_mutator,
														 &pcontext);


	}


	/*
	 * Serialize plan and query trees. Build the common prefix of the
	 * MPPEXEC commands we'll issue.  Note that we're called for a single
	 * slice tree (corresponding to an initPlan or the main plan), so the
	 * parameters are fixed and we can include them in the prefix.
	 */
	squery = serializeNode((Node *) queryDesc->parsetree, &squery_len);
	splan = serializeNode((Node *) queryDesc->plantree, &splan_len);

	Assert(squery != NULL && splan != NULL);

	initStringInfo(&strinfo);

	appendStringInfoLiteral(&strinfo, "mppexec '");
	appendBinaryStringInfo(&strinfo, squery, squery_len);
	appendStringInfoLiteral(&strinfo, "' '");
	appendBinaryStringInfo(&strinfo, splan, splan_len);
	appendStringInfoChar(&strinfo, '\'');

	pfree(squery);
	pfree(splan);

	/*
	 * Next, continue building the MPPEXEC command by adding all the parameters
	 * serialized into RemoteParamData structs, which is just a node wrapper
	 * around ParamListInfo.
	 *
	 * Add parameters to the mppexec command by enclosing the whole set with
	 * ()  and separating individual ones with a comma, so it looks like:
	 * ('Param1', 'Param2', ...) at the end of the mppexec command.
	 */
	if (queryDesc->params != NULL)
	{
		bool		bFirstTime = true;
		ParamListInfo q = queryDesc->params;

		appendStringInfoLiteral(&strinfo, " (");
		while (q->kind != PARAM_INVALID)
		{
			char	   *sparms;
			int			sparms_len;
			RemoteParamData *rpd;

			rpd = makeNode(RemoteParamData);
			memcpy(&rpd->paramData, q, sizeof(ParamListInfoData));
			sparms = serializeNode((Node *) rpd, &sparms_len);
			if (!bFirstTime)
				appendStringInfoLiteral(&strinfo, ", ");
			else
				bFirstTime = false;

			appendStringInfoLiteral(&strinfo, " '");
			appendBinaryStringInfo(&strinfo, sparms, sparms_len);
			appendStringInfoLiteral(&strinfo, "'");

			pfree(sparms);
			pfree(rpd);
			q++;
		}
		appendStringInfoLiteral(&strinfo, ")");
	}

	/*
	 * Hang on to the command so far.  It will serve as the prefix for each
	 * of the MPPEXEC commands we dispatch.  Clear the buffer for use in
	 * constructing the final MPPEXEC command(s).
	 */
	mppexecPrefix = strinfo.data;
	mppexecPrefix_len = strinfo.len;
	initStringInfo(&strinfo);

	/*
	 * Traverse the slice tree in sliceTbl rooted at rootIdx and build a
	 * vector of slice indexes specifying the order of [potential] dispatch.
	 */
	sliceLim = list_length(sliceTbl->slices);
	sliceVector = palloc(sliceLim * sizeof(sliceVector[0]));
	nSlices = fillSliceVector(sliceTbl, rootIdx, sliceVector, sliceLim, 0);

	/* Allocate result array with enough slots for QEs of primary gangs. */
	*pPrimaryResults = cdbdisp_makeDispatchResults(nSlices * getMPPSegmentCount(),
												   sliceLim,
												   cancelOnError);

	/* Allocate result array for mirror gang. */
	if (queryDesc->operation != CMD_SELECT)
	{
		Slice	   *slice = (Slice *) list_nth(sliceTbl->slices, rootIdx);

		if (slice->mirrorGang)
		{
			*pMirrorResults = cdbdisp_makeDispatchResults(getMPPSegmentCount(),
														  sliceLim,
														  cancelOnError);
		}
	}

	/*
	 * Now we need to call CDBDispatchCommand once per slice.  Each such
	 * call dispatches a MPPEXEC command to each of the QEs assigned to
	 * the slice.
	 *
	 * The QE information goes in two places: (1) in the argument to the
	 * function CDBDispatchCommand, and (2) in the serialized MPPEXEC
	 * command sent to the QEs.
	 *
	 * So, for each slice in the tree...
	 */
	for (iSlice = 0; iSlice < nSlices; iSlice++)
	{
		Slice	   *slice = sliceVector[iSlice];
		Gang	   *primaryGang = slice->primaryGang;
		Gang	   *mirrorGang = slice->mirrorGang;

		/*
		 * Is this a slice we should dispatch?
		 */
		if (slice->gangType == GANGTYPE_UNALLOCATED)
		{
			/*
			 * Most slices are dispatched, however, in many  cases the
			 * * root runs only on the QD and is not dispatched to the QEs.
			 */
			continue;
		}

		Assert(primaryGang != NULL);	/* Must have a gang to dispatch to */

		/* Bail out if already got an error or cancellation request. */
		if (cancelOnError)
		{
			if ((*pPrimaryResults)->errcode)
				break;
			if (pMirrorResults && *pMirrorResults && (*pMirrorResults)->errcode)
				break;
			if (InterruptPending)
				break;
		}

		/*
		 * Set up the slice info parameter for this slice and use it to
		 * construct a finished MPPEXEC command.
		 */
		sliceTbl->localSlice = slice->sliceIndex;
		ssliceinfo = serializeNode((Node *) sliceTbl, &ssliceinfo_len);

		appendBinaryStringInfo(&strinfo, mppexecPrefix, mppexecPrefix_len);
		appendStringInfoLiteral(&strinfo, " '");
		appendBinaryStringInfo(&strinfo, ssliceinfo, ssliceinfo_len);
		appendStringInfoLiteral(&strinfo, "'");

		pfree(ssliceinfo);

		/*
		 * Dispatch the plan to our primaryGang.
		 * Doesn't wait for it to finish.
		 */
		cdbdisp_dispatchToGang(strinfo.data,
							   primaryGang,
							   *pPrimaryResults,
							   slice->sliceIndex);

		/*
		 * dispatch the plan to our mirrorGang if we have one.
		 */
		if (mirrorGang != NULL && queryDesc->operation != CMD_SELECT)
		{
			cdbdisp_dispatchToGang(strinfo.data,
								   mirrorGang,
								   *pMirrorResults,
								   slice->sliceIndex);
		}

		/* Empty the buffer for reuse. */
		truncateStringInfo(&strinfo, 0);
	}

	pfree(strinfo.data);
	pfree(mppexecPrefix);
	pfree(sliceVector);

	sliceTbl->localSlice = oldLocalSlice;

	/*
	 * If bailed before completely dispatched, stop QEs and throw error.
	 */
	if (iSlice < nSlices)
	{
		CdbDispatchResults *pr = *pPrimaryResults;
		CdbDispatchResults *mr = pMirrorResults ? *pMirrorResults : NULL;

		elog(DEBUG2, "Plan dispatch canceled; dispatched %d of %d slices",
			 iSlice, nSlices);

		/* Cancel any QEs still running, and wait for them to terminate. */
		CdbCheckDispatchResult(true);

		/*
		 * Check and free the results of all gangs. If any QE had an
		 * error, report it and exit via PG_THROW.
		 */
		*pPrimaryResults = NULL;
		if (mr)
			*pMirrorResults = NULL;
		cdbdisp_finishCommand(pr, mr);

		/* Wasn't an error, must have been an interrupt. */
		CHECK_FOR_INTERRUPTS();

		/* Strange!  Not an interrupt either. */
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg_internal("Unable to dispatch plan.")));
	}
}	/* cdbdisp_dispatchPlan */


/*
 * Helper for CdbDispatchPlan
 *
 * Add the indexes of the slices in the subtree of the slice table rooted
 * at the given index in the order in which they should be dispatched. Used
 * locally in CdbDispatchPlan to decide the order of calls to CdbDispatch.
 *
 * Arguments:
 *
 *	sliceTable	- slice table
 *	sliceIndex	- index of the subtree root in slice table
 *	sliceVector - slice vector under contruction
 *	len			- number of positions allocated in the slice vector
 *	next		- position at which to write the next Slice ptr
 *
 * Returns new value of count.
 */
int
fillSliceVector(SliceTable * sliceTable, int sliceIndex, Slice ** sliceVector,
				int len, int next)
{
	int			childIndex;
	ListCell   *sublist;
	Slice	   *slice = (Slice *) list_nth(sliceTable->slices, sliceIndex);

	foreach(sublist, slice->children)
	{
		childIndex = lfirst_int(sublist);
		next = fillSliceVector(sliceTable, childIndex, sliceVector, len, next);
	}

	if (next >= len)
	{
		ereport(ERROR,
				(errcode(ERRCODE_MPP_INTERNAL_ERROR),
				 errmsg("bad slice vector allocation")));
	}

	sliceVector[next++] = slice;
	return next;
}


/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs.  Does not wait for
 * completion.	Does not start a global transaction.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchUtilityStatement(struct Node *stmt,
								 bool cancelOnError,
								 struct CdbDispatchResults ** pPrimaryResults,
								 struct CdbDispatchResults ** pMirrorResults)
{
	char	   *szCommand;
	int			szCommand_len;
	Query	   *q = makeNode(Query);
	StringInfoData buffer;

	initStringInfo(&buffer);

	q->commandType = CMD_UTILITY;

	Assert(stmt != NULL);
	Assert(stmt->type < 1000);
	Assert(stmt->type > 0);

	q->utilityStmt = stmt;

	q->querySource = QSRC_ORIGINAL;

	/*
	 * We must set q->canSetTag = true.  False would be used to hide a command
	 * introduced by rule expansion which is not allowed to return its
	 * completion status in the command tag (PQcmdStatus/PQcmdTuples). For
	 * example, if the original unexpanded command was SELECT, the status
	 * should come back as "SELECT n" and should not reflect other commands
	 * inserted by rewrite rules.  True means we want the status.
	 */
	q->canSetTag = true;		/* ? */

	/*
	 * Serialize the stmt tree, and create the sql statement: mppexec ....
	 */
	szCommand = serializeNode((Node *) q, &szCommand_len);

	Assert(szCommand != NULL);

	appendStringInfoLiteral(&buffer, "mppexec '");
	appendBinaryStringInfo(&buffer, szCommand, szCommand_len);
	appendStringInfoLiteral(&buffer, "' ''");

	pfree(szCommand);

	cdbdisp_dispatchCommand(buffer.data, cancelOnError,
							pPrimaryResults, pMirrorResults);

	pfree(buffer.data);
}	/* cdbdisp_dispatchUtilityStatement */


/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs, and wait for completion.
 * Starts a global transaction first, if not already started.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.
 */
void
CdbDispatchUtilityStatement(struct Node *stmt)
{
	CdbDispatchResults *primaryResults;
	CdbDispatchResults *mirrorResults;

	/* Start a global transaction and reconfigure the cluster if needed. */
	cdbdisp_startDtxTransaction();

	cdbdisp_dispatchUtilityStatement(stmt, true, &primaryResults, &mirrorResults);

	/* Wait for all QEs to finish.	Throw up if error. */
	cdbdisp_finishCommand(primaryResults, mirrorResults);

}	/* CdbDispatchUtilityStatement */
