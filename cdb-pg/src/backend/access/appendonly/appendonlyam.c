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
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "access/appendonlyam.h"
#include "access/heapam.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_appendonly.h"
#include "cdb/cdbvars.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "storage/freespace.h"
#include "storage/smgr.h"

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

static int32 doCompressAppend(AppendOnlyInsertDesc aoInsertDesc,
							 uint8			*sourceData,
							 int32			 sourceLen,
							 int			 compressLevel);
static void doDecompress(AppendOnlyScanDesc scan,
							 uint8			*compressed,
							 int32			compressedLen,
							 int32			*uncompressedLen);

/* ----------------
 *		initscan - scan code common to appendonly_beginscan and appendonly_rescan
 *
 * In here we initialize our bufferedReader and varBlock modules and allocate
 * memory to be used for scanning our data from the relation file. We also set
 * up the scan keys and misc scan options, and finally open the relation file
 * for scanning while setting the current eof as the last valid data byte.
 * ----------------
 */
static void
initscan(AppendOnlyScanDesc scan, ScanKey key)
{
	int32	memoryLen;
	int32	actualMaxBufferLen;
	uint8	*memory;
	Relation	reln = scan->aos_rd;
	char	   *path;
	AppendOnlyEntry *aoentry;
	int64		eof;	
	
	Assert(scan->maxBufferLen > 0 && scan->largeReadLen == scan->maxBufferLen * 2);
	
	if (!scan->aos_compressed)
	{
		actualMaxBufferLen = scan->maxBufferLen;
	}
	else
	{		
		actualMaxBufferLen = scan->maxBufferLen + sizeof(int32);
		actualMaxBufferLen = ((actualMaxBufferLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);
	}
	
	memoryLen = BufferedReadMemoryLen(actualMaxBufferLen, scan->largeReadLen);
	memory = (uint8*)palloc(memoryLen);

	scan->bufferDone = true; /* so we read a new buffer right away */
	
	if (memory == NULL)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("Cannot allocate memory %d bytes",
							   memoryLen)));
	
	/*
	 * Initialize the BufferedRead module with a large amount of memory.
	 */
	BufferedReadInit(&scan->bufferedRead,
					 memory,
					 memoryLen,
					 actualMaxBufferLen,
					 scan->largeReadLen);

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL)
		memcpy(scan->aos_key, key, scan->aos_nkeys * sizeof(ScanKeyData));

	pgstat_count_heap_scan(&scan->aos_pgstat_info);
	
	/* cache the relfilenode value */
	scan->aos_relfilenode = reln->rd_node.relNode;
	Assert(scan->aos_relfilenode != InvalidOid);
	
	/* Get path for this relation file */
	path = relpath(reln->rd_node);
	
	/*
	 * Open the relation file
	 *
	 * Note: because we use BasicOpenFile(), we are responsible for ensuring
	 * the FD gets closed in any error exit path.  
	 */
	scan->aos_file = BasicOpenFile(path, O_RDWR | PG_BINARY, 0600);
	
	if(scan->aos_file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\"", path)));
	
	   
	/* Get the eof of this relation file from the pg_appendonly catalog */
	aoentry = GetAppendOnlyEntry(scan->aos_relfilenode);
	eof = (int64)aoentry->eof;
	
	if (eof < 0)
	{
		close(scan->aos_file);
		
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Invalid eof " INT64_FORMAT " for relation %s", 
						eof, NameStr(scan->aos_rd->rd_rel->relname))));			
	}
	
	/* Set the file to be read with its eof */
	BufferedReadSetFile(&scan->bufferedRead, scan->aos_file, eof);
		
	if (Debug_appendonly_print_scan)
		elog(LOG,"Append-only scan initialize for table '%s' with path = '%s' (compression = %s, maximum buffer length %d, large read length %d)",
		     NameStr(scan->aos_rd->rd_rel->relname),
		     path,
		     (scan->aos_compressed ? "true" : "false"),
		     scan->maxBufferLen,
		     scan->largeReadLen);
	
	pfree(path);
	pfree(aoentry);
		
	/* ready to go! */
	scan->aos_inited = true;
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
	int32   pageRemainder;
	bool	doSkip;
	uint8  *buffer;
	int32	availableLen;
	
	nextReadPosition = BufferedReadNextBufferPosition(&scan->bufferedRead);
	nextBoundaryPosition = 
		((nextReadPosition + scan->maxBufferLen - 1)/scan->maxBufferLen)*scan->maxBufferLen;
	pageRemainder = (int32)(nextBoundaryPosition - nextReadPosition);

	if (pageRemainder == 0)
		doSkip = false;
	else if (skipLen == -1)
	{
		/*
		 * Skip to end of page.
		 */
		doSkip = true;
		skipLen = pageRemainder;
	}
	else
		doSkip = (pageRemainder < skipLen);
	
	if (doSkip)
	{
		/*
		 * Read through the remainder.
		 */
		buffer = BufferedReadGetNextBuffer(&scan->bufferedRead,
										   pageRemainder,
										   &availableLen);

		/*
		 * Since our file EOF should always be a multiple of the file-system
		 * page, we do not expect a short read here.
		 */
		if (buffer == NULL)
			availableLen = 0;
		if (buffer == NULL || pageRemainder != availableLen)
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Unexpected end of file.  Expected to read %d bytes after position " INT64_FORMAT  " but found %d bytes (bufferCount %d)\n", 
							pageRemainder, 
							nextReadPosition, 
							availableLen,
							scan->bufferCount)));			
		}

		// UNDONE: For verification purposes, we should verify the
		// UNDONE: reminder is all zeroes.
		
		if (Debug_appendonly_print_scan)
			elog(LOG,"Append-only scan skipping zero padded remainder for table '%s' (nextReadPosition = " INT64_FORMAT  ", pageRemainder = %d)",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     nextReadPosition,
			     pageRemainder);
	}
}


