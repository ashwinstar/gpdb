/*-------------------------------------------------------------------------
 *
 * tuplesort.c
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of heap tuples, index tuples, or single
 * Datums (and could easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().	Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * See Knuth, volume 3, for more than you want to know about the external
 * sorting algorithm.  We divide the input into sorted runs using replacement
 * selection, in the form of a priority tree implemented as a heap
 * (essentially his Algorithm 5.2.3H), then merge the runs using polyphase
 * merge, Knuth's Algorithm 5.4.2D.  The logical "tapes" used by Algorithm D
 * are implemented by logtape.c, which avoids space wastage by recycling
 * disk space as soon as each block is read from its "tape".
 *
 * We do not form the initial runs using Knuth's recommended replacement
 * selection data structure (Algorithm 5.4.1R), because it uses a fixed
 * number of records in memory at all times.  Since we are dealing with
 * tuples that may vary considerably in size, we want to be able to vary
 * the number of records kept in memory to ensure full utilization of the
 * allowed sort memory space.  So, we keep the tuples in a variable-size
 * heap, with the next record to go out at the top of the heap.  Like
 * Algorithm 5.4.1R, each record is stored with the run number that it
 * must go into, and we use (run number, key) as the ordering key for the
 * heap.  When the run number at the top of the heap changes, we know that
 * no more records of the prior run are left in the heap.
 *
 * The approximate amount of memory allowed for any one sort operation
 * is specified in kilobytes by the caller (most pass work_mem).  Initially,
 * we absorb tuples and simply store them in an unsorted array as long as
 * we haven't exceeded workMem.  If we reach the end of the input without
 * exceeding workMem, we sort the array using qsort() and subsequently return
 * tuples just by scanning the tuple array sequentially.  If we do exceed
 * workMem, we construct a heap using Algorithm H and begin to emit tuples
 * into sorted runs in temporary tapes, emitting just enough tuples at each
 * step to get back within the workMem limit.  Whenever the run number at
 * the top of the heap changes, we begin a new run with a new output tape
 * (selected per Algorithm D).	After the end of the input is reached,
 * we dump out remaining tuples in memory into a final run (or two),
 * then merge the runs using Algorithm D.
 *
 * When merging runs, we use a heap containing just the frontmost tuple from
 * each source run; we repeatedly output the smallest tuple and insert the
 * next tuple from its source tape (if any).  When the heap empties, the merge
 * is complete.  The basic merge algorithm thus needs very little memory ---
 * only M tuples for an M-way merge, and M is constrained to a small number.
 * However, we can still make good use of our full workMem allocation by
 * pre-reading additional tuples from each source tape.  Without prereading,
 * our access pattern to the temporary file would be very erratic; on average
 * we'd read one block from each of M source tapes during the same time that
 * we're writing M blocks to the output tape, so there is no sequentiality of
 * access at all, defeating the read-ahead methods used by most Unix kernels.
 * Worse, the output tape gets written into a very random sequence of blocks
 * of the temp file, ensuring that things will be even worse when it comes
 * time to read that tape.	A straightforward merge pass thus ends up doing a
 * lot of waiting for disk seeks.  We can improve matters by prereading from
 * each source tape sequentially, loading about workMem/M bytes from each tape
 * in turn.  Then we run the merge algorithm, writing but not reading until
 * one of the preloaded tuple series runs out.	Then we switch back to preread
 * mode, fill memory again, and repeat.  This approach helps to localize both
 * read and write accesses.
 *
 * When the caller requests random access to the sort result, we form
 * the final sorted run on a logical tape which is then "frozen", so
 * that we can access it randomly.	When the caller does not need random
 * access, we return from tuplesort_performsort() as soon as we are down
* to one run per logical tape.  The final merge is then performed
* on-the-fly as the caller repeatedly calls tuplesort_getXXX; this
* saves one cycle of writing all the data out to disk and reading it in.
*
* Before Postgres 8.2, we always used a seven-tape polyphase merge, on the
* grounds that 7 is the "sweet spot" on the tapes-to-passes curve according
* to Knuth's figure 70 (section 5.4.2).  However, Knuth is assuming that
* tape drives are expensive beasts, and in particular that there will always
* be many more runs than tape drives.	In our implementation a "tape drive"
* doesn't cost much more than a few Kb of memory buffers, so we can afford
* to have lots of them.  In particular, if we can have as many tape drives
* as sorted runs, we can eliminate any repeated I/O at all.  In the current
* code we determine the number of tapes M on the basis of workMem: we want
* workMem/M to be large enough that we read a fair amount of data each time
* we preread from a tape, so as to maintain the locality of access described
* above.  Nonetheless, with large workMem we can have many tapes.
*
*
* Portions Copyright (c) 2007-2008, Greenplum inc
* Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
*
* IDENTIFICATION
*	  $PostgreSQL: pgsql/src/backend/utils/sort/tuplesort.c,v 1.70 2006/10/04 00:30:04 momjian Exp $
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/tuptoaster.h"
#include "catalog/pg_type.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_operator.h"
#include "executor/instrument.h"        /* Instrumentation */
#include "lib/stringinfo.h"             /* StringInfo */
#include "executor/nodeSort.h" 		/* gpmon */
#include "miscadmin.h"
#include "utils/datum.h"
#include "storage/buffile.h"
#include "utils/logtape.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/pg_locale.h"
#include "utils/builtins.h"

#include "tuplesort_mk.h"
#include "cdb/cdbvars.h"

/*
 * Possible states of a Tuplesort object.  These denote the states that
 * persist between calls of Tuplesort routines.
 */
typedef enum
{
    TSS_INITIAL,				/* Loading tuples; still within memory limit */
    TSS_BUILDRUNS,				/* Loading tuples; writing to tape */
    TSS_SORTEDINMEM,			/* Sort completed entirely in memory */
    TSS_SORTEDONTAPE,			/* Sort completed, final run is on tape */
    TSS_FINALMERGE				/* Performing final merge on-the-fly */
} TupSortStatus;

/*
 * Parameters for calculation of number of tapes to use --- see inittapes()
 * and tuplesort_merge_order().
 *
 * In this calculation we assume that each tape will cost us about 3 blocks
 * worth of buffer space (which is an underestimate for very large data
 * volumes, but it's probably close enough --- see logtape.c).
 *
 * MERGE_BUFFER_SIZE is how much data we'd like to read from each input
 * tape during a preread cycle (see discussion at top of file).
 */
#define MINORDER		6		/* minimum merge order */
#define MAXORDER 		250 	/* maximum merge order */
#define TAPE_BUFFER_OVERHEAD		(BLCKSZ * 3)
#define MERGE_BUFFER_SIZE			(BLCKSZ * 32)

/* 
 * Current postion of Tuplesort operation.
 */
struct TuplesortPos_mk
{
    /*
     * These variables are used after completion of sorting to keep track of
     * the next tuple to return.  (In the tape case, the tape's current read
     * position is also critical state.)
     */
    int		current;	/* array index (only used if SORTEDINMEM) */
    bool		eof_reached;	/* reached EOF (needed for cursors) */

    /* markpos_xxx holds marked position for mark and restore */
    union {
        LogicalTapePos tapepos;
        long 		   mempos;
    } markpos;
    bool markpos_eof; 

    LogicalTape* 		cur_work_tape;      /* current tape that I am working on */
};

/* Sort prepare/compare context info */
typedef struct TupsortMKContext
{
    /* Common part */
    MKContext_LvCommon mk_common;

    /* For comp */
    SortFunctionKind sortfnkind; 
    bool  nullfirst;

    int16 attno;
    int16 sknum;

    /* For prep */
    Tuplesortstate_mk *tsstate;
} TupsortMKContext;

/* Merge reader (read back from runs) context for mk_heap */
typedef struct TupsortMergeReadCtxt
{
    Tuplesortstate_mk *tsstate;
    TuplesortPos_mk pos;

    /* Buffer for preread */
    MKEntry *p;
    int allocsz;
    int cnt;
    int cur;

    int mem_allowed;
    int mem_used;
} TupsortMergeReadCtxt;


/*
 * Private state of a Tuplesort operation.
 */
struct Tuplesortstate_mk
{
    TupSortStatus 	status;		/* enumerated value as shown above */
    int		nKeys;		/* number of columns in sort key */
    bool		randomAccess;	/* did caller request random access? */

    long 		memAllowed;
    long 		memUsed;

    int		maxTapes;	/* number of tapes (Knuth's T) */
    int		tapeRange;	/* maxTapes-1 (Knuth's P) */
    MemoryContext 	sortcontext;	/* memory context holding all sort data */
    LogicalTapeSet 	*tapeset;	/* logtape.c object for tapes in a temp file */

    /*
     * Function to copy a supplied input tuple into palloc'd space and set up
     * its SortTuple representation (ie, set tuple/datum1/isnull1).  Also,
     * state->availMem must be decreased by the amount of space used for the
     * tuple copy (note the SortTuple struct itself is not counted).
     */
    void (*copytup) (Tuplesortstate_mk *state, MKEntry *e, void *tup);

    /*
     * Function to write a stored tuple onto tape.	The representation of the
     * tuple on tape need not be the same as it is in memory; requirements on
     * the tape representation are given below.  After writing the tuple,
     * pfree() the out-of-line data (not the SortTuple struct!), and increase
     * state->availMem by the amount of memory space thereby released.
     */
    void (*writetup) (Tuplesortstate_mk *state, LogicalTape *lt, MKEntry *e);

    /*
     * Function to read a stored tuple from tape back into memory. 'len' is
     * the already-read length of the stored tuple.  Create a palloc'd copy,
     * initialize tuple/datum1/isnull1 in the target SortTuple struct, and
     * decrease state->availMem by the amount of memory space consumed.
     */
    void (*readtup) (Tuplesortstate_mk *state, TuplesortPos_mk *pos, MKEntry *e, 
            LogicalTape *lt, uint32 len);

    /*
     * This array holds the tuples now in sort memory.	If we are in state
     * INITIAL, the tuples are in no particular order; if we are in state
     * SORTEDINMEM, the tuples are in final sorted order; 
     */
    MKEntry *entries;
    long entry_allocsize;
    long entry_count;

    /* 
     * BUILDRUNS and FINALMERGE: Heap for producing and merging runs
     */
    MKHeap  *mkheap;
    MKHeapReader *mkhreader;
    TupsortMergeReadCtxt *mkhreader_ctxt;
    int mkhreader_allocsize;

    /*
     * MK context, holds info needed by compare/prepare functions 
     */
    MKContext mkctxt;

    int currentRun;

    /*
     * Unless otherwise noted, all pointer variables below are pointers to
     * arrays of length maxTapes, holding per-tape data.
     */

    /*
     * These variables are only used during merge passes.  mergeactive[i] is
     * true if we are reading an input run from (actual) tape number i and
     * have not yet exhausted that run.  mergenext[i] is the memtuples index
     * of the next pre-read tuple (next to be loaded into the heap) for tape
     * i, or 0 if we are out of pre-read tuples.  mergelast[i] similarly
     * points to the last pre-read tuple from each tape.  mergeavailslots[i]
     * is the number of unused memtuples[] slots reserved for tape i, and
     * mergeavailmem[i] is the amount of unused space allocated for tape i.
     * mergefreelist and mergefirstfree keep track of unused locations in the
     * memtuples[] array.  The memtuples[].tupindex fields link together
     * pre-read tuples for each tape as well as recycled locations in
     * mergefreelist. It is OK to use 0 as a null link in these lists, because
     * memtuples[0] is part of the merge heap and is never a pre-read tuple.
     */
    bool 	*mergeactive;		/* active input run source? */
    int 	*mergenext;		/* first preread tuple for each source */
    int 	*mergelast;		/* last preread tuple for each source */
    int 	*mergeavailslots;	/* slots left for prereading each tape */
    long 	*mergeavailmem;		/* availMem for prereading each tape */
    int 	mergefreelist;		/* head of freelist of recycled slots */
    int	mergefirstfree; 	/* first slot never used in this merge */

    /*
     * Variables for Algorithm D.  Note that destTape is a "logical" tape
     * number, ie, an index into the tp_xxx[] arrays.  Be careful to keep
     * "logical" and "actual" tape numbers straight!
     */
    int	Level;			/* Knuth's l */
    int	destTape;		/* current output tape (Knuth's j, less 1) */
    int   	*tp_fib;		/* Target Fibonacci run counts (A[]) */
    int   	*tp_runs;		/* # of real runs on each tape */
    int   	*tp_dummy;		/* # of dummy runs for each tape (D[]) */
    int   	*tp_tapenum;		/* Actual tape numbers (TAPE[]) */
    int	activeTapes;		/* # of active input tapes in merge pass */

