/*-------------------------------------------------------------------------
 *
 * cdbmirroredflatfile.c
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
#include "cdb/cdbmirroredflatfile.h"
#include "storage/fd.h"
#include "catalog/catalog.h"
#include "utils/memutils.h"

/*
 * Open a relation for mirrored write.
 */
int MirroredFlatFile_Open(
	MirroredFlatFileOpen *open,
				/* The resulting open struct. */

	char 			*subDirectory,
	
	char			*simpleFileName,
				/* The simple file name. */

	int 			fileFlags, 

	int 			fileMode,

	bool			suppressError,

	bool			isMirrorRecovery) 
/* NOTE do we need to consider supressError on mirror */

{
	int save_errno = 0;
	
	Assert(open != NULL);
	
	MemSet(open, 0, sizeof(MirroredFlatFileOpen));

	open->isMirrorRecovery = isMirrorRecovery;
	open->usingDbDirNode = false;
	open->subDirectory = MemoryContextStrdup(TopMemoryContext, subDirectory);
	open->simpleFileName = MemoryContextStrdup(TopMemoryContext, simpleFileName);
	open->path = MemoryContextAlloc(TopMemoryContext, strlen(subDirectory) + 1 + strlen(simpleFileName) + 1);

	sprintf(open->path, "%s/%s", subDirectory, simpleFileName);
	
	if (FileRepPrimary_MirrorOpen(
				  FileRep_GetFlatFileIdentifier(
								subDirectory, 
								simpleFileName),
				  FileRepRelationTypeFlatFile,
				  FILEREP_OFFSET_UNDEFINED,
				  fileFlags,
				  fileMode,
				  suppressError) != 0) 
		ereport(LOG,
				(errmsg("could not sent open file to mirror \"%s\"", open->path)));		

	if (! open->isMirrorRecovery) {
		open->primaryFile = PathNameOpenFile(open->path, fileFlags, fileMode);
		if (open->primaryFile < 0)
		{
			if (!suppressError)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", open->path)));
			}
		}
		else
			errno = 0;
		save_errno = errno;
	}
	
	open->appendPosition = 0;

	open->isActive = true;

	return save_errno;
}

/*
 * Open a relation for mirrored write.
 */
int MirroredFlatFile_OpenInDbDir(
	MirroredFlatFileOpen *open,
				/* The resulting open struct. */

	DbDirNode 		*dbDirNode,
	
	char			*simpleFileName,
				/* The simple file name. */

	int 			fileFlags, 

	int 			fileMode,

	bool			suppressError)

{
	char *databasePath;
	int save_errno = 0;

	Assert(open != NULL);
	
	MemSet(open, 0, sizeof(MirroredFlatFileOpen));

	open->usingDbDirNode = true;
	open->dbDirNode = *dbDirNode;

	databasePath = GetDatabasePath(
								   dbDirNode->database,
								   dbDirNode->tablespace);
	open->subDirectory = MemoryContextStrdup(TopMemoryContext, databasePath);
	pfree(databasePath);

	open->simpleFileName = MemoryContextStrdup(TopMemoryContext, simpleFileName);
	open->path = MemoryContextAlloc(TopMemoryContext, strlen(open->subDirectory) + 1 + strlen(simpleFileName) + 1);
	
	sprintf(open->path, "%s/%s", open->subDirectory, simpleFileName);
	
	if (FileRepPrimary_MirrorOpen(
					  FileRep_GetFlatFileIdentifier(
							open->subDirectory, 
							open->simpleFileName),
					  FileRepRelationTypeFlatFile,
					  FILEREP_OFFSET_UNDEFINED,
					  fileFlags,
					  fileMode,
					  suppressError) != 0) 
		ereport(LOG,
				(errmsg("could not sent open file to mirror \"%s\"", open->path)));		

	open->primaryFile = PathNameOpenFile(open->path, fileFlags, fileMode);
	if (open->primaryFile < 0)
	{
		if (!suppressError)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", open->path)));
		}
	}
	else
		errno = 0;
	
	save_errno = errno;
	
	open->appendPosition = 0;

	open->isActive = true;

	return save_errno;
}


extern bool MirroredFlatFile_IsActive(
	MirroredFlatFileOpen *open)
				/* The open struct. */
{
	return open->isActive;
}

