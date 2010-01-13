/*-------------------------------------------------------------------------
 *
 * cdbmirroreddatabasedirectory.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBMIRROREDDATABASEDIRECTORY_H
#define CDBMIRROREDDATABASEDIRECTORY_H

#include "utils/palloc.h"
#include "storage/fd.h"

/*
 * This module is for database directory create and drop.
 *
 * For create, it makes the file-system create of database directory fully transactional so
 * the directory will be deleted even on system crash.
 */

typedef struct DatabaseDirectoryNode
{
	Oid			tablespaceOid;
	Oid			databaseOid;
} DatabaseDirectoryNode;

// -----------------------------------------------------------------------------
// Create 
// -----------------------------------------------------------------------------

/*
 * Generate a unique database OID.
 *
 * If the master is doing this in the context of the GPDB cluster, then the database OID will be
 * unique across the segments.
 */
extern Oid MirroredDatabaseDirectory_GetOid(void);

/*
 * Create a relation file as part of the current transaction.
 *
 * Generates and flushes a XLOG IntentToCreate record to guard the file-system create to 
 * guarantee proper clean-up in case of system crash that aborts the transaction.
 *
 * The highest LSN value for the XLOG IntentToCreate records will be flushed first.
 * This action guarantees proper clean-up in case of system crash that implicitly aborts a
 * transaction before abort records are written.  The IntentToCreate XLOG records preserve
 * what happened so crash recovery can generate a proper abort XLOG record.
 *
 * And the directory create will be listed inside an abort, commit, distributed commit, distributed 
 * prepared, distributed commit prepared, and distributed abort prepared XLOG records, too.
 * Listing them in those records preserves that directories should be kept or deleted if one
 * or more system crashes occur after the XLOG records are flushed.
 */
extern void MirroredDatabaseDirectory_Create(
	DatabaseDirectoryNode 		*createDatabaseDirectoryNode);
				/* The tablespace and database OIDs for the create. */

// -----------------------------------------------------------------------------
// Drop  
// -----------------------------------------------------------------------------

/*
 * Indicate we intend to drop a database directory as part of the current transaction.
 *
 * This database directory to drop will be listed inside a commit, distributed commit, a
 * distributed prepared, and distributed commit prepared XOG records.
 *
 * For any of the commit type records, once that XLOG record is flushed then the actual
 * file-system directory remove will occur.  The flush guarantees the action will be retried
 * after system crash.
 */
extern void MirroredDatabaseDirectory_IntentToDrop(
	DatabaseDirectoryNode		*dropPendingDatabaseDirectoryNode);
				/* The tablespace and database OIDs for the drop. */

// -----------------------------------------------------------------------------
// Transaction End  
// -----------------------------------------------------------------------------
				
/*
 * Get array of DatabaseDirectoryNode for the database directories being created in the
 * current transaction.
 */
extern void MirroredDatabaseDirectory_GetCreates(
	TransactionId		localXid,
				/* The local transaction id. */

	DatabaseDirectoryNode	**createDatabaseDirectoryNodeArray,
				/* The palloc'd array of create directories. */

	int					*createCount);
				/* The number of directories being created. */
				
/*
 * Get array of DatabaseDirectoryNode for the database directories being dropped in the
 * current transaction.
 */
extern void MirroredDatabaseDirectory_GetDrops(
	TransactionId		localXid,
				/* The local transaction id. */

	DatabaseDirectoryNode 	**dropDatabaseDirectoryNodeArray,
				/* The palloc'd array of drop database directories. */

	int 				*dropCount);
				/* The number of database directories being dropped. */
				
/*
 * Perform file-system actions for database directory create and drop after a commit type
 * XLOG record has been flushed (or for system crash recovery redo).
 *
 * This call is idempotent.
 */
extern void MirroredDatabaseDirectory_HandleTransactionCommit(
	TransactionId 		commitXid,
				/* The local commit transaction id. */

	DatabaseDirectoryNode 	*createDatabaseDirectoryNodeArray,
				/* The array of create database directories. */
	
	int 				createCount,
				/* The number of database directories being created. */

	DatabaseDirectoryNode 	*dropDatabaseDirectoryNodeArray,
				/* The array of drop database directories. */
	
	int 				*dropCount);
				/* The number of database directories being dropped. */

/*
 * Perform file-system actions for relation file create and drop after an abort type XLOG record
 * has been flushed (or for system crash recovery redo).
 *
 * This call is idempotent.
 */
extern void MirroredDatabaseDirectory_HandleTransactionAbort(
	TransactionId		abortXid,
				/* The local abort transaction id. */
	
	DatabaseDirectoryNode	*createDatabaseDirectoryNodeArray,
				/* The array of create database directories. */
	
	int 				createCount);
				/* The number of database directories being created. */

// -----------------------------------------------------------------------------
// TEMPORARY SELF-UPGRADE	
// -----------------------------------------------------------------------------
				
/*
 * Build the new persistent file-system database directory table from pg_database.
 */
extern void MirroredDatabaseDirectory_BuildPersistentTable(void);
				
// -----------------------------------------------------------------------------
// Shmem
// -----------------------------------------------------------------------------
				
/*
 * Return the required shared-memory size for this module.
 */
extern Size MirroredDatabaseDirectory_ShmemSize(void);
								
/*
 * Initialize the shared-memory for this module.
 */
extern void MirroredDatabaseDirectory_ShmemInit(void);
																
#endif   /* CDBMIRROREDDATABASEDIRECTORY_H */

