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
#include "cdb/ams-net.h"
#include "cdb/cdbmotion.h"
#include "cdb/mpiutils.h"
#include "utils/memutils.h"



typedef struct PgMpiBuffer
{
	/* pointer to the data-buffer. */
	uint8 *pBuffer;

	/* size of the message in the buffer, if any. */
	int msgSize;

} PgMpiBuffer;



/*=========================================================================
 * STATIC STATE VARS
 */

/* our own segmentId, (or contentId) */
static int segId;

/* total number of dbs in the system. */
static int numDbs;

/* keeps track of whether we have an MPI Send operation in progress. */
static bool sendInProgress;

/* buffer used to Recv data from MPI into. */
static uint8 *inBuff;


static MPI_Request mpiSendReq;
static MPI_Request mpiRecvReq;


/* MPI Communicator between us and our dedicated ams-svc. */
static MPI_Comm amssvc_comm;

/* Array of Buffers (total of numDbs).  These buffers are used to form AMS 
 * Packets by aggregating tcItems into them.  There is one per destination.
 * The destination that corresponds to us is used for broadcast packets.
 */
static PgMpiBuffer *outBuffs;

/* The one single buffer that is used to send data out via MPI.  Data is 
 * generally copied from one of the outBuffs into this buffer before the
 * appropriate MPI send call is performed.
 */
static uint8 *outMpiBuff;

/* Keeps track of the last buffer that was sent. */
static int lastBuffSent;

/* marks whether we are in End Of Stream mode or not.  
 */
static bool eosMode;

/* when in eosMode, this keeps track of which buffers have already
 * been flushed out. */
static int flushedIdx;

/* keeps track of the motion Id we are expecting when in eosMode. */
static int eosMotionId;

/* Set to true after the first call to SendShutdownToAMS(). */
static bool shutdownStarted;



/*=========================================================================
 * FUNCTION DEFINITIONS
 */

static TupleChunkListItem getTupleChunk( int recvSize );
static bool sendEndOfStream( TupleChunkListItem tcItem );
static void sendBuffer( int buffId ,  TupleChunkListItem tcItem );



/*=========================================================================
 * VISIBLE FUNCTIONS
 */
 
/* Sets our MPI Communicator. 
 */
void setMpiComm( MPI_Comm comm )
{
    amssvc_comm = comm;
}


/* Function used to initialize.
 *
 *	PARAMETERS:
 *	- segmentId:  The content field of ourself.
 */
void InitMotionLayerIPC( int segmentId )
{

	int i,rc;
	MemoryContext oldCtxt;
	
#ifdef AMS_VERBOSE_LOGGING
	elog( DEBUG3, "Entering InitMotionLayerIPC");
#endif


	segId = segmentId;

	//switch to a permanent memory context.
	oldCtxt = MemoryContextSwitchTo(TopMemoryContext);


	flushedIdx = 0;
	sendInProgress = false;

	/* get a connection to our ams-svc process. */
	numDbs = ams_fabric_connect();
	
	/*setup the mpi requests */
	mpiSendReq = MPI_REQUEST_NULL;
	mpiRecvReq = MPI_REQUEST_NULL;


	/* setup the buffers we are going to use. */
	outBuffs = palloc( numDbs * sizeof( PgMpiBuffer) );
	for( i = 0; i < numDbs; i++ )
	{
		outBuffs[i].pBuffer = (uint8 *) palloc( sizeof(uint8) * MAX_AMS_PACKET_SIZE );
		outBuffs[i].msgSize = 0;
	}
	outMpiBuff = palloc( sizeof(uint8) * MAX_AMS_PACKET_SIZE );

	inBuff  = palloc( sizeof(uint8) * MAX_TUPLE_CHUNK_SIZE );


	/* start receiving */
	rc = MPI_Irecv( inBuff, MAX_TUPLE_CHUNK_SIZE, MPI_BYTE, MPI_ANY_SOURCE,
                    /*tag*/ MPI_ANY_TAG, amssvc_comm, &mpiRecvReq );

    checkMpiRc( rc, "Error during InitMotionLayerIPC");

	MemoryContextSwitchTo(oldCtxt);


#ifdef AMS_VERBOSE_LOGGING
	elog( DEBUG3, "Leaving InitMotionLayerIPC");
#endif

}



void CleanUpMotionLayerIPC(void)
{
	int i;

	shutdownStarted = false;	

	/* cancel any pending MPI sends and receives. */
	if( mpiSendReq != MPI_REQUEST_NULL)
		MPI_Cancel( &mpiSendReq );

	MPI_Request_free( &mpiSendReq );

	if( mpiRecvReq != MPI_REQUEST_NULL)
		MPI_Cancel( &mpiRecvReq );

	MPI_Request_free( &mpiRecvReq );

	
	for( i = 0; i < numDbs; i++ )
	{
		pfree( outBuffs[i].pBuffer );
		
	}
	
	pfree(inBuff);
	pfree(mpiSendReq);
	pfree(mpiRecvReq);
	pfree(outBuffs);
	pfree(outMpiBuff);
	
	
	//TODO: any more state to wipe?
}



