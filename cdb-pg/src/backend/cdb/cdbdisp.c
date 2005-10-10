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

#include "executor/execdesc.h"      /* QueryDesc */
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
	CdbDispatchResult  **dispatchResultPtrArray;

	/*
	 * A flag indicating whether a failure of the command for one connection
	 * should act to requestCancel for all the other connections.
	 *
	 */
	bool		cancelFromBelow;

	/*
	 * The pthread_t thread handle.
	 */
	pthread_t	thread;
	
	/*
	 * Returned resultset
	 */
	struct pg_result  **resultSets;

}	DispatchCommandParms;


/*
 * Structure used in thread proc for keeping track of
 * commands that are still running.  For each command
 * that has not yet finished, we keep the segdbDesc pointer and
 * a flag indicating whether we've  requested cancel.
 */
typedef struct
{
    CdbDispatchResult  *dispatchResult;
}	RunningInfo;


/*
 * Static Helper functions
 */
static void *thread_DispatchCommand(void *arg);


static bool shouldStillDispatchCommand( CdbDispatchResult  *dispatchResult,
							 			bool cancelFromBelow);
							 			
static bool dispatchCommand( CdbDispatchResult *dispatchResult,
							 const char *szCommand,
							 bool cancelFromBelow );
							
static void
handleSelectError(RunningInfo  *stillRunning,
                  int           stillRunningCount,
                  int           sock_errno);							

static void checkForBadConnections( RunningInfo *stillRunning ,int *stillRunningCount, bool cancelFromBelow);

static void
handleSelectTimeout(RunningInfo    *stillRunning,
                    int            *stillRunningCount,
                    int            *timeoutCounter);

static void
processResults(RunningInfo         *runningInfo,
               bool                 cancelFromBelow,
               struct pg_result   **pqresults,
               int                  db_count,
               int                 *resultCount);


static void
addSegDBToDispatchThreadPool(DispatchCommandParms  *ParmsAr,
                             int                    segdbs_in_thread_pool,
							 const char            *szCommand,
							 bool                   cancelFromBelow,
							 CdbDispatchResult     *dispatchResult);

static void setCancelFlag(void);

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
static volatile bool cancelledByUs = false;


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchToGang:
 * Send the szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * Unless an error prevents delegating the command to the threads, on return
 * there is a CdbDispatchResults object attached to the caller's Gang
 * object.  It contains an array of CdbDispatchResult objects, one per QE,
 * paralleling the Gang's db_descriptors array.  Success or failure of each
 * QE will be noted in the corresponding CdbDispatchResult entry, but before
 * examining it, the caller must wait for execution to end by calling
 * cdbdisp_checkDispatchResults().
 *
 * On return, the caller's Gang object owns the CdbDispatchResults object.
 * After examining the CdbDispatchResults object, the caller should detach
 * it from the Gang and call cdbdisp_destroyDispatchResults() to free it.
 */
void
cdbdisp_dispatchToGang(const char          *szCommand,
                       struct Gang         *gp,
                       int                  sliceIndex,
                       bool                 cancelFromBelow,
                       struct pg_result  ***pgresultsets)
{
    SegmentDatabaseDescriptor  *segdbDesc;
	int			i;
	int			max_threads;
	int			segdbs_in_thread_pool = 0;
	int			x;
	int			newThreads = 0;

	Assert(Mpp_role == MPP_ROLE_DISPATCH);
	Assert(gp != NULL);
	Assert(gp->size > 0);

    elog(DEBUG3, "dispatchToGang: sliceIndex=%d gangsize=%d  %.50s",
         sliceIndex, gp->size, szCommand);

    if (pgresultsets)
        *pgresultsets = NULL;

    /* Free old CdbDispatchResults if still hanging around. */
    if (gp->dispatchResults)
    {
        cdbdisp_destroyDispatchResults(gp->dispatchResults);
        gp->dispatchResults = NULL;
    }

    /* If there was a cancellation notice before we submit the command,
     * then don't submit.
     */
    if (QueryCancelPending && !InterruptHoldoffCount)
    {
        elog(DEBUG3,
             "dispatchToGang: Cancellation pending%s; will not dispatch %s",
             cancelledByUs ? " (cancelledByUs)" : "",
             szCommand);
        CHECK_FOR_INTERRUPTS();     /* probably won't return */
    }

    cancelledByUs = false;

