/*-------------------------------------------------------------------------
 *
 * appendonlyam.c
 *	  append-only relation access method code
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2008, Greenplum Inc.
 *
 *
 * INTERFACE ROUTINES
 *		appendonly_beginscan		- begin relation scan
 *		appendonly_rescan			- restart a relation scan
 *		appendonly_endscan			- end relation scan
 *		appendonly_getnext			- retrieve next tuple in scan
 *		appendonly_insert_init		- initialize an insert operation
 *		appendonly_insert			- insert tuple into a relation
 *		appendonly_insert_finish	- finish an insert operation
 *
 * NOTES
 *	  This file contains the appendonly_ routines which implement
 *	  the access methods used for all append-only relations.
 *
 *            $Id: $
 *        $Change: $
 *      $DateTime: $
 *        $Author: $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#ifndef WIN32
#include <sys/fcntl.h>
#else
#include <io.h>
#endif 
#include <sys/file.h>
#include <unistd.h>

#include "access/appendonlystorage.h"
#include "access/aomd.h"
#include "access/heapam.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/xact.h"
#include "access/aosegfiles.h"
#include "catalog/catalog.h"
#include "catalog/pg_appendonly.h"
#include "catalog/namespace.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbappendonlyam.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "storage/freespace.h"
#include "storage/smgr.h"
#include "storage/quicklz1.h"
#include "storage/quicklz3.h"
#include "utils/guc.h"

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#define SCANNED_SEGNO  (&scan->aos_segfile_arr[ \
					(scan->aos_segfiles_processed == 0 ? 0 : scan->aos_segfiles_processed - 1)])->segno

typedef enum AoExecutorBlockKind
{
	AoExecutorBlockKind_None = 0,
	AoExecutorBlockKind_VarBlock,
	AoExecutorBlockKind_SingleRow,
	MaxAoExecutorBlockKind /* must always be last */
} AoExecutorBlockKind;

static void doCompressAppend(AppendOnlyInsertDesc 	aoInsertDesc,
							 uint8					*sourceData,
							 int32			 		sourceLen,
							 int					executorBlockKind,
							 int			 		itemCount);

static void doDecompress(AppendOnlyScanDesc scan,
							 uint8			*compressed,
							 int32			compressedLen,
							 uint8			*uncompressed,
							 int32			uncompressedLen);

static int32 getLimitedMaxBufferLen(int32 maxBufferLen);
static CompressorType CompressorStrToType(const char* comp_str, int comp_level);

/* ----------------
 *		initscan - scan code common to appendonly_beginscan and appendonly_rescan
 * ----------------
 */
static void
initscan(AppendOnlyScanDesc scan, ScanKey key)
{

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL)
		memcpy(scan->aos_key, key, scan->aos_nkeys * sizeof(ScanKeyData));

	scan->aos_file = -1;
	scan->aos_filenamepath[0] = '\0';
	scan->aos_segfiles_processed = 0;
	scan->aos_need_new_segfile = true; /* need to assign a file to be scanned */
	scan->aos_done_all_segfiles = false;
	scan->bufferDone = true;
	
	/*
	 * We may need to reference ctid and tableoid later. init them here.
	 */
	ItemPointerSet(&scan->cdb_fake_ctid, 0, 0);

	scan->rowNumber = 0;
	scan->rowNumberInSegmentFile = 0;
	
	pgstat_count_heap_scan(&scan->aos_pgstat_info);
}

/*
 * doPadOutRemainder
 *
 * Since we do not do typical recovery processing of append-only file-system 
 * pages, we pad out the last file-system byte with zeroes. The number of bytes
 * that are padded with zero's is determined by safefswritesize.
 * This function pads with 0's of length padLen or pads the whole remainder 
 * of the safefswritesize size with 0's if padLen is -1. 
 */
static void
doPadOutRemainder(AppendOnlyInsertDesc aoInsertDesc, 
				  int32	padLen)
{
	int64	nextWritePosition;
	int64   nextBoundaryPosition;
	int32	safeWrite = aoInsertDesc->safefswritesize; 
	int32	safeWriteRemainder;
	bool	doPad;
	uint8   *buffer;
	
	/* early exit if no pad needed */
	if(safeWrite == 0)
		return;
	
	nextWritePosition = BufferedAppendNextBufferPosition(&aoInsertDesc->bufferedAppend);
	nextBoundaryPosition = 
		((nextWritePosition + safeWrite - 1)/safeWrite)*safeWrite;
	safeWriteRemainder = (int32)(nextBoundaryPosition - nextWritePosition);
	
	if (safeWriteRemainder <= 0)
		doPad = false;
	else if (padLen == -1)
	{
		/*
		 * Pad to end of page.
		 */
		doPad = true;
		padLen = safeWriteRemainder;
	}
	else
		doPad = (safeWriteRemainder < padLen);
	
	if (doPad)
	{
		/*
		 * Get buffer of the remainder to pad.
		 */
		buffer = BufferedAppendGetBuffer(&aoInsertDesc->bufferedAppend,
										 safeWriteRemainder);
		
		if (buffer == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("We do not expect files to be have a maximum length")));						
		}
		
		memset(buffer, 0, safeWriteRemainder);
		BufferedAppendFinishBuffer(&aoInsertDesc->bufferedAppend, 
								   safeWriteRemainder, 
								   safeWriteRemainder);
		
		if (Debug_appendonly_print_insert)
			elog(LOG,"Append-only insert zero padded safeWriteRemainder for table '%s' (nextWritePosition = " INT64_FORMAT ", safeWriteRemainder = %d)",
			     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
			     nextWritePosition,
			     safeWriteRemainder);
		
	}
}

/*
 * doSkipPadding
 *
 * This function is called when the file system block we are scanning has
 * no more valid data but instead is padded with zero's from the position
 * we are currently in until the end of the block. The function will skip
 * to the end of block if skipLen is -1 or skip skipLen bytes otherwise.
 */
static void
doSkipPadding(AppendOnlyScanDesc scan,
			  int32 skipLen)
{
	int64   nextReadPosition;
	int64   nextBoundaryPosition;
	int32   safeWriteRemainder;
	bool	doSkip;
	uint8  *buffer;
	int32	availableLen;
	int32   safewrite = scan->safefswritesize;
	
	/* early exit if no pad used */
	if(safewrite == 0)
		return;
	
	nextReadPosition = BufferedReadNextBufferPosition(&scan->bufferedRead);
	nextBoundaryPosition = 
		((nextReadPosition + safewrite - 1)/safewrite)*safewrite;
	safeWriteRemainder = (int32)(nextBoundaryPosition - nextReadPosition);
	
	if (safeWriteRemainder <= 0)
		doSkip = false;
	else if (skipLen == -1)
	{
		/*
		 * Skip to end of page.
		 */
		doSkip = true;
		skipLen = safeWriteRemainder;
	}
	else
		doSkip = (safeWriteRemainder < skipLen);
	
	if (doSkip)
	{
		/*
		 * Read through the remainder.
		 */
		buffer = BufferedReadGetNextBuffer(&scan->bufferedRead,
										   safeWriteRemainder,
										   &availableLen);
		
		/*
		 * Since our file EOF should always be a multiple of the file-system
		 * page, we do not expect a short read here.
		 */
		if (buffer == NULL)
			availableLen = 0;
		if (buffer == NULL || safeWriteRemainder != availableLen)
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Unexpected end of file.  Expected to read %d bytes after position " INT64_FORMAT " but found %d bytes (bufferCount  " INT64_FORMAT ")\n", 
							safeWriteRemainder, 
							nextReadPosition, 
							availableLen,
							scan->bufferCount)));			
		}
		
		// UNDONE: For verification purposes, we should verify the
		// UNDONE: reminder is all zeroes.
		
		if (Debug_appendonly_print_scan)
			elog(LOG,"Append-only scan skipping zero padded remainder for table '%s' (nextReadPosition = " INT64_FORMAT ", safeWriteRemainder = %d)",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     nextReadPosition,
			     safeWriteRemainder);
	}
}

/*
 * Open the next file segment to scan and allocate all resources needed for it.
 */
