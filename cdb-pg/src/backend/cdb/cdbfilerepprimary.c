/*
 *  cdbfilerepprimary.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved.
 *
 */

/*
 * FileRep process (sender thread) launched by main thread.
 * Responsibilities of this module.
 *		*) 
 *
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include "cdb/cdbvars.h"

#include "access/xlog_internal.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerepconnclient.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "postmaster/primary_mirror_mode.h"
#include "utils/faultinjector.h"
#include "utils/memutils.h"

static volatile sig_atomic_t stateChangeRequested = false;
/* state change informed by SIGUSR1 signal from postmaster */

/* NOTE configure signaling for stateChangeRequested */

static bool FileRepPrimary_IsMirroringRequired(FileRepRelationType_e fileRepRelationType);

/****************************************************************
 * SENDER THREAD definitions
 ****************************************************************/

static int FileRepPrimary_RunSender(void);
static int FileRepPrimary_ConstructAndInsertMessage(
				FileRepIdentifier_u		fileRepIdentifier,
				FileRepRelationType_e	fileRepRelationType,
				FileRepOperation_e		fileRepOperation,
				FileRepOperationDescription_u fileRepOperationDescription);

/****************************************************************
 * Routines called by BACKEND PROCESSES 
 ****************************************************************/

bool
FileRepPrimary_IsResyncWorker(void)
{
	return(
		fileRepProcessType == FileRepProcessTypeResyncWorker1 ||
		fileRepProcessType == FileRepProcessTypeResyncWorker2 ||
		fileRepProcessType == FileRepProcessTypeResyncWorker3 ||
		fileRepProcessType == FileRepProcessTypeResyncWorker4);
}

bool
FileRepPrimary_IsResyncManagerOrWorker(void)
{		
	return(
		   fileRepProcessType == FileRepProcessTypeResyncManager ||
		   fileRepProcessType == FileRepProcessTypeResyncWorker1 ||
		   fileRepProcessType == FileRepProcessTypeResyncWorker2 ||
		   fileRepProcessType == FileRepProcessTypeResyncWorker3 ||
		   fileRepProcessType == FileRepProcessTypeResyncWorker4);
}

MirrorDataLossTrackingState 
FileRepPrimary_GetMirrorDataLossTrackingSessionNum(int64 *sessionNum)
{
    
    *sessionNum = getChangeTrackingSessionId();
    
    getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, NULL, NULL);
    
	switch (dataState) {
		case DataStateNotInitialized:
			return MirrorDataLossTrackingState_MirrorNotConfigured;
			
		case DataStateInResync:
			return MirrorDataLossTrackingState_MirrorCurrentlyUpInResync;
			
		case DataStateInSync:
			return MirrorDataLossTrackingState_MirrorCurrentlyUpInSync;
			
		case DataStateInChangeTracking:
			return MirrorDataLossTrackingState_MirrorDown;

		default:
			Assert(0);
			return MirrorDataLossTrackingState_MirrorNotConfigured;
	}	
	return MirrorDataLossTrackingState_MirrorNotConfigured;
}

void 
FileRepPrimary_IntentAppendOnlyCommitWork(void)
{
	FileRepResync_IncAppendOnlyCommitCount();
}

void 
FileRepPrimary_FinishedAppendOnlyCommitWork(int count)
{
	FileRepResync_DecAppendOnlyCommitCount(count);
}

/*
 *
 * If (dataState == Change Logging) then mirroring is not required.
 * If (dataState == InSync or InResync) then mirroring is required
 * except when segmentState is in Fault.
 * In that case the request is on hold. It is waiting on third coordinator 
 * to decide about the next action (failover, stop, ...). 
 */
static bool
FileRepPrimary_IsMirroringRequired(FileRepRelationType_e fileRepRelationType)
{
	bool	isMirroringRequired = FALSE; 
	int		report = FALSE;
	bool	isInTransition = FALSE;
	DataState_e dataStateTransition;
	
	while (1) {
/*		if (stateChangeRequested) {
			
			stateChangeRequested = FALSE;
*/			
			getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, &isInTransition, &dataStateTransition);
/*		} */
		
		switch (fileRepRole) {
			case FileRepRoleNotInitialized:
				Insist(0);
				break;
			case FileRepMirrorRole:
				Insist(0);
				break;
			case FileRepPrimaryRole:
				Insist(dataState != DataStateNotInitialized);
				break;
			case FileRepNoRoleConfigured:
				Insist(dataState == DataStateNotInitialized);
				break;
            default:
                Insist(0);
                break;
		}
		
		switch (dataState) {
			case DataStateNotInitialized:
				/* Insist(0); */
				break;
				
			case DataStateInResync:
				if (! FileRepResync_IsReCreate())
				{
					if (segmentState == SegmentStateInitialization ||
						segmentState == SegmentStateInResyncTransition) 
					{
						break;
					}
				}
				/* no break */
			case DataStateInSync:
				if (segmentState != SegmentStateFault &&
					!(isInTransition == TRUE &&
					  dataStateTransition == DataStateInChangeTracking)) 
				{
					isMirroringRequired = TRUE;
				} else {
					if (! report) {
						ereport(LOG,
								(errmsg("WARNING(filerep): failure is detected in segment mirroring, "
										"failover requested "),
								FileRep_errcontext()));
						report = TRUE;
					}
					pg_usleep(500000L); /* 500 ms */
					continue;
				}
				break;
				
			case DataStateInChangeTracking:
				if (segmentState == SegmentStateInitialization ||
					(segmentState == SegmentStateInChangeTrackingTransition &&
					 fileRepRelationType != FileRepRelationTypeFlatFile))
				{
					pg_usleep(500000L); /* 500 ms */
					continue;
				}
				if (report) {
					ereport(LOG,
							(errmsg("LOG(filerep): failover is performed to the primary segment, "
									"segment mirroring is suspended "),
							 errSendAlert(true),
							FileRep_errcontext()));
					report = FALSE;
				}	
				/* No Operation */
				break;
            default:
                Insist(0);
                break;
		}
		break;
	} // while (1)
	
	return isMirroringRequired;
}

