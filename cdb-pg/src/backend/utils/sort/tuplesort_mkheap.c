/* 
 * tuplesort_mkheap.c
 * 		Multi level key heap.
 * 
 * Copyright (c) Greenplum Inc, 2008.
 */

#include "postgres.h"
#include "tuplesort_mk.h"
#include "utils/datum.h"

#include "cdb/cdbvars.h"

/* 
 * MKHeap. Multi Key Heap.
 * 	This is a heap, plus an array.  The array holds the expanded level of keys (Datum).
 *	For each entry in the heap, it is expanded to a certain level lv.  From 0 to lv-1, 
 * 	it means the key is the same as the one in the array.  See mkheap_compare.
 *
 * The purpose of this weird heap is that we do not do any unnecessary lv expansions.  Also,
 * the compare is cheap. 
 */

static void mkheap_heapify(MKHeap *mkheap, bool doLv0); 

static inline int LEFT(int n)
{
    Assert(n>=0);
    return (n << 1) + 1;
}
static inline int RIGHT(int n)
{
    Assert(n>=0);
    return (n << 1) + 2;
}
static inline int PARENT(int n)
{
    Assert(n > 0);
    return (n-1) >> 1;
}
static inline int SIBLING(int n)
{
    Assert(n > 0);
    return ((n-1) ^ 1) + 1;
}
static inline bool ISLEFT(int n)
{
    Assert(n > 0);
    return (n & 1) != 0;
}
static inline bool ISRIGHT(int n)
{
    Assert(n > 0);
    return (n & 1) == 0;
}

/* 
 * Heap compare.
 * 	Rule:
 *	1. Empty entry is always bigger, so empty cells will be at the bottom
 *	2. A entry is expanded at lv, means 0 - (lv-1) is the same as heaptop.
 *	3. From 2, deeper level is smaller.
 *	4. Smaller run is always smaller.  Why we do this after 2. and 3.?  
 *		When a entry is inserted, we don't know this entry belongs to
 *		current run on the next run yet.
 *	5. Same run, same level, then we compare the null flag.  Pg 8.2 (that is,
 *         what we are using, always use null last.)   We do have a null first bit
 *         reseved.
 *      6. Compare non null datum.
 *		
 * These rules guarantee the heap top is minimal of current run.  However, at
 * deeper level, mkheap_compare return 0 does not mean the two entry are really
 * equal.  They are just equal to cerntain level.
 *
 * Step 1 - 5 are captured in compflags.  
 */
static inline int32 mkheap_compare(MKHeap *heap, MKEntry *a, MKEntry *b) 
{
    int32 ret = (a->compflags & (~MKE_CF_NULLBITS)) - 
                (b->compflags & (~MKE_CF_NULLBITS));

    if (ret == 0)
    {
        int32 limitmask = heap->mkctxt->limitmask;
        bool needcmp = false;
        ret = a->compflags - b->compflags;
        needcmp = (ret == 0) && (!mke_is_null(a));

        if (needcmp)
        {
            int32 lv = mke_get_lv(a);
            ret = (heap->mkctxt->comp[lv])(a, b, mk_get_ctxt(heap->mkctxt, lv));
        }
        ret = (ret & (~limitmask)) + ((-ret) & limitmask);
    }

    return ret;
}

#ifdef USE_ASSERT_CHECKING
extern void mkheap_verify_heap(MKHeap *heap, int top);
#else
#define mkheap_verify_heap(heap, top) 
#endif

/* 
 * Create a mkheap from an array of entries.  After creatation, the 
 * heap owns the array
 */
