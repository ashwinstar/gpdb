/*-------------------------------------------------------------------------
 *
 * twophase.c
 *		Two-phase commit support functions.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		$Header$
 *
 * NOTES
 *		Each global transaction is associated with a global transaction 
 *		identifier (GID). A GID is assigned to a postgres transaction
 *		with the PREPARE TRANSACTION command. 
 *
 *		We keep all active global transactions in an in-memory hash table.
 *		When the PREPARE TRANSACTION command is issued, the GID is 
 *		reserved for the transaction in the hash table. This is done before
 *		an xlog entry is made, because the reservation checks for duplicate
 *		GIDs and aborts the transaction if there already is a global
 *		transaction in prepared state with the same GID.
 *
 *		When you issue a COMMIT PREPARED or ROLLBACK PREPARED command,
 *		the prepare phase must already be finished and the xlog entry must
 *		already be written. Those commands must therefore also check that
 * 		the prepare phase has finished by calling TransactionIdIsPrepared.
 *
 *		In order to survive crashes and shutdowns, all prepared
 *		transactions must be stored to permanent storage. This includes
 *		locking information, pending notifications etc. All that state
 *		information is written to the per-transaction state file.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/fd.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "miscadmin.h"

#include "access/xact.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"


/*
 * Dir is set during TwoPhaseShmemInit and does not change thereafter.
 * The value is automatically inherited by backends via fork, and
 * doesn't need to be in shared memory.
 */
char twophase_dir[MAXPGPATH];


/************************************************************************/
/* Shared memory bookkeeping                                            */
/************************************************************************/

LWLockId ControlLock;


int max_prepared_xacts = 100; /* GUC */

/* prepared transaction array */
static GXACT *gxact_array;
static int *num_prepared_xacts = 0;

/* active transaction array */
static GXACT *axact_array;
static int *num_active_xacts = 0;

/*
 * Reserves the gid for the given transaction.
 */
