/*
 *  cdbfilerepprimaryack.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved. *
 */
#include "postgres.h"

#include <signal.h>

#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "cdb/cdbvars.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepconnserver.h"
#include "libpq/pqsignal.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/faultinjector.h"
#include "utils/hsearch.h"
#include "utils/ps_status.h"
#include "utils/memutils.h"


/*
 * It specifies fields maintained by each entry in hash table
 * of acks.
 *
 * Only SYNC messages are entered into hash table
 * NOTE Do we need to maintain caller? For TXs that shares the same file 
 * taht will be required.
 */
typedef struct FileRepAckHashEntry_s {
	char				fileName[MAXPGPATH+1];  
	/* It is key. It has to be first field in the structure. */
	
	FileRepOperation_e		fileRepOperation;
	/* File operation that is acknowledged. It is required for consistency check. */
	
	FileRepRelationType_e	fileRepRelationType;
	/* Type of the file. It is required for consistency check. */
	
	FileRepAckState_e		fileRepAckState;
		/* state of the ack */
	
	XLogRecPtr				xLogEof;
		/* logical xlog EOF, required for xlog reconcile */
	
} FileRepAckHashEntry_s;


static HTAB *fileRepAckHash = NULL;

static XLogRecPtr xLogEof = {0, 0};

/*
 * Find if acknowledgement is received from mirror.
 * Access is protected by shared  lock.
 */
static FileRepAckHashEntry_s* FileRepAckPrimary_LookupHashEntry(FileName fileName);
/*
 * Insert entry with info related to ack in hash table.
 * Access is protected by exclusive lock.
 */
static FileRepAckHashEntry_s* FileRepAckPrimary_InsertHashEntry(
								  FileName		fileName, 
								  bool			*exists);

/*
 * Remove entry from hash table.
 * Return TRUE if ack was removed.
 * Access is protected by exclusive lock.
 */
static bool FileRepAckPrimary_RemoveHashEntry(FileName  fileName);


static int FileRepAckPrimary_UpdateHashEntry(
					 FileRepIdentifier_u	fileRepIdentifier,
					 FileRepRelationType_e	fileRepRelationType,
					 FileRepAckState_e		fileRepAckState);

static int FileRepAckPrimary_RunReceiver(void);

static int FileRepAckPrimary_RunConsumer(void);


/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Ack Primary RECEIVER Process)
 ****************************************************************/
/*
 * 
 * FileRepAckPrimary_StartReceiver
 */
void 
FileRepAckPrimary_StartReceiver(void)
{	
	char			*primaryHostAddress = NULL;
	/* The IPv4 address of the primary segment. */
	
	char			*mirrorHostAddress = NULL;
	/* The IPv4 address of the mirror. */
	
	int			primaryPort = 0;
	/* The communication port of the primary. Required for listener ACK. */
	
	int			mirrorPort = 0;
	/* The communication port of the mirror. Required for listener. */
		
	int status = STATUS_OK;	
	
	int retval;
	
	elog(LOG, "FileRepAckPrimary_StartReceiver() ");

	primaryMirrorGetFilerepArguments(
									 &fileRepRole,
									 &segmentState,
									 &dataState,
									 &primaryHostAddress,
									 &primaryPort,
									 &mirrorHostAddress,
									 &mirrorPort);
		
	ereport(LOG,
			(errmsg("LOG(filerep): P_StartReceiver primary host address:'%s' and port:'%d' "
					"mirror host address:'%s' and port:'%d' ",
					primaryHostAddress,
					primaryPort,
					mirrorHostAddress,
					mirrorPort),
			 FileRep_errcontext()));	
	
	Insist(fileRepRole == FileRepPrimaryRole);
	
	status = FileRepConnServer_StartListener(
								 primaryHostAddress,
								 primaryPort);
	
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

		PG_SETMASK(&BlockSig);
		retval = FileRepConnServer_Select();	
		PG_SETMASK(&UnBlockSig);

		/* 
		 * check and process any signals received 
		 * The routine returns TRUE if the received signal requests
		 * process shutdown.
		 */
		if (FileRepSubProcess_ProcessSignals()) {
			continue;
		}
		
		if (retval < 0) {
			status = STATUS_ERROR;
			continue;
		}
		
		if (retval == 0) {
			continue;
		}
		
		Assert(retval > 0);
		
		status = FileRepConnServer_CreateConnection();
		
		if (status != STATUS_OK) {
			continue;
		}				
		
		status = FileRepConnServer_ReceiveStartupPacket();
		if (status != STATUS_OK) {
			continue;
		} 
		
		fileRepShmemArray[0]->state = FileRepStateInitialization;
		
		status = FileRepAckPrimary_RunReceiver();
		
	} // while(1)
		
	if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
		/* perform graceful shutdown */
	}
	
	/*FileRepConnServer_CloseConnection();*/
	
	/* free memory */
	
	return;
}


