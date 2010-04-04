/*
 *  cdbfilerep.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved.
 *
 */

/*
 * FileRep process (main thread) forked by postmaster.
 *
 * Responsibilities of this module.
 *		*) 
 *
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <assert.h>
#include <sys/time.h>

#include "miscadmin.h"
#include "catalog/catalog.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerepmirror.h"
#include "cdb/cdbfilerepmirrorack.h"
#include "cdb/cdbfilerepconnserver.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "libpq/pqsignal.h"
#include "postmaster/fork_process.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/memutils.h"

/****************************************************************
 * FILEREP PROCESS definitions
 ****************************************************************/
/*
 * GUC parameter
 *			file_rep_message_body_length
 *
 * Size (in bytes) for FileRep Message Body (contains user data). 
 * Default is 32 kB.
 * NOTE add to guc.c "extern uint32 file_rep_message_body_length"
 */
int file_rep_message_body_length = 32 * 1024;

static FileRepState_e fileRepState = FileRepStateNotInitialized;
	/* state of FileRep process */
FileRepRole_e	fileRepRole = FileRepRoleNotInitialized;
SegmentState_e	segmentState = SegmentStateNotInitialized;
DataState_e		dataState = DataStateNotInitialized;

char *fileRepPrimaryHostAddress = NULL;
int fileRepPrimaryPort = -1;
char *fileRepMirrorHostAddress = NULL;
int fileRepMirrorPort = -1;
bool fileRepFullResync = false;

FileRepConsumerProcIndex_e 	fileRepProcIndex = FileRepMessageTypeXLog;

static	MemoryContext	fileRepMemoryContext;
/* 
 * Parameters set by signal handlers for later service n the main loop 
 */
static volatile sig_atomic_t reloadConfigFile = false;

static volatile sig_atomic_t shutdownRequested = false;
	/* graceful shutdown performed by SIGUSR2 signal from postmaster */

static volatile sig_atomic_t immediateShutdownRequested = false;
	/* graceful shutdown performed by SIGQUIT signal from postmaster */

static volatile sig_atomic_t childTermination = false;
	/* child termination of the main file rep process */

static volatile sig_atomic_t stateChangeRequested = false;
	/* state change informed by SIGUSR1 signal from postmaster */

static void FileRep_SigHupHandler(SIGNAL_ARGS);
static void FileRep_ImmediateShutdownHandler(SIGNAL_ARGS);
static void FileRep_ShutdownHandler(SIGNAL_ARGS);
static void FileRep_ChildTerminationHandler(SIGNAL_ARGS);
static void FileRep_StateHandler(SIGNAL_ARGS);

static bool FileRep_IsAnyChildAlive(void);
static bool FileRep_IsAnyBackendChildAlive(void);
static void FileRep_SignalChildren(int signal, bool backendsOnly);
static bool FileRep_ProcessSignals(void);
static void FileRep_ConfigureSignals(void);

#define MaxFileRepSubProc 17	

/**
 * the level of logging for filerep process messages (having to do with process create/destroy)
 * It can be used only for testing purpose (set to LOG). Logging in signal handlers is not allowed, can cause deadlock.
 * Remove the logs when code gets more stable.
 */
#define FILEREP_PROCESS_LOG_LEVEL DEBUG5

typedef struct FileRepSubProc {
	
	pid_t					pid;
	FileRepProcessType_e	fileRepProcessType;
	
} FileRepSubProc;
			
static FileRepSubProc FileRepSubProcList[MaxFileRepSubProc] =
{
	{0, FileRepProcessTypeNotInitialized},
	{0, FileRepProcessTypeMain},
	{0, FileRepProcessTypePrimarySender},
	{0, FileRepProcessTypeMirrorReceiver},
	{0, FileRepProcessTypeMirrorConsumer},
	{0, FileRepProcessTypeMirrorSenderAck},
	{0, FileRepProcessTypePrimaryReceiverAck},
	{0, FileRepProcessTypePrimaryConsumerAck},
	{0, FileRepProcessTypePrimaryRecovery},
	{0, FileRepProcessTypeMirrorConsumerWriter},
	{0, FileRepProcessTypeMirrorConsumerAppendOnly1},
	{0, FileRepProcessTypeMirrorConsumerAppendOnly2},
	{0, FileRepProcessTypeResyncManager},
	{0, FileRepProcessTypeResyncWorker1},
	{0, FileRepProcessTypeResyncWorker2},
	{0, FileRepProcessTypeResyncWorker3},
	{0, FileRepProcessTypeResyncWorker4},
};


/*
 * GUC parameter
 *			file_rep_num_log_entries = 512;
 * 
 * Number of log entries maintained in memory for troubleshooting.
 */
#define	file_rep_num_log_entries 512

static FileRepLogEntry_s fileRepLog[file_rep_num_log_entries];

static	int	logIndex = 0;

static	int fileRepInsertedLogEntries = 0; 

static void FileRep_ReportLogEntry(int	index);

const char*
FileRepProcessTypeToString[] = {
	"FileRep Uninitialized Process",
	"FileRep Main Process", /* note that this one is actually changed when we set the title */
	"FileRep Primary Sender Process",
	"FileRep Mirror Receiver Process",
	"FileRep Mirror Consumer Process",

	"FileRep Mirror Sender Ack Process",
	"FileRep Primary Receiver Ack Process",
	"FileRep Primary Consumer Ack Process",
	"FileRep Primary Recovery Process",
	"FileRep Mirror Consumer Writer Process",

	"FileRep Mirror Consumer Append Only 1 Process",
	"FileRep Mirror Consumer Append Only 2 Process",

	"FileRep Primary Resync Manager Process",

	"FileRep Primary Resync Worker 1 Process",
	"FileRep Primary Resync Worker 2 Process",
	"FileRep Primary Resync Worker 3 Process",
	"FileRep Primary Resync Worker 4 Process",
};

const char*
FileRepRoleToString[] = {
	"Role not Initialized",
	"Role not Configured",
	"Primary Role",
	"Mirror Role",
};

const char*
SegmentStateToString[] = {
	"Not Initialized",
	"Initialization and Recovery",
	"Transition to InResync",	
	"Transition to InSync",
	"Up and Running",
	"Change Tracking Disabled",
	"In Fault",
	"In Shutdown of Backends",
	"In Shutdown",
	"In Immediate Shutdown",
};

const char*
DataStateToString[] = {
	"Not Initialized",
	"In Sync",
	"In Change Tracking",
	"In Resync",
};

const char*
FileRepStateToString[] = {
	"Not Initialized",
	"Initialization and Recovery",
	"Up and Running",
	"In Fault",
	"In Shutdown of Backends",
	"In Shutdown",
	"In Immediate Shutdown",	
};
						  
const char*
FileRepOperationToString[] = {
	"Open",
	"Write",
	"Flush (fsync)",
	"Close",
	"Flush (fsync) and Close",
	"Truncate",
	"Create",
	"Drop",
	"Reconcile xlog",
	"Link or Rename",
	"Shutdown",
	"In Sync Transition",
	"Heart Beat",
	"Not Specified",
};

const char *
FileRepRelationTypeToString[] = {
	"Flat File",
	"Append Only",
	"Buffer Pool",
	"Bulk",
	"Directory",
	"Control Message",
	"Not Specified",
};

const char *
FileRepAckStateToString[] = {
	"Not Initialized",
	"Waiting for Ack",
	"Ack Completed",
	"Mirror In Fault",
};

const char*
FileRepConsumerProcIndexToString[] = {
	"xlog",
	"writer",
	"append only 1",
	"append only 2",
	"shutdown",
	"undefined"
};

/****************************************************************
 * FILEREP_SHARED MEMORY 
 ****************************************************************/
/*
 * FileRep Shared Memory for communication of messages to be mirrored
 *		a) between backends and FileRep process on a primary
 *		b) between Receiver and Consumer threads on a mirror
 *	
 *	FileRep Shared Memory is initialized by Postmaster process
 *	before FileRep Process is 
 *
 * Messages are inserted into shared memory in wraparound mode (from
 * beginPosition to wraparoundPosition and then back to beginPosition).  
 * Messages are consumed in order (first inserted first consumed). 
 *
 * Messages (FileRepMessage_s) that are mirrored are variable sizes. 
 * Message size depends on operation type.
 *		a) Write operation sends data in addition to metadata.
 *		b) Open, Flush, Close operation send metadata only.
 *
 * Variable messages size means that
 *		a) Messages are inserted to different position in shared memory
 *		b) Messages are not alligned with endPosition
 *		  	
 * beginPosition
 *	|	 -------------------------------------------------
 *	|	| FileRepShmem_s								 |
 * 	 =>  -------------------------------------------------
 *		| FileRepShmemMessageDescr_s | FileRepMessage_s  |
 *		 -------------------------------------------------
 *		| FileRepShmemMessageDescr_s | FileRepMessage_s  |
 *		 -------------------------------------------------
 *		|												 |
 *		 -------------------------------------------------
 *		|												 |
 *		 -------------------------------------------------
 *		| FileRepShmemMessageDescr_s | FileRepMessage_s  |
 *		 -------------------------------------------------  <= endPosition
 *													  ^
 *													  |
 *											wraparoundPosition
 */

/*
 * GUC parameter
 *			file_rep_mirror_consumer_process_count
 *
 * Number of processes to consume (perform file operations) messages on mirror.
 */
int		file_rep_mirror_consumer_process_count = 4;

/*
 * GUC parameter
 *			file_rep_buffer_size
 *
 * Size (in bytes) for FileRep Shared Memory. 
 * Default=2MB MIN=1MB MAX=16MB
 */
int		file_rep_buffer_size = 2 * 1024 * 1024;

/*
 * GUC parameter
 *			file_rep_ack_buffer_size
 *
 * Size (in bytes) for FileRepAck Shared Memory. 
 * Default=512kB MIN=128kB MAX=1MB
 */
int		file_rep_ack_buffer_size = 512 * 1024;

/*
 * GUC parameter
 *			file_rep_retry = 300;
 * 
 * Number of retries to insert message into shared memory. Buffer
 * may be full due to i.e. network down, mirror host down, 
 * slow processing, ...
 */
int		file_rep_retry = 1500;  // 136 seconds   300 was old value

/**
 * GUC parameter
 *          file_rep_min_data_before_flush
 *
 * Once a client's buffer has >= this amount of data (in kb) then the client will flush
 */