    /* Before we begin, check each gang member for connection errors.
     * An error_message should have been built if there was an error.
     */
	for (x = 0; x < gp->size; x++)
	{
		segdbDesc = &gp->db_descriptors[x];
        if (segdbDesc->error_message.len > 0)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_MPP_QD_QE_CONNECTION_ERROR),
                     errmsg("%s", segdbDesc->error_message.data)));
        }
        if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
        {                       /* error_message wasn't set; shouldn't happen */
			ereport(ERROR,
					(errcode(ERRCODE_MPP_QD_QE_CONNECTION_ERROR),
					 errmsg("Cannot dispatch command to %s, because "
					        "there is no connection.",
                            segdbDesc->whoami)));
        }
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
				elog(NOTICE, "BUG: Reallocating dispatchCommandParmsAr");
										
			dispatchCommandParmsAr =
				(DispatchCommandParms *) repalloc(dispatchCommandParmsAr, (threadCount + max_threads) *
											   sizeof(DispatchCommandParms));
			memset(dispatchCommandParmsAr + threadCount, 0, max_threads * sizeof(DispatchCommandParms));
		}

        /* Create a CdbDispatchResults object and attach it to the Gang. */
        gp->dispatchResults = cdbdisp_makeDispatchResults(gp->size, szCommand);

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
        CdbDispatchResult  *result = &gp->dispatchResults->resultArray[x];

        segdbDesc = &gp->db_descriptors[x];

        /* Save the slice index for error messages related to this QE. */
        cdbconn_setSliceIndex(segdbDesc, sliceIndex);

        /* Make a CdbDispatchResult array entry for this QE. */
        cdbdisp_initResult(result, segdbDesc);

        segdbs_in_thread_pool = x;
        addSegDBToDispatchThreadPool(dispatchCommandParmsAr + threadCount,
									 segdbs_in_thread_pool,
                                     szCommand,
									 cancelFromBelow,
                                     result);
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
		*pgresultsets = (struct pg_result **)calloc(gp->size + 1,
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

}                               /* cdbdisp_dispatchToGang */


/*
 * addSegDBToDispatchThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to have commands dispatched to.
 * It figures out which thread will handle it, based on the setting of
 * mpp_connections_per_thread.
 */
void
addSegDBToDispatchThreadPool(DispatchCommandParms  *ParmsAr,
                             int                    segdbs_in_thread_pool,
                             const char            *szCommand,
                             bool                   cancelFromBelow,
                             CdbDispatchResult     *dispatchResult)
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
			(CdbDispatchResult **)palloc0(mpp_connections_per_thread *
                                          sizeof(CdbDispatchResult *));
		pParms->cancelFromBelow = cancelFromBelow;
		memset(&pParms->thread, 0, sizeof(pthread_t));
		pParms->db_count = 0;
	}

	/*
	 * Just add to the end of the used portion of the dispatchResultPtrArray
	 * and bump the count of members
	 */
	pParms->dispatchResultPtrArray[pParms->db_count++] = dispatchResult;

}                               /* addSegDBToDispatchThreadPool */


/*
 * CdbCheckDispatchResult:
 * Waits for threads that were launched in cdbdisp_dispatchToGang
 * to finish.  Returns true if they all succeeded.
 */
bool                            /* returns true if success on all QEs */
CdbCheckDispatchResult(void)
{
	return CdbCheckDispatchResultInt(NULL, NULL);
}

bool                            /* returns true if success on all QEs */
CdbCheckDispatchResultInt(struct SegmentDatabaseDescriptor   ***failedSegDB,
                          int                                  *numOfFailed)
{
    bool        success = true;
	int			i;
	int			j;
	DispatchCommandParms       *pParms;
    CdbDispatchResult          *dispatchResult;
    SegmentDatabaseDescriptor  *segdbDesc;

    /* No-op if no work was dispatched since the last time we were called.  */
	if (threadCount == 0)
    {
        elog(DEBUG5, "CheckDispatchResult: no threads active");
        return true;
    }

	/*
	 * Wait for threads to finish.
	 */
	for (i = 0; i < threadCount; i++)
	{                           /* loop over threads */
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
        {                       /* loop over QEs managed by one thread */
            dispatchResult = pParms->dispatchResultPtrArray[j];

            Assert(dispatchResult && dispatchResult->segdbDesc);

            segdbDesc = dispatchResult->segdbDesc;

            if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
            {
                /* Message should have been built already.  Just in case... */
                if (dispatchResult->error_message.len == 0)
                {
                    if (!dispatchResult->errcode)
                        dispatchResult->errcode = ERRCODE_MPP_QD_QE_CONNECTION_ERROR;
                    appendPQExpBuffer(&dispatchResult->error_message,
                                      "(%s) CheckDispatchResult: CONNECTION_BAD\n",
                                      segdbDesc->whoami);
                }

                if (failedSegDB != NULL)
                {
                    /* TODO: What memory context is this palloc coming from? */
                    if (*failedSegDB == NULL)
                        *failedSegDB = palloc(sizeof(SegmentDatabaseDescriptor *) *
                                                pParms->db_count);
                    (*failedSegDB)[(*numOfFailed)++] = segdbDesc;
                }
            }

            /* Did someone set error_message without setting errcode? */
            if (dispatchResult->error_message.len > 0 &&
                !dispatchResult->errcode)
            {
                dispatchResult->errcode = ERRCODE_MPP_INTERNAL_ERROR;
                elog(DEBUG2, "(%s) CheckDispatchResult: errcode not set for error_message: %s",
                     segdbDesc->whoami, dispatchResult->error_message.data);
            }

            /* Log the result */
            cdbdisp_debugDispatchResult(dispatchResult, DEBUG2, DEBUG3);

            if (dispatchResult->errcode)
                success = false;

        }                       /* loop over QEs managed by one thread */
    }                           /* loop over threads */

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
                pfree(pParms->dispatchResultPtrArray);
            if (pParms->szCommand)
                pfree(pParms->szCommand);
		}
		pfree(dispatchCommandParmsAr);
        dispatchCommandParmsAr = NULL;
	}
			
	cancelledByUs = false;

	threadCount = 0;

	/*
	 * handle network failures
	 */
	if (numOfFailed != NULL && *numOfFailed > 0)
		FtsHandleNetFailure(*failedSegDB, *numOfFailed);

    elog(DEBUG5, "CheckDispatchResult done");
	return success;
}                               /* cdbdisp_checkDispatchResults */


