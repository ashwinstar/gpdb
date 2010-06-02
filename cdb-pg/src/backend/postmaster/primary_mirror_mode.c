/**
 * Copyright (c) 2005-2009, Greenplum inc
 *
 * Manages transitions between primary/mirror modes.
 *
 */
#include "postgres.h"
#include "postmaster/postmaster.h"
#include "postmaster/primary_mirror_mode.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "miscadmin.h"
#include "access/xlog.h"
#include <string.h>

/**
 * todo MIRRORING: error when doRequestedPrimaryMirrorModeTransitions should cause a panic?  Or at least should wake
 *                 up requestXXX.  Do we need any critical sections or anything here?  Review setup of
 *                 signal handlers here -- if we are inside a transition will we quit out properly on shutdown?
 */

/**
 * The transition state is used when the postmaster needs to return back to the server loop.
 *   The primary/mirror mode function will mark its transition as being in-progress...
 *   later calls to doRequestedPrimaryMirrorModeTransition will check this field.
 *
 * Note: if the order or # of values is changed then update the labels below.
 *
 * this implements a state machine.  Each transition manages its own transition order through this machine
 *
 * Note that some places (notably, getFileRepRoleAndState) rely on the order of these
 *   for implementation.  Be careful of that when changing this.
 */
typedef enum
{
        TSNone = 0,

        /*
         * The postmaster has signaled filerep to shutdown and is now waiting for it to
         *   complete
         */
        TSDoFilerepBackendsShutdown,
        TSAwaitingFilerepBackendsShutdown,
        TSDoneFilerepBackendsShutdown,
        TSDoFilerepShutdown,
        TSAwaitingFilerepShutdown,
        TSDoneFilerepShutdown,

        /* sequence for doing filerep startup */
        TSDoFilerepStartup,
        TSAwaitingFilerepStartup,
        TSDoneFilerepStartup,

        /* for two-phase recovery (where filerep goes into changelogging before database startup and
         *   needs to be notified after the database is up) */

        TSDoDatabaseStartup,
        TSAwaitingDatabaseStartup,
        TSDoneDatabaseStartup,

        TSDone,

        /* note: these should be in sync with getTransitionStateLabel */

        /* the number of elements in the enumeration */
        TS__EnumerationCount
} TransitionState;

/* State management */
static void assertModuleInitialized(void);
static void assertIsTransitioning(void);

/* actual transitions */
static PrimaryMirrorModeTransitionResult applyStepForTransitionToUninitializedMode(PrimaryMirrorModeTransitionArguments *args,
                                TransitionState *stateInOut, char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO]);
static PrimaryMirrorModeTransitionResult applyStepForTransitionToMasterOrMirrorlessMode(PrimaryMirrorModeTransitionArguments *args,
                                TransitionState *stateInOut, char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO]);
static PrimaryMirrorModeTransitionResult applyStepForTransitionToQuiescentSegmentMode(PrimaryMirrorModeTransitionArguments *args,
                                TransitionState *stateInOut, char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO]);
static PrimaryMirrorModeTransitionResult applyStepForTransitionToPrimarySegmentMode(PrimaryMirrorModeTransitionArguments *args,
                                TransitionState *stateInOut, char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO]);
static PrimaryMirrorModeTransitionResult applyStepForTransitionToMirrorSegmentMode(PrimaryMirrorModeTransitionArguments *args,
                                TransitionState *stateInOut, char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO]);

/**
 * Shared globals for this module live in a single instance of PMModuleState.
 */
typedef struct PMModuleState
{
	/** all accesses to the isTransitionRequested and requestedTransition must lock on this */
	slock_t    lock;

    /* TRANSITION INFORMATION */

    /**
     * Has a transition been requested (and not yet fulfilled completed by the postmaster)?
     */
	volatile bool isTransitionRequested;

    /**
     * Has the backend that requested the current transition completed execution?
     */
	volatile bool isTransitionListenerFinished;

	/**
	 * The requested transition.
	 */
	volatile PrimaryMirrorModeTransitionArguments requestedTransition;

	/**
	 * The result of the transition; only valid when isTransitionRequested is false (postmaster done) and
	 *    isTransitionListenerFinished is also false (backend has not yet received this transitionResult
	 */
	volatile PrimaryMirrorModeTransitionResult transitionResult;

	/**
	 * The extra information about the transition result; used to report more info
	 *   back to the caller (such as to differentiate a mirroring failure from other failure cases)
	 */
	volatile char transitionResultExtraInfo[MAX_TRANSITION_RESULT_EXTRA_INFO];

	/**
	 * For transitions that require the postmaster to do multiple steps of work, this will hold the current
	 *    progress point.  Only valid while isTransitionRequested is true
	 */
	volatile TransitionState transitionState;

	/**
	 * Has the postmaster copied the currently requested transition to its local memory yet?
	 */
	volatile bool hasPostmasterCopiedTransitionToLocalMemory;

    /* FILE REPLICATION PARAMETERS */
	volatile PrimaryMirrorMode mode;
	volatile SegmentState_e segmentState;
	volatile DataState_e dataState;

	/* changeTrackingSessionId starts at zero and is incremented each time the system enters change tracking */
	volatile int64 changeTrackingSessionId;
	volatile bool isDatabaseRunning;

	/* connection info for self (which can be a mirror or primary, depending on mode) */
	volatile char hostAddress[PM_MAX_HOST_NAME_LENGTH+1];

	/* file replication port on self */
	volatile int hostPort;

	/* connection info for the peer (which can be a mirror or primary, depending on mode) */
	volatile char peerAddress[PM_MAX_HOST_NAME_LENGTH+1];

	/* file replication port on self */
	volatile int peerPort;

    /* OTHER FIELDS */
	volatile bool haveStartedDatabase;

	/** used by resync processes, this field will be kept up-to-date by the postmaster */
	volatile pid_t bgWriterPID;

    /**
     * a counter used to assign values to transitions; that values are for logging.
     */
	volatile int transitionNumberCounter;

	/**
	 *  if true, means we are in fault after postmaster reset and waiting for a transition to be sent
	 */
    volatile bool isInFaultFromPostmasterReset;
} PMModuleState;

static volatile PMModuleState *pmModuleState = NULL;

/**
 * Local globals live in a single PMLocalState instance
 */
typedef struct PMLocalState
{
    /**
     * The most recent transition applied.  This variable is only valid
     *   in the postmaster process; it is used to save around transitions
     *   for later postmaster reset.
     *
     * Only has meaning if haveSetMostRecentTransition is true
     */
    PrimaryMirrorModeTransitionArguments mostRecentTransition;

    /**
     * See mostRecentTransition
     */
    bool haveSetMostRecentTransition;

    /**
     * Was the most recent (non-fault) segment state "change tracking disabled"?
     */
    bool mostRecentSegmentStateWasChangeTrackingDisabled;
} PMLocalState;

PMLocalState pmLocalState;
bool gHaveInitializedLocalState = false;

/**
 * Extra result information that can be returned to the calling script so it
 *       can choose to take a different action besides just standard, generic handling
 */
#define RESULT_INFO_MIRRORING_FAILURE "MirroringFailure"
#define RESULT_INFO_POSTMASTER_DIED "PostmasterDied"
#define RESULT_INFO_IN_SHUTDOWN "ServerIsInShutdown"
#define RESULT_INFO_INVALID_STATE_TRANSITION "InvalidStateTransition"

