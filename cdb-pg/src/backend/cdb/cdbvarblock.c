/*-------------------------------------------------------------------------
 *
 * cdbvarblock.c
 *	  A general data package that has variable-length item array that can be
 *    efficiently indexed once complete.
 *
 * (See .h file for usage comments)
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "cdb/cdbvarblock.h"

static VarBlockByteLen VarBlockGetItemLen(
    VarBlockReader      *varBlockReader,
    int                 itemIndex);
static void VarBlockGetItemPtrAndLen(
    VarBlockReader      *varBlockReader,
    int                 itemIndex,
    uint8				**itemPtr,
    VarBlockByteLen		*itemLen);


/*
 * Initialize the VarBlock maker.
 *
 * Since we are going to pack the item offset array
 * right after the variable-length item array when
 * finished, we need a temporary buffer for the
 * offsets while we are making the block.  The caller
 * supplies the tempScratchSpace parameter for this
 * purpose.  Note that tempScratchSpaceLen in effect
 * serves as the maximum number of items (UNDONE: more 2-byte vs. 4-byte).
 */
void VarBlockMakerInit(
    VarBlockMaker        *varBlockMaker,
    uint8                *buffer,
    VarBlockByteLen      maxBufferLen,
    uint8                *tempScratchSpace,
    int                  tempScratchSpaceLen)
{
	Assert(varBlockMaker != NULL);
	Assert(buffer != NULL);								// UNDONE: And ALIGN(4)
	Assert(maxBufferLen >= sizeof(VarBlockHeader));		// UNDONE: And even.
	Assert(tempScratchSpace != NULL);					// UNDONE: And ALIGN(4).
	Assert(tempScratchSpaceLen > 0);					// UNDONE: And even.
	
	varBlockMaker->header = (VarBlockHeader*)buffer;
    varBlockMaker->maxBufferLen = maxBufferLen;
    varBlockMaker->currentItemLenSum = 0;

    varBlockMaker->tempScratchSpace = tempScratchSpace;
    varBlockMaker->tempScratchSpaceLen = tempScratchSpaceLen;

    varBlockMaker->nextItemPtr = &buffer[sizeof(VarBlockHeader)];
	// UNDONE: Fix the constant to be the $MIN of approx. 16k (or 32k)
	// UNDONE: and maxBufferLen...
    varBlockMaker->last2ByteOffsetPtr = &buffer[10 * 1024];
    varBlockMaker->currentItemCount = 0;
    varBlockMaker->maxItemCount = tempScratchSpaceLen / 2;

	memset(buffer, 0, sizeof(VarBlockHeader));
	varBlockMaker->header->version = InitialVersion;
	varBlockMaker->header->offsetsAreSmall = true;
}

/*
 * Get a pointer to the next variable-length item so it can
 * be filled in.
 *
 * Returns NULL when there is no more space left in the VarBlock.
 */
