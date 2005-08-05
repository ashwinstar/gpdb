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
#include "storage/ipc.h"  /* For proc_exit_inprogress  */
#include "miscadmin.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbtm.h"

#include "cdb/cdbgang.h"
#include "cdb/cdblink.h"  /* just for our CdbProcess population hack. */

#include "cdb/cdbsrlz.h"
#include "cdb/cdbsubplan.h"

/*
 * Parameter structure for the DispatchCommand threads
 */
typedef struct DispatchCommandParms
{
	/*
	 * The SQL command
	 */
	char					*szCommand;

	/*
	 * db_count: The number of segdbs that this thread is responsible
	 * for dispatching the command to.
	 * Equals the count of segdbDescPtrArray below.
	 */
	int							db_count;
	
	/*
	 * segdbDescPtrArray: Array of SegmentDatabaseDescriptor* 's that this thread is
	 * responsible for dispatching the command to. Has size equal to db_count.
	 */
	SegmentDatabaseDescriptor	**segdbDescPtrArray;

	/*
	 * A flag indicating whether a failure of the command for one connection
	 * should act to requestCancel for all the other connections.
	 *
	 */
	bool						cancelFromBelow;
	
	/*
	 * The pthread_t thread handle.
	 */
	pthread_t					thread;

    /*
     * Returned resultset
     */
    PGresult                      ***resultSets;

} DispatchCommandParms;

/*
 * Structure used in thread proc for keeping track of
 * commands that are still running.  For each command
 * that has not yet finished, we keep the segdbDesc pointer and
 * a flag indicating whether we've  requested cancel.
 */
typedef struct
{
	SegmentDatabaseDescriptor	*segdbDesc;
	bool						requestedCancel;	
}RunningInfo;

/*
 * Structure used when targetSet is DTS_LISTENER.
 * In that case, we need to sort the segdbDesc's by listenerid
 * and then choose one per listener only.
 */
typedef struct
{
	int							listenerid;
	SegmentDatabaseDescriptor	*segdbDesc;
} ListenerSegDBPair;

/*
 * Static Helper functions
 */
static void *thread_DispatchCommand(void *arg);
/*static int listenerSegDBPairCompare(const void *p1, const void *p2);
static void createThreadPoolPerListener( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow);
static void createThreadPoolPerSegment( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow); 
static void createThreadPoolPerSegDB( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow);*/
static void createThreadPoolEnumeration( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow,
										 SegmentDatabaseDescriptor **segdbList, int segdbListCount);
static void addSegDBToDispatchThreadPool( DispatchCommandParms *ParmsAr, int *segdbCount, const char *szCommand,
										bool cancelFromBelow, SegmentDatabaseDescriptor *segdbDesc);
static CdbDispatchResults *initCdbDispatchResults(void);
static void CdbDumpDispatchResult( CdbDispatchResult *result, bool verbose, StringInfo message );
static void setCancelFlag(void);

static int fillSliceVector(SliceTable *sliceTable, int sliceIndex, int *indexVector, int len, int next);


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
static DispatchCommandParms* dispatchCommandParmsAr = NULL;
static int	threadCount = 0;

/*
 * CdbDispatchCommandInt:
 * Send ths szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the targetSet parameter.  cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.  They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable cdb_connections_per_thread.
 */
void CdbDispatchCommandInt( const char *szCommand, DispatchTargetSet targetSet,
                        SegmentDatabaseDescriptor **segdbList, int segdbListCount,
                        bool cancelFromBelow, void ***resultSets )
{
	int i;
	int max_threads;
	int segdbs_in_thread_pool;
	//int segdb_count;
	
	if ( Cdb_role != CDB_ROLE_DISPATCH )
		return;

	Assert( segdbList == NULL || targetSet == DTS_ENUMERATION );
	Assert( segdbListCount == 0 || targetSet == DTS_ENUMERATION );
	Assert( segdbList == NULL || segdbListCount > 0 );
	
	//segdb_count = CdbClusterDefinition->segdb_count;
	//Assert( segdb_count > 0 );
	
	/*
	 * The most threads we could have is segdb_count / cdb_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / cdb_connections_per_thread.
	 * We allocate enough memory for this many DispoatchCommandParms structures,
	 * even though we may not use them all.
	 */
	Assert( cdb_connections_per_thread > 0 );
	
	segdbs_in_thread_pool = 0;
	
	if ( targetSet == DTS_SEGDB )
	{
		int k = 0;
		int m = 0;
		
		Gang * primaryGang = allocateGang(GANGTYPE_PRIMARY_WRITER, getCdbSegmentCount(), -1);
		Gang * mirrorGang = allocateGang(GANGTYPE_MIRROR_WRITER, getCdbSegmentCount() , -1);
		Assert(primaryGang != NULL);
		
		m = primaryGang->segdb_count;
		if (mirrorGang != NULL)
			m += mirrorGang->segdb_count;
			
		max_threads =  1 + (m-1) / cdb_connections_per_thread;
		dispatchCommandParmsAr = (DispatchCommandParms *)
							palloc0( max_threads * sizeof(DispatchCommandParms) );
		
		segdbList = palloc(m * sizeof(SegmentDatabaseDescriptor*));
		segdbListCount = 0;
		for (k = 0; k < primaryGang->segdb_count; k++)
		{
			segdbList[segdbListCount] = &primaryGang->db_descriptors[k];
			segdbListCount++;
		}
		if (mirrorGang != NULL)
			for (k = 0; k < mirrorGang->segdb_count; k++)
			{
				segdbList[segdbListCount] = &primaryGang->db_descriptors[k];
				segdbListCount++;
			}
		
		elog(DEBUG2,"CdbDispatchCommandInt Dispatching to DTS_SEGDB segdbs = %d", segdbListCount);	
		
		
	} 
	else
	{
		max_threads =  1 + (segdbListCount-1) / cdb_connections_per_thread;
		dispatchCommandParmsAr = (DispatchCommandParms *)
							palloc0( max_threads * sizeof(DispatchCommandParms) );
	}
	/*
	 * Create the thread parms structures based targetSet parameter.
	 * This will add the segdbDesc pointers appropriate to the
	 * targetSet into the thread Parms structures, amking sure that each thread
	 * handles cdb_connections_per_thread segdbs.
	 */
	/*switch ( targetSet )
	{
		case DTS_LISTENER:
			createThreadPoolPerListener( dispatchCommandParmsAr, &segdbs_in_thread_pool,
										 szCommand, cancelFromBelow);
			break;

		case DTS_SEGMENT:
			createThreadPoolPerSegment( dispatchCommandParmsAr, &segdbs_in_thread_pool,
										 szCommand, cancelFromBelow);
			break; 

		case DTS_SEGDB:
			createThreadPoolPerSegDB( dispatchCommandParmsAr, &segdbs_in_thread_pool,
										 szCommand, cancelFromBelow);
			break;

		case DTS_ENUMERATION:*/
			createThreadPoolEnumeration( dispatchCommandParmsAr, &segdbs_in_thread_pool,
										 szCommand, cancelFromBelow, segdbList, segdbListCount);
			/*break;

	}*/

    /* set result set variables if return of result set is required */
    if (resultSets != NULL)
        dispatchCommandParmsAr->resultSets = (PGresult***) resultSets;

	/*
	 * Compute the thread count based on how many segdbs were added into the
	 * thread pool, knowing that each thread handles cdb_connections_per_thread
	 * segdbs.
	 */						
	if ( segdbs_in_thread_pool == 0 )
		threadCount = 0;
	else	
		threadCount = 1 + (segdbs_in_thread_pool - 1) / cdb_connections_per_thread;
		
	elog(DEBUG4,"CdbDispatchCommandInt threadCount %d, segdbs_in_thread_pool %d, conns_per_thread %d", threadCount, segdbs_in_thread_pool, cdb_connections_per_thread );	
	

	/*
	 * Create the threads.
	 */						
	for ( i=0; i<threadCount; i++ )
	{
		DispatchCommandParms* pParms = &dispatchCommandParmsAr[i];

		elog(DEBUG2, "CdbDispatchCommandInt Creating thread %d of %d for CommandDispatch", i+1, threadCount);

		if (0!=pthread_create(&pParms->thread,
						NULL,
						thread_DispatchCommand,
						pParms))
						{
							elog(NOTICE, "pthread_create failed");
						}
	}
}

