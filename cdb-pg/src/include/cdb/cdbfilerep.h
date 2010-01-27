/*
 *  cdbfilerep.h
 *  
 *
 *  Copyright 2009-2010, Greenplum Inc. All rights reserved.
 *
 */

#ifndef CDBFILEREP_H
#define CDBFILEREP_H

#include "c.h"
#include "utils/pg_crc.h"
#include "pg_config_manual.h"
#include "storage/relfilenode.h"
#include "access/xlogdefs.h"
#include "postmaster/primary_mirror_mode.h"
#include "storage/fd.h"
#include "storage/dbdirnode.h"
#include "signal.h"




	/* ATTENTION: Code has to be changed if FILEREP_MAX_LISTEN > 1 */
#define FILEREP_MAX_LISTEN	1
	/* Max number of listener */
#define FILEREP_MESSAGEBODY_LEN 32 * 1024
	/* should we have GUC */
#define FILEREP_MAX_OPEN_FILES 128   // 131072   // 128k


#define FILEREP_OFFSET_UNDEFINED -1  

#define FILEREP_UNDEFINED 0xFFFFFFFF
	
/* GUC parameters */
extern int file_rep_message_body_length; 	
extern int file_rep_buffer_size;
extern int file_rep_ack_buffer_size;
extern int file_rep_retry;
extern int file_rep_min_data_before_flush;

extern FileRepRole_e		fileRepRole;
extern SegmentState_e		segmentState;
extern DataState_e			dataState;

extern const char*	FileRepProcessTypeToString[];
extern const char*	FileRepRoleToString[];
extern const char*	SegmentStateToString[];
extern const char*	DataStateToString[];
extern const char*  FileRepStateToString[];
extern const char*	FileRepOperationToString[];
extern const char*	FileRepRelationTypeToString[];
extern const char*	FileRepAckStateToString[];

/****************************************************************
 * FILEREP SHARED MEMORY
 ****************************************************************/
/*
 * The structure describes position for next message to be inserted 
 * and next message to be consumed. The same structure is used on 
 * primary and mirror.
 *
 *  beginPosition
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
	
typedef struct FileRepShmem_s {
	
	char*	positionBegin;
	/* 
	 * Begin position for buffering messages.  
	 * It is initialized when shared memory is initalized.
	 * The value does not change after.
	 */
	
	char*	positionEnd;
	/* 
	 * End position for buffering messages.  
	 * It is initialized when shared memory is initalized.
	 * The value does not change after.
	 */
	
	char*	positionInsert;
	/* 
	 * Identifies the insert position in shared memory for next message.
	 * It is protected by FileRepShmemLock.
	 */
	
	char*	positionConsume;
	/* 
	 * Identifies the position in shared memory for next message to be 
	 * mirrored. It is protected by FileRepShmemLock.
	 */
	
	char* positionWraparound;
	/* 
	 * Identifies the end of last message in shared memory. 
	 * It is protected by FileRepShmemLock
	 * The value is updated at wraparound when message is inserted 
	 * at startPosition in shared memory.
	 * It is used by consumer thread in order to detect wraparound.  
	 */
	
	uint32	insertCount;
	/* counter of inserted messages. It is used for testing code correctness. */
	
	uint32	consumeCount;
	/* counter of consumed messages. It is used for testing code correctness. */
	
	volatile sig_atomic_t state;
	
} FileRepShmem_s;

/*
 * The structure describes the state of the FileRep message 
 * in shared memory.
 */
typedef enum FileRepShmemMessageState_e {
	
	FileRepShmemMessageStateInitialized=0,
		/* Memory is set to 0 (memset(). */
	
	FileRepShmemMessageStateReserved,
		/* Memory is reserved for next message to be inserted. */
	
	FileRepShmemMessageStateReady
		/* Message is inserted and ready to be consumed. */
		 
} FileRepShmemMessageState_e;

/*
 * The structure describes FileRep Message related information 
 * that is required for consuming FileRep message (removing message 
 * from shared memory and send it over the network to mirror).
 * That information is kept only in shared memory. 
 */
