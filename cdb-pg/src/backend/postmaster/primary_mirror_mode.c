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
#include <string.h>

/**
 * todo MIRRORING: errors during shutdown of filerep are handled properly?
 * todo MIRRORING: if we crash in the middle of a transition then how do we recover properly (so we don't have the spinlock in a permanently locked state)
 * todo MIRRORING: do we need to enter critical sections to make sure shared state is not messed up?
 * todo MIRRORING: error when doRequestedPrimaryMirrorModeTransitions should cause a panic?  Or at least should wake up requestXXX.
 * todo MIRRORING: proper locking around get/set of segmentState and others -- because filerep can fault the state while postmaster is updating it!
 * todo MIRRORING: do a review of locking
 */

/**
 * The transition progress is used when the postmaster needs to return back to the server loop.
 *   The primary/mirror mode function will mark its transition as being in-progress...
 *   later calls to doRequestedPrimaryMirrorModeTransition will check this field.
 *
 * Note: if the order or # of values is changed then update the labels below
 */
typedef enum
{
        TPNone,
        TPAwaitingFilerepShutdown,
        TPDoneFilerepShutdown,
        TPAwaitingFilerepProcessStartup,
        TPDoneFilerepProcessStartup

        /* note: these should be in sync with gTransitionProgressLabels */
} TransitionProgress;

/* State management */
static void assertModuleInitialized(void);
static void assertIsTransitioning(void);

/* actual transitions */
static PrimaryMirrorModeTransitionResult applyTransitionToUninitializedMode(TransitionProgress *progressInOut);
static PrimaryMirrorModeTransitionResult applyTransitionToMasterMode(TransitionProgress *progressInOut);
static PrimaryMirrorModeTransitionResult applyTransitionToQuiescentSegmentMode(TransitionProgress *progressInOut);
static PrimaryMirrorModeTransitionResult applyTransitionToPrimarySegmentMode(PrimaryMirrorModeTransitionArguments *args, TransitionProgress *progressInOut);
static PrimaryMirrorModeTransitionResult applyTransitionToMirrorSegmentMode(PrimaryMirrorModeTransitionArguments *args, TransitionProgress *progressInOut);
static PrimaryMirrorModeTransitionResult applyTransitionToMirrorlessSegmentMode(TransitionProgress *progressInOut);

/**
 * All globals for this module live in PMModuleState, which is allocated in shared memory.
 */
typedef struct PMModuleState
{
	/** all accesses to the isTransitionRequested and requestedTransition must lock on this */
	slock_t    lock;

	volatile bool isTransitionRequested;
	volatile PrimaryMirrorModeTransitionArguments requestedTransition;
	volatile bool isTransitionListenerFinished;
	volatile PrimaryMirrorModeTransitionResult transitionResult;
	volatile TransitionProgress transitionProgress;

	volatile PrimaryMirrorMode mode;
	volatile bool haveStartedDatabase;

	/***** CONFIGURATION options ******/
	/* connection info for self (which can be a mirror or primary, depending on mode) */
	volatile char hostAddress[PM_MAX_HOST_NAME_LENGTH+1];
	volatile int hostPort;        /* filerep port */

	/* connection info for the peer (which can be a mirror or primary, depending on mode) */
	volatile char peerAddress[PM_MAX_HOST_NAME_LENGTH+1];
	volatile int peerPort; /* filerep port */

	volatile SegmentState_e	  segmentState;
	volatile DataState_e	dataState;

	volatile int transitionNumberCounter;
} PMModuleState;

static volatile PMModuleState *pmModuleState = NULL;

/**
 * LABELS and parsing of values
 */

/**
 * Get the label for a primary mirror mode.  Returns a pointer to static memory.
 *
 * Note that the label is externally visible (through a getStatus call to the postmaster, for example)
 */
const char *
getMirrorModeLabel(PrimaryMirrorMode mode)
{
	static const char *labels[] =
	{
		"Uninitialized", "Master", "MirrorlessSegment",
		"QuiescentSegment", "PrimarySegment", "MirrorSegment"
	};

	return labels[mode];
}

/**
 * Get the label for a data state.  Returns a pointer to static memory.
 *
 * Note that the label is externally visible (through a getStatus call to the postmaster, for example)
 */
