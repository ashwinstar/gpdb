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
static int listenerSegDBPairCompare(const void *p1, const void *p2);
static void createThreadPoolPerListener( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow);
static void createThreadPoolPerSegment( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow);
static void createThreadPoolPerSegDB( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow);
static void createThreadPoolEnumeration( DispatchCommandParms *dispatchCommandParmsAr, int *psegdbs_in_thread_pool,
										 const char *szCommand, bool cancelFromBelow, 
										 SegmentDatabaseDescriptor **segdbList, int segdbListCount);
static void addSegDBToDispatchThreadPool( DispatchCommandParms *ParmsAr, int *segdbCount, const char *szCommand, 
										bool cancelFromBelow, SegmentDatabaseDescriptor *segdbDesc);
static CdbDispatchResults *initCdbDispatchResults(void);
static void CdbDumpDispatchResult( CdbDispatchResult *result, bool verbose, StringInfo message );
static void setCancelFlag(void);

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
 * CdbDispatchCommand:
 * Send ths szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the targetSet parameter.  cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.  They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable cdb_connections_per_thread.
 */
void CdbDispatchCommand( const char *szCommand, DispatchTargetSet targetSet, 
						SegmentDatabaseDescriptor **segdbList, int segdbListCount, 
						bool cancelFromBelow )
{
	int i;
	int max_threads;
	int segdbs_in_thread_pool;
	
	if ( Cdb_role != CDB_ROLE_DISPATCH )
		return;

	Assert( segdbList == NULL || targetSet == DTS_ENUMERATION ); 
	Assert( segdbListCount == 0 || targetSet == DTS_ENUMERATION );
	Assert( segdbList == NULL || segdbListCount > 0 );
	
	int segdb_count = CdbClusterDefinition->segdb_count;
	
	/*
	 * The most threads we could have is segdb_count / cdb_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / cdb_connections_per_thread.
	 * We allocate enough memory for this many DispoatchCommandParms structures,
	 * even though we may not use them all.
	 */
	Assert( cdb_connections_per_thread > 0 );
	Assert( segdb_count > 0 );
	
	max_threads =  1 + (segdb_count-1) / cdb_connections_per_thread;
	dispatchCommandParmsAr = (DispatchCommandParms *)
							palloc0( max_threads * sizeof(DispatchCommandParms) );
	segdbs_in_thread_pool = 0;
	
	/*
	 * Create the thread parms structures based targetSet parameter.
	 * This will add the segdbDesc pointers appropriate to the 
	 * targetSet into the thread Parms structures, amking sure that each thread
	 * handles cdb_connections_per_thread segdbs.
	 */
	switch ( targetSet )
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

		case DTS_ENUMERATION:
			createThreadPoolEnumeration( dispatchCommandParmsAr, &segdbs_in_thread_pool,
										 szCommand, cancelFromBelow, segdbList, segdbListCount);
			break;

	}

	/*
	 * Compute the thread count based on how many segdbs were added into the 
	 * thread pool, knowing that each thread handles cdb_connections_per_thread
	 * segdbs.
	 */						
	if ( segdbs_in_thread_pool == 0 )
		threadCount = 0;
	else	
		threadCount = 1 + (segdbs_in_thread_pool - 1) / cdb_connections_per_thread;
		
	/*
	 * Create the threads.
	 */						
	for ( i=0; i<threadCount; i++ )
	{
		DispatchCommandParms* pParms = &dispatchCommandParmsAr[i];

		elog(DEBUG2, "Creating thread %d of %d for CommandDispatch", i+1, threadCount);

		pthread_create(&pParms->thread, 
						NULL, 
						thread_DispatchCommand, 
						pParms);
	}
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
	SegmentDatabaseDescriptor	*segdbDesc;
	DispatchCommandParms		*pParms;
	CdbDispatchResults			*results = initCdbDispatchResults();

	Assert( results != NULL );

	if ( threadCount == 0 )
		return results;

	/*
	 * Wait for threads to finish.
	 */						
	for ( i=0; i<threadCount; i++ )		
	{
		pParms = &dispatchCommandParmsAr[i];
		
		elog(DEBUG2, "Joining to thread %d of %d for CommandDispatch", i+1, threadCount);
		
		pthread_join( pParms->thread, NULL );
	}
	
	/*
	 * Check whether there were error messages.
	 * This is stored in the error_message field of the
	 * segment_database_descriptor.  
	 */			

	int x = 0;

	for ( i=0; i<threadCount; i++ )
	{
		pParms = &dispatchCommandParmsAr[i];
		
		for ( j=0; j<pParms->db_count; j++ )
		{
			segdbDesc = pParms->segdbDescPtrArray[j];
			Assert(	segdbDesc != NULL );

			CdbDispatchResult *result = &results->pResults[x++];
			
			if ( segdbDesc->error_message != NULL )
			{
				results->success = false;
				
	 		/* Because we cannot use palloc inside a thread, the error messages
	 		 * were malloc'd. So we must free the malloc'd memory after
	 		 * appending them to the StringInfoData.
	 		 * We must zero out the segdbDesc->error_message too.
			 */
				char *pszErrMsg = segdbDesc->error_message;
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
			Assert( pParms->szCommand != NULL );
			pfree(  pParms->szCommand );
		}
		pfree( dispatchCommandParmsAr );
		dispatchCommandParmsAr = NULL;
	}
    	
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

	/* 
	 * In threads, we cannot use palloc, because it's not thread safe.  
	 */
	stillRunning = (RunningInfo *)calloc(db_count, sizeof(RunningInfo));
					
	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to send commands through to.
	 */
	for ( i=0; i<db_count; i++ )
	{
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
			
			appendPQExpBuffer(&buffer, "Cannot issue command to segment database"
										"segindex %d isprimary %c, because there is no connection", 
										q->segindex, (q->isprimary ? 't' : 'f')),
									
			segdbDesc->error_message = buffer.data;
			if ( cancelFromBelow )
				setCancelFlag();
			continue;
		}
		
		/*
		 * Submit the command asynchronously.
		 */
		if ( 0 == PQsendQuery(conn, szCommand) )
		{
			PQExpBufferData buffer;
			initPQExpBuffer(&buffer);
			
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
		struct timeval tv;
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		fd_set input_mask;
		FD_ZERO( &input_mask );
		int max_sock = 0;
		
		for ( i=0; i<stillRunningCount; i++ )
		{
			conn = stillRunning[i].segdbDesc->conn;
			int sock = PQsocket( conn );
			FD_SET( sock, &input_mask );
			max_sock = max_sock < sock ? sock : max_sock;
		}
	
		int n = select( max_sock+1, &input_mask, NULL, NULL, &tv );
	
		if ( n < 0 )
		{
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
			PQExpBufferData buffer;
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
		int x = 0;
		for ( i=0; i<stillRunningCount; i++ )
		{
			segdbDesc = stillRunning[i].segdbDesc;
			conn = segdbDesc->conn;
			if ( PQstatus( conn ) == CONNECTION_BAD )
			{
				q = segdbDesc->segment_database_info;
				Assert( q != NULL );
				
				PQExpBufferData buffer;
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
				segdbDesc = stillRunning[i].segdbDesc;
				conn = segdbDesc->conn;
				q = segdbDesc->segment_database_info;
				Assert( q != NULL );

				int sock = PQsocket( conn );
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
							char *cmdStatus = PQcmdStatus(pRes);
							if ( cmdStatus != NULL )
								segdbDesc->cmdStatus = strdup(cmdStatus);

							char *cmdTuples = PQcmdTuples(pRes);
							if ( cmdTuples != NULL )
								segdbDesc->cmdTuples = atoi(cmdTuples);
						}
						
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
	
	free(stillRunning);
	
	return(NULL);
}

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
	
	pPairAr = (ListenerSegDBPair *)palloc0( sizeof(ListenerSegDBPair) * segdb_count );
	
	for ( i=0; i<CdbClusterDefinition->segment_count; i++ )
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
	int lastListenerID = -1;
	bool bFoundConnection = true;
	for ( i=0; i<PairArCount; i++ )
	{
		if ( pPairAr[i].listenerid != lastListenerID  )
		{
			// Check to see if we got a connection for the last listenerid
			if ( !bFoundConnection )
			{
				// This is an error condition	
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
	
	// Check to see if we got a connection for the last listenerid
	if ( !bFoundConnection )
	{
		// This is an error condition	
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
	
	for ( i=0; i<CdbClusterDefinition->segment_count; i++ )
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
	
	// Validate that we have connections that we need
	if ( !CdbClusterDefinition->all_valid_segdbs_connected )
	{
		ereport( ERROR,
					(errcode( ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION ),
					 errmsg("Cannot dispatch command %s to all segdbs: "
					 		"there is no connection to at least one valid segdb",
					 		szCommand
				)));
	}
	
	for ( i=0; i<CdbClusterDefinition->segment_count; i++ )
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
		Assert( segdbDesc != NULL );
		segdbDesc = segdbList[x];
		if ( segdbDesc->conn == NULL )
		{
			q = segdbDesc->segment_database_info;
			Assert( q != NULL );

			// This is an error condition	
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
static int listenerSegDBPairCompare(const void *p1, const void *p2)
{
	const ListenerSegDBPair *obj1 = (ListenerSegDBPair *)p1;
	const ListenerSegDBPair *obj2 = (ListenerSegDBPair *)p2;
	
	return (obj1->listenerid - obj2->listenerid);
}

/*
 * addSegDBToDispatchThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to have commands dispatched to.
 * It figures out which thread will handle it, based on the setting of
 * cdb_connections_per_thread.
 */
void addSegDBToDispatchThreadPool( DispatchCommandParms *ParmsAr, int *segdbCount, const char *szCommand, bool cancelFromBelow,  SegmentDatabaseDescriptor *segdbDesc)
{
	DispatchCommandParms *pParms;
	
	Assert( segdbCount != NULL );
	
	/*
	 * The proper index into the DispatchCommandParms array is computed, based on 
	 * having cdb_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % cdb_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	int	ParmsIndex = (*segdbCount) / cdb_connections_per_thread;
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
		results->pResults = (CdbDispatchResult *)palloc0( results->count * sizeof(CdbDispatchResult) );
		
		int x = 0;
		for ( i=0; i<threadCount; i++ )
		{
			pParms = &dispatchCommandParmsAr[i];

			int j;
			for ( j=0; j<pParms->db_count; j++ )
			{
				segdbDesc = pParms->segdbDescPtrArray[j];
				Assert(	segdbDesc != NULL );
				q = segdbDesc->segment_database_info;
				Assert( q != NULL );

				CdbDispatchResult *result = &results->pResults[x++];

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
 * CdbDoCommand:
 * Combination of CdbDispatchCommand and then CdbCheckDispatchResult.
 * If it fails on any of the targets, and the undo command is non-NULL,
 * then the undo command is run on any targets that were successful.
 * If the command successfully ran, returns true.
 * If it failed on at least one target, and the undo was NULL or successful, then returns false.
 * If it failed but the undo failed, ereport is called.
 */
bool CdbDoCommand( const char *szCommand, const char *szUndoCommand, 
					DispatchTargetSet targetSet, 
					bool cancelFromBelow )
{
	CdbDispatchResults* results = NULL;
	bool				rtn		= true;
	char				*message_do;
	char				*message_undo;

	Assert(szCommand != NULL);

	CdbDispatchCommand( szCommand, targetSet, NULL, 0, cancelFromBelow );

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

		if ( szUndoCommand != NULL )
		{
			int i;
			int x = 0;
			SegmentDatabaseDescriptor	**segdbsForUndo;
			CdbDispatchResults* undo_results = NULL;


			segdbsForUndo = (SegmentDatabaseDescriptor **)palloc0( results->count * sizeof(SegmentDatabaseDescriptor *) );
			for( i=0; i<results->count; i++ )
			{
				CdbDispatchResult *pResult = &results->pResults[i];
				if ( pResult->error_message == NULL )
				{
					segdbsForUndo[x++] = pResult->segdbDesc;
				}
			}

			CdbDispatchCommand( szUndoCommand, DTS_ENUMERATION, segdbsForUndo, x, cancelFromBelow );

			undo_results = CdbCheckDispatchResult();

			message_undo = CdbDumpDispatchResults(undo_results, true);
			Assert( message_undo != NULL );
			elog( DEBUG2,
					"cdbexec undo command results for \"%s\": %s",
					szUndoCommand, message_undo );

			pfree(message_undo);

			if ( !undo_results->success )
			{
				// This is an error condition	
				message_undo = CdbDumpDispatchResults(undo_results, false);
				ereport( ERROR,
							(errcode( ERRCODE_CDB_COMMAND_ERROR ),
							errmsg("Command \"%s\" failed on at least one segment database: %s, "
							"and then the undo command \"%s\" also failed on at least one segment database: %s",
					 		szCommand, message_do, szUndoCommand, message_undo
						)));
			}
			else
			{
				ereport( DEBUG5,
							(errcode( ERRCODE_CDB_COMMAND_ERROR ),
							errmsg("Command \"%s\" failed on at least one segment database: %s, "
							"and then the undo command \"%s\" executed successfully",
					 		szCommand, message_do, szUndoCommand
						)));

			}

			DestroyCdbDispatchResults( undo_results );
		}
		else
		{

			ereport( DEBUG5,
						(errcode( ERRCODE_CDB_COMMAND_ERROR ),
						errmsg("Command \"%s\" failed on at least one segment database: %s, "
						"and then the undo command \"%s\" executed successfully",
					 	szCommand, message_do, szUndoCommand
					)));

		}

		pfree( message_do );
	}

	DestroyCdbDispatchResults( results );

	return rtn;
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
