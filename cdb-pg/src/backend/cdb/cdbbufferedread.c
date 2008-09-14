/*-------------------------------------------------------------------------
 *
 * cdbbufferedread.c
 *	  Read buffers sequentially from a file efficiently.
 *
 * (See .h file for usage comments)
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>				/* for read() */

#include "cdb/cdbbufferedread.h"

static void BufferedReadIo(
    BufferedRead        *bufferedRead);
static uint8 *BufferedReadUseBeforeBuffer(
    BufferedRead       *bufferedRead,
    int32              maxReadAheadLen,
    int32              *nextBufferLen);


/*
 * Determines the amount of memory to supply for
 * BufferedRead given the desired buffer and
 * large write lengths.
 */
int32 BufferedReadMemoryLen(
    int32                maxBufferLen,
    int32                largeReadLen)
{
	Assert(maxBufferLen > 0);
	Assert(largeReadLen >= maxBufferLen);

	// Adjacent before memory for 1 buffer and large read memory.
	return (maxBufferLen + largeReadLen);
}

/*
 * Initialize BufferedRead.
 *
 * Use the BufferedReadMemoryLen procedure to
 * determine the amount of memory to supply.
 */
void BufferedReadInit(
    BufferedRead         *bufferedRead,
    uint8                *memory,
    int32                memoryLen,
    int32                maxBufferLen,
    int32                largeReadLen)
{
	Assert(bufferedRead != NULL);
	Assert(memory != NULL);
	Assert(maxBufferLen > 0);
	Assert(largeReadLen >= maxBufferLen);
	Assert(memoryLen >= BufferedReadMemoryLen(maxBufferLen, largeReadLen));
	
	memset(bufferedRead, 0, sizeof(BufferedRead));

	/*
	 * Large-read memory level members.
	 */
    bufferedRead->maxBufferLen = maxBufferLen;
    bufferedRead->largeReadLen = largeReadLen;

	bufferedRead->memory = memory;
    bufferedRead->memoryLen = memoryLen;

    bufferedRead->beforeBufferMemory = memory;
	bufferedRead->largeReadMemory =
						&memory[maxBufferLen];

	bufferedRead->position = 0;
	bufferedRead->readLen = 0;
	
	/*
	 * Buffer level members.
	 */	
	bufferedRead->bufferOffset = 0;
	bufferedRead->bufferLen = 0;

	/*
	 * File level members.
	 */
	bufferedRead->file = -1;
    bufferedRead->fileLen = 0;
}

/*
 * Takes an open file handle for the next file.
 */
void BufferedReadSetFile(
    BufferedRead       *bufferedRead,
    File 				file,
    char				*filePathName,
    int64               fileLen)
{
	Assert(bufferedRead != NULL);
	
	Assert(bufferedRead->file == -1);
	Assert(bufferedRead->fileLen == 0);
		
    Assert(bufferedRead->position == 0);
    Assert(bufferedRead->readLen == 0);

    Assert(bufferedRead->bufferOffset == 0);
    Assert(bufferedRead->bufferLen == 0);

	Assert(file >= 0);
	Assert(fileLen >= 0);
	
	bufferedRead->file = file;
    bufferedRead->filePathName = filePathName;
    bufferedRead->fileLen = fileLen;

	if (fileLen > 0)
	{
		/*
		 * Do the first read.
		 */
		if (fileLen > bufferedRead->largeReadLen)
			bufferedRead->readLen = bufferedRead->largeReadLen;
		else
			bufferedRead->readLen = (int32)fileLen;
		BufferedReadIo(bufferedRead);
	}
}

/*
 * Perform a large read i/o.
 */
static void BufferedReadIo(
    BufferedRead        *bufferedRead)
{
	int32 readLen;
	uint8 *largeReadMemory;

	readLen = bufferedRead->readLen;
	Assert(bufferedRead->readLen > 0);
	largeReadMemory = bufferedRead->largeReadMemory;

#ifdef USE_ASSERT_CHECKING
	{
		int64 currentReadPosition; 
		
		currentReadPosition = FileSeek(bufferedRead->file, 0, SEEK_CUR);
		if (currentReadPosition < 0)
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("unable to get current position for file \"%s\" (errcode %d)",
							       bufferedRead->filePathName,
								   errno)));
			
		if (currentReadPosition != bufferedRead->position)
		{
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("Current position mismatch actual "
								   INT64_FORMAT ", expected " INT64_FORMAT " in file \"%s\"",
								   currentReadPosition, bufferedRead->position,
								   bufferedRead->filePathName)));
		}
	}
#endif
	
	while (readLen > 0) 
	{
		int actualLen = FileRead(
							bufferedRead->file,
							(char*)largeReadMemory,
							readLen);

		if (actualLen == 0) 
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("read beyond eof in file \"%s\"",
							       bufferedRead->filePathName)));
		else if (actualLen < 0)
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("unable to read file \"%s\" (errcode %d)", 
								   bufferedRead->filePathName,
								   errno)));
		readLen -= actualLen;
		largeReadMemory += actualLen;
	}
}

