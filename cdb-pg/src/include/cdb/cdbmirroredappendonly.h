/*-------------------------------------------------------------------------
 *
 * cdbmirroredappendonly.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBMIRROREDAPPENDONLY_H
#define CDBMIRROREDAPPENDONLY_H

#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"

/*
 * This module is for doing mirrored writes for Append-Only relation files privately being written
 * by a backend process.
 *
 * It is intended for Append-Only relation files not under the management of the Buffer Pool.
 */

/*
 * This structure contains write open information.  Consider the fields
 * inside to be private.
 */
typedef struct MirroredAppendOnlyOpen
{
	bool	isActive;

	RelFileNode	relFileNode;
	
	uint32		segmentFileNum;
	
	File		primaryFile;

} MirroredAppendOnlyOpen;

#define MirroredAppendOnlyOpen_Init {FALSE, {0, 0, 0}, 0, -1}

// -----------------------------------------------------------------------------
// Open, Flush, and Close 
// -----------------------------------------------------------------------------

/*
 * Open an Append-Only relation for mirrored append.
 */
extern void MirroredAppendOnly_Open(
	MirroredAppendOnlyOpen	*open,
				/* The resulting open struct. */

	RelFileNode 	*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	int32			segmentFileNum,
				/* Which segment file. */

	int64			logicalEof,
				/* The logical EOF to begin appending the new data. */
									
	int				fileFlags,
									
	int				fileMode);
				/* The resulting open struct. */

extern bool MirroredAppendOnly_IsActive(
					MirroredAppendOnlyOpen *open);


/*
 * Flush and Close an Append-Only relation file.
 *
 * If the flush is unable to complete on the mirror, then information (segment file, old EOF, 
 * new EOF) on the new Append-Only data will be added to the commit, distributed commit,
 * distributed prepared and commit prepared XLOG records so that data can be resynchronized
 * later.
 */
extern void MirroredAppendOnly_FlushAndClose(
						MirroredAppendOnlyOpen *open);
							/* The open struct. */
				
/*
 * Flush and Close an Append-Only relation file.
 */
extern void MirroredAppendOnly_Close(
						 MirroredAppendOnlyOpen *open);
							/* The open struct. */



// -----------------------------------------------------------------------------
// Append 
// -----------------------------------------------------------------------------
				
/*
 * Append mirrored.
 */
extern void MirroredAppendOnly_Append(
	MirroredAppendOnlyOpen *open,
				/* The open struct. */

	void		*appendData,
				/* Pointer to the Append-Only data. */

	int32		appendDataLen);
				/* The byte length of the Append-Only data. */

#endif   /* CDBMIRROREDAPPENDONLY_H */