/*
 * Flush a flat file.
 *
 */
int MirroredFlatFile_Flush(
	MirroredFlatFileOpen *open,
				/* The open struct. */	

	bool				suppressError)

{
	bool	mirrorFlush = TRUE;
	int		save_errno = 0;
	
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorFlush(
			   FileRep_GetFlatFileIdentifier(
							 open->subDirectory, 
							 open->simpleFileName),
			   FileRepRelationTypeFlatFile) != 0) {
		
		mirrorFlush = FALSE;
		ereport(LOG,
				(errmsg("could not sent fsync file to mirror \"%s\"", open->path)));
	}

	if (! open->isMirrorRecovery) {
		errno = 0;
		if (FileSync(open->primaryFile) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", open->path)));

		save_errno = errno;
	}

	if (mirrorFlush) {
		if (FileRepPrimary_IsOperationCompleted(
					   FileRep_GetFlatFileIdentifier(
								 open->subDirectory, 
								 open->simpleFileName),									
					   FileRepRelationTypeFlatFile) == FALSE)	
			ereport(LOG,
				    (errmsg("could not fsync file on mirror \"%s\"", open->path)));	
	}
		
	return save_errno;
}

/*
 * Close a bulk relation file.
 *
 */
void MirroredFlatFile_Close(
	MirroredFlatFileOpen *open)
				/* The open struct. */				

{
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorClose(
				   FileRep_GetFlatFileIdentifier(
							 open->subDirectory, 
							 open->simpleFileName),
				   FileRepRelationTypeFlatFile) != 0) 
		ereport(LOG,
				(errmsg("could not sent close file to mirror \"%s\"", open->path)));
	
	if (! open->isMirrorRecovery) {
		errno = 0;
		FileClose(open->primaryFile);
	}
	
	Assert(open->subDirectory != NULL);
	pfree(open->subDirectory);
	open->subDirectory = NULL;
	Assert(open->simpleFileName != NULL);
	pfree(open->simpleFileName);
	open->simpleFileName = NULL;
	Assert(open->path != NULL);
	pfree(open->path);
	open->path = NULL;

	open->isActive = false;
}

/*
 * Rename a flat file.
 *
 */
int MirroredFlatFile_Rename(
							char 			*subDirectory,
							
							char			*oldSimpleFileName,
								/* The simple file name. */
							
							char			*newSimpleFileName,
								/* The simple file name. */
						
							bool			suppressError,
							
							bool			isMirrorRecovery) 
					/* if TRUE then request is not performed on primary */
{
	char oldPath[MAXPGPATH];
	char newPath[MAXPGPATH];
	bool mirrorRename = TRUE;
	int	 save_errno = 0;
	
	sprintf(oldPath, "%s/%s", subDirectory, oldSimpleFileName);
	sprintf(newPath, "%s/%s", subDirectory, newSimpleFileName);
	
	if (FileRepPrimary_MirrorRename(
								   FileRep_GetFlatFileIdentifier(
												subDirectory, 
												oldSimpleFileName),
									FileRep_GetFlatFileIdentifier(
											  subDirectory, 
											  newSimpleFileName),
									FileRepRelationTypeFlatFile) != 0) {
		
		mirrorRename = FALSE;
		ereport(LOG,
				(errmsg("could not sent rename file to mirror from \"%s\" to \"%s\"", 
						oldPath, newPath)));
	}

	if (! isMirrorRecovery) {
#if HAVE_WORKING_LINK
		if (link(oldPath, newPath) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not link file from \"%s\" to \"%s\": %m", 
							oldPath, newPath)));
		else
			errno = 0;
#else
		if (rename(oldPath, newPath) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rename file from \"%s\" to \"%s\": %m", 
							oldPath, newPath)));
		else
			errno = 0;
#endif
		save_errno = errno;
	}
	
	if (mirrorRename) {
		if (FileRepPrimary_IsOperationCompleted(
							FileRep_GetFlatFileIdentifier(
													subDirectory, 
													oldSimpleFileName),									
							FileRepRelationTypeFlatFile) == FALSE)	
			ereport(LOG,
					(errmsg("could not rename file on mirror from \"%s\" to \"%s\" ", 
							oldPath, newPath)));	
	}
	
	return save_errno;
}


