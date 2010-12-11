/*
 * testutils.c
 * 		Implementation of testing utilities.
 *
 * Copyright (c) 2010, Greenplum inc
 */

#include <sys/time.h>
#include <sys/resource.h>

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/elog.h"
#include "utils/testutils.h"

#ifdef USE_TEST_UTILS

#define MAX_FRAME_DEPTH   64
#define USECS_IN_MSEC     1000
#define MSECS_IN_SEC      1000

/* holds return addresses of frames in stack */
static void *stackAddressesLastCFI[MAX_FRAME_DEPTH];

/* stack depth */
static uint32 stackDepthLastCFI = 0;

/* elapsed user time */
static struct timeval userTimeLastCFI;

/* static functions */
static int32 timeElapsedMs(struct timeval *tvLast, struct timeval *tvFirst);


/*
 * Reset time slice
 */
void
TimeSliceReset()
{
	struct rusage ru;

	if (0 != getrusage(RUSAGE_SELF, &ru))
	{
		elog(ERROR, "Time slicer: Failed to retrieve user time");
	}

	userTimeLastCFI = ru.ru_utime;
	stackDepthLastCFI = gp_backtrace(stackAddressesLastCFI, MAX_FRAME_DEPTH);
}


/*
 * Check if time slice since last check-for-interrupts (CFI) has been exceeded
 */
void
TimeSliceCheck(const char *file, int line)
{
	Assert(gp_test_time_slice);

	struct rusage ru;
	int32 elapsedMs = 0;

	/* CFI is disabled inside critical sections */
	if (0 != InterruptHoldoffCount || 0 != CritSectionCount)
	{
		return;
	}

	/* get current user time */
	if (getrusage(RUSAGE_SELF, &ru) != 0)
	{
		elog(ERROR, "Time slicer: Failed to retrieve user time");
	}

	elapsedMs = timeElapsedMs(&ru.ru_utime, &userTimeLastCFI);

	Assert(0 <= elapsedMs);

	/* check elapsed time since last CFI  */
	if (gp_test_time_slice_interval < elapsedMs)
	{
		void *stackAddressesCurrent[MAX_FRAME_DEPTH];
		uint32 stackDepthCurrent = gp_backtrace(stackAddressesCurrent, MAX_FRAME_DEPTH);

		char *stackTraceLastCFI = gp_stacktrace(stackAddressesLastCFI, stackDepthLastCFI);
		char *stackTraceCurrent = gp_stacktrace(stackAddressesCurrent, stackDepthCurrent);

		/* report time slice violation error */
		ereport(ERROR,
				(errmsg("Time slice of %d ms exceeded at (%s:%d), last CFI before %d ms.\n"
						"Stack trace of last CFI:\n%s\n"
						"Current stack trace:\n%s\n",
						gp_test_time_slice_interval,
						file,
						line,
						elapsedMs,
						stackTraceLastCFI,
						stackTraceCurrent)));
	}

	/* reset time slice */
	userTimeLastCFI = ru.ru_utime;
	stackDepthLastCFI = gp_backtrace(stackAddressesLastCFI, MAX_FRAME_DEPTH);
}


/*
 * Get elapsed time between two timestamps in ms
 */
static int32
timeElapsedMs(struct timeval *tvLast, struct timeval *tvFirst)
{
	return (tvLast->tv_sec - tvFirst->tv_sec) * MSECS_IN_SEC +
			(tvLast->tv_usec - tvFirst->tv_usec) / USECS_IN_MSEC;
}


#ifdef USE_TEST_UTILS_X86

/*
 * check if lightweight lock(s) are held;
 * print stack trace where lock(s) got acquired and error out;
 */
void
LWLockHeldDetect(const char *file, int line)
{
	Assert(gp_test_deadlock_hazard);

	if (LWLocksHeld())
	{
		void *stackAddressesCurrent[MAX_FRAME_DEPTH];
		uint32 stackDepthCurrent = gp_backtrace(stackAddressesCurrent, MAX_FRAME_DEPTH);
		char *stackTraceCurrent = gp_stacktrace(stackAddressesCurrent, stackDepthCurrent);

		const char *stackTraces = LWLocksHeldStackTraces();
		Assert(NULL != stackTraces);

		/* report time slice violation error */
		ereport(ERROR,
				(errmsg("Attempt to acquire database lock at (%s:%d) while holding lightweight lock(s).\n"
						"Stack trace(s) where lightweight lock(s) got acquired:\n%s\n"
						"Current stack trace:\n%s\n",
						file,
						line,
						stackTraces,
						stackTraceCurrent)));
	}
}

#endif /* USE_TEST_UTILS_X86 */

#endif /* USE_TEST_UTILS */

/* EOF */
