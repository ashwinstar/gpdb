/*
 *  cdbfilerepmirrorack.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved. *
 */

#include "postgres.h"

#include <signal.h>

#include "cdb/cdbvars.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepmirrorack.h"
#include "cdb/cdbfilerepconnclient.h"
#include "storage/lwlock.h"
#include "utils/faultinjector.h"


/****************************************************************
 * FILE REP ACK SENDER Process definitions
 ****************************************************************/

static int FileRepAckMirror_RunSender(void);
static int FileRepAckMirror_ConstructAndInsertMessage(
					FileRepIdentifier_u		fileRepIdentifier,
					FileRepRelationType_e	fileRepRelationType,
					FileRepOperation_e		fileRepOperation,
					FileRepOperationDescription_u fileRepOperationDescription,
					FileRepAckState_e		fileRepAckState);


/****************************************************************
 * Routines called by CONSUMER PROCESSES 
 ****************************************************************/

/*
 * FileRepPrimary_MirrorOpen
 *		Reserve shared memory
 *		Construct Message Header for open operation
 *		Construct Message Header Crc
 *		Insert Message into Mirror Messages Shared Memory
 *		Set FileRep Message state to Ready, so message can be consumed.
 */
static int 
FileRepAckMirror_ConstructAndInsertMessage(
			 FileRepIdentifier_u	fileRepIdentifier,
			 FileRepRelationType_e	fileRepRelationType,
			 FileRepOperation_e		fileRepOperation,
			 FileRepOperationDescription_u fileRepOperationDescription,
			 FileRepAckState_e		fileRepAckState)
{
	
	FileRepShmemMessageDescr_s	*fileRepShmemMessageDescr;
	FileRepMessageHeader_s		*fileRepMessageHeader;
	pg_crc32					*fileRepMessageHeaderCrc;
	pg_crc32					fileRepMessageHeaderCrcLocal;
	char*						msgPositionInsert = NULL;
	uint32						msgLength;
	uint32						spareField;
	
	msgLength = sizeof(FileRepMessageHeader_s) + sizeof(pg_crc32);
	/*
	 * Shared memory is reserved in advance in order to avoid 
	 * holding lock during memcpy() and Crc calculation...
	 *
	 * Construct FileRepShmemMessageDescr_s 
	 */
	msgPositionInsert = FileRepAck_ReserveShmem(msgLength, &spareField);
	
	if (msgPositionInsert == NULL) {
		ereport(LOG,
				(errmsg("ERROR(filerep): could not queue ack message to primary"),
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   fileRepOperation),
				 FileRep_errdetail_ShmemAck(),
				 FileRep_errcontext()));							

		return STATUS_ERROR;
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
	
	fileRepMessageHeader->fileRepAckState = fileRepAckState;
	
	fileRepMessageHeader->messageCount = spareField;
		
	switch (fileRepOperation) {
		case FileRepOperationReconcileXLogEof:
			fileRepMessageHeader->fileRepOperationDescription = fileRepOperationDescription;
			break;
			
		default:
			break;
	}
	
	/* 
	 * The following fields are not in use during fsync(). 
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
						   "M_ConstructAndInsertMessageAck",
						   fileRepIdentifier,
						   fileRepRelationType,
						   fileRepOperation,
						   fileRepMessageHeaderCrcLocal,
						   FILEREP_UNDEFINED,
						   fileRepAckState,
						   FILEREP_UNDEFINED,
						   fileRepMessageHeader->messageCount);	

#ifdef FILEREP_DEBUG
	ereport(LOG,
			(errmsg("LOG(filerep): M_ConstructAndInsertMessageAck msg header count:'%d' "
					"msg header crc:'%u' position insert:'%p' ack state:'%s' ",
					fileRepMessageHeader->messageCount,
					fileRepMessageHeaderCrcLocal,
					msgPositionInsert,
					FileRepAckStateToString[fileRepAckState]),
			 FileRep_errdetail(fileRepIdentifier,
							   fileRepRelationType,
							   fileRepOperation),
			 FileRep_errdetail_ShmemAck(),
			 FileRep_errcontext()));		
#endif
	

	fileRepShmemMessageDescr = 
	(FileRepShmemMessageDescr_s*) msgPositionInsert;	
	
	fileRepShmemMessageDescr->messageSync = 
				FileRep_IsOperationSynchronous(fileRepOperation);		
	
	fileRepShmemMessageDescr->messageState = FileRepShmemMessageStateReady; 
	
	return STATUS_OK;
}	

/*
 * FileRepAckMirror_MirrorOpen
 */
int 
FileRepAckMirror_Ack(
			FileRepIdentifier_u			fileRepIdentifier,
			FileRepRelationType_e		fileRepRelationType,
			FileRepOperation_e			fileRepOperation,
			FileRepOperationDescription_u fileRepOperationDescription,
			FileRepAckState_e			fileRepAckState)
{
	int status = STATUS_OK;
	
	status = FileRepAckMirror_ConstructAndInsertMessage(
							fileRepIdentifier,
							fileRepRelationType,
							fileRepOperation,
							fileRepOperationDescription,
							fileRepAckState);
	
	return status;
}	

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Ack Mirror SENDER Process)
 ****************************************************************/
/*
 * 
 * FileRepPrimary_StartSender
 */
