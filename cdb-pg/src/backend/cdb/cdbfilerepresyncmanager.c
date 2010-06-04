/*
 *  cdbfilerepprimaryrecovery.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved. *
 *
 */

/*
 * INVARIANCES
 *
 *		*) Change Tracking
 *				*) Change Tracking is turned ON when mirror segment is down 
 *				   (gp_segment_configuration is in CHANGE TRACKING after failover)
 *				*) In the transition to Change Tracking recovery from the latest checkpoint is done
 *				*) Change tracking keeps track of changes to Heap Data Store 
 *				   (RelFileNode, Block#, LSN, Persistent TID, Persistent Serial Number)
 *				*) Change Tracking is turned OFF (disabled) during resynchronization transition 
 *				   while primary segment is suspended		
 *					*) That means that new changes are not tracked and 
 *					   Change tracking log files are NOT deleted
 *				*) Change Tracking log files are deleted when primary and mirror segment 
 *				   transitions to InSYNC state
 *
 *		*) Resynchronization (full copy, incremental file copy, incremental block copy)
 *				*) Take last LSN to be resynced “last resync LSN”
 *				*) Resync to mirror all CREATE file operations (RelFileNode, segmentFileNum)
 *				*) Resync to mirror all DROP file operations (RelFileNode, segmentFileNum)
 *				*) Resync to mirror all file changes up to “last resync LSN”. 
 *				   Three options available:
 *						*) Full Copy
 *						   For each segment file in a relation for each relation in a 
 *						   segment repeat the following steps:
 *								*) Copy all blocks in segment file that has LSN < “last resync LSN”
 *								*) Blocks that has LSN =>”last resync LSN” will be mirrored
 *								*) After copy of each segment file take “resync CKPT” 
 *								   (persistently store the latest LSN, it identifies the start 
 *									resync point in case of restarting resync due to failures)
 *						*) Incremental File Copy
 *						   For each segment file in a relation that was modified during change tracking
 *								*) Copy all blocks in segment file that has LSN < “last resync LSN”
 *								*) Blocks that has LSN =>”last resync LSN” will be mirrored
 *								*) After copy of each segment file take “resync CKPT” 
 *								   (persistently store the latest LSN)
 *						*) Incremental Block Copy
 *						   For each segment file in a relation that was modified during change tracking
 *								*) Copy all blocks in segment file that has LSN < “last resync LSN”
 *								*) Blocks that has LSN =>”last resync LSN” will be mirrored
 *								*) After copy of each segment file take “resync CKPT” 
 *								   (persistently store the latest LSN)
 *				*) Crash Recovery of primary segment during resynchronization will 
 *				   restart copy from “resync CKPT”
 *				*) Mirror down during resynchronization will turn Change tracking ON and 
 *				   turn Resynchronization OFF
 *				*) When Mirror comes up and resynchronization is restarted
 *				   the resynchronization will be restarted from the “resync CKPT”
 *				*) Changes to be resynchronized are cumulative when Change Tracking is 
 *				   turned on multiple times
 *				*) Catch up of pg_twophase, pg_xlog, pg_control, ... before transition to InSYNC mode
 * 
 *		*) Ongoing IO to primary segment
 *				*) When resynchronization transition from its recovery to ready state 
 *				   changes to primary segment are mirrored (last resync LSN + 1)
 *				*) If primary segment crashes during resynchronization then during 
 *				   recovery any change applied during WAL replay on primary is applied 
 *				   also to mirror
 *				*) If mirror segment goes down then Change Tracking is turned ON on the 
 *				   primary. No disruption to IO on primary segment.
 *				*) During resynchronization WAL and flat files are excluded from mirroring.
 *
 */

/*
 *	The following steps are performed during dataState == DataStateResynchronizing 
 *		*) FULL Copy
 *			*) Transition Segment State 
 *				*) mark all relations re-create needed
 *				*) mark all relations full copy request
 *
 *
 *			*) Ready Segment State
 *
 *		*) INCREMENTAL Copy
 *				*) Transition Segment State
 *
 *				*) Ready Segment State
 *					*) Scan Change Tracking to get all relations to be marked 
 *					   in persistent file system object
 *
 */

/*
 Transition from Change Tracking to InResync on primary segment
 ==============================================================
 1)	Shutdown FileRep Main process 
 2)	postmaster set
		*) dataState = DataStateInResync 
		*) segmentState = SegmentStateInResyncTransition
 3)	Start FileRep Main process 
 4)	Start 1 FileRep backend process as a Resync Manager
		*) FileRepMain -> FileRepPrimary_StartResyncManager()
 5)	Start N FileRep backend processes as a Resync Worker  ( N should be GUC, Min=1, Max=8, Default =1 (Default will be increased later))
		*) FileRepMain -> FileRepPrimary_StartResyncWorker()
 6)	FileRep Resync backend processes exit when resynchronization is completed 
	ResyncManager set
		*) dataState == DataStateInSync 
		*) segmentState = SegmentStateReady
 
 Crash Recovery when dataState = DataStateInResync on primary segment
 ====================================================================
 1) postmaster sets 
		*) dataState = DataStateInResync 
		*) segmentState = SegmentStateInitialization
 2)	Start FileRep Main process 
 3)	Start 1 FileRep backend process as a Resync Manager
		*)	FileRepMain() -> FileRepPrimary_StartResyncManager()
 5)	Start N FileRep backend processes as a Resync Worker  ( N should be GUC, Min=1, Max=8, Default =1 (Default will be increased later))
		*) FileRepMain() -> FileRepPrimary_StartResyncWorker()
 6)	Resync Manager process checks the state of resync transition
	If resync transition has not been completed yet then
		a) Resync Manager sets 
			*) dataState = DataStateInChangeTracking 
			*) segmentState = SegmentStateReady or SegmentStateChangeTrackingDisabled
		b) postmaster run XLOG recovery 
		c) postmaster sets 
			*) dataState = DataStateInResync 
			*) segmentState = SegmentStateInResyncTransition
	else
		a) Resync Manager sets 
			*) dataState = DataStateInResync 
			*) segmentState = SegmentStateReady
		b) postmaster run XLOG recovery 
			*) dataState = DataStateInResync 
			*) segmentState = SegmentStateReady
 6)	FileRep Resync backend processes exit when resynchronization is completed 
	ResyncManager set
		*) dataState == DataStateInSync 
		*) segmentState = SegmentStateReady
 
*/ 

