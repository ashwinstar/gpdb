/*
 *  cdbfilerepprimaryresync.c  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved.
 *
 */

#include "postgres.h"

#include <signal.h>
#include <fcntl.h>

#include "cdb/cdbvars.h"
#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepprimaryrecovery.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbfilerepresyncworker.h"
#include "cdb/cdbmirroredappendonly.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/relcache.h"

static int FileRepPrimary_RunResyncWorker(void);
static int FileRepPrimary_ResyncWrite(FileRepResyncHashEntry_s	*entry);


/*
 * FileRepPrimary_StartResyncWorker()
 *
 */
void 
FileRepPrimary_StartResyncWorker(void)
{	
	int	status = STATUS_OK;
	
	elog(LOG, "FileRepPrimary_StartResyncWorker()");
	
	Insist(fileRepRole == FileRepPrimaryRole);
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() != FileRepStateShutdown &&
			   FileRepSubProcess_GetState() != FileRepStateImmediateShutdown &&
			   ! (FileRepSubProcess_GetState() == FileRepStateReady && 
			    dataState == DataStateInResync)) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
			
			break;
		}
				
		status = FileRepPrimary_RunResyncWorker();
		
		if (status != STATUS_OK) {
			continue;
		}
		
		break;
		
	} // while(1)	
		
}

/*
 * FileRepPrimary_RunResyncWorker()
 *
 */
static int
FileRepPrimary_RunResyncWorker(void)
{
	int							status = STATUS_OK;
	FileRepResyncHashEntry_s	*entry = NULL;
	
	
	elog(LOG, "FileRepPrimary_RunResyncWorker()");
	
	
	while (1) {

		FileRepSubProcess_ProcessSignals();
		
		if (! (FileRepSubProcess_GetState() == FileRepStateReady && 
			   dataState == DataStateInResync))
		{
			break;
		}

		entry = FileRepPrimary_GetResyncEntry();
				
		if (entry == NULL) {
			
			pg_usleep(100000L); /* 100 ms */
			continue;
		}

		status = FileRepPrimary_ResyncWrite(entry);
		
		if (status == STATUS_OK)
		{
			entry->fileRepResyncState = FileRepResyncStateCompleted;
		}
		else
		{
			entry->fileRepResyncState = FileRepResyncStateFault;
		}
		
		status = FileRepResync_UpdateEntry(entry);
		
		if (status != STATUS_OK)
		{
			break;
		}
		
	}
	
	return status;
}

/*
 *
 * RESYNC relation (Buffer Pool)
 *		INPUT parameters
 *				*) RelFileNode
 *				*) beginLSN (the earliest LSN to be re-synchronized) 
 *				*) endLSN (the latest LSN to be re-synchronized)
 *				*) endBlockNumber (the latest block in relation to be re-synchronized)
 *				*) 
 *
 *
 *	RESYNC relation (Append Only)
 *		INPUT parameters
 *				*) RelFileNode
 *				*)
 *
 *
 */

