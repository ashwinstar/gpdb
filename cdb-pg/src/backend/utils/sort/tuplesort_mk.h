/* 
 * tuplesort_mk.h 
 * 	Tuplesort Multi Key.	
 *
 * Copyright (c) Greenplum Inc. 2008.
 */

#ifndef TUPLESORT_MK_H
#define TUPLESORT_MK_H

/* mk_heap: multi level key heap */
/* mk_qsort: multi level key quick sort */
#define MKE_Flag1 	1
#define MKE_NullFlag 	(MKE_Flag1)
#define MKE_Empty       (MKE_Flag1 << 1)
#define MKE_RefCnt 	(MKE_Flag1 << 2)
#define MKE_Copied  	(MKE_Flag1 << 3)

/* 
 * Our impl. of mk qsort and heap only handles the following data
 * type.  
 */
typedef struct MKEntry 
{
	Datum d;
	void *ptr;
	int16 flags;
	int16 lv;
	int16 reader;
	int16 run;
} MKEntry;

static inline void mke_clear_flag(MKEntry *e, int16 flag)
{
	e->flags &= ~flag;
}
static inline bool mke_test_flag(MKEntry *e, int16 flag)
{
	return (e->flags & flag) != 0;
}
static inline bool mke_test_all_flags(MKEntry *e, int16 flag)
{
	return (e->flags & flag) == flag;
}
static inline void mke_set_flag(MKEntry *e, int16 flag)
{
	e->flags |= flag;
}

typedef int (*MKCompare) (MKEntry *v1, MKEntry *v2, void *ctxt);
typedef void (*MKPrepare)(MKEntry *a, void *ctxt);
typedef void (*MKCopyFree) (MKEntry *dst, MKEntry *src, void *ctxt); 

static inline void mk_prepare_array(MKEntry *a, int i, int j, int lv, void *ctxt, MKPrepare prep)
{
	for(;i<=j;++i)
	{
		(prep)(a+i, ctxt);
		a[i].lv = lv;
	}
}

typedef struct MKContext_LvCommon
{
	bool typByVal;
	int  typLen;
} MKContext_LvCommon;

typedef struct MKContext {
	int total_lv;
	MKCompare *comp; 	/* One comparison function for each level */
	MKPrepare *prep;    	/* One prepare function (memtuple_getattr) for each level */

	MKCopyFree  cpfr;   	/* Just one refcnt function.  */
	size_t ctxt_size;
	void *ctxt;

	int64 limit;		/* Limit sort */
	bool unique;		/* Unique sort */
	bool enforceUnique;	/* enforce Unique, for index build */
} MKContext;

static inline void *mk_get_ctxt(MKContext *mk_ctxt, int n)
{
	return (void *) ((char *) mk_ctxt->ctxt + n * mk_ctxt->ctxt_size);
}

/* MK quicksort stuff */
extern void mk_qsort_impl(MKEntry *a, int left, int right, int lv, bool lvdown, MKContext *ctxt, bool seenNull);
static inline void mk_qsort(MKEntry* a, int n, MKContext *ctxt)
{
	mk_qsort_impl(a, 0, n-1, 0, true, ctxt, false);
}

/* MK Heap stuff */
typedef bool (*MKFlagPtrReader) (void *ctxt, MKEntry *e);
typedef struct MKHeapReader
{
	MKFlagPtrReader reader;
	void *mkhr_ctxt;
} MKHeapReader;

typedef struct MKHeap 
{
	MKContext *mkctxt;

	MKEntry *lvtops;

	MKEntry *p;
	int count;
	int maxentry;
	int alloc_size;

	MKHeapReader *readers;
	int nreader;
} MKHeap;


/* 
 * Create a heap from an array.  Once the heap is created, it owns the array. 
 */
extern MKHeap *mkheap_from_array(MKEntry *entries, int alloc_size, int count, MKContext *mkctxt); 
/* 
 * Create a heap from an array of readers.  MKHeap never owns the reader.  Readers
 * are assumed to have a longer life cycle than the mkheap. 
 */
extern MKHeap *mkheap_from_reader(MKHeapReader *readers, int nreader, MKContext *mkctxt);

extern void mkheap_destroy(MKHeap *mkheap);

/* 
 * Insert into heap.  
 * NOTE: Unusual interfaces.
 * 		insert an flagptr.  
 *		return value < 0 means fail to insert because the value is smaller than the smallest entry in heap.
 *		return value = 0 means fail to insert because the value equals to smallest entry in heap.
 *		return value > 0 means successfully inserted into the heap.
 *
 * This means, we cannot insert anything smaller than current smallest entry, however, we will see this
 * is enough for our use in external sort or limit sort.  More general way of doing this is definately 
 * doable, but not needed.
 */
extern int mkheap_puget(MKHeap *mkheap, MKEntry *e);
extern int mkheap_puget_run(MKHeap *mkheap, MKEntry *e, int16 run); 
extern int mkheap_puget_reader(MKHeap *mkheap, MKEntry *e);

/* 
 * Remove and return smallest elements from the heap.
 */
extern MKEntry *mkheap_peek(MKHeap *mkheap);

static inline bool mkheap_run_match(MKHeap *mkheap, int16 run)
{
	MKEntry *e = mkheap_peek(mkheap);
	if(!e) 
		return false;

	return run == e->run;
}

static inline bool mkheap_empty(MKHeap *mkheap)
{
	return mkheap->count == 0;
}
static inline int64 mkheap_cnt(MKHeap *mkheap)
{
	return mkheap->count;
}

#endif
