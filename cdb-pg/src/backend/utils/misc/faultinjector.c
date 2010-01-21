/*
 *  faultinjector.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved. *
 */

/* 
 * GP Fault Injector utility (gpfaultinjector python script) is used 
 * for Greenplum internal testing only. 
 * 
 * The utility inject faults (as defined by 'fault_type') on primary or mirror segment 
 *	at predefined 'fault_name. 
 * 
 * The utility is started on master host. Master host sends the fault injection request to specified segment. 
 *		•	It connects to postmaster on a segment. 
 *		•	Postmaster spawns backend process that sets fault injection request into shared memory. 
 *			Shared memory is accessible to all segment processes.
 *		•	Segment processes are checking shared memory to find if/when fault has to be injected.
 *
 */

#include "postgres.h"

#include <signal.h>

#include "cdb/cdbfilerep.h"
#include "cdb/cdbfilerepservice.h"
#include "storage/spin.h"
#include "storage/shmem.h"
#include "utils/faultinjector.h"
#include "utils/hsearch.h"

#ifdef FAULT_INJECTOR

typedef struct FaultInjectorShmem_s {
	slock_t		lock;
	
	int			faultInjectorSlots;	
		/* number of fault injection set */
	
	HTAB		*hash;
} FaultInjectorShmem_s;

static	FaultInjectorShmem_s *faultInjectorShmem = NULL;

static void LockAcquire(void);
	
static void LockRelease(void);

static FaultInjectorEntry_s* FaultInjector_LookupHashEntry(
								FaultInjectorIdentifier_e identifier);

static FaultInjectorEntry_s* FaultInjector_InsertHashEntry(
								FaultInjectorIdentifier_e identifier, 
								bool	*exists);

static int FaultInjector_NewHashEntry(
								FaultInjectorEntry_s	*entry);

static int FaultInjector_UpdateHashEntry(
								FaultInjectorEntry_s	*entry);

static bool FaultInjector_RemoveHashEntry(
								FaultInjectorIdentifier_e identifier);

const char*
FaultInjectorTypeEnumToString[] = {
	"", /* not specified */
	"sleep",
	"fault",
	"fatal",
	"panic",
	"error",
	"infinite_loop",
	"data_corruption",
	"reset",
	"status",
	"not recognized",
};

const char*
FaultInjectorIdentifierEnumToString[] = {
	"",		/* not specified */
	"filerep_consumer",
		/* 
		 * inject fault before data are processed
		 *		*) file operation is issued to file system (if mirror)
		 *		*) file operation performed on mirror is acknowledged to backend processes (if primary)
		 */
	"filerep_sender",
		/* inject fault before data are sent to network */
	"filerep_receiver",
		/* 
		 * inject fault after data are received from the network and 
		 * before data are made available for consuming 
		 */
	"filerep_flush",
		/* inject fault before fsync is issued to file system */
	"not recognized",
};

const char*
FaultInjectorDDLEnumToString[] = {
	"",		/* not specified */
	"create_database",
	"drop_database",
	"create_table",
	"alter_table",
	"drop_table",
	"create_index",
	"alter_index",
	"reindex",
	"drop_index",
	"create_filespaces",
	"drop_filespaces",
	"create_tablespaces",
	"drop_tablespaces",
	"truncate",
	"vacuum",
	"not recognized",
};

const char*
FaultInjectorStateEnumToString[] = {
	"not initialized",
	"waiting",
	"completed",
	"failed",
};

/*
 *
 */
FaultInjectorType_e
FaultInjectorTypeStringToEnum(
							  char*		faultTypeString)
{
	FaultInjectorType_e	faultTypeEnum = FaultInjectorTypeMax;
	int	ii;
	
	for (ii=0; ii < FaultInjectorTypeMax; ii++) {
		if (strcmp(FaultInjectorTypeEnumToString[ii], faultTypeString) == 0) {
			faultTypeEnum = ii;
			break;
		}
	}
	return faultTypeEnum;
}

/*
 *
 */
FaultInjectorIdentifier_e
FaultInjectorIdentifierStringToEnum(
									char*	faultName)
{
	FaultInjectorIdentifier_e	faultId = FaultInjectorIdMax;
	int	ii;
	
	for (ii=0; ii < FaultInjectorIdMax; ii++) {
		if (strcmp(FaultInjectorIdentifierEnumToString[ii], faultName) == 0) {
			faultId = ii;
			break;
		}
	}
	return faultId;
}

