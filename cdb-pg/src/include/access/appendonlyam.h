/*-------------------------------------------------------------------------
 *
 * appendonlyam.h
 *	  append-only relation access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2007, Greenplum Inc.
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDONLYAM_H
#define APPENDONLYAM_H

#include "access/htup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/tupmacs.h"
#include "access/xlogutils.h"
#include "executor/tuptable.h"
#include "nodes/primnodes.h"
#include "storage/block.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#include "cdb/cdbvarblock.h"
#include "cdb/cdbbufferedappend.h"


#define MAX_VARBLOCK_BUFFER_LEN		32*1024
#define MAX_VARBLOCK_TEMPSPACE_LEN	 4*1024 

/*
 * AppendOnlyInsertDescData is used for inserting data into append-only
 * relations. It serves an equivalent purpose as AppendOnlyScanDescData
 * (relscan.h) only that the later is used for scanning append-only 
 * relations. 
 */
typedef struct AppendOnlyInsertDescData
{
	Relation		aoi_rel;
	MemTupleBinding *mt_bind;
	Oid				relfilenode;
	float8			insertCount;
	float8			varblockCount;
	BufferedAppend	bufferedAppend;
	VarBlockMaker	varBlockMaker;
	int32			bufferCount;
	bool			shouldCompress;
	int				compressLevel;  /* compression level 1-9 */
	uint8			tempSpace[MAX_VARBLOCK_TEMPSPACE_LEN];
	uint8			varBlockBuffer[MAX_VARBLOCK_BUFFER_LEN]; /* used for compression */

} AppendOnlyInsertDescData;

typedef AppendOnlyInsertDescData *AppendOnlyInsertDesc;

/* ----------------
 *		function prototypes for appendonly access method
 * ----------------
 */

extern AppendOnlyScanDesc appendonly_beginscan(Relation relation, 
											   int nkeys, 
											   ScanKey key);
extern void appendonly_rescan(AppendOnlyScanDesc scan, ScanKey key);
extern void appendonly_endscan(AppendOnlyScanDesc scan);
extern MemTuple appendonly_getnext(AppendOnlyScanDesc scan, 
									ScanDirection direction,
									TupleTableSlot *slot);
extern AppendOnlyInsertDesc appendonly_insert_init(Relation rel);
extern Oid appendonly_insert(AppendOnlyInsertDesc aoInsertDesc, MemTuple tup);
extern void appendonly_insert_finish(AppendOnlyInsertDesc aoInsertDesc);
extern BlockNumber RelationGuessNumberOfBlocks(double totalbytes);

#endif   /* APPENDONLYAM_H */
