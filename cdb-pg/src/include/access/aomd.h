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
OpenAOSegmentFile(Relation rel, BlockNumber segno);


#endif   /* AOMD_H */
