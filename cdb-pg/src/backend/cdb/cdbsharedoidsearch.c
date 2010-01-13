/*-------------------------------------------------------------------------
 *
 * cdbsharedoidsearch.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "cdb/cdbshareddoublylinked.h"
#include "cdb/cdbsharedoidsearch.h"

// -----------------------------------------------------------------------------
// Free Pool 
// -----------------------------------------------------------------------------

static void SharedOidSearch_MakeFreeObjPool(
	SharedOidSearchFreeObjPool 	*sharedFreeObjPool,
				/* The shared free object pool to initialize. */

	void *sharedMem,
				/* The shared-memory to use. */

	int32 sharedMemLen,
				/* The byte length of the shared-memory. */

	int32 objectLen)
				/* 
				 * The total length of the objects that includes the embedded header
				 * SharedOidSearchObjHeader.
				 */
{
	int32 freeCount;
	int32 i;

	SharedListBase_Init(
		&sharedFreeObjPool->listBase,
		sharedMem,
		objectLen,
		offsetof(SharedOidSearchObjHeader,private.links));

	freeCount = sharedMemLen / objectLen;

	SharedDoublyLinkedHead_Init(&sharedFreeObjPool->freeList);

	/*
	 * Initalize the free objects.
	 */
	for (i = 0; i < freeCount; i++)
	{
		SharedOidSearchObjHeader *ele;

		ele = (SharedOidSearchObjHeader*)
					SharedListBase_ToElement(
							&sharedFreeObjPool->listBase, i);
		
		SharedDoubleLinks_Init(&ele->private.links, i);

		SharedDoublyLinkedHead_AddLast(
			&sharedFreeObjPool->listBase,
			&sharedFreeObjPool->freeList,
			ele);
	}
}

// -----------------------------------------------------------------------------
// Initialize
// -----------------------------------------------------------------------------

void SharedOidSearch_InitDescr(
	SharedOidSearchDescr *descr,
				/* The template of shared search tables to initialize. */

	int32 tableLen,
				/* The byte length of the search table. */

	int32 hashSize,
				/* The hash array size */

	uint8 *sharedMem,
				/* The shared-memory to use. */

	int32 sharedMemLen,
				/* The byte length of the shared-memory. */

	int32 objectLen)
				/* 
				 * The total length of the objects that includes the embedded header
				 * SharedOidSearchObjHeader.
				 */

{
	descr->private.tableLen = tableLen;
	descr->private.hashSize = hashSize;

	SharedOidSearch_MakeFreeObjPool(
						&descr->private.freePool,
						sharedMem,
						sharedMemLen,
						objectLen);	
}

void SharedOidSearch_InitTable(
	SharedOidSearchTable *table,
				/* The shared search tables to initialize. */

	SharedOidSearchDescr *descr)
				/* The template of shared search tables. */
{
	SharedOidSearchHashBucket	*bucketArray;
	int32 hashSize;
	int32 i;

	table->private.descr = descr;

	bucketArray = &table->private.buckets[0];
	hashSize = descr->private.hashSize;
	for (i = 0; i < hashSize; i++)
		SharedDoublyLinkedHead_Init(&bucketArray[i].bucketListHead);
}


// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static SharedOidSearchHashBucket *SharedOidSearch_GetBucket(
	SharedOidSearchTable		*table,
	SharedOidSearchDescr 		*descr,
	Oid 						oid1)
{
	int32 bucketNum;
	
	bucketNum = oid1 % descr->private.hashSize;
	return &table->private.buckets[bucketNum];
}

static SharedOidSearchObjHeader *SharedOidSearch_FindInBucket(
	SharedOidSearchTable		*table,
	SharedOidSearchHashBucket	*bucket,
	Oid 						oid1,
	Oid							oid2)
{
	SharedOidSearchDescr		*descr = table->private.descr;
	SharedListBase				*listBase = &descr->private.freePool.listBase;
	SharedDoublyLinkedHead		*listHead;
	SharedOidSearchObjHeader 	*ele;

	listHead = &bucket->bucketListHead;
	ele = (SharedOidSearchObjHeader*)
					SharedDoublyLinkedHead_First(listBase, listHead);
	while (ele != NULL)
	{
		if (ele->oid1 == oid1 && ele->oid2 == oid2)
			return ele;

		ele = (SharedOidSearchObjHeader*)
					SharedDoubleLinks_Next(listBase, listHead, ele);
	}

	return NULL;
}

