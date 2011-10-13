/*
 * Copyright (c) 2011 EMC Corporation All Rights Reserved
 *
 * This software is protected, without limitation, by copyright law
 * and international treaties. Use of this software and the intellectual
 * property contained therein is expressly limited to the terms and
 * conditions of the License Agreement under which it is provided by
 * or on behalf of EMC.
 *
 * ---------------------------------------------------------------------
 */

#ifndef USER_LEVEL_TESTING

/* includes for running in database backend as normal */
#include "postgres.h"
#include "c.h"
#include "access/reloptions.h"
#include "cdb/cdbappendonlyam.h"
#include "cdb/cdbappendonlystoragelayer.h"
#include "storage/rle_type.h"
#include "utils/datumstream.h"
#include "utils/guc.h"


#else

/****   THIS SECTION FOR USER_LEVEL_TESTING *************/
/* 
 * Maintaining the ability to run at user level is helpful for performance testing of rle
 * it allows issues such as round robin among segments and
 * the granularity at which data is loaded to be factored out
 * Set Debug_rle_type_compression and rle_type_compresssion_stats as desired and
 * then compile with g++ -D USER_LEVEL_TESTING rle_type.c
 * Can also uncomment #define of USER_LEVEL_TESTING above
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* a few #defines and declarations to "port" to user level */
#define Assert(x) assert(x)
#define Oid int
/* might as well leave on by default at userlevel */
bool Debug_rle_type_compression = true;
bool rle_type_compression_stats = true;

/**********************************************************/
#endif

#define MAX_REPEAT 255
#define VARIABLE_LENGTH -1

/*  RLE replaces runs of the same data values aaaaaa
 *  with  <value, count> pairs (e.g. <a, 6> )
 *
 *  More specifically if a data value occurs once it will be left unmodifed:
 *  a -> a
 * 
 *  If a data value occurs more than once it will be replaced with two copies 
 *  of the data followed by the total number of copies-1, so:
 *  aa -> aa1
 *  aaa -> aa2 
 *  aaaa -> aa3  
 *  etc.
 *  
 *  We use a single byte to hold the count so the largest count value is 255
 *  and the largest run we can represent is a run of 256 like so:
 *  256 a's -> aa255 
 * 
 * Runs longer than 256 will be divided into multiple runs of 256 or less 
 * and handled seperately so:
 * 257 a's -> aa255a
 * 258 a's -> aa255aa1
 * 259 a's -> aa255aa2
 * 260 a's -> aa255aa3
 *
 *
 * One additional complication is that in the Greenplum database data blocks 
 * begin with a header that will be skipped before beginning actual 
 * compression (in other words the header will be copied as 
 * is into the compressed data and then actual compression will be applied 
 * to the data that follows the header) - this could also have been handled 
 * by passing just the data into the compression/decompression
 * functions and while this may be cleaner, it would mean that 
 * these compression algorithms need more information than others which would 
 * complicate the caller
 * 
 * Note: we can't have aa -> aa0 and aa255 -> a257 because a 0 following datum can
 * be misinterpreted as padding 
 */

/*   
 *   rle_type_comp
 *
 *         typeLen_bytes:    size of each data element, if -1 then variable size
 *         colType:          Oid of the colType (e.g. char or timestamp, etc.)
 *         align:            char to indicate alignment of the datums
 *         inbuf:            buffer of data to be compressed
 *         inSizeBytes:      amount of data to be compressed
 *         outbuf:           buffer into which we should place the decompressed  
 *                           data
 *         outbufSizeBytes:  physical size of the output buffer
 *         outSizeBytes:     amount of decompressed data actually placed in the  
 *                           output buffer
 *
 *   returns -1 if there is an error that cannot be handled
 *   returns the number of bytes compressed
 *   if return is not inSizeBytes then there was not enough space
 *   to write all the output - the data must either be left unompressed or
 *   the function should be recalled with an output buffer large enough 
 *   to hold all the compressed data
 *    
 *   Function cannot just be recalled on remaining data unless we pass in a 
 *   parameter that says not to expect a block header in subsequent calls
 * 
 *   if outbufSizeBytes is 0 then there wasn't enough room in the output buffer
 *   to even compress the next piece of data - as long as the buffer is 
 *   2*typeLenBytes+1 that should not happen
 *
 */

