/*-------------------------------------------------------------------------
 *
 * circbuff.h
 *     A single-reader single-writer circular buffer data structure.
 *   
 * 
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
#ifndef CIRCBUFF_H
#define CIRCBUFF_H

#include "postgres_fe.h"

#include <stdbool.h>
#include <signal.h>


//get the size of the 3 fields in front of the buffer.
#define HEADER_SIZE (sizeof(unsigned int) + 2*sizeof(sig_atomic_t))


typedef struct circbuff{  
    unsigned int  size;
    sig_atomic_t  head;        //read data from here
    sig_atomic_t  tail;        //write data starting here    
    uint8         buffer[];
} circbuff;





/* Making, destroying, and resizing queues */

/*
 * Used for initializing the CircBuff datastructure passed in.
 * NOTE:  this does not allocate memory, that should be done *before*
 * calling this method.  
 *
 * param cb - pointer to the allocated circbuff.
 * param size - the size of the circbuff buffer in bytes.
 */
extern void initCircBuff( circbuff *cb, unsigned int size );

extern char * getDataStart( circbuff *cb );

/* Returns the total size of the circbuff datastructure in bytes including
 * the buffer space.
 */
extern unsigned int totalCircBuffSize( circbuff *cb );

/* Data operations */

/* Returns the number of bytes that can be read out of the buffer. */
extern unsigned int getDataAvail( circbuff *cb );

/* Returns the number of bytes that can be written into the buffer. */
extern unsigned int getSpaceAvail( circbuff *cb );

/* If there is enough space available, this function writes "length" bytes
 * into the circbuff starting from "bytes".  The function returns the number
 * of bytes written into the circbuff.  This should be either length if the
 * write was successful, or 0 if there was not enough space to write the entire
 * message.
 */
extern int writeBytes( circbuff *cb, char* bytes, int length );

/* If there is enough space available, this function reads "legnth" bytes
 * into "msg".  The function returns the number of bytes read out of circbuff
 * and written into "msg".  This should be either "length" if the read was 
 * successful, or 0 if there was not enough data in circbuff to read.
 */
extern int readBytes( circbuff *cb, char* msg, int length );

#endif /*CIRCBUFF_H */
