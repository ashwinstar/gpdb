/*
 *  cdbfilerepmirror.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved.
 *
 */

/*
 * FileRep process (receiver and consumer threads) launched by main thread.
 *
 * Responsibilities of this module
 *		*) 
 *
 */
#include "postgres.h"
#include <signal.h>

#include "port.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <glob.h>
#include <signal.h>

#include "cdb/cdbvars.h"

#include "access/xlog.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepmirror.h"
#include "cdb/cdbfilerepmirrorack.h"
#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepconnserver.h"
#include "libpq/pqsignal.h"
#include "storage/lwlock.h"
#include "utils/faultinjector.h"
#include "utils/hsearch.h"

/*
 * It specifies fields maintained by each entry in hash table
 * of open files.
 */
typedef struct OpenFileEntry {
	/* NOTE check it should be char fileName[MAXPGPATH] */
	char		fileName[MAXPGPATH+1];  
	/* It is key. It has to be first field in the structure. */
	
	File		fd;
	/* File Descriptor */
	
	FileRepRelationType_e fileRepRelationType;
	/* Type of the file. It is required for consistency check. */
	
	int			reference; /* NOTE check if multiple open will have also multiple close */
	/* 
	 * Tracks number of transactions that have concurrently opened file.
	 * File types that can be opened concurrently by multiple TX are
	 *		a) Flat File
	 *		b) Buffer Pool
	 * 
	 * File types that are not allowed to be opened concurrently by multiple TX are
	 *		a) Append Only
	 *		b) Bulk
	 *
	 */
	
} OpenFileEntry;


static HTAB *openFilesTable = NULL;


static int	FileRepMirror_Init(void);
static void FileRepMirror_Cleanup(void);

static int FileRepMirror_RunReceiver(void);
static int FileRepMirror_RunConsumer(void);

static FileRepShmem_s *fileRepShmem = NULL;

/*
 * Find file descriptor in hash table of open files.
 */
static File FileRepMirror_GetFileDescriptor(FileName fileName);

/*
 * Lookup for entry in hash table of open files.
 */
static OpenFileEntry* FileRepMirror_LookupFileName(FileName fileName);
/*
 * Insert entry with info related to new open file in hash table of open files. 
 */
static int FileRepMirror_InsertFileName(
					FileName				fileName, 
					File					fd,
					FileRepRelationType_e	fileRepRelationType);
/*
 * Remove entry from hash table of opend files.
 */
static void FileRepMirror_RemoveFileName(FileName fileName);

static void FileRepMirror_ReInit(void);

static int FileRepMirror_Validation(FileName fileName);

static void FileRepMirror_DropFilesFromDir(FileName fileName);

static int SegmentFileFillZero(File fd, int64 writeOffset);

static int FileRepMirror_DropBufferPoolSegmentFiles(FileName  fileName, int segmentFileNum);

static int FileRepMirror_Drop(FileName fileName);

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Mirror RECEIVER Process)
 ****************************************************************/

/*
 * 
 * FileRepMirror_StartReceiver
 */
void 
FileRepMirror_StartReceiver(void)
{	
	int status = STATUS_OK;
	int retval;

	elog(LOG, "FileRepMirror_StartReceiver() ");
	
	elog(LOG, "FileRepMirror_StartReceiver() started with role %d\n\t\tstate %d\n\t\tPrimary %s:%d\n\t\tMirror %s:%d\n",
		 fileRepRole, segmentState,
		 fileRepPrimaryHostAddress, fileRepPrimaryPort, 
		 fileRepMirrorHostAddress, fileRepMirrorPort);
	
	Insist(fileRepRole == FileRepMirrorRole);
	
	status = FileRepConnServer_StartListener(
								 fileRepMirrorHostAddress,
								 fileRepMirrorPort);
	if (status == STATUS_OK) {
		FileRepSubProcess_SetState(FileRepStateReady);
	}
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateFault) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
			
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
		
		status = FileRepMirror_RunReceiver();
		
	} // while(1)	
	
	FileRepConnServer_CloseConnection();
	
	return;
}

enum mirror_receiver_state { RECVTYPE, RECVLEN, RECVBODY};

/*
 * 
 */
static int
FileRepMirror_RunReceiver(void)
{
	uint32_t				msgLength = 0;
	FileRepConsumerProcIndex_e	msgType;
	int						status = STATUS_OK;
	char					*msgPositionInsert = NULL;
	FileRepShmemMessageDescr_s  *fileRepShmemMessageDescr;
	uint32					spareField;
	bool					waitShutdown = FALSE;
	bool					requestShutdown = FALSE;

	enum mirror_receiver_state current_state = RECVTYPE;

	elog(LOG, "FileRepMirror_RunReceiver()");
	
	for (;;)
	{
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateReady)
		{
			break;
		}

		if (!FileRepConnServer_AwaitMessageBegin())
			continue;

		if (waitShutdown == TRUE)
		{
			pg_usleep(50000L); // 50ms
			continue;
		}
		
		status = STATUS_OK;

		switch (current_state)
		{
			case RECVTYPE:
				status = FileRepConnServer_ReceiveMessageType(&msgType);

				if (status == STATUS_OK)
					current_state = RECVLEN;

				if (msgType == FileRepMessageTypeShutdown)
				{
					msgType = FileRepMessageTypeXLog;
					requestShutdown = TRUE;
					
					FileRep_InsertLogEntry(
										   "M_RunReceiver",
										   FileRep_GetFlatFileIdentifier("", ""),
										   FileRepRelationTypeNotSpecified,
										   FileRepOperationShutdown,
										   FILEREP_UNDEFINED,
										   FILEREP_UNDEFINED,
										   FileRepAckStateNotInitialized,
										   spareField,
										   FILEREP_UNDEFINED);						
				}
				
				break;
			case RECVLEN:
				/* DATA MESSAGE TYPE */
				status = FileRepConnServer_ReceiveMessageLength(&msgLength);

				if (status != STATUS_OK)
				{
					break;
				}
								
				msgPositionInsert = FileRep_ReserveShmem(
														 fileRepShmemArray[msgType],
														 msgLength, 
														 &spareField,
														 FileRepOperationNotSpecified);
		
				if (msgPositionInsert == NULL)
				{
					status = STATUS_ERROR;
					ereport(WARNING,
							(errmsg("mirror failure, "
									"could not queue received message to be processed, "
									"failover requested"), 
							 errhint("run gprecoverseg to re-establish mirror connectivity"),
							 FileRep_errdetail_Shmem(),
							 FileRep_errcontext()));										
			
					break;
				}
				if (status == STATUS_OK)
					current_state = RECVBODY;

				break;
			case RECVBODY:
				status = FileRepConnServer_ReceiveMessageData(
					msgPositionInsert + sizeof(FileRepShmemMessageDescr_s),
					msgLength);
		
				if (status != STATUS_OK)
				{
					break;
				}		
							
				FileRep_InsertLogEntry(
									   "M_RunReceiver",
									   FileRep_GetFlatFileIdentifier("", ""),
									   FileRepRelationTypeNotSpecified,
									   FileRepOperationNotSpecified,
									   FILEREP_UNDEFINED,
									   FILEREP_UNDEFINED,
									   FileRepAckStateNotInitialized,
									   spareField,
									   FILEREP_UNDEFINED);	
				
#ifdef FAULT_INJECTOR
				FaultInjector_InjectFaultIfSet(
											   FileRepReceiver,
											   DDLNotSpecified,
											   "",	//databaseName
											   ""); // tableName
#endif				
				
#ifdef FILEREP_DEBUG
				fileRepProcIndex = msgType;
				ereport(LOG,
						(errmsg("LOG(filerep): M_RunReceiver "
								"local count:'%d' position insert:'%p' msg length:'%u' "
								"consumer proc index:'%d' ",
								spareField,
								msgPositionInsert,
								msgLength,
								msgType),
						 FileRep_errdetail_Shmem(),
						 FileRep_errcontext()));		
#endif				
				
				fileRepShmemMessageDescr = (FileRepShmemMessageDescr_s*) msgPositionInsert;	
		
				/* it is not in use */
				fileRepShmemMessageDescr->messageSync = FALSE;		
				fileRepShmemMessageDescr->messageState = FileRepShmemMessageStateReady; 
				
				LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
				FileRep_IpcSignal(fileRepIpcArray[fileRepShmemArray[msgType]->ipcArrayIndex]->semC, 
								  fileRepIpcArray[fileRepShmemArray[msgType]->ipcArrayIndex]->refCountSemC);
				LWLockRelease(FileRepShmemLock);

				if (status == STATUS_OK)
					current_state = RECVTYPE;

				if (requestShutdown == TRUE)
					waitShutdown = TRUE;
				
				break;

			default:
				status = STATUS_ERROR;
				break;
			break;
		}
		
		if (status != STATUS_OK) {
			break;
		}
	} /* for (;;) */
		
	FileRepConnServer_CloseConnection();
	
	return status;
}

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Mirror CONSUMER Process)
 ****************************************************************/