static uint8 *BufferedReadUseBeforeBuffer(
    BufferedRead       *bufferedRead,
    int32              maxReadAheadLen,
    int32              *nextBufferLen)
{
	int64 nextPosition;
	int64 remainingFileLen;
	int32 nextReadLen;
	int32 beforeLen;
	int32 beforeOffset;
	int32 extraLen;

	Assert(bufferedRead->readLen - bufferedRead->bufferOffset > 0);
	
	/*
	 * Requesting more data than in the current read.
	 */		
	nextPosition = bufferedRead->position + bufferedRead->readLen;
	
	if (nextPosition == bufferedRead->fileLen)
	{
		/*
		 * No more file to read.
		 */
		bufferedRead->bufferLen = bufferedRead->readLen -
								  bufferedRead->bufferOffset;
		Assert(bufferedRead->bufferLen > 0);
		
		*nextBufferLen = bufferedRead->bufferLen;
		return &bufferedRead->largeReadMemory[bufferedRead->bufferOffset];
	}

	remainingFileLen = bufferedRead->fileLen - 
	                   nextPosition;
	if (remainingFileLen > bufferedRead->largeReadLen)
		nextReadLen = bufferedRead->largeReadLen;
	else
		nextReadLen = (int32)remainingFileLen;
	
	beforeLen = bufferedRead->largeReadLen - bufferedRead->bufferOffset;
	beforeOffset = bufferedRead->maxBufferLen - beforeLen;
	
	/*
	 * Copy data from the current large-read buffer into the before memory.
	 */
	memcpy(&bufferedRead->beforeBufferMemory[beforeOffset],
	       &bufferedRead->largeReadMemory[bufferedRead->bufferOffset],
	       beforeLen);

	/*
	 * Do the next read.
	 */
	bufferedRead->position = nextPosition;
	bufferedRead->readLen = nextReadLen;
	
	BufferedReadIo(bufferedRead);

	extraLen = maxReadAheadLen - beforeLen;
	Assert(extraLen > 0);
	if (extraLen > nextReadLen)
		extraLen = (int32)nextReadLen;

	/*
	 * Return a buffer using a negative offset that starts in the
	 * before memory and goes into the large-read memory.
	 */
	bufferedRead->bufferOffset = -beforeLen;
	bufferedRead->bufferLen = beforeLen + extraLen;
	Assert(bufferedRead->bufferLen > 0);
	
	*nextBufferLen = bufferedRead->bufferLen;

	return &bufferedRead->beforeBufferMemory[beforeOffset];
}

/*
 * Return the position of the next read in bytes.
 */
int64 
BufferedReadNextBufferPosition(
    BufferedRead       *bufferedRead)
{
	Assert(bufferedRead != NULL);
	Assert(bufferedRead->file >= 0);

	/*
	 * Note that the bufferOffset can be negative when we are using the
	 * before buffer, but that is ok.  We should get an accurate
	 * next position.
	 */
	return bufferedRead->position + 
		   bufferedRead->bufferOffset + 
		   bufferedRead->bufferLen;
}

/*
 * Get the next buffer space for reading with a specified max read-ahead
 * amount.
 *
 * Returns NULL when the current file has been completely read.
 */
