/*-------------------------------------------------------------------------
 *
 * primary_mirror_mode.h
 *	  Exports from primary_mirror_mode.c.
 *
 * Copyright (c) 2006-2009, Greenplum inc
 *
 * $PostgreSQL: pgsql/src/include/postmaster/primary_mirror_mode.h,v 1.20 2009/05/05 19:59:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PRIMARY_MIRROR_MODE_H
#define _PRIMARY_MIRROR_MODE_H

#include "c.h"
#include "postgres.h"

/*
 * Segment states are set only when mirroring is configured.  
 *	FileRepRole is either FileRepPrimary or FileRepMirrorRole.
 *
 * Note: if the order or # of values is changed then update the labels in primary_mirror_mode.c
 */
typedef enum DataState_e {
        DataStateNotInitialized=0,
		/* State has not been set */

        DataStateInSync,
		/* Primary and mirror are in sync */

        DataStateInChangeLogging,
		/* Primary is logging changing. Mirror is down. */

        DataStateInResync,
		/* Mirror is back. Changes are re-synchronized from primary. */

    	/* note that these must be in sync with gDataStateLabels inside the implementation file */
} DataState_e;

/**
 * Note: if the order or # of values is changed then update the labels in primary_mirror_mode.c
 */
typedef enum SegmentState_e {

        SegmentStateNotInitialized=0,
		/* State has not been set */

		SegmentStateInitialization,
		/* 
		 * FileRep initialization.
		 * Change Logging initialization. 
		 */ 

		SegmentStateRecovery,
		/* 
		 * WAL recovery, Change logging recovery.
		 */

		SegmentStateReady,
		/* Normal operation. */
	
		SegmentStateFault,
		/* 
		 * Fault is detected. Notify fts prober. 
		 * Waiting for third coordinator decision about recovery.
		 */
		SegmentStateShutdown,
		/* gracefull shutdown in progress. */
	
		SegmentStateImmediateShutdown 
		/* immediate shutdown in progress */
} SegmentState_e;


typedef enum FileRepRole_e {
    FileRepRoleNotInitialized=0,
	/* Configuration has not been set */

    FileRepNoRoleConfigured,
	/* Master or Mirroring is not configured.  */

    FileRepPrimaryRole,
	/* Mirroring is configured. Segment is in the role of primary. */

    FileRepMirrorRole
	/* Mirroring is configured. Segment is in the role of mirror. */
} FileRepRole_e;

/**
 * Note: if the order or # of values is changed then update the labels in primary_mirror_mode.c
 */
typedef enum
{
	/* PMModeUninitialized: the server's mode has never been set */
	PMModeUninitialized = 0,

	/* PMModeMaster: acting as a cluster master */
	PMModeMaster,

	/* A segment without an associated mirror */
	PMModeMirrorlessSegment,

	/* PMModeQuiescentSegment: a quiescent segment cannot receive work; it can only transition to other states (like PrimarySegment or MirrorSegment) */
	PMModeQuiescentSegment,

	/* PMModePrimarySegment: a primary segment can answer queries, and can also send file system updates to a mirror */
	PMModePrimarySegment,

	/* PMModeMirrorSegment: a mirror segment cannot answer queries; it can only wait for primary-segment connections and handle them */
	PMModeMirrorSegment

	/* note that these must be in sync with gModeLabels inside the implementation file */
} PrimaryMirrorMode;

/**
 * Note: if the order or # of values is changed then update the labels in primary_mirror_mode.c
 */
typedef enum
{
	/* Success! */
	PMTransitionSuccess = 0,

	/* The target mode is invalid given the current mode */
	PMTransitionInvalidTargetMode,

	/* Some other transition is in progress */
	PMTransitionOtherTransitionInProgress,

	/* an input parameter to the transition was wrong */
	PMTransitionInvalidInputParameter,

	/* an error happened during transition */
	PMTransitionError,

	/**
	 * The transition was okay but is still in progress.
	 *
	 * Note that this will only be used by the postmaster process while processing the request and never
	 *   actually returned to the original caller
	 */
	PMTransitionOkayButStillInProgress

	/* note that these must be in sync with gTransitionResultLabels inside the implementation file */
} PrimaryMirrorModeTransitionResult;

#define PM_MAX_HOST_NAME_LENGTH (200)

/*
 * This structure will live in shared memory so it must not use any allocation to build it!
 */
typedef struct
{
	PrimaryMirrorMode mode;
	DataState_e dataState;

	char hostAddress[PM_MAX_HOST_NAME_LENGTH+1];
	int hostPort;
	char peerAddress[PM_MAX_HOST_NAME_LENGTH+1];
	int peerPort;

    /* a unique identifer for logging, will be filled in by the call to requestTransition... */
	int transitionNumber;

	/* note that if more fields are added here, must be sure to update the no-op check in primary_mirror_mode.c */
} PrimaryMirrorModeTransitionArguments;

/* module initialization */
extern void primaryMirrorModeShmemInit(void);
extern Size primaryMirrorModeShmemSize(void);
extern void setInitialRequestedPrimaryMirrorMode( PrimaryMirrorMode targetMode );

/* returns PMModeUninitialized if the value could not be parsed as an initialized mode! */
extern PrimaryMirrorMode decipherPrimaryMirrorModeArgument(const char *arg);
/* transitioning */
extern PrimaryMirrorModeTransitionResult requestTransitionToPrimaryMirrorMode(PrimaryMirrorModeTransitionArguments *args );
extern void doRequestedPrimaryMirrorModeTransitions(void);
extern PrimaryMirrorModeTransitionArguments *createNewTransitionArguments(void);
extern void primaryMirrorGetFilerepArguments(
				FileRepRole_e	*fileRepRoleOut, 
				SegmentState_e	*segmentStateOut, 
				DataState_e		*dataStateOut,
				char **primaryHostAddressOut, 
				int *primaryPortOut,
				char **mirrorHostAddressOut, 
				int *mirrorPortOut);

/* state */
extern bool isPrimaryMirrorModeAFullPostmaster(bool checkTargetModeAsWell);
extern bool doesPrimaryMirrorModeRequireFilerepProcess(bool checkTargetModeAsWell);
extern void getFileRepRoleAndState(FileRepRole_e *fileRepRole, SegmentState_e *segmentState, DataState_e *dataState);

/* functions for the postmaster/filerep interaction */
extern bool updateConfig(SegmentState_e segmentState);

/* labeling enums */

extern const char * getTransitionResultLabel(PrimaryMirrorModeTransitionResult res);
extern void getPrimaryMirrorModeStatus( char *bufOut, int bufSize);
extern void getPrimaryMirrorStatusCodes(PrimaryMirrorMode *pm_mode, SegmentState_e *s_state, DataState_e *d_state);

extern const char *getDataStateLabel(DataState_e state);
extern const char *getMirrorModeLabel(PrimaryMirrorMode mode);
extern const char *getSegmentStateLabel(SegmentState_e state);

#endif   /* _PRIMARY_MIRROR_MODE_H */