/*
 *
 */
DDLStatement_e
FaultInjectorDDLStringToEnum(
									char*	ddlString)
{
	DDLStatement_e	ddlEnum = DDLMax;
	int	ii;
	
	for (ii=0; ii < DDLMax; ii++) {
		if (strcmp(FaultInjectorDDLEnumToString[ii], ddlString) == 0) {
			ddlEnum = ii;
			break;
		}
	}
	return ddlEnum;
}

static void
LockAcquire(void) 
{	
	SpinLockAcquire(&faultInjectorShmem->lock);
}

static void
LockRelease(void) 
{	
	SpinLockRelease(&faultInjectorShmem->lock);
}

/****************************************************************
 * FAULT INJECTOR routines
 ****************************************************************/
Size
FaultInjector_ShmemSize(void)
{
	Size	size;
	
	size = hash_estimate_size(
							  (Size)FAULTINJECTOR_MAX_SLOTS, 
							  sizeof(FaultInjectorEntry_s));
	
	size = add_size(size, sizeof(slock_t));
	
	return size;	
}

/*
 * Hash table contains fault injection that are set on the system waiting to be injected.
 * FaultInjector identifier is the key in the hash table.
 * Hash table in shared memory is initialized only on primary and mirror segment. 
 * It is not initialized on master host.
 */
void
FaultInjector_ShmemInit(void)
{
	HASHCTL	hash_ctl;
	
	faultInjectorShmem = (FaultInjectorShmem_s *) ShmemAlloc(sizeof(FaultInjectorShmem_s));
	
	SpinLockInit(&faultInjectorShmem->lock);
	
	faultInjectorShmem->faultInjectorSlots = 0;
	
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(int32);
	hash_ctl.entrysize = sizeof(FaultInjectorEntry_s);
	hash_ctl.hash = int32_hash;
	
	faultInjectorShmem->hash = ShmemInitHash("Fault Injector Hash",
								   FAULTINJECTOR_MAX_SLOTS,
								   FAULTINJECTOR_MAX_SLOTS,
								   &hash_ctl,
								   HASH_ELEM | HASH_FUNCTION);
	
	if (faultInjectorShmem->hash == NULL) {
		ereport(ERROR, 
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 (errmsg("Not enough shared memory for Fault Injector."))));
	}
	
	return;						  
}

void
FaultInjector_InjectFaultIfSet(
							   FaultInjectorIdentifier_e identifier,
							   DDLStatement_e			 ddlStatement,
							   char*					 databaseName,
							   char*					 tableName)
{
	
	FaultInjectorEntry_s	*entryLocal;
	char					databaseNameLocal[NAMEDATALEN];
	char					tableNameLocal[NAMEDATALEN];
	int						ii = 0;
	
	LockAcquire();
	
	entryLocal = FaultInjector_LookupHashEntry(identifier);
	
	LockRelease();
	
	/* Verify if fault injection is set */
	
	if (entryLocal == NULL) 
		/* fault injection is not set */
		return;
	
	if (entryLocal->ddlStatement != ddlStatement) 
		/* fault injection is not set for the specified DDL */
		return;
	
	snprintf(databaseNameLocal, sizeof(databaseNameLocal), "%s", databaseName);
	
	if (strcmp(entryLocal->databaseName, databaseNameLocal) != 0) 
		/* fault injection is not set for the specified database name */
		return;
	
	snprintf(tableNameLocal, sizeof(tableNameLocal), "%s", tableName);

	if (strcmp(entryLocal->tableName, tableNameLocal) != 0) 
		/* fault injection is not set for the specified table name */
		return;
	
	if (entryLocal->faultInjectorState == FaultInjectorStateCompleted ||
		entryLocal->faultInjectorState == FautInjectorStateFailed) {
		/* fault injection was already executed */
		return;
	}
	
	/* Update the injection fault entry in hash table */
	
	if (entryLocal->occurrence > 1) {
		entryLocal->occurrence--;
	} else {
		entryLocal->faultInjectorState = FaultInjectorStateCompleted;
	}
	
	FaultInjector_UpdateHashEntry(entryLocal);	
	
	
	/* Inject fault */
	
	switch (entryLocal->faultInjectorType) {
		case FaultInjectorTypeNotSpecified:
			
			break;
		case FaultInjectorTypeSleep:
			ereport(LOG, 
					(errmsg("Fault is injected identifier:'%s'  ",
							FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier])));	
			
			pg_usleep(entryLocal->sleepTime * 1000000L);
			break;
		case FaultInjectorTypeFault:
			ereport(LOG, 
					(errmsg("Fault is injected identifier:'%s'  ",
							FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier])));	
			
			break;
		case FaultInjectorTypeFatal:
			ereport(FATAL, 
					(errmsg("Fault is injected identifier:'%s'  ",
							FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier])));	

			break;
		case FaultInjectorTypePanic:
			ereport(PANIC, 
					(errmsg("Fault is injected identifier:'%s'  ",
							FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier])));	

			break;
		case FaultInjectorTypeError:
			ereport(ERROR, 
					(errmsg("Fault is injected identifier:'%s'  ",
							FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier])));	
			break;
		case FaultInjectorTypeInfiniteLoop:
			ereport(LOG, 
					(errmsg("Fault is injected identifier:'%s'  ",
							FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier])));	
			for (ii=0; ii < 36; ii++)
				pg_usleep(100000000L); // sleep for 1 hour (100 sec * 36 = 1 hour)
			break;
		case FaultInjectorTypeDataCorruption:
			/* TO DO */
			break;
		case FaultInjectorTypeReset:
		case FaultInjectorTypeStatus:

			Assert(0);
			break;
		default:
			Assert(0);
			break;
	}
		
	return;
}

