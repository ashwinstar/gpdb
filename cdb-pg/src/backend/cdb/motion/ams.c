/*-------------------------------------------------------------------------
 *
 * ams
 *	  Internal utility support functions for interacting with ams.
 *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <mpi.h>

#include "cdb/ams.h"
#include "cdb/ams-net.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "cdb/mpiutils.h"
#include "lib/stringinfo.h"
#include "cdb/ml_ipc.h"

#include "executor/spi.h" 
#include "commands/sequence.h"  /* for nextval() */
#include "storage/ipc.h"  /*for on_proc_exit() */


/* the number of times we will try and lookup our ams-svc component
 * in ams_fabric_connect().  
 */
#define MPI_LOOKUP_TRIES 30

/* The amount of time we sleep in microseconds in between lookup tries.
 */ 
#define MPI_LOOKUP_SLEEP 3000


/*=========================================================================
 * STATIC STATE VARS
 */

/* how to talk to ams-svc process. */
static MPI_Comm amssvc_comm;

/* keeps track of if disconnected from ams-svc or not. */
static bool disconnected;

/*=========================================================================
 * FUNCTION DEFINITIONS
 */

/* Declarations of helper functions. */

static void qd_ams_setup(void);



/*=========================================================================
 * VISIBLE FUNCTIONS
 */

/*
 * Performs basic setup of the ams networking infrastructure.  This function
 * initializes the networking layer (MPI), and for the Query Dispatcher, it
 * registers information with the ams daemon to establish the collection of
 * ams-svc processes that are used to facilitate communication.
 *
 * Note that this is only the first phase of setup.  There is a second phase
 * of setup that must take place AFTER this phase, and after the Query
 * Dispatcher has successfully established connections to all Query Executors.
 * The second phase is in the ams_fabric_connect() function, and is called
 * during the motion-layer IPC setup.
 *
 * TODO: this should probably be moved out of here and into someplace else.
 */
void ams_setup( )
{
	int rc;
    
    /* If we are utility role... exit now */
    if ( Cdb_role == CDB_ROLE_UTILITY )
	{
		elog(DEBUG1, "CDB is in utility mode.  Skipping ams_setup.");
        return;
	}

	/*
     * Establish a call-back so that when the process exit we also
     * send a kill message to our ams-svg.
     *
	 */
    on_proc_exit( ams_disconnect, (Datum) NULL ); 
	
	/* Perform MPI Init.  We do this for QD AND QE */
    
#if USE_ASSERT_CHECKING
    {
		/* Sanity check:  We shouldn't already be initialized! */
        int deb_mpi_initialized;
        MPI_Initialized(&deb_mpi_initialized);
        AssertState(!deb_mpi_initialized);
    }
#endif
	
    rc = MPI_Init( NULL, NULL );	/* No command-line arguments to pass. */
	checkMpiRc( rc, "MPI initialization error");
	
	MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
	
	/* Dispatcher specifies the query-group ID.  Executors get it from the
	 * connection-details.
	 */
	if (Cdb_role == CDB_ROLE_DISPATCH)
	{
		Datum seqval;
		text *seqname = DatumGetTextP(DirectFunctionCall1(textin,
									CStringGetDatum("cdb.group_seq")));
		
		seqval = DirectFunctionCall1(nextval, PointerGetDatum(seqname));

		Cdb_querygroup_id = Int32GetDatum((int32) DatumGetInt64(seqval));
		pfree(seqname);

	}
	elog(DEBUG1, "This process is part of query-group %d.\n", Cdb_querygroup_id);
    
    /* The Query Dispatcher needs to tell the amsd what the CDB looks like,
	 * to complete this part of setup.
	 */
	if (Cdb_role == CDB_ROLE_DISPATCH)
		qd_ams_setup();
}



/* sends a Shutdown message to ams-svc process from ams-svc before calling
 * MPI_Finalize
 */
void ams_disconnect( int exit_code, Datum arg )
{
    int rc;
	bool done = false;
	TupleChunkListItem tcItem;
	TupleChunkListItem abortTcItem;

    //don't do anything if we aren't even connected.
    if( disconnected )
        return;
        
        
	elog( DEBUG3, "AMS Disconnect Started" );

	//form our abortTcItem
	abortTcItem = makeTupleChunkListItem( TC_SHUTDOWN, NULL, 0, CdbIdentity.segindex,   
											/*tgtSegIdx*/ 0, /*mnid*/ 0 );	

	/* we will loop around until we are able to send our TC_Abort_Stream
	 * message.  We also need to keep pulling out tuples and throwing
	 * them away in case we need to "un-stick" someone else in order
	 * to allow us to send
	 */
	while( !done )
	{
		//send
		if( SendTupleChunkToAMS(abortTcItem) )
		{
			//we are done here...
			done = true;
		}	

		//receive
		tcItem = RecvTupleChunkFromAMS();

		if( tcItem != NULL )
			pfree(tcItem);

	}	
	
    disconnected = true;
    
	elog( DEBUG3, "AMS Disconnect Finished." );
	
	rc = MPI_Finalize();
	if (rc != MPI_SUCCESS)
		elog(WARNING, "Error during MPI_Finalize().");
		
	return;
}


