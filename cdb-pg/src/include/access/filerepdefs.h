/*
 *  cdbfilerepdefs.h
 *  
 * Definitions needed by in header files outside of cdb.
 *
 *  Copyright 2009-2010, Greenplum Inc. All rights reserved.
 *
 */

#ifndef FILEREPDEFS_H
#define FILEREPDEFS_H

typedef enum MirrorDataLossTrackingState {
	MirrorDataLossTrackingState_MirrorNotConfigured=0,
	
	MirrorDataLossTrackingState_MirrorCurrentlyUpInSync,
	
	MirrorDataLossTrackingState_MirrorCurrentlyUpInResync,
	
	MirrorDataLossTrackingState_MirrorDown,
	
} MirrorDataLossTrackingState;

#endif   /* FILEREPDEFS_H */
