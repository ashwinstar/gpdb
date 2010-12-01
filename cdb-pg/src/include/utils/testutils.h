/*
 * testutils.h
 *		Collection of testing utilities.
 *
 * Copyright (c) 2010, Greenplum inc
 */

#ifndef TESTUTILS_H_
#define TESTUTILS_H_

#ifdef USE_TEST_UTILS

#define CHECK_TIME_SLICE()      TimeSliceCheck(__FILE__, __LINE__)
#define CHECK_LW_LOCKS_HELD()   LWLockHeldDetect(__FILE__, __LINE__)

/* external variables */
extern PGDLLIMPORT volatile int32 InterruptHoldoffCount;
extern PGDLLIMPORT volatile int32 CritSectionCount;

/* time slicing */
extern void TimeSliceReset(void);
extern void TimeSliceCheck(const char *file, int line);

/* detect database-lightweight lock conflict */
extern void LWLockHeldDetect(const char *file, int line);

/* GUCs */
extern bool gp_test_time_slice;           /* session GUC, controls time slice violation checking */
extern int  gp_test_time_slice_interval;  /* session GUC, sets time slice interval in ms */
extern bool gp_test_deadlock_hazard;       /* session GUC, controls database-lightweight lock
							               * conflict detection */

#endif /* USE_TEST_UTILS */

#endif /* TESTUTILS_H_ */


/* EOF */