void
MarkAsBegun(TransactionId xid, char *gid, AclId owner) 
{
	GXACT *gxact = NULL;
	int i;

	ereport(LOG, (errmsg("MarkAsBegun: %d, %s", xid, gid)));

	LWLockAcquire(ControlLock, LW_EXCLUSIVE);

	/* find gid from active array */
	for(i=0; i < *num_active_xacts; i++) {
		if(strcmp(axact_array[i].gid, gid) == 0) {
			gxact = &axact_array[i];
			break;
		}
	}
	if ( gxact == NULL )
	{
		/* find gid from prepared array */
		for(i=0; i < *num_prepared_xacts; i++) {
			if(strcmp(gxact_array[i].gid, gid) == 0) {
				gxact = &gxact_array[i];
				break;
			}
		}
	}
	if (gxact != NULL)
	{
		LWLockRelease(ControlLock);
		ereport(ERROR, 
			(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
			 errmsg("global transaction identifier \"%s\" is in use", gid)));
	}

	if(*num_active_xacts >= max_prepared_xacts) {
		LWLockRelease(ControlLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of active global transactions reached")));
		/* TODO: proper error code and hint */
	}

	gxact = &axact_array[*num_active_xacts];
	(*num_active_xacts)++;

	TransactionIdStore(xid, &gxact->xid);
	strncpy(gxact->gid, gid, GIDSIZE);
	gxact->owner = owner;

	LWLockRelease(ControlLock);
}

/**
 * Removes the prepared transaction from our data structures.
 */
TransactionId
MarkAsNoLongerBegun(char *gid, AclId user)
{
    GXACT *gxact;
    TransactionId xid = InvalidTransactionId;
    int i;

    ereport(LOG, (errmsg("MarkAsNoLongerBegun: %s", gid)));

    LWLockAcquire(ControlLock, LW_EXCLUSIVE);

    for(i=0; i < *num_active_xacts; i++) {
        gxact = &axact_array[i];
        /* TODO: Should check TransactionIdIsInProgress, but now
         * that it calls TransactionIdIsPrepared, it's not what we want.
         * Without it, there's a small window of opportunity
         * for someone else to call COMMIT PREPARED before the transaction
         * is actually fully prepared.
         */
         if(strncmp(gxact->gid, gid, GIDSIZE) == 0 /* && !TransactionIdIsInProgress(gxact->xid) */) {
            xid = gxact->xid;

            if(user != gxact->owner && !superuser_arg(user)) 
            {
                LWLockRelease(ControlLock);
                ereport(ERROR,
                        (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                         errmsg("permission denied to finish prepared transaction"),
                         errhint("Must be superuser or the user that prepared the transaction to finish it")));
            }

            /* Move the last gxact to this slot */
            if(i != (*num_active_xacts) - 1) {
                axact_array[i] = axact_array[(*num_active_xacts) - 1];
            }
            (*num_active_xacts)--;

            break;
        }
    }

    LWLockRelease(ControlLock);
    return xid;
}

/*
 * Reserves the gid for the given transaction.
 */
void
MarkAsPrepared(TransactionId xid, char *gid, AclId owner) 
{
	GXACT *gxact = NULL;
	int i;

	ereport(LOG, (errmsg("MarkAsPrepared: %d, %s", xid, gid)));

	LWLockAcquire(ControlLock, LW_EXCLUSIVE);

	/* find gid from active array */
	for(i=0; i < *num_active_xacts; i++) {
		if(strcmp(axact_array[i].gid, gid) == 0) {
			gxact = &axact_array[i];
			break;
		}
	}
	if (gxact == NULL)
	{
		LWLockRelease(ControlLock);
		ereport(ERROR, 
			(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
			 errmsg("global transaction identifier \"%s\" is not found", gid)));
	}

/*
	for(i=0; i < *num_prepared_xacts; i++) {
		if(strcmp(gxact_array[i].gid, gid) == 0) {
			LWLockRelease(ControlLock);
			ereport(ERROR, 
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("global transaction identifier \"%s\" is already in use", gid)));
		}
	}
*/

	if(*num_prepared_xacts >= max_prepared_xacts) {
		LWLockRelease(ControlLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of prepared transactions reached")));
		/* TODO: proper error code and hint */
	}

	memcpy(&gxact_array[*num_prepared_xacts], gxact, sizeof(GXACT));

	(*num_prepared_xacts)++;
	/* Move the last gxact to this slot */
	if(i != (*num_active_xacts) - 1) {
		axact_array[i] = axact_array[(*num_active_xacts) - 1];
	}
	(*num_active_xacts)--;
	
/*
	gxact = &gxact_array[*num_prepared_xacts];
	TransactionIdStore(xid, &gxact->xid);
	strncpy(gxact->gid, gid, GIDSIZE);
	gxact->owner = owner;
*/
	gxact->owner = owner;

	LWLockRelease(ControlLock);
}

/**
 * Removes the prepared transaction from our data structures.
 */
TransactionId
MarkAsNoLongerPrepared(char *gid, AclId user)
{
	GXACT *gxact;
	TransactionId xid = InvalidTransactionId;
	int i;

	ereport(LOG, (errmsg("MarkAsNoLongerPrepared: %s", gid)));

	LWLockAcquire(ControlLock, LW_EXCLUSIVE);

	for(i=0; i < *num_prepared_xacts; i++) {
		gxact = &gxact_array[i];
		/* TODO: Should check TransactionIdIsInProgress, but now
		 * that it calls TransactionIdIsPrepared, it's not what we want.
		 * Without it, there's a small window of opportunity
		 * for someone else to call COMMIT PREPARED before the transaction
		 * is actually fully prepared.
		 */
		 if(strncmp(gxact->gid, gid, GIDSIZE) == 0 /* && !TransactionIdIsInProgress(gxact->xid) */) {
			xid = gxact->xid;

			if(user != gxact->owner && !superuser_arg(user)) 
			{
				LWLockRelease(ControlLock);
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to finish prepared transaction"),
						 errhint("Must be superuser or the user that prepared the transaction to finish it")));
			}

			/* Move the last gxact to this slot */
			if(i != (*num_prepared_xacts) - 1) {
				gxact_array[i] = gxact_array[(*num_prepared_xacts) - 1];
			}
			(*num_prepared_xacts)--;

			break;
		}
	}

	LWLockRelease(ControlLock);
	return xid;
}