#include "postgres.h"

#include <signal.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/twophase.h"
#include "access/slru.h"
#include "cdb/cdbfilerepprimaryrecovery.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbmirroredflatfile.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "postmaster/bgwriter.h"
#include "postmaster/primary_mirror_mode.h"
#include "utils/flatfiles.h"
#include "utils/faultinjector.h"


typedef struct FileRepResyncShmem_s {
	
	volatile int	appendOnlyCommitCount;
	
	bool		checkpointRequest;
	
	bool		reCreateRequest;
	
	bool		readBufferRequest;
		/* allow flushing buffers from buffer pool to primary and mirror from resync worker process */
	
	slock_t		lock;
	
	XLogRecPtr	endFullResyncLSN;
	
	XLogRecPtr	endIncrResyncLSN;
	
	int			writeCount;
	/* number of Write operations waiting to be processed */
	
	//int			resyncWaitingCount; // create + drop + write
	/* NOTE - probably to be removed */
	
	int			resyncInProgressCount; // create + drop + write
	/* NOTE - probably to be removed */
	
	int			resyncCompletedCount; // create + drop + write
	
	HTAB		*fileRepResyncHash;
	/* List of relations to be resynced */
	
} FileRepResyncShmem_s;

static FileRepResyncShmem_s *fileRepResyncShmem = NULL;

static void FileRepResync_ShmemReInit(void);

static void FileRepResync_Cleanup(void);

static int FileRepResyncManager_InResyncTransition(bool isLastLocTracked);
static int FileRepResyncManager_InSyncTransition(void);
static int FileRepPrimary_RunResyncManager(void);

static void FileRepResync_LockAcquire(void);
static void FileRepResync_LockRelease(void);

static int FileRepResync_InsertEntry(
									 FileRepResyncHashEntry_s*	entry);
static FileRepResyncHashEntry_s* FileRepResync_LookupEntry(
													FileName fileName);
static bool FileRepResync_RemoveEntry(
									  FileName	fileName);

static int FileRepResync_CheckProgress(void);

static void
FileRepResync_LockAcquire(void) 
{	
	SpinLockAcquire(&fileRepResyncShmem->lock);
}

static void
FileRepResync_LockRelease(void) 
{	
	SpinLockRelease(&fileRepResyncShmem->lock);
}

void
FileRepResync_IncAppendOnlyCommitCount(void)
{
	if (fileRepResyncShmem != NULL)
	{
		fileRepResyncShmem->appendOnlyCommitCount++;
	}
}

void
FileRepResync_DecAppendOnlyCommitCount(int	count)
{
	if (fileRepResyncShmem != NULL)
	{	
		fileRepResyncShmem->appendOnlyCommitCount -= count;
	
		Assert(fileRepResyncShmem->appendOnlyCommitCount >= 0);
	}		
}

int
FileRepResync_GetAppendOnlyCommitCount(void)
{
	if (fileRepResyncShmem != NULL)
	{
		return fileRepResyncShmem->appendOnlyCommitCount;
	}
	else
	{
		return 0;
	}
}

XLogRecPtr
FileRepResync_GetEndIncrResyncLSN(void)
{
	return fileRepResyncShmem->endIncrResyncLSN;
}

XLogRecPtr
FileRepResync_GetEndFullResyncLSN(void)
{
	return fileRepResyncShmem->endFullResyncLSN;
}

void
FileRepResyncManager_SetEndResyncLSN(XLogRecPtr endResyncLSN)
{
	Assert(! (endResyncLSN.xlogid == 0 && endResyncLSN.xrecoff == 0));
			   
	if (isFullResync())
	{
		fileRepResyncShmem->endFullResyncLSN.xlogid = endResyncLSN.xlogid;
		fileRepResyncShmem->endFullResyncLSN.xrecoff = endResyncLSN.xrecoff;
	}
	else
	{
		fileRepResyncShmem->endIncrResyncLSN.xlogid = endResyncLSN.xlogid;
		fileRepResyncShmem->endIncrResyncLSN.xrecoff = endResyncLSN.xrecoff;		
	}
	
    elog(LOG, "FileRepResync_SetEndResyncLSN() isFullResync:'%d' lsn:'%u'/'%u' '%s'",
		 isFullResync(),
		 endResyncLSN.xlogid,
		 endResyncLSN.xrecoff,
		 XLogLocationToString(&endResyncLSN));

}

void
FileRepResync_ResetReCreateRequest(void)
{
	fileRepResyncShmem->reCreateRequest = FALSE;
}

void
FileRepResync_SetReCreateRequest(void)
{
	fileRepResyncShmem->reCreateRequest = TRUE;
}

void
FileRepResync_ResetReadBufferRequest(void)
{
	fileRepResyncShmem->readBufferRequest = FALSE;
}

void
FileRepResync_SetReadBufferRequest(void)
{
	fileRepResyncShmem->readBufferRequest = TRUE;
}

bool 
FileRepResync_IsTransitionFromResyncToInSync(void)
{
	return((fileRepResyncShmem != NULL)?
		   (fileRepResyncShmem->checkpointRequest == TRUE &&
			fileRepProcessType == FileRepProcessTypeResyncManager):false);
}

bool
FileRepResync_IsReCreate(void)
{
	return((fileRepResyncShmem != NULL)?
		   (fileRepResyncShmem->reCreateRequest == TRUE &&
			fileRepProcessType == FileRepProcessTypeResyncManager):false);	
}

bool
FileRepResync_IsReadBuffer(void)
{
	return((fileRepResyncShmem != NULL)?
		   (fileRepResyncShmem->readBufferRequest == TRUE &&
			FileRepPrimary_IsResyncWorker()):false);	
}

