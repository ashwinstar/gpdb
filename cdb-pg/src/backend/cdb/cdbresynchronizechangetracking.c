/*-------------------------------------------------------------------------
 *
 * cdbresynchronizechangetracking.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <sys/stat.h>

#include "postgres.h"
#include "miscadmin.h"

#include "access/htup.h"
#include "access/nbtree.h"
#include "catalog/pg_authid.h"
#include "executor/spi.h"
#include "postmaster/primary_mirror_mode.h"
#include "storage/fd.h"
#include "storage/lock.h"
#include "storage/relfilenode.h"
#include "storage/shmem.h"
#include "utils/palloc.h"

#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbpersistentstore.h"

#define CHANGETRACKING_FILE_EOF INT64CONST(-1)
#define CHANGETRACKING_METABUFLEN 128
/*
 * Global Variables
 */
char*	changeTrackingBuffer;
char	metabuf[CHANGETRACKING_METABUFLEN];
ChangeTrackingBufStatusData* changeTrackingBufStatus;
ChangeTrackingResyncMetaData* changeTrackingResyncMeta;

/*
 * Local functions
 */
static File ChangeTracking_OpenLogFile(void);
static void ChangeTracking_CloseLogFile(File file);
static File ChangeTracking_LogFileExists(void);
static void ChangeTracking_WriteBuffer(File file);
static void ChangeTracking_AddBufferPoolChange(XLogRecPtr* 	xlogLocation,
													   RelFileNode*	relFileNode,
													   BlockNumber	blockNum,
													   ItemPointerData	persistentTid,
													   int64 			persistentSerialNum);
//static IncrementalChangeList* ChangeTracking_InitIncrementalChangeList(int count);
static ChangeTrackingResult* ChangeTracking_FormResult(int count);
static void ChangeTracking_AddResultEntry(ChangeTrackingResult *result, 
										  Oid space,
										  Oid db,
										  Oid rel,
										  BlockNumber blocknum,
										  XLogRecPtr* lsn_end);

/*
 * Return the required shared-memory size for this module.
 */
extern Size ChangeTrackingShmemSize(void)
{
	Size		size = 0;
	
	size = add_size(size, CHANGETRACKING_BLCKSZ);  /* 32kB shmem buffer*/
	size = add_size(size, sizeof(ChangeTrackingBufStatusData)); /* the buffer status */
	size = add_size(size, sizeof(ChangeTrackingResyncMetaData)); /* the resync metadata */
	
	return size;
}
								
/*
 * Initialize the shared-memory for this module.
 */
extern void ChangeTrackingShmemInit(void)
{
	bool	foundBuffer,
			foundStatus,
			foundMeta;
	size_t	bufsize = CHANGETRACKING_BLCKSZ;
	
	changeTrackingBuffer = (char *) 
		ShmemInitStruct("Change Tracking Log Buffer", 
						bufsize, 
						&foundBuffer);
	
	changeTrackingBufStatus = (ChangeTrackingBufStatusData *) 
		ShmemInitStruct("Change Tracking Log Buffer Status", 
						sizeof(ChangeTrackingBufStatusData), 
						&foundStatus);

	changeTrackingResyncMeta = (ChangeTrackingResyncMetaData *) 
		ShmemInitStruct("Change Tracking Resync Meta Data", 
						sizeof(ChangeTrackingResyncMetaData), 
						&foundMeta);

	/* See if we are already initialized */
	if (foundBuffer || foundStatus || foundMeta)
	{
		/* all should be present or neither */
		Assert(foundBuffer && foundBuffer && foundMeta);
		return;
	}

	/* init buffer */
	memset(changeTrackingBuffer, 0, bufsize);
	
	/* init buffer status */
	changeTrackingBufStatus->recordcount = 0;
	changeTrackingBufStatus->maxbufsize = bufsize;
	changeTrackingBufStatus->bufsize = 0;
	changeTrackingBufStatus->fileseg = CHANGETRACKING_FILESEGMENT;
	changeTrackingBufStatus->nextwritepos = CHANGETRACKING_FILE_EOF;
	
	/* init meta data */
	changeTrackingResyncMeta->resync_mode_full = false;
	changeTrackingResyncMeta->resync_lsn_end.xlogid = 0;
	changeTrackingResyncMeta->resync_lsn_end.xrecoff = 0;
	
	ereport(DEBUG1, (errmsg("initialized changetracking shared memory structures")));

	return;
}


/*
 * This procedure will be called when mirror loss has been detected AND 
 * the master has told the primary it is carrying on.
 * It scans the xlog starting from the most recent checkpoint and collects 
 * all the interesting changes for the changelog. add them to the changelog.
 */
void ChangeTracking_CreateInitialFromPreviousCheckpoint(void)
{	
	File	file;
	int 	count = 0;
	
	/*
	 * Open the changelog file if not opened (and create if doesn't exist).
	 */
	file = ChangeTracking_OpenLogFile();
	
	count = XLogAddRecordsToChangeTracking();

	elog(LOG, "considered %d initial xlog records since last checkpoint "
			  "for writing into the resynchronize change log", count);	
	
	ChangeTracking_CloseLogFile(file);

	/* if any error (i.e. disk space) then mark FULL COPY in shared memory and on persistent media */
}


/*
 * Add a buffer pool change into the change log.
 * xlogLocation - The XLOG LSN of the record that describes the page change.
 * relFileNode  - The tablespace, database, and relation OIDs for the changed relation.
 * blockNum     - the block that was changed.
 * 
 */
static void ChangeTracking_AddBufferPoolChange(XLogRecPtr* 		xlogLocation,
											   RelFileNode*		relFileNode,
											   BlockNumber		blockNum,
											   ItemPointerData	persistentTid,
											   int64 			persistentSerialNum)
{
	ChangeTrackingRecord rec;
	int	freespace = 0;

	/* gp_persistent relation change? we shouldn't log it. exit early */
	if(GpPersistent_SkipXLogInfo(relFileNode->relNode))
		return;
	
	/* populate a new change log record */
	rec.xlogLocation = *xlogLocation;
	rec.relFileNode = *relFileNode;
	rec.bufferPoolBlockNum = blockNum;
	rec.persistentTid = persistentTid;
	rec.persistentSerialNum = persistentSerialNum;

	LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);
	
	if(changeTrackingBufStatus->bufsize == 0)
		changeTrackingBufStatus->bufsize = sizeof(ChangeTrackingPageHeader); /* leave room for header (first time around only) */
	
	/* copy to our shared memory buffer */
	memcpy(changeTrackingBuffer + changeTrackingBufStatus->bufsize, &rec, sizeof(rec));
	
	/* update state in shared memory */
	changeTrackingBufStatus->recordcount++;
	changeTrackingBufStatus->bufsize += sizeof(rec);

	/* 
	 * check if buffer is full. if it is pad it with zeros, add a
	 * header and write it to the change log file. We don't flush 
	 * it yet, it will be done during checkpoint.
	 */
	freespace = changeTrackingBufStatus->maxbufsize - changeTrackingBufStatus->bufsize;
	if(freespace < sizeof(ChangeTrackingRecord))
	{
		/*
		 * NOTE: We open the file, write it, and close it each time a buffer gets
		 * written. Why? The 'File' reference used to be kept in shmem but
		 * when the background writer comes in with a checkpoint the fd.c
		 * cache of the bgwriter process didn't know about this file. so,
		 * for now we keep it in the local fd.c cache and open and close each 
		 * time until a better solution is found.
		 */
		File file = ChangeTracking_OpenLogFile();
		ChangeTracking_WriteBuffer(file);
		ChangeTracking_CloseLogFile(file);
	}
	
	LWLockRelease(ChangeTrackingWriteLock);
}

