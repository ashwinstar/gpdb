/*-------------------------------------------------------------------------
 *
 * circbuff.c
 *    A single-reader single-writer circular buffer data structure.
 * 
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
 
 #include <stdlib.h>
 #include <assert.h>
 
 #include "cdb/circbuff.h"
 #include "cdb/shmutils.h"
 

void initCircQueue( circbuff *cb, unsigned int size )
{
    cb->head = 0;
	cb->tail = 0;
    cb->size = size;  
    
}


unsigned int totalCircQueueSize( circbuff *cb )
{
	return HEADER_SIZE + cb->size;	
}

char * getDataStart( circbuff *cb)
{
    return (char*) cb + HEADER_SIZE;
}


unsigned int getDataAvail( circbuff *cb )
{
    int size;
    
    if( cb != NULL )
    {
        size = (cb->size + cb->tail - cb->head) % cb->size;
    }
    else
        size = 0;
        
    
    return size;    
}



unsigned int getSpaceAvail( circbuff *cb )
{
    int size;
    
    if( cb != NULL )
    {
        size = (cb->size + cb->head - cb->tail - 1 ) % cb->size;
    }
    else
        size = 0;
    
    return size;    
}


void updateBytesRead( circbuff *cb, int bytesRead )
{
    cb->head =  (cb->head + bytesRead) % cb->size;  
}


void updateBytesWritten( circbuff *cb, int bytesWritten )
{
    cb->tail =  (cb->tail + bytesWritten) % cb->size;   
}

