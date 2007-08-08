/*
 * tuplestorenew.c
 *		A better tuplestore
 *
 */

#include "postgres.h"
#include "access/heapam.h"
#include "executor/instrument.h"
#include "storage/buffile.h"
#include "utils/tuplestorenew.h"

typedef struct NTupleStorePageHeader
{
	long blockn;        /* block number */
	long page_flag;     /* flag */ 
	void *prev_1;    	/* in mem list */	
	void *next_1;		/* in mem list */
	int pin_cnt;		/* in mem pin count */
	int data_bcnt;		/* tail of payload data */
	int slot_cnt;		/* number of slot writen in the page */
	int first_slot;     /* first valid slot */
} NTupleStorePageHeader;

typedef struct NTupleStorePageSlotEntry
{
	short data_start; 	/* data starts */
	short size;			/* size of the entry */
} NTupleStorePageSlotEntry;

#define NTS_MAX_ENTRY_SIZE (BLCKSZ - sizeof(NTupleStorePageHeader) - sizeof(NTupleStorePageSlotEntry))

/* Page.  data grown incr, slot grow desc */
typedef struct NTupleStorePage
{
	NTupleStorePageHeader header;
	char data[NTS_MAX_ENTRY_SIZE]; 
	NTupleStorePageSlotEntry slot[1];
} NTupleStorePage;

/* some convinient macros */
/* page flag bits */
#define NTS_PAGE_DIRTY 1
static inline bool nts_page_is_dirty(NTupleStorePage * page)
{
	return (page->header.page_flag & NTS_PAGE_DIRTY) != 0;
}
static inline void nts_page_set_dirty(NTupleStorePage *page, bool dirty) 
{
	if(dirty)
		(page)->header.page_flag |= NTS_PAGE_DIRTY;
	else
		(page)->header.page_flag &= (~NTS_PAGE_DIRTY);
}

#define NTS_ALLIGN8(n) (((n)+7) & 7) 

/* page stuff.  Note slot grow desc, so the minus array index stuff. */
static inline long nts_page_blockn(NTupleStorePage *page) { return page->header.blockn; }
static inline void nts_page_set_blockn(NTupleStorePage *page, long blockn) { page->header.blockn = blockn; }
static inline NTupleStorePage *nts_page_prev(NTupleStorePage *page) { return (NTupleStorePage *) page->header.prev_1; }
static inline void nts_page_set_prev(NTupleStorePage *page, NTupleStorePage* prev) { page->header.prev_1 = (void *) prev; }
static inline NTupleStorePage *nts_page_next(NTupleStorePage *page) { return (NTupleStorePage *) page->header.next_1; }
static inline void nts_page_set_next(NTupleStorePage *page, NTupleStorePage *next) { page->header.next_1 = (void *) next; }
static inline int nts_page_pin_cnt(NTupleStorePage *page) { return page->header.pin_cnt; }
static inline void nts_page_set_pin_cnt(NTupleStorePage *page, int pc) { page->header.pin_cnt = pc; }
static inline void nts_page_incr_pin_cnt(NTupleStorePage *page) { ++page->header.pin_cnt; }
static inline void nts_page_decr_pin_cnt(NTupleStorePage *page) { --page->header.pin_cnt; }
static inline int nts_page_slot_cnt(NTupleStorePage *page) { return page->header.slot_cnt; }
static inline void nts_page_set_slot_cnt(NTupleStorePage *page, int sc) { page->header.slot_cnt = sc; }
static inline void nts_page_incr_slot_cnt(NTupleStorePage *page) { ++page->header.slot_cnt; }
static inline int nts_page_data_bcnt(NTupleStorePage *page) { return page->header.data_bcnt; }
static inline void nts_page_set_data_bcnt(NTupleStorePage *page, int bc) { page->header.data_bcnt = bc; }
static inline int nts_page_first_valid_slotn(NTupleStorePage *page) { return page->header.first_slot; }
static inline void nts_page_set_first_valid_slotn(NTupleStorePage *page, int fs) { page->header.first_slot = fs; }
static inline int nts_page_valid_slot_cnt(NTupleStorePage *page)
{
	return nts_page_slot_cnt(page) - nts_page_first_valid_slotn(page);
}
static inline NTupleStorePageSlotEntry *nts_page_slot_entry(NTupleStorePage *page, int slotn)
{
	return &(page->slot[-slotn]);
}
static inline char *nts_page_slot_data_ptr(NTupleStorePage *page, int slotn)
{
	return page->data + nts_page_slot_entry(page, slotn)->data_start; 
}
static inline int nts_page_slot_data_len(NTupleStorePage *page, int slotn)
{
	return nts_page_slot_entry(page, slotn)->size;
}