static bool
SetNextFileSegForRead(AppendOnlyScanDesc scan)
{
	Relation		reln = scan->aos_rd;
	MemoryContext	oldcontext;
	int				segno;
	int64			eof;
	bool			finished_all_files = true; /* assume */
	
	Assert(scan->aos_need_new_segfile);   /* only call me when last segfile completed */
	Assert(!scan->aos_done_all_segfiles); /* don't call me if I told you to stop */
	
	/*
	 * first, allocate varblock buffer space. This used to be done in initscan
	 * but moved here so that memory will be allocated only when really needed.
	 * we allocate it in the query context, not per-tuple context we're in
	 * so that is won't go away on us later.
	 */
	oldcontext = MemoryContextSwitchTo(scan->aoScanInitContext);
	scan->uncompressedBuffer = (uint8 *) palloc(scan->maxBufferLen * sizeof(uint8));
	
	/*
	 * if quicklz compression used, prepare scratch space for decompression.
	 */
	switch(scan->aos_compresstype)
	{
		case COMPTYPE_QUICKLZ1:
			scan->qlz_scratch = (char*) palloc(QLZ1_SCRATCH_DECOMPRESS);
			memset(scan->qlz_scratch, 0, QLZ1_SCRATCH_DECOMPRESS);		
			break;
		case COMPTYPE_QUICKLZ3:
			scan->qlz_scratch = (char*) palloc(QLZ3_SCRATCH_DECOMPRESS);
			memset(scan->qlz_scratch, 0, QLZ3_SCRATCH_DECOMPRESS);		
			break;
		case COMPTYPE_ZLIB:
		case COMPTYPE_NONE:
			break; /* nothing to do */
		default:
			Insist(false);
	}
	
	MemoryContextSwitchTo(oldcontext);
	
	/*
	 * Initialize the buffered read module if not done yet. We allocate it in 
	 * the query-wide context.
	 */
	if(!scan->aos_bufread_inited)
	{
		int32	memoryLen;
		uint8	*memory;
		
		memoryLen = BufferedReadMemoryLen(scan->maxBufferLen, scan->largeReadLen);

		/* step away from per tuple context */
		oldcontext = MemoryContextSwitchTo(scan->aoScanInitContext);
		memory = (uint8*)palloc(memoryLen);

		/*
		 * Initialize the BufferedRead module with a large amount of memory.
		 */
		BufferedReadInit(&scan->bufferedRead,
						 memory,
						 memoryLen,
						 scan->maxBufferLen,
						 scan->largeReadLen);
		
		/* return to per tuple context */
		MemoryContextSwitchTo(oldcontext);
		
		scan->bufferDone = true; /* so we read a new buffer right away */
		scan->aos_bufread_inited = true;
	}
	
	/*
	 * Do we have more segment files to read or are we done?
	 */
	while(scan->aos_segfiles_processed < scan->aos_total_segfiles)
	{
		/* still have more segment files to read. get info of the next one */
		FileSegInfo *fsinfo = &scan->aos_segfile_arr[scan->aos_segfiles_processed];
		segno = fsinfo->segno;
		eof = (int64)fsinfo->eof;
		
		scan->aos_segfiles_processed++;
		
		/* 
		 * special case: we are the QD reading from an AO table in utility mode
		 * (gp_dump). We see entries in the aoseg table but no files or data
		 * actually exist. If we try to open this file we'll get an error, so
		 * we must skip to the next. For now, we can test if the file exists by 
		 * looking at the eof value - it's always 0 on the QD.
		 */
		if(eof > 0)
		{
			finished_all_files = false;
			break;
		}
	}
	
	if(finished_all_files)
	{
		/* finished reading all segment files */
		scan->aos_need_new_segfile = false;
		scan->aos_done_all_segfiles = true;

		return false;		
	}

	/* Open this file 'segment' */
	OpenAOSegmentFile(reln, segno, /* create */ false,
					  &scan->aos_file, scan->aos_filenamepath);
	Assert(strlen(scan->aos_filenamepath) + 1 <= scan->aos_filenamepath_maxlen);

	/* Set the file to be read with its eof */
	BufferedReadSetFile(
				&scan->bufferedRead,
				scan->aos_file, 
				scan->aos_filenamepath, 
				eof);
	
	/* ready to go! */
	scan->aos_need_new_segfile = false;

	scan->rowNumberInSegmentFile = 0;
	
	if (Debug_appendonly_print_scan)
		elog(LOG,"Append-only scan initialize for table '%s' segment file %u (compression = %s, maximum buffer length %d, large read length %d)",
			 NameStr(scan->aos_rd->rd_rel->relname),
			 segno,
			 (scan->aos_compressed ? "true" : "false"),
		     scan->maxBufferLen,
		     scan->largeReadLen);

	return true;
}

/*
 * Open the next file segment for write.
 */
static void
SetCurrentFileSegForWrite(AppendOnlyInsertDesc aoInsertDesc)
{
	FileSegInfo		*fsinfo;
	int64			eof,
					eof_uncompressed,
					result;

	/* Open the 'segment' file */
	OpenAOSegmentFile(aoInsertDesc->aoi_rel, 
					  aoInsertDesc->cur_segno, 
					  /* create */ true,
					  &aoInsertDesc->appendFile, 
					  aoInsertDesc->appendFilePathName);		
	Assert(strlen(aoInsertDesc->appendFilePathName) + 1 <= aoInsertDesc->appendFilePathNameMaxLen);
	
	/* get the information for the file segment we insert into */
	fsinfo = GetFileSegInfo(aoInsertDesc->aoi_rel, 
							aoInsertDesc->cur_segno,
							/* forupdate */ true);
	
	Assert(fsinfo);
	eof = (int64)fsinfo->eof;
	eof_uncompressed = (int64)fsinfo->eof_uncompressed;
	pfree(fsinfo);
	fsinfo = NULL;
			
	/* Seek to the starting point (current eof from catalog) */
	result = FileSeek(aoInsertDesc->appendFile, eof, SEEK_SET);
	if (result != eof)
	{
		FileClose(aoInsertDesc->appendFile);
		aoInsertDesc->appendFile = -1;
		aoInsertDesc->appendFilePathName[0] = '\0';
        ereport(ERROR, 
				(errcode(ERRCODE_IO_ERROR),
			     errmsg("Append-only I/O error on %s.  FileSeek offset = " INT64_FORMAT ".  Error code = %d (%s)",
        	 		    aoInsertDesc->appendFilePathName,
             		    eof,
             		    (int)result,
             			strerror((int)result))));
	}

	BufferedAppendSetFile(&aoInsertDesc->bufferedAppend, 
						  aoInsertDesc->appendFile, 
						  aoInsertDesc->appendFilePathName, 
						  eof,
						  eof_uncompressed);

	/* reset counts */
	aoInsertDesc->insertCount = 0;
	aoInsertDesc->varblockCount = 0;
}

/*
 * Finished scanning this file segment. Close it.
 */
static void
CloseScannedFileSeg(AppendOnlyScanDesc scan)
{
	/* 
	 * close the relation file segment. Note that QD has no file to close and some
	 * slices don't either, so check first. 
	 */
	if(scan->aos_file > 0)
	{
		if (Debug_appendonly_print_scan)
			elog(LOG,
				 "Append-only scan closing segment file #%d in table '%s' "
				 "(records scanned in segment file = " INT64_FORMAT ", block count " INT64_FORMAT ")",
			     SCANNED_SEGNO,
			     NameStr(scan->aos_rd->rd_rel->relname),
			     scan->rowNumberInSegmentFile,
			     scan->bufferCount);
		
		FileClose(scan->aos_file);
		scan->aos_file = -1;
		scan->aos_filenamepath[0] = '\0';
	}
	
	/*
	 * call BufferedReadCompleteFile if not already done
	 */
	if((&scan->bufferedRead)->file >= 0)
		BufferedReadCompleteFile(&scan->bufferedRead);
	
	scan->aos_need_new_segfile = true;
}

/*
 * Finished writing to this file segment. Update catalog and close file.
 */
static void
CloseWritableFileSeg(AppendOnlyInsertDesc aoInsertDesc)
{
	int64	fileLen;
	int64	fileLen_uncompressed;
	
	/*
	 * We pad out append commands to the page boundary.
	 */
	doPadOutRemainder(aoInsertDesc, /* indicate till end of page */ -1);
	
	/* 
	 * Have the BufferedAppend module let go, but this does not close the file.
	 */
	BufferedAppendCompleteFile(&aoInsertDesc->bufferedAppend, 
							   &fileLen, 
							   &fileLen_uncompressed);

	/* 
	 * Update the AO segment info table with our new eof
	 */
	UpdateFileSegInfo(aoInsertDesc->aoi_rel, 
					  aoInsertDesc->cur_segno,
					  fileLen,
					  fileLen_uncompressed,
					  aoInsertDesc->insertCount,
					  aoInsertDesc->varblockCount);
	
	/*
	 * We must take care of fsynching to disk ourselves since
	 * the fd API won't do it for us.
	 */
	FileSync(aoInsertDesc->appendFile);
	
	FileClose(aoInsertDesc->appendFile);
	
	aoInsertDesc->appendFile = -1;
	aoInsertDesc->appendFilePathName[0] = '\0';

	if (Debug_appendonly_print_insert)
		elog(LOG, 
		     "Append-only scan closed write file segment #%d for table %s "
		     "(file length " INT64_FORMAT ", insert count %f, VarBlock count %f",
		     aoInsertDesc->cur_segno,
		     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
		     fileLen,
		     aoInsertDesc->insertCount,
		     aoInsertDesc->varblockCount);
		     
}