/****************************************************************
 * FILEREP_RESYNC SHARED MEMORY 
 ****************************************************************/

Size
FileRepResync_ShmemSize(void)
{
	Size	size;
	
	size = hash_estimate_size(
							  (Size)FILEREP_MAX_RESYNC_FILES, 
							  sizeof(FileRepResyncHashEntry_s));
	
	size = add_size(size, sizeof(FileRepResyncShmem_s));
		
	return size;	
}

/*
 * Hash table contains 
 * FileName(identifier) is the key in the hash table.
 * Hash table in shared memory is initialized only on primary segment. 
 * It is not initialized on mirror and master host.
 */
void
FileRepResync_ShmemInit(void)
{
	HASHCTL	hash_ctl;
	bool	foundPtr;
	
	fileRepResyncShmem = (FileRepResyncShmem_s *) ShmemInitStruct("FileRep Resync", 
																  sizeof(FileRepResyncShmem_s),
																  &foundPtr);
	  
	if (fileRepResyncShmem == NULL) {
		ereport(ERROR,
		  (errcode(ERRCODE_OUT_OF_MEMORY),
		   (errmsg("Not enough shared memory for Mirroring."))));
	}	
  
	if (! foundPtr) 
	{
		MemSet(fileRepResyncShmem, 0, sizeof(FileRepResyncShmem_s));
	}
	
	fileRepResyncShmem->appendOnlyCommitCount = 0;
	
	fileRepResyncShmem->checkpointRequest = FALSE;
	
	fileRepResyncShmem->reCreateRequest = FALSE;	
	
	fileRepResyncShmem->readBufferRequest = FALSE;
	
	SpinLockInit(&fileRepResyncShmem->lock);
	
	fileRepResyncShmem->endFullResyncLSN.xlogid = 0;
	fileRepResyncShmem->endFullResyncLSN.xrecoff = 0;
	
	fileRepResyncShmem->endIncrResyncLSN.xlogid = 0;
	fileRepResyncShmem->endIncrResyncLSN.xrecoff = 0;
	
	fileRepResyncShmem->writeCount = 0;
	
	fileRepResyncShmem->resyncInProgressCount = 0;
	fileRepResyncShmem->resyncCompletedCount = 0;
	
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = MAXPGPATH;
	hash_ctl.entrysize = sizeof(FileRepResyncHashEntry_s);
	hash_ctl.hash = string_hash;
	
	fileRepResyncShmem->fileRepResyncHash = ShmemInitHash("FileRep Resync Hash",
														  FILEREP_MAX_RESYNC_FILES,
														  FILEREP_MAX_RESYNC_FILES,
														  &hash_ctl,
														  HASH_ELEM | HASH_FUNCTION);
	
	if (fileRepResyncShmem->fileRepResyncHash == NULL) 
	{
		ereport(ERROR, 
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 (errmsg("Not enough shared memory for FileRep Resync."))));
	}
	
	return;						  
}

/*
 *
 */
static void
FileRepResync_ShmemReInit(void)
{
	HASH_SEQ_STATUS				hash_status;
	FileRepResyncHashEntry_s	*entry;
	
	if (fileRepResyncShmem == NULL) {
		ereport(ERROR,
		  (errcode(ERRCODE_OUT_OF_MEMORY),
		   (errmsg("Not enough shared memory for Mirroring."))));
	}	
	
	fileRepResyncShmem->appendOnlyCommitCount = 0;
	
	fileRepResyncShmem->checkpointRequest = FALSE;
	
	fileRepResyncShmem->reCreateRequest = FALSE;	
	
	fileRepResyncShmem->readBufferRequest = FALSE;
	
	SpinLockInit(&fileRepResyncShmem->lock);
	
	fileRepResyncShmem->endFullResyncLSN.xlogid = 0;
	fileRepResyncShmem->endFullResyncLSN.xrecoff = 0;
	
	fileRepResyncShmem->endIncrResyncLSN.xlogid = 0;
	fileRepResyncShmem->endIncrResyncLSN.xrecoff = 0;
	
	fileRepResyncShmem->writeCount = 0;
	
	fileRepResyncShmem->resyncInProgressCount = 0;
	fileRepResyncShmem->resyncCompletedCount = 0;
		
	if (fileRepResyncShmem->fileRepResyncHash == NULL) 
	{
		ereport(ERROR, 
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 (errmsg("Not enough shared memory for FileRep Resync."))));
	}
	
	FileRepResync_LockAcquire();
	
	hash_seq_init(&hash_status, fileRepResyncShmem->fileRepResyncHash);
	
	while ((entry = (FileRepResyncHashEntry_s *) hash_seq_search(&hash_status)) != NULL) {
	
		FileRepResync_RemoveEntry(entry->fileName);
	}
	
	FileRepResync_LockRelease();
	
	return;						  
}

/*
 *
 */
static void
FileRepResync_Cleanup(void)
{
	HASH_SEQ_STATUS				hash_status;
	FileRepResyncHashEntry_s	*entry;
	
	if (fileRepResyncShmem == NULL) {
		return;
	}	
		
	if (fileRepResyncShmem->fileRepResyncHash == NULL) 
	{
		return;
	}
	
	FileRepResync_LockAcquire();
	
	hash_seq_init(&hash_status, fileRepResyncShmem->fileRepResyncHash);
	
	while ((entry = (FileRepResyncHashEntry_s *) hash_seq_search(&hash_status)) != NULL) {
		
		elog(LOG, "LOG(filerep) FileRepResync_Cleanup() mirror relation path:'%s' ",
			 entry->fileName);
			 
		FileRepResync_RemoveEntry(entry->fileName);
		UnlockRelationForResynchronize(
									   &entry->relFileNode, 
									   AccessExclusiveLock);		
	}
	
	FileRepResync_LockRelease();
	
	return;						  
}

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Primary RECOVERY Process)
 ****************************************************************/
/*
 * 
 * FileRepPrimary_StartResyncManager()
 *
 *
 */