// -----------------------------------------------------------------------------
// Append 
// -----------------------------------------------------------------------------

/*
 * Write a mirrored flat file.
 */
int MirroredFlatFile_Append(
	MirroredFlatFileOpen *open,
				/* The open struct. */

	void		*buffer,
				/* Pointer to the buffer. */

	int32		bufferLen,
				/* Byte length of buffer. */

	bool		suppressError)

{
	int save_errno = 0;
	
	Assert(open != NULL);
	Assert(open->isActive);
	
	if (FileRepPrimary_MirrorWrite(
				   FileRep_GetFlatFileIdentifier(
								 open->subDirectory, 
								 open->simpleFileName),
				   FileRepRelationTypeFlatFile,
				   FILEREP_OFFSET_UNDEFINED,
				   buffer, 
				   bufferLen, 
				   FileRep_GetXLogRecPtrUndefined())	!= 0) 					  
		ereport(LOG,
				(errmsg("could not sent write to mirror \"%s\" ", open->path)));

	if (! open->isMirrorRecovery) {
		errno = 0;
		if ((int) FileWrite(open->primaryFile, buffer, bufferLen) != bufferLen)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;

			if (!suppressError)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to file \"%s\": %m", open->path)));
		}
		save_errno = errno;
	}
	
	open->appendPosition += bufferLen;

	return save_errno;
}

int32 MirroredFlatFile_GetAppendPosition(
	MirroredFlatFileOpen *open)
				/* The open struct. */
{
	return open->appendPosition;
}

/*
 * Write a mirrored flat file.
 */
int MirroredFlatFile_Write(
	MirroredFlatFileOpen *open,
				/* The open struct. */

	int32		position,
				/* The position to write the data. */

	void		*buffer,
				/* Pointer to the buffer. */

	int32		bufferLen,
				/* Byte length of buffer. */

	bool		suppressError)

{
	int64	seekResult;
	char*	buf = NULL;
	int		save_errno = 0;
	
	Assert(open != NULL);
	Assert(open->isActive);
	
	buf = (char*) palloc(bufferLen);
	
	/* 
	 * memcpy() is required since buffer is still changing, however filerep
	 * requires identical data are written to primary and its mirror 
	 */	
	memcpy(buf, buffer, bufferLen);
	
	if (FileRepPrimary_MirrorWrite(
				   FileRep_GetFlatFileIdentifier(
								 open->subDirectory, 
								 open->simpleFileName),
				   FileRepRelationTypeFlatFile,
				   position,
				   buf, 
				   bufferLen, 
				   FileRep_GetXLogRecPtrUndefined())	!= 0) 					  
		ereport(LOG,
				(errmsg("could not sent seek and write to mirror \"%s\"", open->path)));

	if (! open->isMirrorRecovery) {
		errno = 0;
		seekResult = FileSeek(open->primaryFile, position, SEEK_SET);
		if (seekResult != position)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in file to position %d in file \"%s\": %m", 
							position,
							open->path)));
		}

		if ((int) FileWrite(open->primaryFile, buf, bufferLen) != bufferLen)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			if (!suppressError)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to file \"%s\": %m", open->path)));
		}
		save_errno = errno;
	}
	
	if (buf) {
		pfree(buf);
		buf = NULL;
	}
	
	return save_errno;
}

/*
 * Open a relation on primary
 */
int MirroredFlatFile_OpenPrimary(
						  MirroredFlatFileOpen *open,
						  /* The resulting open struct. */
						  
						  char 			*subDirectory,
						  
						  char			*simpleFileName,
						  /* The simple file name. */
						  
						  int 			fileFlags, 
						  
						  int 			fileMode,
								 
						  bool			suppressError) 
/* NOTE do we need to consider supressError on mirror */