uint8* VarBlockMakerGetNextItemPtr(
    VarBlockMaker        *varBlockMaker,
    VarBlockByteLen      itemLen)
{
	int currentItemCount;
    uint8 *buffer;
	uint8 *nextItemPtr;
	VarBlockByteOffset nextItemOffset;
	VarBlockByteLen newItemLenSum;
	VarBlockByteLen newTotalLen;
	uint8 *tempScratchSpace;
	int16 *tempScratchOffsetArray16;
	int32 *tempScratchOffsetArray32;
	
	Assert(varBlockMaker != NULL);
	Assert(itemLen >= 0);

	currentItemCount = varBlockMaker->currentItemCount;
	if (currentItemCount >= varBlockMaker->maxItemCount)
	{
		/*
		 * Reached a limit on the number of items based on the
		 * scratch space...
		 */
		return NULL;	
	}

    buffer = (uint8*)varBlockMaker->header;
	nextItemPtr = varBlockMaker->nextItemPtr;
	nextItemOffset = (VarBlockByteOffset)(nextItemPtr - buffer);
	newItemLenSum = varBlockMaker->currentItemLenSum + itemLen;
	tempScratchSpace = 	varBlockMaker->tempScratchSpace;

	/*
	 * We require VarBlocks to given to us on 4 byte boundaries...
	 */
	if (varBlockMaker->header->offsetsAreSmall)
	{
		int newMaxItemCount;
		
		/*
		 * Will small 2 byte offsets still work?
		 */
		if (nextItemPtr > varBlockMaker->last2ByteOffsetPtr)
		{		
			int r;
			
			/*
			 * Try out large 4-byte offsets.
			 */
			newTotalLen = sizeof(VarBlockHeader) +
			              ((newItemLenSum + 3)/4)* 4 +
						  currentItemCount * 4 + 4;
			if (newTotalLen > varBlockMaker->maxBufferLen)
				return NULL;	// Out of space.

    		newMaxItemCount = varBlockMaker->tempScratchSpaceLen / 4;
			if (currentItemCount >= newMaxItemCount)
			{
				/*
				 * Reached a limit on the number of items based on the
				 * scratch space with large offsets...
				 */
				return NULL;	
			}

			varBlockMaker->header->offsetsAreSmall = false;
			varBlockMaker->maxItemCount = newMaxItemCount;

			/*
			 * Reverse move to use 4-byte offsets.
			 */
			tempScratchOffsetArray16 = (int16*)tempScratchSpace;
			tempScratchOffsetArray32 = (int32*)tempScratchSpace;
			for (r = currentItemCount; r >= 0; r--)
			{
				tempScratchOffsetArray32[r] = tempScratchOffsetArray16[r];
			}

			tempScratchOffsetArray32[currentItemCount] = nextItemOffset;
		}
		else
		{
			/*
			 * Normal calculation.
			 */
			newTotalLen = sizeof(VarBlockHeader) +
			              ((newItemLenSum + 1)/2)* 2 +
						  currentItemCount * 2 + 2;
			if (newTotalLen > varBlockMaker->maxBufferLen)
				return NULL;	// Out of space.
				
			tempScratchOffsetArray16 = (int16*)tempScratchSpace;
			tempScratchOffsetArray16[currentItemCount] = (int16)nextItemOffset;
		}
	}
	else
	{
		/*
		 * Large 4 byte ofsets.
		 */
		newTotalLen = sizeof(VarBlockHeader) +
		              ((newItemLenSum + 3)/4)* 4 +
					  currentItemCount * 4 + 4;
		if (newTotalLen > varBlockMaker->maxBufferLen)
			return NULL;	// Out of space.

		tempScratchOffsetArray32 = (int32*)tempScratchSpace;
		tempScratchOffsetArray32[currentItemCount] = nextItemOffset;
	}

	varBlockMaker->nextItemPtr += itemLen;
	varBlockMaker->currentItemCount++;
	varBlockMaker->currentItemLenSum = newItemLenSum;

	return nextItemPtr;
}

/*
 * Get the variable-length item count.
 */
int VarBlockMakerItemCount(
    VarBlockMaker *varBlockMaker)
{
	Assert(varBlockMaker != NULL);
	
	return varBlockMaker->currentItemCount;
}

/*
 * Finish making the VarBlock.
 *
 * The item-offsets array will be added to the end.
 */
VarBlockByteLen VarBlockMakerFinish(
    VarBlockMaker *varBlockMaker)
{
    uint8 *buffer;
	int itemCount;
	VarBlockByteLen itemLenSum; 
	uint8 *tempScratchSpace;
	VarBlockByteLen headerAndItemsLen;
	VarBlockByteOffset offsetToOffsetArray;
	VarBlockByteLen totalLen;
	int z;
	
	Assert(varBlockMaker != NULL);

    buffer = (uint8*)varBlockMaker->header;
	itemCount = varBlockMaker->currentItemCount;
	itemLenSum = varBlockMaker->currentItemLenSum;
	
	tempScratchSpace = varBlockMaker->tempScratchSpace;

	headerAndItemsLen = sizeof(VarBlockHeader) + itemLenSum;
	
	if (varBlockMaker->header->offsetsAreSmall)
	{
		/*
		 * Round to 2-byte boundary.
		 */
		offsetToOffsetArray = ((headerAndItemsLen + 1)/2) * 2;

		memcpy(&buffer[offsetToOffsetArray], tempScratchSpace, itemCount * 2);

		totalLen = offsetToOffsetArray + itemCount * 2;
	}
	else
	{
		/*
		 * Round to 4-byte boundary.
		 */
		offsetToOffsetArray = ((headerAndItemsLen + 3)/4) * 4;
		
		memcpy(&buffer[offsetToOffsetArray], tempScratchSpace, itemCount * 4);

		totalLen = offsetToOffsetArray + itemCount * 4;
	}

	/*
	 * Zero the pad between the last item and the offset array for data
	 * security.
	 */
	for (z = headerAndItemsLen; z < offsetToOffsetArray; z++)
	{
		buffer[z] = 0;
	}

	varBlockMaker->header->itemLenSum = itemLenSum;
	varBlockMaker->header->itemCount = itemCount;
	
	return totalLen;
}