MKHeap *mkheap_from_array(MKEntry *entries, int alloc_sz, int cnt, MKContext *mkctxt)
{
    MKHeap *heap = (MKHeap *) palloc(sizeof(MKHeap));

    Assert(mkctxt);
    Assert(alloc_sz >= cnt);
    AssertEquivalent(entries!=NULL, cnt > 0);
    AssertEquivalent(!entries, cnt == 0);

    heap->mkctxt = mkctxt;
    heap->lvtops = palloc0(mkctxt->total_lv * sizeof(MKEntry));

    heap->readers = NULL;
    heap->nreader = 0;

    AssertImply(alloc_sz == 0, !entries);
    Assert(cnt >= 0 && cnt <= alloc_sz);

    heap->p = entries;
    heap->alloc_size = alloc_sz;
    heap->count = cnt;
    heap->maxentry = cnt;

#ifdef USE_ASSERT_CHECKING
    {
        int i;
        for(i=0; i<cnt; ++i)
        {
            Assert(mke_get_lv(entries+i) == 0);
            Assert(mke_get_reader(entries+i) == 0);
        }
    }
#endif

    if(mkctxt->prep[0])
        mk_prepare_array(entries, 0, cnt-1, 0, mk_get_ctxt(mkctxt, 0), mkctxt->prep[0]);

    mkheap_heapify(heap, true); 
    return heap;
}

/* 
 * Create a mkheap from an array of readers.
 */
MKHeap *mkheap_from_reader(MKHeapReader *readers, int nreader, MKContext *ctxt)
{
    int i, j;
    MKHeap *heap = (MKHeap *) palloc(sizeof(MKHeap));

    Assert(readers && nreader > 0);
    Assert(ctxt);

    heap->mkctxt = ctxt;
    heap->lvtops = palloc0(ctxt->total_lv * sizeof(MKEntry));
    heap->readers = readers;
    heap->nreader = nreader;

    /* Create initial heap. */
    heap->p = palloc0(sizeof(MKEntry) * (nreader+1));
    heap->alloc_size = nreader+1;

    for(i=0, j=0; i<nreader; ++i)
    {
        MKEntry* e = heap->p + j;
        bool fOK = readers[i].reader(readers[i].mkhr_ctxt, e);

        if(fOK)
        {
            mke_set_reader(e, i); 
            ++j;
        }
    }

    heap->count = j; 
    heap->maxentry = j; 

    if(ctxt->prep[0])
        mk_prepare_array(heap->p, 0, j-1, 0, mk_get_ctxt(ctxt, 0), ctxt->prep[0]);

    mkheap_heapify(heap, true); 
    return heap;
}

/* 
 * Destroy mkheap.  Free entry arrays at each level.
 */
void mkheap_destroy(MKHeap *mkheap)
{
    if(mkheap->p)
        pfree(mkheap->p);

    if(mkheap->lvtops)
        pfree(mkheap->lvtops);

    pfree(mkheap);
}

/*
 * MKHeap siftdown an entry, starting with a hole.  
 */
static void mkheap_siftdown(MKHeap *heap, int hole, MKEntry *e)
{
    int c;
    int left;
    int right;  
    int child;

    static int roundrobin = 0;

    Assert(hole >= 0 && hole < heap->maxentry);

    while(1)
    {
        left = LEFT(hole);
        right = RIGHT(hole);
        child = left;

        if (right < heap->maxentry)
        {
            c = mkheap_compare(heap, heap->p+left, heap->p+right);

            if (c==0)
                child = left + ((roundrobin++) & 1);
            else if(c > 0)
                child = right;
        }

        if (child < heap->maxentry)
        {
            c = mkheap_compare(heap, e, heap->p+child);

            if(c > 0)
            {
                heap->p[hole] = heap->p[child];
                hole = child;
                continue;
            }
        }

        /* We are in right position */
        heap->p[hole] = *e;
        return;
    }

    Assert(!"Never here");
}

/* 
 * Prepare heap (expand lv) at postion cur.  Return how deep (max) into the heap we expand.
 */