typedef struct FileRepShmemMessageDescr_s {
	
	bool			messageSync;
	/* 
	 * Synchronous FileRep Message.
	 * Client flushes all data from buffers when
	 * synchronous message is sent.
	 */

	uint32			messageLength;
	/* FileRep Message length (FileRepMessage_s) */

	volatile sig_atomic_t	messageState; 
	/* 
	 * FileRep Message state.
	 *		a) Reserved state (reserve memory for next message insert) 
	 *		b) Ready state (calculate Crc and insert message)
	 *		   Message is ready to be consumed.
	 */
	
} FileRepShmemMessageDescr_s;	

/*
 * Return the required shared-memory size for this module.
 */
extern Size FileRep_ShmemSize(void);

/*
 * Initialize the shared-memory for this module.
 */
extern void FileRep_ShmemInit(void);
	
/*
 * Return the required shared-memory size for this module.
 */
extern Size FileRepAck_ShmemSize(void);

/*
 * Initialize the shared-memory for this module.
 */
extern void FileRepAck_ShmemInit(void);



/****************************************************************
 * FILEREP PROCESS
 ****************************************************************/

extern FileRepShmem_s	*fileRepShmem;

extern FileRepShmem_s	*fileRepAckShmem;

typedef enum FileRepState_e {
	FileRepStateNotInitialized=0,
	/* */
	
	FileRepStateInitialization, 
	/* 
	 * Start listener, establish connection and reconcile
	 * WAL to same EOF (if in-sync state).
	 * Establish Connection (if in resync data state)
	 */
	FileRepStateReady,
	/* 
	 * Mirroring or resynchronization is in progress.
	 * Data state is Change Logging or Resynchronization.
	 */
	
	FileRepStateFault,
	/* 
	 * Fault is detected on primary or mirror. 
	 * It transitions into Suspend state.
	 */
	
	FileRepStateSuspend,
	/* Wait for segment state change. Segment is in
	 * change logging.
	 */
	
	FileRepStateShutdown,
	/* Process is going to exit (graceful termination). */
	
	FileRepStateImmediateShutdown
	/* Process is going to exit (emergency termination). */
} FileRepState_e;	
		
/*
 * Responsibilities of this module!!!
 *
 * Mirror Message contains
 *		a) Header (describes operation and data that are mirrored)
 *		b) Crc of the header 
 *		c) Body (contains data that are mirrored.). If mirror operation
 *         is different than 'write' then data are not mirrored
 *         and hence Body is empty.
 *
 *   --------------------------------------------------------------
 *  | Message       | Message     | Message Body                   |
 *  | Header        | Header CRC  |     configurable size          |
 *  |               |     4 bytes |     default is 32k bytes       |
 *   --------------------------------------------------------------
 *
 */


typedef enum FileRepMessageHeaderVersion_e {
	FileRepMessageHeaderVersionOne=0
} FileRepMessageHeaderVersion_e;


typedef enum FileRepMessageType_e {
	FileRepMessageTypeNotConfigured=0,
	
	FileRepMessageTypeData,
	
	FileRepMessageTypeControl,
	
	FileRepMessageTypeShutdown,
		/* Primary runs gracefull shutdown. It informs mirror about it. */
	
	FileRepMessageTypeDataHeader,
		/* Message contains data header and header Crc. */
	
	FileRepMessageTypeDataHeaderAndBody,
		/* Message contains data header, header Crc and body (contains data). */
	
	FileRepMessageTypeDataHeaderAndFileSpaceBody,
	/* Message contains data header, header CRC and body (contains filespace map). */	

} FileRepMessageType_e;

/* 
 * Describe relation types that are mirrored.
 */
typedef enum FileRepRelationType_e {
	FileRepRelationTypeFlatFile=0,
		/* Mirror Relation is flat file. */
	
	FileRepRelationTypeAppendOnly,
		/* Mirror Relation is Append Only file */
	
	FileRepRelationTypeBufferPool,
		/* Mirror Relation is Heap file. */
	
	FileRepRelationTypeBulk,
		/* Mirror Relation is bulk loaded file. */
	
	FileRepRelationTypeFile,
		/* Mirror Relation is generic file create or drop. */
	
	FileRepRelationTypeDir,
		/* Mirror Relation is generic directory create and drop. */
	
	FileRepRelationNotSpecified,
	
} FileRepRelationType_e;
	
