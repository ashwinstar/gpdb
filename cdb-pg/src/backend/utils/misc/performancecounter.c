/*
 * Copyright (c) 2012 EMC Corporation All Rights Reserved
 *
 * This software is protected, without limitation, by copyright law
 * and international treaties. Use of this software and the intellectual
 * property contained therein is expressly limited to the terms and
 * conditions of the License Agreement under which it is provided by
 * or on behalf of EMC.
 * ---------------------------------------------------------------------
 *
 * Interfaces to performance counter functionality.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "cdb/cdbgang.h"

#include "postgres.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/performancecounter.h"

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "cdb/cdbvars.h"


/*
 * globals
 */
AllPerformanceInfo_s       allPerformanceInfo;
bool                       performanceCountersInitialized = false;
PerformanceCounterShmem_s *performanceCounterShmem = NULL;


/*
 * static globals
 */

/* Must be kept in sync with PerformanceCounterName_e */
static const char *PerformanceCounterNameAsString[] = \
	{ \
	 "performance_counter_NotSpecified" ,
	 "performance_counter_AOtableInput" ,
	 "performance_counter_LWLockAcquire" ,
	 "performance_counter_Max" ,
	 "performance_counter_All" ,
	 "performance_counter_LastEntry"
	};

/* Must be kept in sync with PerformanceCounterType_e */
static const char *PerformanceCounterTypeAsString[] = \
	{ \
	"PerformanceCounterTypeNotSpecified" ,
	"activate" ,
	"deactivate" ,
	"status" ,
	"PerformanceCounterTypeLastEntry" ,
	};

static char          *fullPath = NULL;
static File           performanceCounterFd = -1;

/*
 * Static functions.
 */
static void deinitializePerformanceCounters(int code, Datum arg);
static PerformanceCounterType_e	PerformanceCounterTypeStringToEnum(const char *performanceCounterTypeString);
static PerformanceCounterName_e PerformanceCounterNameStringToEnum(const char *performanceCounterName);
static const char * PerformanceCounterNameEnumToString(PerformanceCounterName_e	performanceCounterNameEnum);
static const char * PerformanceCounterTypeEnumToString(PerformanceCounterType_e	performanceCounterTypeEnum);


/*
 * This function is used in conjunction with snprintf to determine if the size of a string
 * written to a buffer is greater than the max size available in the buffer. snprintf will
 * return sizeWritten as the size value if the buffer was too long (i.e. the returned size
 * is the copied size minus the '\0' on the end).
 *
 */
static bool checkBufferOverflow(char *outBuffer, int outBufferLen, int maxSize, int sizeWritten)
{
	bool retValue = false;

	if (sizeWritten >= maxSize)
	{
		/* Not enough room in the buffer. */
		outBuffer[outBufferLen] = '\0';  /* make sure we have a null on the end */
		elog(WARNING, "Performance counter buffer is not large enough to contain status message.");
		retValue = true;
	}
	return retValue;
}

/*
 * Given a performance counter name, return its enumerated equivalent.
 */
PerformanceCounterName_e
PerformanceCounterNameStringToEnum(const char *performanceCounterName)
{
	PerformanceCounterName_e	performanceCounterNameEnum = performance_counter_name_not_specified;
	const char *candidateName;
	int	ii;

	for (ii=0; ii < performance_counter_LastEntry; ii++)
	{
		candidateName = PerformanceCounterNameEnumToString(ii);
		if (strcmp(candidateName, performanceCounterName) == 0)
		{
			performanceCounterNameEnum = ii;
			break;
		}
	}
	return performanceCounterNameEnum;
}

/*
 * Given a performance counter enumeration, return its string equivalent.
 */
const char *
PerformanceCounterNameEnumToString(PerformanceCounterName_e	performanceCounterNameEnum)
{
	Assert(performanceCounterNameEnum >= performance_counter_name_not_specified);
	Assert(performanceCounterNameEnum <= performance_counter_LastEntry);
	return PerformanceCounterNameAsString[performanceCounterNameEnum];
}