int file_rep_min_data_before_flush = 128 * 1024;

int fileRepShmemArrayCount = 4;

/*
FileRepShmem_s *fileRepShmem = NULL;
*/
FileRepShmem_s	*fileRepShmemArray[FILEREP_MAX_PROC];

FileRepShmem_s *fileRepAckShmem = NULL;

static void FileRep_ShmemInitArray(void);

FileRepIpcShmem_s	*fileRepIpcArray[FILEREP_MAX_IPC_ARRAY];

int
FileRepSemas(void)
{
	/* 
	 *	PRIMARY
	 *		1) backend and sender (FileRepShmem_s = 2)
	 *		2) Receiver Ack and Consumer Ack (FileRepAckShmem_s = 2)
	 *		3) Consumer Ack and backend (IsOperationCompleted = 1)
	 *		4) backend and backend (InsertNewHashEntry = 1)
	 *
	 *	MIRROR
	 *		1) receiver and consumer XLOG (FileRepShmem_s = 2)
	 *		2) receiver and consumer bgwriter (FileRepShmem_s =2)
	 *		3) receiver and consumer Append Only 1 (FileRepShmem_s = 2)
	 *		4) receiver and consumer Append Only 2 (FileRepShmem_s = 2)
	 *		5) consumers and sender ack (FileRepAckShmem_s = 2)
	 *
	 */
	return(FILEREP_MAX_SEMAPHORES);
}

Size
FileRepIpc_ShmemSize(void)
{
	Size	size;
	
	size = sizeof(FileRepIpcShmem_s);
	
	size = mul_size(size, FILEREP_MAX_IPC_ARRAY);
	
	return size;
}

void
FileRepIpc_ShmemInit(void)
{
	int		ii;
	bool	foundPtr;
	char label[16];
		
	for (ii=0; ii < FILEREP_MAX_IPC_ARRAY; ii++)
	{
		sprintf(label, "FileRep Ipc %d", ii);
		fileRepIpcArray[ii] = (FileRepIpcShmem_s *) ShmemInitStruct(label,
																	sizeof(FileRepIpcShmem_s), 
																	&foundPtr);
		
		if (fileRepIpcArray[ii] == NULL) {
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 (errmsg("Not enough shared memory for Mirroring."))));
		}	
		
		if (! foundPtr) {
			MemSet(fileRepIpcArray[ii], 0, sizeof(FileRepIpcShmem_s));
		}
		
		PGSemaphoreCreate(&fileRepIpcArray[ii]->semP);
		PGSemaphoreCreate(&fileRepIpcArray[ii]->semC);
		
		fileRepIpcArray[ii]->refCountSemP = 0;
		fileRepIpcArray[ii]->refCountSemC = 0;		
	}
}

void
FileRep_IpcWait(PGSemaphoreData sem)
{
	PGSemaphoreLockInterruptable(&sem);
}

void
FileRep_IpcSignal(PGSemaphoreData sem, int refCount)
{
	int ii;

	for (ii=0; ii < refCount; ii++)
	{
		PGSemaphoreUnlock(&sem);
	}	
}

static FileRepShmem_s *FileRep_ShmemInitOneBuffer(const char *bufferLabel, int shmemSize, int bufSize)
{
	bool	foundPtr;
	FileRepShmem_s *result;

	Assert( shmemSize >= bufSize + sizeof(FileRepShmem_s));

	result = (FileRepShmem_s *)
			ShmemInitStruct(bufferLabel, shmemSize, &foundPtr);

	if (result == NULL) {
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				(errmsg("Not enough shared memory for Mirroring."))));
	}

	if (! foundPtr) {
		MemSet(result, 0, sizeof(FileRepShmem_s));
	}

	result->positionBegin =
		((char *) result) + sizeof(FileRepShmem_s);

	MemSet(result->positionBegin, 0, bufSize);

	result->positionEnd =
		result->positionBegin + bufSize;

	result->positionInsert = result->positionBegin;

	result->positionConsume = result->positionBegin;

	result->positionWraparound = result->positionEnd;

	result->insertCount = 0;
	
	result->consumeCount = 0;
	
	result->state = FileRepStateNotInitialized;

	elog(LOG, "FileRep_ShmemInit result:%p insert:%p end:%p ", 
		 result, 
		 result->positionBegin,
		 result->positionEnd);

	
	/*
	 * positionWraparound is initialized
	 * when message is inserted at the positionBegin.
	 */

	return result;
}

/* 
 * FileRep_ShmemInit()
 *			Allocate and initialize FileRep Shared Memory 
 *
 */
void 
FileRep_ShmemInit(void)
{
	fileRepShmemArray[fileRepProcIndex] = FileRep_ShmemInitOneBuffer("FileRep Primary Mirror Messages",
							FileRep_ShmemSize(), FileRep_ShmemSize() - sizeof(FileRepShmem_s));
	
	fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex = IndexIpcArrayShmem;
		
	elog(LOG, "FileRep_ShmemInit shmemArray:%p array index:'%d' ", 
		 fileRepShmemArray[fileRepProcIndex], fileRepProcIndex);
}

/*
 * FileRepAck_ShmemInit()
 *			Allocate and initialize FileRepAck Shared Memory 
 *
 */
void 
FileRepAck_ShmemInit(void)
{
	fileRepAckShmem = FileRep_ShmemInitOneBuffer("FileRepAck Primary Mirror Messages",
							FileRepAck_ShmemSize(), file_rep_ack_buffer_size );
	
	fileRepAckShmem->ipcArrayIndex = IndexIpcArrayAckShmem;
}

/*
 * FileRep_ShmemSize()
 *		Calculate size required for shared memory for mirroring
 */
Size 
FileRep_ShmemSize(void)
{
	Size	size;

	size = sizeof(FileRepShmem_s);
	
	size = add_size(size, file_rep_buffer_size);
	size = mul_size(size, fileRepShmemArrayCount);
	
	return size;
}

/*
 * FileRepAck_ShmemSize()
 *		Calculate size required for shared memory for acknowledgement 
 * NOTES add call to ipci.c (grep BgWriter)
 */	
Size
FileRepAck_ShmemSize(void)
{
	Size	size;
	
	/* shared memory for acknowledgement */
	size = sizeof(FileRepShmem_s);
	
	size = add_size(size, file_rep_ack_buffer_size);

	return size;
}

static void
FileRep_ShmemInitArray(void)
{
	int	ii;
	
	elog(LOG, "FileRep_ShmemInitArray() position begin:'%p' position end:'%p' ",
		 fileRepShmemArray[0]->positionBegin,
		 fileRepShmemArray[0]->positionEnd);
	
	for (ii = 0; ii < fileRepShmemArrayCount; ii++) {
		
		fileRepShmemArray[ii]->positionBegin =
			((char *) fileRepShmemArray[ii]) + sizeof(FileRepShmem_s);
		
		fileRepShmemArray[ii]->positionEnd =
				fileRepShmemArray[ii]->positionBegin + file_rep_buffer_size;
		
		elog(LOG, "FileRep_ShmemInitArray() position begin:'%p' position end:'%p' index:'%d' ",
			 fileRepShmemArray[ii]->positionBegin,
			 fileRepShmemArray[ii]->positionEnd, ii);
		
		
		fileRepShmemArray[ii]->positionInsert = fileRepShmemArray[ii]->positionBegin;
		
		fileRepShmemArray[ii]->positionConsume = fileRepShmemArray[ii]->positionBegin;
		
		fileRepShmemArray[ii]->positionWraparound = fileRepShmemArray[ii]->positionEnd;
		
		fileRepShmemArray[ii]->insertCount = 0;
		
		fileRepShmemArray[ii]->consumeCount = 0;
		
		fileRepShmemArray[ii]->state = FileRepStateNotInitialized;
		
		fileRepShmemArray[ii]->ipcArrayIndex = ii+1;
		
		if ((ii + 1) < fileRepShmemArrayCount) {
			fileRepShmemArray[ii+1] = (FileRepShmem_s *)fileRepShmemArray[ii]->positionEnd;
		}
	}	
	
	return;
}

static void
FileRep_ShmemReInitOneBuffer(FileRepShmem_s *result, int shmemSize, int bufSize)
{
	
	Assert( shmemSize >= bufSize + sizeof(FileRepShmem_s));
	
	if (result == NULL) {
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 (errmsg("shared memory for Mirroring not initialized."))));
	}
	
	
	MemSet(result, 0, sizeof(FileRepShmem_s));
	
	result->positionBegin =
	((char *) result) + sizeof(FileRepShmem_s);
	
	MemSet(result->positionBegin, 0, bufSize);
	
	result->positionEnd =
	result->positionBegin + bufSize;
	
	result->positionInsert = result->positionBegin;
	
	result->positionConsume = result->positionBegin;
	
	result->positionWraparound = result->positionEnd;
	
	result->insertCount = 0;
	
	result->consumeCount = 0;
	
	result->state = FileRepStateNotInitialized;
	
	elog(LOG, "FileRep_ShmemReInitOneBuffer() result:%p insert:%p end:%p ", 
		 result, 
		 result->positionBegin,
		 result->positionEnd);
	
	
	/*
	 * positionWraparound is initialized
	 * when message is inserted at the positionBegin.
	 */
	
}

/* 
 * FileRep_ShmemInit()
 *			Allocate and initialize FileRep Shared Memory 
 *
 */
static void 
FileRep_ShmemReInit(void)
{
	FileRep_ShmemReInitOneBuffer(fileRepShmemArray[fileRepProcIndex],
								 FileRep_ShmemSize(), 
								 FileRep_ShmemSize() - sizeof(FileRepShmem_s));
	
	fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex = IndexIpcArrayShmem;
	
	fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->refCountSemC = 0;
	fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->refCountSemP = 0;
	
	PGSemaphoreReset(&fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->semC);
	PGSemaphoreReset(&fileRepIpcArray[fileRepShmemArray[fileRepProcIndex]->ipcArrayIndex]->semP);

	elog(LOG, "FileRep_ShmemReInit() shmemArray:%p array index:'%d' ", 
		 fileRepShmemArray[fileRepProcIndex], fileRepProcIndex);
}

/*
 * FileRepAck_ShmemReInit()
 *			Allocate and initialize FileRepAck Shared Memory 
 *
 */