/* NOTES
 *		thread pool
 *		have protection between open files and fd inserted in hash table
 *		have protection to shared memory consumption
 *		decrement reference and removing HASH entry should be atomic
 *      check if reference is required (multiple open result in multiple close)
 *      see example cdbdisp.c (call gp_pthread_create)
 *		see www.kernel.org (pthread_create)
 */


/*
 * Initialize hash table of open files in private memory of FileRep process.
 * FileName of the file is the key of the hash table.
 * FileName is relative file path from $PGDATA directory.
 */
static int
FileRepMirror_Init(void)
{
	HASHCTL	hash_ctl;
	int status = STATUS_OK;
							  
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = MAXPGPATH;
	hash_ctl.entrysize = sizeof(OpenFileEntry);
	hash_ctl.hash = string_hash;
														
	openFilesTable = hash_create("Open Files Table",
								FILEREP_MAX_OPEN_FILES,
								&hash_ctl,
								HASH_ELEM | HASH_FUNCTION); 
														
	if (openFilesTable == NULL) {
		status = STATUS_ERROR;
	} 
														
	return status;						  
}
														
/*
 *	a) Close files that are in the hash table.
 *	b) Remove hash table (free memory).
 */
static void
FileRepMirror_Cleanup(void)
{
	HASH_SEQ_STATUS		hash_status;
	OpenFileEntry		*entry;
	
	if (openFilesTable == NULL) {
		return;
	} 
	
	hash_seq_init(&hash_status, openFilesTable);
	
	while ((entry = (OpenFileEntry *) hash_seq_search(&hash_status)) != NULL) {
		Assert(entry->fd > 0);
		FileClose(entry->fd);
	}
	
	hash_destroy(openFilesTable);
	
	return;
}

/*
 *	Remove hash entry.
 */
static void
FileRepMirror_ReInit(void)
{
	HASH_SEQ_STATUS		hash_status;
	OpenFileEntry		*entry;
	
	if (openFilesTable == NULL) 
	{
		return;
	} 
	
	hash_seq_init(&hash_status, openFilesTable);
	
	while ((entry = (OpenFileEntry *) hash_seq_search(&hash_status)) != NULL) 
	{
		FileRepMirror_RemoveFileName(entry->fileName);
	}
	
	return;
}

/*
 * 
 * FileRepMirror_StartConsumer
 */
void 
FileRepMirror_StartConsumer(void)
{
	int status = STATUS_OK;
	
	if (Debug_filerep_print)
		elog(LOG, "FileRepMirror_StartConsumer() ");
	
	FileRepMirror_ReInit();
	
	switch (fileRepProcessType) 
	{
		case FileRepProcessTypeMirrorConsumer:
			fileRepProcIndex = FileRepMessageTypeXLog;  
			break;
		case FileRepProcessTypeMirrorConsumerWriter:
			fileRepProcIndex = FileRepMessageTypeWriter;
			break;			
		case FileRepProcessTypeMirrorConsumerAppendOnly1:
			fileRepProcIndex = FileRepMessageTypeAO01;
			break;
		case FileRepProcessTypeMirrorConsumerAppendOnly2:
			fileRepProcIndex = FileRepMessageTypeAO02;
			break;
		default:
			Assert(0);
			status = STATUS_ERROR;
			break;
	}
	
	fileRepShmem = fileRepShmemArray[fileRepProcIndex];
	
	if (Debug_filerep_print)
		elog(LOG, "FileRepMirror_StartConsumer consumer proc index:'%d' fileRepShmem:%p \n",
			 fileRepProcIndex,
			 fileRepShmem);
	
	status = FileRepMirror_Init();
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateInitialization ||
			   FileRepSubProcess_GetState() == FileRepStateFault) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
			
			break;
		}		
		
		Insist(FileRepSubProcess_GetState() == FileRepStateReady);
		
		status = FileRepMirror_RunConsumer();
		
	} // while(1)
	if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
		/* perform graceful shutdown */
	}
	
	/* NOTE free memory (if any) */
	FileRepMirror_Cleanup();
	
	return;
}


/*
 *		NOTE: Draw state machine
 *
 *		a) File create  
 *				After create the file is closed. 
 *		b) File delete/drop
 *				Before delete the file must be closed 
 *				otherwise error will be returned.
 *		c) File open
 *				Before open the file must be created. If private relation then 
 *				file mustn't be already opened.
 *		d) File write
 *				Before write the file must be opened.
 *		e) File close
 *				Before close the file must be opened.
 *		f) Directory create
 *				Before create the parent directory must have the right permissions.
 *		g) Directory delete/drop
 *				Before delete the directory must be empty.
 */
