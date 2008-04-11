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
#include <unistd.h>				/* for write() */

#include "cdb/cdbbufferedappend.h"

// #define BUFFEREDTEST_APPEND

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
    bufferedAppend->fileLen = 0;
}

/*
 * Takes an open file handle for the next file.
 */
void BufferedAppendSetFile(
    BufferedAppend       *bufferedAppend,
    File 				 file,
    int64				 eof)
{
	Assert(bufferedAppend != NULL);
    Assert(bufferedAppend->position == 0);
    Assert(bufferedAppend->len == 0);
    Assert(bufferedAppend->bufferLen == 0);
	Assert(bufferedAppend->file == -1);
	Assert(bufferedAppend->fileLen == 0);

	Assert(file >= 0);
	Assert(eof >= 0);	// UNDONE: Should be a multiple of file-system block length.

	bufferedAppend->position = eof;
	bufferedAppend->file = file;
	bufferedAppend->fileLen = eof;
}


/*
 * Perform a large write i/o.
 */
static void BufferedAppendWrite(
    BufferedAppend      *bufferedAppend)
{
	int32 writeLen;
	uint8 *largeWriteMemory;
	int64 currentWritePosition; 

	writeLen = bufferedAppend->len;
	Assert(bufferedAppend->len > 0);
	largeWriteMemory = bufferedAppend->largeWriteMemory;
	
	
#ifdef BUFFEREDTEST_APPEND
	currentWritePosition = lseek(bufferedAppend->file, 0, SEEK_CUR);
	if (currentWritePosition < 0)
	{
		fprintf(stderr,"unable to get current position (errcode %d)", 
							   errno);
		exit(1);
	}

	if (currentWritePosition != bufferedAppend->position)
	{
		fprintf(stderr,"Current position mismatch actual %lld, expected %lld",
							   currentWritePosition, bufferedAppend->position);
		exit(1);
	}
#else
	currentWritePosition = FileSeek(bufferedAppend->file, 0, SEEK_CUR);
	if (currentWritePosition < 0)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("unable to get current position (errcode %d)", 
							   errno)));

	if (currentWritePosition != bufferedAppend->position)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("Current position mismatch actual "
							   INT64_FORMAT ", expected " INT64_FORMAT,
							   currentWritePosition, bufferedAppend->position)));
#endif
		
	while (writeLen > 0) 
	{
#ifdef BUFFEREDTEST_APPEND
		int len = write(bufferedAppend->file, (char*)largeWriteMemory, writeLen);
		if (len == 0) 
		{
			fprintf(stderr,"write beyond eof\n");
			exit(1);
		}
		else if (len < 0)
		{
			fprintf(stderr,"unable to write file (errcode %d)\n", 
								   errno);
			exit(1);
		}
#else
		int len = FileWrite(bufferedAppend->file, (char*)largeWriteMemory, writeLen);
		if (len == 0) 
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("write beyond eof")));
		else if (len < 0)
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("unable to write file (errcode %d)", 
								   errno)));
#endif
		writeLen -= len;
		largeWriteMemory += len;
	}

	bufferedAppend->position += bufferedAppend->len;
	bufferedAppend->len = 0;

	bufferedAppend->fileLen += bufferedAppend->len;

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
	Assert(bufferLen <= bufferedAppend->maxBufferLen);

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


/*
 * Indicate the current buffer is finished.
 */
void BufferedAppendFinishBuffer(
    BufferedAppend       *bufferedAppend,
    int32                usedLen)
{
	int32 newLen;
	
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);
	Assert(usedLen <= bufferedAppend->bufferLen);

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
    int32                usedLen)
{
	Assert(bufferedAppend != NULL);
	
	BufferedAppendFinishBuffer(
			    		bufferedAppend,
			    		usedLen);
	
	return BufferedAppendGetMaxBuffer(bufferedAppend);
}


/*
 * Returns the current file’s length.
 */
int32 BufferedAppendFileLen(
    BufferedAppend *bufferedAppend)
{
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);
	
	return bufferedAppend->fileLen;
}

/*
 * Flushes the current file for append.  Caller is resposible for closing
 * the file afterwards.
 *
 * Returns the file length.
 */
int32 BufferedAppendCompleteFile(
    BufferedAppend       *bufferedAppend)
{
	int32 fileLen;
	
	Assert(bufferedAppend != NULL);
	Assert(bufferedAppend->file >= 0);

	if (bufferedAppend->len > 0)
		BufferedAppendWrite(bufferedAppend);

	fileLen = bufferedAppend->fileLen;

#ifdef BUFFEREDTEST_APPEND
	close(bufferedAppend->file);
#else
	/* done with the file */
	FileSync(bufferedAppend->file);
	FileClose(bufferedAppend->file);
#endif
	
	bufferedAppend->position = 0;
	bufferedAppend->len = 0;
	
	bufferedAppend->bufferLen = 0;
	
	bufferedAppend->fileLen = 0;
	bufferedAppend->file = -1;

	return fileLen;
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

}
