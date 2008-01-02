/*-------------------------------------------------------------------------
 *
 * cdbvarblock.h
 *	  A general data package that has variable-length item array that can be
 *    efficiently indexed once complete.
 *
 *  The variable-length items are byte sized.
 *
 *  Alignment:
 *      The first item will be aligned on an eight-byte (64 bit) boundary.
 *      If all items are a multiple or 8, 4, 2 in length – then they will
 *      be aligned on those multiples.  If the items are any length, then
 *      the alignment is 1 byte.
 *
 *  For efficient VarBlock making, the client is given a pointer directly
 *  into the VarBlock buffer and the client fills their items directly.
 *
 *  Examples:
 *
 *      #define MY_VARBLOCK_TEMP_SCRATCH_LEN 2000
 *
 *      // Make a VarBlock in the buffer from some records.
 *		VarBlockByteLen MakerExample(
 *		        byte *buffer, VarBlockByteLen maxBufferLen,
 *		        Records *records, int numRecords)
 *		{
 *		    VarBlockMaker  myVarBlockMaker;
 *		    uint8 tempScratch [MY_VARBLOCK_TEMP_SCRATCH_LEN];
 *		    byte *item;
 *
 *		    VarBlockMakerInit(
 *		        &myVarBlockMaker,
 *		        buffer,
 *		        maxBufferLen,
 *		        tempScratch,
 *		        MY_VARBLOCK_TEMP_SCRATCH_LEN);
 *
 *		    for (int i = 0; i < numRecords; i++)
 *		    {
 *		        item = VarBlockMakerGetNextItemPtr(
 *		                       &myVarBlockMaker,
 *		                       records[i].length);
 *		        Assert(item != NULL);   // Real code would deal with NULL
 *		                                // indicating VarBlock full...
 *		                                
 *		        MemCpy(item,records[i].data,records[i].length);
 *		    }
 *
 *		    return VarBlockMakerFinish(&myVarBlockMaker);
 *		}
 *
 *		// Extract records out of VarBlock.
 *		void ReaderExample(
 *		        byte *buffer, VarBlockByteLen bufferLen,
 *		        Records *records, int *numRecords)
 *		{
 *		    VarBlockReader  myVarBlockReader;
 *		    byte *item;
 *
 *		    VarBlockReaderInit(
 *		        &myVarBlockReader,
 *		        buffer,
 *		        bufferLen);
 *
 *		    *numRecords = VarBlockReaderItemCount(&myVarBlockReader);
 *
 *		    for (int i = 0; i < *numRecords; i++)
 *		    {
 *		        item = VarBlockMakerGetNextItemPtr(
 *		                       &myVarBlockMaker,
 *		                       &records[i].length);
 *		        Assert(item != NULL);
 *
 *		        //
 *		        // Keep a pointer to each item.
 *		        //
 *		        records[i].data = item;
 *		    }
 *       }
 *        
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBVARBLOCK_H
#define CDBVARBLOCK_H

#include "postgres.h"

typedef int32 VarBlockByteLen;
typedef int32 VarBlockByteOffset;

// ----------------------------------------------------------
typedef enum VarBlockVersion
{
	InitialVersion = 1
} VarBlockVersion;


// UNDONE: Make sure it packs to 64 bits exactly.
typedef struct VarBlockHeader

{
    unsigned  		offsetsAreSmall:1;  
                           /* True if the offsets are small and occupy 2 bytes.
                            * Otherwise, the block is large and 4 byte offsets
                            * are used.
                            */
    unsigned  		reserved:13; 
						   /* Reserved for future. */
    VarBlockVersion version:2;  
						   /* Version number */

    unsigned  		itemLenSum:24;
                           /* Offset to the item offset array. */

    unsigned  		itemCount:24;
                           /* Total number of items. */

} VarBlockHeader;

// ----------------------------------------------------------