int
rle_type_comp(
	int typeLen_bytes,  
	Oid colType, 
	char align,
	char * inbuf,         
	int inSizeBytes,      
	char * outbuf,       
	int outbufSizeBytes,  
	int * outSizeBytes)   
{

	char                * lastDatum = NULL;
	char				* thisDatum = NULL;
	char                * data = NULL;
	int                 finishedIndex = 0;
	int                 dataIndex =0;
	int                 outIndex = 0;
	int                 repeatDataCount= 0;
	int                 dataSizeBytes = 0;
	int                 headerNullSize = 0;
	int                 thisDatumLen = 0;
	int                 lastDatumLen = 0;
	int                 in_nDatum = 0;
	int                 out_nDatum = 0;
	bool                endOfData = false;
	char                * next;
	char                * adjustedNext;
	int                 newDatumLen;		

	if (typeLen_bytes == 0)
	{
		return -1;
	}
#ifdef USER_LEVEL_TESTING
	
	data = inbuf;
	dataSizeBytes = inSizeBytes;
	headerNullSize = 0;
	
#else
	DatumStreamBlock	* block = NULL;
	block = (DatumStreamBlock*) inbuf;
	

	/* 
	 * move past the block header and Null Bit map at the beginning
	 * of the block 
	 * we just want to compress the actual data elements
	 */
	headerNullSize = sizeof(DatumStreamBlock) + block->nullsz;
	data = inbuf + headerNullSize;
	dataSizeBytes = inSizeBytes - headerNullSize;

	/* copy the hedaer and null bit map unchanged over to the output buffer */
	memcpy(outbuf, inbuf, headerNullSize);

	if (Debug_rle_type_compression)
	{
		elog(LOG, 
			 "rle_type_comp START typeLen_bytes %d colType %d align %c inSizeBytes %d outbufSizeBytes %d ndatum %d headerNullSize %d dataSizeBytes %d\n",
			 typeLen_bytes,
			 colType,
			 align,
			 inSizeBytes,
			 outbufSizeBytes,
			 block->ndatum,
			 headerNullSize,
			 dataSizeBytes);
	}

	if (headerNullSize == inSizeBytes)
	{
		/* 
		 * all that is in this buffer is the DatumStreamBlock header 
		 * and the null bit map - no more work to do 
		 * oddly I have seen block->ndatum be 1 in this case?
		 */
		*outSizeBytes = headerNullSize;
		return inSizeBytes;

	}

#endif

	/* we've copied headerNullSize bytes to the outbuf */
	outIndex = headerNullSize;
	/* we've processed headerNullSize bytes of the inbuf */
	finishedIndex= headerNullSize;

	/* point lastDatum to the first data element in the input buffer */
	lastDatum = data;
	in_nDatum = 1;
	out_nDatum = 0;
	repeatDataCount = 1;
	

	/* 
	 * set dataIndex to the second data element   
	 * based on how long the first data element is
	 */
	if (typeLen_bytes == VARIABLE_LENGTH) 
	{
#ifdef USER_LEVEL_TESTING
		/* 
		 * for user level testing assume it is one byte
		 * len followed by data
		 */
		unsigned char len = *((unsigned char*) lastDatum);
		lastDatumLen = len +1;
#else
		lastDatumLen = VARSIZE_ANY((struct varlena *) lastDatum);
		Assert(lastDatumLen != 0);
#endif

	} 
	else 
	{
		lastDatumLen = 	typeLen_bytes;
	}
	/* ADJUST lastDatumLen for alignment */
#ifndef USER_LEVEL_TESTING
	next = lastDatum + lastDatumLen;
	adjustedNext = lastDatum+ lastDatumLen;
	
	if ((typeLen_bytes == VARIABLE_LENGTH) &&(*next == 0))
	{
		
		adjustedNext = 	(char *) att_align(adjustedNext, align);
		newDatumLen = adjustedNext - lastDatum;
		
		if ((newDatumLen != lastDatumLen) && (newDatumLen < dataSizeBytes))
		{
			Assert(newDatumLen > lastDatumLen);
			lastDatumLen = newDatumLen;
		}
	}
#endif

	dataIndex = lastDatumLen;

	endOfData = false;	
	while (!endOfData)
	{
		bool thisDatumMatchesLast = false;

		if (dataIndex == dataSizeBytes)
		{
			/* 
			 * we have completely processed all
			 * the input data and should write
			 * out the last compressed run 
			 */
			endOfData = true;
			thisDatum = NULL;
			thisDatumLen = 0;
			thisDatumMatchesLast = false;
		} 
		else 
		{
			endOfData = false;
			/* 
			 * set the thisDatum to the next
			 * element in the data buffer 
			 */
			thisDatum = &(data[dataIndex]);
			
			if (typeLen_bytes == VARIABLE_LENGTH) 
			{
#ifdef USER_LEVEL_TESTING
				/* 
				 * for user level testing assume it is one byte
				 * len followed by data
				 */
				unsigned char len = *((unsigned char*) thisDatum);
				thisDatumLen = len +1;
#else
				thisDatumLen = VARSIZE_ANY((struct varlena *) thisDatum);
				Assert(thisDatumLen != 0);
#endif
			} 
			else 
			{
				thisDatumLen = 	typeLen_bytes;
			}

			/* ADJUST thisDatumLen for alignment */
#ifndef USER_LEVEL_TESTING
			next = thisDatum+ thisDatumLen;
			adjustedNext = thisDatum+ thisDatumLen;
			
			if ((typeLen_bytes == VARIABLE_LENGTH) &&(*next == 0))
			{
				
				adjustedNext = 	(char *) att_align(adjustedNext, align);
				newDatumLen = adjustedNext - thisDatum;
				
				if ( (newDatumLen != thisDatumLen) &&
					 (dataIndex + newDatumLen <=dataSizeBytes))
				{
					Assert(newDatumLen > thisDatumLen);
					thisDatumLen = newDatumLen;
					
				}
			}
#endif

			if (dataIndex + thisDatumLen > dataSizeBytes) 
			{

#ifndef USER_LEVEL_TESTING
				if (Debug_rle_type_compression)
				{
					elog(LOG, 
						 "rle_type_comp: not enough input left for full datum dataIndex %d thisDatumLen %d dataSizeBytes %d dataSizeBytes- dataIndex %d",
						 dataIndex,
						 thisDatumLen,
						 dataSizeBytes,
						 dataSizeBytes- dataIndex
						);

					elog(LOG,
						 "rle_type_comp: garbage past data in input finishedIndex %d inSizeBytes %d inSizeBytes- finishedIndex %d",
						 finishedIndex,
						 inSizeBytes,
						 inSizeBytes - finishedIndex);
				}
#endif
				/* 
				 * data not long enough to hold this whole datum
				 * - must be garbage at end of input
				 * We used to just assert
				 * Assert (dataIndex + thisDatumLen <= dataSizeBytes);
				 * This is a more defensive version that will 
				 * continue to behave well even if the input is 
				 * malformed
				 */
				endOfData = true;
				thisDatum = NULL;
				thisDatumLen = 0;
				thisDatumMatchesLast = false;


				/* 
				 * compress the garbage by just ignoring and 
				 * not copying it over to the output buffer 
				 */
				finishedIndex = inSizeBytes;

#ifndef USER_LEVEL_TESTING
			} 
			else if (in_nDatum == block->ndatum) 
			{

				endOfData = true;
				thisDatum = NULL;
				thisDatumLen = 0;
				thisDatumMatchesLast = false;

				/* 
				 * we've already processed all the datum in this block
				 * but there is still garbage data passed it
				 * can we safely skip the garbage data and *not* copy it over
				 * to the output buffer - compress it by removing it completely
				 */
				finishedIndex = inSizeBytes;
				
#endif				
			} 
			else 
			{
				in_nDatum++;
				
				/* compare thisDatum to lastDatum */
				if (thisDatumLen == lastDatumLen)
				{
					if (memcmp(thisDatum, lastDatum, thisDatumLen) == 0)
					{
						thisDatumMatchesLast = true;
					} 
					else 
					{
						thisDatumMatchesLast = false;
					}						
				} 
				else
				{
					thisDatumMatchesLast = false;
				}
				
			}
		}

		/* 
		 * if we are at the end of the buffer OR if this datum does 
		 * not match the last datum so we must write out the next 
		 * compressed run of lastDatum values
		 
		 * largest value we can write is 255 so largest
		 * number in a row we can have is 257  so even 
		 * if this matches the last - if repeatDataCount > 256
		 * we still ahve to write out the next compressed run
		 *  e.g.
		 *  a = a
		 *  aa0 = aa
		 *  aa1 = aaa
		 *  aa255 = 257 a's
		 *  
		 */
		if (endOfData || 
			!thisDatumMatchesLast ||
			(thisDatumMatchesLast && (repeatDataCount > MAX_REPEAT))
			)		
		{

#ifndef USER_LEVEL_TESTING
			if (Debug_rle_type_compression)
			{
				elog(LOG, 
					 "rle_type_comp in_nDatum %d repeatDataCount %d lastDatumLen %d\n",
					 in_nDatum,
					 repeatDataCount,
					 lastDatumLen
					 );
			}
#endif
			if (repeatDataCount == 1)
			{
				/* just one of these in a row this time*/

				/* 
				 * check for overflow first - 
				 * we need room for lastDatumLen 
				 */
				if (outIndex+ lastDatumLen > outbufSizeBytes)
				{
					*outSizeBytes = outIndex;
					/* 
					 * can't be recalled unless we pass a parameter into 
					 * subsequent calls that says not to expect a
					 * block header - but don't just return -1
					 * because upper layer needs to know difference
					 * between fundamental error and not room in output
					 * buffer to handle all output
					 */
					return finishedIndex;

				}

				Assert (lastDatumLen != 0);
				memcpy(&(outbuf[outIndex]), lastDatum, lastDatumLen);
				outIndex+=lastDatumLen;
				out_nDatum++;

#ifdef USER_LEVEL_TESTING
				if (Debug_rle_type_compression && (typeLen_bytes == 1) )
				{
					printf("%c", *lastDatum);
				}
#endif
				/* we've handled 1 more item of size lastDatumLen*/
				finishedIndex+=lastDatumLen;
				Assert(finishedIndex <= inSizeBytes);
			}
			else
			{
				/* we've had at least two in a row */

				/* check for overflow first - 
				 *   we need room for  2* lastDatumLen + 1 bytes 
				 */
				Assert (lastDatumLen != 0);
				if (outIndex+ 2*(lastDatumLen) + 1 > outbufSizeBytes)
				{
					*outSizeBytes = outIndex;
					/* 
					 * can't be recalled unless we pass a parameter into 
					 * subsequent calls that says not to expect a
					 * block header - but don't just return -1
					 * because upper layer needs to know difference
					 * between fundamental error and not room in output
					 * buffer to handle all output
					 */
					return finishedIndex;

				}

				memcpy(&(outbuf[outIndex]), lastDatum, lastDatumLen);
				outIndex+=lastDatumLen;

				memcpy(&(outbuf[outIndex]), lastDatum,lastDatumLen);
				outIndex+=lastDatumLen;

				/* use a single byte to store the repeatDataCount */
				outbuf[outIndex++] = (unsigned char)(repeatDataCount-1);

				out_nDatum += repeatDataCount;

#ifdef USER_LEVEL_TESTING
				if (Debug_rle_type_compression && (typeLen_bytes == 1) )
				{
					printf("%c%c%d", 
						   *lastDatum, 
						   *lastDatum, 
						   (unsigned char)(repeatDataCount-2));
				}
#endif
				/* we've handled repeatDataCount items of size lastDatumLen*/
				finishedIndex += (repeatDataCount* lastDatumLen);
			}

			/* set lastDatum to thisDatum and reset repeatDataCount to 1 */
			lastDatum = thisDatum;
			lastDatumLen = thisDatumLen;
			repeatDataCount = 1;
			
		}
		else
		{
			Assert(thisDatum != NULL);
			Assert(thisDatumLen != 0);
			Assert(thisDatumMatchesLast);
			Assert(repeatDataCount <= MAX_REPEAT);
			Assert(!endOfData);
			Assert(dataIndex < dataSizeBytes);
		
			/*all we need to do here is increment the repeatDataCount */
			repeatDataCount++;

		}
			
		/* move dataIndex past thisDatum */
		dataIndex+= thisDatumLen;
	}
	*outSizeBytes = outIndex;
	
#ifndef USER_LEVEL_TESTING
	if (rle_type_compression_stats)
	{
		elog(LOG, 
			 "rle_type_comp END colType %d align %c inSizeBytes %d outsizeBytes %d dataSizeBytes %d compression ratio TOTAL %0.2f space savings %0.2f WITHOUT_HEADER_OVERHEAD compression ratio %0.2f space savings %0.2f\n",
			 colType,
			 align,
			 inSizeBytes,
			 outIndex,
			 dataSizeBytes,
			 ((float)(outIndex)/(float) inSizeBytes) *100.0,
			 100.0 - (((float)(outIndex)/(float) inSizeBytes) *100.0),
			 ((float)(outIndex-headerNullSize)/
			  (float) (inSizeBytes-headerNullSize)) *100.0,
			 100.0 - (((float)(outIndex-headerNullSize)/
					 (float) (inSizeBytes-headerNullSize)) *100.0)
			);
	}
#endif


	/* 
	 * we should have finished all the input
	 * and processed exactly the number of datum
	 * as described in the block header
	 * only thing that should cause these assertions
	 * would be if there was garbage past the data values */
	Assert(finishedIndex == inSizeBytes);
	
#ifndef USER_LEVEL_TESTING
	Assert (in_nDatum == out_nDatum);
	Assert (in_nDatum == block->ndatum);
#endif

	return finishedIndex;
}


