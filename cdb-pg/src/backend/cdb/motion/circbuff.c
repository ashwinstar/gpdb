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
 
/* initializes the circbuff datastructure. */
void initCircBuff( circbuff *cb, unsigned int size )
{
    cb->head = 0;
	cb->tail = 0;
    cb->size = size;
	//cb->buffer = &(cb->tail) + sizeof(sig_atomic_t);   
}


char * getDataStart( circbuff *cb )
{
	return ((char*)cb) + HEADER_SIZE;
}


/* Returns the total size of the circbuff in bytes.
 */
unsigned int totalCircBuffSize( circbuff *cb )
{
	return HEADER_SIZE + cb->size;	
}



/* Returns the number of bytes that can be read out of the buffer.
 */
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


/* Returns the number of bytes that can be written into the buffer. 
 */
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


/* Writes "length" bytes from msg into circbuff and returns the number of bytes
 * written.  This function is all or nothing so the number of bytes written 
 * should either be "length" or 0.
 */
int writeBytes( circbuff *cb, char* msg, int length )
{
	int spaceAvail = getSpaceAvail( cb );
	
	//safetycheck
	if( spaceAvail < length || spaceAvail == 0 )
		return 0;
	
	//so we have enough room...send away
	char *dstTail = cb->buffer + cb->tail;
	
	if( length < ( cb->size - cb->tail) )
		memcpy( dstTail, msg, length );
	else
	{			
		int firstWrite = cb->size - cb->tail;
		memcpy( dstTail, msg, firstWrite);
		memcpy( cb->buffer, msg+firstWrite, length-firstWrite);                     
	}
			
	//update the tail
	cb->tail =  (cb->tail + length) % cb->size;
	
	return length;	
}


/* Reads "length" bytes from the circbuff and writes them to "msg."
 * This is all or none, so if there aren't "length" bytes to read,
 * then this does nothing.
 *
 * Returns the number of bytes read.
 */
int readBytes( circbuff *cb, char* msg, int length )
{
	int bytesToRead = getDataAvail( cb );
	
	//safety check...
	if( bytesToRead < length || length < 0 )
		return 0;
	
	char *dstHead = cb->buffer + cb->head;
	
	if( length < ( cb->size - cb->head) )
		memcpy( msg, dstHead, length );
	else
	{
		//wrap around necessary, split into 2 memcpy's
		int firstRead = cb->size - cb->head;
		memcpy( msg, dstHead, firstRead);
		memcpy( msg+firstRead, cb->buffer, length-firstRead);			
	}
	
	//update the head
	cb->head =  (cb->head + length) % cb->size;
	
	return length;

}