const char *
getDataStateLabel(DataState_e state)
{
	static const char *labels[] =
	{
			"NotInitialized", "InSync", "InChangeLogging", "InResync"
	};

	return labels[state];
}

/**
 * Note that the label is externally visible (through a getStatus call to the postmaster, for example)
 */
const char *
getSegmentStateLabel(SegmentState_e state)
{
	static const char *labels[] =
	{
		"NotInitialized", "Initialization", "Recovery", "Ready", "Fault", "Shutdown", "ImmediateShutdown"
	};

	return labels[state];
}

/**
 * Get the label for a transition result.  Returns a pointer to static memory.
 */
const char *
getTransitionResultLabel(PrimaryMirrorModeTransitionResult res)
{
	static const char *labels[] =
	{
		"Success", "InvalidTargetMode", "OtherTransitionInProgress",
		"InvalidInputParameter", "Error", "OkayButStillInProgress"
	};

	return labels[res];
}

/**
 * Get the label for transition progress.  Returns a pointer to static memory.
 */
static const char *
getTransitionProgressLabel(TransitionProgress progress)
{
	static const char *labels[] =
	{
		"None", "AwaitingFilerepShutdown", "DoneFilerepShutdown", "AwaitingFilerepProcessStartup", "DoneFilerepProcessStartup"
	};

	return labels[progress];
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
 * This is called in a context in which it is safe to look at data without getting the lock....
 * todo: convert from spinlock to mutex so we can lock here for safety
 */
void
primaryMirrorGetFilerepArguments(
			FileRepRole_e	*fileRepRoleOut, 
			SegmentState_e	*segmentStateOut, 
			DataState_e		*dataStateOut,
			char **primaryHostAddressOut,
			int *primaryPortOut,
			char **mirrorHostAddressOut, 
			int *mirrorPortOut)
{
	PrimaryMirrorMode mode;
	volatile PrimaryMirrorModeTransitionArguments *args;

	assertModuleInitialized();

	/* todo: only fetch mode from transition in some cases (or never?) */
	args = &pmModuleState->requestedTransition;
	mode = args->mode;

	if (mode == PMModePrimarySegment)
	{
		*fileRepRoleOut = FileRepPrimaryRole;
		copyHostData(primaryHostAddressOut, primaryPortOut, (char*)args->hostAddress, args->hostPort);
		copyHostData(mirrorHostAddressOut, mirrorPortOut, (char*)args->peerAddress, args->peerPort);
		*dataStateOut = args->dataState;
		*segmentStateOut = pmModuleState->segmentState;
	}
	else if (mode == PMModeMirrorSegment)
	{
		*fileRepRoleOut = FileRepMirrorRole;
		copyHostData(primaryHostAddressOut, primaryPortOut, (char*)args->peerAddress, args->peerPort);
		copyHostData(mirrorHostAddressOut, mirrorPortOut, (char*)args->hostAddress, args->hostPort);
		*dataStateOut = args->dataState;
		*segmentStateOut = pmModuleState->segmentState;
	}
	else
	{
		*fileRepRoleOut = FileRepNoRoleConfigured;
		*primaryHostAddressOut = *mirrorHostAddressOut = NULL;
		*primaryPortOut = *mirrorPortOut = 0;
		*segmentStateOut = SegmentStateNotInitialized;
		*dataStateOut = DataStateNotInitialized;
	}
}

void
getFileRepRoleAndState(
			FileRepRole_e	*fileRepRole, 
			SegmentState_e	*segmentState,
			DataState_e		*dataState)
{
	assertModuleInitialized();

	acquireModuleSpinLock();
	{
		/* todo: only fetch mode from transition in some cases (or never?) */
		volatile PrimaryMirrorModeTransitionArguments *args = &pmModuleState->requestedTransition;
		PrimaryMirrorMode mode = args->mode;

		switch (mode)
		{
			case PMModePrimarySegment:
				*fileRepRole = FileRepPrimaryRole;
				*dataState = pmModuleState->dataState;
				*segmentState = pmModuleState->segmentState;
				break;
			case PMModeMirrorSegment:
				*fileRepRole = FileRepMirrorRole;
				*dataState = pmModuleState->dataState;
				*segmentState = pmModuleState->segmentState;
				break;
			default:
				*fileRepRole = FileRepNoRoleConfigured;
				*dataState = DataStateNotInitialized;
				*segmentState = SegmentStateNotInitialized;
				break;
		}
	}
	releaseModuleSpinLock();
}

static void
assertIsTransitioning(void)
{
	assertModuleInitialized();
	Assert((pmModuleState->isTransitionRequested || !pmModuleState->isTransitionListenerFinished) && "transition must be marked in-progress");
}

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
	pmModuleState = (PMModuleState *) ShmemAlloc(sizeof(PMModuleState));
	pmModuleState->isTransitionRequested = false;
	pmModuleState->mode = PMModeUninitialized;
	pmModuleState->segmentState = SegmentStateNotInitialized;
	pmModuleState->dataState = DataStateNotInitialized;
	pmModuleState->isTransitionListenerFinished = true;
	pmModuleState->transitionProgress = TPNone;

	pmModuleState->hostAddress[0] = '\0';
	pmModuleState->hostPort = 0;
	pmModuleState->peerAddress[0] = '\0';
	pmModuleState->peerPort = 0;

	pmModuleState->haveStartedDatabase = false;
	pmModuleState->transitionNumberCounter = 1;

	clearTransitionArgs(&pmModuleState->requestedTransition);

	SpinLockInit(&pmModuleState->lock);
}

