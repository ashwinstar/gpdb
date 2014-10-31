/*-------------------------------------------------------------------------
 *
 * runaway_cleaner.c
 *	 Implementation of the runaway cleaner that checks if a session is marked
 *	 as runaway (i.e., consuming too much vmem) by the red-zone handler
 *	 (redzone_handler.c). The runaway cleaner cleans up such session by triggering
 *	 an elog(ERROR, ...) which rolls back transaction and releases memory. Once
 *	 cleanup is finished, the runaway cleaner also informs the red zone handler
 *	 so that a new runaway session can be chosen if necessary.
 *
 * Copyright (c) 2014 Pivotal Inc. All Rights Reserved
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/atomic.h"
#include "cdb/cdbvars.h"
#include "utils/vmem_tracker.h"
#include "utils/session_state.h"
#include "utils/faultinjector.h"
#include "miscadmin.h"

/* External dependencies within the runaway cleanup framework */
extern bool vmemTrackerInited;
extern bool isProcessActive;
extern EventVersion activationVersion;
extern EventVersion deactivationVersion;
extern volatile uint32 *isRunawayDetector;
extern volatile EventVersion *latestRunawayVersion;

/*
 * The cleanupCountdown in the SessionState determines how many
 * processes we need to cleanup to declare a session clean. If it
 * reaches 0, we mark the session clean. However, -1 indicates
 * that the session is either done cleaning previous runaway event
 * or it never started a cleaning.
 */
#define CLEANUP_COUNTDOWN_BEFORE_RUNAWAY -1

/* The runaway version for which this process started cleaning up */
EventVersion beginCleanupRunawayVersion = 0;

/* The runaway version for which this process finished cleaning up */
EventVersion endCleanupRunawayVersion = 0;

void RunawayCleaner_Init(void);
void RunawayCleaner_StartCleanup(void);
bool RunawayCleaner_IsCleanupInProgress(void);

/*
 * Initializes the per-process states of the runaway cleaner.
 */
void
RunawayCleaner_Init()
{
	beginCleanupRunawayVersion = 0;
	endCleanupRunawayVersion = 0;
}

/*
 * Starts a runaway cleanup by triggering an ERROR if the VMEM tracker is active
 * and a commit is not already in progress. Otherwise, it marks the process as clean.
 */
void
RunawayCleaner_StartCleanup()
{
	/*
	 * Cleanup can be attempted from multiple places, such as before deactivating
	 * a process (if a pending runaway event) or periodically from CHECK_FOR_INTERRUPTS
	 * (indirectly via RedZoneHandler_DetectRunaway). We don't carry multiple cleanup
	 * for a single runaway event. Every time we *start* a cleanup process, we set the
	 * beginCleanupRunawayVersion to the runaway version for which we started cleaning
	 * up. Later on, if we reenter this method (e.g., another CHECK_FOR_INTERRUPTS()
	 * during cleanup), we can observe that the cleanup already started from this runaway
	 * event, and therefore we skip duplicate cleanup.
	 */
	if (NULL != MySessionState && MySessionState->runawayStatus != RunawayStatus_NotRunaway &&
			beginCleanupRunawayVersion != *latestRunawayVersion)
	{
		Assert(beginCleanupRunawayVersion < *latestRunawayVersion);
		Assert(endCleanupRunawayVersion < *latestRunawayVersion);
		/* We don't want to cleanup multiple times for same runaway event */
		beginCleanupRunawayVersion = *latestRunawayVersion;

		if (CritSectionCount == 0 && InterruptHoldoffCount == 0 && vmemTrackerInited &&
			/* Super user is terminated only when it's the primary runaway consumer (i.e., the top consumer) */
			(!superuser() || MySessionState->runawayStatus == RunawayStatus_PrimaryRunawaySession))
		{
#ifdef FAULT_INJECTOR
	FaultInjector_InjectFaultIfSet(
			RunawayCleanup,
			DDLNotSpecified,
			"",  // databaseName
			""); // tableName
#endif

			elog(ERROR, "Canceling query because of high VMEM usage. Used: %dMB, available %dMB, red zone: %dMB",
					VmemTracker_ConvertVmemChunksToMB(MySessionState->sessionVmem), VmemTracker_GetAvailableVmemMB(),
					RedZoneHandler_GetRedZoneLimitMB());
		}

		/*
		 * If we cannot error out because of a commit in progress,
		 * simply declare this process as clean
		 */
		RunawayCleaner_RunawayCleanupDoneForProcess(true /* ignoredCleanup */);
	}
}

/*
 * Resets the runaway flag and enables runaway detector.
 *
 * Note: this method should not need any additional locks.
 * Either the MySessionState entry is being released, and
 * we already have a lock on SessionState, and therefore,
 * no new runaway detector can run until the lock is released.
 *
 * Alternatively, we may reset this while still in a live
 * session. In such case, our runaway event versioning should
 * ensure that every process of this session would do another round
 * of cleanup if it is detected as a runaway session again.
 */
void
RunawayCleaner_RunawayCleanupDoneForSession()
{
	Assert(NULL != MySessionState);
	if (MySessionState->runawayStatus != RunawayStatus_NotRunaway)
	{
		/* The last runaway cleanup should have finished */
		Assert(endCleanupRunawayVersion == beginCleanupRunawayVersion);
		Assert(endCleanupRunawayVersion == *latestRunawayVersion);
		Assert(CLEANUP_COUNTDOWN_BEFORE_RUNAWAY == MySessionState->cleanupCountdown);

		MySessionState->runawayStatus = RunawayStatus_NotRunaway;
		MySessionState->sessionVmemRunaway = 0;
		MySessionState->commandCountRunaway = 0;

		/*
		 * Reset the exclusive runaway detector flag so that
		 * another runaway detector can be chosen
		 */
		*isRunawayDetector = 0;
	}
}