/*
 * Given a performance counter type, return its enumerated equivalent.
 */
PerformanceCounterType_e
PerformanceCounterTypeStringToEnum(const char *performanceCounterTypeString)
{
	PerformanceCounterType_e	performanceCounterTypeEnum = PerformanceCounterTypeNotSpecified;
	int	ii;

	for (ii=0; ii < PerformanceCounterTypeLastEntry; ii++) {
		if (strcmp(PerformanceCounterTypeAsString[ii], performanceCounterTypeString) == 0)
		{
			performanceCounterTypeEnum = ii;
			break;
		}
	}
	return performanceCounterTypeEnum;
}


/*
 * Given a performance counter type enumeration, return its string equivalent.
 */
const char *
PerformanceCounterTypeEnumToString(PerformanceCounterType_e	performanceCounterTypeEnum)
{
	Assert(performanceCounterTypeEnum >= PerformanceCounterTypeNotSpecified);
	Assert(performanceCounterTypeEnum <= PerformanceCounterTypeLastEntry);
	return PerformanceCounterTypeAsString[performanceCounterTypeEnum];
}

/*
 * Return the size (in bytes) needed by the performance counter shared memory logic.
 */
Size
PerformanceCounter_ShmemSize(void)
{
	Size	size;

	size = sizeof(PerformanceCounterShmem_s);
	return size;
}

/*
 * Shared memory contains information for each performance counter.
 */
void
PerformanceCounter_ShmemInit(void)
{
	bool	foundPtr;

	performanceCounterShmem = (PerformanceCounterShmem_s *) ShmemInitStruct("performance counter",
																  sizeof(PerformanceCounterShmem_s),
																  &foundPtr);

	if (performanceCounterShmem == NULL) {
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 (errmsg("not enough shared memory for performance counters"))));
	}

	if (foundPtr == false)
	{
		/* Initialize the shared memory. */
		int i = 0;

		for (i = 0; i < performance_counter_LastEntry; i++)
		{
			performanceCounterShmem->status[i].type = PerformanceCounterTypeDeactivate;
			performanceCounterShmem->status[i].sleepTime = 0;
		}
	}
}

/*
 *  Get the status of the performance counters, log the results,
 *  and format the results in the outBuffer. If the outBuffer is
 *  too short to contain the output, truncate the output to fit,
 *  and log a warning.
 */
static void
PerformanceCounter_GetStatus(
		PerformanceCounterName_e startRange,
		PerformanceCounterName_e endRange,
		char *outBuffer,
		int   outBufferLen)
{
	const char *name;
	const char *statusType;
	int         statusSleepTime;
	int         maxSize = 0;
	int         sizeWritten = 0;
	int         offset = 0;

	Assert(startRange > performance_counter_name_not_specified);
	Assert(endRange < performance_counter_Max);

	maxSize = outBufferLen;
	sizeWritten =  snprintf(&outBuffer[offset], maxSize, "Success: \n");
	checkBufferOverflow(outBuffer, outBufferLen, maxSize, sizeWritten);
	offset = offset + sizeWritten;
	for (int i = startRange; i <= endRange; i++)
	{
		name            = PerformanceCounterNameAsString[i];
		statusType      = PerformanceCounterTypeEnumToString(performanceCounterShmem->status[i].type);
		statusSleepTime = performanceCounterShmem->status[i].sleepTime;
		ereport(LOG, (errmsg("performance counter name: '%s' "
												 "status: '%s' "
												 "sleepTime: '%d' ",
												 name,
												 statusType,
												 statusSleepTime)));
		maxSize = outBufferLen - offset;
		sizeWritten = snprintf(&outBuffer[offset], maxSize,
				 "performance counter name: '%s' "
				 "status: '%s'"
				 "sleepTime: '%d' \n",
				 name,
				 statusType,
				 statusSleepTime);
		checkBufferOverflow(outBuffer, outBufferLen, maxSize, sizeWritten);
		offset = offset + sizeWritten;
	}
}


