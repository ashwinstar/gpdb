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
 *    This function returns the time in the monotonic order.
 *
 * The new time is stored in time->endTime, which has a larger value than
 * the original value. The original endTime is copied to beginTime, whose
 * original value is overwritten.
 *
 * This function is intended for computing elapsed time between two
 * calls. It is not for getting the system time.
 */
void
gp_get_monotonic_time(GpMonotonicTime *time)
{
	time->beginTime.tv_sec = time->endTime.tv_sec;
	time->beginTime.tv_usec = time->endTime.tv_usec;

	bool useBeginTime = false;

#if HAVE_LIBRT
	/* Use clock_gettime to return monotonic time value. */
	struct timespec ts;
	int status = clock_gettime(CLOCK_MONOTONIC, &ts);

	struct timeval newTime;
	newTime.tv_sec = ts.tv_sec;
	newTime.tv_usec = ts.tv_nsec / 1000;

	if (status == 0 &&
		timeCmp(&time->beginTime, &newTime) < 0)
	{
		time->endTime.tv_sec = newTime.tv_sec;
		time->endTime.tv_usec = newTime.tv_usec;
	} else
	{
		useBeginTime = true;
	}

#else

	gettimeofday(&time->endTime, NULL);

	if (timeCmp(&time->beginTime, &time->endTime) >= 0)
	{
		useBeginTime = true;
	}
#endif

	if (useBeginTime)
	{
		time->endTime.tv_usec = time->beginTime.tv_usec + 1;

		time->endTime.tv_sec = time->beginTime.tv_sec +
			(time->endTime.tv_usec / 1000000);
		time->endTime.tv_usec = time->endTime.tv_usec % 1000000;
	}
	
}
