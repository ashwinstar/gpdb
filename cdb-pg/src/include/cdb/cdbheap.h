/*-------------------------------------------------------------------------
 *
 * cdbheap.h
 * Functions for managing a heap of tuple results
 * for use by sorted motion nodes
 * Copyright (c) 2003-2005, Metapa
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

typedef struct CdbHeapTupleCompareInfo
{
	FmgrInfo			*sortFunctions;
	SortFunctionKind	*sortFnKinds;
	int					numSortCols;
	AttrNumber			*sortColIdx;
	TupleDesc			tupDesc;
} CdbHeapTupleCompareInfo;

typedef struct CdbTupleHeapInfo
{
	HeapTuple					tuple;
	int							sourceSegIndex;
	CdbHeapTupleCompareInfo*	cmpinfo;
} CdbTupleHeapInfo;



typedef int (*CdbHeapCmpFn)(void *, void *);

typedef struct CdbHeap
{
	int					size;
	void				**heap;
	bool				initialSortDone;
	CdbHeapCmpFn		cmpfn;
} CdbHeap;

CdbHeap *CreateCdbTupleHeap( int nSourceSegments,
							CdbHeapCmpFn cmpfn,
							MotionState *node,
							Motion *motion);
void DestroyCdbTupleHeap( CdbHeap *cdbheap );
void SetCdbTupleHeapInitialValue( CdbHeap *cdbheap, HeapTuple tuple, int sourceSegIndex );
CdbTupleHeapInfo *GetLowestValueFromCdbTupleHeap( CdbHeap *cdbheap );
void MergeNewValueIntoCdbTupleHeap( CdbHeap *cdbheap, HeapTuple h, int sourceSegIndex );
int CmpTuplesForSortedMotion( void *lhs, void *rhs );


#endif	/* CDBHEAP_H */
