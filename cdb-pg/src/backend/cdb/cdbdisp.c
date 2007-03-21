/*-------------------------------------------------------------------------
 *
 * cdbdisp.c
 *	  Functions to dispatch commands to QExecutors
 *
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <pthread.h>
#include <limits.h>

#include "executor/execdesc.h"	/* QueryDesc */
#include "storage/ipc.h"		/* For proc_exit_inprogress  */
#include "miscadmin.h"
#include "utils/memutils.h"

#include "utils/tqual.h" 			/*for the snapshot */
#include "storage/proc.h"  			/* MyProc */
#include "access/xact.h"  			/*for GetCurrentTransactionId */


#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "utils/datum.h"
#include "executor/executor.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "tcop/tcopprot.h"
#include "cdb/cdbplan.h"

#include "cdb/cdbselect.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"
#include "cdb/cdblink.h"		/* just for our CdbProcess population hack. */
#include "cdb/cdbsrlz.h"
#include "cdb/cdbsubplan.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbllize.h"

//#undef socket
//#undef accept
//#undef connect
//#undef select
//#undef recv
//#undef send

#ifndef _WIN32
#define mythread() ((unsigned int) pthread_self())
#else
#define mythread() ((unsigned int) pthread_self().p)
#endif 

/*
 * Static Helper functions
 */
static void *thread_DispatchCommand(void *arg);


static bool
shouldStillDispatchCommand(CdbDispatchResult * dispatchResult);

static
void dispatchCommand(CdbDispatchResult *dispatchResult,
                     const char        *query_text,
                     int 			  	query_len,
                     const char        *debug_text);
							
static void
handleSelectError(DispatchCommandParms *pParms,
                  int                   db_count,
                  int                   sock_errno);							

static void
handleSelectTimeout(DispatchCommandParms   *pParms,
                    int                     db_count,
                    int                    *timeoutCounter);

static bool                     /* returns true if command complete */
processResults(CdbDispatchResult   *dispatchResult);


static void
addSegDBToDispatchThreadPool(DispatchCommandParms  *ParmsAr,
                             int                    segdbs_in_thread_pool,
							 const char            *strCommand,
							 char			       *serializedQuerytree,
							 int					serializedQuerytreelen,
							 char 		           *serializedPlantree,
							 int					serializedPlantreelen,
							 char			       *serializedParams,
							 int					serializedParamslen,
							 char			       *serializedSliceInfo,	
							 int					serializedSliceInfolen,		
							 char			       *serializedSnapshot,
							 int					serializedSnapshotlen,
							 int					txnOptions,
							 int					sliceId,
							 CdbDispatchResult     *dispatchResult);

static void
cdbdisp_dispatchCommandToAllGangs(const char                 *strCommand,
						char			   			*serializedQuerytree,
						int							 serializedQuerytreelen,
						char	 		   			*serializedPlantree,
						int							 serializedPlantreelen,
                        bool                        cancelOnError,
                        bool						startTransaction,
                        struct CdbDispatchResults **pPrimaryResults,
                        struct CdbDispatchResults **pMirrorResults,
						struct CdbDispatchCmdThreads  **pDispatchThreads);

/*
 * We need an array describing the relationship between a slice and
 * the number of "child" slices which depend on it.
 */
typedef struct {
	int sliceIndex;
	int children;
	Slice *slice;
} sliceVec;

static int fillSliceVector(SliceTable * sliceTable, int sliceIndex, sliceVec *sliceVector, int len);

/* determines which dispatchOptions need to be set. */
static int generateTxnOptions( bool shouldStart );

typedef struct
{
	bool		single_row_insert;
}	pre_dispatch_function_evaluation_context;

static Node *pre_dispatch_function_evaluation_mutator(Node *node,
						 pre_dispatch_function_evaluation_context * context);

/* ====================================================
 * STATIC STATE VARIABLES should not be declared!
 * global state will break the ability to run cursors.
 * only globals with a higher granularity than a running
 * command (i.e: transaction, session) are ok.
 * ==================================================== */

static volatile bool pleaseCancel = false; /* (transaction granularity) */

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
 * Send the strCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
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
 * When dispatchResults->cancelOnError is false, strCommand is to be
 * dispatched to every connected gang member if possible, despite any
 * cancellation requests, QE errors, connection failures, etc.
 *
 * This function is passing out a pointer to the newly allocated
 * CdbDispatchCmdThreads object. It holds the dispatch thread information
 * including an array of dispatch thread commands. It gets destroyed later
 * on when the command is finished, along with the DispatchResults objects.
 *
 * NB: This function should return normally even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 *
 * Note: the maxSlices argument is used to allocate the parameter
 * blocks for dispatch, it should be set to the maximum number of
 * slices in a plan. For dispatch of single commands (ie most uses of
 * cdbdisp_dispatchToGang()), setting it to 1 is fine.
 */
void
cdbdisp_dispatchToGang(struct CdbDispatchCmdThreads **dispatchThreads, /* OUT */
					   const char                  *strCommand,
					   char						   *serializedQuerytree,
					   int							serializedQuerytreelen,
					   char 					   *serializedPlantree,
					   int							serializedPlantreelen,
					   char						   *serializedParams,
					   int							serializedParamslen,
					   char						   *serializedSliceInfo,
					   int							serializedSliceInfolen,				
					   char						   *serializedSnapshot,
					   int							serializedSnapshotlen,
					   int							txnOptions,
                       struct Gang                 *gp,
                       struct CdbDispatchResults   *dispatchResults,
                       int                          sliceIndex,
                       unsigned int                 maxSlices)
{
	SegmentDatabaseDescriptor *segdbDesc;
	int	i,
		max_threads,
		segdbs_in_thread_pool = 0,
		x,
		newThreads = 0;	

	MemoryContext oldContext;

	Assert(Mpp_role == MPP_ROLE_DISPATCH &&
		   gp &&
		   gp->size > 0 &&
		   dispatchResults &&
		   dispatchResults->resultArray);

	if (DEBUG2 >= log_min_messages)
	{
		if (sliceIndex >= 0)
			elog(DEBUG2, "dispatchToGang: sliceIndex=%d gangsize=%d  %.100s",
				 sliceIndex, gp->size, strCommand);
		else
			elog(DEBUG2, "dispatchToGang: gangsize=%d  %.100s",
				 gp->size, strCommand);
	}

	/*
	 * The most threads we could have is segdb_count / gp_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / gp_connections_per_thread.
	 * We allocate enough memory for this many DispatchCommandParms structures,
	 * even though we may not use them all.
	 *
	 * We can only use gp->size here if we're not dealing with a
	 * singleton gang. It is safer to use the actual segment count.
	 */
	Assert(gp_connections_per_thread > 0);

	segdbs_in_thread_pool = 0;
	max_threads = 1 + (getMPPSegmentCount() - 1) / gp_connections_per_thread;

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	if (!(*dispatchThreads))
	{
		/* the maximum number of command parameter blocks we'll possibly need is
		 * one for each slice on the primary and mirror gangs. Max sure that we
		 * have enough -- once we've created the command block we're stuck with it
		 * for the duration of this statement (including CDB-DTM ). 
		 * 1 * maxthreads * slices for each primary
		 * 1 * maxthreads * slices for each mirror
		 * X 2 for good measure ? */
		int paramCount = max_threads * 4 * Max(maxSlices, 5);

		elog(DEBUG4, "dispatcher: allocating command array with maxslices %d paramCount %d", maxSlices, paramCount);
			
		*dispatchThreads = cdbdisp_makeDispatchThreads(paramCount);
	}
	else
	{
		/* If we attempt to reallocate, there is a race here: we
		 * know that we have threads running using the
		 * dispatchCommandParamsAr! If we reallocate we
		 * potentially yank it out from under them! Don't do
		 * it! */
		if ((*dispatchThreads)->dispatchCommandParmsArSize < ((*dispatchThreads)->threadCount + max_threads))
		{
			elog(ERROR, "Attempted to reallocate dispatchCommandParmsAr while other threads still running size %d new threadcount %d", (*dispatchThreads)->dispatchCommandParmsArSize, ((*dispatchThreads)->threadCount + max_threads));
		}
	}

	MemoryContextSwitchTo(oldContext);

	/*
	 * Create the thread parms structures based targetSet parameter.
	 * This will add the segdbDesc pointers appropriate to the
	 * targetSet into the thread Parms structures, making sure that each thread
	 * handles gp_connections_per_thread segdbs.
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

		addSegDBToDispatchThreadPool((*dispatchThreads)->dispatchCommandParmsAr + (*dispatchThreads)->threadCount,
									 x,
                                     strCommand,
                                     serializedQuerytree, serializedQuerytreelen,
									 serializedPlantree, serializedPlantreelen,
									 serializedParams, serializedParamslen,
									 serializedSliceInfo, serializedSliceInfolen,
                                     serializedSnapshot, serializedSnapshotlen,
                                     txnOptions,
									 sliceIndex,
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
	 * thread pool, knowing that each thread handles gp_connections_per_thread
	 * segdbs.
	 */
	newThreads += 1 + (segdbs_in_thread_pool - 1) / gp_connections_per_thread;

	/*
	 * Create the threads. (which also starts the dispatching).
	 */
	for (i = 0; i < newThreads; i++)
	{
		int		pthread_err = 0;
		DispatchCommandParms *pParms = &((*dispatchThreads)->dispatchCommandParmsAr + (*dispatchThreads)->threadCount)[i];

 		/* attributes of the thread we're creating */
		pthread_attr_t t_atts;

		Assert(pParms != NULL);

		elog(DEBUG4, "dispatchToGang: Creating thread %d of %d for CommandDispatch",
			 i + 1, newThreads);

		/* save ourselves some memory: the defaults for thread stack
		 * size are large (1M+) */
		pthread_attr_init(&t_atts);
#ifdef pg_on_solaris
		/* Solaris doesn't have PTHREAD_STACK_MIN ? */
		pthread_attr_setstacksize(&t_atts, (64*1024));
#else
		pthread_attr_setstacksize(&t_atts, Max(PTHREAD_STACK_MIN, (64*1024)));
#endif

		pthread_err = pthread_create(&pParms->thread, &t_atts, thread_DispatchCommand, pParms);

		pthread_attr_destroy(&t_atts);
		if (pthread_err != 0)
		{
			int j;

			/* Error during thread create (this should be caused by
			 * resource constraints). If we leave the threads running,
			 * they'll immediately have some problems -- so we need to
			 * join them, and *then* we can issue our FATAL error */

			pleaseCancel = true;

			for (j = 0; j < (*dispatchThreads)->threadCount + (i - 1); j++)
			{
				DispatchCommandParms *pParms;

				pParms = &(*dispatchThreads)->dispatchCommandParmsAr[j];
				pthread_join(pParms->thread, NULL);
			}

			ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("dispatchToGang: failed to create thread %d of %d", i + 1, newThreads),
							errdetail("pthread_create() failed with err %d", pthread_err),
							errhint("gp_connections_per_thread controls the number of threads created during parallel dispatch, current setting is %d increase the setting to spawn fewer threads", gp_connections_per_thread)));
		}

	}

	(*dispatchThreads)->threadCount += newThreads;
	elog(DEBUG4, "dispatchToGang: Total threads now %d",
		 (*dispatchThreads)->threadCount);
}	/* cdbdisp_dispatchToGang */


