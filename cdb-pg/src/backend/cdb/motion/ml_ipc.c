/*-------------------------------------------------------------------------
 * ml_ipc.c
 *     Motion Layer IPC Layer.  
 *
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <mpi.h>

#include "cdb/tupchunklist.h"
#include "cdb/ml_ipc.h"
#include "cdb/ams.h"
#include "cdb/cdbmotion.h"
#include "cdb/mpiutils.h"
#include "utils/memutils.h"


/*=========================================================================
 * STATIC STATE VARS
 */

/* our own segmentId, (or contentId) */
static int segId;


static bool sendInProgress;
static uint8 *outBuff;
static uint8 *inBuff;

static bool tcReady;
static MPI_Request mpiSendReq;
static MPI_Request mpiRecvReq;

static MPI_Request *waitReqs;

static MPI_Comm amssvc_comm;


/*=========================================================================
 * FUNCTION DEFINITIONS
 */

static TupleChunkListItem getTupleChunk(void);


/*=========================================================================
 * VISIBLE FUNCTIONS
 */
void setMpiComm( MPI_Comm comm )
{
    amssvc_comm = comm;
}

void InitMotionLayerIPC( int segmentId )
{
	MemoryContext oldCtxt;

	int numDbs,rc;

	segId = segmentId;

	oldCtxt = MemoryContextSwitchTo(TopMemoryContext);

	/* get a connection to our ams-svc process. */
	numDbs = ams_fabric_connect();
	
	/*setup the mpi requests */
	mpiSendReq = MPI_REQUEST_NULL;
	mpiRecvReq = MPI_REQUEST_NULL;

	sendInProgress = false;

	/* waitReqs needs to hold all send and recv requests */
	waitReqs = palloc( sizeof(MPI_Request) * 2 );

	/* setup the buffers we are going to use. */
	outBuff = palloc( sizeof(uint8) * MAX_TUPLE_CHUNK_SIZE );
	inBuff  = palloc( sizeof(uint8) * MAX_TUPLE_CHUNK_SIZE );

	/* start receiving */
	rc = MPI_Irecv( inBuff, MAX_TUPLE_CHUNK_SIZE, MPI_BYTE, MPI_ANY_SOURCE,
                    /*tag*/ MPI_ANY_TAG, amssvc_comm, &mpiRecvReq );

    checkMpiRc( rc, "Error during InitMotionLayerIPC");

	MemoryContextSwitchTo(oldCtxt);
}



void CleanUpMotionLayerIPC(void)
{
	pfree(outBuff);
	pfree(inBuff);
	pfree(mpiSendReq);
	pfree(mpiRecvReq);
	

	/* cancel any pending MPI sends and receives. */
	if( mpiSendReq != MPI_REQUEST_NULL)
		MPI_Cancel( &mpiSendReq );

	MPI_Request_free( &mpiSendReq );

	if( mpiRecvReq != MPI_REQUEST_NULL)
		MPI_Cancel( &mpiRecvReq );

	MPI_Request_free( &mpiRecvReq );


	//TODO: any more state to wipe?
}


/* This method only accepts a single tcItem, and not a chain of
 * tcItems.
 */
bool SendTupleChunkToAMS(TupleChunkListItem tcItem)
{
	int rc,flag;
	MPI_Status status;

	Assert( tcItem != NULL );
	Assert( tcItem->p_next == NULL );


	/* check to see if we have a free buffer for the destination.
     * If yes, send it, otherwise return false. 
     */

	rc = MPI_Test( &mpiSendReq, &flag, &status );

	checkMpiRc( rc, "Error during MPI_Test");

	if( sendInProgress )
	{
		//see if we are done.
		if( flag )
		{
			//we've finished this one.  don't send it again.
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG2, "Send tuple-chunk completed.");
#endif
			sendInProgress = false;
			return true;
		}
		else
		{
			//not done.
			return false;
		}
	}
	else
	{
		//no send in progress... need to kick one off.
		memcpy( outBuff, tcItem->chunk_data, tcItem->chunk_length );

		rc = MPI_Isend( outBuff, tcItem->chunk_length, MPI_BYTE, /*dest*/ 0, /*tag*/ 0, 
					amssvc_comm, &mpiSendReq);

		checkMpiRc( rc, "Error during MPI_Isend");
		sendInProgress = true;

#ifdef AMS_VERBOSE_LOGGING
		elog(DEBUG2, "Started sending tuple-chunk.");
#endif

		/* not sure if this helps.  but it would be nice to catch sends that go out 
     	 * immediately.  Otherwise every TC that gets sent will require at least
     	 * 2 calls to this function. 
      	 */
		rc = MPI_Test( &mpiSendReq, &flag, &status );
		checkMpiRc( rc, "Error during MPI_Test");

		if (flag)
		{
#ifdef AMS_VERBOSE_LOGGING
			elog(DEBUG2, "Send tuple-chunk completed (quick-complete).");
#endif
			sendInProgress = false;
		}

		return flag;

	}
		
}


TupleChunkListItem RecvTupleChunkFromAMS(void)
{
	int rc,flag;
	MPI_Status status;
	
	if( tcReady )
	{
		/* this means a wait(...) event below was nuked our mpiRecvReq
		 * AND that a tuplechunk is ready. 
 		 */
		tcReady = false;
		return getTupleChunk();
		
	}

	//check on our status
	rc = MPI_Test( &mpiRecvReq, &flag, &status );

	checkMpiRc( rc, "Error during MPI_Test");

	if( flag )
	{
		return getTupleChunk( );
	}
	else
	{
		//nothing to do here.
		return NULL;
	}

}




bool CanWriteToAMS( int segId )
{
    Assert( segId == WRITE_INTEREST_ALL );

    if( sendInProgress )
    {
        int rc,flag;
        MPI_Status status;


        rc = MPI_Test( &mpiSendReq, &flag, &status );
        checkMpiRc( rc, "Error during MPI_Test");


        if( flag )
            return true;
        else
            return false;

    }
    else
    {
        /* if we don't have a send in progress then return true. */
        return true;
    }

}


/*=========================================================================
 * HELPER FUNCTIONS
 */


/* This helper function does several things.  First of all, it creates a
 * new TupleChunkListItem from a byte-sequence just received from the AMS
 * layer.  Second, it initiates a new asynchronous receive operation.
 *
 * NOTE:  The new TupleChunkListItem is palloc()'d, and therefore care
 *        needs to be taken in the management of the memory contexts, etc.
 */
static TupleChunkListItem getTupleChunk( )
{
     //we got something.  form it into a TupleChunkListItem and pass it
    // back
	int rc;
    TupleChunkListItem tcItem;
    uint16 tcSize;

    tcSize = TUPLE_CHUNK_HEADER_SIZE + GetChunkDataSize( inBuff );
    tcItem = (TupleChunkListItem)palloc0( sizeof(TupleChunkListData) + tcSize );

    tcItem->chunk_length = tcSize;
    memcpy( tcItem->chunk_data, inBuff, tcSize );

	int16 srcSegId = GetSourceSegIdx( tcItem->chunk_data );
#ifdef AMS_VERBOSE_LOGGING
	elog( DEBUG2, "Received TupleChunk from  segId: %d", srcSegId );
#endif

	//kick off the next recv
	rc = MPI_Irecv( inBuff, MAX_TC_DATA_SIZE, MPI_BYTE, MPI_ANY_SOURCE,
					/*tag*/ MPI_ANY_TAG, amssvc_comm, &mpiRecvReq );

	checkMpiRc( rc, "Error during getTupleChunk");

    return tcItem;

}

