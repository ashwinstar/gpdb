/*-------------------------------------------------------------------------
 *
 * aomd.h
 *	  declarations and functions for supporting aomd.c
 *
 * Portions Copyright (c) 2008, Greenplum Inc.
 *-------------------------------------------------------------------------
 */
#ifndef AOMD_H
#define AOMD_H

#include "storage/fd.h"
#include "utils/rel.h"

extern int
AOSegmentFilePathNameLen(Relation rel);

extern void
OpenAOSegmentFile(Relation rel, int segno, bool create,
	File *file, char *filepathname);

extern void
CloseAOSegmentFile(File file, Relation rel, BlockNumber segno);

extern void
TruncateAOSegmentFile(File aosegfile, Relation rel, long offset, int elevel);

#endif   /* AOMD_H */
