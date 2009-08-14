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
#include "storage/gp_compress.h"

#include "cdb/cdbvarblock.h"
#include "cdb/cdbbufferedappend.h"
#include "cdb/cdbbufferedread.h"
#include "cdb/cdbvarblock.h"

#include "cdb/cdbappendonlystoragelayer.h"
#include "cdb/cdbappendonlystorageread.h"
#include "cdb/cdbappendonlystoragewrite.h"

#define DEFAULT_COMPRESS_LEVEL				 0
#define MIN_APPENDONLY_BLOCK_SIZE			 8 * 1024
#define DEFAULT_APPENDONLY_BLOCK_SIZE		32 * 1024
#define MAX_APPENDONLY_BLOCK_SIZE			 2 * 1024 * 1024
#define DEFAULT_VARBLOCK_TEMPSPACE_LEN   	 4 * 1024 
#define DEFAULT_FS_SAFE_WRITE_SIZE			 0

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
	File			appendFile;
	int				appendFilePathNameMaxLen;
	char			*appendFilePathName;
	float8			insertCount;
	float8			varblockCount;
	BlockNumber		cur_segno;
	VarBlockMaker	varBlockMaker;
	int64			bufferCount;
	int64			blockFirstRowNum;
	bool			shouldCompress;
	bool			usingChecksum;
	bool			useNoToast;
	int32			fixedHeaderLen;
	uint8			*tempSpace;
	uint8			*uncompressedBuffer; /* used for compression */

	int32			usableBlockSize;
	int32			maxDataLen;
	int32			tempSpaceLen;
	
	/*
	 * These serve the equivalent purpose of the uppercase constants of the same
	 * name in tuptoaster.h but here we make these values dynamic.
	 */	
	int32			toast_tuple_threshold;
	int32			toast_tuple_target;
	AppendOnlyStorageAttributes storageAttributes;
	AppendOnlyStorageWrite		storageWrite;

	uint8			*nonCompressedData;

} AppendOnlyInsertDescData;

typedef AppendOnlyInsertDescData *AppendOnlyInsertDesc;

typedef struct AppendOnlyExecutorReadBlock
{
	MemoryContext	memoryContext;

	int64			headerOffsetInFile;
	uint8			*dataBuffer;
	int32			dataLen;
	int 			executorBlockKind;
	int64			blockFirstRowNum;
	int 			rowCount;
	bool			isLarge;
	bool			isCompressed;

	uint8			*uncompressedBuffer; /* for decompression */

	uint8			*largeContentBuffer;
	int32			largeContentBufferLen;

	VarBlockReader  varBlockReader;
	int				readerItemCount;
	
	uint8			*singleRow;
	int32			singleRowLen;

} AppendOnlyExecutorReadBlock;

/*
 * used for scan of append only relations using BufferedRead and VarBlocks
 */
typedef struct AppendOnlyScanDescData
{
	/* scan parameters */
	Relation	aos_rd;				/* target relation descriptor */
	Index       aos_scanrelid;
	int			aos_nkeys;			/* number of scan keys */
	ScanKey		aos_key;			/* array of scan key descriptors */
	bool		aos_notoast;		/* using toast for this relation? */
	
	/* file segment scan state */
	int			aos_filenamepath_maxlen;
	char		*aos_filenamepath;
									/* the current segment file pathname. */
	int			aos_total_segfiles;	/* the relation file segment number */
	int			aos_segfiles_processed; /* num of segfiles already processed */
	FileSegInfo *aos_segfile_arr;	/* array of all segfiles information */
	bool		aos_need_new_segfile;
	bool		aos_done_all_segfiles;
	
	MemoryContext	aoScanInitContext; /* mem context at init time */
	char			*qlzScratch;
	int32			qlzScratchLen;
	int32			qlzScratchGuardLen;

	int32			usableBlockSize;
	int32			maxDataLen;
	int32			largeReadLen;

	int64           bufferCount;
	int64			blockFirstRowNum;
	int64			rowNumber;
	int64			rowNumberInSegmentFile;

	AppendOnlyExecutorReadBlock	executorReadBlock;

	int				currentItemCount;
	
	/* current scan state */
	bool		bufferDone;
	TupleDesc	aos_tupDesc;
	MemTupleData aos_ctup;		/* current tuple in scan, if any */
	
	/* synthetic system attributes */
	ItemPointerData cdb_fake_ctid;
	MemTupleBinding *mt_bind;

	PgStat_Info aos_pgstat_info; /* statistics collector hook */

	bool	aos_initedNewReadStorageRoutines;

	AppendOnlyStorageAttributes	aos_storageAttributes;
	AppendOnlyStorageRead		aos_storageRead;

}	AppendOnlyScanDescData;

typedef AppendOnlyScanDescData *AppendOnlyScanDesc;

/* ----------------
 *		function prototypes for appendonly access method
 * ----------------
 */

extern AppendOnlyScanDesc appendonly_beginscan(Relation relation,
											   Snapshot snapshot,
											   int nkeys, 
											   ScanKey key);
extern void appendonly_rescan(AppendOnlyScanDesc scan, ScanKey key);
extern void appendonly_endscan(AppendOnlyScanDesc scan);
extern MemTuple appendonly_getnext(AppendOnlyScanDesc scan, 
									ScanDirection direction,
									TupleTableSlot *slot);
extern AppendOnlyInsertDesc appendonly_insert_init(Relation rel, int segno);
extern Oid appendonly_insert(AppendOnlyInsertDesc aoInsertDesc, MemTuple tup);
extern void appendonly_insert_finish(AppendOnlyInsertDesc aoInsertDesc);
extern BlockNumber RelationGuessNumberOfBlocks(double totalbytes);

#endif   /* APPENDONLYAM_H */
