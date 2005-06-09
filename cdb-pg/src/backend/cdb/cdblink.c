/*-------------------------------------------------------------------------
 *
 * cdblink.c
 *
 *    Setting up libpq connections to qRxecs from Dispatcher
 *
 *
 * ATTRIBUTION
 *
 *     Concept and initial logic based on contrib/dblink by:
 *
 *     Joe Conway <mail@joeconway.com>
 *     And contributors:
 *     Darko Prenosil <Darko.Prenosil@finteh.hr>
 *     Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 *     Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *     ALL RIGHTS RESERVED;
 *
 *     Permission to use, copy, modify, and distribute this software and its
 *     documentation for any purpose, without fee, and without a written agreement
 *     is hereby granted, provided that the above copyright notice and this
 *     paragraph and the following two paragraphs appear in all copies.
 *
 *     IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 *     DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 *     LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 *     DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 *     THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 *     INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 *     AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 *     ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 *     PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 *
 */
#include "postgres.h"

#include <ctype.h>
#include <pthread.h>

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "cdb/cdbcat.h"
#include "cdb/cdbconn.h"
#include "cdb/cdblink.h"
#include "cdb/cdbvars.h"


/*
 * Points to the anchor block (CdbClusterDescriptor) of the cluster
 * definition and connection blocks.  The block is located in 
 * TopMemoryContext, is allocated at connection time (by cdblink_setup),
 * and persists for the life of the Postgres backend.
 */
CdbClusterDescriptor	*CdbClusterDefinition = NULL;

/*
 * Points to the result of getCdbComponentDatabases()
 */
static CdbComponentDatabases *cdb_component_dbs	= NULL;

/*
 *	buildCdbClusterDefinition reads the Cdb catalog tables and build a CdbComponentDatabases structure.
 *	It then converts this to a CdbClusterDefinition structure and initializes
 *	all the non-connection related fields.
 */
static CdbClusterDescriptor *buildCdbClusterDefinition(void);

/*
 *	thread_DoConnect is the thread proc used to perform the connection to one of the qExecs.
 */
static void *thread_DoConnect(void *arg);

/* general utility */
#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

/*
 * Parameter structure for the DoConnect threads
 */ 
typedef struct DoConnectParms
{
	/*
	 * db_count: The number of segdbs that this thread is responsible for 
	 * connecting to.
	 * Equals the count of segdbDescPtrArray below.
	 */ 
	int db_count;

	/*
	 * segdbDescPtrArray: Array of SegmentDatabaseDescriptor* 's that this thread is 
	 * responsible for connecting to. Has size equal to db_count.
	 */ 
	SegmentDatabaseDescriptor **segdbDescPtrArray;
	
	/*
	 * The pthread_t thread handle.
	 */ 
	pthread_t	thread;
} DoConnectParms;

/*
 * AddSegDBToConnThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to be connected to.
 * It figures out which thread will handle it, based on the setting of
 * cdb_connections_per_thread.
 */
static void addSegDBToConnThreadPool( DoConnectParms *ParmsAr, int *segdbCount, SegmentDatabaseDescriptor *segdbDesc);

/* 
 * Called by backend/utils/init/postinit.c to setup the connections to the
 * tails and store them for the duration of the session.  Currently, since each
 * user session has their own process, all of the information is stored in
 * static variables.
 *
 * Note -- this function is called with TopTransactionContext *before* 
 *      MessageContext and the other statement-oriented contexts are
 *      allocated.  We want to store the SegmentDbArray and the
 *      heap returned by getCdbSegmentInstances() in TopMemoryContext
 *      (basically permanent storage).  TopTransactionContext is 
 *      reset shortly after this routine is called.
 */
 