/*
 * FileRepPrimary_ConstructAndInsertMessage
 *		Reserve shared memory
 *		Construct Message Header for open operation
 *		Construct Message Header Crc
 *		Insert Message into Mirror Messages Shared Memory
 *		Set FileRep Message state to Ready, so message can be consumed.
 */
static int 
FileRepPrimary_ConstructAndInsertMessage(
				FileRepIdentifier_u		fileRepIdentifier,
				FileRepRelationType_e	fileRepRelationType,
				FileRepOperation_e		fileRepOperation,
				FileRepOperationDescription_u fileRepOperationDescription)
{

	FileRepShmemMessageDescr_s	*fileRepShmemMessageDescr;
	FileRepMessageHeader_s		*fileRepMessageHeader;
	pg_crc32					*fileRepMessageHeaderCrc;
	pg_crc32					fileRepMessageHeaderCrcLocal;
	char*						msgPositionInsert = NULL;
	uint32						msgLength;
	int							status = STATUS_OK;
	uint32						spareField;
	
	if (FileRep_IsOperationSynchronous(fileRepOperation) == TRUE) { 
		
		status = FileRepAckPrimary_NewHashEntry(
												fileRepIdentifier,
												fileRepOperation,
												fileRepRelationType);
	}
	
	if (status != STATUS_OK && dataState != DataStateInChangeTracking) {
		ereport(LOG,			
				(errmsg("ERROR(filerep): could not insert message in ACK table"),
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));	
		
		if (updateSegmentState(SegmentStateFault)) {
			SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
		}					
		
		FileRepPrimary_IsMirroringRequired(fileRepRelationType);
		return status;
	}	
		
	msgLength = sizeof(FileRepMessageHeader_s) + sizeof(pg_crc32);
/*
 * Shared memory is reserved in advance in order to avoid 
 * holding lock during memcpy() and Crc calculation...
 *
 * Construct FileRepShmemMessageDescr_s 
 */
	 msgPositionInsert = FileRep_ReserveShmem(
											  fileRepShmemArray[fileRepProcIndex], 
											  msgLength, 
											  &spareField,
											  fileRepOperation);
	
	if (msgPositionInsert == NULL && dataState != DataStateInChangeTracking) {
		
		status = STATUS_ERROR;
		ereport(LOG,			
				(errmsg("ERROR(filerep): could not queue message to be mirrored"), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));	

		if (updateSegmentState(SegmentStateFault)) {
			SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
		}					
		
		FileRepPrimary_IsMirroringRequired(fileRepRelationType);
		
		return status;
	}
		
/* Construct FileRepMessageHeader_s */
	
	fileRepMessageHeader = 
		(FileRepMessageHeader_s *) (msgPositionInsert + 
		sizeof(FileRepShmemMessageDescr_s));

	fileRepMessageHeader->fileRepMessageHeaderVersion = FileRepMessageHeaderVersionOne;
	
	fileRepMessageHeader->fileRepRelationType = fileRepRelationType;
	
	fileRepMessageHeader->fileRepOperation = fileRepOperation;

	fileRepMessageHeader->fileRepIdentifier = fileRepIdentifier;
	
	fileRepMessageHeader->messageCount = spareField;
	
	switch (fileRepOperation) {
		case FileRepOperationReconcileXLogEof:
		case FileRepOperationOpen:
		case FileRepOperationTruncate:
		case FileRepOperationRename:
		case FileRepOperationHeartBeat:
		case FileRepOperationCreateAndOpen:
			
			fileRepMessageHeader->fileRepOperationDescription = fileRepOperationDescription;
			break;
			
		default:
			break;
	}
		
	/* 
	 * The following fields are not in use during open(). 
	 * They were zeroed during reserving message buffer.  
	 *		a) fileRepOperationDescription
	 *		b) fileRepMessageBodyCrc
	 *		c) messageCount
	 */
	
/* Construct FileRepShmemMessageHeaderCrc */
	
	fileRepMessageHeaderCrc =
		(pg_crc32 *) (msgPositionInsert + 
		sizeof(FileRepMessageHeader_s) + 
		sizeof(FileRepShmemMessageDescr_s));
								 
	FileRep_CalculateCrc((char *) fileRepMessageHeader,
						 sizeof(FileRepMessageHeader_s),
						 &fileRepMessageHeaderCrcLocal);
	
	*fileRepMessageHeaderCrc = fileRepMessageHeaderCrcLocal;
	
	FileRep_InsertLogEntry(
						   "P_ConstructAndInsertMessage",
						   fileRepIdentifier,
						   fileRepRelationType,
						   fileRepOperation,
						   fileRepMessageHeaderCrcLocal,
						   FILEREP_UNDEFINED,
						   FileRepAckStateNotInitialized,
						   FILEREP_UNDEFINED,
						   fileRepMessageHeader->messageCount);
						   	
	if (Debug_filerep_print)
		ereport(LOG,
			(errmsg("LOG(filerep): P_ConstructAndInsertMessage "
					"msg header count:'%d' msg header crc:'%u' position insert:'%p' "
					"msg length:'%u' open file flags:'%x' "
					"truncate position:'" INT64_FORMAT "' ",
					fileRepMessageHeader->messageCount,
					fileRepMessageHeaderCrcLocal,
					msgPositionInsert,
					msgLength,
					(fileRepOperation == FileRepOperationOpen || fileRepOperation == FileRepOperationCreateAndOpen)?
					fileRepOperationDescription.open.fileFlags:0,
					(fileRepOperation == FileRepOperationTruncate)?
					fileRepOperationDescription.truncate.position:0),
			 FileRep_errdetail(fileRepIdentifier,
							   fileRepRelationType,
							   fileRepOperation),
			 FileRep_errdetail_Shmem(),
			 FileRep_errcontext()));		

	
	fileRepShmemMessageDescr = 
	(FileRepShmemMessageDescr_s*) msgPositionInsert;	
	
	fileRepShmemMessageDescr->messageSync = 
				FileRep_IsOperationSynchronous(fileRepOperation);		
	
	Assert(status == STATUS_OK);
	
	fileRepShmemMessageDescr->messageState = FileRepShmemMessageStateReady; 
	
	LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
	FileRep_IpcSignal(fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->semC, 
					  fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->refCountSemC);
	LWLockRelease(FileRepShmemLock);
	
	return status;
}	