/**
 * LABELS and parsing of values
 */

/**
 * Get the label for a primary mirror mode.  Returns a pointer to static memory.
 *
 * Note that the label is externally visible (through a getStatus call to the postmaster, for example)
 *
 * note also that some python code (as used by gpstate) may use these label values, so when changing them
 *  be sure to search the management scripts code.
 */
const char *
getMirrorModeLabel(PrimaryMirrorMode mode)
{
	static const char *labels[] =
	{
		"Uninitialized", "Master", "MirrorlessSegment",
		"QuiescentSegment", "PrimarySegment", "MirrorSegment"
	};

    COMPILE_ASSERT(ARRAY_SIZE(labels) == PMMode__EnumerationCount);
	
	return labels[mode];
}

/**
 * Get the label for a data state.  Returns a pointer to static memory.
 *
 * Note that the label is externally visible (through a getStatus call to the postmaster, for example)
 *
 * note also that some python code (as used by gpstate) may use these label values, so when changing them
 *  be sure to search the management scripts code.
 */
const char *
getDataStateLabel(DataState_e state)
{
	static const char *labels[] =
	{
			"NotInitialized", "InSync", "InChangeTracking",
			"InResync"
	};
    COMPILE_ASSERT(ARRAY_SIZE(labels) == DataState__EnumerationCount);

	return labels[state];
}

/**
 * Note that the label is externally visible (through a getStatus call to the postmaster, for example)
 *
 * note also that some python code (as used by gpstate) may use these label values, so when changing them
 *  be sure to search the management scripts code.
 */
const char *
getSegmentStateLabel(SegmentState_e state)
{
	static const char *labels[] =
	{
		"NotInitialized", "Initialization", "InChangeTrackingTransition", "InResyncTransition",
		"InSyncTransition", "Ready", "ChangeTrackingDisabled", "Fault",
		"ShutdownBackends", "Shutdown", "ImmediateShutdown"
	};
    COMPILE_ASSERT(ARRAY_SIZE(labels) == SegmentState__EnumerationCount);

	return labels[state];
}

/**
 * Get the label for a transition result.  Returns a pointer to static memory.
 *
 * Note that scripts use these values in strings that may be parsed, so please keep
 *      them as alphabetic values only
 */
const char *
getTransitionResultLabel(PrimaryMirrorModeTransitionResult res)
{
	static const char *labels[] =
	{
		"Success", "InvalidTargetMode", "OtherTransitionInProgress",
		"InvalidInputParameter", "Error", "OkayButStillInProgress"
	};
    COMPILE_ASSERT(ARRAY_SIZE(labels) == PMTransition__EnumerationCount);

	return labels[res];
}

/**
 * Get the label for transition state.  Returns a pointer to static memory.
 */
static const char *
getTransitionStateLabel(TransitionState state)
{
	static const char *labels[] =
	{
        "None",

        "DoFilerepBackendsShutdown",
        "AwaitingFilerepBackendsShutdown",
        "DoneFilerepBackendsShutdown",

        "DoFilerepShutdown",
        "AwaitingFilerepShutdown",
        "DoneFilerepShutdown",

        "DoFilerepStartup",
        "AwaitingFilerepStartup",
        "DoneFilerepStartup",

        "DoDatabaseStartup",
        "AwaitingDatabaseStartup",
        "DoneDatabaseStartup",

        "Done"
	};
    COMPILE_ASSERT(ARRAY_SIZE(labels) == TS__EnumerationCount);

	return labels[state];
}

/**
 * Convert the given string name for a primary mirror mode to a mode.  Note
 *    that any unparseable arg value returns PMModeUninitialized.
 *
 * @param arg should be non-NULL
 */
PrimaryMirrorMode
decipherPrimaryMirrorModeArgument(const char *arg)
{
	if (strcmp("master", arg) == 0)
		return PMModeMaster;
	else if (strcmp("mirrorless", arg) == 0)
		return PMModeMirrorlessSegment;
	else if (strcmp("quiescent", arg) == 0)
		return PMModeQuiescentSegment;
	else if (strcmp("primary", arg) == 0)
		return PMModePrimarySegment;
	else if (strcmp("mirror", arg) == 0)
		return PMModeMirrorSegment;
	else
		return PMModeUninitialized;
}

/**
 * acquire the module spin lock that is used to protect the contents of pmModuleState
 */
static void
acquireModuleSpinLock(void)
{
	SpinLockAcquire(&pmModuleState->lock);
}

/**
 * release the module spin lock that is used to protect the contents of pmModuleState
 */
static void
releaseModuleSpinLock(void)
{
	SpinLockRelease(&pmModuleState->lock);
}

static void
clearTransitionArgs(volatile PrimaryMirrorModeTransitionArguments *args)
{
	args->mode = PMModeUninitialized;
	args->dataState = DataStateNotInitialized;
	args->hostAddress[0] = '\0';
	args->hostPort = 0;
	args->peerAddress[0] = '\0';
	args->peerPort = 0;
	args->forceFullResync = false;
	args->transitionToFault = false;
	args->transitionToChangeTrackingDisabledMode = false;
}

PrimaryMirrorModeTransitionArguments *
createNewTransitionArguments(void)
{
	PrimaryMirrorModeTransitionArguments *result = palloc(sizeof(PrimaryMirrorModeTransitionArguments));
	clearTransitionArgs(result);
	return result;
}

static void
assertModuleInitialized(void)
{
	Assert(pmModuleState != NULL && "init must be called first");
}

static void
copyHostData(char **addressOut, int *portOut, char *address, int port)
{
	*addressOut = pstrdup(address);
	*portOut = port;
}

void
getPrimaryMirrorStatusCodes(PrimaryMirrorMode *pm_mode, SegmentState_e *s_state, DataState_e *d_state)
{
	acquireModuleSpinLock();
	{
        if (pm_mode)
            *pm_mode = pmModuleState->mode;

        if (s_state)
            *s_state = pmModuleState->segmentState;

        if (d_state)
            *d_state = pmModuleState->dataState;
	}
	releaseModuleSpinLock();
}

void
getPrimaryMirrorModeStatus(char *bufOut, int bufSize)
{
	PrimaryMirrorMode pm_mode;
	SegmentState_e s_state;
	DataState_e d_state;

	assertModuleInitialized();

	getPrimaryMirrorStatusCodes(&pm_mode, &s_state, &d_state);

	snprintf(bufOut, bufSize, "mode: %s\n""segmentState: %s\n""dataState: %s",
			 getMirrorModeLabel(pm_mode), getSegmentStateLabel(s_state), getDataStateLabel(d_state));
}

/**
 * This should only be called by FileRepMain during its startup
 */
