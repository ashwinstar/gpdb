/*
 * gptime
 *   define time-related functions used in GPDB
 * 
 * Copyright (c), EMC DCD (Greenplum)
 *
 * IDENTIFICATION
 *   $Id$
 */
#include "c.h"

#include <sys/time.h>

#ifdef HAVE_LIBRT
#include <time.h>
#endif

#include "pgtime.h"


/*
 * gp_get_monotonic_time
 *    This function makes the best effort to return the time
 * in the monotonic order.
 *
 * This function returns 0 on success, and -1 on failure.
 *
 * This function is intended for computing elapsed time between two
 * calls. It is not for getting the system time.
 */
int
gp_get_monotonic_time(struct timeval *restrict tp)
{
#if HAVE_LIBRT
	/* Use clock_gettime to return monotonic time value. */
	struct timespec ts;
	int status = clock_gettime(CLOCK_MONOTONIC, &ts);
	
	/*
	 * If the status is -1, we set ts to the maximum value for
	 * the time.
	 */
	if (status == -1)
	{
		tp->tv_sec = 0x3fffffff;
		tp->tv_usec = 0x3fffffff;
	}
	else
	{
		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
	}

	return status;
#else

	return gettimeofday(tp, NULL);
	
#endif
}