static bool
getNextStorageBlockInFile(
	AppendOnlyScanDesc 	scan,
	int64				*headerOffsetInFile,
	uint8				**dataBuffer,
	int32				*dataLen,
	int					*executorBlockKind,
	int					*rowCount)
{
	uint8  				*header;
	int32				availableLen;
	int					i;
	AOHeaderCheckError	checkError;
	AoHeaderKind	    headerKind;
	int32				offset;
	int32				length;
	bool				isCompressed;
	int64				fileRemainderLen;
	int32				blockLimitLen;
	int32				overallBlockLen;
	int32				uncompressedLen;
	pg_crc32			storedChecksum;
	pg_crc32			computedChecksum;
	
	/*
	 * Peek ahead just enough so we can see the Append-Only storage
	 * header.
	 *
	 * However, we need to honor the file-system page boundaries here
	 * since we do not let the length information cross the boundary.
	 */
	doSkipPadding(scan, scan->headerLen);

	*headerOffsetInFile = BufferedReadNextBufferPosition(&scan->bufferedRead);

	header = BufferedReadGetNextBuffer(&scan->bufferedRead,
									   scan->headerLen,
									   &availableLen);
	
	if (header == NULL)
	{
		/* done reading the file */
		CloseScannedFileSeg(scan);
		return false;			
	}

	scan->bufferCount++;
				
	if (availableLen != scan->headerLen)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Expected %d bytes and got %d bytes in table %s "
				        "(segment file #%d, header offset in file = " INT64_FORMAT ", bufferCount " INT64_FORMAT ")\n", 
						scan->headerLen, 
						availableLen, 
					    NameStr(scan->aos_rd->rd_rel->relname),
					    SCANNED_SEGNO,
						*headerOffsetInFile,
						scan->bufferCount)));			

	/*
	 * First check for zero padded page remainder.
	 */
	i = 0;
	while (true)
	{
		if (header[i] != 0)
			break;

		i++;
		if (i >= scan->headerLen)
		{
			/*
			 * Skip over zero padding caused when the append command
			 * left a partially full page.
			 */
			doSkipPadding(scan, /* indicated till end of page */ -1);

			/*
			 * Now try to get the peek data from the new page.
			 */
			*headerOffsetInFile = BufferedReadNextBufferPosition(&scan->bufferedRead);
			
			header = BufferedReadGetNextBuffer(&scan->bufferedRead,
											   scan->headerLen,
											   &availableLen);
			
			if (header == NULL)
			{
				/* done reading the file */
				CloseScannedFileSeg(scan);
				return false;			
			}
			
			if (availableLen != scan->headerLen)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Expected %d bytes and found %d bytes in table %s "
						 		"(segment file #%d, header offset in file = " INT64_FORMAT ", bufferCount " INT64_FORMAT ")", 
								scan->headerLen,
								availableLen,
							    NameStr(scan->aos_rd->rd_rel->relname),
							    SCANNED_SEGNO,
								*headerOffsetInFile,
								scan->bufferCount)));			
			break;
		}
	}

	/*
	 * Determine the maximum boundary of the block.
	 * UNDONE: When we have a block directory, we will tighten the limit down.
	 */
	fileRemainderLen = scan->bufferedRead.fileLen -
		               *headerOffsetInFile;
	if (scan->maxBufferLen > fileRemainderLen)
		blockLimitLen = (int32)fileRemainderLen;
	else
		blockLimitLen = scan->maxBufferLen;

	/*
	 * Proceed carefully:
	 * [ 1. Verify header checksum ]
	 *   2. Examine (basic) header.
	 *   3. Examine specific header.
	 * [ 4. Verify the block checksum ]
	 */	
	if (scan->aos_checksum)
	{
		if (!AppendOnlyStorage_VerifyHeaderChecksum(
												header,
												&storedChecksum,
												&computedChecksum))
			elog(ERROR, 
			     "Header checksum does not match in table %s.  Expected 0x%X and found 0x%X "
			     "(segment file #%d, header offset in file = " INT64_FORMAT ", bufferCount " INT64_FORMAT ")",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     storedChecksum,
			     computedChecksum,
			     SCANNED_SEGNO,
			     *headerOffsetInFile,
			     scan->bufferCount);
		}

	/*
	 * Check the (basic) header information.
	 */
	checkError = AppendOnlyStorage_GetHeaderInfo(
											header,
											scan->headerLen,
											&headerKind);
	if (checkError != AOHeaderCheckOk)
		elog(ERROR, "Bad append-only storage header in table %s "
		            "(AO header check error %d, detail '%s', "
				    "segment file #%d, header offset in file = " INT64_FORMAT ", bufferCount " INT64_FORMAT ")",
					NameStr(scan->aos_rd->rd_rel->relname),
					(int)checkError,
					AppendOnlyStorage_GetHeaderCheckErrorStr(),
				    SCANNED_SEGNO,
					*headerOffsetInFile,
					scan->bufferCount);		

	/*
	 * For now, we only have Block headers.
	 */
	Assert(headerKind == AoHeaderKind_Block);

	/*
	 * Check the Block header information.
	 */
	checkError = AppendOnlyStorage_GetBlockHeaderInfo(
											header,
											scan->headerLen,
											scan->aos_checksum,
											blockLimitLen,
											&overallBlockLen,
											&offset,	// Offset to data.
											&length,
											executorBlockKind,
											rowCount,
											&isCompressed);
	if (checkError != AOHeaderCheckOk)
		elog(ERROR, "Bad append-only storage block header in table %s "
			 "(AO header check error %d, detail '%s', "
			 "segment file #%d, header offset in file = " INT64_FORMAT ", bufferCount " INT64_FORMAT ")",
			 NameStr(scan->aos_rd->rd_rel->relname),
			 (int)checkError,
			 AppendOnlyStorage_GetHeaderCheckErrorStr(),
			 SCANNED_SEGNO,
			 *headerOffsetInFile,
			 scan->bufferCount);			
	
	/*
	 * Grow the buffer to the full block length to avoid any
	 * unnecessary copying by BufferedRead.
	 *
	 * Since the BufferedRead module may have to copy information around,
	 * we do not save any pointers to the prior buffer call.  This why
	 * AppendOnlyStorage_GetHeaderInfo passes back the offset to the data, 
	 * not a pointer.
	 */	
	header = BufferedReadGrowBuffer(&scan->bufferedRead,
									overallBlockLen,
									&availableLen);

	if (overallBlockLen != availableLen)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Wrong buffer length in table %s.  Eexpected %d byte length buffer and got %d "
				        "(segment file #%d, header offset in file = " INT64_FORMAT ", bufferCount " INT64_FORMAT ")",
					    NameStr(scan->aos_rd->rd_rel->relname),
						overallBlockLen,
						availableLen,
						SCANNED_SEGNO,
					    *headerOffsetInFile,
						scan->bufferCount)));


	if (scan->aos_checksum &&
		gp_appendonly_verify_block_checksums)
	{
		/*
		 * Now that the header has been verified, verify the block checksum
		 * in the header with the checksum of the data portion.
		 */
		if (!AppendOnlyStorage_VerifyBlockChecksum(
												header,
												overallBlockLen,
												&storedChecksum,
												&computedChecksum))
			elog(ERROR, 
			     "Block checksum does not match in table %s.  Expected 0x%X and found 0x%X "
			     "(segment file #%d, header offset in file = " INT64_FORMAT ", bufferCount " INT64_FORMAT ")",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     storedChecksum,
			     computedChecksum,
			     SCANNED_SEGNO,
			     *headerOffsetInFile,
			     scan->bufferCount);
	}

	if (Debug_appendonly_print_storage_headers)
		AppendOnlyStorage_LogBlockHeader(
							NameStr(scan->aos_rd->rd_rel->relname),
							SCANNED_SEGNO,
							*headerOffsetInFile,
							header,
							scan->aos_checksum);
		
	if (!isCompressed)
	{
		*dataBuffer = 		&header[offset];
		*dataLen = 			length;
		
		if (Debug_appendonly_print_scan)
			elog(LOG,
				 "Append-only scan read non-compressed block for table '%s' "
				 "(length = %d, segment file #%d, header offset in file = " INT64_FORMAT ", block count " INT64_FORMAT ")",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     length,
			     SCANNED_SEGNO,
			     *headerOffsetInFile,
			     scan->bufferCount);
	}
	else
	{
		/*
		 * Decompress into the uncompressedBuffer.
		 */
		uncompressedLen = AppendOnlyStorage_GetUncompressedLen(header);
		
		doDecompress(
					scan,
					&header[offset],			// Compressed data in block.
					length,						// Interpret as compressed data length.
					scan->uncompressedBuffer,	// Temporary buffer to hold uncompressed data.
					uncompressedLen);

		*dataBuffer = 		scan->uncompressedBuffer;
		*dataLen = 			uncompressedLen;
		
		if (Debug_appendonly_print_scan)
			elog(LOG,
			     "Append-only scan read decompressed block for table '%s' "
				 "(compressed length %d, length = %d, segment file #%d, "
				 "header offset in file = " INT64_FORMAT ", block count " INT64_FORMAT ")",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     length,
			     uncompressedLen,
			     SCANNED_SEGNO,
			     *headerOffsetInFile,
			     scan->bufferCount);
	}

	return true;
}	