/*
 *   rle_type_decomp:
 *
 *         typeLen_bytes:    size of each data element, if -1 then variable size
 *         colType:          Oid of the colType (e.g. char or timestamp, etc.)
 *         align:            char to indicate alignment of the datums
 *         inbuf:            buffer of data to be decompressed
 *         inSizeBytes:      amount of data to be decompressed
 *         outbuf:           buffer into which we should place the
 *                           uncompressed data
 *         outbufSizeBytes:  physical size of the output buffer
 *         outSizeBytes:     amount of uncompressed data actually placed in 
 *                           the output buffer
 *
 *   returns -1 if there is an error that cannot be handled
 *
 *   otherwise returns the number of bytes decompressed
 *   if return is not inSizeBytes then output buffer was not large enough
 *   to hold all the output - it would need to be recalled with an output buffer
 *   large enough to hold the entire output
 *
 *   Function cannot just be recalled on remaining data unless we pass in a 
 *   parameter that says not to expect a block header in subsequent calls 
 *
 *   if outSizeBytes is 0 there wasn't room in the outbuf to make any 
 *   forward progress 
 *   in the worst casse scenario, the output buffer may need to be 
 *   typeLen_bytes*257 bytes in order to decompress aa255 
 */
int
rle_type_decomp(
	int typeLen_bytes,
	Oid colType,
	char align,
	char * inbuf,
	int inSizeBytes,
	char * outbuf,
	int outbufSizeBytes,
	int * outSizeBytes)
{


	char                * lastDatum = NULL;
	char				* thisDatum = NULL;
	char                * data = NULL;
	int                 finishedIndex = 0;
	int                 dataIndex = 0;
	int                 outIndex = 0;
	int                 dataSizeBytes = 0;
	int                 headerNullSize = 0;
	int                 thisDatumLen = 0;
	int                 lastDatumLen = 0;
	int                 nDatum = 0;
	bool                endOfData = false;
	char                * nextInput = NULL;
	char                * nextOutput = NULL;
	char                * adjustedNextOutput = NULL;
	char                *thisDatumOutput = NULL;
	int                 newDatumLen = 0;
	unsigned char       repeatDataCount = 0;
	int                 bytesPadding = 0;


	if (typeLen_bytes == 0)
	{
		return -1;
	}

#ifdef USER_LEVEL_TESTING
	
	data = inbuf;
	dataSizeBytes = inSizeBytes;
	headerNullSize = 0;
	
#else
	DatumStreamBlock	* block = NULL;
	block = (DatumStreamBlock*) inbuf;
	

	/* 
	 * move past the block header and Null Bit map at the beginning
	 * of the block 
	 * we just want to compress the actual data elements
	 */
	headerNullSize = sizeof(DatumStreamBlock) + block->nullsz;
	data = inbuf + headerNullSize;
	dataSizeBytes = inSizeBytes - headerNullSize;

	/* copy the hedaer and null bit map unchanged over to the output buffer */
	memcpy(outbuf, inbuf, headerNullSize);

	if (Debug_rle_type_compression)
	{
		elog(LOG, 
			 "rle_type_decomp START typeLen_bytes %d colType %d align %c inSizeBytes %d outbufSizeBytes %d ndatum %d headerNullSize %d dataSizeBytes %d\n",
			 typeLen_bytes,
			 colType,
			 align,
			 inSizeBytes,
			 outbufSizeBytes,
			 block->ndatum,
			 headerNullSize,
			 dataSizeBytes);
	}

	if (headerNullSize == inSizeBytes)
	{
		/* 
		 * all that is in this buffer is the DatumStreamBlock header 
		 * and the null bit map - no more work to do 
		 * oddly I have seen block->ndatum be 1 in this case?
		 */
		*outSizeBytes = headerNullSize;
		return inSizeBytes;

	}

#endif

	/* we've copied headerNullSize bytes to the outbuf */
	outIndex = headerNullSize;
	*outSizeBytes = outIndex;
	/* we've processed headerNullSize bytes of the inbuf */
	finishedIndex = headerNullSize;

	/* 
	 * point lastDatum to the first data element in the input buffer 
	 * assumes this starts with an item and not a repeatDataCount or 
	 * a second item 
	 */
	lastDatum = NULL;
	lastDatumLen = 0;
	dataIndex = 0;

	endOfData = false;	
	while (!endOfData)
	{
		bool thisDatumMatchesLast = false;
		
		if (dataIndex == dataSizeBytes)
		{
			/* 
			 * we have completely processed all
			 * the input data and should write
			 * out the last compressed run 
			 */
			endOfData = true;
			thisDatum = NULL;
			thisDatumLen = 0;
			thisDatumMatchesLast = false;
#ifndef USER_LEVEL_TESTING
			if (Debug_rle_type_compression)
			{
				elog(LOG, 
					 "rle_type_decomp: dataIndex == dataSizeBytes %d ",
					 dataIndex);
			}
#endif
		} 
		else 
		{
			/* there are more bytes in the input */

			/* 
			 * set the thisDatum to the next
			 * element in the data buffer 
			 */
			thisDatum = &(data[dataIndex]);
			
			if (typeLen_bytes == VARIABLE_LENGTH) 
			{
#ifdef USER_LEVEL_TESTING
				/* 
				 * for user level testing assume it is one byte
				 * len followed by data
				 */
				unsigned char len = *((unsigned char*) thisDatum);
				thisDatumLen = len +1;
#else
				thisDatumLen = VARSIZE_ANY((struct varlena *) thisDatum);
#endif
			} 
			else 
			{
				thisDatumLen = 	typeLen_bytes;
			}

			/* ADJUST thisDatumLen for alignment */
#ifndef USER_LEVEL_TESTING

			thisDatumOutput  = &(outbuf[outIndex]);
			
			nextInput = thisDatum + thisDatumLen;
			nextOutput = thisDatumOutput + thisDatumLen;
			adjustedNextOutput = thisDatumOutput + thisDatumLen;
			
			if ((typeLen_bytes == VARIABLE_LENGTH) &&(*nextInput == 0))
			{
				
				adjustedNextOutput = 	(char *) att_align(adjustedNextOutput, align);

				newDatumLen = adjustedNextOutput - thisDatumOutput;
				
				if ((newDatumLen != thisDatumLen) &&
					(dataIndex + newDatumLen <=dataSizeBytes))
				{
					Assert(newDatumLen > thisDatumLen);
					/* bytesPadding calculated to aid in debugging */
					bytesPadding = newDatumLen - thisDatumLen;
					if (Debug_rle_type_compression)
					{
						elog(LOG, 
							 "rle_type_decomp: thisDatumLen %d bytesPadding %d newDatumLen %d",
							 thisDatumLen,
							 bytesPadding,
							 newDatumLen);
					}
					thisDatumLen = newDatumLen;
					
				}
			}			
#endif

#ifndef USER_LEVEL_TESTING
			if (Debug_rle_type_compression)
			{
				elog(LOG, 
					 "rle_type_decomp: thisDatumLen %d ",
					 thisDatumLen);
			}
#endif
			if (dataIndex + thisDatumLen > dataSizeBytes)
			{
				/* 
				 * data not long enough to include this whole datum
				 * - must be garbage at end of input
				 * We used to just assert
				 * Assert (dataIndex + thisDatumLen <= dataSizeBytes);
				 * This is a more defensive version that will 
				 * continue to behave well even if the input is 
				 * malformed
				 */
				endOfData = true;
				thisDatum = NULL;
				thisDatumLen = 0;
				thisDatumMatchesLast = false;

#ifndef USER_LEVEL_TESTING
				if (Debug_rle_type_compression)
				{
					elog(LOG, 
						 "rle_type_decomp: not enough input left for full datum dataIndex %d thisDatumLen %d dataSizeBytes %d dataSizeBytes- dataIndex %d",
						 dataIndex,
						 thisDatumLen,
						 dataSizeBytes,
						 dataSizeBytes- dataIndex
						);

					elog(LOG,
						 "rle_type_decomp: garbage past data in input finishedIndex %d inSizeBytes %d inSizeBytes- finishedIndex %d",
						 finishedIndex,
						 inSizeBytes,
						 inSizeBytes - finishedIndex);
				}
#endif
				/* 
				 * compress the garbage by just ignoring and 
				 * not copying it over to the output buffer 
				 */
				finishedIndex = inSizeBytes;

#ifndef USER_LEVEL_TESTING
			} 
			else if (nDatum == block->ndatum) 
			{

				endOfData = true;
				thisDatum = NULL;
				thisDatumLen = 0;
				thisDatumMatchesLast = false;

				if (Debug_rle_type_compression)
				{
					elog(LOG, 
						 "rle_type_decomp: we've processed all of block->ndatum %d; garbage past data in input finishedIndex %d inSizeBytes %d inSizeBytes- finishedIndex %d",
						 block->ndatum,
						 finishedIndex,
						 inSizeBytes,
						 inSizeBytes - finishedIndex);

				}

				/* 
				 * we've already processed all the datum in this block
				 * but there is still garbage data passed it
				 * can we safely skip the garbage data and *not* copy it over
				 * to the output buffer - compress it by removing it completely
				 */
				finishedIndex = inSizeBytes;
				
#endif			
			} 
			else 
			{

				/* compare thisDatum to lastDatum */
				if (thisDatumLen == lastDatumLen)
				{
					if (memcmp(thisDatum, lastDatum, thisDatumLen) == 0)
					{
						thisDatumMatchesLast = true;
					} 
					else 
					{
						thisDatumMatchesLast = false;
					}
				
				} 
				else
				{
					thisDatumMatchesLast = false;
				}
			}
			
		}
		
		/* move dataIndex past thisDatum */
		dataIndex+= thisDatumLen;

		if (thisDatumMatchesLast)
		{
			int i;
		
			/* 
			 * two of the same in a row, expect to 
			 * read repeatDataCount next and then expand
			 */
			repeatDataCount = (unsigned char) data[dataIndex];

			/* 
			 * check for overflow - 
			 * we need room for repeatDataCount items of size
			 * thisDatumLen - the first one in this run will
			 * be written out seperately 
			 */
			if (outIndex+(repeatDataCount)*thisDatumLen > outbufSizeBytes)
			{
				/* 
				 * if we have to return without making
				 * forward progress we could return -X where X is number of
				 * additional bytes we would need 
				 */
		
				*outSizeBytes = outIndex;
				/* 
				 * can't be recalled unless we pass a parameter into 
				 * subsequent calls that says not to expect a
				 * block header - but don't just return -1
				 * because upper layer needs to know difference
				 * between fundamental error and not room in output
				 * buffer to handle all output
				 */
				return finishedIndex;
					
			}

			for (i = 0; i< (int)repeatDataCount; i++)
			{
				nDatum++;
				memcpy(&(outbuf[outIndex]), lastDatum, lastDatumLen);
				outIndex+=lastDatumLen;

#ifdef USER_LEVEL_TESTING
				if (Debug_rle_type_compression && (typeLen_bytes == 1) )
				{
					printf("%c", *lastDatum);
				}
#endif
			}

			/* 
			 * move past the 1 bytes of len in the input
			 * that we just consumed
			 */
			dataIndex+=1;

			/* 
			 * add both this lastDaumLen and the 1 byte for len
			 * to the finishedIndex 
			 */
			finishedIndex += (lastDatumLen +1);



#ifndef USER_LEVEL_TESTING
			if (Debug_rle_type_compression)
			{
				elog(LOG, 
					 "rle_type_decomp nDatum %d numOutput %d finishedIndex %d dataIndex %d lastDatumLen %d\n",
					 nDatum,
					 repeatDataCount,
					 finishedIndex,
					 dataIndex,
					 lastDatumLen
					 );
			}
#endif

			/*
			 * we don't want the next datum to match this one
			 * this <datum,datum, count> sequence is over
			 * and we must look for another 
			 */
			lastDatumLen = 0;
			

		}
		else if (!endOfData)
		{
			/* 
			 *  thisDatum does not match the last one so 
			 *  write out the last one as is to outbuf 
			 */
			
			/* 
			 *first check for overflow 
			 *  we need to write lastDatumLen bytes of space
			 */
			if (outIndex+thisDatumLen > outbufSizeBytes)
			{
				*outSizeBytes = outIndex;
				/* 
				 * can't be recalled unless we pass a parameter into 
				 * subsequent calls that says not to expect a
				 * block header - but don't just return -1
				 * because upper layer needs to know difference
				 * between fundamental error and not room in output
				 * buffer to handle all output
				 */
				return finishedIndex;
			}

			nDatum++;
			memcpy(&(outbuf[outIndex]), thisDatum, thisDatumLen);
			outIndex+=thisDatumLen;
			*outSizeBytes = outIndex;

			finishedIndex += thisDatumLen;
		
#ifndef USER_LEVEL_TESTING
			if (Debug_rle_type_compression)
			{
				elog(LOG, 
					 "rle_type_decomp nDatum %d first instance (finishedIndex %d)\n",
					 nDatum,
					 finishedIndex);
			}
#endif

			

			lastDatum = thisDatum;
			lastDatumLen = thisDatumLen;

#ifdef USER_LEVEL_TESTING
			if (Debug_rle_type_compression && (typeLen_bytes == 1) )
			{
				printf("%c", *lastDatum);
			}
#endif
		}
	}


	*outSizeBytes = outIndex;

#ifndef USER_LEVEL_TESTING
	if (rle_type_compression_stats)
	{
		elog(LOG, 
			 "rle_type_decomp END colType %d align %c inSizeBytes %d outsizeBytes %d dataSizeBytes %d compression ratio TOTAL %0.2f space savings %0.2f WITHOUT_HEADER_OVERHEAD compression ratio %0.2f space savings %0.2f\n",
			 colType,
			 align,
			 inSizeBytes,
			 outIndex,
			 dataSizeBytes,
			 ((float)(inSizeBytes)/(float) outIndex) *100.0,
			 100.0 - (((float)(inSizeBytes)/(float) outIndex) *100.0),
			 ((float)(inSizeBytes-headerNullSize)/
			  (float) (outIndex-headerNullSize)) *100.0,
			 100.0 - (((float)(inSizeBytes-headerNullSize)/
				   (float) (outIndex-headerNullSize)) *100.0)
			);
	}
#endif
	
	/* 
	 * we should have finished all the input
	 * and processed exactly the number of datum
	 * as described in the block header
	 * only thing that should cause these assertions
	 * would be if there was garbage past the data values 
	 */
#ifndef USER_LEVEL_TESTING
	if (Debug_rle_type_compression)
	{
		if (finishedIndex != inSizeBytes)
		{
			elog(LOG, 
				 "rle_type_decomp: finishedIndex %d != inSizeBytes %d\n",
				 finishedIndex,
				 inSizeBytes);
		}
	}
#endif
	Assert(finishedIndex == inSizeBytes);

#ifndef USER_LEVEL_TESTING
	if (nDatum != block->ndatum )
	{
		elog( LOG, "rle_type_decomp: nDatum %d != block->ndatum %d",
			  nDatum,
			  block->ndatum);
	}
	Assert (nDatum == block->ndatum);
#endif
	return finishedIndex;
}