/*
 *
 */
static int
FileRepAckPrimary_RunReceiver(void)
{
	uint32_t				msgLength = 0;
	FileRepConsumerProcIndex_e	msgType;
	int						status = STATUS_OK;
	char					*msgPositionInsert;
	FileRepShmemMessageDescr_s  *fileRepShmemMessageDescr;
	uint32					spareField;
	
	elog(DEBUG1, "FileRepAckPrimary_RunReceiver()");
	
	while (1) {
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateReady &&
			FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}
		
		if ( ! FileRepConnServer_AwaitMessageBegin()) {
			/* call was interrupted ... go back to beginning to process signals */
			continue;
		}

		status = FileRepConnServer_ReceiveMessageType(&msgType);
		
		if (status != STATUS_OK) {
			break;
		}
				
		/* DATA MESSAGE TYPE */
		status = FileRepConnServer_ReceiveMessageLength(&msgLength);
		
		if (status != STATUS_OK) {
			break;
		}

		msgPositionInsert = FileRepAck_ReserveShmem(msgLength, /* not used */ &spareField);
		
		if (msgPositionInsert == NULL) {
			
			status = STATUS_ERROR;
			ereport(LOG,
					(errmsg("ERROR(filerep): could not queue ack message to be processed"),
					 FileRep_errdetail_Shmem(),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));							
			break;
		}
		
		status = FileRepConnServer_ReceiveMessageData(
						msgPositionInsert + sizeof(FileRepShmemMessageDescr_s),
						msgLength);
		
		if (status != STATUS_OK) {
			break;
		}		
		
#ifdef FAULT_INJECTOR
		FaultInjector_InjectFaultIfSet(
									   FileRepReceiver,
									   DDLNotSpecified,
									   "",	//databaseName
									   ""); // tableName
#endif				
		
		fileRepShmemMessageDescr = 
		(FileRepShmemMessageDescr_s*) msgPositionInsert;	
		
		/* it is not in use */
		fileRepShmemMessageDescr->messageSync = FALSE;
		
		fileRepShmemMessageDescr->messageState = FileRepShmemMessageStateReady; 
		
		FileRep_InsertLogEntry(
							   "P_RunReceiver",
							   FileRep_GetFlatFileIdentifier("", ""),
							   FileRepRelationNotSpecified,
							   FileRepOperationNotSpecified,
							   FILEREP_UNDEFINED,
							   FILEREP_UNDEFINED,
							   FileRepAckStateNotInitialized,
							   spareField,
							   FILEREP_UNDEFINED);			
				
	} // while(1)
	
	return status;
}

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Ack Primary CONSUMER Process)
 ****************************************************************/

Size
FileRepAckPrimary_ShmemSize(void)
{
	Size	size;
	
	size = hash_estimate_size(
				(Size)FILEREP_MAX_OPEN_FILES, 
				sizeof(FileRepAckHashEntry_s));
	
	return size;	
}

/*
 * Initialize hash table of open files in private memory of FileRep process.
 * FileName of the file is the key of the hash table.
 * FileName is relative file path from $PGDATA directory.
 */