/*
 * You can think of this routine as get next "executor" AO block.
 *
 * It calls up on getNextStorageBlockInFile to get the next AO storage block
 * and returns pointers to the data inside, which is owned by the executor.
 */
static bool
getNextBlock(
	AppendOnlyScanDesc 	scan,
	int					*executorBlockKind)
{
	int64	headerOffsetInFile;
	uint8  	*buffer = NULL;
	int32	bufferLen = 0;
	int		rowCount;

	VarBlockCheckError varBlockCheckError;

	
	if (scan->aos_need_new_segfile)
	{
		/* need to open a new segment file */
		if(!SetNextFileSegForRead(scan))
			return false; 
	}

	if (!getNextStorageBlockInFile(
								scan,
								&headerOffsetInFile,
								&buffer,
								&bufferLen,
								executorBlockKind,
								&rowCount))
	{
		/* done reading the file */
		return false;			
	}

	/*
	 * The executorBlockKind value is what the executor -- i.e. the upper
	 * part of this appendonlyam module! -- has stored in the Append-Only
	 * Storage header.  We interpret it here.
	 */

	switch (*executorBlockKind)
	{
	case AoExecutorBlockKind_VarBlock:
		varBlockCheckError = VarBlockIsValid(buffer, bufferLen);
		if (varBlockCheckError != VarBlockCheckOk)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("VarBlock  is not valid "
					        "(valid block check error %d, detail '%s', segment file #%d, "
					        "header offset in file = " INT64_FORMAT ", block count " INT64_FORMAT ")\n", 
							varBlockCheckError,
							VarBlockGetCheckErrorStr(),
							SCANNED_SEGNO,
							headerOffsetInFile,
							scan->bufferCount)));				
		/*
		 * Now use the VarBlock module to extract the items out.
		 */
		VarBlockReaderInit(&scan->varBlockReader, 
						   buffer, 
						   bufferLen);
		
		scan->currentItemCount = 0;
		scan->readerItemCount = VarBlockReaderItemCount(&scan->varBlockReader);	

		if (rowCount != scan->readerItemCount)
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Row count %d in append-only storage header does not match VarBlock item count %d "
					        "(segment file #%d, header offset in file = " INT64_FORMAT ", block count " INT64_FORMAT ")\n", 
							rowCount,
							scan->readerItemCount,
							SCANNED_SEGNO,
							headerOffsetInFile,
							scan->bufferCount)));				
		}

		if (Debug_appendonly_print_scan)
		{
			elog(LOG,"Append-only scan read VarBlock for table '%s' with %d items (block count " INT64_FORMAT ")",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     scan->readerItemCount,
			     scan->bufferCount);
		}
		break;

	case AoExecutorBlockKind_SingleRow:
		if (rowCount != 1)
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Row count %d in append-only storage header is not 1 for single row "
					        "(segment file #%d, header offset in file = " INT64_FORMAT ", block count " INT64_FORMAT ")\n", 
							rowCount,
							SCANNED_SEGNO,
							headerOffsetInFile,
							scan->bufferCount)));				
		}
		scan->singleRow = buffer;
		scan->singleRowLen = bufferLen;
		if (Debug_appendonly_print_scan)
		{
			elog(LOG,"Append-only scan read single row for table '%s' with length %d (block count " INT64_FORMAT ")",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     scan->singleRowLen,
			     scan->bufferCount);
		}
		break;
		
	default:
		elog(ERROR, "Unrecognized append-only executor block kind: %d",
			*executorBlockKind);
		break;
	}	

	return true;
}

static bool
doProcessTuple(
	AppendOnlyScanDesc 	scan,
	MemTuple			tuple,
	int32				tupleLen,
	int 				nkeys,
	ScanKey 			key,
	TupleTableSlot 		*slot)
{
	bool	valid = true;	// Assume for HeapKeyTestUsingSlot define.
	AOTupleId *aoTupleId = (AOTupleId*)&scan->cdb_fake_ctid;
	int segmentFileNum = SCANNED_SEGNO;

	/*
	 * The row number, regardless of whether it qualifies to be returned.
	 * The count is initialized to 0; the first row is 1.
	 */
	scan->rowNumber++;
	scan->rowNumberInSegmentFile++;

	AOTupleIdInit_Init(aoTupleId);
	AOTupleIdInit_segmentFileNum(aoTupleId, segmentFileNum);
	AOTupleIdInit_rowNum(aoTupleId, scan->rowNumberInSegmentFile);
	
	if(slot)
	{
		ExecStoreMemTuple(tuple, slot, false);

		slot->tts_synthetic_ctid = scan->cdb_fake_ctid;
	}
	
	/* skip visibility test, all tuples are visible */
	
	if (key != NULL)
		HeapKeyTestUsingSlot(slot, nkeys, key, valid);

	if (Debug_appendonly_print_scan_tuple && valid)
		elog(LOG,"Append-only scan tuple for table '%s' "
		     "(AOTupleId %s, tuple length %d, memtuple length %d, block count " INT64_FORMAT ")",
		     NameStr(scan->aos_rd->rd_rel->relname),
		     AOTupleIdToString(aoTupleId),
		     tupleLen,
		     memtuple_get_size(tuple, scan->mt_bind),
		     scan->bufferCount);
	
	return valid;				
}

/* ----------------
 *		appendonlygettup - fetch next heap tuple
 *
 *		Initialize the scan if not already done; then advance to the next
 *		tuple in forward direction; return the next tuple in scan->aos_ctup,
 *		or set scan->aos_ctup.t_data = NULL if no more tuples.
 *
 * Note: the reason nkeys/key are passed separately, even though they are
 * kept in the scan descriptor, is that the caller may not want us to check
 * the scankeys.
 * ----------------
 */
static MemTuple
appendonlygettup(AppendOnlyScanDesc scan,
				 ScanDirection dir,
				 int nkeys,
				 ScanKey key,
				 TupleTableSlot *slot)
{
	MemTuple	tuple = &(scan->aos_ctup);
	int			executorBlockKind;

	Assert(ScanDirectionIsForward(dir));
	Assert(scan->maxBufferLen > 0 && scan->largeReadLen == scan->maxBufferLen * 2);
	
	for(;;)
	{
		executorBlockKind = AoExecutorBlockKind_VarBlock;		// Assume.
		
		if(scan->bufferDone)
		{
			/*
			 * Get the next block. We call this function until we
			 * successfully get a block to process, or finished reading
			 * all the data (all 'segment' files) for this relation.
			 */
			while(!getNextBlock(scan, &executorBlockKind))
			{
				/* have we read all this relation's data. done! */
				if(scan->aos_done_all_segfiles)
					return NULL;					
			}
			
			scan->bufferDone = false;
		}	

		switch (executorBlockKind)
		{
		case AoExecutorBlockKind_VarBlock:
		
			/*
			 * get the next item (tuple) from the varblock
			 */
			while (true)
			{
				int		itemLen = 0;
				uint8  *itemPtr;
				
				itemPtr = VarBlockReaderGetNextItemPtr(&scan->varBlockReader,&itemLen);
							
				if (itemPtr == NULL)
				{
					/* no more items in the varblock, get new buffer */
					scan->bufferDone = true;
					break;
				}
				
				scan->currentItemCount++;
				
				if (itemLen > 0)
				{
					if(tuple == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_GP_INTERNAL_ERROR),
								 errmsg("Invalid tuple format in appendonlygettup")));			
					
					tuple = (MemTuple) itemPtr;

					if (doProcessTuple(
									scan,
									tuple,
									itemLen,
									nkeys,
									key,
									slot))
						return tuple;
				}
				
			}
			
			/* varblock sanity check */
			if (scan->readerItemCount != scan->currentItemCount)
				elog(NOTICE, "Varblock mismatch: Reader count %d, found %d items\n",
							scan->readerItemCount,
							scan->currentItemCount);
			break;
		
		case AoExecutorBlockKind_SingleRow:
			{
				int32 singleRowLen;
				
				Assert(scan->singleRow != NULL);
				Assert(scan->singleRowLen != 0);
				
				if(tuple == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_GP_INTERNAL_ERROR),
							 errmsg("Invalid tuple format in appendonlygettup")));			
					
				tuple = (MemTuple) scan->singleRow;
				singleRowLen = scan->singleRowLen;
				
				scan->singleRow = NULL;
				scan->singleRowLen = 0;
				scan->bufferDone = true;
				
				if (doProcessTuple(
								scan,
								tuple,
								singleRowLen,
								nkeys,
								key,
								slot))
					return tuple;
			}
			break;
			
		default:
			elog(ERROR, "Unrecognized append-only executor block kind: %d",
				executorBlockKind);
			break;
		}
	}
	
}