static bool
getNextBlock(AppendOnlyScanDesc scan)
{
	int32	peekLen = (!scan->aos_compressed ? VARBLOCK_HEADER_LEN : sizeof(int32));
	int32	availableLen;
	int 	i;
	int32	growLen;
	uint8  *buffer;
	int32	blockLen = 0;
	int32	compressedLen = 0;
	int32	uncompressedLen = 0;

	VarBlockCheckError varBlockCheckError;

	/*
	 * Peek ahead just enough so we can determine the length of the 
	 * uncompressed VarBlock or the length for compressed data.
	 *
	 * However, we need to honor the file-system page boundaries here
	 * since we do not let the length information cross the boundary.
	 */
	doSkipPadding(scan, 
	              peekLen);

	buffer = BufferedReadGetNextBuffer(&scan->bufferedRead,
									   peekLen,
									   &availableLen);
	
	if (buffer == NULL)
	{
		/* done reading the file */
		BufferedReadCompleteFile(&scan->bufferedRead);
		scan->aos_inited = false;	
		return false;			
	}
	
	scan->bufferCount++;
				
	if (availableLen != peekLen)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Expected %d bytes and found %d bytes (bufferCount %d)\n", 
						peekLen, availableLen, scan->bufferCount)));			

	/*
	 * First check for zero padded page remainder.
	 */
	i = 0;
	while (true)
	{
		if (buffer[i] != 0)
			break;

		i++;
		if (i >= peekLen)
		{
			/*
			 * Skip over zero padding caused when the append command
			 * left a partially full page.
			 */
			doSkipPadding(scan,
				          /* indicated till end of page */ -1);

			/*
			 * Now try to get the peek data from the new page.
			 */
			buffer = BufferedReadGetNextBuffer(&scan->bufferedRead,
											   peekLen,
											   &availableLen);
			
			if (buffer == NULL)
			{
				/* done reading the file */
				BufferedReadCompleteFile(&scan->bufferedRead);
				scan->aos_inited = false;	
				return false;			
			}
			
			if (availableLen != peekLen)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Expected %d bytes and found %d bytes (bufferCount %d)\n", 
								peekLen, availableLen, scan->bufferCount)));			
			break;
		}
	}

	/*
	 * Now evaluate the peek ahead.
	 */
	if(!scan->aos_compressed)
	{
		varBlockCheckError = VarBlockHeaderIsValid(buffer, availableLen);
		if (varBlockCheckError != VarBlockCheckOk)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("VarBlock header is not valid (bufferCount %d, detail '%s')\n", 
							scan->bufferCount,
							VarBlockGetCheckErrorStr())));			
		
		/*
		 * Ask the VarBlock module to use the header to how large the whole
		 * VarBlock is.
		 */
		blockLen = VarBlockLenFromHeader(buffer, availableLen);
		
		if (blockLen <= peekLen)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Expected %d byte VarBlock length to be larger than %d header bytes (bufferCount %d)", 
							blockLen, peekLen, scan->bufferCount)));							
		
		growLen = blockLen;
	}
	else
	{
		int32	*lengthWord;
		int32	roundUpLen;
	
		lengthWord = (int32*)buffer;
		compressedLen = *lengthWord;
		if (compressedLen == 0)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Expected non-zero compessed data length (bufferCount %d)", 
							scan->bufferCount)));							
		roundUpLen = sizeof(int32) + ((compressedLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);
		growLen = roundUpLen;
	}
	
	/*
	 * Grow the amount we can see just to the VarBlock length to avoid any
	 * unnecessary copying by BufferedRead.
	 *
	 * Since the BufferedRead module may have to copy information around,
	 * we do not save any pointers to the prior buffer call.
	 */	
	buffer = BufferedReadGrowBuffer(&scan->bufferedRead,
									growLen,
									&availableLen);

	if (growLen != availableLen)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Wrong available length (expected %d byte length buffer and got %d (bufferCount %d)",
						growLen, availableLen, scan->bufferCount)));			

	
	/* 
	 * Actually do the decompression now.
	 */
	if(scan->aos_compressed)
	{	
		/*
		 * Decompress into the varBlockBuffer.
		 */
		doDecompress(
					scan,
					buffer + sizeof(int32),
					compressedLen,
					&uncompressedLen);
	}
	
	/*
	 * Validate VarBlock.
	 */
	if(!scan->aos_compressed)
	{
		varBlockCheckError = VarBlockIsValid(buffer, availableLen);
		if (varBlockCheckError != VarBlockCheckOk)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("VarBlock  is not valid (valid block check error %d, detail '%s', block count %d)\n", 
							varBlockCheckError,
							VarBlockGetCheckErrorStr(),
							scan->bufferCount)));				
	}
	else
	{
		varBlockCheckError = VarBlockHeaderIsValid(
											scan->varBlockBuffer, 
											uncompressedLen);
		if (varBlockCheckError != VarBlockCheckOk)
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Uncompressed VarBlock is not valid (uncompressed length %d, valid block check error %d, detail '%s', block count %d)\n", 
							uncompressedLen,
							varBlockCheckError,
							VarBlockGetCheckErrorStr(),
							scan->bufferCount)));				
		}

		/*
		 * Verify length from header matches the length from uncompress.
		 */
		blockLen = VarBlockLenFromHeader(scan->varBlockBuffer,
										 uncompressedLen);
		
		if (blockLen != uncompressedLen)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Expected %d byte VarBlock length to equal %d bytes (block count %d)", 
							blockLen, uncompressedLen, scan->bufferCount)));
	}
	
	/*
	 * Now use the VarBlock module to extract the items out.
	 */
	VarBlockReaderInit(&scan->varBlockReader, 
					   (!scan->aos_compressed ? buffer : scan->varBlockBuffer), 
					   blockLen);
	
	scan->currentItemCount = 0;
	scan->readerItemCount = VarBlockReaderItemCount(&scan->varBlockReader);			

	if (Debug_appendonly_print_scan)
	{
		if(scan->aos_compressed)
			elog(LOG,"Append-only scan read uncompressed VarBlock for table '%s' (compressed length %d, length = %d, item count %d, block count %d)",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     compressedLen,
			     uncompressedLen,
			     scan->readerItemCount,
			     scan->bufferCount);
		else
		{
			elog(LOG,"Append-only scan read VarBlock for table '%s' (length = %d, item count %d, block count %d)",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     blockLen,
			     scan->readerItemCount,
			     scan->bufferCount);
		}
	}

	return true;
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

	Assert(ScanDirectionIsForward(dir));
	Assert(scan->maxBufferLen > 0 && scan->largeReadLen == scan->maxBufferLen * 2);
	Assert(scan->aos_inited);

	for(;;)
	{
		if(scan->bufferDone)
		{
			if (!getNextBlock(scan))
			{
				return NULL;			
			}

			scan->bufferDone = false;
		}	

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
				bool	valid = true;
				
				if(tuple == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_GP_INTERNAL_ERROR),
							 errmsg("Invalid tuple format in appendonlygettup")));			
				
				tuple = (MemTuple) itemPtr;
				
				if(slot)
					ExecStoreMemTuple(tuple, slot, false);
				
				/* skip visibility test, all tuples are visible */
				
				if (key != NULL)
					HeapKeyTestUsingSlot(slot, nkeys, key, valid);
				
				if (valid)
					return tuple;				
			}
			
		}
		
		/* varblock sanity check */
		if (scan->readerItemCount != scan->currentItemCount)
			elog(NOTICE, "Varblock mismatch: Reader count %d, found %d items\n",
						scan->readerItemCount,
						scan->currentItemCount);
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
appendonly_beginscan(Relation relation, int nkeys, ScanKey key)
{
	AppendOnlyScanDesc scan;
	int				   complevel;

	/*
	 * increment relation ref count while scanning relation
	 *
	 * This is just to make really sure the relcache entry won't go away while
	 * the scan has a pointer to it.  Caller should be holding the rel open
	 * anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	/*
	 * allocate and initialize scan descriptor
	 */
	scan = (AppendOnlyScanDesc) palloc0(sizeof(AppendOnlyScanDescData));
	scan->aos_inited = false;
	scan->aos_rd = relation;
	scan->aos_nkeys = nkeys;
	scan->aos_relfilenode = InvalidOid;
	scan->maxBufferLen = RelationGetBlockSize(relation, DEFAULT_VARBLOCK_SIZE);
	scan->largeReadLen = 2 * scan->maxBufferLen;
	
	complevel = RelationGetCompressLevel(relation, DEFAULT_COMPRESS_LEVEL);
	scan->aos_compressed = (complevel > 0 ? true : false);
	scan->varBlockBuffer = (uint8 *) palloc(scan->maxBufferLen * sizeof(uint8));

	/* we only need to set this up once */
	//scan->aos_ctup.t_tableOid = RelationGetRelid(relation);

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
	
	/* 
	 * close the relation file. Note that QD has no file to close and some
	 * slices don't either, so check first. 
	 */
	if(Gp_role != GP_ROLE_DISPATCH && scan->aos_file > 0)
	{
		if(close(scan->aos_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close append only file")));
	}
	
	/*
	 * In some cases we didn't get around to call BufferedReadCompleteFile
	 * because we didn't finish processing the buffer (e.g LIMIT). do it now
	 * if that's the case.
	 */
	if((&scan->bufferedRead)->file >= 0)
		BufferedReadCompleteFile(&scan->bufferedRead);
	
	BufferedReadFinish(&scan->bufferedRead);
	
	scan->aos_inited = false;

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
	
	/* 
	 * close the relation file. Note that QD has no file to close and some
	 * slices don't either, so check first. 
	 */
	if(Gp_role != GP_ROLE_DISPATCH && scan->aos_file > 0)
	{
		if(close(scan->aos_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close append only file")));
	}
			
	/*
	 * In some cases we didn't get around to call BufferedReadCompleteFile
	 * because we didn't finish processing the buffer (e.g LIMIT). do it now
	 * if that's the case.
	 */
	if((&scan->bufferedRead)->file >= 0)
		BufferedReadCompleteFile(&scan->bufferedRead);
	
	BufferedReadFinish(&scan->bufferedRead);
		
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
appendonly_insert_init(Relation rel)
{
	AppendOnlyInsertDesc	aoInsertDesc;
	uint8					*memory;
	uint8					*buffer;
	int32					actualMaxBufferLen;
	int32					memoryLen;
	int64					eof;
	int						maxtupsize;
	char					*path;
	File					appendFile;
	AppendOnlyEntry			*aoentry;

	aoInsertDesc  = (AppendOnlyInsertDesc) palloc(sizeof(AppendOnlyInsertDescData));
	aoInsertDesc->aoi_rel = rel;
	aoInsertDesc->mt_bind = create_memtuple_binding(RelationGetDescr(rel));
	aoInsertDesc->bufferCount = 0;
	aoInsertDesc->insertCount = 0;
	aoInsertDesc->varblockCount = 0;
	aoInsertDesc->maxBufferLen = RelationGetBlockSize(rel, DEFAULT_VARBLOCK_SIZE);
	aoInsertDesc->compressLevel = RelationGetCompressLevel(rel, DEFAULT_COMPRESS_LEVEL);
	aoInsertDesc->largeWriteLen = 2 * aoInsertDesc->maxBufferLen;
	aoInsertDesc->shouldCompress = (aoInsertDesc->compressLevel > 0 ? true : false);
	aoInsertDesc->varBlockBuffer = (uint8 *) palloc(aoInsertDesc->maxBufferLen * sizeof(uint8));
	aoInsertDesc->tempSpaceLen = aoInsertDesc->maxBufferLen / 8; /* TODO - come up with a more efficient calculation */
	aoInsertDesc->tempSpace = (uint8 *) palloc(aoInsertDesc->tempSpaceLen * sizeof(uint8));
	maxtupsize = aoInsertDesc->maxBufferLen - VARBLOCK_HEADER_LEN - 4;
	aoInsertDesc->toast_tuple_threshold = maxtupsize / 4; /* see tuptoaster.h for more information */
	aoInsertDesc->toast_tuple_target = maxtupsize / 4;
	
	if (!aoInsertDesc->shouldCompress)
	{
		actualMaxBufferLen = aoInsertDesc->maxBufferLen;
	}
	else
	{		
		actualMaxBufferLen = aoInsertDesc->maxBufferLen + sizeof(int32);
		actualMaxBufferLen = ((actualMaxBufferLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);
	}

	
	memoryLen = BufferedAppendMemoryLen(actualMaxBufferLen, aoInsertDesc->largeWriteLen);
	memory = (uint8*)palloc(memoryLen);
	
	if (memory == NULL)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("Cannot allocate memory %d bytes",
							   memoryLen)));
	
	/* 
	 * set up the the buffered appender for this data load 
	 */
	BufferedAppendInit(&aoInsertDesc->bufferedAppend, 
					   memory, 
					   memoryLen, 
					   actualMaxBufferLen, 
					   aoInsertDesc->largeWriteLen);
		
	/* cache the relfilenode value for later use */
	aoInsertDesc->relfilenode = rel->rd_node.relNode;
	
	/* Get path for this relation file */
	path = relpath(rel->rd_node);
	
	if (Debug_appendonly_print_insert)
		elog(LOG,"Append-only insert initialize for table '%s' with path = '%s' (compression = %s, compression level %d, maximum buffer length %d, large write length %d)",
		     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
		     path,
		     (aoInsertDesc->shouldCompress ? "true" : "false"),
		     aoInsertDesc->compressLevel,
		     aoInsertDesc->maxBufferLen,
		     aoInsertDesc->largeWriteLen);
	
	/* 
	 * Open the relation file
	 *
	 * Note: because we use BasicOpenFile(), we are responsible for ensuring
	 * the FD gets closed in any error exit path.  
	 */
	appendFile = BasicOpenFile(path, O_RDWR | PG_BINARY, 0600);
	
	if(appendFile < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\"", path)));
	
	/* get the catalog entry with information for this table */
	aoentry = GetAppendOnlyEntry(aoInsertDesc->relfilenode);

	/* Seek to the starting point (current eof from catalog) */
	eof = (int64) lseek(appendFile, (off_t) aoentry->eof, SEEK_SET);
		
	if (eof < 0)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("Cannot seek to end (error %d)", errno)));
	
	BufferedAppendSetFile(&aoInsertDesc->bufferedAppend, appendFile, eof);
	
	Assert(aoInsertDesc->tempSpaceLen > 0);
	
	if(!aoInsertDesc->shouldCompress)
	{
		buffer = BufferedAppendGetMaxBuffer(&aoInsertDesc->bufferedAppend);
	
		/* prepare our first varblock for items */
		VarBlockMakerInit(&aoInsertDesc->varBlockMaker, 
						  buffer, 
						  aoInsertDesc->maxBufferLen, 
						  aoInsertDesc->tempSpace, 
						  aoInsertDesc->tempSpaceLen);		
	}
	else
	{
		/* Block oriented compression */
		VarBlockMakerInit(&aoInsertDesc->varBlockMaker, 
						  aoInsertDesc->varBlockBuffer, 
						  aoInsertDesc->maxBufferLen, 
						  aoInsertDesc->tempSpace, 
						  aoInsertDesc->tempSpaceLen);				
	}
	aoInsertDesc->bufferCount++;
	
	pfree(aoentry);
	
	return aoInsertDesc;
}