void
cdblink_setup( void )
{
	/* If cdb_role is UTILITY, skip this call. 
	 * If cdb_role is EXECUTE, we need to call buildCdbClusterDefinition()
	 * so we know who we are and what the cluster specification is.
	 * But then we leave, as no connections are required in EXECUTE role.
	 */
	 
	if ( Cdb_role == CDB_ROLE_UTILITY )
		return;

	getCdbCdb();
		
	if ( Cdb_role == CDB_ROLE_EXECUTE )
		return;

	int total_segment_databases = 0;
    int successful_connections = 0;

	/*
	 * MemoryContexts used in this routine.
	 */
	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;
	
	elog(DEBUG1, "CDBLINK_SETUP---" );	
	
	/*
	 * buildCdbClusterDefinition() calls getCdbComponentDatabases()
	 * to read the Cdb catalog tables and build a CdbComponentDatabases structure.
	 * It then converts this to a CdbClusterDefinition structure and initializes
	 * all the non-connection related fields.
	 */
	CdbClusterDefinition = buildCdbClusterDefinition();
	Assert( CdbClusterDefinition != NULL );

	/*
	 * Establish a call-back to disconnect any connections we
	 * create here.  We do it at this point to make sure we
	 * disconnect any connections established up to the point
	 * of a failed one that takes us down.
	 */
	on_proc_exit( cdblink_disconnect, (Datum) NULL );
	
	/*
	 * Loop through the segment_descriptors items inside CdbClusterDefinition.  
	 * For each segdb in db_descriptors in segment_descriptor, attempt to get 
	 * a connection to the Postgres instance for that database.  
	 * If the connection fails, mark it (locally) as unavailable
	 * If no database serving a segindex is available, fail the user's connection request.
	 */
	SegmentDescriptor					*segDesc;
	SegmentDatabaseDescriptor			*segdbDesc;
	CdbComponentDatabaseInfo			*q;
	int									i;
	int									j; 
	DoConnectParms						*doConnectParmsAr = NULL;
	int									threadCount = 0;
	int									max_threads;
	int									segdbs_in_thread_pool;
	int									segdb_count = CdbClusterDefinition->segdb_count;
	
	Assert( cdb_connections_per_thread > 0 );
	Assert( segdb_count > 0 );
	
	/*
	 * The most threads we could have is segdb_count / cdb_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / cdb_connections_per_thread.
	 * We allocate enough memory for this many DoConnectParms structures,
	 * even though we may not use them all.
	 */
	max_threads =  1 + (segdb_count-1) / cdb_connections_per_thread;
	doConnectParmsAr = (DoConnectParms *)
						palloc0( max_threads * sizeof(DoConnectParms) );
	segdbs_in_thread_pool = 0;
	
	/*
	 * In this loop, we check each segdb.  If it's marked as valid, and is not
	 * ourself, we prepare a thread Parm struct for connecting to it.
	 * If a segment has no valid segdbs, we recognize that here and give
	 * an error message.
	 */
	for ( i=0; i<CdbClusterDefinition->segment_count; i++ )
	{
		segDesc = &CdbClusterDefinition->segment_descriptors[i];
			
		/*
		 * Create the connection requests.  If we find a segment without a valid segdb
		 * we error out.  Also, if this segdb is invalid, we msut fail the connection.
		 */
		for ( j=0; j<segDesc->db_count; j++ )
		{
			total_segment_databases++;
				
			segdbDesc = &segDesc->db_descriptors[j];
			q = segdbDesc->segment_database_info;
					
			/*
			* This function handles putting the segdbDesc into the proper thread's Parms
			* structure, based on cdb_connections_per_thread.
			* It does this based on the value of segdbs_in_thread_pool,
			* which it increments each time it is called.
			*/				
			addSegDBToConnThreadPool( doConnectParmsAr, &segdbs_in_thread_pool, segdbDesc);	
		}
	}
	
	/*
	 * Spin off the threads we've prepared.
	 */
	if ( segdbs_in_thread_pool == 0 )
		threadCount = 0;
	else	
		threadCount = 1 + (segdbs_in_thread_pool - 1) / cdb_connections_per_thread;
		
	for ( i=0; i<threadCount; i++ )
	{
		DoConnectParms* pParms = &doConnectParmsAr[i];

		elog(DEBUG2, "Creating thread %d of %d for libpq connections", i+1, threadCount);
			
		pthread_create( &pParms->thread,
						NULL, 
						thread_DoConnect, 
						pParms );
	}
	
    /* wait for all of the DoConnect threads to complete. */
	for ( i=0; i<threadCount; i++ ) 
	{
		DoConnectParms* pParms = &doConnectParmsAr[i];
		
		elog(DEBUG2, "Joining to thread %d of %d for libpq connections", i+1, threadCount);
			
		pthread_join(pParms->thread, NULL);
	}
	
	/*
	 * Free the memory allocated for the threadParms array
	 */
	if (  doConnectParmsAr != NULL )
	{
		for ( i=0; i<threadCount; i++ ) 
		{
			DoConnectParms* pParms = &doConnectParmsAr[i];
			pfree( pParms->segdbDescPtrArray );
		}
		pfree( doConnectParmsAr );
		doConnectParmsAr = NULL;
	}
	
	/*
	 * For now, set all_valid_segdbs_connected to true.
	 * Reset to false if we get fail to connect to a valid segdb
	 */
	CdbClusterDefinition->all_valid_segdbs_connected = true;
	
	/*
	 * In this loop, we check whether the connections were successful.
	 * If not, we recreate the error message with palloc and show it as
	 * a warning.  
	 */
	for ( i=0; i<CdbClusterDefinition->segment_count; i++ )
	{
		segDesc = &CdbClusterDefinition->segment_descriptors[i];

		for ( j=0; j<segDesc->db_count; j++ )
		{
			segdbDesc = &segDesc->db_descriptors[j];
			q = segdbDesc->segment_database_info;
			if ( segdbDesc->conn == NULL )
			{
				/*
					* The error message in segdbDesc->error_message
					* was allocated with malloc, since it was done in a thread,
					* and we cannot use palloc in a thread, because it is not thread safe.
					* So we need to make a palloc copy of it here, and free the original.
					*/
				char* pszErrorMessage = segdbDesc->error_message;
				Assert( pszErrorMessage != NULL );
				oldContext = MemoryContextSwitchTo(permanentContext);
				segdbDesc->error_message = pstrdup(pszErrorMessage);
				MemoryContextSwitchTo(oldContext);
				free( pszErrorMessage );

				/*
					* Set a warning for failed connections.  Complete failures
					* are taken care of later.
					*/
				ereport(WARNING,
						(errcode(ERRCODE_WARNING),
							errmsg("CDBLINK_SETUP--- Could not establish connection with segment database "
								"segindex %d isprimary %c", 
								q->segindex, (q->isprimary ? 't' : 'f')),
							errdetail("%s", segdbDesc->error_message)));
							
				CdbClusterDefinition->all_valid_segdbs_connected = false;
			}
			else
			{
				/*
					* We have a live connection!
					*/
				successful_connections++;
				segDesc->connected_count++;

				/*
					* Select the first connected segment database as the one to use for queries.
					* This could be changed for load balancing purposes.
					*/
				if ( !segDesc->query_db )
					segDesc->query_db = segdbDesc;
			}
		}
	}
		
	/*
	 * If no segdb for a segment got a successful 
	 * connection, we error out.  We had to do this as a separate
	 * loop from the preceding one yo make sure all error messages were
	 * palloced before potentially calling ereport.
	 */
	for ( i=0; i<CdbClusterDefinition->segment_count; i++ )
	{
		segDesc = &CdbClusterDefinition->segment_descriptors[i];
		
		if (segDesc->connected_count == 0)
		{
			StringInfoData buffer;

			/* 
			 * Initialize the StringInfo object using our "desired" memory
			 * context.  Future expansions of this StringInfo will occur in
			 * the "desired" context.
			 */
			oldContext = MemoryContextSwitchTo(permanentContext);
			initStringInfo(&buffer);
			MemoryContextSwitchTo(oldContext);

			appendStringInfo(&buffer, "CDBLINK_SETUP--- Could not establish connection with any" 
					                  " database for segindex %d", 
					                  segDesc->segindex);

			ereport(ERROR,
					(errcode(ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION),
					 errmsg(buffer.data),
					 errdetail("see previous warnings or server log")));

			segDesc->error_message = buffer.data;
		}
	}
					

    elog(DEBUG1, "CDB segment databases: defined=%d, connected=%d", 
			total_segment_databases, successful_connections);
    elog(DEBUG1, "CDB segments: defined = %d", CdbClusterDefinition->segment_count);
    
    return;
}