void 
FileRepPrimary_StartResyncManager(void)
{	
	int	status = STATUS_OK;
	bool isLastLocTracked = FALSE;
	
	elog(LOG, "FileRepPrimary_StartResyncManager()");
	
	Insist(fileRepRole == FileRepPrimaryRole);

	Insist(dataState == DataStateInResync);
	
	FileRepResync_ShmemReInit();

	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateFault ||
			   
			   (fileRepShmemArray[0]->state == FileRepStateNotInitialized &&
				FileRepSubProcess_GetState() != FileRepStateShutdownBackends &&
			    FileRepSubProcess_GetState() != FileRepStateShutdown &&
			    FileRepSubProcess_GetState() != FileRepStateImmediateShutdown)) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateShutdownBackends ||
			FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
			
			break;
		}
			
		if (segmentState == SegmentStateInitialization)
		{
			
			if (ChangeTracking_RetrieveLastChangeTrackedLoc())
			{
				isLastLocTracked = TRUE;
				FileRepSubProcess_SetState(FileRepStateReady);
				getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, NULL, NULL);
				
				while (FileRepSubProcess_GetState() != FileRepStateShutdown &&
					   FileRepSubProcess_GetState() != FileRepStateShutdownBackends &&
					   FileRepSubProcess_GetState() != FileRepStateImmediateShutdown &&
					   isDatabaseRunning() == FALSE) {
					
					FileRepSubProcess_ProcessSignals();
					pg_usleep(50000L); /* 50 ms */	
				}		
				
				if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
					FileRepSubProcess_GetState() == FileRepStateShutdownBackends ||
					FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
					
					break;
				}				
				
			}
			else
			{
				
				FileRep_SetDataState(DataStateInChangeTracking, FALSE /* signal postmaster */);
				
				if (isFullResync())
				{
					FileRep_SetSegmentState(SegmentStateChangeTrackingDisabled);
				}
				else
				{
					ChangeTracking_CreateInitialFromPreviousCheckpoint();
					FileRepSubProcess_SetState(FileRepStateReady);
				}
				
				/* Wait that XLOG replay is done */
				while (FileRepSubProcess_GetState() != FileRepStateShutdown &&
					   FileRepSubProcess_GetState() != FileRepStateShutdownBackends &&
					   FileRepSubProcess_GetState() != FileRepStateImmediateShutdown &&
					   ! (segmentState == SegmentStateInResyncTransition && 
						  dataState == DataStateInResync)) {
					
					FileRepSubProcess_ProcessSignals();
					pg_usleep(50000L); /* 50 ms */	
				}		

				if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
					FileRepSubProcess_GetState() == FileRepStateShutdownBackends ||
					FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
					
					break;
				}
			}
		}

		Insist(segmentState == SegmentStateInResyncTransition ||
			   segmentState == SegmentStateReady);

		status = FileRepResyncManager_InResyncTransition(isLastLocTracked);
				
		if (status != STATUS_OK ||
			! (FileRepSubProcess_GetState() == FileRepStateReady &&
			   dataState == DataStateInResync))
		{
			continue;
		}		
		
		status = FileRepPrimary_RunResyncManager();

		if (status != STATUS_OK ||
			! (FileRepSubProcess_GetState() == FileRepStateReady &&
			   dataState == DataStateInResync))
		{
			continue;
		}		
		
		status = FileRepResyncManager_InSyncTransition();
		
		if (status != STATUS_OK) 
		{
			continue;
		}
		
		break;
		
	} // while(1)	
	
	elog(LOG, "FileRepPrimary_StartResyncManager() END");

}

/*
 * FileRepResyncManager_InResyncTransition()
 */
static int
FileRepResyncManager_InResyncTransition(bool isLastLocTracked)
{
	int status = STATUS_OK;
	
	elog(LOG, "FileRepResyncManager_InResyncTransition()");

	if (LWLockHeldByMe(MirroredLock))
		elog(ERROR, "FileRep Manager already holding the MirroredLock");
	
	LWLockAcquire(MirroredLock, LW_EXCLUSIVE);
	
	elog(LOG, "FileRepResyncManager_InResyncTransition() ChangeTracking_RecordLastChangeTrackedLoc");

	if (isLastLocTracked == FALSE)
	{
		ChangeTracking_RecordLastChangeTrackedLoc();
		
		if (FileRepSubProcess_GetState() == FileRepStateFault)
		{
			goto exit;
		}
	}
	
	FileRepSubProcess_ProcessSignals();
	if (! ((segmentState == SegmentStateInResyncTransition ||
			segmentState == SegmentStateReady) &&
		   dataState == DataStateInResync))
	{
		goto exit;
	}						
	
	if (isFullResync())
	{
		PersistentFileSysObj_MarkWholeMirrorFullCopy();
		elog(LOG, "FileRepResyncManager_InResyncTransition() PersistentFileSysObj_MarkWholeMirrorFullCopy");

	}
	else
	{
		/*
		 * First, mark the special persistent tables and others as 'Scan Incremental'.
		 *
		 *     These include OIDs {5090, 5091, 5092, 5093, and 5096} and others
		 *     (see GpPersistent_SkipXLogInfo).
		 *
		 * And, mark any Buffer Pool managed relations that were physically truncated as
		 * as 'Scan incremental' because we don't know how to process old changes in
		 * the change tracking log.
		 */
		PersistentFileSysObj_MarkSpecialScanIncremental();

		/*
		 * Second, now mark any relations that have changes in the change tracking
		 * log as 'Page Incremental', except those relations marked 'Scan Incremental' in the
		 * first step.
		 */
		PersistentFileSysObj_MarkPageIncrementalFromChangeLog();

		/*
		 * Finally, find any Append-Only tables that have mirror data loss.
		 */
		PersistentFileSysObj_MarkAppendOnlyCatchup();

		elog(LOG, "FileRepResyncManager_InResyncTransition() PersistentFileSysObj_MarkPageIncrementalFromChangeLog");
	}
	
	FileRepSubProcess_ProcessSignals();
	if (! ((segmentState == SegmentStateInResyncTransition ||
			segmentState == SegmentStateReady) &&
		   dataState == DataStateInResync))
	{
		goto exit;
	}		
	
#ifdef FAULT_INJECTOR	
	FaultInjector_InjectFaultIfSet(
								   FileRepTransitionToInResync, 
								   DDLNotSpecified,
								   "",	// databaseName
								   ""); // tableName
#endif
	
	PersistentFileSysObj_MirrorReCreate();	
	elog(LOG, "FileRepResyncManager_InResyncTransition() PersistentFileSysObj_MirrorReCreate DONE");
	
	FileRepSubProcess_ProcessSignals();
	if (! ((segmentState == SegmentStateInResyncTransition ||
			segmentState == SegmentStateReady) &&
		   dataState == DataStateInResync))
	{
		goto exit;
	}	
	
	if (isLastLocTracked == FALSE)
	{
		ChangeTracking_MarkTransitionToResyncCompleted();

		FileRepSubProcess_ProcessSignals();
		if (! ((segmentState == SegmentStateInResyncTransition ||
				segmentState == SegmentStateReady) &&
			   dataState == DataStateInResync))
		{
			goto exit;
		}	
	}	
	
	FileRepSubProcess_SetState(FileRepStateReady);	
	
	PersistentFileSysObj_MarkMirrorReCreated();
	elog(LOG, "FileRepResyncManager_InResyncTransition() PersistentFileSysObj_MarkMirrorReCreated DONE");
	
exit:
	LWLockRelease(MirroredLock);
	
	return status;
}

