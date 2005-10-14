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

	/*
	 * Returned resultset
	 */
	struct pg_result **resultSets;

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

static bool						/* returns true if command complete */ processResults(CdbDispatchResult * dispatchResult,
			   struct pg_result ** pqresults,
			   int db_count,
			   int *resultCount);

static int						/* returns errcode, forced nonzero */
seterrcode(int errcode,
		   int resultIndex,		/* use -1 if no PGresult */
		   CdbDispatchResult * dispatchResult);


static void addSegDBToDispatchThreadPool(DispatchCommandParms * ParmsAr,
							 int segdbs_in_thread_pool,
							 const char *szCommand,
							 CdbDispatchResult * dispatchResult);

static int fillSliceVector(SliceTable * sliceTable, int sliceIndex,
				int *indexVector, int len, int next);


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

/* This mutex serializes writes by dispatcher threads to the
 * iFirstError and errcode fields of CdbDispatchResults objects.
 */
static pthread_mutex_t processResultsMutex = PTHREAD_MUTEX_INITIALIZER;


/*--------------------------------------------------------------------*/


/* Start a global transaction and invoke FTS reconfiguration if needed. */
void
cdbdisp_startDtxTransaction(void)
{
	/*
	 * need to start distributed transaction
	 */
	condStartDtxTransaction();

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
 * Unless an error prevents delegating the command to the threads, on return
 * there is a CdbDispatchResults object attached to the caller's Gang
 * object.	It contains an array of CdbDispatchResult objects, one per QE,
 * paralleling the Gang's db_descriptors array.  Success or failure of each
 * QE will be noted in the corresponding CdbDispatchResult entry, but before
 * examining it, the caller must wait for execution to end by calling
 * cdbdisp_checkDispatchResults().
 *
 * On return, the caller's Gang object owns the CdbDispatchResults object.
 * After examining the CdbDispatchResults object, the caller should detach
 * it from the Gang and call cdbdisp_destroyDispatchResults() to free it.
 *
 * NB: When cancelOnError is false, szCommand is to be dispatched to
 * every connected gang member without regard for cancellation requests
 * nor for any other QE's errors, connection failures, etc.  Also this
 * function mustn't exit via elog(error, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 */
void
cdbdisp_dispatchToGang(const char *szCommand,
					   struct Gang * gp,
					   int sliceIndex,
					   bool cancelOnError,
					   struct pg_result *** pgresultsets)
{
	SegmentDatabaseDescriptor *segdbDesc;
	int			i;
	int			max_threads;
	int			segdbs_in_thread_pool = 0;
	int			x;
	int			newThreads = 0;

	Assert(Mpp_role == MPP_ROLE_DISPATCH);
	Assert(gp != NULL);
	Assert(gp->size > 0);

	if (DEBUG2 >= log_min_messages)
	{
		if (sliceIndex >= 0)
			elog(DEBUG2, "dispatchToGang: sliceIndex=%d gangsize=%d  %.100s",
				 sliceIndex, gp->size, szCommand);
		else
			elog(DEBUG2, "dispatchToGang: gangsize=%d  %.100s",
				 gp->size, szCommand);
	}

	if (pgresultsets)
		*pgresultsets = NULL;

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
				elog(NOTICE, "BUG: Reallocating dispatchCommandParmsAr");

			dispatchCommandParmsAr =
				(DispatchCommandParms *) repalloc(dispatchCommandParmsAr, (threadCount + max_threads) *
											   sizeof(DispatchCommandParms));
			memset(dispatchCommandParmsAr + threadCount, 0, max_threads * sizeof(DispatchCommandParms));
		}

		/* Create a CdbDispatchResults object and attach it to the Gang. */
		gp->dispatchResults = cdbdisp_makeDispatchResults(gp->size, cancelOnError);

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
		CdbDispatchResult *dispatchResult = &gp->dispatchResults->resultArray[x];

		segdbDesc = &gp->db_descriptors[x];

		/* Provide the slice index for error messages related to this QE. */
		cdbconn_setSliceIndex(segdbDesc, sliceIndex);

		/* Initialize the QE's CdbDispatchResult object. */
		cdbdisp_initResult(dispatchResult, segdbDesc);

		/* Transfer any connection errors from segdbDesc. */
		if (segdbDesc->errcode ||
			segdbDesc->error_message.len)
		{
			seterrcode(segdbDesc->errcode, -1, dispatchResult);
			cdbdisp_mergeConnectionErrors(dispatchResult, segdbDesc);
		}

		segdbs_in_thread_pool = x;
		addSegDBToDispatchThreadPool(dispatchCommandParmsAr + threadCount,
									 segdbs_in_thread_pool,
									 szCommand,
									 dispatchResult);

		/*
		 * This CdbDispatchResult/SegmentDatabaseDescriptor pair will be
		 * dispatched and monitored by a thread to be started below. Only that
		 * thread should touch them until the thread is finished with them and
		 * resets the stillRunning flag. Caller must CdbCheckDispatchResult()
		 * to wait for completion.
		 */
		dispatchResult->stillRunning = true;
	}

	/*
	 * Compute the thread count based on how many segdbs were added into the
	 * thread pool, knowing that each thread handles mpp_connections_per_thread
	 * segdbs.
	 */
	newThreads += 1 + (segdbs_in_thread_pool - 1) / mpp_connections_per_thread;

	/*
	 * set result set variables if return of result set is required
	 */
	if (pgresultsets != NULL)
	{
		/*
		 * allocate one more slot to indicate the end of array, so caller
		 * don't need to get the length of the array.
		 */
		*pgresultsets = (struct pg_result **) calloc(gp->size + 1,
													 sizeof(**pgresultsets));

		for (i = 0; i < newThreads; i++)
		{
			DispatchCommandParms *pParms = &(dispatchCommandParmsAr + threadCount)[i];

			pParms->resultSets = *pgresultsets + (i * mpp_connections_per_thread);
		}
	}

	elog(DEBUG4,
		 "dispatchToGang: threadCount %d, segdbs_in_thread_pool %d, conns_per_thread %d",
		 threadCount, segdbs_in_thread_pool, mpp_connections_per_thread);

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
 * Waits for threads that were launched in CdbDispatchCommand
 * to finish.  Returns true if all QEs succeeded.
 * If 'requestCancel' is true, this function will attempt to cancel
 * unfinished work that was started with 'cancelOnError' true.
 *
 * NB: This function, and those that it calls, must not exit via
 * PG_THROW, ereport(ERROR, ...), CHECK_FOR_INTERRUPTS, etc. !!
 */