/*
 * addSegDBToDispatchThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to have commands dispatched to.
 * It figures out which thread will handle it, based on the setting of
 * gp_connections_per_thread.
 */
void
addSegDBToDispatchThreadPool(DispatchCommandParms  *ParmsAr,
                             int                    segdbs_in_thread_pool,
                             const char        	   *strCommand,
                             char			   	   *serializedQuerytree,
                             int					serializedQuerytreelen,
							 char 		  		   *serializedPlantree,
							 int					serializedPlantreelen,
							 char			   	   *serializedParams,
							 int					serializedParamslen,
							 char			   	   *serializedSliceInfo,	
							 int					serializedSliceInfolen,		
                             char			   	   *serializedSnapshot,
                             int					serializedSnapshotlen,
                             int					txnOptions,
                             int					sliceId,
                             CdbDispatchResult     *dispatchResult)
{
	DispatchCommandParms *pParms;
	int			ParmsIndex;

	/*
	 * The proper index into the DispatchCommandParms array is computed, based on
	 * having gp_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % gp_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	ParmsIndex = segdbs_in_thread_pool / gp_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];

	if (segdbs_in_thread_pool % gp_connections_per_thread == 0)
	{

		if (strCommand == NULL || strlen(strCommand) == 0)
		{	
			pParms->strCommand = NULL;
			pParms->strCommandlen = 0;
		}
		else
		{
			pParms->strCommand = strCommand;
			pParms->strCommandlen = strlen(strCommand) + 1;
		}
		
		if (serializedQuerytree == NULL || serializedQuerytreelen == 0)
		{	
			pParms->serializedQuerytree = NULL;
			pParms->serializedQuerytreelen = 0;
		}
		else
		{
			pParms->serializedQuerytree = serializedQuerytree;			
			pParms->serializedQuerytreelen = serializedQuerytreelen;
		}
		
		if (serializedPlantree == NULL || serializedPlantreelen == 0)
		{	
			pParms->serializedPlantree = NULL;
			pParms->serializedPlantreelen = 0;
		}
		else
		{
			pParms->serializedPlantree = serializedPlantree;
			pParms->serializedPlantreelen = serializedPlantreelen;
		}
		
		if (serializedParams == NULL || serializedParamslen == 0)
		{	
			pParms->serializedParams = NULL;
			pParms->serializedParamslen = 0;
		}
		else
		{
			pParms->serializedParams = serializedParams;
			pParms->serializedParamslen = serializedParamslen;
		}
		
		if (serializedSliceInfo == NULL || serializedSliceInfolen == 0)
		{	
			pParms->serializedSliceInfo = NULL;
			pParms->serializedSliceInfolen = 0;
		}
		else
		{
			pParms->serializedSliceInfo = serializedSliceInfo;
			pParms->serializedSliceInfolen = serializedSliceInfolen;
		}
		
		if (serializedSnapshot == NULL || serializedSnapshotlen == 0)
		{
			pParms->serializedSnapshot = NULL;
			pParms->serializedSnapshotlen = 0;
		}
		else
		{
			pParms->serializedSnapshot = serializedSnapshot;
			pParms->serializedSnapshotlen = serializedSnapshotlen;
		}
		
		pParms->txnOptions = txnOptions;
		pParms->cmdID = gp_command_count;
		pParms->localSlice = sliceId;
		pParms->dispatchResultPtrArray =
			(CdbDispatchResult **) palloc0(gp_connections_per_thread *
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
CdbCheckDispatchResult(struct CdbDispatchCmdThreads  *dispatchThreads, bool cancelUnfinishedWork)
{
	CdbCheckDispatchResultInt(dispatchThreads, NULL, NULL, cancelUnfinishedWork);
}

void
CdbCheckDispatchResultInt(struct CdbDispatchCmdThreads  *dispatchThreads,
						  struct SegmentDatabaseDescriptor *** failedSegDB,
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
	if (!dispatchThreads || dispatchThreads->threadCount == 0)
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
	for (i = 0; i < dispatchThreads->threadCount; i++)
	{							/* loop over threads */
		int			pthread_err = 0;

		pParms = &dispatchThreads->dispatchCommandParmsAr[i];
		Assert(pParms != NULL);

		elog(DEBUG4, "CheckDispatchResult: Joining to thread %d of %d",
			 i + 1, dispatchThreads->threadCount);

		pthread_err = pthread_join(pParms->thread, NULL);
		if (pthread_err != 0)
			elog(FATAL, "CheckDispatchResult: pthread_join failed on thread %d (0x%x) of %d (returned %d attempting to join to 0x%x)",
				 i + 1, 
#ifndef _WIN32
				 (unsigned int) pParms->thread, 
#else
				 (unsigned int) pParms->thread.p,
#endif
				 dispatchThreads->threadCount, pthread_err, mythread());

		memset(&pParms->thread, 0, sizeof(pParms->thread));

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

	/* reset thread state (will be destroyed later on in finishCommand) */
    dispatchThreads->threadCount = 0;
			
	/* Reset dispatcher state. */
	pleaseCancel = false;

	/* It looks like everything went fine, make sure we don't miss a
	 * user cancellation ?
	 *
	 * The cancelUnfinishedWork argument is true when we're handling
	 * an error, false when we are doing "normal work" */
	if (!cancelUnfinishedWork)
		CHECK_FOR_INTERRUPTS();
}	/* cdbdisp_checkDispatchResult */


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
cdbdisp_dispatchRMCommand(const char *strCommand,
						  bool withSnapshot,
						  bool dispatchToMirrors,
						  StringInfo errmsgbuf,
						  int *numresults)

