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

#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "commands/tablespace.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "access/persistentfilesysobjname.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/guc.h"

#include "catalog/pg_tablespace.h"
#include "catalog/pg_resqueue.h"
#include "catalog/indexing.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <glob.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

/*
 * This struct of function pointers defines the API between smgr.c and
 * any individual storage manager module.  Note that smgr subfunctions are
 * generally expected to report problems via elog(ERROR).  An exception is
 * that smgr_unlink should use elog(WARNING), rather than erroring out,
 * because we normally unlink relations during post-commit/abort cleanup,
 * and so it's too late to raise an error.  Also, various conditions that
 * would normally be errors should be allowed during bootstrap and/or WAL
 * recovery --- see comments in md.c for details.
 */
typedef struct f_smgr
{
	bool		(*smgr_init) (void);	/* may be NULL */
	bool		(*smgr_shutdown) (void);		/* may be NULL */
	bool		(*smgr_close) (SMgrRelation reln);
	bool		(*smgr_createtablespacedir) (TablespaceDirNode *tablespaceDirNode, bool isRedo);
	bool		(*smgr_createdbdir) (DbDirNode *dbDirNode, bool isRedo);
	bool		(*smgr_create) (SMgrRelation reln, bool isRedo);
	bool		(*smgr_unlink) (RelFileNode rnode, bool isRedo);
	bool		(*smgr_rmtablespacedir) (TablespaceDirNode *tablespaceDirNode, bool isRedo);
	bool		(*smgr_rmdbdir) (DbDirNode *dbDirNode, bool isRedo);
	bool		(*smgr_extend) (SMgrRelation reln, BlockNumber blocknum,
											char *buffer, bool isTemp);
	bool		(*smgr_read) (SMgrRelation reln, BlockNumber blocknum,
										  char *buffer);
	bool		(*smgr_write) (SMgrRelation reln, BlockNumber blocknum,
										   char *buffer, bool isTemp);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln);
	BlockNumber (*smgr_truncate) (SMgrRelation reln, BlockNumber nblocks,
											  bool isTemp);
	bool		(*smgr_immedsync) (SMgrRelation reln);
	bool		(*smgr_commit) (void);	/* may be NULL */
	bool		(*smgr_abort) (void);	/* may be NULL */
	bool		(*smgr_sync) (void);	/* may be NULL */
} f_smgr;


static const f_smgr smgrsw[] = {
	/* magnetic disk */
	{mdinit, NULL, mdclose, mdcreatetablespacedir, mdcreatedbdir, mdcreate, mdunlink, mdrmtablespacedir, mdrmdbdir, mdextend,
		mdread, mdwrite, mdnblocks, mdtruncate, mdimmedsync,
		NULL, NULL, mdsync
	}
};

static const int NSmgr = lengthof(smgrsw);


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

	int			which;			/* which storage manager? */
	bool		isLocalBuf;	    /* CDB: true => uses local buffer mgr */
	bool		atCommit;		/* T=delete at commit; F=delete at abort */
	bool		sameTransCreateDrop; /* Collapsed create-delete? */
	ItemPointerData persistentTid;
	int64		persistentSerialNum;
	int			nestLevel;		/* xact nesting level of request */
	struct PendingDelete *next;		/* linked-list link */
} PendingDelete;

static PendingDelete *pendingDeletes = NULL; /* head of linked list */
static int pendingDeletesCount = 0;
static bool pendingDeletesSorted = false;

typedef PendingDelete *PendingDeletePtr;

/*
 * Declarations for smgr-related XLOG records
 *
 * Note: we log file creation and truncation here, but logging of deletion
 * actions is handled by xact.c, because it is part of transaction commit.
 */

/* XLOG gives us high 4 bits */
#define XLOG_SMGR_CREATE	0x10
#define XLOG_SMGR_TRUNCATE	0x20

typedef struct xl_smgr_create
{
	RelFileNode rnode;
} xl_smgr_create;

typedef struct xl_smgr_truncate
{
	BlockNumber blkno;
	RelFileNode rnode;
} xl_smgr_truncate;


