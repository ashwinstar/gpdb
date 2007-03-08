/*-------------------------------------------------------------------------
*
* fileam.h
*	  external file access method definitions.
*
* Copyright (c) 2007, greenplum inc
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

extern FileScanDesc file_beginscan(Relation relation, List *uriList, List *fmtOpts,
			   bool isText, int nkeys, ScanKey key);
extern void file_rescan(FileScanDesc scan, ScanKey key);
extern void file_endscan(FileScanDesc scan);
extern HeapTuple file_getnext(FileScanDesc scan, ScanDirection direction);
extern void close_external_file(FILE *dataSource, char *dataSourceName);
extern void AtAbort_ExtTables(void);

#endif   /* FILEAM_H */
