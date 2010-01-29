/*-------------------------------------------------------------------------
 *
 * cdbpersistentfilespace.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPERSISTENTFILEESPACE_H
#define CDBPERSISTENTFILEESPACE_H

#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/itemptr.h"
#include "cdb/cdbsharedoidsearch.h"
#include "access/persistentfilesysobjname.h"

extern bool PersistentFilespace_Check(
	Oid 			filespace);

extern void PersistentFilespace_GetPrimaryAndMirrorUnderLock(
	Oid 		filespaceOid,
				/* The filespace OID for the create. */

	char **primaryFilespaceLocation,
				/* The primary filespace directory path.  Return NULL for global and base. */
	
	char **mirrorFilespaceLocation);
				/* The primary filespace directory path.  Return NULL for global and base. 
				 * Or, returns NULL when mirror not configured. */

extern void PersistentFilespace_GetPrimaryAndMirror(
	Oid 		filespaceOid,
				/* The filespace OID for the create. */

	char **primaryFilespaceLocation,
				/* The primary filespace directory path.  Return NULL for global and base. */
	
	char **mirrorFilespaceLocation);
				/* The primary filespace directory path.  Return NULL for global and base. 
				 * Or, returns NULL when mirror not configured. */

/*
 * Indicate we intend to create a relation file as part of the current transaction.
 *
 * An XLOG IntentToCreate record is generated that will guard the subsequent file-system
 * create in case the transaction aborts.
 *
 * After 1 or more calls to this routine to mark intention about relation files that are going
 * to be created, call ~_DoPendingCreates to do the actual file-system creates.  (See its
 * note on XLOG flushing).
 */
extern void PersistentFilespace_MarkCreatePending(
	Oid 		filespaceOid,
				/* The filespace where the filespace lives. */

	int16		primaryDbId,
	
	char		*primaryFilespaceLocation,
				/* 
				 * The primary filespace directory path.  NOT Blank padded.
				 * Just a NULL terminated string.
				 */
	
	int16		mirrorDbId,
	
	char		*mirrorFilespaceLocation,
	
	ItemPointer		persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */

	bool			flushToXLog);
				/* When true, the XLOG record for this change will be flushed to disk. */

// -----------------------------------------------------------------------------
// Transaction End	
// -----------------------------------------------------------------------------
				
/*
 * Indicate the transaction commited and the relation is officially created.
 */
extern void PersistentFilespace_Created(							
	Oid 		filespaceOid,
				/* The filespace OID for the create. */
	
	ItemPointer 	persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */
				
	int64			serialNum);
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */
					
/*
 * Indicate we intend to drop a relation file as part of the current transaction.
 *
 * This relation file to drop will be listed inside a commit, distributed commit, a distributed 
 * prepared, and distributed commit prepared XOG records.
 *
 * For any of the commit type records, once that XLOG record is flushed then the actual
 * file-system delete will occur.  The flush guarantees the action will be retried after system
 * crash.
 */
extern void PersistentFilespace_MarkDropPending(
	Oid 		filespaceOid,
				/* The filespace OID for the drop. */
	
	ItemPointer 	persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */
							
	int64			serialNum);
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */

/*
 * Indicate we are aborting the create of a relation file.
 *
 * This state will make sure the relation gets dropped after a system crash.
 */
extern void PersistentFilespace_MarkAbortingCreate(
	Oid 		filespaceOid,
				/* The filespace OID for the aborting create. */
							
	ItemPointer 	persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */
							
	int64			serialNum);
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */
					
/*
 * Indicate we phsyicalled removed the relation file.
 */
extern void PersistentFilespace_Dropped(
	Oid 		filespaceOid,
				/* The filespace OID for the dropped filespace. */
										
	ItemPointer 	persistentTid,
				/* TID of the gp_persistent_rel_files tuple for the rel file */
							
	int64			serialNum);
				/* Serial number for the relation.	Distinquishes the uses of the tuple. */
	
// -----------------------------------------------------------------------------
// Shmem and Startup/Shutdown
// -----------------------------------------------------------------------------
				
/*
 * Return the required shared-memory size for this module.
 */
extern Size PersistentFilespace_ShmemSize(void);
								
/*
 * Initialize the shared-memory for this module.
 */
extern void PersistentFilespace_ShmemInit(void);
																																																																
#endif   /* CDBPERSISTENTFILEESPACE_H */