{
	CdbDispatchResults *gangResults;
	CdbDispatchResults *primaryResults;
	CdbDispatchResults *mirrorResults;
	CdbDispatchResult *dispatchResult;
	CdbDispatchCmdThreads  *dispatchThreads;
	
	PGresult  **resultSets = NULL;
	int			nslots;
	int			nresults = 0;
	int			i;
	int			totalResultCount=0;
	
	/* never want to start a global transaction for these */
	bool		startTransaction = false;

	/* Launch the command.	Don't cancel on error. */
	cdbdisp_dispatchCommand(strCommand, NULL, 0,
							/* cancelOnError */false,
							startTransaction, withSnapshot, 
							dispatchToMirrors, /* disp to primaries*/ true,
							&primaryResults, &mirrorResults, &dispatchThreads);

	/* Wait for all QEs to finish.	Don't cancel. */
	CdbCheckDispatchResult(dispatchThreads, false);

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
		totalResultCount = gangResults->resultCount;

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
		totalResultCount += gangResults->resultCount;
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

	/* free memory allocated for the dispatch threads struct */
	cdbdisp_destroyDispatchThreads(dispatchThreads);
	
	/* Put a stopper at the end of the array. */
	Assert(nresults < nslots);
	resultSets[nresults] = NULL;

	/* If our caller is interested, tell them how many sets we're returning. */
	if (numresults != NULL)
		*numresults = totalResultCount;

	return resultSets;
}	/* cdbdisp_dispatchRMCommand */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_dispatchCommand:
 * Send the strCommand SQL statement to all segdbs in the cluster
 * cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
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
cdbdisp_dispatchCommand(const char                 *strCommand,
						char			   		   *serializedQuerytree,
						int							serializedQuerytreelen,
                        bool                        cancelOnError,
                        bool						startTransaction,
                        bool						withSnapshot,
                        bool						dispatchToMirrors,
						bool						dispatchToPrimaries,
                        struct CdbDispatchResults **pPrimaryResults,    /* OUT */
                        struct CdbDispatchResults **pMirrorResults,     /* OUT */
						struct CdbDispatchCmdThreads **pDispatchThreads)    /* OUT */
{
	Gang	*primaryGang;
	Gang	*mirrorGang;
	int		txnOptions;
	char	*serializedSnapshot = NULL;
	int		serializedSnapshotlen = 0;
	int		nsegdb = getMPPSegmentCount();
	
	Assert(dispatchToPrimaries || dispatchToMirrors);
	
	if (DEBUG5 >= log_min_messages)
    	elog(DEBUG3, "cdbdisp_dispatchCommand: %s", strCommand);
    else
    	elog(DEBUG3, "cdbdisp_dispatchCommand: %.50s", strCommand);

    *pPrimaryResults = NULL;
    if (pMirrorResults)
		*pMirrorResults = NULL;

	*pDispatchThreads = NULL;
	
	/*
	 * Allocate a primary and mirror QE for every available segDB in the system.
	 */
	primaryGang = allocateGang(GANGTYPE_PRIMARY_WRITER, nsegdb, -1, NULL);

	Assert(primaryGang);

	mirrorGang = allocateGang(GANGTYPE_MIRROR_WRITER, nsegdb, -1, NULL);

	if (mirrorGang &&
		(mirrorGang->segdb_count < nsegdb || mirrorGang->size < nsegdb))
	{
		elog(LOG, "Some segments are down: MIRROR nsegdb %d mirrrorGang->segdb_count: %d mirrorGang->size: %d", nsegdb, mirrorGang->segdb_count, mirrorGang->size);
		elog(LOG, "                       PRIMARY nsegdb %d mirrrorGang->segdb_count: %d mirrorGang->size: %d", nsegdb, primaryGang->segdb_count, primaryGang->size);
		elog(LOG, "Some segments are down.  Dispatching command %.50s to remaining segments only", strCommand);

		if (strncmp(strCommand, "BEGIN ", 6) != 0 &&
			strncmp(strCommand, "PREPARE ", 8) != 0 &&
			strncmp(strCommand, "COMMIT ", 6) != 0 &&
			strncmp(strCommand, "ROLLBACK ", 9) != 0 &&
			strncmp(strCommand, "select pg_relation_size(", 24) != 0)
		{
			if (strncmp(strCommand, "MPPEXEC ", 7) == 0 || strncmp(strCommand, "mppexec ", 7) == 0)
				elog(NOTICE, "Some segments are down.  Dispatching command to remaining segments only");
			else
				elog(NOTICE, "Some segments are down.  Dispatching command %.50s to remaining segments only", strCommand);
		}
	}

	/* Serialize a version of our snapshot */
	if (withSnapshot)
		serializedSnapshot = serializeSnapshot(&serializedSnapshotlen);
	else
	{
		serializedSnapshot = NULL;
		serializedSnapshotlen = 0;
	}

	/* generate our transactions options. */
	txnOptions = generateTxnOptions(startTransaction);
	
	/*
     * Dispatch the command.
     */
	if (dispatchToPrimaries)
	{
		*pPrimaryResults = cdbdisp_makeDispatchResults(nsegdb, 0, cancelOnError);
		cdbdisp_dispatchToGang(pDispatchThreads, strCommand, 
								serializedQuerytree, serializedQuerytreelen, 
								NULL, 0, NULL, 0, NULL, 0,
							   serializedSnapshot, serializedSnapshotlen,
							   txnOptions, primaryGang, *pPrimaryResults, -1, 1);
	}
	
	if (dispatchToMirrors && mirrorGang)
    {
        *pMirrorResults = cdbdisp_makeDispatchResults(nsegdb, 0, cancelOnError);
        cdbdisp_dispatchToGang(pDispatchThreads, strCommand, 
        						serializedQuerytree, serializedQuerytreelen, 
        						NULL, 0, NULL, 0, NULL, 0,
							   serializedSnapshot, serializedSnapshotlen,
							   txnOptions, mirrorGang, *pMirrorResults, -1, 1);
    }

	/*
	 * don't pfree serializedShapshot here, it will be pfree'd when
	 * the first thread is destroyed.
	 */

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
					  struct CdbDispatchResults * mirrorResults,
					  struct CdbDispatchCmdThreads * dispatchThreads)
{
	StringInfoData buf;
	int			errorcode = 0;

	/* If cdbdisp_dispatchToGang() wasn't called, don't wait. */
	if (!primaryResults &&
		!mirrorResults)
		return;

	/* Wait for all QEs to finish. Don't cancel them. */
	CdbCheckDispatchResult(dispatchThreads, false);

	/* If no errors, free the CdbDispatchResults objects and return. */
	if (primaryResults)
		errorcode = primaryResults->errcode;
	if (!errorcode && mirrorResults)
		errorcode = mirrorResults->errcode;