/* 
 * Called by on_proc_exit processing (in /backend/storage/ipc/ipc.c) to drop 
 * the connections to the tails. 
 *
 */
void cdblink_disconnect( int exit_code, Datum arg )
{
	int		i;
	
	/*
	 * If we had no segments, we have no work to do
	 */
	if ( CdbClusterDefinition == 0 || CdbClusterDefinition->segment_count == 0 )
		return;
	
	/*
	 * Loop through the segment_descriptors array and, for each
	 * SegmentDescriptor, loop through the SegmentDatabaseDescriptor.
	 * For each SegmentDatabaseDescriptor:
	 *     1) discard the query results (if any),
	 *     2) disconnect the session, and
	 *     3) discard any connection error message.
	 */
	for ( i = 0; i < CdbClusterDefinition->segment_count; i++ )
	{
		SegmentDescriptor	*segDesc = &(CdbClusterDefinition->segment_descriptors[i]);

		if ( segDesc->db_descriptors != NULL )
		{
			int		j;
			for ( j = 0; j < segDesc->db_count; j++ )
			{
				SegmentDatabaseDescriptor *segdbDesc = &(segDesc->db_descriptors[j]);
				
				/* TODO: Perform cancellation of "still running" queries */

/*				if ( pairDesc->query_result )
				{
					elog(DEBUG3, "Clearing result block for segment %d (current status = %s)",
							pairDesc->segment_instance_info->segid,
							PQresStatus(PQresultStatus(pairDesc->query_result)));
					PQclear(pairDesc->query_result);
					pairDesc->query_result = NULL;
				}
*/
				if ( segdbDesc->conn )
				{
					PGTransactionStatusType status = PQtransactionStatus( segdbDesc->conn );
					elog(DEBUG2, "Finishing connection with segindex %d isprimary %c (current status = %d)",
								segdbDesc->segment_database_info->segindex, 
								(segdbDesc->segment_database_info->segindex ? 't' : 'f'),
								status);
					PQfinish( segdbDesc->conn );
					segdbDesc->conn = NULL;
				}

				if ( segdbDesc->error_message )
				{
					pfree(segdbDesc->error_message);
					segdbDesc->error_message = NULL;
				}

			}
		}

		segDesc->query_db = NULL;

		pfree(segDesc->db_descriptors);
		segDesc->db_descriptors = NULL;

		if ( segDesc->error_message )
		{
			pfree(segDesc->error_message);
			segDesc->error_message = NULL;
		}

		/* segDesc->sql resides in a memory context already discarded */
	}

    /*
     * Discard the segment array and the cluster descriptor
     */
    pfree(CdbClusterDefinition->segment_descriptors);
    CdbClusterDefinition->segment_descriptors = NULL;
	pfree(CdbClusterDefinition);
	CdbClusterDefinition = NULL;

    /*
     * Discard the segment/instance information
     */
    freeCdbComponentDatabases(cdb_component_dbs);
    cdb_component_dbs = NULL;

	freeCdbCdb();

    return;
}