/* local function prototypes */
static void smgrshutdown(int code, Datum arg);
static void smgr_internal_unlink(RelFileNode rnode, int which,
					 bool isLocalBuf, bool isRedo);


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
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
		{
			if (!(*(smgrsw[i].smgr_init)) ())
				elog(FATAL, "smgr initialization failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * on_proc_exit hook for smgr cleanup during backend shutdown
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
		{
			if (!(*(smgrsw[i].smgr_shutdown)) ())
				elog(FATAL, "smgr shutdown failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
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

	if (!(*(smgrsw[reln->smgr_which].smgr_close)) (reln))
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
 *	smgrcreatetablespacedir() -- Create a new tablespace directory.
 *
 */
void
smgrcreatetablespacedir(TablespaceDirNode *tablespaceDirNode, bool isRedo)
{
	PendingDelete *pending; 
	
	if (!isRedo)
	{
		/* Add the relation to the list of stuff to delete at abort */
		pending = (PendingDelete *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(PendingDelete));
		
		PersistentFileSysObjName_SetTablespaceDir(
									&pending->fsObjName,
									tablespaceDirNode->filespace,
									tablespaceDirNode->tablespace);

		pending->which = 0;  // We only have 1 today.
		pending->isLocalBuf = false;
		pending->atCommit = false;	/* delete if abort */
		pending->sameTransCreateDrop = false;
		pending->nestLevel = GetCurrentTransactionNestLevel();
		pending->next = pendingDeletes;
		pendingDeletes = pending;
		pendingDeletesCount++;
		pendingDeletesSorted = false;
		
		// UNDONE: For now, ~_MarkCreatePending will do the XLOG Insert and XLOG Flush
		// UNDONE: of the PendingCreate 
		// UNDONE: We'll improve performance later by moving the XLOG Insert sooner...
		// UNDONE: and flushing later...
		PersistentTablespace_MarkCreatePending(
									tablespaceDirNode,
									&pending->persistentTid,
									/* flushToXLog */ true);

	}
	
	if (!(*(smgrsw[0].smgr_createtablespacedir)) (tablespaceDirNode, isRedo))
	{
	    ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create tablespace directory %u, filespace %u: %m",
					tablespaceDirNode->tablespace,
					tablespaceDirNode->filespace)));
	}
}

/*
 *	smgrcreatedbdir() -- Create a new database directory.
 *
 */
void
smgrcreatedbdir(DbDirNode *dbDirNode, bool isRedo)
{
	PendingDelete *pending; 
	Oid tablespace;
	Oid database;

	tablespace = dbDirNode->tablespace;
	database = dbDirNode->database;
	
	if (!isRedo)
	{
		/* Add the relation to the list of stuff to delete at abort */
		pending = (PendingDelete *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(PendingDelete));
		
		PersistentFileSysObjName_SetDatabaseDir(&pending->fsObjName,tablespace,database);

		pending->which = 0;  // We only have 1 today.
		pending->isLocalBuf = false;
		pending->atCommit = false;	/* delete if abort */
		pending->sameTransCreateDrop = false;
		pending->nestLevel = GetCurrentTransactionNestLevel();
		pending->next = pendingDeletes;
		pendingDeletes = pending;
		pendingDeletesCount++;
		pendingDeletesSorted = false;
		
		// UNDONE: For now, ~_MarkCreatePending will do the XLOG Insert and XLOG Flush
		// UNDONE: of the PendingCreate 
		// UNDONE: We'll improve performance later by moving the XLOG Insert sooner...
		// UNDONE: and flushing later...
		PersistentDatabase_MarkCreatePending(
									&pending->fsObjName.variant.dbDirNode,
									&pending->persistentTid,
									/* flushToXLog */ true);

	}
	
	if (!(*(smgrsw[0].smgr_createdbdir)) (dbDirNode, isRedo))
	{
	    ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create database directory %u/%u: %m",
					tablespace,
					database)));
	}
}