/* Format and free a Gang's CdbDispatchResults object.
 * Appends error messages to caller's StringInfo buffer.
 * Returns false if some error was found, or true if no errors.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
bool                            /* returns true if ok */
cdbdisp_dumpGangResults(struct Gang    *gang,
                        StringInfo      buffer,      /* OUT */
                        bool            verbose)
{
    CdbDispatchResults *results;
    CdbDispatchResult  *dispatchResult;

    if (!gang ||
        !gang->dispatchResults)
        return true;

    results = gang->dispatchResults;

    /* Find the CdbDispatchResult for the first QE that got an error. */
    dispatchResult = cdbdisp_firstErrorResult(results);
    if (!dispatchResult)
    {
        cdbdisp_destroyGangResults(gang);
        return true;
    }

    /* Format one QE's result. */
    cdbdisp_dumpDispatchResult(dispatchResult, verbose, buffer);

    /* Optionally, format results from the rest of the QEs that got errors. */
    if (verbose)
    {
        CdbDispatchResult  *endp = &results->resultArray[results->resultCount];
        while (++dispatchResult < endp)
            if (dispatchResult->errcode)
                cdbdisp_dumpDispatchResult(dispatchResult, verbose, buffer);
    }

    cdbdisp_destroyGangResults(gang);
    return false;
}                               /* cdbdisp_dumpGangResults */


/* Free a Gang's CdbDispatchResults object. */
void
cdbdisp_destroyGangResults(struct Gang *gang)
{
    if (gang &&
        gang->dispatchResults)
    {
        cdbdisp_destroyDispatchResults(gang->dispatchResults);
        gang->dispatchResults = NULL;
    }
}                               /* cdbdisp_destroyGangResults */


/* Get sum of cmdTuples statistics returned by the successful QEs of a Gang.
 * Used to determine the row counts for insert, update, and delete.
 */
int
cdbdisp_sumGangCmdTuples(struct Gang *gang)
{
    return gang ? cdbdisp_sumCmdTuples(gang->dispatchResults)
                : 0;
}                               /* cdbdisp_sumGangCmdTuples */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchCommand:
 * Send ths szCommand SQL statement to all segdbs in the cluster
 * cancelFromBelow indicates whether an error
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
cdbdisp_dispatchCommand(const char         *szCommand,
                        bool                startDtxTransaction,
                        bool                cancelFromBelow,
                        struct pg_result ***resultSets,         /* OUT */
                        struct Gang       **pPrimaryGang,       /* OUT */
                        struct Gang       **pMirrorGang)        /* OUT */
{
	Gang	   *primaryGang = NULL;
	Gang	   *mirrorGang = NULL;

    elog(DEBUG3, "cdbdisp_dispatchCommand: %.50s", szCommand);

    if (pPrimaryGang)
        *pPrimaryGang = NULL;
    if (pMirrorGang)
        *pMirrorGang = NULL;

    /* Wait for previous command to finish before starting this one. */
    if (threadCount)
    {
        elog(DEBUG3, "%d threads running at entry to cdbdisp_dispatchCommand.  %s",
             threadCount, szCommand);
        CdbCheckDispatchResult();
    }

    if (startDtxTransaction)
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
            * recofig mpp connections
            */
            if (!FtsReConfigureMPP())
                elog(PANIC, "FTS: reconfigMPP failed");
        }
    }

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

	/*
     * Dispatch the command.
     */
    cdbdisp_dispatchToGang(szCommand, primaryGang, -1, cancelFromBelow, resultSets);
	
	if (mirrorGang)
		cdbdisp_dispatchToGang(szCommand, mirrorGang, -1, cancelFromBelow, resultSets);

}                               /* cdbdisp_dispatchCommand */