static void mkheap_prep_siftdown_lv(MKHeap *mkheap, int lv, int cur, MKPrepare prep, void *ctxt) 
{
    bool expchild = false;

    int c;

    Assert(cur < mkheap->maxentry);
    Assert(cur >= 0);

    /* If necessary, expand right */
    if(RIGHT(cur) < mkheap->maxentry)
    {
        c = mkheap_compare(mkheap, mkheap->p+cur, mkheap->p+RIGHT(cur));
        if(c==0)
        {
            mkheap_prep_siftdown_lv(mkheap, lv, RIGHT(cur), prep, ctxt);
            expchild = true; 
        }
    }

    /* If necessary, expand left */
    if(LEFT(cur) < mkheap->maxentry)
    {
        c = mkheap_compare(mkheap, mkheap->p+cur, mkheap->p+LEFT(cur));
        if(c==0)
        {
            mkheap_prep_siftdown_lv(mkheap, lv, LEFT(cur), prep, ctxt);
            expchild = true;
        }
    }

    Assert(mke_get_lv(mkheap->p+cur) == lv-1);
    (prep)(mkheap->p+cur, ctxt);
    mke_set_lv(mkheap->p+cur, lv); 

    if(expchild)
    {
        MKEntry tmp = mkheap->p[cur];
        mkheap_siftdown(mkheap, cur, &tmp);
    }
}

static void mkheap_save_lvtop(MKHeap *mkheap)
{
    int lv = mke_get_lv(mkheap->p);
    MKContext_LvCommon *ctxt = (MKContext_LvCommon *) mk_get_ctxt(mkheap->mkctxt, lv);

    Assert(mkheap->count > 0);
    Assert(lv < mkheap->mkctxt->total_lv);

    /* Free the old one and copy in the new one */
    (*mkheap->mkctxt->cpfr)(mkheap->lvtops + lv, mkheap->p, ctxt);
}

static inline bool mkheap_need_heapify(MKHeap *mkheap)
{
    return (
            /* More than one entry */
            mkheap_cnt(mkheap) > 1 && 
            /* Not fully expanded */
            mke_get_lv(mkheap->p) < mkheap->mkctxt->total_lv-1 &&
            /* Cannot determine by comp flag */
            (
             mkheap->p[0].compflags == mkheap->p[1].compflags 
             ||
             mkheap->p[0].compflags == mkheap->p[2].compflags 
            )
           );
}

static void mkheap_heapify(MKHeap *mkheap, bool doLv0) 
{
    int i;
    MKEntry tmp;

    /* Build heap, expanded already at lv 0 */
    if(doLv0)
    {
        for(i=PARENT(mkheap->maxentry); i >= 0; --i)
        {
            tmp = mkheap->p[i];
            mkheap_siftdown(mkheap, i, &tmp);
        }
    }

    /* Populate deeper levels */
    while(mke_get_lv(mkheap->p) < mkheap->mkctxt->total_lv - 1)
    {
        int nlv = mke_get_lv(mkheap->p) + 1;
        MKPrepare prep = mkheap->mkctxt->prep[nlv];
        void *ctxtnext = mk_get_ctxt(mkheap->mkctxt, nlv); 

        mkheap_save_lvtop(mkheap);
        mkheap_prep_siftdown_lv(mkheap, nlv, 0, prep, ctxtnext);
    }

#ifdef USE_ASSERT_CHECKING
    if(gp_mk_sort_check)
        mkheap_verify_heap(mkheap, 0);
#endif
}

/* Insert an entry at certain level.  Will recursive go down to next level
 * if necessary. 
 * Return < 0 if smaller than heap top at this and lower levels.
 * Return = 0 if eq to heap top at lowest level 
 * Return > 0 if successfully inserted.  The inserted entry may be at this
 * level or at lower level.
 */