/*
 * Returns an array of all prepared transactions for user-level
 * functions in pg_prepared_xact.c
 *
 * The returned array and all the elements are copies of internal data
 * structures, to minimize the time we need to hold the ControlLock.
 */
PreparedXactArray *
GetPreparedTransactionList(void)
{
	PreparedXactArray *array_copy;
	int i;

	LWLockAcquire(ControlLock, LW_SHARED);

	if(*num_prepared_xacts == 0) {
		LWLockRelease(ControlLock);
		return NULL;
	}

	array_copy = (PreparedXactArray *) palloc(SizeOfPreparedXactArray(*num_prepared_xacts));
	array_copy->nelements = *num_prepared_xacts;

	for(i=0; i < *num_prepared_xacts; i++) {
		memcpy(&array_copy->elements[i], &gxact_array[i], sizeof(GXACT));
	}

	LWLockRelease(ControlLock);

	return array_copy;
}

/*
 * Iterator routines for scanning through all currently
 * prepared transactions.
 */

static int scan_ptr;

int
StartPreparedTransactionListScan(void)
{
	scan_ptr = 0;
	LWLockAcquire(ControlLock, LW_SHARED);
	return *num_prepared_xacts;
}

TransactionId
NextPreparedTransactionId(void)
{
	if(scan_ptr >= *num_prepared_xacts)
		return InvalidTransactionId;

	return gxact_array[scan_ptr++].xid;
}


void
EndPreparedTransactionListScan(void)
{
	LWLockRelease(ControlLock);
}

/*
 * TransactionIdIsPrepared
 *		True iff transaction associated with the identifier is prepared
 *      for two-phase commit
 *
 * Note:
 *		Assumes transaction identifier is valid.
 */
bool							/* true if given transaction is prepared */
TransactionIdIsPrepared(TransactionId xid)
{
	int i;
	TransactionId topxid;

	topxid = SubTransGetTopmostTransaction(xid);

	LWLockAcquire(ControlLock, LW_SHARED);

	for(i=0; i < *num_prepared_xacts; i++) {
		if(TransactionIdEquals(gxact_array[i].xid, topxid)) {
			LWLockRelease(ControlLock);
			return true;
		}
	}

	LWLockRelease(ControlLock);
	return false;
}

void
BootStrapTwoPhase(void)
{
	/* NOP */
}

/*
 * Initialization of shared memory
 */
int
TwoPhaseShmemSize(void)
{
    return MAXALIGN( sizeof(LWLockId) + (sizeof(int) * 2) +
                    (max_prepared_xacts * sizeof(GXACT) * 2));
}


void
TwoPhaseShmemInit(void)
{
	bool found;
	struct TwoPhase_Shared {
		LWLockId ControlLock;
		int num_prepared_xacts;
		int num_active_xacts;
		struct GXACT gxact_array[max_prepared_xacts];
		struct GXACT axact_array[max_prepared_xacts];
	} *shared;

	shared = ShmemInitStruct("global transaction lookup table", TwoPhaseShmemSize(), &found);
	if(!shared)
		elog(FATAL, "could not initialize global transaction lookup table");
	
	if (!IsUnderPostmaster)
		/* Initialize locks and shared memory area */
	{
		shared->ControlLock = LWLockAssign();
	}

	ControlLock = shared->ControlLock;
	gxact_array = shared->gxact_array;
	num_prepared_xacts = &shared->num_prepared_xacts;
	axact_array = shared->axact_array;
	num_active_xacts = &shared->num_active_xacts;

	/* Init directory path */
	snprintf(twophase_dir, MAXPGPATH, "%s/pg_twophase", DataDir);
}