static int
FileRepMirror_RunConsumer(void)
{
	FileRepShmemMessageDescr_s	*fileRepShmemMessageDescr=NULL;
	FileRepMessageHeader_s		*fileRepMessageHeader;
	pg_crc32					*fileRepMessageHeaderCrc;
	char						*fileRepMessageBody = NULL;
	pg_crc32					messageHeaderCrcLocal;
	pg_crc32					messageBodyLocal;
	File						fd = 0;
	FileName					fileName=NULL;
	FileName					newFileName;
	int							fileMode = 0666; // change to 0600
	int							status = STATUS_OK;
	int64						seekResult;
	bool						found = FALSE;
	OpenFileEntry				*entry = NULL;
	bool						movePositionConsume = FALSE;
	XLogRecPtr					mirrorEofRecPtr = {0, 0};
	uint32						spare = 0;
		
	if (Debug_filerep_print)
		elog(LOG, "FileRepMirror_RunConsumer() ");

	errno = 0;
	
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
			if (FileRepSubProcess_GetState() != FileRepStateReady) {
				LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
				break;
			}
			
			FileRep_IpcWait(fileRepIpcArray[fileRepShmem->ipcArrayIndex]->semC);
	
			//pg_usleep(1000L);  /* 1 ms */
			
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
		if (FileRepSubProcess_GetState() != FileRepStateReady) {
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
		fileRepMessageHeader = (FileRepMessageHeader_s*) (fileRepShmem->positionConsume + 
								  sizeof(FileRepShmemMessageDescr_s));
		
		FileRep_CalculateCrc((char *) fileRepMessageHeader,
							 sizeof(FileRepMessageHeader_s),
							 &messageHeaderCrcLocal);	
		
		fileRepMessageHeaderCrc =
		(pg_crc32 *) (fileRepShmem->positionConsume + 
					sizeof(FileRepMessageHeader_s) + 
					sizeof(FileRepShmemMessageDescr_s));
		
		if (*fileRepMessageHeaderCrc != messageHeaderCrcLocal) {
			
			status = STATUS_ERROR;
			ereport(WARNING,
				(errmsg("mirror failure, "
						"could not match message header checksum between primary:'%u' and mirror:'%u', "
						"failover requested", 
						*fileRepMessageHeaderCrc, 
						messageHeaderCrcLocal),
				 errhint("run gprecoverseg to re-establish mirror connectivity"),
				 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
								   fileRepMessageHeader->fileRepRelationType,
								   fileRepMessageHeader->fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));		
			
			break;
		}
		
  		/* if message body present then calculate and compare Data Crc */
		
		if (fileRepMessageHeader->fileRepOperation == FileRepOperationWrite) {
			
			fileRepMessageBody =
				(char*) (fileRepShmem->positionConsume + 
						 sizeof(FileRepMessageHeader_s) + 
						 sizeof(FileRepShmemMessageDescr_s) + 
						 sizeof(pg_crc32));
			
			FileRep_CalculateCrc((char *) fileRepMessageBody,
					fileRepMessageHeader->fileRepOperationDescription.write.dataLength,
					&messageBodyLocal);	
			
			if (messageBodyLocal != fileRepMessageHeader->fileRepMessageBodyCrc) {
				ereport(WARNING,
					(errmsg("mirror failure, "
							"could not match message body checksum between primary:'%u' and mirror:'%u' data length:'%u', "
							"failover requested", 
							fileRepMessageHeader->fileRepMessageBodyCrc, 
							messageBodyLocal,
							fileRepMessageHeader->fileRepOperationDescription.write.dataLength),
					 errhint("run gprecoverseg to re-establish mirror connectivity"),
						FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
										  fileRepMessageHeader->fileRepRelationType,
										  fileRepMessageHeader->fileRepOperation),
						FileRep_errdetail_Shmem(),
						FileRep_errcontext()));		
				
				
				status = STATUS_ERROR;
				break;
			}	
		}
		
		FileRep_InsertLogEntry(
							   "M_RunConsumer",
							   fileRepMessageHeader->fileRepIdentifier,
							   fileRepMessageHeader->fileRepRelationType,
							   fileRepMessageHeader->fileRepOperation,
							   messageHeaderCrcLocal,
							   fileRepMessageHeader->fileRepMessageBodyCrc,
							   FileRepAckStateNotInitialized,
							   spare,
							   fileRepMessageHeader->messageCount);		
		
		if (Debug_filerep_print)
			ereport(LOG,
				(errmsg("LOG(filerep): M_RunConsumer msg header count:'%d' local count:'%d' "
						"consumer proc index:'%d'",
						fileRepMessageHeader->messageCount,
						spare,
						fileRepProcIndex),
				 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
								   fileRepMessageHeader->fileRepRelationType,
								   fileRepMessageHeader->fileRepOperation),
				 FileRep_errdetail_Shmem(),
				 FileRep_errcontext()));		
	
		spare = fileRepMessageHeader->messageCount;
		
		/* fileName is relative path to $PGDATA directory */
		fileName = FileRep_GetFileName(
						fileRepMessageHeader->fileRepIdentifier,
						fileRepMessageHeader->fileRepRelationType);
				
		switch (fileRepMessageHeader->fileRepOperation) {
			case FileRepOperationOpen:
			case FileRepOperationCreateAndOpen:
				found = FALSE;
				
				entry = FileRepMirror_LookupFileName(fileName);
				
				if (entry != NULL) {
					Assert(entry->fd > 0);
					fd = entry->fd;
					
					switch (fileRepMessageHeader->fileRepRelationType) {
							
						case FileRepRelationTypeAppendOnly:	
							
							Assert(fileRepMessageHeader->fileRepRelationType == entry->fileRepRelationType);
							
							seekResult = FileSeek(fd, 
												  fileRepMessageHeader->fileRepOperationDescription.open.logicalEof, 
												  SEEK_SET);
							if (seekResult != fileRepMessageHeader->fileRepOperationDescription.open.logicalEof) {
								status = STATUS_ERROR;
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("mirror failure, "
												"could not seek to file position:" INT64_FORMAT " : %m, "
												"failover requested",
												fileRepMessageHeader->fileRepOperationDescription.open.logicalEof),
										 errhint("run gprecoverseg to re-establish mirror connectivity"),
										 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
														   fileRepMessageHeader->fileRepRelationType,
														   fileRepMessageHeader->fileRepOperation),
										 FileRep_errcontext()));					
								break;
							}
							/* no break */

						case FileRepRelationTypeFlatFile:
						case FileRepRelationTypeBufferPool:
						case FileRepRelationTypeBulk:

							found = TRUE;
#ifdef FILEREP_DEBUG
							ereport(LOG, 
									(errmsg("LOG(filerep): File is already opened with file descriptor:'%d' ",
											entry->fd),
									FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													  fileRepMessageHeader->fileRepRelationType,
													  fileRepMessageHeader->fileRepOperation),
									FileRep_errcontext()));		
#endif							
							break;
						
						default:
							break;
					}
					if (status != STATUS_OK || found) {
						break;
					}
				}
				
				/* special handling for Append Only in order to behave the same as primary */
				
				if (fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeAppendOnly) {
										
					fd = PathNameOpenFile(
							  fileName,
							  fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
							  fileRepMessageHeader->fileRepOperationDescription.open.fileMode);
					if (fd < 0) {
						if (fileRepMessageHeader->fileRepOperationDescription.open.logicalEof > 0) {

							status = STATUS_ERROR;
							
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("mirror failure, "
											"could not open file position:" INT64_FORMAT " file flags:'%x' "
											"file mode:'%o' : %m, "
											"failover requested", 
											fileRepMessageHeader->fileRepOperationDescription.open.logicalEof,
											fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
											fileRepMessageHeader->fileRepOperationDescription.open.fileMode),
									 errhint("run gprecoverseg to re-establish mirror connectivity"),
									 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													   fileRepMessageHeader->fileRepRelationType,
													   fileRepMessageHeader->fileRepOperation),
									 FileRep_errcontext()));					
							
						} else {
							
							fd = 0;
#ifdef FILEREP
							ereport(LOG,
									(errcode_for_file_access(),
									 errmsg("LOG(filerep): could not open file, primary will send another request for create " 
											"position:" INT64_FORMAT " file flags:'%x' "
											"file mode:'%o' : %m", 
											fileRepMessageHeader->fileRepOperationDescription.open.logicalEof,
											fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
											fileRepMessageHeader->fileRepOperationDescription.open.fileMode),
									 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													   fileRepMessageHeader->fileRepRelationType,
													   fileRepMessageHeader->fileRepOperation),
									 FileRep_errcontext()));					