/*
 * FileRepPrimary_MirrorOpen
 */
int 
FileRepPrimary_MirrorOpen(FileRepIdentifier_u	fileRepIdentifier,
						  FileRepRelationType_e	fileRepRelationType,
						  int64					logicalEof,
						  int					fileFlags,
						  int					fileMode,
						  bool					suppressError)
{
	int					status = STATUS_OK;
	FileRepOperation_e	fileRepOperation = FileRepOperationOpen; 
	bool				mirrorCreateAndOpen = FALSE;
	FileRepOperationDescription_u descr;
	
	descr.open.fileFlags = fileFlags;

	descr.open.fileMode = fileMode;
	descr.open.logicalEof = logicalEof;
	descr.open.suppressError = suppressError;
		
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{
			if ((fileFlags & O_CREAT) == O_CREAT)
			{
				fileRepOperation = FileRepOperationCreateAndOpen;
				mirrorCreateAndOpen = TRUE;
			}
			
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  fileRepOperation, 
							  descr);
			if (status != STATUS_OK)
			{
				mirrorCreateAndOpen = FALSE;
			}	
		}
	}
	
	if (mirrorCreateAndOpen) 
	{
		if (FileRepPrimary_IsOperationCompleted(
												fileRepIdentifier,									
												fileRepRelationType) ==  FALSE)
		{
			status = STATUS_ERROR;
		}
	}
	
#ifdef FILEREP_DEBUG
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
			(errmsg("LOG(filerep): Change Tracking "), 
			 FileRep_errdetail(fileRepIdentifier,
							   fileRepRelationType,
							   fileRepOperation),
			 FileRep_errcontext()));	
#endif	
			
	return status;
}	

/*
 * FileRepPrimary_MirrorFlush
 */
int 
FileRepPrimary_MirrorFlush(FileRepIdentifier_u		fileRepIdentifier,
						   FileRepRelationType_e	fileRepRelationType)
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
	
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationFlush, 
							  descr);
		}
	}

#ifdef FILEREP_DEBUG
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationFlush),
				 FileRep_errcontext()));	
#endif	
	return status;
}

/*
 * FileRepPrimary_MirrorClose
 */
int 
FileRepPrimary_MirrorClose(FileRepIdentifier_u		fileRepIdentifier,
						   FileRepRelationType_e	fileRepRelationType)
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
			
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{				
			status = FileRepPrimary_ConstructAndInsertMessage(
						  fileRepIdentifier,
						  fileRepRelationType,
						  FileRepOperationClose, 
						  descr);
		}
	}

#ifdef FILEREP_DEBUG
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationClose),
				 FileRep_errcontext()));	
#endif
	
	return status;	
}

