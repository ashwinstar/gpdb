/*
 *  cdbfilerepprimary.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved.
 *
 */

/*
 * FileRep process (sender thread) launched by main thread.
 *
 * Responsibilities of this module.
 *		*) 
 *
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "cdb/cdbvars.h"

#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerepconnclient.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "postmaster/primary_mirror_mode.h"
#include "utils/faultinjector.h"
#include "utils/memutils.h"

static volatile sig_atomic_t stateChangeRequested = false;
/* state change informed by SIGUSR1 signal from postmaster */

/* NOTE configure signaling for stateChangeRequested */

static bool FileRepPrimary_IsMirroringRequired(void);

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

/*
 *
 * If (dataState == Change Logging) then mirroring is not required.
 * If (dataState == InSync or InResync) then mirroring is required
 * except when segmentState is in Fault.
 * In that case the request is on hold. It is waiting on third coordinator 
 * to decide about the next action (failover, stop, ...). 
 */
static bool
FileRepPrimary_IsMirroringRequired(void)
{
	bool	isMirroringRequired = FALSE; 
	int		report = FALSE;
	
	while (1) {
/*		if (stateChangeRequested) {
			
			stateChangeRequested = FALSE;
*/			
			getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);
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
		}
		
		switch (dataState) {
			case DataStateNotInitialized:
				/* Insist(0); */
				break;
				
			case DataStateInIncrementalResync:
			case DataStateInFullResync:
			case DataStateInSync:
				if (segmentState != SegmentStateFault) {
					isMirroringRequired = TRUE;
				} else {
					if (! report) {
						ereport(WARNING,
								(errmsg("WARNING(filerep): failure is detected in segment mirroring, "
										"failover requested "),
								FileRep_errcontext()));
						report = TRUE;
					}
					pg_usleep(500000L); /* 500 ms */
					continue;
				}
				break;
				
			case DataStateInChangeLogging:
				if (report) {
					ereport(LOG,
							(errmsg("LOG(filerep): failover is performed to the primary segment, "
									"segment mirroring is suspended "),
							FileRep_errcontext()));
					report = FALSE;
				}	
				/* No Operation */
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
	
	if (status != STATUS_OK) {
		ereport(LOG,			
				(errmsg("ERROR(filerep): could not insert message in ACK table"),
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));	
		
		if (updateConfig(SegmentStateFault)) {
			SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
		}					
		
		FileRepPrimary_IsMirroringRequired();
		return status;
	}	
		
	msgLength = sizeof(FileRepMessageHeader_s) + sizeof(pg_crc32);
/*
 * Shared memory is reserved in advance in order to avoid 
 * holding lock during memcpy() and Crc calculation...
 *
 * Construct FileRepShmemMessageDescr_s 
 */
	 msgPositionInsert = FileRep_ReserveShmem(msgLength, &spareField);
	
	if (msgPositionInsert == NULL) {
		
		status = STATUS_ERROR;
		ereport(LOG,			
				(errmsg("ERROR(filerep): could not queue message to be mirrored"), 
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));	

		if (updateConfig(SegmentStateFault)) {
			SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
		}					
		
		FileRepPrimary_IsMirroringRequired();
		
		return status;
	}
		
/* Construct FileRepMessageHeader_s */
	
	fileRepMessageHeader = 
		(FileRepMessageHeader_s *) (msgPositionInsert + 
		sizeof(FileRepShmemMessageDescr_s));

	fileRepMessageHeader->fileRepMessageHeaderVersion = FileRepMessageHeaderVersionOne;
	
	fileRepMessageHeader->fileRepMessageType = FileRepMessageTypeData;
	
	fileRepMessageHeader->fileRepRelationType = fileRepRelationType;
	
	fileRepMessageHeader->fileRepOperation = fileRepOperation;

	fileRepMessageHeader->fileRepIdentifier = fileRepIdentifier;
	
	fileRepMessageHeader->messageCount = spareField;
	
	switch (fileRepOperation) {
		case FileRepOperationReconcileXLogEof:
		case FileRepOperationOpen:
		case FileRepOperationTruncate:
		case FileRepOperationRename:
			
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
						   	
#ifdef FILEREP_DEBUG
	ereport(LOG,
			(errmsg("LOG(filerep): P_ConstructAndInsertMessage "
					"msg header count:'%d' msg header crc:'%u' position insert:'%p' "
					"msg length:'%u' ",
					fileRepMessageHeader->messageCount,
					fileRepMessageHeaderCrcLocal,
					msgPositionInsert,
					msgLength),
			 FileRep_errdetail(fileRepIdentifier,
							   fileRepRelationType,
							   fileRepOperation),
			 FileRep_errdetail_Shmem(),
			 FileRep_errcontext()));		
#endif
	
	fileRepShmemMessageDescr = 
	(FileRepShmemMessageDescr_s*) msgPositionInsert;	
	
	fileRepShmemMessageDescr->messageSync = 
				FileRep_IsOperationSynchronous(fileRepOperation);		
	
	Assert(status == STATUS_OK);
	
	fileRepShmemMessageDescr->messageState = FileRepShmemMessageStateReady; 	
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
						  int					fileMode)
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
	
	descr.open.fileFlags = fileFlags;

	descr.open.fileMode = fileMode;
	descr.open.logicalEof = logicalEof;
	
	if (FileRepPrimary_IsMirroringRequired()) {
		status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationOpen, 
							  descr);
	}
			
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
	
	if (FileRepPrimary_IsMirroringRequired()) {
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationFlush, 
							  descr);
	}
						
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
			
	if (FileRepPrimary_IsMirroringRequired()) {	
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationClose, 
							  descr);
	}
		
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

	if (FileRepPrimary_IsMirroringRequired()) {	
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationFlushAndClose, 
							  descr);
	}
						
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
								  
				int32 position) 
{
	int status = STATUS_OK;
	
	FileRepOperationDescription_u descr;
	
	descr.truncate.position = position;
	
	if (FileRepPrimary_IsMirroringRequired()) {	
			status = FileRepPrimary_ConstructAndInsertMessage(
									  fileRepIdentifier,
									  fileRepRelationType,
									  FileRepOperationTruncate, 
									  descr);
	}
	
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
	
	if (FileRepPrimary_IsMirroringRequired()) {	
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
	
	if (FileRepPrimary_IsMirroringRequired()) {	
		status = FileRepPrimary_ConstructAndInsertMessage(
											  oldFileRepIdentifier,
											  fileRepRelationType,
											  FileRepOperationRename, 
											  descr);
	}
	
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

	if (FileRepPrimary_IsMirroringRequired()) {	
			
		while (dataLength > 0) {
			
			dataLengthLocal = Min(FILEREP_MESSAGEBODY_LEN, dataLength);
			
			msgLength = sizeof(FileRepMessageHeader_s) + sizeof(pg_crc32) + dataLengthLocal;
			/*
			 * Shared memory is reserved in advance in order to avoid 
			 * holding lock during memcpy() and Crc calculation...
			 *
			 * Construct FileRepShmemMessageDescr_s 
			 */
			msgPositionInsert = FileRep_ReserveShmem(msgLength, &spareField);
			
			if (msgPositionInsert == NULL) {
				
				status = STATUS_ERROR;
				ereport(LOG,
						(errmsg("ERROR(filerep): could not queue message to be mirrored"), 
						 FileRep_errdetail(fileRepIdentifier,
										   fileRepRelationType,
										   FileRepOperationWrite),
						 FileRep_errdetail_Shmem(),
						 FileRep_errcontext()));	
				
				if (updateConfig(SegmentStateFault)) {
					SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
				}					
				
				FileRepPrimary_IsMirroringRequired();

				break;
			}
			
			/* Construct FileRepMessageHeader_s */
			
			fileRepMessageHeader = 
					(FileRepMessageHeader_s *) (msgPositionInsert + 
					sizeof(FileRepShmemMessageDescr_s));
			
			fileRepMessageHeader->fileRepMessageHeaderVersion = FileRepMessageHeaderVersionOne;
			
			fileRepMessageHeader->fileRepMessageType = FileRepMessageTypeDataHeaderAndBody;
			
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
						
#ifdef FILEREP_DEBUG
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
#endif
			
			
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
			
			/* the order should not be changed. */
			data = (char *) data + dataLengthLocal;		
			if (offset != FILEREP_OFFSET_UNDEFINED) {
				offset += dataLengthLocal;	
			}
			dataLength -= dataLengthLocal;
					
		} // while()			

	}
				
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

	if (FileRepPrimary_IsMirroringRequired()) {		
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationCreate,
							  descr);
	}
			
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
			
	if (FileRepPrimary_IsMirroringRequired()) {						
			status = FileRepPrimary_ConstructAndInsertMessage(
							  fileRepIdentifier,
							  fileRepRelationType,
							  FileRepOperationDrop,
							  descr);
	}

	return status;
}