/* 
 * Describe relation operations that are mirrored.
 */
typedef enum FileRepOperation_e {
	FileRepOperationOpen=0,
		/*	Open mirror relation or flat file. */
	
	FileRepOperationWrite,
		/* Write data to a mirror. */ 
	
	FileRepOperationFlush,
		/* Issue synchronous flush on a mirror. */
	
	FileRepOperationClose,
		/* Close a mirror relation or flat file. */
	
	FileRepOperationFlushAndClose,
		/* Issue synchronous flush and close a mirror relation */
	
	FileRepOperationTruncate,
	/* Issue truncate to a mirror relation */
	
	FileRepOperationCreate,
		/* Create a mirror relation, flat file, or database directory. */
	
	FileRepOperationDrop,
		/* Drop mirror relation, flat file, or database directory. */
	
	FileRepOperationReconcileXLogEof,
		/* Reconcile XLog Eof between primary and mirror */
	
	FileRepOperationRename,
		/* Rename flat file on a mirror relation */
	
	FileRepOperationNotSpecified,
	
} FileRepOperation_e;

/* 
 * Describe relation operations that are mirrored.
 */
typedef enum FileRepAckState_e {
	FileRepAckStateNotInitialized=0,
	
	FileRepAckStateWaiting,
	
	FileRepAckStateCompleted,
	
	FileRepAckStateMirrorInFault,
	
} FileRepAckState_e;

/*
 * This structure identifies uniquely Append Only,
 * Buffer Pool, and Bulk load relation.
 */
typedef struct FileRepRelationIdentifier_s {
	RelFileNode	relFileNode;
		/* The tablespace, database, and relation OIDs for the open. */
	
	uint32		segmentFileNum;
		/* Which segment file? */
} FileRepRelationIdentifier_s;

/*
 * This structure identifies uniquely flat file.
 */
typedef struct FileRepFlatFileIdentifier_s {
	char	directorySimpleName[MAXPGPATH+1];
		/* The simple name of the directory in the instance database directory. */
	
	char	fileSimpleName[MAXPGPATH+1];
		/* The simple name of the file. */
} FileRepFlatFileIdentifier_s;

/*
 * This structure identifies uniquely database directory
 * (the tablespace and database OIDs).
 */
typedef DbDirNode FileRepDbDirNodeIdentifier_s;	
	
	
/* 
 * This structure identifies uniquely receiver of the message.
 */
typedef union FileRepIdentifier_u {
	FileRepRelationIdentifier_s fileRepRelationIdentifier;
		/*  */

	FileRepFlatFileIdentifier_s fileRepFlatFileIdentifier;
		/* */
	
	FileRepDbDirNodeIdentifier_s fileRepDbDirNodeIdentifier; 
		/* */
	

} FileRepIdentifier_u;

/*
 *	 
 */
typedef struct FileRepOperationDescriptionOpen_s {
	int			fileFlags;
		/* file flags */
	
	int			fileMode;
		/* file mode */
	
	int64		logicalEof;
	
} FileRepOperationDescriptionOpen_s;

/*
 *	 
 */
typedef struct FileRepOperationDescriptionTruncate_s {
	int32			position;
	/* the position to cutoff the data */
	
} FileRepOperationDescriptionTruncate_s;

/*
 *	 
 */
typedef struct FileRepOperationDescriptionWrite_s {
	int32		offset;
		/* blocknum if buffer pool */
	
	uint32		dataLength;
		/* 32k for buffer pool */
	
	XLogRecPtr	lsn;
		/* */
} FileRepOperationDescriptionWrite_s;
	
/*
 *	 
 */
typedef struct FileRepOperationDescriptionReconcile_s {
	
	XLogRecPtr	xLogEof;
		/* XLog logical eof */
	
} FileRepOperationDescriptionReconcile_s;