static void
setupNextWriteBlock(AppendOnlyInsertDesc aoInsertDesc)
{
	if(!aoInsertDesc->shouldCompress)
	{
		uint8	*header;
		
		/*
		 * Allocate space directly in the buffered append write memory.
		 */
		header = BufferedAppendGetMaxBuffer(&aoInsertDesc->bufferedAppend);
		if (header == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("We do not expect files to be have a maximum length")));						

		/* 
		 * Prepare our VarBlock for items.  Leave room for the Append-Only
		 * Storage header.
		 */
		VarBlockMakerInit(&aoInsertDesc->varBlockMaker, 
						  &header[aoInsertDesc->headerLen], 
						  aoInsertDesc->maxDataLen, 
						  aoInsertDesc->tempSpace, 
						  aoInsertDesc->tempSpaceLen);	

		aoInsertDesc->nonCompressedHeader = header;
	}
	else
	{
		/* 
		 * Block oriented compression.  We also restrict the size of the
		 * buffer to leave room for the Append-Only Storage header in case the
		 * block cannot be compressed by the compress library.
		 */
		VarBlockMakerInit(&aoInsertDesc->varBlockMaker, 
						  aoInsertDesc->uncompressedBuffer, 
						  aoInsertDesc->maxDataLen, 
						  aoInsertDesc->tempSpace, 
						  aoInsertDesc->tempSpaceLen);
		
		aoInsertDesc->nonCompressedHeader = NULL;
	}
	
	aoInsertDesc->bufferCount++;
}

static void
finishWriteBlock(AppendOnlyInsertDesc aoInsertDesc)
{
	int		executorBlockKind;
	int		itemCount;
	int32 	dataLen;

	executorBlockKind = AoExecutorBlockKind_VarBlock;	// Assume.
	
	itemCount = VarBlockMakerItemCount(&aoInsertDesc->varBlockMaker);
	if (itemCount == 0)
	{
		/*
		 * "Cancel" the last block allocation.
		 */
		BufferedAppendCancelLastBuffer(&aoInsertDesc->bufferedAppend);
		return;
	}
	
	dataLen = VarBlockMakerFinish(&aoInsertDesc->varBlockMaker);

	aoInsertDesc->varblockCount++;

	if(!aoInsertDesc->shouldCompress)
	{
		uint8 	*nonCompressedHeader;
		uint8	*nonCompressedData;
		int32 	dataRoundedUpLen;
		int32	totallen;
		int32 	uncompressedlen;
		
		nonCompressedHeader = aoInsertDesc->nonCompressedHeader;

			
		nonCompressedData = &nonCompressedHeader[aoInsertDesc->headerLen];
		if (itemCount == 1)
		{
			dataLen = VarBlockCollapseToSingleItem(
						   /* target */ nonCompressedData, 
					       /* source */ &nonCompressedHeader[aoInsertDesc->headerLen],
					       /* sourceLen */ dataLen);
			executorBlockKind = AoExecutorBlockKind_SingleRow;
		}
		
		dataRoundedUpLen = AOStorage_RoundUp(dataLen);

		AOStorage_ZeroPad(
				nonCompressedData,
				dataLen,
				dataRoundedUpLen);
		
		/*
		 * Make the header and compute the checksum if necessary.
		 */
		AppendOnlyStorage_MakeBlockHeader(
								nonCompressedHeader,
								aoInsertDesc->usingChecksum,
								executorBlockKind,
								itemCount,
								dataLen,
								/* compressedLength */ 0);


		if (Debug_appendonly_print_storage_headers)
			AppendOnlyStorage_LogBlockHeader(
								NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
								aoInsertDesc->cur_segno,
								BufferedAppendCurrentBufferPosition(&aoInsertDesc->bufferedAppend),
								nonCompressedHeader,
								aoInsertDesc->usingChecksum);
		
		totallen = aoInsertDesc->headerLen + dataRoundedUpLen;
		uncompressedlen = totallen; /* since there's no compression.. */

		BufferedAppendFinishBuffer(
					&aoInsertDesc->bufferedAppend, 
					totallen,
					uncompressedlen);
		
		if (Debug_appendonly_print_insert)
			elog(LOG,
			     "Append-only insert finished uncompressed block for table '%s' "
			     "(length = %d, application specific %d, item count %d, block count " INT64_FORMAT ")",
			     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
			     dataLen,
			     executorBlockKind,
			     itemCount,
			     aoInsertDesc->bufferCount);
	}
	else
	{
		if (itemCount == 1)
		{
			dataLen = VarBlockCollapseToSingleItem(
							   /* target */ aoInsertDesc->uncompressedBuffer, 
						       /* source */ aoInsertDesc->uncompressedBuffer,
						       /* sourceLen */ dataLen);
			executorBlockKind = AoExecutorBlockKind_SingleRow;
		}

		doCompressAppend(
					 aoInsertDesc, 
					 aoInsertDesc->uncompressedBuffer, 
					 dataLen,
					 executorBlockKind,
					 itemCount);
	}
}

/* ----------------------------------------------------------------
 *					 append-only access method interface
 * ----------------------------------------------------------------
 */


/* ----------------
 *		appendonly_beginscan	- begin relation scan
 * ----------------
 */
AppendOnlyScanDesc
appendonly_beginscan(Relation relation, Snapshot snapshot, int nkeys, ScanKey key)
{
	AppendOnlyScanDesc	scan;
	AppendOnlyEntry		*aoentry;
	char*				comptype;
	int					complevel;
	
	/*
	 * increment relation ref count while scanning relation
	 *
	 * This is just to make really sure the relcache entry won't go away while
	 * the scan has a pointer to it.  Caller should be holding the rel open
	 * anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	/*
	 * allocate scan descriptor
	 */
	scan = (AppendOnlyScanDesc) palloc0(sizeof(AppendOnlyScanDescData));
	
	/*
	 * Get the pg_appendonly information for this table
	 */
	aoentry = GetAppendOnlyEntry(RelationGetRelid(relation));
	complevel = aoentry->compresslevel;
	comptype = aoentry->compresstype;
	Assert(aoentry->majorversion == 1 && aoentry->minorversion == 0);
	
	/*
	 * initialize the scan descriptor
	 */
	scan->aos_file = -1;
	scan->aos_filenamepath_maxlen = AOSegmentFilePathNameLen(relation) + 1;
	scan->aos_filenamepath = (char*)palloc(scan->aos_filenamepath_maxlen);
	scan->aos_filenamepath[0] = '\0';
	scan->maxBufferLen = getLimitedMaxBufferLen(aoentry->blocksize);
	scan->safefswritesize = aoentry->safefswritesize;
	scan->aos_bufread_inited = false;
	scan->aos_rd = relation;
	scan->aos_nkeys = nkeys;
	scan->largeReadLen = 2 * scan->maxBufferLen;
	scan->aoScanInitContext = CurrentMemoryContext;
	scan->aos_compressed = (complevel > 0 ? true : false);
	scan->aos_compresslevel = complevel;
	scan->aos_compresstype = CompressorStrToType(comptype, complevel);
		
	scan->aos_checksum = aoentry->checksum;
			
	scan->headerLen = AppendOnlyStorage_HeaderLenNeeded(scan->aos_checksum);
	scan->maxDataLen = 
					scan->maxBufferLen -
	 				scan->headerLen;
	scan->uncompressedBuffer = NULL;
	scan->singleRow = NULL;
	scan->singleRowLen = 0;

	scan->mt_bind = create_memtuple_binding(RelationGetDescr(relation));
	
	/*
	 * Get information about all the file segments we need to scan
	 */
	scan->aos_segfile_arr = GetAllFileSegInfo(relation, snapshot, &scan->aos_total_segfiles);
		
	/*
	 * we do this here instead of in initscan() because appendonly_rescan also calls
	 * initscan() and we don't want to allocate memory again
	 */
	if (nkeys > 0)
		scan->aos_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->aos_key = NULL;

	pgstat_initstats(&scan->aos_pgstat_info, relation);
	initscan(scan, key);

	pfree(aoentry);
	
	return scan;
}

