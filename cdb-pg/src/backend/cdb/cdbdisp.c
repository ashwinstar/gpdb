
/*-------------------------------------------------------------------------
 *
 * cdbdisp.c
 *	  Functions to dispatch commands to QExecutors
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include <pthread.h>
#include "pqexpbuffer.h"
#include "lib/stringinfo.h"
#include "storage/ipc.h"		/* For proc_exit_inprogress  */
#include "miscadmin.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbtm.h"

#include "cdb/cdbgang.h"
#include "cdb/cdblink.h"		/* just for our CdbProcess population hack. */

#include "cdb/cdbsrlz.h"
#include "cdb/cdbsubplan.h"
#include "cdb/cdbfts.h"

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
	 * segdbDescPtrArray: Array of SegmentDatabaseDescriptor* 's that this thread is
	 * responsible for dispatching the command to. Has size equal to db_count.
	 */
	SegmentDatabaseDescriptor **segdbDescPtrArray;

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
	 * Is this a primary gang?	Only count activity for primary, not mirror gangs
	 */

	bool		isPrimary;


	/*
	 * Returned resultset
	 */
	PGresult  **resultSets;

}	DispatchCommandParms;

/*
 * Structure used in thread proc for keeping track of
 * commands that are still running.  For each command
 * that has not yet finished, we keep the segdbDesc pointer and
 * a flag indicating whether we've  requested cancel.
 */
typedef struct
{
	SegmentDatabaseDescriptor *segdbDesc;
	bool		requestedCancel;
}	RunningInfo;

/*
 * Structure used when targetSet is DTS_LISTENER.
 * In that case, we need to sort the segdbDesc's by listenerid
 * and then choose one per listener only.
 */
typedef struct
{
	int			listenerid;
	SegmentDatabaseDescriptor *segdbDesc;
}	ListenerSegDBPair;

/*
 * Static Helper functions
 */
static void *thread_DispatchCommand(void *arg);


static bool shouldStillDispatchCommand( SegmentDatabaseDescriptor *segdbDesc, 
							 			bool cancelFromBelow);
							 			
static bool dispatchCommand( SegmentDatabaseDescriptor *segdbDesc,
							 const char *szCommand,
							 bool cancelFromBelow );
							 
static void handleSelectError( RunningInfo *stillRunning,
							   int stillRunningCount,	
	  					       SegmentDatabaseDescriptor *segdbDesc, 
					    	   bool cancelFromBelow );							 

static void checkForBadConnections( RunningInfo *stillRunning ,int *stillRunningCount, bool cancelFromBelow);

static void handleSelectTimeout( RunningInfo *stillRunning, int *stillRunningCount );
static void processResults( RunningInfo *segInfo, bool cancelFromBelow, PGresult  **pqresults, int db_count, int *resultCount );


static void
addSegDBToDispatchThreadPool(DispatchCommandParms * ParmsAr,
							 int *segdbCount,
							 const char *szCommand,
							 bool cancelFromBelow,
							 bool isPrimary,
							 SegmentDatabaseDescriptor *
							 segdbDesc);

static CdbDispatchResults *initCdbDispatchResults(void);

static void CdbDumpDispatchResult(CdbDispatchResult * result, bool verbose,
					  StringInfo message);

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

/*
 * CdbDispatchToGang:
 * Send ths szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 */