/*
 * FileRepPrimary_RunResyncManager()
 */
static int 
FileRepPrimary_RunResyncManager(void)
{
	int		status = STATUS_OK;
	bool	retval;
	int		ii=0;
	bool	mirroredLock = FALSE;
		
	FileRepResyncHashEntry_s	entry;
	ResynchronizeScanToken		token;
	
	elog(LOG, "FileRepPrimary_RunResyncManager()");

	PersistentFileSysObj_MirrorReDrop();
	
	elog(LOG, "FileRepPrimary_RunResyncManager() PersistentFileSysObj_MirrorReDrop DONE");
	
	ResynchronizeScanToken_Init(&token);
	
	while (1) {
		
		FileRepSubProcess_ProcessSignals();
		if (! (FileRepSubProcess_GetState() == FileRepStateReady &&
			   dataState == DataStateInResync))
		{
			break;
		}

		if (FileRepResync_CheckProgress() == FILEREP_MAX_RESYNC_FILES)
		{
			pg_usleep(50000L);  // 50ms
			continue;
		}
						
		retval = PersistentFileSysObj_ResynchronizeScan(
											   &token,
											   &entry.relFileNode,
											   &entry.segmentFileNum,
											   &entry.relStorageMgr,
											   &entry.mirrorDataSynchronizationState,
											   &entry.mirrorBufpoolResyncChangedPageCount,
											   &entry.mirrorBufpoolResyncCkptLoc,
											   &entry.mirrorBufpoolResyncCkptBlockNum,
											   &entry.mirrorAppendOnlyLossEof,											   
											   &entry.mirrorAppendOnlyNewEof, 	
											   &entry.persistentTid,
											   &entry.persistentSerialNum);
		
		if (retval == FALSE)
		{
			/* wait that resync workers are completed */
			while (FileRepResync_CheckProgress() > 0)
			{
				pg_usleep(50000L);  // 50ms
				
				FileRepSubProcess_ProcessSignals();
				if (! (FileRepSubProcess_GetState() == FileRepStateReady &&
					   dataState == DataStateInResync))
				{
					break;
				}				
			}
			
			Assert(FileRepResync_CheckProgress() == 0);
			
			if (mirroredLock == TRUE)
			{
				LWLockRelease(MirroredLock);

				mirroredLock = FALSE;
				break;
			}
			
			if (! (FileRepSubProcess_GetState() == FileRepStateReady &&
				   dataState == DataStateInResync))
			{
				break;
			}				
			
			LWLockAcquire(MirroredLock, LW_EXCLUSIVE);
			mirroredLock = TRUE;
						
			if (fileRepResyncShmem->appendOnlyCommitCount == 0)
			{
				PersistentFileSysObj_MarkAppendOnlyCatchup();
				ResynchronizeScanToken_Init(&token);
								
				/* MirroredLock is not released. AppendOnly Resync has to finish under MirrorLock. */
				continue;
			}
			else
			{
				LWLockRelease(MirroredLock);
				mirroredLock = FALSE;
				/* 
				 * wait that all in progress AppendOnly transactions are committed
				 * and periodically check if more work to do for resync
				 */
				pg_usleep(100000L);  // 100ms
				
				LWLockAcquire(MirroredLock, LW_EXCLUSIVE);
				PersistentFileSysObj_MarkAppendOnlyCatchup();
				LWLockRelease(MirroredLock);
				
				ResynchronizeScanToken_Init(&token);
				continue;
			}
		}
		
		if (entry.mirrorDataSynchronizationState == MirroredRelDataSynchronizationState_DataSynchronized ||
			entry.mirrorDataSynchronizationState == MirroredRelDataSynchronizationState_None)
		{
			continue;
		}
		
		/*
		 * Resynchronize Lock is taken for particular relation to protect from drop and truncate for particular relation.
		 */
		LockRelationForResynchronize(
									 &entry.relFileNode, 
									 AccessExclusiveLock);
		
		if (!PersistentFileSysObj_ResynchronizeRefetch(
													   &entry.relFileNode,
													   &entry.segmentFileNum,
													   &entry.persistentTid,
													   entry.persistentSerialNum,
													   &entry.relStorageMgr,
													   &entry.mirrorDataSynchronizationState,
													   &entry.mirrorBufpoolResyncCkptLoc,
													   &entry.mirrorBufpoolResyncCkptBlockNum,
													   &entry.mirrorAppendOnlyLossEof,											   
													   &entry.mirrorAppendOnlyNewEof))								
		{
				UnlockRelationForResynchronize(
											   &entry.relFileNode, 
											   AccessExclusiveLock);
				continue;
		}
		
#ifdef FAULT_INJECTOR	
		FaultInjector_InjectFaultIfSet(
									   FileRepResync, 
									   DDLNotSpecified,
									   "",	// databaseName
									   ""); // tableName
#endif
		
		FileRep_GetRelationPath(
								entry.fileName, 
								entry.relFileNode, 
								entry.segmentFileNum);
								
		FileRepResync_InsertEntry(&entry);

		elog(LOG, "FileRepPrimary_RunResyncManager() mirror relation path:'%s' index:'%d' "
				 "relStorageMgr:'%s(%d)' mirrorDataSynchronizationState:'%s(%d)' "
				 "ResyncCkptLoc:'%d/%d' "
				 "ResyncCkptBlockNum:'%u' "
				 "AppendOnlyLossEof:" INT64_FORMAT " "
				 "AppendOnlyNewEof:" INT64_FORMAT " "
				 "mirrorBufpoolResyncChangedPageCount:" INT64_FORMAT " ", 
				 entry.fileName, ii++,
				 PersistentFileSysRelStorageMgr_Name(entry.relStorageMgr),
			     entry.relStorageMgr,
				 MirroredRelDataSynchronizationState_Name(entry.mirrorDataSynchronizationState),
				 entry.mirrorDataSynchronizationState,
				 entry.mirrorBufpoolResyncCkptLoc.xlogid,
				 entry.mirrorBufpoolResyncCkptLoc.xrecoff,
				 entry.mirrorBufpoolResyncCkptBlockNum,
				 entry.mirrorAppendOnlyLossEof,
				 entry.mirrorAppendOnlyNewEof,
				 entry.mirrorBufpoolResyncChangedPageCount);
		
		
	}
	
	FileRepResync_Cleanup();
	
	return status;
}