/* Wait for all QEs to finish, then report any errors from the given gangs.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.  No-op if both gang ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct Gang  *primaryGang,
                      struct Gang  *mirrorGang)
{
    StringInfoData  buf;

    /* If caller didn't allocate gangs, don't wait. */
    if (!primaryGang &&
        !mirrorGang)
        return;

    /* Wait for all QEs to finish, and see if they were all successful. */
    if (CdbCheckDispatchResult())
    {
        /* Upon success, it's not necessary to call cdbdisp_destroyGangResults().
         * Successful results are not big and will be freed eventually when
         * the Gang is reused or destroyed.  But what the heck, it's easy,
         * let's just take care of it now.
         */
        cdbdisp_destroyGangResults(primaryGang);
        cdbdisp_destroyGangResults(mirrorGang);
        return;
    }

    /* Format and free the results from QEs. */
    initStringInfo(&buf);

    cdbdisp_dumpGangResults(primaryGang, &buf, false);

    if (buf.len == 0)
        cdbdisp_dumpGangResults(mirrorGang, &buf, false);
    else
        cdbdisp_destroyGangResults(mirrorGang);

    /* Caller's gangs might have succeeded even though there was
     * an error somewhere in the batch of commands checked by
     * CdbCheckDispatchResult.
     */
    if (buf.len == 0)
    {
        pfree(buf.data);
        return;
    }

    /* Too bad, our gang got an error. */
    PG_TRY();
    {
        ereport(ERROR, (errcode(ERRCODE_MPP_COMMAND_ERROR),
                        errmsg("%s", buf.data) ));
    }
    PG_CATCH();
    {
        pfree(buf.data);
        PG_RE_THROW();
    }
    PG_END_TRY();

    /* not reached */
}                               /* cdbdisp_finishCommand */


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
 */
void *
thread_DispatchCommand(void *arg)
{
	DispatchCommandParms   *pParms = (DispatchCommandParms *) arg;
	int			            db_count = pParms->db_count;
	bool		            cancelFromBelow = pParms->cancelFromBelow;
    CdbDispatchResult      *dispatchResult;
	RunningInfo            *stillRunning = NULL;
	int			            stillRunningCount = 0;
    int		                timeoutCounter = 0;
	int 		            resultCount = 0;	
    int			            i;	
    fd_set                  input_mask, input_mask_copy;
	int                     max_sock;
    int			            sock;

	/*
	 * In threads, we cannot use palloc, because it's not thread safe.
	 */
	stillRunning = (RunningInfo *) calloc(db_count, sizeof(RunningInfo));

	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to send commands through to.
	 */
	for (i = 0; i < db_count; i++)
	{
		/* Don't use elog, it's not thread-safe */
         if (DEBUG5 >= log_min_messages)
              fprintf(stderr, "thread_DispatchCommand working on %d of %d commands\n", i+1, db_count);

        dispatchResult = pParms->dispatchResultPtrArray[i];

		if( shouldStillDispatchCommand(dispatchResult, cancelFromBelow) )
		{
			/* actually kick off the command over the libpq connection. */
			if( dispatchCommand( dispatchResult, pParms->szCommand, cancelFromBelow ) )
			{
				/*
				 * OK, command was successfully submitted. Add to the stillRunning array
				 * so we can monitor it.
				 */
				stillRunning[stillRunningCount].dispatchResult = dispatchResult;
				stillRunningCount++;
			}
		}
	}
	
	max_sock = 0;
    FD_ZERO(&input_mask);
	for (i = 0; i < stillRunningCount; i++)
	{
        dispatchResult = stillRunning[i].dispatchResult;
		sock = PQsocket(dispatchResult->segdbDesc->conn);
		FD_SET(sock, &input_mask);
		max_sock = max_sock < sock ? sock : max_sock;
	}
	
	/*
	 * OK, we are finished submitting the commnad to the segdbs.
	 * Now, we have to wait for them to finish.
	 */
	while (stillRunningCount > 0)
    {                           /* some QEs running */
		int			n;
		struct timeval tv;

		/*
		 * Before doing anything else, see whether any of the remaining connections went down.
		 * Hopefully in this way we can detect a dropped connection quickly.
		 */
		checkForBadConnections(stillRunning, &stillRunningCount, cancelFromBelow);
        if (!stillRunningCount)
            break;

		/*
         * Propagate pending cancellation to QEs if requested.
		 */
        if (QueryCancelPending && cancelFromBelow)
		{
			for (i = 0; i < stillRunningCount; i++)
			{
                dispatchResult = stillRunning[i].dispatchResult;
				if (!dispatchResult->requestedCancel)
				{
                    if (DEBUG3 >= log_min_messages)
                        fprintf(stderr,
                                "thread_DispatchCommand: calling PQrequestCancel for %s %s\n",
                                dispatchResult->segdbDesc->whoami,
                                cancelledByUs ? "cancelledByUs " : "");
					PQrequestCancel(dispatchResult->segdbDesc->conn);
					dispatchResult->requestedCancel = true;
				}
			}
		}

        /* select() IN/OUT parameters need to be reinitialized each time */
        memcpy(&input_mask_copy, &input_mask, sizeof(fd_set));
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        /*
         * Wait for results from QEs.
         */
        n = select(max_sock + 1, &input_mask_copy, NULL, NULL, &tv);

		if (n < 0)
        {
			int sock_errno = SOCK_ERRNO;

            if (sock_errno == EINTR)
				continue;

            handleSelectError( stillRunning, stillRunningCount, sock_errno );
            free(stillRunning);
			return NULL;
        }

		if (n == 0)
        {
			handleSelectTimeout(stillRunning, &stillRunningCount, &timeoutCounter);
			continue;
        }

		if (n > 0)
        {                       /* received something */
			/*
			 * We have data waiting on one or more of the connections.
			 */
			int			x = 0;

            /* CDB TODO: Process results from all sockets that have data,
             *  even those that are still connected but not stillRunning.
             */

            for (i = 0; i < stillRunningCount; i++)
			{
                dispatchResult = stillRunning[i].dispatchResult;
				sock = PQsocket(dispatchResult->segdbDesc->conn);

				if (FD_ISSET(sock, &input_mask_copy))
				{
					processResults(&stillRunning[i],
                                   cancelFromBelow,
                                   pParms->resultSets,
                                   db_count,
                                   &resultCount);
					resultCount++;
				}
				else
				{
					stillRunning[x++] = stillRunning[i];
				}
			}

			stillRunningCount = x;
        }                       /* received something */
    }                           /* some QEs running */

	free(stillRunning);

    if (DEBUG2 >= log_min_messages)
        fflush(stderr);

	return (NULL);
}                               /* thread_DispatchCommand */


