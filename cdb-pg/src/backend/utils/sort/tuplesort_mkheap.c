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
 *	5. Same run, same level, then we compare the datum.
 *		
 * These rules guarantee the heap top is minimal of current run.  However, at
 * deeper level, mkheap_compare return 0 does not mean the two entry are really
 * equal.  They are just equal to cerntain level.
 */
static inline int mkheap_compare(MKHeap *heap, MKEntry *a, MKEntry *b) 
{
	int ret;

	/* First, empty entry is always bigger */
	if(mke_test_flag(b, MKE_Empty))
	{
		if(mke_test_flag(a, MKE_Empty))
			return 0;
		return -1;
	}

	if(mke_test_flag(a, MKE_Empty))
		return 1;

	/* Now test lvs, deeper level is smaller */
	if(a->lv != b->lv)
	{
		Assert(a->lv <= heap->p[0].lv);
		Assert(b->lv <= heap->p[0].lv);

		return (a->lv < b->lv) ? 1 : -1;
	}

	/* Now test runs. prev run is smaller */
	if(a->run != b->run)
		return (a->run < b->run) ? -1 : 1;

	/* Same run.  Invoke comparison, and reverse if reverse. */
	ret = (heap->mkctxt->comp[a->lv])(a, b, mk_get_ctxt(heap->mkctxt, a->lv));
	return heap->mkctxt->limit == 0 ? ret : (-ret);
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
			Assert(entries->lv == 0);
			Assert(entries->reader == 0);
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
			e->reader = i;
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
 * Choose a child to sift down a hole (empty entry)
 */
static int mkheap_siftdown_choosecld(MKHeap *heap, int hole, int run)
{
	int left = LEFT(hole);
	int right = RIGHT(hole);
	int c;

	/* 
	 * Left will be a choice only if
	 * 	1. There is a left (not >= maxentry).
	 *	2. Left is not a hole itself.
	 *	3. We don't put a hole down into next run (because we 
	 *		will sift up an entry later in this run.)
	 */
	if(left >= heap->maxentry 
		  || mke_test_flag(heap->p+left, MKE_Empty)
		  || heap->p[left].run > run 
	  )
		left = 0;

	/* Same for right */
	if(right >= heap->maxentry 
		  || mke_test_flag(heap->p+right, MKE_Empty)
		  || heap->p[right].run > run 
	  )
		right = 0;

	if(!left)
		return right;
	if(!right)
		return left;

	/* Return smaller of two child */
	c =  mkheap_compare(heap, heap->p+left, heap->p+right);

	if(c <= 0)
		return left;
	return right;
}

/*
 * MKHeap siftdown a hole.  Return the hole position.
 */
static void mkheap_siftdown(MKHeap *heap, MKEntry *e)
{
	int hole = 0;
	int cld;
	int c;

	while(1)
	{
		cld = mkheap_siftdown_choosecld(heap, hole, e->run);
		
		/* Reached bottom, stop */
		if(cld == 0)
			break;

		c = mkheap_compare(heap, e, heap->p+cld);
		if(c <= 0)
			break;

		heap->p[hole] = heap->p[cld]; 
		hole = cld;
	}

	heap->p[hole] = *e;
}

/* Sift up */
static int mkheap_siftup(MKHeap *heap, int cur, MKEntry *e)
{
	int c, p;

	while(1) 
	{
		/* Root, cannot go any higher. */
		if(cur == 0)
		{
			heap->p[0] = *e;
			return 0;
		}

		p = PARENT(cur);
		c = mkheap_compare(heap, heap->p+p, e); 

		/* We are in pos */
		if(c <= 0)
		{
			heap->p[cur] = *e;
			return cur;
		}

		heap->p[cur] = heap->p[p];
		cur = p;
	}

	Assert(!"Not here");
	return -1;
}

/* 
 * Prepare heap (expand lv) at postion cur.  Return how deep (max) into the heap we expand.
 */
static int mkheap_prep_lv(MKHeap *mkheap, int lv, int cur, MKPrepare prep, void *ctxt) 
{
	int n;
	int max = cur;

	int c;

	Assert(cur < mkheap->maxentry);

	/* If necessary, expand right */
	if(RIGHT(cur) < mkheap->maxentry)
	{
		c = mkheap_compare(mkheap, mkheap->p+cur, mkheap->p+RIGHT(cur));
		if(c==0)
		{
			n = mkheap_prep_lv(mkheap, lv, RIGHT(cur), prep, ctxt);
			if(n>max)
				max = n;
		}
	}
	
	/* If necessary, expand left */
	if(LEFT(cur) < mkheap->maxentry)
	{
		c = mkheap_compare(mkheap, mkheap->p+cur, mkheap->p+LEFT(cur));
		if(c==0)
		{
			n = mkheap_prep_lv(mkheap, lv, LEFT(cur), prep, ctxt);
			if(n>max)
				max = n;
		}
	}

	Assert(mkheap->p[cur].lv == lv-1);
	if(prep && max > 0)
	{
		(prep)(mkheap->p+cur, ctxt);
		mkheap->p[cur].lv = lv;
	}

	return max;
}

static void mkheap_save_lvtop(MKHeap *mkheap)
{
	int lv = mkheap->p[0].lv;
	MKContext_LvCommon *ctxt = (MKContext_LvCommon *) mk_get_ctxt(mkheap->mkctxt, lv);
	
	Assert(mkheap->count > 0);
	Assert(lv < mkheap->mkctxt->total_lv);

	/* Free the old one and copy in the new one */
	(*mkheap->mkctxt->cpfr)(mkheap->lvtops + lv, mkheap->p, ctxt);
}

static void mkheap_heapify(MKHeap *mkheap, bool doLv0) 
{
	int i;
	MKEntry tmp;

	/* Build heap, expanded already at lv 0 */

	if(doLv0)
	{
		for(i=1; i<mkheap->maxentry; ++i)
		{
			tmp = mkheap->p[i];
			mkheap_siftup(mkheap, i, &tmp);
		}
	}

	/* Populate deeper levels */
	while(mkheap->p[0].lv < mkheap->mkctxt->total_lv - 1)
	{
		int i;
		int lv = mkheap->p[0].lv;
		MKPrepare prep = mkheap->mkctxt->prep[lv+1];
		void *ctxtnext = mk_get_ctxt(mkheap->mkctxt, lv+1);
		int last;
		
		mkheap_save_lvtop(mkheap);
		last = mkheap_prep_lv(mkheap, lv+1, 0, prep, ctxtnext);

		if(last == 0)
			break;

		/* Heapify, and looping for expand next level j*/
		for(i=1; i<=last; ++i)
		{
			tmp = mkheap->p[i];
			mkheap_siftup(mkheap, i, &tmp);
		}
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

	if(mke_test_flag(e, MKE_Empty))
		--mkheap->count;
	else if (e->run != mkheap->p[0].run)
	{
		/* Not same run, at least prep lv 0 */

		/* Hack for speed.
		 * If different run, it must went through at least one "prep".
		 * If lv is 0, reuse last prep
		 */
		if(e->lv != 0)
		{
			if(mkheap->mkctxt->prep[0])
				(mkheap->mkctxt->prep[0])(e, mk_get_ctxt(mkheap->mkctxt, 0));
			e->lv = 0;
		}
	}
 	else
	{
		toplv = mkheap->p[0].lv;

		for(e->lv = 0; e->lv < toplv; ++e->lv)
		{
			/* Compare with heaptop of current level */
			prep = mkheap->mkctxt->prep[e->lv];
			ctxt = mk_get_ctxt(mkheap->mkctxt, e->lv);

			if(prep)
				(prep)(e, ctxt);

			c = mkheap_compare(mkheap, e, mkheap->lvtops+e->lv); 
			if(c!=0)
				break;
		}

		/* Smaller than top */
		if(c<0)
			return -1;

		/* We have not done e->lv == toplv yet */
		if(e->lv == toplv)
		{
			prep = mkheap->mkctxt->prep[e->lv];
			ctxt = mk_get_ctxt(mkheap->mkctxt, e->lv);

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

			Assert(e->lv == mkheap->p[0].lv);

			/* First find out if e is <, ==, or > than the heap top.  Do 
			 * expansion as necessary 
			 */
			while(e->lv < mkheap->mkctxt->total_lv - 1)
			{
				int lv = mkheap->p[0].lv;
				MKPrepare prep = mkheap->mkctxt->prep[lv+1];
				void *ctxtnext = mk_get_ctxt(mkheap->mkctxt, lv+1);

				mkheap_save_lvtop(mkheap); 

				if(prep)
				{
					(prep)(mkheap->p, ctxtnext);
					(prep)(e, ctxtnext);
					++mkheap->p[0].lv;
					++e->lv;
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
	mkheap_siftdown(mkheap, &tmp);
	
	/* Heapify if needed */
	if(!mkheap_empty(mkheap) && mkheap->p[0].lv < mkheap->mkctxt->total_lv-1)
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

	Assert(!mke_test_flag(e, MKE_Empty));
	e->run = run;
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

	++e->run;
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
		out->flags = MKE_Empty;
		return -1;
	}

	Assert(!mke_test_flag(e, MKE_Empty));
	Assert(e->run == 0);

	n = e->reader;
	Assert(n>=0 && n<mkheap->nreader);

	fOK = mkheap->readers[n].reader(mkheap->readers[n].mkhr_ctxt, out); 
	if(!fOK)
		out->flags = MKE_Empty;
	else
		out->reader = n;

	Assert(out->run == 0);

	ins = mkheap_puget_impl(mkheap, out);
	if(mkheap->mkctxt->enforceUnique && ins == 0)
	{
		ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
					errmsg("could not create unique index"),
					errdetail("Table contains duplicated values.")));
	}

	Assert(ins >= 0);
	Assert(out->reader == n);
	Assert(!mke_test_flag(out, MKE_Empty));

	return ins;
}

int mkheap_puget(MKHeap *mkheap, MKEntry *out)
{
	if(mkheap->nreader > 0)
		return mkheap_puget_reader(mkheap, out);
	else
		return mkheap_puget_impl(mkheap, out);
}

MKEntry *mkheap_peek(MKHeap *mkheap)
{
	if(mkheap->count == 0)
		return NULL;

	Assert(!mke_test_flag(mkheap->p, MKE_Empty));
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
	e.lv = 0;
	e.flags = 0;

	/* Checking for lvtops */
	for(i=0; i<heap->p[0].lv; ++i)
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
				e.lv = i;
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

		if(mke_test_flag(heap->p+i, MKE_Empty)) 
			++empty_cnt;

		if(left >= heap->maxentry)
			continue;

		cl = mkheap_compare(heap, heap->p+i, heap->p+left); 
		Assert(cl<=0);
		if(i==0 && cl==0)
			Assert(heap->p[0].lv == heap->mkctxt->total_lv-1);

		if(right >= heap->maxentry)
			continue;

		cr = mkheap_compare(heap, heap->p+i, heap->p+right); 
		Assert(cr<=0);
		if(i==0 && cr==0)
			Assert(heap->p[0].lv == heap->mkctxt->total_lv-1);
	}
}
#endif
