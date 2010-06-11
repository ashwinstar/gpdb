/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/smgr/smgr.c,v 1.101.2.2 2007/07/20 16:29:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/persistentfilesysobjname.h"
#include "access/xact.h"
#include "access/xlogmm.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_resqueue.h"
#include "catalog/pg_tablespace.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "commands/filespace.h"
#include "commands/tablespace.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <glob.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 */
static HTAB *SMgrRelationHash = NULL;

/*
 * We keep a list of all relations (represented as RelFileNode values)
 * that have been created or deleted in the current transaction.  When
 * a relation is created, we create the physical file immediately, but
 * remember it so that we can delete the file again if the current
 * transaction is aborted.	Conversely, a deletion request is NOT
 * executed immediately, but is just entered in the list.  When and if
 * the transaction commits, we can delete the physical file.
 *
 * To handle subtransactions, every entry is marked with its transaction
 * nesting level.  At subtransaction commit, we reassign the subtransaction's
 * entries to the parent nesting level.  At subtransaction abort, we can
 * immediately execute the abort-time actions for all entries of the current
 * nesting level.
 *
 * NOTE: the list is kept in TopMemoryContext to be sure it won't disappear
 * unbetimes.  It'd probably be OK to keep it in TopTransactionContext,
 * but I'm being paranoid.
 */
typedef struct PendingDelete
{
	PersistentFileSysObjName fsObjName;		
							/* File-system object that may need to be deleted */
	
	PersistentFileSysRelStorageMgr relStorageMgr;

	char		*relationName;
	
	bool		isLocalBuf;	    /* CDB: true => uses local buffer mgr */
	bool		dropForCommit;		/* T=delete at commit; F=delete at abort */
	bool		sameTransCreateDrop; /* Collapsed create-delete? */
	ItemPointerData persistentTid;
	int64		persistentSerialNum;
	int			nestLevel;		/* xact nesting level of request */
	struct PendingDelete *next;		/* linked-list link */
} PendingDelete;

static PendingDelete *pendingDeletes = NULL; /* head of linked list */
static int pendingDeletesCount = 0;
static bool pendingDeletesSorted = false;
static bool pendingDeletesPerformed = true;

typedef PendingDelete *PendingDeletePtr;

static PendingDelete *PendingDelete_AddEntry(
	PersistentFileSysObjName *fsObjName,

	ItemPointer 			persistentTid,

	int64					persistentSerialNum,

	bool					dropForCommit)
{
	PendingDelete *pending; 

	/* Add the filespace to the list of stuff to delete at abort */
	pending = (PendingDelete *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(PendingDelete));

	pending->fsObjName = *fsObjName;
	pending->isLocalBuf = false;
	pending->relationName = NULL;
	pending->relStorageMgr = PersistentFileSysRelStorageMgr_None;
	pending->dropForCommit = dropForCommit;
	pending->sameTransCreateDrop = false;
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->persistentTid = *persistentTid;
	pending->persistentSerialNum = persistentSerialNum;
	pending->next = pendingDeletes;
	pendingDeletes = pending;
	pendingDeletesCount++;
	pendingDeletesSorted = false;
	pendingDeletesPerformed = false;

	return pending;
}

static PendingDelete *PendingDelete_AddCreatePendingEntry(
	PersistentFileSysObjName *fsObjName,

	ItemPointer 			persistentTid,

	int64					persistentSerialNum)
{
	return PendingDelete_AddEntry(
							fsObjName,
							persistentTid,
							persistentSerialNum,
							/* dropForCommit */ false);
}

static PendingDelete *PendingDelete_AddDropEntry(
	PersistentFileSysObjName *fsObjName,

	ItemPointer 			persistentTid,

	int64					persistentSerialNum)
{
	return PendingDelete_AddEntry(
							fsObjName,
							persistentTid,
							persistentSerialNum,
							/* dropForCommit */ true);
}

static inline PersistentEndXactFileSysAction PendingDelete_Action(
	PendingDelete *pendingDelete)
{
	if (pendingDelete->dropForCommit)
	{
		return (pendingDelete->sameTransCreateDrop ?
						PersistentEndXactFileSysAction_AbortingCreateNeeded :
						PersistentEndXactFileSysAction_Drop);
	}
	else
		return PersistentEndXactFileSysAction_Create;
}

typedef struct AppendOnlyMirrorResyncEofsKey
{
	RelFileNode	relFileNode;

	int32		segmentFileNum;

	int			nestLevel;		/* Transaction nesting level. */
} AppendOnlyMirrorResyncEofsKey;

typedef struct AppendOnlyMirrorResyncEofs
{
	AppendOnlyMirrorResyncEofsKey key;		

	char		*relationName;
	
	ItemPointerData persistentTid;
	int64			persistentSerialNum;

	int64		mirrorLossEof;
	int64		mirrorNewEof;

} AppendOnlyMirrorResyncEofs;

static HTAB* AppendOnlyMirrorResyncEofsTable = NULL;

/*
 * Declarations for smgr-related XLOG records
 *
 * Note: we log file creation and truncation here, but logging of deletion
 * actions is handled by xact.c, because it is part of transaction commit.
 */

/* 
 * XLOG gives us high 4 bits. Unlike in the xlog code, we need not
 * make the flags OR-able, we we have 16 bits to play with here.
 */
#define XLOG_SMGR_CREATE	0x10
#define XLOG_SMGR_TRUNCATE	0x20
#define XLOG_SMGR_CREATE_DIR 0x30

typedef struct xl_smgr_create
{
	RelFileNode rnode;
} xl_smgr_create;

typedef struct xl_smgr_truncate
{
	BlockNumber blkno;
	RelFileNode rnode;
	ItemPointerData persistentTid;
	int64 persistentSerialNum;
} xl_smgr_truncate;

/* local function prototypes */
static void smgrshutdown(int code, Datum arg);
static void smgr_internal_unlink(
	RelFileNode 				rnode,

	bool 						isLocalBuf,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	bool  						primaryOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred);

static void
AppendOnlyMirrorResyncEofs_Merge(
	RelFileNode		*relFileNode,

	int32			segmentFileNum,

	int				nestLevel,		/* Transaction nesting level. */
	
	char			*relationName,
	
	ItemPointer 	persistentTid,
	int64			persistentSerialNum,

	int64			mirrorLossEof,
	int64			mirrorNewEof);

static void
AppendOnlyMirrorResyncEofs_RemoveForDrop(
	RelFileNode		*relFileNode,

	int32			segmentFileNum,

	int				nestLevel);		/* Transaction nesting level. */

char *StorageManagerMirrorMode_Name(
	StorageManagerMirrorMode		mirrorMode)
{
	switch (mirrorMode)
	{
	case StorageManagerMirrorMode_None: 		return "None";
	case StorageManagerMirrorMode_PrimaryOnly: 	return "Primary Only";
	case StorageManagerMirrorMode_MirrorOnly: 	return "Mirror Only";
	case StorageManagerMirrorMode_Both: 		return "Both";

	default:
		return "Unknown";
	}
}

/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								  managers.
 *
 * Note: smgrinit is called during backend startup (normal or standalone
 * case), *not* during postmaster start.  Therefore, any resources created
 * here or destroyed in smgrshutdown are backend-local.
 */
void
smgrinit(void)
{
	mdinit();

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * on_proc_exit hook for smgr cleanup during backend shutdown
 */
static void
smgrshutdown(int code, Datum arg)
{
}

/*
 *	smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 *		This does not attempt to actually open the object.
 */
SMgrRelation
smgropen(RelFileNode rnode)
{
	SMgrRelation reln;
	bool		found;

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(RelFileNode);
		ctl.entrysize = sizeof(SMgrRelationData);
		ctl.hash = tag_hash;
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_FUNCTION);
	}

	/* Look up or create an entry */
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		/* hash_search already filled in the lookup key */
		reln->smgr_owner = NULL;
		reln->smgr_which = 0;	/* we only have md.c at present */
		reln->md_mirvec = NULL;		/* mark it not open */
	}

	return reln;
}

/*
 * smgrsetowner() -- Establish a long-lived reference to an SMgrRelation object
 *
 * There can be only one owner at a time; this is sufficient since currently
 * the only such owners exist in the relcache.
 */
void
smgrsetowner(SMgrRelation *owner, SMgrRelation reln)
{
	/*
	 * First, unhook any old owner.  (Normally there shouldn't be any, but it
	 * seems possible that this can happen during swap_relation_files()
	 * depending on the order of processing.  It's ok to close the old
	 * relcache entry early in that case.)
	 */
	if (reln->smgr_owner)
		*(reln->smgr_owner) = NULL;

	/* Now establish the ownership relationship. */
	reln->smgr_owner = owner;
	*owner = reln;
}

/*
 *	smgrclose() -- Close and delete an SMgrRelation object.
 */