static void 
FileRepAck_ShmemReInit(void)
{
	FileRep_ShmemReInitOneBuffer(fileRepAckShmem,
								 FileRepAck_ShmemSize(), 
								 file_rep_ack_buffer_size);
	
	fileRepAckShmem->ipcArrayIndex = IndexIpcArrayAckShmem;
	
	fileRepIpcArray[fileRepAckShmem->ipcArrayIndex]->refCountSemC = 0;
	fileRepIpcArray[fileRepAckShmem->ipcArrayIndex]->refCountSemP = 0;
	
	PGSemaphoreReset(&fileRepIpcArray[fileRepAckShmem->ipcArrayIndex]->semC);
	PGSemaphoreReset(&fileRepIpcArray[fileRepAckShmem->ipcArrayIndex]->semP);		
	
	elog(LOG, "FileRepAck_ShmemReInit() fileRepAckShmem:%p  ", 
		 fileRepAckShmem);
}

char *
FileRepAck_ReserveShmem(
					 uint32			msgLength,
					 uint32			*spareField)
{
	bool	insert = false;
	int		retry = 0;
	bool	wait = FALSE;
	char	*msgPositionInsert = NULL;
	FileRepShmemMessageDescr_s *fileRepShmemMessageDescr;
		
	while ((insert == false) && (retry < file_rep_retry)) {
		
		LWLockAcquire(FileRepAckShmemLock, LW_EXCLUSIVE);
		
		if (wait == TRUE)
		{
			fileRepIpcArray[fileRepAckShmem->ipcArrayIndex]->refCountSemP--;
			wait = FALSE;
		}
		
		if (fileRepAckShmem->positionInsert > fileRepAckShmem->positionConsume) {
			if ((fileRepAckShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepAckShmem->positionEnd) {
				insert = true;
			} else {
				/* Message wraparound */
				if (fileRepAckShmem->positionConsume != fileRepAckShmem->positionBegin) {
					fileRepAckShmem->positionWraparound = fileRepAckShmem->positionInsert;
					fileRepAckShmem->positionInsert = fileRepAckShmem->positionBegin;
					LWLockRelease(FileRepAckShmemLock);
					continue;
				}
			}
		}
		
		if (fileRepAckShmem->positionInsert < fileRepAckShmem->positionConsume) {
			if ((fileRepAckShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepAckShmem->positionConsume) {
				insert = true;
			} 
		}	
		
		if (fileRepAckShmem->positionInsert == fileRepAckShmem->positionConsume) {
			if ((fileRepAckShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepAckShmem->positionEnd) {
				insert = true;
			} else {
				/* Message wraparound */
				fileRepAckShmem->positionWraparound = fileRepAckShmem->positionInsert;
				fileRepAckShmem->positionInsert = fileRepAckShmem->positionBegin;
				LWLockRelease(FileRepAckShmemLock);
				continue;

			}			
		}
		
		if (insert == true) {
			if ((fileRepAckShmem->positionInsert + 
				 msgLength + sizeof(FileRepShmemMessageDescr_s)) == 
				fileRepAckShmem->positionConsume) {
				
				insert = false;
			} else {
				msgPositionInsert = fileRepAckShmem->positionInsert;
				
				MemSet(msgPositionInsert, 0, 
					   msgLength + sizeof(FileRepShmemMessageDescr_s));
				
				Assert(msgPositionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s) < fileRepAckShmem->positionEnd);
				
				fileRepShmemMessageDescr = 
				(FileRepShmemMessageDescr_s*) msgPositionInsert;
				
				fileRepShmemMessageDescr->messageLength = msgLength;
				
				fileRepShmemMessageDescr->messageState = 
				FileRepShmemMessageStateReserved; 
				
				fileRepAckShmem->insertCount++;
				
				*spareField = fileRepAckShmem->insertCount; 
				
				fileRepAckShmem->positionInsert = msgPositionInsert + 
						msgLength + sizeof(FileRepShmemMessageDescr_s);
			}
		}
		
		if (insert == FALSE)
		{
			fileRepIpcArray[fileRepAckShmem->ipcArrayIndex]->refCountSemP++;
			wait = TRUE;
		}
		
		LWLockRelease(FileRepAckShmemLock);
		
		if (insert == false) {
			if (FileRepSubProcess_IsStateTransitionRequested())
			{
				break;
			}	
			
			FileRep_IpcWait(fileRepIpcArray[fileRepAckShmem->ipcArrayIndex]->semP);
						
			//FileRep_Sleep10ms(retry);

			retry++;
		}
		
	}
	
	if (insert == false) {
		ereport(LOG,
				(errmsg("Reserving FileRepAck Shared Memory fail.")));
	}
	
	return (msgPositionInsert);
}

/*
 * Ensure next insertPosition is also available. That will happen 
 * only when insert and consume messages are aligned. 
 * next insertPosition == consumePosition is TRUE when no space
 * for message insert is available.
 */
/* TEST SCENARIOS 
 *		a) wraparound that is/ is not aligned with endPosition 
 *		b) no more space to insert message
 *		c) no more messages to consume
 *
 */
char *
FileRep_ReserveShmem(
					 FileRepShmem_s		*fileRepShmem,
					 uint32				msgLength,
					 uint32				*spareField,
					 FileRepOperation_e	fileRepOperation)
{
	bool	insert = false;
	int		retry = 0;
	bool	wait = FALSE;
	char	*msgPositionInsert = NULL;
	FileRepShmemMessageDescr_s *fileRepShmemMessageDescr;
	
	while ((insert == false) && (retry < file_rep_retry)) {
		
		LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
		
		if (! FileRep_IsIpcSleep(fileRepOperation))
		{					
			if (wait == TRUE)
			{
				fileRepIpcArray[fileRepShmem->ipcArrayIndex]->refCountSemP--;
				wait = FALSE;
			}
		}
		
		if (fileRepShmem->positionInsert > fileRepShmem->positionConsume) {
			if ((fileRepShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepShmem->positionEnd) {
				insert = true;
			} else {
				/* Message wraparound */
				if (fileRepShmem->positionConsume != fileRepShmem->positionBegin) {
					fileRepShmem->positionWraparound = fileRepShmem->positionInsert;
					fileRepShmem->positionInsert = fileRepShmem->positionBegin;
					LWLockRelease(FileRepShmemLock);
					continue;
				}
			}
		}
		
		if (fileRepShmem->positionInsert < fileRepShmem->positionConsume) {
			if ((fileRepShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepShmem->positionConsume) {
				insert = true;
			} 
		}	
		
		if (fileRepShmem->positionInsert == fileRepShmem->positionConsume) {
			if ((fileRepShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepShmem->positionEnd) {
				insert = true;
			} else {
				/* Message wraparound */
				fileRepShmem->positionWraparound = fileRepShmem->positionInsert;
				fileRepShmem->positionInsert = fileRepShmem->positionBegin;
				LWLockRelease(FileRepShmemLock);
				continue;
			}
		}
		
		if (insert == true) {
			if ((fileRepShmem->positionInsert + 
				 msgLength + sizeof(FileRepShmemMessageDescr_s)) == 
				fileRepShmem->positionConsume) {
				
				insert = false;
			} else {				
				msgPositionInsert = fileRepShmem->positionInsert;
				
				MemSet(msgPositionInsert, 0, 
					   msgLength + sizeof(FileRepShmemMessageDescr_s));
				
				Assert(msgPositionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s) < fileRepShmem->positionEnd);
				
				fileRepShmemMessageDescr = 
				(FileRepShmemMessageDescr_s*) msgPositionInsert;
				
				fileRepShmemMessageDescr->messageLength = msgLength;
				
				fileRepShmemMessageDescr->messageState = 
				FileRepShmemMessageStateReserved; 
				
				fileRepShmem->insertCount++;
				
				*spareField = fileRepShmem->insertCount; 
				
				fileRepShmem->positionInsert = msgPositionInsert + 
						msgLength + sizeof(FileRepShmemMessageDescr_s);
			}
		}
			
		if (! FileRep_IsIpcSleep(fileRepOperation))
		{					
			if (insert == FALSE)
			{
				fileRepIpcArray[fileRepShmem->ipcArrayIndex]->refCountSemP++;
				wait = TRUE;
			}
		}
		
		LWLockRelease(FileRepShmemLock);
		
		if (insert == false) {
			if (FileRepSubProcess_IsStateTransitionRequested())
			{
				break;
			}		
			
			if (FileRep_IsIpcSleep(fileRepOperation))
			{					
				FileRep_Sleep10ms(retry);
			}
			else
			{
				FileRep_IpcWait(fileRepIpcArray[fileRepShmem->ipcArrayIndex]->semP);
			}

			retry++;
		}
	}
	
	if (insert == false) {
		ereport(LOG,
			(errmsg("Reserving FileRep Shared Memory fail.")));
	}
	
	return (msgPositionInsert);
}

/****************************************************************
 * ERROR REPORTING
 ****************************************************************/
int
FileRep_errcontext(void)
{
	
	errcontext(
			   "file rep role:'%s' "
			   "primary-mirror pair state:'%s' "
			   "segment state:'%s' "
			   "process pid:'%d' "
			   "process name:'%s' "
			   "filerep state:'%s' ",
			   FileRepRoleToString[fileRepRole],
			   DataStateToString[dataState],
			   SegmentStateToString[segmentState],
			   getpid(),
			   FileRepProcessTypeToString[fileRepProcessType],
			   FileRepStateToString[FileRepSubProcess_GetState()]);
	
	return 0;
}

int
FileRep_errdetail_Shmem(void)
{
	
	errcontext(
		   "position begin:'%p' "
		   "position end:'%p' "
		   "position insert:'%p' "
		   "position consume:'%p' "
		   "position wraparound:'%p' "
		   "insert count:'%d' "
		   "consume count:'%d' ",
			fileRepShmemArray[fileRepProcIndex]->positionBegin,
			fileRepShmemArray[fileRepProcIndex]->positionEnd,
			fileRepShmemArray[fileRepProcIndex]->positionInsert,
			fileRepShmemArray[fileRepProcIndex]->positionConsume,
			fileRepShmemArray[fileRepProcIndex]->positionWraparound,
			fileRepShmemArray[fileRepProcIndex]->insertCount,
			fileRepShmemArray[fileRepProcIndex]->consumeCount);

	return 0;
}

int
FileRep_errdetail_ShmemAck(void)
{
	
	errcontext(
			  "position ack begin:'%p' "
			  "position ack end:'%p' "
			  "position ack insert:'%p' "
			  "position ack consume:'%p' "
			  "position ack wraparound:'%p' "
			  "insert count ack:'%d' "
			  "consume count ack:'%d' ",
			  fileRepAckShmem->positionBegin,
			  fileRepAckShmem->positionEnd,
			  fileRepAckShmem->positionInsert,
			  fileRepAckShmem->positionConsume,
			  fileRepAckShmem->positionWraparound,
			  fileRepAckShmem->insertCount,
			  fileRepAckShmem->consumeCount);
	
	return 0;
}

int
FileRep_errdetail(
				  FileRepIdentifier_u		fileRepIdentifier,
				  FileRepRelationType_e		fileRepRelationType,
				  FileRepOperation_e		fileRepOperation)
{
	char						*fileName = NULL;

	fileName = FileRep_GetFileName(
								   fileRepIdentifier,
								   fileRepRelationType);

	errdetail(
			  "identifier:'%s' "
			  "operation:'%s' "
			  "relation type:'%s' ",
			  (fileName == NULL) ? "":fileName,
			  FileRepOperationToString[fileRepOperation],
			  FileRepRelationTypeToString[fileRepRelationType]);	
		
	if (fileName) {
		pfree(fileName);
		fileName = NULL;
	}				  
	return 0;
}

int
FileRep_ReportRelationPath(
				  char*			filespaceLocation,
				  RelFileNode	relFileNode,
				  uint32		segmentFileNum)
{
	char		path[MAXPGPATH+1];
	
	FormRelationPath(
					 path,
					 filespaceLocation,
					 relFileNode);
	
	if (segmentFileNum != 0) 
	{
		sprintf(path, "%s.%u", path, segmentFileNum);
	}
	
	errdetail("relation path:'%s' ", path);
	
	return 0;
}


/*
 * static FileRepLogEntry *fileRepLog[file_rep_num_log_entries];
 * static logIndex
 */
void
FileRep_InsertLogEntry(
					char*					routineName,
					FileRepIdentifier_u		fileRepIdentifier,
					FileRepRelationType_e	fileRepRelationType,
					FileRepOperation_e		fileRepOperation,
					pg_crc32				fileRepMessageHeaderCrc,
					pg_crc32				fileRepMessageBodyCrc,
					FileRepAckState_e		fileRepAckState,
					uint32					localCount,
					uint32					fileRepMessageCount)
{
	
	strcpy(fileRepLog[logIndex].routineName, routineName);
	
	fileRepLog[logIndex].fileRepRelationType = fileRepRelationType;
	
	fileRepLog[logIndex].fileRepOperation = fileRepOperation;
	
	fileRepLog[logIndex].fileRepIdentifier = fileRepIdentifier;
	
	fileRepLog[logIndex].fileRepMessageHeaderCrc = fileRepMessageHeaderCrc;
	
	fileRepLog[logIndex].fileRepMessageBodyCrc = fileRepMessageBodyCrc;
	
	fileRepLog[logIndex].fileRepAckState = fileRepAckState;
	
	fileRepLog[logIndex].localCount = localCount;
	
	fileRepLog[logIndex].fileRepMessageCount = fileRepMessageCount; 
	
	fileRepLog[logIndex].shmemPositionBegin = fileRepShmemArray[fileRepProcIndex]->positionBegin;
	fileRepLog[logIndex].shmemPositionEnd = fileRepShmemArray[fileRepProcIndex]->positionEnd;
	fileRepLog[logIndex].shmemPositionInsert = fileRepShmemArray[fileRepProcIndex]->positionInsert;
	fileRepLog[logIndex].shmemPositionConsume = fileRepShmemArray[fileRepProcIndex]->positionConsume;
	fileRepLog[logIndex].shmemPositionWraparound = fileRepShmemArray[fileRepProcIndex]->positionWraparound;
	fileRepLog[logIndex].shmemInsertCount = fileRepShmemArray[fileRepProcIndex]->insertCount;
	fileRepLog[logIndex].shmemConsumeCount = fileRepShmemArray[fileRepProcIndex]->consumeCount;

	fileRepLog[logIndex].shmemAckPositionBegin = fileRepAckShmem->positionBegin;
	fileRepLog[logIndex].shmemAckPositionEnd = fileRepAckShmem->positionEnd;
	fileRepLog[logIndex].shmemAckPositionInsert = fileRepAckShmem->positionInsert;
	fileRepLog[logIndex].shmemAckPositionConsume = fileRepAckShmem->positionConsume;
	fileRepLog[logIndex].shmemAckPositionWraparound = fileRepAckShmem->positionWraparound;
	fileRepLog[logIndex].shmemAckInsertCount = fileRepAckShmem->insertCount;
	fileRepLog[logIndex].shmemAckConsumeCount = fileRepAckShmem->consumeCount;

	fileRepLog[logIndex].fileRepRole = fileRepRole;
	
	fileRepLog[logIndex].segmentState = segmentState;
	
	fileRepLog[logIndex].dataState = dataState;
	
	fileRepLog[logIndex].fileRepState = FileRepSubProcess_GetState();
	
	fileRepLog[logIndex].procPid = getpid();
	
	fileRepLog[logIndex].fileRepProcessType = fileRepProcessType;
	
	gettimeofday(&fileRepLog[logIndex].logTime, NULL);
	
	logIndex++;
	
	if (logIndex == file_rep_num_log_entries)
		logIndex = 0;
	
	if (fileRepInsertedLogEntries < file_rep_num_log_entries)
		fileRepInsertedLogEntries++;
}

/*
 * 30 parameters
 */

static void
FileRep_ReportLogEntry(int	index)
{
	char		*fileName = NULL;
	char		temp[128];
	char		buftime[128];
	pg_time_t	tt;
	
	tt = (pg_time_t) fileRepLog[index].logTime.tv_sec;
	pg_strftime(temp, sizeof(temp), "%a %b %d %H:%M:%S.%%06d %Y %Z",
				pg_localtime(&tt, session_timezone));
	snprintf(buftime, sizeof(buftime), temp, fileRepLog[index].logTime.tv_usec);
	
	fileName = FileRep_GetFileName(
								   fileRepLog[index].fileRepIdentifier,
								   fileRepLog[index].fileRepRelationType);	

	ereport(LOG,
			(errmsg("LOG(filerep): 	time:'%s' "
					"routine:'%s' "
					"identifier:'%s' "
					"operation:'%s' "
					"relation type:'%s' "
					"local count:'%d' "
					"msg count:'%d' "
					"msg header crc:'%u' "
					"msg body crc:'%u' "
					"msg ack state:'%s' "
					"position begin:'%p' "
					"position end:'%p' "
					"position insert:'%p' "
					"position consume:'%p' "
					"position wraparound:'%p' "
					"shmem insert count:'%d' "
					"shmem consume count:'%d' "
					"position ack begin:'%p' "
					"position ack end:'%p' "
					"position ack insert:'%p' "
					"position ack consume:'%p' "
					"position ack wraparound:'%p' "
					"shmem insert count ack:'%d' "
					"shmem consume count ack:'%d' "
					"file rep role:'%s' "
					"primary-mirror pair state:'%s' "
					"segment state:'%s' "
					"filerep state:'%s' "
					"process name:'%s' "
					"process pid:'%d' ",
					buftime,
					fileRepLog[index].routineName,
					(fileName == NULL) ? "" : fileName,
					FileRepOperationToString[fileRepLog[index].fileRepOperation],
					FileRepRelationTypeToString[fileRepLog[index].fileRepRelationType],
					fileRepLog[index].localCount,
					fileRepLog[index].fileRepMessageCount,
					fileRepLog[index].fileRepMessageHeaderCrc,
					fileRepLog[index].fileRepMessageBodyCrc,
					FileRepAckStateToString[fileRepLog[index].fileRepAckState],
					fileRepLog[index].shmemPositionBegin,
					fileRepLog[index].shmemPositionEnd,
					fileRepLog[index].shmemPositionInsert,
					fileRepLog[index].shmemPositionConsume,
					fileRepLog[index].shmemPositionWraparound,
					fileRepLog[index].shmemInsertCount,
					fileRepLog[index].shmemConsumeCount,
					fileRepLog[index].shmemAckPositionBegin,
					fileRepLog[index].shmemAckPositionEnd,
					fileRepLog[index].shmemAckPositionInsert,
					fileRepLog[index].shmemAckPositionConsume,
					fileRepLog[index].shmemAckPositionWraparound,
					fileRepLog[index].shmemAckInsertCount,
					fileRepLog[index].shmemAckConsumeCount,
					FileRepRoleToString[fileRepLog[index].fileRepRole],
					DataStateToString[fileRepLog[index].dataState],
					SegmentStateToString[fileRepLog[index].segmentState],
					FileRepStateToString[FileRepSubProcess_GetState()],
					FileRepProcessTypeToString[fileRepLog[index].fileRepProcessType], 
					getpid())));
			 
					
					
	if (fileName) {
		pfree(fileName);
		fileName = NULL;
	}		
}
	
void
FileRep_ReportLog(void)
{
	int	ii;
	int numEntries;

	numEntries = Min(fileRepInsertedLogEntries, file_rep_num_log_entries);
	
	for (ii = logIndex; ii < numEntries; ii++) 
	{
		FileRep_ReportLogEntry(ii);
	}
	
	for (ii = 0; ii < logIndex; ii++) {
		FileRep_ReportLogEntry(ii);
	}
	
}

/****************************************************************
 *  FILEREP PROCESS
 ****************************************************************/

/*
 *  SIGHUP signal from postmaster
 *  It re-loads configuration file at next convenient time.
 */
static void 
FileRep_SigHupHandler(SIGNAL_ARGS)
{
	reloadConfigFile = true;
	elog(FILEREP_PROCESS_LOG_LEVEL, "FileRep_SigHupHandler()");
}

/* 
 *  SIGQUIT signal from postmaster
 */
static void 
FileRep_ImmediateShutdownHandler(SIGNAL_ARGS)
{
	immediateShutdownRequested = true;
	elog(FILEREP_PROCESS_LOG_LEVEL, "FileRep_ImmediateShutdownHandler()");
}

/*
 *  SIGUSR2 signal from postmaster
 */
static void 
FileRep_ShutdownHandler(SIGNAL_ARGS)
{
	shutdownRequested = true;
	elog(FILEREP_PROCESS_LOG_LEVEL, "FileRep_ShutdownHandler()");
}

/*
 *  SIGCHLD signal from FileRep process children
 */
static void 
FileRep_ChildTerminationHandler(SIGNAL_ARGS)
{
	childTermination = true;
	elog(FILEREP_PROCESS_LOG_LEVEL, "FileRep_ChildTerminationHandler()");
}

/*
 *  SIGUSR1 signal from postmaster
 *  It signals about data and/or segment state change.
 */
static void 
FileRep_StateHandler(SIGNAL_ARGS)
{
	stateChangeRequested = true;
	elog(FILEREP_PROCESS_LOG_LEVEL, "FileRep_StateHandler() stateChangeRequested");
}

/*
 * NOTE - should we report fault
 *
 * @param backendsOnly if true then only filerep processes that act as backends will be signalled.
 */
static void
FileRep_SignalChildren(int signal, bool backendsOnly)
{
	int ii;

	for (ii=0; ii < MaxFileRepSubProc; ii++) {
		FileRepSubProc *subProc = &FileRepSubProcList[ii];
		
		if (subProc->pid == 0) {
		    continue;
        }
		    
        if ( backendsOnly &&
             ! FileRepIsBackendSubProcess( subProc->fileRepProcessType)) {
            continue;
        }

        if (kill(subProc->pid, signal) < 0)  {
            ereport(LOG,
                    (errmsg("Signal to children failed pid %ld signal %d: %m",
                     (long) subProc->pid, signal)));

        }
	}
	return;
}

static bool
FileRep_IsAnyChildAlive(void)
{
	int i;

	for (i=0; i < MaxFileRepSubProc; i++)
	{
		if ( FileRepSubProcList[i].pid != 0 )
			return true;
	}
	return false;
}

static bool
FileRep_IsAnyBackendChildAlive(void)
{
	int i;

	for (i=0; i < MaxFileRepSubProc; i++)
	{
		if ( FileRepSubProcList[i].pid != 0 &&
		     FileRepIsBackendSubProcess(FileRepSubProcList[i].fileRepProcessType))
        {
		    return true;
        }
	}
	return false;
}

/*
 * Log the death of a child process.
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	if (WIFEXITED(exitstatus))
		ereport(lev,
				
				/*------
		 translator: %s is a noun phrase describing a child process, such as
		 "server process" */
				(errmsg("%s (PID %d) exited with exit code %d",
						procname,
						pid, WEXITSTATUS(exitstatus))));
	else if (WIFSIGNALED(exitstatus))
#if defined(WIN32)
		ereport(lev,
				
				/*------
		 translator: %s is a noun phrase describing a child process, such as
		 "server process" */
				(errmsg("%s (PID %d) was terminated by exception 0x%X",
						procname,
						pid, WTERMSIG(exitstatus)),
				 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value.")));
#elif defined(HAVE_DECL_SYS_SIGLIST) && HAVE_DECL_SYS_SIGLIST
	ereport(lev,
			
			/*------
	 translator: %s is a noun phrase describing a child process, such as
	 "server process" */
			// strsignal() is preferred over the deprecated use of sys_siglist, on platforms that support it.
			// Solaris and Linux do support it, but I think MAC OSX doesn't?
			(errmsg("%s (PID %d) was terminated by signal %d: %s",
					procname, 
					pid, WTERMSIG(exitstatus),
					WTERMSIG(exitstatus) < NSIG ?
					sys_siglist[WTERMSIG(exitstatus)] : "(unknown)")));
#else
	{
		// If we don't have strsignal or sys_siglist, do our own translation
		const char *signalName;
		
		switch (WTERMSIG(exitstatus))
		{
			case SIGINT:	
				signalName = "SIGINT";
				break;
			case SIGTERM:	
				signalName = "SIGTERM";
				break;
			case SIGQUIT:	
				signalName = "SIGQUIT";
				break;
			case SIGSTOP:	
				signalName = "SIGSTOP";
				break;
			case SIGUSR1:
				signalName = "SIGUSR1";
				break;
			case SIGUSR2:	
				signalName = "SIGUSR2";
				break;
			default:	
				signalName = "(unknown)";
				break;
		}
		
		ereport(lev,
				
				/*------
		 translator: %s is a noun phrase describing a child process, such as
		 "server process" */
			    (errmsg("%s (PID %d) was terminated by signal %d: %s",
						procname, 
						pid, WTERMSIG(exitstatus), signalName)));
	}
#endif
	else
		ereport(lev,
				
				/*------
		 translator: %s is a noun phrase describing a child process, such as
		 "server process" */
				(errmsg("%s (PID %d) exited with unrecognized status %d",
						FileRepProcessTypeToString[fileRepProcessType], 
						pid, exitstatus)));
}

/*
 *  FileRep_ProcessSignals()
 *
 */
static bool
FileRep_ProcessSignals()
{
	bool processExit = false;
	
	if (reloadConfigFile)
	{
		FileRep_SignalChildren(SIGHUP, false);
		reloadConfigFile = false;
		ProcessConfigFile(PGC_SIGHUP);
	}
	
	if (shutdownRequested)
	{
	    SegmentState_e segmentState;
		getPrimaryMirrorStatusCodes(NULL, &segmentState, NULL);

		shutdownRequested = false;

		if ( segmentState == SegmentStateShutdownFilerepBackends )
		{
            elog(FILEREP_PROCESS_LOG_LEVEL, "Filerep backends shutdown requested");
            FileRep_SetState(FileRepStateShutdownBackends);
    		FileRep_SignalChildren(SIGUSR2, true);
		}
		else
		{
            elog(FILEREP_PROCESS_LOG_LEVEL, "Filerep shutdown requested");

            /* inform mirror that gracefull shutdown is in progress */
            if (fileRepRole == FileRepPrimaryRole)
            {
                if (MyProc == NULL)
                {
                    InitAuxiliaryProcess();

                    InitBufferPoolBackend();
                }

                FileRepPrimary_MirrorShutdown();
            }

    		processExit = true;
    		FileRep_SetState(FileRepStateShutdown);
    		FileRep_SignalChildren(SIGUSR2, false);
		}
	}

	if (immediateShutdownRequested)
	{
	    elog(FILEREP_PROCESS_LOG_LEVEL, "Filerep Immediate shutdown requested");
		immediateShutdownRequested = false;
		FileRep_SignalChildren(SIGQUIT, false);
		processExit = true;
		FileRep_SetState(FileRepStateImmediateShutdown);
	}
	
	/*
	 * Immediate shutdown if postmaster is not alive to avoid
	 * manual cleanup.
	 */
	if (! PostmasterIsAlive(true))
	{
	    elog(FILEREP_PROCESS_LOG_LEVEL, "Filerep: postmaster is gone, entering immediate shutdown");
		FileRep_SignalChildren(SIGQUIT, false);
		processExit = true;
		FileRep_SetState(FileRepStateImmediateShutdown);
	}
	
	if (childTermination)
	{
		int i;
		int term_pid;

		childTermination = false;

		for (;;)
		{
			/* Don't leave child processes defunct. */
#ifdef HAVE_WAITPID
			int term_status;
			errno = 0;
			term_pid = waitpid(-1, &term_status, WNOHANG);
#else
			union wait term_status;
			errno = 0;
			term_pid = wait3(&term_status, WNOHANG, NULL);
#endif

			if (term_pid == 0)
				break;
			if ( errno == ECHILD)
				break;

			/* NOTE see do_reaper() */
			for (i=0; i < MaxFileRepSubProc; i++)
			{
				if (term_pid == FileRepSubProcList[i].pid)
				{
					/*
					if (! ((term_status == 0) &&
						   (i == FileRepProcessTypePrimaryRecovery ||
							i == FileRepProcessTypeResyncManager ||
							i == FileRepProcessTypeResyncWorker1 ||
							i == FileRepProcessTypeResyncWorker2 ||
							i == FileRepProcessTypeResyncWorker3 ||
							i == FileRepProcessTypeResyncWorker4 )))
					{ */
						LogChildExit(LOG, (FileRepProcessTypeToString[i]),
									 FileRepSubProcList[i].pid,
									 term_status);
					//}
					if (term_status != 0 &&
						segmentState != SegmentStateShutdown &&
						segmentState != SegmentStateImmediateShutdown &&
						segmentState != SegmentStateShutdownFilerepBackends)
					{
						FileRep_SetSegmentState(SegmentStateFault);
					}
					FileRepSubProcList[i].pid = 0;
					break;
				}
			}
		}
		errno = 0;
	}

	if (stateChangeRequested)
	{
		getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);
		stateChangeRequested = false;
#ifdef FILEREP_DEBUG
		ereport(LOG,
				(errmsg("LOG(filerep): FileRep_ProcessSignals() "),
						FileRep_errcontext()));		
#endif
		FileRep_SignalChildren(SIGUSR1, false);
	}
	
	return(processExit);
}

/*
 *  FileRep_GetState()
 *  Return state of FileRep process
 */
FileRepState_e 
FileRep_GetState(void)
{
	return fileRepState;
}	

/* NOTE temporary routine */
bool
FileRep_IsInRecovery(void)
{	
	return (segmentState == SegmentStateInitialization);
}

/* NOTE temporary routine */
bool
FileRep_IsInResynchronizeReady(void)
{	
	return (dataState == DataStateInResync &&
		   segmentState != SegmentStateInResyncTransition);	
}

/**
 * Return true if the process type represents a process that acts like
 *   a normal database backend
 */
bool
FileRepIsBackendSubProcess(FileRepProcessType_e processType)
{
    switch(processType)
    {
        case FileRepProcessTypeNotInitialized:
	    case FileRepProcessTypeMain:
	    case FileRepProcessTypePrimarySender:
	    case FileRepProcessTypeMirrorReceiver:
    	case FileRepProcessTypeMirrorConsumer:
	    case FileRepProcessTypeMirrorSenderAck:
    	case FileRepProcessTypePrimaryReceiverAck:
	    case FileRepProcessTypePrimaryConsumerAck:
	    case FileRepProcessTypeMirrorConsumerWriter:
        case FileRepProcessTypeMirrorConsumerAppendOnly1:
        case FileRepProcessTypeMirrorConsumerAppendOnly2:
            return false;
		case FileRepProcessTypePrimaryRecovery:
        case FileRepProcessTypeResyncManager:
        case FileRepProcessTypeResyncWorker1:
        case FileRepProcessTypeResyncWorker2:
        case FileRepProcessTypeResyncWorker3:
        case FileRepProcessTypeResyncWorker4:
            return true;
        default:
            Assert(!"Unknown process type in FileRepIsBackendSubProcess");
            return false;
    }
}

/*
 * NOTE temporary, it will be removed
 */
bool
FileRep_IsInChangeTracking(void)
{
	DataState_e snapDataState;
	bool result;
	
	getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);
	snapDataState = dataState;
	
	result = (((snapDataState == DataStateInChangeTracking && 
			    segmentState != SegmentStateChangeTrackingDisabled) ||
			   (snapDataState == DataStateInResync &&
			    !FileRep_IsInResynchronizeReady() &&
			    !isFullResync())) &&
			  !FileRep_IsInRecovery());
	
	return result;
}

void 
FileRep_SetDataState(DataState_e dataState, bool signalPostmaster)
{
	if (updateDataState(dataState) && signalPostmaster) {
		SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
	}
#ifdef FILEREP_DEBUG
	elog(LOG, "FileRep_SetDataState() state:'%s' MyProcPid:'%d'", 
		 DataStateToString[dataState],
		 getpid());	
#endif
}

void 
FileRep_SetSegmentState(SegmentState_e segmentState)
{	
	if (updateSegmentState(segmentState)) {
		SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
	}
#ifdef FILEREP_DEBUG
	elog(LOG, "FileRep_SetSegmentState() state:'%s' MyProcPid:'%d'", 
		 SegmentStateToString[segmentState],
		 getpid());	
#endif
}

void
FileRep_SetPostmasterReset(void)
{
		SendPostmasterSignal(PMSIGNAL_FILEREP_POSTMASTER_RESET_REQUIRED);
}

/*
 *  Set state in FileRep process and sent signal to postmaster
 */
void 
FileRep_SetState(FileRepState_e fileRepStateLocal)
{
	elog(FILEREP_PROCESS_LOG_LEVEL, "FileRep_SetState(%s, %d) MyProcPid:'%d'",
		 FileRepStateToString[fileRepStateLocal], fileRepStateLocal,
		 getpid());

	switch (fileRepState) {
		case FileRepStateNotInitialized:
		case FileRepStateInitialization:
			
			fileRepState = fileRepStateLocal;
			break;
		case FileRepStateReady:
			
			switch (fileRepStateLocal) {
				case FileRepStateInitialization:
				case FileRepStateFault:
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
				case FileRepStateShutdownBackends:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
					Assert(0);
				case FileRepStateReady:

					break;
                default:
                    Assert(0);
                    break;
			}
			break;
		case FileRepStateFault:
			
			switch (fileRepStateLocal) {
				case FileRepStateFault:
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
				case FileRepStateShutdownBackends:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
				case FileRepStateInitialization:
				case FileRepStateReady:
					break;
                default:
                    Assert(0);
                    break;
			}
			
			break;

        case FileRepStateShutdownBackends:

			switch (fileRepStateLocal) {
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
				case FileRepStateShutdownBackends:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
				case FileRepStateInitialization:
				case FileRepStateReady:

				case FileRepStateFault:
					break;
                default:
                    Assert(0);
                    break;
			}

			break;

		case FileRepStateShutdown:
			
			switch (fileRepStateLocal) {
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
				case FileRepStateInitialization:
				case FileRepStateReady:

				case FileRepStateFault:
					break;					
                default:
                    Assert(0);
                    break;
			}
			
			break;
		case FileRepStateImmediateShutdown:

			break;

        default:
            Assert(0);
            break;
	}

	switch (fileRepState) {
		case FileRepStateReady:
			FileRep_SetSegmentState(SegmentStateReady);
			break;
			
		case FileRepStateFault:
			/* update shared memory configuration 
			   bool updateSegmentState(FAULT);
			   return TRUE if state was updated;
			   return FALSE if state was already set to FAULT
			   change signal to PMSIGNAL_FILEREP_SEGMENT_STATE_CHANGE
			 */
			FileRep_SetSegmentState(SegmentStateFault);
			break;
			
		case FileRepStateInitialization:
        case FileRepStateShutdownBackends:
		case FileRepStateShutdown:
		case FileRepStateImmediateShutdown:
		case FileRepStateNotInitialized:
			/* No operation */
			break;
        default:
            Assert(0);
            break;
	}
#ifdef FILEREP_DEBUG
	elog(LOG, "LOG(filerep)FileRep_SetState() state:'%s' MyProcPid:'%d'",
		 FileRepStateToString[FileRep_GetState()],
		 getpid());
#endif
}

/*
 *
 */
static void
FileRep_ConfigureSignals(void)					 
{

/* Accept Signals */
	/* emergency shutdown */
	pqsignal(SIGQUIT, FileRep_ImmediateShutdownHandler);
	
	/* graceful shutdown */
	pqsignal(SIGUSR2, FileRep_ShutdownHandler);
	
	/* reload configuration file */
	pqsignal(SIGHUP, FileRep_SigHupHandler);
	
	/* child terminated */
	pqsignal(SIGCHLD, FileRep_ChildTerminationHandler);	
	
	/* data or segment state changed */
	pqsignal(SIGUSR1, FileRep_StateHandler);
	
/* Ignore Signals */
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
		
/* Use default action */
	pqsignal(SIGINT, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);
	
}

/*
 *
 */
static pid_t 
FileRep_StartChildProcess(FileRepProcessType_e type)
{
	pid_t	pid;
	
	pid = fork_process();
	
	/* in child */
	if (pid == 0) {    

		
		/* Release postmaster's working memory context */
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextDelete(fileRepMemoryContext);
		fileRepMemoryContext = NULL;
		
		/* check if that is allowed */
		IsUnderPostmaster = TRUE;
		
		fileRepProcessType = type;
					
		FileRepSubProcess_Main();
		proc_exit(0);
	}
	
	/* in parent, fork failed */
	if (pid < 0) {	
		
		ereport(LOG,
				(errmsg("could not fork %s : %m", 
						FileRepProcessTypeToString[type])));
		
		/* it is set in Main routine: FileRep_SetState(FileRepStateFault); */
	}
	
	FileRepSubProcList[type].pid = pid;
	
	/* in parent, successful fork */
	return pid;
}

/*
 * 100 * 10ms + 100 * 50ms + 100 * 100ms = 16 sec
 * file_rep_retry = 300 (max value)
 */
void
FileRep_Sleep10ms(int retry)
{

	if (retry < 100) {
		pg_usleep(10000L); /* 10 ms */
	} else {
		if (retry < 200)
			pg_usleep(50000L); /* 50 ms */
		else {
			pg_usleep(100000L); /* 100 ms */
		}
	}
}

/*
 * 50 * 1ms + 50 * 10ms + 100 * 50ms + 100 * 100ms = 15.55 sec
 * file_rep_retry = 300 (max value)
 */
void
FileRep_Sleep1ms(int retry)
{

	if (retry < 50) {
		pg_usleep(1000L); /* 1 ms */
	} else {
		if (retry < 100) {
			pg_usleep(10000L); /* 10 ms */
		} else {
			if (retry < 200) {
				pg_usleep(50000L); /* 50 ms */
			} else {
				pg_usleep(100000L); /* 100 ms */
			}
		}
	}
}


/*
 *
 */
void
FileRep_CalculateCrc(
					 char*		data,
					 uint32		length,
					 pg_crc32	*crc)
{
	pg_crc32 _crc = 0;

	if (Debug_filerep_crc_on)
	{
		/* Compute Crc */
		INIT_CRC32(_crc);
		
		COMP_CRC32(_crc, (char *) data, length);
		
		FIN_CRC32(_crc);
	}

	*crc = _crc;
}

bool
FileRep_IsIpcSleep(FileRepOperation_e fileRepOperation)
{
	switch (fileRepOperation) {
			
		case FileRepOperationShutdown:
		case FileRepOperationHeartBeat:
			
			return TRUE;	
			
		default:			
			return FALSE;
	}	
	return FALSE;	
}

bool
FileRep_IsOperationSynchronous(FileRepOperation_e fileRepOperation)
{	

	switch (fileRepOperation) {
			
		case FileRepOperationFlush:
		case FileRepOperationCreate:
		case FileRepOperationDrop:
		case FileRepOperationFlushAndClose:
		case FileRepOperationReconcileXLogEof:
		case FileRepOperationRename:
		case FileRepOperationShutdown:
		case FileRepOperationInSyncTransition:
		case FileRepOperationHeartBeat:
			
			return TRUE;	

		case FileRepOperationOpen: 
		case FileRepOperationClose:
		case FileRepOperationTruncate:
		case FileRepOperationWrite:
			
			return FALSE;
			
		case FileRepOperationNotSpecified:
			Assert(0);
			break;

        default:
            Assert(0);
            break;
	}	
	return FALSE;
}

XLogRecPtr 
FileRep_GetXLogRecPtrUndefined(void)
{
	XLogRecPtr r;
	
	r.xlogid = 0;
	r.xrecoff = 0;
	
	return r;
}

/*
 *
 */
FileRepIdentifier_u
FileRep_GetRelationIdentifier(
				char			*mirrorFilespaceLocation,
				RelFileNode		relFileNode,
				int32			segmentFileNum)
{
	FileRepIdentifier_u identifier;
	
	identifier.fileRepRelationIdentifier.relFileNode.spcNode = relFileNode.spcNode;
	identifier.fileRepRelationIdentifier.relFileNode.dbNode = relFileNode.dbNode;
	identifier.fileRepRelationIdentifier.relFileNode.relNode = relFileNode.relNode;
	
	identifier.fileRepRelationIdentifier.segmentFileNum = segmentFileNum;		
	
	if (mirrorFilespaceLocation == NULL)
		sprintf(identifier.fileRepRelationIdentifier.mirrorFilespaceLocation,
				"%s", 
				"");
	else
		sprintf(identifier.fileRepRelationIdentifier.mirrorFilespaceLocation,
				"%s", 
				mirrorFilespaceLocation);	
	
	return identifier;
}

FileRepIdentifier_u 
FileRep_GetDirFilespaceIdentifier(
						 char				*mirrorFilespaceLocation)
{
	
	FileRepIdentifier_u identifier;
	
	identifier.fileRepDirIdentifier.fileRepDirType = FileRepDirFilespace;
	
	identifier.fileRepDirIdentifier.dbDirNode.tablespace = 0;
	identifier.fileRepDirIdentifier.dbDirNode.database = 0;
	
	if (mirrorFilespaceLocation == NULL)
		sprintf(identifier.fileRepDirIdentifier.mirrorFilespaceLocation,
				"%s", 
				"");
	else
		sprintf(identifier.fileRepDirIdentifier.mirrorFilespaceLocation,
				"%s", 
				mirrorFilespaceLocation);	
	
	return identifier;
}

FileRepIdentifier_u 
FileRep_GetDirTablespaceIdentifier(
						 char		*mirrorFilespaceLocation,
						 Oid		tablespace)
{
	
	FileRepIdentifier_u identifier;
	
	identifier.fileRepDirIdentifier.fileRepDirType = FileRepDirTablespace;
	
	identifier.fileRepDirIdentifier.dbDirNode.tablespace = tablespace;
	identifier.fileRepDirIdentifier.dbDirNode.database = 0;
	
	if (mirrorFilespaceLocation == NULL)
		sprintf(identifier.fileRepDirIdentifier.mirrorFilespaceLocation,
				"%s", 
				"");
	else
		sprintf(identifier.fileRepDirIdentifier.mirrorFilespaceLocation,
				"%s", 
				mirrorFilespaceLocation);	
	
	return identifier;
}

FileRepIdentifier_u 
FileRep_GetDirDatabaseIdentifier(
						 char				*mirrorFilespaceLocation,
						 DbDirNode			dbDirNode)
{
	
	FileRepIdentifier_u identifier;
	
	identifier.fileRepDirIdentifier.fileRepDirType = FileRepDirDatabase;
	
	identifier.fileRepDirIdentifier.dbDirNode.tablespace = dbDirNode.tablespace;
	identifier.fileRepDirIdentifier.dbDirNode.database = dbDirNode.database;
	
	if (mirrorFilespaceLocation == NULL)
		sprintf(identifier.fileRepDirIdentifier.mirrorFilespaceLocation,
				"%s", 
				"");
	else
		sprintf(identifier.fileRepDirIdentifier.mirrorFilespaceLocation,
				"%s", 
				mirrorFilespaceLocation);	
	
	return identifier;
}

FileRepIdentifier_u 
FileRep_GetFlatFileIdentifier(
				char*	subDirectory,
				char*	simpleFileName)
{
	
	FileRepIdentifier_u identifier;
	
	strncpy(identifier.fileRepFlatFileIdentifier.directorySimpleName, 
			subDirectory, 
			MAXPGPATH);
	
	strncpy(identifier.fileRepFlatFileIdentifier.fileSimpleName,
			simpleFileName,
			MAXPGPATH);
	
	return identifier;
}

FileRepIdentifier_u 
FileRep_GetUnknownIdentifier(
							  char	*unknownId)
{
	
	FileRepIdentifier_u identifier;
	
	strncpy(identifier.fileRepUnknownIdentifier.unknownId, 
			unknownId, 
			FILEREP_UNKNOWN_IDENTIFIER_LEN);
	
	return identifier;
}

/*
 * $PGDATA/filespace/segment/tablespace_oid/database_oid/relfilenode_oid
 */
FileName
FileRep_GetFileName(
					  FileRepIdentifier_u	 fileRepIdentifier,
					  FileRepRelationType_e	fileRepRelationType)
{
	FileName fileName = NULL;
		
	switch (fileRepRelationType) {
		case FileRepRelationTypeFlatFile:

			fileName = (char*) palloc(
				strlen(fileRepIdentifier.fileRepFlatFileIdentifier.directorySimpleName) +
				strlen(fileRepIdentifier.fileRepFlatFileIdentifier.fileSimpleName) + 2);
			
			sprintf(fileName, "%s/%s",
					fileRepIdentifier.fileRepFlatFileIdentifier.directorySimpleName,
					fileRepIdentifier.fileRepFlatFileIdentifier.fileSimpleName);
		
			break;
		
		case FileRepRelationTypeBulk:
		case FileRepRelationTypeBufferPool:						
		case FileRepRelationTypeAppendOnly:

			fileName = (char*) palloc(MAXPGPATH+1+20); /* + 20 is to add room for . and the segment file num */
			
			FormRelationPath(
							 fileName,
							 fileRepIdentifier.fileRepRelationIdentifier.mirrorFilespaceLocation,
							 fileRepIdentifier.fileRepRelationIdentifier.relFileNode);
			
			if (fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum != 0) 
			{
				sprintf(fileName, "%s.%u", 
						fileName, 
						fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum);
			}

			break;
			
		case FileRepRelationTypeDir:
			
			fileName = (char*) palloc(MAXPGPATH+1);
		
			switch (fileRepIdentifier.fileRepDirIdentifier.fileRepDirType)
			{
				case FileRepDirFilespace:
					sprintf(fileName, "%s",
							fileRepIdentifier.fileRepDirIdentifier.mirrorFilespaceLocation);
					break;
					
				case FileRepDirTablespace:
					if (strcmp(fileRepIdentifier.fileRepDirIdentifier.mirrorFilespaceLocation,
							   "") == 0)
						FormTablespacePath(
										   fileName,
										   NULL,
										   fileRepIdentifier.fileRepDirIdentifier.dbDirNode.tablespace);
					
					else 
						FormTablespacePath(
										   fileName,
										   fileRepIdentifier.fileRepDirIdentifier.mirrorFilespaceLocation,
										   fileRepIdentifier.fileRepDirIdentifier.dbDirNode.tablespace);
					
					break;
					
				case FileRepDirDatabase:
					if (strcmp(fileRepIdentifier.fileRepDirIdentifier.mirrorFilespaceLocation,
							   "") == 0)
						FormDatabasePath(
										 fileName,
										 NULL,
										 fileRepIdentifier.fileRepDirIdentifier.dbDirNode.tablespace,
										 fileRepIdentifier.fileRepDirIdentifier.dbDirNode.database);
					else
						FormDatabasePath(
										 fileName,
										 fileRepIdentifier.fileRepDirIdentifier.mirrorFilespaceLocation,
										 fileRepIdentifier.fileRepDirIdentifier.dbDirNode.tablespace,
										 fileRepIdentifier.fileRepDirIdentifier.dbDirNode.database);
					break;
					
				default:
					Assert(0);
					break;
			}
			break;
		
		case FileRepRelationTypeUnknown:
			
			fileName = (char*) palloc(FILEREP_UNKNOWN_IDENTIFIER_LEN + 1);
			sprintf(fileName, "%s",
					fileRepIdentifier.fileRepUnknownIdentifier.unknownId);			
			break;
			
		case FileRepRelationTypeNotSpecified:
			break;
			
		default:
			Assert(0);	
			break;
	}
		
	return fileName;										  
}		

void
FileRep_GetRelationPath(
					char		*relationPath, 
					RelFileNode relFileNode,
					uint32		segmentFileNum)
{
	char *primaryFilespaceLocation;
	char *mirrorFilespaceLocation;
	
	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
													   relFileNode.spcNode,
													   &primaryFilespaceLocation,
													   &mirrorFilespaceLocation);
	
	FormRelationPath(
					 relationPath,
					 mirrorFilespaceLocation,
					 relFileNode);
	
	if (segmentFileNum != 0) 
	{
		sprintf(relationPath, "%s.%u", 
				relationPath, 
				segmentFileNum);
	}
	
	if (primaryFilespaceLocation != NULL)
		pfree(primaryFilespaceLocation);
	
	if (mirrorFilespaceLocation != NULL)
		pfree(mirrorFilespaceLocation);	
}			
	
/*
 * Postmaster is responsible to start, monitor and exit FileRep process.
 * FileRep process is required only if segment has mirror configured.
 *
 * FileRep process is started on Primary and its Mirror by postmaster at 
 *		*) gpstart
 *		*) gprecoverseg
 *		*) gpaddmirrors (if utility is not deprecated)
 *
 * FileRep process is exited on Primary and its Mirror by postmaster at
 *		*) gpfailover 
 *		*) gpstop
 *
 *
 * FileRep process state transitions
 *
 *					   gpstart       
 *					   gprecoverseg  
 *					   gpaddmirrors  
 *							|
 *						   \|/
 *					-------------------
 *					| INITIALIZATION  |
 *					-------------------
 *							|
 *						   \|/
 *					-------------------
 *					|	  READY		  |
 *					-------------------
 *				gpstop|	         	|
 *          gpfailover|				|
 *					 \|/	       \|/
 *         -------------  gpstop   ---------------
 *         |  SHUTDOWN | <---------|   FAULT     |
 *         ------------- gpfailover---------------
 *                |	
 *	  		     \|/
 *		  ----------------
 *		  |	    DOWN	 | 
 *		  ----------------
 *
 *
 * START
 *		Postmaster forks FileRep process. Input (gp_configuration) 
 *		parameters for FileRep process are stored in shared memory. 
 *		Postmaster-backend process writes input parameters. FileRep 
 *		and Postmaster processes read them (never write).
 *
 *		INPUT parameters (gp_configuration parameters)
 *				Replica Role
 *				Replica State
 *				Primary IP address
 *				Mirror IP address
 *				Primary Port
 *				Mirror Port
 *
 *		After FileRep process completes Initialization phase it
 *      signal SIGUSR1 (reason PMSIGNAL_RECOVERY_CONSISTENT) 
 *      to postmaster. Postmaster (if Primary Role) 
 *		proceeds with File System object and Wal Recovery.
 *		
 *		In addition postmaster
 *			*) initialize and populate shared memory for FileSpaces
 *			*) initialize shared memory for FileRep mirror messages
 *			*) initialize shared memory for FileRep Ack mirror messages
 *			*) initialize and populate shared memory for gp_configuration
 *			*) FS_OBJECTS is TBD
 *			*) dedicate shared memory for maintaining FileRepState_e
 *			   and postmaster PID.
 *
 * MONITOR
 *		Postmaster monitors FileRep process.
 *
 *			If the FileRep process exits unexpectedly, the postmaster 
 *			treats that condition as Fault. Fault is reported to 
 *			Faulty coordinator, that determines recovery procedure.
 *
 *			If the FileRep process detects an error then it sends
 *			signal SIGUSR1 (reason PMSIGNAL_FAULT) to its postmaster 
 *			and waits in "Fault" state. 
 *			The postmaster
 *			treats that condition as Fault. Fault is reported to 
 *			Faulty coordinator, that determines recovery procedure.
 *			
 * EXIT
 *		Postmaster on a segment behaves differently for graceful 
 *		and emergency process termination.
 *			a) Graceful termination
 *					1) Postmaster signals termination to all processes
 *					   except FileRep process
 *					2) After Postmaster receives that all processes
 *					   exit then it signals FileRep process
 *					3) After Postmaster receives that FileRep process
 *					   exit then it cleans shared memory	
 *
 *			b) Emergency termination
 *					1) Postmaster signals termination to all processes
 *					2) After Postmaster receives that all processes
 *					   exit then it cleans shared memory
 *
 *		Postmaster exits the FileRep process by graceful or emergency
 *		termination.
 *			a) Graceful termination is performed by signaling SIGUSR2
 *				to FileRep process on primary and its mirror.
 *				FileRep process in Primary Role
 *					*) ensure all data are flushed
 *						-) no message is located in FileRep shared memory
 *						-) no message is located in FileRep Ack shared memory
 *					*) inform Mirror about Shutdown 
 *						-) Mirror close ACK connection to primary
 *						-) Mirror will not report fault, but wait
 *						   to get shutdown from mirror postmaster
 *						-) flush to persistent storage (file) information 
 *					       for fast recovery (failover optimization)
 *						-) flush to persistent storage (file) information 
 *						   about Replication Role and Replication 
 *						   State (for integrity check) 
 *					*) close connection to mirror
 *					*) clean up (free memory, ...)
 *					*) exit with sending signal SIGCHLD (status=OK)
 *					   to postmaster
 *
 *				FileRep process in Mirror Role
 *					*) ensure Primary did clean shutdown
 *					*) clean up (free memory, ...)
 *					*) exit with sending signal SIGCHLD (status=OK) 
 *					   to postmaster
 *		
 *			b) Emergency termination is performed by signaling SIGQUIT
 *			   to FileRep process.
 *
 *				FileRep process in Primary and Mirror Role aborts
 *				work in progress, close connection, clean up
 *				and exit with sending signal SIGCHLD
 *				(status=ABORT) to postmaster.
 *
 */	
void FileRep_Main(void)
{
	SegmentState_e	segmentState = SegmentStateNotInitialized;

	int status = STATUS_OK;
	
	char title[100];
	
	sigjmp_buf		local_sigjmp_buf;

    COMPILE_ASSERT(ARRAY_SIZE(FileRepProcessTypeToString) == FileRepProcessType__EnumerationCount);
    COMPILE_ASSERT(ARRAY_SIZE(FileRepRoleToString) == FileRepRole_e_Count);
    COMPILE_ASSERT(ARRAY_SIZE(SegmentStateToString) == SegmentState__EnumerationCount);
    COMPILE_ASSERT(ARRAY_SIZE(DataStateToString) == DataState__EnumerationCount);
    COMPILE_ASSERT(ARRAY_SIZE(FileRepStateToString) == FileRepState__EnumerationCount);
    COMPILE_ASSERT(ARRAY_SIZE(FileRepOperationToString) == FileRepOperation__EnumerationCount);
    COMPILE_ASSERT(ARRAY_SIZE(FileRepRelationTypeToString) == FileRepRelationType__EnumerationCount);
    COMPILE_ASSERT(ARRAY_SIZE(FileRepAckStateToString) == FileRepAckState__EnumerationCount);
    COMPILE_ASSERT(ARRAY_SIZE(FileRepConsumerProcIndexToString) == FileRepMessageType__EnumerationCount);

	Insist(IsUnderPostmaster == TRUE);

	fileRepProcessType = FileRepProcessTypeMain;
	
	FileRep_ConfigureSignals();

	main_tid = pthread_self();
	
	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();
		
		/* Report the error to the server log */
		EmitErrorReport();
		
		/*
		 * We can now go away.	Note that because we'll call InitProcess, a
		 * callback will be registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}
	
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;
	
	PG_SETMASK(&UnBlockSig);
	
	/* Create the memory context where cross-transaction state is stored */
	fileRepMemoryContext = AllocSetContextCreate(TopMemoryContext,
										  "FileRep memory context",
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_INITSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);
	
	MemoryContextSwitchTo(fileRepMemoryContext);
	
	FileRep_SetState(FileRepStateInitialization);
	
	FileRep_ShmemReInit();
	FileRepAck_ShmemReInit();

	MemoryContext oldContext = MemoryContextSwitchTo(TopMemoryContext);
	primaryMirrorGetFilerepArguments(&fileRepRole, &segmentState, &dataState, &fileRepPrimaryHostAddress,
	    &fileRepPrimaryPort, &fileRepMirrorHostAddress, &fileRepMirrorPort, &fileRepFullResync );
    MemoryContextSwitchTo(oldContext);
	
	if (fileRepRole == FileRepNoRoleConfigured) {
		ereport(LOG,
			(errmsg("Retrieving Configuration parameters fail.")));
		FileRep_SetState(FileRepStateFault);
		goto shutdown;
	}
		
	snprintf(title, sizeof(title), "%s",
			 fileRepRole == FileRepPrimaryRole ?
			 	"FileRep Primary Main Process" : "FileRep Mirror Main Process");
	init_ps_display(title, "", "", "");
		
	elog(LOG, "FileRep_Main() started with role %d\n\t\t segmentState %d\n\t\t dataState %d\n\t\t"
	            "primary: %s:%d\n\t\tmirror: %s:%d"
	            "\n",
		        fileRepRole, segmentState, dataState,
		        fileRepPrimaryHostAddress, fileRepPrimaryPort, fileRepMirrorHostAddress, fileRepMirrorPort );
	
	if (dataState == DataStateInChangeTracking) 
	{
		if (segmentState == SegmentStateFault)
		{
			FileRep_SetPostmasterReset();
		}
		
		if (FileRep_StartChildProcess(FileRepProcessTypePrimaryRecovery) < 0) {
			status = STATUS_ERROR;
			FileRep_SetPostmasterReset();
		}
		
		/* enter shutdown state -- wait for the postmaster to stop us */
		goto shutdown;
	}
		
	Insist(dataState == DataStateInSync || dataState == DataStateInResync);
	
	switch (fileRepRole) {
		case FileRepPrimaryRole:
				
			if (FileRep_StartChildProcess(FileRepProcessTypePrimaryReceiverAck) < 0) {
				status = STATUS_ERROR;
				break;
			}			
				
			if (FileRep_StartChildProcess(FileRepProcessTypePrimarySender) < 0) {
				status = STATUS_ERROR;
				break;
			}
	
			if (FileRep_StartChildProcess(FileRepProcessTypePrimaryConsumerAck) < 0) {
				status = STATUS_ERROR;
				break;
			}			
	
			if (FileRep_StartChildProcess(FileRepProcessTypePrimaryRecovery) < 0) {
				status = STATUS_ERROR;
				break;
			}	

			if (dataState == DataStateInResync) 
			{
				if (FileRep_StartChildProcess(FileRepProcessTypeResyncManager) < 0) {
					status = STATUS_ERROR;
					break;
				}						
				if (FileRep_StartChildProcess(FileRepProcessTypeResyncWorker1) < 0) {
					status = STATUS_ERROR;
					break;
				}		
				if (FileRep_StartChildProcess(FileRepProcessTypeResyncWorker2) < 0) {
					status = STATUS_ERROR;
					break;
				}										
				if (FileRep_StartChildProcess(FileRepProcessTypeResyncWorker3) < 0) {
					status = STATUS_ERROR;
					break;
				}										
				if (FileRep_StartChildProcess(FileRepProcessTypeResyncWorker4) < 0) {
					status = STATUS_ERROR;
					break;
				}										
				
			}
		
												
			break;
		
		case FileRepMirrorRole:
			
			FileRep_ShmemInitArray();
			
			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorReceiver) < 0) {
				status = STATUS_ERROR;
				break;
			}			

			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorConsumer) < 0) {
				status = STATUS_ERROR;
				break;
			}
		
			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorConsumerWriter) < 0) {
				status = STATUS_ERROR;
				break;
			}
			
			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorConsumerAppendOnly1) < 0) {
				status = STATUS_ERROR;
				break;
			}
			
			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorConsumerAppendOnly2) < 0) {
				status = STATUS_ERROR;
				break;
			}
			
			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorSenderAck) < 0) {
				status = STATUS_ERROR;
				break;
			}
						
			break;
		case FileRepNoRoleConfigured:
		case FileRepRoleNotInitialized:
		default:
			Assert(0);
			break;

	}			
			
	if (status != STATUS_OK) {
		FileRep_SetState(FileRepStateFault);
		goto shutdown;
	}