#endif							
						}
						break;
					}

					seekResult = FileSeek(fd, 
										  fileRepMessageHeader->fileRepOperationDescription.open.logicalEof, 
										  SEEK_SET);
					if (seekResult != fileRepMessageHeader->fileRepOperationDescription.open.logicalEof) {
						status = STATUS_ERROR;
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("mirror failure, "
										"could not seek to file position:" INT64_FORMAT " : %m, "
										"failover requested", 
										fileRepMessageHeader->fileRepOperationDescription.open.logicalEof),
								 errhint("run gprecoverseg to re-establish mirror connectivity"),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));					
 						break;
					}
				} else {
			
					if (fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeFlatFile)
					{
						errno = 0;
						fd = PathNameOpenFile(
											  fileName,
											  fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
											  fileRepMessageHeader->fileRepOperationDescription.open.fileMode);
						
						if (fd < 0 && 
							fileRepMessageHeader->fileRepOperationDescription.open.suppressError == TRUE && 
							errno == ENOENT) 
						{
							if (Debug_filerep_print)
								ereport(LOG,
										(errcode_for_file_access(),
										 errmsg("LOG(filerep): could not open file \"%s\" suppressError==TRUE : %m", fileName)));
							
							errno = 0;
							fd = PathNameOpenFile(
												  fileName,
												  fileRepMessageHeader->fileRepOperationDescription.open.fileFlags | O_CREAT,
												  fileRepMessageHeader->fileRepOperationDescription.open.fileMode);								
						}						
												
						if (fd < 0 && 
							fileRepMessageHeader->fileRepOperationDescription.open.suppressError == TRUE && 
							errno == ENOENT) {
							if (Debug_filerep_print)
								ereport(LOG,
										(errcode_for_file_access(),
										 errmsg("LOG(filerep): could not open file \"%s\" suppressError==TRUE : %m", fileName)));
							fd = 0;
							break;							
						}	
						
						if (fd < 0) {
							status = STATUS_ERROR;
							
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("mirror failure, "
											"could not open file position:" INT64_FORMAT " "
											"file flags:'%x' file mode:'%o' : %m, "
											"failover requested", 
											fileRepMessageHeader->fileRepOperationDescription.open.logicalEof,
											fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
											fileRepMessageHeader->fileRepOperationDescription.open.fileMode),
									 errhint("run gprecoverseg to re-establish mirror connectivity"),
									 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													   fileRepMessageHeader->fileRepRelationType,
													   fileRepMessageHeader->fileRepOperation),
									 FileRep_errcontext()));					
							
							break;
						}	
						
					} else {
					
						errno = 0;
						fd = PathNameOpenFile(
								fileName,
								fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
								fileRepMessageHeader->fileRepOperationDescription.open.fileMode);
						
						/*
						 *	Segment Files > 0 for Buffer Pool are not created transactionally hence
						 *	during in transition to Resync and crash recovery are created explicitly
						 */
						if (fd < 0 && 
							errno == ENOENT && 
							fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeBufferPool &&
							fileRepMessageHeader->fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum > 0)
						{
							errno = 0;
							fd = PathNameOpenFile(
												  fileName,
												  O_CREAT | fileRepMessageHeader->fileRepOperationDescription.open.fileFlags | O_RDWR | PG_BINARY,
												  fileRepMessageHeader->fileRepOperationDescription.open.fileMode);
						}
						
						if ((fd < 0) && ((errno == EEXIST) || (errno == ENOENT))) {
							
							if (Debug_filerep_print)
								ereport(LOG,
										(errcode_for_file_access(),
										 errmsg("WARNING(filerep): could not open file position:" INT64_FORMAT " "
												"file flags:'%x' file mode:'%o' : %m", 
												fileRepMessageHeader->fileRepOperationDescription.open.logicalEof,
												fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
												fileRepMessageHeader->fileRepOperationDescription.open.fileMode),
										 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
														   fileRepMessageHeader->fileRepRelationType,
														   fileRepMessageHeader->fileRepOperation),
										 FileRep_errcontext()));					
				
							/* NOTE see md.c L242 there are exceptions, how mirror will know about it */ 
							fd = 0;
							break;
						}

						if (fd < 0) {
							status = STATUS_ERROR;

							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("mirror failure, "
											"could not open file position:" INT64_FORMAT " "
											"file flags:'%x' file mode:'%o' : %m, "
											"failover requested", 
											fileRepMessageHeader->fileRepOperationDescription.open.logicalEof,
											fileRepMessageHeader->fileRepOperationDescription.open.fileFlags,
											fileRepMessageHeader->fileRepOperationDescription.open.fileMode),
									 errhint("run gprecoverseg to re-establish mirror connectivity"),
									 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													   fileRepMessageHeader->fileRepRelationType,
													   fileRepMessageHeader->fileRepOperation),
									 FileRep_errcontext()));					
							
							break;
						}	
						
					}
				}
				status = FileRepMirror_InsertFileName(
							fileName, 
							fd,
							fileRepMessageHeader->fileRepRelationType);
				
				if (status != STATUS_OK) {
					/* 
					 * File is closed explicitly since it is not 
					 * inserted into hash table. If file is inserted
					 * in hash table then it is closed as part of the
					 * removing hash table during shutdown.
					 */
					FileClose(fd);
					break;
				}
				
				break;
			case FileRepOperationWrite:
				
				fd = FileRepMirror_GetFileDescriptor(fileName);
				
				elog(DEBUG1, "FileRepOperationWrite() filename:%s fd:%d", 
					 fileName, fd);	
				
				if (fd < 0) {	
					if (fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeAppendOnly) {

						ereport(WARNING,
								(errmsg("mirror failure, "
										"could not find file descriptor, "
										"failover requested"), 
								 errhint("run gprecoverseg to re-establish mirror connectivity"),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));					
						
						status = STATUS_ERROR;
						break;
					}
					
					errno = 0;
					fd = PathNameOpenFile(
										  fileName,
										  O_RDWR,
										  fileMode);
					
					
					/*
					 *	Segment Files > 0 for Buffer Pool are not created transactionally hence
					 *	during in transition to Resync and crash recovery are created explicitly
					 */
					if (fd < 0 && 
						errno == ENOENT && 
						dataState == DataStateInResync &&
						fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeBufferPool &&
						fileRepMessageHeader->fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum > 0)
					{
						errno = 0;
						fd = PathNameOpenFile(
											  fileName,
											  O_CREAT | O_RDWR | PG_BINARY,
											  fileMode);
					}					
					
					if (fd < 0) {
						status = STATUS_ERROR;
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("mirror failure, "
										"could not open file file flags:'%x' file mode:'%o': %m, "
										"failover requested",
								 O_CREAT | O_RDWR | PG_BINARY,
								 fileMode), 
								 errhint("run gprecoverseg to re-establish mirror connectivity"),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));					
						break;
					}	
					
					status = FileRepMirror_InsertFileName(
										  fileName, 
										  fd,
										  fileRepMessageHeader->fileRepRelationType);
					
					if (status != STATUS_OK) {
						/* 
						 * File is closed explicitly since it is not 
						 * inserted into hash table. If file is inserted
						 * in hash table then it is closed as part of the
						 * removing hash table during shutdown.
						 */
						FileClose(fd);
						break;
					}
				}
				
				/*
				 *	Segment Files > 0 for Buffer Pool are not created transactionally hence
				 *	during in transition to Resync and crash recovery are created explicitly
				 */
				if (fd > 0 && 
					dataState == DataStateInResync &&
					fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeBufferPool &&
					fileRepMessageHeader->fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum > 0)
				{
					Assert(fileRepMessageHeader->fileRepOperationDescription.write.offset != FILEREP_OFFSET_UNDEFINED);
					
					status = SegmentFileFillZero(fd,
												 (int64)fileRepMessageHeader->fileRepOperationDescription.write.offset);
					
					if (status != STATUS_OK)
					{
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("mirror failure, "
										"could not fill zero blocks into segment file : %m, "
										"failover requested"),
								 errhint("run gprecoverseg to re-establish mirror connectivity"),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));					
						
						break;
					}	
				}
				
				if (fileRepMessageHeader->fileRepOperationDescription.write.offset != FILEREP_OFFSET_UNDEFINED) {
					seekResult = FileSeek(fd, 
										  fileRepMessageHeader->fileRepOperationDescription.write.offset, 
										  SEEK_SET);
					if (seekResult != fileRepMessageHeader->fileRepOperationDescription.write.offset) {
						status = STATUS_ERROR;						
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("mirror failure, "
										"could not seek to file position:'%d' : %m, "
										"failover requested", 
										fileRepMessageHeader->fileRepOperationDescription.write.offset),
								 errhint("run gprecoverseg to re-establish mirror connectivity"),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));	
						break;
						
					}
				}
				
				errno = 0;
				if ((int) FileWrite(
							fd, 
							fileRepMessageBody,
							fileRepMessageHeader->fileRepOperationDescription.write.dataLength) != 
							fileRepMessageHeader->fileRepOperationDescription.write.dataLength) {

					/* if write didn't set errno, assume problem is no disk space */
					if (errno == 0)
						errno = ENOSPC;
					
					status = STATUS_ERROR;
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("mirror failure, "
									"could not write to file : %m, "
									"failover requested"), 
							 errhint("run gprecoverseg to re-establish mirror connectivity"),
							 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
											   fileRepMessageHeader->fileRepRelationType,
											   fileRepMessageHeader->fileRepOperation),
							 FileRep_errcontext()));					
					
					break;
				}	
				
				break;
			case FileRepOperationFlush:
				
				fd = FileRepMirror_GetFileDescriptor(fileName);
				elog(DEBUG1, "FileRepOperationFlush() filename:%s fd:%d", 
					 fileName, fd);	
					
					if (fd < 0) {	
						if (fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeAppendOnly) {

							 ereport(WARNING,
									 (errcode_for_file_access(),
									  errmsg("mirror failure, "
											 "could not find file descriptor : %m, "
											 "failover requested"), 
									  errhint("run gprecoverseg to re-establish mirror connectivity"),
									  FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
														fileRepMessageHeader->fileRepRelationType,
														fileRepMessageHeader->fileRepOperation),
									  FileRep_errcontext()));					
							
							 status = STATUS_ERROR;
							 break;
						}
						
						errno = 0;
						fd = PathNameOpenFile(
											  fileName,
											  O_RDWR,
											  fileMode);
						if (fd < 0 && errno == ENOENT) {
							fd = 0;
							if (Debug_filerep_print)
									 ereport(LOG,
										(errcode_for_file_access(),
										 errmsg("LOG(filerep): could not open file; "
												"file was already dropped identifier:'%s' : %m", 
												(fileName == NULL) ? "" : fileName)));
							break;
						}
						
						if (fd < 0) {
							status = STATUS_ERROR;
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("mirror failure, "
											"could not find file descriptor or open file : %m"
											"failover requested"),  
									 errhint("run gprecoverseg to re-establish mirror connectivity"),
									 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													   fileRepMessageHeader->fileRepRelationType,
													   fileRepMessageHeader->fileRepOperation),
									 FileRep_errcontext()));					
							
							break;
						}	
						
						status = FileRepMirror_InsertFileName(
										  fileName, 
										  fd,
										  fileRepMessageHeader->fileRepRelationType);
						
						if (status != STATUS_OK) {
							/* 
							 * File is closed explicitly since it is not 
							 * inserted into hash table. If file is inserted
							 * in hash table then it is closed as part of the
							 * removing hash table during shutdown.
							 */
							FileClose(fd);
							break;
						}
				}
				