/*
 * CdbDispatchCommand:
 * Send ths szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the targetSet parameter.  cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.  They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable cdb_connections_per_thread.
 * Called by general users, this method includes global transaction control.
 */
void CdbDispatchCommand( const char *szCommand, DispatchTargetSet targetSet,
                        SegmentDatabaseDescriptor **segdbList, int segdbListCount,
                        bool cancelFromBelow )
{
    condStartDtxTransaction();
    CdbDispatchCommandInt( szCommand, targetSet,
                        segdbList, segdbListCount,
                        cancelFromBelow, NULL );
}

/*
 * CdbCheckDispatchResult:
 * Waits for threads that were launched in CdbDispatchCommand
 * to finish, and then checks to see whether they all succeeded.
 * If so, returns true.  If not, returns false, and fills in pszError
 * out parameter with a concatenation of errors from any failed command.
 */
CdbDispatchResults *CdbCheckDispatchResult(void)
{
	int							i;
	int							j;
	int x = 0;
	SegmentDatabaseDescriptor	*segdbDesc;
	DispatchCommandParms		*pParms;
	CdbDispatchResults			*results = initCdbDispatchResults();

	Assert( results != NULL );
	
	elog(DEBUG4,"CheckDispatchResult threadCount %d", threadCount );	

	if ( threadCount == 0 )
		return results;

	/*
	 * Wait for threads to finish.
	 */						
	for ( i=0; i<threadCount; i++ )		
	{
		pParms = &dispatchCommandParmsAr[i];
		
		elog(DEBUG2, "Joining to thread %d of %d for CommandDispatch", i+1, threadCount);
		
		if (0!=pthread_join( pParms->thread, NULL ))
		{
			elog(NOTICE, "pthread_join failed");
		}
	}
	
	/*
	 * Check whether there were error messages.
	 * This is stored in the error_message field of the
	 * segment_database_descriptor.
	 */			

	elog(DEBUG4,"CheckDispatchResult joined, checking" );	
	x = 0;

	for ( i=0; i<threadCount; i++ )
	{
		pParms = &dispatchCommandParmsAr[i];
		
		for ( j=0; j<pParms->db_count; j++ )
		{
			CdbDispatchResult *result;
			
			segdbDesc = pParms->segdbDescPtrArray[j];
			Assert(	segdbDesc != NULL );

			result = &results->pResults[x++];
			
			if ( segdbDesc->error_message != NULL )
			{
				char *pszErrMsg;
				results->success = false;
				
	 		/* Because we cannot use palloc inside a thread, the error messages
	 		 * were malloc'd. So we must free the malloc'd memory after
	 		 * appending them to the StringInfoData.
	 		 * We must zero out the segdbDesc->error_message too.
			 */
				pszErrMsg = segdbDesc->error_message;
				segdbDesc->error_message = NULL;
				
				result->error_message = pstrdup( pszErrMsg );
				free( pszErrMsg );
			}

			if ( segdbDesc->cmdStatus != NULL )
			{
				char *cmdStatus = segdbDesc->cmdStatus;
				segdbDesc->cmdStatus = NULL;
				
				result->cmdStatus = pstrdup( cmdStatus );
				free( cmdStatus );
			}

			result->cmdTuples = segdbDesc->cmdTuples;
			segdbDesc->cmdTuples = 0;

			result->resultStatus = segdbDesc->resultStatus;
			segdbDesc->resultStatus = PGRES_COMMAND_OK;;
		}
	}	
	
	elog(DEBUG4,"CheckDispatchResult freeing" );	
	
	/*
	 * pfree the memory allocated for the dispatchCommandParmsAr
	 */
	if ( dispatchCommandParmsAr != NULL )
	{
		for ( i=0; i<threadCount; i++ )
		{
			pParms = &dispatchCommandParmsAr[i];
			Assert( pParms != NULL && pParms->segdbDescPtrArray != NULL);
			pfree( pParms->segdbDescPtrArray );
			pParms->segdbDescPtrArray = NULL;
			Assert( pParms->szCommand != NULL );
			pfree(  pParms->szCommand );
			pParms->szCommand = NULL;
		}
		pfree( dispatchCommandParmsAr );
		dispatchCommandParmsAr = NULL;
	}
	
	elog(DEBUG4,"CheckDispatchResult done" );	
    	
	threadCount = 0;

	return results;	
}

