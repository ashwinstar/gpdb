/*-------------------------------------------------------------------------
 *
 * cdbmirroredbulkrelation.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>

#include "utils/palloc.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbmirroredbulkrelation.h"
#include "storage/fd.h"
#include "cdb/cdbpersistenttablespace.h"
#include "catalog/catalog.h"

/*
 * Open a relation for mirrored write.
 */
void MirroredBulkRelation_Open(
	MirroredBulkRelationOpen *open,
				/* The resulting open struct. */

	int 			fileFlags, 

	int 			fileMode,

	RelFileNode		*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	int32			segmentFileNum)
{
	char *primaryFilespaceLocation;
	char *mirrorFilespaceLocation;
	char dbPath[MAXPGPATH];
	char path[MAXPGPATH];

	Assert(open != NULL);
	
	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
										relFileNode->spcNode,
										&primaryFilespaceLocation,
										&mirrorFilespaceLocation);
	
	FormDatabasePath(
				dbPath,
				primaryFilespaceLocation,
				relFileNode->spcNode,
				relFileNode->dbNode);

	if (segmentFileNum == 0)
		sprintf(path, "%s/%u", dbPath, relFileNode->relNode);
	else
		sprintf(path, "%s/%u.%u", dbPath, relFileNode->relNode, segmentFileNum);

	if (FileRepPrimary_MirrorOpen(
				  FileRep_GetRelationIdentifier(
							*relFileNode, 
							segmentFileNum),
				  FileRepRelationTypeBulk,
				  FILEREP_OFFSET_UNDEFINED,
				  fileFlags,
				  fileMode,
				  FALSE /* supressError */) != 0) 
		ereport(LOG,
				(errmsg("could not sent open file to mirror \"%s\"", 
						path)));

	MemSet(open, 0, sizeof(MirroredBulkRelationOpen));


	errno = 0;
	
	open->primaryFile = PathNameOpenFile(path, fileFlags, fileMode);
	if (open->primaryFile < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	strcpy(open->path, path);

	open->relFileNode = *relFileNode;
	open->segmentFileNum = segmentFileNum;

	open->isActive = true;

	if (primaryFilespaceLocation != NULL)
		pfree(primaryFilespaceLocation);
	if (mirrorFilespaceLocation != NULL)
		pfree(mirrorFilespaceLocation);

}

/*
 * Flush and close a bulk relation file.
 *
 * If the flush is unable to complete on the mirror, then this relation will be marked in the
 * commit, distributed commit, distributed prepared and commit prepared records as having
 * un-mirrored bulk initial data.
 */
void MirroredBulkRelation_FlushAndClose(
	MirroredBulkRelationOpen *open)
				/* The open struct. */				

{
	bool mirrorFlush = TRUE;
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorFlushAndClose(
				   FileRep_GetRelationIdentifier(
								 open->relFileNode, 
								 open->segmentFileNum),
				   FileRepRelationTypeBulk) != 0) {
		mirrorFlush = FALSE;
		ereport(LOG,
				(errmsg("could not sent fsync and close file to mirror \"%s\"", 
						open->path)));
	
	}
	
	errno = 0;
	
	if (FileSync(open->primaryFile) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", open->path)));

	FileClose(open->primaryFile);
	
	if (mirrorFlush) {
		if (FileRepPrimary_IsOperationCompleted(
				   FileRep_GetRelationIdentifier(
							 open->relFileNode, 
							 open->segmentFileNum),									
				   FileRepRelationTypeBulk) == FALSE)	

			ereport(LOG,
					(errmsg("could not fsync file on mirror \"%s\"", 
							open->path)));					 
	}


	open->isActive = false;
}

// -----------------------------------------------------------------------------
// Append 
// -----------------------------------------------------------------------------
				
/*
 * Write bulk mirrored.
 */
void MirroredBulkRelation_Append(
	MirroredBulkRelationOpen *open,
				/* The open struct. */

	void		*buffer,
				/* Pointer to the buffer. */

	int32		bufferLen)
				/* Byte length of buffer. */
{
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorWrite(
				   FileRep_GetRelationIdentifier(
								 open->relFileNode, 
								 open->segmentFileNum),
				   FileRepRelationTypeBulk,
				   FILEREP_OFFSET_UNDEFINED,
				   buffer, 
				   bufferLen, 
				   FileRep_GetXLogRecPtrUndefined())	!= 0) 					  
		ereport(LOG,
				(errmsg("could not sent write to mirror \"%s\"", 
						open->path)));

	errno = 0;
	if ((int) FileWrite(open->primaryFile, buffer, bufferLen) != bufferLen)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", open->path)));
	}
}
