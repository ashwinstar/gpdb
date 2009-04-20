/*-------------------------------------------------------------------------
 *
 * cdbbufferedappend.c
 *	  Write buffers to the end of a file efficiently.
 *
 * (See .h file for usage comments)
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "cdb/cdbbufferedappend.h"
#include <unistd.h>				/* for write() */

static void BufferedAppendWrite(
    BufferedAppend        *bufferedAppend);

/*
 * Determines the amount of memory to supply for
 * BufferedAppend given the desired buffer and
 * large write lengths.
 */
int32 BufferedAppendMemoryLen(
    int32                maxBufferLen,
    int32                largeWriteLen)
{
	Assert(maxBufferLen > 0);
	Assert(largeWriteLen >= maxBufferLen);

	// Large write memory areas plus adjacent extra memory for 1 buffer.
	return (largeWriteLen + maxBufferLen);
}

/*
 * Initialize BufferedAppend.
 *
 * Use the BufferedAppendMemoryLen procedure to
 * determine the amount of memory to supply.
 */
void BufferedAppendInit(
    BufferedAppend       *bufferedAppend,
    uint8                *memory,
    int32                memoryLen,
    int32                maxBufferLen,
    int32                largeWriteLen)
{
	Assert(bufferedAppend != NULL);
	Assert(memory != NULL);
	Assert(maxBufferLen > 0);
	Assert(largeWriteLen >= maxBufferLen);
	Assert(memoryLen >= BufferedAppendMemoryLen(maxBufferLen, largeWriteLen));
	
	memset(bufferedAppend, 0, sizeof(BufferedAppend));

	/*
	 * Large-read memory level members.
	 */
    bufferedAppend->maxBufferLen = maxBufferLen;
    bufferedAppend->largeWriteLen = largeWriteLen;

	bufferedAppend->memory = memory;
    bufferedAppend->memoryLen = memoryLen;
	
	bufferedAppend->largeWriteMemory = memory;
    bufferedAppend->afterBufferMemory = 
						&memory[largeWriteLen];

	bufferedAppend->position = 0;
	bufferedAppend->len = 0;

	/*
	 * Buffer level members.
	 */	
	bufferedAppend->bufferLen = 0;

	/*
	 * File level members.
	 */
	bufferedAppend->file = -1;
	bufferedAppend->filePathName = NULL;
    bufferedAppend->fileLen = 0;
}

/*
 * Takes an open file handle for the next file.
 * 
 * Note that eof_uncompressed is used only for storing the
 * uncompressed file size in the catalog, so that the compression
 * ratio could be calculated at the user's request.
 */
void BufferedAppendSetFile(
    BufferedAppend       *bufferedAppend,
    File 				 file,
    char				 *filePathName,
    int64				 eof,
	int64				 eof_uncompressed)
{
	Assert(bufferedAppend != NULL);
    Assert(bufferedAppend->position == 0);
    Assert(bufferedAppend->len == 0);
    Assert(bufferedAppend->bufferLen == 0);
	Assert(bufferedAppend->file == -1);
	Assert(bufferedAppend->fileLen == 0);
	Assert(bufferedAppend->fileLen_uncompressed == 0);

	Assert(file >= 0);
	Assert(eof >= 0);

	bufferedAppend->position = eof;
	bufferedAppend->file = file;
	bufferedAppend->filePathName = filePathName;
	bufferedAppend->fileLen = eof;
	bufferedAppend->fileLen_uncompressed = eof_uncompressed;
}


/*
 * Perform a large write i/o.
 */
static void BufferedAppendWrite(
    BufferedAppend      *bufferedAppend)
{
	int32 writeLen;
	uint8 *largeWriteMemory;

	writeLen = bufferedAppend->len;
	Assert(bufferedAppend->len > 0);
	largeWriteMemory = bufferedAppend->largeWriteMemory;

#ifdef USE_ASSERT_CHECKING
	{
		int64 currentWritePosition; 

		currentWritePosition = FileNonVirtualCurSeek(bufferedAppend->file);
		if (currentWritePosition < 0)
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("unable to get current position for file \"%s\" (errcode %d)",
							       bufferedAppend->filePathName,
								   errno)));

		if (currentWritePosition != bufferedAppend->position)
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("Current position mismatch actual "
								   INT64_FORMAT ", expected " INT64_FORMAT " in file \"%s\"",
								   currentWritePosition, bufferedAppend->position,
								   bufferedAppend->filePathName)));
	}
#endif	

	while (writeLen > 0) 
	{
		int actualLen = FileWrite(
							bufferedAppend->file,
							(char*)largeWriteMemory,
							writeLen);

		if (actualLen == 0) 
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("no disk space for file \"%s\"",
							       bufferedAppend->filePathName)));
		else if (actualLen < 0)
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("unable to write file \"%s\" (errcode %d)", 
								   bufferedAppend->filePathName,
								   errno)));
		writeLen -= actualLen;
		largeWriteMemory += actualLen;
	}
	
	bufferedAppend->position += bufferedAppend->len;
	bufferedAppend->len = 0;

}

/*
 * Return the position of the current write buffer in bytes.
 */
int64 BufferedAppendCurrentBufferPosition(
    BufferedAppend     *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);
	
	return bufferedAppend->position;
}

/*
 * Return the position of the next write buffer in bytes.
 */
int64 BufferedAppendNextBufferPosition(
    BufferedAppend     *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);
	
	return bufferedAppend->position +
		   bufferedAppend->len;
}

/*
 * Get the next buffer space for appending with a specified length.
 *
 * Returns NULL when the current file does not have enough
 * room for another buffer.
 */
