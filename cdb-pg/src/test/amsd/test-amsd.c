/*-------------------------------------------------------------------------
 *
 * test-circbuff.c
 *     This source-file contains unit-tests for the Circular Buffer
 *     datastructure that is used to transfer data between SegmentDatabase
 *     and amsd via Shared Memory.
 *
 * $Header$
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <assert.h>
#include <stdio.h>

#include "cdb/tupchunk.h"
#include "cdb/circbuff.h"
#include "cdb/shmutils.h"
#include "testframe.h"


void testBasicAmsdOperation(void);
void testBroadcast(void);


int main(int argc, char **argv)
{

    RUN_TEST(testBasicAmsdOperation);
    RUN_TEST(testBroadcast);

//Error:
    return 0;
}


void testBasicAmsdOperation(void)
{
	int cbuff_size = QSIZE + sizeof(circbuff);
	//int shmem_size = 2 * NUM_SEGS * (cbuff_size + sizeof(circbuff)); 	
	
	char *shm = ShmemAttach( SHMEM_KEY );
	int semId = SemGet( SEM_KEY );
	
	circbuff *inputQueues[NUM_SEGS];
	circbuff *outputQueues[NUM_SEGS];
	
	int i;
	for( i=0; i < NUM_SEGS; i++ )
	{
		inputQueues[i] = (circbuff*)(shm + (2*i*cbuff_size));
		outputQueues[i] = (circbuff*)(shm + ( ( (2*i) +1) * cbuff_size ));
	}
	
	uint8 tupleOut[100];
	uint8 tupleIn[100];
	
	int targetSegId = 2;
	int queryGroupId = 3;
	int srcSegId = 1;
	int motionNodeId = 5;
	int chunkDataSize = 10;
	int chunkType = TC_WHOLE;
	
	//send a tuple out on SegId=1 outputQueue and send it to SegId=2
	SetTargetSegIdx( tupleOut, targetSegId );
	SetQueryGroupID( tupleOut, queryGroupId );
	SetSourceSegIdx( tupleOut, srcSegId );
	SetMotionNodeID( tupleOut, motionNodeId );
	SetChunkDataSize( tupleOut, chunkDataSize );
	SetChunkType( tupleOut, chunkType );
	
	bool sent = false;
	
	while( !sent )
	{
		if( getSpaceAvail( outputQueues[0] ) >= (TUPLE_CHUNK_HEADER_SIZE+10) )
		{
			//enough space.. so send away
			int bytesWrote = writeBytes( outputQueues[0], tupleOut, (TUPLE_CHUNK_HEADER_SIZE+10) );
 	

			//post
			SemPost( semId );
			
			sent = true;
		}
	}
	
	//now we need to read it in
	bool read = false;
	while( !read )
	{
		if( getDataAvail( inputQueues[1] ) >= (TUPLE_CHUNK_HEADER_SIZE+10) )
		{
			readBytes( inputQueues[1], tupleIn, (TUPLE_CHUNK_HEADER_SIZE+10)	);
			read = true;
		}
		else
		{
			sleep(1);
		}
		
	}
	
	
	//now make sure things are equal
	
	
	VERIFY( GetTargetSegIdx(tupleOut) == GetTargetSegIdx(tupleIn) );
	VERIFY( GetQueryGroupID(tupleOut) == GetQueryGroupID(tupleIn) );
	VERIFY( GetSourceSegIdx(tupleOut) == GetSourceSegIdx(tupleIn) );
	VERIFY( GetMotionNodeID(tupleOut) == GetMotionNodeID(tupleIn) );
	VERIFY( GetChunkDataSize(tupleOut) == GetChunkDataSize(tupleIn) );
	VERIFY( GetChunkType(tupleOut) == GetChunkType(tupleIn) );
	
}

void testBroadcast(void)
{
    int cbuff_size = QSIZE + sizeof(circbuff);
	//int shmem_size = 2 * NUM_SEGS * (cbuff_size + sizeof(circbuff)); 	
	
	char *shm = ShmemAttach( SHMEM_KEY );
	int semId = SemGet( SEM_KEY );
	
	circbuff *inputQueues[NUM_SEGS];
	circbuff *outputQueues[NUM_SEGS];
	
	int i;
	for( i=0; i < NUM_SEGS; i++ )
	{
		inputQueues[i] = (circbuff*)(shm + (2*i*cbuff_size));
		outputQueues[i] = (circbuff*)(shm + ( ( (2*i) +1) * cbuff_size ));
	}
	
	uint8 tupleOut[100];
	uint8 tupleIn[100];
	
	int targetSegId = BROADCAST_SEGIDX;
	int queryGroupId = 3;
	int srcSegId = 1;
	int motionNodeId = 5;
	int chunkDataSize = 10;
	int chunkType = TC_WHOLE;
	
	//send a tuple out on SegId=1 outputQueue and send it to SegId=2
	SetTargetSegIdx( tupleOut, targetSegId );
	SetQueryGroupID( tupleOut, queryGroupId );
	SetSourceSegIdx( tupleOut, srcSegId );
	SetMotionNodeID( tupleOut, motionNodeId );
	SetChunkDataSize( tupleOut, chunkDataSize );
	SetChunkType( tupleOut, chunkType );
	
	bool sent = false;
	
	while( !sent )
	{
		if( getSpaceAvail( outputQueues[0] ) >= (TUPLE_CHUNK_HEADER_SIZE+10) )
		{
			//enough space.. so send away
			int bytesWrote = writeBytes( outputQueues[0], tupleOut, (TUPLE_CHUNK_HEADER_SIZE+10) );
 	

			//post
			SemPost( semId );
			
			sent = true;
		}
	}
	
	//now we will go through and try and read it from all the other queues
	
	for(i=1; i < NUM_SEGS; i++){
	   //now we need to read it in
        bool read = false;
        while( !read )
        {
            if( getDataAvail( inputQueues[i] ) >= (TUPLE_CHUNK_HEADER_SIZE+10) ){
                readBytes(inputQueues[i], tupleIn, (TUPLE_CHUNK_HEADER_SIZE+10));
                read = true;
            }
            else{
                sleep(1);
            }            
        }
                
        //now make sure things are equal                
        VERIFY( GetTargetSegIdx(tupleOut) == GetTargetSegIdx(tupleIn) );
        VERIFY( GetQueryGroupID(tupleOut) == GetQueryGroupID(tupleIn) );
        VERIFY( GetSourceSegIdx(tupleOut) == GetSourceSegIdx(tupleIn) );
        VERIFY( GetMotionNodeID(tupleOut) == GetMotionNodeID(tupleIn) );
        VERIFY( GetChunkDataSize(tupleOut) == GetChunkDataSize(tupleIn) );
        VERIFY( GetChunkType(tupleOut) == GetChunkType(tupleIn) );   
		
	}
	
	
}

