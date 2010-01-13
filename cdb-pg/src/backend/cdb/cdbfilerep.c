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
#include "libpq/pqsignal.h"
#include "postmaster/fork_process.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
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
static void FileRep_SignalChildren(int signal);
static bool FileRep_ProcessSignals(void);
static void FileRep_ConfigureSignals(void);

#define MaxFileRepSubProc 9	

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

static void FileRep_ReportLogEntry(int	index);

const char*
FileRepProcessTypeToString[] = {
	"FileRep Uninitialized Process",
	"FileRep Main Process",
	"FileRep Primary Sender Process",
	"FileRep Mirror Receiver Process",
	"FileRep Mirror Consumer Process",
	"FileRep Mirror Sender Ack Process",
	"FileRep Primary Receiver Ack Process",
	"FileRep Primary Consumer Ack Process",
	"FileRep Primary Recovery Process",
	"FileRep Unknown Process",
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
	"In Recovery",				/* may be removed */
	"Up and Running",
	"In Fault",
	"In Shutdown",
	"In Immediate Shutdown",	/* may be removed */
};

const char*
DataStateToString[] = {
	"Not Initialized",
	"In Sync",
	"In Change Logging",
	"In Resync",
};

const char*
FileRepStateToString[] = {
	"Not Initialized",
	"Initialization and Recovery",
	"Up and Running",
	"In Fault",
	"In Suspend",
	"In Shutdown",
	"In Immediate Shutdown",	/* may be removed */
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
	"Not Specified",
};

const char *
FileRepRelationTypeToString[] = {
	"Flat File",
	"Append Only",
	"Buffer Pool",
	"Bulk",
	"File",
	"Directory",
	"Not Specified",
};

const char *
FileRepAckStateToString[] = {
	"Not Initialized",
	"Waiting for Ack",
	"Ack Completed",
	"Mirror In Fault",
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
int		file_rep_retry = 300;

/**
 * GUC parameter
 *          file_rep_min_data_before_flush
 *
 * Once a client's buffer has >= this amount of data (in kb) then the client will flush
 */
int file_rep_min_data_before_flush = 128 * 1024;

FileRepShmem_s *fileRepShmem = NULL;
	/* It points to FileRep shared memory */

FileRepShmem_s *fileRepAckShmem = NULL;

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
		memset(result, 0, sizeof(FileRepShmem_s));
	}

	result->positionBegin =
		((char *) result) + sizeof(FileRepShmem_s);

	memset(result->positionBegin, 0, bufSize);

	result->positionEnd =
		result->positionBegin + bufSize;

	result->positionInsert = result->positionBegin;

	result->positionConsume = result->positionBegin;

	result->positionWraparound = result->positionEnd;

	result->insertCount = 0;
	
	result->consumeCount = 0;
	
	result->state = FileRepStateNotInitialized;

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
	fileRepShmem = FileRep_ShmemInitOneBuffer("FileRep Primary Mirror Messages",
							FileRep_ShmemSize(), file_rep_buffer_size );
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
	elog(DEBUG1, "FileRep_ShmemSize() size:%u", (uint)size);	
	
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
	elog(DEBUG1, "FileRepAck_ShmemSize() size:%u", (uint)size);	
	return size;
}