/************************************************************************/
/* State file support                                                   */
/************************************************************************/

#define TwoPhaseFilePath(path, xid) \
	snprintf(path, MAXPGPATH, "%s/%08X", twophase_dir, xid)

static void register_statefileblock(void *data, uint32 len);

/*
 * Header for each record in a state file
 *
 * NOTE: len counts only the rmgr data, not the TwoPhaseRecordOnDisk header.
 */
typedef struct TwoPhaseRecordOnDisk
{
	uint32		len;			/* length of rmgr data */
	TwoPhaseRmgrId		rmid;	/* resource manager for this record */

	/* ACTUAL DATA FOLLOWS AT END OF STRUCT */
	char data[0];
} TwoPhaseRecordOnDisk;


struct xllist {
  XLogRecData *head;
  XLogRecData *tail;
  int bytesFree;
} records;

static void
register_statefileblock(void *data, uint32 len)
{
	if(len > records.bytesFree) {
		records.tail->next = palloc(sizeof(XLogRecData));
		records.tail = records.tail->next;
		records.tail->buffer = InvalidBuffer;
		records.tail->next = NULL;
        records.tail->len = 0;

		records.bytesFree = len > 512 ? len : 512;
		records.tail->data = palloc(records.bytesFree);
	}
	memcpy(&((char *)records.tail->data)[records.tail->len], data, len);
	records.tail->len += len;
    records.bytesFree -= len;
}

void
StartPrepare(char *gid)
{
	TwoPhaseFileHeader hdr;
	TransactionId *children;

	/* Initialize linked list */
	records.head = palloc(sizeof(XLogRecData));
	records.head->data = palloc(512);
	records.head->len = 0;
	records.head->next = NULL;
	records.head->buffer = InvalidBuffer;
	records.bytesFree = 512;

	records.tail = records.head;

	/* Create header */

	strncpy(hdr.gid, gid, GIDSIZE);
	TransactionIdStore(GetTopTransactionId(), &hdr.xid);
	hdr.owner = GetUserId();

	hdr.nsubxacts = xactGetCommittedChildren(&children);

	register_statefileblock(&hdr, sizeof(TwoPhaseFileHeader));
	register_statefileblock(children, hdr.nsubxacts * sizeof(TransactionId));
}

void
EndPrepare(void)
{
	char		path[MAXPGPATH];
	TransactionId xid = GetTopTransactionId();
	TwoPhaseRecordOnDisk end_record;
	int written_len;
	XLogRecData *record;
	XLogRecPtr	recptr;
	static crc64 statefile_crc;
	int twophase_fd;

	/* register end sentinel */
	end_record.len = 0;
	end_record.rmid = 0;
	register_statefileblock(&end_record, sizeof(end_record));

	/*
	 * We have to record transaction prepares even if we didn't
	 * make any updates, because the transaction manager might
	 * get confused if we lose a global transaction.
	 */

	/* If we crash now, we have already prepared */

	/* Create file and initialize CRC */
	INIT_CRC64(statefile_crc);

	TwoPhaseFilePath(path, xid);

	twophase_fd = BasicOpenFile(path, O_CREAT | O_EXCL | O_WRONLY | PG_BINARY, S_IRUSR | S_IWUSR);
	if (twophase_fd < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create twophase state file \"%s\": %m",
						path)));
	}

	/* Write records */

	record = records.head;
	while(record != NULL) {
		written_len = write(twophase_fd, record->data, record->len);
		if(written_len != record->len)
		{
			close(twophase_fd);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to twophase state file: %m")));
		}

		COMP_CRC64(statefile_crc, record->data, record->len);

		record = record->next;
	}

	FIN_CRC64(statefile_crc);

	/* Insert entry to WAL */
	START_CRIT_SECTION();
	recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_PREPARE, records.head);
	XLogFlush(recptr);
	END_CRIT_SECTION();

	records.tail = records.head = NULL;

	/* write CRC and close file */

	written_len = write(twophase_fd, &statefile_crc, sizeof(statefile_crc));
	if(written_len != sizeof(statefile_crc))
	{
		close(twophase_fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to twophase state file: %m")));
	}

	close(twophase_fd);
}

