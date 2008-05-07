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

static int32 doCompressAppend(BufferedAppend	*bufferedAppend,
							 uint8			*sourceData,
							 int32			 sourceLen,
							 int			 compressLevel);

static int32	maxBufferLen = MAX_VARBLOCK_BUFFER_LEN;
static int32    largeReadLen = 2 * MAX_VARBLOCK_BUFFER_LEN;
static int32    largeWriteLen = 2 * MAX_VARBLOCK_BUFFER_LEN;
static bool		compressed_by_default = true;

/* ----------------
 *		initscan - scan code common to appendonly_beginscan and appendonly_rescan
 *
 * In here we initialize our bufferedReader and varBlock modules and allocate
 * memory to be used for scanning our data from the relation file. We also set
 * up the scan keys and misc scan options.
 * ----------------
 */
static void
initscan(AppendOnlyScanDesc scan, ScanKey key)
{
	int32	memoryLen;
	int32	actualMaxBufferLen;

	uint8	*memory;
		
	if (!scan->aos_compressed)
	{
		actualMaxBufferLen = maxBufferLen;
	}
	else
	{		
		actualMaxBufferLen = maxBufferLen + sizeof(int32);
		actualMaxBufferLen = ((actualMaxBufferLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);
	}
	
	memoryLen = BufferedReadMemoryLen(actualMaxBufferLen, largeReadLen);
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
					 largeReadLen);

	scan->aos_inited = false;

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL)
		memcpy(scan->aos_key, key, scan->aos_nkeys * sizeof(ScanKeyData));

	pgstat_count_heap_scan(&scan->aos_pgstat_info);
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
	int32		byteLen = (!scan->aos_compressed ? sizeof(VarBlockHeader) : sizeof(int32));
	int32	   *lengthWord;
	int32		bufferLen;
	int32		roundUpLen;

	Assert(ScanDirectionIsForward(dir));
	
	if (!scan->aos_inited)
	{
		Relation	reln = scan->aos_rd;
		char	   *path;
		AppendOnlyEntry *aoentry;
		int64		eof;
							
		/* cache the relfilenode value */
		scan->aos_relfilenode = reln->rd_node.relNode;
		Assert(scan->aos_relfilenode != InvalidOid);
		
		/* Get path for this relation file */
		path = relpath(reln->rd_node);

		/* Open the relation file */
		scan->aos_file = PathNameOpenFile(path, O_RDWR | PG_BINARY, 0600);
		
		/* Get the eof of this relation file from the pg_appendonly catalog */
		aoentry = GetAppendOnlyEntry(scan->aos_relfilenode);
		eof = (int64)aoentry->eof;

		if (eof < 0)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Invalid eof %d for relation %s\n", 
							(int)eof, NameStr(scan->aos_rd->rd_rel->relname))));			
		
		/* Set the file to be read with its eof */
		BufferedReadSetFile(&scan->bufferedRead, scan->aos_file, eof);
		
		/* ready to go! */
		scan->aos_inited = true;
		
		if (Debug_appendonly_print_scan)
			elog(LOG,"Append-only scan initialize for table '%s' with path = '%s' (compression = %s, maximum buffer length %d, large read length %d)",
			     NameStr(scan->aos_rd->rd_rel->relname),
			     path,
			     (scan->aos_compressed ? "true" : "false"),
			     maxBufferLen,
			     largeReadLen);
	
		pfree(path);
		pfree(aoentry);
	}

	for(;;)
	{
		if(scan->bufferDone)
		{
			int32	availableLen;
			int32	maxralen;
			uint8  *buffer;
			int32	compressedDataLen = 0;
			int32	uncompressedDataLen = 0;

			VarBlockCheckError varBlockCheckError;

			/*
			 * Get a buffer of just the header in order to get the VarBlock length.
			 */
			buffer = BufferedReadGetNextBuffer(&scan->bufferedRead,
											   byteLen,
											   &availableLen);
			
			/* process the buffer until done */
			scan->bufferDone = false;
			
			if (buffer == NULL)
			{
				/* done reading the file */
				BufferedReadCompleteFile(&scan->bufferedRead);
				scan->aos_inited = false;	
				tuple = NULL;
				return tuple;			
			}
			
			scan->bufferCount++;
						
			if (availableLen != byteLen)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Expected %d byte length (bufferCount %d)\n", 
								byteLen, scan->bufferCount)));			
						
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
				bufferLen = VarBlockLenFromHeader(buffer, availableLen);
				
				if (bufferLen <= byteLen)
					ereport(ERROR,
							(errcode(ERRCODE_GP_INTERNAL_ERROR),
							 errmsg("Expected %d byte VarBlock length to be larger than %d header bytes (bufferCount %d)", 
									bufferLen, byteLen, scan->bufferCount)));							
				
				maxralen = bufferLen;
			}
			else
			{
				lengthWord = (int32*)buffer;
				bufferLen = *lengthWord;
				roundUpLen = sizeof(int32) + ((bufferLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);
				maxralen = roundUpLen;
			}
			
			/*
			 * Grow the amount we can see just to the VarBlock length to avoid any
			 * unnecessary copying by BufferedRead.
			 */	
			buffer = BufferedReadGrowBuffer(&scan->bufferedRead,
											maxralen,
											&availableLen);

			if (maxralen != availableLen)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Wrong available length (expected %d byte length buffer and got %d (bufferCount %d)",
								maxralen, availableLen, scan->bufferCount)));			

			
			/* 
			 * actually do the decompression now.
			 */
			if(scan->aos_compressed)
			{	
				int res;
				unsigned long compressedSize;
				unsigned long uncompressedSize;
				
				compressedSize = (unsigned long)bufferLen;
				uncompressedSize = (unsigned long)(scan->bufferedRead).maxBufferLen;

#ifndef HAVE_LIBZ
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Don't have ZLIB on the system!")));			
#else
				res = uncompress(scan->varBlockBuffer, &uncompressedSize, (Bytef *)(buffer + sizeof(int32)), compressedSize);

				if (res != Z_OK)
				{
					char *detail;
					
					switch(res)
					{
						case Z_MEM_ERROR:
							detail = "not enough memory";
							break;
						case Z_BUF_ERROR:
							detail = "not enough room in the output buffer";
							break;
						case Z_DATA_ERROR:
							detail = "data was corrupted";
							break;
						default:
							detail = "reason unknown";
					}

					elog(ERROR,"ZLIB uncompress failed (detail: '%s', output buffer length %d, compressed length %d)",
						 detail, (scan->bufferedRead).maxBufferLen, bufferLen);
				}

				compressedDataLen = (int32)compressedSize;
				uncompressedDataLen = (int32)uncompressedSize;
#endif
			}
			
			