/* TODO Get rid of this once Donnie takes out these calls from his code */
/* 
 * getCdbIdentity:
 * Returns my_segindex and my_isprimary in out parameters
 * for the postgres process.  If either pointer is NULL,
 * it skips returning that parameter.
 * 
 * N.B.  Cdb_role must be either dispatch or execute, since
 * when utiliy  no cdb catalog tables are read.  An Assert is
 * thrown if Cdb_role = utility.
 */
void getCdbIdentity(int *segindex, bool *isprimary)
{
	Assert( Cdb_role != CDB_ROLE_UTILITY );
		
	if ( segindex != NULL )
	{
		*segindex = CdbIdentity.segindex;
	}
			
	if ( isprimary != NULL )
	{
		*isprimary = true;
	}
}

/* 
 * Returns the number of segments
 * 
 * N.B.  Cdb_role must be either dispatch or execute, since
 * when utiliy  no cdb catalog tables are read.  An Assert is
 * thrown if Cdb_role = utility.
 */
int getCdbSegmentCount(void)
{
	Assert( Cdb_role != CDB_ROLE_UTILITY );

	return CdbIdentity.numsegments;
}

/* 
 * Returns an array of the segindexes, and fills in the segment count
 * in the out parameter.
 * 
 * N.B.  Cdb_role must be either dispatch or execute, since
 * when utiliy  no cdb catalog tables are read.  An Assert is
 * thrown if Cdb_role = utility.
 */
int *getCdbSegments(int *segcount)
{
	int *segs = NULL;
	int i;

	Assert( segcount != NULL );
	Assert( Cdb_role != CDB_ROLE_UTILITY );
	Assert( CdbClusterDefinition != NULL && 
			CdbClusterDefinition->segdb_count > 0 && 
			CdbClusterDefinition->segment_descriptors != NULL);

	segs = (int *)palloc0( CdbClusterDefinition->segdb_count * sizeof(int) );
	for ( i=0; i<CdbClusterDefinition->segdb_count; i++ )
	{
		segs[i] = CdbClusterDefinition->segment_descriptors[i].segindex;
	}

	*segcount = CdbClusterDefinition->segdb_count;
	return segs;
}

