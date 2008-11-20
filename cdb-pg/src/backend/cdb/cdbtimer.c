/*-------------------------------------------------------------------------
 *
 * cdbtimer.c
 *	  Functions to manipulate timers used in a backend.
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include <stddef.h>

#include "postgres.h"
#include "libpq/pqsignal.h"
#include "cdb/cdbtimer.h"


/*
 * resetTimers - disable process interval timers
 *
 * Testing has shown that fork on some systems doesn't clear timers as it should and
 * exec is subjected to *fatal* interruption by timers so the timers are disabled
 * before the fork is done.  Before resetting the interval timers, the all signals
 * are blocked.  After resetting the interval timers, the signal mask is restored.
 *
 * otimers: a pointer to an itmers struct to contain the old timer values.  If
 * 		NULL, the old timer valuess are not preserved.
 *
 * Returns -1 if an error is encountered; 0 if successful.
 */
int
resetTimers(struct itimers *otimers)
{
	struct itimerval xtimer;				/* Zero timer for disabling */
	struct itimerval *p_rtimer = NULL;		/* ITIMER_REAL */
	struct itimerval *p_vtimer = NULL;		/* ITIMER_VIRTUAL */
	struct itimerval *p_ptimer = NULL;		/* ITIMER_PROF */

	int rc = -1;

	if (otimers != NULL)
	{
		p_rtimer = &otimers->rtimer;
		p_vtimer = &otimers->vtimer;
		p_ptimer = &otimers->ptimer;
	}

	/*
	 * Block signals while capturing timers.
	 */
	PG_SETMASK(&BlockSig);

	/*
	 * Disable all process interval timers preserving the old
	 * values if requested.
	 */
	timerclear(&xtimer.it_interval);
	timerclear(&xtimer.it_value);

	if (setitimer(ITIMER_REAL, &xtimer, p_rtimer) < 0)
	{
		elog( ERROR, "Unable to fetch/clear ITIMER_REAL" );
	}
	else if (setitimer(ITIMER_VIRTUAL, &xtimer, p_vtimer) < 0)
	{
		elog( ERROR, "Unable to fetch/clear ITIMER_VIRTUAL" );
	}
	else if (setitimer(ITIMER_REAL, &xtimer, p_ptimer) < 0)
	{
		elog( ERROR, "Unable to fetch/clear ITIMER_PROF" );
	}
	else
	{
		rc = 0;
	}

	/*
	 * Restore signal mask.
	 */
	PG_SETMASK(&UnBlockSig);

	return rc;
}


/*
 * restoreTimers - restore timers from itimers struct.
 *
 * Restores the process interval timers stored in the itimers struct
 * by resetTimers.
 *
 * timers: a reference to the itimers struct passed to the resetTimers
 * 		function.
 *
 * Returns -1 if an error is encountered; 0 if successful.
 */
int
restoreTimers(const struct itimers *timers)
{
	if (timers == NULL)
	{
		/* Coding error! */
		elog( FATAL, "Old timer values not provided" );
		return -1;
	}

	/*
	 * Restore any active timers.
	 */
	if (timerisset(&timers->rtimer.it_interval) || timerisset(&timers->rtimer.it_value))
	{
		if (setitimer(ITIMER_REAL, &timers->rtimer, NULL) < 0)
		{
			elog( ERROR, "Unable to restore ITIMER_REAL" );
			return -1;
		}
	}
	if (timerisset(&timers->vtimer.it_interval) || timerisset(&timers->vtimer.it_value))
	{
		if (setitimer(ITIMER_VIRTUAL, &timers->vtimer, NULL) < 0)
		{
			elog( ERROR, "Unable to restore ITIMER_VIRTUAL" );
			return -1;
		}
	}
	if (timerisset(&timers->ptimer.it_interval) || timerisset(&timers->ptimer.it_value))
	{
		if (setitimer(ITIMER_PROF, &timers->ptimer, NULL) < 0)
		{
			elog( ERROR, "Unable to restore ITIMER_PROF" );
			return -1;
		}
	}

	return 0;
}