/* Helper function to thread_DispatchCommand that decides if we should dispatch
 * to this segment database.
 *
 * (1) don't dispatch if there is already a query cancel notice pending.
 * (2) make sure our libpq connection is still good.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *       thread_DispatchCommand absolutely no elog'ing.
 *
 * cancelledByUS is a global?
 */
bool
shouldStillDispatchCommand( CdbDispatchResult  *dispatchResult,
							bool cancelFromBelow)
{
	SegmentDatabaseDescriptor  *segdbDesc = dispatchResult->segdbDesc;
    PGconn *conn = segdbDesc->conn;
    int     msgoff;
		
    /* If there was a cancellation notice before we submit the command,
     * then don't submit.
     */
    if (QueryCancelPending && !InterruptHoldoffCount)
	{
        /*Don't use elog, it's not thread-safe*/			
        msgoff = dispatchResult->error_message.len;
        appendPQExpBuffer(&dispatchResult->error_message,
					      "(%s) Cancellation pending; command not started.\n",
                          segdbDesc->whoami);						
        if (!dispatchResult->errcode)
            dispatchResult->errcode = ERRCODE_MPP_OPERATION_CANCELED;

        if (DEBUG3 >= log_min_messages)
            fputs(dispatchResult->error_message.data + msgoff, stderr);
		
        /* Don't annoy user with excess messages when canceling due to error. */
        if ( cancelledByUs )
            truncatePQExpBuffer(&dispatchResult->error_message, msgoff);
		
		return false;
	}

	/*
	 * conn should never be NULL, but if it is, or if the status is BAD,
	 * then don't submit.
	 */
	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
        msgoff = dispatchResult->error_message.len;
        if (!dispatchResult->errcode)
            dispatchResult->errcode = ERRCODE_MPP_QD_QE_CONNECTION_ERROR;
		appendPQExpBuffer(&dispatchResult->error_message,
						  "(%s) Connection problem; command not started.  "
                          "connStatus=%d\n",
                          segdbDesc->whoami,
                          conn ? PQstatus(conn) : 0);
						
        /*Don't use elog, it's not thread-safe*/			
        if (DEBUG3 >= log_min_messages)
            fputs(dispatchResult->error_message.data + msgoff, stderr);

        /* Stop other QEs (if caller wants us to). */
        if (cancelFromBelow)
			setCancelFlag();
		
		return false;
	}
	
	return true;
}                                      /* shouldStillDispatchCommand */


/* Helper function to thread_DispatchCommand that actually kicks off the
 * command on the libpq connection.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *       thread_DispatchCommand absolutely no elog'ing.
 *
 * cancelledByUS is a global?
 */
bool
dispatchCommand( CdbDispatchResult *dispatchResult, const char *szCommand,
				 bool cancelFromBelow)
{
    SegmentDatabaseDescriptor  *segdbDesc = dispatchResult->segdbDesc;
	PGconn 					   *conn = segdbDesc->conn;	

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
        int     msgoff = dispatchResult->error_message.len;

        if (!dispatchResult->errcode)
            dispatchResult->errcode = ERRCODE_MPP_QD_QE_CONNECTION_ERROR;

        appendPQExpBuffer(&dispatchResult->error_message,
                          "SQL Command dispatch error: (%s) %s",
                          segdbDesc->whoami,
                          PQerrorMessage(conn));

        /* Don't use elog, it's not thread-safe */
        if (DEBUG1 >= log_min_messages)
            fputs(dispatchResult->error_message.data + msgoff, stderr);

        /* Stop other QEs (if caller wants us to). */
        if (cancelFromBelow)
            setCancelFlag();
		
		/* signal that we had some issues. */
		return false;
	}
	
	return true;
}                                      /* dispatchCommand */