void
FileRepAckPrimary_ShmemInit(void)
{
	HASHCTL	hash_ctl;
	
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = MAXPGPATH+1;
	hash_ctl.entrysize = sizeof(FileRepAckHashEntry_s);
	hash_ctl.hash = string_hash;
	
	fileRepAckHash = ShmemInitHash("FileRep Ack Hash",
								   FILEREP_MAX_OPEN_FILES,
								   FILEREP_MAX_OPEN_FILES,
								   &hash_ctl,
								   HASH_ELEM | HASH_FUNCTION);
	
	if (fileRepAckHash == NULL) {
		ereport(ERROR, 
				(errcode(ERRCODE_OUT_OF_MEMORY),
				(errmsg("Not enough shared memory for Mirroring."))));
	}
	
	return;						  
}

/*
 * lookup ack entry
 */
static FileRepAckHashEntry_s*
FileRepAckPrimary_LookupHashEntry(FileName fileName)
{
	FileRepAckHashEntry_s	*entry;
	char					key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	Assert(fileRepAckHash != NULL);
		
	entry = (FileRepAckHashEntry_s *) hash_search(
										  fileRepAckHash, 
										  (void *) &key, 
										  HASH_FIND, 
										  NULL);
	
	if (entry == NULL) {
		ereport(LOG,
				(errmsg("ERROR(filerep): could not find ACK entry in hash table identifier:'%s' ",
						fileName),
				FileRep_errcontext()));
	} 

	return entry;
}

/*
 * insert ack entry in hash table by caller of the mirror operation
 */ 
static FileRepAckHashEntry_s*
FileRepAckPrimary_InsertHashEntry(
			 FileName		fileName, 
			 bool			*exists)
{
	
	bool					foundPtr;
	FileRepAckHashEntry_s	*entry;
	char					key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	Assert(fileRepAckHash != NULL);
	
	entry = (FileRepAckHashEntry_s *) hash_search(
										  fileRepAckHash, 
										  (void *) &key, 
										  HASH_ENTER_NULL, 
										  &foundPtr);
	
	if (entry == NULL) {
		*exists = FALSE;
		return entry;
	} 
	
	elog(DEBUG1, "FileRepAckPrimary_InsertHashEntry() entry_key:%s", 
		 entry->fileName);
	
	if (foundPtr) {
		*exists = TRUE;
	} else {
		*exists = FALSE;
	}
		
	return entry;
}

/*
 * 
 *
 * fileName is relative path from $PGDATA directory
 */
static bool
FileRepAckPrimary_RemoveHashEntry(FileName  fileName)
{	
	/* NOTE check behavior when two users have concurrently open files */
	
	FileRepAckHashEntry_s	*entry;
	bool					isRemoved = FALSE;
	char					key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", fileName);
		
	Assert(fileRepAckHash != NULL);
		
	entry = (FileRepAckHashEntry_s *) hash_search(
										fileRepAckHash, 
										(void *) &key, 
										HASH_REMOVE, 
										NULL);
	
	if (entry) {
		elog(DEBUG1, "FileRepAckPrimary_RemoveHashEntry() Removed hash entry entry_key:%s", entry->fileName);
		isRemoved = TRUE;
	}
				
	return isRemoved;			
}
				
/*
 * It is called by backend process to insert new ack entry into hash table. 
 */		
