/*-------------------------------------------------------------------------
 *
 * cdbbufferedread.h
 *	  Read buffers sequentially from a file efficiently.
 *
 * The client is given direct access to large read buffer for reading
 * buffers efficiency.
 *        
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBBUFFEREDREAD_H
#define CDBBUFFEREDREAD_H

#include "postgres.h"
#include "storage/fd.h"

typedef struct BufferedRead
{
	/*
	 * Large-read memory level members.
	 */
    int32      			 maxBufferLen;
    int32      			 largeReadLen;
	
	uint8                *memory;
    int32                memoryLen;

    uint8                *beforeBufferMemory;
    uint8                *largeReadMemory;

	int64				 position;
    int32                readLen;
	
	/*
	 * Buffer level members.
	 */	
	int32                bufferOffset;
	int32                bufferLen;

	/*
	 * File level members.
	 */
	File 				 file;
    char				 *filePathName;
    int64                fileLen;
} BufferedRead;

/*
 * Determines the amount of memory to supply for
 * BufferedRead given the desired buffer and
 * large read lengths.
 */
extern int32 BufferedReadMemoryLen(
    int32                maxBufferLen,
    int32                largeReadLen);

/*
 * Initialize BufferedRead.
 *
 * Use the BufferedReadMemoryLen procedure to
 * determine the amount of memory to supply.
 */
extern void BufferedReadInit(
    BufferedRead         *bufferedRead,
    uint8                *memory,
    int32                memoryLen,
    int32                maxBufferLen,
    int32                largeReadLen);

/*
 * Takes an open file handle for the next file.
 */
extern void BufferedReadSetFile(
    BufferedRead         *bufferedRead,
    File 				 file,
    char				 *filePathName,
    int64                fileLen);

/*
 * Return the position of the next read buffer in bytes.
 */
extern int64 BufferedReadNextBufferPosition(
    BufferedRead       *bufferedRead);

/*
 * Get the next, maximum buffer space for reading.
 *
 * Returns NULL when the current file has been completely read.
 */
extern uint8 *BufferedReadGetMaxBuffer(
    BufferedRead       *bufferedRead,
    int32              *nextBufferLen);

/*
 * Get the next buffer space for reading with a specified max read-ahead
 * amount.
 *
 * Returns NULL when the current file has been completely read.
 */
extern uint8 *BufferedReadGetNextBuffer(
    BufferedRead       *bufferedRead,
    int32              maxReadAheadLen,
    int32              *nextBufferLen);

/*
 * Grow the available length of the current buffer.
 *
 * NOTE: The buffer address returned can be different, even for previously
 * examined buffer data.  In other words, don't keep buffer pointers in
 * the buffer region.  Use offsets to re-establish pointers after this call.
 *
 * If the current file has been completely read, availableLen will remain
 * the current value.
 */
uint8 *BufferedReadGrowBuffer(
    BufferedRead       *bufferedRead,
    int32              newMaxReadAheadLen,
    int32              *growBufferLen);

/*
 * Return the address of the current read buffer.
 */
uint8 *BufferedReadGetCurrentBuffer(
    BufferedRead       *bufferedRead);

/*
 * Return the current buffer's start position.
 */
int64 BufferedReadCurrentPosition(
    BufferedRead       *bufferedRead);

/*
 * Finishes the current file for reading.  Caller is resposible for closing
 * the file afterwards.
 */
extern void BufferedReadCompleteFile(
    BufferedRead       *bufferedRead);

/*
 * Finish with reading all together.
 */
extern void BufferedReadFinish(
    BufferedRead *bufferedRead);

#endif   /* CDBBUFFEREDREAD_H */