bool							/* returns true if success on all QEs */
CdbCheckDispatchResult(bool requestCancel)
{
	return CdbCheckDispatchResultInt(NULL, NULL, requestCancel);
}

bool							/* returns true if success on all QEs */
CdbCheckDispatchResultInt(struct SegmentDatabaseDescriptor *** failedSegDB,
						  int *numOfFailed,
						  bool cancelUnfinishedWork)
{
	bool		success = true;
	int			i;
	int			j;
	int			numToReconnect = 0;
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
		return true;
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

			/* Transfer any new connection errors from segdbDesc. */
			if (segdbDesc->errcode ||
				segdbDesc->error_message.len)
			{
				seterrcode(segdbDesc->errcode, -1, dispatchResult);
				cdbdisp_mergeConnectionErrors(dispatchResult, segdbDesc);
			}

			/* Did someone set error_message without setting errcode? */
			if (dispatchResult->error_message.len > 0 &&
				!dispatchResult->errcode)
			{
				seterrcode(ERRCODE_MPP_INTERNAL_ERROR, -1, dispatchResult);
				elog(DEBUG2, "(%s) CheckDispatchResult: errcode not set for error_message: %s",
					 segdbDesc->whoami, dispatchResult->error_message.data);
			}

			/* Log the result */
			if (DEBUG2 >= log_min_messages)
				cdbdisp_debugDispatchResult(dispatchResult, DEBUG2, DEBUG3);

			/* Notify FTS to reconnect if connection lost or never connected. */
			if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
				numToReconnect++;

			if (dispatchResult->errcode)
				success = false;

		}						/* loop over QEs managed by one thread */
	}							/* loop over threads */

	/*
	 * Build broken connection list for FTS.
	 */
	if (numToReconnect > 0 &&
		failedSegDB)
	{							/* build broken connection list */
		int			nFailed = 0;

		/* Allocate storage.  Caller should pfree() it. */
		*failedSegDB = (SegmentDatabaseDescriptor **)
			palloc(sizeof(SegmentDatabaseDescriptor *) * (numToReconnect + 1));

		for (i = 0; i < threadCount; i++)
		{						/* over threads */
			pParms = &dispatchCommandParmsAr[i];
			for (j = 0; j < pParms->db_count; j++)
			{					/* over connections */
				dispatchResult = pParms->dispatchResultPtrArray[j];
				segdbDesc = dispatchResult->segdbDesc;

				if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
					(*failedSegDB)[nFailed++] = segdbDesc;

			}					/* over connections */
		}						/* over threads */
		(*failedSegDB)[nFailed] = NULL;
		Assert(nFailed == numToReconnect);
	}							/* build broken connection list */

	if (numOfFailed)
		*numOfFailed = numToReconnect;

	/*
	 * pfree the memory allocated for the dispatchCommandParmsAr
	 */
	if (dispatchCommandParmsAr != NULL)
	{
		elog(DEBUG5, "CheckDispatchResult freeing");
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

	/*
	 * Attempt to re-establish broken connections.
	 */
	if (numOfFailed != NULL && *numOfFailed > 0)
		FtsHandleNetFailure(*failedSegDB, *numOfFailed);

	elog(DEBUG5, "CheckDispatchResult done");
	return success;
}	/* cdbdisp_checkDispatchResults */