int 
FileRepAckPrimary_NewHashEntry(
					FileRepIdentifier_u		fileRepIdentifier, 
					FileRepOperation_e		fileRepOperation,
					FileRepRelationType_e	fileRepRelationType)
{

	FileRepAckHashEntry_s	*entry=NULL;
	bool					exists;
	FileName				fileName = NULL;
	int						status = STATUS_OK;
	int						retry = 0;
	
	fileName = FileRep_GetFileName(fileRepIdentifier, fileRepRelationType);
		
	while (retry < file_rep_retry) {
		LWLockAcquire(FileRepAckHashShmemLock, LW_EXCLUSIVE);
		
		entry = FileRepAckPrimary_InsertHashEntry(fileName, &exists);
					
		if (entry == NULL) {
			LWLockRelease(FileRepAckHashShmemLock);
			status = STATUS_ERROR;
			ereport(LOG,
					(errmsg("ERROR(filerep): could not insert ACK entry into table, no memory"),
					 FileRep_errdetail(fileRepIdentifier,
									   fileRepRelationType,
									   fileRepOperation),
					 FileRep_errcontext()));							
			goto exit;
		}
					
		if (exists) {
			LWLockRelease(FileRepAckHashShmemLock);
			
			FileRep_Sleep1ms(retry);
			retry++;
			continue;
		}

		entry->fileRepOperation = fileRepOperation;
		entry->fileRepRelationType = fileRepRelationType;
		entry->fileRepAckState = FileRepAckStateWaiting;
		entry->xLogEof = xLogEof;
		
		LWLockRelease(FileRepAckHashShmemLock);
		break;
	}

	if (retry == file_rep_retry) {
		status = STATUS_ERROR;
		ereport(LOG,
				(errmsg("ERROR(filerep): could not insert ACK entry into table, entry already exists"),
				 FileRep_errdetail(fileRepIdentifier,
								   fileRepRelationType,
								   fileRepOperation),
				 FileRep_errcontext()));		
	} else {
	
		elog(DEBUG1, "FileRepAckPrimary_NewHashEntry() %s", entry->fileName);
	}
	
exit:
	
	if (fileName) {
		pfree(fileName);
		fileName = NULL;
	}		
	
	return status;			
}
/*
 * Consumer process on Primary update the state in the hash entry 
 */		
static int 
FileRepAckPrimary_UpdateHashEntry(
					FileRepIdentifier_u		fileRepIdentifier,
					FileRepRelationType_e	fileRepRelationType,
					FileRepAckState_e		fileRepAckState)
{

	FileRepAckHashEntry_s	*entry;
	bool					exists;
	int						status = STATUS_OK;
	FileName				fileName = NULL;
	
	fileName = FileRep_GetFileName(fileRepIdentifier, fileRepRelationType);
	
	LWLockAcquire(FileRepAckHashShmemLock, LW_EXCLUSIVE);
	
	entry = FileRepAckPrimary_InsertHashEntry(fileName, &exists);
				
	/* entry should be found since ack has not been processed yet */			
	Assert(entry != NULL);
				
	if (!exists) {
		LWLockRelease(FileRepAckHashShmemLock);
		status = STATUS_ERROR;
		ereport(LOG,
				(errmsg("ERROR(filerep): could not update ACK entry with ACK status, no entry found "
						"identifier:'%s' relation type:'%s' ",
						fileName,
						FileRepRelationTypeToString[fileRepRelationType]),
				 FileRep_errcontext()));		
		goto exit;
	}

	entry->fileRepAckState = fileRepAckState;
	
	entry->xLogEof = xLogEof;
	
	LWLockRelease(FileRepAckHashShmemLock);
	
#ifdef FILEREP_DEBUG	
	ereport(LOG,
			(errmsg("LOG(filerep): P_UpdateHashEntry identifier:'%s' ack state:'%s' ",
					entry->fileName, 
					FileRepAckStateToString[entry->fileRepAckState])));
#endif

exit:	
	if (fileName) {
		pfree(fileName);
		fileName = NULL;
	}		
	
	return status;			
}
				
/*
 * Backend checks if acknowledgement that its operation is completed
 * is received from mirror. 
 * If acknowledgement is received (state == FileRepAckStateCompleted) then
 *			a) entry is removed from hash
 *			b) TRUE is returned 
 */