/* sends a shutdown message to the ams. this message will cancel
 * any message that is inflight and then send this one.
 *
 * NOTE: Once this has been called, its not safe to do any more work
 *		 in this component.  So no more Send(...) and Receive(...) calls.
 * 
 */	
bool SendShutdownToAMS(TupleChunkListItem tcItem)
{
	int rc;
	
	if( shutdownStarted )
	{
		int flag;
		MPI_Status status;
		
		/* We've already kicked off Sending our shutdown message. Now we just
		 * want to keep checking until MPI says its been sent out.  Then we 
		 * can consider ourselves shutdown.
		 */
		
		//just check if its done.
		rc = MPI_Test( &mpiSendReq, &flag, &status );
        checkMpiRc( rc, "Error during MPI_Test");
            
        if( flag )
            return true;
        else
            return false;
	}
	else
	{
		/* This is a minor sin.  we are taking over mpiSendReq. but if we
		 * at the point of shutting down... no one else will be calling
		 * the other functions here.
		 *
		 * kick off sending our Shutdown message.  Notice we are not using
		 * the outMpiBuff and are even kicking off a 2nd send.  This is ok,
		 * MPI will deliver this and the ams-svc will get it.
		 */
		rc = MPI_Isend( tcItem->chunk_data, tcItem->chunk_length, MPI_BYTE,
						0,0, amssvc_comm, &mpiSendReq );
		checkMpiRc( rc, "MPI Error while sending Shutdown msg to AMS");
		return false;
		
	}
		
}



/* This method only accepts a single tcItem, and not a chain of
 * tcItems.
 */
bool SendTupleChunkToAMS(TupleChunkListItem tcItem)
{
	int rc,flag, idx;
	MPI_Status status;
	int16 destSegId;


	Assert( tcItem != NULL );
	Assert( tcItem->p_next == NULL );

	//handle EOS messages.
	if( eosMode )
	{
		/* EndOfStream mode.  Means we've seen an EOS message but haven't 
		 * finished processing it.  For now, if we are in eosMode and
		 * we see a tcItem that is not an eosMode then: (1) it better
		 * be for a different motion node. (2) we just return false
		 * and hope that we'll see our eos tcItem again.
		 *
		 * TODO: this is a leap of faith to think that if we are seeing
		 *		  another motion nodes tcItems interleaved with an EOS message
		 *		  that everything will work out. But once the threaded version
		 *		  of ml_ipc is done, this hole won't be here.
		 */
		if( GetChunkType( tcItem->chunk_data ) != TC_END_OF_STREAM )
		{
			Assert( GetMotionNodeID(tcItem->chunk_data) != eosMotionId );
			return false;
		}	
		else
		{	
			/* TODO:  This code is definitely broken if we have interleaved
			 *		  EOS messages.  The threaded version will fix this, but
			 *		  for now, we will just blowup via the Assert and at least
			 *		  be safe.
			 */
			//We are assuming that EOS messages can't be interleaved. In fact, 
			// this code doesn't work if that's not the case.
			
			Assert( GetMotionNodeID(tcItem->chunk_data) == eosMotionId );
					
			return sendEndOfStream( tcItem );
		}
		
	}
	else if( GetChunkType( tcItem->chunk_data ) == TC_END_OF_STREAM )
	{
		/* Time to enter into eosMode.  See the comments above about the
		 * limitations of this.  All of this funk should go away when the 
		 * threaded implementation gets done. 
		 */
		eosMode = true;
		eosMotionId = GetMotionNodeID( tcItem->chunk_data);
		return sendEndOfStream( tcItem );
	}



	destSegId = GetTargetSegIdx( tcItem->chunk_data );
	

	/* we cheat and use our own segid slot for broadcast. Also need to shift
	 * things up once since the QD has segId of -1.
	 */
	if( destSegId == BROADCAST_SEGIDX )
		idx = segId + 1;
	else
		idx = destSegId + 1;


	if((outBuffs[idx].msgSize + tcItem->chunk_length) < MAX_AMS_PACKET_SIZE )
	{
		/* This TupleChunk will fit into our temp buffer. so just copy it in
		 * there and return success.
		 */
		memcpy( outBuffs[idx].pBuffer + outBuffs[idx].msgSize, 
			tcItem->chunk_data, tcItem->chunk_length );
		
		outBuffs[idx].msgSize += tcItem->chunk_length;
		return true;

	}
	else
	{
		//tcItem doesn't fit in our temp buffer.  try and send out our
		// temp buffer.
		if( sendInProgress )
		{
			rc = MPI_Test( &mpiSendReq, &flag, &status );
			checkMpiRc( rc, "Error during MPI_Test");
			
			if( flag )
			{
				sendBuffer( idx, tcItem );
				return true;
			}
			else
				return false;
		}
		else
		{
			sendBuffer( idx, tcItem );
			return true;
		}
	}
	

}




