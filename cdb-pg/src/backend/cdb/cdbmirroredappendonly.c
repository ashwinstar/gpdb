/*-------------------------------------------------------------------------
 *
 * cdbmirroredappendonly.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>

#include "utils/palloc.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbmirroredappendonly.h"
#include "storage/fd.h"
#include "catalog/catalog.h"
#include "cdb/cdbpersistenttablespace.h"

/*
 * Open a relation for mirrored write.
 */
void MirroredAppendOnly_Open(
	MirroredAppendOnlyOpen *open,
			/* The resulting open struct. */
							 
	RelFileNode		*relFileNode,
			/* The tablespace, database, and relation OIDs for the open. */
	 
	int32			segmentFileNum,
							 
	int64			logicalEof,
				/* The file name path. */

	int 			fileFlags, 

	int 			fileMode)
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
	
	if (FileRepPrimary_MirrorOpen(
				  FileRep_GetRelationIdentifier(
							*relFileNode, 
							segmentFileNum),
				  FileRepRelationTypeAppendOnly,
				  logicalEof,
				  fileFlags,
				  fileMode) != 0) 
		ereport(LOG,
				(errmsg("could not sent open file to mirror \"%s\", segment file %d",
						relpath(*relFileNode), segmentFileNum)));

	MemSet(open, 0, sizeof(MirroredAppendOnlyOpen));

	FormDatabasePath(
				dbPath,
				primaryFilespaceLocation,
				relFileNode->spcNode,
				relFileNode->dbNode);

	if (segmentFileNum == 0)
		sprintf(path, "%s/%u", dbPath, relFileNode->relNode);
	else
		sprintf(path, "%s/%u.%u", dbPath, relFileNode->relNode, segmentFileNum);
	
	open->primaryFile = PathNameOpenFile(path, fileFlags, fileMode);
		
	open->relFileNode = *relFileNode;

	open->segmentFileNum = segmentFileNum;

	open->isActive = true;

	if (primaryFilespaceLocation != NULL)
		pfree(primaryFilespaceLocation);
	if (mirrorFilespaceLocation != NULL)
		pfree(mirrorFilespaceLocation);

}

extern bool MirroredAppendOnly_IsActive(
							MirroredAppendOnlyOpen *open)
/* The open struct. */
{
	return open->isActive;
}

/*
 * Flush and close a bulk relation file.
 *
 * If the flush is unable to complete on the mirror, then this relation will be marked in the
 * commit, distributed commit, distributed prepared and commit prepared records as having
 * un-mirrored bulk initial data.
 */
void MirroredAppendOnly_FlushAndClose(
	MirroredAppendOnlyOpen *open)
				/* The open struct. */				

{
	bool mirrorFlush = TRUE;
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorFlushAndClose(
				   FileRep_GetRelationIdentifier(
								 open->relFileNode, 
								 open->segmentFileNum),
				   FileRepRelationTypeAppendOnly) != 0) {
		mirrorFlush = FALSE;
		ereport(LOG,
				(errmsg("could not sent fsync and close file to mirror \"%s\", segment file %d", 
						relpath(open->relFileNode), open->segmentFileNum)));
	
	}
	
	errno = 0;
	
	if (FileSync(open->primaryFile) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\", segment file %d: %m", 
						relpath(open->relFileNode), open->segmentFileNum)));

	FileClose(open->primaryFile);
	
	if (mirrorFlush) {
		if (FileRepPrimary_IsOperationCompleted(
				   FileRep_GetRelationIdentifier(
							 open->relFileNode, 
							 open->segmentFileNum),									
				   FileRepRelationTypeAppendOnly) == FALSE)	

			ereport(LOG,
					(errmsg("could not fsync file on mirror \"%s\", segment file %d", 
							relpath(open->relFileNode), open->segmentFileNum)));					 
	}


	open->isActive = false;
	open->primaryFile = 0;
}

/*
 * Close a bulk relation file.
 *
 */
void MirroredAppendOnly_Close(
							MirroredAppendOnlyOpen *open)
/* The open struct. */				

{

	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorClose(
						  FileRep_GetRelationIdentifier(
											 open->relFileNode, 
											 open->segmentFileNum),
						   FileRepRelationTypeAppendOnly) != 0) {

		ereport(LOG,
				(errmsg("could not sent close file to mirror \"%s\", segment file %d", 
						relpath(open->relFileNode), open->segmentFileNum)));
		
	}
	
	errno = 0;
	
	FileClose(open->primaryFile);
	
	open->isActive = false;
	open->primaryFile = 0;
}

// -----------------------------------------------------------------------------
// Append 
// -----------------------------------------------------------------------------
				
/*
 * Write bulk mirrored.
 */
void MirroredAppendOnly_Append(
	MirroredAppendOnlyOpen *open,
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
				   FileRepRelationTypeAppendOnly,
				   FILEREP_OFFSET_UNDEFINED,
				   buffer, 
				   bufferLen, 
				   FileRep_GetXLogRecPtrUndefined())	!= 0) 					  
		ereport(LOG,
				(errmsg("could not sent write to mirror \"%s\", segment file %d", 
						relpath(open->relFileNode), open->segmentFileNum)));
	
	errno = 0;	

	if ((int) FileWrite(open->primaryFile, buffer, bufferLen) != bufferLen)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\", segment file %d: %m", 
						relpath(open->relFileNode), open->segmentFileNum)));
	}
}
