/*
 * Copyright (c) 2012 EMC Corporation All Rights Reserved
 *
 * This software is protected, without limitation, by copyright law
 * and international treaties. Use of this software and the intellectual
 * property contained therein is expressly limited to the terms and
 * conditions of the License Agreement under which it is provided by
 * or on behalf of EMC.
 *
 * ---------------------------------------------------------------------
 *
 * Interfaces to performance counter functionality.
 *
 * Generally, performance counter functionality is accessed via the
 * "START_PERFORMANCE_COUNTER" and "END_PERFORMANCE_COUNTER" macros.
 * The other macros and functions have already been placed in the
 * code in the appropriate locations.
 *
 * To create a new performance counter, add an entry to the "PerformanceCounterName_e"
 * typedef and its associated entry to "PerformanceCounterNameAsString".
 *
 * For more information, see the, "Performance Counters Tiny Design Specification".
 *
 */

#ifndef PERFORMANCECOUNTER_H
#define PERFORMANCECOUNTER_H

#include "pg_config_manual.h"
#include <sys/time.h>
#include "c.h"
#include "access/xact.h"
#include <unistd.h>
#include "port.h"

/*
 * Defines
 */
#define PERFORMANCE_COUNTERS_DIR "gp_performance_counters"
#define MAX_PID_LENGTH 20  /* max for 64-bit integer in decimal places */


#define INIT_PERFORMANCE_COUNTERS() \
	if (log_performance_counters) \
		initializePerformanceCounters();

#define RESET_PERFORMANCE_COUNTERS(queryString) \
	if (log_performance_counters) \
		resetPerformanceCounters(queryString)

#define FLUSH_PERFORMANCE_COUNTERS() \
	if (log_performance_counters) \
		flushPerformanceCounters();

#define DEINITIALIZE_PERFORMANCE_COUNTERS() \
	if (log_performance_counters) \
		deinitialize_performance_counters()

#define START_PERFORMANCE_COUNTER(performanceCounterType) \
	if (log_performance_counters) \
		startCounter(performanceCounterType);

#define END_PERFORMANCE_COUNTER(performanceCounterType) \
	if (log_performance_counters) \
		endCounter(performanceCounterType);

#define PERFORMANCE_COUNTERS_SET_STATEMENT_START_AND_END_TIME(gpCommandCount) \
	if (log_performance_counters) \
		setStatementStartAndEndTime(gpCommandCount);


/* Must be kept in sync with PerformanceCounterNameAsString */
typedef enum PerformanceCounterName_e {
	performance_counter_name_not_specified = 0, /* This should be the first entry */
	performance_counter_AO_table_IO,
	performance_counter_LWLOCK_Acquire,
	performance_counter_Max,              /* All new entries should go before this entry */
	performance_counter_All,
	performance_counter_LastEntry         /* This should be the last entry */
} PerformanceCounterName_e;


/* Must be kept in sync with PerformanceCounterTypeAsString */
typedef enum PerformanceCounterType_e {
	PerformanceCounterTypeNotSpecified = 0, /* This should be the first entry */
	PerformanceCounterTypeActivate,
	PerformanceCounterTypeDeactivate,
	PerformanceCounterTypeStatus,
	PerformanceCounterTypeLastEntry,        /* This should be the last entry */
} PerformanceCounterType_e;


typedef struct PerformanceCounterStatus_s {
	volatile PerformanceCounterType_e type;
	volatile int                      sleepTime;  /* in seconds */
} PerformanceCounterStatus_s;


typedef struct PerformanceCounterShmem_s {
	volatile slock_t                    lock;
	volatile PerformanceCounterStatus_s status[performance_counter_LastEntry];
} PerformanceCounterShmem_s;


typedef struct PerformanceCounterInfo_s {
	bool   isActive;
	uint64 startTime;         /* Clock time (microseconds) when counter started for this counter block.        */
	uint64 startCpuTicks;     /* CPU ticks value when counter started for this counter block.                  */
	uint64 totalTime;         /* Cumulative clock time (microseconds) spent in this counter's start/end block. */
	uint64 totalCpuTicks;     /* Cumulative CPU ticks spent in this counter's start/end block.                 */
	uint64 totalMeasurements; /* The total number of times the counter's start/end block was entered.          */
} PerformanceCounterInfo_s;