void smgrcreatepending(
	RelFileNode		relFileNode, 
	ItemPointer		persistentTid,
	int64			*persistentSerialNum,
	bool			isLocalBuf,
	bool			flushToXLog)
{
	PendingDelete *pending;

	PersistentRelation_AddCreatePending(
								&relFileNode,
								persistentTid,
								persistentSerialNum,
								flushToXLog);
	
	/* Add the relation to the list of stuff to delete at abort */
	pending = (PendingDelete *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(PendingDelete));
	
	PersistentFileSysObjName_SetRelationFile(&pending->fsObjName,&relFileNode);
	
	pending->which = 0;  // We only have 1 today.
	pending->isLocalBuf = isLocalBuf;	/*CDB*/
	pending->atCommit = false;	/* delete if abort */
	pending->sameTransCreateDrop = false;
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->persistentTid = *persistentTid;
	pending->persistentSerialNum = *persistentSerialNum;
	pending->next = pendingDeletes;
	pendingDeletes = pending;
	pendingDeletesCount++;
	pendingDeletesSorted = false;
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		Given an already-created (but presumably unused) SMgrRelation,
 *		cause the underlying disk file or other storage to be created.
 *
 *		If isRedo is true, it is okay for the underlying file to exist
 *		already because we are in a WAL replay sequence.  In this case
 *		we should make no PendingDelete entry; the WAL sequence will
 *		tell whether to drop the file.
 */
void
smgrcreate(
	SMgrRelation reln, 
	ItemPointer persistentTid, 
	int64 *persistentSerialNum,
	bool isLocalBuf, 
	bool isRedo)
{
	/*
	 * We may be using the target table space for the first time in this
	 * database, so create a per-database subdirectory if needed.
	 *
	 * XXX this is a fairly ugly violation of module layering, but this seems
	 * to be the best place to put the check.  Maybe TablespaceCreateDbspace
	 * should be here and not in commands/tablespace.c?  But that would imply
	 * importing a lot of stuff that smgr.c oughtn't know, either.
	 */
	TablespaceCreateDbspace(reln->smgr_rnode.spcNode,
							reln->smgr_rnode.dbNode,
							isRedo);


	if (!isRedo)
	{
		XLogRecPtr	lsn;
		XLogRecData rdata;
		xl_smgr_create xlrec;
	
		// UNDONE: When PersistentFileSysObj is fully up, we can get rid of this XLOG record.
		/*
		 * Make a non-transactional XLOG entry showing the file creation. It's
		 * non-transactional because we should replay it whether the transaction
		 * commits or not; if not, the file will be dropped at abort time.
		 */
		xlrec.rnode = reln->smgr_rnode;

		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		lsn = XLogInsert(RM_SMGR_ID, XLOG_SMGR_CREATE | XLOG_NO_TRAN, &rdata);

		// UNDONE: For now, ~_MarkCreatePending will do the XLOG Insert and XLOG Flush
		// UNDONE: of the PendingCreate 
		// UNDONE: We'll improve performance later by moving the XLOG Insert sooner...
		// UNDONE: and flushing later...
		smgrcreatepending(
					reln->smgr_rnode, 
					persistentTid,
					persistentSerialNum,
					isLocalBuf,
					/* flushToXLog */ true);
	}
	
	if (!(*(smgrsw[reln->smgr_which].smgr_create)) (reln, isRedo))
	{
	    ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create relation %u/%u/%u: %m",
					reln->smgr_rnode.spcNode,
					reln->smgr_rnode.dbNode,
					reln->smgr_rnode.relNode)));
	}
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
	SMgrRelation reln,
	ItemPointer persistentTid,
	int64 persistentSerialNum,
	bool isLocalBuf)
{
	PendingDelete *pending;

	/* Add the relation to the list of stuff to delete at commit */
	pending = (PendingDelete *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(PendingDelete));

	PersistentFileSysObjName_SetRelationFile(&pending->fsObjName,&reln->smgr_rnode);

	pending->which = reln->smgr_which;
	pending->isLocalBuf = isLocalBuf;   /*CDB*/
	pending->atCommit = true;	/* delete if commit */
	pending->sameTransCreateDrop = false;
	pending->persistentTid = *persistentTid;
	pending->persistentSerialNum = persistentSerialNum;
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;
	pendingDeletesCount++;
	pendingDeletesSorted = false;

	/* Now close the file and throw away the hashtable entry */
	smgrclose(reln);
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
smgrdounlink(SMgrRelation reln, bool isLocalBuf, bool isRedo)
{
	RelFileNode rnode = reln->smgr_rnode;
	int			which = reln->smgr_which;

	/* Close the file and throw away the hashtable entry */
	smgrclose(reln);

	smgr_internal_unlink(rnode, which, isLocalBuf, isRedo);
}

/*
 * Shared subroutine that actually does the unlink ...
 */
static void
smgr_internal_unlink(RelFileNode rnode, int which, bool isLocalBuf, bool isRedo)
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
	if (!(*(smgrsw[which].smgr_unlink)) (rnode, isRedo))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove relation %u/%u/%u: %m",
						rnode.spcNode,
						rnode.dbNode,
						rnode.relNode)));
}