    LogicalTape  *result_tape;	/* actual tape of finished output */
    TuplesortPos_mk pos; 		/* current postion */

    /*
     * Tuple desc and binding, for MemTuple.
     */
    TupleDesc	tupDesc;
    MemTupleBinding *mt_bind;
    ScanKey		cmpScanKey;

    /*
     * These variables are specific to the IndexTuple case; they are set by
     * tuplesort_begin_index and used only by the IndexTuple routines.
     */
    Relation	indexRel;

    /*
     * These variables are specific to the Datum case; they are set by
     * tuplesort_begin_datum and used only by the DatumTuple routines.
     */
    Oid		datumType;
    Oid		sortOperator;
    bool    nullfirst;

    /* we need typelen and byval in order to know how to copy the Datums. */
    int		datumTypeLen;
    bool		datumTypeByVal;

    /*
     * CDB: EXPLAIN ANALYZE reporting interface and statistics.
     */
    struct Instrumentation *instrument;
    struct StringInfoData  *explainbuf;
    int64       spilledBytes;

    /* 
     * File fo dump/load logical tape set state.  Used by sharing sort across slice 
     */
    char    *pfile_rwfile_prefix;
    BufFile *pfile_rwfile_state;

    /* My debug counters */
    int64 put_cnt;
    int64 get_cnt;

    /* Gpmon */
    gpmon_packet_t *gpmon_pkt;
    int *gpmon_sort_tick;
};

static bool is_sortstate_rwfile(Tuplesortstate_mk *state)
{
    return state->pfile_rwfile_state != NULL;
}
#ifdef USE_ASSERT_CHECKING
static bool is_under_sort_ctxt(Tuplesortstate_mk *state)
{
    return CurrentMemoryContext == state->sortcontext;
}
#endif

#define COPYTUP(state,stup,tup) ((*(state)->copytup) (state, stup, tup))
#define WRITETUP(state,tape,stup)	((*(state)->writetup) (state, tape, stup))
#define READTUP(state,pos,stup,tape,len) ((*(state)->readtup) (state, pos, stup, tape, len))
#define LACKMEM(state)		((state)->memUsed > (state)->memAllowed)
static inline void USEMEM(Tuplesortstate_mk *state, int amt)
{
    state->memUsed += amt;
    if(state->gpmon_pkt)
        Gpmon_M_Add(state->gpmon_pkt, GPMON_SORT_MEMORY_BYTE, amt);
}
static inline void FREEMEM(Tuplesortstate_mk *state, int amt)
{
    state->memUsed -= amt;
    if(state->gpmon_pkt)
        Gpmon_M_Add(state->gpmon_pkt, GPMON_SORT_MEMORY_BYTE, -amt);
}

/*
 * NOTES about on-tape representation of tuples:
 *
 * We require the first "unsigned int" of a stored tuple to be the total size
 * on-tape of the tuple, including itself (so it is never zero; an all-zero
 * unsigned int is used to delimit runs).  The remainder of the stored tuple
 * may or may not match the in-memory representation of the tuple ---
 * any conversion needed is the job of the writetup and readtup routines.
 *
 * If state->randomAccess is true, then the stored representation of the
 * tuple must be followed by another "unsigned int" that is a copy of the
 * length --- so the total tape space used is actually sizeof(unsigned int)
 * more than the stored length value.  This allows read-backwards.	When
 * randomAccess is not true, the write/read routines may omit the extra
 * length word.
 *
 * writetup is expected to write both length words as well as the tuple
 * data.  When readtup is called, the tape is positioned just after the
 * front length word; readtup must read the tuple data and advance past
 * the back length word (if present).
 *
 * The write/read routines can make use of the tuple description data
 * stored in the Tuplesortstate_mk record, if needed.	They are also expected
 * to adjust state->availMem by the amount of memory space (not tape space!)
 * released or consumed.  There is no error return from either writetup
 * or readtup; they should ereport() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space allocated for tuples against the workMem limit, plus
 * the space used by the variable-size memtuples array.  Fixed-size space
 * is not counted; it's small enough to not be interesting.
 *
 * Note that we count actual space used (as shown by GetMemoryChunkSpace)
 * rather than the originally-requested size.  This is important since
 * palloc can add substantial overhead.  It's not a complete answer since
 * we won't count any wasted space in palloc allocation blocks, but it's
 * a lot better than what we were doing before 7.3.
 */

static Tuplesortstate_mk *tuplesort_begin_common(int workMem, bool randomAccess, bool allocmemtuple);
static void puttuple_common(Tuplesortstate_mk *state, MKEntry *e); 
static void selectnewtape_mk(Tuplesortstate_mk *state);
static void mergeruns(Tuplesortstate_mk *state);
static void beginmerge(Tuplesortstate_mk *state);
static uint32 getlen(Tuplesortstate_mk *state, TuplesortPos_mk *pos, LogicalTape *lt, bool eofOK);
static void markrunend(Tuplesortstate_mk *state, int tapenum);

static void copytup_heap(Tuplesortstate_mk *state, MKEntry *e, void *tup);
static void writetup_heap(Tuplesortstate_mk *state, LogicalTape *lt, MKEntry *e); 
static void readtup_heap(Tuplesortstate_mk *state, TuplesortPos_mk *pos, MKEntry *e, 
        LogicalTape *lt, uint32 len);

static void copytup_index(Tuplesortstate_mk *state, MKEntry *e, void *tup);
static void writetup_index(Tuplesortstate_mk *state, LogicalTape *lt, MKEntry *e);
static void readtup_index(Tuplesortstate_mk *state, TuplesortPos_mk *pos, MKEntry *e, 
        LogicalTape *lt, uint32 len);

static void copytup_datum(Tuplesortstate_mk *state, MKEntry *e, void *tup);
static void writetup_datum(Tuplesortstate_mk *state, LogicalTape *lt, MKEntry *e); 
static void readtup_datum(Tuplesortstate_mk *state, TuplesortPos_mk *pos, MKEntry *e,
        LogicalTape *lt, uint32 len);

static int tupsort_compare_datum(MKEntry *v1, MKEntry *v2, void *ctxt);
static int tupsort_compare_char(MKEntry *v1, MKEntry *v2, void *ctxt);
static int tupsort_compare_int32(MKEntry *v1, MKEntry *v2, void *ctxt);

static void tupsort_prepare_mtup(MKEntry *a, void *ctxt);
static void tupsort_prepare_itup(MKEntry *a, void *ctxt);

static void tupsort_prepare_mtup_char(MKEntry *a, void *ctxt);
static void tupsort_prepare_mtup_text(MKEntry *a, void *ctxt);
static void tupsort_prepare_itup_char(MKEntry *a, void *ctxt);
static void tupsort_prepare_itup_text(MKEntry *a, void *ctxt);

static void tuplesort_limit_insert(Tuplesortstate_mk *state, MKEntry *e);
static void tuplesort_heap_insert(Tuplesortstate_mk *state, MKEntry *e);
static void tuplesort_limit_sort(Tuplesortstate_mk *state);

static void tupsort_refcnt(void *vp, int ref); 
static void tupsort_cpfr(MKEntry *dst, MKEntry *src, void *ctxt);

/* Declare the following as extern so that older dtrace will not complain */
extern void inittapes_mk(Tuplesortstate_mk *state, const char* rwfile_prefix);
extern void dumptuples_mk(Tuplesortstate_mk *state, bool alltuples);
extern void mergeonerun_mk(Tuplesortstate_mk *state);

extern void mkheap_verify_heap(MKHeap *heap, int top);

/*
 *		tuplesort_begin_xxx
 *
 * Initialize for a tuple sort operation.
 *
 * After calling tuplesort_begin, the caller should call tuplesort_putXXX
 * zero or more times, then call tuplesort_performsort when all the tuples
 * have been supplied.	After performsort, retrieve the tuples in sorted
 * order by calling tuplesort_getXXX until it returns false/NULL.  (If random
 * access was requested, rescan, markpos, and restorepos can also be called.)
 * Call tuplesort_end to terminate the operation and release memory/disk space.
 *
 * Each variant of tuplesort_begin has a workMem parameter specifying the
 * maximum number of kilobytes of RAM to use before spilling data to disk.
 * (The normal value of this parameter is work_mem, but some callers use
 * other values.)  Each variant also has a randomAccess parameter specifying
 * whether the caller needs non-sequential access to the sort result.
 *
 * CDB: During EXPLAIN ANALYZE, after tuplesort_begin_xxx() the caller should
 * use tuplesort_set_instrument() (q.v.) to enable statistical reporting.
 */

    static Tuplesortstate_mk *
tuplesort_begin_common(int workMem, bool randomAccess, bool allocmemtuple)
{
    Tuplesortstate_mk *state;
    MemoryContext sortcontext;
    MemoryContext oldcontext;

    /*
     * Create a working memory context for this sort operation. All data
     * needed by the sort will live inside this context.
     */
    sortcontext = AllocSetContextCreate(CurrentMemoryContext,
            "TupleSort",
            ALLOCSET_DEFAULT_MINSIZE,
            ALLOCSET_DEFAULT_INITSIZE,
            ALLOCSET_DEFAULT_MAXSIZE);

    /*
     * Make the Tuplesortstate_mk within the per-sort context.  This way, we
     * don't need a separate pfree() operation for it at shutdown.
     */
    oldcontext = MemoryContextSwitchTo(sortcontext);

    /* 
     * palloc0, need to zero out memeories, so we do not need to set
     * every var of the tuplestate to 0 below.
     */
    state = (Tuplesortstate_mk *) palloc0(sizeof(Tuplesortstate_mk));

    state->status = TSS_INITIAL;
    state->randomAccess = randomAccess;
    state->memAllowed = workMem * 1024L;

    state->sortcontext = sortcontext;

    if(allocmemtuple)
    {
        int i;
        state->entry_allocsize = 1024;
        state->entries = (MKEntry *) palloc0(state->entry_allocsize * sizeof(MKEntry));

        for(i=0; i<state->entry_allocsize; ++i)
            mke_blank(state->entries+i);

        USEMEM(state, GetMemoryChunkSpace(state->entries));

        /* workMem must be large enough for the minimal memtuples array */
        if (LACKMEM(state))
            elog(ERROR, "insufficient memory allowed for sort");
    } 

    /*
     * maxTapes, tapeRange, and Algorithm D variables will be initialized by
     * inittapes(), if needed
     */
    MemoryContextSwitchTo(oldcontext);
    return state;
}


/*
 * Initialize some extra CDB attributes for the sort, including limit
 * and uniqueness.  Should do this after begin_heap.
 * 
 */
    void
cdb_tuplesort_init_mk(Tuplesortstate_mk *state, 
        int64 offset, int64 limit, int unique, int sort_flags,
        int64 maxdistinct)
{
    UnusedArg(sort_flags);
    UnusedArg(maxdistinct);

    if(limit)
    {
        int64 uselimit = offset + limit;

        /* Only use limit for less than 10 million */
        if(uselimit < 10000000) 
        {
            state->mkctxt.limit = (int32) uselimit;
            state->mkctxt.limitmask = -1;
        }
    }

    if(unique)
        state->mkctxt.unique = true;
}

/* make a copy of current state pos */
void tuplesort_begin_pos_mk(Tuplesortstate_mk *st, TuplesortPos_mk **pos)
{
    TuplesortPos_mk *st_pos;
    Assert(st);

    st_pos = (TuplesortPos_mk *) palloc(sizeof(TuplesortPos_mk));
    memcpy(st_pos, &(st->pos), sizeof(TuplesortPos_mk));

    if(st->tapeset)
        st_pos->cur_work_tape = LogicalTapeSetDuplicateTape(st->tapeset, st->result_tape);

    *pos = st_pos;
}

    Tuplesortstate_mk *