#ifdef FAULT_INJECTOR
				FaultInjector_InjectFaultIfSet(
											   FileRepFlush,
											   DDLNotSpecified,
											   "",	//databaseName
											   ""); // tableName
#endif								
				
				status = FileSync(fd);
				
				if (status != STATUS_OK) {
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("mirror failure, "
									"could not fsync file : %m"
									"failover requested"), 
							 errhint("run gprecoverseg to re-establish mirror connectivity"),
							 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
											   fileRepMessageHeader->fileRepRelationType,
											   fileRepMessageHeader->fileRepOperation),
							 FileRep_errcontext()));					
					
					break;
				}						
								
				break;
				
			case FileRepOperationClose:
								
				fd = FileRepMirror_GetFileDescriptor(fileName);
				
				elog(DEBUG1, "FileRepOperationClose() filename:%s fd:%d", 
					 fileName, fd);	
				
				if (fd < 0) {
#ifdef FILEREP_DEBUG
					ereport(LOG,
							(errmsg("LOG(filerep): could not find file descriptor"),
							 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
											   fileRepMessageHeader->fileRepRelationType,
											   fileRepMessageHeader->fileRepOperation),
							 FileRep_errcontext()));					
#endif					
					fd = 0;
					break;
				}
				
				FileClose(fd);
				
				FileRepMirror_RemoveFileName(fileName);
				
				break;
				
			case FileRepOperationFlushAndClose:
								
				fd = FileRepMirror_GetFileDescriptor(fileName);
				
				elog(DEBUG1, "FileRepOperationFlushAndClose() filename:%s fd:%d", 
					 fileName, fd);	
				
				if (fd < 0) {
					/*
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not find file descriptor \"%s\": %m", 
									fileName)));
					status = STATUS_ERROR;
					break;
					*/
					
					errno = 0;
					fd = PathNameOpenFile(
										  fileName,
										  O_RDWR,
										  fileMode);

					
					if (fd < 0 && errno == ENOENT) {
						fd = 0;
						ereport(LOG,
								(errcode_for_file_access(),
								 errmsg("LOG(filerep): could not flush and close file, "
										"The file was already dropped identifier:'%s' : %m", 
										(fileName == NULL) ? "" : fileName)));
						break;
					}					
					
					if (fd < 0) {
						status = STATUS_ERROR;
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("mirror failure, "
										"could not find file descriptor or open file : %m"
										"failover requested"), 
								 errhint("run gprecoverseg to re-establish mirror connectivity"),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));					
						
						break;
					}	
					
					status = FileRepMirror_InsertFileName(
									  fileName, 
									  fd,
									  fileRepMessageHeader->fileRepRelationType);
					
					if (status != STATUS_OK) {
						/* 
						 * File is closed explicitly since it is not 
						 * inserted into hash table. If file is inserted
						 * in hash table then it is closed as part of the
						 * removing hash table during shutdown.
						 */
						FileClose(fd);
						break;
					}					
				}
				
				status = FileSync(fd);
				
				if (status != STATUS_OK) {
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("mirror failure, "
									"could not fsync file : %m"
									"failover requested"),  
							 errhint("run gprecoverseg to re-establish mirror connectivity"),
							 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
											   fileRepMessageHeader->fileRepRelationType,
											   fileRepMessageHeader->fileRepOperation),
							 FileRep_errcontext()));					
					
					/* no break */
				}										
				
				FileClose(fd);
				
				FileRepMirror_RemoveFileName(fileName);				
												
				break;
				
				
			case FileRepOperationTruncate:
				
				fd = FileRepMirror_GetFileDescriptor(fileName);
				
				elog(DEBUG1, "FileRepOperationTruncate() filename:%s fd:%d", 
					 fileName, fd);	
				
				if (fd < 0) {
					/*
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not find file descriptor \"%s\": %m", 
									fileName)));
					status = STATUS_ERROR;
					break;
					*/
					
					errno = 0;
					fd = PathNameOpenFile(
										  fileName,
										  O_RDWR,
										  fileMode);
					
					if (fd < 0) {
						status = STATUS_ERROR;
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("mirror failure, "
										"could not find file descriptor or open file : %m"
										"failover requested"),  
								 errhint("run gprecoverseg to re-establish mirror connectivity"),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));					
						
						break;
					}	
					
					status = FileRepMirror_InsertFileName(
											  fileName, 
											  fd,
											  fileRepMessageHeader->fileRepRelationType);
					
					if (status != STATUS_OK) {
						/* 
						 * File is closed explicitly since it is not 
						 * inserted into hash table. If file is inserted
						 * in hash table then it is closed as part of the
						 * removing hash table during shutdown.
						 */
						FileClose(fd);
						break;
					}					
					
				}
				
				status = FileTruncate(fd, fileRepMessageHeader->fileRepOperationDescription.truncate.position);
				
				if (status < 0) {
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("mirror failure, "
									"could not truncate file to position:'" INT64_FORMAT "' : %m"
									"failover requested",  
							 fileRepMessageHeader->fileRepOperationDescription.truncate.position), 
							 errhint("run gprecoverseg to re-establish mirror connectivity"),
							 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
											   fileRepMessageHeader->fileRepRelationType,
											   fileRepMessageHeader->fileRepOperation),
							 FileRep_errcontext()));					
					
					status = STATUS_ERROR;
					break;
				}
				
				if (fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeBufferPool)
				{
					FileName	fileNameLocal = NULL;
					int			segmentFileNumLocal = fileRepMessageHeader->fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum;
					
					fileRepMessageHeader->fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum = 0;
					
					fileNameLocal = FileRep_GetFileName(
												   fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType);
					
					fileRepMessageHeader->fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum = segmentFileNumLocal;			
					
					status = FileRepMirror_DropBufferPoolSegmentFiles(fileNameLocal, 
																	  segmentFileNumLocal);
					if (fileNameLocal) {
						pfree(fileNameLocal);
						fileNameLocal = NULL;
					}							
				}
				
				break;				
				
			case FileRepOperationReconcileXLogEof:
				elog(DEBUG1, "FileRepOperationReconcile() filename:%s", fileName);	
				
				status = XLogReconcileEofMirror(
						fileRepMessageHeader->fileRepOperationDescription.reconcile.xLogEof,
						&mirrorEofRecPtr);
				
			    if (status != STATUS_OK) {
				    ereport(WARNING,
							(errmsg("mirror failure, "
								   "could not reconcile xlog file : %m"
								   "failover requested"),  
							 errhint("run gprecoverseg to re-establish mirror connectivity"),
							FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
											  fileRepMessageHeader->fileRepRelationType,
											  fileRepMessageHeader->fileRepOperation),
							FileRep_errcontext()));					
						
				    break;
			    }		
				
				fileRepMessageHeader->fileRepOperationDescription.reconcile.xLogEof = mirrorEofRecPtr;

				elog(LOG, "FileRepOperationReconcile() filename:%s xlogid:%d xrecoff:%d", 
					 fileName, mirrorEofRecPtr.xlogid, 
					 mirrorEofRecPtr.xrecoff);	

			    break;
				
			case FileRepOperationCreate:
				elog(DEBUG1, "FileRepOperationCreate() filename:%s", fileName);	

				switch (fileRepMessageHeader->fileRepRelationType) {
					case FileRepRelationTypeDir:

						/* when tablespaces are added mkdir() 
						   creates only one sub-directory */
						errno = 0;
						status = mkdir(fileName, 0700);
						
						if (errno == EEXIST &&
							fileRepMessageHeader->fileRepOperationDescription.create.ignoreAlreadyExists)
						{
							status = STATUS_OK;
							errno = 0;
						}
						
						if (status < 0)
						{		
							/* 
							 * status == STATUS_OK then
							 *			segment does not transition into Fault, instead error is reported to primary 
							 *
							 * status == STATUS_ERROR then
							 *			segment transitions into Fault
							 */													
							if (fileRepMessageHeader->fileRepOperationDescription.create.ignoreAlreadyExists)
							{
								status = STATUS_ERROR;
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("mirror failure, "
												"could not create directory : %m"
												"failover requested"),  
										 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
														   fileRepMessageHeader->fileRepRelationType,
														   fileRepMessageHeader->fileRepOperation),
										 FileRep_errcontext()));										
							}
							else
							{
								status = STATUS_OK;
								
								if (Debug_filerep_print)
									ereport(LOG,
											(errcode_for_file_access(),
											 errmsg("could not create directory : %m"), 
											 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
															   fileRepMessageHeader->fileRepRelationType,
															   fileRepMessageHeader->fileRepOperation),
											 FileRep_errcontext()));										
							}
						}
						
						switch (errno) 
						{
						case 0:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusSuccess;
							break;
	
						case EEXIST:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusDirectoryExist;
							break;
								
						case EACCES:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusNoPermissions;
							break;
								
						case ENOENT:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusNoSuchFileOrDirectory;
							break;
								
						case ENOTDIR:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusNotDirectory;
							break;
								
						case EROFS:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusReadOnlyFileSystem;
							break;
								
						case ENOSPC:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusNoSpace;
							break;
								
						default:
							fileRepMessageHeader->fileRepOperationDescription.create.mirrorStatus = FileRepStatusMirrorError;	
							break;
						}
												
						break;
						
					case FileRepRelationTypeFlatFile:
						
						fd = PathNameOpenFile(
								fileName,
								O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
								fileMode);	
						
						if (fd < 0 && errno == EEXIST) {
							fd = PathNameOpenFile(
									  fileName,
									  O_RDWR | PG_BINARY,
									  fileMode);
							
						}
							   
						if (fd < 0) {
							status = STATUS_ERROR;
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("mirror failure, "
											"could not create file : %m, "
											"failover requested"), 
									 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													   fileRepMessageHeader->fileRepRelationType,
													   fileRepMessageHeader->fileRepOperation),
									 FileRep_errcontext()));					
					
							break;
						}
						
						/* 
						 * The file is closed after create.
						 * Open operation is required to open file for write. 
						 */
						status = FileRepMirror_InsertFileName(
										  fileName, 
										  fd,
										  fileRepMessageHeader->fileRepRelationType);
						
						if (status != STATUS_OK) {
							/* 
							 * File is closed explicitly since it is not 
							 * inserted into hash table. If file is inserted
							 * in hash table then it is closed as part of the
							 * removing hash table during shutdown.
							 */
							FileClose(fd);
						}
						break;
									
					default:
						Assert(0);
				}
				
				break;
				
			case FileRepOperationDrop:
				elog(DEBUG1, "FileRepOperationDrop() filename:%s", fileName);	
			
				switch (fileRepMessageHeader->fileRepRelationType) {
					case FileRepRelationTypeDir:
						
						errno = 0;
						if (rmtree(fileName, true) == FALSE && errno != ENOENT) 
						{

							status = STATUS_ERROR;
							ereport(LOG,
									(errcode_for_file_access(),
									 errmsg("ERROR(filerep): could not remove directory : %m"), 
									 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
													   fileRepMessageHeader->fileRepRelationType,
													   fileRepMessageHeader->fileRepOperation),
									 FileRep_errcontext()));	
							break;
						}
						
						break;
						
					case FileRepRelationTypeBufferPool:
						/* Buffer Pool drops segmentFileNum = 0 */
						status = FileRepMirror_DropBufferPoolSegmentFiles(fileName, 0);
						if (status != STATUS_OK)
						{
							break;
						}
						/* NO BREAK */
					case FileRepRelationTypeAppendOnly:
					case FileRepRelationTypeFlatFile:
						
						status = FileRepMirror_Drop(fileName);
						break;
												
					default:
						Assert(0);
				}
				
				break;
				
			case FileRepOperationDropFilesFromDir:
				
				elog(DEBUG1, "FileRepOperationDropFilesFromDir() filename:%s", fileName);
				
				switch (fileRepMessageHeader->fileRepRelationType) 
				{
					case FileRepRelationTypeFlatFile:
						FileRepMirror_DropFilesFromDir(fileName);
						break;
						
					default:
						break;
				}
				break;
				
			case FileRepOperationValidation:
				
				elog(DEBUG1, "FileRepOperationValidation() filename:%s", fileName);	

				switch (fileRepMessageHeader->fileRepOperationDescription.validation.fileRepValidationType)
				{	
					case FileRepValidationFilespace:
						
						Assert(fileRepMessageHeader->fileRepRelationType == FileRepRelationTypeDir);
						
						fileRepMessageHeader->fileRepOperationDescription.validation.mirrorStatus = FileRepMirror_Validation(fileName);
						break;
					
					case FileRepValidationExistence:
					{	
						struct stat	 st;
						
						if (stat(fileName, &st) >= 0)
						{
							fileRepMessageHeader->fileRepOperationDescription.validation.mirrorStatus = FileRepStatusSuccess;
						}
						else
						{
							fileRepMessageHeader->fileRepOperationDescription.validation.mirrorStatus = FileRepStatusNoSuchFileOrDirectory;
						}
						
						break;
					}	
					default:
						Assert(0);
						break;
				}
										
				break;
				
			case FileRepOperationRename:
						
				elog(DEBUG1, "FileRepOperationRename() from filename:%s", fileName);	
				errno = 0;
				
				/* Ensure that file is not opened. */
				fd = FileRepMirror_GetFileDescriptor(fileName);
				
				if (fd > 0) {
					ereport(LOG,
							(errmsg("FileRepOperationRename() Removing file name from table. fd:%d", fd)));		

				/*	FileClose(fd);*/
					FileRepMirror_RemoveFileName(fileName);
				}
				
				newFileName = FileRep_GetFileName(
												  fileRepMessageHeader->fileRepOperationDescription.rename.fileRepIdentifier,
												  fileRepMessageHeader->fileRepRelationType);

				/* REMOVE after fix in Open */
				
				fd = FileRepMirror_GetFileDescriptor(newFileName);
				
				if (fd > 0) {
					ereport(LOG,
							(errmsg("FileRepOperationRename() newFileName:%s fd:%d", newFileName, fd)));		
					
					FileUnlink(fd);
					FileRepMirror_RemoveFileName(newFileName);
				}
				fd = 0;
				
				/****/
				

				/* XXX: hacky, I know. By damn, we need to ship 4.0 some day */
				if (strcmp(newFileName, "global/pg_database") == 0 ||
					strcmp(newFileName, "global/pg_auth") == 0)
				{
					elog(DEBUG1, "FileRepOperationRename()  from filename:%s to %s",
						 fileName, newFileName);	

					errno = 0;
					if (rename(fileName, newFileName) < 0 && errno != ENOENT)
					{
						status = STATUS_ERROR;
						ereport(LOG,
								(errcode_for_file_access(),
								 errmsg("ERROR(filerep): could not rename to:'%s' : %m",
										newFileName),
								 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												   fileRepMessageHeader->fileRepRelationType,
												   fileRepMessageHeader->fileRepOperation),
								 FileRep_errcontext()));					
				
						break;
					}
				}
				else
				{
					elog(DEBUG1, "LINK from filename:%s to %s", fileName, newFileName);	
	
					errno = 0;
					if (link(fileName, newFileName) < 0 && errno != ENOENT) {
						status = STATUS_ERROR;
						ereport(LOG,
								(errcode_for_file_access(),
								errmsg("ERROR(filerep): could not link to:'%s' : %m",
									   newFileName),
								FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
												  fileRepMessageHeader->fileRepRelationType,
												  fileRepMessageHeader->fileRepOperation),
								FileRep_errcontext()));					
						break;
						
					}
				}
				if (newFileName) {
					pfree(newFileName);
					newFileName = NULL;
				}		
								
				break;
			case FileRepOperationShutdown:
				/* NoOp. Just send ACK back. */
				break;
				
			case FileRepOperationInSyncTransition:
				
				FileRep_SetDataState(DataStateInSync, TRUE);
				
				FileRep_SetSegmentState(SegmentStateReady);							
				break;
				
			case FileRepOperationHeartBeat:
				/* NoOp. Just send ACK back. */
				break;
				
			default:
				Assert(0);
				break;
			
		}

		if (fd == -1) {
			Assert(status == STATUS_ERROR);
		}
		
		if (FileRep_IsOperationSynchronous(
								fileRepMessageHeader->fileRepOperation)) {
			if (status != STATUS_OK) {
				fileRepMessageHeader->fileRepAckState = FileRepAckStateMirrorInFault;
			} else {
				fileRepMessageHeader->fileRepAckState = FileRepAckStateCompleted;
			}
			
			if (FileRepAckMirror_Ack(
							  fileRepMessageHeader->fileRepIdentifier,				
							  fileRepMessageHeader->fileRepRelationType,
							  fileRepMessageHeader->fileRepOperation,
							  fileRepMessageHeader->fileRepOperationDescription,
							  fileRepMessageHeader->fileRepAckState) != STATUS_OK) {
			
				status = STATUS_ERROR;
				ereport(WARNING,
						(errmsg("mirror failure, could not queue ACK message to be sent to primary, failover requested"),
						 FileRep_errdetail(fileRepMessageHeader->fileRepIdentifier,
										   fileRepMessageHeader->fileRepRelationType,
										   fileRepMessageHeader->fileRepOperation),
						 FileRep_errcontext()));					

			}
		}		
				
		if (status != STATUS_OK) {
			break;
		}
					
		if (fileName) {
			pfree(fileName);
			fileName = NULL;
		}		
		
		movePositionConsume = TRUE;
	} // while(1)	
	
	if (fileName) {
		pfree(fileName);
		fileName = NULL;
	}		
	
	return status;
}

						
							  							  
