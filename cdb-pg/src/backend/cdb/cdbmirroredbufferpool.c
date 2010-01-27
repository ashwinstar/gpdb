/*-------------------------------------------------------------------------
 *
 * cdbmirroredbufferpool.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <signal.h>

#include "utils/palloc.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbmirroredbufferpool.h"
#include "storage/fd.h"
#include "cdb/cdbpersistenttablespace.h"

/*
 * Open a relation for mirrored write.
 */
bool MirroredBufferPool_Open(
	MirroredBufferPoolOpen *open,
				/* The resulting open struct. */

	RelFileNode 	*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	uint32			segmentFileNum,
				/* Which segment file. */

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
				  FileRepRelationTypeBufferPool,
				  FILEREP_OFFSET_UNDEFINED,
				  fileFlags,
				  fileMode) != 0) 
		ereport(LOG,
				(errmsg("could not sent open file to mirror file \"%s\", segment file %d", 
						relpath(open->relFileNode), open->segmentFileNum)));

	MemSet(open, 0, sizeof(MirroredBufferPoolOpen));
	open->primaryFile = -1;

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
	if (open->primaryFile < 0)
		return false;
	else
		errno = 0;

	open->relFileNode = *relFileNode;
	open->segmentFileNum = segmentFileNum;
	
	open->isActive = true;

	if (primaryFilespaceLocation != NULL)
		pfree(primaryFilespaceLocation);
	if (mirrorFilespaceLocation != NULL)
		pfree(mirrorFilespaceLocation);

	return true;
}

extern bool MirroredBufferPool_IsActive(
	MirroredBufferPoolOpen *open)
				/* The open struct. */
{
	return open->isActive;
}

/*
 * Flush a flat file.
 *
 */
bool MirroredBufferPool_Flush(
	MirroredBufferPoolOpen *open)
				/* The open struct. */	

{
	bool mirrorFlush = TRUE;
	bool retval = TRUE;
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorFlush(
					FileRep_GetRelationIdentifier(
								open->relFileNode, 
								open->segmentFileNum),
					FileRepRelationTypeBufferPool) != 0) {
		
		mirrorFlush = FALSE;
		ereport(LOG,
				(errmsg("could not sent fsync file to mirror file \"%s\", segment file %d", 
						relpath(open->relFileNode), open->segmentFileNum)));
	}
	
	errno = 0;

	if (FileSync(open->primaryFile) < 0) 
		retval = false;

	if (mirrorFlush) {
		if (FileRepPrimary_IsOperationCompleted(
						FileRep_GetRelationIdentifier(
								  open->relFileNode, 
								  open->segmentFileNum),									
						FileRepRelationTypeBufferPool) == FALSE)	
			
			ereport(LOG,
					(errmsg("could not fsync file on mirror file \"%s\", segment file %d", 
							relpath(open->relFileNode), open->segmentFileNum)));
	}
	
	return retval;
}

/*
 * Close a bulk relation file.
 *
 */
void MirroredBufferPool_Close(
	MirroredBufferPoolOpen *open)
				/* The open struct. */				

{
	Assert(open != NULL);
	Assert(open->isActive);
						
	if (FileRepPrimary_MirrorClose(
				FileRep_GetRelationIdentifier(
								 open->relFileNode, 
								 open->segmentFileNum),
				FileRepRelationTypeBufferPool) != 0) 
		
		ereport(LOG,
				(errmsg("could not sent close file to mirror file \"%s\", segment file %d", 
						relpath(open->relFileNode), open->segmentFileNum)));
    errno = 0;
	
	FileClose(open->primaryFile);
	open->primaryFile = -1;

	open->isActive = false;
}

/*
 * Write a mirrored flat file.
 */
bool MirroredBufferPool_Write(
	MirroredBufferPoolOpen *open,
				/* The open struct. */

	int32		position,
				/* The position to write the data. */

	void		*buffer,
				/* Pointer to the buffer. */

	int32		bufferLen)
				/* Byte length of buffer. */