void ams_cleanup( )
{
	
    /* If we are utility role... exit now */
    if ( Cdb_role == CDB_ROLE_UTILITY )
	{
		elog(DEBUG1, "CDB is in utility mode.  Skipping ams_cleanup.");
        return;
	}
	
	/* TODO: impl this.  There has to at least be more than disconnecting
	 *       from MPI!
	 */
	 
    //disconnect from ams-svc.	 
    ams_disconnect( 0, NULL );
    
	
	return;
}


/* If this backend is not in utility mode, this function connects the backend
 * to the corresponding ams-svc process that should have been started in phase
 * 1 of the AMS setup.  If the comm-link cannot be established, it is a fatal
 * error.
 *
 * RETURNS
 *  - the number of dbs in the system
 */
int ams_fabric_connect(void)
{
	int rc,numDbs;
	StringInfoData amsSvcPubName;
    char amssvc_port_name[MPI_MAX_PORT_NAME];
    uint8 buff[MAX_CMD_SIZE];
    int bytesWritten;
    MPI_Status status;
	
	
    /* If we are utility role... exit now */
    if ( Cdb_role == CDB_ROLE_UTILITY )
	{
		elog(DEBUG1, "CDB is in utility mode.  Skipping ams_setup.");
        return -1;
	}
	
	
	initStringInfo(&amsSvcPubName);
	appendStringInfo(&amsSvcPubName, "ams-svc-%s_%d_%d", CdbIdentity.cdbname, 
	                   Cdb_querygroup_id, CdbIdentity.segindex );
	
	/* Connect this Postgres backend to the corresponding ams-svc process.
     * We loop here if we are not successful since its possible that we will get 
     * here before the ams-svc has fully gotten setup. 
	 */
    int limit = MPI_LOOKUP_TRIES;
    int tries;	

	for( tries=0; tries < limit; tries++ )
	{
    	elog(DEBUG3, "Looking up ams-svc port using name %s.", amsSvcPubName.data);
    	rc = MPI_Lookup_name(amsSvcPubName.data, MPI_INFO_NULL, amssvc_port_name);

		if( rc != MPI_SUCCESS )
			pg_usleep(MPI_LOOKUP_SLEEP);
		else
			break;
	}
	
	checkMpiRc( rc, "AMS setup could not lookup the ams-svc daemon");
	
    elog( DEBUG3, "Connecting to ams-svc at address %s.", amssvc_port_name );
    rc = MPI_Comm_connect(amssvc_port_name, MPI_INFO_NULL, 0, MPI_COMM_SELF, &amssvc_comm);
    if (rc != MPI_SUCCESS)
	{
    	elog(FATAL, "Postgres could not establish a connection to ams-svc"
			" at %s.  Reason:\n\t%s", amssvc_port_name, getMpiErrMsg(rc));
	}
	
	/* we are connected... be nice and say Hello. */
	elog( DEBUG3, "Sending a DbHello to ams-svc at address %s.", amssvc_port_name );
	
	bytesWritten = WriteDbHelloMessage( buff, MAX_CMD_SIZE, CdbIdentity.cdbname,
	                                    Cdb_querygroup_id, CdbIdentity.segindex );
	rc = MPI_Send(buff, bytesWritten, MPI_UNSIGNED_CHAR, /*dest*/ 0, /*tag*/ 0, amssvc_comm);
	checkMpiRc( rc, "AMS setup Failed to send HELLO message to ams-svc");

	/* get back the number of dbs in the system. */
	rc = MPI_Recv( &numDbs, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, amssvc_comm, &status);
	checkMpiRc( rc, "AMS setup Failed to receive db count from ams-svc");

	setMpiComm( amssvc_comm );

    disconnected = false;
    
	return numDbs;	
	
}



/*=========================================================================
 * HELPER FUNCTIONS
 */

/* This helper function contains the parts of ams-setup that are specific to
 * the Query Dispatcher.
 */