void
primaryMirrorGetFilerepArguments(
			FileRepRole_e	*fileRepRoleOut, 
			SegmentState_e	*segmentStateOut, 
			DataState_e		*dataStateOut,
			char **primaryHostAddressOut,
			int *primaryPortOut,
			char **mirrorHostAddressOut, 
			int *mirrorPortOut,
            bool *fullSyncOut)
{
	PrimaryMirrorMode mode;
	volatile PrimaryMirrorModeTransitionArguments *args;

	assertModuleInitialized();

    acquireModuleSpinLock();
    {
        args = &pmModuleState->requestedTransition;
        mode = args->mode;

        if (mode == PMModePrimarySegment)
        {
            *fileRepRoleOut = FileRepPrimaryRole;
            copyHostData(primaryHostAddressOut, primaryPortOut, (char*)args->hostAddress, args->hostPort);
            copyHostData(mirrorHostAddressOut, mirrorPortOut, (char*)args->peerAddress, args->peerPort);
            *dataStateOut = args->dataState;
            *segmentStateOut = pmModuleState->segmentState;
            *fullSyncOut = args->forceFullResync;
        }
        else if (mode == PMModeMirrorSegment)
        {
            *fileRepRoleOut = FileRepMirrorRole;
            copyHostData(primaryHostAddressOut, primaryPortOut, (char*)args->peerAddress, args->peerPort);
            copyHostData(mirrorHostAddressOut, mirrorPortOut, (char*)args->hostAddress, args->hostPort);
            *dataStateOut = args->dataState;
            *segmentStateOut = pmModuleState->segmentState;
            *fullSyncOut = false;
        }
        else
        {
            *fileRepRoleOut = FileRepNoRoleConfigured;
            *primaryHostAddressOut = *mirrorHostAddressOut = NULL;
            *primaryPortOut = *mirrorPortOut = 0;
            *segmentStateOut = SegmentStateNotInitialized;
            *dataStateOut = DataStateNotInitialized;
            *fullSyncOut = false;
        }
    }
    releaseModuleSpinLock();
}

bool
isFullResync(void)
{
	volatile PrimaryMirrorModeTransitionArguments *args;

	assertModuleInitialized();

	acquireModuleSpinLock();

	args = &pmModuleState->requestedTransition;

	releaseModuleSpinLock();

	return(args->forceFullResync);
}

void
setFullResync(void)
{
	volatile PrimaryMirrorModeTransitionArguments *args;

	acquireModuleSpinLock();
	{
		args = &pmModuleState->requestedTransition;
		args->forceFullResync = TRUE;
	}
	releaseModuleSpinLock();
}
	
bool
isDatabaseRunning(void)
{
	bool isDatabaseRunning = FALSE;
	
	assertModuleInitialized();
	
	acquireModuleSpinLock();
	
	isDatabaseRunning = pmModuleState->isDatabaseRunning;
	
	releaseModuleSpinLock();
	
	return(isDatabaseRunning);
}

/**
 *
 * Fetch the current file and target replication role and states.  Returns values through the pointer arguments.
 *
 * Note that any of the arguments may be NULL in which case that piece of information is not returned.
 *
 * @param fileRepRoleOut may be NULL.  If non-null, *fileRepRoleOut is filled in with the current filerep role
 * @param segmentStateOut may be NULL.  If non-null, *segmentStateOut is filled in with the current segment state
 * @param dataStateOut  may be NULL.  If non-null, *dataStateOut is filled in with the current segment state
 * @param isInFilerepTransitionOut  may be NULL.  If non-null, *isInTransitionOut is filled in with true or false depending on
 *                           if the segment is in filerep transition to primary or mirror mode
 *                           Note that this is true only if we have reached the point in the full transition
 *                           where earlier values have been shutdown
 * @param transitionTargetDataStateOut  may be NULL.  If non-null, *transitionTargetDataStateOut is filled
 *                           in with the target data state, or DataStateNotInitialized if there is
 *                           no transition to primary or mirror mode is in progress.
 */
void
getFileRepRoleAndState(
			FileRepRole_e	*fileRepRoleOut,
			SegmentState_e	*segmentStateOut,
			DataState_e		*dataStateOut,

			bool *isInFilerepTransitionOut,
			DataState_e *transitionTargetDataStateOut
			)
{
	assertModuleInitialized();

    FileRepRole_e fileRepRole;
    SegmentState_e	segmentState;
    DataState_e		dataState;
    bool isInFilerepTransition;
    DataState_e transitionTargetDataState;

	acquireModuleSpinLock();
	{
        /* check to see if postmaster is still processing a transition.  Note that here we don't
         *  need to check to see if the listener is complete */
	    isInFilerepTransition = pmModuleState->isTransitionRequested &&
	                            pmModuleState->transitionState >= TSDoFilerepStartup;

		PrimaryMirrorMode mode = pmModuleState->mode;
		switch (mode)
		{
			case PMModePrimarySegment:
			case PMModeMirrorSegment:
			    fileRepRole = mode == PMModePrimarySegment ? FileRepPrimaryRole : FileRepMirrorRole;
                dataState = pmModuleState->dataState;
                segmentState = pmModuleState->segmentState;
				    
                if ( isInFilerepTransition )
                {
                    transitionTargetDataState = pmModuleState->requestedTransition.dataState;
                }
                else
                {
                    transitionTargetDataState = DataStateNotInitialized;
                }
				    
				break;
			default:
                fileRepRole = FileRepNoRoleConfigured;
                dataState = DataStateNotInitialized;
                segmentState = SegmentStateNotInitialized;
                transitionTargetDataState = DataStateNotInitialized;
				break;
		}
	}
	releaseModuleSpinLock();

	/* now fill in output! */
	if ( fileRepRoleOut )
	    *fileRepRoleOut = fileRepRole;
    if ( segmentStateOut )
	    *segmentStateOut = segmentState;
    if ( dataStateOut )
	    *dataStateOut = dataState;

    if ( isInFilerepTransitionOut )
	    *isInFilerepTransitionOut = isInFilerepTransition;
    if ( transitionTargetDataStateOut )
	    *transitionTargetDataStateOut = transitionTargetDataState;
}

static void
assertIsTransitioning(void)
{
	assertModuleInitialized();
	Assert((pmModuleState->isTransitionRequested || !pmModuleState->isTransitionListenerFinished)
	                    && "transition must be marked in-progress");
}

/**
 * NOTE: in some cases the caller should also call isInFaultFromPostmasterReset() to check to
 *       make sure the database will be started/should be started.  When in fault from postmaster reset,
 *       database processes should not be started.
 */
bool
isPrimaryMirrorModeAFullPostmaster(bool checkTargetModeAsWell)
{
	bool result;

	assertModuleInitialized();

	/* the locking here may not be technically necessary */
	acquireModuleSpinLock();
	{
		PrimaryMirrorMode mode = pmModuleState->mode;
		result = mode == PMModeMaster || mode == PMModePrimarySegment || mode == PMModeMirrorlessSegment;

		if (checkTargetModeAsWell && pmModuleState->isTransitionRequested)
		{
			mode = pmModuleState->requestedTransition.mode;
			result |= mode == PMModeMaster || mode == PMModePrimarySegment || mode == PMModeMirrorlessSegment;
		}
	}
	releaseModuleSpinLock();
	return result;
}

bool
isInFaultFromPostmasterReset()
{
    bool result;

    assertModuleInitialized();
	/* the locking here may not be technically necessary */
	acquireModuleSpinLock();
	{
		result = pmModuleState->isInFaultFromPostmasterReset;
	}
	releaseModuleSpinLock();
	return result;
}