static inline NTupleStorePageSlotEntry *nts_page_next_slot_entry(NTupleStorePage *page)
{
	return nts_page_slot_entry(page, nts_page_slot_cnt(page));
}
static inline char *nts_page_next_slot_data_ptr(NTupleStorePage *page)
{
	return page->data + NTS_ALLIGN8(nts_page_data_bcnt(page));
}
static inline int nts_page_avail_bytes(NTupleStorePage *page)
{
	return (char *) nts_page_slot_entry(page, nts_page_slot_cnt(page)) - 
		(char *) nts_page_next_slot_data_ptr(page);
}

static inline void verify_nts_page_format_is_sane()
{
	Assert(sizeof(NTupleStorePage) == BLCKSZ); 
	Assert(NTS_ALLIGN8(sizeof(NTupleStorePageHeader)) == sizeof(NTupleStorePageHeader)); 
	Assert(offsetof(NTupleStorePage, data) == sizeof(NTupleStorePageHeader)); 
	Assert(offsetof(NTupleStorePage, slot) == BLCKSZ-sizeof(NTupleStorePageSlotEntry)); 
}

static inline void update_page_slot_entry(NTupleStorePage *page, int len)
{
	short data_start = (short) NTS_ALLIGN8( nts_page_data_bcnt(page) );
	NTupleStorePageSlotEntry *slot = nts_page_next_slot_entry(page);

	Assert(len >= 0 && len < NTS_MAX_ENTRY_SIZE);

	slot->data_start = data_start;
	slot->size = (short) len;

	nts_page_incr_slot_cnt(page);
	nts_page_set_data_bcnt(page, data_start+len);
	nts_page_set_dirty(page, true);
}

/* tuple store type */
#define NTS_NOT_READERWRITER 1
#define NTS_IS_WRITER 2
#define NTS_IS_READER 3

struct NTupleStore
{
	BufFile *pfile; 	/* underlying backed file */

	int page_max;   /* max page the store can use */
	int page_effective_max; 
	int page_cnt; 	/* page count */
	int pin_cnt;    /* pin count */
	NTupleStorePage *first_page; 		/* first page */
	NTupleStorePage *last_page;     	/* last page */
	NTupleStorePage *first_free_page; 	/* free page, cached to save palloc */

	long first_ondisk_blockn;           /* first blockn that is written to disk */
	int rwflag;  /* if I am ordinary store, or a reader, or a writer of readerwriter (share input) */

	/* some stats, which are not restictly required */
	int racc_cnt;   
	int wacc_cnt;

	Instrumentation *instrument;
};

struct NTupleStoreAccessor
{
	NTupleStore *store;
	bool isWriter;

	NTupleStorePos  pos;
	NTupleStorePage *page;
};

void ntuplestore_setinstrument(NTupleStore *st, struct Instrumentation *instr)
{
	st->instrument = instr;
}

#ifdef DEBUG_NTUPLESTORE
/* Sanity checks on a TupleTableSlot.  Will fail if someone called advance/seek etc.
 * on accessor 
 */
void check_tableslot_tuplestore(TupleTableSlot *slot)
{
	if(slot->tts_nts_slot)
	{
		NTupleStoreAccessor *acc = (NTupleStoreAccessor *) slot->tts_nts_slot;
		void *data;
		int  len;

		Assert(!slot->tts_shouldFree);
		ntuplestore_acc_current_data(acc, &data, &len);

		Assert((char *) data == (char *) slot->tts_mintuple);
	}
}

/* Verify tuplestore page lists are sane */
static void verify_ts_page_list(NTupleStore * ts)
{
	int pg_cnt = 0;
	int pin_cnt = 0;
	NTupleStorePage *p = ts->first_page;
	NTupleStorePage *pnext;
	
	nts_verify_page_formate_is_sane();

	Assert(p);
	Assert(nts_page_blockn(p) >= ts->first_ondisk_blockn);
	AssertEquivalent(ts->last_page == NULL, ts->rwflag == NTS_IS_READER); 
	AssertImply((ts->rwflag == NTS_IS_READER || ts->rwflag == NTS_IS_WRITER), ts->pfile);
	
	pnext = nts_page_next(p); 
	{
		bool validstarted = false;
		short s = 0;
		int prev_data_start = 0;
		int prev_data_len = 0;

		for(s = 0; s<nts_page_slot_cnt(pnext); ++s)
		{
			NTupleStoreSlotEntry* slotentry = nts_page_slot_entry(pnext, s); 
			if(slotentry->data_start < 0 || slotentry->size < 0)
			{
				Assert(validstarted == false);
				continue;
			}

			validstarted = true;

			Assert(slotentry->data_start >= prev_data_start + prev_data_len);
			Assert(slotentry->data_start == NTS_ALLIGN8(slotentry->data_start));

			prev_data_start = slotentry->data_start;
			prev_data_len = slotentry->size;
		}
	}

	while(p)
	{
		++pg_cnt;
		pin_cnt += nts_page_pin_cnt(p); 

		Assert(nts_page_blockn(p) >= 0);
		Assert(nts_page_pin_cnt(p) >= 0);
		Assert(nts_page_slot_cnt(p) >= 0);
		Assert(nts_page_avail_bytes(p) >= 0);

		if(pnext)
		{
			short s = 0;
			int prev_data_start = 0;
			int prev_data_len = 0;

			Assert(p == nts_page_prev(pnext)); 
			Assert(nts_page_blockn(p) < nts_page_blockn(pnext)); 
			Assert(nts_page_slot_cnt(p) >= 0);
			Assert(nts_page_first_valid_slotn(p) == 0);

			for(s = 0; s<nts_page_slot_cnt(pnext); ++s)
			{
				NTupleStoreSlotEntry* slotentry = nts_page_slot_entry(pnext, s);
				Assert(slotentry->data_start >= prev_data_start + prev_data_len);
				Assert(slotentry->data_start == NTS_ALLIGN8(slotentry->data_start));

				prev_data_start = slotentry->data_start;
				prev_data_len = slotentry->size;
			}
		}
	}

	Assert(pin_cnt == ts->pin_cnt);

	p = ts->first_free_page;
	while(p)
	{
		++pg_cnt;
		p = nts_page_next(p);
	}

	Assert(pg_cnt == ts->page_cnt);
}
#endif