/*
 * Marks the current process as clean. If all the processes are marked
 * as clean for this session (i.e., cleanupCountdown == 0 in the
 * MySessionState) then we reset session's runaway status as well as
 * the runaway detector flag (i.e., a new runaway detector can run).
 *
 * Parameters:
 * 		ignoredCleanup: whether the cleanup was ignored, i.e., no elog(ERROR, ...)
 * 		was thrown. In such case a deactivated process is not reactivated as the
 * 		deactivation didn't get interrupted.
 */
void
RunawayCleaner_RunawayCleanupDoneForProcess(bool ignoredCleanup)
{
	/*
	 * Ensure everyone in this function is responding to a single
	 * OOM event, as we are not using locking extensively and if
	 * the process is in deactivated state, it is not guaranteed to see
	 * a single runaway event as it does not have the leverage on
	 * cleanupCoundown, and multiple runaway events may be seen
	 * during the course of this method.
	 */
	EventVersion chosenRunawayEventVersion = *latestRunawayVersion;

	/*
	 * If we error out again after cleaning up a runaway event (not necessarily
	 * a runaway cleanup error, but could be any error), this method would be
	 * called again from PostgresMain error handler. We don't want to record
	 * every cleanup as a runaway cleanup. Particularly, we don't want to
	 * decrement the cleanupCountdown multiple times for a single runaway
	 * event. Therefore, each call to this method records the runaway event
	 * version for which it finished cleanup in endCleanupRunawayVersion.
	 * Later on, no matter how many times we call this method during regular
	 * error handling, we will not execute duplicate cleanup as the
	 * endCleanupRunawayVersion is already set to the most recent runaway
	 * event version.
	 */
	if (NULL != MySessionState && MySessionState->runawayStatus != RunawayStatus_NotRunaway &&
			endCleanupRunawayVersion != chosenRunawayEventVersion)
	{
		/*
		 * This process threw ERROR to cleanup at the time of deactivation.
		 * Therefore, we need to "reactivate" it before we consider it
		 * clean as we are no longer going to enter blocking state
		 * in ReadCommand in immediate future.
		 *
		 * Note: in this case, the process does hold leverage on cleanupCountdown
		 * as it is responding to a pre-deactivation era runaway event.
		 */
		if (beginCleanupRunawayVersion == chosenRunawayEventVersion &&
				chosenRunawayEventVersion < deactivationVersion &&
				(activationVersion < deactivationVersion /* Runaway event would ensure
				that the activationVersion is strictly smaller than deactivationVersion */&&
				 !isProcessActive) && !ignoredCleanup)
		{
			Assert(1 == *isRunawayDetector);
			Assert(0 < MySessionState->cleanupCountdown);
			/*
			 * As the process threw ERROR instead of going into ReadCommand() blocking
			 * state, we have to reactivate the process from its current Deactivated
			 * state
			 */
			IdleTracker_ActivateProcess();
		}
		/*
		 * Setting beginCleanupRunawayVersion would prohibit this process
		 * to become terminator in the future for the same runaway event
		 */
		beginCleanupRunawayVersion = chosenRunawayEventVersion;
		/*
		 * Setting endCleanupRunawayVersion would block multiple
		 * cleanup by the same process for a single runaway event
		 */
		endCleanupRunawayVersion = beginCleanupRunawayVersion;

		/* We only decrement if we were active when the runaway event happened */
		if (chosenRunawayEventVersion < deactivationVersion ||
				(chosenRunawayEventVersion > activationVersion &&
				(activationVersion >= deactivationVersion /* Unlike rollbacking deactivation
				in previous if block, we need a ">=" here as between previous deactivation and
				reactivation there may not be any runaway event, and therefore activationVersion
				may be equal to deactivationVersion */ && isProcessActive)))
		{
			/*
			 * In this case, this process is holding a cleanupCountdown, so no other
			 * runaway event is possible unless this one is finished cleanup.
			 */
			Assert(chosenRunawayEventVersion == *latestRunawayVersion);
			Assert(0 < MySessionState->cleanupCountdown);
#if USE_ASSERT_CHECKING
			int cleanProgress =
#endif
					gp_atomic_add_32(&MySessionState->cleanupCountdown, -1);
			Assert(0 <= cleanProgress);

			bool finalCleaner = compare_and_swap_32((uint32*) &MySessionState->cleanupCountdown,
					0, CLEANUP_COUNTDOWN_BEFORE_RUNAWAY);

			if (finalCleaner)
			{
				/*
				 * The final cleaner is responsible to reset the runaway flag,
				 * and enable the runaway detection process.
				 */
				RunawayCleaner_RunawayCleanupDoneForSession();
			}
		}
	}
}

/*
 * Returns true if a cleanup is in progress (i.e., endCleanupRunawayVersion
 * is smaller than beginCleanupRunawayVersion).
 */
bool
RunawayCleaner_IsCleanupInProgress()
{
	Assert(endCleanupRunawayVersion <= beginCleanupRunawayVersion);
	return endCleanupRunawayVersion < beginCleanupRunawayVersion;
}