tuplesort_begin_heap_mk(TupleDesc tupDesc,
        int nkeys,
        Oid *sortOperators, AttrNumber *attNums,
        int workMem, bool randomAccess)
{
    Tuplesortstate_mk *state = tuplesort_begin_common(workMem, randomAccess, true);
    MemoryContext oldcontext;
    int	i;

    oldcontext = MemoryContextSwitchTo(state->sortcontext);

    AssertArg(nkeys > 0);

    if (trace_sort)
        PG_TRACE3(tuplesort__begin, nkeys, workMem, randomAccess);

    state->nKeys = nkeys;
    state->copytup = copytup_heap;
    state->writetup = writetup_heap;
    state->readtup = readtup_heap;

    state->tupDesc = tupDesc;	/* assume we need not copy tupDesc */
    state->mt_bind = create_memtuple_binding(tupDesc);
    state->cmpScanKey = palloc0(nkeys * sizeof(ScanKeyData));

    state->mkctxt.total_lv = nkeys;
    state->mkctxt.comp = (MKCompare *) palloc0(nkeys * sizeof(MKCompare));
    state->mkctxt.prep = (MKPrepare *) palloc0(nkeys * sizeof(MKPrepare)); 

    state->mkctxt.cpfr = tupsort_cpfr;

    state->mkctxt.ctxt_size = sizeof(TupsortMKContext); 
    state->mkctxt.ctxt = palloc0(nkeys * sizeof(TupsortMKContext)); 

    for (i = 0; i < nkeys; i++)
    {
        RegProcedure sortFunction;
        TupsortMKContext *sinfo = (TupsortMKContext *) mk_get_ctxt(&state->mkctxt, i);
        ScanKey sk = state->cmpScanKey + i;

        AssertArg(sortOperators[i] != 0);
        AssertArg(attNums[i] != 0);

        /* select a function that implements the sort operator */
        SelectSortFunction(sortOperators[i], &sortFunction, &sinfo->sortfnkind);

        if(sinfo->sortfnkind == SORTFUNC_REVLT || sinfo->sortfnkind == SORTFUNC_REVCMP)
            sinfo->nullfirst = true;
        else
            sinfo->nullfirst = false;

        /*
         * We needn't fill in sk_strategy or sk_subtype since these scankeys
         * will never be passed to an index.
         */
        ScanKeyInit(sk, 
                attNums[i],
                InvalidStrategy,
                sortFunction,
                (Datum) 0);

        sinfo->attno = attNums[i];
        sinfo->sknum = i;
        sinfo->tsstate = state;
        sinfo->mk_common.typByVal = tupDesc->attrs[sinfo->attno-1]->attbyval;
        sinfo->mk_common.typLen = tupDesc->attrs[sinfo->attno-1]->attlen;

        state->mkctxt.comp[i] = tupsort_compare_datum; 
        state->mkctxt.prep[i] = tupsort_prepare_mtup;

        if(sk->sk_func.fn_addr == btint4cmp)
            state->mkctxt.comp[i] = tupsort_compare_int32;

        if(!lc_collate_is_c() )
        {
            if(sk->sk_func.fn_addr == bpcharcmp) 
            {
                state->mkctxt.comp[i] = tupsort_compare_char;
                state->mkctxt.prep[i] = tupsort_prepare_mtup_char;
            } 
            else if(sk->sk_func.fn_addr == bttextcmp) 
            {
                state->mkctxt.comp[i] = tupsort_compare_char;
                state->mkctxt.prep[i] = tupsort_prepare_mtup_text;
            }
        }
    }

    MemoryContextSwitchTo(oldcontext);

    return state;
}

Tuplesortstate_mk *
tuplesort_begin_heap_file_readerwriter_mk(
        const char *rwfile_prefix, bool isWriter,
        TupleDesc tupDesc,
        int nkeys,
        Oid *sortOperators, AttrNumber *attNums,
        int workMem, bool randomAccess)
{
    Tuplesortstate_mk *state;
    char statedump[MAXPGPATH];

    Assert(randomAccess);

    snprintf(statedump, sizeof(statedump), "%s_sortstate", rwfile_prefix);

    if(isWriter)
    {
        /*
         * Writer is a oridinary tuplesort, except the underlying buf file are named by
         * rwfile_prefix.
         */
        state = tuplesort_begin_heap_mk(tupDesc, nkeys, sortOperators, attNums, workMem, randomAccess);

        state->pfile_rwfile_prefix = MemoryContextStrdup(state->sortcontext, rwfile_prefix);
        state->pfile_rwfile_state = BufFileCreateTemp_ReaderWriter(statedump, true);
        return state;
    }
    else
    {

        /* 
         * For reader, we really don't know anything about sort op, attNums, etc.  
         * All the readers cares are the data on the logical tape set.  The state
         * of the logical tape set has been dumped, so we load it back and that is
         * it.
         */
        MemoryContext oldctxt;
        state = tuplesort_begin_common(workMem, randomAccess, false);
        state->status = TSS_SORTEDONTAPE;
        state->randomAccess = true;

        state->readtup = readtup_heap;

        oldctxt = MemoryContextSwitchTo(state->sortcontext);

        state->pfile_rwfile_prefix = MemoryContextStrdup(state->sortcontext, rwfile_prefix);
        state->pfile_rwfile_state = BufFileCreateTemp_ReaderWriter(statedump, false);

        state->tapeset = LoadLogicalTapeSetState(state->pfile_rwfile_state, rwfile_prefix);
        state->currentRun = 0;
        state->result_tape = LogicalTapeSetGetTape(state->tapeset, 0);

        state->pos.eof_reached =false;
        state->pos.markpos.tapepos.blkNum = 0;
        state->pos.markpos.tapepos.offset = 0; 
        state->pos.markpos.mempos = 0;
        state->pos.markpos_eof = false;
        state->pos.cur_work_tape = NULL;

        MemoryContextSwitchTo(oldctxt);
        return state;
    }
}

    Tuplesortstate_mk *
tuplesort_begin_index_mk(Relation indexRel,
        bool enforceUnique,
        int workMem, bool randomAccess)
{
    Tuplesortstate_mk *state = tuplesort_begin_common(workMem, randomAccess, true);
    MemoryContext oldcontext;
    TupleDesc tupdesc;

    oldcontext = MemoryContextSwitchTo(state->sortcontext);

    if (trace_sort)
        PG_TRACE3(tuplesort__begin, enforceUnique, workMem, randomAccess);

    state->nKeys = RelationGetNumberOfAttributes(indexRel);
    tupdesc = RelationGetDescr(indexRel);

    state->copytup = copytup_index;
    state->writetup = writetup_index;
    state->readtup = readtup_index;

    state->indexRel = indexRel;

    /* see comments below about btree dependence of this code... */
    {
        int i;
        int nkeys = state->nKeys;

        state->cmpScanKey = _bt_mkscankey_nodata(indexRel);
        state->mkctxt.total_lv = nkeys;
        state->mkctxt.comp = (MKCompare *) palloc0(nkeys * sizeof(MKCompare)); 
        state->mkctxt.prep = (MKPrepare *) palloc0(nkeys * sizeof(MKPrepare)); 
        state->mkctxt.cpfr = tupsort_cpfr; 

        state->mkctxt.ctxt_size = sizeof(TupsortMKContext); 
        state->mkctxt.ctxt = palloc0(nkeys * sizeof(TupsortMKContext)); 

        for(i=0; i<nkeys; ++i)
        {
            TupsortMKContext *sinfo = (TupsortMKContext *) mk_get_ctxt(&state->mkctxt, i);
            ScanKey sk = state->cmpScanKey + i;

            sinfo->sortfnkind = SORTFUNC_CMP;
            sinfo->nullfirst = false;

            sinfo->attno = i+1;
            sinfo->sknum = i;
            sinfo->tsstate = state;
            sinfo->mk_common.typByVal = tupdesc->attrs[i]->attbyval;
            sinfo->mk_common.typLen = tupdesc->attrs[i]->attlen; 

            state->mkctxt.comp[i] = tupsort_compare_datum;
            state->mkctxt.prep[i] = tupsort_prepare_itup;

            if(sk->sk_func.fn_addr == btint4cmp)
                state->mkctxt.comp[i] = tupsort_compare_int32;


            if(!lc_collate_is_c() )
            {
                if(sk->sk_func.fn_addr == bpcharcmp) 
                {
                    state->mkctxt.comp[i] = tupsort_compare_char;
                    state->mkctxt.prep[i] = tupsort_prepare_itup_char;
                } 
                else if(sk->sk_func.fn_addr == bttextcmp) 
                {
                    state->mkctxt.comp[i] = tupsort_compare_char;
                    state->mkctxt.prep[i] = tupsort_prepare_itup_text;
                }
            }
        }
    }

    state->mkctxt.enforceUnique = enforceUnique;
    MemoryContextSwitchTo(oldcontext);

    return state;
}

    Tuplesortstate_mk *
tuplesort_begin_datum_mk(Oid datumType,
        Oid sortOperator,
        int workMem, bool randomAccess)
{
    Tuplesortstate_mk *state = tuplesort_begin_common(workMem, randomAccess, true);
    MemoryContext oldcontext;
    RegProcedure sortFunction;
    int16		typlen;
    bool		typbyval;

    oldcontext = MemoryContextSwitchTo(state->sortcontext);

    if (trace_sort)
        PG_TRACE3(tuplesort__begin, datumType, workMem, randomAccess);

    state->nKeys = 1;	/* always a one-column sort */

    state->copytup = copytup_datum;
    state->writetup = writetup_datum;
    state->readtup = readtup_datum;

    state->datumType = datumType;

    /* lookup necessary attributes of the datum type */
    get_typlenbyval(datumType, &typlen, &typbyval);
    state->datumTypeLen = typlen;
    state->datumTypeByVal = typbyval;

    state->sortOperator = sortOperator;

    state->cmpScanKey = palloc0(sizeof(ScanKeyData));

    state->mkctxt.total_lv = 1;
    state->mkctxt.comp = (MKCompare *) palloc(sizeof(MKCompare));
    state->mkctxt.comp[0] = tupsort_compare_datum;
    state->mkctxt.prep = (MKPrepare *) palloc0(sizeof(MKPrepare));

    state->mkctxt.cpfr = tupsort_cpfr;

    state->mkctxt.ctxt_size = sizeof(TupsortMKContext);
    state->mkctxt.ctxt = palloc0(sizeof(TupsortMKContext));

    {
        TupsortMKContext *sinfo = (TupsortMKContext *) mk_get_ctxt(&state->mkctxt, 0);
        SelectSortFunction(sortOperator, &sortFunction, &sinfo->sortfnkind);
        fmgr_info(sortFunction, &state->cmpScanKey->sk_func); 

        if(sinfo->sortfnkind == SORTFUNC_REVLT || sinfo->sortfnkind == SORTFUNC_REVCMP)
            sinfo->nullfirst = true;
        else
            sinfo->nullfirst = false;
        state->nullfirst = sinfo->nullfirst;

        sinfo->mk_common.typByVal = typbyval;
        sinfo->mk_common.typLen = typlen;
        sinfo->attno = -1;
        sinfo->sknum = 0;
        sinfo->tsstate = state;
    }

    MemoryContextSwitchTo(oldcontext);

    return state;
}

/*
 * tuplesort_end
 *
 *	Release resources and clean up.
 *
 * NOTE: after calling this, any pointers returned by tuplesort_getXXX are
 * pointing to garbage.  Be careful not to attempt to use or free such
 * pointers afterwards!
 */
    void
tuplesort_end_mk(Tuplesortstate_mk *state)
{
    /* context swap probably not needed, but let's be safe */
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

    long		spaceUsed;

    if (state->tapeset)
        spaceUsed = LogicalTapeSetBlocks(state->tapeset);
    else
        spaceUsed = (state->memUsed + 1023) / 1024;

    /*
     * Delete temporary "tape" files, if any.
     *
     * Note: want to include this in reported total cost of sort, hence need
     * for two #ifdef TRACE_SORT sections.
     */
    if (state->tapeset)
    {
        LogicalTapeSetClose(state->tapeset);
        state->tapeset = NULL;
    }

    /* 
     * CDB: Report statistics to EXPLAIN ANALYZE. 
     */ 
    if (state->instrument)
    {
        /* How close did we come to the work_mem limit? */
        if (state->instrument->workmemused < state->memUsed) 
            state->instrument->workmemused = state->memUsed; 

        /* Report executor memory used by our memory context. */
        state->instrument->execmemused +=
            (double)MemoryContextGetPeakSpace(state->sortcontext);
    }

    if (trace_sort)
        PG_TRACE2(tuplesort__end, state->tapeset ? 1 : 0, spaceUsed);

    MemoryContextSwitchTo(oldcontext);
    /*
     * Free the per-sort memory context, thereby releasing all working memory,
     * including the Tuplesortstate_mk struct itself.
     */
    MemoryContextDelete(state->sortcontext);
}


/*
 * tuplesort_set_instrument
 *
 * May be called after tuplesort_begin_xxx() to enable reporting of
 * statistics and events for EXPLAIN ANALYZE.
 *
 * The 'instr' and 'explainbuf' ptrs are retained in the 'state' object for
 * possible use anytime during the sort, up to and including tuplesort_end().
 * The caller must ensure that the referenced objects remain allocated and
 * valid for the life of the Tuplesortstate_mk object; or if they are to be
 * freed early, disconnect them by calling again with NULL pointers.
 */
    void 