/* Helper function to thread_DispatchCommand that handles errors that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *       thread_DispatchCommand absolutely no elog'ing.
 *
 * cancelledByUS is a global?
 */
void
handleSelectError(RunningInfo  *stillRunning,
                  int           stillRunningCount,
                  int           sock_errno)
{
	int i;
	
    /* Don't use elog, it's not thread-safe */
	if (DEBUG1 >= log_min_messages)
		fprintf(stderr, "thread_DispatchCommand encountered an error during select() with errno: %d\n",
				sock_errno);
				
	/*
	 * Based on the select man page, we could get here with
	 * errno == EBADF (bad descriptor), EINVAL (highest descriptor negative or negative timeout)
	 * or ENOMEM (out of memory).
	 * Since this is most likely a programming error or a bad system
	 * failure, we will just error out all remaining threads.
	 */

	for (i = 0; i < stillRunningCount; i++)
	{
        CdbDispatchResult  *dispatchResult = stillRunning[i].dispatchResult;	
        dispatchResult->errcode = ERRCODE_MPP_QD_QE_CONNECTION_ERROR;
        appendPQExpBuffer(&dispatchResult->error_message,
                          "(%s) Socket select returned error %d, command status unknown.\n",
                          dispatchResult->segdbDesc->whoami, sock_errno);
	}

    /* No point in trying to cancel the other QEs with select() broken. */
}                                      /* handleSelectError */


void checkForBadConnections( RunningInfo *stillRunning ,int *stillRunningCount, bool cancelFromBelow)
{
	int x = 0;
	int i;
	
	for (i = 0; i < *stillRunningCount; i++)
	{
        CdbDispatchResult  *dispatchResult = stillRunning[i].dispatchResult;	

		if (PQstatus(dispatchResult->segdbDesc->conn) == CONNECTION_BAD)
		{
            if (!dispatchResult->errcode)
                dispatchResult->errcode = ERRCODE_MPP_QD_QE_CONNECTION_ERROR;

            appendPQExpBuffer(&dispatchResult->error_message,
							  "(%s) SQL Command error: lost connection\n",
							  dispatchResult->segdbDesc->whoami);

            /* Stop other QEs (if caller wants us to). */
            if (cancelFromBelow)
				setCancelFlag();
		}
		else
		{
			stillRunning[x++] = stillRunning[i];
		}
	}

	*stillRunningCount = x;
}                                      /* checkForBadConnections */



/* Helper function to thread_DispatchCommand that handles timeouts that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *       thread_DispatchCommand absolutely no elog'ing.
 *
 * cancelledByUS is a global?
 */
void
handleSelectTimeout(RunningInfo    *stillRunning,
                    int            *stillRunningCount,
                    int            *timeoutCounter)
{
    CdbDispatchResult          *dispatchResult;
    SegmentDatabaseDescriptor  *segdbDesc;
	int     i;
	
	/*
	 * check the connection still valid, set 1 min time interval
	  * this may affect performance, should turn it off if required.
	 */

	if ((*timeoutCounter)++ > 30)
	{
		int			x = 0;
		bool		sampling = true;

        *timeoutCounter = 0;

		for (i = 0; i < *stillRunningCount; i++)
		{
            dispatchResult = stillRunning[i].dispatchResult;	
			segdbDesc = dispatchResult->segdbDesc;
			if (!FtsTestConnection(segdbDesc->segment_database_info, segdbDesc->conn->dbName))
			{
                if (!dispatchResult->errcode)
                    dispatchResult->errcode = ERRCODE_MPP_QD_QE_CONNECTION_ERROR;

                appendPQExpBuffer(&dispatchResult->error_message,
							      "(%s) SQL command error: lost connection\n",
                                  segdbDesc->whoami);

                /* CDB TODO: Are we allowed to store into the PGconn object? */
                segdbDesc->conn->status = CONNECTION_BAD;
				sampling = false;
				
				/* free connection and PQfinish() is done in ftsReconnect*/

			}
			else
			{
				 /* for performance reason, we do just sampling here
				  * for now, the first one is picked.
				  */
				 if (sampling)
				 {
					 x = *stillRunningCount;
					 break;
				 }
				stillRunning[x++] = stillRunning[i];
			}
		}
		*stillRunningCount = x;
	}

    if (DEBUG2 >= log_min_messages &&
        (*timeoutCounter & 0x07) == 1)
        fflush(stderr);

}                                      /* handleSelectTimeout */