void
CdbDispatchToGang(const char *szCommand, Gang * gp,
				  bool cancelFromBelow, void ***resultSet)
{
	int			i;
	int			max_threads;
	int			segdbs_in_thread_pool = 0;
	int			x;

	int			newThreads = 0;
	PGresult ***pgresultsets = (PGresult ***) resultSet;



	if (Mpp_role != MPP_ROLE_DISPATCH)
		elog(ERROR, "CdbDispatchToGang must be called in DISPATCH mode.");


	Assert(Mpp_role == MPP_ROLE_DISPATCH);
	Assert(gp != NULL);
	Assert(gp->size > 0);

	elog(DEBUG1, "CdbDispatchToGang gangize %d -> %.50s", gp->size, szCommand);



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

		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * Create the thread parms structures based targetSet parameter.
	 * This will add the segdbDesc pointers appropriate to the
	 * targetSet into the thread Parms structures, making sure that each thread
	 * handles mpp_connections_per_thread segdbs.
	 */



	elog(DEBUG3, "createThreadPoolEnumeration");

	for (x = 0; x < gp->size; x++)
	{

		SegmentDatabaseDescriptor *segdbDesc = &gp->db_descriptors[x];

		Assert(segdbDesc != NULL);

		if (segdbDesc->conn == NULL)
		{
			CdbComponentDatabaseInfo *q = segdbDesc->segment_database_info;

			Assert(q != NULL);

			/*
			 * This is an error condition
			 */
			ereport(ERROR,
					(errcode
				 (ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION),
					 errmsg
					 ("Cannot dispatch command %s to segindex %d isprimary %c, because "
					  "there is no connection.", szCommand, q->segindex,
					  (q->isprimary ? 't' : 'f'))));

		}
		
		/* TODO: is it really possible that all of these could be not null?
		 *       mostly there were moved here to get them out of all the rest of
		 *		 the code and to concentrate them here in one place. 
		 */
		 
		if( segdbDesc->error_message != NULL )
		{
			pfree(segdbDesc->error_message);
			segdbDesc->error_message = NULL;
		}
		
		if( segdbDesc->cmdStatus != NULL )
		{
			pfree(segdbDesc->cmdStatus);
			segdbDesc->cmdStatus = NULL;
		}

		addSegDBToDispatchThreadPool(dispatchCommandParmsAr + threadCount,
									 &segdbs_in_thread_pool, szCommand,
			cancelFromBelow, gp->type == GANGTYPE_PRIMARY_WRITER, segdbDesc);
	}

	elog(DEBUG1, "segdbs_in_thread_pool %d", segdbs_in_thread_pool);

	/*
	 * Compute the thread count based on how many segdbs were added into the
	 * thread pool, knowing that each thread handles mpp_connections_per_thread
	 * segdbs.
	 */

	if (segdbs_in_thread_pool > 0)
		newThreads += 1 + (segdbs_in_thread_pool - 1) / mpp_connections_per_thread;

	/*
	 * set result set variables if return of result set is required
	 */
	if (pgresultsets != NULL)
	{
		/*
		 * Don't use elog, it's not thread-safe
		 */
		if (DEBUG3 >= log_min_messages)
			fprintf(stderr, "doing calloc for pgresults\n");

		/*
		 * allocate one more slot to indicate the end of array, so caller
		 * don't need to get the length of the array.
		 */
		*pgresultsets = (PGresult **) calloc(gp->size + 1, sizeof(PGresult *));

		for (i = 0; i < newThreads; i++)
		{
			DispatchCommandParms *pParms = &(dispatchCommandParmsAr + threadCount)[i];

			pParms->resultSets = *pgresultsets + (i * mpp_connections_per_thread);
		}
	}

	elog(DEBUG4,
		 "CdbDispatchToGang threadCount %d, segdbs_in_thread_pool %d, conns_per_thread %d",
		 threadCount, segdbs_in_thread_pool, mpp_connections_per_thread);


	/*
	 * Create the threads. (which also starts the dispatching).
	 */
	for (i = 0; i < newThreads; i++)
	{
		DispatchCommandParms *pParms = &(dispatchCommandParmsAr + threadCount)[i];

		Assert(pParms != NULL);


		if (0 != pthread_create(&pParms->thread, NULL, thread_DispatchCommand, pParms))
		{
			elog(NOTICE, "pthread_create failed");
		}

		elog(DEBUG2, "CdbDispatchToGang Creating thread %d of %d for CommandDispatch",
			 i + 1, newThreads);

	}

	threadCount += newThreads;



}

/*
 * CdbDispatchCommandInt:
 * Send ths szCommand SQL statement to all segdbs in the cluster
 * cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 */
void
CdbDispatchCommandInt(const char *szCommand, bool cancelFromBelow,
					  void ***resultSets)
{
	int			segdbs_in_thread_pool;

	Gang	   *primaryGang = NULL;
	Gang	   *mirrorGang = NULL;

	if (Mpp_role != MPP_ROLE_DISPATCH)
		elog(ERROR, "CdbDispatchCommandInt should not be called while not in DISPATCH mode.");

	elog(DEBUG1, "CdbDispatchCommandInt %.50s", szCommand);

	Assert(Mpp_role == MPP_ROLE_DISPATCH);


	/*
	 * There should be no outstanding threads when we are here
	 *
	 */
	Assert(threadCount == 0);

	/*
	 * The most threads we could have is segdb_count / mpp_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / mpp_connections_per_thread.
	 * We allocate enough memory for this many DispoatchCommandParms structures,
	 * even though we may not use them all.
	 */
	Assert(mpp_connections_per_thread > 0);

	segdbs_in_thread_pool = 0;

	/*
	 * We want to send to every available segDB in the system.
	 *
	 */
	primaryGang = allocateGang(GANGTYPE_PRIMARY_WRITER, getMPPSegmentCount(), -1);
	mirrorGang = allocateGang(GANGTYPE_MIRROR_WRITER, getMPPSegmentCount(), -1);
	Assert(primaryGang != NULL);

	CdbDispatchToGang(szCommand, primaryGang, cancelFromBelow, resultSets);

	if (mirrorGang)
		CdbDispatchToGang(szCommand, mirrorGang, cancelFromBelow, resultSets);

}




/*
 * CdbDispatchCommand:
 * Send ths szCommand SQL statement to all segdbs in the cluster
 * cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 * Called by general users, this method includes global transaction control.
 */
void
CdbDispatchCommand(const char *szCommand, bool cancelFromBelow)
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

	CdbDispatchCommandInt(szCommand, cancelFromBelow, NULL);
}

/*
 * CdbCheckDispatchResult:
 * Waits for threads that were launched in CdbDispatchCommand
 * to finish, and then checks to see whether they all succeeded.
 * If so, returns true.  If not, returns false, and fills in pszError
 * out parameter with a concatenation of errors from any failed command.
 */