static inline void init_page(NTupleStorePage *page)
{
	nts_page_set_blockn(page, -1);
	nts_page_set_prev(page, NULL);
	nts_page_set_next(page, NULL);
	nts_page_set_dirty(page, true);
	nts_page_set_pin_cnt(page, 0);
	nts_page_set_data_bcnt(page, 0);
	nts_page_set_slot_cnt(page, 0);
}

static inline void nts_pin_page(NTupleStore* nts, NTupleStorePage *page)
{
	Assert(nts && page);
	nts_page_incr_pin_cnt(page);
	++nts->pin_cnt;
}

static inline void nts_unpin_page(NTupleStore *nts, NTupleStorePage *page)
{
	Assert(nts && nts->pin_cnt > 0);
	Assert(page && nts_page_pin_cnt(page) > 0); 
	
	--nts->pin_cnt;
	nts_page_decr_pin_cnt(page);
}

/* Prepend to list head.  Pass in pointer of prev, and next so that the function
 * can be reused later if the one page can be on server lists 
 */
static NTupleStorePage *nts_prepend_to_dlist(
		NTupleStorePage *head, 
		NTupleStorePage *page, 
		void **prev, void **next)
{
	void **head_prev;

	*prev = NULL;

	if(!head)
	{
		(*next) = NULL;
		return page;
	}

	(*next) = head;
	head_prev = (void **) ((char *) head + ((char *) prev - (char *) page));
	*head_prev = page;

	return page;
}
#define NTS_PREPEND_1(head, page) (nts_prepend_to_dlist((head), (page), &((page)->header.prev_1), &((page)->header.next_1)))

/* Append page after curr */
static void nts_append_to_dlist(
		NTupleStorePage *curr,
		NTupleStorePage *page,
		void **prev, void **next)
{
	*prev = curr;

	if(!curr)
		*next = NULL;
	else
	{
		void **curr_next = (void **) ((char *) curr + ((char *) next - (char *) page));
		NTupleStorePage *nextpage = (NTupleStorePage *) (*curr_next);

		*curr_next = page;
		*next = nextpage;

		if(nextpage)
		{
			void **next_prev = (void **) ((char *) nextpage + ((char *) prev - (char *) page));
			Assert( *next_prev == curr );
			*next_prev = page;
		}
	}
}
#define NTS_APPEND_1(curr, page) (nts_append_to_dlist((curr), (page), &((page)->header.prev_1), &((page)->header.next_1)))

static void nts_remove_page_from_dlist(NTupleStorePage *page, void **prev, void **next)
{
	void ** prev_next = (void **) ((char *) (*prev) + ((char *) next - (char *) page));
	void ** next_prev = (void **) ((char *) (*next) + ((char *) prev - (char *) page)); 

	if(*prev)
		*prev_next = *next;
	if(*next)
		*next_prev = *prev;
}
#define NTS_REMOVE_1(page) (nts_remove_page_from_dlist((page), &((page)->header.prev_1), &((page)->header.next_1)))

static bool ntsReadBlock(NTupleStore *ts, int blockn, NTupleStorePage *page)
{
	long diskblockn = blockn - ts->first_ondisk_blockn;

	Assert(ts->first_ondisk_blockn >= 0);
	Assert(ts && diskblockn >= 0 && page);
	if(BufFileSeekBlock(ts->pfile, diskblockn) != 0 ||
			BufFileRead(ts->pfile, page, BLCKSZ) != BLCKSZ)
		return false;

	Assert(nts_page_blockn(page) == blockn); 
	Assert(!nts_page_is_dirty(page)); 

	nts_page_set_pin_cnt(page, 0);
	nts_page_set_prev(page, NULL);
	nts_page_set_next(page, NULL);

	return true;
}

