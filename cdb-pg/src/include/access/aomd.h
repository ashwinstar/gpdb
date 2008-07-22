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

extern File
OpenAOSegmentFile(Relation rel, BlockNumber segno, bool create);

extern void
CloseAOSegmentFile(File file, Relation rel, BlockNumber segno);

extern void
TruncateAOSegmentFile(File aosegfile, Relation rel, long offset, int elevel);

#endif   /* AOMD_H */
