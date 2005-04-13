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

/*
 * Function specific to 
 * heaps of CdbTupleHeapInfo data.
 */
static int CmpTuplesForSortedMotion( void *lhs, void *rhs );
static void *CreateCdbTupleHeapInfo( void *createData, void *initData );
static void CopyCdbTupleHeapInfo( void *destination, void *source );
static void SwitchCdbTupleHeapInfo( void *left, void *right );


/*
 * Specific CdbTupleHeapInfo heap implementation
 */

CdbHeap *CreateCdbTupleHeap( int nSourceSegments,
							MotionState *node,
							Motion *motion)
{
	CdbHeapTupleCompareInfo *cmpinfo;

	Assert( nSourceSegments >= 1 
			&& node != NULL 
			&& motion != NULL );

	cmpinfo = (CdbHeapTupleCompareInfo *)palloc0( sizeof(CdbHeapTupleCompareInfo) );
	cmpinfo->sortFunctions	= node->sortFunctions;
	cmpinfo->sortFnKinds	= node->sortFnKinds;
	cmpinfo->numSortCols	= motion->numSortCols;
	cmpinfo->sortColIdx		= motion->sortColIdx;
	cmpinfo->tupDesc		= ExecGetResultType(&node->ps);


	return CreateCdbHeap( nSourceSegments,
						CmpTuplesForSortedMotion,
						CreateCdbTupleHeapInfo,
						CopyCdbTupleHeapInfo,
						SwitchCdbTupleHeapInfo,
						cmpinfo);
}

void SetCdbTupleHeapInitialValue( CdbHeap *cdbheap, HeapTuple tuple, bool shouldFree, int sourceSegIndex )
{
	CdbTupleHeapInfo info;

	info.tuple = tuple;
	info.shouldFree = shouldFree;
	info.sourceSegIndex = sourceSegIndex;

	SetCdbHeapInitialValue( cdbheap, &info);
}


CdbTupleHeapInfo *GetLowestValueFromCdbTupleHeap( CdbHeap *cdbheap )
{
	return (CdbTupleHeapInfo *)GetLowestValueFromCdbHeap( cdbheap );
}