XLogRecData* ChangeTracking_MakeRdataCopy(XLogRecData *rdata)
{
	XLogRecData* rdata_copy;
	
	rdata_copy = (XLogRecData*) palloc(sizeof(XLogRecData));
	rdata_copy->data = (char *) palloc(rdata->len);
	
	rdata_copy->len = rdata->len;
	rdata_copy->buffer_std = rdata->buffer_std;
	rdata_copy->buffer = rdata->buffer;
	rdata_copy->next = rdata->next;
	
	memcpy(rdata_copy->data, rdata->data, rdata->len);
	
	return rdata_copy;
}

void ChangeTracking_FreeRdataCopy(XLogRecData *rdata)
{
	pfree(rdata->data);
	pfree(rdata);		
}

/*
 * When a new xlog record is created and we're in changetracking mode this 
 * function gets called in order to create a changetracking record as well.
 * If the passed in xlog record is uninteresting to us, the function will
 * not log it and will return normally.
 * 
 * We pass in the actual RM data *separately* from the XLogRecord. We normally
 * wouldn't need to do that, because the data follows the XLogRecord header,
 * however it turns out that XLogInsert() will break apart an xlog record if in
 * buffer boundaries and load some of it in the end of current buffer and the
 * rest, therefore leaving it no longer contigious in memory.
 */
void ChangeTracking_AddRecordFromXlog(
									  RmgrId	xl_rmid,
									  uint8		xl_info,
									  void		*data, 
									  XLogRecPtr *loc)
{
	RelationChangeInfo 	relationChangeInfoArray[MaxRelationChangeInfoReturns];
	int					relationChangeInfoArrayCount;
	int					i;
		
	ChangeTracking_GetRelationChangeInfoFromXlog(
										  xl_rmid,
										  xl_info,
										  data, 
										  relationChangeInfoArray,
										  &relationChangeInfoArrayCount);

	for (i = 0; i < relationChangeInfoArrayCount; i++)
		ChangeTracking_AddBufferPoolChange(loc,
										   &relationChangeInfoArray[i].relFileNode,
										   relationChangeInfoArray[i].blockNumber,
										   relationChangeInfoArray[i].persistentTid,
										   relationChangeInfoArray[i].persistentSerialNum);
}

void ChangeTracking_PrintRelationChangeInfo(
									  RmgrId	xl_rmid,
									  uint8		xl_info,
									  void		*data, 
									  XLogRecPtr *loc,
									  bool		weAreGeneratingXLogNow)
{
	RelationChangeInfo 	relationChangeInfoArray[MaxRelationChangeInfoReturns];
	int					relationChangeInfoArrayCount;
	int					i;
	
	ChangeTracking_GetRelationChangeInfoFromXlog(
										  xl_rmid,
										  xl_info,
										  data, 
										  relationChangeInfoArray,
										  &relationChangeInfoArrayCount);

	for (i = 0; i < relationChangeInfoArrayCount; i++)
	{
		RelationChangeInfo	*relationChangeInfo;
		int64				maxPersistentSerialNum;
		bool				skip;
		bool				zeroTid = false;
		bool				invalidTid = false;
		bool				zeroSerialNum = false;
		bool				invalidSerialNum = false;
		bool				skipIssue = false;

		relationChangeInfo = &relationChangeInfoArray[i];

		if (weAreGeneratingXLogNow)
			maxPersistentSerialNum = PersistentRelation_MyHighestSerialNum();
		else
			maxPersistentSerialNum = PersistentRelation_CurrentMaxSerialNum();

		skip = GpPersistent_SkipXLogInfo(relationChangeInfo->relFileNode.relNode);
		if (!skip)
		{
			zeroTid = PersistentStore_IsZeroTid(&relationChangeInfo->persistentTid);
			if (!zeroTid)
				invalidTid = !ItemPointerIsValid(&relationChangeInfo->persistentTid);
			zeroSerialNum = (relationChangeInfo->persistentSerialNum == 0);
			if (!zeroSerialNum)
			{
				invalidSerialNum = (relationChangeInfo->persistentSerialNum < 0);

				/*
				 * If we have'nt done the scan yet... do not do upper range check.
				 */
				if (maxPersistentSerialNum != 0 &&
					relationChangeInfo->persistentSerialNum > maxPersistentSerialNum)
					invalidSerialNum = true;
			}
			skipIssue = (zeroTid || invalidTid || zeroSerialNum || invalidSerialNum);
		}

		if (!Debug_print_xlog_relation_change_info_skip_issues_only || skipIssue)
			elog(LOG, 
				 "ChangeTracking_PrintRelationChangeInfo: [%d] xl_rmid %d, xl_info 0x%X, %u/%u/%u, block number %d, LSN %s, persistent serial num " INT64_FORMAT ", TID %s, maxPersistentSerialNum " INT64_FORMAT ", skip %s, zeroTid %s, invalidTid %s, zeroSerialNum %s, invalidSerialNum %s, skipIssue %s",
				i,
				xl_rmid,
				xl_info,
				relationChangeInfo->relFileNode.spcNode,
				relationChangeInfo->relFileNode.dbNode,
				relationChangeInfo->relFileNode.relNode,
				relationChangeInfo->blockNumber,
				XLogLocationToString(loc),
				relationChangeInfo->persistentSerialNum,
				ItemPointerToString(&relationChangeInfo->persistentTid),
				maxPersistentSerialNum,
				(skip ? "true" : "false"),
				(zeroTid ? "true" : "false"),
				(invalidTid ? "true" : "false"),
				(zeroSerialNum ? "true" : "false"),
				(invalidSerialNum ? "true" : "false"),
				(skipIssue ? "true" : "false"));
	}
}


static void ChangeTracking_AddRelationChangeInfo(
									  RelationChangeInfo	*relationChangeInfoArray,
									  int					*relationChangeInfoArrayCount,
									  RelFileNode *relFileNode,
									  BlockNumber blockNumber,
									  ItemPointer persistentTid,
									  int64	  	  persistentSerialNum)
{
	RelationChangeInfo	*relationChangeInfo;
	
	Assert (*relationChangeInfoArrayCount < MaxRelationChangeInfoReturns);

	relationChangeInfo = &relationChangeInfoArray[*relationChangeInfoArrayCount];

	relationChangeInfo->relFileNode = 			*relFileNode;
	relationChangeInfo->blockNumber = 			blockNumber;
	relationChangeInfo->persistentTid = 		*persistentTid;
	relationChangeInfo->persistentSerialNum = 	persistentSerialNum;

	(*relationChangeInfoArrayCount)++;
}