/*
 * FileRepResyncManager_InSyncTransition()
 */
static int 
FileRepResyncManager_InSyncTransition(void)
{	
	int			status = STATUS_OK;
	
	elog(LOG, "FileRepResyncManager_InSyncTransition()");
	
	while (1) {
		/*
		 * (MirroredLock, LW_EXCLUSIVE) is acquired and released in CreateCheckPoint
		 */
		elog(LOG, "FileRepResyncManager_InSyncTransition() RequestCheckpoint() BEGIN");
		
		MirroredFlatFile_DropFilesFromDir();
		
		fileRepResyncShmem->checkpointRequest = TRUE;		
		CreateCheckPoint(false, true);
		fileRepResyncShmem->checkpointRequest = FALSE;
		
		elog(LOG, "FileRepResyncManager_InSyncTransition() RequestCheckpoint() END");
		
		FileRepSubProcess_ProcessSignals();
		if (! (FileRepSubProcess_GetState() == FileRepStateReady &&
			   dataState == DataStateInSync))
		{
			break;
		}				
		FileRepPrimary_MirrorInSyncTransition();
		
		ChangeTracking_MarkTransitionToInsyncCompleted();
		
		break;
	}
		
	return status;
}

/*
 *
 */
void
FileRepResyncManager_ResyncFlatFiles(void)
{
	int status = STATUS_OK;
	
	FileRep_SetSegmentState(SegmentStateInSyncTransition);
	
	elog(LOG, "FileRepResyncManager_ResyncFlatFiles()");
	
#ifdef FAULT_INJECTOR	
	FaultInjector_InjectFaultIfSet(
								   FileRepTransitionToInSync, 
								   DDLNotSpecified,
								   "",	// databaseName
								   ""); // tableName
#endif	
	
	while (1) 
	{
	
		status = XLogRecoverMirrorControlFile();
		elog(LOG, "FileRepResyncManager_ResyncFlatFiles() XLogRecoverMirrorControlFile");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (segmentState != SegmentStateInSyncTransition) {
			break;
		}		
			
		status = FlatFilesRecoverMirror();
		elog(LOG, "FileRepResyncManager_ResyncFlatFiles() FlatFilesRecoverMirror");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (segmentState != SegmentStateInSyncTransition) {
			break;
		}		
		
		status = TwoPhaseRecoverMirror();
		elog(LOG, "FileRepResyncManager_ResyncFlatFiles() TwoPhaseRecoverMirror");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (segmentState != SegmentStateInSyncTransition) {
			break;
		}	
		
		status = SlruRecoverMirror();
		elog(LOG, "FileRepResyncManager_ResyncFlatFiles() SlruRecoverMirror");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (segmentState != SegmentStateInSyncTransition) {
			break;
		}	
		
		status = PgVersionRecoverMirror();
		elog(LOG, "FileRepResyncManager_ResyncFlatFiles() PgVersionRecoverMirror");
		
		if (status != STATUS_OK) {
			break;
		}

		FileRepSubProcess_ProcessSignals();		
		
		break;
	}

	if (status != STATUS_OK) {
		FileRepSubProcess_SetState(FileRepStateFault);
	}
	
	if (segmentState == SegmentStateInSyncTransition &&
		status == STATUS_OK)
	{
		FileRep_SetDataState(DataStateInSync, FALSE);
		
		FileRep_SetSegmentState(SegmentStateReady);		
	}
	
	return;
}