void 
FileRepAckMirror_StartSender(void)
{
	int status = STATUS_OK;
	
	int retryLocal = 0; 

	char		*primaryHostAddress = NULL;
	/* The IPv4 address of the primary segment. */
	
	char		*mirrorHostAddress = NULL;
	/* The IPv4 address of the mirror. */
	
	int			primaryPort = 0;
	/* The communication port of the primary. Required for listener ACK. */
	
	int			mirrorPort = 0;
	/* The communication port of the mirror. Required for listener. */	
	
	elog(LOG, "FileRepMirror_StartSender() ");
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateSuspend ||
			   FileRepSubProcess_GetState() == FileRepStateInitialization ||
			   FileRepSubProcess_GetState() == FileRepStateFault ||
			   fileRepShmem->state == FileRepStateNotInitialized) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
			
			break;
		}

		primaryMirrorGetFilerepArguments(
							 &fileRepRole,
							 &segmentState,
							 &dataState,
							 &primaryHostAddress,
							 &primaryPort,
							 &mirrorHostAddress,
							 &mirrorPort);

		ereport(LOG,
				(errmsg("LOG(filerep): M_StartSender primary host address:'%s' and port:'%d' "
						"mirror host address:'%s' and port:'%d' ",
						primaryHostAddress,
						primaryPort,
						mirrorHostAddress,
						mirrorPort),
				 FileRep_errcontext()));	
				
		Insist(fileRepRole == FileRepMirrorRole);

		status = FileRepConnClient_EstablishConnection(
									   primaryHostAddress,
									   primaryPort);		

		while (status != STATUS_OK && retryLocal < file_rep_retry)  {
			pg_usleep(500000L); /* 500 ms */
		 
			status = FileRepConnClient_EstablishConnection(
							primaryHostAddress,
							primaryPort);

			retryLocal++;
		}
 
		if (status != STATUS_OK) {
			continue;
		}
		
		status = FileRepAckMirror_RunSender();
		
	} // while(1)
	
	if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
		/* perform graceful shutdown */
	}
	
	FileRepConnClient_CloseConnection();
	
	/* free memory if any required */
	
	return;
}

/*
 * SenderLoop
 *
 */
static int
FileRepAckMirror_RunSender(void)
{
	FileRepShmemMessageDescr_s	*fileRepShmemMessageDescr=NULL;
	char						*fileRepMessage;
	int							status = STATUS_OK;
	bool						movePositionConsume = FALSE;
	
	elog(LOG, "FileRepMirror_RunSender() ");
	
	while (1) {

		LWLockAcquire(FileRepAckShmemLock, LW_EXCLUSIVE);
		
		if (movePositionConsume) {
			
			fileRepAckShmem->positionConsume = 
					fileRepAckShmem->positionConsume +
					fileRepShmemMessageDescr->messageLength + 
					sizeof(FileRepShmemMessageDescr_s);
			
			if (fileRepAckShmem->positionConsume == fileRepAckShmem->positionWraparound &&
				fileRepAckShmem->positionInsert != fileRepAckShmem->positionWraparound) {
				
				fileRepAckShmem->positionConsume = fileRepAckShmem->positionBegin;
				fileRepAckShmem->positionWraparound = fileRepAckShmem->positionEnd;
			}
		}
		
		fileRepShmemMessageDescr = 
		(FileRepShmemMessageDescr_s*) fileRepAckShmem->positionConsume;	
		
		while ((fileRepAckShmem->positionConsume == fileRepAckShmem->positionInsert) ||
			   ((fileRepAckShmem->positionConsume != fileRepAckShmem->positionInsert) &&
				(fileRepShmemMessageDescr->messageState != FileRepShmemMessageStateReady))) {
			
			LWLockRelease(FileRepAckShmemLock);
			
			FileRepSubProcess_ProcessSignals();
			if (FileRepSubProcess_GetState() != FileRepStateReady) {

				LWLockAcquire(FileRepAckShmemLock, LW_EXCLUSIVE);
				break;
			}
			
			pg_usleep(1000L); /* 1 ms */
			
			LWLockAcquire(FileRepAckShmemLock, LW_EXCLUSIVE); 
			
			if (fileRepAckShmem->positionConsume == fileRepAckShmem->positionWraparound &&
				fileRepAckShmem->positionInsert != fileRepAckShmem->positionWraparound) {
				
				fileRepAckShmem->positionConsume = fileRepAckShmem->positionBegin;
				fileRepAckShmem->positionWraparound = fileRepAckShmem->positionEnd;
			}			
			
			/* Re-assign to find if messageState is changed */
			fileRepShmemMessageDescr = 
			(FileRepShmemMessageDescr_s*) fileRepAckShmem->positionConsume;				
		} // while internal
		fileRepAckShmem->consumeCount++;
		
		LWLockRelease(FileRepAckShmemLock); 

		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateReady) {
			break;
		}
	
		FileRep_InsertLogEntry(
							   "M_RunSenderAck",
							   FileRep_GetFlatFileIdentifier("", ""),
							   FileRepRelationNotSpecified,
							   FileRepOperationNotSpecified,
							   FILEREP_UNDEFINED,
							   FILEREP_UNDEFINED,
							   FileRepAckStateNotInitialized,
							   FILEREP_UNDEFINED,
							   FILEREP_UNDEFINED);		
		
#ifdef FAULT_INJECTOR
		FaultInjector_InjectFaultIfSet(
									   FileRepSender,
									   DDLNotSpecified,
									   "",	//databaseName
									   ""); // tableName
#endif						
		
		fileRepMessage = (char*) (fileRepAckShmem->positionConsume + 
								  sizeof(FileRepShmemMessageDescr_s));

		if (! FileRepConnClient_SendMessage(
						FileRepMessageTypeData, 
						fileRepShmemMessageDescr->messageSync,
						fileRepMessage,
						fileRepShmemMessageDescr->messageLength)) {

			ereport(LOG, 
					(errcode_for_socket_access(),
					 errmsg("ERROR(filerep): could not sent ACK message to primary : %m"),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));		
			
			status = STATUS_ERROR;
			break;
		}

		movePositionConsume = TRUE;
	} // while(1)
	
	return status;
}