/*
 * FileRepPrimary_MirrorFlushAndClose
 */
int 
FileRepPrimary_MirrorFlushAndClose(FileRepIdentifier_u		fileRepIdentifier,
								   FileRepRelationType_e	fileRepRelationType)
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;

	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationFlushAndClose, 
							  descr);
		}
	}

#ifdef FILEREP_DEBUG
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationFlushAndClose),
				 FileRep_errcontext()));	
#endif	
	return status;	
}

/*
 * FileRepPrimary_MirrorTruncate
 */
int 
FileRepPrimary_MirrorTruncate(
				FileRepIdentifier_u	fileRepIdentifier,
				  /* */
								  
				FileRepRelationType_e fileRepRelationType,
				  /* */
								  
				int64 position) 
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
	
	descr.truncate.position = position;
	
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			status = FileRepPrimary_ConstructAndInsertMessage(
									  fileRepIdentifier,
									  fileRepRelationType,
									  FileRepOperationTruncate, 
									  descr);
		}
	}
	
#ifdef FILEREP_DEBUG	
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationTruncate),
				 FileRep_errcontext()));	
#endif	
	
	return status;	
}


/*
 * FileRepPrimary_ReconcileXLogEof
 */
int FileRepPrimary_ReconcileXLogEof(
						FileRepIdentifier_u		fileRepIdentifier,
						FileRepRelationType_e	fileRepRelationType,
						XLogRecPtr				primaryXLogEof)
{

	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
	
	descr.reconcile.xLogEof = primaryXLogEof;
	
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) {	
		status = FileRepPrimary_ConstructAndInsertMessage(
								  fileRepIdentifier,
								  fileRepRelationType,
								  FileRepOperationReconcileXLogEof, 
								  descr);
	}
	
	return status;		
}	

/*
 * FileRepPrimary_Rename
 */
int FileRepPrimary_MirrorRename(
									FileRepIdentifier_u		oldFileRepIdentifier,
									FileRepIdentifier_u		newFileRepIdentifier,
									FileRepRelationType_e	fileRepRelationType)
{
	
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
	
	descr.rename.fileRepIdentifier = newFileRepIdentifier;
	
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			status = FileRepPrimary_ConstructAndInsertMessage(
											  oldFileRepIdentifier,
											  fileRepRelationType,
											  FileRepOperationRename, 
											  descr);
		}
	}
	
#ifdef FILEREP_DEBUG	
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(oldFileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationRename),
				 FileRep_errcontext()));	
#endif	
	
	return status;		
}	


/*
 * FileRepPrimary_MirrorWrite
 */