static int
FileRepResync_InsertEntry(
						  FileRepResyncHashEntry_s*	entry)
{
	int							status = STATUS_OK;
	bool						foundPtr;
	FileRepResyncHashEntry_s	*entryLocal;
	char						key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", entry->fileName);
	
	elog(LOG, "FileRepResync_InsertEntry() identifier:'%s'  ", key);
	
	FileRepResync_LockAcquire();
	
	Assert(fileRepResyncShmem->fileRepResyncHash != NULL);
	
	entryLocal = (FileRepResyncHashEntry_s *) hash_search(
														  fileRepResyncShmem->fileRepResyncHash, 
														  (void *) &key, 
														  HASH_ENTER_NULL, 
														  &foundPtr);
	
	if (entryLocal == NULL) {
		
		status = STATUS_ERROR;
		ereport(LOG, 
				(errmsg("ERROR(filerep): could not insert resync information into hash table, no memory "
						"identifier:'%s' ",
						key)));
		goto exit;
	}
	
	if (foundPtr) {
		
		status = STATUS_ERROR;
		ereport(LOG, 
				(errmsg("LOG(filerep): could not insert resync information into hash table, "
						"entry is already inserted "
						"identifier:'%s' ",
						key)));
	} else {
		
		entryLocal->relFileNode.relNode = entry->relFileNode.relNode;
		entryLocal->relFileNode.spcNode = entry->relFileNode.spcNode;
		entryLocal->relFileNode.dbNode = entry->relFileNode.dbNode;
		entryLocal->segmentFileNum = entry->segmentFileNum;
		
		entryLocal->relStorageMgr = entry->relStorageMgr;
		entryLocal->mirrorDataSynchronizationState = entry->mirrorDataSynchronizationState;
		entryLocal->mirrorBufpoolResyncCkptLoc = entry->mirrorBufpoolResyncCkptLoc;
		entryLocal->mirrorBufpoolResyncCkptBlockNum = entry->mirrorBufpoolResyncCkptBlockNum;
		
		entryLocal->mirrorAppendOnlyLossEof = entry->mirrorAppendOnlyLossEof;
		entryLocal->mirrorAppendOnlyNewEof = entry->mirrorAppendOnlyNewEof;
		
		entryLocal->mirrorBufpoolResyncChangedPageCount = entry->mirrorBufpoolResyncChangedPageCount;
		
		entryLocal->persistentTid = entry->persistentTid;
		entryLocal->persistentSerialNum = entry->persistentSerialNum;
								
		entryLocal->fileRepResyncState = FileRepResyncStateInitialized;
		
		fileRepResyncShmem->writeCount++;
		
	}
	
exit:							  
	FileRepResync_LockRelease();
	
	return status;
}


static FileRepResyncHashEntry_s*
FileRepResync_LookupEntry(FileName fileName)
{
	FileRepResyncHashEntry_s * entry;
	char			key[MAXPGPATH+1];
	bool			foundPtr;
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	Assert(fileRepResyncShmem->fileRepResyncHash != NULL);
	
	entry = (FileRepResyncHashEntry_s *) hash_search(
													 fileRepResyncShmem->fileRepResyncHash, 
													 (void *) &key, 
													 HASH_ENTER_NULL, 
													 &foundPtr);
	
	if (entry != NULL) {
		elog(DEBUG1, "FileRepResync_LookupEntry() identifier:'%s'", 
			 fileName);
	} else {
		elog(LOG, "could not find resync entry identifier:'%s' ", fileName);
	}
	
	return entry;							  
	
}

static bool
FileRepResync_RemoveEntry(FileName	fileName)
{
	
	FileRepResyncHashEntry_s	*entry;
	char						key[MAXPGPATH+1];
	bool						isRemoved = FALSE;
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	Assert(fileRepResyncShmem->fileRepResyncHash != NULL);
	
	entry = hash_search(fileRepResyncShmem->fileRepResyncHash, 
						(void *) &key, 
						HASH_REMOVE, 
						NULL);
	
	if (entry) {
		if (Debug_filerep_print)
			elog(LOG, "FileRepResync_RemoveEntry() removed resync entry identifier:'%s' ", fileName);		
		isRemoved = TRUE;
	} 
	
	return isRemoved;
}	

static int
FileRepResync_CheckProgress(void)
{
	FileRepResyncHashEntry_s	*entry = NULL;
	HASH_SEQ_STATUS				hash_status;
	int							countProgress = 0;
	
	FileRepResync_LockAcquire();
	
	Assert(fileRepResyncShmem->fileRepResyncHash != NULL);
	
	if (fileRepResyncShmem->resyncCompletedCount > 0)
	{
	
		hash_seq_init(&hash_status, fileRepResyncShmem->fileRepResyncHash);
		
		while ((entry = (FileRepResyncHashEntry_s *) hash_seq_search(&hash_status)) != NULL) 
		{
			switch (entry->fileRepResyncState)
			{
				case FileRepResyncStateInitialized:
				case FileRepResyncStateInProgress:
					
					break;
					
				case FileRepResyncStateCompleted:
					if (FileRepResync_RemoveEntry(entry->fileName) == TRUE) {
						fileRepResyncShmem->resyncCompletedCount--;	
						Assert(fileRepResyncShmem->resyncCompletedCount >= 0);
					} else {
						Assert(0);
					}
					
					/* Release Resynchronize Lock on relation that was taken before fetching the relation */
					UnlockRelationForResynchronize(
												   &entry->relFileNode, 
												   AccessExclusiveLock);
					
					PersistentFileSysObj_ResynchronizeRelationComplete(
							&entry->persistentTid,
							entry->persistentSerialNum,
							entry->mirrorAppendOnlyNewEof,
							TRUE); 
					
					if (Debug_filerep_print)
						elog(LOG, "FileRepResync_CheckProgress() identifier:'%s' state:'%d' resyncCompletedCount:'%d' ", 
							 entry->fileName,
							 entry->fileRepResyncState,
							 fileRepResyncShmem->resyncCompletedCount);
					
					break;
					
				/* 	FileRepResyncStateFault is not in use */
				case FileRepResyncStateFault:					
				case FileRepResyncStateNotInitialized:
					Assert(0);
					break;
			}
		}
	}

	countProgress = fileRepResyncShmem->resyncCompletedCount + 
					fileRepResyncShmem->writeCount + 
					fileRepResyncShmem->resyncInProgressCount;
	
	FileRepResync_LockRelease();
	
	return countProgress;
}

/* 
 * 
 */