bool 
FileRepPrimary_IsOperationCompleted(
			   FileRepIdentifier_u	 fileRepIdentifier,
			   FileRepRelationType_e fileRepRelationType)
{
	bool retval = TRUE;
	
	if (FileRepPrimary_IsMirroringRequired()) {	
			retval = FileRepAckPrimary_IsOperationCompleted(
							fileRepIdentifier,									
						    fileRepRelationType);
			/* suspend if mirror reported fault */
		if (retval == FALSE) {
			ereport(LOG,
					(errmsg("ERROR(filerep): could not complete operation on mirror"), 
					 FileRep_errdetail(fileRepIdentifier,
									   fileRepRelationType,
									   FileRepOperationNotSpecified), 
					 FileRep_errdetail_Shmem(),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));	
			
			if (updateConfig(SegmentStateFault)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}			
			
			FileRepPrimary_IsMirroringRequired();
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
	
	char		*primaryHostAddress = NULL;
	/* The IPv4 address of the primary segment. */
	
	char		*mirrorHostAddress = NULL;
	/* The IPv4 address of the mirror. */
	
	int			primaryPort = 0;
	/* The communication port of the primary. Required for listener ACK. */
	
	int			mirrorPort = 0;
	/* The communication port of the mirror. Required for listener. */
	
	elog(LOG, "FileRepPrimary_StartSender() ");
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateSuspend ||
			   FileRepSubProcess_GetState() == FileRepStateFault) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
		
			break;
		}
		
		Assert(FileRepSubProcess_GetState() == FileRepStateInitialization);
		
		primaryMirrorGetFilerepArguments(
						 &fileRepRole,
						 &segmentState,
						 &dataState,
						 &primaryHostAddress,
						 &primaryPort,
						 &mirrorHostAddress,
						 &mirrorPort);
		
		ereport(LOG,
				(errmsg("LOG(filerep): P_StartSender primary host address:'%s' and port:'%d' "
						"mirror host address:'%s' and port:'%d' ",
						primaryHostAddress,
						primaryPort,
						mirrorHostAddress,
						mirrorPort),
				FileRep_errcontext()));	

		Insist(fileRepRole == FileRepPrimaryRole);
		Insist(dataState == DataStateInSync ||
			   dataState == DataStateInIncrementalResync ||
			   dataState == DataStateInFullResync);
		
		status = FileRepConnClient_EstablishConnection(
								mirrorHostAddress,
								mirrorPort);
		if (status != STATUS_OK) {
			continue;
		}
		
		status = FileRepPrimary_RunSender();
		
	} // while(1)
	
	if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
	 /* perform graceful shutdown */
	}
		
	FileRepConnClient_CloseConnection();	
	/* free memory if any allocated*/
	
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
		}

		fileRepShmemMessageDescr = 
		(FileRepShmemMessageDescr_s*) fileRepShmem->positionConsume;	
		
		while ((fileRepShmem->positionConsume == fileRepShmem->positionInsert) ||
			   ((fileRepShmem->positionConsume != fileRepShmem->positionInsert) &&
				(fileRepShmemMessageDescr->messageState != FileRepShmemMessageStateReady))) {
			
			LWLockRelease(FileRepShmemLock);

			FileRepSubProcess_ProcessSignals();
			if (FileRepSubProcess_GetState() != FileRepStateReady &&
				FileRepSubProcess_GetState() != FileRepStateInitialization) {
				
				LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE); 
				break;
			}
			
			pg_usleep(1000L); /* 1 ms */
			
			LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
			
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
		
#ifdef FILEREP_DEBUG
		ereport(LOG,
				(errmsg("LOG(filerep): P_RunSender msg header count:'%d' local count:'%d' ",
						fileRepMessageHeader->messageCount,
						spare),
				 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
								   fileRepMessageHeader->fileRepRelationType,
								   fileRepMessageHeader->fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));		
#endif
		
		
		spare = fileRepMessageHeader->messageCount;
	
		if (! FileRepConnClient_SendMessage(
					FileRepMessageTypeData, 
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
	
	return status;
}