/*
 * Find File Descriptor in hash table of open files.
 */
static File
FileRepMirror_GetFileDescriptor(FileName fileName)
{
	OpenFileEntry	*entry;
	File			fd;
	char			key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	
	
	Assert(openFilesTable != NULL);
		
	entry = (OpenFileEntry *) hash_search(
						openFilesTable, 
						(void *) &key, 
						HASH_FIND, 
						NULL);
	
	if (entry == NULL) {
		fd = -1;
		elog(DEBUG1, "FileRepMirror_GetFileDescriptor() fd not found in hash table filename:%s fd:%d", fileName, fd); 
	} else {
		fd = entry->fd;
/*		elog(DEBUG1, "FileRepMirror_GetFileDescriptor() filename:%s fd:%d", fileName, fd); */
	}
	
	return fd;
}

static OpenFileEntry*
FileRepMirror_LookupFileName(FileName fileName)
{
	OpenFileEntry	*entry;
	char			key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	Assert(openFilesTable != NULL);
	
	entry = (OpenFileEntry *) hash_search(
										  openFilesTable, 
										  (void *) &key, 
										  HASH_FIND, 
										  NULL);
	
	if (entry != NULL) {
		elog(DEBUG1, "FileRepMirror_LookupFileName() filename:%s", 
			 fileName);
	}
	
	return entry;
}