tuplesort_set_instrument_mk(Tuplesortstate_mk            *state,
        struct Instrumentation    *instrument,
        struct StringInfoData     *explainbuf)
{
    state->instrument = instrument;
    state->explainbuf = explainbuf;
}                               /* tuplesort_set_instrument */

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
    void
tuplesort_puttupleslot_mk(Tuplesortstate_mk *state, TupleTableSlot *slot)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
    MKEntry e;
    mke_blank(&e);

    COPYTUP(state, &e, (void *) slot); 
    puttuple_common(state, &e);

    MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one index tuple while collecting input data for sort.
 *
 * Note that the input tuple is always copied; the caller need not save it.
 */
    void
tuplesort_putindextuple_mk(Tuplesortstate_mk *state, IndexTuple tuple)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
    MKEntry e;
    mke_blank(&e);

    COPYTUP(state, &e, (void *) tuple); 
    puttuple_common(state, &e); 

    MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one Datum while collecting input data for sort.
 *
 * If the Datum is pass-by-ref type, the value will be copied.
 */
    void
tuplesort_putdatum_mk(Tuplesortstate_mk *state, Datum val, bool isNull)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
    MKEntry e;
    mke_blank(&e);

    /*
     * If it's a pass-by-reference value, copy it into memory we control, and
     * decrease availMem.  Then call the common code.
     */
    if (isNull || state->datumTypeByVal)
    {
        e.d = val;
        if(isNull)
            mke_set_null(&e, state->nullfirst); 
        else
            mke_set_not_null(&e);
    }
    else
    {
        mke_set_not_null(&e);
        e.d = datumCopy(val, false, state->datumTypeLen);
        USEMEM(state, GetMemoryChunkSpace(DatumGetPointer(e.d))); 
    }

    puttuple_common(state, &e); 
    MemoryContextSwitchTo(oldcontext);
}

/*
 * Shared code for tuple and datum cases.
 */
    static void
puttuple_common(Tuplesortstate_mk *state, MKEntry *e)
{
    Assert(is_under_sort_ctxt(state));
    ++state->put_cnt;

    if(state->gpmon_pkt)
        Gpmon_M_Incr(state->gpmon_pkt, GPMON_QEXEC_M_ROWSIN);

    switch (state->status)
    {
        case TSS_INITIAL:

            /*
             * Save the tuple into the unsorted array.	First, grow the array
             * as needed.  Note that we try to grow the array when there is
             * still one free slot remaining --- if we fail, there'll still be
             * room to store the incoming tuple, and then we'll switch to
             * tape-based operation.
             */
            if (!state->mkheap && state->entry_count >= state->entry_allocsize - 1) 
            {
                int i;
                int j;
                if(state->entries)
                    FREEMEM(state, GetMemoryChunkSpace(state->entries));

                state->entries = (MKEntry *) repalloc(state->entries, state->entry_allocsize * 2 * sizeof(MKEntry));

                j = state->entry_allocsize * 2;
                for(i=state->entry_allocsize; i<j; ++i)
                    mke_blank(state->entries + i);

                state->entry_allocsize *= 2;
                USEMEM(state, GetMemoryChunkSpace(state->entries));
            }

            /* full sort? */
            if (state->mkctxt.limit == 0)
                state->entries[state->entry_count++] = *e;
            else
            {
                tuplesort_limit_insert(state, e); 
                return;
            }

            /* If out of work_mem, switch to diskmode */
            if(LACKMEM(state))
            {
                inittapes_mk(state, is_sortstate_rwfile(state) ? state->pfile_rwfile_prefix : NULL);
                Assert(state->status == TSS_BUILDRUNS);
            }
            break;

        case TSS_BUILDRUNS:

            /*
             * Insert the tuple into the heap
             */
            Assert(state->mkheap); 
            tuplesort_heap_insert(state, e);
            break;
        default:
            elog(ERROR, "invalid tuplesort state");
            break;
    }
}

/*
 * All tuples have been provided; finish the sort.
 */
    void
tuplesort_performsort_mk(Tuplesortstate_mk *state)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

    if (trace_sort)
        PG_TRACE(tuplesort__perform__sort);

    switch (state->status)
    {
        case TSS_INITIAL:
            /*
             * We were able to accumulate all the tuples within the allowed
             * amount of memory.  Just qsort 'em and we're done.
             */
            if(state->mkctxt.limit == 0)
                mk_qsort(state->entries, state->entry_count, &state->mkctxt);
            else
                tuplesort_limit_sort(state);

            state->pos.current = 0;
            state->pos.eof_reached = false;
            state->pos.markpos.mempos = 0;
            state->pos.markpos_eof = false;
            state->status = TSS_SORTEDINMEM;

            /* Not shareinput sort, we are done. */
            if(!is_sortstate_rwfile(state))
                break;

            /* Shareinput sort, need to put this stuff onto disk */
            inittapes_mk(state, state->pfile_rwfile_prefix);
            /* Fall through */
        case TSS_BUILDRUNS:
            dumptuples_mk(state, true);

            if(state->instrument)
            {
                state->instrument->workmemwanted =
                    Max(state->instrument->workmemwanted, state->memUsed + state->spilledBytes);
            }

            mergeruns(state);

            state->pos.eof_reached = false;
            state->pos.markpos.tapepos.blkNum = 0;
            state->pos.markpos.tapepos.offset = 0;
            state->pos.markpos_eof = false;
            break;

        default:
            elog(ERROR, "Invalid tuplesort state");
            break;
    }
    MemoryContextSwitchTo(oldcontext);
}

void tuplesort_flush_mk(Tuplesortstate_mk *state)
{
    Assert(state->status == TSS_SORTEDONTAPE);
    Assert(state->tapeset && state->pfile_rwfile_state);
    Assert(state->pos.cur_work_tape == NULL);
    LogicalTapeFlush(state->tapeset, state->result_tape, state->pfile_rwfile_state);
    BufFileFlush(state->pfile_rwfile_state);
}

/*
 * Internal routine to fetch the next tuple in either forward or back
 * direction into *stup.  Returns FALSE if no more tuples.
 * If *should_free is set, the caller must pfree stup.tuple when done with it.
 */
    static bool
tuplesort_gettuple_common_pos(Tuplesortstate_mk *state, TuplesortPos_mk *pos,
        bool forward, MKEntry *e, bool *should_free)
{
    uint32 tuplen;
    LogicalTape *work_tape;
    bool fOK;

    Assert(is_under_sort_ctxt(state));
    ++state->get_cnt;

    switch (state->status)
    {
        case TSS_SORTEDINMEM:
            Assert(forward || state->randomAccess);
            *should_free = false;

            if (forward)
            {
                if (pos->current < state->entry_count)
                {
                    *e = state->entries[pos->current++];
                    return true;
                }
                pos->eof_reached = true;
                return false;
            }
            else
            {
                if (pos->current <= 0)
                    return false;

                /*
                 * if all tuples are fetched already then we return last
                 * tuple, else - tuple before last returned.
                 */
                if (pos->eof_reached)
                    pos->eof_reached = false;
                else
                {
                    pos->current--;	/* last returned tuple */
                    if (pos->current <= 0)
                        return false;
                }
                *e = state->entries[pos->current-1]; 
                return true;
            }
            break;

        case TSS_SORTEDONTAPE:
            AssertEquivalent((pos == &state->pos), (pos->cur_work_tape == NULL));
            Assert(forward || state->randomAccess);
            *should_free = true;
            work_tape =	pos->cur_work_tape == NULL ? state->result_tape : pos->cur_work_tape; 

            if (forward)
            {
                if (pos->eof_reached)
                    return false;
                if ((tuplen = getlen(state, pos, work_tape, true)) != 0) 
                {
                    READTUP(state, pos, e, work_tape, tuplen);
                    return true;
                }
                else
                {
                    pos->eof_reached = true;
                    return false;
                }
            }

            /*
             * Backward.
             *
             * if all tuples are fetched already then we return last tuple,
             * else - tuple before last returned.
             */
            /*
             * Seek position is pointing just past the zero tuplen at the
             * end of file; back up to fetch last tuple's ending length
             * word.  If seek fails we must have a completely empty file.
             */
            fOK = LogicalTapeBackspace(state->tapeset, work_tape,  2*sizeof(uint32)); 
            if(!fOK)
                return false;

            if (pos->eof_reached)
            {
                pos->eof_reached = false;
            }
            else
            {
                tuplen = getlen(state, pos, work_tape, false);

                /*
                 * Back up to get ending length word of tuple before it.
                 */
                fOK = LogicalTapeBackspace(state->tapeset, work_tape, tuplen + 2 *sizeof(uint32)); 

                if (!fOK)
                {
                    /*
                     * If that fails, presumably the prev tuple is the first
                     * in the file.  Back up so that it becomes next to read
                     * in forward direction (not obviously right, but that is
                     * what in-memory case does).
                     */
                    fOK = LogicalTapeBackspace(state->tapeset, work_tape, tuplen + 2 *sizeof(uint32)); 
                    if(!fOK)
                        elog(ERROR, "bogus tuple length in backward scan");

                    return false;
                }
            }

            tuplen = getlen(state, pos, work_tape, false);

            /*
             * Now we have the length of the prior tuple, back up and read it.
             * Note: READTUP expects we are positioned after the initial
             * length word of the tuple, so back up to that point.
             */
            fOK = LogicalTapeBackspace(state->tapeset, work_tape, tuplen);
            if (!fOK) 
                elog(ERROR, "bogus tuple length in backward scan");

            READTUP(state, pos, e, work_tape, tuplen);
            return true;

        case TSS_FINALMERGE:
            Assert(forward);
            Assert(pos == &state->pos && pos->cur_work_tape == NULL);

            *should_free = true;
            mkheap_puget_reader(state->mkheap, e);

            tupsort_cpfr(e, NULL, mk_get_ctxt(&state->mkctxt, mke_get_lv(e)));

            return !mke_is_empty(e);

        default:
            elog(ERROR, "invalid tuplesort state");
            return false;		/* keep compiler quiet */
    }
}

/*
 * Fetch the next tuple in either forward or back direction.
 * If successful, put tuple in slot and return TRUE; else, clear the slot
 * and return FALSE.
 */
    bool
tuplesort_gettupleslot_mk(Tuplesortstate_mk *state, bool forward,
        TupleTableSlot *slot)
{
    return tuplesort_gettupleslot_pos_mk(state, &state->pos, forward, slot);
}

    bool
tuplesort_gettupleslot_pos_mk(Tuplesortstate_mk *state, TuplesortPos_mk *pos,
        bool forward, TupleTableSlot *slot)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
    MKEntry	e; 

    bool should_free = false;
    bool fOK;

    mke_set_empty(&e); 
    fOK = tuplesort_gettuple_common_pos(state, pos, forward, &e, &should_free);

    MemoryContextSwitchTo(oldcontext);

    if (fOK) 
    {
        Assert(!mke_is_empty(&e));
        ExecStoreMemTuple(e.ptr, slot, should_free);

        if(state->gpmon_pkt)
            Gpmon_M_Incr_Rows_Out(state->gpmon_pkt);

        return true;
    }

    ExecClearTuple(slot);
    return false;
}

/*
 * Fetch the next index tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If *should_free is set, the
 * caller must pfree the returned tuple when done with it.
 */
    IndexTuple
tuplesort_getindextuple_mk(Tuplesortstate_mk *state, bool forward,
        bool *should_free)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
    MKEntry e;
    bool fOK = tuplesort_gettuple_common_pos(state, &state->pos, forward, &e, should_free);

    MemoryContextSwitchTo(oldcontext);

    if(fOK)
        return (IndexTuple) (e.ptr); 
    return NULL;
}

/*
 * Fetch the next Datum in either forward or back direction.
 * Returns FALSE if no more datums.
 *
 * If the Datum is pass-by-ref type, the returned value is freshly palloc'd
 * and is now owned by the caller.
 */
    bool
tuplesort_getdatum_mk(Tuplesortstate_mk *state, bool forward,
        Datum *val, bool *isNull)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
    MKEntry e;
    bool		should_free = false;

    bool fOK = tuplesort_gettuple_common_pos(state, &state->pos, forward, &e, &should_free);

    if(fOK)
    {
        *isNull = mke_is_null(&e);
        if(*isNull || state->datumTypeByVal || should_free)
            *val = e.d;
        else
            *val = datumCopy(e.d, false, state->datumTypeLen);
    }

    MemoryContextSwitchTo(oldcontext);
    return fOK;
}