/*
 * doPadOutPageRemainder
 *
 * Since we do not do typical recovery processing of append-only file-system 
 * pages, we pad out the last file-system page with zeroes. This function pads
 * the page with 0's of length padLen or pads the whole remainder of the page
 * with 0's if padLen is -1. 
 */
static void
doPadOutPageRemainder(AppendOnlyInsertDesc aoInsertDesc, 
	int32	padLen)
{
	int64	nextWritePosition;
	int64   nextBoundaryPosition;
	int32	pageRemainder;
	int32	bufsize = aoInsertDesc->maxBufferLen;
	bool	doPad;
	uint8   *buffer;
	
	nextWritePosition = BufferedAppendNextBufferPosition(&aoInsertDesc->bufferedAppend);
	nextBoundaryPosition = 
		((nextWritePosition + bufsize - 1)/bufsize)*bufsize;
	pageRemainder = (int32)(nextBoundaryPosition - nextWritePosition);

	if (pageRemainder == 0)
		doPad = false;
	else if (padLen == -1)
	{
		/*
		 * Pad to end of page.
		 */
		doPad = true;
		padLen = pageRemainder;
	}
	else
		doPad = (pageRemainder < padLen);

	if (doPad)
	{
		/*
		 * Get buffer of the remainder to pad.
		 */
		buffer = BufferedAppendGetBuffer(&aoInsertDesc->bufferedAppend,
										 pageRemainder);

		if (buffer == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("We do not expect files to be have a maximum length")));						
		}

		memset(buffer, 0, pageRemainder);
		BufferedAppendFinishBuffer(&aoInsertDesc->bufferedAppend, pageRemainder);

		if (Debug_appendonly_print_insert)
			elog(LOG,"Append-only insert zero padded remainder for table '%s' (nextWritePosition = " INT64_FORMAT ", pageRemainder = %d)",
			     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
			     nextWritePosition,
			     pageRemainder);

	}
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
	uint8			*buffer;
	VarBlockByteLen	 itemLen;
	uint8			*itemPtr;
	MemTuple 		 tup = NULL;
	Oid 			retOid;
	bool			need_toast;
	
	Assert(aoInsertDesc->maxBufferLen > 0 && aoInsertDesc->tempSpaceLen > 0);
    Assert(aoInsertDesc->largeWriteLen == aoInsertDesc->maxBufferLen * 2);
	Assert(aoInsertDesc->toast_tuple_threshold > 0 && aoInsertDesc->toast_tuple_target > 0);

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

	/* TODO: figure what we need this for and how to enable it if needed */
	/*	tup->t_tableOid = RelationGetRelid(relation); */

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
	itemPtr = VarBlockMakerGetNextItemPtr(&aoInsertDesc->varBlockMaker, itemLen);
	
	/*
	 * If no more room to place items in the current varblock
	 * finish it and start inserting into the next one.
	 */
	if (itemPtr == NULL)
	{		
		int32	bufferLen;
		int		actualItemCount = VarBlockMakerItemCount(&aoInsertDesc->varBlockMaker);
		
		if (actualItemCount == 0)
		{
			/* used BasicOpenFile - prevent leaking fd's. must close it */
			close((&aoInsertDesc->bufferedAppend)->file);
			
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Item too long (#1)")));			
		}
		
		bufferLen = VarBlockMakerFinish(&aoInsertDesc->varBlockMaker);	
		aoInsertDesc->varblockCount++;
		
		if(!aoInsertDesc->shouldCompress)
		{
			BufferedAppendFinishBuffer(&aoInsertDesc->bufferedAppend, bufferLen);
			
			if (Debug_appendonly_print_insert)
				elog(LOG,"Append-only insert finished uncompressed VarBlock for table '%s' (length = %d, item count %d, block count %d)",
				     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
				     bufferLen,
				     actualItemCount,
				     aoInsertDesc->bufferCount);

			doPadOutPageRemainder(aoInsertDesc, VARBLOCK_HEADER_LEN);
	
			buffer = BufferedAppendGetMaxBuffer(&aoInsertDesc->bufferedAppend);
			
			if (buffer == NULL)
			{
				/* TODO: need to clean up before aborting! */
				
				/* used BasicOpenFile - prevent leaking fd's. must close it */
				close((&aoInsertDesc->bufferedAppend)->file);

				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("We do not expect files to be have a maximum length")));						
			}
			
			VarBlockMakerInit(&aoInsertDesc->varBlockMaker,
							  buffer,
							  aoInsertDesc->maxBufferLen,
							  aoInsertDesc->tempSpace,
							  aoInsertDesc->tempSpaceLen);
		}
		else
		{
			int32 compressedDataLen;
			
			compressedDataLen = doCompressAppend(aoInsertDesc, 
												 aoInsertDesc->varBlockBuffer, 
												 bufferLen,
												 aoInsertDesc->compressLevel);
			
			VarBlockMakerInit(&aoInsertDesc->varBlockMaker,
							  aoInsertDesc->varBlockBuffer,
							  aoInsertDesc->maxBufferLen,
							  aoInsertDesc->tempSpace,
							  aoInsertDesc->tempSpaceLen);
			
			if (Debug_appendonly_print_insert)
				elog(LOG,"Append-only insert finished compressed VarBlock for table '%s' (length = %d, compressed length %d, item count %d, block count %d)",
				     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
				     bufferLen,
				     compressedDataLen,
				     actualItemCount,
				     aoInsertDesc->bufferCount);
		}
		aoInsertDesc->bufferCount++;

		itemPtr = VarBlockMakerGetNextItemPtr(&aoInsertDesc->varBlockMaker, itemLen);
		if (itemPtr == NULL)
		{
			/* used BasicOpenFile - prevent leaking fd's. must close it */
			close((&aoInsertDesc->bufferedAppend)->file);

			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Item too long (#2)")));			
		}
	}
		
	Assert(itemPtr != NULL);
	
	if (itemLen > 0)
		memcpy(itemPtr, tup, itemLen);
	
	aoInsertDesc->insertCount++;
	pgstat_count_heap_insert(&relation->pgstat_info);

	retOid = MemTupleGetOid(tup, aoInsertDesc->mt_bind);
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
	int		actualItemCount = VarBlockMakerItemCount(&aoInsertDesc->varBlockMaker);
	int64	fileLen; 
		
	/* 
	 * finish up that last varblock 
	 */
	if (actualItemCount > 0)
	{
		int32 bufferLen = VarBlockMakerFinish(&aoInsertDesc->varBlockMaker);	
		aoInsertDesc->varblockCount++;
		
		if(!aoInsertDesc->shouldCompress)
		{
			BufferedAppendFinishBuffer(&aoInsertDesc->bufferedAppend, bufferLen);
			
			if (Debug_appendonly_print_insert)
				elog(LOG,"Append-only insert finished uncompressed VarBlock for table '%s' (length = %d, item count %d, block count %d)",
				     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
				     bufferLen,
				     actualItemCount,
				     aoInsertDesc->bufferCount);
		}
		else
		{
			int32 compressedDataLen;
			
			compressedDataLen = doCompressAppend(aoInsertDesc, 
												 aoInsertDesc->varBlockBuffer, 
												 bufferLen,
												 aoInsertDesc->compressLevel);
			
			if (Debug_appendonly_print_insert)
				elog(LOG,"Append-only insert finished compressed VarBlock for table '%s' (length = %d, compressed length %d, item count %d, block count %d)",
				     NameStr(aoInsertDesc->aoi_rel->rd_rel->relname),
				     bufferLen,
				     compressedDataLen,
				     actualItemCount,
				     aoInsertDesc->bufferCount);
		}
	}

	/*
	 * We pad out append commands to the page boundary.
	 */
	doPadOutPageRemainder(aoInsertDesc,
		                  /* indicate till end of page */ -1);
	
	fileLen = BufferedAppendFileLen(&aoInsertDesc->bufferedAppend);
	
	/* update pg_appendonly with our new eof */
	UpdateAppendOnlyEntry(aoInsertDesc->relfilenode, 
						  fileLen, 
						  aoInsertDesc->insertCount,
						  aoInsertDesc->varblockCount);
	
	/* done with the rel file, close it */
	BufferedAppendCompleteFile(&aoInsertDesc->bufferedAppend);
		
	pfree(aoInsertDesc);
}