/* Format and free a Gang's CdbDispatchResults object.
 * Appends error messages to caller's StringInfo buffer.
 * Returns false if some error was found, or true if no errors.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
bool							/* returns true if ok */
cdbdisp_dumpGangResults(struct Gang * gang,
						StringInfo buffer,		/* OUT */
						bool verbose)
{
	CdbDispatchResults *gangResults = gang ? gang->dispatchResults : NULL;
	CdbDispatchResult *dispatchResult;

	if (!gang ||
		!gangResults)
		return true;

	/*
	 * If no QE got an error, free CdbDispatchResults and return. NB:
	 * Cancellation requested by us doesn't set gangResults->errcode or
	 * gangResults->iFirstError.
	 */
	if (!gangResults->errcode)
	{
		cdbdisp_destroyGangResults(gang);
		return true;
	}

	/* Find the CdbDispatchResult of the first QE that got an error. */
	Assert(gangResults->iFirstError >= 0 &&
		   gangResults->iFirstError < gang->size);

	dispatchResult = &gangResults->resultArray[gangResults->iFirstError];

	Assert(dispatchResult->segdbDesc &&
		   dispatchResult->segdbDesc->gang == gang &&
		   dispatchResult->errcode != 0);

	/* Format one QE's result. */
	cdbdisp_dumpDispatchResult(dispatchResult, verbose, buffer);

	/* Optionally, format results from the rest of the QEs that got errors. */
	if (verbose)
	{
		int			i;

		for (i = 0; i < gangResults->resultCount; ++i)
		{
			dispatchResult = &gangResults->resultArray[i];
			if (i == gangResults->iFirstError)
				continue;
			if (dispatchResult->errcode)
				cdbdisp_dumpDispatchResult(dispatchResult, verbose, buffer);
		}
	}

	cdbdisp_destroyGangResults(gang);
	return false;
}	/* cdbdisp_dumpGangResults */


/* Free a Gang's CdbDispatchResults object. */
void
cdbdisp_destroyGangResults(struct Gang * gang)
{
	if (gang &&
		gang->dispatchResults)
	{
		cdbdisp_destroyDispatchResults(gang->dispatchResults);
		gang->dispatchResults = NULL;
	}
}	/* cdbdisp_destroyGangResults */


/* Get sum of cmdTuples statistics returned by the successful QEs of a Gang.
 * Used to determine the row counts for insert, update, and delete.
 */
int
cdbdisp_sumGangCmdTuples(struct Gang * gang)
{
	return gang ? cdbdisp_sumCmdTuples(gang->dispatchResults)
		: 0;
}	/* cdbdisp_sumGangCmdTuples */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchCommand:
 * Send ths szCommand SQL statement to all segdbs in the cluster
 * cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * The Gang objects allocated for the command can optionally be returned
 * in *pPrimaryGang and *pMirrorGang.  The caller, after calling
 * CdbCheckDispatchResult, can examine the CdbDispatchResults object
 * attached to each Gang.  The Gang objects are owned by the QE Factory.
 * However, the caller may optionally detach the CdbDispatchResults objects,
 * keep them as long as needed (perhaps longer than the gangs exist),
 * and ultimately free them with cdbdisp_destroyDispatchResults().
 */