/*** BEGIN DRIVER FOR TESTING ***/
#ifdef USER_LEVEL_TESTING
void
printUsage(char * prog)
{
	printf("Usage: %s c/d typeLenBytes <infile> <outfile>\n", prog);
	printf("c = compress; d = decompress\n", prog);
}

#define BUFFER_SIZE 8192

int
main(int argc, char ** argv)
{

	FILE * infile, *outfile;
	char * infileName,  *outfileName;
	char inbuf[BUFFER_SIZE];
	char outbuf[BUFFER_SIZE];

	int thisBytesIn = 0;
	int totalBytesIn = 0;

	int thisBytesOut = 0;
	int totalBytesOut = 0;

	int thisBytesProcessed = 0;
	int totalBytesProcessed = 0;

	int typeLenBytes;
	int numBlocks = 0;

	if (argc != 5)
	{
		printUsage(argv[0]);
		exit(1);
	}

	if ((strcmp(argv[1], "c") != 0) &&  (strcmp(argv[1], "d") != 0))
	{
		printUsage(argv[0]);
		exit(1);

	}

	typeLenBytes = atoi(argv[2]);
	if (typeLenBytes <=0)
	{
		printf("%d is not a valid typelen.\n",
			   typeLenBytes);
		printUsage(argv[0]);
		exit (1);
	}

	infile = fopen (argv[3], "rb");
	if ( infile == NULL)
	{
		printf("Unable to open file %s", argv[3]);
		exit (1);
	}

	outfile = fopen (argv[4], "wb");
	if ( outfile == NULL)
	{
		printf("Unable to open file %s", argv[4]);
		exit (1);
	}



	/* WARNING:
	   In our code base, we will compress/decompress one block at a time

	   if a run of the same item spans block boundaries it will just be 
	   treated as two separate runs

	   In the database, the compressed data from one block is handed back for
	   decompression separately - you never get some from one block and some
	   from another

	   In this driver that is not true and introduces problems - 
	   consider this case:

	   you have a run of 8 w's that spans a block boundary

	   one w is in the end of the first block and so is compressed as one w

	   the rest of them (seven w's) are at the beginning of the second block 
	   and are compressed as ww5

	   if we respect block boundaries (as we do inthe DB) we are fine
	   w -> w
	   ww5 -> wwwwwww

	   however if they run together we get

	   www5 -> this will result in w+2 w's and then a 5 because the first 
	   two w's flag the start of a run and so the third w is treated as the 
	   repeatDataCount field then the 2 again is treated as an item

	   THEREFORE it is important to understand that this driver should work
	   correctly for any file less than the BLOCK_SIZE

	   over the BLOCK_SIZE this error may appear and the decompressed file
	   may not match the original

	   it still may be usefile for performance testing with large files 
	   outside the database but be aware of this limitation - compression 
	   works fine and should give the right size/speed
	   it is decompression that can go wrong if the block boundaries are 
	   not known

	   If the first character of the current output block matches the last 
	   output character of the last block then this problem can be triggered

	*/

	while (!feof(infile))
	{
		numBlocks++;
		thisBytesIn = fread(inbuf, 1, BUFFER_SIZE, infile);

		if (thisBytesIn == 0)
		{
			continue;
		}

		totalBytesIn += thisBytesIn;
		totalBytesProcessed = 0;

		while (totalBytesProcessed < thisBytesIn)
		{

			if (strcmp(argv[1], "c") == 0)
			{
				thisBytesProcessed = rle_type_comp(
					typeLenBytes,
					0,
					'x',
					&(inbuf[totalBytesProcessed]),
					thisBytesIn-totalBytesProcessed,
					outbuf,
					BUFFER_SIZE,
					&thisBytesOut);
			} 
			else 
			{
				thisBytesProcessed = rle_type_decomp(
					typeLenBytes,
					0,
					'x',
					&(inbuf[totalBytesProcessed]),
					thisBytesIn-totalBytesProcessed,
					outbuf,
					BUFFER_SIZE,
					&thisBytesOut);
			}

			if (Debug_rle_type_compression)
			{
				printf("\ntotalBytesProcessed %d thisBytesIn %d leftToDo %d thisBytesProcessed %d, thisBytesOut %d\n",
					   totalBytesProcessed,
					   thisBytesIn,
					   thisBytesIn-totalBytesProcessed,
					   thisBytesProcessed,
					   thisBytesOut);
			}
			if (thisBytesProcessed < 0)
			{
				printf(
					"Fatal error file after %d bytes input/ %d bytes output\n",
					totalBytesIn,
					totalBytesOut);
				exit (1);
			}
			else if (thisBytesProcessed == 0)
			{
				printf(
					"Unable to make forward progress after %d bytes input/ %d bytes output\n",
					totalBytesIn,
					totalBytesOut);
				exit (1);
			}
			fwrite (outbuf, 1, thisBytesOut, outfile);
			totalBytesOut += thisBytesOut;

			totalBytesProcessed += thisBytesProcessed;
		}
	}

	if (numBlocks > 1)
	{
		printf("WARNING: compress/decompress meant only for use 1 block at a time; when decompressed data from multiple compressed blocks is combined together errors can be introduced\n");
		printf("See source code comment for details\n");
	}

	printf("%d bytes in; %d bytes out\n",
		   totalBytesIn,
		   totalBytesOut);
	fclose(outfile);
	fclose(infile);
	return 0;
}

#endif
/*** END DRIVER FOR TESTING ***/