int
FileRepPrimary_MirrorWrite(FileRepIdentifier_u		fileRepIdentifier,
						   FileRepRelationType_e	fileRepRelationType,
						   int32					offset,
						   char						*data, 
						   uint32					dataLength,
						   XLogRecPtr				lsn)
{	
	FileRepShmemMessageDescr_s	*fileRepShmemMessageDescr;
	FileRepMessageHeader_s		*fileRepMessageHeader;
	pg_crc32					*fileRepMessageHeaderCrc;
	pg_crc32					fileRepMessageHeaderCrcLocal;
	char						*fileRepMessageBody;
	char*						msgPositionInsert = NULL;
	uint32						msgLength;
	uint32						dataLengthLocal;
	int							status = STATUS_OK;
	uint32						spareField;

	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			
		while (dataLength > 0) {
			
			dataLengthLocal = Min(FILEREP_MESSAGEBODY_LEN, dataLength);
			
			msgLength = sizeof(FileRepMessageHeader_s) + sizeof(pg_crc32) + dataLengthLocal;
			/*
			 * Shared memory is reserved in advance in order to avoid 
			 * holding lock during memcpy() and Crc calculation...
			 *
			 * Construct FileRepShmemMessageDescr_s 
			 */
			msgPositionInsert = FileRep_ReserveShmem(
													 fileRepShmemArray[fileRepProcIndex],
													 msgLength, 
													 &spareField,
													 FileRepOperationWrite);
			
			if (msgPositionInsert == NULL && dataState != DataStateInChangeTracking) {
				
				status = STATUS_ERROR;
				ereport(LOG,
						(errmsg("ERROR(filerep): could not queue message to be mirrored"), 
						 FileRep_errdetail(fileRepIdentifier,
										   fileRepRelationType,
										   FileRepOperationWrite),
						 FileRep_errdetail_Shmem(),
						 FileRep_errcontext()));	
				
				if (updateSegmentState(SegmentStateFault)) {
					SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
				}					
				
				FileRepPrimary_IsMirroringRequired(fileRepRelationType);

				break;
			}
			
			/* Construct FileRepMessageHeader_s */
			
			fileRepMessageHeader = 
					(FileRepMessageHeader_s *) (msgPositionInsert + 
					sizeof(FileRepShmemMessageDescr_s));
			
			fileRepMessageHeader->fileRepMessageHeaderVersion = FileRepMessageHeaderVersionOne;
			
			fileRepMessageHeader->fileRepRelationType = fileRepRelationType;
			
			fileRepMessageHeader->fileRepOperation = FileRepOperationWrite;

			fileRepMessageHeader->fileRepIdentifier = fileRepIdentifier;
			
			fileRepMessageHeader->messageCount = spareField;
			
			fileRepMessageHeader->fileRepOperationDescription.write.offset = offset;
			fileRepMessageHeader->fileRepOperationDescription.write.dataLength = dataLengthLocal;
			fileRepMessageHeader->fileRepOperationDescription.write.lsn = lsn;
			
			/* Construct fileRepMessageBodyCrc */
			FileRep_CalculateCrc((char *) data,
								 dataLengthLocal,
								 &fileRepMessageHeader->fileRepMessageBodyCrc);
			
			/* 
			 * The following fields are not in use during open(). 
			 * They were zeroed during reserving message buffer.  
			 *		a) fileRepOperationDescription
			 *		c) messageCount
			 */
			
			/* Construct fileRepMessageHeaderCrc */
			
			fileRepMessageHeaderCrc =
			(pg_crc32 *) (msgPositionInsert + 
						sizeof(FileRepMessageHeader_s) + 
						sizeof(FileRepShmemMessageDescr_s));
			
			FileRep_CalculateCrc((char *) fileRepMessageHeader,
								 sizeof(FileRepMessageHeader_s),
								 &fileRepMessageHeaderCrcLocal);
			
			*fileRepMessageHeaderCrc = fileRepMessageHeaderCrcLocal;
			
			FileRep_InsertLogEntry(
								   "P_ConstructAndInsertMessage",
								   fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationWrite,
								   fileRepMessageHeaderCrcLocal,
								   fileRepMessageHeader->fileRepMessageBodyCrc,
								   FileRepAckStateNotInitialized,
								   FILEREP_UNDEFINED,
								   fileRepMessageHeader->messageCount);			
						
			if (Debug_filerep_print)
				ereport(LOG,
					(errmsg("LOG(filerep): P_ConstructAndInsertMessage "
							"msg header count:'%d' msg header crc:'%u' position insert:'%p' "
							"msg body crc:'%u' ",
							fileRepMessageHeader->messageCount,
							fileRepMessageHeaderCrcLocal,
							msgPositionInsert,
							fileRepMessageHeader->fileRepMessageBodyCrc),
					 FileRep_errdetail(fileRepIdentifier,
									   fileRepRelationType,
									   FileRepOperationWrite),
					 FileRep_errdetail_Shmem(),
					 FileRep_errcontext()));					
			
			/* Copy Data */
			fileRepMessageBody = (char *) (msgPositionInsert + 
								sizeof(FileRepMessageHeader_s) + 
								sizeof(FileRepShmemMessageDescr_s) +
								sizeof(pg_crc32));

			memcpy(fileRepMessageBody, data, dataLengthLocal);
						
			fileRepShmemMessageDescr = 
			(FileRepShmemMessageDescr_s*) msgPositionInsert;	
			
			fileRepShmemMessageDescr->messageSync = 
					FileRep_IsOperationSynchronous(FileRepOperationWrite);		

			fileRepShmemMessageDescr->messageState = FileRepShmemMessageStateReady; 
			
			LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
			FileRep_IpcSignal(fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->semC, 
							  fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->refCountSemC);
			LWLockRelease(FileRepShmemLock);
			
			/* the order should not be changed. */
			data = (char *) data + dataLengthLocal;		
			if (offset != FILEREP_OFFSET_UNDEFINED) {
				offset += dataLengthLocal;	
			}
			dataLength -= dataLengthLocal;
					
		} // while()			
		} // if
	}
	
#ifdef FILEREP_DEBUG		
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationWrite),
				 FileRep_errcontext()));	
#endif
	
	return status;
}

/*
 * FileRepPrimary_MirrorCreate
 */
int 
FileRepPrimary_MirrorCreate(FileRepIdentifier_u		fileRepIdentifier,
							FileRepRelationType_e	fileRepRelationType)
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;

	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationCreate,
							  descr);
		}
	}
	
#ifdef FILEREP_DEBUG
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationCreate),
				 FileRep_errcontext()));	
#endif	
			
	return status;	
}

/*
 * FileRepPrimary_MirrorDrop
 */
int 
FileRepPrimary_MirrorDrop(FileRepIdentifier_u	fileRepIdentifier,
						  FileRepRelationType_e	fileRepRelationType)
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
			
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationDrop,
							  descr);
		}
	}

#ifdef FILEREP_DEBUG
	if (dataState == DataStateInChangeTracking)
		ereport(LOG,			
				(errmsg("LOG(filerep): Change Tracking "), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   FileRepOperationDrop),
				 FileRep_errcontext()));	
#endif
	
	return status;
}

/*
 * FileRepPrimary_MirrorShutdown
 *	inform mirror about graceful shutdown
 */