/*
 * Reset the VarBlock maker so it can make another one
 * using the same inputs as given to VarBlockMakerInit.
 */
void VarBlockMakerReset(
    VarBlockMaker *varBlockMaker)
{
    uint8 *buffer;

	Assert(varBlockMaker != NULL);
	
    buffer = (uint8*)varBlockMaker->header;
	
	memset(buffer, 0, sizeof(VarBlockHeader));
	varBlockMaker->header->version = InitialVersion;
	varBlockMaker->header->offsetsAreSmall = true;
	
    varBlockMaker->currentItemLenSum = 0;

    varBlockMaker->nextItemPtr = &buffer[sizeof(VarBlockHeader)];
    varBlockMaker->currentItemCount = 0;
    varBlockMaker->maxItemCount = varBlockMaker->tempScratchSpaceLen / 2;

}

/*
 * Initialize the VarBlock reader.
 */
void VarBlockReaderInit(
    VarBlockReader      *varBlockReader,
    uint8               *buffer,
    VarBlockByteLen     bufferLen)
{
	VarBlockHeader *header;
	VarBlockByteLen itemLenSum;
	VarBlockByteOffset offsetToOffsetArray;
	int divisor;
	int calculatedItemCount;
	
	Assert(varBlockReader != NULL);
	Assert(buffer != NULL);								// UNDONE: And ALIGN(4)
	Assert(bufferLen >= sizeof(VarBlockHeader));		// UNDONE: And even.
	
	header = (VarBlockHeader*)buffer;
	
	varBlockReader->header = header;
    varBlockReader->bufferLen = bufferLen;

	// UNDONE: Check itemLenSum against bufferLen...
	itemLenSum = header->itemLenSum;
	
	if (header->offsetsAreSmall)
	{
		offsetToOffsetArray = sizeof(VarBlockHeader) +
				              ((itemLenSum + 1)/2)* 2;

		divisor = 2;
	}
	else
	{
		offsetToOffsetArray = sizeof(VarBlockHeader) +
				              ((itemLenSum + 3)/4)* 4;
		
		divisor = 4;
	}
    calculatedItemCount = (bufferLen - offsetToOffsetArray) / divisor;
	Assert(calculatedItemCount == header->itemCount);
	
    varBlockReader->firstOffsetPtr = &buffer[offsetToOffsetArray];
    varBlockReader->nextIndex = 0;
    varBlockReader->nextItemPtr = &buffer[sizeof(VarBlockHeader)];
}

/*
 * Set the position to a variable-length item.
 *
 * The next call to VarBlockReaderGetNextItemPtr will
 * get the specified item.
 */
void VarBlockReaderPosition(
    VarBlockReader      *varBlockReader,
    int                 itemIndex)
{
	VarBlockByteLen itemLen;
	
	Assert(varBlockReader != NULL);
	Assert(itemIndex >= 0);
	Assert(itemIndex < varBlockReader->header->itemCount);

	VarBlockGetItemPtrAndLen(varBlockReader, itemIndex, 
		                     &varBlockReader->nextItemPtr, &itemLen);
	
	varBlockReader->nextIndex = itemIndex;
}

static VarBlockByteLen VarBlockGetItemLen(
    VarBlockReader      *varBlockReader,
    int                 itemIndex)
{
	VarBlockHeader *header;
	VarBlockByteLen itemLen;
	
	Assert(varBlockReader != NULL);
	Assert(itemIndex >= 0);

	header = varBlockReader->header;

	if (header->offsetsAreSmall)
	{
		int16 *offsetArray16 = (int16*)varBlockReader->firstOffsetPtr;
		int16 offset16 = offsetArray16[itemIndex];
		if (offset16 < 0)
		{
			fprintf(stderr, 
				    "Offset16 for itemIndex %d is less than 0 (%d).\n",
				    itemIndex, offset16);
			exit(1);
		}
		if (itemIndex < header->itemCount - 1)
		{
			int32 nextOffset16 = offsetArray16[itemIndex + 1];
			Assert(nextOffset16 > 0);
			
			itemLen = nextOffset16 - offset16; 
			Assert(itemLen > 0);
		}
		else
		{
			Assert(itemIndex == header->itemCount - 1);
			itemLen = sizeof(VarBlockHeader) + header->itemLenSum - offset16;
			Assert(itemLen > 0);
		}
	}
	else
	{
		int32 *offsetArray32 = (int32*)varBlockReader->firstOffsetPtr;
		int32 offset32 = offsetArray32[itemIndex];
		Assert(offset32 > 0);
		if (itemIndex < header->itemCount - 1)
		{
			int32 nextOffset32 = offsetArray32[itemIndex + 1];
			Assert(nextOffset32 > 0);
			
			itemLen = nextOffset32 - offset32; 
			Assert(itemLen > 0);
		}
		else
		{
			Assert(itemIndex == header->itemCount - 1);
			itemLen = sizeof(VarBlockHeader) + header->itemLenSum - offset32;
			Assert(itemLen > 0);
		}
	}

	return itemLen;
}