void
smgrclose(SMgrRelation reln)
{
	SMgrRelation *owner;

	if (!mdclose(reln))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));

	owner = reln->smgr_owner;

	if (hash_search(SMgrRelationHash,
					(void *) &(reln->smgr_rnode),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	/*
	 * Unhook the owner pointer, if any.  We do this last since in the remote
	 * possibility of failure above, the SMgrRelation object will still exist.
	 */
	if (owner)
		*owner = NULL;
}

/*
 *	smgrcloseall() -- Close all existing SMgrRelation objects.
 */
void
smgrcloseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrclose(reln);
}

/*
 *	smgrclosenode() -- Close SMgrRelation object for given RelFileNode,
 *					   if one exists.
 *
 * This has the same effects as smgrclose(smgropen(rnode)), but it avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrclosenode(RelFileNode rnode)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrclose(reln);
}

/*
 *	smgrcreatefilespacedirpending() -- Create a new filespace directory.
 *
 */
void
smgrcreatefilespacedirpending(
	Oid 							filespaceOid,

	int16 							primaryDbId,

	char 							*primaryFilespaceLocation,

	int16 							mirrorDbId,

	char 							*mirrorFilespaceLocation,

	MirroredObjectExistenceState 	mirrorExistenceState,

	ItemPointer						persistentTid,

	int64							*persistentSerialNum,

	bool							flushToXLog)
{
	PersistentFileSysObjName fsObjName;		
	
	PersistentFileSysObjName_SetFilespaceDir(
										&fsObjName,
										filespaceOid);
	
	PersistentFilespace_MarkCreatePending(
								filespaceOid,
								primaryDbId,
								primaryFilespaceLocation,
								mirrorDbId,
								mirrorFilespaceLocation,
								mirrorExistenceState,
								persistentTid,
								persistentSerialNum,
								flushToXLog);

	PendingDelete_AddCreatePendingEntry(
								&fsObjName,
								persistentTid,
								*persistentSerialNum);
}

/*
 *	smgrcreatefilespacedir() -- Create a new filespace directory.
 *
 */
void
smgrcreatefilespacedir(
	Oid 						filespaceOid,

	char						*primaryFilespaceLocation,
								/* 
								 * The primary filespace directory path.  NOT Blank padded.
								 * Just a NULL terminated string.
								 */

	char						*mirrorFilespaceLocation,

	StorageManagerMirrorMode	mirrorMode,

	bool						ignoreAlreadyExists,

	int 						*primaryError,

	bool						*mirrorDataLossOccurred)
{

#ifdef MASTER_MIRROR_SYNC
	mmxlog_log_create_filespace(filespaceOid);	
#endif

	mdcreatefilespacedir(
					filespaceOid, 
					primaryFilespaceLocation, 
					mirrorFilespaceLocation, 
					mirrorMode,
					ignoreAlreadyExists,
					primaryError,
					mirrorDataLossOccurred);
}

/*
 *	smgrcreatetablespacedirpending() -- Create a new tablespace directory.
 *
 */
void
smgrcreatetablespacedirpending(
	TablespaceDirNode 				*tablespaceDirNode,

	MirroredObjectExistenceState 	mirrorExistenceState,

	ItemPointer						persistentTid,

	int64							*persistentSerialNum,

	bool							flushToXLog)
{
	PersistentFileSysObjName fsObjName;		

	PersistentFileSysObjName_SetTablespaceDir(
								&fsObjName,
								tablespaceDirNode->tablespace);

	PersistentTablespace_MarkCreatePending(
								tablespaceDirNode->filespace,
								tablespaceDirNode->tablespace,
								mirrorExistenceState,
								persistentTid,
								persistentSerialNum,
								flushToXLog);

	PendingDelete_AddCreatePendingEntry(
						&fsObjName,
						persistentTid,
						*persistentSerialNum);
}

/*
 *	smgrcreatetablespacedir() -- Create a new tablespace directory.
 *
 */
void
smgrcreatetablespacedir(
	Oid							tablespaceOid,

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{
#ifdef MASTER_MIRROR_SYNC
	mmxlog_log_create_tablespace(tablespaceOid);	
#endif

	mdcreatetablespacedir(
					tablespaceOid, 
					mirrorMode,
					ignoreAlreadyExists,
					primaryError,
					mirrorDataLossOccurred);
}

/*
 *	smgrcreatedbdirpending() -- Create a new database directory.
 *
 */
void
smgrcreatedbdirpending(
	DbDirNode 						*dbDirNode,

	MirroredObjectExistenceState 	mirrorExistenceState,

	ItemPointer						persistentTid,

	int64							*persistentSerialNum,

	bool							flushToXLog)
{
	PersistentFileSysObjName fsObjName;		
	
	Oid tablespace = dbDirNode->tablespace;
	Oid database = dbDirNode->database;
		
	PersistentFileSysObjName_SetDatabaseDir(
									&fsObjName,
									tablespace,
									database);

	PersistentDatabase_MarkCreatePending(
								dbDirNode,
								mirrorExistenceState,
								persistentTid,
								persistentSerialNum,
								flushToXLog);

	PendingDelete_AddCreatePendingEntry(
						&fsObjName,
						persistentTid,
						*persistentSerialNum);
}

/*
 *	smgrcreatedbdir() -- Create a new database directory.
 *
 */
void
smgrcreatedbdir(
	DbDirNode					*dbDirNode,

	StorageManagerMirrorMode 	mirrorMode,

	bool						ignoreAlreadyExists,

	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{
#ifdef MASTER_MIRROR_SYNC
	mmxlog_log_create_database(dbDirNode->tablespace, dbDirNode->database);	
#endif

	mdcreatedbdir(
			dbDirNode, 
			mirrorMode,
			ignoreAlreadyExists,
			primaryError,
			mirrorDataLossOccurred);
}

void
smgrcreatedbdirjustintime(
	DbDirNode 					*justInTimeDbDirNode,

	MirroredObjectExistenceState 	mirrorExistenceState,

	StorageManagerMirrorMode 	mirrorMode,

	ItemPointer 				persistentTid,
	
	int64 						*persistentSerialNum,

	int 						*primaryError,
	
	bool 						*mirrorDataLossOccurred)
{

	PersistentDatabase_MarkJustInTimeCreatePending(
											justInTimeDbDirNode,
											mirrorExistenceState,
											persistentTid,
											persistentSerialNum);

#ifdef MASTER_MIRROR_SYNC
	mmxlog_log_create_database(justInTimeDbDirNode->tablespace,
							   justInTimeDbDirNode->database);	
#endif

	mdcreatedbdir(
				justInTimeDbDirNode, 
				mirrorMode,
				/* ignoreAlreadyExists */ false,
				primaryError,
				mirrorDataLossOccurred);
	if (*primaryError != 0)
	{
		PersistentDatabase_AbandonJustInTimeCreatePending(
													justInTimeDbDirNode,
													persistentTid,
													*persistentSerialNum);
		return;
	}

	PersistentDatabase_JustInTimeCreated(
									justInTimeDbDirNode,
									persistentTid,
									*persistentSerialNum);
}


void smgrcreatepending(
	RelFileNode						*relFileNode,

	int32							segmentFileNum,

	PersistentFileSysRelStorageMgr relStorageMgr,

	MirroredObjectExistenceState 	mirrorExistenceState,

	MirroredRelDataSynchronizationState relDataSynchronizationState,

	char							*relationName,

	ItemPointer						persistentTid,

	int64							*persistentSerialNum,

	bool							isLocalBuf,

	bool							flushToXLog)
{
	PersistentFileSysObjName fsObjName;		
	
	PendingDelete *pending;
		
	PersistentFileSysObjName_SetRelationFile(
										&fsObjName,
										relFileNode,
										segmentFileNum);
		
	PersistentRelation_AddCreatePending(
								relFileNode,
								segmentFileNum,
								relStorageMgr,
								mirrorExistenceState,
								relDataSynchronizationState,
								relationName,
								persistentTid,
								persistentSerialNum,
								flushToXLog);
	
	
	pending = PendingDelete_AddCreatePendingEntry(
								&fsObjName,
								persistentTid,
								*persistentSerialNum);

	pending->relStorageMgr = relStorageMgr;
	pending->relationName = MemoryContextStrdup(TopMemoryContext, relationName);
	pending->isLocalBuf = isLocalBuf;	/*CDB*/
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		Given an already-created (but presumably unused) SMgrRelation,
 *		cause the underlying disk file or other storage to be created.
 *
 *		We assume the persistent 'Create Pending' work has already been done.
 *
 *		And, we assume the Just-In-Time database directory in the tablespace has already
 *		been created.
 */
void
smgrcreate(
	SMgrRelation 				reln,

	bool 						isLocalBuf, 

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */

	MirrorDataLossTrackingState mirrorDataLossTrackingState,

	int64						mirrorDataLossTrackingSessionNum,
	
	bool						ignoreAlreadyExists,

	int							*primaryError,

	bool						*mirrorDataLossOccurred)
{

#ifdef MASTER_MIRROR_SYNC
	/* ignoreAlreadyExists will only be true if we're called during recovery */
	if (!ignoreAlreadyExists)
		mmxlog_log_create_relfilenode(reln->smgr_rnode.spcNode,
									  reln->smgr_rnode.dbNode,
									  reln->smgr_rnode.relNode,
									  0);	
#endif

	mdcreate(
			reln,
			isLocalBuf,
			relationName,
			mirrorDataLossTrackingState,
			mirrorDataLossTrackingSessionNum,
			ignoreAlreadyExists,
			primaryError,
			mirrorDataLossOccurred);
}

/*
 *	smgrscheduleunlink() -- Schedule unlinking a relation at xact commit.
 *
 *		The relation is marked to be removed from the store if we
 *		successfully commit the current transaction.
 *
 * This also implies smgrclose() on the SMgrRelation object.
 */
void
smgrscheduleunlink(
	RelFileNode 	*relFileNode,

	int32			segmentFileNum,

	PersistentFileSysRelStorageMgr relStorageMgr,

	bool 			isLocalBuf,

	char			*relationName,

	ItemPointer 	persistentTid,

	int64 			persistentSerialNum)
{
	PersistentFileSysObjName fsObjName;		
	
	PendingDelete *pending;

	PersistentFileSysObjName_SetRelationFile(
										&fsObjName,
										relFileNode,
										segmentFileNum);

	pending = PendingDelete_AddDropEntry(
								&fsObjName,
								persistentTid,
								persistentSerialNum);

	pending->relStorageMgr = relStorageMgr;
	pending->relationName = MemoryContextStrdup(TopMemoryContext, relationName);
	pending->isLocalBuf = isLocalBuf;	/*CDB*/

	if (relStorageMgr == PersistentFileSysRelStorageMgr_AppendOnly)
	{
		/*
		 * Remove pending updates for Append-Only mirror resync EOFs, too.
		 *
		 * But only at this transaction level !!!
		 */
		AppendOnlyMirrorResyncEofs_RemoveForDrop(
											relFileNode,
											segmentFileNum,
											GetCurrentTransactionNestLevel());
	}
}

/*
 *	smgrdounlink() -- Immediately unlink a relation.
 *
 *		The relation is removed from the store.  This should not be used
 *		during transactional operations, since it can't be undone.
 *
 *		If isRedo is true, it is okay for the underlying file to be gone
 *		already.
 *
 * This also implies smgrclose() on the SMgrRelation object.
 */
void
smgrdounlink(
	RelFileNode 				*relFileNode,

	bool 						isLocalBuf, 

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	bool  						primaryOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
	smgr_internal_unlink(
				*relFileNode, 
				isLocalBuf,
				relationName,
				primaryOnly,
				ignoreNonExistence,
				mirrorDataLossOccurred);
}

/*
 * Shared subroutine that actually does the unlink ...
 */
static void
smgr_internal_unlink(
	RelFileNode 				rnode,

	bool 						isLocalBuf,

	char						*relationName,
					/* For tracing only.  Can be NULL in some execution paths. */
	
	bool  						primaryOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
	/*
	 * Get rid of any remaining buffers for the relation.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(rnode, isLocalBuf, 0);   /*CDB*/

	/*
	 * Tell the free space map to forget this relation.  It won't be accessed
	 * any more anyway, but we may as well recycle the map space quickly.
	 */
	FreeSpaceMapForgetRel(&rnode);

	/*
	 * It'd be nice to tell the stats collector to forget it immediately, too.
	 * But we can't because we don't know the OID (and in cases involving
	 * relfilenode swaps, it's not always clear which table OID to forget,
	 * anyway).
	 */

	/*
	 * And delete the physical files.
	 *
	 * Note: we treat deletion failure as a WARNING, not an error, because
	 * we've already decided to commit or abort the current xact.
	 */
	if (!mdunlink(rnode, relationName, primaryOnly, ignoreNonExistence, mirrorDataLossOccurred))
	{
		if (relationName == NULL)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove relation %u/%u/%u: %m",
							rnode.spcNode,
							rnode.dbNode,
							rnode.relNode)));
		else
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove relation %u/%u/%u '%s': %m",
							rnode.spcNode,
							rnode.dbNode,
							rnode.relNode,
							relationName)));
	}
}