#ifdef DEBUG
			/*
			 * In DEBUG, check whole VarBlock.
			 */
			if(!scan->aos_compressed)
			{
				varBlockCheckError = VarBlockIsValid(buffer, availableLen);
				if (varBlockCheckError != VarBlockCheckOk)
					ereport(ERROR,
							(errcode(ERRCODE_GP_INTERNAL_ERROR),
							 errmsg("VarBlock  is not valid (bufferCount %d, detail '%s')\n", 
									scan->bufferCount,
									VarBlockGetCheckErrorStr())));				
			}
			else
			{
				varBlockCheckError = VarBlockIsValid(scan->varBlockBuffer,
												     uncompressedDataLen);
				if (varBlockCheckError != VarBlockCheckOk)
					ereport(ERROR,
							(errcode(ERRCODE_GP_INTERNAL_ERROR),
							 errmsg("VarBlock  is not valid (bufferCount %d, detail '%s')\n", 
									scan->bufferCount,
									VarBlockGetCheckErrorStr())));				
				
			}
#endif

			if(scan->aos_compressed)
			{
				/*
				 * Ask the VarBlock module to use the header to how large the whole
				 * VarBlock is.
				 */
				bufferLen = VarBlockLenFromHeader(scan->varBlockBuffer,
												  uncompressedDataLen);
				
				if (bufferLen != uncompressedDataLen)
					ereport(ERROR,
							(errcode(ERRCODE_GP_INTERNAL_ERROR),
							 errmsg("Expected %d byte VarBlock length to equal %d bytes (bufferCount %d)", 
									bufferLen, uncompressedDataLen, scan->bufferCount)));
			}
			
			/*
			 * Now use the VarBlock module to extract the lines out.
			 */
			VarBlockReaderInit(&scan->varBlockReader, 
							   (!scan->aos_compressed ? buffer : scan->varBlockBuffer), 
							   bufferLen);
			
			scan->currentItemCount = 0;
			scan->readerItemCount = VarBlockReaderItemCount(&scan->varBlockReader);			

			if (Debug_appendonly_print_scan)
			{
				if(scan->aos_compressed)
					elog(LOG,"Append-only scan read compressed VarBlock for table '%s' (compressed length %d, length = %d, item count %d, block count %d)",
					     NameStr(scan->aos_rd->rd_rel->relname),
					     compressedDataLen,
					     uncompressedDataLen,
					     scan->readerItemCount,
					     scan->bufferCount);
				else
				{
					elog(LOG,"Append-only scan read uncompressed VarBlock for table '%s' (length = %d, item count %d, block count %d)",
					     NameStr(scan->aos_rd->rd_rel->relname),
					     bufferLen,
					     scan->readerItemCount,
					     scan->bufferCount);
				}
			}
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

	scan->aos_rd = relation;
	scan->aos_nkeys = nkeys;
	scan->aos_relfilenode = InvalidOid;
	scan->aos_compressed = RelationUsesCopmression(relation, 
												   compressed_by_default);

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
 * ----------------
 */
