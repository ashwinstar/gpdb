/* 
 * tuplesort_mkheap.c
 * 		Multi level key quick sort.
 * 
 * Copyright (c) Greenplum Inc, 2008.
 *
 * A multi level key Bentley McIlroy 3-way partitioning quick sort.
 * 	See [1] J. Bentley, M. McIlroy.  Engineering a sort function, 
 * 		Software Practice and Experience, Vol 23(11) Nov, 1993.
 * 	    [2] R. Sedgewick, J. Bentley. Quicksort is optimal.
 */

#include "postgres.h"
#include "utils/tuplesort.h"
#include "utils/tuplesort_mk.h"

#include "miscadmin.h"

#ifdef MKQSORT_VERIFY 
extern void mkqsort_verify(MKEntry *a, int l, int r, MKContext *mkctxt);
#endif

/**
 * Given an array, swap the entries at a[i] and a[j]
 */
static inline void mkqs_swap(MKEntry *a, int i, int j)
{
	if(i!=j)
	{
		MKEntry tmp = a[i];
		a[i] = a[j];
		a[j] = tmp;
	}
}

/**
 * Compare the two entries, only does comparison using compFlags and the level in the context
 */
static inline int32 mkqs_comp(MKCompare comp, MKEntry *a, MKEntry *b, void *ctxt)
{
	int ret = a->compflags - b->compflags;

	if (ret == 0 && !mke_is_null(a))
		ret = (comp)(a, b, ctxt);

	return ret;
}

/**
 *  Return the median of three multi-key entries
 */
static inline MKEntry *mkqs_med3(MKEntry *a, MKEntry *b, MKEntry *c, MKCompare comp, MKLvContext *ctxt) 
{
	return mkqs_comp(comp, a, b, ctxt) < 0 ? 
                        (mkqs_comp(comp, b, c, ctxt) < 0 ? b : mkqs_comp(comp, a, c, ctxt) < 0 ? c : a )
                        :
                        (mkqs_comp(comp, b, c, ctxt) > 0 ? b : mkqs_comp(comp, a, c, ctxt) > 0 ? c : a )
                        ;
}

/**
 * Choose a pivot from the array of elements (a) from between left (inclusive) and right (inclusive).
 *
 * lv/comp/ctxt combine to allow comparison.  Note that, as usual with the multi-key structure, comparision is only done
 *   up to a certain level
 */
static inline MKEntry *mkqs_choose_pivot(MKEntry *a, int left, int right, int lv, MKCompare comp, MKLvContext *ctxt)
{
	int s;
	int n = right - left + 1;
	MKEntry *pl, *pm, *pn;

	Assert(n >= 0);
	
	pm = a + (left + n/2);

	if(n > 7)
	{
		pl = a+left;
		pn = a+right;

		if(n > 40)
		{
			s = n/8;
			pl = mkqs_med3(a+left, a+left+s, a+left+s*2, comp, ctxt);
			pm = mkqs_med3(a+left+n/2-s, pm, a+left+n/2+s, comp, ctxt);
			pn = mkqs_med3(a+right-s*2, a+right-s, a+right, comp, ctxt);
		}

		pm = mkqs_med3(pl, pm, pn, comp, ctxt);
	}

	return pm;
}

/*
 * Three way partition quick sort.
 *
 * left and right are both INCLUSIVE
 */
