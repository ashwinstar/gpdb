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
	int numDbs,rc;

	segId = segmentId;

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
			elog(DEBUG2, "Send tuple-chunk completed.");
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

		elog(DEBUG2, "Started sending tuple-chunk.");


		/* not sure if this helps.  but it would be nice to catch sends that go out 
     	 * immediately.  Otherwise every TC that gets sent will require at least
     	 * 2 calls to this function. 
      	 */
		rc = MPI_Test( &mpiSendReq, &flag, &status );
		checkMpiRc( rc, "Error during MPI_Test");

		if (flag)
		{
			elog(DEBUG2, "Send tuple-chunk completed (quick-complete).");
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



void waitForMLIpc( bool readInterest, int *readSegId, bool writeInterest, int *writeSegId )
{
	int rc, index,count;
	MPI_Status status;

	if( readInterest )
	{
		//Currently we don't support read interest for anything other than all.
		Assert( *readSegId == READ_INTEREST_ALL );

		waitReqs[count] = mpiRecvReq;
		count++;
	}

	if( writeInterest )
	{
		waitReqs[count] = mpiSendReq;
		count++;
	}

	//wait for any of our requests to finish.	
	rc = MPI_Waitany( count, waitReqs, &index, &status );

	checkMpiRc( rc, "Error during MPI_Waitany");

	/* post-process what happened. we have work to do here if it was a read because 
	 * MPI_Waitany() sets our request to NULL if its ready. (that isn't very nice).
     * So we'll need to book keep that that read is done so that the 
     * RecvTupleChunkFromAMS() function can do the right thing.
     */
	if( readInterest && index == 0)
	{
		tcReady = true;	
	} 
	
	
	/* TODO: in this impl we don't even have to say which event happened.  which kind
	 * of sux.  but is ok, for the basic way we are going to use it in the motion node.
	 * otherwise we need to pass back what happened in readSegId and writeSegId.
	 */
   

}

/*=========================================================================
 * HELPER FUNCTIONS
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
	elog( DEBUG2, "Received TupleChunk from  segId: %d", srcSegId );

	//kick off the next recv
	rc = MPI_Irecv( inBuff, MAX_TC_DATA_SIZE, MPI_BYTE, MPI_ANY_SOURCE,
					/*tag*/ MPI_ANY_TAG, amssvc_comm, &mpiRecvReq );

	checkMpiRc( rc, "Error during getTupleChunk");

    return tcItem;

}