{
	Assert(open != NULL);
	
	MemSet(open, 0, sizeof(MirroredFlatFileOpen));
	
	open->usingDbDirNode = false;
	open->subDirectory = MemoryContextStrdup(TopMemoryContext, subDirectory);
	open->simpleFileName = MemoryContextStrdup(TopMemoryContext, simpleFileName);
	open->path = MemoryContextAlloc(TopMemoryContext, strlen(subDirectory) + 1 + strlen(simpleFileName) + 1);
	
	sprintf(open->path, "%s/%s", subDirectory, simpleFileName);
		
	open->primaryFile = PathNameOpenFile(open->path, fileFlags, fileMode);
	if (open->primaryFile < 0)
	{
		if (!suppressError)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", open->path)));
		}
	}
	else
		errno = 0;

	
	open->appendPosition = 0;
	
	open->isActive = true;
	
	return errno;
}

/*
 * Close a bulk relation file.
 *
 */
void MirroredFlatFile_ClosePrimary(
							MirroredFlatFileOpen *open)
/* The open struct. */				

{
	Assert(open != NULL);
	Assert(open->isActive);
	
	errno = 0;
	
	FileClose(open->primaryFile);
	
	Assert(open->subDirectory != NULL);
	pfree(open->subDirectory);
	open->subDirectory = NULL;
	Assert(open->simpleFileName != NULL);
	pfree(open->simpleFileName);
	open->simpleFileName = NULL;
	Assert(open->path != NULL);
	pfree(open->path);
	open->path = NULL;
	
	open->isActive = false;
}


/*
 * Read a local flat file.
 */
int MirroredFlatFile_Read(
	MirroredFlatFileOpen *open,
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
				 errmsg("could not seek in file to position %d in file \"%s\": %m", 
				        position,
				        open->path)));
	}

	return FileRead(open->primaryFile, buffer, bufferLen);
}

/*
 * Seek in local flat file
 */
int64 MirroredFlatFile_SeekSet(
	MirroredFlatFileOpen *open,
				/* The open struct. */

	int32		position)
{
	errno = 0;
	return FileSeek(open->primaryFile, position, SEEK_SET);
}

/*
 * Seek to the end of local flat file
 */
int64 MirroredFlatFile_SeekEnd(
							   MirroredFlatFileOpen *open)
							   /* The open struct. */
{
	errno = 0;
	return FileSeek(open->primaryFile, 0, SEEK_END);
}


/*
 * Mirrored drop.
 */
int MirroredFlatFile_Drop(
	char 			*subDirectory,
	
	char			*simpleFileName,
		/* The simple file name. */

	bool			suppressError,

	bool			isMirrorRecovery) 
		/* if TRUE then request is not performed on primary */
{
	char path[MAXPGPATH];
	bool mirrorDrop = TRUE;
	int	 save_errno = 0;
	
	sprintf(path, "%s/%s", subDirectory, simpleFileName);
	
	if (FileRepPrimary_MirrorDrop(
				   FileRep_GetFlatFileIdentifier(
							 subDirectory, 
							 simpleFileName),
				   FileRepRelationTypeFlatFile) != 0) {
		
		mirrorDrop = FALSE;
		ereport(LOG,
				(errmsg("could not sent drop file to mirror \"%s\"", path)));
	}
	
	if (! isMirrorRecovery) {
		errno = 0;
		if (unlink(path) < 0 && !suppressError)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not unlink file \"%s\": %m", path)));
		save_errno = errno;
	}
	
	if (mirrorDrop) {
		if (FileRepPrimary_IsOperationCompleted(
					   FileRep_GetFlatFileIdentifier(
								 subDirectory, 
								 simpleFileName),									
					   FileRepRelationTypeFlatFile) == FALSE)	
			ereport(LOG,
					(errmsg("could not drop file on mirror \"%s\"", path)));	
	}
		
	return save_errno;
}


/*
 * Reconcile xlog (WAL) EOF between primary and mirror.
 *		xlog gets truncated to the same EOF on primary and mirror.
 *		Truncated area is set to zero.
 */
