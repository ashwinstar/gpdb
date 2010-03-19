/*-------------------------------------------------------------------------
 *
 * cdbmirroredappendonly.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "utils/palloc.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbmirroredappendonly.h"
#include "storage/fd.h"
#include "catalog/catalog.h"
#include "cdb/cdbpersistenttablespace.h"
#include "storage/smgr.h"

/*
 * Open a relation for mirrored write.
 */
void MirroredAppendOnly_Open(
	MirroredAppendOnlyOpen *open,
			/* The resulting open struct. */
							 
	RelFileNode		*relFileNode,
			/* The tablespace, database, and relation OIDs for the open. */
	 
	int32			segmentFileNum,
	
	char			*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
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
	
	MemSet(open, 0, sizeof(MirroredAppendOnlyOpen));
	
	open->relFileNode = *relFileNode;
	
	open->segmentFileNum = segmentFileNum;	
	
	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
										relFileNode->spcNode,
										&primaryFilespaceLocation,
										&mirrorFilespaceLocation);
	
	if (mirrorFilespaceLocation == NULL)
		sprintf(open->mirrorFilespaceLocation, "%s", "");
	else
		sprintf(open->mirrorFilespaceLocation, "%s", mirrorFilespaceLocation);
	
	if (FileRepPrimary_MirrorOpen(
				  FileRep_GetRelationIdentifier(
												open->mirrorFilespaceLocation,
												open->relFileNode, 
												open->segmentFileNum),
				  FileRepRelationTypeAppendOnly,
				  logicalEof,
				  fileFlags,
				  fileMode,
				  TRUE /* supressError */) != 0) 
		ereport(LOG,
				(errmsg("ERROR(filerep): could not sent file open request to mirror "),
						FileRep_ReportRelationPath(
												   open->mirrorFilespaceLocation,
												   open->relFileNode,
												   open->segmentFileNum)));
	
	FormDatabasePath(
				dbPath,
				primaryFilespaceLocation,
				relFileNode->spcNode,
				relFileNode->dbNode);
	
	open->isResync = FileRepPrimary_IsResync();

	if (segmentFileNum == 0)
		sprintf(path, "%s/%u", dbPath, relFileNode->relNode);
	else
		sprintf(path, "%s/%u.%u", dbPath, relFileNode->relNode, segmentFileNum);
	
	open->primaryFile = PathNameOpenFile(path, fileFlags, fileMode);
			
	if (open->primaryFile > -1)
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
	Assert(open->isResync == FALSE);
	
	if (FileRepPrimary_MirrorFlushAndClose(
				   FileRep_GetRelationIdentifier(
												 open->mirrorFilespaceLocation,
												 open->relFileNode, 
												 open->segmentFileNum),
				   FileRepRelationTypeAppendOnly) != 0) {
		mirrorFlush = FALSE;
		ereport(LOG,
				(errmsg("ERROR(filerep): could not sent file fsync and close request to mirror "), 
						FileRep_ReportRelationPath(
												   open->mirrorFilespaceLocation,
												   open->relFileNode,
												   open->segmentFileNum)));		
	}
	
	if (open->isResync == FALSE)
	{
		errno = 0;
		
		if (FileSync(open->primaryFile) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\", segment file %d: %m", 
							relpath(open->relFileNode), open->segmentFileNum)));

		FileClose(open->primaryFile);
	}
	
	if (mirrorFlush) {
		if (FileRepPrimary_IsOperationCompleted(
				   FileRep_GetRelationIdentifier(
												 open->mirrorFilespaceLocation,
												 open->relFileNode, 
												 open->segmentFileNum),									
				   FileRepRelationTypeAppendOnly) == FALSE)	

			ereport(LOG,
					(errmsg("ERROR(filerep): could not fsync file on mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));		
	}


	open->isActive = false;
	open->primaryFile = 0;
}

/*
 * Flush and close a bulk relation file.
 *
 * If the flush is unable to complete on the mirror, then this relation will be marked in the
 * commit, distributed commit, distributed prepared and commit prepared records as having
 * un-mirrored bulk initial data.
 */
void MirroredAppendOnly_Flush(
									  MirroredAppendOnlyOpen *open)
/* The open struct. */				

{
	bool mirrorFlush = TRUE;
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorFlush(
								   FileRep_GetRelationIdentifier(
																 open->mirrorFilespaceLocation,
																 open->relFileNode, 
																 open->segmentFileNum),
								   FileRepRelationTypeAppendOnly) != 0) 
	{
		mirrorFlush = FALSE;
		ereport(LOG,
				(errmsg("ERROR(filerep): could not sent file fsync and close request to mirror "), 
						FileRep_ReportRelationPath(
												   open->mirrorFilespaceLocation,
												   open->relFileNode,
												   open->segmentFileNum)));				
	}
	
	if (open->isResync == FALSE)
	{
		errno = 0;
		
		if (FileSync(open->primaryFile) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\", segment file %d: %m", 
							relpath(open->relFileNode), open->segmentFileNum)));
	}
	
	if (mirrorFlush) 
	{
		if (FileRepPrimary_IsOperationCompleted(
												FileRep_GetRelationIdentifier(
																			  open->mirrorFilespaceLocation,
																			  open->relFileNode, 
																			  open->segmentFileNum),									
												FileRepRelationTypeAppendOnly) == FALSE)	
			
			ereport(LOG,
					(errmsg("ERROR(filerep): could not fsync file on mirror "), 
							FileRep_ReportRelationPath(
													   open->mirrorFilespaceLocation,
													   open->relFileNode,
													   open->segmentFileNum)));				
	}
	
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
														open->mirrorFilespaceLocation,
														open->relFileNode, 
														open->segmentFileNum),
						   FileRepRelationTypeAppendOnly) != 0) {

		ereport(LOG,
				(errmsg("ERROR(filerep): could not sent file close request to mirror "), 
						FileRep_ReportRelationPath(
												   open->mirrorFilespaceLocation,
												   open->relFileNode,
												   open->segmentFileNum)));						
	}
	
	errno = 0;
	
	FileClose(open->primaryFile);
	
	open->isActive = false;
	open->primaryFile = 0;
}