/*
 * Append data compressed with LIBZ.
 */
static int32
doCompressAppend(AppendOnlyInsertDesc aoInsertDesc,
				 uint8			*sourceData,
				 int32			 sourceLen,
				 int			 compressLevel)
{
	BufferedAppend *bufferedAppend = &aoInsertDesc->bufferedAppend; 
	uint8	*buffer;
	unsigned long compressedSize;
	int 	 res;
	int32	 compressedDataLen;
	int32	*lengthWord;
	int32	 roundUpLen;
		
	Assert(aoInsertDesc->maxBufferLen > 0 && 
		   aoInsertDesc->largeWriteLen == aoInsertDesc->maxBufferLen * 2);

	doPadOutPageRemainder(aoInsertDesc, sizeof(int32));
	
	buffer = BufferedAppendGetMaxBuffer(&aoInsertDesc->bufferedAppend);

#ifndef HAVE_LIBZ
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERNAL_ERROR),
			 errmsg("Don't have ZLIB on the system!")));			
#else
	
	/*Input to compress2. UNDONE: Create function to get this value. */
	compressedSize = (unsigned long)bufferedAppend->maxBufferLen; 
	
	res = compress2(buffer + sizeof(int32), &compressedSize, (Bytef *)sourceData, sourceLen, compressLevel);
	if (res != Z_OK)
	{
		char *detail = NULL;
		
		switch(res)
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
				elog(ERROR,"ZLIB compress2 failed -- reason %d unknown (output buffer length %d, uncompressed length %d)",
					 res, bufferedAppend->maxBufferLen, sourceLen);
				break;
		}

		elog(ERROR,"ZLIB compress2 failed (detail: '%s', output buffer length %d, uncompressed length %d)",
			 detail, bufferedAppend->maxBufferLen, sourceLen);
	}