FileRepResyncHashEntry_s*
FileRepPrimary_GetResyncEntry(ChangeTrackingRequest **request)
{
	bool						found = FALSE;
	FileRepResyncHashEntry_s	*entry = NULL;
	HASH_SEQ_STATUS				hash_status;
	int							NumberOfRelations = 0;
	ChangeTrackingRequest		*requestLocal = NULL;
	int64						changedPageCount = 0;
		
	FileRepResync_LockAcquire();
	
	Assert(fileRepResyncShmem->fileRepResyncHash != NULL);
	
	if (fileRepResyncShmem->writeCount > 0) {
		
		hash_seq_init(&hash_status, fileRepResyncShmem->fileRepResyncHash);
		
		while ((entry = (FileRepResyncHashEntry_s *) hash_seq_search(&hash_status)) != NULL) 
		{
			if (entry->fileRepResyncState != FileRepResyncStateInitialized)
			{
				continue;
			}
			
			if (entry->mirrorDataSynchronizationState == MirroredRelDataSynchronizationState_BufferPoolPageIncremental)
			{
				changedPageCount += entry->mirrorBufpoolResyncChangedPageCount;
				
				if (changedPageCount > CHANGETRACKING_MAX_RESULT_SIZE )
				{
					if (NumberOfRelations == 0)
					{
						NumberOfRelations++;
					}
					hash_seq_term(&hash_status);
					break;
				}
				
				NumberOfRelations++;
			}
			else 
			{
				if (NumberOfRelations > 0)
				{	
					/* 
					 * if first entry has BufferPoolPageIncremental state then group only 
					 * relations with BufferPoolPageIncremental for next resync.
					 */
					continue;
				}
				hash_seq_term(&hash_status);
				found = TRUE;
				entry->fileRepResyncState = FileRepResyncStateInProgress;
				fileRepResyncShmem->writeCount--;
				fileRepResyncShmem->resyncInProgressCount++;
				
				Assert(fileRepResyncShmem->writeCount >= 0);
				
				if (Debug_filerep_print) 
					elog(LOG, 
						 "FileRepPrimary_GetResyncEntry() identifier:'%s' "
						 "mirrorDataSynchronizationState:'%s(%d)' ",
						 entry->fileName,
						 MirroredRelDataSynchronizationState_Name(entry->mirrorDataSynchronizationState),
						 entry->mirrorDataSynchronizationState);									
								
				break;
			}
		}  // while()
		
		if (NumberOfRelations > 0)
		{
			int			count = 0;
			XLogRecPtr	endResyncLSN = FileRepResync_GetEndIncrResyncLSN();
			
			requestLocal = ChangeTracking_FormRequest(NumberOfRelations);				
			
			hash_seq_init(&hash_status, fileRepResyncShmem->fileRepResyncHash);
			
			while (count < NumberOfRelations &&
				   (entry = (FileRepResyncHashEntry_s *) hash_seq_search(&hash_status)) != NULL)
			{
				if (entry->fileRepResyncState != FileRepResyncStateInitialized)
				{
					continue;
				}
				
				if (entry->mirrorDataSynchronizationState == MirroredRelDataSynchronizationState_BufferPoolPageIncremental)
				{
					
					/*
					 * When a single SN has more than 64K, it must be called in GetChanges by itself 
					 * (without other relfilenodes). The GetChanges() routine will return the first 64K changes, 
					 * plus an indication that GetChanges() did not finish with this relation. When the consumer 
					 * sees this “not done yet” flag, he needs to call GetChanges() for this relfilenode again 
					 * (when ready) and pass the end lsn from the previous call as the beginning lsn for this call. 
					 * GetChanges() will then return the next 64K changes, etc, etc. 
					 *
					 * If GetChanges() was called with more than 1 relfilenode/SN at a time AND it sees more than 
					 * 64K changes it will be an internal error.
					 */
					if (entry->mirrorBufpoolResyncChangedPageCount > CHANGETRACKING_MAX_RESULT_SIZE)
					{
						Assert(NumberOfRelations == 1);
					}
					
					ChangeTracking_AddRequestEntry(requestLocal, 
												   entry->relFileNode,
												   &entry->mirrorBufpoolResyncCkptLoc,   //beginIncrResyncLSN,
												   &endResyncLSN);	
										
					found = TRUE;
					fileRepResyncShmem->writeCount--;
					fileRepResyncShmem->resyncInProgressCount++;
					entry->fileRepResyncState = FileRepResyncStateInProgress;
					count++;
					
					Assert(fileRepResyncShmem->writeCount >= 0);

					if (Debug_filerep_print) 
						elog(LOG, 
							 "FileRepPrimary_GetResyncEntry() identifier:'%s' NumberOfRelations:'%d' "
							 "mirrorDataSynchronizationState:'%s(%d)' count:'%d' ",
							 entry->fileName,
							 NumberOfRelations,
							 MirroredRelDataSynchronizationState_Name(entry->mirrorDataSynchronizationState),
							 entry->mirrorDataSynchronizationState,
							 count);					
				}
			}
			if (entry != NULL)
			{
				hash_seq_term(&hash_status);
				entry = NULL;
			}
						
			*request = requestLocal;
						
		} // if (NumberOfRelations > 0)
		
	} // if (fileRepResyncShmem->writeCount > 0)
		
	FileRepResync_LockRelease();
	
	if (found == FALSE)
	{
		entry = NULL;
	}
		
	return entry;
}

int
FileRepResync_UpdateEntry(
						  FileRepResyncHashEntry_s*	entry)
{
	FileRepResyncHashEntry_s	*entryLocal;
	int							status = STATUS_OK;
	
	FileRepResync_LockAcquire();
	
	entryLocal = FileRepResync_LookupEntry(entry->fileName);
	
	if (entryLocal != NULL) {
		fileRepResyncShmem->resyncCompletedCount++;
		fileRepResyncShmem->resyncInProgressCount--;
		
		entryLocal->fileRepResyncState = FileRepResyncStateCompleted;
		
		Assert(fileRepResyncShmem->resyncInProgressCount >= 0);

	} else {
		Assert(0);
		status = STATUS_ERROR;
	}
	
	if (Debug_filerep_print)
		elog(LOG, "FileRepResync_UpdateEntry() identifier:'%s' state:'%d' resyncCompletedCount:'%d' ", 
			 entry->fileName,
			 entry->fileRepResyncState,
			 fileRepResyncShmem->resyncCompletedCount);
	
	FileRepResync_LockRelease();
	
	return status;
}