uint8 *BufferedReadGetNextBuffer(
    BufferedRead       *bufferedRead,
    int32              maxReadAheadLen,
    int32              *nextBufferLen)
{
	Assert(bufferedRead != NULL);
	Assert(bufferedRead->file >= 0);
	Assert(maxReadAheadLen > 0);
	Assert(nextBufferLen != NULL);
	if (maxReadAheadLen > bufferedRead->maxBufferLen)
	{
		Assert(maxReadAheadLen <= bufferedRead->maxBufferLen);
		
		elog(ERROR, "Read ahead length %d is greater than maximum buffer length %d",
		     maxReadAheadLen, bufferedRead->maxBufferLen);
	}
	

	if (bufferedRead->position == bufferedRead->fileLen)
	{
		/*
		 * At end of file.
		 */
		*nextBufferLen = 0;
		return NULL;
	}
	
	/*
	 * Finish previous buffer.
	 */
	bufferedRead->bufferOffset += bufferedRead->bufferLen;
	bufferedRead->bufferLen = 0;

	/*
	 * Any more left in current read?
	 */
	if (bufferedRead->bufferOffset == bufferedRead->readLen)
	{
		int64 remainingFileLen;
	
		/*
		 * Used exactly all of it.  Attempt read more.
		 */
		bufferedRead->position += bufferedRead->readLen;
		bufferedRead->bufferOffset = 0;
		
		remainingFileLen = bufferedRead->fileLen - 
		                   bufferedRead->position;
		if (remainingFileLen > bufferedRead->largeReadLen)
			bufferedRead->readLen = bufferedRead->largeReadLen;
		else
			bufferedRead->readLen = (int32)remainingFileLen;
		if (bufferedRead->readLen == 0)
		{
			/*
			 * At end of file.
			 */
			*nextBufferLen = 0;
			return NULL;
		}

		BufferedReadIo(bufferedRead);

		if (maxReadAheadLen > bufferedRead->readLen)
			bufferedRead->bufferLen = bufferedRead->readLen;
		else
			bufferedRead->bufferLen = maxReadAheadLen;

		Assert(bufferedRead->bufferOffset == 0);
		Assert(bufferedRead->bufferLen > 0);
		*nextBufferLen = bufferedRead->bufferLen;
		return bufferedRead->largeReadMemory;
	}

	if (bufferedRead->bufferOffset + maxReadAheadLen > bufferedRead->readLen)
	{
		/*
		 * Odd boundary.  Use before memory to carry us over.
		 */
		Assert(bufferedRead->readLen - bufferedRead->bufferOffset > 0);
	
		return BufferedReadUseBeforeBuffer(
									bufferedRead, 
									maxReadAheadLen,
									nextBufferLen);
	}

	/*
	 * We can satisify request from current read.
	 */
	bufferedRead->bufferLen = maxReadAheadLen;
	
	Assert(bufferedRead->bufferLen > 0);
	*nextBufferLen = bufferedRead->bufferLen;
	return &bufferedRead->largeReadMemory[bufferedRead->bufferOffset];
}

/*
 * Get the next, maximum buffer space for reading.
 *
 * Returns NULL when the current file has been completely read.
 */
uint8 *BufferedReadGetMaxBuffer(
    BufferedRead       *bufferedRead,
    int32              *nextBufferLen)
{
	Assert(bufferedRead != NULL);
	Assert(bufferedRead->file >= 0);
	Assert(nextBufferLen != NULL);

	return BufferedReadGetNextBuffer(
							bufferedRead,
							bufferedRead->maxBufferLen,
							nextBufferLen);
}

/*
 * Grow the available length of the current buffer.
 *
 * NOTE: The buffer address returned can be different, even for previously
 * examined buffer data.  In other words, don't keep buffer pointers in
 * the buffer region.  Use offsets to re-establish pointers after this call.
 *
 * If the current file has been completely read, bufferLen will remain
 * the current value.
 */
uint8 *BufferedReadGrowBuffer(
    BufferedRead       *bufferedRead,
    int32              newMaxReadAheadLen,
    int32              *growBufferLen)
{
	int32 originalBufferLen;
	int32 newNextOffset;

	Assert(bufferedRead != NULL);
	Assert(bufferedRead->file >= 0);
	Assert(newMaxReadAheadLen > bufferedRead->bufferLen);
	Assert(newMaxReadAheadLen <= bufferedRead->maxBufferLen);
	Assert(growBufferLen != NULL);

	originalBufferLen = bufferedRead->bufferLen;
	
	newNextOffset = bufferedRead->bufferOffset + newMaxReadAheadLen;
    if (newNextOffset > bufferedRead->readLen)
	{
		/*
		 * Odd boundary.  Use before memory to carry us over.
		 */
		Assert(bufferedRead->readLen - bufferedRead->bufferOffset > 0);
	
		return BufferedReadUseBeforeBuffer(
									bufferedRead, 
									newMaxReadAheadLen,
									growBufferLen);
	}

	/*
	 * There is maxReadAheadLen more left in current large-read memory.
	 */
	bufferedRead->bufferLen = newMaxReadAheadLen;
	Assert(bufferedRead->bufferLen > 0);
	
	*growBufferLen = bufferedRead->bufferLen;
	return &bufferedRead->largeReadMemory[bufferedRead->bufferOffset];
}

/*
 * Flushes the current file for append.  Caller is resposible for closing
 * the file afterwards.
 *
 */
void BufferedReadCompleteFile(
    BufferedRead       *bufferedRead)
{
	Assert(bufferedRead != NULL);
	Assert(bufferedRead->file >= 0);
	
	bufferedRead->file = -1;
    bufferedRead->filePathName = NULL;
	bufferedRead->fileLen = 0;
	
    bufferedRead->bufferOffset = 0;
    bufferedRead->bufferLen = 0;
	
	bufferedRead->position = 0;
}


/*
 * Finish with reading all together.
 */
void BufferedReadFinish(
    BufferedRead *bufferedRead)
{
	Assert(bufferedRead != NULL);
	
	//Assert(bufferedRead->file == -1);
	Assert(bufferedRead->fileLen == 0);
	
    Assert(bufferedRead->bufferOffset == 0);
    Assert(bufferedRead->bufferLen == 0);	
}
