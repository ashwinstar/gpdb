/*-------------------------------------------------------------------------
 *
 * cdbresynchronizechangelog.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBRESYNCHRONIZECHANGELOG_H
#define CDBRESYNCHRONIZECHANGELOG_H

#include "utils/palloc.h"
#include "storage/fd.h"

/*
 * This module is for initializing, recovering, writing, reading, checkpointing, and dropping the
 * mirror resynchronize change log.
 *
 * Multiple backends can write and read the resynchronize change log at the same time.
 */

// -----------------------------------------------------------------------------
// Creation, Recovery, and Drop
// -----------------------------------------------------------------------------

/*
 * Create the initial portion of the resynchronize change log after mirror loss by processing
 * the XLOG starting at the previous checkpoint.
 *
 * UNDONE: Talk about XLOG stability -- can have XLOG files disappearing or new XLOG writes
 * UNDONE: occurring since they would intermix and we can't handle that..
 */
extern void ResynchronizeChangeLog_CreateInitialFromPreviousCheckpoint(void);

/*
 * Recover the end of the resynchronize change log by determing the last good change record
 * and rebuilding from there.
 */
extern void ResynchronizeChangeLog_RecoverUsingXLog(void);

/*
 * Drop the resynchronize change log.
 */
extern void ResynchronizeChangeLog_Drop(void);

// -----------------------------------------------------------------------------
// Add 
// -----------------------------------------------------------------------------

/*
 * Add Buffer Pool change of a page.
 */
extern void ResynchronizeChangeLog_AddBufferPoolChange(
	XLogRecPtr		xlogLocation,
				/* The XLOG LSN of the record that describes the page change. */

	RelFileNode 	*relFileNode,
				/* The tablespace, database, and relation OIDs for the changed relation. */

	uint32			blockNum);
				/* The block number. */

/*
 * Add Append-Only change.
 *
 * Describes new data committed to the Append-Only relation between oldEof and newEof
 * that was not mirrored.
 */
extern void ResynchronizeChangeLog_AddAppendOnlyChange(
	XLogRecPtr		xlogLocation,
				/* The XLOG LSN of the record that describes the change. */

	RelFileNode 	*relFileNode,
				/* The tablespace, database, and relation OIDs for the changed relation. */

	uint32			segmentFileNum,
				/* The segment file number. */

	int64			oldEof,
				/* The old logical EOF value of the prior commit to the segment file. */
				
	int64			newEof);
				/* The new logical EOF value for the new commit to the segment file. */
							
/*
 * Add notification of an initial bulk change that was not made to the mirror.
 */
extern void ResynchronizeChangeLog_AddInitialBulkChange(
	XLogRecPtr		xlogLocation,
				/* The XLOG LSN of the record that describes the page change. */

	RelFileNode 	*relFileNode);
				/* The tablespace, database, and relation OIDs for the changed relation. */
				
				
// -----------------------------------------------------------------------------
// Reading 
// -----------------------------------------------------------------------------

typedef struct AppendOnlyChange
{
	uint32	segmentFileNum;
	int64	oldEof;
	int64	newEof;
} AppendOnlyChange;
				
typedef struct IncrementalChangeRecord
{
	XLogRecPtr		xlogLocation;

	RelFileNode 	relFileNode,
				/* The tablespace, database, and relation OIDs for the changed relation. */

	uint8 		   change;

	union
	{
		/*
		* BufferPool
		*/
		uint32		bufferPoolBlockNum;

		/*
		* AppendOnly
		*/
		AppendOnlyChange appendOnlyChange;
	} detail;
} IncrementalChangeRecord;

/*
 * Set the next read position.
 */
extern void ResynchronizeChangeLog_SetReadPosition(
	int64		position);
				/* The new read position. */
				
/*
 * Read the next change.
 */
extern bool ResynchronizeChangeLog_ReadNext(
	IncrementalChangeRecord		changeRecord);
				/* The change record found. */
								
/*
 * Get the next read position.
 */
extern void ResynchronizeChangeLog_GetReadPosition(
	int64		*position);
				/* The next read position. */
								
// -----------------------------------------------------------------------------
// Checkpointing and Shutting Down
// -----------------------------------------------------------------------------
				
/*
 * Flush the reschronize change log to disk for checkpoint.
 */
extern void ResynchronizeChangeLog_CheckpointFlush(void);
												
/*
 * Shutdown the reschronize change log.
 */
extern void ResynchronizeChangeLog_Shutdown(void);
																								
// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------
				
/*
 * Return the required shared-memory size for this module.
 */
extern Size ResynchronizeChangeLog_ShmemSize(void);
								
/*
 * Initialize the shared-memory for this module.
 */
extern void ResynchronizeChangeLog_ShmemInit(void);
																
#endif   /* CDBRESYNCHRONIZECHANGELOG_H */