void ChangeTracking_GetRelationChangeInfoFromXlog(
									  RmgrId	xl_rmid,
									  uint8 	xl_info,
									  void		*data, 
									  RelationChangeInfo	*relationChangeInfoArray,
									  int					*relationChangeInfoArrayCount)
{

	uint8	info = xl_info & ~XLR_INFO_MASK;
	uint8	op = 0;
		
	//Assert(ChangeTracking_ShouldTrackChanges());

	MemSet(relationChangeInfoArray, 0, sizeof(RelationChangeInfo) * MaxRelationChangeInfoReturns);
	*relationChangeInfoArrayCount = 0;

	/*
	 * Find the RM for this xlog record and see whether we are
	 * interested in logging it as a buffer pool change or not.
	 */
	switch (xl_rmid)
	{

		/*
		 * The following changes aren't interesting to the change log
		 */
		case RM_XLOG_ID:
		case RM_CLOG_ID:
		case RM_SEQ_ID:
		case RM_MULTIXACT_ID:
		case RM_XACT_ID:
		case RM_SMGR_ID:
		case RM_DISTRIBUTEDLOG_ID:
		case RM_DBASE_ID:
		case RM_TBLSPC_ID:
		case RM_BITMAP_ID:
			break;
			
		/* UNDONE? do we need to add these as well? */
		case RM_HASH_ID:
		case RM_GIST_ID:
		case RM_GIN_ID:
			break;
			
		/*
		 * The following changes must be logged in the change log.
		 */
		case RM_HEAP2_ID:
			switch (info)
			{
				case XLOG_HEAP2_FREEZE:
				{
					xl_heap_freeze *xlrec = (xl_heap_freeze *) data;

					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->heapnode.node),
													   xlrec->block,
													   &xlrec->heapnode.persistentTid,
													   xlrec->heapnode.persistentSerialNum);
				}
				break;

				default:
					elog(ERROR, "internal error: unsupported RM_HEAP2_ID op (%d) in AddRecordFromXlog", info);
			}
			break;
		case RM_HEAP_ID:
			op = info & XLOG_HEAP_OPMASK;
			switch (op)
			{
				case XLOG_HEAP_INSERT:
				{
					xl_heap_insert *xlrec = (xl_heap_insert *) data;
										
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   ItemPointerGetBlockNumber(&(xlrec->target.tid)),
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);
					break;
				}
				case XLOG_HEAP_DELETE:
				{
					xl_heap_delete *xlrec = (xl_heap_delete *) data;
										
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   ItemPointerGetBlockNumber(&(xlrec->target.tid)),
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);
					break;
				}
				case XLOG_HEAP_UPDATE:
				case XLOG_HEAP_MOVE:
				{
					xl_heap_update *xlrec = (xl_heap_update *) data;
					
					BlockNumber oldblock = ItemPointerGetBlockNumber(&(xlrec->target.tid));
					BlockNumber	newblock = ItemPointerGetBlockNumber(&(xlrec->newtid));						
					bool		samepage = (oldblock == newblock);
					
					
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   newblock,
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);
					if(!samepage)
						ChangeTracking_AddRelationChangeInfo(
														   relationChangeInfoArray,
														   relationChangeInfoArrayCount,
														   &(xlrec->target.node),
														   oldblock,
														   &xlrec->target.persistentTid,
														   xlrec->target.persistentSerialNum);
						
					break;
				}
				case XLOG_HEAP_CLEAN:
				{
					xl_heap_clean *xlrec = (xl_heap_clean *) data;

					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->heapnode.node),
													   xlrec->block,
													   &xlrec->heapnode.persistentTid,
													   xlrec->heapnode.persistentSerialNum);
					break;
				}
				case XLOG_HEAP_NEWPAGE:
				{
					xl_heap_newpage *xlrec = (xl_heap_newpage *) data;

					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->heapnode.node),
													   xlrec->blkno,
													   &xlrec->heapnode.persistentTid,
													   xlrec->heapnode.persistentSerialNum);
					break;
				}
				case XLOG_HEAP_LOCK:
				{
					xl_heap_lock *xlrec = (xl_heap_lock *) data;

					if (xl_info & XLR_BKP_BLOCK_1)
						break;
								
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   ItemPointerGetBlockNumber(&(xlrec->target.tid)),
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);
					break;
				}
				case XLOG_HEAP_INPLACE:
				{
					xl_heap_inplace *xlrec = (xl_heap_inplace *) data;
										
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   ItemPointerGetBlockNumber(&(xlrec->target.tid)),
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);
					break;
				}

				default:
					elog(ERROR, "internal error: unsupported RM_HEAP_ID op (%d) in AddRecordFromXlog", op);
			}
			break;

		case RM_BTREE_ID:
			switch (info)
			{
				case XLOG_BTREE_INSERT_LEAF:
				case XLOG_BTREE_INSERT_UPPER:
				case XLOG_BTREE_INSERT_META:
				{
					xl_btree_insert *xlrec = (xl_btree_insert *) data;
					BlockIdData blkid = xlrec->target.tid.ip_blkid;
					
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   BlockIdGetBlockNumber(&blkid),
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);

					break;
				}
				case XLOG_BTREE_SPLIT_L:
				case XLOG_BTREE_SPLIT_R:
				case XLOG_BTREE_SPLIT_L_ROOT:
				case XLOG_BTREE_SPLIT_R_ROOT:
				{
					xl_btree_split *xlrec = (xl_btree_split *) data;
					BlockIdData blkid = xlrec->target.tid.ip_blkid;
					
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   BlockIdGetBlockNumber(&blkid),
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);
					
					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->target.node),
													   xlrec->otherblk,
													   &xlrec->target.persistentTid,
													   xlrec->target.persistentSerialNum);
					break;
				}
				case XLOG_BTREE_DELETE:
				{
					xl_btree_delete *xlrec = (xl_btree_delete *) data;

					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->btreenode.node),
													   xlrec->block,
													   &xlrec->btreenode.persistentTid,
													   xlrec->btreenode.persistentSerialNum);
					break;
				}
				case XLOG_BTREE_DELETE_PAGE:
				case XLOG_BTREE_DELETE_PAGE_HALF:
				case XLOG_BTREE_DELETE_PAGE_META:
				{
					xl_btree_delete_page *xlrec = (xl_btree_delete_page *) data;
					BlockIdData blkid = xlrec->target.tid.ip_blkid;

					if (!(xl_info & XLR_BKP_BLOCK_1))
						ChangeTracking_AddRelationChangeInfo(
														   relationChangeInfoArray,
														   relationChangeInfoArrayCount,
														   &(xlrec->target.node),
														   BlockIdGetBlockNumber(&blkid),
														   &xlrec->target.persistentTid,
														   xlrec->target.persistentSerialNum);
					
					if (!(xl_info & XLR_BKP_BLOCK_2))
						ChangeTracking_AddRelationChangeInfo(
														   relationChangeInfoArray,
														   relationChangeInfoArrayCount,
														   &(xlrec->target.node),
														   xlrec->rightblk,
														   &xlrec->target.persistentTid,
														   xlrec->target.persistentSerialNum);

					if (!(xl_info & XLR_BKP_BLOCK_3) && xlrec->leftblk != P_NONE)
						ChangeTracking_AddRelationChangeInfo(
														   relationChangeInfoArray,
														   relationChangeInfoArrayCount,
														   &(xlrec->target.node),
														   xlrec->leftblk,
														   &xlrec->target.persistentTid,
														   xlrec->target.persistentSerialNum);					
					break;
				}
				case XLOG_BTREE_NEWROOT:
				{
					xl_btree_newroot *xlrec = (xl_btree_newroot *) data;

					ChangeTracking_AddRelationChangeInfo(
													   relationChangeInfoArray,
													   relationChangeInfoArrayCount,
													   &(xlrec->btreenode.node),
													   xlrec->rootblk,
													   &xlrec->btreenode.persistentTid,
													   xlrec->btreenode.persistentSerialNum);					
					
					break;
				}

				default:
					elog(ERROR, "internal error: unsupported RM_BTREE_ID op (%d) in AddRecordFromXlog", info);
			}
			break;

		default:
			elog(ERROR, "internal error: unsupported resource manager type (%d) in AddRecordFromXlog", xl_rmid);
	}

}