/*
 *	buildCdbClusterDefinition: reads the Cdb catalog tables and build a CdbComponentDatabases structure.
 *	It then converts this to a CdbClusterDefinition structure and initializes
 *	all the non-connection related fields.
 */
CdbClusterDescriptor *buildCdbClusterDefinition(void)
{
	CdbClusterDescriptor *ClusterDefn = NULL;
	
	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;
	
	/*
	 * Get the structure representing the configured segment databases.  
	 * This structure is fetched into a permanent memory context -- probably the TopMemoryContext.  We
	 * also allocate space for and partially construct the data structure
	 * pointed to by ClusterDefn and headed by CdbClusterDescriptor.
	 */
    oldContext = MemoryContextSwitchTo(permanentContext);

	ClusterDefn = (CdbClusterDescriptor *)palloc0( sizeof(CdbClusterDescriptor) );

	cdb_component_dbs = getCdbComponentDatabases();
	
	ClusterDefn->all_valid_segdbs_connected	= false;
	
	if ( cdb_component_dbs != NULL && cdb_component_dbs->total_segments != 0 )
	{
		ClusterDefn->segment_count	= cdb_component_dbs->total_segments;
		ClusterDefn->segdb_count	= cdb_component_dbs->total_segment_dbs;
		ClusterDefn->segment_descriptors = 
			(SegmentDescriptor *) palloc0 ( ClusterDefn->segment_count * sizeof( SegmentDescriptor ) );
	}
	else
    {
        ClusterDefn->segment_count = 0;
		ClusterDefn->segment_descriptors = NULL;
    }

	MemoryContextSwitchTo(oldContext);

    if ( !ClusterDefn->segment_descriptors )
    {
        elog(NOTICE, "No CDB segment/instance pairs configured");
        return ClusterDefn;
    }

	Assert( cdb_component_dbs->total_segment_dbs > 0 );
		
	SegmentDescriptor				    *segDesc;
	SegmentDatabaseDescriptor		    *segdbDesc;
	CdbComponentDatabaseInfo			*p;
		
	/*
	 * Pass through each segment database in segment_dbs
	 * and Count up databases per segment_index.  Use this to initialize
	 * the fields in ClusterDefn->segment_descriptors
	 */
	int seg_count = 0; 
	int last_segindex;
	
	/*
	 * We loop through the segment_db_info.  Each item has a segindex.
	 * They are sorted by segindex, and there can be > 1 segment_db_info for
	 * a given segindex (currently, there can be 1 or 2)
	 * segDesc points to the SegmentDescriptor for the segindex
	 */
	segDesc = ClusterDefn->segment_descriptors - 1;
	int i;
		
	for ( i=0; i<cdb_component_dbs->total_segment_dbs; i++ )
	{
		p = &cdb_component_dbs->segment_db_info[i];
		if ( i == 0 || (p->segindex != last_segindex) )
		{
			/*
			 * We have a new segindex.  Advance segDesc, and set count to 1.
			 */
			segDesc++;
			Assert( segDesc - ClusterDefn->segment_descriptors < ClusterDefn->segment_count );
			last_segindex = p->segindex;
			segDesc->segindex = p->segindex;
			segDesc->db_count = 1;
			segDesc->error_message = NULL;
			segDesc->query_db = NULL;
			segDesc->sql = NULL;
			seg_count++;
		}
		else
		{
			/*
			 * We have another segdb for the same segindex.  Bump up the count.
			 * It should never go above 2, since we can only have 1 secondary
			 * segdb in the current design.
			 */
			segDesc->db_count++;
			Assert( segDesc->db_count <= 2 );
		}
	}
		
	Assert( seg_count == ClusterDefn->segment_count );
		
	/*
	 * Loop through each segment descriptor in ClusterDefn->segment_descriptors
	 * to allocate and initialize the db_descriptors array.
	 */
	p = &cdb_component_dbs->segment_db_info[0];

	for ( i=0; i<seg_count; i++ )
	{
		segDesc = &ClusterDefn->segment_descriptors[i];
		
		Assert(	segDesc->db_count > 0 && segDesc->db_count <= 2 );
		
	    oldContext = MemoryContextSwitchTo(permanentContext);

		segDesc->db_descriptors = (SegmentDatabaseDescriptor *)
						palloc0( sizeof(SegmentDatabaseDescriptor) * 
								segDesc->db_count );

		MemoryContextSwitchTo(oldContext);
			
		int j;						
		for ( j=0; j<segDesc->db_count; j++ )
		{
			Assert( p->segindex == segDesc->segindex );
			segdbDesc = &segDesc->db_descriptors[j];
			segdbDesc->segment_descriptor = segDesc;
			segdbDesc->segment_database_info = p;
			segdbDesc->conn = NULL;		
			segdbDesc->thread = (pthread_t)NULL;
			segdbDesc->error_message = NULL;
			segdbDesc->cmdTuples = 0;
			segdbDesc->resultStatus = PGRES_COMMAND_OK;
			segdbDesc->cmdStatus = NULL;
			
			p++;
		}									
		
	}
	
	return ClusterDefn;
}