/*
 * Registers a recovery record. 
 */
void
RegisterTwoPhaseRecord(TwoPhaseRmgrId rmid, void *data, uint32 len)
{
	TwoPhaseRecordOnDisk record;
	record.rmid = rmid;
	record.len = len;

	register_statefileblock(&record, sizeof(TwoPhaseRecordOnDisk));
	register_statefileblock(data, len);
}

#define BUF_SIZE 512 /* Buffer size used in ValidateTwoPhaseFile */

/*
 * Validates the state file for xid. Returns true if
 * it looks OK.
 */
static bool
ValidateTwoPhaseFile(TransactionId xid)
{
	char		path[MAXPGPATH];
	char        buf[BUF_SIZE];
	crc64 calc_crc, file_crc;
	struct stat stat;
	off_t total_read = 0;
	int fd;
	int len;

	TwoPhaseFilePath(path, xid);

	elog(DEBUG1, "ValidateTwoPhaseFile(%x)", xid);

	fd = BasicOpenFile(path, PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open twophase state file \"%s\": %m", 
						path)));
		return false;
	}

	fstat(fd, &stat);

	/* Read through the file and accumulate the CRC until 
	 * end - sizeof(crc64). */

	INIT_CRC64(calc_crc);
	for(;;) {
		int bytesleft = stat.st_size - total_read - sizeof(crc64);

		len = read(fd, buf, bytesleft < BUF_SIZE ? bytesleft : BUF_SIZE);
		total_read += len;
		if(len == -1) {
			close(fd);
			return false;
		}
		if(len == 0) {
			break;
		}
		COMP_CRC64(calc_crc, buf, len);
	}
	FIN_CRC64(calc_crc);

	len = read(fd, &file_crc, sizeof(crc64));
	close(fd);

	if(len != sizeof(crc64)) {
		return false;
	}

	return EQ_CRC64(calc_crc, file_crc);
}

static void
read_with_ereport(int fd, void *data, int len)
{
	int read_len;

	read_len = read(fd, data, len);

	if(read_len != len)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("error reading twophase state file: %m")));
	}
}

/**
 * File format
 *
 *  1. TwoPhaseFileHeader
 *  2. TransactionId[] (subtransactions)
 *  3. CommitAbortRecord
 *  4. ...
 *  5. RecoverRecord
 *  6. ...
 *  7. EndRecord
 *  8. CRC64
 */

int
OpenTwoPhaseFile(TransactionId xid, TwoPhaseFileHeader *hdr, TransactionId **children)
{
	char		path[MAXPGPATH];
	int len;
	int fd;

	TwoPhaseFilePath(path, xid);

	elog(DEBUG1, "OpenTwoPhaseFile (%x)", xid);

	fd = BasicOpenFile(path, PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open twophase state file \"%s\": %m", 
						path)));
	}

	read_with_ereport(fd, hdr, sizeof(TwoPhaseFileHeader));

	Assert(TransactionIdEquals(hdr->xid, xid));

	len = hdr->nsubxacts * sizeof(TransactionId);

	if(children != NULL) {
		*children = palloc(len);

		read_with_ereport(fd, *children, len);
	} else {
		lseek(fd, len, SEEK_CUR);
	}
	return fd;
}

/**
 * Reads the state file for the transaction. Calls the
 * callbacks for each record in the file.
 */