int MirroredFlatFile_ReconcileXLogEof(	
							  char 			*subDirectory,
							  
							  char			*simpleFileName,
							  
							  XLogRecPtr	primaryXLogEof,
							  
							  XLogRecPtr	*mirrorXLogEof)
{
	bool	mirrorReconcile = TRUE;
	int		status = STATUS_OK;
	
	if (FileRepPrimary_ReconcileXLogEof(
						FileRep_GetFlatFileIdentifier(
											  subDirectory, 
											  simpleFileName),	
						FileRepRelationTypeFlatFile,
						primaryXLogEof) != 0) {
		
		mirrorReconcile = FALSE;
		ereport(LOG,
				(errmsg("could not sent reconcile Xlog EOF request to mirror")));
		status = STATUS_ERROR;
	}
		
	if (mirrorReconcile) {
		if (FileRepPrimary_IsOperationCompleted(
								FileRep_GetFlatFileIdentifier(
													  subDirectory, 
													  simpleFileName),									
								FileRepRelationTypeFlatFile) == FALSE) {	
			ereport(LOG,
					(errmsg("could not reconcile Xlog EOF on mirror")));	
			status = STATUS_ERROR;
	
		} else {
			
			*mirrorXLogEof = FileRepPrimary_GetMirrorXLogEof();
			if (mirrorXLogEof->xlogid == 0 && mirrorXLogEof->xrecoff == 0) {
				ereport(LOG,
						(errmsg("could not get Xlog EOF from mirror")));	
				status = STATUS_ERROR;
			}
		}
	}
	
	return status;
}

/*
 *
 */
int
MirrorFlatFile(
			   char 		*subDirectory,
			   char			*simpleFileName)
{
	MirroredFlatFileOpen	mirroredOpen;
	MirroredFlatFileOpen	primaryOpen;
	char					*buffer = NULL;
	int32					endOffset = 0;
	int32					startOffset = 0;
	int32					bufferLen = 0;
	int						retval = 0;
	
	errno = 0;
	
	while (1) {
		
		retval = MirroredFlatFile_OpenPrimary(
											  &primaryOpen,
											  subDirectory,
											  simpleFileName,
											  O_RDONLY | PG_BINARY,
											  S_IRUSR | S_IWUSR,
											  /* suppressError */ false);
		if (retval != 0)
			break;
		
		/*
		 * Determine the end.
		 */
		endOffset = MirroredFlatFile_SeekEnd(&primaryOpen);
		if (endOffset < 0) {
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek to end of file \"%s\" : %m",
							primaryOpen.path)));
			break;
		}
		
		retval = MirroredFlatFile_Drop(
									   subDirectory,
									   simpleFileName,
									   /* suppressError */ false,
									   /*isMirrorRecovery */ TRUE);
		if (retval != 0)
			break;
		
		retval = MirroredFlatFile_Open(
									   &mirroredOpen,
									   subDirectory,
									   simpleFileName,
									   O_CREAT | O_TRUNC | O_RDWR | PG_BINARY,
									   S_IRUSR | S_IWUSR,
									   /* suppressError */ false,
									   /*isMirrorRecovery */ TRUE);
		
		if (retval != 0)
			break;
				
		bufferLen = (Size) Min(BLCKSZ, endOffset - startOffset);
		buffer = (char*) palloc(bufferLen);
		if (buffer == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 (errmsg("Not enough shared memory for Mirroring."))));
		
		MemSet(buffer, 0, bufferLen);
				
		while (startOffset < endOffset) {
			
			retval = MirroredFlatFile_Read(	
										   &primaryOpen,
										   startOffset,
										   buffer,
										   bufferLen);
			
			if (retval != bufferLen) {
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read from position:%d in file \"%s\" : %m",
								startOffset, primaryOpen.path)));
				
				break;
			}
			
			retval = MirroredFlatFile_Append(
											 &mirroredOpen,
											 buffer,
											 bufferLen,
											 /* suppressError */ false);
			
			if (retval != 0)
				break;
			
			startOffset += bufferLen;
			
			bufferLen = (Size) Min(BLCKSZ, endOffset - startOffset);
		} // while
		
		if (retval != 0)
			break;
		
		retval = MirroredFlatFile_Flush(
										&mirroredOpen,
										/* suppressError */ FALSE);
		if (retval != 0)
			break;
		
		break;
	} // while(1)
	
	if (buffer) {
		pfree(buffer);
		buffer = NULL;
	}
	
	if (MirroredFlatFile_IsActive(&mirroredOpen)) {
		MirroredFlatFile_Close(&mirroredOpen);
	}
	
	if (MirroredFlatFile_IsActive(&primaryOpen)) {
		MirroredFlatFile_ClosePrimary(&primaryOpen);
	}
	
	return retval;
}