/*
 * 
 *
 * fileName is relative path to $PGDATA directory
 */
static int
FileRepMirror_InsertFileName(
				FileName				fileName, 
				File					fd,
				FileRepRelationType_e	fileRepRelationType)
{
	int status = STATUS_OK;

	bool			foundPtr;
	OpenFileEntry	*entry;
	char			key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	
	Assert(openFilesTable != NULL);
	
	elog(DEBUG1, "FileRepMirror_InsertFileName() filename:%s fd:%d ", fileName, fd);
	
	entry = (OpenFileEntry *) hash_search(
							  openFilesTable, 
							  (void *) &key, 
							  HASH_ENTER, 
							  &foundPtr);

	if (entry == NULL) {
		ereport(LOG, 
				(errmsg("ERROR(filerep): could not insert file information into hash table "
						"identifier:'%s' file descriptor:'%d' ",
				 fileName, fd)));
		status = STATUS_ERROR;
		return status;
	}
	
	if (foundPtr) {
		switch (fileRepRelationType) {
			case FileRepRelationTypeFlatFile:
			case FileRepRelationTypeBufferPool:
				
				Assert(entry->fd == fd);
				Assert(entry->fileRepRelationType == fileRepRelationType);
				entry->reference++;
				
				ereport(LOG, 
					(errmsg("LOG(filerep): could not insert file information into hash table, "
							"entry is already inserted "
							"identifier:'%s' file descriptor:'%d' relation type:'%s' ",
							fileName,
							fd,
							FileRepRelationTypeToString[fileRepRelationType])));
				
				break;
			case FileRepRelationTypeAppendOnly:
			case FileRepRelationTypeBulk:
				
				ereport(LOG, 
					(errmsg("ERROR(filerep): could not insert file information into hash table, "
							"entry is already inserted "
							"identifier:'%s' file descriptor:'%d' relation type:'%s' ",
							fileName,
							fd,
							FileRepRelationTypeToString[fileRepRelationType])));
				status = STATUS_ERROR;
				
				break;
			default:
				break;
		}
		
	} else {
		entry->fd = fd;
		entry->fileRepRelationType = fileRepRelationType;
		entry->reference = 1;
	}
	
	return status;
}

/*
 * 
 *
 * fileName is relative path from $PGDATA directory
 */