typedef struct AllPerformanceInfo_s {
	int     mppSessionId;
	int     queryCommandId;
	char   *queryString;
	uint64  statementStartTime;  /* Clock time (microseconds) when the statement started execution. */
	uint64  statementEndTime;    /* Clock time (microseconds) when the statement ended execution.   */
	struct  PerformanceCounterInfo_s performanceCounterInfo[performance_counter_LastEntry];
} AllPerformanceInfo_s;


extern PerformanceCounterShmem_s *performanceCounterShmem;
extern AllPerformanceInfo_s allPerformanceInfo;
extern bool performanceCountersInitialized;


/*
 * Static function declarations
 */

/*
 * This function is called when a statement has completed running.
 */
static void inline setStatementStartAndEndTime(int gpCommandCount)
{
	allPerformanceInfo.statementStartTime =  GetCurrentStatementStartTimestamp();
	allPerformanceInfo.statementEndTime = GetCurrentTimestamp();
	allPerformanceInfo.queryCommandId = gpCommandCount;
}


/*
 * This function is not called directly, but is invoked via the START_PERFORMANCE_COUNTER() define.
 * It records information about the current time, and activates the state of the counter.
 */
static void inline startCounter(PerformanceCounterName_e performanceCounterNameEnum)
{
	struct timeval nowTime;

	if (   performanceCountersInitialized == false
			|| performanceCounterShmem == NULL
			|| performanceCounterShmem->status[performanceCounterNameEnum].type == PerformanceCounterTypeDeactivate
			|| allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].isActive == true
		 )
		/* nothing to do */
		return;
	if (performanceCounterShmem->status[performanceCounterNameEnum].sleepTime > 0)
	{
		pg_usleep(performanceCounterShmem->status[performanceCounterNameEnum].sleepTime * 1000000L);
	}
	if (log_performance_counters_clock_time == true)
	{
		gettimeofday(&nowTime, NULL);
	}
	else
	{
		memset(&nowTime, 0, sizeof(nowTime));
	}
	allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].startTime = (nowTime.tv_sec * 1000000) + nowTime.tv_usec;
	allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].isActive = true;
	allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].startCpuTicks = get_rdtsc();
}

/*
 * This function is not called directly, but is invoked via the END_PERFORMANCE_COUNTER() define.
 * It takes the information recorded when START_PERFORMANCE_COUNTER was called, and will
 * record the cumulative time spend in the counter.
 */
static inline void endCounter(PerformanceCounterName_e performanceCounterNameEnum)
{
	struct timeval nowTime;
	uint64 totalTime;
	uint64 nowCpuTicks;
	uint64 totalCpuTicks;

	if (   performanceCountersInitialized == false
			|| performanceCounterShmem == NULL
			|| performanceCounterShmem->status[performanceCounterNameEnum].type == PerformanceCounterTypeDeactivate
			|| allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].isActive == false
		 )
		/* nothing to do */
		return;
	gettimeofday(&nowTime, NULL);
	totalTime = allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].totalTime;
	totalTime = totalTime +
		(((nowTime.tv_sec * 1000000) + nowTime.tv_usec) - allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].startTime);
	allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].totalTime = totalTime;

	nowCpuTicks = get_rdtsc();
	totalCpuTicks = allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].totalCpuTicks;
	totalCpuTicks = totalCpuTicks + (nowCpuTicks - allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].startCpuTicks);
	allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].totalCpuTicks = totalCpuTicks;

	allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].totalMeasurements =
		   allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].totalMeasurements + 1;
	allPerformanceInfo.performanceCounterInfo[performanceCounterNameEnum].isActive = false;
}

extern Size
PerformanceCounter_ShmemSize(void);

extern void
PerformanceCounter_ShmemInit(void);

extern int
PerformanceCounter_TakeAction(char *performanceCounterName,
				char *typeName,
				int   sleepTime,
				char *outBuffer,
				int   outBufferLen);

extern int
initializePerformanceCounters(void);

extern int
resetPerformanceCounters(const char * queryString);

extern int
flushPerformanceCounters(void);


#endif	/* PERFORMANCECOUNTER_H */