static void mk_qsort_part3(MKEntry *a, int left, int right, int lv, MKContext *mkqs_ctxt, int *i, int *j)
{
	/* p, q points to the last equals we put to both end */
	int p = left-1;
	int q = right+1;

	int k;
	MKCompare comp = mkqs_ctxt->comp; 
    MKLvContext *lvctxt = mkqs_ctxt->lvctxt + lv;

	MKEntry v = {0, };
	MKEntry *pv;

	Assert(left < right);

	/*
	 * *i points to the known <= upper,
	 * *j points to the known >= lower.
	 * 
	 * Initially, we know nothing, so point them to out of bounds.
	 */
	*i = left-1;
	*j = right+1;

	/* Choose a pivot */
	pv = mkqs_choose_pivot(a, left, right, lv, comp, lvctxt);

	/* Save pivot in v */ 
	/* Hacking a ref count */ // TODO: understand this
	if(mke_is_refc(pv))
		mkqs_ctxt->cpfr(&v, pv, lvctxt);
	else 
		v = *pv;

	while(1)
	{
		int tmpi;
		int c1 = -1;
		int c2 = 1;

		while(1)
		{
			/* increase i from the left until a[i] >= pivot */
			while(++(*i) < (*j) &&
				 (c1 = mkqs_comp(comp, a+(*i), &v, lvctxt)) < 0)
			{
				Assert(*i >= left && *i<= right);
				Assert(*j >= left && *j<= right+1);
				Assert(*i <= *j);
			}

			/* now i refers to the first value that is >= the pivot */
			if(*i == *j || c1 > 0)
				break;

			/* since *i equals the pivot, put the equals one at the beginning and move one there to take its place. */
			mkqs_swap(a, ++p, *i);
		}

		if(*i == *j)
		{ 
			/**
			 * Caught up, back down and done loop
			 */
			--(*i);
			break; 
		}

		/**
		 *  now, p points to the last value of a run of values equal to the pivot
		 *  i points one beyond a run of values <= the pivot (and between p+1 and i they are < the pivot)
		 */

		/* tmpi points at the last of those <= pivot */
		tmpi = (*i)-1;

		/* same case as above, but from the upper end and stashing equal values to the q end */
		while(1)
		{
			while(--(*j) > tmpi &&
					(c2 = mkqs_comp(comp, &v, a+(*j), lvctxt)) < 0)
			{
				Assert(tmpi >= left-1 && tmpi<= right);
				Assert(*j >= left && *j<= right);
				Assert(tmpi <= *j);
			}

			if(tmpi == *j || c2 > 0)
				break;

			mkqs_swap(a, *j, --q);
		}

		if(tmpi == *j)
		{
			--(*i); /* make *i = tmpi */
			++(*j); /* move j up to avoid overlap */
			break;
		}

		Assert(*i < *j);
		mkqs_swap(a, *i, *j);

		if((*i)+1 == *j)
			break;
	}

	/* Now *i points to where the run of values <= the pivot ends
     * And *j points to where the run of values >= the pivot begins
     */
	Assert(*i+1 == *j);
	Assert(*j>=left && *i<=right);

	/**
	 * Push all the equal values at the beginning to the middle (at i)
	 */
	for(k=left; k<=p; ++k, --(*i))
	{
		Assert(k>=left && k<=right);
		Assert(*i>=left && *i<=right);
		mkqs_swap(a, k, *i);
	}

	/**
	 * Push all the equal values at the end to the middle (at j)
	 */
	for(k=right; k>=q; --k, ++(*j))
	{
		Assert(k>=left && k<=right);
		Assert(*j>=left && *j<=right);
		mkqs_swap(a, *j, k);
	}

	/*
	 * Now *i points to the last value of the run of values < the pivot
	 * And *j points to the last value of the run of values > the pivot
	 */
	Assert(*i >= left-1 && *i < right);
	Assert(*j > left && *j <= right+1);
	Assert(*j >= *i+2);

	/* Hack ref count */
	if(mke_is_refc(&v))
		mkqs_ctxt->cpfr(&v, NULL, lvctxt);
}