CdbDispatchResults *
CdbCheckDispatchResult(void)
{
	return CdbCheckDispatchResultInt(NULL, NULL);
}

CdbDispatchResults *
CdbCheckDispatchResultInt(void ***p_failedSegDB, int *numOfFailed)
{
	int			i;
	int			j;
	int			x = 0;
	SegmentDatabaseDescriptor *segdbDesc;
	DispatchCommandParms *pParms;
	CdbDispatchResults *results = initCdbDispatchResults();
	SegmentDatabaseDescriptor ***failedSegDB =
	(SegmentDatabaseDescriptor ***) p_failedSegDB;

	Assert(results != NULL);

	elog(DEBUG4, "CheckDispatchResult threadCount %d", threadCount);

	if (threadCount == 0)
		return results;

	/*
	 * Wait for threads to finish.
	 */
	for (i = 0; i < threadCount; i++)
	{
		pParms = &dispatchCommandParmsAr[i];

		Assert(pParms != NULL);

		elog(DEBUG2, "Joining to thread %d of %d for CommandDispatch",
			 i + 1, threadCount);

		if (0 != pthread_join(pParms->thread, NULL))
		{
			elog(NOTICE, "pthread_join failed");
		}
	}

	/*
	 * Check whether there were error messages.
	 * This is stored in the error_message field of the
	 * segment_database_descriptor.
	 */

	elog(DEBUG4, "CheckDispatchResult joined, checking");
	x = 0;

	for (i = 0; i < threadCount; i++)
	{
		pParms = &dispatchCommandParmsAr[i];

		Assert(pParms != NULL);

		for (j = 0; j < pParms->db_count; j++)
		{
			CdbDispatchResult *result;

			segdbDesc = pParms->segdbDescPtrArray[j];
			Assert(segdbDesc != NULL);

			result = &results->pResults[x++];

			result->isPrimary = pParms->isPrimary;

			if (segdbDesc->error_message != NULL)
			{
				char	   *pszErrMsg;

				results->success = false;

				if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
				{
					if (failedSegDB != NULL)
					{
						/*
						 * What memory context is this palloc coming from?
						 */
						if (*failedSegDB == NULL)
							*failedSegDB = palloc(sizeof(SegmentDatabaseDescriptor *) *
												  pParms->db_count);
						(*failedSegDB)[(*numOfFailed)++] = segdbDesc;
					}
				}

				/*
				 * Because we cannot use palloc inside a thread, the error messages
				 * * were malloc'd. So we must free the malloc'd memory after
				 * * appending them to the StringInfoData.
				 * * We must zero out the segdbDesc->error_message too.
				 */
				pszErrMsg = segdbDesc->error_message;
				segdbDesc->error_message = NULL;

				/*
				 * What memory context is this palloc coming from?
				 */
				result->error_message = pstrdup(pszErrMsg);
				free(pszErrMsg);
			}

			if (segdbDesc->cmdStatus != NULL)
			{
				char	   *cmdStatus = segdbDesc->cmdStatus;

				segdbDesc->cmdStatus = NULL;

				result->cmdStatus = pstrdup(cmdStatus);
				free(cmdStatus);
			}

			result->cmdTuples = segdbDesc->cmdTuples;
			segdbDesc->cmdTuples = 0;

			result->resultStatus = segdbDesc->resultStatus;
			segdbDesc->resultStatus = PGRES_COMMAND_OK;
		}
	}

	elog(DEBUG4, "CheckDispatchResult freeing");

	/*
	 * pfree the memory allocated for the dispatchCommandParmsAr
	 */
	if (dispatchCommandParmsAr != NULL)
	{
		for (i = 0; i < threadCount; i++)
		{
			pParms = &dispatchCommandParmsAr[i];
			Assert(pParms != NULL && pParms->segdbDescPtrArray != NULL);
			pfree(pParms->segdbDescPtrArray);
			pParms->segdbDescPtrArray = NULL;
			Assert(pParms->szCommand != NULL);
			pfree(pParms->szCommand);
			pParms->szCommand = NULL;
		}
		pfree(dispatchCommandParmsAr);
		dispatchCommandParmsAr = NULL;
	}


	cancelledByUs = false;

	elog(DEBUG4, "CheckDispatchResult done");

	threadCount = 0;

	/*
	 * handle network failures
	 */
	if (numOfFailed != NULL && *numOfFailed > 0)
		FtsHandleNetFailure(*failedSegDB, *numOfFailed);

	return results;
}


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
	DispatchCommandParms *pParms = (DispatchCommandParms *) arg;
	const char 			 *szCommand = pParms->szCommand;
	SegmentDatabaseDescriptor **segdbDescPtrArray = pParms->segdbDescPtrArray;
	int			db_count = pParms->db_count;
	bool		cancelFromBelow = pParms->cancelFromBelow;
	int			i;
	SegmentDatabaseDescriptor *segdbDesc;
	PGconn	   	*conn = NULL;
	RunningInfo *stillRunning = NULL;
	PGresult  	**pqresults = NULL;
	
	int			stillRunningCount = 0;
	int 		resultCount = 0;	
	

	fd_set input_mask, input_mask_copy;
	int max_sock = 0;

	/*
	 * In threads, we cannot use palloc, because it's not thread safe.
	 */
	stillRunning = (RunningInfo *) calloc(db_count, sizeof(RunningInfo));

	if (pParms->resultSets != NULL)
		pqresults = pParms->resultSets;

	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to send commands through to.
	 */
	for (i = 0; i < db_count; i++)
	{
		/* Don't use elog, it's not thread-safe */
		if (DEBUG3 >= log_min_messages)
			fprintf(stderr, "thread_DispatchCommand working on %d of %d commands\n", i, db_count);

		segdbDesc = segdbDescPtrArray[i];


		if( shouldStillDispatchCommand( segdbDesc, cancelFromBelow) )
		{
			/* actually kick off the command over the libpq connection. */
			if( dispatchCommand( segdbDesc, szCommand, cancelFromBelow ) )
			{
				/*
				 * OK, command was successfully submitted. Add to the stillRunning array
				 * so we can monitor it.
				 */
				stillRunning[stillRunningCount].segdbDesc = segdbDesc;
				stillRunning[stillRunningCount].requestedCancel = false;
				stillRunningCount++;
			}
		}
	}


	
	FD_ZERO(&input_mask);
	for (i = 0; i < stillRunningCount; i++)
	{
		int			sock;

		conn = stillRunning[i].segdbDesc->conn;
		sock = PQsocket(conn);
		FD_SET(sock, &input_mask);
		max_sock = max_sock < sock ? sock : max_sock;
	}
	
	/*
	 * OK, we are finished submitting the commnad to the segdbs.
	 * Now, we have to wait for them to finish.
	 */
	while (stillRunningCount > 0)
	{
		/* Make sure to do the setup for tv each time that select is called.  
		 * select changes their values!
		 */		
		int			n;
		struct timeval tv;

		tv.tv_sec = 2;
		tv.tv_usec = 0;



		/* reset out input_mask from the master copy. since select() 
		   changes the values of input_mask_copy each time.
		 */
		memcpy(&input_mask_copy, &input_mask, sizeof(fd_set));

		n = select(max_sock + 1, &input_mask_copy, NULL, NULL, &tv);


		if (n < 0)
		{
			if (SOCK_ERRNO == EINTR)
				continue;
			else
			{
				handleSelectError( stillRunning, stillRunningCount, segdbDesc, cancelFromBelow );
				return NULL;
			}
		}


		/*
		 * Before doing anything else, see whether any of the remaining connections went down.
		 * Hopefully in this way we can detect a dropped connection quickly.
		 */
		checkForBadConnections( stillRunning, &stillRunningCount, cancelFromBelow);


		if (n == 0)
		{
			handleSelectTimeout( stillRunning, &stillRunningCount );
			continue;
		}


		if (n > 0)
		{
			/*
			 * We have data waiting on one or more of the connections.
			 */
			int			x = 0;

			for (i = 0; i < stillRunningCount; i++)
			{
				PGconn *conn = stillRunning[i].segdbDesc->conn;
				int sock = PQsocket(conn);
				
	
				/* Don't use elog, it's not thread-safe */
				 
				/*
				 * if (DEBUG3 >= log_min_messages)
				 * fprintf(stderr, "thread_DispatchCommand checking on sock %d \n",sock);
				 */
				if (FD_ISSET(sock, &input_mask_copy))
				{
					processResults( &(stillRunning[i]), cancelFromBelow, pqresults, db_count, &resultCount );
					resultCount++;
				}
				else
				{
					stillRunning[x++] = stillRunning[i];
				}
			}

			stillRunningCount = x;

			/*
			 * Check again to see whether we should cancel.
			 */
			if (QueryCancelPending)
			{
				for (i = 0; i < stillRunningCount; i++)
				{
					if (!stillRunning[i].requestedCancel)
					{
						conn = stillRunning[i].segdbDesc->conn;
						PQrequestCancel(conn);
						stillRunning[i].requestedCancel = true;
					}
				}
			}
		}
	}


	free(stillRunning);

	return (NULL);
}

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
shouldStillDispatchCommand( SegmentDatabaseDescriptor *segdbDesc, 
							bool cancelFromBelow)
{
	CdbComponentDatabaseInfo *q    = segdbDesc->segment_database_info;
	PGconn 					 *conn = segdbDesc->conn;
		
	Assert(q != NULL);

	if (QueryCancelPending)
	{
		/* If there was a cancellation notice before we submit the command,
		 * then don't submit.
		 */
		segdbDesc->error_message = NULL;

		if ( !cancelledByUs )
		{					
			PQExpBufferData buffer;
			initPQExpBuffer(&buffer);

			appendPQExpBuffer(&buffer,
							  "Cancellation request noticed before command was issued to segment database"
							  "segindex %d isprimary %c", q->segindex,
							  (q->isprimary ? 't' : 'f'));
							  
			segdbDesc->error_message = buffer.data;
		}
		
		return false;
	}

	/*
	 * conn should never be NULL, but if it is, or if the status is BAD,
	 * then don't submit.
	 */
	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		PQExpBufferData buffer;
		ConnStatusType connStatus;
		
		initPQExpBuffer(&buffer);
		
		connStatus = PQstatus(conn);
		
		/*Don't use elog, it's not thread-safe*/			
		if (DEBUG3 >= log_min_messages)
		{
			fprintf(stderr, "thread_DispatchCommand connection bad: status: %d to segindex: %d "
							"isprimary %c not dispatching command \n", connStatus,
							q->segindex, (q->isprimary ? 't' : 'f'));
		} 

		appendPQExpBuffer(&buffer,
						  "Cannot issue command to segment database"
						  "segindex %d isprimary %c, because there is a problem with the connection:"
						  " connStatus: %d", q->segindex, (q->isprimary ? 't' : 'f'), connStatus);
						  
		segdbDesc->error_message = buffer.data;
		
		if (cancelFromBelow)
		{
			setCancelFlag();
			cancelledByUs = true;
		}
		
		return false;
	}
	
	return true;
}