static int mkheap_puget_impl(MKHeap *mkheap, MKEntry *e)
{
    int c = 0; 
    int toplv;
    void *ctxt; 
    MKPrepare prep;
    MKEntry tmp;

    Assert(mkheap->count > 0);

    if(mke_is_empty(e)) 
        --mkheap->count;
    else if (mke_get_run(e) != mke_get_run(mkheap->p))
    {
        /* Not same run, at least prep lv 0 */

        /* Hack for speed.
         * If different run, it must went through at least one "prep".
         * If lv is 0, reuse last prep
         */
        if(mke_get_lv(e) != 0)
        {
            if(mkheap->mkctxt->prep[0])
                (mkheap->mkctxt->prep[0])(e, mk_get_ctxt(mkheap->mkctxt, 0));
            mke_set_lv(e, 0);
        }
    }
    else
    {
        int lv = 0;

        toplv = mke_get_lv(mkheap->p); 
        mke_set_lv(e, lv);

        while (lv < toplv)
        {
            prep = mkheap->mkctxt->prep[lv];
            ctxt = mk_get_ctxt(mkheap->mkctxt, lv);

            if(prep)
                (prep)(e, ctxt);

            c = mkheap_compare(mkheap, e, mkheap->lvtops+lv); 
            if(c!=0)
                break;

            mke_set_lv(e, ++lv);
        }

        /* Smaller than top */
        if(c<0)
            return -1;

        /* We have not done e->lv == toplv yet */
        Assert(mke_get_lv(e) == lv);
        if(lv == toplv)
        {
            prep = mkheap->mkctxt->prep[lv];
            ctxt = mk_get_ctxt(mkheap->mkctxt, lv);

            if(prep)
                (prep)(e, ctxt);

            c = mkheap_compare(mkheap, e, mkheap->p);
            if(c < 0)
                return -1;
        }

        if(c==0)
        {
            /* 
             * Eq and at top level.
             * This means, e is smaller/eq to any entry except the heap top.
             */
            Assert(mke_get_lv(e) == lv);
            Assert(lv == mke_get_lv(mkheap->p));

            /* First find out if e is <, ==, or > than the heap top.  Do 
             * expansion as necessary 
             */
            while(lv < mkheap->mkctxt->total_lv - 1)
            {
                MKPrepare prep = mkheap->mkctxt->prep[lv+1];
                void *ctxtnext = mk_get_ctxt(mkheap->mkctxt, lv+1);

                mkheap_save_lvtop(mkheap); 

                if(prep)
                {
                    (prep)(mkheap->p, ctxtnext);
                    (prep)(e, ctxtnext);

                    ++lv;
                    mke_set_lv(mkheap->p, lv);
                    mke_set_lv(e, lv);
                }

                c = mkheap_compare(mkheap, e, mkheap->p);
                if(c != 0)
                    break;
            }

            if(c<=0)
                return c; 
            else
            {
                /* Swap heaptop with e, then we are done */
                tmp = *e;
                *e = mkheap->p[0];
                mkheap->p[0] = tmp;

                return 1;
            }
        }
    }

    /* Now, I am bigger than top */
    tmp = *e;
    *e = mkheap->p[0];

    /* Sift down a hole to bottom of (current or next) run, depends on tmp.run */
    mkheap_siftdown(mkheap, 0, &tmp);

    if(mkheap_need_heapify(mkheap))
        mkheap_heapify(mkheap, false);

#ifdef USE_ASSERT_CHECKING
    if(gp_mk_sort_check)
        mkheap_verify_heap(mkheap, 0);
#endif

    return 1;
}

/*
 * Insert with run.
 *
 *		If the entry is greater than the top, we can
 * 		insert it using the same run.
 *
 *  	If the entry is less than the heap top, we will
 * 		put it to the next run, and insert it.
 *
 *		Both non-equal cases will ends up with the entry
 * 		inserted, and return a postive number.
 *
 *		If the entry equal to the heaptop, we will NOT
 *		insert the element, but return 0.  Caller need to
 * 		handle this.  In case of sort build runs, that means
 * 		e can be written to current run immediately.
 */
int mkheap_puget_run(MKHeap *mkheap, MKEntry *e, int16 run)
{
    int ins;

    Assert(!mke_is_empty(e));
    mke_set_run(e, run);
    ins = mkheap_puget_impl(mkheap, e); 

    /* Greater than or Equal. */
    if(ins >= 0)
    {
        if(mkheap->mkctxt->enforceUnique && ins == 0)
        {
            ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
                        errmsg("could not create unique index"),
                        errdetail("Table contains duplicated values.")));
        }
        return ins;
    }

    mke_set_run(e, run+1);
    ins = mkheap_puget_impl(mkheap, e); 

    Assert(ins > 0);
    return ins;
}