/*
 * lookup if fault injection is set
 */
static FaultInjectorEntry_s*
FaultInjector_LookupHashEntry(
							  FaultInjectorIdentifier_e identifier)
{
	FaultInjectorEntry_s	*entry;
	
	Assert(faultInjectorShmem->hash != NULL);
	
	entry = (FaultInjectorEntry_s *) hash_search(
												  faultInjectorShmem->hash, 
												  (void *) &identifier, // key 
												  HASH_FIND, 
												  NULL);
	
	if (entry == NULL) {
		ereport(DEBUG5,
				(errmsg("FaultInjector_LookupHashEntry() could not find fault injection hash entry identifier:'%d' ",
						identifier)));
	} 
	
	return entry;
}

/*
 * insert fault injection in hash table 
 */ 
static FaultInjectorEntry_s*
FaultInjector_InsertHashEntry(
							FaultInjectorIdentifier_e identifier, 
							bool	*exists)
{
	
	bool					foundPtr;
	FaultInjectorEntry_s	*entry;

	Assert(faultInjectorShmem->hash != NULL);
	
	entry = (FaultInjectorEntry_s *) hash_search(
												  faultInjectorShmem->hash, 
												  (void *) &identifier, // key
												  HASH_ENTER_NULL, 
												  &foundPtr);
	
	if (entry == NULL) {
		*exists = FALSE;
		return entry;
	} 
	
	elog(DEBUG1, "FaultInjector_InsertHashEntry() entry_key:%d", 
		 entry->faultInjectorIdentifier);
	
	if (foundPtr) {
		*exists = TRUE;
	} else {
		*exists = FALSE;
	}
	
	return entry;
}

/*
 * 
 */
static bool
FaultInjector_RemoveHashEntry(
							  FaultInjectorIdentifier_e identifier)
{	
	
	FaultInjectorEntry_s	*entry;
	bool					isRemoved = FALSE;
	
	Assert(faultInjectorShmem->hash != NULL);
	
	entry = (FaultInjectorEntry_s *) hash_search(
												  faultInjectorShmem->hash, 
												  (void *) &identifier, // key
												  HASH_REMOVE, 
												  NULL);
	
	if (entry) {
		elog(DEBUG1, "FaultInjector_RemoveHashEntry() Removed hash entry entry_key:%d", 
			 entry->faultInjectorIdentifier);
		isRemoved = TRUE;
	}
	
	return isRemoved;			
}

/*
 *
 */