/*
 * tuplesort_merge_order - report merge order we'll use for given memory
 * (note: "merge order" just means the number of input tapes in the merge).
 *
 * This is exported for use by the planner.  allowedMem is in bytes.
 */
    int
tuplesort_merge_order(long allowedMem)
{
    int			mOrder;

    /*
     * We need one tape for each merge input, plus another one for the output,
     * and each of these tapes needs buffer space.	In addition we want
     * MERGE_BUFFER_SIZE workspace per input tape (but the output tape doesn't
     * count).
     *
     * Note: you might be thinking we need to account for the memtuples[]
     * array in this calculation, but we effectively treat that as part of the
     * MERGE_BUFFER_SIZE workspace.
     */
    mOrder = (allowedMem - TAPE_BUFFER_OVERHEAD) /
        (MERGE_BUFFER_SIZE + TAPE_BUFFER_OVERHEAD);

    mOrder = Max(mOrder, MINORDER);
    mOrder = Min(mOrder, MAXORDER); 

    return mOrder;
}

/*
 * inittapes - initialize for tape sorting.
 *
 * This is called only if we have found we don't have room to sort in memory.
 */
    void
inittapes_mk(Tuplesortstate_mk *state, const char* rwfile_prefix)
{
    int			maxTapes;
    int 		j;
    long		tapeSpace;

    Assert(is_under_sort_ctxt(state));

    /* Compute number of tapes to use: merge order plus 1 */
    maxTapes = tuplesort_merge_order(state->memAllowed) + 1;

    /*
     * We must have at least 2*maxTapes slots in the memtuples[] array, else
     * we'd not have room for merge heap plus preread.  It seems unlikely that
     * this case would ever occur, but be safe.
     */
    maxTapes = Min(maxTapes, state->entry_count / 2);
    maxTapes = Max(maxTapes, 1);

    /* XXX XXX: with losers, only need 1x slots because we don't need a merge heap */

    state->maxTapes = maxTapes;
    state->tapeRange = maxTapes - 1;

    if (trace_sort)
        PG_TRACE1(tuplesort__switch__external, maxTapes);

    /*
     * Decrease availMem to reflect the space needed for tape buffers; but
     * don't decrease it to the point that we have no room for tuples. (That
     * case is only likely to occur if sorting pass-by-value Datums; in all
     * other scenarios the memtuples[] array is unlikely to occupy more than
     * half of allowedMem.	In the pass-by-value case it's not important to
     * account for tuple space, so we don't care if LACKMEM becomes
     * inaccurate.)
     */
    tapeSpace = maxTapes * TAPE_BUFFER_OVERHEAD;
    if (tapeSpace + GetMemoryChunkSpace(state->entries) < state->memAllowed)
        USEMEM(state, tapeSpace);

    /*
     * Create the tape set and allocate the per-tape data arrays.
     */
    if(!rwfile_prefix)
        state->tapeset = LogicalTapeSetCreate(maxTapes);
    else
        state->tapeset = LogicalTapeSetCreate_ReaderWriter(rwfile_prefix, maxTapes);

    state->mergeactive = (bool *) palloc0(maxTapes * sizeof(bool));
    state->mergenext = (int *) palloc0(maxTapes * sizeof(int));
    state->mergelast = (int *) palloc0(maxTapes * sizeof(int));
    state->mergeavailslots = (int *) palloc0(maxTapes * sizeof(int));
    state->mergeavailmem = (long *) palloc0(maxTapes * sizeof(long));
    state->tp_fib = (int *) palloc0(maxTapes * sizeof(int));
    state->tp_runs = (int *) palloc0(maxTapes * sizeof(int));
    state->tp_dummy = (int *) palloc0(maxTapes * sizeof(int));
    state->tp_tapenum = (int *) palloc0(maxTapes * sizeof(int));

    /*
     * Convert the unsorted contents of memtuples[] into a heap. Each tuple is
     * marked as belonging to run number zero.
     *
     * NOTE: we pass false for checkIndex since there's no point in comparing
     * indexes in this step, even though we do intend the indexes to be part
     * of the sort key...
     */
    Assert(state->mkheap == NULL);
    Assert(state->status == TSS_INITIAL || state->status == TSS_SORTEDINMEM);
    if(state->status == TSS_INITIAL)
    {
        state->mkheap = mkheap_from_array(state->entries, state->entry_allocsize, state->entry_count, &state->mkctxt);
        state->entries = NULL;
        state->entry_allocsize = 0;
        state->entry_count = 0;
    }

    state->currentRun = 0;

    /*
     * Initialize variables of Algorithm D (step D1).
     */
    for (j = 0; j < maxTapes; j++)
    {
        state->tp_fib[j] = 1;
        state->tp_runs[j] = 0;
        state->tp_dummy[j] = 1;
        state->tp_tapenum[j] = j;
    }
    state->tp_fib[state->tapeRange] = 0;
    state->tp_dummy[state->tapeRange] = 0;

    state->Level = 1;
    state->destTape = 0;

    state->status = TSS_BUILDRUNS;
}

/*
 * selectnewtape -- select new tape for new initial run.
 *
 * This is called after finishing a run when we know another run
 * must be started.  This implements steps D3, D4 of Algorithm D.
 */
    static void
selectnewtape_mk(Tuplesortstate_mk *state)
{
    int			j;
    int			a;

    /* Step D3: advance j (destTape) */
    if (state->tp_dummy[state->destTape] < state->tp_dummy[state->destTape + 1])
    {
        state->destTape++;
        return;
    }
    if (state->tp_dummy[state->destTape] != 0)
    {
        state->destTape = 0;
        return;
    }

    /* Step D4: increase level */
    state->Level++;
    a = state->tp_fib[0];
    for (j = 0; j < state->tapeRange; j++)
    {
        state->tp_dummy[j] = a + state->tp_fib[j + 1] - state->tp_fib[j];
        state->tp_fib[j] = a + state->tp_fib[j + 1];
    }
    state->destTape = 0;
}

/*
 * mergeruns -- merge all the completed initial runs.
 *
 * This implements steps D5, D6 of Algorithm D.  All input data has
 * already been written to initial runs on tape (see dumptuples).
 */
    static void
mergeruns(Tuplesortstate_mk *state)
{
    int			tapenum,
                svTape,
                svRuns,
                svDummy;
    LogicalTape *lt = NULL;

    Assert(state->status == TSS_BUILDRUNS);

    /*
     * If we produced only one initial run (quite likely if the total data
     * volume is between 1X and 2X workMem), we can just use that tape as the
     * finished output, rather than doing a useless merge.	(This obvious
     * optimization is not in Knuth's algorithm.)
     */
    if (state->currentRun == 1)
    {
        state->result_tape = LogicalTapeSetGetTape(state->tapeset, state->tp_tapenum[state->destTape]);
        /* must freeze and rewind the finished output tape */
        LogicalTapeFreeze(state->tapeset, state->result_tape);
        state->status = TSS_SORTEDONTAPE;
        return;
    }

    /* End of step D2: rewind all output tapes to prepare for merging */
    for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
    {
        lt = LogicalTapeSetGetTape(state->tapeset, tapenum);
        LogicalTapeRewind(state->tapeset, lt, false);
    }

    /* Clear gpmon for respilling data */
    if(state->gpmon_pkt)
    {
        Gpmon_M_Incr(state->gpmon_pkt, GPMON_SORT_SPILLPASS);
        Gpmon_M_Reset(state->gpmon_pkt, GPMON_SORT_CURRSPILLPASS_TUPLE, 0);
        Gpmon_M_Reset(state->gpmon_pkt, GPMON_SORT_CURRSPILLPASS_BYTE, 0);
    }

    for (;;)
    {
        /*
         * At this point we know that tape[T] is empty.  If there's just one
         * (real or dummy) run left on each input tape, then only one merge
         * pass remains.  If we don't have to produce a materialized sorted
         * tape, we can stop at this point and do the final merge on-the-fly.
         */
        if (!state->randomAccess)
        {
            bool		allOneRun = true;

            Assert(state->tp_runs[state->tapeRange] == 0);
            for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
            {
                if (state->tp_runs[tapenum] + state->tp_dummy[tapenum] != 1)
                {
                    allOneRun = false;
                    break;
                }
            }
            if (allOneRun)
            {
                /* Tell logtape.c we won't be writing anymore */
                LogicalTapeSetForgetFreeSpace(state->tapeset);
                /* Initialize for the final merge pass */
                beginmerge(state);
                state->status = TSS_FINALMERGE;
                return;
            }
        }

        /* Step D5: merge runs onto tape[T] until tape[P] is empty */
        while (state->tp_runs[state->tapeRange - 1] ||
                state->tp_dummy[state->tapeRange - 1])
        {
            bool		allDummy = true;

            for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
            {
                if (state->tp_dummy[tapenum] == 0)
                {
                    allDummy = false;
                    break;
                }
            }

            if (allDummy)
            {
                state->tp_dummy[state->tapeRange]++;
                for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
                    state->tp_dummy[tapenum]--;
            }
            else
                mergeonerun_mk(state);
        }

        /* Step D6: decrease level */
        if (--state->Level == 0)
            break;

        /* rewind output tape T to use as new input */
        lt = LogicalTapeSetGetTape(state->tapeset, state->tp_tapenum[state->tapeRange]);
        LogicalTapeRewind(state->tapeset, lt, false);

        /* rewind used-up input tape P, and prepare it for write pass */
        lt = LogicalTapeSetGetTape(state->tapeset, state->tp_tapenum[state->tapeRange - 1]);
        LogicalTapeRewind(state->tapeset, lt, true);

        state->tp_runs[state->tapeRange - 1] = 0;

        /*
         * reassign tape units per step D6; note we no longer care about A[]
         */
        svTape = state->tp_tapenum[state->tapeRange];
        svDummy = state->tp_dummy[state->tapeRange];
        svRuns = state->tp_runs[state->tapeRange];
        for (tapenum = state->tapeRange; tapenum > 0; tapenum--)
        {
            state->tp_tapenum[tapenum] = state->tp_tapenum[tapenum - 1];
            state->tp_dummy[tapenum] = state->tp_dummy[tapenum - 1];
            state->tp_runs[tapenum] = state->tp_runs[tapenum - 1];
        }
        state->tp_tapenum[0] = svTape;
        state->tp_dummy[0] = svDummy;
        state->tp_runs[0] = svRuns;
    }

    /*
     * Done.  Knuth says that the result is on TAPE[1], but since we exited
     * the loop without performing the last iteration of step D6, we have not
     * rearranged the tape unit assignment, and therefore the result is on
     * TAPE[T].  We need to do it this way so that we can freeze the final
     * output tape while rewinding it.	The last iteration of step D6 would be
     * a waste of cycles anyway...
     */
    state->result_tape = LogicalTapeSetGetTape(state->tapeset, state->tp_tapenum[state->tapeRange]);
    LogicalTapeFreeze(state->tapeset, state->result_tape);
    state->status = TSS_SORTEDONTAPE;
}

/*
 * Merge one run from each input tape, except ones with dummy runs.
 *
 * This is the inner loop of Algorithm D step D5.  We know that the
 * output tape is TAPE[T].
 */
    void
mergeonerun_mk(Tuplesortstate_mk *state)
{
    int			destTape = state->tp_tapenum[state->tapeRange];
    MKEntry e;

    LogicalTape *lt = NULL;

    /*
     * Start the merge by loading one tuple from each active source tape into
     * the heap.  We can also decrease the input run/dummy run counts.
     */
    beginmerge(state);

    /*
     * Execute merge by repeatedly extracting lowest tuple in heap, writing it
     * out, and replacing it with next tuple from same tape (if there is
     * another one).
     */
    lt = LogicalTapeSetGetTape(state->tapeset, destTape);

    Assert(state->mkheap);

    while (mkheap_puget_reader(state->mkheap, &e) >= 0) 
        WRITETUP(state, lt, &e); 

    /*
     * When the heap empties, we're done.  Write an end-of-run marker on the
     * output tape, and increment its count of real runs.
     */
    markrunend(state, destTape);
    state->tp_runs[state->tapeRange]++;

    if (trace_sort)
        PG_TRACE1(tuplesort__mergeonerun, state->activeTapes);
}