bool 
FileRepAckPrimary_IsOperationCompleted(
					   FileRepIdentifier_u	 fileRepIdentifier,
					   FileRepRelationType_e fileRepRelationType)
{

	FileRepAckHashEntry_s	*entry=NULL;
	bool					isCompleted = FALSE;
	bool					isRemoved;
	FileName				fileName = NULL;
	int						retry = 0;
	bool					retval = FALSE;

	fileName = FileRep_GetFileName(fileRepIdentifier, fileRepRelationType);
	
	while ((isCompleted == FALSE) && (retry < file_rep_retry)) {
		
		LWLockAcquire(FileRepAckHashShmemLock, LW_EXCLUSIVE);
		
		entry = FileRepAckPrimary_LookupHashEntry(fileName);
					
		/* entry should be found since backend haven't received ack yet */
		Assert(entry != NULL);
					
		switch (entry->fileRepAckState) {
				
			case FileRepAckStateWaiting:
				/* No Operation */
				break;
			case FileRepAckStateCompleted:
				
				retval = TRUE;
				xLogEof = entry->xLogEof;
				/* no BREAK */
			case FileRepAckStateMirrorInFault:
				
				isCompleted = TRUE;
				
				isRemoved = FileRepAckPrimary_RemoveHashEntry(fileName);
				
				Assert(isRemoved == TRUE);
				
				break;
			default:
				Assert(0);
		}
		
		LWLockRelease(FileRepAckHashShmemLock);
		
		if (isCompleted == false) {
			FileRep_Sleep1ms(retry);
			retry++;
		}
	}

	if (retval == FALSE) {
		ereport(LOG,
				(errmsg("ERROR(filerep): could not complete mirror request identifier:'%s' ack state:'%s' "
						"position insert:'%p' count:'%d' ",
						fileName,
						FileRepAckStateToString[entry->fileRepAckState],
						fileRepAckShmem->positionInsert,
						fileRepAckShmem->insertCount),
				 FileRep_errdetail_ShmemAck(),
				 FileRep_errcontext()));		
	}
		
	if (fileName) {
		pfree(fileName);
		fileName = NULL;
	}		
	
	return retval;
}
		

/*
 * 
 * FileRepAckPrimary_StartConsumer
*/
void 
FileRepAckPrimary_StartConsumer(void)
{
	int status = STATUS_OK;
	
	elog(LOG, "FileRepAckPrimary_StartConsumer()");
		
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
		
		status = FileRepAckPrimary_RunConsumer();
		
	} // while(1)
		

	if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
		/* perform graceful shutdown */
	}

	/* NOTE free memory (if any) */
	return;
}

				
/*
 * FileRepAckPrimary_RunConsumer()
 */
static int
FileRepAckPrimary_RunConsumer(void)
{
	FileRepShmemMessageDescr_s	*fileRepShmemMessageDescr=NULL;
	FileRepMessageHeader_s		*fileRepMessageHeader;
	pg_crc32					*fileRepMessageHeaderCrc;
	pg_crc32					messageHeaderCrcLocal;
	int							status = STATUS_OK;
	bool						movePositionConsume = FALSE;
				
	elog(LOG, "FileRepAckPrimary_RunConsumer() ");
	
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
			if (FileRepSubProcess_GetState() != FileRepStateReady &&
				FileRepSubProcess_GetState() != FileRepStateInitialization) {
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
				
		} // internal while
		fileRepAckShmem->consumeCount++;
		LWLockRelease(FileRepAckShmemLock);
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateReady && 
			FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}
		
#ifdef FAULT_INJECTOR
		FaultInjector_InjectFaultIfSet(
									   FileRepConsumer,
									   DDLNotSpecified,
									   "",	//databaseName
									   ""); // tableName