static bool ntsWriteBlock(NTupleStore *ts, NTupleStorePage *page)
{
	Assert(ts->rwflag != NTS_IS_READER);
	Assert(nts_page_blockn(page) >= 0); 

	if(ts->first_ondisk_blockn == -1)
	{
		AssertImply(ts->rwflag == NTS_IS_WRITER, nts_page_blockn(page) == 0); 
		ts->first_ondisk_blockn = nts_page_blockn(page);
	}

	Assert(nts_page_blockn(page) >= ts->first_ondisk_blockn);
	Assert(nts_page_is_dirty(page));

	nts_page_set_dirty(page, false);

	if(BufFileSeekBlock(ts->pfile, nts_page_blockn(page) - ts->first_ondisk_blockn) != 0 || 
			BufFileWrite(ts->pfile, page, BLCKSZ) != BLCKSZ)
		return false;
	
	return true;
}

/* Put a page onto the free list.  Do not increase nts->page_cnt */
static void nts_return_free_page(NTupleStore *nts, NTupleStorePage *page)
{
	nts->first_free_page = NTS_PREPEND_1(nts->first_free_page, page);
}

/* Get a free page.  Will shrink if necessary.
 * The page returned still belong to the store (accounted by nts->page_cnt), but it is
 * not on any list.  Caller is responsible to put it back onto a list.
 *
 * Page is allocated/freed with gcc malloc/free, not palloc/pfree.
 */
static NTupleStorePage *nts_get_free_page(NTupleStore *nts)
{
	NTupleStorePage *page = NULL;
	NTupleStorePage *page_next = NULL;
	int page_max = nts->page_max;

	/* if have free page, just use it */
	if(nts->first_free_page)
	{
		page = nts->first_free_page;
		nts->first_free_page = nts_page_next(page); 

		init_page(page);
		return page;
	}

	/* use 1/4 of max allowed if we are working with a disk file */
	if(nts->pfile)
		page_max >>= 2;

	if(nts->page_cnt >= page_max)
	{
		if(!nts->pfile)
			nts->pfile = BufFileCreateTemp(false);

		page = nts->first_page;

		while(page) 
		{
			page_next = nts_page_next(page); 

			if(page_next && nts_page_pin_cnt(page_next) == 0)
			{
				NTS_REMOVE_1(page_next);

				if(nts_page_is_dirty(page_next))
				{
					Assert(nts->rwflag != NTS_IS_READER);
					if(!ntsWriteBlock(nts, page_next))
						elog(ERROR, "NTupleStore shrink write page failed");
				}

				if(nts->page_cnt >= page_max)
				{
					free(page_next);
					--nts->page_cnt;
				}
				else
					break;
			}
			else
				page = page_next;
		}

		if(page_next)
		{
			init_page(page_next);
			return page_next;
		}

		/* failed to shrink */
		if(nts->page_cnt >= page_max)
			return NULL;
	}

	Assert(page_next == NULL && nts->page_cnt < page_max);
	page = (NTupleStorePage *) malloc(sizeof(NTupleStorePage));
	init_page(page);
	++nts->page_cnt;

	return page;
}

/* Find a page in the store with blockn, return NULL if not found.
 * It will load the page from disk if necessary.  The page returned 
 * is not pin-ed.
 */
static NTupleStorePage *nts_load_page(NTupleStore* store, int blockn)
{
	NTupleStorePage *page = NULL;
	NTupleStorePage *page_next = NULL;
	bool readOK;

	/* fast track.  Try last page */
	if(store->last_page && nts_page_blockn(store->last_page) == blockn)
		return store->last_page;

	/* try the in mem list */
	page = store->first_page;
	while(page)
	{
		if(nts_page_blockn(page) == blockn)
			return page;

		page_next = nts_page_next(page); 
		if(page_next == NULL || nts_page_blockn(page_next) > blockn) 
			break;
		
		page = page_next;
	}
	
	Assert(page);

	/* not found.  Need to load from disk */
	page_next = nts_get_free_page(store);
	if(!page_next)
		return NULL;

	readOK = ntsReadBlock(store, blockn, page_next);
	if(!readOK)
	{
		nts_return_free_page(store, page_next);
		return NULL;
	}

	NTS_APPEND_1(page, page_next);
	return page_next;
}

/* Find the next page.  This function is likely to be faster than 
 * 	nts_load_page(nts_page_blockn(page) + 1);
 */
static NTupleStorePage *nts_load_next_page(NTupleStore* store, NTupleStorePage *page)
{
	int blockn = nts_page_blockn(page) + 1;
	bool fOK; 
	NTupleStorePage *next = nts_page_next(page);

	AssertImply(!next, store->rwflag == NTS_IS_READER);

	if(!next || nts_page_blockn(next) > blockn)
	{
		next = nts_get_free_page(store);
		fOK = ntsReadBlock(store, blockn, next);
		if(!fOK)
		{
			Assert(store->rwflag == NTS_IS_READER);
			nts_return_free_page(store, next);
			return NULL;
		}

		NTS_APPEND_1(page, next);
	}

	Assert(next && nts_page_blockn(next) == blockn);
	return next;
}