#endif

	Assert(compressedSize > 0);
	compressedDataLen = (int32)compressedSize;
    lengthWord = (int32*)buffer;
	*lengthWord = compressedDataLen;
	roundUpLen = sizeof(int32) + ((compressedDataLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);

	BufferedAppendFinishBuffer(bufferedAppend, roundUpLen);

	return compressedDataLen;
}

static void
doDecompress(AppendOnlyScanDesc scan,
			uint8			*compressed,
			int32			compressedLen,
			int32			*uncompressedLen)
{
	int res;
	unsigned long compressedSize;
	unsigned long uncompressedSize;
	
	compressedSize = (unsigned long)compressedLen;
	uncompressedSize = (unsigned long)(scan->bufferedRead).maxBufferLen;

#ifndef HAVE_LIBZ
	*uncompressedLen = 0;
	
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERNAL_ERROR),
			 errmsg("Don't have ZLIB on the system!")));			
#else
	res = uncompress(scan->varBlockBuffer, &uncompressedSize, (Bytef *)compressed, compressedSize);

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
					 res, (scan->bufferedRead).maxBufferLen, compressedLen);
				break;
		}

		elog(ERROR,"ZLIB uncompress failed (detail: '%s', output buffer length %d, compressed length %d)",
			 detail, (scan->bufferedRead).maxBufferLen, compressedLen);
	}

	*uncompressedLen = (int32)uncompressedSize;
#endif
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