shutdown:
	while (FileRep_ProcessSignals() == false) {

		if ( FileRep_GetState() == FileRepStateShutdownBackends &&
		     ! FileRep_IsAnyBackendChildAlive())
        {
            /* tell postmaster that filerep backends have been shutdown */
            SendPostmasterSignal(PMSIGNAL_PRIMARY_MIRROR_ALL_BACKENDS_SHUTDOWN);
        }

		/*
		 * check and process any signals received 
		 * The routine returns TRUE if the received signal requests
		 * process shutdown.
		 */

		pg_usleep(1000000L); // 1sec
	}
	
	if ( FileRep_GetState() == FileRepStateShutdown)
	{
		/* wait for all children to terminate, unless immediate shutdown is reached */
		while ( FileRep_IsAnyChildAlive())
		{
			FileRep_ProcessSignals();
			if ( FileRep_GetState() == FileRepStateImmediateShutdown )
				break;
			if ( ! FileRep_IsAnyChildAlive())
				break;
			pg_usleep(1000000L); // 1sec
		}
	}

	if (FileRep_GetState() == FileRepStateShutdown) {
		proc_exit(0);
	} else {
		/* ImmediateShutdown */			
		Assert(FileRep_GetState() == FileRepStateImmediateShutdown);
		proc_exit(2);	
	}
} // FileRep_Main()


