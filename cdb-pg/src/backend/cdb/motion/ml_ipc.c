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

#include "cdb/tupchunklist.h"
#include "cdb/shmutils.h"
#include "cdb/circbuff.h"
#include "cdb/ml_ipc.h"


int segId;
char *shm_start;
int  semId;
circbuff *inputQueue;
circbuff *outputQueue;

uint8 hdr[ TUPLE_CHUNK_HEADER_SIZE];

void InitMotionLayerIPC( int segmentId )
{
	
	Assert( segmentId > 0 && segmentId <= NUM_SEGS );
	segId = segmentId;
	
	
	
	//amsd should have already setup shared memory and semaphore
	// so we will try and attach and complain loudly if there are problems.
	shm_start = ShmemAttach( SHMEM_KEY );
    
	if( shm_start == NULL )
    {
		ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("Error attaching to Shared Memory: shmemget() failed. "
							   "Check that amsd is running.")));		
    }
    else if( shm_start == (char *) -1 )
    {
        ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("Error attaching to Shared Memory: shmat() failed. "
							   "Check that amsd is running.")));
    }
	
	semId = SemGet( SEM_KEY );
	
	if( semId < 0 )
	{
		ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("Error obtaining Semaphore: semget() failed. "
							   "Check that amsd is running.")));
	}
	
	
	//attach to our input and output queue.  amsd should have already 
	// set these up.  
	// TODO: eventually there will be a shared memory registry that will help
	//       us locate our queue.  for now, it is hardwired.
	int cbuff_size = QSIZE + sizeof(circbuff);
	inputQueue = (circbuff*)(shm_start + (2*(segId-1)*cbuff_size));
	outputQueue = (circbuff*)(shm_start + ( ( (2*(segId-1)) +1) * cbuff_size ));
	
}


void CleanUpMotionLayerIPC(void)
{
	
	/* For now i don't believe there is any cleanup to do..  All of the 
	 * Semaphore operations are done with SEM_UNDO, so they will be undone
	 * when the process dies.  
	 *
	 * TODO: this may not be enough, revisit this when error handling matures.
	 */
	
}


bool SendTupleChunkToAMS(TupleChunkListItem tcItem)
{
	int currSpaceAvail = getSpaceAvail( outputQueue );			
	
	if( currSpaceAvail >= tcItem->chunk_length )
	{			
		//send away
		writeBytes( outputQueue,  tcItem->chunk_data, tcItem->chunk_length );		
		
		//post that data is avail
		SemPost( semId );
		
		//success
		return true;		
	}
	else
	{
		//not enough room...
		return false;
	}
	
}


/* IMPORTANT:  This function assumes that the inputQueue can only ever
 * have an integral number of chunks in the queue at any given time.
 *
 */
TupleChunkListItem RecvTupleChunkFromAMS(void)
{
	int dataToRead = getDataAvail( inputQueue );

	//short circuit exit if we have nothing to do.
	if( dataToRead == 0 )
		return NULL;
	
	//if there is data, it should at least be a tuple chunk header size.
	Assert( dataToRead >= TUPLE_CHUNK_HEADER_SIZE );
	
	
	TupleChunkListItem firstTcItem = NULL;
	TupleChunkListItem lastTcItem = NULL;
	TupleChunkListItem tcItem = NULL;
	int bytesRead = 0;
	
	while( bytesRead != dataToRead )
	{
		//we should never read to much
		Assert( bytesRead <= dataToRead );
				
		peekBytes( inputQueue, hdr, TUPLE_CHUNK_HEADER_SIZE );		
		uint16 totalChunkSize = TUPLE_CHUNK_HEADER_SIZE + GetChunkDataSize( hdr );
		
		
		tcItem = (TupleChunkListItem)palloc0( sizeof(TupleChunkListData) + 
											  totalChunkSize);
		tcItem->chunk_length = totalChunkSize;
		readBytes( inputQueue, tcItem->chunk_data, totalChunkSize );
		
		bytesRead += totalChunkSize;
		
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
	
	return firstTcItem;
}
