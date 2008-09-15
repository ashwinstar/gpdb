/*-------------------------------------------------------------------------
 *
 * aomd.c
 *	  This code manages append only relations that reside on magnetic disk.
 *	  It serves the same general purpose as smgr/md.c however we introduce
 *    AO specific file access functions mainly because would like to bypass 
 *	  md.c's and bgwriter's fsyncing. AO relations also use a non constant
 *	  block number to file segment mapping unlike heap relations.
 *
 *	  As of now we still let md.c create and unlink AO relations for us. This
 *	  may need to change if inconsistencies arise.
 *
 * Portions Copyright (c) 2008, Greenplum Inc.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/aomd.h"
#include "catalog/catalog.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

int
AOSegmentFilePathNameLen(Relation rel)
{
	char		*basepath;
	int 		len;
		
	/* Get base path for this relation file */
	basepath = relpath(rel->rd_node);

	len = strlen(basepath) + 8;	// Generous.
	
	pfree(basepath);

	return len;
}

/*
 * Open an Append Only relation file segment
 *
 * The fd module's PathNameOpenFile() is used to open the file, so the
 * the File* routines can be used to read, write, close, etc, the file.
 *
 * The filepathname parameter assume sufficient space.
 */
void
OpenAOSegmentFile(Relation rel, int segno, bool create,
	File *file, char *filepathname)
{
	char	*basepath;
	int		fileflags = O_RDWR | PG_BINARY;
		
	/* Get base path for this relation file */
	basepath = relpath(rel->rd_node);

	/* 
	 * Get the segfile extention. this code is borrowed from md.c 
	 */
	if (segno > 0)
	{
		/* be sure we have enough space for the '.segno' */
		sprintf(filepathname, "%s.%u", basepath, segno);
	}
	else
		strcpy(filepathname, basepath);
	pfree(basepath);
	
	*file = PathNameOpenFile(filepathname, fileflags, 0600);
	
	if(*file < 0)
	{
		if(!create)
		{
			/* we expected to find this file */
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open append only file \"%s\"", filepathname)));
		}
		else
		{
			/* file doesn't exist. create a new one. */
			fileflags |= O_CREAT;
			*file = PathNameOpenFile(filepathname, fileflags, 0600);

			if(*file < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("failed to create a new file segment: \"%s\"", filepathname)));
		}
	}
}

/*
 * Close an Append Only relation file segment
 */
void
CloseAOSegmentFile(File aosegfile, Relation rel, BlockNumber segno)
{
	Assert(aosegfile > 0);
	Assert(segno >= 0);

	FileClose(aosegfile);
}

/*
 * Truncate all bytes from offset to end of file.
 */
void
TruncateAOSegmentFile(File aosegfile, Relation rel, long offset, int elevel)
{
	char *relname = RelationGetRelationName(rel);
	
	Assert(aosegfile > 0);
	Assert(offset >= 0);

	// Must be converted to the the 'fd' module and 64-bit lengths...
	Assert(false);
	if (ftruncate(aosegfile, offset) < 0)
		ereport(elevel,
				(errmsg("\"%s\": failed to truncate data after eof", relname)));
	
}