/*
 *	thread_DispatchCommand is the thread proc used to dispatch the command to one or more of the qExecs.
 */
void *thread_DispatchCommand(void *arg)
{
	DispatchCommandParms		*pParms				= (DispatchCommandParms *)arg;
	const char					*szCommand			= pParms->szCommand;
	SegmentDatabaseDescriptor **segdbDescPtrArray	= pParms->segdbDescPtrArray;
	int							db_count			= pParms->db_count;
	bool						cancelFromBelow		= pParms->cancelFromBelow;
	int							i;
	SegmentDatabaseDescriptor	*segdbDesc;
	CdbComponentDatabaseInfo	*q;
	PGconn						*conn;
	RunningInfo					*stillRunning = NULL;
	int							stillRunningCount = 0;
	PGresult **pqresults = NULL;

	/*
	 * In threads, we cannot use palloc, because it's not thread safe.
	 */
	stillRunning = (RunningInfo *)calloc(db_count, sizeof(RunningInfo));

    
    if (pParms->resultSets != NULL)
    {
        pqresults = (PGresult **)calloc(db_count, sizeof(pqresults));
        *pParms->resultSets = pqresults;
    }
					
	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to send commands through to.
	 */
	for ( i=0; i<db_count; i++ )
	{
		//elog(DEBUG3, "thread_DispatchCommand working on %d", i);
		segdbDesc = segdbDescPtrArray[i];
		conn = segdbDesc->conn;
		q = segdbDesc->segment_database_info;
		Assert( q != NULL );
		
		if ( QueryCancelPending )
		{
			/*
			 * If there was a cancellation notice before we submit the command,
			 * then don't submit.
			 */
			PQExpBufferData buffer;
			initPQExpBuffer(&buffer);
			
			appendPQExpBuffer(&buffer, "Cancellation request noticed before command was issued to segment database"
										"segindex %d isprimary %c",
										q->segindex, (q->isprimary ? 't' : 'f')),
									
			segdbDesc->error_message = buffer.data;
			continue;
		}
		
		/*
		 * conn should never be NULL, but if it is, or if the status is BAD,
		 * then don't submit.
		 */
		if ( conn == NULL || PQstatus( conn ) == CONNECTION_BAD )
		{
			PQExpBufferData buffer;
			initPQExpBuffer(&buffer);
			//elog(DEBUG3, "thread_DispatchCommand connection bad ");
			
			appendPQExpBuffer(&buffer, "Cannot issue command to segment database"
										"segindex %d isprimary %c, because there is no connection",
										q->segindex, (q->isprimary ? 't' : 'f')),
									
			segdbDesc->error_message = buffer.data;
			if ( cancelFromBelow )
				setCancelFlag();
			continue;
		}
		//elog(DEBUG3, "thread_DispatchCommand sending %s", szCommand);
		/*
		 * Submit the command asynchronously.
		 */
		if ( 0 == PQsendQuery(conn, szCommand) )
		{
			PQExpBufferData buffer;
			initPQExpBuffer(&buffer);
			
			//elog(DEBUG3, "thread_DispatchCommand failed to send %s", szCommand);
			
			appendPQExpBuffer( &buffer, "SQL Command error: "
										"segindex %d isprimary %c, %s",
										q->segindex, (q->isprimary ? 't' : 'f'),
										PQerrorMessage(conn) );
			segdbDesc->error_message = buffer.data;
			if ( cancelFromBelow )
				setCancelFlag();
			continue;
		}
		
		/*
		 * OK, command was successfully submitted. Add to the stillRunning array
		 * so we can monitor it.
		 */
		stillRunning[stillRunningCount].segdbDesc = segdbDesc;
		stillRunning[stillRunningCount].requestedCancel = false;
		stillRunningCount++;
	}
	
	/*
	 * OK, we are finished submitting the commnad to the segdbs.
	 * Now, we have to wait for them to finish.
	 */
	while ( stillRunningCount > 0 )
	{
		/*
		 * Make sure to do the setup for input_mask and tv each time that
		 * select is called.  select changes their values!
		 */
		fd_set input_mask;
		int max_sock = 0;
		int n;
		int x = 0;
		struct timeval tv;
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		
		
		FD_ZERO( &input_mask );
		
		
		for ( i=0; i<stillRunningCount; i++ )
		{
			int sock;
			conn = stillRunning[i].segdbDesc->conn;
			sock = PQsocket( conn );
			FD_SET( sock, &input_mask );
			max_sock = max_sock < sock ? sock : max_sock;
		}
		
		 
	
		n = select( max_sock+1, &input_mask, NULL, NULL, &tv );
		
		//elog(DEBUG3, "thread_DispatchCommand done socket select %d ", n);
	
		if ( n < 0 )
		{
			PQExpBufferData buffer;
			if ( SOCK_ERRNO == EINTR )
			{
				continue;		
			}
			
			/*
			 * Based on the select man page, we could get here with
			 * errno == EBADF (bad descriptor), EINVAL (highest descriptor negative or negative timeout)
			 * or ENOMEM (out of memory).
			 * Since this is most likely a programming error or a bad system
			 * failure, we will just error out all remaining threads.
			 */
			
			initPQExpBuffer(&buffer);
			appendPQExpBuffer( &buffer, "SQL Command error: socket select returned error, command ststus unknown.");
			for ( i=0; i<stillRunningCount; i++ )
			{
				segdbDesc = stillRunning[i].segdbDesc;
				segdbDesc->error_message = strdup(buffer.data);
			}

			if ( cancelFromBelow )
				setCancelFlag();
				
			free( buffer.data );
			return NULL;	
		}
		
		/*
		 * Before doing anything else, see whether any of the remaining connections went down.
		 * Hopefully in this way we can detect a dropped connection quickly.
		 */
		x = 0;
		//elog(DEBUG3, "thread_DispatchCommand check for bad connections ");
		for ( i=0; i<stillRunningCount; i++ )
		{
			segdbDesc = stillRunning[i].segdbDesc;
			conn = segdbDesc->conn;
			if ( PQstatus( conn ) == CONNECTION_BAD )
			{
				PQExpBufferData buffer;
				q = segdbDesc->segment_database_info;
				Assert( q != NULL );
				
				
				initPQExpBuffer(&buffer);
				appendPQExpBuffer( &buffer, "SQL Command error: lost connection for "
											"segindex %d isprimary %c",
											q->segindex, (q->isprimary ? 't' : 'f') );
				segdbDesc->error_message = buffer.data;
				if ( cancelFromBelow )
					setCancelFlag();
			}
			else
			{
				stillRunning[x++] = stillRunning[i];
			}
		}
		
		stillRunningCount = x;
		//elog(DEBUG3, "thread_DispatchCommand stillRunningCount %d ",stillRunningCount);
		
		if ( n == 0  )
		{
			/*
			 * This is a timeout.  Check to see whether we should cancel.
			 */
			if ( QueryCancelPending )
			{
				for ( i=0; i<stillRunningCount; i++ )
				{
					if ( !stillRunning[i].requestedCancel )
					{
						conn = stillRunning[i].segdbDesc->conn;
						PQrequestCancel( conn );
						stillRunning[i].requestedCancel = true;
					}
				}
			}
			
			continue;
		}
		
		if ( n > 0 )
		{
			/*
			 * We have data waiting on one or more of the connections.
			 */
			int x = 0;
			for ( i=0; i<stillRunningCount; i++ )
			{
				int sock;
				segdbDesc = stillRunning[i].segdbDesc;
				conn = segdbDesc->conn;
				//elog(DEBUG3, "thread_DispatchCommand checking on connection %d ",i);
				q = segdbDesc->segment_database_info;
				Assert( q != NULL );
				

				sock = PQsocket( conn );
				//elog(DEBUG3, "thread_DispatchCommand checking on sock %d ",sock);
				if ( FD_ISSET( sock, &input_mask ) )
				{
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
					PGresult *pRes;
					while ( NULL != (pRes = PQgetResult(conn) ) )
					{
						segdbDesc->resultStatus = PQresultStatus( pRes );
						//elog(DEBUG3, "thread_DispatchCommand resultStatus %d ",segdbDesc->resultStatus);
						if ( PGRES_COMMAND_OK != segdbDesc->resultStatus &&
							PGRES_TUPLES_OK != segdbDesc->resultStatus )
						{
							PQExpBufferData buffer;
							initPQExpBuffer(&buffer);
							appendPQExpBuffer( &buffer, "SQL Command error: "
														"resultStatus %d "
														"segindex %d isprimary %c, %s",
														segdbDesc->resultStatus,
														q->segindex, (q->isprimary ? 't' : 'f'),
														PQerrorMessage(conn) );
							if ( segdbDesc->error_message != NULL )
							{
								free( segdbDesc->error_message );														
								segdbDesc->error_message = NULL;
							}

							segdbDesc->error_message = buffer.data;
							
							if ( cancelFromBelow )
								setCancelFlag();
							
						}
						else
						{
							char *cmdTuples;
							char *cmdStatus = PQcmdStatus(pRes);
							if ( cmdStatus != NULL )
								segdbDesc->cmdStatus = strdup(cmdStatus);

							cmdTuples = PQcmdTuples(pRes);
							if ( cmdTuples != NULL )
								segdbDesc->cmdTuples = atoi(cmdTuples);
						}
						
				        if (pqresults != NULL)
                         {
                            /*
                             * TODO:   need to handle multiple result sets
                             * check the segindex for segment case
                             */
                   
                   // FIXME Turn off this assert because it keeps the system from coming up with mirroring defined
                            //Assert(pqresults[segdbDesc->segindex] == NULL);
                            pqresults[segdbDesc->segindex] = pRes;
                         }
                         else		
						  PQclear( pRes );
					}
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
			if ( QueryCancelPending )
			{
				for ( i=0; i<stillRunningCount; i++ )
				{
					if ( !stillRunning[i].requestedCancel )
					{
						conn = stillRunning[i].segdbDesc->conn;
						PQrequestCancel( conn );
						stillRunning[i].requestedCancel = true;
					}
				}
			}
		}
	}
	//elog(DEBUG3, "thread_DispatchCommand done ");
	
	free(stillRunning);
	
	return(NULL);
}

#if 0
/*
 * createThreadPoolPerListener:
 * This function figures out a subset of the segdbs so that there is exactly
 * one segdb per listener.  It is used for commands that have listener scope, like
 * create user.  It assigns each segdb in the subset to a thread in the thread pool.
 * This is based on each thread handling cdb_connections_per_thread connections.
 */
void createThreadPoolPerListener( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
								 const char *szCommand, bool cancelFromBelow)
{
	/*
	 * The code in this function makes the assumption that
	 * valid listenerids are always >= 0
	 */
	
	/*
	 * Here's the strategy.  We only want to send the command to
	 * one segdb per listener.  In order to determine what this subset is,
	 * we create a structure with the segdbDesc and listenerid for each
	 * valid segdb, sort it by listenered.  Then we can loop through and easily
	 * pick out one segdbDesc to use per listener.
	 * We also need to make sure that for each listenerid with at least one valid segdb,
	 * we find a connected segdb.  If not, it's an error.
	 *
	 */
	int segdb_count = CdbClusterDefinition->segdb_count;
	int i;
	int j;
	int	PairArCount = 0;
	SegmentDescriptor *segDesc;
	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *q;
	ListenerSegDBPair* pPairAr = NULL;
	int lastListenerID = -1;
	bool bFoundConnection = true;
	
	pPairAr = (ListenerSegDBPair *)palloc0( sizeof(ListenerSegDBPair) * segdb_count );
	
	for ( i=0; i<getCdbSegmentCount(); i++ )
	{
		segDesc	= &CdbClusterDefinition->segment_descriptors[i];
		for ( j=0; j<segDesc->db_count; j++ )
		{
			segdbDesc = &segDesc->db_descriptors[j];
			q	= segdbDesc->segment_database_info;

			pPairAr[PairArCount].listenerid = q->listenerid;
			pPairAr[PairArCount].segdbDesc = segdbDesc;
			PairArCount++;
		}
	}
	
	Assert( PairArCount == segdb_count -1 );
	
	/*
	 *  Sort pPairAr by listenerid
	 */
	qsort( pPairAr, PairArCount, sizeof(ListenerSegDBPair),
		  listenerSegDBPairCompare);
			
	/*
	 * Pick out one segdbDesc per distinct listenerid, and avoid the listenerid
	 * of the dispatcher.
	 */
	
	 /*
	  * We need to insure that for every listenerid in the pPairAr,
	  * we find a connected segdb.  Otherwise, we need to report an error.
	  * An example where this could happen:
	  * 2 listeners, 2 segments, 4 segdb's.
	  * listener 1: segindex 1 primary, segindex 2 secondary.
	  * listener 2: segindex 2 primary, segindex 1 secondary.
	  * If during cdblink_connect, we failed to connect to segindex 2 primary
	  * and segindex 1 secondary, we  would have passes cdblink_setup, because
	  * there was at least one connected segdb for each segindex.
	  * But we would have no connection to listener2.
	  */
	
	for ( i=0; i<PairArCount; i++ )
	{
		if ( pPairAr[i].listenerid != lastListenerID  )
		{
			/* Check to see if we got a connection for the last listenerid */
			if ( !bFoundConnection )
			{
				/* This is an error condition */	
				ereport( ERROR,
							(errcode( ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION ),
							 errmsg("Cannot dispatch command %s to all listeners: "
							 		"there is no connection to any segdbs for listenerid %d",
							 		szCommand, lastListenerID
						)));
				
			}
			
			bFoundConnection = false;
			lastListenerID = pPairAr[i].listenerid;
		}
		
		if ( !bFoundConnection )
		{
			segdbDesc = pPairAr[i].segdbDesc;
		
			if ( segdbDesc->conn != NULL )
			{
				addSegDBToDispatchThreadPool( dispatchCommandParmsAr, psegdbs_in_thread_pool, szCommand, cancelFromBelow, segdbDesc );
				bFoundConnection = true;
			}
		}
	}
	
	/* Check to see if we got a connection for the last listenerid */
	if ( !bFoundConnection )
	{
		/* This is an error condition	 */
		ereport( ERROR,
					(errcode( ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION ),
					 errmsg("Cannot dispatch command %s to all listeners: "
					 		"there is no connection to any segdbs for listenerid %d",
					 		szCommand, lastListenerID
				)));
		
	}
			
	
	pfree( pPairAr );
}

/*
 * createThreadPoolPerSegment:
 * This function figures out a subset of the segdbs so that there is exactly
 * one segdb per segment.  It is used for commands that have database scope, but shouldn't
 * be executed for both primary and secondary copies, like
 * a select statement.  It assigns each segdb in the subset to a thread in the thread pool.
 * This is based on each thread handling cdb_connections_per_thread connections.
 */
void createThreadPoolPerSegment( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
								 const char *szCommand, bool cancelFromBelow)
{
	int i;
	SegmentDescriptor *segDesc;
	
	for ( i=0; i<getCdbSegmentCount(); i++ )
	{
		segDesc = &CdbClusterDefinition->segment_descriptors[i];

		/*
		 * The query_db was set up for just this purpose.  So use it.
		 */
		Assert( segDesc->query_db != NULL );
		addSegDBToDispatchThreadPool( dispatchCommandParmsAr, psegdbs_in_thread_pool, szCommand, cancelFromBelow, segDesc->query_db );
	}
}


/*
 * createThreadPoolPerSegDB:
 * This function uses all segdbs that have a valid connection.  It is used for commands that have database scope,
 * and should be executed for both primary and secondary copies, like
 * a create table statement.  It assigns each segdb in the subset to a thread in the thread pool.
 * This is based on each thread handling cdb_connections_per_thread connections.
 */
void createThreadPoolPerSegDB( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
								 const char *szCommand, bool cancelFromBelow)
{
	int i;
	int j;
	SegmentDescriptor *segDesc;
	SegmentDatabaseDescriptor *segdbDesc;
	
	/* Validate that we have connections that we need */
	if ( !CdbClusterDefinition->all_valid_segdbs_connected )
	{
		ereport( ERROR,
					(errcode( ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION ),
					 errmsg("Cannot dispatch command %s to all segdbs: "
					 		"there is no connection to at least one valid segdb",
					 		szCommand
				)));
	}
	
	for ( i=0; i<getCdbSegmentCount(); i++ )
	{
		segDesc = &CdbClusterDefinition->segment_descriptors[i];
		
		for ( j=0; j<segDesc->db_count; j++ )
		{
			segdbDesc = &segDesc->db_descriptors[j];
		
			if ( segdbDesc->conn != NULL )
			{
				addSegDBToDispatchThreadPool( dispatchCommandParmsAr, psegdbs_in_thread_pool, szCommand, cancelFromBelow, segdbDesc );
			}
		}
	} 	
}
#endif 


/*
 * createThreadPoolEnumeration:
 * This function uses the parameter segdbList as the subset definition.
 * It assigns each segdb in the subset to a thread in the thread pool.
 * This is based on each thread handling cdb_connections_per_thread connections.
 */
void createThreadPoolEnumeration( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
									const char *szCommand, bool cancelFromBelow,
									SegmentDatabaseDescriptor **segdbList, int segdbListCount)
{
	int x;
	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *q;

	for ( x=0; x<segdbListCount; x++)
	{
		
		segdbDesc = segdbList[x];
		Assert( segdbDesc != NULL );
		
		if ( segdbDesc->conn == NULL )
		{
			q = segdbDesc->segment_database_info;
			Assert( q != NULL );

			/* This is an error condition	*/
			ereport( ERROR,
						(errcode( ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION ),
						errmsg("Cannot dispatch command %s to segindex %d isprimary %c, because "
					 			"there is no connection.",
					 			szCommand, q->segindex, (q->isprimary ? 't' : 'f')
					)));

		}

		addSegDBToDispatchThreadPool( dispatchCommandParmsAr, psegdbs_in_thread_pool,
									szCommand, cancelFromBelow, segdbDesc );
	}
}

/*
 * listenerSegDBPairCompare:
 * A compare function for ListenerSegDBPair structs
 * that compares based on listenerid,
 * for use with qsort.
 */
/*static int listenerSegDBPairCompare(const void *p1, const void *p2)
{
	const ListenerSegDBPair *obj1 = (ListenerSegDBPair *)p1;
	const ListenerSegDBPair *obj2 = (ListenerSegDBPair *)p2;
	
	return (obj1->listenerid - obj2->listenerid);
}*/

/*
 * addSegDBToDispatchThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to have commands dispatched to.
 * It figures out which thread will handle it, based on the setting of
 * cdb_connections_per_thread.
 */
void addSegDBToDispatchThreadPool( DispatchCommandParms *ParmsAr, int *segdbCount, const char *szCommand, bool cancelFromBelow,  SegmentDatabaseDescriptor *segdbDesc)
{
	DispatchCommandParms *pParms;
	int	ParmsIndex;
	
	Assert( segdbCount != NULL );
	
	/*
	 * The proper index into the DispatchCommandParms array is computed, based on
	 * having cdb_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % cdb_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	ParmsIndex = (*segdbCount) / cdb_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];
	
	if ( (*segdbCount) % cdb_connections_per_thread == 0 )
	{
		pParms->szCommand = pstrdup(szCommand);
		pParms->segdbDescPtrArray = (SegmentDatabaseDescriptor **)palloc0( cdb_connections_per_thread * sizeof(SegmentDatabaseDescriptor *) );
		pParms->cancelFromBelow = cancelFromBelow;
		pParms->thread = (pthread_t)0;
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
CdbDispatchResults *initCdbDispatchResults(void)
{
	int							i;
	SegmentDatabaseDescriptor	*segdbDesc;
	CdbComponentDatabaseInfo	*q;
	CdbDispatchResults			*results = (CdbDispatchResults *)palloc0( sizeof(CdbDispatchResults) );
	DispatchCommandParms		*pParms;

	results->success = true;
	results->count = 0;
	results->pResults = NULL;
	results->szCommand = NULL;

	for ( i=0; i<threadCount; i++ )
	{
		pParms = &dispatchCommandParmsAr[i];
		results->count += pParms->db_count;

		if ( i == 0 && pParms->szCommand != NULL )
			results->szCommand = pstrdup( pParms->szCommand );
	}

	if ( results->count > 0 )
	{
		int x = 0;
		results->pResults = (CdbDispatchResult *)palloc0( results->count * sizeof(CdbDispatchResult) );
		
		
		for ( i=0; i<threadCount; i++ )
		{
			int j;
			pParms = &dispatchCommandParmsAr[i];

			
			for ( j=0; j<pParms->db_count; j++ )
			{
				CdbDispatchResult *result;
				
				segdbDesc = pParms->segdbDescPtrArray[j];
				Assert(	segdbDesc != NULL );
				q = segdbDesc->segment_database_info;
				Assert( q != NULL );

				result = &results->pResults[x++];

				result->segdbDesc		= segdbDesc;
				result->error_message	= NULL;
				result->cmdTuples		= 0;
				result->cmdStatus		= NULL;
				result->resultStatus	= PGRES_COMMAND_OK;
			}
		}
	}

	return results;
}

/*
 * DestroyCdbDispatchResults:
 * pfrees all memory allocated in CdbDispatchResults struct.
 */
void DestroyCdbDispatchResults(CdbDispatchResults *results)
{
	if ( results == NULL )
		return;
		
	elog(DEBUG4,"DestroyCdbDispatchResults" );	

	if ( results->count > 0 )
	{
		int i;

		for ( i=0; i<results->count; i++ )
		{
			CdbDispatchResult *result = &results->pResults[i];
		
			if ( result->error_message != NULL )
				pfree( result->error_message );

			if ( result->cmdStatus != NULL )
				pfree( result->cmdStatus );
		}

		pfree( results->pResults );
	}

	if ( results->szCommand != NULL )
		pfree( results->szCommand );

	pfree( results );

}

/*
 * SumCmdTuples:
 * Convenience routine for looping through and summing
 * the cmdTuples values for all elements of results->pResults.
 * Used to determine the row counts for insert, update, and delete commands.
 */
int SumCmdTuples(CdbDispatchResults *results)
{
	int i;
	int cmdTuples = 0;

	for (i=0; i<results->count; i++ )
	{
		CdbDispatchResult *result = &results->pResults[i];

		cmdTuples += result->cmdTuples;
	}

	return cmdTuples;
}

/*
 * CdbDoCommandInt:
 * Combination of CdbDispatchCommand and then CdbCheckDispatchResult.
 * If the command successfully ran, returns true, else error is thrown.
 */
bool CdbDoCommandInt( const char *szCommand, DispatchTargetSet targetSet,
					bool cancelFromBelow, void ***resultSets )
{
	CdbDispatchResults* results = NULL;
	bool				rtn		= true;
	char				*message_do;


	Assert(szCommand != NULL);

	CdbDispatchCommandInt( szCommand, targetSet, NULL, 0, cancelFromBelow, resultSets );

	results = CdbCheckDispatchResult();

	message_do = CdbDumpDispatchResults(results, true);
	Assert( message_do != NULL );
	elog( DEBUG5,
			"cdbexec command results for \"%s\": %s",
			szCommand, message_do );

	pfree(message_do);

	if ( !results->success )
	{
		message_do = CdbDumpDispatchResults(results, false);
		rtn = false;

		ereport( ERROR,
					(errcode( ERRCODE_CDB_COMMAND_ERROR ),
					errmsg("Command \"%s\" failed on at least one segment database: %s, ",
				 	szCommand, message_do
				)));

		pfree( message_do );
	}

	DestroyCdbDispatchResults( results );

	return rtn;
}

/*
 * CdbDoCommand:
 * Combination of CdbDispatchCommand and then CdbCheckDispatchResult.
 * If the command successfully ran, returns true, else error is thrown.
 * Called by general users, this method includes global transaction control.
 */
bool CdbDoCommand( const char *szCommand,
                    DispatchTargetSet targetSet,
                    bool cancelFromBelow, void ***resultSets )
{
	bool ret;
    /* need to start distributed transaction */
    condStartDtxTransaction();
    ret = CdbDoCommandInt( szCommand, targetSet,
                            cancelFromBelow, resultSets );
    return ret;
}


/*
 * CdbDumpDispatchResults:
 * Formats a string with results info.
 * If verbose = true, reports on all target databases.
 * If false, reports on only failures.
 */
char *CdbDumpDispatchResults(CdbDispatchResults *results, bool verbose)
{
	StringInfoData buffer;
	int i;

	Assert( results != NULL );

	if ( !verbose && results->success )
		return NULL;

	initStringInfo( &buffer );

	for ( i=0; i<results->count; i++ )
	{
		CdbDispatchResult *result = &results->pResults[i];
		CdbDumpDispatchResult( result, verbose, &buffer );
	}

	return buffer.data;
}

void CdbDumpDispatchResult( CdbDispatchResult *result, bool verbose, StringInfo message )
{
	CdbComponentDatabaseInfo *q;

	Assert( result != NULL );
	Assert( result->segdbDesc != NULL );
	Assert( message != NULL );

	q = result->segdbDesc->segment_database_info;
	Assert( q != NULL );

	if ( !verbose )
	{
		if ( result->error_message == NULL )
			return;

		appendStringInfo( message, "segindex %d isprimary %c : %s\n",
							q->segindex, (q->isprimary ? 't' : 'f'),
							result->error_message );
	}
	else
	{
		/*
		 * In verbose mode, dump out all fields
		 */
		char *error_message = result->error_message;
		char *cmdStatus		= result->cmdStatus;

		if ( error_message == NULL )
			error_message = "NULL";

		if ( cmdStatus == NULL )
			cmdStatus = "NULL";

		appendStringInfo( message, "segindex %d isprimary %c \n"
						"error message: %s "
						"cmdTuples : %d "
						"cmdStatus : %s "
						"resultStatus : %d\n",
						q->segindex, (q->isprimary ? 't' : 'f'),
						error_message,
						result->cmdTuples,
						cmdStatus,
						result->resultStatus);
	}
}


/*
 * setCancelFlag:
 * This utility function is called to signal to postgres that
 * a cancel is requested, similar to what happens when a ctrl-C
 * is pressed (StatementCancelHandler)
 */
void setCancelFlag(void)
{
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}
}



/*
 * Compose and dispatch the CDBEXEC commands corresponding to a plan tree
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
void CdbDispatchPlan(QueryDesc *queryDesc)
{
	StringInfoData strinfo;
	char *squery, *splan, *ssliceinfo;
	char *cdbexecPrefix;
	SliceTable *sliceTbl;
	int rootIdx;
	int oldLocalSlice;
	int *sliceVector, nSlices, sliceLim;
	int i;
	
	
	Assert( Cdb_role == CDB_ROLE_DISPATCH );	
	Assert( queryDesc != NULL && queryDesc->estate != NULL);
	
	elog(DEBUG4,"CdbDispatchPlan" );	
	
	 /* need to start distributed transaction */
    condStartDtxTransaction();
	
	/*
	 * Later we'll need to operate with the slice table provided via the
	 * EState structure in the argument QueryDesc.  Cache this information
	 * locally and assert our expectations about it.
	 */
	sliceTbl = queryDesc->estate->es_sliceTable;
	rootIdx = queryDesc->estate->es_cur_root_idx;
	
	Assert( sliceTbl!=NULL );
	Assert( rootIdx==0 || ( rootIdx>sliceTbl->nMotions && rootIdx<sliceTbl->nextInitPlan ) );
	
	/*
	 * Keep old value so we can restore it.  We use this field as a parameter.
	 */
	oldLocalSlice = sliceTbl->localSlice;

	/*
	 * Serialize plan and query trees. Build the common prefix of the
	 * CDBEXEC commands we'll issue.  Note that we're called for a single
	 * slice tree (corresponding to an initPlan or the main plan), so the
	 * parameters are fixed and we can include them in the prefix.
	 */
	squery = serializeNode((Node *)queryDesc->parsetree);
	splan = serializeNode((Node *)queryDesc->plantree);
	
	Assert(squery != NULL && splan != NULL);
	
	initStringInfo( &strinfo );
	appendStringInfo(&strinfo, "cdbexec '%s' '%s'", squery, splan);
	
	pfree(squery);
	pfree(splan);
	
	/*
	 * Next, continue building the CDBEXEC command by adding all the parameters
	 * serialized into RemoteParamData structs, which is just a node wrapper
	 * around ParamListInfo.
	 *
	 * Add parameters to the cdbexec command by enclosing the whole set with
	 * ()  and separating individual ones with a comma, so it looks like:
	 * ('Param1', 'Param2', ...) at the end of the cdbexec command.
	 */
	if ( queryDesc->params != NULL )
	{
		bool bFirstTime = true;
		ParamListInfo q = queryDesc->params;
		appendStringInfo(&strinfo, " (");
		while ( q->kind != PARAM_INVALID )
		{
			char *sparms;
			RemoteParamData *rpd;
			
			rpd = makeNode(RemoteParamData);
			memcpy( &rpd->paramData, q, sizeof(ParamListInfoData) );
			sparms = serializeNode((Node *)rpd);
			if ( !bFirstTime )
				appendStringInfo(&strinfo, ", ");
			else
				bFirstTime = false;

			appendStringInfo(&strinfo, " '%s'", sparms);

			pfree(sparms);
			pfree( rpd );
			q++;
		}
		appendStringInfo(&strinfo, ")");
	}
	
	/*
	 * Hang on to the command so far.  It will serve as the prefix for each
	 * of the CDBEXEC commands we dispatch.  Clear the buffer for use in
	 * constructing the final CDBEXEC command(s).
	 */
	cdbexecPrefix = strinfo.data;
	initStringInfo( &strinfo );

		
	/*
	 * Traverse the slice tree in sliceTbl rooted at rootIdx and build a
	 * vector of slice indexes specifying the order of [potential] dispatch.
	 */
	sliceLim = list_length(sliceTbl->slices);
	sliceVector = palloc(sliceLim * sizeof(int));
	nSlices = fillSliceVector(sliceTbl, rootIdx, sliceVector, sliceLim, 0);

	
	/*
	 * Now we need to call CDBDispatchCommand once per slice.  Each such
	 * call dispatches a CDBEXEC command to each of the QEs assigned to
	 * the slice.
	 *
	 * The QE information goes in two places: (1) in the argument to the
	 * function CDBDispatchCommand, and (2) in the serialized CDBEXEC
	 * command sent to the QEs.
	 *
	 * So, for each slice in the tree...
	 */
	for ( i = 0; i < nSlices; i++ )
	{
		SegmentDatabaseDescriptor ** sdp;
		
		Slice *slice = (Slice *)list_nth(sliceTbl->slices, sliceVector[i]);
		int    k;
		
		Gang * primaryGang = slice->primaryGang;
		
		
		 /* Is this a slice we should dispatch?
		  *
		  */
		if ( slice->gangType == GANGTYPE_UNALLOCATED )
		{
			/* Most slices are dispatched, however, in many  cases the
			 * root runs only on the QD and is not dispatched to the QEs.
			 */
			continue;
		}
		
		Assert(primaryGang != NULL);   /* Must have a gang to dispatch to */
		
	
		/*
		 * Set up the slice info parameter for this slice and use it to
		 * construct a finished CDBEXEC command.
		 */				
		sliceTbl->localSlice = slice->sliceIndex;
		ssliceinfo = serializeNode((Node *)sliceTbl);
		appendStringInfo(&strinfo, "%s '%s'", cdbexecPrefix, ssliceinfo);
		pfree(ssliceinfo);
		
		
		/*
		 * TODO - SLICE PLAN INTEGRATION
		 *
		 * Need to know how to specify target set: from CDBProcess
		 * list, a group identifier, or how.
		 *
		 * The information In the argument to CDBDispatchCommand must specify
		 * the same QEs as are carried in the serialized CDBEXEC command.
		 * In addition, we need (in case of writers) specify two QE lists
		 * (primary and mirror).  This could be via extended arguments to
		 * or, perhaps, by making two calls, to CDBDispatchCommand.
		 *
		 */


					
					
		sdp = palloc(primaryGang->segdb_count * sizeof(SegmentDatabaseDescriptor*));
		for (k = 0; k < primaryGang->segdb_count; k++)
		{
			sdp[k] = &primaryGang->db_descriptors[k];
		}
		
		
	
		/*
		 * This call returns after launching the threads that send the command to the appropriate
		 * segdbs.  It does not wait for them to finish.
		 */
		

		CdbDispatchCommandInt(strinfo.data, DTS_ENUMERATION, sdp, primaryGang->segdb_count, true, NULL );

		
		pfree(strinfo.data);
		initStringInfo( &strinfo );
	}
	elog(DEBUG4,"CdbDispatchPlan done" );	
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
 *	IndexVector	- index vector under contruction
 *	len			- number of positions allocated in the index vector
 *	next		- position at which to write the next index
 *
 * Returns new value of count.
 */
int fillSliceVector(SliceTable *sliceTable, int sliceIndex, int *indexVector, int len, int next)
{
	int childIndex;
	ListCell *sublist;
	Slice *slice = (Slice*)list_nth(sliceTable->slices, sliceIndex);
	
	foreach( sublist, slice->children )
	{
		childIndex = lfirst_int(sublist);
		next = fillSliceVector(sliceTable, childIndex, indexVector, len, next);
	}
	
	if ( next >= len )
	{
		ereport( ERROR,
			(errcode ( ERRCODE_CDB_INTERNAL_ERROR ),
			errmsg("bad slice vector allocation")
		));
	}
	
	indexVector[next++] = sliceIndex;
	return next;
}
