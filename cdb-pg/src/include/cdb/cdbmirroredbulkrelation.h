/*-------------------------------------------------------------------------
 *
 * cdbmirroredbulkrelation.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBMIRROREDBULKRELATION_H
#define CDBMIRROREDBULKRELATION_H

#include "postgres.h"
#include "utils/palloc.h"
#include "storage/relfilenode.h"
#include "storage/dbdirnode.h"
#include "storage/fd.h"

/*
 * This module is for doing mirrored writes for relation files privately being bulk loaded by
 * a backend process.
 *
 * It is intended for bulk loading new created relation files not under the management of
 * the Buffer Pool (or Append-Only) yet.
 */

/*
 * This structure contains write open information.  Consider the fields
 * inside to be private.
 */
typedef struct MirroredBulkRelationOpen
{
	bool	isActive;

	File primaryFile;

	char path[MAXPGPATH];

	RelFileNode relFileNode;

	int32 segmentFileNum;

} MirroredBulkRelationOpen;


// -----------------------------------------------------------------------------
// CreateAndOpen and FlushAndClose 
// -----------------------------------------------------------------------------

/*
 * Open a relation for mirrored write.
 */
extern void MirroredBulkRelation_Open(
	MirroredBulkRelationOpen *open,
				/* The resulting open struct. */

	int 			fileFlags, 

	int 			fileMode,

	RelFileNode		*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	int32			segmentFileNum);



/*
 * Flush and close a bulk relation file.
 *
 * If the flush is unable to complete on the mirror, then this relation will be marked in the
 * commit, distributed commit, distributed prepared and commit prepared records as having
 * un-mirrored bulk initial data.
 */
extern void MirroredBulkRelation_FlushAndClose(
	MirroredBulkRelationOpen *open);
				/* The open struct. */				
								
// -----------------------------------------------------------------------------
// Append 
// -----------------------------------------------------------------------------
				
/*
 * Write bulk mirrored.
 */
extern void MirroredBulkRelation_Append(
	MirroredBulkRelationOpen *open,
				/* The open struct. */

	void		*buffer,
				/* Pointer to the buffer. */

	int32		bufferLen);
				/* Byte length of buffer. */

#endif   /* CDBMIRROREDBULKRELATION_H */