void
appendonly_rescan(AppendOnlyScanDesc scan,
				  ScanKey key)
{

	/* TODO... implement rescan */
	ereport(ERROR,
			(errcode(ERRCODE_GP_FEATURE_NOT_YET),
			 errmsg("cannot rescan append only table yet")));						
		
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
		FileClose(scan->aos_file);
			
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
	char					*path;
	File					appendFile;
	AppendOnlyEntry			*aoentry;
	
	aoInsertDesc  = (AppendOnlyInsertDesc) palloc(sizeof(AppendOnlyInsertDescData));
	aoInsertDesc->aoi_rel = rel;
	aoInsertDesc->mt_bind = create_memtuple_binding(RelationGetDescr(rel));
	aoInsertDesc->bufferCount = 0;
	aoInsertDesc->insertCount = 0;
	aoInsertDesc->shouldCompress = RelationUsesCopmression(rel, 
														   compressed_by_default);
	if(aoInsertDesc->shouldCompress)
		aoInsertDesc->compressLevel = RelationGetCompressLevel(rel, 3 /* default */);
	
	if (!aoInsertDesc->shouldCompress)
	{
		actualMaxBufferLen = maxBufferLen;
	}
	else
	{		
		actualMaxBufferLen = maxBufferLen + sizeof(int32);
		actualMaxBufferLen = ((actualMaxBufferLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);
	}

	
	memoryLen = BufferedAppendMemoryLen(actualMaxBufferLen, largeWriteLen);
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
					   largeWriteLen);
		
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
		     maxBufferLen,
		     largeWriteLen);
	
	/* Open the relation file */
	/* TODO: this should really be done in a cleaner way via smgr */
	appendFile = PathNameOpenFile(path, O_RDWR | PG_BINARY, 0600);

	/* get the catalog entry with information for this table */
	aoentry = GetAppendOnlyEntry(aoInsertDesc->relfilenode);

	/* Seek to the starting point (current eof from catalog) */
	eof = FileSeek(appendFile, (off_t) aoentry->eof, SEEK_SET);
		
	if (eof < 0)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("Cannot seek to end (error %d)", errno)));
	
	BufferedAppendSetFile(&aoInsertDesc->bufferedAppend, appendFile, eof);
	
	if(!aoInsertDesc->shouldCompress)
	{
		buffer = BufferedAppendGetMaxBuffer(&aoInsertDesc->bufferedAppend);
		
		/* prepare our first varblock for items */
		VarBlockMakerInit(&aoInsertDesc->varBlockMaker, 
						  buffer, 
						  maxBufferLen, 
						  aoInsertDesc->tempSpace, 
						  MAX_VARBLOCK_TEMPSPACE_LEN);		
	}
	else
	{
		/* Block oriented compression */
		VarBlockMakerInit(&aoInsertDesc->varBlockMaker, 
						  aoInsertDesc->varBlockBuffer, 
						  maxBufferLen, 
						  aoInsertDesc->tempSpace, 
						  MAX_VARBLOCK_TEMPSPACE_LEN);				
	}
	aoInsertDesc->bufferCount++;
	
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
appendonly_insert(AppendOnlyInsertDesc aoInsertDesc, MemTuple tup)
{
	Relation		 relation = aoInsertDesc->aoi_rel;
	uint8			*buffer;
	VarBlockByteLen	 itemLen;
	uint8			*itemPtr;

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
		if (!OidIsValid(MemTupleGetOid(tup, aoInsertDesc->mt_bind)))
			MemTupleSetOid(tup, aoInsertDesc->mt_bind, GetNewOid(relation));
	}
	else
	{
		/* check there is not space for an OID */
		MemTupleNoOidSpace(tup);
	}