/*
 * CheckPointChangeTracking
 * 
 * When the system wide checkpoint is performed, this function is called.
 * We write and fsync the current page we have (partially full) into disk.
 * Also, we save this partially full buffer in shared memory, so that after
 * this checkpoint we will continue filling it up, and when it is full use
 * it to overwrite this partial page with a full one.
 */
void CheckPointChangeTracking(void)
{
	File	file;
	char	path[MAXPGPATH];
	
	/* do nothing if not in proper operating mode */
	if(!ChangeTracking_ShouldTrackChanges())
		return;
	
	LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);

	/* 
	 * see comment in ChangeTracking_AddBufferPoolChange 
	 * for why we open and close the file here.
	 */
	file = ChangeTracking_OpenLogFile();
	
	/* write the existing (non-full) shmem buffer */
	if(changeTrackingBufStatus->recordcount > 0)
		ChangeTracking_WriteBuffer(file);
	
	errno = 0;
	/* time to fsync change log to disk */
	if(FileSync(file) < 0)
	{
		ChangeTrackingLogFilePath(path);
		ereport(WARNING, 
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): no disk space for change tracking log file \"%s\", "
						"change tracking disabled : %m", 
						path)));	
		
		FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
		
		ChangeTracking_MarkFullResync();
	}
	
	ChangeTracking_CloseLogFile(file);

	LWLockRelease(ChangeTrackingWriteLock);

}

void ChangeTracking_RecoverUsingXLog(void)
{	
	/* do nothing if not in proper operating mode */
	if(!ChangeTracking_ShouldTrackChanges())
		return;

	/* ... recovery ... */
	
}

void ChangeTracking_Shutdown(void)
{	
	LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);

	changeTrackingBufStatus->bufsize = 0;
	changeTrackingBufStatus->recordcount = 0;
	changeTrackingBufStatus->nextwritepos = CHANGETRACKING_FILE_EOF;

	LWLockRelease(ChangeTrackingWriteLock);
}


/************************************************************************
 * API for getting incremental change entries for gp_persistent relation
 ************************************************************************/

IncrementalChangeList* ChangeTracking_InitIncrementalChangeList(int count)
{
	IncrementalChangeList* result;
	
	result = (IncrementalChangeList*) palloc0(sizeof(IncrementalChangeList));
	result->count = count;
	result->entries = (IncrementalChangeEntry*) palloc(sizeof(IncrementalChangeEntry) * count);
	
	return result;
}

/*
 * Get an ordered list of [persistent_tid,persistent_serialnum] from the
 * change tracking log, with unique tid values, each paired with the newest
 * serial num found for it.
 */
IncrementalChangeList* ChangeTracking_GetIncrementalChangeList(void)
{
	IncrementalChangeList* result = NULL;
	StringInfoData 	sqlstmt;
	int 			ret;
	int 			proc;
	bool 			connected = false;
	ResourceOwner 	save = CurrentResourceOwner;
	MemoryContext 	oldcontext = CurrentMemoryContext;

	Assert(dataState == DataStateInResync);	
	
	/* assemble our query string */
	initStringInfo(&sqlstmt);
	appendStringInfo(&sqlstmt, "SELECT persistent_tid, max(persistent_sn) "
							   "FROM gp_changetracking_log() " 
							   "GROUP BY persistent_tid "
							   "ORDER BY persistent_tid");
	
	PG_TRY();
	{

		/* must be in a transaction in order to use SPI */
		StartTransactionCommand();
		
		ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
		
		SetSessionUserId(10,true); /* TODO: HACK!!! find a solution */

		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Unable to obtain change tracking information from segment database."),
							errdetail("SPI_connect failed in ChangeTracking_GetIncrementalChangeList()")));
		}
		connected = true;

		/* Do the query. */
		ret = SPI_execute(sqlstmt.data, true, 0);
		proc = SPI_processed;


		if (ret > 0 && SPI_tuptable != NULL)
		{
			TupleDesc 		tupdesc = SPI_tuptable->tupdesc;
			SPITupleTable*	tuptable = SPI_tuptable;
			MemoryContext 	cxt_save;
			int 			i;

			/*
			 * Iterate through each result tuple
			 */
			for (i = 0; i < proc; i++)
			{
				HeapTuple 	tuple = tuptable->vals[i];

				IncrementalChangeEntry* entry;
				ItemPointer		persistentTid;
				int64 			persistentSerialNum;
				char*			str_tid;
				char*			str_sn;
				
				/* get result columns from SPI (as strings) */
				str_tid = SPI_getvalue(tuple, tupdesc, 1);
				str_sn = SPI_getvalue(tuple, tupdesc, 2);

				//elog(NOTICE,"tuple %d: tid %s sn %s", i, str_tid, str_sn);

				/* use our own context so that SPI won't free our stuff later */
				cxt_save = MemoryContextSwitchTo(oldcontext);

				/* init the result memory on first pass */
				if(i == 0)
					result = ChangeTracking_InitIncrementalChangeList(proc);
				
				/* convert to desired data type */
				persistentTid = DatumGetPointer(DirectFunctionCall1(tidin, CStringGetDatum(str_tid)));
				persistentSerialNum = DatumGetInt64(DirectFunctionCall1(int8in, CStringGetDatum(str_sn)));
				
				/* populate this entry */
				entry = &(result->entries[i]);
				entry->persistentTid = *persistentTid;
				entry->persistentSerialNum = persistentSerialNum;
				
				MemoryContextSwitchTo(cxt_save);
			}
		}
		else
		{
			/* no results for this given request */
			result = NULL;
		}

		connected = false;
		SPI_finish();
		
		CommitTransactionCommand();
	}

	/* Clean up in case of error. */
	PG_CATCH();
	{
		if (connected)
			SPI_finish();

		AbortCurrentTransaction();

		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(sqlstmt.data);

	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = save;

	return result;
}