void MergeNewValueIntoCdbTupleHeap( CdbHeap *cdbheap, HeapTuple h, bool shouldFree, int sourceSegIndex )
{
	CdbTupleHeapInfo info;

	info.tuple = h;
	info.shouldFree = shouldFree;
	info.sourceSegIndex = sourceSegIndex;

	MergeNewValueIntoCdbHeap( cdbheap, &info );
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

void *CreateCdbTupleHeapInfo( void *createData, void *initData )
{
	CdbTupleHeapInfo *info;
	CdbTupleHeapInfo *initTupleHeapInfo;
	CdbHeapTupleCompareInfo *cmpinfo;

	Assert( createData != NULL );
	Assert( initData != NULL );

	initTupleHeapInfo = (CdbTupleHeapInfo *)initData;
	cmpinfo = (CdbHeapTupleCompareInfo *)createData;

	info = palloc0( sizeof(CdbTupleHeapInfo) );

	info->tuple = initTupleHeapInfo->tuple;
	info->shouldFree = initTupleHeapInfo->shouldFree;
	info->sourceSegIndex = initTupleHeapInfo->sourceSegIndex;
	info->cmpinfo = cmpinfo;

	return info;
}

void CopyCdbTupleHeapInfo( void *destination, void *source )
{
	CdbTupleHeapInfo *infoDest;
	CdbTupleHeapInfo *infoSrc;

	Assert( destination != NULL );
	Assert( source != NULL );

	infoDest = (CdbTupleHeapInfo *)destination;
	infoSrc = (CdbTupleHeapInfo *)source;

	infoDest->tuple = infoSrc->tuple;
	infoDest->shouldFree = infoSrc->shouldFree;
	infoDest->sourceSegIndex = infoSrc->sourceSegIndex;
}

void SwitchCdbTupleHeapInfo( void *left, void *right )
{
	CdbTupleHeapInfo *infoLeft;
	CdbTupleHeapInfo *infoRight;
	HeapTuple tempTuple;
	bool tempShouldFree;
	int tempSegIndex;

	Assert( left != NULL );
	Assert( right != NULL );

	infoLeft = (CdbTupleHeapInfo *)left;
	infoRight = (CdbTupleHeapInfo *)right;

	tempTuple = infoLeft->tuple;
	tempShouldFree = infoLeft->shouldFree;
	tempSegIndex = infoLeft->sourceSegIndex;

	infoLeft->tuple = infoRight->tuple;
	infoLeft->sourceSegIndex = infoRight->sourceSegIndex;
	infoRight->tuple = tempTuple;
	infoRight->shouldFree = tempShouldFree;
	infoRight->sourceSegIndex = tempSegIndex;
}

int GetCountCdbHeap( CdbHeap *cdbheap )
{
	int	count = 0;
	int	i;
	void** heap;
	CdbTupleHeapInfo *info;

	if ( cdbheap == NULL || 
		cdbheap->heap == NULL ||
		cdbheap->size <= 0 )
		return count;

	heap = cdbheap->heap;

	for ( i=1; i<=cdbheap->size; i++ )
	{
		if ( heap[i] == NULL )
			continue;
		
		info = (CdbTupleHeapInfo *)heap[i];
		if ( info != NULL && info->tuple != NULL )
			count++;
	}

	return count;
}

/*
 * Generic Heap Implementation
 */

CdbHeap *CreateCdbHeap( int nSlots,
						CdbHeapCmpFn cmpfn,
						CdbHeapCreateFn createfn,
						CdbHeapCopyFn copyfn,
						CdbHeapSwitchFn switchfn,
						void *createData)
{
	CdbHeap *result;
	void **heap;

	Assert( cmpfn != NULL 
			&& createfn != NULL
			&& copyfn != NULL 
			&& switchfn != NULL
			&& nSlots >= 1 );

	result = palloc0( sizeof(CdbHeap) );
	result->size = nSlots;

	heap = (void **)palloc0( (nSlots + 1) * sizeof(void *) );

	result->heap = (void **)heap;
	result->initialSortDone = false;
	result->cmpfn = cmpfn;
	result->createfn = createfn;
	result->copyfn = copyfn;
	result->switchfn = switchfn;
	result->createdata = createData;

	return result;
}

void DestroyCdbHeap( CdbHeap *cdbheap )
{
	int i;

	if ( cdbheap == NULL )
		return;

	if ( cdbheap->heap != NULL )
	{
		void** heap;
		heap = cdbheap->heap;

		for ( i=0; i<=cdbheap->size; i++ )
		{
			if ( heap[i] != NULL )
			{
				pfree( heap[i] );
			}
		}

		pfree( heap );
	}

	if ( cdbheap->createdata != NULL )
		pfree(cdbheap->createdata);

	pfree( cdbheap );
}

void SetCdbHeapInitialValue( CdbHeap *cdbheap, void *value )
{
	int i;
	void **heap;
	CdbHeapCreateFn createfn;

	Assert( cdbheap != NULL 
			&& cdbheap->heap != NULL 
			&& cdbheap->size >= 1
			&& cdbheap->createfn != NULL);

	heap = cdbheap->heap;
	createfn = cdbheap->createfn;

	for ( i=1; i<=cdbheap->size; i++ )
	{
		if ( heap[i] == NULL )
		{
			heap[i] = (*createfn)(cdbheap->createdata, value);
			return;
		}
	}

	/* Oops, found no open slot  
	 * This is a programming error 
	 * Caller didn't pass correct nSlots in createCdbHeap,
	 * or is calling this function too many times.
	 */

	Assert( false );
}


void *GetLowestValueFromCdbHeap( CdbHeap *cdbheap )
{
	void** heap;
	int i;
	int n;

	Assert( cdbheap != NULL 
		&& cdbheap->heap != NULL
		&& cdbheap->size >= 1);

	heap = cdbheap->heap;

	if ( !cdbheap->initialSortDone )
	{
		/*
		 * Validate that the heap array is full, from 1 to cdbheap->size
		 */
		for ( i=1; i<cdbheap->size; i++ )
		{
			Assert( heap[i] != NULL );
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

void MergeNewValueIntoCdbHeap( CdbHeap *cdbheap, void *value )
{
	void **heap;
	CdbHeapCopyFn copyfn;

	Assert( cdbheap != NULL 
			&& cdbheap->heap != NULL 
			&& cdbheap->size >= 1 
			&& cdbheap->copyfn != NULL);

	heap = cdbheap->heap;
	copyfn = cdbheap->copyfn;

	// Replace top value and sift down
	(*copyfn)( heap[1], value );

	SiftDown( cdbheap, 1, cdbheap->size );
}

void SiftDown( CdbHeap *cdbheap, int From, int End )
{
	int i;
	void **heap;
	CdbHeapCmpFn cmpfn;
	CdbHeapSwitchFn switchfn;

	Assert( cdbheap != NULL 
			&& cdbheap->heap != NULL 
			&& cdbheap->size >= 1 
			&& cdbheap->cmpfn != NULL
			&& cdbheap->switchfn != NULL);

	i = From;
	heap = cdbheap->heap;
	cmpfn = cdbheap->cmpfn;
	switchfn = cdbheap->switchfn;
	
	while ( i <= End/2 )
	{
		int j = 2*i;
		
		if ( j+1 <= End )
		{
			if ( (*cmpfn)(heap[j+1], heap[j]) < 0 )
				j++;	
		}
			
		if ( (*cmpfn)(heap[i], heap[j]) <= 0 )
			break;
	
		// Switch i and j
		(*switchfn)( heap[i], heap[j] );
		i = j;
	}
}