static bool tupsort_preread(TupsortMergeReadCtxt *ctxt)
{
    uint32 tuplen;

    Assert(ctxt->mem_allowed > 0);

    if(!ctxt->pos.cur_work_tape || ctxt->pos.eof_reached)
        return false;

    ctxt->mem_used = 0;

    Assert(ctxt->p && ctxt->allocsz > 0);
    Assert(ctxt->mem_allowed > 0);

    for(ctxt->cnt=0; 
            ctxt->cnt<ctxt->allocsz && ctxt->mem_used < ctxt->mem_allowed; 
            ++ctxt->cnt)
    {
        tuplen = getlen(ctxt->tsstate, &ctxt->pos, ctxt->pos.cur_work_tape, true);
        if(tuplen != 0)
        {
            READTUP(ctxt->tsstate, &ctxt->pos, ctxt->p + ctxt->cnt, ctxt->pos.cur_work_tape, tuplen);
            ctxt->mem_used += tuplen;
        }
        else
        {
            ctxt->pos.eof_reached = true;
            break;
        }
    }

    ctxt->cur = 0;
    return ctxt->cnt > ctxt->cur;
}

static bool tupsort_mergeread(void *pvctxt, MKEntry *e) 
{
    TupsortMergeReadCtxt *ctxt = (TupsortMergeReadCtxt *) pvctxt;

    Assert(ctxt->mem_allowed > 0);

    if(ctxt->cur < ctxt->cnt)
    {
        *e = ctxt->p[ctxt->cur++];
        return true;
    }

    if(!tupsort_preread(ctxt))
        return false;

    Assert(ctxt->cur == 0 && ctxt->cnt > 0);
    *e = ctxt->p[ctxt->cur++];
    return true;
}

/*
 * beginmerge - initialize for a merge pass
 *
 * We decrease the counts of real and dummy runs for each tape, and mark
 * which tapes contain active input runs in mergeactive[].	Then, load
 * as many tuples as we can from each active input tape, and finally
 * fill the merge heap with the first tuple from each active tape.
 */
    static void
beginmerge(Tuplesortstate_mk *state)
{
    int			activeTapes;
    int			tapenum;
    int			srcTape;
    int 		totalSlots;
    int			slotsPerTape;
    long		spacePerTape;

    int i;

    MemoryContext oldctxt;

    /* Heap should be empty here */
    Assert(mkheap_empty(state->mkheap)); 

    /* Adjust run counts and mark the active tapes */
    memset(state->mergeactive, 0,
            state->maxTapes * sizeof(*state->mergeactive));
    activeTapes = 0;
    for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
    {
        if (state->tp_dummy[tapenum] > 0)
            state->tp_dummy[tapenum]--;
        else
        {
            Assert(state->tp_runs[tapenum] > 0);
            state->tp_runs[tapenum]--;
            srcTape = state->tp_tapenum[tapenum];
            state->mergeactive[srcTape] = true;
            activeTapes++;
        }
    }
    state->activeTapes = activeTapes;

    /* Clear merge-pass state variables */
    memset(state->mergenext, 0,
            state->maxTapes * sizeof(*state->mergenext));
    memset(state->mergelast, 0,
            state->maxTapes * sizeof(*state->mergelast));
    state->mergefreelist = 0;	/* nothing in the freelist */
    state->mergefirstfree = activeTapes;		/* 1st slot avail for preread */

    /*
     * Initialize space allocation to let each active input tape have an equal
     * share of preread space.
     */
    Assert(activeTapes > 0);
    totalSlots = (state->mkheap == NULL) ? state->entry_allocsize : state->mkheap->alloc_size;
    slotsPerTape = (totalSlots - state->mergefirstfree) / activeTapes;
    slotsPerTape = Max(slotsPerTape, 128);
    spacePerTape = state->memAllowed / activeTapes;


    oldctxt = MemoryContextSwitchTo(state->sortcontext);
    if(state->mkheap)
    {
        mkheap_destroy(state->mkheap);
        state->mkheap = NULL;
    }

    if(state->mkhreader)
    {
        Assert(state->mkhreader_ctxt);
        for(i=0; i<state->mkhreader_allocsize; ++i)
        {
            TupsortMergeReadCtxt *mkhr_ctxt = state->mkhreader_ctxt+i;
            AssertEquivalent(mkhr_ctxt->p!=NULL, mkhr_ctxt->allocsz>0);
            if(mkhr_ctxt->p)
                pfree(mkhr_ctxt->p);
        }

        pfree(state->mkhreader_ctxt);
        pfree(state->mkhreader);
    }

    state->mkhreader = palloc0(sizeof(MKHeapReader) * activeTapes);
    state->mkhreader_ctxt = palloc0(sizeof(TupsortMergeReadCtxt) * activeTapes);

    for(i=0; i<activeTapes; ++i)
    {
        TupsortMergeReadCtxt *mkhr_ctxt = state->mkhreader_ctxt+i;
        state->mkhreader[i].reader = tupsort_mergeread;
        state->mkhreader[i].mkhr_ctxt = mkhr_ctxt;
        mkhr_ctxt->tsstate = state;
    }

    state->mkhreader_allocsize = activeTapes;

    for (i=0, srcTape = 0; srcTape < state->maxTapes; srcTape++)
    {
        if (state->mergeactive[srcTape])
        {
            TupsortMergeReadCtxt *mkhr_ctxt = state->mkhreader_ctxt+i;

            mkhr_ctxt->pos.cur_work_tape = LogicalTapeSetGetTape(state->tapeset, srcTape);
            mkhr_ctxt->pos.eof_reached = false;
            mkhr_ctxt->mem_allowed = spacePerTape;
            Assert(mkhr_ctxt->mem_allowed > 0);
            mkhr_ctxt->mem_used = 0;
            mkhr_ctxt->cur = 0;
            mkhr_ctxt->cnt = 0;

            Assert(mkhr_ctxt->p == NULL);
            Assert(slotsPerTape > 0);
            mkhr_ctxt->p = (MKEntry *) palloc(sizeof(MKEntry) * slotsPerTape);
            mkhr_ctxt->allocsz = slotsPerTape;

            ++i;
        }
    }

    Assert(i==activeTapes);

    state->mkheap = mkheap_from_reader(state->mkhreader, i, &state->mkctxt);
    Assert(state->mkheap);

    MemoryContextSwitchTo(oldctxt); 
}


/*
 * dumptuples - remove tuples from heap and write to tape
 *
 * This is used during initial-run building, but not during merging.
 *
 * When alltuples = false, dump only enough tuples to get under the
 * availMem limit (and leave at least one tuple in the heap in any case,
 * since puttuple assumes it always has a tuple to compare to).  We also
 * insist there be at least one free slot in the memtuples[] array.
 *
 * When alltuples = true, dump everything currently in memory.
 * (This case is only used at end of input data.)
 *
 * If we empty the heap, close out the current run and return (this should
 * only happen at end of input data).  If we see that the tuple run number
 * at the top of the heap has changed, start a new run.
 */
    void
dumptuples_mk(Tuplesortstate_mk *state, bool alltuples)
{
    int i;
    int     bDumped = 0;
    long    spilledBytes = state->memAllowed - state->memUsed; 
    LogicalTape *lt = NULL;
    MKEntry e;

    Assert(is_under_sort_ctxt(state));

    if(alltuples && !state->mkheap)
    {
        /* Dump a sorted array.  Must be shared input that sends up here */

        /* ShareInput or sort: The sort may sort nothing, we still
         * need to handle it here 
         */
        if(state->entry_count != 0)
        {
            lt = LogicalTapeSetGetTape(state->tapeset, state->tp_tapenum[state->destTape]);
            for(i=0; i<state->entry_count; ++i)
                WRITETUP(state, lt, &state->entries[i]);
        }

        markrunend(state, state->tp_tapenum[state->destTape]);
        state->currentRun++;
        state->tp_runs[state->destTape]++;
        state->tp_dummy[state->destTape]--; /* per Alg D step D2 */
        return;
    }

    /* OK.  Normal case */
    Assert(state->mkheap);
    while (1)
    {
        if (mkheap_empty(state->mkheap)) 
        {
            markrunend(state, state->tp_tapenum[state->destTape]);
            state->currentRun++;
            state->tp_runs[state->destTape]++;
            state->tp_dummy[state->destTape]--; /* per Alg D step D2 */

            break;
        }

        if (!bDumped) 
            bDumped = 1;

        /*
         * Dump the heap's frontmost entry, and sift up to remove it from the
         * heap.
         */
        Assert(!mkheap_empty(state->mkheap)); 
        lt = LogicalTapeSetGetTape(state->tapeset, state->tp_tapenum[state->destTape]);

        mke_set_empty(&e);
        mke_set_run(&e, state->currentRun+1);
        mkheap_puget(state->mkheap, &e);

        Assert(!mke_is_empty(&e));

        WRITETUP(state, lt, &e); 

        /*
         * If the heap is empty *or* top run number has changed, we've
         * finished the current run.
         */
        if(mkheap_empty(state->mkheap) || !mkheap_run_match(state->mkheap, state->currentRun))
        {
#ifdef USE_ASSERT_CHECKING
            mkheap_verify_heap(state->mkheap, 0);
#endif
            markrunend(state, state->tp_tapenum[state->destTape]);
            state->currentRun++;
            state->tp_runs[state->destTape]++;
            state->tp_dummy[state->destTape]--; /* per Alg D step D2 */

            if (trace_sort)
                PG_TRACE3(tuplesort__dumptuples, state->entry_count, state->currentRun, state->destTape);

            /*
             * Done if heap is empty, else prepare for new run.
             */
            if (mkheap_empty(state->mkheap)) 
                break;

            Assert(mkheap_run_match(state->mkheap, state->currentRun));
            selectnewtape_mk(state);
        }
    }

    /* CDB: Accumulate total size of spilled tuples. */
    spilledBytes = state->memAllowed - state->memUsed - spilledBytes;
    if (spilledBytes > 0)
        state->spilledBytes += spilledBytes;

    if(state->gpmon_pkt)
        tuplesort_checksend_gpmonpkt(state->gpmon_pkt, state->gpmon_sort_tick);
}

/*
 * Put pos at the begining of the tuplesort.  Create pos->work_tape if necessary
 */
    void
tuplesort_rescan_pos_mk(Tuplesortstate_mk *state, TuplesortPos_mk *pos)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

    Assert(state->randomAccess);

    switch (state->status)
    {
        case TSS_SORTEDINMEM:
            pos->current = 0;
            pos->eof_reached = false;
            pos->markpos.mempos = 0; 
            pos->markpos_eof = false;
            pos->cur_work_tape = NULL;
            break;
        case TSS_SORTEDONTAPE:
            if(pos == &state->pos)
            {
                Assert(pos->cur_work_tape == NULL);
                LogicalTapeRewind(state->tapeset, state->result_tape, false);
            }
            else
            {
                if(pos->cur_work_tape == NULL)
                    pos->cur_work_tape = state->result_tape;

                LogicalTapeRewind(state->tapeset, pos->cur_work_tape, false);
            }

            pos->eof_reached = false;
            pos->markpos.tapepos.blkNum = 0L; 
            pos->markpos.tapepos.offset = 0;
            pos->markpos_eof = false;
            break;
        default:
            elog(ERROR, "invalid tuplesort state");
            break;
    }

    MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_rescan		- rewind and replay the scan
 */

void tuplesort_rescan_mk(Tuplesortstate_mk *state)
{
    tuplesort_rescan_pos_mk(state, &state->pos);
}

/*
 * tuplesort_markpos	- saves current position in the merged sort file
 */
    void
tuplesort_markpos_pos_mk(Tuplesortstate_mk *state, TuplesortPos_mk *pos)
{
    LogicalTape *work_tape = NULL;
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

    Assert(state->randomAccess);

    switch (state->status)
    {
        case TSS_SORTEDINMEM:
            pos->markpos.mempos = pos->current;
            pos->markpos_eof = pos->eof_reached;
            break;
        case TSS_SORTEDONTAPE:
            AssertEquivalent(pos == &state->pos, pos->cur_work_tape == NULL);	
            work_tape = pos->cur_work_tape == NULL ? state->result_tape : pos->cur_work_tape;
            LogicalTapeTell(state->tapeset, work_tape, &pos->markpos.tapepos);
            pos->markpos_eof = pos->eof_reached;
            break;
        default:
            elog(ERROR, "invalid tuplesort state");
            break;
    }

    MemoryContextSwitchTo(oldcontext);
}
    void 
tuplesort_markpos_mk(Tuplesortstate_mk *state)
{
    tuplesort_markpos_pos_mk(state, &state->pos);
}

/*
 * tuplesort_restorepos - restores current position in merged sort file to
 *						  last saved position
 */
    void