void ChangeTracking_FreeIncrementalChangeList(IncrementalChangeList* iclist)
{
	Assert(iclist);
	
	pfree(iclist->entries);
	pfree(iclist);		
}

/*************************************
 * API for the synchronizer process
 *************************************/

/*
 * Allocate memory for a request that includes information about
 * the objects of interest.
 */
ChangeTrackingRequest* ChangeTracking_FormRequest(int max_count)
{
	ChangeTrackingRequest* request;
	
	request = (ChangeTrackingRequest*) palloc0(sizeof(ChangeTrackingRequest));
	request->count = 0;
	request->max_count = max_count;
	request->entries = (ChangeTrackingRequestEntry*) palloc(sizeof(ChangeTrackingRequestEntry) * max_count);
	
	return request;
}

/*
 * Add an entry to the request. An entry is a specification of a relation
 * and the start/end LSN of interest. If start LSN isn't needed, the caller
 * must specify 0/0 for now.  
 */
void ChangeTracking_AddRequestEntry(ChangeTrackingRequest *request, 
									RelFileNode		relFileNode,
									XLogRecPtr*		lsn_start,
									XLogRecPtr*		lsn_end)
{
	ChangeTrackingRequestEntry* entry;
	
	if(request->count + 1 > request->max_count)
		elog(ERROR, "ChangeTracking: trying to add more request entries than originally requested");
	
	entry = &(request->entries[request->count]);
	entry->relFileNode.relNode = relFileNode.relNode;
	entry->relFileNode.spcNode = relFileNode.spcNode;
	entry->relFileNode.dbNode = relFileNode.dbNode;
	entry->lsn_start = *lsn_start;  
	entry->lsn_end = *lsn_end;
	
	request->count++;
}

/*
 * Deallocate memory associated with the request.
 */
void ChangeTracking_FreeRequest(ChangeTrackingRequest *request)
{
	pfree(request->entries);
	pfree(request);
}

/*
 * Allocate memory for assembling the result. This will be used internally
 * by the resync changetracking after the synchronizer requested information,
 * the result of the request will be stored here and passed back to the caller.
 */
static ChangeTrackingResult* ChangeTracking_FormResult(int max_count)
{
	ChangeTrackingResult* result;
	
	result = (ChangeTrackingResult*) palloc0(sizeof(ChangeTrackingResult));
	result->count = 0;
	result->max_count = max_count;
	result->entries = (ChangeTrackingResultEntry*) palloc(sizeof(ChangeTrackingResultEntry) * max_count);
	
	return result;

}

static void ChangeTracking_AddResultEntry(ChangeTrackingResult *result, 
										  Oid space,
										  Oid db,
										  Oid rel,
										  BlockNumber blocknum,
										  XLogRecPtr* lsn_end)
{
	ChangeTrackingResultEntry* entry;
	
	if(result->count + 1 > result->max_count)
		elog(ERROR, "ChangeTracking: trying to add more result entries than originally requested");
	
	entry = &(result->entries[result->count]);
	entry->relFileNode.spcNode = space;
	entry->relFileNode.dbNode = db;
	entry->relFileNode.relNode = rel;
	entry->block_num = blocknum;
	entry->lsn_end = *lsn_end;
	
	result->count++;
}

/*
 * We are in resync mode and the synchronizer module is asking
 * us for the information we have gathered. 
 * 
 * The synchronizer passes in a list of relfilenodes, each with 
 * a start and end LSN. For each of those relations that are 
 * found in the change tracking log file this routine will return
 * the list of block numbers and the end LSN of each.
 * 
 */
ChangeTrackingResult* ChangeTracking_GetChanges(ChangeTrackingRequest *request)
{
	ChangeTrackingResult* result = NULL;
	StringInfoData 	sqlstmt;
	int 			ret;
	int 			proc;
	int				i;
	volatile bool 	connected = false; /* needs to survive PG_TRY()/CATCH() */
	ResourceOwner 	save = CurrentResourceOwner;
	MemoryContext 	oldcontext = CurrentMemoryContext;

	Assert(dataState == DataStateInResync);	
	
	/* assemble our query string */
	initStringInfo(&sqlstmt);
	appendStringInfo(&sqlstmt, "SELECT space, db, rel, blocknum, max(xlogloc) "
							   "FROM gp_changetracking_log() "
							   "WHERE ");

	for(i = 0 ; i < request->count ;  i++)
	{
		XLogRecPtr 	lsn_start = request->entries[i].lsn_start;
		XLogRecPtr 	lsn_end  = request->entries[i].lsn_end;

		Oid 	space = request->entries[i].relFileNode.spcNode;
		Oid 	db = request->entries[i].relFileNode.dbNode;
		Oid 	rel = request->entries[i].relFileNode.relNode;
		
		if(i != 0)
			appendStringInfo(&sqlstmt, "OR ");
		
		appendStringInfo(&sqlstmt, "(space = %u AND "
								   "db = %u AND "
								   "rel = %u AND "
								   "xlogloc < '(%X/%X)' AND "
								   "xlogloc >= '(%X/%X)') ",
								   space, db, rel, lsn_end.xlogid, lsn_end.xrecoff, 
								   lsn_start.xlogid, lsn_start.xrecoff);
		
		/* TODO: use gpxloglocout() to format the '(%X/%X)' instead of doing it manually here */
	}
	
	appendStringInfo(&sqlstmt, "GROUP BY space, db, rel, blocknum "
							   "ORDER BY space, db, rel, blocknum");
	
	//elog(LOG, "ChangeTracking_GetChanges query: %s", sqlstmt.data);
	
	PG_TRY();
	{
		/* must be in a transaction in order to use SPI */
		StartTransactionCommand();

		ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());

		/* 
		 * TODO: HACK!!! find a solution
		 * It's possible that the bootstrap superuser has been removed from the
		 * system. ftsprobe has a routine for determining a valid superuser by
		 * scanning pg_authid.
		*/
		SetSessionUserId(BOOTSTRAP_SUPERUSERID, true);

		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Unable to obtain change tracking information from segment database."),
							errdetail("SPI_connect failed in ChangeTracking_GetChanges()")));
		}
		connected = true;

		/* Do the query. */
		ret = SPI_execute(sqlstmt.data, true, 0);
		proc = SPI_processed;


		if (ret > 0 && SPI_tuptable != NULL)
		{
			TupleDesc 		tupdesc = SPI_tuptable->tupdesc;
			SPITupleTable*	tuptable = SPI_tuptable;
			MemoryContext 	cxt_save;
			int i;

			/*
			 * Iterate through each result tuple
			 */
			for (i = 0; i < proc; i++)
			{
				HeapTuple 	tuple = tuptable->vals[i];

				BlockNumber blocknum;
				XLogRecPtr*	endlsn;
				Oid 		space;
				Oid 		db;
				Oid 		rel;
				char*		str_space;
				char*		str_db;
				char*		str_rel;
				char*		str_blocknum;
				char*		str_endlsn;
				
				/* get result columns from SPI (as strings) */
				str_space = SPI_getvalue(tuple, tupdesc, 1);
				str_db = SPI_getvalue(tuple, tupdesc, 2);
				str_rel = SPI_getvalue(tuple, tupdesc, 3);
				str_blocknum = SPI_getvalue(tuple, tupdesc, 4);
				str_endlsn = SPI_getvalue(tuple, tupdesc, 5);

				//elog(NOTICE,"tuple %d: %s %s %s block %s lsn %s", i, str_space, str_db, str_rel, str_blocknum, str_endlsn);

				/* use our own context so that SPI won't free our stuff later */
				cxt_save = MemoryContextSwitchTo(oldcontext);

				/* init the result memory on first pass */
				if(i == 0)
					result = ChangeTracking_FormResult(proc);
				
				/* convert to desired data type */
				space = DatumGetObjectId(DirectFunctionCall1(oidin, CStringGetDatum(str_space)));
				db = DatumGetObjectId(DirectFunctionCall1(oidin, CStringGetDatum(str_db)));
				rel = DatumGetObjectId(DirectFunctionCall1(oidin, CStringGetDatum(str_rel)));
				blocknum = DatumGetUInt32(DirectFunctionCall1(int4in, CStringGetDatum(str_blocknum)));
				endlsn = (XLogRecPtr*) DatumGetPointer(DirectFunctionCall1(gpxloglocin, CStringGetDatum(str_endlsn)));
				/* TODO: in the above should use DatumGetXLogLoc instead, but it's not public */
				
				/* add our entry to the result */
				ChangeTracking_AddResultEntry(result, 
											  space,
											  db,
											  rel,
											  blocknum,
											  endlsn);
				
				MemoryContextSwitchTo(cxt_save);
			}
		}
		else
		{
			/* no results for this given request */
			result = NULL;
		}

		connected = false;
		SPI_finish();
		
		CommitTransactionCommand();
	}

	/* Clean up in case of error. */
	PG_CATCH();
	{
		if (connected)
			SPI_finish();

		AbortCurrentTransaction();

		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(sqlstmt.data);

	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = save;

	return result;
}