void MirroredAppendOnly_Drop(
	RelFileNode					*relFileNode,
	 
	int32						segmentFileNum,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	StorageManagerMirrorMode	mirrorMode,
	
	bool						ignoreNonExistence)
{
	char *primaryFilespaceLocation;
	char *mirrorFilespaceLocation;
	char dbPath[MAXPGPATH];	
	char path[MAXPGPATH];
	bool mirrorDrop = TRUE;	
	
	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
													   relFileNode->spcNode,
													   &primaryFilespaceLocation,
													   &mirrorFilespaceLocation);	
	
	if (FileRepPrimary_MirrorDrop(
								  FileRep_GetRelationIdentifier(
																mirrorFilespaceLocation,
																*relFileNode, 
																segmentFileNum),				  
								  FileRepRelationTypeAppendOnly) != 0) {
		
		mirrorDrop = FALSE;
		ereport(LOG,
				(errmsg("ERROR(filerep): could not sent file drop request to mirror "), 
				 FileRep_ReportRelationPath(
											mirrorFilespaceLocation,
											*relFileNode,
											segmentFileNum)));
	}
	
	if (FileRepPrimary_IsResync() == FALSE)
	{
		FormDatabasePath(
						 dbPath,
						 primaryFilespaceLocation,
						 relFileNode->spcNode,
						 relFileNode->dbNode);
		
		if (segmentFileNum == 0)
			sprintf(path, "%s/%u", dbPath, relFileNode->relNode);
		else
			sprintf(path, "%s/%u.%u", dbPath, relFileNode->relNode, segmentFileNum);
		
		errno = 0;
		
		if (unlink(path) < 0) 
		{
			// UNDONE: For now, cheat and call the MirroredBufferPool module...
			if (!ignoreNonExistence || errno != ENOENT)
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove relation %u/%u/%u (segment file #%d): %m",
								relFileNode->spcNode,
								relFileNode->dbNode,
								relFileNode->relNode,
								segmentFileNum)));
			}
		}
	}
	
	if (mirrorDrop) {
		if (FileRepPrimary_IsOperationCompleted(
												FileRep_GetRelationIdentifier(
																			  mirrorFilespaceLocation,
																			  *relFileNode, 
																			  segmentFileNum),									
												FileRepRelationTypeAppendOnly) == FALSE)	
			
			ereport(LOG,
					(errmsg("ERROR(filerep): could not drop file on mirror "), 
					 FileRep_ReportRelationPath(
												mirrorFilespaceLocation,
												*relFileNode,
												segmentFileNum)));		
	}
	
	if (primaryFilespaceLocation != NULL)
		pfree(primaryFilespaceLocation);
	
	if (mirrorFilespaceLocation != NULL)
		pfree(mirrorFilespaceLocation);	
	
	return;
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
												 open->mirrorFilespaceLocation,
												 open->relFileNode, 
												 open->segmentFileNum),
				   FileRepRelationTypeAppendOnly,
				   FILEREP_OFFSET_UNDEFINED,
				   buffer, 
				   bufferLen, 
				   FileRep_GetXLogRecPtrUndefined())	!= 0) 					  
		ereport(LOG,
				(errmsg("ERROR(filerep): could not sent write request to mirror "), 
						FileRep_ReportRelationPath(
												   open->mirrorFilespaceLocation,
												   open->relFileNode,
												   open->segmentFileNum)));						
	
	if (open->isResync == FALSE)
	{
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
}

// -----------------------------------------------------------------------------
// Truncate
// ----------------------------------------------------------------------------
bool MirroredAppendOnly_Truncate(
								 MirroredAppendOnlyOpen *open,
								 /* The open struct. */
								 
								 int32		position)
									/* The position to cutoff the data. */
{
	
	if (FileRepPrimary_MirrorTruncate(
									  FileRep_GetRelationIdentifier(
																	open->mirrorFilespaceLocation,
																	open->relFileNode, 
																	open->segmentFileNum),
									  FileRepRelationTypeAppendOnly,
									  position) != 0) 
		
		ereport(LOG,
				(errmsg("ERROR(filerep): could not sent file truncate request to mirror "), 
						FileRep_ReportRelationPath(
												   open->mirrorFilespaceLocation,
												   open->relFileNode,
												   open->segmentFileNum)));						
				
	if (open->isResync == FALSE)
	{
		errno = 0;
		
		if (FileTruncate(open->primaryFile, position) < 0)
			return false;
	}
	return true;
}

/*
 * Read an append only file in sequential way.
 * The routine is used for Resync.
 */
int MirroredAppendOnly_Read(
							MirroredAppendOnlyOpen *open,
								/* The open struct. */
							
							void		*buffer,
								/* Pointer to the buffer. */
							
							int32		bufferLen)
								/* Byte length of buffer. */

{	
	Assert(open != NULL);
	Assert(buffer != NULL);
	Assert(open->isActive);
	
	errno = 0;
	return FileRead(open->primaryFile, buffer, bufferLen);
}