static SharedOidSearchObjHeader *SharedOidSearch_ProbeInBucket(
	SharedOidSearchTable		*table,
	SharedOidSearchHashBucket	*bucket,
	Oid 						oid1)
{
	SharedOidSearchDescr		*descr = table->private.descr;
	SharedListBase				*listBase = &descr->private.freePool.listBase;
	SharedDoublyLinkedHead		*listHead;
	SharedOidSearchObjHeader 	*ele;

	listHead = &bucket->bucketListHead;
	ele = (SharedOidSearchObjHeader*)
					SharedDoublyLinkedHead_First(listBase, listHead);
	while (ele != NULL)
	{
		if (ele->oid1 == oid1)
			return ele;

		ele = (SharedOidSearchObjHeader*)
					SharedDoubleLinks_Next(listBase, listHead, ele);
	}

	return NULL;
}

static void SharedOidSearch_RemoveFromBucket(
	SharedOidSearchTable		*table,
	SharedOidSearchHashBucket	*bucket,
	SharedOidSearchObjHeader 	*ele)
{
	SharedOidSearchDescr		*descr = table->private.descr;
	SharedListBase				*listBase = &descr->private.freePool.listBase;
	SharedDoublyLinkedHead		*listHead;

	Assert(ele->private.isDeleted);
	Assert(ele->private.pinCount == 0);

	listHead = &bucket->bucketListHead;

	SharedDoubleLinks_Remove(listBase, listHead, ele);
	
	SharedDoublyLinkedHead_AddLast(
					&descr->private.freePool.listBase,
					&descr->private.freePool.freeList,
					ele);
}


// -----------------------------------------------------------------------------
// Add, Find, Probe, and Delete
// -----------------------------------------------------------------------------

SharedOidSearchAddResult SharedOidSearch_Add(
	SharedOidSearchTable 		*table,
	Oid 						oid1,
	Oid 						oid2,
	SharedOidSearchObjHeader	**header)
{
	SharedOidSearchDescr 		*descr = table->private.descr;
	SharedOidSearchHashBucket	*bucket;
	SharedOidSearchObjHeader 	*ele;

	bucket = SharedOidSearch_GetBucket(table, descr, oid1);

	ele = SharedOidSearch_FindInBucket(table, bucket, oid1, oid2);
	if (ele != NULL)
		return SharedOidSearchAddResult_Exists;

	ele = 
		(SharedOidSearchObjHeader*)
				SharedDoublyLinkedHead_RemoveFirst(
								&descr->private.freePool.listBase,
								&descr->private.freePool.freeList);
	if (ele == NULL)
		return SharedOidSearchAddResult_NoMemory;

	ele->private.pinCount = 0;
	ele->private.isDeleted = false;
	ele->oid1 = oid1;
	ele->oid2 = oid2;

	SharedDoublyLinkedHead_AddLast(
							&descr->private.freePool.listBase,
						    &bucket->bucketListHead,
						    ele);

	*header = ele;
	return SharedOidSearchAddResult_Ok;
}

SharedOidSearchObjHeader *SharedOidSearch_Find(
	SharedOidSearchTable 	*table,
	Oid 					oid1,
	Oid 					oid2)
{
	SharedOidSearchDescr 		*descr = table->private.descr;
	SharedOidSearchHashBucket	*bucket;
	SharedOidSearchObjHeader 	*ele;

	bucket = SharedOidSearch_GetBucket(table, descr, oid1);

	ele = SharedOidSearch_FindInBucket(table, bucket, oid1, oid2);
	if (ele == NULL)
		return NULL;

	return ele;
}

SharedOidSearchObjHeader *SharedOidSearch_Probe(
	SharedOidSearchTable 	*table,
	Oid 					oid1)
{
	SharedOidSearchDescr 		*descr = table->private.descr;
	SharedOidSearchHashBucket	*bucket;
	SharedOidSearchObjHeader 	*ele;

	bucket = SharedOidSearch_GetBucket(table, descr, oid1);

	ele = SharedOidSearch_ProbeInBucket(table, bucket, oid1);
	if (ele == NULL)
		return NULL;

	return ele;
}