/*
 * Free all memory associated with the result, after using it.
 */
void ChangeTracking_FreeResult(ChangeTrackingResult *result)
{
	if(result)
	{
		pfree(result->entries);
		pfree(result);		
	}
}

/*************************************
 * File I/O routines
 *************************************/

/*
 * Open change tracking log file for write, and seek to eof
 */
static File ChangeTracking_OpenLogFile(void)
{
	File	file;
	char	path[MAXPGPATH];

	/* set up correct path */
	ChangeTrackingLogFilePath(path);
	
	/* open it (and create if doesn't exist) */
	file = 	PathNameOpenFile(path, 
							 O_WRONLY | O_CREAT | PG_BINARY, 
							 S_IRUSR | S_IWUSR);
		
	/* 
	 * seek to eof for appending. 
	 * (can't use O_APPEND because we may like to reposition later on).
	 */
	FileSeek(file, 0, SEEK_END); 
	
	return file;
}

static void ChangeTracking_CloseLogFile(File file)
{
	FileClose(file);
}

/*
 * Open change tracking meta file for write, and seek to beginning.
 */
static File ChangeTracking_OpenMetaFile(void)
{
	File	file;
	char	path[MAXPGPATH];

	/* set up correct path */
	ChangeTrackingMetaFilePath(path);
	
	/* open it (and create if doesn't exist) */
	file = 	PathNameOpenFile(path, 
							 O_RDWR | O_CREAT | PG_BINARY, 
							 S_IRUSR | S_IWUSR);
		
	/* 
	 * seek to beginning of file. The meta file only has a single
	 * block. we will overwrite it each time with new meta data.
	 */
	FileSeek(file, 0, SEEK_SET); 
	
	return file;
}

static void ChangeTracking_CloseMetaFile(File file)
{
	FileClose(file);
}

static File ChangeTracking_LogFileExists(void)
{
	File	file;
	char	path[MAXPGPATH];

	/* set up correct path */
	ChangeTrackingLogFilePath(path);
	
	/* open it (and create if doesn't exist) */
	file = 	PathNameOpenFile(path, 
							 O_RDONLY | PG_BINARY, 
							 S_IRUSR | S_IWUSR);
	
	if(file < 0)
		return false;
	else
		ChangeTracking_CloseLogFile(file);
	
	return true;
}

static void ChangeTracking_DropLogFile(void)
{
	File	file;

	//LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);

	file = ChangeTracking_OpenLogFile();
	
	changeTrackingBufStatus->bufsize = 0;
	changeTrackingBufStatus->recordcount = 0;
	changeTrackingBufStatus->nextwritepos = CHANGETRACKING_FILE_EOF;

	/* delete the change tracking log file*/
	FileUnlink(file);
		
	//LWLockRelease(ChangeTrackingWriteLock);
}

static void ChangeTracking_DropMetaFile(void)
{
	File	file;

	//LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);

	file = ChangeTracking_OpenMetaFile();
	
	changeTrackingResyncMeta->resync_mode_full = false;
	changeTrackingResyncMeta->resync_lsn_end.xlogid = 0;
	changeTrackingResyncMeta->resync_lsn_end.xrecoff = 0;
	
	/* delete the change tracking meta file*/
	FileUnlink(file);
		
	//LWLockRelease(ChangeTrackingWriteLock);
}

/*
 * Drop all change tracking files (log and meta).
 */
void ChangeTracking_DropAll(void)
{
	LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);
	
	ChangeTracking_DropLogFile();
	ChangeTracking_DropMetaFile();
	
	LWLockRelease(ChangeTrackingWriteLock);
}

/*
 * Write our shared memory buffer to the log file. This will be
 * called if we have a buffer full of change log records or 
 * during a checkpoint (a checkpoint will flush it too).
 *  
 * NOTE: caller must hold a LW lock before calling this function.
 * NOTE: caller must also verify that the buffer isn't empty.
 */
