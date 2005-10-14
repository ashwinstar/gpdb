/*-------------------------------------------------------------------------
 *
 * cdbheap.h
 * Functions for managing a heap of tuple results
 * for use by sorted motion nodes
 * Copyright (c) 2003-2005, Greenplum
 *
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBHEAP_H
#define CDBHEAP_H

#include "access/htup.h"
#include "fmgr.h"
#include "utils/tuplesort.h"
#include "nodes/execnodes.h"

/*
 * CdbHeapTupleCompareInfo:
 * This contains the information necessary to compare
 * two tuples (other than the tuples themselves).
 * It includes :
 *		1) the number and array of indexes into the tuples columns
 *			that are the basis for the ordering
 *			(numSortCols, sortColIdx)
 *		2) the kind and FmgrInfo of the compare function
 *			for each column being ordered
 *			(sortFnKinds, sortFunctions)
 *		3) the tuple desc
 *			(tupDesc)
 *
 * Since the compare function is set to takes two void * arguments,
 * we include a pointer to a CdbHeapTupleCompareInfo in CdbTupleHeapInfo.
 * That way, the pointers that gets passed to the compare function
 * always have the CdbHeapTupleCompareInfo available.
 */
typedef struct CdbHeapTupleCompareInfo
{
	FmgrInfo   *sortFunctions;
	SortFunctionKind *sortFnKinds;
	int			numSortCols;
	AttrNumber *sortColIdx;
	TupleDesc	tupDesc;
}	CdbHeapTupleCompareInfo;

/*
 * CdbTupleHeapInfo:
 * This is the data that the heap contains and orders.
 * It includes:
 *		1) The actual HeapTuple data from the executor (tuple)
 *
 *		3) The routeId that this data came from. (the place in the
 *			slicetable process list )
 *
 *		4) A pointer to the CdbHeapTupleCompareInfo that is global to
 *			this heap (i.e. doesn't vary by heap tuple ) (cmpInfo)
 */
typedef struct CdbTupleHeapInfo
{
	HeapTuple	tuple;
	int			sourceRouteId;
	CdbHeapTupleCompareInfo *cmpinfo;
}	CdbTupleHeapInfo;


/*
 * typedef for a compare function
 * used by the cdb heap inmplementation
 */
typedef int (*CdbHeapCmpFn) (void *, void *);

/*
 * typedef for an initialize function
 * used by the cdb heap inmplementation
 */
typedef void *(*CdbHeapCreateFn) (void *, void *);

/*
 * typedef for a copy function
 * used by the cdb heap inmplementation
 */
typedef void (*CdbHeapCopyFn) (void *, void *);

/*
 * typedef for a switch function
 * used by the cdb heap inmplementation
 */
typedef void (*CdbHeapSwitchFn) (void *, void *);

/*
 * CdbHeap:
 * A data structure used for sorting, especially well suited for
 * match-merge type operations.
 * Based on Knuth Vol 3.
 * The actual data is pointed to by the elements of the array heap.
 * and compared for order based on the cmpfn.
 *
 * The properties of the heap are:
 *	1) There are size+1 elements in the array heap.
 *		The real data is put at indexex 1 thru size.
 *		Index 0 never has any data.
 *		This as done for ease of algorithms.
 *	2) Each element of heap is a pointer to some struct
 *	3) To compare the structs pointed to by elements of heap,
 *		the function cmpfn( heap[i], heap[j] ) is called, and
 *		returns -1, 0, or 1 as usual for cmp functions.
 *	4) Once the heap has been sorted, the following
 *		holds:	heap[i] >= heap[i/2] for each i: 2 <= i <= size
 *	5) This implies that the lowest value is always at index 1 of the heap.
 *	6) To add an element to the heap, it is placed at index 1, then the heap is resorted.
 *
 */
typedef struct CdbHeap
{
	int			size;
	void	  **heap;
	bool		initialSortDone;
	CdbHeapCmpFn cmpfn;
	CdbHeapCreateFn createfn;
	CdbHeapCopyFn copyfn;
	CdbHeapSwitchFn switchfn;
	void	   *createdata;
}	CdbHeap;

/*
 * Generic heap functions, that can be used with any
 * data structure and cmpfunc
 */
extern void SetCdbHeapInitialValue(CdbHeap * cdbheap, void *value);
extern void *GetLowestValueFromCdbHeap(CdbHeap * cdbheap);
extern void MergeNewValueIntoCdbHeap(CdbHeap * cdbheap, void *value);
extern CdbHeap *CreateCdbHeap(int nSlots,
			  CdbHeapCmpFn cmpfn,
			  CdbHeapCreateFn createfn,
			  CdbHeapCopyFn copyfn,
			  CdbHeapSwitchFn switchfn,
			  void *createData);
extern void DestroyCdbHeap(CdbHeap * cdbheap);

/*
 * Specific heap functions, specific to
 * heaps of CdbTupleHeapInfo data.
 */
extern void SetCdbTupleHeapInitialValue(CdbHeap * cdbheap, HeapTuple tuple, int sourceRouteId);
extern CdbTupleHeapInfo *GetLowestValueFromCdbTupleHeap(CdbHeap * cdbheap);
extern void MergeNewValueIntoCdbTupleHeap(CdbHeap * cdbheap, HeapTuple h, int sourceRouteId);
extern int	GetCountCdbHeap(CdbHeap * cdbheap);

/*
 * Create heap functions, specific to
 * heaps of CdbTupleHeapInfo data.
 */
extern CdbHeap *CreateCdbTupleHeap(int numRoutes,
				   MotionState * node,
				   Motion * motion);




#endif   /* CDBHEAP_H */