int mkheap_puget_reader(MKHeap *mkheap, MKEntry *out)
{
    int n;
    bool fOK;
    int ins;
    MKEntry *e = mkheap_peek(mkheap);

    if(!e)
    {
        mke_set_empty(out);
        return -1;
    }

    Assert(!mke_is_empty(e));
    Assert(mke_get_run(e) == 0);

    n = mke_get_reader(e); 
    Assert(n>=0 && n<mkheap->nreader);

    fOK = mkheap->readers[n].reader(mkheap->readers[n].mkhr_ctxt, out); 
    if(fOK)
        mke_set_reader(out, n);
    else
        mke_set_empty(out); 

    Assert(mke_get_run(out) == 0);

    ins = mkheap_puget_impl(mkheap, out);
    if(mkheap->mkctxt->enforceUnique && ins == 0)
    {
        ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
                    errmsg("could not create unique index"),
                    errdetail("Table contains duplicated values.")));
    }

    Assert(ins >= 0);
    Assert(mke_get_reader(out) == n);
    Assert(!mke_is_empty(out));

    return ins;
}

int mkheap_puget(MKHeap *mkheap, MKEntry *out)
{
    int ret = 0;
    if(mkheap->nreader > 0)
        ret = mkheap_puget_reader(mkheap, out);
    else
        ret = mkheap_puget_impl(mkheap, out);

    Assert(out);
    if(mkheap->mkctxt->cpfr)
        (mkheap->mkctxt->cpfr)(out, NULL, mk_get_ctxt(mkheap->mkctxt, mke_get_lv(out)));
    return ret;
}

MKEntry *mkheap_peek(MKHeap *mkheap)
{
    if(mkheap->count == 0)
        return NULL;

    Assert(!mke_is_empty(mkheap->p));
    return mkheap->p;
}

#ifdef USE_ASSERT_CHECKING
void mkheap_verify_heap(MKHeap *heap, int top)
{
    int empty_cnt = 0;
    int i; 
    MKEntry e;

    if(heap->count == 0)
        return;

    Assert(heap->count > 0);
    Assert(heap->maxentry > 0);
    Assert(heap->count <= heap->maxentry);
    Assert(heap->maxentry <= heap->alloc_size);

    e = heap->p[0];
    mke_set_lv(&e, 0);
    mke_clear_refc_copied(&e);

    /* Checking for lvtops */
    for(i=0; i<mke_get_lv(heap->p); ++i)
    {
        MKPrepare prep = heap->mkctxt->prep[i];
        void *ctxt = mk_get_ctxt(heap->mkctxt, i);
        int c;

        /* Too much trouble dealing with ref counters.  Just don't */
        /*
           if(!mke_test_flag(heap->lvtops+i, MKE_RefCnt | MKE_Copied))
           {
         */
        if(prep)
        {
            mke_set_lv(&e, i);
            (prep) (&e, ctxt);
        }

        c = mkheap_compare(heap, &e, heap->lvtops+i);
        Assert(c==0);
        /*
           }
         */
    }

    /* Verify Heap property */
    for(i=top; i<heap->maxentry; ++i)
    {
        int left = LEFT(i);
        int right = RIGHT(i);
        int cl, cr;

        if(mke_is_empty(heap->p+i))
            ++empty_cnt;

        if(left >= heap->maxentry)
            continue;

        cl = mkheap_compare(heap, heap->p+i, heap->p+left); 
        Assert(cl<=0);
        if(i==0 && cl==0)
            Assert(mke_get_lv(heap->p) == heap->mkctxt->total_lv-1);

        if(right >= heap->maxentry)
            continue;

        cr = mkheap_compare(heap, heap->p+i, heap->p+right); 
        Assert(cr<=0);
        if(i==0 && cr==0)
            Assert(mke_get_lv(heap->p) == heap->mkctxt->total_lv-1);
    }
}
#endif
