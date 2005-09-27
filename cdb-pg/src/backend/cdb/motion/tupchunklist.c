/*-------------------------------------------------------------------------
* tupchunklist.c
*     The data-structures and functions for dealing with tuple chunk lists.
*
* Copyright (c) 2005, Greenplum
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
										  void *pChunkData, uint16 chunkDataSize)
{
	int chunkSize;
	TupleChunkListItem tcItem;

	/* AssertArg(chunkType >= 0); -- chunkType is unsigned, so this AssertArg is a constant expression */
	AssertArg(chunkType < TC_MAXVAL);

	if (chunkDataSize > 0)
		AssertArg(pChunkData != NULL);

	/* Allocate tuple-chunk list item, including space for the header. */

	chunkSize = TUPLE_CHUNK_HEADER_SIZE + chunkDataSize;

	tcItem = (TupleChunkListItem)palloc0(sizeof(TupleChunkListData) + chunkSize);

	/* Fill in chunk details. */
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


/* Free the contents of a dynamically-allocated tuple chunk list, and then
 * free the list anchor itself.  A pointer to a pointer to the anchor is
 * passed in so that the pointer can be NULLed out after freeing it.
 */
void deleteTCList(TupleChunkList *pTCList)
{
    AssertArg(pTCList != NULL);

    clearTCList(*pTCList);
    pfree(*pTCList);
    *pTCList = NULL;
}