/*
 *	smgrschedulermtablespacedir() -- Schedule removing a tablespace directory at xact commit.
 *
 *		The tablespace directory is marked to be removed from the store if we
 *		successfully commit the current transaction.
 */
void
smgrschedulermtablespacedir(TablespaceDirNode *tablespaceDirNode)
{
	PendingDelete *pending;

	/* Add the database directory to the list of stuff to delete at commit */
	pending = (PendingDelete *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(PendingDelete));
	
	PersistentFileSysObjName_SetTablespaceDir(
									&pending->fsObjName,
									tablespaceDirNode->filespace,
									tablespaceDirNode->tablespace);
	
	pending->which = 0;  // We only have 1 today.
	pending->isLocalBuf = false;
	pending->atCommit = true;	/* delete if commit */
	pending->sameTransCreateDrop = false;
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;
	pendingDeletesCount++;
	pendingDeletesSorted = false;
}

/*
 *	smgrschedulermdbdir() -- Schedule removing a DB directory at xact commit.
 *
 *		The database directory is marked to be removed from the store if we
 *		successfully commit the current transaction.
 */
void
smgrschedulermdbdir(DbDirNode *dropDbDirNode)
{
	PendingDelete *pending;
	Oid tablespace;
	Oid database;

	/* Add the database directory to the list of stuff to delete at commit */
	pending = (PendingDelete *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(PendingDelete));

	tablespace = dropDbDirNode->tablespace;
	database = dropDbDirNode->database;
	
	PersistentFileSysObjName_SetDatabaseDir(&pending->fsObjName,tablespace,database);
	
	pending->which = 0;  // We only have 1 today.
	pending->isLocalBuf = false;
	pending->atCommit = true;	/* delete if commit */
	pending->sameTransCreateDrop = false;
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;
	pendingDeletesCount++;
	pendingDeletesSorted = false;
}

/*
 * Shared subroutine that actually does the rmdir of a database directory ...
 */
static void
smgr_internal_rmtablespacedir(TablespaceDirNode *tablespaceDirNode, int which, bool isRedo)
{
	/*
	 * And remove the physical tablespace directory.
	 *
	 * Note: we treat deletion failure as a WARNING, not an error, because
	 * we've already decided to commit or abort the current xact.
	 */
	if (!(*(smgrsw[which].smgr_rmtablespacedir)) (tablespaceDirNode, isRedo))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove tablespace directory %u, filespace %u: %m",
						tablespaceDirNode->tablespace,
						tablespaceDirNode->filespace)));
}
void
smgrdormtablespacedir(TablespaceDirNode *tablespaceDirNode, bool isRedo)
{
	smgr_internal_rmtablespacedir(tablespaceDirNode, 0, isRedo);
}

/*
 * Shared subroutine that actually does the rmdir of a database directory ...
 */