void 
FileRepPrimary_MirrorShutdown(void)
{
	int status = STATUS_OK;
	bool retval = TRUE;
	FileRepIdentifier_u	fileRepIdentifier;
	FileRepOperationDescription_u descr;
	
	fileRepIdentifier = FileRep_GetUnknownIdentifier("shutdown");

    /* see if we should send the shutdown to the mirror */
	bool shouldSendShutdownToMirror = false;
    getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, NULL, NULL);
	if ( dataState == DataStateInSync )
	{
        shouldSendShutdownToMirror = true;
	}
    else if ( dataState == DataStateInResync &&
	      segmentState != SegmentStateInitialization &&
	      segmentState != SegmentStateInResyncTransition )
    {
        shouldSendShutdownToMirror = true;
    }

    /* send graceful shutdown if required */
	if ( shouldSendShutdownToMirror )
	{
        status = FileRepPrimary_ConstructAndInsertMessage(
															  fileRepIdentifier,
															  FileRepRelationTypeUnknown,
															  FileRepOperationShutdown,
															  descr);
		
		retval = FileRepAckPrimary_IsOperationCompleted(
											   fileRepIdentifier,									
											   FileRepRelationTypeUnknown);
		if (retval == FALSE && dataState != DataStateInChangeTracking) {
			ereport(LOG,
					(errmsg("WARNING(filerep): could not complete operation on mirror"), 
					 FileRep_errdetail(fileRepIdentifier,
									   FileRepRelationTypeUnknown,
									   FileRepOperationShutdown), 
					 FileRep_errdetail_Shmem(),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));	
			
		}
		
	}
	
	return;
}

/*
 * FileRepPrimary_MirrorInSyncTransition
 * transition mirror from InResync to InSync dataState
 */
void 
FileRepPrimary_MirrorInSyncTransition(void)
{
	int status = STATUS_OK;
	bool retval = TRUE;
	FileRepIdentifier_u	fileRepIdentifier;
	FileRepOperationDescription_u descr;
	
	fileRepIdentifier = FileRep_GetUnknownIdentifier("inSyncTransition");
	
	if (FileRepPrimary_IsMirroringRequired(FileRepRelationTypeUnknown)) 
	{
		status = FileRepPrimary_ConstructAndInsertMessage(
														  fileRepIdentifier,
														  FileRepRelationTypeUnknown,
														  FileRepOperationInSyncTransition,
														  descr);
		
		retval = FileRepAckPrimary_IsOperationCompleted(
														fileRepIdentifier,									
														FileRepRelationTypeUnknown);
		if (retval == FALSE && dataState != DataStateInChangeTracking) {
			ereport(LOG,
					(errmsg("ERROR(filerep): could not complete operation on mirror"), 
					 FileRep_errdetail(fileRepIdentifier,
									   FileRepRelationTypeUnknown,
									   FileRepOperationInSyncTransition), 
					 FileRep_errdetail_Shmem(),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));	
			
			if (updateSegmentState(SegmentStateFault)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}						
		}
		
	}
	
	return;
}

/*
 * FileRepPrimary_MirrorHeartBeat
 * verify that flow from primary to mirror and back is alive
 */
void 
FileRepPrimary_MirrorHeartBeat(FileRepConsumerProcIndex_e index)
{
	int status = STATUS_OK;
	bool retval = TRUE;
	FileRepIdentifier_u	fileRepIdentifier;
	FileRepOperationDescription_u descr;
	
	descr.heartBeat.fileRepConsumerProcIndex = index;
	
	fileRepIdentifier = FileRep_GetUnknownIdentifier("heartBeat");
	
	if (FileRepPrimary_IsMirroringRequired(FileRepRelationTypeUnknown)) 
	{
		status = FileRepPrimary_ConstructAndInsertMessage(
														  fileRepIdentifier,
														  FileRepRelationTypeUnknown,
														  FileRepOperationHeartBeat,
														  descr);
		
		retval = FileRepAckPrimary_IsOperationCompleted(
														fileRepIdentifier,									
														FileRepRelationTypeUnknown);
		if (retval == FALSE && dataState != DataStateInChangeTracking) {
			ereport(LOG,
					(errmsg("ERROR(filerep): could not complete operation on mirror"), 
					 FileRep_errdetail(fileRepIdentifier,
									   FileRepRelationTypeUnknown,
									   FileRepOperationNotSpecified), 
					 FileRep_errdetail_Shmem(),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));	
			
			if (updateSegmentState(SegmentStateFault)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}			
		}		
	}
	
	return;
}

bool 
FileRepPrimary_IsOperationCompleted(
			   FileRepIdentifier_u	 fileRepIdentifier,
			   FileRepRelationType_e fileRepRelationType)
{
	bool retval = TRUE;
	
	if (FileRepPrimary_IsMirroringRequired(fileRepRelationType)) 
	{
		if (! (dataState == DataStateInResync &&
			   fileRepRelationType == FileRepRelationTypeFlatFile &&
			   segmentState != SegmentStateInSyncTransition))
		{			
			retval = FileRepAckPrimary_IsOperationCompleted(
							fileRepIdentifier,									
						    fileRepRelationType);
				/* suspend if mirror reported fault */
			if (retval == FALSE && dataState != DataStateInChangeTracking) {
				ereport(LOG,
						(errmsg("ERROR(filerep): could not complete operation on mirror"), 
						 FileRep_errdetail(fileRepIdentifier,
										   fileRepRelationType,
										   FileRepOperationNotSpecified), 
						 FileRep_errdetail_Shmem(),
						 FileRep_errdetail_ShmemAck(),
						 FileRep_errcontext()));	
				
				if (updateSegmentState(SegmentStateFault)) {
					SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
				}			
				
				FileRepPrimary_IsMirroringRequired(fileRepRelationType);
			}
		}
	}
	
	return retval;
	
}

