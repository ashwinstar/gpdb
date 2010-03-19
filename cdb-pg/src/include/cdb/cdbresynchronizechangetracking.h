/*-------------------------------------------------------------------------
 *
 * cdbresynchronizechangetracking.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBRESYNCHRONIZECHANGETRACKING_H
#define CDBRESYNCHRONIZECHANGETRACKING_H

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "storage/relfilenode.h"

#define CHANGETRACKINGDIR  "pg_changetracking"
#define CHANGETRACKING_STORAGE_VERSION 1
#define CHANGETRACKING_BLCKSZ BLCKSZ
#define CHANGETRACKING_FILESEGMENT 1 /* for now, doesn't change and doesn't mean anything. a single file */

/*
 * The LOG file is where all the incremental buffer
 * pool changes are stored.
 */
#define ChangeTrackingLogFileName(fname)	\
	snprintf(fname, MAXFNAMELEN, "CT%08X", CHANGETRACKING_FILESEGMENT)

#define ChangeTrackingLogFilePath(path)	\
	snprintf(path, MAXPGPATH, CHANGETRACKINGDIR "/CT%08X", CHANGETRACKING_FILESEGMENT)

/*
 * The META file is where we store information about
 * if it's a full resync and the last resync lsn.
 */
#define ChangeTrackingMetaFileName(fname)	\
	snprintf(fname, MAXFNAMELEN, "MD%08X", CHANGETRACKING_FILESEGMENT)

#define ChangeTrackingMetaFilePath(path)	\
	snprintf(path, MAXPGPATH, CHANGETRACKINGDIR "/MD%08X", CHANGETRACKING_FILESEGMENT)



/*
 * This module is for initializing, recovering, writing, reading, checkpointing, and dropping the
 * mirror resynchronize change log.
 *
 * Multiple backends can write and read the resynchronize change log at the same time.
 */


extern bool ChangeTracking_ShouldTrackChanges(void);

extern bool ChangeTracking_RetrieveLastChangeTrackedLoc(void);

extern void ChangeTracking_MarkFullResync(void);

extern void ChangeTracking_RecordLastChangeTrackedLoc(void);

// -----------------------------------------------------------------------------
// Creation, Recovery, and Drop
// -----------------------------------------------------------------------------

/*
 * Create the initial portion of the resynchronize change log after mirror loss by processing
 * the XLOG starting at the previous checkpoint.
 *
 * UNDONE: Talk about XLOG stability -- can have XLOG files disappearing or new XLOG writes
 * UNDONE: occurring since they would intermix and we can't handle that..
 */
extern void ChangeTracking_CreateInitialFromPreviousCheckpoint(void);

/*
 * Make a copy of the original rdata so that we use it in case xlog
 * changes it later on.
 */
extern XLogRecData* ChangeTracking_MakeRdataCopy(XLogRecData *rdata);
extern void ChangeTracking_FreeRdataCopy(XLogRecData *rdata);

/*
 * Send a single xlog record into the changetracker to examine it and add
 * the relevant information from it into the changetracker if desired.
 */
extern void ChangeTracking_AddRecordFromXlog(
											 RmgrId		xl_rmid,
											 uint8		xl_info,
											 void		*data, 
											 XLogRecPtr *loc);

typedef struct RelationChangeInfo
{
	RelFileNode 	relFileNode;
	BlockNumber 	blockNumber;
	ItemPointerData persistentTid;
	int64	  		persistentSerialNum;
} RelationChangeInfo;

#define MaxRelationChangeInfoReturns 3

extern void ChangeTracking_GetRelationChangeInfoFromXlog(
									  RmgrId	xl_rmid,
									  uint8 	xl_info,
									  void		*data, 
									  RelationChangeInfo	*relationChangeInfoArray,
									  int					*relationChangeInfoArrayCount);

extern void ChangeTracking_PrintRelationChangeInfo(
									  RmgrId	xl_rmid,
									  uint8		xl_info,
									  void		*data, 
									  XLogRecPtr *loc,
									  bool	  weAreGeneratingXLogNow);

/*
 * Recover the end of the resynchronize change log by determing the last good change record
 * and rebuilding from there.
 */
extern void ChangeTracking_RecoverUsingXLog(void);

/*
 * read the change tracking log and return the last loc
 * written into the file so far. This routine will be used for 
 * recovery purposes
 */
extern XLogRecPtr* ChangeTracking_ReadLastLocOnDisk(void);

/*
 * Drop the resynchronize change log and meta files.
 */
extern void ChangeTracking_DropAll(void);

// -----------------------------------------------------------------------------
// Add 
// -----------------------------------------------------------------------------

							
/*
 * Add notification of an initial bulk change that was not made to the mirror.
 */
extern void ChangeTracking_AddInitialBulkChange(
	XLogRecPtr		xlogLocation,	/* The XLOG LSN of the record that describes the page change. 			*/
	RelFileNode 	*relFileNode);	/* The tablespace, database, and relation OIDs for the changed relation	*/
				
				