static void
FileRepMirror_RemoveFileName(FileName  fileName)
{	
	/* NOTE check behavior when two users have concurrently open files */
	
	/* NOTE make it atomic when thread pool is introduced */
	
	OpenFileEntry	*entry;
	char			key[MAXPGPATH+1];
	
	snprintf(key, sizeof(key), "%s", fileName);
	
	Assert(openFilesTable != NULL);
	
	elog(DEBUG1, "FileRepMirror_RemoveFileName() filename:%s ", fileName);
	
	entry = (OpenFileEntry *) hash_search(
						  openFilesTable, 
						  (void *) &key, 
						  HASH_FIND, 
						  NULL);
	Assert(entry != NULL);
	
	if (entry->reference > 1) {
		entry->reference--;
	} else {
		/* File should be closed. */
		/*Assert(FileIsNotOpen(entry->fd));  Not public routine. */
		hash_search(openFilesTable, (void *) &key, HASH_REMOVE, NULL);
	}
	return;
}

/*
 * See checkpath2(path) in commands/filespace.c for comments.
 */
static int
FileRepMirror_Validation(FileName fileName)
{
	int			 mirrorStatus = FileRepStatusSuccess;
	char        *parentdir	  = NULL;
	char        *errordir     = NULL;
	struct stat	 st;
	
	errno = 0;
	/* Does the path exist? */
	if (stat(fileName, &st) >= 0)
	{
		mirrorStatus = FileRepStatusDirectoryExist;
		errordir = fileName;
		goto exit;
	}
	
	/* path does not exist, run checks on parent */
	parentdir = pstrdup(fileName);
	get_parent_directory(parentdir);
		
	/* Does the parent directory exist? */
	if (stat(parentdir, &st) < 0)
	{
		mirrorStatus = FileRepStatusNoSuchFileOrDirectory;
		errordir = parentdir;
		goto exit;
	}
		
	/* Is it actually a directory? */
	if (! S_ISDIR(st.st_mode))
	{
		mirrorStatus = FileRepStatusNotDirectory;
		errordir = parentdir;
		goto exit;
	}
		
	/* Check write permissions */
	if (access(parentdir, W_OK|X_OK) < 0)
	{
		mirrorStatus = FileRepStatusNoPermissions;
		errordir = fileName;
		goto exit;
	}

exit:
	if (mirrorStatus != FileRepStatusSuccess)
	{
		Assert(errordir != NULL);
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): %s: %s", 
						errordir, FileRepStatusToString[mirrorStatus])));
	}
	if (parentdir)
	{
		pfree(parentdir);
	}
	return mirrorStatus;
}

static void
FileRepMirror_DropFilesFromDir(FileName fileName)
{
	DIR				*dir = NULL;
	struct dirent	*de;
	struct stat		st;
	char			path[MAXPGPATH+1];
	
	errno = 0;
	/* Does the path exist? */
	if (stat(fileName, &st) >= 0)
	{
		/* Is it actually a directory? */
		if (! S_ISDIR(st.st_mode))
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("LOG(filerep): %s : Not a directory : %m", fileName)));
			goto exit;
		}
				
		/* Drop from the directory flat files */
		if ((dir = AllocateDir(fileName)) == NULL)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("LOG(filerep): %s : Error reading directory : %m", fileName)));
			goto exit;
		}
		
		while ((de = ReadDir(dir, fileName)) != NULL)
		{
			if (strcmp(de->d_name, ".") == 0 ||
				strcmp(de->d_name, "..") == 0)
				continue;
			
			sprintf(path, "%s/%s", fileName, de->d_name);
			
			errno = 0;
			if (unlink(path))
			{
				if (errno == EPERM)
				{
					stat(path, &st);
					if (S_ISDIR(st.st_mode))
						continue;
				}	
					
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("LOG(filerep): %s : unlink failed : %m", path)));
			}
		}
	}

exit:
	if (dir)
	{
		FreeDir(dir);
	}
	return;
}

/*
 *
 */
static int
SegmentFileFillZero(File fd, int64 writeOffset)
{
	int		status = STATUS_OK;
	int64	startOffset = 0;
	int64	seekResult;
	
	char	zbuffer[BLCKSZ];

	Assert(writeOffset < RELSEG_SIZE * BLCKSZ);
	
	MemSet(zbuffer, 0, sizeof(zbuffer));
	
	errno = 0;
	startOffset = FileSeek(fd, 0L, SEEK_END);
	
	if (startOffset < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): could not seek to the end of file : %m")));
		status = STATUS_ERROR;		
		return status;
	}
	
	if (startOffset >= writeOffset)
	{
		return status;
	}
	
	Assert(startOffset < writeOffset);
	
	errno = 0;
	seekResult = FileSeek(fd, startOffset, SEEK_SET);
	if (seekResult != startOffset)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): could not seek in file to position " INT64_FORMAT ": %m", 
				        startOffset)));
		status = STATUS_ERROR;		
		return status;
	}
		
	while (startOffset < writeOffset) 
	{
		errno = 0;
		
		if ((int) FileWrite(fd, zbuffer, BLCKSZ) != (int) BLCKSZ) {
			int			save_errno = errno;
			
			/*
			 * If we fail to make the file, delete it to release disk space
			 */
			
			/* if write didn't set errno, assume problem is no disk space */
			errno = save_errno ? save_errno : ENOSPC;
			
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("ERROR(filerep): could not zero segment file : %m")));
			status = STATUS_ERROR;
			return status;
		}
		startOffset += BLCKSZ;
	}
	
	errno = 0;
	if (FileSync(fd) != 0) 
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("ERROR(filerep): could not fsync file : %m")));
		status = STATUS_ERROR;
	}
	
	return status;
}

/*
 *
 */
static int
FileRepMirror_Drop(FileName fileName)
{
	File	fd = 0;
	int		fileMode = 0600; 
	int		status = STATUS_OK;
	
	fd = FileRepMirror_GetFileDescriptor(fileName);

	if (fd < 0) 
	{
		errno = 0;
		fd = PathNameOpenFile(fileName,
							  O_WRONLY,
							  fileMode);	
		
		if (fd < 0 && errno == ENOENT) 
		{
			fd = 0;
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("LOG(filerep): file was already dropped identifier:'%s' : %m", 
							fileName)));
			return status;
		}							
		
		if (fd < 0) 
		{
			status = STATUS_ERROR;
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("ERROR(filerep): could not open file to issue drop:'%s' : %m", 
							fileName)));
			
			return status;
		}	
	} 
	else 
	{
		FileRepMirror_RemoveFileName(fileName);
	}

	FileUnlink(fd);
	
	return status;
}

/*
 *
 */
static int
FileRepMirror_DropBufferPoolSegmentFiles(FileName  fileName, int segmentFileNum)
{
	char	tmp[MAXPGPATH+1];
	char	fileNameLocal[MAXPGPATH+1];
	
	int		status = STATUS_OK;
	int		constantPrefixLen;
	int		ii;
	glob_t	g;

	sprintf(tmp, "%s.*", fileName);
	
	constantPrefixLen = strlen(tmp) - 1;

	ii = glob(tmp, 0, 0, &g);

	/* glob success */
	if (ii == 0)
	{
		size_t n;
		for (n = 0; n < g.gl_pathc; ++n)
		{
			char	*match = g.gl_pathv[n];
			int		scanCount;
			int		segmentFileNumLocal;
			
			if (strncmp(match, tmp, constantPrefixLen) != 0)
			{
				elog(LOG, "LOG(filerep): File name pattern problem prefix:'%s' ", tmp);
				continue;
			}
			
			scanCount = sscanf(&match[constantPrefixLen], "%u", &segmentFileNumLocal);
			
			if (segmentFileNumLocal <= 0)
			{
				elog(LOG, "LOG(filerep): Invalid segment file num:'%u' ", segmentFileNumLocal);
				continue;
			}
			
			if (segmentFileNumLocal <= segmentFileNum)
			{
				continue;
			}
			
			sprintf(fileNameLocal, "%s.%u", fileName, segmentFileNumLocal);
			
			status = FileRepMirror_Drop(fileNameLocal);
			
			if (status != STATUS_OK)
			{
				break;
			}
		}
		
		globfree(&g);
	}
	
	return status;
}
