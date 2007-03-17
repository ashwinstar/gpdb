/*-------------------------------------------------------------------------
 *
 * instrument.h
 *	  definitions for run-time statistics collection
 *
 *
 * Portions Copyright (c) 2006-2007, greenplum inc
 * Copyright (c) 2001-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/executor/instrument.h,v 1.16 2006/06/09 19:30:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include <sys/time.h>

struct CdbExplain_NodeSummary;          /* private def in cdb/cdbexplain.c */

/*
 * gettimeofday() does not have sufficient resolution on Windows,
 * so we must use QueryPerformanceCounter() instead.  These macros
 * also give some breathing room to use other high-precision-timing APIs
 * on yet other platforms.	(The macro-ization is not complete, however;
 * see subtraction code in instrument.c and explain.c.)
 */
#ifndef WIN32

typedef struct timeval instr_time;

#define INSTR_TIME_IS_ZERO(t)	((t).tv_sec == 0 && (t).tv_usec == 0)
#define INSTR_TIME_SET_ZERO(t)	((t).tv_sec = 0, (t).tv_usec = 0)
#define INSTR_TIME_SET_CURRENT(t)	gettimeofday(&(t), NULL)
#define INSTR_TIME_GET_DOUBLE(t) \
	(((double) (t).tv_sec) + ((double) (t).tv_usec) / 1000000.0)

static inline void
InstrTimeAdd(instr_time *sum, const instr_time *a, const instr_time *b)
{
	sum->tv_sec = a->tv_sec + b->tv_sec;
	sum->tv_usec = a->tv_usec + b->tv_usec;
	/* Normalize afterward to avoid overflow/underflow of tv_usec */
}

static inline void
InstrTimeSubtract(instr_time *diff, const instr_time *t1, const instr_time *t0)
{
	diff->tv_sec = t1->tv_sec - t0->tv_sec;
	diff->tv_usec = t1->tv_usec - t0->tv_usec;
	/* Normalize afterward to avoid overflow/underflow of tv_usec */
}

static inline void
InstrTimeNormalize(instr_time *t)
{
	while (t->tv_usec < 0)
	{
		t->tv_usec += 1000000;
		t->tv_sec--;
	}
	while (t->tv_usec >= 1000000)
	{
		t->tv_usec -= 1000000;
		t->tv_sec++;
	}
}

static inline void
InstrTimeAddDifference(instr_time *sum, const instr_time *t1, const instr_time *t0)
{
    sum->tv_sec += t1->tv_sec - t0->tv_sec;
    sum->tv_usec += t1->tv_usec - t0->tv_usec;
    InstrTimeNormalize(sum);
}

static inline bool
InstrTimeIsLess(const instr_time *a, const instr_time *b)
{
    return a->tv_sec < b->tv_sec ||
            (a->tv_sec == b->tv_sec &&
             a->tv_usec < b->tv_usec);
}

static inline bool
InstrTimeIsLessOrEqual(const instr_time *a, const instr_time *b)
{
    return a->tv_sec < b->tv_sec ||
            (a->tv_sec == b->tv_sec &&
             a->tv_usec <= b->tv_usec);
}

static inline bool
InstrTimeIsEqual(const instr_time *a, const instr_time *b)
{
    return a->tv_usec == b->tv_usec &&
           a->tv_sec == b->tv_sec;
}

#else							/* WIN32 */

typedef LARGE_INTEGER instr_time;

#define INSTR_TIME_IS_ZERO(t)	((t).QuadPart == 0)
#define INSTR_TIME_SET_ZERO(t)	((t).QuadPart = 0)
#define INSTR_TIME_SET_CURRENT(t)	QueryPerformanceCounter(&(t))
#define INSTR_TIME_GET_DOUBLE(t) \
	(((double) (t).QuadPart) / GetTimerFrequency())

extern double InstrTimerFrequency;

static __inline__ double
GetTimerFrequency(void)
{
    if (InstrTimerFrequency == 0.0)
    {
	    LARGE_INTEGER f;

        QueryPerformanceFrequency(&f);
	    InstrTimerFrequency = (double)f.QuadPart;
    }
    return InstrTimerFrequency;
}

static inline void
InstrTimeAdd(instr_time *sum, const instr_time *a, const instr_time *b)
{
	sum->QuadPart = a->QuadPart + b->QuadPart;
}

static inline void
InstrTimeSubtract(instr_time *diff, const instr_time *t1, const instr_time *t0)
{
	diff->QuadPart = t1->QuadPart - t0->QuadPart;
}

static inline void
InstrTimeNormalize(instr_time *t)
{}

static inline void
InstrTimeAddDifference(instr_time *sum, const instr_time *t1, const instr_time *t0)
{
    sum->QuadPart += t1->QuadPart - t0->QuadPart;
}

static inline bool
InstrTimeIsLess(const instr_time *a, const instr_time *b)
{
    return a->QuadPart < b->QuadPart;
}

static inline bool
InstrTimeIsLessOrEqual(const instr_time *a, const instr_time *b)
{
    return a->QuadPart <= b->QuadPart;
}

static inline bool
InstrTimeIsEqual(const instr_time *a, const instr_time *b)
{
    return a->QuadPart == b->QuadPart;
}

#endif   /* WIN32 */

typedef struct Instrumentation
{
	/* Info about current plan cycle: */
	bool		running;		/* TRUE if we've completed first tuple */
	instr_time	starttime;		/* Start time of current iteration of node */
	instr_time	counter;		/* Accumulated runtime for this node */
	double		firsttuple;		/* Time for first tuple of this cycle */
	double		tuplecount;		/* Tuples emitted so far this cycle */
	/* Accumulated statistics across all completed cycles: */
	double		startup;		/* Total startup time (in seconds) */
	double		total;			/* Total total time (in seconds) */
	double		ntuples;		/* Total tuples produced */
	double		nloops;			/* # of run cycles for this node */
    double      workmemused;    /* CDB: work_mem actually used (bytes) */
    double      workmemwanted;  /* CDB: work_mem to avoid scratch i/o (bytes) */
	instr_time	firststart;		/* CDB: Start time of first iteration of node */
    struct CdbExplain_NodeSummary  *cdbNodeSummary; /* stats from all qExecs */
} Instrumentation;

extern Instrumentation *InstrAlloc(int n);
extern void InstrStartNode(Instrumentation *instr);
extern void InstrStopNode(Instrumentation *instr, double nTuples);
extern void InstrEndLoop(Instrumentation *instr);

#endif   /* INSTRUMENT_H */