// -----------------------------------------------------------------------------
// Reading 
// -----------------------------------------------------------------------------

		
/*
 * This is the format of a change log record on disk.
 * These records are stored in the LOG file.
 */
typedef struct ChangeTrackingRecord
{
	XLogRecPtr		xlogLocation;
	RelFileNode 	relFileNode; 			/* The tablespace, database, and relation OIDs for the changed relation. */
	uint32			bufferPoolBlockNum; 	/* BufferPool */
	ItemPointerData persistentTid;
	int64 			persistentSerialNum;
} ChangeTrackingRecord;

/*
 * The log record block header describes the version and number of 
 * records stored in it. for now it's primitive and not so compact. 
 * We could probably keep it 8 bytes in side if we add another small 
 * field or 2.
 */
typedef struct ChangeTrackingPageHeader
{
	uint32		blockversion;		/* version == 1. increment if we change the block layout in the future */
	uint32		numrecords;			/* number of records stored in the current block */
	
} ChangeTrackingPageHeader;

/*
 * This is the format of a meta data record on disk.
 * These records are stored in the META file.
 */
typedef struct ChangeTrackingMetaRecord
{
	XLogRecPtr		resync_lsn_end;	  /* endResyncLSN                         */
	bool			resync_mode_full; /* marks that resync FULL was requested */

} ChangeTrackingMetaRecord;

// -----------------------------------------------------------------------------
// Checkpointing and Shutting Down
// -----------------------------------------------------------------------------
				
/*
 * Flush the reschronize change log to disk for checkpoint.
 */
extern void CheckPointChangeTracking(void);
												
/*
 * Shutdown the reschronize change log.
 */
extern void ChangeTracking_Shutdown(void);

// -----------------------------------------------------------------------------
// Get incremental change entries for gp_persistent relation
// -----------------------------------------------------------------------------

typedef struct IncrementalChangeEntry
{
	ItemPointerData persistentTid;
	int64 			persistentSerialNum;

} IncrementalChangeEntry;

typedef struct IncrementalChangeList
{
	IncrementalChangeEntry* entries;
	int						count;

} IncrementalChangeList;

IncrementalChangeList* ChangeTracking_InitIncrementalChangeList(int count);


IncrementalChangeList* ChangeTracking_GetIncrementalChangeList(void);
void ChangeTracking_FreeIncrementalChangeList(IncrementalChangeList* iclist);

// -----------------------------------------------------------------------------
// Consuming required information from change log
// -----------------------------------------------------------------------------

typedef struct ChangeTrackingRequestEntry
{
	XLogRecPtr		lsn_start;
	XLogRecPtr		lsn_end;
	RelFileNode 	relFileNode; 	/* The tablespace, database, and relation OIDs for the requested relation. */

} ChangeTrackingRequestEntry;

typedef struct ChangeTrackingRequest
{
	ChangeTrackingRequestEntry* entries;
	int							count;
	int							max_count;
	
} ChangeTrackingRequest;

typedef struct ChangeTrackingResultEntry
{
	RelFileNode 	relFileNode; 	/* The tablespace, database, and relation OIDs for the requested relation. */
	BlockNumber		block_num;
	XLogRecPtr		lsn_end;

} ChangeTrackingResultEntry;

typedef struct ChangeTrackingResult
{
	ChangeTrackingResultEntry*  entries;
	int							count;
	int							max_count;
	
} ChangeTrackingResult;

extern ChangeTrackingRequest* ChangeTracking_FormRequest(int count);
extern void ChangeTracking_AddRequestEntry(ChangeTrackingRequest* request, 
										   RelFileNode		relFileNode,
										   XLogRecPtr*		lsn_start,
										   XLogRecPtr*		lsn_end);

extern ChangeTrackingResult* ChangeTracking_GetChanges(ChangeTrackingRequest* request);
extern void ChangeTracking_FreeRequest(ChangeTrackingRequest* request);
extern void ChangeTracking_FreeResult(ChangeTrackingResult* result);

// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------

typedef struct ChangeTrackingBufStatusData
{
	uint32			recordcount;  	/* Total change log records in buffer currently 	*/
	uint32			maxbufsize;		/* buffer capacity                              	*/
	uint32			bufsize;		/* Current buffer size                          	*/
	uint32			fileseg;		/* The change log file segment we are writing into	*/
	int64			nextwritepos;   /* The next file position to append buffer data to  */
	
} ChangeTrackingBufStatusData;

/* save resync metadata info in shmem and write to meta file when needed */
typedef struct ChangeTrackingResyncMetaData
{
	XLogRecPtr		resync_lsn_end;	  /* endResyncLSN                         */
	bool			resync_mode_full; /* marks that resync FULL was requested */	
} ChangeTrackingResyncMetaData;

/*
 * Return the required shared-memory size for this module.
 */
extern Size ChangeTrackingShmemSize(void);
								
/*
 * Initialize the shared-memory for this module.
 */
extern void ChangeTrackingShmemInit(void);
																
#endif   /* CDBRESYNCHRONIZECHANGETRACKING_H */

