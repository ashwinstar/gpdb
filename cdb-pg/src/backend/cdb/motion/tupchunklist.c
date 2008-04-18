/*-------------------------------------------------------------------------
* tupchunklist.c
*	  The data-structures and functions for dealing with tuple chunk lists.
*
* Copyright (c) 2005, Greenplum
*
* IDENTIFICATION:
*	  $Id$
*-------------------------------------------------------------------------
*/
#include "postgres.h"

#include <arpa/inet.h>
#include "nodes/execnodes.h" //SliceTable
#include "cdb/cdbmotion.h"
#include "cdb/tupser.h"
#include "lib/stringinfo.h"


/* Appends a TupleChunkListItem to the end of a TupleChunkList.  The list's
* "num_chunks" value is incremented as well.
*/
void
appendChunkToTCList(TupleChunkList tcList, TupleChunkListItem tcItem)
{
	AssertArg(tcList != NULL);
	AssertArg(tcItem != NULL);

	/* Append the chunk to the list. */
	tcList->num_chunks++;

	if (tcList->p_last != NULL) /* List not empty. */
	{
		tcList->p_last->p_next = tcItem;
		tcList->p_last = tcItem;
	}
	else
		/* List is empty. */
	{
		Assert(tcList->p_first == NULL);
		tcList->p_first = tcItem;
		tcList->p_last = tcItem;
	}
}

static int gp_interconnect_chunk_cache=10;

TupleChunkListItem
getChunkFromCache(TupleChunkListCache *cache)
{
	TupleChunkListItem item;

	if (cache->items != NULL)
	{
		item = cache->items;
		cache->items = item->p_next;
		cache->len = cache->len - 1;
		
		/*
		 * 3.1.1: performance regression:
		 * Clearing the entire data buffer isn't necessary -- and
		 * turns out to waste a lot of time. We just need to zero-out
		 * the top part of the structure, plus the 4-byte header of
		 * the chunk itself.
 
		  MemSetAligned(item, 0, sizeof(TupleChunkListItemData) + Gp_max_tuple_chunk_size);
		*/
		MemSetAligned(item, 0, sizeof(TupleChunkListItemData) + 4);
	}
	else
	{
		item = (TupleChunkListItem)
			palloc0(sizeof(TupleChunkListItemData) + Gp_max_tuple_chunk_size);
	}

	return item;
}

static void
putChunkToCache(TupleChunkListCache *cache, TupleChunkListItem item)
{
	if (cache->len > gp_interconnect_chunk_cache)
	{
		pfree(item);
	}
	else
	{
		item->p_next = cache->items;
		cache->items = item;
		cache->len = cache->len + 1;
	}
	return;
}

void
clearTCList(TupleChunkListCache *cache, TupleChunkList tcList)
{
	TupleChunkListItem tcItem,
				tcNext;

	AssertArg(tcList != NULL);

	tcItem = tcList->p_first;
	while (tcItem != NULL)
	{
		tcNext = tcItem->p_next;

		if (cache != NULL)
			putChunkToCache(cache, tcItem);
		else
			pfree(tcItem);

		tcItem = tcNext;
	}

	tcList->p_first = NULL;
	tcList->p_last = NULL;

	tcList->num_chunks = 0;
	tcList->serialized_data_length = 0;
}