static void
smgr_internal_rmdbdir(DbDirNode *dropDbDirNode, int which, bool isRedo)
{
	/*
	 * And remove the physical database directory.
	 *
	 * Note: we treat deletion failure as a WARNING, not an error, because
	 * we've already decided to commit or abort the current xact.
	 */
	if (!(*(smgrsw[which].smgr_rmdbdir)) (dropDbDirNode, isRedo))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove database directory %u/%u: %m",
						dropDbDirNode->tablespace,
						dropDbDirNode->database)));
}

void
smgrdormdbdir(DbDirNode *dropDbDirNode, bool isRedo)
{
	smgr_internal_rmdbdir(dropDbDirNode, 0, isRedo);
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
	if (!(*(smgrsw[reln->smgr_which].smgr_extend)) (reln, blocknum, buffer,
													isTemp))
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
	if (!(*(smgrsw[reln->smgr_which].smgr_read)) (reln, blocknum, buffer))
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
	if (!(*(smgrsw[reln->smgr_which].smgr_write)) (reln, blocknum, buffer,
												   isTemp))
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

	nblocks = (*(smgrsw[reln->smgr_which].smgr_nblocks)) (reln);

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
smgrtruncate(SMgrRelation reln, BlockNumber nblocks, bool isTemp, bool isLocalBuf)
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
	newblks = (*(smgrsw[reln->smgr_which].smgr_truncate)) (reln, nblocks,
														   isTemp);
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

		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		lsn = XLogInsert(RM_SMGR_ID, XLOG_SMGR_TRUNCATE | XLOG_NO_TRAN,
						 &rdata);
	}

	return newblks;
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
	if (!(*(smgrsw[reln->smgr_which].smgr_immedsync)) (reln))
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
	pfree(*ele);

	*ele = NULL;
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
}


