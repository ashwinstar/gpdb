/*-------------------------------------------------------------------------
 * tupchunk.c
 *     The data-structures and functions for dealing with tuple chunks.
 *
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <arpa/inet.h>

#include "cdb/tupchunk.h"
#include "cdb/tupser.h"
#include "lib/stringinfo.h"


TupleChunkListItem makeTupleChunkListItem(TupleChunkType chunkType,
    void *pChunkData, uint16 chunkDataSize,
    uint16 srcSegIdx, uint16 tgtSegIdx, uint16 mnid);


/*=========================================================================
 * Functions to get/set values on tuple chunks.  These respect network byte
 * ordering differences with hardware byte ordering (they use htons() etc.).
 */


#define getUint16(pNetData)    ntohs( *( (uint16 *) (pNetData) ) )


/*-------------------------------*/
/* Get/Set target segment index. */
/*-------------------------------*/

uint16 GetTargetSegIdx(uint8 *tc_data)
{
    /* Target segment index = offset 0 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 0);
}

void SetTargetSegIdx(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Target segment index = offset 0 */
    netval = htons(value);
    memcpy(tc_data + 0, &netval, 2);
}

/*-------------------------*/
/* Get/Set query group ID. */
/*-------------------------*/

uint16 GetQueryGroupID(uint8 *tc_data)
{
    /* Query group ID = offset 2 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 2);
}

void SetQueryGroupID(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Query group ID = offset 2 */
    netval = htons(value);
    memcpy(tc_data + 2, &netval, 2);
}

/*-------------------------------*/
/* Get/Set source segment index. */
/*-------------------------------*/

uint16 GetSourceSegIdx(uint8 *tc_data)
{
    /* Source segment index = offset 4 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 4);
}

void SetSourceSegIdx(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Source segment index = offset 4 */
    netval = htons(value);
    memcpy(tc_data + 4, &netval, 2);
}

/*-------------------------*/
/* Get/Set motion node ID. */
/*-------------------------*/

uint16 GetMotionNodeID(uint8 *tc_data)
{
    /* Motion node ID = offset 6 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 6);
}

void SetMotionNodeID(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Motion node ID = offset 6 */
    netval = htons(value);
    memcpy(tc_data + 6, &netval, 2);
}

/*---------------------------*/
/* Get/Set tuple chunk size. */
/*---------------------------*/

uint16 GetChunkDataSize(uint8 *tc_data)
{
    /* Motion node ID = offset 8 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 8);
}

void SetChunkDataSize(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Motion node ID = offset 8 */
    netval = htons(value);
    memcpy(tc_data + 8, &netval, 2);
}

/*---------------------------*/
/* Get/Set tuple chunk type. */
/*---------------------------*/

uint8 GetChunkType(uint8 *tc_data)
{
    return *(tc_data + 10);
}

void SetChunkType(uint8 *tc_data, TupleChunkType value)
{
    *(tc_data + 10) = (uint8) value;
}


/*=========================================================================
 * Functions to convert between tuple-chunks and heap tuples.
 */


TupleChunkList CvtHeapTupToChunks(HeapTuple htup, TupleDesc tupdesc,
    int maxChunkDataSize, uint16 srcSegIdx, uint16 tgtSegIdx, uint16 mnid)
{
    StringInfoData serialTup;
    TupleChunkList tcList;
    TupleChunkListItem tcItem;
    int i;
    uint16 chunkDataSize;
    TupleChunkType tcType;
    
    AssertArg(htup != NULL);
    AssertArg(tupdesc != NULL);
    AssertArg(maxChunkDataSize >= MIN_TC_DATA_SIZE);
    AssertArg(maxChunkDataSize <= MAX_TC_DATA_SIZE);
    
    /* Generate serialized version of heap tuple into the string info. */
    
    initStringInfo(&serialTup);
    SerializeTuple(htup, tupdesc, &serialTup);
    
    /* Create the tuple-chunk list from the serialized data. */
    
    tcList = (TupleChunkList) palloc0(sizeof(TupleChunkListData));
    tcList->serialized_data_length = serialTup.len;
    
    if (serialTup.len <= maxChunkDataSize)   /* Tuple fits in single chunk. */
    {
        /* Create the new tuple chunk. */
        chunkDataSize = serialTup.len;
        tcItem = makeTupleChunkListItem(TC_WHOLE, serialTup.data,
            chunkDataSize, srcSegIdx, tgtSegIdx, mnid);

        /* Update the chunk list. */
        appendChunkToTCList(tcList, tcItem);
        Assert(tcList->num_chunks == 1);
    }
    else
    {
        /* Tuple requires at least two chunks. */
        
        for (i = 0; i < serialTup.len; /*nothing*/ )
        {
            /* Calculate the size of the next chunk of data, and create the
             * new tuple chunk.
             */

            chunkDataSize = serialTup.len - i;
            if (chunkDataSize > maxChunkDataSize)
            {
                chunkDataSize = maxChunkDataSize;
                tcType = (i == 0 ? TC_PARTIAL_START : TC_PARTIAL_MID);
            }
            else
            {
                /* Sanity check:  If we get here, it's because the tuple data
                 * won't fit in a single chunk.  So we shouldn't get in here
                 * on the first iteration of the loop.
                 */
                Assert(i > 0);
                
                /* Since we finally got to the point where the whole rest of
                 * the tuple fits in a chunk, this is the end chunk for the
                 * tuple.
                 */
                tcType = TC_PARTIAL_END;
            }

            tcItem = makeTupleChunkListItem(tcType, serialTup.data + i,
                chunkDataSize, srcSegIdx, tgtSegIdx, mnid);

            /* Update the chunk list. */
            appendChunkToTCList(tcList, tcItem);
            
            /* Finally, increment i! */
            i += chunkDataSize;
        }
    }
    
    return tcList;
}