/* Find prev page.  Likely to be faster than nts_load_page(nts_page_blockn(page) - 1) */
static NTupleStorePage *nts_load_prev_page(NTupleStore *store, NTupleStorePage *page)
{
	int blockn = nts_page_blockn(page) - 1;
	bool fOK;
	NTupleStorePage *prev = NULL;

	if(blockn < nts_page_blockn(store->first_page))
		return NULL;

	prev = nts_page_prev(page);
	Assert(prev && nts_page_blockn(prev) <= blockn);

	if(nts_page_blockn(prev) == blockn)
		return prev;
	else
	{
		NTupleStorePage *p = nts_get_free_page(store);
		if(!p)
			return NULL;

		fOK = ntsReadBlock(store, blockn, p);
		Assert(fOK);

		NTS_APPEND_1(prev, p);

		return p;
	}

	return NULL;
}

extern NTupleStore *ntuplestore_create(int maxBytes)
{
	NTupleStore *store = (NTupleStore *) palloc(sizeof(NTupleStore));

#ifdef DEBUG
	verify_ts_page_sane();
#endif
	store->pfile = NULL;
	store->first_ondisk_blockn = -1;

	Assert(maxBytes >= 0);
	store->page_max = maxBytes / BLCKSZ;
	/* give me at least 16 pages */
	if(store->page_max < 16)
		store->page_max = 16;

	store->first_page = (NTupleStorePage *) palloc(sizeof(NTupleStorePage));
	init_page(store->first_page);

	store->page_cnt = 1;
	store->pin_cnt = 0;

	store->last_page = store->first_page;
	nts_pin_page(store, store->first_page);
	nts_pin_page(store, store->last_page);

	store->first_free_page = NULL;

	store->rwflag = NTS_NOT_READERWRITER;
	store->racc_cnt = 0;
	store->wacc_cnt = 0;

	store->instrument = NULL;
	return store;
}

extern NTupleStore *ntuplestore_create_readerwriter(const char *filename, int maxBytes, bool isWriter)
{
	NTupleStore* store = NULL;
	bool fOK; 
	
	if(isWriter)
	{
		store = ntuplestore_create(maxBytes);
		store->pfile = BufFileCreateTemp_ReaderWriter(filename, isWriter);
		store->rwflag = NTS_IS_WRITER;
	}
	else
	{
		store = (NTupleStore *) palloc(sizeof(NTupleStore));
		store->pfile = BufFileCreateTemp_ReaderWriter(filename, isWriter);
		store->first_ondisk_blockn = 0;
		store->rwflag = NTS_IS_READER;
	
		Assert(maxBytes >= 0);
		store->page_max = maxBytes / BLCKSZ;
		/* give me at least 16 pages */
		if(store->page_max < 16)
			store->page_max = 16;

		fOK = ntsReadBlock(store, 0, store->first_page);
		Assert(fOK);

		store->page_cnt = 1;
		store->pin_cnt = 0;

		nts_pin_page(store, store->first_page);
		
		store->last_page = NULL;
		store->first_free_page = NULL;

		store->racc_cnt = 0;
		store->wacc_cnt = 0;

		store->instrument = NULL;
	}
	return store;
}
		

extern void ntuplestore_reset(NTupleStore *ts)
{
	NTupleStorePage *p = ts->first_page;

	Assert(ts->wacc_cnt == 0 && ts->racc_cnt == 0);
	Assert(ts->rwflag == NTS_NOT_READERWRITER || !"Reset NYI for reader writer");

	while(p)
	{
		NTupleStorePage *next = nts_page_next(p); 
		ts->first_free_page = NTS_PREPEND_1(ts->first_free_page, p);
		p = next;
	}
	ts->pin_cnt = 0;

	Assert(ts->first_free_page != NULL);

	ts->first_page = ts->first_free_page;
	ts->first_free_page = nts_page_next(ts->first_page);
	init_page(ts->first_page);

	ts->last_page = ts->first_page;
	nts_pin_page(ts, ts->first_page);
	nts_pin_page(ts, ts->last_page);
}

extern void ntuplestore_flush(NTupleStore *ts)
{
	NTupleStorePage *p = ts->first_page;

	Assert(ts->rwflag == NTS_IS_WRITER || !"Flush NYI for Non ReaderWriter or Reader");
	
#ifdef DEBUG
	verify_ts_page_list(ts);
#endif

	Assert(ts->pfile);

	while(p)
	{
		if(nts_page_is_dirty(p) && nts_page_slot_cnt(p) > 0)
			ntsWriteBlock(ts, p);
		p = nts_page_next(p);
	}
	
	BufFileFlush(ts->pfile);
}