void
setInitialRequestedPrimaryMirrorMode(PrimaryMirrorMode targetMode)
{
	Assert(pmModuleState->mode == PMModeUninitialized);
	Assert(targetMode != PMModePrimarySegment && targetMode != PMModeMirrorSegment);

	pmModuleState->isTransitionRequested = true;
	pmModuleState->requestedTransition.mode = targetMode;
	pmModuleState->isTransitionListenerFinished = true; /* yes, true, because this is non-blocking */
	pmModuleState->transitionProgress = TPNone;
    pmModuleState->requestedTransition.transitionNumber = pmModuleState->transitionNumberCounter++;
}

PrimaryMirrorModeTransitionResult
requestTransitionToPrimaryMirrorMode(PrimaryMirrorModeTransitionArguments *args)
{
	volatile bool isTransitioning = false;
	PrimaryMirrorMode targetMode;

	Assert(args != NULL);
	assertModuleInitialized();
	targetMode = args->mode;

	elog(LOG, "TransitionRequest: to primary/mirror mode %s, data state %s,  host %s, port %d, peer %s, peerPort %d",
		 getMirrorModeLabel(targetMode), getDataStateLabel(args->dataState),
		 args->hostAddress, args->hostPort,
		 args->peerAddress, args->peerPort
		 );

	/* first, request the transition */
	PG_TRY();
	{
		acquireModuleSpinLock();
		{
		    args->transitionNumber = pmModuleState->transitionNumberCounter++;

			/**
			 * we can have one without the other because
			 *   a) postmaster will finish before listener (so isTransitionRequested = false, isTransitionListenerFinished = true)
			 *   b) listener may die, but postmaster still needs to finish its work (isTransitionRequested = true, isTransitionListenerFinished = false)
			 */
			if (pmModuleState->isTransitionRequested ||
				!pmModuleState->isTransitionListenerFinished)
			{
				elog(LOG, "TransitionResult (%d): Cannot transition: modulestate is: %s %s",
				     args->transitionNumber,
					 (pmModuleState->isTransitionRequested ? "requested transition and" : ""),
					 (pmModuleState->isTransitionListenerFinished ? "listener is finished" : "transition listener isn't finished"));
			}
			else
			{
				isTransitioning = true;
				pmModuleState->isTransitionRequested = true;
				pmModuleState->requestedTransition = *args;
				pmModuleState->isTransitionListenerFinished = false;
				pmModuleState->transitionProgress = TPNone;
                pmModuleState->requestedTransition.transitionNumber = args->transitionNumber;
			}
		}

		if (!isTransitioning)
		{
			releaseModuleSpinLock();

			elog(LOG, "TransitionResult (%d): Transition to primary/mirror mode %s, data state %s resulted in %s pending request(%s%s)",
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
			strcmp((char *)args->peerAddress, (char *)pmModuleState->peerAddress) == 0)
		{
			pmModuleState->isTransitionListenerFinished = true;
			pmModuleState->isTransitionRequested = false;

			releaseModuleSpinLock();

			elog(LOG, "TransitionResult (%d): Transition to primary/mirror mode %s, data state %s is as no-op",
				 args->transitionNumber, getMirrorModeLabel(targetMode), getDataStateLabel(args->dataState));
			return PMTransitionSuccess;
		}

		releaseModuleSpinLock(); /* set the postmaster loose */

		/* tell postmaster to wake up */
		SendPostmasterSignal(PMSIGNAL_PRIMARY_MIRROR_TRANSITION_RECEIVED);

		/* Wait for postmaster to apply transition and turn isTransitionRequested to false */
		for (;;)
		{
			PrimaryMirrorModeTransitionResult result = PMTransitionSuccess;
			bool isDone = false;

			pg_usleep(1000L); /* sleep 1 ms, waiting for postmaster to do something */

			acquireModuleSpinLock();
			{
				if (!pmModuleState->isTransitionRequested)
				{
					// postmaster completed the work
					pmModuleState->isTransitionListenerFinished = true;
					result = pmModuleState->transitionResult;
					isDone = true;
				}
			}
			releaseModuleSpinLock();

			if (isDone)
			{
				elog(LOG, "TransitionResult: Transition to primary/mirror mode %s, data state %s resulted in %s",
					 getMirrorModeLabel(targetMode), getDataStateLabel(args->dataState),
					 getTransitionResultLabel(result));
				return result;
			}
		}
	}
	PG_CATCH();
	{
		elog(LOG, "Caught exception in requestTransitionToPrimaryMirrorMode(), isTransitioning %d", isTransitioning);

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

bool
updateConfig(SegmentState_e segmentState)
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
		else if ( curState != segmentState )
		{
		    isUpdated = TRUE;
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

/**
 * This will be called with signals blocked.
 */
void
doRequestedPrimaryMirrorModeTransitions(void)
{
	PrimaryMirrorModeTransitionArguments requestedTransition;
	PrimaryMirrorModeTransitionResult result;
	PrimaryMirrorMode currentMode;
	DataState_e currentDataState;
	bool isRequested = false;
	bool isPrevRequestComplete = false;
	TransitionProgress currentProgress;
	int transitionNumber;

	Assert(!IsUnderPostmaster);
	assertModuleInitialized();

	acquireModuleSpinLock();
	{
		isRequested = pmModuleState->isTransitionRequested;
		isPrevRequestComplete = pmModuleState->isTransitionListenerFinished;
		requestedTransition = pmModuleState->requestedTransition;
		currentMode = pmModuleState->mode;
		currentDataState = pmModuleState->dataState;
		currentProgress = pmModuleState->transitionProgress;
		transitionNumber = pmModuleState->requestedTransition.transitionNumber;
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

	elog(LOG, "PostmasterPrimaryMirrorTransition (%d): Attempting requested transition mode to %s, data state to %s, progress is currently %s",
	     transitionNumber,
		 getMirrorModeLabel(requestedTransition.mode),
		 getDataStateLabel(requestedTransition.dataState),
		 getTransitionProgressLabel(currentProgress));

	switch (requestedTransition.mode)
	{
		case PMModeUninitialized:
			result = applyTransitionToUninitializedMode(&currentProgress);
			break;
		case PMModeMaster:
			result = applyTransitionToMasterMode(&currentProgress);
			break;
		case PMModeMirrorlessSegment:
			result = applyTransitionToMirrorlessSegmentMode(&currentProgress);
			break;
		case PMModeQuiescentSegment:
			result = applyTransitionToQuiescentSegmentMode(&currentProgress);
			break;
		case PMModePrimarySegment:
			result = applyTransitionToPrimarySegmentMode(&requestedTransition, &currentProgress);
			break;
		case PMModeMirrorSegment:
			result = applyTransitionToMirrorSegmentMode(&requestedTransition, &currentProgress);
			break;
		default:
			Assert(false && "Unknown targetMode");
			result = PMTransitionInvalidTargetMode;
			break;
	}


	if (result == PMTransitionOkayButStillInProgress)
	{
		Assert(currentProgress != TPNone);
		pmModuleState->transitionProgress = currentProgress;
    	elog(LOG, "PostmasterPrimaryMirrorTransition (%d): in-progress, current progress %s",
	    	 transitionNumber,
	    	 getTransitionProgressLabel(currentProgress));
	}
	else
	{
	    elog(LOG, "PostmasterPrimaryMirrorTransition (%d) Finished with %s", 
	        transitionNumber,getTransitionResultLabel(result));
		acquireModuleSpinLock();
		{
			if (result == PMTransitionSuccess)
			{
				pmModuleState->mode = requestedTransition.mode;
			}
			pmModuleState->transitionResult = result;
			pmModuleState->isTransitionRequested = false;
			elog(LOG, "PostmasterPrimaryMirrorTransition (%d): cleared isTransitionRequested (backend finished is %d)",
			                transitionNumber,
			                pmModuleState->isTransitionListenerFinished);
			pmModuleState->transitionProgress = TPNone;
		}
		releaseModuleSpinLock();
	}

}

/**
 * Copy the transition input parameters into shared memory, doing any needed validation.
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
		pmModuleState->dataState = args->dataState;
		pmModuleState->segmentState = segmentState;
	}
	releaseModuleSpinLock();
}

/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyTransitionToUninitializedMode(TransitionProgress *progressInOut)
{
	assertIsTransitioning();

	return PMTransitionInvalidTargetMode;
}

/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyTransitionToMasterMode(TransitionProgress *progressInOut)
{
	assertIsTransitioning();

	if (pmModuleState->mode != PMModeUninitialized)
	{
		elog(LOG, "PostmasterPrimaryMirrorTransition: applyTransitionToMasterSegmentMode: invalid target mode");
		return PMTransitionInvalidTargetMode;
	}

	Assert(!pmModuleState->haveStartedDatabase);

	pmModuleState->haveStartedDatabase = true;
	StartMasterOrPrimaryPostmasterProcesses();
	return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
doFilerepShutdownTransitionStep(TransitionProgress *progressInOut)
{
	Assert(*progressInOut != TPDoneFilerepShutdown);

	if (*progressInOut != TPAwaitingFilerepShutdown)
	{
		Assert(*progressInOut == TPNone);
		SignalShutdownFilerepProcess();
		*progressInOut = TPAwaitingFilerepShutdown;
	}

	Assert(*progressInOut == TPAwaitingFilerepShutdown);
	if (IsFilerepProcessRunning())
	{
        elog(LOG, "PostmasterPrimaryMirrorTransition: filerep still running, will wait longer");
		return PMTransitionOkayButStillInProgress;
	}

	*progressInOut = TPDoneFilerepShutdown;
	return PMTransitionSuccess;
}

static PrimaryMirrorModeTransitionResult
doFilerepStartupTransitionStep(PrimaryMirrorModeTransitionArguments *args, TransitionProgress *progressInOut)
{
	Assert(*progressInOut != TPDoneFilerepProcessStartup);

	if (*progressInOut != TPAwaitingFilerepProcessStartup)
	{
        elog(LOG, "PostmasterPrimaryMirrorTransition: launching filerep");

		copyTransitionInputParameters(args, SegmentStateInitialization);
		StartFilerepProcesses();

		*progressInOut = TPAwaitingFilerepProcessStartup;
		return PMTransitionOkayButStillInProgress;
	}
	else
	{
		if (!IsFilerepProcessRunning() ||
			pmModuleState->segmentState == SegmentStateFault)
		{
			elog(LOG, "PostmasterPrimaryMirrorTransition: doFilerepStartupTransitionStep: transition error");
			/* todo MIRRORING, if in fault then we should shut it down! */
			/* todo MIRRORING: Crash detection should have set segmentState to fault (instead of here) */
			pmModuleState->segmentState = SegmentStateFault;
			return PMTransitionError;
		}
		else if (pmModuleState->segmentState == SegmentStateReady)
		{
			elog(LOG, "PostmasterPrimaryMirrorTransition: filerep is up");
			*progressInOut = TPDoneFilerepProcessStartup;
			return PMTransitionSuccess;
		}
		else
		{
			elog(LOG, "PostmasterPrimaryMirrorTransition: filerep is still starting up");
			return PMTransitionOkayButStillInProgress;
		}
	}
}


/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyTransitionToQuiescentSegmentMode(TransitionProgress *progressInOut)
{
	assertIsTransitioning();

	if (pmModuleState->mode == PMModeUninitialized)
	{
		return PMTransitionSuccess;
	}
	else if (pmModuleState->mode == PMModeMirrorSegment)
	{
		return doFilerepShutdownTransitionStep(progressInOut);
	}

	/* else */
	return PMTransitionInvalidTargetMode;
}

/**
 * Assuming we are the only transition running, try a transition
 */
static PrimaryMirrorModeTransitionResult
applyTransitionToPrimarySegmentMode(PrimaryMirrorModeTransitionArguments *args, TransitionProgress *progressInOut)
{
	assertIsTransitioning();

	/* verify the transition */
	if (*progressInOut == TPNone)
	{
		switch (pmModuleState->mode)
		{
			case PMModeMirrorSegment:
			case PMModePrimarySegment:
			case PMModeQuiescentSegment:
			case PMModeUninitialized:
				break;
			default:
				elog(LOG, "PostmasterPrimaryMirrorTransition: applyTransitionToPrimarySegmentMode: invalid target mode");
				return PMTransitionInvalidTargetMode;
		}
	}

	/* shutdown previous filerep if needed */
	if (*progressInOut == TPNone || *progressInOut == TPAwaitingFilerepShutdown)
	{
		PrimaryMirrorMode previousMode = pmModuleState->mode;
	
		if (previousMode == PMModeMirrorSegment || previousMode == PMModePrimarySegment)
		{
			if (pmModuleState->dataState != DataStateInChangeLogging)
			{
				PrimaryMirrorModeTransitionResult res = doFilerepShutdownTransitionStep(progressInOut);

				if (res != PMTransitionSuccess)
				{
					elog(LOG, "PostmasterPrimaryMirrorTransition: applyTransitionToPrimarySegmentMode: unsuccessful transition, returning %d", res);
					return res;
				}
			}
		}
	}

	/* start up filerep processes as needed */
	if (args->dataState != DataStateInChangeLogging)
	{
		if (*progressInOut != TPDoneFilerepProcessStartup)
		{
			PrimaryMirrorModeTransitionResult res = doFilerepStartupTransitionStep(args, progressInOut);

			if (res != PMTransitionSuccess)
			{
				elog(LOG, "PostmasterPrimaryMirrorTransition: applyTransitionToPrimarySegmentMode: unsuccessful transition, returning %d", res);
				return res;
			}
		}
	}
	else
	{
		/* in change-logging there is no need to start up filerep */
	    copyTransitionInputParameters(args, SegmentStateNotInitialized);
	}

	if (!pmModuleState->haveStartedDatabase)
	{
        elog(LOG, "PostmasterPrimaryMirrorTransition: starting database processes");
	    pmModuleState->haveStartedDatabase = true;
		StartMasterOrPrimaryPostmasterProcesses();
	}

    elog(LOG, "PostmasterPrimaryMirrorTransition: transition to primary done");
	return PMTransitionSuccess;
}

/**
 */
static PrimaryMirrorModeTransitionResult
applyTransitionToMirrorSegmentMode(PrimaryMirrorModeTransitionArguments *args, TransitionProgress *progressInOut)
{
	assertIsTransitioning();

	if (*progressInOut == TPNone)
	{
		switch (pmModuleState->mode)
		{
			case PMModeMirrorSegment:
			case PMModeQuiescentSegment:
			case PMModeUninitialized:
				break;
			default:
				elog(LOG, "PostmasterPrimaryMirrorTransition: applyTransitionToMirrorSegmentMode: invalid target mode %s for progress %s", getMirrorModeLabel(pmModuleState->mode), getTransitionProgressLabel(*progressInOut));
				return PMTransitionInvalidTargetMode;
		}
	}

	elog(LOG, "PostmasterPrimaryMirrorTransition: applyTransitionToMirrorSegmentMode: doing startup sets, target mode %s for progress %s", getMirrorModeLabel(pmModuleState->mode), getTransitionProgressLabel(*progressInOut));

	return doFilerepStartupTransitionStep(args, progressInOut);
}

static PrimaryMirrorModeTransitionResult
applyTransitionToMirrorlessSegmentMode(TransitionProgress *progressInOut)
{
	assertIsTransitioning();

	if (pmModuleState->mode != PMModeUninitialized)
	{
		elog(LOG, "PostmasterPrimaryMirrorTransition: applyTransitionToMirrorlessSegmentMode: invalid target mode");
		return PMTransitionInvalidTargetMode;
	}

	Assert(!pmModuleState->haveStartedDatabase);

	pmModuleState->haveStartedDatabase = true;
	StartMasterOrPrimaryPostmasterProcesses();
	return PMTransitionSuccess;
}