char *
FileRepAck_ReserveShmem(
					 uint32			msgLength,
					 uint32			*spareField)
{
	bool	insert = false;
	int		retry = 0;
	char	*msgPositionInsert = NULL;
	FileRepShmemMessageDescr_s *fileRepShmemMessageDescr;
		
	while ((insert == false) && (retry < file_rep_retry)) {
		
		LWLockAcquire(FileRepAckShmemLock, LW_EXCLUSIVE);
		
		if (fileRepAckShmem->positionInsert > fileRepAckShmem->positionConsume) {
			if ((fileRepAckShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepAckShmem->positionEnd) {
				insert = true;
			} else {
				/* Message wraparound */
				if (fileRepAckShmem->positionConsume != fileRepAckShmem->positionBegin) {
					fileRepAckShmem->positionWraparound = fileRepAckShmem->positionInsert;
					fileRepAckShmem->positionInsert = fileRepAckShmem->positionBegin;
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
			}			
		}
		
		if (insert == true) {
			if ((fileRepAckShmem->positionInsert + 
				 msgLength + sizeof(FileRepShmemMessageDescr_s)) == 
				fileRepAckShmem->positionConsume) {
				
				insert = false;
			} else {
				msgPositionInsert = fileRepAckShmem->positionInsert;
				
				memset(msgPositionInsert, 0, 
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
		
		LWLockRelease(FileRepAckShmemLock);
		
		if (insert == false) {
			FileRep_Sleep10ms(retry);
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
					 uint32			msgLength,
					 uint32			*spareField)
{
	bool	insert = false;
	int		retry = 0;
	char	*msgPositionInsert = NULL;
	FileRepShmemMessageDescr_s *fileRepShmemMessageDescr;
	
	while ((insert == false) && (retry < file_rep_retry)) {
		
		LWLockAcquire(FileRepShmemLock, LW_EXCLUSIVE);
		
		if (fileRepShmem->positionInsert > fileRepShmem->positionConsume) {
			if ((fileRepShmem->positionInsert + msgLength + sizeof(FileRepShmemMessageDescr_s)) < 
				fileRepShmem->positionEnd) {
				insert = true;
			} else {
				/* Message wraparound */
				if (fileRepShmem->positionConsume != fileRepShmem->positionBegin) {
					fileRepShmem->positionWraparound = fileRepShmem->positionInsert;
					fileRepShmem->positionInsert = fileRepShmem->positionBegin;
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
			}
		}
		
		if (insert == true) {
			if ((fileRepShmem->positionInsert + 
				 msgLength + sizeof(FileRepShmemMessageDescr_s)) == 
				fileRepShmem->positionConsume) {
				
				insert = false;
			} else {				
				msgPositionInsert = fileRepShmem->positionInsert;
				
				memset(msgPositionInsert, 0, 
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
		
		LWLockRelease(FileRepShmemLock);
		
		if (insert == false) {
			FileRep_Sleep10ms(retry); 
			retry++;
			elog(DEBUG1, "FileRep_ReserveShmem() RETRY:%d", 
				 retry);	
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
			   "filerep state:'%s' ",
			   FileRepRoleToString[fileRepRole],
			   DataStateToString[dataState],
			   SegmentStateToString[segmentState],
			   getpid(),
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
				fileRepShmem->positionBegin,
				fileRepShmem->positionEnd,
				fileRepShmem->positionInsert,
				fileRepShmem->positionConsume,
				fileRepShmem->positionWraparound,
				fileRepShmem->insertCount,
				fileRepShmem->consumeCount);
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
			  fileName,
			  FileRepOperationToString[fileRepOperation],
			  FileRepRelationTypeToString[fileRepRelationType]);

	if (fileName) {
		pfree(fileName);
		fileName = NULL;
	}		
			  
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
	
	fileRepLog[logIndex].shmemPositionBegin = fileRepShmem->positionBegin;
	fileRepLog[logIndex].shmemPositionEnd = fileRepShmem->positionEnd;
	fileRepLog[logIndex].shmemPositionInsert = fileRepShmem->positionInsert;
	fileRepLog[logIndex].shmemPositionConsume = fileRepShmem->positionConsume;
	fileRepLog[logIndex].shmemPositionWraparound = fileRepShmem->positionWraparound;
	fileRepLog[logIndex].shmemInsertCount = fileRepShmem->insertCount;
	fileRepLog[logIndex].shmemConsumeCount = fileRepShmem->consumeCount;

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
					fileName,
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

	for (ii = logIndex; ii < file_rep_num_log_entries; ii++) {
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
	elog(DEBUG1, "FileRep_SigHupHandler()");
}

/* 
 *  SIGQUIT signal from postmaster
 */
static void 
FileRep_ImmediateShutdownHandler(SIGNAL_ARGS)
{
	immediateShutdownRequested = true;
	elog(DEBUG1, "FileRep_ImmediateShutdownHandler()");
}

/*
 *  SIGUSR2 signal from postmaster
 */
static void 
FileRep_ShutdownHandler(SIGNAL_ARGS)
{
	shutdownRequested = true;
	elog(DEBUG1, "FileRep_ShutdownHandler()");	
}

/*
 *  SIGCHLD signal from FileRep process children
 */
static void 
FileRep_ChildTerminationHandler(SIGNAL_ARGS)
{
	childTermination = true;
	elog(DEBUG1, "FileRep_ChildTerminationHandler()");	
}

/*
 *  SIGUSR1 signal from postmaster
 *  It signals about data and/or segment state change.
 */
static void 
FileRep_StateHandler(SIGNAL_ARGS)
{
	stateChangeRequested = true;
	elog(DEBUG1, "FileRep_StateHandler() stateChangeRequested");	
}

/*
 * NOTE - should we report fault
 */
static void
FileRep_SignalChildren(int signal)
{
	int ii;

	for (ii=0; ii < MaxFileRepSubProc; ii++) {
		FileRepSubProc *subProc = &FileRepSubProcList[ii];
		
		if (subProc->pid != 0) {
			if (kill(subProc->pid, signal) < 0) {
				ereport(LOG, 
						(errmsg("Signal to children failed pid %ld signal %d: %m",
						 (long) subProc->pid, signal)));

			}
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
		FileRep_SignalChildren(SIGHUP);
		reloadConfigFile = false;
		ProcessConfigFile(PGC_SIGHUP);
	}
	
	if (shutdownRequested)
	{
		shutdownRequested = false;
		FileRep_SignalChildren(SIGUSR2);
		processExit = true;
		FileRep_SetState(FileRepStateShutdown);
	}		

	if (immediateShutdownRequested)
	{
		immediateShutdownRequested = false;
		FileRep_SignalChildren(SIGQUIT);
		processExit = true;
		FileRep_SetState(FileRepStateImmediateShutdown);
	}
	
	/*
	 * Immediate shutdown if postmaster is not alive to avoid
	 * manual cleanup.
	 */
	if (! PostmasterIsAlive(true))
	{
		FileRep_SignalChildren(SIGUSR2);
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
					FileRepSubProcList[i].pid = 0;
					break;
				}
			}
		}
		errno = 0;
	}

	if (stateChangeRequested)
	{
		stateChangeRequested = false;
		FileRep_SignalChildren(SIGUSR1);
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

/*
 *  Set state in FileRep process and sent signal to postmaster
 */
void 
FileRep_SetState(FileRepState_e fileRepStateLocal)
{
	
	switch (fileRepState) {
		case FileRepStateNotInitialized:
		case FileRepStateInitialization:
			
			fileRepState = fileRepStateLocal;
			break;
		case FileRepStateReady:
			
			switch (fileRepStateLocal) {
				case FileRepStateFault:
				case FileRepStateSuspend:
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
				case FileRepStateInitialization:
				case FileRepStateReady:
					Assert(0);
					break;
			}
			break;
		case FileRepStateFault:
		case FileRepStateSuspend:
			
			switch (fileRepStateLocal) {
				case FileRepStateFault:
				case FileRepStateSuspend:
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
				case FileRepStateInitialization:
				case FileRepStateReady:
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
				case FileRepStateSuspend:
					Assert(0);
				case FileRepStateFault:
					break;					
			}
			
			break;
		case FileRepStateImmediateShutdown:

			break;
	}

	switch (fileRepState) {
		case FileRepStateReady:
			if (updateConfig(SegmentStateReady)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}			
			break;
			
		case FileRepStateFault:
			/* update shared memory configuration 
			   bool updateSegmentState(FAULT);
			   return TRUE if state was updated;
			   return FALSE if state was already set to FAULT
			   change signal to PMSIGNAL_FILEREP_SEGMENT_STATE_CHANGE
			 */
			if (updateConfig(SegmentStateFault)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}			
			break;
			
		case FileRepStateInitialization:
		case FileRepStateShutdown:	
		case FileRepStateImmediateShutdown:
		case FileRepStateNotInitialized:
		case FileRepStateSuspend:
			/* No operation */
			break;
	}
	
	elog(DEBUG1, "FileRep_SetState() state:'%s' MyProcPid:'%d'", 
		 FileRepStateToString[FileRep_GetState()],
		 getpid());

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
	pg_crc32 _crc;
	
	/* Compute Crc */
	INIT_CRC32(_crc);
	
	COMP_CRC32(_crc, (char *) data, length);
	
	FIN_CRC32(_crc);
	*crc = _crc;
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
			
			return TRUE;	

		case FileRepOperationOpen: 
		case FileRepOperationClose:
		case FileRepOperationTruncate:
		case FileRepOperationWrite:
			
			return FALSE;
			
		case FileRepOperationNotSpecified:
			Assert(0);
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
				RelFileNode		relFileNode,
				int32			segmentFileNum)
{
	FileRepIdentifier_u identifier;
	
	identifier.fileRepRelationIdentifier.relFileNode.spcNode = relFileNode.spcNode;
	identifier.fileRepRelationIdentifier.relFileNode.dbNode = relFileNode.dbNode;
	identifier.fileRepRelationIdentifier.relFileNode.relNode = relFileNode.relNode;
	
	identifier.fileRepRelationIdentifier.segmentFileNum = segmentFileNum;		
	
	return identifier;
}

FileRepIdentifier_u 
FileRep_GetDirIdentifier(DbDirNode	dbDirNode)
{
	
	FileRepIdentifier_u identifier;
	
	identifier.fileRepDbDirNodeIdentifier.tablespace = dbDirNode.tablespace;
	identifier.fileRepDbDirNodeIdentifier.database = dbDirNode.database;
	
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
	/* NOTE can flat file routine be added for constructing path */
		
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
	/* NOTE add to fileName filespaces and segment */
			
		/* palloc() is done in relpath() routine */
			if (fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum == 0) {
				fileName = relpath(fileRepIdentifier.fileRepRelationIdentifier.relFileNode);
			} else {
				/*fileName = (char*) palloc(strlen(local) + sizeof(uint32) + 2);*/
				fileName = (char*) palloc(MAXPGPATH);
				sprintf(fileName, "%s.%u", 
						relpath(fileRepIdentifier.fileRepRelationIdentifier.relFileNode), 
						fileRepIdentifier.fileRepRelationIdentifier.segmentFileNum);

			}
			break;
			
		
		case FileRepRelationTypeFile:
	/* NOTE add to fileName filespaces and segment */

		/* palloc() is done in relpath() routine */
			fileName = relpath(fileRepIdentifier.fileRepRelationIdentifier.relFileNode);
		
			break;
		
		case FileRepRelationTypeDir:
	/* NOTE add to fileName filespaces and segment */

		
			fileName = GetDatabasePath(
					fileRepIdentifier.fileRepDbDirNodeIdentifier.database,
					fileRepIdentifier.fileRepDbDirNodeIdentifier.tablespace);	
			break;
		
		case FileRepRelationNotSpecified:
			
			break;
		default:
			Assert(0);		
	}
		
	return fileName;										  
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
	
	Insist(IsUnderPostmaster == TRUE);
	
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
	
	getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);
	
	if (fileRepRole == FileRepNoRoleConfigured) {
		ereport(LOG,
			(errmsg("Retrieving Configuration parameters fail.")));
		FileRep_SetState(FileRepStateFault);
		goto shutdown;
	}
		
	snprintf(title, sizeof(title), "%s %s", 
			 FileRepProcessTypeToString[FileRepProcessTypeMain],
			 fileRepRole == FileRepPrimaryRole ? "Primary" : "Mirror");
	init_ps_display(title, "", "", "");
		
	elog(DEBUG1, "FileRep_Main() started with role %d\n\t\t segmentState %d\n\t\t dataState %d\n\t\t\n", 
		 fileRepRole, segmentState, dataState);
	
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
		
												
			break;
		
		case FileRepMirrorRole:
			
			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorReceiver) < 0) {
				status = STATUS_ERROR;
				break;
			}			

			if (FileRep_StartChildProcess(FileRepProcessTypeMirrorConsumer) < 0) {
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
			Assert(0);
			break;
	}			
			
	if (status != STATUS_OK) {
		FileRep_SetState(FileRepStateFault);
		goto shutdown;
	}
shutdown:
	while (FileRep_ProcessSignals() == false) {
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


