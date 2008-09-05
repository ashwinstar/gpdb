/*-------------------------------------------------------------------------
*
* fileam.h
*	  external file access method definitions.
*
* Copyright (c) 2007-2008, Greenplum inc
*
*-------------------------------------------------------------------------
*/
#ifndef FILEAM_H
#define FILEAM_H

#include "access/htup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/tupmacs.h"
#include "access/xlogutils.h"
#include "nodes/primnodes.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/tqual.h"

extern FileScanDesc file_beginscan(Relation relation, Index scanrelid, 
								   uint32 scancounter, List *uriList, 
								   List *fmtOpts, bool isText, bool isMasterOnly,
								   int rejLimit, bool rejLimitInRows, 
								   Oid fmterrtbl, int encoding);
extern void file_rescan(FileScanDesc scan);
extern void file_endscan(FileScanDesc scan);
extern HeapTuple file_getnext(FileScanDesc scan, ScanDirection direction);
extern void AtAbort_ExtTables(void);
char*linenumber_atoi(char buffer[20],int64_t linenumber);

#endif   /* FILEAM_H */