/* Helper function to thread_DispatchCommand that actually kicks off the 
 * command on the libpq connection.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *       thread_DispatchCommand absolutely no elog'ing.
 *
 * cancelledByUS is a global?
 */
bool
dispatchCommand( SegmentDatabaseDescriptor *segdbDesc, const char *szCommand,
				 bool cancelFromBelow)
{
	CdbComponentDatabaseInfo *q    = segdbDesc->segment_database_info;
	PGconn 					 *conn = segdbDesc->conn;	

	/*
	 * Don't use elog, it's not thread-safe
	 */
	if (DEBUG3 >= log_min_messages)
	{
		fprintf(stderr,
			 "thread_DispatchCommand sending command %.40s...\n sent to "
				"port: %s with options %s and motionListener: %d\n",
				szCommand, PQport(conn), PQoptions(conn),
				PQmotionListener(conn));
	}
	
	/*
	 * Submit the command asynchronously.
	 */
	if (0 == PQsendQuery(conn, szCommand))
	{
		PQExpBufferData buffer;

		initPQExpBuffer(&buffer);

		/*
		 * Don't use elog, it's not thread-safe
		 */
		if (DEBUG3 >= log_min_messages)
			fprintf(stderr, "thread_DispatchCommand failed to send %s\n",
					szCommand);

		appendPQExpBuffer(&buffer, "SQL Command dispatch error: "
						  "segindex %d isprimary %c, %s",
						  q->segindex, (q->isprimary ? 't' : 'f'),
						  PQerrorMessage(conn));
		segdbDesc->error_message = buffer.data;
		
		if (cancelFromBelow)
		{
			setCancelFlag();
			cancelledByUs = true;
		}
		
		/* signal that we had some issues. */
		return false;
	}
	
	return true;
}