void
processResults(RunningInfo         *runningInfo,
               bool                 cancelFromBelow,
               struct pg_result   **pqresults,
               int                  db_count,
               int                 *resultCount)
{
	CdbDispatchResult          *dispatchResult = runningInfo->dispatchResult;
    SegmentDatabaseDescriptor  *segdbDesc = dispatchResult->segdbDesc;
	PGconn					   *conn = segdbDesc->conn;
	struct pg_result           *pRes;

	/*
	 * Our strategy is once we have some data for a socket,
	 * wait for it all, since this function is used for commands
	 * that really only return 1 result, not set returning commands
	 * that might return many.
	 */

	/*
	 * It is critical that for any connection that had an asynchronous
	 * command sent thru it, we call PQgetResult until it returns NULL.
	 * Otherwise, the next time a command is sent to that connection,
	 * it will return an error that there's a command pending.
	 */
	while (NULL != (pRes = PQgetResult(conn)))
	{                           /* while more results */
        ExecStatusType  resultStatus = PQresultStatus(pRes);

        /* Attach the PGresult object to the CdbDispatchResult object. */
        if (!pqresults)
            cdbdisp_appendResult(dispatchResult, pRes);

        /* Stuff PGresult ptr into caller's array. */
        else
        {
            /* check array index is in bounds */
            Assert(*resultCount < db_count);
            /* TODO: not handling multiple result sets */
            Assert(pqresults[*resultCount] == NULL);
            pqresults[*resultCount++] = pRes;
        }

        /*
         * Successful completion of a command?
         */
        if (resultStatus == PGRES_COMMAND_OK ||
            resultStatus == PGRES_TUPLES_OK)
        {
            /* Save the index of the last successful PGresult.
             * Can be given to cdbdisp_getPGresult() to get tuple count, etc.
             */
            dispatchResult->okindex = cdbdisp_numPGresult(dispatchResult) - 1;

            /* Don't use elog, it's not thread-safe */
            if (DEBUG3 >= log_min_messages)
            {
                char   *cmdStatus = PQcmdStatus(pRes);
                fprintf(stderr, "cdbdisp_doCommand: ok from %s %s\n",
                        segdbDesc->whoami,
                        cmdStatus ? cmdStatus : "");
            }
        }

        /*
         * Note QE error.  Cancel the whole statement if requested.
         */
        else
        {                       /* QE reported an error */
            char   *errorMessage = PQerrorMessage(conn);
            char   *sqlstate = PQresultErrorField(pRes, PG_DIAG_SQLSTATE);
            int     errcode = 0;

            /* Don't use elog, it's not thread-safe */
            if (DEBUG2 >= log_min_messages)
                fprintf(stderr,
                        "cdbdisp_doCommand: Error from %s -> %s %s %s",
                        segdbDesc->whoami,
                        PQresStatus(resultStatus),
                        sqlstate ? sqlstate : "(no SQLSTATE)",
                        errorMessage ? errorMessage : "");

            /* Convert SQLSTATE to an error code (ERRCODE_xxx).
             * Use a generic nonzero error code if no SQLSTATE.
             */
            if (sqlstate &&
                strlen(sqlstate) == 5)
                errcode = cdbdisp_sqlstate_to_errcode(sqlstate);
            if (!errcode)
                errcode = ERRCODE_INTERNAL_ERROR;   /* punt */

            /* If this is the first error, save the error code
             * and the index of the PGresult buffer entry.
             */
            if (!dispatchResult->errcode)
            {
                dispatchResult->errcode = errcode;
                if (!pqresults)
                    dispatchResult->errindex = cdbdisp_numPGresult(dispatchResult) - 1;
            }

            /* Tell any remaining QEs to stop (at caller's option). */
            if (cancelFromBelow)
                setCancelFlag();
        }                       /* QE reported an error */
    }                           /* while more results */
}                               /* processResults */


/*--------------------------------------------------------------------*/


/*
 * CdbDoCommandInt:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommandInt(const char         *szCommand,
                bool                cancelFromBelow,
                struct pg_result ***resultSets)
{
    Gang   *primaryGang;
    Gang   *mirrorGang;

    cdbdisp_dispatchCommand(szCommand, false, cancelFromBelow, resultSets,
                            &primaryGang, &mirrorGang);

    /* Wait for all QEs to finish.
     * If not all of our QEs were successful, report the error and throw up.
     */
    cdbdisp_finishCommand(primaryGang, mirrorGang);
}                               /* CdbDoCommandInt */


/*
 * CdbDoCommand:
 * Called by general users, this method includes global transaction control.
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommand(const char *szCommand, bool cancelFromBelow)
{
    Gang   *primaryGang;
    Gang   *mirrorGang;

    cdbdisp_dispatchCommand(szCommand, true, cancelFromBelow, NULL,
                            &primaryGang, &mirrorGang);

    /* Wait for all QEs to finish.
     * If not all of our QEs were successful, report the error and throw up.
     */
    cdbdisp_finishCommand(primaryGang, mirrorGang);
}