/*
 *	smgrschedulermfilespacedir() -- Schedule removing a filespace directory at xact commit.
 *
 *		The filespace directory is marked to be removed from the store if we
 *		successfully commit the current transaction.
 */
void
smgrschedulermfilespacedir(
	Oid 				filespaceOid,

	ItemPointer 		persistentTid,

	int64				persistentSerialNum)
{
	PersistentFileSysObjName fsObjName;		
		
	PersistentFileSysObjName_SetFilespaceDir(
									&fsObjName,
									filespaceOid);
	
	PendingDelete_AddDropEntry(
						&fsObjName,
						persistentTid,
						persistentSerialNum);
}

void
smgrdormfilespacedir(
	Oid							filespaceOid,

	char						*primaryFilespaceLocation,
								/* 
								 * The primary filespace directory path.  NOT Blank padded.
								 * Just a NULL terminated string.
								 */
	
	char						*mirrorFilespaceLocation,
	
	bool						primaryOnly,

	bool					 	mirrorOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
#ifdef MASTER_MIRROR_SYNC
	if (!ignoreNonExistence)
		mmxlog_log_remove_filespace(filespaceOid);
#endif

	/*
	 * And remove the physical filespace directory.
	 *
	 * Note: we treat deletion failure as a WARNING, not an error, because
	 * we've already decided to commit or abort the current xact.
	 */
	if (!mdrmfilespacedir(
						filespaceOid,
						primaryFilespaceLocation,
						mirrorFilespaceLocation,
						primaryOnly,
						mirrorOnly,
						ignoreNonExistence,
						mirrorDataLossOccurred))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove filespace directory %u: %m",
						filespaceOid)));
}


/*
 *	smgrschedulermtablespacedir() -- Schedule removing a tablespace directory at xact commit.
 *
 *		The tablespace directory is marked to be removed from the store if we
 *		successfully commit the current transaction.
 */
void
smgrschedulermtablespacedir(
	Oid 				tablespaceOid,

	ItemPointer 		persistentTid,

	int64				persistentSerialNum)
{
	PersistentFileSysObjName fsObjName;		
	
	PersistentFileSysObjName_SetTablespaceDir(
									&fsObjName,
									tablespaceOid);
	
	PendingDelete_AddDropEntry(
						&fsObjName,
						persistentTid,
						persistentSerialNum);
}

/*
 *	smgrschedulermdbdir() -- Schedule removing a DB directory at xact commit.
 *
 *		The database directory is marked to be removed from the store if we
 *		successfully commit the current transaction.
 */
void
smgrschedulermdbdir(
	DbDirNode			*dbDirNode,
	
	ItemPointer			persistentTid,

	int64 				persistentSerialNum)
{
	PersistentFileSysObjName fsObjName;		
	
	Oid tablespace;
	Oid database;

	tablespace = dbDirNode->tablespace;
	database = dbDirNode->database;
	
	PersistentFileSysObjName_SetDatabaseDir(
									&fsObjName,
									tablespace,
									database);
	
	PendingDelete_AddDropEntry(
						&fsObjName,
						persistentTid,
						persistentSerialNum);
}

void
smgrdormtablespacedir(
	Oid							tablespaceOid,
	
	bool						primaryOnly,

	bool					 	mirrorOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
#ifdef MASTER_MIRROR_SYNC
	mmxlog_log_remove_tablespace(tablespaceOid);
#endif

	/*
	 * And remove the physical tablespace directory.
	 *
	 * Note: we treat deletion failure as a WARNING, not an error, because
	 * we've already decided to commit or abort the current xact.
	 */
	if (!mdrmtablespacedir(tablespaceOid, primaryOnly, mirrorOnly, ignoreNonExistence, mirrorDataLossOccurred))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove tablespace directory %u: %m",
						tablespaceOid)));
}

/*
 * Shared subroutine that actually does the rmdir of a database directory ...
 */
static void
smgr_internal_rmdbdir(
	DbDirNode					*dbDirNode,
	
	bool						primaryOnly,

	bool					 	mirrorOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
	/*
	 * And remove the physical database directory.
	 *
	 * Note: we treat deletion failure as a WARNING, not an error, because
	 * we've already decided to commit or abort the current xact.
	 */
	if (!mdrmdbdir(dbDirNode, primaryOnly, mirrorOnly, ignoreNonExistence, mirrorDataLossOccurred))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove database directory %u/%u: %m",
						dbDirNode->tablespace,
						dbDirNode->database)));
}

void
smgrdormdbdir(
	DbDirNode					*dbDirNode,
	
	bool						primaryOnly,

	bool					 	mirrorOnly,

	bool 						ignoreNonExistence,

	bool						*mirrorDataLossOccurred)
{
#ifdef MASTER_MIRROR_SYNC
	mmxlog_log_remove_database(dbDirNode->tablespace, dbDirNode->database);
#endif

	smgr_internal_rmdbdir(
					dbDirNode,
					primaryOnly, 
					mirrorOnly,
					ignoreNonExistence,
					mirrorDataLossOccurred);
}