static void ChangeTracking_WriteBuffer(File file)
{
	ChangeTrackingPageHeader	header;
	void	*headerptr;
	char	path[MAXPGPATH];
	int 	freespace = changeTrackingBufStatus->maxbufsize - changeTrackingBufStatus->bufsize;
	int 	wrote = 0;
	int64	restartpos;
	
	if(changeTrackingBufStatus->recordcount == 0)
		elog(ERROR, "ChangeTracking_WriteBuffer called with empty buffer");
	
	/* set the page header and copy it */	
	header.blockversion = CHANGETRACKING_STORAGE_VERSION;
	header.numrecords = changeTrackingBufStatus->recordcount;
	headerptr = &header;
	memcpy(changeTrackingBuffer, headerptr, sizeof(ChangeTrackingPageHeader));
	
	/* pad end of page with zeros */	
	MemSet(changeTrackingBuffer + changeTrackingBufStatus->bufsize, 0, freespace);
	
	/*
	 * 1) set the file write position with FileSeek.
	 * 2) save the position in 'restartpos' for the next round (if the page 
	 *    we'll write soon isn't full). see nextwritepos at the bottom of 
	 *    this function for more information.
	 */
	if (changeTrackingBufStatus->nextwritepos != CHANGETRACKING_FILE_EOF)
		restartpos = FileSeek(file, changeTrackingBufStatus->nextwritepos, SEEK_SET);
	else
		restartpos = FileSeek(file, 0, SEEK_END);

	
	/* write the complete buffer to file */
	errno = 0;
	wrote = FileWrite(file, 
					  changeTrackingBuffer, 
					  changeTrackingBufStatus->maxbufsize);

	/* TODO: should these be PANIC instead of ERROR? */
	if (wrote == 0)
	{
		ChangeTrackingLogFilePath(path);
		ereport(WARNING, 
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): no disk space for change tracking log file \"%s\", "
						"change tracking disabled : %m", 
					   path)));	
		
		FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
		
		ChangeTracking_MarkFullResync();
	}
	else if (wrote < 0)
	{
		ChangeTrackingLogFilePath(path);
		ereport(WARNING, 
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): unable to write change tracking log file \"%s\", "
						"change tracking disabled : %m", 
					   path)));
		
		FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
		
		ChangeTracking_MarkFullResync();
	}

	/*
	 * check if there's room for more records in this buffer. 
	 */
	if (freespace < sizeof(ChangeTrackingRecord))
	{
		/* block written and is full. reset it to zero */
		changeTrackingBufStatus->bufsize = 0;
		changeTrackingBufStatus->recordcount = 0;
		changeTrackingBufStatus->nextwritepos = CHANGETRACKING_FILE_EOF;
	}
	else
	{
		/* 
		 * written block is partially full (probably due to a checkpoint).
		 * Don't reset the buffer in shared memory, as we'd like to fill 
		 * it up and later overwrite the block we just wrote. Set the next 
		 * write position to the beginning of this page we just wrote, to 
		 * use in the next round.
		 */
		changeTrackingBufStatus->nextwritepos = restartpos;
	}
	
}

/*
 * Write a meta record to the meta file.
 */
static void ChangeTracking_WriteMeta(File file)
{
	
	ChangeTrackingMetaRecord rec;
	char			path[MAXPGPATH];
	int 			wrote = 0;

	Assert(CHANGETRACKING_METABUFLEN >= sizeof(rec));

	/* populate a new meta record */
	rec.resync_lsn_end = changeTrackingResyncMeta->resync_lsn_end;
	rec.resync_mode_full = changeTrackingResyncMeta->resync_mode_full;
	
	
	/* init buffer and copy rec into it */	
	MemSet(metabuf, 0, CHANGETRACKING_METABUFLEN);
	memcpy(metabuf, &rec, sizeof(rec));

	/* write the complete buffer to meta file */
	errno = 0;
	wrote = FileWrite(file, metabuf, CHANGETRACKING_METABUFLEN);

	/* TODO: should these be PANIC instead of ERROR? */
	if (wrote == 0)
	{
		ChangeTrackingMetaFilePath(path);
		ereport(WARNING, 
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): no disk space for change tracking meta file \"%s\", "
						"change tracking disabled : %m", 
					   path)));	
		
		FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
		
		ChangeTracking_MarkFullResync();
	}
	else if (wrote < 0)
	{
		ChangeTrackingMetaFilePath(path);
		ereport(WARNING, 
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): unable to write change tracking meta file \"%s\", "
						"change tracking disabled : %m", 
					   path)));
		
		FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
		
		ChangeTracking_MarkFullResync();
	}
}

/*
 * Read a meta record from the Meta file.
 */
static ChangeTrackingMetaRecord*
ChangeTracking_ReadMeta(File file, ChangeTrackingMetaRecord* rec)
{
	char			path[MAXPGPATH];
	int 			nbytes = 0;

	Assert(CHANGETRACKING_METABUFLEN >= sizeof(ChangeTrackingMetaRecord));	
	
	/* init buffer */	
	MemSet(metabuf, 0, CHANGETRACKING_METABUFLEN);

	/* read the record from the meta file, if any */
	errno = 0;
	nbytes = FileRead(file, metabuf, sizeof(ChangeTrackingMetaRecord));

	if (nbytes == 0)
	{
		/* no record */
		return NULL;
	}
	else if (nbytes < 0)
	{
		ChangeTrackingMetaFilePath(path);
		ereport(WARNING, 
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): unable to read change tracking meta file \"%s\", "
						"change tracking disabled : %m", 
					   path)));
		return NULL;
	}
	
	/* populate a the meta record with data from the file */
	rec->resync_lsn_end = ((ChangeTrackingMetaRecord *)metabuf)->resync_lsn_end;
	rec->resync_mode_full = ((ChangeTrackingMetaRecord *)metabuf)->resync_mode_full;
	
	return rec;
}

bool ChangeTracking_ShouldTrackChanges(void)
{
	return (FileRep_IsInChangeTracking());
}	
	
/*************************************
 * Resync related routines.
 *************************************/

/*
 * The routine mark in shared memory and persistently in Change Tracking log file
 * that full resync is required and disable change tracking.
 *
 * The routine is called 
 *		a) if failure is detected during Change Tracking 
 *		b) if user requested gprecoverseg with --FULL option (i.e. mirror node replacement)
 *		c) if user performed gpaddmirrors
 */
void
ChangeTracking_MarkFullResync(void)
{
	File	file;
	char	path[MAXPGPATH];

	LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);

	/* it will be removed when backend signaling will be implemented */
	getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);

	/* NOTE commented till fix is implemented for marking full copy before transition to InResync */
	//Assert(dataState == DataStateInChangeTracking);

	setFullResync(); // the routine set full resync required in shared memory

	/* NOTE commented till fix is implemented for marking full copy before transition to InResync */
	//FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
	
	/* 
	 * Insert an entry into the meta file that mark FullResync needed, and fsync.
	 */
	elog(LOG, "CHANGETRACKING: marking full resync");
	changeTrackingResyncMeta->resync_mode_full = true;
	file = ChangeTracking_OpenMetaFile();	
	ChangeTracking_WriteMeta(file);
	
	if(FileSync(file) < 0)
	{
		ChangeTrackingMetaFilePath(path);
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): could not fsync change tracking meta file \"%s\": %m", 
						path)));
		FileRep_SetState(FileRepStateFault);
	}
	
	ChangeTracking_CloseMetaFile(file);
		
	/*
	 * Delete Change Tracking log file (if exists)
	 * 
	 * TODO: should ereport(ERROR) if file doesn't exist?
	 */
	ChangeTracking_DropLogFile();
	
	LWLockRelease(ChangeTrackingWriteLock);
}