static void
smgrDoDeleteActions(
	PendingDelete 	**list,
	int					*listCount,
	bool				forCommit)
{
	PendingDelete *current;
	int entryIndex;
	bool doDelete;

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

		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Storage Manager: Do delete action for list entry #%d: '%s' (forCommit %s, transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ", sameTransCreateDrop %s)",
				 entryIndex,
				 PersistentFileSysObjName_TypeAndObjectName(&current->fsObjName),
				 (forCommit ? "true" : "false"),
				 current->nestLevel,
				 ItemPointerToString(&current->persistentTid),
				 current->persistentSerialNum,
				 (current->sameTransCreateDrop ? "true" : "false"));

		doDelete = false;	// Assume.
		if (forCommit)
		{
			/*
			 * Do commit processing.
			 */
			if (current->atCommit)
			{
				/*
				 * DROP on commit.
				 */
				PersistentFileSysObj_MarkDropPending(
								&current->fsObjName,
								&current->persistentTid,
								current->persistentSerialNum);

				doDelete = true;
			}
			else
			{
				PersistentFileSysObj_Created(
								&current->fsObjName,
								&current->persistentTid,
								current->persistentSerialNum);
			}
		}
		else
		{
			/*
			 * Do abort processing.
			 */
			if (!current->atCommit || current->sameTransCreateDrop)
			{
				/*
				 * Abort the CREATE, or 
				 * delete the same transaction CREATE-DROP.
				 */
				PersistentFileSysObj_MarkAbortingCreate(
								&current->fsObjName,
								&current->persistentTid,
								current->persistentSerialNum);

				doDelete = true;
			}
		}

		if (doDelete)
		{
			/*
			 * UNDONE: Need to guard this for failure and continue to mark items
			 * UNDONE: drop pending...
			 * UNDONE: And handle other file-system object types...
			 */
			PersistentFileSysObj_Drop(
							&current->fsObjName,
							&current->persistentTid,
							current->persistentSerialNum,
							/* isRedo */ false);
		}

		/* must explicitly free the list entry */
		PendingDelete_Free(&current);

		entryIndex++;

		
	}

	Assert(*listCount == 0);
	Assert(*list == NULL);
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
		if (entry1->atCommit == entry2->atCommit)
			return 0;
		else if (entry1->atCommit)
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
			if (prev->atCommit)
				elog(ERROR, "Expected a CREATE for file-system object name '%s'",
					PersistentFileSysObjName_ObjectName(&prev->fsObjName));
			if (!current->atCommit)
				elog(ERROR, "Expected a DROP for file-system object name '%s'",
					PersistentFileSysObjName_ObjectName(&current->fsObjName));

			prev->atCommit = true;				// Make the CREATE a DROP.
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



	// UNDONE: Put this under DEBUG ifdef
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
smgrGetPendingDeletes(bool forCommit, PersistentEndXactFsObj **ptr)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	int			nrels;
	PersistentEndXactFsObj *rptr;
	PendingDelete *pending;
	int			entryIndex;

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
		if (pending->nestLevel >= nestLevel && 
			pending->atCommit == forCommit)
			nrels++;
	}
	if (nrels == 0)
	{
		*ptr = NULL;
		return 0;
	}

	if (Debug_persistent_print)
		elog(Debug_persistent_print_level, 
			 "Storage Manager: Get list entries (forCommit %s, current transaction nest level %d",
			 (forCommit ? "true" : "false"),
			 nestLevel);

	rptr = (PersistentEndXactFsObj *) 
							palloc(nrels * sizeof(PersistentEndXactFsObj));
	*ptr = rptr;
	entryIndex = 0;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		bool returned;

		returned = false;
		if (pending->nestLevel >= nestLevel && 
			pending->atCommit == forCommit)
		{
			rptr->fsObjName = pending->fsObjName;
			rptr->persistentTid = pending->persistentTid;
			rptr->persistentSerialNum = pending->persistentSerialNum;

			rptr++;
			returned = true;
		}

		if (Debug_persistent_print)
			elog(Debug_persistent_print_level, 
				 "Storage Manager: Get list entry #%d: '%s' (forCommit %s, returned %s, transaction nest level %d, persistent TID %s, persistent serial number " INT64_FORMAT ", sameTransCreateDrop %s)",
				 entryIndex,
				 PersistentFileSysObjName_TypeAndObjectName(&pending->fsObjName),
				 (forCommit ? "true" : "false"),
				 (returned ? "true" : "false"),
				 pending->nestLevel,
				 ItemPointerToString(&pending->persistentTid),
				 pending->persistentSerialNum,
				 (pending->sameTransCreateDrop ? "true" : "false"));
		entryIndex++;
	}
	return nrels;
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

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit)
			return true;
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

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel)
			pending->nestLevel = nestLevel - 1;
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
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_commit)
		{
			if (!(*(smgrsw[i].smgr_commit)) ())
				elog(ERROR, "transaction commit failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}

/*
 *	smgrabort() -- Clean up after transaction abort.
 */
void
smgrabort(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_abort)
		{
			if (!(*(smgrsw[i].smgr_abort)) ())
				elog(ERROR, "transaction abort failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}

/*
 *	smgrsync() -- Sync files to disk at checkpoint time.
 */
void
smgrsync(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_sync)
		{
			if (!(*(smgrsw[i].smgr_sync)) ())
				elog(ERROR, "storage sync failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}


void
smgr_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	ItemPointerData persistentTid;

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(record);
		SMgrRelation reln;

		reln = smgropen(xlrec->rnode);
		// UNDONE: What about the persistent rel files table???
		smgrcreate(reln, &persistentTid, 0, false, true);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);
		SMgrRelation reln;
		BlockNumber newblks;

		reln = smgropen(xlrec->rnode);

		/*
		 * Forcibly create relation if it doesn't exist (which suggests that
		 * it was dropped somewhere later in the WAL sequence).  As in
		 * XLogOpenRelation, we prefer to recreate the rel and replay the
		 * log as best we can until the drop is seen.
		 */
		// UNDONE: What about the persistent rel files table???
		smgrcreate(reln, &persistentTid, 0, false, true);

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
		newblks = (*(smgrsw[reln->smgr_which].smgr_truncate)) (reln,
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