typedef struct FileRepOperationDescriptionRename_s {
	
	FileRepIdentifier_u fileRepIdentifier;
	/* new identifier (after rename). */
	
} FileRepOperationDescriptionRename_s;

/* 
 *
 */
typedef union FileRepOperationDescription_u {
	
	FileRepOperationDescriptionOpen_s open;
		/* */
	
	FileRepOperationDescriptionTruncate_s truncate;
		/* */
	
	FileRepOperationDescriptionWrite_s write;
		/* */
	
	FileRepOperationDescriptionReconcile_s reconcile;
	
	FileRepOperationDescriptionRename_s rename;
	
} FileRepOperationDescription_u;

/* 
 * This structure describes operation to be mirrored.
 */
typedef struct FileRepMessageHeader_s {
	/* NOTE probably this field can be removed, we are checking protocol
	  version  during handshake */
	FileRepMessageHeaderVersion_e fileRepMessageHeaderVersion;
		/* Message Header Version */
	
	FileRepMessageType_e fileRepMessageType;
		/* Describe the message content. */
	
	FileRepRelationType_e fileRepRelationType;
		/* Mirror Relation type. */
	
	FileRepOperation_e fileRepOperation;
		/* Operation to be issued on a mirror. */
	
	FileRepIdentifier_u fileRepIdentifier;
		/* Message Receiver identifier. */
	
	FileRepOperationDescription_u fileRepOperationDescription;
		/* Describes metadata required to perform operation */
	
	pg_crc32 fileRepMessageBodyCrc;
		/* CRC of the message body */
	
	FileRepAckState_e fileRepAckState;	
		/* state/status of the ACK message */

	uint32	messageCount;
	
	uint32 spareField;
		/* reserved for future */
	
} FileRepMessageHeader_s;

typedef struct FileRepMessage_s {
	FileRepMessageHeader_s fileRepMessageHeader;
		/* */
	
	pg_crc32	fileRepMessageHeaderCrc; 
		/* */
	
	char		fileRepMessageBody[FILEREP_MESSAGEBODY_LEN+1];
		/* */
	
} FileRepMessage_s;	

/*********************************************************
 * FILE REP PROCESSES
 *********************************************************/

typedef enum FileRepProcessType_e {
	FileRepProcessTypeNotInitialized=0,
	
	FileRepProcessTypeMain,
	/* Main File Rep process forked by Postmaster. */
	
	FileRepProcessTypePrimarySender,
	/* Primary Sender process forked by main file rep process. */
	
	FileRepProcessTypeMirrorReceiver,
	/* Mirror Receiver process forked by main file rep process. */
	
	FileRepProcessTypeMirrorConsumer,
	/* Mirror Consumer process forked by main file rep process. */
	
	FileRepProcessTypeMirrorSenderAck,
	/* Mirror Sender ACK process forked by main file rep process. */
	
	FileRepProcessTypePrimaryReceiverAck,
	/* Primary Receiver ACK process forked by main file rep process. */
	
	FileRepProcessTypePrimaryConsumerAck,
	/* Primary Consumer ACK process forked by main file rep process. */
	
	FileRepProcessTypePrimaryRecovery,
	/* Recovery process that runs only during recovery time at gpstart */
	
} FileRepProcessType_e;


/*********************************************************
 * FILE REP IN-MEMORY LOG
 *********************************************************/
/*
 * Wraparound log for troubleshooting is maintained by each file rep process
 */