/*
 *	Store last change tracked LSN into shared memory and 
 *	in Change Tracking meta file (LSN has to be fsync-ed to persistent media).
 *	Also make sure to write any remaining log file data and fsync it.
 */
void 
ChangeTracking_RecordLastChangeTrackedLoc(void)
{
	File		file;
	char		path[MAXPGPATH];
	XLogRecPtr 	endResyncLSN;
	
	LWLockAcquire(ChangeTrackingWriteLock, LW_EXCLUSIVE);

	endResyncLSN = XLogLastChangeTrackedLoc();
	
	/* the routine stores last LSN in shared memory */
	FileRepResyncManager_SetEndResyncLSN(endResyncLSN); 

	/* 
	 * append "endResyncLSN" (last LSN recorded in Change Tracking log files)
	 * in Change Tracking meta file. "endResyncLSN" also marks the last entry 
	 * in Change Tracking before resync takes place. Later If transition from 
	 * Resync to Change tracking occurs then new changes will be appended.
	 */
	elog(LOG, "CHANGETRACKING: setting resync lsn");
	changeTrackingResyncMeta->resync_lsn_end = endResyncLSN;
	file = ChangeTracking_OpenMetaFile();	
	ChangeTracking_WriteMeta(file);

	errno = 0;
	
	/* fsync meta file to disk */
	if(FileSync(file) < 0)
	{
		ChangeTrackingMetaFilePath(path);
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): could not fsync change tracking log file \"%s\": %m", 
						path)));
		FileRep_SetState(FileRepStateFault);
	}
	
	ChangeTracking_CloseMetaFile(file);
			
	/* 
	 * log file: write the existing (non-full) buffer and fsync it 
	 */
	file = ChangeTracking_OpenLogFile();	
	
	if(changeTrackingBufStatus->recordcount > 0)
		ChangeTracking_WriteBuffer(file);
	
	if(FileSync(file) < 0)
	{
		ChangeTrackingLogFilePath(path);
		ereport(WARNING, 
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): no disk space for change tracking log file \"%s\", "
						"change tracking disabled : %m", 
						path)));	
		
		/* TODO: what needs to be set here in case of failure? any of these?
		FileRep_SetState(FileRepStateFault);
		FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
		ChangeTracking_MarkFullResync();*/
	}
	
	ChangeTracking_CloseLogFile(file);

	LWLockRelease(ChangeTrackingWriteLock);
}

/*
 * The routine is called during gpstart if dataState == DataStateInResync. 
 * Responsibilities:
 *		*) determines when crash recovery will be performed by checking
 *		   if lastChangeTrackedLoc is recorded
 *			*) if lastChangeTrackedLoc is recorded then 
 *					1) resync recovers to ready state
 *					2) WAL replays to primary and mirror
 *			*) if lastChangeTrackedLoc is NOT recorded then
 *					1) WAL replays to primary with Change Tracking ON
 *					2) transition to Resync
 *		*) check/set if fullResync is recorded
 */
bool
ChangeTracking_RetrieveLastChangeTrackedLoc(void)
{	
	ChangeTrackingMetaRecord  	rec;
	ChangeTrackingMetaRecord* 	recptr = NULL;
	XLogRecPtr					DummyRecPtr = {0, 0};
	File						file;
	
	file = ChangeTracking_OpenMetaFile();	

	recptr = ChangeTracking_ReadMeta(file, &rec);
	
	/* is there a meta record in the meta file? */
	if (recptr)
	{
		elog(LOG, "CHANGETRACKING: found an MD record. is full resync %d, last lsn (%d/%d)",
				recptr->resync_mode_full, 
				recptr->resync_lsn_end.xlogid,
				recptr->resync_lsn_end.xrecoff);

		if (recptr->resync_mode_full) 	
			setFullResync();
		 
		/* 
		 * if resync_lsn_end isn't {0,0} then we have a valid value 
		 * that was set earlier. in that case set it in the resync 
		 * manager shared memory
		 */
		if (!XLByteEQ(recptr->resync_lsn_end, DummyRecPtr))
		{
			 FileRepResyncManager_SetEndResyncLSN(recptr->resync_lsn_end); 
			 return true;
		}
	}
	else
		elog(LOG, "CHANGETRACKING: didn't find an MD record");

	
	ChangeTracking_CloseMetaFile(file);
	
	return false;
}

/*
 * Actually read the change tracking log and return the last loc
 * written into the file so far. This routine will be used for 
 * recovery purposes, for when we crashed during change tracking
 * and need to restart it. The first thing we'd like to know is
 * which record is currently our last one.
 * 
 * return the last loc lsn, or NULL if doesn't exist (due to file 
 * doesn't exist or empty)
 */
XLogRecPtr* ChangeTracking_ReadLastLocOnDisk(void)
{	
	StringInfoData 	sqlstmt;
	int 			ret;
	bool 			connected = false;
	XLogRecPtr*		endlsn;
	ResourceOwner 	save = CurrentResourceOwner;
	MemoryContext 	oldcontext = CurrentMemoryContext;
	

	/* first find out if log file exists, if not return NULL */
	if(!ChangeTracking_LogFileExists())
		return NULL;
	
	
	/* assemble our query string */
	initStringInfo(&sqlstmt);
	appendStringInfo(&sqlstmt, "SELECT max(xlogloc) "
							   "FROM gp_changetracking_log()");
	
	PG_TRY();
	{

		/* must be in a transaction in order to use SPI */
		StartTransactionCommand();

		ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
		
		SetSessionUserId(10,true); /* TODO: HACK!!! find a solution */

		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
							errmsg("Unable to obtain change tracking information from segment database."),
							errdetail("SPI_connect failed in ChangeTracking_ReadLastLocOnDisk()")));
		}
		connected = true;

		/* Do the query. */
		ret = SPI_execute(sqlstmt.data, true, 0);
		
		Assert(SPI_processed == 1);


		if (ret > 0 && SPI_tuptable != NULL)
		{
			TupleDesc 		tupdesc = SPI_tuptable->tupdesc;
			SPITupleTable*	tuptable = SPI_tuptable;
			MemoryContext 	cxt_save;
			HeapTuple 		tuple = tuptable->vals[0];
			char*			str_endlsn = SPI_getvalue(tuple, tupdesc, 1);
				
			/* use our own context so that SPI won't free our stuff later */
			cxt_save = MemoryContextSwitchTo(oldcontext);

			endlsn = (XLogRecPtr*) DatumGetPointer(DirectFunctionCall1(gpxloglocin, CStringGetDatum(str_endlsn)));
				
			MemoryContextSwitchTo(cxt_save);
			
		}
		else
		{
			/* no results for this given request - empty file. return 0 LSN */
			endlsn = NULL;
		}

		connected = false;
		SPI_finish();
		
		CommitTransactionCommand();
	}

	/* Clean up in case of error. */
	PG_CATCH();
	{
		if (connected)
			SPI_finish();

		AbortCurrentTransaction();
		
		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = save;

	pfree(sqlstmt.data);

	return endlsn;

}
