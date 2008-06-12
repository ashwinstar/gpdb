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

/*
 * Open an Append Only relation file segment
 *
 * Note: because we use BasicOpenFile(), we are responsible for ensuring
 * the FD gets closed in any error exit path.  
 */
File
OpenAOSegmentFile(Relation rel, BlockNumber segno)
{
	File		aosegfile;
	char		*path,
				*fullpath;

	/* Get base path for this relation file */
	path = relpath(rel->rd_node);

	/* 
	 * Get the segfile extention. this code is borrowed from md.c 
	 */
	if (segno > 0)
	{
		/* be sure we have enough space for the '.segno' */
		fullpath = (char *) palloc(strlen(path) + 12);
		sprintf(fullpath, "%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;
	
	
	aosegfile = BasicOpenFile(path, O_RDWR | PG_BINARY, 0600);
	
	if(aosegfile < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open append only file \"%s\"", fullpath)));
	
	return aosegfile;
}

