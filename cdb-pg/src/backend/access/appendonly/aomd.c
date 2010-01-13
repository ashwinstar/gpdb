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

#include "utils/guc.h"
#include "access/appendonlytid.h"
#include "cdb/cdbappendonlystorage.h"

int
AOSegmentFilePathNameLen(Relation rel)
{
	char		*basepath;
	int 		len;
		
	/* Get base path for this relation file */
	basepath = relpath(rel->rd_node);

	/*
	 * The basepath will be the RelFileNode number.  Optional part is dot "." plus 
	 * 6 digit segment file number.
	 */
	len = strlen(basepath) + 8;	// Generous.
	
	pfree(basepath);

	return len;
}

/*
 * Formats an Append Only relation file segment file name.
 *
 * The filepathname parameter assume sufficient space.
 */
void
FormatAOSegmentFileName(
							char *basepath, 
							int segno, 
							int col, 
							int32 *fileSegNo,
							char *filepathname)
{
	int	pseudoSegNo;
	
	Assert(segno >= 0);
	Assert(segno <= AOTupleId_MaxSegmentFileNum);

	if (col < 0)
	{
		/*
		 * Row oriented Append-Only.
		 */
		pseudoSegNo = segno;		
	}
	else
	{
		/*
		 * Column oriented Append-only.
		 */
		pseudoSegNo = (col*AOTupleId_MultiplierSegmentFileNum) + segno;
	}
	
	*fileSegNo = pseudoSegNo;

	if (pseudoSegNo > 0)
	{
		sprintf(filepathname, "%s.%u", basepath, pseudoSegNo);
	}
	else
		strcpy(filepathname, basepath);
}

/*
 * Make an Append Only relation file segment file name.
 *
 * The filepathname parameter assume sufficient space.
 */
void
MakeAOSegmentFileName(
							Relation rel, 
							int segno, 
							int col, 
							char *filepathname)
{
	char	*basepath;
	int32	fileSegNo;
	
	/* Get base path for this relation file */
	basepath = relpath(rel->rd_node);

	FormatAOSegmentFileName(basepath, segno, col, &fileSegNo, filepathname);

	pfree(basepath);
}

/*
 * Open an Append Only relation file segment
 *
 * The fd module's PathNameOpenFile() is used to open the file, so the
 * the File* routines can be used to read, write, close, etc, the file.
 */
void
OpenAOSegmentFile(
					Relation rel, 
					char *filepathname, 
					bool create,
					File *file)
{
	int		fileflags = O_RDWR | PG_BINARY;
	
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
CloseAOSegmentFile(File aosegfile, Relation rel  __attribute__((unused)), BlockNumber segno __attribute__((unused)))
{
	Assert(aosegfile > 0);
	Assert(segno >= 0);

	FileClose(aosegfile);
}

/*
 * Truncate all bytes from offset to end of file.
 */
void
TruncateAOSegmentFile(File aosegfile, Relation rel, int64 offset, int elevel)
{
	char *relname = RelationGetRelationName(rel);
	
	Assert(aosegfile > 0);
	Assert(offset >= 0);

	/*
	 * Call the 'fd' module with a 64-bit length since AO segment files
	 * can be multi-gigabyte to the terabytes...
	 */
	if (FileTruncate(aosegfile, offset) < 0)
		ereport(elevel,
				(errmsg("\"%s\": failed to truncate data after eof", relname)));
	
}