void smgrappendonlymirrorresynceofs(
	RelFileNode						*relFileNode,

	int32							segmentFileNum,

	char							*relationName,

	ItemPointer						persistentTid,

	int64							persistentSerialNum,

	int64							mirrorLossEof,

	int64							mirrorNewEof)
{
	Assert(mirrorNewEof > 0);

	AppendOnlyMirrorResyncEofs_Merge(
								relFileNode,
								segmentFileNum,
								GetCurrentTransactionNestLevel(),
								relationName,
								persistentTid,
								persistentSerialNum,
								mirrorLossEof,
								mirrorNewEof);

	/*
	 * Indicate we have work to do in AtEOXact_smgr.
	 */
	pendingDeletesPerformed = false;
}

/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		The semantics are basically the same as smgrwrite(): write at the
 *		specified position.  However, we are expecting to extend the
 *		relation (ie, blocknum is the current EOF), and so in case of
 *		failure we clean up by truncating.
 */
void
smgrextend(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{
	if (!mdextend(reln, blocknum, buffer, isTemp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not extend relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode),
				 errhint("Check free disk space.")));
}

/*
 *	smgrread() -- read a particular block from a relation into the supplied
 *				  buffer.
 *
 *		This routine is called from the buffer manager in order to
 *		instantiate pages in the shared buffer cache.  All storage managers
 *		return pages in the format that POSTGRES expects.
 */
void
smgrread(SMgrRelation reln, BlockNumber blocknum, char *buffer)
{
	if (!mdread(reln, blocknum, buffer))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read block %u of relation %u/%u/%u: %m",
						blocknum,
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is not a synchronous write -- the block is not necessarily
 *		on disk at return, only dumped out to the kernel.  However,
 *		provisions will be made to fsync the write before the next checkpoint.
 *
 *		isTemp indicates that the relation is a temp table (ie, is managed
 *		by the local-buffer manager).  In this case no provisions need be
 *		made to fsync the write before checkpointing.
 */
void
smgrwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{
	if (!mdwrite(reln, blocknum, buffer, isTemp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block %u of relation %u/%u/%u: %m",
						blocknum,
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));
}

/*
 *	smgrnblocks() -- Calculate the number of blocks in the
 *					 supplied relation.
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
BlockNumber
smgrnblocks(SMgrRelation reln)
{
	BlockNumber nblocks;

	nblocks = mdnblocks(reln);

	/*
	 * NOTE: if a relation ever did grow to 2^32-1 blocks, this code would
	 * fail --- but that's a good thing, because it would stop us from
	 * extending the rel another block and having a block whose number
	 * actually is InvalidBlockNumber.
	 */
	if (nblocks == InvalidBlockNumber)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not count blocks of relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));

	return nblocks;
}

/*
 *	smgrtruncate() -- Truncate supplied relation to the specified number
 *					  of blocks
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
BlockNumber
smgrtruncate(SMgrRelation reln, BlockNumber nblocks, bool isTemp, bool isLocalBuf, ItemPointer persistentTid, int64 persistentSerialNum)
{
	BlockNumber newblks;

	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(reln->smgr_rnode, isLocalBuf, nblocks);

	/*
	 * Tell the free space map to forget anything it may have stored for the
	 * about-to-be-deleted blocks.	We want to be sure it won't return bogus
	 * block numbers later on.
	 */
	FreeSpaceMapTruncateRel(&reln->smgr_rnode, nblocks);

	/* Do the truncation */
	newblks = mdtruncate(reln, nblocks, isTemp);
	if (newblks == InvalidBlockNumber)
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not truncate relation %u/%u/%u to %u blocks: %m",
					 reln->smgr_rnode.spcNode,
					 reln->smgr_rnode.dbNode,
					 reln->smgr_rnode.relNode,
					 nblocks)));

	if (!isTemp)
	{
		/*
		 * Make a non-transactional XLOG entry showing the file truncation.
		 * It's non-transactional because we should replay it whether the
		 * transaction commits or not; the underlying file change is certainly
		 * not reversible.
		 */
		XLogRecPtr	lsn;
		XLogRecData rdata;
		xl_smgr_truncate xlrec;

		xlrec.blkno = newblks;
		xlrec.rnode = reln->smgr_rnode;
		xlrec.persistentTid = *persistentTid;
		xlrec.persistentSerialNum = persistentSerialNum;

		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		lsn = XLogInsert(RM_SMGR_ID, XLOG_SMGR_TRUNCATE | XLOG_NO_TRAN,
						 &rdata);
	}

	return newblks;
}

bool smgrgetpersistentinfo(	
	XLogRecord		*record,

	RelFileNode	*relFileNode,

	ItemPointer	persistentTid,

	int64		*persistentSerialNum)
{
	uint8 info;

	Assert (record->xl_rmid == RM_SMGR_ID);
	
	info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);

		*relFileNode = xlrec->rnode;
		*persistentTid = xlrec->persistentTid;
		*persistentSerialNum = xlrec->persistentSerialNum;
		return true;
	}

	return false;
}

/*
 *	smgrimmedsync() -- Force the specified relation to stable storage.
 *
 *		Synchronously force all previous writes to the specified relation
 *		down to disk.
 *
 *		This is useful for building completely new relations (eg, new
 *		indexes).  Instead of incrementally WAL-logging the index build
 *		steps, we can just write completed index pages to disk with smgrwrite
 *		or smgrextend, and then fsync the completed index file before
 *		committing the transaction.  (This is sufficient for purposes of
 *		crash recovery, since it effectively duplicates forcing a checkpoint
 *		for the completed index.  But it is *not* sufficient if one wishes
 *		to use the WAL log for PITR or replication purposes: in that case
 *		we have to make WAL entries as well.)
 *
 *		The preceding writes should specify isTemp = true to avoid
 *		duplicative fsyncs.
 *
 *		Note that you need to do FlushRelationBuffers() first if there is
 *		any possibility that there are dirty buffers for the relation;
 *		otherwise the sync is not very meaningful.
 */
void
smgrimmedsync(SMgrRelation reln)
{
	if (!mdimmedsync(reln))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not sync relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));
}

static void
PendingDelete_Free(
	PendingDelete **ele)
{
	if ((*ele)->relationName != NULL)
		pfree((*ele)->relationName);

	pfree(*ele);

	*ele = NULL;
}

static void
AppendOnlyMirrorResyncEofs_HashTableInit(void)
{
	HASHCTL			info;
	int				hash_flags;

	/* Set key and entry sizes. */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(AppendOnlyMirrorResyncEofsKey);
	info.entrysize = sizeof(AppendOnlyMirrorResyncEofs);
	info.hash = tag_hash;
	info.hcxt = TopMemoryContext;

	hash_flags = (HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	AppendOnlyMirrorResyncEofsTable = hash_create("AO Mirror Resync EOFs", 10, &info, hash_flags);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager: Append-Only mirror resync eofs hash-table created");
}

static void
AppendOnlyMirrorResyncEofs_HashTableRemove(
	char *procName)
{
	if (AppendOnlyMirrorResyncEofsTable == NULL)
		return;

	hash_destroy(AppendOnlyMirrorResyncEofsTable);
	AppendOnlyMirrorResyncEofsTable = NULL;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager (%s): Append-Only mirror resync eofs hash-table removed",
			 procName);
}

static void
AppendOnlyMirrorResyncEofs_InitKey(
	AppendOnlyMirrorResyncEofsKey *key,

	RelFileNode		*relFileNode,

	int32			segmentFileNum,

	int				nestLevel)		/* Transaction nesting level. */
{
	MemSet(key, 0, sizeof(AppendOnlyMirrorResyncEofsKey));
	key->relFileNode = *relFileNode;
	key->segmentFileNum = segmentFileNum;
	key->nestLevel = nestLevel;
}