typedef struct VarBlockMaker
{
	VarBlockHeader       *header;
							/* The buffer with the header at the beginning. */
							
    VarBlockByteLen      maxBufferLen;
							/* The maximum amount of space that can be used
							 * for the VarBlock.
							 */
	
    VarBlockByteLen      currentItemLenSum;
							/* The running sum of the item data lengths. */

    uint8                *tempScratchSpace;
    int                  tempScratchSpaceLen;
							/* The scratch space where we will temporarily
							 * keep the item offsets while making the
							 * VarBlock. 
							 */

    uint8                *nextItemPtr;
							/* Pointer in the buffer to the beginning of
							 * the next item.
							 */
							 
	uint8                *last2ByteOffsetPtr;
							/* The boundary point where we need to switch
							 * from 2-byte offsets to 4-bytes.
							 */
							 
    int                  currentItemCount;
							/* The running count of items. */
							
	int                  maxItemCount;
							/* The maximum number of items for the VarBlock.
							 * Based on the length of the scratch area.
							 */
} VarBlockMaker;

// ----------------------------------------------------------

typedef struct VarBlockReader
{
	VarBlockHeader       *header;
							/* The buffer with the header at the beginning. */
							
    VarBlockByteLen      bufferLen;
							/* The exact byte length of this VarBlock. */

    int                  nextIndex;
    uint8                *nextItemPtr;
							/* The index and pointer when doing get-next
							 * scanning of the VarBlock.
							 */
							 
    uint8                *firstOffsetPtr;
							/* Pointer to the beginning of the offset array. */
							
} VarBlockReader;

/*
 * Initialize the VarBlock maker.
 *
 * Since we are going to pack the item offset array
 * right after the variable-length item array when
 * finished, we need a place to buffer the
 * offsets while we are making the block.  The caller
 * supplies the tempItemOffsets parameter for this
 * purpose.  Note that maxTempItemOffsets in effect
 * serves as the maximum number of items.
 */
extern void VarBlockMakerInit(
    VarBlockMaker        *varBlockMaker,
    uint8                *buffer,
    VarBlockByteLen      maxBufferLen,
    uint8                *tempScratchSpace,
    int                  tempScratchSpaceLen);

/*
 * Get a pointer to the next variable-length item so it can
 * be filled in.
 *
 * Returns NULL when there is no more space left in the VarBlock.
 */
extern uint8* VarBlockMakerGetNextItemPtr(
    VarBlockMaker        *varBlockMaker,
    VarBlockByteLen      itemLen);

/*
 * Get the variable-length item count.
 */
extern int VarBlockMakerItemCount(
    VarBlockMaker *varBlockMaker);

/*
 * Finish making the VarBlock.
 *
 * The item-offsets array will be added to the end.
 */
extern VarBlockByteLen VarBlockMakerFinish(
    VarBlockMaker *varBlockMaker);

/*
 * Reset the VarBlock maker so it can make another one
 * using the same inputs as given to VarBlockMakerInit.
 */
void VarBlockMakerReset(
    VarBlockMaker *varBlockMaker);

/*
 * Initialize the VarBlock reader.
 */
extern void VarBlockReaderInit(
    VarBlockReader      *varBlockReader,
    uint8               *buffer,
    VarBlockByteLen     bufferLen);

/*
 * Set the position to a variable-length item.
 *
 * The next call to VarBlockReaderGetNextItemPtr will
 * get the specified item.
 */
extern void VarBlockReaderPosition(
    VarBlockReader      *varBlockReader,
    int                 itemIndex);

/*
 * Get a pointer to the next variable-length item.
 *
 * Returns NULL when there are no more items.
 */
extern uint8* VarBlockReaderGetNextItemPtr(
    VarBlockReader      *varBlockReader,
    VarBlockByteLen     *itemLen);

/*
 * Get the variable-length item count.
 */
extern int VarBlockReaderItemCount(
    VarBlockReader *varBlockReader);

/*
 * Get a pointer to a variable-length item.
 */
extern uint8* VarBlockReaderGetItemPtr(
    VarBlockReader      *varBlockReader,
    int                 itemIndex,
    VarBlockByteLen     *itemLen);


#endif   /* CDBVARBLOCK_H */

