/*-------------------------------------------------------------------------
 *
 * cdbbufferedappend.h
 *	  Write buffers to the end of a file efficiently.
 *
 * The client is given direct access to the write buffer for appending
 * buffers efficiency.
 *        
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBBUFFEREDAPPEND_H
#define CDBBUFFEREDAPPEND_H

#include "postgres.h"
#include "storage/fd.h"

typedef struct BufferedAppend
{
	/*
	 * Large-write memory level members.
	 */
    int32      			 maxBufferLen;
    int32      			 largeWriteLen;

	uint8                *memory;
    int32                memoryLen;

    uint8                *largeWriteMemory;
    uint8                *afterBufferMemory;
	
	int64				 position;
    int32                len;
	
	/*
	 * Buffer level members.
	 */	
    int32                bufferLen;

	/*
	 * File level members.
	 */
	File 				 file;
    int64                fileLen;
} BufferedAppend;

/*
 * Determines the amount of memory to supply for
 * BufferedAppend given the desired buffer and
 * large write lengths.
 */
extern int32 BufferedAppendMemoryLen(
    int32                maxBufferLen,
    int32                largeWriteLen);

/*
 * Initialize BufferedAppend.
 *
 * Use the BufferedAppendMemoryLen procedure to
 * determine the amount of memory to supply.
 */
extern void BufferedAppendInit(
    BufferedAppend       *bufferedAppend,
    uint8                *memory,
    int32                memoryLen,
    int32                maxBufferLen,
    int32                largeWriteLen);

/*
 * Takes an open file handle for the next file.
 */
extern void BufferedAppendSetFile(
    BufferedAppend       *bufferedAppend,
    File 				 file,
    int64				 eof);

/*
 * Return the position of the next write buffer in bytes.
 */
extern int64 BufferedAppendNextBufferPosition(
    BufferedAppend     *bufferedAppend);

/*
 * Get the next buffer space for appending with a specified length.
 *
 * Returns NULL when the current file does not have enough
 * room for another buffer.
 */
extern uint8 *BufferedAppendGetBuffer(
    BufferedAppend       *bufferedAppend,
    int32				 bufferLen);

/*
 * Get the next maximum length buffer space for appending.
 *
 * Returns NULL when the current file does not have enough
 * room for another buffer.
 */
extern uint8 *BufferedAppendGetMaxBuffer(
    BufferedAppend       *bufferedAppend);

/*
 * Indicate the current buffer is finished.
 */
extern void BufferedAppendFinishBuffer(
    BufferedAppend       *bufferedAppend,
    int32                usedLen);

/*
 * Finish the current buffer and get the next
 * buffer for appending.
 *
 * Returns NULL when the current file does not have enough
 * room for another buffer.
 */
extern uint8 *BufferedAppendMoveToNextBuffer(
    BufferedAppend       *bufferedAppend,
    int32                usedLen);


/*
 * Returns the current file’s length.
 */
extern int64 BufferedAppendFileLen(
    BufferedAppend *bufferedAppend);

/*
 * Flushes the current file for append.  Caller is resposible for closing
 * the file afterwards.
 *
 * Returns the file length.
 */
extern int64 BufferedAppendCompleteFile(
    BufferedAppend       *bufferedAppend);


/*
 * Finish with writing all together.
 */
extern void BufferedAppendFinish(
    BufferedAppend *bufferedAppend);

#endif   /* CDBBUFFEREDAPPEND_H */
