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

/* flags, involved in cmp */
#define MKE_CF_NullFirst       1                      /* Null is smaller than non null */ 
#define MKE_CF_NotNull         2
#define MKE_CF_NullLast        3                      /* Null is bigger than non null  */
#define MKE_CF_NULLBITS        3                      

#define MKE_CF_Lv              0xFFFC                 /* Runs use 14 bits             */
#define MKE_CF_Run             0x3FFF0000             /* Lv use 14 bits               */
#define MKE_CF_Empty           0x40000000             /* Empty use 1 bit              */
#define MKE_CF_MAX             0x7FFFFFFF             /* max value. */

#define MKE_MAX_LV    0x3FFF
#define MKE_MAX_RUN   0x3FFF
#define MKE_RunShift  16 
#define MKE_LvShift   2 
/* Top bit is always unused */

/* flags, not comp */ 
#define MKE_F_RefCnt          1
#define MKE_F_Copied          2 
#define MKE_F_Reader          0xFFFC               
#define MKE_MAX_READER        0x3FFF
#define MKE_ReaderShift       2 


/* 
 * Our impl. of mk qsort and heap only handles the following data
 * type.  
 */
typedef struct MKEntry 
{
    int32 compflags;
    int32 flags;
	Datum d;
	void *ptr;
} MKEntry;

static inline void mke_blank(MKEntry *e)
{
    e->compflags = (MKE_MAX_LV << MKE_LvShift);
    e->flags = 0;
}

static inline void i_clear_flag(int32 *p, int32 flag)
{
	*p &= ~flag;
}
static inline bool i_test_flag(int32 *p, int32 flag)
{
	return (*p & flag) != 0;
}
static inline void i_set_flag(int32 *p, int32 flag)
{
	*p |= flag;
}
static inline int32 i_get_flag(int32 *p, int32 flag, int32 shiftsz)
{
        return ((*p & flag) >> shiftsz);
}

/* Getter, Setter */
static inline void mke_set_null(MKEntry *e, bool nullfirst)
{
    i_clear_flag(&e->compflags, MKE_CF_NULLBITS);
    if (nullfirst)
        i_set_flag(&e->compflags, MKE_CF_NullFirst);
    else
        i_set_flag(&e->compflags, MKE_CF_NullLast);
}
static inline void mke_set_not_null(MKEntry *e)
{
    i_clear_flag(&e->compflags, MKE_CF_NULLBITS);
    i_set_flag(&e->compflags, MKE_CF_NotNull);
}
static inline bool mke_is_null(MKEntry *e)
{
    return (e->compflags & MKE_CF_NULLBITS) == MKE_CF_NotNull;
}
static inline int32 mke_get_nullbits(MKEntry *e)
{
    return (e->compflags & MKE_CF_NULLBITS);
}

static inline int32 mke_get_run(MKEntry *e)
{
        return i_get_flag(&e->compflags, MKE_CF_Run, MKE_RunShift);
}
static inline void mke_set_run(MKEntry *e, int32 run)
{
        Assert(run >= 0 && run <= MKE_MAX_RUN);
        i_clear_flag(&e->compflags, MKE_CF_Run);
        i_set_flag(&e->compflags, run << MKE_RunShift);
}

static inline int32 mke_get_lv(MKEntry *e)
{
        return (MKE_MAX_LV - i_get_flag(&e->compflags, MKE_CF_Lv, MKE_LvShift));
}
static inline void mke_set_lv(MKEntry *e, int32 lv)
{
        Assert(lv >= 0 && lv <= MKE_MAX_RUN);
        i_clear_flag(&e->compflags, MKE_CF_Lv);
        i_set_flag(&e->compflags, (MKE_MAX_LV - lv) << MKE_LvShift);
}

static inline void mke_set_empty(MKEntry *e)
{
        e->compflags = MKE_CF_Empty; 
        e->flags = 0;
}
static inline bool mke_is_empty(MKEntry *e)
{
        return i_test_flag(&e->compflags, MKE_CF_Empty); 
}

static inline void mke_set_comp_max(MKEntry *e)
{
        e->compflags = MKE_CF_MAX;
}

static inline void mke_set_refc(MKEntry *e)
{
        i_set_flag(&e->flags, MKE_F_RefCnt);
}
static inline void mke_clear_refc(MKEntry *e)
{
        i_clear_flag(&e->flags, MKE_F_RefCnt);
}
static inline bool mke_is_refc(MKEntry *e)
{
        return i_test_flag(&e->flags, MKE_F_RefCnt);
}

static inline void mke_set_copied(MKEntry *e)
{
        i_set_flag(&e->flags, MKE_F_Copied);
}
static inline void mke_clear_copied(MKEntry *e)
{
        i_clear_flag(&e->flags, MKE_F_Copied);
}
static inline bool mke_is_copied(MKEntry *e)
{
        return i_test_flag(&e->flags, MKE_F_Copied);
}

static inline void mke_clear_refc_copied(MKEntry *e)
{
        i_clear_flag(&e->flags, MKE_F_RefCnt | MKE_F_Copied);
}

static inline int32 mke_get_reader(MKEntry *e)
{
        return i_get_flag(&e->flags, MKE_F_Reader, MKE_ReaderShift);
}
static inline void mke_set_reader(MKEntry *e, int32 r)
{
        Assert(r >= 0 && r <= MKE_MAX_READER);
        i_clear_flag(&e->flags, MKE_F_Reader);
        i_set_flag(&e->flags, r << MKE_ReaderShift);
}

typedef int32 (*MKCompare) (MKEntry *v1, MKEntry *v2, void *ctxt);
typedef void (*MKPrepare)(MKEntry *a, void *ctxt);
typedef void (*MKCopyFree) (MKEntry *dst, MKEntry *src, void *ctxt); 

static inline void mk_prepare_array(MKEntry *a, int i, int j, int32 lv, void *ctxt, MKPrepare prep)
{
        MKEntry *last = a+j;

        do {
                (prep)(a, ctxt);
                mke_set_lv(a, lv);
        } while (++a <= last);
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

	int32 limit;		/* Limit sort */
        int32 limitmask;        /* Bit trick */
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

static inline bool mkheap_run_match(MKHeap *mkheap, int32 run)
{
	MKEntry *e = mkheap_peek(mkheap);
        
        if(e)
                return (run == mke_get_run(e)); 
        else
                return false;
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