/*
 * This function is called as a result of a request to take performance counter related action.
 * This would usually be from an invocation of the gpperformancecounter tool.
 * The parameters are:
 *
 *   performanceCounterName - The name of a performance counter
 *   typeName               - The type of action (activate, deactiveate, get status, etc.)
 *   sleepTime              - Applicable if we are activating a performance counter
 *   outBuffer              - The output of the action (usually used by gpperformancecounter tool).
 *   outBufferLen           - The length of the outBuffer in bytes.
 *
 */
int
PerformanceCounter_TakeAction(char *performanceCounterName, char *typeName, int sleepTime, char *outBuffer, int outBufferLen)
{
	int		returnValue  = STATUS_OK;
	int   maxSize = 0;
	int   sizeWritten = 0;
	int   offset = 0;

	PerformanceCounterName_e  performanceCounterEnum;
	PerformanceCounterName_e  startRange;
	PerformanceCounterName_e  endRange;
  PerformanceCounterType_e  typeEnum;

  if (log_performance_counters == false)
  	return returnValue;

	/* Make sure we recognize the performance counter */
	performanceCounterEnum = PerformanceCounterNameStringToEnum(performanceCounterName);
	if (performanceCounterEnum == performance_counter_name_not_specified)
	{
		sizeWritten = snprintf(outBuffer, outBufferLen, "Failure: Unknown performance counter name: '%s' \n", performanceCounterName);
		checkBufferOverflow(outBuffer, outBufferLen, maxSize, sizeWritten);
		returnValue = STATUS_ERROR;
		return returnValue;
	}

	/* Make sure we recognize the type */
	typeEnum = PerformanceCounterTypeStringToEnum(typeName);
	if (typeEnum == PerformanceCounterTypeNotSpecified)
	{
		sizeWritten = snprintf(outBuffer, outBufferLen, "Failure: Unknown performance counter type: '%s' \n", typeName);
		checkBufferOverflow(outBuffer, outBufferLen, maxSize, sizeWritten);
		returnValue = STATUS_ERROR;
		return returnValue;
	}

	if (performanceCounterEnum == performance_counter_All)
	{
		startRange = performance_counter_name_not_specified + 1;
		endRange   = performance_counter_Max - 1;
	}
	else
	{
		startRange = performanceCounterEnum;
		endRange   = performanceCounterEnum;
	}

	switch (typeEnum) {
		case PerformanceCounterTypeStatus:
		{
			PerformanceCounter_GetStatus(startRange, endRange, &outBuffer[offset], outBufferLen);
			break;
		}
		/* Activate and deactivate cases are handled the same, so do both here. */
		case PerformanceCounterTypeActivate:
		case PerformanceCounterTypeDeactivate:
		{
			offset = offset + snprintf(&outBuffer[offset], (outBufferLen - offset), "Success: \n");
			checkBufferOverflow(outBuffer, outBufferLen, maxSize, sizeWritten);
			for (int i = startRange; i <= endRange; i++)
			{
				performanceCounterShmem->status[i].type = typeEnum;
				performanceCounterShmem->status[i].sleepTime = sleepTime;
			}
			break;
		}
		default:
			offset = offset + snprintf(&outBuffer[offset], (outBufferLen - offset),
					"Failure: Unknown performance counter type %d \n", typeEnum);
			checkBufferOverflow(outBuffer, outBufferLen, maxSize, sizeWritten);
			returnValue = STATUS_ERROR;
			break;
	}

	return returnValue;
}