uint8 *BufferedAppendGetBuffer(
    BufferedAppend       *bufferedAppend,
    int32				 bufferLen)
{
	int32 currentLargeWriteLen;
	
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);
	if (bufferLen > bufferedAppend->maxBufferLen)
		elog(ERROR,
		     "bufferLen %d greater than maxBufferLen %d at position " INT64_FORMAT,
		     bufferLen, bufferedAppend->maxBufferLen, bufferedAppend->position);

	/*
	 * Let next buffer carry-over into the extra buffer space after
	 * the large write buffer.
	 */
	currentLargeWriteLen = bufferedAppend->len;
	Assert (currentLargeWriteLen + bufferLen <=
										bufferedAppend->largeWriteLen +
						                bufferedAppend->maxBufferLen);

	bufferedAppend->bufferLen = bufferLen;

	return &bufferedAppend->largeWriteMemory[currentLargeWriteLen];
}

/*
 * Get the address of the current buffer space being used appending.
 */
uint8 *BufferedAppendGetCurrentBuffer(
    BufferedAppend       *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);

	return &bufferedAppend->largeWriteMemory[bufferedAppend->len];
}

/*
 * Get the next maximum length buffer space for appending.
 *
 * Returns NULL when the current file does not have enough
 * room for another buffer.
 */
uint8 *BufferedAppendGetMaxBuffer(
    BufferedAppend       *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);

	return BufferedAppendGetBuffer(
						bufferedAppend,
						bufferedAppend->maxBufferLen);
}

void BufferedAppendCancelLastBuffer(
    BufferedAppend       *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);

	bufferedAppend->bufferLen = 0;
}

/*
 * Indicate the current buffer is finished.
 */
void BufferedAppendFinishBuffer(
    BufferedAppend       *bufferedAppend,
    int32                usedLen,
    int32				 usedLen_uncompressed)
{
	int32 newLen;
	
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);
	if (usedLen > bufferedAppend->bufferLen)
		elog(ERROR,
		     "Used length %d greater than bufferLen %d at position " INT64_FORMAT,
		     usedLen, bufferedAppend->bufferLen, bufferedAppend->position);
		     

	newLen = bufferedAppend->len + usedLen;
	Assert (newLen <= bufferedAppend->largeWriteLen +
		              bufferedAppend->maxBufferLen);
	if (newLen >= bufferedAppend->largeWriteLen)
	{
		/*
		 * Current large-write memory is full.
		 */
		bufferedAppend->len = bufferedAppend->largeWriteLen;
		BufferedAppendWrite(bufferedAppend);
		
		if (newLen > bufferedAppend->largeWriteLen)
		{
			int32 excessLen;
			
			/*
			 * We have carry-over in the extra buffer.  Write and then 
			 * copy the extra to the front of the large write buffer.
			 */
			excessLen = newLen - bufferedAppend->largeWriteLen;
			
			bufferedAppend->len = bufferedAppend->largeWriteLen;
			
			memcpy(bufferedAppend->largeWriteMemory,
				   bufferedAppend->afterBufferMemory,
				   excessLen);

			bufferedAppend->len = excessLen;
		}
		else
		{
			/*
			 * Exactly fits.
			 */
			Assert(newLen == bufferedAppend->largeWriteLen);
			Assert(bufferedAppend->len == 0);
		}
	}
	else
	{
		/*
		 * Normal case -- added more data to current buffer.
		 */
		bufferedAppend->len = newLen;
	}

	bufferedAppend->fileLen += usedLen;
	bufferedAppend->fileLen_uncompressed += usedLen_uncompressed;
}

/*
 * Finish the current buffer and get the next
 * buffer for appending.
 *
 * Returns NULL when the current file does not have enough
 * room for another buffer.
 */
uint8 *BufferedAppendMoveToNextBuffer(
    BufferedAppend       *bufferedAppend,
    int32                usedLen,
    int32				 usedLen_uncompressed)
{
	Assert(bufferedAppend != NULL);
	
	BufferedAppendFinishBuffer(
			    		bufferedAppend,
			    		usedLen,
			    		usedLen_uncompressed);
	
	return BufferedAppendGetMaxBuffer(bufferedAppend);
}


/*
 * Returns the current file length.
 */
int64 BufferedAppendFileLen(
    BufferedAppend *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);
	
	return bufferedAppend->fileLen;
}

/*
 * Flushes the current file for append.  Caller is resposible for closing
 * the file afterwards.  That close will flush any buffered writes for the
 * file.
 *
 * Returns the file length.
 */
void BufferedAppendCompleteFile(
    BufferedAppend	*bufferedAppend,
    int64 			*fileLen,
    int64 			*fileLen_uncompressed)
{
	
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);

	if (bufferedAppend->len > 0)
		BufferedAppendWrite(bufferedAppend);

	*fileLen = bufferedAppend->fileLen;
	*fileLen_uncompressed = bufferedAppend->fileLen_uncompressed;

	bufferedAppend->position = 0;
	bufferedAppend->len = 0;
	
	bufferedAppend->bufferLen = 0;
	
	bufferedAppend->fileLen = 0;
	bufferedAppend->fileLen_uncompressed = 0;
	bufferedAppend->file = -1;
	bufferedAppend->filePathName = NULL;
}

/*
 * Finish with writing all together.
 */
void BufferedAppendFinish(
    BufferedAppend *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file == -1);
    Assert(bufferedAppend->bufferLen == 0);
	Assert(bufferedAppend->fileLen == 0);
	Assert(bufferedAppend->fileLen_uncompressed == 0);

}