static void
AppendOnlyMirrorResyncEofs_Merge(
	RelFileNode		*relFileNode,

	int32			segmentFileNum,

	int				nestLevel,		/* Transaction nesting level. */
	
	char			*relationName,
	
	ItemPointer		persistentTid,
	int64			persistentSerialNum,

	int64			mirrorLossEof,
	int64			mirrorNewEof)
{
	AppendOnlyMirrorResyncEofsKey key;
	AppendOnlyMirrorResyncEofs *entry;
	bool found;

	if (AppendOnlyMirrorResyncEofsTable == NULL)
		AppendOnlyMirrorResyncEofs_HashTableInit();

	AppendOnlyMirrorResyncEofs_InitKey(
									&key,
									relFileNode,
									segmentFileNum,
									nestLevel);
	
	entry = 
		(AppendOnlyMirrorResyncEofs*) 
						hash_search(AppendOnlyMirrorResyncEofsTable,
									(void *) &key,
									HASH_ENTER,
									&found);

	if (!found)
	{
		entry->relationName = MemoryContextStrdup(TopMemoryContext, relationName);
		entry->persistentSerialNum = persistentSerialNum;
		entry->persistentTid = *persistentTid;
		entry->mirrorLossEof = mirrorLossEof;
		entry->mirrorNewEof = mirrorNewEof;
	}
	else
	{
		if (entry->mirrorLossEof == entry->mirrorNewEof)
		{
			/*
			 * If the 2 EOFs are synchronized, then we use the loss value (which is often equal to new).
			 */
			entry->mirrorLossEof = mirrorLossEof;
		}
		else
		{
			/*
			 * Old value should be less than or equal.  Keep it.
			 */
			if (entry->mirrorLossEof > mirrorLossEof)
			{
				elog(ERROR,
					"Merge Append-Only mirror resync mirror loss EOF current value should be less than or equal : %u/%u/%u, segment file #%d, relation name '%s' (transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ", input mirror loss EOF " INT64_FORMAT ", current mirror loss EOF " INT64_FORMAT ", input mirror new EOF " INT64_FORMAT ", current mirror new EOF " INT64_FORMAT ")",
					entry->key.relFileNode.spcNode,
					entry->key.relFileNode.dbNode,
					entry->key.relFileNode.relNode,
					entry->key.segmentFileNum,
					(entry->relationName == NULL ? "<null>" : entry->relationName),
					entry->key.nestLevel,
					ItemPointerToString(&entry->persistentTid),
					entry->persistentSerialNum,
					mirrorLossEof,
					entry->mirrorLossEof,
					mirrorNewEof,
					entry->mirrorNewEof);
			}
		}
		
		if (mirrorNewEof > entry->mirrorNewEof)
			entry->mirrorNewEof = mirrorNewEof;
	}

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager: %s Append-Only mirror resync eofs entry: %u/%u/%u, segment file #%d, relation name '%s' (transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ", input mirror loss EOF " INT64_FORMAT ", saved mirror loss EOF " INT64_FORMAT ", input mirror new EOF " INT64_FORMAT ", saved mirror new EOF " INT64_FORMAT ")",
			 (found ? "Merge" : "New"),
			 entry->key.relFileNode.spcNode,
			 entry->key.relFileNode.dbNode,
			 entry->key.relFileNode.relNode,
			 entry->key.segmentFileNum,
			 (entry->relationName == NULL ? "<null>" : entry->relationName),
			 entry->key.nestLevel,
			 ItemPointerToString(&entry->persistentTid),
			 entry->persistentSerialNum,
			 mirrorLossEof,
			 entry->mirrorLossEof,
			 mirrorNewEof,
			 entry->mirrorNewEof);
}

static void
AppendOnlyMirrorResyncEofs_Remove(
	char						*procName,

	AppendOnlyMirrorResyncEofs 	*entry)
{
	Assert(AppendOnlyMirrorResyncEofsTable != NULL);

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager (%s): Remove Append-Only mirror resync eofs entry: %u/%u/%u, segment file #%d, relation name '%s' (transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ", saved mirror loss EOF " INT64_FORMAT ", saved mirror new EOF " INT64_FORMAT ")",
			 procName,
			 entry->key.relFileNode.spcNode,
			 entry->key.relFileNode.dbNode,
			 entry->key.relFileNode.relNode,
			 entry->key.segmentFileNum,
			 (entry->relationName == NULL ? "<null>" : entry->relationName),
			 entry->key.nestLevel,
			 ItemPointerToString(&entry->persistentTid),
			 entry->persistentSerialNum,
			 entry->mirrorLossEof,
			 entry->mirrorNewEof);

	if (entry->relationName != NULL)
		pfree(entry->relationName);

	hash_search(
			AppendOnlyMirrorResyncEofsTable, 
			(void *) &entry->key, 
			HASH_REMOVE, 
			NULL);
}

static void
AppendOnlyMirrorResyncEofs_Promote(
	AppendOnlyMirrorResyncEofs *entry,

	int							newNestLevel)
{
	Assert(AppendOnlyMirrorResyncEofsTable != NULL);

	AppendOnlyMirrorResyncEofs_Merge(
								&entry->key.relFileNode,
								entry->key.segmentFileNum,
								newNestLevel,
								entry->relationName,
								&entry->persistentTid,
								entry->persistentSerialNum,
								entry->mirrorLossEof,
								entry->mirrorNewEof);

	AppendOnlyMirrorResyncEofs_Remove(
								"AppendOnlyMirrorResyncEofs_Promote",
								entry);
}

static void
AppendOnlyMirrorResyncEofs_RemoveForDrop(
	RelFileNode		*relFileNode,

	int32			segmentFileNum,

	int				nestLevel)		/* Transaction nesting level. */
{
	AppendOnlyMirrorResyncEofsKey key;
	AppendOnlyMirrorResyncEofs *entry;
	bool found;

	if (AppendOnlyMirrorResyncEofsTable == NULL)
		return;

	AppendOnlyMirrorResyncEofs_InitKey(
									&key,
									relFileNode,
									segmentFileNum,
									nestLevel);
	
	entry = 
		(AppendOnlyMirrorResyncEofs*) 
						hash_search(AppendOnlyMirrorResyncEofsTable,
									(void *) &key,
									HASH_FIND,
									&found);
	if (found)
		AppendOnlyMirrorResyncEofs_Remove(
								"AppendOnlyMirrorResyncEofs_RemoveForDrop",
								entry);
}

/*
 *	PostPrepare_smgr -- Clean up after a successful PREPARE
 *
 * What we have to do here is throw away the in-memory state about pending
 * relation deletes.  It's all been recorded in the 2PC state file and
 * it's no longer smgr's job to worry about it.
 */
void
PostPrepare_smgr(void)
{
	PendingDelete *pending;
	PendingDelete *next;

	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		pendingDeletes = next;

		pendingDeletesCount--;

		/* must explicitly free the list entry */
		PendingDelete_Free(&pending);
	}

	Assert(pendingDeletesCount == 0);
	pendingDeletesSorted = false;
	pendingDeletesPerformed = true;

	/*
	 * Free the Append-Only mirror resync EOFs hash table.
	 */
	AppendOnlyMirrorResyncEofs_HashTableRemove("PostPrepare_smgr");

}


static void
smgrDoDeleteActions(
	PendingDelete 	**list,
	int					*listCount,
	bool				forCommit)
{
	MIRRORED_LOCK_DECLARE;

	PendingDelete *current;
	int entryIndex;

	PersistentEndXactFileSysAction action;
	
	bool dropPending;
	bool abortingCreate;
	
	/*
	 * We need to do the transition to 'Aborting Create' or 'Drop Pending' and perform
	 * the file-system drop while under one acquistion of the MirroredLock.  Otherwise,
	 * we could race with resynchronize's ReDrop.
	 */
	MIRRORED_LOCK;

	/*
	 * First pass does the initial State-Changes.
	 */
	entryIndex = 0;
	current = *list;
	while (true)
	{
		/*
		 * Keep adjusting the list to maintain its integrity.
		 */
		if (current == NULL)
			break;
		
		action = PendingDelete_Action(current);

		if (Debug_persistent_print)
		{
			if (current->relationName == NULL)
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Do 1st delete state-change action for list entry #%d: '%s' (persistent end transaction action '%s', transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ")",
					 entryIndex,
					 PersistentFileSysObjName_TypeAndObjectName(&current->fsObjName),
					 PersistentEndXactFileSysAction_Name(action),
					 current->nestLevel,
					 ItemPointerToString(&current->persistentTid),
					 current->persistentSerialNum);
			else
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Do 1st delete state-change action for list entry #%d: '%s', relation name '%s' (persistent end transaction action '%s', transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ")",
					 entryIndex,
					 PersistentFileSysObjName_TypeAndObjectName(&current->fsObjName),
					 current->relationName,
					 PersistentEndXactFileSysAction_Name(action),
					 current->nestLevel,
					 ItemPointerToString(&current->persistentTid),
					 current->persistentSerialNum);
		}

		switch (action)
		{
		case PersistentEndXactFileSysAction_Create:
			if (forCommit)
			{
				PersistentFileSysObj_Created(
								&current->fsObjName,
								&current->persistentTid,
								current->persistentSerialNum,
								/* retryPossible */ false);
			}
			else
			{
				PersistentFileSysObj_MarkAbortingCreate(
								&current->fsObjName,
								&current->persistentTid,
								current->persistentSerialNum,
								/* retryPossible */ false);
			}
			break;

		case PersistentEndXactFileSysAction_Drop:
			if (forCommit)
			{
				PersistentFileSysObj_MarkDropPending(
								&current->fsObjName,
								&current->persistentTid,
								current->persistentSerialNum,
								/* retryPossible */ false);
			}
			break;

		case PersistentEndXactFileSysAction_AbortingCreateNeeded:
			/*
			 * Always whether transaction commits or aborts.
			 */
			PersistentFileSysObj_MarkAbortingCreate(
							&current->fsObjName,
							&current->persistentTid,
							current->persistentSerialNum,
							/* retryPossible */ false);
			break;
				
		default:
			elog(ERROR, "Unexpected persistent end transaction file-system action: %d",
				 action);
		}

		current = current->next;
		entryIndex++;

	}

	/*
	 * Make the above State-Changes permanent.
	 */
	PersistentFileSysObj_FlushXLog();

	/*
	 * Second pass does physical drops and final State-Changes.
	 */
	entryIndex = 0;
	while (true)
	{
		/*
		 * Keep adjusting the list to maintain its integrity.
		 */
		current = *list;
		if (current == NULL)
			break;
		
 		Assert(*listCount > 0);
		(*listCount)--;

		*list = current->next;

		action = PendingDelete_Action(current);

		dropPending = false;		// Assume.
		abortingCreate = false;		// Assume.

		switch (action)
		{
		case PersistentEndXactFileSysAction_Create:
			if (!forCommit)
			{
				abortingCreate = true;
			}
			break;

		case PersistentEndXactFileSysAction_Drop:
			if (forCommit)
			{
				dropPending = true;
			}
			break;

		case PersistentEndXactFileSysAction_AbortingCreateNeeded:
			/*
			 * Always whether transaction commits or aborts.
			 */
			abortingCreate = true;
			break;
				
		default:
			elog(ERROR, "Unexpected persistent end transaction file-system action: %d",
				 action);
		}
		
		if (abortingCreate || dropPending)
		{
			PersistentFileSysObj_EndXactDrop(
							&current->fsObjName,
							current->relStorageMgr,
							current->relationName,
							&current->persistentTid,
							current->persistentSerialNum,
							/* ignoreNonExistence */ abortingCreate);
		}

		/* must explicitly free the list entry */
		PendingDelete_Free(&current);

		entryIndex++;
		
	}
	Assert(*listCount == 0);
	Assert(*list == NULL);

	MIRRORED_UNLOCK;

	PersistentFileSysObj_FlushXLog();

}

