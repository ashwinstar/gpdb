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


void clearTCList(TupleChunkList tcList)
{
    TupleChunkListItem tcItem, tcNext;
    
    AssertArg(tcList != NULL);
    
    tcItem = tcList->p_first;
    while (tcItem != NULL)
    {
        tcNext = tcItem->p_next;
        pfree(tcItem);
        tcItem = tcNext;
    }
    
    tcList->p_first = NULL;
    tcList->p_last = NULL;
    
    tcList->num_chunks = 0;
    tcList->serialized_data_length = 0;
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
