/*-------------------------------------------------------------------------
 *
 * shmbufftest.c
 *      Unit Test for testing the Shared Memory Circular Buffer code.
 *     
 *   
 * 
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>

#include "cdb/circbuff.h"
#include "cdb/shmutils.h"



#define MODE_RECEIVE 1
#define MODE_SEND 2
#define MODE_DELETE 3

#define MODE_RECEIVE_STR "r"
#define MODE_SEND_STR    "s"
#define MODE_DELETE_STR	 "d"


#define SHMEM_KEY 42
#define SEM_KEY 43


#define SHMEM_SIZE  64000

void usage(int exitCode);
void doSend(int numToSend, int sizeOfMsg );
void doReceive(void);
void doDelete(void);


int main( int argc, char *argv[])
{
    int op_mode;
	
	if( argc < 2 )
	    usage(-1);

	char *mode = argv[1];
		
	if (strcmp(mode, MODE_RECEIVE_STR) == 0) 
    	op_mode = MODE_RECEIVE;
    else if (strcmp(mode, MODE_SEND_STR) == 0) 
       	op_mode = MODE_SEND;
    else if (strcmp(mode, MODE_DELETE_STR) == 0)
        op_mode = MODE_DELETE;
    else 
    {    
    	printf("Didn't recognize mode-string '%s'.\n\n", mode);
        usage(-1);
    }

    if( op_mode == MODE_SEND )
    {
        int numToSend = atoi( argv[2] );
        int sizeOfMsg = atoi( argv[3] );
        doSend( numToSend, sizeOfMsg );
    }
    else if( op_mode == MODE_RECEIVE )
        doReceive();
    else if( op_mode == MODE_DELETE )
        doDelete();
    else
    {
    }
    
    exit(0);

}
 
 
/* Print the test program's usage. */
void usage(int exitCode)
{
    printf("shmbufftest\n\n");
    printf("Usage: shmbufftest <mode> \n");
    printf("\tmode is 'r' to receive, 's' to send, 'd' to delete\n");
    exit(exitCode);
}

void doSend( int numToSend, int sizeOfMsg )
{
    int semid;
    
    //setup the shared memory stuff
    char *shm = ShmemCreate( (key_t)SHMEM_KEY, SHMEM_SIZE + sizeof(circbuff) );
    
    if( shm == NULL )
    {        
        perror("error creating shmem segment:");
        exit(1);
    }
    else if( shm == (char *) -1 )
    {
        perror("error attaching to shmem:");
        exit(1);        
    }

    if( (semid = SemCreate(SEM_KEY) ) < 0 )
    {
        perror("error creating Semaphore:");
        exit(1);
    }
    
    if( SemInit( semid, 0 ) < 0 )
    {
        perror("error initializing Semaphore:");
        exit(1);
    }
  
    /*
     * Now read what the server put in the memory.
     */
    circbuff *cb;		
    cb = (circbuff*)shm;
    
    initCircBuff( cb, SHMEM_SIZE );
    

    //generate a some data to send over and over and over...
    
    char *msg = malloc( sizeOfMsg );
    
    int i;
    for( i=0; i < sizeOfMsg; i++ )
    {
        //TODO: create better test messages.
        *(msg+i) = 'a';
    }

    
    
    struct timeval startTime;
    struct timeval endTime;
    int spaceAvail;
    bool tupSent = false;
    
    gettimeofday(&startTime, NULL);
    
    for( i=0; i < numToSend; i++ )
    {
        
        while( !tupSent )
        {
            
            spaceAvail = getSpaceAvail( cb );
            
            if( spaceAvail >= sizeOfMsg )
            {
				int bytesWritten = writeBytes( cb, msg, sizeOfMsg );
				
				if (bytesWritten == sizeOfMsg )
					tupSent = true;
            }
            else
            {
                //for now we are stupid... so we spin!
            }
            
        }
        
        tupSent = false;
    }
    
    //send a signal byte to indicate we are done
    while( getSpaceAvail( cb ) == 0 )
    {
        //do nothing...
    }
    
	char eos_msg = '\1';
	writeBytes(cb, &eos_msg, 1 );
    
    gettimeofday(&endTime, NULL);
    
    float sec = endTime.tv_sec - startTime.tv_sec;
    float usec = endTime.tv_usec - startTime.tv_usec;
    float elapsed = elapsed = (float)sec + ((float)usec/(float)1000000.0);
  
    
    long totalBytes = numToSend * sizeOfMsg;
    double totalKBytes = totalBytes / 1024;


    printf( "Sending took: %f seconds to send: %f MB of msg data\n", (double)elapsed, totalKBytes/1024 );
    printf( "Msg Data Bandwidth: %f MB/sec\n", (double)( totalKBytes/1024 / elapsed ) );
    
}


void doReceive()
{
    int semid;
    
    //setup the shared memory stuff
    char * shm = ShmemAttach( SHMEM_KEY);
    
    if( shm == NULL )
    {        
        perror("error creating shmem segment:");
        exit(1);
    }
    else if( shm == (char *) -1 )
    {
        perror("error attaching to shmem:");
        exit(1);        
    }

    if( (semid = SemGet( SEM_KEY )) < 0 )
    {
        perror("error creating Semaphore:");
        exit(1);
    }
    
 
    /*
     * setup the pre-existing circbuff
     */
    circbuff *cb;		
    cb = (circbuff*)shm;
    
    
    //now its time to read!
    struct timeval startTime;
    struct timeval endTime;
    
    long totalBytes = 0;
    
    gettimeofday(&startTime, NULL);
    
    bool done = false;
    
    int bytesToRead;
    
    char *msg = malloc( cb->size );
    while( !done )
    {
               
        bytesToRead = getDataAvail( cb );


	    //printf("bytesToRead:%d\n", bytesToRead );
        
        if( bytesToRead > 0 )
        {
			readBytes( cb, msg, bytesToRead );
			totalBytes += bytesToRead;
            
            //check the last byte read in to see if its the end            
            if( *(msg + bytesToRead - 1) == '\1' )
                done = true;
           
        }  
		//usleep(50000);
    }
    
    gettimeofday(&endTime, NULL);
    
    float sec = endTime.tv_sec - startTime.tv_sec;
    float usec = endTime.tv_usec - startTime.tv_usec;
    float elapsed = elapsed = (float)sec + ((float)usec/(float)1000000.0);
    
    double totalKBytes = totalBytes / 1024;
    double totalMBytes = totalKBytes / 1024;
  
    printf( "Receiving took: %f seconds to get: %f MB of msg data\n", (double)elapsed, totalMBytes );
    printf( "Msg Data Bandwidth: %f MB/sec\n", (double)( totalMBytes / elapsed ) );
    
    
}

void doDelete()
{       
    if( ShmemRemove( SHMEM_KEY ) < 0 )
        perror("couldn't delete shmem seg:");
        
    if( SemRemove( SEM_KEY ) < 0 )
        perror("couldn't delete shmem semaphore:");
}