static int
FileRepPrimary_ResyncWrite(FileRepResyncHashEntry_s	*entry)
{

	int				status = STATUS_OK;
	Page			page;
	Buffer			buf; 
	BlockNumber		numBlocks;
	BlockNumber		blkno;
	SMgrRelation	smgr_relation = NULL;
	int				NumberOfRelations = 1;	
	int				ii;
	XLogRecPtr		loc;
	XLogRecPtr		loc1;
	
	ChangeTrackingRequest*	request = NULL;
	ChangeTrackingResult*	result = NULL;
	
	switch (entry->relStorageMgr)
	{

		case PersistentFileSysRelStorageMgr_BufferPool:
			
			switch (entry->mirrorDataSynchronizationState)
			{
				case MirroredRelDataSynchronizationState_BufferPoolScanIncremental:
				case MirroredRelDataSynchronizationState_FullCopy:

					if (smgr_relation == NULL) 
					{
						smgr_relation = smgropen(entry->relFileNode);
						
						smgr_relation->smgr_rnode.relNode = entry->relFileNode.relNode;
						smgr_relation->smgr_rnode.spcNode = entry->relFileNode.spcNode;
						smgr_relation->smgr_rnode.dbNode = entry->relFileNode.dbNode;
					}
					
					numBlocks = smgrnblocks(smgr_relation);
					
					elog(LOG, "FileRepPrimary_ResyncWrite() relNode:'%d' spcNode:'%d' dbNode:'%d' "
							  "numBlocks:'%d' ",
						 smgr_relation->smgr_rnode.relNode,
						 smgr_relation->smgr_rnode.spcNode,
						 smgr_relation->smgr_rnode.dbNode,
						 numBlocks);					
					
					for (blkno = entry->mirrorBufpoolResyncCkptBlockNum; blkno < numBlocks; blkno++) 
					{
						XLogRecPtr	endResyncLSN = (isFullResync() ? 
													FileRepResync_GetEndFullResyncLSN() :
													FileRepResync_GetEndIncrResyncLSN());
						
						buf = ReadBuffer_ResyncEx(smgr_relation, 
										 blkno);
						
						LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
						page = BufferGetPage(buf);
						
						loc = PageGetLSN(page);
						
						elog(LOG, "FileRepPrimary_ResyncWrite() blkno:'%d' "
							 "lsn_begin:'%d'/'%d' "
							 "lsn_page:'%d'/'%d' "
							 "lsn_end:'%d'/'%d' ",
							 blkno,
							 entry->mirrorBufpoolResyncCkptLoc.xlogid,
							 entry->mirrorBufpoolResyncCkptLoc.xrecoff,
							 loc.xlogid,
							 loc.xrecoff,
							 endResyncLSN.xlogid,
							 endResyncLSN.xrecoff);
						
						if (XLByteLE(PageGetLSN(page), endResyncLSN) &&
							XLByteLE(entry->mirrorBufpoolResyncCkptLoc, PageGetLSN(page))) 
						{
							smgrwrite(smgr_relation, 
									  blkno,
									  (char *)BufferGetBlock(buf),
									  FALSE);
						}
							
						UnlockReleaseBuffer(buf);
					}
							
					smgrimmedsync(smgr_relation);
					
					smgrclose(smgr_relation);
					
					smgr_relation = NULL;
					break;
					
				case MirroredRelDataSynchronizationState_BufferPoolPageIncremental:
					
					request = ChangeTracking_FormRequest(NumberOfRelations);				
				
					for (ii = 0; ii < NumberOfRelations; ii++) 
					{
						XLogRecPtr	endResyncLSN = FileRepResync_GetEndIncrResyncLSN();
						
						ChangeTracking_AddRequestEntry(request, 
													   entry->relFileNode,
													   &entry->mirrorBufpoolResyncCkptLoc,   //beginIncrResyncLSN,
													   &endResyncLSN);
					}
				
					if ((result = ChangeTracking_GetChanges(request)) != NULL) 
					{
						elog(LOG, "FileRepPrimary_ResyncWrite() relNode:'%d' spcNode:'%d' dbNode:'%d' ",
							 result->entries[0].relFileNode.relNode,
							 result->entries[0].relFileNode.spcNode,
							 result->entries[0].relFileNode.dbNode);

						if (smgr_relation == NULL)
						{
							smgr_relation = smgropen(result->entries[0].relFileNode);
							
							smgr_relation->smgr_rnode.relNode = result->entries[0].relFileNode.relNode;
							smgr_relation->smgr_rnode.spcNode = result->entries[0].relFileNode.spcNode;
							smgr_relation->smgr_rnode.dbNode = result->entries[0].relFileNode.dbNode;
						}
						
						numBlocks = smgrnblocks(smgr_relation);
								
						elog(LOG, "FileRepPrimary_ResyncWrite() relNode:'%d' spcNode:'%d' dbNode:'%d' ",
							 smgr_relation->smgr_rnode.relNode,
							 smgr_relation->smgr_rnode.spcNode,
							 smgr_relation->smgr_rnode.dbNode);
						
						for (ii = 0; ii < result->count; ii++)
						{
						
							buf = ReadBuffer_ResyncEx(smgr_relation, 
											 result->entries[ii].block_num);
							
							LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
							page = BufferGetPage(buf);
							
							loc = PageGetLSN(page); 
							loc1 =  result->entries[ii].lsn_end;
							
							elog(LOG, "FileRepPrimary_ResyncWrite() blkno:'%d' "
								 "lsn_begin:'%d'/'%d' "
								 "lsn_page:'%d'/'%d' "
								 "lsn_end:'%d'/'%d' ",
								 result->entries[ii].block_num,
								 entry->mirrorBufpoolResyncCkptLoc.xlogid,
								 entry->mirrorBufpoolResyncCkptLoc.xrecoff,
								 loc.xlogid,
								 loc.xrecoff,
								 loc1.xlogid,
								 loc1.xrecoff);

							if (XLByteEQ(PageGetLSN(page), result->entries[ii].lsn_end)) 
							{
			
								smgrwrite(smgr_relation, 
										  result->entries[ii].block_num,
										  (char *)BufferGetBlock(buf),
										  FALSE);
								
							}
							else
							{
								Assert(XLByteLT(FileRepResync_GetEndIncrResyncLSN(), PageGetLSN(page)));
							}
								
							UnlockReleaseBuffer(buf); 
						}
												   
					}
					
					smgrimmedsync(smgr_relation);
							
					smgrclose(smgr_relation);
					
					smgr_relation = NULL;
					
					ChangeTracking_FreeRequest(request);
					ChangeTracking_FreeResult(result);
					break;
					
				case MirroredRelDataSynchronizationState_Synchronized:
					break;
				default:
					elog(LOG, "FileRepPrimary_ResyncWrite() SynchronizationState:'%d' ",
								entry->mirrorDataSynchronizationState);					
					//Assert(0);
					break;
			}
			break;
			
		case PersistentFileSysRelStorageMgr_AppendOnly:
		{
			MirroredAppendOnlyOpen	mirroredOpen;
			char					*buffer = NULL;
			int32					endOffset = entry->mirrorAppendOnlyEof;
			int32					startOffset = 0;
			int32					bufferLen = 0;
			int						retval = 0;
			
			switch (entry->mirrorDataSynchronizationState)
			{
				
				case MirroredRelDataSynchronizationState_FullCopy:
					errno = 0;
					
					MirroredAppendOnly_Open(&mirroredOpen, 
											&entry->relFileNode,
											entry->segmentFileNum,
											/* relationName */ NULL,		// Ok to be NULL -- we don't know the name here.
											startOffset,
											O_RDWR | PG_BINARY,
											0600);
					
					if (! MirroredAppendOnly_IsActive(&mirroredOpen))
						break;
					
					bufferLen = (Size) Min(BLCKSZ, endOffset - startOffset);
					buffer = (char*) palloc(bufferLen);
					if (buffer == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 (errmsg("Not enough memory for Resynchronization."))));
					
					MemSet(buffer, 0, bufferLen);
					
					while (startOffset < endOffset)
					{
						retval = MirroredAppendOnly_Read(
												&mirroredOpen,
												buffer,
												bufferLen);
						
						if (retval != bufferLen) 
						{
							ereport(ERROR,
									(errcode_for_file_access(),
									 errmsg("could not read from position:%d in file %d/%d/%d.%d : %m",
											startOffset, 
											entry->relFileNode.dbNode,
											entry->relFileNode.spcNode,
											entry->relFileNode.relNode,
											entry->segmentFileNum)));
							
							break;
						}						
						
						MirroredAppendOnly_Append(
												  &mirroredOpen,
												  buffer,
												  bufferLen);
						
						startOffset += bufferLen;
						
						bufferLen = (Size) Min(BLCKSZ, endOffset - startOffset);						
					}
					
					/* Flush written data on Mirror */
					MirroredAppendOnly_Flush(&mirroredOpen);
					
					/* Close Primary and Mirror */
					MirroredAppendOnly_Close(&mirroredOpen);
					
					if (buffer) 
					{
						pfree(buffer);
						buffer = NULL;
					}
			
					break;
				case MirroredRelDataSynchronizationState_AppendOnlyCatchup:
					
					break;
					
				default:
					Assert(0);
					break;
			}
			
			break;
		}	//case
		default:
			Assert(0);
			break;
	} //switch
	
	
	return status;
}