tuplesort_restorepos_pos_mk(Tuplesortstate_mk *state, TuplesortPos_mk *pos)
{
    MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

    Assert(state->randomAccess);

    switch (state->status)
    {
        case TSS_SORTEDINMEM:
            pos->current = pos->markpos.mempos; 
            pos->eof_reached = pos->markpos_eof;
            break;
        case TSS_SORTEDONTAPE:
            AssertEquivalent(pos == &state->pos, pos->cur_work_tape == NULL);	
            {
                LogicalTape *work_tape = pos->cur_work_tape == NULL ? state->result_tape : pos->cur_work_tape;
                bool fSeekOK = LogicalTapeSeek(state->tapeset, work_tape, &pos->markpos.tapepos);

                if(!fSeekOK)
                    elog(ERROR, "tuplesort_restorepos failed");
                pos->eof_reached = pos->markpos_eof;
            }
            break;
        default:
            elog(ERROR, "invalid tuplesort state");
            break;
    }

    MemoryContextSwitchTo(oldcontext);
}
    void 
tuplesort_restorepos_mk(Tuplesortstate_mk *state)
{
    tuplesort_restorepos_pos_mk(state, &state->pos);
}

/*
 * Tape interface routines
 */

    static uint32 
getlen(Tuplesortstate_mk *state, TuplesortPos_mk *pos, LogicalTape *lt, bool eofOK)
{
    uint32 len;
    size_t readSize;

    Assert(lt);
    readSize = LogicalTapeRead(state->tapeset, lt, (void *)&len, sizeof(len));

    if(readSize != sizeof(len))
    {
        Assert(!"Catch me");
        elog(ERROR, "unexpected end of tape");
    }

    if (len == 0 && !eofOK)
        elog(ERROR, "unexpected end of data");
    return len;
}

    static void
markrunend(Tuplesortstate_mk *state, int tapenum)
{
    uint32 len = 0;
    LogicalTape *lt = LogicalTapeSetGetTape(state->tapeset, tapenum);

    LogicalTapeWrite(state->tapeset, lt, (void *) &len, sizeof(len));
}


/*
 * Inline-able copy of FunctionCall2() to save some cycles in sorting.
 */
    static inline Datum