/*
 * setCancelFlag:
 * This utility function is called to signal to postgres that
 * a cancel is requested, similar to what happens when a ctrl-C
 * is pressed (StatementCancelHandler)
 */
void
setCancelFlag(void)
{
    cancelledByUs = true;
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}
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
	 * need to start distributed transaction
	 */
	condStartDtxTransaction();
	
    /* Wait for previous command to finish before starting this one. */
    if (threadCount)
    {
        elog(DEBUG3, "%d threads running at entry to cdbdisp_dispatchPlan",
             threadCount);
        CdbCheckDispatchResult();
    }

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
}                               /* cdbdisp_dispatchPlan */


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
    ListCell       *sliceCell;

    if (!queryDesc ||
        !queryDesc->estate ||
        !queryDesc->estate->es_sliceTable)
        return;

    foreach(sliceCell, queryDesc->estate->es_sliceTable->slices)
    {
        Slice  *slice = (Slice *)lfirst(sliceCell);

        /* Ignore QD slices. */
        if (slice->gangType == GANGTYPE_UNALLOCATED)
            continue;

        /* Free results of primary gang. */
        cdbdisp_destroyGangResults(slice->primaryGang);

        /* Free results of mirror gang. */
        if (queryDesc->operation != CMD_SELECT)
            cdbdisp_destroyGangResults(slice->mirrorGang);
    }
}                               /* cdbdisp_destroyPlanResults */


/* Check and free the results of all gangs in a query plan's slice table.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
void
cdbdisp_dumpPlanResults(struct QueryDesc *queryDesc)
{
    SliceTable     *sliceTable = queryDesc->estate->es_sliceTable;
    ListCell       *sliceCell;
    StringInfoData  buf;

    initStringInfo(&buf);

    /* For each slice, see if all of the QEs were successful. */
    foreach(sliceCell, sliceTable->slices)
    {
        Slice  *slice = (Slice *)lfirst(sliceCell);

        /* Ignore QD slices. */
        if (slice->gangType == GANGTYPE_UNALLOCATED)
            continue;

        /* Check (and free) results of primary gang. */
        cdbdisp_dumpGangResults(slice->primaryGang, &buf, false);

        /* Check (and free) results of mirror gang. */
        if (buf.len == 0 &&
            queryDesc->operation != CMD_SELECT)
            cdbdisp_dumpGangResults(slice->mirrorGang, &buf, false);
        else
            cdbdisp_destroyGangResults(slice->mirrorGang);
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
        ereport(ERROR, (errcode(ERRCODE_MPP_COMMAND_ERROR),
                        errmsg("%s", buf.data) ));
    }
    PG_CATCH();
    {
        pfree(buf.data);
        PG_RE_THROW();
    }
    PG_END_TRY();

    /* not reached */
}                               /* cdbdisp_dumpPlanResults */


/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs.  To wait for completion
 * and check for errors, it is suggested that the caller use
 * cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchUtilityStatement(struct Node   *stmt,
                                 bool           cancelFromBelow,
                                 struct Gang  **pPrimaryGang,  /* OUT */
                                 struct Gang  **pMirrorGang)   /* OUT */
{
    char	   *szCommand;

	/*
	 * Serialize the stmt tree, and create the sql statement: mppexec ....
	 */
	Query	   *q = makeNode(Query);

	StringInfoData buffer;

	initStringInfo(&buffer);

	q->commandType = CMD_UTILITY;

    Assert(stmt!=NULL);
    Assert(stmt->type < 1000);
    Assert(stmt->type > 0);

	q->utilityStmt = stmt;
	
    q->querySource = QSRC_ORIGINAL;

    /* We must set q->canSetTag = true.  False would be used to hide a
     * command introduced by rule expansion which is not allowed to return
     * its completion status in the command tag (PQcmdStatus/PQcmdTuples).
     * For example, if the original unexpanded command was SELECT, the
     * status should come back as "SELECT n" and should not reflect other
     * commands inserted by rewrite rules.  True means we want the status.
     */
    q->canSetTag = true;		/* ? */

	szCommand = serializeNode((Node *) q);

	Assert(szCommand != NULL);

	appendStringInfo(&buffer, "mppexec '%s' ''", szCommand);

	pfree(szCommand);

	cdbdisp_dispatchCommand(buffer.data, true, cancelFromBelow, NULL,
                            pPrimaryGang, pMirrorGang);

    pfree(buffer.data);
}                               /* cdbdisp_dispatchUtilityStatement */


void
CdbDispatchUtilityStatement(struct Node *stmt)
{
    Gang   *primaryGang;
    Gang   *mirrorGang;

    cdbdisp_dispatchUtilityStatement(stmt, true, &primaryGang, &mirrorGang);

    /* Wait for all QEs to finish.  Throw up if error. */
    cdbdisp_finishCommand(primaryGang, mirrorGang);

}                               /* CdbDispatchUtilityStatement */


