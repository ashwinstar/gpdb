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
			   FileRepSubProcess_GetState() != FileRepStateShutdownBackends &&
			   FileRepSubProcess_GetState() != FileRepStateImmediateShutdown &&
			   ! (FileRepSubProcess_GetState() == FileRepStateReady && 
			    dataState == DataStateInResync)) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateShutdownBackends ||
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
 * The following relations will be always resynced as scan incremental
 *		OIDs {5090, 5091, 5092, 5093, and 5096}
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
	int				count = 0;
	int				thresholdCount = 0;
	bool			mirrorDataLossOccurred = FALSE;
	
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
#ifdef FILEREP_DEBUG					
					elog(LOG, "FileRepPrimary_ResyncWrite() relfilenode:'%u/%u/%u' "
							  "numBlocks:'%d' ",
						 smgr_relation->smgr_rnode.spcNode,
						 smgr_relation->smgr_rnode.dbNode,
						 smgr_relation->smgr_rnode.relNode,
						 numBlocks);					
#endif					
					thresholdCount = Min(numBlocks, 1024);
					for (blkno = entry->mirrorBufpoolResyncCkptBlockNum; blkno < numBlocks; blkno++) 
					{
						XLogRecPtr	endResyncLSN = (isFullResync() ? 
													FileRepResync_GetEndFullResyncLSN() :
													FileRepResync_GetEndIncrResyncLSN());
						
						FileRepResync_SetReadBufferRequest();
						buf = ReadBuffer_ResyncEx(smgr_relation, 
										 blkno);
						FileRepResync_ResetReadBufferRequest();
						
						LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
						page = BufferGetPage(buf);
						
						loc = PageGetLSN(page);
#ifdef FILEREP_DEBUG						
						elog(LOG, "FileRepPrimary_ResyncWrite() blkno:'%d' "
							 "lsn_begin:'%u'/'%u' "
							 "lsn_page:'%u'/'%u' "
							 "lsn_end:'%u'/'%u' ",
							 blkno,
							 entry->mirrorBufpoolResyncCkptLoc.xlogid,
							 entry->mirrorBufpoolResyncCkptLoc.xrecoff,
							 loc.xlogid,
							 loc.xrecoff,
							 endResyncLSN.xlogid,
							 endResyncLSN.xrecoff);
#endif						
						if (XLByteLE(PageGetLSN(page), endResyncLSN) &&
							XLByteLE(entry->mirrorBufpoolResyncCkptLoc, PageGetLSN(page))) 
						{
							smgrwrite(smgr_relation, 
									  blkno,
									  (char *)BufferGetBlock(buf),
									  FALSE);
						}
							
						UnlockReleaseBuffer(buf);
						
						if (count > thresholdCount)
						{
							count = 0;
							FileRepSubProcess_ProcessSignals();
							
							if (! (FileRepSubProcess_GetState() == FileRepStateReady && 
								   dataState == DataStateInResync))
							{
								mirrorDataLossOccurred = TRUE;
								break;
							}
						}
						else
							count++;
					}
						
					if (mirrorDataLossOccurred)
						break;
					
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
					
					/* allow flushing buffers from buffer pool during scan */
					FileRepResync_SetReadBufferRequest();
					if ((result = ChangeTracking_GetChanges(request)) != NULL) 
					{
						FileRepResync_ResetReadBufferRequest();

#ifdef FILEREP_DEBUG
						elog(LOG, "FileRepPrimary_ResyncWrite() relfilenode:'%u/%u/%u' ",
							 result->entries[0].relFileNode.spcNode,
							 result->entries[0].relFileNode.dbNode,
							 result->entries[0].relFileNode.relNode);
#endif
						if (smgr_relation == NULL)
						{
							smgr_relation = smgropen(result->entries[0].relFileNode);
							
							smgr_relation->smgr_rnode.relNode = result->entries[0].relFileNode.relNode;
							smgr_relation->smgr_rnode.spcNode = result->entries[0].relFileNode.spcNode;
							smgr_relation->smgr_rnode.dbNode = result->entries[0].relFileNode.dbNode;
						}
						
						numBlocks = smgrnblocks(smgr_relation);
						if (Debug_filerep_print)
							elog(LOG, "FileRepPrimary_ResyncWrite() relfilenode:'%u/%u/%u' "
									  "numBlocks:'%d' ",
									 smgr_relation->smgr_rnode.spcNode,
									 smgr_relation->smgr_rnode.dbNode,
									 smgr_relation->smgr_rnode.relNode,
									 numBlocks);
					
						thresholdCount = Min(numBlocks, 1024);

						for (ii = 0; ii < result->count; ii++)
						{
							loc1 =  result->entries[ii].lsn_end;
							
							/*
							 * if relation was truncated then block_num from change tracking can be beyond numBlocks 
							 */
							if (result->entries[ii].block_num >=  numBlocks)
							{
								ereport(LOG, 
										(errmsg("WARNING(filerep): did not resync (maybe due to truncate) "
												"relfilenode:'%u/%u/%u' numBlocks:'%d' " 
												"blkno:''%d' "
												"lsn_begin:'%u'/'%u' "
												"lsn_end:'%u'/'%u' '%s' ",
												smgr_relation->smgr_rnode.spcNode,
												smgr_relation->smgr_rnode.dbNode,
												smgr_relation->smgr_rnode.relNode,
												numBlocks,
												result->entries[ii].block_num,
												entry->mirrorBufpoolResyncCkptLoc.xlogid,
												entry->mirrorBufpoolResyncCkptLoc.xrecoff,
												loc1.xlogid,
												loc1.xrecoff,
												XLogLocationToString(&loc1))));
								
								continue;
							}
							
							/* allow flushing buffers from buffer pool during scan */
							FileRepResync_SetReadBufferRequest();
							buf = ReadBuffer_ResyncEx(smgr_relation, 
											 result->entries[ii].block_num);
							FileRepResync_ResetReadBufferRequest();
							
							Assert(result->entries[ii].block_num < numBlocks);
							
							LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
							page = BufferGetPage(buf);
							
							loc = PageGetLSN(page); 
							
							if (Debug_filerep_print)
								elog(LOG, "FileRepPrimary_ResyncWrite() blkno:'%d' "
									 "lsn_begin:'%u'/'%u' "
									 "lsn_page:'%u'/'%u' '%s' "
									 "lsn_end:'%u'/'%u' '%s' ",
									 result->entries[ii].block_num,
									 entry->mirrorBufpoolResyncCkptLoc.xlogid,
									 entry->mirrorBufpoolResyncCkptLoc.xrecoff,
									 loc.xlogid,
									 loc.xrecoff,
									 XLogLocationToString(&loc),
									 loc1.xlogid,
									 loc1.xrecoff,
									 XLogLocationToString(&loc1));

							if (XLByteEQ(PageGetLSN(page), result->entries[ii].lsn_end)) 	
							{
			
								smgrwrite(smgr_relation, 
										  result->entries[ii].block_num,
										  (char *)BufferGetBlock(buf),
										  FALSE);
								
							}
							else
							{
								ereport(LOG, 
										(errmsg("WARNING(filerep): did not resync "
												"relfilenode:'%u/%u/%u' numBlocks:'%d' " 
												"blkno:''%d' "
												"lsn_begin:'%u'/'%u' "
												"lsn_page:'%u'/'%u' '%s' "
												"lsn_end:'%u'/'%u' '%s' ",
												smgr_relation->smgr_rnode.spcNode,
												smgr_relation->smgr_rnode.dbNode,
												smgr_relation->smgr_rnode.relNode,
												numBlocks,
												result->entries[ii].block_num,
												entry->mirrorBufpoolResyncCkptLoc.xlogid,
												entry->mirrorBufpoolResyncCkptLoc.xrecoff,
												loc.xlogid,
												loc.xrecoff,
												XLogLocationToString(&loc),
												loc1.xlogid,
												loc1.xrecoff,
												XLogLocationToString(&loc1))));
								
								Assert(XLByteLT(result->entries[ii].lsn_end, PageGetLSN(page)));
								
							}
								
							UnlockReleaseBuffer(buf); 
							
							if (count > thresholdCount)
							{
								count = 0;
								FileRepSubProcess_ProcessSignals();
								
								if (! (FileRepSubProcess_GetState() == FileRepStateReady && 
									   dataState == DataStateInResync))
								{
									mirrorDataLossOccurred = TRUE;
									break;
								}
							}
							else
								count++;
						}
												   
					}
					
					FileRepResync_ResetReadBufferRequest();

					if (smgr_relation != NULL) 
					{
						if (! mirrorDataLossOccurred)
						{
							smgrimmedsync(smgr_relation);
									
							smgrclose(smgr_relation);
						}
					}
					else 
					{
						ereport(LOG, 
								(errmsg("LOG(filerep): no blocks were resynced "
										"relfilenode:'%u/%u/%u' "
										"SynchronizationState:'%s' ",
										 entry->relFileNode.relNode,
										 entry->relFileNode.spcNode,
										 entry->relFileNode.dbNode,
										 MirroredRelDataSynchronizationState_Name(entry->mirrorDataSynchronizationState))));
						
					}
					smgr_relation = NULL;
					
					ChangeTracking_FreeRequest(request);
					ChangeTracking_FreeResult(result);
					break;
					
				case MirroredRelDataSynchronizationState_Synchronized:
					break;
				default:
					elog(LOG, "FileRepPrimary_ResyncWrite() SynchronizationState:'%s(%d)' ",
							   MirroredRelDataSynchronizationState_Name(entry->mirrorDataSynchronizationState),
							   entry->mirrorDataSynchronizationState);					
					Assert(0);
					break;
			}
			break;
			
		case PersistentFileSysRelStorageMgr_AppendOnly:
		{
			MirroredAppendOnlyOpen	mirroredOpen;
			int						primaryError;
			bool					mirrorDataLossOccurred;
			char					*buffer = NULL;
			int64					endOffset = entry->mirrorAppendOnlyNewEof;
			int64					startOffset = entry->mirrorAppendOnlyLossEof;
			int32					bufferLen = 0;
			int						retval = 0;
			
			switch (entry->mirrorDataSynchronizationState)
			{
				case MirroredRelDataSynchronizationState_AppendOnlyCatchup:
				case MirroredRelDataSynchronizationState_FullCopy:
					/*
					 * The MirroredAppendOnly_OpenResynchonize routine knows we are a resynch worker and
					 * will open BOTH, but write only the MIRROR!!!
					 */
					MirroredAppendOnly_OpenResynchonize(
											&mirroredOpen, 
											&entry->relFileNode,
											entry->segmentFileNum,
											startOffset,
											&primaryError,
											&mirrorDataLossOccurred);
					if (primaryError != 0)
					{
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not open file %u/%u/%u.%u : %s",
										entry->relFileNode.dbNode,
										entry->relFileNode.spcNode,
										entry->relFileNode.relNode,
										entry->segmentFileNum,
										strerror(primaryError))));
						
						break;
					}

					if (mirrorDataLossOccurred)
						break;
					
					/* AO and CO Data Store writes 64k size by default */
					bufferLen = (Size) Min(2*BLCKSZ, endOffset - startOffset);
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
									 errmsg("could not read from position:" INT64_FORMAT " in file %u/%u/%u.%u : %m",
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
											  bufferLen,
											  &primaryError,
											  &mirrorDataLossOccurred);
						
						if (mirrorDataLossOccurred)
							break;

						Assert(primaryError == 0);	// No primary writes as resync worker.
						
						startOffset += bufferLen;
						/* AO and CO Data Store writes 64k size by default */
						bufferLen = (Size) Min(2*BLCKSZ, endOffset - startOffset);						
					}
					
					if (buffer) 
					{
						pfree(buffer);
						buffer = NULL;
					}
					
					if (mirrorDataLossOccurred)
						break;
					
					/* Flush written data on Mirror */
					MirroredAppendOnly_Flush(
										&mirroredOpen,
										&primaryError,
										&mirrorDataLossOccurred);
					if (mirrorDataLossOccurred)
						break;
					
					Assert(primaryError == 0);	// Not flushed on primary as resync worker.
					
					/* Close Primary and Mirror */
					MirroredAppendOnly_Close(
										&mirroredOpen,
										&mirrorDataLossOccurred);
								
					break;
					
				default:
					 elog(LOG, "FileRepPrimary_ResyncWrite() SynchronizationState:'%s(%d)' ",
						  MirroredRelDataSynchronizationState_Name(entry->mirrorDataSynchronizationState),
						  entry->mirrorDataSynchronizationState);					
					 Assert(0);
					break;
			}
			
			break;
		}	//case
		default:
			Assert(0);
			break;
	} //switch
	
	if (mirrorDataLossOccurred)
		status = STATUS_ERROR;
	
	return status;
}