#endif				
		
		/* Calculate and compare FileRepMessageHeader_s Crc */
		fileRepMessageHeader = (FileRepMessageHeader_s*) (fileRepAckShmem->positionConsume + 
														  sizeof(FileRepShmemMessageDescr_s));
		
		FileRep_CalculateCrc((char *) fileRepMessageHeader,
							 sizeof(FileRepMessageHeader_s),
							 &messageHeaderCrcLocal);	
		
		fileRepMessageHeaderCrc =
			(pg_crc32 *) (fileRepAckShmem->positionConsume + 
						  sizeof(FileRepMessageHeader_s) + 
						  sizeof(FileRepShmemMessageDescr_s));
		
		if (*fileRepMessageHeaderCrc != messageHeaderCrcLocal) {
			 
			status = STATUS_ERROR;
			ereport(LOG,
					(errmsg("ERROR(filerep): could not match Message Header CRC primary crc:'%u' mirror crc:'%u' ", 
							*fileRepMessageHeaderCrc, 
							messageHeaderCrcLocal),
					 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
									   fileRepMessageHeader->fileRepRelationType,
									   fileRepMessageHeader->fileRepOperation),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));					
			
			break;
		}
				
	    /* Write operation is never acknowledged. 
		 * That means message should never have body. 
		 * CRC of body should be always 0.
		 */
		Assert(fileRepMessageHeader->fileRepOperation != FileRepOperationWrite);
		Assert(fileRepMessageHeader->fileRepMessageBodyCrc == 0);
		
		if (fileRepMessageHeader->fileRepOperation == FileRepOperationReconcileXLogEof) {
			
			xLogEof = fileRepMessageHeader->fileRepOperationDescription.reconcile.xLogEof;
#ifdef FILEREP_DEBUG
			ereport(LOG,
					(errmsg("LOG(filerep): P_RunConsumer ack reconcile xlogid:'%d' reconcile xrecoff:'%d' ",
							xLogEof.xlogid, 
							xLogEof.xrecoff)));	
#endif
		}	
		
		if (fileRepMessageHeader->fileRepAckState != FileRepAckStateCompleted) {
			/* 
			 * FAULT has to be set before entry is updated in hash
			 * in order to suspend backend process.
			 */			
			status = STATUS_ERROR;
			ereport(LOG,
					(errmsg("ERROR(filerep): could not complete operation on mirror "
							"msg header count:'%d' ack state:'%s' ",
							fileRepMessageHeader->messageCount,
							FileRepAckStateToString[fileRepMessageHeader->fileRepAckState]),
					 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
									   fileRepMessageHeader->fileRepRelationType,
									   fileRepMessageHeader->fileRepOperation),
					 FileRep_errdetail_Shmem(),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));					
			
			FileRepSubProcess_SetState(FileRepStateFault);
		}
				
		if (FileRepAckPrimary_UpdateHashEntry(
				fileRepMessageHeader->fileRepIdentifier,
				fileRepMessageHeader->fileRepRelationType,
				fileRepMessageHeader->fileRepAckState) != STATUS_OK) {
			
			status = STATUS_ERROR;
			ereport(LOG,
					(errmsg("ERROR(filerep): could not update ack state "
							"msg header count:'%d'  ack state:'%s' ",
							fileRepMessageHeader->messageCount,
							FileRepAckStateToString[fileRepMessageHeader->fileRepAckState]),
					 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
									   fileRepMessageHeader->fileRepRelationType,
									   fileRepMessageHeader->fileRepOperation),
					 FileRep_errdetail_Shmem(),
					 FileRep_errdetail_ShmemAck(),
					 FileRep_errcontext()));					
		}
	
		FileRep_InsertLogEntry(
							   "P_RunConsumer",
							   fileRepMessageHeader->fileRepIdentifier,
							   fileRepMessageHeader->fileRepRelationType,
							   fileRepMessageHeader->fileRepOperation,
							   messageHeaderCrcLocal,
							   fileRepMessageHeader->fileRepMessageBodyCrc,
							   fileRepMessageHeader->fileRepAckState,
							   FILEREP_UNDEFINED,
							   fileRepMessageHeader->messageCount);				
		
#ifdef FILEREP_DEBUG
		ereport(LOG,
				(errmsg("LOG(filerep): P_RunConsumer ack msg header count:'%d' ack state:'%s' ",
						fileRepMessageHeader->messageCount,
						FileRepAckStateToString[fileRepMessageHeader->fileRepAckState]),
				 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
								   fileRepMessageHeader->fileRepRelationType,
								   fileRepMessageHeader->fileRepOperation),
				 FileRep_errdetail_ShmemAck(),
				 FileRep_errcontext()));		
#endif		
		

		if (status != STATUS_OK) {
			break;
		}
				
		movePositionConsume = TRUE;		
	} // while(1)	
	
	return status;
}

XLogRecPtr 
FileRepAckPrimary_GetMirrorXLogEof(void)
{
	return xLogEof;
}
				