static bool SharedOidSearch_NextBucket(
	SharedOidSearchTable		*table,
	SharedOidSearchDescr 		*descr,
	SharedOidSearchHashBucket	**bucket)
{
	int32 hashSize = descr->private.hashSize;

	if (*bucket == &table->private.buckets[hashSize-1])
		return false;

	(*bucket)++;
	return true;
}

static bool SharedOidSearch_FindNonDeletedInBucket(
	SharedListBase				*listBase,
	SharedDoublyLinkedHead		*listHead,
	SharedOidSearchObjHeader	**header)
{
	while (true)
	{
		if (!(*header)->private.isDeleted)
			return true;
		
		*header = 
			(SharedOidSearchObjHeader*)
					SharedDoubleLinks_Next(
										listBase,
										listHead,
										*header);
		if (*header == NULL)
			return false;
	}
}


void SharedOidSearch_Iterate(
	SharedOidSearchTable 		*table,
	SharedOidSearchObjHeader	**header)
{
	SharedOidSearchDescr 		*descr = table->private.descr;
	SharedListBase				*listBase = &descr->private.freePool.listBase;
	SharedOidSearchHashBucket	*bucket;
	SharedOidSearchObjHeader	*current = *header;

	if (current != NULL)
	{
		/*
		 * Try to get next element in current bucket.
		 */
		bucket = SharedOidSearch_GetBucket(table, descr, current->oid1);

		Assert(current->private.pinCount > 0);
		current->private.pinCount--;
			
		*header = 
			(SharedOidSearchObjHeader*)
					SharedDoubleLinks_Next(
										listBase,
										&bucket->bucketListHead,
										current);

		if (current->private.isDeleted &&
			current->private.pinCount == 0)
			SharedOidSearch_RemoveFromBucket(table, bucket, current);
			
		if (*header != NULL &&
			SharedOidSearch_FindNonDeletedInBucket(
											listBase,
											&bucket->bucketListHead,
											header))
		{
			(*header)->private.pinCount++;
			return;
		}

		Assert(*header == NULL);

		if (!SharedOidSearch_NextBucket(
									table,
									descr,
									&bucket))
			return;		// No more buckets.
	}
	else
		bucket = &table->private.buckets[0];

	/*
	 * Find next non-empty bucket.
	 */
	 while (true)
	{
		*header = 
				(SharedOidSearchObjHeader*)
						SharedDoublyLinkedHead_First(
												listBase,
												&bucket->bucketListHead);
		if (*header != NULL &&
			SharedOidSearch_FindNonDeletedInBucket(
											listBase,
											&bucket->bucketListHead,
											header))
		{
			(*header)->private.pinCount++;
			return;
		}

		Assert(*header == NULL);

		if (!SharedOidSearch_NextBucket(
									table,
									descr,
									&bucket))
			return;		// No more buckets.
	}
}

void SharedOidSearch_ReleaseIterator(
	SharedOidSearchTable 		*table,
	SharedOidSearchObjHeader	**header)
{
	SharedOidSearchDescr 		*descr = table->private.descr;
	SharedOidSearchObjHeader	*current = *header;
	SharedOidSearchHashBucket	*bucket;
	
	Assert(current->private.pinCount > 0);
	current->private.pinCount--;

	if (current->private.isDeleted &&
		current->private.pinCount == 0)
	{
		bucket = SharedOidSearch_GetBucket(table, descr, current->oid1);
		SharedOidSearch_RemoveFromBucket(table, bucket, current);
	}
	
	*header = NULL;
}

void SharedOidSearch_Delete(
	SharedOidSearchTable 		*table,
	SharedOidSearchObjHeader	*header)
{
	SharedOidSearchDescr 		*descr = table->private.descr;
	SharedOidSearchHashBucket	*bucket;

	Assert(!header->private.isDeleted);
	header->private.isDeleted = true;

	if (header->private.pinCount > 0)
		return;		// Let the last iterator turn out the light.

	bucket = SharedOidSearch_GetBucket(table, descr, header->oid1);

	SharedOidSearch_RemoveFromBucket(table, bucket, header);
}
