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
#include "testframe.h"

#define QSIZE 10000

void testBasicBufferOperation(void);
void testWrapAround(void);
void testLimitWrite(void);
void initBuffCheck( circbuff *cb, int qsize );



int main(int argc, char **argv)
{

    RUN_TEST(testBasicBufferOperation);
    RUN_TEST(testWrapAround);
    RUN_TEST(testLimitWrite);
    
//Error:    
    return 0;
}


/* This test sends and receives the DtmVersionMsg message. */
void testBasicBufferOperation(void)
{
    //create a new circbuff datastructure   
    circbuff *cb = malloc( sizeof(circbuff) + QSIZE );
    initCircBuff( cb, QSIZE );
    
    initBuffCheck( cb, QSIZE );

    
    //let's write a msg
    char *msg = "hello";    
    writeBytes( cb, msg, 6 );
    
    VERIFY( getSpaceAvail( cb ) == (QSIZE - 1 - 6) );
    VERIFY( getDataAvail( cb ) == 6 );
    
    //let's read our message
    
    char *readMsg = malloc( 6 );
    int bytesRead = readBytes( cb, readMsg, 6 );
    
    VERIFY( bytesRead == 6 );    
    VERIFY( strncmp( msg, readMsg, 6 ) == 0 );
    VERIFY( getSpaceAvail(cb) == (QSIZE - 1 ) );
    VERIFY( getDataAvail(cb) == 0 );
    
    
    
    free(cb);
    free( readMsg );
}

void testWrapAround(void)
{
    int qsize = 5;
    
     circbuff *cb = malloc( sizeof(circbuff) + qsize );
     initCircBuff( cb, qsize );
     
     //check the init
     initBuffCheck( cb, qsize );
     
     char *msg = "1111";
     char *msg2 = "000";
     
     char *msgRead = malloc(6);
     
     int workDone;
     
     workDone = writeBytes( cb, msg, 4 );
     workDone = readBytes( cb, msgRead, 3 );
     workDone = writeBytes( cb, msg2, 3 );
     workDone = readBytes( cb, msgRead, 4 );
     
     VERIFY( strncmp( "1000", msgRead, 4 ) == 0 );
     
     free( cb); 
     free( msgRead );
     
}

void testLimitWrite(void)
{
    int qsize = 5;
    
     circbuff *cb = malloc( sizeof(circbuff) + qsize );
     initCircBuff( cb, qsize );
     
     //check the init
     initBuffCheck( cb, qsize );
    
     char *msg = "1111";
     char *msg2 = "0000";
     char *msgRead = malloc( 6 );
    
     int workDone;
     
     workDone = writeBytes( cb, msg, 4 );
     workDone = readBytes( cb, msgRead, 4 );
     
     VERIFY( strcmp( msg, msgRead ) == 0) ;
     
     workDone = writeBytes( cb, msg2, 4 );          
     workDone = readBytes( cb, msgRead, 4 );        
     VERIFY( strcmp( msg2, msgRead) == 0 );
     
     
     workDone = writeBytes( cb, msg, 4 );          
     workDone = readBytes( cb, msgRead, 4 );        
     VERIFY( strcmp( msg, msgRead) == 0 );
     
     free( cb );
     free( msgRead );
     
}


void initBuffCheck( circbuff *cb, int qsize )
{
    VERIFY( cb->size == qsize);
    VERIFY( cb->head == 0 );
    VERIFY( cb->tail == 0 );
    VERIFY( getDataAvail( cb ) == 0 );
    VERIFY( getSpaceAvail( cb ) == (qsize - 1) );
}