int
initializePerformanceCounters(void)
{
	int returnStatus = STATUS_OK;

	if (performanceCountersInitialized == true)
		/* Already initialized */
		returnStatus = STATUS_OK;
	else if (performanceCounterShmem == NULL)
		/* No shared memory */
		returnStatus = STATUS_ERROR;
	else
	{
		int   pcdSize = 0;
		int   fullPathLen = 0;
		char  pidAsString[MAX_PID_LENGTH + 1];
		char *performanceCountersDirectory = NULL;
		MemoryContext oldContext;

		/*
		 * The performance counter file for this process exists as long as the processes exists,
		 * so we allocate the information about the file in the top memory context.
		 */
		oldContext = MemoryContextSwitchTo(TopMemoryContext);

		pcdSize = strlen(data_directory) + 1 + strlen(PERFORMANCE_COUNTERS_DIR) + 1;
		performanceCountersDirectory = palloc(pcdSize);
		snprintf(performanceCountersDirectory, pcdSize, "%s/%s", data_directory, PERFORMANCE_COUNTERS_DIR);

		pg_ltoa(MyProcPid, pidAsString);
		fullPathLen = strlen(performanceCountersDirectory) + 1 + MAX_PID_LENGTH + 1;
		fullPath = palloc(fullPathLen);
		snprintf(fullPath, fullPathLen, "%s/%s", performanceCountersDirectory, pidAsString);
		pfree(performanceCountersDirectory);

		MemoryContextSwitchTo(oldContext);

		on_proc_exit(deinitializePerformanceCounters, 0);
		performanceCountersInitialized = true;
	}
	return returnStatus;
}

int
resetPerformanceCounters(const char * queryString)
{
	int returnStatus = STATUS_OK;
	int queryStringLen = 0;
	MemoryContext oldContext;

	if (performanceCountersInitialized == false)
	{
		/* Not initialized. */
		returnStatus = STATUS_ERROR;
	}
	else
	{
		allPerformanceInfo.mppSessionId   = MyProc->mppSessionId;
		allPerformanceInfo.queryCommandId = gp_command_count;
		oldContext = MemoryContextSwitchTo(TopMemoryContext);
		if (queryString != NULL)
		{
			queryStringLen = strlen(queryString);
			allPerformanceInfo.queryString = palloc(queryStringLen + 1);
			strcpy(allPerformanceInfo.queryString, queryString);
		}
		else
		{
			queryStringLen = 0;
			allPerformanceInfo.queryString = palloc(queryStringLen + 1);
			strcpy(allPerformanceInfo.queryString, "");
		}
		MemoryContextSwitchTo(oldContext);

		/*
		 * We need to do a little editing of the query string so that COPY doesn't get confused.
		 * We will replace all double quotes (") with question marks (?). This makes the COPY
		 * command happy. The edited string is for logging purposes only, and will not be used
		 * in a context where the editing matters.
		 */
		int i;
		for (i = 0; i < queryStringLen; i++)
		{
			if (allPerformanceInfo.queryString[i] == '"')
				allPerformanceInfo.queryString[i] = '?';
		}

		allPerformanceInfo.statementStartTime = 0;
		allPerformanceInfo.statementEndTime = 0;

		for (int i = 0; i < performance_counter_LastEntry; i++)
		{
			allPerformanceInfo.performanceCounterInfo[i].isActive = false;
			allPerformanceInfo.performanceCounterInfo[i].startTime = 0;
			allPerformanceInfo.performanceCounterInfo[i].startCpuTicks = 0;
			allPerformanceInfo.performanceCounterInfo[i].totalTime = 0;
			allPerformanceInfo.performanceCounterInfo[i].totalCpuTicks = 0;
			allPerformanceInfo.performanceCounterInfo[i].totalMeasurements = 0;
		}
	}
	return returnStatus;
}