static int 
FaultInjector_NewHashEntry(
						   FaultInjectorEntry_s	*entry)
{
	
	FaultInjectorEntry_s	*entryLocal=NULL;
	bool					exists;
	int						status = STATUS_OK;
	
	LockAcquire();
		
	if ((faultInjectorShmem->faultInjectorSlots + 1) >= FAULTINJECTOR_MAX_SLOTS) {
		LockRelease();
		status = STATUS_ERROR;
		ereport(LOG,
				(errmsg("ERROR(fault injector): could not insert fault injection entry into table, no slots"
						"identifier:'%s' ",
						FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier])));
		snprintf(entry->bufOutput, sizeof(entry->bufOutput), 
				 "could not insert fault injection, max slots:'%d' reached",
				 FAULTINJECTOR_MAX_SLOTS);
		
		goto exit;
	}

	entryLocal = FaultInjector_InsertHashEntry(entry->faultInjectorIdentifier, &exists);
		
	if (entryLocal == NULL) {
		LockRelease();
		status = STATUS_ERROR;
		ereport(LOG,
				(errmsg("ERROR(fault injector): could not insert fault injection entry into table, no memory"
						"identifier:'%s' ",
						FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier])));
		snprintf(entry->bufOutput, sizeof(entry->bufOutput), 
				 "could not insert fault injection, no memory");
		
		goto exit;
	}
		
	if (exists) {
		LockRelease();
		status = STATUS_ERROR;
		ereport(LOG,
				(errmsg("ERROR(fault injector): could not insert fault injection entry into table, entry already exists "
						"identifier:'%s' ",
						FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier])));
		snprintf(entry->bufOutput, sizeof(entry->bufOutput), 
				 "could not insert fault injection, entry already exists");
		
		goto exit;
	}
		
	entryLocal->faultInjectorType = entry->faultInjectorType;
	
	entryLocal->sleepTime = entry->sleepTime;
	entryLocal->ddlStatement = entry->ddlStatement;
	entryLocal->occurrence = entry->occurrence;
	strcpy(entryLocal->databaseName, entry->databaseName);
	strcpy(entryLocal->tableName, entry->tableName);
		
	entryLocal->faultInjectorState = FaultInjectorStateWaiting;

	faultInjectorShmem->faultInjectorSlots++;
		
	LockRelease();
	
	elog(DEBUG1, "FaultInjector_NewHashEntry() identifier:'%s'", 
		 FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier]);
	
exit:
		
	return status;			
}

/*
 * update hash entry with state 
 */		
static int 
FaultInjector_UpdateHashEntry(
							FaultInjectorEntry_s	*entry)
{
	
	FaultInjectorEntry_s	*entryLocal;
	bool					exists;
	int						status = STATUS_OK;
	
	LockAcquire();
	
	entryLocal = FaultInjector_InsertHashEntry(entry->faultInjectorIdentifier, &exists);
	
	/* entry should be found since fault has not been injected yet */			
	Assert(entryLocal != NULL);
	
	if (!exists) {
		LockRelease();
		status = STATUS_ERROR;
		ereport(LOG,
				(errmsg("ERROR(fault injector): could not update fault injection hash entry with fault injection status, no entry found "
						"identifier:'%s' ",
						FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier])));
		goto exit;
	}
	
	entryLocal->faultInjectorState = entry->faultInjectorState;
	entryLocal->occurrence = entry->occurrence;
	
	LockRelease();
	
	ereport(DEBUG1,
			(errmsg("LOG(fault injector): update fault injection hash entry "
					"identifier:'%s' state:'%s' occurrence:'%d' ",
					FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier], 
					FaultInjectorStateEnumToString[entry->faultInjectorState],
					entry->occurrence)));
	
exit:	
	
	return status;			
}

/*
 * 
 */