HeapTuple CvtChunksToHeapTup(TupleChunkList tcList, TupleDesc tupdesc)
{
    StringInfoData serData;
    TupleChunkListItem tcItem;
    int i;
    HeapTuple htup;
    
    AssertArg(tcList != NULL);
    AssertArg(tcList->p_first != NULL);
    AssertArg(tupdesc != NULL);
    
    /* Dump all of the data in the tuple chunk list into a single StringInfo,
     * so that we can convert it into a HeapTuple.  Check chunk types based on
     * whether there is only one chunk, or multiple chunks.
     */

    initStringInfo(&serData);
    tcItem = tcList->p_first;
    i = 0;
    do
    {
        /* Make sure that the type of this tuple chunk is correct! */

        if (i == 0)
        {
            if (tcItem->p_next == NULL)
            {
                if (GetChunkType(tcItem->chunk_data) != TC_WHOLE)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("Single chunk's type must be TC_WHOLE.")));
                }
            }
            else    /* tcItem->p_next != NULL */
            {
                if (GetChunkType(tcItem->chunk_data) != TC_PARTIAL_START)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("First chunk of collection must have type"
                               " TC_PARTIAL_START.")));
                }
            }
        }
        else    /* i > 0 */
        {
            if (tcItem->p_next == NULL)
            {
                if (GetChunkType(tcItem->chunk_data) != TC_PARTIAL_END)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("Last chunk of collection must have type"
                               " TC_PARTIAL_END.")));
                }
            }
            else    /* tcItem->p_next != NULL */
            {
                if (GetChunkType(tcItem->chunk_data) != TC_PARTIAL_MID)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("Last chunk of collection must have type"
                               " TC_PARTIAL_MID.")));
                }
            }
        }

        /* Copy this chunk into the tuple data.  Don't include the header! */
        appendBinaryStringInfo(&serData,
            tcItem->chunk_data + TUPLE_CHUNK_HEADER_SIZE,
            tcItem->chunk_length - TUPLE_CHUNK_HEADER_SIZE);

        /* Go to the next chunk. */
        tcItem = tcItem->p_next;
        i++;
    }
    while (tcItem != NULL);
    
    /* Create the HeapTuple from the concatenated chunks. */
    htup = DeserializeTuple(tupdesc, &serData);

    /* Free up memory we used, just to be leaner. */
    pfree(serData.data);
    
    return htup;
}


TupleChunkListItem makeTupleChunkListItem(TupleChunkType chunkType,
    void *pChunkData, uint16 chunkDataSize,
    uint16 srcSegIdx, uint16 tgtSegIdx, uint16 mnid)
{
    int chunkSize;
    TupleChunkListItem tcItem;
    
    AssertArg(chunkType >= 0);
    AssertArg(chunkType < TC_MAXVAL);
    AssertArg(pChunkData != NULL);
    
    /* Allocate tuple-chunk list item, including space for the header. */
    
    chunkSize = TUPLE_CHUNK_HEADER_SIZE + chunkDataSize;
    
    tcItem = (TupleChunkListItem) palloc0(sizeof(TupleChunkListData) +
        chunkSize);

    /* Fill in chunk details. */

    SetTargetSegIdx( tcItem->chunk_data, srcSegIdx);
/*    SetQueryGroupID( tcItem->chunk_data, mnInfo->process_group_id); */
    SetSourceSegIdx( tcItem->chunk_data, srcSegIdx);
    SetMotionNodeID( tcItem->chunk_data, mnid);
    SetChunkDataSize(tcItem->chunk_data, chunkDataSize);
    SetChunkType(tcItem->chunk_data, chunkType);

    tcItem->chunk_length = chunkSize;

    /* Copy in the data, starting right after the header. */
    memcpy(tcItem->chunk_data + TUPLE_CHUNK_HEADER_SIZE, pChunkData,
        chunkDataSize);

    return tcItem;
}


/* Appends a TupleChunkListItem to the end of a TupleChunkList.  The list's
 * "num_chunks" value is incremented as well.
 */
void appendChunkToTCList(TupleChunkList tcList, TupleChunkListItem tcItem)
{
    AssertArg(tcList != NULL);
    AssertArg(tcItem != NULL);
    
    /* Append the chunk to the list. */
    
    tcList->num_chunks++;
    
    if (tcList->p_last != NULL)     /* List not empty. */
    {
        tcList->p_last->p_next = tcItem;
        tcList->p_last = tcItem;
    }
    else                            /* List is empty. */
    {
        Assert(tcList->p_first == NULL);
        tcList->p_first = tcItem;
        tcList->p_last  = tcItem;
    }
}


/*=========================================================================
 * Functions to send and receive tuple chunks.  (TODO:  THIS NEEDS WORK!)
 */


bool SendTupleChunkToAMS(TupleChunkListItem tcItem)
{
    /* TODO:  Implement! */
    return false;
}


TupleChunkListItem RecvTupleChunkFromAMS(void)
{
    /* TODO:  Implement! */
    return NULL;
}