XLogRecPtr
FileRepPrimary_GetMirrorXLogEof(void) 
{
	return FileRepAckPrimary_GetMirrorXLogEof();
}

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Primary SENDER Process)
 ****************************************************************/
/*
 * Primary Sender process is responsible for
 *
 *		b) Data Messages
 *				*) data written by backend
 *			
 */


/*
 * 
 * FileRepPrimary_StartSender
 * check hash_create for allocating memory context
 */
void 
FileRepPrimary_StartSender(void)
{
	int status = STATUS_OK;
	
	elog(LOG, "FileRepPrimary_StartSender() ");
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateFault) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
		
			break;
		}
		
		Assert(FileRepSubProcess_GetState() == FileRepStateInitialization);
		
		ereport(LOG,
				(errmsg("LOG(filerep): P_StartSender primary host address:'%s' and port:'%d' "
						"mirror host address:'%s' and port:'%d', fullResync %s ",
						fileRepPrimaryHostAddress,
						fileRepPrimaryPort,
						fileRepMirrorHostAddress,
						fileRepMirrorPort,
						fileRepFullResync ? "true" : "false"),
				FileRep_errcontext()));	

		Insist(fileRepRole == FileRepPrimaryRole);
		Insist(dataState == DataStateInSync ||
			   dataState == DataStateInResync);
		
		status = FileRepConnClient_EstablishConnection(
								fileRepMirrorHostAddress,
								fileRepMirrorPort);
		if (status != STATUS_OK) {
			continue;
		}
		
		status = FileRepPrimary_RunSender();
		
	} // while(1)
	
	FileRepConnClient_CloseConnection();	
		
	return;
}

/*
 * SenderLoop
 *
 */
static int
FileRepPrimary_RunSender(void)
{
	FileRepShmemMessageDescr_s	*fileRepShmemMessageDescr=NULL;
	char						*fileRepMessage;
	int							status = STATUS_OK;
	bool						movePositionConsume = FALSE;
	uint32						spare = 0;
	FileRepMessageHeader_s		*fileRepMessageHeader;
	FileRepShmem_s				*fileRepShmem = fileRepShmemArray[fileRepProcIndex];
	FileRepConsumerProcIndex_e	messageType = FileRepMessageTypeUndefined;
	
	elog(LOG, "FileRepPrimary_RunSender() ");
	
	while (1) {
		
		LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
		
		if (movePositionConsume) {
		
			fileRepShmem->positionConsume = 
					fileRepShmem->positionConsume +
					fileRepShmemMessageDescr->messageLength + 
					sizeof(FileRepShmemMessageDescr_s);
			
			if (fileRepShmem->positionConsume == fileRepShmem->positionWraparound &&
				fileRepShmem->positionInsert != fileRepShmem->positionWraparound) {
				
				fileRepShmem->positionConsume = fileRepShmem->positionBegin;
				fileRepShmem->positionWraparound = fileRepShmem->positionEnd;
			}
			
			FileRep_IpcSignal(fileRepIpcArray[fileRepShmem->ipcArrayIndex]->semP,
							  fileRepIpcArray[fileRepShmem->ipcArrayIndex]->refCountSemP);
		}

		fileRepShmemMessageDescr = 
		(FileRepShmemMessageDescr_s*) fileRepShmem->positionConsume;	
		
		while ((fileRepShmem->positionConsume == fileRepShmem->positionInsert) ||
			   ((fileRepShmem->positionConsume != fileRepShmem->positionInsert) &&
				(fileRepShmemMessageDescr->messageState != FileRepShmemMessageStateReady))) {
			
			fileRepIpcArray[fileRepShmem->ipcArrayIndex]->refCountSemC++;

			LWLockRelease(FileRepShmemLock);

			FileRepSubProcess_ProcessSignals();
			if (FileRepSubProcess_GetState() != FileRepStateReady &&
				FileRepSubProcess_GetState() != FileRepStateInitialization) {
				
				LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE); 
				break;
			}
			
			FileRep_IpcWait(fileRepIpcArray[fileRepShmem->ipcArrayIndex]->semC);
			
			//pg_usleep(1000L); /* 1 ms */
			
			LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
			
			fileRepIpcArray[fileRepShmem->ipcArrayIndex]->refCountSemC--;
			
			if (fileRepShmem->positionConsume == fileRepShmem->positionWraparound &&
				fileRepShmem->positionInsert != fileRepShmem->positionWraparound) {	
				
				fileRepShmem->positionConsume = fileRepShmem->positionBegin;
				fileRepShmem->positionWraparound = fileRepShmem->positionEnd;
			}			
			
			/* Re-assign to find if messageState is changed */
			fileRepShmemMessageDescr = 
			(FileRepShmemMessageDescr_s*) fileRepShmem->positionConsume;				
		}
		fileRepShmem->consumeCount++;
		
		LWLockRelease(FileRepShmemLock);
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateReady &&
			FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}
				
