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
extern void initCircQueue( circbuff *cb, unsigned int size );
extern unsigned int totalCircQueueSize( circbuff *cb );
extern char * getDataStart( circbuff *cb);

/* Data operations */


extern void updateBytesRead( circbuff *cb, int bytesRead ); 
extern void updateBytesWritten( circbuff *cb, int bytesWritten );

extern unsigned int getDataAvail( circbuff *cb );
extern unsigned int getSpaceAvail( circbuff *cb );

#endif /*CIRCBUFF_H */