static void
smgrDoAppendOnlyResyncEofs(bool forCommit)
{
	int appendOnlyMirrorResyncEofsCount = 0;

	HASH_SEQ_STATUS iterateStatus;
	AppendOnlyMirrorResyncEofs *entry;

	if (AppendOnlyMirrorResyncEofsTable == NULL)
		return;

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager: Enter Append-Only mirror resync eofs list entries (Append-Only commit work count %d)",
			 FileRepPrimary_GetAppendOnlyCommitWorkCount());

	hash_seq_init(
			&iterateStatus, 
			AppendOnlyMirrorResyncEofsTable);
	
	while ((entry = hash_seq_search(&iterateStatus)) != NULL)
	{
		if (forCommit)
		{
			PersistentFileSysObj_UpdateAppendOnlyMirrorResyncEofs(
															&entry->key.relFileNode,
															entry->key.segmentFileNum,
															&entry->persistentTid,
															entry->persistentSerialNum,
															entry->mirrorLossEof,
															entry->mirrorNewEof,
															/* recovery */ false,
															/* flushToXLog */ false);
		}

		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Storage Manager: Append-Only mirror resync eofs list entry #%d: %u/%u/%u, segment file #%d, relation name '%s' (forCommit %s, persistent TID %s, persistent serial number " INT64_FORMAT ", mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT ")",
				 appendOnlyMirrorResyncEofsCount,
				 entry->key.relFileNode.spcNode,
				 entry->key.relFileNode.dbNode,
				 entry->key.relFileNode.relNode,
				 entry->key.segmentFileNum,
				 (entry->relationName == NULL ? "<null>" : entry->relationName),
				 (forCommit ? "true" : "false"),
				 ItemPointerToString(&entry->persistentTid),
				 entry->persistentSerialNum,
				 entry->mirrorLossEof,
				 entry->mirrorNewEof);

		appendOnlyMirrorResyncEofsCount++;
	}

	/*
	 * Only for COMMIT do we collect Append-Only mirror resync EOFs for the commit record and issue intents.
	 */
	if (appendOnlyMirrorResyncEofsCount > 0)
	{
		MIRRORED_LOCK_DECLARE;

		int oldSystemAppendOnlyCommitWorkCount;

		MIRRORED_LOCK;	// NOTE: When we use the MirroredLock for the whole routine, this can go.
		
		oldSystemAppendOnlyCommitWorkCount = FileRepPrimary_GetAppendOnlyCommitWorkCount();	// Snapshot.

		if (forCommit)
		{
			int newSystemAppendOnlyCommitWorkCount;

			newSystemAppendOnlyCommitWorkCount = 
							oldSystemAppendOnlyCommitWorkCount - 
							appendOnlyMirrorResyncEofsCount;

			if (newSystemAppendOnlyCommitWorkCount < 0)
				elog(ERROR, 
					 "Append-Only Mirror Resync EOFs intent count would go negative "
					 "(system count %d, entry count %d)",
					 oldSystemAppendOnlyCommitWorkCount,
					 appendOnlyMirrorResyncEofsCount);
			
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Append-Only Mirror Resync EOFs intent count finishing commit work with system count %d remaining "
					 "(old system count %d, entry count %d)",
					 newSystemAppendOnlyCommitWorkCount,
					 oldSystemAppendOnlyCommitWorkCount,
					 appendOnlyMirrorResyncEofsCount);

			FileRepPrimary_FinishedAppendOnlyCommitWork(appendOnlyMirrorResyncEofsCount);
		}
		else
		{
			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Append-Only Mirror Resync EOFs intent count finishing abort work with system count %d remaining "
					 " (entry count %d)",
					 oldSystemAppendOnlyCommitWorkCount,
					 appendOnlyMirrorResyncEofsCount);
		}
			
		MIRRORED_UNLOCK;
	}

}

/*
 * A compare function for 2 PendingDelete.
 */
static int
PendingDelete_Compare(const PendingDelete *entry1, const PendingDelete *entry2)
{
	int cmp;

	cmp = PersistentFileSysObjName_Compare(
								&entry1->fsObjName, 
								&entry2->fsObjName);
	if (cmp == 0)
	{
		/*
		 * Sort CREATE before DROP for detecting same transaction create-drops.
		 */
		if (entry1->dropForCommit == entry2->dropForCommit)
			return 0;
		else if (entry1->dropForCommit)
			return 1;
		else
			return -1;
	}
	else 
		return cmp;
}

/*
 * A compare function for array of PendingDeletePtr for use with qsort.
 */
static int
PendingDeletePtr_Compare(const void *p1, const void *p2)
{
	const PendingDeletePtr *entry1Ptr = (PendingDeletePtr *) p1;
	const PendingDeletePtr *entry2Ptr = (PendingDeletePtr *) p2;
	const PendingDelete *entry1 = *entry1Ptr;
	const PendingDelete *entry2 = *entry2Ptr;

	return PendingDelete_Compare(entry1, entry2);
}

static void
smgrSortDeletesList(
	PendingDelete 	**list, 
	int 			*listCount,
	int				nestLevel)
{
	PendingDeletePtr *ptrArray;
	PendingDelete *current;
	int i;
	PendingDelete *prev;
	int collapseCount;

	if (*listCount == 0)
		return;

	ptrArray = 
			(PendingDeletePtr*)
						palloc(*listCount * sizeof(PendingDeletePtr));
	

	i = 0;
	for (current = *list; current != NULL; current = current->next)
	{
		ptrArray[i++] = current;
	}
	Assert(i == *listCount);

	/*
	 * Sort the list.
	 *
	 * Supports the collapsing of same transaction create-deletes and to be able
	 * to process relations before database directories, etc.
	 */
	qsort(
		ptrArray,
		*listCount, 
		sizeof(PendingDeletePtr),
		PendingDeletePtr_Compare);

	/*
	 * Collapse same transaction create-drops and re-link list.
	 */
	*list = ptrArray[0];
	prev = ptrArray[0];
	collapseCount = 0;
	i = 0;
	while (true)
	{
		i++;	// Start processing elements after the first one.

		if (i == *listCount)
		{
			prev->next = NULL;
			break;
		}

		current = ptrArray[i];

		/*
		 * Only do CREATE-DROP collapsing when both are at or below the requested
		 * transaction nest level.
		 */
		if (current->nestLevel >= nestLevel &&
			prev->nestLevel >= nestLevel &&
			(PersistentFileSysObjName_Compare(
								&prev->fsObjName, 
								&current->fsObjName) == 0))
		{
			if (prev->dropForCommit)
				elog(ERROR, "Expected a CREATE for file-system object name '%s'",
					PersistentFileSysObjName_ObjectName(&prev->fsObjName));
			if (!current->dropForCommit)
				elog(ERROR, "Expected a DROP for file-system object name '%s'",
					PersistentFileSysObjName_ObjectName(&current->fsObjName));

			prev->dropForCommit = true;				// Make the CREATE a DROP.
			prev->sameTransCreateDrop = true;	// Don't ignore DROP on abort.
			collapseCount++;

			if (Debug_persistent_print)
				elog(Debug_persistent_print_level, 
					 "Storage Manager: CREATE (transaction level %d) - DROP (transaction level %d) collapse for %s, filter transaction level %d, TID %s, serial " INT64_FORMAT,
					 current->nestLevel,
					 prev->nestLevel,
					 PersistentFileSysObjName_TypeAndObjectName(&current->fsObjName),
					 nestLevel,
					 ItemPointerToString(&current->persistentTid),
					 current->persistentSerialNum);
			
			PendingDelete_Free(&current);

			// Don't advance prev pointer.
		}
		else
		{
			// Re-link.
			prev->next = current;
			
			prev = current;
		}
	}

	pfree(ptrArray);

	/*
	 * Adjust count.
	 */
	(*listCount) -= collapseCount;

#ifdef suppress
	{
		PendingDelete	*check; 
		PendingDelete	*checkPrev;
		int checkCount;

		checkPrev = NULL;
		checkCount = 0;
		for (check = *list; check != NULL; check = check->next)
		{
			checkCount++;
			if (checkPrev != NULL)
			{
				int cmp;
			
				cmp = PendingDelete_Compare(
										checkPrev,
										check);
				if (cmp >= 0)
					elog(ERROR, "Not sorted correctly ('%s' >= '%s')",
						 PersistentFileSysObjName_ObjectName(&checkPrev->fsObjName),
						 PersistentFileSysObjName_ObjectName(&check->fsObjName));
					
			}

			checkPrev = check;
		}

		if (checkCount != *listCount)
			elog(ERROR, "List count does not match (expected %d, found %d)",
			     *listCount, checkCount);
	}
#endif
}