#ifdef FAULT_INJECTOR	
		FaultInjector_InjectFaultIfSet(
									   FileRepSender, 
									   DDLNotSpecified,
									   "",	// databaseName
									   ""); // tableName
#endif
		
		fileRepMessage = (char*) (fileRepShmem->positionConsume + 
								  sizeof(FileRepShmemMessageDescr_s));
		
		
		fileRepMessageHeader = (FileRepMessageHeader_s*) (fileRepShmem->positionConsume + 
														  sizeof(FileRepShmemMessageDescr_s));

		/* next two IF will be removed */
		if (fileRepMessageHeader->messageCount <= spare) {
			elog(LOG, "ERROR(filerep): FileRepPrimary_RunSender() proc:%d spare:%d spareLocal:%d consume:%p insert:%p wraparound:%p", 
				 getpid(),
				 fileRepMessageHeader->messageCount,
				 spare,
				 fileRepShmem->positionConsume,
				 fileRepShmem->positionInsert,
				 fileRepShmem->positionWraparound);
		} 
		
		if (fileRepMessageHeader->messageCount > spare + 5) {
			elog(LOG, "ERROR(filerep): FileRepPrimary_RunSender() proc:%d spare:%d spareLocal:%d consume:%p insert:%p wraparound:%p", 
				 getpid(),
				 fileRepMessageHeader->messageCount,
				 spare,
				 fileRepShmem->positionConsume,
				 fileRepShmem->positionInsert,
				 fileRepShmem->positionWraparound); 
		} 

		FileRep_InsertLogEntry(
							   "P_RunSender",
							   fileRepMessageHeader->fileRepIdentifier,
							   fileRepMessageHeader->fileRepRelationType,
							   fileRepMessageHeader->fileRepOperation,
							   FILEREP_UNDEFINED,
							   fileRepMessageHeader->fileRepMessageBodyCrc,
							   FileRepAckStateNotInitialized,
							   spare,
							   fileRepMessageHeader->messageCount);					
		
		if (Debug_filerep_print)
			ereport(LOG,
				(errmsg("LOG(filerep): P_RunSender msg header count:'%d' local count:'%d' ",
						fileRepMessageHeader->messageCount,
						spare),
				 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
								   fileRepMessageHeader->fileRepRelationType,
								   fileRepMessageHeader->fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));				
		
		spare = fileRepMessageHeader->messageCount;
	
		switch (fileRepMessageHeader->fileRepRelationType)
		{
			case FileRepRelationTypeUnknown:
				switch (fileRepMessageHeader->fileRepOperation)
				{
					case FileRepOperationShutdown:
						messageType = FileRepMessageTypeShutdown;
						break;
					
					case FileRepOperationInSyncTransition:
						messageType = FileRepMessageTypeXLog;
						break;
						
					case FileRepOperationHeartBeat:
						messageType = fileRepMessageHeader->fileRepOperationDescription.heartBeat.fileRepConsumerProcIndex;
						break;
						
					default:
						Assert(0);
						break;
				}
				break;
			case FileRepRelationTypeFlatFile:
			case FileRepRelationTypeBulk:
				messageType = FileRepMessageTypeXLog;
				break;
				
			case FileRepRelationTypeAppendOnly:
				messageType = FileRepMessageTypeAO01;
				/* messageType = FileRepMessageTypeAO02; */
				break;
				
			case FileRepRelationTypeBufferPool:
			case FileRepRelationTypeDir:
				messageType = FileRepMessageTypeWriter;
				break;
				
			default:
				elog(LOG, "ERROR(filerep): P_RunSender unknown relation type:'%d' ",
					 fileRepMessageHeader->fileRepRelationType);
				Assert(0);
				break;
		}
		
		if (! FileRepConnClient_SendMessage(
					messageType, 
					fileRepShmemMessageDescr->messageSync,
					fileRepMessage,
					fileRepShmemMessageDescr->messageLength)) {
			
			ereport(LOG, 
					(errcode_for_socket_access(),
					 errmsg("ERROR(filerep): could not sent message to mirror msg header count:'%d' local count:'%d' : %m",
							fileRepMessageHeader->messageCount,
							spare),
					 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
									   fileRepMessageHeader->fileRepRelationType,
									   fileRepMessageHeader->fileRepOperation),
					 FileRep_errdetail_Shmem(),
					 FileRep_errcontext()));		

			status = STATUS_ERROR;
			break;
		}

		movePositionConsume = TRUE;
	} // while(1)
	
	FileRepConnClient_CloseConnection();
	
	return status;
}