TupleChunkListItem RecvTupleChunkFromAMS(void)
{
	int rc,flag;
	MPI_Status status;

	//check on our status
	rc = MPI_Test( &mpiRecvReq, &flag, &status );
	checkMpiRc( rc, "Error during MPI_Test");


	if( flag )
	{
		int recvSize;
		MPI_Get_count(&status, MPI_BYTE, &recvSize);
		return getTupleChunk( recvSize );
	}
	else
	{
		//nothing to do here.
		return NULL;
	}

}




bool CanWriteToAMS( int segId )
{
	//TODO: this code doesn't get used until waitFor is implemented.
	
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
static TupleChunkListItem getTupleChunk( int recvSize )
{
    //we got something.  form it into a TupleChunkListItem and pass it
    // back
	int rc;
    TupleChunkListItem tcItem;
	TupleChunkListItem firstTcItem = NULL;
	TupleChunkListItem lastTcItem = NULL;

    uint16 tcSize;
	uint16 bytesRead = 0;


	while( bytesRead != recvSize )
	{
    	tcSize = TUPLE_CHUNK_HEADER_SIZE + GetChunkDataSize( inBuff + bytesRead );
		tcItem = (TupleChunkListItem)palloc0( sizeof(TupleChunkListData) + tcSize );	
		tcItem->chunk_length = tcSize;

		memcpy( tcItem->chunk_data, inBuff + bytesRead, tcSize );

		bytesRead += tcSize;

		if( firstTcItem == NULL )
		{
			firstTcItem = tcItem;
			lastTcItem = tcItem;
		}
		else
		{
			lastTcItem->p_next = tcItem;
			lastTcItem = tcItem;
		}
			
	}


	//kick off the next recv
	rc = MPI_Irecv( inBuff, MAX_TC_DATA_SIZE, MPI_BYTE, MPI_ANY_SOURCE,
					/*tag*/ MPI_ANY_TAG, amssvc_comm, &mpiRecvReq );
	checkMpiRc( rc, "Error during getTupleChunk");


    return firstTcItem;
}


/* Helper function used to process and send out an EndOfStream message.
 *
 * We need to be careful here, we don't want to "return true" until the MPI
 * message with the EOS has actually been sent out.  This is because once
 * we say we've sent out the "EOS" message, we can't really be sure that
 * someone will call into this ml_ipc code anymore.  So we need to also
 * make sure that all of the buffers will be flushed BEFORE we send out the
 * EOS message, since all rows prior to it must arrive before the EOS.
 *
 * TODO: is it possible to have multiple EOS messages interleaved?  I think that
 *       its possible.  This makes things much more complicated.
 *
 * Once we see an EOS message for the 1st time... will lock out all other motion
 * Ids from being able to send out until the EOS is processed.
 *
 *
 */
static bool sendEndOfStream( TupleChunkListItem tcItem  )
{
	int rc, flag;
	MPI_Status status;
	
	rc = MPI_Test( &mpiSendReq, &flag, &status );
	checkMpiRc( rc, "Error during MPI_Test");

	
	if( flag )
	{
		sendInProgress = false;
		
	
		if( flushedIdx < numDbs )
		{
			//flush our next destSegID buffer
			if( outBuffs[flushedIdx].msgSize != 0 )
			{
				rc = MPI_Isend( outBuffs[flushedIdx].pBuffer,
					outBuffs[flushedIdx].msgSize, MPI_BYTE, 0, 0, amssvc_comm, &mpiSendReq );
				checkMpiRc( rc, "Error during MPI_ISend");

				//update state
				sendInProgress = true;
				outBuffs[flushedIdx].msgSize = 0;
			}

			flushedIdx++;
			return false;
		}
		else if( flushedIdx == numDbs )
		{
			//now we will actually send our EOS
			rc = MPI_Isend( tcItem->chunk_data, tcItem->chunk_length, MPI_BYTE, 0, 0,
				amssvc_comm, &mpiSendReq );
			sendInProgress = true;
			flushedIdx++;
			return false;
		}
		else
		{
			flushedIdx = 0;
			eosMode = false;
			eosMotionId = -1;
			return true;
		}		
	}
	else
	{
		return false;
	}
}

static void sendBuffer( int buffId, TupleChunkListItem tcItem )
{
	int rc;

	memcpy( outMpiBuff, outBuffs[buffId].pBuffer, outBuffs[buffId].msgSize );	

	rc = MPI_Isend( outMpiBuff, outBuffs[buffId].msgSize, MPI_BYTE, 0, 0,
		amssvc_comm, &mpiSendReq );
	checkMpiRc( rc, "Error during MPI_Isend");

	sendInProgress = true;
	outBuffs[buffId].msgSize = 0;

	//copy *this* tuple into the buffer
	memcpy( outBuffs[buffId].pBuffer, tcItem->chunk_data, tcItem->chunk_length );
	
	outBuffs[buffId].msgSize += tcItem->chunk_length;
	
}