extern void ntuplestore_destroy(NTupleStore *ts)
{
	NTupleStorePage *p = ts->first_page;

#ifdef DEBUG
	verify_ts_page_list(ts);

	Assert(ts->racc_cnt == 0 || !"NTupleStore leaking a read accessor");
	Assert(ts->wacc_cnt == 0 || !"NTupleStore leaking a write accessor");
#endif

	while(p)
	{
		NTupleStorePage *pnext = nts_page_next(p); 
		free(p);
		p = pnext;
	}

	p = ts->first_free_page;
	while(p)
	{
		NTupleStorePage *pnext = nts_page_next(p); 
		free(p);
		p = pnext;
	}

	if(ts->pfile && ts->rwflag == NTS_NOT_READERWRITER) 
		BufFileClose(ts->pfile);

	pfree(ts);
}

extern NTupleStoreAccessor* ntuplestore_create_accessor(NTupleStore *ts, bool isWriter)
{
	NTupleStoreAccessor *acc = (NTupleStoreAccessor *) palloc(sizeof(NTupleStoreAccessor));

	acc->store = ts;
	acc->isWriter = isWriter;
	acc->page = NULL;
	acc->pos.blockn = -1;
	acc->pos.slotn = -1;

	ntuplestore_acc_seek_first(acc);

	if(isWriter) 
	{
		Assert(ts->rwflag != NTS_IS_READER);
		Assert(ts->wacc_cnt == 0);
		++ts->wacc_cnt;
	}
	else
		++ts->racc_cnt;

	return acc;
}
	
void ntuplestore_destroy_accessor(NTupleStoreAccessor *acc)
{
	Assert(acc->store);

	if(acc->isWriter)
	{
		Assert(acc->store->wacc_cnt == 1);
		--acc->store->wacc_cnt;
	}
	else
	{
		Assert(acc->store->racc_cnt > 0);
		--acc->store->racc_cnt;
	}

	if(acc->page)
		nts_unpin_page(acc->store, acc->page);

	pfree(acc);
}

void ntuplestore_acc_put_tupleslot(NTupleStoreAccessor *tsa, TupleTableSlot *slot)
{
	char *dest_ptr;
	unsigned int  dest_len;
	bool put_ok;

	
	Assert(tsa->page && tsa->isWriter);
	Assert(tsa->store && tsa->store->first_page && tsa->store->last_page);

	/* first move the page to last page */
	if(tsa->page != tsa->store->last_page)
	{
		nts_unpin_page(tsa->store, tsa->page);
		tsa->page = tsa->store->last_page;
		nts_pin_page(tsa->store, tsa->page);
	}

	dest_ptr = nts_page_next_slot_data_ptr(tsa->page); 
	dest_len = (unsigned int) nts_page_avail_bytes(tsa->page); 

	Assert(dest_len >= 0 && dest_len <= NTS_MAX_ENTRY_SIZE);
	put_ok = ExecCopySlotMinimalTupleTo(slot, dest_ptr, &dest_len);

	if(put_ok)
		update_page_slot_entry(tsa->page, (int) dest_len);
	else if(dest_len > NTS_MAX_ENTRY_SIZE)
		elog(ERROR, "NTuplestore cannot store slot: len is %d", dest_len);
	else
	{
		NTupleStorePage *page = nts_get_free_page(tsa->store);
		if(!page)
			elog(ERROR, "NTuplestore out of page");

		Assert(nts_page_slot_cnt(page) == 0 && nts_page_data_bcnt(page) == 0); 

		dest_ptr = nts_page_next_slot_data_ptr(page);
		dest_len = (unsigned int) nts_page_avail_bytes(page);
		put_ok = ExecCopySlotMinimalTupleTo(slot, dest_ptr, &dest_len);
		Assert(put_ok);

		update_page_slot_entry(page, (int) dest_len);
		nts_page_set_blockn(page, nts_page_blockn(tsa->page) + 1); 
		NTS_APPEND_1(tsa->page, page);

		nts_unpin_page(tsa->store, tsa->page);
		nts_pin_page(tsa->store, page);
		tsa->page = page;

		nts_unpin_page(tsa->store, tsa->store->last_page);
		tsa->store->last_page = page;
		nts_pin_page(tsa->store, tsa->store->last_page);
	}

	tsa->pos.blockn = nts_page_blockn(tsa->page); 
	tsa->pos.slotn = nts_page_slot_cnt(tsa->page) - 1; 
}