/*
 *	thread_DoConnect is the thread proc used to perform the connection to one of the qExecs.
 */
static void *thread_DoConnect(void *arg)
{
	DoConnectParms				*pParms				= (DoConnectParms *)arg;
	SegmentDatabaseDescriptor **segdbDescPtrArray	= pParms->segdbDescPtrArray;
	int							db_count			= pParms->db_count;
	
	int		i;
	
	SegmentDatabaseDescriptor	*segdbDesc;
	CdbComponentDatabaseInfo	*q;
	
	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to connect to. 
	 */
	for ( i=0; i<db_count; i++ )
	{
		segdbDesc = segdbDescPtrArray[i];
		q = segdbDesc->segment_database_info;

		/*
		 * We use PQExpBufferData instead of StringInfoData
		 * because the former uses malloc, the latter palloc.
		 * We are in a thread, and we CANNOT use palloc since it's not
		 * thread safe.  We cannot call elog or ereport either for the
		 * same reason.
		 */
		PQExpBufferData buffer;
		initPQExpBuffer(&buffer);
		
		/*
		 * Build the connection string
		 */
		appendPQExpBuffer(&buffer,
			"options='-c cdb_session_role=EXECUTE -c cdb_querygroup_id=%d' ",
			Cdb_querygroup_id);
		
		if ( q->hostname == NULL ) 
			appendPQExpBufferStr(&buffer, "host='' " );
        else if ( isdigit(q->hostname[0]) )
			appendPQExpBuffer(&buffer, "hostaddr=%s ", q->hostname );
		else 
			appendPQExpBuffer(&buffer, "host=%s ", q->hostname );
					
		appendPQExpBuffer(&buffer, "port=%u ", q->port);
		appendPQExpBuffer(&buffer, "dbname=%s ", q->dbname);
		appendPQExpBuffer(&buffer, "user=%s ", q->cdbsuperuser);

		if ( q->cdbsuperpswd != NULL )
			appendPQExpBuffer(&buffer, "password='%s' ", q->cdbsuperpswd);

		/*
		 * Call libpq to connect
		 */					
		PGconn *conn = PQconnectdb(buffer.data);

		if ( PQstatus(conn) == CONNECTION_BAD )
		{
			/*
			 * When we get an error, we strdup it here.  When the main thread
			 * checks for errors, it makes a palloc copy of this, and frees
			 * this.
			 */					
			segdbDesc->error_message = strdup(PQerrorMessage(conn));
			PQfinish(conn);
			conn = NULL;
		}

		segdbDesc->conn = conn;
		free(buffer.data);
	}

	return(NULL);
}

/*
 * AddSegDBToConnThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to be connected to.
 * It figures out which thread will handle it, based on the setting of
 * cdb_connections_per_thread.
 */
void addSegDBToConnThreadPool( DoConnectParms *ParmsAr, int *segdbCount, SegmentDatabaseDescriptor *segdbDesc)
{
	DoConnectParms *pParms;
	int	ParmsIndex;
	
	Assert( segdbCount != NULL );
	
	/*
	 * The proper index into the DoConnectParms array is computed, based on 
	 * having cdb_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % cdb_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	ParmsIndex = (*segdbCount) / cdb_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];
	
	if ( (*segdbCount) % cdb_connections_per_thread == 0 )
	{
		pParms->segdbDescPtrArray = (SegmentDatabaseDescriptor **)palloc0( cdb_connections_per_thread * sizeof(SegmentDatabaseDescriptor *) );
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
