/*-------------------------------------------------------------------------
 *
 * cdbheap.c
 * Functions for managing a heap of tuple results
 * for use by sorted motion nodes *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/heapam.h"
#include "executor/executor.h"
#include "cdb/cdbheap.h"

static void SiftDown( CdbHeap *cdbheap, int From, int End );


CdbHeap *CreateCdbTupleHeap( int nSourceSegments,
							CdbHeapCmpFn cmpfn,
							MotionState *node,
							Motion *motion)
{
	CdbHeap *result;
	CdbTupleHeapInfo** heap;
	int i;
	CdbHeapTupleCompareInfo *cmpinfo;

	Assert( cmpfn != NULL 
			&& node != NULL 
			&& motion != NULL );

	result = palloc0( sizeof(CdbHeap) );
	result->size = nSourceSegments;

	heap = (CdbTupleHeapInfo**)palloc0( (result->size + 1) * sizeof(CdbTupleHeapInfo *) );

	cmpinfo = (CdbHeapTupleCompareInfo *)palloc0( sizeof(CdbHeapTupleCompareInfo) );
	cmpinfo->sortFunctions	= node->sortFunctions;
	cmpinfo->sortFnKinds	= node->sortFnKinds;
	cmpinfo->numSortCols	= motion->numSortCols;
	cmpinfo->sortColIdx		= motion->sortColIdx;
	cmpinfo->tupDesc		= ExecGetResultType(&node->ps);

	for ( i=0; i<=result->size; i++ )
	{
		heap[i] = (CdbTupleHeapInfo *)palloc0( sizeof(CdbTupleHeapInfo) );
		heap[i]->tuple = NULL;
		heap[i]->sourceSegIndex = INT_MIN;
		heap[i]->cmpinfo = cmpinfo;
	}

	result->heap = (void **)heap;
	result->initialSortDone = false;
	result->cmpfn = cmpfn;

	return result;
}

void DestroyCdbTupleHeap( CdbHeap *cdbheap )
{
	int i;

	if ( cdbheap == NULL )
		return;

	if ( cdbheap->heap != NULL )
	{
		CdbTupleHeapInfo** heap;
		heap = (CdbTupleHeapInfo**)cdbheap->heap;

		for ( i=0; i<=cdbheap->size; i++ )
		{
			if ( heap[i] != NULL )
			{
				if ( i == 0 )
					pfree( heap[i]->cmpinfo );

				pfree( heap[i] );
			}
		}

		pfree( heap );
	}

	pfree( cdbheap );
}

void SetCdbTupleHeapInitialValue( CdbHeap *cdbheap, HeapTuple tuple, int sourceSegIndex )
{
	int i;
	CdbTupleHeapInfo** heap;

	Assert( cdbheap != NULL 
			&& sourceSegIndex >= 0 );
	Assert( cdbheap->initialSortDone == false );
	Assert( cdbheap->heap != NULL );

	heap = (CdbTupleHeapInfo**)cdbheap->heap;

	// Find next open slot
	for ( i=1; i<=cdbheap->size; i++ )
	{
		if ( heap[i] != NULL && heap[i]->sourceSegIndex == INT_MIN )
		{
			heap[i]->tuple = tuple;
			heap[i]->sourceSegIndex = sourceSegIndex;
			return;
		}
	}

	/* Oops, found no open slot  
	 * This is a programming error 
	 * Caller didn't pass correct nSourceSegments in createCdbTupleHeap,
	 * or is calling this too many times.
	 */

	Assert( false );
}