/* Helper function to thread_DispatchCommand that handles errors that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *       thread_DispatchCommand absolutely no elog'ing.
 *
 * cancelledByUS is a global?
 */
void handleSelectError( RunningInfo *stillRunning, 
						int stillRunningCount,
					    SegmentDatabaseDescriptor *segdbDesc, 
					    bool cancelFromBelow )
{
	PQExpBufferData buffer;
	int i;
	
	/*
	 * Don't use elog, it's not thread-safe
	 */
	if (DEBUG3 >= log_min_messages)
		fprintf(stderr, "thread_DispatchCommand encountered an error during select() with errno: %d\n",
				errno);
				
	/*
	 * Based on the select man page, we could get here with
	 * errno == EBADF (bad descriptor), EINVAL (highest descriptor negative or negative timeout)
	 * or ENOMEM (out of memory).
	 * Since this is most likely a programming error or a bad system
	 * failure, we will just error out all remaining threads.
	 */

	initPQExpBuffer(&buffer);
	appendPQExpBuffer(&buffer, "SQL Command error: socket select returned error, command status unknown.");
	
	for (i = 0; i < stillRunningCount; i++)
	{
		segdbDesc = stillRunning[i].segdbDesc;	
		segdbDesc->error_message = strdup(buffer.data);
	}

	if (cancelFromBelow)
	{
		setCancelFlag();
		cancelledByUs = true;
	}

	free(buffer.data);
	free(stillRunning);
	
}


void checkForBadConnections( RunningInfo *stillRunning ,int *stillRunningCount, bool cancelFromBelow)
{
	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo  *q;
	PGconn 					  *conn;
	
	int x = 0;
	int i;
	
	for (i = 0; i < *stillRunningCount; i++)
	{
		segdbDesc = stillRunning[i].segdbDesc;
		conn = segdbDesc->conn;
		
		if (PQstatus(conn) == CONNECTION_BAD)
		{
			PQExpBufferData buffer;

			q = segdbDesc->segment_database_info;
			Assert(q != NULL);


			initPQExpBuffer(&buffer);
			appendPQExpBuffer(&buffer,
							  "SQL Command error: lost connection for "
							  "segindex %d isprimary %c", q->segindex,
							  (q->isprimary ? 't' : 'f'));
			segdbDesc->error_message = buffer.data;
			if (cancelFromBelow)
			{
				setCancelFlag();
				cancelledByUs = true;
			}
		}
		else
		{
			stillRunning[x++] = stillRunning[i];
		}
	}

	*stillRunningCount = x;
}