int
FaultInjector_SetFaultInjection(
						   FaultInjectorEntry_s	*entry)
{
	int		status = STATUS_OK;
	bool	isRemoved = FALSE;
	
	switch (entry->faultInjectorType) {
		case FaultInjectorTypeReset:
	
			LockAcquire();
			isRemoved = FaultInjector_RemoveHashEntry(entry->faultInjectorIdentifier);
			if (isRemoved == TRUE) {
				faultInjectorShmem->faultInjectorSlots--;
			}
			LockRelease();
			
			if (isRemoved == FALSE) {
				ereport(DEBUG1,
						(errmsg("LOG(fault injector): could not remove fault injection from hash"
								"identifier:'%s' ",
								FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier])));
			}			
			
			break;
		case FaultInjectorTypeStatus:
		{	
			HASH_SEQ_STATUS			hash_status;
			FaultInjectorEntry_s	*entryLocal;
			bool					found = FALSE;
			
			if (faultInjectorShmem->hash == NULL) {
				status = STATUS_ERROR;
				break;
			} 
			
			hash_seq_init(&hash_status, faultInjectorShmem->hash);
			
			while ((entryLocal = (FaultInjectorEntry_s *) hash_seq_search(&hash_status)) != NULL) {
				ereport(LOG,
					(errmsg("LOG(fault injector): "
							"fault name:'%s' "
							"type:'%s' "
							"ddl statement:'%s' "
							"database name:'%s' "
							"table name:'%s' "
							"occurrence:'%d' "
							"sleep time:'%d' "
							"fault injection state:'%s' ",
							FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier],
							FaultInjectorTypeEnumToString[entryLocal->faultInjectorType],
							FaultInjectorDDLEnumToString[entryLocal->ddlStatement],
							entryLocal->databaseName,
							entryLocal->tableName,
							entryLocal->occurrence,
							entryLocal->sleepTime,
							FaultInjectorStateEnumToString[entryLocal->faultInjectorState])));
				
				if (entry->faultInjectorIdentifier == entryLocal->faultInjectorIdentifier) {
						snprintf(entry->bufOutput, sizeof(entry->bufOutput), "Fault Injector(status): "
								 "fault name:'%s' "
								 "type:'%s' "
								 "ddl statement:'%s' "
								 "database name:'%s' "
								 "table name:'%s' "
								 "occurrence:'%d' "
								 "sleep time:'%d' "
								 "fault injection state:'%s' ",
								 FaultInjectorIdentifierEnumToString[entryLocal->faultInjectorIdentifier],
								 FaultInjectorTypeEnumToString[entryLocal->faultInjectorType],
								 FaultInjectorDDLEnumToString[entryLocal->ddlStatement],
								 entryLocal->databaseName,
								 entryLocal->tableName,
								 entryLocal->occurrence,
								 entryLocal->sleepTime,
								 FaultInjectorStateEnumToString[entryLocal->faultInjectorState]);		
						found = TRUE;
				}
			}
			if (found == FALSE) {
				snprintf(entry->bufOutput, sizeof(entry->bufOutput), "Fault Injector(status): "
						 "fault name:'%s' not set",
						 FaultInjectorIdentifierEnumToString[entry->faultInjectorIdentifier]);
			}
			break;
		}
		default: 
			
			status = FaultInjector_NewHashEntry(entry);
			break;
	}
	return status;
}

/*
 * 
 */
bool
FaultInjector_IsFaultInjected(
							  FaultInjectorIdentifier_e identifier)
{
	FaultInjectorEntry_s	*entry = NULL;
	bool					isCompleted = FALSE;
	bool					retval = FALSE;
	bool					isRemoved;
		
	LockAcquire();
		
	entry = FaultInjector_LookupHashEntry(identifier);
		
	if (entry == NULL) {
		retval = TRUE;
		isCompleted = TRUE;
		goto exit;
	}
		
	switch (entry->faultInjectorState) {
		case FaultInjectorStateWaiting:
			/* No operation */
			break;
		case FaultInjectorStateCompleted:
			
			retval = TRUE;
			/* NO break */
		case FautInjectorStateFailed:
			
			isCompleted = TRUE;
			isRemoved = FaultInjector_RemoveHashEntry(identifier);
			
			if (isRemoved == FALSE) {
				ereport(DEBUG1,
						(errmsg("LOG(fault injector): could not remove fault injection from hash"
								"identifier:'%s' ",
								FaultInjectorIdentifierEnumToString[identifier])));
			} else {
				faultInjectorShmem->faultInjectorSlots--;
			}

			break;
		default:
			Assert(0);
	}

exit:
	LockRelease();						
	
	if ((isCompleted == TRUE) && (retval == FALSE)) {
		ereport(LOG,
				(errmsg("ERROR(fault injector): could not complete fault injection identifier:'%s' ",
						FaultInjectorIdentifierEnumToString[identifier])));
	}
	return isCompleted;
}
#endif