	if (!errorcode)
	{
		cdbdisp_destroyDispatchResults(primaryResults);
		cdbdisp_destroyDispatchResults(mirrorResults);
		cdbdisp_destroyDispatchThreads(dispatchThreads);
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
	cdbdisp_destroyDispatchThreads(dispatchThreads);

	/* Too bad, our gang got an error. */
	PG_TRY();
	{
		ereport(ERROR, (errcode(errorcode),
                        errOmitLocation(true),
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

/*
 * cdbdisp_handleError
 *
 * When caller catches an error, the PG_CATCH handler can use this
 * function instead of cdbdisp_finishCommand to wait for all QEs
 * to finish, clean up, and report QE errors if appropriate.
 * This function should be called only from PG_CATCH handlers.
 *
 * This function destroys and frees the given CdbDispatchResults objects.
 * It is a no-op if both CdbDispatchResults ptrs are NULL.
 *
 * On return, the caller is expected to finish its own cleanup and
 * exit via PG_RE_THROW().
 */
void
cdbdisp_handleError(struct CdbDispatchResults  *primaryResults,
				    struct CdbDispatchResults  *mirrorResults,
					struct CdbDispatchCmdThreads *dispatchThreads)
{
    int     qderrcode;
    bool    useQeError = false;

    qderrcode = elog_geterrcode();

    /* If cdbdisp_dispatchToGang() wasn't called, don't wait. */
    if (!primaryResults &&
        !mirrorResults)
        return;

    /*
     * Request any remaining commands executing on qExecs to stop.
     * We need to wait for the threads to finish.  This allows for proper
     * cleanup of the results from the async command executions.
     */
    CdbCheckDispatchResult(dispatchThreads, true);   /* cancel any QEs still running */

    /*
     * When a QE stops executing a command due to an error, as a
     * consequence there can be a cascade of interconnect errors
     * (usually "sender closed connection prematurely") thrown in
     * downstream processes (QEs and QD).  So if we are handling
     * an interconnect error, and a QE hit a more interesting error,
     * we'll let the QE's error report take precedence.
     */
    if (qderrcode == ERRCODE_MPP_INTERCONNECTION_ERROR)
    {
        if (primaryResults &&
			primaryResults->errcode &&
            primaryResults->errcode != ERRCODE_MPP_INTERCONNECTION_ERROR)
            useQeError = true;
        else if (mirrorResults &&
				 mirrorResults->errcode &&
				 mirrorResults->errcode != ERRCODE_MPP_INTERCONNECTION_ERROR)
            useQeError = true;
    }

    if (useQeError)
    {
        /*
         * Throw the QE's error, catch it, and fall thru to return
         * normally so caller can finish cleaning up.  Afterwards
         * caller must exit via PG_RE_THROW().
         */
        PG_TRY();
        {
            cdbdisp_finishCommand(primaryResults, mirrorResults, dispatchThreads);
        }
        PG_CATCH();
        {}                      /* nop; fall thru */
        PG_END_TRY();
    }
    else
    {
        /*
         * Discard any remaining results from QEs; don't confuse matters by
         * throwing a new error.  Any results of interest presumably should
         * have been examined before raising the error that the caller is
         * currently handling.
         */
        cdbdisp_destroyDispatchResults(primaryResults);
        cdbdisp_destroyDispatchResults(mirrorResults);
		cdbdisp_destroyDispatchThreads(dispatchThreads);

    }
}                               /* cdbdisp_handleError */

void
cdbdisp_dispatchCommandToAllGangs(const char                 *strCommand,
						char				   		*serializedQuerytree,
						int							serializedQuerytreelen,
						char 		   				*serializedPlantree,
						int							serializedPlantreelen,
                        bool                        cancelOnError,
                        bool						startTransaction,
                        struct CdbDispatchResults **pPrimaryResults,    /* OUT */
                        struct CdbDispatchResults **pMirrorResults,     /* OUT */
						struct CdbDispatchCmdThreads **pDispatchThreads)   /* OUT */
{
	Gang		*primaryGang,
				*mirrorGang;
    List		*readerGangs;
    ListCell	*le;

    int			nsegdb = getMPPSegmentCount();
	char		*serializedSnapshot;
	int			serializedSnapshotlen;
	int			txnOptions,
				gangCount;
	
    elog(DEBUG3, "cdbdisp_dispatchCommandToAllGangs: %.50s", strCommand);

    *pPrimaryResults = NULL;
    if (pMirrorResults)
        *pMirrorResults = NULL;
	
	*pDispatchThreads = NULL;

	/*
	 * Allocate a primary and mirror QE for every available segDB in the system.
	 */
	primaryGang = allocateGang(GANGTYPE_PRIMARY_WRITER, nsegdb, -1, NULL);

	Assert(primaryGang);

	mirrorGang = allocateGang(GANGTYPE_MIRROR_WRITER, nsegdb, -1, NULL);

	if (mirrorGang &&
		(mirrorGang->segdb_count < nsegdb || mirrorGang->size < nsegdb))
	{
		elog(LOG, "Some segments are down: MIRROR nsegdb %d mirrrorGang->segdb_count: %d mirrorGang->size: %d",
			 nsegdb, mirrorGang->segdb_count, mirrorGang->size);
		elog(LOG, "                       PRIMARY nsegdb %d mirrrorGang->segdb_count: %d mirrorGang->size: %d",
			 nsegdb, primaryGang->segdb_count, primaryGang->size);
		elog(LOG, "Some segments are down.  Dispatching command %.50s to remaining segments only", strCommand);

		if (strncmp(strCommand, "BEGIN ", 6) != 0 &&
			strncmp(strCommand, "PREPARE ", 8) != 0 &&
			strncmp(strCommand, "COMMIT ", 6) != 0 &&
			strncmp(strCommand, "ROLLBACK ", 9) != 0 &&
			strncmp(strCommand, "select pg_relation_size(", 24) != 0)
		{
			if (strncmp(strCommand, "MPPEXEC ", 7) == 0 || strncmp(strCommand, "mppexec ", 7) == 0)
				elog(NOTICE, "Some segments are down.  Dispatching command to remaining segments only");
			else
				elog(NOTICE, "Some segments are down.  Dispatching command %.50s to remaining segments only", strCommand);
		}
	}

	/* serialized a version of our snapshot */
	serializedSnapshot = serializeSnapshot(&serializedSnapshotlen);
	
	/* generate our transactions options. */
	txnOptions = generateTxnOptions(startTransaction);
	
	readerGangs = getAllReaderGangs();
	
	/*
     * Dispatch the command.
     */
	gangCount = 1 + list_length(readerGangs);
    *pPrimaryResults = cdbdisp_makeDispatchResults(nsegdb * gangCount, 0, cancelOnError);
    cdbdisp_dispatchToGang(pDispatchThreads, strCommand, 
	    serializedQuerytree, serializedQuerytreelen, 
	    serializedPlantree, serializedPlantreelen,
	    NULL, 0, NULL, 0,
	    serializedSnapshot, serializedSnapshotlen,
	    txnOptions, primaryGang, *pPrimaryResults, -1, gangCount);

	if (mirrorGang)
    {
        *pMirrorResults = cdbdisp_makeDispatchResults(nsegdb, 0, cancelOnError);
        cdbdisp_dispatchToGang(pDispatchThreads, strCommand, 
	        serializedQuerytree, serializedQuerytreelen, 
		    serializedPlantree, serializedPlantreelen,
		    NULL, 0, NULL, 0,
		    serializedSnapshot, serializedSnapshotlen,
	        txnOptions, mirrorGang, *pMirrorResults, -1, gangCount);
    }


    foreach(le, readerGangs)
    {
    	Gang  *rg = lfirst(le);
    	cdbdisp_dispatchToGang(pDispatchThreads, strCommand, 
	    	serializedQuerytree, serializedQuerytreelen, 
		    serializedPlantree, serializedPlantreelen,
		    NULL, 0, NULL, 0,
		    serializedSnapshot, serializedSnapshotlen,
	    	txnOptions, rg, *pPrimaryResults, -1, gangCount);
    }



}	/* cdbdisp_dispatchCommandToAllGangs */

/*--------------------------------------------------------------------*/



/*
 * thread_DispatchCommand is the thread proc used to dispatch the command to one or more of the qExecs.
 *
 * NOTE: This function MUST NOT contain elog or ereport statements. (or most any other backend code)
 *		 elog is NOT thread-safe.  Developers should instead use something like:
 *
 *	if (DEBUG3 >= log_min_messages)
 *			write_stderr("my brilliant log statement here.\n");
 *
 * NOTE: In threads, we cannot use palloc, because it's not thread safe.
 */
void *
thread_DispatchCommand(void *arg)
{
	DispatchCommandParms		*pParms = (DispatchCommandParms *) arg;
	SegmentDatabaseDescriptor	*segdbDesc;
	CdbDispatchResult			*dispatchResult;
	int							i, db_count = pParms->db_count;
	int							timeoutCounter = 0;
	
	pParms->query_text = PQbuildMppQueryString(
				pParms->strCommand, pParms->strCommandlen, 
				pParms->serializedQuerytree, pParms->serializedQuerytreelen, 
				pParms->serializedPlantree, pParms->serializedPlantreelen, 
				pParms->serializedParams, pParms->serializedParamslen, 
				pParms->serializedSliceInfo, pParms->serializedSliceInfolen, 
				pParms->serializedSnapshot, pParms->serializedSnapshotlen, 
				pParms->txnOptions, pParms->cmdID, pParms->localSlice, 
				GetSessionUserId(), superuser_arg(GetSessionUserId()),
				GetOuterUserId(), superuser_arg(GetOuterUserId()), GetUserId(),
				&pParms->query_text_len);

	if (pParms->query_text == NULL)
	{
		write_stderr("could not build query string, total length %d\n", pParms->query_text_len);
		pParms->query_text_len = 0;
		return NULL;
	}

	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to send commands through to.
	 */
	for (i = 0; i < db_count; i++)
	{
		

		dispatchResult = pParms->dispatchResultPtrArray[i];
		
		/* Don't use elog, it's not thread-safe */
		if (DEBUG5 >= log_min_messages)
			write_stderr("Thread %u: thread_DispatchCommand working on %d of %d commands.  asyncStatus %d\n",mythread(),
                         i + 1, db_count, dispatchResult->segdbDesc->conn->asyncStatus);

        dispatchResult->hasDispatched = false;
        dispatchResult->requestedCancel = false;
        dispatchResult->wasCanceled = false;

        if (!shouldStillDispatchCommand(dispatchResult))
        {
            /* Don't dispatch if cancellation pending or no connection. */
            dispatchResult->stillRunning = false;
            if (PQisBusy(dispatchResult->segdbDesc->conn))
				write_stderr("Thread %u: We thought we were done, because !shouldStillDispatchCommand(), but libpq says we are still busy\n",mythread());
        }
        else
        {
            /* Kick off the command over the libpq connection.
             * If unsuccessful, proceed anyway, and check for lost connection below.
             */
			if (PQisBusy(dispatchResult->segdbDesc->conn))
			{
				PGresult   *pRes;
				ExecStatusType resultStatus;
				
				write_stderr("Thread %u: Trying to send to busy connection %s  %d %d asyncStatus %d\n",mythread(),dispatchResult->segdbDesc->whoami,i,db_count,dispatchResult->segdbDesc->conn->asyncStatus);
				/*
				 * We weren't expecting the session to be in use!, let's see what it says 
				 */
				pRes = PQgetResult(dispatchResult->segdbDesc->conn);
				if (pRes != NULL) /* null is OK ? */
				{
					resultStatus = PQresultStatus(pRes);
					write_stderr("Thread %u: ResultStatus = %s\n",mythread(),PQresStatus(resultStatus));
					write_stderr("%s",PQerrorMessage(dispatchResult->segdbDesc->conn));
				}
				else
				{
					write_stderr("Thread %u: result was null\n",mythread());
					write_stderr("%s",PQerrorMessage(dispatchResult->segdbDesc->conn));
					Assert(!PQisBusy(dispatchResult->segdbDesc->conn));
				}
			}
            dispatchCommand(dispatchResult, pParms->query_text, pParms->query_text_len, pParms->strCommand);
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
		int			max_sock;
		int			sock;
		int			n;

		/*
		 * Which QEs are still running and could send results to us?
		 */
		max_sock = -1;
		MPP_FD_ZERO(&pParms->mask);

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
				MPP_FD_SET(sock, &pParms->mask);
				max_sock = Max(max_sock, sock);
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
		n = select(max_sock + 1, (fd_set *)&pParms->mask, NULL, NULL, &tv);

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

			if (DEBUG4 >= log_min_messages)
				write_stderr("Thread %u: looking for results from %d of %d\n",mythread(),i+1,db_count);

			/* Skip this connection if it has no input available. */
			sock = PQsocket(segdbDesc->conn);
			if (sock >= 0 &&
				!MPP_FD_ISSET(sock, &pParms->mask))
				continue;

			if (DEBUG4 >= log_min_messages)
				write_stderr("Thread %u: PQsocket says there are results from %d\n",mythread(),i+1);
			/* Receive and process results from this QE. */
			finished = processResults(dispatchResult);

			/* Are we through with this QE now? */
			if (finished)
			{
				if (DEBUG4 >= log_min_messages)
					write_stderr("Thread %u: processResults says we are finished with %d:  %s\n",mythread(),i+1,segdbDesc->whoami);
				dispatchResult->stillRunning = false;
				if (PQisBusy(dispatchResult->segdbDesc->conn))
					write_stderr("We thought we were done, because finished==true, but libpq says we are still busy\n");
			}
			else
				if (DEBUG4 >= log_min_messages)
					write_stderr("Thread %u: processResults says we have more to do with %d: %s\n",mythread(),i+1,segdbDesc->whoami);
		}						/* input available; receive and process it */

	}							/* some QEs running */

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

	/* Don't dispatch to a QE that is not connected. Note, that PQstatus() correctly
	 * handles the case where segdbDesc->conn is NULL, and we *definitely* want to
	 * produce an error for that case. */
	if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
	{
		char	   *msg = PQerrorMessage(segdbDesc->conn);

		/* Save error info for later. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_MPP_INTERCONNECTION_ERROR,
							  "Lost connection to %s.  %s",
							  segdbDesc->whoami,
							  msg ? msg : "");

		if (DEBUG4 >= log_min_messages)
		{
			/* Don't use elog, it's not thread-safe */
			write_stderr("Thread %u: Lost connection: %s\n", mythread(), segdbDesc->whoami);
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
				write_stderr("Thread %u: Error cleanup in progress; command not sent to %s\n", mythread(),
						     segdbDesc->whoami);
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
			write_stderr("Thread %u: Cancellation request pending; command not sent to %s\n",mythread(),
					     segdbDesc->whoami);
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
static void
dispatchCommand(CdbDispatchResult  *dispatchResult,
                const char		   *query_text,
                int				   query_text_len,
                const char         *strCommand)
{
	SegmentDatabaseDescriptor *segdbDesc = dispatchResult->segdbDesc;
	PGconn	   *conn = segdbDesc->conn;

	/* Don't use elog, it's not thread-safe */
	if (DEBUG3 >= log_min_messages)
		write_stderr("Thread %u: %s <- %.120s\n", mythread(), segdbDesc->whoami, strCommand);

	/*
	 * Submit the command asynchronously.
	 */
	
	if (PQsendMppQuery_shared(conn, (char *)query_text, query_text_len) == 0)
	{
		if (DEBUG3 >= log_min_messages)
			write_stderr("Thread %u: PQsendMPPQuery_shared error %s %s \n", mythread(),segdbDesc->whoami,
							  PQerrorMessage(conn));
		/* Note the error. */
		cdbdisp_appendMessage(dispatchResult, LOG,
							  ERRCODE_MPP_INTERCONNECTION_ERROR,
						   "Command could not be sent to segment db %s;  %s",
							  segdbDesc->whoami,
							  PQerrorMessage(conn));
		PQfinish(conn);
		segdbDesc->conn = NULL;
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
		write_stderr("Thread %u: thread_DispatchCommand select() failed; errno=%d\n",mythread(),
				     sock_errno);
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
		if (PQisBusy(dispatchResult->segdbDesc->conn))
				write_stderr("Thread %u: We thought we were done, because of an error, but we are still busy\n",mythread());
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
	 * CDB TODO: PQcancel() is expensive, and we do them
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
				
				char errbuf[256];
				PGcancel * cn = PQgetCancel(segdbDesc->conn);
				
				if (DEBUG4 >= log_min_messages)
					write_stderr("Calling PQcancel for %s\n",
							     segdbDesc->whoami);

				/*
				 * Block here until segment postmaster replies that it has
				 * carried out the cancellation request, which means signaling
				 * SIGINT to the QE process, something like: pid =
				 * PQbackendPID(segdbDesc->conn); kill(pid, SIGINT);
				 */
				dispatchResult->requestedCancel = true;
				
				if (PQcancel(cn, errbuf, 256)==0)
				{	
					if (LOG >= log_min_messages)
						write_stderr("Unable to cancel: %s\n", errbuf);
				}
				PQfreeCancel(cn);
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

			if (DEBUG5 >= log_min_messages)
				write_stderr("Thread %u: checking status %d of %d     %s stillRunning %d \n", mythread(),i+1,db_count,segdbDesc->whoami,dispatchResult->stillRunning);

			/* Skip if already finished or didn't dispatch. */
			if (!dispatchResult->stillRunning)
				continue;

            /* Skip the entry db. */
            if (segdbDesc->segindex < 0)
                continue;

            if (DEBUG5 >= log_min_messages)
				write_stderr("Thread %u: testing connection %d of %d     %s stillRunning %d \n", mythread(),i+1,db_count,segdbDesc->whoami,dispatchResult->stillRunning);
            

			if (!FtsTestConnection(segdbDesc->segment_database_info, segdbDesc->username, segdbDesc->dbname))
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

	if (DEBUG5 >= log_min_messages)
		write_stderr("Thread %u: processResults.  isBusy = %d\n",mythread(),PQisBusy(segdbDesc->conn));

	/* Receive input from QE. */
	rc = PQconsumeInput(segdbDesc->conn);

	/* If PQconsumeInput fails, we're hosed. */
	if (rc == 0)
	{							/* handle PQconsumeInput error */
		write_stderr("Thread %u: PQconsumeInput failed\n",mythread());
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

	if (DEBUG4 >= log_min_messages && PQisBusy(segdbDesc->conn))
		write_stderr("Thread %u: PQisBusy\n",mythread());
			
	/* If we have received one or more complete messages, process them. */
	while (!PQisBusy(segdbDesc->conn))
	{							/* loop to call PQgetResult; won't block */
		PGresult   *pRes;
		ExecStatusType resultStatus;
		int			resultIndex = cdbdisp_numPGresult(dispatchResult);

		if (DEBUG4 >= log_min_messages)
			write_stderr("Thread %u: PQgetResult\n",mythread());
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
				write_stderr("Thread %u: %s -> idle\n", mythread(), segdbDesc->whoami);
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

				write_stderr("%s -> ok %s\n",
						     segdbDesc->whoami,
						     cmdStatus ? cmdStatus : "(no cmdStatus)");
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
				write_stderr("%s -> %s %s  %s",
                             segdbDesc->whoami,
						     PQresStatus(resultStatus),
						     sqlstate ? sqlstate : "(no SQLSTATE)",
						     errorMessage ? errorMessage : "\n");
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
 *
 * startTransaction specifies a desire to include global transaction control
 *  before dispatch.
 */
void
CdbDoCommand(const char *strCommand,
				  bool cancelOnError,
				  bool startTransaction,
				  bool dispatchToMirrors,
				  bool dispatchToPrimaries)
{
	CdbDispatchResults *primaryResults;
	CdbDispatchResults *mirrorResults;
	CdbDispatchCmdThreads *dispatchThreads;

	Assert(dispatchToMirrors || dispatchToPrimaries);
	
	if (startTransaction)
	{
		/* Start a global transaction and reconfigure the cluster if needed. */
		cdbdisp_startDtxTransaction();
		
		/* mark this txn as dirty */
		setCurrentDtxDirty();

	}

	cdbdisp_dispatchCommand(strCommand, NULL, 0, cancelOnError, startTransaction, 
							/* withSnapshot */true, dispatchToMirrors, dispatchToPrimaries,
							&primaryResults, &mirrorResults, &dispatchThreads);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(primaryResults, mirrorResults, dispatchThreads);

}	/* CdbDoCommandNoTxn */


void
CdbDoCommandOnAllGangs(const char *strCommand,
				  bool cancelOnError,
				  bool startTransaction)
{
	CdbDispatchResults *primaryResults;
	CdbDispatchResults *mirrorResults;
	CdbDispatchCmdThreads *dispatchThreads;

	if (startTransaction)
	{
		/* Start a global transaction and reconfigure the cluster if needed. */
		cdbdisp_startDtxTransaction();
		
		/* mark this txn as dirty */
		setCurrentDtxDirty();
	}
	
	cdbdisp_dispatchCommandToAllGangs(strCommand, NULL, 0, NULL, 0, cancelOnError, startTransaction,
							&primaryResults, &mirrorResults, &dispatchThreads);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(primaryResults, mirrorResults, dispatchThreads);

}	

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
	List * savetargets = NIL;
	Node * newNode = 0;
	
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
		 * either NIL or a List node, so we can call expression_tree_mutator
		 * directly rather than recursing to self.
		 */
		args = (List *) expression_tree_mutator((Node *) expr->args,
									pre_dispatch_function_evaluation_mutator,
										  (void *) context);
										
		funcid = expr->funcid;

		/*
		 * Check for constant inputs
		 */
		has_nonconst_input = false;
		
		foreach(arg, args)
		{
			if (!IsA(lfirst(arg), Const))
				has_nonconst_input = true;
		}
		
		if (!has_nonconst_input)
		{
			Oid sequenceoid = 0;

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
				
				
			if (funcid == 1574)  /* nextval */
			{
				Const * arg1 = lfirst(list_head(args));
				sequenceoid = 0;
				if (arg1->consttype == REGCLASSOID)
					sequenceoid = DatumGetObjectId(arg1->constvalue);
			}

			/*
			 * The targetlist happens in multiple places in the plan tree, but we don't
			 * want to execute nextval() every time we see it.	Rather than figure out
			 * which one matters, and somehow passing that in to here, I cache the first time
			 * we run nextval(), and just re-use that value.
			 *
			 * This does have a bug, if we really wanted nextval() to be evaluated more than
			 * once in this single row insert.
			 */


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
	if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;
		List	   *args;

		OpExpr	   *newexpr;

		/*
		 * Reduce constants in the OpExpr's arguments.  We know args is either
		 * NIL or a List node, so we can call expression_tree_mutator directly
		 * rather than recursing to self.
		 */
		args = (List *) expression_tree_mutator((Node *) expr->args,
											  pre_dispatch_function_evaluation_mutator,
												(void *) context);

		/*
		 * Need to get OID of underlying function.	Okay to scribble on input
		 * to this extent.
		 */
		set_opfuncid(expr);
		
		newexpr = makeNode(OpExpr);
		newexpr->opno = expr->opno;
		newexpr->opfuncid = expr->opfuncid;
		newexpr->opresulttype = expr->opresulttype;
		newexpr->opretset = expr->opretset;
		newexpr->args = args;
		return (Node *) newexpr;
	}


	/*
	 * For any node type not handled above, we recurse using
	 * plan_tree_mutator, which will copy the node unchanged but try to
	 * simplify its arguments (if any) using this routine.
	 */
	
	savetargets = NIL;
		
	if (is_plan_node(node) &&
		!is_projection_capable_plan((Plan *)node))
	{
		/*
		 * don't do targetlists in nodes where the targetlist isn't used.  Hide them from the mutator
		 */
		savetargets = ((Plan *)node)->targetlist;
		((Plan *)node)->targetlist = NIL;
	}

	newNode =  plan_tree_mutator(node, pre_dispatch_function_evaluation_mutator,
													 (void *) context);

	if (savetargets != NIL)
	{
		((Plan *)node)->targetlist = savetargets;
		((Plan *)newNode)->targetlist = savetargets;
	}
			
		
	return newNode;
}

/*
 * Compose and dispatch the MPPEXEC commands corresponding to a plan tree
 * within a complete parallel plan.  (A plan tree will correspond either
 * to an initPlan or to the main plan.)
 *
 * If cancelOnError is true, then any dispatching error, a cancellation
 * request from the client, or an error from any QE in the , may cause
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
					 bool planRequiresTxn,
					 bool cancelOnError,
					 struct CdbDispatchResults ** pPrimaryResults,		/* OUT */
					 struct CdbDispatchResults ** pMirrorResults,       /* OUT */
					 struct CdbDispatchCmdThreads ** pDispatchThreads)		/* OUT */
{
	 
	char	   *squery,
			   *splan,
			   *ssliceinfo,
			   *sparams;
	char	   *serializedSnapshot;		
	int			serializedSnapshotlen;
	int			squery_len,
				splan_len,
				ssliceinfo_len,
				sparams_len;
	SliceTable *sliceTbl;
	int			rootIdx;
	int			oldLocalSlice;
	sliceVec	*sliceVector = NULL;
	int			nSlices,
				sliceLim;
	int			iSlice;
	int			txnOptions;
 
	pre_dispatch_function_evaluation_context pcontext;

	*pPrimaryResults = NULL;
	if (pMirrorResults)
		*pMirrorResults = NULL;
	
	*pDispatchThreads = NULL;

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
			   && rootIdx <= sliceTbl->nMotions + sliceTbl->nSubplans));

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

		List * savetargets = NIL;
		Plan * oldplan = NULL;
		/*
		 * The Query tree doesn't have to be mutated here, since it isn't used during
		 * the execution.
		 */
		
		savetargets = NIL;
		
		if (is_plan_node((Node *)queryDesc->plantree) &&
			!is_projection_capable_plan((Plan *)queryDesc->plantree))
		{
			/*
			 * don't do targetlists in nodes where the targetlist isn't used.  Hide them from the mutator
			 */
			savetargets = queryDesc->plantree->targetlist;
			queryDesc->plantree->targetlist = NIL;
			oldplan = queryDesc->plantree;
		}


		queryDesc->plantree = (Plan *) plan_tree_mutator((Node *) queryDesc->plantree,
									pre_dispatch_function_evaluation_mutator,
														 &pcontext);
														
		if (savetargets != NIL)
		{
			queryDesc->plantree->targetlist = savetargets;
			oldplan->targetlist = savetargets;
		}

	}


	/*
	 * serialized plan and query trees. Note that we're called for a single
	 * slice tree (corresponding to an initPlan or the main plan), so the
	 * parameters are fixed and we can include them in the prefix.
	 */
	squery = serializeNode((Node *) queryDesc->parsetree, &squery_len);
	splan = serializeNode((Node *) queryDesc->plantree, &splan_len);

	Assert(squery != NULL && splan != NULL);

	/*
	*/
	if (queryDesc->params != NULL && queryDesc->params->numParams > 0)
	{		
		int i;
		char * p;
		Size length;

		sparams_len = sizeof(ParamListInfoData) + (queryDesc->params->numParams-1) * sizeof(ParamExternData);
		
		elog(DEBUG1,"Serializing %d params",queryDesc->params->numParams);

		/* We run through the parameters twice: first time adding up the space we'll need */
		/* figure out how much buffer we need */
		for (i = 0; i < queryDesc->params->numParams; i++)
		{
			ParamExternData * pxd = &queryDesc->params->params[i];
			bool isnull = pxd->isnull;
			int16 typlen;
			bool typbyval;

			get_typlenbyval(pxd->ptype, &typlen, &typbyval);
			if (!typbyval && typlen < 0 && pxd->value == 0 )
				isnull = true;

			if (!isnull)
			{
				length = datumGetSize(pxd->value, typbyval, typlen);
				if (!typbyval)
					sparams_len += sizeof(length);
				sparams_len += length;
			}
		}
		sparams = palloc(sparams_len);
		p = sparams;
		memcpy(p, &queryDesc->params->numParams, sizeof(int)); p+=sizeof(int);

		for (i = 0; i < queryDesc->params->numParams; i++)
		{
			ParamExternData * pxd = &queryDesc->params->params[i];
			bool isnull = pxd->isnull;
			int16 typlen;
			bool typbyval;

			get_typlenbyval(pxd->ptype, &typlen, &typbyval);
			if (!typbyval && typlen < 0 && pxd->value == 0)
				isnull = true;
			
			memcpy(p, &pxd->ptype, sizeof(Oid)); p += sizeof(Oid);
			memcpy(p, &isnull, sizeof(bool)); p += sizeof(bool);
			memcpy(p, &pxd->pflags, sizeof(uint16)); p += sizeof(uint16);

			if (!isnull)
			{
				/* this is much like _outDatum */
				length = datumGetSize(pxd->value, typbyval, typlen);
				if (typbyval)
				{
					memcpy(p, &pxd->value, length); p += length;
				}
				else /* passed by ref, possibly VARLEN */
				{
					char *s = (char *)DatumGetPointer(pxd->value);

					if (PointerIsValid(s))
					{
						memcpy(p, &length, sizeof(length)); p += sizeof(length);
						memcpy(p, s, length); p += length;
					}
					else
					{
						memset(p, 0, sizeof(length)); p += sizeof(length);
					}
				}
			}
		}
	}
	else
	{
		sparams = NULL;
		sparams_len = 0;
	}

	ssliceinfo = serializeNode((Node *) sliceTbl, &ssliceinfo_len);
	
 
	/*
	 * Traverse the slice tree in sliceTbl rooted at rootIdx and build a
	 * vector of slice indexes specifying the order of [potential] dispatch.
	 */
	sliceLim = list_length(sliceTbl->slices);
	sliceVector = palloc0(sliceLim * sizeof(sliceVec));

	nSlices = fillSliceVector(sliceTbl, rootIdx, sliceVector, sliceLim);

	/* Allocate result array with enough slots for QEs of primary gangs. */
	*pPrimaryResults = cdbdisp_makeDispatchResults(nSlices * getMPPSegmentCount(),
												   sliceLim,
												   cancelOnError);

	/* Allocate result array for mirror gang. */
	if (queryDesc->operation != CMD_SELECT || queryDesc->parsetree->into != NULL)
	{
		Slice	   *slice = (Slice *) list_nth(sliceTbl->slices, rootIdx);

		if (slice->mirrorGang)
		{
			*pMirrorResults = cdbdisp_makeDispatchResults(getMPPSegmentCount(),
														  sliceLim,
														  cancelOnError);
		}
	}


	/* serialized a version of our snapshot */
	serializedSnapshot = serializeSnapshot(&serializedSnapshotlen);
	
	/* 
	 * Generate our transction isolations.  We generally want Plan
	 * based dispatch to be in a global transaction. The executor gets
	 * to decide if the special circumstances exist which allow us to
	 * dispatch without starting a global xact.
	 */
	txnOptions = generateTxnOptions(planRequiresTxn);

	cdb_total_plans++;
	cdb_total_slices += nSlices;
	if (nSlices > cdb_max_slices)
		cdb_max_slices = nSlices;
	
	/*
	 * Now we need to call CDBDispatchCommand once per slice.  Each such
	 * call dispatches a MPPEXEC command to each of the QEs assigned to
	 * the slice.
	 *
	 * The QE information goes in two places: (1) in the argument to the
	 * function CDBDispatchCommand, and (2) in the serialized
	 * command sent to the QEs.
	 *
	 * So, for each slice in the tree...
	 */
	for (iSlice = 0; iSlice < nSlices; iSlice++)
	{
		Slice	   *slice = sliceVector[iSlice].slice;
		Gang	   *primaryGang = slice->primaryGang;
		Gang	   *mirrorGang = slice->mirrorGang;

		/*
		 * Is this a slice we should dispatch?
		 */
		if (slice->gangType == GANGTYPE_UNALLOCATED)
		{
			/*
			 * Most slices are dispatched, however, in many  cases the
			 * root runs only on the QD and is not dispatched to the QEs.
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
		 * Dispatch the plan to our primaryGang.
		 * Doesn't wait for it to finish.
		 */
		cdbdisp_dispatchToGang(pDispatchThreads,
							   debug_query_string,
							   squery, squery_len,
							   splan, splan_len,
							   sparams, sparams_len,
							   ssliceinfo, ssliceinfo_len, 
						       serializedSnapshot, serializedSnapshotlen,
						       txnOptions,
                               primaryGang,
                               *pPrimaryResults,
                               slice->sliceIndex, sliceLim);


		/*
		 * dispatch the plan to our mirrorGang if we have one.
		 */
		if (mirrorGang != NULL && (queryDesc->operation != CMD_SELECT || queryDesc->parsetree->into != NULL))
		{
            cdbdisp_dispatchToGang(pDispatchThreads,
								   debug_query_string,
								   squery, squery_len,
								   splan, splan_len,
								   sparams, sparams_len,
								   ssliceinfo, ssliceinfo_len, 
							       serializedSnapshot, serializedSnapshotlen,
            					   txnOptions,
                                   mirrorGang,
                                   *pMirrorResults,
                                   slice->sliceIndex, sliceLim);
		}
	}


    pfree(sliceVector);

	sliceTbl->localSlice = oldLocalSlice;

	/*
	 * If bailed before completely dispatched, stop QEs and throw error.
	 */
	if (iSlice < nSlices)
	{
		CdbDispatchResults *pr = *pPrimaryResults;
		CdbDispatchResults *mr = pMirrorResults ? *pMirrorResults : NULL;
		CdbDispatchCmdThreads *dThreads = *pDispatchThreads;

		elog(DEBUG2, "Plan dispatch canceled; dispatched %d of %d slices",
			 iSlice, nSlices);

		/* Cancel any QEs still running, and wait for them to terminate. */
		CdbCheckDispatchResult(dThreads, true);

		/*
		 * Check and free the results of all gangs. If any QE had an
		 * error, report it and exit via PG_THROW.
		 */
		*pPrimaryResults = NULL;
		if (mr)
			*pMirrorResults = NULL;
		*pDispatchThreads = NULL;
		cdbdisp_finishCommand(pr, mr, dThreads);

		/* Wasn't an error, must have been an interrupt. */
		CHECK_FOR_INTERRUPTS();

		/* Strange!  Not an interrupt either. */
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg_internal("Unable to dispatch plan.")));
	}
}	/* cdbdisp_dispatchPlan */


/*
 * Three Helper functions for CdbDispatchPlan:
 *
 * Used to figure out the dispatch order for the sliceTable by
 * counting the number of dependent child slices for each slice; and
 * then sorting based on the count (all indepenedent slices get
 * dispatched first, then the slice above them and so on).
 *
 * fillSliceVector: figure out the number of slices we're dispatching,
 * and order them.
 *
 * count_dependent_children(): walk tree counting up children.
 *
 * compare_slice_order(): comparison function for qsort(): order the
 * slices by the number of dependent children. Empty slices are 
 * sorted last (to make this work with initPlans).
 * 
 */
static int
compare_slice_order(const void *aa, const void *bb)
{
	sliceVec *a = (sliceVec *)aa;
	sliceVec *b = (sliceVec *)bb;

	if (a->slice == NULL)
		return 1;
	if (b->slice == NULL)
		return -1;

	if (a->children == b->children)
		return 0;
	else if (a->children > b->children)
		return 1;
	else
		return -1;
}

static int
count_dependent_children(SliceTable * sliceTable, int sliceIndex, sliceVec *sliceVector, int len)
{
	int			res;
	int			childIndex;
	ListCell   *sublist;
	Slice	   *slice = (Slice *) list_nth(sliceTable->slices, sliceIndex);

	res = list_length(slice->children);

	foreach(sublist, slice->children)
	{
		childIndex = lfirst_int(sublist);
		res += count_dependent_children(sliceTable, childIndex, sliceVector, len);
	}

	sliceVector[sliceIndex].sliceIndex = sliceIndex;
	sliceVector[sliceIndex].children = res;
	sliceVector[sliceIndex].slice = slice;

	return res;
}

static int
fillSliceVector(SliceTable *sliceTbl, int rootIdx, sliceVec *sliceVector, int sliceLim)
{
	int top_count;

	/* count doesn't include top slice add 1 */
	top_count = 1 + count_dependent_children(sliceTbl, rootIdx, sliceVector, sliceLim);

	qsort(sliceVector, sliceLim, sizeof(sliceVec), compare_slice_order);

	return top_count;
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
								 bool startTransaction,
								 struct CdbDispatchResults ** pPrimaryResults,
								 struct CdbDispatchResults ** pMirrorResults,
								 struct CdbDispatchCmdThreads ** pDispatchThreads)
{
	char	   *serializedQuerytree;
	int			serializedQuerytree_len;
	Query	   *q = makeNode(Query);
	StringInfoData buffer;
	bool 		dispatchToMirrors = true;

	/* mark the dtx as dirty */
	setCurrentDtxDirty();
	
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
	 * serialized the stmt tree, and create the sql statement: mppexec ....
	 */
	serializedQuerytree = serializeNode((Node *) q, &serializedQuerytree_len);

	Assert(serializedQuerytree != NULL);

	cdbdisp_dispatchCommand(debug_query_string, serializedQuerytree, serializedQuerytree_len, cancelOnError, startTransaction,
							/* withSnapshot */true, dispatchToMirrors, /* primaries */true, 
							pPrimaryResults, pMirrorResults, pDispatchThreads);



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
	CdbDispatchCmdThreads *dispatchThreads;
	bool	startTransaction = true;
	
	/* Start a global transaction and reconfigure the cluster if needed. */
	cdbdisp_startDtxTransaction();

	cdbdisp_dispatchUtilityStatement(stmt, true, startTransaction, &primaryResults, &mirrorResults, &dispatchThreads);


	/* Wait for all QEs to finish.	Throw up if error. */
	cdbdisp_finishCommand(primaryResults, mirrorResults, dispatchThreads);


}	/* CdbDispatchUtilityStatement */



char*
serializeSnapshot(int * size)
{
	char *serializedSnapshot;

	Snapshot snapshot = NULL;
	
	
	/* If we already have a LatestSnapshot set then no reason to try
	 * and get a new one.  just use that one.  But... there is one important
	 * reason why this HAS to be here.  ROLLBACK stmts get dispatched to QEs
	 * in the abort transaction code.  This code tears down enough stuff such
	 * that you can't call GetTransactionSnapshot() within that code. So we
	 * need to use the LatestSnapshot since we can't re-gen a new one.
	 *
	 * It is also very possible that for a single user statement which may
	 * only generate a single snapshot that we will dispatch multiple statements
	 * to our qExecs.  Something like:
	 *
	 *							QD				QEs
	 *							|  				|
	 * User SQL Statement ----->|	  BEGIN		|
	 *  						|-------------->|
	 *						    | 	  STMT		|
	 *							|-------------->|
	 *						 	|    PREPARE	|
	 *							|-------------->|
	 *							|    COMMIT		|
	 *							|-------------->|
	 *							|				|
	 *
	 * This may seem like a problem because all four of those will dispatch
	 * the same snapshot with the same curcid.  But... this is OK because
	 * BEGIN, PREPARE, and COMMIT don't need Snapshots on the QEs.
	 *
	 * NOTE: This will be a problem if we ever need to dispatch more than one
	 *  	 statement to the qExecs and more than one needs a snapshot!
	 */
	if (LatestSnapshot != NULL)
	{
		snapshot = LatestSnapshot;
	}
	else if (!IsAbortInProgress() )  		
	{
		/* unfortunately, the dtm issues a select for prepared xacts at the
		 * beginning and this is before a snapshot has been set up.  so we need
		 * one for that but not for when we dont have a valid XID.
		 *
		 * but we CANT do this if an ABORT is in progress... instead we'll send
		 * a NONE since the qExecs dont need the information to do a ROLLBACK.
		 *
		 */
		snapshot = GetTransactionSnapshot();
	}
	
	if (snapshot != NULL)
	{
		int i;
		char * p;
		TransactionId currentTransactionId = GetCurrentTransactionId();

		*size = sizeof(TransactionId)*3 + 
										sizeof(uint32) + /*xcnt*/
										snapshot->xcnt*sizeof(TransactionId) + sizeof(CommandId);
		serializedSnapshot = palloc(*size);
		p = serializedSnapshot;
		memcpy(p,&currentTransactionId,sizeof(TransactionId)); p += sizeof(TransactionId);
		memcpy(p,&snapshot->xmin,sizeof(TransactionId)); p += sizeof(TransactionId);
		memcpy(p,&snapshot->xmax,sizeof(TransactionId)); p += sizeof(TransactionId);
		memcpy(p,&snapshot->xcnt,sizeof(uint32)); p += sizeof(uint32);
		for (i = 0; i < snapshot->xcnt; i++)
		{
			memcpy(p,&snapshot->xip[i],sizeof(TransactionId)); p += sizeof(TransactionId);
		}
		memcpy(p,&snapshot->curcid,sizeof(CommandId)); p += sizeof(CommandId);
		
		elog(DEBUG5,"serializeSnapshot:  txnId %d, curcid %d",currentTransactionId,snapshot->curcid);
		
	}
	else
	{
		/* basically this should mean that we have an Abort in progress and
		 * so can't and don't need to send a serialized snapshot.  so we'll
		 * send nothing.
		 */
		serializedSnapshot = NULL;
		*size=0;
	}

	return serializedSnapshot;
}

/* generateTxnOptions:
 * Generates an int containing the appropriate flags to direct the remote
 * segdb QE process to perform any needed transaction commands before or
 * after the statement.
 *
 * shouldStart - specifies whether this statement even wants a transaction to
 *				 be started.  Certain utility statements dont want to be in a
 *				 distributed transaction.
 */
static int
generateTxnOptions( bool shouldStart )
{
	int options = 0;
	
	/*set auto-begin */
	if (shouldStart)
	{
		options = mppTxnOptions(true /* AutoBegin */);
		
		/* at this point we've decided that are going to begin a transaction.
		 * so we need to let the DTM know this so that if anything bad happens
		 * it can react.
		 */
		setTxnBegan();
		
	}
	
	return options;
	
}

/*
 * cdbdisp_makeDispatchThreads:
 * Allocates memory for a CdbDispatchCmdThreads struct that holds
 * the thread count and array of dispatch command parameters (which
 * is being allocated here as well).
 */
CdbDispatchCmdThreads *
cdbdisp_makeDispatchThreads(int paramCount)
{
	CdbDispatchCmdThreads *dThreads = palloc0(sizeof(*dThreads));

	dThreads->dispatchCommandParmsAr =
		(DispatchCommandParms *) palloc0(paramCount * sizeof(DispatchCommandParms));
	
	dThreads->dispatchCommandParmsArSize = paramCount;

    dThreads->threadCount = 0;

    return dThreads;
}                               /* cdbdisp_makeDispatchThreads */

/*
 * cdbdisp_destroyDispatchThreads:
 * Frees all memory allocated in CdbDispatchCmdThreads struct.
 */
void
cdbdisp_destroyDispatchThreads(CdbDispatchCmdThreads *dThreads)
{

	DispatchCommandParms *pParms;
	int i;

	if (!dThreads)
        return;

	/*
	 * pfree the memory allocated for the dispatchCommandParmsAr
	 */
	elog(DEBUG3, "destroydispatchthreads: threadcount %d array size %d", dThreads->threadCount, dThreads->dispatchCommandParmsArSize);
	for (i = 0; i < dThreads->dispatchCommandParmsArSize; i++)
	{
		pParms = &(dThreads->dispatchCommandParmsAr[i]);
		if (pParms->dispatchResultPtrArray)
		{
			pfree(pParms->dispatchResultPtrArray);
			pParms->dispatchResultPtrArray = NULL;
		}
		if (pParms->query_text)
		{
			/* NOTE: query_text gets malloc()ed by the pqlib code, use
			 * free() not pfree() */
			free(pParms->query_text);
			pParms->query_text = NULL;
		}
		if (pParms->strCommand)
		{
			/* Caller frees if desired */
			pParms->strCommand = NULL;
		}
		
		if (pParms->serializedSnapshot)
		{
			if (i==0)
				pfree(pParms->serializedSnapshot);
			pParms->serializedSnapshot = NULL;
		}
		
		if (pParms->serializedSliceInfo)
		{
			if (i==0)
				pfree(pParms->serializedSliceInfo);
			pParms->serializedSliceInfo = NULL;
		}
		
		if (pParms->serializedQuerytree)
		{
			if (i==0)
				pfree(pParms->serializedQuerytree);
			pParms->serializedQuerytree = NULL;
		}
		
		if (pParms->serializedPlantree)
		{
			if (i==0)
				pfree(pParms->serializedPlantree);
			pParms->serializedPlantree = NULL;
		}
		
		if (pParms->serializedParams)
		{
			if (i==0)
				pfree(pParms->serializedParams);
			pParms->serializedParams = NULL;
		}
		
		pParms->txnOptions = 0;
	}
	
	pfree(dThreads->dispatchCommandParmsAr);	
	dThreads->dispatchCommandParmsAr = NULL;

    dThreads->dispatchCommandParmsArSize = 0;
    dThreads->threadCount = 0;
		
	pfree(dThreads);
	
}                               /* cdbdisp_destroyDispatchThreads */