CdbTupleHeapInfo *GetLowestValueFromCdbTupleHeap( CdbHeap *cdbheap )
{
	CdbTupleHeapInfo** heap;
	int i;
	int n;

	Assert( cdbheap != NULL 
		&& cdbheap->heap != NULL
		&& cdbheap->size >= 1);

	heap = (CdbTupleHeapInfo**)cdbheap->heap;

	if ( !cdbheap->initialSortDone )
	{
		/*
		 * Validate that the heap array is full, from 1 to cdbheap->size
		 */
		for ( i=1; i<cdbheap->size; i++ )
		{
			Assert( heap[i]->sourceSegIndex >= 0 );
			Assert( heap[i]->cmpinfo != NULL );
		}

		/*
		*  Now, do the Sift Down
		*/
		n = cdbheap->size;
		for ( i=n/2; i>=1; i-- )
		{
			SiftDown( cdbheap, i, n );
		}
	}

	// Return a pointer to top of heap
	return cdbheap->heap[1];
}

void MergeNewValueIntoCdbTupleHeap( CdbHeap *cdbheap, HeapTuple h, int sourceSegIndex )
{
	CdbTupleHeapInfo** heap;

	Assert( cdbheap != NULL 
			&& cdbheap->heap != NULL 
			&& cdbheap->size >= 1 );

	heap = (CdbTupleHeapInfo**)cdbheap->heap;

	// Replace top value and sift down
	heap[1]->tuple = h;
	heap[1]->sourceSegIndex = sourceSegIndex;

	SiftDown( cdbheap, 1, cdbheap->size );
}

void SiftDown( CdbHeap *cdbheap, int From, int End )
{
	int i;
	void **heap;
	CdbHeapCmpFn cmpfn;

	Assert( cdbheap != NULL 
			&& cdbheap->heap != NULL 
			&& cdbheap->size >= 1 
			&& cdbheap->cmpfn != NULL );

	i = From;
	heap = cdbheap->heap;
	cmpfn = cdbheap->cmpfn;
	
	while ( i <= End/2 )
	{
		int j = 2*i;
		void *temp;
		
		if ( j+1 <= End )
		{
			if ( (*cmpfn)(heap[j+1], heap[j]) < 0 )
				j++;	
		}
			
		if ( (*cmpfn)(heap[i], heap[j]) <= 0 )
			break;
	
		// Switch i and j
		temp = heap[i];
		heap[i] = heap[j];
		heap[j] = temp;
		i = j;
	}
}

/*
 * cmpTuplesForSortedMotion:
 * Used to compare tuples for a sorted motion node.
*/

int CmpTuplesForSortedMotion( void *lhs, void *rhs ) 
{
	CdbTupleHeapInfo *linfo = (CdbTupleHeapInfo *)lhs;
	CdbTupleHeapInfo *rinfo = (CdbTupleHeapInfo *)rhs;
	HeapTuple ltup;
	HeapTuple rtup;
	FmgrInfo *sortFunctions;
	SortFunctionKind *sortFnKinds;
	int numSortCols;
	AttrNumber *sortColIdx;
	TupleDesc tupDesc;
	int nkey;

	Assert( linfo->cmpinfo != NULL &&
		linfo->cmpinfo == rinfo->cmpinfo );

	CdbHeapTupleCompareInfo *cmpinfo = linfo->cmpinfo;

	ltup = linfo->tuple;
	rtup = rinfo->tuple; 
	sortFunctions = cmpinfo->sortFunctions;
	sortFnKinds = cmpinfo->sortFnKinds;
    numSortCols = cmpinfo->numSortCols;
	sortColIdx = cmpinfo->sortColIdx;
	tupDesc = cmpinfo->tupDesc;

	// NULL is greater than any non-NULL tuple.
	if ( ltup == NULL )
	{
		if ( rtup == NULL )
			return 0;

		return 1;
	}
	else if ( rtup == NULL )
	{
		return -1;
	}

	for ( nkey=0; nkey<numSortCols; nkey++ )
	{
		AttrNumber	attno = sortColIdx[nkey];
		Datum		datum1,
					datum2;
		bool		isnull1,
					isnull2;
		int32		compare;

		datum1 = heap_getattr(ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(rtup, attno, tupDesc, &isnull2);

		compare = ApplySortFunction(&sortFunctions[nkey],
									sortFnKinds[nkey],
									datum1, isnull1,
									datum2, isnull2);
		if (compare != 0)
			return compare;
	}

	return 0;

}