/*
 *	smgrSubTransAbort() -- Take care of relation deletes on sub-transaction abort.
 *
 * We want to clean up a failed subxact immediately.
 */
static void
smgrSubTransAbort(void)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingDelete *pending;
	PendingDelete *prev;
	PendingDelete *next;
	PendingDelete *subTransList;
	int			subTransCount;

	HASH_SEQ_STATUS iterateStatus;
	AppendOnlyMirrorResyncEofs *entry;

	subTransList = NULL;
	subTransCount = 0;
	prev = NULL;
	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		if (pending->nestLevel < nestLevel)
		{
			/* outer-level entries should not be processed yet */
			prev = pending;
		}
		else
		{
			if (prev)
				prev->next = next;
			else
				pendingDeletes = next;

			pendingDeletesCount--;

			// Move to sub-transaction list.
			pending->next = subTransList;
			subTransList = pending;

			subTransCount++;

			/* prev does not change */
		}
	}

	/*
	 * Sort the list in relation, database directory, tablespace, etc order.
	 * And, collapse same transaction create-deletes.
	 */
	smgrSortDeletesList(
					&subTransList, 
					&subTransCount,
					nestLevel);

	pendingDeletesSorted = (nestLevel <= 1);

	/*
	 * Do abort actions for the sub-transaction's creates and deletes.
	 */
	smgrDoDeleteActions(
					&subTransList, 
					&subTransCount,
					/* forCommit */ false);

	Assert(subTransList == NULL);
	Assert(subTransCount == 0);

	/*
	 * Throw away sub-transaction Append-Only mirror resync EOFs.
	 */
	hash_seq_init(
			&iterateStatus, 
			AppendOnlyMirrorResyncEofsTable);
	
	while ((entry = hash_seq_search(&iterateStatus)) != NULL)
	{
		if (entry->key.nestLevel >= nestLevel)
		{
			AppendOnlyMirrorResyncEofs_Remove(
										"smgrSubTransAbort",
										entry);
		}
	}

}

/*
 * smgrGetPendingDeletes() -- Get a list of relations to be deleted.
 *
 * The return value is the number of relations scheduled for termination.
 * *ptr is set to point to a freshly-palloc'd array of RelFileNodes.
 * If there are no relations to be deleted, *ptr is set to NULL.
 *
 * Note that the list does not include anything scheduled for termination
 * by upper-level transactions.
 */
int
smgrGetPendingDeletes(
	EndXactRecKind						endXactRecKind,

	PersistentEndXactFileSysActionInfo 	**ptr)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	int			nrels;

	PersistentEndXactFileSysActionInfo *rptr;

	PendingDelete *pending;
	int			entryIndex;

	PersistentEndXactFileSysAction action;

	Assert(endXactRecKind == EndXactRecKind_Commit ||
		   endXactRecKind == EndXactRecKind_Abort ||
		   endXactRecKind == EndXactRecKind_Prepare);

	if (!pendingDeletesSorted)
	{
		/*
		 * Sort the list in relation, database directory, tablespace, etc order.
		 * And, collapse same transaction create-deletes.
		 */
		smgrSortDeletesList(
						&pendingDeletes, 
						&pendingDeletesCount,
						nestLevel);

		pendingDeletesSorted = (nestLevel <= 1);
	}
	
	nrels = 0;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		action = PendingDelete_Action(pending);

		if (pending->nestLevel >= nestLevel &&
			EndXactRecKind_NeedsAction(endXactRecKind, action))
		{
			nrels++;
		}
	}
	if (nrels == 0)
	{
		*ptr = NULL;
		return 0;
	}

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager: Get list entries (transaction kind '%s', current transaction nest level %d)",
			 EndXactRecKind_Name(endXactRecKind),
			 nestLevel);

	rptr = (PersistentEndXactFileSysActionInfo *) 
							palloc(nrels * sizeof(PersistentEndXactFileSysActionInfo));
	*ptr = rptr;
	entryIndex = 0;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		bool returned;

		action = PendingDelete_Action(pending);
		returned = false;

		if (pending->nestLevel >= nestLevel && 
			EndXactRecKind_NeedsAction(endXactRecKind, action))
		{
			rptr->action = action;
			rptr->fsObjName = pending->fsObjName;
			rptr->relStorageMgr = pending->relStorageMgr;
			rptr->persistentTid = pending->persistentTid;
			rptr->persistentSerialNum = pending->persistentSerialNum;

			rptr++;
			returned = true;
		}

		if (Debug_persistent_print)
		{
			if (pending->relationName == NULL)
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Get list entry #%d: '%s' (transaction kind '%s', returned %s, transaction nest level %d, relation storage manager '%s', persistent TID %s, persistent serial number " INT64_FORMAT ")",
					 entryIndex,
					 PersistentFileSysObjName_TypeAndObjectName(&pending->fsObjName),
					 EndXactRecKind_Name(endXactRecKind),
					 (returned ? "true" : "false"),
					 pending->nestLevel,
					 PersistentFileSysRelStorageMgr_Name(pending->relStorageMgr),
					 ItemPointerToString(&pending->persistentTid),
					 pending->persistentSerialNum);
			else
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Get list entry #%d: '%s', relation name '%s' (transaction kind '%s', returned %s, transaction nest level %d, relation storage manager '%s', persistent TID %s, persistent serial number " INT64_FORMAT ")",
					 entryIndex,
					 PersistentFileSysObjName_TypeAndObjectName(&pending->fsObjName),
					 pending->relationName,
					 EndXactRecKind_Name(endXactRecKind),
					 (returned ? "true" : "false"),
					 pending->nestLevel,
					 PersistentFileSysRelStorageMgr_Name(pending->relStorageMgr),
					 ItemPointerToString(&pending->persistentTid),
					 pending->persistentSerialNum);
		}
		entryIndex++;
	}
	return nrels;
}

int
smgrGetAppendOnlyMirrorResyncEofs(
	EndXactRecKind									endXactRecKind,

	PersistentEndXactAppendOnlyMirrorResyncEofs 	**ptr)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	int			nentries;
	PersistentEndXactAppendOnlyMirrorResyncEofs *rptr;
	HASH_SEQ_STATUS iterateStatus;
	AppendOnlyMirrorResyncEofs *entry;
	int			entryIndex;

	nentries = 0;

	if (AppendOnlyMirrorResyncEofsTable !=  NULL)
	{
		hash_seq_init(
				&iterateStatus, 
				AppendOnlyMirrorResyncEofsTable);

		while ((entry = hash_seq_search(&iterateStatus)) != NULL)
		{
			if (entry->key.nestLevel >= nestLevel)
				nentries++;
		}
	}
	if (nentries == 0)
	{
		*ptr = NULL;
		return 0;
	}

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager: Get Append-Only mirror resync eofs list entries (current transaction nest level %d, Append-Only commit work system count %d)",
			 nestLevel,
			 FileRepPrimary_GetAppendOnlyCommitWorkCount());

	rptr = (PersistentEndXactAppendOnlyMirrorResyncEofs *) 
							palloc(nentries * sizeof(PersistentEndXactAppendOnlyMirrorResyncEofs));
	*ptr = rptr;
	entryIndex = 0;
	hash_seq_init(
			&iterateStatus, 
			AppendOnlyMirrorResyncEofsTable);
	
	while ((entry = hash_seq_search(&iterateStatus)) != NULL)
	{
		bool returned;

		returned = false;
		if (entry->key.nestLevel >= nestLevel)
		{
			MIRRORED_LOCK_DECLARE;

			rptr->relFileNode = entry->key.relFileNode;
			rptr->segmentFileNum = entry->key.segmentFileNum;

			rptr->persistentTid = entry->persistentTid;
			rptr->persistentSerialNum = entry->persistentSerialNum;

			rptr->mirrorLossEof = entry->mirrorLossEof;
			rptr->mirrorNewEof = entry->mirrorNewEof;

			rptr++;
			returned = true;

			/*
			 * UNDONE: Don't know if this is really the best place for this.
			 */
			MIRRORED_LOCK;
			FileRepPrimary_IntentAppendOnlyCommitWork();
			MIRRORED_UNLOCK;
		}

		if (Debug_persistent_print)
		{
			if (entry->relationName == NULL)
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Get Append-Only mirror resync eofs list entry #%d: %u/%u/%u, segment file #%d (returned %s, transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ", mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT ")",
					 entryIndex,
					 entry->key.relFileNode.spcNode,
					 entry->key.relFileNode.dbNode,
					 entry->key.relFileNode.relNode,
					 entry->key.segmentFileNum,
					 (returned ? "true" : "false"),
					 entry->key.nestLevel,
					 ItemPointerToString(&entry->persistentTid),
					 entry->persistentSerialNum,
					 entry->mirrorLossEof,
					 entry->mirrorNewEof);
			else
				elog(Debug_persistent_print_level, 
					 "Storage Manager: Get Append-Only mirror resync eofs list entry #%d: %u/%u/%u, segment file #%d, relation name '%s' (returned %s, transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ", mirror loss EOF " INT64_FORMAT ", mirror new EOF " INT64_FORMAT ")",
					 entryIndex,
					 entry->key.relFileNode.spcNode,
					 entry->key.relFileNode.dbNode,
					 entry->key.relFileNode.relNode,
					 entry->key.segmentFileNum,
					 entry->relationName,
					 (returned ? "true" : "false"),
					 entry->key.nestLevel,
					 ItemPointerToString(&entry->persistentTid),
					 entry->persistentSerialNum,
					 entry->mirrorLossEof,
					 entry->mirrorNewEof);
		}
		entryIndex++;
	}
	return nentries;
}