void
cdbdisp_dispatchCommand(const char *szCommand,
						bool startDtxTransaction,
						bool cancelOnError,
						struct pg_result *** resultSets,		/* OUT */
						struct Gang ** pPrimaryGang,	/* OUT */
						struct Gang ** pMirrorGang)		/* OUT */
{
	Gang	   *primaryGang = NULL;
	Gang	   *mirrorGang = NULL;

	elog(DEBUG3, "cdbdisp_dispatchCommand: %.50s", szCommand);

	if (pPrimaryGang)
		*pPrimaryGang = NULL;
	if (pMirrorGang)
		*pMirrorGang = NULL;

	/*
	 * Start a global transaction and reconfigure the cluster if needed.
	 */
	if (startDtxTransaction)
		cdbdisp_startDtxTransaction();

	/*
	 * Allocate a primary and mirror QE for every available segDB in the system.
	 */
	primaryGang = allocateGang(GANGTYPE_PRIMARY_WRITER, getMPPSegmentCount(), -1);

	Assert(primaryGang);

	if (pPrimaryGang)
		*pPrimaryGang = primaryGang;

	mirrorGang = allocateGang(GANGTYPE_MIRROR_WRITER, getMPPSegmentCount(), -1);

	if (pMirrorGang)
		*pMirrorGang = mirrorGang;

	if (mirrorGang && (mirrorGang->segdb_count < getMPPSegmentCount() || mirrorGang->size < getMPPSegmentCount()))
	{
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
	cdbdisp_dispatchToGang(szCommand, primaryGang, -1, cancelOnError, resultSets);

	if (mirrorGang)
		cdbdisp_dispatchToGang(szCommand, mirrorGang, -1, cancelOnError, resultSets);

}	/* cdbdisp_dispatchCommand */


/* Wait for all QEs to finish, then report any errors from the given gangs.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.  No-op if both gang ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct Gang * primaryGang,
					  struct Gang * mirrorGang)
{
	StringInfoData buf;
	bool		success;

	/* If caller didn't allocate gangs, don't wait. */
	if (!primaryGang &&
		!mirrorGang)
		return;

	/* Wait for all QEs to finish, and see if they were all successful. */
	if (CdbCheckDispatchResult(false))
	{
		cdbdisp_destroyGangResults(primaryGang);
		cdbdisp_destroyGangResults(mirrorGang);
		return;
	}

	/* Format and free the results from QEs. */
	initStringInfo(&buf);

	success = cdbdisp_dumpGangResults(primaryGang, &buf, false);

	/* Display mirror gang errors only if there were no primary errors. */
	if (!success)
		cdbdisp_dumpGangResults(mirrorGang, &buf, false);
	else
		cdbdisp_destroyGangResults(mirrorGang);

	/*
	 * Caller's gangs might have succeeded even though there was an error
	 * somewhere in the batch of commands checked by CdbCheckDispatchResult.
	 */
	if (buf.len == 0)
	{
		pfree(buf.data);
		return;
	}