/* ----------------
 *		appendonly_rescan		- restart a relation scan
 *
 *
 * TODO: instead of freeing resources here and reallocating them in initscan
 * over and over see which of them can be refactored into appendonly_beginscan
 * and persist there until endscan is finally reached. For now this will do.
 * ----------------
 */
void
appendonly_rescan(AppendOnlyScanDesc scan,
				  ScanKey key)
{
	
	CloseScannedFileSeg(scan);
	BufferedReadFinish(&scan->bufferedRead);
	
	if(scan->uncompressedBuffer)
	{
		pfree(scan->uncompressedBuffer);
		scan->uncompressedBuffer = NULL;
	}
	
	scan->aos_need_new_segfile = true;

	/*
	 * reinitialize scan descriptor
	 */
	initscan(scan, key);
}

/* ----------------
 *		appendonly_endscan	- end relation scan
 * ----------------
 */
void
appendonly_endscan(AppendOnlyScanDesc scan)
{

	RelationDecrementReferenceCount(scan->aos_rd);

	if (scan->aos_key)
		pfree(scan->aos_key);
	
	CloseScannedFileSeg(scan);	
	BufferedReadFinish(&scan->bufferedRead);

	if(scan->uncompressedBuffer)
	{
		pfree(scan->uncompressedBuffer);
		scan->uncompressedBuffer = NULL;
	}

	pfree(scan->aos_filenamepath);
	
	pfree(scan);
}

/* ----------------
 *		appendonly_getnext	- retrieve next tuple in scan
 * ----------------
 */
MemTuple
appendonly_getnext(AppendOnlyScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	MemTuple tup = appendonlygettup(scan, direction, scan->aos_nkeys, scan->aos_key, slot);

	if (tup == NULL)
	{
		if(slot)
			ExecClearTuple(slot);
		
		return NULL;
	}

	pgstat_count_heap_getnext(&scan->aos_pgstat_info);

	return tup;
}

/*
 * appendonly_insert_init
 *
 * before using appendonly_insert() to insert tuples we need to call
 * this function to initialize our varblock and bufferedAppend structures
 * and memory for appending data into the relation file.
 *
 * see appendonly_insert() for more specifics about inserting tuples into
 * append only tables.
 */
AppendOnlyInsertDesc
appendonly_insert_init(Relation rel, int segno)
{
	AppendOnlyInsertDesc	aoInsertDesc;
	AppendOnlyEntry			*aoentry;
	uint8					*memory;
	int32					memoryLen;
	int						maxtupsize;
		
	/*
	 * Get the pg_appendonly information for this table
	 */
	aoentry = GetAppendOnlyEntry(RelationGetRelid(rel));
	Assert(aoentry->majorversion == 1 && aoentry->minorversion == 0);

	/*
	 * allocate and initialize the insert descriptor
	 */	
	aoInsertDesc  = (AppendOnlyInsertDesc) palloc0(sizeof(AppendOnlyInsertDescData));
	aoInsertDesc->aoi_rel = rel;
	aoInsertDesc->mt_bind = create_memtuple_binding(RelationGetDescr(rel));
	
	aoInsertDesc->appendFile = -1;
	aoInsertDesc->appendFilePathNameMaxLen = AOSegmentFilePathNameLen(rel) + 1;
	aoInsertDesc->appendFilePathName = (char*)palloc(aoInsertDesc->appendFilePathNameMaxLen);
	aoInsertDesc->appendFilePathName[0] = '\0';

	aoInsertDesc->bufferCount = 0;
	aoInsertDesc->insertCount = 0;
	aoInsertDesc->varblockCount = 0;
	
	Assert(segno >= 0);
	aoInsertDesc->cur_segno = segno;

	aoInsertDesc->safefswritesize = aoentry->safefswritesize;
	aoInsertDesc->compressLevel = aoentry->compresslevel;
	aoInsertDesc->compressType = CompressorStrToType(aoentry->compresstype, aoentry->compresslevel);
	aoInsertDesc->shouldCompress = (aoInsertDesc->compressLevel > 0 ? true : false);
	
	aoInsertDesc->usingChecksum = aoentry->checksum;

	aoInsertDesc->maxBufferLen = getLimitedMaxBufferLen(aoentry->blocksize);
	aoInsertDesc->headerLen = AppendOnlyStorage_HeaderLenNeeded(aoInsertDesc->usingChecksum);
	aoInsertDesc->maxDataLen = 
						aoInsertDesc->maxBufferLen -
		 				aoInsertDesc->headerLen;
	aoInsertDesc->largeWriteLen = 2 * aoInsertDesc->maxBufferLen;

	aoInsertDesc->nonCompressedHeader = NULL;

	if (aoInsertDesc->shouldCompress)
	{
		aoInsertDesc->uncompressedBuffer = (uint8 *) palloc(aoInsertDesc->maxBufferLen * sizeof(uint8));
		
		/*
		 * if quicklz compression used, set up scratch space.
		 */
		switch(aoInsertDesc->compressType)
		{
			case COMPTYPE_QUICKLZ1:
				aoInsertDesc->qlz_scratch = (char*) palloc(QLZ1_SCRATCH_COMPRESS);
				memset(aoInsertDesc->qlz_scratch, 0, QLZ1_SCRATCH_COMPRESS);			
				break;
			case COMPTYPE_QUICKLZ3:
				aoInsertDesc->qlz_scratch = (char*) palloc(QLZ3_SCRATCH_COMPRESS);
				memset(aoInsertDesc->qlz_scratch, 0, QLZ3_SCRATCH_COMPRESS);			
				break;
			case COMPTYPE_ZLIB:
				break; /* nothing to do */
			case COMPTYPE_NONE:
			default:
				Insist(false);
		}

	}
	else
	{
		aoInsertDesc->uncompressedBuffer = NULL;
		aoInsertDesc->qlz_scratch = NULL;		
	}
	
	aoInsertDesc->tempSpaceLen = aoInsertDesc->maxBufferLen / 8; /* TODO - come up with a more efficient calculation */
	aoInsertDesc->tempSpace = (uint8 *) palloc(aoInsertDesc->tempSpaceLen * sizeof(uint8));
	maxtupsize = aoInsertDesc->maxDataLen - VARBLOCK_HEADER_LEN - 4;
	aoInsertDesc->toast_tuple_threshold = maxtupsize / 4; /* see tuptoaster.h for more information */
	aoInsertDesc->toast_tuple_target = maxtupsize / 4;
		
	memoryLen = BufferedAppendMemoryLen(aoInsertDesc->maxBufferLen, aoInsertDesc->largeWriteLen);
	memory = (uint8*)palloc(memoryLen);
	
	/* 
	 * set up the the buffered appender for this data load 
	 */
	BufferedAppendInit(&aoInsertDesc->bufferedAppend, 
					   memory,
					   memoryLen, 
					   aoInsertDesc->maxBufferLen, 
					   aoInsertDesc->largeWriteLen);
				
	if (Debug_appendonly_print_insert)
		elog(LOG,"Append-only insert initialize for table '%s' segment file %u "
		     "(compression = %s, compression level %d, maximum buffer length %d, large write length %d)",
		     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
		     aoInsertDesc->cur_segno,
		     (aoInsertDesc->shouldCompress ? "true" : "false"),
		     aoInsertDesc->compressLevel,
		     aoInsertDesc->maxBufferLen,
		     aoInsertDesc->largeWriteLen);
	
	/* open our current relation file segment for write */
	SetCurrentFileSegForWrite(aoInsertDesc);
	
	Assert(aoInsertDesc->tempSpaceLen > 0);

	setupNextWriteBlock(aoInsertDesc);

	pfree(aoentry);

	return aoInsertDesc;
}


/*
 *	appendonly_insert		- insert tuple into a varblock
 *
 * Note the following major differences from heap_insert
 *
 * - wal is always bypassed here.
 * - transaction information is of no interest.
 * - tuples inserted into varblocks, not via the postgresql buf/page manager.
 * - no need to pin buffers.
 *
 * The return value is the OID assigned to the tuple (either here or by the
 * caller), or InvalidOid if no OID.  The header fields of *tup are updated
 * to match the stored tuple;  
 */