void ntuplestore_acc_put_data(NTupleStoreAccessor *tsa, void *data, int len)
{
	char *dest_ptr;
	int   dest_len;

	Assert(tsa->page && tsa->isWriter);
	Assert(tsa->store && tsa->store->first_page && tsa->store->last_page);

	if(len > NTS_MAX_ENTRY_SIZE)
		elog(ERROR, "NTupleStore cannot store data: len is %d", len);

	/* first move the page to last page */
	if(tsa->page != tsa->store->last_page)
	{
		nts_unpin_page(tsa->store, tsa->page);
		tsa->page = tsa->store->last_page;
		nts_pin_page(tsa->store, tsa->page);
	}

	dest_ptr = nts_page_next_slot_data_ptr(tsa->page); 
	dest_len = nts_page_avail_bytes(tsa->page); 

	if(dest_len >= len)
	{
		memcpy(dest_ptr, data, len);
		update_page_slot_entry(tsa->page, len);
	}
	else
	{
		NTupleStorePage *page = nts_get_free_page(tsa->store);
		if(!page)
			elog(ERROR, "NTuplestore out of page");

		Assert(nts_page_slot_cnt(page) == 0 && nts_page_data_bcnt(page) == 0); 

		dest_ptr = nts_page_next_slot_data_ptr(page); 
		memcpy(dest_ptr, data, len);

		update_page_slot_entry(page, len);
		nts_page_set_blockn(page, nts_page_blockn(tsa->page) + 1);
		NTS_APPEND_1(tsa->page, page);

		nts_unpin_page(tsa->store, tsa->page);
		nts_pin_page(tsa->store, page);
		tsa->page = page;

		nts_unpin_page(tsa->store, tsa->store->last_page);
		tsa->store->last_page = page;
		nts_pin_page(tsa->store, tsa->store->last_page);
	}

	tsa->pos.blockn = nts_page_blockn(tsa->page);
	tsa->pos.slotn = nts_page_slot_cnt(tsa->page) - 1; 
}

static void trim_page_to_slotn(NTupleStorePage *page, int slotn)
{

#ifdef DEBUG
	int i; 
	for(i = 0; i<nts_page_first_valid_slotn(page); ++i)
	{
		NTupleStorePageSlotEntry *slot = nts_page_slot_entry(page, i); 
		Assert(slot->data_start == -1 && slot->size == -1);
	}

	for(; i<slotn; ++i)
	{
		NTupleStorePageSlotEntry *slot = nts_page_slot_entry(page, i); 
		slot->data_start = -1;
		slot->size = -1;
	}
#endif
	
	nts_page_set_first_valid_slotn(page, slotn);
}

/* XXX Not done yet.
 * Postgres does not allow hole in the BufFile, therefore, even if we are trimming
 * a page, if, we are already in disk mode, we will have to flush the page (or write
 * any junk, but we need to write a page).  A better way is to may logical page blockn
 * to a physical blockn.  
 */
void ntuplestore_acc_trim(NTupleStoreAccessor *tsa, NTupleStorePos *pos)
{
	bool fOK;

	Assert(tsa->page && tsa->isWriter);
	Assert(tsa->store && tsa->store->first_page && tsa->store->last_page);
	Assert(pos->blockn >= nts_page_blockn(tsa->store->first_page));
	Assert(pos->blockn <= nts_page_blockn(tsa->store->last_page)); 

	/* fisrt seek to the page */
	fOK = ntuplestore_acc_seek(tsa, pos);
	Assert(fOK);

	trim_page_to_slotn(tsa->page, pos->slotn);

	if(tsa->store->first_page == tsa->page)
		return;

	nts_unpin_page(tsa->store, tsa->store->first_page);

	while(tsa->store->first_page != tsa->page)
	{
		NTupleStorePage *next = nts_page_next(tsa->store->first_page);

		if(nts_page_is_dirty(tsa->store->first_page))
		{
			if(tsa->store->pfile)
				ntsWriteBlock(tsa->store, tsa->store->first_page);
		}

		tsa->store->first_free_page = NTS_PREPEND_1(tsa->store->first_free_page, tsa->store->first_page); 
		tsa->store->first_page = next;
	}

	nts_pin_page(tsa->store, tsa->store->first_page);
	return;
}

static void ntuplestore_acc_advance_in_page(NTupleStoreAccessor *tsa, int* pn)
{
	if(*pn == 0)
		return ;

	if(*pn > 0) /* seeking forward */
	{
		Assert(tsa->pos.slotn >= nts_page_first_valid_slotn(tsa->page)); 

		if((tsa->pos.slotn + *pn) < nts_page_slot_cnt(tsa->page))
		{
			tsa->pos.slotn += *pn;
			*pn = 0;
		}
		else
			*pn -= nts_page_slot_cnt(tsa->page) - tsa->pos.slotn;
	}
	else /* seeking backward */
	{
		if(tsa->pos.slotn + *pn >= nts_page_first_valid_slotn(tsa->page))
		{
			tsa->pos.slotn += *pn;
			*pn = 0;
		}
		else
			*pn += nts_page_valid_slot_cnt(tsa->page);
	}
}