bool
doesPrimaryMirrorModeRequireFilerepProcess(bool checkTargetModeAsWell)
{
	bool result;

	assertModuleInitialized();

	/* the locking here may not be technically necessary */
	acquireModuleSpinLock();
	{
		PrimaryMirrorMode mode = pmModuleState->mode;
		result = mode == PMModePrimarySegment || mode == PMModeMirrorSegment;

		if (checkTargetModeAsWell && pmModuleState->isTransitionRequested)
		{
			mode = pmModuleState->requestedTransition.mode;
			result |= mode == PMModePrimarySegment || mode == PMModeMirrorSegment;
		}
	}
	releaseModuleSpinLock();
	return result;
}

Size
primaryMirrorModeShmemSize(void)
{
	Size		size;
	size = sizeof(PMModuleState);
	return size;
}

void
primaryMirrorModeShmemInit(void)
{
    if ( ! gHaveInitializedLocalState )
    {
        /* only initialize local state once */
        pmLocalState.haveSetMostRecentTransition = false;
        pmLocalState.mostRecentSegmentStateWasChangeTrackingDisabled = false;

        gHaveInitializedLocalState = true;
    }

    pmModuleState = (PMModuleState *) ShmemAlloc(sizeof(PMModuleState));
	pmModuleState->isTransitionRequested = false;
	pmModuleState->hasPostmasterCopiedTransitionToLocalMemory = false;
	pmModuleState->mode = PMModeUninitialized;
	pmModuleState->segmentState = SegmentStateNotInitialized;
	pmModuleState->dataState = DataStateNotInitialized;
	pmModuleState->changeTrackingSessionId = 0;
	pmModuleState->isDatabaseRunning = false;
	pmModuleState->isTransitionListenerFinished = true;
	pmModuleState->transitionState = TSNone;

	pmModuleState->hostAddress[0] = '\0';
	pmModuleState->hostPort = 0;
	pmModuleState->peerAddress[0] = '\0';
	pmModuleState->peerPort = 0;

	pmModuleState->haveStartedDatabase = false;
	pmModuleState->transitionNumberCounter = 1;
	pmModuleState->isInFaultFromPostmasterReset = false;

	clearTransitionArgs(&pmModuleState->requestedTransition);

	SpinLockInit(&pmModuleState->lock);
}

/**
 * Reset all necessary parameters EXCEPT for the requested transition itself.  Caller must do that.
 *
 * Will be used for setting initial mode (on startup) and handling postmaster reset
 *
 * The only requestedTransition field reset is the transitionNumber
 */
static void
resetModuleStateForInitialTransition(void)
{
	pmModuleState->isTransitionRequested = true;
	pmModuleState->hasPostmasterCopiedTransitionToLocalMemory = false;
	pmModuleState->isTransitionListenerFinished = true; /* yes, true, because there is no waiting dispatcher */
	pmModuleState->transitionState = TSNone;
    pmModuleState->requestedTransition.transitionNumber = pmModuleState->transitionNumberCounter++;
}


void
setInitialRequestedPrimaryMirrorMode(PrimaryMirrorMode targetMode)
{
	Assert(pmModuleState->mode == PMModeUninitialized);
	Assert(targetMode != PMModePrimarySegment && targetMode != PMModeMirrorSegment);

	pmModuleState->requestedTransition.mode = targetMode;
    resetModuleStateForInitialTransition();
}

/**
 * Handle a postmaster reset.  This is called AFTER shared memory has been reset.
 *
 * This function will use the locally stored data from the most recently requested transition
 *    in order to transition.
 */
void
primaryMirrorHandlePostmasterReset(void)
{
    bool transitionToFault = false;

	Assert(pmModuleState->mode == PMModeUninitialized);
    Assert(pmLocalState.haveSetMostRecentTransition );

    if ( pmLocalState.mostRecentTransition.mode == PMModeMirrorSegment)
    {
        transitionToFault = true;
    }
    else if ( pmLocalState.mostRecentTransition.mode == PMModePrimarySegment)
    {
        if ( pmLocalState.mostRecentTransition.dataState != DataStateInChangeTracking )
        {
            /* transition to fault to let fault prober decide what to do */
            transitionToFault = true;
        }
    }
    else
    {
        transitionToFault = false;
    }

    elog(LOG, "PrimaryMirrorMode: Processing postmaster reset%s",
                transitionToFault ? " to fault state" : " to non-fault state");

    pmModuleState->requestedTransition = pmLocalState.mostRecentTransition;
    pmModuleState->requestedTransition.transitionToFault = transitionToFault;

    if ( pmModuleState->requestedTransition.dataState == DataStateInChangeTracking &&
         pmModuleState->requestedTransition.mode == PMModePrimarySegment &&

         pmLocalState.mostRecentSegmentStateWasChangeTrackingDisabled)
    {
        /**
         * before reset we were in change tracking disabled; continue that through to after the reset
         */
        pmModuleState->requestedTransition.transitionToChangeTrackingDisabledMode = true;
    }

    /* come after assigning requestedTransition, so we get the transition number set */
    resetModuleStateForInitialTransition();
}

/**
 *
 * @param extraResultInfoOut in the case of error, this may be populated with addition data (the
 *         RESULT_INFO_* values are the possible ones)
 */