Oid
appendonly_insert(AppendOnlyInsertDesc aoInsertDesc, MemTuple instup)
{
	Relation		 relation = aoInsertDesc->aoi_rel;
	VarBlockByteLen	 itemLen;
	uint8			*itemPtr;
	MemTuple 		 tup = NULL;
	Oid 			retOid;
	bool			need_toast;

	Assert(aoInsertDesc->maxBufferLen > 0 && aoInsertDesc->tempSpaceLen > 0);
    Assert(aoInsertDesc->largeWriteLen == aoInsertDesc->maxBufferLen * 2);
	Assert(aoInsertDesc->toast_tuple_threshold > 0 && aoInsertDesc->toast_tuple_target > 0);

	Insist(relation->rd_rel->relstorage == RELSTORAGE_AO);
	if (relation->rd_rel->relhasoids)
	{

		/*
		 * If the object id of this tuple has already been assigned, trust the
		 * caller.	There are a couple of ways this can happen.  At initial db
		 * creation, the backend program sets oids for tuples. When we define
		 * an index, we set the oid.  Finally, in the future, we may allow
		 * users to set their own object ids in order to support a persistent
		 * object store (objects need to contain pointers to one another).
		 */
		if (!OidIsValid(MemTupleGetOid(instup, aoInsertDesc->mt_bind)))
			MemTupleSetOid(instup, aoInsertDesc->mt_bind, GetNewOid(relation));
	}
	else
	{
		/* check there is not space for an OID */
		MemTupleNoOidSpace(instup);
	}

	need_toast = (MemTupleHasExternal(instup, aoInsertDesc->mt_bind) ||
				  memtuple_get_size(instup, aoInsertDesc->mt_bind) > 
				  aoInsertDesc->toast_tuple_threshold);
	
	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 *
	 * Note: below this point, tup is the data we actually intend to store
	 * into the relation; instup is the caller's original untoasted data.
	 */	
	if (need_toast)
		tup = (MemTuple) toast_insert_or_update(relation, (HeapTuple) instup, 
												NULL, aoInsertDesc->mt_bind,
												aoInsertDesc->toast_tuple_target);
	else
		tup = instup;

	/* 
	 * get space to insert our next item (tuple) 
	 */
	itemLen = memtuple_get_size(tup, aoInsertDesc->mt_bind);

	/*
	 * If we are at the limit for append-only storage header's row count,
	 * force this VarBlock to finish.
	 */
	if (VarBlockMakerItemCount(&aoInsertDesc->varBlockMaker) >= AOBlockHeader_MaxRowCount)
		itemPtr = NULL;
	else
		itemPtr = VarBlockMakerGetNextItemPtr(&aoInsertDesc->varBlockMaker, itemLen);
	
	/*
	 * If no more room to place items in the current varblock
	 * finish it and start inserting into the next one.
	 */
	if (itemPtr == NULL)
	{		
		if (VarBlockMakerItemCount(&aoInsertDesc->varBlockMaker) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Item too long (#1): length %d, maxBufferLen %d", 
					        itemLen, aoInsertDesc->varBlockMaker.maxBufferLen)));

		finishWriteBlock(aoInsertDesc);
		
		setupNextWriteBlock(aoInsertDesc);

		itemPtr = VarBlockMakerGetNextItemPtr(&aoInsertDesc->varBlockMaker, itemLen);

		if (itemPtr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Item too long (#2): length %d, maxBufferLen %d", 
					        itemLen, aoInsertDesc->varBlockMaker.maxBufferLen)));			

	}
		
	Assert(itemPtr != NULL);

	if (itemLen > 0)
		memcpy(itemPtr, tup, itemLen);
	
	aoInsertDesc->insertCount++;
	pgstat_count_heap_insert(&relation->pgstat_info);

	retOid = MemTupleGetOid(tup, aoInsertDesc->mt_bind);

	if (Debug_appendonly_print_insert_tuple)
	{
		AOTupleId 	aoTupleId;
		int64		rowNum = (int64)aoInsertDesc->insertCount;
		
		AOTupleIdInit_Init(&aoTupleId);
		AOTupleIdInit_segmentFileNum(&aoTupleId, aoInsertDesc->cur_segno);
		AOTupleIdInit_rowNum(&aoTupleId, rowNum);
		
		elog(LOG,"Append-only insert tuple for table '%s' "
		     "(AOTupleId %s, memtuple length %d, block count " INT64_FORMAT ")",
		     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
		     AOTupleIdToString(&aoTupleId),
		     itemLen,
		     aoInsertDesc->bufferCount);
	}
	
	if(tup != instup)
		pfree(tup);

	return retOid;
}

/*
 * appendonly_insert_finish
 *
 * when done inserting all the data via appendonly_insert() we need to call
 * this function to flush all remaining data in the buffer into the file.
 */
void
appendonly_insert_finish(AppendOnlyInsertDesc aoInsertDesc)
{
	/* 
	 * Finish up that last varblock.
	 */
	finishWriteBlock(aoInsertDesc);
	
	CloseWritableFileSeg(aoInsertDesc);
	
	pfree(aoInsertDesc);
}

static void
issueCompressError(
	int				zlibCompressError,
	int32			sourceLen)
{
	char *detail = NULL;
	
	switch(zlibCompressError)
	{
	case Z_MEM_ERROR:
		detail = "Insufficient memory";
		break;
	case Z_BUF_ERROR:
		detail = "The buffer was not large enough to hold the compressed data";
		break;
	case Z_STREAM_ERROR:
		detail = "The level was not Z_DEFAULT_LEVEL, or was not between 0 and 9";
		break;
	default:
		elog(ERROR,"ZLIB compress2 failed -- reason %d unknown (uncompressed length %d)",
			 zlibCompressError, sourceLen);
		break;
	}

	elog(ERROR,"ZLIB compress2 failed (detail: '%s', uncompressed length %d)",
		 detail, sourceLen);
}
	
static int
tryToCompress(
	 uint8		*sourceData,
	 int32		sourceLen,
	 uint8		*compressedBuffer,
	 int32		maxCompressedLen,
	 int32		*compressedLen,
	 int		compressLevel,
	 CompressorType  compressType,
	 char		*qlz_scratch)
{
	unsigned long 	compressedSize;
	
	Assert(compressLevel > 0);
	
	if(compressType == COMPTYPE_ZLIB)
	{

#ifndef HAVE_LIBZ
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Don't have ZLIB on the system!")));
		*compressedLen = 0;
		return -1;	// Never reaches here.
#else
		int				zlibCompressError;

		/* 
		 * On input, the 2nd 'destLen' parameter to compress2 is the maximum size
		 * of the compression buffer.  On output, it is the resulting compressed
		 * length.
		 */
		compressedSize = (unsigned long)maxCompressedLen; 
		
		zlibCompressError = compress2(compressedBuffer, &compressedSize, 
									  (Bytef *)sourceData, sourceLen, 
									  compressLevel);
		
		if (zlibCompressError != Z_OK)
			return zlibCompressError;
#endif
	}
	else if(compressType == COMPTYPE_QUICKLZ1)
	{
		Insist(compressLevel == 1);

		compressedSize = qlz1_compress((const void *)sourceData, 
									  (char *)compressedBuffer, 
									  (size_t)sourceLen, 
									  qlz_scratch);
	    
		if(compressedSize > (unsigned long)maxCompressedLen)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Quicklz level 1 compressed size larger than max "
							"allowed size")));
			
		if(compressedSize > sourceLen)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Quicklz level 1 compressed size (%u) larger than "
							"uncompressed size (%u)", (unsigned int)sourceLen, 
							(unsigned int)compressedSize)));		
	}
	else /* COMPTYPE_QUICKLZ3 */
	{
		Insist(compressType == COMPTYPE_QUICKLZ3);
		Insist(compressLevel == 3);
		
		compressedSize = qlz3_compress((const void *)sourceData, 
									  (char *)compressedBuffer, 
									  (size_t)sourceLen, 
									  qlz_scratch);
	    
		if(compressedSize > (unsigned long)maxCompressedLen)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Quicklz level 3 compressed size larger than max "
							"allowed size")));
			
		if(compressedSize > sourceLen)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Quicklz level 3 compressed size (%u) larger than "
							"uncompressed size (%u)", (unsigned int)sourceLen, 
							(unsigned int)compressedSize)));		

	}
	
	Assert(compressedSize > 0);
	*compressedLen = (int32)compressedSize;

	return Z_OK;
}

/*
 * Append data compressed with LIBZ.
 */