int
flushPerformanceCounters(void)
{
	int returnStatus = STATUS_OK;

	if (performanceCountersInitialized == false)
		/* no need to do anything */
		returnStatus = STATUS_OK;
	else
	{
		StringInfoData strInfo;

		initStringInfo(&strInfo);
		for (int i = performance_counter_name_not_specified + 1; i < performance_counter_LastEntry; i++)
		{
			if (performanceCounterShmem->status[i].type == PerformanceCounterTypeDeactivate)
				/* counter is not activated */
				continue;

			if (allPerformanceInfo.performanceCounterInfo[i].isActive == true)
				/* This should not happen normally. It means we are still in the counter block for some reason. */
				elog(WARNING, "Performance counter %s is in an Active state", PerformanceCounterTypeEnumToString(i));

			/* KAS might not need this */
			if (allPerformanceInfo.performanceCounterInfo[i].totalCpuTicks == 0 && Gp_role != GP_ROLE_DISPATCH)
				/* Implies that it is not used */
				continue;

			uint64 totalTime = allPerformanceInfo.performanceCounterInfo[i].totalTime;
			uint64 totalCpuTicks = allPerformanceInfo.performanceCounterInfo[i].totalCpuTicks;
			uint64 totalMeasurements = allPerformanceInfo.performanceCounterInfo[i].totalMeasurements;

			/*
			 * Record format (each entry separated by  a vertical bar "|").
			 *
			 *   process id
			 *   seg id
			 *   session id
			 *   command id
			 *   query string
			 *   query start time (master only)
			 *   query end time   (master only)
			 *   performance counter id
			 *   total time spent in performance counter
			 *   total CPU ticks
			 *   total number of measurements taken
			 */
			appendStringInfo( &strInfo
							, "%d|%d|%d|%d|\"%s\"|%d|" UINT64_FORMAT "|" UINT64_FORMAT "|" UINT64_FORMAT "|" UINT64_FORMAT "|" UINT64_FORMAT "\n"
							, MyProcPid
							, (int16)GpIdentity.dbid
							, allPerformanceInfo.mppSessionId
							, allPerformanceInfo.queryCommandId
							, allPerformanceInfo.queryString
							, i
							, allPerformanceInfo.statementStartTime
							, allPerformanceInfo.statementEndTime
							, totalTime
							, totalCpuTicks
							, totalMeasurements
							);

			totalTime = 0;
			totalCpuTicks = 0;
			totalMeasurements = 0;

			/* Create/open the file if the first time */
			if (performanceCounterFd < 0)
			{
				performanceCounterFd =	PathNameOpenFile(fullPath, O_APPEND | O_CREAT | O_WRONLY, S_IRWXU | S_IRWXG | S_IRWXO);
			}
			if (performanceCounterFd < 0)
			{
				ereport( WARNING
							 , ( errcode_for_file_access()
								 , errmsg( "Performance counters: could not open file '%s' "
												 , (fullPath == NULL) ? "<null>" : fullPath
												 )
								 )
							 );
				returnStatus = STATUS_ERROR;
				return returnStatus;
			}

			int err = 0;

			err = FileWrite(performanceCounterFd, strInfo.data, strInfo.len);
			if (err != strInfo.len)
			{
				ereport( WARNING
							 , ( errcode_for_file_access()
								 , errmsg( "Performance counters: could not write to file '%s' "
												 , (fullPath == NULL) ? "<null>" : fullPath
												 )
								 )
							 );
				returnStatus = STATUS_ERROR;
				return returnStatus;
			}
			resetStringInfo(&strInfo);
		}  /* end for */

		if (strInfo.data != NULL)
			pfree(strInfo.data);

		/* cleanup */
		if (allPerformanceInfo.queryString != NULL)
			pfree(allPerformanceInfo.queryString);
		allPerformanceInfo.queryString = NULL;
	}
	return returnStatus;
}

static void
deinitializePerformanceCounters(int code, Datum arg)
{
	if (fullPath != NULL)
		pfree(fullPath);

	if (performanceCounterFd >= 0)
	{
		FileClose(performanceCounterFd);
		performanceCounterFd = -1;
	}

	performanceCountersInitialized = false;
}