void qd_ams_setup(void)
{
	int rc, i, count;
    MPI_Comm amsd_comm;
    MPI_Status status;
    uint8 buff[MAX_CMD_SIZE];
    char amsd_port_name[MPI_MAX_PORT_NAME];
    CdbComponentDatabaseInfo  *p_db_info;
    CdbComponentDatabases *cdb_component_dbs;
    int bytesWritten;
	DbEntry *db_entry;

	AssertState(Cdb_role == CDB_ROLE_DISPATCH);
	
    /* We are the QD, and we need to register with the AMSD.  Start by
	 * connecting to the amsd.
	 */
    rc = MPI_Lookup_name(AMSD_PUBNAME, MPI_INFO_NULL, amsd_port_name);
	checkMpiRc( rc, "AMS setup could not lookup the AMSD daemon");

    elog( DEBUG3, "Connecting to amsd at address %s.", amsd_port_name );
    
    rc = MPI_Comm_connect(amsd_port_name, MPI_INFO_NULL, 0, MPI_COMM_SELF, &amsd_comm );    
    if ( rc != MPI_SUCCESS )
	{
    	elog( FATAL, "AMS setup could not establish a connection to AMSD daemon at: %s", 
    		         amsd_port_name );
	}
    
    /* Now get info from admin schema and prepare Register structure */        
	
    getCdbCdb();				/* read in cdb table */
	
    cdb_component_dbs = getCdbComponentDatabases();
    
    Register *reg = palloc( sizeof( Register ) );
    reg->cdbName = CdbIdentity.cdbname;
    reg->groupId = Cdb_querygroup_id;
    reg->numDbs = cdb_component_dbs->total_entry_dbs + cdb_component_dbs->total_segment_dbs;
    reg->listOfDbs = palloc( sizeof( DbEntry) * reg->numDbs );
    
	/* We use this to keep track of which db-entry we are on, across these 2 loops. */
	count = 0;
	
    /* now go through and form our listOfDbs */
    for ( i = 0; i < cdb_component_dbs->total_entry_dbs; i++ )
    {
        p_db_info = &cdb_component_dbs->entry_db_info[i];
    	db_entry = reg->listOfDbs + count++;
		db_entry->contentId = p_db_info->segindex;
		db_entry->hostName = p_db_info->hostname;
    }
    
    /* Send the component dbs 2nd. */
    for ( i = 0; i < cdb_component_dbs->total_segment_dbs; i++ )
    {
    	p_db_info = &cdb_component_dbs->segment_db_info[i];        
    	db_entry = reg->listOfDbs + count++;
		db_entry->contentId = p_db_info->segindex;
		db_entry->hostName = p_db_info->hostname;   		
    }
    
    
    /* SEND 1 REGISTER Message.  if we can't fit all of our DB info, then we'll send
     * as many MAP Messages as it takes to send it.  then we'll wait for a response
     * from amsd. 
     */    
    count = WriteRegisterMessage( reg, buff, MAX_CMD_SIZE, &bytesWritten );
    
    rc = MPI_Send(buff, bytesWritten, MPI_UNSIGNED_CHAR, /*dest*/ 0, /*tag*/ 0, amsd_comm);

    if (rc != MPI_SUCCESS)
    	elog( FATAL, "AMS setup Failed to send REGISTER message to amsd.\n");


	elog( DEBUG3, "Sent REGISTER message to amsd with %d DbEntries in it.\n", count );
    
    /* send some Map Messages if we need to. */
    while( count < reg->numDbs )
    {
    	bytesWritten = 0;
    	
    	count = WriteMapMessage( reg, count, buff, MAX_CMD_SIZE, &bytesWritten );    	
    	rc = MPI_Send( buff, bytesWritten, MPI_UNSIGNED_CHAR, /*dest*/ 0, /*tag*/ 0, amsd_comm );
    	
    	elog( DEBUG3, "Sent MAP message to amsd with %d DbEntries in it.\n", count );
		
		checkMpiRc( rc, "AMS setup Failed to send MAP message to amsd ");
    }
    
    
    /* receive our response gram */
    
    rc = MPI_Recv(buff, MAX_CMD_SIZE, MPI_UNSIGNED_CHAR, MPI_ANY_SOURCE, 
    				MPI_ANY_TAG, amsd_comm, &status );
	checkMpiRc( rc, "AMS setup Failed to receive a REGISTER RESPONE message from amsd");
    
    rc = MPI_Get_count( &status, MPI_UNSIGNED_CHAR, &count );				
	checkMpiRc( rc, "AMS setup Failed to receive a REGISTER RESPONE message from amsd");
	


    /* get our response code. */
    rc = ReadRegisterResponseMessage( buff, count );
    
    if( rc == AMP_OK )
    	elog( DEBUG3, "Registration with AMSD complete!\n");
    else
	    elog( FATAL, "AMS setup Failed.  amsd sent REGISTER RESPONSE indicating error."
	    		   	 " See amsd log for more info.\n");
    
	
    /* Disconnect from the amsd. */
    MPI_Comm_disconnect(&amsd_comm);
}