/*
 * smgrIsPendingDeletes() -- Returns true if there are relations to be deleted.
 *
 * Note that the list does not include anything scheduled for termination
 * by upper-level transactions.
 */
bool
smgrIsPendingDeletes(bool forCommit)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingDelete *pending;
	HASH_SEQ_STATUS iterateStatus;
	AppendOnlyMirrorResyncEofs *entry;

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->dropForCommit == forCommit)
			return true;
	}

	hash_seq_init(
			&iterateStatus, 
			AppendOnlyMirrorResyncEofsTable);
	
	while ((entry = hash_seq_search(&iterateStatus)) != NULL)
	{
		if (entry->key.nestLevel >= nestLevel)
		{
			hash_seq_term(&iterateStatus);
			return true;
		}
	}

	return false;
}



/*
 * AtSubCommit_smgr() --- Take care of subtransaction commit.
 *
 * Reassign all items in the pending-deletes list to the parent transaction.
 */
void
AtSubCommit_smgr(void)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingDelete *pending;
	HASH_SEQ_STATUS iterateStatus;
	AppendOnlyMirrorResyncEofs *entry;

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel)
		{
			pending->nestLevel = nestLevel - 1;
			
			if (pending->fsObjName.type == PersistentFsObjType_RelationFile &&
				pending->relStorageMgr == PersistentFileSysRelStorageMgr_AppendOnly)
			{
				/*
				 * If we are promoting a DROP of an Append-Only table, be sure to remove any
				 * pending Append-Only mirror resync EOFs updates for the NEW TRANSACTION
				 * LEVEL, too.
				 */
				AppendOnlyMirrorResyncEofs_RemoveForDrop(
													PersistentFileSysObjName_GetRelFileNodePtr(&pending->fsObjName),
													PersistentFileSysObjName_GetSegmentFileNum(&pending->fsObjName),
													pending->nestLevel);
			}
		}
	}

	hash_seq_init(
			&iterateStatus, 
			AppendOnlyMirrorResyncEofsTable);
	
	while ((entry = hash_seq_search(&iterateStatus)) != NULL)
	{
		if (entry->key.nestLevel >= nestLevel)
			AppendOnlyMirrorResyncEofs_Promote(
											entry,
											nestLevel - 1);
	}
}

/*
 * AtSubAbort_smgr() --- Take care of subtransaction abort.
 *
 * Delete created relations and forget about deleted relations.
 * We can execute these operations immediately because we know this
 * subtransaction will not commit.
 */
void
AtSubAbort_smgr(void)
{
	smgrSubTransAbort();
}

/*
 * AtEOXact_smgr() --- Take care of transaction end.
 *
 * For commit:
 *   1) Physically unlink any relations that were dropped.
 *   2) Change CreatePending relations to Created.
 *
 * ELSE for abort:
 *   1) Change CreatePending relations to DropPending
 *   2) Physicaly unlink the aborted creates.
 */
void
AtEOXact_smgr(bool forCommit)
{
	/*
	 * Sort the list in relation, database directory, tablespace, etc order.
	 * And, collapse same transaction create-deletes.
	 */
	if (!pendingDeletesSorted)
	{
		smgrSortDeletesList(
						&pendingDeletes, 
						&pendingDeletesCount,
						/* nestLevel */ 0);

		pendingDeletesSorted = true;
	}

	if (!pendingDeletesPerformed)
	{
		/*
		 * We need to complete this work, or let Crash Recovery complete it.
		 */
		START_CRIT_SECTION();

		/*
		 * Do abort actions for the sub-transaction's creates and deletes.
		 */
		smgrDoDeleteActions(
						&pendingDeletes, 
						&pendingDeletesCount,
						forCommit);


		Assert(pendingDeletes == NULL);
		Assert(pendingDeletesCount == 0);
		pendingDeletesSorted = false;

		/*
		 * Update the Append-Only mirror resync EOFs.
		 */
		smgrDoAppendOnlyResyncEofs(forCommit);
		
		/*
		 * Free the Append-Only mirror resync EOFs hash table.
		 */
		AppendOnlyMirrorResyncEofs_HashTableRemove("AtEOXact_smgr");

		pendingDeletesPerformed = true;

		END_CRIT_SECTION();
	}
}

/*
 *	smgrcommit() -- Prepare to commit changes made during the current
 *					transaction.
 *
 *		This is called before we actually commit.
 */
void
smgrcommit(void)
{
}

/*
 *	smgrabort() -- Clean up after transaction abort.
 */
void
smgrabort(void)
{
}

/*
 *	smgrsync() -- Sync files to disk at checkpoint time.
 */
void
smgrsync(void)
{
	mdsync();
}


void
smgr_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	int primaryError = 0;
	bool mirrorDataLossOccurred = false;

	if (info == XLOG_SMGR_CREATE)
	{
		MirrorDataLossTrackingState mirrorDataLossTrackingState;
		int64 mirrorDataLossTrackingSessionNum;

		xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(record);
		SMgrRelation reln;

		reln = smgropen(xlrec->rnode);

		mirrorDataLossTrackingState = 
					FileRepPrimary_GetMirrorDataLossTrackingSessionNum(
													&mirrorDataLossTrackingSessionNum);
		smgrcreate(
				reln,
				/* isLocalBuf */ false, 
				/* relationName */ NULL,		// Ok to be NULL -- we don't know the name here.
				mirrorDataLossTrackingState,
				mirrorDataLossTrackingSessionNum,
				/* ignoreAlreadyExists */ true,
				&primaryError,
				&mirrorDataLossOccurred);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);
		SMgrRelation reln;
		BlockNumber newblks;

		reln = smgropen(xlrec->rnode);
		
		/* Can't use smgrtruncate because it would try to xlog */

		/*
		 * First, force bufmgr to drop any buffers it has for the to-be-
		 * truncated blocks.  We must do this, else subsequent XLogReadBuffer
		 * operations will not re-extend the file properly.
		 */
		DropRelFileNodeBuffers(xlrec->rnode, false, xlrec->blkno);

		/*
		 * Tell the free space map to forget anything it may have stored for
		 * the about-to-be-deleted blocks.	We want to be sure it won't return
		 * bogus block numbers later on.
		 */
		FreeSpaceMapTruncateRel(&reln->smgr_rnode, xlrec->blkno);

		/* Do the truncation */
		newblks = mdtruncate(
						reln,
					   	xlrec->blkno,
					   	false);
		if (newblks == InvalidBlockNumber)
			ereport(WARNING,
					(errcode_for_file_access(),
			  errmsg("could not truncate relation %u/%u/%u to %u blocks: %m",
					 reln->smgr_rnode.spcNode,
					 reln->smgr_rnode.dbNode,
					 reln->smgr_rnode.relNode,
					 xlrec->blkno)));

		/* Also tell xlogutils.c about it */
		XLogTruncateRelation(xlrec->rnode, xlrec->blkno);
	}
	else
		elog(PANIC, "smgr_redo: unknown op code %u", info);
}

void
smgr_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) rec;

		appendStringInfo(buf, "file create: %u/%u/%u",
						 xlrec->rnode.spcNode, xlrec->rnode.dbNode,
						 xlrec->rnode.relNode);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) rec;

		appendStringInfo(buf, "file truncate: %u/%u/%u to %u blocks",
						 xlrec->rnode.spcNode, xlrec->rnode.dbNode,
						 xlrec->rnode.relNode, xlrec->blkno);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