void
ProcessRecords(int fd, TransactionId xid, const TwoPhaseCallback callbacks[])
{
	TwoPhaseRecordOnDisk *record;

	elog(DEBUG1, "ProcessRecords (%x)", xid);

	record = palloc(sizeof(TwoPhaseRecordOnDisk));
	for(;;) {
		/* Read record header */
		read_with_ereport(fd, record, sizeof(TwoPhaseRecordOnDisk));

		Assert(record->rmid <= TWOPHASE_RM_MAX_ID);

		/* Watch for end sentinel */
		if(record->rmid == TWOPHASE_RM_END_ID) {
			break;
		}

		/* Read resource manager specific data and call the hook */
		record = repalloc(record, sizeof(TwoPhaseRecordOnDisk) + record->len);
		read_with_ereport(fd, record->data, record->len);

		if(callbacks[record->rmid] != NULL)
			callbacks[record->rmid](xid, record->data, record->len);
	}
	pfree(record);
}

void RemoveTwoPhaseFile(TransactionId xid)
{
	char path[MAXPGPATH];
	TwoPhaseFilePath(path, xid);

	if(unlink(path))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove two-phase state file \"%s\": %m", 
						path)));
}

void 
RecreateTwoPhaseFile(TransactionId xid, void *content, int len)
{
	char		path[MAXPGPATH];
	int written_len;
	crc64 statefile_crc;
	int fd;

	TwoPhaseFilePath(path, xid);

	fd = BasicOpenFile(path, O_CREAT | O_WRONLY | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not recreate twophase state file \"%s\": %m",
						path)));
	}

	/* Write content */
	INIT_CRC64(statefile_crc);
	COMP_CRC64(statefile_crc, content, len);
	FIN_CRC64(statefile_crc);

	written_len = write(fd, content, len);
	if(written_len != len)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to twophase state file: %m")));
	}

	written_len = write(fd, &statefile_crc, sizeof(statefile_crc));
	if(written_len != sizeof(statefile_crc))
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to twophase state file: %m")));
	}

	close(fd);
}

/*
 * Scans the pg_twophase directory and recovers every found 
 * prepared transaction
 */
void
RecoverPreparedTransactions(void)
{
	DIR                *cldir;
	struct dirent *clde;

	elog(LOG, "Recovering prepared transactions");

	cldir = opendir(twophase_dir);
	if (cldir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", twophase_dir)));
	
	errno = 0;
    
	while ((clde = readdir(cldir)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			TwoPhaseFileHeader hdr;
			TransactionId *subxids;
			int fd;
			int i;
			sscanf(clde->d_name, "%X", &xid);

			if(TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
			{	
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("removing stale twophase state file \"%s\": %m", 
								clde->d_name)));
				RemoveTwoPhaseFile(xid);
				continue;
			}

			if(!ValidateTwoPhaseFile(xid))
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("removing corrupt twophase state file \"%s\": %m", 
								clde->d_name)));

				RemoveTwoPhaseFile(xid);
				continue;
			}

			fd = OpenTwoPhaseFile(xid, &hdr, &subxids);

			for(i = 0; i < hdr.nsubxacts; i++) {
				ereport(LOG, (errmsg("Subxid %d", subxids[i])));
				SubTransSetParent(subxids[i], xid);
			}

			pfree(subxids);

			MarkAsBegun(xid, hdr.gid, hdr.owner);
			MarkAsPrepared(xid, hdr.gid, hdr.owner);
			ProcessRecords(fd, xid, recover_callbacks);
			close(fd);
		}
	}
	if (errno)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m", 
						twophase_dir)));

	closedir(cldir);
}


void
CheckPointTwoPhase(void)
{
	int n;
	int i;
	int fd;
	int ret;
	
	n = StartPreparedTransactionListScan();

	{
		TransactionId xids[n];

		for(i=0; i < n; i++) {
			xids[i] = NextPreparedTransactionId();
		}
		EndPreparedTransactionListScan();

		for(i=0; i < n; i++) {
			char		path[MAXPGPATH];

			TwoPhaseFilePath(path, xids[i]);

			fd = BasicOpenFile(path, PG_BINARY, S_IRUSR | S_IWUSR);
			if (fd < 0)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open twophase state file \"%s\": %m",
								path)));
			}
			ret = fsync(fd);
			if (ret != 0)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open fsync twophase state file \"%s\": %m",
								path)));
			}

			close(fd);
 		}
	}
}