static void VarBlockGetItemPtrAndLen(
    VarBlockReader      *varBlockReader,
    int                 itemIndex,
    uint8				**itemPtr,
    VarBlockByteLen		*itemLen)
{
	VarBlockHeader *header;
	uint8 *buffer;
	
	Assert(varBlockReader != NULL);

	header = varBlockReader->header;
	buffer = (uint8*)header;

	if (header->offsetsAreSmall)
	{
		int16 *offsetArray16 = (int16*)varBlockReader->firstOffsetPtr;
		int16 offset16 = offsetArray16[itemIndex];
		Assert(offset16 > 0);
		if (itemIndex < header->itemCount - 1)
		{
			int32 nextOffset16 = offsetArray16[itemIndex + 1];
			Assert(nextOffset16 > 0);
			
			*itemLen = nextOffset16 - offset16; 
			Assert(*itemLen > 0);
		}
		else
		{
			Assert(itemIndex == header->itemCount - 1);
			*itemLen = sizeof(VarBlockHeader) + header->itemLenSum - offset16;
			Assert(*itemLen > 0);
		}

		*itemPtr = &buffer[offset16];
	}
	else
	{
		int32 *offsetArray32 = (int32*)varBlockReader->firstOffsetPtr;
		int32 offset32 = offsetArray32[itemIndex];
		Assert(offset32 > 0);
		if (itemIndex < header->itemCount - 1)
		{
			int32 nextOffset32 = offsetArray32[itemIndex + 1];
			Assert(nextOffset32 > 0);
			
			*itemLen = nextOffset32 - offset32; 
			Assert(*itemLen > 0);
		}
		else
		{
			Assert(itemIndex == header->itemCount - 1);
			*itemLen = sizeof(VarBlockHeader) + header->itemLenSum - offset32;
			Assert(*itemLen > 0);
		}

		*itemPtr = &buffer[offset32];
	}
}


/*
 * Get a pointer to the next variable-length item.
 *
 * Returns NULL when there are no more items.
 */
uint8* VarBlockReaderGetNextItemPtr(
    VarBlockReader      *varBlockReader,
    VarBlockByteLen     *itemLen)
{
	uint8 *nextItemPtr;
	
	Assert(varBlockReader != NULL);
	Assert(itemLen != NULL);
	
	if (varBlockReader->nextIndex >= varBlockReader->header->itemCount)
	{
		return NULL;
	}
	
	*itemLen = VarBlockGetItemLen(varBlockReader, varBlockReader->nextIndex);
	nextItemPtr = varBlockReader->nextItemPtr;

	varBlockReader->nextItemPtr += *itemLen;
	varBlockReader->nextIndex++;
	
	return nextItemPtr;
}

/*
 * Get the variable-length item count.
 */
int VarBlockReaderItemCount(
    VarBlockReader *varBlockReader)
{
	Assert(varBlockReader != NULL);
	
	return varBlockReader->header->itemCount;
}

/*
 * Get a pointer to a variable-length item.
 */
uint8* VarBlockReaderGetItemPtr(
    VarBlockReader      *varBlockReader,
    int                 itemIndex,
    VarBlockByteLen     *itemLen)
{
	uint8 *nextItemPtr;
	
	Assert(varBlockReader != NULL);
	Assert(itemIndex >= 0);
	Assert(itemIndex < varBlockReader->header->itemCount);
	
	VarBlockGetItemPtrAndLen(varBlockReader, itemIndex, &nextItemPtr, itemLen);
	
	return nextItemPtr;
}