void mk_qsort_impl(MKEntry *a, int left, int right, int lv, bool lvdown, MKContext *ctxt, bool seenNull)
{
	int i; 
	int j; 

	Assert(ctxt);
	Assert(lv < ctxt->total_lv);

    CHECK_FOR_INTERRUPTS();

	if(right <= left)
		return;
	
	/* Prepare at level lv */
	if(lvdown && ctxt->prep) 
        mk_prepare_array(a, left, right, lv, ctxt, ctxt->prep);

	/* 
	 * According to Bentley & McIlroy [1] (1993), using insert sort for case 
	 * n < 7 is a significant saving.  However, according to Sedgewick & 
	 * Bentley [2] (2002), the wisdom of new millenium is not to special case
	 * smaller cases.  Here, we do not special case it because we want to save
	 * memtuple_getattr, and expensive comparisons that has been prepared.
	 *
	 * XXX Find out why we have a new wisdom in [2] and impl. & compare.
	 */
	mk_qsort_part3(a, left, right, lv, ctxt, &i, &j);

#if 0 
	/* Check that the partition is done right. */
	{
		MKCompare comp = ctxt->comp[lv];
		void *cmpctxt = mk_get_ctxt(ctxt, lv); 

		int x = 0;
		int c;

		MKEntry *pv = a+i+1;

		for(x=left; x<=i; ++x)
		{
			c = mkqs_comp(comp, a+x, pv, cmpctxt);
			Assert(c < 0);
		}

		for(x=i+1; x<j; ++x)
		{
			if(a+x != pv)
			{
				c = mkqs_comp(comp, a+x, pv, cmpctxt);
				Assert(c == 0);
			}
		}

		for(x=j; x<=right; ++x)
		{
			c = mkqs_comp(comp, a+x, pv, cmpctxt);
			Assert(c > 0);
		}
	}
#endif

	/* recurse to left chunk */
	mk_qsort_impl(a, left, i, lv, false, ctxt, seenNull);

	/* recurse to right chunk */
	mk_qsort_impl(a, j, right, lv, false, ctxt, seenNull);

	if(lv < ctxt->total_lv-1)
	{
		/*
		 * [i+1,j-1] defines the pivot region which was all equal at level lv.  So increase the level and compare that region!
		 */
		mk_qsort_impl(a, i+1, j-1, lv+1, true, ctxt, seenNull || mke_is_null(a+i+1)); /* a + i + 1 points to the pivot */
	}
	else
	{
		/* values are all equal to the deepest level...no need for more compares, but check uniqueness */
		if(ctxt->enforceUnique && j-1 > i+1 && !seenNull && !mke_is_null(a+i+1)) /* a + i + 1 points to the pivot */
			ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
					errmsg("could not create unique index"),
					errdetail("Table contains duplicate values.")));
	}

#ifdef MKQSORT_VERIFY 
	if(lv == 0)
		mkqsort_verify(a, left, right, ctxt);
#endif
}

#ifdef MKQSORT_VERIFY 
static int mkqsort_comp_entry_all_lv(MKEntry *a, MKEntry *b, MKContext *mkctxt)
{
	int i;
	int c;

	MKEntry aa = {0, }; 
	MKEntry bb = {0, }; 

	(*mkctxt->cpfr)(&aa, a, mk_get_ctxt(mkctxt, mke_get_lv(a)));
	(*mkctxt->cpfr)(&bb, b, mk_get_ctxt(mkctxt, mke_get_lv(b)));

	for(i=0; i<mkctxt->total_lv; ++i)
	{
		MKPrepare prep = mkctxt->prep[i];
		MKCompare comp = mkctxt->comp[i];

		void *ctxt = mk_get_ctxt(mkctxt, i);
        int32 nf1, nf2;

		if(prep)
		{
			(prep)(&aa, ctxt);
			(prep)(&bb, ctxt);
		}

        nf1 = mke_get_nullbits(&aa);
        nf2 = mke_get_nullbits(&bb);

        c = nf1 - nf2;
        if (c!=0)
            return c;

        if (!mke_is_null(&aa))
        {
            Assert(!mke_is_null(&bb));
            c = (comp)(&aa, &bb, ctxt);

            if (c!=0)
                return c;
        }
	}

	return 0;
}

void mkqsort_verify(MKEntry *a, int i, int j, MKContext *mkctxt)
{
	int c;
	for(; i<j; ++i)
	{
		c = mkqsort_comp_entry_all_lv(a+i, a+i+1, mkctxt);
		Assert(c <= 0);
	}
}
#endif