myFunctionCall2(FmgrInfo *flinfo, Datum arg1, Datum arg2)
{
    FunctionCallInfoData fcinfo;
    Datum		result;

    InitFunctionCallInfoData(fcinfo, flinfo, 2, NULL, NULL);

    fcinfo.arg[0] = arg1;
    fcinfo.arg[1] = arg2;
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;

    result = FunctionCallInvoke(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

    return result;
}

/*
 * Apply a sort function (by now converted to fmgr lookup form)
 * and return a 3-way comparison result.  This takes care of handling
 * NULLs and sort ordering direction properly.
 */
    static inline int32
inlineApplySortFunction(FmgrInfo *sortFunction, SortFunctionKind kind,
        Datum datum1, bool isNull1,
        Datum datum2, bool isNull2)
{
    switch (kind)
    {
        case SORTFUNC_LT:
            if (isNull1)
            {
                if (isNull2)
                    return 0;
                return 1;		/* NULL sorts after non-NULL */
            }
            if (isNull2)
                return -1;
            if (DatumGetBool(myFunctionCall2(sortFunction, datum1, datum2)))
                return -1;		/* a < b */
            if (DatumGetBool(myFunctionCall2(sortFunction, datum2, datum1)))
                return 1;		/* a > b */
            return 0;

        case SORTFUNC_REVLT:
            /* We reverse the ordering of NULLs, but not the operator */
            if (isNull1)
            {
                if (isNull2)
                    return 0;
                return -1;		/* NULL sorts before non-NULL */
            }
            if (isNull2)
                return 1;
            if (DatumGetBool(myFunctionCall2(sortFunction, datum1, datum2)))
                return -1;		/* a < b */
            if (DatumGetBool(myFunctionCall2(sortFunction, datum2, datum1)))
                return 1;		/* a > b */
            return 0;

        case SORTFUNC_CMP:
            if (isNull1)
            {
                if (isNull2)
                    return 0;
                return 1;		/* NULL sorts after non-NULL */
            }
            if (isNull2)
                return -1;
            return DatumGetInt32(myFunctionCall2(sortFunction,
                        datum1, datum2));

        case SORTFUNC_REVCMP:
            if (isNull1)
            {
                if (isNull2)
                    return 0;
                return -1;		/* NULL sorts before non-NULL */
            }
            if (isNull2)
                return 1;
            return -DatumGetInt32(myFunctionCall2(sortFunction,
                        datum1, datum2));

        default:
            elog(ERROR, "unrecognized SortFunctionKind: %d", (int) kind);
            return 0;			/* can't get here, but keep compiler quiet */
    }
}

    static void
copytup_heap(Tuplesortstate_mk *state, MKEntry *e, void *tup)
{
    /*
     * We expect the passed "tup" to be a TupleTableSlot, and form a
     * MinimalTuple using the exported interface for that.
     */
    TupleTableSlot *slot = (TupleTableSlot *) tup;

#if 0
    /* copy the tuple into sort storage */
    /* XXX BUG BUG:
     * Slot mismatch.  slot is from outer plan, but we will
     * access using state->mt_bind
     */
    stup->tuple = ExecCopySlotMemTuple(slot);
#else
    slot_getallattrs(slot);
    e->ptr = (void *) memtuple_form_to(state->mt_bind, 
            slot_get_values(slot),
            slot_get_isnull(slot),
            NULL, NULL, false
            );
#endif
    USEMEM(state, GetMemoryChunkSpace(e->ptr)); 
    Assert(state->mt_bind);
}

/*
 * Since MinimalTuple already has length in its first word, we don't need
 * to write that separately.
 */
    static void
writetup_heap(Tuplesortstate_mk *state, LogicalTape *lt, MKEntry *e)
{
    uint32 tuplen = memtuple_get_size(e->ptr, NULL); 
    LogicalTapeWrite(state->tapeset, lt, e->ptr, tuplen);

    if (state->randomAccess)	/* need trailing length word? */
        LogicalTapeWrite(state->tapeset, lt, (void *) &tuplen, sizeof(tuplen));

    if (state->gpmon_pkt)
    {
        Gpmon_M_Incr(state->gpmon_pkt, GPMON_SORT_SPILLTUPLE);
        Gpmon_M_Add(state->gpmon_pkt, GPMON_SORT_SPILLBYTE, tuplen);
        Gpmon_M_Incr(state->gpmon_pkt, GPMON_SORT_CURRSPILLPASS_TUPLE);
        Gpmon_M_Add(state->gpmon_pkt, GPMON_SORT_CURRSPILLPASS_BYTE, tuplen);
    }

    FREEMEM(state, GetMemoryChunkSpace(e->ptr)); 
    pfree(e->ptr); 
    e->ptr = NULL;
}

    static void
readtup_heap(Tuplesortstate_mk *state, TuplesortPos_mk *pos, MKEntry *e, LogicalTape *lt, uint32 len)
{
    uint32 tuplen;
    size_t readSize;

    Assert(is_under_sort_ctxt(state));

    MemSet(e, 0, sizeof(MKEntry));
    e->ptr = palloc(memtuple_size_from_uint32(len));

    USEMEM(state, GetMemoryChunkSpace(e->ptr)); 
    memtuple_set_mtlen((MemTuple)e->ptr, NULL, len);

    Assert(lt);
    readSize = LogicalTapeRead(state->tapeset, lt,
            (void *) ((char *) e->ptr + sizeof(uint32)),
            memtuple_size_from_uint32(len) - sizeof(uint32));

    if (readSize != (size_t) (memtuple_size_from_uint32(len) - sizeof(uint32))) 
        elog(ERROR, "unexpected end of data");

    if (state->randomAccess)	/* need trailing length word? */
    {
        readSize = LogicalTapeRead(state->tapeset, lt, (void *)&tuplen, sizeof(tuplen));

        if(readSize != sizeof(tuplen))
            elog(ERROR, "unexpected end of data");
    }

    /* For shareinput on sort, the reader will not set mt_bind.  In this case,
     * we will not call compare.
     */
    AssertImply(!state->mt_bind, state->status == TSS_SORTEDONTAPE);
}

    static void
copytup_index(Tuplesortstate_mk *state, MKEntry *e, void *tup)
{
    IndexTuple	tuple = (IndexTuple) tup;
    uint32 tuplen = IndexTupleSize(tuple);

    /* copy the tuple into sort storage */
    e->ptr = palloc(tuplen);
    memcpy(e->ptr, tuple, tuplen);
    USEMEM(state, GetMemoryChunkSpace(e->ptr)); 
}

    static void
writetup_index(Tuplesortstate_mk *state, LogicalTape *lt, MKEntry *e) 
{
    IndexTuple	tuple = (IndexTuple) e->ptr; 
    uint32 tuplen;

    tuplen = IndexTupleSize(tuple) + sizeof(tuplen);

    LogicalTapeWrite(state->tapeset, lt, (void *) &tuplen, sizeof(tuplen));
    LogicalTapeWrite(state->tapeset, lt, (void *) tuple, IndexTupleSize(tuple));

    if (state->randomAccess)	/* need trailing length word? */
        LogicalTapeWrite(state->tapeset, lt, (void *) &tuplen, sizeof(tuplen));

    FREEMEM(state, GetMemoryChunkSpace(tuple));
    pfree(tuple);
    e->ptr = NULL;
}

    static void
readtup_index(Tuplesortstate_mk *state, TuplesortPos_mk *pos, MKEntry *e, 
        LogicalTape *lt, uint32 len)
{
    uint32 tuplen = len - sizeof(uint32);
    IndexTuple	tuple = (IndexTuple) palloc(tuplen);
    size_t readSize;

    Assert(lt); 
    USEMEM(state, GetMemoryChunkSpace(tuple));

    MemSet(e, 0, sizeof(MKEntry));
    readSize = LogicalTapeRead(state->tapeset, lt, (void *)tuple, tuplen);

    if(readSize != tuplen)
        elog(ERROR, "unexpected end of data");

    if (state->randomAccess)	/* need trailing length word? */
    {
        readSize = LogicalTapeRead(state->tapeset, lt, (void *)&tuplen, sizeof(tuplen));

        if (readSize != sizeof(tuplen))
            elog(ERROR, "unexpected end of data");
    }
    e->ptr = (void *) tuple;
}


    static void
copytup_datum(Tuplesortstate_mk *state, MKEntry *e, void *tup)
{
    /* Not currently needed */
    elog(ERROR, "copytup_datum() should not be called");
}

    static void
writetup_datum(Tuplesortstate_mk *state, LogicalTape *lt, MKEntry *e) 
{
    void	   *waddr;
    uint32 tuplen;
    uint32 writtenlen;
    bool needfree = false;

    if (mke_is_null(e))
    {
        waddr = NULL;
        tuplen = 0;
    }
    else if (state->datumTypeByVal)
    {
        waddr = &e->d; 
        tuplen = sizeof(Datum);
    }
    else
    {
        waddr = DatumGetPointer(e->d); 
        tuplen = datumGetSize(e->d, false, state->datumTypeLen);
        needfree = true;
        Assert(tuplen != 0);
    }

    writtenlen = tuplen + sizeof(uint32); 

    LogicalTapeWrite(state->tapeset, lt, (void *) &writtenlen, sizeof(writtenlen));
    LogicalTapeWrite(state->tapeset, lt, waddr, tuplen);

    if (state->randomAccess)	/* need trailing length word? */
        LogicalTapeWrite(state->tapeset, lt, (void *) &writtenlen, sizeof(writtenlen));

    if (needfree) 
    {
        FREEMEM(state, tuplen); 
        pfree(waddr); 
    }
}

    static void
readtup_datum(Tuplesortstate_mk *state, TuplesortPos_mk *pos, MKEntry *e, 
        LogicalTape *lt, uint32 len)
{
    size_t readSize; 
    uint32 tuplen = len - sizeof(uint32); 

    Assert(lt); 

    MemSet(e, 0, sizeof(MKEntry));

    if (tuplen == 0)
        mke_set_null(e, state->nullfirst); 
    else if (state->datumTypeByVal)
    {
        mke_set_not_null(e);

        Assert(tuplen == sizeof(Datum));
        readSize = LogicalTapeRead(state->tapeset, lt, (void *)&e->d, tuplen);

        if (readSize != tuplen) 
            elog(ERROR, "unexpected end of data");
    }
    else
    {
        void	   *raddr = palloc(tuplen);

        readSize = LogicalTapeRead(state->tapeset, lt, raddr, tuplen);

        if(readSize != tuplen)
            elog(ERROR, "unexpected end of data");

        mke_set_not_null(e);
        e->d = PointerGetDatum(raddr); 
        USEMEM(state, GetMemoryChunkSpace(raddr));
    }

    if (state->randomAccess)	/* need trailing length word? */
    {
        readSize = LogicalTapeRead(state->tapeset, lt, (void *)&tuplen, sizeof(tuplen));

        if (readSize != sizeof(tuplen)) 
            elog(ERROR, "unexpected end of data");
    }
}

typedef struct refcnt_locale_str
{
    int ref;
    int xfrm_pos;
    char data[1];
} refcnt_locale_str;

typedef struct refcnt_locale_str_k
{
    int ref;
    int xfrm_pos;
    char data[1024];
} refcnt_locale_str_k;


static int tupsort_compare_datum(MKEntry *v1, MKEntry *v2, void *ctxt)
{
    TupsortMKContext *mkctxt = (TupsortMKContext *) ctxt;
    ScanKey sk = mkctxt->tsstate->cmpScanKey + mkctxt->sknum;

    Assert(!mke_is_null(v1));
    Assert(!mke_is_null(v2));

    return inlineApplySortFunction(&sk->sk_func, mkctxt->sortfnkind, 
            v1->d, false,
            v2->d, false
            ); 
}

static void tupsort_cpfr(MKEntry *dst, MKEntry *src, void *ctxt) 
{
    TupsortMKContext *mkctxt = (TupsortMKContext *) ctxt;

    Assert(dst);
    if(mke_is_refc(dst))
    {
        Assert(dst->d != 0);
        Assert(!mke_is_copied(dst));
        tupsort_refcnt(DatumGetPointer(dst->d), -1);
    }

    if(mke_is_copied(dst)) 
    {
        Assert(dst->d != 0);
        Assert(!mke_is_refc(dst));
        pfree(DatumGetPointer(dst->d));
    }

    mke_clear_refc_copied(dst);

    if(src)
    {
        *dst = *src;
        if(!mke_is_null(src))
        {
            if(mke_is_refc(src)) 
                tupsort_refcnt(DatumGetPointer(dst->d), 1);
            else if(!mkctxt->mk_common.typByVal)
            {
                Assert(src->d != 0);
                dst->d = datumCopy(src->d, mkctxt->mk_common.typByVal, mkctxt->mk_common.typLen);
                mke_set_copied(dst);
            }
        }
    }
}

static void tupsort_refcnt(void *vp, int ref)
{
    refcnt_locale_str *p = (refcnt_locale_str *) vp;

    Assert(p && p->ref > 0);
    Assert(ref == 1 || ref == -1);

    if(ref == 1) 
        ++p->ref;
    else
    {
        if(--p->ref == 0)
            pfree(p);
    }
}

static int tupsort_compare_int32(MKEntry *v1, MKEntry *v2, void *ctxt)
{
    int result = 0;
    TupsortMKContext *mkctxt = (TupsortMKContext *) ctxt;

    int32 i1 = DatumGetInt32(v1->d);
    int32 i2 = DatumGetInt32(v2->d);

    Assert(!mke_is_null(v1));
    Assert(!mke_is_null(v2));

    result = (i1 < i2) ? -1 : ((i1 == i2) ? 0 : 1);

    return (mkctxt->sortfnkind == SORTFUNC_CMP) ? result : -result;
}

static int tupsort_compare_char(MKEntry *v1, MKEntry *v2, void *ctxt)
{
    int result = 0;
    TupsortMKContext *mkctxt = (TupsortMKContext *) ctxt;

    refcnt_locale_str *p1 = (refcnt_locale_str *) DatumGetPointer(v1->d);
    refcnt_locale_str *p2 = (refcnt_locale_str *) DatumGetPointer(v2->d);

    Assert(!mke_is_null(v1));
    Assert(!mke_is_null(v2));

    Assert(!lc_collate_is_c());
    Assert(mkctxt->sortfnkind == SORTFUNC_CMP || mkctxt->sortfnkind == SORTFUNC_REVCMP);

    Assert(p1->ref > 0 && p2->ref > 0);

    if(p1 == p2)
    {
        Assert(p1->ref >= 2);
        result = 0;
    }
    else
    {
        result = strcmp(p1->data + p1->xfrm_pos, p2->data + p2->xfrm_pos); 

        /* 
         * See varstr_cmp for comment on comparing str with locale.
         * Essentially, for some locale, strcoll may return eq even if
         * original str are different.  
         */
        if(result == 0)
            result = strcmp(p1->data, p2->data); 

        if(result == 0)
        {
            if(p1->ref >= p2->ref)
            {
                v2->d = v1->d;
                tupsort_refcnt(p1, 1); 
                tupsort_refcnt(p2, -1); 
            }
            else
            {
                v1->d = v2->d;
                tupsort_refcnt(p2, 1);
                tupsort_refcnt(p1, -1);
            }
        }
    }

    return (mkctxt->sortfnkind == SORTFUNC_CMP) ? result : -result;
}

static void tupsort_prepare_mtup(MKEntry *a, void *ctxt)
{
    TupsortMKContext *mkctxt = (TupsortMKContext *) ctxt;
    bool isnull;

    Assert(is_under_sort_ctxt(mkctxt->tsstate));

    tupsort_cpfr(a, NULL, mkctxt); 

    a->d = memtuple_getattr(
            (MemTuple) a->ptr, 
            mkctxt->tsstate->mt_bind, 
            mkctxt->attno,
            &isnull
            );

    if(!isnull)
        mke_set_not_null(a); 
    else 
        mke_set_null(a, mkctxt->nullfirst);
}

static void tupsort_prepare_itup(MKEntry *a, void *ctxt)
{
    TupsortMKContext *mkctxt = (TupsortMKContext *) ctxt;
    TupleDesc tupdesc = RelationGetDescr(mkctxt->tsstate->indexRel);
    bool isnull;

    Assert(is_under_sort_ctxt(mkctxt->tsstate));

    tupsort_cpfr(a, NULL, mkctxt); 

    a->d = index_getattr(
            (IndexTuple) a->ptr, 
            mkctxt->attno,
            tupdesc,
            &isnull
            );

    if(!isnull)
        mke_set_not_null(a); 
    else 
        mke_set_null(a, mkctxt->nullfirst);
}

/* "True" length (not counting trailing blanks) of a BpChar */
    static inline int
bcTruelen(char *p, int len)
{
    int			i;

    for (i = len - 1; i >= 0; i--)
    {
        if (p[i] != ' ')
            break;
    }
    return i + 1;
}

static void tupsort_prepare_char(MKEntry *a, bool isCHAR)
{
    char *p;
    void *tofree = NULL;
    int len;
    int llen;
    int retlen;

    refcnt_locale_str *ret;
    refcnt_locale_str_k kstr;
    refcnt_locale_str *pstr = (refcnt_locale_str *) &kstr;
    int avail = 1024;

    Assert(!lc_collate_is_c());

    if(mke_is_null(a))
        return;

    varattrib_untoast_ptr_len(a->d, &p, &len, &tofree);

    if(isCHAR)
        len = bcTruelen(p, len);

    if(len > avail - 2) 
    {
        pstr = (refcnt_locale_str *) palloc(offsetof(refcnt_locale_str, data) + len + 2); 
        avail = len + 2;
    }

    pstr->ref = 1;
    pstr->xfrm_pos = len+1;
    memcpy(pstr->data, p, len);
    pstr->data[len] = '\0';

    avail -= len + 1;
    Assert(avail > 0);

    llen = (int) strxfrm(pstr->data+pstr->xfrm_pos, pstr->data, avail);
    retlen = offsetof(refcnt_locale_str, data) + len + 1 + llen + 1;

    ret = (refcnt_locale_str *) palloc(retlen); 

    if(llen < avail)
    {
        memcpy(ret, pstr, retlen); 
        Assert(ret->ref == 1);
        Assert(ret->data[ret->xfrm_pos - 1] == '\0');
        Assert(ret->data[ret->xfrm_pos + llen] == '\0');
    }
    else
    {
        memcpy(ret, pstr, offsetof(refcnt_locale_str, data) + len + 1);
        avail = retlen - offsetof(refcnt_locale_str, data) - len - 1;
        Assert(avail > llen);
        Assert(ret->ref == 1);
        Assert(ret->xfrm_pos == len + 1);
        Assert(ret->data[len] == '\0');

        llen = (int) strxfrm(ret->data+ret->xfrm_pos, pstr->data, avail); 
        Assert(llen < avail); 
        Assert(ret->data[ret->xfrm_pos + llen] == '\0'); 
    }

    if(pstr != (refcnt_locale_str *) &kstr)
        pfree(pstr);

    if(tofree)
        pfree(tofree);

    a->d = PointerGetDatum(ret);
    mke_set_refc(a);
}

static void tupsort_prepare_mtup_char(MKEntry *a, void *ctxt)
{
    tupsort_prepare_mtup(a, ctxt);
    tupsort_prepare_char(a, true);
}

static void tupsort_prepare_mtup_text(MKEntry *a, void *ctxt)
{
    tupsort_prepare_mtup(a, ctxt);
    tupsort_prepare_char(a, false);
}

static void tupsort_prepare_itup_char(MKEntry *a, void *ctxt)
{
    tupsort_prepare_itup(a, ctxt);
    tupsort_prepare_char(a, true);
}

static void tupsort_prepare_itup_text(MKEntry *a, void *ctxt)
{
    tupsort_prepare_itup(a, ctxt);
    tupsort_prepare_char(a, false);
}


static void tuplesort_limit_insert(Tuplesortstate_mk *state, MKEntry *e)
{
    int i;

    Assert(state->mkctxt.limit > 0);
    Assert(is_under_sort_ctxt(state));
    Assert(!mke_is_empty(e));


    if(!state->mkheap)
    {
        if(state->entry_count < state->mkctxt.limit)
        {
            state->entries[state->entry_count++] = *e;
            return;
        }

        state->mkheap = mkheap_from_array(state->entries, 
                state->entry_allocsize, 
                state->entry_count,
                &state->mkctxt);
        state->entries = NULL;
        state->entry_allocsize = 0;
        state->entry_count = 0;
    }

    Assert(state->mkheap && !mkheap_empty(state->mkheap));
    Assert(state->mkheap->count == state->mkctxt.limit);
    i = mkheap_puget(state->mkheap, e); 
    tupsort_cpfr(e, NULL, mk_get_ctxt(&state->mkctxt, mke_get_lv(e)));
    Assert(state->mkheap->count == state->mkctxt.limit);

    Assert(!mke_is_empty(e));
    Assert(e->ptr);
    pfree(e->ptr);
} 

static void tuplesort_heap_insert(Tuplesortstate_mk *state, MKEntry *e)
{
    int ins;
    Assert(state->mkheap);
    Assert(is_under_sort_ctxt(state));

    ins = mkheap_puget_run(state->mkheap, e, state->currentRun);
    tupsort_cpfr(e, NULL, mk_get_ctxt(&state->mkctxt, mke_get_lv(e)));

    Assert(ins >= 0);

    {
        LogicalTape *lt = LogicalTapeSetGetTape(state->tapeset, state->tp_tapenum[state->destTape]);

        WRITETUP(state, lt, e);

        if(!mkheap_run_match(state->mkheap, state->currentRun))
        {
            markrunend(state, state->tp_tapenum[state->destTape]);
            state->currentRun++;
            state->tp_runs[state->destTape]++;
            state->tp_dummy[state->destTape]--;

            selectnewtape_mk(state);
        }
    }
}

static void tuplesort_limit_sort(Tuplesortstate_mk *state)
{
    Assert(state->mkctxt.limit > 0);
    Assert(is_under_sort_ctxt(state));

    if(!state->mkheap)
    {
        Assert(state->entry_count <= state->mkctxt.limit);
        mk_qsort(state->entries, state->entry_count, &state->mkctxt);
        return;
    }
    else
    {
        int i;

#ifdef USE_ASSERT_CHECKING
        mkheap_verify_heap(state->mkheap, 0);
#endif
        state->entry_allocsize = mkheap_cnt(state->mkheap);
        state->entries = palloc(state->entry_allocsize * sizeof (MKEntry));

        /* Put these things to minimum */
        for(i=0; i<state->entry_allocsize; ++i)
            mke_set_comp_max(state->entries+i); 

        for(i=state->entry_allocsize-1; i>=0; --i)
            mkheap_puget(state->mkheap, state->entries+i);

        Assert(mkheap_cnt(state->mkheap) == 0);
        state->entry_count = state->entry_allocsize;
    }

    mkheap_destroy(state->mkheap);
    state->mkheap = NULL;
}

    void 
tuplesort_set_gpmon_mk(Tuplesortstate_mk *state, gpmon_packet_t *gpmon_pkt, int *gpmon_tick)
{
    state->gpmon_pkt = gpmon_pkt;
    state->gpmon_sort_tick = gpmon_tick;
}