//	tup->t_tableOid = RelationGetRelid(relation);

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
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Item too long (#1)")));
		
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
	
			buffer = BufferedAppendGetMaxBuffer(&aoInsertDesc->bufferedAppend);
			
			if (buffer == NULL)
			{
				/* TODO: need to clean up before aborting! */
				
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Item too long (#2)")));						
			}
			
			VarBlockMakerInit(&aoInsertDesc->varBlockMaker,
							  buffer,
							  maxBufferLen,
							  aoInsertDesc->tempSpace,
							  MAX_VARBLOCK_TEMPSPACE_LEN);
		}
		else
		{
			int32 compressedDataLen;
			
			compressedDataLen = doCompressAppend(&aoInsertDesc->bufferedAppend, 
												 aoInsertDesc->varBlockBuffer, 
												 bufferLen,
												 aoInsertDesc->compressLevel);
			
			VarBlockMakerInit(&aoInsertDesc->varBlockMaker,
							  aoInsertDesc->varBlockBuffer,
							  maxBufferLen,
							  aoInsertDesc->tempSpace,
							  MAX_VARBLOCK_TEMPSPACE_LEN);
			
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
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERNAL_ERROR),
					 errmsg("Item too long (#2)")));
	}
		
	Assert(itemPtr != NULL);
	
	if (itemLen > 0)
		memcpy(itemPtr, tup, itemLen);
	
	aoInsertDesc->insertCount++;
	pgstat_count_heap_insert(&relation->pgstat_info);

	return MemTupleGetOid(tup, aoInsertDesc->mt_bind);
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
			
			compressedDataLen = doCompressAppend(&aoInsertDesc->bufferedAppend, 
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
 * doCompressAppend
 *
 * append data compressed with LIBZ.
 */
static int32
doCompressAppend(BufferedAppend	*bufferedAppend,
				 uint8			*sourceData,
				 int32			 sourceLen,
				 int			 compressLevel)
{
	uint8	*buffer;
	unsigned long compressedSize;
	int32	 compressedDataLen;
	int32	*lengthWord;
	int32	 roundUpLen;
		
	buffer = BufferedAppendGetMaxBuffer(bufferedAppend);

#ifndef HAVE_LIBZ
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERNAL_ERROR),
			 errmsg("Don't have ZLIB on the system!")));			
#else
	
	compressedSize = (unsigned long)bufferedAppend->maxBufferLen; /*Input to compress2. UNDONE: Create function to get this value. */
	
	if (compress2(buffer + sizeof(int32), &compressedSize, (Bytef *)sourceData, sourceLen, compressLevel) != Z_OK)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Compression failed")));			
#endif

	Assert(compressedSize > 0);
	compressedDataLen = (int32)compressedSize;
    lengthWord = (int32*)buffer;
	*lengthWord = compressedDataLen;
	roundUpLen = sizeof(int32) + ((compressedDataLen + sizeof(int32) - 1)/sizeof(int32))*sizeof(int32);

	BufferedAppendFinishBuffer(bufferedAppend, roundUpLen);

	return compressedDataLen;
}

/*
 * AORelationGetNumberOfBlocks 
 *
 * Has the same meaning as RelationGetNumberOfBlocks for heap relations
 * however uses an estimation since AO relations use variable len blocks
 * which are meaningless to the optimizer. 
 *
 * This function is, in other words answers the following question - "If 
 * I were a heap relation, how many blocks would I have had?"
 */
BlockNumber 
RelationGuessNumberOfBlocks(double totalbytes)
{
	/* for now it's very simple */
	return (BlockNumber)(totalbytes/BLCKSZ) + 1;
}