static void
doCompressAppend(
	 AppendOnlyInsertDesc 	aoInsertDesc,
	 uint8					*sourceData,
	 int32			 		sourceLen,
	 int					executorBlockKind,
	 int			 		itemCount)
{
	BufferedAppend *bufferedAppend = &aoInsertDesc->bufferedAppend;
	uint8			*header;
	uint8 			*dataBuffer;
	int 	 		res;
	int32			compressedLen;
	int32 			dataRoundedUpLen = 0;	// Shutup compiler.
		
	Assert(aoInsertDesc->maxBufferLen > 0 && 
		   aoInsertDesc->largeWriteLen == aoInsertDesc->maxBufferLen * 2);

	header = BufferedAppendGetMaxBuffer(&aoInsertDesc->bufferedAppend);
	if (header == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("We do not expect files to be have a maximum length")));
	
	dataBuffer = &header[aoInsertDesc->headerLen];

	// UNDONE: Here is a point where we could look at the sourceLen and
	// UNDONE: and decide it is too small to compress and go directly
	// UNDONE: to copying in non-compressed data.
	
	/*
	 * Compress into the BufferedAppend buffer after the large header (and
	 * optional checksum, etc.
	 */
	res = tryToCompress(
					sourceData,
					sourceLen,
					dataBuffer,
					sourceLen,	// Limit compression to be no more than the input size.
					&compressedLen,
					aoInsertDesc->compressLevel,
					aoInsertDesc->compressType,
					aoInsertDesc->qlz_scratch);
	
	if (res == Z_OK)
	{
		/*
		 * Compression successful.
		 */
		dataRoundedUpLen = AOStorage_RoundUp(compressedLen);

		AOStorage_ZeroPad(
					dataBuffer,
					compressedLen,
					dataRoundedUpLen);	
		
		/*
		 * Make the header and compute the checksum if necessary.
		 */
		AppendOnlyStorage_MakeBlockHeader(
									header,
									aoInsertDesc->usingChecksum,
									executorBlockKind,
									itemCount,
									sourceLen,
									compressedLen);
		
		if (Debug_appendonly_print_storage_headers)
			AppendOnlyStorage_LogBlockHeader(
								NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
								aoInsertDesc->cur_segno,
								BufferedAppendCurrentBufferPosition(&aoInsertDesc->bufferedAppend),
								header,
								aoInsertDesc->usingChecksum);
		
		if (Debug_appendonly_print_insert)
			elog(LOG,"Append-only insert finished compressed block for table '%s' "
				 "(segment file #%d, header offset in file " INT64_FORMAT ", "
				 "length = %d, compressed length %d, item count %d, block count " INT64_FORMAT ")",
			     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
			     aoInsertDesc->cur_segno,
			     BufferedAppendCurrentBufferPosition(&aoInsertDesc->bufferedAppend),
			     sourceLen,
			     compressedLen,
			     itemCount,
			     aoInsertDesc->bufferCount);
		
		/*
		 * Fall below to finish the BufferedAppend buffer.
		 */
	}
	else if (res == Z_BUF_ERROR)
	{
		int32 	overallBlockLen;
		
		/*
		 * Unable to compress the data to smaller the input size.
		 * Solution: Indicate in the header we are storing an non-compressed
		 * block.
		 */
		dataRoundedUpLen = AOStorage_RoundUp(sourceLen);

		overallBlockLen = aoInsertDesc->headerLen + dataRoundedUpLen;

		/*
		 * Copy non-compressed data in after the header information.
		 */
		memcpy(dataBuffer, sourceData, sourceLen);
		
		AOStorage_ZeroPad(
					dataBuffer,
					sourceLen,
					dataRoundedUpLen);	

		/*
		 * Make the header and compute the checksum if necessary.
		 */
		AppendOnlyStorage_MakeBlockHeader(
								header,
								aoInsertDesc->usingChecksum,
								executorBlockKind,
								itemCount,
								sourceLen,
								/* compressedLength */ 0);

		if (Debug_appendonly_print_storage_headers)
			AppendOnlyStorage_LogBlockHeader(
								NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
							    aoInsertDesc->cur_segno,
							    BufferedAppendCurrentBufferPosition(&aoInsertDesc->bufferedAppend),
								header,
								aoInsertDesc->usingChecksum);
	
		if (Debug_appendonly_print_insert)
			elog(LOG,"Append-only insert could not compress block for table '%s' smaller -- non-compressed block stored "
				 "(segment file #%d, header offset in file " INT64_FORMAT ", "
				 "length = %d, item count %d, block count " INT64_FORMAT ")",
			     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
			     aoInsertDesc->cur_segno,
			     BufferedAppendCurrentBufferPosition(&aoInsertDesc->bufferedAppend),
			     sourceLen,
			     itemCount,
			     aoInsertDesc->bufferCount);

		/*
		 * Fall below to finish the BufferedAppend buffer.
		 */
	}
	else
		/*
		 * Throw an ERROR.
		 */
		issueCompressError(
						res,
						sourceLen);

	BufferedAppendFinishBuffer(
						bufferedAppend,
						aoInsertDesc->headerLen + dataRoundedUpLen, /* compressed size */
						aoInsertDesc->headerLen + AOStorage_RoundUp(sourceLen) /* non-compressed size */);

}

static void
doDecompress(AppendOnlyScanDesc scan,
			uint8			*compressed,
			int32			compressedLen,
			uint8			*uncompressed,
			int32			uncompressedLen)
{
	unsigned long compressedSize;
	unsigned long uncompressedSize;

	int32 resultingUncompressedLen;
	
	compressedSize = (unsigned long)compressedLen;
	uncompressedSize = (unsigned long)uncompressedLen;

	if(scan->aos_compresstype == COMPTYPE_ZLIB)
	{
		int res;
		
#ifndef HAVE_LIBZ
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Don't have ZLIB on the system!")));			
#else		
		res = uncompress(uncompressed, &uncompressedSize, 
						 (Bytef *)compressed, compressedSize);
	
		if (res != Z_OK)
		{
			char *detail = NULL;
			
			switch(res)
			{
				case Z_MEM_ERROR:
					detail = "Insufficient memory";
					break;
				case Z_BUF_ERROR:
					detail = "The buffer was not large enough to hold the uncompressed data";
					break;
				case Z_DATA_ERROR:
					detail = "The compressed data was corrupted";
					break;
				default:
					elog(ERROR,"ZLIB uncompress failed -- reason %d unknown (output buffer length %d, compressed length %d)",
						 res, uncompressedLen, compressedLen);
					break;
			}
	
			elog(ERROR,"ZLIB uncompress failed (detail: '%s', output buffer length %d, compressed length %d, (block count " INT64_FORMAT "))",
				 detail, uncompressedLen, compressedLen, scan->bufferCount);
		}	
#endif
	}
	else if(scan->aos_compresstype == COMPTYPE_QUICKLZ1)
	{
		uncompressedSize = qlz1_decompress((const char *)compressed, 
										  uncompressed, 
										  scan->qlz_scratch);
	}
	else
	{
		Insist(scan->aos_compresstype == COMPTYPE_QUICKLZ3);

		uncompressedSize = qlz3_decompress((const char *)compressed, 
										  uncompressed, 
										  scan->qlz_scratch);

	}
	
	resultingUncompressedLen = (int32)uncompressedSize;

	if (resultingUncompressedLen != uncompressedLen)
		elog(ERROR, 
			 "Uncompress returned length %d which is different than the " 
			 "expected length %d (block count " INT64_FORMAT ")",
			 resultingUncompressedLen,
			 uncompressedLen,
			 scan->bufferCount);
}

/*
 * RelationGuessNumberOfBlocks 
 *
 * Has the same meaning as RelationGetNumberOfBlocks for heap relations
 * however uses an estimation since AO relations use variable len blocks
 * which are meaningless to the optimizer. 
 *
 * This function, in other words, answers the following question - "If 
 * I were a heap relation, about how many blocks would I have had?"
 */
BlockNumber 
RelationGuessNumberOfBlocks(double totalbytes)
{
	/* for now it's very simple */
	return (BlockNumber)(totalbytes/BLCKSZ) + 1;
}

int32 
getLimitedMaxBufferLen(int32 maxBufferLen)
{
	int32 result;

	/*
	 * UNDONE: Need to fix parser to use new lower blocksize limit.
	 */
	if (maxBufferLen > AOBlockHeader_MaxLength)
		result = AOBlockHeader_MaxLength;
	else
		result = maxBufferLen;

	/*
	 * Round down to 32-bit boundary.
	 */
	result = (result / sizeof(uint32)) * sizeof(uint32);
	
	return result;
}

static CompressorType CompressorStrToType(const char* comp_str, int comp_level)
{
	CompressorType comptype = COMPTYPE_NONE;
	
	if (!comp_str)
	{
		comptype = COMPTYPE_NONE;		
	}
	else if (pg_strcasecmp(comp_str, "zlib") == 0)
	{
		comptype = COMPTYPE_ZLIB;		
	}
	else if (pg_strcasecmp(comp_str, "quicklz") == 0)
	{
		switch(comp_level)
		{
			case 1:
				comptype = COMPTYPE_QUICKLZ1;
				break;
			case 3:
				comptype = COMPTYPE_QUICKLZ3;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Found quicklz with unsupported compression "
								"level %d", comp_level)));
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Found undefined compressor %s", comp_str)));

	}
	
	return comptype;
}