	/* Too bad, our gang got an error. */
	PG_TRY();
	{
		/* CDB TODO: get errcode from gangs above */
		ereport(ERROR, (errcode(ERRCODE_MPP_COMMAND_ERROR),
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
	CdbDispatchResults *gangResults;
	int			timeoutCounter = 0;
	int			resultCount = 0;
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
			gangResults = segdbDesc->gang->dispatchResults;

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
				int			msgoff;

				/* Save first error code. */
				seterrcode(ERRCODE_MPP_INTERCONNECTION_ERROR, -1, dispatchResult);

				msgoff = dispatchResult->error_message.len;
				appendPQExpBuffer(&dispatchResult->error_message,
								  "Lost connection to %s.  %s",
								  segdbDesc->whoami,
								  msg ? msg : "\n");

				if (DEBUG1 >= log_min_messages)
				{
					/* Don't use elog, it's not thread-safe */
					fputs(dispatchResult->error_message.data + msgoff, stderr);
					fflush(stderr);
				}

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
			finished = processResults(dispatchResult,
									  pParms->resultSets,
									  db_count,
									  &resultCount);

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
	CdbDispatchResults *gangResults = segdbDesc->gang->dispatchResults;

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
		int			msgoff;

		seterrcode(ERRCODE_MPP_INTERCONNECTION_ERROR, -1, dispatchResult);

		msgoff = dispatchResult->error_message.len;
		appendPQExpBuffer(&dispatchResult->error_message,
						  "Lost connection to %s.  %s",
						  segdbDesc->whoami,
						  msg ? msg : "\n");

		if (LOG >= log_min_messages)
		{
			/* Don't use elog, it's not thread-safe */
			fputs(dispatchResult->error_message.data + msgoff, stderr);
			fflush(stderr);
		}

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
	if (QueryCancelPending &&
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
		fprintf(stderr, "Sending command to %s\n"
				"    %.120s\n",
				segdbDesc->whoami,
				szCommand);

	/*
	 * Submit the command asynchronously.
	 */
	if (0 == PQsendQuery(conn, szCommand))
	{
		int			msgoff = dispatchResult->error_message.len;

		seterrcode(ERRCODE_MPP_INTERCONNECTION_ERROR, -1, dispatchResult);

		appendPQExpBuffer(&dispatchResult->error_message,
						  "Command could not be sent to segment db %s;  %s",
						  segdbDesc->whoami,
						  PQerrorMessage(conn));

		if (LOG >= log_min_messages)
		{
			/* Don't use elog, it's not thread-safe */
			fputs(dispatchResult->error_message.data + msgoff, stderr);
			fflush(stderr);
		}
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

		/* Tell the client. */
		seterrcode(ERRCODE_MPP_INTERCONNECTION_ERROR, -1, dispatchResult);
		appendPQExpBuffer(&dispatchResult->error_message,
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
	CdbDispatchResults *gangResults;
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
		gangResults = segdbDesc->gang->dispatchResults;

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
			QueryCancelPending ||
			gangResults->errcode)
		{
			if (gangResults->cancelOnError &&
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
				int			msgoff = dispatchResult->error_message.len;

				seterrcode(ERRCODE_MPP_INTERCONNECTION_ERROR, -1, dispatchResult);

				appendPQExpBuffer(&dispatchResult->error_message,
								  "Segment db no longer connected: %s\n",
								  segdbDesc->whoami);

				/* Don't use elog, it's not thread-safe */
				if (DEBUG1 >= log_min_messages)
				{
					fputs(dispatchResult->error_message.data + msgoff, stderr);
					fflush(stderr);
				}

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
processResults(CdbDispatchResult * dispatchResult,
			   struct pg_result ** pqresults,
			   int db_count,
			   int *resultCount)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	char	   *errorMessage;
	int			msgoff;
	int			rc;

	/* Receive input from QE. */
	rc = PQconsumeInput(segdbDesc->conn);

	/* If PQconsumeInput fails, we're hosed. */
	if (rc == 0)
	{							/* handle PQconsumeInput error */
		errorMessage = PQerrorMessage(segdbDesc->conn);

		seterrcode(ERRCODE_MPP_INTERCONNECTION_ERROR, -1, dispatchResult);

		msgoff = dispatchResult->error_message.len;
		appendPQExpBuffer(&dispatchResult->error_message,
						  "System error on PQconsumeInput from %s;  %s",
						  segdbDesc->whoami,
						  errorMessage ? errorMessage : "\n");

		if (LOG >= log_min_messages)
		{
			/* Don't use elog, it's not thread-safe */
			fputs(dispatchResult->error_message.data + msgoff, stderr);
			fflush(stderr);
		}

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
		if (!pqresults)
			cdbdisp_appendResult(dispatchResult, pRes);

		/*
		 * Stuff PGresult ptr into caller's array.
		 *
		 * CDB TODO: Remove this old interface.  Change caller to get the
		 * PGresult objects from the CdbDispatchResult.
		 */
		else
		{
			/* check array index is in bounds */
			Assert(*resultCount < db_count);
			/* TODO: not handling multiple result sets */
			Assert(pqresults[*resultCount] == NULL);
			pqresults[(*resultCount)++] = pRes;
		}

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

				fprintf(stderr, "%s ok ->  %s\n",
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
				fprintf(stderr, "%s -> result  %s %s  %s",
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
			seterrcode(errcode, resultIndex, dispatchResult);
		}						/* QE reported an error */
	}							/* loop to call PQgetResult; won't block */

	return false;				/* we must keep on monitoring this socket */
}	/* processResults */


/*
 * Take note of an error.
 *
 * NOTE: since this can be called via a thread, the same rules apply as to
 *		 thread_DispatchCommand: absolutely no elog'ing.
 */
int								/* returns errcode, forced nonzero */
seterrcode(int errcode,			/* ERRCODE_xxx or 0 */
		   int resultIndex,		/* use -1 if no PGresult */
		   CdbDispatchResult * dispatchResult)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	Gang	   *gang = segdbDesc ? segdbDesc->gang : NULL;
	CdbDispatchResults *gangResults = gang ? gang->dispatchResults : NULL;

	/* We must ensure a nonzero errcode. */
	if (!errcode)
		errcode = ERRCODE_INTERNAL_ERROR;

	/* Was the command canceled? */
	if (errcode == ERRCODE_MPP_OPERATION_CANCELED ||
		errcode == ERRCODE_QUERY_CANCELED)
		dispatchResult->wasCanceled = true;

	/*
	 * If this is the first error from this QE, save the error code and the
	 * index of the PGresult buffer entry.	We assume the caller has not yet
	 * added the item to the PGresult buffer.
	 */
	if (!dispatchResult->errcode)
	{
		dispatchResult->errcode = errcode;
		if (resultIndex >= 0)
			dispatchResult->errindex = resultIndex;
	}

	/*
	 * Remember which QE reported an error first among the gang, but keep
	 * quiet about cancellation done at our request.
	 */
	if (errcode == ERRCODE_MPP_OPERATION_CANCELED &&
		dispatchResult->requestedCancel)
	{
	}							/* nop */

	else if (gangResults &&
			 !gangResults->errcode)
	{
		pthread_mutex_lock(&processResultsMutex);
		if (!gangResults->errcode)
		{
			gangResults->errcode = errcode;
			gangResults->iFirstError = segdbDesc->gangindex;
		}
		pthread_mutex_unlock(&processResultsMutex);
	}

	return errcode;
}	/* seterrcode */


/*--------------------------------------------------------------------*/


/*
 * CdbDoCommandInt:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommandInt(const char *szCommand,
				bool cancelOnError,
				struct pg_result *** resultSets)
{
	Gang	   *primaryGang;
	Gang	   *mirrorGang;

	cdbdisp_dispatchCommand(szCommand, false, cancelOnError, resultSets,
							&primaryGang, &mirrorGang);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(primaryGang, mirrorGang);
}	/* CdbDoCommandInt */


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
	Gang	   *primaryGang;
	Gang	   *mirrorGang;

	cdbdisp_dispatchCommand(szCommand, true, cancelOnError, NULL,
							&primaryGang, &mirrorGang);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(primaryGang, mirrorGang);
}


/*--------------------------------------------------------------------*/


/*
 * Compose and dispatch the MPPEXEC commands corresponding to a plan tree
 * within a complete parallel plan.  (A plan tree will correspond either
 * to an initPlan or to the main plan.)
 *
 * The actual dispatch is performed by CdbDispatchCommand(), which is called
 * once for each slice of the tree.
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
cdbdisp_dispatchPlan(struct QueryDesc *queryDesc)
{
	StringInfoData strinfo;
	char	   *squery,
			   *splan,
			   *ssliceinfo;
	char	   *mppexecPrefix;
	SliceTable *sliceTbl;
	int			rootIdx;
	int			oldLocalSlice;
	int		   *sliceVector,
				nSlices,
				sliceLim;
	int			i;


	Assert(Mpp_role == MPP_ROLE_DISPATCH);
	Assert(queryDesc != NULL && queryDesc->estate != NULL);

	elog(DEBUG4, "CdbDispatchPlan");

	/*
	 * Start a global transaction and reconfigure the cluster if needed.
	 */
	cdbdisp_startDtxTransaction();

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
	 * Serialize plan and query trees. Build the common prefix of the
	 * MPPEXEC commands we'll issue.  Note that we're called for a single
	 * slice tree (corresponding to an initPlan or the main plan), so the
	 * parameters are fixed and we can include them in the prefix.
	 */
	squery = serializeNode((Node *) queryDesc->parsetree);
	splan = serializeNode((Node *) queryDesc->plantree);

	Assert(squery != NULL && splan != NULL);

	initStringInfo(&strinfo);
	appendStringInfo(&strinfo, "mppexec '%s' '%s'", squery, splan);

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

		appendStringInfo(&strinfo, " (");
		while (q->kind != PARAM_INVALID)
		{
			char	   *sparms;
			RemoteParamData *rpd;

			rpd = makeNode(RemoteParamData);
			memcpy(&rpd->paramData, q, sizeof(ParamListInfoData));
			sparms = serializeNode((Node *) rpd);
			if (!bFirstTime)
				appendStringInfo(&strinfo, ", ");
			else
				bFirstTime = false;

			appendStringInfo(&strinfo, " '%s'", sparms);

			pfree(sparms);
			pfree(rpd);
			q++;
		}
		appendStringInfo(&strinfo, ")");
	}

	/*
	 * Hang on to the command so far.  It will serve as the prefix for each
	 * of the MPPEXEC commands we dispatch.  Clear the buffer for use in
	 * constructing the final MPPEXEC command(s).
	 */
	mppexecPrefix = strinfo.data;
	initStringInfo(&strinfo);


	/*
	 * Traverse the slice tree in sliceTbl rooted at rootIdx and build a
	 * vector of slice indexes specifying the order of [potential] dispatch.
	 */
	sliceLim = list_length(sliceTbl->slices);
	sliceVector = palloc(sliceLim * sizeof(int));
	nSlices = fillSliceVector(sliceTbl, rootIdx, sliceVector, sliceLim, 0);


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
	for (i = 0; i < nSlices; i++)
	{
		/*
		 * SegmentDatabaseDescriptor ** sdp;
		 */

		Slice	   *slice =
		(Slice *) list_nth(sliceTbl->slices, sliceVector[i]);


		Gang	   *primaryGang = slice->primaryGang;
		Gang	   *mirrorGang = slice->mirrorGang;

		/*
		 * Is this a slice we should dispatch?
		 * *
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


		/*
		 * Set up the slice info parameter for this slice and use it to
		 * construct a finished MPPEXEC command.
		 */
		sliceTbl->localSlice = slice->sliceIndex;
		ssliceinfo = serializeNode((Node *) sliceTbl);
		appendStringInfo(&strinfo, "%s '%s'", mppexecPrefix, ssliceinfo);
		pfree(ssliceinfo);


		/*
		 * This call returns after launching the threads that send the command to the appropriate
		 * segdbs.	It does not wait for them to finish.
		 */
		cdbdisp_dispatchToGang(strinfo.data, primaryGang,
							   slice->sliceIndex, true, NULL);


		/*
		 * dispatch the plan to our mirrorGang if we have one.
		 */
		if (mirrorGang != NULL && queryDesc->operation != CMD_SELECT)
		{
			/*
			 * SegmentDatabaseDescriptor ** sdpMirror;
			 */

			elog(DEBUG3, "Slice #%d has a non-null mirror gang.", i);


			cdbdisp_dispatchToGang(strinfo.data, mirrorGang,
								   slice->sliceIndex, true, NULL);

		}

		/* Empty the buffer for reuse. */
		truncateStringInfo(&strinfo, 0);
	}

	pfree(strinfo.data);
	pfree(mppexecPrefix);

	sliceTbl->localSlice = oldLocalSlice;
	elog(DEBUG4, "CdbDispatchPlan done");
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
 *	IndexVector - index vector under contruction
 *	len			- number of positions allocated in the index vector
 *	next		- position at which to write the next index
 *
 * Returns new value of count.
 */
int
fillSliceVector(SliceTable * sliceTable, int sliceIndex, int *indexVector,
				int len, int next)
{
	int			childIndex;
	ListCell   *sublist;
	Slice	   *slice = (Slice *) list_nth(sliceTable->slices, sliceIndex);

	foreach(sublist, slice->children)
	{
		childIndex = lfirst_int(sublist);
		next = fillSliceVector(sliceTable, childIndex, indexVector, len, next);
	}

	if (next >= len)
	{
		ereport(ERROR,
				(errcode(ERRCODE_MPP_INTERNAL_ERROR),
				 errmsg("bad slice vector allocation")));
	}

	indexVector[next++] = sliceIndex;
	return next;
}


/* Free the results of all gangs in a query plan's slice table.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
void
cdbdisp_destroyPlanResults(struct QueryDesc *queryDesc)
{
	ListCell   *sliceCell;

	if (!queryDesc ||
		!queryDesc->estate ||
		!queryDesc->estate->es_sliceTable)
		return;

	foreach(sliceCell, queryDesc->estate->es_sliceTable->slices)
	{
		Slice	   *slice = (Slice *) lfirst(sliceCell);

		/* Ignore QD slices. */
		if (slice->gangType == GANGTYPE_UNALLOCATED)
			continue;

		/* Free results of primary gang. */
		cdbdisp_destroyGangResults(slice->primaryGang);

		/* Free results of mirror gang. */
		if (queryDesc->operation != CMD_SELECT)
			cdbdisp_destroyGangResults(slice->mirrorGang);
	}
}	/* cdbdisp_destroyPlanResults */


/* Check and free the results of all gangs in a query plan's slice table.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
void
cdbdisp_dumpPlanResults(struct QueryDesc *queryDesc)
{
	SliceTable *sliceTable = queryDesc->estate->es_sliceTable;
	ListCell   *sliceCell;
	StringInfoData buf;

	initStringInfo(&buf);

	/* For each slice, see if all of the QEs were successful. */
	foreach(sliceCell, sliceTable->slices)
	{
		Slice	   *slice = (Slice *) lfirst(sliceCell);
		bool		success;

		/* Ignore QD slices. */
		if (slice->gangType == GANGTYPE_UNALLOCATED)
			continue;

		/* Check (and free) results of primary gang. */
		success = cdbdisp_dumpGangResults(slice->primaryGang, &buf, false);

		/* Check (and free) results of mirror gang. */
		if (slice->mirrorGang &&
			queryDesc->operation != CMD_SELECT)
		{
			/* Show mirror gang errors if there were no primary errors. */
			if (success)
				cdbdisp_dumpGangResults(slice->mirrorGang, &buf, false);
			else
				cdbdisp_destroyGangResults(slice->mirrorGang);
		}
	}

	/* Success? */
	if (buf.len == 0)
	{
		pfree(buf.data);
		return;
	}

	/* Report the errors and exit to the exception handler. */
	PG_TRY();
	{
		/* CDB TODO: Get errcode returned by gangs. */
		ereport(ERROR, (errcode(ERRCODE_MPP_COMMAND_ERROR),
						errmsg("%s", buf.data)));
	}
	PG_CATCH();
	{
		pfree(buf.data);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* not reached */
}	/* cdbdisp_dumpPlanResults */


/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs.  To wait for completion
 * and check for errors, it is suggested that the caller use
 * cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchUtilityStatement(struct Node *stmt,
								 bool cancelOnError,
								 struct Gang ** pPrimaryGang,	/* OUT */
								 struct Gang ** pMirrorGang)	/* OUT */
{
	char	   *szCommand;

	/*
	 * Serialize the stmt tree, and create the sql statement: mppexec ....
	 */
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

	szCommand = serializeNode((Node *) q);

	Assert(szCommand != NULL);

	appendStringInfo(&buffer, "mppexec '%s' ''", szCommand);

	pfree(szCommand);

	cdbdisp_dispatchCommand(buffer.data, true, cancelOnError, NULL,
							pPrimaryGang, pMirrorGang);

	pfree(buffer.data);
}	/* cdbdisp_dispatchUtilityStatement */


void
CdbDispatchUtilityStatement(struct Node *stmt)
{
	Gang	   *primaryGang;
	Gang	   *mirrorGang;

	cdbdisp_dispatchUtilityStatement(stmt, true, &primaryGang, &mirrorGang);

	/* Wait for all QEs to finish.	Throw up if error. */
	cdbdisp_finishCommand(primaryGang, mirrorGang);

}	/* CdbDispatchUtilityStatement */