PrimaryMirrorModeTransitionResult
requestTransitionToPrimaryMirrorMode(PrimaryMirrorModeTransitionArguments *args,
                                            char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
	volatile bool isTransitioning = false;
	PrimaryMirrorMode targetMode;
	extraResultInfoOut[0] = '\0';

	Assert(args != NULL);
	assertModuleInitialized();
	targetMode = args->mode;

	elog(LOG, "PrimaryMirrorTransitionRequest: to primary/mirror mode %s, "
	                "data state %s, host %s, port %d, peer %s, peerPort %d%s%s",
		 getMirrorModeLabel(targetMode), getDataStateLabel(args->dataState),
		 args->hostAddress, args->hostPort,
		 args->peerAddress, args->peerPort,
		 args->forceFullResync ? ", force full resync" : "",
		 args->transitionToFault ? ", transition to fault" : ""
		 );

	/* first, request the transition */
	PG_TRY();
	{
		acquireModuleSpinLock();
		{
		    args->transitionNumber = pmModuleState->transitionNumberCounter++;

			/**
			 * we can have one without the other because
			 *   a) postmaster will finish before listener
			 *                  (so isTransitionRequested = false, isTransitionListenerFinished = true)
			 *   OR
			 *
			 *   b) listener may die, but postmaster still needs to finish its work
			 *      (isTransitionRequested = true, isTransitionListenerFinished = false)
			 */
			if (pmModuleState->isTransitionRequested ||
				!pmModuleState->isTransitionListenerFinished)
			{
				elog(LOG, "PrimaryMirrorTransitionRequest (%d) Result: Cannot transition: modulestate is: %s %s",
				     args->transitionNumber,
					 (pmModuleState->isTransitionRequested ? "requested transition and" : ""),
					 (pmModuleState->isTransitionListenerFinished ?
					            "listener is finished" : "transition listener isn't finished"));
			}
			else
			{
				isTransitioning = true;
				pmModuleState->isTransitionRequested = true;
				pmModuleState->requestedTransition = *args;
				pmModuleState->isTransitionListenerFinished = false;
				pmModuleState->hasPostmasterCopiedTransitionToLocalMemory = false;
				pmModuleState->transitionState = TSNone;
                pmModuleState->requestedTransition.transitionNumber = args->transitionNumber;
			}
		}

		if (!isTransitioning)
		{
			releaseModuleSpinLock();

			elog(LOG, "PrimaryMirrorTransitionRequest (%d) Result: Transition to primary/mirror mode %s, "
			                "data state %s resulted in %s pending request(%s%s)",
				 args->transitionNumber,
				 getMirrorModeLabel(targetMode), getDataStateLabel(args->dataState),
				 getTransitionResultLabel(PMTransitionOtherTransitionInProgress),
				 getMirrorModeLabel(pmModuleState->requestedTransition.mode),
				 getDataStateLabel(pmModuleState->requestedTransition.dataState));
			return PMTransitionOtherTransitionInProgress;
		}

		/* see if the transition is a no-op */
		if (args->mode == pmModuleState->mode &&
			args->dataState == pmModuleState->dataState &&
			args->hostPort == pmModuleState->hostPort &&
			args->peerPort == pmModuleState->peerPort &&
			strcmp((char *)args->hostAddress, (char *)pmModuleState->hostAddress) == 0 &&
			strcmp((char *)args->peerAddress, (char *)pmModuleState->peerAddress) == 0 &&

			! args->forceFullResync /* a force full resync can never be a no-op */   
			)
		{
			pmModuleState->isTransitionListenerFinished = true;
			pmModuleState->isTransitionRequested = false;

			releaseModuleSpinLock();

			elog(LOG, "PrimaryMirrorTransitionRequest (%d) Result: Transition to primary/mirror mode %s, "
			                        "data state %s is as no-op",
				                    args->transitionNumber,
				                    getMirrorModeLabel(targetMode), 
				                    getDataStateLabel(args->dataState));
			return PMTransitionSuccess;
		}

		releaseModuleSpinLock(); /* set the postmaster loose */

		/* tell postmaster to wake up */
		SendPostmasterSignal(PMSIGNAL_PRIMARY_MIRROR_TRANSITION_RECEIVED);

		/* Wait for postmaster to apply transition and turn isTransitionRequested to false */
		int counter = 0;
		for (;;)
		{
			PrimaryMirrorModeTransitionResult result = PMTransitionSuccess;
			bool isDone = false;
			bool postmasterIsDead = false;

            /* sleep 10 ms, waiting for postmaster to do something
             * don't change the scale here without updating counter check below!
             */
			pg_usleep(10 * 1000L);

			/* every ~ 1 second, see if postmaster is gone */
			counter++;
			if ( counter == 100)
			{
			    if ( ! PostmasterIsAlive(true))
			        postmasterIsDead = true;
			    counter = 0;
			}

			acquireModuleSpinLock();
			{
			    if ( postmasterIsDead)
			    {
			        pmModuleState->isTransitionRequested = false;
			        pmModuleState->isTransitionListenerFinished = true;
			        result = PMTransitionError;
			        strncpy(extraResultInfoOut, RESULT_INFO_POSTMASTER_DIED, MAX_TRANSITION_RESULT_EXTRA_INFO);
			        isDone = true;
			    }
				else if (!pmModuleState->isTransitionRequested)
				{
					// postmaster completed the work
					pmModuleState->isTransitionListenerFinished = true;
					result = pmModuleState->transitionResult;
					strncpy(extraResultInfoOut, (char*) pmModuleState->transitionResultExtraInfo,
					        MAX_TRANSITION_RESULT_EXTRA_INFO);
					isDone = true;
				}
			}
			releaseModuleSpinLock();

			if (isDone)
			{
			    /* this won't get transitionToFault, because that 'succeeds', but that's okay because
			     *    that happens on a panic from in-sync anyway */
			    int logLevel = result == PMTransitionError ? WARNING : LOG;
				elog(logLevel, "PrimaryMirrorTransitionRequest (%d) Result: Transition to primary/mirror mode %s, "
				    "data state %s%s resulted in %s",
				        args->transitionNumber,
                        getMirrorModeLabel(targetMode), getDataStateLabel(args->dataState),
					    args->transitionToFault ? " (to fault)" : "",
					    getTransitionResultLabel(result));
				return result;
			}
		}
	}
	PG_CATCH();
	{
		elog(LOG, "PrimaryMirrorTransitionRequest: Caught exception in requestTransitionToPrimaryMirrorMode(), "
		                "isTransitioning %d", isTransitioning);

		if (isTransitioning)
		{
			acquireModuleSpinLock();
			{
				pmModuleState->isTransitionListenerFinished = true;
			}
			releaseModuleSpinLock();
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static bool
updateDataStateUnderSpinLock(DataState_e dataState)
{
    if (pmModuleState->dataState != dataState)
    {
        if ( dataState == DataStateInChangeTracking )
            pmModuleState->changeTrackingSessionId++;
        pmModuleState->dataState = dataState;
        return true;
    }
    return false;
}

int64
getChangeTrackingSessionId(void)
{
    assertModuleInitialized();
    return pmModuleState->changeTrackingSessionId;
}

/**
 * Update data state from an internal transition.  This should ONLY be called by
 *    filerep backend processes.
 */
bool
updateDataState(DataState_e dataState)
{
	bool isUpdated = FALSE;

	acquireModuleSpinLock();
	{
	    isUpdated = updateDataStateUnderSpinLock(dataState);
	}
	releaseModuleSpinLock();

	return isUpdated;
}

bool
updateSegmentState(SegmentState_e segmentState)
{
	bool isUpdated = FALSE;

	acquireModuleSpinLock();
	{
	    SegmentState_e curState = pmModuleState->segmentState;
	    if ( curState == SegmentStateImmediateShutdown )
	    {
	        /* never set to anything else during immediate shutdown */
	    }
	    else if (curState == SegmentStateShutdown )
        {
            /* allow promotion from shutdown to immediate shutdown */
            if ( segmentState == SegmentStateImmediateShutdown )
            {
                isUpdated = TRUE;
            }
        }
        else if ( curState == SegmentStateShutdownFilerepBackends)
        {
            /* allow promotion from backend shutdown to other shutdowns */
            if ( segmentState == SegmentStateShutdown ||
                 segmentState == SegmentStateImmediateShutdown)
            {
                isUpdated = TRUE;
            }
        }
		else if ( curState != segmentState )
		{
			/* During change tracking failure is reported by signaling postmaster Reset (no Fault) */
			if (! (segmentState == SegmentStateFault && 
				   pmModuleState->dataState == DataStateInChangeTracking))
			{					
				isUpdated = TRUE;
			}
		}
		if (isUpdated)
		{
            pmModuleState->segmentState = segmentState;
		}
	}
	releaseModuleSpinLock();
	
	return isUpdated;
}

/**
 * To be called by the postmaster.  The answer may become true immediately
 *  after this call but shouldn't (because postmaster is the only one who resets it)
 *  become false
 *
 * Does not acquire the lock as we are only peeking.
 */
bool
isPrimaryMirrorModeTransitionRequested(void)
{
    return pmModuleState->isTransitionRequested;
}

static void copyTransitionParametersToLocalMemoryIfNeeded(PrimaryMirrorModeTransitionArguments *args)
{
    Assert(pmModuleState->isTransitionRequested );

    if ( pmModuleState->hasPostmasterCopiedTransitionToLocalMemory)
    {
        return;
    }

    pmLocalState.mostRecentTransition = *args;
    pmLocalState.haveSetMostRecentTransition = true;
    pmModuleState->hasPostmasterCopiedTransitionToLocalMemory = true;
}

/**
 * Copy the transition input parameters into shared memory and reset isInFaultFromPostmasterReset
 */
static void
copyTransitionInputParameters(PrimaryMirrorModeTransitionArguments *args, SegmentState_e segmentState)
{
	acquireModuleSpinLock();
	{
		Assert(sizeof(args->hostAddress) == sizeof(pmModuleState->hostAddress));
		Assert(sizeof(args->peerAddress) == sizeof(pmModuleState->peerAddress));

		strcpy((char*) pmModuleState->hostAddress, args->hostAddress);
		pmModuleState->hostPort = args->hostPort;

		strcpy((char*) pmModuleState->peerAddress, args->peerAddress);
		pmModuleState->peerPort = args->peerPort;

		updateDataStateUnderSpinLock(args->dataState);
		pmModuleState->segmentState = segmentState;

		pmModuleState->isInFaultFromPostmasterReset = false;

        /* we've reached the point of no-return, so update this */
        pmModuleState->mode = args->mode;		
	}
	releaseModuleSpinLock();
}


/**
 * This will be called with signals blocked.
 */
void
doRequestedPrimaryMirrorModeTransitions(bool isInShutdown)
{
	PrimaryMirrorModeTransitionArguments requestedTransition;
	PrimaryMirrorModeTransitionResult result;
	PrimaryMirrorMode currentMode;
	DataState_e currentDataState;
	bool isRequested = false;
	bool isPrevRequestComplete = false;
	TransitionState currentState;
	int transitionNumber;
	bool transitionToFault;

	Assert(!IsUnderPostmaster);
	assertModuleInitialized();

	acquireModuleSpinLock();
	{
		isRequested = pmModuleState->isTransitionRequested;
		isPrevRequestComplete = pmModuleState->isTransitionListenerFinished;
		requestedTransition = pmModuleState->requestedTransition;
		currentMode = pmModuleState->mode;
		currentDataState = pmModuleState->dataState;
		currentState = pmModuleState->transitionState;
		transitionNumber = pmModuleState->requestedTransition.transitionNumber;
		transitionToFault = pmModuleState->requestedTransition.transitionToFault;
	}
	releaseModuleSpinLock();

	if (!isRequested /* && isPrevRequestComplete */)
	{
	    if ( ! isPrevRequestComplete )
	    {
	        elog(LOG, "PostmasterPrimaryMirrorTransition (%d): Nothing to do but listener is not done",
                        transitionNumber );
	    }
		return;
	}

	elog(DEBUG1, "PostmasterPrimaryMirrorTransition (%d): Attempting requested transition mode to %s, "
	                "data state to %s, state is currently %s",
	                transitionNumber,
		            getMirrorModeLabel(requestedTransition.mode),
		            getDataStateLabel(requestedTransition.dataState),
		            getTransitionStateLabel(currentState));

    char extraResultInfo[MAX_TRANSITION_RESULT_EXTRA_INFO];
    extraResultInfo[0] = '\0';
    if ( isInShutdown )
    {
	    elog(LOG, "PostmasterPrimaryMirrorTransition error: system is in shutdown");
	    strncpy(extraResultInfo, RESULT_INFO_IN_SHUTDOWN, MAX_TRANSITION_RESULT_EXTRA_INFO);
        result = PMTransitionError;
    }
    else if ( transitionToFault)
    {
        assertIsTransitioning();

        /* filerep should have been shut down by postmaster reset prior to this transition */
        Assert(!IsFilerepProcessRunning());

    	copyTransitionParametersToLocalMemoryIfNeeded(&requestedTransition);
        copyTransitionInputParameters(&requestedTransition, SegmentStateInitialization);

        /**
         * must be after copying the transition input parameters
         */
        pmModuleState->isInFaultFromPostmasterReset = true;
        
        if ( updateSegmentState(SegmentStateFault))
            NotifyProcessesOfFilerepStateChange();
        result = PMTransitionSuccess;
    }
    else
    {
        for ( ;; )
        {
            TransitionState initialState = currentState;

            switch (requestedTransition.mode)
            {
                case PMModeUninitialized:
                    result = applyStepForTransitionToUninitializedMode(&requestedTransition, &currentState, extraResultInfo);
                    break;
                case PMModeMaster:
                case PMModeMirrorlessSegment:
                    result = applyStepForTransitionToMasterOrMirrorlessMode(&requestedTransition, &currentState, extraResultInfo);
                    break;
                case PMModePrimarySegment:
                    result = applyStepForTransitionToPrimarySegmentMode(&requestedTransition, &currentState, extraResultInfo);
                    break;
                case PMModeQuiescentSegment:
                    result = applyStepForTransitionToQuiescentSegmentMode(&requestedTransition, &currentState, extraResultInfo);
                    break;
                case PMModeMirrorSegment:
                    result = applyStepForTransitionToMirrorSegmentMode(&requestedTransition, &currentState, extraResultInfo);
                    break;
                default:
                    Assert(!"Unknown targetMode");
                    result = PMTransitionInvalidTargetMode;
                    break;
            }

            if ( initialState != currentState)
            {
                acquireModuleSpinLock();
                {
                    pmModuleState->transitionState = currentState;
                }
                releaseModuleSpinLock();

                elog(LOG, "PostmasterPrimaryMirrorTransition (%d), after completed step, transition state is %s",
                    transitionNumber, getTransitionStateLabel(currentState));
            }

            Assert(result != PMTransitionOtherTransitionInProgress);

            if ( result != PMTransitionInvalidTargetMode &&
                 result != PMTransitionInvalidInputParameter )
            {
                /* if the result was  code that indicated that we are committed to this transition
                 *   then go ahead and copy it to local memory so that a postmaster reset will replay it */
                copyTransitionParametersToLocalMemoryIfNeeded(&requestedTransition);
            }

            if ( result != PMTransitionSuccess )
            {
                break;
            }

            if ( currentState == TSDone)
            {
                elog(LOG, "PostmasterPrimaryMirrorTransition (%d): transition to %s done",
                    transitionNumber,
                    getDataStateLabel(requestedTransition.dataState));
                result = PMTransitionSuccess;
                break;
            }
            else if ( currentState == initialState )
            {
                result = PMTransitionOkayButStillInProgress;
                break;
            }
        }
    }

	if (result == PMTransitionOkayButStillInProgress)
	{
		Assert(pmModuleState->transitionState != TSNone);
    	elog(DEBUG1, "PostmasterPrimaryMirrorTransition (%d): in-progress, current state %s",
	    	 transitionNumber,
	    	 getTransitionStateLabel(currentState));
	}
	else
	{
        int logLevel = result == PMTransitionError ? WARNING : LOG;
	    elog(logLevel, "PostmasterPrimaryMirrorTransition (%d) Finished with %s",
	        transitionNumber, getTransitionResultLabel(result));
		acquireModuleSpinLock();
		{
			pmModuleState->transitionResult = result;
			strncpy((char*)pmModuleState->transitionResultExtraInfo, extraResultInfo, MAX_TRANSITION_RESULT_EXTRA_INFO);
			pmModuleState->isTransitionRequested = false;
			elog(LOG, "PostmasterPrimaryMirrorTransition (%d): completed (waiting backend finished? %s)",
			                transitionNumber,
			                pmModuleState->isTransitionListenerFinished ? "yes" : "no");
			pmModuleState->transitionState = TSNone;
		}
		releaseModuleSpinLock();
	}

}

static PrimaryMirrorModeTransitionResult
stepDoDatabaseStartup(TransitionState nextState, TransitionState *stateInOut)
{
    Assert(!pmModuleState->haveStartedDatabase);

    pmModuleState->haveStartedDatabase = true;
    StartMasterOrPrimaryPostmasterProcesses();
	if ( IsDatabaseInRunMode())
	{
		pmModuleState->isDatabaseRunning = true;
	}	
    *stateInOut = TSDone;
    return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
stepDoFilerepStartup(TransitionState nextState, TransitionState *stateInOut)
{
#ifdef USE_ASSERT_CHECKING
    /* verify that filerep will get correct values when it asks for them */
    bool isInTransition = false;
    FileRepRole_e role = FileRepRoleNotInitialized;
    getFileRepRoleAndState(&role, NULL, NULL, &isInTransition, NULL);
    Assert(isInTransition);
    Assert(role == FileRepPrimaryRole || role == FileRepMirrorRole);
#endif

    StartFilerepProcesses();
    *stateInOut = nextState;
    return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
stepDoFilerepShutdown(TransitionState nextState, TransitionState *stateInOut)
{
    /* shutdown filerep */
    SignalShutdownFilerepProcess();
    *stateInOut = nextState;
    return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
stepDoFilerepBackendsShutdown(TransitionState nextState, TransitionState *stateInOut)
{
    /* shutdown filerep backend processes */
    SignalShutdownFilerepBackendProcesses();
    *stateInOut = nextState;
    return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
stepWaitForFilerepShutdown(TransitionState nextState, TransitionState *stateInOut)
{
    /* waiting for filerep to complete shutdown */
    if (IsFilerepProcessRunning())
    {
        elog(DEBUG1, "PostmasterPrimaryMirrorTransition: filerep still running, will wait longer");
    }
    else
    {
        /** note that at this point the postmaster will have set segment state to fault. */
        *stateInOut = nextState;
    }
    return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
stepWaitForFilerepBackendsShutdown(TransitionState nextState, TransitionState *stateInOut)
{
    /* waiting for filerep to complete shutdown of its backends */
    if (IsFilerepBackendsDoneShutdown())
    {
        *stateInOut = nextState;
    }
    else
    {
        elog(DEBUG1, "PostmasterPrimaryMirrorTransition: will wait longer for backends to shutdown");
    }
    return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
stepWaitForFilerepStartup(TransitionState nextState, TransitionState *stateInOut,
                char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
    SegmentState_e segmentState;
    getPrimaryMirrorStatusCodes(NULL, &segmentState, NULL);

    if (!IsFilerepProcessRunning() ||
        segmentState == SegmentStateFault)
    {
        elog(LOG, "PostmasterPrimaryMirrorTransition: filerep startup: transition error");
        if ( updateSegmentState(SegmentStateFault))
            NotifyProcessesOfFilerepStateChange();
        strncpy(extraResultInfoOut, RESULT_INFO_MIRRORING_FAILURE, MAX_TRANSITION_RESULT_EXTRA_INFO);
        return PMTransitionError;
    }

    if (segmentState == SegmentStateReady ||
        segmentState == SegmentStateChangeTrackingDisabled )
    {
        elog(LOG, "PostmasterPrimaryMirrorTransition: filerep startup: completed");
        *stateInOut = nextState;
    }
    else
    {
        elog(DEBUG1, "PostmasterPrimaryMirrorTransition: filerep startup: still starting up");
    }
    return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
invalidMode(PrimaryMirrorModeTransitionArguments *args)
{
    elog(LOG, "PostmasterPrimaryMirrorTransition: invalid target mode");
    return PMTransitionInvalidTargetMode;
}

/**
 * being in an invalid state is a programming error!
 */
static PrimaryMirrorModeTransitionResult
invalidState(TransitionState *stateInOut, char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
    elog(ERROR, "Invalid state %d", *stateInOut );
    strncpy(extraResultInfoOut, RESULT_INFO_INVALID_STATE_TRANSITION, MAX_TRANSITION_RESULT_EXTRA_INFO);
    return PMTransitionError;
}

/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyStepForTransitionToUninitializedMode(PrimaryMirrorModeTransitionArguments *args, TransitionState *stateInOut,
                        char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
	assertIsTransitioning();
	return invalidMode(args);
}

/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyStepForTransitionToMasterOrMirrorlessMode(PrimaryMirrorModeTransitionArguments *args, TransitionState *stateInOut,
                        char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
	assertIsTransitioning();

	switch ( *stateInOut )
	{
	    case TSNone:
	        XLogStartupInit(); /* do this as early as possible */

            switch (pmModuleState->mode)
            {
                case PMModeUninitialized:
                    *stateInOut = TSDoDatabaseStartup;
                    break;
                default:
                    return invalidMode(args);
            }
            return PMTransitionSuccess;

        case TSDoDatabaseStartup:
            copyTransitionInputParameters(args, SegmentStateNotInitialized);
            return stepDoDatabaseStartup(TSDone, stateInOut );

        default:
            return invalidState(stateInOut, extraResultInfoOut);
	}
}

/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyStepForTransitionToQuiescentSegmentMode(PrimaryMirrorModeTransitionArguments *args, TransitionState *stateInOut,
                        char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
	assertIsTransitioning();

    switch ( *stateInOut )
    {
        case TSNone:
            switch (pmModuleState->mode)
            {
                case PMModeMirrorSegment:
                    *stateInOut = TSDoFilerepBackendsShutdown;
                    return PMTransitionSuccess;
                case PMModeUninitialized:
                    *stateInOut = TSDone;
                    return PMTransitionSuccess;
                default:
                    return invalidMode(args);
            }

        case TSDoFilerepBackendsShutdown:
            return stepDoFilerepBackendsShutdown(TSAwaitingFilerepBackendsShutdown, stateInOut );
        case TSAwaitingFilerepBackendsShutdown:
            return stepWaitForFilerepBackendsShutdown(TSDoneFilerepBackendsShutdown, stateInOut );
        case TSDoneFilerepBackendsShutdown:
            *stateInOut = TSDoFilerepShutdown;
            return PMTransitionSuccess;

        case TSDoFilerepShutdown:
            return stepDoFilerepShutdown(TSAwaitingFilerepShutdown, stateInOut );
        case TSAwaitingFilerepShutdown:
            return stepWaitForFilerepShutdown(TSDoneFilerepShutdown, stateInOut );
        case TSDoneFilerepShutdown:

            *stateInOut = TSDone;
            copyTransitionInputParameters(args, SegmentStateNotInitialized);
            return PMTransitionSuccess;

        default:
            return invalidState(stateInOut, extraResultInfoOut);
    }
	/* else */
	return PMTransitionInvalidTargetMode;
}

/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyStepForTransitionToPrimarySegmentMode(PrimaryMirrorModeTransitionArguments *args, TransitionState *stateInOut,
                        char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
	assertIsTransitioning();

	switch ( *stateInOut )
	{
	    case TSNone:
            /* verify the transition mode */
            switch (pmModuleState->mode)
            {
                case PMModeMirrorSegment:
                case PMModePrimarySegment:
                case PMModeQuiescentSegment:
                case PMModeMirrorlessSegment:
                case PMModeUninitialized:
                    *stateInOut = TSDoFilerepBackendsShutdown;
                    return PMTransitionSuccess;
                default:
                    return invalidMode(args);
            }

        case TSDoFilerepBackendsShutdown:
            return stepDoFilerepBackendsShutdown(TSAwaitingFilerepBackendsShutdown, stateInOut );
        case TSAwaitingFilerepBackendsShutdown:
            return stepWaitForFilerepBackendsShutdown(TSDoneFilerepBackendsShutdown, stateInOut );
        case TSDoneFilerepBackendsShutdown:
            *stateInOut = TSDoFilerepShutdown;
            return PMTransitionSuccess;

        case TSDoFilerepShutdown:
            return stepDoFilerepShutdown(TSAwaitingFilerepShutdown, stateInOut );
        case TSAwaitingFilerepShutdown:
            return stepWaitForFilerepShutdown(TSDoneFilerepShutdown, stateInOut );
        case TSDoneFilerepShutdown:
            *stateInOut = TSDoFilerepStartup;
            return PMTransitionSuccess;

        case TSDoFilerepStartup:

            /* do this as early as possible -- must do it before any of the new filerep processes are built */
            XLogStartupInit();

            if ( args->transitionToChangeTrackingDisabledMode )
            {
                copyTransitionInputParameters(args, SegmentStateChangeTrackingDisabled );
                *stateInOut = TSDoDatabaseStartup;
                return PMTransitionSuccess;
            }
            else
            {
                SegmentState_e segState;
                if (args->dataState == DataStateInResync )
                {
                    if ( pmModuleState->dataState == DataStateNotInitialized )
                    {
                        /* starting up in resync -- just go to initialization */
                        segState = SegmentStateInitialization;
                    }
                    else
                    {
                        segState = SegmentStateInResyncTransition;
                    }
                }
                else
                {
                    segState = SegmentStateInitialization;
                }

                copyTransitionInputParameters(args, segState);
                return stepDoFilerepStartup(TSAwaitingFilerepStartup, stateInOut );
            }
        case TSAwaitingFilerepStartup:
            return stepWaitForFilerepStartup(TSDoneFilerepStartup, stateInOut, extraResultInfoOut );
        case TSDoneFilerepStartup:
            *stateInOut = TSDoDatabaseStartup;
            return PMTransitionSuccess;

        case TSDoDatabaseStartup:
            if ( ! pmModuleState->haveStartedDatabase )
            {
                Assert(!pmModuleState->haveStartedDatabase);
                elog(LOG, "PostmasterPrimaryMirrorTransition: starting database processes");
                pmModuleState->haveStartedDatabase = true;
                StartMasterOrPrimaryPostmasterProcesses();
            }

            *stateInOut = TSAwaitingDatabaseStartup;
            return PMTransitionSuccess;

        case TSAwaitingDatabaseStartup:
            if ( IsDatabaseInRunMode())
            {
                *stateInOut = TSDoneDatabaseStartup;
				pmModuleState->isDatabaseRunning = true;
            }
            return PMTransitionSuccess;

        case TSDoneDatabaseStartup:
            if ( args->dataState == DataStateInResync &&
				pmModuleState->dataState == DataStateInChangeTracking )
            {
				/* if filerep wants to be notified
				 * when XLOG crash recovery has completed then do that
				 */
			
				/*
				 * update segment states
				 */
				bool isNowInFault = false;
				acquireModuleSpinLock();
				{
					if ( pmModuleState->segmentState == SegmentStateFault )
					{
						isNowInFault = true;
					}
					else
					{
						updateDataStateUnderSpinLock(DataStateInResync);
						pmModuleState->segmentState = SegmentStateInResyncTransition;
					}
				}
				releaseModuleSpinLock();

				if ( isNowInFault )
				{
				    /* we are now in mirroring fault so we can't go to resync, report as mirroring failure */
				    strncpy(extraResultInfoOut, RESULT_INFO_MIRRORING_FAILURE, MAX_TRANSITION_RESULT_EXTRA_INFO);
					return PMTransitionError;
				}
				/** tell filerep processes that they can begin resync now! */
				NotifyProcessesOfFilerepStateChange();
			}
			
            *stateInOut = TSDone;
            return PMTransitionSuccess;

        default:
            return invalidState(stateInOut, extraResultInfoOut);
    }
}

/**
 */
static PrimaryMirrorModeTransitionResult
applyStepForTransitionToMirrorSegmentMode(PrimaryMirrorModeTransitionArguments *args, TransitionState *stateInOut,
                        char extraResultInfoOut[MAX_TRANSITION_RESULT_EXTRA_INFO])
{
	assertIsTransitioning();

	switch ( *stateInOut )
	{
        case TSNone:
            switch (pmModuleState->mode)
            {
                case PMModeQuiescentSegment:
                case PMModeUninitialized:
                    *stateInOut = TSDoFilerepStartup;
                    return PMTransitionSuccess;
                default:
                    return invalidMode(args);
            }

        case TSDoFilerepStartup:
            copyTransitionInputParameters(args, SegmentStateInitialization);
            return stepDoFilerepStartup(TSAwaitingFilerepStartup, stateInOut );
        case TSAwaitingFilerepStartup:
            return stepWaitForFilerepStartup(TSDoneFilerepStartup, stateInOut, extraResultInfoOut );
        case TSDoneFilerepStartup:
            *stateInOut = TSDone;
            return PMTransitionSuccess;

        default:
            return invalidState(stateInOut, extraResultInfoOut);
	}
}

void primaryMirrorRecordSegmentStateToPostmasterLocalMemory(void)
{
    Assert(!IsUnderPostmaster);

    SegmentState_e segmentState = pmModuleState->segmentState;
    switch ( segmentState )
    {
        case SegmentStateInitialization:
        case SegmentStateInResyncTransition:
        case SegmentStateInSyncTransition:
        case SegmentStateReady:
            pmLocalState.mostRecentSegmentStateWasChangeTrackingDisabled = false;
            break;

        case SegmentStateChangeTrackingDisabled:
            pmLocalState.mostRecentSegmentStateWasChangeTrackingDisabled = true;
            break;

        case SegmentStateNotInitialized:
		case SegmentStateInChangeTrackingTransition:
        case SegmentStateFault:
        case SegmentStateShutdownFilerepBackends:
        case SegmentStateShutdown:
        case SegmentStateImmediateShutdown:
            /* does not change mostRecentSegmentStateWasChangeTrackingDisabled */
             break;

         case SegmentState__EnumerationCount:
            Assert(0);
            break;
    }
}

/**
 * set the bgwriter pid.  Should only be called by the postmaster
 */
void primaryMirrorSetBGWriterPID( pid_t pid )
{
    assertModuleInitialized();

    /* updating single int field, no need for fancy locking */
    pmModuleState->bgWriterPID = pid;
}

/**
 * get the current bgwriter pid.  Note, of course, that the writer may crash by the time the caller
 *   can use this value -- so construct client code appropriately.
 */
pid_t primaryMirrorGetBGWriterPID(void)
{
    assertModuleInitialized();
    /* fetching single int field, no need for fancy locking */
    return pmModuleState->bgWriterPID;
}