{
	int64 seekResult;
	char*	buf = NULL;

	Assert(open != NULL);
	Assert(buffer != NULL);
	Assert(open->isActive);
	
	buf = (char*) palloc(bufferLen);
	
	/* 
	 * memcpy() is required since buffer is still changing, however filerep
	 * requires identical data to be written to primary and its mirror 
	 */
	memcpy(buf, buffer, bufferLen);
	
	if (FileRepPrimary_MirrorWrite(
					   FileRep_GetRelationIdentifier(
									 open->relFileNode, 
									 open->segmentFileNum),
					   FileRepRelationTypeBufferPool,
					   position,
					   buf, 
					   bufferLen, 
					   FileRep_GetXLogRecPtrUndefined())	!= 0) 		
		
		ereport(LOG,
				 (errmsg("could not sent write to mirror position %d in file \"%s\", segment file %d", 
						position, relpath(open->relFileNode), open->segmentFileNum)));
	
	errno = 0;
	
	seekResult = FileSeek(open->primaryFile, position, SEEK_SET);
	if (seekResult != position)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in file to position %d in file \"%s\", segment file %d: %m", 
				        position,
				        relpath(open->relFileNode), open->segmentFileNum)));
	}

	errno = 0;
	if ((int) FileWrite(open->primaryFile, buf, bufferLen) != bufferLen)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		return false;
	}
	
	if (buf) {
		pfree(buf);
		buf = NULL;
	}

	return true;
}

/*
 * Read a mirrored buffer pool page.
 */
int MirroredBufferPool_Read(
	MirroredBufferPoolOpen *open,
				/* The open struct. */

	int32		position,
				/* The position to write the data. */

	void		*buffer,
				/* Pointer to the buffer. */

	int32		bufferLen)
				/* Byte length of buffer. */

{
	int64 seekResult;

	Assert(open != NULL);
	Assert(buffer != NULL);
	Assert(open->isActive);

	errno = 0;
	seekResult = FileSeek(open->primaryFile, position, SEEK_SET);
	if (seekResult != position)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in file to position %d in file \"%s\", segment file %d: %m", 
				        position,
				        relpath(open->relFileNode), open->segmentFileNum)));
	}

	return FileRead(open->primaryFile, buffer, bufferLen);
}

int64 MirroredBufferPool_SeekSet(
	MirroredBufferPoolOpen *open,
				/* The open struct. */

	int32		position)
{
	errno = 0;
	return FileSeek(open->primaryFile, position, SEEK_SET);
}


int64 MirroredBufferPool_SeekEnd(
	MirroredBufferPoolOpen *open)
				/* The open struct. */
{
	errno = 0;
	return FileSeek(open->primaryFile, 0L, SEEK_END);
}

/*
 * Mirrored drop.
 */
bool MirroredBufferPool_Drop(
	RelFileNode 	*relFileNode,
				/* The tablespace, database, and relation OIDs for the open. */

	uint32			segmentFileNum)
				/* Which segment file. */
{
	char *subDirectory;
	char path[MAXPGPATH];
	bool mirrorDrop = TRUE;
	bool retval = TRUE;
	
	if (FileRepPrimary_MirrorDrop(
				FileRep_GetRelationIdentifier(
							*relFileNode, 
							segmentFileNum),				  
				FileRepRelationTypeBufferPool) != 0) {
		
		mirrorDrop = FALSE;
		ereport(LOG,
				 (errmsg("could not sent drop file to mirror file \"%s\", segment file %d", 
						relpath(*relFileNode), segmentFileNum)));
	}

	subDirectory = GetDatabasePath(
								relFileNode->dbNode,
								relFileNode->spcNode);
	if (segmentFileNum == 0)
		sprintf(path, "%s/%u", subDirectory, relFileNode->relNode);
	else
		sprintf(path, "%s/%u.%u", subDirectory, relFileNode->relNode, segmentFileNum);
	pfree(subDirectory);

	errno = 0;
	
	if (unlink(path) < 0) 
		retval = false;

	if (mirrorDrop) {
		if (FileRepPrimary_IsOperationCompleted(
					FileRep_GetRelationIdentifier(
									*relFileNode, 
									segmentFileNum),									
					FileRepRelationTypeBufferPool) == FALSE)	
			
			ereport(LOG,
					(errmsg("could not fsync file on mirror file \"%s\", segment file %d", 
							relpath(*relFileNode), segmentFileNum)));
	}
	
	return retval;
}

bool MirroredBufferPool_Truncate(
	MirroredBufferPoolOpen *open,
				/* The open struct. */

	int32		position)
				/* The position to cutoff the data. */
{
	
	if (FileRepPrimary_MirrorTruncate(
				FileRep_GetRelationIdentifier(
							open->relFileNode, 
							open->segmentFileNum),
				FileRepRelationTypeBufferPool,
				position) != 0) 
		
		ereport(LOG,
				(errmsg("could not sent truncate file to mirror file \"%s\", segment file %d", 
						relpath(open->relFileNode), open->segmentFileNum)));	
	
	errno = 0;
	
	if (FileTruncate(open->primaryFile, position) < 0)
		return false;

	return true;
}