/* Helper function to thread_DispatchCommand that handles timeouts that occur
 * during the select() call.
 *
 * NOTE: since this is called via a thread, the same rules apply as to
 *       thread_DispatchCommand absolutely no elog'ing.
 *
 * cancelledByUS is a global?
 */
void handleSelectTimeout( RunningInfo *stillRunning, 
						  int *stillRunningCount )
{
	int			counter = 0;
	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo  *q;
	int i;
	
	/*
	 * This is a timeout.  Check to see whether we should cancel.
	 */
	if (QueryCancelPending)
	{
		for (i = 0; i < *stillRunningCount; i++)
		{
			if (!stillRunning[i].requestedCancel)
			{
				PGconn *conn = stillRunning[i].segdbDesc->conn;
				PQrequestCancel(conn);
				stillRunning[i].requestedCancel = true;
			}
		}
	}

	/*
	 * check the connection still valid, set 1 min time interval
	  * this may affect performance, should turn it off if required.
	 */
	if (counter++ > 30)
	{
		int			x = 0;
		bool		sampling = true;

		for (i = 0; i < *stillRunningCount; i++)
		{
			segdbDesc = stillRunning[i].segdbDesc;
			if (!FtsTestConnection(segdbDesc->segment_database_info, segdbDesc->conn->dbName))
			{
				PQExpBufferData buffer;

				q = segdbDesc->segment_database_info;

				initPQExpBuffer(&buffer);
				appendPQExpBuffer(&buffer,
							"SQL Command error: lost connection for "
								  "segindex %d isprimary %c",
								  q->segindex,
								  (q->isprimary ? 't' : 'f'));
				segdbDesc->error_message = buffer.data;
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

		counter = 0;
	}

}

void processResults( RunningInfo *segInfo, bool cancelFromBelow, PGresult  	**pqresults, int db_count, int *resultCount )
{
	SegmentDatabaseDescriptor *segdbDesc = segInfo->segdbDesc;
	CdbComponentDatabaseInfo  *q         = segdbDesc->segment_database_info;
	PGconn 					  *conn      = segdbDesc->conn;				
	
	PGresult   *pRes;

	
	Assert(q != NULL);


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
	{
		segdbDesc->resultStatus = PQresultStatus(pRes);
		
		if (PGRES_COMMAND_OK != segdbDesc->resultStatus &&
			PGRES_TUPLES_OK != segdbDesc->resultStatus)
		{
			PQExpBufferData buffer;

			char	   *pqerrormsg = PQerrorMessage(conn);

			initPQExpBuffer(&buffer);

			/* If we requested a cancel, and we got back "cancelled", 
			 * that's not really an error
			 */
			if (cancelledByUs && segInfo->requestedCancel &&
				strcmp(pqerrormsg, "ERROR:  canceling query due to user request\n") == 0)
			{
				segdbDesc->resultStatus = 0;
			}
			else
			{
				appendPQExpBuffer(&buffer, "%s",pqerrormsg);
				segdbDesc->error_message = buffer.data;
			}

			if (cancelFromBelow)
			{
				setCancelFlag();
				cancelledByUs = true;
			}
		}
		else
		{
			char *cmdTuples;
			char *cmdStatus = PQcmdStatus(pRes);

			if (cmdStatus != NULL)
				segdbDesc->cmdStatus = strdup(cmdStatus);

			cmdTuples = PQcmdTuples(pRes);
			if (cmdTuples != NULL)
				segdbDesc->cmdTuples = atoi(cmdTuples);
		}

		if (pqresults != NULL)
		{
			/* check array index is in bounds */
			Assert(*resultCount < db_count);
			/* TODO: not handling multiple result sets */
			Assert(pqresults[*resultCount] == NULL);
			pqresults[*resultCount] = pRes;
		}
		else
			PQclear(pRes);
	}
}



/*
 * addSegDBToDispatchThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to have commands dispatched to.
 * It figures out which thread will handle it, based on the setting of
 * mpp_connections_per_thread.
 */
void
addSegDBToDispatchThreadPool(DispatchCommandParms * ParmsAr, int *segdbCount,
				 const char *szCommand, bool cancelFromBelow, bool isPrimary,
							 SegmentDatabaseDescriptor * segdbDesc)
{
	DispatchCommandParms *pParms;
	int			ParmsIndex;

	elog(DEBUG3, "addSegDBtoDispatchThreadPool %d", (*segdbCount));
	Assert(segdbCount != NULL);

	/*
	 * The proper index into the DispatchCommandParms array is computed, based on
	 * having mpp_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % mpp_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	ParmsIndex = (*segdbCount) / mpp_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];

	if ((*segdbCount) % mpp_connections_per_thread == 0)
	{
		pParms->szCommand = pstrdup(szCommand);
		pParms->segdbDescPtrArray =
			(SegmentDatabaseDescriptor **) palloc0(mpp_connections_per_thread *
										sizeof(SegmentDatabaseDescriptor *));
		pParms->cancelFromBelow = cancelFromBelow;
		pParms->isPrimary = isPrimary;
		memset(&pParms->thread, 0, sizeof(pthread_t));
		pParms->db_count = 0;
	}

	/*
	 * Just add to the end of the used portion of the segdbDescPtrArray
	 * and bump the count of members
	 */
	pParms->segdbDescPtrArray[pParms->db_count++] = segdbDesc;

	/*
	 * Bump the count of segdbs processed
	 */
	(*segdbCount)++;
	elog(DEBUG3, "addSegDBtoDispatchThreadPool %d done", (*segdbCount));

}

/*
 * initCdbDispatchResults:
 * Allocates and initializes a CdbDispatchResults struct,
 * based on static variables threadCount and dispatchCommandParmsAr.
 * This should be called after CdbDispatchCommand has been called, since
 * this is what sets these static variables.
 * The caller is responsible for calling DestroyCdbDispatchResults on the returned
 * pointer when done using it.
 */
CdbDispatchResults *
initCdbDispatchResults(void)
{
	int			i;
	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *q;
	CdbDispatchResults *results =
	(CdbDispatchResults *) palloc0(sizeof(CdbDispatchResults));
	DispatchCommandParms *pParms;

	results->success = true;
	results->count = 0;
	results->pResults = NULL;
	results->szCommand = NULL;

	for (i = 0; i < threadCount; i++)
	{
		pParms = &dispatchCommandParmsAr[i];
		results->count += pParms->db_count;

		if (i == 0 && pParms->szCommand != NULL)
			results->szCommand = pstrdup(pParms->szCommand);
	}

	if (results->count > 0)
	{
		int			x = 0;

		results->pResults =
			(CdbDispatchResult *) palloc0(results->count *
										  sizeof(CdbDispatchResult));


		for (i = 0; i < threadCount; i++)
		{
			int			j;

			pParms = &dispatchCommandParmsAr[i];


			for (j = 0; j < pParms->db_count; j++)
			{
				CdbDispatchResult *result;

				segdbDesc = pParms->segdbDescPtrArray[j];
				Assert(segdbDesc != NULL);
				q = segdbDesc->segment_database_info;
				Assert(q != NULL);

				result = &results->pResults[x++];

				result->segdbDesc = segdbDesc;
				result->error_message = NULL;
				result->cmdTuples = 0;
				result->cmdStatus = NULL;
				result->resultStatus = PGRES_COMMAND_OK;
				result->isPrimary = false;
			}
		}
	}

	return results;
}

/*
 * DestroyCdbDispatchResults:
 * pfrees all memory allocated in CdbDispatchResults struct.
 */
void
DestroyCdbDispatchResults(CdbDispatchResults * results)
{
	if (results == NULL)
		return;

	elog(DEBUG4, "DestroyCdbDispatchResults");

	if (results->count > 0)
	{
		int			i;

		for (i = 0; i < results->count; i++)
		{
			CdbDispatchResult *result = &results->pResults[i];

			if (result->error_message != NULL)
				pfree(result->error_message);

			if (result->cmdStatus != NULL)
				pfree(result->cmdStatus);
		}

		pfree(results->pResults);
	}

	if (results->szCommand != NULL)
		pfree(results->szCommand);

	pfree(results);

}

/*
 * SumCmdTuples:
 * Convenience routine for looping through and summing
 * the cmdTuples values for all elements of results->pResults.
 * Used to determine the row counts for insert, update, and delete commands.
 */
int
SumCmdTuples(CdbDispatchResults * results)
{
	int			i;
	int			cmdTuples = 0;

	for (i = 0; i < results->count; i++)
	{
		CdbDispatchResult *result = &results->pResults[i];

		if (result->isPrimary)
			cmdTuples += result->cmdTuples;
	}

	return cmdTuples;
}

/*
 * CdbDoCommandInt:
 * Combination of CdbDispatchCommand and then CdbCheckDispatchResult.
 * If the command successfully ran, returns true, else error is thrown.
 */
bool
CdbDoCommandInt(const char *szCommand,
				bool cancelFromBelow, void ***resultSets)
{
	CdbDispatchResults *results = NULL;
	bool		rtn = true;
	char	   *message_do;


	Assert(szCommand != NULL);

	cancelledByUs = false;

	elog(DEBUG3, "DoCommandInt calling DispatchCommandInt");
	CdbDispatchCommandInt(szCommand, cancelFromBelow, resultSets);
	elog(DEBUG3, "DoCommandInt checking result");
	results = CdbCheckDispatchResult();

	message_do = CdbDumpDispatchResults(results, true);
	Assert(message_do != NULL);
	elog(DEBUG5,
		 "mppexec command results for \"%s\": %s", szCommand, message_do);

	pfree(message_do);

	if (!results->success)
	{
		message_do = CdbDumpDispatchResults(results, false);
		rtn = false;

		ereport(ERROR,
				(errcode(ERRCODE_CDB_COMMAND_ERROR),
				 errmsg
				 ("%s",
				  message_do)));

		pfree(message_do);
	}

	DestroyCdbDispatchResults(results);

	return rtn;
}

/*
 * CdbDoCommand:
 * Combination of CdbDispatchCommand and then CdbCheckDispatchResult.
 * If the command successfully ran, returns true, else error is thrown.
 * Called by general users, this method includes global transaction control.
 */
bool
CdbDoCommand(const char *szCommand, bool cancelFromBelow)
{
	bool		ret;

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

	elog(DEBUG3, "DoCommand calling DoCommandInt");
	ret = CdbDoCommandInt(szCommand, cancelFromBelow, NULL);
	return ret;
}


/*
 * CdbDumpDispatchResults:
 * Formats a string with results info.
 * If verbose = true, reports on all target databases.
 * If false, reports on only failures.
 */
char *
CdbDumpDispatchResults(CdbDispatchResults * results, bool verbose)
{
	StringInfoData buffer;
	int			   i;

	Assert(results != NULL);

	if (!verbose && results->success)
		return NULL;

	initStringInfo(&buffer);
	appendStringInfo(&buffer, "results from QEs:\n");
	
	for (i = 0; i < results->count; i++)
	{
		CdbDispatchResult *result = &results->pResults[i];

		CdbDumpDispatchResult(result, verbose, &buffer);
	}

	return buffer.data;
}

void
CdbDumpDispatchResult(CdbDispatchResult * result, bool verbose, StringInfo message)
{
	CdbComponentDatabaseInfo *q;
	char 					 *detailMsg = NULL;
	
	Assert(result != NULL);
	Assert(result->segdbDesc != NULL);
	Assert(message != NULL);

	q = result->segdbDesc->segment_database_info;
	Assert(q != NULL);

	
	if (!verbose && result->error_message == NULL)
		return;

	appendStringInfo(message, "\tdbId#%d seg#%d%c - ", q->dbid, q->segindex, (q->isprimary ? 'P': 'M'));
	
	if( verbose )
	{
		char  *error_message = result->error_message;
		char  *cmdStatus = result->cmdStatus;

		if (error_message == NULL)
			error_message = "NULL";

		if (cmdStatus == NULL)
			cmdStatus = "NULL";
		
		/* TODO: convert resultStatus into a text string and not a cryptic number. */
		appendStringInfo(message, "cmdTuples: %d cmdStatus: %s resultStatus: %d\n",
			result->cmdTuples, cmdStatus, result->resultStatus );
	}

	/* see if the elog message has a DETAIL: part to it. if it does we'll split
	 * the normal part of the message apart from the DETAIL portion so we can
	 * format it better.
	 */
	if( result->error_message != NULL ) 
		detailMsg = strstr( result->error_message, "DETAIL:");
	
	if( detailMsg != NULL )
	{
		int detailStart = detailMsg - (result->error_message);			
			
		appendBinaryStringInfo(message, result->error_message, detailStart ); 			
		appendStringInfo(message, "\t\t\t%s", detailMsg );
	}
	else
	{
		appendStringInfo(message, "%s", result->error_message ); 
	}

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
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}
}



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
CdbDispatchPlan(QueryDesc *queryDesc)
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


	cancelledByUs = false;

	elog(DEBUG4, "CdbDispatchPlan");

	/*
	 * need to start distributed transaction
	 */
	condStartDtxTransaction();

	/*
	 * There should be no outstanding threads when we are here
	 *
	 */
	Assert(threadCount == 0);


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
		CdbDispatchToGang(strinfo.data, primaryGang, true, NULL);


		/*
		 * dispatch the plan to our mirrorGang if we have one.
		 */
		if (mirrorGang != NULL && queryDesc->operation != CMD_SELECT)
		{
			/*
			 * SegmentDatabaseDescriptor ** sdpMirror;
			 */

			elog(DEBUG3, "Slice #%d has a non-null mirror gang.", i);


			CdbDispatchToGang(strinfo.data, mirrorGang, true, NULL);

		}


		pfree(strinfo.data);
		initStringInfo(&strinfo);
	}

	pfree(mppexecPrefix);

	sliceTbl->localSlice = oldLocalSlice;
	elog(DEBUG4, "CdbDispatchPlan done");
}


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
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("bad slice vector allocation")));
	}

	indexVector[next++] = sliceIndex;
	return next;
}


void
CdbDispatchUtilityStatement(Node *stmt)
{
	char	   *szCommand;

	/*
	 * Serialize the stmt tree, and create the sql statement: mppexec ....
	 */
	Query	   *q = makeNode(Query);

	StringInfoData buffer;

	initStringInfo(&buffer);

	q->commandType = CMD_UTILITY;
	q->utilityStmt = stmt;

	q->querySource = QSRC_ORIGINAL;
	q->canSetTag = true;		/* ? */


	szCommand = serializeNode((Node *) q);

	Assert(szCommand != NULL);
	Assert(Mpp_role == MPP_ROLE_DISPATCH);

	elog(DEBUG1, "Comand %s", szCommand);

	appendStringInfo(&buffer, "mppexec '%s' ''", szCommand);

	pfree(szCommand);

	CdbDoCommand(buffer.data, false);

}