typedef struct FileRepLogEntry_s {
	
	char	routineName[32];
		
	FileRepRelationType_e fileRepRelationType;
		/* Relation type. */
	
	FileRepOperation_e fileRepOperation;
		/* Operation logged. */
	
	FileRepIdentifier_u fileRepIdentifier;
		/* Identifier logged */
		
	pg_crc32 fileRepMessageHeaderCrc;	
		
	pg_crc32 fileRepMessageBodyCrc;
		/* CRC of the message body */
	
	FileRepAckState_e fileRepAckState;	
		/* state/status of the ACK message */	
	
	uint32	localCount;
		/* local count */
	
	uint32	fileRepMessageCount;
		/* count in the message */
	
	char*	positionInsert;
			
	char*	shmemPositionBegin;
	char*	shmemPositionEnd;
	char*	shmemPositionInsert;
	char*	shmemPositionConsume;
	char*	shmemPositionWraparound;
	uint32	shmemInsertCount;
	uint32	shmemConsumeCount;

	char*	shmemAckPositionBegin;
	char*	shmemAckPositionEnd;
	char*	shmemAckPositionInsert;
	char*	shmemAckPositionConsume;
	char*	shmemAckPositionWraparound;
	uint32	shmemAckInsertCount;
	uint32	shmemAckConsumeCount;
	
	FileRepRole_e		fileRepRole;
	
	SegmentState_e		segmentState;
	
	DataState_e			dataState;

	FileRepState_e		fileRepState;

	int					procPid;
	
	FileRepProcessType_e fileRepProcessType;
		/* process type */
	
	struct timeval		logTime;
	
} FileRepLogEntry_s;

/*
 * Postmaster forks FileRep process. Input parameters 
 * are stored in shared memory. Postmaster-backend process
 * writes input parameters. FileRep and Postmaster processes 
 * read them (never write).
 *
 * INPUT parameters
 *			Replica Role
 *			Replica State
 *			Primary IP address
 *			Mirror IP address
 *			Primary Port
 *			Mirror Port
 */	
extern void FileRep_Main(void);
	
/*
 * Set state in FileRep process and sent signal to postmaster
 * (if required).
 */
extern void FileRep_SetState(
				FileRepState_e fileRepStateLocal);

/*
 * Return state of FileRep process.
 */
extern FileRepState_e FileRep_GetState(void);


/*
 * Return TRUE if shutdown was requested
 */	
extern bool FileRep_IsShutdownRequested(void);
		
	
/*
 * Reserve memory in order to insert FileRep message.
 * The routine returns the insert position in FileRep shared memory.
 */
extern char *FileRep_ReserveShmem(
					uint32		msgLength,
					uint32		*spareField);

extern char *FileRepAck_ReserveShmem(
								  uint32		msgLength,
								  uint32		*spareField);

/*
 * ERROR REPORTING
 */

extern int FileRep_errcontext(void);

extern int FileRep_errdetail(
				  FileRepIdentifier_u		fileRepIdentifier,
				  FileRepRelationType_e		fileRepRelationType,
				  FileRepOperation_e		fileRepOperation);

extern int FileRep_errdetail_Shmem(void);

extern int FileRep_errdetail_ShmemAck(void);

extern	void
FileRep_InsertLogEntry(
					   char						*routineName,
					   FileRepIdentifier_u		fileRepIdentifier,
					   FileRepRelationType_e	fileRepRelationType,
					   FileRepOperation_e		fileRepOperation,
					   pg_crc32					fileRepMessageHeaderCrc,
					   pg_crc32					fileRepMessageBodyCrc,
					   FileRepAckState_e		fileRepAckState,
					   uint32					localCount,
					   uint32					fileRepMessageCount);

extern void FileRep_ReportLog(void);

extern void FileRep_Sleep1ms(int retry);
extern void FileRep_Sleep10ms(int retry);

/*
 * Calculate 32 bit Crc
 */
extern void FileRep_CalculateCrc(
						char		*data,
						uint32		length,
						pg_crc32	*crc);

extern FileRepIdentifier_u FileRep_GetRelationIdentifier(
							  RelFileNode	relFileNode,
							  int32			segmentFileNum);

extern FileRepIdentifier_u FileRep_GetDirIdentifier(
							DbDirNode	dbDirNode);

extern FileRepIdentifier_u FileRep_GetFlatFileIdentifier(
							  char*	subDirectory,
							  char*	simpleFileName);
														 
extern bool FileRep_IsOperationSynchronous(
						FileRepOperation_e fileRepOperation);											 

extern XLogRecPtr FileRep_GetXLogRecPtrUndefined(void);

	
extern FileName FileRep_GetFileName(
				 FileRepIdentifier_u	fileRepIdentifier,
				 FileRepRelationType_e	fileRepRelationType);
	
#endif   /* CDBFILEREP_H */