bool ntuplestore_acc_advance(NTupleStoreAccessor *tsa, int n)
{
	if(!tsa->page)
		return false;

	ntuplestore_acc_advance_in_page(tsa, &n);

	while(n != 0)
	{
		if(n > 0) /* seek forward */
		{
			nts_unpin_page(tsa->store, tsa->page);
			tsa->page = nts_load_next_page(tsa->store, tsa->page);
			if(!tsa->page)
			{
				tsa->pos.blockn = -1;
				tsa->pos.slotn = -1;
				return false;
			}

			Assert(nts_page_blockn(tsa->page) == tsa->pos.blockn + 1);
			Assert(nts_page_valid_slot_cnt(tsa->page) > 0);

			nts_pin_page(tsa->store, tsa->page);
			tsa->pos.blockn = nts_page_blockn(tsa->page);
			tsa->pos.slotn = 0;
		}
		else /* seeking backward */
		{
			nts_unpin_page(tsa->store, tsa->page);
			tsa->page = nts_load_prev_page(tsa->store, tsa->page);
			if(!tsa->page)
			{
				tsa->pos.blockn = -1;
				tsa->pos.slotn = -1;
				return false;
			}

			Assert(nts_page_blockn(tsa->page) == tsa->pos.blockn - 1);
			Assert(nts_page_valid_slot_cnt(tsa->page) > 0);

			nts_pin_page(tsa->store, tsa->page);
			tsa->pos.blockn = nts_page_blockn(tsa->page);
			tsa->pos.slotn = nts_page_slot_cnt(tsa->page) - 1;
		}

		ntuplestore_acc_advance_in_page(tsa, &n);
	}

	return true;
}

bool ntuplestore_acc_current_tupleslot(NTupleStoreAccessor *tsa, TupleTableSlot *slot)
{
	MinimalTuple tuple = NULL;
	int len = 0;
	bool fOK = ntuplestore_acc_current_data(tsa, (void **) &tuple, &len);

	if(!fOK)
	{
		ExecClearTuple(slot);
		return false;
	}

	Assert(tuple && len > 0);

	ExecStoreMinimalTuple(tuple, slot, false);
	return true;
}

bool ntuplestore_acc_current_data(NTupleStoreAccessor *tsa, void **data, int *len)
{
	if(!tsa->page || tsa->pos.slotn == -1)
		return false;

	*data = (void *) nts_page_slot_data_ptr(tsa->page, tsa->pos.slotn); 
	*len = nts_page_slot_data_len(tsa->page, tsa->pos.slotn); 

	return true;
}

bool ntuplestore_acc_tell(NTupleStoreAccessor *tsa, NTupleStorePos *pos)
{
	if(!tsa->page || tsa->pos.slotn == -1)
		return false;

	if(pos)
	{
		pos->blockn = tsa->pos.blockn;
		pos->slotn = tsa->pos.slotn;
	}

	return true;
}

bool ntuplestore_acc_seek(NTupleStoreAccessor *tsa, NTupleStorePos *pos)
{
	Assert(tsa && pos);
	if(pos->slotn < 0)
	{
		Assert(pos->slotn == -1);
		return false;
	}

	/* first seek page */
	if(!tsa->page || nts_page_blockn(tsa->page) != pos->blockn)
	{
		if(tsa->page)
			nts_unpin_page(tsa->store, tsa->page);

		tsa->page = nts_load_page(tsa->store, pos->blockn);
		if(!tsa->page)
			return false;

		nts_pin_page(tsa->store, tsa->page);
	}

	Assert(tsa->page && nts_page_blockn(tsa->page) == pos->blockn);
	Assert(nts_page_slot_cnt(tsa->page) > pos->slotn && nts_page_first_valid_slotn(tsa->page) <= pos->slotn);

	tsa->pos.blockn = pos->blockn;
	tsa->pos.slotn = pos->slotn;

	return true;
}

bool ntuplestore_acc_seek_first(NTupleStoreAccessor *tsa)
{
	Assert(tsa->store && tsa->store->first_page);

	if(tsa->page)
		nts_unpin_page(tsa->store, tsa->page);

	tsa->page = tsa->store->first_page;
	nts_pin_page(tsa->store, tsa->page);

	tsa->pos.blockn = nts_page_blockn(tsa->page);
	tsa->pos.slotn = nts_page_first_valid_slotn(tsa->page); 

	if(nts_page_valid_slot_cnt(tsa->page) == 0)
	{
		tsa->pos.slotn = -1;
		return false;
	}

	return true;
}

bool ntuplestore_acc_seek_last(NTupleStoreAccessor *tsa)
{
	Assert(tsa && tsa->store && tsa->store->last_page);

	if(tsa->page)
		nts_unpin_page(tsa->store, tsa->page);

	tsa->page = tsa->store->last_page;
	nts_pin_page(tsa->store, tsa->page);

	tsa->pos.blockn = nts_page_blockn(tsa->page);
	tsa->pos.slotn = nts_page_slot_cnt(tsa->page);

	return ntuplestore_acc_advance(tsa, -1);
}
